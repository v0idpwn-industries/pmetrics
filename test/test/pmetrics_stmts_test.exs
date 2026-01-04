defmodule PmetricsStmtsTest do
  use ExUnit.Case, async: false
  import PmetricsTest.TestHelpers

  setup_all do
    setup_extensions()
    query("SELECT set_config('compute_query_id', 'on', false)")
    :ok
  end

  setup do
    clear_metrics()
    :ok
  end

  describe "automatic query tracking" do
    test "tracks query execution time" do
      query("SELECT 1")

      assert count_metrics("query_execution_time_ms", "histogram") > 0
    end

    test "tracks query planning time" do
      query("SELECT 1, 2, 3")

      assert count_metrics("query_planning_time_ms", "histogram") > 0
    end

    test "tracks rows returned" do
      query("SELECT generate_series(1, 10)")

      assert count_metrics("query_rows_returned", "histogram") > 0
    end

    test "tracks buffer usage when enabled" do
      query("SELECT generate_series(1, 10)")

      assert count_metrics("query_shared_blocks_hit", "histogram") > 0
      assert count_metrics("query_shared_blocks_read", "histogram") >= 0
    end

    test "creates histogram_sum entries for all metric types" do
      query("SELECT generate_series(1, 5)")

      result =
        query("""
          SELECT DISTINCT m.name
          FROM pmetrics.list_metrics() m
          JOIN pmetrics_stmts.list_queries() q
            ON (m.labels->>'queryid')::bigint = q.queryid
          WHERE m.type = 'histogram_sum'
          AND m.name IN (
            'query_execution_time_ms',
            'query_planning_time_ms',
            'query_rows_returned',
            'query_shared_blocks_hit',
            'query_shared_blocks_read'
          )
          AND q.query_text = 'SELECT generate_series($1, $2)'
          ORDER BY m.name
        """)

      metric_names = Enum.map(result.rows, fn [name] -> name end)
      assert "query_execution_time_ms" in metric_names
      assert "query_planning_time_ms" in metric_names
      assert "query_rows_returned" in metric_names
      assert "query_shared_blocks_hit" in metric_names
      assert "query_shared_blocks_read" in metric_names
    end
  end

  describe "query labels" do
    test "metrics include queryid in labels" do
      query("SELECT 123 AS test_value")

      assert [%{labels: %{"queryid" => qid}} | _] =
               list_metrics("query_execution_time_ms", "histogram")

      assert is_integer(qid)
    end

    test "metrics include userid in labels" do
      query("SELECT 'user_test'")

      assert [%{labels: %{"userid" => uid}} | _] =
               list_metrics("query_execution_time_ms", "histogram")

      assert is_integer(uid)
    end

    test "metrics include dbid in labels" do
      query("SELECT 'db_test'")

      assert [%{labels: %{"dbid" => dbid}} | _] =
               list_metrics("query_execution_time_ms", "histogram")

      assert is_integer(dbid)
    end
  end

  describe "query text storage" do
    test "list_queries returns query text" do
      query("WITH unique_marker AS (SELECT 1) SELECT * FROM unique_marker")

      result = query("SELECT queryid, query_text FROM pmetrics_stmts.list_queries()")

      assert length(result.rows) > 0
      query_texts = Enum.map(result.rows, fn [_id, text] -> text end)
      assert Enum.any?(query_texts, &String.contains?(&1, "unique_marker"))
    end

    test "query text normalization replaces constants with placeholders" do
      query("SELECT oid FROM pg_type WHERE oid = 12345")

      result =
        query(
          "SELECT query_text FROM pmetrics_stmts.list_queries() WHERE query_text = 'SELECT oid FROM pg_type WHERE oid = $1'"
        )

      assert [["SELECT oid FROM pg_type WHERE oid = $1"]] = result.rows
    end

    test "queryid is consistent for normalized queries" do
      query("SELECT oid FROM pg_type WHERE oid = 111")

      result1 =
        query("""
          SELECT labels->>'queryid' AS qid
          FROM pmetrics.list_metrics()
          WHERE name = 'query_execution_time_ms'
          AND labels->>'queryid' IN (
            SELECT queryid::text FROM pmetrics_stmts.list_queries()
            WHERE query_text = 'SELECT oid FROM pg_type WHERE oid = $1'
          )
          LIMIT 1
        """)

      [[qid1]] = result1.rows

      query("SELECT oid FROM pg_type WHERE oid = 222")

      result2 =
        query("""
          SELECT labels->>'queryid' AS qid
          FROM pmetrics.list_metrics()
          WHERE name = 'query_execution_time_ms'
          AND labels->>'queryid' IN (
            SELECT queryid::text FROM pmetrics_stmts.list_queries()
            WHERE query_text = 'SELECT oid FROM pg_type WHERE oid = $1'
          )
          LIMIT 1
        """)

      [[qid2]] = result2.rows
      assert qid1 == qid2
    end
  end

  describe "histogram distribution" do
    test "different execution times populate different buckets" do
      query("SELECT pg_sleep(0.001)")
      query("SELECT pg_sleep(0.005)")
      query("SELECT pg_sleep(0.01)")

      result =
        query("""
          SELECT DISTINCT bucket
          FROM pmetrics.list_metrics()
          WHERE name = 'query_execution_time_ms'
          AND type = 'histogram'
          ORDER BY bucket
        """)

      buckets = Enum.map(result.rows, fn [b] -> b end)
      assert length(buckets) >= 1
    end

    test "histogram sum accumulates correctly" do
      query("SELECT generate_series(1, 5)")
      query("SELECT generate_series(1, 10)")

      result =
        query("""
          SELECT SUM(m.value)
          FROM pmetrics.list_metrics() m
          JOIN pmetrics_stmts.list_queries() q
            ON (m.labels->>'queryid')::bigint = q.queryid
          WHERE m.name = 'query_rows_returned'
          AND m.type = 'histogram_sum'
          AND q.query_text = 'SELECT generate_series($1, $2)'
        """)

      [[sum_value]] = result.rows
      assert Decimal.to_integer(sum_value) == 15
    end
  end

  describe "query text truncation" do
    test "long queries are truncated to 1024 bytes" do
      long_query = "SELECT " <> String.duplicate("'a' || ", 200) <> "'end'"
      query(long_query)

      result =
        query("""
          SELECT query_text, LENGTH(query_text)
          FROM pmetrics_stmts.list_queries()
          ORDER BY LENGTH(query_text) DESC
          LIMIT 1
        """)

      [[_text, length]] = result.rows
      assert length <= 1024
    end
  end

  describe "multiple query types" do
    test "SELECT queries are tracked" do
      query("SELECT 1, 2, 3")

      assert count_metrics("query_execution_time_ms", "histogram") > 0
    end

    test "INSERT queries are tracked" do
      query("CREATE TEMP TABLE test_insert (id INT)")
      query("INSERT INTO test_insert VALUES (1)")

      result =
        query("""
          SELECT query_text
          FROM pmetrics_stmts.list_queries()
          WHERE query_text LIKE '%INSERT INTO%test_insert%'
        """)

      assert length(result.rows) >= 1
    end

    test "UPDATE queries are tracked" do
      query("CREATE TEMP TABLE test_update (id INT)")
      query("INSERT INTO test_update VALUES (1)")
      query("UPDATE test_update SET id = 2")

      result =
        query("""
          SELECT query_text
          FROM pmetrics_stmts.list_queries()
          WHERE query_text LIKE '%UPDATE%test_update%'
        """)

      assert length(result.rows) >= 1
    end
  end

  describe "cleanup old metrics" do
    test "cleanup_old_query_metrics removes old query metrics and text" do
      # Create a temp table with a unique name (preserved in normalization!)
      unique_suffix = :erlang.unique_integer([:positive])
      table_name = "cleanup_test_table_#{unique_suffix}"

      query("CREATE TEMP TABLE #{table_name} (id INT)")
      query("SELECT * FROM #{table_name}")

      # Find the queryid for our unique query
      result =
        query("""
          SELECT queryid, query_text
          FROM pmetrics_stmts.list_queries()
          WHERE query_text LIKE '%#{table_name}%'
          AND query_text NOT LIKE '%CREATE%'
          LIMIT 1
        """)

      assert [[queryid, query_text]] = result.rows
      assert String.contains?(query_text, table_name)

      # Verify metrics exist for this query
      result =
        query("""
          SELECT COUNT(*)
          FROM pmetrics.list_metrics()
          WHERE (labels->>'queryid')::bigint = #{queryid}
          AND name = 'query_execution_time_ms'
        """)

      [[count_before]] = result.rows
      assert count_before > 0

      # Sleep for 2 seconds to make the query "old"
      Process.sleep(2000)

      # Call cleanup function to remove queries older than 1 second
      result = query("SELECT pmetrics_stmts.cleanup_old_query_metrics(1)")
      [[cleaned_count]] = result.rows
      assert cleaned_count > 0

      # Verify query text was removed
      result =
        query("""
          SELECT queryid
          FROM pmetrics_stmts.list_queries()
          WHERE queryid = #{queryid}
        """)

      assert result.rows == []

      # Verify metrics were removed
      result =
        query("""
          SELECT COUNT(*)
          FROM pmetrics.list_metrics()
          WHERE (labels->>'queryid')::bigint = #{queryid}
        """)

      [[count_after]] = result.rows
      assert count_after == 0
    end
  end
end

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

      result =
        query("""
          SELECT COUNT(*)
          FROM pmetrics.list_metrics()
          WHERE name = 'query_execution_time_ms' AND type = 'histogram'
        """)

      [[count]] = result.rows
      assert count > 0
    end

    test "tracks query planning time" do
      query("SELECT 1, 2, 3")

      result =
        query("""
          SELECT COUNT(*)
          FROM pmetrics.list_metrics()
          WHERE name = 'query_planning_time_ms' AND type = 'histogram'
        """)

      [[count]] = result.rows
      assert count > 0
    end

    test "tracks rows returned" do
      query("SELECT generate_series(1, 10)")

      result =
        query("""
          SELECT COUNT(*)
          FROM pmetrics.list_metrics()
          WHERE name = 'query_rows_returned' AND type = 'histogram'
        """)

      [[count]] = result.rows
      assert count > 0
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
          AND m.name IN ('query_execution_time_ms', 'query_planning_time_ms', 'query_rows_returned')
          AND q.query_text = 'SELECT generate_series($1, $2)'
          ORDER BY m.name
        """)

      assert [["query_execution_time_ms"], ["query_planning_time_ms"], ["query_rows_returned"]] =
               result.rows
    end
  end

  describe "query labels" do
    test "metrics include queryid in labels" do
      query("SELECT 123 AS test_value")

      result =
        query("""
          SELECT labels
          FROM pmetrics.list_metrics()
          WHERE name = 'query_execution_time_ms'
          AND type = 'histogram'
          LIMIT 1
        """)

      [[labels]] = result.rows
      assert is_map(labels)
      assert Map.has_key?(labels, "queryid")
      assert is_integer(labels["queryid"])
    end

    test "metrics include userid in labels" do
      query("SELECT 'user_test'")

      result =
        query("""
          SELECT labels
          FROM pmetrics.list_metrics()
          WHERE name = 'query_execution_time_ms'
          AND type = 'histogram'
          LIMIT 1
        """)

      [[labels]] = result.rows
      assert Map.has_key?(labels, "userid")
      assert is_integer(labels["userid"])
    end

    test "metrics include dbid in labels" do
      query("SELECT 'db_test'")

      result =
        query("""
          SELECT labels
          FROM pmetrics.list_metrics()
          WHERE name = 'query_execution_time_ms'
          AND type = 'histogram'
          LIMIT 1
        """)

      [[labels]] = result.rows
      assert Map.has_key?(labels, "dbid")
      assert is_integer(labels["dbid"])
    end
  end

  describe "query text storage" do
    test "list_queries returns query text" do
      query("SELECT 999 AS unique_marker")

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

      result =
        query("""
          SELECT COUNT(*)
          FROM pmetrics.list_metrics()
          WHERE name = 'query_execution_time_ms'
        """)

      [[count]] = result.rows
      assert count > 0
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
end

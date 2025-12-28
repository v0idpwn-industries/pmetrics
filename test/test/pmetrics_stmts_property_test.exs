defmodule PmetricsStmtsPropertyTest do
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

  test "concurrent workload with timing, counts, and row assertions" do
    buckets = get_all_buckets()

    workload = [
      %{
        query: "SELECT generate_series(1, 10)",
        normalized: "SELECT generate_series($1, $2)",
        executions: 5000,
        rows_per_execution: 10
      },
      %{
        query: "SELECT COUNT(*) FROM generate_series(1, 100)",
        normalized: "SELECT COUNT(*) FROM generate_series($1, $2)",
        executions: 3000,
        rows_per_execution: 1
      },
      %{
        query: "SELECT pg_sleep(0.2)",
        normalized: "SELECT pg_sleep($1)",
        executions: 100,
        rows_per_execution: 1,
        target_bucket_ms: 200
      },
      %{
        query: "SELECT * FROM generate_series(1, 50)",
        normalized: "SELECT * FROM generate_series($1, $2)",
        executions: 2000,
        rows_per_execution: 50
      }
    ]

    all_tasks =
      for spec <- workload, _i <- 1..spec.executions do
        Task.async(fn ->
          query(spec.query)
          spec
        end)
      end

    Task.await_many(all_tasks, 60_000)

    metrics =
      query("""
        SELECT
          m.name,
          m.labels,
          m.type,
          m.bucket,
          m.value,
          q.query_text
        FROM pmetrics.list_metrics() m
        LEFT JOIN pmetrics_stmts.list_queries() q
          ON (m.labels->>'queryid')::bigint = q.queryid
      """)

    metrics_map =
      metrics.rows
      |> Enum.group_by(fn [name, _labels, type, _bucket, _value, query_text] ->
        {name, query_text, type}
      end)

    for spec <- workload do
      assert get_histogram_sum(metrics_map, "query_rows_returned", spec.normalized) ==
               spec.executions * spec.rows_per_execution

      execution_count =
        get_histogram_count(metrics_map, "query_execution_time_ms", spec.normalized)

      assert execution_count == spec.executions

      planning_count = get_histogram_count(metrics_map, "query_planning_time_ms", spec.normalized)
      assert planning_count == spec.executions

      assert execution_count ==
               sum_bucket_values(metrics_map, "query_execution_time_ms", spec.normalized)

      assert planning_count ==
               sum_bucket_values(metrics_map, "query_planning_time_ms", spec.normalized)

      if Map.has_key?(spec, :target_bucket_ms) do
        target_bucket = find_bucket_for_value(buckets, spec.target_bucket_ms)

        assert get_bucket_value(
                 metrics_map,
                 "query_execution_time_ms",
                 spec.normalized,
                 target_bucket
               ) ==
                 spec.executions

        execution_sum = get_histogram_sum(metrics_map, "query_execution_time_ms", spec.normalized)
        assert execution_sum >= spec.executions * spec.target_bucket_ms
      end
    end

    assert Enum.sum(Enum.map(workload, & &1.executions)) ==
             Enum.sum(
               Enum.map(workload, & &1.normalized)
               |> Enum.map(fn query_text ->
                 get_histogram_count(metrics_map, "query_execution_time_ms", query_text)
               end)
             )

    assert 0 ==
             Enum.count(metrics.rows, fn [_name, _labels, _type, _bucket, value, _query_text] ->
               Decimal.compare(value, 0) == :lt
             end)

    assert [] ==
             Enum.filter(metrics.rows, fn [_name, labels, _type, _bucket, _value, _query_text] ->
               not (Map.has_key?(labels, "queryid") and
                      Map.has_key?(labels, "userid") and
                      Map.has_key?(labels, "dbid"))
             end)
  end

  defp get_all_buckets do
    result = query("SELECT bucket FROM pmetrics.list_histogram_buckets() ORDER BY bucket")
    Enum.map(result.rows, fn [bucket] -> bucket end)
  end

  defp get_histogram_sum(metrics_map, metric_name, query_text) do
    key = {metric_name, query_text, "histogram_sum"}

    case Map.get(metrics_map, key, []) do
      [] -> 0
      [[_name, _labels, _type, _bucket, value, _query_text]] -> to_int(value)
    end
  end

  defp get_histogram_count(metrics_map, metric_name, query_text) do
    sum_bucket_values(metrics_map, metric_name, query_text)
  end

  defp sum_bucket_values(metrics_map, metric_name, query_text) do
    key = {metric_name, query_text, "histogram"}

    case Map.get(metrics_map, key, []) do
      [] ->
        0

      rows ->
        rows
        |> Enum.map(fn [_name, _labels, _type, _bucket, value, _query_text] ->
          to_int(value)
        end)
        |> Enum.sum()
    end
  end

  defp get_bucket_value(metrics_map, metric_name, query_text, target_bucket) do
    key = {metric_name, query_text, "histogram"}

    metrics_map
    |> Map.get(key, [])
    |> Enum.find_value(0, fn [_name, _labels, _type, bucket, value, _query_text] ->
      if bucket == target_bucket, do: to_int(value)
    end)
  end

  defp to_int(value) when is_integer(value), do: value
  defp to_int(%Decimal{} = value), do: Decimal.to_integer(value)

  defp find_bucket_for_value(buckets, value_ms) do
    Enum.find(buckets, List.last(buckets), fn bucket -> value_ms <= bucket end)
  end
end

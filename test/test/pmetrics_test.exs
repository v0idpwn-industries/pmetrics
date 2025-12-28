defmodule PmetricsTest do
  use ExUnit.Case, async: false
  import PmetricsTest.TestHelpers

  setup_all do
    setup_extensions()
    :ok
  end

  setup do
    clear_metrics()
    :ok
  end

  describe "counters" do
    test "increment_counter increases value by 1" do
      result = query("SELECT pmetrics.increment_counter('test_counter', '{}'::jsonb)")
      assert [[1]] = result.rows

      result = query("SELECT pmetrics.increment_counter('test_counter', '{}'::jsonb)")
      assert [[2]] = result.rows
    end

    test "increment_counter_by increases by specified amount" do
      query("SELECT pmetrics.increment_counter_by('batch_counter', '{}'::jsonb, 10)")
      query("SELECT pmetrics.increment_counter_by('batch_counter', '{}'::jsonb, 5)")

      result =
        query(
          "SELECT value FROM pmetrics.list_metrics() WHERE name = 'batch_counter' AND type = 'counter'"
        )

      assert [[15]] = result.rows
    end

    test "counters with different labels are independent" do
      query("SELECT pmetrics.increment_counter('http_requests', '{\"status\": 200}'::jsonb)")
      query("SELECT pmetrics.increment_counter('http_requests', '{\"status\": 200}'::jsonb)")
      query("SELECT pmetrics.increment_counter('http_requests', '{\"status\": 404}'::jsonb)")

      result =
        query(
          "SELECT value FROM pmetrics.list_metrics() WHERE name = 'http_requests' AND labels = '{\"status\": 200}'::jsonb"
        )

      assert [[2]] = result.rows

      result =
        query(
          "SELECT value FROM pmetrics.list_metrics() WHERE name = 'http_requests' AND labels = '{\"status\": 404}'::jsonb"
        )

      assert [[1]] = result.rows
    end
  end

  describe "gauges" do
    test "set_gauge sets exact value" do
      query("SELECT pmetrics.set_gauge('temperature', '{}'::jsonb, 72)")

      result =
        query(
          "SELECT value FROM pmetrics.list_metrics() WHERE name = 'temperature' AND type = 'gauge'"
        )

      assert [[72]] = result.rows

      query("SELECT pmetrics.set_gauge('temperature', '{}'::jsonb, 68)")

      result =
        query(
          "SELECT value FROM pmetrics.list_metrics() WHERE name = 'temperature' AND type = 'gauge'"
        )

      assert [[68]] = result.rows
    end

    test "add_to_gauge modifies existing value" do
      query("SELECT pmetrics.set_gauge('balance', '{}'::jsonb, 100)")
      query("SELECT pmetrics.add_to_gauge('balance', '{}'::jsonb, 50)")
      query("SELECT pmetrics.add_to_gauge('balance', '{}'::jsonb, -25)")

      result =
        query(
          "SELECT value FROM pmetrics.list_metrics() WHERE name = 'balance' AND type = 'gauge'"
        )

      assert [[125]] = result.rows
    end

    test "gauges with labels are independent" do
      query("SELECT pmetrics.set_gauge('memory_usage', '{\"host\": \"server1\"}'::jsonb, 1024)")
      query("SELECT pmetrics.set_gauge('memory_usage', '{\"host\": \"server2\"}'::jsonb, 2048)")

      result =
        query(
          "SELECT value FROM pmetrics.list_metrics() WHERE name = 'memory_usage' AND labels = '{\"host\": \"server1\"}'::jsonb"
        )

      assert [[1024]] = result.rows

      result =
        query(
          "SELECT value FROM pmetrics.list_metrics() WHERE name = 'memory_usage' AND labels = '{\"host\": \"server2\"}'::jsonb"
        )

      assert [[2048]] = result.rows
    end
  end

  describe "histograms" do
    test "record_to_histogram creates bucket entries" do
      query("SELECT pmetrics.record_to_histogram('response_time', '{}'::jsonb, 150.5)")

      result =
        query(
          "SELECT bucket FROM pmetrics.list_metrics() WHERE name = 'response_time' AND type = 'histogram'"
        )

      assert length(result.rows) == 1
      [[bucket]] = result.rows
      assert is_integer(bucket)
    end

    test "histogram_sum tracks total values" do
      query("SELECT pmetrics.record_to_histogram('latency', '{}'::jsonb, 100.0)")
      query("SELECT pmetrics.record_to_histogram('latency', '{}'::jsonb, 200.0)")
      query("SELECT pmetrics.record_to_histogram('latency', '{}'::jsonb, 50.0)")

      result =
        query(
          "SELECT value FROM pmetrics.list_metrics() WHERE name = 'latency' AND type = 'histogram_sum'"
        )

      assert [[350]] = result.rows
    end

    test "multiple values in same bucket increment count" do
      query("SELECT pmetrics.record_to_histogram('request_time', '{}'::jsonb, 100.0)")
      query("SELECT pmetrics.record_to_histogram('request_time', '{}'::jsonb, 100.0)")
      query("SELECT pmetrics.record_to_histogram('request_time', '{}'::jsonb, 100.0)")

      result =
        query("""
          SELECT bucket, value
          FROM pmetrics.list_metrics()
          WHERE name = 'request_time' AND type = 'histogram'
          ORDER BY bucket
        """)

      assert length(result.rows) >= 1
      [row | _] = result.rows
      [_bucket, count] = row
      assert count == 3
    end

    test "histograms with labels are independent" do
      query(
        "SELECT pmetrics.record_to_histogram('api_latency', '{\"endpoint\": \"/users\"}'::jsonb, 50.0)"
      )

      query(
        "SELECT pmetrics.record_to_histogram('api_latency', '{\"endpoint\": \"/posts\"}'::jsonb, 150.0)"
      )

      result =
        query(
          "SELECT value FROM pmetrics.list_metrics() WHERE name = 'api_latency' AND type = 'histogram_sum' AND labels = '{\"endpoint\": \"/users\"}'::jsonb"
        )

      assert [[50]] = result.rows

      result =
        query(
          "SELECT value FROM pmetrics.list_metrics() WHERE name = 'api_latency' AND type = 'histogram_sum' AND labels = '{\"endpoint\": \"/posts\"}'::jsonb"
        )

      assert [[150]] = result.rows
    end
  end

  describe "type safety" do
    test "counter and gauge with same name are distinct metrics" do
      query("SELECT pmetrics.increment_counter('metric_name', '{}'::jsonb)")
      query("SELECT pmetrics.set_gauge('metric_name', '{}'::jsonb, 100)")

      result =
        query(
          "SELECT type, value FROM pmetrics.list_metrics() WHERE name = 'metric_name' ORDER BY type"
        )

      assert [["counter", 1], ["gauge", 100]] = result.rows
    end

    test "histogram and counter with same name are distinct" do
      query("SELECT pmetrics.increment_counter('requests', '{}'::jsonb)")
      query("SELECT pmetrics.record_to_histogram('requests', '{}'::jsonb, 42.0)")

      result =
        query(
          "SELECT DISTINCT type FROM pmetrics.list_metrics() WHERE name = 'requests' ORDER BY type"
        )

      types = Enum.map(result.rows, fn [type] -> type end)
      assert "counter" in types
      assert "histogram" in types
    end
  end

  describe "list_metrics" do
    test "returns all metric fields" do
      query("SELECT pmetrics.increment_counter('full_metric', '{\"label\": \"value\"}'::jsonb)")

      result =
        query("""
          SELECT name, labels, type, bucket, value
          FROM pmetrics.list_metrics()
          WHERE name = 'full_metric'
        """)

      assert [[name, labels, type, bucket, value]] = result.rows
      assert name == "full_metric"
      assert labels == %{"label" => "value"}
      assert type == "counter"
      assert bucket == 0
      assert value == 1
    end

    test "filters by specific criteria" do
      query("SELECT pmetrics.increment_counter('filter_test', '{\"env\": \"prod\"}'::jsonb)")
      query("SELECT pmetrics.increment_counter('filter_test', '{\"env\": \"dev\"}'::jsonb)")
      query("SELECT pmetrics.set_gauge('other_metric', '{}'::jsonb, 50)")

      result =
        query("""
          SELECT COUNT(*)
          FROM pmetrics.list_metrics()
          WHERE name = 'filter_test'
        """)

      assert [[2]] = result.rows
    end
  end

  describe "configuration" do
    test "metrics are recorded when enabled" do
      query("SELECT pmetrics.increment_counter('enabled_test', '{}'::jsonb)")

      result = query("SELECT value FROM pmetrics.list_metrics() WHERE name = 'enabled_test'")
      assert [[1]] = result.rows
    end
  end

  describe "list_histogram_buckets" do
    test "returns available bucket values" do
      result =
        query("SELECT bucket FROM pmetrics.list_histogram_buckets() ORDER BY bucket LIMIT 10")

      assert length(result.rows) == 10
      buckets = Enum.map(result.rows, fn [b] -> b end)
      assert Enum.all?(buckets, &is_integer/1)
      assert buckets == Enum.sort(buckets)
    end
  end
end

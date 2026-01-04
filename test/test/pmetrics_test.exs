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

      assert 15 = get_metric_value("batch_counter", "counter")
    end

    test "counters with different labels are independent" do
      query("SELECT pmetrics.increment_counter('http_requests', '{\"status\": 200}'::jsonb)")
      query("SELECT pmetrics.increment_counter('http_requests', '{\"status\": 200}'::jsonb)")
      query("SELECT pmetrics.increment_counter('http_requests', '{\"status\": 404}'::jsonb)")

      assert 2 = get_metric_value("http_requests", "counter", %{"status" => 200})
      assert 1 = get_metric_value("http_requests", "counter", %{"status" => 404})
    end
  end

  describe "gauges" do
    test "set_gauge sets exact value" do
      query("SELECT pmetrics.set_gauge('temperature', '{}'::jsonb, 72)")
      assert 72 = get_metric_value("temperature", "gauge")

      query("SELECT pmetrics.set_gauge('temperature', '{}'::jsonb, 68)")
      assert 68 = get_metric_value("temperature", "gauge")
    end

    test "add_to_gauge modifies existing value" do
      query("SELECT pmetrics.set_gauge('balance', '{}'::jsonb, 100)")
      query("SELECT pmetrics.add_to_gauge('balance', '{}'::jsonb, 50)")
      query("SELECT pmetrics.add_to_gauge('balance', '{}'::jsonb, -25)")

      assert 125 = get_metric_value("balance", "gauge")
    end

    test "gauges with labels are independent" do
      query("SELECT pmetrics.set_gauge('memory_usage', '{\"host\": \"server1\"}'::jsonb, 1024)")
      query("SELECT pmetrics.set_gauge('memory_usage', '{\"host\": \"server2\"}'::jsonb, 2048)")

      assert 1024 = get_metric_value("memory_usage", "gauge", %{"host" => "server1"})
      assert 2048 = get_metric_value("memory_usage", "gauge", %{"host" => "server2"})
    end
  end

  describe "histograms" do
    test "record_to_histogram creates bucket entries" do
      query("SELECT pmetrics.record_to_histogram('response_time', '{}'::jsonb, 150.5)")
      [%{bucket: 150, value: 1}] = list_metrics("response_time", "histogram")
    end

    test "histogram_sum tracks total values" do
      query("SELECT pmetrics.record_to_histogram('latency', '{}'::jsonb, 100.0)")
      query("SELECT pmetrics.record_to_histogram('latency', '{}'::jsonb, 200.0)")
      query("SELECT pmetrics.record_to_histogram('latency', '{}'::jsonb, 50.0)")

      assert 350 = get_histogram_sum("latency")
    end

    test "multiple values in same bucket increment count" do
      query("SELECT pmetrics.record_to_histogram('request_time', '{}'::jsonb, 100.0)")
      query("SELECT pmetrics.record_to_histogram('request_time', '{}'::jsonb, 100.0)")
      query("SELECT pmetrics.record_to_histogram('request_time', '{}'::jsonb, 100.0)")

      [%{bucket: 101, value: 3}] = list_metrics("request_time", "histogram")
    end

    test "histograms with labels are independent" do
      query(
        "SELECT pmetrics.record_to_histogram('api_latency', '{\"endpoint\": \"/users\"}'::jsonb, 50.0)"
      )

      query(
        "SELECT pmetrics.record_to_histogram('api_latency', '{\"endpoint\": \"/posts\"}'::jsonb, 150.0)"
      )

      assert 50 = get_histogram_sum("api_latency", %{"endpoint" => "/users"})
      assert 150 = get_histogram_sum("api_latency", %{"endpoint" => "/posts"})
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

      [
        %{
          name: "full_metric",
          labels: %{"label" => "value"},
          type: "counter",
          bucket: 0,
          value: 1
        }
      ] =
        list_metrics("full_metric", "counter")
    end

    test "filters by specific criteria" do
      query("SELECT pmetrics.increment_counter('filter_test', '{\"env\": \"prod\"}'::jsonb)")
      query("SELECT pmetrics.increment_counter('filter_test', '{\"env\": \"dev\"}'::jsonb)")
      query("SELECT pmetrics.set_gauge('other_metric', '{}'::jsonb, 50)")

      assert 2 = count_metrics("filter_test", "counter")
    end
  end

  describe "configuration" do
    test "metrics are recorded when enabled" do
      query("SELECT pmetrics.increment_counter('enabled_test', '{}'::jsonb)")

      assert 1 = get_metric_value("enabled_test", "counter")
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

  describe "delete_metric" do
    test "deletes counter" do
      query("SELECT pmetrics.increment_counter('test_counter', '{}'::jsonb)")
      assert 1 = get_metric_value("test_counter", "counter")

      result = query("SELECT pmetrics.delete_metric('test_counter', '{}'::jsonb)")
      assert [[1]] = result.rows

      assert is_nil(get_metric_value("test_counter", "counter"))
    end

    test "deletes all histogram metrics (buckets and sum)" do
      query("SELECT pmetrics.record_to_histogram('response_time', '{}'::jsonb, 100.0)")
      query("SELECT pmetrics.record_to_histogram('response_time', '{}'::jsonb, 200.0)")

      # Should have bucket entries and sum
      metrics = query("SELECT * FROM pmetrics.list_metrics() WHERE name = 'response_time'")
      initial_count = length(metrics.rows)
      assert initial_count > 1

      result = query("SELECT pmetrics.delete_metric('response_time', '{}'::jsonb)")
      [[deleted_count]] = result.rows
      assert deleted_count == initial_count

      # All should be gone
      metrics = query("SELECT * FROM pmetrics.list_metrics() WHERE name = 'response_time'")
      assert length(metrics.rows) == 0
    end
  end
end

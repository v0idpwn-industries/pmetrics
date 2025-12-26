-- pgbench script for testing pmetrics - single random operation per transaction
-- Usage: pgbench -c 50 -j 8 -T 60 -f bench_metrics.sql postgres

-- Generate random data
\set site_id random(1, 100)
\set status random(200, 503)
\set value random(1, 1000)
\set latency random(1, 5000)
\set op_type random(1, 100)

-- Use CASE to randomly choose one operation (1% chance of list_metrics)
SELECT CASE
  WHEN :op_type <= 20 THEN pmetrics.increment_counter('http_requests_total', 'site=' || :site_id || ',status=' || :status)
  WHEN :op_type <= 40 THEN pmetrics.increment_counter_by('errors_total', 'site=' || :site_id, :value)
  WHEN :op_type <= 60 THEN pmetrics.set_gauge('active_connections', 'site=' || :site_id, :value)
  WHEN :op_type <= 80 THEN pmetrics.add_to_gauge('queue_size', 'site=' || :site_id, CASE WHEN :value = 500 THEN 1 ELSE :value - 500 END)
  WHEN :op_type <= 99 THEN pmetrics.record_to_histogram('response_time_ms', 'endpoint=/api/users,method=GET', :latency)
  ELSE (SELECT COUNT(*) FROM pmetrics.list_metrics())::bigint  -- 1% chance: read all metrics
END;

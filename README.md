# pmetrics

An instrumentation toolkit for PostgreSQL extensions. Supports counters, gauges
and histograms. Metrics are held in postgres shared memory, and can be queried
for integration with other tools (e.g.: Prometheus).

## Configuration
Accepts the following custom options:

- `pmetrics.max_metrics`: Maximum number of metrics to store. Defaults to 1024.
Notice that while counters and gauges take a single slot each, histograms take
one per bucket. If your database uses a lot of histograms, you may need to increase
this setting.
- `pmetrics.enabled`: Enable metrics collection. Defaults to true. Can be used to
totally disable pmetrics.
- `pmetrics.bucket_variability`: Used to calculate the exponential buckets.
Defaults to 0.1. See the source for more information on the formula.
- `pmetrics.buckets_upper_bound`: the limit for the maximum histogram bucket.
Defaults to 30000. Values over this will be truncated and fitted into the last
bucket. A notice is raised whenever this happens.

## API

Metrics are created automatically when they are first used. There is no safeguards
over trying to use a metric as another metric type, so this must be kept in mind.

### Counters

There are two functions for incrementing counters: `increment_counter` and
`increment_counter_by`.

```
postgres=# select * from pmetrics.increment_counter('requests', 'site=1');
 increment_counter
-------------------
                 1
(1 row)

postgres=# select * from pmetrics.increment_counter('requests', 'site=1');
 increment_counter
-------------------
                 2
(1 row)

postgres=# select * from pmetrics.increment_counter_by('requests', 'site=2', 5);
 increment_counter_by
----------------------
                    5
(1 row)

postgres=# select * from pmetrics.list_metrics();
   name   | labels | value
----------+--------+-------
 requests | site=2 |     5
 requests | site=1 |     2
(2 rows)
```

### Gauges
There are two functions for working with gauges: `set_gauge` and `add_to_gauge`.

```
postgres=# select * from pmetrics.set_gauge('memory', '', 50_000);
 set_gauge
-----------
     50000
(1 row)

postgres=# select * from pmetrics.set_gauge('cpu_utilization', '', 30);
 set_gauge
-----------
        30
(1 row)

postgres=# select * from pmetrics.add_to_gauge('memory', '', -5000);
 add_to_gauge
--------------
        45000
(1 row)

postgres=# select * from pmetrics.add_to_gauge('cpu_utilization', '', 2);
 add_to_gauge
--------------
           32
(1 row)

postgres=# select * from pmetrics.list_metrics();
      name       | labels | value
-----------------+--------+-------
 cpu_utilization |        |    32
 memory          |        | 45000
(3 rows)
```

### Histograms
For working with histograms, there's a single function: record to histogram. PMetrics
takes care of bucketing according to configuration:

```
postgres=# select * from pmetrics.record_to_histogram('response_time', 'status=200', 100);
 record_to_histogram
---------------------
                   1
(1 row)

postgres=# select * from pmetrics.record_to_histogram('response_time', 'status=200', 150);
 record_to_histogram
---------------------
                   1
(1 row)

postgres=# select * from pmetrics.record_to_histogram('response_time', 'status=200', 30);
 record_to_histogram
---------------------
                   1
(1 row)

postgres=# select * from pmetrics.record_to_histogram('response_time', 'status=200', 98);
 record_to_histogram
---------------------
                   2
(1 row)

postgres=# select * from pmetrics.record_to_histogram('response_time', 'status=200', 31);
 record_to_histogram
---------------------
                   1
(1 row)

postgres=# select * from pmetrics.list_metrics();
       name        |   labels   | value
-------------------+------------+-------
 response_time_101 | status=200 |     2
 response_time_150 | status=200 |     1
 response_time_30  | status=200 |     1
 response_time_37  | status=200 |     1
(4 rows)
```

On the above example, notice that both 98ms and 100ms went to the same bucket (of 101)
while 30 and 31 went to different buckets (30 and 37).

## Constraints/notices

- Names are limited to NAMEDATALEN. Histogram names are limited to even less, according
to maximum bucket size. See the source for more information.
- Tags are limited to 128 characters.
- While this library has performance in mind, it wasn't extensively benchmarked yet,
and may suffer from performance issues.
- This library wasn't extensively tested yet, and may suffer of leaks or other problems.

# pmetrics

`pmetrics` is a PostgreSQL extension providing metrics instrumentation for database applications. It implements counters, gauges, and histograms with structured JSONB labels, storing all metrics in PostgreSQL dynamic shared memory.

## Overview

This extension enables collection and querying of application metrics directly within PostgreSQL. Metrics are stored in dynamic shared memory using PostgreSQL's DSA (Dynamic Shared Area) and dshash (dynamic shared hash) APIs, allowing the hash table to grow automatically without fixed limits.

Metrics are accessible via SQL functions, making them suitable for integration with monitoring systems such as Prometheus. The extension also provides a C API for use by other PostgreSQL extensions.

## Features

- **Three metric types**: counters (monotonically increasing), gauges (arbitrary values), and histograms (distribution tracking)
- **JSONB labels**: Structured key-value labels for multi-dimensional metrics
- **Dynamic memory allocation**: No predefined limit on metric count; hash table grows as needed
- **Type safety**: Metrics uniquely identified by (name, labels, type, bucket)
- **Concurrent access**: Uses dshash partition locking (128 partitions) for efficient parallel operations
- **C API**: Public interface for extensions to record metrics programmatically

## Installation

### Building

```bash
PG_CONFIG=/path/to/pg_config make clean
PG_CONFIG=/path/to/pg_config make
PG_CONFIG=/path/to/pg_config make install
```

### Configuration

Add to `postgresql.conf`:

```ini
shared_preload_libraries = 'pmetrics'
```

Restart PostgreSQL, then create the extension:

```sql
CREATE EXTENSION pmetrics;
```

## Configuration Parameters

All parameters are prefixed with `pmetrics.` and can be set in `postgresql.conf` or via `ALTER SYSTEM`.

### pmetrics.enabled

- **Type**: Boolean
- **Default**: `true`
- **Context**: PGC_SIGHUP (reload without restart)
- **Description**: Enables or disables metrics collection. When disabled, all metric functions return NULL.

### pmetrics.bucket_variability

- **Type**: Real
- **Default**: `0.1`
- **Context**: PGC_POSTMASTER (requires restart)
- **Description**: Controls histogram bucket spacing. Gamma is calculated as `γ = (1 + variability) / (1 - variability)`. Lower values create finer-grained buckets.

### pmetrics.buckets_upper_bound

- **Type**: Integer
- **Default**: `30000`
- **Context**: PGC_POSTMASTER (requires restart)
- **Description**: Maximum histogram bucket value. Values exceeding this are clamped to the last bucket with a notice.

## SQL API

### Data Types

```sql
-- Composite type returned by list_metrics()
CREATE TYPE metric_type AS (
    name TEXT,
    labels JSONB,
    type TEXT,
    bucket INTEGER,
    value BIGINT
);

-- Composite type returned by list_histogram_buckets()
CREATE TYPE histogram_buckets_type AS (
    bucket INTEGER
);
```

### Counter Functions

Counters represent monotonically increasing values.

#### increment_counter(name, labels)

```sql
SELECT increment_counter('http_requests_total', '{"method": "GET", "status": "200"}');
```

Increments the counter by 1. Returns the new value.

#### increment_counter_by(name, labels, increment)

```sql
SELECT increment_counter_by('bytes_sent', '{"endpoint": "/api"}', 1024);
```

Increments the counter by the specified amount. Returns the new value.

### Gauge Functions

Gauges represent arbitrary values that can increase or decrease.

#### set_gauge(name, labels, value)

```sql
SELECT set_gauge('active_connections', '{"database": "mydb"}', 42);
```

Sets the gauge to an absolute value. Returns the new value.

#### add_to_gauge(name, labels, value)

```sql
SELECT add_to_gauge('queue_depth', '{"queue": "jobs"}', -1);
```

Adds (or subtracts if negative) to the current gauge value. Returns the new value.

### Histogram Functions

Histograms track value distributions using exponential bucketing inspired by DDSketch.

#### record_to_histogram(name, labels, value)

```sql
SELECT record_to_histogram('query_duration_ms', '{"query_type": "select"}', 45.3);
```

Records a value to the histogram. Automatically creates two metric entries:
- A `histogram` bucket entry with count incremented
- A `histogram_sum` entry tracking cumulative sum

Returns the bucket count.

**Bucketing**: Buckets are calculated using `bucket = ceil(log(value) / log(γ))` where γ is derived from `pmetrics.bucket_variability`.

### Query Functions

#### list_metrics()

```sql
SELECT * FROM list_metrics() ORDER BY name, type, labels::text, bucket;
```

Returns all metrics as rows. Each row contains:
- `name`: Metric name (TEXT)
- `labels`: JSONB object with labels
- `type`: One of `counter`, `gauge`, `histogram`, `histogram_sum`
- `bucket`: Bucket number for histograms (0 for other types)
- `value`: Current metric value (BIGINT)

#### list_histogram_buckets()

```sql
SELECT bucket FROM list_histogram_buckets() ORDER BY bucket;
```

Returns all possible histogram bucket thresholds based on current configuration. Useful for histogram visualization.

## C API

The extension provides a public C API defined in `pmetrics.h`. Other extensions should include this header and link against pmetrics.

### Public Functions

```c
/* Get shared state structure */
PMetricsSharedState *pmetrics_get_shared_state(void);

/* Get DSA area (for extensions sharing the same DSA) */
dsa_area *pmetrics_get_dsa(void);

/* Record a metric value */
Datum pmetrics_increment_by(const char *name_str, Jsonb *labels_jsonb,
                            MetricType type, int bucket, int64 amount);

/* Calculate histogram bucket for a value */
int pmetrics_bucket_for(double value);

/* Check if metrics collection is enabled */
bool pmetrics_is_enabled(void);
```

### Metric Types

```c
typedef enum MetricType {
    METRIC_TYPE_COUNTER = 0,
    METRIC_TYPE_GAUGE = 1,
    METRIC_TYPE_HISTOGRAM = 2,
    METRIC_TYPE_HISTOGRAM_SUM = 3
} MetricType;
```

### Example Usage

```c
#include "extension/pmetrics/pmetrics.h"

void record_custom_metric(void)
{
    Jsonb *labels;
    int bucket;

    /* Build labels JSONB */
    labels = build_my_labels();

    /* Record counter */
    pmetrics_increment_by("custom_events", labels,
                          METRIC_TYPE_COUNTER, 0, 1);

    /* Record histogram */
    bucket = pmetrics_bucket_for(123.45);
    pmetrics_increment_by("custom_latency", labels,
                          METRIC_TYPE_HISTOGRAM, bucket, 1);
    pmetrics_increment_by("custom_latency", labels,
                          METRIC_TYPE_HISTOGRAM_SUM, 0, 123.45);
}
```

## Limitations

- Metric names limited to `NAMEDATALEN` (typically 64 bytes)
- Labels stored as JSONB; practical limit based on available DSA memory
- Histogram values exceeding `pmetrics.buckets_upper_bound` clamped to last bucket
- No built-in metric expiration or cleanup; metrics persist until server restart

## See Also

- `pmetrics_stmts`: Extension for automatic query performance tracking
- PostgreSQL Dynamic Shared Memory documentation
- DDSketch algorithm: https://arxiv.org/abs/1908.10693

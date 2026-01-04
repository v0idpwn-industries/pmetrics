# pmetrics

pmetrics is a PostgreSQL extension providing metrics collection infrastructure. It implements counters, gauges, and histograms with JSONB labels, stored in dynamic shared memory.

## Overview

This extension provides a metrics framework for PostgreSQL extensions and stored procedures. Metrics are stored in dynamic shared memory using PostgreSQL's DSA and dshash APIs, with no predefined limits.

Metrics can be recorded from PL/pgSQL functions via SQL or from C extensions via the public C API. All metrics are queryable via SQL.

**Key features:**

- Three metric types: counters, gauges, histograms
- JSONB labels for multi-dimensional metrics
- Exponential histogram bucketing (DDSketch-inspired)
- Partition-based locking (128 partitions) for concurrent access

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

/* Increment a counter by 1 */
int64 pmetrics_increment_counter(const char *name_str, Jsonb *labels_jsonb);

/* Increment a counter by a specific amount */
int64 pmetrics_increment_counter_by(const char *name_str, Jsonb *labels_jsonb,
                                     int64 amount);

/* Set a gauge to a specific value */
int64 pmetrics_set_gauge(const char *name_str, Jsonb *labels_jsonb, int64 value);

/* Add to a gauge (can be positive or negative) */
int64 pmetrics_add_to_gauge(const char *name_str, Jsonb *labels_jsonb, int64 amount);

/* Record a histogram value (automatically creates bucket and sum entries) */
int64 pmetrics_record_to_histogram(const char *name_str, Jsonb *labels_jsonb,
                                 double value);

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

void record_custom_metrics(void)
{
    Jsonb *labels;

    /* Build labels JSONB */
    labels = build_my_labels();

    /* Increment a counter */
    pmetrics_increment_counter("requests_total", labels);

    /* Increment counter by specific amount */
    pmetrics_increment_counter_by("bytes_sent", labels, 1024);

    /* Set a gauge */
    pmetrics_set_gauge("active_connections", labels, 42);

    /* Add to a gauge */
    pmetrics_add_to_gauge("queue_depth", labels, -1);

    /* Record histogram - automatically creates both bucket and sum entries */
    pmetrics_record_to_histogram("custom_latency", labels, 123.45);
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

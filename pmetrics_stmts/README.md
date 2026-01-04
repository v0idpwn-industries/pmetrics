# pmetrics_stmts

`pmetrics_stmts` is a PostgreSQL extension that automatically tracks query performance metrics using histogram distributions. It serves as an alternative to `pg_stat_statements` with enhanced distribution tracking capabilities.

## Overview

This extension hooks into PostgreSQL's planner and executor to optionally measure planning time, execution time, rows returned, and buffer usage for all queries. Unlike `pg_stat_statements` which provides aggregate statistics, `pmetrics_stmts` records metrics as histograms, preserving the full distribution of observed values.

All metrics are stored using the `pmetrics` extension's infrastructure and are queryable via SQL. Tracking can be controlled separately for time metrics (enabled by default), row counts (enabled by default), and buffer usage (disabled by default).

## Dependencies

**Requires**: `pmetrics` extension must be loaded first.

Both extensions must be listed in `shared_preload_libraries` with `pmetrics` before `pmetrics_stmts`.

## Installation

### Building

The `pmetrics` extension must be built and installed first:

```bash
# From repository root
PG_CONFIG=/path/to/pg_config make clean
PG_CONFIG=/path/to/pg_config make pmetrics.all
PG_CONFIG=/path/to/pg_config make pmetrics.install
PG_CONFIG=/path/to/pg_config make pmetrics_stmts.all
PG_CONFIG=/path/to/pg_config make pmetrics_stmts.install
```

This builds and installs both `pmetrics` and `pmetrics_stmts` in the correct order.

### Configuration

Add to `postgresql.conf`:

```ini
shared_preload_libraries = 'pmetrics,pmetrics_stmts'
compute_query_id = on
```

**Note**: `compute_query_id` must be set to `on` for query tracking to work. The default `auto` setting won't generate query IDs (it only enables them if `pg_stat_statements` is loaded).

Restart PostgreSQL, then create both extensions:

```sql
CREATE EXTENSION pmetrics;
CREATE EXTENSION pmetrics_stmts;
```

## Configuration Parameters

### pmetrics_stmts.track_times

- **Type**: Boolean
- **Default**: `true`
- **Context**: PGC_SIGHUP (reload without restart)
- **Description**: Enables or disables query planning and execution time tracking. When disabled, planning and execution time metrics are not recorded.

### pmetrics_stmts.track_rows

- **Type**: Boolean
- **Default**: `true`
- **Context**: PGC_SIGHUP (reload without restart)
- **Description**: Enables or disables query row count tracking. When disabled, row count metrics are not recorded.

### pmetrics_stmts.track_buffers

- **Type**: Boolean
- **Default**: `false`
- **Context**: PGC_SIGHUP (reload without restart)
- **Description**: Enables or disables buffer usage tracking (shared blocks hit/read). When disabled, buffer metrics are not recorded. Disabled by default due to additional overhead.

### pmetrics_stmts.cleanup_interval_seconds

- **Type**: Integer
- **Default**: `86400` (24 hours)
- **Context**: PGC_SIGHUP (reload without restart)
- **Description**: Interval in seconds between automatic cleanup runs. The background worker wakes up at this interval to remove metrics for queries that haven't been executed recently. Set to `0` to disable automatic cleanup entirely (manual cleanup via SQL still works).

### pmetrics_stmts.cleanup_max_age_seconds

- **Type**: Integer
- **Default**: `86400` (24 hours)
- **Context**: PGC_SIGHUP (reload without restart)
- **Description**: Maximum age in seconds for inactive query metrics. During cleanup, metrics for queries that haven't been executed in this many seconds are removed from shared memory to prevent unbounded growth.

## Tracked Metrics

The extension automatically creates histogram metrics for each unique query based on enabled tracking options:

### query_planning_time_ms

Planning time in milliseconds, measured by the planner hook.

**Type**: Histogram

**Controlled by**: `pmetrics_stmts.track_times`

**Labels**:

- `queryid`: PostgreSQL query identifier (uint64)
- `userid`: User OID executing the query
- `dbid`: Database OID

### query_execution_time_ms

Execution time in milliseconds, measured from executor start to executor end.

**Type**: Histogram

**Controlled by**: `pmetrics_stmts.track_times`

**Labels**: Same as `query_planning_time_ms`

### query_rows_returned

Number of rows returned by the query.

**Type**: Histogram

**Controlled by**: `pmetrics_stmts.track_rows`

**Labels**: Same as `query_planning_time_ms`

### query_shared_blocks_hit

Number of shared buffer cache hits (data found in memory).

**Type**: Histogram

**Controlled by**: `pmetrics_stmts.track_buffers`

**Labels**: Same as `query_planning_time_ms`

### query_shared_blocks_read

Number of shared buffer reads from disk.

**Type**: Histogram

**Controlled by**: `pmetrics_stmts.track_buffers`

**Labels**: Same as `query_planning_time_ms`

## SQL API

### list_queries()

```sql
SELECT * FROM pmetrics_stmts.list_queries();
```

Returns a mapping of query IDs to query text:

```sql
CREATE TYPE query_text_type AS (
    queryid BIGINT,
    query_text TEXT
);
```

**Query text storage**: Query text is truncated to 1024 bytes and stored in a separate dshash table. The extension uses first-write-wins semantics: once a query ID has stored text, subsequent queries with the same ID do not overwrite it.

## Example Queries

### View all query performance metrics

```sql
SELECT
    m.name,
    m.labels->>'queryid' AS queryid,
    m.labels->>'userid' AS userid,
    m.labels->>'dbid' AS dbid,
    m.bucket,
    m.value
FROM pmetrics.list_metrics() m
WHERE m.name IN ('query_planning_time_ms', 'query_execution_time_ms', 'query_rows_returned')
ORDER BY m.name, queryid::bigint, m.bucket;
```

### Join metrics with query text

```sql
SELECT
    q.query_text,
    m.name AS metric_name,
    m.bucket,
    m.value
FROM pmetrics.list_metrics() m
JOIN pmetrics_stmts.list_queries() q
    ON (m.labels->>'queryid')::bigint = q.queryid
WHERE m.name = 'query_execution_time_ms'
ORDER BY q.queryid, m.bucket;
```

### Summarize execution time per query

```sql
WITH histogram_sums AS (
    SELECT
        (m.labels->>'queryid')::bigint AS queryid,
        SUM(m.value) AS total_value
    FROM pmetrics.list_metrics() m
    WHERE m.name = 'query_execution_time_ms'
      AND m.type = 'histogram_sum'
    GROUP BY queryid
),
histogram_counts AS (
    SELECT
        (m.labels->>'queryid')::bigint AS queryid,
        SUM(m.value) AS total_count
    FROM pmetrics.list_metrics() m
    WHERE m.name = 'query_execution_time_ms'
      AND m.type = 'histogram'
    GROUP BY queryid
)
SELECT
    q.query_text,
    c.total_count AS executions,
    ROUND(s.total_value / c.total_count, 2) AS avg_time_ms
FROM histogram_sums s
JOIN histogram_counts c USING (queryid)
JOIN pmetrics_stmts.list_queries() q USING (queryid)
ORDER BY s.total_value DESC;
```

## Comparison with pg_stat_statements

| Feature                  | pg_stat_statements | pmetrics_stmts              |
| ------------------------ | ------------------ | --------------------------- |
| Metrics storage          | In-memory stats    | pmetrics histograms         |
| Distribution tracking    | No (averages only) | Yes (full histogram)        |
| Percentile queries       | No                 | Yes (via histogram buckets) |
| Integration              | Standalone         | Requires pmetrics           |
| Query text normalization | Yes                | No                          |
| Memory overhead          | Fixed pool         | Dynamic (DSA)               |

**Use pmetrics_stmts when**: You need distribution data (p50, p95, p99) or integration with broader pmetrics-based monitoring.

**Use pg_stat_statements when**: You need normalized query text, plan fingerprinting, or minimal dependencies.

## Automatic Cleanup

The extension includes a background worker that periodically cleans up metrics for inactive queries to prevent unbounded memory growth.

Cleanup behavior can be configured via:

- `pmetrics_stmts.cleanup_interval_seconds`: How often cleanup runs (default: 86400 seconds)
- `pmetrics_stmts.cleanup_max_age_seconds`: Age threshold for removal (default: 86400 seconds)

Set `cleanup_interval_seconds` to `0` to disable automatic cleanup. You can still manually trigger cleanup:

```sql
SELECT pmetrics_stmts.cleanup_old_query_metrics(86400);  -- Remove queries inactive for 24 hours
```

## Limitations

- Query text truncated to 1024 bytes
- Requires pmetrics extension

## See Also

- `pmetrics`: Core metrics extension
- PostgreSQL Hook Functions documentation
- `pg_stat_statements`: Standard query statistics module

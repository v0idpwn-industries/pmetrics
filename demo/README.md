# pmetrics_stmts Demo Application

Demonstrates pmetrics_stmts automatic query performance tracking with problematic query patterns where **histogram-based metrics are essential**.

## What's Included

- **PostgreSQL 17** with pmetrics and pmetrics_stmts extensions
- **Python demo app** with 5 workers executing problematic queries
- **Prometheus exporter** serving metrics on port 9187
- **Grafana** with pre-configured dashboards

## Quick Start

```bash
docker-compose up
```

Access:
- PostgreSQL: `localhost:5432` (postgres/postgres)
- Prometheus metrics: http://localhost:9187/metrics
- Prometheus UI: http://localhost:9090
- Grafana: http://localhost:3000 (admin/admin)

## Why This Demo Matters

This demo showcases queries where **averages hide critical performance issues**. Each worker demonstrates scenarios where histogram distributions reveal problems that simple averages would miss:

### 1. N+1 Query Pattern
Classic anti-pattern: fetches products, then queries reviews for each product individually.
- **What histograms reveal**: High frequency of fast queries that aggregate to terrible performance
- **Why averages fail**: Each query is fast (~1-5ms), but doing 10+ per request kills throughput

### 2. Data Skew Worker
Queries products by popularity (no index). Popular products = few rows (fast). Unpopular = many rows (slow).
- **What histograms reveal**: Bimodal distribution with two distinct performance peaks
- **Why averages fail**: "Average" performance doesn't represent either case

### 3. Complex Analytics Worker
CTEs and window functions with varying time ranges (7 days = fast, 365 days = slow).
- **What histograms reveal**: Multi-modal distribution based on data volume
- **Why averages fail**: p50 might be 50ms, p99 might be 5000ms - very different behaviors

### 4. Unpredictable Join Worker
Same JOIN query with different WHERE filters produces wildly different result sets.
- **What histograms reveal**: Tail latency and outliers
- **Why averages fail**: 95% fast doesn't help when 5% timeout

### 5. Table Bloat Worker
Sequential scans get slower as dead tuples accumulate between vacuum runs.
- **What histograms reveal**: Performance degradation over time
- **Why averages fail**: Time-based variance isn't captured by overall averages

## Viewing Metrics

**Prometheus endpoint**: http://localhost:9187/metrics

All query performance metrics are automatically exported with histogram buckets. Each metric includes the full query text as a label, making it easy to identify problematic queries in Grafana/Prometheus.

### Metrics Tracked

pmetrics_stmts automatically tracks:

- `query_planning_time_ms` - Planning time distribution (with query_text, userid, dbid labels)
- `query_execution_time_ms` - Execution time distribution (with query_text, userid, dbid labels)
- `query_rows_returned` - Result set size distribution (with query_text, userid, dbid labels)

### Useful Prometheus Queries

**p95 execution time per query:**
```promql
histogram_quantile(0.95, sum by (query_text, le) (rate(query_execution_time_ms_bucket[5m])))
```

**p99 execution time per query:**
```promql
histogram_quantile(0.99, sum by (query_text, le) (rate(query_execution_time_ms_bucket[5m])))
```

**Query execution rate:**
```promql
sum by (query_text) (rate(query_execution_time_ms_count[5m]))
```

**Identify bimodal distributions (look for queries with multiple peaks in Grafana heatmap)**

## Clean Up

```bash
docker-compose down -v
```

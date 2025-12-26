# pmetrics

A metrics instrumentation toolkit for PostgreSQL. Provides counters, gauges, and histograms with JSONB labels, stored in PostgreSQL dynamic shared memory and queryable via SQL.

## Components

### pmetrics (Core Extension)

PostgreSQL extension providing metrics collection infrastructure with counters, gauges, and histograms. Metrics are stored in dynamic shared memory with no fixed limits and are queryable via SQL.

**See**: [pmetrics/README.md](pmetrics/README.md)

### pmetrics_stmts (Query Tracking Extension)

PostgreSQL extension for automatic query performance tracking. Records planning time, execution time, and rows returned as histograms. Serves as an alternative to `pg_stat_statements` with full distribution tracking.

**Requires**: `pmetrics` core extension.

**See**: [pmetrics_stmts/README.md](pmetrics_stmts/README.md)

### pmetrics Prometheus Exporter

Python service that queries PostgreSQL and exports metrics in Prometheus text exposition format via HTTP endpoint.

**Requires**: `pmetrics` extension (core); `pmetrics_stmts` optional.

**See**: [prometheus_exporter/README.md](prometheus_exporter/README.md)

## Installation

### PostgreSQL Extensions

Build and install both extensions:

```bash
PG_CONFIG=/path/to/pg_config make clean
PG_CONFIG=/path/to/pg_config make
PG_CONFIG=/path/to/pg_config make install
```

Add to `postgresql.conf`:

```ini
# Core metrics only
shared_preload_libraries = 'pmetrics'

# Core metrics + query tracking
shared_preload_libraries = 'pmetrics,pmetrics_stmts'
```

Restart PostgreSQL and create extensions:

```sql
CREATE EXTENSION pmetrics;
CREATE EXTENSION pmetrics_stmts;  -- optional
```

### Prometheus Exporter

```bash
cd prometheus_exporter
pip install -r requirements.txt
DATABASE_URL=postgresql:///mydb python exporter.py
```

## Quick Example

```sql
-- Record metrics
SELECT increment_counter('requests', '{"method": "GET"}');
SELECT record_to_histogram('latency_ms', '{"endpoint": "/api"}', 42.5);

-- Query metrics
SELECT * FROM pmetrics.list_metrics();
```

## Requirements

- PostgreSQL 12 or later
- C compiler for extension building
- Python 3.7+ for Prometheus exporter

## License

MIT

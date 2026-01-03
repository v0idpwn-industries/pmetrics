# pmetrics

pmetrics is a PostgreSQL extension that provides a metrics collection infrastructure for use by other PostgreSQL extensions. It implements counters, gauges, and histograms with JSONB labels, stored in dynamic shared memory and queryable via SQL.

```mermaid
graph LR
    Q[Queries]
    P[PL/pgSQL]
    E[Extensions]

    Q -->|hooks| S[pmetrics_stmts]
    P -->|SQL API| C[pmetrics]
    E -->|C API| C
    S --> C

    C <--. X[pmetrics prometheus exporter]
    X --> M[Prometheus/Grafana]

    style C fill:#336791,stroke:#2c5985,stroke-width:2px,color:#fff
    style S fill:#5d8fc4,stroke:#4a7399,stroke-width:2px,color:#fff
    style X fill:#4CAF50,stroke:#45a049,stroke-width:2px,color:#fff
    style M fill:#f9f9f9,stroke:#666,stroke-width:2px,color:#333
    style Q fill:#fff,stroke:#999,stroke-width:2px,color:#333
    style P fill:#fff,stroke:#999,stroke-width:2px,color:#333
    style E fill:#fff,stroke:#999,stroke-width:2px,color:#333
```

## Components

### pmetrics (Core Extension)

Provides the metrics collection infrastructure with counters, gauges, and histograms. Extensions record metrics via the C API or PL/pgSQL functions can use the SQL API.

[Documentation](https://v0idpwn-industries.github.io/pmetrics/)

### pmetrics_stmts (Query Tracking Extension)

A pg_stat_statements alternative built on top of pmetrics. Tracks query performance metrics (planning time, execution time, rows returned) as histograms.

[Documentation](pmetrics_stmts/README.md)

### pmetrics Prometheus Exporter

Python service that queries PostgreSQL and exports metrics in Prometheus text exposition format.

[Documentation](prometheus_exporter/README.md)

## Installation

### PostgreSQL Extensions

Build and install both extensions:

```bash
PG_CONFIG=/path/to/pg_config make clean
PG_CONFIG=/path/to/pg_config make pmetrics.all
PG_CONFIG=/path/to/pg_config make pmetrics.install
PG_CONFIG=/path/to/pg_config make pmetrics_stmts.all
PG_CONFIG=/path/to/pg_config make pmetrics_stmts.install
```

Add to `postgresql.conf`:

```ini
# Core metrics only
shared_preload_libraries = 'pmetrics'

# Core metrics + query tracking
shared_preload_libraries = 'pmetrics,pmetrics_stmts'
compute_query_id = on  # Required for pmetrics_stmts query tracking
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

## Example

The `examples/pmetrics_txn/` directory contains a minimal example extension demonstrating how to integrate with pmetrics. It tracks PostgreSQL transactions (commits and aborts).

## Requirements

- PostgreSQL 17 or later
- C compiler for building extensions

## License

MIT

# pmetrics

pmetrics is a PostgreSQL extension that provides a metrics collection infrastructure for Postgres. It implements counters, gauges, and histograms with JSONB labels, stored in dynamic shared memory and queryable via SQL.

```mermaid
flowchart LR
    Q[SQL Queries]
    P[PL/pgSQL]
    E[Extensions]

    S[pmetrics_stmts]
    C[pmetrics]

    X[pmetrics Prometheus Exporter]

    Q -->|hooks| S
    P -->|SQL API| C
    E -->|C API| C
    S -->|C API| C

    C e1@--> X
    e1@{ animation: fast }

    style C fill:#336791,stroke:#2c5985,stroke-width:3px,color:#fff
    style S fill:#4a7399,stroke:#336791,stroke-width:2px,color:#fff
    style X fill:#5d8fc4,stroke:#4a7399,stroke-width:2px,color:#fff
    style Q fill:none,stroke:#8d99ae,stroke-width:1px,color:#2b2d42,stroke-dasharray: 5 5
    style P fill:none,stroke:#8d99ae,stroke-width:1px,color:#2b2d42,stroke-dasharray: 5 5
    style E fill:none,stroke:#8d99ae,stroke-width:1px,color:#2b2d42,stroke-dasharray: 5 5
```

## Components

### pmetrics (Core Extension)

Provides the metrics collection infrastructure. Extensions record metrics via the C API, PL/pgSQL functions can use the SQL API.

[Documentation](https://v0idpwn-industries.github.io/pmetrics/)

### pmetrics_stmts (Query Tracking Extension)

A pg_stat_statements alternative built on top of pmetrics. Tracks query performance metrics (planning time, execution time, rows returned, blocks hit) as histograms.

[Documentation](pmetrics_stmts/README.md)

### pmetrics Prometheus Exporter

Service that queries PostgreSQL and exports metrics in Prometheus text exposition format.

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
go build -o pmetrics-exporter
DATABASE_URL=postgresql:///mydb ./pmetrics-exporter
```

## Using pmetrics from other extensions

The `examples/pmetrics_txn/` directory contains a minimal example extension demonstrating how to integrate with pmetrics. It tracks PostgreSQL transactions (commits and aborts).

## Requirements

- PostgreSQL 17 or later
- C compiler for building extensions

## License

MIT

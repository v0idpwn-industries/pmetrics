# pmetrics Prometheus Exporter

A lightweight service that exports `pmetrics` metrics in Prometheus text format. The exporter queries PostgreSQL via SQL and serves metrics on an HTTP endpoint for Prometheus scraping.

## Overview

This exporter translates pmetrics data structures (counters, gauges, histograms with JSONB labels) into Prometheus exposition format.

When `pmetrics_stmts` is enabled, the exporter automatically joins query performance metrics with their corresponding query text, adding truncated query strings as labels for easier identification.

## Dependencies

**Required for building**:
- Go 1.21 or later

**Required PostgreSQL extensions**:
- `pmetrics`: Core metrics extension (required)
- `pmetrics_stmts`: Query tracking extension (optional)

## Installation

```bash
go build -o pmetrics-exporter
```

Build static binary for Linux:

```bash
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -o pmetrics-exporter
```

## Configuration

The exporter is configured via environment variables:

### DATABASE_URL

- **Required**: Yes
- **Format**: PostgreSQL connection string
- **Example**: `postgresql://user:pass@localhost:5432/dbname`

### PORT

- **Required**: No
- **Default**: `9187`
- **Description**: HTTP port for the metrics endpoint

## Usage

### Basic Usage

```bash
DATABASE_URL=postgresql://user:pass@host:5432/dbname ./pmetrics-exporter
```

The service starts and listens on port 9187 (default).

### Custom Port

```bash
DATABASE_URL=postgresql://user:pass@host:5432/dbname PORT=9188 ./pmetrics-exporter
```

## Endpoints

### GET /metrics

Returns metrics in Prometheus text exposition format.

**Content-Type**: `text/plain; version=0.0.4`

**Example output**:

```
# TYPE http_requests_total counter
http_requests_total{method="GET",status="200"} 1523
http_requests_total{method="POST",status="201"} 94

# TYPE active_connections gauge
active_connections{database="mydb"} 42

# TYPE query_execution_time_ms histogram
query_execution_time_ms_bucket{dbid="16384",queryid="3248392847",userid="10",query="SELECT * FROM users WHERE id = $1",le="1"} 245
query_execution_time_ms_bucket{dbid="16384",queryid="3248392847",userid="10",query="SELECT * FROM users WHERE id = $1",le="2"} 489
query_execution_time_ms_bucket{dbid="16384",queryid="3248392847",userid="10",query="SELECT * FROM users WHERE id = $1",le="+Inf"} 512
query_execution_time_ms_count{dbid="16384",queryid="3248392847",userid="10",query="SELECT * FROM users WHERE id = $1"} 512
query_execution_time_ms_sum{dbid="16384",queryid="3248392847",userid="10",query="SELECT * FROM users WHERE id = $1"} 687
```

## Prometheus Configuration

Add a scrape target to `prometheus.yml`:

```yaml
scrape_configs:
  - job_name: 'pmetrics'
    scrape_interval: 15s
    static_configs:
      - targets: ['localhost:9187']
        labels:
          instance: 'pg-prod-01'
```

## Performance Considerations

- **Scrape interval**: The exporter queries PostgreSQL on each scrape. Set `scrape_interval` appropriately (15-60 seconds recommended).
- **Metric cardinality**: High-cardinality labels (many unique label combinations) increase memory usage in both PostgreSQL and Prometheus.
- **Query text truncation**: Query text is truncated to 200 characters in labels to limit metric size.

## Limitations

- No built-in authentication (use firewall rules or reverse proxy)
- No TLS support (use reverse proxy for HTTPS)
- Synchronous request handling (one scrape at a time)
- No caching (queries PostgreSQL on every scrape)

## See Also

- Prometheus exposition formats: https://prometheus.io/docs/instrumenting/exposition_formats/
- `pmetrics`: Core PostgreSQL metrics extension
- `pmetrics_stmts`: Query performance tracking extension

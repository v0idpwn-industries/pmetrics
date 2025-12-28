# pmetrics_stmts Benchmark Suite

Benchmark comparing performance overhead of pmetrics_stmts vs pg_stat_statements vs baseline PostgreSQL.

## Setup

Start all three PostgreSQL containers:

```bash
docker-compose up -d
```

This starts:
- **baseline** (port 5432): PostgreSQL with no extensions
- **pgss** (port 5433): PostgreSQL with pg_stat_statements
- **pmetrics** (port 5434): PostgreSQL with pmetrics + pmetrics_stmts

## Run Benchmark

```bash
./run-benchmark.sh
```

### Configuration

Set environment variables to customize the benchmark:

```bash
SCALE_FACTOR=50 CLIENTS=20 THREADS=8 DURATION=120 ./run-benchmark.sh
```

Variables:
- `SCALE_FACTOR`: pgbench scale factor (default: 10)
- `CLIENTS`: number of concurrent clients (default: 10)
- `THREADS`: number of threads (default: 4)
- `DURATION`: benchmark duration in seconds (default: 60)

## Clean Up

Stop and remove containers:

```bash
docker-compose down
```

Remove volumes:

```bash
docker-compose down -v
```

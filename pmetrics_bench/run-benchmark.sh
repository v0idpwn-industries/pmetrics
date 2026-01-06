#!/bin/bash
set -e

# Default configuration
CLIENTS=${CLIENTS:-10}
DURATION=${DURATION:-60}
BENCH_SQL=${BENCH_SQL:-bench.sql}
PGHOST=${PGHOST:-localhost}
PGPORT=${PGPORT:-5432}
PGUSER=${PGUSER:-$(whoami)}
PGDATABASE=${PGDATABASE:-postgres}

echo "=== pmetrics Benchmark ==="
echo "Clients: $CLIENTS"
echo "Duration: ${DURATION}s"
echo "Database: ${PGHOST}:${PGPORT}/${PGDATABASE}"
echo ""

# Clear metrics before benchmark
echo "Clearing existing metrics..."
psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -c "SELECT pmetrics.clear_metrics();" > /dev/null

echo "Running benchmark..."
echo ""

# Run pgbench with custom script
# Use same number of threads as clients for max parallelism
pgbench -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" \
  -c "$CLIENTS" \
  -j "$CLIENTS" \
  -T "$DURATION" \
  -P 5 \
  -f "$BENCH_SQL"

echo ""
echo "=== Benchmark Complete ==="
echo ""
echo "To run with different settings:"
echo "  CLIENTS=20 DURATION=30 ./run-benchmark.sh"

#!/bin/bash
set -e

SCALE_FACTOR=${SCALE_FACTOR:-10}
CLIENTS=${CLIENTS:-10}
THREADS=${THREADS:-4}
DURATION=${DURATION:-60}

export PGPASSWORD=postgres

echo "=== Baseline PostgreSQL (port 5432) ==="
pgbench -h localhost -p 5432 -U postgres -i -s "$SCALE_FACTOR" benchdb
pgbench -h localhost -p 5432 -U postgres -c "$CLIENTS" -j "$THREADS" -T "$DURATION" -P 10 -S benchdb

echo ""
echo "=== pg_stat_statements (port 5433) ==="
pgbench -h localhost -p 5433 -U postgres -i -s "$SCALE_FACTOR" benchdb
pgbench -h localhost -p 5433 -U postgres -c "$CLIENTS" -j "$THREADS" -T "$DURATION" -P 10 -S benchdb

echo ""
echo "=== pmetrics_stmts (port 5434) ==="
pgbench -h localhost -p 5434 -U postgres -i -s "$SCALE_FACTOR" benchdb
pgbench -h localhost -p 5434 -U postgres -c "$CLIENTS" -j "$THREADS" -T "$DURATION" -P 10 -S benchdb

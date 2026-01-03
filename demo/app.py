#!/usr/bin/env python3
"""
pmetrics Demo

Query patterns with variable performance:

1. pg_sleep($1): 10ms (10%), 100ms (89%), 5s (1%)
2. generate_series(1, $1): 1 row (50%), 10 rows (30%), 100 rows (15%), 1000 rows (4%), 10000 rows (1%)
3. table scan (BETWEEN): small ranges 100-1000 rows (70%), large ranges 10K-50K rows (30%)

Table size: 500,000 rows (~100-200MB) to generate buffer cache misses and disk reads
"""
import os
import time
import random
import threading
import psycopg2
from psycopg2 import pool
from contextlib import contextmanager


DATABASE_URL = os.environ.get(
    "DATABASE_URL", "postgresql://postgres:postgres@localhost:5432/demo"
)

# Thread-safe connection pool
connection_pool = None


def init_pool():
    """Initialize the connection pool."""
    global connection_pool
    connection_pool = pool.ThreadedConnectionPool(
        minconn=1, maxconn=10, dsn=DATABASE_URL
    )


@contextmanager
def get_db_connection():
    """Get a connection from the pool. Always use with 'with' statement."""
    conn = connection_pool.getconn()
    try:
        yield conn
    finally:
        connection_pool.putconn(conn)


def setup_schema():
    """Create pmetrics extensions and demo tables."""
    print("Setting up database...")
    with get_db_connection() as conn:
        conn.autocommit = True
        with conn.cursor() as cur:
            # Create extensions
            cur.execute("CREATE EXTENSION IF NOT EXISTS pmetrics;")
            cur.execute("CREATE EXTENSION IF NOT EXISTS pmetrics_stmts;")

            # Create demo table for buffer tracking
            # Large enough to exceed typical shared_buffers and generate disk reads
            cur.execute("DROP TABLE IF EXISTS demo_buffer_test;")
            cur.execute(
                """
                CREATE TABLE demo_buffer_test (
                    id SERIAL PRIMARY KEY,
                    data TEXT,
                    padding TEXT,
                    value INTEGER
                );
            """
            )

            # Insert test data (500K rows with padding to ensure significant size)
            # This should be ~100-200MB, ensuring cache misses
            cur.execute(
                """
                INSERT INTO demo_buffer_test (data, padding, value)
                SELECT
                    'data_' || i::text,
                    repeat('x', 200),
                    (random() * 1000)::integer
                FROM generate_series(1, 500000) i;
            """
            )

            # Create index
            cur.execute(
                "CREATE INDEX idx_demo_buffer_value ON demo_buffer_test(value);"
            )

    print("Setup complete!\n")


def worker_sleep():
    """Worker that runs pg_sleep with variable duration."""
    worker_name = "sleep"
    print(f"[{worker_name}] Starting worker...")

    while True:
        try:
            # Determine sleep duration
            rand = random.random()
            if rand < 0.10:
                sleep_ms = 10  # 10% of the time: 10ms
            elif rand < 0.99:
                sleep_ms = 100  # 89% of the time: 100ms
            else:
                sleep_ms = 5000  # 1% of the time: 5s

            with get_db_connection() as conn:
                conn.autocommit = True
                with conn.cursor() as cur:
                    cur.execute("SELECT pg_sleep(%s);", (sleep_ms / 1000.0,))

        except Exception as e:
            print(f"[{worker_name}] Error: {e}")

        time.sleep(random.uniform(0.01, 0.05))


def worker_read_rows():
    """Worker that reads variable number of rows using generate_series."""
    worker_name = "read_rows"
    print(f"[{worker_name}] Starting worker...")

    while True:
        try:
            # Determine row count
            rand = random.random()
            if rand < 0.50:
                count = 1  # 50%: 1 row
            elif rand < 0.80:
                count = 10  # 30%: 10 rows
            elif rand < 0.95:
                count = 100  # 15%: 100 rows
            elif rand < 0.99:
                count = 1000  # 4%: 1000 rows
            else:
                count = 10000  # 1%: 10000 rows

            with get_db_connection() as conn:
                conn.autocommit = True
                with conn.cursor() as cur:
                    cur.execute("SELECT * FROM generate_series(1, %s);", (count,))
                    rows = cur.fetchall()

        except Exception as e:
            print(f"[{worker_name}] Error: {e}")

        time.sleep(random.uniform(0.01, 0.05))


def worker_fluctuating():
    """Worker with sinusoidal query performance pattern."""
    import math

    worker_name = "fluctuating"
    print(f"[{worker_name}] Starting worker...")

    start_time = time.time()

    while True:
        try:
            # Calculate sinusoidal sleep duration
            # Period: 5 minutes (one full cycle per 5 minutes)
            # Range: 50ms to 500ms
            elapsed = time.time() - start_time
            phase = (elapsed % 300) / 300 * 2 * math.pi  # 300 seconds = 5 minutes
            sin_value = math.sin(phase)  # -1 to 1

            # Map sin_value from [-1, 1] to [0.050, 0.500] (50ms to 500ms)
            sleep_duration = 0.275 + sin_value * 0.225

            with get_db_connection() as conn:
                conn.autocommit = True
                with conn.cursor() as cur:
                    cur.execute("SELECT pg_sleep(%s);", (sleep_duration,))

        except Exception as e:
            print(f"[{worker_name}] Error: {e}")

        time.sleep(random.uniform(0.01, 0.05))


def worker_table_scan():
    """Worker that scans table ranges to generate buffer activity."""
    worker_name = "table_scan"
    print(f"[{worker_name}] Starting worker...")

    while True:
        try:
            # Bimodal distribution: small ranges (cached) vs large ranges (disk reads)
            rand = random.random()
            if rand < 0.70:
                # 70%: Small range (likely cached, high buffer hits)
                start = random.randint(1, 490000)
                end = start + random.randint(100, 1000)
            else:
                # 30%: Large range (will cause disk reads due to table size)
                start = random.randint(1, 400000)
                end = start + random.randint(10000, 50000)

            with get_db_connection() as conn:
                conn.autocommit = True
                with conn.cursor() as cur:
                    cur.execute(
                        "SELECT * FROM demo_buffer_test WHERE id BETWEEN %s AND %s;",
                        (start, end),
                    )
                    rows = cur.fetchall()

        except Exception as e:
            print(f"[{worker_name}] Error: {e}")

        time.sleep(random.uniform(0.01, 0.05))


def main():
    """Main entry point."""
    print("=" * 70)
    print("pmetrics Demo")
    print("=" * 70)

    # Initialize connection pool
    init_pool()

    setup_schema()

    workers = [
        threading.Thread(target=worker_sleep, daemon=True),
        threading.Thread(target=worker_sleep, daemon=True),
        threading.Thread(target=worker_sleep, daemon=True),
        threading.Thread(target=worker_read_rows, daemon=True),
        threading.Thread(target=worker_read_rows, daemon=True),
        threading.Thread(target=worker_read_rows, daemon=True),
        threading.Thread(target=worker_fluctuating, daemon=True),
        threading.Thread(target=worker_fluctuating, daemon=True),
        threading.Thread(target=worker_table_scan, daemon=True),
        threading.Thread(target=worker_table_scan, daemon=True),
    ]

    for worker in workers:
        worker.start()

    print("\nWorkers running.")
    print("Metrics: http://localhost:9187/metrics")
    print("Grafana: http://localhost:3000")
    print("Press Ctrl+C to stop\n")

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nShutting down...")


if __name__ == "__main__":
    main()

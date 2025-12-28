#!/usr/bin/env python3
"""
pmetrics Demo

Two query patterns with variable performance:

1. pg_sleep($1): 10ms (10%), 100ms (89%), 5s (1%)
2. generate_series(1, $1): 1 row (50%), 10 rows (30%), 100 rows (15%), 1000 rows (4%), 10000 rows (1%)
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
    """Create pmetrics extensions."""
    print("Setting up database...")
    with get_db_connection() as conn:
        conn.autocommit = True
        with conn.cursor() as cur:
            # Create extensions
            cur.execute("CREATE EXTENSION IF NOT EXISTS pmetrics;")
            cur.execute("CREATE EXTENSION IF NOT EXISTS pmetrics_stmts;")

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
            # Range: 5ms to 500ms
            elapsed = time.time() - start_time
            phase = (elapsed % 300) / 300 * 2 * math.pi  # 300 seconds = 5 minutes
            sin_value = math.sin(phase)  # -1 to 1

            # Map sin_value from [-1, 1] to [0.005, 0.500] (5ms to 500ms)
            sleep_duration = 0.005 + (sin_value + 1) / 2 * 0.495

            with get_db_connection() as conn:
                conn.autocommit = True
                with conn.cursor() as cur:
                    cur.execute("SELECT pg_sleep(%s);", (sleep_duration,))

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

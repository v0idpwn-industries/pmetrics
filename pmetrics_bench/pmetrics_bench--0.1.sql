/* pmetrics_bench--0.1.sql */

/**
 * Benchmark function that reuses a small set of metrics.
 * Increments 10 counters repeatedly (1M operations total).
 *
 * Returns the number of operations completed.
 * Use with pgbench to measure throughput under concurrent load.
 */
CREATE FUNCTION bench_metrics ()
    RETURNS BIGINT
    AS '$libdir/pmetrics_bench'
    LANGUAGE C STRICT;

/**
 * Benchmark function that creates new metrics constantly.
 * Creates 1M unique metrics, one per operation.
 *
 * Returns the number of operations completed.
 * Use with pgbench to measure behavior when creating new metrics.
 */
CREATE FUNCTION bench_new_metrics ()
    RETURNS BIGINT
    AS '$libdir/pmetrics_bench'
    LANGUAGE C STRICT;

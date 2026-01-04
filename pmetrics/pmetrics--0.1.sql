/** Composite type representing a metric entry */
CREATE TYPE metric_type AS (name TEXT, labels JSONB, type TEXT, bucket INTEGER, value BIGINT);

/** Composite type representing a histogram bucket upper bound */
CREATE TYPE histogram_buckets_type AS (bucket INTEGER);

/**
 * Increment a counter by 1.
 * Returns the new counter value, or NULL if pmetrics.enabled=false.
 */
CREATE FUNCTION increment_counter (name TEXT, labels JSONB) RETURNS BIGINT AS '$libdir/pmetrics' LANGUAGE C STRICT;

/**
 * Increment a counter by a specified amount (must be > 0).
 * Returns the new counter value, or NULL if pmetrics.enabled=false.
 */
CREATE FUNCTION increment_counter_by (name TEXT, labels JSONB, increment INTEGER) RETURNS BIGINT AS '$libdir/pmetrics' LANGUAGE C STRICT;

/**
 * Set a gauge to an absolute value.
 * Returns the value that was set, or NULL if pmetrics.enabled=false.
 */
CREATE FUNCTION set_gauge (name TEXT, labels JSONB, value BIGINT) RETURNS BIGINT AS '$libdir/pmetrics' LANGUAGE C STRICT;

/**
 * Add or subtract from a gauge (value cannot be zero).
 * Returns the new gauge value, or NULL if pmetrics.enabled=false.
 */
CREATE FUNCTION add_to_gauge (name TEXT, labels JSONB, value BIGINT) RETURNS BIGINT AS '$libdir/pmetrics' LANGUAGE C STRICT;

/**
 * Record a value to a histogram.
 * Returns the updated bucket count, or NULL if pmetrics.enabled=false.
 */
CREATE FUNCTION record_to_histogram (name TEXT, labels JSONB, value FLOAT) RETURNS BIGINT AS '$libdir/pmetrics' LANGUAGE C STRICT;

/**
 * List all metrics currently stored in shared memory.
 * Histograms have multiple rows (one per non-empty bucket).
 * Empty buckets are not returned.
 */
CREATE FUNCTION list_metrics () RETURNS SETOF metric_type AS '$libdir/pmetrics' LANGUAGE C STRICT;

/**
 * List all possible histogram bucket upper bounds based on current configuration.
 */
CREATE FUNCTION list_histogram_buckets () RETURNS SETOF histogram_buckets_type AS '$libdir/pmetrics' LANGUAGE C STRICT;

/**
 * Clear all metrics from shared memory.
 * Returns the number of metrics deleted.
 */
CREATE FUNCTION clear_metrics () RETURNS BIGINT AS '$libdir/pmetrics' LANGUAGE C STRICT;

/**
 * Delete all metrics with the specified name and labels.
 * Returns the number of metrics deleted.
 * Deletes all metric types (counter, gauge, histogram buckets, histogram sum) that match.
 */
CREATE FUNCTION delete_metric (name TEXT, labels JSONB) RETURNS BIGINT AS '$libdir/pmetrics' LANGUAGE C STRICT;

-- Type documentation
COMMENT ON TYPE metric_type IS 'Composite type representing a metric entry with name, labels, type, bucket (for histograms), and value';
COMMENT ON TYPE histogram_buckets_type IS 'Composite type representing a histogram bucket upper bound';

-- Function documentation
COMMENT ON FUNCTION increment_counter(TEXT, JSONB) IS
'Increment a counter by 1. Returns the new counter value, or NULL if pmetrics.enabled=false.';

COMMENT ON FUNCTION increment_counter_by(TEXT, JSONB, INTEGER) IS
'Increment a counter by a specified amount (must be > 0). Returns the new counter value, or NULL if pmetrics.enabled=false.';

COMMENT ON FUNCTION set_gauge(TEXT, JSONB, BIGINT) IS
'Set a gauge to an absolute value. Returns the value that was set, or NULL if pmetrics.enabled=false.';

COMMENT ON FUNCTION add_to_gauge(TEXT, JSONB, BIGINT) IS
'Add or subtract from a gauge (value cannot be zero). Returns the new gauge value, or NULL if pmetrics.enabled=false.';

COMMENT ON FUNCTION record_to_histogram(TEXT, JSONB, FLOAT) IS
'Record a value to a histogram. Returns the updated bucket count, or NULL if pmetrics.enabled=false.';

COMMENT ON FUNCTION list_metrics() IS
'List all metrics currently stored in shared memory. Histograms have multiple rows (one per non-empty bucket). Empty buckets are not returned.';

COMMENT ON FUNCTION list_histogram_buckets() IS
'List all possible histogram bucket upper bounds based on current configuration.';

COMMENT ON FUNCTION clear_metrics() IS
'Clear all metrics from shared memory. Returns the number of metrics deleted.';

COMMENT ON FUNCTION delete_metric(TEXT, JSONB) IS
'Delete all metrics with the specified name and labels. Returns the number of metrics deleted. Deletes all metric types (counter, gauge, histogram buckets, histogram sum) that match.';

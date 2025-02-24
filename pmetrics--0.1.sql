CREATE TYPE metric_type AS (name TEXT, labels TEXT, value BIGINT);

CREATE FUNCTION increment_counter (name TEXT, labels TEXT) RETURNS BIGINT AS '$libdir/pmetrics' LANGUAGE C STRICT;

CREATE FUNCTION increment_counter_by (name TEXT, labels TEXT, increment INTEGER) RETURNS BIGINT AS '$libdir/pmetrics' LANGUAGE C STRICT;

CREATE FUNCTION set_gauge (name TEXT, labels TEXT, value BIGINT) RETURNS BIGINT AS '$libdir/pmetrics' LANGUAGE C STRICT;

CREATE FUNCTION add_to_gauge (name TEXT, labels TEXT, value BIGINT) RETURNS BIGINT AS '$libdir/pmetrics' LANGUAGE C STRICT;

CREATE FUNCTION record_to_histogram (name TEXT, labels TEXT, value FLOAT) RETURNS BIGINT AS '$libdir/pmetrics' LANGUAGE C STRICT;

CREATE FUNCTION list_metrics () RETURNS SETOF metric_type AS '$libdir/pmetrics' LANGUAGE C STRICT;

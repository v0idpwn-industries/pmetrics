CREATE TYPE metric_type AS (name TEXT, labels TEXT, value BIGINT);

CREATE FUNCTION increment_counter (cname TEXT, labels TEXT) RETURNS BIGINT AS '$libdir/pmetrics' LANGUAGE C STRICT;

CREATE FUNCTION increment_counter_by (cname TEXT, labels TEXT, increment INTEGER) RETURNS BIGINT AS '$libdir/pmetrics' LANGUAGE C STRICT;

CREATE FUNCTION set_gauge (cname TEXT, labels TEXT, value BIGINT) RETURNS BIGINT AS '$libdir/pmetrics' LANGUAGE C STRICT;

CREATE FUNCTION add_to_gauge (cname TEXT, labels TEXT, value BIGINT) RETURNS BIGINT AS '$libdir/pmetrics' LANGUAGE C STRICT;

CREATE FUNCTION list_metrics () RETURNS SETOF metric_type AS '$libdir/pmetrics' LANGUAGE C STRICT;

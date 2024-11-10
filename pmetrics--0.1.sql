CREATE TYPE metric_type AS (
  name TEXT,
  value BIGINT
);

CREATE FUNCTION inc_counter(cname TEXT)
RETURNS BIGINT
AS '$libdir/pmetrics'
LANGUAGE C STRICT;

CREATE FUNCTION list_metrics()
RETURNS SETOF metric_type
AS '$libdir/pmetrics'
LANGUAGE C STRICT;

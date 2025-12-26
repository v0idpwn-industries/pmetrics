/* pmetrics_stmts--0.1.sql */

/** Composite type representing a stored query */
CREATE TYPE query_text_type AS (queryid BIGINT, query_text TEXT);

/**
 * List all stored query texts.
 *
 * Returns query texts that have been tracked, keyed by queryid.
 * Query text is stored on first observation (first-write-wins).
 * Query text is truncated to 1024 bytes if longer.
 */
CREATE FUNCTION list_queries ()
    RETURNS SETOF query_text_type
    AS '$libdir/pmetrics_stmts'
    LANGUAGE C STRICT;

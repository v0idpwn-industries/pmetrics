-- pmetrics_txn extension version 0.1
--
-- This extension doesn't provide any SQL-callable functions.
-- It works entirely through PostgreSQL's hook system, automatically
-- tracking transaction events (commits and aborts) in the background.
--
-- All the magic happens in C code when the extension is loaded via
-- shared_preload_libraries.
--
-- To view the metrics this extension creates, use pmetrics functions:
--   SELECT * FROM list_metrics() WHERE name = 'pg_transactions';

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pmetrics_txn" to load this file. \quit

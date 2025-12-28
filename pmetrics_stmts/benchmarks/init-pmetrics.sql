-- PostgreSQL with pmetrics and pmetrics_stmts initialization

CREATE EXTENSION pmetrics;
CREATE EXTENSION pmetrics_stmts;

SELECT 'pmetrics and pmetrics_stmts initialized' AS status;

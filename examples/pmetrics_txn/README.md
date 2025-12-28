# pmetrics_txn - Simple Transaction Tracking Extension

A minimal PostgreSQL extension demonstrating how to integrate with pmetrics to track database events using PostgreSQL's hook system.

## What It Does

This extension tracks PostgreSQL transactions and records metrics for:
- **Commits**: Successful transaction completions
- **Aborts**: Transaction rollbacks

It creates a single counter metric: `pg_transactions` with an `event` label indicating the type (commit/abort).

## Why This Example?

This is intentionally the **simplest possible pmetrics extension** to demonstrate:

1. **Hook Registration**: How to register a PostgreSQL callback hook
2. **Event Tracking**: How to capture database events automatically
3. **pmetrics Integration**: How to call pmetrics functions from another extension
4. **Minimal Code**: ~100 lines including comments - perfect for understanding the basics

Unlike `pmetrics_stmt` which is feature-rich and complex, this example focuses on clarity and educational value.

## How It Works

### The XactCallback Hook

PostgreSQL's `RegisterXactCallback()` allows extensions to register functions that are called automatically at transaction lifecycle events:

- `XACT_EVENT_COMMIT` - When a transaction commits successfully
- `XACT_EVENT_ABORT` - When a transaction is rolled back
- Plus others for two-phase commit, parallel transactions, etc.

### The Flow

```
User runs: BEGIN; ... COMMIT;
                              ↓
         PostgreSQL transaction system
                              ↓
         Calls all registered XactCallbacks
                              ↓
         pmetrics_txn_callback() executes
                              ↓
         Calls pmetrics increment_counter()
                              ↓
         Metric updated in shared memory
```

## Building and Installing

```bash
cd examples/pmetrics_txn
PG_CONFIG=/opt/homebrew/opt/postgresql@17/bin/pg_config make clean
PG_CONFIG=/opt/homebrew/opt/postgresql@17/bin/pg_config make
PG_CONFIG=/opt/homebrew/opt/postgresql@17/bin/pg_config make install
```

## Setup

**Prerequisites**: pmetrics must be installed and loaded first.

Add to `postgresql.conf`:
```
shared_preload_libraries = 'pmetrics,pmetrics_txn'
```

Restart PostgreSQL, then:
```sql
CREATE EXTENSION pmetrics;
CREATE EXTENSION pmetrics_txn;
```

## Usage

Just use PostgreSQL normally. The extension tracks automatically:

```sql
-- Generate some commits
BEGIN;
CREATE TABLE test (id int);
COMMIT;

-- Generate an abort
BEGIN;
INSERT INTO test VALUES (1);
ROLLBACK;

-- View the metrics
SELECT * FROM list_metrics() WHERE name = 'pg_transactions';
```

Expected output:
```
      name       |      labels       | type    | bucket | value
-----------------+-------------------+---------+--------+-------
 pg_transactions | {"event":"commit"}| COUNTER |      0 |     1
 pg_transactions | {"event":"abort"} | COUNTER |      0 |     1
```

## Code Structure

The entire implementation is in `pmetrics_txn.c`:

1. **Header Includes**: PostgreSQL and pmetrics headers
2. **Module Magic**: Required PostgreSQL extension macros
3. **Function Declarations**: External pmetrics functions we'll call
4. **Helper Function**: `labels_with()` to build JSONB labels easily
5. **Callback Function**: `pmetrics_txn_callback()` - the hook implementation
6. **Initialization**: `_PG_init()` - registers the hook when extension loads

## Learning Points

**Why shared_preload_libraries?**
- Extensions that use hooks must be loaded at server start
- This allows them to register callbacks before any transactions run

**Why declare pmetrics functions?**
- We're calling functions from another extension (pmetrics)
- PostgreSQL's dynamic linking finds them at runtime
- No need to link against pmetrics at build time

**Why use JSONB for labels?**
- pmetrics uses JSONB for structured key-value metadata
- Allows flexible, queryable metric dimensions
- More powerful than simple string concatenation

**Why so few lines of code?**
- PostgreSQL's hook system does the heavy lifting
- pmetrics handles all the shared memory complexity
- We just connect the dots!

## Extending This Example

Want to track more? Easy additions:

- **Track prepared transactions**: Add `XACT_EVENT_PREPARE` case
- **Add database labels**: Include `MyDatabaseId` in labels
- **Add user labels**: Include `GetUserId()` in labels
- **Track duration**: Record timestamps and compute transaction duration

## Comparison with pmetrics_stmt

| Feature | pmetrics_txn | pmetrics_stmt |
|---------|--------------|---------------|
| Lines of code | ~100 | ~500 |
| Hooks used | 1 (XactCallback) | 3 (Planner, ExecutorStart, ExecutorEnd) |
| Metrics tracked | 2 (commit/abort) | Many (planning time, execution time, rows) |
| Query parsing | None | Full query normalization |
| State management | None | Per-query state tracking |
| Purpose | Educational example | Production-ready query tracking |

## Files

- `README.md` - This file
- `pmetrics_txn.control` - Extension metadata (version, dependencies)
- `pmetrics_txn--0.1.sql` - SQL installation script (minimal for this extension)
- `pmetrics_txn.c` - Main source code with detailed comments
- `Makefile` - Build configuration using PostgreSQL's PGXS system

## License

Same as pmetrics and PostgreSQL.

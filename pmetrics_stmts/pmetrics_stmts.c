/*
 * pmetrics_stmts - Query performance tracking for pmetrics
 *
 * This extension tracks query execution metrics (planning time, execution time,
 * rows returned) and stores them using the pmetrics metrics system.
 *
 * Requires pmetrics to be loaded first via shared_preload_libraries.
 *
 * Most of this code is derived from PostgreSQL's contrib/pg_stat_statements.
 * Copyright (c) 2008-2025, PostgreSQL Global Development Group
 */

#include "postgres.h"
#include "extension/pmetrics/pmetrics.h"

#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/dshash.h"
#include "miscadmin.h"
#include "nodes/queryjumble.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "parser/scanner.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/guc.h"
#include "utils/jsonb.h"
#include "utils/timestamp.h"
#include "executor/spi.h"
#include "tcop/utility.h"
#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "pgstat.h"
#include "access/xact.h"

#include <stdio.h>

/* Signal handling */
static volatile sig_atomic_t got_SIGTERM = false;

static void pmetrics_stmts_sigterm_handler(SIGNAL_ARGS)
{
	int save_errno = errno;
	got_SIGTERM = true;
	SetLatch(MyLatch);
	errno = save_errno;
}

PG_MODULE_MAGIC;

/* LWLock tranche ID for queries table */
#define LWTRANCHE_PMETRICS_QUERIES 1003

#define MAX_QUERY_TEXT_LEN 1024 /* Max query text length we store */
#define CLEANUP_INTERVAL_MS 3600000 /* Cleanup interval in milliseconds (1 hour) */
#define CLEANUP_MAX_AGE_SECONDS 3600 /* Clean up queries older than this many seconds (1 hour) */

/* Shared state stored in static shared memory */
typedef struct PMetricsStmtsSharedState {
	dsa_handle pmetrics_dsa; /* Reference to pmetrics DSA */
	dshash_table_handle queries_handle;
	LWLock *init_lock;
	bool initialized;
} PMetricsStmtsSharedState;

static PMetricsStmtsSharedState *stmts_shared_state = NULL;

/* Backend-local state (not in shared memory) */
static dsa_area *local_dsa = NULL;
static dshash_table *local_queries_table = NULL;

/* Hooks */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart_hook = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd_hook = NULL;

/* Nesting level for query hooks */
static int nesting_level = 0;

/* Configs */
#define DEFAULT_TRACK_TIMES true
#define DEFAULT_TRACK_ROWS true
#define DEFAULT_TRACK_BUFFERS false

static bool pmetrics_stmts_track_times = DEFAULT_TRACK_TIMES;
static bool pmetrics_stmts_track_rows = DEFAULT_TRACK_ROWS;
static bool pmetrics_stmts_track_buffers = DEFAULT_TRACK_BUFFERS;

/* Query text storage structures */
typedef struct {
	uint64 queryid;
} QueryTextKey;

typedef struct {
	QueryTextKey key;
	int query_len;
	char query_text[MAX_QUERY_TEXT_LEN];
} QueryTextEntry;

/* Function declarations */
void _PG_init(void);
static void pmetrics_stmts_shmem_request(void);
static void pmetrics_stmts_shmem_startup(void);
static dshash_table *get_queries_table(void);
static void cleanup_pmetrics_stmts_backend(int code, Datum arg);
static void pmetrics_stmts_post_parse_analyze(ParseState *pstate, Query *query,
                                              JumbleState *jstate);
static PlannedStmt *pmetrics_stmts_planner_hook(Query *parse,
                                                const char *query_string,
                                                int cursorOptions,
                                                ParamListInfo boundParams);
static void pmetrics_stmts_ExecutorStart_hook(QueryDesc *queryDesc, int eflags);
static void pmetrics_stmts_ExecutorEnd_hook(QueryDesc *queryDesc);
static Jsonb *build_query_labels(uint64 queryid, Oid userid, Oid dbid);

/* Background worker functions */
void pmetrics_stmts_cleanup_worker_main(Datum main_arg);

/* Cleanup function - can be called from C or SQL */
int64 pmetrics_stmts_cleanup_old_metrics(int64 max_age_seconds);

/* Query normalization functions (adapted from pg_stat_statements) */
static int comp_location(const void *a, const void *b);
static void fill_in_constant_lengths(JumbleState *jstate, const char *query,
                                     int query_loc);
static char *generate_normalized_query(JumbleState *jstate, const char *query,
                                       int query_loc, int *query_len_p);

/*
 * Hash function for QueryTextKey (dshash signature).
 * Simple hash of the uint64 queryid.
 */
static uint32 query_hash_dshash(const void *key, size_t key_size, void *arg)
{
	const QueryTextKey *k = (const QueryTextKey *)key;
	return hash_bytes((const unsigned char *)&k->queryid, sizeof(uint64));
}

/*
 * Compare function for QueryTextKey (dshash signature).
 */
static int query_compare_dshash(const void *a, const void *b, size_t key_size,
                                void *arg)
{
	const QueryTextKey *k1 = (const QueryTextKey *)a;
	const QueryTextKey *k2 = (const QueryTextKey *)b;

	if (k1->queryid < k2->queryid)
		return -1;
	if (k1->queryid > k2->queryid)
		return 1;
	return 0;
}

static const dshash_parameters queries_params = {
    .key_size = sizeof(QueryTextKey),
    .entry_size = sizeof(QueryTextEntry),
    .compare_function = query_compare_dshash,
    .hash_function = query_hash_dshash,
    .copy_function = dshash_memcpy,
    .tranche_id = LWTRANCHE_PMETRICS_QUERIES};

static void pmetrics_stmts_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(MAXALIGN(sizeof(PMetricsStmtsSharedState)));
	RequestNamedLWLockTranche("pmetrics_stmts_init", 1);
}

static void pmetrics_stmts_shmem_startup(void)
{
	bool found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	if (!pmetrics_is_initialized())
		elog(ERROR, "pmetrics_stmts requires pmetrics to be loaded first in "
		            "shared_preload_libraries");

	stmts_shared_state =
	    ShmemInitStruct("pmetrics_stmts_shared_state",
	                    sizeof(PMetricsStmtsSharedState), &found);

	if (!found) {
		dsa_area *dsa;
		dshash_table *queries_table;

		/* Reuse pmetrics' DSA to avoid multiple DSA areas */
		stmts_shared_state->pmetrics_dsa = pmetrics_get_dsa_handle();

		dsa = dsa_attach(stmts_shared_state->pmetrics_dsa);

		queries_table = dshash_create(dsa, &queries_params, NULL);
		stmts_shared_state->queries_handle =
		    dshash_get_hash_table_handle(queries_table);

		stmts_shared_state->init_lock =
		    &(GetNamedLWLockTranche("pmetrics_stmts_init")[0].lock);
		stmts_shared_state->initialized = true;

		/*
		 * Detach from postmaster so backends don't inherit the attachment
		 * state. pmetrics has already pinned the DSA.
		 */
		dshash_detach(queries_table);
		dsa_detach(dsa);

		elog(DEBUG1, "pmetrics_stmts: initialized with DSA handle %lu",
		     (unsigned long)stmts_shared_state->pmetrics_dsa);
	}
}

void _PG_init(void)
{
	DefineCustomBoolVariable("pmetrics_stmts.track_times",
	                         "Track query planning and execution times", NULL,
	                         &pmetrics_stmts_track_times, DEFAULT_TRACK_TIMES,
	                         PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable("pmetrics_stmts.track_rows",
	                         "Track query row counts", NULL,
	                         &pmetrics_stmts_track_rows, DEFAULT_TRACK_ROWS,
	                         PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
	    "pmetrics_stmts.track_buffers", "Track buffer usage distributions",
	    NULL, &pmetrics_stmts_track_buffers, DEFAULT_TRACK_BUFFERS, PGC_SIGHUP,
	    0, NULL, NULL, NULL);

	MarkGUCPrefixReserved("pmetrics_stmts");

	LWLockRegisterTranche(LWTRANCHE_PMETRICS_QUERIES, "pmetrics_queries");

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pmetrics_stmts_shmem_startup;
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = pmetrics_stmts_shmem_request;

	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = pmetrics_stmts_post_parse_analyze;
	prev_planner_hook = planner_hook;
	planner_hook = pmetrics_stmts_planner_hook;
	prev_ExecutorStart_hook = ExecutorStart_hook;
	ExecutorStart_hook = pmetrics_stmts_ExecutorStart_hook;
	prev_ExecutorEnd_hook = ExecutorEnd_hook;
	ExecutorEnd_hook = pmetrics_stmts_ExecutorEnd_hook;

	/* Register background worker for periodic cleanup */
	{
		BackgroundWorker worker;

		memset(&worker, 0, sizeof(worker));
		sprintf(worker.bgw_name, "pmetrics_stmts cleanup");
		sprintf(worker.bgw_type, "pmetrics_stmts cleanup");
		worker.bgw_flags =
		    BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
		worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
		worker.bgw_restart_time = 3600; /* Restart every hour */
		sprintf(worker.bgw_library_name, "pmetrics_stmts");
		sprintf(worker.bgw_function_name, "pmetrics_stmts_cleanup_worker_main");
		worker.bgw_notify_pid = 0;

		RegisterBackgroundWorker(&worker);
	}
}

/*
 * Cleanup callback when backend exits.
 * Detach from queries table only (pmetrics owns the DSA).
 */
static void cleanup_pmetrics_stmts_backend(int code, Datum arg)
{
	if (local_queries_table != NULL) {
		dshash_detach(local_queries_table);
		local_queries_table = NULL;
	}

	/*
	 * Don't detach from DSA - it's owned by pmetrics and will be
	 * cleaned up by pmetrics' cleanup handler.
	 */
	local_dsa = NULL;

	elog(DEBUG1, "pmetrics_stmts: backend %d cleaned up", MyProcPid);
}

/*
 * Get queries table for this backend.
 * Reuses pmetrics' DSA and attaches to our queries table.
 */
static dshash_table *get_queries_table(void)
{
	MemoryContext oldcontext;

	/* Already attached in this backend? */
	if (local_queries_table != NULL)
		return local_queries_table;

	/* Ensure shared state exists and was initialized */
	if (stmts_shared_state == NULL)
		elog(ERROR, "pmetrics_stmts shared state not initialized");

	if (!stmts_shared_state->initialized)
		elog(ERROR, "pmetrics_stmts not properly initialized during startup");

	/*
	 * Switch to TopMemoryContext to ensure the dshash_table structure
	 * persists for the backend's lifetime.
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * Reuse pmetrics' DSA instead of attaching separately.
	 * This avoids the "can't attach the same segment more than once" error.
	 */
	local_dsa = pmetrics_get_dsa();
	if (local_dsa == NULL)
		elog(ERROR, "pmetrics_stmts: could not get DSA from pmetrics");

	/* Attach to queries table */
	local_queries_table = dshash_attach(
	    local_dsa, &queries_params, stmts_shared_state->queries_handle, NULL);

	MemoryContextSwitchTo(oldcontext);

	elog(DEBUG1, "pmetrics_stmts: backend %d attached to queries table",
	     MyProcPid);

	/* Register cleanup callback for when backend exits */
	on_shmem_exit(cleanup_pmetrics_stmts_backend, 0);

	return local_queries_table;
}

PG_FUNCTION_INFO_V1(list_queries);
Datum list_queries(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	QueryTextEntry **queries;
	int current_idx;

	if (SRF_IS_FIRSTCALL()) {
		MemoryContext oldcontext;
		TupleDesc tupdesc;
		dshash_table *table;
		dshash_seq_status status;
		QueryTextEntry *query;
		int capacity = 16;
		int count = 0;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
			        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			         errmsg("function returning record called in context "
			                "that cannot accept type record")));

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		table = get_queries_table();

		/*
		 * Materialize all queries in the first call.
		 * We can't use dshash_seq_next() across SRF calls because it holds
		 * partition locks that must be released between iterations.
		 */
		queries =
		    (QueryTextEntry **)palloc(capacity * sizeof(QueryTextEntry *));

		dshash_seq_init(&status, table, false);
		while ((query = (QueryTextEntry *)dshash_seq_next(&status)) != NULL) {
			if (count >= capacity) {
				capacity *= 2;
				queries = (QueryTextEntry **)repalloc(
				    queries, capacity * sizeof(QueryTextEntry *));
			}

			queries[count] = (QueryTextEntry *)palloc(sizeof(QueryTextEntry));
			memcpy(queries[count], query, sizeof(QueryTextEntry));
			count++;
		}
		dshash_seq_term(&status);

		funcctx->user_fctx = queries;
		funcctx->max_calls = count;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	queries = (QueryTextEntry **)funcctx->user_fctx;
	current_idx = funcctx->call_cntr;

	if (current_idx < funcctx->max_calls) {
		QueryTextEntry *query = queries[current_idx];
		Datum values[2];
		bool nulls[2] = {false, false};
		HeapTuple tuple;
		Datum result;

		values[0] = Int64GetDatum(query->key.queryid);
		values[1] = CStringGetTextDatum(query->query_text);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	} else {
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * Helper function to build JSONB labels for query tracking.
 * Returns a JSONB object with queryid, userid, and dbid.
 */
static Jsonb *build_query_labels(uint64 queryid, Oid userid, Oid dbid)
{
	JsonbParseState *state = NULL;
	JsonbValue key, val;

	pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

	key.type = jbvString;
	key.val.string.val = "queryid";
	key.val.string.len = strlen("queryid");
	pushJsonbValue(&state, WJB_KEY, &key);

	val.type = jbvNumeric;
	val.val.numeric = DatumGetNumeric(
	    DirectFunctionCall1(int8_numeric, Int64GetDatum(queryid)));
	pushJsonbValue(&state, WJB_VALUE, &val);

	key.val.string.val = "userid";
	key.val.string.len = strlen("userid");
	pushJsonbValue(&state, WJB_KEY, &key);

	val.val.numeric = DatumGetNumeric(
	    DirectFunctionCall1(int4_numeric, Int32GetDatum(userid)));
	pushJsonbValue(&state, WJB_VALUE, &val);

	key.val.string.val = "dbid";
	key.val.string.len = strlen("dbid");
	pushJsonbValue(&state, WJB_KEY, &key);

	val.val.numeric =
	    DatumGetNumeric(DirectFunctionCall1(int4_numeric, Int32GetDatum(dbid)));
	pushJsonbValue(&state, WJB_VALUE, &val);

	return JsonbValueToJsonb(pushJsonbValue(&state, WJB_END_OBJECT, NULL));
}

/*
 * Planner hook: measure planning time and record to histogram.
 */
static PlannedStmt *pmetrics_stmts_planner_hook(Query *parse,
                                                const char *query_string,
                                                int cursorOptions,
                                                ParamListInfo boundParams)
{
	PlannedStmt *result;
	instr_time start_time, end_time;
	double elapsed_ms;
	Jsonb *labels_jsonb;
	char metric_name[NAMEDATALEN];

	/* Track metrics only if both pmetrics and track_times are enabled, and
	 * at top level */
	if (pmetrics_is_enabled() && pmetrics_stmts_track_times &&
	    nesting_level == 0 && query_string &&
	    parse->queryId != UINT64CONST(0)) {
		INSTR_TIME_SET_CURRENT(start_time);

		nesting_level++;
		PG_TRY();
		{
			if (prev_planner_hook)
				result = prev_planner_hook(parse, query_string, cursorOptions,
				                           boundParams);
			else
				result = standard_planner(parse, query_string, cursorOptions,
				                          boundParams);
		}
		PG_FINALLY();
		{
			nesting_level--;
		}
		PG_END_TRY();

		INSTR_TIME_SET_CURRENT(end_time);
		INSTR_TIME_SUBTRACT(end_time, start_time);

		elapsed_ms = INSTR_TIME_GET_MILLISEC(end_time);

		labels_jsonb =
		    build_query_labels(parse->queryId, GetUserId(), MyDatabaseId);

		snprintf(metric_name, NAMEDATALEN, "query_planning_time_ms");
		pmetrics_record_to_histogram(metric_name, labels_jsonb, elapsed_ms);
	} else {
		nesting_level++;
		PG_TRY();
		{
			if (prev_planner_hook)
				result = prev_planner_hook(parse, query_string, cursorOptions,
				                           boundParams);
			else
				result = standard_planner(parse, query_string, cursorOptions,
				                          boundParams);
		}
		PG_FINALLY();
		{
			nesting_level--;
		}
		PG_END_TRY();
	}

	return result;
}

/*
 * ExecutorStart hook: set up instrumentation.
 */
static void pmetrics_stmts_ExecutorStart_hook(QueryDesc *queryDesc, int eflags)
{
	if (prev_ExecutorStart_hook)
		prev_ExecutorStart_hook(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	/* Allocate instrumentation if we're tracking any metrics and at top level
	 */
	if (pmetrics_is_enabled() &&
	    (pmetrics_stmts_track_times || pmetrics_stmts_track_rows ||
	     pmetrics_stmts_track_buffers) &&
	    nesting_level == 0 &&
	    queryDesc->plannedstmt->queryId != UINT64CONST(0)) {
		if (queryDesc->totaltime == NULL) {
			MemoryContext oldcxt;

			/* Allocate in query's memory context so it persists */
			oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
			queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL, false);
			MemoryContextSwitchTo(oldcxt);
		}

		/* Track query start timestamp */
		{
			uint64 queryid = queryDesc->plannedstmt->queryId;
			Jsonb *labels_jsonb =
			    build_query_labels(queryid, GetUserId(), MyDatabaseId);
			TimestampTz now = GetCurrentTimestamp();
			int64 timestamp_seconds = (int64)(timestamptz_to_time_t(now));
			char metric_name[NAMEDATALEN];

			snprintf(metric_name, NAMEDATALEN, "query_last_exec_timestamp");
			pmetrics_set_gauge(metric_name, labels_jsonb, timestamp_seconds);
		}
	}
}

/*
 * ExecutorEnd hook: collect execution metrics (time, row count, and optionally
 * buffer usage).
 */
static void pmetrics_stmts_ExecutorEnd_hook(QueryDesc *queryDesc)
{
	uint64 queryid = queryDesc->plannedstmt->queryId;
	Jsonb *labels_jsonb;
	char metric_name[NAMEDATALEN];
	double total_time_ms;
	uint64 rows_processed;

	if (queryid != UINT64CONST(0) && queryDesc->totaltime &&
	    pmetrics_is_enabled() &&
	    (pmetrics_stmts_track_times || pmetrics_stmts_track_rows ||
	     pmetrics_stmts_track_buffers) &&
	    nesting_level == 0) {
		/* Finalize timing - this must be called before reading totaltime */
		InstrEndLoop(queryDesc->totaltime);

		labels_jsonb = build_query_labels(queryid, GetUserId(), MyDatabaseId);

		/* Track execution time if enabled */
		if (pmetrics_stmts_track_times) {
			total_time_ms = queryDesc->totaltime->total * 1000.0;
			snprintf(metric_name, NAMEDATALEN, "query_execution_time_ms");
			pmetrics_record_to_histogram(metric_name, labels_jsonb,
			                             total_time_ms);
		}

		/* Track row count if enabled */
		if (pmetrics_stmts_track_rows) {
			rows_processed = queryDesc->estate->es_processed;
			snprintf(metric_name, NAMEDATALEN, "query_rows_returned");
			pmetrics_record_to_histogram(metric_name, labels_jsonb,
			                             (double)rows_processed);
		}

		/* Track buffer usage if enabled */
		if (pmetrics_stmts_track_buffers) {
			BufferUsage *bufusage = &queryDesc->totaltime->bufusage;

			snprintf(metric_name, NAMEDATALEN, "query_shared_blocks_hit");
			pmetrics_record_to_histogram(metric_name, labels_jsonb,
			                             (double)bufusage->shared_blks_hit);

			snprintf(metric_name, NAMEDATALEN, "query_shared_blocks_read");
			pmetrics_record_to_histogram(metric_name, labels_jsonb,
			                             (double)bufusage->shared_blks_read);
		}
	}

	if (prev_ExecutorEnd_hook)
		prev_ExecutorEnd_hook(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * Query normalization functions adapted from pg_stat_statements
 *
 * Copyright (c) 2008-2025, PostgreSQL Global Development Group
 *
 * These functions are adapted from
 * contrib/pg_stat_statements/pg_stat_statements.c to provide query
 * normalization (replacing constants with placeholders).
 */

/*
 * Comparator for sorting locations (qsort comparator)
 */
static int comp_location(const void *a, const void *b)
{
	int l = ((const LocationLen *)a)->location;
	int r = ((const LocationLen *)b)->location;

	if (l < r)
		return -1;
	else if (l > r)
		return 1;
	else
		return 0;
}

/*
 * Background worker main function for periodic cleanup
 */

__attribute__((visibility("default"))) void
pmetrics_stmts_cleanup_worker_main(Datum main_arg)
{
	/* Set up signal handlers */
	pqsignal(SIGTERM, pmetrics_stmts_sigterm_handler);
	BackgroundWorkerUnblockSignals();

	/* Connect to a database */
	BackgroundWorkerInitializeConnection("postgres", NULL, 0);

	/* DEBUG: Sleep to allow debugger attachment */
	elog(LOG, "pmetrics_stmts cleanup worker PID %d - sleeping 30 seconds for debugger attach", MyProcPid);
	pg_usleep(30000000L); /* 30 seconds */
	elog(LOG, "pmetrics_stmts cleanup worker PID %d - continuing", MyProcPid);

	while (!got_SIGTERM) {
		int rc;

		/* Wait for cleanup interval or until signaled */
		rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
		               CLEANUP_INTERVAL_MS, 0);
		ResetLatch(MyLatch);

		/* Emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		/* Process any signals received */
		if (got_SIGTERM)
			break;

		/* Perform cleanup */
		if (pmetrics_is_enabled()) {
			int64 cleaned_count = 0;

			StartTransactionCommand();
			PushActiveSnapshot(GetTransactionSnapshot());
			PG_TRY();
			{
				cleaned_count = pmetrics_stmts_cleanup_old_metrics(CLEANUP_MAX_AGE_SECONDS);
				PopActiveSnapshot();
				CommitTransactionCommand();

				elog(LOG, "pmetrics_stmts: cleaned up metrics for %ld queries", cleaned_count);
			}
			PG_CATCH();
			{
				PopActiveSnapshot();
				AbortCurrentTransaction();
				/* Log error but don't exit - we'll retry next iteration */
				EmitErrorReport();
				FlushErrorState();
			}
			PG_END_TRY();
		}
	}

	proc_exit(0);
}

/*
 * Clean up metrics for queries that haven't been executed in max_age_seconds
 * Returns the number of queries whose metrics were cleaned up.
 */
int64 pmetrics_stmts_cleanup_old_metrics(int64 max_age_seconds)
{
	TimestampTz cutoff_time = TimestampTzPlusMilliseconds(
	    GetCurrentTimestamp(), -max_age_seconds * 1000);
	int64 cutoff_seconds = (int64)(timestamptz_to_time_t(cutoff_time));
	char query_sql[512];
	int ret;
	uint64 queryid;
	Oid userid, dbid;
	int64 cleaned_queries = 0;

	/* Find queries with last execution timestamp older than max_age_seconds */
	snprintf(query_sql, sizeof(query_sql),
	         "SELECT (labels->>'queryid')::bigint as queryid, "
	         "(labels->>'userid')::oid as userid, "
	         "(labels->>'dbid')::oid as dbid "
	         "FROM pmetrics.list_metrics() "
	         "WHERE name = 'query_last_exec_timestamp' AND value < %ld",
	         cutoff_seconds);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	ret = SPI_execute(query_sql, true, 0);

	if (ret == SPI_OK_SELECT) {
		for (int i = 0; i < SPI_processed; i++) {
			HeapTuple tuple = SPI_tuptable->vals[i];
			bool isnull;

			queryid = DatumGetInt64(
			    SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isnull));
			if (isnull)
				continue;

			userid = DatumGetObjectId(
			    SPI_getbinval(tuple, SPI_tuptable->tupdesc, 2, &isnull));
			if (isnull)
				continue;

			dbid = DatumGetObjectId(
			    SPI_getbinval(tuple, SPI_tuptable->tupdesc, 3, &isnull));
			if (isnull)
				continue;

			/* Delete all metrics for this query */
			{
				Jsonb *labels_jsonb = build_query_labels(queryid, userid, dbid);
				const char *metric_names[] = {
				    "query_planning_time_ms",   "query_execution_time_ms",
				    "query_rows_returned",      "query_shared_blocks_hit",
				    "query_shared_blocks_read", "query_last_exec_timestamp"};
				int num_metrics =
				    sizeof(metric_names) / sizeof(metric_names[0]);

				for (int j = 0; j < num_metrics; j++) {
					pmetrics_delete_metric(metric_names[j], labels_jsonb);
				}

				/* Also delete the query text entry */
				{
					dshash_table *table = get_queries_table();
					QueryTextKey key;
					key.queryid = queryid;
					dshash_delete_key(table, &key);
				}
			}
		}
	}

	cleaned_queries = SPI_processed;

	SPI_finish();

	elog(DEBUG1, "pmetrics_stmts: cleaned up metrics for %ld old queries",
	     cleaned_queries);

	return cleaned_queries;
}

/*
 * SQL wrapper function for cleanup
 */
PG_FUNCTION_INFO_V1(cleanup_old_query_metrics);
Datum cleanup_old_query_metrics(PG_FUNCTION_ARGS)
{
	int64 max_age_seconds = PG_GETARG_INT64(0);
	int64 cleaned_count;

	if (!pmetrics_is_enabled())
		PG_RETURN_NULL();

	cleaned_count = pmetrics_stmts_cleanup_old_metrics(max_age_seconds);
	PG_RETURN_INT64(cleaned_count);
}

/*
 * Fill in constant lengths by scanning the query text.
 * Adapted from pg_stat_statements.
 */
static void fill_in_constant_lengths(JumbleState *jstate, const char *query,
                                     int query_loc)
{
	LocationLen *locs;
	core_yyscan_t yyscanner;
	core_yy_extra_type yyextra;
	core_YYSTYPE yylval;
	YYLTYPE yylloc;

	/* Sort the records by location */
	if (jstate->clocations_count > 1)
		qsort(jstate->clocations, jstate->clocations_count, sizeof(LocationLen),
		      comp_location);
	locs = jstate->clocations;

	/* Initialize the flex scanner */
	yyscanner = scanner_init(query, &yyextra, &ScanKeywords, ScanKeywordTokens);

	/* We don't want to re-emit any escape string warnings */
	yyextra.escape_string_warning = false;

	/* Search for each constant, in sequence */
	for (int i = 0; i < jstate->clocations_count; i++) {
		int loc;
		int tok;

		/* Ignore constants after the first one in the same location */
		if (i > 0 && locs[i].location == locs[i - 1].location) {
			locs[i].length = -1;
			continue;
		}

		/* Adjust recorded location if we're dealing with partial string */
		loc = locs[i].location - query_loc;
		Assert(loc >= 0);

		/* Lex tokens until we find the desired constant */
		for (;;) {
			tok = core_yylex(&yylval, &yylloc, yyscanner);

			/* We should not hit end-of-string, but if we do, behave sanely */
			if (tok == 0)
				break;

			/* If we run past it, work with that */
			if (yylloc >= loc) {
				if (query[loc] == '-') {
					/*
					 * It's a negative value - this is the one and only case
					 * where we replace more than a single token.
					 */
					tok = core_yylex(&yylval, &yylloc, yyscanner);
					if (tok == 0)
						break;
				}

				/*
				 * We now rely on the assumption that flex has placed a zero
				 * byte after the text of the current token in scanbuf.
				 */
				locs[i].length = strlen(yyextra.scanbuf + loc);
				break;
			}
		}

		/* If we hit end-of-string, give up, leaving remaining lengths -1 */
		if (tok == 0)
			break;
	}

	scanner_finish(yyscanner);
}

/*
 * Generate a normalized query string from a JumbleState.
 * Adapted from pg_stat_statements.
 */
static char *generate_normalized_query(JumbleState *jstate, const char *query,
                                       int query_loc, int *query_len_p)
{
	char *norm_query;
	int query_len = *query_len_p;
	int norm_query_buflen, /* Space allowed for norm_query */
	    len_to_wrt,        /* Length (in bytes) to write */
	    quer_loc = 0,      /* Source query byte location */
	    n_quer_loc = 0,    /* Normalized query byte location */
	    last_off = 0,      /* Offset from start for previous tok */
	    last_tok_len = 0;  /* Length (in bytes) of that tok */
	int num_constants_replaced = 0;

	/* Get constants' lengths (also ensures items are sorted by location) */
	fill_in_constant_lengths(jstate, query, query_loc);

	/*
	 * Allow for $n symbols to be longer than the constants they replace.
	 * Constants must take at least one byte in text form, while a $n symbol
	 * certainly isn't more than 11 bytes, even if n reaches INT_MAX.
	 */
	norm_query_buflen = query_len + jstate->clocations_count * 10;

	/* Allocate result buffer */
	norm_query = palloc(norm_query_buflen + 1);

	for (int i = 0; i < jstate->clocations_count; i++) {
		int off,     /* Offset from start for cur tok */
		    tok_len; /* Length (in bytes) of that tok */

		off = jstate->clocations[i].location;

		/* Adjust recorded location if we're dealing with partial string */
		off -= query_loc;

		tok_len = jstate->clocations[i].length;

		if (tok_len < 0)
			continue; /* ignore any duplicates */

		/* Copy next chunk (what precedes the next constant) */
		len_to_wrt = off - last_off;
		len_to_wrt -= last_tok_len;
		Assert(len_to_wrt >= 0);
		memcpy(norm_query + n_quer_loc, query + quer_loc, len_to_wrt);
		n_quer_loc += len_to_wrt;

		/*
		 * And insert a param symbol in place of the constant token.
		 */
		n_quer_loc += sprintf(norm_query + n_quer_loc, "$%d",
		                      num_constants_replaced + 1 +
		                          jstate->highest_extern_param_id);
		num_constants_replaced++;

		/* move forward */
		quer_loc = off + tok_len;
		last_off = off;
		last_tok_len = tok_len;
	}

	/*
	 * We've copied up until the last ignorable constant.  Copy over the
	 * remaining bytes of the original query string.
	 */
	len_to_wrt = query_len - quer_loc;

	Assert(len_to_wrt >= 0);
	memcpy(norm_query + n_quer_loc, query + quer_loc, len_to_wrt);
	n_quer_loc += len_to_wrt;

	Assert(n_quer_loc <= norm_query_buflen);
	norm_query[n_quer_loc] = '\0';

	*query_len_p = n_quer_loc;
	return norm_query;
}

/*
 * Post-parse-analyze hook: save normalized query text.
 */
static void pmetrics_stmts_post_parse_analyze(ParseState *pstate, Query *query,
                                              JumbleState *jstate)
{
	const char *query_text;
	int query_loc;
	int query_len;
	dshash_table *table;
	QueryTextKey key;
	QueryTextEntry *entry;
	bool found;

	/* Chain to previous hook if any */
	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query, jstate);

	/* Do nothing if no tracking is enabled or if we don't have valid data */
	if (!pmetrics_is_enabled() ||
	    (!pmetrics_stmts_track_times && !pmetrics_stmts_track_rows &&
	     !pmetrics_stmts_track_buffers))
		return;

	if (query->queryId == UINT64CONST(0) || pstate->p_sourcetext == NULL)
		return;

	/* Try to insert - only normalize if we actually need to insert */
	table = get_queries_table();
	if (table == NULL)
		return;

	key.queryid = query->queryId;
	entry = (QueryTextEntry *)dshash_find_or_insert(table, &key, &found);

	if (found) {
		/* Already have this query text, nothing to do */
		dshash_release_lock(table, entry);
		return;
	}

	/* New entry - we need to populate it with normalized query text */
	query_text = pstate->p_sourcetext;
	query_loc = query->stmt_location;
	query_len = query->stmt_len;

	/* Adjust for partial query strings */
	if (query_loc >= 0) {
		Assert(query_loc <= strlen(query_text));
		query_text += query_loc;

		if (query_len <= 0)
			query_len = strlen(query_text);
		else
			Assert(query_len <= strlen(query_text));
	} else {
		/* If query location is unknown, use entire string */
		query_loc = 0;
		query_len = strlen(query_text);
	}

	/* Generate normalized query if we have constant locations */
	if (jstate && jstate->clocations_count > 0) {
		char *norm_query;
		int norm_query_len = query_len;
		int text_len;

		norm_query = generate_normalized_query(jstate, pstate->p_sourcetext,
		                                       query_loc, &norm_query_len);

		/* Copy normalized query into the entry */
		text_len = norm_query_len;
		if (text_len >= MAX_QUERY_TEXT_LEN)
			text_len = MAX_QUERY_TEXT_LEN - 1;

		memcpy(entry->query_text, norm_query, text_len);
		entry->query_text[text_len] = '\0';
		entry->query_len = text_len;

		pfree(norm_query);
	} else {
		/* No constants to normalize, store original query */
		int text_len = query_len;
		if (text_len >= MAX_QUERY_TEXT_LEN)
			text_len = MAX_QUERY_TEXT_LEN - 1;

		memcpy(entry->query_text, query_text, text_len);
		entry->query_text[text_len] = '\0';
		entry->query_len = text_len;
	}

	dshash_release_lock(table, entry);
}

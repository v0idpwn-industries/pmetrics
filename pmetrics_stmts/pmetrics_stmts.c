/*
 * pmetrics_stmts - Query performance tracking for pmetrics
 *
 * This extension tracks query execution metrics (planning time, execution time,
 * rows returned) and stores them using the pmetrics metrics system.
 *
 * Requires pmetrics to be loaded first via shared_preload_libraries.
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
#include "optimizer/planner.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/guc.h"
#include "utils/jsonb.h"

#include <stdio.h>

PG_MODULE_MAGIC;

/* LWLock tranche ID for queries table */
#define LWTRANCHE_PMETRICS_QUERIES 1003

#define MAX_QUERY_TEXT_LEN 1024  /* Max query text length we store */

/* Shared state stored in static shared memory */
typedef struct PMetricsStmtsSharedState
{
	dsa_handle			pmetrics_dsa;		/* Reference to pmetrics DSA */
	dshash_table_handle	queries_handle;
	LWLock			   *init_lock;
	bool				initialized;
} PMetricsStmtsSharedState;

static PMetricsStmtsSharedState *stmts_shared_state = NULL;

/* Backend-local state (not in shared memory) */
static dsa_area *local_dsa = NULL;
static dshash_table *local_queries_table = NULL;
static bool backend_attached = false;

/* Hooks */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart_hook = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd_hook = NULL;

/* Nesting level for query hooks */
static int nesting_level = 0;

/* Configs */
#define DEFAULT_ENABLED true

static bool pmetrics_stmts_enabled = DEFAULT_ENABLED;

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
static void store_query_text(uint64 queryid, const char *query_text);
static PlannedStmt *pmetrics_stmts_planner_hook(Query *parse, const char *query_string,
												int cursorOptions, ParamListInfo boundParams);
static void pmetrics_stmts_ExecutorStart_hook(QueryDesc *queryDesc, int eflags);
static void pmetrics_stmts_ExecutorEnd_hook(QueryDesc *queryDesc);
static Jsonb *build_query_labels(uint64 queryid, Oid userid, Oid dbid);

/*
 * Hash function for QueryTextKey (dshash signature).
 * Simple hash of the uint64 queryid.
 */
static uint32
query_hash_dshash(const void *key, size_t key_size, void *arg)
{
	const QueryTextKey *k = (const QueryTextKey *) key;
	return hash_bytes((const unsigned char *) &k->queryid, sizeof(uint64));
}

/*
 * Compare function for QueryTextKey (dshash signature).
 */
static int
query_compare_dshash(const void *a, const void *b, size_t key_size, void *arg)
{
	const QueryTextKey *k1 = (const QueryTextKey *) a;
	const QueryTextKey *k2 = (const QueryTextKey *) b;

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
	.tranche_id = LWTRANCHE_PMETRICS_QUERIES
};

static void
pmetrics_stmts_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(MAXALIGN(sizeof(PMetricsStmtsSharedState)));
	RequestNamedLWLockTranche("pmetrics_stmts_init", 1);
}

static void
pmetrics_stmts_shmem_startup(void)
{
	bool		found;
	PMetricsSharedState *pmetrics_state;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	pmetrics_state = pmetrics_get_shared_state();
	if (pmetrics_state == NULL || !pmetrics_state->initialized)
		elog(ERROR, "pmetrics_stmts requires pmetrics to be loaded first in shared_preload_libraries");

	stmts_shared_state = ShmemInitStruct("pmetrics_stmts_shared_state",
										 sizeof(PMetricsStmtsSharedState),
										 &found);

	if (!found)
	{
		dsa_area *dsa;
		dshash_table *queries_table;

		/* Reuse pmetrics' DSA to avoid multiple DSA areas */
		stmts_shared_state->pmetrics_dsa = pmetrics_state->dsa;

		dsa = dsa_attach(stmts_shared_state->pmetrics_dsa);

		queries_table = dshash_create(dsa, &queries_params, NULL);
		stmts_shared_state->queries_handle = dshash_get_hash_table_handle(queries_table);

		stmts_shared_state->init_lock = &(GetNamedLWLockTranche("pmetrics_stmts_init")[0].lock);
		stmts_shared_state->initialized = true;

		/*
		 * Detach from postmaster so backends don't inherit the attachment state.
		 * pmetrics has already pinned the DSA.
		 */
		dshash_detach(queries_table);
		dsa_detach(dsa);

		elog(DEBUG1, "pmetrics_stmts: initialized with DSA handle %lu", (unsigned long)stmts_shared_state->pmetrics_dsa);
	}
}

void _PG_init(void) {
	PMetricsSharedState *pmetrics_state;

	pmetrics_state = pmetrics_get_shared_state();
	if (pmetrics_state == NULL)
		elog(WARNING, "pmetrics_stmts: pmetrics does not appear to be loaded");

	DefineCustomBoolVariable("pmetrics_stmts.enabled", "Enable query performance tracking",
							 NULL, &pmetrics_stmts_enabled, DEFAULT_ENABLED, PGC_SIGHUP,
							 0, NULL, NULL, NULL);

	MarkGUCPrefixReserved("pmetrics_stmts");

	LWLockRegisterTranche(LWTRANCHE_PMETRICS_QUERIES, "pmetrics_queries");

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pmetrics_stmts_shmem_startup;
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = pmetrics_stmts_shmem_request;

	prev_planner_hook = planner_hook;
	planner_hook = pmetrics_stmts_planner_hook;
	prev_ExecutorStart_hook = ExecutorStart_hook;
	ExecutorStart_hook = pmetrics_stmts_ExecutorStart_hook;
	prev_ExecutorEnd_hook = ExecutorEnd_hook;
	ExecutorEnd_hook = pmetrics_stmts_ExecutorEnd_hook;
}

/*
 * Cleanup callback when backend exits.
 * Detach from queries table only (pmetrics owns the DSA).
 */
static void
cleanup_pmetrics_stmts_backend(int code, Datum arg)
{
	if (local_queries_table != NULL)
	{
		dshash_detach(local_queries_table);
		local_queries_table = NULL;
	}

	/*
	 * Don't detach from DSA - it's owned by pmetrics and will be
	 * cleaned up by pmetrics' cleanup handler.
	 */
	local_dsa = NULL;

	backend_attached = false;

	elog(DEBUG1, "pmetrics_stmts: backend %d cleaned up", MyProcPid);
}

/*
 * Get queries table for this backend.
 * Reuses pmetrics' DSA and attaches to our queries table.
 */
static dshash_table *
get_queries_table(void)
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
	local_queries_table = dshash_attach(local_dsa,
										&queries_params,
										stmts_shared_state->queries_handle,
										NULL);

	MemoryContextSwitchTo(oldcontext);

	elog(DEBUG1, "pmetrics_stmts: backend %d attached to queries table", MyProcPid);

	/* Register cleanup callback for when backend exits */
	on_shmem_exit(cleanup_pmetrics_stmts_backend, 0);
	backend_attached = true;

	return local_queries_table;
}

/*
 * Store query text for a given queryid.
 * If the queryid already exists, we don't update it (first-write-wins).
 */
static void
store_query_text(uint64 queryid, const char *query_text)
{
	dshash_table *table;
	QueryTextKey key;
	QueryTextEntry *entry;
	bool found;
	int text_len;

	if (queryid == UINT64CONST(0) || query_text == NULL)
		return;

	table = get_queries_table();
	if (table == NULL)
		return;

	key.queryid = queryid;

	entry = (QueryTextEntry *) dshash_find(table, &key, false);
	if (entry != NULL)
	{
		/* First-write-wins: don't overwrite existing query text */
		dshash_release_lock(table, entry);
		return;
	}

	entry = (QueryTextEntry *) dshash_find_or_insert(table, &key, &found);

	if (!found)
	{
		text_len = strlen(query_text);
		if (text_len >= MAX_QUERY_TEXT_LEN)
			text_len = MAX_QUERY_TEXT_LEN - 1;

		memcpy(&entry->key, &key, sizeof(QueryTextKey));
		memcpy(entry->query_text, query_text, text_len);
		entry->query_text[text_len] = '\0';
		entry->query_len = text_len;
	}

	dshash_release_lock(table, entry);
}

PG_FUNCTION_INFO_V1(list_queries);
Datum
list_queries(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	QueryTextEntry **queries;
	int			current_idx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;
		dshash_table *table;
		dshash_seq_status status;
		QueryTextEntry *query;
		int			capacity = 16;
		int			count = 0;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("function returning record called in context "
								   "that cannot accept type record")));

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		table = get_queries_table();

		/*
		 * Materialize all queries in the first call.
		 * We can't use dshash_seq_next() across SRF calls because it holds
		 * partition locks that must be released between iterations.
		 */
		queries = (QueryTextEntry **) palloc(capacity * sizeof(QueryTextEntry *));

		dshash_seq_init(&status, table, false);
		while ((query = (QueryTextEntry *) dshash_seq_next(&status)) != NULL)
		{
			if (count >= capacity)
			{
				capacity *= 2;
				queries = (QueryTextEntry **) repalloc(queries, capacity * sizeof(QueryTextEntry *));
			}

			queries[count] = (QueryTextEntry *) palloc(sizeof(QueryTextEntry));
			memcpy(queries[count], query, sizeof(QueryTextEntry));
			count++;
		}
		dshash_seq_term(&status);

		funcctx->user_fctx = queries;
		funcctx->max_calls = count;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	queries = (QueryTextEntry **) funcctx->user_fctx;
	current_idx = funcctx->call_cntr;

	if (current_idx < funcctx->max_calls)
	{
		QueryTextEntry *query = queries[current_idx];
		Datum		values[2];
		bool		nulls[2] = {false, false};
		HeapTuple	tuple;
		Datum		result;

		values[0] = Int64GetDatum(query->key.queryid);
		values[1] = CStringGetTextDatum(query->query_text);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}
	else
	{
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * Helper function to build JSONB labels for query tracking.
 * Returns a JSONB object with queryid, userid, and dbid.
 */
static Jsonb *
build_query_labels(uint64 queryid, Oid userid, Oid dbid)
{
	JsonbParseState *state = NULL;
	JsonbValue key, val;

	pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

	key.type = jbvString;
	key.val.string.val = "queryid";
	key.val.string.len = strlen("queryid");
	pushJsonbValue(&state, WJB_KEY, &key);

	val.type = jbvNumeric;
	val.val.numeric = DatumGetNumeric(DirectFunctionCall1(int8_numeric, Int64GetDatum(queryid)));
	pushJsonbValue(&state, WJB_VALUE, &val);

	key.val.string.val = "userid";
	key.val.string.len = strlen("userid");
	pushJsonbValue(&state, WJB_KEY, &key);

	val.val.numeric = DatumGetNumeric(DirectFunctionCall1(int4_numeric, Int32GetDatum(userid)));
	pushJsonbValue(&state, WJB_VALUE, &val);

	key.val.string.val = "dbid";
	key.val.string.len = strlen("dbid");
	pushJsonbValue(&state, WJB_KEY, &key);

	val.val.numeric = DatumGetNumeric(DirectFunctionCall1(int4_numeric, Int32GetDatum(dbid)));
	pushJsonbValue(&state, WJB_VALUE, &val);

	return JsonbValueToJsonb(pushJsonbValue(&state, WJB_END_OBJECT, NULL));
}

/*
 * Planner hook: measure planning time and record to histogram.
 */
static PlannedStmt *
pmetrics_stmts_planner_hook(Query *parse, const char *query_string,
							int cursorOptions, ParamListInfo boundParams)
{
	PlannedStmt *result;
	instr_time start_time, end_time;
	double elapsed_ms;
	Jsonb *labels_jsonb;
	char metric_name[NAMEDATALEN];
	int bucket;

	/* Track metrics only if both pmetrics and pmetrics_stmts are enabled, and at top level */
	if (pmetrics_is_enabled() && pmetrics_stmts_enabled && nesting_level == 0 &&
		query_string && parse->queryId != UINT64CONST(0))
	{
		INSTR_TIME_SET_CURRENT(start_time);

		nesting_level++;
		PG_TRY();
		{
			if (prev_planner_hook)
				result = prev_planner_hook(parse, query_string, cursorOptions, boundParams);
			else
				result = standard_planner(parse, query_string, cursorOptions, boundParams);
		}
		PG_FINALLY();
		{
			nesting_level--;
		}
		PG_END_TRY();

		INSTR_TIME_SET_CURRENT(end_time);
		INSTR_TIME_SUBTRACT(end_time, start_time);

		elapsed_ms = INSTR_TIME_GET_MILLISEC(end_time);

		labels_jsonb = build_query_labels(parse->queryId, GetUserId(), MyDatabaseId);

		bucket = pmetrics_bucket_for(elapsed_ms);
		snprintf(metric_name, NAMEDATALEN, "query_planning_time_ms");
		pmetrics_increment_by(metric_name, labels_jsonb, METRIC_TYPE_HISTOGRAM, bucket, 1);

		store_query_text(parse->queryId, query_string);
	}
	else
	{
		nesting_level++;
		PG_TRY();
		{
			if (prev_planner_hook)
				result = prev_planner_hook(parse, query_string, cursorOptions, boundParams);
			else
				result = standard_planner(parse, query_string, cursorOptions, boundParams);
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
static void
pmetrics_stmts_ExecutorStart_hook(QueryDesc *queryDesc, int eflags)
{
	if (prev_ExecutorStart_hook)
		prev_ExecutorStart_hook(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	/* Allocate instrumentation if we're tracking queries and at top level */
	if (pmetrics_is_enabled() && pmetrics_stmts_enabled && nesting_level == 0 &&
		queryDesc->plannedstmt->queryId != UINT64CONST(0))
	{
		if (queryDesc->totaltime == NULL)
		{
			MemoryContext oldcxt;

			/* Allocate in query's memory context so it persists */
			oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
			queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL, false);
			MemoryContextSwitchTo(oldcxt);
		}
	}
}

/*
 * ExecutorEnd hook: collect execution metrics (time and row count).
 */
static void
pmetrics_stmts_ExecutorEnd_hook(QueryDesc *queryDesc)
{
	uint64 queryid = queryDesc->plannedstmt->queryId;
	Jsonb *labels_jsonb;
	char metric_name[NAMEDATALEN];
	double total_time_ms;
	uint64 rows_processed;
	int bucket;

	if (queryid != UINT64CONST(0) && queryDesc->totaltime &&
		pmetrics_is_enabled() && pmetrics_stmts_enabled && nesting_level == 0)
	{
		/* Finalize timing - this must be called before reading totaltime */
		InstrEndLoop(queryDesc->totaltime);

		total_time_ms = queryDesc->totaltime->total * 1000.0;
		rows_processed = queryDesc->estate->es_processed;

		labels_jsonb = build_query_labels(queryid, GetUserId(), MyDatabaseId);

		bucket = pmetrics_bucket_for(total_time_ms);
		snprintf(metric_name, NAMEDATALEN, "query_execution_time_ms");
		pmetrics_increment_by(metric_name, labels_jsonb, METRIC_TYPE_HISTOGRAM, bucket, 1);

		bucket = pmetrics_bucket_for((double) rows_processed);
		snprintf(metric_name, NAMEDATALEN, "query_rows_returned");
		pmetrics_increment_by(metric_name, labels_jsonb, METRIC_TYPE_HISTOGRAM, bucket, 1);

		if (queryDesc->sourceText)
			store_query_text(queryid, queryDesc->sourceText);
	}

	if (prev_ExecutorEnd_hook)
		prev_ExecutorEnd_hook(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

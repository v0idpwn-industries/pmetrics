/*
 * pmetrics - An instrumentation toolkit for PostgreSQL extensions
 *
 * This module provides simple metrics functionality with support
 * for counters, gauges and histograms, with labels.
 *
 * Metrics are stored in dynamic shared memory and the hash table grows
 * automatically as needed (no fixed limit).
 *
 * Each metric is uniquely identified by name, labels, type, and bucket.
 *
 * Accepts the following custom options:
 * - pmetrics.enabled: Enable metrics collection. Defaults to true.
 * - pmetrics.bucket_variability: Used to calculate the exponential buckets.
 *   Defaults to 0.1.
 * - pmetrics.buckets_upper_bound: the limit for the maximum histogram bucket.
 *   Defaults to 30000. Values over this will be truncated and fitted into the
 *   last bucket. A notice is raised whenever this happens.
 *
 * Labels are stored as JSONB for structured key-value data. Names are limited
 * to NAMEDATALEN.
 */

#include "postgres.h"
#include "pmetrics.h"

#include "common/hashfn.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/dshash.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/jsonb.h"

#include "math.h"
#include <stdio.h>

PG_MODULE_MAGIC;

/* LWLock tranche IDs (must not conflict with other extensions) */
#define LWTRANCHE_PMETRICS_DSA 43001
#define LWTRANCHE_PMETRICS 43002

/* GUC defaults */
#define DEFAULT_ENABLED true
#define DEFAULT_BUCKET_VARIABILITY 0.1
#define DEFAULT_BUCKETS_UPPER_BOUND 30000

/* Metric types */
typedef enum MetricType {
	METRIC_TYPE_COUNTER = 0,
	METRIC_TYPE_GAUGE = 1,
	METRIC_TYPE_HISTOGRAM = 2,
	METRIC_TYPE_HISTOGRAM_SUM = 3
} MetricType;

/* Shared state stored in static shared memory */
typedef struct PMetricsSharedState {
	dsa_handle dsa;
	dshash_table_handle metrics_handle;
	LWLock *init_lock;
	bool initialized;
} PMetricsSharedState;

typedef enum LabelsLocation {
	LABELS_NONE = 0,  /* No labels (empty JSONB or null) */
	LABELS_LOCAL = 1, /* labels.local_ptr is valid (search key) */
	LABELS_DSA = 2    /* labels.dsa_ptr is valid (stored key) */
} LabelsLocation;

typedef struct {
	char name[NAMEDATALEN];
	LabelsLocation labels_location;
	union {
		dsa_pointer dsa_ptr; /* When LABELS_DSA */
		Jsonb *local_ptr;    /* When LABELS_LOCAL */
	} labels;
	MetricType type;
	int bucket; /* Only used for histograms, 0 for counter/gauge */
} MetricKey;

typedef struct {
	MetricKey key;
	int64 value;
} Metric;

static PMetricsSharedState *shared_state = NULL;

/* Backend-local state (not in shared memory) */
static dsa_area *local_dsa = NULL;
static dshash_table *local_metrics_table = NULL;

/* Hooks */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;

/* Configs */

static bool pmetrics_enabled = DEFAULT_ENABLED;
static double bucket_variability = DEFAULT_BUCKET_VARIABILITY;
static int buckets_upper_bound = DEFAULT_BUCKETS_UPPER_BOUND;

static double gamma_val = 0;
static double log_gamma = 0;

/* Function declarations */
void _PG_init(void);
static void metrics_shmem_request(void);
static void metrics_shmem_startup(void);
static dshash_table *get_metrics_table(void);
static void cleanup_metrics_backend(int code, Datum arg);
static void validate_inputs(const char *name);
static void init_metric_key(MetricKey *key, const char *name,
                            Jsonb *labels_jsonb, MetricType type, int bucket);
static int bucket_for(double value);
static int64 increment_by(const char *name_str, Jsonb *labels_jsonb,
                          MetricType type, int bucket, int64 amount);
static int64 delete_metrics_by_name_labels(const char *name_str,
                                           Jsonb *labels_jsonb);
static void extract_metric_args(FunctionCallInfo fcinfo, int name_arg,
                                int labels_arg, char **name_out,
                                Jsonb **labels_out);
static Jsonb *get_labels_jsonb(const MetricKey *key, dsa_area *dsa);
static uint32 metric_hash_dshash(const void *key, size_t key_size, void *arg);
static int metric_compare_dshash(const void *a, const void *b, size_t key_size,
                                 void *arg);
static void metric_key_copy(void *dst, const void *src, size_t key_size,
                            void *arg);

/* dshash parameters (references function pointers declared above) */
static const dshash_parameters metrics_params = {
    .key_size = sizeof(MetricKey),
    .entry_size = sizeof(Metric),
    .compare_function = metric_compare_dshash,
    .hash_function = metric_hash_dshash,
    .copy_function = metric_key_copy,
    .tranche_id = LWTRANCHE_PMETRICS};

static void metrics_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(MAXALIGN(sizeof(PMetricsSharedState)));
	RequestNamedLWLockTranche("pmetrics_init", 1);
}

static void metrics_shmem_startup(void)
{
	bool found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	shared_state = ShmemInitStruct("pmetrics_shared_state",
	                               sizeof(PMetricsSharedState), &found);

	if (!found) {
		dsa_area *dsa;
		dshash_table *metrics_table;

		dsa = dsa_create(LWTRANCHE_PMETRICS_DSA);
		shared_state->dsa = dsa_get_handle(dsa);

		/*
		 * Pin the DSA to keep it alive even after we detach.
		 * This prevents it from being destroyed when postmaster detaches.
		 */
		dsa_pin(dsa);

		metrics_table = dshash_create(dsa, &metrics_params, NULL);
		shared_state->metrics_handle =
		    dshash_get_hash_table_handle(metrics_table);

		shared_state->init_lock =
		    &(GetNamedLWLockTranche("pmetrics_init")[0].lock);
		shared_state->initialized = true;

		/*
		 * Detach from postmaster so backends don't inherit the attachment
		 * state. The DSA is pinned so it won't be destroyed.
		 */
		dshash_detach(metrics_table);
		dsa_detach(dsa);

		elog(DEBUG1, "pmetrics: initialized with DSA handle %lu",
		     (unsigned long)shared_state->dsa);
	}
}

void _PG_init(void)
{
	int max_bucket_exp;

	/*
	 * Must be loaded via shared_preload_libraries since we allocate shared
	 * memory and register hooks. Fail if loaded any other way.
	 */
	if (!process_shared_preload_libraries_in_progress)
		ereport(
		    ERROR,
		    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
		     errmsg("pmetrics must be loaded via shared_preload_libraries")));

	DefineCustomBoolVariable(
	    "pmetrics.enabled", "Enable metrics collection",
	    "When disabled, all metric recording functions return NULL immediately",
	    &pmetrics_enabled, DEFAULT_ENABLED, PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomRealVariable("pmetrics.bucket_variability",
	                         "Bucket variability for histograms",
	                         "Controls histogram bucket spacing. Higher values "
	                         "create fewer, wider buckets. "
	                         "Used to calculate gamma = (1 + variability) / (1 "
	                         "- variability). Requires restart.",
	                         &bucket_variability, DEFAULT_BUCKET_VARIABILITY,
	                         0.01, 1.0, PGC_POSTMASTER, 0, NULL, NULL, NULL);

	DefineCustomIntVariable(
	    "pmetrics.buckets_upper_bound", "Maximum value for histogram buckets",
	    "Values larger than this will be placed in the highest bucket. "
	    "The actual upper bound will be rounded up to the nearest bucket "
	    "boundary. Requires restart.",
	    &buckets_upper_bound, DEFAULT_BUCKETS_UPPER_BOUND, 1, INT_MAX,
	    PGC_POSTMASTER, 0, NULL, NULL, NULL);

	gamma_val = (1 + bucket_variability) / (1 - bucket_variability);
	log_gamma = log(gamma_val);

	max_bucket_exp = ceil(log(buckets_upper_bound) / log_gamma);
	buckets_upper_bound = (int)pow(gamma_val, max_bucket_exp);

	MarkGUCPrefixReserved("pmetrics");

	LWLockRegisterTranche(LWTRANCHE_PMETRICS_DSA, "pmetrics_dsa");
	LWLockRegisterTranche(LWTRANCHE_PMETRICS, "pmetrics");

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = metrics_shmem_startup;
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = metrics_shmem_request;
}

static void validate_inputs(const char *name)
{
	if (name == NULL)
		elog(ERROR, "null input not allowed");

	if (strlen(name) >= NAMEDATALEN)
		elog(ERROR, "name too long");
}

/*
 * Extract name and labels from PG_FUNCTION_ARGS with proper error handling.
 * Validates inputs and allocates name_str which must be freed by caller.
 */
static void extract_metric_args(FunctionCallInfo fcinfo, int name_arg,
                                int labels_arg, char **name_out,
                                Jsonb **labels_out)
{
	text *name_text;

	name_text = PG_GETARG_TEXT_PP(name_arg);
	*labels_out = PG_GETARG_JSONB_P(labels_arg);
	*name_out = text_to_cstring(name_text);
	validate_inputs(*name_out);
}

/*
 * Initialize a MetricKey structure with local JSONB pointer
 */
static void init_metric_key(MetricKey *key, const char *name,
                            Jsonb *labels_jsonb, MetricType type, int bucket)
{
	strlcpy(key->name, name, NAMEDATALEN);

	if (labels_jsonb != NULL) {
		key->labels.local_ptr = labels_jsonb;
		key->labels_location = LABELS_LOCAL;
	} else {
		key->labels.local_ptr = NULL;
		key->labels_location = LABELS_NONE;
	}

	key->type = type;
	key->bucket = bucket;
}

/*
 * Cleanup callback when backend exits.
 * Detach from DSA and hash tables.
 */
static void cleanup_metrics_backend(int code, Datum arg)
{
	if (local_metrics_table != NULL) {
		dshash_detach(local_metrics_table);
		local_metrics_table = NULL;
	}

	if (local_dsa != NULL) {
		dsa_detach(local_dsa);
		local_dsa = NULL;
	}

	elog(DEBUG1, "pmetrics: backend %d cleaned up", MyProcPid);
}

/*
 * Get metrics table for this backend.
 * The DSA and hash table are created in postmaster during startup.
 * Each backend must attach to get its own valid pointers.
 */
static dshash_table *get_metrics_table(void)
{
	MemoryContext oldcontext;

	/* Already attached in this backend? */
	if (local_metrics_table != NULL)
		return local_metrics_table;

	/* Ensure shared state exists and was initialized */
	if (shared_state == NULL)
		elog(ERROR, "pmetrics shared state not initialized");

	if (!shared_state->initialized)
		elog(ERROR, "pmetrics not properly initialized during startup");

	/*
	 * Switch to TopMemoryContext to ensure the dshash_table structure
	 * persists for the backend's lifetime and doesn't get freed/reused
	 * by short-lived memory contexts.
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * Each backend must attach to the DSA to get valid pointers.
	 * The postmaster keeps the DSA alive, but each backend needs its own
	 * attachment.
	 */
	local_dsa = dsa_attach(shared_state->dsa);

	/*
	 * Pin the DSA mapping to keep it attached for the backend's lifetime.
	 * Without this, the resource owner will detach it at statement end,
	 * causing dangling pointers and crashes on subsequent calls.
	 */
	dsa_pin_mapping(local_dsa);

	local_metrics_table = dshash_attach(local_dsa, &metrics_params,
	                                    shared_state->metrics_handle, NULL);

	MemoryContextSwitchTo(oldcontext);

	elog(DEBUG1, "pmetrics: backend %d attached to tables", MyProcPid);

	/* Register cleanup callback for when backend exits */
	on_shmem_exit(cleanup_metrics_backend, 0);

	return local_metrics_table;
}

static int64 increment_by(const char *name_str, Jsonb *labels_jsonb,
                          MetricType type, int bucket, int64 amount)
{
	Metric *entry;
	MetricKey metric_key;
	bool found;
	dshash_table *table;
	int64 result;

	table = get_metrics_table();
	if (table == NULL)
		elog(ERROR, "pmetrics not initialized");

	init_metric_key(&metric_key, name_str, labels_jsonb, type, bucket);

	entry = (Metric *)dshash_find_or_insert(table, &metric_key, &found);

	if (!found)
		entry->value = 0;

	entry->value += amount;
	result = entry->value;

	dshash_release_lock(table, entry);

	return result;
}

PG_FUNCTION_INFO_V1(increment_counter);
Datum increment_counter(PG_FUNCTION_ARGS)
{
	Datum new_value;
	Jsonb *labels_jsonb;
	char *name_str = NULL;

	if (!pmetrics_enabled)
		PG_RETURN_NULL();

	PG_TRY();
	{
		extract_metric_args(fcinfo, 0, 1, &name_str, &labels_jsonb);
		new_value =
		    increment_by(name_str, labels_jsonb, METRIC_TYPE_COUNTER, 0, 1);
	}
	PG_CATCH();
	{
		if (name_str)
			pfree(name_str);
		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(name_str);
	return new_value;
}

PG_FUNCTION_INFO_V1(increment_counter_by);
Datum increment_counter_by(PG_FUNCTION_ARGS)
{
	Datum new_value;
	Jsonb *labels_jsonb;
	char *name_str = NULL;
	int increment;

	if (!pmetrics_enabled)
		PG_RETURN_NULL();

	PG_TRY();
	{
		extract_metric_args(fcinfo, 0, 1, &name_str, &labels_jsonb);
		increment = PG_GETARG_INT32(2);

		if (increment <= 0)
			elog(ERROR, "increment must be greater than 0");

		new_value = increment_by(name_str, labels_jsonb, METRIC_TYPE_COUNTER, 0,
		                         increment);
	}
	PG_CATCH();
	{
		if (name_str)
			pfree(name_str);
		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(name_str);
	return new_value;
}

PG_FUNCTION_INFO_V1(set_gauge);
Datum set_gauge(PG_FUNCTION_ARGS)
{
	Jsonb *labels_jsonb;
	char *name_str = NULL;
	int64 new_value;
	int64 result;

	if (!pmetrics_enabled)
		PG_RETURN_NULL();

	PG_TRY();
	{
		extract_metric_args(fcinfo, 0, 1, &name_str, &labels_jsonb);
		new_value = PG_GETARG_INT64(2);

		result = pmetrics_set_gauge(name_str, labels_jsonb, new_value);
	}
	PG_CATCH();
	{
		if (name_str)
			pfree(name_str);

		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(name_str);
	PG_RETURN_INT64(result);
}

PG_FUNCTION_INFO_V1(add_to_gauge);
Datum add_to_gauge(PG_FUNCTION_ARGS)
{
	Datum new_value;
	Jsonb *labels_jsonb;
	char *name_str = NULL;
	int increment;

	if (!pmetrics_enabled)
		PG_RETURN_NULL();

	PG_TRY();
	{
		extract_metric_args(fcinfo, 0, 1, &name_str, &labels_jsonb);
		increment = PG_GETARG_INT32(2);

		if (increment == 0)
			elog(ERROR, "value can't be 0");

		new_value = increment_by(name_str, labels_jsonb, METRIC_TYPE_GAUGE, 0,
		                         increment);
	}
	PG_CATCH();
	{
		if (name_str)
			pfree(name_str);

		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(name_str);
	return new_value;
}

PG_FUNCTION_INFO_V1(list_metrics);
Datum list_metrics(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Metric **metrics;
	int current_idx;

	if (SRF_IS_FIRSTCALL()) {
		MemoryContext oldcontext;
		TupleDesc tupdesc;
		dshash_table *table;
		dshash_seq_status status;
		Metric *metric;
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

		table = get_metrics_table();

		/*
		 * Materialize all metrics in the first call.
		 * We can't use dshash_seq_next() across SRF calls because it holds
		 * partition locks that must be released between iterations.
		 */
		metrics = (Metric **)palloc(capacity * sizeof(Metric *));

		/* Scan the entire hash table and copy all entries */
		dshash_seq_init(&status, table, false); /* false = shared lock */
		while ((metric = (Metric *)dshash_seq_next(&status)) != NULL) {
			Jsonb *labels_copy = NULL;

			/* Expand array if needed */
			if (count >= capacity) {
				capacity *= 2;
				metrics =
				    (Metric **)repalloc(metrics, capacity * sizeof(Metric *));
			}

			/* Copy the metric to backend-local memory */
			metrics[count] = (Metric *)palloc(sizeof(Metric));
			memcpy(metrics[count], metric, sizeof(Metric));

			/* Copy JSONB labels to backend-local memory if they exist in DSA */
			if (metric->key.labels_location == LABELS_DSA &&
			    metric->key.labels.dsa_ptr != InvalidDsaPointer) {
				Jsonb *dsa_labels = (Jsonb *)dsa_get_address(
				    local_dsa, metric->key.labels.dsa_ptr);
				size_t jsonb_size = VARSIZE(dsa_labels);

				labels_copy = (Jsonb *)palloc(jsonb_size);
				memcpy(labels_copy, dsa_labels, jsonb_size);

				/* Update the copied metric to point to local copy */
				metrics[count]->key.labels.local_ptr = labels_copy;
				metrics[count]->key.labels_location = LABELS_LOCAL;
			}

			count++;
		}
		dshash_seq_term(&status);

		/* Store the materialized metrics and count */
		funcctx->user_fctx = metrics;
		funcctx->max_calls = count;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	metrics = (Metric **)funcctx->user_fctx;
	current_idx = funcctx->call_cntr;

	if (current_idx < funcctx->max_calls) {
		Metric *metric = metrics[current_idx];
		Datum values[5];
		bool nulls[5] = {false, false, false, false, false};
		HeapTuple tuple;
		Datum result;
		const char *type_str;

		/* Convert metric type enum to string */
		switch (metric->key.type) {
		case METRIC_TYPE_COUNTER:
			type_str = "counter";
			break;
		case METRIC_TYPE_GAUGE:
			type_str = "gauge";
			break;
		case METRIC_TYPE_HISTOGRAM:
			type_str = "histogram";
			break;
		case METRIC_TYPE_HISTOGRAM_SUM:
			type_str = "histogram_sum";
			break;
		default:
			type_str = "unknown";
			break;
		}

		values[0] = CStringGetTextDatum(metric->key.name);

		if (metric->key.labels_location == LABELS_LOCAL &&
		    metric->key.labels.local_ptr != NULL) {
			values[1] = JsonbPGetDatum(metric->key.labels.local_ptr);
		} else {
			nulls[1] = true;
		}

		values[2] = CStringGetTextDatum(type_str);
		values[3] = Int32GetDatum(metric->key.bucket);
		values[4] = Int64GetDatum(metric->value);
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	} else {
		/* All metrics returned */
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * C API functions for other extensions to call
 * These are marked with visibility("default") to be externally accessible
 */

__attribute__((visibility("default"))) int64
pmetrics_increment_counter(const char *name_str, Jsonb *labels_jsonb)
{
	validate_inputs(name_str);
	return increment_by(name_str, labels_jsonb, METRIC_TYPE_COUNTER, 0, 1);
}

__attribute__((visibility("default"))) int64 pmetrics_increment_counter_by(
    const char *name_str, Jsonb *labels_jsonb, int64 amount)
{
	validate_inputs(name_str);

	if (amount <= 0)
		elog(ERROR, "increment must be greater than 0");

	return increment_by(name_str, labels_jsonb, METRIC_TYPE_COUNTER, 0, amount);
}

__attribute__((visibility("default"))) int64
pmetrics_set_gauge(const char *name_str, Jsonb *labels_jsonb, int64 value)
{
	Metric *entry;
	MetricKey metric_key;
	bool found;
	dshash_table *table;
	int64 result;

	validate_inputs(name_str);

	table = get_metrics_table();
	if (table == NULL)
		elog(ERROR, "pmetrics not initialized");

	init_metric_key(&metric_key, name_str, labels_jsonb, METRIC_TYPE_GAUGE, 0);

	entry = (Metric *)dshash_find_or_insert(table, &metric_key, &found);

	entry->value = value;
	result = entry->value;

	dshash_release_lock(table, entry);

	return result;
}

__attribute__((visibility("default"))) int64
pmetrics_add_to_gauge(const char *name_str, Jsonb *labels_jsonb, int64 amount)
{
	validate_inputs(name_str);

	if (amount == 0)
		elog(ERROR, "value can't be 0");

	return increment_by(name_str, labels_jsonb, METRIC_TYPE_GAUGE, 0, amount);
}

__attribute__((visibility("default"))) int64 pmetrics_record_to_histogram(
    const char *name_str, Jsonb *labels_jsonb, double value)
{
	Datum bucket_count;
	int bucket;

	validate_inputs(name_str);

	bucket = bucket_for(value);

	/* Increment the histogram bucket count */
	bucket_count =
	    increment_by(name_str, labels_jsonb, METRIC_TYPE_HISTOGRAM, bucket, 1);

	/* Add to histogram sum (bucket is always 0 for sum type) */
	increment_by(name_str, labels_jsonb, METRIC_TYPE_HISTOGRAM_SUM, 0,
	             (int64)value);

	return bucket_count;
}

PG_FUNCTION_INFO_V1(record_to_histogram);
Datum record_to_histogram(PG_FUNCTION_ARGS)
{
	int64 result;
	Jsonb *labels_jsonb;
	char *name_str = NULL;
	double value;

	if (!pmetrics_enabled)
		PG_RETURN_NULL();

	PG_TRY();
	{
		extract_metric_args(fcinfo, 0, 1, &name_str, &labels_jsonb);
		value = PG_GETARG_FLOAT8(2);
		result = pmetrics_record_to_histogram(name_str, labels_jsonb, value);
	}
	PG_CATCH();
	{
		if (name_str)
			pfree(name_str);

		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(name_str);
	PG_RETURN_INT64(result);
}

PG_FUNCTION_INFO_V1(list_histogram_buckets);
Datum list_histogram_buckets(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	int *buckets;
	int current_idx;

	if (SRF_IS_FIRSTCALL()) {
		MemoryContext oldcontext;
		TupleDesc tupdesc;
		int max_bucket_exp;
		int num_buckets;
		int i;
		int count;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
			        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			         errmsg("function returning record called in context "
			                "that cannot accept type record")));

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		max_bucket_exp = ceil(log(buckets_upper_bound) / log_gamma);
		num_buckets = max_bucket_exp + 1;

		buckets = (int *)palloc(num_buckets * sizeof(int));

		/* Generate unique bucket values */
		count = 0;
		buckets[count++] = 0;
		for (i = 1; i <= max_bucket_exp; i++) {
			int bucket_value = (int)pow(gamma_val, i);
			if (bucket_value != buckets[count - 1])
				buckets[count++] = bucket_value;
		}

		funcctx->user_fctx = buckets;
		funcctx->max_calls = count;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	buckets = (int *)funcctx->user_fctx;
	current_idx = funcctx->call_cntr;

	if (current_idx < funcctx->max_calls) {
		Datum values[1];
		bool nulls[1] = {false};
		HeapTuple tuple;
		Datum result;

		values[0] = Int32GetDatum(buckets[current_idx]);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	} else {
		SRF_RETURN_DONE(funcctx);
	}
}

__attribute__((visibility("default"))) int64 pmetrics_clear_metrics(void)
{
	dshash_table *metrics_table;
	dshash_seq_status status;
	Metric *entry;
	int64 deleted_count = 0;

	metrics_table = get_metrics_table();

	dshash_seq_init(&status, metrics_table, true);
	while ((entry = dshash_seq_next(&status)) != NULL) {
		if (entry->key.labels_location == LABELS_DSA) {
			dsa_free(local_dsa, entry->key.labels.dsa_ptr);
		}
		dshash_delete_current(&status);
		deleted_count++;
	}
	dshash_seq_term(&status);

	return deleted_count;
}

PG_FUNCTION_INFO_V1(clear_metrics);
Datum clear_metrics(PG_FUNCTION_ARGS)
{
	int64 deleted_count;

	deleted_count = pmetrics_clear_metrics();

	PG_RETURN_INT64(deleted_count);
}

static int64 delete_metrics_by_name_labels(const char *name_str,
                                           Jsonb *labels_jsonb)
{
	dshash_table *metrics_table;
	dshash_seq_status status;
	Metric *entry;
	int64 deleted_count = 0;
	Jsonb *entry_labels;

	metrics_table = get_metrics_table();
	if (metrics_table == NULL)
		elog(ERROR, "pmetrics not initialized");

	dshash_seq_init(&status, metrics_table, true);
	while ((entry = dshash_seq_next(&status)) != NULL) {
		/* Check if name matches */
		if (strcmp(entry->key.name, name_str) != 0)
			continue;

		/* Check if labels match */
		entry_labels = get_labels_jsonb(&entry->key, local_dsa);

		/* Compare labels - both NULL means match */
		if (labels_jsonb == NULL && entry_labels == NULL) {
			/* Both are NULL, they match */
		} else if (labels_jsonb != NULL && entry_labels != NULL) {
			/* Both exist, compare them */
			if (compareJsonbContainers(&labels_jsonb->root,
			                           &entry_labels->root) != 0)
				continue;
		} else {
			/* One is NULL, the other isn't - no match */
			continue;
		}

		/* Free DSA-allocated labels before deleting */
		if (entry->key.labels_location == LABELS_DSA) {
			dsa_free(local_dsa, entry->key.labels.dsa_ptr);
		}
		dshash_delete_current(&status);
		deleted_count++;
	}
	dshash_seq_term(&status);

	return deleted_count;
}

__attribute__((visibility("default"))) int64
pmetrics_delete_metric(const char *name_str, Jsonb *labels_jsonb)
{
	validate_inputs(name_str);
	return delete_metrics_by_name_labels(name_str, labels_jsonb);
}

PG_FUNCTION_INFO_V1(delete_metric);
Datum delete_metric(PG_FUNCTION_ARGS)
{
	int64 deleted_count;
	Jsonb *labels_jsonb;
	char *name_str = NULL;

	if (!pmetrics_enabled)
		PG_RETURN_NULL();

	PG_TRY();
	{
		extract_metric_args(fcinfo, 0, 1, &name_str, &labels_jsonb);
		deleted_count = delete_metrics_by_name_labels(name_str, labels_jsonb);
	}
	PG_CATCH();
	{
		if (name_str)
			pfree(name_str);
		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(name_str);
	PG_RETURN_INT64(deleted_count);
}

__attribute__((visibility("default"))) bool pmetrics_is_initialized(void)
{
	return shared_state != NULL && shared_state->initialized;
}

__attribute__((visibility("default"))) dsa_handle pmetrics_get_dsa_handle(void)
{
	if (shared_state == NULL || !shared_state->initialized)
		elog(ERROR, "pmetrics not initialized");

	return shared_state->dsa;
}

__attribute__((visibility("default"))) dsa_area *pmetrics_get_dsa(void)
{
	if (local_dsa == NULL)
		get_metrics_table();

	return local_dsa;
}

__attribute__((visibility("default"))) bool pmetrics_is_enabled(void)
{
	return pmetrics_enabled;
}

static int bucket_for(double value)
{
	int bucket;
	int this_bucket_upper_bound;

	if (value < 1.0)
		bucket = 0;
	else
		bucket = (int)fmax(ceil(log(value) / log_gamma), 0);

	this_bucket_upper_bound = (int)pow(gamma_val, bucket);

	if (this_bucket_upper_bound > buckets_upper_bound) {
		elog(NOTICE, "Histogram data truncated: value %f to %d", value,
		     buckets_upper_bound);
		this_bucket_upper_bound = buckets_upper_bound;
	}

	return this_bucket_upper_bound;
}

/*
 * Helper function to get JSONB from MetricKey, handling both local and DSA
 * locations.
 */
static Jsonb *get_labels_jsonb(const MetricKey *key, dsa_area *dsa)
{
	switch (key->labels_location) {
	case LABELS_LOCAL:
		return key->labels.local_ptr;
	case LABELS_DSA:
		if (key->labels.dsa_ptr != InvalidDsaPointer)
			return (Jsonb *)dsa_get_address(dsa, key->labels.dsa_ptr);
		return NULL;
	case LABELS_NONE:
	default:
		return NULL;
	}
}

/*
 * Custom hash function for MetricKey (dshash signature).
 * Handles both local (search) keys and DSA (stored) keys.
 */
static uint32 metric_hash_dshash(const void *key, size_t key_size, void *arg)
{
	const MetricKey *k = (const MetricKey *)key;
	uint32 hash;
	Jsonb *labels;

	hash = string_hash(k->name, NAMEDATALEN);
	hash ^= hash_bytes((const unsigned char *)&k->type, sizeof(MetricType));
	hash ^= hash_uint32((uint32)k->bucket);

	/* Hash JSONB labels if present */
	labels = get_labels_jsonb(k, local_dsa);
	if (labels != NULL)
		hash ^= hash_bytes((unsigned char *)labels, VARSIZE(labels));

	return hash;
}

/*
 * Custom compare function for MetricKey (dshash signature).
 * Handles both local (search) keys and DSA (stored) keys.
 * Returns <0, 0, or >0 like strcmp.
 */
static int metric_compare_dshash(const void *a, const void *b, size_t key_size,
                                 void *arg)
{
	const MetricKey *k1 = (const MetricKey *)a;
	const MetricKey *k2 = (const MetricKey *)b;
	Jsonb *labels1, *labels2;
	int cmp;

	/* Compare name */
	cmp = strcmp(k1->name, k2->name);
	if (cmp != 0)
		return cmp;

	/* Compare type */
	if (k1->type != k2->type)
		return (k1->type < k2->type) ? -1 : 1;

	/* Compare bucket */
	if (k1->bucket != k2->bucket)
		return (k1->bucket < k2->bucket) ? -1 : 1;

	/* Compare JSONB labels */
	labels1 = get_labels_jsonb(k1, local_dsa);
	labels2 = get_labels_jsonb(k2, local_dsa);

	if (labels1 == NULL && labels2 == NULL)
		return 0;
	if (labels1 == NULL)
		return -1;
	if (labels2 == NULL)
		return 1;

	/*
	 * Use memcmp instead of compareJsonbContainers to avoid collation lookup.
	 *
	 * compareJsonbContainers() calls varstr_cmp() which requires
	 * pg_newlocale_from_collation(), triggering syscache lookups that fail
	 * during early backend initialization when the system catalog cache is
	 * not yet available.
	 *
	 * Binary comparison is safe here because:
	 * - JSONB has a canonical binary format (sorted keys, no duplicates)
	 * - Identical JSON produces identical binary representations
	 * - We only need equality checking, not locale-aware sorting
	 */
	{
		Size size1 = VARSIZE(labels1);
		Size size2 = VARSIZE(labels2);

		if (size1 != size2)
			return (size1 < size2) ? -1 : 1;

		return memcmp(labels1, labels2, size1);
	}
}

/*
 * Custom copy function for MetricKey (dshash signature).
 * When inserting a new entry, allocates JSONB to DSA if source has local JSONB.
 */
static void metric_key_copy(void *dst, const void *src, size_t key_size,
                            void *arg)
{
	MetricKey *dest_key = (MetricKey *)dst;
	const MetricKey *src_key = (const MetricKey *)src;
	Jsonb *src_labels;
	Jsonb *dest_labels;
	Size jsonb_size;

	memcpy(dest_key, src_key, sizeof(MetricKey));

	if (src_key->labels_location == LABELS_LOCAL &&
	    src_key->labels.local_ptr != NULL) {
		src_labels = src_key->labels.local_ptr;
		jsonb_size = VARSIZE(src_labels);

		dest_key->labels.dsa_ptr = dsa_allocate(local_dsa, jsonb_size);
		if (dest_key->labels.dsa_ptr == InvalidDsaPointer)
			elog(ERROR, "out of dynamic shared memory for metric labels");

		dest_labels =
		    (Jsonb *)dsa_get_address(local_dsa, dest_key->labels.dsa_ptr);
		memcpy(dest_labels, src_labels, jsonb_size);

		dest_key->labels_location = LABELS_DSA;
	}
}

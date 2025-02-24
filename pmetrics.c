/*
 * pmetrics - An instrumentation toolkit for PostgreSQL extensions
 *
 * This module provides simple metrics functionality with support
 * for counters, gauges and histograms, with labels.
 *
 * Accepts the following custom options:
 * - pmetrics.max_metrics: Maximum number of metrics to store. Defaults to 1024.
 * - pmetrics.enabled: Enable metrics collection. Defaults to true.
 * - pmetrics.bucket_variability: Used to calculate the exponential buckets.
 *   Defaults to 0.1.
 * - pmetrics.buckets_upper_bound: the limit for the maximum histogram bucket.
 *   Defaults to 30000. Values over this will be truncated and fitted into the last
 *   bucket. A notice is raised whenever this happens.
 *
 * Labels are limited to 128 characters. Names are limited to NAMEDATALEN. Names
 * for histograms are limited further based in the `pmetrics.buckets_upper_bound`
 * config. For example, if the bucket upper bound is 5000, the name will be limited
 * to NAMEDATALEN - 6, as 4 characters are reserved for the bucket name, 1 for the
 * separator, and 1 for the null terminator.
 */

#include "postgres.h"

#include "common/hashfn.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"

#include "math.h"
#include <stdio.h>

PG_MODULE_MAGIC;

#define LABELS_LEN 128

/* Shmem */
static HTAB *metrics_tbl = NULL;
static LWLock *metrics_tbl_lock = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;

/* Configs */
#define DEFAULT_MAX_METRICS 1024
#define DEFAULT_ENABLED true
#define DEFAULT_BUCKET_VARIABILITY 0.1
#define DEFAULT_BUCKETS_UPPER_BOUND 30000

static int	max_metrics = DEFAULT_MAX_METRICS;
static bool pmetrics_enabled = DEFAULT_ENABLED;
static double bucket_variability = DEFAULT_BUCKET_VARIABILITY;
static int	buckets_upper_bound = DEFAULT_BUCKETS_UPPER_BOUND;

/* Gamma is set in init based on bucket_variability config value, computed on PG_init */
static double gamma_val = 0;
static double log_gamma = 0;

/* Max histogram name len is based on bucket upper bound, computed on PG_init */
static int	max_histogram_name_len = 0;

typedef struct
{
	char		name[NAMEDATALEN];
	char		labels[LABELS_LEN];
}			MetricKey;

typedef struct
{
	MetricKey	key;
	int64		value;
}			Metric;

typedef struct
{
	HASH_SEQ_STATUS table_scan_status;
}			ListMetricsIterState;

void		_PG_init(void);
static Size metric_memsize(void);
static void metrics_shmem_request(void);
uint32		metric_hash(const void *hashable, Size size);
Datum		increment_by(const char *name_str, const char *labels_str, int64 amount);
int			bucket_for(double value);

static Size
metric_memsize(void)
{
	Size		size = MAXALIGN(sizeof(Metric) * max_metrics);

	return size;
}

static int
metric_match(const void *key1, const void *key2, Size keysize)
{
	const		MetricKey *k1 = (const MetricKey *) key1;
	const		MetricKey *k2 = (const MetricKey *) key2;

	int			name_cmp = strcmp(k1->name, k2->name);
	int			labels_cmp = strcmp(k1->labels, k2->labels);

	return !(name_cmp == 0 && labels_cmp == 0);
}

static void *
metric_keycopy(void *dest, const void *src, Size keysize)
{
	MetricKey  *d = (MetricKey *) dest;
	const		MetricKey *s = (const MetricKey *) src;

	strlcpy(d->name, s->name, NAMEDATALEN);
	strlcpy(d->labels, s->labels, LABELS_LEN);
	return dest;
}

uint32
metric_hash(const void *key, Size size)
{
	const		MetricKey *k = (const MetricKey *) key;

	return (string_hash(k->name, NAMEDATALEN) >> 1) +
		(string_hash(k->labels, LABELS_LEN) >> 1);
}

static void
metrics_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	RequestNamedLWLockTranche("pmetrics_metrics", 1);
	RequestAddinShmemSpace(metric_memsize());
}

static void
metrics_shmem_startup(void)
{
	HASHCTL		info;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(MetricKey);
	info.entrysize = sizeof(Metric);
	info.hash = metric_hash;
	info.match = metric_match;
	info.keycopy = metric_keycopy;
	metrics_tbl = ShmemInitHash("pmetrics_metrics_table", max_metrics,
								max_metrics, &info, HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);
	metrics_tbl_lock = &(GetNamedLWLockTranche("pmetrics_metrics"))->lock;
}

void
_PG_init(void)
{
	int			max_bucket_exp;

	DefineCustomIntVariable("pmetrics.max_metrics",
							"Maximum number of metrics that can be stored",
							NULL,
							&max_metrics,
							DEFAULT_MAX_METRICS,
							1,
							INT_MAX,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("pmetrics.enabled",
							 "Metrics collection enabled",
							 NULL,
							 &pmetrics_enabled,
							 DEFAULT_ENABLED,
							 PGC_SIGHUP,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomRealVariable("pmetrics.bucket_variability",
							 "Bucket variability for histograms",
							 "Used to calculate bucket boundaries. Bigger = less buckets.",
							 &bucket_variability,
							 DEFAULT_BUCKET_VARIABILITY,
							 0.01,
							 1.0,
							 PGC_POSTMASTER,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("pmetrics.buckets_upper_bound",
							"Maximum value for histogram buckets",
							"Values bigger than this will be stored in the last bucket. The actual value will probably be bigger than the configuration value, as it goes to the nearest upper bucket",
							&buckets_upper_bound,
							DEFAULT_BUCKETS_UPPER_BOUND,
							1,
							INT_MAX,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	/* We must properly initialize log gamma and bucket upper bound */
	gamma_val = (1 + bucket_variability) / (1 - bucket_variability);
	log_gamma = log(gamma_val);

	max_bucket_exp = ceil(log(buckets_upper_bound) / log_gamma);
	buckets_upper_bound = (int) pow(gamma_val, max_bucket_exp);

	/* Digits = log10(n) + 1. Plus 1 for the underscore, plus 1 for terminator */
	max_histogram_name_len = NAMEDATALEN - ((int) log10(buckets_upper_bound) + 3);

	MarkGUCPrefixReserved("pmetrics");

	/* Set hooks  */
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = metrics_shmem_startup;
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = metrics_shmem_request;
}

static void
validate_inputs(const char *name, const char *labels)
{
	if (name == NULL || labels == NULL)
		elog(ERROR, "null input not allowed");

	if (strlen(name) >= NAMEDATALEN)
		elog(ERROR, "name too long");

	if (strlen(labels) >= LABELS_LEN)
		elog(ERROR, "labels too long");

}

Datum
increment_by(const char *name_str, const char *labels_str, int64 amount)
{
	Metric	   *entry;
	MetricKey	metric_key;
	bool		found;

	if (metrics_tbl == NULL)
		elog(ERROR, "pmetrics not initialized");

	strlcpy(metric_key.name, name_str, NAMEDATALEN);
	strlcpy(metric_key.labels, labels_str, LABELS_LEN);

	LWLockAcquire(metrics_tbl_lock, LW_EXCLUSIVE);
	entry =
		(Metric *) hash_search(metrics_tbl, &metric_key, HASH_ENTER, &found);

	if (!found)
	{
		memcpy(&entry->key, &metric_key, sizeof(MetricKey));
		entry->value = 0;
	}
	entry->value += amount;
	LWLockRelease(metrics_tbl_lock);
	PG_RETURN_INT64(entry->value);
}

PG_FUNCTION_INFO_V1(increment_counter);
Datum
increment_counter(PG_FUNCTION_ARGS)
{
	Datum		new_value;
	text	   *name_text,
			   *labels_text;
	char	   *name_str = NULL,
			   *labels_str = NULL;

	if (!pmetrics_enabled)
		PG_RETURN_NULL();

	name_text = PG_GETARG_TEXT_PP(0);
	labels_text = PG_GETARG_TEXT_PP(1);

	PG_TRY();
	{
		name_str = text_to_cstring(name_text);
		labels_str = text_to_cstring(labels_text);
		validate_inputs(name_str, labels_str);
		new_value = increment_by(name_str, labels_str, 1);
	}
	PG_CATCH();
	{
		if (name_str)
			pfree(name_str);
		if (labels_str)
			pfree(labels_str);
		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(name_str);
	pfree(labels_str);
	return new_value;
}

PG_FUNCTION_INFO_V1(increment_counter_by);
Datum
increment_counter_by(PG_FUNCTION_ARGS)
{
	Datum		new_value;
	text	   *name_text,
			   *labels_text;
	char	   *name_str = NULL,
			   *labels_str = NULL;
	int			increment;

	if (!pmetrics_enabled)
		PG_RETURN_NULL();

	PG_TRY();
	{
		name_text = PG_GETARG_TEXT_PP(0);
		labels_text = PG_GETARG_TEXT_PP(1);
		name_str = text_to_cstring(name_text);
		labels_str = text_to_cstring(labels_text);
		increment = PG_GETARG_INT32(2);

		validate_inputs(name_str, labels_str);

		if (increment <= 0)
			elog(ERROR, "increment must be greater than 0");

		new_value = increment_by(name_str, labels_str, increment);
	}
	PG_CATCH();
	{
		if (name_str)
			pfree(name_str);
		if (labels_str)
			pfree(labels_str);
		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(name_str);
	pfree(labels_str);
	return new_value;
}

PG_FUNCTION_INFO_V1(set_gauge);
Datum
set_gauge(PG_FUNCTION_ARGS)
{
	Metric	   *entry;
	MetricKey	metric_key;
	bool		found;
	text	   *name_text,
			   *labels_text;
	char	   *name_str = NULL,
			   *labels_str = NULL;
	int			new_value;

	if (!pmetrics_enabled)
		PG_RETURN_NULL();

	PG_TRY();
	{
		name_text = PG_GETARG_TEXT_PP(0);
		labels_text = PG_GETARG_TEXT_PP(1);
		name_str = text_to_cstring(name_text);
		labels_str = text_to_cstring(labels_text);
		new_value = PG_GETARG_INT32(2);

		if (metrics_tbl == NULL)
			elog(ERROR, "pmetrics not initialized");

		validate_inputs(name_str, labels_str);

		strlcpy(metric_key.name, name_str, NAMEDATALEN);
		strlcpy(metric_key.labels, labels_str, LABELS_LEN);

		LWLockAcquire(metrics_tbl_lock, LW_EXCLUSIVE);
		entry =
			(Metric *) hash_search(metrics_tbl, &metric_key, HASH_ENTER, &found);

		if (!found)
			memcpy(&entry->key, &metric_key, sizeof(MetricKey));

		entry->value = new_value;

		LWLockRelease(metrics_tbl_lock);
	}
	PG_CATCH();
	{
		if (name_str)
			pfree(name_str);
		if (labels_str)
			pfree(labels_str);
		if (LWLockHeldByMe(metrics_tbl_lock))
			LWLockRelease(metrics_tbl_lock);

		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(name_str);
	pfree(labels_str);
	PG_RETURN_INT64(entry->value);
}

PG_FUNCTION_INFO_V1(add_to_gauge);
Datum
add_to_gauge(PG_FUNCTION_ARGS)
{
	Datum		new_value;
	text	   *name_text,
			   *labels_text;
	char	   *name_str = NULL,
			   *labels_str = NULL;
	int			increment;

	if (!pmetrics_enabled)
		PG_RETURN_NULL();

	PG_TRY();
	{
		increment = PG_GETARG_INT32(2);
		name_text = PG_GETARG_TEXT_PP(0);
		labels_text = PG_GETARG_TEXT_PP(1);
		name_str = text_to_cstring(name_text);
		labels_str = text_to_cstring(labels_text);

		if (increment == 0)
			elog(ERROR, "value can't be 0");

		validate_inputs(name_str, labels_str);

		new_value = increment_by(name_str, labels_str, increment);
	}
	PG_CATCH();
	{
		if (name_str)
			pfree(name_str);

		if (labels_str)
			pfree(labels_str);

		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(name_str);
	pfree(labels_str);
	return new_value;
}

PG_FUNCTION_INFO_V1(list_metrics);

/* List all metrics
 * This acquires a full table lock tables for the seq scan
 */
Datum
list_metrics(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	ListMetricsIterState *iter_state;
	Metric	   *metric;

	/*
	 * First call prepares tuple desc, acquires an exclusive lock in the
	 * counters table lock and starts hash scan
	 */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Create tuple descriptor */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("function returning record called in context "
								   "that cannot accept type record")));

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		LWLockAcquire(metrics_tbl_lock, LW_EXCLUSIVE);
		iter_state = (ListMetricsIterState *) palloc(sizeof(ListMetricsIterState));
		hash_seq_init(&iter_state->table_scan_status, metrics_tbl);
		funcctx->user_fctx = iter_state;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	iter_state = (ListMetricsIterState *) funcctx->user_fctx;
	metric = (Metric *) hash_seq_search(&iter_state->table_scan_status);

	if (metric != NULL)
	{
		Datum		values[3];
		bool		nulls[3] = {false, false, false};
		HeapTuple	tuple;
		Datum		result;

		values[0] = CStringGetTextDatum(metric->key.name);
		values[1] = CStringGetTextDatum(metric->key.labels);
		values[2] = Int64GetDatum(metric->value);
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}
	else
	{
		/* Once table scan is finished, we release the lock and complete */
		LWLockRelease(metrics_tbl_lock);
		SRF_RETURN_DONE(funcctx);
	}
}

PG_FUNCTION_INFO_V1(record_to_histogram);
Datum
record_to_histogram(PG_FUNCTION_ARGS)
{
	Datum		new_value;
	char		name_str[NAMEDATALEN];
	int			bucket,
				name_len;
	text	   *name_text,
			   *labels_text;
	char	   *labels_str = NULL;
	double		value;

	if (!pmetrics_enabled)
		PG_RETURN_NULL();

	PG_TRY();
	{
		name_text = PG_GETARG_TEXT_PP(0);
		labels_text = PG_GETARG_TEXT_PP(1);
		value = PG_GETARG_FLOAT8(2);
		bucket = bucket_for(value);
		labels_str = text_to_cstring(labels_text);

		text_to_cstring_buffer(name_text, name_str, sizeof(name_str));

		name_len = strlen(name_str);

		if (name_len > max_histogram_name_len)
		{
			elog(ERROR, "metric name %s is too long for histograms, max is %d", name_str, max_histogram_name_len);
		}

		snprintf(name_str + name_len, sizeof(name_str) - name_len, "_%d", bucket);
		validate_inputs(name_str, labels_str);
		new_value = increment_by(name_str, labels_str, 1);
	}
	PG_CATCH();
	{
		if (labels_str)
			pfree(labels_str);

		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree(labels_str);
	return new_value;
}

/*
 * This is based on Rkallos' Peep exponential bucketing, which in turn is based
 * on DDSketch.
 *
 * Added the max_bucket config, which is used to add an artificial upper bound
 * to buckets.
 */
int
bucket_for(double value)
{
	int			bucket;
	int			this_bucket_upper_bound;

	if (value < 1.0)
		bucket = 0;
	else
		bucket = (int) fmax(ceil(log(value) / log_gamma), 0);

	this_bucket_upper_bound = (int) pow(gamma_val, bucket);

	if (this_bucket_upper_bound > buckets_upper_bound)
	{
		elog(NOTICE, "Histogram data truncated: value %f to %d", value, buckets_upper_bound);
		this_bucket_upper_bound = buckets_upper_bound;
	}

	return this_bucket_upper_bound;
}

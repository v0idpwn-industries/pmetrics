/*
 * pmetrics - An instrumentation toolkit for PostgreSQL extensions
 *
 * This module provides simple metrics functionality with support
 * for counters, gauges and histograms, with labels.
 *
 * Accepts two custom options:
 * - pmetrics.max_metrics: Maximum number of metrics to store. Defaults to 1024.
 * - pmetrics.enabled: Enable metrics collection. Defaults to true.
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

static int max_metrics = DEFAULT_MAX_METRICS;
static bool pmetrics_enabled = DEFAULT_ENABLED;

typedef struct {
  char name[NAMEDATALEN];
  char labels[LABELS_LEN];
} MetricKey;

typedef struct {
  MetricKey key;
  int64 value;
} Metric;

typedef struct {
  HASH_SEQ_STATUS table_scan_status;
} ListMetricsIterState;

void _PG_init(void);
static Size metric_memsize(void);
static void metrics_shmem_request(void);
uint32 metric_hash(const void *hashable, Size size);
Datum increment_by(const char* name_str, const char* labels_str, int64 amount);

static Size metric_memsize(void) {
  Size size = MAXALIGN(sizeof(Metric) * max_metrics);
  return size;
}

static int metric_match(const void *key1, const void *key2, Size keysize) {
    const MetricKey *k1 = (const MetricKey *)key1;
    const MetricKey *k2 = (const MetricKey *)key2;

    int name_cmp = strcmp(k1->name, k2->name);
    int labels_cmp = strcmp(k1->labels, k2->labels);

    return !(name_cmp == 0 && labels_cmp == 0);
}

static void *metric_keycopy(void *dest, const void *src, Size keysize) {
    MetricKey *d = (MetricKey *)dest;
    const MetricKey *s = (const MetricKey *)src;

    strlcpy(d->name, s->name, NAMEDATALEN);
    strlcpy(d->labels, s->labels, LABELS_LEN);
    return dest;
}

uint32 metric_hash(const void *key, Size size) {
  const MetricKey *k = (const MetricKey *)key;

  return (string_hash(k->name, NAMEDATALEN) >> 1) +
         (string_hash(k->labels, LABELS_LEN) >> 1);
}

static void metrics_shmem_request(void) {
  if (prev_shmem_request_hook)
    prev_shmem_request_hook();
  RequestNamedLWLockTranche("pmetrics_metrics", 1);
  RequestAddinShmemSpace(metric_memsize());
}

static void metrics_shmem_startup(void) {
  HASHCTL info;

  if (prev_shmem_startup_hook)
    prev_shmem_startup_hook();

  memset(&info, 0, sizeof(info));
  /* TODO: implement keycopy for better performance than memcpy */
  info.keysize = sizeof(MetricKey);
  info.entrysize = sizeof(Metric);
  info.hash = metric_hash;
  info.match = metric_match;
  info.keycopy = metric_keycopy;
  metrics_tbl = ShmemInitHash("pmetrics_metrics_table", max_metrics,
                               max_metrics, &info, HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);
  metrics_tbl_lock = &(GetNamedLWLockTranche("pmetrics_metrics"))->lock;
}

void _PG_init(void) {
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

  prev_shmem_startup_hook = shmem_startup_hook;
  shmem_startup_hook = metrics_shmem_startup;
  prev_shmem_request_hook = shmem_request_hook;
  shmem_request_hook = metrics_shmem_request;
}

static void validate_inputs(const char *name, const char *labels) {
  if (name == NULL || labels == NULL)
    elog(ERROR, "null input not allowed");

  if (strlen(name) >= NAMEDATALEN)
      elog(ERROR, "name too long");

  if (strlen(labels) >= LABELS_LEN)
    elog(ERROR, "labels too long");

}

Datum increment_by(const char* name_str, const char* labels_str, int64 amount){
    Metric *entry;
    MetricKey metric_key;
    bool found;

    strlcpy(metric_key.name, name_str, NAMEDATALEN);
    strlcpy(metric_key.labels, labels_str, LABELS_LEN);

    LWLockAcquire(metrics_tbl_lock, LW_EXCLUSIVE);
    entry =
        (Metric *)hash_search(metrics_tbl, &metric_key, HASH_ENTER, &found);

    if (!found) {
        memcpy(&entry->key, &metric_key, sizeof(MetricKey));
        entry->value = 0;
    }
    entry->value += amount;
    LWLockRelease(metrics_tbl_lock);
    PG_RETURN_INT64(entry->value);
}


PG_FUNCTION_INFO_V1(increment_counter);
Datum increment_counter(PG_FUNCTION_ARGS) {
  Datum new_value;

  if(!pmetrics_enabled){
    PG_RETURN_NULL();
  } else {
    text *name_text = PG_GETARG_TEXT_PP(0);
    text *labels_text = PG_GETARG_TEXT_PP(1);
    char *name_str = text_to_cstring(name_text);
    char *labels_str = text_to_cstring(labels_text);

    if (metrics_tbl == NULL)
        elog(ERROR, "pmetrics not initialized");

    validate_inputs(name_str, labels_str);
    new_value = increment_by(name_str, labels_str, 1);
    pfree(name_str);
    pfree(labels_str);
    return new_value;
  }
}

PG_FUNCTION_INFO_V1(increment_counter_by);
Datum increment_counter_by(PG_FUNCTION_ARGS) {
  Datum new_value;

  if(!pmetrics_enabled){
    PG_RETURN_NULL();
  } else {
    text *name_text = PG_GETARG_TEXT_PP(0);
    text *labels_text = PG_GETARG_TEXT_PP(1);
    char *name_str = text_to_cstring(name_text);
    char *labels_str = text_to_cstring(labels_text);
    int increment = PG_GETARG_INT32(2);

    if (metrics_tbl == NULL)
        elog(ERROR, "pmetrics not initialized");

    validate_inputs(name_str, labels_str);

    if(increment <= 0)
        elog(ERROR, "increment must be greater than 0");

    new_value = increment_by(name_str, labels_str, increment);
    pfree(name_str);
    pfree(labels_str);
    return new_value;
  }
}

PG_FUNCTION_INFO_V1(set_gauge);
Datum set_gauge(PG_FUNCTION_ARGS) {
  Metric *entry;
  MetricKey metric_key;
  bool found;

  if(!pmetrics_enabled){
    PG_RETURN_NULL();
  } else {
    text *name_text = PG_GETARG_TEXT_PP(0);
    text *labels_text = PG_GETARG_TEXT_PP(1);
    char *name_str = text_to_cstring(name_text);
    char *labels_str = text_to_cstring(labels_text);
    int new_value = PG_GETARG_INT32(2);

    if (metrics_tbl == NULL)
        elog(ERROR, "pmetrics not initialized");

    validate_inputs(name_str, labels_str);

    strlcpy(metric_key.name, name_str, NAMEDATALEN);
    strlcpy(metric_key.labels, labels_str, LABELS_LEN);

    LWLockAcquire(metrics_tbl_lock, LW_EXCLUSIVE);
    entry =
        (Metric *)hash_search(metrics_tbl, &metric_key, HASH_ENTER, &found);

    if (!found) {
        memcpy(&entry->key, &metric_key, sizeof(MetricKey));
    }

    entry->value = new_value;
    LWLockRelease(metrics_tbl_lock);

    pfree(name_str);
    pfree(labels_str);
    PG_RETURN_INT64(entry->value);
  }
}

PG_FUNCTION_INFO_V1(add_to_gauge);
Datum add_to_gauge(PG_FUNCTION_ARGS) {
  Datum new_value;

  if(!pmetrics_enabled){
    PG_RETURN_NULL();
  } else {
    text *name_text = PG_GETARG_TEXT_PP(0);
    text *labels_text = PG_GETARG_TEXT_PP(1);
    char *name_str = text_to_cstring(name_text);
    char *labels_str = text_to_cstring(labels_text);
    int increment = PG_GETARG_INT32(2);

    if (metrics_tbl == NULL)
        elog(ERROR, "pmetrics not initialized");

    validate_inputs(name_str, labels_str);

    if(increment == 0)
        elog(ERROR, "value can't be 0");

    new_value = increment_by(name_str, labels_str, increment);
    pfree(name_str);
    pfree(labels_str);
    return new_value;
  }
}

PG_FUNCTION_INFO_V1(list_metrics);

/* List all metrics
 * This acquires a full table lock tables for the seq scan
 */
Datum list_metrics(PG_FUNCTION_ARGS) {
  FuncCallContext *funcctx;
  ListMetricsIterState *iter_state;
  Metric *metric;

  /* First call prepares tuple desc, acquires an exclusive lock in the counters
   * table lock and starts hash scan */
  if (SRF_IS_FIRSTCALL()) {
    MemoryContext oldcontext;
    TupleDesc tupdesc;

    funcctx = SRF_FIRSTCALL_INIT();
    oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

    /* Create tuple descriptor */
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("function returning record called in context "
                             "that cannot accept type record")));

    funcctx->tuple_desc = BlessTupleDesc(tupdesc);

    LWLockAcquire(metrics_tbl_lock, LW_EXCLUSIVE);
    iter_state = (ListMetricsIterState *)palloc(sizeof(ListMetricsIterState));
    hash_seq_init(&iter_state->table_scan_status, metrics_tbl);
    funcctx->user_fctx = iter_state;
    MemoryContextSwitchTo(oldcontext);
  }

  funcctx = SRF_PERCALL_SETUP();
  iter_state = (ListMetricsIterState *)funcctx->user_fctx;
  metric = (Metric *)hash_seq_search(&iter_state->table_scan_status);

  if (metric != NULL) {
    Datum values[3];
    bool nulls[3] = {false, false, false};
    HeapTuple tuple;
    Datum result;

    values[0] = CStringGetTextDatum(metric->key.name);
    values[1] = CStringGetTextDatum(metric->key.labels);
    values[2] = Int64GetDatum(metric->value);
    tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
    result = HeapTupleGetDatum(tuple);
    SRF_RETURN_NEXT(funcctx, result);
  } else {
    /* Once table scan is finished, we release the lock and complete */
    LWLockRelease(metrics_tbl_lock);
    SRF_RETURN_DONE(funcctx);
  }
}

#include "postgres.h"

#include "funcapi.h"
#include "common/hashfn.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"

PG_MODULE_MAGIC;

#define MAX_COUNTERS 1024

static HTAB *counter_hash = NULL;
static LWLock *counter_table_lock = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;

typedef struct {
  char key[NAMEDATALEN];
  int64 count;
} Counter;

typedef struct {
    HASH_SEQ_STATUS counter_scan_status;
} ListMetricsIterState;

void _PG_init(void);
static Size counter_memsize(void);
static void counter_shmem_request(void);

static Size counter_memsize(void) {
  Size size = MAXALIGN(sizeof(Counter) * MAX_COUNTERS);
  return size;
}

static void counter_shmem_request(void) {
  if (prev_shmem_request_hook)
    prev_shmem_request_hook();
  RequestNamedLWLockTranche("pmetrics_counters", 1);
  RequestAddinShmemSpace(counter_memsize());
}

static void counter_shmem_startup(void) {
  HASHCTL info;

  if (prev_shmem_startup_hook)
    prev_shmem_startup_hook();

  // LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

  memset(&info, 0, sizeof(info));
  info.keysize = NAMEDATALEN;
  info.entrysize = sizeof(Counter);
  info.hash = string_hash;
  counter_hash = ShmemInitHash("pmetrics_counters_tbl", MAX_COUNTERS,
                               MAX_COUNTERS, &info, HASH_ELEM | HASH_FUNCTION);
  counter_table_lock = &(GetNamedLWLockTranche("pmetrics_counters"))->lock;
}

void _PG_init(void) {
  prev_shmem_startup_hook = shmem_startup_hook;
  shmem_startup_hook = counter_shmem_startup;
  prev_shmem_request_hook = shmem_request_hook;
  shmem_request_hook = counter_shmem_request;
}

PG_FUNCTION_INFO_V1(inc_counter);

Datum inc_counter(PG_FUNCTION_ARGS) {
  text *key_text = PG_GETARG_TEXT_PP(0);
  char *key_str = text_to_cstring(key_text);
  Counter *entry;
  bool found;

  if (counter_hash == NULL)
    elog(ERROR, "counter_hash not initialized");

  if (strlen(key_str) >= NAMEDATALEN)
    elog(ERROR, "key too long");

  LWLockAcquire(counter_table_lock, LW_EXCLUSIVE);
  entry = (Counter *)hash_search(counter_hash, key_str, HASH_ENTER, &found);

  if (!found) {
    strlcpy(entry->key, key_str, NAMEDATALEN);
    entry->count = 0;
  }

  entry->count++;
  LWLockRelease(counter_table_lock);

  pfree(key_str);
  PG_RETURN_INT64(entry->count);
}

PG_FUNCTION_INFO_V1(list_metrics);

Datum
list_metrics(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    ListMetricsIterState *iter_state;
    Counter *counter;

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext oldcontext;
        TupleDesc tupdesc;

        funcctx = SRF_FIRSTCALL_INIT();
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* Create tuple descriptor */
        if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("function returning record called in context "
                            "that cannot accept type record")));

        funcctx->tuple_desc = BlessTupleDesc(tupdesc);

        /* Initialize the hash table scan */
        iter_state = (ListMetricsIterState *) palloc(sizeof(ListMetricsIterState));
        hash_seq_init(&iter_state->counter_scan_status, counter_hash);

        funcctx->user_fctx = iter_state;

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    iter_state = (ListMetricsIterState *) funcctx->user_fctx;
    counter = (Counter *) hash_seq_search(&iter_state->counter_scan_status);

    if (counter != NULL) {
        Datum values[2];
        bool nulls[2] = {false, false};
        HeapTuple tuple;
        Datum result;

        values[0] = CStringGetTextDatum(counter->key);
        values[1] = Int64GetDatum(counter->count);
        tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        result = HeapTupleGetDatum(tuple);
        SRF_RETURN_NEXT(funcctx, result);
    } else {
        SRF_RETURN_DONE(funcctx);
    }
}

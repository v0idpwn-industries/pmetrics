/*
 * pmetrics_bench.c
 *
 * Benchmark extension for testing pmetrics under different workloads.
 */

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"

#include "extension/pmetrics/pmetrics.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(bench_metrics);
PG_FUNCTION_INFO_V1(bench_new_metrics);

/*
 * bench_metrics() -> BIGINT
 *
 * Increments 10 existing counters repeatedly (1M operations total).
 * Tests behavior when reusing a small set of metrics.
 * Returns the number of operations completed.
 */
Datum
bench_metrics(PG_FUNCTION_ARGS)
{
	int64 i;
	int counter_id;
	char metric_name[64];
	const int num_counters = 10;
	const int iterations_per_counter = 100000;
	const int64 total_ops = num_counters * iterations_per_counter;

	if (!pmetrics_is_initialized())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pmetrics is not initialized")));

	for (i = 0; i < total_ops; i++) {
		counter_id = i % num_counters;
		snprintf(metric_name, sizeof(metric_name), "bench_counter_%d", counter_id);
		pmetrics_increment_counter(metric_name, NULL);
	}

	PG_RETURN_INT64(total_ops);
}

/*
 * bench_new_metrics() -> BIGINT
 *
 * Creates 1M unique metrics, one per operation.
 * Tests behavior when creating new metrics constantly.
 * Includes backend PID in metric name to avoid collisions across backends.
 * Returns the number of operations completed.
 */
Datum
bench_new_metrics(PG_FUNCTION_ARGS)
{
	int64 i;
	char metric_name[64];
	const int64 total_ops = 1000000;
	int backend_pid = MyProcPid;

	if (!pmetrics_is_initialized())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pmetrics is not initialized")));

	for (i = 0; i < total_ops; i++) {
		snprintf(metric_name, sizeof(metric_name), "new_metric_%d_%lld", backend_pid, (long long)i);
		pmetrics_increment_counter(metric_name, NULL);
	}

	PG_RETURN_INT64(total_ops);
}

void
_PG_init(void)
{
	/* Nothing to initialize */
}

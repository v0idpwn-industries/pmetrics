/*
 * pmetrics.h - Public API for pmetrics extension
 *
 * This header defines the public interface for other PostgreSQL extensions
 * to interact with pmetrics metrics collection system.
 */

#ifndef PMETRICS_H
#define PMETRICS_H

#include "postgres.h"
#include "storage/lwlock.h"
#include "utils/jsonb.h"
#include "lib/dshash.h"
#include "utils/dsa.h"

/* LWLock tranche IDs (must not conflict with other extensions) */
#define LWTRANCHE_PMETRICS_DSA  1001
#define LWTRANCHE_PMETRICS      1002

/* Metric types */
typedef enum MetricType {
	METRIC_TYPE_COUNTER = 0,
	METRIC_TYPE_GAUGE = 1,
	METRIC_TYPE_HISTOGRAM = 2,
	METRIC_TYPE_HISTOGRAM_SUM = 3
} MetricType;

/* Shared state stored in static shared memory */
typedef struct PMetricsSharedState
{
	dsa_handle			dsa;
	dshash_table_handle	metrics_handle;
	LWLock			   *init_lock;
	bool				initialized;
} PMetricsSharedState;

/**
 * Get the shared state structure for accessing pmetrics from other extensions.
 * Returns NULL if pmetrics is not loaded or not initialized.
 */
extern PMetricsSharedState *pmetrics_get_shared_state(void);

/**
 * Get the DSA area pointer for sharing with other extensions.
 * Triggers backend attachment if not already done.
 * Do not call dsa_attach() separately if using this function.
 */
extern dsa_area *pmetrics_get_dsa(void);

/**
 * Core metric update function - increments or sets a metric value.
 *
 * This is the internal API used by all metric types (counters, gauges, histograms).
 * Creates the metric if it doesn't exist, otherwise updates the existing value.
 *
 * @param name_str Metric name
 * @param labels_jsonb JSONB labels (can be NULL)
 * @param type Metric type (COUNTER, GAUGE, HISTOGRAM, HISTOGRAM_SUM)
 * @param bucket Bucket value (for histograms only, 0 otherwise)
 * @param amount Amount to add to the metric value
 * @return New metric value after update
 */
extern Datum pmetrics_increment_by(const char *name_str, Jsonb *labels_jsonb,
								   MetricType type, int bucket, int64 amount);

/**
 * Calculate histogram bucket upper bound for a given value.
 *
 * Uses exponential bucketing based on DDSketch algorithm:
 *   gamma = (1 + variability) / (1 - variability)
 *   bucket = ceil(log(value) / log(gamma))
 *
 * Values exceeding buckets_upper_bound are clamped to the maximum bucket.
 * Configuration: pmetrics.bucket_variability, pmetrics.buckets_upper_bound
 */
extern int pmetrics_bucket_for(double value);

/**
 * Check if metrics collection is currently enabled.
 * Returns the value of pmetrics.enabled configuration parameter.
 */
extern bool pmetrics_is_enabled(void);

#endif /* PMETRICS_H */

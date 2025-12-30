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
#define LWTRANCHE_PMETRICS_DSA 1001
#define LWTRANCHE_PMETRICS 1002

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
 * Increment a counter by 1.
 *
 * @param name_str Metric name
 * @param labels_jsonb JSONB labels (can be NULL for empty object)
 * @return New counter value after increment
 */
extern int64 pmetrics_increment_counter(const char *name_str,
                                        Jsonb *labels_jsonb);

/**
 * Increment a counter by a specific amount.
 *
 * @param name_str Metric name
 * @param labels_jsonb JSONB labels (can be NULL for empty object)
 * @param amount Amount to increment (must be > 0)
 * @return New counter value after increment
 */
extern int64 pmetrics_increment_counter_by(const char *name_str,
                                           Jsonb *labels_jsonb, int64 amount);

/**
 * Set a gauge to a specific value.
 *
 * @param name_str Metric name
 * @param labels_jsonb JSONB labels (can be NULL for empty object)
 * @param value Value to set
 * @return The value that was set
 */
extern int64 pmetrics_set_gauge(const char *name_str, Jsonb *labels_jsonb,
                                int64 value);

/**
 * Add to a gauge (can be positive or negative).
 *
 * @param name_str Metric name
 * @param labels_jsonb JSONB labels (can be NULL for empty object)
 * @param amount Amount to add (can be negative; cannot be 0)
 * @return New gauge value after addition
 */
extern int64 pmetrics_add_to_gauge(const char *name_str, Jsonb *labels_jsonb,
                                   int64 amount);

/**
 * Record a value to a histogram.
 *
 * Creates both a histogram bucket entry and a histogram_sum entry.
 * This is the recommended way to record histogram values from C code.
 *
 * @param name_str Metric name
 * @param labels_jsonb JSONB labels (can be NULL for empty object)
 * @param value The value to record
 * @return Bucket count after recording
 */
extern Datum pmetrics_record_histogram(const char *name_str,
                                       Jsonb *labels_jsonb, double value);

/**
 * Check if metrics collection is currently enabled.
 * Returns the value of pmetrics.enabled configuration parameter.
 */
extern bool pmetrics_is_enabled(void);

#endif /* PMETRICS_H */

/**
 * @file pmetrics.h
 * @brief Public C API for pmetrics extension
 *
 * @mainpage pmetrics C API
 *
 * Metrics collection for PostgreSQL extensions. Provides counters, gauges, and
 * histograms with JSONB labels, stored in dynamic shared memory. The metrics
 * are stored in a dshash.
 *
 * Load pmetrics **before dependent extensions** in shared_preload_libraries.
 *
 * Dependent extensions should check pmetrics_is_initialized() in their
 * `shmem_startup` hook.
 *
 * Check `pmetrics_stmts` or the simpler `pmetrics_txn` for usage examples.
 *
 * ## Functions
 *
 * **Counters**: pmetrics_increment_counter(), pmetrics_increment_counter_by().
 * **Gauges**: pmetrics_set_gauge(), pmetrics_add_to_gauge().
 * **Histograms**: pmetrics_record_to_histogram().
 * **Utilities**: pmetrics_is_initialized(), pmetrics_is_enabled(),
 * pmetrics_get_dsa(), pmetrics_clear_metrics().
 */

#ifndef PMETRICS_H
#define PMETRICS_H

#include "postgres.h"
#include "utils/jsonb.h"
#include "utils/dsa.h"

/**
 * Check if pmetrics is properly initialized.
 * Returns true if pmetrics shared state is initialized and ready.
 */
extern bool pmetrics_is_initialized(void);

/**
 * Get the DSA handle for pmetrics' dynamic shared memory area.
 * This is useful for other extensions that need to store the handle
 * in their own shared state during startup.
 *
 * Raises ERROR if pmetrics is not initialized.
 */
extern dsa_handle pmetrics_get_dsa_handle(void);

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
extern int64 pmetrics_record_to_histogram(const char *name_str,
                                          Jsonb *labels_jsonb, double value);

/**
 * Clear all metrics from the metrics table.
 *
 * Deletes all metric entries and frees associated DSA memory for labels.
 * This is an administrative function typically used for testing or maintenance.
 *
 * @return Number of metrics deleted
 */
extern int64 pmetrics_clear_metrics(void);

/**
 * Delete all metrics with the specified name and labels.
 *
 * Deletes all metric types (counter, gauge, histogram buckets, histogram sum)
 * that match the given name and labels combination. Frees associated DSA memory
 * for labels.
 *
 * Note that this can be an expensive operation, as it needs to iterate through
 * all metrics.
 *
 * @param name_str Metric name to match
 * @param labels_jsonb JSONB labels to match (can be NULL for empty object)
 * @return Number of metrics deleted
 */
extern int64 pmetrics_delete_metric(const char *name_str, Jsonb *labels_jsonb);

/**
 * Check if metrics collection is currently enabled.
 * Returns the value of pmetrics.enabled configuration parameter.
 */
extern bool pmetrics_is_enabled(void);

#endif /* PMETRICS_H */

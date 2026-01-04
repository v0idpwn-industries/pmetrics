/*
 * pmetrics.h - Public API for pmetrics extension
 *
 * This header defines the public interface for other PostgreSQL extensions
 * to interact with pmetrics metrics collection system.
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
 * Check if metrics collection is currently enabled.
 * Returns the value of pmetrics.enabled configuration parameter.
 */
extern bool pmetrics_is_enabled(void);

#endif /* PMETRICS_H */

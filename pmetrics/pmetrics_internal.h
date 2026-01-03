/*
 * pmetrics_internal.h - Internal helper functions for pmetrics extension
 *
 * This header defines internal helper functions used within the pmetrics
 * extension. These are not part of the public API and should not be used
 * by other extensions.
 */

#ifndef PMETRICS_INTERNAL_H
#define PMETRICS_INTERNAL_H

#include "postgres.h"
#include "fmgr.h"
#include "lib/dshash.h"
#include "utils/dsa.h"
#include "utils/jsonb.h"

#include "pmetrics.h"

/* Labels location enumeration */
typedef enum LabelsLocation {
	LABELS_NONE = 0,  /* No labels (empty JSONB or null) */
	LABELS_LOCAL = 1, /* labels.local_ptr is valid (search key) */
	LABELS_DSA = 2    /* labels.dsa_ptr is valid (stored key) */
} LabelsLocation;

/* MetricKey structure - used as hash table key */
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

/* Metric structure - complete hash table entry */
typedef struct {
	MetricKey key;
	int64 value;
} Metric;

/* Backend-local state (not in shared memory) */
extern dsa_area *local_dsa;
extern dshash_table *local_metrics_table;

/* dshash parameters defined in pmetrics.c */
extern const dshash_parameters metrics_params;

/**
 * Get the metrics hash table for this backend.
 * Handles attachment to DSA and hash table on first call.
 *
 * @return The metrics hash table pointer
 */
extern dshash_table *get_metrics_table(void);

/**
 * Validate metric name inputs.
 * Checks for NULL and length constraints.
 *
 * @param name The metric name to validate
 */
extern void validate_inputs(const char *name);

/**
 * Extract metric name and labels from function arguments.
 *
 * @param fcinfo Function call info structure
 * @param name_arg Argument index for name
 * @param labels_arg Argument index for labels
 * @param name_out Output parameter for name (must be freed by caller)
 * @param labels_out Output parameter for labels JSONB
 */
extern void extract_metric_args(FunctionCallInfo fcinfo, int name_arg,
                                int labels_arg, char **name_out,
                                Jsonb **labels_out);

/**
 * Get JSONB labels from a MetricKey.
 * Handles both local and DSA-stored labels.
 *
 * @param key The metric key
 * @param dsa The DSA area pointer
 * @return JSONB pointer or NULL if no labels
 */
extern Jsonb *get_labels_jsonb(const MetricKey *key, dsa_area *dsa);

/**
 * Hash function for MetricKey (dshash signature).
 *
 * @param key Pointer to MetricKey
 * @param key_size Size of key structure
 * @param arg Optional argument (unused)
 * @return Hash value
 */
extern uint32 metric_hash_dshash(const void *key, size_t key_size, void *arg);

/**
 * Compare function for MetricKey (dshash signature).
 *
 * @param a First MetricKey pointer
 * @param b Second MetricKey pointer
 * @param key_size Size of key structure
 * @param arg Optional argument (unused)
 * @return <0, 0, or >0 like strcmp
 */
extern int metric_compare_dshash(const void *a, const void *b, size_t key_size,
                                 void *arg);

/**
 * Copy function for MetricKey (dshash signature).
 * Allocates JSONB to DSA when inserting new entries.
 *
 * @param dst Destination MetricKey pointer
 * @param src Source MetricKey pointer
 * @param key_size Size of key structure
 * @param arg Optional argument (unused)
 */
extern void metric_key_copy(void *dst, const void *src, size_t key_size,
                            void *arg);

/**
 * Initialize a MetricKey structure with local JSONB pointer.
 *
 * @param key The key to initialize
 * @param name Metric name
 * @param labels_jsonb JSONB labels (can be NULL)
 * @param type Metric type
 * @param bucket Bucket value (0 for non-histogram types)
 */
extern void init_metric_key(MetricKey *key, const char *name,
                            Jsonb *labels_jsonb, MetricType type, int bucket);

#endif /* PMETRICS_INTERNAL_H */

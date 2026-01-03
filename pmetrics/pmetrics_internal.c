/*
 * pmetrics_internal.c - Internal helper functions for pmetrics extension
 *
 * This module contains internal helper functions used within the pmetrics
 * extension. These are not part of the public API.
 */

#include "postgres.h"
#include "pmetrics_internal.h"

#include "common/hashfn.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include <string.h>

/* Backend-local state (not in shared memory) */
dsa_area *local_dsa = NULL;
dshash_table *local_metrics_table = NULL;

/* Backend attachment state */
static bool backend_attached = false;

/* Forward declarations for static functions */
static void cleanup_metrics_backend(int code, Datum arg);

/* dshash parameters (references function pointers defined in this file) */
const dshash_parameters metrics_params = {
    .key_size = sizeof(MetricKey),
    .entry_size = sizeof(Metric),
    .compare_function = metric_compare_dshash,
    .hash_function = metric_hash_dshash,
    .copy_function = metric_key_copy,
    .tranche_id = LWTRANCHE_PMETRICS};

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

	backend_attached = false;

	elog(DEBUG1, "pmetrics: backend %d cleaned up", MyProcPid);
}

/*
 * Get metrics table for this backend.
 * The DSA and hash table are created in postmaster during startup.
 * Each backend must attach to get its own valid pointers.
 */
dshash_table *get_metrics_table(void)
{
	MemoryContext oldcontext;
	PMetricsSharedState *shared_state;

	/* Already attached in this backend? */
	if (local_metrics_table != NULL)
		return local_metrics_table;

	/* Get shared state from public API */
	shared_state = pmetrics_get_shared_state();

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

	/* Attach to the metrics hash table using parameters from pmetrics.c */
	local_metrics_table = dshash_attach(local_dsa, &metrics_params,
	                                    shared_state->metrics_handle, NULL);

	MemoryContextSwitchTo(oldcontext);

	elog(DEBUG1, "pmetrics: backend %d attached to tables", MyProcPid);

	/* Register cleanup callback for when backend exits */
	on_shmem_exit(cleanup_metrics_backend, 0);
	backend_attached = true;

	return local_metrics_table;
}

void validate_inputs(const char *name)
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
void extract_metric_args(FunctionCallInfo fcinfo, int name_arg, int labels_arg,
                         char **name_out, Jsonb **labels_out)
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
void init_metric_key(MetricKey *key, const char *name, Jsonb *labels_jsonb,
                     MetricType type, int bucket)
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
 * Helper function to get JSONB from MetricKey, handling both local and DSA
 * locations.
 */
Jsonb *get_labels_jsonb(const MetricKey *key, dsa_area *dsa)
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
uint32 metric_hash_dshash(const void *key, size_t key_size, void *arg)
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
int metric_compare_dshash(const void *a, const void *b, size_t key_size,
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
void metric_key_copy(void *dst, const void *src, size_t key_size, void *arg)
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

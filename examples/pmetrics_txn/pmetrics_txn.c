/*
 * pmetrics_txn - Simple transaction tracking using pmetrics
 *
 * Tracks transaction commits and aborts using PostgreSQL's XactCallback hook.
 * Demonstrates basic pmetrics integration from another extension.
 */

#include "postgres.h"
#include "extension/pmetrics/pmetrics.h"
#include "access/xact.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

static Jsonb *empty_labels;

static void pmetrics_txn_callback(XactEvent event, void *arg)
{
	const char *metric_name;

	switch (event) {
	case XACT_EVENT_COMMIT:
		metric_name = "pg_transactions_commit";
		break;
	case XACT_EVENT_ABORT:
		metric_name = "pg_transactions_abort";
		break;
	default:
		return;
	}

	PG_TRY();
	{
		pmetrics_increment_counter(metric_name, empty_labels);
	}
	PG_CATCH();
	{
		FlushErrorState();
	}
	PG_END_TRY();
}

void _PG_init(void)
{
	JsonbParseState *state = NULL;
	MemoryContext old_context;

	old_context = MemoryContextSwitchTo(TopMemoryContext);
	pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
	empty_labels = JsonbValueToJsonb(pushJsonbValue(&state, WJB_END_OBJECT, NULL));
	MemoryContextSwitchTo(old_context);

	RegisterXactCallback(pmetrics_txn_callback, NULL);
}

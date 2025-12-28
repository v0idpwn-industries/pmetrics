MODULE_big = pmetrics_txn
OBJS = pmetrics_txn.o
EXTENSION = pmetrics_txn
DATA = pmetrics_txn--0.1.sql

# Use flat namespace to allow symbols to be resolved at runtime from pmetrics
# This is crucial for calling pmetrics functions (increment_counter, etc.)
# without linking against pmetrics at build time
SHLIB_LINK = -Wl,-flat_namespace -Wl,-undefined,dynamic_lookup

# Add debug symbols if DEBUG=1 is set
ifdef DEBUG
PG_CFLAGS = -g -O0
endif

# pmetrics.h should be installed by pmetrics extension
# PostgreSQL will find it automatically in $(includedir-server)

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

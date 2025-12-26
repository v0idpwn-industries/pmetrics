MODULE_big = pmetrics_stmts
OBJS = pmetrics_stmts.o
EXTENSION = pmetrics_stmts
DATA = pmetrics_stmts--0.1.sql

# Use flat namespace to allow symbols to be resolved at runtime from pmetrics
SHLIB_LINK = -Wl,-flat_namespace -Wl,-undefined,dynamic_lookup

# Add debug symbols if DEBUG=1 is set
ifdef DEBUG
PG_CFLAGS = -g -O0
endif

# pmetrics.h should be installed by pmetrics extension
# PostgreSQL will find it automatically in $(includedir-server)

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

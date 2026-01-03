MODULE_big = pmetrics
OBJS = pmetrics.o pmetrics_internal.o
EXTENSION = pmetrics
DATA = pmetrics--0.1.sql

# Install the header files
HEADERS = pmetrics.h pmetrics_internal.h

# Add debug symbols if DEBUG=1 is set
ifdef DEBUG
PG_CFLAGS = -g -O0
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

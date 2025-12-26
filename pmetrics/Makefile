MODULE_big = pmetrics
OBJS = pmetrics.o
EXTENSION = pmetrics
DATA = pmetrics--0.1.sql

# Install the header file
HEADERS = pmetrics.h

# Add debug symbols if DEBUG=1 is set
ifdef DEBUG
PG_CFLAGS = -g -O0
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

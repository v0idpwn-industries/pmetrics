MODULES = pmetrics
EXTENSION = pmetrics
DATA = pmetrics--0.1.sql

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
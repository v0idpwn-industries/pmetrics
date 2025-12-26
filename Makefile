# Top-level Makefile for pmetrics project
# Builds both pmetrics and pmetrics_stmts extensions in order

.PHONY: all clean install installcheck pmetrics pmetrics_stmts

# Build and install pmetrics first (for pmetrics.h), then pmetrics_stmts
all:
	$(MAKE) -C pmetrics all
	$(MAKE) -C pmetrics install
	$(MAKE) -C pmetrics_stmts all

pmetrics:
	$(MAKE) -C pmetrics all
	$(MAKE) -C pmetrics install

pmetrics_stmts: pmetrics
	$(MAKE) -C pmetrics_stmts all

install: all
	$(MAKE) -C pmetrics_stmts install

clean:
	$(MAKE) -C pmetrics clean
	$(MAKE) -C pmetrics_stmts clean

installcheck:
	$(MAKE) -C pmetrics installcheck
	$(MAKE) -C pmetrics_stmts installcheck
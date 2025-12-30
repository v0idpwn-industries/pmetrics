# Top-level Makefile for pmetrics project
# Builds both pmetrics and pmetrics_stmts extensions in order

.PHONY: all clean install installcheck pmetrics pmetrics_stmts format docs

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

format:
	find pmetrics pmetrics_stmts -name '*.c' -o -name '*.h' | xargs clang-format -i
	@test -d prometheus_exporter/venv || python3 -m venv prometheus_exporter/venv
	@prometheus_exporter/venv/bin/pip install -q -r prometheus_exporter/requirements.txt
	prometheus_exporter/venv/bin/python -m black demo/*.py prometheus_exporter/*.py
	cd test && mix format

docs:
	doxygen Doxyfile
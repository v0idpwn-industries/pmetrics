# Top-level Makefile for pmetrics project
# Builds both pmetrics and pmetrics_stmts extensions in order

.PHONY: clean installcheck pmetrics.all pmetrics.install pmetrics_stmts.all pmetrics_stmts.install format docs

pmetrics.all:
	$(MAKE) -C pmetrics all

pmetrics.install:
	$(MAKE) -C pmetrics install

pmetrics_stmts.all:
	$(MAKE) -C pmetrics_stmts all

pmetrics_stmts.install:
	$(MAKE) -C pmetrics_stmts install

clean:
	$(MAKE) -C pmetrics clean
	$(MAKE) -C pmetrics_stmts clean

installcheck:
	$(MAKE) -C pmetrics installcheck
	$(MAKE) -C pmetrics_stmts installcheck

format:
	find pmetrics pmetrics_stmts -name '*.c' -o -name '*.h' | xargs clang-format -i
	@test -d demo/venv || python3 -m venv demo/venv
	@demo/venv/bin/pip install -q black
	demo/venv/bin/python -m black demo/*.py
	cd test && mix format
	gofmt -w prometheus_exporter/*.go

docs:
	doxygen Doxyfile
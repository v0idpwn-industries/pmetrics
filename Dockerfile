FROM postgres:17

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    postgresql-server-dev-17 \
    && rm -rf /var/lib/apt/lists/*

# Set PG_CONFIG for the build
ENV PG_CONFIG=/usr/bin/pg_config

# Copy pmetrics extension source
COPY pmetrics/ /tmp/pmetrics/
WORKDIR /tmp/pmetrics
RUN make clean && make && make install

# Copy pmetrics_stmts extension source
COPY pmetrics_stmts/ /tmp/pmetrics_stmts/
WORKDIR /tmp/pmetrics_stmts
RUN make clean && make && make install

# Clean up build artifacts
RUN apt-get purge -y build-essential postgresql-server-dev-17 \
    && apt-get autoremove -y \
    && rm -rf /tmp/pmetrics /tmp/pmetrics_stmts

WORKDIR /

# Use the default postgres entrypoint

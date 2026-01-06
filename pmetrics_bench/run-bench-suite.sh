#!/bin/bash
set -e

# First argument is the output filename (required)
# Second argument is the workload type: "reuse" or "create" (optional, defaults to "reuse")
if [ -z "$1" ]; then
    echo "Usage: $0 <output_file> [reuse|create]"
    echo "Example: $0 results_reuse.txt reuse"
    echo "Example: $0 results_create.txt create"
    exit 1
fi

OUTPUT_FILE="$1"
WORKLOAD_TYPE="${2:-reuse}"  # Default to "reuse" if not specified
DURATION=30
PROGRESS_INTERVAL=5

# Select the appropriate bench SQL file based on workload type
if [ "$WORKLOAD_TYPE" = "create" ]; then
    BENCH_SQL="bench_create.sql"
    CASE_DESCRIPTION="creating new metrics (bench_new_metrics)"
elif [ "$WORKLOAD_TYPE" = "reuse" ]; then
    BENCH_SQL="bench.sql"
    CASE_DESCRIPTION="reusing existing metrics (bench_metrics)"
else
    echo "Error: Workload type must be 'reuse' or 'create'"
    exit 1
fi

echo "=== pmetrics Benchmark Suite ===" | tee "$OUTPUT_FILE"
echo "Case: $CASE_DESCRIPTION" | tee -a "$OUTPUT_FILE"
echo "Date: $(date)" | tee -a "$OUTPUT_FILE"
echo "Duration: ${DURATION}s per test" | tee -a "$OUTPUT_FILE"
echo "Progress updates every ${PROGRESS_INTERVAL}s" | tee -a "$OUTPUT_FILE"
echo "" | tee -a "$OUTPUT_FILE"

for CLIENTS in 1 10; do
    echo "======================================" | tee -a "$OUTPUT_FILE"
    echo "Running with $CLIENTS client(s)..." | tee -a "$OUTPUT_FILE"
    echo "======================================" | tee -a "$OUTPUT_FILE"

    CLIENTS=$CLIENTS DURATION=$DURATION BENCH_SQL="$BENCH_SQL" ./run-benchmark.sh 2>&1 | tee -a "$OUTPUT_FILE"

    echo "" | tee -a "$OUTPUT_FILE"
    sleep 2
done

echo "======================================" | tee -a "$OUTPUT_FILE"
echo "Benchmark complete! Results saved to $OUTPUT_FILE" | tee -a "$OUTPUT_FILE"
echo "======================================" | tee -a "$OUTPUT_FILE"

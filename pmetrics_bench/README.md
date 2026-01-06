# pmetrics_bench

Benchmark extension for testing pmetrics under different workloads.

## Usage

The extension provides two benchmark functions:

### `bench_metrics()`

Reuses a small set of metrics:

- Increments 10 different counters
- 100k times each
- 1M total operations

### `bench_new_metrics()`

Creates new metrics constantly:

- Creates 1M unique metrics
- One new metric per operation
- Includes backend PID to avoid collisions

### Running the benchmark suite

Use the provided script to run benchmarks with different client counts:

```bash
cd pmetrics_bench
./run-bench-suite.sh output.txt reuse    # Run bench_metrics()
./run-bench-suite.sh output.txt create   # Run bench_new_metrics()
```

Or run a single benchmark manually:

```bash
CLIENTS=20 DURATION=30 ./run-benchmark.sh
```

## Benchmark Scenarios

### Reusing metrics (`bench_metrics`)

Multiple clients update the same 10 counters concurrently.

### Creating new metrics (`bench_new_metrics`)

Each client creates unique metrics (no overlap between clients).

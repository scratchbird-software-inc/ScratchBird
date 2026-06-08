# Performance Budget Gate

Search key: `DRIVER_SERVER_PERFORMANCE_BUDGET`.

## Purpose

Convert benchmark output into enforceable release criteria for the real public
route.

## Required Budgets

Each claimed benchmark profile must define:

- Route name and exact client-to-engine path.
- Authentication policy and user identity used by the benchmark.
- Dataset or fixture.
- Warmup rule.
- Measurement duration or iteration count.
- Latency budget.
- Throughput budget.
- TLS overhead budget when TLS is in route.
- Prepared statement budget.
- Bulk transfer budget.
- Memory allocation or resident-set budget where measurable.
- Current baseline evidence.
- Regression threshold and CTest label.

## Invalid Evidence

Benchmark evidence is invalid when it bypasses SBWP/TLS or admitted IPC,
listener, parser pool, parser, server, engine security, or MGA finality.

## Closure Rule

`DSR-043` closes only when real-route benchmark thresholds are explicit and CTest
fails on regressions instead of only recording descriptive results.

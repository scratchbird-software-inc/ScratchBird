# ScratchBird Benchmarks

`ScratchBird-Benchmarks` is a Docker-first benchmark harness for establishing
repeatable upstream baselines before ScratchBird itself is added as a target.

The project has two jobs:

1. Measure current upstream engine behavior under the same harness.
2. Prepare the exact comparison model ScratchBird will use later:
   - `ScratchBird native` vs upstream engines
   - `ScratchBird emulation mode` vs the original engine it emulates

This repository is not just a raw speed leaderboard. Its most important goal is
to answer questions like:

- Which access path did the engine choose?
- Did it stay on the expected index family or fall back to a scan?
- Is the plan comparable to the peer engine's plan?
- Is performance better, equivalent, or worse once the plan is normalized?

## What Is Benchmarked

### Current runtime targets

- FirebirdSQL
- MySQL
- PostgreSQL

These upstream engines are the current benchmarkable targets.

### Deferred targets

- `scratchbird-native`
- `scratchbird-firebird`
- `scratchbird-mysql`
- `scratchbird-postgresql`

Those ScratchBird targets are already declared in the target registry for the
normalized index-comparison lane, but they remain disabled until a benchmarkable
ScratchBird service exists.

## Comparison Model

The harness is organized around two comparison classes.

### 1. Native upstream baseline comparison

This compares upstream engines against each other using the same benchmark
harness and output format.

Use this to answer:

- how each engine behaves on the same stress or ACID lane
- which upstream engine is the best native baseline for a given feature area
- how stable the benchmark harness itself is

### 2. Normalized index-equivalence comparison

This compares feature-equivalent index behavior instead of comparing engines as
black boxes.

Current phase-1 scope is conservative:

- B-tree point lookup
- B-tree range scan
- B-tree composite predicate with ordered output

This is the lane that will later support:

- upstream engine vs ScratchBird emulation mode
- ScratchBird native vs upstream engines
- pairwise verdicts such as `better`, `equivalent`, and `fallback`

## Docker-First Runtime

The project is designed so users do not need full upstream source trees in
order to run the main benchmark lanes.

Required for normal use:

- Docker Engine or Docker Desktop
- Python 3
- benchmark dependencies from `requirements.txt`

Optional:

- local upstream source clones for regression-only lanes

Source clones are only needed for upstream regression suites. They are not
required for:

- `stress`
- `acid`
- `engine-differential`
- `index-comparison`

## Benchmark Suites

### Authoritative suites today

- `index-comparison`
  - Normalized plan and performance comparison by index family.
  - This is the most important future ScratchBird comparison lane.
- `stress`
  - Synthetic OLTP and mixed-workload pressure with joins, aggregations, bulk
    operations, and large result sets.
- `acid`
  - Atomicity, consistency, durability, and baseline isolation checks.
- `engine-differential`
  - Engine-biased scenario pack that highlights where each engine family tends
    to excel or diverge.
- `regression`
  - Optional upstream regression integration when local clones are available.

### Bounded Profile Suites

- `performance`
- `tpc-c`
- `tpc-h`

Those lanes execute bounded SQL profiles in this harness. They are valid for
regression detection and parser/driver/engine path coverage. They are not
publication-grade TPC or cross-engine performance claims unless the comparison
guard also proves repeatability, warm/cold separation, and variance policy.

## What Each Suite Measures

### `index-comparison`

Measures:

- execution status
- normalized plan family
- plan capture success
- expectation status
- average latency
- p95 latency
- p99 latency
- throughput in queries per second
- per-scenario quality score

Why:

- This suite is about plan correctness first and speed second.
- It tells you whether the engine chose the expected access path.
- It provides the pairwise comparison model ScratchBird will use later.

### `stress`

Measures:

- data-load row counts
- data-load duration
- data-load rows per second
- per-query duration
- rows returned
- rows affected
- pass/fail/error status

Why:

- This suite exposes workload stability, not just microbenchmark speed.
- It shows whether engines remain functional under large joins, aggregations,
  and bulk operations.
- It gives a practical mixed-workload baseline for later ScratchBird work.

### `acid`

Measures:

- test pass/fail/error/skip status
- expected vs actual verification result
- duration per test
- category rollups for atomicity, consistency, isolation, and durability

Why:

- This suite is a correctness gate.
- It ensures future performance work is not built on broken transactional
  behavior.

### `engine-differential`

Measures:

- scenario runtime
- execution success vs engine-specific error
- scenario-level behavior on engine-biased SQL patterns

Why:

- This suite highlights planner and engine-shape differences.
- It is useful for understanding why one engine behaves differently from
  another.
- It is informative, but it should not be treated as a strict correctness gate
  in the same way as `acid` or `index-comparison`.

### `regression`

Measures:

- upstream regression totals and result summaries

Why:

- This lane helps compare ScratchBird compatibility work against the original
  engine's own regression expectations.
- It requires local upstream source/test trees and is therefore optional.

## Verdict Model For `index-comparison`

Pairwise comparison is directional. A candidate target is compared against a
baseline target for the same normalized scenario.

- `better`
  - The candidate stayed on an equal-or-better normalized plan and improved
    performance outside the configured noise band.
- `equivalent`
  - The candidate matched the expected plan quality and stayed within the noise
    band.
- `worse`
  - The candidate ran but lost plan quality or performance versus baseline.
- `fallback`
  - The candidate fell back to a worse access strategy such as a scan when the
    expected indexed path should have been used.
- `unsupported`
  - The scenario or plan capture is not supported for that target.
- `invalid`
  - The result is unusable because the scenario did not produce a valid
    comparison artifact.

Execution status and comparative verdict are separate concepts. A run can
execute successfully and still receive a `worse` or `fallback` verdict.

See also:

- [index-comparison-tests/VERDICT_MODEL.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/index-comparison-tests/VERDICT_MODEL.md)

## Output Artifacts

For a matrix run under `results/matrix-<run-id>/`, the primary artifacts are:

- `matrix-summary.json`
  - overall run integrity and per-suite execution status
- `.matrix-runs.tsv`
  - one row per engine/suite invocation
- `matrix-comparison-unified.csv`
  - consolidated comparison table across engines and suites
- `<engine>/<suite>/*.json`
  - raw suite artifacts
- `comparison-<suite>/benchmark_comparison_*.txt`
  - human-readable suite comparison output
- `comparison-index-comparison/index-comparison-pairwise-*.json`
  - pairwise normalized verdict output for index-comparison

The unified CSV is the main decision artifact because it lets you compare:

- run health
- correctness counts
- suite durations
- suite-specific summary metrics
- raw artifact provenance

## Quick Start

```bash
cd project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness

python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

Run the Docker-first authoritative baseline:

```bash
./scripts/run-benchmark-matrix.sh \
  --engines=firebird,mysql,postgresql \
  --suites=stress,acid,engine-differential,index-comparison \
  --report --compare
```

Run a single engine and suite:

```bash
./scripts/start-engine.sh postgresql start
./scripts/run-benchmark.sh postgresql index-comparison --report
./scripts/start-engine.sh postgresql stop
```

Run regression only if local source trees are available:

```bash
./scripts/run-benchmark-matrix.sh \
  --engines=firebird,mysql,postgresql \
  --suites=regression \
  --report --compare
```

## Recommended Use Today

If you want decision-grade results right now, prioritize:

1. `acid`
2. `stress`
3. `index-comparison`
4. `engine-differential`

Use `performance`, `tpc-c`, and `tpc-h` only as work-in-progress lanes until
their scenario packs and reporting contracts are expanded.

## Why This Project Exists

This repository gives ScratchBird a stable baseline before ScratchBird enters
the matrix.

That matters because later comparisons should answer:

- Is ScratchBird correct against the original engine?
- Does ScratchBird choose the same or a better normalized plan?
- Does ScratchBird native behavior compete with the best relevant upstream
  engine?

Without this baseline, later ScratchBird results would be hard to interpret.

## Related Documentation

- [TEST_STRATEGY.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/TEST_STRATEGY.md)
- [docs/REPORTS_AND_CONSOLIDATED_CSV.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/docs/REPORTS_AND_CONSOLIDATED_CSV.md)
- [docs/OPERATIONS_RUNBOOK.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/docs/OPERATIONS_RUNBOOK.md)
- [index-comparison-tests/README.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/index-comparison-tests/README.md)
- [acid-tests/README.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/acid-tests/README.md)
- [stress-tests/README.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/stress-tests/README.md)
- [engine-differential-tests/README.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/engine-differential-tests/README.md)
- [engines/README.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/engines/README.md)

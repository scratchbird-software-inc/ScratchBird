# Index Comparison Tests

This suite measures index-to-index behavior using a normalized target model,
plan schema, and result schema.

Current required runtime scope:

- PostgreSQL
- MySQL
- Firebird

Current required operator path:

- Docker for the engine runtime
- Python with the benchmark repo virtualenv or equivalent dependencies

No upstream source clone is required for this suite.

## Comparison Classes

- Upstream baseline:
  - `upstream-postgresql`
  - `upstream-mysql`
  - `upstream-firebird`
- Deferred ScratchBird targets:
  - `scratchbird-postgresql`
  - `scratchbird-mysql`
  - `scratchbird-firebird`
  - `scratchbird-native`

ScratchBird targets are declared in the target registry but remain disabled
until a benchmarkable ScratchBird service exists.

## Current Scenario Pack

Phase 1 scenarios are intentionally conservative and focus on stable
feature-equivalent B-tree behavior:

- point lookup
- range scan
- composite predicate with ordered output

These scenarios establish the normalized target, plan, and reporting contract
that later families will reuse.

## Running The Suite

Single engine:

```bash
./scripts/start-engine.sh postgresql start
./scripts/run-benchmark.sh postgresql index-comparison --report
./scripts/start-engine.sh postgresql stop
```

Matrix:

```bash
./scripts/run-benchmark-matrix.sh \
  --engines=firebird,mysql,postgresql \
  --suites=index-comparison \
  --report --compare
```

## Result Artifacts

Per-engine suite output:

- `index-comparison-<target>-<timestamp>.json`

Matrix comparison output:

- generic text comparison via `system-info/submit/result_formatter.py`
- pairwise comparator artifacts in `comparison-index-comparison/`

The pairwise comparator produces directional verdicts using the benchmark
verdict model:

- `better`
- `equivalent`
- `worse`
- `fallback`
- `unsupported`
- `invalid`

Execution status is tracked separately from the comparative verdict.

See also:

- [VERDICT_MODEL.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/index-comparison-tests/VERDICT_MODEL.md)

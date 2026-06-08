# Benchmark Driver Readiness Policy

Search key: `DRIVER_BENCHMARK_READINESS_POLICY`

Benchmark execution must use current-repo driver and tool paths only.

Required output root:

```text
build/benchmarks/execution_plan10/
```

The Execution_Plan 10 runner must:

- use `project/drivers/driver/python/src` for the ScratchBird Python driver;
- use CLI tools built from `project/drivers/tool/cli`;
- write current ScratchBird JSON result bundles under `build/benchmarks/`;
- avoid the old external `ScratchBird-driver` repository;
- preserve result JSON, logs, environment, command line, and comparison report.

The comparison report remains under
`docs/reference/legacy_execution_plan_10_performance_parity/comparison/`, but current
benchmark execution artifacts must be generated under `build/`.

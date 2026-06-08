# Driver Execution_Plan Validation Result

Search key: `DRIVER_EXECUTION_PLAN_VALIDATION_RESULT_20260508`

Date: 2026-05-08

## Commands Run

Configure:

```bash
cmake -S project -B build/driver_gates -DSB_BUILD_DRIVER_GATES=ON
```

CTest:

```bash
ctest --test-dir build/driver_gates -L driver --output-on-failure
```

## Result

Final run:

- `39` CTest labels discovered under `-L driver`.
- `38` tests passed.
- `0` tests failed.
- `1` test was skipped by explicit toolchain waiver: `driver_mojo_gate`.
- Total real time: `226.51 sec`.

Primary evidence:

- `artifacts/logs/driver_gates_configure_native_runner.log`
- `artifacts/logs/driver_gates_ctest_native_runner_pass.log`

## Scope Proven By This Run

- Inventory, old-path hygiene, source-tree artifact isolation, CTest matrix,
  Execution_Plan 10 current-driver routing, release-claim policy, and final
  zero-drift audit passed.
- Native/offline component gates passed for C++, Dart, .NET, Elixir, Go, JDBC,
  Node, ODBC, Pascal, PHP, Python, R, Ruby, Rust, Swift, all imported adaptors,
  and the CLI tool.
- Component build/test outputs and dependency caches were routed under
  `build/drivers`.

## Explicit Exclusions

- Mojo is not validated on this host because the Mojo toolchain is absent or not
  discoverable by the runner. The skip is intentional under
  `SB_DRIVER_ALLOW_TOOLCHAIN_WAIVERS=ON`.
- JDBC and .NET live endpoint test classes are filtered from the offline
  component gates. They remain assigned to the pending server-fixture and live
  conformance slices.
- Packaging install smoke and Execution_Plan 10 benchmark execution are not proven by
  this run beyond current-path and policy readiness.

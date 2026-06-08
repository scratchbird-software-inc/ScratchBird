# Validation Plan

Search key: `DRIVER_ADAPTOR_TOOL_VALIDATION_PLAN`

## Validation Strategy

Validation is CTest-first. Language-native test commands may be used internally
only through CTest wrappers that set output, cache, dependency, package, and log
paths under `build/`.

## Required Validation Classes

| Class | Required evidence |
| --- | --- |
| Inventory | `driver_source_inventory_gate` proves every imported tree is known and classified. |
| Path hygiene | `driver_old_path_gate` proves no live build/test/runtime path references old external driver roots. |
| Artifact isolation | `driver_build_artifact_isolation_gate` and `driver_source_tree_write_guard` prove all generated output stays under `build/`. |
| Toolchain | `driver_toolchain_detection_gate` proves required toolchains are present or explicitly waived. |
| Dependency policy | `driver_dependency_policy_gate` proves package-manager caches and downloaded dependencies do not enter source trees. |
| Driver conformance | `driver_common_conformance_gate` proves connect/auth/execute/fetch/metadata/transactions/errors/types/reconnect/protocol behavior. |
| Server fixture | `driver_server_fixture_gate` proves current `sb_server` lifecycle is isolated and CTest-managed. |
| Per-driver | `driver_<name>_gate` proves each imported driver builds and tests in its build root. |
| Per-adaptor | `adaptor_<name>_gate` proves each imported adaptor builds and tests in its build root. |
| Tool | `tool_cli_gate` proves CLI tools build and test in their build root. |
| Benchmark | `driver_execution_plan10_runner_gate` proves the Execution_Plan 10 runner uses current repo driver/tool paths and writes comparable JSON under `build/benchmarks/`. |
| Packaging | `driver_packaging_install_gate` proves distributable artifacts build under `build/` and pass install/import smoke tests. |
| Release claims | `driver_release_claim_gate` proves docs do not claim unsupported driver/adaptor/tool support. |
| Final | `drivers_all` and `drivers_final_zero_drift_audit` prove aggregate closure. |

## Failure Inventory Rule

Full validation must collect all failing driver/adaptor/tool gates in one run.
Do not stop after the first failed ecosystem unless the user explicitly requests
targeted debugging.

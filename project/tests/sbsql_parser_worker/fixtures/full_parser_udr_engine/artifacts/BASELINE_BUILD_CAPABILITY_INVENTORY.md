# Baseline Build and Capability Inventory

Status: complete
Search key: `FSPE-BASELINE-BUILD-CAPABILITY-INVENTORY`
Generated: 2026-05-07 20:22:35 EDT

## Scope

Baseline captured from the existing `build/sbsql_parser_worker_validation` CMake tree after reconfigure. This is a focused pre-implementation baseline for parser worker, parser-support UDR, server/listener route, engine public ABI, and related tests; it is not a full clean release gate.

## Paths

| Item | Path |
| --- | --- |
| Source root | `project` |
| Build root | `build/sbsql_parser_worker_validation` |
| Baseline evidence root | `project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/baseline` |

## Toolchain

| Setting | Value |
| --- | --- |
| CMake build type | `Release` |
| C compiler | `/usr/bin/cc` |
| CXX compiler | `/usr/bin/c++` |
| Non-cluster engine profile | `release-complete` |
| Release-complete profile | `ON` |
| Degraded profile | `OFF` |

## Relevant CMake Feature Flags

| Flag | Value |
| --- | --- |
| `SB_BUILD_SBSQL_PARSER_WORKER` | `ON` |
| `SB_BUILD_SBSQL_PARSER_WORKER_TESTS` | `ON` |
| `SB_BUILD_SBU_SBSQL_PARSER_SUPPORT` | `ON` |
| `SB_BUILD_SERVER` | `ON` |
| `SB_BUILD_LISTENER` | `ON` |
| `SB_BUILD_TESTS` | `ON` |
| `SB_BUILD_SB_LISTENER` | `OFF` |
| `SB_BUILD_SERVER_EVENT_NOTIFICATIONS` | `OFF` |
| `SB_BUILD_ENGINE_INTERNAL_API` | `OFF` |
| `SB_BUILD_ENGINE_API_NONCLUSTER_BEHAVIOR_PROBE` | `OFF` |

## LLVM and Numeric Capability

| Capability | Value |
| --- | --- |
| LLVM library | `${PROJECT_ROOT}/project/tools/llvm/lib/libLLVM-23.so` |
| LLVM min major | `23` |
| LLVM project root | `${LLVM_SOURCE_ROOT}` |
| LLVM tools root | `${PROJECT_ROOT}/project/tools/llvm` |
| `llvm-config` | `not found under project/tools/llvm/bin` |
| int128 / uint128 | `compiler-supported assumption for GCC 13 C++23; no standalone probe run in P0B` |
| real128 / quadmath probe | `1` |

## Build Targets Observed

- Target help listed 70 targets. Full list is preserved in `artifacts/baseline/target-help.log`.
- `all (the default if no target is provided)`
- `clean`
- `depend`
- `edit_cache`
- `install`
- `install/local`
- `install/strip`
- `list_install_components`
- `rebuild_cache`
- `test`
- `sb_core_agents`
- `sb_core_bulk_load`
- `sb_core_catalog`
- `sb_core_datatypes`
- `sb_core_index`
- `sb_core_memory`
- `sb_core_metrics`
- `sb_core_platform`
- `sb_core_resources`
- `sb_core_uuid`
- `sb_domain_read_policy_probe`
- `sb_engine`
- `sb_engine_executor`
- `sb_engine_gpu_acceleration`
- `sb_engine_internal_api`
- `sb_engine_native_compile`
- `sb_engine_optimizer_contract_only`
- `sb_engine_plan_shapes`
- `sb_engine_public_abi_c_fixture`
- `sb_engine_public_abi_cpp_fixture`
- `sb_engine_public_abi_symbol_gate`
- `sb_engine_public_diagnostic_shape_fixture`
- `sb_engine_public_documentation_example_fixture`
- `sb_engine_public_metrics_thread_fixture`
- `sb_engine_public_sblr_admission_fixture`
- `sb_engine_public_source_boundary_fixture`
- `sb_engine_public_wire_stability_fixture`
- `sb_engine_shared`
- `sb_ipc_tester`
- `sb_listener`
- `sb_parser_dummy`
- `sb_server`
- `sb_server_core`
- `sb_server_event_notifications`
- `sb_storage_database`
- `sb_storage_disk`
- `sb_storage_filespace`
- `sb_storage_page`
- `sb_transaction_mga`
- `sbl_listener_control_plane`
- `sbl_listener_runtime`
- `sbl_manager_protocol`
- `sbl_numeric`
- `sbl_parser_server_ipc_schema`
- `sbl_sbsql_parser_pipeline`
- `sbl_sbsql_parser_worker_core`
- `sbp_probe`
- `sbp_sbsql`
- `sbp_sbsql_full_route_execution_smoke`
- `sbp_sbsql_ipc_schema_probe`
- Additional targets omitted here: 10; see `target-help.log`.

## Baseline Command Results

| Command | Status | Evidence | Log |
| --- | --- | --- | --- |
| configure | `passed` | configure/generate completed | `artifacts/baseline/configure.log` |
| build | `passed` | build completed; 0.35 seconds incremental rebuild | `artifacts/baseline/build.log`; `artifacts/baseline/build-timed.log` |
| ctest | `passed` | 100% tests passed; 0.22 seconds focused CTest run | `artifacts/baseline/ctest.log`; `artifacts/baseline/ctest-timed.log` |

## Environment Risks

- Filesystem state: `/dev/mapper/luks-f2eb1969-9169-42bd-803f-0cd6f77f6669  905G  797G   62G  93% /`.
- Build tree size: `68M	build/sbsql_parser_worker_validation`.
- Worktree state was dirty before P0B; snapshot preserved in `artifacts/baseline/git-status-short.log`.
- No stale socket/process cleanup was required for the focused baseline commands.

## Baseline Use

Later implementation validation must compare against this baseline and must not treat pre-existing dirty worktree state or existing generated build-tree state as a new regression without evidence.

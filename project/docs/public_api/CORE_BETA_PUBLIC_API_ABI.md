# Core Beta Public API and ABI Freeze

This source-level freeze document is paired with
`CORE_BETA_PUBLIC_API_ABI_MANIFEST.json`. The manifest is the machine-readable
regression input; this document is the human-readable API/ABI inventory.

- Manifest schema: `scratchbird.core_beta.public_api_abi_freeze.v1`
- Freeze ID: `core_beta_public_api_abi_freeze_2026_05`

## Invariants

- Engine execution authority is `engine_sblr_internal_api_only`.
- SQL text is never runtime authority inside the engine.
- UUID identity and descriptor/operand authority remain internal authority.
- MGA transaction inventory remains finality authority.
- Cluster-positive behavior is outside core and must route through a provider
  or fail closed in non-cluster builds.

## ABI Version

- Family: `sb_engine_public_abi`
- Version: `1.0.0`
- Packed value: `0x00010000`
- Source header: `project/include/scratchbird/engine/version.h`
- Runtime functions: `sb_engine_abi_version_packed`,
  `sb_engine_abi_build_id`

## Packaged Public Headers

These are the public headers installed from `project/include/scratchbird/engine`:

- `project/include/scratchbird/engine/descriptor.hpp`
- `project/include/scratchbird/engine/diagnostic.h`
- `project/include/scratchbird/engine/engine.h`
- `project/include/scratchbird/engine/engine.hpp`
- `project/include/scratchbird/engine/error.h`
- `project/include/scratchbird/engine/export.h`
- `project/include/scratchbird/engine/result.hpp`
- `project/include/scratchbird/engine/sblr/lowering.hpp`
- `project/include/scratchbird/engine/sblr/raising.hpp`
- `project/include/scratchbird/engine/sblr_envelope.hpp`
- `project/include/scratchbird/engine/session.hpp`
- `project/include/scratchbird/engine/transaction.hpp`
- `project/include/scratchbird/engine/types.hpp`
- `project/include/scratchbird/engine/value.hpp`
- `project/include/scratchbird/engine/version.h`

## C ABI Symbols

The frozen exported C ABI symbol set is:

- `sb_engine_abi_build_id`
- `sb_engine_abi_version_packed`
- `sb_engine_close`
- `sb_engine_describe_capabilities`
- `sb_engine_dispatch_sblr`
- `sb_engine_metric_root`
- `sb_engine_open`
- `sb_engine_result_class`
- `sb_engine_result_completion`
- `sb_engine_result_diagnostics`
- `sb_engine_result_next_batch`
- `sb_engine_result_payload`
- `sb_engine_result_release`
- `sb_engine_result_summary`
- `sb_engine_session_begin`
- `sb_engine_session_end`
- `sb_engine_status_name`
- `sb_engine_transaction_begin`
- `sb_engine_transaction_commit`
- `sb_engine_transaction_rollback`

## Public Surfaces

### `embedded_engine_c_abi_v1`

- Classification: core.
- Contract version: `sb_engine_public_abi_1_0_0`.
- Source paths:
  - `project/include/scratchbird/engine/engine.h`
  - `project/include/scratchbird/engine/diagnostic.h`
  - `project/include/scratchbird/engine/error.h`
  - `project/include/scratchbird/engine/version.h`
  - `project/src/engine/public_abi.cpp`
- Execution authority: `engine_sblr_internal_api_only`.

### `embedded_engine_cpp_wrappers_v1`

- Classification: core.
- Contract version: `sb_engine_cpp_wrappers_1_0_0`.
- Source paths:
  - `project/include/scratchbird/engine/engine.hpp`
  - `project/include/scratchbird/engine/session.hpp`
  - `project/include/scratchbird/engine/transaction.hpp`
  - `project/include/scratchbird/engine/result.hpp`
  - `project/include/scratchbird/engine/types.hpp`
  - `project/include/scratchbird/engine/sblr_envelope.hpp`
- Execution authority: thin RAII wrappers over the C ABI.

### `driver_package_manifest_v1`

- Classification: core driver/tool/adaptor boundary.
- Contract version: `driver_package_manifest_v1`.
- Source path: `project/drivers/DriverPackageManifest.csv`.
- Driver surfaces request work through listener/manager/server routes or
  engine SBLR/API boundaries. Drivers do not own transaction finality.

### `sbsql_parser_package_v3`

- Classification: core parser package.
- Contract version: `sb_parser_package_v3`.
- Source paths:
  - `project/src/server/parser_package_registry.hpp`
  - `project/src/parsers/native/v3/package/native_v3_parser_package.hpp`
- Parser package output is translation evidence until the engine accepts a
  validated SBLR envelope and dispatches through engine API/security gates.

### `sbsql_parser_support_udr_v1`

- Classification: core parser UDR package.
- Contract version: `sb_udr_v1`.
- Source paths:
  - `project/src/udr/sbu_sbsql_parser_support/sbu_sbsql_parser_support.hpp`
  - `project/src/udr/runtime/sb_udr_runtime.hpp`
- Frozen entrypoints:
  - `sbu_sbsql_validate_syntax`
  - `sbu_sbsql_parse_to_sblr`
  - `sbu_sbsql_parse_expression`
  - `sbu_sbsql_normalize`
  - `sbu_sbsql_describe_statement`
  - `sbu_sbsql_decompile_sblr`
  - `sbu_sbsql_debug_capabilities`

### `trusted_cpp_udr_runtime_v1`

- Classification: core UDR runtime boundary.
- Contract version: `sb_udr_v1`.
- Source path: `project/src/udr/runtime/sb_udr_runtime.hpp`.
- Descriptor anchor: `UdrPackageDescriptor`.

### `cluster_provider_boundary_v1`

- Classification: non-core cluster boundary.
- Contract version: `sb_cluster_provider_v1`.
- Source paths:
  - `project/src/cluster_provider/cluster_provider.hpp`
  - `project/src/cluster_provider/no_cluster_provider.cpp`
  - `project/src/cluster_provider_stub/stub_cluster_provider.cpp`
- Non-cluster refusal code: `SBLR.CLUSTER.SUPPORT_NOT_ENABLED`.
- Positive cluster execution requires an external or stub provider boundary and
  is not claimed as core implementation.

## Extension Boundary Rows

The source-level extension boundary manifest freezes these rows:

- `parser_package.sbsql_v3`
- `parser_package.private_cluster_acceleration`
- `udr_package.trusted_cpp_v1`
- `cluster_provider.v1`
- `cluster_metrics.v1`
- `cluster_agents.v1`
- `cluster_manager.v1`

The CTest gate validates these IDs and versions against
`project/src/engine/internal_api/extensibility/extension_boundary_manifest.cpp`.

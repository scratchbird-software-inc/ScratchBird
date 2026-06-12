# DBLC-014 / P14 Reference Lifecycle Mapping Report

Status: implementation evidence only. Coordinator-owned trackers, queues, heartbeats, acceptance gates, and shared CMake files are not updated here.

## Gate Evidence

- `DBLC_P14_REFERENCE_MAPPING_COMPLETE`: SBSQL and FirebirdSQL lifecycle command surfaces now have explicit parser lifecycle mapping descriptors.
- `database_lifecycle_reference_mapping`: conformance coverage is provided in `project/tests/database_lifecycle/reference_mapping_conformance.cpp`.
- `DBLC_STATIC_NO_REFERENCE_ENGINE_SQL`: static coverage is provided in `project/tests/database_lifecycle/reference_mapping_static.py`.

## Mapping Summary

- SBSQL owns its lifecycle mapping in `project/src/parsers/sbsql_worker/statement` and `project/src/parsers/sbsql_worker/lowering`.
- SBSQL maps 15 lifecycle commands to ScratchBird lifecycle SBLR/API requests and maps 4 non-file reference/file-management command families to exact emulated diagnostics.
- FirebirdSQL owns its lifecycle mapping in `project/src/parsers/compatibility/firebird`.
- FirebirdSQL maps 6 lifecycle command families to ScratchBird lifecycle SBLR/API requests and maps 6 non-file reference/file-management command families to exact emulated diagnostics.

## Authority Rules

- Parser output is SBLR/management request evidence only; engine authority remains required for finality, storage, security, UUID resolution, and MGA lifecycle evidence.
- Reference filesystem behavior is not performed. Create database, shadow, backup/restore, file-management, service, external-plugin, and reference-tool surfaces route to ScratchBird lifecycle APIs or exact emulated diagnostics.
- No reference engine SQL execution is introduced. No SQLite, PRAGMA, WAL, or cross-dialect parser dependency shortcut is introduced.

## Shared CMake Snippet Needed

This worker did not edit shared CMake. To wire the new gates, the coordinator can add equivalent target/test wiring in the database lifecycle test area:

```cmake
add_executable(database_lifecycle_reference_mapping_conformance
  reference_mapping_conformance.cpp
)
target_compile_features(database_lifecycle_reference_mapping_conformance PRIVATE cxx_std_23)
target_link_libraries(database_lifecycle_reference_mapping_conformance
  PRIVATE
    sbl_sbsql_parser_worker_core
    sbl_firebird_parser_pipeline
)
add_test(
  NAME database_lifecycle_reference_mapping_conformance
  COMMAND database_lifecycle_reference_mapping_conformance
)
set_tests_properties(database_lifecycle_reference_mapping_conformance PROPERTIES
  LABELS "database_lifecycle_reference_mapping;DBLC_P14_REFERENCE_MAPPING_COMPLETE;database_lifecycle"
)

add_test(
  NAME database_lifecycle_reference_mapping_static
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/reference_mapping_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_reference_mapping_static PROPERTIES
  LABELS "database_lifecycle_reference_mapping;DBLC_STATIC_NO_REFERENCE_ENGINE_SQL;database_lifecycle"
)
```

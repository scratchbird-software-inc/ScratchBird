# DBLC-013S Upgrade Migration Report

Worker scope: DBLC-013S / P13S upgrade migration and refusal lifecycle.

## Implementation Summary

- Added explicit migration/refusal classifications for database headers, catalog/filespace manifest evidence, startup state formats, server configuration formats, server lifecycle state files, and IPC endpoint descriptors.
- Wired database open to fail closed on ambiguous startup/header identity and catalog/filespace/resource manifest migration evidence before resource or transaction admission continues.
- Preserved current-version acceptance while refusing unsupported old/new versions, unsafe downgrades, newer-than-supported artifacts, missing migration plans, and migration-required-without-plan cases with canonical diagnostics.
- Added the repeatable `upgrade_migration_conformance.cpp` gate and `upgrade_migration_static.py` no-guessing gate. Shared CMake wiring remains coordinator-owned.

## Canonical Diagnostics

- `ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED`
- `ENGINE.DBLC_MIGRATION_REQUIRED_WITHOUT_PLAN`
- `ENGINE.DBLC_MIGRATION_PLAN_MISSING`
- `ENGINE.DBLC_MIGRATION_UNSUPPORTED_OLD_ARTIFACT`
- `ENGINE.DBLC_MIGRATION_UNSUPPORTED_NEW_ARTIFACT`
- `ENGINE.DBLC_FORMAT_DOWNGRADE_REFUSED`
- `ENGINE.DBLC_FORMAT_NEWER_THAN_SUPPORTED`
- `SB-STARTUP-STATE-MIGRATION-REQUIRED-WITHOUT-PLAN`
- `SB-STARTUP-STATE-MIGRATION-PLAN-MISSING`
- `CONFIG.MIGRATION_REQUIRED_WITHOUT_PLAN`
- `CONFIG.MIGRATION_PLAN_MISSING`
- `CONFIG.VERSION_NEWER_THAN_SUPPORTED`
- `IPC.LIFECYCLE.MIGRATION_REQUIRED_WITHOUT_PLAN`
- `IPC.LIFECYCLE.MIGRATION_PLAN_MISSING`
- `IPC.LIFECYCLE.AMBIGUOUS_IDENTITY_REFUSED`
- `IPC.LIFECYCLE.VERSION_NEWER_THAN_SUPPORTED`

## Validation Notes

Validation results from this worker run:

- `python3 -B project/tests/database_lifecycle/upgrade_migration_static.py --repo-root ${PROJECT_ROOT}`: passed.
- `cmake -S project -B build`: passed.
- `cmake --build build --target sb_storage_database sb_server_core -j2`: passed.
- Direct compile of `project/tests/database_lifecycle/upgrade_migration_conformance.cpp` against the built libraries: passed.
- Direct run of `/tmp/database_lifecycle_upgrade_migration_conformance`: passed.
- `python3 -B ${PUBLIC_TOOL_ROOT}/skills/scratchbird-mga-transaction-authority/scripts/mga_policy_gate.py --repo ${PROJECT_ROOT}`: passed.
- `git diff --check -- <owned DBLC-013S files>`: passed.
- `ctest --test-dir build --output-on-failure -L database_lifecycle_upgrade_migration`: no tests found because shared CMake wiring is coordinator-owned and intentionally not edited by this worker.

## Shared CMake Snippet

Coordinator-owned wiring needed after review:

```cmake
if(TARGET sb_storage_database AND TARGET sb_server_core)
  add_executable(database_lifecycle_upgrade_migration_conformance
    upgrade_migration_conformance.cpp
  )
  target_compile_features(database_lifecycle_upgrade_migration_conformance PRIVATE cxx_std_23)
  target_link_libraries(database_lifecycle_upgrade_migration_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_storage_database
      sb_storage_disk
      sb_storage_page
      sb_transaction_mga
      sb_server_core
  )
  add_test(
    NAME database_lifecycle_upgrade_migration_conformance
    COMMAND database_lifecycle_upgrade_migration_conformance
  )
  set_tests_properties(database_lifecycle_upgrade_migration_conformance PROPERTIES
    LABELS "database_lifecycle_upgrade_migration;DBLC_P13S_UPGRADE_MIGRATION_COMPLETE;database_lifecycle;mga_transaction_regression"
  )
endif()

add_test(
  NAME database_lifecycle_upgrade_migration_static
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/upgrade_migration_static.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(database_lifecycle_upgrade_migration_static PROPERTIES
  LABELS "database_lifecycle_upgrade_migration;DBLC_STATIC_MIGRATION_NO_GUESSING;database_lifecycle"
)
```

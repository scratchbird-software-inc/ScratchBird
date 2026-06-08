# FSPE-012H Validation Result

Status: passed
Date: 2026-05-08
Owning slice: `FSPE-012H`
Search key: `FSPE-012H-VALIDATION-RESULT`

## Scope

FSPE-012H implemented and validated the upgrade/migration compatibility gate for parser registry versions, SBLR envelope versions, catalog seed compatibility, fixture regeneration, database open compatibility, parser cache upgrade dimensions, and engine-boundary invariants.

## Commands

| Command | Result |
| --- | --- |
| `cmake -S project -B build/sbsql_parser_worker_validation` | passed |
| `cmake --build build/sbsql_parser_worker_validation --target sbsql_upgrade_migration_compatibility_gate -j 4` | passed |
| `cmake --build build/sbsql_parser_worker_validation --target sbsql_persistence_restart_conformance sbsql_cache_epoch_correctness_conformance sbsql_security_redaction_side_channel_gate -j 4` | passed |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_upgrade_migration_compatibility_gate --output-on-failure` | passed 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L "sbsql_persistence_restart_conformance|sbsql_cache_epoch_correctness_conformance|sbsql_security_redaction_side_channel_gate|sbsql_upgrade_migration_compatibility_gate" --output-on-failure` | passed 4/4 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_deterministic_no_network_gate --output-on-failure` | passed 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure` | passed 39/39 |
| `python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/p0_precode_validation.py --gate all` | passed |

## Evidence

- `project/tests/sbsql_parser_worker/generated/upgrade/sbsql_upgrade_migration_compatibility_gate.cpp`
- `project/tests/sbsql_parser_worker/generated/upgrade/UPGRADE_MIGRATION_COMPATIBILITY_FIXTURES.csv`
- `project/tests/sbsql_parser_worker/generated/repro/DETERMINISTIC_ARTIFACT_MANIFEST.csv`
- `project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/UPGRADE_MIGRATION_COMPATIBILITY_REPORT.md`

## Result

FSPE-012H is complete. FSPE-013 is ready for assignment as the next serialized slice.

# Full Regression Suite Publication

Status: complete
Search key: `FSPE-FULL_REGRESSION_SUITE_PUBLICATION`
Owning slice: `FSPE-014C`
Date: 2026-05-08

## Configure Command

```bash
cmake -S project -B build/sbsql_parser_worker_validation
```

## Build Commands

Use targeted parser-worker builds in the validation tree. The broad default `all` target is not the published regression entrypoint because it may include unrelated non-parser linkage targets.

```bash
cmake --build build/sbsql_parser_worker_validation --target sbsql_upgrade_migration_compatibility_gate -j 4
cmake --build build/sbsql_parser_worker_validation --target sbsql_fixed_uuid_catalog_seed_gate sbsql_security_redaction_side_channel_gate sbsql_persistence_restart_conformance -j 4
cmake --build build/sbsql_parser_worker_validation --target sbsql_exhaustive_e2e_regression_gate -j 4
```

## CTest Labels

Fast full parser-worker regression:

```bash
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
```

Focused closure labels:

```bash
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_upgrade_migration_compatibility_gate --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_security_redaction_side_channel_gate --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_fixed_uuid_catalog_seed_gate --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_deterministic_no_network_gate --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_exhaustive_e2e_regression --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_generated_full_surface_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sb_server_sbsql_admission_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sb_engine_sbsql_behavior_conformance --output-on-failure
```

Execution_Plan audit labels and commands:

```bash
python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/spec_synchronization_audit.py --repo-root .
python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/developer_handoff_map_gate.py --repo-root .
python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/source_size_maintainability_gate.py --repo-root .
python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/zero_sql_engine_boundary_gate.py --repo-root .
python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/p0_precode_validation.py --gate all
```

## Fixture Roots

| Root | Contents |
| --- | --- |
| `project/tests/sbsql_parser_worker/generated/` | Generated parser-worker conformance fixtures. |
| `project/tests/sbsql_parser_worker/generated/replay/` | Differential replay fixture index and expected payloads. |
| `project/tests/sbsql_parser_worker/generated/exhaustive_e2e/` | Registered-surface route regression and dynamic UDR/SBLR gate. |
| `project/tests/sbsql_parser_worker/generated/repro/` | Deterministic generated artifact manifest and no-network gate. |
| `project/tests/sbsql_parser_worker/generated/upgrade/` | FSPE-012H upgrade/migration compatibility fixtures. |
| `project/tests/sbsql_parser_worker/generated/security/` | FSPE-012G security redaction and side-channel fixtures. |
| `project/tests/engine_public_abi/` | Engine public ABI and SBSQL behavior conformance fixtures. |

## Runtime And Disk Expectations

The fast `sbsql_parser_worker` label currently runs 39 tests in approximately two seconds in the validation build after required targets are built. Generated replay payload files are several megabytes; keep the validation build and execution_plan artifacts on local disk with at least 1 GiB free for logs, temporary databases, sockets, and future extended runs.

## Failure Triage

1. Capture the exact command, exit code, and `--output-on-failure` text.
2. Record the failing test name and label group in `artifacts/FAILURE_INVENTORY.csv` if the failure is not immediately corrected.
3. Do not open a dependent slice while a validation failure remains.
4. Re-run the failed focused label first, then re-run `sbsql_parser_worker`.
5. If a failure is infrastructure-related, record whether a message-vector diagnostic row exists or must be added.

## Cleanup Commands

These commands are publication guidance only; do not run them while evidence is still needed.

```bash
rm -rf build/sbsql_parser_worker_validation
rm -f /tmp/sb_*.log
rm -f /tmp/sb_*.sock
rm -f /tmp/sb_*.sbdb /tmp/sb_*.sbdb.*
```

Retain `project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts/`, tracked generated fixtures, validation result reports, and deterministic manifests.

## Result

The full parser-worker regression suite and execution_plan audit commands are published with exact invocation, fixture roots, expected runtime, triage, and cleanup policy. FSPE-014C is complete and FSPE-014D can open as the next serialized slice.

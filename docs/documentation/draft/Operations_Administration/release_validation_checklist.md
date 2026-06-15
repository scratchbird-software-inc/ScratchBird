# Release Validation Checklist

## Purpose

This checklist is for operators and developers who need to confirm that a ScratchBird build is trustworthy before using it with real data. It is not a support contract or a guarantee of production readiness — it is a structured evidence-gathering exercise that replaces "I think it works" with "I observed it working under these specific conditions."

**This checklist validates a build, not a deployment configuration.** Run it against the actual build you intend to use, on a representative platform, with access to the same test infrastructure that the build tree provides.

Work through each section in order. Mark each item as PASS, FAIL, or SKIP (with justification). A single unaddressed FAIL in sections 1-6 should block release use.

---

## Section 1: Build Output Completeness

Run before doing anything else. A build with missing artifacts fails in unpredictable ways.

- [ ] **1.1** The output directory contains the listener binary, manager binary, and parser worker binaries for all parsers included in this build.
- [ ] **1.2** All UDR parser support shared libraries are present in the expected output locations.
- [ ] **1.3** Parser package definition files (capability manifests) are present and readable.
- [ ] **1.4** The `tools/ceic_integrated_support_bundle.py` script is present and executable.
- [ ] **1.5** The license header gate has passed. Every compiled source file with the ScratchBird copyright block carries an MPL-2.0 SPDX identifier. Verify against the CMakeLists license header gate.

**Test tree reference**: `tests/release/driver_release_gate_foundation.py`, `tests/release/driver_release_artifact_manifest_gate.py` (via `tools/release/`).

---

## Section 2: License and Notices

- [ ] **2.1** The build carries the MPL-2.0 license text in the expected location.
- [ ] **2.2** No third-party dependency has been introduced since the last license review without a corresponding license notice.
- [ ] **2.3** The `SPDX-License-Identifier: MPL-2.0` comment is present in ScratchBird source files as verified by the CMake license header gate.

---

## Section 3: Configuration Validation

- [ ] **3.1** A minimal listener configuration file passes validation without errors.
- [ ] **3.2** A minimal manager configuration file passes validation without errors.
- [ ] **3.3** Configuration values that were valid in the previous build remain valid in this build (no `CONFIG.DOWNGRADE_REFUSED` on unchanged configurations).
- [ ] **3.4** A configuration with an intentionally invalid value produces a diagnostic code (`LISTENER.CONFIG.INVALID_VALUE` or equivalent), not a crash.

**Test tree reference**: Static analysis tests in `tests/release/` (`authority_drift_static.py`, `catalog_identity_boundary_static.py`).

---

## Section 4: IPC Smoke Test

The IPC smoke test confirms the listener and manager can establish the control-plane connection and exchange hello/hello-ack frames.

- [ ] **4.1** Start the listener. Confirm the IPC socket appears at the configured path.
- [ ] **4.2** Run `listener.status` (or equivalent) via the manager and receive a valid state snapshot with no error codes.
- [ ] **4.3** Confirm the `kCurrentParserApiMajor` version negotiates correctly: the hello-ack is `accepted = true`.
- [ ] **4.4** Send a `PING` to the listener control plane and receive a valid `HEALTH_CHECK` acknowledgment.
- [ ] **4.5** Confirm `SBSQL.SERVER.UNAVAILABLE` is not present in the response.

**Test tree reference**: `tests/manager/ipc_tester.cpp`, `tests/manager/protocol_unit_tests.cpp`, `tests/listener/control_plane_probe.cpp`.

---

## Section 5: Parser Registration and Route Smoke Test

- [ ] **5.1** At least one parser package registers successfully with the listener (no `UDR.BRIDGE.MISSING_CAPABILITY`, no `SBSQL.SERVER.UNAVAILABLE` during registration).
- [ ] **5.2** The parser package capability declaration includes the expected `supported_topologies` list (at minimum `logical_backup_restore`).
- [ ] **5.3** A simple `SELECT 1` or equivalent smoke statement routes to the engine and returns a result.
- [ ] **5.4** A statement that should be refused (`GBAK` or similar) produces the expected diagnostic code (`SBSQL.EMULATION.REFERENCE_TOOL_NOT_EXECUTED`) and does not crash the parser worker.
- [ ] **5.5** The `beta_package_smoke_gate.py` passes if a beta parser package is included.

**Test tree reference**: `tests/release/beta_package_smoke_gate.py`, `tests/release/attach_auth_conformance.cpp`, `tests/release/capability_profile_conformance.cpp`.

---

## Section 6: Database Create, Open, and Reopen

- [ ] **6.1** Create a new database. The create operation completes without error.
- [ ] **6.2** Open the newly created database. The open returns healthy status. No `FORMAT.VERSION_DOWNGRADE_REFUSED`, `SB-STARTUP-STATE-FORMAT-DOWNGRADE-REFUSED`, or startup state errors.
- [ ] **6.3** Close and reopen the database. The reopen succeeds. The catalog manifest format version is present in the open evidence.
- [ ] **6.4** Attempt to open the database with an intentionally older (incompatible) build or a simulated future format version. Confirm the refused diagnostic code is produced rather than a crash or silent corruption.
- [ ] **6.5** Confirm the `catalog_manifest_format_version` in the open evidence matches the expected current value (`1` as of this build).

**Test tree reference**: `tests/database_lifecycle/database_lifecycle_manager_conformance.py`, `tests/release/catalog_object_conformance.cpp`, `tests/release/public_upgrade_migration_gate.cpp`.

---

## Section 7: Transaction Commit and Rollback

- [ ] **7.1** Open a transaction, write a row, commit. The committed row is visible in a subsequent read.
- [ ] **7.2** Open a transaction, write a row, roll back. The row is not visible after rollback.
- [ ] **7.3** A savepoint can be set, rolled back to, and released.
- [ ] **7.4** Confirm the transaction cleanup horizon advances after committed transactions complete (check `dpc030_authoritative_cleanup_horizon_v1` evidence).
- [ ] **7.5** A long-running transaction that is not committed does not permanently block cleanup of unrelated transactions.

**Test tree reference**: `tests/release/cache_checkpoint_conformance.cpp`, `tests/mga_transaction_regression/`, `tests/release/public_transaction_mga_cow_gate.cpp`, `tests/release/public_transaction_savepoint_limbo_cleanup_gate.cpp`.

---

## Section 8: Backup and Restore Drill

A backup that has never been tested is not a backup.

- [ ] **8.1** Run `BACKUP DATABASE TO <uri>` on a database with at least one committed schema object and at least one committed data row. The backup completes without error.
- [ ] **8.2** Restore from that backup to a new (empty) database path using `RESTORE DATABASE FROM <uri>`.
- [ ] **8.3** Open the restored database. Confirm the schema object is present.
- [ ] **8.4** Confirm the data row is present.
- [ ] **8.5** Attempt `BACKUP DATABASE` (without `TO`). Confirm `SBSQL.EMULATION.NON_FILE_OPERATION` is returned and the operation does not crash or hang.
- [ ] **8.6** Attempt `NBACKUP`. Confirm the same refusal code.

**Test tree reference**: `tests/release/backup_archive_restore_conformance.cpp`, `tests/release/backup_restore_export_admin_gate_conformance.cpp`, `tests/release/public_backup_forward_session_gate.cpp`, `tests/release/public_backup_update_coverage_gate.cpp`.

---

## Section 9: Diagnostics and Support Bundle

- [ ] **9.1** Collect a listener support bundle while the listener is running. The bundle directory is created, the bundle JSON is valid, and no errors are reported.
- [ ] **9.2** Collect a manager support bundle. The `config-redacted.txt` file is present. Path values are replaced with `[path-redacted]` and secret refs with `<redacted-secret-ref-present>`. No raw socket paths or secret values appear.
- [ ] **9.3** The `RedactListenerSupportabilityText` and `RedactManagerSupportBundleText` functions are exercised: confirm that a synthetic sensitive value placed in a known field is replaced in the bundle output.
- [ ] **9.4** The `agent_metrics_audit_support_bundle_gate` passes: agent observability data is correctly included.
- [ ] **9.5** The `diagnostic_registry_gate` passes: the diagnostic code registry is consistent.

**Test tree reference**: `tests/release/agent_metrics_audit_support_bundle_gate.cpp`, `tests/release/agent_evidence_audit_redaction_retention_gate.cpp`, `tests/listener/diagnostic_registry_gate.cpp`.

---

## Section 10: Drain Admission and Graceful Shutdown

- [ ] **10.1** Issue `listener.drain`. Confirm the listener stops accepting new connections (`accepting_new_connections = false`).
- [ ] **10.2** Confirm existing sessions complete normally during drain.
- [ ] **10.3** Issue `listener.undrain`. Confirm the listener resumes accepting connections.
- [ ] **10.4** Issue `listener.stop` (graceful). Confirm the listener exits within `graceful_drain_timeout_ms` milliseconds.
- [ ] **10.5** The `drain_admission_smoke` test passes.

**Test tree reference**: `tests/listener/drain_admission_smoke.cpp`.

---

## Section 11: SBLR Surface and Parser Compatibility

- [ ] **11.1** The `sblr_surface_guardrail_gate` passes (if `SB_BUILD_SBLR_SURFACE_GUARDRAIL_GATES` is enabled for this build type). Zero unreconciled rows in the reference SBLR fixture closure.
- [ ] **11.2** The `final_sblr_sbsql_enterprise_proof_closure_gate` passes (enterprise builds).
- [ ] **11.3** The compatibility parser dialect isolation audit passes (`parser_dialect_isolation_audit_gate`).
- [ ] **11.4** Compatibility surface refusals for `COPY PROGRAM` and `COPY TO <file>` produce the correct parser diagnostics (not crashes, not silent drops).

**Test tree reference**: `tests/sblr_surface/`, `tests/sbsql_parser_worker/final_sblr_sbsql_enterprise_proof_closure_gate.py`, `tests/compatibility/`.

---

## Section 12: Known Limitations Review

Before marking a build as release-ready, the following must be reviewed:

- [ ] **12.1** The SBsql language beta proof gate (`SBsql language beta proof gate`) for this build has been reviewed and any deferral is documented.
- [ ] **12.2** All `SKIP` items in this checklist have documented justifications. No item is skipped simply because it is inconvenient to test.
- [ ] **12.3** Platform-specific test results have been reviewed for the target platform(s). Tests that are not expected to pass on a given platform are explicitly listed.
- [ ] **12.4** Any new `DOWNGRADE_REFUSED` codes introduced since the last release are documented in the release notes so operators can plan accordingly.

---

## Recording Evidence

For each checklist item that passes, record:
- Build identifier (git commit hash or release tag)
- Platform and OS version
- Date and operator name
- Test invocation command or procedure
- Observed outcome (pass/fail and any diagnostic output)

A checklist with no recorded evidence is not a completed checklist.

---

## Related Pages

- [Installation And Output Layout](installation_and_output_layout.md)
- [Operating Modes Runbook](operating_modes_runbook.md)
- [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md)
- [Backup, Restore, And Data Movement](backup_restore_and_data_movement.md)
- [Upgrade And Compatibility Policy](upgrade_and_compatibility_policy.md)

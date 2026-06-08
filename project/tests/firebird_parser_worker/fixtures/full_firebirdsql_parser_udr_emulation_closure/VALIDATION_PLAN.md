# Full FirebirdSQL Parser/UDR/Emulation Validation Plan

Status: draft
Date: 2026-05-08
Search key: `FULL-FIREBIRDSQL-PARSER-UDR-EMULATION-VALIDATION-PLAN`

## Validation Levels

| Level | Purpose |
| --- | --- |
| L0 authority/source | Prove every Firebird source, spec, seed, reference, and regression input has a hash, role, and implementation owner. |
| L0A package boundary | Prove same-dialect parser/UDR sharing is allowed and cross-dialect dependencies are impossible. |
| L0B exact row expansion | Prove every grammar/API/BLR/service/admin/catalog surface has implementation, emulation, invalid-input diagnostic, or authority-violation diagnostic ownership with zero unassigned rows. |
| L0C non-file emulation | Prove file/storage/admin surfaces never perform real Firebird file effects and have explicit emulated/report/authority-violation behavior. |
| L0D donor-native regression acquisition | Prove Firebird tools and original regression inputs are located, hashed, imported, and classified; missing roots block closure. |
| L0E clean-room provenance | Prove Firebird source is behavior evidence only and ScratchBird implementation code has clean-room provenance. |
| L0F version profile | Prove every supported Firebird version-profile difference has implementation or emulation ownership with zero unassigned rows. |
| L0G BLR and parameter buffers | Prove BLR, message BLR, SQLDA, DPB, TPB, SPB, and BPB surfaces have implementation, emulation, diagnostics, and tests. |
| L0H donor-native harness policy | Prove donor tools run in sandboxed tiered CTest lanes with deterministic result normalization and complete failure inventory. |
| L0I wire/API full scope | Prove every Role A wire/API/service/replication/proxy surface has implementation or emulation ownership with zero unimplemented rows. |
| L0J agent orchestration | Prove every implementation slice has agent ownership, five-minute refresh evidence, test cadence, blocker state, and escalation handling. |
| L1 parser unit | Lexer, CST, AST, binder, descriptor, UUID cache, datatypes, expressions, DML, DDL, PSQL, diagnostics. |
| L2 UDR ABI | `sbup_firebird` Role B dynamic SQL and Role C environment installer entrypoints. |
| L3 parser worker/runtime | `sbp_firebird` process isolation, parser-server IPC, prepare/bind/execute/fetch/cancel, result rendering. |
| L4 SBLR/engine | Firebird BoundAST lowers to finite SBLR subset and engine behavior matches accepted/emulated rows. |
| L5 catalog/security overlays | RDB$, MON$, SEC$, datatype domains, helper functions, and emulated security surfaces. |
| L6 donor-native tools | Firebird-built `isql` and service tools drive ScratchBird Firebird endpoint as test-only clients. |
| L6A original donor regression replay | Original Firebird regression cases replay through ScratchBird or block closure until acquired. |
| L6B differential oracle | Real Firebird normalized output is compared with ScratchBird output where legal and practical. |
| L7 diagnostics | Canonical diagnostics render as Firebird status vectors, SQLCODE, SQLSTATE, warnings, and SQLDA/result metadata. |
| L8 hardening | No third-party Firebird runtime libraries, no cross-dialect linkage, no raw SQL to engine, no donor authority import, no resource abuse. |
| L9 final E2E | Every row has parser, UDR if applicable, SBLR, engine, diagnostic, donor-native replay, implementation, or emulation evidence. |

## Mandatory CTest Gates

```text
firebird_surface_registry_lint
firebird_agent_orchestration_gate
firebird_package_boundary_gate
firebird_lexer_conformance
firebird_cst_ast_conformance
firebird_binder_descriptor_uuid_conformance
firebird_datatype_descriptor_conformance
firebird_expression_builtin_conformance
firebird_dml_query_conformance
firebird_ddl_catalog_overlay_conformance
firebird_psql_dynamic_sql_conformance
firebird_non_file_emulation_gate
firebird_sblr_lowering_verifier
firebird_parser_worker_ipc_conformance
firebird_udr_dynamic_sql_conformance
firebird_udr_environment_installer_conformance
firebird_bridge_service_surface_conformance
firebird_wire_api_scope_gate
firebird_status_vector_diagnostic_conformance
firebird_upgrade_install_drift_gate
firebird_donor_tool_build_gate
firebird_donor_tool_sandbox_gate
firebird_isql_original_regression_gate
firebird_service_tool_regression_gate
firebird_donor_native_result_normalization_gate
firebird_original_regression_replay_gate
firebird_donor_tool_diff_oracle_gate
firebird_operational_failure_inventory_gate
firebird_tool_runtime_isolation_gate
firebird_blr_parameter_buffer_conformance
firebird_version_profile_gate
firebird_clean_room_provenance_gate
firebird_malicious_input_fuzz_gate
firebird_runtime_absence_gate
firebird_exhaustive_e2e_regression_gate
firebird_final_zero_open_surface_audit
```

## Test Case Coverage Rule

Every expanded Firebird row must have at least one of:

- Parser-only valid or invalid-input test.
- Parser bind/lower/SBLR verifier test.
- UDR parity test.
- Engine E2E test.
- Donor-native tool replay test.
- Differential oracle test.
- Emulated behavior test.
- Authority-violation diagnostic test for forbidden real file or native-authority attempts.

The final audit fails if any valid Firebird-visible row lacks implementation or emulation evidence. It also fails if any invalid-input or authority-violation row lacks diagnostic evidence.

## Donor-Native Replay Requirements

Donor-native replay must:

- Build Firebird tools only in a test-only external build area.
- Preserve donor tool stdout, stderr, exit status, status vectors, and normalized result output.
- Never link donor tools or Firebird client libraries into ScratchBird runtime, parser worker, or UDR products.
- Run donor tools in isolated temporary directories with loopback-only endpoints, no system install, no external network, strict timeouts, and deterministic cleanup.
- Run `isql` against SQL/PSQL/query/DDL/DML/catalog tests.
- Run service utilities or service-manager client paths for backup, restore, validation, trace, users, roles, nbackup, gstat, and related service surfaces.
- Compare exact output where stable and normalized output where Firebird includes timestamps, IDs, paths, page numbers, or environment-specific values.
- Classify file-affecting tests as emulated or authority-violation diagnostic unless a ScratchBird migration/report surface explicitly implements the behavior.

## Failure Handling

Failures must be collected as a complete inventory before debugging. A failed donor-native regression may close only when:

- ScratchBird behavior is corrected; or
- The Firebird row is implemented as exact emulation with evidence; or
- The Firebird row is an invalid-input or authority-violation case with canonical diagnostic evidence; or
- The test depends on real Firebird file/storage/recovery/security authority and is covered by non-file emulation or authority-violation diagnostics instead.

# Full FirebirdSQL Parser, Parser-Support UDR, Emulation, and Regression Closure Execution_Plan

Status: draft
Created: 2026-05-08
Owner: ScratchBird reference parser/UDR implementation coordinator
Search key: `FULL-FIREBIRDSQL-PARSER-UDR-EMULATION-IMPLEMENTATION-CLOSURE`

## Purpose

Implement the FirebirdSQL reference compatibility package as an independent dialect bundle:

```text
Firebird SQL/API/BLR/service input
  -> sbp_firebird or sbup_firebird Firebird-owned lexer/CST/AST/BoundAST
  -> UUID, descriptor, policy, transaction, and render binding
  -> Firebird finite SBLR subset
  -> sb_server admission and engine execution
  -> Firebird-compatible status vector, SQLDA/result, catalog, or service response
```

This is not an SBSQL alias layer. FirebirdSQL must not depend on SBSQL, PostgreSQL, MySQL, or any other reference parser/UDR package. The external parser worker and trusted parser-support UDR may share Firebird-owned dialect/wire/support libraries so same-dialect behavior cannot diverge.

## Scope

The closure covers:

- `sbp_firebird`: untrusted external parser worker and parser-server IPC integration.
- `sbup_firebird`: trusted parser-support UDR with Role A bridge, Role B dynamic SQL parser, and Role C environment installer behavior as required by parser-v3 UDR contracts.
- Firebird-owned shared libraries such as `sbl_firebird_dialect` and, where needed, `sbl_firebird_wire`.
- RDB$, MON$, SEC$, and optional INFORMATION_SCHEMA-compatible overlay surfaces.
- Firebird datatype, descriptor, domain, blob, array, charset, collation, and pseudo-type behavior.
- Firebird functions, operators, aggregates, windows, context variables, generator behavior, and diagnostics.
- DSQL, DDL, DML, PSQL, routines, triggers, packages, dynamic SQL, services, utility, API, and BLR-facing compatibility surfaces.
- Non-file emulation for file/storage/admin commands such as `CREATE DATABASE`, shadow, backup, restore, validation, nbackup, external tables, trace, plugins, and service manager operations.
- CTest coverage generated from exact Firebird surface rows plus reference-native tool and original reference regression replay.

## Boundary Rules

- Reusing SBSQL parser or UDR code.
- Linking ScratchBird runtime products against Firebird reference tools or third-party Firebird client libraries.
- Importing Firebird storage, recovery, filesystem, security database, transaction, optimizer, cluster, finality, or catalog authority into ScratchBird core.
- Allowing reference SQL/API/BLR frames to enter `sb_engine` as executable authority.

## Authority Inputs

| Input | Role |
| --- | --- |
| `public_contract_snapshot` | Firebird parser profile, identifier rules, response shape, command/builtin mapping counts, security boundary. |
| `public_contract_snapshot` | Firebird capability, catalog, setup, and conformance authority. |
| `public_contract_snapshot` through `appendix-reference-firebird-exact-extraction-slice-72-completion-closure-and-conformance-summary-profile.md` | Firebird 5.0.4 exact extraction closure inputs. |
| `public_contract_snapshot` | Exact row model for valid reference syntax, invalid input, API, protocol, metadata, and admin operations. |
| `public_contract_snapshot` | CST/AST/BoundAST/SBLR lowering map rules and Firebird map keys. |
| `public_contract_snapshot` | Parser-support UDR Role A, Role B, Role C, sharing, and trust boundary. |
| `public_contract_snapshot` | Trusted dynamic SQL parser UDR ABI and behavior. |
| `public_contract_snapshot` | Reference environment installer ABI, idempotency, UUID, and version drift behavior. |
| `public_contract_snapshot` | Firebird status-vector and diagnostic rendering contract. |
| `project/tests/reference_regression/reference_release_acquisition/firebird/5.0.4/source` | Local Firebird 5.0.4 source used as read-only reference evidence. |
| `project/tests/reference_regression/reference_catalog_seeds/firebird/` | Firebird catalog seed, builtin inventory, behavior mapping, and catalog overlay inputs. |
| `docs/reference/reference_reference_packets/emulation_1_to_1_engine_reference_packets_2026-04-02/firebird/` | Firebird datatype, index, catalog, and source authority reference packets. |

## Current Readiness

Enough local source and contract evidence exists to build the implementation plan. The full implementation must not start until P0 artifacts expand this evidence into machine-checkable rows.

Known starting facts:

- No Firebird parser, UDR, or tests currently exist under `project/src/parsers`, `project/src/udr`, or `project/tests`.
- Firebird source code is locally available under `project/tests/reference_regression/reference_release_acquisition/firebird/5.0.4/source`.
- Firebird parser and protocol source evidence includes `src/dsql/parse.y`, `src/dsql/Parser.cpp`, `src/dsql/Keywords.cpp`, `src/common/ParserTokens.h`, `src/include/firebird/impl/blr.h`, and `src/remote/parser.cpp`.
- Firebird tool source exists for `isql`, `gbak`, `gfix`/`alice`, `gstat`, `nbackup`, `fbsvcmgr`, `fbtracemgr`, and `gsec`.
- The release evidence packet currently marks upstream regression roots as unresolved in `project/tests/reference_regression/reference_release_acquisition/firebird/5.0.4/regression/SOURCE_POINTERS.md`; this is a P0 blocker for reference-native regression completeness.
- The Firebird profile records `mapped: 122`, `emulated: 2`, and `unresolved: 6` command rows, plus `accepted_alias: 3` and `unresolved_evidence_gap: 224` builtin alias rows. P0 must expand every row and assign implementation, emulation, invalid-input diagnostic, or authority-violation diagnostic ownership.

## Non-Negotiable Rules

- Cross-dialect dependencies are design-breaking.
- Same-dialect parser/UDR shared code is allowed only inside Firebird-owned libraries.
- ScratchBird core must run with no parser/UDR packages installed.
- Firebird parser packages are optional installable surfaces.
- `sbp_firebird` is untrusted and must not execute SQL, mutate engine state, open database files, or enforce security as authority.
- `sbup_firebird` is trusted only after registration, signature, version, ABI, and policy checks.
- Firebird source is read-only behavior evidence. ScratchBird implementation code must be clean-room and must not copy Firebird implementation code.
- Firebird `5.0.4` is the initial evidence baseline. All supported Firebird profile differences must be captured in the version-profile matrix with zero unassigned rows.
- All Firebird-visible SQL, API, BLR, service, metadata, utility, and diagnostic elements are in scope. No valid Firebird-visible element may be closed without implementation or emulation ownership and tests.
- Firebird file/storage/admin commands must have zero real Firebird file effects. They must become emulated catalog/service surfaces, migration/report operations, or exact authority-violation diagnostics when a client requests a forbidden real file effect.
- Firebird security/catalog compatibility surfaces must remain emulated projections over ScratchBird authority.
- No third-party Firebird client library may be wrapped by runtime products.
- Reference-native tools may be built and used only by the CTest harness, never linked into ScratchBird runtime products.
- Reference-native tools must run in a sandboxed test area with loopback-only endpoints, strict timeouts, no system install, no external network, preserved logs, and deterministic cleanup.
- The engine executes SBLR/internal procedures only.
- Object identity remains UUID-based.
- Recovery remains MGA-based. Firebird or PostgreSQL-style WAL authority must not be introduced.

## Required P0 Artifacts

P0 is complete only when the artifacts listed in `artifacts/FIREBIRD_REQUIRED_P0_ARTIFACTS.csv` exist and are internally linted.

Required artifact families:

- Source authority and license hash records.
- Clean-room provenance and source-use policy.
- Firebird version profile matrix.
- Firebird surface registry and implementation backlog.
- Firebird grammar/API/BLR-to-SBLR lowering matrix.
- Firebird BLR, message BLR, SQLDA, DPB, TPB, SPB, and BPB matrix.
- Firebird datatype and descriptor matrix.
- Firebird builtin/function/operator matrix.
- Firebird catalog overlay matrix.
- Firebird non-file emulation policy.
- Firebird diagnostic/status-vector matrix.
- Firebird package boundary and no-cross-dialect dependency map.
- Firebird wire/API Role A full-scope implementation map.
- Firebird CTest variation matrix.
- Firebird CTest tiering policy.
- Firebird reference-native tool and original regression replay manifest.
- Firebird reference-native result normalization and operational failure inventory policy.
- Firebird agent implementation orchestration policy and execution status template.

## High-Level Implementation Order

```text
P0 source/surface/regression acquisition
  -> P0A package boundary and placement decision
  -> P0B exact row expansion
  -> P0C non-file emulation policy
  -> P0D reference-native regression acquisition
  -> P0E definition of done
  -> P0F clean-room provenance and source-use gate
  -> P0G Firebird version-profile scope matrix
  -> P0H BLR SQLDA and parameter-buffer matrix
  -> P0I reference-tool sandbox result-normalization CTest-tier and failure-inventory policy
  -> P0J wire/API Role A full-scope implementation decision
  -> P0K agent orchestration five-minute refresh and test cadence policy
  -> P1 registry and linter generation
  -> P2 lexer/tokenizer
  -> P3 CST/AST
  -> P4 binder/descriptors/UUID cache
  -> P5 datatypes/domains/blob/array
  -> P6 expressions/builtins/functions
  -> P7 DML/query/cursor/locking
  -> P8 DDL/catalog overlays
  -> P9 PSQL/routines/triggers/packages/dynamic SQL
  -> P10 non-file emulation
  -> P11 Firebird SBLR lowering and verifier
  -> P12 parser worker runtime
  -> P13 parser-support UDR Role B
  -> P14 environment installer UDR Role C
  -> P14A upgrade install drift and silent-repair diagnostic tests
  -> P15 bridge/service/wire surfaces Role A
  -> P15A reference-native tool build and smoke
  -> P15B original reference regression replay
  -> P15C differential reference oracle lane
  -> P15D reference-native result normalization and operational failure inventory
  -> P16 diagnostics/status-vector rendering
  -> P17 hardening package isolation and no-authority-drift gates
  -> P17A Firebird SQL BLR wire buffer service and reference-tool fuzzing
  -> P17B runtime absence disabled incompatible and policy-denied package tests
  -> P18 final generated E2E and zero-open audit
```

## Reference-Native Regression Requirement

The CTest suite must include a reference-native regression lane. This lane builds selected Firebird tools from the local reference source in an isolated test-only build area, points those tools at the ScratchBird Firebird parser/listener endpoint, and replays reference-original tests where available.

The reference-native lane must classify every test case as one of:

- `pass_exact`
- `pass_normalized`
- `emulated_expected`
- `authority_violation_expected`
- `invalid_input_expected`

The lane must fail if a reference-visible surface has no matching generated test, reference-native replay test, differential oracle, or emulation/diagnostic rule.

## Agent-Managed Implementation Requirement

Implementation must be executed as an agent-managed workstream once the execution_plan enters code execution. The agent manager owns active slice assignment, refresh cadence, test cadence, integration order, and escalation.

Required operating rules:

- Every active slice must have an assigned agent role, owned file/module scope, expected outputs, current status, last refresh timestamp, current test gate, and evidence path in `artifacts/AGENT_EXECUTION_STATUS.csv`.
- The agent manager must refresh status at least every five minutes while implementation agents are active. The refresh checks agent completion, uploaded changes, local diff scope, current blocker state, and next runnable tests.
- If an agent completes a patch, the agent manager must review the patch, integrate it, run the narrowest relevant tests, update the status artifact, and either assign the next unblocked slice or start the next integration/test cycle.
- If no agent is active and unblocked work remains, the agent manager must assign the next ready slice rather than idle.
- Broad CTest lanes run at milestone boundaries. Narrow tests run after each integrated patch that changes code, generated fixtures, registry data, ABI surfaces, parser behavior, UDR behavior, or diagnostics.
- Implementation continues until all slices pass their acceptance gates and final zero-open audit, or until a human decision is required for an authority conflict, missing reference regression acquisition source, destructive operation outside approved boundaries, security/policy contradiction, or incompatible contract requirement.
- Human escalation must include the slice id, exact decision required, evidence paths, attempted alternatives, tests already run, and the reason autonomous continuation would risk incorrect implementation.

## Required CTest Closure Labels

The exact commands are materialized during implementation, but these labels are mandatory:

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
firebird_reference_tool_build_gate
firebird_reference_tool_sandbox_gate
firebird_isql_original_regression_gate
firebird_service_tool_regression_gate
firebird_reference_native_result_normalization_gate
firebird_original_regression_replay_gate
firebird_reference_tool_diff_oracle_gate
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

## Definition Of Done

This execution_plan is complete only when:

- Every Firebird surface row is implemented or emulated with evidence.
- Every accepted row has parser, UDR if applicable, SBLR lowering, diagnostic, and E2E CTest coverage.
- Every invalid input, policy denial, and native-authority/file-effect attempt is recognized and fails before engine execution with Firebird-compatible diagnostics.
- Every non-file command has emulated/report behavior or an authority-violation diagnostic with no real file effect.
- `sbp_firebird` and `sbup_firebird` share Firebird-owned code where required and do not diverge.
- No Firebird package links against SBSQL or another reference parser/UDR.
- ScratchBird core still builds and runs without Firebird installed.
- Original reference tool/regression lanes are passing or blocked with a mandatory acquisition gap that prevents closure.
- Agent execution status shows no active blockers, stale refreshes, unintegrated patches, or untested completed slices.
- The final zero-open audit passes with no unowned rows, no missing tests, zero unassigned rows, no unresolved builtin evidence gaps, and no cross-dialect dependency.

# No-Defer Lifecycle Contract

Status: P0A draft/preparatory contract artifact; not closure evidence until `DBLC-000` prerequisite closure and coordinator validation
Owning slice: `DBLC-000A`
Acceptance gate: `DBLC_P0A_NO_DEFER_READY`
Search key: `NO-DEFER-LIFECYCLE-CONTRACT`

## Purpose

This contract defines the closure rules for the full database lifecycle
execution_plan. It prevents a slice from closing by documenting intent, leaving a
parser-only/API-only/spec-only path, recording a stub, or assigning behavior to a
future slice without a canonical refusal or implementation path.

`DBLC-000A` depends on `DBLC-000`. This contract is preparatory evidence only
and must not be used to mark `DBLC-000A` or `DBLC_P0A_NO_DEFER_READY` passed
before `DBLC-000` is closed and the coordinator validates this artifact against
the authority map and baseline inventory.

This file is execution-control evidence. It is not product behavior. Product
behavior must be present in manifest-listed canonical contracts, accepted
decision records, registries, implementation packets, implementation code, and
tests before any lifecycle slice can close.

## Mandatory Closure Rule

Every lifecycle state, operation, transition, invalid transition, route,
diagnostic, and refusal class must close by one of these paths:

| Closure path | Required proof |
| --- | --- |
| `implemented_full_route` | Canonical spec or packet behavior exists; registry/API rows exist where applicable; engine/server/listener/parser/IPC/client route behavior is implemented; diagnostics, audit, metrics, cache invalidation, and CTest/static gates pass. |
| `implemented_engine_internal` | Canonical behavior exists; operation is explicitly engine-internal or no-user-command; engine implementation and internal conformance tests pass; route surfaces refuse unsupported external access with canonical diagnostics. |
| `canonical_fail_closed` | Canonical spec or packet defines the refusal; diagnostic-code and diagnostic-shape rows exist; implementation returns exact message vectors; negative tests prove fail-closed behavior. |
| `canonical_no_user_command` | Canonical spec states that no CLI/admin/client command exists for the operation; internal operation route is implemented or canonical fail-closed behavior is tested for external attempts. |
| `canonical_cluster_private` | Canonical cluster scope defines the private behavior or states that standalone mode fails closed; standalone tests prove no cluster route, schema, metric, transaction, agent, or lifecycle path is entered. |
| `canonical_not_applicable` | Canonical spec proves the concept does not apply to ScratchBird behavior, defines the exact refused route if exposed, and has tests or static gates preventing accidental admission. |

Any other closure path is invalid.

## Prohibited Closure Outcomes

These words may appear in this artifact only as prohibited examples. They must
not appear as closure status, acceptance evidence, implementation rationale, or
tracker resolution for any lifecycle row: `todo`, `stub`, `placeholder`,
`future`, `later`, `deferred`, `parser-only`, `api-only`, `spec-only`,
`documentation-only`, `not implemented`, `unknown`, `tbd`, `temporary`, or
`best effort`.

| Invalid outcome | Why it fails | Required replacement |
| --- | --- | --- |
| Work exists only in this execution_plan. | Execution_Plans are execution control and not product behavior. | Add behavior to canonical specs/packets and close through implementation or canonical refusal. |
| Operation exists only in parser syntax. | Parser cannot be lifecycle authority and cannot execute SQL in the engine. | Lower to SBLR/API route and implement engine behavior or exact refusal. |
| Operation exists only in an internal API row. | Registry/API exposure without behavior leaves no product contract. | Implement dispatch, behavior, diagnostics, and tests or remove/refuse through canonical rule. |
| Operation exists only in a contract. | Spec text without code/tests does not close implementation. | Implement and test, or define canonical fail-closed behavior and test it. |
| Unsupported behavior returns generic error text. | Lifecycle refusals require canonical message vectors and parser rendering. | Add diagnostic-code and diagnostic-shape rows, message-vector mapping, and tests. |
| Standalone cluster path is silently ignored. | Cluster behavior must fail closed until authority exists. | Return canonical cluster-authority refusal with tests. |
| Recovery or migration guesses from timestamps, UUID order, file order, parser state, route state, cache state, or checkpoint state. | These are not MGA finality authority. | Use MGA evidence, transaction inventory, durable state, and canonical recovery/refusal rules. |
| Shutdown force is implicit. | Force termination must be explicit and database-scoped. | Require explicit force flag, association evidence, isolation proof, and tests. |
| Drop succeeds despite blockers. | Unsafe removal can destroy evidence or another database's files. | Refuse with exact blocker diagnostics or quarantine by canonical policy. |

## Slice Closure Checklist

A coordinator may mark a lifecycle slice `validation_passed` only when every
applicable item below is true.

1. Canonical destination exists in `public_release_evidence`, an accepted decision
   record, a registry, or an implementation packet listed by authority.
2. State names and transitions map to
   `artifacts/DATABASE_LIFECYCLE_STATE_MODEL_DRAFT.md` or an accepted canonical
   successor mapping.
3. Every source state, target state, invalid transition, and recovery action is
   explicit.
4. Every operation family has an intended route: client/admin/CLI, internal
   engine route, no-user-command policy, or canonical fail-closed route.
5. Every public or cross-process route is database-scoped and proves it cannot
   control unrelated databases.
6. Every success path records durable evidence, runtime evidence, audit,
   metrics, cache invalidation where applicable, and idempotency/recovery keys.
7. Every refusal path has diagnostic-code and diagnostic-shape registry entries,
   message-vector rendering, redaction, retryability, audit policy, and owner.
8. Every implementation path preserves MGA transaction, rollback, recovery,
   cleanup, and finality authority.
9. Parser, listener, manager, driver, adapter, tool, donor, UDR, cache,
   checkpoint, route, and job state do not become storage, catalog, security,
   transaction, or SBLR execution authority.
10. Tests or static gates cover success, refusal, invalid transition,
    authorization denial, stale evidence, malformed input, recovery, restart, and
    full-route behavior where applicable.
11. The relevant tracker row, acceptance gate, generated validation command, and
    evidence path can be updated by the coordinator after validation.
12. No closure evidence relies on source line numbers as permanent audit
    anchors; use stable search keys, row ids, operation ids, or fixture ids.

## Canonical Destination Requirement

Before a slice may begin implementation, each lifecycle item must be assigned to
one or more destination classes.

| Item class | Required canonical destination |
| --- | --- |
| Durable database state | `public_contract_snapshot`, `public_contract_snapshot`, and `public_input_snapshot`. |
| Filespace state | Storage/MGA canonical specs plus `public_input_snapshot`. |
| Server, listener, parser, manager, IPC, route, and association state | `public_contract_snapshot` plus matching implementation packets for server, IPC, manager, listener, parser, and interface closure. |
| Authentication, authorization, security provider, protected material, principal, role, privilege, and audit state | `public_contract_snapshot` plus security/default-policy packets and diagnostic registries. |
| Configuration, policy, default policy seed, and policy generation state | `public_input_snapshot`, `public_input_snapshot`, default policy registry, conformance manifest, and diagnostic registries. |
| Resource seed, timezone, charset, collation, locale, quota, and resource pool state | `public_contract_snapshot` and operational/resource conformance gates. |
| Maintenance, restricted-open, inspect, verify, repair, health, support bundle, packaging, service, and operational mode state | `public_contract_snapshot` plus engine lifecycle and service/runtime packets. |
| SBLR/API operation behavior | SBLR opcode registry, operation matrix, engine API surface registry, public ABI mapping when applicable, and dispatch tests. |
| Refusal, warning, failure, and diagnostic behavior | Reconciliation diagnostic-code registry, diagnostic-shape registry, message-vector fixtures, redaction policy, retryability, audit policy, and parser rendering tests. |
| Test and traceability behavior | P0C validation command materialization, `VALIDATION_PLAN.md`, generated CTest labels/static gates, and generated state/transition/refusal/route traceability. |

## Transition Closure Contract

Each transition must have a transition record in canonical specs or generated
traceability with these fields:

| Field | Required value |
| --- | --- |
| `transition_id` | Stable identifier. |
| `operation_family` | Execution_Plan operation family or registry operation. |
| `source_state` | Exact lifecycle state or explicit state set. |
| `target_state` | Exact target lifecycle state. |
| `admission_authority` | Engine/MGA/security/policy/config/catalog/owner/route authority needed before transition starts. |
| `success_evidence` | Durable and runtime evidence required before external success. |
| `refusal_class` | Exact refusal class for each invalid or unsafe condition. |
| `message_vector` | Diagnostic code and shape used for refusal/failure/warning. |
| `idempotency_key` | Operation id, transaction id, database UUID, filespace UUID, route generation, shutdown generation, or explicit none with canonical justification. |
| `recovery_action` | Complete, roll back, restricted open, quarantine, operator review, refuse open, or ignore advisory state. |
| `full_route_scope` | Client/admin/CLI/internal route, database scope, and cross-database isolation proof where applicable. |
| `test_gate` | CTest label, generated fixture, static gate, or blocking gate. |

If any field is absent, the transition is not closed.

## Refusal Closure Contract

Every refusal class from the state model must close through exact diagnostics
and tests. The classes below are mandatory P0A vocabulary; later canonical specs
may split a class into narrower rows but cannot remove the fail-closed behavior
without an accepted authority update.

| Refusal class | Required closure proof |
| --- | --- |
| `refusal.invalid_state_transition` | Invalid source/target operation tests and canonical message vector. |
| `refusal.missing_canonical_authority` | Blocking gate proving no implementation closes from execution_plan-only behavior. |
| `refusal.missing_durable_evidence` | Tests for missing tx, owner, state, manifest, header, catalog, audit, or route evidence. |
| `refusal.tx1_incomplete` | Interrupted create tests proving complete, roll back, quarantine, or refusal under MGA authority. |
| `refusal.tx2_incomplete` | Interrupted first-open activation tests proving recovery/refusal before ordinary work. |
| `refusal.final_shutdown_missing` | Unclean shutdown reopen tests proving recovery classification. |
| `refusal.owner_missing_or_stale` | Stale/missing owner token tests. |
| `refusal.owner_ambiguous` | Multi-owner and ambiguous shared/dedicated daemon tests. |
| `refusal.authentication_denied` | Engine-owned auth denial with parser relaying only. |
| `refusal.authorization_denied` | Engine-owned authorization denial with audit/redaction. |
| `refusal.policy_missing_or_stale` | Missing/stale policy generation refusal and cache invalidation tests. |
| `refusal.policy_override_rejected` | Override fixture rejection tests, including standalone cluster-only refusal. |
| `refusal.catalog_epoch_stale` | Metadata/cache/projection stale epoch tests. |
| `refusal.security_epoch_stale` | Security provider/principal/role/privilege stale epoch tests. |
| `refusal.resource_epoch_stale` | Resource seed/version/locale/collation stale or unsupported tests. |
| `refusal.filespace_identity_missing` | Missing registry/header/manifest/catalog identity tests. |
| `refusal.filespace_identity_duplicate` | Duplicate UUID and duplicate active-primary tests. |
| `refusal.filespace_identity_mismatch` | Database UUID, filespace UUID, page size, generation, manifest, header, or catalog mismatch tests. |
| `refusal.filespace_quarantined` | Quarantined filespace read/write/attach/transaction/drop behavior tests. |
| `refusal.recovery_required` | Dirty/unclean reopen refuses ordinary work until classification. |
| `refusal.recovery_ambiguous` | Ambiguous evidence quarantines or refuses without guessing. |
| `refusal.format_unknown` | Unknown protocol/persisted-format version refusal tests. |
| `refusal.format_downgrade` | Newer/unsafe downgrade refusal tests. |
| `refusal.migration_unsupported` | Unsupported old artifact migration refusal tests. |
| `refusal.route_association_missing` | Missing manager/listener/parser/IPC/session association evidence tests. |
| `refusal.route_association_stale` | Stale generation/heartbeat/descriptor/shutdown association tests. |
| `refusal.route_scope_cross_database` | Shared/dedicated server isolation and unrelated database non-impact tests. |
| `refusal.ipc_malformed` | Malformed frame/descriptor refusal tests. |
| `refusal.ipc_unauthenticated` | Unauthenticated IPC refusal tests. |
| `refusal.ipc_unauthorized` | Unauthorized IPC refusal tests. |
| `refusal.backpressure_policy` | Backpressure/quota admission refusal tests. |
| `refusal.shutdown_ack_timeout` | Acknowledgement timeout and listener-failure parser fallback tests. |
| `refusal.force_not_explicit` | Non-force command cannot terminate associated components. |
| `refusal.transaction_admission_denied` | Ownership/security/policy/catalog/filespace/lock/memory/cluster transaction denial tests. |
| `refusal.transaction_outcome_unknown` | Unknown outcome diagnostics persist until MGA evidence resolves. |
| `refusal.mode_fence_active` | Maintenance/restricted/verify/repair/shutdown/quarantine fences block forbidden operations. |
| `refusal.repair_plan_missing` | Repair without plan/evidence refuses. |
| `refusal.backup_archive_hold` | Backup/archive/restore/shadow/snapshot/legal hold blockers tested. |
| `refusal.udr_extension_policy` | UDR/extension policy, resource, and authority-boundary refusal tests. |
| `refusal.resource_quota` | Workload/resource quota refusal tests. |
| `refusal.cluster_authority_missing` | Standalone cluster path fail-closed tests. |
| `refusal.encryption_key_missing` | Missing/stale/unavailable key refusal tests. |
| `refusal.drop_blocked` | Open handle, active pin, ownership, backup, repair, cluster, storage, and policy blocker tests. |

## Operation Family Closure Matrix

Each operation family from the execution_plan must close with one of the valid closure
paths. The default is `implemented_full_route` unless this table states a
narrower valid option.

| Operation family | Valid closure path |
| --- | --- |
| `lifecycle.create_database` | `implemented_full_route` through create route, tx1, first filespace, policy/resource/security/catalog seed, diagnostics, and tests. |
| `policy.default_catalog.bootstrap` | `implemented_engine_internal` for generated seed execution plus full create-route tests and policy diagnostics. |
| `policy.default_catalog.registry_hardening` | `implemented_engine_internal` plus registry/static gates and generated fixture tests. |
| `storage.filespace.lifecycle` | `implemented_full_route` or `implemented_engine_internal` for internal-only transitions, with canonical refusal for unsafe user attempts. |
| `lifecycle.first_open_activation` | `implemented_full_route` through open/attach/session activation and tx2 evidence. |
| `lifecycle.open_database` | `implemented_full_route` with startup classification, ownership, recovery, and refusal tests. |
| `lifecycle.attach_database` | `implemented_full_route` with engine auth/authz and deny message vectors. |
| `lifecycle.detach_database` | `implemented_full_route` with cleanup of transaction/cursor/parser/IPC/session/notification/cache state. |
| `lifecycle.begin_transaction_admission` | `implemented_full_route` through engine MGA admission and refusal vectors. |
| `lifecycle.enter_maintenance` | `implemented_full_route` through intended admin route or `canonical_no_user_command` for any internal-only entry substep. |
| `lifecycle.exit_maintenance` | `implemented_full_route` through intended admin route or `canonical_no_user_command` for any internal-only exit substep. |
| `lifecycle.enter_restricted_open` | `implemented_full_route` or `implemented_engine_internal` for recovery-triggered restricted entry. |
| `lifecycle.exit_restricted_open` | `implemented_full_route` or `implemented_engine_internal` with exact diagnostics. |
| `lifecycle.inspect_database` | `implemented_full_route` with redaction, no mutation authority, and diagnostic tests. |
| `lifecycle.verify_database` | `implemented_full_route` with bounded-memory evidence and no silent repair. |
| `lifecycle.repair_database` | `implemented_full_route` with accepted plan/evidence or `canonical_fail_closed`. |
| `lifecycle.recovery_entry` | `implemented_engine_internal` plus external open/reopen tests proving recovery/refusal before user work. |
| `lifecycle.shutdown_database` | `implemented_full_route` with fence, notification, acknowledgement, drain, final transaction, owner release, and isolation tests. |
| `lifecycle.shutdown_force_database` | `implemented_full_route` with explicit force, target association set, termination evidence, and isolation tests. |
| `lifecycle.shutdown_ack_database` | `implemented_full_route` or `implemented_engine_internal` for component ack handling, with stale/missing refusal tests. |
| `lifecycle.shutdown_parser_fallback` | `implemented_engine_internal` plus route tests for listener failure, parser association lookup, stale/missing refusal, and isolation. |
| `lifecycle.drop_database` | `implemented_full_route` with blocker refusal, evidence preservation, removal/quarantine policy, and filespace cleanup tests. |
| `lifecycle.exclusive_ownership` | `implemented_engine_internal` plus open/reopen/server tests for stale, missing, and ambiguous owners. |
| `process.manager_family.lifecycle` | `implemented_full_route` for control-plane surfaces; `canonical_cluster_private` for sbmc private cluster-manager behavior in standalone mode. |
| `process.listener.lifecycle` | `implemented_full_route` with parser-pool, drain, forced stop, failure, quarantine, and isolation tests. |
| `process.parser_family.lifecycle` | `implemented_full_route` with package admission, handshake, handoff, pre-auth relay, attach relay, active, drain, recycle, terminate, and no authority bypass tests. |
| `process.lifecycle.association_registry` | `implemented_engine_internal` plus full shutdown/fallback/isolation tests. |
| `process.server_daemon.lifecycle` | `implemented_full_route` with shared/dedicated isolation, service-ready, stop, force stop, restart, orphan cleanup, failed, and quarantine tests. |
| `process.ipc_channel.lifecycle` | `implemented_full_route` with endpoint descriptor, authn/authz, frame validation, backpressure, drain, stale cleanup, failure, and quarantine tests. |
| `runtime.attachment_session_request_cursor.lifecycle` | `implemented_full_route` with unknown-outcome diagnostics and cleanup tests. |
| `runtime.database_engine_agent.lifecycle` | `implemented_engine_internal` plus health/status route tests and no-authority-bypass gates. |
| `storage.buffer_page_cache_checkpoint.lifecycle` | `implemented_engine_internal` plus storage/recovery/shutdown tests proving no finality substitution. |
| `security.configuration_policy_provider.lifecycle` | `implemented_full_route` for reload/admission surfaces and `implemented_engine_internal` for cache/provider substeps. |
| `storage.backup_archive_restore_shadow_snapshot.lifecycle` | `implemented_full_route` with hold/blocker/refusal tests and no live-file shortcut gates. |
| `runtime.udr_extension.lifecycle` | `implemented_full_route` where user/admin surfaced; internal unload/quiesce substeps may use `implemented_engine_internal`. |
| `runtime.workload_resource_quota.lifecycle` | `implemented_full_route` for admission/refusal behavior and internal quota updates. |
| `catalog.schema_object_dependency.lifecycle` | `implemented_full_route` through DDL/API routes with MGA/catalog/authorization tests. |
| `catalog.system_physical_index_profiles` | `implemented_engine_internal` plus static/catalog conformance gates. |
| `catalog.sys_information_projection` | `implemented_full_route` for client metadata reads plus projection/static gates. |
| `storage.index_statistics_plan.lifecycle` | `implemented_full_route` where admin/user surfaced; internal build/recovery substeps may use `implemented_engine_internal`. |
| `runtime.lock_wait_deadlock.lifecycle` | `implemented_engine_internal` plus transaction/session/cancel/deadlock/shutdown tests. |
| `runtime.temp_spill_workspace.lifecycle` | `implemented_engine_internal` plus commit/rollback/disconnect/shutdown/recovery tests. |
| `runtime.event_notification_subscription.lifecycle` | `implemented_full_route` with authorization, ordering, disconnect, shutdown, and recovery tests. |
| `security.encryption_key_protected_material.lifecycle` | `implemented_full_route` for key admission/encrypted open and internal purge/redaction tests. |
| `resources.seed_i18n.lifecycle` | `implemented_engine_internal` plus upgrade/refusal/cache/index-dependency tests. |
| `mga.garbage_collection_retention.lifecycle` | `implemented_engine_internal` plus MGA cleanup/sweep/retention tests. |
| `runtime.background_jobs_scheduler.lifecycle` | `implemented_engine_internal` plus health/admin/status and shutdown drain tests. |
| `cluster.boundary_fail_closed.lifecycle` | `canonical_cluster_private` with standalone fail-closed tests. |
| `security.principal_privilege_policy.lifecycle` | `implemented_full_route` through security/admin/DDL routes with MGA/audit/cache tests. |
| `storage.allocation_freespace_pagemap.lifecycle` | `implemented_engine_internal` plus storage/filespace/recovery tests. |
| `runtime.executable_database_object.lifecycle` | `implemented_full_route` through DDL/SBLR/UDR routes and permission/dependency tests. |
| `runtime.sequence_generator.lifecycle` | `implemented_full_route` through DDL/SBLR routes and persistence/recovery tests. |
| `observability.operational_evidence_supportability.lifecycle` | `implemented_full_route` for admin/support routes and internal log/audit flush tests. |
| `runtime.capability_profile_feature_gate.lifecycle` | `implemented_full_route` for package/profile/edition-gate admission and downgrade refusal tests. |
| `replication.changefeed_boundary.lifecycle` | `canonical_cluster_private` or `canonical_fail_closed` until canonical replication authority exists. |
| `lifecycle.existing_implementation_reconciliation` | `implemented_full_route` or `implemented_engine_internal` per reconciled surface, with audit proving zero legacy-contract drift. |
| `lifecycle.protocol_persisted_format_versioning` | `implemented_full_route` or `implemented_engine_internal` per protocol/format with unknown/downgrade/migration tests. |
| `lifecycle.admin_cli_command_surface` | `implemented_full_route` or `canonical_no_user_command` for each operation. |
| `lifecycle.packaging_service_runtime` | `implemented_full_route` with package/service/runtime-directory tests. |
| `lifecycle.spec_to_test_traceability` | `implemented_engine_internal` as generator/static gate proving complete mapping. |
| `lifecycle.upgrade_migration_refusal` | `implemented_engine_internal` plus open/reopen/format/migration/refusal tests. |
| `lifecycle.security_threat_model_gate` | `implemented_engine_internal` as threat-model and abuse-case gates. |
| `lifecycle.donor_mapping` | `implemented_full_route` for accepted mappings or `canonical_fail_closed` for emulated/non-file diagnostics. |
| `lifecycle.observability_diagnostics` | `implemented_full_route` or `implemented_engine_internal` per surface, with diagnostics, metrics, audit, and cache invalidation tests. |
| `lifecycle.exhaustive_regression` | `implemented_engine_internal` as generated CTest/static coverage. |
| `lifecycle.hardening_fault_injection` | `implemented_engine_internal` as fault injection and authority-drift gates. |
| `lifecycle.release_declaration` | `implemented_engine_internal` as final audit and no-overclaim release evidence. |

## Full-Route Requirements

Any lifecycle operation exposed to a client, CLI, admin surface, parser profile,
donor mapping, manager, listener, or IPC route must prove:

1. Client or command input reaches only the intended listener/parser/IPC/server
   route.
2. Parser surfaces lower lifecycle commands to ScratchBird lifecycle SBLR/API
   operations or return exact emulated/non-file diagnostics.
3. Parser, listener, and manager surfaces do not authenticate, authorize,
   execute SQL, own transaction finality, read/write database files, or bypass
   server/engine admission.
4. Server and IPC surfaces authenticate, authorize, frame-validate,
   backpressure-check, and route by database scope.
5. Engine owns authentication, authorization, SBLR/internal execution, policy,
   transaction, catalog, storage, and lifecycle decisions.
6. Denials return canonical message vectors through the route back to the
   client or command surface.
7. Database-scoped shutdown/drop/maintenance/repair commands cannot affect
   unrelated database associations.

## Durable Evidence Requirements

No success path is complete until required evidence is durable or explicitly
runtime-only by canonical rule.

| Evidence type | Required for |
| --- | --- |
| `database_uuid` | Create, open, attach, route association, shutdown, drop, diagnostics, audit, and tests. |
| `filespace_uuid` | Every filespace, including the first physical database file. |
| `transaction_1` | Durable create bootstrap and seed evidence. |
| `transaction_2` | First successful open/session activation and runtime activation evidence. |
| `final_shutdown_transaction` | Clean shutdown success and reopen-clean classification. |
| `owner_token` | Exclusive ownership, open/reopen, shutdown, drop, and stale owner refusal. |
| `policy_generation` | Default policy seed, attach/admission, lifecycle operations, reload, stale refusal, and cache invalidation. |
| `catalog_generation` | Catalog/object/dependency lifecycle, sys.catalog/sys.information visibility, metadata cache invalidation, and DDL closure. |
| `resource_epoch` | Resource seed, collation/charset/timezone/locale, quota, index dependency, and runtime cache behavior. |
| `route_generation` | Listener/parser/IPC/server/session association, shutdown notification, parser fallback, and stale route refusal. |
| `shutdown_generation` | Shutdown notification, acknowledgement, drain, force termination, and finalization. |
| `audit_pointer` | Security-sensitive lifecycle action, refusal, repair, drop, force shutdown, support bundle, and policy override. |

## Authority Drift Rejection Rules

The coordinator must reject a slice if any patch or spec text:

- Replaces MGA transaction finality with WAL, redo, undo, donor log, parser
  state, cache state, checkpoint state, file timestamp, UUID order, wall-clock
  order, route acknowledgement, or job status.
- Performs rollback outside engine MGA methods.
- Lets parser, listener, manager, driver, donor adapter, tool, UDR, or IPC glue
  decide authentication, authorization, transaction finality, storage identity,
  catalog visibility, policy authority, or SBLR execution semantics.
- Treats human-facing SQL names as durable identity instead of UUID and identity
  resolver authority.
- Stores human-facing SQL object names or localized display text in base
  `sys.catalog` tables.
- Lets `sys.information` own catalog identity, transaction visibility, policy,
  or write authority.
- Uses UUIDv7 ordering as commit order, creation order, history order, range
  authority, or transaction finality.
- Allows a cluster route, cluster schema, cluster metric, cluster agent, cluster
  transaction, or cluster lifecycle path in standalone execution without
  canonical cluster authority.
- Allows unregistered or stale lifecycle protocol or persisted-format versions
  to be interpreted silently.
- Overclaims release support without tests and final audit evidence.

## Validation Evidence Required

P0A itself is a document artifact, so validation is structural. Dependent slices
must add executable validation through P0C and later implementation work.

Required P0A validation:

1. The state model artifact exists and is non-empty.
2. The no-defer lifecycle contract exists and is non-empty.
3. The two artifacts name canonical destinations for states, operation
   families, transition requirements, and refusal classes.
4. The two artifacts do not require edits outside the DBLC execution_plan artifact
   ownership scope.
5. Markdown formatting passes repository whitespace checks.

Required dependent-slice validation:

1. P0C materializes CTest/static/audit commands for every lifecycle label.
2. P13R traceability proves every state, transition, diagnostic, invalid
   transition, operation family, and route maps to CTest or static gates.
3. P16 exhaustive regression proves every lifecycle operation, state transition,
   invalid transition, datatype/resource/security variation, and route.
4. P17 authority-drift gates prove no WAL, parser-finality, donor-authority, or
   cluster-path drift.
5. P18 final audit proves zero open lifecycle spec, implementation, registry,
   diagnostic, and test gaps.

## P0A Draft Readiness Statement

These two files are draft/preparatory artifacts only. They can be submitted to
the coordinator when they exist, remain inside the assigned artifact ownership
scope, and give every known lifecycle state, operation family, transition
requirement, and refusal class a canonical destination and a non-placeholder
closure path.

`DBLC-000A` is not closed by these files. The coordinator must first close
`DBLC-000`, reconcile this draft against the authority map and baseline
inventory, run required validation, and explicitly update the tracker and
acceptance gate before P0A can be marked passed.

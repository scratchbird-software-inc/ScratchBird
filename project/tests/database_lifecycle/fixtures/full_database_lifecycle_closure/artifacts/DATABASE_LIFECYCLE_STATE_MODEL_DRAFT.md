# Database Lifecycle State Model Draft

Status: P0A draft/preparatory artifact; not closure evidence until `DBLC-000` prerequisite closure and coordinator validation
Owning slice: `DBLC-000A`
Acceptance gate: `DBLC_P0A_NO_DEFER_READY`
Search key: `DATABASE-LIFECYCLE-STATE-MODEL-DRAFT`

## Purpose

This artifact gives the lifecycle execution_plan a shared state, operation, transition,
and refusal vocabulary before canonical contract edits and implementation
slices begin.

`DBLC-000A` depends on `DBLC-000`. This draft is preparatory evidence only and
must not be used to mark `DBLC-000A` or `DBLC_P0A_NO_DEFER_READY` passed before
`DBLC-000` is closed and the coordinator validates this artifact against the
authority map and baseline inventory.

This file is execution-control evidence only. Product behavior must be added to
manifest-listed canonical contracts, accepted decision records,
registries, or implementation packets before any implementation slice can close.

## Authority Rules

- Canonical behavior belongs under `public_release_evidence`.
- Expanded implementation behavior belongs under
  `public_release_evidence`.
- This artifact may name required states, transitions, and refusal classes, but
  it does not make them product behavior.
- ScratchBird MGA remains the transaction, visibility, rollback, recovery,
  cleanup, and finality authority.
- WAL, redo logs, undo logs, reference logs, parser state, file timestamps,
  wall-clock ordering, UUID ordering, cache state, checkpoint state, or route
  state must not become finality authority.
- Engine-owned authentication and authorization are authoritative. Parsers,
  listeners, managers, drivers, tools, and reference adapters only relay lifecycle
  requests and render responses unless a canonical spec explicitly assigns them
  non-authoritative lifecycle duties.
- Cluster lifecycle states and transitions fail closed in standalone execution
  until canonical cluster mapping and cluster transaction authority exist.

## Canonical Destination Map

Every state, transition, operation family, and refusal class in this draft must
land in one or more of these destinations before closure.

| Destination key | Canonical destination | Required content |
| --- | --- | --- |
| `canonical.storage_mga` | `public_contract_snapshot` and `public_contract_snapshot` | Durable database lifecycle state, tx1, tx2, final shutdown transaction, transaction inventory, recovery, rollback, cleanup, lock/wait interaction, and MGA finality rules. |
| `canonical.server_ipc_config` | `public_contract_snapshot` | Server, listener, parser IPC, management IPC, configuration epoch, route, owner token, drain, shutdown, and process association rules. |
| `canonical.security_audit` | `public_contract_snapshot` | Engine-owned authentication, authorization, policy, audit, redaction, security-provider, role, privilege, and protected-material lifecycle behavior. |
| `canonical.resources` | `public_contract_snapshot` | Resource seed, timezone, charset, collation, locale, versioning, epoch, cache, and upgrade/refusal behavior. |
| `canonical.ops_observability` | `public_contract_snapshot` | Operational modes, maintenance, verify, repair, health, metrics, support bundle, package/service/runtime, diagnostics, and release declaration behavior. |
| `packet.engine_lifecycle` | `public_input_snapshot` or accepted successor packet | Implementation-ready database lifecycle operation contracts, state transitions, evidence fields, diagnostic routes, and conformance anchors. |
| `packet.startup_open_filespace` | `public_input_snapshot` | Startup/open ordering, first filespace identity, ownership, filespace state coupling, recovery classification, verify, repair, and shutdown evidence. |
| `packet.sb_server` | `public_input_snapshot` | Hosted database supervision, shared/dedicated server isolation, service-ready, graceful stop, force stop, restart, failure, quarantine, and database-scope routing. |
| `packet.ipc` | `public_input_snapshot` | Parser-server IPC, management IPC, endpoint descriptor, frame, authentication, authorization, backpressure, drain, stale cleanup, failure, and quarantine lifecycle. |
| `packet.configuration` | `public_input_snapshot` | Configuration sources, epochs, policy reload, stale refusal, cache invalidation, and generated policy authority interaction. |
| `packet.security` | `public_input_snapshot` | Security provider, password-hash verification, principal/role/privilege/policy, engine authorization, audit, and redaction implementation guidance. |
| `packet.default_policy` | `public_input_snapshot` plus `public_contract_snapshot` | Lifecycle policy families, properties, default values, overrides, tx1 seed rows, policy generations, generated seed data, and policy diagnostics. |
| `packet.backup_archive` | `public_input_snapshot` | Backup, archive, restore, shadow, snapshot, hold/blocker, legal retention, restore-inspection open, shutdown/drop interaction, and no live-file shortcut behavior. |
| `packet.udr` | `public_input_snapshot` | UDR/extension registration, load, unload/quiesce, parser-support UDR rules, resource limits, shutdown cleanup, and authority boundaries. |
| `packet.events` | `public_input_snapshot` | LISTEN/NOTIFY, subscriptions, delivery ordering, security filtering, disconnect cleanup, shutdown, and recovery behavior. |
| `packet.indexes_optimizer` | `public_input_snapshot` and `public_input_snapshot` | Index, statistics, optimizer plan, collation/charset epoch, plan cache, interrupted build recovery, and catalog/MGA consistency behavior. |
| `packet.mga_transactions` | `public_input_snapshot` | MGA transaction cleanup, sweep, retention, limbo, unknown outcome, backup/archive holds, locks, waits, and shutdown cleanup behavior. |
| `packet.managers_listeners_parsers` | `public_input_snapshot`, `public_input_snapshot`, `public_input_snapshot`, `public_input_snapshot`, `public_input_snapshot`, and `public_input_snapshot` | Manager-family, listener-family, parser-family, control-plane, parser-pool, handoff, drain, force stop, failure, quarantine, and process association behavior. |
| `registry.sblr_api` | `public_contract_snapshot`, `public_contract_snapshot`, and `project/src/engine/internal_api/ENGINE_API_SURFACE_REGISTRY.yaml` | Lifecycle SBLR/API operation rows, dispatch mapping, request/result contracts, unsupported/refusal behavior, and full-route coverage anchors. |
| `registry.diagnostics` | `public_contract_snapshot` and `public_contract_snapshot` | Message-vector diagnostics, severity, retryability, redaction, audit policy, owner metadata, and parser-rendering shapes for lifecycle refusals and failures. |
| `conformance.lifecycle` | `VALIDATION_PLAN.md`, P0C generated commands, CTest labels, and static gates | Generated coverage for every state, transition, invalid transition, operation family, refusal class, diagnostic, and route path. |

## State Namespace Rules

State names use lower snake case and are stable identifiers for traceability.
Names may be refined in canonical specs, but each refined name must preserve an
alias or migration note until downstream tests and registries are updated.

| Namespace | Meaning | Destination keys |
| --- | --- | --- |
| `database.*` | Durable database lifecycle and operational mode state. | `canonical.storage_mga`, `canonical.ops_observability`, `packet.engine_lifecycle`, `packet.startup_open_filespace` |
| `filespace.*` | Durable filespace identity, attachment, mutability, recovery, verify, repair, and drop state. | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `ownership.*` | Owner-token, process ownership, exclusive open, stale owner, and ambiguous-owner state. | `canonical.storage_mga`, `canonical.server_ipc_config`, `packet.sb_server`, `packet.startup_open_filespace` |
| `recovery.*` | Startup classification, recovery-required, recovering, repaired, quarantined, and fail-open-refusal state. | `canonical.storage_mga`, `packet.startup_open_filespace`, `packet.mga_transactions` |
| `route.*` | Client, SBWP/TLS, listener, parser, IPC, server, engine, and response route state. | `canonical.server_ipc_config`, `packet.ipc`, `packet.managers_listeners_parsers`, `packet.sb_server` |
| `process.*` | Manager, listener, parser, server daemon, and IPC endpoint lifecycle state. | `canonical.server_ipc_config`, `packet.managers_listeners_parsers`, `packet.sb_server`, `packet.ipc` |
| `session.*` | Attachment, session, request, statement, cursor, cancel, timeout, disconnect, drain, and cleanup state. | `canonical.server_ipc_config`, `canonical.security_audit`, `packet.engine_lifecycle`, `registry.sblr_api` |
| `transaction.*` | Admission, active, committing, rolling back, unknown outcome, finality evidence, and cleanup state. | `canonical.storage_mga`, `packet.mga_transactions` |
| `policy.*` | Default policy seed, generation, reload, stale policy, override, cache invalidation, and fail-closed state. | `canonical.security_audit`, `packet.default_policy`, `packet.configuration`, `registry.diagnostics` |
| `catalog.*` | Catalog object, dependency, identity resolver, sys.catalog, sys.information, metadata cache, and visibility state. | `canonical.storage_mga`, `registry.sblr_api`, `conformance.lifecycle` |
| `resource.*` | Timezone, charset, collation, locale, seed version, runtime epoch, quota, and resource-pool state. | `canonical.resources`, `canonical.ops_observability` |
| `mode.*` | Maintenance, restricted-open, diagnostic, verify, repair, safe mode, shutdown, and quarantine mode state. | `canonical.ops_observability`, `packet.engine_lifecycle` |
| `diagnostic.*` | Message-vector, audit, metrics, redaction, retryability, and supportability state. | `canonical.ops_observability`, `registry.diagnostics` |

## Durable Database Lifecycle States

| State | Meaning | Entry evidence | Permitted next states | Destination keys |
| --- | --- | --- | --- | --- |
| `database.absent` | No admitted ScratchBird database exists at the target locator. | Missing or refused locator evidence. | `database.creating_tx1`, `database.open_refused`, `database.dropped` | `canonical.storage_mga`, `packet.engine_lifecycle` |
| `database.creating_tx1` | Create is admitted and tx1 durable bootstrap is in progress. | Create operation id, generated database UUIDv7, generated first filespace UUIDv7, owner token, tx1 transaction id, bootstrap manifest seed plan. | `database.closed_created`, `database.create_failed_recoverable`, `database.quarantined` | `canonical.storage_mga`, `packet.default_policy`, `packet.startup_open_filespace` |
| `database.create_failed_recoverable` | Startup found incomplete or interrupted create evidence that can be completed, rolled back, or quarantined by MGA evidence. | Partial tx1 evidence, operation id, recovery classification, refusal diagnostics. | `recovery.classifying`, `database.quarantined`, `database.absent` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `database.closed_created` | Create tx1 committed and no first open/session activation has committed tx2. | Tx1 committed, clean closed-created marker, first filespace active-primary registration, generated policy/resource/security/catalog seed evidence. | `database.opening`, `database.dropping`, `database.verify_read_only` | `canonical.storage_mga`, `packet.engine_lifecycle` |
| `database.opening` | Open/reopen admitted and startup classification, ownership, format, policy, catalog, and filespace validation are in progress. | Owner token candidate, startup state record, format versions, filespace manifest, catalog/policy/resource epochs. | `database.first_open_activating_tx2`, `database.online`, `mode.restricted_open`, `mode.maintenance`, `recovery.required`, `database.quarantined`, `database.open_refused` | `canonical.storage_mga`, `canonical.server_ipc_config`, `packet.startup_open_filespace` |
| `database.first_open_activating_tx2` | First successful open/session activation is committing tx2 and starting database-local runtime state. | Tx2 transaction id, first admitted session id, agent/cache/IPC/server activation evidence. | `database.online`, `database.activation_failed_recoverable`, `database.quarantined` | `canonical.storage_mga`, `packet.engine_lifecycle`, `packet.sb_server` |
| `database.activation_failed_recoverable` | Tx2 or runtime activation was interrupted and requires startup classification before user work. | Partial tx2 evidence, runtime activation evidence, recovery diagnostics. | `recovery.classifying`, `database.quarantined`, `database.open_refused` | `canonical.storage_mga`, `packet.engine_lifecycle` |
| `database.online` | Normal admitted database operation is available. | Valid owner token, clean startup classification, required filespaces active, security/policy/catalog/resource epochs loaded, route ready. | `mode.maintenance_entering`, `mode.restricted_entering`, `mode.diagnostic_read_only`, `mode.verify_running`, `mode.repair_planning`, `database.shutdown_fencing`, `database.dropping`, `database.failed` | `canonical.storage_mga`, `packet.engine_lifecycle` |
| `mode.maintenance_entering` | Maintenance request admitted and new ordinary work is being fenced. | Authorized command id, drain plan, policy generation, audit record. | `mode.maintenance`, `database.shutdown_fencing`, `mode.entry_refused` | `canonical.ops_observability`, `packet.engine_lifecycle` |
| `mode.maintenance` | Maintenance mode is active; only maintenance-authorized work is admitted. | Durable or runtime mode marker as specified, write fence, admitted actors, metrics/audit. | `mode.maintenance_exiting`, `mode.verify_running`, `mode.repair_planning`, `database.shutdown_fencing`, `database.dropping` | `canonical.ops_observability`, `packet.engine_lifecycle` |
| `mode.maintenance_exiting` | Maintenance exit validation is in progress. | Exit command id, verification result, policy decision, audit record. | `database.online`, `mode.maintenance`, `mode.exit_refused` | `canonical.ops_observability`, `packet.engine_lifecycle` |
| `mode.restricted_entering` | Restricted-open transition is fencing operations and validating permitted read/diagnostic/repair behavior. | Restricted-open command id, policy decision, recovery/diagnostic reason. | `mode.restricted_open`, `mode.entry_refused`, `database.shutdown_fencing` | `canonical.ops_observability`, `packet.engine_lifecycle` |
| `mode.restricted_open` | Restricted or read-only open mode is active. | Mode marker, permitted operation set, read/write fence, policy generation, audit record. | `mode.restricted_exiting`, `mode.verify_running`, `mode.repair_planning`, `database.shutdown_fencing`, `database.quarantined` | `canonical.ops_observability`, `packet.engine_lifecycle` |
| `mode.restricted_exiting` | Restricted-open exit validation is in progress. | Exit command id, verification result, policy decision, audit record. | `database.online`, `mode.maintenance`, `mode.restricted_open`, `mode.exit_refused` | `canonical.ops_observability`, `packet.engine_lifecycle` |
| `mode.diagnostic_read_only` | Redacted diagnostic status/reporting is active without mutation authority. | Diagnostic command id, snapshot/evidence pointer, redaction policy. | Previous admitted mode, `database.shutdown_fencing` | `canonical.ops_observability`, `registry.diagnostics` |
| `mode.verify_running` | Structural, catalog, security, resource, transaction inventory, page/filespace, and name/UUID verification is active. | Verify command id, target scope, evidence sink, bounded-memory plan. | Previous admitted mode, `mode.repair_planning`, `database.quarantined` | `canonical.ops_observability`, `packet.startup_open_filespace` |
| `mode.repair_planning` | Authorized repair is preparing an explicit repair plan while preserving evidence. | Repair authorization, verify findings, repair plan id, evidence preservation pointer. | `mode.repair_running`, previous admitted mode, `database.quarantined`, `mode.repair_refused` | `canonical.ops_observability`, `packet.startup_open_filespace` |
| `mode.repair_running` | Bounded repair is executing under engine authority. | Repair plan id, operation evidence, affected filespace/catalog/page ranges, audit record. | Previous admitted mode, `recovery.required`, `database.quarantined`, `mode.repair_failed` | `canonical.ops_observability`, `canonical.storage_mga` |
| `database.shutdown_fencing` | Shutdown is admitted and new work is fenced for the target database scope. | Shutdown command id, force flag, target database UUID, route generation, policy drain window. | `database.shutdown_notifying`, `database.shutdown_refused` | `canonical.server_ipc_config`, `packet.engine_lifecycle`, `packet.sb_server` |
| `database.shutdown_notifying` | Database-associated clients, sessions, managers, listeners, parsers, IPC daemons, and server daemons are being notified. | Association registry snapshot, notification generation, message-vector plan. | `database.shutdown_draining`, `database.shutdown_forcing`, `database.shutdown_refused` | `canonical.server_ipc_config`, `packet.managers_listeners_parsers`, `packet.ipc` |
| `database.shutdown_draining` | Non-force shutdown is waiting for acknowledgements and graceful commit/rollback/disconnect. | Acknowledgement set, drain deadline, active session list, transaction outcome tracking. | `database.shutdown_finalizing`, `database.shutdown_forcing`, `database.shutdown_refused` | `canonical.server_ipc_config`, `packet.sb_server`, `packet.mga_transactions` |
| `database.shutdown_forcing` | Explicit force shutdown is terminating associated runtime components for the target database scope. | Explicit force command, target association set, termination evidence, isolation evidence. | `database.shutdown_finalizing`, `database.quarantined` | `canonical.server_ipc_config`, `packet.managers_listeners_parsers`, `packet.sb_server` |
| `database.shutdown_finalizing` | Agents, threads, cache, filespace state, and lifecycle evidence are being persisted before external success. | Final transaction id, checkpoint/cache flush evidence, agent stop evidence, ownership release plan. | `database.closed_clean`, `database.quarantined` | `canonical.storage_mga`, `packet.engine_lifecycle`, `packet.startup_open_filespace` |
| `database.closed_clean` | Clean shutdown is durable and ownership is released. | Final lifecycle transaction committed, clean marker, owner token released, route state stopped. | `database.opening`, `database.dropping`, `database.verify_read_only` | `canonical.storage_mga`, `packet.engine_lifecycle` |
| `database.closed_unclean` | Startup found that no clean final lifecycle transaction exists. | Startup state, missing/dirty clean-close evidence, transaction inventory pointer. | `recovery.classifying`, `database.quarantined`, `database.open_refused` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `recovery.required` | Startup or operation determined that recovery classification must run before ordinary access. | Dirty/ambiguous/corrupt/incompatible evidence and recovery reason code. | `recovery.classifying`, `database.quarantined`, `database.open_refused` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `recovery.classifying` | Startup recovery is classifying transaction, filespace, catalog, resource, and runtime evidence. | Classification operation id, bounded-memory plan, evidence set. | `recovery.running`, `mode.restricted_open`, `database.quarantined`, `database.open_refused` | `canonical.storage_mga`, `packet.mga_transactions` |
| `recovery.running` | Recovery is completing, rolling back, or fencing interrupted work under MGA authority. | Recovery action plan, idempotency key, transaction inventory evidence. | `database.online`, `mode.restricted_open`, `database.closed_clean`, `database.quarantined`, `database.open_refused` | `canonical.storage_mga`, `packet.mga_transactions` |
| `database.quarantined` | Database access is isolated because evidence is unsafe, ambiguous, corrupt, unsupported, or requires operator review. | Quarantine reason, evidence pointer, diagnostic vector, allowed inspection policy. | `mode.diagnostic_read_only`, `mode.repair_planning`, `database.dropping`, `database.open_refused` | `canonical.storage_mga`, `canonical.ops_observability`, `registry.diagnostics` |
| `database.dropping` | Drop is admitted and safe local removal or quarantine-by-policy is in progress. | Drop command id, owner proof, closed/fenced state, backup/repair/cluster/blocker checks, target filespace set. | `database.dropped`, `database.drop_quarantined`, `database.drop_refused` | `canonical.storage_mga`, `packet.engine_lifecycle`, `packet.startup_open_filespace` |
| `database.drop_quarantined` | Drop could not safely remove all local storage and policy requires quarantine evidence. | Drop operation id, remaining storage evidence, quarantine reason, diagnostic vector. | `database.dropped`, `database.quarantined`, `database.drop_refused` | `canonical.storage_mga`, `canonical.ops_observability` |
| `database.dropped` | Drop completed according to policy and no ordinary lifecycle operation may target the database UUID. | Drop evidence, removed/quarantined storage manifest, audit record. | `database.absent` | `canonical.storage_mga`, `packet.engine_lifecycle` |
| `database.open_refused` | Open or route admission failed before user-visible work. | Message vector, refusal class, evidence pointer, retryability. | Prior durable state | `registry.diagnostics`, `packet.engine_lifecycle` |
| `database.shutdown_refused` | Shutdown was denied or could not begin safely. | Message vector, refusal class, association evidence, policy decision. | Prior admitted mode | `registry.diagnostics`, `packet.engine_lifecycle` |
| `database.drop_refused` | Drop was denied because the state, ownership, blockers, or policy made removal unsafe. | Message vector, blocker inventory, policy decision. | Prior durable state | `registry.diagnostics`, `packet.engine_lifecycle` |
| `database.failed` | Runtime failure occurred and must be classified before further ordinary work. | Failure vector, component evidence, transaction outcome status. | `recovery.required`, `database.quarantined`, `database.shutdown_fencing` | `canonical.storage_mga`, `canonical.ops_observability` |

## Filespace Lifecycle States

| State | Meaning | Entry evidence | Permitted next states | Destination keys |
| --- | --- | --- | --- | --- |
| `filespace.absent` | No durable filespace identity exists for the target locator or registry key. | Missing registry/header/manifest/catalog evidence. | `filespace.registering`, `filespace.attach_refused` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.registering` | Filespace UUIDv7 identity, header, startup directory entry, manifest, and catalog row are being made durable. | Generated filespace UUIDv7, database UUID, role, locator, physical filespace id, tx evidence. | `filespace.active_primary`, `filespace.active_secondary`, `filespace.registration_failed` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.active_primary` | Filespace is the unique active primary filespace for the database. | Header/manifest/catalog consistency, active-primary uniqueness, page size, generation. | `filespace.read_only`, `filespace.read_write`, `filespace.verify_running`, `filespace.repair_running`, `filespace.moving`, `filespace.detaching`, `filespace.quarantined`, `filespace.dropping` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.active_secondary` | Filespace is an active non-primary filespace associated with the database. | Header/manifest/catalog consistency, role, generation, owner capability. | `filespace.read_only`, `filespace.read_write`, `filespace.promoting`, `filespace.demoting`, `filespace.verify_running`, `filespace.repair_running`, `filespace.moving`, `filespace.detaching`, `filespace.quarantined`, `filespace.dropping` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.read_write` | Writes are admitted according to database mode, policy, and transaction rules. | Mutability marker, policy generation, cache/flush state. | `filespace.read_only`, `filespace.maintenance_fenced`, `filespace.verify_running`, `filespace.repair_running`, `filespace.dropping` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.read_only` | Writes are fenced; reads/diagnostics are admitted by policy. | Mutability marker, reason, audit record. | `filespace.read_write`, `filespace.maintenance_fenced`, `filespace.verify_running`, `filespace.repair_running`, `filespace.dropping` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.maintenance_fenced` | Filespace mutation is fenced while maintenance/restricted/verify/repair activity runs. | Database mode, fence generation, admitted operation set. | `filespace.read_write`, `filespace.read_only`, `filespace.verify_running`, `filespace.repair_running`, `filespace.quarantined` | `canonical.storage_mga`, `canonical.ops_observability` |
| `filespace.verify_running` | Header, manifest, catalog, page, and identity verification is active. | Verify operation id, evidence sink, bounded-memory plan. | Previous mutability state, `filespace.repair_planned`, `filespace.quarantined` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.repair_planned` | Repair plan is authorized and evidence is preserved before mutation. | Repair plan id, findings, authorization, evidence pointer. | `filespace.repair_running`, previous mutability state, `filespace.quarantined` | `canonical.storage_mga`, `canonical.ops_observability` |
| `filespace.repair_running` | Bounded filespace repair is executing under engine authority. | Repair operation id, affected identity/page/manifest/catalog scope, audit record. | Previous mutability state, `filespace.quarantined`, `recovery.required` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.attaching` | Existing secondary filespace is being attached or reattached. | Candidate UUID, header, manifest, catalog row, owner capability. | `filespace.active_secondary`, `filespace.attach_refused`, `filespace.quarantined` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.detaching` | Filespace is being detached after pins, cache, transactions, and catalog dependencies are cleared. | Detach operation id, pin inventory, dependency checks, flush evidence. | `filespace.detached`, `filespace.detach_refused`, `filespace.quarantined` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.detached` | Filespace identity is retained but not available for ordinary database access. | Durable detached marker, manifest/catalog state, audit record. | `filespace.attaching`, `filespace.dropping`, `filespace.quarantined` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.promoting` | Secondary-to-primary role change is validating uniqueness and durability. | Promote operation id, active-primary check, dependency/capability evidence. | `filespace.active_primary`, `filespace.promote_refused`, `filespace.quarantined` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.demoting` | Primary-to-secondary role change is validating replacement primary authority. | Demote operation id, replacement primary evidence, dependency/capability evidence. | `filespace.active_secondary`, `filespace.demote_refused`, `filespace.quarantined` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.moving` | Locator/path move is in progress under identity preservation rules. | Move operation id, old/new locator, owner proof, manifest update evidence. | Previous active state, `filespace.move_refused`, `filespace.quarantined` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.compacting` | Compact operation is moving/reclaiming pages under filespace identity and page-map authority. | Compact operation id, page-map evidence, transaction horizon, bounded-memory plan. | Previous active state, `filespace.compact_refused`, `filespace.quarantined` | `canonical.storage_mga`, `canonical.ops_observability` |
| `filespace.truncating` | Truncate operation is reclaiming physical storage under safe horizon and page-map authority. | Truncate operation id, page-map evidence, horizon proof, cache flush evidence. | Previous active state, `filespace.truncate_refused`, `filespace.quarantined` | `canonical.storage_mga`, `canonical.ops_observability` |
| `filespace.dropping` | Filespace removal or quarantine is admitted and in progress. | Drop operation id, closed/detached proof, pin/dependency checks, policy decision. | `filespace.dropped`, `filespace.drop_quarantined`, `filespace.drop_refused` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.drop_quarantined` | Drop could not safely remove storage and policy requires quarantine. | Drop evidence, remaining locator evidence, diagnostic vector. | `filespace.dropped`, `filespace.quarantined` | `canonical.storage_mga`, `canonical.ops_observability` |
| `filespace.dropped` | Filespace lifecycle ended according to policy. | Drop evidence, catalog/manifest/header retirement, audit record. | `filespace.absent` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.quarantined` | Filespace is isolated from ordinary reads/writes because identity or structure is unsafe. | Quarantine reason, evidence pointer, diagnostic vector. | `filespace.verify_running`, `filespace.repair_planned`, `filespace.dropping` | `canonical.storage_mga`, `registry.diagnostics` |
| `filespace.registration_failed` | Registration was interrupted before durable success and must be classified. | Partial registration evidence, operation id, diagnostic vector. | `recovery.classifying`, `filespace.quarantined`, `filespace.absent` | `canonical.storage_mga`, `packet.startup_open_filespace` |
| `filespace.attach_refused` | Attach/reattach failed closed. | Message vector, identity evidence, policy/ownership decision. | Prior state | `registry.diagnostics`, `packet.startup_open_filespace` |
| `filespace.detach_refused` | Detach failed closed. | Message vector, active pin/dependency/ownership evidence. | Prior state | `registry.diagnostics`, `packet.startup_open_filespace` |
| `filespace.drop_refused` | Drop failed closed. | Message vector, blocker inventory, policy decision. | Prior state | `registry.diagnostics`, `packet.startup_open_filespace` |

## Process, Route, and Session States

These states are first-class lifecycle surfaces. They are not substitutes for
durable database or transaction finality.

| State | Meaning | Required closure |
| --- | --- | --- |
| `process.absent` | No process or endpoint is associated with the target database scope. | Association registry must distinguish absent from stale or ambiguous. |
| `process.starting` | Manager, listener, parser, server daemon, or IPC endpoint is launching and validating configuration. | Owner token, descriptor, package/profile, capability, and policy evidence are required. |
| `process.ready` | Component is ready for admitted lifecycle participation. | Service-ready cannot be published until database association and route scope are valid. |
| `process.associated` | Component is linked to a database UUID, route generation, heartbeat generation, or shutdown generation. | Association evidence must be discoverable by the engine for target-database shutdown and listener-failure parser fallback. |
| `process.active` | Component is performing admitted work. | Work must be routed through server/engine authority and cannot own transaction, security, catalog, or storage finality. |
| `process.draining` | Component is refusing new work and closing admitted work for a target database scope. | Drain must be database-scoped and cannot affect unrelated database associations. |
| `process.stopping` | Component is stopping cleanly. | Clean-stop acknowledgement and resource release evidence are required. |
| `process.force_stopping` | Component is being terminated by explicit force lifecycle command. | Force flag, target scope, termination evidence, and isolation proof are required. |
| `process.stopped` | Component is stopped and no longer routable. | Stale endpoints, PID/owner files, and descriptors must be cleaned or fenced. |
| `process.failed` | Component failed and requires classification. | Failure must route to retry, quarantine, restart policy, or database refusal with diagnostics. |
| `process.quarantined` | Component is unsafe and isolated from route/control paths. | Quarantine reason, evidence pointer, and release/repair policy are required. |
| `route.pre_auth` | Client route exists before engine authentication is complete. | Parser/listener may relay credentials only; engine remains authentication authority. |
| `route.authenticating` | Engine authentication is in progress. | Password-hash or provider checks remain engine-owned and cleartext is not stored or compared. |
| `route.authorizing` | Engine authorization and policy checks are in progress. | Authorization and policy generations must be captured. |
| `route.admitted` | Client/session route is admitted to engine work. | Session identity, attachment identity, policy/catalog/security/resource epochs, and transaction admission prerequisites are captured. |
| `route.refused` | Route admission denied with message vector. | Refusal class, retryability, redaction, and parser rendering must be registered. |
| `route.draining` | Route is closing during detach or shutdown. | Commit/rollback/unknown-outcome behavior must preserve MGA evidence. |
| `route.disconnected` | Route is closed. | Cleanup evidence for parser, IPC, session, cursor, notifications, and cache is required. |
| `session.creating` | Session or attachment is being created. | Engine-owned auth, authorization, catalog/security/policy/resource epoch checks required. |
| `session.attached` | Attachment/session is active. | Attachment UUID/session identity and database UUID association required. |
| `session.request_active` | Request, statement, or cursor is active. | SBLR/internal operation routing and cancel/timeout state required. |
| `session.draining` | Session is completing or cancelling admitted work. | No new work admitted; transaction outcomes tracked. |
| `session.detaching` | Session resources are being released. | Parser, IPC, transaction, cursor, notification, cache, metrics, and audit cleanup required. |
| `session.detached` | Session lifecycle ended. | No remaining routable resources or orphan authority. |
| `session.unknown_transaction_outcome` | Disconnect, timeout, crash, or force left transaction finality unknown to the route. | User-visible diagnostics persist until MGA evidence proves commit or rollback finality. |

## Transaction and Runtime Authority States

| State | Meaning | Required closure |
| --- | --- | --- |
| `transaction.admission_pending` | Begin/admit transaction checks are running. | Ownership, security, policy, catalog snapshot, filespace, lock, memory, and cluster-scope checks required. |
| `transaction.active` | MGA transaction is active. | Transaction inventory is authority for visibility and cleanup. |
| `transaction.committing` | Commit finality is being established. | MGA commit evidence required before success. |
| `transaction.rolling_back` | Rollback is being executed. | Engine MGA rollback methods only; no parser/reference/tool emulation. |
| `transaction.committed` | Commit is durable and visible according to MGA rules. | Commit evidence and visibility publication required. |
| `transaction.rolled_back` | Rollback is durable. | Rollback evidence and cleanup horizon required. |
| `transaction.unknown_outcome` | External actor cannot prove commit or rollback. | Diagnostic persists until transaction inventory resolves finality. |
| `transaction.cleanup_pending` | Old version, sweep, temporary, lock, or resource cleanup remains. | Cleanup follows MGA horizon and policy; no cache/parser/job finality. |
| `runtime.agent_selecting` | Database-local agent selection is running after admitted activation. | Policy-admitted agents only; lifecycle agent cannot become MGA/catalog/storage/security authority. |
| `runtime.agent_active` | Database-local lifecycle agent is supervising health. | Health publication, safe mode, startup/shutdown coordination, metrics, and reporting required. |
| `runtime.agent_stopping` | Agent shutdown is in progress. | Deterministic stop evidence required before clean shutdown success. |
| `runtime.cache_dirty` | Page cache/buffer/checkpoint has dirty state. | Flush/checkpoint evidence can support clean-close but not transaction finality. |
| `runtime.cache_clean` | Cache/checkpoint state is clean for shutdown classification. | Final lifecycle transaction remains required for clean shutdown success. |

## Operation Family Destination Matrix

Every operation family listed here must receive canonical behavior, registry/API
coverage where applicable, diagnostics, tests, and invalid-transition coverage.

| Operation family | State coverage obligation | Canonical destination keys |
| --- | --- | --- |
| `lifecycle.create_database` | `database.absent` to `database.creating_tx1` to `database.closed_created`; partial create recovery/refusal. | `canonical.storage_mga`, `packet.engine_lifecycle`, `packet.startup_open_filespace`, `registry.sblr_api`, `conformance.lifecycle` |
| `policy.default_catalog.bootstrap` | Tx1 seed lifecycle for policy families, properties, generation 1, cache invalidation, and fail-closed missing/default mismatch behavior. | `packet.default_policy`, `canonical.security_audit`, `registry.diagnostics`, `conformance.lifecycle` |
| `policy.default_catalog.registry_hardening` | Registry, generated seed data, conformance manifest, diagnostic rows, fixtures, and repo hygiene state. | `packet.default_policy`, `registry.diagnostics`, `conformance.lifecycle` |
| `storage.filespace.lifecycle` | Full filespace state table, first filespace registration, identity consistency, mutability, verify, repair, quarantine, and drop. | `canonical.storage_mga`, `packet.startup_open_filespace`, `registry.diagnostics`, `conformance.lifecycle` |
| `lifecycle.first_open_activation` | `database.closed_created` to `database.first_open_activating_tx2` to `database.online`; interrupted tx2 recovery/refusal. | `canonical.storage_mga`, `packet.engine_lifecycle`, `packet.sb_server`, `conformance.lifecycle` |
| `lifecycle.open_database` | Closed/unclean/quarantined startup classification, ownership, compatibility, policy, catalog, filespace, and route admission. | `canonical.storage_mga`, `packet.startup_open_filespace`, `packet.engine_lifecycle`, `registry.diagnostics` |
| `lifecycle.attach_database` | `route.pre_auth` through `route.admitted`, `session.creating`, `session.attached`, and denial vectors. | `canonical.security_audit`, `canonical.server_ipc_config`, `packet.security`, `registry.sblr_api` |
| `lifecycle.detach_database` | `session.draining`, `session.detaching`, `session.detached`, transaction/cursor/parser/IPC cleanup. | `canonical.server_ipc_config`, `packet.engine_lifecycle`, `packet.ipc`, `registry.diagnostics` |
| `lifecycle.begin_transaction_admission` | `transaction.admission_pending` to active/refused with ownership, security, policy, catalog, filespace, lock, memory, and cluster checks. | `canonical.storage_mga`, `packet.mga_transactions`, `registry.sblr_api`, `registry.diagnostics` |
| `lifecycle.enter_maintenance` | Online/restricted to `mode.maintenance_entering` to `mode.maintenance`; drain/fence/refusal. | `canonical.ops_observability`, `packet.engine_lifecycle`, `registry.diagnostics` |
| `lifecycle.exit_maintenance` | `mode.maintenance` to exit validation to online or refused. | `canonical.ops_observability`, `packet.engine_lifecycle` |
| `lifecycle.enter_restricted_open` | Opening/online/maintenance to restricted with read/write fences and permitted operation set. | `canonical.ops_observability`, `packet.engine_lifecycle`, `packet.startup_open_filespace` |
| `lifecycle.exit_restricted_open` | Restricted exit validation to online/maintenance/refused. | `canonical.ops_observability`, `packet.engine_lifecycle` |
| `lifecycle.inspect_database` | Diagnostic read-only state, redacted reporting, no mutation authority. | `canonical.ops_observability`, `registry.diagnostics`, `conformance.lifecycle` |
| `lifecycle.verify_database` | Verify-running state for database, catalog, resource, transaction inventory, pages, filespaces, and identity. | `canonical.ops_observability`, `packet.startup_open_filespace`, `conformance.lifecycle` |
| `lifecycle.repair_database` | Repair planning/running, evidence preservation, bounded repair, refusal/quarantine. | `canonical.ops_observability`, `canonical.storage_mga`, `packet.startup_open_filespace` |
| `lifecycle.recovery_entry` | Closed-unclean/recovery-required/classifying/running/restricted/quarantine/open-refused transitions. | `canonical.storage_mga`, `packet.mga_transactions`, `packet.startup_open_filespace` |
| `lifecycle.shutdown_database` | Shutdown fence, notification, drain, finalization, clean final transaction, ownership release. | `canonical.storage_mga`, `canonical.server_ipc_config`, `packet.engine_lifecycle`, `packet.sb_server` |
| `lifecycle.shutdown_force_database` | Explicit force transition with target database association set and isolation evidence. | `canonical.server_ipc_config`, `packet.managers_listeners_parsers`, `packet.sb_server`, `registry.diagnostics` |
| `lifecycle.shutdown_ack_database` | Acknowledgement state for associated managers/listeners/parsers/IPC/server/clients. | `canonical.server_ipc_config`, `packet.managers_listeners_parsers`, `packet.ipc` |
| `lifecycle.shutdown_parser_fallback` | Listener-failure parser lookup through engine-visible association registry, stale/missing refusal. | `canonical.server_ipc_config`, `packet.managers_listeners_parsers`, `registry.diagnostics` |
| `lifecycle.drop_database` | Safe-state refusal, ownership validation, drop/quarantine evidence, filespace removal/quarantine. | `canonical.storage_mga`, `packet.engine_lifecycle`, `packet.startup_open_filespace` |
| `lifecycle.exclusive_ownership` | Owner-token acquisition, stale owner handling, ambiguous owner refusal, cross-process protection. | `canonical.storage_mga`, `canonical.server_ipc_config`, `packet.sb_server` |
| `process.manager_family.lifecycle` | Manager starting/ready/associated/active/draining/stopping/failed/quarantined for `sbmn_manager` and `sbmc_manager`. | `packet.managers_listeners_parsers`, `canonical.server_ipc_config`, `conformance.lifecycle` |
| `process.listener.lifecycle` | Listener launch, bind, parser-pool, accept/handoff, drain, reload, stop, force stop, failure, quarantine. | `packet.managers_listeners_parsers`, `canonical.server_ipc_config`, `conformance.lifecycle` |
| `process.parser_family.lifecycle` | Parser package admission, worker spawn, HELLO, idle, handoff, pre-auth, attach relay, active, cancel, drain, recycle, terminate, quarantine. | `packet.managers_listeners_parsers`, `packet.ipc`, `registry.diagnostics` |
| `process.lifecycle.association_registry` | Database-scoped manager/listener/parser/IPC/session/route/heartbeat/shutdown generation association states. | `canonical.server_ipc_config`, `packet.managers_listeners_parsers`, `packet.sb_server` |
| `process.server_daemon.lifecycle` | Server daemon startup, service-ready, hosted database association, shared/dedicated isolation, stop, restart, orphan cleanup, failure, quarantine. | `packet.sb_server`, `canonical.server_ipc_config`, `conformance.lifecycle` |
| `process.ipc_channel.lifecycle` | Endpoint descriptor, admission, authn/authz, frame validation, backpressure, drain, stale cleanup, failure, quarantine, shutdown. | `packet.ipc`, `canonical.server_ipc_config`, `registry.diagnostics` |
| `runtime.attachment_session_request_cursor.lifecycle` | Attachment/session/request/statement/cursor/cancel/timeout/disconnect/cleanup/unknown-outcome states. | `canonical.server_ipc_config`, `packet.engine_lifecycle`, `packet.mga_transactions` |
| `runtime.database_engine_agent.lifecycle` | Agent selecting/active/stopping/failure/safe-mode/quarantine and no-authority-bypass boundaries. | `packet.engine_lifecycle`, `canonical.ops_observability`, `registry.diagnostics` |
| `storage.buffer_page_cache_checkpoint.lifecycle` | Cache preload, dirty/clean, writeback, checkpoint force/wait/try, clean-close evidence, eviction, memory pressure, shutdown flush. | `canonical.storage_mga`, `packet.startup_open_filespace`, `conformance.lifecycle` |
| `security.configuration_policy_provider.lifecycle` | Configuration epoch, policy reload, provider load/quiesce, stale refusal, password-hash path, cache invalidation. | `canonical.security_audit`, `packet.configuration`, `packet.security`, `packet.default_policy` |
| `storage.backup_archive_restore_shadow_snapshot.lifecycle` | Backup/archive/restore/shadow/snapshot hold/blocker/inspection/recovery/shutdown/drop interaction state. | `packet.backup_archive`, `canonical.storage_mga`, `canonical.ops_observability` |
| `runtime.udr_extension.lifecycle` | UDR/extension registration, load, unload/quiesce, resource, shutdown, failure, no bypass. | `packet.udr`, `canonical.security_audit`, `registry.diagnostics` |
| `runtime.workload_resource_quota.lifecycle` | Workload/resource pool/quota admission, throttling, cancellation, maintenance, shutdown, failure. | `canonical.ops_observability`, `canonical.resources`, `registry.diagnostics` |
| `catalog.schema_object_dependency.lifecycle` | DDL create/alter/drop/rename, UUID/name registry, dependencies, cache invalidation, authorization, MGA visibility. | `canonical.storage_mga`, `registry.sblr_api`, `conformance.lifecycle` |
| `catalog.system_physical_index_profiles` | sys.catalog index profile lifecycle, UUID exact lookup, traversal, transaction/generation/history, no human-name duplication. | `canonical.storage_mga`, `packet.indexes_optimizer`, `conformance.lifecycle` |
| `catalog.sys_information_projection` | sys.information projection lifecycle, resolver joins, authorization, redaction, MGA snapshot visibility, no write/identity authority. | `canonical.security_audit`, `canonical.ops_observability`, `conformance.lifecycle` |
| `storage.index_statistics_plan.lifecycle` | Index build/drop/rebuild, stats refresh, plan invalidation, epoch checks, interrupted build recovery. | `packet.indexes_optimizer`, `canonical.storage_mga`, `registry.diagnostics` |
| `runtime.lock_wait_deadlock.lifecycle` | Lock table, latch, wait, cancellation, timeout, disconnect cleanup, deadlock, shutdown cleanup, unknown outcome. | `packet.mga_transactions`, `canonical.storage_mga`, `registry.diagnostics` |
| `runtime.temp_spill_workspace.lifecycle` | Temp filespace, temp object, spill, quota, commit/rollback/disconnect/shutdown/recovery cleanup. | `canonical.storage_mga`, `canonical.ops_observability`, `registry.diagnostics` |
| `runtime.event_notification_subscription.lifecycle` | LISTEN/NOTIFY, post event, subscription, ordering, security filtering, disconnect, shutdown, recovery. | `packet.events`, `canonical.security_audit`, `registry.diagnostics` |
| `security.encryption_key_protected_material.lifecycle` | Key admission, encrypted open, key cache, rotation, shutdown purge, missing-key refusal, redaction. | `canonical.security_audit`, `canonical.storage_mga`, `registry.diagnostics` |
| `resources.seed_i18n.lifecycle` | Timezone/charset/collation/locale seed versioning, upgrade/refusal, cache invalidation, index dependencies, runtime epochs. | `canonical.resources`, `packet.indexes_optimizer`, `registry.diagnostics` |
| `mga.garbage_collection_retention.lifecycle` | Old-version cleanup, sweep, retention, backup/archive holds, limbo/unknown outcome, bounded memory. | `canonical.storage_mga`, `packet.mga_transactions`, `registry.diagnostics` |
| `runtime.background_jobs_scheduler.lifecycle` | Database-local jobs startup, pause/resume, retry, quarantine, maintenance, shutdown drain, policy/resource authority. | `canonical.ops_observability`, `packet.engine_lifecycle`, `registry.diagnostics` |
| `cluster.boundary_fail_closed.lifecycle` | Standalone fail-closed states for cluster transaction, route, metric, agent, schema, lifecycle, and authority paths. | `canonical.ops_observability`, `registry.diagnostics`, `conformance.lifecycle` |
| `security.principal_privilege_policy.lifecycle` | Users, roles, groups, grants, revokes, row security, definer rights, policy rows, cache invalidation, audit, MGA visibility. | `canonical.security_audit`, `canonical.storage_mga`, `packet.security` |
| `storage.allocation_freespace_pagemap.lifecycle` | Page allocation, free-space, page map, extent, page ownership, compaction, crash recovery, filespace coupling. | `canonical.storage_mga`, `packet.startup_open_filespace`, `registry.diagnostics` |
| `runtime.executable_database_object.lifecycle` | Routines, procedures, functions, triggers, packages, stored SBLR, dependency, permission, invalidation, unload/quiesce. | `canonical.storage_mga`, `packet.udr`, `registry.sblr_api` |
| `runtime.sequence_generator.lifecycle` | Sequence/generator/identity cache window, persistence, transaction interaction, crash recovery, reference mapping, diagnostics. | `canonical.storage_mga`, `registry.sblr_api`, `registry.diagnostics` |
| `observability.operational_evidence_supportability.lifecycle` | Operational logs, audit, retention, rotation, redaction, export, support bundles, shutdown flush, diagnostic access. | `canonical.ops_observability`, `canonical.security_audit`, `registry.diagnostics` |
| `runtime.capability_profile_feature_gate.lifecycle` | Capability, parser profile, edition gate, feature flag, package availability, policy epoch, downgrade refusal. | `canonical.ops_observability`, `canonical.security_audit`, `registry.diagnostics` |
| `replication.changefeed_boundary.lifecycle` | Replication/CDC/changefeed/live ingest/publication/subscription/slot routes, retention, security, reference mapping, standalone refusal. | `canonical.storage_mga`, `canonical.ops_observability`, `registry.diagnostics` |
| `lifecycle.existing_implementation_reconciliation` | Existing implementation paths reconciled to this vocabulary and canonical specs with zero legacy-contract drift. | `packet.engine_lifecycle`, `conformance.lifecycle`, `registry.diagnostics` |
| `lifecycle.protocol_persisted_format_versioning` | Version/compatibility/migration/downgrade/refusal for protocols and persisted formats. | `canonical.server_ipc_config`, `packet.ipc`, `packet.startup_open_filespace`, `registry.diagnostics` |
| `lifecycle.admin_cli_command_surface` | Intended CLI/admin/client path, auth, diagnostics, audit, idempotency, no-user-command policy where applicable. | `canonical.ops_observability`, `registry.sblr_api`, `registry.diagnostics` |
| `lifecycle.packaging_service_runtime` | Install/start/stop/restart/uninstall/runtime directory/PID/owner file/permission/service isolation/cleanup. | `canonical.ops_observability`, `canonical.server_ipc_config`, `registry.diagnostics` |
| `lifecycle.spec_to_test_traceability` | Generated state, transition, invalid transition, diagnostic, operation family, and route traceability. | `conformance.lifecycle` |
| `lifecycle.upgrade_migration_refusal` | Safe migration or fail-closed refusal for old database files, config, state files, protocol descriptors, manifests, catalog rows. | `canonical.storage_mga`, `canonical.server_ipc_config`, `packet.startup_open_filespace`, `registry.diagnostics` |
| `lifecycle.security_threat_model_gate` | Abuse-case states for force shutdown, IPC auth, supervision, UDR load, health, backup/restore, service files, quotas. | `canonical.security_audit`, `canonical.ops_observability`, `conformance.lifecycle` |

## Transition Evidence Contract

Every canonical transition derived from this draft must define these fields.

| Field | Requirement |
| --- | --- |
| `transition_id` | Stable lower snake case identifier used by specs, registries, tests, diagnostics, and traceability. |
| `source_state` | Exact state or state set from this vocabulary. Wildcards require canonical justification. |
| `operation_family` | Execution_Plan operation family or accepted registry operation that causes the transition. |
| `admission_authority` | Engine, MGA, policy, owner token, config epoch, security, catalog, route, or operator authority required before transition begins. |
| `durable_evidence` | Transaction id, owner token, state file, filespace registry/header/manifest/catalog row, configuration epoch, policy generation, association generation, or audit pointer required before success. |
| `runtime_evidence` | Agent, cache, IPC, server, listener, parser, session, request, cursor, route, heartbeat, or drain evidence required before success. |
| `target_state` | Exact target state and any terminal outcome. |
| `invalid_transition_refusal` | Refusal class and message vector when source state, evidence, authorization, policy, or route state is invalid. |
| `idempotency_key` | Operation id, database UUID, filespace UUID, transaction id, route generation, shutdown generation, or explicit none with justification. |
| `recovery_action` | Complete, roll back, quarantine, restricted open, operator review, refuse open, or ignore advisory state. |
| `cache_invalidation` | Required cache, metadata, plan, resource, policy, route, or parser invalidation. |
| `metrics_audit` | Success and failure metrics/audit evidence. |
| `test_anchor` | CTest label, generated fixture, static gate, or blocking gate required before closure. |

## Refusal Class Vocabulary

Every refusal class must have a diagnostic-code row, diagnostic-shape row,
message-vector rendering, redaction rule, retryability rule, audit policy, owner,
and conformance coverage before closure.

| Refusal class | Trigger | Required destination keys |
| --- | --- | --- |
| `refusal.invalid_state_transition` | Operation is not valid from the current state or mode. | `packet.engine_lifecycle`, `registry.diagnostics`, `conformance.lifecycle` |
| `refusal.missing_canonical_authority` | Execution_Plan, code, or registry lacks canonical spec authority for behavior. | `canonical.ops_observability`, `packet.engine_lifecycle`, `conformance.lifecycle` |
| `refusal.missing_durable_evidence` | Required tx, owner, state, filespace, catalog, manifest, or audit evidence is absent. | `canonical.storage_mga`, `packet.startup_open_filespace`, `registry.diagnostics` |
| `refusal.tx1_incomplete` | Create bootstrap tx1 did not commit or seed required structures. | `canonical.storage_mga`, `packet.default_policy`, `registry.diagnostics` |
| `refusal.tx2_incomplete` | First-open activation tx2 or required runtime activation evidence is incomplete. | `canonical.storage_mga`, `packet.engine_lifecycle`, `registry.diagnostics` |
| `refusal.final_shutdown_missing` | Clean shutdown lacks final lifecycle transaction or owner release evidence. | `canonical.storage_mga`, `packet.engine_lifecycle`, `registry.diagnostics` |
| `refusal.owner_missing_or_stale` | Owner token is missing, stale, expired, or cannot be validated. | `canonical.storage_mga`, `canonical.server_ipc_config`, `registry.diagnostics` |
| `refusal.owner_ambiguous` | Multiple owners or ambiguous process/runtime ownership exists. | `canonical.server_ipc_config`, `packet.sb_server`, `registry.diagnostics` |
| `refusal.authentication_denied` | Engine authentication denies credentials/provider result. | `canonical.security_audit`, `packet.security`, `registry.diagnostics` |
| `refusal.authorization_denied` | Engine authorization denies lifecycle operation. | `canonical.security_audit`, `packet.security`, `registry.diagnostics` |
| `refusal.policy_missing_or_stale` | Required default policy row/generation/cache state is missing or stale. | `packet.default_policy`, `packet.configuration`, `registry.diagnostics` |
| `refusal.policy_override_rejected` | Requested override is forbidden, unauthorized, stale, or cluster-only in standalone mode. | `packet.default_policy`, `canonical.security_audit`, `registry.diagnostics` |
| `refusal.catalog_epoch_stale` | Catalog, metadata cache, identity resolver, or sys.information projection epoch is stale or mismatched. | `canonical.storage_mga`, `registry.diagnostics`, `conformance.lifecycle` |
| `refusal.security_epoch_stale` | Security provider, principal, role, privilege, or policy epoch is stale. | `canonical.security_audit`, `packet.security`, `registry.diagnostics` |
| `refusal.resource_epoch_stale` | Timezone, charset, collation, locale, resource seed, or quota epoch is stale or unsupported. | `canonical.resources`, `registry.diagnostics`, `conformance.lifecycle` |
| `refusal.filespace_identity_missing` | Required filespace registry/header/manifest/catalog identity evidence is missing. | `canonical.storage_mga`, `packet.startup_open_filespace`, `registry.diagnostics` |
| `refusal.filespace_identity_duplicate` | Duplicate filespace UUID or active-primary evidence exists. | `canonical.storage_mga`, `packet.startup_open_filespace`, `registry.diagnostics` |
| `refusal.filespace_identity_mismatch` | Database UUID, filespace UUID, page size, generation, header, manifest, or catalog evidence conflicts. | `canonical.storage_mga`, `packet.startup_open_filespace`, `registry.diagnostics` |
| `refusal.filespace_quarantined` | Required filespace is quarantined or unsafe for requested access. | `canonical.storage_mga`, `packet.startup_open_filespace`, `registry.diagnostics` |
| `refusal.recovery_required` | Startup classification or recovery must run before ordinary operation. | `canonical.storage_mga`, `packet.mga_transactions`, `registry.diagnostics` |
| `refusal.recovery_ambiguous` | Evidence cannot prove complete, roll back, restricted open, or safe repair. | `canonical.storage_mga`, `packet.mga_transactions`, `registry.diagnostics` |
| `refusal.format_unknown` | Protocol or persisted format version is unknown. | `canonical.server_ipc_config`, `packet.ipc`, `packet.startup_open_filespace`, `registry.diagnostics` |
| `refusal.format_downgrade` | Artifact requires a newer unsupported version or unsafe downgrade. | `canonical.server_ipc_config`, `packet.ipc`, `packet.startup_open_filespace`, `registry.diagnostics` |
| `refusal.migration_unsupported` | Older artifact cannot be safely migrated by canonical rule. | `canonical.storage_mga`, `packet.startup_open_filespace`, `registry.diagnostics` |
| `refusal.route_association_missing` | Database-to-manager/listener/parser/IPC/session/route/shutdown association evidence is missing. | `canonical.server_ipc_config`, `packet.managers_listeners_parsers`, `registry.diagnostics` |
| `refusal.route_association_stale` | Association generation, heartbeat, endpoint descriptor, or shutdown generation is stale. | `canonical.server_ipc_config`, `packet.ipc`, `registry.diagnostics` |
| `refusal.route_scope_cross_database` | Lifecycle control would affect another database scope. | `canonical.server_ipc_config`, `packet.sb_server`, `registry.diagnostics` |
| `refusal.ipc_malformed` | IPC frame, descriptor, or protocol data is malformed. | `packet.ipc`, `canonical.server_ipc_config`, `registry.diagnostics` |
| `refusal.ipc_unauthenticated` | IPC actor is not authenticated. | `packet.ipc`, `canonical.security_audit`, `registry.diagnostics` |
| `refusal.ipc_unauthorized` | IPC actor is authenticated but not authorized. | `packet.ipc`, `canonical.security_audit`, `registry.diagnostics` |
| `refusal.backpressure_policy` | Backpressure, quota, or route policy forbids admission. | `packet.ipc`, `canonical.ops_observability`, `registry.diagnostics` |
| `refusal.shutdown_ack_timeout` | Associated component failed to acknowledge within policy/command drain window. | `canonical.server_ipc_config`, `packet.managers_listeners_parsers`, `registry.diagnostics` |
| `refusal.force_not_explicit` | Terminating associated components would require force but force was not explicitly requested. | `canonical.server_ipc_config`, `packet.engine_lifecycle`, `registry.diagnostics` |
| `refusal.transaction_admission_denied` | MGA transaction admission checks fail. | `canonical.storage_mga`, `packet.mga_transactions`, `registry.diagnostics` |
| `refusal.transaction_outcome_unknown` | Commit/rollback finality is unknown to caller or route. | `canonical.storage_mga`, `packet.mga_transactions`, `registry.diagnostics` |
| `refusal.mode_fence_active` | Maintenance, restricted-open, verify, repair, shutdown, or quarantine fence blocks operation. | `canonical.ops_observability`, `packet.engine_lifecycle`, `registry.diagnostics` |
| `refusal.repair_plan_missing` | Repair was requested without accepted plan or preserved evidence. | `canonical.ops_observability`, `packet.startup_open_filespace`, `registry.diagnostics` |
| `refusal.backup_archive_hold` | Backup, archive, restore, shadow, snapshot, legal retention, or hold blocks lifecycle operation. | `packet.backup_archive`, `canonical.ops_observability`, `registry.diagnostics` |
| `refusal.udr_extension_policy` | UDR/extension lifecycle action violates policy, resource, or authority boundaries. | `packet.udr`, `canonical.security_audit`, `registry.diagnostics` |
| `refusal.resource_quota` | Workload, memory, thread, temp, filespace, parser/listener, IPC, or UDR quota blocks admission. | `canonical.ops_observability`, `canonical.resources`, `registry.diagnostics` |
| `refusal.cluster_authority_missing` | Cluster path was requested without canonical cluster mapping and transaction authority. | `canonical.ops_observability`, `registry.diagnostics`, `conformance.lifecycle` |
| `refusal.encryption_key_missing` | Required key authority is missing, stale, rotated out, or unavailable. | `canonical.security_audit`, `canonical.storage_mga`, `registry.diagnostics` |
| `refusal.drop_blocked` | Drop is unsafe due to open handles, active pins, ownership, backup, repair, cluster, storage, or policy blockers. | `canonical.storage_mga`, `packet.engine_lifecycle`, `registry.diagnostics` |

## Traceability Rules

- Every state in this file must map to at least one canonical destination key.
- Every operation family must map to a state transition set, diagnostic set,
  CTest label or static gate, and canonical destination.
- Every invalid transition must map to `refusal.invalid_state_transition` or a
  more specific refusal class.
- Every refusal class must map to diagnostic-code and diagnostic-shape registry
  entries before implementation closure.
- Every full-route operation must prove the client to SBWP/TLS to listener to
  parser to IPC to server to engine response path or record an explicit
  canonical no-user-command policy.
- Traceability must use stable search keys or row identifiers, not line-number
  references.

## P0A Draft Validation Checklist

This draft can be submitted for coordinator review only when:

1. Canonical spec workers can use this vocabulary to update lifecycle specs and
   packets without inventing new state classes.
2. Registry/API workers can assign every operation family to SBLR/API rows,
   dispatch contracts, and unsupported/refusal behavior.
3. Diagnostic workers can assign every refusal class to message-vector rows,
   redaction, retryability, audit, and parser rendering.
4. Test workers can generate state, transition, invalid-transition, refusal,
   operation-family, and route coverage from this vocabulary.
5. No dependent slice is allowed to close on a state, transition, operation, or
   refusal that lacks a canonical destination from this artifact.
6. `DBLC-000` has closed and its authority map and baseline inventory have been
   reconciled with this vocabulary.
7. The coordinator has validated this artifact and explicitly updated the
   execution_plan tracker and acceptance gate.

This draft alone does not close `DBLC-000A` and does not mark
`DBLC_P0A_NO_DEFER_READY` passed.

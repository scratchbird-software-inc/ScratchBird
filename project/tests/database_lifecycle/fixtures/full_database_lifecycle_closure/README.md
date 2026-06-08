# Full Database Lifecycle Contract and Implementation Closure Execution_Plan

Status: passed
Created: 2026-05-09
Owner: database lifecycle implementation coordinator
Search key: `FULL-DATABASE-LIFECYCLE-CONTRACT-IMPLEMENTATION-CLOSURE`

## Purpose

Close the full ScratchBird database lifecycle from canonical contract through implementation, routing, diagnostics, tests, and release declaration. This execution_plan is execution control only. It does not define product behavior by itself; every lifecycle rule must be added to manifest-listed canonical contracts, accepted decision records, registries, or implementation packets before the corresponding implementation slice can close.

The closure scope is the entire database lifecycle:

```text
create database
  -> transaction 1 durable bootstrap
  -> closed-created state
  -> first open activation
  -> transaction 2 first connection/session activation
  -> attach session
  -> authenticate and authorize through engine authority
  -> begin/admit transactions
  -> normal operation
  -> maintenance/restricted/diagnostic/repair modes
  -> shutdown notification to database-associated clients/managers/listeners/parsers
  -> policy or command-defined graceful drain window
  -> detach sessions
  -> drain and shutdown
  -> forced shutdown termination when explicitly requested
  -> final clean-shutdown transaction
  -> reopen or recovery/quarantine path
  -> drop database
```

The full external route must be covered:

```text
client
  -> SBWP/TLS and INET layer
  -> listener network port
  -> pool-allocated SBSQL parser
  -> parser-server IPC
  -> sb_server
  -> sb_engine
  -> authentication policy
  -> authentication plugin or engine-owned password-hash verification
  -> engine session and transaction admission on accept
  -> deny message vector on refusal
  -> sb_server
  -> IPC
  -> parser response
  -> client
```

## Authority Inputs

Canonical authority remains under `public_release_evidence`. The implementation coordinator must reconcile, amend, and test against these inputs before code slices close.

| Input | Role |
| --- | --- |
| `public_contract_snapshot` | Authority gate for all normative files. |
| `public_contract_snapshot` | Authority order, execution_plan non-authority rule, and MGA/no-WAL invariant. |
| `public_contract_snapshot` | Durable transaction inventory as single-node transaction finality authority. |
| `public_input_snapshot` | Central default policy catalog for create-database tx1 policy family seed rows, properties, default values, override rules, and policy generation behavior. |
| `public_contract_snapshot` | Machine-readable default policy registry required for generated seed data, static gates, and registry-to-packet consistency. |
| `public_contract_snapshot` | Conformance manifest tying policy families, defaults, diagnostics, fixtures, generated seed data, and gates to implementation closure. |
| `public_contract_snapshot` | Canonical diagnostic-code registry that must include all lifecycle `POLICY.*` failures. |
| `public_contract_snapshot` | Canonical diagnostic-shape and message-vector mapping registry for policy and lifecycle diagnostics. |
| `public_input_snapshot` | Current lifecycle implementation packet to expand or supersede. |
| `public_input_snapshot` | Startup/open/filespace ordering and conformance closure packet. |
| `public_input_snapshot` | Server daemon lifecycle and hosted database supervision implementation packet. |
| `public_input_snapshot` | IPC endpoint and channel lifecycle implementation packet. |
| `public_input_snapshot` | Configuration source and policy epoch authority packet. |
| `public_input_snapshot` | Security provider and authentication policy implementation packet. |
| `public_input_snapshot` | Backup, archive, restore, shadow, and snapshot implementation packet. |
| `public_input_snapshot` | UDR and extension lifecycle implementation packet. |
| `public_input_snapshot` | Event notification and subscription lifecycle implementation packet. |
| `public_input_snapshot` | Index lifecycle and access method implementation packet. |
| `public_input_snapshot` | Optimizer plan and statistics implementation packet. |
| `public_input_snapshot` | MGA transaction cleanup, sweep, retention, locking, and temporary-storage implementation packet. |
| `public_input_snapshot` | Manager-family lifecycle and manager/server handoff implementation packet for `sbmn_manager` and `sbmc_manager`. |
| `public_input_snapshot` | Standalone manager product lifecycle implementation packet. |
| `public_input_snapshot` | Listener lifecycle and parser-pool implementation packet. |
| `public_input_snapshot` | Standalone listener product lifecycle implementation packet. |
| `public_input_snapshot` | Parser framework and parser worker lifecycle implementation packet. |
| `public_input_snapshot` | Concrete listener/manager/server/parser control-plane closure packet. |
| `public_contract_snapshot` | Storage, MGA, recovery, lifecycle, and durability authority. |
| `public_contract_snapshot` | Server, listener, IPC, configuration, and lifecycle hosting authority. |
| `public_contract_snapshot` | Authentication, authorization, policy, and audit authority. |
| `public_contract_snapshot` | Timezone, character set, collation, and resource bootstrap authority. |
| `public_contract_snapshot` | Management, metrics, diagnostic, repair, and operational lifecycle authority. |
| `public_contract_snapshot` | Detailed MGA transaction, recovery, attach/session, cleanup, conformance, and repair appendices. |
| `public_contract_snapshot` | SBLR opcode registry for lifecycle and management operations. |
| `public_contract_snapshot` | SBLR operation-family to engine API mapping authority. |
| `project/src/engine/internal_api/ENGINE_API_SURFACE_REGISTRY.yaml` | Current engine internal API registry requiring lifecycle completion. |

## Current Baseline Facts

The current implementation has partial lifecycle support, not full closure.

| Area | Current evidence | Closure implication |
| --- | --- | --- |
| Storage create/open/shutdown | `project/src/storage/database/database_lifecycle.*`, `startup_state.*` | Create/open/clean shutdown exist but need full lifecycle state, recovery, ownership, diagnostic, repair, detach, and drop closure. |
| Filespace lifecycle | `project/src/storage/filespace/filespace_lifecycle.*`, `filespace_header.*`, `filespace_identity.*`, `filespace_secondary.*` | Filespace create, attach, detach, promote, demote, verify, move, compact, truncate, drop, identity registry, and lifecycle evidence exist partially but must be bound to database lifecycle and CTest closure. |
| Engine lifecycle API | `project/src/engine/internal_api/lifecycle/engine_lifecycle_api.*` | Create/open/attach/detach/shutdown/maintenance/restricted-open exist, but registry and behavior are incomplete for the entire lifecycle. |
| Server hosting | `project/src/server/engine_host.*`, `project/src/server/lifecycle.*` | Hosted open/auto-create and process lifecycle exist, but full authenticated route, drain, shutdown, ownership, and lifecycle state propagation need closure. |
| Server daemon lifecycle | `project/src/server/lifecycle.*`, `startup.*`, `engine_host.*`, `listener_orchestrator.*`, `manager_control.*` | `sb_server` startup, service-ready, hosted database association, shared/dedicated isolation, graceful stop, force stop, restart, orphan cleanup, failure, and quarantine need first-class closure. |
| IPC channel lifecycle | `project/src/server/ipc_server.*`, `parser_server_event_ipc.*`, `project/src/wire/parser_server_ipc/` | IPC endpoint descriptor creation, admission, authentication, authorization, frame validation, backpressure, drain, stale endpoint cleanup, failure, quarantine, and shutdown need first-class closure. |
| Attachment/session/request/cursor lifecycle | `project/src/server/session_registry.*`, `sblr_admission.*`, `sblr_dispatch_server.*` | Session, attachment, request, statement, cursor, cancel, timeout, disconnect, cleanup, and unknown-outcome behavior need first-class closure. |
| Database/engine lifecycle agent | `project/src/core/agents/agent_engine_lifecycle.*`, `agent_lifecycle.*`, `agent_runtime.*`, `sys/agents_views.*` | Database-local lifecycle agent selection, health supervision, startup/shutdown coordination, reporting, safe-mode, failure, and quarantine need first-class closure. |
| Buffer page-cache and checkpoint lifecycle | `project/src/storage/page/page_cache.*`, storage checkpoint/clean-close paths | Cache preload, dirty tracking, writeback, checkpoint, clean-close evidence, eviction, memory pressure, and shutdown flush behavior need first-class closure. |
| Configuration policy and security-provider lifecycle | `project/src/server/config.*`, `project/src/core/config/`, `project/src/core/security/`, `project/src/engine/internal_api/security/` | Configuration source epochs, policy reload, auth-provider plugin lifecycle, default policy installation, password-hash verification, cache invalidation, and stale-policy refusal need closure. |
| Backup archive restore shadow snapshot lifecycle | backup/archive packets and storage operations roots | Backup holds, archive/legal retention, restore-inspection open mode, shadow/snapshot lifecycle, shutdown/drop interactions, and engine-owned no-live-file-shortcut behavior need closure. |
| UDR/extension lifecycle | `project/src/udr/`, parser-support UDR packages | Registration, load/unload, policy checks, resource limits, shutdown cleanup, diagnostics, and no authority bypass need closure. |
| Workload resource quota lifecycle | `project/src/engine/sblr/sblr_resource_governance.*`, resource and quota roots | Workload class admission, resource pool startup/shutdown, memory/thread/temp/filespace quota, throttling, cancellation, maintenance, shutdown, and failure behavior need closure. |
| Default policy catalog | `public_input_snapshot`, `public_contract_snapshot`, `public_contract_snapshot`, policy/security/config/resource/lifecycle specs | Every lifecycle policy family, property, default value, override class, tx1 seed row, policy generation, fail-closed boundary, generated seed-data source, diagnostic mapping, and override fixture must be centralized before create-database implementation. |
| Catalog schema object dependency lifecycle | `project/src/engine/internal_api/catalog/`, catalog-schema specs, parser DDL lowering | DDL create/alter/drop/rename, UUID/name registry, dependency graph, metadata cache invalidation, schema ownership, and MGA visibility need first-class lifecycle closure. |
| System catalog physical index profiles and information projections | catalog-schema specs, index specs, name-resolution specs, `project/src/engine/internal_api/catalog/`, `project/src/core/index/` | Every `sys.catalog` table needs explicit physical index profiles for UUID exact lookup, parent-child traversal, generation/history scans, policy catalog load, and identity resolution. Human-facing SQL names must not be duplicated into base catalog tables; the identity resolver is the only human-name authority. `sys.information` must expose SQL-standard information schema compatible views plus ScratchBird extended user-friendly catalog projection views for clients. |
| Index statistics optimizer plan lifecycle | `project/src/core/index/`, index and optimizer packets | Index build/drop/rebuild, statistics refresh, plan cache invalidation, collation/charset epoch checks, and crash recovery during index work need closure. |
| Lock latch wait deadlock lifecycle | MGA locking/wait/deadlock specs and transaction code | Lock table state, latches, wait queues, cancellation, disconnect cleanup, deadlock detection, and shutdown cleanup need explicit lifecycle coverage. |
| Temporary spill sort workspace lifecycle | temp filespace, memory, sort, materialization, and resource governance roots | Temporary filespaces, session temp objects, sort/hash spill, workspace cleanup, quotas, shutdown, and recovery need closure. |
| Event notification subscription lifecycle | `event_notifications.md`, `project/src/server/event_notification_router.*`, parser-server event IPC | LISTEN/NOTIFY, post event, subscriptions, delivery ordering, security filtering, disconnect cleanup, shutdown, and recovery need closure. |
| Encryption key protected material lifecycle | security/KMS specs, encrypted filespace open paths, protected-material redaction paths | Key admission, encrypted open, key cache lifetime, key rotation, purge on shutdown, missing-authority refusal, and protected-material redaction need closure. |
| Resource seed timezone charset collation lifecycle | resource registry, timezone, charset, collation, locale, datatype, and index epoch paths | Seed versioning, upgrade/refusal, cache invalidation, index dependencies, and runtime epoch behavior need closure beyond tx1 bootstrap. |
| MGA garbage collection sweep history retention lifecycle | MGA sweep, retention, archive, backup hold, and transaction inventory paths | Old-version cleanup, sweep, history retention, backup/archive holds, limbo/unknown outcome protection, and bounded-memory behavior need explicit MGA-authoritative closure. |
| Background jobs scheduler database-local task lifecycle | job scheduler metrics, agents, MGA background task scheduling, resource governance | Database-local jobs need startup, pause/resume, retry, quarantine, maintenance, shutdown drain, policy authority, and failure closure. |
| Cluster-boundary fail-closed lifecycle | cluster specs, `project/src/cluster/`, cluster admission guards | Standalone execution must prove it never enters cluster transaction, route, metric, agent, schema, or lifecycle paths until cluster mapping and authority exist. |
| Security principal role privilege policy lifecycle | `project/src/engine/internal_api/security/`, security specs, policy catalogs | Users, roles, groups, grants, revokes, row security, definer rights, policy rows, cache invalidation, audit, authorization, and MGA visibility need first-class closure. |
| Storage allocation free-space page-map lifecycle | `project/src/storage/page/`, page allocation agent/specs | Page allocation, free-space maps, extent reservations, page ownership, reusable space, compaction, crash recovery, and filespace coupling need first-class closure. |
| Executable database object lifecycle | `project/src/engine/sblr/sblr_routine_runtime.*`, `project/src/engine/internal_api/ddl/`, trigger/procedure specs | Routines, procedures, functions, triggers, event triggers, packages, stored SBLR, dependencies, permissions, invalidation, side effects, and unload/quiesce need first-class closure. |
| Sequence generator identity state lifecycle | `project/src/engine/sblr/sblr_sequence_runtime.*`, DDL sequence create paths | Sequences, generators, identity columns, cache windows, persistence, transaction interaction, crash recovery, donor mapping, and diagnostics need first-class closure. |
| Operational log audit support-bundle lifecycle | `project/src/server/server_observability.*`, metrics and support-bundle specs | Operational logs, audit evidence, retention, rotation, redaction, export, support bundles, shutdown flush, diagnostic access, and protected-material filtering need first-class closure. |
| Capability profile edition feature-gate lifecycle | `project/src/core/agents/agent_feature_gates.*`, capability specs | Installed capabilities, parser profiles, edition gates, feature flags, package availability, policy epochs, downgrade refusal, and diagnostics need first-class closure. |
| Replication CDC changefeed boundary lifecycle | `project/src/engine/internal_api/cluster/replication_api.*`, donor replication/CDC specs | Replication, CDC, changefeed, live ingest, publication, subscription, slot, route, retention, security, donor mapping, and standalone fail-closed behavior need first-class closure. |
| Existing lifecycle implementation reconciliation | Current `project/src/manager/`, `listener/`, `parsers/`, `server/`, `storage/`, `udr/`, `core/agents/`, configuration, security, backup, and resource code | Existing code must be audited and modified to match the updated lifecycle contract rather than being left on old assumptions. |
| Protocol and persisted-format versioning | SBWP/TLS, parser IPC, management IPC, lifecycle state files, filespace headers, manifests, catalog rows, and configuration epochs | Every lifecycle protocol and durable format needs explicit version, upgrade, downgrade, compatibility, and fail-closed refusal behavior. |
| CLI and admin lifecycle commands | `project/src/server/cli.*`, manager control paths, admin/client command routes | All lifecycle operations need intended command surfaces, authorization, diagnostics, audit, and CTest coverage. |
| Packaging service runtime lifecycle | service wrappers, runtime directories, PID files, owner files, control sockets, permissions, install/uninstall material | Install, start, stop, restart, uninstall, cleanup, runtime permissions, and database-scoped service behavior need closure. |
| Spec-to-test traceability | lifecycle specs, tracker, acceptance gates, validation plan, CTest labels, static gates | Every lifecycle state, transition, diagnostic, invalid transition, and route needs generated traceability to tests or gates. |
| Upgrade migration refusal lifecycle | database files, filespace manifests, lifecycle state files, protocol descriptors, catalog rows, config epochs | Older artifacts must either migrate safely or fail closed with canonical diagnostics. |
| Security threat-model and abuse-case gate | security specs, IPC paths, supervision paths, UDR loading, backup/restore, force shutdown, quota paths | Lifecycle operations need least-privilege, abuse-case, and fail-closed tests. |
| Manager-family lifecycle | `project/src/manager/node/manager_lifecycle.*`, `manager_runtime.*`, `manager_listener_control.*` plus future/private `sbmc_manager` implementation targets | `manager` means `sbmn_manager` and `sbmc_manager`; node-manager startup, ready, proxy, supervision, listener control, drain, shutdown, restart, quarantine, and cluster-manager fail-closed/private lifecycle boundaries must be closed as part of database lifecycle. |
| Listener lifecycle | `project/src/listener/listener_runtime.*`, `parser_pool.*`, `control_plane.*` | Listener launch, bind, ready, parser-pool control, accept/handoff, drain, reload, shutdown, and forced stop must be closed. |
| Parser-family lifecycle | `project/src/parsers/sbsql_worker/`, `project/src/parsers/native/`, `project/src/parsers/donor/`, parser-support UDR packages where applicable, and future parser package roots | `parser` means any ScratchBird parser package or parser worker, not only currently implemented parsers; package admission, spawn, handshake, pre-auth, attach, active work, drain, disconnect, recycle, quarantine, and shutdown must be closed for the parser family. |
| Tests | `project/tests/sbsql_parser_worker/sbsql_database_create_schema_bootstrap_gate.cpp` and MGA regression tests | Some tx1/tx2 and clean-shutdown evidence exists, but no exhaustive database lifecycle suite exists. |
| Registries | `sblr.database.management.v3` and lifecycle API rows exist partially | Missing lifecycle operations must be registered, mapped, dispatched, and tested. |

## Non-Negotiable Closure Rules

- No lifecycle operation may remain parser-only, API-only, spec-only, stubbed, TODO, placeholder, future, or deferred at closure.
- Execution_Plan text cannot be treated as product behavior. Missing behavior must be added to canonical specs or implementation packets, then implemented.
- ScratchBird MGA remains the transaction, visibility, rollback, recovery, cleanup, and finality authority.
- No WAL, redo log, undo log, donor transaction log, parser state, file timestamp, wall-clock order, or UUID order may be used as transaction finality authority.
- Rollback behavior must use engine MGA methods. Parser, donor, driver, adapter, tool, listener, or server glue must not emulate rollback outside MGA.
- Cluster lifecycle paths must fail closed until cluster mapping and cluster transaction authority are implemented by canonical cluster contracts and code.
- Local schema roots are `sys`, `users`, `remote`, and `emulated`. Cluster schema roots must not appear unless a cluster exists.
- Database create must assign a generated UUIDv7 to the database and fresh UUIDv7 values to every bootstrap object and row that requires identity.
- Every filespace must have its own generated UUIDv7 that is distinct from the database UUID and from every other filespace UUID. The first physical database file is also the first filespace and must receive and register its own filespace UUID during create.
- Filespace identity must be durable before lifecycle success is reported. Path, physical file order, file timestamp, page number, or UUID ordering must never substitute for the filespace registry or MGA lifecycle evidence.
- Filespaces must understand database lifecycle state: create, open, attach, detach, maintenance/restricted, verify, repair, shutdown, recovery, quarantine, and drop must update or validate filespace state and registration before user-visible success.
- System catalog/name associations must use the identity resolver and its common name-to-UUID and UUID-to-name tables rather than fixed path-only identity.
- `sys.catalog` holds the low-level system catalog tables. Those tables may be read by authorized humans for diagnostics, but they must not perform helpful name translation, localization, or display-name duplication. They use UUIDs, generated row identities, parent and child UUID relationships, machine-stable registry keys, transaction/generation/history fields, and typed descriptors.
- Base `sys.catalog` tables must not store human-facing SQL object names or localized display text. Those values may exist only in the identity resolver surface, which is the sole source of truth for name-to-UUID and UUID-to-name presentation.
- `sys.information` is the client-facing database metadata schema. It must contain SQL-standard information schema compatible views plus ScratchBird extended user-friendly views that join `sys.catalog` UUID data to the information projection resolver, language/default fallback, authorization filtering, redaction policy, and MGA snapshot visibility.
- `sys.information` views are projections only. They must not own catalog identity, name authority, transaction visibility, dependency authority, policy authority, or write authority.
- UUID exact lookup should use hash-capable equality index profiles where enabled and collision-rechecked. B-tree over UUID fields is allowed only when it is serving a documented ordered/group/prefix traversal, deterministic bootstrap/verification scan, composite access path, or temporary baseline fallback; it must not be treated as creation order, commit order, or transaction finality.
- Catalog history, visibility, and order use MGA transaction inventory, created/retired transaction fields, catalog generation, policy generation, and explicit history sequence fields. UUIDv7 ordering must never substitute for those authorities.
- `sys`, `sys.security`, `sys.metrics`, `sys.catalog`, resource registries, timezone data, charset/collation data, default policies, default roles/groups, `users`, `users.public`, and configured user-home schema policy must be ready after database creation.
- Default policy behavior must come from `public_input_snapshot`. Create/open/attach/transaction/lifecycle code must not invent implicit policy defaults or rely on spread-out subsystem defaults when the catalog row is missing.
- Default policy implementation must use a manifest-listed machine-readable registry and generated seed-data path. Subsystems must not hand-code or duplicate default policy values, override classes, seed states, or diagnostic rules outside that path.
- Every `POLICY.*` diagnostic used by default policy bootstrap, open, attach, transaction admission, lifecycle operations, configuration reload, cache invalidation, and fail-closed boundary behavior must have canonical diagnostic-code and diagnostic-shape registry rows before implementation can close.
- Every default policy override class must have accepted and rejected fixtures. `cluster_only` overrides must fail closed for standalone databases until cluster authority exists.
- Manifest and repository hygiene are implementation gates: a manifest-listed contract, registry, or conformance file must exist, be trackable, not be ignored by repository excludes, and not be duplicated in the authority inventory.
- Transaction 1 is reserved for durable database structure bootstrap. Transaction 2 is reserved for the first successful connection/session activation that starts database-specific runtime agents, cache preload, IPC state, and server/session activation.
- Clean shutdown must write durable lifecycle evidence, stop agents and threads deterministically, release ownership, and persist a final transaction record before external success is returned.
- Shutdown contract must define database-scoped notification and drain behavior for every manager, listener, parser, IPC server daemon, server daemon, session, and client associated with the target database.
- Server daemon, IPC channel, attachment/session/request/cursor, database/engine lifecycle agent, buffer/page-cache/checkpoint, configuration/policy/security-provider, backup/archive/restore/shadow/snapshot, UDR/extension, workload/resource/quota, manager-family, listener, and parser-family lifecycles must be specified and implemented as first-class database lifecycle surfaces, not only as shutdown helpers.
- The database/engine lifecycle agent is the engine-owned health supervisor for a database. It may select policy-admitted database-local agents, start and stop them, monitor health, coordinate safe mode, publish database health to connection/session paths, and report lifecycle status. It must not become MGA, catalog, storage, authentication, authorization, policy, or SBLR execution authority.
- Server daemon and IPC lifecycles must preserve per-database isolation across shared daemons and must fail closed on stale endpoint descriptors, ambiguous ownership, malformed frames, unauthenticated IPC, unauthorized IPC, or backpressure policy violations.
- Attachment, session, request, statement, cursor, cancel, timeout, and disconnect lifecycle outcomes must be engine-owned and must preserve unknown transaction outcome diagnostics until MGA evidence proves finality.
- Buffer, page-cache, writeback, and checkpoint lifecycle behavior must support clean shutdown and recovery evidence without substituting cache state, checkpoint state, flush state, or file timestamp for MGA finality.
- Configuration, policy, and security-provider lifecycle changes must use epochs/generations, cache invalidation, and stale-policy refusal. Stale configuration, stale authentication provider state, or stale security policy must not admit sessions or transactions.
- Backup, archive, restore, shadow, and snapshot lifecycle operations must be admitted through engine-owned paths only. External live-file shortcuts cannot satisfy backup, restore, shadow, snapshot, drop, or recovery behavior.
- UDR and extension lifecycle code must be policy-gated, resource-bounded, unloadable or quiesceable by policy, and unable to bypass parser, security, SBLR, transaction, catalog, storage, or lifecycle authority.
- Workload class, resource pool, quota, temp-space, thread, memory, filespace pressure, cancellation, and throttling lifecycle must participate in attach, transaction admission, maintenance, shutdown, and failure handling.
- Catalog schema object and dependency lifecycle must be engine-owned and UUID-based. DDL success cannot be reported until catalog rows, name/UUID associations, dependency invalidation, metadata cache invalidation, authorization, and MGA visibility evidence are correct.
- Index, statistics, and optimizer-plan lifecycle must be tied to catalog and MGA evidence. Index build/drop/rebuild, statistics refresh, plan-cache invalidation, collation/charset epoch checks, and interrupted build recovery must not use parser or cache state as authority.
- Lock, latch, wait, and deadlock lifecycle must preserve MGA transaction authority. Cancellation, disconnect, timeout, deadlock victim selection, and shutdown cleanup must leave no orphan authority and must preserve unknown-outcome diagnostics until MGA evidence resolves finality.
- Temporary filespaces, session temp objects, sort/hash spill, and workspace state must be quota-governed, bounded, and cleaned on commit, rollback, disconnect, shutdown, and recovery without becoming durable catalog or transaction authority.
- Event notification and subscription lifecycle must enforce authorization and redaction, clean up on disconnect/shutdown, define delivery ordering and durability class, and never become transaction finality or security authority.
- Encryption keys and protected material must have explicit admission, cache lifetime, rotation, shutdown purge, missing-key refusal, and redaction behavior. A database or filespace must not store its own decryption key.
- Timezone, charset, collation, locale, and other resource seed lifecycles must have versioned upgrade/refusal, cache invalidation, index dependency, runtime epoch, and diagnostic behavior after tx1 bootstrap.
- MGA garbage collection, sweep, old-version cleanup, retention, backup/archive holds, limbo handling, and unknown-outcome protection must remain MGA-owned and bounded-memory. No WAL, cache, parser, donor, or job state may decide visibility or cleanup finality.
- Background jobs and database-local scheduler tasks must start only after admitted database activation, pause during maintenance where required, drain on shutdown, respect policy and resource authority, and quarantine on unsafe failure.
- Cluster-boundary fail-closed behavior must be a first-class lifecycle surface: standalone execution must not enter cluster transaction, route, metric, agent, schema, or lifecycle paths until cluster mapping and authority exist.
- Security principal, role, group, grant, revoke, row-security, definer-rights, and policy lifecycles must remain engine-authorized, audited, catalog UUID-based, cache-invalidated, and MGA-visible before success.
- Storage allocation, free-space maps, page ownership, extent reservations, reusable space, and compaction must remain tied to filespace identity, page-map evidence, and recovery classification.
- Executable database objects, including routines, procedures, functions, triggers, event triggers, packages, and stored SBLR, must carry dependency, permission, invalidation, side-effect, unload/quiesce, and UDR-boundary behavior.
- Sequences, generators, and identity columns must define cache-window, persistence, transaction interaction, crash recovery, donor mapping, and diagnostic behavior without becoming transaction finality or donor-owned state.
- Operational logs, audit evidence, and support bundles must define retention, rotation, redaction, export, shutdown flush, diagnostic access, and protected-material filtering.
- Installed capabilities, parser profiles, edition gates, feature flags, package availability, and policy epochs must gate lifecycle behavior explicitly and fail closed on downgrade or unsupported profiles.
- Replication, CDC, changefeed, live ingest, publication, subscription, and slot routes must either be admitted by exact engine authority or fail closed in standalone/non-implemented modes.
- Existing manager, listener, parser-family, server, IPC, session, filespace, catalog, index, concurrency, temporary workspace, event, encryption, resource seed, MGA GC, jobs, cluster-boundary, security principal, storage allocation, executable object, sequence generator, supportability, capability, replication, UDR, agent, cache/checkpoint, configuration, security-provider, backup/archive/restore, and workload/resource implementations must be audited and modified to satisfy the updated lifecycle contracts. Compatibility with old lifecycle assumptions is not a valid closure condition.
- Lifecycle protocol and persisted-format surfaces must carry explicit version, compatibility, migration, downgrade, and fail-closed refusal behavior. Silent interpretation of old lifecycle state, old IPC frames, old filespace headers, old manifests, old catalog rows, or stale configuration epochs is forbidden.
- Every lifecycle operation must have an intended CLI/admin/client path or an explicit no-user-command policy with tests proving the chosen route, authorization, diagnostics, and audit behavior.
- Packaging, service, runtime-directory, PID/owner file, permission, install, uninstall, start, stop, restart, and cleanup behavior must preserve database isolation and must not leave stale authorities or routable endpoints.
- Spec-to-test traceability must prove that every lifecycle state, transition, diagnostic, invalid transition, operation family, and full-route path maps to CTest or a static gate before final closure.
- Upgrade and migration behavior must never guess. Unsupported or ambiguous database files, lifecycle state files, protocol descriptors, filespace manifests, catalog rows, or configuration epochs must fail closed with canonical diagnostics.
- Lifecycle threat modeling must include force shutdown, IPC authentication and authorization, manager/listener/parser supervision, UDR loading, health publication, backup/restore, service files, and resource-quota abuse cases.
- Databases hosted by the same server must remain isolated for lifecycle control. A shutdown for one database must not notify, drain, disconnect, or terminate managers, listeners, parsers, sessions, or clients associated only with another database.
- A database may use a dedicated server daemon or shared server daemon as a security-isolation policy choice. Lifecycle routing must identify the target database scope before any shutdown notification, acknowledgement wait, or forced termination.
- Non-force shutdown must fence new work, notify associated clients and routing components, wait for acknowledgements, allow a policy-defined or command-specified drain period, and close each client cleanly after commit or rollback.
- Force shutdown must be explicit. When requested, the engine-owned lifecycle authority must terminate all associated managers, listeners, parsers, IPC daemons, sessions, and client connections for the target database scope before completing shutdown evidence.
- Listeners control their active parser pools during normal operation, but engine lifecycle must maintain or read a database-to-parser association registry so the engine can shut down target-database parsers if a listener fails or cannot acknowledge shutdown.
- Create, attach, detach, drop, inspect, verify, repair, maintenance, restricted-open, shutdown, and recovery outcomes must use canonical message vectors.
- Engine is the only authentication and authorization authority. A parser may relay credentials and render results, but it must not accept or deny as authority.
- `manager` is the short-form family term for `sbmn_manager` and `sbmc_manager`; requirements that apply to only one product must name that product explicitly.
- `parser` is the short-form family term for any parser package or parser worker, including native SBSQL parsers, donor/emulation parsers, parser-support UDR packages where applicable, and future parser packages; product-specific requirements must name the exact parser product or package.
- Managers, listeners, and parsers must never become transaction, storage, catalog, security, authentication-finality, authorization-finality, or SBLR-execution authority through lifecycle control.
- The engine may use an authentication plugin or an engine-owned password-hash comparison when it is its own provider. Unencrypted passwords must not be stored or compared inside the engine.
- Donor/emulation parser surfaces must lower lifecycle commands into ScratchBird lifecycle SBLR/API operations or exact emulated/non-file diagnostics without bypassing engine authority.
- Every implementation slice must include CTest coverage or an explicit blocking gate. No documentation-only waiver can close a slice.

## Required Final Outcomes

At closure, all of these must be true:

- Canonical contracts define the entire database lifecycle: create, open, first-open activation, attach, authenticate, session creation, transaction admission, normal operation, diagnostic, verify, repair, maintenance, restricted-open, detach, shutdown, recovery, quarantine, and drop.
- `public_input_snapshot` or a successor packet is implementation-ready for every lifecycle operation.
- SBLR opcode and operation registries contain every lifecycle operation needed by native SBSQL and donor parser profiles.
- `project/src/engine/internal_api/ENGINE_API_SURFACE_REGISTRY.yaml` includes complete lifecycle API rows with no missing dispatch.
- Engine internal API, public ABI, server hosting, parser-server IPC, listener route, SBWP/TLS route, and parser mapping all support the same lifecycle contract.
- Database create bootstraps all required system structures, resources, security, metrics, default policy, agents, and user schema roots through durable MGA evidence.
- Database create seeds every policy family in the default policy catalog under `sys.policy` with fresh UUIDv7 policy/profile/binding/evidence rows, policy generation 1, exact default properties, override evidence, cache invalidation, metrics, and fail-closed diagnostics.
- Default policy seed data is generated from a manifest-listed machine-readable registry that matches the Markdown packet and conformance manifest exactly.
- `POLICY.*` diagnostic codes and shapes are registered in canonical diagnostic registries with message-vector mapping, redaction, retryability, audit policy, and owner metadata.
- Default policy override fixtures cover all override classes and prove accepted overrides, forbidden overrides, stale-generation refusal, unauthorized security-admin/sysarch refusal, and standalone cluster-only refusal.
- Repository hygiene gates prove every manifest-listed default policy packet, registry, conformance manifest, and diagnostic registry file exists, is not ignored, is trackable, and is not duplicated in the authority inventory.
- Database create registers the first physical database file as the first `active_primary` filespace with a fresh filespace UUIDv7, durable filespace header, startup directory entry, manifest/catalog row, and tx1 evidence.
- Filespace lifecycle is fully specified and implemented for create, attach, detach, promote, demote, read-only/read-write transition, move, verify, repair/quarantine, compact, truncate, drop, ownership, metrics, audit, and cache invalidation.
- Open/reopen performs startup recovery classification before user-visible work and either admits, repairs, restricts, quarantines, or refuses with exact diagnostics.
- Attach/session creation validates database ownership, security policy, authentication, authorization, catalog/security/policy epochs, resource limits, and transaction admission before user-visible work.
- Detach releases parser, IPC, server, session, transaction, cursor, notification, and cache resources deterministically.
- Maintenance and restricted-open modes have enter, exit, drain, fence, and permitted-operation rules.
- Shutdown notifications, acknowledgement waits, graceful drain windows, clean client disconnects, and force termination are specified and implemented for database-associated managers, listeners, parsers, IPC daemons, sessions, and clients.
- Server daemon lifecycle is specified and implemented for startup, configuration load, ownership, service-ready publication, hosted database association, shared/dedicated daemon isolation, graceful stop, force stop, restart policy, orphan cleanup, failed state, and quarantine.
- IPC channel lifecycle is specified and implemented for endpoint descriptor creation, admission, authentication, authorization, frame validation, backpressure, drain, stale descriptor cleanup, failure, quarantine, and shutdown.
- Attachment/session/request/statement/cursor lifecycle is specified and implemented for identity, admission, execution routing, cancellation, timeout, disconnect, cleanup, unknown transaction outcome handling, metrics, and diagnostics.
- Database/engine lifecycle agent is specified and implemented for database-local agent selection, agent startup/shutdown, health supervision, safe-mode entry/exit recommendation, health publication to connections, reporting, failure, and quarantine without authority bypass.
- Buffer/page-cache/checkpoint lifecycle is specified and implemented for preload, dirty tracking, writeback, checkpoint force/wait/try, clean-close evidence, eviction, memory pressure, shutdown flush, recovery classification, metrics, and diagnostics.
- Configuration/policy/security-provider lifecycle is specified and implemented for configuration source epochs, policy reload, default policy installation, authentication provider/plugin load and quiesce, password-hash verification, cache invalidation, stale refusal, metrics, and diagnostics.
- Backup/archive/restore/shadow/snapshot lifecycle is specified and implemented for backup holds, archive coverage, legal retention, restore-inspection open, snapshot/shadow state, filespace interaction, shutdown/drop blockers, recovery evidence, and no external live-file shortcuts.
- UDR/extension lifecycle is specified and implemented for registration, load, unload/quiesce, policy checks, resource limits, parser-support UDR behavior, shutdown cleanup, diagnostics, and no authority bypass.
- Workload/resource/quota lifecycle is specified and implemented for workload classes, resource pools, admission, throttling, cancellation, memory/thread/temp/filespace quotas, maintenance participation, shutdown behavior, failure, metrics, and diagnostics.
- Catalog/schema/object/dependency lifecycle is specified and implemented for DDL create/alter/drop/rename, UUID/name registry, dependency graph, schema ownership, metadata cache invalidation, authorization, and MGA visibility.
- System catalog physical index profiles are specified and implemented for every bootstrap `sys.catalog` table: hash equality profiles for UUID exact lookup where enabled, B-tree profiles for ordered/group/prefix/generation/history traversal where needed, explicit transaction/generation/history indexes for catalog visibility, and no duplicated human-facing SQL object names outside the identity resolver surface.
- `sys.information` catalog projection views are specified and implemented for SQL-standard information schema compatibility and ScratchBird extended user-friendly metadata. Clients read database metadata through `sys.information`; raw `sys.catalog` remains low-level diagnostic/internal storage and performs no name translation.
- Index/statistics/optimizer-plan lifecycle is specified and implemented for index build/drop/rebuild, statistics refresh, plan cache invalidation, collation/charset epoch checks, interrupted build recovery, and catalog/MGA consistency.
- Lock/latch/wait/deadlock lifecycle is specified and implemented for lock table ownership, latches, wait queues, cancellation, disconnect cleanup, deadlock detection, timeout behavior, shutdown cleanup, diagnostics, and MGA authority boundaries.
- Temporary/spill/sort/workspace lifecycle is specified and implemented for temporary filespaces, session temp objects, sort/hash spill, workspace quotas, cleanup on commit/rollback/disconnect/shutdown, and recovery classification.
- Event/notification/subscription lifecycle is specified and implemented for LISTEN/NOTIFY, post event, subscriptions, delivery ordering, durability class, security filtering, disconnect cleanup, shutdown, recovery, metrics, and diagnostics.
- Encryption/key/protected-material lifecycle is specified and implemented for key admission, encrypted filespace open, key cache lifetime, rotation, shutdown purge, missing-authority refusal, redaction, and protected-material diagnostics.
- Resource seed lifecycle is specified and implemented for timezone, charset, collation, locale, resource seed versioning, supported upgrade, unsupported refusal, cache invalidation, index dependencies, runtime epochs, and diagnostics.
- MGA garbage-collection/sweep/history-retention lifecycle is specified and implemented for old-version cleanup, sweep, retention, backup/archive holds, limbo/unknown outcome protection, bounded memory, recovery interaction, and no finality substitution.
- Background jobs/scheduler lifecycle is specified and implemented for database-local job startup, pause/resume, retry, quarantine, maintenance participation, shutdown drain, policy authority, resource limits, metrics, and diagnostics.
- Cluster-boundary fail-closed lifecycle is specified and implemented so standalone execution cannot enter cluster transaction, route, metric, agent, schema, or lifecycle paths without cluster mapping and authority.
- Security principal/role/privilege/policy lifecycle is specified and implemented for users, roles, groups, grants, revokes, row security, definer rights, policy rows, cache invalidation, audit, authorization, and MGA visibility.
- Storage allocation/free-space/page-map lifecycle is specified and implemented for page allocation, free-space maps, page ownership, extent reservations, reusable space, compaction, crash recovery, and filespace coupling.
- Executable database object lifecycle is specified and implemented for routines, procedures, functions, triggers, event triggers, packages, stored SBLR, dependencies, permissions, invalidation, side effects, and unload/quiesce behavior.
- Sequence/generator/identity lifecycle is specified and implemented for sequence identity, generator state, identity columns, cache windows, persistence, transaction interaction, crash recovery, donor mapping, and diagnostics.
- Operational log/audit/support-bundle lifecycle is specified and implemented for logs, audit evidence, retention, rotation, redaction, export, support bundles, shutdown flush, diagnostic access, and protected-material filtering.
- Capability/profile/edition/feature-gate lifecycle is specified and implemented for installed capabilities, parser profiles, edition gates, feature flags, package availability, policy epochs, downgrade refusal, and diagnostics.
- Replication/CDC/changefeed/live-ingest boundary lifecycle is specified and implemented for replication, CDC, changefeed, live ingest, publication, subscription, slot, route, retention, security, donor mapping, and standalone fail-closed behavior.
- Existing lifecycle implementations are reconciled to the updated contracts with an audit report, required patches, regression tests, and zero remaining legacy-contract drift.
- Protocol and persisted-format versioning is implemented for SBWP/TLS, parser IPC, management IPC, lifecycle state files, filespace headers, manifests, catalog rows, and configuration epochs.
- CLI/admin lifecycle command coverage is implemented for every lifecycle operation including forced shutdown, inspect, verify, repair, health, status, and drop routes.
- Packaging/service/runtime-directory lifecycle behavior is implemented for install, start, stop, restart, uninstall, runtime directories, PID/owner files, permissions, service isolation, cleanup, and database-scoped service handling.
- Spec-to-test traceability generator proves every lifecycle state, transition, invalid transition, diagnostic, operation family, and route is covered by CTest or static gates.
- Upgrade/migration/refusal lifecycle behavior safely migrates supported old artifacts and fails closed for unsupported or ambiguous artifacts.
- Security threat-model and abuse-case gates cover lifecycle force, IPC, supervision, UDR, health, backup/restore, packaging/service, and resource-quota risks.
- Manager-family lifecycle is specified and implemented for `sbmn_manager` and `sbmc_manager` scope: configuration load, owner token, proxy/management bind, listener/server supervision, cluster-manager private/fail-closed boundaries, heartbeat, ready/restricted, drain, stop, restart policy, failed state, and quarantine.
- Listener lifecycle is specified and implemented for launch descriptor validation, bind, ready, parser-pool start, health, accept, handoff, drain, reload, stop, forced parser-pool stop, failed state, and quarantine.
- Parser-family lifecycle is specified and implemented for any installed or future parser package: package admission, process spawn, HELLO/HELLO_ACK, idle, socket handoff, pre-auth relay, attach, active session, SBLR request/response, cancel, drain, disconnect, recycle, force termination, failed state, and quarantine.
- Database-to-manager, database-to-listener, database-to-parser, and parser-to-session association registries are specified, implemented, persisted or snapshotted as policy requires, and tested for stale/missing/failure behavior.
- Diagnostic, verify, and repair modes are implemented with bounded memory, preserved evidence, and fail-closed repair authority.
- Drop database is implemented with exact local-file, open-handle, ownership, backup, repair, cluster, and failure semantics.
- Every lifecycle state transition emits metrics, audit evidence, cache invalidation where required, and message-vector diagnostics on failure.
- Full-route CTest suites prove lifecycle behavior through client -> SBWP/TLS -> listener -> parser -> IPC -> server -> engine and back.
- The final release declaration states exactly what lifecycle behavior is supported, with no overclaiming.

## High-Level Implementation Order

```text
P0 authority and baseline freeze
  -> P0A lifecycle state vocabulary and no-defer contract
  -> P0B implementation inventory and gap matrix
  -> P0C validation command materialization
  -> P0D agent orchestration, five-minute heartbeat, and unattended implementation policy
  -> P0E default policy catalog and create-database policy defaults
  -> P0F default policy registry hardening diagnostics fixtures and repo hygiene
  -> P1 canonical lifecycle contract packet
  -> P2 SBLR/API/public ABI registry closure
  -> P3 durable lifecycle state and transaction evidence
  -> P4 create database tx1 bootstrap
  -> P4A filespace lifecycle identity registration and database lifecycle coupling
  -> P5 first-open tx2 activation
  -> P6 open/reopen ownership and recovery classification
  -> P7 authentication authorization session and attach admission
  -> P8 transaction admission and normal operation lifecycle hooks
  -> P9 detach drain and session resource cleanup
  -> P10 maintenance restricted-open diagnostic verify repair modes
  -> P11 shutdown notification graceful drain force termination clean final transaction and runtime stop
  -> P12 drop database lifecycle
  -> P13 server listener SBWP/TLS parser route integration
  -> P13A manager-family lifecycle contract and implementation
  -> P13B listener lifecycle contract and implementation
  -> P13C parser-family lifecycle contract and implementation
  -> P13D process association registry and cross-process supervision
  -> P13E server daemon lifecycle and hosted database supervision
  -> P13F IPC channel and endpoint lifecycle
  -> P13G attachment session request statement cursor lifecycle
  -> P13H database engine lifecycle agent and health supervision
  -> P13I buffer page-cache and checkpoint lifecycle
  -> P13J configuration policy and security-provider lifecycle
  -> P13K backup archive restore shadow snapshot lifecycle
  -> P13L UDR extension lifecycle
  -> P13M workload resource quota lifecycle
  -> P13U catalog schema object dependency lifecycle
  -> P13U1 system catalog physical index profiles sys.information projections and identity-resolver isolation
  -> P13AA resource seed timezone charset collation lifecycle
  -> P13V index statistics and optimizer plan lifecycle
  -> P13W lock latch wait and deadlock lifecycle
  -> P13X temporary spill sort and workspace lifecycle
  -> P13Y event notification and subscription lifecycle
  -> P13Z encryption key and protected material lifecycle
  -> P13AB MGA garbage collection sweep and history retention lifecycle
  -> P13AC background jobs scheduler and database-local task lifecycle
  -> P13AD cluster-boundary fail-closed lifecycle
  -> P13AE security principal role privilege and policy lifecycle
  -> P13AF storage allocation free-space and page-map lifecycle
  -> P13AG executable database object lifecycle
  -> P13AH sequence generator identity and state lifecycle
  -> P13AI operational log audit evidence and support-bundle lifecycle
  -> P13AJ capability profile edition and feature-gate lifecycle
  -> P13AK replication CDC changefeed and live-ingest boundary lifecycle
  -> P13N existing lifecycle implementation reconciliation
  -> P13O protocol and persisted-format lifecycle versioning
  -> P13P CLI and admin lifecycle command coverage
  -> P13Q packaging service and runtime-directory lifecycle
  -> P13S upgrade migration and refusal lifecycle
  -> P13T security threat-model and abuse-case gate
  -> P13R spec-to-test traceability generator
  -> P14 donor/emulation parser lifecycle mapping
  -> P15 observability diagnostics audit metrics cache invalidation
  -> P16 exhaustive lifecycle regression suite
  -> P17 hardening fault injection and no-authority-drift gates
  -> P18 release declaration and final zero-open audit
```

## Agent Orchestration Model

Implementation is designed to be managed by a single coordinator using agents by default. The goal is unattended forward progress after a human starts implementation: the coordinator keeps assigning ready slices, monitoring active agents, integrating work, validating, correcting failures, and updating execution_plan tracking until the execution_plan is complete or a blocker requires a human decision.

- One coordinator owns slice ordering, agent assignment, write-scope allocation, tracker updates, acceptance-gate updates, failure inventory, validation, correction loops, final integration, and closure evidence.
- Agents implement disjoint slices or disjoint parts of a slice. The coordinator may handle immediate blocking integration/correction locally when delegating would overlap active write scopes or delay the critical path.
- The coordinator checks active agent status at least every five minutes during implementation or long validation runs and records every heartbeat in `artifacts/DATABASE_LIFECYCLE_AGENT_HEARTBEAT_LOG.csv`.
- Every active agent must have a row in `artifacts/DATABASE_LIFECYCLE_AGENT_STATUS.csv` with owner, assigned slice, write scope, current state, last heartbeat, current validation gate, evidence path, and blocker state.
- Every writable file or file group assigned to an agent must have a row in `artifacts/DATABASE_LIFECYCLE_AGENT_WRITE_SCOPE_REGISTER.csv`. The coordinator must not assign two agents to the same source, spec, registry, generated artifact, build rule, or test file group at the same time.
- `artifacts/DATABASE_LIFECYCLE_SLICE_EXECUTION_QUEUE.csv` is the live execution queue. It records prerequisite status, current owner, current state, last validation command, result, evidence, and next action for every `TRACKER.csv` slice.
- A dependent slice cannot open until its prerequisites are implemented, validated, recorded in `TRACKER.csv`, reflected in `ACCEPTANCE_GATES.csv` when applicable, and linked to evidence.
- If validation fails, new dependent slice work pauses until correction is complete, the failure is recorded in `artifacts/DATABASE_LIFECYCLE_AGENT_FAILURE_INVENTORY.csv`, and the validation gate is rerun.
- Full, gate, or soak validation runs must collect complete failure inventories and preserve logs. They must not stop on the first failure unless the failure blocks useful collection of the rest of the evidence.
- The implementation loop continues without waiting for human presence unless a required architectural decision, destructive action, credential, external dependency, or ambiguous authority conflict is encountered. Such blockers are recorded in the failure inventory and current agent status before escalation.
- A slice may not close from agent self-report alone. Closure requires coordinator review, merged/integrated changes, required tests or static gates, execution_plan tracker update, acceptance gate update when applicable, and evidence path update.

Slice lifecycle:

```text
pending
  -> assigned
  -> implementing
  -> implementation_complete_pending_validation
  -> validation_failed
  -> correcting
  -> validation_passed
  -> tracker_updated
  -> next_slice_opened
```

## Lifecycle Operation Inventory

This inventory is the required implementation target list. Canonical specs and registries must carry the exact product behavior before implementation closes.

| Operation family | Required closure |
| --- | --- |
| `lifecycle.create_database` | Durable file/header/page/system catalog bootstrap, database UUID, tx1 committed, default resource/security/metrics/policy/user schema seed, no overwrite unless explicitly authorized. |
| `policy.default_catalog.bootstrap` | Every lifecycle policy family, property, default value, override rule, tx1 seed row, policy generation, audit/evidence row, metrics, cache invalidation, and fail-closed diagnostic required by create database. |
| `policy.default_catalog.registry_hardening` | Machine-readable policy registry, generated seed-data path, conformance manifest, `POLICY.*` diagnostic registry rows, override/failure fixture matrix, no-hardcoded-default static gate, and repo-hygiene gate for manifest-listed default policy artifacts. |
| `storage.filespace.lifecycle` | Filespace UUIDv7 generation, first-filespace registration, attach/detach/promote/demote/read-only/read-write/move/verify/repair/quarantine/compact/truncate/drop transitions, durable manifest/catalog/header consistency, metrics, audit, cache invalidation, and fail-closed stale/missing/duplicate identity handling. |
| `lifecycle.first_open_activation` | First successful open/session activation, tx2 committed, runtime agents and cache/IPC/server state activated after durable bootstrap visibility. |
| `lifecycle.open_database` | Ownership acquisition, startup state read, recovery classification, compatibility gate, security/policy/catalog/resource load, write admission decision. |
| `lifecycle.attach_database` | Authenticated session creation, security authorization, default schema/home schema policy, snapshot/catalog/security/policy epoch capture, message-vector refusal on denial. |
| `lifecycle.detach_database` | Transaction/cursor/parser/IPC/session cleanup, cache invalidation, metrics/audit, deterministic release of attachment resources. |
| `lifecycle.begin_transaction_admission` | Engine MGA admission after ownership, security, policy, catalog, filespace, lock, memory, and cluster-scope checks pass. |
| `lifecycle.enter_maintenance` | Drain/fence writes, admit maintenance-authorized users, mark mode, expose permitted operations only. |
| `lifecycle.exit_maintenance` | Verify safe state, un-fence writes when policy permits, update metrics/audit/cache state. |
| `lifecycle.enter_restricted_open` | Open with restricted/read-only behavior, permit only diagnostic/repair-safe operations. |
| `lifecycle.exit_restricted_open` | Return to normal/maintenance/shutdown state only after verification and policy checks. |
| `lifecycle.inspect_database` | Read-only diagnostic state report with redacted security and stable message-vector fields. |
| `lifecycle.verify_database` | Structural, catalog, security, resource, metrics, transaction inventory, page/filespace, and name/UUID verification. |
| `lifecycle.repair_database` | Authorized bounded repair with preserved evidence, explicit repair plan, no silent repair unless policy admits and evidence is persisted. |
| `lifecycle.recovery_entry` | Crash/unclean reopen classification, incomplete transaction handling, recovery/quarantine/restricted-open decision under MGA authority. |
| `lifecycle.shutdown_database` | Admission fence, database-scoped manager/listener/parser/client notification, acknowledgement wait, policy or command-defined graceful drain window, clean disconnect after commit or rollback, stop agents/threads, persist final lifecycle transaction, mark clean shutdown, release ownership. |
| `lifecycle.shutdown_force_database` | Explicit forced shutdown that terminates all managers, listeners, parsers, IPC daemons, sessions, and client connections associated with the target database scope while preserving MGA recovery evidence and database isolation. |
| `lifecycle.shutdown_ack_database` | Associated managers, listeners, parsers, IPC daemons, server daemons, and clients confirm shutdown notification receipt, drain state, and clean-stop completion for the target database scope. |
| `lifecycle.shutdown_parser_fallback` | Engine resolves target-database parser instances from an engine-visible parser association registry and shuts them down when the owning listener fails, times out, or cannot provide authoritative pool state. |
| `lifecycle.drop_database` | Refuse if unsafe; otherwise close sessions, release/validate ownership, persist drop evidence, remove or quarantine local storage by policy, return generated evidence. |
| `lifecycle.exclusive_ownership` | Owner-token and stale-owner handling, ambiguous-owner refusal, cross-process open protection, diagnostic recovery path. |
| `process.manager_family.lifecycle` | Manager-family process lifecycle for `sbmn_manager` and `sbmc_manager`: config, owner token, proxy bind, management bind, listener/server supervision, cluster-manager private/fail-closed boundaries, heartbeat, ready/restricted, drain, stop, restart, failure, and quarantine. |
| `process.listener.lifecycle` | Listener launch, bind, profile validation, parser-pool creation, parser health, accept/handoff, drain, reload, stop, force stop, failure, and quarantine. |
| `process.parser_family.lifecycle` | Parser-family package admission, worker spawn, HELLO, idle, handoff, pre-auth, attach, active execution, cancel, drain, disconnect, recycle, terminate, failure, and quarantine for any parser package or parser worker. |
| `process.lifecycle.association_registry` | Database-scoped runtime association records for managers, listeners, parser pools, parser workers, IPC endpoints, sessions, attachments, routes, heartbeats, and shutdown generations. |
| `process.server_daemon.lifecycle` | `sb_server` daemon startup, service-ready state, hosted database association, shared/dedicated daemon isolation, open/close supervision, graceful stop, force stop, restart, orphan cleanup, failure, and quarantine. |
| `process.ipc_channel.lifecycle` | Parser-server, server-management, and internal IPC endpoint descriptors, admission, authentication, authorization, frame validation, backpressure, drain, stale cleanup, failure, quarantine, and shutdown. |
| `runtime.attachment_session_request_cursor.lifecycle` | Attachment, session, request, statement, cursor, cancel, timeout, disconnect, cleanup, unknown transaction outcome, metrics, and diagnostics. |
| `runtime.database_engine_agent.lifecycle` | Database/engine lifecycle agent selection of active database-local agents, health checks, startup/shutdown coordination, safe-mode handling, database health reporting to connections, failure, and quarantine without authority bypass. |
| `storage.buffer_page_cache_checkpoint.lifecycle` | Buffer pool, page cache, preload, dirty tracking, writeback, checkpoint, clean-close evidence, eviction, memory pressure, shutdown flush, recovery classification, metrics, and diagnostics. |
| `security.configuration_policy_provider.lifecycle` | Configuration source epochs, policy reload, security-provider/plugin lifecycle, default policy installation, password-hash verification path, cache invalidation, stale refusal, metrics, and diagnostics. |
| `storage.backup_archive_restore_shadow_snapshot.lifecycle` | Backup, archive, restore, shadow, snapshot, holds, legal retention, restore inspection, filespace interaction, shutdown/drop blockers, recovery evidence, and engine-owned path enforcement. |
| `runtime.udr_extension.lifecycle` | UDR and extension registration, load, unload/quiesce, policy checks, resource limits, parser-support UDR behavior, shutdown cleanup, diagnostics, and no authority bypass. |
| `runtime.workload_resource_quota.lifecycle` | Workload classes, resource pools, quota admission, throttling, cancellation, memory/thread/temp/filespace quotas, maintenance, shutdown, failure, metrics, and diagnostics. |
| `catalog.schema_object_dependency.lifecycle` | DDL create/alter/drop/rename, UUID/name registry, dependency graph, metadata cache invalidation, schema ownership, authorization, and MGA visibility. |
| `catalog.system_physical_index_profiles` | Physical index profiles for `sys.catalog` low-level system tables: UUID exact lookup hash profiles where enabled, B-tree only for ordered/group/prefix/generation/history traversal or bootstrap fallback, transaction/generation/history indexes for MGA visibility, and human-facing SQL names only in the identity resolver surface. |
| `catalog.sys_information_projection` | Client-facing metadata projections in `sys.information`: SQL-standard information schema compatible views and ScratchBird extended views that join `sys.catalog` UUID tables to the information projection resolver, language fallback, authorization, redaction, and MGA snapshot visibility without becoming catalog identity authority. |
| `storage.index_statistics_plan.lifecycle` | Index build/drop/rebuild, statistics refresh, optimizer plan cache invalidation, collation/charset epoch validation, interrupted-build recovery, metrics, and diagnostics. |
| `runtime.lock_wait_deadlock.lifecycle` | Lock table, latch, wait queue, cancellation, timeout, disconnect cleanup, deadlock detection, shutdown cleanup, and MGA authority boundaries. |
| `runtime.temp_spill_workspace.lifecycle` | Temporary filespaces, session temp objects, sort/hash spill, workspace quotas, cleanup on commit/rollback/disconnect/shutdown, and recovery classification. |
| `runtime.event_notification_subscription.lifecycle` | LISTEN/NOTIFY, post event, subscriptions, delivery ordering, durability class, security filtering, disconnect cleanup, shutdown, recovery, metrics, and diagnostics. |
| `security.encryption_key_protected_material.lifecycle` | Key admission, encrypted filespace open, key cache lifetime, rotation, shutdown purge, missing-authority refusal, protected-material redaction, metrics, and diagnostics. |
| `resources.seed_i18n.lifecycle` | Timezone, charset, collation, locale, resource seed versioning, supported upgrade, unsupported refusal, cache invalidation, index dependency, runtime epoch, and diagnostics. |
| `mga.garbage_collection_retention.lifecycle` | Old-version cleanup, sweep, history retention, backup/archive holds, limbo handling, unknown-outcome protection, bounded memory, and no finality substitution. |
| `runtime.background_jobs_scheduler.lifecycle` | Database-local job startup, pause/resume, retry, quarantine, maintenance participation, shutdown drain, policy authority, resource limits, metrics, and diagnostics. |
| `cluster.boundary_fail_closed.lifecycle` | Standalone fail-closed behavior for cluster transaction, route, metric, agent, schema, lifecycle, and authority paths until cluster mapping exists. |
| `security.principal_privilege_policy.lifecycle` | Users, roles, groups, grants, revokes, row security, definer rights, policy rows, cache invalidation, audit, authorization, and MGA visibility. |
| `storage.allocation_freespace_pagemap.lifecycle` | Page allocation, free-space maps, page ownership, extent reservations, reusable space, compaction, crash recovery, filespace coupling, metrics, and diagnostics. |
| `runtime.executable_database_object.lifecycle` | Routines, procedures, functions, triggers, event triggers, packages, stored SBLR, dependencies, permissions, invalidation, side effects, unload/quiesce, and UDR-boundary behavior. |
| `runtime.sequence_generator.lifecycle` | Sequence identity, generator state, identity columns, cache windows, persistence, transaction interaction, crash recovery, donor mapping, and diagnostics. |
| `observability.operational_evidence_supportability.lifecycle` | Operational logs, audit evidence, retention, rotation, redaction, export, support bundles, shutdown flush, diagnostic access, and protected-material filtering. |
| `runtime.capability_profile_feature_gate.lifecycle` | Installed capabilities, parser profiles, edition gates, feature flags, package availability, policy epochs, downgrade refusal, unsupported-profile refusal, and diagnostics. |
| `replication.changefeed_boundary.lifecycle` | Replication, CDC, changefeed, live ingest, publication, subscription, slot, route, retention, security, donor mapping, and standalone fail-closed behavior. |
| `lifecycle.existing_implementation_reconciliation` | Audit and modify existing manager, listener, parser, server, IPC, session, filespace, catalog, index, concurrency, temporary workspace, event, encryption, resource seed, MGA GC, jobs, cluster-boundary, security principal, storage allocation, executable object, sequence generator, supportability, capability, replication, UDR, agent, cache, configuration, security, backup, resource, and workload code to match the updated lifecycle contracts. |
| `lifecycle.protocol_persisted_format_versioning` | Version, compatibility, migration, downgrade, refusal, and diagnostics for SBWP/TLS, parser IPC, management IPC, lifecycle state files, filespace headers, manifests, catalog rows, and configuration epochs. |
| `lifecycle.admin_cli_command_surface` | Intended CLI/admin/client command path, authorization, diagnostics, audit, and CTest coverage for every lifecycle operation. |
| `lifecycle.packaging_service_runtime` | Install, start, stop, restart, uninstall, runtime directories, PID/owner files, permissions, service isolation, cleanup, and database-scoped service lifecycle. |
| `lifecycle.spec_to_test_traceability` | Generated mapping from lifecycle specs, states, transitions, diagnostics, invalid transitions, operation families, and route paths to CTest or static gates. |
| `lifecycle.upgrade_migration_refusal` | Safe migration or canonical fail-closed refusal for older database files, configuration, lifecycle state files, protocol descriptors, filespace manifests, and catalog rows. |
| `lifecycle.security_threat_model_gate` | Threat model and abuse-case coverage for force shutdown, IPC auth, supervision, UDR loading, health reporting, backup/restore, service files, and resource quotas. |

## Test Strategy

The lifecycle regression suite must be repeatable from CTest and must include:

- Unit tests for storage lifecycle page/state serialization, transaction inventory evidence, startup state, owner tokens, recovery classification, and diagnostic vectors.
- Filespace lifecycle tests for first-filespace UUID generation and registration, secondary filespace add/attach, detach, promote/demote, read-only/read-write transition, move, verify, repair/quarantine, compact, truncate, drop, stale/missing/duplicate UUID refusal, and database lifecycle coupling.
- Default policy catalog tests proving every policy family in `default_policy_catalog.md` is seeded or fail-closed exactly once in tx1 with UUIDv7 identity, generation 1, exact required properties, default values, override rules, metrics, diagnostics, and no cluster schema in standalone mode.
- Default policy registry hardening tests proving the machine-readable registry, Markdown packet, generated seed data, conformance manifest, diagnostic registries, override fixtures, static no-hardcoded-default gate, and repo-hygiene gate remain consistent.
- Engine API tests for every lifecycle operation and every invalid-state transition.
- Server/listener route tests through SBWP/TLS and parser-server IPC.
- Manager-family lifecycle tests for `sbmn_manager` and `sbmc_manager` scope: startup, config, owner token, proxy/management bind, heartbeat, listener/server supervision, cluster-manager private/fail-closed behavior, drain, stop, restart policy, failed state, quarantine, and database isolation.
- Listener lifecycle tests for launch, bind, parser-pool lifecycle, parser health, accept/handoff, drain, reload, stop, forced parser-pool stop, failed state, quarantine, and database isolation.
- Parser-family lifecycle tests for any parser package or parser worker: package admission, HELLO/HELLO_ACK, handoff, pre-auth relay, attach, active request handling, cancel, drain, disconnect, recycle, force termination, failed state, quarantine, and no engine-authority bypass.
- Server daemon lifecycle tests for startup, service-ready, hosted database association, shared/dedicated daemon isolation, graceful stop, force stop, restart, orphan cleanup, failed state, quarantine, and database isolation.
- IPC lifecycle tests for endpoint descriptors, admission, authentication, authorization, frame validation, malformed frames, backpressure, drain, stale descriptor cleanup, failure, quarantine, and shutdown.
- Attachment/session/request/cursor lifecycle tests for session identity, attachment admission, request and cursor open/close, cancellation, timeout, disconnect, unknown transaction outcome, cleanup, and diagnostics.
- Database/engine lifecycle agent tests for tx2 activation, policy-admitted agent selection, health checks, startup/shutdown coordination, health reporting to connections, safe mode, failure, quarantine, and no authority bypass.
- Buffer/page-cache/checkpoint lifecycle tests for preload, dirty tracking, writeback, checkpoint force/wait/try, clean-close evidence, eviction, memory pressure, shutdown flush, recovery classification, and no finality substitution.
- Configuration/policy/security-provider lifecycle tests for source epochs, reload, default policy installation, auth-provider/plugin load/quiesce, password-hash verification path, cache invalidation, stale refusal, and diagnostics.
- Backup/archive/restore/shadow/snapshot lifecycle tests for holds, archive coverage, legal retention, restore-inspection open, filespace interactions, shutdown/drop blockers, recovery evidence, and no external live-file shortcut.
- UDR/extension lifecycle tests for registration, load, unload/quiesce, policy checks, parser-support UDR behavior, resource limits, shutdown cleanup, diagnostics, and no authority bypass.
- Workload/resource/quota lifecycle tests for workload classes, resource pools, memory/thread/temp/filespace quota admission, throttling, cancellation, maintenance, shutdown, and failure behavior.
- Catalog/schema/object/dependency lifecycle tests for DDL create/alter/drop/rename, UUID/name registry, dependency invalidation, metadata cache invalidation, schema ownership, rollback/commit visibility, and authorization.
- System catalog physical index profile tests for UUID exact lookup hash paths, B-tree ordered/group/prefix/generation/history paths, policy catalog load paths, identity resolver paths, parent-child traversal, catalog history scans, and static gates proving human-facing SQL names are not duplicated in base `sys.catalog` tables.
- `sys.information` projection tests for SQL-standard information schema compatible views and ScratchBird extended metadata views, including information projection resolver joins, language/default fallback, authorization filtering, redaction, MGA snapshot visibility, client route usage, raw UUID exposure rules, and no projection write or identity authority.
- Index/statistics/optimizer-plan lifecycle tests for index build/drop/rebuild, statistics refresh, plan cache invalidation, collation/charset epoch checks, interrupted build recovery, and catalog/MGA consistency.
- Lock/latch/wait/deadlock lifecycle tests for lock ownership, latches, wait queues, cancellation, timeout, disconnect cleanup, deadlock victim behavior, shutdown cleanup, and no MGA authority drift.
- Temporary/spill/sort/workspace lifecycle tests for temp filespaces, session temp objects, sort/hash spill, quota enforcement, cleanup on commit/rollback/disconnect/shutdown, and recovery classification.
- Event/notification/subscription lifecycle tests for LISTEN/NOTIFY, post event, subscriptions, delivery ordering, security filtering, disconnect cleanup, shutdown behavior, and recovery classification.
- Encryption/key/protected-material lifecycle tests for key admission, encrypted filespace open, key cache lifetime, rotation, shutdown purge, missing-key refusal, and protected-material redaction.
- Resource seed lifecycle tests for timezone, charset, collation, locale, versioned upgrade/refusal, cache invalidation, index dependency, runtime epoch, and diagnostics.
- MGA garbage-collection/sweep/history-retention lifecycle tests for old-version cleanup, retention, backup/archive holds, limbo/unknown outcome protection, bounded memory, recovery interaction, and no finality substitution.
- Background jobs/scheduler lifecycle tests for database-local startup, pause/resume, retry, quarantine, maintenance participation, shutdown drain, policy authority, resource limits, and diagnostics.
- Cluster-boundary fail-closed lifecycle tests proving standalone execution cannot enter cluster transaction, route, metric, agent, schema, or lifecycle paths without cluster mapping and authority.
- Security principal/role/privilege/policy lifecycle tests for users, roles, groups, grants, revokes, row security, definer rights, policy rows, cache invalidation, audit, authorization, and MGA visibility.
- Storage allocation/free-space/page-map lifecycle tests for page allocation, free-space maps, page ownership, extent reservations, reusable space, compaction, crash recovery, filespace coupling, metrics, and diagnostics.
- Executable database object lifecycle tests for routines, procedures, functions, triggers, event triggers, packages, stored SBLR, dependencies, permissions, invalidation, side effects, unload/quiesce, and UDR-boundary behavior.
- Sequence/generator/identity lifecycle tests for sequence identity, generator state, identity columns, cache windows, persistence, transaction interaction, crash recovery, donor mapping, and diagnostics.
- Operational log/audit/support-bundle lifecycle tests for logs, audit evidence, retention, rotation, redaction, export, support bundles, shutdown flush, diagnostic access, and protected-material filtering.
- Capability/profile/edition/feature-gate lifecycle tests for installed capabilities, parser profiles, edition gates, feature flags, package availability, policy epochs, downgrade refusal, unsupported-profile refusal, and diagnostics.
- Replication/CDC/changefeed/live-ingest boundary lifecycle tests for replication, CDC, changefeed, live ingest, publication, subscription, slot, route, retention, security, donor mapping, and standalone fail-closed behavior.
- Existing implementation reconciliation tests and audit report proving current manager, listener, parser, server, IPC, session, filespace, catalog, index, concurrency, temporary workspace, event, encryption, resource seed, MGA GC, jobs, cluster-boundary, security principal, storage allocation, executable object, sequence generator, supportability, capability, replication, UDR, agent, cache, configuration, security, backup, resource, and workload code matches the updated lifecycle specs.
- Protocol and persisted-format versioning tests for accepted current versions, supported migration versions, downgrade refusal, unknown version refusal, malformed descriptor refusal, and exact diagnostics.
- CLI/admin command tests for every lifecycle operation, including authorization denial, diagnostics, audit, command idempotency, force shutdown, inspect, verify, repair, health, status, and drop.
- Packaging/service/runtime-directory lifecycle tests for install, start, stop, restart, uninstall, runtime directory permissions, PID/owner files, stale socket cleanup, service isolation, and database-scoped cleanup.
- Spec-to-test traceability generation tests proving every lifecycle state, transition, diagnostic, invalid transition, operation family, and route path has a CTest or static-gate target.
- Upgrade/migration/refusal tests for old database files, config files, lifecycle state files, IPC descriptors, filespace headers, manifests, and catalog rows.
- Security threat-model tests for force shutdown abuse, IPC auth bypass, manager/listener/parser supervision abuse, UDR load abuse, health information disclosure, backup/restore abuse, service-file abuse, and resource-quota abuse.
- SBSQL full-route tests for create, attach, authenticate, session, transaction, maintenance, restricted-open, verify, repair, shutdown, reopen, detach, and drop.
- Shutdown tests for database-scoped client notification, manager/listener/parser acknowledgement, listener-failure parser fallback, stale or missing parser association refusal, shared-server isolation, dedicated-server isolation, graceful drain timeout, commit-then-close, rollback-then-close, and explicit force termination.
- Donor parser mapping tests proving FirebirdSQL and other donor lifecycle commands either map to ScratchBird lifecycle behavior or return exact emulated/non-file diagnostics.
- Fault injection tests for partial create, interrupted tx1, interrupted tx2, stale owner token, corrupted startup page, incompatible format, resource seed mismatch, auth denial, transaction admission denial, open while dropping, shutdown while active, and crash before clean-shutdown evidence.
- Fault injection tests for missing first-filespace registration, duplicate filespace UUID, database/filespace UUID mismatch, stale filespace manifest, missing secondary filespace, inconsistent filespace header, failed move, failed promote, and drop with active pins.
- No-WAL/no-parser-finality/no-donor-authority static gates.
- Bounded-memory tests for bootstrap and repair on large catalog/resource seeds.

## Completion Rule

The execution_plan can move to `project/tests/public_migrated_proof/completed/` only after `DBLC_P18_FINAL_CLEAN` passes and the final audit proves:

- zero lifecycle operations unimplemented;
- zero lifecycle registry rows unassigned;
- zero canonical spec gaps for lifecycle behavior;
- zero message-vector gaps for lifecycle errors;
- zero CTest lifecycle gates missing or failing;
- zero existing implementation paths retaining old lifecycle-contract behavior;
- zero lifecycle protocol or persisted-format versioning gaps;
- zero lifecycle states, transitions, diagnostics, invalid transitions, operation families, or routes missing spec-to-test traceability;
- zero unsupported or ambiguous upgrade/migration paths that do not fail closed;
- zero implicit policy defaults outside `public_input_snapshot`;
- zero default policy values, override classes, seed states, or diagnostic rules duplicated outside the manifest-listed machine-readable registry and generated seed-data path;
- zero `POLICY.*` diagnostics missing canonical diagnostic-code, diagnostic-shape, message-vector, redaction, retryability, audit, or owner metadata;
- zero manifest-listed default policy contract, registry, conformance, or diagnostic files that are ignored, missing, duplicated, or untrackable;
- zero base `sys.catalog` tables duplicating human-facing SQL object names outside the identity resolver surface;
- zero `sys.information` projection views bypassing the information projection resolver, authorization filtering, redaction policy, or MGA snapshot visibility;
- zero client-facing database metadata routes reading raw `sys.catalog` when a `sys.information` projection exists unless an explicit privileged diagnostic path is being tested;
- zero UUID B-tree indexes claiming commit order, creation order, history order, range authority, or transaction finality;
- zero implicit catalog, index, concurrency, temp, event, encryption, resource seed, MGA GC, background job, cluster-boundary, security principal, storage allocation, executable object, sequence generator, supportability, capability, or replication boundary lifecycle surfaces;
- zero authority drift from MGA, engine-owned authentication, or SBLR-only engine boundaries.

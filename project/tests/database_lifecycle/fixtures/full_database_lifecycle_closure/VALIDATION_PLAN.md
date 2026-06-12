# Database Lifecycle Validation Plan

Status: passed
Search key: `DATABASE-LIFECYCLE-VALIDATION-PLAN`

## Required CTest Labels

| Label | Required coverage |
| --- | --- |
| `database_lifecycle_unit` | Startup state, durable lifecycle state, transaction inventory evidence, owner-token parsing, registry dispatch, and diagnostic vector unit tests. |
| `database_lifecycle_engine_api` | Engine internal API and public ABI lifecycle operation tests. |
| `database_lifecycle_storage` | Create/open/reopen/recovery/shutdown/drop storage tests with tx1, tx2, and final transaction evidence. |
| `database_lifecycle_filespace` | Filespace lifecycle tests for UUIDv7 identity, first-filespace registration, header/manifest/catalog consistency, attach/detach/promote/demote/read-only/read-write/move/verify/repair/quarantine/compact/truncate/drop, and database lifecycle coupling. |
| `database_lifecycle_security` | Engine-owned authentication, authorization, attach/session, policy, redaction, deny-message-vector tests. |
| `database_lifecycle_server_route` | sb_server lifecycle hosting, parser-server IPC, listener, SBWP/TLS, database-scoped shutdown notification, acknowledgement, drain, force shutdown, and route tests. |
| `database_lifecycle_manager` | Manager-family tests for `sbmn_manager` and `sbmc_manager` scope: config, owner token, proxy/management bind, heartbeat, listener/server supervision, cluster-manager private/fail-closed behavior, drain, stop, restart policy, failed state, quarantine, and database isolation. |
| `database_lifecycle_listener` | Listener launch, bind, parser-pool lifecycle, health, accept, handoff, drain, reload, stop, forced parser-pool stop, failed state, quarantine, and database isolation tests. |
| `database_lifecycle_parser` | Parser-family package admission, worker spawn, HELLO/HELLO_ACK, handoff, pre-auth relay, attach, active request, cancel, drain, disconnect, recycle, terminate, failed state, quarantine, and no-authority-bypass tests for any parser package or parser worker. |
| `database_lifecycle_process_association` | Database-to-manager/listener/parser/session/attachment/route/heartbeat/shutdown association registry, listener failure fallback, stale evidence, and missing evidence tests. |
| `database_lifecycle_server_daemon` | Server daemon lifecycle startup, service-ready, hosted database association, shared/dedicated daemon isolation, graceful stop, force stop, restart, orphan cleanup, failed state, quarantine, and database isolation tests. |
| `database_lifecycle_ipc` | Parser-server, server-management, and internal IPC endpoint descriptor, admission, authentication, authorization, frame validation, backpressure, drain, stale cleanup, failure, quarantine, and shutdown tests. |
| `database_lifecycle_session_request_cursor` | Attachment, session, request, statement, cursor, cancel, timeout, disconnect, cleanup, unknown transaction outcome, metrics, and diagnostics tests. |
| `database_lifecycle_engine_agent` | Database/engine lifecycle agent tests for policy-admitted agent selection, health supervision, startup/shutdown coordination, safe mode, connection health reporting, failure, quarantine, and no authority bypass. |
| `database_lifecycle_cache_checkpoint` | Buffer pool, page-cache, checkpoint, preload, dirty tracking, writeback, clean-close evidence, eviction, memory pressure, shutdown flush, recovery classification, and no-finality-substitution tests. |
| `database_lifecycle_default_policy_catalog` | Default policy catalog tests for every lifecycle policy family, required property, default value, override class, UUIDv7 seed row, tx1 visibility, policy generation, cache invalidation, and fail-closed boundary. |
| `database_lifecycle_default_policy_registry` | Machine-readable default policy registry tests proving Markdown packet, registry YAML, generated seed data, conformance manifest, manifest entries, family count, property schemas, default values, override classes, and policy generation rules remain identical. |
| `database_lifecycle_policy_diagnostic_registry` | Policy diagnostic registry tests proving every `POLICY.*` code has canonical diagnostic-code and diagnostic-shape rows, message-vector mapping, owner, severity, retryability, redaction, audit policy, and safe disclosure behavior. |
| `database_lifecycle_policy_override_fixtures` | Policy override fixture tests for every override class, including accepted create-time overrides, rejected forbidden overrides, rejected stale policy generation, unauthorized security-admin/sysarch overrides, and standalone cluster-only refusal. |
| `database_lifecycle_config_policy_security_provider` | Configuration source epochs, policy reload, security-provider/plugin lifecycle, default policy install, password-hash verification path, cache invalidation, stale refusal, metrics, and diagnostics tests. |
| `database_lifecycle_backup_archive_restore` | Backup, archive, restore, shadow, snapshot, holds, legal retention, restore-inspection open, filespace interaction, shutdown/drop blockers, recovery evidence, and no live-file shortcut tests. |
| `database_lifecycle_udr_extension` | UDR and extension registration, load, unload/quiesce, policy checks, parser-support UDR behavior, resource limits, shutdown cleanup, diagnostics, and no authority bypass tests. |
| `database_lifecycle_workload_resource` | Workload classes, resource pools, admission, throttling, cancellation, memory/thread/temp/filespace quotas, maintenance, shutdown, failure, metrics, and diagnostics tests. |
| `database_lifecycle_catalog_object` | Catalog schema object dependency tests for DDL create/alter/drop/rename, UUID/name registry, dependency graph, metadata cache invalidation, schema ownership, authorization, and MGA visibility. |
| `database_lifecycle_index_statistics_plan` | Index build/drop/rebuild, statistics refresh, optimizer plan cache invalidation, collation/charset epoch, interrupted build recovery, and catalog/MGA consistency tests. |
| `database_lifecycle_lock_wait_deadlock` | Lock table, latch, wait queue, cancellation, timeout, disconnect cleanup, deadlock detection, shutdown cleanup, and MGA authority boundary tests. |
| `database_lifecycle_temp_workspace` | Temporary filespace, session temp object, sort/hash spill, workspace quota, commit/rollback/disconnect/shutdown cleanup, and recovery classification tests. |
| `database_lifecycle_event_notification` | LISTEN/NOTIFY, post event, subscription, delivery ordering, security filtering, disconnect cleanup, shutdown, recovery, metrics, and diagnostics tests. |
| `database_lifecycle_encryption_key` | Key admission, encrypted filespace open, key cache lifetime, rotation, shutdown purge, missing-key refusal, protected-material redaction, and diagnostics tests. |
| `database_lifecycle_resource_seed` | Timezone, charset, collation, locale, resource seed versioning, upgrade/refusal, cache invalidation, index dependency, runtime epoch, and diagnostics tests. |
| `database_lifecycle_mga_gc_retention` | MGA garbage collection, sweep, old-version cleanup, history retention, backup/archive holds, limbo/unknown outcome protection, bounded memory, and no-finality-substitution tests. |
| `database_lifecycle_jobs_scheduler` | Background jobs and database-local scheduler startup, pause/resume, retry, quarantine, maintenance, shutdown drain, policy authority, resource limit, and failure tests. |
| `database_lifecycle_cluster_boundary` | Cluster-boundary fail-closed tests proving standalone execution cannot enter cluster transaction, route, metric, agent, schema, or lifecycle paths without cluster authority. |
| `database_lifecycle_security_principal` | Security principal, role, privilege, policy, user, group, grant, revoke, row-security, definer-rights, cache invalidation, audit, authorization, and MGA visibility tests. |
| `database_lifecycle_storage_allocation` | Storage allocation, free-space map, page-map, extent reservation, page ownership, reusable-space, compaction, crash recovery, and filespace-coupling tests. |
| `database_lifecycle_executable_object` | Routine, procedure, function, trigger, event trigger, package, stored SBLR, dependency, permission, invalidation, side-effect, and unload/quiesce tests. |
| `database_lifecycle_sequence_generator` | Sequence, generator, identity, cache-window, persistence, transaction-interaction, crash-recovery, reference-mapping, and diagnostic tests. |
| `database_lifecycle_supportability_evidence` | Operational log, audit evidence, retention, rotation, redaction, export, support-bundle, shutdown flush, diagnostic access, and protected-material filtering tests. |
| `database_lifecycle_capability_profile` | Installed capability, parser profile, edition gate, feature flag, package availability, policy epoch, downgrade refusal, and diagnostic tests. |
| `database_lifecycle_replication_boundary` | Replication, CDC, changefeed, live-ingest, publication, subscription, slot, route, retention, security, reference mapping, and standalone fail-closed boundary tests. |
| `database_lifecycle_existing_reconciliation` | Existing manager, listener, parser, server, IPC, session, filespace, catalog, index, concurrency, temporary workspace, event, encryption, resource seed, MGA GC, jobs, cluster-boundary, security principal, storage allocation, executable object, sequence generator, supportability, capability, replication, UDR, agent, cache, configuration, security, backup, resource, and workload implementation reconciliation tests. |
| `database_lifecycle_protocol_versioning` | SBWP/TLS, parser IPC, management IPC, lifecycle state file, filespace header, manifest, catalog row, and configuration epoch versioning, migration, downgrade, and fail-closed refusal tests. |
| `database_lifecycle_admin_cli` | CLI/admin/client lifecycle command authorization, diagnostics, audit, idempotency, force shutdown, inspect, verify, repair, health, status, and drop tests. |
| `database_lifecycle_packaging_service` | Install, start, stop, restart, uninstall, runtime directory, PID/owner file, permission, service isolation, stale endpoint cleanup, and database-scoped service tests. |
| `database_lifecycle_traceability` | Generated traceability from lifecycle specs, states, transitions, diagnostics, invalid transitions, operation families, and route paths to CTest or static gates. |
| `database_lifecycle_upgrade_migration` | Upgrade, migration, and fail-closed refusal tests for older database files, configuration, lifecycle state files, protocol descriptors, filespace manifests, and catalog rows. |
| `database_lifecycle_threat_model` | Threat-model and abuse-case tests for force shutdown, IPC auth, supervision, UDR loading, health reporting, backup/restore, service files, and resource quotas. |
| `database_lifecycle_shutdown_notification` | Database-scoped shutdown notification, acknowledgement, graceful drain, force shutdown, shared/dedicated daemon isolation, listener-failure parser fallback, and stale/missing association refusal tests. |
| `database_lifecycle_catalog_index_profile` | System catalog physical index profile tests proving UUID exact lookups use hash-capable equality profiles where enabled, B-tree is used only for ordered/group/prefix/generation/history access or bootstrap fallback, `sys.catalog` base tables do not duplicate human-facing SQL object names, and the identity resolver is the sole human-name authority. |
| `database_lifecycle_sys_information_projection` | `sys.information` projection tests proving SQL-standard information schema views and ScratchBird extended user-friendly catalog views join `sys.catalog` UUID tables to the information projection resolver with authorization filtering language fallback redaction and MGA snapshot visibility while clients use `sys.information` instead of raw `sys.catalog` for database metadata. |
| `database_lifecycle_parser_route` | SBSQL parser lifecycle command mapping and end-to-end response rendering. |
| `database_lifecycle_reference_mapping` | FirebirdSQL and reference lifecycle mapping/emulation diagnostics. |
| `database_lifecycle_fault_injection` | Partial create, interrupted tx1/tx2, unclean shutdown, stale owner, corruption, auth denial, memory/resource pressure, and cluster path fail-closed tests. |
| `database_lifecycle_exhaustive` | Generated replay of every lifecycle operation, state transition, invalid transition, and route classification. |
| `database_lifecycle_release` | Final zero-open audit, release declaration, and no-overclaim gates. |
| `database_lifecycle_canonical_spec_closure` | Canonical lifecycle contract and default-policy contract closure gate. |
| `mga_transaction_regression` | Cross-slice MGA transaction authority regression aggregation. |
| `sbsql_parser_worker` | SBSQL parser worker route and parser-to-engine regression aggregation. |

## Gate Commands

The exact commands must be materialized in `artifacts/database_lifecycle_validation_commands.md` during `DBLC-000C`. The final command set must include:

```bash
ctest --test-dir build --output-on-failure -L database_lifecycle_unit
ctest --test-dir build --output-on-failure -L database_lifecycle_engine_api
ctest --test-dir build --output-on-failure -L database_lifecycle_storage
ctest --test-dir build --output-on-failure -L database_lifecycle_filespace
ctest --test-dir build --output-on-failure -L database_lifecycle_security
ctest --test-dir build --output-on-failure -L database_lifecycle_server_route
ctest --test-dir build --output-on-failure -L database_lifecycle_manager
ctest --test-dir build --output-on-failure -L database_lifecycle_listener
ctest --test-dir build --output-on-failure -L database_lifecycle_parser
ctest --test-dir build --output-on-failure -L database_lifecycle_process_association
ctest --test-dir build --output-on-failure -L database_lifecycle_server_daemon
ctest --test-dir build --output-on-failure -L database_lifecycle_ipc
ctest --test-dir build --output-on-failure -L database_lifecycle_session_request_cursor
ctest --test-dir build --output-on-failure -L database_lifecycle_engine_agent
ctest --test-dir build --output-on-failure -L database_lifecycle_cache_checkpoint
ctest --test-dir build --output-on-failure -L database_lifecycle_default_policy_catalog
ctest --test-dir build --output-on-failure -L database_lifecycle_default_policy_registry
ctest --test-dir build --output-on-failure -L database_lifecycle_policy_diagnostic_registry
ctest --test-dir build --output-on-failure -L database_lifecycle_policy_override_fixtures
ctest --test-dir build --output-on-failure -L database_lifecycle_config_policy_security_provider
ctest --test-dir build --output-on-failure -L database_lifecycle_backup_archive_restore
ctest --test-dir build --output-on-failure -L database_lifecycle_udr_extension
ctest --test-dir build --output-on-failure -L database_lifecycle_workload_resource
ctest --test-dir build --output-on-failure -L database_lifecycle_catalog_object
ctest --test-dir build --output-on-failure -L database_lifecycle_index_statistics_plan
ctest --test-dir build --output-on-failure -L database_lifecycle_lock_wait_deadlock
ctest --test-dir build --output-on-failure -L database_lifecycle_temp_workspace
ctest --test-dir build --output-on-failure -L database_lifecycle_event_notification
ctest --test-dir build --output-on-failure -L database_lifecycle_encryption_key
ctest --test-dir build --output-on-failure -L database_lifecycle_resource_seed
ctest --test-dir build --output-on-failure -L database_lifecycle_mga_gc_retention
ctest --test-dir build --output-on-failure -L database_lifecycle_jobs_scheduler
ctest --test-dir build --output-on-failure -L database_lifecycle_cluster_boundary
ctest --test-dir build --output-on-failure -L database_lifecycle_security_principal
ctest --test-dir build --output-on-failure -L database_lifecycle_storage_allocation
ctest --test-dir build --output-on-failure -L database_lifecycle_executable_object
ctest --test-dir build --output-on-failure -L database_lifecycle_sequence_generator
ctest --test-dir build --output-on-failure -L database_lifecycle_supportability_evidence
ctest --test-dir build --output-on-failure -L database_lifecycle_capability_profile
ctest --test-dir build --output-on-failure -L database_lifecycle_replication_boundary
ctest --test-dir build --output-on-failure -L database_lifecycle_existing_reconciliation
ctest --test-dir build --output-on-failure -L database_lifecycle_protocol_versioning
ctest --test-dir build --output-on-failure -L database_lifecycle_admin_cli
ctest --test-dir build --output-on-failure -L database_lifecycle_packaging_service
ctest --test-dir build --output-on-failure -L database_lifecycle_traceability
ctest --test-dir build --output-on-failure -L database_lifecycle_upgrade_migration
ctest --test-dir build --output-on-failure -L database_lifecycle_threat_model
ctest --test-dir build --output-on-failure -L database_lifecycle_shutdown_notification
ctest --test-dir build --output-on-failure -L database_lifecycle_catalog_index_profile
ctest --test-dir build --output-on-failure -L database_lifecycle_sys_information_projection
ctest --test-dir build --output-on-failure -L database_lifecycle_parser_route
ctest --test-dir build --output-on-failure -L database_lifecycle_reference_mapping
ctest --test-dir build --output-on-failure -L database_lifecycle_fault_injection
ctest --test-dir build --output-on-failure -L database_lifecycle_exhaustive
ctest --test-dir build --output-on-failure -L database_lifecycle_release
ctest --test-dir build --output-on-failure -L mga_transaction_regression
ctest --test-dir build --output-on-failure -L sbsql_parser_worker
```

## Static Gates

Static gates must prove:

- no authoritative WAL/finality substitution was introduced;
- no parser accepts or denies authentication as authority;
- no reference parser or reference tool executes SQL inside the engine boundary;
- no cluster lifecycle path is taken without cluster authority;
- no placeholder, TODO, stub, future, or deferred lifecycle behavior remains in accepted code paths;
- no filespace identity is accepted from path, timestamp, physical order, page number, or UUID ordering instead of durable filespace UUIDv7 registry/header/manifest evidence;
- no database/engine lifecycle agent becomes transaction, storage, catalog, security, authentication, authorization, policy, or SBLR execution authority;
- no create-database, open, attach, transaction, lifecycle, parser, route, security, resource, supportability, reference, replication, or cluster-boundary behavior uses an implicit default policy outside `public_input_snapshot`;
- no default policy family, property, default value, override class, or seed-state value is hardcoded outside the manifest-listed machine-readable default policy registry and generated seed-data path;
- no `POLICY.*` diagnostic can be emitted unless it has canonical diagnostic-code and diagnostic-shape registry rows and a message-vector mapping;
- no default policy registry/conformance artifact listed in `public_contract_snapshot` is ignored, missing, duplicated in the manifest, or untrackable by repository hygiene checks;
- no checkpoint, cache flush, backup marker, archive marker, shadow, snapshot, IPC state, session state, UDR state, or resource-quota state is accepted as transaction finality evidence;
- no external live-file backup, restore, shadow, or snapshot shortcut satisfies an engine-owned lifecycle operation;
- no existing implementation is allowed to retain old lifecycle assumptions after the reconciliation slice closes;
- no lifecycle protocol or persisted format is accepted without explicit version and compatibility handling;
- no lifecycle admin or CLI command bypasses authorization, audit, route admission, message vectors, or engine authority;
- no runtime directory, PID file, owner file, service unit, socket, or control endpoint remains stale and routable after cleanup;
- no lifecycle spec state, transition, invalid transition, diagnostic, or route can be untraced to a test or static gate at release;
- no old artifact migration is performed by guessing identity, generation, format, policy, or transaction outcome;
- no threat-model abuse case is closed without a fail-closed test or documented canonical refusal;
- no DDL catalog mutation reports success without UUID/name registry, dependency, cache invalidation, authorization, and MGA visibility evidence;
- no index build, statistics refresh, optimizer plan cache, collation epoch, or charset epoch becomes catalog or transaction authority;
- no lock, latch, wait, deadlock, timeout, cancellation, disconnect, or shutdown cleanup path can orphan authority or bypass MGA;
- no temporary filespace, temp object, spill, sort, or workspace artifact survives its policy lifetime or becomes durable catalog authority;
- no event notification, subscription, audit event, or async delivery path bypasses authorization, redaction, or transaction visibility rules;
- no protected material, key cache, encrypted filespace open, or missing-key refusal path exposes secrets or guesses decryption authority;
- no timezone, charset, collation, locale, or resource seed is used with stale version, stale epoch, or unresolved index dependency;
- no MGA garbage collection, sweep, retention, backup hold, archive hold, limbo, or unknown-outcome path substitutes non-MGA evidence for cleanup or finality;
- no background job or scheduler task starts before admitted activation or continues through maintenance or shutdown outside policy;
- no standalone route enters cluster transaction, route, metric, agent, schema, or lifecycle behavior without cluster authority;
- no security principal, role, group, grant, revoke, row-security, definer-rights, or policy mutation bypasses engine authorization, audit, catalog UUID identity, or MGA visibility;
- no base catalog table duplicates human-facing SQL object names; only the identity resolver surface may store raw, display, normalized, exact, or full-path human-name text for name-to-UUID and UUID-to-name resolution;
- no UUID B-tree index is claimed to provide commit order, creation order, range authority, or transaction finality; catalog history and visibility ordering must use MGA transaction inventory, created/retired transaction fields, catalog generation, policy generation, or explicit history sequence fields;
- no hash index is used for range, order, prefix, language fallback ordering, generation ordering, or history traversal; hash indexes are exact equality accelerators with exact-key collision recheck;
- no page allocation, free-space map, page ownership, extent reservation, compaction, or reusable-space state bypasses filespace identity or recovery evidence;
- no routine, procedure, function, trigger, event trigger, package, stored SBLR, or executable dependency bypasses catalog dependency invalidation, authorization, side-effect policy, or UDR authority boundaries;
- no sequence, generator, or identity cache window becomes transaction finality, catalog truth, or reference-owned state;
- no operational log, audit evidence, support bundle, export, or diagnostic access path exposes protected material or skips retention/redaction policy;
- no installed capability, parser profile, edition gate, feature flag, or package availability check enables unsupported lifecycle behavior or bypasses policy epochs;
- no replication, CDC, changefeed, live-ingest, publication, subscription, or slot route is admitted without exact engine authority or explicit standalone fail-closed behavior;
- no lifecycle error path returns raw strings without canonical message-vector structure.

## Required Evidence Files

| Evidence | Required by |
| --- | --- |
| `artifacts/DATABASE_LIFECYCLE_AUTHORITY_MAP.csv` | `DBLC_P0_AUTHORITY_FROZEN` |
| `artifacts/DATABASE_LIFECYCLE_IMPLEMENTATION_GAP_MATRIX.csv` | `DBLC_P0B_GAP_MATRIX_READY` |
| `artifacts/DATABASE_LIFECYCLE_CTEST_REQUIRED_GATES.csv` | `DBLC_P0C_VALIDATION_READY` |
| `artifacts/DATABASE_LIFECYCLE_AGENT_ORCHESTRATION.md` | `DBLC_P0D_ORCHESTRATION_READY` |
| `artifacts/DATABASE_LIFECYCLE_AGENT_STATUS.csv` | `DBLC_P0D_ORCHESTRATION_READY` |
| `artifacts/DATABASE_LIFECYCLE_AGENT_WRITE_SCOPE_REGISTER.csv` | `DBLC_P0D_ORCHESTRATION_READY` |
| `artifacts/DATABASE_LIFECYCLE_AGENT_HEARTBEAT_LOG.csv` | `DBLC_P0D_ORCHESTRATION_READY` |
| `artifacts/DATABASE_LIFECYCLE_AGENT_FAILURE_INVENTORY.csv` | `DBLC_P0D_ORCHESTRATION_READY` |
| `artifacts/DATABASE_LIFECYCLE_SLICE_EXECUTION_QUEUE.csv` | `DBLC_P0D_ORCHESTRATION_READY` |
| `artifacts/DATABASE_LIFECYCLE_DEFAULT_POLICY_AUDIT.csv` | `DBLC_P0E_DEFAULT_POLICY_CATALOG_READY` |
| `artifacts/DATABASE_LIFECYCLE_DEFAULT_POLICY_REGISTRY_AUDIT.csv` | `DBLC_P0F_POLICY_REGISTRY_HARDENED` |
| `artifacts/DATABASE_LIFECYCLE_REPO_HYGIENE_AUDIT.csv` | `DBLC_P0F_POLICY_REGISTRY_HARDENED` |
| `artifacts/DATABASE_LIFECYCLE_REGRESSION_REPORT.md` | `DBLC_P16_REGRESSION_COMPLETE` |
| `artifacts/final_database_lifecycle_zero_open_audit.py` | `DBLC_P18_FINAL_CLEAN` |
| `FINAL_AUDIT.md` | `DBLC_P18_FINAL_CLEAN` |
| `VALIDATION_RESULT.md` | `DBLC_P18_FINAL_CLEAN` |
| `CLOSURE_REPORT.md` | `DBLC_P18_FINAL_CLEAN` |

## Full-Route Lifecycle Scenarios

The final regression suite must run these scenarios through the client-to-engine route:

- Create database with generated UUIDv7 database identity and full tx1 system bootstrap.
- Create database seeds every required default policy family in `sys.policy` with fresh UUIDv7 rows, generation 1, required properties, exact default values, and no implicit or missing policy behavior.
- Create database policy seed data is generated from the manifest-listed machine-readable policy registry and is rejected by static gates if a subsystem carries independent hardcoded default policy values.
- Missing, stale, malformed, duplicate, ignored, or manifest-unlisted default policy registry/conformance files fail before create-database implementation can be accepted.
- Every `POLICY.*` create/open/attach/transaction/lifecycle diagnostic renders through the canonical message-vector registry with required redaction, retryability, audit, and owner metadata.
- Every default policy override class is exercised through accepted and rejected full-route fixtures, including standalone refusal for cluster-only policy requests.
- Create database with a generated UUIDv7 database identity and a distinct generated UUIDv7 first-filespace identity for the first physical database file.
- The first filespace is registered as `active_primary` in the durable filespace header, startup directory, filespace manifest, and catalog/name registry before create success is returned.
- First open creates tx2 activation evidence and starts database-specific runtime.
- Attach with valid credentials creates session and admits transaction.
- Attach with invalid credentials returns deny message vector and creates no session/transaction.
- Create user reads policy and creates a default home schema under configured policy.
- Read and write work occurs only after transaction admission.
- Detach releases session and transaction resources.
- Enter and exit maintenance mode with admission fencing.
- Enter and exit restricted open with permitted diagnostic behavior only.
- Inspect and verify database without write authority.
- Repair database only with explicit repair authority and preserved evidence.
- Clean shutdown persists final transaction evidence and releases owner token.
- Reopen clean database follows clean checkpoint path.
- Reopen unclean database classifies recovery before user-visible work.
- Drop database refuses unsafe state and completes safe local removal or quarantine by policy.

## Filespace Lifecycle Scenarios

The final regression suite must include:

- Every filespace, including the first physical database file, receives a generated UUIDv7 filespace identity distinct from the database UUID and from every other filespace.
- Filespace registration persists database UUID, filespace UUID, role, state, path or locator, physical filespace id, page size, lifecycle generation, owner capabilities, and evidence pointer before lifecycle success is returned.
- Open validates required filespace headers, startup directory, manifest, catalog row, active-primary uniqueness, page size, database UUID, filespace UUID, and generation before exposing catalog state.
- Attach and transaction admission fail closed when the target filespace set is missing, stale, quarantined, duplicate, or inconsistent.
- Maintenance and restricted-open modes fence filespace mutation while still allowing authorized verify/repair behavior.
- Verify reports filespace header/manifest/catalog mismatches with canonical message vectors and does not repair unless repair authority is present.
- Repair either produces preserved evidence and durable corrected filespace state or refuses/quarantines without guessing identity.
- Shutdown persists filespace lifecycle state, dirty/clean evidence, metrics, and cache invalidation before releasing ownership.
- Recovery classifies incomplete filespace create, attach, detach, move, promote, demote, repair, and drop operations before ordinary access.
- Drop refuses active pins or unsafe ownership and otherwise removes or quarantines every database-associated filespace by policy.

## Manager Listener Parser Lifecycle Scenarios

The final regression suite must include:

- `manager` is tested as the family shorthand for `sbmn_manager` and `sbmc_manager`; product-specific assertions must name the exact product.
- `sbmn_manager` starts from configuration, acquires owner token, binds proxy and management endpoints, starts heartbeat, supervises listeners/server only by policy, drains, stops, restarts by policy, and enters quarantine on repeated failures.
- `sbmc_manager` cluster-manager lifecycle surfaces remain private/cluster-scoped and fail closed in non-cluster execution until cluster authority is present.
- Manager-family lifecycle for one database cannot control or leak state from managers associated only with another database.
- Listener validates launch descriptor, binds management and edge sockets, starts parser pool, verifies parser health, accepts clients, hands off sockets, drains, reloads, stops, force-stops parser pool, and enters quarantine on unsafe failure.
- Listener lifecycle for one database cannot drain, stop, force-stop, or recycle parser workers associated only with another database.
- `parser` is tested as the family shorthand for any parser package or parser worker; parser-specific assertions must name the exact parser package or product.
- Parser workers are admitted by package/profile, complete HELLO/HELLO_ACK, become idle, receive handoff, relay pre-auth, attach only through server/engine authority, process admitted SBLR route, cancel, drain, disconnect, recycle, terminate, and enter quarantine on unsafe failure.
- Parser-family lifecycle cannot authenticate, authorize, own transaction finality, read database files, execute SQL in the engine, or bypass SBLR/server admission.
- Process association registry records database UUID, manager identity, listener identity, parser identity, IPC endpoint identity, route generation, heartbeat generation, session/attachment identity where present, and shutdown generation.
- Stale or missing manager/listener/parser association evidence fails closed with a canonical message vector rather than controlling processes by guesswork.

## Server IPC Session Agent Cache Security Extension Resource Scenarios

The final regression suite must include:

- `sb_server` starts, validates configuration, publishes service-ready only after required hosted database associations and routes are valid, and refuses ambiguous shared/dedicated daemon scope.
- Shared server-daemon shutdown drains only the target database while unrelated database routes remain active; dedicated daemon shutdown may stop the daemon only when its database association is exclusive.
- IPC endpoint descriptors are created, authenticated, authorized, frame-validated, drained, and removed by lifecycle policy; malformed frames, stale descriptors, and unauthorized IPC fail closed.
- Sessions, attachments, requests, statements, and cursors have explicit identity, admission, cancel, timeout, cleanup, disconnect, and unknown-outcome behavior.
- Database/engine lifecycle agent starts after tx2 activation, selects policy-admitted database-local agents, monitors agent health, coordinates startup and shutdown, reports database health to connections, enters safe mode by policy, and cannot override engine authority.
- Buffer pool and page cache preload, dirty tracking, writeback, eviction, memory pressure, checkpoint force/wait/try, clean-close evidence, shutdown flush, and recovery classification are tested without treating cache or checkpoint state as finality authority.
- Configuration and policy source epochs invalidate caches and reject stale admission; security providers/plugins load and quiesce by policy and engine-owned password-hash verification never stores or compares cleartext passwords.
- Backup, archive, restore, shadow, and snapshot operations use engine-owned admitted paths, create holds/blockers where required, interact with filespace lifecycle, and block unsafe shutdown/drop until policy requirements are satisfied.
- UDR and extension packages register, load, unload/quiesce, enforce policy and resource limits, clean up during shutdown, and cannot bypass parser, SBLR, transaction, catalog, storage, or security authority.
- Workload classes and resource pools enforce memory, thread, temp-space, filespace, transaction-slot, parser/listener, IPC, and UDR quotas during attach, transaction admission, maintenance, shutdown, cancellation, and failure handling.
- Existing manager, listener, parser, server, IPC, session, filespace, catalog, index, concurrency, temporary workspace, event, encryption, resource seed, MGA GC, jobs, cluster-boundary, security principal, storage allocation, executable object, sequence generator, supportability, capability, replication, UDR, agent, cache, configuration, security, backup, resource, and workload implementations are audited and patched against the updated lifecycle specs.
- SBWP/TLS, parser IPC, management IPC, lifecycle state files, filespace headers, manifests, catalog rows, and configuration epochs reject unknown versions and migrate only supported versions.
- CLI/admin/client command coverage reaches each lifecycle operation through the intended route with authorization, diagnostics, audit, and idempotency behavior.
- Packaging and service tests cover install, start, stop, restart, uninstall, runtime directories, PID/owner files, permissions, stale socket cleanup, and database-scoped service isolation.
- Traceability generation fails the suite if any lifecycle state, transition, invalid transition, diagnostic, operation family, or route path lacks a CTest or static-gate target.
- Upgrade and migration tests cover supported older artifacts and canonical refusal for unsupported or ambiguous database files, config files, lifecycle state files, protocol descriptors, filespace manifests, and catalog rows.
- Threat-model tests exercise force shutdown abuse, IPC auth bypass, manager/listener/parser supervision abuse, UDR loading abuse, health information disclosure, backup/restore abuse, service-file abuse, and resource-quota abuse.

## Additional Lifecycle Hardening Scenarios

The final regression suite must include:

- Catalog DDL creates, alters, drops, and renames objects through UUID/name registry updates, dependency graph updates, metadata cache invalidation, authorization, rollback visibility, and commit visibility.
- System catalog physical index profiles cover every bootstrap catalog table and define which access paths use hash equality, which use B-tree ordered/group/generation/history traversal, and which use MGA transaction inventory or catalog generation instead of UUID ordering.
- `sys.catalog` contains low-level system catalog tables for SQL objects, policies, resources, metrics, security, storage, and executable objects. Those tables contain UUIDs, machine-stable keys, parent and child UUID relationships, typed descriptors, transaction fields, generation fields, and history fields only; human-facing SQL names and localized display text exist only in the identity resolver surface.
- `sys.information` contains SQL-standard information schema compatible views plus ScratchBird extended user-friendly views. Those views project `sys.catalog` through the information projection resolver, language fallback, authorization filtering, redaction policy, and MGA snapshot visibility so clients can read metadata without treating raw `sys.catalog` as the user-facing API.
- Static catalog projection gates prove no client-facing database information route bypasses `sys.information` when a projection exists, no `sys.information` view owns identity or transaction visibility, and no SQL-standard information view exposes raw internal UUIDs unless a documented ScratchBird extended view or privileged diagnostic path permits it.
- Index lifecycle builds, drops, rebuilds, refreshes statistics, invalidates optimizer plans, validates collation and charset epochs, and recovers interrupted builds without parser or cache authority.
- Lock and wait lifecycle handles latch acquisition, wait queues, timeout, cancellation, disconnect cleanup, deadlock victim selection, shutdown cleanup, and unknown transaction outcome diagnostics under MGA authority.
- Temporary lifecycle covers temp filespaces, session temp objects, sort/hash spill, workspace quotas, commit cleanup, rollback cleanup, disconnect cleanup, shutdown cleanup, and recovery cleanup.
- Event notification lifecycle covers LISTEN/NOTIFY, post event, subscription activation, delivery ordering, authorization filtering, redaction, disconnect cleanup, shutdown, and recovery behavior.
- Encryption lifecycle covers key admission, encrypted filespace open, key cache lifetime, key rotation, missing-key refusal, shutdown purge, protected-material diagnostics, and redaction.
- Resource seed lifecycle covers timezone, charset, collation, locale, resource seed version upgrades, unsupported version refusal, cache invalidation, index dependency checks, and runtime epoch checks.
- MGA garbage collection lifecycle covers old-version cleanup, sweep, history retention, backup/archive holds, limbo transaction protection, unknown outcome protection, recovery interaction, and bounded memory.
- Background scheduler lifecycle covers database-local job startup after tx2 activation, pause/resume, retry, quarantine, maintenance participation, shutdown drain, policy authority, resource limits, and failure diagnostics.
- Cluster-boundary lifecycle covers every cluster-private transaction, route, metric, agent, schema, and lifecycle path failing closed in standalone mode until cluster mapping and authority exist.
- Security principal lifecycle covers users, roles, groups, grants, revokes, row security, definer rights, policy rows, cache invalidation, audit, authorization, and MGA visibility.
- Storage allocation lifecycle covers page allocation, free-space maps, page ownership, extent reservations, reusable space, compaction, crash recovery, and filespace coupling.
- Executable object lifecycle covers routines, procedures, functions, triggers, event triggers, packages, stored SBLR, dependencies, permissions, invalidation, side effects, and unload/quiesce behavior.
- Sequence generator lifecycle covers sequence identity, generator state, identity columns, cache windows, persistence, transaction interaction, crash recovery, reference mapping, and diagnostics.
- Operational evidence lifecycle covers logs, audit evidence, retention, rotation, redaction, export, support bundles, shutdown flush, diagnostic access, and protected-material filtering.
- Capability profile lifecycle covers installed capabilities, parser profiles, edition gates, feature flags, package availability, policy epochs, downgrade refusal, and diagnostics.
- Replication boundary lifecycle covers replication, CDC, changefeed, live ingest, publication, subscription, slot, route, retention, security, reference mapping, and standalone fail-closed behavior.

## Shutdown Notification Scenarios

The shutdown regression suite must include:

- Graceful shutdown notifies only managers, listeners, parsers, IPC daemons, sessions, and clients associated with the target database.
- A different database hosted by the same server daemon receives no shutdown notification and continues to admit permitted work.
- A database using a dedicated server daemon shuts down only its dedicated daemon and associated routing components.
- A database using a shared server daemon drains only target-database components and leaves the shared daemon alive when other databases still use it.
- Associated managers, listeners, parsers, IPC daemons, and clients acknowledge shutdown notification before the clean-shutdown final transaction is reported externally.
- Non-force shutdown fences new connections and new transactions while allowing existing clients to commit or rollback within the policy-defined or command-specified drain window.
- Client commit during drain closes that client connection cleanly after commit evidence is durable.
- Client rollback during drain closes that client connection cleanly after rollback evidence is durable.
- Drain timeout follows policy exactly and returns a canonical message vector if force was not requested.
- Explicit force shutdown terminates all target-database associated managers, listeners, parsers, IPC daemons, sessions, and client connections without terminating unrelated database components.
- Listener failure does not hide target-database parser instances from engine shutdown because the engine uses the parser association registry to shut down or terminate only parsers associated with the target database.
- Stale or missing parser association evidence fails closed with a canonical message vector instead of terminating parsers by guesswork.

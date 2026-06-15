# Database Lifecycle

## Purpose

A ScratchBird database passes through a well-defined sequence of states from creation through normal operation to shutdown or drop. Understanding these states helps operators diagnose why an open is refused, why a database is in maintenance mode, or why the engine requires recovery before admitting ordinary work.

This chapter describes each lifecycle operation, the conditions that cause the engine to refuse or restrict an open, the durable evidence the engine records at each phase, and a smoke test you can run against a disposable database to confirm end-to-end behavior.

## Lifecycle Phases

The engine tracks each database's current phase in `DatabaseLifecyclePhase`:

| Phase | Meaning |
|-------|---------|
| `created` | Bootstrap transactions committed; not yet opened for ordinary work |
| `opened` | Available for ordinary connections |
| `closed` | Cleanly shut down |
| `maintenance` | Entered maintenance mode; ordinary attaches blocked |
| `restricted_open` | Restricted-open mode active |
| `inspected` | An INSPECT DATABASE command completed |
| `verified` | A VERIFY DATABASE command completed |
| `repaired` | A REPAIR DATABASE command completed |
| `dropped` | Drop evidence recorded |
| `quarantined` | Engine placed the database in quarantine due to ambiguous identity or integrity failure |
| `failed` | Lifecycle operation failed |

The engine also tracks a `StartupLifecycleDurablePhase` in the startup state record on disk. This persisted phase is distinct from the in-memory phase and is used during recovery to determine what operations were completed before a crash.

## Database Identity and Catalog Bootstrap

Every database has a stable UUID assigned at creation time. The startup state record (`startup_state.cpp`) stores both the `database_uuid` and the `first_filespace_uuid` in its on-disk header at fixed offsets, along with a startup magic byte sequence `SBSTV001`. These fields are checked at every open to confirm that the file being opened belongs to the expected database.

At creation, the engine runs two bootstrap transactions:

- **Transaction 1 (tx1)** seeds the catalog with all bootstrap schema roots (`sys`, `sys.catalog`, `sys.security`, `sys.metrics`, `sys.audit`, `sys.storage`, `sys.parser`, `sys.diagnostics`, `sys.information`, `sys.information_schema`, `users`, `users.public`, `remote`, `emulated`, and others), the built-in datatypes, default policies, agent configurations, resource seeds, and the initial security principal. Evidence flag `bootstrap_tx1_committed` is set after tx1 completes.
- **Transaction 2 (tx2)** activates the runtime: it starts agents, loads IPC, enables the server listener, and completes the first-open activation. Evidence flag `first_open_tx2_committed` is set after tx2 completes. The `lifecycle.first_open.tx2_activation` policy (`runtime_activation_v1`) governs this: ordinary work cannot proceed until tx2 commits.

The bootstrap transaction ID for tx1 is defined as `kBootstrapCatalogTransactionId = 1`. The first-open activation transaction is `kFirstOpenActivationLocalTransactionId = 2`.

## Recovery Classification

When the engine opens a database file, it reads the startup state record and classifies the prior session's cleanup:

| Classification | Meaning |
|----------------|---------|
| `clean_checkpoint_path` | Prior session shut down cleanly; no recovery needed |
| `checkpoint_rebuild_required` | Checkpoint must be rebuilt before ordinary work |
| `repaired_recovery` | File was previously repaired; verify state before proceeding |
| `fence_writes_until_safe` | Write admission must remain fenced until recovery completes |
| `corruption_stop` | Corruption detected; restricted open or repair required |
| `restricted_open_required` | Engine requires restricted-open mode |
| `operator_review_required` | Engine requires explicit operator action before opening |

The `lifecycle.recovery_dirty_open` bootstrap policy (`mga_recovery_first_v1`) governs the recovery path. A dirty open (prior session did not record `clean_shutdown`) triggers MGA (Multi-Generation Architecture) transaction recovery before any ordinary work is admitted. The write admission fence (`kFlagWriteAdmissionFenced` in the startup state flags) prevents new writes until recovery determines it is safe.

The startup state record also tracks the `kFlagCleanShutdown` and `kFlagStartupDirty` flags. A file where `kFlagStartupDirty` is set and `kFlagCleanShutdown` is not set indicates the database was not cleanly closed; recovery runs before new work is admitted.

## What Happens When an Open Is Refused

Several conditions cause the engine to refuse an ordinary open and return a diagnostic code:

**Format version downgrade refused.** If the file's format minor version is below `kDatabaseFormatMinorMinSupported`, the engine returns `FORMAT.VERSION_DOWNGRADE_REFUSED`. This prevents an older engine version from opening a file that was written by a newer build that advanced the minor version beyond the minimum the older build understands. The diagnostic includes both the file's format version and the supported minimum.

**Format version unsupported.** If the format major or minor version exceeds `kDatabaseFormatMajorMaxSupported` / `kDatabaseFormatMinorMaxSupported`, the engine returns `FORMAT.VERSION_UNSUPPORTED`. The file was created by a newer build that the current installation cannot read.

**Unknown required compatibility flag.** If the file's `compatibility_flags` field contains bits the engine does not recognize, the engine returns `FORMAT.UNKNOWN_REQUIRED_FLAG`. This prevents an older engine from silently ignoring features it does not implement.

**Database route mismatch.** If the database's route does not match the connection's expected route, the session registry denies attachment with `database_route_mismatch`. See [Parser Registration And Routes](parser_registration_and_routes.md) for details.

**Maintenance mode ordinary attach blocked.** The `lifecycle.maintenance_restricted` policy (`authorized_fence_v1`) sets `ordinary_attach_blocked` when the database is in maintenance mode. Ordinary sessions attempting to attach receive a denial. Only connections with the appropriate authority can attach in this state.

## Maintenance Mode

Maintenance mode is entered with `ENTER DATABASE MAINTENANCE` (or equivalent forms: `ENTER MAINTENANCE`, `SET DATABASE MAINTENANCE`, `ALTER DATABASE ... MAINTENANCE`). It is exited with `EXIT DATABASE MAINTENANCE` (or `EXIT MAINTENANCE`, `CLEAR DATABASE MAINTENANCE`, `ALTER DATABASE ... MAINTENANCE EXIT`).

Entering maintenance mode requires authority (`enter_requires_authority` in the policy). Once entered, ordinary attaches are blocked (`ordinary_attach_blocked`). Verify operations are still permitted in maintenance mode (`verify_allowed`). Repair operations require explicit authority even in maintenance mode (`repair_requires_explicit_authority`).

The durable lifecycle phase transitions to `maintenance_entered` when maintenance mode begins and to `maintenance_exited` when it ends. Evidence flag `maintenance_evidence_recorded` is set.

## Restricted Open Mode

Restricted-open mode is a lighter fence than maintenance mode. It is entered with `OPEN DATABASE ... RESTRICTED` or `ENTER RESTRICTED OPEN` and exited with `EXIT RESTRICTED OPEN`. The durable phase transitions to `restricted_open_entered` and `restricted_open_exited`.

The startup recovery classification `restricted_open_required` causes the engine to require restricted-open mode before ordinary work is admitted. This occurs when the engine detects a state that requires operator confirmation but does not require full maintenance-mode intervention.

## Inspect, Verify, and Repair

Three diagnostic operations are available after a database is opened:

- **INSPECT DATABASE** (also `DIAGNOSE DATABASE`) — reads and reports on the database state without modifying it. Phase transitions to `inspected`.
- **VERIFY DATABASE** — performs structural verification of catalog and storage consistency. Phase transitions to `verified`. Evidence flag `verify_evidence_recorded` is set.
- **REPAIR DATABASE** (also `ALTER DATABASE ... REPAIR`) — attempts to repair structural damage. Phase transitions to `repaired` on success or `repair_refused` on a policy refusal. Evidence flags `repair_evidence_recorded` and `repair_refusal_evidence_recorded` are used accordingly. Repair requires explicit authority even in maintenance mode.

## Shutdown

Graceful shutdown uses `SHUTDOWN DATABASE` and follows the `lifecycle.shutdown_graceful_drain` policy (`drain_then_close_v1`): new work is fenced, active connections are drained, components are notified, and the database is closed after all active transactions commit or roll back. A configurable timeout governs the drain. The `clean_shutdown_local_transaction_id` field and `kFlagCleanShutdown` flag are written to the startup state record on clean close. Evidence flag `clean_shutdown_tx_committed` is set.

Force shutdown uses `FORCE SHUTDOWN DATABASE` or `SHUTDOWN DATABASE ... FORCE` and follows the `lifecycle.shutdown_force` policy (`explicit_force_only_v1`). Force shutdown terminates only the scope of the target database; other databases are not affected (`terminate_target_database_scope_only`). MGA recovery evidence is preserved so that recovery can proceed on the next open.

`ACKNOWLEDGE SHUTDOWN DATABASE` (or `SHUTDOWN ACKNOWLEDGE DATABASE`) acknowledges a pending shutdown state.

## Drop

`DROP DATABASE` records drop evidence in the startup state (`drop_evidence_recorded` flag) before performing any physical cleanup. The phase transitions to `dropped`.

## Smoke Test

The following sequence exercises the essential lifecycle path on a disposable database. Replace `<path>` with a temporary location and adjust syntax to match your connected parser dialect (see [Language Reference: Database](../Language_Reference/syntax_reference/database.md)):

```sql
-- 1. Create a disposable database
CREATE DATABASE '<path>/smoke_test.sbd';

-- 2. Create schema and a table
CREATE SCHEMA test_schema;
CREATE TABLE test_schema.items (id INTEGER, label VARCHAR(100));

-- 3. Insert rows and commit
INSERT INTO test_schema.items VALUES (1, 'alpha');
INSERT INTO test_schema.items VALUES (2, 'beta');
COMMIT;

-- 4. Detach the session
DETACH DATABASE;

-- 5. Reopen
OPEN DATABASE '<path>/smoke_test.sbd';

-- 6. Verify committed rows survived the close/reopen cycle
SELECT id, label FROM test_schema.items ORDER BY id;
-- Expected: rows (1, 'alpha') and (2, 'beta')

-- 7. Test a controlled refusal: attempt maintenance without authority
-- (Expected: denied with ordinary_attach_blocked or enter_requires_authority)
ENTER DATABASE MAINTENANCE;

-- 8. Clean up
DROP DATABASE '<path>/smoke_test.sbd';
```

If step 6 does not return both rows, examine the startup state's `kFlagCleanShutdown` result from the prior detach. If step 7 does not produce a refusal diagnostic, the authority model for the current session may have more privilege than expected — review grant configuration.

## Related Pages

- [Filespaces And Storage](filespaces_and_storage.md)
- [Backup, Restore, And Data Movement](backup_restore_and_data_movement.md)
- [Language Reference: Database](../Language_Reference/syntax_reference/database.md)
- [Operating Modes Runbook](operating_modes_runbook.md)

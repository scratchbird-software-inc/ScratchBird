# Troubleshooting

## Purpose

This chapter is organized by symptom. For each problem an operator might observe, it describes the likely area, what evidence to collect, what diagnostic codes to look for, and where to go next. The goal is to get you to the right evidence quickly without guessing.

If you are seeing a controlled refusal rather than a crash, that is expected behavior — ScratchBird emits structured diagnostics rather than silent failures. A refusal with a diagnostic code is more useful than no response; the code tells you exactly which boundary was crossed.

---

## Symptom Index

1. [Service fails to start](#1-service-fails-to-start)
2. [Database open refused or format mismatch](#2-database-open-refused-or-format-mismatch)
3. [Cannot connect — IPC endpoint unavailable](#3-cannot-connect--ipc-endpoint-unavailable)
4. [Parser refused a statement](#4-parser-refused-a-statement)
5. [Authentication or authorization failure](#5-authentication-or-authorization-failure)
6. [Object not found or not visible](#6-object-not-found-or-not-visible)
7. [Transaction state invalid or recovery required](#7-transaction-state-invalid-or-recovery-required)
8. [Storage unavailable or quarantined](#8-storage-unavailable-or-quarantined)
9. [Listener is live but not ready](#9-listener-is-live-but-not-ready)
10. [Transaction pressure or slow cleanup](#10-transaction-pressure-or-slow-cleanup)
11. [Support bundle is insufficient or too broad](#11-support-bundle-is-insufficient-or-too-broad)

---

## 1. Service Fails to Start

**Symptoms**: The listener or manager process exits immediately. No IPC socket appears. Log lines appear but the process does not stabilize.

**Likely area**: Configuration, resource limits, IPC socket path, or format mismatch on a database that is opened at startup.

**Evidence to collect**:
- Startup log output up to the first error line.
- The configuration file that was passed.
- Any format-related diagnostic codes (look for `FORMAT.VERSION_DOWNGRADE_REFUSED`, `CONFIG.DOWNGRADE_REFUSED`, `IPC.LIFECYCLE.DOWNGRADE_REFUSED`).

**Safe checks to run**:
- Verify the configuration file is syntactically valid by running the validation step described in [Configuration Reference](configuration_reference.md).
- Check that the socket directory and runtime directory exist and the process has write permission.
- Verify the build version against any databases that are opened at startup.

**Expected diagnostic codes**:
- `CONFIG.DOWNGRADE_REFUSED` — a configuration value is incompatible with this build.
- `IPC.LIFECYCLE.DOWNGRADE_REFUSED` — the IPC protocol version is newer than this build supports.
- `FORMAT.VERSION_DOWNGRADE_REFUSED` — a database was written by a newer build.

**Escalation**: Collect a startup log and the configuration; see [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md).

---

## 2. Database Open Refused or Format Mismatch

**Symptoms**: A database that previously opened now refuses to open. The error message mentions format version, downgrade refused, or startup state.

**Likely area**: The database was written by a newer build of ScratchBird than the one currently running, or the startup state block is corrupt.

**Why this happens**: ScratchBird refuses to open a database whose on-disk format version is newer than the build can interpret (`FORMAT.VERSION_DOWNGRADE_REFUSED`). This is a safety refusal — opening an unknown format silently could corrupt data. The same logic applies at the startup state level (`SB-STARTUP-STATE-FORMAT-DOWNGRADE-REFUSED`).

**Evidence to collect**:
- The exact diagnostic code from the open failure.
- The build version currently running.
- The build version that last wrote to the database (if known).

**Expected diagnostic codes**:
- `FORMAT.VERSION_DOWNGRADE_REFUSED` — database format version is newer than supported.
- `ENGINE.DBLC_FORMAT_DOWNGRADE_REFUSED` — same, observed at the engine lifecycle layer.
- `SB-STARTUP-STATE-FORMAT-DOWNGRADE-REFUSED` — startup state format is newer than supported.
- `SB-STARTUP-STATE-MAGIC-INVALID` — startup state block has an invalid magic marker (possible corruption or wrong file).
- `SB-STARTUP-STATE-BODY-TOO-SMALL` — startup state block is truncated.

**Safe checks**:
- Confirm the build version matches what wrote the database.
- If you need to upgrade the running build rather than downgrade the database, see [Upgrade And Compatibility Policy](upgrade_and_compatibility_policy.md).
- Do not attempt to open the database with an incompatible build. Do not attempt manual repair of the startup state block.

**Escalation**: Collect the diagnostic output and the database's format version evidence; consult [Upgrade And Compatibility Policy](upgrade_and_compatibility_policy.md).

---

## 3. Cannot Connect — IPC Endpoint Unavailable

**Symptoms**: A client cannot reach the listener. Connection attempts time out or are refused. The IPC socket does not exist or does not respond.

**Likely area**: Listener is not running, parser pool is not initialized, or the socket path does not match what the client is configured to use.

**Evidence to collect**:
- Check whether the listener process is running.
- Check whether the IPC socket file exists at the configured path.
- Run `listener.status` through the management interface to get the listener state snapshot.
- Look for `SBSQL.SERVER.UNAVAILABLE` in recent diagnostic output.

**Expected diagnostic codes**:
- `SBSQL.SERVER.UNAVAILABLE` — the parser server cannot accept the request.
- `PARSER_SERVER_IPC.PROTOCOL_VERSION_DOWNGRADE_REFUSED` — client is using a newer IPC protocol version than the server.

**Safe checks**:
- Verify the socket path in the listener configuration.
- Check that the listener process has not exited; check process management logs.
- If the listener is alive but `SBSQL.SERVER.UNAVAILABLE` is appearing, check parser pool status via the management interface.

**Escalation**: Collect a support bundle while the listener is alive, then see [Monitoring, Health, And Readiness](monitoring_health_and_readiness.md).

---

## 4. Parser Refused a Statement

**Symptoms**: A SQL statement fails with a diagnostic code, not a transaction error. The message mentions "not supported", "not executed", "emulation boundary", or similar.

**Likely area**: The statement is either unsupported in the current parser dialect, uses a syntax that is emulated-only (file-backed operations), or exceeds a resource limit.

**Understanding parser refusals**: ScratchBird's parser compatibility surfaces recognize many statement forms from Firebird, PostgreSQL, MySQL, and others. Some of those forms are admitted and executed. Others are recognized and refused with a structured diagnostic. A refusal is not a bug — it is a documented boundary.

**Common cases and their codes**:

| What the operator tried | Diagnostic code | Why |
|---|---|---|
| `BACKUP DATABASE` (no `TO`) | `SBSQL.EMULATION.NON_FILE_OPERATION` | File-backed backup syntax has no filesystem effect in SBsql |
| `NBACKUP` | `SBSQL.EMULATION.NON_FILE_OPERATION` | Physical page-copy tool not admitted |
| `GBAK`, `GFIX`, `GSTAT`, `GSEC` | `SBSQL.EMULATION.REFERENCE_TOOL_NOT_EXECUTED` | Reference native tools are not invoked from parser authority |
| `COPY PROGRAM <cmd>` | Parser refusal | Cannot spawn host programs from parser authority |
| `COPY TO <file>` | Parser refusal | Compatibility filesystem writes not permitted |
| Statement exceeds size limit | `SBSQL.RESOURCE.STATEMENT_TOO_LARGE` | Statement too large |
| Too many parameters | `SBSQL.RESOURCE.PARAMETER_COUNT_EXCEEDED` | Reduce parameter count |
| AST too deep | `SBSQL.RESOURCE.AST_DEPTH_EXCEEDED` | Simplify query nesting |

**For file-backed Firebird-style operations**: Use ScratchBird's logical backup/restore surfaces (`BACKUP DATABASE TO <uri>` / `RESTORE DATABASE FROM <uri>`) instead of native tool syntax. See [Backup, Restore, And Data Movement](backup_restore_and_data_movement.md).

**For PostgreSQL COPY**: Use `COPY TO STDOUT` (admitted as a logical export stream) rather than `COPY TO <file>` or `COPY PROGRAM`.

**Escalation**: If a statement you believe should be admitted is being refused, collect the full message vector (not just the top-level error) and report it with the exact statement text.

---

## 5. Authentication or Authorization Failure

**Symptoms**: A connection fails at authentication. A statement fails with a permission or role denial. A TLS negotiation is refused.

**Likely area**: Principal identity, role assignment, policy binding, or TLS downgrade attempt.

**Evidence to collect**:
- The diagnostic code from the failure.
- Whether TLS is involved.
- Whether this is a new user or a regression for an existing user.

**Expected diagnostic codes**:
- `SBSQL.AUTH.REQUIRED` — authentication is required but was not provided.
- `SECURITY.AUTHENTICATION.TLS_DOWNGRADE_REFUSED` — client attempted to downgrade TLS.
- `SECURITY_DENIED` (engine ABI status) — security policy denied the operation.

**Safe checks**:
- Confirm the principal exists and has the expected role bindings.
- Confirm TLS configuration matches on client and server.
- Check whether this is a new policy deployment that may have inadvertently tightened access.

**Escalation**: See [Identity, Security, And Policy](identity_security_and_policy.md).

---

## 6. Object Not Found or Not Visible

**Symptoms**: A query fails with "not found" or "not visible". An object that the operator can see in one context does not appear in another.

**Likely area**: Schema visibility, catalog session scope, or MGA (multi-generational architecture) visibility rules.

**Evidence to collect**:
- The exact object name and schema reference.
- The role and principal under which the query is running.
- Whether the object is visible under a different role.

**Expected diagnostic codes**:
- `SBSQL.NAME_RESOLUTION.NOT_FOUND_OR_NOT_VISIBLE` — name resolution found no accessible object.

**Safe checks**:
- Verify the schema path is correct.
- Verify the querying principal has the right to see the object under its current role.
- If this is a recent DDL change, confirm the DDL transaction committed before the query.

**Escalation**: See [Language Reference: Core Paradigms — UUID Catalog Identity](../Language_Reference/core_paradigms/uuid_catalog_identity.md) and [Identity, Security, And Policy](identity_security_and_policy.md).

---

## 7. Transaction State Invalid or Recovery Required

**Symptoms**: Operations fail with "transaction state invalid", "recovery required", or similar. A database open fails with a recovery classification error.

**Likely area**: A database was not closed cleanly (crash, power loss), or a transaction is in an inconsistent state that requires the recovery path before normal operations can continue.

**Evidence to collect**:
- The diagnostic code from the failure.
- Whether the previous shutdown was clean or unclean.
- The `storage.startup_state.recovery_classification_invalid` or `storage.startup_state.durable_lifecycle_phase_invalid` codes from startup state reading.

**Expected diagnostic codes**:
- `recovery_required` (page/filespace agent state) — the storage layer requires recovery.
- `storage.startup_state.recovery_classification_invalid` — startup state has an invalid recovery classification.
- `storage.startup_state.durable_lifecycle_phase_invalid` — startup state has an invalid lifecycle phase.

**Safe checks**:
- Do not force-open a database that requires recovery.
- Check whether the process that last held the database shut down cleanly.
- Follow the recovery procedures in [Database Lifecycle](database_lifecycle.md).

**Escalation**: Collect the startup diagnostic output and the database's repair event ledger if available. Do not write new data until recovery is confirmed complete.

---

## 8. Storage Unavailable or Quarantined

**Symptoms**: A filespace operation fails. Writes are refused. A storage diagnostic mentions `shrink_blocked`, `refuse`, or a boundary violation.

**Likely area**: Filespace is in a `refused` or `recovery_required` state, or a page-filespace handoff encountered a boundary violation.

**Evidence to collect**:
- The handoff state from storage diagnostics.
- Whether the affected filespace is a primary or shadow.
- The specific boundary violation code if present.

**Expected states and codes**:
- `refused` — filespace request explicitly denied.
- `recovery_required` — filespace cannot accept new work until recovery completes.
- `invalid_filespace_identity` — boundary violation: presented identity is invalid.
- `invalid_page_family` — boundary violation: page family does not match agent's domain.

**Safe checks**:
- If a shadow filespace is the problem, check whether the primary is healthy.
- Do not attempt to manually repair filespace structures.
- Check storage device health (disk full, I/O errors) which can surface as filespace refusals.

**Escalation**: See [Filespaces And Storage](filespaces_and_storage.md).

---

## 9. Listener Is Live but Not Ready

**Symptoms**: The listener process is running and the management interface responds, but client connections are being refused or queued. The `accepting_new_connections` field is false.

**Likely area**: Drain is active, parser pool is not yet initialized, or parser pool has no available workers.

**Evidence to collect**:
- Run `listener.status` to get the current state snapshot.
- Check the `draining` field (true if drain is active).
- Check `queue_depth` and `open_connections`.
- Check parser pool status.

**Safe checks**:
- If `draining` is true, the listener was intentionally drained. Issue `listener.undrain` if the drain was not intended.
- If the pool is initializing, wait for pool startup to complete.
- If the pool has no workers due to saturation, investigate whether the parser worker count is sufficient for the load.

**Escalation**: See [Monitoring, Health, And Readiness](monitoring_health_and_readiness.md) and [Service Lifecycle](service_lifecycle.md).

---

## 10. Transaction Pressure or Slow Cleanup

**Symptoms**: Storage grows unexpectedly. Query performance degrades over time. Diagnostics mention cleanup horizon stall.

**Likely area**: A long-running transaction is holding the oldest transaction ID, preventing the MVCC garbage collector from advancing.

**Evidence to collect**:
- Current cleanup horizon evidence (key: `dpc030_authoritative_cleanup_horizon_v1`).
- Whether any long-running transactions are visible in session management.

**Safe checks**:
- Identify and close or roll back any abandoned long-running transactions.
- Verify that application code commits or rolls back transactions promptly.
- Check that `storage_version_cleanup_agent` and `index_garbage_cleanup_agent` are running.

**What not to do**: Do not kill the listener to clear transaction pressure — this can leave more uncommitted state to recover. Close the specific transactions causing the stall.

**Escalation**: See [Database Lifecycle](database_lifecycle.md) for cleanup agent configuration.

---

## 11. Support Bundle Is Insufficient or Too Broad

**Symptoms**: A support bundle you collected does not have enough information to diagnose the problem. Or: a bundle you are about to share contains information that should not leave your environment.

**If the bundle is insufficient**:
- Check whether the bundle was collected during the failure window or after the service had already recovered.
- Bundles contain a ring buffer of recent management decisions and runtime events (up to 64 events by default). If the failure was older than the ring buffer, the relevant events may have rolled off.
- Collect a new bundle while the failure condition is still active.
- If the listener is no longer running, collect the lifecycle journal and audit files directly.

**If the bundle is too broad**:
- Review `config-redacted.txt` and the metrics JSON for sensitive values.
- Check the bundle manifest for the `redaction_profile` value.
- Consult your security team before sharing a bundle collected during a security-related incident.

**Escalation**: See [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md) for redaction details.

---

## Related Pages

- [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md)
- [Monitoring, Health, And Readiness](monitoring_health_and_readiness.md)
- [Configuration Reference](configuration_reference.md)
- [Service Lifecycle](service_lifecycle.md)
- [Database Lifecycle](database_lifecycle.md)
- [Upgrade And Compatibility Policy](upgrade_and_compatibility_policy.md)

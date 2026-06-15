# Backup, Restore, And Data Movement

## Purpose

This chapter explains how ScratchBird moves data in and out of a database: what operations are admitted, what is refused at the parser boundary, and why the distinction matters for operators who come from Firebird or PostgreSQL backgrounds.

## The Core Distinction: Logical vs Physical

ScratchBird performs **logical** data movement — everything flows through admitted SQL or UDR surfaces with engine authority, transaction semantics, and policy enforcement applied. It does **not** permit physical page-copy, server-local file manipulation, or the invocation of donor engine utilities. This is a deliberate security and isolation boundary, not a gap.

Administrators familiar with Firebird's `GBAK`/`GFIX`/`NBACKUP` or PostgreSQL's `COPY PROGRAM` / `COPY TO file` should expect those surfaces to be refused. ScratchBird parser compatibility layers receive those statements, recognize them, and emit a controlled diagnostic refusal rather than silently ignoring or mis-routing them.

### Allowed-vs-Denied Summary

| Operation | Admitted? | Notes |
|---|---|---|
| `BACKUP DATABASE TO <uri>` | Yes | Logical backup stream |
| `RESTORE DATABASE FROM <uri>` | Yes | Logical restore stream |
| `BACKUP DATABASE` (no `TO`) | No | Emulated: `sbsql.emulated.backup_restore_non_file`, code `SBSQL.EMULATION.NON_FILE_OPERATION` |
| `RESTORE DATABASE` (no `FROM`) | No | Same emulation boundary |
| `NBACKUP` | No | Physical page-copy tool; refused via `sbsql.emulated.backup_restore_non_file` |
| `GBAK` / `GFIX` / `GSTAT` / `GSEC` / `FBSVCMGR` / `FBTRACEMGR` | No | Reference native tools; refused via `sbsql.emulated.reference_tool_non_file`, code `SBSQL.EMULATION.REFERENCE_TOOL_NOT_EXECUTED` |
| `COPY PROGRAM <cmd>` (PostgreSQL compat) | No | Cannot spawn host programs from parser authority |
| `COPY TO <file>` (PostgreSQL compat) | No | Cannot perform compatibility filesystem writes |
| `COPY TO STDOUT` (PostgreSQL compat) | Yes | Remote logical export stream routed through trusted package policy |
| Stream ops: `stream_open`, `stream_read`, `stream_write`, `stream_close` | Yes | UDR bridge stream operations |
| CDC ops: `cdc_start`, `cdc_read`, `cdc_apply` | Yes | Change-data-capture operations |
| `proxy_live_migration` / `cutover` | Yes | Live migration with validated evidence requirement |
| ETL (`mysql.udr.etl.load_data_local_infile`) | Yes | ETL load through UDR surface |
| `ALTER DATABASE ... FILE` | No | Database file management; refused via `sbsql.emulated.database_file_management` |

Sources: `src/parsers/sbsql_worker/statement/statement_catalog.cpp:870-890`, `src/parsers/compatibility/postgresql/postgresql_dialect.cpp:28-54`, `src/parsers/compatibility/firebird/firebird_dialect.cpp:1380`.

---

## Logical Backup and Restore

### Syntax

```sql
BACKUP DATABASE TO '<uri>';
RESTORE DATABASE FROM '<uri>';
```

The `TO` and `FROM` clauses are required. A `BACKUP DATABASE` without `TO` is caught and refused with diagnostic code `SBSQL.EMULATION.NON_FILE_OPERATION` on channel `diagnostic.lifecycle.message_vector`. This protects operators from accidentally issuing a partial statement that would otherwise be ambiguous.

### What a Logical Backup Contains

A logical backup captures the committed, consistent state of database objects and data as a stream. It does not capture raw page images, and it is not equivalent to a filesystem-level snapshot of the data files. As a result:

- The restore can run on a build with a compatible schema format.
- The backup stream passes through policy enforcement; sensitive columns subject to protection policy are handled per the configured redaction and protection rules.
- Backup and restore are admitted through the engine, so they respect transaction cleanup horizons and do not capture uncommitted or partially cleaned row versions.

### Restore Drills

A backup that has never been successfully restored is an untested assumption. Before relying on a backup for recovery, perform a restore drill:

1. Restore to a staging database (not the live copy).
2. Run schema smoke queries to confirm catalog integrity.
3. Run a representative data sample query to confirm row-level data integrity.
4. Record the restore timestamp and outcome in your operations log.

See [Release Validation Checklist](release_validation_checklist.md) for the restore drill steps required before a build is trusted.

---

## Stream-Based Data Movement

ScratchBird's UDR bridge supports continuous and bulk data movement through a stream protocol. The supported operations are:

| Operation | Purpose |
|---|---|
| `stream_open` | Open a data stream between source and target |
| `stream_read` | Read a batch from the source stream |
| `stream_write` | Write a batch to the target stream |
| `stream_close` | Finalize and close the stream |
| `cdc_start` | Start a change-data-capture feed from a source |
| `cdc_read` | Read a CDC batch |
| `cdc_apply` | Apply a CDC batch to the target |
| `proxy_route` | Route a query through a live proxy |
| `compare_result` | Validate source and target agreement |
| `cutover` | Execute the final live migration cutover |

Source: `src/udr/sbu_sbsql_parser_support/sbu_sbsql_parser_support.cpp:216-266`.

### Supported Topology Types

The bridge capabilities declaration lists the following topology types:

- `outbound_federation`
- `inbound_cdc`
- `outbound_replication`
- `proxy_live_migration`
- `sb_to_sb`
- `logical_backup_restore`

Source: `src/udr/sbu_sbsql_parser_support/sbu_sbsql_parser_support.cpp:295-296`.

### Cutover Requirements

A `cutover` operation requires validated evidence before it is admitted. Specifically, the context packet must carry `cutover_evidence=validated`. If `compare_result` has not produced validated evidence and that evidence has not been placed in the context packet, the cutover is refused with `UDR.BRIDGE.CUTOVER_FAILED`. This requirement exists so that a live cutover cannot proceed without documented proof that source and target are in agreement.

**Cutover** means: the final act of switching active traffic from the source system to the migrated ScratchBird database. It is irreversible in the sense that once traffic is live on the target, the source system is no longer the authoritative copy. Do not cutover without a validated compare step.

Source: `src/udr/sbu_sbsql_parser_support/sbu_sbsql_parser_support.cpp:432-435`.

---

## CDC and Replication

**CDC (Change-Data Capture)** is the process of reading the change log from a source database and applying it incrementally to ScratchBird. This is how live migrations maintain low-latency sync between source and target during a cutover window.

**Replication** in the outbound direction (`outbound_replication`) streams committed changes from ScratchBird to a downstream consumer.

Both modes are stream operations routed through the UDR bridge and require the appropriate topology and operation capability to be present in the registered parser package.

---

## ETL Workflows

ETL (Extract, Transform, Load) workflows import data from external sources. The MySQL surface supports `load_data_local_infile` through the UDR ETL surface (`mysql.udr.etl.load_data_local_infile`). ETL operations are subject to the same policy enforcement and transaction semantics as any other admitted write.

---

## Denied Physical Operations: What Operators See

When a Firebird-syntax statement attempts to invoke `GBAK`, `GFIX`, `GSTAT`, `GSEC`, `FBSVCMGR`, or `FBTRACEMGR`, the SBsql parser catches it and emits:

```
diagnostic_code: SBSQL.EMULATION.REFERENCE_TOOL_NOT_EXECUTED
severity:        ERROR
text:            Reference native tools are not invoked by the SBSQL parser; use ScratchBird management routes.
channel:         diagnostic.lifecycle.message_vector
```

This is a controlled refusal, not a crash or timeout. The same pattern applies to `BACKUP DATABASE` without a `TO` clause and `NBACKUP`. The message explicitly tells operators to use ScratchBird management routes instead.

For the PostgreSQL compatibility surface, `COPY PROGRAM` and `COPY TO <file>` produce equivalent diagnostics stating that host-program spawning and compatibility filesystem writes are not permitted from parser authority.

---

## Related Pages

- [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md)
- [Release Validation Checklist](release_validation_checklist.md)
- [Language Reference: Backup, Restore, Replication, Migration](../Language_Reference/syntax_reference/backup_restore_replication_migration.md)
- [Language Reference: Refusal Vectors](../Language_Reference/syntax_reference/refusal_vectors.md)
- [Getting Started: Backup, Restore, And Data Movement Overview](../Getting_Started/administration/backup_restore_and_data_movement_overview.md)

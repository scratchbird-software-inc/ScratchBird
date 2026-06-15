# Database Lifecycle

This page is part of the SBsql Language Reference Manual. It documents database creation, open/attach/use/detach, lifecycle modes, inspection, verification, repair, shutdown, drop, metadata operations, and the boundaries between SQL text, SBLR, catalog identity, storage, and MGA transaction authority.

Generation task: `syntax_reference_database_lifecycle`

Related pages: [Schema Tree And Name Resolution](schema_tree_and_name_resolution.md), [Filespace Lifecycle](filespace.md), [Security And Privileges](security_and_privilege_statements.md), [Transaction Control](transaction_control.md), [Backup, Restore, Replication, And Migration](backup_restore_replication_migration.md), [Management And Operations](management_and_operations.md), [Refusal Vectors](refusal_vectors.md), and [Database Lifecycle EBNF](ebnf/database_lifecycle_statement.md).

## Purpose

A database is an engine-owned durability, catalog, security, schema-tree, and storage root. It owns:

- one database UUID;
- one database lifecycle state;
- one catalog root and schema tree;
- one materialized security and policy context;
- one transaction inventory and MGA visibility domain;
- one default storage/filespace policy set;
- operational evidence for open, recovery, maintenance, shutdown, and supportability.

Database lifecycle statements request engine lifecycle work. The parser recognizes SBsql text and lowers it to SBLR; it does not create files, mark transactions final, bypass security, or declare recovery complete. The engine lifecycle API owns physical create/open/reopen, recovery-required state, shutdown, safe drop, and fail-closed diagnostics.

## Identity Model

| Concept | Meaning | Authority |
| --- | --- | --- |
| Database UUID | Durable identity of the database. | Engine-created and catalog-recorded. |
| Database name | Resolver name visible through authorized metadata. | Name registry and security policy. |
| Database alias | Session-local or configuration-provided handle used by `USE` and attach/open operations. | Session context and lifecycle manager. |
| Database storage reference | Policy-controlled reference to storage placement. | Engine lifecycle and filespace policy. |
| Current database | Database selected for the current session. | Session context after successful attach/use/open. |
| Home schema | Schema branch assigned as the user's default root within the current database. | Security and schema policy. |
| Current schema | Resolver starting point for unqualified object names. | Session context, `SET` commands, and schema policy. |

User-visible names are resolver input. UUID identity and lifecycle evidence are the durable authority.

## Authority Boundaries

| Boundary | Rule |
| --- | --- |
| Parser | Parses context-sensitive SBsql and produces SBLR-bound requests. It does not perform database file work. |
| SBLR verifier | Admits only operation families and descriptor shapes that the public engine accepts. |
| Engine lifecycle | Creates, opens, attaches, detaches, verifies, repairs, shuts down, drops, and fences databases. |
| MGA | Owns transaction finality, visibility, snapshots, cleanup, and rollback for work inside an open database. |
| Security | Materialized policy decides who may create, attach, inspect, repair, shut down, or drop a database. |
| Storage | Filespace and storage code own allocation, sync, integrity checks, and recovery behavior. |

Catalog mutations inside an open database follow MGA visibility. Engine lifecycle operations such as `CREATE DATABASE`, `ATTACH DATABASE`, `OPEN DATABASE`, `SHUTDOWN DATABASE`, and `DROP DATABASE` are lifecycle operations; they return lifecycle evidence and may not require a user transaction context.

## Lifecycle States

| State | Meaning | Allowed Direction |
| --- | --- | --- |
| `creating` | Engine is initializing storage, header, catalog root, and bootstrap metadata. | Success moves to `created`/`open`; failure must leave no usable database or a recovery-required diagnostic. |
| `created` | Database exists but is not necessarily selected by the current session. | Open, attach, inspect, verify, drop where policy admits. |
| `open` | Database is available for sessions and ordinary transactions. | Enter restricted open, maintenance, shutdown, detach, verify. |
| `attached` | Current session has a database attachment/alias. | Use, detach, transact, inspect according to rights. |
| `restricted_open` | Database is open for authorized administration only. | Exit restricted open, maintenance, verify, repair, shutdown. |
| `maintenance` | Ordinary user work is fenced for maintenance, verify, repair, or controlled operational changes. | Exit maintenance, verify, repair, shutdown. |
| `recovery_required` | Engine detected a state that requires recovery or operator decision before ordinary work. | Verify, repair, inspect, shutdown; ordinary writes fail closed. |
| `shutdown_pending` | Database is draining or waiting for acknowledgement. | Acknowledge, force where explicitly admitted, or complete shutdown. |
| `closed` | Database is not open for ordinary work. | Open, attach, inspect, drop where policy admits. |
| `dropping` | Engine is validating safe-drop policy and retiring storage/catalog bindings. | Complete drop or refuse and preserve prior durable state. |
| `dropped` | Database identity is retired according to safe-drop policy. | Reuse of names requires new UUID identity. |

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE DATABASE` | Requests engine-owned initialization of a new database UUID, storage root, bootstrap catalog, security seed, lifecycle evidence, and default schema/filespace policy. |
| Open | `OPEN DATABASE` | Opens a known database for lifecycle-managed use. `OPEN DATABASE ... RESTRICTED` enters restricted-open admission. |
| Attach | `ATTACH DATABASE ... AS ...` | Binds a database storage reference or configured database to a session-visible alias after security and lifecycle checks. |
| Use | `USE DATABASE ...`, `USE ...` | Selects a session database alias as the current database. It does not create or authorize the database by itself. |
| Detach | `DETACH DATABASE ...` | Removes a session attachment after active work, cursor, transaction, and cleanup checks pass. |
| Alter | `ALTER DATABASE` | Changes admitted lifecycle or metadata settings, such as maintenance mode, restricted-open mode, shutdown mode, default schema, default filespace, or policy bindings. |
| Maintenance | `MAINTENANCE DATABASE`, `ENTER DATABASE MAINTENANCE`, `EXIT DATABASE MAINTENANCE` | Enters or exits maintenance admission. Ordinary writes are fenced according to policy while maintenance is active. |
| Restricted open | `ENTER DATABASE RESTRICTED OPEN`, `EXIT DATABASE RESTRICTED OPEN` | Limits admission to authorized administration while preserving controlled open behavior. |
| Inspect | `INSPECT DATABASE`, `DIAGNOSE DATABASE`, `DESCRIBE DATABASE`, `SHOW DATABASE` | Returns authorized lifecycle, storage, catalog, security, and recovery summaries. |
| Verify | `VERIFY DATABASE` | Runs read-only or policy-limited consistency checks and returns evidence. It does not repair by itself. |
| Repair | `REPAIR DATABASE` | Applies an explicit repair plan through engine lifecycle authority. Missing, unsafe, or ambiguous repair plans are refused. |
| Shutdown | `SHUTDOWN DATABASE`, `ALTER DATABASE ... SHUTDOWN` | Drains and closes a database according to policy. |
| Force shutdown | `FORCE SHUTDOWN DATABASE`, `SHUTDOWN DATABASE ... FORCE` | Requires an explicit force descriptor and emits diagnostics. It must preserve recovery correctness. |
| Acknowledge shutdown | `ACKNOWLEDGE SHUTDOWN DATABASE`, `SHUTDOWN ACKNOWLEDGE DATABASE` | Records operator acknowledgement for drain/shutdown lifecycle. |
| Rename | `RENAME DATABASE ... TO ...` | Changes resolver name only. UUID, filespace, recovery, and transaction identity remain unchanged. |
| Comment | `COMMENT ON DATABASE ... IS ...` | Stores authorized descriptive metadata. |
| Recreate | `RECREATE DATABASE` | Requests create-or-replace semantics and must refuse unless retention, drop, recovery, and dependency policy explicitly admit replacement. |
| Drop | `DROP DATABASE ...` | Retires/removes a database only through safe-drop authority after dependency, attachment, transaction, recovery, and storage checks pass. |

## Syntax

```ebnf
database_lifecycle_statement ::=
      create_database_statement
    | open_database_statement
    | attach_database_statement
    | use_database_alias
    | detach_database_statement
    | alter_database_statement
    | maintenance_database_statement
    | inspect_database_statement
    | verify_database_statement
    | repair_database_statement
    | shutdown_database_statement
    | drop_database_statement
    | rename_database_statement
    | comment_database_statement
    | recreate_database_statement
    | show_database_statement
    | describe_database_statement ;
```

```ebnf
create_database_statement ::=
    CREATE DATABASE database_name database_create_options? ;

database_create_options ::=
    WITH database_create_option (","? database_create_option)* ;

database_create_option ::=
      OWNER principal_name
    | DEFAULT SCHEMA schema_name
    | DEFAULT CHARACTER SET charset_name
    | DEFAULT COLLATION collation_name
    | DEFAULT TIME ZONE time_zone_name
    | FILESPACE filespace_name
    | PAGE SIZE integer_literal
    | STORAGE PROFILE storage_profile_name
    | SECURITY POLICY policy_name
    | PROTECTED MATERIAL POLICY policy_name ;
```

```ebnf
open_database_statement ::=
    OPEN DATABASE database_ref (RESTRICTED OPEN?)? ;

attach_database_statement ::=
    ATTACH DATABASE storage_ref AS database_alias attach_options? ;

use_database_alias ::=
      USE DATABASE database_alias
    | USE database_alias ;

detach_database_statement ::=
    DETACH DATABASE database_alias detach_options? ;
```

```ebnf
alter_database_statement ::=
    ALTER DATABASE database_ref alter_database_action+ ;

alter_database_action ::=
      SET DEFAULT SCHEMA schema_name
    | SET DEFAULT FILESPACE filespace_name
    | SET DEFAULT CHARACTER SET charset_name
    | SET DEFAULT COLLATION collation_name
    | SET MAINTENANCE maintenance_options?
    | CLEAR MAINTENANCE
    | ENTER MAINTENANCE maintenance_options?
    | EXIT MAINTENANCE
    | ENTER RESTRICTED OPEN restricted_open_options?
    | EXIT RESTRICTED OPEN
    | VERIFY verify_options?
    | REPAIR repair_options
    | SHUTDOWN shutdown_options? ;
```

```ebnf
maintenance_database_statement ::=
      MAINTENANCE DATABASE database_ref maintenance_options?
    | ENTER DATABASE MAINTENANCE database_ref maintenance_options?
    | EXIT DATABASE MAINTENANCE database_ref ;

inspect_database_statement ::=
      INSPECT DATABASE database_ref inspect_options?
    | DIAGNOSE DATABASE database_ref inspect_options? ;

verify_database_statement ::=
    VERIFY DATABASE database_ref verify_options? ;

repair_database_statement ::=
    REPAIR DATABASE database_ref repair_options ;

shutdown_database_statement ::=
      SHUTDOWN DATABASE database_ref shutdown_options?
    | FORCE SHUTDOWN DATABASE database_ref force_shutdown_options
    | ACKNOWLEDGE SHUTDOWN DATABASE database_ref
    | SHUTDOWN ACKNOWLEDGE DATABASE database_ref ;
```

```ebnf
drop_database_statement ::=
    DROP DATABASE database_ref drop_database_options? ;

drop_database_options ::=
      RESTRICT
    | CASCADE
    | PRESERVE STORAGE
    | DESTROY STORAGE
    | WITH SAFE DROP POLICY policy_name ;

show_database_statement ::=
      SHOW DATABASE database_ref?
    | SHOW DATABASES ;

describe_database_statement ::=
    DESCRIBE DATABASE database_ref ;
```

SBsql is context-sensitive. Words such as `database`, `maintenance`, `restricted`, and `repair` are command words in these statements and do not need to be globally reserved in unrelated expression positions.

## Create Database

`CREATE DATABASE` asks the engine to create a database root. A successful create produces lifecycle evidence and a new database UUID.

| Option Area | Meaning | Notes |
| --- | --- | --- |
| Owner | Initial owner or administrative principal. | Must resolve to an authorized principal or policy-owned bootstrap identity. |
| Default schema | Initial resolver root for ordinary unqualified names. | The schema branch must exist or be created through bootstrap policy. |
| Character set and collation | Default text descriptor behavior. | Columns and domains may override defaults. |
| Time zone | Default session/database temporal interpretation. | Session settings may override where policy admits. |
| Filespace | Default storage placement. | The filespace must exist or be created by bootstrap policy. |
| Page size/storage profile | Storage descriptor family for the database. | Invalid sizes, profiles, or incompatible storage settings are refused. |
| Security policy | Initial materialized security policy. | Raw secrets are never embedded in SQL text; secret references must be policy-owned. |

Example:

```sql
create database tenant_a
with
  owner admin,
  default schema app,
  default character set utf8,
  default collation unicode_ci,
  filespace primary_data,
  storage profile transactional_default;
```

Creation can fail after parsing but before a usable database exists. The result must be one of: no durable database, a complete durable database, or a recovery-required state that fails closed.

## Open, Attach, Use, And Detach

Opening and attaching are not synonyms:

| Operation | Scope | Meaning |
| --- | --- | --- |
| `OPEN DATABASE` | Engine/database lifecycle | Opens a database so it can accept admitted work. |
| `ATTACH DATABASE ... AS ...` | Session lifecycle | Creates a session-visible attachment/alias after lifecycle and security checks. |
| `USE DATABASE` | Session context | Selects an already admitted alias as the current database. |
| `DETACH DATABASE` | Session lifecycle | Releases the session attachment after active work is drained or refused. |

Example:

```sql
attach database 'policy://databases/tenant_a' as tenant_a;
use database tenant_a;
show database tenant_a;
detach database tenant_a;
```

The storage reference is data passed to the engine lifecycle layer. SQL text does not grant arbitrary server-local file access. Deployments decide which storage-reference schemes are admitted.

## Maintenance And Restricted Open

Maintenance and restricted-open are admission modes.

| Mode | Purpose | Ordinary Work |
| --- | --- | --- |
| Maintenance | Verify, repair, operator inspection, storage-policy changes, controlled support actions. | Fenced unless policy admits the user/agent and operation. |
| Restricted open | Keep the database open while limiting admission to authorized administration. | Fenced for ordinary sessions. |

Examples:

```sql
alter database tenant_a set maintenance with evidence;
verify database tenant_a with checksum;
repair database tenant_a with plan tenant_a_repair_plan;
alter database tenant_a clear maintenance;
```

```sql
open database tenant_a restricted;
exit database restricted open tenant_a;
```

## Verify And Repair

`VERIFY DATABASE` is inspection. `REPAIR DATABASE` is mutation.

| Statement | Authority | Required Behavior |
| --- | --- | --- |
| `VERIFY DATABASE` | Lifecycle inspect/verify authority. | Read, classify, and report; do not silently modify durable state. |
| `REPAIR DATABASE` | Lifecycle repair authority. | Require an explicit repair plan or policy-owned plan descriptor. |
| `ALTER DATABASE ... VERIFY` | Same as verify route. | Must return the same class of verification evidence. |
| `ALTER DATABASE ... REPAIR` | Same as repair route. | Must refuse ambiguous or unsafe repairs. |

Repair can update storage, catalog, or lifecycle metadata only through the engine lifecycle API. A repair statement cannot mark a transaction committed or rolled back; MGA remains the finality authority for transaction inventory.

## Shutdown And Drop

Shutdown and drop operate at the lifecycle boundary.

| Operation | Contract |
| --- | --- |
| Graceful shutdown | Drain admitted work, refuse new work, close safely, record lifecycle evidence. |
| Force shutdown | Requires explicit force syntax or descriptor. It may leave recovery-required evidence but must not silently corrupt state. |
| Shutdown acknowledge | Records authorized operator acknowledgement for a drain or shutdown state. |
| Drop restrict | Refuses if active attachments, dependent objects, recovery state, or storage references make removal unsafe. |
| Drop cascade | Requires explicit cascade policy. Cascade is not permission to skip recovery or storage safety checks. |
| Preserve storage | Retires database metadata while preserving storage according to policy. |
| Destroy storage | Requires explicit destructive authority and safe-drop evidence. |

Example:

```sql
shutdown database tenant_a;
acknowledge shutdown database tenant_a;
drop database tenant_a restrict;
```

## SHOW And DESCRIBE

`SHOW DATABASE`, `SHOW DATABASES`, and `DESCRIBE DATABASE` are authorized projections.

Expected fields include:

- database UUID;
- resolver name and aliases visible to the caller;
- lifecycle state;
- open/attach status for the current session;
- home schema and current schema where visible;
- default character set, collation, and time zone;
- default filespace and storage profile;
- transaction inventory summary;
- active maintenance or restricted-open fences;
- recovery-required state and diagnostic reference;
- security policy and authorization epoch;
- filespace health summary;
- backup/restore/replication capability summary where visible;
- support-bundle redaction status;
- version and compatibility metadata.

Protected material, hidden database names, secret references, and security-sensitive storage details must be redacted.

## Metadata Operations

| Operation | Contract |
| --- | --- |
| `RENAME DATABASE` | Updates name-registry metadata only. UUID and storage identity do not change. |
| `COMMENT ON DATABASE` | Stores descriptive text according to catalog comment policy. |
| `RECREATE DATABASE` | Combines drop/create semantics only where replacement policy is explicit and safe. |
| `ALTER DATABASE SET DEFAULT ...` | Updates database defaults for future binding; existing object descriptors keep their stored definitions unless separately altered. |

## Failure Modes

| Failure | Required Behavior |
| --- | --- |
| Unknown or hidden database | Return not-found or hidden-object diagnostic according to security policy. |
| Unauthorized lifecycle action | Refuse before mutating lifecycle state. |
| Storage reference denied | Refuse without attempting parser-side file access. |
| Incompatible page/storage profile | Refuse create/open and return diagnostic evidence. |
| Recovery required | Fence ordinary work and expose only authorized recovery/inspect/repair routes. |
| Active transaction blocks detach/drop/shutdown | Refuse or drain according to explicit policy; never silently discard transaction finality. |
| Missing repair plan | Refuse `REPAIR DATABASE`. |
| Force not explicit | Refuse force-shutdown/drop behavior unless the force/destructive descriptor is present. |
| Unsupported downgrade/upgrade | Refuse open or enter recovery-required mode according to compatibility policy. |

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | Every database lifecycle statement shape is recognized by SBsql. |
| Bind | Database names, aliases, storage refs, schema refs, policy refs, and filespace refs resolve exactly. |
| Authorize | Effective user or agent has the required lifecycle, catalog, security, or repair right. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Execute | Engine lifecycle API, not the parser, performs create/open/attach/detach/verify/repair/shutdown/drop. |
| Commit | Catalog mutations inside an open database become visible only through MGA finality. |
| Recover | Crash/restart cannot leave silent half-created, half-dropped, or incorrectly open database state. |
| Redact | `SHOW`/`DESCRIBE` output hides protected and unauthorized fields. |
| Refuse | Unsafe storage, recovery, force, drop, and repair paths fail closed with canonical diagnostics. |

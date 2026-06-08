# Backup, Restore, Replication, And Migration

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_backup_restore_replication_migration`

Related pages: [Transactions And Recovery](../core_paradigms/transactions_and_recovery.md), [Transaction Control](transaction_control.md), [Database Lifecycle](database.md), [Filespace Lifecycle](filespace.md), [Security And Privileges](security_and_privilege_statements.md), [Bridge Boundary Model](../core_paradigms/bridge_and_cluster_boundaries.md), [Management And Operations](management_and_operations.md), and [Refusal Vectors](refusal_vectors.md).

## Purpose

Backup, restore, archive, replication, changefeed, and migration statements move database state through policy-bound engine routes. These statements can describe what to export, import, publish, subscribe to, validate, compare, or cut over. They do not make stream bytes, client state, file names, timestamps, retry behavior, parser state, or external ordering tokens into database authority.

ScratchBird MGA remains the authority for local transaction identity, snapshots, visibility, commit, rollback, cleanup, and recovery. A backup stream can report what it contains, and a replication stream can report an ordering token, but the engine still decides whether the local database has admitted and finalized the corresponding work.

Use this statement family for:

- logical backup and restore streams;
- native ScratchBird backup sets where policy admits them;
- archive manifests and retention-managed backup catalogs;
- changefeed and replication source/target setup;
- migration planning, validation, replay, comparison, and cutover;
- inspection of backup, restore, replication, and migration job state.

Do not use this statement family for low-level repair, page verification, unsafe server-local file access, or direct operating-system file manipulation. Those are separate administrative surfaces and remain policy controlled.

## Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Logical backup | `BACKUP DATABASE ... TO STREAM ...` | Exports catalog metadata and data changes as a typed logical stream. |
| Native backup set | `BACKUP DATABASE ... TO BACKUP SET ...` | Creates an engine-owned backup set when the product profile and policy admit it. |
| Logical restore | `RESTORE DATABASE ... FROM STREAM ...` | Imports a typed logical stream into a target database or schema branch. |
| Restore validation | `RESTORE ... VALIDATE ONLY` | Reads and validates stream shape, descriptors, manifests, and permissions without applying data. |
| Archive | `ARCHIVE ...` | Registers, expires, pins, verifies, or reports archive/backup manifest metadata. |
| Replication | `REPLICATION ...` | Creates, alters, starts, stops, drains, validates, or drops a replication route. |
| Changefeed | `REPLICATION CHANGEFEED ...` | Publishes ordered row/object changes to a stream target under idempotency and policy rules. |
| Migration | `MIGRATION ...` | Plans, imports, replays, compares, validates, or cuts over a migration route. |
| Inspection | `SHOW ...`, `DESCRIBE ...` within this family | Returns authorized job, manifest, stream, route, and refusal details. |

`SHOW` and `DESCRIBE` are inspection surfaces. They must not disclose hidden databases, schemas, credentials, stream locations, object names, object counts, or refusal details beyond the caller's authorized view.

## Syntax

```ebnf
archive_replication_migration_statement ::=
      archive_statement
    | backup_statement
    | restore_statement
    | replication_statement
    | migration_statement ;
```

```ebnf
backup_statement ::=
    BACKUP backup_target backup_destination backup_option_list? ;

backup_target ::=
      DATABASE database_ref
    | SCHEMA schema_ref
    | TABLE table_ref
    | QUERY "(" query_statement ")" ;

backup_destination ::=
      TO STREAM parameter_ref
    | TO BACKUP SET qualified_name
    | TO LOCATION location_ref ;

backup_option_list ::=
    WITH backup_option ("," backup_option)* ;

backup_option ::=
      FORMAT LOGICAL
    | FORMAT NATIVE
    | SNAPSHOT snapshot_option
    | INCLUDE DATA
    | INCLUDE METADATA
    | INCLUDE SECURITY
    | INCLUDE STATISTICS
    | EXCLUDE SECURITY
    | COMPRESS compression_profile
    | ENCRYPT WITH key_ref
    | MANIFEST qualified_name
    | LABEL string_literal
    | COMMENT string_literal ;
```

```ebnf
restore_statement ::=
    RESTORE restore_target restore_source restore_option_list? ;

restore_target ::=
      DATABASE database_ref
    | SCHEMA schema_ref
    | TABLE table_ref
    | WORKAREA qualified_name ;

restore_source ::=
      FROM STREAM parameter_ref
    | FROM BACKUP SET qualified_name
    | FROM LOCATION location_ref ;

restore_option_list ::=
    WITH restore_option ("," restore_option)* ;

restore_option ::=
      FORMAT LOGICAL
    | FORMAT NATIVE
    | VALIDATE ONLY
    | REPLACE
    | NO REPLACE
    | MAP NAMES mapping_ref
    | MAP SECURITY mapping_ref
    | TARGET SCHEMA schema_ref
    | TARGET FILESPACE filespace_ref
    | RECOVER UNTIL recovery_boundary
    | QUARANTINE INVALID ROWS
    | REQUIRE MANIFEST qualified_name ;
```

```ebnf
archive_statement ::=
    ARCHIVE archive_action archive_payload? archive_option_list? ;

archive_action ::=
      SHOW
    | DESCRIBE
    | REGISTER
    | VERIFY
    | PIN
    | UNPIN
    | EXPIRE
    | DROP ;

archive_payload ::=
      BACKUP SET qualified_name
    | MANIFEST qualified_name
    | DATABASE database_ref ;

archive_option_list ::=
    WITH archive_option ("," archive_option)* ;
```

```ebnf
replication_statement ::=
    REPLICATION replication_action replication_payload? replication_option_list? ;

replication_action ::=
      CREATE
    | ALTER
    | START
    | STOP
    | DRAIN
    | RESET
    | VALIDATE
    | SHOW
    | DESCRIBE
    | DROP
    | CHANGEFEED ;

replication_payload ::=
      ROUTE qualified_name
    | SOURCE replication_endpoint
    | TARGET replication_endpoint
    | CHANGEFEED qualified_name ;

replication_option_list ::=
    WITH replication_option ("," replication_option)* ;

replication_option ::=
      SOURCE replication_endpoint
    | TARGET replication_endpoint
    | INCLUDE TABLE table_ref
    | INCLUDE SCHEMA schema_ref
    | EXCLUDE TABLE table_ref
    | START AT replication_boundary
    | STOP AT replication_boundary
    | MODE SNAPSHOT
    | MODE CONTINUOUS
    | APPLY
    | NO APPLY
    | IDEMPOTENCY KEY expression
    | ORDER BY ordering_token_ref
    | QUARANTINE INVALID ROWS
    | RETRY retry_profile ;
```

```ebnf
migration_statement ::=
    MIGRATION migration_action migration_payload? migration_option_list? ;

migration_action ::=
      PLAN
    | CREATE
    | IMPORT
    | REPLAY
    | COMPARE
    | VALIDATE
    | CUTOVER
    | ABORT
    | SHOW
    | DESCRIBE
    | DROP ;

migration_payload ::=
      ROUTE qualified_name
    | PLAN qualified_name
    | SOURCE migration_endpoint
    | TARGET migration_endpoint ;

migration_option_list ::=
    WITH migration_option ("," migration_option)* ;

migration_option ::=
      SOURCE migration_endpoint
    | TARGET migration_endpoint
    | MAP NAMES mapping_ref
    | MAP TYPES mapping_ref
    | MAP SECURITY mapping_ref
    | MODE SNAPSHOT
    | MODE CONTINUOUS
    | VALIDATE ONLY
    | QUARANTINE INVALID ROWS
    | CUTOVER WHEN migration_cutover_condition ;
```

SBsql is context sensitive. The command words in this family are command words inside these statements and should not be treated as globally reserved identifiers outside this context.

## Stream And Location Model

Backup, restore, replication, and migration can move data through stream handles, backup sets, and policy-admitted locations.

| Carrier | Meaning | Safety rule |
| --- | --- | --- |
| `STREAM parameter` | A client, bridge, or engine stream handle supplied as a parameter descriptor. | Stream frames must bind to an admitted stream profile before read or write. |
| `BACKUP SET name` | Engine-owned backup set cataloged by ScratchBird. | The backup set is durable metadata, not an arbitrary file path. |
| `LOCATION ref` | Policy-controlled server-side storage location. | Server-local file access is denied unless an explicit filesystem/location policy admits it. |
| Bridge endpoint | A registered bridge-capable endpoint. | The bridge is a connection boundary and does not own transaction finality. |
| Archive manifest | Metadata describing backup contents, checksums, descriptors, security profile, and retention state. | Manifest evidence is validated but does not override catalog or MGA authority. |

Raw local file paths should not be used in portable SBsql. Use a named location, backup set, or stream handle so authorization, redaction, retention, and diagnostics can be enforced.

## Logical Backup

A logical backup exports catalog metadata and data as typed instructions and row streams. It is appropriate when the result must be portable across filespaces, platforms, or schema branches.

```sql
backup database app
to stream :client_stream
with format logical,
     include metadata,
     include data,
     manifest app.app_backup_manifest,
     label 'nightly logical backup';
```

Logical backup binds:

| Item | Binding rule |
| --- | --- |
| Database/schema/table scope | Must resolve to authorized catalog UUIDs under the caller's sandbox and privilege set. |
| Snapshot | Must be an engine-owned snapshot; stream order is not snapshot authority. |
| Object descriptors | Type, domain, collation, charset, protected material, index, constraint, trigger, and routine descriptors must be encoded where admitted. |
| Security metadata | Included only when requested and authorized. Secrets are represented by protected references, not raw secret values. |
| Large values | Streamed with frame limits, backpressure, cancellation, and integrity evidence. |
| Manifest | Records stream identity, scope, descriptors, ordering, checksums, policy, and redaction state. |

Example partial logical backup:

```sql
backup schema app.reporting
to stream :client_stream
with format logical,
     include metadata,
     include data;
```

Example query backup:

```sql
backup query (
  select order_id, customer_id, total_amount, closed_at
  from app.orders
  where order_state = 'closed'
)
to stream :client_stream
with format logical,
     label 'closed order export';
```

A query backup exports a rowset, not the full source table identity. It must declare or infer a result descriptor so restore/import can validate shape.

## Native Backup Sets

A native backup set is an engine-owned backup artifact. It can preserve ScratchBird-specific metadata more directly than a logical stream, but it is still created through an admitted engine route rather than direct operating-system file copying.

```sql
backup database app
to backup set app.nightly_native
with format native,
     manifest app.nightly_native_manifest,
     compress default,
     encrypt with key app.backup_key;
```

Native backup admission checks include:

- database recovery state;
- active transaction and snapshot profile;
- filespace readiness;
- storage and location policy;
- protected-material handling;
- manifest integrity;
- operator privilege;
- retention policy;
- cancellation and cleanup behavior.

Low-level repair, page verification, and unsafe physical page-copy import/export are outside this statement family. Administrative maintenance surfaces must use their own authority checks and diagnostics.

## Restore

`RESTORE` imports a logical stream or admitted backup set into a target database, schema, table, or workarea.

```sql
restore database app_restore
from stream :client_stream
with format logical,
     validate only,
     require manifest app.app_backup_manifest;
```

`VALIDATE ONLY` reads enough stream and manifest state to verify descriptors, checksums, compatibility, authorization, and target policy without applying data.

Applying a logical restore:

```sql
restore schema app.imported
from stream :client_stream
with format logical,
     target schema app.imported,
     map names app.import_name_map,
     quarantine invalid rows;
```

Restore must bind and verify:

| Item | Required behavior |
| --- | --- |
| Target | Existing or creatable target scope under the caller's authorization and sandbox root. |
| Source | Stream, backup set, or location admitted by policy. |
| Manifest | Required when policy demands it; validated before applying state. |
| Type descriptors | Source descriptors must be compatible with target descriptors or an admitted mapping. |
| Object identity | New object UUIDs are created unless the route explicitly admits identity preservation. |
| Security metadata | Restored only under security policy and protected-material rules. |
| Row data | Applied through engine-owned insert/update routes, not raw page mutation. |
| Transaction finality | Apply batches commit or roll back through MGA. |

`REPLACE` is destructive and should be policy-gated. `NO REPLACE` refuses if the target exists or if the restore would overwrite visible objects.

## Archive

Archive statements manage backup and manifest metadata. They do not create row data by themselves.

```sql
archive register backup set app.nightly_native
with manifest app.nightly_native_manifest;

archive verify backup set app.nightly_native;

archive pin backup set app.nightly_native
with comment 'release baseline';
```

Archive actions:

| Action | Contract |
| --- | --- |
| `REGISTER` | Adds an existing admitted backup set or manifest to the archive catalog. |
| `VERIFY` | Validates manifest, checksum, descriptor, policy, and retention evidence visible to the caller. |
| `PIN` | Prevents normal retention expiry until unpinned. |
| `UNPIN` | Removes an archive pin. |
| `EXPIRE` | Marks backup metadata eligible for retention cleanup. |
| `DROP` | Removes archive catalog metadata and, where admitted, the associated managed artifact. |
| `SHOW`/`DESCRIBE` | Returns authorized archive state. |

An archive manifest is evidence. It cannot authorize access to objects that the caller cannot otherwise inspect or restore.

## Replication And Changefeeds

Replication statements define and operate routes that move ordered changes between ScratchBird and an admitted endpoint. A route can publish local changes, subscribe to remote changes, or apply a stream into a target scope when the endpoint and policy admit it.

Create a route:

```sql
replication create route app.orders_out
with source database app,
     target endpoint app.reporting_endpoint,
     include table app.orders,
     mode continuous,
     idempotency key order_id;
```

Start, stop, and inspect:

```sql
replication start route app.orders_out
with start at current;

replication show route app.orders_out;

replication drain route app.orders_out;

replication stop route app.orders_out;
```

Changefeed publication:

```sql
replication changefeed app.orders_feed
with source database app,
     include table app.orders,
     mode continuous,
     order by transaction_id,
     idempotency key order_id;
```

Replication route metadata includes:

| Field | Meaning |
| --- | --- |
| Source scope | Database, schema, table, query, or stream endpoint. |
| Target scope | Stream endpoint, bridge endpoint, database, schema, or table where policy admits apply. |
| Mode | Snapshot, continuous, or combined snapshot-plus-continuous operation. |
| Ordering token | Evidence used to order stream records. It is not transaction finality authority. |
| Transaction grouping | Groups row/object changes into apply units. |
| Idempotency key | Allows replay-safe apply behavior and duplicate detection. |
| Quarantine policy | Captures invalid records without silently discarding them. |
| Cutover boundary | Defines when an apply route can be promoted or switched. |
| Diagnostic policy | Defines visible progress, error, refusal, and redaction behavior. |

Replication apply is ordinary engine work. Incoming changes are decoded, validated, mapped, and applied through engine-owned SBLR routes. Local commit succeeds only when MGA finality accepts the local transaction.

## Migration

Migration statements coordinate schema/data movement, validation, replay, comparison, and cutover.

Create a migration plan:

```sql
migration plan app.customer_migration
with source endpoint app.source_endpoint,
     target database app,
     map names app.customer_name_map,
     map types app.customer_type_map,
     validate only;
```

Run import and replay:

```sql
migration import plan app.customer_migration
with mode snapshot,
     quarantine invalid rows;

migration replay plan app.customer_migration
with mode continuous;
```

Validate and compare:

```sql
migration validate plan app.customer_migration;

migration compare plan app.customer_migration
with source endpoint app.source_endpoint,
     target database app;
```

Cut over:

```sql
migration cutover plan app.customer_migration
with cutover when validation complete;
```

Migration phases:

| Phase | Required behavior |
| --- | --- |
| Plan | Resolve source, target, mappings, privileges, and unsupported surfaces before data movement. |
| Validate | Check descriptor, security, stream, and target compatibility without applying data when `VALIDATE ONLY` is used. |
| Import | Apply a snapshot or logical stream through engine-owned routes. |
| Replay | Apply ordered changes after the snapshot boundary. |
| Compare | Compare authorized source and target result sets, counts, checksums, or sampled data. |
| Quarantine | Store invalid or ambiguous records for operator review without silent loss. |
| Cutover | Switch the admitted route only after validation, replay, and policy conditions pass. |
| Abort | Stop the migration route and preserve diagnostics for cleanup or retry. |

Migration can use a bridge endpoint, but the bridge is only a connection boundary. Each participating database keeps its own transaction authority.

## Transaction And Recovery Rules

Backup and restore work can be long-running, but they still follow explicit transaction and recovery rules.

| Operation | Transaction rule |
| --- | --- |
| Logical backup | Reads through an engine-owned snapshot. It does not commit data. |
| Native backup | Captures admitted engine state under backup/fence policy. |
| Restore validate | Does not apply user data. It may create temporary diagnostics or job state where policy admits it. |
| Restore apply | Applies work through engine-owned transactions and MGA finality. |
| Replication publish | Reads committed local changes through a route boundary and publishes evidence. |
| Replication apply | Applies incoming changes through local transactions. |
| Migration import/replay | Uses local transactions for applied batches and explicit diagnostics for ambiguous state. |
| Cutover | Requires a policy-admitted boundary and must fail closed when source/target state is uncertain. |

On crash or restart, a backup, restore, replication, or migration job must be recoverable, resumable, safely abortable, or explicitly fenced. Silent partial success is not an allowed outcome.

## Security And Secrets

These statements are administrative and require explicit privileges.

| Security concern | Rule |
| --- | --- |
| Privileges | `BACKUP`, `RESTORE`, `MANAGE REPLICATION`, `MANAGE MIGRATION`, archive management, and endpoint access are separate grants where policy exposes them. |
| Sandbox roots | A caller can operate only within its authorized database, schema, or workarea root. |
| Protected material | Raw secrets must not appear in SQL text, stream frames, manifests, diagnostics, or support output. Use protected references. |
| Security metadata restore | Requires policy admission and may map identities rather than preserving them. |
| Endpoint credentials | Stored and resolved through protected references. They are not parser packet authority. |
| Inspection | `SHOW` and `DESCRIBE` redact locations, credentials, object names, counts, or diagnostics where policy requires it. |

Example using a protected key reference:

```sql
backup database app
to backup set app.secure_backup
with format native,
     encrypt with key app.backup_key;
```

## Backpressure, Limits, And Cancellation

Streams must be policy-bound so large operations do not exhaust resources or leave ambiguous state.

| Control | Required behavior |
| --- | --- |
| Maximum frame size | Refuse frames that exceed the admitted stream profile. |
| In-flight bytes | Apply backpressure to clients, bridges, or workers. |
| Timeout | Return diagnostics when a stream endpoint stops making progress. |
| Cancellation | Cancel safely at a statement, batch, or job boundary and report the resulting state. |
| Retry | Retry only when idempotency and ordering evidence make it safe. |
| Quarantine | Store invalid records with enough authorized evidence for review. |
| Resume | Resume only from a validated boundary. |

The job state should be inspectable through authorized `SHOW` or `DESCRIBE` surfaces.

## Diagnostics And Refusals

| Condition | Expected diagnostic class |
| --- | --- |
| Surface not available in the build | Unsupported. |
| Feature recognized but not licensed | Unlicensed. |
| Missing administrative privilege | Authorization denied. |
| Target hidden by sandbox | Sandbox denied or object not visible. |
| Server-local location not policy-admitted | Location denied. |
| Stream frame invalid | Stream invalid. |
| Manifest missing or inconsistent | Manifest validation failure. |
| Descriptor mapping missing | Mapping required or incompatible descriptor. |
| Ordering token ambiguous | Ordering ambiguous. |
| Idempotency key missing | Idempotency required. |
| Protected material would leak | Protected-material violation. |
| Recovery-required state | Operation fenced until recovery action completes. |
| Cutover conditions not met | Cutover refused. |

Diagnostics should include a job UUID or statement UUID, route name, visible target, phase, progress boundary, and refusal vector where disclosure policy permits it.

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| backup_stmt | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |
| cluster_publish_options | grammar_production | archive_replication | yes | rs.sbsql.cluster_private_refusal.v1 |
| restore_options | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |
| restore_stmt | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |
| backup_options | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |
| changefeed_options | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |
| archive_stmt | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |
| archive_replication_stmt | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |
| changefeed_stmt | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |
| replication_stmt | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |

## Verification Checklist

| Check | Required outcome |
| --- | --- |
| Parse | The statement family and action are recognized by SBsql. |
| Bind | Database, schema, table, stream, backup set, manifest, endpoint, mapping, and option descriptors resolve. |
| Authorize | The effective user or agent UUID has the required administrative and object privileges. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Validate | Stream, manifest, descriptor, mapping, location, and endpoint policy checks pass. |
| Execute | Work uses engine-owned backup, restore, replication, or migration routes. |
| Finalize | Local applied changes commit or roll back through MGA finality. |
| Recover | Interrupted jobs resume, abort, or fence without silent partial success. |
| Render | Results and diagnostics expose only authorized information. |

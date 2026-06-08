# Transaction Control

This page is part of the SBsql Language Reference Manual. It documents transaction boundaries, autocommit behavior, isolation options, savepoints, locks, runtime inspection, and recovery-facing transaction states for SBsql.

Generation task: `syntax_reference_transaction_control`

Related pages: [Transactions And Recovery](../core_paradigms/transactions_and_recovery.md), [Security And Privileges](security_and_privilege_statements.md), [Procedural SQL](procedural_sql.md), [Procedural Blocks](procedural_sql_blocks.md), [Table Lifecycle](table.md), [Index Lifecycle](index.md), [View Lifecycle](view.md), [Backup, Restore, Replication, And Migration](backup_restore_replication_migration.md), and [Refusal Vectors](refusal_vectors.md).

## Purpose

Transaction statements request a change to the current session's transaction state. ScratchBird MGA remains the final authority for transaction identity, snapshots, visibility, commit, rollback, cleanup horizons, and recovery. Parser text, client state, driver state, timestamps, UUID order, or log text do not own transaction finality.

Use explicit transaction control for scripts that mix DML, DDL, routines, bridge operations, logical stream work, metadata changes, or long reads. Autocommit is a session profile that creates engine-owned transaction boundaries around individual statements; it is not a separate finality model.

## Transaction Authority Model

| Concern | Authority |
| --- | --- |
| Transaction identity | Engine-allocated transaction UUID and local transaction number. |
| Transaction state | Durable MGA transaction inventory. |
| Snapshot | Engine snapshot descriptor with visible-through boundary, active exclusions, catalog epoch, security epoch, policy epoch, and isolation profile. |
| Visibility | MGA inventory, row-version metadata, delete/replacement markers, snapshot rules, page validity, and security policy. |
| Commit | Engine publishes committed state only after required finality evidence and sync/fence policy succeed. |
| Rollback | Engine marks rollback state, releases or compensates resources, and makes transaction versions non-visible. |
| Savepoint | Engine-owned marker inside one transaction; not an independent transaction. |
| Recovery | Engine classifies durable transaction inventory before ordinary work resumes. |

## Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Begin | `BEGIN`, `BEGIN TRANSACTION`, `BEGIN WORK`, `START TRANSACTION` | Starts an explicit engine transaction after admission checks. |
| Set transaction | `SET TRANSACTION ...` | Sets transaction options for the next transaction or current transaction where policy admits it. |
| Commit | `COMMIT`, `COMMIT WORK`, `COMMIT TRANSACTION` | Requests durable finality for the active transaction. Success means MGA finality accepted the commit. |
| Rollback | `ROLLBACK`, `ROLLBACK WORK`, `ROLLBACK TRANSACTION` | Requests full rollback of the active transaction. |
| Savepoint | `SAVEPOINT name` | Creates or replaces a transaction-local rollback marker. |
| Rollback to savepoint | `ROLLBACK TO SAVEPOINT name` | Reverts work after the savepoint while keeping the transaction active. |
| Release savepoint | `RELEASE SAVEPOINT name` | Removes a savepoint marker and keeps later work. |
| Show transaction | `SHOW TRANSACTION`, `SHOW TRANSACTIONS`, `SHOW SAVEPOINTS` | Returns authorized transaction/session state projections. |
| Lock table | `LOCK TABLE ...` | Requests an admitted lock mode inside the active transaction. Locking never bypasses MGA visibility. |

Prepared and limbo transaction states may appear in recovery and diagnostic surfaces. This page does not define a general public `PREPARE TRANSACTION` statement unless a policy-admitted route exposes it. Any such route must still use MGA transaction inventory as final authority and must fail closed when the outcome is uncertain.

## Syntax

```ebnf
transaction_statement ::=
      begin_transaction
    | set_transaction
    | commit_transaction
    | rollback_transaction
    | savepoint_statement
    | lock_table_statement
    | show_transaction_statement ;
```

```ebnf
begin_transaction ::=
      BEGIN (TRANSACTION | WORK)? transaction_option_list?
    | START TRANSACTION transaction_option_list? ;
```

```ebnf
transaction_option_list ::=
    transaction_option ("," transaction_option)* ;

transaction_option ::=
      READ ONLY
    | READ WRITE
    | ISOLATION LEVEL isolation_level
    | WAIT
    | NO WAIT
    | LOCK TIMEOUT duration_literal
    | SNAPSHOT snapshot_option
    | ACCESS MODE access_mode ;

isolation_level ::=
      READ COMMITTED
    | SNAPSHOT
    | SNAPSHOT TABLE STABILITY
    | SERIALIZABLE ;
```

```ebnf
set_transaction ::=
      SET TRANSACTION transaction_option_list
    | SET SESSION TRANSACTION transaction_option_list ;
```

```ebnf
commit_transaction ::=
    COMMIT (TRANSACTION | WORK)? transaction_completion_option* ;

rollback_transaction ::=
    ROLLBACK (TRANSACTION | WORK)? transaction_completion_option* ;

transaction_completion_option ::=
      AND CHAIN
    | AND NO CHAIN
    | RELEASE
    | NO RELEASE ;
```

```ebnf
savepoint_statement ::=
      SAVEPOINT identifier
    | ROLLBACK TO SAVEPOINT? identifier
    | RELEASE SAVEPOINT identifier ;
```

```ebnf
lock_table_statement ::=
    LOCK TABLE qualified_name lock_mode? ;

lock_mode ::=
      IN SHARED MODE
    | IN EXCLUSIVE MODE
    | IN UPDATE MODE ;
```

```ebnf
show_transaction_statement ::=
      SHOW TRANSACTION
    | SHOW TRANSACTIONS
    | SHOW SAVEPOINTS ;
```

SBsql is context sensitive. Transaction words are command words in this statement family and should not be treated as globally reserved outside their transaction context.

## Begin Transaction

`BEGIN TRANSACTION` creates an explicit transaction for the session. Admission must allocate engine transaction identity and construct a snapshot before user-visible work begins.

Admission checks include:

- session authentication and authorization;
- database attachment state;
- sandbox root;
- requested access mode and isolation level;
- memory and resource policy;
- lock and timeout policy;
- filespace and catalog readiness;
- recovery-required state;
- current session transaction state.

Example:

```sql
begin transaction
  isolation level snapshot
  read write;

insert into app.audit_event(event_id, event_text)
values (:event_id, :event_text);

commit;
```

Attempting to begin a second ordinary transaction while one is active must be refused unless a specific policy-owned nested or autonomous route admits it.

## Transaction Options

| Option | Meaning |
| --- | --- |
| `READ ONLY` | Transaction may read but must refuse ordinary data mutation. Administrative and diagnostic actions still require their own policy. |
| `READ WRITE` | Transaction may request reads and writes subject to privileges, policy, locks, and storage admission. |
| `ISOLATION LEVEL READ COMMITTED` | Each statement reads committed data according to the read-committed profile. |
| `ISOLATION LEVEL SNAPSHOT` | Transaction reads through a stable snapshot boundary. |
| `ISOLATION LEVEL SNAPSHOT TABLE STABILITY` | Snapshot isolation plus table-stability semantics where lock and policy admission allow it. |
| `ISOLATION LEVEL SERIALIZABLE` | Strongest public isolation profile. Conflicts must be detected or refused according to the operation policy. |
| `WAIT` | Wait for admitted lock/resource conflicts according to timeout policy. |
| `NO WAIT` | Refuse immediately when a required lock/resource cannot be acquired. |
| `LOCK TIMEOUT` | Sets a transaction lock wait bound where policy admits it. |
| `SNAPSHOT` | Requests a specific snapshot behavior where the session and database policy allow it. |

The engine may refuse an option combination that cannot be made correct for the requested operation.

## Autocommit

Autocommit is a session behavior. It maps each eligible statement or statement group to an engine-owned transaction:

1. open an engine transaction;
2. execute the admitted SBLR operation;
3. commit on success;
4. rollback on failure;
5. open or preserve a replacement transaction only when the session profile requires it.

Autocommit success is commit success only when MGA finality succeeds. A statement result is not a durable commit proof by itself.

Example behavior:

```text
autocommit statement succeeds -> commit requested -> commit finality accepted -> result delivered
autocommit statement fails    -> rollback requested -> rollback state accepted -> diagnostic delivered
```

If autocommit cleanup or replacement fails, the session must return a diagnostic that preserves the uncertain state instead of reporting ordinary success.

## Commit

`COMMIT` requests finality for the active transaction.

Commit must:

1. verify the transaction is active and committable;
2. verify security, policy, resource, and recovery state still admit finality;
3. publish the committed state through the durable MGA transaction inventory;
4. satisfy the configured sync/fence policy;
5. advance visibility and cleanup horizons where applicable;
6. invalidate dependent caches, metadata, and snapshots;
7. return finality evidence or a failure diagnostic.

Example:

```sql
begin transaction;
update app.orders
   set order_state = 'closed'
 where order_id = :order_id;
commit;
```

If the connection drops before the client receives the result, the client must inspect transaction state through an admitted recovery/diagnostic route rather than guessing from client-side state.

## Rollback

`ROLLBACK` requests rollback of the active transaction.

Rollback must:

1. verify the transaction is active or in a rollback-capable state;
2. mark the transaction rolling back;
3. release, compensate, or retire transaction-owned resources;
4. make row versions created by the transaction non-visible;
5. remove savepoint state;
6. publish rolled-back state in the transaction inventory;
7. return rollback evidence or a failure diagnostic.

Example:

```sql
begin transaction;
delete from app.staging_order
 where loaded_batch_id = :batch_id;
rollback;
```

Rollback does not erase audit evidence that policy requires to remain available.

## Savepoints

A savepoint is a named marker inside the current transaction. It is not an independent transaction and cannot commit independently.

```sql
begin transaction;

insert into app.audit_event(event_id, event_text)
values (:id, 'started');

savepoint after_audit;

insert into app.order_event(order_id, event_text)
values (:order_id, 'processing');

rollback to savepoint after_audit;

insert into app.audit_event(event_id, event_text)
values (:id2, 'order event skipped');

commit;
```

Savepoint rules:

| Rule | Contract |
| --- | --- |
| Scope | Savepoint names are scoped to the active transaction. |
| Creation | Creating a savepoint records an engine-owned rollback marker. |
| Replacement | Reusing a savepoint name replaces or shadows the prior marker according to session policy; behavior must be deterministic. |
| Rollback to | Reverts work after the marker and keeps the transaction active. |
| Release | Removes the marker. Later rollback to that name must fail unless a prior marker remains visible by policy. |
| Commit | Removes all savepoints. |
| Full rollback | Removes all savepoints. |
| Recovery | Savepoint markers are not finality authority. Recovery uses transaction inventory. |

## Chain, Retain, And Release Behavior

Completion options control the session boundary after commit or rollback:

| Option | Contract |
| --- | --- |
| `AND CHAIN` | Complete the current transaction and start a new transaction with compatible options. New identity and snapshot are allocated. |
| `AND NO CHAIN` | Complete the current transaction without automatically starting another explicit transaction. |
| `RELEASE` | Complete the transaction and release/detach the session resource where policy admits it. |
| `NO RELEASE` | Complete the transaction and keep the session attached. |

Retain-style behavior must be explicit in the transaction profile. Retaining a cursor, snapshot, or stream cannot make an old transaction remain finality authority after commit or rollback.

## Isolation And Visibility

ScratchBird isolation is MGA-based.

| Isolation | User-Facing Rule |
| --- | --- |
| `READ COMMITTED` | A statement sees committed versions according to the read-committed snapshot profile. Later statements may see newer committed data. |
| `SNAPSHOT` | All statements in the transaction see through the transaction snapshot, plus the transaction's own writes. |
| `SNAPSHOT TABLE STABILITY` | Uses a transaction snapshot and admitted table-stability locks or equivalent conflict prevention. |
| `SERIALIZABLE` | Requires conflict detection or prevention sufficient for serializable behavior under the admitted operation set. |

All isolation levels remain subject to:

- transaction inventory;
- row-version metadata;
- delete and replacement markers;
- index candidate recheck;
- security policy;
- row-level policy and masks;
- page and filespace validity;
- recovery-required fences.

An index can accelerate candidate selection, but it cannot decide final row visibility.

## Locking

`LOCK TABLE` is a transaction-scoped request for lock admission. It does not create transaction finality and does not bypass security, row visibility, or policy checks.

```sql
begin transaction;
lock table app.orders in update mode;
update app.orders
   set order_state = 'queued'
 where order_state = 'new';
commit;
```

Locking can be refused for privilege, timeout, deadlock, policy, resource, or recovery reasons. A lock acquired by a transaction is released or compensated during commit, rollback, session reset, or recovery.

## DDL And Transaction Control

DDL is catalog mutation and follows transaction visibility unless a specific administrative route defines an atomic standalone operation.

```sql
begin transaction;
create table app.import_batch (
  batch_id uuid primary key,
  loaded_at timestamp with time zone
);
comment on table app.import_batch is 'Incoming logical import batches';
commit;
```

If the transaction rolls back, the catalog mutation rolls back. Dependent metadata, prepared statements, plans, and support projections must not observe a half-applied catalog state.

## Procedural Transaction Boundaries

Procedural routines run inside a caller or engine-defined transaction context unless a policy-admitted autonomous route exists.

| Context | Transaction Rule |
| --- | --- |
| Procedure called inside a transaction | Uses caller transaction unless routine security mode and policy define a different admitted context. |
| Function inside an expression | Must not commit or roll back the caller transaction. |
| Trigger | Executes inside the firing statement's transaction context. |
| Execute block | Uses caller transaction unless an autonomous form is explicitly admitted. |
| Autonomous block | Requires its own admitted transaction identity and recovery behavior. It cannot borrow finality from the outer transaction. |

See [Procedural SQL](procedural_sql.md) and [Procedural Blocks](procedural_sql_blocks.md).

## Bridge And Remote Work

A bridge can create local and remote work, but each database keeps its own transaction authority. A local transaction cannot make a remote database committed by assertion, and a remote transaction cannot make the local database committed by assertion.

For ordinary remote table access, the local transaction owns local visibility and the remote transaction owns remote visibility. Distributed query and cross-node finality are outside ordinary local transaction control and require explicit policy-owned routes.

## Prepared, Limbo, And Recovery-Facing States

ScratchBird may classify transaction inventory entries as active, committing, committed, rolling back, rolled back, prepared, limbo, in-doubt, or recovery-required depending on durable evidence.

These states matter for diagnostics and recovery:

| State | Meaning |
| --- | --- |
| `active` | Transaction has started and has not requested finality. |
| `committing` | Commit is in progress and recovery must classify the durable evidence. |
| `committed` | Commit finality is durable and visible according to snapshots. |
| `rolling_back` | Rollback is in progress and recovery must finish or classify it. |
| `rolled_back` | Transaction's versions are not visible. |
| `prepared` | A policy-admitted prepare phase has durable evidence but final decision is not yet complete. |
| `limbo` | Outcome requires recovery classification or operator/policy decision. |
| `recovery_required` | Ordinary writes are fenced until recovery action completes. |

Public transaction control must not silently turn prepared or limbo state into committed or rolled-back state. When uncertain, fail closed.

## SHOW Transaction Surfaces

Inspection surfaces are authorized projections:

```sql
show transaction;
show transactions;
show savepoints;
```

Expected fields may include:

- transaction UUID;
- local transaction number;
- state;
- isolation profile;
- access mode;
- snapshot visible-through boundary;
- active savepoint names visible to the caller;
- lock wait policy;
- timeout policy;
- catalog/security/policy epochs;
- recovery-required flag;
- commit/rollback evidence status.

Inspection must redact or hide transaction information according to security policy.

## Failure Modes

| Failure | Required Behavior |
| --- | --- |
| No active transaction | `COMMIT`, `ROLLBACK`, savepoint, and lock operations refuse unless autocommit/profile policy defines a replacement behavior. |
| Transaction already active | `BEGIN` refuses unless an admitted nested/autonomous route exists. |
| Read-only violation | Mutating statement refuses before durable mutation. |
| Invalid isolation option | Refuse during bind or admission. |
| Lock timeout or no-wait conflict | Refuse with a lock diagnostic and leave the transaction state explicit. |
| Savepoint missing | `ROLLBACK TO` or `RELEASE` refuses without changing transaction finality. |
| Commit uncertainty | Return an uncertain/recovery diagnostic; client must inspect state. |
| Rollback uncertainty | Return an uncertain/recovery diagnostic; recovery must finish or fence. |
| Recovery required | New write transactions refuse until recovery-required state is cleared. |
| Session disconnect | Engine classifies active transaction according to session policy and durable inventory. |

## Practical Patterns

### Explicit Unit Of Work

```sql
begin transaction isolation level snapshot read write;

update app.inventory
   set on_hand = on_hand - :quantity
 where item_id = :item_id;

insert into app.order_event(order_id, event_text)
values (:order_id, 'reserved inventory');

commit;
```

### Savepoint Recovery Inside A Transaction

```sql
begin transaction;

savepoint before_optional_note;

insert into app.order_note(order_id, note_text)
values (:order_id, :note_text);

rollback to savepoint before_optional_note;

insert into app.order_event(order_id, event_text)
values (:order_id, 'note skipped');

commit;
```

### Read-Only Snapshot

```sql
begin transaction isolation level snapshot read only;

select order_state, count(*)
from app.orders
group by order_state
order by order_state;

commit;
```

### Autocommit Conceptual Flow

```text
insert statement
  -> engine opens transaction
  -> statement executes
  -> engine commits on success
  -> result reports command completion and finality evidence
```

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | Every transaction statement shape is recognized by SBsql. |
| Bind | Options, savepoint names, targets, lock modes, and session state bind exactly. |
| Authorize | Session and effective principal can start, inspect, lock, or finalize the transaction. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Begin | Allocates engine transaction identity and snapshot before visible work. |
| Commit | Reports success only after MGA finality and sync/fence policy succeed. |
| Rollback | Makes transaction versions non-visible and records rollback state. |
| Savepoint | Rolls back or releases only transaction-local work after the marker. |
| Autocommit | Commits on success and rolls back on failure through engine-owned transaction boundaries. |
| Isolation | Visibility matches the requested isolation profile and security policy. |
| Recovery | Crash/restart never leaves silent transaction inconsistency. |

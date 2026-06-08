# Trigger Lifecycle

This page is part of the SBsql Language Reference Manual. It documents table triggers, view triggers, statement triggers, event triggers, monitor triggers, transition values, trigger ordering, trigger security, recursion policy, and trigger execution boundaries.

Generation task: `syntax_reference_trigger_lifecycle`

Related pages: [Procedural SQL](procedural_sql.md), [Procedural SQL Triggers And Events](procedural_sql_triggers_and_events.md), [Procedural Blocks](procedural_sql_blocks.md), [Procedural Exceptions](procedural_sql_exceptions.md), [Table Lifecycle](table.md), [View Lifecycle](view.md), [Transaction Control](transaction_control.md), [Security And Privileges](security_and_privilege_statements.md), [Policy, Mask, And RLS Lifecycle](policy_mask_and_rls.md), and [Refusal Vectors](refusal_vectors.md).

## Purpose

A trigger binds executable behavior to an engine event. That event may be a table change, a view change routed through `INSTEAD OF`, a statement-level table operation, a database, session, transaction, catalog, security, policy, operational lifecycle event, or an admitted monitor event. The trigger definition records event metadata, timing, scope, target binding, transition descriptors, security mode, ordering, dependency graph, original source text, and executable SBLR or trusted UDR binding.

Trigger execution is engine dispatched. The parser cannot invent `old`, `new`, transition tables, event payloads, write authority, or transaction finality. A trigger body runs inside a defined transaction and security context and remains subject to MGA visibility, authorization, row policy, masking, protected-material policy, and recovery fences.

## Trigger Families

| Family | Primary Surface | Firing Source | Main Use |
| --- | --- | --- | --- |
| Table row trigger | `CREATE TRIGGER ... FOR EACH ROW` | One affected row from `INSERT`, `UPDATE`, or `DELETE` | Validate or transform row values, maintain audit rows, enforce local invariants. |
| Table statement trigger | `CREATE TRIGGER ... FOR EACH STATEMENT` | One table-changing statement | Audit statement-level changes, maintain summaries, inspect transition tables where admitted. |
| View trigger | `CREATE TRIGGER ... INSTEAD OF ... ON VIEW` | DML routed to a view or virtual relation | Translate view writes into base-table or routine work. |
| Event trigger | `CREATE EVENT TRIGGER` | Engine event such as database connect/disconnect, session lifecycle, transaction start/commit/rollback, catalog, security, policy, DDL, or operational event | Audit, admit, deny, or react to lifecycle and metadata events according to event class policy. |
| Monitor trigger | `CREATE MONITOR TRIGGER` | Metrics, diagnostic, threshold, or observability event | Record or route operational observations without owning source event finality. |
| Internal trigger | Engine-owned metadata | Engine-defined event | Reserved for engine/catalog maintenance and not created through ordinary user text. |

This page documents the public SBsql behavior. Engine-owned internal triggers may appear in authorized diagnostics but are not ordinary user lifecycle objects.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create table trigger | `CREATE TRIGGER` | Creates a durable trigger UUID, table/view target UUID, timing, event set, scope, transition descriptors, ordering, security mode, source text, executable binding, and dependencies. |
| Create event trigger | `CREATE EVENT TRIGGER` | Creates a durable event-trigger UUID, event class binding, filter descriptor, security mode, source text, executable binding, and dependencies. |
| Create monitor trigger | `CREATE MONITOR TRIGGER` | Creates a durable monitor-trigger UUID, monitor source binding, threshold/filter descriptor, delivery policy, source text, executable binding, and dependencies. |
| Alter | `ALTER TRIGGER`, `ALTER EVENT TRIGGER`, `ALTER MONITOR TRIGGER` | Changes admitted metadata such as active state, ordering, security mode, filter, failure policy, or compiled representation. |
| Enable/disable | `ALTER TRIGGER ... ACTIVE`, `ALTER TRIGGER ... INACTIVE`, `ENABLE TRIGGER`, `DISABLE TRIGGER` | Changes firing admission without changing the trigger UUID. |
| Rename | `RENAME TRIGGER ... TO ...` | Changes resolver name only. Target binding, event binding, and executable identity remain UUID-bound. |
| Comment | `COMMENT ON TRIGGER ... IS ...` | Stores authorized descriptive metadata on the trigger catalog row. |
| Show | `SHOW TRIGGER ...`, `SHOW TRIGGERS`, `SHOW EVENT TRIGGERS`, `SHOW MONITOR TRIGGERS` | Returns authorized trigger metadata, active state, readiness, event bindings, and dependency summaries. |
| Describe | `DESCRIBE TRIGGER ...` | Returns one trigger's target, timing, events, scope, transition descriptors, ordering, security mode, body binding, and dependencies according to disclosure policy. |
| Recreate | `RECREATE TRIGGER ...` | Replaces the trigger through fresh parse, bind, source retention, executable encoding, dependency invalidation, and catalog mutation. |
| Drop | `DROP TRIGGER ... [RESTRICT | CASCADE]` | Retires the trigger only after dependency, privilege, transaction, recovery, and sandbox checks pass. |

Trigger lifecycle DDL is transactional. A created, altered, renamed, recreated, enabled, disabled, or dropped trigger becomes visible only when the owning transaction commits.

## Syntax

```ebnf
create_table_trigger_statement ::=
    CREATE trigger_options? TRIGGER qualified_name
    trigger_timing trigger_event_list
    ON trigger_target
    trigger_referencing?
    trigger_scope?
    trigger_when?
    trigger_ordering?
    trigger_security?
    AS routine_body ;
```

```ebnf
create_event_trigger_statement ::=
    CREATE EVENT TRIGGER qualified_name
    ON event_trigger_event
    event_trigger_filter?
    event_trigger_audit_clause?
    trigger_ordering?
    trigger_security?
    AS routine_body ;
```

```ebnf
create_monitor_trigger_statement ::=
    CREATE MONITOR TRIGGER qualified_name
    ON monitor_trigger_source
    monitor_trigger_filter?
    trigger_ordering?
    trigger_security?
    AS routine_body ;
```

```ebnf
trigger_timing ::=
      BEFORE
    | AFTER
    | INSTEAD OF ;

trigger_event_list ::=
    trigger_event (OR trigger_event)* ;

trigger_event ::=
      INSERT
    | UPDATE
    | UPDATE OF column_name_list
    | DELETE
    | TRUNCATE ;

trigger_target ::=
      TABLE qualified_name
    | VIEW qualified_name
    | qualified_name ;

trigger_scope ::=
      FOR EACH ROW
    | FOR EACH STATEMENT ;

trigger_referencing ::=
    REFERENCING transition_reference+ ;

transition_reference ::=
      OLD ROW AS identifier
    | NEW ROW AS identifier
    | OLD TABLE AS identifier
    | NEW TABLE AS identifier ;

trigger_when ::=
    WHEN "(" boolean_expression ")" ;
```

```ebnf
event_trigger_event ::=
      ddl_event
    | catalog_event
    | security_event
    | policy_event
    | database_lifecycle_event
    | session_lifecycle_event
    | transaction_lifecycle_event
    | operational_event
    | diagnostic_event ;

database_lifecycle_event ::=
      DATABASE CONNECT
    | DATABASE DISCONNECT
    | DATABASE ATTACH
    | DATABASE DETACH
    | DATABASE OPEN
    | DATABASE CLOSE
    | DATABASE STARTUP
    | DATABASE SHUTDOWN
    | DATABASE RECOVERY REQUIRED
    | DATABASE RECOVERY COMPLETE ;

session_lifecycle_event ::=
      SESSION CONNECT
    | SESSION DISCONNECT
    | SESSION AUTHENTICATE
    | SESSION AUTHORIZATION CHANGE
    | SESSION CANCEL
    | SESSION TIMEOUT ;

transaction_lifecycle_event ::=
      TRANSACTION START
    | TRANSACTION PREPARE
    | TRANSACTION COMMIT
    | TRANSACTION COMMIT START
    | TRANSACTION COMMIT END
    | TRANSACTION COMMIT FAILED
    | TRANSACTION ROLLBACK
    | TRANSACTION ROLLBACK START
    | TRANSACTION ROLLBACK END
    | TRANSACTION ROLLBACK FAILED
    | TRANSACTION SAVEPOINT CREATE
    | TRANSACTION SAVEPOINT RELEASE
    | TRANSACTION SAVEPOINT ROLLBACK ;
```

```ebnf
alter_trigger_statement ::=
    ALTER TRIGGER qualified_name alter_trigger_action+ ;

alter_trigger_action ::=
      ACTIVE
    | INACTIVE
    | ENABLE
    | DISABLE
    | SET ORDER trigger_ordering
    | SET SECURITY trigger_security_mode
    | SET FAILURE POLICY qualified_name
    | COMPILE
    | VALIDATE ;
```

```ebnf
rename_trigger_statement   ::= RENAME TRIGGER qualified_name TO identifier ;
comment_trigger_statement  ::= COMMENT ON TRIGGER qualified_name IS string_literal ;
show_trigger_statement     ::= SHOW TRIGGER qualified_name | SHOW TRIGGERS ;
describe_trigger_statement ::= DESCRIBE TRIGGER qualified_name ;
recreate_trigger_statement ::= RECREATE TRIGGER qualified_name ... ;
drop_trigger_statement     ::= DROP TRIGGER qualified_name (RESTRICT | CASCADE)? ;
```

SBsql is context sensitive. Trigger words are command words in trigger statements and do not need to be globally reserved in unrelated expression positions.

## Table Trigger Timing

| Timing | Applies To | Contract |
| --- | --- | --- |
| `BEFORE` | Table row triggers for insert/update/delete where admitted | Runs before the row change is applied. May assign admitted `new` values for insert/update row triggers. |
| `AFTER` | Table row or statement triggers | Runs after the row or statement effect is staged, but before transaction finality. The change is not committed yet. |
| `INSTEAD OF` | Views or virtual relation targets where admitted | Runs instead of the requested base operation and must perform the admitted replacement work itself. |

`BEFORE` triggers cannot make a row committed. `AFTER` triggers cannot assume a row is committed. `INSTEAD OF` triggers cannot bypass authorization on the replacement work they perform.

## Table Trigger Events

| Event | Row Transition Values | Statement Transition Tables | Notes |
| --- | --- | --- | --- |
| `INSERT` | `new` available, `old` unavailable | `new table` where admitted | `BEFORE INSERT` may assign admitted `new` fields. |
| `UPDATE` | `old` and `new` available | `old table` and `new table` where admitted | `UPDATE OF column_list` fires only when admitted changed columns match the list. |
| `DELETE` | `old` available, `new` unavailable | `old table` where admitted | `old` is read-only. |
| `TRUNCATE` | No row transition values | Statement event only | Must be statement-level and policy-admitted. |
| Multi-event | Depends on current event | Body must inspect event context before using transition values that are not always present. |

Unavailable transition values must fail at bind or execution with a clear diagnostic. They must not evaluate to arbitrary `null` unless the trigger event descriptor explicitly defines that behavior.

## Row Triggers

Row triggers fire once for each affected row.

```sql
create trigger app.orders_bi
before insert on table app.orders
for each row
when (new.order_id is null)
as
begin
  new.order_id = gen_uuid_v7();
  new.created_at = current_timestamp;
end;
```

Row trigger rules:

| Concern | Contract |
| --- | --- |
| Row descriptor | `old` and `new` are descriptor-bound transition rows provided by the engine. |
| Mutation | Only admitted `new` fields in `BEFORE INSERT` and `BEFORE UPDATE` row triggers may be assigned. |
| Generated columns | Generated columns may refuse direct trigger assignment. |
| Identity columns | Identity policy controls whether a trigger can assign or override values. |
| Domains | Assignments to `new` fields validate domains, defaults, constraints, masks, and column policy. |
| Protected values | Triggers cannot read or emit raw protected material unless policy releases it. |
| Visibility | The row is still governed by the owning transaction snapshot and finality state. |

## Statement Triggers

Statement triggers fire once for a statement, even if the statement affects zero rows.

```sql
create trigger app.orders_as
after update on table app.orders
for each statement
as
begin
  insert into app.table_audit(table_name, event_name, occurred_at)
  values ('app.orders', trigger_event(), current_timestamp);
end;
```

Statement trigger rules:

| Concern | Contract |
| --- | --- |
| Transition rows | `old` and `new` row variables are not available. |
| Transition tables | `old table` and `new table` descriptors may be available where admitted. |
| Zero-row statements | The trigger still fires if the event is statement-scoped and admitted. |
| Summary work | Summary/audit writes are ordinary writes in the same transaction unless an autonomous route is admitted. |
| Ordering | Statement triggers order separately from row triggers unless an ordering policy explicitly combines them. |

## View And INSTEAD OF Triggers

`INSTEAD OF` triggers let a writable view or virtual relation define the replacement work for an insert, update, or delete request.

```sql
create trigger app.customer_view_iio
instead of insert on view app.customer_view
for each row
as
begin
  insert into app.customer(customer_id, display_name)
  values (new.customer_id, new.display_name);
end;
```

Rules:

| Concern | Contract |
| --- | --- |
| Target | The target must be a view or admitted virtual relation. |
| Base operation | The original operation is not applied automatically. The trigger body must perform any intended work. |
| Authorization | The caller must be authorized for the view operation, and the trigger execution mode must authorize replacement work. |
| Transition values | `new` and `old` are view-row descriptors, not base-table rows. |
| Result count | The trigger must report command completion according to the admitted view-update contract. |

## Trigger Calls With Cursor Arguments

A trigger body can declare a cursor, open it under the trigger's transaction and security context, and pass the cursor to a procedure or function that accepts a `cursor` descriptor.

```sql
create trigger app.orders_ai_cursor_audit
after insert on table app.orders
for each row
as
begin
  declare cursor c_customer_orders for
    select order_id, order_total
      from app.orders
     where customer_id = new.customer_id
     order by submitted_at;

  open c_customer_orders;
  execute procedure app.audit_order_stream(c_customer_orders);
  close c_customer_orders;
end;
```

Trigger cursor-call rules:

| Concern | Contract |
| --- | --- |
| Cursor source | The cursor must be declared or otherwise visible in the trigger body context. |
| Event context | The cursor uses the trigger event's transaction, snapshot, transition values, and security mode. |
| Routine argument | The callee receives a borrowed cursor handle unless transfer is explicitly admitted. |
| Lifetime | A trigger-scoped cursor cannot outlive the trigger event unless an admitted holdable route exists. |
| Transition data | A cursor query may reference available `old`, `new`, transition table, or `event` descriptors only where the trigger family exposes them. |
| Failure handling | If the callee fails, trigger failure policy decides whether the source operation is refused, quarantined, or diagnosed. |

## REFERENCING Clause

The `REFERENCING` clause renames transition values or transition tables for the trigger body.

```sql
create trigger app.orders_audit_au
after update on table app.orders
referencing old row as old_order new row as new_order
for each row
as
begin
  insert into app.order_audit(order_id, old_state, new_state)
  values (new_order.order_id, old_order.order_state, new_order.order_state);
end;
```

Renaming transition values changes body names only. It does not change event availability, mutability, descriptor identity, or authorization.

## WHEN Filters

`WHEN` filters are side-effect-free boolean expressions evaluated before the trigger body.

```sql
create trigger app.orders_state_au
after update of order_state on table app.orders
for each row
when (old.order_state is distinct from new.order_state)
as
begin
  insert into app.order_state_event(order_id, old_state, new_state)
  values (new.order_id, old.order_state, new.order_state);
end;
```

| Rule | Contract |
| --- | --- |
| Result descriptor | The expression must bind to `boolean`. |
| Truth rule | Only `true` fires the trigger body. `false` and `null` skip it. |
| Transition references | Availability follows event, timing, and scope. |
| Security | The filter cannot read unauthorized objects or protected values. |
| Side effects | The filter must not mutate data, call side-effecting routines, or alter transaction state. |

## Event Triggers

Event triggers bind to engine events rather than row changes. Event classes include admitted database lifecycle, session lifecycle, transaction lifecycle, catalog, security, policy, DDL, operational, diagnostic, and monitor-related events.

```sql
create event trigger app.audit_ddl
on ddl_command_end
when event.tag in ('CREATE TABLE', 'ALTER TABLE', 'DROP TABLE')
as
begin
  insert into app.ddl_audit(
    event_tag,
    object_uuid,
    object_name,
    principal_uuid,
    occurred_at
  )
  values (
    event.tag,
    event.object_uuid,
    event.object_name,
    event.principal_uuid,
    current_timestamp
  );
end;
```

Event trigger rules:

| Concern | Contract |
| --- | --- |
| Payload source | Event payload is produced by the engine event dispatcher. |
| Payload mutability | Event payload is read-only. |
| Event identity | Event UUID, event class, event tag, object UUIDs, and statement UUIDs are engine-provided evidence. |
| Filtering | Event filters are side-effect-free and descriptor-bound. |
| Disclosure | Event fields are redacted according to security policy. |
| Failure policy | Failure can abort the source event, quarantine the event, or emit a diagnostic only where policy says so. |
| Recursion | Event triggers require recursion and cascade controls to prevent unbounded event loops. |

Event triggers cannot convert an uncommitted DDL operation into a committed operation. They run under the event's transaction and recovery policy.

## Database, Session, And Transaction Event Triggers

Connection and transaction lifecycle triggers are event triggers. They do not have `old` or `new` row descriptors. They receive a read-only `event` descriptor whose fields depend on the event class, lifecycle phase, security policy, and redaction policy.

Lifecycle trigger syntax uses the ordinary event-trigger form:

```sql
create event trigger app.database_connect_audit
on database connect
as
begin
  insert into app.connection_audit(
    event_name,
    database_uuid,
    principal_uuid,
    session_uuid,
    occurred_at
  )
  values (
    event.name,
    event.database_uuid,
    event.principal_uuid,
    event.session_uuid,
    current_timestamp
  );
end;
```

```sql
create event trigger app.transaction_commit_audit
on transaction commit
as
begin
  insert into app.transaction_audit(
    event_name,
    transaction_uuid,
    session_uuid,
    commit_phase,
    occurred_at
  )
  values (
    event.name,
    event.transaction_uuid,
    event.session_uuid,
    event.phase,
    current_timestamp
  );
end;
```

Lifecycle event classes:

| Event Class | Canonical Events | Descriptor Fields | Failure Behavior |
| --- | --- | --- | --- |
| Database connection admission | `DATABASE CONNECT`, `DATABASE ATTACH` | database UUID, connection UUID, principal UUID, session UUID when allocated, client attributes allowed by policy | May deny the connection before the session becomes usable. Must not leak hidden database identity. |
| Database disconnection | `DATABASE DISCONNECT`, `DATABASE DETACH` | database UUID, connection UUID, principal UUID, session UUID, disconnect reason, error state where visible | Best-effort cleanup/audit event. It cannot keep a closing connection open indefinitely. |
| Database open/close | `DATABASE OPEN`, `DATABASE CLOSE`, `DATABASE STARTUP`, `DATABASE SHUTDOWN` | database UUID, lifecycle state, manager/listener/session evidence visible to policy, recovery state | May deny an unsafe open before admission. Close/shutdown events must obey drain and recovery policy. |
| Recovery state | `DATABASE RECOVERY REQUIRED`, `DATABASE RECOVERY COMPLETE` | database UUID, recovery mode, fence reason, diagnostic reference | Must fail closed when recovery state is uncertain. User trigger work cannot repair storage. |
| Session lifecycle | `SESSION CONNECT`, `SESSION AUTHENTICATE`, `SESSION AUTHORIZATION CHANGE`, `SESSION DISCONNECT`, `SESSION CANCEL`, `SESSION TIMEOUT` | session UUID, principal UUID, role/group evidence, authorization epoch, reason code | Admission events may deny. Terminal events are observation/cleanup events unless policy admits a controlled action. |
| Transaction start | `TRANSACTION START` | transaction UUID, session UUID, isolation descriptor, read/write mode, timeout and policy descriptors | May deny transaction admission before ordinary work is allowed. |
| Transaction prepare | `TRANSACTION PREPARE` | transaction UUID, prepare name where visible, participant evidence, durability policy | May deny prepare before prepared state is recorded. Prepared-state finality remains engine-owned. |
| Transaction commit | `TRANSACTION COMMIT`, `TRANSACTION COMMIT START`, `TRANSACTION COMMIT END`, `TRANSACTION COMMIT FAILED` | transaction UUID, session UUID, commit phase, statement UUID, inventory state, diagnostic reference where visible | `COMMIT`/`COMMIT START` may deny before durable finality. `COMMIT END` is observation after commit finality and cannot undo the commit. |
| Transaction rollback | `TRANSACTION ROLLBACK`, `TRANSACTION ROLLBACK START`, `TRANSACTION ROLLBACK END`, `TRANSACTION ROLLBACK FAILED` | transaction UUID, session UUID, rollback phase, rollback reason, diagnostic reference where visible | `ROLLBACK`/`ROLLBACK START` may run cleanup policy before rollback finality. `ROLLBACK END` is observation and cannot restore rolled-back work. |
| Savepoint lifecycle | `TRANSACTION SAVEPOINT CREATE`, `TRANSACTION SAVEPOINT RELEASE`, `TRANSACTION SAVEPOINT ROLLBACK` | transaction UUID, savepoint UUID/name where visible, nesting depth, reason code | May audit or deny according to policy. It cannot make savepoint rollback into transaction rollback. |

`ON TRANSACTION COMMIT` and `ON TRANSACTION ROLLBACK` are phase-aware event classes. Where a deployment needs separate before/after behavior, use the explicit `START`, `END`, and `FAILED` forms. If the unqualified form is used, the event descriptor must expose `event.phase` so procedural code can distinguish admission, completion, and failure phases where policy publishes them.

Lifecycle trigger boundaries:

| Boundary | Rule |
| --- | --- |
| No row descriptors | Lifecycle event triggers do not expose `old`, `new`, `old table`, or `new table`. |
| Admission vs observation | Events before admission or finality may deny the operation. Events after terminal state are observation and cleanup only. |
| MGA finality | Transaction triggers cannot make a commit or rollback final. The durable transaction inventory remains the authority. |
| Audit durability | Audit rows written inside a transaction that later rolls back are rolled back. Durable lifecycle audit after rollback or disconnect requires an admitted event-owned transaction route. |
| Authentication and authorization | Connection/session lifecycle triggers see only fields authorized for the effective trigger security mode. |
| Cancellation and timeout | Disconnect, cancel, and timeout events must respect deadlines and cannot block resource cleanup indefinitely. |
| Recovery | Recovery-required events must not run unsafe user code unless recovery policy explicitly admits it. |
| Recursion | Lifecycle triggers that create sessions, start transactions, or write audit rows must obey recursion and cascade policy. |

Failure examples:

| Scenario | Required Behavior |
| --- | --- |
| `ON DATABASE CONNECT` trigger denies the connection | Return a connection-refused diagnostic and do not expose a usable session. |
| `ON SESSION AUTHENTICATE` cannot read a protected field | Redact or refuse according to the trigger security mode. |
| `ON TRANSACTION START` fails | Refuse the transaction before ordinary statements can use it. |
| `ON TRANSACTION COMMIT START` fails | Abort commit before finality and preserve the transaction error behavior. |
| `ON TRANSACTION COMMIT END` fails | Preserve committed finality and apply the event failure policy, such as diagnostic or quarantine. |
| `ON TRANSACTION ROLLBACK END` writes to ordinary tables without an event-owned route | Refuse or roll the write into the documented event transaction policy; never resurrect the rolled-back transaction. |
| `ON DATABASE DISCONNECT` exceeds its deadline | Stop trigger work according to disconnect cleanup policy and continue resource release. |

## Monitor Triggers

Monitor triggers bind to observability events such as metrics, thresholds, diagnostics, or operational state changes.

```sql
create monitor trigger app.high_latency_observer
on metric app.request_latency
when event.value_ms > 250
as
begin
  insert into app.monitor_event(metric_name, metric_value, observed_at)
  values (event.metric_name, event.value_ms, current_timestamp);
end;
```

Monitor trigger rules:

| Concern | Contract |
| --- | --- |
| Observation only | The monitor event is evidence. It does not own the state it observes. |
| Payload | Metric and diagnostic payloads are engine-provided and read-only. |
| Backpressure | Monitor trigger execution must respect memory, queue, timeout, and failure policy. |
| Redaction | Diagnostics and support data must be redacted according to policy. |
| Failure | A monitor trigger failure must not silently corrupt the monitored subsystem. |

## Security Mode

Trigger security mode controls which principal context is used to authorize body execution.

| Mode | Contract |
| --- | --- |
| Invoker | Executes with the effective user or agent that caused the event, plus trigger metadata policy. |
| Definer | Executes with the trigger owner's admitted definer context, subject to sandboxing, row policy, masks, and protected-material rules. |
| Engine-owned | Reserved for internal maintenance. Not ordinary user-created trigger authority. |

Even in definer mode, a trigger cannot bypass MGA finality, recovery fences, explicit denial, sandbox roots, protected-material redaction, or provider admission gates.

## Ordering

Multiple triggers can bind to the same target, timing, event, and scope. The engine must fire them in deterministic order.

```sql
alter trigger app.orders_validate_bu
  set order before app.orders_audit_bu;
```

Ordering rules:

| Rule | Contract |
| --- | --- |
| Explicit order | `BEFORE trigger_name` or `AFTER trigger_name` style metadata creates an ordering dependency. |
| Priority | Numeric or named priority can be used where policy admits it. |
| Stable default | If no explicit order exists, the engine uses stable catalog order or refuses when order would affect correctness. |
| Cycle detection | Ordering cycles are refused. |
| Event separation | Row, statement, event, and monitor trigger ordering are separate unless policy explicitly unifies them. |

## Recursion And Cascades

Triggers may execute statements that fire other triggers. The engine must enforce recursion and cascade policy.

| Concern | Required Behavior |
| --- | --- |
| Maximum depth | Refuse when configured recursion depth is exceeded. |
| Self-recursion | Refuse unless an explicit policy-owned route admits it. |
| Same-target mutation | Enforce target mutation policy to prevent unstable read/write loops. |
| Event trigger loops | Detect and refuse unbounded event capture loops. |
| Monitor feedback loops | Prevent monitor trigger output from recursively flooding the monitor event source. |
| Diagnostics | Emit canonical message vectors for recursion or cascade refusal. |

## Transaction Behavior

Table, view, and event triggers execute inside an engine-defined transaction context.

| Trigger Type | Transaction Rule |
| --- | --- |
| Row trigger | Runs inside the firing statement's transaction. |
| Statement trigger | Runs inside the firing statement's transaction. |
| View `INSTEAD OF` trigger | Runs inside the transaction that attempted the view operation. |
| Event trigger | Runs inside the event transaction or event policy context. |
| Database/session lifecycle event trigger | Runs under the lifecycle event policy. Admission-phase events may deny before the operation is admitted; terminal events are observation/cleanup events. |
| Transaction lifecycle event trigger | Runs under the transaction lifecycle phase policy. It may observe or deny only where the phase allows; durable transaction finality remains engine-owned. |
| Monitor trigger | Runs under the admitted monitor delivery policy and any transaction it opens must be engine-owned. |
| Autonomous trigger route | Requires explicit admission and its own engine transaction identity. |

If the source statement rolls back, ordinary trigger work in the same transaction rolls back. A trigger body cannot commit or roll back the caller transaction unless an explicit procedural route admits that operation for the context.

## Error Handling

Trigger failure policy must be deterministic.

| Failure Class | Required Behavior |
| --- | --- |
| Transition value unavailable | Refuse with a transition-context diagnostic. |
| Unauthorized read/write | Refuse and preserve the owning transaction's error behavior. |
| Domain or constraint failure | Refuse the assignment or source statement according to the firing context. |
| Recursion refusal | Refuse before unbounded execution. |
| Event payload redacted | Return redacted fields or refuse access according to policy. |
| Trigger body diagnostic | Propagate, handle, quarantine, or log according to trigger failure policy. |
| Recovery-required state | Refuse unsafe trigger execution until recovery state is cleared. |

Procedural handlers are documented in [Procedural Exceptions](procedural_sql_exceptions.md). A handler does not make source changes committed by itself.

## SHOW And DESCRIBE

`SHOW TRIGGERS` and `DESCRIBE TRIGGER` are authorized metadata projections.

Expected metadata includes:

- trigger UUID;
- resolver name;
- trigger family;
- target object UUID;
- timing;
- event list;
- lifecycle event phase where applicable;
- scope;
- transition references;
- `WHEN` or filter descriptor;
- active/inactive state;
- ordering metadata;
- recursion policy;
- security mode;
- failure policy;
- source text hash or source-retention status;
- executable binding status;
- dependencies;
- catalog/security/policy epochs visible to the caller.

Protected body text, event payload fields, secret references, and security-sensitive dependencies must be redacted when policy requires it.

## Practical Examples

### Before Insert Row Trigger

```sql
create trigger app.orders_bi
before insert on table app.orders
for each row
when (new.order_id is null)
as
begin
  new.order_id = gen_uuid_v7();
  new.created_at = current_timestamp;
end;
```

### After Update Audit Trigger

```sql
create trigger app.orders_total_au
after update of order_total on table app.orders
referencing old row as old_order new row as new_order
for each row
when (old_order.order_total is distinct from new_order.order_total)
as
begin
  insert into app.order_audit(order_id, old_total, new_total)
  values (new_order.order_id, old_order.order_total, new_order.order_total);
end;
```

### Statement Trigger

```sql
create trigger app.orders_delete_as
after delete on table app.orders
for each statement
as
begin
  insert into app.table_audit(table_name, event_name, occurred_at)
  values ('app.orders', trigger_event(), current_timestamp);
end;
```

### View INSTEAD OF Trigger

```sql
create trigger app.customer_view_uio
instead of update on view app.customer_view
for each row
as
begin
  update app.customer
     set display_name = new.display_name
   where customer_id = old.customer_id;
end;
```

### Event Trigger

```sql
create event trigger app.security_event_audit
on security_command_end
as
begin
  insert into app.security_audit(event_tag, principal_uuid, occurred_at)
  values (event.tag, event.principal_uuid, current_timestamp);
end;
```

## Failure Modes

| Failure | Required Behavior |
| --- | --- |
| Unknown target | Refuse with not-found or hidden-object diagnostic according to policy. |
| Invalid timing/event combination | Refuse during bind or admission. |
| Invalid transition reference | Refuse when the referenced value/table is not available for the event, timing, and scope. |
| Invalid `WHEN` expression | Refuse if not boolean, not side-effect free, or unauthorized. |
| Body does not compile | Refuse create/recreate or mark not executable only where policy explicitly admits that state. |
| Active trigger fails | Apply the trigger failure policy and preserve transaction correctness. |
| Ordering ambiguity | Use stable order or refuse where order affects correctness. |
| Recursion limit exceeded | Refuse with recursion diagnostic. |
| Recovery-required state | Refuse unsafe trigger execution. |

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | Every trigger lifecycle statement shape is recognized by SBsql. |
| Bind | Target UUIDs, events, timing, scope, transition descriptors, filters, ordering, and body dependencies resolve exactly. |
| Authorize | Effective user or agent is allowed to create, alter, inspect, or drop the trigger. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Store | Original source and executable binding are retained with UUID identity. |
| Fire | Trigger fires for the documented event, timing, scope, and ordering. |
| Transition values | `old`, `new`, transition tables, and event payloads are available only where documented. |
| Lifecycle events | Database, session, and transaction lifecycle triggers expose only documented event descriptors and phase fields. |
| Security | Invoker/definer mode, sandboxing, policies, masks, and protected material are enforced. |
| Transaction | Trigger work follows the owning transaction and MGA finality. |
| Recursion | Recursion and cascade policy are enforced. |
| Recover | Crash/restart never leaves half-applied trigger catalog state or silent trigger execution ambiguity. |

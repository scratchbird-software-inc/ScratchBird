# Procedural SQL Triggers And Events

This page documents table triggers, event triggers, transition values, `WHEN` filters, event capture, and trigger execution context.

Related pages: [procedural_sql.md](procedural_sql.md), [trigger.md](trigger.md), [procedural_sql_blocks.md](procedural_sql_blocks.md), [procedural_sql_exceptions.md](procedural_sql_exceptions.md), [security_and_privilege_statements.md](security_and_privilege_statements.md), [policy_mask_and_rls.md](policy_mask_and_rls.md).

## Trigger Model

Triggers bind procedural behavior to engine events. A trigger definition records event metadata, timing, scope, relation or event binding, dependency graph, security mode, original source text, and executable SBLR or trusted UDR binding.

Trigger execution happens inside an engine-provided context. The parser cannot invent `old`, `new`, event payloads, write authority, or transaction finality.

## Table Trigger Shape

```sql
create trigger app.orders_bi
before insert on app.orders
for each row
when (new.order_id is null)
as
begin
  new.order_id = gen_uuid_v7();
  new.created_at = current_timestamp;
end;
```

```ebnf
table_trigger           ::= "CREATE" trigger_options? "TRIGGER" trigger_name
                            trigger_timing trigger_event_list
                            "ON" table_ref
                            trigger_referencing?
                            trigger_scope?
                            trigger_when?
                            "AS" routine_body ;
```

The EBNF is descriptive. The exact accepted syntax is profile-aware and context-sensitive.

## Timing

| Timing | Contract |
| --- | --- |
| `before` | Runs before the row or statement change is applied. Can modify admitted `new` transition values in row triggers. |
| `after` | Runs after the row or statement change is applied but before transaction finality. Cannot treat the change as committed. |
| `instead of` | Runs instead of the base operation, normally for view or virtual-object surfaces where admitted. |

## Events

| Event | Row Transition Values | Notes |
| --- | --- | --- |
| `insert` | `new` available; `old` unavailable | `before insert` may assign admitted `new` fields. |
| `update` | `old` and `new` available | `old` is read-only; `new` may be mutable in admitted `before update` contexts. |
| `delete` | `old` available; `new` unavailable | `old` is read-only. |
| multi-event trigger | Depends on event being fired | Code should inspect event context before assuming transition availability. |

## Scope

| Scope | Contract |
| --- | --- |
| `for each row` | Fires once per affected row. Transition row descriptors are available according to event. |
| `for each statement` | Fires once per statement. Transition row values are not available unless transition tables/event descriptors are explicitly admitted. |

## REFERENCING Clause

```sql
create trigger app.orders_audit
after update on app.orders
referencing old row as old_order new row as new_order
for each row
as
begin
  insert into app.order_audit(order_id, old_state, new_state)
  values (new_order.order_id, old_order.order_state, new_order.order_state);
end;
```

The `REFERENCING` clause renames transition values for the trigger body. It does not change transition authority. `old` and `new` remain engine-provided descriptors.

## WHEN Filter

```sql
create trigger app.orders_state_au
after update on app.orders
for each row
when (old.order_state is distinct from new.order_state)
as
begin
  insert into app.order_state_event(order_id, old_state, new_state)
  values (new.order_id, old.order_state, new.order_state);
end;
```

The `WHEN` expression is parsed, bound, and executed before the trigger body for each trigger firing.

| Rule | Behavior |
| --- | --- |
| Descriptor binding | `WHEN` must bind to a boolean descriptor. |
| Null behavior | Only `true` fires the body. `false` and `null` skip the body. |
| Transition references | `old`/`new` availability follows event and timing. |
| Authorization | The filter cannot read unauthorized objects. |
| Side effects | `WHEN` must not perform side effects. |

## Transition Value Rules

| Value | Availability | Mutability |
| --- | --- | --- |
| `new.column` in `before insert` | Available | Mutable where column policy admits assignment. |
| `new.column` in `before update` | Available | Mutable where column policy admits assignment. |
| `new.column` in `after insert`/`after update` | Available | Read-only. |
| `old.column` in `update`/`delete` | Available | Read-only. |
| `old.column` in `insert` | Unavailable | Diagnostic if referenced. |
| `new.column` in `delete` | Unavailable | Diagnostic if referenced. |

Generated columns, identity columns, protected columns, masked fields, and policy-owned fields may reject trigger assignment even when `new` is generally mutable.

## Event Trigger Shape

Event triggers capture database, schema, DDL, security, operational, or profile-admitted events rather than row changes.

```sql
create event trigger app.audit_table_ddl
on ddl_command_end
when tag in ('CREATE TABLE', 'ALTER TABLE', 'DROP TABLE')
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

The generated surface rows include `event_trigger_filter` and `event_trigger_security_clause`; event trigger execution is still an engine-defined event dispatch, not a parser-owned hook.

## Event Context

An event trigger receives a read-only event context descriptor. The exact fields are event-class dependent.

| Field Class | Examples | Contract |
| --- | --- | --- |
| Event identity | event UUID, event class, event tag | Engine-provided and read-only. |
| Object identity | object UUID, object class, schema UUID, object name | UUID is authority; names are display/resolver evidence. |
| Principal/session | principal UUID, session UUID, parser profile, connection UUID | Redacted according to security policy. |
| Transaction | transaction UUID, statement UUID, timestamp | MGA remains finality authority. |
| Command metadata | command tag, operation family, result shape | Text may be redacted or omitted where policy requires. |
| Diagnostics | message-vector fields for failure events | Protected material must not be exposed. |

## Event Capture Rules

| Rule | Behavior |
| --- | --- |
| Capture source | Event payload comes from engine event dispatcher. |
| Read-only payload | Procedural code cannot mutate the event payload. |
| Authorization | Event trigger owner and effective execution mode determine visible fields. |
| Filtering | `WHEN`/filter expressions must be side-effect free and descriptor-bound. |
| Recursion control | Event triggers must have recursion/cascade policy to prevent uncontrolled event loops. |
| Failure behavior | Trigger failure follows event policy: abort source statement, quarantine event, or emit diagnostic where admitted. |

## Security Mode

```sql
alter trigger app.audit_table_ddl set security definer;
```

Trigger security mode controls how privileges are evaluated.

| Mode | Contract |
| --- | --- |
| Invoker | Trigger body executes with the effective user/agent that caused the event, plus explicit trigger metadata privileges. |
| Definer | Trigger body executes with the trigger owner/security definer context, subject to sandboxing and protected-material rules. |
| System/internal | Reserved for engine-owned triggers and must not be exposed through ordinary parser text. |

Even in definer mode, the trigger cannot bypass MGA, recovery fences, protected-material redaction, or cluster/public build gates.

## Trigger Ordering

Multiple triggers on the same event require deterministic ordering metadata.

```sql
alter trigger app.orders_validate_bu set order before app.orders_audit_bu;
```

Where explicit order is not provided, the engine must use a stable catalog order or refuse ambiguous ordering when semantics require determinism.

## Recursion And Cascades

Triggers may cause statements that fire other triggers. The engine must enforce recursion and cascade policy.

| Policy Concern | Required Behavior |
| --- | --- |
| Maximum depth | Enforce configured recursion depth. |
| Self-recursion | Refuse or require explicit admission. |
| Mutating same relation | Enforce donor/profile/SBsql policy. |
| Event trigger loops | Prevent infinite event capture loops. |
| Diagnostics | Emit canonical message vectors for recursion refusal. |

## Example: Audit Trigger With Handler

```sql
create trigger app.orders_au
after update on app.orders
for each row
when (old.order_total is distinct from new.order_total)
as
begin
  insert into app.order_audit(order_id, old_total, new_total)
  values (new.order_id, old.order_total, new.order_total);

exception
  when any do
  begin
    signal sqlstate '45000'
      set message_text = 'order audit trigger failed';
  end
end;
```

The handler does not make the base update committed. The update, trigger insert, and raised diagnostic remain under the owning transaction.

## Proof Expectations

Trigger and event implementation should prove:

- event/timing/scope metadata is stored by UUID;
- original source and executable body are both retained;
- `old`, `new`, and `event` descriptors are engine-provided;
- unavailable transition values fail closed;
- `WHEN` filters are side-effect free and descriptor-bound;
- security mode is enforced;
- recursion policy is enforced;
- generated/identity/protected fields cannot be improperly assigned;
- trigger failure behavior is deterministic;
- event payloads do not leak secrets;
- event trigger execution cannot bypass public cluster gates or private provider boundaries.

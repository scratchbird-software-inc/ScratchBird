# Create Event Trigger EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the event-trigger creation production used for database lifecycle, session lifecycle, transaction lifecycle, catalog, security, policy, operational, and diagnostic events.

Related pages: [Trigger Lifecycle](../trigger.md), [Procedural SQL Triggers And Events](../procedural_sql_triggers_and_events.md), [Transaction Control](../transaction_control.md), and [Security And Privileges](../security_and_privilege_statements.md).

## Production

```ebnf
create_event_trigger ::=
    CREATE EVENT TRIGGER qualified_name
    ON event_trigger_event
    event_trigger_filter?
    event_trigger_audit_clause?
    trigger_ordering?
    trigger_security?
    AS routine_body ;
```

```ebnf
event_trigger_filter ::=
    WHEN "(" boolean_expression ")" ;

event_trigger_audit_clause ::=
    AUDIT audit_destination? audit_payload_policy? ;

trigger_security ::=
    SECURITY INVOKER
  | SECURITY DEFINER ;
```

## Meaning

`CREATE EVENT TRIGGER` creates a durable trigger object whose firing source is an engine event rather than a row operation. The trigger is catalog-owned by UUID, and its event binding, filter descriptor, security mode, ordering metadata, source text, executable binding, and dependencies are stored as metadata.

The event payload is engine-provided and read-only. Event triggers do not receive `old` or `new` row descriptors unless the event class explicitly defines a transition descriptor, which database/session/transaction lifecycle events do not.

## Event Classes

| Event Class | Examples |
| --- | --- |
| Database lifecycle | `DATABASE CONNECT`, `DATABASE DISCONNECT`, `DATABASE ATTACH`, `DATABASE DETACH`, `DATABASE OPEN`, `DATABASE CLOSE`, recovery state changes |
| Session lifecycle | `SESSION CONNECT`, `SESSION AUTHENTICATE`, `SESSION AUTHORIZATION CHANGE`, `SESSION DISCONNECT`, `SESSION CANCEL`, `SESSION TIMEOUT` |
| Transaction lifecycle | `TRANSACTION START`, `TRANSACTION PREPARE`, `TRANSACTION COMMIT`, `TRANSACTION ROLLBACK`, savepoint lifecycle |
| Catalog and DDL | Catalog mutation and DDL command lifecycle events |
| Security and policy | User, role, grant, revoke, policy, mask, and authorization lifecycle events |
| Operational and diagnostic | Admitted runtime, diagnostic, refusal, and supportability events |

## Execution Contract

| Concern | Contract |
| --- | --- |
| Authorization | The creator must have trigger creation authority and event-class authority. |
| Filtering | `WHEN` filters must be boolean, side-effect free, and descriptor-bound. |
| Security mode | Invoker and definer execution remain subject to sandboxing, row policy, protected-material redaction, and provider admission gates. |
| Transaction authority | Transaction lifecycle triggers cannot create commit or rollback finality. Finality remains engine-owned. |
| Connection admission | Connection/session admission events may deny before the session becomes usable. |
| Terminal events | Disconnect, commit-end, rollback-end, timeout, and shutdown terminal events are observation/cleanup events unless policy admits an event-owned action. |
| Recovery | Recovery-required events must fail closed unless recovery policy explicitly admits user trigger execution. |

## Example

```sql
create event trigger app.transaction_commit_audit
on transaction commit
when event.database_uuid = current_database_uuid()
security definer
as
begin
  insert into app.transaction_audit(
    event_name,
    transaction_uuid,
    phase,
    occurred_at
  )
  values (
    event.name,
    event.transaction_uuid,
    event.phase,
    current_timestamp
  );
end;
```

The example records the lifecycle event visible to the trigger. It does not make the transaction committed; the engine transaction inventory remains the finality authority.

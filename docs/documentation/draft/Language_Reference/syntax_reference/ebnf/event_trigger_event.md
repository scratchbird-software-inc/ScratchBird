# Event Trigger Event EBNF Production

This page is part of the SBsql Language Reference Manual. It expands the `event_trigger_event` production used by `CREATE EVENT TRIGGER`.

Related pages: [Create Event Trigger](create_event_trigger.md), [Trigger Lifecycle](../trigger.md), [Procedural SQL Triggers And Events](../procedural_sql_triggers_and_events.md), and [Transaction Control](../transaction_control.md).

## Production

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
```

```ebnf
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

## Semantics

An event trigger event name selects an engine event class and phase. The event name is not executable authority by itself. The statement must still bind to an event descriptor, pass event-class authorization, pass trigger security checks, and lower to an admitted SBLR route.

Database, session, and transaction lifecycle events expose an `event` descriptor. The descriptor is read-only and may include:

| Field Class | Examples |
| --- | --- |
| Event identity | event UUID, event name, event class, event phase |
| Database identity | database UUID, database alias where visible, lifecycle state |
| Session identity | session UUID, connection UUID, principal UUID, authorization epoch |
| Transaction identity | transaction UUID, isolation descriptor, read/write mode, prepare name where visible |
| Diagnostics | failure code, refusal class, recovery fence, redacted diagnostic reference |

## Phase Rules

| Event | Phase Rule |
| --- | --- |
| `DATABASE CONNECT` / `SESSION CONNECT` | Admission phase. The trigger may deny before a usable session is exposed. |
| `DATABASE DISCONNECT` / `SESSION DISCONNECT` | Terminal phase. The trigger is observation/cleanup and must not block resource release indefinitely. |
| `TRANSACTION START` | Admission phase. The trigger may deny before ordinary statement execution in that transaction. |
| `TRANSACTION PREPARE` | Pre-finality phase. The trigger may deny before prepared state is recorded. |
| `TRANSACTION COMMIT` / `TRANSACTION COMMIT START` | Pre-finality phase. The trigger may deny before commit finality. |
| `TRANSACTION COMMIT END` | Post-finality phase. The trigger cannot undo commit finality. |
| `TRANSACTION ROLLBACK` / `TRANSACTION ROLLBACK START` | Rollback phase. The trigger may run cleanup policy before rollback finality where admitted. |
| `TRANSACTION ROLLBACK END` | Post-rollback phase. The trigger cannot restore rolled-back work. |
| `TRANSACTION COMMIT FAILED` / `TRANSACTION ROLLBACK FAILED` | Failure phase. The trigger can observe and route diagnostics according to policy. |

## Example

```sql
create event trigger app.database_disconnect_audit
on database disconnect
as
begin
  insert into app.connection_audit(event_name, session_uuid, reason_code, occurred_at)
  values (event.name, event.session_uuid, event.reason_code, current_timestamp);
end;
```

The disconnect event is terminal. If the trigger exceeds the allowed deadline or cannot write its audit row, the engine applies the event failure policy and continues required cleanup.

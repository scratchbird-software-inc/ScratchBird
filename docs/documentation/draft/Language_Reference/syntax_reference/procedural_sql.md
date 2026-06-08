# Procedural SQL

This page is part of the SBsql Language Reference Manual. It is the entry point for the SBsql procedural block language.

## Purpose

Procedural SQL is the block language used inside procedures, functions, triggers, execute blocks, migration helpers, and SBsql-session routine bodies. It lets a routine declare variables, run SQL statements, branch, loop, use cursors, raise diagnostics, handle exceptions, return values, and capture trigger/event context.

Procedural SQL text is not runtime authority. The parser accepts the body, the binder resolves names to UUIDs and descriptors, the body is encoded into executable SBLR or trusted UDR metadata, and the original source text is retained as reference text for editing, migration, auditing, and metadata rendering.

## Reference Structure

| Topic | File |
| --- | --- |
| Blocks, declarations, variables, parameters, assignment, and executable-body storage | [procedural_sql_blocks.md](procedural_sql_blocks.md) |
| Control flow, conditional logic, loops, returns, row emission, and dynamic execution boundaries | [procedural_sql_control_flow.md](procedural_sql_control_flow.md) |
| Cursors, row streams, positioned operations, and cursor metadata | [procedural_sql_cursors.md](procedural_sql_cursors.md) |
| Exceptions, diagnostics, conditions, `SIGNAL`, `RESIGNAL`, and handler behavior | [procedural_sql_exceptions.md](procedural_sql_exceptions.md) |
| Table triggers, event triggers, transition values, `WHEN` filters, event capture, and trigger execution context | [procedural_sql_triggers_and_events.md](procedural_sql_triggers_and_events.md) |

## Related Lifecycle Pages

| Object | Lifecycle Reference |
| --- | --- |
| Procedures | [procedure.md](procedure.md) |
| Functions | [function.md](function.md) |
| Triggers | [trigger.md](trigger.md) |
| Transactions, savepoints, execute blocks, autonomous blocks | [transaction_control.md](transaction_control.md) |
| Security and privilege statements | [security_and_privilege_statements.md](security_and_privilege_statements.md) |
| Policy, masking, and RLS interaction | [policy_mask_and_rls.md](policy_mask_and_rls.md) |

## Related Functional Pages

| Area | Reference |
| --- | --- |
| Cursor runtime functions | [../functional_reference/sb_cursor.md](../functional_reference/sb_cursor.md) |
| Procedural diagnostics and context functions | [../functional_reference/sb_diagnostic.md](../functional_reference/sb_diagnostic.md) |
| Core scalar functions including procedural diagnostic surfaces | [../functional_reference/sb_core.md](../functional_reference/sb_core.md) |
| Operators and result descriptors used in procedural expressions | [operators.md](operators.md) and [operator_type_result_matrix.md](operator_type_result_matrix.md) |
| Type descriptors and conversion rules | [../data_types/type_system_overview.md](../data_types/type_system_overview.md) and [../data_types/conversion_matrix.md](../data_types/conversion_matrix.md) |

## Authority Model

| Concern | Authority |
| --- | --- |
| Routine identity | UUID catalog row, not routine name text. |
| Parameter and variable type | Descriptor UUID and domain metadata. |
| SQL statement execution | SBLR operation admitted by server and engine. |
| Transaction finality | MGA transaction inventory. |
| Object visibility | MGA snapshot and materialized authorization. |
| Trigger firing | Engine event dispatcher and trigger catalog metadata. |
| Event capture | Engine-provided event context descriptor. |
| Diagnostics | Canonical message-vector records. |
| Stored body | Encoded SBLR or trusted UDR binding plus original reference source text. |

## Top-Level Routine Forms

### Procedure

```sql
create procedure app.close_order(p_order_id uuid)
returns (closed_order_id uuid)
as
begin
  update app.orders
     set order_state = 'closed'
   where order_id = p_order_id
  returning order_id into closed_order_id;

  suspend;
end;
```

A procedure may emit zero or more result rows. A procedure with a `returns (...)` list exposes output descriptors as part of its catalog identity.

### Function

```sql
create function app.tax_amount(amount decimal(18,2), rate decimal(9,6))
returns decimal(18,2)
as
begin
  return amount * rate;
end;
```

A function returns a single scalar, row, or admitted structured descriptor. Functions that are marked deterministic, immutable, stable, or volatile must match their implementation behavior.

### Execute Block

```sql
execute block (p_customer_id uuid = :customer_id)
returns (order_count int64)
as
begin
  select count(*)
    from app.orders
   where customer_id = p_customer_id
    into order_count;

  suspend;
end;
```

`EXECUTE BLOCK` is an anonymous procedural routine. It is parsed, bound, admitted, and executed in the caller's session and transaction context unless an explicit autonomous-block form is admitted.

### Trigger

```sql
create trigger app.orders_bi
before insert on app.orders
for each row
when (new.order_id is null)
as
begin
  new.order_id = gen_uuid_v7();
end;
```

Triggers run inside engine-defined table or event contexts. They can read transition values supplied by the engine, but parser text cannot invent storage authority.

## Supported Statement Classes

| Class | Examples | Notes |
| --- | --- | --- |
| Declarations | variables, cursors, conditions, local routine helpers where admitted | See [procedural_sql_blocks.md](procedural_sql_blocks.md). |
| Assignment | `v_total = amount * rate;`, `select ... into ...` | Assignment requires descriptor-compatible values. |
| DML | `select`, `insert`, `update`, `delete`, `merge`, `upsert` | Executes through ordinary SBLR DML routes. |
| Transaction-local control | savepoint, release, rollback to savepoint | Top-level commit/rollback is restricted by routine context and policy. |
| Conditional control | `if`, searched/simple `case` | Conditions use boolean descriptors and SQL null rules. |
| Loops | `while`, `repeat`, `loop`, `for select`, cursor loops | Loop control is encoded into procedural SBLR. |
| Cursor control | declare, open, fetch, close, positioned operations where admitted | See [procedural_sql_cursors.md](procedural_sql_cursors.md). |
| Diagnostics | `signal`, `resignal`, exception handlers, message vectors | See [procedural_sql_exceptions.md](procedural_sql_exceptions.md). |
| Trigger/event control | `old`, `new`, transition rows, event payload, `when` filter | See [procedural_sql_triggers_and_events.md](procedural_sql_triggers_and_events.md). |

## SBsql Procedural Boundary

SBsql procedural bodies must use the SBsql procedural grammar admitted by the active session policy. Any accepted procedural form must lower into this authority model:

- preserve the accepted SBsql source as original reference text;
- bind executable behavior to UUIDs and descriptors;
- store executable SBLR or trusted UDR metadata;
- preserve SBsql-defined null, cursor, exception, trigger, and result-row rules through session policy;
- refuse unsupported SBsql procedural constructs with a canonical message vector.

SBsql procedural SQL is the common ScratchBird model. It is not a license to mix unrelated syntax families inside one routine unless a session policy explicitly owns that syntax.

## Implementation And Verification Expectations

Every procedural routine should be testable through:

- parse and bind proof;
- SBLR encode proof;
- catalog identity and descriptor proof;
- transaction and rollback proof;
- trigger/event context proof where applicable;
- diagnostic/message-vector proof;
- original-source retention proof;
- executable-body re-open proof;
- SBsql lowering proof where the routine came from a parser route.

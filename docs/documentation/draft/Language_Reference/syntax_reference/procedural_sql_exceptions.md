# Procedural SQL Exceptions And Diagnostics

This page documents procedural conditions, exception handlers, diagnostics, `SIGNAL`, `RESIGNAL`, and message-vector behavior.

Related pages: [procedural_sql.md](procedural_sql.md), [procedural_sql_blocks.md](procedural_sql_blocks.md), [../functional_reference/sb_diagnostic.md](../functional_reference/sb_diagnostic.md), [../functional_reference/sb_core.md](../functional_reference/sb_core.md), [refusal_vectors.md](refusal_vectors.md).

## Diagnostic Model

ScratchBird uses canonical message-vector diagnostics. Procedural SQL can name, raise, handle, inspect, and rethrow diagnostics, but it cannot turn diagnostics into storage authority or suppress recovery-required states.

| Concept | Contract |
| --- | --- |
| Condition | Named diagnostic class or SQLSTATE-like code. |
| Exception | Runtime diagnostic raised by a statement, expression, engine operation, policy check, or explicit procedural statement. |
| Handler | Block-local recovery path for selected diagnostics. |
| Message vector | Canonical structured diagnostic output. |
| SQLSTATE context | Profile-compatible condition code where admitted. |
| Refusal | Fail-closed diagnostic for unsupported, denied, unlicensed, unsafe, or unavailable functionality. |

## Conditions

```sql
declare condition duplicate_order sqlstate '23505';
declare condition no_customer sqlstate '02000';
```

Conditions are local names for diagnostic matching. The condition name is resolver input; the diagnostic code/message-vector class is authority.

## Handlers

```sql
begin
  insert into app.orders(order_id, customer_id)
  values (p_order_id, p_customer_id);

exception
  when sqlstate '23505' do
  begin
    signal sqlstate '45000'
      set message_text = 'order already exists';
  end
end
```

| Handler Form | Purpose |
| --- | --- |
| `when sqlstate '<code>' do ...` | Handles one SQLSTATE-like condition. |
| `when condition_name do ...` | Handles a named declared condition. |
| `when any do ...` | Handles all diagnostics admitted by the profile. |
| `when not_found do ...` | Handles cursor/query no-row conditions where profile exposes a not-found condition. |
| `when constraint_violation do ...` | Handles constraint class diagnostics where profile exposes class names. |

The SBsql defines which condition names are accepted. The engine still emits canonical message-vector records.

## Handler Scope

```sql
begin
  begin
    insert into app.customer(customer_id) values (p_customer_id);
  exception
    when sqlstate '23505' do
    begin
      v_customer_already_exists = true;
    end
  end;

  if v_customer_already_exists then
    execute procedure app.audit_duplicate_customer(p_customer_id);
  end if;
end
```

Handlers are scoped to their block. If a diagnostic is not handled in the current block, it propagates to the nearest enclosing block with a matching handler. If no handler accepts it, the routine exits with the diagnostic.

## SIGNAL

```sql
signal sqlstate '45000'
  set message_text = 'invalid order state',
      detail = 'closed orders cannot be reopened by this routine';
```

`SIGNAL` raises a new procedural diagnostic. The generated functional reference exposes a procedural signal surface in [../functional_reference/sb_core.md](../functional_reference/sb_core.md).

| Field | Contract |
| --- | --- |
| SQLSTATE/code | Profile-compatible code or ScratchBird diagnostic class. |
| Message text | User-facing diagnostic text, redacted where policy requires. |
| Detail | Additional diagnostic detail. |
| Hint | Optional user guidance. |
| Object identity | UUID references where relevant and authorized. |
| Protected material | Must not be exposed. |

## RESIGNAL

```sql
exception
  when any do
  begin
    insert into app.error_audit(error_text)
    values (current_diagnostic_text());

    resignal;
  end
end
```

`RESIGNAL` rethrows the current diagnostic, optionally with additional message-vector fields where admitted. The functional reference exposes a procedural resignal surface in [../functional_reference/sb_core.md](../functional_reference/sb_core.md).

## Diagnostic Inspection

Procedural code can inspect admitted diagnostic context through diagnostic functions. The exact function names are profile and package dependent, but the model is:

```sql
exception
  when any do
  begin
    v_state = current_sqlstate();
    v_text = current_diagnostic_text();
    insert into app.error_audit(sqlstate_code, diagnostic_text)
    values (v_state, v_text);
    resignal;
  end
end
```

Diagnostic inspection must be read-only. It must not expose protected material, secret values, or unauthorized catalog details.

## Not Found

`SELECT ... INTO`, cursor fetch, and positioned operations may produce not-found conditions.

```sql
begin
  select customer_id
    from app.customer
   where customer_id = p_customer_id
    into v_customer_id;

exception
  when not_found do
  begin
    signal sqlstate '02000'
      set message_text = 'customer was not found';
  end
end
```

Profiles differ on whether no-row assignment yields nulls, a not-found condition, or a diagnostic. The SBsql must preserve SBsql behavior while mapping the condition to canonical message-vector output.

## Case Not Found

A searched or simple `CASE` without an `ELSE` may raise a case-not-found diagnostic where the profile requires it.

```sql
case v_state
  when 'open' then
    v_action = 'close';
  when 'closed' then
    v_action = 'archive';
end case;
```

The generated functional reference exposes a case-not-found surface in [../functional_reference/sb_core.md](../functional_reference/sb_core.md).

## Transaction Interaction

Handlers execute in the same transaction context unless the code is inside an admitted autonomous block.

| Situation | Behavior |
| --- | --- |
| Statement fails inside ordinary block | Effects of the failing statement are undone according to statement atomicity. Prior successful statements remain part of the active transaction unless rolled back. |
| Handler runs | Handler statements execute in the same transaction context. |
| Handler signals/resignals | Diagnostic propagates; transaction finality remains with MGA. |
| Savepoint used before risky statement | Handler may roll back to the savepoint if policy admits it. |
| Autonomous block | Has its own transaction context and recovery evidence. |

## Refusal Diagnostics

Procedural SQL must fail closed for:

- unsupported condition or handler form;
- handler that attempts unauthorized object access;
- diagnostic text that would leak protected material;
- `SIGNAL`/`RESIGNAL` outside a valid procedural context;
- invalid SQLSTATE or diagnostic class;
- suppressed recovery-required state;
- attempt to convert fail-closed refusal into success.

## Example: Savepoint And Handler

```sql
begin
  savepoint before_insert;

  insert into app.orders(order_id, customer_id)
  values (p_order_id, p_customer_id);

exception
  when sqlstate '23505' do
  begin
    rollback to savepoint before_insert;
    signal sqlstate '45000'
      set message_text = 'duplicate order';
  end
end
```

## Proof Expectations

Exception handling should prove:

- handler matching is deterministic;
- diagnostics are canonical message vectors;
- protected material is redacted;
- `SIGNAL` and `RESIGNAL` preserve SQLSTATE/profile compatibility;
- savepoint interaction is MGA-owned;
- unhandled diagnostics propagate;
- fail-closed diagnostics cannot be swallowed into unsafe success.

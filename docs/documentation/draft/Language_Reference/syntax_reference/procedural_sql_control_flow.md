# Procedural SQL Control Flow

This page documents conditional execution, loops, return behavior, row emission, dynamic execution boundaries, and transaction restrictions inside procedural SQL.

Related pages: [procedural_sql.md](procedural_sql.md), [procedural_sql_blocks.md](procedural_sql_blocks.md), [operators.md](operators.md), [transaction_control.md](transaction_control.md).

## Statement Model

Procedural control-flow statements are encoded operations. They are not runtime text branches. Each condition, expression, query, and nested statement must bind to descriptors, UUIDs, authorization context, and an admitted SBLR route.

```ebnf
procedural_statement    ::= assignment_stmt
                          | dml
                          | if_stmt
                          | case_stmt
                          | loop_stmt
                          | cursor_stmt
                          | return_stmt
                          | suspend_stmt
                          | signal_stmt
                          | nested_block ;
```

## IF

```sql
if v_total > 1000 then
begin
  v_status = 'large';
end
else
begin
  v_status = 'standard';
end
```

| Rule | Behavior |
| --- | --- |
| Condition descriptor | Must be boolean or explicitly castable to boolean by the SBsql. |
| SQL null | A null condition is treated as not true. Portable code should write explicit `is true`, `is false`, or `is unknown` where needed. |
| Branch binding | Both branches are parsed and bound before execution. |
| Authorization | Statements inside both branches must be admissible under routine security policy. |

## Searched CASE

```sql
case
  when v_total is null then
    v_status = 'missing';
  when v_total >= 1000 then
    v_status = 'large';
  else
    v_status = 'standard';
end case;
```

A searched `CASE` evaluates boolean conditions in order. If no branch matches and no `ELSE` exists, the routine may raise a case-not-found diagnostic where the SBsql requires it. The functional reference exposes a procedural case-not-found surface in [../functional_reference/sb_core.md](../functional_reference/sb_core.md).

## Simple CASE

```sql
case v_order_state
  when 'open' then
    v_action = 'close';
  when 'closed' then
    v_action = 'archive';
  else
    v_action = 'inspect';
end case;
```

Simple `CASE` compares the selector expression to each branch value using descriptor-aware equality. Text comparisons use collation descriptors.

## WHILE

```sql
while v_attempts < 3 do
begin
  v_attempts = v_attempts + 1;
  execute procedure app.try_send_notice(:p_order_id);
end
```

The condition is evaluated before each iteration. A null condition is not true unless a SBsql policy explicitly defines a different rule.

## REPEAT

```sql
repeat
  v_attempts = v_attempts + 1;
  execute procedure app.try_send_notice(:p_order_id);
until v_attempts >= 3
end repeat;
```

`REPEAT` runs the body before evaluating the condition. Generated surface rows identify `psql_repeat_stmt`; SBsql policies may render equivalent syntax differently.

## LOOP

```sql
loop
  if v_done then
    leave;
  end if;

  v_count = v_count + 1;
end loop;
```

An unconditional loop must contain an exit path such as `leave`, `exit`, `return`, a raised diagnostic, or a profile-admitted bounded form. Static analysis should warn or refuse loops with no possible exit where policy requires proof.

## FOR SELECT

```sql
for
  select order_id, order_total
    from app.orders
   where customer_id = p_customer_id
    into v_order_id, v_order_total
do
begin
  execute procedure app.audit_order(v_order_id, v_order_total);
end
```

`FOR SELECT` binds the query once and fetches rows through an engine-owned cursor/row stream. Output fields are assigned by ordinal to target descriptors for each iteration.

## FOR Counter

```sql
for v_i = 1 to 10 do
begin
  insert into app.sequence_audit(n_value) values (v_i);
end
```

Counter loops are descriptor-bound integer loops. Step, bounds, overflow, and direction are policy controlled. Use explicit integer descriptors when portability matters.

## Cursor FOR

```sql
for cursor c_orders do
begin
  fetch c_orders into v_order_id, v_order_total;
  if row_not_found() then
    leave;
  end if;
end
```

Cursor loops are detailed in [procedural_sql_cursors.md](procedural_sql_cursors.md).

## EXIT, LEAVE, CONTINUE

| Statement | Contract |
| --- | --- |
| `leave` | Exits the innermost loop or the named loop where profile admits labels. |
| `exit` | Alias or SBsql-specific exit form. |
| `continue` | Skips to the next loop iteration where admitted. |
| labeled exit | Exits the named block or loop if the SBsql supports labels. |

Labels are resolver input only. They do not create durable identity.

## RETURN

```sql
return amount * rate;
```

`RETURN` completes a function or exits a procedure/trigger block where the context admits it.

| Context | Behavior |
| --- | --- |
| Scalar function | Expression must assign to the function return descriptor. |
| Procedure | Exits the procedure without emitting an additional row. |
| Selectable procedure | Does not emit a row unless a prior `suspend` did so. |
| Trigger | Ends the trigger body. Transition-row changes already made remain part of the trigger context unless rolled back. |
| Execute block | Ends the anonymous block. |

## SUSPEND

```sql
suspend;
```

`SUSPEND` emits one result row from the current output variables in a selectable procedure or execute block with a `returns` list.

| Rule | Behavior |
| --- | --- |
| Output descriptors | Current output fields must match the declared result descriptor. |
| Transaction | Emitted rows are visible to the caller as routine output, not as committed durable state. |
| DML inside body | DML remains governed by the active transaction. |
| Non-selectable context | `SUSPEND` is refused where the routine has no row-emitting result descriptor. |

## Dynamic Execution

```sql
execute statement :sql_text
  into v_result;
```

Dynamic execution is policy-sensitive. If admitted, the dynamic statement is parsed and lowered through the normal SBsql or SBsql-session pipeline. It cannot bypass:

- SBsql session policy admission;
- UUID name resolution;
- descriptor checks;
- materialized authorization;
- SBLR admission;
- MGA transaction authority.

Server-local file access, low-level repair/verify behavior, or SBsql physical-page operations must be refused unless an SBsql-only administrative policy explicitly admits the operation.

## Transaction Control In Routines

Procedural SQL can use savepoints where admitted:

```sql
savepoint before_detail;
insert into app.detail_log(log_text) values ('detail started');
rollback to savepoint before_detail;
```

Top-level `commit` and `rollback` inside stored routines are restricted. MGA remains final authority. Autonomous blocks are special transaction contexts and are documented with [transaction_control.md](transaction_control.md).

| Operation | Routine Rule |
| --- | --- |
| `savepoint` | Admitted inside an active transaction when policy allows. |
| `rollback to savepoint` | Admitted for local undo inside current transaction. |
| `release savepoint` | Admitted for local savepoint cleanup. |
| `commit` | Refused in ordinary stored routines unless a policy admits an autonomous context. |
| `rollback` | Refused in ordinary stored routines unless a policy admits an autonomous context. |
| autonomous block | Creates a separate transaction context with explicit policy and recovery evidence. |

## Diagnostics And Proof

Control-flow implementation should prove:

- every condition is descriptor-bound;
- branch bodies are bound before execution;
- loops have explicit execution and cancellation behavior;
- row-emitting routines return the declared descriptor;
- dynamic statements are re-parsed and re-authorized;
- transaction-control statements cannot claim finality outside MGA;
- routine source text is retained but not executed as authority.

# Procedural SQL Cursors

This page documents cursor declarations, cursor lifecycle, row fetching, positioned operations, and cursor diagnostics in procedural SQL.

Related pages: [procedural_sql.md](procedural_sql.md), [procedural_sql_blocks.md](procedural_sql_blocks.md), [procedural_sql_control_flow.md](procedural_sql_control_flow.md), [../functional_reference/sb_cursor.md](../functional_reference/sb_cursor.md).

## Cursor Model

A cursor is a named row stream bound to a query, rowset, table value, or engine cursor handle. It does not own transaction finality or visibility. Rows fetched from a cursor remain governed by the statement snapshot, transaction context, materialized authorization, and cursor descriptor.

## Declaration

```sql
declare cursor c_orders for
  select order_id, order_total
    from app.orders
   where customer_id = :p_customer_id
   order by submitted_at;
```

| Declaration Field | Contract |
| --- | --- |
| Cursor name | Local resolver symbol inside the block. |
| Query | Parsed, bound, and lowered to SBLR. |
| Parameters | Bound from routine variables or parameters. |
| Row descriptor | Derived from query projection descriptors. |
| Scrollability | Forward-only by default unless descriptor/profile admits scrollable behavior. |
| Holdability | Transaction-scoped by default unless descriptor/profile admits holdable behavior. |
| Sensitivity | Cursor sees rows according to its snapshot and profile policy. |

## Lifecycle

```sql
open c_orders;

fetch c_orders into v_order_id, v_order_total;

close c_orders;
```

| Operation | Contract |
| --- | --- |
| Declare | Creates a local cursor descriptor, not an open stream. |
| Open | Binds parameters, starts the query, and creates an engine cursor handle. |
| Fetch | Reads the next row or requested position into descriptor-compatible targets. |
| Close | Releases cursor resources. |
| End of data | Produces a not-found condition or cursor state that procedural code can test. |

## Cursor FOR Loop

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

The query is cursor-backed even when no cursor name is declared. The engine owns row production and visibility.

## Explicit Cursor Loop

```sql
declare cursor c_orders for
  select order_id, order_total
    from app.orders
   where customer_id = p_customer_id;

begin
  open c_orders;

  loop
    fetch c_orders into v_order_id, v_order_total;
    if row_not_found() then
      leave;
    end if;

    execute procedure app.audit_order(v_order_id, v_order_total);
  end loop;

  close c_orders;
end
```

`row_not_found()` is illustrative of a handler/context function. The exact name can be profile-specific; the operation must read procedural diagnostic context rather than inspect raw cursor state directly.

## Fetch Targets

```sql
fetch c_orders into v_order_id, v_order_total;
```

| Rule | Behavior |
| --- | --- |
| Ordinal matching | Projection field 1 assigns to target 1, and so on. |
| Descriptor matching | Each fetched value must assign to the target descriptor. |
| Null policy | A null fetched into a `not null` target raises a diagnostic. |
| Too few/many targets | Diagnostic unless a profile admits a structured target. |
| Structured target | Row variables or record variables may be admitted by profile. |

## Cursor Metadata Functions

The generated functional reference exposes cursor-related functions in [../functional_reference/sb_cursor.md](../functional_reference/sb_cursor.md). Procedural SQL may use these functions where the cursor handle is visible and policy admits context reads.

| Function Area | Use |
| --- | --- |
| `cursor_open` / `cursor_close` | Runtime open/close helper surfaces. |
| `cursor_active` | Determines whether a cursor is active. |
| `cursor_state` | Reads cursor state for diagnostics. |
| `cursor_position` | Reads current position when the cursor profile supports position. |
| `cursor_holdability` | Reports holdability. |
| `cursor_scrollability` | Reports scrollability. |
| `current_row_locator` | Reads current row locator where positioned operations are admitted. |
| `rowset_to_cursor` / `cursor_to_rowset` | Converts between rowset and cursor abstractions where admitted. |

These functions are metadata and runtime helper surfaces. They do not grant storage, visibility, or transaction authority.

## Positioned UPDATE And DELETE

```sql
update app.orders
   set reviewed = true
 where current of c_orders;

delete from app.orders
 where current of c_orders;
```

Positioned operations are admitted only when:

- the cursor is open;
- the cursor has a current row;
- the current row locator is valid for the target relation;
- the target row is still visible and writable under MGA;
- the effective user or agent has write privileges;
- no profile or policy blocks positioned mutation.

An index or cursor row locator is candidate evidence. The engine must recheck row identity, visibility, security, and write conflict status.

## Scrollable Cursors

```sql
fetch prior from c_orders into v_order_id, v_order_total;
fetch absolute 10 from c_orders into v_order_id, v_order_total;
```

Scrollable fetch forms are profile and descriptor dependent. A cursor that is not declared or opened as scrollable must refuse prior, absolute, relative, and random-position fetches.

## Holdable Cursors

A holdable cursor can survive a transaction boundary only where policy admits it. Ordinary procedural cursors are transaction-scoped. A cursor that crosses transaction boundaries must carry explicit descriptor, snapshot, cleanup, and recovery evidence.

## Cursor Diagnostics

| Condition | Required Diagnostic Behavior |
| --- | --- |
| Fetch before open | Cursor-not-open diagnostic. |
| Open already-open cursor | Cursor-state diagnostic or profile-defined reopen behavior. |
| Fetch after close | Cursor-not-open diagnostic. |
| End of cursor | Not-found condition or cursor-state result. |
| Descriptor mismatch | Assignment diagnostic. |
| Positioned operation without current row | Positioned-operation diagnostic. |
| Cursor handle stale after rollback/recovery | Fail-closed diagnostic. |

## Boundaries

- Cursor names are local symbols, not durable identity.
- Cursor handles are not transaction authority.
- Cursor state cannot bypass row visibility or security checks.
- Parser-support UDRs may expose donor cursor behavior, but the engine cursor descriptor owns ScratchBird execution behavior.
- Cursor metadata can appear in support bundles only through authorized, redacted projections.

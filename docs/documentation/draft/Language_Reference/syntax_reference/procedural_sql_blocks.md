# Procedural SQL Blocks

This page documents procedural block structure, declarations, variables, parameters, assignment, and stored body representation.

Related pages: [procedural_sql.md](procedural_sql.md), [procedure.md](procedure.md), [function.md](function.md), [trigger.md](trigger.md), [transaction_control.md](transaction_control.md).

## Block Shape

A procedural body has an optional declaration section and an executable block.

```ebnf
routine_body            ::= declaration_section? block ;
declaration_section     ::= declaration+ ;
block                   ::= "BEGIN" procedural_statement* exception_section? "END" ;
exception_section       ::= "EXCEPTION" exception_handler+ ;
```

The exact SBsql spelling can vary by SBsql session policy. The ScratchBird binding model is the same: declarations create descriptor-bound local symbols, executable statements lower to SBLR operations, and the block is stored as encoded executable metadata plus original reference source.

## Basic Block Example

```sql
create procedure app.reprice_order(p_order_id uuid, p_rate decimal(9,6))
returns (new_total decimal(18,2))
as
declare variable v_subtotal decimal(18,2) default 0;
declare variable v_tax decimal(18,2) default 0;
begin
  select sum(line_total)
    from app.order_line
   where order_id = p_order_id
    into v_subtotal;

  v_tax = v_subtotal * p_rate;
  new_total = v_subtotal + v_tax;

  update app.orders
     set order_total = new_total
   where order_id = p_order_id;

  suspend;
end;
```

## Declaration Kinds

| Declaration | Purpose | Example |
| --- | --- | --- |
| Parameter | Input, output, or input/output routine argument | `p_order_id uuid` |
| Return field | Named output column for selectable procedures | `returns (order_id uuid, status text)` |
| Local variable | Descriptor-bound local storage | `declare variable v_count bigint default 0;` |
| Cursor | Named row stream over a query | `declare cursor c_orders for select ...;` |
| Condition | Named diagnostic condition | `declare condition duplicate_order sqlstate '23505';` |
| Exception handler | Handles a diagnostic class inside a block | `when sqlstate '23505' do begin ... end` |
| Local helper routine | Nested routine where admitted by profile | `declare function local_tax(...) returns ... as ...` |

Not every SBsql policy admits every declaration kind. If a parser route accepts a SBsql-defined declaration, it must lower it to descriptor-aware ScratchBird metadata or fail closed.

## Parameters

Routine parameters are part of the routine signature. Parameter names are resolver input; parameter descriptors are authority.

```sql
create function app.discounted_amount(
  amount decimal(18,2),
  discount_rate decimal(9,6) default 0
)
returns decimal(18,2)
as
begin
  return amount - (amount * discount_rate);
end;
```

| Parameter Attribute | Contract |
| --- | --- |
| Name | Resolver input within the routine body. |
| Mode | `in`, `out`, `inout`, or SBsql-specific default. |
| Descriptor | Type, domain, collation, charset, scale, precision, timezone, and null policy. |
| Default | Bound expression evaluated under routine-call rules. |
| Authorization | Caller must be authorized to execute the routine; body execution uses invoker/definer policy. |

## Return Descriptors

Functions have one return descriptor. Selectable procedures have a result row descriptor made from the `returns (...)` list.

```sql
create procedure app.open_orders(p_customer_id uuid)
returns (
  order_id uuid,
  submitted_at timestamptz,
  order_total decimal(18,2)
)
as
begin
  for
    select order_id, submitted_at, order_total
      from app.orders
     where customer_id = p_customer_id
       and order_state = 'open'
      into order_id, submitted_at, order_total
  do
  begin
    suspend;
  end
end;
```

`SUSPEND` emits the current output descriptor values as one result row for selectable procedures or execute blocks with a `returns` list.

## Local Variables

```sql
declare variable v_attempts int default 0;
declare variable v_status varchar(30) not null default 'pending';
declare variable v_started_at timestamptz default current_timestamp;
```

| Rule | Behavior |
| --- | --- |
| Descriptor binding | Variable type is resolved to a descriptor before execution. |
| Default expression | Default expression must bind to the variable descriptor. |
| Nullability | A `not null` variable rejects null assignment. |
| Scope | Local to the block where declared and nested blocks under it unless shadowing is admitted by profile. |
| Lifetime | Lives for one routine invocation or execute-block invocation. |
| Storage authority | Local variables do not own durable storage or transaction finality. |

## Assignment

Assignment changes a local, output, or transition value when the context admits mutation.

```sql
v_count = v_count + 1;
new.updated_at = current_timestamp;
out_status = 'closed';
```

Assignment is descriptor-checked. The right side must be assignable to the target descriptor through exact matching, safe implicit conversion, or explicit cast.

## SELECT INTO

`SELECT ... INTO` assigns query output fields to variables or output fields.

```sql
select count(*), coalesce(sum(line_total), 0)
  from app.order_line
 where order_id = p_order_id
  into v_line_count, v_subtotal;
```

| Case | Required Behavior |
| --- | --- |
| One row | Assigns projected fields by ordinal to target descriptors. |
| No row | Follows routine/profile policy: null assignment, not-found diagnostic, or handler-visible condition. |
| More than one row | Diagnostic unless the statement shape explicitly admits multi-row behavior. |
| Descriptor mismatch | Diagnostic before or during execution. |

## Nested Blocks

Nested blocks isolate declarations and handlers.

```sql
begin
  declare variable v_outer bigint default 1;

  begin
    declare variable v_inner bigint default v_outer + 1;
    insert into app.audit_event(event_text) values ('inner=' || cast(v_inner as text));
  end
end;
```

A nested block can install exception handlers for its statements. If a handler consumes an exception, execution continues according to the handler rule. Otherwise the diagnostic propagates to the outer block.

## Dynamic Execution Boundary

Dynamic SQL is a policy-sensitive surface. When admitted, it must parse and bind under the current security, descriptor, and transaction context. Dynamic text cannot bypass UUID resolution or materialized authorization.

```sql
execute statement :dynamic_sql
  into v_result;
```

Dynamic SQL must be refused when:

- the routine context does not admit it;
- the caller lacks authority;
- the statement attempts server-local file access not admitted by policy;
- the statement cannot be parsed and lowered to SBLR;
- the statement tries to acquire parser, SBsql, storage, or recovery authority.

## Stored Body Contract

Every stored procedural object must retain:

| Stored Item | Reason |
| --- | --- |
| Original source text | Editing, compatibility display, migration audit, and SBsql reference. |
| Parser profile | Re-rendering and metadata rendering. |
| Bound dependency graph | Invalidation, privilege checks, recompile, support bundles. |
| Parameter and return descriptors | Call binding, result shape, and driver metadata. |
| Executable representation | Runtime execution, JIT/AOT eligibility, and proof that execution does not depend on raw text. |
| Message-vector behavior | Stable diagnostics. |
| Security mode | Invoker/definer behavior and sandboxing. |

## Boundaries

- A declaration does not create durable catalog state unless it is part of a catalog object definition.
- Local variable names are not UUID identity.
- Routine body text is not engine authority.
- A parser route may preserve SBsql reference source, but execution must use admitted SBLR or trusted UDR binding.
- JIT/AOT can only consume the encoded representation and descriptor graph, not unbound SBsql text.

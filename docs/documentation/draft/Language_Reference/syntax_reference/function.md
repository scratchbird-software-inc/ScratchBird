# Function Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_function_lifecycle`

Related pages: [Procedural SQL](procedural_sql.md), [Procedural Blocks](procedural_sql_blocks.md), [Procedural Control Flow](procedural_sql_control_flow.md), [Procedural Exceptions](procedural_sql_exceptions.md), [Procedures](procedure.md), [Triggers](trigger.md), [Security And Privileges](security_and_privilege_statements.md), [Type System Overview](../data_types/type_system_overview.md), and [Function Lifecycle EBNF](ebnf/function_lifecycle_statement.md).

## Purpose

Functions are executable catalog objects that return a value or rowset descriptor. A function definition includes durable identity, resolver name, overload signature, parameter descriptors, return descriptor, body language, dependency graph, security mode, execution policy, determinism/volatility metadata, original source text, and executable SBLR or trusted UDR binding.

The original SBsql text is retained as reference text for editing, audit, migration, and metadata rendering. Execution must use the encoded representation that was parsed, bound, validated, and admitted by the engine. The parser cannot execute function text as storage or transaction authority.

## Complete Lifecycle Model

1. Define the function with a name, parameter descriptors, return descriptor, body or external binding, security mode, and execution attributes.
2. Parse and bind the function body to UUID catalog identity, descriptors, dependencies, variables, parameters, and routine context.
3. Encode executable behavior into SBLR or a trusted UDR binding.
4. Store the durable function UUID, overload signature, source reference, executable form, dependency graph, grants, and metadata.
5. Make the catalog mutation visible only when the owning transaction commits.
6. Invalidate dependent plans, expression indexes, generated columns, routines, triggers, metadata caches, and support projections.
7. Retire or drop the function only after dependency, privilege, transaction, recovery, and sandbox checks pass.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE FUNCTION` | Creates the durable function UUID, signature descriptors, security mode, dependency graph, original source reference, and executable SBLR or trusted UDR binding. |
| Alter | `ALTER FUNCTION` | Changes admitted metadata such as determinism, volatility, cost, security mode, package binding, JIT/AOT eligibility, or execution policy. |
| Rename | `RENAME FUNCTION ... TO ...` | Changes resolver name only; overload identity, signature descriptors, grants, dependencies, and executable form remain UUID-bound. |
| Comment | `COMMENT ON FUNCTION ... IS ...` | Stores authorized descriptive metadata on the function catalog row. |
| Show | `SHOW FUNCTION ...`, `SHOW FUNCTIONS` | Returns authorized function metadata, overloads, package binding, readiness, and compilation state. |
| Describe | `DESCRIBE FUNCTION ...` | Returns signature descriptors, return type, body language, dependency graph, security mode, and execution binding for one overload or overload set. |
| Recreate | `RECREATE FUNCTION ...` | Replaces the function through a fresh parse, bind, encode, dependency validation, and invalidation route. |
| Drop | `DROP FUNCTION ... [RESTRICT | CASCADE]` | Retires the function only after overload resolution and dependency handling are explicit. |

Function lifecycle is not text storage alone. The executable representation must be complete enough for validation, execution, audit, dependency analysis, and JIT/AOT eligibility where admitted.

## Syntax

```ebnf
function_lifecycle_statement ::=
      create_function
    | alter_function
    | rename_function
    | recreate_function
    | comment_on_function
    | show_function
    | describe_function
    | drop_function ;

create_function ::=
    CREATE FUNCTION function_ref function_parameter_list?
    RETURNS function_return_descriptor
    function_attribute_list?
    AS function_body ;

function_parameter_list ::=
    "(" function_parameter ("," function_parameter)* ")" ;

function_parameter ::=
    parameter_name type_descriptor parameter_default? ;

parameter_default ::=
    DEFAULT expression ;

function_return_descriptor ::=
      type_descriptor
    | TABLE "(" result_column ("," result_column)* ")"
    | ROW "(" result_column ("," result_column)* ")" ;

function_attribute_list ::=
    function_attribute* ;

function_attribute ::=
      DETERMINISTIC
    | NOT DETERMINISTIC
    | IMMUTABLE
    | STABLE
    | VOLATILE
    | RETURNS NULL ON NULL INPUT
    | CALLED ON NULL INPUT
    | SECURITY DEFINER
    | SECURITY INVOKER
    | COST numeric_literal
    | LANGUAGE identifier
    | JIT ELIGIBLE
    | AOT ELIGIBLE
    | EXTERNAL NAME string_literal ;

function_body ::=
      procedural_block
    | RETURN expression
    | EXTERNAL UDR function_udr_binding ;
```

SBsql is context sensitive. Function lifecycle words are command words inside this statement family and should not be treated as globally reserved identifiers outside this context.

## Basic Scalar Function

```sql
create function app.tax_amount(amount decimal(18,2), rate decimal(9,6))
returns decimal(18,2)
deterministic
returns null on null input
as
begin
  return amount * rate;
end;
```

This creates:

- a function UUID;
- resolver name `app.tax_amount`;
- parameter descriptors for `amount` and `rate`;
- return descriptor `decimal(18,2)`;
- deterministic and null-input metadata;
- dependency edges for used descriptors and functions;
- original source reference text;
- encoded executable representation.

## Expression-Body Function

Simple functions may use a single return expression when admitted.

```sql
create function app.extended_price(quantity int32, unit_price decimal(18,2))
returns decimal(18,2)
deterministic
returns null on null input
as return quantity * unit_price;
```

The expression is still parsed, bound, descriptor-checked, and encoded. It is not stored as unbound text for runtime interpretation.

## Table-Valued Function

A table-valued function returns a rowset descriptor and can appear in `FROM` through `TABLE(function_call)`.

```sql
create function app.order_totals(p_customer_id uuid)
returns table (
  order_id uuid,
  total_amount decimal(18,2)
)
stable
security invoker
as
begin
  for select order_id, total_amount
      from app.orders
      where customer_id = p_customer_id
      into order_id, total_amount
  do
  begin
    suspend;
  end
end;
```

Query use:

```sql
select t.order_id, t.total_amount
from table(app.order_totals(:customer_id)) t;
```

The returned table columns are part of the function signature descriptor.

## Parameters And Defaults

Function parameters are input descriptors.

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

| Parameter property | Contract |
| --- | --- |
| Name | Local routine resolver name. |
| Descriptor | Canonical type or domain descriptor. |
| Default | Bound expression used when the caller omits an argument. |
| Null handling | Governed by function null-input attributes and descriptor rules. |
| Dependency | Parameter type/domain dependencies are recorded in the catalog. |

Overload resolution uses function name, argument count, argument descriptors, defaults, and coercion policy. Ambiguous overloads must be refused.

## Function Calls And Overload Resolution

A function call binds in expression, procedural, generated-column, check-constraint, index-expression, table-function, and default-expression contexts only when the selected overload is valid for that context.

```sql
select app.tax_amount(o.total_amount, :tax_rate) as tax_amount
from app.orders o
where app.tax_amount(o.total_amount, :tax_rate) > 0;
```

Overload resolution is descriptor based:

1. resolve the visible function name in the active schema and sandbox scope;
2. collect visible overloads;
3. match argument count, named arguments, and defaulted parameters;
4. prefer exact descriptor matches;
5. apply admitted coercions;
6. reject ambiguous or lossy matches unless policy explicitly admits them;
7. bind the return descriptor into the surrounding expression.

Named arguments improve readability and reduce ambiguity.

```sql
select app.discounted_amount(
  amount => o.total_amount,
  discount_rate => :discount_rate
) as discounted_total
from app.orders o;
```

If a function is used in an index expression, generated column, or persisted computed value, the function must meet the determinism, dependency, and descriptor stability requirements for that use.

## Return Descriptors

| Return form | Meaning |
| --- | --- |
| Scalar descriptor | Function returns one value. |
| Row descriptor | Function returns a structured row value where admitted. |
| Table descriptor | Function returns zero or more rows and can be used as a table function. |
| Cursor descriptor | Function returns an admitted cursor handle descriptor whose lifetime and ownership are governed by cursor policy. |
| Protected descriptor | Function returns protected material by reference or policy-admitted value. |
| Structured descriptor | Function returns JSON/document, vector, spatial, graph, or other admitted descriptors. |

The function body must return values compatible with the declared return descriptor on every successful path. Missing returns must be refused unless the descriptor and body policy define an explicit null return.

## Cursor Arguments

A function may accept a `cursor` descriptor when the call context admits cursor handles.

```sql
create function app.cursor_is_active(p_rows cursor)
returns boolean
volatile
as
begin
  return cursor_active(p_rows);
end;
```

Cursor-aware functions are constrained by the same descriptor and authority rules as procedural cursor statements:

| Concern | Contract |
| --- | --- |
| Argument value | The function receives an engine cursor handle and descriptor, not a materialized result set. |
| State access | Reading cursor state, position, holdability, or metadata is context-sensitive and policy-controlled. |
| Fetching | A function that fetches from a cursor advances the handle and must be declared with compatible volatility. |
| Planner use | Cursor-aware functions are refused in immutable contexts, expression indexes, generated columns, and persisted computed values unless their behavior is proven descriptor-stable and side-effect-free. |
| Return descriptors | Returning a rowset or cursor descriptor must preserve lifetime, ownership, and cleanup rules. |
| Security | A cursor argument does not grant extra object visibility or write authority. |

Cursor conversion helper functions are documented in [../functional_reference/sb_cursor.md](../functional_reference/sb_cursor.md), and procedural rules are documented in [procedural_sql_cursors.md](procedural_sql_cursors.md).

## Determinism, Volatility, And Planner Use

Function attributes tell the binder and optimizer how the function may be used.

| Attribute | Contract |
| --- | --- |
| `DETERMINISTIC` | Same inputs and same admitted context produce the same output. |
| `NOT DETERMINISTIC` | Result may vary even for the same inputs. |
| `IMMUTABLE` | Result depends only on inputs and immutable descriptors. |
| `STABLE` | Result is stable within a statement or transaction according to policy. |
| `VOLATILE` | Result can change between calls and cannot be freely reordered. |
| `RETURNS NULL ON NULL INPUT` | If any input is null, the function returns null without executing the body where admitted. |
| `CALLED ON NULL INPUT` | Body executes even when one or more arguments are null. |
| `COST` | Optimizer hint used for planning, not execution authority. |

Incorrectly declaring a function as deterministic or immutable can corrupt query semantics, expression indexes, generated columns, and cached plans. SBsql must store and expose the declared attribute, and tests should verify behavior for functions used in indexes or generated expressions.

## Security Mode

| Mode | Contract |
| --- | --- |
| `SECURITY INVOKER` | Function executes with the caller's effective privileges and sandbox root. |
| `SECURITY DEFINER` | Function executes with the definer authority admitted by policy, with explicit dependency and audit metadata. |

Security-definer functions require stricter validation. They must not leak protected material, bypass sandbox roots, or expose hidden object details through diagnostics.

## External UDR Binding

Functions can bind to a trusted UDR package where policy admits it.

```sql
create function app.normalize_email(email varchar(320))
returns varchar(320)
deterministic
language udr
external name 'app_text.normalize_email'
as external udr app_text.normalize_email;
```

External binding must record:

- package or library identity;
- function symbol or operation name;
- ABI/profile version;
- parameter and return descriptors;
- determinism and security attributes;
- secret/protected-material policy;
- failure and diagnostic contract;
- upgrade/version pinning where admitted.

The UDR receives typed values and returns typed values. It does not own storage, catalog identity, authorization, or transaction finality.

## Allowed Execution Contexts

| Context | Contract |
| --- | --- |
| Scalar expression | Function returns one descriptor-compatible value. |
| Predicate | Function result must coerce to a boolean predicate descriptor. |
| Projection | Function result descriptor becomes part of the result row descriptor. |
| Default expression | Function must be admitted for default evaluation and transaction context. |
| Generated column | Function must satisfy persistence and determinism requirements. |
| Check constraint | Function must be deterministic or otherwise admitted by constraint policy. |
| Expression index | Function must be deterministic and descriptor-stable. |
| Procedural block | Function executes inside the routine's statement and transaction context. |
| Table expression | Function must return a table descriptor and be invoked through `TABLE(...)`. |
| Cursor-aware routine | Function may read or consume an admitted cursor handle only where volatility, lifetime, and security policy allow it. |

Functions should not be used for uncontrolled side effects. If a statement needs side effects, streaming, external interaction, or data movement, use the statement family or routine type that owns that behavior explicitly.

## Dependencies And Invalidations

Functions can depend on:

- type descriptors and domains;
- collations and character sets;
- other functions, procedures, packages, or UDR bindings;
- tables, views, indexes, generated columns, policies, and triggers referenced by the body;
- protected material policies;
- schema search rules and sandbox roots;
- optimizer attributes such as cost and volatility.

Changing any dependency may invalidate the function or dependent plans.

```sql
alter function app.tax_amount set stable;
describe function app.tax_amount;
```

`DESCRIBE FUNCTION` should show dependency state and readiness where visible.

## JIT And AOT Eligibility

JIT/AOT eligibility is metadata, not a guarantee that compilation will happen.

```sql
alter function app.tax_amount set jit eligible;
```

Eligibility requires:

- complete executable encoding;
- deterministic descriptor behavior where required;
- no unsupported dynamic execution;
- stable dependency graph;
- safe protected-material handling;
- platform and policy admission;
- proof that compiled and interpreted behavior agree.

If any requirement fails, the function can still remain executable through the ordinary admitted route, but JIT/AOT status must report not-ready or refused.

## Lifecycle Examples

```sql
create function app.tax_amount(amount decimal(18,2), rate decimal(9,6))
returns decimal(18,2)
deterministic
as
begin
  return amount * rate;
end;

alter function app.tax_amount set stable;
comment on function app.tax_amount is 'Computes tax from amount and rate';
show function app.tax_amount;
describe function app.tax_amount;
rename function app.tax_amount to tax_amount_v1;
recreate function app.tax_amount_v1(amount decimal(18,2), rate decimal(9,6))
returns decimal(18,2)
deterministic
as return amount * rate;
drop function app.tax_amount_v1 restrict;
```

`RECREATE FUNCTION` is a drop-and-create lifecycle operation with dependency checks. It must fail closed if existing dependencies make replacement unsafe.

## Transaction And Recovery Rules

Function DDL is transactional. A created, altered, renamed, recreated, commented, or dropped function becomes visible only when the owning transaction commits. Rollback restores the prior visible catalog state.

Function execution occurs inside the caller's statement and transaction context unless a policy-admitted autonomous route is explicitly used. Function execution cannot commit or roll back the caller's transaction by itself.

## Diagnostics And Refusals

| Condition | Expected diagnostic class |
| --- | --- |
| Ambiguous overload | Function resolution failure. |
| Parameter descriptor not found | Descriptor resolution failure. |
| Body return type mismatch | Return descriptor mismatch. |
| Missing return path | Function body validation failure. |
| Security-definer policy violation | Authorization or sandbox denied. |
| UDR binding unavailable | UDR unavailable or incompatible. |
| Determinism attribute contradicted by body | Function attribute validation failure. |
| Dependency invalid | Dependency validation failure. |
| Drop blocked by dependents | Dependency violation. |
| Recovery-required state | Operation fenced until recovery action completes. |

## Proof Expectations

The function proof suite should include:

- scalar, row, and table-valued functions;
- expression-body and procedural-body functions;
- defaults, named arguments, exact overloads, coercion overloads, and ambiguous overload refusal;
- null-input behavior for `RETURNS NULL ON NULL INPUT` and `CALLED ON NULL INPUT`;
- security-invoker and security-definer execution with sandbox roots;
- UDR binding success, unavailable binding refusal, and incompatible ABI refusal;
- dependency invalidation after referenced object changes;
- generated-column and expression-index eligibility checks;
- transactional create, alter, recreate, rename, comment, drop, commit, rollback, close, and reopen behavior.

## Boundaries

- User-visible names are resolver input; UUID rows are durable identity.
- The parser cannot create catalog truth by accepting syntax.
- Catalog DDL must be transactionally visible and rollback-safe.
- The executable body is encoded SBLR or trusted UDR metadata plus original reference source.
- UDR functions never own storage, authorization, catalog identity, or transaction finality.
- Support and diagnostic surfaces may inspect the object only through authorized projections.

## Verification Checklist

| Check | Required outcome |
| --- | --- |
| Parse | Function lifecycle statement shape is recognized by SBsql. |
| Bind | Names, UUIDs, descriptors, parameters, return type, attributes, and dependencies resolve. |
| Authorize | Effective user or agent UUID may create, alter, inspect, execute, or drop the function. |
| Encode | Body is encoded into SBLR or trusted UDR metadata. |
| Store | Original reference text, executable form, signature, and dependency graph are cataloged. |
| Commit | Catalog mutation becomes visible only through MGA transaction finality. |
| Execute | Function calls use descriptor-compatible arguments and return values. |
| Invalidate | Dependent caches, metadata, plans, indexes, generated columns, and projections are refreshed or refused. |

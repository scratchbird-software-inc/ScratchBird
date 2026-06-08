# Domains, Casts, And Coercion

This page is part of the SBsql Language Reference Manual. It explains how domains participate in descriptor binding, implicit assignment, explicit casts, operation result types, and validation.

Generation task: `data_types_domains_and_casts`

Related pages: [Domain Lifecycle](../syntax_reference/domain.md), [Type System Overview](type_system_overview.md), [Conversion Matrix](conversion_matrix.md), [Operator Type Result Matrix](../syntax_reference/operator_type_result_matrix.md), [Table Lifecycle](../syntax_reference/table.md), and [sys.catalog.domain_descriptor](../catalog_reference/sys_catalog_domain_descriptor.md).

## Purpose

Domains are catalog objects that wrap descriptors with policy. A descriptor defines the carrier value. A domain defines how that carrier value may be used.

The binder never treats a type spelling as final authority. It resolves the spelling to a descriptor UUID, a domain UUID, or a domain stack. SBLR carries the resolved descriptor and domain metadata needed by the engine to recheck the operation.

## Core Terms

| Term | Meaning |
| --- | --- |
| Carrier descriptor | The canonical type descriptor that owns representation, base comparison, base hashing, collation, charset, timezone, precision, scale, and storage. |
| Domain | A named policy layer over a carrier descriptor or another domain. |
| Domain stack | The ordered chain from the base descriptor through each wrapped domain. |
| Assignment coercion | Conversion used when storing, passing, returning, or assigning a value. It is stricter than display rendering. |
| Explicit cast | A `cast(...)`, `try_cast(...)`, or policy-owned conversion request. |
| Domain preservation | Keeping the domain UUID in the result descriptor after an expression or assignment. |
| Domain erasure | Returning only the base carrier descriptor after a cast or operation. |

## Assignment Pipeline

When an expression is assigned to a domain-bearing target, ScratchBird applies this pipeline:

1. Resolve the target domain and base descriptor.
2. Infer or bind the expression result descriptor.
3. Apply an assignment conversion to the base descriptor if the cast policy admits it.
4. Apply the target domain null policy.
5. Apply parent-domain constraints.
6. Apply target-domain constraints.
7. Apply element validation for compound values.
8. Preserve the target domain UUID in the assigned slot.

This pipeline applies to:

- table column inserts and updates;
- generated columns;
- default expressions;
- routine parameters;
- routine local variables;
- routine return values;
- trigger transition assignments;
- materialized-view population where the result descriptor includes a domain;
- explicit `cast(value as domain)` and admitted equivalent forms.

## Implicit Coercion

Implicit coercion is intentionally conservative.

| From | To | Default Rule |
| --- | --- | --- |
| Exact integer | Wider exact integer | Allowed if the target range contains the value. |
| Exact integer | Decimal | Allowed if precision and scale can represent the value. |
| Decimal | Decimal | Allowed only when precision/scale loss is not silent or policy explicitly admits rounding. |
| Real | Real | Allowed for widening precision. Narrowing requires explicit cast. |
| Text | Text | Allowed when charset, collation, and length policy admit it. Truncation is not silent. |
| Binary | Binary | Allowed when byte-length policy admits it. Truncation is not silent. |
| Temporal | Temporal | Allowed when precision, timezone, and calendar policy admit it. |
| Domain | Its base carrier | Allowed only where the operation requests the carrier and domain erasure is admitted. |
| Base carrier | Domain | Allowed for assignment only after full domain validation. |
| Domain | Related domain | Allowed only by target-domain assignment validation. |
| Structured value | Compound domain | Allowed only when every element descriptor and element policy validates. |
| Any family | Unrelated family | Requires explicit cast or a named conversion function. |

## Explicit Casts

```sql
select cast(:candidate as app.email_text);
select try_cast(:candidate as app.email_text);
```

`cast(value as domain)` performs:

1. value-to-carrier conversion;
2. domain null-policy check;
3. domain constraint checks;
4. element-policy checks where applicable;
5. result descriptor assignment to the target domain.

`try_cast(value as domain)` uses the same conversion and validation path, but returns the failure result defined by the function contract instead of raising the ordinary conversion diagnostic.

## Domain Preservation Rules

| Expression Form | Result Descriptor Rule |
| --- | --- |
| Column reference declared as a domain | Preserves the domain in the column expression descriptor. |
| Parameter or variable declared as a domain | Preserves the domain. |
| `cast(value as domain)` | Returns the target domain if validation succeeds. |
| Assignment to a domain slot | Stores the base carrier value and records the target domain identity in the slot metadata. |
| Arithmetic on numeric domains | Usually returns the computed carrier descriptor unless an operation policy preserves a domain. |
| Concatenation on text domains | Usually returns a text carrier descriptor unless the operation policy preserves a domain. |
| Comparison between domains | Returns `boolean`; comparison uses operation policy and carrier descriptor rules. |
| Aggregate over a domain column | Returns the aggregate-defined descriptor. Domain preservation occurs only where the aggregate descriptor says so. |
| `coalesce` over compatible domains | Preserves a common domain only when all admitted arms resolve to the same domain or a policy-owned common domain. |
| `case` expression | Preserves a domain only when result arms resolve to an admitted common domain. |

Domain preservation is never inferred merely from display names. It must be present in descriptor metadata or operation policy.

## Defaults

Defaults are resolved at the assignment site.

| Situation | Default Used |
| --- | --- |
| Column has a default | Column default. |
| Column has no default and domain has a default | Domain default. |
| Routine parameter has a default | Parameter default. |
| Routine parameter has no default and domain has a default | Domain default, where the call form admits omitted parameters. |
| Explicit `null` is supplied | No default is substituted; null policy decides. |

Every default expression must bind to the domain carrier and pass domain validation. Defaults are part of the dependency graph because they may reference sequences, functions, policies, collations, or other catalog objects.

## Constraint Evaluation

Domain checks use the `VALUE` pseudo-value.

```sql
create domain app.percent_value as decimal(7, 4)
  not null
  check (value >= 0 and value <= 100);
```

Constraint rules:

| Rule | Contract |
| --- | --- |
| Evaluation input | `VALUE` is the candidate after carrier coercion. |
| Parent domains | Parent checks run before child checks. |
| Nulls | Null policy is checked before ordinary constraints. |
| Pass condition | A check passes only when it evaluates to `true`. |
| Diagnostics | Named constraints should appear in diagnostics where policy allows disclosure. |
| Volatility | Constraint expressions must be deterministic or explicitly policy-admitted. |
| Dependency | Functions, collations, policies, and descriptors used by a constraint are dependencies. |

## Cast Policy

The cast policy is represented by `sys.catalog.domain_descriptor.cast_policy_uuid`.

| Policy Concern | Meaning |
| --- | --- |
| Implicit assignment | Whether a source descriptor may be assigned to the domain without explicit cast syntax. |
| Explicit cast | Whether a `cast(...)` request is admitted. |
| Lossiness | Whether rounding, truncation, timezone loss, charset loss, precision loss, or representation loss is refused or admitted. |
| Domain erasure | Whether a domain value may be treated as its carrier. |
| Domain stacking | Whether this domain may wrap another domain. |
| Failure result | Whether a failing conversion raises a diagnostic or can return a policy-defined failure value through `try_cast`. |

## Operation Policy

The operation policy is represented by `sys.catalog.domain_descriptor.operation_policy_uuid`.

| Operation Class | Domain Effect |
| --- | --- |
| Comparison | Controls ordering, equality, collation, charset, timezone, and special-value handling. |
| Hashing | Controls hash keys used by indexes, grouping, joins, and hash operators. |
| Arithmetic | Controls whether numeric operations erase or preserve the domain. |
| Text operations | Controls collation-sensitive comparison, concatenation, substring, search, and regex admission. |
| Temporal operations | Controls timezone, precision, and calendar behavior. |
| Document/vector/graph operations | Controls path, dimension, metric, traversal, and exact-recheck behavior. |
| Opaque operations | Admits only named methods, functions, or UDR routes. |

Indexes, grouping, sorting, joins, materialized-view refresh, and optimizer rewrites must all use the same operation policy that the expression binder used.

## Compound Domain Elements

Compound domains define addressable elements through `sys.catalog.domain_element`. Each element has its own target descriptor or target domain, null policy, visibility policy, and mutation policy.

Example logical shape:

```text
app.address_value
|
+-- street
+-- city
+-- region
+-- postal_code
+-- country_code
```

Element policy matters for:

- structured value validation;
- path access;
- partial updates;
- masking and redaction;
- generated columns derived from elements;
- indexes on element paths;
- support-bundle rendering;
- UDR argument and result binding.

## Examples

### Positive Integer

```sql
create domain app.positive_int as int64
  not null
  check (value > 0);

select cast(:candidate as app.positive_int);
```

### Email Text

```sql
create domain app.email_text as varchar(320)
  not null
  check (regexp_like(value, '^[^@]+@[^@]+$'));

create table app.account (
  account_id uuid primary key,
  email app.email_text unique
);
```

### Domain Over Domain

```sql
create domain app.nonblank_text as varchar(200)
  not null
  check (char_length(value) > 0);

create domain app.customer_label as app.nonblank_text
  check (char_length(value) <= 80);
```

The `customer_label` domain validates the `nonblank_text` parent first, then its own maximum-length rule.

## Failure Modes

| Failure | Behavior |
| --- | --- |
| Implicit conversion would lose information | Refuse unless cast policy explicitly admits it. |
| Explicit cast cannot convert carrier | Raise conversion diagnostic, or return the `try_cast` failure result. |
| Domain check fails | Refuse assignment and name the constraint where disclosure policy admits it. |
| Null violates policy | Refuse before ordinary constraint checks. |
| Element validation fails | Refuse the compound value or partial mutation. |
| Operation policy has no route | Refuse operation before execution. |
| Domain erasure is not admitted | Refuse use of the value as a bare carrier. |

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Cast to domain | Performs carrier conversion and full domain validation. |
| Try-cast to domain | Uses the same validation path and returns the documented failure result. |
| Domain default | Applies only when no more specific default exists. |
| Domain constraints | Evaluate in stack order with `VALUE` bound correctly. |
| Domain preservation | Result descriptors preserve or erase domain identity only according to operation policy. |
| Index/group/order | Uses the domain operation policy consistently. |
| Compound element | Validates descriptor, target domain, null policy, visibility policy, and mutation policy. |
| Rollback/recovery | Does not leave partial domain metadata or invalid validation state visible. |

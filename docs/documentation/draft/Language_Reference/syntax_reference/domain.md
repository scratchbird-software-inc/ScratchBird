# Domain Lifecycle

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_domain_lifecycle`

Related pages: [Type System Overview](../data_types/type_system_overview.md), [Domains, Casts, And Coercion](../data_types/domains_casts_and_coercion.md), [Conversion Matrix](../data_types/conversion_matrix.md), [Table Lifecycle](table.md), [Operator Type Result Matrix](operator_type_result_matrix.md), [sys.catalog.domain_descriptor](../catalog_reference/sys_catalog_domain_descriptor.md), and [sys.catalog.domain_element](../catalog_reference/sys_catalog_domain_element.md).

## Purpose

A domain is a named, UUID-owned policy layer over a canonical type descriptor or over another domain. The base descriptor controls physical value representation and primitive operations. The domain adds language-visible meaning: null policy, defaults, constraints, masking, element rules, cast rules, operation rules, display metadata, and dependency identity.

A domain is not just a type alias. A column, parameter, variable, return value, or element declared with a domain remains bound to the domain UUID until an admitted cast or operation erases that domain identity.

## Domain Model

| Part | Meaning |
| --- | --- |
| Domain UUID | Durable identity used by catalog dependencies, SBLR, routines, views, indexes, and stored descriptors. |
| Resolver name | Human-readable name such as `app.email_text`; it can be renamed without changing identity. |
| Base descriptor | Canonical carrier descriptor such as `decimal(18,2)`, `varchar(320)`, `uuid`, `json`, or `vector<float32,1536>`. |
| Base domain | Optional parent domain when a domain is layered over another domain. |
| Domain stack | Resolved chain of domains and the base descriptor, represented by a stable stack hash. |
| Null policy | Whether `null` is admitted before constraints and storage. |
| Default expression | Expression used when an assignment site has no more specific default. |
| Constraint set | Boolean predicates checked against the candidate value. |
| Element policy | Field, path, array, map, range, variant, or opaque-element visibility and mutation rules for compound domains. |
| Cast policy | Rules for implicit assignment, explicit casts, lossiness, domain preservation, and domain erasure. |
| Operation policy | Rules for comparison, ordering, hashing, indexing, arithmetic, text, temporal, document, vector, spatial, or opaque operations. |
| Masking policy | Optional redaction or protected-value behavior applied to reads, logs, support bundles, and projections. |

## Domain Kinds

`sys.catalog.domain_descriptor.domain_kind` records the domain kind. The public language contract is:

| Kind | Use |
| --- | --- |
| `scalar` | A constrained scalar value over one base descriptor. |
| `compound` | A named structured value with elements described in `sys.catalog.domain_element`. |
| `array` | A sequence domain with an element descriptor or element domain. |
| `row` | A row-shaped domain used by routines, structured values, or row expressions. |
| `map` | A key/value domain with key and value element policies. |
| `range` | A bounded interval domain with lower and upper element policy. |
| `enum` | A closed set of admitted labels or values. |
| `variant` | A tagged value whose active payload depends on a variant tag. |
| `opaque` | A value whose internals are only accessible through admitted operations or UDR bindings. |
| `alias_profile` | A renderable alias over a descriptor or domain where policy keeps alias behavior explicit. |
| `protected_history` | A domain that participates in protected-material retention, masking, or audit policy. |

Not every domain kind requires unique storage. Most domains store the same physical carrier as their base descriptor. The domain controls validation and operation admission.

## Lifecycle Statement Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE DOMAIN` | Creates the durable domain UUID, resolver name, base descriptor or base domain, null policy, default, constraints, cast policy, operation policy, optional mask, and dependencies. |
| Alter | `ALTER DOMAIN` | Mutates admitted domain metadata without changing the domain UUID. Existing data must remain valid or be revalidated through an explicit policy-owned route. |
| Rename | `RENAME DOMAIN ... TO ...` | Changes the resolver name only. Dependent columns, variables, routines, views, indexes, and stored expressions remain bound to the domain UUID. |
| Comment | `COMMENT ON DOMAIN ... IS ...` | Stores authorized descriptive metadata on the domain catalog row. |
| Show | `SHOW DOMAIN ...`, `SHOW DOMAINS` | Lists authorized domain names, descriptors, policies, validation state, and dependency summaries. |
| Describe | `DESCRIBE DOMAIN ...` | Shows one domain's base descriptor, base domain, stack hash, constraints, default, null policy, cast policy, operation policy, mask policy, elements, and dependencies. |
| Recreate | `RECREATE DOMAIN ...` | Replaces the definition through create-or-replace semantics only when dependency and data-validation policy admit it. |
| Drop | `DROP DOMAIN ... [RESTRICT | CASCADE]` | Retires the domain only after dependency, privilege, transaction, recovery, and sandbox checks pass. |

Domain lifecycle operations preserve descriptor authority. A parser-visible SBsql type name never overrides the canonical descriptor and domain UUID binding.

## Syntax

```ebnf
create_domain_statement ::=
    CREATE DOMAIN qualified_name AS domain_base domain_option* ;

domain_base ::=
      data_type
    | qualified_domain_name ;

domain_option ::=
      NULL
    | NOT NULL
    | DEFAULT expression
    | CHECK "(" domain_check_expression ")"
    | COLLATE collation_name
    | USING CAST POLICY qualified_name
    | USING OPERATION POLICY qualified_name
    | MASKED WITH qualified_name
    | ELEMENT domain_element_definition ;

domain_check_expression ::=
    expression_using_value ;
```

```ebnf
alter_domain_statement ::=
    ALTER DOMAIN qualified_name alter_domain_action ;

alter_domain_action ::=
      SET DEFAULT expression
    | DROP DEFAULT
    | SET NOT NULL
    | DROP NOT NULL
    | ADD CONSTRAINT identifier CHECK "(" domain_check_expression ")"
    | DROP CONSTRAINT identifier
    | VALIDATE CONSTRAINT identifier
    | SET CAST POLICY qualified_name
    | DROP CAST POLICY
    | SET OPERATION POLICY qualified_name
    | DROP OPERATION POLICY
    | SET MASK qualified_name
    | DROP MASK
    | SET BASE domain_base
    | ADD ELEMENT domain_element_definition
    | ALTER ELEMENT identifier alter_domain_element_action
    | DROP ELEMENT identifier ;
```

```ebnf
rename_domain_statement   ::= RENAME DOMAIN qualified_name TO identifier ;
comment_domain_statement  ::= COMMENT ON DOMAIN qualified_name IS string_literal ;
show_domain_statement     ::= SHOW DOMAIN qualified_name | SHOW DOMAINS ;
describe_domain_statement ::= DESCRIBE DOMAIN qualified_name ;
recreate_domain_statement ::= RECREATE DOMAIN qualified_name AS domain_base domain_option* ;
drop_domain_statement     ::= DROP DOMAIN qualified_name (RESTRICT | CASCADE)? ;
```

`VALUE` is the domain check pseudo-value. It represents the candidate value after descriptor coercion and before final storage. Domain checks must bind to deterministic or policy-admitted expressions. A domain check is not a table trigger and cannot silently read unrelated table state.

## Validation Order

When a value is assigned to a domain, ScratchBird validates it in a stable order:

1. Resolve the target domain UUID and domain stack under the active transaction snapshot.
2. If the assignment supplies no value, choose the most specific default: column or parameter default first, then domain default.
3. Coerce or cast the candidate value to the base descriptor through the assignment cast policy.
4. Apply the domain null policy.
5. Evaluate parent-domain constraints from the base of the stack outward.
6. Evaluate this domain's constraints with `VALUE` bound to the candidate.
7. Apply element policy for compound, array, row, map, range, variant, or opaque values.
8. Apply masking/protected-value policy when the stored value or projected value requires it.
9. Store the value under the base descriptor while preserving the domain identity in row, variable, parameter, routine, index, or result metadata where required.

If any step fails, the assignment fails and the owning statement remains subject to normal transaction rollback rules.

## Defaults And Nullability

| Rule | Contract |
| --- | --- |
| Domain default | Used only when the assignment site has no more specific default and no explicit value. |
| Column default | Overrides the domain default for that column. |
| Routine parameter default | Overrides the domain default for that parameter. |
| Explicit `null` | Is not replaced by a default. It is validated by null policy. |
| Domain `NOT NULL` | Applies everywhere the domain is used. A column or variable may be stricter but cannot loosen a `NOT NULL` domain. |
| Dropping `NOT NULL` | Admitted only if dependent objects and policy allow the looser domain. Existing storage does not need rewrite solely because null is newly allowed. |
| Adding `NOT NULL` | Requires proof that existing reachable values are non-null or an explicit validation/rewrite route. |

## Constraints

Domain constraints are value constraints. They are evaluated for each assignment, cast-to-domain, generated value, default value, routine argument, routine return, and stored expression that produces the domain.

```sql
create domain app.positive_amount as decimal(18, 2)
  default 0.00
  not null
  check (value >= 0);
```

```sql
create domain app.email_text as varchar(320)
  check (regexp_like(value, '^[^@]+@[^@]+$'));
```

Constraint behavior:

| Concern | Contract |
| --- | --- |
| Truth value | A check passes only when it evaluates to `true`. `false` fails. `null` fails unless the constraint explicitly admits it. |
| Parent domains | Parent-domain checks run before child-domain checks. |
| Constraint names | Constraint names are dependency targets and diagnostic labels. |
| Existing data | Adding or tightening a constraint requires validation of dependent stored values unless policy records the domain as not yet validated. |
| Generated values | Generated expressions are validated after generation and before row visibility. |
| Routines | Procedure/function parameters and returns declared as domains are validated at call and return boundaries. |

## Casts And Domain Preservation

Domains participate in casting through the domain cast policy:

| Situation | Default Contract |
| --- | --- |
| Base descriptor to domain | Assignment or explicit cast is allowed only if descriptor coercion and all domain validation steps pass. |
| Domain to base descriptor | Allowed where the operation requests the carrier and the domain policy allows erasure. |
| Domain to related domain | Coerce to the common carrier, then validate through the target domain stack. |
| Domain arithmetic | Usually returns the carrier descriptor unless the operation descriptor explicitly preserves the domain. |
| Domain comparison | Uses the operation policy, collation, charset, timezone, and base descriptor comparison rules. |
| `cast(value as domain)` | Performs descriptor conversion and full domain validation. |
| `try_cast(value as domain)` | Returns `null` or a policy-defined failure result instead of raising a conversion diagnostic. Domain validation still occurs before success. |

See [Domains, Casts, And Coercion](../data_types/domains_casts_and_coercion.md) for the full coercion table.

## Compound Domains And Elements

Compound domains describe addressable parts of a value. The element rows are catalog metadata, not ad hoc path strings.

```text
domain app.address_doc
|
+-- street        target domain/text descriptor
+-- city          target domain/text descriptor
+-- postal_code   target domain/text descriptor
+-- country_code  target domain/text descriptor
```

Each element has an identity, ordinal, optional name, path-segment kind, target descriptor or target domain, null policy, visibility policy, and mutation policy. Element metadata is used for structured access, update admission, masking, support diagnostics, and dependency invalidation.

## Tables, Indexes, Views, And Routines

| Dependent Surface | Domain Effect |
| --- | --- |
| Table column | Column storage uses the base descriptor. Column metadata keeps the domain UUID. Inserts, updates, defaults, generated expressions, and row imports validate the domain. |
| Index | Index keys use descriptor comparison, collation, hash, and operation policy. Tightening a domain may require index validation or rebuild. |
| View/materialized view | Result columns preserve domain identity when the projection does not erase it. Domain changes can invalidate view descriptors or materialized generations. |
| Procedure/function | Parameters, local variables, return values, and emitted rows validate at boundaries. Stored executable bodies depend on the domain UUID. |
| Trigger | Transition values declared through table domains preserve domain metadata. Assigning to `new` values revalidates domains. |
| UDR binding | UDR entry and return descriptors carry domain UUIDs where the binding admits domain-aware values. |

## Alteration Rules

`ALTER DOMAIN` must be fail-closed. The binder records the intended mutation, dependency set, validation mode, and recovery behavior before catalog state changes.

| Alteration | Required Proof |
| --- | --- |
| Set default | New expression binds to the domain carrier and validates through the domain. |
| Drop default | Assignment sites without local defaults must be able to handle missing defaults. |
| Add constraint | Existing reachable values are validated or the domain is marked not-yet-validated with reads/writes governed by policy. |
| Drop constraint | Dependent generated expressions, indexes, routines, and views are invalidated if their assumptions changed. |
| Tighten null policy | Existing reachable values and defaults are proven non-null. |
| Loosen null policy | Dependent code that assumes non-null is invalidated or refused. |
| Change base descriptor | Requires explicit conversion, lossiness policy, existing-row validation, index impact analysis, and routine/view rebind. |
| Change cast policy | Prepared statements, routines, views, and generated columns that used the prior policy are invalidated. |
| Change operation policy | Indexes, comparisons, ordering, grouping, hash tables, and optimizer plans are invalidated. |
| Change mask policy | Security epochs and support-bundle redaction proofs are invalidated. |
| Change elements | Compound-domain element users are invalidated and path access is rechecked. |

## Dependency And Transaction Semantics

Domain DDL is transactional. A created, altered, renamed, recreated, or dropped domain becomes visible only when the owning transaction commits. Rollback restores the previous visible catalog state.

The dependency graph includes:

- columns and generated columns using the domain;
- defaults and constraints that reference functions, collations, sequences, policies, or other domains;
- indexes whose key descriptors or operation policies depend on the domain;
- views and materialized views whose result descriptors preserve the domain;
- routines, triggers, and UDR bindings that use the domain;
- casts and operation descriptors involving the domain;
- catalog projections, metadata rendering, and support-bundle surfaces.

`DROP DOMAIN ... RESTRICT` refuses if authorized dependencies remain. `DROP DOMAIN ... CASCADE` must make each dependent action explicit in the engine plan and must not silently erase stored data meaning.

## SHOW And DESCRIBE

`SHOW DOMAINS` returns the domains visible to the effective user or agent.

`DESCRIBE DOMAIN` should include:

- resolver name and domain UUID;
- domain kind;
- base descriptor and base domain;
- null policy;
- default expression;
- constraint names and validation state;
- cast policy and operation policy;
- masking policy;
- element list for compound domains;
- dependent object summary;
- last catalog generation and security epoch visible to the caller.

Catalog base rows remain protected. `SHOW` and `DESCRIBE` are authorized projections, not direct catalog authority.

## Practical Examples

### Monetary Amount

```sql
create domain app.nonnegative_money as decimal(18, 2)
  default 0.00
  not null
  check (value >= 0);

create table app.invoice (
  invoice_id uuid primary key,
  subtotal app.nonnegative_money,
  tax app.nonnegative_money,
  total app.nonnegative_money
);
```

### Enumerated State

```sql
create domain app.order_state as varchar(16)
  default 'new'
  not null
  check (value in ('new', 'paid', 'shipped', 'closed', 'cancelled'));

alter domain app.order_state
  add constraint order_state_not_blank check (char_length(value) > 0);
```

### Renaming Without Breaking Dependencies

```sql
create domain app.customer_code as varchar(40)
  not null
  check (char_length(value) >= 3);

rename domain app.customer_code to customer_identifier;

describe domain app.customer_identifier;
```

The rename changes the visible name. Columns and routines that already reference the domain remain bound to the same domain UUID.

### Explicit Cast To A Domain

```sql
select cast(:candidate_amount as app.nonnegative_money);
select try_cast(:candidate_amount as app.nonnegative_money);
```

Both forms run descriptor conversion and domain validation. The `try_cast` form uses its failure-return contract instead of raising a conversion diagnostic.

## Failure Modes

| Failure | Required Behavior |
| --- | --- |
| Unknown domain | Return a not-found or hidden-object diagnostic according to metadata-hiding policy. |
| Ambiguous name | Refuse binding and report ambiguity. |
| Unauthorized create/alter/drop | Refuse before catalog mutation. |
| Invalid base descriptor | Refuse create or alter before catalog mutation. |
| Default does not type-check | Refuse create or alter. |
| Constraint does not type-check | Refuse create or alter. |
| Existing data violates tightened policy | Refuse, or record an explicit not-yet-validated state only where policy admits it. |
| Dependency prevents drop | Refuse `RESTRICT`; require explicit cascade plan for `CASCADE`. |
| Crash during DDL | Recover to old visible state, new committed state, or recovery-required fail-closed state. Silent partial catalog mutation is not allowed. |

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | Every lifecycle statement shape is recognized by SBsql. |
| Bind | Names, UUIDs, descriptors, defaults, constraints, options, elements, and dependencies resolve exactly. |
| Authorize | The effective user or agent UUID is allowed to create, alter, describe, or drop the domain. |
| Admit | SBLR route and result shape are accepted by the engine verifier. |
| Validate | Defaults, casts, constraints, null policy, and element policy are enforced in the documented order. |
| Commit | Catalog mutation becomes visible only through MGA transaction finality. |
| Rollback | Catalog mutation and validation state roll back cleanly. |
| Invalidate | Dependent caches, metadata, plans, indexes, routines, and projections are refreshed or refused. |
| Recover | Crash/restart never leaves a silently inconsistent domain descriptor. |

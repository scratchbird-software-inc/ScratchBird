# Sequence Lifecycle

This page is part of the SBsql Language Reference Manual. It documents sequence creation, value generation, alteration, restart, cache behavior, ownership, inspection, comments, recreation, and drop behavior.

Generation task: `syntax_reference_sequence_lifecycle`

Related pages: [Table Lifecycle](table.md), [Domain Lifecycle](domain.md), [INSERT Statement](insert.md), [Transaction Control](transaction_control.md), [Schema Tree And Name Resolution](schema_tree_and_name_resolution.md), [Security And Privileges](security_and_privilege_statements.md), [Refusal Vectors](refusal_vectors.md), [Numeric Types](../data_types/numeric_types.md), and [UUID Catalog Identity](../core_paradigms/uuid_catalog_identity.md).

## Purpose

A sequence is a catalog object that produces ordered numeric values under an explicit allocation policy. Sequences are commonly used for generated keys, ticket numbers, durable counters that allow gaps, and identity-column backing state.

A sequence has durable catalog identity, a resolver name, an owner, a numeric descriptor, bounds, increment direction, restart state, cache policy, cycle behavior, dependencies, privileges, comments, and inspection policy. The sequence name is resolver input; the sequence UUID and state descriptor are the authority used by the engine.

Sequence value generation is a runtime operation, not only DDL. Creating or altering the sequence is transactional catalog mutation. Allocating a value uses the sequence state machine and must preserve uniqueness rules under concurrency and recovery.

## Lifecycle Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE SEQUENCE` | Creates the sequence UUID, numeric descriptor, bounds, increment, cache policy, cycle behavior, owner, and dependencies. |
| Generate value | `NEXT VALUE FOR` | Allocates one value from the sequence according to the current state and policy. |
| Inspect current value | `CURRENT VALUE FOR`, `SHOW SEQUENCE` | Returns authorized state projection without granting allocation authority. |
| Alter | `ALTER SEQUENCE` | Changes admitted descriptor metadata, bounds, increment, restart point, cache, cycle, ownership, or policy. |
| Restart | `ALTER SEQUENCE ... RESTART` | Changes the next allocation point after catalog and state admission checks. |
| Rename | `RENAME SEQUENCE ... TO ...` | Changes resolver label only; sequence UUID, dependencies, and allocation state remain attached to the same object. |
| Comment | `COMMENT ON SEQUENCE ... IS ...` | Stores or clears descriptive metadata. |
| Show | `SHOW SEQUENCE`, `SHOW SEQUENCES` | Lists authorized metadata and state projections. |
| Describe | `DESCRIBE SEQUENCE` | Returns the complete authorized descriptor and dependency summary for one sequence. |
| Recreate | `RECREATE SEQUENCE` | Replaces a sequence only when dependency and state-loss behavior are explicit and admitted. |
| Drop | `DROP SEQUENCE` | Retires the sequence after dependency handling. |

## Syntax

```ebnf
sequence_lifecycle_statement ::=
      create_sequence_statement
    | alter_sequence_statement
    | rename_sequence_statement
    | recreate_sequence_statement
    | comment_on_sequence_statement
    | show_sequence_statement
    | describe_sequence_statement
    | drop_sequence_statement ;
```

```ebnf
create_sequence_statement ::=
    CREATE SEQUENCE if_not_exists? sequence_ref sequence_option_list? ;

sequence_option_list ::=
    sequence_option+ ;

sequence_option ::=
      AS type_descriptor
    | START WITH signed_integer_literal
    | INCREMENT BY signed_integer_literal
    | MINVALUE signed_integer_literal
    | NO MINVALUE
    | MAXVALUE signed_integer_literal
    | NO MAXVALUE
    | CACHE integer_literal
    | NO CACHE
    | CYCLE
    | NO CYCLE
    | OWNED BY column_ref
    | OWNED BY NONE
    | WITH POLICY policy_ref
    | COMMENT string_literal ;
```

```ebnf
alter_sequence_statement ::=
    ALTER SEQUENCE sequence_ref alter_sequence_action+ ;

alter_sequence_action ::=
      AS type_descriptor
    | RESTART
    | RESTART WITH signed_integer_literal
    | SET INCREMENT BY signed_integer_literal
    | SET MINVALUE signed_integer_literal
    | SET NO MINVALUE
    | SET MAXVALUE signed_integer_literal
    | SET NO MAXVALUE
    | SET CACHE integer_literal
    | SET NO CACHE
    | SET CYCLE
    | SET NO CYCLE
    | SET OWNER principal_ref
    | SET OWNED BY column_ref
    | SET OWNED BY NONE
    | SET POLICY policy_ref
    | RESET POLICY ;
```

```ebnf
rename_sequence_statement ::=
    RENAME SEQUENCE sequence_ref TO identifier ;

recreate_sequence_statement ::=
    RECREATE SEQUENCE sequence_ref sequence_option_list? ;

comment_on_sequence_statement ::=
    COMMENT ON SEQUENCE sequence_ref IS (string_literal | NULL) ;
```

```ebnf
show_sequence_statement ::=
      SHOW SEQUENCES show_sequence_filter?
    | SHOW SEQUENCE sequence_ref show_sequence_option_list? ;

show_sequence_filter ::=
      LIKE string_literal
    | WHERE predicate ;

show_sequence_option_list ::=
    WITH show_sequence_option ("," show_sequence_option)* ;

show_sequence_option ::=
      STATE
    | BOUNDS
    | CACHE
    | OWNERSHIP
    | DEPENDENCIES
    | GRANTS
    | UUIDS ;
```

```ebnf
describe_sequence_statement ::=
    DESCRIBE SEQUENCE sequence_ref describe_sequence_option_list? ;

describe_sequence_option_list ::=
    WITH describe_sequence_option ("," describe_sequence_option)* ;

describe_sequence_option ::=
      STATE
    | BOUNDS
    | CACHE
    | OWNERSHIP
    | DEPENDENCIES
    | GRANTS
    | UUIDS ;
```

```ebnf
drop_sequence_statement ::=
    DROP SEQUENCE if_exists? sequence_ref drop_sequence_behavior? ;

drop_sequence_behavior ::=
      RESTRICT
    | CASCADE ;
```

```ebnf
sequence_value_expression ::=
      NEXT VALUE FOR sequence_ref
    | CURRENT VALUE FOR sequence_ref ;
```

SBsql is context sensitive. Sequence lifecycle words are command words inside sequence statements and should not be treated as globally reserved identifiers outside those contexts.

## Create Sequence

Basic ascending sequence:

```sql
create sequence app.order_number
  start with 1
  increment by 1
  no cycle;
```

Sequence with an explicit integer descriptor and cache:

```sql
create sequence app.invoice_number
  as int64
  start with 100000
  increment by 1
  minvalue 100000
  maxvalue 999999999
  cache 100
  no cycle;
```

Descending sequence:

```sql
create sequence app.countdown
  as int32
  start with 100
  increment by -1
  minvalue 1
  maxvalue 100
  no cycle;
```

The binder must prove:

- the effective principal has `CREATE SEQUENCE` in the target schema;
- the sequence name resolves within the session schema root;
- the numeric descriptor is supported and compatible with the declared range;
- the increment is not zero;
- start, minimum, maximum, restart, and cycle rules are coherent;
- cache size is within policy limits;
- ownership and policy descriptors resolve and are authorized;
- the sequence catalog route is available and admitted;
- the database is not fenced against catalog writes.

## Numeric Descriptor

`AS type_descriptor` selects the value descriptor. Portable sequence descriptors are exact integer descriptors. A sequence should not use approximate numeric values because allocation, ordering, uniqueness, and recovery require exact comparison.

| Descriptor Area | Rule |
| --- | --- |
| Exact integer | Portable and recommended. |
| Decimal exact numeric | Admitted only where policy defines integer-step behavior. |
| Approximate numeric | Refused for ordinary sequences. |
| Domain descriptor | Admitted only when the domain carrier is an exact integer and its constraints are compatible with sequence allocation. |

If `AS` is omitted, the default sequence descriptor is policy-defined. Portable scripts should declare it explicitly.

## Bounds, Start, And Increment

The sequence state machine uses:

| Property | Meaning |
| --- | --- |
| `START WITH` | Initial value or restart default. |
| `INCREMENT BY` | Step applied after each allocation. Must not be zero. |
| `MINVALUE` | Lowest value admitted for allocation. |
| `MAXVALUE` | Highest value admitted for allocation. |
| `CYCLE` | When a bound is reached, wrap to the opposite bound if policy admits it. |
| `NO CYCLE` | Refuse allocation after exhaustion. |

Ascending sequence:

```text
start <= next <= maxvalue
increment > 0
```

Descending sequence:

```text
minvalue <= next <= start
increment < 0
```

If an allocation would pass the bound:

- `NO CYCLE` returns an exhaustion diagnostic;
- `CYCLE` wraps to the minimum for ascending sequences or the maximum for descending sequences;
- cycle behavior must still preserve uniqueness only within the active cycle policy, not across all historical values.

## Cache Policy

`CACHE n` lets the engine reserve a block of sequence values for efficient allocation.

```sql
alter sequence app.order_number
  set cache 1000;
```

Cache behavior is part of the public contract:

- cached values may be skipped after crash, process stop, failover of responsibility, or cache invalidation;
- cached values must not be handed out twice;
- the durable high-water mark must be advanced safely before cached values are exposed;
- `NO CACHE` minimizes cache-related gaps but does not make the sequence gapless;
- changing cache size affects future allocation, not values already handed out.

Applications that require gapless audited numbers should use a separate serialized allocation design, not a normal sequence.

## Value Generation

Use `NEXT VALUE FOR` to allocate a value:

```sql
select next value for app.order_number;
```

Use a sequence in a column default:

```sql
create table app.orders (
  order_number int64 not null default next value for app.order_number,
  order_id uuid not null,
  primary key (order_number)
);
```

Insert with default generation:

```sql
insert into app.orders (order_id)
values (uuid '019d0000-0000-7000-8000-000000000001')
returning order_number;
```

Rules:

- each evaluation of `NEXT VALUE FOR` allocates one value;
- the allocated value is descriptor-bound before assignment;
- the caller needs sequence usage authority and target-column assignment authority;
- value allocation must be concurrency safe;
- allocation can produce gaps;
- allocation is not undone merely because the caller's transaction rolls back;
- allocation never gives the parser authority to mutate table rows by itself.

`CURRENT VALUE FOR` returns the current value visible to the session according to the sequence policy. If no value has been allocated in the required scope, it returns a diagnostic rather than inventing a value.

```sql
select current value for app.order_number;
```

## Identity Columns

An identity column may be backed by a sequence-like state object. The table descriptor owns the column behavior; the sequence object owns allocation state when the identity is represented as a named or internal sequence.

Example:

```sql
create sequence app.customer_number_seq
  as int64
  start with 1
  increment by 1
  no cycle;

create table app.customer (
  customer_number int64 not null default next value for app.customer_number_seq,
  customer_id uuid not null,
  display_name varchar(120) not null,
  primary key (customer_number)
);
```

When `OWNED BY` links a sequence to a column, dependency handling must preserve or explicitly break that relationship during rename, alter, recreate, backup, restore, and drop.

```sql
alter sequence app.customer_number_seq
  set owned by app.customer.customer_number;
```

Dropping the table or column with a cascade plan may drop an owned sequence only when the dependency plan explicitly admits it.

## Alter Sequence

Alter range, increment, and cache:

```sql
alter sequence app.order_number
  set increment by 10
  set cache 50
  set no cycle;
```

Restart:

```sql
alter sequence app.order_number
  restart with 1000000;
```

Change owner:

```sql
alter sequence app.order_number
  set owner app_owner;
```

Alteration rules:

- changing bounds must not make the current or next value invalid unless an explicit restart resolves it;
- changing increment direction requires coherent bounds and restart state;
- changing descriptor must preserve representability of bounds, state, cache, and dependent columns;
- restart is a state mutation and must be audited;
- owner and policy changes require the relevant security authority;
- active cached reservations must be invalidated or reconciled according to the cache policy.

`RESTART` without `WITH` uses the declared start value or the policy-defined restart point.

## Rename Sequence

```sql
rename sequence app.order_number to order_number_old;
```

Rename changes the resolver label only. It does not change:

- sequence UUID;
- allocation state;
- dependencies from defaults or identity columns;
- grants;
- comments;
- ownership;
- cached reservations;
- support-bundle identity.

Prepared statements and metadata caches that resolved the old name must rebind or fail closed after commit.

## Recreate Sequence

`RECREATE SEQUENCE` is a controlled replacement surface:

```sql
recreate sequence app.stage_number
  as int64
  start with 1
  increment by 1
  no cache
  no cycle;
```

If the sequence does not exist, it is created. If it exists, replacement is admitted only when dependencies, grants, current state, cache reservations, and policy allow replacement. Recreate must not silently reset a production sequence that is referenced by table defaults, identity columns, routines, or active prepared statements.

## Comment, Show, And Describe

Comments:

```sql
comment on sequence app.order_number is 'Public order number allocator';
comment on sequence app.order_number is null;
```

Inspection:

```sql
show sequences;
show sequence app.order_number with bounds, cache, ownership;
describe sequence app.order_number with state, dependencies, grants;
```

Inspection does not allocate values. State fields such as last allocated value, next value, high-water mark, cache reservations, and exhaustion status are returned only when the caller has inspection authority. Policies may redact exact state while still showing descriptor metadata.

## Drop Sequence

```sql
drop sequence app.stage_number restrict;
```

`RESTRICT` refuses the drop if any dependency remains, including:

- table defaults;
- identity columns;
- generated expressions;
- routines;
- triggers;
- policies;
- views or materialized views;
- prepared statement metadata;
- backup or migration plans;
- ownership links.

`CASCADE` requires an explicit authorized dependency plan. It must not remove dependent objects or defaults by surprise.

```sql
drop sequence app.stage_number cascade;
```

`IF EXISTS` suppresses a not-found result only for an absent visible sequence. It does not hide a privilege failure, sandbox denial, dependency failure, recovery fence, or object-class mismatch.

## Transaction And Recovery Semantics

Sequence DDL is transactional. Create, alter, rename, comment, recreate, and drop become visible through MGA transaction finality.

Sequence allocation has different semantics:

- once a value is handed to a statement, it may remain consumed even if the caller rolls back;
- gaps are allowed and expected;
- cache reservations may skip values after crash or restart;
- recovery must never make one sequence value visible as allocated to two independent executions;
- if recovery cannot prove the safe next value, allocation is fenced and returns a refusal vector;
- restart and range changes are catalog/state operations and must be durable before future allocation observes them.

This distinction lets sequences remain safe under high concurrency without pretending to be gapless counters.

## Security

Sequence privileges are separate from table privileges.

| Privilege | Meaning |
| --- | --- |
| `USAGE` | Allows `NEXT VALUE FOR` where policy admits allocation. |
| `SELECT` | Allows authorized inspection such as `CURRENT VALUE FOR` or state projections. |
| `ALTER` | Allows metadata and state changes admitted by policy. |
| `DROP` | Allows dropping the sequence after dependency checks. |
| `COMMENT` | Allows comment mutation. |
| `DESCRIBE` | Allows metadata inspection. |

A user who can insert into a table does not automatically have direct sequence usage unless the table default or identity-column policy delegates allocation for that insert. A user who can inspect sequence metadata does not automatically have allocation authority.

## Refusal Conditions

| Condition | Result |
| --- | --- |
| Increment is zero | Bind diagnostic. |
| Start value outside bounds | Bind diagnostic. |
| Min/max incompatible with descriptor | Bind diagnostic. |
| Sequence exhausted with `NO CYCLE` | Runtime diagnostic or refusal according to allocation stage. |
| Cycle requested by policy-forbidden sequence | `denied` or `unsupported`. |
| Cache size exceeds policy | `denied`. |
| Caller lacks `USAGE` for allocation | `denied`. |
| Caller lacks state inspection authority | `denied` or redacted result. |
| Restart would collide with protected policy | `denied`. |
| Drop has remaining dependencies under `RESTRICT` | `denied`. |
| Sequence outside session sandbox root | `denied`. |
| Recovery cannot prove safe high-water mark | `denied` with recovery stage. |
| Product profile omits a gated route | `unsupported` or `unlicensed` according to route admission. |

## Practical Patterns

Application order number:

```sql
create sequence app.order_number
  as int64
  start with 1
  increment by 1
  cache 100
  no cycle;

create table app.orders (
  order_number int64 not null default next value for app.order_number,
  order_id uuid not null,
  customer_id uuid not null,
  primary key (order_number)
);
```

Small bounded cycle for a reusable slot label:

```sql
create sequence app.slot_number
  as int32
  start with 1
  increment by 1
  minvalue 1
  maxvalue 32
  cycle;
```

Administrative restart after an admitted staging reset:

```sql
alter sequence app.stage_number
  restart with 1;
```

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | Every lifecycle statement and value expression shape is recognized by SBsql. |
| Bind | Sequence names resolve to sequence UUIDs; options bind to descriptors and policies. |
| Create | Catalog row records descriptor, bounds, increment, cache, cycle, owner, policy, and dependencies. |
| Generate | Concurrent `NEXT VALUE FOR` calls never receive the same value. |
| Rollback | Rolling back a transaction that used a sequence does not make the value available for unsafe reuse. |
| Cache | Cached allocation advances durable high-water state safely and may skip values after crash. |
| Alter | Bound, increment, descriptor, cache, cycle, owner, and policy changes are validated before commit. |
| Restart | Restart is audited and cannot violate bounds, descriptor, dependency, or recovery policy. |
| Rename | Resolver label changes while UUID and allocation state remain stable. |
| Comment | Comments persist, clear with `NULL`, and obey disclosure policy. |
| Show/describe | Inspection surfaces redact state when required and do not allocate values. |
| Drop | `RESTRICT` refuses dependencies; `CASCADE` requires explicit authorized dependency handling. |
| Recovery | Reopen after crash never duplicates allocated values and fences uncertain state. |
| Proof | Full rebuild tests regenerate parser, SBLR, catalog, security, allocation, crash/recovery, and refusal evidence. |

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| `create_sequence_statement` | statement | DDL | yes | catalog mutation |
| `alter_sequence_statement` | statement | DDL | yes | catalog mutation |
| `rename_sequence_statement` | statement | DDL | yes | catalog mutation |
| `recreate_sequence_statement` | statement | DDL | yes | catalog mutation |
| `drop_sequence_statement` | statement | DDL | yes | catalog mutation |
| `comment_on_sequence_statement` | statement | metadata | yes | catalog mutation |
| `show_sequence_statement` | statement | inspection | yes | metadata rowset |
| `describe_sequence_statement` | statement | inspection | yes | metadata rowset |
| `next_value_for` | expression | sequence allocation | yes | scalar value |
| `current_value_for` | expression | sequence inspection | yes | scalar value |

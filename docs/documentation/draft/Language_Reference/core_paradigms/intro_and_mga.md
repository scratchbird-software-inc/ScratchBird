# Intro And MGA Authority

This page is part of the SBsql Language Reference Manual. It introduces the
language model used by ScratchBird, the role of SBsql, and the MGA authority
rules that govern visibility, transaction finality, catalog identity, and
recovery behavior.

Generation task: `core_paradigms_intro_and_mga`

## Purpose

ScratchBird is a convergent data engine. It exposes one authority model for
relational data, document data, graph data, vectors, key-value structures,
streams, catalog metadata, security metadata, and operational state. SBsql is
the native language used to describe work against that engine.

The engine does not execute SQL text. SBsql text is parsed, bound, lowered into
SBLR, admitted by the server boundary, and executed by engine-owned operation
identity. This keeps text syntax, parser behavior, client tools, and rendering
choices separate from durable state and recovery authority.

MGA is the transaction authority. MGA controls:

- transaction identity;
- snapshots and row-version visibility;
- commit, rollback, savepoint, retain, and chain behavior;
- cleanup and retention horizons;
- recovery classification after interruption;
- the final decision about whether a version is visible to a transaction.

A practical reading rule follows from this:

> Treat every SBsql name, clause, keyword, parameter, and option as a request
> that must be parsed, bound, admitted, authorized, and executed. Treat every
> durable object, durable value, transaction outcome, and security decision as
> engine-owned state.

## Core Concepts

| Concept | Meaning |
| --- | --- |
| SBsql | The native user-facing command language. It is context sensitive and intentionally keeps most words usable as identifiers. |
| SBLR | The canonical operation language admitted by the server and executed by the engine. |
| UUID catalog identity | Durable object identity. Names, aliases, and visible paths resolve to UUIDs before execution. |
| Descriptor | Engine-readable type, row, parameter, stream, result, policy, or object-shape metadata. |
| MGA | Multi-generational architecture: writes create or retire versions, and transactions see versions through snapshots. |
| Transaction inventory | Durable transaction-state evidence used for visibility, cleanup, commit, rollback, and recovery. |
| Snapshot | The visibility context used by a transaction or statement. |
| Message vector | Structured diagnostic output returned for success, warning, refusal, or failure. |
| Refusal | An explicit unsupported, denied, unavailable, unlicensed, invalid, or fail-closed response. |

## Authority Boundaries

![diagram](./intro_and_mga-1.svg)

| Layer | Owns | Does Not Own |
| --- | --- | --- |
| Client | Source text, parameter values, display preferences, cancellation request. | Catalog identity, transaction finality, authorization, storage state. |
| Parser | Tokenization, grammar recognition, source spans, AST construction. | Durable object identity, MGA visibility, engine execution. |
| Binder | Name resolution, UUID binding, descriptor binding, scope binding. | Commit/rollback outcome, recovery classification, storage mutation. |
| Server admission | Envelope validation, operation-family admission, fail-closed rejection of malformed SBLR. | Business semantics, final row visibility, durable catalog mutation. |
| Engine | Catalog, storage, MGA, optimizer execution, index maintenance, large-value storage, policy enforcement, recovery. | User-facing grammar, source-text spelling, client display formatting. |
| Result renderer | Human-readable output, command tags, result formatting. | Reinterpreting source text or overriding the result envelope. |

## What MGA Means In Practice

MGA stores and evaluates multiple versions of data. A transaction sees the
version set admitted by its snapshot and security context. Writers create new
versions or retire existing versions; they do not overwrite the meaning of
already-visible data for existing snapshots.

![diagram](./intro_and_mga-2.svg)

The important user-facing effects are:

- readers do not become storage authority by reading a row;
- writers do not make uncommitted versions visible to other transactions;
- indexes provide candidate access paths, not final row authority;
- a committed transaction is visible only when the engine's transaction
  inventory and snapshot rules say it is visible;
- a rolled-back transaction's versions are not visible to later valid snapshots;
- recovery fences ordinary work when finality evidence is uncertain.

## Statement Lifecycle

Every executable SBsql statement follows the same broad lifecycle:

1. Tokenize the source text.
2. Parse the statement and build a diagnostic source map.
3. Bind names to UUID catalog identity.
4. Bind parameters, literals, expressions, rows, streams, and results to
   descriptors.
5. Lower the bound statement into an SBLR execution envelope.
6. Admit or refuse the envelope at the server boundary.
7. Recheck authorization, policy, sandbox, transaction state, and resource
   state.
8. Execute under engine authority.
9. Return a result envelope and message vector.

This lifecycle applies to queries, DML, DDL, transaction control, security
statements, management statements, procedural SQL, multimodel statements,
streaming operations, backup/restore/replication/migration operations, and
recognized refusal routes.

## Name Identity Versus UUID Identity

Names are for users. UUIDs are for durable identity.

For example, a user might write:

```sql
select order_id, total
from app.orders
where total > 100.00;
```

During binding, `app.orders`, `order_id`, and `total` resolve to catalog UUIDs
and descriptors. The engine receives the bound object and descriptor identity,
not a request to search for the text `orders` at execution time.

This matters because:

- renaming an object changes resolver state, not the object's durable identity;
- dependency tracking uses UUID identity;
- support diagnostics can refer to objects unambiguously;
- scripts can use names for readability while automation can use UUID references
  when ambiguity must be avoided;
- authorization and policy checks are applied to the resolved object identity.

See [UUID Catalog Identity](uuid_catalog_identity.md) and
[Schema Tree And Name Resolution](../syntax_reference/schema_tree_and_name_resolution.md)
for the detailed resolver rules.

## Descriptors And Type Authority

SBsql syntax can name types and write literals in a human-friendly form. The
engine operates on descriptors.

A descriptor can describe:

- scalar types such as integer, decimal, real, text, binary, UUID, temporal,
  boolean, protected material, and large values;
- structured values such as rows, records, arrays, multisets, ranges, documents,
  graph values, vectors, and cursor rows;
- domain constraints, default behavior, nullability, collation, character set,
  scale, precision, length, and time-zone behavior;
- parameter slots and result columns;
- stream frame shapes and backpressure expectations.

Descriptor binding prevents the parser from treating a value as merely textual
after it has entered the execution pipeline. A value's type, length, range,
coercion path, and result behavior must be explicit enough for admission and
engine execution to validate.

See [Type System Overview](../data_types/type_system_overview.md),
[Numeric Types](../data_types/numeric_types.md), and
[Conversion Matrix](../data_types/conversion_matrix.md).

## Transaction Authority

SBsql can request transaction operations:

```sql
begin transaction isolation snapshot;

insert into app.orders (order_id, total)
values (uuid '018f2f1e-6d8a-7c31-9f44-000000000001', 42.50);

commit;
```

The parser can recognize `begin transaction`, `insert`, and `commit`. Binding
can resolve `app.orders`, assign descriptors, and lower each statement to SBLR.
Only the engine can:

- allocate the transaction identity;
- decide the snapshot;
- create row versions;
- publish commit finality;
- roll back versions;
- classify recovery state if interruption occurs.

Autocommit is also an engine-coordinated execution profile. It does not create a
shortcut around MGA.

See [Transactions And Recovery](transactions_and_recovery.md) and
[Transaction Control](../syntax_reference/transaction_control.md).

## Catalog And DDL Authority

DDL statements are catalog mutation requests. They are parsed as SBsql, bound to
UUID identity and descriptors, and executed as transaction-protected catalog
operations.

Examples include:

```sql
create schema app;

create table app.orders (
    order_id uuid primary key,
    total decimal(18,2) not null
);

comment on table app.orders is 'Orders accepted by the application';
```

The engine decides whether these operations can be admitted, whether the caller
has the required privileges, which catalog UUIDs are created, which dependencies
are recorded, and whether the transaction commits.

DDL is not merely metadata text. It changes durable catalog state under MGA and
recovery rules.

## Query Authority

A query result is produced by the engine under the active snapshot and security
context. The optimizer may use indexes, statistics, plan cache entries, and
physical layout evidence, but final row admission still requires:

- catalog identity validation;
- descriptor validation;
- MGA visibility;
- predicate evaluation;
- row-level policy and mask evaluation;
- transaction and recovery safety;
- result-shape compliance.

This is why an index can accelerate a query without becoming row authority.

## Security And Policy Authority

Security is applied after names are resolved and before results or durable
changes are exposed. A user may be able to type a valid statement and still be
refused because the resolved object, operation, policy, or sandbox does not
permit it.

Security and policy checks include:

- identity and role context;
- schema-root and sandbox visibility;
- object privileges;
- row-level policy;
- column masks;
- protected-material policy;
- management and operational privileges;
- bridge, stream, file, and external-access policy where applicable.

See [Security And Sandboxing](security_and_sandboxing.md),
[Security And Privileges](../syntax_reference/security_and_privilege_statements.md),
and [Policy, Mask, And RLS Lifecycle](../syntax_reference/policy_mask_and_rls.md).

## Recovery Authority

Recovery is engine-owned. After a crash, forced stop, interrupted sync, partial
write, or uncertain finality event, the engine classifies durable state before
ordinary work proceeds.

Possible outcomes include:

- old state survives;
- new committed state survives;
- rollback evidence is accepted;
- recovery-required state is reported;
- write admission is fenced;
- operator or policy decision is required;
- corruption or uncertainty is detected and fails closed.

SQL text, client retry behavior, timestamps, ordinary logs, and parser state are
not recovery authority.

## Refusal As A First-Class Result

ScratchBird treats refusal as a valid, explicit outcome. A statement can be
recognized and still be refused because it is unsupported, denied, unavailable,
unlicensed, malformed after binding, unsafe under recovery state, or outside the
admitted build capability set.

This matters for release safety. Silent no-op behavior and best-effort
substitution are not acceptable when a command has durable, security, recovery,
or operational meaning.

See [Refusal Vectors](../syntax_reference/refusal_vectors.md).

## Syntax Productions

The top-level language productions are:

```ebnf
script                  ::= statement_list EOF ;
```

```ebnf
statement_list          ::= statement ( statement_terminator statement )*
                          | empty ;
```

```ebnf
statement               ::= native_statement
                          | refusal_statement ;
```

```ebnf
native_statement        ::= query_dml_stmt
                          | dml
                          | ddl_catalog
                          | transaction_statement
                          | dcl_security_stmt
                          | policy_stmt
                          | observability_statement
                          | management_stmt
                          | acceleration_stmt
                          | archive_replication_stmt
                          | multi_model_op_stmt
                          | cluster_gated_statement ;
```

```ebnf
transaction_statement   ::= begin_transaction
                          | commit_stmt
                          | rollback_transaction
                          | savepoint_statement
                          | set_transaction
                          | show_transaction ;
```

## Binding And Execution Summary

| Step | User-Facing Meaning | Engine Boundary Meaning |
| --- | --- | --- |
| Parse | The text is valid SBsql syntax. | No durable authority yet. |
| Bind | Names, parameters, expressions, and results have known meaning. | UUID references and descriptors are available for SBLR. |
| Lower | The request has a canonical SBLR envelope. | SQL text is no longer executable authority. |
| Admit | The server accepts the envelope shape and route. | Malformed or gated requests fail closed before engine dispatch. |
| Authorize | The resolved operation is allowed for this principal and context. | Security and policy are rechecked against bound identity. |
| Execute | The operation runs under MGA and storage authority. | Durable state changes and result visibility are engine-owned. |
| Return | The client receives rows, command completion, stream state, or diagnostics. | The result envelope is the rendering source of truth. |

## Practical Rules For SBsql Authors

- Use names for readable scripts, but understand that names bind to UUIDs before
  execution.
- Use explicit casts when a literal or parameter could bind to more than one
  descriptor.
- Treat transaction control as a request; finality is reported by the engine.
- Do not infer success from the absence of returned rows; inspect the command
  completion and message vector.
- Expect security and policy to be checked after name resolution.
- Expect recovery-required state to refuse ordinary work until classified.
- Expect unsupported and gated surfaces to return explicit refusals.
- Use `show`, `describe`, catalog views, and support diagnostics to inspect
  admitted state rather than relying on source text.

## Related Reference Pages

- [Parser To SBLR Pipeline](parser_to_sblr_pipeline.md)
- [UUID Catalog Identity](uuid_catalog_identity.md)
- [Transactions And Recovery](transactions_and_recovery.md)
- [Security And Sandboxing](security_and_sandboxing.md)
- [Schema Tree And Name Resolution](../syntax_reference/schema_tree_and_name_resolution.md)
- [Transaction Control](../syntax_reference/transaction_control.md)
- [Security And Privileges](../syntax_reference/security_and_privilege_statements.md)
- [Policy, Mask, And RLS Lifecycle](../syntax_reference/policy_mask_and_rls.md)
- [Refusal Vectors](../syntax_reference/refusal_vectors.md)
- [Type System Overview](../data_types/type_system_overview.md)

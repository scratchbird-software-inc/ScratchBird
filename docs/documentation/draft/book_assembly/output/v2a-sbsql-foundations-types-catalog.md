---
title: "SBsql Language Reference — Foundations, Data Types, and Catalog"
---

# SBsql Language Reference — Foundations, Data Types, and Catalog

*ScratchBird documentation — draft*

## Who this book is for

Developers writing SBsql; readers needing the type system and catalog model.

## About this book

This volume covers the foundations of the SBsql language: the core paradigms that govern how SBsql relates to the engine, the data type system, the system catalog surfaces, and localized language support. It is the first of three SBsql reference volumes.

## Parts in this volume

- **Language Reference Overview**
- **Core Paradigms**
- **Data Types**
- **Catalog Reference**
- **Language Support**

> This is a **draft**. See *About This Documentation* at the end of this book for
> status and license. Confirm any specific behavior against the current build.

\newpage



# Language Reference Overview




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/README.md -->

<a id="ch-language-reference-readme-md"></a>

# SBsql Language Reference Manual

This directory contains the draft SBsql Language Reference Manual. It is organized as section files rather than one large manuscript so editors can revise, test, and promote sections independently.

## Directory Map

| Directory | Purpose |
| --- | --- |
| core_paradigms | Architecture and authority rules needed to read the language reference correctly. |
| data_types | Descriptor, domain, conversion, collation, temporal, vector, document, and protected-value mechanics. |
| syntax_reference | Statement syntax, object lifecycles, DML, query clauses, procedural SQL, operational commands, refusal vectors, and per-production EBNF files. |
| functional_reference | Built-in functions, operators, aggregates, windows, and special forms grouped by package namespace. |
| catalog_reference | System catalog tables and schema-tree metadata surfaces. |

## Reading Model

SBsql text is parser input. ScratchBird execution authority is SBLR, UUID catalog identity, descriptors, MGA transaction state, and materialized security policy. Every section in this manual follows that boundary.

## Key Entry Points

| Topic | File |
| --- | --- |
| Syntax reference index | syntax_reference/README.md (SBsql Language Reference — Syntax, page XXX) |
| SBsql language profiles | [core_paradigms/sbsql_language_profiles.md](#ch-language-reference-core-paradigms-sbsql-language-profiles-md) |
| Schema tree and name resolution | syntax_reference/schema_tree_and_name_resolution.md (SBsql Language Reference — Syntax, page XXX) |
| Procedural SQL overview | syntax_reference/procedural_sql.md (SBsql Language Reference — Syntax, page XXX) |
| Procedural blocks and declarations | syntax_reference/procedural_sql_blocks.md (SBsql Language Reference — Syntax, page XXX) |
| Procedural control flow | syntax_reference/procedural_sql_control_flow.md (SBsql Language Reference — Syntax, page XXX) |
| Procedural cursors | syntax_reference/procedural_sql_cursors.md (SBsql Language Reference — Syntax, page XXX) |
| Procedural exceptions and diagnostics | syntax_reference/procedural_sql_exceptions.md (SBsql Language Reference — Syntax, page XXX) |
| Procedural triggers and event capture | syntax_reference/procedural_sql_triggers_and_events.md (SBsql Language Reference — Syntax, page XXX) |

## Generation State

Progress and recovery data is kept in `.generation_state.json` in this directory. The state file is intentionally local to the generated manual tree.




# Core Paradigms




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/core_paradigms/index.md -->

<a id="ch-language-reference-core-paradigms-index-md"></a>

# Core Paradigms Index

The core paradigms chapter explains the architectural rules that underpin every
statement in the SBsql language. Before reading the syntax or functional
reference, it is worth knowing how SBsql text becomes engine work, what MGA
transaction authority means, how catalog identity is stored, and where security
decisions are made. The pages in this chapter answer those questions in
conceptual terms so that the rest of the manual makes sense on first read.

These pages do not document statement syntax. They document the execution model
that syntax must satisfy.

## Pages In This Chapter

| Page | File | Use it for |
| --- | --- | --- |
| Intro And MGA Authority | [intro_and_mga.md](#ch-language-reference-core-paradigms-intro-and-mga-md) | Understanding what SBsql is, why the engine does not run SQL text directly, and what MGA controls. |
| Parser To SBLR Pipeline | [parser_to_sblr_pipeline.md](#ch-language-reference-core-paradigms-parser-to-sblr-pipeline-md) | Following the path from SBsql source text through tokenization, binding, lowering, and server admission to execution. |
| SBsql Language Profiles | [sbsql_language_profiles.md](#ch-language-reference-core-paradigms-sbsql-language-profiles-md) | Understanding how localized keyword spellings and locale-specific tools map onto the same canonical engine work. |
| UUID Catalog Identity | [uuid_catalog_identity.md](#ch-language-reference-core-paradigms-uuid-catalog-identity-md) | Understanding why catalog objects are identified by UUID rather than by name, and what that means for rename, migration, and dependency tracking. |
| Transactions And Recovery | [transactions_and_recovery.md](#ch-language-reference-core-paradigms-transactions-and-recovery-md) | Understanding MGA transaction lifecycle, snapshot rules, commit and rollback finality, and recovery classification. |
| Security And Sandboxing | [security_and_sandboxing.md](#ch-language-reference-core-paradigms-security-and-sandboxing-md) | Understanding how identity, roles, grants, sandbox roots, policy, masking, and fail-closed refusal fit together. |
| Bridge And Cluster Boundaries | [bridge_and_cluster_boundaries.md](#ch-language-reference-core-paradigms-bridge-and-cluster-boundaries-md) | Understanding the difference between local operations, bridge operations, and cluster-classified operations. |

## Suggested Reading Order

For a first reading, work through the pages in the order shown in the table
above. `intro_and_mga.md` introduces the vocabulary the other pages depend on.
`parser_to_sblr_pipeline.md` makes the flow concrete. The remaining pages can
be read in any order once those two are clear.

Readers who come to this chapter from an operational context (asking "why did
my statement get refused?") may want to go directly to `security_and_sandboxing.md`
or `transactions_and_recovery.md`.

## Back To The Chapter List

[Language Reference](#ch-language-reference-readme-md)




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/core_paradigms/intro_and_mga.md -->

<a id="ch-language-reference-core-paradigms-intro-and-mga-md"></a>

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

![diagram](/home/dcalford/CliWork/ScratchBird/docs/documentation/draft/Language_Reference/core_paradigms/intro_and_mga-1.svg)

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

![diagram](/home/dcalford/CliWork/ScratchBird/docs/documentation/draft/Language_Reference/core_paradigms/intro_and_mga-2.svg)

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

See [UUID Catalog Identity](#ch-language-reference-core-paradigms-uuid-catalog-identity-md) and
Schema Tree And Name Resolution (SBsql Language Reference — Syntax, page XXX)
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

See [Type System Overview](#ch-language-reference-data-types-type-system-overview-md),
[Numeric Types](#ch-language-reference-data-types-numeric-types-md), and
[Conversion Matrix](#ch-language-reference-data-types-conversion-matrix-md).

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

See [Transactions And Recovery](#ch-language-reference-core-paradigms-transactions-and-recovery-md) and
Transaction Control (SBsql Language Reference — Syntax, page XXX).

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

See [Security And Sandboxing](#ch-language-reference-core-paradigms-security-and-sandboxing-md),
Security And Privileges (SBsql Language Reference — Syntax, page XXX),
and Policy, Mask, And RLS Lifecycle (SBsql Language Reference — Syntax, page XXX).

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

See Refusal Vectors (SBsql Language Reference — Syntax, page XXX).

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

- [Parser To SBLR Pipeline](#ch-language-reference-core-paradigms-parser-to-sblr-pipeline-md)
- [UUID Catalog Identity](#ch-language-reference-core-paradigms-uuid-catalog-identity-md)
- [Transactions And Recovery](#ch-language-reference-core-paradigms-transactions-and-recovery-md)
- [Security And Sandboxing](#ch-language-reference-core-paradigms-security-and-sandboxing-md)
- Schema Tree And Name Resolution (SBsql Language Reference — Syntax, page XXX)
- Transaction Control (SBsql Language Reference — Syntax, page XXX)
- Security And Privileges (SBsql Language Reference — Syntax, page XXX)
- Policy, Mask, And RLS Lifecycle (SBsql Language Reference — Syntax, page XXX)
- Refusal Vectors (SBsql Language Reference — Syntax, page XXX)
- [Type System Overview](#ch-language-reference-data-types-type-system-overview-md)




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/core_paradigms/parser_to_sblr_pipeline.md -->

<a id="ch-language-reference-core-paradigms-parser-to-sblr-pipeline-md"></a>

# Parser To SBLR Pipeline

This page is part of the SBsql Language Reference Manual. It describes how
SBsql text becomes SBLR, how that SBLR is admitted by the server boundary, and
what the engine treats as authority. The pipeline exists so the parser can
remain expressive and context sensitive while the engine executes one canonical
command language.

Generation task: `core_paradigms_parser_to_sblr`

## Purpose

When you write an SBsql statement, the engine never runs the text you typed.
Instead, a parser session translates that text through a defined pipeline —
tokenize, bind names to UUID catalog objects, lower the result into a structured
`SBLRExecutionEnvelope` — and only then submits the envelope to the server for
admission and execution. SBLR (ScratchBird Logical Representation) is the
binary operation language the engine understands; SBsql text is the human-facing
layer on top of it.

This separation is a core ScratchBird rule:

- SQL text is user input, diagnostic evidence, and source-reference material.
- Catalog names are user-layer references until they are bound to UUID catalog
  identity.
- Type names, literal forms, parameters, row shapes, stream contracts, and
  result shapes become descriptor-bound SBLR data.
- Parser output is never transaction finality, storage finality, authorization
  finality, or catalog authority.
- The engine can reject a syntactically valid statement if binding, admission,
  authorization, policy, transaction state, storage state, or resource limits do
  not permit execution.

A successful statement therefore passes three user-visible phases: parse,
bind/admit, and execute. A failure in any phase returns a message vector instead
of continuing with guessed behavior.

## Pipeline At A Glance

![diagram](/home/dcalford/CliWork/ScratchBird/docs/documentation/draft/Language_Reference/core_paradigms/parser_to_sblr_pipeline-1.svg)

The same pipeline applies whether the client uses an embedded engine path, a
local IPC server, or a network path through the manager, listener, parser, IPC
server, and engine. Network deployment adds transport and handoff boundaries;
it does not change the requirement that the engine receives canonical SBLR
rather than SQL text.

## Stage 1: Tokenization

The tokenizer reads the SBsql script under the active session profile. SBsql is
context sensitive, so most words are interpreted by context rather than by a
large global reserved-word list. This lets identifiers use ordinary business
terms while still allowing command clauses to be recognized where the grammar
expects them.

When an SBsql language profile is selected, profile-specific spelling,
topology, and diagnostic resources are normalized here into the canonical
element stream. UUID binding still happens after that normalization step; the
localized words themselves are not engine authority.

Tokenization preserves:

- source spans for diagnostics and support bundles;
- quoted identifier spelling;
- string, binary, numeric, temporal, UUID, document, graph, vector, and protected
  value literal forms;
- parameter marker positions;
- statement separators;
- comments, when comment retention is enabled for diagnostic or source-reference
  output.

Tokenization does not resolve object identity. A token such as `orders` is only
text until the binding phase determines which visible object, if any, it names.

## Stage 2: Concrete Syntax Tree

The parser builds a concrete syntax tree that is lossless enough to render
precise diagnostics. The concrete tree records the user's spelling, clause
order, nesting, and token spans. It is the right structure for error reporting
and source annotation, but it is not the structure that the engine executes.

Examples of concrete syntax decisions include:

- whether a statement is a query, DML statement, DDL statement, transaction
  statement, procedural statement, security statement, management statement,
  multimodel statement, stream statement, or recognized refusal route;
- where each clause starts and ends;
- which expressions appear in projection, filtering, grouping, windowing,
  ordering, DML assignments, defaults, constraints, policies, and procedural
  blocks;
- whether a token sequence is ambiguous and needs binding context to decide its
  meaning.

Syntax errors stop here. The parser returns a message vector containing the
source span, expected grammar context, and statement boundary when possible.

## Stage 3: Statement AST

The parser then builds a statement AST. The AST removes purely textual
structure, classifies the statement family, and prepares the statement for
binding. It still contains source spans for diagnostics, but the main purpose is
semantic organization.

The AST records:

- statement family and subfamily;
- object references, aliases, CTE names, correlation names, and local block
  variables;
- expression trees and predicate trees;
- DDL lifecycle actions such as create, alter, recreate, rename, comment,
  describe, show, and drop;
- DML source and target structure;
- rowset, scalar, cursor, stream, and command-completion expectations;
- procedural blocks, variables, cursor operations, handlers, triggers, and
  event bindings;
- security, policy, management, backup, restore, replication, migration,
  multimodel, and gated operation intent.

The AST is still parser-owned. It cannot create durable state, open server-local
files, commit transactions, bypass policy, or treat display names as engine
identity.

## Stage 4: Binding

Binding converts parser-owned semantic intent into ScratchBird identity and type
evidence. This is the first phase where names become durable catalog references.

Binding resolves:

- database, schema, object, column, domain, type, index, sequence, view,
  procedure, function, trigger, package, role, policy, agent, stream, and
  filespace names to UUID catalog identity;
- the active schema root, home schema, current schema, recursive schema lookup
  rules, sandbox root, and visibility branch;
- aliases, CTE names, recursive CTE columns, local variables, parameters,
  transition variables, cursor variables, and row fields;
- overload selection for functions, procedures, operators, casts, aggregates,
  and procedural calls;
- literal descriptors, parameter descriptors, row descriptors, record
  descriptors, collection descriptors, stream descriptors, and result
  descriptors;
- expression input types, nullability, collation, character set, scale,
  precision, temporal zone behavior, binary length, vector dimensions, document
  shape, graph shape, and protected-material policy labels;
- transaction context, savepoint context, autocommit behavior, isolation
  request, retain/chain behavior, and execution timeout;
- security context, invoker/definer behavior, row-level policy inputs, masking
  inputs, and sandbox constraints.

After binding, an executable operation must not depend on display-name lookup
inside the engine. The operation carries UUID-bound references and descriptors.
The engine may still reject those references if the referenced object has
changed, become invisible, been revoked, requires recovery, or no longer
matches the bound descriptor contract.

## Stage 5: Lowering To SBLR

Lowering converts the bound AST into an `SBLRExecutionEnvelope`. The envelope is
the parser-to-server command carrier. It is versioned, canonical, and explicit
about what the engine is being asked to do.

An execution envelope contains the following classes of information:

| Field Class | Purpose |
| --- | --- |
| Envelope version | Identifies the SBLR envelope contract accepted by server admission. |
| Operation identity | Identifies the SBLR operation and statement family to dispatch. |
| UUID references | Carries bound catalog identity for objects, columns, descriptors, policies, agents, streams, and filespaces. |
| Descriptor table | Defines parameter, literal, expression, row, cursor, stream, and result shapes. |
| Argument table | Carries typed values, parameter slots, default requests, and expression operands. |
| Scope table | Carries CTE, alias, local variable, cursor, and procedural block scope bindings. |
| Transaction request | Carries begin, commit, rollback, savepoint, retain, chain, timeout, and isolation intent where relevant. |
| Security request | Carries invoker/definer, principal UUID, role context, policy context, and sandbox evidence. |
| Stream contract | Defines cursor, large value, copy/load, logical backup/restore, CDC, replication, migration, and result streaming behavior. |
| Diagnostic shape | Defines how refusals, warnings, row counts, command tags, and support evidence are returned. |
| Source evidence | Carries source spans, normalized source digests, and non-authoritative source references for diagnostics. |

The envelope must not treat raw SQL text as engine authority. Source text may be
retained for diagnostics or reference, but the executable request is the SBLR
payload.

## Stage 6: Server Admission

Server admission is a fail-closed validation boundary between parser output and
engine execution. It rejects malformed, ambiguous, stale, or unauthorized
envelopes before dispatch.

Admission validates:

- the `SBLRExecutionEnvelope` version;
- operation identity, operation family, and opcode consistency;
- result shape and diagnostic shape;
- UUID-bound object references and required descriptor references;
- that parser-resolved names have been converted to UUID references;
- that the envelope contains no SQL text as an executable command;
- that authority fields are not duplicated or contradictory;
- that family-only routing is not used as final dispatch authority;
- that public ABI dispatch metadata is present where required;
- that gated operation surfaces fail with the correct message vector when not
  enabled or not licensed;
- that unsupported, denied, unlicensed, and unavailable-capability routes return
  explicit refusals rather than silent fallthrough.

Admission success does not mean the operation will complete. It means the
request is well formed enough for engine-level authorization, policy,
transaction, storage, and resource checks.

## Stage 7: Engine Execution

The engine dispatches the admitted SBLR operation by operation identity. The
engine owns durable state, MGA transaction authority, catalog mutation,
visibility, cleanup, storage, index maintenance, large-value storage, optimizer
execution, authorization enforcement, policy enforcement, and recovery fences.

Execution rules:

- MGA decides transaction visibility, commit, rollback, savepoint release,
  retention, cleanup, and recovery state.
- Catalog UUID identity is the durable identity. Names remain display and
  resolution material.
- Indexes, optimizer plans, statistics, cached plans, and parser claims are
  evidence, not final row authority.
- Authorization and policy are rechecked at the engine boundary.
- Durable metadata updates are versioned and integrity checked.
- Uncertain recovery or inconsistent durable state fails closed.
- Result rows, command completion, warnings, refusals, and diagnostics are
  returned through an execution result envelope.

## Result Contracts

Each operation declares the result shape it expects. The parser renders the
result according to that shape; it does not infer success by rereading the
original SQL text.

Common result contracts are:

| Result Contract | Returned Form |
| --- | --- |
| Command completion | Command tag, affected object UUID, row count where applicable, warnings, and message vector. |
| Rowset | Descriptor-bound rows with column UUIDs, aliases, type descriptors, nullability, collation, and cursor metadata. |
| Scalar | One value plus its descriptor and diagnostic context. |
| Cursor | Cursor handle, descriptor, snapshot or transaction context, fetch policy, and close behavior. |
| Stream | Stream handle, frame limits, backpressure policy, cancellation behavior, and completion status. |
| Diagnostic/refusal | Message vector, refusal class, source span, object UUID when safe to reveal, and remediation hint when available. |
| Support/metadata | Redacted support evidence, catalog summaries, plan summaries, policy summaries, or health summaries. |

## Message Vector Classes

The pipeline reports failures at the phase that detects them. Message vectors
must be explicit enough that an operator or client can tell whether the problem
is syntax, binding, admission, authorization, policy, resource state, or engine
execution.

| Phase | Typical Message Vector Class |
| --- | --- |
| Tokenization | Invalid token, unterminated literal, invalid escape, invalid numeric form, invalid protected-value marker. |
| Parsing | Unexpected token, missing clause, invalid statement boundary, invalid procedural block, invalid expression grammar. |
| Binding | Unknown object, ambiguous object, invalid scope, unresolved overload, descriptor mismatch, invalid recursive CTE shape. |
| Admission | Unsupported envelope version, missing operation identity, SQL text forbidden, UUID binding required, result-shape mismatch. |
| Authorization | Permission denied, sandbox denied, role inactive, policy denied, masked value unavailable, protected material denied. |
| Transaction | No active transaction, invalid savepoint, isolation conflict, transaction requires recovery, commit refused, rollback refused. |
| Resource | File access denied, stream limit exceeded, timeout, cancellation, memory pressure, filespace unavailable, disk policy refused. |
| Execution | Constraint violation, duplicate key, data exception, arithmetic exception, object changed, recovery required, fail-closed refusal. |
| Gated operation | Unsupported, unlicensed, unavailable capability, unavailable provider, compile-time disabled route. |

## Example: Query Lowering

The following query uses SBsql syntax and a parameter marker:

```sql
select
    order_id,
    total
from app.orders
where total > cast(:minimum_total as decimal(18,2))
order by order_id;
```

Conceptually, the pipeline produces:

| Source Fragment | Bound Evidence | SBLR Carrier |
| --- | --- | --- |
| `app.orders` | Table UUID plus visible schema branch. | UUID object reference. |
| `order_id` | Column UUID, type descriptor, collation if applicable. | Projection descriptor entry. |
| `total` | Column UUID, numeric descriptor, nullability. | Projection and predicate descriptor entries. |
| `:minimum_total` | Parameter slot, expected numeric descriptor. | Parameter descriptor entry. |
| `cast(... as decimal(18,2))` | Conversion target descriptor and conversion operation. | Expression operation entry. |
| `total > ...` | Boolean predicate with numeric comparison semantics. | Predicate operation entry. |
| `order by order_id` | Sort key descriptor and collation/null-order rules. | Ordering descriptor entry. |

The engine does not execute the text `app.orders` or `order_id`. It receives a
UUID-bound operation with descriptors, validates the request, executes it under
the active transaction and authorization context, and returns a rowset envelope.

## Example: DDL Lowering

```sql
create table app.invoice_line (
    invoice_line_id uuid primary key,
    invoice_id uuid not null,
    line_no int not null,
    amount decimal(18,2) not null
);
```

DDL binding resolves the target schema UUID, validates that the caller may
create a table in that schema, resolves type descriptors for every column,
creates constraint descriptors, and lowers the request to a catalog-mutation
SBLR operation. The engine performs the durable catalog update inside the active
transaction and returns command completion. If any descriptor, constraint,
authorization, filespace, recovery, or catalog-version check fails, the command
is refused.

## Non-Authority Rules

The parser must not:

- execute SQL text inside the engine;
- treat names as durable identity after binding;
- decide transaction finality;
- create or modify durable storage outside an admitted engine operation;
- open or manipulate server-local files unless the admitted operation explicitly
  permits that behavior under policy;
- grant itself authorization or bypass row-level policy;
- treat an optimizer plan, index candidate, or statistics record as final row
  authority;
- convert a recognized unsupported command into best-effort behavior;
- silently ignore statement clauses that affect semantics;
- hide a gated capability refusal behind a generic parse error.

## Syntax Productions

This page summarizes the high-level productions used by the pipeline. Detailed
statement pages and EBNF pages define the individual command surfaces.

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
bound_statement         ::= statement_ast
                            catalog_uuid_bindings
                            descriptor_bindings
                            scope_bindings
                            transaction_context
                            security_context ;
```

```ebnf
sblr_execution_request  ::= envelope_version
                            operation_identity
                            uuid_reference_table
                            descriptor_table
                            argument_table
                            scope_table
                            result_contract
                            diagnostic_contract ;
```

```ebnf
execution_result        ::= command_completion
                          | rowset_result
                          | scalar_result
                          | cursor_result
                          | stream_result
                          | diagnostic_result ;
```

## Pipeline Surface Map

| Surface | Kind | Pipeline Role | SBLR Or Result Expectation |
| --- | --- | --- | --- |
| `script` | Grammar production | Defines batch and statement boundaries. | No execution until each statement is parsed and bound. |
| `statement` | Grammar production | Selects native execution or explicit refusal. | Operation identity or refusal vector. |
| `qualified_name` | Grammar production | Supplies user-layer object reference text. | UUID-bound object reference after binding. |
| `uuid_reference` | Grammar production | Supplies explicit identity evidence. | Object class, visibility, and authorization still rechecked. |
| `parameter_marker` | Grammar production | Names a client-supplied value slot. | Parameter descriptor entry. |
| `literal` | Grammar production | Supplies a source literal. | Typed value descriptor and canonical value representation. |
| `expression` | Grammar production | Builds a scalar, row, document, graph, vector, predicate, or procedural expression. | Descriptor-bound expression SBLR. |
| `query_dml_stmt` | Statement family | Requests a rowset, scalar, cursor, or stream. | Query operation envelope and result descriptor. |
| `dml` | Statement family | Requests MGA-governed insert, update, delete, merge, upsert, load, or return rows. | DML operation envelope and affected-row/result contract. |
| `ddl_catalog` | Statement family | Requests a catalog mutation or object lifecycle command. | Catalog operation envelope and command completion. |
| `transaction_statement` | Statement family | Requests begin, commit, rollback, savepoint, retain, chain, or transaction setting. | MGA transaction operation and finality result. |
| `dcl_security_stmt` | Statement family | Requests identity, role, grant, revoke, policy, masking, or protected-material operation. | Security operation envelope and authorization result. |
| `management_stmt` | Statement family | Requests diagnostic, health, configuration, support, agent, or operational action. | Management operation envelope or explicit refusal. |
| `refusal_statement` | Statement family | Recognizes a command shape that must not execute. | Explicit unsupported, denied, unlicensed, or unavailable message vector. |

## Verification Checklist

A parser-to-SBLR proof should demonstrate all of the following for each covered
statement family:

- valid source parses successfully;
- invalid source fails at parse with a precise message vector;
- contextual words can be identifiers outside command contexts;
- object names bind to UUID catalog identity;
- unresolved and ambiguous names fail during binding;
- parameter and literal descriptors match the expected expression types;
- result shapes are declared before admission;
- raw SQL text is not executable authority in the envelope;
- server admission rejects malformed or incomplete envelopes;
- authorization and policy are rechecked after admission;
- transaction state is owned by MGA execution;
- engine dispatch occurs by operation identity;
- result envelopes match the declared result contract;
- unsupported, denied, unlicensed, and unavailable surfaces return explicit
  message vectors;
- the proof is part of the normal project test suite.

## Related Reference Pages

- [Intro And MGA](#ch-language-reference-core-paradigms-intro-and-mga-md)
- [SBsql Language Profiles](#ch-language-reference-core-paradigms-sbsql-language-profiles-md)
- [UUID Catalog Identity](#ch-language-reference-core-paradigms-uuid-catalog-identity-md)
- [Transactions And Recovery](#ch-language-reference-core-paradigms-transactions-and-recovery-md)
- [Security And Sandboxing](#ch-language-reference-core-paradigms-security-and-sandboxing-md)
- Script Tokens And Identifiers (SBsql Language Reference — Syntax, page XXX)
- Schema Tree And Name Resolution (SBsql Language Reference — Syntax, page XXX)
- Refusal Vectors (SBsql Language Reference — Syntax, page XXX)
- [Type System Overview](#ch-language-reference-data-types-type-system-overview-md)
- [Conversion Matrix](#ch-language-reference-data-types-conversion-matrix-md)
- Projection (SBsql Language Reference — Syntax, page XXX)
- Transaction Control (SBsql Language Reference — Syntax, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/core_paradigms/sbsql_language_profiles.md -->

<a id="ch-language-reference-core-paradigms-sbsql-language-profiles-md"></a>

# SBsql Language Profiles

This page is part of the SBsql Language Reference Manual. It explains how
localized SBsql source, standard English fallback, local predictive resources,
and SBLR-to-SBsql rendering fit into the parser-to-SBLR model.

Generation task: `core_paradigms_sbsql_language_profiles`

## Purpose

SBsql is designed so that the _syntax_ a user writes can look different from
locale to locale while the _work_ the engine executes stays identical. That
separation is achieved through language profiles.

A language profile is a parser resource pack. It contains keyword spellings,
phrase structures, diagnostic messages, predictive-completion tables, and
renderer templates for one exact locale (for example `fr-CA` or `en-GB`). When
a session loads a profile, the parser accepts the localized spellings and
translates them into a canonical element stream before binding and execution.
The engine never sees the original locale-specific text.

The core pipeline rule is:

```text
localized SBsql source -> canonical element stream -> UUID binding -> SBLR
```

The engine executes admitted SBLR with UUID identity, descriptors, security
policy, and MGA transaction authority. It does not execute localized source
text. A language profile therefore controls user experience and tooling surface
only; it cannot change what work the engine authorizes or how it records that
work.

## What A Language Profile Can Change

A language profile may define:

- keyword and phrase aliases;
- clause and sentence order for a specific locale;
- localized diagnostic and message-vector text;
- localized object-label rendering where the catalog contains admitted labels;
- predictive-text tables for tools, drivers, and editors;
- SBLR-to-SBsql renderer templates;
- locale-specific literal rules where the profile explicitly admits them.

The profile is exact. For example, a profile for `fr-CA` is not the same as a
profile for `fr-FR`, and a profile for `en-US` is not the same as `en-GB`.

## What A Language Profile Cannot Change

A language profile cannot change:

- canonical SBsql surface IDs;
- SBLR operation families;
- UUID catalog identity;
- descriptor identity;
- authorization and disclosure policy;
- transaction visibility, commit, rollback, savepoint, cleanup, or recovery;
- storage, index, page, or filespace behavior;
- whether a feature is implemented, enabled, or release-supported.

If a localized statement parses successfully but binding or engine admission
fails, the request is refused in the same way as canonical SBsql.

## Canonical Element Stream

The parser normalizes localized source before UUID resolution. It does not
rewrite the text into English SQL. Instead, it emits a canonical element stream:

![diagram](/home/dcalford/CliWork/ScratchBird/docs/documentation/draft/Language_Reference/core_paradigms/sbsql_language_profiles-1.svg)

The stream records canonical token IDs, surface IDs, command slots, source
spans, profile identities, resource hashes, and resolver inputs. That gives the
binder stable input without making localized words into engine authority.

## Standard SBsql Fallback

Many applications and tools will continue to generate canonical English SBsql
without knowing that a user has selected another preferred language. A
non-English session may therefore accept standard SBsql as an input syntax
fallback when policy enables it and the preferred language profile does not
parse the statement.

Fallback has two important limits:

- the user's preferred rendering language remains selected;
- the canonical element stream records that the input syntax profile was
  standard English fallback.

If a statement is ambiguous between the preferred profile and fallback, the
parser must refuse it unless the caller supplies an explicit syntax profile.

## Rendering SBLR Back To SBsql

SBLR-to-SBsql conversion should render in the preferred language when an
admitted renderer template exists and the rendered text can round-trip to the
same canonical element stream, SBLR operation family, and UUID resolver inputs.

If preferred-language rendering is not available, the converter falls back to
canonical English SBsql with an explicit fallback reason. If neither rendering
path is safe, it returns a not-renderable diagnostic rather than inventing
source.

Rendering is canonical source generation. It is not guaranteed to reproduce the
original source text exactly.

## Shared Resources For Tools And Drivers

Parser workers, drivers, command-line tools, driver-equivalent adaptors, IDE
integrations, and AI assistants must use the same admitted SBsql resource pack
for local SBsql intelligence. That pack contains the language profiles,
topology profiles, dialect profile metadata, predictive tables, diagnostics,
and renderer templates needed for local syntax help and draft SBLR creation.

Local parsing, local completion, and draft SBLR remain client-side assistance.
The server still revalidates SBLR, UUIDs, descriptors, grants, resource hashes,
language profile identity, and MGA transaction context before execution.

## Security And Privacy

Language profiles and predictive resources must not reveal hidden objects or
protected material. Completion should distinguish grammar help from object-name
completion. Object-name completion must use authorized metadata or server
resolver results.

Confusable characters, mixed scripts, bidirectional controls, transliteration
aliases, mirrored punctuation, and localized/default-name collisions must fail
closed unless an explicit policy admits the case.

## Release Status

This page describes the architecture and release-scoped behavior. It does not
claim that every language profile, tool, driver, adaptor, or renderer is
release-supported in every build. Check the release evidence, admitted resource
manifest, and current test gates for the exact profiles available in a build.

## Where To Go Next

- [Parser To SBLR Pipeline](#ch-language-reference-core-paradigms-parser-to-sblr-pipeline-md)
- [UUID Catalog Identity](#ch-language-reference-core-paradigms-uuid-catalog-identity-md)
- Script Tokens And Identifiers (SBsql Language Reference — Syntax, page XXX)
- Schema Tree And Name Resolution (SBsql Language Reference — Syntax, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/core_paradigms/uuid_catalog_identity.md -->

<a id="ch-language-reference-core-paradigms-uuid-catalog-identity-md"></a>

# UUID Catalog Identity

This page is part of the SBsql Language Reference Manual. It explains how
ScratchBird identifies catalog objects, how user-facing names bind to durable
UUID identity, and why engine execution depends on UUID references and
descriptors rather than display text.

Generation task: `core_paradigms_uuid_catalog_identity`

## Purpose

ScratchBird catalog identity is UUID based. User-facing names, localized names,
aliases, synonyms, path labels, and SBsql-visible spellings are resolver input.
They are not durable identity.

When an SBsql statement names an object, binding resolves that name to an object
UUID, object class, parent schema UUID, catalog generation, security epoch, and
descriptor evidence. The parser then lowers the bound operation into SBLR. The
engine executes the SBLR request against UUID identity and descriptors.

This model lets ScratchBird support rename, localized names, aliases, recursive
schemas, schema sandboxes, catalog projections, migrations, support
diagnostics, dependency tracking, and cache invalidation without confusing a
spelling with the object itself.

## Core Rule

> Names are for users and scripts. UUIDs are for durable identity.

Examples:

- renaming a table changes resolver metadata, not the table's durable identity;
- commenting on an object changes descriptive metadata, not identity;
- moving an object where the lifecycle permits it changes parent or placement
  metadata, not identity unless the operation explicitly creates a replacement;
- dropping an object retires the object identity under catalog lifecycle rules;
- recreating an object creates a new durable identity unless the statement is
  explicitly an admitted metadata mutation of the existing object;
- dependencies are tracked by UUID, not by the text that happened to name the
  object when the dependency was created.

## Identity Flow

![diagram](/home/dcalford/CliWork/ScratchBird/docs/documentation/draft/Language_Reference/core_paradigms/uuid_catalog_identity-1.svg)

The resolver context includes current schema, home schema, search path, default
root, sandbox root, identifier profile, language profile, active transaction,
security epoch, and policy state.

## What Has UUID Identity

Durable catalog objects use UUID identity. The visible name of an object is a
label over that identity.

| Object Family | UUID Identity Applies To |
| --- | --- |
| Database and filespaces | Database identity, filespace identity, placement metadata, filespace lifecycle records. |
| Schemas | Root schemas, recursive child schemas, home schemas, workarea roots, system branches, remote roots. |
| Relations | Tables, temporary tables, views, materialized views, external rowsets, relation descriptors. |
| Relation members | Columns, generated columns, constraints, indexes, row descriptors, storage descriptors. |
| Routines | Functions, procedures, packages, package members, triggers, trigger events, compiled routine bodies. |
| Type system | Type descriptors, domains, domain elements, casts, operators, collations, character sets. |
| Security | Users, roles, groups, grants, policies, masks, RLS rules, protected-material descriptors. |
| Operations | Agents, streams, bridge descriptors, replication descriptors, migration descriptors, support-bundle descriptors. |
| Catalog metadata | Comments, aliases, localized names, dependencies, invalidation records, audit records. |

A result may render a name, but the bound object remains the UUID. A diagnostic
may show both when disclosure policy admits it.

## Names, Paths, And Labels

ScratchBird supports several name forms. All are resolver inputs:

| Name Form | Meaning |
| --- | --- |
| Primary name | The ordinary display and lookup name for an object in a schema scope. |
| Qualified name | A path of names resolved through parent schema UUIDs. |
| Alias or synonym | Additional resolver label that points to a durable object UUID where policy admits it. |
| Localized name | Language/profile-specific display name or lookup label. |
| Quoted identifier | Exact spelling that participates in the active identifier profile. |
| Unquoted identifier | Context-sensitive word folded according to the active identifier profile. |
| UUID reference | Direct identity reference that bypasses name search but still requires visibility, class, and authorization checks. |

Qualified names are path labels, not identity. In `app.orders`, `app` resolves
to a schema UUID, then `orders` resolves inside that parent UUID.

See Schema Tree And Name Resolution (SBsql Language Reference — Syntax, page XXX).

## UUID References

UUID references are useful for automation, migration, support diagnostics, and
scripts that must avoid ambiguity. A UUID reference is still subject to object
class, transaction visibility, sandbox, authorization, policy, and recovery
checks.

```sql
describe table uuid '019d0000-0000-7000-8000-000000000001';
```

The UUID literal above is illustrative. Real UUID values are assigned and
validated by the engine.

UUID references do not use the search path. They do not bypass security. A user
who knows an object UUID still cannot access the object unless the effective
security context is authorized and disclosure policy admits the operation.

## Resolver Evidence

Binding a name produces evidence that can be validated by admission and engine
execution.

| Evidence | Purpose |
| --- | --- |
| Object UUID | Durable identity of the resolved object. |
| Object class | Confirms that the resolved object is the expected class, such as table, view, domain, function, or role. |
| Parent schema UUID | Establishes namespace, sandbox, dependency, and lifecycle context. |
| Descriptor UUID | Identifies type, row, parameter, result, stream, policy, or storage descriptors where needed. |
| Catalog generation | Detects stale bindings after DDL or catalog lifecycle changes. |
| Security epoch | Detects stale grants, revokes, roles, groups, policies, masks, and RLS state. |
| Name-resolution epoch | Detects stale aliases, renames, localized labels, search path, and schema resolver state. |
| Resource epoch | Detects stale filespace, stream, bridge, placement, or operational resource state. |
| Source span | Lets diagnostics point back to the user's text without treating that text as authority. |

Prepared statements, compiled routines, cached plans, metadata projections, and
support diagnostics use this evidence to decide whether reuse is safe or
revalidation is required.

## Lifecycle Effects

Object lifecycle statements affect identity differently depending on what they
do.

| Operation | Identity Effect |
| --- | --- |
| `CREATE` | Allocates a new object UUID and records parent, owner, descriptor, dependency, and lifecycle metadata. |
| `ALTER` | Mutates admitted metadata or descriptors for the existing object UUID unless the specific action creates a child or replacement object. |
| `RENAME` | Changes resolver labels. Durable UUID identity remains stable. |
| `COMMENT ON` | Changes descriptive metadata. Durable UUID identity remains stable. |
| `DESCRIBE` | Reads authorized metadata for the bound UUID. No identity change. |
| `SHOW` | Reads authorized projections. No identity change. |
| `VALIDATE` | Checks descriptors, dependencies, policy, or storage readiness. No identity change unless a documented repair route is admitted. |
| `RECREATE` | Drops and creates through an explicit lifecycle route. The replacement object receives a new UUID unless the reference page for that object states a narrower admitted behavior. |
| `DROP` | Retires the object identity subject to dependencies, transaction finality, recovery safety, and retention policy. |
| `RESTORE` or `IMPORT` | Maps incoming logical identity to admitted ScratchBird UUID identity according to the operation's mapping policy. |

DDL becomes visible according to MGA transaction finality. A created object is
not visible to other transactions until commit rules make it visible. A dropped
or renamed object can remain visible to existing valid snapshots according to
the same transaction rules.

## Identity And Dependencies

Dependencies are recorded against UUID identity and descriptor identity. This
prevents rename from breaking dependent objects and lets the engine invalidate
or refuse stale work precisely.

Common dependency edges include:

| Dependent | Referenced Identity |
| --- | --- |
| View | Tables, columns, functions, domains, collations, policies. |
| Materialized view | Source rowsets, refresh policy, storage descriptors, indexes. |
| Procedure or function | Referenced tables, routines, domains, types, packages, variables, result descriptors. |
| Trigger | Target relation, timing/event descriptor, transition descriptors, called routines. |
| Constraint | Table, column, domain, index, referenced relation, expression descriptors. |
| Index | Target relation, key expressions, collation, null behavior, storage descriptor. |
| Policy, mask, or RLS | Target object, security context inputs, expression descriptors, protected-material descriptors. |
| Prepared statement | Object UUIDs, descriptors, security epoch, name-resolution epoch, result shape. |

When an object changes, dependent state must be invalidated, revalidated,
rebuilt, or refused. It must not continue using stale name text.

## Identity And Security

Security applies to the resolved UUID. The resolver must also enforce sandbox
and metadata visibility before returning the UUID to the operation.

Security rules:

- a hidden object may render as not found when policy requires metadata hiding;
- a UUID reference does not bypass grants, roles, policy, masks, RLS, or
  protected-material rules;
- catalog projections may reveal selected metadata without granting direct
  traversal or object access;
- result envelopes and diagnostics may redact object UUIDs, names, parent paths,
  policy names, and dependency details;
- grants and revokes advance the security epoch and invalidate dependent
  bindings.

See [Security And Sandboxing](#ch-language-reference-core-paradigms-security-and-sandboxing-md).

## Identity And Transactions

Catalog identity participates in MGA. Creating, renaming, altering, dropping, or
retiring an object is a transaction-governed catalog operation.

![diagram](/home/dcalford/CliWork/ScratchBird/docs/documentation/draft/Language_Reference/core_paradigms/uuid_catalog_identity-2.svg)

The UUID may be allocated before commit, but its user-visible finality depends
on transaction outcome. If the transaction rolls back, the object is not visible
as a committed catalog object.

See [Transactions And Recovery](#ch-language-reference-core-paradigms-transactions-and-recovery-md).

## Identity In SBLR

SBLR envelopes carry UUID references and descriptors. They do not ask the engine
to execute names.

For a query such as:

```sql
select order_id, total
from app.orders
where total > cast(:minimum_total as decimal(18,2));
```

binding produces:

| Source Text | Bound Identity |
| --- | --- |
| `app` | Schema UUID. |
| `orders` | Table UUID under the schema UUID. |
| `order_id` | Column UUID and type descriptor. |
| `total` | Column UUID and numeric descriptor. |
| `:minimum_total` | Parameter descriptor slot. |
| `decimal(18,2)` | Numeric descriptor. |

The resulting SBLR request carries operation identity, UUID references,
descriptor entries, parameter slots, predicate operations, result shape, and
diagnostic shape. SQL text may be retained as non-authoritative source evidence.

See [Parser To SBLR Pipeline](#ch-language-reference-core-paradigms-parser-to-sblr-pipeline-md).

## Identity Rendering

SBsql can render identity in different ways depending on command and disclosure
policy.

| Surface | Typical Rendering |
| --- | --- |
| `SHOW` | Lists authorized objects with names and selected metadata. UUIDs may be hidden or shown according to policy. |
| `DESCRIBE` | Shows one object's authorized descriptor, parent, dependencies, lifecycle state, and comments. |
| Catalog views | Return policy-filtered metadata rows. |
| Support diagnostics | Return redacted identity evidence suitable for troubleshooting. |
| Error messages | Show names, UUIDs, both, or neither according to disclosure policy. |

Rendering is not authority. It is an authorized projection over catalog state.

## Examples

Create an object with a user-facing name:

```sql
create schema app;

create table app.orders (
    order_id uuid primary key,
    total decimal(18,2) not null
);
```

The engine assigns UUID identity to the schema, table, columns, constraints, and
descriptors.

Rename the table without changing its durable identity:

```sql
rename table app.orders to app.sales_order;
```

Describe by name:

```sql
describe table app.sales_order;
```

Describe by UUID when an automation script has recorded the object identity:

```sql
describe table uuid '019d0000-0000-7000-8000-000000000001';
```

Comment on an object without changing identity:

```sql
comment on table app.sales_order is 'Application order table';
```

Drop the object through a transaction-governed lifecycle route:

```sql
drop table app.sales_order restrict;
```

## Syntax Productions

```ebnf
object_ref              ::= uuid_reference
                          | qualified_name ;
```

```ebnf
qualified_name          ::= name_part ("." name_part)* ;
```

```ebnf
uuid_reference                ::= "UUID" string_literal ;
```

```ebnf
name_part               ::= identifier
                          | delimited_identifier ;
```

```ebnf
identity_rendering      ::= show_stmt
                          | describe_stmt
                          | catalog_query
                          | support_diagnostic ;
```

## Binding And Execution Summary

| Step | Identity Rule |
| --- | --- |
| Parse | Object references are still text or UUID literal tokens. |
| Resolve | Names bind through schema, search path, sandbox, and metadata visibility rules. |
| Validate class | The resolved UUID must match the object class required by the statement. |
| Bind descriptors | Type, row, column, parameter, stream, policy, and result descriptors are attached. |
| Lower | SBLR carries UUID references and descriptors, not executable names. |
| Admit | Server admission rejects missing, ambiguous, stale, or class-invalid identity evidence. |
| Authorize | Security and policy check the resolved UUID and operation descriptor. |
| Execute | Engine operates on catalog UUIDs under MGA and recovery authority. |
| Render | Results display names, UUIDs, metadata, or redactions according to policy. |

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Name not found | Return a bind diagnostic or hidden-object diagnostic according to disclosure policy. |
| Name resolves to multiple visible UUIDs | Return ambiguity; do not choose arbitrarily. |
| UUID has wrong object class | Refuse with a class mismatch diagnostic. |
| UUID exists but is hidden | Return denied, not visible, or not found according to policy. |
| UUID belongs outside sandbox | Return sandbox denial or redacted not-visible diagnostic. |
| Catalog generation changed | Rebind, invalidate cached state, or refuse stale execution. |
| Security epoch changed | Reauthorize and rebind policy-sensitive state. |
| Object dropped in another transaction | Apply MGA visibility and recovery rules. |
| Dependency invalidated | Revalidate, rebuild, or refuse dependent object execution. |
| Recovery state uncertain | Fail closed before using uncertain catalog identity. |

## Practical Rules For SBsql Authors

- Use ordinary names for readable scripts.
- Use UUID references for support, automation, and migration tasks that require
  unambiguous identity.
- Do not assume a rename changes the object a dependency points to.
- Do not assume a UUID bypasses security or sandboxing.
- Reprepare or rebind statements after DDL, grant/revoke, policy, search-path,
  schema, type, or routine changes.
- Use `describe`, `show`, and catalog views to inspect identity and descriptors
  rather than relying on source text.
- Treat object UUIDs in examples as illustrative unless they were returned by
  the engine.

## Verification Checklist

An identity proof should demonstrate:

- every durable catalog object receives UUID identity;
- names, aliases, localized names, and paths resolve to UUID identity;
- qualified names resolve under parent schema UUIDs;
- unqualified names follow current schema and search-path rules;
- UUID references bypass search path but not security;
- rename preserves object UUID;
- comment preserves object UUID;
- recreate creates a replacement UUID where documented;
- drop retires identity under transaction and dependency rules;
- created and dropped catalog objects obey MGA visibility;
- hidden objects do not leak through diagnostics where policy forbids it;
- stale catalog, security, resolver, and resource epochs invalidate cached
  bindings;
- SBLR envelopes contain UUID references and descriptors;
- result rendering follows disclosure policy.

## Related Reference Pages

- [Intro And MGA](#ch-language-reference-core-paradigms-intro-and-mga-md)
- [Parser To SBLR Pipeline](#ch-language-reference-core-paradigms-parser-to-sblr-pipeline-md)
- [SBsql Language Profiles](#ch-language-reference-core-paradigms-sbsql-language-profiles-md)
- [Transactions And Recovery](#ch-language-reference-core-paradigms-transactions-and-recovery-md)
- [Security And Sandboxing](#ch-language-reference-core-paradigms-security-and-sandboxing-md)
- Schema Tree And Name Resolution (SBsql Language Reference — Syntax, page XXX)
- Script Tokens And Identifiers (SBsql Language Reference — Syntax, page XXX)
- Table Lifecycle (SBsql Language Reference — Syntax, page XXX)
- View Lifecycle (SBsql Language Reference — Syntax, page XXX)
- Function Lifecycle (SBsql Language Reference — Syntax, page XXX)
- Procedure Lifecycle (SBsql Language Reference — Syntax, page XXX)
- Trigger Lifecycle (SBsql Language Reference — Syntax, page XXX)
- [Type System Overview](#ch-language-reference-data-types-type-system-overview-md)
- Refusal Vectors (SBsql Language Reference — Syntax, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/core_paradigms/transactions_and_recovery.md -->

<a id="ch-language-reference-core-paradigms-transactions-and-recovery-md"></a>

# Transactions And Recovery

This page is part of the SBsql Language Reference Manual. It explains the ScratchBird transaction and recovery model at a conceptual level. Statement syntax is documented in ../syntax_reference/transaction_control.md (SBsql Language Reference — Syntax, page XXX).

Generation task: `core_paradigms_transactions_and_recovery`

## Purpose

A transaction in ScratchBird is not a client-side concept. The engine's
Multi-Generation Architecture (MGA) owns every transaction from the moment it
opens to the moment its outcome becomes durable history. That means the engine
controls transaction identity, the snapshot that determines what rows a
transaction can see, how row versions accumulate and expire, commit and rollback
finality, cleanup horizons, and how an interrupted transaction is classified
during recovery. SBsql provides the statements to request transaction actions
and inspect admitted state, but the text of those statements is not itself
authority — it is a request that the engine may accept, defer, or refuse.

## Core Invariants

| Invariant | Meaning |
| --- | --- |
| Engine-owned identity | Every active transaction has an engine-allocated transaction UUID and local transaction number. |
| Snapshot before work | A transaction snapshot is constructed before user-visible work begins. |
| Versions, not overwrite | Writes create or retire row versions. They do not make older committed versions disappear for existing snapshots. |
| Inventory finality | Commit and rollback state are published through durable MGA transaction inventory. |
| Security recheck | Visibility is the intersection of MGA visibility and security/materialized policy. |
| Indexes are evidence | Index entries accelerate candidates. Final row authority requires MGA, predicate, descriptor, and security recheck. |
| Recovery before work | Recovery-required state fences ordinary work until classification is complete. |

## Transaction Lifecycle

![diagram](/home/dcalford/CliWork/ScratchBird/docs/documentation/draft/Language_Reference/core_paradigms/transactions_and_recovery-1.svg)

## Snapshot Contents

A transaction snapshot includes:

- visible-through transaction boundary;
- active transaction exclusions;
- cleanup and retention horizons;
- catalog epoch;
- security epoch;
- policy epoch;
- isolation profile;
- attached database/workarea root;
- effective principal and role context;
- resource and filespace readiness state.

The exact encoded snapshot is engine-owned. The user-facing rule is that visibility must be reproducible for the isolation profile and must fail closed when the required snapshot evidence is missing.

## Commit And Rollback

Commit publishes new visible state only when the durable transaction inventory and required sync/fence policy accept the commit. Rollback makes the transaction's versions non-visible and releases, compensates, or retires transaction-owned resources.

Commit and rollback are not parser decisions. A parser, driver, bridge, or client can request finality, but the engine decides and records it.

## Autocommit

Autocommit is an execution profile:

1. open a transaction;
2. execute one admitted statement or statement group;
3. commit on success;
4. rollback on failure;
5. report finality evidence or a diagnostic.

Autocommit does not weaken transaction guarantees and does not create a second authority path.

## Savepoints

Savepoints are rollback markers inside one transaction. They let a script undo part of a transaction while keeping the transaction active. They do not have independent commit authority, independent snapshots, or independent recovery finality.

## Isolation

ScratchBird public isolation profiles are documented in Transaction Control (SBsql Language Reference — Syntax, page XXX). At a high level:

- `READ COMMITTED` can see newer committed data at statement boundaries.
- `READ CONSISTENCY` and `REPEATABLE READ` are also admitted isolation spellings; see Transaction Control for their admitted behavior.
- `SNAPSHOT` uses a stable transaction snapshot.
- `SERIALIZABLE` requires conflict detection or prevention for the admitted operation set.

All profiles still require MGA row-version visibility, page/filespace validity, and security policy recheck.

## Recovery Classification

On startup or reopen after interruption, the engine classifies transaction inventory before ordinary work resumes.

| Evidence State | Required Outcome |
| --- | --- |
| Committed evidence complete | Transaction remains committed. |
| Rolled-back evidence complete | Transaction remains rolled back. |
| Active without finality | Transaction is rolled back or classified according to recovery policy. |
| Commit in progress | Recovery completes commit only if durable evidence proves it. |
| Rollback in progress | Recovery completes rollback or fences if uncertain. |
| Prepared or limbo | Recovery waits for a valid local decision, policy decision, or operator action. |
| Inconsistent evidence | Fail closed with recovery-required state. |

Silent inconsistency is not an allowed outcome.

## Bridge And Remote Transactions

Bridge operations may create a local transaction and one or more remote transactions. Each participating database keeps its own transaction authority. A local commit cannot assert remote finality, and a remote commit cannot assert local finality. Cross-node or distributed finality requires explicit policy-owned routes and must preserve local MGA rules.

## Diagnostics And Inspection

Authorized inspection surfaces can expose:

- transaction UUID;
- local transaction number;
- state;
- isolation profile;
- snapshot boundary;
- active savepoints;
- lock/resource wait state;
- recovery-required state;
- commit/rollback evidence status.

Inspection is policy controlled and may redact details.

## Failure Principle

When transaction outcome is uncertain, ScratchBird must return an explicit diagnostic, fence unsafe work, and require recovery or operator/policy decision. It must not infer finality from SQL text, client retry behavior, timestamps, UUID order, parser state, or ordinary log messages.




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/core_paradigms/security_and_sandboxing.md -->

<a id="ch-language-reference-core-paradigms-security-and-sandboxing-md"></a>

# Security And Sandboxing

This page is part of the SBsql Language Reference Manual. It explains the
security model that applies before SBsql text can become engine work: identity,
roles, grants, sandbox roots, catalog visibility, policy, masking, row-level
security, protected material, and fail-closed refusal behavior.

Generation task: `core_paradigms_security_and_sandboxing`

## Purpose

Security in ScratchBird is materialized engine state, not a gate checked once
at login. Every operation is evaluated against the intersection of who the
caller is, what roles they hold, which schema sandbox they are in, what policy
applies to the object they are touching, and what the current transaction and
recovery state permits. Passing authentication, owning an object, or holding a
GRANT are all necessary inputs — but the engine rechecks all of them at
execution time, not just at the moment the session opened.

The model is fail-closed: when any required piece of evidence is missing,
stale, ambiguous, or contradicted by an explicit denial, the engine refuses the
operation rather than guessing:

- explicit denial wins over allow;
- hidden objects must stay hidden unless disclosure policy admits them;
- policy, masks, and row-level security narrow access after ordinary grants;
- server-local file, network, bridge, stream, backup, restore, diagnostic, and
  management surfaces require explicit policy admission;
- missing, stale, ambiguous, corrupted, or recovery-fenced security evidence
  refuses the operation rather than silently allowing it.

## Security Flow

![diagram](/home/dcalford/CliWork/ScratchBird/docs/documentation/draft/Language_Reference/core_paradigms/security_and_sandboxing-1.svg)

Security is evaluated on bound identity. A name that cannot be resolved inside
the session's visible schema branch does not become a security decision about a
hidden object. When disclosure policy requires it, missing-object and
hidden-object diagnostics can be intentionally indistinguishable.

## Principal Model

Every principal has durable UUID identity. Names are resolver inputs and display
labels.

| Principal | Purpose |
| --- | --- |
| User | Human or application identity that can attach, authenticate, own objects, and receive grants. |
| Agent | Admitted engine, management, maintenance, migration, replication, or operational actor with a UUID and policy-bound authority. |
| Role | Privilege bundle that can be activated by a session where policy admits it. |
| Group | Membership collection used to organize users, agents, and roles. |
| Public | Optional pseudo-principal for privileges intentionally available to every attached session. |
| Owner | Principal recorded as owning a durable object. Ownership is materialized authorization, not a parser bypass. |

The effective security context for a statement includes:

- authenticated principal UUID;
- attached database or workarea UUID;
- sandbox root;
- home schema and current schema;
- active role set;
- group membership;
- session attributes admitted by policy;
- transaction UUID and isolation profile;
- security epoch;
- policy epoch;
- redaction and disclosure profile;
- operation descriptor.

## Authentication And Session Binding

Authentication proves that a client or agent may become a principal in the
current attach context. Authorization decides what that principal may do after
attach.

An attach can establish:

- user UUID;
- agent UUID;
- provider identity evidence;
- session UUID;
- attached database/workarea root;
- network, IPC, embedded, or bridge endpoint evidence;
- default schema and home schema;
- initial active role set;
- sandbox root;
- policy profile;
- resource limits and timeout profile.

Raw secret material is not ordinary SBsql data. Secret references, provider
tokens, credential handles, and protected-material handles must not be rendered
in result sets, logs, diagnostics, catalog display, support bundles, or message
vectors unless an explicit protected-material release policy admits that
specific disclosure.

## Sandboxed Schema Roots

A sandbox root limits what a session can name, inspect, and operate on through
ordinary name resolution. A session connected to a workarea, tenant branch,
application branch, or other bounded schema root sees that root as its visible
world unless SBsql administrative authority and policy admit broader access.

![diagram](/home/dcalford/CliWork/ScratchBird/docs/documentation/draft/Language_Reference/core_paradigms/security_and_sandboxing-2.svg)

Sandbox rules:

- ordinary name resolution starts inside the session root;
- recursive schema lookup cannot escape the admitted root;
- current schema and search path entries outside the root are refused or hidden;
- grants inside the sandbox do not imply grants outside the sandbox;
- catalog projections can render authorized metadata from outside the root only
  when the projection object itself has authority and policy admits disclosure;
- support diagnostics must redact names, UUIDs, paths, values, and policy
  details according to the session's disclosure profile.

See Schema Tree And Name Resolution (SBsql Language Reference — Syntax, page XXX).

## Grants And Effective Privileges

Grants attach privileges to principal UUIDs, roles, groups, or public scope.
Revokes remove privilege edges. Effective privileges are computed for a bound
operation, not for raw source text.

Resolution order:

1. Authenticate or bind the effective principal UUID.
2. Establish sandbox root, attached database/workarea, current schema, and home
   schema.
3. Resolve the target object to UUID identity under metadata visibility rules.
4. Collect direct grants to the principal.
5. Collect grants inherited through active roles and admitted groups.
6. Apply ownership and administration policy.
7. Apply explicit denials.
8. Apply row, column, object, stream, file, bridge, and management policies.
9. Check transaction and recovery gates.
10. Admit or refuse the operation.

Common privilege targets include:

| Target | Examples Of Controlled Operations |
| --- | --- |
| Database | connect, create schema, create filespace, backup, restore, security administration, configuration. |
| Schema | usage, create object, alter, drop, describe. |
| Table | select, insert, update, delete, truncate, references, trigger, alter, drop, comment, describe. |
| Column | select, insert, update, references, describe. |
| View and materialized view | select, refresh, alter, drop, comment, describe. |
| Routine | execute, alter, drop, comment, describe. |
| Domain and type descriptor | usage, alter, drop, comment, describe. |
| Sequence | usage, select, update, alter, drop. |
| Policy, mask, and RLS | apply, alter, drop, enable, disable, describe, validate. |
| Filespace | usage, create object, attach, detach, alter, drop, describe, promote where admitted. |
| Stream or bridge | connect, import, export, replicate, migrate, validate, cutover. |
| Management surface | show metrics, manage sessions, validate configuration, generate support bundle, run admitted maintenance. |

See Security And Privilege Statements (SBsql Language Reference — Syntax, page XXX).

## Explicit Deny Wins

An allow edge can make an operation possible. A deny edge or policy refusal can
still block it.

Examples:

- a user has `SELECT` on a table, but RLS hides rows for that user's tenant;
- a user has `SELECT` on a column, but a mask returns a redacted value;
- a user has `UPDATE` on a table, but a policy denies updates after a workflow
  state is sealed;
- a user has a management role, but a recovery fence denies write admission;
- a user can create objects in a schema, but filespace policy denies the
  requested placement.

Denial is not a parser error. It is an explicit message vector result.

## Policy, Masks, And Row-Level Security

Policy objects refine grant authority.

| Object | Role |
| --- | --- |
| Policy | General durable rule applied to an object, operation family, principal scope, security context, stream, bridge, support surface, or management surface. |
| Mask | Value rendering rule that transforms or redacts a visible value before it leaves the engine. |
| RLS | Row-level rule that filters visible row versions or checks proposed row mutations. |

Evaluation order is conceptually:

1. MGA determines which row versions are visible to the transaction.
2. Object and column privileges determine whether the operation can proceed.
3. RLS filters visible rows or refuses row mutation.
4. Masks transform values that are visible but protected.
5. Protected-material release policy decides whether sensitive values can leave
   the engine.
6. Diagnostics and support output are redacted according to disclosure policy.

RLS is not a transaction system. It does not decide whether a row version exists
or whether a transaction committed. MGA owns that. RLS decides whether the
effective security context may see or mutate the otherwise visible row.

See Policy, Mask, And RLS Lifecycle (SBsql Language Reference — Syntax, page XXX).

## Protected Material

Protected material includes secrets, credentials, keys, tokens, protected
configuration values, masked values, sensitive diagnostic fields, and any value
whose release is governed by policy.

Rules:

- store and pass secret references rather than raw secrets where possible;
- redact protected material in diagnostics and support bundles by default;
- do not expose protected values through casts, string concatenation, errors,
  logs, `show`, `describe`, catalog views, or procedure output without release
  authority;
- treat export, backup, replication, migration, bridge, and stream routes as
  protected-material release surfaces when values can cross a boundary;
- record audit evidence for admitted release where policy requires it.

## Catalog Projections

Catalog tables and views can reveal metadata. Metadata itself can be sensitive:
object names, existence, ownership, privilege edges, policy names, filespace
layout, endpoint labels, and diagnostic handles can all disclose information.

ScratchBird therefore distinguishes:

- base catalog authority;
- projection authority;
- disclosure policy;
- redaction policy;
- sandbox root;
- ordinary object privileges.

A user may have `SELECT` on an authorized catalog projection that renders a
safe view of objects outside the sandbox. That does not grant the user direct
name resolution or object privileges outside the sandbox.

## Procedural Security

Procedures, functions, packages, and triggers run with an explicit security
mode. The mode is part of the routine descriptor and is checked when the routine
is compiled, invoked, invalidated, or revalidated.

| Mode | Contract |
| --- | --- |
| Invoker rights | The routine executes with the caller's effective principal, active roles, sandbox root, and policy context. |
| Definer rights | The routine executes with admitted definer authority for the routine body while preserving caller context for auditing and policy inputs. |
| Agent rights | The routine is invoked by an admitted agent and runs only within the agent's registered authority and purpose. |
| Restricted rights | The routine runs with an intentionally reduced privilege set even when caller or definer has broader authority. |

Procedural security rules:

- routine source text is not authority;
- compiled routine SBLR must reference UUID-bound objects and descriptors;
- dependency, grant, policy, domain, type, and catalog changes can invalidate a
  compiled routine;
- dynamic execution must pass through parse, bind, admission, authorization,
  and engine dispatch;
- cursors, result sets, streams, and protected values passed between routines
  retain descriptor and security context;
- trigger execution must not bypass the security and transaction context of the
  firing operation.

See Procedural SQL (SBsql Language Reference — Syntax, page XXX),
Function Lifecycle (SBsql Language Reference — Syntax, page XXX),
Procedure Lifecycle (SBsql Language Reference — Syntax, page XXX), and
Trigger Lifecycle (SBsql Language Reference — Syntax, page XXX).

## File, Stream, Bridge, And Network Gates

Operations that move data across a boundary are policy-controlled even when the
ordinary object privileges are present.

| Surface | Security Rule |
| --- | --- |
| `COPY FROM STDIN` | Client-supplied stream frames are admitted through the stream contract and target-object privileges. |
| `COPY TO STDOUT` | Data can leave the engine only through result and stream policy. |
| Server-local location | Opening a server-local path requires explicit policy admission before any file access occurs. |
| Logical backup | May stream logical data only through admitted backup and protected-material policy. |
| Logical restore | May ingest logical instructions only through admitted restore policy and target privileges. |
| Replication and CDC | Requires source, target, ordering, idempotency, quarantine, and cutover authority. |
| Migration | Requires metadata, data, stream, validation, and cutover authority. |
| Bridge | Requires bridge-use privilege, endpoint policy, identity delegation policy, and stream policy. |
| Management diagnostics | Requires management privilege and redaction policy. |

Low-level repair, verification, page-copy backup, page-copy restore, and direct
storage manipulation are not ordinary parser privileges. They require explicit
administrative SBsql routes and policy admission.

See COPY Streaming Import And Export (SBsql Language Reference — Syntax, page XXX) and
Backup, Restore, Replication, And Migration (SBsql Language Reference — Syntax, page XXX).

## Agent Sandboxing

Agents are principals. They are not ambient superusers.

An agent has:

- UUID identity;
- registered purpose;
- owner or controller;
- admitted scope;
- allowed operation families;
- resource limits;
- activation policy;
- audit policy;
- shutdown and cancellation policy;
- support-bundle disclosure policy.

An agent may run maintenance, migration, replication, validation, support, or
management work only within its registered authority. Agent work still uses
SBLR admission, engine authorization, MGA transaction authority, resource
limits, and fail-closed recovery behavior.

See Agents And Agent Management (SBsql Language Reference — Syntax, page XXX).

## Security Epochs And Invalidation

Security changes advance a security epoch. Dependent state must be invalidated
or revalidated when the epoch changes.

Dependent state includes:

- prepared statements;
- bound SBLR envelopes;
- compiled procedures and functions;
- trigger plans;
- optimizer plans and plan cache entries;
- metadata caches;
- catalog projection caches;
- bridge sessions;
- stream authorizations;
- support-bundle projections;
- active security snapshots where policy requires revalidation.

Invalidation prevents stale authorization from surviving a grant, revoke,
policy, mask, role, group, identity, sandbox, or protected-material change.

## Statement Examples

Create a role, grant schema usage, and grant read access:

```sql
create role app_reader;

grant usage on schema app to app_reader;
grant select on table app.orders to app_reader;
```

Activate a role for the current session:

```sql
set role app_reader;
```

Create a row-level rule that narrows visible rows:

```sql
create rls app.orders_tenant_rls
on table app.orders
for select
using tenant_uuid = current_tenant_uuid()
to role app_reader
active;
```

Create a mask for a protected column:

```sql
create mask app.customer_email_mask
on column app.customer.email
using case
    when has_role('support_full_contact') then email
    else null
end
active;
```

Attempting to read outside the sandbox returns a denial or a redacted
missing-object diagnostic according to disclosure policy:

```sql
select *
from admin.security_audit;
```

## Syntax Productions

```ebnf
dcl_security_stmt ::=
      create_principal_stmt
    | alter_principal_stmt
    | drop_principal_stmt
    | grant_stmt
    | revoke_stmt
    | set_role_statement
    | show_security_statement
    | describe_security_statement ;
```

```ebnf
principal_ref           ::= uuid_reference
                          | qualified_name ;
```

```ebnf
grant_stmt              ::= "GRANT" grant_payload "TO" principal_ref grant_option_list? ;
```

```ebnf
revoke_stmt             ::= "REVOKE" revoke_payload "FROM" principal_ref revoke_option_list? ;
```

```ebnf
policy_stmt             ::= create_policy_statement
                          | alter_policy_statement
                          | drop_policy_statement
                          | create_mask_statement
                          | alter_mask_statement
                          | drop_mask_statement
                          | create_rls_statement
                          | alter_rls_statement
                          | drop_rls_statement
                          | show_policy_statement
                          | describe_policy_statement
                          | validate_policy_statement ;
```

## Binding And Execution Summary

| Step | Security Meaning |
| --- | --- |
| Parse | The statement shape is recognized. No authority is granted. |
| Bind principal | User, role, group, agent, or public references resolve to UUIDs where visible. |
| Bind target | Object names resolve under sandbox and metadata visibility rules. |
| Bind descriptors | Parameters, expressions, rowsets, streams, masks, policies, and result shapes receive descriptors. |
| Admit envelope | Server admission checks route, envelope version, operation identity, and gated capability state. |
| Authorize | Grants, roles, groups, ownership, deny edges, policy, masks, RLS, and protected-material rules are evaluated. |
| Execute | Engine performs the admitted operation under MGA and recovery authority. |
| Render | Results and diagnostics are redacted according to disclosure policy. |

## Refusal Classes

| Refusal | Typical Cause |
| --- | --- |
| `unsupported` | Security surface, grant target, policy option, provider route, or management route is not available in the build or target. |
| `denied` | Principal lacks privilege, sandbox blocks resolution, policy blocks access, protected material cannot be released, recovery fences work, or server-local access is not admitted. |
| `unlicensed` | A recognized route reaches a provider or gated boundary that reports the capability is not licensed. |
| Parse error | The source text does not form a valid SBsql statement. |
| Bind error | Principal, object, descriptor, role, policy, or target reference cannot be resolved unambiguously. |

See Refusal Vectors (SBsql Language Reference — Syntax, page XXX).

## Verification Checklist

A security and sandbox proof should demonstrate:

- authenticated sessions bind to a principal UUID;
- unauthenticated sessions cannot perform protected operations;
- role activation changes effective privileges only where admitted;
- explicit deny overrides direct and inherited allow;
- sandbox roots prevent ordinary name resolution outside the root;
- hidden objects do not leak through diagnostics when disclosure policy forbids
  it;
- authorized catalog projections can render only policy-admitted metadata;
- grants and revokes are transactional;
- security epoch changes invalidate dependent plans and metadata;
- RLS filters otherwise visible row versions;
- masks transform visible values without changing stored values;
- protected material is redacted from diagnostics and support output by default;
- server-local file access is denied unless explicitly admitted;
- bridge and stream operations require endpoint, identity, object, and stream
  authority;
- agent operations run only inside registered authority;
- recovery-required state fences unsafe security and data operations;
- unsupported, denied, and unlicensed routes return explicit message vectors.

## Related Reference Pages

- [Intro And MGA](#ch-language-reference-core-paradigms-intro-and-mga-md)
- [Parser To SBLR Pipeline](#ch-language-reference-core-paradigms-parser-to-sblr-pipeline-md)
- [UUID Catalog Identity](#ch-language-reference-core-paradigms-uuid-catalog-identity-md)
- [Transactions And Recovery](#ch-language-reference-core-paradigms-transactions-and-recovery-md)
- Security And Privilege Statements (SBsql Language Reference — Syntax, page XXX)
- Policy, Mask, And RLS Lifecycle (SBsql Language Reference — Syntax, page XXX)
- Schema Tree And Name Resolution (SBsql Language Reference — Syntax, page XXX)
- Refusal Vectors (SBsql Language Reference — Syntax, page XXX)
- Agents And Agent Management (SBsql Language Reference — Syntax, page XXX)
- COPY Streaming Import And Export (SBsql Language Reference — Syntax, page XXX)
- Backup, Restore, Replication, And Migration (SBsql Language Reference — Syntax, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/core_paradigms/bridge_and_cluster_boundaries.md -->

<a id="ch-language-reference-core-paradigms-bridge-and-cluster-boundaries-md"></a>

# Bridge And Cluster Boundaries

This page is part of the SBsql Language Reference Manual. It explains the
boundary between ordinary bridge operations, remote data access, logical data
movement, provider-backed capabilities, and cluster-classified operations.

Generation task: `core_paradigms_bridge_and_cluster_boundaries`

## Purpose

A bridge is a connection boundary. It lets an authorized local user, session, or
agent connect to another database endpoint through a registered bridge-capable
package. The bridge can carry statements, cursors, rows, streams, logical
backup/restore data, replication data, migration data, diagnostics, and
capability information.

A bridge does not move transaction finality, catalog identity, storage
authority, recovery authority, or security authority out of the participating
databases. Each database keeps its own MGA transaction authority and durable
catalog identity.

Cluster-classified operations are different. They coordinate placement,
membership, distributed query planning, distributed transaction barriers,
failover, cross-node route authority, and similar multi-node behavior. Those
surfaces are admitted only through cluster gates. In a public build they either
return an explicit unsupported/unlicensed message vector or route to the public
compile/link stub and fail closed.

## Boundary Summary

| Surface | Classification | Authority Model |
| --- | --- | --- |
| Local table query | Local engine operation | Local MGA, local catalog UUIDs, local security, local storage. |
| Remote table through bridge | Bridge operation | Local session plus remote session; each database keeps its own transaction authority. |
| Logical backup stream | Stream operation | Source database owns snapshot and export authority; stream policy controls release. |
| Logical restore stream | Stream operation | Target database owns catalog mapping, transaction finality, and import authority. |
| CDC or replication route | Bridge/stream operation | Source and target each own local transaction state; ordering evidence is route evidence, not finality authority. |
| Migration route | Bridge/stream operation | Source, target, mapping, validation, quarantine, and cutover are explicit policy-bound phases. |
| Cross-node optimizer fanout | Cluster-classified operation | Requires cluster provider admission. Public builds fail closed. |
| Distributed transaction barrier | Cluster-classified operation | Requires cluster provider admission. Local MGA remains local authority. |
| Membership, failover, placement, shard routing | Cluster-classified operation | Requires cluster provider admission. Public builds provide diagnostics only. |

An ordinary query that reads a remote relation through a bridge is not a
distributed query. The local operation treats the remote relation as an input
reached through a connection. A distributed query lets a cluster-aware authority
plan, route, and coordinate work across nodes or fragments; that surface is
cluster-classified.

## Bridge Lifecycle

![diagram](/home/dcalford/CliWork/ScratchBird/docs/documentation/draft/Language_Reference/core_paradigms/bridge_and_cluster_boundaries-1.svg)

Bridge registration makes a capability available. It does not create an
automatic connection or grant anyone authority to use it. An authorized SBsql
statement creates the connection descriptor and policy state. A session or
agent then attaches, authenticates, opens a session, and begins one or more
remote transactions as needed.

## Authority Model

Bridge authority is layered.

| Layer | Owns |
| --- | --- |
| Local database | Local session identity, local transaction, local catalog, local policy, local resource limits, local result rendering. |
| Bridge descriptor | Endpoint reference, capability profile, stream limits, security policy, retry policy, diagnostic profile. |
| Bridge package | Wire protocol, remote statement rendering, result decoding, capability reporting, stream framing. |
| Remote database | Remote authentication, remote session, remote catalog, remote transaction, remote security, remote result semantics. |
| Engine admission | Whether the bound SBsql operation may call the bridge route at all. |

The bridge package is not durable authority. It reports capabilities, translates
requests, frames data, and returns evidence. The local engine decides whether
the local operation is admitted. The remote endpoint decides whether the remote
operation is admitted.

## Local And Remote Transactions

A bridge operation can involve one local transaction and one or more remote
transactions. Each participating database owns its own transaction state.

![diagram](/home/dcalford/CliWork/ScratchBird/docs/documentation/draft/Language_Reference/core_paradigms/bridge_and_cluster_boundaries-2.svg)

Rules:

- local commit is local MGA finality;
- remote commit is remote finality;
- local rollback does not prove remote rollback unless the remote transaction
  actually rolled back and returned evidence;
- remote commit evidence does not override local recovery classification;
- savepoints, retain/chain behavior, autocommit, and prepare are used only when
  both the route and the remote endpoint report support;
- uncertain finality must return explicit diagnostics and fence unsafe work.

## Bridge Operations

The public bridge surface groups operations by purpose.

| Operation | Purpose |
| --- | --- |
| Describe capabilities | Report package ABI, supported statement families, streams, transaction modes, diagnostics, and limits. |
| Create connection | Create a durable connection descriptor with endpoint, policy, capability, and secret-reference metadata. |
| Attach/connect | Establish a connection handle under a user or agent context. |
| Authenticate | Establish remote identity using policy-admitted credential references or delegation. |
| Open session | Create a remote session that can own cursors and remote transactions. |
| Close session | End a remote session and release session-owned resources. |
| Detach | Close the connection handle and return final diagnostics. |
| Ping/health | Check availability, route state, and provider readiness without changing data. |
| Cancel | Request cancellation of an active remote operation. |
| Drain | Stop accepting new work and allow admitted work to finish or cancel by policy. |
| Shutdown | Shut down an admitted bridge package or connection scope where policy permits it. |
| Begin transaction | Start a remote transaction associated with the bridge session. |
| Commit transaction | Request remote commit and return remote finality evidence. |
| Rollback transaction | Request remote rollback and return remote finality evidence. |
| Savepoint | Create or roll back to a remote savepoint where supported. |
| Cursor fetch | Fetch descriptor-bound rows under stream and backpressure policy. |
| Stream read/write | Move stream frames for rows, large values, logical backup, restore, CDC, replication, migration, and result data. |
| Validate | Validate connection, mapping, stream shape, endpoint capabilities, and cutover readiness. |
| Compare | Compare source and target data or metadata under an admitted migration/replication route. |
| Cutover | Complete an admitted migration or replication transition after validation. |

Unsupported operations must return explicit message vectors. They must not be
silently ignored.

## Bridge Handles And Scope

Bridge handles are opaque session-scoped or agent-scoped references. The
database stores connection configuration and policy. The bridge package supplies
the ability to connect; it does not own the durable connection policy.

Handle rules:

- handles are not raw pointers or client-trusted tokens;
- handles are bound to session, agent, transaction, endpoint, policy, and
  security context;
- a handle can own multiple remote transactions when the route permits it;
- a handle can be cancelled, drained, closed, or invalidated by policy;
- handle metadata is redacted in diagnostics unless disclosure policy admits it;
- stale handles fail closed after disconnect, revoke, policy change, provider
  reload, or recovery fence.

## Streams And Backpressure

Bridge streams carry typed frames. They are not arbitrary byte pipes once they
enter the SBsql execution pipeline.

Stream contracts include:

- frame type and descriptor;
- maximum frame size;
- maximum in-flight bytes;
- timeout and cancellation behavior;
- retry and idempotency policy;
- ordering token where required;
- transaction grouping where required;
- quarantine behavior for invalid records;
- redaction and protected-material policy;
- completion and failure diagnostics.

Large values, cursors, logical backup streams, restore streams, CDC streams,
replication streams, migration streams, and `COPY` streams all use explicit
stream contracts.

## Logical Backup And Restore Across A Bridge

Logical backup and logical restore are allowed where the route, policy, and
endpoint capabilities admit them.

| Operation | Allowed Shape |
| --- | --- |
| Logical backup to client or bridge stream | Exports metadata and data as typed logical instructions and row frames. |
| Logical restore from client or bridge stream | Reads typed logical instructions and applies them through target catalog and DML routes. |
| Partial logical backup | Exports an admitted subset such as a schema, table, query, or policy-bound scope. |
| Partial logical restore | Imports an admitted subset with explicit mapping and validation. |
| Physical page-copy backup or restore | Denied through bridge/parser routes unless an explicit administrative route admits it. |
| Server-local file manipulation | Denied by default unless an explicit named location policy admits it. |

Logical streams are interpreted as instructions and typed data. They are not
trusted as catalog or transaction authority. The target database decides what
UUIDs, descriptors, security mappings, and transaction outcomes are admitted.

## CDC, Replication, ETL, And Migration

CDC, replication, ETL, and migration routes are bridge and stream operations.
They require direction, capability, identity, ordering, idempotency, mapping,
quarantine, validation, and cutover policy.

| Concern | Rule |
| --- | --- |
| Direction | A route can be source, target, or both only when capability negotiation reports support. |
| Transaction grouping | Changes must preserve group boundaries where the route requires them. |
| Ordering token | Ordering evidence must be present when replay or cutover depends on it. |
| Record identity | Each record must carry enough identity to apply, compare, quarantine, or reject it. |
| Idempotency | Replayed changes require an idempotency key or equivalent route contract where policy requires it. |
| Quarantine | Invalid or ambiguous records must be quarantined or refused according to policy. |
| Cutover | Cutover requires validation that the target is ready and that no required changes are missing. |
| Finality | Route evidence does not override local or remote MGA finality. |

## Remote Query Versus Distributed Query

A remote query through a bridge:

- connects to one remote endpoint through a bridge session;
- treats remote rows as a relation input;
- uses local and remote transactions according to bridge policy;
- does not give the local optimizer authority to distribute work across a
  cluster;
- does not create cluster placement, membership, route, or failover authority.

A distributed query:

- plans or routes work across nodes, shards, fragments, or distributed
  participants;
- can require distributed read safety, fanout, partial aggregation, merge,
  placement, route authority, and distributed diagnostics;
- is cluster-classified;
- requires cluster provider admission;
- fails closed in public builds unless an admitted provider boundary exists.

## Cluster Gate

Cluster-classified statements are recognized so tools and scripts can receive
stable diagnostics. Recognition is not execution.

![diagram](/home/dcalford/CliWork/ScratchBird/docs/documentation/draft/Language_Reference/core_paradigms/bridge_and_cluster_boundaries-3.svg)

The public compile/link stub exists to prove parser routing, SBLR mapping, ABI
wiring, diagnostics, and fail-closed behavior. It provides no cluster
membership, routing authority, replication authority, failover, recovery,
distributed transaction control, or production cluster behavior.

See Cluster-Gated Statements (SBsql Language Reference — Syntax, page XXX).

## Cluster-Classified Operation Families

| Family | Examples |
| --- | --- |
| Topology | Inspect topology, define regions, define shard profiles, publish topology manifests, validate topology schema. |
| Membership | Admit, remove, drain, assign role, inspect health, validate node suitability. |
| Routing | Publish ownership, reject stale ownership, inspect route plans. |
| Placement | Place objects, rebalance shards, validate partition distribution, assign ranges. |
| Distributed transactions | Begin distributed work, prepare participants, publish barriers, recover limbo, advance cleanup, validate finality evidence. |
| Replication and reconciliation | Consume events, reconcile ledgers, apply merge policy, report conflicts, publish reconciled state. |
| Security and fencing | Validate epoch, issue or revoke fence tokens, validate policy versions, validate route authority. |
| Jobs and throttling | Start or cancel controlled jobs, throttle workloads, run admitted maintenance. |
| Metrics and support | Inspect provider status, route traces, events, support evidence, and readiness state. |
| Distributed query | Plan and admit cross-node work, route fragments, fanout reads, merge rows, aggregate partials, validate safe reads. |

These operation families are not implemented by ordinary local query execution
or by ordinary bridge remote-table access.

## Security And Secrets

Bridge and cluster-gated operations are security-sensitive because they can move
data, create remote sessions, expose metadata, and affect operational state.

Security rules:

- bridge use requires an explicit privilege on the bridge descriptor or route;
- endpoint access requires policy admission;
- external network access requires policy admission;
- credential material uses secret references or provider-owned handles, not raw
  statement text;
- remote authentication follows the destination endpoint's authority model;
- metadata rendering is redacted by disclosure policy;
- protected material cannot be exported, logged, diagnosed, replicated, or
  streamed without release authority;
- management operations require management privileges even when they return a
  refusal;
- cluster-classified operations require cluster gate admission before any
  provider behavior can run.

See [Security And Sandboxing](#ch-language-reference-core-paradigms-security-and-sandboxing-md) and
Security And Privilege Statements (SBsql Language Reference — Syntax, page XXX).

## Recovery And Failure Rules

Bridge and cluster boundary failures must be explicit.

| Failure | Required Behavior |
| --- | --- |
| Missing bridge package | Return unsupported or unavailable capability. |
| Bridge package cannot authenticate | Return bridge authentication failure. |
| Endpoint lacks capability | Return missing capability or unsupported operation. |
| Stream frame invalid | Reject frame, quarantine record, or abort stream according to policy. |
| Ordering ambiguous | Refuse replay, CDC, migration, or cutover until ordering evidence exists. |
| Idempotency missing | Refuse replay/apply route where idempotency is required. |
| Remote transaction uncertain | Fence dependent local work and report uncertainty. |
| Local transaction uncertain | Follow local MGA recovery and fail closed. |
| Provider unavailable | Return unavailable provider or unlicensed provider message vector. |
| Cluster route disabled | Return unsupported before provider call. |
| Cluster stub reached | Return unlicensed or fail-closed diagnostic from the stub boundary. |

Silent partial success is not allowed for bridge or cluster-classified
operations.

## Syntax Productions

```ebnf
bridge_operation        ::= bridge_connection_operation
                          | bridge_session_operation
                          | bridge_transaction_operation
                          | bridge_cursor_operation
                          | bridge_stream_operation
                          | bridge_replication_operation
                          | bridge_migration_operation
                          | bridge_diagnostic_operation ;
```

```ebnf
bridge_connection_operation ::=
      describe_bridge_capabilities
    | create_bridge_connection
    | alter_bridge_connection
    | drop_bridge_connection
    | validate_bridge_connection
    | attach_bridge
    | detach_bridge
    | ping_bridge
    | health_bridge ;
```

```ebnf
bridge_session_operation ::=
      open_bridge_session
    | close_bridge_session
    | cancel_bridge_operation
    | drain_bridge_session
    | shutdown_bridge_scope ;
```

```ebnf
bridge_transaction_operation ::=
      begin_bridge_transaction
    | commit_bridge_transaction
    | rollback_bridge_transaction
    | savepoint_bridge_transaction ;
```

```ebnf
bridge_stream_operation ::=
      open_bridge_stream
    | read_bridge_stream
    | write_bridge_stream
    | close_bridge_stream ;
```

```ebnf
cluster_stmt ::= show_cluster
               | alter_cluster_stmt
               | create_cluster_stmt
               | drop_cluster_stmt ;
```

```ebnf
show_cluster            ::= "SHOW" "CLUSTER" cluster_target ;
alter_cluster_stmt      ::= "ALTER" "CLUSTER" cluster_action ;
create_cluster_stmt     ::= "CREATE" "CLUSTER" cluster_create_payload ;
drop_cluster_stmt       ::= "DROP" "CLUSTER" cluster_ref ;
```

The grammar production name `cluster_stmt` (formerly `private_cluster_statement`)
describes cluster-classified statement grouping. It does not mean production
cluster implementation code is present in the public build.

## Binding And Execution Summary

| Step | Bridge Meaning | Cluster-Gated Meaning |
| --- | --- | --- |
| Parse | Recognize bridge, stream, remote, replication, migration, or diagnostic intent. | Recognize cluster-classified intent. |
| Bind | Resolve bridge descriptors, endpoints, streams, objects, parameters, transactions, and policy inputs. | Resolve names and descriptors required to classify the cluster operation. |
| Lower | Produce SBLR for bridge route or explicit refusal. | Produce SBLR for cluster gate or explicit refusal. |
| Admit | Check bridge capability, security, stream, endpoint, and resource policy. | Check compile-time cluster gate and provider boundary admission. |
| Execute | Bridge package performs admitted connection or stream work; engines retain authority. | Provider boundary executes only in an admitted build/profile. Public stub fails closed. |
| Return | Result envelope includes rows, stream state, remote evidence, diagnostics, or refusal. | Result envelope includes provider diagnostics, unsupported, unlicensed, or fail-closed refusal. |

## Verification Checklist

A bridge and cluster-boundary proof should demonstrate:

- bridge package registration does not grant automatic use authority;
- connection descriptors require explicit SBsql creation and policy;
- bridge attach/auth/session lifecycle returns explicit diagnostics;
- local and remote transactions remain separately authoritative;
- remote commit evidence cannot override local MGA recovery;
- local rollback cannot pretend remote rollback occurred;
- stream frame limits, cancellation, timeouts, and backpressure are enforced;
- logical backup and restore use typed streams and policy-bound mappings;
- server-local file access is denied unless a named location policy admits it;
- physical page-copy backup/restore is denied through bridge/parser routes;
- CDC, replication, ETL, and migration require ordering, idempotency, mapping,
  quarantine, validation, and cutover evidence where policy requires it;
- remote-table access is distinct from distributed query;
- cluster-classified statements return unsupported when the build gate is off;
- cluster-classified statements reaching the public stub return unlicensed or
  fail-closed diagnostics;
- provider errors do not become silent success;
- protected material is not leaked through streams, diagnostics, logs, support
  bundles, or metadata rendering;
- all refusals use explicit message vectors.

## Related Reference Pages

- [Intro And MGA](#ch-language-reference-core-paradigms-intro-and-mga-md)
- [Parser To SBLR Pipeline](#ch-language-reference-core-paradigms-parser-to-sblr-pipeline-md)
- [Transactions And Recovery](#ch-language-reference-core-paradigms-transactions-and-recovery-md)
- [Security And Sandboxing](#ch-language-reference-core-paradigms-security-and-sandboxing-md)
- Cluster-Gated Statements (SBsql Language Reference — Syntax, page XXX)
- Backup, Restore, Replication, And Migration (SBsql Language Reference — Syntax, page XXX)
- COPY Streaming Import And Export (SBsql Language Reference — Syntax, page XXX)
- Management And Operations (SBsql Language Reference — Syntax, page XXX)
- Agents And Agent Management (SBsql Language Reference — Syntax, page XXX)
- Refusal Vectors (SBsql Language Reference — Syntax, page XXX)




# Data Types




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/data_types/index.md -->

<a id="ch-language-reference-data-types-index-md"></a>

# Data Types Index

The data types chapter explains how SBsql values are described, compared,
stored, converted, and carried across the parser-to-engine boundary. The key
concept throughout is the _descriptor_: a resolved, engine-bound description of
a value's type, character set, precision, collation, domain policy, and
conversion rules. Knowing how descriptors work helps you write queries and
schema definitions that behave predictably under all conditions.

This chapter covers scalar types (numeric, text, temporal, binary), identity and
protected-material types (UUID, secret references), multimodel types (document,
graph, vector, spatial), and the domain and coercion rules that apply across all
of them.

## Pages In This Chapter

| Page | File | Use it for |
| --- | --- | --- |
| Type System Overview | [type_system_overview.md](#ch-language-reference-data-types-type-system-overview-md) | Understanding the descriptor model, how type names resolve to descriptors before execution, and why the engine does not infer type behavior from SQL text. |
| Numeric Types | [numeric_types.md](#ch-language-reference-data-types-numeric-types-md) | Reference for integer, unsigned integer, decimal, decimal-float, approximate real, and money descriptor families, ranges, and arithmetic rules. |
| Text, Collation, And Charset | [text_collation_and_charset.md](#ch-language-reference-data-types-text-collation-and-charset-md) | Reference for character and large-text descriptors, character set rules, collation behavior, comparison, and indexing. |
| Temporal Types | [temporal_types.md](#ch-language-reference-data-types-temporal-types-md) | Reference for date, time, timestamp, timestamptz, and interval descriptors, precision rules, timezone policy, and arithmetic. |
| Binary, UUID, And Protected Values | [binary_uuid_and_protected_values.md](#ch-language-reference-data-types-binary-uuid-and-protected-values-md) | Reference for byte-string, UUID, and protected-material descriptors, including the security rules that govern protected values. |
| Document, Graph, Vector, And Multimodel Types | [document_graph_vector_and_multimodel_types.md](#ch-language-reference-data-types-document-graph-vector-and-multimodel-types-md) | Reference for JSON document, array, vector, spatial, graph, time-series, and key-value descriptor families. |
| Domains, Casts, And Coercion | [domains_casts_and_coercion.md](#ch-language-reference-data-types-domains-casts-and-coercion-md) | Understanding how domains wrap descriptors with policy, how explicit casts and assignment coercion work, and when domain identity is preserved or erased. |
| Conversion Matrix | [conversion_matrix.md](#ch-language-reference-data-types-conversion-matrix-md) | Quick-reference matrix for which source-to-target conversions are implicit, require explicit casts, are lossy, or are refused. |

## Suggested Reading Order

Read `type_system_overview.md` first — it defines the vocabulary (descriptor,
carrier, domain, coercion) used by every other page in this chapter. Then read
the specific type pages relevant to your work. Finish with
`domains_casts_and_coercion.md` and `conversion_matrix.md` when you need to
understand how values move between types and domains.

Readers debugging a failed cast or an unexpected result type will find
`conversion_matrix.md` the fastest reference.

## Back To The Chapter List

[Language Reference](#ch-language-reference-readme-md)




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/data_types/type_system_overview.md -->

<a id="ch-language-reference-data-types-type-system-overview-md"></a>

# Type System Overview

This page is part of the SBsql Language Reference Manual. It explains the
descriptor model behind SBsql values, literals, parameters, columns, domains,
expressions, rows, streams, and result sets.

Generation task: `data_types_type_system_overview`

## Purpose

When you write a type name in SBsql — whether as a column definition, a cast,
a parameter, or a literal — the parser does not pass that text to the engine.
It resolves the spelling into a _descriptor_: a structured, engine-bound
description of how the value should be represented, compared, ordered, hashed,
indexed, stored, and rendered. The engine then works from the descriptor, not
from the original text. This is why behavior is consistent regardless of which
spelling or alias you used.

The type system distinguishes two layers:

- a _carrier descriptor_ defines the base representation and operations for a
  value (for example: a 64-bit signed integer, or UTF-8 text with a specific
  collation);
- a _domain_ is a named policy layer over a carrier or another domain — it can
  add null rules, constraints, and rounding policy without changing the
  underlying carrier.

A descriptor-bound expression can preserve a domain, erase a domain, or bind
to a new domain only when an operation policy says so. The engine receives
descriptor identity through SBLR and does not infer type behavior from SQL text
after binding.

## Descriptor Binding Flow

![diagram](/home/dcalford/CliWork/ScratchBird/docs/documentation/draft/Language_Reference/data_types/type_system_overview-1.svg)

Descriptor binding happens before execution. If a descriptor cannot be resolved
or a value cannot fit the descriptor, the statement returns a diagnostic rather
than allowing implicit best-effort behavior.

## Supported Type Families

| Family | Canonical SBsql Names | Main Use | Fixed Size Or Bounds |
| --- | --- | --- | --- |
| Null marker | `null` | Unknown or absent value before contextual typing. | No payload. Requires nullable target or explicit cast context. |
| Boolean | `boolean`, `bool` | Three-valued logic. | 1 byte logical payload plus null marker. |
| Signed integer | `int8`, `int16`, `int32`, `int64`, `int128`, `smallint`, `int`, `integer`, `bigint` | Exact signed whole numbers. | 1, 2, 4, 8, or 16 bytes. |
| Unsigned integer | `uint8`, `uint16`, `uint32`, `uint64`, `uint128` | Exact unsigned whole numbers. | 1, 2, 4, 8, or 16 bytes. |
| Decimal | `decimal(p,s)`, `numeric(p,s)`, `money` | Exact base-10, decimal floating, and money-like values. | Precision, scale, rounding, and display are descriptor-owned. |
| Approximate real | `real`, `double precision`, `float(p)` | Approximate binary floating values. | 4 or 8 byte descriptors in the portable profile. |
| Text | `char(n)`, `varchar(n)`, `text`, `clob`, `nchar(n)`, `nvarchar(n)`, `nclob` | Character data. | Character count, byte count, charset, collation, and overflow are descriptor-owned. |
| Binary | `binary(n)`, `varbinary(n)`, `blob`, `bytea` | Byte-oriented values and binary large values. | Byte count, overflow, and stream policy are descriptor-owned. |
| UUID | `uuid` | Application UUIDs and catalog identity references. | 16 bytes. |
| Temporal | `date`, `time(p)`, `timestamp(p)`, `timestamptz`, `interval` | Calendar, clock, instant, and duration values. | Precision, calendar, timezone, and range are descriptor-owned. |
| Document | `document`, `json_document`, `binary_json_document` | Structured document values and path-addressable data. | Payload and normalization profile are descriptor-owned. |
| Collection | `array<T>`, `multiset<T>`, `row(...)`, `record(...)` | Structured values, routine arguments, rowsets, and compound domains. | Element descriptors and shape descriptors own bounds. |
| Vector | `vector` | Fixed-dimension numerical vectors. | Dimension multiplied by element size plus descriptor metadata. |
| Spatial | `geometry`, `geography` | Spatial values, spatial predicates, and spatial indexes. | Shape, coordinate profile, and exact-recheck policy are descriptor-owned. |
| Graph | `graph`, `node`, `edge`, `path` | Graph data and traversal payloads. | Node, edge, path, and traversal descriptors own shape. |
| Search | `search_document`, `lexeme`, search-vector descriptors where admitted | Full-text/search payloads. | Tokenization and index profile are descriptor-owned. |
| Time-series | `timeseries`, `sample`, `bucket` descriptors where admitted | Time-series observations and windows. | Time key, value descriptor, and window profile are descriptor-owned. |
| Key-value | `kv_key`, `kv_value`, map-like descriptors | Key-value and map payloads. | Key descriptor, value descriptor, and ordering/hash policy are descriptor-owned. |
| Protected material | `secret_ref`, `protected_blob_ref`, protected descriptors | References to protected values and release-controlled material. | References only unless release policy admits raw access. |
| Domain | User-defined domain names | Named constraints, defaults, null policy, masks, and operation policy. | Underlying carrier plus domain policy. |

## Canonical Names And Aliases

SBsql can accept aliases, but binding resolves each spelling to a canonical
descriptor. After binding, the engine uses descriptor identity. The original
spelling remains useful for diagnostics and source references, not execution
authority.

Examples:

```sql
create table app.example_types (
    id uuid primary key,
    exact_count uint128,
    label varchar(120),
    payload document,
    embedding vector,
    created_at timestamptz
);
```

The table definition creates column descriptors for each column. Inserts,
updates, defaults, indexes, constraints, masks, and query projections use those
descriptors.

## Null Behavior

`null` has no standalone carrier. It adopts the target descriptor when the
target is known and nullable.

| Context | Rule |
| --- | --- |
| Assignment to nullable column | `null` is admitted as the target descriptor's null value. |
| Assignment to non-null target | Refused before storage mutation. |
| Function argument | Requires an overload that admits null for that argument. |
| Comparison | Uses three-valued logic unless the operator has explicit null-handling semantics. |
| Domain assignment | Domain null policy is checked before ordinary constraints. |
| Array, row, and document values | Element or field null behavior is descriptor-owned. |

Use an explicit cast when the target type cannot be inferred:

```sql
select cast(null as decimal(18,2)) as empty_amount;
```

## Literal Binding

Literals are parsed text until they bind to descriptors.

| Literal Form | Binding Rule |
| --- | --- |
| Integer literal | Binds by context or to the smallest admitted exact descriptor that can represent it. |
| Unsigned literal | Uses an explicit unsigned suffix or contextual unsigned target. |
| Decimal literal | Binds to an exact decimal descriptor unless context selects another admitted numeric descriptor. |
| String literal | Binds as text until context, cast, charset introducer, or target descriptor changes it. |
| Binary literal | Binds as bytes and does not carry charset or collation. |
| UUID literal | Binds to a 16-byte UUID descriptor. |
| Temporal literal | Binds only when context or explicit cast states the temporal descriptor. |
| Document literal | Binds through document/json descriptor rules. |
| Vector literal | Requires an element descriptor and dimension. |

Ambiguous or lossy literal binding is refused unless a documented conversion
policy admits it.

## Storage And Overflow

Descriptor validity is not the same as storage admission. A value can have a
valid descriptor and still be refused because it cannot fit the row, page,
overflow, stream, transaction, filespace, or policy limits.

| Concern | Controlled By |
| --- | --- |
| Inline row payload | Row descriptor, page size, null map, alignment, and storage policy. |
| Large values | Overflow/large-value descriptor and transaction policy. |
| Text byte size | Character set, encoded byte length, collation key policy, and overflow policy. |
| Binary byte size | Byte descriptor, overflow policy, and stream limits. |
| Index key size | Index descriptor, collation key, expression descriptor, and index policy. |
| Stream frames | Stream descriptor, frame limit, backpressure, timeout, and cancellation policy. |

## Type Authority In SBLR

SBLR carries descriptor tables. An execution envelope can include:

- column descriptors;
- parameter descriptors;
- literal descriptors;
- expression result descriptors;
- row and record descriptors;
- cursor descriptors;
- stream descriptors;
- domain stacks;
- conversion operations;
- result shapes;
- diagnostic shapes.

Server admission rejects malformed, missing, stale, contradictory, or
unsupported descriptor evidence before engine dispatch.

## Related Pages

- [Numeric Types](#ch-language-reference-data-types-numeric-types-md)
- [Text, Collation, And Charset](#ch-language-reference-data-types-text-collation-and-charset-md)
- [Temporal Types](#ch-language-reference-data-types-temporal-types-md)
- [Binary, UUID, And Protected Values](#ch-language-reference-data-types-binary-uuid-and-protected-values-md)
- [Document, Graph, Vector, And Multimodel Types](#ch-language-reference-data-types-document-graph-vector-and-multimodel-types-md)
- [Domains, Casts, And Coercion](#ch-language-reference-data-types-domains-casts-and-coercion-md)
- [Conversion Matrix](#ch-language-reference-data-types-conversion-matrix-md)
- Operator Type Result Matrix (SBsql Language Reference — Syntax, page XXX)
- [Parser To SBLR Pipeline](#ch-language-reference-core-paradigms-parser-to-sblr-pipeline-md)

## Verification Checklist

The type-system proof suite should demonstrate:

- every supported type spelling resolves to a canonical descriptor;
- unsupported aliases are refused rather than accepted as inert text;
- `null` binds only through a valid nullable target;
- fixed-width numeric ranges reject overflow and underflow;
- text length checks use character count while storage uses encoded byte count;
- collation affects comparison, grouping, ordering, and indexes consistently;
- temporal precision and timezone policy are descriptor-owned;
- UUID values store as 16 bytes and do not bypass authorization;
- protected material is carried by reference unless release policy admits it;
- document, graph, vector, spatial, search, time-series, and key-value indexes
  produce candidate evidence only;
- domains apply null policy, defaults, constraints, and operation policy in the
  documented order;
- SBLR envelopes carry descriptors rather than executable type text;
- stale descriptor or policy epochs invalidate dependent statements and plans.




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/data_types/numeric_types.md -->

<a id="ch-language-reference-data-types-numeric-types-md"></a>

# Numeric Types

This page is part of the SBsql Language Reference Manual. It defines the public
numeric descriptor families, ranges, literal binding rules, arithmetic result
rules, aggregate behavior, comparison behavior, indexing behavior, and
diagnostics.

Generation task: `data_types_numeric`

## Purpose

SBsql supports a range of numeric types from single-byte integers through
high-precision decimals and approximate reals. Choosing the right type is a
matter of knowing which precision and range you need, since the descriptor
chosen at bind time controls how arithmetic results are typed, how values
compare, and what happens at the boundary of the representable range.

Arithmetic is strict by default. Overflow, underflow, divide-by-zero,
unsupported precision, ambiguous signed/unsigned widening, invalid casts, and
unsupported special values return diagnostics rather than silent wraparound or
silent truncation. If you need a computation to succeed even when precision is
lost, use an explicit cast that states the conversion you intend.

## Supported Numeric Types

| Canonical Type | Common Aliases | Family | Payload | Value Range |
| --- | --- | --- | --- | --- |
| `int8` | none | signed integer | 1 byte | -128 to 127 |
| `uint8` | none | unsigned integer | 1 byte | 0 to 255 |
| `int16` | `smallint` | signed integer | 2 bytes | -32768 to 32767 |
| `uint16` | none | unsigned integer | 2 bytes | 0 to 65535 |
| `int32` | `int`, `integer` | signed integer | 4 bytes | -2147483648 to 2147483647 |
| `uint32` | none | unsigned integer | 4 bytes | 0 to 4294967295 |
| `int64` | `bigint` | signed integer | 8 bytes | -9223372036854775808 to 9223372036854775807 |
| `uint64` | none | unsigned integer | 8 bytes | 0 to 18446744073709551615 |
| `int128` | none | signed integer | 16 bytes | -170141183460469231731687303715884105728 to 170141183460469231731687303715884105727 |
| `uint128` | none | unsigned integer | 16 bytes | 0 to 340282366920938463463374607431768211455 |
| `decimal(p,s)` | `numeric(p,s)` | exact decimal | descriptor-dependent | `p` total digits and `s` fractional digits. |
| `decimal_float` | `decfloat` in compatibility dialects | decimal floating | descriptor-defined | Up to 34 decimal digits of precision; exponent and special values are descriptor-owned. |
| `real` | none | approximate real | 4 bytes | Binary32-style finite range with approximately 6 to 9 significant decimal digits. |
| `double precision` | `double` | approximate real | 8 bytes | Binary64-style finite range with approximately 15 to 17 significant decimal digits. |
| `float(p)` | none | approximate real | 4 or 8 bytes | `p` selects an admitted real descriptor under database policy. |
| `money` | none | domain over exact decimal | descriptor-dependent | Currency, scale, rounding, and rendering are domain or descriptor policy. |

`decimal(p,s)` admits precision and scale only when the active descriptor policy
supports them. The portable baseline is precision `1` through `38` and scale
`0` through `p`. Higher precision can be admitted by policy, but portable
scripts should declare only the precision they require and treat unsupported
precision as a bind-time diagnostic.

## Literal Binding

Numeric literals are not final type authority until binding.

| Literal Form | Binding Rule |
| --- | --- |
| `123` | Binds by context, or to the smallest admitted signed integer descriptor that can represent it. |
| `123U` | Binds to the default unsigned integer descriptor for the active policy. |
| `123U8`, `123U16`, `123U32`, `123U64`, `123U128` | Binds to the named unsigned width and rejects values outside that width. |
| `123I8`, `123I16`, `123I32`, `123I64`, `123I128` | Binds to the named signed width and rejects values outside that width. |
| `123.45` | Binds to an exact decimal descriptor unless context selects another admitted numeric descriptor. |
| `1.2e10` | Binds to an admitted exact or approximate descriptor according to context and literal policy. |
| `-123` | Parses as unary minus applied to a positive literal descriptor. It cannot bind to an unsigned descriptor unless an explicit conversion policy admits the final value, which ordinary unsigned descriptors do not. |
| String-to-number | Requires an explicit cast or assignment conversion. Invalid text is diagnostic. |

Examples:

```sql
select 123U128 as exact_unsigned_value;

select cast('340282366920938463463374607431768211455' as uint128)
       as max_uint128;
```

Negative text or numeric input cannot bind to `uint128`. Values greater than
`340282366920938463463374607431768211455` are refused as overflow.

## Arithmetic Result Rules

| Operation Class | Result Descriptor Rule |
| --- | --- |
| Signed integer plus/minus/multiply | Uses the admitted result descriptor selected by operand widths and context. Overflow is diagnostic. |
| Unsigned integer plus/multiply | Uses an unsigned result descriptor when admitted. Overflow is diagnostic. |
| Unsigned subtraction | Requires a signed or wider descriptor if the result can be negative, or refuses underflow. |
| Mixed signed/unsigned arithmetic | Requires a descriptor that can represent both operands and the operation result, or an explicit cast. Ambiguous widening is refused. |
| Integer division | Uses the operator form and descriptor policy. Divide-by-zero is diagnostic. |
| Decimal arithmetic | Derives precision and scale from operands. Refuses when the derived descriptor exceeds policy. |
| Approximate real arithmetic | Uses the wider approximate descriptor unless an explicit cast fixes the result. |
| Decimal plus approximate real | Requires an explicit cast unless the context selects an admitted lossy conversion policy. |
| Unary minus | Refuses for unsigned values that cannot be represented by the target descriptor. |
| Modulo | Uses exact integer or exact decimal descriptors only where the operation policy admits it. |
| Power | Uses a descriptor-specific operation; exactness and overflow are policy-owned. |

Silent wraparound is not an SBsql numeric behavior.

## Comparison And Ordering

Numeric comparison uses descriptor-aware comparison.

| Case | Rule |
| --- | --- |
| Same exact descriptor | Compare exact numeric values. |
| Widenable exact descriptors | Compare after exact widening. |
| Signed and unsigned | Compare only after an exact descriptor can represent both values. |
| Decimal and integer | Compare exactly when the decimal descriptor can represent the integer. |
| Approximate real | Compare according to approximate descriptor rules. NaN and infinity behavior is descriptor-owned. |
| Domain values | Use domain operation policy first, then carrier comparison where admitted. |

Numeric index keys must use the same comparison rule as expression evaluation.
An index can produce candidates, but final row visibility and predicate truth
still require engine recheck.

## Aggregates

Aggregate state can be wider than the displayed result.

| Aggregate | Rule |
| --- | --- |
| `count(*)` | Returns an exact integer descriptor large enough for admitted row counts. |
| `sum(integer)` | Uses a widened exact accumulator descriptor. Overflow is diagnostic unless a documented wider accumulator is available. |
| `sum(decimal)` | Uses a decimal accumulator with derived precision/scale. Policy can refuse unsupported precision. |
| `avg(integer)` | Returns a descriptor capable of fractional results. |
| `avg(decimal)` | Uses decimal result rules with derived precision/scale. |
| `min` and `max` | Preserve comparison descriptor and return a compatible descriptor. |
| Statistical aggregates | Use function-specific descriptors and diagnostics for unsupported inputs. |

Example:

```sql
select sum(invoice_total), avg(invoice_total)
from billing.invoice
where invoice_total > cast(0 as decimal(18,2));
```

## Assignment And Casts

Assignment to a numeric target follows this order:

1. bind the source descriptor;
2. bind the target descriptor or domain;
3. apply exact widening where admitted;
4. apply explicit cast policy if the statement requested it;
5. validate range, scale, precision, rounding, and domain constraints;
6. store or return the descriptor-bound value.

Examples:

```sql
create table app.measurement (
    measurement_id uuid primary key,
    count_exact uint128,
    amount decimal(18,2) not null,
    ratio double precision
);

insert into app.measurement (measurement_id, count_exact, amount, ratio)
values (:id, cast(:count_text as uint128), cast(:amount as decimal(18,2)), :ratio);
```

## Diagnostics

| Condition | Required Result |
| --- | --- |
| Overflow or underflow | Numeric diagnostic before silent wrap or truncation. |
| Divide by zero | Numeric diagnostic. |
| Decimal precision/scale unsupported | Bind or execution diagnostic, depending on when the derived descriptor is known. |
| Ambiguous signed/unsigned result | Bind diagnostic requiring explicit cast. |
| Fractional value cast to integer | Diagnostic unless an explicit rounding function is used. |
| NaN/infinity to exact numeric | Diagnostic unless descriptor policy explicitly admits a mapping. |
| Invalid text-to-number cast | Conversion diagnostic. |
| Domain constraint failure | Domain diagnostic after carrier conversion. |

## Syntax Productions

```ebnf
numeric_type            ::= signed_integer_type
                          | unsigned_integer_type
                          | decimal_type
                          | real_type
                          | money_type ;
```

```ebnf
signed_integer_type     ::= "int8" | "int16" | "smallint"
                          | "int32" | "int" | "integer"
                          | "int64" | "bigint"
                          | "int128" ;
```

```ebnf
unsigned_integer_type   ::= "uint8" | "uint16" | "uint32"
                          | "uint64" | "uint128" ;
```

```ebnf
decimal_type            ::= ("decimal" | "numeric") "(" precision "," scale ")"
                          | "decimal_float" ;
```

```ebnf
real_type               ::= "real"
                          | "double" "precision"
                          | "float" "(" precision ")" ;
```

## Related Pages

- [Type System Overview](#ch-language-reference-data-types-type-system-overview-md)
- [Conversion Matrix](#ch-language-reference-data-types-conversion-matrix-md)
- [Domains, Casts, And Coercion](#ch-language-reference-data-types-domains-casts-and-coercion-md)
- Operator Type Result Matrix (SBsql Language Reference — Syntax, page XXX)

## Verification Checklist

The numeric proof suite should demonstrate:

- every numeric spelling resolves to the expected descriptor;
- signed and unsigned ranges reject out-of-range values;
- `uint128` accepts the documented maximum and rejects maximum-plus-one;
- negative input refuses unsigned assignment;
- decimal precision and scale are enforced;
- integer, decimal, and approximate arithmetic use documented result rules;
- aggregate state widens according to descriptor rules;
- ambiguous mixed signed/unsigned arithmetic is refused;
- explicit casts and assignment conversions produce identical validation for the
  same target descriptor;
- numeric indexes use the same comparison rule as expression evaluation;
- diagnostics never silently wrap, truncate, or coerce lossy values.




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/data_types/text_collation_and_charset.md -->

<a id="ch-language-reference-data-types-text-collation-and-charset-md"></a>

# Text, Collation, And Charset

This page is part of the SBsql Language Reference Manual. It defines SBsql text
descriptors, character set rules, collation behavior, length semantics,
comparison behavior, index behavior, casts, and diagnostics.

Generation task: `data_types_text_collation`

## Purpose

Storing and comparing text correctly requires knowing more than the number of
characters. Every text value in SBsql carries two descriptor properties: a
_character set_ (the encoding used to store bytes) and a _collation_ (the rules
for ordering, comparing, and making values unique). The descriptor, not the
spelling of the SQL type, controls storage encoding, character length,
comparison, ordering, grouping, uniqueness, pattern matching, hash keys, index
keys, generated columns, masks, and result rendering.

If you compare two text values from columns with different collations without an
explicit collation clause, the engine reports a collation mismatch rather than
silently choosing one. This keeps results deterministic and avoids surprising
behavior when collations differ between databases or sessions.

Binary values are not text. A byte string can become text only through an
explicit conversion that states an encoding or through an admitted assignment
policy.

## Supported Text Types

| Canonical Type | Common Aliases | Length Unit | Payload | Value Bounds |
| --- | --- | --- | --- | --- |
| `char(n)` | `character(n)` | characters | Fixed-length encoded text padded according to descriptor policy. | Exactly `n` characters after padding rules. |
| `varchar(n)` | `character varying(n)` | characters | Variable-length encoded text. | 0 through `n` characters. |
| `text` | none | characters | Variable-length encoded text with overflow where admitted. | Policy bounded by row, page, overflow, and stream limits. |
| `clob` | `character large object` | characters or stream chunks | Character large-value stream. | Policy bounded by large-value and stream limits. |
| `nchar(n)` | national fixed character | characters | Fixed-length national-character text. | Exactly `n` characters under the national charset descriptor. |
| `nvarchar(n)` | national varying character | characters | Variable-length national-character text. | 0 through `n` characters under the national charset descriptor. |
| `nclob` | national character large object | characters or stream chunks | National-character large-value stream. | Policy bounded by large-value and stream limits. |

Declared length is a character count. Storage uses encoded bytes plus row,
descriptor, and overflow metadata. A value can satisfy the declared character
count and still be refused if the encoded byte count, collation key, row size,
index key size, stream limit, or policy limit cannot admit it.

## Character Sets

Every text descriptor has a character set.

| Charset Source | Rule |
| --- | --- |
| Database default charset | Used when a declaration omits `character set`. |
| Column charset | Stored in the column descriptor and used by assignment, comparison, functions, indexes, and rendering. |
| Literal introducer | Binds a literal to a charset before coercion. Unsupported or lossy conversion is refused unless policy admits it. |
| National character set | Used by `nchar`, `nvarchar`, and `nclob`. |
| Result expression | Derived from operands, casts, functions, and collation/charset rules. Ambiguity is refused. |

Example:

```sql
create table app.customer (
    customer_id uuid primary key,
    display_name varchar(120) character set utf8 collate default,
    legal_name nvarchar(240)
);
```

The spelling of `utf8` and the national-character descriptor bind to catalog
descriptors. The engine does not rely on the text spelling after binding.

## Collations

Collation is part of the descriptor and affects equality, ordering, grouping,
uniqueness, joins, indexes, and some string functions.

| Collation Concern | Behavior |
| --- | --- |
| Default collation | Applied when neither the column nor expression states a collation. |
| Explicit `collate` | Overrides expression collation for that expression and can define an index key collation. |
| Deterministic comparison | Required for equality, uniqueness, grouping, and B-tree ordering unless a provider proof admits otherwise. |
| Case sensitivity | Descriptor-owned. Do not infer it from display spelling. |
| Accent sensitivity | Descriptor-owned. |
| Normalization | Descriptor-owned. Comparisons must not silently normalize outside the collation contract. |
| Null ordering | Owned by query/index ordering rules, not by text collation alone. |

Example:

```sql
select customer_id, display_name
from app.customer
where display_name collate default = :name
order by display_name collate default;
```

## Length Functions

Text has multiple length concepts.

| Function Class | Meaning |
| --- | --- |
| Character length | Number of characters under the descriptor character set. |
| Octet length | Number of encoded bytes. |
| Collation key length | Internal comparison key length where the collation uses one. |
| Large-value length | Logical character length and/or stream byte length according to descriptor policy. |

Portable scripts should use the function that matches the intended limit rather
than assuming one length measure implies another.

## Assignment And Padding

Assignment to a text target follows this order:

1. bind source descriptor;
2. bind target text or domain descriptor;
3. convert charset if admitted;
4. apply target length rule;
5. apply padding or trimming only where the descriptor says it is allowed;
6. validate domain constraints and policy;
7. store or return the descriptor-bound value.

`char(n)` padding is descriptor-owned. `varchar(n)` and `text` do not imply
padding. Silent truncation is not admitted by default.

## Text Comparison And Indexes

Text indexes use descriptor-aware keys. A text index can accelerate candidate
selection, but final result admission still requires engine recheck.

| Feature | Rule |
| --- | --- |
| Equality | Uses the active text descriptor's collation and normalization rules. |
| Ordering | Uses the active collation and query/index null-order rules. |
| Grouping | Uses the same equality semantics as the descriptor. |
| Unique indexes | Use deterministic collation keys; non-deterministic collations must be refused unless explicitly admitted. |
| Prefix/range indexes | Must preserve the descriptor's ordering and exact recheck requirements. |
| Pattern matching | Uses the descriptor, function/operator policy, and optional collation profile. |
| Full-text/search | Uses search descriptors and tokenization profiles, not ordinary text equality alone. |

Example:

```sql
create index app.customer_name_ix
on app.customer (display_name collate default);
```

## Text Literals

| Literal Form | Binding Rule |
| --- | --- |
| `'text'` | Binds as text under the active literal charset until context chooses a target descriptor. |
| `N'text'` | Binds as national-character text. |
| Character set introducer | Binds the literal to the stated charset before target coercion. |
| Escaped literal | Escape rules are parser-profile controlled and must lower to canonical text bytes. |
| Concatenation | Result descriptor derives charset, collation, and length from operands and operation policy. |

When the target is `uuid`, numeric, temporal, binary, document, or protected
material, a string literal remains text until an explicit cast or assignment
conversion admits the target.

## Text And Protected Values

Text can carry sensitive data. Protected material policy can block rendering,
logging, support-bundle output, casts, concatenation, export, backup,
replication, bridge output, or diagnostic text.

Masking a text column does not change the stored value. It changes the rendered
result under policy.

## Diagnostics

| Condition | Required Result |
| --- | --- |
| Invalid encoded bytes | Conversion diagnostic. |
| Unsupported charset | Bind diagnostic. |
| Unsupported collation | Bind diagnostic. |
| Lossy charset conversion | Diagnostic unless explicitly admitted. |
| Character length exceeded | Assignment diagnostic. |
| Encoded byte limit exceeded | Storage or stream diagnostic. |
| Index key too large | DDL or DML diagnostic according to when the value is known. |
| Non-deterministic collation used for unique key | DDL refusal unless admitted by policy. |
| Binary passed to text function without conversion | Bind diagnostic. |
| Protected value rendered without release authority | Denied message vector. |

## Syntax Productions

```ebnf
text_type               ::= fixed_text_type
                          | varying_text_type
                          | large_text_type
                          | national_text_type ;
```

```ebnf
fixed_text_type         ::= ("char" | "character") "(" length ")" text_type_options? ;
varying_text_type       ::= ("varchar" | "character" "varying") "(" length ")" text_type_options? ;
large_text_type         ::= "text" text_type_options?
                          | "clob" text_type_options? ;
national_text_type      ::= "nchar" "(" length ")"
                          | "nvarchar" "(" length ")"
                          | "nclob" ;
```

```ebnf
text_type_options       ::= character_set_clause? collation_clause? ;
character_set_clause    ::= "character" "set" charset_ref ;
collation_clause        ::= "collate" collation_ref ;
```

## Related Pages

- [Type System Overview](#ch-language-reference-data-types-type-system-overview-md)
- [Conversion Matrix](#ch-language-reference-data-types-conversion-matrix-md)
- [Domains, Casts, And Coercion](#ch-language-reference-data-types-domains-casts-and-coercion-md)
- Operator Type Result Matrix (SBsql Language Reference — Syntax, page XXX)
- Policy, Mask, And RLS Lifecycle (SBsql Language Reference — Syntax, page XXX)

## Verification Checklist

The text proof suite should demonstrate:

- declared character length is enforced separately from encoded byte length;
- charset conversion refuses unsupported or lossy conversions by default;
- collation affects equality, ordering, grouping, and index behavior
  consistently;
- quoted and unquoted literals bind to the expected descriptors;
- `char(n)` padding follows descriptor policy;
- `varchar(n)` rejects over-length values without silent truncation;
- text indexes use the same comparison rule as expression evaluation;
- non-deterministic collations are refused for features requiring deterministic
  keys unless an admitted proof exists;
- binary values require explicit conversion before text operations;
- protected text is redacted or denied according to policy.




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/data_types/temporal_types.md -->

<a id="ch-language-reference-data-types-temporal-types-md"></a>

# Temporal Types

This page is part of the SBsql Language Reference Manual. It defines date,
time, timestamp, timezone, interval, precision, literal, arithmetic, comparison,
indexing, and diagnostic behavior for temporal descriptors.

Generation task: `data_types_temporal`

## Purpose

Temporal values are descriptor-bound. A temporal descriptor states whether a
value is a calendar date, clock time, timestamp without timezone, instant with
timezone rendering, or duration. It also owns precision, calendar policy,
timezone policy, comparison, arithmetic, indexing, and rendering.

Portable scripts should avoid relying on client-local display rules. Use
explicit casts, explicit precision, named extraction parts, and timezone-aware
types when the instant matters.

## Supported Temporal Types

| Canonical Type | Common Aliases | Logical Payload | SQL-Visible Contract |
| --- | --- | --- | --- |
| `date` | none | Day ordinal under descriptor calendar policy. | Calendar date without time of day or timezone. |
| `time(p)` | `time` | Time of day with fractional precision `p`. | Clock time without date or timezone. |
| `time(p) with time zone` | `time with time zone` | Time of day plus timezone or offset descriptor data. | Clock time with timezone rendering/comparison policy. |
| `timestamp(p)` | none | Date plus time fields with fractional precision `p`. | Timestamp fields without timezone normalization. |
| `timestamptz` | `timestamp_tz` | Instant plus timezone rendering policy. | Stored and compared as an instant; rendered through session/profile timezone policy. |
| `interval` | `interval year to month`, `interval day to second` | Duration fields selected by descriptor. | Duration, not a calendar instant. |

The portable fractional-second precision is `0` through `6`. Higher precision
can be admitted by database policy, but scripts that require portability should
declare the precision they require and treat unsupported precision as a
bind-time diagnostic.

## Date, Time, Timestamp, And Instant

| Type | Timezone Meaning |
| --- | --- |
| `date` | No timezone component. A date is not shifted by session timezone. |
| `time(p)` | No date and no timezone component. |
| `time(p) with time zone` | Includes timezone or offset behavior defined by descriptor policy. |
| `timestamp(p)` | Stores date/time fields without timezone normalization. Session timezone does not change the stored value. |
| `timestamptz` | Represents an instant. Session timezone affects rendering, not stored instant identity. |
| `interval` | Has no timezone; it is applied to a temporal value according to arithmetic rules. |

Use `timestamptz` when the value must represent a real instant across sessions.
Use `timestamp` when the stored date/time fields are intended to remain local fields.

## Precision

Precision `p` controls fractional seconds.

| Rule | Behavior |
| --- | --- |
| Omitted precision | Uses the SBsql default temporal descriptor. |
| Supported precision | Binds to the exact descriptor. |
| Unsupported precision | Refused at bind time. |
| Assignment to lower precision | Refused unless an explicit rounding/truncation policy or function is used. |
| Cast to lower precision | Uses explicit cast policy; silent precision loss is not default behavior. |

## Literals And Casts

String literals remain text until a temporal context or explicit cast binds
them to a temporal descriptor.

```sql
select cast('2026-06-08' as date) as business_date;

select cast('2026-06-08 14:30:00.123456' as timestamp(6))
       as local_event_time;

select cast('2026-06-08 14:30:00.123456-04:00'
            as timestamptz)
       as event_instant;
```

Invalid calendar fields, invalid time fields, unsupported timezone names,
unsupported precision, and ambiguous literal forms return diagnostics.

## Current Temporal Functions

Current-time functions bind to engine expression operations. The descriptor must
make the timestamp source explicit.

| Function Class | Rule |
| --- | --- |
| Transaction timestamp | Stable for the transaction where the function contract says so. |
| Statement timestamp | Stable for the statement where the function contract says so. |
| Clock timestamp | Reads a current clock source where admitted. |
| Current date/time | Derived from the session timezone and descriptor policy. |
| Timezone conversion | Uses named timezone descriptors or offset descriptors. |

Current-time functions must be testable. They should not silently use a client
display clock as engine authority.

## Temporal Arithmetic

| Operation | Result Rule |
| --- | --- |
| `date + interval` | Produces a date or timestamp according to interval fields and target descriptor. |
| `timestamp + interval` | Produces a timestamp descriptor compatible with the input timestamp. |
| `timestamptz + interval` | Produces an instant descriptor with timezone rendering policy preserved. |
| `date - date` | Produces an interval or integer day count according to operator descriptor. |
| `timestamp - timestamp` | Produces an interval descriptor. |
| `time - time` | Produces an interval descriptor where admitted. |
| `interval + interval` | Produces an interval descriptor when fields are compatible. |
| `interval * numeric` | Uses interval scaling policy and refuses unsupported fractional or overflow results. |

Ambiguous units are refused. For example, adding a month interval can depend on
calendar policy and target date. The descriptor must own that behavior.

## Extraction, Truncation, And Formatting

Extraction and truncation bind named parts to operation descriptors.

| Operation Class | Rule |
| --- | --- |
| Extract | Field names such as year, month, day, hour, minute, second, timezone offset, or epoch bind to a descriptor-owned operation. |
| Truncation | `date_trunc`-style operations return a descriptor compatible with the input and requested part. |
| Formatting | Text rendering is explicit and profile-owned. Formatting is not storage authority. |
| Parsing | Text parsing uses explicit cast or conversion functions and fails on invalid input. |

Example:

```sql
select date_trunc('day', created_at) as event_day,
       count(*) as event_count
from app.event_log
group by date_trunc('day', created_at);
```

## Comparison And Indexes

| Type | Comparison Rule |
| --- | --- |
| `date` | Compare calendar ordinal under descriptor calendar policy. |
| `time` | Compare time-of-day under descriptor precision. |
| `time with time zone` | Compare according to descriptor timezone policy. |
| `timestamp` | Compare stored date/time fields without timezone conversion. |
| `timestamptz` | Compare instants. Rendering timezone does not change ordering. |
| `interval` | Compare only where the interval descriptor admits a total ordering. |

Temporal indexes use the same comparison rule as expression evaluation. Indexes
produce candidate evidence; final row visibility still requires MGA, predicate,
descriptor, and security recheck.

## Timezone Policy

Timezone behavior is descriptor-owned.

| Concern | Rule |
| --- | --- |
| Session timezone | Used for rendering and current date/time functions where the descriptor says so. |
| Stored timezone | May be stored as offset, named zone, normalized instant, or descriptor metadata according to policy. |
| Timezone database | Versioned timezone data can change rendering and must be tracked as descriptor or resource metadata. |
| Ambiguous local time | Refused or resolved according to explicit policy. |
| Nonexistent local time | Refused or resolved according to explicit policy. |

## Diagnostics

| Condition | Required Result |
| --- | --- |
| Invalid date or time field | Conversion diagnostic. |
| Unsupported precision | Bind diagnostic. |
| Precision loss | Diagnostic unless explicit cast policy admits it. |
| Unsupported timezone | Bind or conversion diagnostic. |
| Ambiguous or nonexistent local time | Diagnostic unless descriptor policy explicitly resolves it. |
| Temporal arithmetic overflow | Diagnostic. |
| Interval field mismatch | Bind diagnostic. |
| Text parsed as temporal without explicit context | Ambiguous bind diagnostic. |

## Syntax Productions

```ebnf
temporal_type           ::= date_type
                          | time_type
                          | timestamp_type
                          | interval_type ;
```

```ebnf
date_type               ::= "date" ;
```

```ebnf
time_type               ::= "time" precision_clause? ;
timestamp_type          ::= "timestamp" precision_clause?
                          | "timestamptz" ;
```

```ebnf
interval_type           ::= "interval" interval_qualifier? ;
```

## Related Pages

- [Type System Overview](#ch-language-reference-data-types-type-system-overview-md)
- [Conversion Matrix](#ch-language-reference-data-types-conversion-matrix-md)
- [Domains, Casts, And Coercion](#ch-language-reference-data-types-domains-casts-and-coercion-md)
- Operator Type Result Matrix (SBsql Language Reference — Syntax, page XXX)
- Transaction Control (SBsql Language Reference — Syntax, page XXX)

## Verification Checklist

The temporal proof suite should demonstrate:

- each temporal spelling resolves to the expected descriptor;
- fractional precision is enforced;
- text literals require context or explicit cast;
- invalid dates, times, and timezones are refused;
- `timestamp` and `timestamptz` differ in storage and comparison behavior;
- session timezone affects rendering where documented and not stored instant
  identity;
- temporal arithmetic uses documented result descriptors;
- interval field compatibility is enforced;
- temporal indexes use the same comparison rule as execution;
- current-time functions use engine-owned timestamp sources;
- timezone-data changes invalidate dependent rendering or planning state where
  required.




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/data_types/binary_uuid_and_protected_values.md -->

<a id="ch-language-reference-data-types-binary-uuid-and-protected-values-md"></a>

# Binary, UUID, And Protected Values

This page is part of the SBsql Language Reference Manual. It defines binary
descriptors, UUID descriptors, catalog UUID references, large binary values,
protected-material references, conversion behavior, security behavior, and
diagnostics.

Generation task: `data_types_binary_uuid_protected`

## Purpose

Binary values are byte sequences. They do not carry character set, collation, or
text rendering behavior. UUID values are 16-byte descriptors that can be used as
application values or as catalog identity references. Protected values are
security-controlled references to sensitive material and must not be treated as
ordinary text or binary data.

The binder must know which of these categories a value belongs to before SBLR
admission.

## Supported Binary And Identity Types

| Canonical Type | Common Aliases | Unit | Payload | Bounds |
| --- | --- | --- | --- | --- |
| `binary(n)` | fixed byte string | bytes | Exactly `n` bytes plus descriptor metadata. | Exactly `n` bytes. |
| `varbinary(n)` | `binary varying(n)` | bytes | 0 through `n` bytes plus descriptor metadata. | 0 through `n` bytes. |
| `blob` | `binary large object` | bytes or stream chunks | Binary large-value stream or overflow value. | Policy bounded by row, page, overflow, stream, and transaction limits. |
| `bytea` | byte array alias where admitted | bytes | Variable byte payload. | Policy bounded. |
| `uuid` | UUID literal and UUID-valued columns | 16 bytes | RFC-style UUID bytes. | Exactly 16 bytes. |
| `secret_ref` | protected secret reference | UUID plus metadata | Reference to protected material. | Raw secret is not carried in ordinary values. |
| `protected_blob_ref` | protected binary reference | UUID plus metadata | Reference to protected binary material. | Raw payload release requires policy. |

## Binary Values

Binary values are byte descriptors. They can be compared, hashed, stored,
indexed, streamed, and rendered only through binary-aware operations.

| Operation | Rule |
| --- | --- |
| Equality | Byte-wise descriptor comparison unless a domain or operation policy overrides it. |
| Ordering | Byte-ordering only where a binary descriptor admits ordering. |
| Hashing | Uses binary descriptor hash rules. |
| Text functions | Refused unless an explicit conversion supplies charset/encoding. |
| Pattern matching | Requires a binary-pattern operation, not text collation. |
| Indexing | Uses binary descriptor keys and exact recheck where required. |
| Large values | Use overflow or stream descriptors; inline row storage is not assumed. |

Example:

```sql
create table app.file_store (
    file_id uuid primary key,
    digest binary(32) not null,
    payload blob
);
```

## Binary Literals And Encoding

Binary literal syntax binds to byte descriptors. Text literal syntax does not
become binary without an explicit conversion.

| Form | Binding Rule |
| --- | --- |
| Binary literal | Binds as bytes under the active binary literal profile. |
| Hex text converted to binary | Requires explicit decode or cast function. |
| Binary converted to text | Requires explicit encoding or charset conversion. |
| Large binary stream | Requires a stream descriptor and policy-admitted frame limits. |

Invalid hex, invalid base encoding, unsupported encoding, over-length values,
and binary/text confusion return diagnostics.

## UUID Values

UUID values store as 16 bytes. Default rendering is canonical lower-case text
with hyphens.

| Rule | Behavior |
| --- | --- |
| Scalar UUID | Application data value with UUID descriptor. |
| Catalog UUID reference | Identity evidence for a catalog object. Requires object-class, visibility, sandbox, authorization, and policy checks. |
| Literal syntax | `uuid '<canonical-text>'` binds a UUID value. |
| String literal | Remains text until a cast or target descriptor binds it as UUID. |
| Comparison | Uses UUID descriptor comparison. |
| Indexing | UUID indexes use UUID descriptor keys and still require MGA/security recheck. |

Example:

```sql
select cast('018f0000-0000-7000-8000-000000000001' as uuid) as object_id;
```

Example catalog reference:

```sql
describe table uuid '018f0000-0000-7000-8000-000000000001';
```

Knowing a UUID does not grant access. The resolved object must be visible and
authorized.

## Protected Values

Protected values are not ordinary binary or text values. They are references to
material whose release is controlled by security policy.

Protected material can include:

- secrets;
- credentials;
- keys;
- tokens;
- encrypted payload handles;
- protected binary values;
- protected text values;
- sensitive diagnostic fields;
- support-bundle evidence references.

Rules:

- raw secret material must not appear in ordinary parser packets;
- raw secret material must not appear in SBLR payloads except in an explicitly
  protected envelope admitted by policy;
- support bundles, logs, diagnostics, catalog display, and bridge messages
  redact protected material by default;
- casts from protected material to raw text or raw binary are denied unless an
  explicit release surface admits them;
- export, backup, replication, migration, bridge, and stream routes are release
  surfaces when protected values can cross a boundary;
- release should produce audit evidence where policy requires it.

## Protected References

A protected reference can be stored or passed without exposing the raw value.

| Reference | Meaning |
| --- | --- |
| `secret_ref` | Reference to secret material managed by an admitted provider or protected catalog. |
| `protected_blob_ref` | Reference to protected binary material. |
| Protected descriptor UUID | Descriptor controlling release, masking, rotation, expiry, and audit behavior. |
| Policy binding | Rule that decides who can resolve, rotate, release, export, or inspect the protected material. |

Authorized inspection can return redacted metadata such as owner, status,
rotation time, expiry time, reachability, and policy identity. It must not
return the raw protected value unless release authority is explicitly admitted.

## Conversion Rules

| Conversion | Default Rule |
| --- | --- |
| `binary` to `text` | Explicit encoding or charset conversion required. |
| `text` to `binary` | Explicit encoding, decode function, or binary assignment policy required. |
| `text` to `uuid` | Explicit cast or UUID target required; invalid text is diagnostic. |
| `uuid` to `text` | Explicit cast renders canonical UUID text. |
| `uuid` to `binary(16)` | Explicit cast required. |
| `binary(16)` to `uuid` | Explicit cast required and validates UUID descriptor policy. |
| Protected reference to raw value | Denied unless an explicit release route admits it. |
| Raw value to protected reference | Requires an admitted protect/store route, not an ordinary cast. |

## Large Binary Values And Streams

Large binary values use overflow or stream descriptors. They are not guaranteed
to fit inline in a row.

Stream contracts define:

- frame type;
- maximum frame size;
- maximum in-flight bytes;
- timeout;
- cancellation behavior;
- retry behavior;
- checksum or digest policy where required;
- transaction ownership;
- protected-material release behavior;
- completion diagnostics.

## Diagnostics

| Condition | Required Result |
| --- | --- |
| Binary length mismatch for `binary(n)` | Assignment diagnostic. |
| `varbinary(n)` length exceeded | Assignment diagnostic. |
| Large binary exceeds stream or overflow policy | Stream/storage diagnostic. |
| Invalid binary literal encoding | Parse or conversion diagnostic. |
| Binary used as text without conversion | Bind diagnostic. |
| UUID text invalid | Conversion diagnostic. |
| UUID object reference wrong class | Bind/admission diagnostic. |
| UUID object hidden or outside sandbox | Denied or redacted not-visible diagnostic. |
| Protected material rendered without authority | Denied message vector. |
| Protected material appears in support output | Test failure; output must be redacted. |

## Syntax Productions

```ebnf
binary_type             ::= fixed_binary_type
                          | varying_binary_type
                          | large_binary_type ;
```

```ebnf
fixed_binary_type       ::= "binary" "(" length ")" ;
varying_binary_type     ::= "varbinary" "(" length ")"
                          | "binary" "varying" "(" length ")" ;
large_binary_type       ::= "blob"
                          | "binary" "large" "object"
                          | "bytea" ;
```

```ebnf
uuid_type               ::= "uuid" ;
uuid_literal            ::= "uuid" string_literal ;
uuid_ref                ::= "UUID" string_literal ;
```

```ebnf
protected_type          ::= "secret_ref"
                          | "protected_blob_ref" ;
```

## Related Pages

- [Type System Overview](#ch-language-reference-data-types-type-system-overview-md)
- [UUID Catalog Identity](#ch-language-reference-core-paradigms-uuid-catalog-identity-md)
- [Security And Sandboxing](#ch-language-reference-core-paradigms-security-and-sandboxing-md)
- [Conversion Matrix](#ch-language-reference-data-types-conversion-matrix-md)
- COPY Streaming Import And Export (SBsql Language Reference — Syntax, page XXX)
- Backup, Restore, Replication, And Migration (SBsql Language Reference — Syntax, page XXX)

## Verification Checklist

The binary/UUID/protected-value proof suite should demonstrate:

- fixed binary values require exactly the declared byte count;
- variable binary values reject values above the declared byte count;
- binary values do not accidentally use text charset or collation;
- binary-to-text conversion requires an explicit encoding or charset rule;
- UUID values store and compare as 16-byte descriptors;
- UUID catalog references require object-class and authorization checks;
- knowing an object UUID does not bypass sandboxing;
- large binary streams enforce frame and transaction limits;
- protected references do not expose raw values in ordinary result sets;
- logs, diagnostics, support bundles, bridge messages, and catalog projections
  redact protected material by default;
- release routes produce explicit authorization and audit evidence where policy
  requires it.




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/data_types/document_graph_vector_and_multimodel_types.md -->

<a id="ch-language-reference-data-types-document-graph-vector-and-multimodel-types-md"></a>

# Document, Graph, Vector, And Multimodel Types

This page is part of the SBsql Language Reference Manual. It defines descriptor
behavior for document, collection, vector, spatial, graph, search, time-series,
and key-value values.

Generation task: `data_types_document_graph_vector`

## Purpose

SBsql includes relational, document, graph, vector, search, time-series, and
key-value surfaces. These are not separate engines. They use the same parser,
descriptor, SBLR, catalog, MGA transaction, security, policy, storage, index,
and recovery rules as other SBsql operations.

The common rule is:

> Multimodel indexes and providers produce candidate evidence. They do not own
> final row authority.

Final result delivery still requires engine recheck of descriptor compatibility,
MGA visibility, security, policy, predicate truth, ordering requirements, and
result shape.

## Supported Multimodel Types

| Canonical Type | Family | Payload | Primary Operations | Required Recheck |
| --- | --- | --- | --- | --- |
| `json_document` | document | Text-preserving JSON document value. | JSON extraction, path predicates, rendering. | Path, scalar type, predicate, MGA, and security. |
| `binary_json_document` | document | Normalized binary JSON document value. | Containment, path extraction, comparison where admitted, indexing. | Path, scalar type, predicate, MGA, and security. |
| `document` | document | Document payload plus schema/profile metadata. | Document insert, update, query, patch, path indexes. | Document identity, path semantics, MGA, and policy. |
| `array<T>` | collection | Ordered homogeneous elements. | Element access, unnesting, containment, assignment. | Element descriptor and bounds. |
| `multiset<T>` | collection | Unordered homogeneous elements with multiplicity. | Bag operations and aggregate-style element operations. | Element descriptor and multiplicity. |
| `row(...)` | structured | Named or positional field descriptors. | Row constructors, function returns, result descriptors. | Field descriptor and null policy. |
| `record(...)` | structured | Runtime record shape. | Procedural variables, cursor rows, dynamic rowsets. | Shape descriptor and policy. |
| `vector` (element type `real32`) | vector | `4 * n` bytes plus descriptor metadata. | Vector search and vector functions. | Exact vector, metric, MGA, security, and predicate. |
| `vector` (element type `real16`/`bfloat16`) | vector | `2 * n` bytes plus descriptor metadata. | Vector search where fp16 is admitted. | Exact source vector or quantization proof, plus MGA/security. |
| `vector` (element type `int8`) | vector | `n` bytes plus descriptor metadata. | Quantized vector search where admitted. | Quantization proof, exact rerank where required, MGA/security. |
| `geometry` | spatial | Geometry payload plus spatial reference/profile metadata. | Spatial predicates and indexes. | Exact geometry predicate and visibility. |
| `geography` | spatial | Geodetic payload plus spatial reference/profile metadata. | Geodetic predicates and indexes. | Exact geodetic predicate and visibility. |
| `graph` | graph | Graph container or traversal payload. | Node, edge, path, and traversal operations. | Node/edge/path identity, traversal order, MGA/security. |
| `node` | graph | Node identity and properties. | Node lookup and traversal. | Node identity and visibility. |
| `edge` | graph | Edge identity, endpoints, and properties. | Edge lookup and traversal. | Endpoint, edge identity, and visibility. |
| `path` | graph | Ordered traversal result. | Path queries and graph pattern results. | Traversal order and visibility. |
| `search_document` | search | Tokenized or tokenizable search payload. | Full-text predicates, ranking, highlighting where admitted. | Source document, token profile, ranking policy, MGA/security. |
| `timeseries` | time-series | Time-keyed observations or windows. | Windowing, resampling, interpolation where admitted. | Time key, order, window descriptor, MGA/security. |
| `kv_key`, `kv_value` | key-value | Key/value payload descriptors. | Get, put, delete, iterate, range, sorted-set style operations where admitted. | Key descriptor, value descriptor, ordering/hash, MGA/security. |

## Document Types

Document descriptors own path semantics.

| Concern | Rule |
| --- | --- |
| Missing versus null | Missing path and JSON null are distinct where the operator requires it. |
| Object key order | Preserved or normalized according to descriptor. |
| Scalar extraction | Extracted scalars bind to target descriptors through explicit or assignment conversion. |
| Path expressions | Bind to descriptor-aware operations; path text is not execution authority. |
| Patch/update | Validates target path, mutation policy, and resulting document descriptor. |
| Indexing | Document indexes produce candidate rows and path evidence; executor recheck remains mandatory. |

Example:

```sql
select document_id,
       cast(payload->>'created_at' as timestamptz) as created_at
from app.ingest_document
where payload @? '$.status == "ready"';
```

The exact path operator surface is descriptor-bound. Invalid paths, unsupported
path features, and scalar conversion failures return diagnostics.

## Collections And Structured Values

Collections and structured values are descriptor shapes.

| Type | Rule |
| --- | --- |
| `array<T>` | Ordered and position-addressable. Element descriptor applies to every element. |
| `multiset<T>` | Unordered and multiplicity-aware. Equality and containment are descriptor-owned. |
| `row(...)` | Fixed field shape used for row constructors and result descriptors. |
| `record(...)` | Runtime field shape used for procedural values and dynamic rowsets. |

Element nullability, domain validation, masking, and protected-material rules
apply to elements and fields.

## Vector Types

Vector descriptors must state:

- element profile;
- dimension;
- metric;
- provider family;
- normalization requirements;
- quantization profile where used;
- exact-recheck requirements;
- index generation;
- metric/resource epoch;
- result ordering rule.

| Provider Family | Contract |
| --- | --- |
| Exact vector | Computes exact candidates and still returns candidate evidence only. |
| Approximate graph-style vector index | Must exact-rerank or provide proof that exact rerank occurred before final result delivery. |
| Inverted-file style vector index | Must carry training generation, descriptor epoch, metric epoch, and exact-rerank proof. |
| Quantized vector | Must carry quantization profile and exact-source or admitted quantization proof. |

Example:

```sql
vector search app.embedding_index
using :query_vector
limit 10;
```

The query vector parameter must bind to the index vector descriptor or an
admitted conversion. Dimension mismatch is a bind diagnostic.

## Spatial Types

Spatial descriptors define coordinate profile, spatial reference identity,
dimensionality, indexing behavior, and exact-recheck requirements.

| Concern | Rule |
| --- | --- |
| Spatial reference | Must be part of the descriptor or explicit operation. |
| Geometry validity | Invalid shapes are refused or repaired only by explicit admitted functions. |
| Bounding-box indexes | Candidate evidence only. Exact predicate recheck is mandatory. |
| Geodetic behavior | Descriptor-owned. Do not infer planar behavior from spelling alone. |

## Graph Types

Graph descriptors define node identity, edge identity, endpoint descriptors,
property descriptors, traversal shape, and path result ordering.

| Concern | Rule |
| --- | --- |
| Node identity | UUID or descriptor-owned graph identity. |
| Edge identity | Edge UUID plus endpoint identity and direction where relevant. |
| Traversal order | Part of the operation descriptor when stable order is required. |
| Path result | Ordered sequence of node/edge descriptors. |
| Security | Each visible node, edge, property, and path element remains policy-controlled. |

Graph indexes and traversal providers are candidate sources only. They do not
own row visibility, security, or transaction finality.

## Search Types

Search descriptors own tokenization, language/profile, ranking, normalization,
highlighting, and index behavior.

| Concern | Rule |
| --- | --- |
| Token profile | Descriptor-owned and versioned. |
| Ranking | Operation-owned result descriptor. |
| Highlighting | Rendering operation that must not reveal protected material. |
| Search index | Candidate evidence only unless exact proof and policy admit final result. |
| Ordering | Ranking order is part of the result contract. |

## Time-Series Types

Time-series descriptors define time key, value descriptor, series identity,
bucket/window behavior, ordering, gap policy, interpolation policy, and result
shape.

| Concern | Rule |
| --- | --- |
| Time key | Temporal descriptor controls comparison and ordering. |
| Window | Window descriptor owns boundary inclusion, timezone, and precision. |
| Resampling | Requires explicit policy for fill, interpolation, and missing values. |
| Ordering | Stable time ordering must be part of the result contract. |

## Key-Value Types

Key-value descriptors define key shape, value shape, hash/order behavior, TTL or
expiry policy where admitted, and iteration behavior.

| Concern | Rule |
| --- | --- |
| Key descriptor | Controls equality, hashing, ordering, and range iteration. |
| Value descriptor | Controls stored value, domain checks, masks, and protected material. |
| Iteration | Ordering must be descriptor-defined for range or sorted operations. |
| TTL/expiry | Policy-owned and transaction-aware where admitted. |

## Index And Provider Recheck Rule

Every multimodel access path follows this rule:

1. The index or provider returns candidate evidence.
2. The executor rechecks the source row or object under the active transaction.
3. The executor rechecks descriptor compatibility.
4. Security, masking, row-level policy, and protected-material policy are
   applied.
5. The result shape and ordering contract are validated.
6. Only then is the row, document, graph path, vector result, search hit,
   time-series sample, or key-value item returned.

## Diagnostics

| Condition | Required Result |
| --- | --- |
| Document path invalid | Bind or execution diagnostic according to when path is known. |
| Missing path used as scalar | Diagnostic or null/missing result according to operator descriptor. |
| Vector dimension mismatch | Bind diagnostic. |
| Unsupported vector metric | Bind/admission diagnostic. |
| Approximate index lacks exact-recheck proof | Refusal before final result delivery. |
| Invalid geometry | Diagnostic unless explicit repair function is used. |
| Graph traversal cycle or limit exceeded | Diagnostic or partial result according to operation contract. |
| Search profile missing | Bind/admission diagnostic. |
| Time-series ordering ambiguous | Refusal for operations requiring stable order. |
| Key descriptor mismatch | Bind diagnostic. |
| Protected value appears in multimodel rendering | Denied or redacted result. |

## Syntax Productions

```ebnf
multimodel_type         ::= document_type
                          | collection_type
                          | vector_type
                          | spatial_type
                          | graph_type
                          | search_type
                          | timeseries_type
                          | key_value_type ;
```

```ebnf
document_type           ::= "document"
                          | "json_document"
                          | "binary_json_document" ;
```

```ebnf
collection_type         ::= "array" "<" data_type ">"
                          | "multiset" "<" data_type ">"
                          | row_type
                          | record_type ;
```

```ebnf
vector_type             ::= "vector" ;
```

```ebnf
spatial_type            ::= "geometry" | "geography" ;
graph_type              ::= "graph" | "node" | "edge" | "path" ;
```

## Related Pages

- [Type System Overview](#ch-language-reference-data-types-type-system-overview-md)
- [Conversion Matrix](#ch-language-reference-data-types-conversion-matrix-md)
- [Domains, Casts, And Coercion](#ch-language-reference-data-types-domains-casts-and-coercion-md)
- Multimodel Statements (SBsql Language Reference — Syntax, page XXX)
- Index Lifecycle (SBsql Language Reference — Syntax, page XXX)
- Projection (SBsql Language Reference — Syntax, page XXX)
- Where (SBsql Language Reference — Syntax, page XXX)

## Verification Checklist

The multimodel proof suite should demonstrate:

- document missing/null behavior matches descriptor rules;
- document path indexes require executor recheck;
- collection element descriptors and null policies are enforced;
- vector dimensions, element profiles, and metrics are enforced;
- approximate vector indexes cannot return final rows without exact recheck or
  admitted proof;
- spatial indexes use candidate evidence plus exact predicate recheck;
- graph traversals preserve node, edge, path, order, and visibility rules;
- search ranking and ordering match result contracts;
- time-series windows preserve time key, boundary, and ordering semantics;
- key-value range and hash operations use descriptor-owned key behavior;
- protected material is redacted or denied across all multimodel result shapes.




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/data_types/domains_casts_and_coercion.md -->

<a id="ch-language-reference-data-types-domains-casts-and-coercion-md"></a>

# Domains, Casts, And Coercion

This page is part of the SBsql Language Reference Manual. It explains how domains participate in descriptor binding, implicit assignment, explicit casts, operation result types, and validation.

Generation task: `data_types_domains_and_casts`

Related pages: Domain Lifecycle (SBsql Language Reference — Syntax, page XXX), [Type System Overview](#ch-language-reference-data-types-type-system-overview-md), [Conversion Matrix](#ch-language-reference-data-types-conversion-matrix-md), Operator Type Result Matrix (SBsql Language Reference — Syntax, page XXX), Table Lifecycle (SBsql Language Reference — Syntax, page XXX), and [sys.catalog.domain_descriptor](#ch-language-reference-catalog-reference-sys-catalog-domain-descriptor-md).

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
create domain app.positive_int as bigint
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




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/data_types/conversion_matrix.md -->

<a id="ch-language-reference-data-types-conversion-matrix-md"></a>

# Conversion Matrix

This page is part of the SBsql Language Reference Manual. It defines descriptor
conversion behavior for implicit assignment, explicit casts, `try_cast`,
literal binding, domain validation, lossy conversion, protected values, and
diagnostics.

Generation task: `data_types_conversion_matrix`

## Purpose

This page is a quick reference for what the binder does when a value of one
type must be used where another type is expected. The short version: implicit
conversion is intentionally narrow — SBsql will not silently widen, truncate, or
re-encode a value across descriptor-family boundaries. When you need a value in
a different type, write an explicit `cast(... as ...)` so the conversion is
visible in the query and testable in isolation.

The full picture depends on the combination of source descriptor, target
descriptor, and conversion class (implicit assignment, explicit cast, try-cast,
named conversion function, domain validation, or protected release). The tables
below capture that matrix for the common cases.

## Conversion Classes

| Class | Meaning |
| --- | --- |
| Contextual literal binding | A literal adopts a target descriptor because the surrounding expression supplies one. |
| Implicit assignment | Safe assignment conversion used for storage, parameters, returns, variables, and generated values. |
| Explicit cast | User-requested conversion through `cast(value as type)`. |
| Try-cast | User-requested conversion that returns a documented failure value instead of raising the normal diagnostic. |
| Named conversion function | Function with explicit conversion semantics, such as decode, encode, rounding, parsing, or formatting. |
| Domain validation | Conversion to a domain carrier followed by domain null policy and constraints. |
| Protected release | Policy-controlled release of protected material; not an ordinary cast. |

## Core Matrix

| Source | Target | Implicit Assignment | Explicit Cast | Notes |
| --- | --- | --- | --- | --- |
| `null` | nullable target | yes | yes | `null` adopts the target descriptor. Non-nullable targets reject it. |
| `boolean` | `text` | no | yes | Renders canonical boolean text unless a profile renders otherwise. |
| `text` | `boolean` | no | yes | Accepted spellings are descriptor-owned. Invalid text is diagnostic. |
| signed integer | wider signed integer | yes | yes | Refused if value cannot fit. |
| unsigned integer | wider unsigned integer | yes | yes | Refused if value cannot fit. |
| signed integer | unsigned integer | no | yes | Negative or out-of-range values are refused. |
| unsigned integer | signed integer | no | yes | Refused if value cannot fit target signed range. |
| exact integer | decimal | yes when exact | yes | Target precision and scale must admit the exact value. |
| decimal | exact integer | no | yes | Fractional values require explicit rounding function; plain cast refuses. |
| exact numeric | approximate real | policy-dependent | yes | May be lossy. Portable scripts cast explicitly. |
| approximate real | exact numeric | no | yes | NaN, infinity, out-of-range, or non-exact values are refused unless policy admits them. |
| numeric | text | no | yes | Rendering is descriptor-owned. |
| text | numeric | no | yes | Invalid syntax or out-of-range values are diagnostics. |
| text | temporal | no | yes | Requires valid literal syntax for target descriptor. |
| temporal | text | no | yes | Rendering uses timezone/profile policy. |
| temporal | temporal | policy-dependent | yes | Precision, timezone, and calendar loss must be explicit. |
| binary | text | no | yes | Requires encoding or charset rule. |
| text | binary | no | yes | Requires encoding or decode rule. |
| text | UUID | no | yes | Text must be canonical or profile-admitted UUID text. |
| UUID | text | no | yes | Renders canonical UUID text unless policy says otherwise. |
| UUID | `binary(16)` | no | yes | Converts to 16-byte representation. |
| `binary(16)` | UUID | no | yes | Validates UUID descriptor policy. |
| document scalar | scalar target | no | yes | Extracted value must be scalar and compatible with target descriptor. |
| scalar | document | no | yes | Creates a document scalar value. |
| array element | another element descriptor | no | yes | Every element conversion must be admitted. |
| vector element | another vector element descriptor | no | yes | Dimension must match; quantization/lossiness must be explicit. |
| protected reference | raw text or binary | no | no ordinary cast | Requires protected release authority. |
| raw text or binary | protected reference | no | no ordinary cast | Requires admitted protect/store route. |
| domain | base carrier | policy-dependent | yes | Domain erasure must be admitted. |
| base carrier | domain | assignment only after validation | yes | Carrier conversion, null policy, and constraints all apply. |

## Implicit Assignment

Implicit assignment is allowed only when conversion is exact, deterministic, and
does not hide policy-sensitive behavior.

Safe default examples:

| Source | Target | Reason |
| --- | --- | --- |
| `int16` | `int32` | Exact widening. |
| `int32` | `int64` | Exact widening. |
| `int64` | `int128` | Exact widening. |
| `uint16` | `uint32` | Exact widening. |
| `uint64` | `uint128` | Exact widening. |
| exact integer | `decimal(p,0)` | Exact when precision admits the value. |
| `null` | nullable target | Null adopts target descriptor. |
| domain | same domain target | Same domain descriptor. |

Implicit assignment should not silently:

- truncate text or binary values;
- round decimals;
- lose timezone;
- lose fractional temporal precision;
- convert text to numeric;
- reinterpret binary as text;
- erase domain policy;
- release protected material;
- change vector dimension;
- coerce document missing values to null unless the operator says so.

## Explicit Cast

Explicit casts state user intent and make conversion auditable.

```sql
select cast(:amount_text as decimal(18,2)) as amount;

select cast(:event_text as timestamptz) as event_at;

select cast(:id_text as uuid) as object_id;
```

An explicit cast still fails when the target descriptor refuses the value.
Explicit does not mean unsafe.

## Try-Cast

`try_cast` follows the same conversion rules as `cast`, but returns the
documented failure result instead of raising the ordinary conversion diagnostic.
The failure result is descriptor-owned and must be distinguishable from a valid
value when the contract requires it.

Example:

```sql
select try_cast(payload->>'amount' as decimal(18,2)) as parsed_amount
from app.ingest_document;
```

`try_cast` must not hide protected-material denial, sandbox denial, recovery
fences, or operation admission failures.

## Lossy Conversions

| Conversion | Default Behavior |
| --- | --- |
| Decimal with fractional part to integer | Refuse. Use explicit rounding/truncation function when intended. |
| Decimal to lower precision/scale | Refuse unless explicit cast policy admits rounding. |
| Approximate real to exact numeric | Refuse for non-exact values, NaN, infinity, or out-of-range values. |
| Timestamp to lower precision | Refuse unless explicit cast policy admits precision loss. |
| `timestamptz` to `timestamp` | Refuse unless timezone-loss policy is explicit. |
| Text to shorter text | Refuse unless explicit truncation function is used. |
| Text charset conversion with unrepresentable characters | Refuse unless explicit replacement policy is used. |
| Binary to text with invalid encoded bytes | Refuse. |
| Vector `real32` element to `int8` element | Refuse unless quantization policy is explicit. |
| Document number to lower-precision numeric | Refuse unless explicit cast policy admits the loss. |

## Domain Conversion

Conversion to a domain follows the domain assignment pipeline:

1. convert value to the domain carrier descriptor;
2. apply domain null policy;
3. apply parent domain constraints;
4. apply target domain constraints;
5. apply element policy for compound values;
6. preserve the target domain descriptor where the operation says so.

Example:

```sql
select cast(:candidate as app.email_text) as email_value;
```

Domain validation is not optional merely because the carrier conversion
succeeded.

## Text, Binary, And UUID Conversions

| Conversion | Required Detail |
| --- | --- |
| Text to binary | Encoding, decode rule, or binary target policy. |
| Binary to text | Charset or encoding rule. |
| Text to UUID | Canonical or admitted UUID text syntax. |
| UUID to text | Rendering policy, default canonical UUID text. |
| UUID to binary | Explicit 16-byte representation. |
| Binary to UUID | Exactly 16 bytes and UUID descriptor validation. |

Text is never treated as a byte string without conversion. Binary is never
treated as text without conversion.

## Temporal Conversions

| Conversion | Rule |
| --- | --- |
| Text to date/time/timestamp/interval | Explicit cast or target context required. Invalid fields are diagnostic. |
| Timestamp to `timestamptz` | Requires timezone rule. |
| `timestamptz` to timestamp | Requires explicit timezone-loss rule. |
| Date to timestamp | Admitted when default time-of-day policy is explicit. |
| Timestamp to date | Refuses time loss unless explicit cast policy admits it. |
| Interval to numeric | Requires explicit operation defining units. |
| Numeric to interval | Requires explicit operation defining units. |

## Document And Collection Conversions

| Conversion | Rule |
| --- | --- |
| Document scalar to scalar | Requires path extraction and scalar descriptor compatibility. |
| Scalar to document scalar | Explicit cast creates document scalar value. |
| Document object to text | Rendering operation, not ordinary implicit conversion. |
| Array to array | Element-by-element conversion required. |
| Row to record | Shape and field descriptor compatibility required. |
| Record to row | Target field names/order and descriptor compatibility required. |
| Missing document path | Missing is not null unless the operator descriptor says so. |

## Vector, Spatial, Graph, Search, Time-Series, And Key-Value Conversions

| Conversion | Rule |
| --- | --- |
| Vector element type change | Explicit cast; dimension must match; quantization/lossiness must be explicit. |
| Vector dimension change | Refused unless a named operation defines padding, projection, or embedding conversion. |
| Geometry/geography conversion | Requires spatial reference and geodetic policy. |
| Graph node/edge/path conversion | Requires graph descriptor compatibility and identity preservation. |
| Search document conversion | Requires tokenization profile and rendering policy. |
| Time-series sample conversion | Requires time key and value descriptor compatibility. |
| Key-value conversion | Requires key and value descriptor compatibility. |

## Protected Material

Protected material cannot be released through ordinary casts.

| Request | Default Result |
| --- | --- |
| Protected reference to text | Denied unless a release surface admits it. |
| Protected reference to binary | Denied unless a release surface admits it. |
| Protected value in diagnostic | Redacted. |
| Protected value in support bundle | Redacted unless release policy admits specific evidence. |
| Protected value in backup/replication/migration stream | Requires explicit release/export policy. |

## Syntax Productions

```ebnf
cast_expression         ::= "cast" "(" expression "as" data_type ")" ;
```

```ebnf
try_cast_expression     ::= "try_cast" "(" expression "as" data_type ")" ;
```

```ebnf
conversion_function     ::= function_call ;
```

```ebnf
assignment_conversion   ::= expression target_descriptor ;
```

## Diagnostics

| Condition | Required Result |
| --- | --- |
| Unsupported conversion | Bind diagnostic or unsupported message vector. |
| Ambiguous conversion | Bind diagnostic requiring explicit cast. |
| Out-of-range value | Conversion diagnostic. |
| Lossy conversion without policy | Conversion diagnostic. |
| Invalid literal syntax | Parse or conversion diagnostic according to phase. |
| Domain validation failure | Domain diagnostic. |
| Protected release denied | Denied message vector. |
| Descriptor stale | Rebind or stale descriptor diagnostic. |
| Recovery state uncertain | Fail-closed diagnostic before conversion side effects. |

## Related Pages

- [Type System Overview](#ch-language-reference-data-types-type-system-overview-md)
- [Numeric Types](#ch-language-reference-data-types-numeric-types-md)
- [Text, Collation, And Charset](#ch-language-reference-data-types-text-collation-and-charset-md)
- [Temporal Types](#ch-language-reference-data-types-temporal-types-md)
- [Binary, UUID, And Protected Values](#ch-language-reference-data-types-binary-uuid-and-protected-values-md)
- [Document, Graph, Vector, And Multimodel Types](#ch-language-reference-data-types-document-graph-vector-and-multimodel-types-md)
- [Domains, Casts, And Coercion](#ch-language-reference-data-types-domains-casts-and-coercion-md)
- Operator Type Result Matrix (SBsql Language Reference — Syntax, page XXX)

## Verification Checklist

The conversion proof suite should demonstrate:

- implicit conversions are limited to exact, deterministic cases;
- explicit casts do not bypass range, precision, timezone, charset, or policy
  checks;
- `try_cast` uses the same validation path as `cast`;
- decimal, temporal, text, binary, and vector lossy conversions are refused by
  default;
- domain conversion applies carrier conversion, null policy, parent checks, and
  target checks;
- binary/text conversions require explicit encoding or charset rules;
- protected material cannot be released by ordinary casts;
- document missing/null behavior is preserved through conversions;
- vector dimension mismatches are refused;
- stale descriptor and security epochs invalidate cached conversion decisions.




# Catalog Reference




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/catalog_reference/index.md -->

<a id="ch-language-reference-catalog-reference-index-md"></a>

# Catalog Reference Index

This section documents public SBsql catalog surfaces. A catalog page explains
how to read an authorized metadata surface; it is not a direct mutation API.
Catalog rows are engine-owned, UUID-identified, transactionally visible, and
redacted by security policy.

## Catalog Authority

Catalog data records durable engine metadata: type descriptors, domain
descriptors, operation descriptors, protected-material metadata, policy
bindings, and protected-material audit evidence. User-facing names are resolver
input. Durable catalog identity is UUID based.

The public rules are:

- base catalog rows are mutated only by engine-managed DDL, security,
  protected-material, or catalog lifecycle operations;
- `SHOW`, `DESCRIBE`, information-style views, and support diagnostics are
  authorized projections over catalog state;
- projections can redact or hide rows according to the caller's security
  context;
- catalog rows become visible only through MGA transaction finality;
- cached parser, plan, driver, UDR, support-bundle, and metadata state must
  revalidate when catalog, security, resolver, or resource epochs change.

## Reading A Catalog Page

Each table page follows the same public interpretation model.

| Section | Meaning |
| --- | --- |
| Role | Why the surface exists and which engine behavior depends on it. |
| Keys and columns | Public metadata fields and their descriptor families. |
| Column semantics | How important fields affect binding, validation, policy, or execution. |
| Visibility and mutation | Who can read the projection and which engine operation can change it. |
| Dependencies and invalidation | Which cached or compiled state must rebind when rows change. |
| Failure modes | Required diagnostics for missing, stale, hidden, invalid, or policy-blocked state. |
| Verification checklist | Proof expectations for the surface. |

## Catalog Surfaces

| Catalog Surface | File |
| --- | --- |
| `sys.catalog.type_descriptor` | [sys_catalog_type_descriptor.md](#ch-language-reference-catalog-reference-sys-catalog-type-descriptor-md) |
| `sys.catalog.domain_descriptor` | [sys_catalog_domain_descriptor.md](#ch-language-reference-catalog-reference-sys-catalog-domain-descriptor-md) |
| `sys.catalog.domain_element` | [sys_catalog_domain_element.md](#ch-language-reference-catalog-reference-sys-catalog-domain-element-md) |
| `sys.catalog.reference_type_mapping` | [sys_catalog_donor_type_mapping.md](#ch-language-reference-catalog-reference-sys-catalog-donor-type-mapping-md) |
| `sys.catalog.type_capability` | [sys_catalog_type_capability.md](#ch-language-reference-catalog-reference-sys-catalog-type-capability-md) |
| `sys.catalog.operation_descriptor` | [sys_catalog_operation_descriptor.md](#ch-language-reference-catalog-reference-sys-catalog-operation-descriptor-md) |
| `sys.security.protected_material_catalog` | [sys_security_protected_material_catalog.md](#ch-language-reference-catalog-reference-sys-security-protected-material-catalog-md) |
| `sys.security.protected_material_version` | [sys_security_protected_material_version.md](#ch-language-reference-catalog-reference-sys-security-protected-material-version-md) |
| `sys.security.protected_material_policy_binding` | [sys_security_protected_material_policy_binding.md](#ch-language-reference-catalog-reference-sys-security-protected-material-policy-binding-md) |
| `sys.security.protected_material_audit` | [sys_security_protected_material_audit.md](#ch-language-reference-catalog-reference-sys-security-protected-material-audit-md) |

## Common Query Pattern

Catalog examples use explicit column lists so public scripts do not depend on
hidden, redacted, version-specific, or future columns.

```sql
select descriptor_uuid,
       canonical_type,
       type_family
from sys.catalog.type_descriptor
order by canonical_type;
```

## Related Reference Pages

- [UUID Catalog Identity](#ch-language-reference-core-paradigms-uuid-catalog-identity-md)
- [Security And Sandboxing](#ch-language-reference-core-paradigms-security-and-sandboxing-md)
- [Parser To SBLR Pipeline](#ch-language-reference-core-paradigms-parser-to-sblr-pipeline-md)
- [Type System Overview](#ch-language-reference-data-types-type-system-overview-md)
- [Domains, Casts, And Coercion](#ch-language-reference-data-types-domains-casts-and-coercion-md)
- Refusal Vectors (SBsql Language Reference — Syntax, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/catalog_reference/sys_catalog_type_descriptor.md -->

<a id="ch-language-reference-catalog-reference-sys-catalog-type-descriptor-md"></a>

# sys.catalog.type_descriptor Catalog Reference

This page documents the authorized catalog surface that describes canonical
type descriptors. Type descriptors are the durable metadata records that let
the binder, SBLR envelope, executor, optimizer, index layer, stream layer, and
result renderer agree on what a value is.

Generation task: `catalog_sys_catalog_type_descriptor`

Related pages: [Type System Overview](#ch-language-reference-data-types-type-system-overview-md),
[Conversion Matrix](#ch-language-reference-data-types-conversion-matrix-md),
[sys.catalog.type_capability](#ch-language-reference-catalog-reference-sys-catalog-type-capability-md), and
[sys.catalog.domain_descriptor](#ch-language-reference-catalog-reference-sys-catalog-domain-descriptor-md).

## Role

`sys.catalog.type_descriptor` records canonical carrier identity. A type name
such as `uint128`, `varchar(120)`, `timestamp(6) with time zone`, or
`vector<float32,1536>` resolves to a descriptor row or to a domain row that
wraps a descriptor row.

The row is used for:

- literal, parameter, column, variable, routine, stream, and result binding;
- cast and assignment validation;
- row, page, overflow, and stream admission;
- comparison, hashing, ordering, grouping, and index eligibility;
- text charset/collation, temporal timezone, vector dimension, document shape,
  and protected-material behavior;
- cache invalidation when descriptor-affecting metadata changes.

## Keys And Columns

Primary key: `descriptor_uuid`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `descriptor_uuid` | UUID | Durable descriptor identity. This is what SBLR and dependent catalog objects bind to. |
| `canonical_type` | enum domain | Canonical SBsql type name or carrier class. |
| `type_family` | enum domain | Scalar, numeric, text, binary, temporal, document, spatial, vector, graph, collection, protected, opaque, or extension family. |
| `source_type_uuid` | nullable UUID | Descriptor this descriptor derives from, where derivation is represented. |
| `domain_uuid` | nullable UUID | Domain identity when this descriptor is a domain-bound slot projection. |
| `modifier_profile_uuid` | nullable UUID | Precision, scale, length, dimension, spatial reference, timezone, or shape modifier profile. |
| `charset_uuid` | nullable UUID | Character set descriptor for text values. |
| `collation_uuid` | nullable UUID | Collation descriptor for text comparison and index keys. |
| `timezone_policy_uuid` | nullable UUID | Temporal timezone and rendering policy. |
| `storage_codec_uuid` | nullable UUID | Storage/overflow/large-value codec descriptor where applicable. |
| `comparison_contract_uuid` | nullable UUID | Equality, ordering, hashing, canonicalization, and null-comparison contract. |
| `capability_uuid` | nullable UUID | Link to the capability row for this descriptor. |

## Column Semantics

### Descriptor UUID

`descriptor_uuid` is stable identity. Names and aliases can change, but SBLR
payloads, prepared statements, indexes, routines, domains, and result
descriptors use the UUID.

### Canonical Type And Type Family

`canonical_type` is the public carrier name or class. `type_family` controls
which descriptor-specific rules apply. For example:

- numeric descriptors own range, precision, scale, and arithmetic behavior;
- text descriptors own character set and collation;
- temporal descriptors own precision and timezone policy;
- vector descriptors own dimension, element profile, metric, and recheck rules;
- protected descriptors own release and redaction policy.

### Modifier Profile

`modifier_profile_uuid` prevents a type spelling from being the only record of
important modifiers. A descriptor for `decimal(18,2)` must carry precision and
scale. A descriptor for `vector<float32,1536>` must carry element profile and
dimension. A descriptor for `timestamp(6) with time zone` must carry precision
and timezone behavior.

### Charset, Collation, And Timezone

These fields are nullable because not every descriptor uses them. When present,
they are dependencies that can affect comparison, sorting, indexing, rendering,
and cache invalidation.

## Visibility And Mutation

Base rows are engine-owned. Users inspect type descriptors through authorized
catalog projections, `SHOW TYPE`, `DESCRIBE TYPE`, information-style views, or
support diagnostics.

Direct user mutation of `sys.catalog.type_descriptor` is not the DDL API.
Descriptor changes must occur through admitted type, domain, catalog, bootstrap,
or extension lifecycle operations and become visible only through MGA
transaction finality.

## Dependencies And Invalidation

Descriptor changes can invalidate:

- prepared statements;
- compiled routines and triggers;
- domains and compound domain elements;
- table columns and generated columns;
- indexes and statistics;
- casts, operators, aggregates, and window functions;
- backup, restore, replication, migration, bridge, and stream descriptors;
- support-bundle and metadata projections.

When any descriptor-affecting epoch changes, dependent state must rebind,
revalidate, rebuild, or refuse execution.

## Example Inspection

```sql
select descriptor_uuid,
       canonical_type,
       type_family,
       modifier_profile_uuid,
       capability_uuid
from sys.catalog.type_descriptor
order by type_family, canonical_type;
```

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Descriptor UUID missing | Bind diagnostic for unresolved descriptor. |
| Descriptor hidden by policy | Redacted not-visible or denied diagnostic. |
| Descriptor family mismatch | Bind/admission diagnostic. |
| Required modifier missing | Descriptor-invalid diagnostic. |
| Charset/collation/timezone dependency missing | Bind or admission diagnostic. |
| Capability row missing where required | Capability diagnostic; do not assume defaults. |
| Stale descriptor epoch | Rebind, invalidate cache, or refuse stale execution. |

## Verification Checklist

Proof should demonstrate:

- every public type spelling resolves to a descriptor UUID;
- descriptor UUIDs are stable across rename or alias changes;
- modifiers are represented by descriptor metadata, not text-only spelling;
- charset, collation, timezone, storage, and comparison dependencies are
  enforced;
- SBLR envelopes carry descriptor references;
- hidden descriptors do not leak through unauthorized projections;
- dependent plans and compiled objects invalidate when descriptor metadata
  changes.




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/catalog_reference/sys_catalog_domain_descriptor.md -->

<a id="ch-language-reference-catalog-reference-sys-catalog-domain-descriptor-md"></a>

# sys.catalog.domain_descriptor Catalog Reference

This page is part of the SBsql Language Reference Manual. It documents the authorized catalog surface that describes domain identity, domain stacks, validation policy, cast policy, operation policy, and masking policy.

Generation task: `catalog_sys_catalog_domain_descriptor`

Related pages: Domain Lifecycle (SBsql Language Reference — Syntax, page XXX), [Domains, Casts, And Coercion](#ch-language-reference-data-types-domains-casts-and-coercion-md), [sys.catalog.domain_element](#ch-language-reference-catalog-reference-sys-catalog-domain-element-md), [sys.catalog.type_descriptor](#ch-language-reference-catalog-reference-sys-catalog-type-descriptor-md), and [sys.catalog.type_capability](#ch-language-reference-catalog-reference-sys-catalog-type-capability-md).

## Role

`sys.catalog.domain_descriptor` records durable metadata for every domain. A domain descriptor is the catalog authority for:

- the domain UUID;
- the base carrier descriptor;
- the optional parent domain;
- the resolved domain stack;
- null policy;
- defaults;
- constraints;
- element policy;
- cast policy;
- operation policy;
- masking/protected-value policy;
- SBsql alias rendering metadata.

Catalog rows are not parser authority. They are visible through authorized catalog projections, `SHOW DOMAIN`, `DESCRIBE DOMAIN`, information-style views, or support tooling. Base catalog mutation must go through engine-managed domain lifecycle operations.

## Keys And Columns

Primary key: `domain_uuid`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `domain_uuid` | UUID | Durable domain identity. This value is what dependent columns, routines, indexes, views, and SBLR descriptors bind to. |
| `domain_kind` | enum domain | Domain family: `scalar`, `compound`, `array`, `row`, `map`, `range`, `enum`, `variant`, `opaque`, `alias_profile`, or `protected_history`. |
| `base_descriptor_uuid` | nullable UUID | Canonical carrier descriptor used for physical representation and primitive operations. Required unless the domain kind is represented entirely through a base domain or policy-owned opaque binding. |
| `base_domain_uuid` | nullable UUID | Parent domain UUID for domain-over-domain stacks. |
| `domain_stack_hash` | binary/hash | Stable hash of the resolved base descriptor plus parent-domain chain and this domain's validation identity. |
| `nullable_policy_uuid` | UUID | Policy that admits or refuses `null` for values of this domain. |
| `default_expression_uuid` | nullable UUID | Bound default expression used when no more specific assignment-site default exists. |
| `constraint_set_uuid` | nullable UUID | Set of named domain checks evaluated with the `VALUE` pseudo-value. |
| `element_policy_uuid` | nullable UUID | Compound-domain element policy set. Used with `sys.catalog.domain_element`. |
| `cast_policy_uuid` | nullable UUID | Policy governing implicit assignment, explicit casts, lossiness, domain preservation, and domain erasure. |
| `operation_policy_uuid` | nullable UUID | Policy governing comparison, hashing, ordering, arithmetic, text, temporal, document, vector, spatial, opaque, aggregate, and index behavior. |
| `masking_policy_uuid` | nullable UUID | Redaction, protected-value, support-bundle, or projection masking policy. |
| `source_family` | nullable enum domain | SBsql alias/rendering family used when a domain has a policy-owned alternate surface name. |
| `source_type_name` | nullable text domain | SBsql-facing type or alias spelling rendered by authorized metadata views. |

## Column Semantics

### Identity

`domain_uuid` is stable across rename. The resolver name can change, but stored descriptors, SBLR payloads, indexes, routines, views, and table columns remain bound to the UUID.

### Base Descriptor And Base Domain

A domain may wrap a canonical descriptor directly:

```text
app.nonnegative_money
|
+-- base_descriptor_uuid -> decimal(18,2)
```

It may also wrap another domain:

```text
app.customer_label
|
+-- base_domain_uuid -> app.nonblank_text
    |
    +-- base_descriptor_uuid -> varchar(200)
```

The binder resolves this chain before execution and records the result in the domain stack.

### Domain Stack Hash

`domain_stack_hash` detects whether the resolved chain used by a prepared statement, stored routine, generated column, index, or view is still valid. If any domain in the chain changes in a way that affects validation or operations, dependent compiled metadata must rebind or refuse.

### Policy UUIDs

Policy UUID columns point to policy records outside this table. The domain descriptor stores references so the binder and engine can apply a single authority model for validation, casting, comparison, indexing, masking, and element mutation.

## Dependency Behavior

The following objects normally depend on `domain_uuid`:

- table columns;
- generated columns;
- defaults and constraints;
- indexes and index expressions;
- views and materialized views;
- procedure/function parameters and returns;
- triggers;
- UDR bindings;
- cast descriptors;
- operation descriptors;
- support-bundle and metadata projections.

Changing a domain descriptor can invalidate any of those dependencies. The engine must either refresh them, revalidate them, or refuse the change.

## Operational Boundaries

- Base catalog rows require UUID identity and transaction lifecycle metadata.
- Direct user mutation of `sys.catalog.domain_descriptor` is not the domain DDL API.
- Visibility is policy controlled and may use redaction.
- Derived views must preserve base-row authority and must not become engine identity.
- `SHOW DOMAIN` and `DESCRIBE DOMAIN` are authorized projections over this table and related policy tables.
- DDL changes become visible only through MGA transaction finality.

## Example Inspection

```sql
select domain_uuid,
       domain_kind,
       base_descriptor_uuid,
       base_domain_uuid,
       domain_stack_hash
from sys.catalog.domain_descriptor
order by domain_kind, domain_uuid;
```

Use Domain Lifecycle (SBsql Language Reference — Syntax, page XXX) for supported mutation syntax. This catalog page is for metadata interpretation.




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/catalog_reference/sys_catalog_domain_element.md -->

<a id="ch-language-reference-catalog-reference-sys-catalog-domain-element-md"></a>

# sys.catalog.domain_element Catalog Reference

This page is part of the SBsql Language Reference Manual. It documents the authorized catalog surface for addressable elements inside compound domains.

Generation task: `catalog_sys_catalog_domain_element`

Related pages: Domain Lifecycle (SBsql Language Reference — Syntax, page XXX), [Domains, Casts, And Coercion](#ch-language-reference-data-types-domains-casts-and-coercion-md), and [sys.catalog.domain_descriptor](#ch-language-reference-catalog-reference-sys-catalog-domain-descriptor-md).

## Role

`sys.catalog.domain_element` records the field, path, ordinal, range-bound, map-key, variant-tag, opaque-accessor, or document-pointer metadata for compound domains. It is how ScratchBird represents structured domain members without treating path text as durable authority.

An element row is used by:

- structured value validation;
- path access;
- partial update admission;
- element masking/redaction;
- generated columns derived from domain elements;
- indexes over domain elements;
- routine and UDR argument binding;
- metadata rendering;
- support diagnostics.

Catalog rows are not parser authority. They are visible through authorized projections, `DESCRIBE DOMAIN`, information-style views, or support tooling.

## Keys And Columns

Primary key: `element_uuid`

Unique key: `domain_uuid`, `element_ordinal`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `element_uuid` | UUID | Durable element identity. |
| `domain_uuid` | UUID | Owning compound domain. References `sys.catalog.domain_descriptor.domain_uuid`. |
| `element_ordinal` | unsigned integer | Stable ordinal for positional rendering, row-like domains, arrays, lists, and deterministic metadata output. |
| `element_name_uuid` | nullable UUID | Localized/display name reference for named fields or path segments. |
| `path_segment_kind` | enum domain | Segment family: `field_uuid`, `field_ordinal`, `array_index`, `list_index`, `map_key`, `variant_tag`, `range_lower`, `range_upper`, `set_member`, `opaque_accessor`, or `document_pointer`. |
| `target_descriptor_uuid` | UUID | Carrier descriptor for the element value. |
| `target_domain_uuid` | nullable UUID | Domain descriptor for the element value when the element itself is domain-bound. |
| `nullable_policy_uuid` | UUID | Element-level null policy. |
| `visibility_policy_uuid` | UUID | Element read/disclosure policy. |
| `mutation_policy_uuid` | UUID | Element update and partial-mutation policy. |

## Element Identity

Element names are rendering metadata. The durable identity is `element_uuid`, and the owning relationship is `domain_uuid`.

```text
domain_uuid app.address_value
|
+-- element_uuid street
+-- element_uuid city
+-- element_uuid postal_code
```

Renaming an element or changing its display label must not silently change its identity. If a change alters validation or mutation behavior, dependent expressions, indexes, routines, views, and generated columns must rebind or refuse.

## Path Segment Kinds

| Segment Kind | Meaning |
| --- | --- |
| `field_uuid` | Named field resolved by durable field identity. |
| `field_ordinal` | Positional field in a row-like value. |
| `array_index` | Array element selected by index. |
| `list_index` | List element selected by index. |
| `map_key` | Map value selected by key descriptor. |
| `variant_tag` | Active payload selected by variant tag. |
| `range_lower` | Lower bound of a range value. |
| `range_upper` | Upper bound of a range value. |
| `set_member` | Member element of a set-like value. |
| `opaque_accessor` | Policy-owned accessor for opaque values. |
| `document_pointer` | Document path pointer governed by descriptor and policy. |

## Target Descriptor And Target Domain

Every element has a target carrier descriptor. It may also have a target domain. When both are present, assignment to the element must satisfy both the carrier descriptor and the target domain validation pipeline.

```text
element app.address_value.postal_code
|
+-- target_descriptor_uuid -> varchar(20)
+-- target_domain_uuid     -> app.postal_code_text
```

## Visibility And Mutation

Element policy is separate from whole-value policy.

| Policy | Contract |
| --- | --- |
| Null policy | Determines whether the element may be `null`. |
| Visibility policy | Determines whether the effective user or agent may read or render the element. |
| Mutation policy | Determines whether the element may be inserted, updated, patched, cleared, or modified through partial update syntax. |
| Masking policy | Inherited or referenced through the owning domain. May redact an element even when the compound value is visible. |

## Operational Boundaries

- Direct user mutation of `sys.catalog.domain_element` is not the compound-domain DDL API.
- Element metadata must be transactionally visible and rollback-safe.
- Element path text is resolver input only. It is not durable identity.
- Derived views must preserve base-row authority and must not become engine identity.
- Element changes can invalidate generated columns, indexes, routines, UDR bindings, views, materialized views, support projections, and cached plans.

## Example Inspection

```sql
select element_uuid,
       domain_uuid,
       element_ordinal,
       path_segment_kind,
       target_descriptor_uuid,
       target_domain_uuid
from sys.catalog.domain_element
where domain_uuid = :domain_uuid
order by element_ordinal;
```

Use Domain Lifecycle (SBsql Language Reference — Syntax, page XXX) for supported mutation syntax. This catalog page is for metadata interpretation.




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/catalog_reference/sys_catalog_donor_type_mapping.md -->

<a id="ch-language-reference-catalog-reference-sys-catalog-donor-type-mapping-md"></a>

# sys.catalog.reference_type_mapping Catalog Reference

This page documents the authorized catalog surface that maps SBsql-visible type
spellings, aliases, profiles, and rendering names to canonical descriptors or
domains.

Generation task: `catalog_sys_catalog_reference_type_mapping`

Related pages: [Type System Overview](#ch-language-reference-data-types-type-system-overview-md),
[sys.catalog.type_descriptor](#ch-language-reference-catalog-reference-sys-catalog-type-descriptor-md),
[sys.catalog.domain_descriptor](#ch-language-reference-catalog-reference-sys-catalog-domain-descriptor-md), and
Script Tokens And Identifiers (SBsql Language Reference — Syntax, page XXX).

## Role

`sys.catalog.reference_type_mapping` lets SBsql remain context sensitive while the
engine remains descriptor-driven. It records which public spelling is accepted
in which profile, what descriptor or domain it resolves to, and how metadata
should render that type back to an authorized user.

Aliases are not execution authority. After binding, the SBLR envelope carries
descriptor and domain UUIDs.

## Keys And Columns

Primary key: `mapping_uuid`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `mapping_uuid` | UUID | Durable mapping identity. |
| `profile_family` | enum domain | SBsql profile or compatibility profile that owns the spelling. |
| `profile_version` | text/domain | Version or policy profile for the spelling. |
| `visible_type_name` | text domain | Public type spelling accepted or rendered by the profile. |
| `visible_type_code` | nullable text/domain | Optional profile-owned symbolic code for metadata rendering. |
| `representation_class` | enum domain | `native`, `domain`, `compound_domain`, `opaque_domain`, `udr_bridge`, `render_only`, or `unsupported_by_policy`. |
| `descriptor_uuid` | nullable UUID | Canonical type descriptor used when the alias maps to a carrier. |
| `domain_uuid` | nullable UUID | Domain descriptor used when the alias maps to a domain. |
| `udr_package_uuid` | nullable UUID | Trusted package responsible for an opaque or bridge-backed representation. |
| `literal_policy_uuid` | nullable UUID | Literal typing policy for this spelling. |
| `bind_policy_uuid` | nullable UUID | Parameter and assignment binding policy. |
| `metadata_policy_uuid` | nullable UUID | Rules for rendering the alias in metadata output. |
| `compatibility_mode` | enum domain | `strict_scratchbird`, `alias_profile`, `bridge_only`, `degraded`, `render_only`, or `unsupported_by_policy`. |

## Alias Classes

| Representation Class | Meaning |
| --- | --- |
| `native` | The visible name maps directly to a canonical ScratchBird descriptor. |
| `domain` | The visible name maps to a domain UUID. |
| `compound_domain` | The visible name maps to a structured domain with elements. |
| `opaque_domain` | The visible name maps to a descriptor whose behavior is exposed only through admitted operations. |
| `udr_bridge` | The visible name requires a trusted package boundary for rendering or binding. |
| `render_only` | The name can be shown in metadata but cannot be used as an executable type declaration. |
| `unsupported_by_policy` | The spelling is recognized only to return a stable unsupported diagnostic. |

## Binding Rules

When the parser sees a type spelling:

1. resolve the spelling under the active SBsql profile;
2. check `compatibility_mode`;
3. bind either `descriptor_uuid` or `domain_uuid`;
4. apply literal and bind policy;
5. attach descriptor/domain identity to the SBLR envelope;
6. render diagnostics using metadata policy.

If both descriptor and domain are null for an executable mapping, binding must
fail. If both are present, the mapping must define which identity is carrier
authority and which is domain policy.

## Metadata Rendering

Metadata rendering can choose a visible type name that differs from the
canonical descriptor name. Rendering is presentation only. It must not make the
visible name into engine authority.

Example:

```sql
select visible_type_name,
       representation_class,
       compatibility_mode
from sys.catalog.reference_type_mapping
where profile_family = 'sbsql'
order by visible_type_name;
```

## Visibility And Mutation

Base rows are engine-owned and are created or updated through type, domain,
profile, package, or catalog lifecycle operations. Users inspect mappings
through authorized catalog projections, type inspection, metadata views, or
support diagnostics.

Hidden or unsupported aliases should produce the same public result as an
unknown type when metadata-hiding policy requires it.

## Dependencies And Invalidation

Alias mapping changes can invalidate:

- prepared statements that used a type spelling;
- routine and trigger compilation;
- metadata caches;
- driver and parser metadata;
- support-bundle projections;
- UDR package bindings;
- cast and conversion decisions.

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Visible name maps to no descriptor/domain | Bind diagnostic. |
| Mapping is `render_only` but used in DDL | Unsupported diagnostic. |
| Mapping is `unsupported_by_policy` | Stable unsupported message vector. |
| Trusted package missing for `udr_bridge` | Unavailable capability diagnostic. |
| Ambiguous visible names in one profile | Ambiguity diagnostic; no arbitrary winner. |
| Metadata policy hides mapping | Redacted or not-visible result. |
| Mapping epoch stale | Rebind or refuse cached statement. |

## Verification Checklist

Proof should demonstrate:

- accepted type spellings map to descriptor or domain UUIDs;
- unsupported spellings return explicit diagnostics;
- render-only aliases cannot create executable descriptors;
- profile-specific mappings do not leak outside their profile;
- metadata rendering does not become type authority;
- alias changes invalidate cached parser, plan, routine, and metadata state;
- hidden mappings do not leak through unauthorized projections.




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/catalog_reference/sys_catalog_type_capability.md -->

<a id="ch-language-reference-catalog-reference-sys-catalog-type-capability-md"></a>

# sys.catalog.type_capability Catalog Reference

This page documents the authorized catalog surface that records what a type
descriptor can safely do: compare, order, hash, group, index, store, render,
move through streams, participate in backup/replication, and cross trusted
runtime boundaries.

Generation task: `catalog_sys_catalog_type_capability`

Related pages: [sys.catalog.type_descriptor](#ch-language-reference-catalog-reference-sys-catalog-type-descriptor-md),
[Type System Overview](#ch-language-reference-data-types-type-system-overview-md),
Index Lifecycle (SBsql Language Reference — Syntax, page XXX), and
[Conversion Matrix](#ch-language-reference-data-types-conversion-matrix-md).

## Role

`sys.catalog.type_capability` prevents the engine from guessing what a type can
do. A descriptor can exist without being orderable, hashable, indexable,
wire-renderable, stream-safe, or safe for acceleration. Capability rows make
those boundaries explicit.

The row is used by:

- binder overload selection;
- comparison, grouping, sorting, and distinct planning;
- index DDL and DML maintenance;
- optimizer plan selection;
- stream, backup, restore, replication, and migration admission;
- trusted UDR and native acceleration admission;
- protected-material handling and redaction.

## Keys And Columns

Primary key: `capability_uuid`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `capability_uuid` | UUID | Durable capability row identity. |
| `descriptor_uuid` | UUID | Type or domain descriptor governed by this capability row. |
| `comparable` | boolean | Equality operation support. |
| `orderable` | boolean | Total or admitted partial ordering support. |
| `hashable` | boolean | Hash-key support for joins, grouping, lookup, or hash indexes. |
| `groupable` | boolean | `GROUP BY`, `DISTINCT`, and grouping-set eligibility. |
| `indexable` | boolean | At least one index family can use this descriptor. |
| `storable` | boolean | Ordinary row, overflow, or large-value storage is admitted. |
| `wire_renderable` | boolean | Value can be rendered through an admitted client/parser/driver result contract. |
| `backup_safe` | boolean | Logical backup can include this descriptor under policy. |
| `replication_safe` | boolean | Logical replication/change streams can carry this descriptor under policy. |
| `cluster_transport_safe` | boolean | Descriptor has a declared transport profile for gated cluster-provider routes. |
| `udr_safe` | boolean | Descriptor can cross a trusted UDR boundary under policy. |
| `llvm_eligible` | boolean | Descriptor can participate in admitted native acceleration. |
| `protected_value_capable` | boolean | Descriptor can carry protected references or release-controlled material. |
| `element_addressable` | boolean | Descriptor supports element/path addressing through domain or compound-value metadata. |

## Capability Semantics

Capability flags are not privileges. They state whether the descriptor has the
technical and semantic contract needed for an operation. Security and policy
are still checked separately.

| Capability | Does Not Mean |
| --- | --- |
| `comparable` | The user may compare hidden or protected values without authorization. |
| `orderable` | Every index family can order the descriptor. |
| `hashable` | Hashes are stable across descriptor-version changes. |
| `indexable` | A specific index declaration is valid. The index family must also be compatible. |
| `wire_renderable` | Protected material can be released. |
| `backup_safe` | The caller has backup privilege. |
| `replication_safe` | The route has ordering, idempotency, or release authority. |
| `udr_safe` | Any UDR can receive the value. The UDR package still requires trust and policy. |
| `llvm_eligible` | Acceleration is required or always enabled. |

## Index And Optimizer Use

`indexable` is broad. A descriptor can be indexable for one index family and not
another. The optimizer must still consult index compatibility, operation
descriptor, collation, comparison contract, statistics, policy, and exact
recheck requirements.

An index provides candidate evidence. It does not own row visibility, security,
transaction finality, or predicate truth.

## Stream And Transport Use

`backup_safe`, `replication_safe`, and transport-related capability flags state
that the descriptor can be represented in an admitted stream. The stream route
still requires:

- object privileges;
- protected-material release policy;
- stream frame limits;
- ordering and idempotency where required;
- target descriptor compatibility;
- transaction and recovery safety.

## Visibility And Mutation

Base rows are engine-owned and are changed through descriptor, extension,
domain, catalog, or bootstrap lifecycle operations. Users inspect capability
state through authorized projections, `DESCRIBE TYPE`, `SHOW TYPE`, support
diagnostics, or information-style views.

## Example Inspection

```sql
select td.canonical_type,
       tc.comparable,
       tc.orderable,
       tc.hashable,
       tc.groupable,
       tc.indexable,
       tc.storable,
       tc.wire_renderable
from sys.catalog.type_capability tc
join sys.catalog.type_descriptor td
  on td.descriptor_uuid = tc.descriptor_uuid
order by td.canonical_type;
```

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Capability row missing for descriptor | Refuse operation that requires capability evidence. |
| Operation requires ordering but `orderable` is false | Bind/admission diagnostic. |
| Hash grouping requires `hashable` but flag is false | Bind/admission diagnostic. |
| Index creation requires an unsupported capability | DDL diagnostic. |
| Stream route requires unsafe descriptor | Stream admission diagnostic. |
| Protected value is wire-rendered without release authority | Denied message vector. |
| Capability epoch stale | Rebind, invalidate, or refuse stale execution. |

## Verification Checklist

Proof should demonstrate:

- capability rows exist for all public descriptors that require operation
  admission;
- non-comparable descriptors cannot be used in equality operations;
- non-orderable descriptors cannot be sorted or ordered by B-tree-style keys;
- non-hashable descriptors cannot be used for hash operations;
- index creation checks descriptor and index-family compatibility;
- stream/backup/replication routes check descriptor capability and policy;
- protected descriptors remain redacted unless release is admitted;
- optimizer plans invalidate when capability metadata changes.




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/catalog_reference/sys_catalog_operation_descriptor.md -->

<a id="ch-language-reference-catalog-reference-sys-catalog-operation-descriptor-md"></a>

# sys.catalog.operation_descriptor Catalog Reference

This page documents the authorized catalog surface that describes SBsql/SBLR
operations: argument descriptors, result descriptors, determinism, null and
missing behavior, domain preservation, implementation routing, security, cost,
and index eligibility.

Generation task: `catalog_sys_catalog_operation_descriptor`

Related pages: [Parser To SBLR Pipeline](#ch-language-reference-core-paradigms-parser-to-sblr-pipeline-md),
Operator Type Result Matrix (SBsql Language Reference — Syntax, page XXX),
[Conversion Matrix](#ch-language-reference-data-types-conversion-matrix-md), and
[sys.catalog.type_descriptor](#ch-language-reference-catalog-reference-sys-catalog-type-descriptor-md).

## Role

`sys.catalog.operation_descriptor` is the metadata bridge between a bound
expression or statement and the executable SBLR operation. It tells the binder
and server admission what argument shape is valid, what result shape is
produced, how null/missing values behave, whether domains are preserved or
erased, whether an index can be used, and which implementation route is
admitted.

The page covers the public interpretation of the table. It does not expose
private implementation entry points.

## Keys And Columns

Primary key: `operation_uuid`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `operation_uuid` | UUID | Durable operation identity used by SBLR routing. |
| `operation_family_uuid` | UUID | Operation family identity for grouping, inspection, and admission. |
| `operation_kind` | enum domain | Compare, hash, arithmetic, text, temporal, document, spatial, vector, aggregate, window, locator, management, stream, opaque, or other admitted family. |
| `argument_signature_uuid` | UUID | Ordered argument descriptors, domain rules, parameter modes, and variadic behavior. |
| `result_descriptor_uuid` | UUID | Descriptor of the scalar, row, cursor, stream, command, or diagnostic result. |
| `domain_stack_policy_uuid` | UUID | Domain preservation, erasure, or common-domain derivation behavior. |
| `null_missing_policy_uuid` | UUID | Null, missing, default, unknown, and error behavior. |
| `resource_dependency_set_uuid` | nullable UUID | Required collation, timezone, metric, tokenizer, spatial reference, stream, package, or provider resources. |
| `security_policy_uuid` | UUID | Execution privilege, masking, protected-material, or disclosure policy. |
| `determinism_class` | enum domain | `deterministic`, `stable`, `transaction_stable`, `statement_stable`, `volatile`, or `side_effecting`. |
| `cost_class` | enum/domain | Optimizer cost family and planning hints. |
| `index_eligibility_uuid` | nullable UUID | Index compatibility and exact-recheck contract. |
| `implementation_ref_uuid` | UUID | Public operation route identity for SBLR dispatch. |
| `fallback_ref_uuid` | nullable UUID | Alternate operation route used only when admitted. |

## Operation Identity

`operation_uuid` is the executable identity after binding. Names such as
operator symbols, function names, aggregate names, and statement-family labels
are resolver input. SBLR routes by operation identity and descriptor shape.

## Argument And Result Signatures

An operation's argument signature records:

- number of arguments;
- argument order;
- descriptor or domain expected for each argument;
- parameter mode where applicable;
- variadic behavior;
- null and missing handling;
- implicit assignment conversions allowed before execution;
- protected-material restrictions;
- result-shape derivation rules.

The result descriptor can be scalar, row, cursor, stream, command completion,
diagnostic, or another descriptor-bound shape.

## Determinism

| Determinism Class | Meaning |
| --- | --- |
| `deterministic` | Same arguments and same descriptor/resource versions produce the same result. |
| `stable` | Stable within a catalog/resource epoch. |
| `transaction_stable` | Stable within one transaction context. |
| `statement_stable` | Stable within one admitted statement. |
| `volatile` | May change between calls and cannot be freely reordered or folded. |
| `side_effecting` | Performs state-changing or externally visible work and requires stricter admission. |

Determinism affects constant folding, generated columns, indexes, materialized
views, plans, cacheability, and support diagnostics.

## Null, Missing, And Domain Policies

`null_missing_policy_uuid` separates SQL null, document missing, default value,
unknown, empty, and error behavior. An operation must not silently collapse
these states unless its descriptor says so.

`domain_stack_policy_uuid` controls whether a domain result is preserved,
erased to its carrier, or converted to a common domain. Domain behavior must be
explicit because domains can carry constraints, masks, and operation policies.

## Index Eligibility

`index_eligibility_uuid` says whether an operation can use an index and what
recheck is required.

Index eligibility can depend on:

- operation kind;
- argument descriptors;
- collation;
- comparison contract;
- temporal precision;
- vector metric;
- spatial reference;
- document path;
- graph traversal descriptor;
- exact-recheck requirement;
- protected-material and policy state.

An index is candidate evidence. The executor still rechecks MGA visibility,
security, predicate truth, descriptor compatibility, and result shape.

## Security And Protected Material

`security_policy_uuid` binds the operation to required privileges and
redaction/release policy. Operations that render values, export streams, open
bridge routes, inspect metadata, or release protected material require explicit
policy admission.

An operation can be syntactically valid and descriptor-valid but still refused
by security policy.

## Example Inspection

```sql
select operation_uuid,
       operation_kind,
       argument_signature_uuid,
       result_descriptor_uuid,
       determinism_class
from sys.catalog.operation_descriptor
where operation_kind in ('arithmetic', 'text', 'aggregate')
order by operation_kind, operation_uuid;
```

## Visibility And Mutation

Base rows are engine-owned and created or updated by catalog, type, function,
operator, aggregate, window, extension, package, or bootstrap lifecycle
operations. User statements inspect operation metadata through authorized
projections, `DESCRIBE FUNCTION`, `DESCRIBE OPERATOR`, `SHOW FUNCTIONS`,
operator documentation, or support diagnostics.

## Dependencies And Invalidation

Operation descriptor changes can invalidate:

- prepared expressions and statements;
- generated columns;
- indexes and statistics;
- views and materialized views;
- compiled routines and triggers;
- cast and overload decisions;
- optimizer plans;
- stream and bridge operation routes;
- support-bundle projections.

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Operation name has no visible overload | Bind diagnostic. |
| Multiple overloads rank equally | Ambiguity diagnostic. |
| Argument descriptor mismatch | Bind diagnostic. |
| Result descriptor missing | Catalog diagnostic; operation cannot execute. |
| Null/missing policy absent | Admission diagnostic. |
| Operation is not deterministic but used in deterministic context | DDL or bind diagnostic. |
| Index eligibility missing for indexed expression | DDL or planning diagnostic. |
| Implementation route unavailable | Unsupported or unavailable capability message vector. |
| Security policy denies operation | Denied message vector. |
| Operation epoch stale | Rebind or refuse cached execution. |

## Verification Checklist

Proof should demonstrate:

- operation names resolve to operation UUIDs only after descriptor-aware
  overload selection;
- ambiguous overloads are refused;
- argument and result descriptors match the documented operation contract;
- null, missing, and domain policies are enforced;
- non-deterministic operations are refused in deterministic-only contexts;
- index eligibility requires exact recheck where applicable;
- security policy can deny an otherwise valid operation;
- stale operation metadata invalidates dependent plans and compiled objects;
- SBLR envelopes route by operation identity, not by source text.




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/catalog_reference/sys_security_protected_material_catalog.md -->

<a id="ch-language-reference-catalog-reference-sys-security-protected-material-catalog-md"></a>

# sys.security.protected_material_catalog Catalog Reference

This page documents the authorized catalog surface that records protected
material identity and lifecycle metadata. It does not expose raw protected
values.

Generation task: `catalog_sys_security_protected_material_catalog`

Related pages: [Binary, UUID, And Protected Values](#ch-language-reference-data-types-binary-uuid-and-protected-values-md),
[Security And Sandboxing](#ch-language-reference-core-paradigms-security-and-sandboxing-md),
[sys.security.protected_material_version](#ch-language-reference-catalog-reference-sys-security-protected-material-version-md),
[sys.security.protected_material_policy_binding](#ch-language-reference-catalog-reference-sys-security-protected-material-policy-binding-md), and
[sys.security.protected_material_audit](#ch-language-reference-catalog-reference-sys-security-protected-material-audit-md).

## Role

`sys.security.protected_material_catalog` is the durable identity record for secret,
credential, key, token, protected binary, protected text, protected diagnostic,
or other release-controlled material.

The table stores metadata and references. It must not store or render plaintext
secret material through ordinary catalog projections.

## Keys And Columns

Primary key: `protected_material_uuid`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `protected_material_uuid` | UUID | Stable protected-material identity. |
| `object_class` | enum/text | Protected-material class or admitted subclass. |
| `owner_scope_uuid` | UUID | Owning database, schema, package, principal, security scope, or system scope. |
| `purpose_class` | enum/text | Default purpose for access, release, rotation, backup, replication, or support use. |
| `storage_class` | enum | `direct`, `wrapped`, `split`, `external_reference`, `derived`, or `redacted`. Plaintext catalog storage is not an ordinary public class. |
| `active_version_uuid` | nullable UUID | Active version visible for the current catalog generation. |
| `lifecycle_state` | enum | `active` or `retained_no_active_version`. |
| `retention_policy_uuid` | UUID | Retention policy for metadata and references. |
| `access_policy_uuid` | UUID | Metadata visibility policy. |
| `release_policy_uuid` | UUID | Purpose-bound release policy. |
| `purge_policy_uuid` | UUID | Purge/destruction policy. |
| `audit_policy_uuid` | UUID | Audit evidence policy. |
| `created_local_transaction_id` | uint64 | Creating local MGA transaction ID. |
| `updated_local_transaction_id` | uint64 | Last mutating local MGA transaction ID. |
| `catalog_generation_id` | uint64 | Visible catalog generation. |
| `security_epoch` | uint64 | Security epoch for visibility and release. |

## Protected Material Identity

The UUID identifies the protected material, not the raw secret. Versions carry
rotated or replaced protected references. Policy bindings decide who can inspect
metadata, resolve a reference, release a value, purge a version, or include
evidence in support output.

## Lifecycle States

| State | Meaning |
| --- | --- |
| `active` | Material has an active version and can be resolved where policy admits it. |
| `retained_no_active_version` | Metadata is retained while no version is active, for example after the active version is purged without a replacement. |

## Visibility And Mutation

Rows are hidden by default unless the effective principal can inspect protected
material metadata. Visible projections must redact sensitive fields.

Mutation is performed only by engine-managed protected-material lifecycle
operations such as create, add version (which rotates the active version), or
purge. Ordinary parser text, driver metadata,
support-bundle generation, diagnostics rendering, and catalog projections must
not directly mutate the base table.

## Example Inspection

```sql
select protected_material_uuid,
       object_class,
       purpose_class,
       lifecycle_state,
       active_version_uuid,
       security_epoch
from sys.security.protected_material_catalog
order by protected_material_uuid;
```

Returned rows and columns depend on disclosure policy.

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Metadata hidden by policy | Return redacted not-visible result or denied diagnostic. |
| Active version missing | Refuse resolution and report retained/no-active-version state where visible. |
| Purged material requested | Return purged-state diagnostic without raw reference data. |
| Security epoch stale | Reauthorize and rebind before release or rendering. |
| Support output attempts raw value | Deny or redact; this is a proof failure if leaked. |

## Verification Checklist

Proof should demonstrate:

- raw protected values are absent from ordinary catalog projections;
- hidden metadata does not leak through errors;
- active version selection uses MGA-visible catalog state;
- release requires release policy and audit evidence;
- purge removes protected reference reachability without deleting required audit
  evidence;
- lifecycle states gate resolution and release;
- security epoch changes invalidate cached protected-material handles.




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/catalog_reference/sys_security_protected_material_version.md -->

<a id="ch-language-reference-catalog-reference-sys-security-protected-material-version-md"></a>

# sys.security.protected_material_version Catalog Reference

This page documents the authorized catalog surface that records versioned
protected-material references. A version row represents a rotated, replaced,
derived, or retained protected reference without exposing raw protected values.

Generation task: `catalog_sys_security_protected_material_version`

Related pages: [sys.security.protected_material_catalog](#ch-language-reference-catalog-reference-sys-security-protected-material-catalog-md),
[sys.security.protected_material_policy_binding](#ch-language-reference-catalog-reference-sys-security-protected-material-policy-binding-md),
[sys.security.protected_material_audit](#ch-language-reference-catalog-reference-sys-security-protected-material-audit-md), and
[Binary, UUID, And Protected Values](#ch-language-reference-data-types-binary-uuid-and-protected-values-md).

## Role

`sys.security.protected_material_version` gives protected material an MGA-visible
version history. Rotation, replacement, quarantine, purge, and retention are
recorded without turning raw secret material into ordinary catalog data.

## Keys And Columns

Primary key: `protected_material_version_uuid`

Unique key: `protected_material_uuid`, `version_number`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `protected_material_version_uuid` | UUID | Stable version identity. |
| `protected_material_uuid` | UUID | Owning protected material. |
| `version_number` | uint64 | Monotonic per protected material. |
| `protected_reference_hash` | hash/text | Digest of protected reference metadata. Must not reveal sensitive reference text. |
| `protected_envelope_hash` | hash/text | Digest of wrapped, enveloped, split, or derived metadata. |
| `payload_hash` | hash/text | Integrity hash for referenced payload where policy admits storing it. |
| `storage_class` | enum | `direct`, `wrapped`, `split`, `external_reference`, `derived`, or `redacted`. |
| `rotation_state` | enum | `active`, `rotated`, or `purged`. |
| `valid_from_local_transaction_id` | uint64 | MGA transaction ID that makes the version visible. |
| `valid_until_local_transaction_id` | nullable uint64 | MGA transaction ID that ends active visibility. |
| `retention_policy_uuid` | UUID | Version retention policy. |
| `access_policy_uuid` | UUID | Version metadata access policy. |
| `release_policy_uuid` | UUID | Version release policy. |
| `purge_policy_uuid` | UUID | Version purge policy. |
| `audit_policy_uuid` | UUID | Version audit policy. |
| `retention_until_epoch_millis` | uint64 | Earliest policy-admitted purge time. |
| `legal_hold` | boolean | Purge refusal flag until cleared by policy. |
| `purged` | boolean | Protected reference reachability has been removed. |
| `catalog_generation_id` | uint64 | Visible catalog generation. |
| `security_epoch` | uint64 | Security epoch for visibility and release. |

## Version Selection

Active resolution selects the highest version that is visible to the caller's
MGA snapshot, is not ended for that snapshot, and is admitted by policy.

```text
protected material
|
+-- version 1: rotated
+-- version 2: rotated
+-- version 3: active
```

The active version can differ by transaction snapshot. A version created in an
uncommitted transaction is not ordinary visible state.

## Rotation And Purge

Adding a version closes the previous active version by recording the ending
transaction boundary. Purge removes protected-reference reachability according
to purge policy while preserving hashes and audit evidence where retention
policy requires it.

Purge must not rewrite transaction finality, erase required audit records, or
return plaintext through diagnostics.

## Visibility And Mutation

Rows are exposed only through authorized projections. Mutation is performed by
engine-managed protected-material lifecycle operations: add version (which
rotates the previous active version) or purge.

## Example Inspection

```sql
select protected_material_version_uuid,
       protected_material_uuid,
       version_number,
       rotation_state,
       valid_from_local_transaction_id,
       valid_until_local_transaction_id,
       purged
from sys.security.protected_material_version
where protected_material_uuid = :protected_material_uuid
order by version_number;
```

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Version not visible to snapshot | Return not visible or select an older visible version. |
| Purged version requested | Return purged-state diagnostic without reference data. |
| Legal hold active | Refuse purge. |
| Hash mismatch | Fail closed according to policy. |
| Version gap detected | Refuse resolution until classified. |
| Stale security epoch | Reauthorize before release or metadata rendering. |

## Verification Checklist

Proof should demonstrate:

- version numbers are monotonic per protected material;
- version visibility obeys MGA transaction snapshots;
- rotation closes the previous active version;
- purge removes reachability without leaking raw values;
- legal hold blocks purge;
- hidden versions are redacted from unauthorized projections;
- hash mismatch or version gaps fail closed;
- release and support output use version policy and audit evidence.




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/catalog_reference/sys_security_protected_material_policy_binding.md -->

<a id="ch-language-reference-catalog-reference-sys-security-protected-material-policy-binding-md"></a>

# sys.security.protected_material_policy_binding Catalog Reference

This page documents the authorized catalog surface that binds protected
material or protected-material versions to retention, access, release, purge,
audit, diagnostic, redaction, backup, restore, replication, migration, and
support policies.

Generation task: `catalog_sys_security_protected_material_policy_binding`

Related pages: [sys.security.protected_material_catalog](#ch-language-reference-catalog-reference-sys-security-protected-material-catalog-md),
[sys.security.protected_material_version](#ch-language-reference-catalog-reference-sys-security-protected-material-version-md),
[sys.security.protected_material_audit](#ch-language-reference-catalog-reference-sys-security-protected-material-audit-md), and
[Security And Sandboxing](#ch-language-reference-core-paradigms-security-and-sandboxing-md).

## Role

`sys.security.protected_material_policy_binding` records which policies control
protected material. A material can have material-level policies and
version-level policies. Version-level policy can narrow or override behavior
only where the policy model admits it.

The table is used before metadata rendering, reference resolution, release,
export, backup, restore, replication, purge, diagnostics, and support-bundle
generation.

## Keys And Columns

Primary key: `binding_uuid`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `binding_uuid` | UUID | Stable binding identity. |
| `protected_material_uuid` | UUID | Bound protected material. |
| `protected_material_version_uuid` | nullable UUID | Bound version, or null for material-level binding. |
| `policy_uuid` | UUID | Policy object that controls the behavior. |
| `policy_kind` | enum/text | `retention`, `access`, `release`, `purge`, or `audit`. |
| `diagnostic_state` | enum/text | Engine-assigned diagnostic policy state for the binding. |
| `catalog_generation_id` | uint64 | Visible catalog generation. |
| `security_epoch` | uint64 | Security epoch for visibility and release. |

## Policy Kinds

| Policy Kind | Controls |
| --- | --- |
| `retention` | How long metadata, versions, hashes, and audit evidence are kept. |
| `access` | Who can inspect metadata or resolve references. |
| `release` | Who can obtain raw material for a specific admitted purpose. |
| `purge` | When protected-reference reachability can be destroyed. |
| `audit` | Which events must be recorded and how long evidence is retained. |

## Resolution Rules

When protected material is accessed:

1. bind protected material UUID;
2. select visible version where needed;
3. load material-level policy bindings;
4. load version-level policy bindings;
5. combine policies according to policy precedence;
6. apply security epoch and transaction visibility;
7. admit, redact, deny, or quarantine the request;
8. record audit evidence where policy requires it.

Missing required policy is a refusal, not permission to proceed.

## Visibility And Mutation

Rows are visible only through authorized projections. Base rows are created or
changed by protected-material lifecycle and security-policy operations.

Changing a binding advances the security epoch and invalidates cached protected
handles, stream routes, support projections, metadata projections, and any
compiled or prepared operation that depended on the prior binding.

## Example Inspection

```sql
select protected_material_uuid,
       protected_material_version_uuid,
       policy_kind,
       diagnostic_state,
       security_epoch
from sys.security.protected_material_policy_binding
where protected_material_uuid = :protected_material_uuid
order by policy_kind;
```

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Required policy missing | Refuse protected-material operation. |
| Binding hidden by policy | Redact or hide binding metadata. |
| Binding disabled | Deny the controlled behavior. |
| Release blocked | Deny release and emit release diagnostic where visible. |
| Purge blocked | Refuse purge and preserve reachability. |
| Conflicting policies | Fail closed unless precedence resolves them. |
| Stale security epoch | Reauthorize and invalidate cached state. |

## Verification Checklist

Proof should demonstrate:

- material-level and version-level policies are both considered;
- missing required policy fails closed;
- release, purge, support, backup, replication, and migration behavior depend on
  explicit bindings;
- binding changes advance security epoch and invalidate dependent state;
- unauthorized users cannot infer hidden policy details;
- diagnostics are redacted according to diagnostic/redaction policy;
- audit evidence is recorded where policy requires it.




===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/catalog_reference/sys_security_protected_material_audit.md -->

<a id="ch-language-reference-catalog-reference-sys-security-protected-material-audit-md"></a>

# sys.security.protected_material_audit Catalog Reference

This page documents the authorized catalog surface that records redacted audit
events for protected-material lifecycle, access, release, denial, purge,
policy, and inspection activity.

Generation task: `catalog_sys_security_protected_material_audit`

Related pages: [sys.security.protected_material_catalog](#ch-language-reference-catalog-reference-sys-security-protected-material-catalog-md),
[sys.security.protected_material_version](#ch-language-reference-catalog-reference-sys-security-protected-material-version-md),
[sys.security.protected_material_policy_binding](#ch-language-reference-catalog-reference-sys-security-protected-material-policy-binding-md),
[Security And Sandboxing](#ch-language-reference-core-paradigms-security-and-sandboxing-md), and
Refusal Vectors (SBsql Language Reference — Syntax, page XXX).

## Role

`sys.security.protected_material_audit` is the durable, redacted
evidence surface for protected-material decisions. It lets authorized security,
support, and operations users answer questions such as:

- who attempted to inspect protected material metadata;
- whether a release was allowed or denied;
- which policy kind controlled the decision;
- which protected material and version were involved;
- which diagnostic was returned;
- whether redaction was applied;
- which transaction or catalog generation the event belongs to.

Audit events are evidence. They do not expose raw protected material and do not
grant release authority.

## Keys And Columns

Primary key: `audit_event_uuid`

| Column | Type Family | Requirement |
| --- | --- | --- |
| `audit_event_uuid` | UUID/text | Stable audit event identity. |
| `protected_material_uuid` | UUID | Material involved in the event. |
| `protected_material_version_uuid` | nullable UUID | Version involved, if the event is version-specific. |
| `actor_uuid` | UUID | Effective user, role, agent, or system actor. Redacted in projections where policy requires it. |
| `event_kind` | enum | `create`, `add_version`, `rotate`, `resolve`, `release`, `deny`, `purge`, `policy_change`, `inspect`, `quarantine`, or `support_export`. |
| `decision` | enum | `allow`, `deny`, `redact`, `quarantine`, or `not_applicable`. |
| `diagnostic_code` | nullable text | Message-vector code emitted for denial, refusal, quarantine, or warning. |
| `redacted_detail` | text | Human-readable, policy-redacted event detail. |
| `event_epoch_millis` | uint64 | Event time from engine audit context. |
| `local_transaction_id` | uint64 | Local MGA transaction ID associated with the event, or zero when no user transaction applies. |
| `catalog_generation_id` | uint64 | Catalog generation associated with the event. |
| `redaction_applied` | boolean | True when protected-material redaction policy was applied to event details. |

## Event Kinds

| Event Kind | Meaning |
| --- | --- |
| `create` | Protected material identity was created. |
| `add_version` | A new protected material version was added. |
| `rotate` | Active protected material version changed. |
| `resolve` | A protected reference was resolved without releasing raw material. |
| `release` | Raw material or release-controlled value was admitted for a purpose. |
| `deny` | Access, resolution, release, purge, export, or support collection was denied. |
| `purge` | Protected reference reachability was removed under purge policy. |
| `policy_change` | A policy binding or policy epoch changed. |
| `inspect` | Metadata was inspected through an authorized projection. |
| `quarantine` | Material or version was fenced because integrity or policy state was uncertain. |
| `support_export` | Redacted evidence was included in support output. |

## Redaction Rules

Audit rows are sensitive even when they contain no raw protected value.

Redaction can apply to:

- actor UUID;
- material UUID;
- version UUID;
- policy names or UUIDs;
- endpoint, bridge, stream, backup, replication, migration, or support context;
- diagnostic detail;
- hashes or reference metadata;
- timing information where policy requires it.

`redaction_applied` must be true for rows where protected-material redaction
policy affected rendering. A false value is allowed only when policy confirms
that the rendered audit row contains no protected detail for the caller.

## Visibility And Mutation

Audit rows are append-only evidence from the public user's point of view.
Engine-managed security and protected-material operations create them.
Ordinary catalog queries, support export, diagnostics rendering, and parser
metadata requests must not directly mutate the base audit table.

Retention and purge of audit rows are governed by audit and retention policy.
Purging protected reference reachability must not remove audit rows that policy
requires to remain.

## Example Inspection

```sql
select audit_event_uuid,
       protected_material_uuid,
       event_kind,
       decision,
       diagnostic_code,
       event_epoch_millis,
       redaction_applied
from sys.security.protected_material_audit
where protected_material_uuid = :protected_material_uuid
order by event_epoch_millis;
```

Returned rows and columns depend on the caller's disclosure policy.

## Support-Bundle Behavior

Support bundles may include protected-material audit evidence only through
redacted projections. A support bundle must not include raw secrets, raw
protected payloads, credential text, unredacted protected references, or
unredacted release evidence unless a specific release policy admits that
content for that support purpose.

## Failure Modes

| Condition | Required Behavior |
| --- | --- |
| Audit row hidden by policy | Redact or omit row according to disclosure policy. |
| Actor hidden | Redact `actor_uuid` or render a policy-safe actor class. |
| Material hidden | Redact material identity while preserving authorized event class. |
| Diagnostic detail protected | Render `diagnostic_code` and redacted summary only. |
| Audit append fails for required event | Fail closed or quarantine according to audit policy. |
| Support export requests raw audit detail | Deny or redact. |
| Retention policy blocks deletion | Refuse deletion or purge. |

## Verification Checklist

Proof should demonstrate:

- protected-material lifecycle operations emit audit events where policy
  requires them;
- release denials and release approvals are distinguishable without leaking raw
  material;
- unauthorized users cannot infer hidden protected material through audit
  queries;
- support bundles include only redacted audit evidence;
- purging protected reference reachability preserves required audit rows;
- redaction policy controls actor, material, version, diagnostic, and endpoint
  fields;
- audit append failure does not silently allow an operation that requires audit
  evidence;
- audit rows are transactionally and catalog-generation consistent.




# Language Support




===== FILE SEPARATION =====

<!-- chapter source: Language_Support/README.md -->

<a id="ch-language-support-readme-md"></a>

# ScratchBird Language Support Manual

## Purpose

This manual is the single authority on how ScratchBird implements localized and
multilingual SBsql. It covers both sides of the language surface: the
server-side parser that admits, validates, and normalizes localized SBsql
source; and the client and editor side that provides completion, canonical
preview, local draft SBLR, and localized diagnostics.

Other manuals — the Client and Driver Guide, the Operations and Administration
Guide, the Language Reference — refer to this manual rather than duplicating
its content.

## What Language Support Is

SBsql is the query language of ScratchBird, a Convergent Data Engine. Language
support means that the _syntax_ a user writes can be expressed in a supported
natural language (locale) while the _work_ the engine performs is
locale-independent. A French-speaking user may write SBsql using French
keywords; a German-speaking user may use German keywords. The engine executes
the same canonical representation regardless of the locale used to express it.

This separation is enforced by signed i18n resource packs that carry keyword
spellings, phrase structures, predictive completion tables, renderer templates,
and localized diagnostic messages for each supported locale. The pack is the
only trusted surface through which localization reaches the engine.

## The Two Sides

Language support spans two domains.

**Server parser side.** The parser worker (`sbsql_worker`) loads and validates
signed language resource packs. It performs keyword aliasing, topology-slot
phrase ordering, canonical element stream production, and security checks
(confusable, bidi, mixed-script, mirrored-punctuation). The server owns UUID
identity, descriptor authority, security policy, and MGA transaction finality.
It revalidates any client-produced canonical stream or local draft SBLR before
admitting it.

**Client and editor side.** Drivers and adaptors carry a common resource pack
to support local parsing, predictive completion, SBLR-to-SBsql rendering, and
localized diagnostics in the client process. These resources are explicitly
untrusted until the server revalidates them. Capability negotiation requires
exact resource identity match. Each driver and adaptor is listed by
`component_id` in the language surface manifest with its specific capability
posture.

## The Untrusted Client Resource Boundary

The core invariant, stated in the `common_resource_contract` field of the
language surface manifest, is:

- Client language resources are untrusted until server revalidation.
- Renderer output is canonical, not a reconstruction of the original source text.
- The server owns UUID, descriptor, security, and MGA authority.
- English fallback preserves the preferred language setting; it does not
  silently switch the session locale.
- The system fails closed on profile mismatch — a resource that cannot be
  validated is refused, not silently downgraded.

## Supported Locale Profiles

The following seven exact-tag locale profiles are listed in the i18n resource
pack manifest (`manifest.sblrp.json`) and in the language surface manifest
(`common_resource_pack_metadata.supported_exact_profiles`). Support state as
found in source is shown.

| Exact Tag | Profile UUID | Release Channel | Support State |
|-----------|-------------|-----------------|---------------|
| `en-US` | `sbsql.language.en-US.canonical-recovery.v1` | `release_supported` | `release_supported` |
| `en-CA` | `sbsql.language.en-CA.canonical-recovery.v1` | `release_supported` | `release_supported` |
| `fr-FR` | `sbsql.language.fr-FR.machine-beta.v1` | `beta` | `fully_populated_native_review_required` |
| `fr-CA` | `sbsql.language.fr-CA.machine-beta.v1` | `beta` | `fully_populated_native_review_required` |
| `de-DE` | `sbsql.language.de-DE.machine-beta.v1` | `beta` | `fully_populated_native_review_required` |
| `it-IT` | `sbsql.language.it-IT.machine-beta.v1` | `beta` | `fully_populated_native_review_required` |
| `es-ES` | `sbsql.language.es-ES.machine-beta.v1` | `beta` | `fully_populated_native_review_required` |

The built-in canonical English recovery profile (`sbsql.builtin.recovery.en`,
exact tag `en`) is always available in the parser as a fail-safe fallback. It
is not externally replaceable.

The `fr-FR`, `fr-CA`, `de-DE`, `it-IT`, and `es-ES` profiles carry
`native_review_state: native_technical_review_required_before_release_support`.
They are machine-generated beta resources and are not release-supported. Do not
use them in production deployments that require release-support guarantees.

## Directory Map

| Page | Contents |
|------|----------|
| [overview_and_authority_model.md](#ch-language-support-overview-and-authority-model-md) | End-to-end model: localized surface to canonical stream to UUID to SBLR; common_resource_contract invariants |
| [locale_profiles_and_resource_packs.md](#ch-language-support-locale-profiles-and-resource-packs-md) | Exact-tag locale profiles, i18n resource pack structure, signing, admission |
| [server_parser_language_support.md](#ch-language-support-server-parser-language-support-md) | Engine-side keyword aliasing, topology slots, predictive limits, canonical element stream, security checks |
| [rendering_and_fallback.md](#ch-language-support-rendering-and-fallback-md) | Renderer templates, lossiness classes, canonical-not-source-reconstruction rule, English fallback |
| [client_and_editor_language_surface.md](#ch-language-support-client-and-editor-language-surface-md) | Driver and adaptor language surface manifest, capability negotiation, editor tool protocol |
| [diagnostics_reference.md](#ch-language-support-diagnostics-reference-md) | Consolidated table of SBSQL.LANG_RESOURCE.* diagnostic codes with guidance |

## Reading Model

If you are a developer integrating a driver or adaptor, start with
[client_and_editor_language_surface.md](#ch-language-support-client-and-editor-language-surface-md),
then read [rendering_and_fallback.md](#ch-language-support-rendering-and-fallback-md) for lossiness
classification and diagnostic handling.

If you are an operator installing or updating resource packs, start with
[locale_profiles_and_resource_packs.md](#ch-language-support-locale-profiles-and-resource-packs-md),
then cross-reference the Operations and Administration Guide for the resource
pack install layout.

If you are auditing parser-side security, start with
[server_parser_language_support.md](#ch-language-support-server-parser-language-support-md) and
[overview_and_authority_model.md](#ch-language-support-overview-and-authority-model-md).

## Cross-References

- Engine-side language profile concepts: [../Language_Reference/core_paradigms/sbsql_language_profiles.md](#ch-language-reference-core-paradigms-sbsql-language-profiles-md)
- Driver and adaptor integration: ../Client_Driver_Guide/README.md (ScratchBird — Application Development and Integration, page XXX)
- Resource pack installation layout: ../Operations_Administration/installation_and_output_layout.md (ScratchBird — Operations, Security, and Autonomy, page XXX)
- Parser registration: ../Operations_Administration/parser_registration_and_routes.md (ScratchBird — Operations, Security, and Autonomy, page XXX)

## Draft Status

This is a draft manual. All technical claims have been verified against the
source tree at `project/src/parsers/sbsql_worker/resources/language_resource_contract.cpp`,
`project/drivers/language/sbsql_language_surface_manifest.json`,
`project/drivers/language/sbsql_editor_tool_protocol.schema.json`, and
`project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack/manifest.sblrp.json`.
Claims that could not be verified from source have been omitted.




===== FILE SEPARATION =====

<!-- chapter source: Language_Support/overview_and_authority_model.md -->

<a id="ch-language-support-overview-and-authority-model-md"></a>

# Overview and Authority Model

## Purpose

This page describes the end-to-end model for how localized SBsql becomes
engine-executed SBLR, what the authority boundary between client and server is,
and why localization never changes engine authority over UUID identity,
descriptors, security policy, or MGA transaction finality.

## The Pipeline

The canonical pipeline is:

```
localized SBsql source
  -> canonical element stream
  -> UUID binding
  -> SBLR
  -> engine execution
```

Each stage has a defined trust boundary.

**Localized SBsql source.** The user or application produces SBsql text using
the keyword spellings and phrase structures of their locale (for example,
`fr-CA` or `en-US`). This text is untrusted at the point it arrives at the
parser. The locale is identified by an exact-tag profile (for example `fr-CA`)
associated with the active language resource pack.

**Canonical element stream.** The parser worker normalizes localized tokens
against keyword aliases loaded from the active language resource pack and
produces a canonical element stream. Each element carries its canonical text
and ID, a hash of the original localized text, and a source span. Topology
normalization — phrase ordering via topology slots — must occur before UUID
resolution. This ordering is enforced by the `normalized_before_uuid_resolution`
flag on every canonical element stream.

The canonical element stream requires server revalidation. A stream produced by
a client or a local draft parser is not trusted until the server has admitted it.

**UUID binding.** Once the stream is normalized, identifiers are resolved
against UUID-keyed catalog objects. The server owns UUID and descriptor
authority. No client resource can override this.

**SBLR.** The Standardized Binding Language Representation (SBLR) is the
admitted execution form. It carries UUID identity and is the only form the
engine executes. Localized source text does not persist into SBLR. The
canonical element stream's localized text hash is retained for diagnostics but
does not influence execution.

**Engine execution.** The engine executes admitted SBLR. MGA transaction
finality and security policy enforcement are server-side and are not influenced
by language profiles.

## The common_resource_contract Invariants

The `common_resource_contract` field of
`project/drivers/language/sbsql_language_surface_manifest.json` states the
invariants that every driver, adaptor, and editor tool must uphold:

| Field | Value | Meaning |
|-------|-------|---------|
| `draft_sblr_is_untrusted_until_server_admission` | `true` | A local draft SBLR produced by a client driver is untrusted until the server revalidates and admits it. |
| `predictive_text_must_not_infer_hidden_objects` | `true` | Completion suggestions must not disclose objects the current principal cannot access. |
| `renderer_output_is_canonical_not_source_reconstruction` | `true` | When the renderer produces SBsql from SBLR it outputs a canonical form, not the user's original source text. |
| `server_owns_mga_transaction_finality` | `true` | MGA transaction commit/abort authority rests entirely with the server. |
| `server_owns_uuid_descriptor_security_authority` | `true` | UUID resolution, descriptor validation, and security policy are server-only. |
| `server_revalidates_client_sblr` | `true` | Any SBLR a client produces is revalidated before the server executes it. |
| `standard_english_fallback_preserves_preferred_language` | `true` | When English fallback is used because the preferred-language renderer is unavailable, the session language setting is not changed. |

These invariants are not implementation notes — they are contract terms that
the validation layer in `language_resource_contract.cpp` enforces. A language
resource pack that violates them fails closed at admission time.

## Why Localization Never Changes Engine Authority

Localization operates entirely in the _surface layer_: keyword spelling,
phrase ordering, predictive completion, and diagnostic message language. None
of these surfaces has access to:

- UUID catalog resolution
- Descriptor or schema authority
- Security policy evaluation
- MGA transaction commit/abort

The canonical element stream passes these concerns upward unchanged. A
French-keyword query that references a table named `employees` produces the
same UUID reference as an English-keyword query. The language profile affects
what the user types; it does not affect what the engine does.

The server's enforcement of this separation is not optional. The parse profile
order is fixed at four deterministic steps:

1. `explicit_syntax_profile` — use a profile explicitly declared in the
   session or query
2. `preferred_language_and_dialect` — use the session's preferred language
   resource pack
3. `canonical_english_fallback_when_preferred_fails` — fall back to canonical
   English if the preferred-language parse fails; emit
   `SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH`
4. `fail_closed` — if no parse succeeds, emit
   `SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH` and refuse the
   operation

No other order is accepted. The validation function
`ValidateParseProfileOrder` in `language_resource_contract.cpp` enforces this
at resource admission time.

## Fail-Closed Semantics

The system fails closed, not open, on any language resource failure. A resource
that is missing, unsigned, revoked, expired, or incompatible produces an error
diagnostic and the operation is refused. The failure kinds and their diagnostic
codes are enumerated in [diagnostics_reference.md](#ch-language-support-diagnostics-reference-md).

Revoked or removed resources are refused at load time, not at use time. An
attempt to load a revoked bundle produces `SBSQL.LANG_BUNDLE.REVOKED`. An
attempt to use a revoked profile produces `SBSQL.LANG_RESOURCE.REVOKED`.

## Cross-References

- Locale profiles and pack structure: [locale_profiles_and_resource_packs.md](#ch-language-support-locale-profiles-and-resource-packs-md)
- Rendering and fallback behavior: [rendering_and_fallback.md](#ch-language-support-rendering-and-fallback-md)
- Parser security checks: [server_parser_language_support.md](#ch-language-support-server-parser-language-support-md)
- Language profile concepts in the Language Reference: [../Language_Reference/core_paradigms/sbsql_language_profiles.md](#ch-language-reference-core-paradigms-sbsql-language-profiles-md)




===== FILE SEPARATION =====

<!-- chapter source: Language_Support/locale_profiles_and_resource_packs.md -->

<a id="ch-language-support-locale-profiles-and-resource-packs-md"></a>

# Locale Profiles and Resource Packs

## Purpose

This page describes the exact-tag locale profiles supported by ScratchBird, the
structure of the i18n resource pack that carries those profiles, and the
signing, admission, and lifecycle model that governs when a pack may be used.

## Exact-Tag Locale Profiles

ScratchBird identifies a language profile by an _exact tag_: a BCP-47 locale
identifier that includes both language and region (for example `fr-CA`, not
`fr`). Using exact tags rather than partial tags prevents ambiguous fallback
chains and allows the parser to enforce deterministic resource identity.

The seven profiles listed in both the language surface manifest
(`common_resource_pack_metadata.supported_exact_profiles`) and the i18n
resource pack manifest (`profiles` array) are:

| Exact Tag | Profile UUID | Release Channel | Native Review State | Support State |
|-----------|-------------|-----------------|---------------------|---------------|
| `en-US` | `sbsql.language.en-US.canonical-recovery.v1` | `release_supported` | `source_authority_reviewed` | `release_supported` |
| `en-CA` | `sbsql.language.en-CA.canonical-recovery.v1` | `release_supported` | `source_authority_reviewed` | `release_supported` |
| `fr-FR` | `sbsql.language.fr-FR.machine-beta.v1` | `beta` | `native_technical_review_required_before_release_support` | `fully_populated_native_review_required` |
| `fr-CA` | `sbsql.language.fr-CA.machine-beta.v1` | `beta` | `native_technical_review_required_before_release_support` | `fully_populated_native_review_required` |
| `de-DE` | `sbsql.language.de-DE.machine-beta.v1` | `beta` | `native_technical_review_required_before_release_support` | `fully_populated_native_review_required` |
| `it-IT` | `sbsql.language.it-IT.machine-beta.v1` | `beta` | `fully_populated_native_review_required` | `fully_populated_native_review_required` |
| `es-ES` | `sbsql.language.es-ES.machine-beta.v1` | `beta` | `native_technical_review_required_before_release_support` | `fully_populated_native_review_required` |

In addition, the built-in canonical English recovery profile is always present
in the parser:

| Exact Tag | Profile UUID | Channel | Notes |
|-----------|-------------|---------|-------|
| `en` | `sbsql.builtin.recovery.en` | `release_supported` | Built-in, not externally replaceable, used as fallback only |

The `en` built-in profile is the English fallback used when the
preferred-language parse fails. It is built into the parser binary and cannot
be overridden by an external resource pack.

### Beta Profile Caution

The `fr-FR`, `fr-CA`, `de-DE`, `it-IT`, and `es-ES` profiles are in `beta`
channel and carry the support state `fully_populated_native_review_required`.
They are machine-generated and have not completed native technical review. They
are admitted as limited-support resources and emit a lifecycle warning diagnostic
`SBSQL.LANG_RESOURCE.BETA_LIMITED_SUPPORT` when loaded. Do not use them in
production deployments that require release-support guarantees.

## The i18n Resource Pack

The i18n resource pack is located at:

```
project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack/
```

It contains three top-level files:

| File | Purpose |
|------|---------|
| `manifest.sblrp.json` | Pack manifest: schema version, resource identity, profiles array, file list with per-file SHA-256 hashes, generation metadata |
| `hashes.sha256` | External hash file for pack integrity verification |
| `manifest.sblrp.sig` | Pack signature file |

### manifest.sblrp.json Structure

The manifest's top-level fields include:

| Field | Value (from source) |
|-------|---------------------|
| `schema_version` | `sbsql.language_resource_pack_manifest.v1` |
| `pack_schema_version` | `sbsql.language_resource_pack.v1` |
| `resource_identity` | `sbsql.common_resource_pack.v1` |
| `common_resource_hash` | `sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc` |
| `dialect_profile_uuid` | `sbsql.v3` |
| `topology_profile_uuid` | `topology.sbsql.canonical.v1` |
| `registry_row_count` | `2645` |
| `translation_source_row_count` | `2721` |
| `generated_by` | `project/tools/sb_parser_gen/generate_sbsql_language_resource_pack.py` |

The `files` array contains one entry per resource file with a relative `path`
and a `sha256` hash. The `profiles` array contains one entry per locale with
`exact_tag`, `profile_uuid`, `release_channel`, `support_state`,
`native_review_state`, and `resource_path`.

### Resource Subdirectory Layout

The `resources/` directory inside the pack is organized into functional
subdirectories:

| Subdirectory | Contents |
|-------------|---------|
| `canonical/` | Dialect baseline, system object name registry, translation source corpus, style guide |
| `conformance/` | Conformance corpus, online translation verification corpus |
| `diagnostics/` | Database message catalog, diagnostic messages |
| `dialects/` | Per-dialect profile (e.g., `sbsql-v3-dialect-profile.json`) |
| `languages/` | Per-locale language profile (`en-US/language-profile.json`, `fr-FR/language-profile.json`, etc.) |
| `phrases/` | Phrase table |
| `predictive/` | Predictive grammar |
| `provenance/` | Native review status, provenance records |
| `rendering/` | Rendering templates |
| `resolver/` | Resolver policy |
| `topology/` | Topology profiles |
| `unicode/` | Unicode policy |

## Signing and Admission

Every language resource pack must be signed before it can be admitted by the
server. The validation layer in `language_resource_contract.cpp` enforces this:

- A bundle missing `signature_id` produces `SBSQL.LANG_RESOURCE.UNSIGNED` and
  fails closed.
- A bundle without `signing_key_id` produces `SBSQL.LANG_RESOURCE.SIGNING_KEY_MISSING`.
- An unsigned bundle (where `signed_bundle` is false and it is not a parser
  language library) produces `SBSQL.LANG_BUNDLE.UNSIGNED`.

Release-supported and deprecated packs additionally require full governance
evidence: `author_id`, `reviewer_id`, `native_technical_reviewer_id`,
`security_reviewer_id`, `support_owner_id`, `release_approval_id`,
`revocation_policy_id`, `contribution_provenance_id`, and
`governance_evidence_id` must all be present. Missing any of these produces a
specific `SBSQL.LANG_RESOURCE.*_MISSING` error.

## Lifecycle States

A language resource bundle progresses through lifecycle states. The known
states, as validated by `IsKnownLanguagePackLifecycleState` in source, are:

`staged` → `generated` → `signed` → `published` → `admitted` → `downloaded`
→ `cached` → `delta_updated` → `rolled_back` → `revoked` → `expired`
→ `removed`

A bundle in `revoked`, `expired`, or `removed` state fails closed at load time.
A bundle in `rolled_back` state is admitted with a warning diagnostic
`SBSQL.LANG_BUNDLE.ROLLED_BACK`.

## Provenance Requirements

Every release-supported or deprecated resource requires provenance rows. Each
provenance row must carry `source_name`, `source_version`, `license_id`,
`transformation_id`, `sbom_component_id`, and `third_party_notice_id`. The
`redistribution_allowed` flag must be `true`. Missing or incomplete provenance
produces `SBSQL.LANG_RESOURCE.PROVENANCE_MISSING` or
`SBSQL.LANG_RESOURCE.PROVENANCE_INCOMPLETE`.

## Predictive Resource Limits

Language resource packs may carry predictive (completion) tables. Release
safety limits enforced by the validation layer are:

| Limit | Maximum |
|-------|---------|
| Predictive table size | 8 MB |
| Transition fanout | 1024 |
| Completion results | 4096 |
| Generation time | 1000 ms |
| Predictive memory | 16 MB |
| Nested expansion depth | 64 |

Exceeding any of these limits produces `SBSQL.LANG_RESOURCE.PREDICTIVE_LIMIT_EXCEEDED`.

## Installation Layout

For the operator-facing resource pack installation layout and how packs are
loaded into a running deployment, see:
../Operations_Administration/installation_and_output_layout.md (ScratchBird — Operations, Security, and Autonomy, page XXX)

and

../Operations_Administration/parser_registration_and_routes.md (ScratchBird — Operations, Security, and Autonomy, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Language_Support/server_parser_language_support.md -->

<a id="ch-language-support-server-parser-language-support-md"></a>

# Server Parser Language Support

## Purpose

This page describes how the ScratchBird server-side parser worker handles
localized SBsql: how it aliases localized keywords to canonical forms, how
topology slots enforce phrase ordering, how the canonical element stream is
produced, what limits apply to predictive resources, and what security checks
are applied to localized identifiers.

For the conceptual overview of language profiles, see the Language Reference
page: [../Language_Reference/core_paradigms/sbsql_language_profiles.md](#ch-language-reference-core-paradigms-sbsql-language-profiles-md).

## Keyword and Phrase Aliasing

The parser resolves localized tokens against a keyword alias table loaded from
the active language resource pack. Each entry in the alias table associates a
localized text form with a `canonical_id` and a `surface_id`.

From `cst.cpp`, the alias resolution logic:

- The alias table is searched for a case-insensitive ASCII match against the
  raw token text.
- If an alias is found, the canonical text is taken from `alias.canonical_text`
  and uppercased as ASCII.
- If no alias matches but the token carries a `canonical_text` value directly,
  that value is used.
- If neither match occurs, the token text itself is uppercased and used as the
  canonical form.

This means that a French keyword like `SÉLECTIONNER` (if present in the
language profile) maps to the same canonical ID as the English keyword
`SELECT`. The engine never sees the original localized token after the canonical
element stream is produced.

## Topology Slots and Phrase Ordering

Language profiles can define alternative phrase orderings. For example, some
languages place modifiers after the verb while English places them before.
Topology slots define how phrase components are sequenced.

Each topology slot requires:

- `slot_id` — unique slot identifier
- `phrase_id` — the phrase this slot belongs to
- `topology_role` — the role this slot plays (e.g., head, modifier)
- `canonical_id` — the canonical token or surface this slot resolves to
- `surface_id` — the surface declaration this slot references
- `min_elements` and `max_elements` — cardinality constraints (min must be > 0)

Topology normalization must occur **before** UUID resolution. This is enforced
by the `normalized_before_uuid_resolution` flag on the canonical element stream.
If a stream arrives without this flag set, the validation layer produces
`SBSQL.CANONICAL_STREAM.POST_UUID_NORMALIZATION` and refuses the stream.

## The Canonical Element Stream

The canonical element stream is the internal representation passed from the
parser to the UUID resolution and SBLR production stages. Its structure, as
validated by `ValidateCanonicalElementStream` in `language_resource_contract.cpp`,
requires:

| Field | Requirement |
|-------|-------------|
| `resource_identity` | Must be present — identifies which common resource pack produced this stream |
| `language_profile_uuid` | Must be present — identifies the locale profile used |
| `exact_tag` | Must be present — the exact locale tag |
| `dialect_profile_uuid` | Must be present |
| `topology_profile_uuid` | Must be present |
| `common_resource_hash` | Must be present — hash of the resource pack used |
| `source_hash` | Must be present — hash of the original localized source text |
| `canonical_order_id` | Must be present — identifies the canonical ordering rule applied |
| `normalized_before_uuid_resolution` | Must be `true` |
| `server_revalidation_required` | Must be `true` |
| `elements` | Must be non-empty |

Each element in the stream requires:

| Field | Requirement |
|-------|-------------|
| `canonical_text` | Canonical parser text for this token or phrase element |
| `canonical_id` | Canonical token or surface identifier |
| `localized_text_hash` | Hash of the original localized text (retained for diagnostics) |
| `source_span.length` | Must be > 0 — the span in the localized source |

The localized text hash is retained so that support bundles and diagnostics can
reference original source position without re-exposing the original text. The
canonical stream itself does not contain the original localized text.

## The `canonical_english_fallback_used` Flag

The CST document carries a `canonical_english_fallback_used` flag (set in
`cst.cpp`). When `true`, it indicates that the preferred-language parse failed
and canonical English was used instead. This flag is surfaced in diagnostics and
determines which `SBSQL.LANG_RESOURCE.*` diagnostic code is emitted.

## The `input_language_fallback_tag` Wire Field

The SBWP wire protocol (`sbsql_sbwp_wire.cpp`) carries an
`input_language_fallback_tag` field on the session state. When present, this
tag is included in the message vector so that diagnostics can report the
fallback locale applied during the session. The field is redacted in telemetry
and support bundle exports consistent with the no-disclosure contract.

## Parse Profile Order

The server applies language profile resolution in a fixed four-step order. No
other order is accepted. The `ValidateParseProfileOrder` function enforces this
at resource admission time.

| Step | Name | Behavior |
|------|------|---------|
| 1 | `explicit_syntax_profile` | Use a profile explicitly declared in the session or statement |
| 2 | `preferred_language_and_dialect` | Use the session's preferred locale resource pack |
| 3 | `canonical_english_fallback_when_preferred_fails` | Fall back to canonical English if step 2 fails; emit `SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH` |
| 4 | `fail_closed` | Refuse the operation; emit `SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH` |

## Predictive Resources and Limits

Language resource packs may carry predictive tables for completion. These tables
are subject to release safety limits. The limits are enforced by
`ValidatePredictiveTextResourceFootprint` and `ValidateLanguageResourceManifest`
in `language_resource_contract.cpp`:

| Resource dimension | Release safety limit |
|--------------------|---------------------|
| Table size | 8 MB (`max_predictive_table_bytes`) |
| Transition fanout | 1024 (`max_transition_fanout`) |
| Completion results | 4096 (`max_completion_results`) |
| Generation time | 1000 ms (`max_generation_millis`) |
| Memory use | 16 MB (`max_predictive_memory_bytes`) |
| Nested expansion depth | 64 (`max_nested_expansion_depth`) |

Predictive resources must enforce limits deterministically
(`deterministic_limit_enforcement` must be `true`). Completion results must
not disclose hidden or inaccessible objects
(`hidden_object_no_disclosure` must be `true`).

Every predictive state in a language element manifest must carry
`server_revalidation_required: true`. A predictive state without server
revalidation is rejected with
`SBSQL.LANG_ELEMENT_MANIFEST.PREDICTIVE_REVALIDATION_REQUIRED`.

## Security Checks

The parser applies several security checks to localized identifier and literal
text. These checks are implemented in `language_resource_contract.cpp`.

### Confusable and Mixed-Script Identifiers

The function `HasMixedScriptOrConfusableRisk` inspects identifier text for
security risks:

- **Bidi control characters** (Unicode code points U+202A–U+202E and
  U+2066–U+2069) are refused unless the confusable policy explicitly permits
  them (`allow_bidi_controls: true`).
- **Mirrored punctuation** (U+061B, U+061F, U+FD3E, U+FD3F) is refused unless
  the policy permits it (`allow_mirrored_punctuation: true`).
- **Mixed-script identifiers** — identifiers that contain characters from more
  than one non-ASCII script class — are refused unless
  `allow_mixed_script_identifiers: true`.
- **Transliteration aliases** using Greek or Cyrillic characters are refused
  unless `allow_transliteration_aliases: true`.

Script classes recognized are: ASCII, Latin, Greek, Cyrillic, Arabic, Hebrew,
and Other (any other non-ASCII character).

### Locale Literal Classification

The function `ClassifyLocaleLiteral` classifies localized literals for
admission:

- Localized digit forms (Arabic-Indic U+0660–U+0669, Extended Arabic-Indic
  U+06F0–U+06F9, Devanagari U+0966–U+096F) require an explicit locale policy
  that admits them; without one the literal produces
  `LocaleLiteralClassification::kRequiresExplicitProfile`.
- Decimal comma shape (digit-comma-digit) requires `admits_decimal_comma: true`.
- Localized month names (French month names are checked) require
  `admits_localized_month_names: true`.
- Bidi control characters in a literal cause `kRefuseAmbiguous`.
- Mirrored punctuation in a literal causes `kRefuseAmbiguous`.

### Resource Pack Admission Security

At pack admission time (`AdmitLanguageResourceBundleOperation`), the bundle
must pass security policy admission before it can be loaded:

- `admitted_by_security_policy` must be `true`.
- The bundle must be compatible with the current parser version
  (`compatible_with_parser` must be `true`).

A bundle that fails either check is refused at load time with
`SBSQL.LANG_BUNDLE.SECURITY_ADMISSION_REQUIRED` or
`SBSQL.LANG_BUNDLE.INCOMPATIBLE`.

## Cross-References

- Canonical element stream in the context of rendering: [rendering_and_fallback.md](#ch-language-support-rendering-and-fallback-md)
- Locale profiles and resource pack signing: [locale_profiles_and_resource_packs.md](#ch-language-support-locale-profiles-and-resource-packs-md)
- Diagnostic codes: [diagnostics_reference.md](#ch-language-support-diagnostics-reference-md)
- Language profile concepts: [../Language_Reference/core_paradigms/sbsql_language_profiles.md](#ch-language-reference-core-paradigms-sbsql-language-profiles-md)




===== FILE SEPARATION =====

<!-- chapter source: Language_Support/rendering_and_fallback.md -->

<a id="ch-language-support-rendering-and-fallback-md"></a>

# Rendering and Fallback

## Purpose

This page describes how ScratchBird renders SBLR back to SBsql text for
display, what lossiness classes the renderer assigns, why rendered output is
canonical rather than a source reconstruction, and how the English fallback
mechanism works when the preferred-language renderer is unavailable.

## Renderer Templates

Rendering templates are carried in the language resource pack under
`resources/rendering/rendering-templates.json`. Each renderer entry in a
language element manifest requires a `renderer_id`, a `profile_uuid`, and a
`canonical_english_fallback_profile_uuid`. Every renderer must carry
`server_revalidation_required: true`; a renderer without server revalidation is
rejected with `SBSQL.LANG_ELEMENT_MANIFEST.RENDERER_REVALIDATION_REQUIRED`.

The rendering subsystem in `rendering.cpp` produces a diagnostic record for
each rendering decision. The diagnostic includes `render_decision`,
`renderer_lossiness`, `selected_profile`, `fallback_profile`,
`canonical_english_fallback`, and `server_revalidation_required` fields.

## Renderer Output Is Canonical, Not Source Reconstruction

A fundamental invariant stated in `common_resource_contract` is:

> `renderer_output_is_canonical_not_source_reconstruction: true`

When the renderer produces SBsql from SBLR, it outputs a canonical form of the
statement — keyword casing, structure, and whitespace are governed by the
renderer templates. It does not attempt to reproduce the original localized text
the user typed. The original source is not retained in SBLR; only a hash of it
is carried in the canonical element stream for diagnostic purposes.

This is a deliberate security and authority boundary. The original text may have
contained confusable characters, mixed scripts, or unusual encodings that were
sanitized during canonicalization. Reconstructing original source from SBLR
would bypass those security checks.

Source reconstruction is actively refused. If a render request sets
`source_reconstruction_requested: true`, the render decision is
`kRefuseSourceReconstruction` and the diagnostic
`SBSQL.LANG_RESOURCE.RENDERER_SOURCE_RECONSTRUCTION_FORBIDDEN` is emitted.

## Renderer Lossiness Classes

Every render operation produces a lossiness classification. The five lossiness
classes are defined in the `common_resource_pack_metadata.renderer_lossiness_classes`
field of the language surface manifest and in the
`ExpectedRendererLossinessClasses` function in `language_resource_contract.cpp`.

| Lossiness Class | Code Name | Meaning |
|----------------|-----------|---------|
| Lossless canonical | `lossless_canonical` | The preferred renderer is the canonical English profile itself. Output is identical to canonical form. |
| Canonical equivalent | `canonical_equivalent` | The preferred-language renderer covers the full statement. Output is semantically equivalent to canonical. |
| Preferred language partial | `preferred_language_partial` | The preferred-language renderer covers only part of the statement. Some elements fall back to canonical form. |
| Canonical English fallback | `canonical_english_fallback` | The preferred-language renderer was unavailable; canonical English renderer was used. |
| Not renderable | `not_renderable` | No admitted renderer can produce output. The statement cannot be rendered. |

The lossiness class is always emitted with the diagnostic
`SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED`.

## Render Decision Logic

The `ClassifySblrRenderRequest` function in `language_resource_contract.cpp`
evaluates render requests in this order:

| Condition checked | Outcome |
|------------------|---------|
| `sblr_uuid_authority_valid` is false | `kRefuseMissingCanonicalAuthority` — diagnostic `SBSQL.LANG_RESOURCE.MISSING_CANONICAL_AUTHORITY` |
| `source_reconstruction_requested` is true | `kRefuseSourceReconstruction` — diagnostic `SBSQL.LANG_RESOURCE.RENDERER_SOURCE_RECONSTRUCTION_FORBIDDEN` |
| `resource_revoked` is true | `kRefuseRevokedResource` — diagnostic `SBSQL.LANG_RESOURCE.REVOKED` |
| `resource_incompatible` is true | `kRefuseIncompatibleResource` — diagnostic `SBSQL.LANG_RESOURCE.INCOMPATIBLE` |
| `preferred_renderer_available` is true | `kPreferredLanguage` — use preferred renderer |
| `canonical_english_renderer_available` is true | `kCanonicalEnglishFallback` — use English fallback |
| None of the above | `kRefuseRendererUnavailable` — diagnostic `SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE` |

## English Fallback Behavior

When the preferred-language renderer is unavailable (step `kCanonicalEnglishFallback`
above), the system selects the canonical English renderer. The diagnostic
`SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH` is emitted with severity
`warning`. The render output carries `canonical_english_fallback: true` in its
diagnostic record.

The `common_resource_contract` invariant
`standard_english_fallback_preserves_preferred_language: true` means that using
the English renderer does not change the session's preferred language setting.
The session remains configured for the preferred locale; only the render output
for this particular SBLR is produced in English.

Fallback is distinct from fail-closed. Fallback occurs when the preferred
renderer is simply unavailable for rendering — the parse succeeded in the
preferred language, SBLR was produced, but rendering back to text must use
English because the preferred-language renderer template is not available.
Fail-closed occurs when no parse succeeds at all.

## Restore and Renderer Fallback

When a session restores SBLR from a prior operation (`ClassifyRestoreLanguageResourceState`
in `language_resource_contract.cpp`), the restoration decision also handles
renderer availability:

| State | Meaning |
|-------|---------|
| `exact_resource_available` | The exact resource pack and preferred renderer are available |
| `canonical_authority_valid_renderer_fallback` | UUID authority is valid but the exact renderer is unavailable; canonical English renderer will be used |
| `refuse_revoked_resource` | Refused — resource is revoked |
| `refuse_missing_canonical_authority` | Refused — SBLR UUID authority cannot be validated |
| `refuse_incompatible_resource` | Refused — resource is incompatible with current parser |

## Fallback and Rendering Diagnostics

The two categories of language diagnostics emitted from the rendering layer are
listed in `common_resource_pack_metadata` in the language surface manifest.

**Fallback diagnostics** (session-level):

| Code | When emitted |
|------|-------------|
| `SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH` | Preferred-language renderer unavailable; canonical English selected |
| `SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH` | No parse succeeded; operation refused |

**Rendering diagnostics** (per render operation):

| Code | When emitted |
|------|-------------|
| `SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED` | Always when a render decision is made (records which lossiness class applies) |
| `SBSQL.LANG_RESOURCE.RENDERER_SOURCE_RECONSTRUCTION_FORBIDDEN` | When `source_reconstruction_requested` is true |
| `SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE` | When no renderer is available |

For the full diagnostic reference, see [diagnostics_reference.md](#ch-language-support-diagnostics-reference-md).

## Cross-References

- Lossiness classes used in client capability declarations: [client_and_editor_language_surface.md](#ch-language-support-client-and-editor-language-surface-md)
- Authority model explaining why source reconstruction is forbidden: [overview_and_authority_model.md](#ch-language-support-overview-and-authority-model-md)
- Diagnostics table: [diagnostics_reference.md](#ch-language-support-diagnostics-reference-md)




===== FILE SEPARATION =====

<!-- chapter source: Language_Support/client_and_editor_language_surface.md -->

<a id="ch-language-support-client-and-editor-language-surface-md"></a>

# Client and Editor Language Surface

## Purpose

This page describes the driver and editor side of ScratchBird language support:
the language surface manifest, capability negotiation, per-component capability
posture, the editor tool protocol schema, and how client-produced draft SBLR is
handled. The Client and Driver Guide references this page for language surface
details rather than restating them.

## The Language Surface Manifest

The language surface manifest is located at:

```
project/drivers/language/sbsql_language_surface_manifest.json
```

It is the authoritative declaration of how every driver, adaptor, and tool
exposes language support. Its top-level fields are:

| Field | Value |
|-------|-------|
| `schema_version` | `sbsql.driver_language_surface_manifest.v1` |
| `resource_identity` | `sbsql.common_resource_pack.v1` |
| `protocol_schema` | `project/drivers/language/sbsql_editor_tool_protocol.schema.json` |
| `driver_package_manifest` | `project/drivers/DriverPackageManifest.csv` |

The manifest contains three top-level sections: `common_resource_contract`,
`common_resource_pack_metadata`, and `components`.

## The common_resource_contract

All seven invariants in `common_resource_contract` apply uniformly to every
component. They are described in detail in
[overview_and_authority_model.md](#ch-language-support-overview-and-authority-model-md). No driver or
adaptor may relax them.

## The common_resource_pack_metadata

The `common_resource_pack_metadata` section describes the shared resource pack
used by all components:

| Field | Value |
|-------|-------|
| `resource_identity` | `sbsql.common_resource_pack.v1` |
| `support_state` | `release_supported` |
| `resource_pack_path` | `project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack` |
| `resource_pack_manifest_sha256` | `sha256:a7a30e7650ad7d4f2402bf3ab502d37cb8ff8b0dde171752320ee288aaab9ec2` |
| `resource_hash` | `sha256:f5469159a874fad2a22765e4c75d938e6d4cc8ac740169fb215a96dcac2b1be3` |
| `resource_pack_common_resource_hash` | `sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc` |

The `supported_exact_profiles` field lists the seven supported locale tags:
`en-US`, `en-CA`, `fr-FR`, `fr-CA`, `de-DE`, `it-IT`, `es-ES`.

Deterministic validation requirements:
- `no_wall_clock_fields: true`
- `sort_keys_for_hash: true`
- `stable_utf8_json: true`

## The Components Array

The `components` array lists 31 entries — one per driver, adaptor, or tool that
exposes a language surface. Each entry carries the required fields defined by
the editor tool protocol schema.

### Component Categories

Components fall into three categories:

| `component_category` | Count | Description |
|---------------------|-------|-------------|
| `driver` | 21 | Language drivers that implement the common resource protocol directly |
| `adaptor` | 9 | Adaptors that delegate language surface operations to an underlying driver or common protocol consumer |
| `tool` | 1 | Native CLI tool |

### Drivers

The following component IDs have `component_category: driver`:

`driver:adbc`, `driver:flightsql`, `driver:julia`, `driver:perl`,
`driver:r2dbc`, `driver:cpp`, `driver:dart`, `driver:dotnet`,
`driver:elixir`, `driver:go`, `driver:jdbc`, `driver:mojo`,
`driver:node`, `driver:odbc`, `driver:pascal`, `driver:php`,
`driver:python`, `driver:r`, `driver:ruby`, `driver:rust`, `driver:swift`

All drivers carry `implementation_state: runtime_integrated_with_tests`.

### Adaptors

The following component IDs have `component_category: adaptor`:

`adaptor:scratchbird-airbyte`, `adaptor:scratchbird-dbt-adapter`,
`adaptor:scratchbird-looker`, `adaptor:scratchbird-powerbi`,
`adaptor:scratchbird-tableau`, `adaptor:scratchbird-dbeaver-driver`,
`adaptor:scratchbird-hibernate-dialect`, `adaptor:scratchbird-metabase-driver`,
`adaptor:scratchbird-prisma-adapter`, `adaptor:scratchbird-sqlalchemy-dialect`,
`adaptor:scratchbird-superset-driver`, `adaptor:scratchbird-typeorm-adapter`

### Tool

`tool:cli` — the native command-line interface.

## Capability Fields

Every component entry declares the following capability fields. The permitted
values for each field are defined by the editor tool protocol schema.

| Field | Driver value | Adaptor value |
|-------|-------------|---------------|
| `local_parse` | `common_resource_pack_required` | `delegates_to_common_resource_pack_consumer` |
| `draft_sblr` | `local_draft_allowed_server_revalidated` | `delegates_to_driver_local_draft_server_revalidated` |
| `completion` | `common_protocol_required` | `delegates_to_common_protocol_consumer` |
| `diagnostics` | `canonical_message_vector_keys_required` | `delegates_to_canonical_message_vector_consumer` |
| `canonical_preview` | `required` | `delegates_to_common_protocol_consumer` |
| `renderer` | `preferred_language_then_canonical_english` | `delegates_to_common_renderer_consumer` |
| `renderer_lossiness` | `classified_required` | `delegates_to_common_renderer_consumer` |
| `predictive` | `resource_bounded_no_hidden_objects` | `delegates_to_common_protocol_consumer` |
| `standard_english_fallback` | `enabled_only_when_preferred_profile_fails` | `delegates_to_common_parser_consumer` |
| `offline_cache` | `signed_hash_epoch_scoped` | `delegates_to_common_resource_cache` |
| `redaction_metadata` | `required_no_query_text_or_hidden_identifiers` | `delegates_to_common_protocol_consumer` |
| `capability_negotiation` | `exact_resource_identity_required` | `delegates_to_common_negotiation_consumer` |
| `fail_closed_on_mismatch` | `true` | `true` |
| `server_revalidation_authority` | `required` | `required` |
| `authority_boundary` | `client_resources_are_untrusted_until_server_revalidation` | `client_resources_are_untrusted_until_server_revalidation` |
| `resource_identity` | `sbsql.common_resource_pack.v1` | `sbsql.common_resource_pack.v1` |

The tool:cli entry matches the driver posture for all fields.

## Capability Negotiation

All direct-implementation components (`component_category: driver` and
`tool:cli`) declare `capability_negotiation: exact_resource_identity_required`.
This means:

- The client must present the exact resource identity (`sbsql.common_resource_pack.v1`)
  during capability negotiation.
- A mismatch between the client's declared resource identity and the server's
  admitted resource identity fails closed (`fail_closed_on_mismatch: true`).

Adaptor components delegate negotiation (`delegates_to_common_negotiation_consumer`)
to the underlying driver they consume.

## Canonical Preview

Drivers and `tool:cli` declare `canonical_preview: required`. Before a locally
drafted statement is submitted to the server, the driver must produce a
canonical preview — a render of the canonical form based on the local resource
pack. This preview is produced by the local renderer and carries a lossiness
classification. It is not authoritative; the server revalidates on submission.

## Local Draft SBLR

Drivers may produce a local draft SBLR from the locally parsed canonical element
stream. The `draft_sblr` field `local_draft_allowed_server_revalidated` means:

- The driver is permitted to produce a draft SBLR locally for responsiveness.
- The draft SBLR is not trusted by the server until revalidation.
- `draft_sblr_is_untrusted_until_server_admission: true` from
  `common_resource_contract` applies.

A local draft SBLR that the server refuses produces
`SBSQL.LANG_RESOURCE.LOCAL_DRAFT_SBLR_REFUSED`.

## Localized Diagnostics

All direct-implementation components declare
`diagnostics: canonical_message_vector_keys_required`. Diagnostic messages
delivered to the application must use canonical message vector keys. Localized
message text is applied by the diagnostics resource for the active locale; keys
are stable across locales and driver versions.

Diagnostic key stability is required because:
- Applications may programmatically inspect diagnostic codes.
- Localized message text may change between pack versions.
- Keys never change once admitted.

## The Editor Tool Protocol Schema

The editor tool protocol schema is located at:

```
project/drivers/language/sbsql_editor_tool_protocol.schema.json
```

Its fields:

| Field | Value |
|-------|-------|
| `schema_version` | `sbsql.editor_tool_protocol.schema.v1` |
| `protocol_version` | `sbsql.editor_tool.v1` |

The schema declares the `required_component_fields` that every component entry
in the language surface manifest must carry:

`component_id`, `component_category`, `resource_identity`,
`syntax_profile_order`, `local_parse`, `draft_sblr`, `completion`,
`diagnostics`, `canonical_preview`, `renderer`, `renderer_lossiness`,
`predictive`, `standard_english_fallback`, `offline_cache`,
`redaction_metadata`, `capability_negotiation`, `fail_closed_on_mismatch`,
`server_revalidation_authority`, `authority_boundary`,
`implementation_state`

The schema specifies the `syntax_profile_order` (four deterministic steps),
`renderer_lossiness_classes` (five classes), `fallback_diagnostics` (two
codes), and `rendering_diagnostics` (three codes) that every conforming
component must declare. These are cross-verified in the validation function
`ValidateEditorToolProtocol` in `language_resource_contract.cpp`.

The `allowed_component_values` section of the schema enumerates the valid
string values for each field. An `unsupported_release_blocked` value is
available for any capability not yet supported; using it in a component means
that capability is explicitly refused at the release boundary.

## Offline Cache

Direct-implementation components declare
`offline_cache: signed_hash_epoch_scoped`. The offline cache is scoped by:
- Signed resource hash — the cache is keyed to a specific signed pack version.
- Epoch — the cache is invalidated when the resource pack epoch changes.

A cache with a hash that does not match the currently admitted resource pack is
refused.

## Redaction Metadata

All components declare `redaction_metadata: required_no_query_text_or_hidden_identifiers`
(or delegate to a consumer that does). This means:

- Diagnostic payloads must not include raw query text.
- Diagnostic payloads must not include hidden object identifiers.
- Telemetry and support bundle exports require explicit redaction evidence.

## Cross-References

- Common resource contract invariants: [overview_and_authority_model.md](#ch-language-support-overview-and-authority-model-md)
- Rendering lossiness classes: [rendering_and_fallback.md](#ch-language-support-rendering-and-fallback-md)
- Diagnostic codes: [diagnostics_reference.md](#ch-language-support-diagnostics-reference-md)
- Client and Driver Guide: ../Client_Driver_Guide/README.md (ScratchBird — Application Development and Integration, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Language_Support/diagnostics_reference.md -->

<a id="ch-language-support-diagnostics-reference-md"></a>

# Diagnostics Reference

## Purpose

This page is a consolidated reference for the `SBSQL.LANG_RESOURCE.*` and
related language-resource diagnostic codes emitted by the ScratchBird parser
and renderer. All codes are verified against
`project/src/parsers/sbsql_worker/resources/language_resource_contract.cpp`
and `project/drivers/language/sbsql_language_surface_manifest.json`.

Codes are organized by functional area. For the rendering-layer diagnostics
that are formally declared in `common_resource_pack_metadata`, see the
[Rendering and Fallback](#ch-language-support-rendering-and-fallback-md) page for contextual detail.

## Session and Parse-Profile Diagnostics

These diagnostics relate to which parse profile was selected for a session or
statement.

| Code | Severity | Emitted when | Guidance |
|------|----------|-------------|---------|
| `SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH` | warning | Preferred-language renderer unavailable; canonical English renderer selected | Verify the preferred locale resource pack is correctly installed and admitted. The session language setting is preserved. |
| `SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH` | error | No parse succeeded with any profile; operation refused | Check that the active language resource pack matches the statement's locale. Confirm the pack is signed and admitted. |

## Rendering Diagnostics

These diagnostics are emitted per render operation (SBLR to SBsql text).

| Code | Severity | Emitted when | Guidance |
|------|----------|-------------|---------|
| `SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED` | info | Any render decision is made | Inspect the `renderer_lossiness` field of the diagnostic for the specific lossiness class (`lossless_canonical`, `canonical_equivalent`, `preferred_language_partial`, `canonical_english_fallback`, `not_renderable`). |
| `SBSQL.LANG_RESOURCE.RENDERER_SOURCE_RECONSTRUCTION_FORBIDDEN` | error | A render request with `source_reconstruction_requested: true` was received | Source reconstruction from SBLR is not permitted. The renderer produces canonical output, not original source text. |
| `SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE` | error | No admitted renderer (preferred or English fallback) is available | Install the required language resource pack or allow canonical English fallback. |
| `SBSQL.LANG_RESOURCE.MISSING_CANONICAL_AUTHORITY` | error | SBLR UUID authority cannot be validated | The SBLR presented for rendering does not have valid UUID authority. The server must admit the SBLR before it can be rendered. |

## Resource Lifecycle and Admission Diagnostics

These diagnostics relate to the lifecycle state of a language resource or bundle.

| Code | Severity | Emitted when | Guidance |
|------|----------|-------------|---------|
| `SBSQL.LANG_RESOURCE.MISSING` | error | Required language resource is unavailable | Install the required language resource pack. |
| `SBSQL.LANG_RESOURCE.UNSIGNED` | error | Language resource has no signature identity | Ensure the resource pack was generated and signed through the official toolchain. |
| `SBSQL.LANG_RESOURCE.SIGNING_KEY_MISSING` | error | Language resource has no signing key identity | The signing key identity is required in the manifest. |
| `SBSQL.LANG_RESOURCE.REVOKED` | error | Language resource or bundle has been revoked | Remove the revoked pack. Do not attempt to re-admit it. Obtain a replacement pack if needed. |
| `SBSQL.LANG_RESOURCE.EXPIRED` | error | Language resource has expired | Obtain and install a non-expired pack. |
| `SBSQL.LANG_RESOURCE.INCOMPATIBLE` | error | Language resource is incompatible with this parser version | Check parser version compatibility ranges in the bundle manifest. |
| `SBSQL.LANG_RESOURCE.UNSUPPORTED_CHANNEL` | error | Language resource channel is unsupported | Only `experimental`, `preview`, `beta`, `release_supported`, and `deprecated` channels are loadable. |
| `SBSQL.LANG_RESOURCE.AMBIGUOUS_FALLBACK` | error | Language resource fallback chain is ambiguous | Each profile may have at most one fallback parent; circular or multi-path fallback chains are refused. |
| `SBSQL.LANG_RESOURCE.REMOVED` | error | Language resource has been removed | The resource is permanently unavailable. Obtain a replacement. |
| `SBSQL.LANG_RESOURCE.TOPOLOGY_DIALECT_UNICODE_UNSUPPORTED` | error | Language resource topology or dialect Unicode profile is unsupported | Check topology and dialect compatibility with the parser version. |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_RESOURCE_REFUSED` | error | Predictive language resource was refused | Check predictive resource limits and admission criteria. |
| `SBSQL.LANG_RESOURCE.LOCAL_DRAFT_SBLR_REFUSED` | error | Local draft SBLR is untrusted and was refused by the server | The server revalidation of a client-produced draft SBLR failed. Submit for server revalidation and resolve any admission errors. |

## Lifecycle Channel Diagnostics

These diagnostics are warnings emitted when a resource with a non-release
channel is loaded. They do not prevent loading but indicate support limitations.

| Code | Severity | Emitted when | Guidance |
|------|----------|-------------|---------|
| `SBSQL.LANG_RESOURCE.EXPERIMENTAL_UNSUPPORTED` | warning | An experimental language resource is admitted | Experimental resources have no support commitment. Do not use in production. |
| `SBSQL.LANG_RESOURCE.PREVIEW_LIMITED_SUPPORT` | warning | A preview language resource is admitted | Preview resources require native review and are not release-supported. |
| `SBSQL.LANG_RESOURCE.BETA_LIMITED_SUPPORT` | warning | A beta language resource is admitted | Beta resources are machine-generated and require native review before release support. |
| `SBSQL.LANG_RESOURCE.RELEASE_SUPPORTED` | info | A release-supported language resource is admitted | Informational — the admitted resource meets release support criteria. |
| `SBSQL.LANG_RESOURCE.DEPRECATED` | warning | A deprecated language resource is admitted | The resource is still usable but has been deprecated. Plan migration to a supported replacement. |

## Validation Error Diagnostics

These error codes are produced at resource manifest validation time, not at
runtime. They appear in operator-facing admission logs.

| Code | Meaning |
|------|---------|
| `SBSQL.LANG_RESOURCE.PROFILE_UUID_MISSING` | Profile UUID not present in manifest |
| `SBSQL.LANG_RESOURCE.EXACT_TAG_MISSING` | Exact language tag not present |
| `SBSQL.LANG_RESOURCE.COMMON_HASH_MISSING` | Common resource hash not present |
| `SBSQL.LANG_RESOURCE.SURFACE_REGISTRY_HASH_MISSING` | Canonical surface registry hash not present |
| `SBSQL.LANG_RESOURCE.SBLR_REGISTRY_HASH_MISSING` | SBLR registry hash not present |
| `SBSQL.LANG_RESOURCE.SUPPORT_STATE_MISMATCH` | Release-supported channel requires release-supported support state |
| `SBSQL.LANG_RESOURCE.GOVERNANCE_EVIDENCE_MISSING` | Release resource missing full governance evidence |
| `SBSQL.LANG_RESOURCE.AUTHOR_MISSING` | Author identity required for release resources |
| `SBSQL.LANG_RESOURCE.REVIEWER_MISSING` | Reviewer identity required for release resources |
| `SBSQL.LANG_RESOURCE.NATIVE_TECHNICAL_REVIEWER_MISSING` | Native technical reviewer required for release resources |
| `SBSQL.LANG_RESOURCE.SECURITY_REVIEWER_MISSING` | Security reviewer required for release resources |
| `SBSQL.LANG_RESOURCE.SUPPORT_OWNER_MISSING` | Support owner required for release resources |
| `SBSQL.LANG_RESOURCE.RELEASE_APPROVAL_MISSING` | Release approval evidence required |
| `SBSQL.LANG_RESOURCE.REVOCATION_POLICY_MISSING` | Revocation policy evidence required |
| `SBSQL.LANG_RESOURCE.CONTRIBUTION_PROVENANCE_MISSING` | Contribution provenance evidence required |
| `SBSQL.LANG_RESOURCE.DEPRECATION_NOTICE_MISSING` | Deprecated resources require deprecation notice evidence |
| `SBSQL.LANG_RESOURCE.REVOCATION_NOTICE_MISSING` | Revoked resources require revocation notice evidence |
| `SBSQL.LANG_RESOURCE.REMOVAL_NOTICE_MISSING` | Removed resources require removal notice evidence |
| `SBSQL.LANG_RESOURCE.CYCLIC_FALLBACK_PARENT` | Fallback parent points to the same profile |
| `SBSQL.LANG_RESOURCE.RENDERER_RECURSION` | Renderer edge points to the same profile |
| `SBSQL.LANG_RESOURCE.DUPLICATE_CANONICAL_ID` | Canonical IDs must be unique |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_LIMIT_EXCEEDED` | Predictive resources exceed release safety limits |
| `SBSQL.LANG_RESOURCE.PROVENANCE_MISSING` | Release resources require provenance rows |
| `SBSQL.LANG_RESOURCE.PROVENANCE_INCOMPLETE` | Provenance rows require all required fields |
| `SBSQL.LANG_RESOURCE.REDISTRIBUTION_NOT_ALLOWED` | Release resource data must be redistributable |
| `SBSQL.LANG_RESOURCE.RECOVERY_PROFILE_REPLACEABLE` | Built-in recovery profile cannot be externally replaceable |
| `SBSQL.LANG_RESOURCE.EXPERIMENTAL_SUPPORT_STATE_MISMATCH` | Experimental resources must not claim reviewed or release support state |
| `SBSQL.LANG_RESOURCE.PREVIEW_REVIEW_REQUIRED` | Preview resources require native review |
| `SBSQL.LANG_RESOURCE.BETA_REVIEW_REQUIRED` | Beta resources require native review |
| `SBSQL.LANG_RESOURCE.DEPRECATED_SUPPORT_STATE_MISMATCH` | Deprecated resources must retain release-supported support state |
| `SBSQL.LANG_RESOURCE.DEPRECATED_GOVERNANCE_EVIDENCE_MISSING` | Deprecated resources require release governance evidence |

## Predictive Resource Validation Diagnostics

| Code | Meaning |
|------|---------|
| `SBSQL.LANG_RESOURCE.PREDICTIVE_RESOURCE_IDENTITY_MISSING` | Predictive resource identity is required |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_TABLE_SIZE_LIMIT` | Predictive table exceeds 8 MB limit |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_FANOUT_LIMIT` | Transition fanout exceeds 1024 limit |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_COMPLETION_LIMIT` | Completion result count exceeds 4096 limit |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_TIME_LIMIT` | Generation time exceeds 1000 ms limit |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_MEMORY_LIMIT` | Predictive memory exceeds 16 MB limit |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_DEPTH_LIMIT` | Nested expansion depth exceeds 64 limit |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_DETERMINISM_REQUIRED` | Limits must be enforced deterministically |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_NO_DISCLOSURE_REQUIRED` | Completions must not disclose hidden objects |

## Bundle Admission Diagnostics

These codes are on the `SBSQL.LANG_BUNDLE.*` prefix and relate to bundle
(pack-level) rather than profile-level validation.

| Code | Meaning |
|------|---------|
| `SBSQL.LANG_BUNDLE.UNSIGNED` | Bundle must be signed unless it is a parser language library |
| `SBSQL.LANG_BUNDLE.REVOKED` | Revoked bundle fails closed |
| `SBSQL.LANG_BUNDLE.EXPIRED` | Expired bundle fails closed |
| `SBSQL.LANG_BUNDLE.REMOVED` | Removed bundle fails closed |
| `SBSQL.LANG_BUNDLE.ROLLED_BACK` | Rolled-back bundle admitted with warning |
| `SBSQL.LANG_BUNDLE.INCOMPATIBLE` | Bundle is not compatible with this parser version |
| `SBSQL.LANG_BUNDLE.SECURITY_ADMISSION_REQUIRED` | Bundle requires security admission before use |
| `SBSQL.LANG_BUNDLE.ACTIVE_PROFILE_IN_USE` | Active language profiles cannot be unloaded |
| `SBSQL.LANG_BUNDLE.REQUIRED_PROFILE` | Required language profiles cannot be unloaded |

## Canonical Stream Diagnostics

These codes relate to canonical element stream validation.

| Code | Meaning |
|------|---------|
| `SBSQL.CANONICAL_STREAM.RESOURCE_IDENTITY_MISSING` | Stream requires a resource identity |
| `SBSQL.CANONICAL_STREAM.LANGUAGE_PROFILE_MISSING` | Stream requires a language profile UUID |
| `SBSQL.CANONICAL_STREAM.EXACT_TAG_MISSING` | Stream requires an exact language tag |
| `SBSQL.CANONICAL_STREAM.DIALECT_PROFILE_MISSING` | Stream requires a dialect profile UUID |
| `SBSQL.CANONICAL_STREAM.TOPOLOGY_PROFILE_MISSING` | Stream requires a topology profile UUID |
| `SBSQL.CANONICAL_STREAM.COMMON_HASH_MISSING` | Stream requires the common resource pack hash |
| `SBSQL.CANONICAL_STREAM.SOURCE_HASH_MISSING` | Stream requires the localized source hash |
| `SBSQL.CANONICAL_STREAM.CANONICAL_ORDER_MISSING` | Stream requires a canonical order identifier |
| `SBSQL.CANONICAL_STREAM.POST_UUID_NORMALIZATION` | Topology normalization must occur before UUID resolution |
| `SBSQL.CANONICAL_STREAM.SERVER_REVALIDATION_REQUIRED` | Stream remains untrusted until server revalidation |
| `SBSQL.CANONICAL_STREAM.EMPTY` | Stream must contain at least one element |
| `SBSQL.CANONICAL_STREAM.ELEMENT_CANONICAL_TEXT_MISSING` | Each element requires canonical text |
| `SBSQL.CANONICAL_STREAM.ELEMENT_CANONICAL_ID_MISSING` | Each element requires a canonical ID |
| `SBSQL.CANONICAL_STREAM.ELEMENT_SOURCE_HASH_MISSING` | Each element must retain a localized source hash |
| `SBSQL.CANONICAL_STREAM.ELEMENT_SOURCE_SPAN_MISSING` | Each element must retain its localized source span |

## Parse Profile Validation

| Code | Meaning |
|------|---------|
| `SBSQL.PARSE_PROFILE.ORDER_UNSUPPORTED` | Parse profile order must be exactly: explicit profile, preferred language, canonical English fallback, fail closed |

## Editor Protocol Validation

| Code | Meaning |
|------|---------|
| `SBSQL.EDITOR_PROTOCOL.VERSION_UNSUPPORTED` | Unsupported editor protocol version (must be `sbsql.editor_tool.v1`) |
| `SBSQL.EDITOR_PROTOCOL.RESOURCE_IDENTITY_MISSING` | Resource identity is required |
| `SBSQL.EDITOR_PROTOCOL.REQUIRED_FEATURE_MISSING` | Common editor tool protocol must expose all required surfaces |
| `SBSQL.EDITOR_PROTOCOL.PARSE_PROFILE_ORDER_UNSUPPORTED` | Protocol must declare deterministic parse profile order |
| `SBSQL.EDITOR_PROTOCOL.RENDERER_LOSSINESS_CLASSES_MISSING` | Protocol must declare all renderer lossiness classes |
| `SBSQL.EDITOR_PROTOCOL.FALLBACK_DIAGNOSTICS_MISSING` | Protocol must declare canonical English fallback diagnostics |
| `SBSQL.EDITOR_PROTOCOL.RENDERING_DIAGNOSTICS_MISSING` | Protocol must declare renderer classification diagnostics |
| `SBSQL.EDITOR_PROTOCOL.AUTHORITY_METADATA_MISSING` | Protocol must fail closed and keep server revalidation authority |
| `SBSQL.EDITOR_PROTOCOL.AUTHORITY_BOUNDARY_UNSUPPORTED` | Protocol must preserve client-resource authority boundary |

## Cross-References

- Rendering diagnostic context: [rendering_and_fallback.md](#ch-language-support-rendering-and-fallback-md)
- Server parser validation context: [server_parser_language_support.md](#ch-language-support-server-parser-language-support-md)
- Client authority boundary: [client_and_editor_language_surface.md](#ch-language-support-client-and-editor-language-surface-md)
- Authority model: [overview_and_authority_model.md](#ch-language-support-overview-and-authority-model-md)




<a id="ch-glossary"></a>

# Glossary

## Purpose

This glossary defines terms used across the ScratchBird documentation set. The
definitions are written for end users, evaluators, operators, and developers.
They are intentionally concise and cautious: a term appearing here does not mean
the related feature is complete, enabled, or available in every build.

## ScratchBird Product Names

| Term | Meaning |
| --- | --- |
| ScratchBird | The project and product line described by this documentation. |
| ScratchBird Convergent Data Engine | The full product concept: engine, parsers, tools, resources, and operational surfaces. |
| SB | Short brand form used in names and examples. |
| SBcore | ScratchBird Engine. The embedded engine library that owns durable catalog identity, transactions, storage, security admission, recovery decisions, and engine diagnostics. |
| SBsql | ScratchBird SQL. The native ScratchBird command language and script runner surface. |
| SBParser | ScratchBird Core Parser. The native SBsql parser package that lowers SBsql requests to SBLR. |
| SBsrv | ScratchBird IPC Server. A local multi-user server process for same-machine clients. |
| SBgate | ScratchBird Listener. The listener and parser-facing entry point used for network-facing client traffic. |
| SBmgr | ScratchBird Single Node Manager. A single-node front door that can proxy authenticated connections to internal listener routes in managed deployments. |
| SBadm | ScratchBird Administrator. Administrative utility name for configuration, time zone, character set, collation, and policy management where present. |
| SBbak | ScratchBird Backup Manager. Utility name for backup and backup-set operations where present. |
| SBsec | ScratchBird Security. Utility name for security provider, user, role, group, and policy management where present. |
| SBdoc | ScratchBird Doctor. Utility name for analysis, diagnosis, and repair-oriented workflows where present and admitted. |
| SBcop | ScratchBird Conformance Officer. Utility name for conformance and comparison checks where present. |

## Architecture Terms

| Term | Meaning |
| --- | --- |
| Convergent Data Engine | An engine design that attempts to bring multiple data shapes, parser surfaces, transaction rules, security rules, and diagnostics under one shared engine authority model. |
| CDE | Abbreviation for Convergent Data Engine. |
| Engine authority | The rule that durable behavior belongs to SBcore: object identity, descriptors, transactions, security admission, storage, recovery, and diagnostics. |
| Parser boundary | The separation between a client language or wire protocol and engine execution authority. |
| Parser package | A component that accepts a specific language or protocol surface and lowers accepted work to ScratchBird execution requests. |
| Compatibility parser | A standalone parser package for one reference-system client family. It should not silently accept unrelated dialects. |
| SBsql language profile | A parser resource profile that can change user-facing SBsql spellings, phrase order, diagnostics, completion hints, and source rendering without changing SBLR, UUID identity, descriptors, security, storage, or MGA transaction authority. |
| Canonical element stream | The normalized parser output created before UUID binding. It records canonical token and surface identities rather than treating localized words as engine authority. |
| Standard SBsql fallback | A policy-controlled input fallback that lets a non-English session accept canonical English SBsql when the preferred language profile does not parse the statement. |
| Parser route | The configured path that determines which parser handles a client request. |
| SBLR | ScratchBird's bound engine-facing request representation. Parsers emit SBLR after parsing and binding accepted work. |
| Bound request | A structured request whose names, values, parameters, and types have been resolved enough to submit toward engine authority. |
| Raw text | The command text received from a client before parsing. Raw text is not durable engine authority. |
| Catalog projection | A view or metadata surface that presents engine catalog information in a particular shape for a parser, tool, or user. |
| Workarea | A schema-root area presented to a parser or user as its operating root. |
| Compatibility surface | The subset of behavior a parser or tool is designed and proven to accept, execute, or refuse clearly. |
| Refusal | A controlled response that says a request is unsupported, denied, unavailable, unsafe, or otherwise not admitted. |

## Database And Catalog Terms

| Term | Meaning |
| --- | --- |
| Database | A managed durable store of data, metadata, identity, transactions, security rules, diagnostics, and recovery behavior. |
| Metadata | Information that describes data, such as schemas, tables, columns, types, constraints, indexes, views, routines, grants, policies, and catalog rows. |
| Catalog | Engine-owned metadata that describes durable database objects and their relationships. |
| Catalog identity | The durable identity of a catalog object, separate from the user-facing name used to spell it. |
| UUID identity | Durable object identity based on UUIDs rather than only text names. |
| Object descriptor | Engine metadata describing an object shape, type, storage behavior, dependency, or operational capability. |
| Type descriptor | Metadata describing a datatype, its value behavior, binary representation, capabilities, and related rules. |
| Domain | A reusable constrained type definition. |
| Constraint | A rule attached to a table, column, domain, or related object. |
| Index | A search structure maintained for faster lookup, ordering, constraint enforcement, or query planning where implemented. |
| View | A named query projection. |
| Materialized view | A stored projection whose refresh and dependency behavior must be defined by the implementation. |
| Procedure | A stored routine that can perform controlled work and may return output parameters or result sets where supported. |
| Function | A routine that returns a value or result. |
| Package | A named grouping of routine definitions where supported. |
| Trigger | Routine behavior tied to table, database, transaction, or event-style actions where implemented. |
| Sequence | A database object that generates ordered values according to its definition. |
| Comment | Descriptive metadata attached to an object. Comments do not grant authority and should not contain secrets. |

## Schema And Name Terms

| Term | Meaning |
| --- | --- |
| Schema | A namespace branch that can contain objects and, where supported, child schemas. |
| Recursive schema tree | A schema model where schemas can contain child schemas, creating a tree rather than one flat namespace. |
| Database root | The top of the durable database tree. Not every session can see it directly. |
| Parser-visible root | The root of the namespace presented to the selected parser route. |
| Home schema | The schema associated with a user, identity, or configured workarea. |
| Current schema | The default schema used for unqualified names in a session. |
| Search path | An ordered lookup path used by commands that allow path-based name resolution. |
| Qualified name | A name that includes schema or path information, such as `app.notes`. |
| Unqualified name | A name without schema qualification, such as `notes`. |
| Name resolution | The process of turning a user-visible name into engine object identity. |
| Sandbox | The visible boundary that limits what a session or parser route can name, inspect, or access. |
| Schema branch | A subtree of the database namespace. |
| Object lifecycle | The create, alter, rename, comment, describe, use, refresh, validate, or drop actions that apply to an object type. |

## Transaction And Recovery Terms

| Term | Meaning |
| --- | --- |
| Transaction | A boundary around work that can commit, roll back, and participate in visibility rules. |
| Commit | Make a transaction's admitted changes final according to engine visibility rules. |
| Rollback | Discard uncommitted transaction changes. |
| Savepoint | A named point inside a transaction that can be rolled back without ending the whole transaction where supported. |
| Autocommit | A mode where each statement may be committed automatically according to session and parser rules. |
| MGA | ScratchBird's transaction and visibility authority model. In this documentation, the key rule is that transaction finality belongs to the engine. |
| Visibility | The rule that determines which transaction versions a session can see. |
| Cleanup | Engine-controlled work that reclaims or resolves old transaction state when it is safe. |
| Recovery | The process of reopening or refusing a database after shutdown, interruption, or uncertain durable state. |
| Recovery-required state | A state where the engine requires recovery handling before normal writes can proceed. |
| Fail closed | Refuse work when the safe outcome is uncertain instead of silently accepting it. |
| Reopen proof | A test that closes and reopens a database to verify committed state is still present. |

## Security Terms

| Term | Meaning |
| --- | --- |
| Identity | The authenticated user, service, or agent identity attached to a session or operation. |
| Principal | A user, role, group, service, or other authority-bearing identity. |
| Authentication | Establishing who the session or agent is. |
| Authorization | Deciding what an authenticated identity is allowed to do. |
| Grant | A permission given to a principal or object. |
| Revoke | Removal of a previously granted permission. |
| Role | A named set of privileges that can be granted and activated according to policy. |
| Policy | A rule that controls access, masking, row visibility, external access, operational admission, or protected material use. |
| Row-level security | Policy behavior that limits which rows a session can see or change. |
| Mask | A policy-controlled transformation that hides or changes protected values in query output. |
| Protected material | Secrets or sensitive values that require controlled storage, reference, redaction, and use. |
| Secret reference | A reference to protected material without placing the raw secret in a parser packet, script, or diagnostic. |
| Materialized authorization | Authorization information loaded into an engine-admissible form before work is executed. |
| Denied | A refusal because the authenticated identity, policy, or sandbox does not admit the operation. |

## Data And Type Terms

| Term | Meaning |
| --- | --- |
| Datatype | A named value category with storage, comparison, conversion, and validation behavior. |
| Scalar value | A single value such as an integer, timestamp, boolean, UUID, or text value. |
| Numeric type | A datatype for integer, unsigned integer, decimal, fixed-point, or floating-point values. |
| Text type | A datatype for character data governed by character set and collation rules. |
| Character set | The encoding rules for text values. |
| Collation | The comparison and ordering rules for text values. |
| Temporal type | A datatype for dates, times, timestamps, intervals, or time-zone-aware values. |
| UUID | A fixed-size identifier value commonly used for durable identity. |
| Binary value | A sequence of bytes with type-specific interpretation. |
| Protected value | A value governed by protected-material policy. |
| Document value | A structured value such as JSON-like data where implemented. |
| Graph value | A relationship-oriented value or model surface where implemented. |
| Vector value | A numeric vector used for similarity or embedding-style operations where implemented. |
| Time-series value | A value or record organized around time-oriented measurement behavior where implemented. |
| Coercion | An implicit or explicit conversion between compatible types. |
| Cast | An explicit type conversion requested by the user. |
| Null | A marker for absence of a value, distinct from zero, empty string, or false. |

## Query Terms

| Term | Meaning |
| --- | --- |
| DDL | Data definition language: commands that create, alter, describe, comment on, rename, or drop database objects. |
| DML | Data manipulation language: commands that read or change rows and values. |
| Query | A request that reads data and returns a result set or scalar result. |
| Result set | Rows and columns returned by a query or routine. |
| Projection | The selected output columns or expressions of a query. |
| Predicate | A condition used to filter rows or control logic. |
| Join | A query operation that combines rows from more than one source. |
| Grouping | A query operation that forms groups of rows for aggregate calculations. |
| Aggregate | A calculation over multiple rows, such as count or sum where implemented. |
| Window function | A calculation over a window of rows related to the current row where implemented. |
| CTE | Common table expression. A named temporary query expression inside a statement. |
| Recursive CTE | A CTE that refers to itself according to the rules of the language surface. |
| Ordering | The explicit sort order requested for result rows. |
| Limit | A request to return only a bounded number of rows. |
| Offset | A request to skip a number of rows before returning results. |
| Upsert | Insert-or-update behavior according to a conflict rule where supported. |
| Merge | A statement that conditionally inserts, updates, or deletes based on a source relation where supported. |
| Copy | A large or streaming data input or output surface where implemented and admitted. |

## Procedural Terms

| Term | Meaning |
| --- | --- |
| Procedural SQL | Stored routine language constructs such as blocks, variables, control flow, cursors, exceptions, and triggers where implemented. |
| Block | A procedural unit containing declarations and executable statements. |
| Variable | A named procedural value local to a routine or block. |
| Cursor | A controlled handle over a result set. |
| Result-set cursor | A cursor passed or returned as a routine-controlled result where supported. |
| Exception handler | Procedural logic that handles a diagnostic or error condition. |
| Event trigger | Trigger-style behavior tied to database, transaction, or event actions where implemented. |
| UDR | User-defined routine or parser-support routine package, depending on context. In parser documentation, it commonly means the package that supports bridge or extension behavior for that parser. |
| Bridge | A controlled connection or interface used by a parser-support routine to reach another database surface where configured and admitted. |

## Operations And Data Movement Terms

| Term | Meaning |
| --- | --- |
| Configuration | Settings that control startup, resource locations, parser registration, security providers, policy defaults, and runtime behavior. |
| Resource file | A staged file needed by the product, such as character set, collation, time zone, policy, or configuration data. |
| Health check | A diagnostic check that reports whether a component appears alive and able to answer. |
| Readiness check | A diagnostic check that reports whether a component is ready to accept intended work. |
| Liveness check | A diagnostic check that reports whether a component is still running. |
| Support bundle | A redacted package of diagnostic evidence for review or support. |
| Redaction | Removing or masking protected material before diagnostics are shown or bundled. |
| Message vector | Structured diagnostic output used for errors, refusals, and operational status. |
| Logical stream | Data movement represented as statements, rows, records, or events rather than physical page files. |
| Logical backup | A backup stream that represents database content as logical metadata and data operations. |
| Logical restore | Replaying a logical stream as admitted database operations. |
| Physical backup | A page-copy or file-copy backup shape. Compatibility parser routes should not treat physical page-copy formats as normal logical restore input. |
| Import | Bring external logical data into a database through an admitted parser or tool route. |
| Export | Write logical data from a database to an external stream or file according to policy. |
| CDC | Change data capture. A stream or record of changes suitable for replication, ETL, or integration where implemented. |
| Replication | Copying changes between systems according to an ordering and identity model where implemented. |
| ETL | Extract, transform, load. A data movement workflow that reads from one source, transforms, and writes to another target. |
| Migration | Moving schema, data, routines, security, or operational behavior from one database shape to another. |
| Quarantine | Holding questionable incoming records or events aside for review instead of applying them silently. |
| Cutover | The controlled switch from one active source or route to another. |
| Idempotency key | A value used to detect repeated events or operations so replay can be handled safely. |

## Build And Release Terms

| Term | Meaning |
| --- | --- |
| Build output | The generated binaries, libraries, parser packages, resources, and configuration artifacts for a target platform. |
| Output tree | The staged directory layout intended for testing or release packaging. |
| Target platform | The operating system and architecture being built or tested. |
| Proof gate | A test or validation step intended to prove that a behavior remains implemented and has not regressed. |
| CTest | The test runner integration used by many project tests. |
| Conformance test | A test that compares behavior against a declared specification, parser expectation, or compatibility target. |
| Smoke test | A small test proving that a basic workflow starts, runs, and stops. |
| Regression test | A test intended to prevent a previously handled behavior from breaking again. |
| Draft documentation | Documentation under active review. Draft status means users should verify commands and claims against the current build and tests. |



# About This Documentation

This book is part of the ScratchBird documentation set. ScratchBird is a
Convergent Data Engine (CDE).

**Draft status.** This is draft documentation. It describes the architecture and
intended behavior of the source tree. A topic appearing here does not by itself
guarantee that a feature is complete, enabled, performant, certified, or
available in any particular build. Always confirm against the current build,
configuration, tests, and release notes.

**License.** The ScratchBird engine is distributed under the Mozilla Public
License 2.0 (MPL-2.0). This documentation describes that open-source engine.

**No certification claim.** Nothing in this documentation constitutes a security
certification, performance benchmark, or compatibility guarantee.

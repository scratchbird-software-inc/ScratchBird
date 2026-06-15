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

![diagram](./parser_to_sblr_pipeline-1.svg)

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

- [Intro And MGA](intro_and_mga.md)
- [SBsql Language Profiles](sbsql_language_profiles.md)
- [UUID Catalog Identity](uuid_catalog_identity.md)
- [Transactions And Recovery](transactions_and_recovery.md)
- [Security And Sandboxing](security_and_sandboxing.md)
- [Script Tokens And Identifiers](../syntax_reference/script_tokens_and_identifiers.md)
- [Schema Tree And Name Resolution](../syntax_reference/schema_tree_and_name_resolution.md)
- [Refusal Vectors](../syntax_reference/refusal_vectors.md)
- [Type System Overview](../data_types/type_system_overview.md)
- [Conversion Matrix](../data_types/conversion_matrix.md)
- [Projection](../syntax_reference/projection.md)
- [Transaction Control](../syntax_reference/transaction_control.md)

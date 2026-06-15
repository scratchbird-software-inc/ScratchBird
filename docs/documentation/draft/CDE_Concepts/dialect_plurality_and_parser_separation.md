# Dialect Plurality And Parser Separation

## Purpose

This page explains how ScratchBird accepts requests from many different client
languages and wire protocols, how those requests are lowered to a single engine
representation, and why the strict separation between parser and engine is a
security and correctness requirement rather than a convenience.

For the full pipeline detail, see
[../Language_Reference/core_paradigms/parser_to_sblr_pipeline.md](../Language_Reference/core_paradigms/parser_to_sblr_pipeline.md)
and
[../Getting_Started/architecture/sbsql_and_sblr.md](../Getting_Started/architecture/sbsql_and_sblr.md).
For the parser/engine boundary and sandbox model, see
[../Getting_Started/architecture/engine_parser_boundary.md](../Getting_Started/architecture/engine_parser_boundary.md).
For reference system compatibility scope, see
[../Getting_Started/using_scratchbird/reference_system_compatibility.md](../Getting_Started/using_scratchbird/reference_system_compatibility.md).

**This is a draft.** No claim here is a compatibility completeness assertion.

---

## Many Dialects, One Engine

A conventional database engine speaks one language — usually the one it was
designed around. Compatibility with other languages requires either a separate
gateway product, a view layer, or direct port of client code.

ScratchBird takes a different approach: the engine defines a single internal
representation of bound operations (SBLR), and many different client languages
and wire protocols are each supported by a separate *parser package* that
translates that dialect's input into SBLR. The engine itself never sees raw
client text. It only sees SBLR.

The compatibility parser packages in the source tree cover multiple categories
of client interface, including:

- A widely-used relational dialect and its compatible variant
- A distributed relational dialect
- Key-value store wire protocols
- Document store wire protocols and query languages
- Immutable and append-only store protocols
- Distributed SQL dialects
- Full-text and analytics search query languages
- Vector database query protocols
- Graph database query languages
- Distributed transactional key-value store protocols
- Columnar and analytical SQL dialects
- Event-sourced database query protocols

These are represented in the source tree under `src/parsers/compatibility/` with
separate subdirectories per compatibility family. The engine's own native language
is SBsql, compiled by the native parser package under `src/parsers/native/` and
`src/parsers/sbsql_worker/`.

---

## Parser Packages Are Untrusted External Processes

Parser packages do not run inside the engine process with engine authority.
They run as separate untrusted processes that communicate with the engine
through a structured protocol. A parser package can:

- Receive client text and wire protocol input.
- Parse that input and produce an SBLR envelope.
- Return the SBLR envelope to the engine.

A parser package cannot:

- Claim transaction finality or visibility authority.
- Bypass security policy evaluation.
- Assert that a result is correct without the engine's MGA recheck.
- Access storage directly.

This separation is enforced at the process boundary and in the SBLR authority
model. The `RefuseParserAuthorityAttempt` function in
`src/engine/sblr/sblr_parser_authority_guards.hpp` is the engine-side
enforcement point: any attempt by a parser to claim engine authority is refused
with a diagnostic, not silently accepted.

In the `EngineNoSqlMgaRecheckProof`, the field
`parser_claims_transaction_finality_authority` defaults to `false` and any
true value triggers `SB_NOSQL_PROVIDER_PARSER_FINALITY_AUTHORITY_REFUSED`.
This applies to every parser, including the native SBsql parser.

---

## SBsql: The Native Language

SBsql is ScratchBird's own query and data-definition language. It is the most
complete interface to the engine: all engine capabilities are expressible in
SBsql. Compatibility dialects cover the subset of operations that can be
safely and correctly translated.

SBsql compiles to SBLR through the native parser. The native parser is also an
untrusted process — it does not receive more authority than a compatibility
parser. The difference is scope: SBsql can express all engine operations,
while compatibility surfaces cover a scoped subset.

For SBsql syntax and language profiles, see the
[Language Reference](../Language_Reference/README.md).

---

## SBLR: The Bound Internal Representation

SBLR (ScratchBird Low-level Representation) is the single format in which all
operations enter the engine from parsers. It is a structured envelope that carries:

- The operation identity (operation id).
- Operands with type, name, and value.
- Source artifact metadata (for error reporting and diagnostics) — which is
  explicitly marked as render metadata only, not as authority:
  `raw_sql_text_authoritative = false` in `SblrSourceArtifactMap`.
- The engine ABI and SBLR version, allowing the engine to validate that the
  envelope was produced for the current engine.

SBLR is defined as an internal API. There is no public SBLR serialization format
intended for external production use. Client applications interact through
SBsql or a supported compatibility dialect, not through SBLR directly.

### SQL Text Is Not Authority

The most important property of SBLR is that it separates the SQL text (which is
client input and cannot be trusted as an authority) from the bound internal
representation (which the engine evaluates). The `SblrSourceArtifactMap` carries
the original source text only for diagnostic rendering:

```
bool contains_sql_text = false;
bool raw_sql_text_authoritative = false;
```

The engine evaluates the SBLR, not the text. Security grants, visibility
decisions, and catalog operations all flow through the SBLR and the engine's
internal authority structures (UUID, MGA, materialized policy), never through
re-parsing or re-evaluating client text at runtime.

---

## Compatibility Scope And Refusal

Because each compatibility parser covers a scoped subset of the target dialect,
there will always be operations that a given compatibility surface does not support.
ScratchBird's policy is explicit refusal with a diagnostic — never silent
substitution, never approximate execution, never a result that violates engine
correctness guarantees.

When a compatibility parser cannot translate an operation — either because the
operation is outside scope, or because the engine does not support the underlying
capability in the current build — it returns a structured diagnostic. The
caller receives an explicit signal, not a wrong answer.

This connects to the parser registration system documented in
[../Operations_Administration/parser_registration_and_routes.md](../Operations_Administration/parser_registration_and_routes.md).
Parser routes are registered and enabled per-database; a parser that is not
registered for a given database is not accessible to connections for that
database.

---

## The Separation In The CDE Design

Parser-engine separation is not merely an implementation detail; it is a
structural property that enables the CDE design:

1. **New dialects do not affect engine authority.** Adding a compatibility
   parser for a new client language does not require changes to the engine's
   security model, transaction model, or storage model. The new parser lowers
   to SBLR; the engine evaluates as before.

2. **Isolation of external code.** Compatibility parsers may incorporate
   third-party parsing libraries or grammar implementations. Running them as
   untrusted external processes means a defect or exploit in a parser cannot
   directly affect engine storage or security.

3. **Consistency of correctness guarantees.** Because all paths through all
   dialects converge on the same SBLR and the same engine evaluation, the
   MGA recheck, security evaluation, and diagnostic behavior are identical
   regardless of which client language was used.

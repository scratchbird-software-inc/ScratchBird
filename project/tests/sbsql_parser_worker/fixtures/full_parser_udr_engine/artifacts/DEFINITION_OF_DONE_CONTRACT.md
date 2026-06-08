# Definition of Done Contract

Status: complete
Search key: `FSPE-DEFINITION-OF-DONE-CONTRACT`
Owning slice: `FSPE-000E`

## Purpose

This artifact defines what `implemented in full` means for the full SBSQL parser, parser-support UDR, SBLR lowering, sb_server IPC route, sb_engine behavior, diagnostics, documentation, and regression assets.

## Mandatory Closure Rules

An item is not complete until all rules below are satisfied.

1. The accepted surface row exists in the canonical SBSQL surface registry with a stable row id, family, profile gate, and fixed UUID rule where applicable.
2. The parser accepts the syntax, rejects invalid syntax with an exact message-vector mapping, and preserves source spans for diagnostics.
3. The parser binds through server-public name resolution only and never treats names as durable authority.
4. The bound operation lowers to canonical SBLR with UUIDs, descriptors, security context references, and result-shape contract metadata.
5. The parser-support UDR implements every required helper for SQL-to-SBLR conversion and never mutates engine state directly.
6. sb_server admits, validates, routes, controls, streams, cancels, and reports the operation over IPC.
7. sb_engine behavior is implemented for every non-cluster operation with no SQL text execution path.
8. Cluster-private operations have exact refusal, policy-blocked, or cluster-required message-vector behavior in standalone builds.
9. Every failure path has a diagnostic/message-vector row and parser rendering rule.
10. Every success path has command completion, row metadata, warning, and result-shape behavior where applicable.
11. Every accepted donor alias maps to the native SBSQL surface or is recorded as policy-blocked or refused with exact rationale.
12. Tests exist for parse, bind, lower, server admission, engine execution, result rendering, restart/persistence where applicable, and negative/security cases.
13. Generated fixtures are reproducible from repository-local inputs without internet access.
14. Source files remain inside the source-size and logical-family gates.
15. No `TODO`, `stub`, `future`, `defer`, `parser-only`, or `spec-only` marker remains for accepted non-cluster behavior.

## Slice-Level Done Rules

Every implementation slice must provide all applicable evidence before the coordinator may mark it `validation_passed`:

| Area | Done requirement |
| --- | --- |
| Parser syntax | Lexer/CST/AST coverage exists for every accepted row assigned to the slice, including invalid-input diagnostics. |
| Binding | Name resolution uses server-public authority only; UUIDs, descriptors, security context, transaction context, and result contracts are present in BoundAST evidence. |
| SBLR lowering | Lowering emits canonical SBLR envelope data and verifier fixtures; invalid/stale/malformed SBLR fails closed. |
| Parser-support UDR | Exported UDR functions validate, parse, describe, normalize, decompile where applicable, and return message vectors under engine-supplied context. |
| Server route | Server admission, revalidation, cancellation, streaming/result metadata where applicable, and diagnostic return paths are tested through IPC. |
| Engine behavior | Non-cluster behavior is implemented through internal API/SBLR execution with no SQL text execution path. |
| Cluster/private behavior | Standalone builds fail closed with exact message vectors; profile-enabled paths require explicit profile fixtures. |
| Diagnostics | Every success, warning, refusal, and failure path has a message-vector row, redaction rule, parser rendering template, and fixture id. |
| Regression assets | Generated fixtures are durable CTest assets with labels, timeouts, expected output, and failure summaries. |
| Documentation/spec sync | Any implementation-corrected behavior is reflected in the canonical spec or implementation packet before final closure. |

## Evidence Required

| Evidence | Required content |
| --- | --- |
| `PARSER_COVERAGE_REPORT.md` | Accepted, refused, and negative syntax coverage by family. |
| `UDR_COVERAGE_REPORT.md` | Parser-support UDR ABI functions and route tests. |
| `SERVER_ENGINE_GAP_CLOSURE_REPORT.md` | Non-cluster engine/server behavior rows closed. |
| `MESSAGE_VECTOR_COVERAGE_REPORT.md` | All error and warning paths mapped to parser-renderable vectors. |
| `FULL_ROUTE_CONFORMANCE_REPORT.md` | Parser-to-server-to-engine-to-parser route coverage. |
| `FULL_REGRESSION_SUITE_PUBLICATION.md` | Commands and labels for future full CTest runs. |

## Rejection Conditions

The coordinator must mark the slice failed if any accepted row is implemented in parser only, UDR only, server only, or engine only without full-route coverage.

Additional rejection conditions:

- A row is closed by source-status vocabulary rather than implementation/refusal evidence.
- A parser path opens database files, performs authority checks as the source of truth, executes SQL, or bypasses `sb_server`.
- A UDR path mutates engine state directly or executes SQL to determine semantics.
- An engine path accepts SQL text instead of SBLR/internal procedures.
- A raw string crosses a parser/server/engine boundary as the only diagnostic.
- A generated fixture depends on network access or non-reproducible external state.
- A source file becomes a monolithic catch-all instead of a logical-family implementation unit.

## P0E Acceptance

This contract is the shared done standard for all P1+ slices. A slice-specific exception requires an explicit coordinator record, canonical authority reference, and validation gate update before implementation work begins.

# Resource Budget Policy

Status: complete
Search key: `FSPE-RESOURCE-BUDGET-POLICY`
Generated: 2026-05-07 20:32:37 EDT

## Purpose

This pre-code policy establishes default parser, UDR, server, diagnostic, and generated-fixture budgets before architecture decisions hard-code unbounded behavior.

## Default Budgets

| Budget | Default limit | Enforcement owner | Notes |
| --- | ---: | --- | --- |
| SQL statement bytes | 1 MiB | parser runtime | Larger inputs require explicit profile override and streaming design. |
| Identifier bytes | 256 bytes | lexer/binder | Applies after decoding, before catalog lookup. |
| Token count | 131,072 | parser runtime | Excess token streams fail closed before AST/lowering work expands. |
| Literal bytes | 1 MiB | lexer/parser runtime | Applies to raw literal token text before lowering. |
| AST depth | 256 nodes deep | parser/CST/AST | Deep nesting must fail closed with message vector. |
| Parameter count | 65,535 | parser/server | Must fit descriptor and SBLR envelope limits. |
| SBLR envelope bytes | 16 MiB | lowering/server verifier | Chunking required above this limit; no silent truncation. |
| Diagnostic payload bytes | 64 KiB | parser/server/engine | Large detail fields move to retained evidence references. |
| Message-vector count per operation | 1,024 | all diagnostic producers | Excess vectors summarize with truncation diagnostic. |
| Result metadata columns | 4,096 | server/result shape | Wider results require explicit large-result tests. |
| Render output bytes | 1 MiB | parser renderer | Rendered diagnostics/results must not expand unboundedly. |
| Generated fixture rows per batch | 100 surfaces | conformance worker | Mirrors P0C max batch size. |
| Generated fixture shard wall time | 120 seconds | conformance worker | Longer shards must be split or marked long-running. |
| Fuzz single input wall time | 5 seconds | fuzz worker | Timeout must return fail-closed diagnostic. |
| Parser cache entry count | 10,000 | parser cache | Eviction must respect schema/security/descriptor epochs. |

## Enforcement Rules

- Limits are defaults for implementation and tests; any change must update this file, generated fixtures, and message-vector diagnostics.
- Exceeding a limit must return a canonical message vector, not raw text or process abort.
- Budget checks must avoid spinlocks and busy wait loops.
- No resource budget may introduce WAL recovery semantics; recovery remains MGA-based.

## Acceptance

P1+ design and implementation work must reference these budgets before adding parser tables, generated fixture shards, cache state, diagnostic payloads, or server packet chunking behavior.

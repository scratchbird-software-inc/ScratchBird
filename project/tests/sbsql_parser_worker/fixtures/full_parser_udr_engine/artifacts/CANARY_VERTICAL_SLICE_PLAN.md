# Canary Vertical Slice Plan

Status: complete
Search key: `FSPE-CANARY-VERTICAL-SLICE-PLAN`
Generated: 2026-05-07 20:40:29 EDT
Owning slice: `FSPE-001A`

## Purpose

Prove the registry/linter/fixture/oracle/message-vector machinery works end-to-end on a deliberately small representative set before opening broad lexer, CST/AST, expression, statement, binder, lowering, UDR, server, and engine implementation slices.

## Position In Execution_Plan

`FSPE-001A` runs after `FSPE-001` registry generation and linter infrastructure passes. `FSPE-002` must not start until the canary passes and the tracker/gate/queue state is updated.

## Canary Surface Set

| Case | Source row | Required proof |
| --- | --- | --- |
| Native statement | `SBSQL-E4E0E6EB328C` / `create_table_stmt` | Registry lookup, parse entry, fixture lookup, oracle lookup, diagnostic route, and handler coverage lint. |
| Expression/runtime | `SBSQL-971C709406A0` / `@` | Registry lookup for expression/runtime row and handler coverage lint. |
| Cluster-private standalone fail-closed | `SBSQL-39C545BEBF5A` / `cluster_publish_options` | Profile gate lookup and standalone fail-closed message-vector rendering. |
| Native-now closure decision | `SBSQL-DF502F8DF4FA` / `Accept` | Registry lookup proves the former promotion row is native_now, routed through the expression runtime parser/lowering/engine path, and retains closure-action evidence. |
| Reference alias mapping/rendering | `apache_ignite:query_select` | Reference alias backlog lookup, native surface mapping/refusal, and reference rendering diagnostic path. |

## Required Outputs

- Generated canary registry subset or registry-linter fixture manifest.
- Small durable CTest shard under `project/tests/sbsql_parser_worker/generated/canary/` or equivalent generated test root.
- Canary result report in `artifacts/CANARY_VERTICAL_SLICE_RESULT.md`.
- Failure inventory rows for any canary failure, including message-vector registration requirement where applicable.

## Acceptance

The canary passes only when all required proof points below are true:

- Every canary row resolves through generated registry constants and row membership/oracle artifacts.
- Handler coverage lint fails closed for a missing parser, UDR, SBLR, server, engine, diagnostic, or fixture assignment.
- At least one parser-to-server-to-engine route is exercised, even if the route is a minimal smoke fixture.
- The cluster-private row fails closed in standalone mode with a concrete message vector and parser rendering template.
- The reference alias case maps to native SBSQL behavior or exact canonical refusal with reference/profile rendering evidence.
- The canary CTest label is durable and included in the validation command registry before `FSPE-001A` closes.

## Non-Acceptance

Parser-only, direct-helper-only, documentation-only, or manually inspected evidence cannot close this slice.

## Closure

This historical plan is complete. Execution evidence is retained in `artifacts/CANARY_VERTICAL_SLICE_RESULT.md`, where `sbsql_canary_vertical_slice_gate` passed `2/2` and the focused parser-worker shard passed `17/17`.

# Performance Resource And Operational Hardening Report

Status: complete
Search key: `FSPE-012-HARDENING-GATE-REPORT`
Owning slice: `FSPE-012`

## Summary

FSPE-012 materialized `sbsql_no_spin_no_wal_no_direct_db_gate` as the P12 hardening gate.

The gate verifies:

- Parser resource-budget defaults match `RESOURCE_BUDGET_POLICY.md`.
- Parser metrics snapshots and heartbeat records expose resource budgets, cache counts, parser state, redaction policy, and parser subsystem counters.
- Parser wire execution rejects oversized statement payloads with `SBSQL.RESOURCE.STATEMENT_TOO_LARGE`.
- Parser, UDR, server, and engine route source does not introduce spinlock/busy-wait terms.
- WAL-related source text in scanned paths is limited to parser keywords/registry rows or explicit anti-WAL/refusal evidence.
- Parser and parser-support UDR source does not open database files or use direct engine/database file authority.
- Engine internal/SBLR paths do not include parser pipeline authority such as CST/AST/binder/lowering calls.

## Gate

| Field | Value |
| --- | --- |
| CTest label | `sbsql_no_spin_no_wal_no_direct_db_gate` |
| Source | `project/tests/sbsql_parser_worker/generated/hardening/sbsql_no_spin_no_wal_no_direct_db_gate.cpp` |
| Parser runtime changes | `ParserResourceBudget`; metrics resource-budget JSON; max statement byte rejection |
| Unexpected failures | 0 |

## Boundary Result

The engine boundary remains SBLR/internal-procedure only. Parser and UDR paths do not gain direct database-file authority. Recovery remains MGA-based, with WAL mentions only as rejected/anti-authority evidence or SBSQL surface names.

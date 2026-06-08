# FSPE-009 Validation Result

Status: passed
Search key: `FSPE-009-VALIDATION-RESULT`
Date: 2026-05-07

## Scope

FSPE-009 implements the engine-owned SBSQL behavior-family gate:

- Direct engine API/SBLR dispatch conformance for the current engine operation matrix.
- Public ABI operation-envelope bridge for a non-mutating, name-free `observability.show_version` path.
- Exact non-cluster refusal for cluster-private rows when private cluster authority is unavailable.
- Zero SQL text accepted by the engine operation-envelope path.

## Validation Commands

```bash
ctest --test-dir build/sbsql_parser_worker_validation -L sb_engine_sbsql_behavior_conformance --output-on-failure
```

Result: passed, `1/1`.

```bash
ctest --test-dir build/sbsql_parser_worker_validation -R 'sb_engine_public_sblr_admission_fixture|sb_engine_sbsql_behavior_conformance_fixture' --output-on-failure
```

Result: passed, `2/2`.

## Matrix Closure

- `ENGINE_GAP_IMPLEMENTATION_BACKLOG.csv`: `816` non-cluster/profile-scoped rows closed by `closed_by_engine_api_sblr_family_gate`; `116` cluster-private rows closed by `closed_by_cluster_fail_closed_gate`.
- `SBSQL_ENGINE_GAP_MATRIX.csv`: same status closure applied to the implementation-packet matrix.
- `SBLR_API_OPERATION_MATRIX.yaml`: callable rows are validated through engine SBLR dispatch; cluster rows are validated as fail-closed without private authority.

## Boundary

This slice closes the engine-side behavior-family gate only. It does not claim FSPE-010 server IPC/full-route behavior, FSPE-010A message-vector rendering closure, generated full-surface regression coverage, or final zero-SQL audit closure.

The engine remains non-SQL: it accepts validated SBLR/internal operation envelopes only, rejects SQL-text-marked envelopes, and keeps UUID/security/transaction authority engine-owned.

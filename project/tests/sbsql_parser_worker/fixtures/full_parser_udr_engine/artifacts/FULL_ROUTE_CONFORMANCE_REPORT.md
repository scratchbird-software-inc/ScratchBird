# Full Route Conformance Report

Status: passed_fspe_010
Search key: `FSPE-FULL_ROUTE_CONFORMANCE_REPORT`

## Covered Route

FSPE-010 validated the current full route:

```text
client SBSQL
  -> sb_listener
  -> sbp_sbsql
  -> sb_server SBPS auth/attach/resolve/execute
  -> server SBLR admission
  -> public engine ABI for canonical binary operation envelopes
  -> parser-visible result/message frames
```

## Evidence

- `sb_listener_sbp_sbsql_server_engine_execution_smoke` passed under the `sb_server_sbsql_admission_conformance` label.
- `sb_server_sbsql_admission_conformance` passed direct server admission checks for all 15 current SBLR operation-family values.
- Raw SQL/source-text-marked envelopes and non-cluster cluster-private envelopes fail closed at server admission.
- Focused shard passed `25/25`.

## Deferred By Design

- Full diagnostic/message-vector rendering closure remains FSPE-010A.
- Large result streaming and cursor protocol expansion remains FSPE-010B.

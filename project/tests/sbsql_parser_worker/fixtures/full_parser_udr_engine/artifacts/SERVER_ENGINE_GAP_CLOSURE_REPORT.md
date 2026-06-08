# SERVER ENGINE GAP CLOSURE REPORT

Status: complete
Search key: `FSPE-SERVER_ENGINE_GAP_CLOSURE_REPORT`

## FSPE-009 Result

FSPE-009 closed the engine-owned behavior-family gate for the SBSQL gap matrix.

Counts:

- Non-cluster/profile-scoped rows: `816`, status `closed_by_engine_api_sblr_family_gate`.
- Cluster-private rows: `116`, status `closed_by_cluster_fail_closed_gate`.
- Open engine-gap rows after FSPE-009: `0`.

Validation:

- `ctest --test-dir build/sbsql_parser_worker_validation -L sb_engine_sbsql_behavior_conformance --output-on-failure`
- Result: passed, `1/1`.
- Focused public ABI regression: `ctest --test-dir build/sbsql_parser_worker_validation -R 'sb_engine_public_sblr_admission_fixture|sb_engine_sbsql_behavior_conformance_fixture' --output-on-failure`, passed `2/2`.

## Implemented Evidence

- `project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml` now aligns `cluster.sys.agents` with the cluster fail-closed dispatch behavior.
- `project/src/engine/sblr/sblr_dispatch.cpp` maps `session.notification.unlisten_all` to `SBLR_EVENT_CHANNEL_UNLISTEN_ALL`.
- `project/src/engine/public_abi.cpp` admits binary SBLR operation envelopes only when canonical bytes are a validated engine-owned operation envelope.
- `project/tests/engine_public_abi/sbsql_behavior_conformance_fixture.cpp` loads the SBLR/API matrix and proves mapped non-cluster rows dispatch to engine APIs while cluster rows fail closed without private authority.
- `project/tests/engine_public_abi/public_sblr_admission_fixture.cpp` proves `observability.show_version` reaches a public ABI row-batch result through SBLR operation-envelope dispatch and that SQL-text-marked envelopes are rejected.

## FSPE-010 Server Admission Handoff Result

FSPE-010 consumed the FSPE-009 engine handoff by adding a server-owned SBLR admission validator and full-route conformance label.

Validation:

- `ctest --test-dir build/sbsql_parser_worker_validation -L sb_server_sbsql_admission_conformance --output-on-failure`
- Result: passed, `2/2`.
- Focused shard: `ctest --test-dir build/sbsql_parser_worker_validation --output-on-failure`, passed `25/25`.

## Boundary

This report does not close FSPE-010A diagnostic rendering or FSPE-010B large-result streaming. Those remain successor gates.

No engine path added by FSPE-009 parses SQL text, executes donor command text, trusts parser AST/BoundAST, or accepts names as durable authority. Engine authority remains SBLR/internal operation envelopes, UUID/descriptor context, engine diagnostics, and engine-owned security/transaction checks.

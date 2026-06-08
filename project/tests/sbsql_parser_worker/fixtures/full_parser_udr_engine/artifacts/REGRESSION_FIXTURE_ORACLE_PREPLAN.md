# Regression Fixture and Oracle Preplan

Status: complete
Search key: `FSPE-REGRESSION-FIXTURE-ORACLE-PREPLAN`
Generated: 2026-05-07 20:32:37 EDT

## Purpose

This artifact moves fixture sharding and expected-result authority planning ahead of implementation. P1+ workers must use `BATCH_ROW_MEMBERSHIP.csv` and `SEMANTIC_ORACLE_AUTHORITY_MAP.csv` before generating parser, UDR, server, engine, or full-route fixtures.

## Fixture Roots

| Fixture class | Required root |
| --- | --- |
| Generated batch fixtures | `project/tests/sbsql_parser_worker/generated/<batch_id>/` |
| Golden parser artifacts | `project/tests/sbsql_parser_worker/golden/parser/<batch_id>/` |
| UDR ABI fixtures | `project/tests/sbsql_parser_worker/golden/udr/<batch_id>/` |
| Server/full-route fixtures | `project/tests/sbsql_parser_worker/golden/full_route/<batch_id>/` |
| Refusal/negative fixtures | `project/tests/sbsql_parser_worker/golden/refusal/<batch_id>/` |
| Replay manifests | `project/tests/sbsql_parser_worker/replay/<batch_id>/` |

## Sharding Rules

- Every surface row belongs to exactly one `batch_id` in `BATCH_ROW_MEMBERSHIP.csv`.
- No generated fixture shard may contain more than the batch `max_batch_size` recorded in `REGISTRY_FAMILY_BATCHING_PLAN.csv`.
- Every generated test must carry its `batch_id`, `surface_id`, `validation_fixture_id`, and CTest label.
- A fixture may be parser-only as an intermediate artifact, but parser-only evidence cannot close an accepted non-cluster behavior.

## Oracle Rules

- Every fixture must resolve its expected result from `SEMANTIC_ORACLE_AUTHORITY_MAP.csv` before it is emitted.
- `native_future` rows must receive a concrete promotion/refusal expected result before executable fixture generation.
- Cluster-private rows must include standalone fail-closed expected diagnostics and, where profile-enabled, cluster-profile expected behavior.
- Donor aliases must compare donor/profile rendering only after native SBSQL expected behavior is resolved.

## Acceptance

The preplan is complete when every surface row has batch membership and an assigned oracle source. Expected output values may still be filled by the owning implementation slice, but the authority source cannot be selected ad hoc during fixture generation.

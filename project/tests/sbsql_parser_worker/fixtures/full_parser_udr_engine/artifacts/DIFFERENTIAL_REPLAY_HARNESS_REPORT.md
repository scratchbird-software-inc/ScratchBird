# Differential Replay Harness Report

Status: complete
Search key: `FSPE-DIFFERENTIAL-REPLAY-HARNESS-REPORT`
Owning slice: `FSPE-011F`

## Summary

| Field | Value |
| --- | --- |
| Fixture corpus version | FSPE-011F deterministic replay corpus |
| Fixture index | `project/tests/sbsql_parser_worker/generated/replay/DIFFERENTIAL_REPLAY_FIXTURE_INDEX.csv` |
| Expected payloads | `project/tests/sbsql_parser_worker/generated/replay/DIFFERENTIAL_REPLAY_EXPECTED_PAYLOADS.jsonl` |
| Route manifest | `project/tests/sbsql_parser_worker/generated/replay/DIFFERENTIAL_REPLAY_ROUTE_MANIFEST.csv` |
| Total fixtures | 2,617 |
| Parser-only passes | 2,617 indexed replay records |
| Parser bind/lower passes | 2,617 indexed replay records |
| UDR conversion passes | 2,560 native standalone/profile replay records |
| Server admission passes | 2,617 indexed replay records |
| Engine behavior passes | 2,560 native standalone/profile replay records |
| Full-route passes | 2,560 native standalone/profile replay records |
| Donor alias passes | 10 native donor-surface replay records plus donor route manifest coverage |
| Diagnostic passes | 2,617 indexed replay records |
| Expected refusals | 57 cluster/profile-gated replay records |
| Unexpected failures | 0 |
| Retained evidence root | `project/tests/sbsql_parser_worker/generated/replay/` |
| CTest gate | `sbsql_differential_replay_harness_gate` |
| Exhaustive E2E consumer | `sbsql_exhaustive_e2e_regression_gate` validates this corpus plus dynamic UDR-to-SBLR execution |

## Route Coverage

| Route | Fixture count | Label |
| --- | ---: | --- |
| `parser_parse_only` | 2,617 | `sbsql_replay_parser_only` |
| `parser_bind_lower` | 2,617 | `sbsql_replay_parser_bind_lower` |
| `udr_sql_to_sblr` | 2,560 | `sbsql_replay_udr` |
| `server_admission` | 2,617 | `sbsql_replay_server_admission` |
| `engine_behavior` | 2,560 | `sbsql_replay_engine_behavior` |
| `full_route` | 2,560 | `sbsql_replay_full_route` |
| `donor_alias` | 10 | `sbsql_replay_donor_alias` |
| `diagnostic` | 2,617 | `sbsql_replay_diagnostic` |

## Authority Checks

The harness validates every replay fixture against:

- `SEMANTIC_ORACLE_AUTHORITY_MAP.csv`
- `BATCH_ROW_MEMBERSHIP.csv`
- `SURFACE_IMPLEMENTATION_BACKLOG.csv`
- `SBSQL_SURFACE_REGISTRY.csv`
- `SBSQL_TO_SBLR_OPERATION_MATRIX.csv`
- `DONOR_ALIAS_TO_SBSQL_SURFACE_MATRIX.csv`
- `DONOR_ALIAS_RENDERING_FIXTURES.csv`
- `VALIDATION_COMMAND_MATERIALIZATION.csv`

## Failure Row Schema

| Field | Required content |
| --- | --- |
| `fixture_id` | Stable replay fixture id. |
| `surface_id` | SBSQL surface registry row. |
| `operation_family` | SBLR operation family from the operation matrix. |
| `batch_id` | Owning deterministic regression batch. |
| `route` | Replay route that failed. |
| `expected` | Expected parse, bound shape, SBLR policy, result, engine effect, or diagnostic. |
| `actual` | Actual parse, bound shape, SBLR policy, result, engine effect, or diagnostic. |
| `message_vector_id` | Exact returned vector id when present. |
| `evidence_path` | Retained log or packet dump path. |
| `closure_action` | Fixed, accepted non-blocking, or blocker. |

## Closure Rule

FSPE-011F is complete when the CTest label `sbsql_differential_replay_harness_gate` passes and this report continues to record zero unexpected failures.

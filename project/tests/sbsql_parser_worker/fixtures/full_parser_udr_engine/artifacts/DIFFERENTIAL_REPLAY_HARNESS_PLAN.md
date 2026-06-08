# Differential Replay Harness Plan

Status: complete
Search key: `FSPE-DIFFERENTIAL-REPLAY-HARNESS-PLAN`
Owning slice: `FSPE-011F`

## Purpose

The replay harness makes every generated SBSQL surface fixture addressable as a durable regression input without regenerating expected results from the current implementation. It cross-links fixture identity, parser profile, session context, route assignment, semantic oracle authority, result-shape expectation, message-vector expectation, and cleanup policy.

The engine route remains SBLR/internal-procedure only. No SQL text reaches engine as replay authority.

## Durable Assets

| Asset | Role |
| --- | --- |
| `project/tests/sbsql_parser_worker/generated/replay/DIFFERENTIAL_REPLAY_ROUTE_MANIFEST.csv` | Defines replay routes and CTest labels. |
| `project/tests/sbsql_parser_worker/generated/replay/DIFFERENTIAL_REPLAY_FIXTURE_INDEX.csv` | One replay-ready row for each of the 2,617 generated surface fixtures. |
| `project/tests/sbsql_parser_worker/generated/replay/DIFFERENTIAL_REPLAY_EXPECTED_PAYLOADS.jsonl` | Nested expected payloads keyed by stable `fixture_id`. |
| `project/tests/sbsql_parser_worker/generated/replay/sbsql_differential_replay_harness_gate.cpp` | CTest gate validating route, fixture, oracle, batch, registry, operation-matrix, and payload consistency. |

## Fixture Record Schema

Each replay fixture index row contains these fields.

| Field | Required value |
| --- | --- |
| `fixture_id` | Stable `SBSQL-SURFACE-*` id from `SEMANTIC_ORACLE_AUTHORITY_MAP.csv`. |
| `surface_id` | Stable `SBSQL-*` surface id. |
| `batch_id` | Owning deterministic regression batch. |
| `canonical_name` | Surface canonical name for crosswalk only; it is not used as engine authority. |
| `family` / `surface_kind` / `source_status` / `cluster_scope` | Frozen surface classification from execution_plan artifacts. |
| `operation_family` | SBLR operation family from the operation matrix. |
| `primary_route` / `route_set` | Replay route assignment. |
| `parser_profile` | Standalone, native-future exact-refusal/promotion, or cluster-private profile/refusal policy. |
| `session_context` | Required session, database, transaction, security, language, and result-contract fields. |
| `input_text` | Durable client-facing SBSQL/donor-profile text or explicit surface replay key for grammar-fragment fixtures. |
| `expected_parse` | Accepted, profile-gated, or exact canonical refusal expectation. |
| `expected_bound_shape` | Operation family and `ExecutionResultEnvelope.v3` result shape. |
| `expected_sblr_digest_policy` | Stable normalized digest policy, explicitly independent from current implementation output. |
| `expected_server_result` | Admission/result or exact-refusal contract. |
| `expected_engine_effect` | SBLR/internal-procedure execution or no-mutation exact refusal; no SQL text reaches engine. |
| `expected_message_vector` | Canonical message-vector expectation. |
| `expected_rendered_output` | Expected rendered result envelope. |
| `oracle_type` / `oracle_source` / `source_search_key` | Independent semantic oracle authority. |
| `expected_result_summary` | Expected behavior summary from the semantic oracle map. |
| `expected_payload_json` | JSONL anchor for nested expected CST/AST/BoundAST/SBLR/result/message payload. |
| `status` | `replay_ready`. |

## Replay Routes

| Route | CTest label | Required scope |
| --- | --- | --- |
| `parser_parse_only` | `sbsql_replay_parser_only` | Lexical, CST, AST, and parse diagnostic replay. |
| `parser_bind_lower` | `sbsql_replay_parser_bind_lower` | BoundAST, descriptor, and SBLR envelope replay. |
| `udr_sql_to_sblr` | `sbsql_replay_udr` | Trusted parser-support UDR conversion replay. |
| `server_admission` | `sbsql_replay_server_admission` | sb_server SBLR admission/revalidation and exact refusal replay. |
| `engine_behavior` | `sbsql_replay_engine_behavior` | SBLR/internal API engine behavior or exact refusal replay. |
| `full_route` | `sbsql_replay_full_route` | Parser worker to server to engine result-envelope replay. |
| `donor_alias` | `sbsql_replay_donor_alias` | Donor alias rendering/profile/exact-refusal replay. |
| `diagnostic` | `sbsql_replay_diagnostic` | Message-vector and rendered diagnostic replay. |

## Failure Row Schema

Replay failures must report `fixture_id`, `surface_id`, `operation_family`, `route`, `batch_id`, `expected`, `actual`, `message_vector_id`, and retained `evidence_path`.

## Closure Rule

The slice closes only when `sbsql_differential_replay_harness_gate` passes and the report records zero unexpected failures.

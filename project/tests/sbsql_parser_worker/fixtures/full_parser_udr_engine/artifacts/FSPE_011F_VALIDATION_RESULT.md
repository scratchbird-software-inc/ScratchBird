# FSPE-011F Validation Result

Status: complete
Search key: `FSPE-011F-VALIDATION-RESULT`
Owning slice: `FSPE-011F`

## Commands Run

| Command | Result |
| --- | --- |
| `cmake -S project -B build/sbsql_parser_worker_validation` | passed |
| `cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_differential_replay_harness_gate -j 4` | passed |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_differential_replay_harness_gate --output-on-failure` | passed, 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L "sbsql_regression_test_bed_generation_gate\|sbsql_semantic_oracle_authority_gate\|sbsql_differential_replay_harness_gate" --output-on-failure` | passed, 3/3 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure` | passed, 29/29 |
| `python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/p0_precode_validation.py --gate all` | passed |

## Closure Evidence

FSPE-011F materialized the differential replay harness as durable repository assets:

- `project/tests/sbsql_parser_worker/generated/replay/DIFFERENTIAL_REPLAY_ROUTE_MANIFEST.csv`
- `project/tests/sbsql_parser_worker/generated/replay/DIFFERENTIAL_REPLAY_FIXTURE_INDEX.csv`
- `project/tests/sbsql_parser_worker/generated/replay/DIFFERENTIAL_REPLAY_EXPECTED_PAYLOADS.jsonl`
- `project/tests/sbsql_parser_worker/generated/replay/sbsql_differential_replay_harness_gate.cpp`

The gate validates 2,617 replay-ready fixtures against semantic oracle authority, batch membership, surface backlog, canonical surface registry, SBLR operation matrix, donor alias matrix, donor alias fixture policy, and validation-command materialization.

## Result

FSPE-011F is complete. FSPE-012 can open as the next serialized slice.

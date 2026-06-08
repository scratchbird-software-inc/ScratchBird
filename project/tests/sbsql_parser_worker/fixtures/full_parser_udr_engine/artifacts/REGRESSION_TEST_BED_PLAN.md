# SBSQL/SB Regression Test Bed Plan

Status: complete
Search key: `FSPE-REGRESSION-TEST-BED-PLAN`

Owning slice: `FSPE-011A`

## Scope

This artifact defines the reusable generated regression test bed for SBSQL/SB
surfaces. It is a test-bed policy and asset-layout gate, not a claim that every
semantic fixture has already been generated or executed. Donor rendering,
independent semantic oracle completion, persistence, concurrency, and replay
remain owned by FSPE-011B through FSPE-011F.

FSPE-014G extends this plan with `project/tests/sbsql_parser_worker/generated/exhaustive_e2e/`,
which provides a CTest gate over every registered surface replay fixture plus
the dynamic UDR-to-SBLR procedure path. This preserves the policy/asset split:
FSPE-011A defines the durable layout, while FSPE-014G asserts the registered
surface regression suite can be rerun end to end.

## Project Asset

The durable project-side manifest is:

```text
project/tests/sbsql_parser_worker/generated/regression/REGRESSION_TEST_BED_MANIFEST.csv
```

The manifest is the reusable asset consumed by the FSPE-011A gate and by later
generated fixture slices. Execution_Plan evidence may describe the asset, but durable
fixture roots must remain under `project/tests/`, not under `build/`, `/tmp`, or
execution_plan-only artifact directories.

## Fixture Roots

Generated fixtures are rooted under `project/tests/sbsql_parser_worker/generated/regression/`.

Required fixture suites:

| Suite | Durable root | CTest label |
| --- | --- | --- |
| Smoke fixtures | `project/tests/sbsql_parser_worker/generated/regression/smoke` | `sbsql_regression_smoke` |
| Parser lexer/CST/AST fixtures | `project/tests/sbsql_parser_worker/generated/regression/parser` | `sbsql_regression_parser_unit` |
| Expression and builtin fixtures | `project/tests/sbsql_parser_worker/generated/regression/expression` | `sbsql_regression_expression_builtin` |
| Statement-family fixtures | `project/tests/sbsql_parser_worker/generated/regression/statement` | `sbsql_regression_statement_family` |
| Binder/name/descriptor/security fixtures | `project/tests/sbsql_parser_worker/generated/regression/binder` | `sbsql_regression_binder_authority` |
| SBLR lowering and verifier fixtures | `project/tests/sbsql_parser_worker/generated/regression/lowering` | `sbsql_regression_sblr_lowering` |
| UDR ABI/context fixtures | `project/tests/sbsql_parser_worker/generated/regression/udr` | `sbsql_regression_udr_support` |
| Server admission/result/message fixtures | `project/tests/sbsql_parser_worker/generated/regression/server` | `sbsql_regression_server_admission` |
| Engine behavior fixtures | `project/tests/sbsql_parser_worker/generated/regression/engine` | `sbsql_regression_engine_behavior` |
| Donor alias parser/rendering fixtures | `project/tests/sbsql_parser_worker/generated/regression/donor_alias` | `sbsql_regression_donor_alias` |
| Diagnostic/message-vector fixtures | `project/tests/sbsql_parser_worker/generated/regression/diagnostics` | `sbsql_regression_diagnostic` |
| Full-route listener/parser/server/engine fixtures | `project/tests/sbsql_parser_worker/generated/regression/full_route` | `sbsql_regression_full_route` |
| Exhaustive registered-surface E2E gate | `project/tests/sbsql_parser_worker/generated/exhaustive_e2e` | `sbsql_exhaustive_e2e_regression` |
| Fuzz and malicious-input fixtures | `project/tests/sbsql_parser_worker/generated/regression/fuzz` | `sbsql_regression_fuzz` |
| Hardening gates | `project/tests/sbsql_parser_worker/generated/regression/hardening` | `sbsql_regression_hardening` |
| Long-running suites | `project/tests/sbsql_parser_worker/generated/regression/long_running` | `sbsql_regression_long_running` |

## Batch And Shard Rules

- `REGISTRY_FAMILY_BATCHING_PLAN.csv` remains the source of batch IDs, fixture roots, CTest labels, row counts, and `max_batch_size`.
- `BATCH_ROW_MEMBERSHIP.csv` remains the source of per-surface batch membership.
- Batch roots must be under `project/tests/`.
- Each generated surface fixture must retain its stable `SBSQL-SURFACE-*` fixture ID.
- Each batch must have one stable CTest label from `REGISTRY_FAMILY_BATCHING_PLAN.csv`.
- Membership rows must use the same CTest label as their owning batch.
- `max_batch_size` must be a positive integer and must not exceed 100 rows.
- A batch may be split only by adding a new deterministic `BATCH-*` row and updating membership in a reviewable diff.

## Naming Rules

- Surface fixture files use the stable fixture ID as the basename.
- Batch manifests use `BATCH-NNNN.manifest.json`.
- Expected-result files use `<fixture_id>.expected.json`.
- Failure summaries use `<batch_id>.failure-summary.json`.
- Donor alias fixtures use `SBSQL-DONOR-*` IDs.
- Engine gap fixtures use `SBSQL-GAP-*` IDs.
- Message-vector fixtures use `MSGV-*` IDs.

## Timeout And Retry Policy

| Suite class | Default timeout | Retry policy |
| --- | ---: | --- |
| Smoke | 30 seconds | no retry |
| Parser unit | 60 seconds | no retry |
| UDR support | 60 seconds | no retry |
| Server admission | 90 seconds | no retry |
| Engine behavior | 120 seconds | no retry |
| Donor alias | 90 seconds | no retry |
| Diagnostic | 60 seconds | no retry |
| Full route | 180 seconds | no retry |
| Fuzz | 300 seconds | quarantine only after reproducible fixture capture |
| Hardening | 300 seconds | no retry |
| Long-running | 900 seconds | no retry without coordinator approval |

Retries must not hide nondeterminism. Any retried failure must preserve the
first failure summary and must be recorded in `FAILURE_INVENTORY.csv` when it is
an infrastructure or parser-visible event.

## Failure Summary Format

Every generated test failure summary must include:

- `surface_id`
- `fixture_id`
- `batch_id`
- `ctest_label`
- `canonical_name`
- `family`
- `sblr_operation_family`
- `diagnostic_code`
- `oracle_source`
- `expected_result_summary`
- `actual_result_summary`
- `route`
- `owning_slice`

This format is designed so a full CTest run can report exact surface IDs,
operation families, diagnostics, and owning batch without reading implementation
source.

## Regeneration Rules

- Regeneration inputs are canonical specs, implementation packets, frozen
  execution_plan matrices, and deterministic generator code in this repository.
- No network access is permitted for fixture regeneration.
- Regenerated files must be stable across repeated runs with the same inputs.
- Review diffs must show added, removed, or changed fixture IDs and batch IDs.
- A generator may not write under `build/`, `/tmp`, or execution_plan evidence roots
  for durable regression assets.
- Later semantic slices may add fixture payloads, but they must preserve the
  manifest columns and batch labels defined here.

## Reuse Rule

Later full SB regression runs execute durable generated suites by CTest labels.
FSPE-014C publishes the final full-regression command set, but FSPE-011A fixes
the reusable roots, labels, shard policy, and failure summary shape.

## Validation

The FSPE-011A gate is:

```bash
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_regression_test_bed_generation_gate --output-on-failure
```

# FSPE-011A Validation Result

Status: complete
Search key: `FSPE_011A_VALIDATION_RESULT`

## Scope

This artifact records validation evidence for the FSPE-011A reusable generated
regression test-bed policy and project-side manifest.

Scope boundary:

- FSPE-011A defines durable fixture roots, labels, shard policy, timeout policy,
  retry policy, failure summaries, and deterministic regeneration rules.
- FSPE-011A does not claim every semantic fixture has already been generated or
  executed.
- Reference rendering, independent semantic oracle completion, persistence,
  concurrency, and replay remain owned by FSPE-011B through FSPE-011F.

## Validation Commands

Passed:

```bash
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_regression_test_bed_generation_gate -j 4
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_regression_test_bed_generation_gate --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
```

## Observed Results

- `sbsql_regression_test_bed_generation_gate`: 1/1 passed.
- `sbsql_parser_worker`: 24/24 passed with the regression test-bed generation gate included.
- Direct gate summary: `manifest_suites=15 batches=77 membership_rows=2617`.

## Remaining FSPE-011A Closure Gaps

None. `FSPE-011B` is the next serialized execution_plan slice.

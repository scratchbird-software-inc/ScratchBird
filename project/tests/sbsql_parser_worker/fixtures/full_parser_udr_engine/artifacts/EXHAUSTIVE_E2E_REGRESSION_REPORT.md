# Exhaustive E2E Regression Report

Status: complete
Search key: `FSPE-EXHAUSTIVE-E2E-REGRESSION`
Owning slice: `FSPE-014G`
Date: 2026-05-08

## Summary

FSPE-014G adds `sbsql_exhaustive_e2e_regression_gate` as a reusable CTest gate for the full registered SBSQL surface. It closes the prior gap between generated route-accounting evidence and a repeatable parser-to-UDR-to-server-to-engine regression gate.

The gate is exhaustive over the finite registered surface and fixture authority:

- 2,617 `SBSQL_SURFACE_REGISTRY.csv` rows have one replay fixture and one expected payload.
- 2,617 replay fixtures cover `parser_parse_only`, `parser_bind_lower`, `diagnostic`, and `server_admission`.
- 2,560 executable fixtures cover `udr_sql_to_sblr`, `engine_behavior`, and `full_route`.
- 57 exact-refusal fixtures are verified to avoid `engine_behavior` and `full_route` mutation routes.
- 1,534 expression-runtime datatype/function/operator/variable surfaces are covered, including 1,515 functions, 18 operators, and 1 variable.
- 1,083 non-expression command and statement surfaces are covered.
- 19 SBLR operation families are represented.
- 932 engine gap rows are closed by engine API/SBLR family or cluster fail-closed gates.
- 312 donor alias rows are closed by donor alias rendering coverage.

## Dynamic Procedure Path

The gate includes a stored-procedure simulation that concatenates SBSQL text, refuses execution without engine-resolved UUID context, sends the combined string through `sbu_sbsql_parse_to_sblr`, validates that the generated payload is SBLR without raw SQL text, verifies the engine-supplied `resolved_uuid=` value is preserved in `resolved_object_uuids`, admits it through `sb_server`, prepares it, executes it, and fetches the cursor result.

Tamper coverage verifies that a generated SBLR payload with a `sql_text` marker is rejected with `SBLR.SQL_TEXT_FORBIDDEN`.

## Commands

```bash
cmake --build build/sbsql_parser_worker_validation --target sbsql_exhaustive_e2e_regression_gate -j 4
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_exhaustive_e2e_regression --output-on-failure
```

## Result

`sbsql_exhaustive_e2e_regression_gate` passed on 2026-05-08. This is a durable regression label and is also tracked by the deterministic no-network generated-artifact manifest.

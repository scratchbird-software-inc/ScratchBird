# FSPE-014G Validation Result

Status: complete
Search key: `FSPE-014G-VALIDATION-RESULT`
Owning slice: `FSPE-014G`
Date: 2026-05-08

## Commands Run

| Command | Result |
| --- | --- |
| `cmake -S project -B build/sbsql_parser_worker_validation -DCMAKE_BUILD_TYPE=Release` | passed |
| `cmake --build build/sbsql_parser_worker_validation --target sbsql_exhaustive_e2e_regression_gate -j 4` | passed |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_exhaustive_e2e_regression --output-on-failure` | passed, 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_deterministic_no_network_gate --output-on-failure` | passed, 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure` | passed, 40/40 |

## Closure Evidence

FSPE-014G added `project/tests/sbsql_parser_worker/generated/exhaustive_e2e/EXHAUSTIVE_E2E_VARIATION_MATRIX.csv` and `project/tests/sbsql_parser_worker/generated/exhaustive_e2e/sbsql_exhaustive_e2e_regression_gate.cpp`, wired the gate into CMake/CTest, and updated `DETERMINISTIC_ARTIFACT_MANIFEST.csv`.

## Result

FSPE-014G is complete. The final audit now depends on the exhaustive E2E regression gate.

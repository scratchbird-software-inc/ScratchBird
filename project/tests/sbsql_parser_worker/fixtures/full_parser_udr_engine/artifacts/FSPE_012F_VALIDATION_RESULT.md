# FSPE-012F Validation Result

Status: complete
Date: 2026-05-08
Search key: `FSPE-012F-VALIDATION-RESULT`
Owning slice: `FSPE-012F`

## Scope

FSPE-012F added the metadata and result-shape compatibility gate for parser/binder result-shape keys plus server rowset, multi-result, warning, command-tag, affected-row, and cursor metadata contracts.

## Implementation Evidence

- `project/tests/sbsql_parser_worker/generated/result_shape/sbsql_metadata_result_shape_gate.cpp` adds the generated result-shape gate.
- `project/tests/sbsql_parser_worker/CMakeLists.txt` wires `sbsql_metadata_result_shape_gate` into CTest with labels `sbsql_metadata_result_shape_gate`, `sbsql_result_shape_gate`, and `sbsql_parser_worker`.
- `project/src/server/sblr_dispatch_server.cpp` emits canonical rowset metadata, completion metadata, multi-result metadata columns, and cursor metadata contract fields under search key `SB_SERVER_SBLR_DISPATCH_RESULTS`.
- `project/tests/sbsql_parser_worker/generated/repro/DETERMINISTIC_ARTIFACT_MANIFEST.csv` now includes the new result-shape gate as a tracked generated fixture.

## Validation

| Command | Result |
| --- | --- |
| `cmake -S project -B build/sbsql_parser_worker_validation` | Passed |
| `cmake --build build/sbsql_parser_worker_validation --target sbsql_metadata_result_shape_gate -j 4` | Passed |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_metadata_result_shape_gate --output-on-failure` | Passed, 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_deterministic_no_network_gate --output-on-failure` | Passed, 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure` | Passed, 36/36 |
| `python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/p0_precode_validation.py --gate all` | Passed |

## Result

FSPE-012F is complete. FSPE-012G can open as the next serialized slice.

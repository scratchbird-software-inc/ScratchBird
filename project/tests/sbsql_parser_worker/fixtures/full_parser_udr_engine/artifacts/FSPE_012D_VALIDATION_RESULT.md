# FSPE-012D Validation Result

Status: complete
Date: 2026-05-08
Search key: `FSPE-012D-VALIDATION-RESULT`
Owning slice: `FSPE-012D`

## Scope

FSPE-012D added the cross-platform path/process/IPC gate for parser, listener, and server runtime boundaries.

## Implementation Evidence

- `project/tests/sbsql_parser_worker/generated/platform/sbsql_cross_platform_ipc_gate.cpp` adds the runtime and source-contract gate.
- `project/tests/sbsql_parser_worker/CMakeLists.txt` wires `sbsql_cross_platform_ipc_gate` into CTest with labels `sbsql_cross_platform_ipc_gate`, `sbsql_platform_ipc_gate`, and `sbsql_parser_worker`.
- `project/tests/sbsql_parser_worker/generated/repro/DETERMINISTIC_ARTIFACT_MANIFEST.csv` now includes the new platform gate as a tracked generated fixture.

## Validation

| Command | Result |
| --- | --- |
| `cmake -S project -B build/sbsql_parser_worker_validation` | Passed |
| `cmake --build build/sbsql_parser_worker_validation --target sbsql_cross_platform_ipc_gate -j 4` | Passed |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_cross_platform_ipc_gate --output-on-failure` | Passed, 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_deterministic_no_network_gate --output-on-failure` | Passed, 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure` | Passed, 34/34 |
| `python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/p0_precode_validation.py --gate all` | Passed |

## Result

FSPE-012D is complete. FSPE-012E can open as the next serialized slice.

# FSPE-013A Validation Result

Status: complete
Date: 2026-05-08
Search key: `FSPE-013A-VALIDATION-RESULT`
Owning slice: `FSPE-013A`

## Scope

FSPE-013A verifies stable standard function identity by reconciling engine seed rows with the canonical fixed UUID, name lookup, and catalog object requirement packets.

## Implementation Evidence

- `project/src/engine/functions/registry/function_seed_registry.cpp` contains 114 canonical fixed UUID seed rows and 6,257 canonical/default/donor/plugin name seed rows.
- `project/src/engine/functions/registry/function_seed_registry.hpp` adds name seed metadata fields for seed ID, namespace, target kind, parser profile, and notes.
- `project/tests/sbsql_parser_worker/generated/catalog/sbsql_fixed_uuid_catalog_seed_gate.cpp` validates canonical fixed UUIDs, name lookup rows, catalog object requirements, engine seed package parity, and non-authoritative parser registry evidence.
- `project/tests/sbsql_parser_worker/CMakeLists.txt` wires the FSPE-013A gate into CTest.
- `project/tests/sbsql_parser_worker/generated/repro/DETERMINISTIC_ARTIFACT_MANIFEST.csv` tracks the generated catalog seed gate.

## Validation

| Command | Result |
| --- | --- |
| `cmake -S project -B build/sbsql_parser_worker_validation` | Passed |
| `cmake --build build/sbsql_parser_worker_validation --target sbsql_fixed_uuid_catalog_seed_gate -j 4` | Passed |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_fixed_uuid_catalog_seed_gate --output-on-failure` | Passed, 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_deterministic_no_network_gate --output-on-failure` | Passed, 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure` | Passed, 38/38 |
| `python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/p0_precode_validation.py --gate all` | Passed |

## Result

FSPE-013A is complete. FSPE-012H has since closed, and FSPE-013 is ready as the next serialized slice.

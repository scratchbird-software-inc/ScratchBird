# Fixed UUID Catalog Seed Report

Status: complete
Search key: `FSPE-FIXED_UUID_CATALOG_SEED_REPORT`
Owning slice: `FSPE-013A`

## Summary

FSPE-013A reconciled engine-owned standard function seed identity with the canonical fixed UUID seed packet and materialized `sbsql_fixed_uuid_catalog_seed_gate`.

The gate verifies:

- `FIXED_FUNCTION_UUID_REGISTRY.csv` contains 114 unique canonical `sb.fn.*` function IDs with UUIDv7-compatible fixed function UUIDs.
- The engine `BuildStandardFunctionSeedPackage()` registry exactly matches the canonical fixed UUID registry.
- `FUNCTION_NAME_LOOKUP_SEED_MATRIX.csv` contains 6,257 canonical/default/reference/plugin name rows and every engine name seed row matches it.
- Reference and plugin aliases are labels only under compatibility namespaces and are explicitly not durable authority.
- `CATALOG_OBJECT_REQUIREMENTS.csv` has 114 rows matching the fixed registry and requires function, signature, name, alias, UUID, namespace, and descriptor coverage.
- Parser generated registry evidence remains non-authoritative and does not reuse canonical function fixed UUIDs or `sb.fn.*` durable function authority.

## Implementation Evidence

- `project/src/engine/functions/registry/function_seed_registry.cpp` seeds the 114 canonical function UUID rows and 6,257 name/alias rows.
- `project/src/engine/functions/registry/function_seed_registry.hpp` exposes function name seed metadata needed by catalog verification.
- `project/tests/sbsql_parser_worker/generated/catalog/sbsql_fixed_uuid_catalog_seed_gate.cpp` implements the generated FSPE-013A gate.
- `project/tests/sbsql_parser_worker/CMakeLists.txt` wires `sbsql_fixed_uuid_catalog_seed_gate` into CTest with label `sbsql_fixed_uuid_catalog_seed_gate`.

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

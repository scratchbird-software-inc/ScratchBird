# FSPE-012A Validation Result

Status: complete
Search key: `FSPE-012A-VALIDATION-RESULT`
Owning slice: `FSPE-012A`

## Commands Run

| Command | Result |
| --- | --- |
| `cmake -S project -B build/sbsql_parser_worker_validation` | passed |
| `cmake --build build/sbsql_parser_worker_validation --target sbsql_cache_epoch_correctness_conformance -j 4` | passed |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_cache_epoch_correctness_conformance --output-on-failure` | passed, 1/1 |
| `cmake --build build/sbsql_parser_worker_validation --target sbsql_concurrent_session_transaction_conformance -j 4` | passed |
| `ctest --test-dir build/sbsql_parser_worker_validation -L "sbsql_cache_epoch_correctness_conformance\|sbsql_concurrent_session_transaction_conformance" --output-on-failure` | passed, 2/2 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure` | passed, 31/31 |

## Closure Evidence

FSPE-012A added explicit cache key dimensions and invalidation APIs for catalog, security, descriptor, UDR, search path, language, policy profile, parser profile, and result contract changes. The parser wire cache key now records those dimensions before storing SBLR payloads.

## Result

FSPE-012A is complete. FSPE-012B can open as the next serialized slice.

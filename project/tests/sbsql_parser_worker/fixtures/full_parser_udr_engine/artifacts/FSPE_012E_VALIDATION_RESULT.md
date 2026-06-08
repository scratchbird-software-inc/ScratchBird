# FSPE-012E Validation Result

Status: complete
Date: 2026-05-08
Search key: `FSPE-012E-VALIDATION-RESULT`
Owning slice: `FSPE-012E`

## Scope

FSPE-012E added the locale, charset, collation, and timezone regression gate for parser, engine name registry, resource seed-pack, donor temporal wire, and SBLR runtime behavior.

## Implementation Evidence

- `project/tests/sbsql_parser_worker/generated/i18n/sbsql_locale_charset_collation_timezone_gate.cpp` adds the generated i18n/time gate.
- `project/tests/sbsql_parser_worker/CMakeLists.txt` wires `sbsql_locale_charset_collation_timezone_gate` into CTest with labels `sbsql_locale_charset_collation_timezone_gate`, `sbsql_i18n_time_gate`, and `sbsql_parser_worker`.
- `project/tests/sbsql_parser_worker/generated/repro/DETERMINISTIC_ARTIFACT_MANIFEST.csv` now includes the new i18n gate as a tracked generated fixture.

## Validation

| Command | Result |
| --- | --- |
| `cmake -S project -B build/sbsql_parser_worker_validation` | Passed |
| `cmake --build build/sbsql_parser_worker_validation --target sbsql_locale_charset_collation_timezone_gate -j 4` | Passed |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_locale_charset_collation_timezone_gate --output-on-failure` | Passed, 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_deterministic_no_network_gate --output-on-failure` | Passed, 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure` | Passed, 35/35 |
| `python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/p0_precode_validation.py --gate all` | Passed |

## Result

FSPE-012E is complete. FSPE-012F can open as the next serialized slice.

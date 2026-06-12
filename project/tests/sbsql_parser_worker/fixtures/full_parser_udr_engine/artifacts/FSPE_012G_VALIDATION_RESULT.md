# FSPE-012G Validation Result

Status: complete
Date: 2026-05-08
Search key: `FSPE-012G-VALIDATION-RESULT`
Owning slice: `FSPE-012G`

## Scope

FSPE-012G added the security redaction and side-channel gate for public parser/server diagnostics, SBPS message vectors, result metadata projection, cache authority dimensions, prepared-statement stale checks, and hidden-as-missing name-resolution behavior.

## Implementation Evidence

- `project/tests/sbsql_parser_worker/generated/security/sbsql_security_redaction_side_channel_gate.cpp` adds the generated security redaction and side-channel gate.
- `project/tests/sbsql_parser_worker/generated/security/SECURITY_REDACTION_SIDE_CHANNEL_FIXTURES.csv` records hidden, missing, diagnostic, cache-authority, metadata-projection, and reference-rendering fixtures.
- `project/tests/sbsql_parser_worker/CMakeLists.txt` wires `sbsql_security_redaction_side_channel_gate` into CTest with labels `sbsql_security_redaction_side_channel_gate`, `sbsql_security_redaction_gate`, `sbsql_side_channel_gate`, and `sbsql_parser_worker`.
- `project/src/wire/parser_server_ipc/parser_ipc_common.cpp`, `project/src/server/diagnostics.cpp`, and `project/src/server/sbps.cpp` filter public diagnostic/message-vector fields.
- `project/src/parsers/sbsql_worker/rendering/rendering.cpp` redacts hidden/system result metadata from public parser rendering.
- `project/src/parsers/sbsql_worker/cache/sblr_template_cache.*`, `project/src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp`, `project/src/server/session_registry.hpp`, and `project/src/server/sblr_dispatch_server.cpp` include grant, role-set, group-set, descriptor, search-path, language, and policy dimensions in cache/prepared authority checks.
- `project/tests/sbsql_parser_worker/generated/repro/DETERMINISTIC_ARTIFACT_MANIFEST.csv` now includes the security gate and fixture as tracked generated artifacts.

## Validation

| Command | Result |
| --- | --- |
| `cmake -S project -B build/sbsql_parser_worker_validation` | Passed |
| `cmake --build build/sbsql_parser_worker_validation --target sbsql_security_redaction_side_channel_gate -j 4` | Passed |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_security_redaction_side_channel_gate --output-on-failure` | Passed, 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L 'sbsql_cache_epoch_correctness_conformance\|sb_message_vector_error_surface_conformance\|sbsql_metadata_result_shape_gate' --output-on-failure` | Passed, 3/3 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_deterministic_no_network_gate --output-on-failure` | Passed, 1/1 |
| `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure` | Passed, 37/37 |
| `python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/p0_precode_validation.py --gate all` | Passed |

## Residual Build Surface Note

`cmake --build build/sbsql_parser_worker_validation -j 4` reached an unrelated `libsb_engine.so` link failure caused by non-PIC static engine objects. The FSPE-012G target, adjacent regression labels, deterministic gate, full `sbsql_parser_worker` CTest label, and P0 validator all passed.

## Result

FSPE-012G is complete. FSPE-013A can open as the next serialized slice.

# Security Redaction and Side-Channel Report

Status: complete
Search key: `FSPE-SECURITY-REDACTION-SIDE-CHANNEL-REPORT`
Owning slice: `FSPE-012G`

## Implemented Test Areas

| Area | Required behavior |
| --- | --- |
| Hidden object lookup | Public name resolution returns the standard not-found/does-not-exist message vector and never reveals hidden UUIDs. |
| Parser cache | Cached SBLR is invalidated or refused when role, grant, search path, language, policy, or descriptor epoch changes. |
| Error fields | Message vectors expose only approved public fields for parser rendering. |
| Timing | Not-found and not-authorized paths use the same externally observable result class where policy requires non-disclosure. |
| Metadata | Result metadata omits hidden columns, policies, indexes, filespaces, security labels, and internal agent state. |
| Diagnostics | Debug-only detail is available only under explicit dev policy and never in normal client profiles. |
| Donor rendering | Donor-profile errors do not leak ScratchBird internal authority unless the profile explicitly permits it. |

## Evidence

- `project/tests/sbsql_parser_worker/generated/security/SECURITY_REDACTION_SIDE_CHANNEL_FIXTURES.csv` records the public, donor, hidden, missing, cache-authority, metadata-projection, expected-message-vector, returned-field, elapsed-time-class, and closure-status rows.
- `project/tests/sbsql_parser_worker/generated/security/sbsql_security_redaction_side_channel_gate.cpp` validates parser diagnostic redaction, server diagnostic redaction, SBPS message-vector redaction, public result metadata projection, cache authority key dimensions, prepared-statement staleness, and hidden-as-missing source behavior.
- `project/src/wire/parser_server_ipc/parser_ipc_common.cpp` and `project/src/server/diagnostics.cpp` enforce public diagnostic field filtering for canonical UUIDs, hidden names, paths, policy IDs, provider details, credentials, and internal fields while preserving safe sentinel fields.
- `project/src/parsers/sbsql_worker/rendering/rendering.cpp` projects public result payloads without hidden/system result metadata.
- `project/src/parsers/sbsql_worker/cache/sblr_template_cache.*`, `project/src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp`, `project/src/server/session_registry.hpp`, and `project/src/server/sblr_dispatch_server.cpp` carry grant, role-set, group-set, descriptor, search-path, language, and policy dimensions through cache keys and prepared-statement stale checks.

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

## Closure Rule

Any UUID/name/security-policy leakage in a public parser-facing response is a blocker.

## Result

FSPE-012G is complete. FSPE-013A can open as the next serialized slice.

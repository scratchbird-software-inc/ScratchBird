# FSPE-010B7 Validation Result

Status: complete
Search key: `FSPE_010B7_VALIDATION_RESULT`

## Scope

This artifact closes `FSPE-010B7`: warning chains and partial-result diagnostics on successful streaming result paths. It does not close `FSPE-010B8` or `FSPE-010B9`.

The implemented path is a server-admitted SBLR warning/partial-result cursor contract. It does not allow SQL text, reference command text, parser AST, or BoundAST to become engine authority.

## Validated Coverage

- Server warning/partial-result cursors return visible partial result rows through bounded fetches.
- Warning-chain events are carried as non-aborting diagnostics with stable diagnostic codes.
- Finality is explicit: successful partial streams end with `completed_with_warnings` and cursor end-of-stream metadata.
- Parser full route can open a warning/partial-result cursor and render partial rows, warning diagnostics, and finality through listener -> parser -> server.
- Existing B1-B6 streaming, chunking, COPY, and multi-result gates still pass after B7.

## Implementation Evidence

- `project/src/server/session_registry.hpp` with search key `warning_stream_kind`.
- `project/src/server/sblr_admission.cpp` with search key `warning_chain_count`.
- `project/src/server/sblr_dispatch_server.cpp` with search key `WarningStreamPacket`.
- `project/src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp` with search key `WARNING STREAM`.
- `project/tests/sbsql_parser_worker/sbsql_warning_partial_result_conformance.cpp` with search key `sbsql_warning_partial_result_conformance`.
- `project/tests/sbsql_parser_worker/sbsql_full_route_execution_smoke.cpp` with search key `WARNING STREAM`.
- `project/tests/sbsql_parser_worker/CMakeLists.txt` with search key `sbsql_warning_partial_result_conformance`.

## Validation Commands

Passed:

```bash
cmake -S project -B build/sbsql_parser_worker_validation
cmake --build build/sbsql_parser_worker_validation --target sbsql_warning_partial_result_conformance sbp_sbsql sbp_sbsql_full_route_execution_smoke sb_server sb_listener
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_warning_partial_result_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -E sb_engine_public_abi_symbol_gate --output-on-failure
```

Observed results:

- `sbsql_warning_partial_result_conformance`: 2/2 passed, covering focused server warning/partial-result streaming and full-route parser/server warning stream rendering.
- `sbsql_parser_worker`: 21/21 passed.
- Focused shard excluding `sb_engine_public_abi_symbol_gate`: 32/32 passed.

## Remaining Work

`FSPE-010B8` is now the next ready slice: timeout, cancellation, disconnect, parser-kill, and drain finality for active streams.

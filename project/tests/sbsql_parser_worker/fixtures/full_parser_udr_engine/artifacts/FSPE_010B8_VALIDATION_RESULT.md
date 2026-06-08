# FSPE-010B8 Validation Result

Status: complete
Search key: `FSPE_010B8_VALIDATION_RESULT`

## Scope

This artifact closes `FSPE-010B8`: timeout, cancellation, drain, disconnect, and parser-kill finality for active streams. It does not close the parent `FSPE-010B`; `FSPE-010B9` remains for completion-gate evidence and parent status updates.

The implemented path records server-owned cursor finality. It does not allow SQL text, donor command text, parser AST, or BoundAST to become engine authority.

## Validated Coverage

- Timed-out streams fail closed with `SERVER.STREAM.TIMEOUT`, release active result state, and record `timed_out` cursor finality.
- Draining streams return a deterministic finality event and end-of-cursor metadata.
- Cancelled streams use an explicit close flag and record `cancelled` finality.
- Parser disconnect and parser-kill notices close owned cursors, release engine result state, and record deterministic finality.
- Parser full route covers timeout diagnostics, drain finality rendering, and cancel finality rendering through listener -> parser -> server.
- Existing B1-B7 streaming, chunking, COPY, multi-result, and warning gates still pass after B8.

## Implementation Evidence

- `project/src/server/session_registry.hpp` with search key `finality_kind`.
- `project/src/server/session_registry.cpp` with search key `parser_killed`.
- `project/src/server/sblr_dispatch_server.cpp` with search key `StreamFinalityPacket`.
- `project/src/parsers/sbsql_worker/ipc/sbps_client.hpp` with search key `CancelCursor`.
- `project/src/parsers/sbsql_worker/ipc/sbps_client.cpp` with search key `kCursorCloseFlagCancel`.
- `project/src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp` with search key `TIMEOUT STREAM`.
- `project/tests/sbsql_parser_worker/sbsql_stream_finality_conformance.cpp` with search key `sbsql_stream_finality_conformance`.
- `project/tests/sbsql_parser_worker/sbsql_full_route_execution_smoke.cpp` with search key `TIMEOUT STREAM`.
- `project/tests/sbsql_parser_worker/CMakeLists.txt` with search key `sbsql_stream_finality_conformance`.

## Validation Commands

Passed:

```bash
cmake -S project -B build/sbsql_parser_worker_validation
cmake --build build/sbsql_parser_worker_validation --target sbsql_stream_finality_conformance sbp_sbsql sbp_sbsql_full_route_execution_smoke sb_server sb_listener
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_stream_finality_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -E sb_engine_public_abi_symbol_gate --output-on-failure
```

Observed results:

- `sbsql_stream_finality_conformance`: 2/2 passed, covering focused server finality and full-route parser/server finality rendering.
- `sbsql_parser_worker`: 22/22 passed.
- Focused shard excluding `sb_engine_public_abi_symbol_gate`: 33/33 passed.

## Remaining Work

`FSPE-010B9` is now the next ready slice: completion gate and parent evidence update for `FSPE-010B`.

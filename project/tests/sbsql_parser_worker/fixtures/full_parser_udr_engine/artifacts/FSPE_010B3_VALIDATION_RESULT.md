# FSPE-010B3 Validation Result

Status: complete
Search key: `FSPE_010B3_VALIDATION_RESULT`

## Scope

This artifact closes `FSPE-010B3`: parser/client full-route streaming rendering. It does not close `FSPE-010B4` through `FSPE-010B9`.

The implemented route remains parser-boundary safe: the test-wire command submits a canonical binary SBLR operation envelope to the server, the engine executes an internal operation, and no raw SQL text, donor command text, parser AST, or BoundAST is treated as engine authority.

## Validated Coverage

- SBPS client fetch preserves server detail metadata returned with streamed cursor batches.
- Test-wire `FETCH` rendering includes cursor metadata detail before row packet content.
- `ENGINE STREAM` submits a canonical binary SBLR operation envelope for `observability.show_version`, not raw SQL.
- Full route listener -> parser worker -> server -> engine opens an engine-backed cursor, fetches cursor metadata and `observability.show_version` payload content, and closes the cursor.
- Existing `FSPE-010B1` engine-backed row-batch streaming and `FSPE-010B2` server cursor protocol gates still pass after the full-route rendering extension.

## Implementation Evidence

- `project/src/parsers/sbsql_worker/ipc/sbps_client.hpp` with search key `ServerFetchResult`.
- `project/src/parsers/sbsql_worker/ipc/sbps_client.cpp` with search key `FetchCursor`.
- `project/src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp` with search key `ENGINE STREAM`.
- `project/src/parsers/sbsql_worker/CMakeLists.txt` with search key `sbl_sbsql_parser_worker_core`.
- `project/tests/sbsql_parser_worker/sbsql_full_route_execution_smoke.cpp` with search key `ENGINE STREAM`.
- `project/tests/sbsql_parser_worker/CMakeLists.txt` with search key `sbsql_full_route_streaming_conformance`.

## Validation Commands

Passed:

```bash
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_full_route_execution_smoke
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql
cmake --build build/sbsql_parser_worker_validation --target sb_server
cmake --build build/sbsql_parser_worker_validation --target sb_listener
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_full_route_streaming_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sb_server_cursor_protocol_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sb_engine_backed_streaming_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -E sb_engine_public_abi_symbol_gate --output-on-failure
```

Observed results:

- `sbsql_full_route_streaming_conformance`: 1/1 passed.
- `sb_server_cursor_protocol_conformance`: 1/1 passed.
- `sb_engine_backed_streaming_conformance`: 2/2 passed.
- `sbsql_parser_worker`: 17/17 passed.
- Focused shard excluding `sb_engine_public_abi_symbol_gate`: 28/28 passed.

## Remaining Work

`FSPE-010B4` is now the next ready slice: chunked SBPS payload assembly for large SBLR, parameter, result, and message-vector payloads.

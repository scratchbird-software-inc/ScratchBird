# FSPE-010B4 Validation Result

Status: complete
Search key: `FSPE_010B4_VALIDATION_RESULT`

## Scope

This artifact closes `FSPE-010B4`: chunked SBPS payload assembly for large SBLR, parameter, result, and message-vector payloads. It does not close `FSPE-010B5` through `FSPE-010B9`.

The implementation is transport-level chunking only. Parser output remains untrusted until server SBLR admission revalidates it, and the engine still executes SBLR/internal procedures rather than SQL text, parser AST, BoundAST, or reference command text.

## Validated Coverage

- SBPS physical frames are split under the negotiated frame ceiling with deterministic stream ID, sequence number, chunk flag, and final flag handling.
- Parser client request sending chunks large execute payloads instead of emitting unsafe oversize frames.
- Server IPC request handling assembles chunked physical frames before session-bound message dispatch.
- Server responses and diagnostic message-vector payloads can be chunked and reassembled by the parser client.
- Large SBLR envelope fields, parser-side parameter packet fields, execute result row packets, and server fetch/result detail fields use an extended length encoding that preserves values beyond the legacy 16-bit string limit.
- Full route listener -> parser -> server validates a >1 MiB parser-generated SBLR/parameter request and a >1 MiB execute result response through the SBPS client.
- Large message-vector payloads chunk and preserve first/final diagnostic codes after assembly.

## Implementation Evidence

- `project/src/server/sbps.hpp` with search key `kFlagPayloadChunk`.
- `project/src/server/sbps.cpp` with search key `EncodeFrameSequence`.
- `project/src/server/ipc_server.cpp` with search key `AssembleChunkedFrame`.
- `project/src/server/sblr_dispatch_server.cpp` with search key `kLongStringSentinel`.
- `project/src/parsers/sbsql_worker/ipc/sbps_client.cpp` with search key `AssembleChunkedFrame`.
- `project/src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp` with search key `SBPS CHUNKED EXECUTE`.
- `project/tests/sbsql_parser_worker/sbsql_sbps_chunked_payload_conformance.cpp` with search key `sbps_chunked_payload_conformance`.
- `project/tests/sbsql_parser_worker/sbsql_full_route_execution_smoke.cpp` with search key `SBPS CHUNKED EXECUTE`.
- `project/tests/sbsql_parser_worker/CMakeLists.txt` with search key `sbps_chunked_payload_conformance`.

## Validation Commands

Passed:

```bash
cmake -S project -B build/sbsql_parser_worker_validation
cmake --build build/sbsql_parser_worker_validation --target sbps_chunked_payload_conformance
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql sbp_sbsql_full_route_execution_smoke sb_server sb_listener
ctest --test-dir build/sbsql_parser_worker_validation -L sbps_chunked_payload_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sb_engine_backed_streaming_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sb_server_cursor_protocol_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_full_route_streaming_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -E sb_engine_public_abi_symbol_gate --output-on-failure
```

Observed results:

- `sbps_chunked_payload_conformance`: 2/2 passed, covering the focused chunked payload test and full-route chunked parser/server round trip.
- `sbsql_parser_worker`: 18/18 passed.
- `sb_engine_backed_streaming_conformance`: 2/2 passed.
- `sb_server_cursor_protocol_conformance`: 1/1 passed.
- `sbsql_full_route_streaming_conformance`: 1/1 passed.
- Focused shard excluding `sb_engine_public_abi_symbol_gate`: 29/29 passed.

## Remaining Work

`FSPE-010B5` is now the next ready slice: COPY/import/export/load streaming, progress diagnostics, reject records, bulk summaries, and final status through parser/server/engine routes.

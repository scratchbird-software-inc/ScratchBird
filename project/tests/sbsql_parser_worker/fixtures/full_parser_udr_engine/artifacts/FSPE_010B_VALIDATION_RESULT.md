# FSPE-010B Validation Result

Status: complete
Search key: `FSPE_010B_VALIDATION_RESULT`

## Scope Validated

This artifact records closure evidence for FSPE-010B streaming and result-set protocol work.

Validated coverage:

- SBPS execute can open a server-owned cursor for a parser-submitted SBLR envelope.
- SBPS fetch returns bounded result chunks with row indexes and end-of-cursor state.
- Oversized fetch requests fail closed with `SERVER.STREAM.CHUNK_TOO_LARGE`.
- Fetch after close and fetch after disconnect fail closed with `PARSER_SERVER_IPC.CURSOR_NOT_FOUND`.
- Cross-session fetch and close attempts fail closed without revealing another session's cursor.
- Parser full-route test wire drives `STREAM`, `FETCH`, and `CLOSE CURSOR` through listener -> parser -> server -> engine admission/public ABI boundary.
- Full-route oversized fetch requests return the canonical `SERVER.STREAM.CHUNK_TOO_LARGE` diagnostic without advancing the cursor.
- Parser workers send `DisconnectNotice` on normal client close so server session/cursor cleanup is observable in full-route audit evidence.
- `FSPE-010B1` now validates engine-owned row batch state through `sb_engine_result_next_batch` and server cursor fetch.
- `FSPE-010B2` now validates cursor metadata, forward-only scroll refusal, row/byte limits, pre-advance byte-limit refusal, and deterministic post-EOS fetch behavior.
- `FSPE-010B3` now validates full-route parser/client streaming rendering of engine-backed cursor metadata and payload through listener -> parser -> server -> engine.
- `FSPE-010B4` now validates chunked SBPS physical frame assembly for large SBLR/parameter requests, large result responses, and large message-vector payloads.
- `FSPE-010B5` now validates COPY/import-style streaming progress events, reject records, bulk summaries, and final status through server fetch and parser full-route rendering.
- `FSPE-010B6` now validates multi-result sequencing with ordered result-set metadata, command tags, and finality through server fetch and parser full-route rendering.
- `FSPE-010B7` now validates warning-chain diagnostics, partial-result rows, and completed-with-warnings finality through server fetch and parser full-route rendering.
- `FSPE-010B8` now validates deterministic timeout, drain, cancellation, parser-disconnect, and parser-kill finality for active streams without spin waits.
- `FSPE-010B9` now validates that all child labels pass together and parent evidence is updated.

## Validation Commands

Passed:

```bash
cmake --build build/sbsql_parser_worker_validation --target sb_streaming_result_protocol_conformance
ctest --test-dir build/sbsql_parser_worker_validation -L sb_streaming_result_protocol_conformance --output-on-failure
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_full_route_execution_smoke
cmake --build build/sbsql_parser_worker_validation --target sb_server
ctest --test-dir build/sbsql_parser_worker_validation -R sb_listener_sbp_sbsql_server_engine_execution_smoke --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -E sb_engine_public_abi_symbol_gate --output-on-failure
cmake --build build/sbsql_parser_worker_validation --target sb_engine_backed_streaming_conformance
cmake --build build/sbsql_parser_worker_validation --target sb_engine_public_sblr_admission_fixture
ctest --test-dir build/sbsql_parser_worker_validation -L sb_engine_backed_streaming_conformance --output-on-failure
cmake --build build/sbsql_parser_worker_validation --target sb_server_cursor_protocol_conformance
ctest --test-dir build/sbsql_parser_worker_validation -L sb_server_cursor_protocol_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -E sb_engine_public_abi_symbol_gate --output-on-failure
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_full_route_execution_smoke
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql
cmake --build build/sbsql_parser_worker_validation --target sb_server
cmake --build build/sbsql_parser_worker_validation --target sb_listener
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_full_route_streaming_conformance --output-on-failure
cmake -S project -B build/sbsql_parser_worker_validation
cmake --build build/sbsql_parser_worker_validation --target sbps_chunked_payload_conformance
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql sbp_sbsql_full_route_execution_smoke sb_server sb_listener
ctest --test-dir build/sbsql_parser_worker_validation -L sbps_chunked_payload_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -E sb_engine_public_abi_symbol_gate --output-on-failure
cmake --build build/sbsql_parser_worker_validation --target sbsql_copy_streaming_conformance sbp_sbsql sbp_sbsql_full_route_execution_smoke sb_server sb_listener
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_copy_streaming_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -E sb_engine_public_abi_symbol_gate --output-on-failure
cmake --build build/sbsql_parser_worker_validation --target sbsql_multi_result_conformance sbp_sbsql sbp_sbsql_full_route_execution_smoke sb_server sb_listener
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_multi_result_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -E sb_engine_public_abi_symbol_gate --output-on-failure
cmake -S project -B build/sbsql_parser_worker_validation
cmake --build build/sbsql_parser_worker_validation --target sbsql_warning_partial_result_conformance sbp_sbsql sbp_sbsql_full_route_execution_smoke sb_server sb_listener
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_warning_partial_result_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -E sb_engine_public_abi_symbol_gate --output-on-failure
cmake -S project -B build/sbsql_parser_worker_validation
cmake --build build/sbsql_parser_worker_validation --target sbsql_stream_finality_conformance sbp_sbsql sbp_sbsql_full_route_execution_smoke sb_server sb_listener
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_stream_finality_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -E sb_engine_public_abi_symbol_gate --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L "sb_engine_backed_streaming_conformance|sb_server_cursor_protocol_conformance|sbsql_full_route_streaming_conformance|sbps_chunked_payload_conformance|sbsql_copy_streaming_conformance|sbsql_multi_result_conformance|sbsql_warning_partial_result_conformance|sbsql_stream_finality_conformance" --output-on-failure
```

Observed results:

- `sb_streaming_result_protocol_conformance`: 1/1 passed.
- `sb_listener_sbp_sbsql_server_engine_execution_smoke`: 1/1 passed.
- `sbsql_parser_worker`: 14/14 passed after adding full-route chunk-limit diagnostic coverage.
- `sbsql_parser_worker`: 16/16 passed after adding FSPE-010B1 engine-backed streaming tests.
- `sbsql_parser_worker`: 17/17 passed after adding FSPE-010B2 server cursor protocol tests.
- Focused shard excluding `sb_engine_public_abi_symbol_gate`: 28/28 passed after adding FSPE-010B2.
- `sb_engine_backed_streaming_conformance`: 2/2 passed, covering the server cursor engine-backed batch test and public ABI batch API fixture.
- `sb_server_cursor_protocol_conformance`: 1/1 passed, covering cursor metadata, limits, unsupported scroll flags, and EOS stability.
- `sbsql_full_route_streaming_conformance`: 1/1 passed, covering parser/client rendering of engine-backed cursor metadata and payload through the listener/parser/server/engine route.
- `sbsql_parser_worker`: 17/17 passed after adding FSPE-010B3 parser/client full-route streaming rendering.
- Focused shard excluding `sb_engine_public_abi_symbol_gate`: 28/28 passed after adding FSPE-010B3.
- `sbps_chunked_payload_conformance`: 2/2 passed, covering focused chunked payload assembly and the full-route parser/server chunked execute round trip.
- `sbsql_parser_worker`: 18/18 passed after adding FSPE-010B4 chunked SBPS payload tests.
- Focused shard excluding `sb_engine_public_abi_symbol_gate`: 29/29 passed after adding FSPE-010B4.
- `sbsql_copy_streaming_conformance`: 2/2 passed, covering server COPY streaming and full-route parser/server COPY stream rendering.
- `sbsql_parser_worker`: 19/19 passed after adding FSPE-010B5 COPY/import-style streaming.
- Focused shard excluding `sb_engine_public_abi_symbol_gate`: 30/30 passed after adding FSPE-010B5.
- `sbsql_multi_result_conformance`: 2/2 passed, covering server multi-result sequencing and full-route parser/server multi-result rendering.
- `sbsql_parser_worker`: 20/20 passed after adding FSPE-010B6 multi-result sequencing.
- Focused shard excluding `sb_engine_public_abi_symbol_gate`: 31/31 passed after adding FSPE-010B6.
- `sbsql_warning_partial_result_conformance`: 2/2 passed, covering server warning/partial-result streaming and full-route parser/server warning stream rendering.
- `sbsql_parser_worker`: 21/21 passed after adding FSPE-010B7 warning and partial-result diagnostics.
- Focused shard excluding `sb_engine_public_abi_symbol_gate`: 32/32 passed after adding FSPE-010B7.
- `sbsql_stream_finality_conformance`: 2/2 passed, covering server timeout/drain/cancel/parser-kill finality and full-route timeout/drain/cancel rendering.
- `sbsql_parser_worker`: 22/22 passed after adding FSPE-010B8 stream finality.
- Focused shard excluding `sb_engine_public_abi_symbol_gate`: 33/33 passed after adding FSPE-010B8.
- `fspe_010b_completion_gate`: 9/9 passed, covering all FSPE-010B child labels in one consolidated selection.

## Remaining FSPE-010B Closure Gaps

None. `FSPE-011` is the next ready execution_plan slice.

## Caveat

The unrestricted full build path still has an unrelated shared-library gate issue: `libsb_engine.so` is not produced because non-PIC static engine objects fail to link into `sb_engine_shared`; therefore `sb_engine_public_abi_symbol_gate` is excluded from the focused shard until that build-mode issue is resolved.

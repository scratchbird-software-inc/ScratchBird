# Streaming Result Protocol Report

Status: complete
Search key: `STREAMING_RESULT_PROTOCOL_REPORT`

## Implemented Protocol Surface

The current FSPE-010B sub-slice establishes a minimal streaming cursor lifecycle across server and parser routes:

- Server cursor records carry operation ID, total row count, next row index, fetch count, and max chunk rows.
- `ExecuteSblr` can create a cursor and defer row delivery to fetch calls.
- `Fetch` returns bounded chunks and preserves cursor progress.
- Oversized fetch requests return `SERVER.STREAM.CHUNK_TOO_LARGE`; full-route smoke verifies the diagnostic and then confirms the cursor can still fetch from the first row.
- `CloseCursor` closes only cursors owned by the requesting session.
- `DisconnectNotice` closes the session's prepared statements and cursors.
- Parser `STREAM`, `FETCH`, and `CLOSE CURSOR` commands drive the SBPS route through full-route smoke coverage.
- SBPS now chunks large physical payloads and reassembles large SBLR, parameter, result, and message-vector payloads without oversize frames.
- Parser full-route smoke includes a chunked execute request/response round trip with large parser-generated SBLR/parameter content and a large result packet.
- COPY/import-style streaming cursors now return progress events, reject records, bulk summaries, and final status through server fetch and parser full-route rendering.
- Multi-result cursors now return ordered result-set metadata, command tags, and finality through server fetch and parser full-route rendering.
- Warning/partial-result cursors now return partial result rows, non-aborting warning-chain diagnostics, and completed-with-warnings finality through server fetch and parser full-route rendering.
- Active stream finality now records deterministic timeout, drain, cancellation, parser-disconnect, and parser-kill outcomes without spin waits.

## Security And Boundary Notes

- Cursor fetch/close now validates session ownership and returns the same safe not-found diagnostic for cross-session attempts.
- Parser output remains untrusted until server admission validates the SBLR envelope.
- The engine is not given SQL text or parser AST authority.
- Synthetic stream row counts are test-only SBLR envelope metadata used to exercise protocol chunking; they do not establish a production large-result engine contract.
- MGA/anti-WAL invariants are unaffected.
- The implementation uses blocking request/response and existing poll-based server accept loops, not spin/busy-wait loops.

## Evidence

Primary tests:

- `sb_streaming_result_protocol_conformance`
- `sb_listener_sbp_sbsql_server_engine_execution_smoke`
- `sbps_chunked_payload_conformance`
- `sbsql_copy_streaming_conformance`
- `sbsql_multi_result_conformance`
- `sbsql_warning_partial_result_conformance`
- `sbsql_stream_finality_conformance`
- `sbsql_parser_worker`

Evidence artifact:

- `FSPE_010B_VALIDATION_RESULT.md`

## Remaining Work

`FSPE_P10B_STREAMING_COMPLETE` is closed by `FSPE-010B1` through `FSPE-010B9`:

- `FSPE-010B1`: Engine-backed row batch streaming. Complete; evidence in `FSPE_010B1_VALIDATION_RESULT.md`.
- `FSPE-010B2`: Server cursor protocol completion. Complete; evidence in `FSPE_010B2_VALIDATION_RESULT.md`.
- `FSPE-010B3`: Parser/client full-route streaming. Complete; evidence in `FSPE_010B3_VALIDATION_RESULT.md`.
- `FSPE-010B4`: Chunked SBPS payload assembly. Complete; evidence in `FSPE_010B4_VALIDATION_RESULT.md`.
- `FSPE-010B5`: COPY/import/export/load streaming. Complete; evidence in `FSPE_010B5_VALIDATION_RESULT.md`.
- `FSPE-010B6`: Multi-result statement sequencing. Complete; evidence in `FSPE_010B6_VALIDATION_RESULT.md`.
- `FSPE-010B7`: Warning chains and partial-result diagnostics. Complete; evidence in `FSPE_010B7_VALIDATION_RESULT.md`.
- `FSPE-010B8`: Timeout/cancel/drain/parser-kill finality. Complete; evidence in `FSPE_010B8_VALIDATION_RESULT.md`.
- `FSPE-010B9`: Completion gate and evidence update. Complete; evidence in `FSPE_010B9_VALIDATION_RESULT.md`.

Next execution_plan slice: `FSPE-011` generated conformance and full-route execution tests.

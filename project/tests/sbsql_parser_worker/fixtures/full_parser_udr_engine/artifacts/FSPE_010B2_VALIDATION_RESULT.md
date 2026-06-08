# FSPE-010B2 Validation Result

Status: complete
Search key: `FSPE_010B2_VALIDATION_RESULT`

## Scope Validated

FSPE-010B2 closes server cursor protocol completion for the current forward-only cursor slice. It does not close parser/client full-route rendering, COPY/import/export/load streaming, chunked payload assembly, multi-result sequencing, warning chains, or cancellation/drain finality.

Validated coverage:

- Fetch payloads support bounded `max_rows`, optional `max_bytes`, and fetch flags.
- Oversized `max_rows` and oversized `max_bytes` fail closed with canonical server diagnostics.
- Too-small client `max_bytes` fails before advancing the cursor.
- Unsupported scroll/fetch flags fail closed with `SERVER.CURSOR.SCROLL_UNSUPPORTED`; current cursor capability is explicitly forward-only.
- Successful fetch responses include cursor metadata in the fetch detail payload.
- End-of-cursor fetches are deterministic and return empty EOS batches after exhaustion.
- Existing engine-backed and synthetic streaming cursor gates continue to pass.

## Implementation Evidence

Code paths updated:

- `project/src/server/sblr_dispatch_server.hpp` and `project/src/server/sblr_dispatch_server.cpp`: test fetch payloads now materialize `max_bytes` and fetch flags; fetch handling validates row, byte, and forward-only cursor constraints; fetch results carry cursor metadata detail.
- `project/src/server/session_registry.hpp`: cursor records now carry a server byte-limit contract.
- `project/tests/sbsql_parser_worker/sbsql_server_cursor_protocol_conformance.cpp`: validates cursor metadata, byte limits, unsupported scroll flags, EOS stability, and max-row refusal.
- `project/tests/sbsql_parser_worker/CMakeLists.txt`: adds the runnable `sb_server_cursor_protocol_conformance` CTest target and label.

## Validation Commands

Passed:

```bash
cmake --build build/sbsql_parser_worker_validation --target sb_server_cursor_protocol_conformance
ctest --test-dir build/sbsql_parser_worker_validation -L sb_server_cursor_protocol_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sb_streaming_result_protocol_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sb_engine_backed_streaming_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation -E sb_engine_public_abi_symbol_gate --output-on-failure
```

Observed results:

- `sb_server_cursor_protocol_conformance`: 1/1 passed.
- `sb_streaming_result_protocol_conformance`: 1/1 passed.
- `sb_engine_backed_streaming_conformance`: 2/2 passed.
- `sbsql_parser_worker`: 17/17 passed.
- Focused shard excluding `sb_engine_public_abi_symbol_gate`: 28/28 passed.

## Remaining Parent Scope

`FSPE-010B3` is now ready for assignment. Parent `FSPE-010B` remains open for parser/client full-route streaming rendering, chunked SBPS payload assembly, COPY/import/export/load streaming, multi-result sequencing, warning and partial-result diagnostics, timeout/cancel/drain finality, and final completion evidence.

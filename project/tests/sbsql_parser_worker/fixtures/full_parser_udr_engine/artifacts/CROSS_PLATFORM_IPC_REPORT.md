# Cross-Platform IPC Report

Status: complete
Search key: `FSPE-CROSS_PLATFORM_IPC_REPORT`
Owning slice: `FSPE-012D`

## Summary

FSPE-012D materialized `sbsql_cross_platform_ipc_gate` as a parser/listener/server IPC and process-boundary CTest gate.

The gate verifies:

- Listener control-plane payload round trips and fail-closed diagnostics for `HELLO`, `HANDOFF_SOCKET`, `HANDOFF_ACK`, `SESSION_BINDING_REPORT`, and `TAKEOVER_REQUEST`.
- SBPS chunked frame behavior rejects oversized physical frames and preserves bounded per-frame decoding.
- Parser SBPS client endpoint parsing rejects overlong Unix endpoint paths with `PARSER_SERVER_IPC.ENDPOINT_PATH_TOO_LONG`.
- Parser child launch policy preserves the explicit environment whitelist, inherited-handle contract, handle limit, and process count.
- Server, listener, parser-runtime, and SBPS-client sources retain platform/path-limit, handle-passing, stale-owner cleanup, endpoint descriptor, and exact unsupported-platform diagnostics.

## Gate

| Field | Value |
| --- | --- |
| CTest label | `sbsql_cross_platform_ipc_gate` |
| Test source | `project/tests/sbsql_parser_worker/generated/platform/sbsql_cross_platform_ipc_gate.cpp` |
| CMake target | `sbsql_cross_platform_ipc_gate` |
| Supporting labels | `sbsql_platform_ipc_gate`; `sbsql_parser_worker` |
| Unexpected failures | 0 |

## Coverage

The gate combines runtime payload checks with static source-contract checks so the current host can validate both exercised Unix behavior and exact-refusal contracts for unsupported platform paths.

Validated search keys and contracts:

- `project/src/listener/control_plane.cpp`: `SCM_RIGHTS`, `CMSG_SPACE`, bounded payload and read-timeout diagnostics.
- `project/src/listener/parser_pool.cpp`: Unix `socketpair`, listener control FD, server endpoint environment passing, and exec fail-closed exit path.
- `project/src/listener/listener_runtime.cpp`: management socket path-limit, unavailable socket, and Windows unsupported diagnostics.
- `project/src/listener/listener_socket_identity.cpp`: live-owner detection and clean-shutdown token behavior.
- `project/src/server/ipc_server.cpp`: SBPS endpoint descriptor format, Unix transport descriptor, path-limit guard, descriptor write diagnostic, and Windows platform branch.
- `project/src/server/listener_orchestrator.cpp`: listener platform unavailable, invalid management socket, and path-limit guard diagnostics.
- `project/src/parsers/sbsql_worker/runtime/parser_runtime.cpp`: listener control FD, parser IPC endpoint, missing-client-FD diagnostic, and Windows platform branch.
- `project/src/parsers/sbsql_worker/ipc/sbps_client.cpp`: unsupported-platform, endpoint path-limit, Unix endpoint prefix, and socket path bounds.

## Boundary

The gate does not claim Windows named-pipe support is implemented. It proves current Unix IPC behavior is bounded and that unsupported or not-yet-implemented platform paths fail closed with explicit diagnostics instead of silent fallback.

## Result

Cross-platform parser/listener/server path, process, inherited-handle, endpoint, and unsupported-platform behavior is now represented by a runnable validation gate and included in the full parser-worker CTest label.

# Wire Protocol: SBWP

## Purpose

This page describes the ScratchBird Wire Protocol (SBWP), version `sbwp_v1_1`. It covers the
protocol's role in the connection stack, what version `sbwp_v1_1` means, the framing model as
visible in source, the feature-bit negotiation mechanism, and the relationship between SBWP and
the internal SBPS (parser-to-server) IPC layer.

All values on this page are verified against
`project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp`. Frame byte layouts that cannot
be confirmed from available source are noted as unverified and omitted.

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## Role

SBWP is the client-to-server wire protocol. Every standard ScratchBird driver and adaptor
communicates with the engine over SBWP. It runs over TCP (TLS-wrapped by default under the
`scratchbird_tls_1_3_floor` profile) and mediates:

- session startup and authentication
- query execution (simple and extended query protocols)
- transaction control
- result streaming
- cancellation and termination
- optional capabilities negotiated at startup (compression, streaming, SBLR execution, etc.)

SBWP is distinct from SBPS (the parser-to-server IPC protocol), which is an internal
communication path between the sbsql_worker parser process and the core engine. Client
drivers do not speak SBPS. The `src/wire/` tree contains components used by both SBWP and
SBPS (result batch transfer, streaming cursor manager, etc.).

---

## Version

| Manifest Value | Meaning |
| --- | --- |
| `sbwp_v1_1` | SBWP major version 1, minor version 1 |

Source: `DriverPackageManifest.csv` column `wire_protocol_set` — `sbwp_v1_1` for all 21
drivers, 12 adaptors, and the CLI tool.

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp`:

```cpp
constexpr std::uint8_t kSbwpMajor = 1;
constexpr std::uint8_t kSbwpMinor = 1;
constexpr std::uint16_t kSbwpVersionMin  = 0x0100;  // 1.0 — minimum accepted
constexpr std::uint16_t kSbwpVersionCurrent = 0x0101;  // 1.1 — current
```

The server accepts connections from clients presenting version `0x0100` (1.0) or higher. The
current version is `0x0101` (1.1).

---

## Frame Header

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp`:

```cpp
constexpr std::size_t kSbwpHeaderSize = 40;
constexpr std::uint32_t kMaxPayloadBytes = 64u * 1024u * 1024u;  // 64 MiB
```

Each SBWP frame has a 40-byte header. Maximum payload per frame is 64 MiB.

Frame flags (from the header):

| Flag Constant | Value | Meaning |
| --- | --- | --- |
| `kFrameFlagCompressed` | bit 0 | Payload is compressed |
| `kFrameFlagPartial` | bit 1 | Frame is a partial (continuation) frame |

Unknown flag bits outside `kFrameFlagKnownMask` should be treated as a protocol error by
conformant clients.

The exact byte layout of the 40-byte header (field positions, endianness) is not documented
here as the source shows only the size constant and not the full struct layout accessible in
this audit.

---

## Message Types

The following message type codes are defined in source. Client-to-server messages are in the
`0x01`–`0x2F` range; server-to-client messages are in the `0x40`–`0x81` range.

### Client-to-Server Messages

| Constant | Code | Purpose |
| --- | --- | --- |
| `kStartup` | `0x01` | Initiate a session |
| `kAuthResponse` | `0x02` | Authentication credential response |
| `kQuery` | `0x03` | Simple query execution |
| `kParse` | `0x04` | Extended query: parse a statement |
| `kBind` | `0x05` | Extended query: bind parameters |
| `kDescribe` | `0x06` | Extended query: describe a portal or statement |
| `kExecute` | `0x07` | Extended query: execute a portal |
| `kClose` | `0x08` | Extended query: close a portal or statement |
| `kSync` | `0x09` | Extended query: synchronize pipeline |
| `kCancel` | `0x0b` | Cancel an in-progress request |
| `kTerminate` | `0x0c` | Terminate the session |
| `kCopyData` | `0x0d` | COPY data payload |
| `kCopyDone` | `0x0e` | COPY transfer complete |
| `kCopyFail` | `0x0f` | COPY transfer failed |
| `kSblrExecute` | `0x10` | Execute an SBLR bytecode payload directly |
| `kSubscribe` | `0x11` | Subscribe to notifications |
| `kUnsubscribe` | `0x12` | Unsubscribe from notifications |
| `kStreamControl` | `0x14` | Streaming flow control |
| `kTxnBegin` | `0x15` | Begin a transaction |
| `kTxnCommit` | `0x16` | Commit a transaction |
| `kTxnRollback` | `0x17` | Roll back a transaction |
| `kTxnSavepoint` | `0x18` | Create a savepoint |
| `kTxnRelease` | `0x19` | Release a savepoint |
| `kTxnRollbackTo` | `0x1a` | Roll back to a savepoint |
| `kPing` | `0x1b` | Liveness ping |
| `kSetOption` | `0x1c` | Set a session option |
| `kResetSession` | `0x21` | Reset session state |
| `kReauth` | `0x22` | Re-authenticate on existing connection |
| `kTraceContext` | `0x23` | Attach a trace/telemetry context |

### Server-to-Client Messages

| Constant | Code | Purpose |
| --- | --- | --- |
| `kAuthRequest` | `0x40` | Server auth challenge |
| `kAuthOk` | `0x41` | Authentication succeeded |
| `kReady` | `0x43` | Server ready for next command |
| `kRowDescription` | `0x44` | Column metadata for a result set |
| `kDataRow` | `0x45` | A data row |
| `kCommandComplete` | `0x46` | Command finished |
| `kError` | `0x48` | Error diagnostic |
| `kParseComplete` | `0x4a` | Parse step complete |
| `kBindComplete` | `0x4b` | Bind step complete |
| `kCloseComplete` | `0x4c` | Close complete |
| `kParameterStatus` | `0x4f` | Session parameter status |
| `kParameterDescription` | `0x50` | Parameter type description |
| `kCopyInResponse` | `0x51` | Server ready to receive COPY data |
| `kNotification` | `0x54` | Async notification |
| `kPong` | `0x5d` | Liveness pong (response to ping) |
| `kQueryProgress` | `0x60` | Query progress update |
| `kServerInfo` | `0x61` | Server information |
| `kStateNotification` | `0x62` | Session state change notification |
| `kCancelAck` | `0x65` | Cancel acknowledged |
| `kCancelled` | `0x66` | Operation was cancelled |
| `kMultiResultBegin` | `0x67` | Begin of multi-result response |
| `kMultiResultEnd` | `0x68` | End of multi-result response |
| `kGeneratedKeys` | `0x69` | Generated keys from INSERT/UPDATE |
| `kOutParameters` | `0x6a` | Callable out-parameters |
| `kBatchResult` | `0x6b` | Batch execution result |
| `kPipelineStatus` | `0x6c` | Pipeline execution status |
| `kArrayBindStatus` | `0x6d` | Array bind status |
| `kBulkRejectData` | `0x6e` | Bulk operation reject data |
| `kLobLocator` | `0x6f` | LOB locator reference |
| `kLobChunk` | `0x70` | LOB data chunk |
| `kLobClose` | `0x71` | LOB handle close |
| `kCursorStatus` | `0x72` | Cursor state update |
| `kFailoverHint` | `0x73` | Failover endpoint hint |
| `kHeartbeat` | `0x80` | Server heartbeat |
| `kExtension` | `0x81` | Protocol extension frame |

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp` — `enum Msg` definition.

---

## Ready Reasons

The `kReady` message carries a reason code indicating why the server became ready:

| Constant | Value | Trigger |
| --- | --- | --- |
| `kStartup` | `1` | Session startup complete |
| `kCommandComplete` | `2` | Previous command completed |
| `kErrorRecovered` | `3` | Error was recovered; pipeline continues |
| `kResetComplete` | `4` | Session reset complete |
| `kReauthComplete` | `5` | Re-authentication complete |
| `kCancelOutcome` | `6` | Cancel request outcome delivered |
| `kStateChange` | `7` | Session state changed |

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp` — `enum class ReadyReason`.

---

## Feature Negotiation

During startup, the client and server negotiate optional capabilities using a feature-bit
field. Each bit represents a capability the client wishes to use. The server's response
indicates which capabilities it will honor.

| Feature Constant | Bit | Capability |
| --- | --- | --- |
| `kFeatureCompression` | 0 | Frame-level payload compression |
| `kFeatureStreaming` | 1 | Streaming result windows |
| `kFeatureSblr` | 2 | Direct SBLR bytecode execution (`kSblrExecute`) |
| `kFeatureNotifications` | 4 | Async notifications (`kNotification`) |
| `kFeatureBatch` | 6 | Batch execution (`kBatchResult`) |
| `kFeaturePipeline` | 7 | Pipelined execution (`kPipelineStatus`) |
| `kFeatureBinaryCopy` | 8 | Binary COPY format |
| `kFeatureSavepoints` | 9 | Savepoint support |
| `kFeatureMultiResult` | 13 | Multi-result responses |
| `kFeatureGeneratedKeys` | 14 | Generated key return |
| `kFeatureOutParameters` | 15 | Callable out-parameters |
| `kFeatureArrayBind` | 16 | Array parameter binding |
| `kFeatureBulkRejects` | 17 | Bulk operation reject reporting |
| `kFeatureLobLocator` | 18 | LOB locator references |
| `kFeatureCursors` | 19 | Cursor control |
| `kFeatureCopyBackpressure` | 20 | COPY backpressure flow control |
| `kFeatureSessionReset` | 21 | Session reset (`kResetSession`) |
| `kFeatureReauth` | 22 | Re-authentication on existing connection |
| `kFeatureFailoverHints` | 23 | Failover endpoint hints |
| `kFeatureTraceContext` | 24 | Trace context attachment |

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp` — `constexpr uint64_t kFeature*` definitions.

---

## SBWP and SBPS

SBWP is the **external** wire protocol. SBPS (ScratchBird Parser-Server Protocol) is the
**internal** IPC protocol between the sbsql_worker parser process and the core engine server.
Client drivers speak SBWP only; they never interact with SBPS directly.

The `src/wire/` directory contains shared components that serve both paths:
streaming result windows (`streaming_result_window`), result batch transfer
(`result_batch_transfer`), streaming cursor manager (`streaming_cursor_manager`), and direct
binary result frames (`direct_binary_result_frame`). These are engine internals.

The `src/wire/parser_server_ipc/` subdirectory contains the SBPS IPC layer
(`parser_server_ipc.cpp`, `parser_ipc_common.cpp`). This is also an engine internal.

---

## Cross-References

- [connection_and_dsn.md](connection_and_dsn.md) — session opening sequence that uses STARTUP
- [authentication.md](authentication.md) — AuthRequest/AuthResponse exchange over SBWP
- [tls_profiles.md](tls_profiles.md) — TLS layer under which SBWP frames run
- [diagnostics_and_sqlstate.md](diagnostics_and_sqlstate.md) — how `kError` messages map to SQLSTATE

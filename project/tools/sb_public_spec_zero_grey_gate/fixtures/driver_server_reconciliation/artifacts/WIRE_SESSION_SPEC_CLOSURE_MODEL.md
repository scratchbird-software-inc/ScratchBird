# Wire And Session Contract Closure Model

Search key: `DRIVER-SERVER-RECONCILIATION-WIRE-SESSION-SPEC-CLOSURE-MODEL`.

## Required Native Wire Surfaces

| Surface | Required contents |
| --- | --- |
| Startup | Version, feature flags, connect key/value list, auth method hint, trace context, application identity, default schema/catalog, charset, timezone. |
| Server info | Product/version, server UUID, database UUID, parser profile, protocol profile, feature bitmap, auth surface summary. |
| Session state | ReadyForQuery, transaction state, session UUID, backend PID equivalent, security generation, catalog generation, metadata epochs. |
| Liveness | Ping/Pong opcodes, timeout behavior, keepalive configuration, stale connection diagnostic. |
| Reset | RESET_SESSION request/response, cleanup scope, prepared statement handling, cursor handling, transaction outcome, pool return rule. |
| Reauth | REAUTH/token refresh request/response, method restrictions, accepted/denied diagnostics, generation update. |
| Cancel | Out-of-band cancel request, ownership proof, backend key/session key, cancel outcome, SQLSTATE mapping. |
| Close | Graceful disconnect request/ack, forced abort behavior, drain/shutdown client notifications. |
| Notifications | Channel, payload, sender identity, connection state, prepared statement changed, server shutdown/drain states. |
| Tracing | W3C `traceparent` and `tracestate` transport in startup and per request. |

## Required Execution Surfaces

| Surface | Required contents |
| --- | --- |
| Prepare/describe | ParameterDescription byte layout, result descriptor layout, metadata bitmap, extension slots. |
| Bind | ParameterDataPacket layout for positional/named, null, precision, scale, inferred type, output slots, array-bind. |
| Execute | Request id, portal/cursor id, row limit, timeout, idempotency, transaction expectation. |
| Results | RowDescription, DataRow, CommandComplete, EmptyResponse, generated keys, OUT/INOUT, multi-result sequencing. |
| Bulk/copy | COPY/BULK modes, per-row reject events, progress, summary, final status, redaction digest format. |
| LOB | Locator open/read/write/seek/free, chunk offsets, bounded memory, transaction/locator lifetime. |
| Cursors | Forward/scrollable, holdable, sensitivity, concurrency, updatable refusal or implementation. |
| Pipeline | Batch payload, pipeline error mode, Sync behavior, backpressure/StreamControl. |

## Implementation Readiness Rule

No wire implementation slice may be marked ready until its message family has a
byte-level spec, invalid-state diagnostic, feature negotiation bit, and CTest
route test plan.

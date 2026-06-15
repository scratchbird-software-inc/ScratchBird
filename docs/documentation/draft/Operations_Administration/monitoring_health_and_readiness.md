# Monitoring, Health, And Readiness

## Purpose

An administrator needs three different answers from a running ScratchBird deployment: "Is it alive?", "Is it ready to take work?", and "Is it healthy?". These are distinct questions with distinct answers, and conflating them leads to bad operational decisions — for example, sending traffic to a node that is alive but draining, or restarting a node that is healthy but temporarily not accepting new connections because it is recovering from a transaction pressure spike.

This chapter defines the observable states, the control-plane operations that expose them, the drain mechanism, the transaction cleanup horizon signal, and the metrics infrastructure.

---

## The Four Operational States

| State | Meaning | What it implies |
|---|---|---|
| **Liveness** | The listener process is running and its control plane is responsive | The process has not crashed; management commands are accepted |
| **Readiness** | The listener is ready to accept and process client connections | New sessions should be routed here |
| **Health** | The listener and its required dependencies (parser pool, database attachments, transaction cleanup) are in acceptable operating condition | No degraded subsystem is impairing service |
| **Drain** | The listener is intentionally refusing new connections while existing sessions finish | Do not route new sessions; expect graceful shutdown after the drain window |

These states can be combined: a listener can be live and healthy but in drain (expected during a rolling restart). A listener can be live but not ready (still initializing the parser pool). Understanding the combination tells an operator what action to take.

---

## Control-Plane Operations

The listener exposes its state through a set of management operations carried over the internal control plane. These are not HTTP endpoints — they are management-channel operations delivered via `kManagementCommand` (opcode `0x0060`) frames. The manager node translates higher-level management calls into these operations.

| Operation | Listener Command | Effect |
|---|---|---|
| `listener.status` / `listener.list` | `STATUS` | Returns current listener state snapshot (non-mutating) |
| `listener.drain` | `DRAIN` | Signals the listener to stop accepting new connections |
| `listener.undrain` | `UNDRAIN` | Reverses a drain; listener resumes accepting connections |
| `listener.start` / `listener.restart` | `POOL RESTART` | Restarts the parser pool |
| `listener.stop` | `STOP GRACEFUL` or `STOP FORCE` | Graceful shutdown or forced shutdown |
| `listener.reload` | `RELOAD` | Reloads configuration without stopping |

Source: `src/manager/node/manager_listener_control.cpp:199-231`.

### Health Check Opcode

The control-plane health check opcode is `kHealthCheck = 0x0030`. A `kHealthReport` response (`0x0031`) carries a `state` byte and a `last_error` uint32. Operators using lower-level diagnostic tools can observe this opcode directly.

Source: `src/listener/control_plane.hpp:29-31`.

### PING, STATUS, HEALTH

Within the management envelope, the following operation names map to the health domain:

- `PING` — minimal liveness probe
- `STATUS` — state snapshot
- `HEALTH` — full health report

`PING` and `HEALTH` both map to the `SBLR_BRIDGE_HEALTH` opcode when routed through the UDR bridge.

Source: `src/listener/control_plane.cpp:240-244`.

---

## Drain and the Graceful Drain Timeout

When a drain is initiated, the listener stops accepting new connections. Existing sessions are allowed to complete. The listener waits up to `graceful_drain_timeout_ms` milliseconds for all parser workers to finish their current work. The default value is `30000` (30 seconds).

This value is configurable:

```
graceful_drain_timeout_ms = <unsigned integer milliseconds>
```

If the timeout expires before all workers have finished, the drain may be interrupted depending on whether `STOP FORCE` or a graceful stop was requested.

Source: `src/listener/listener_config.hpp:98`, `src/listener/listener_config.cpp:183-185`.

### Reading Drain State

The listener support bundle snapshot records `draining` (boolean) and `stop_requested` (boolean) fields. These are the primary observability signals for drain state.

The listener metrics (`ListenerMetrics`) record named counters and gauges that can be observed via the `metrics_json` field in the status output. Specific counter names are dynamically registered and may vary by build configuration; the `ToJson()` method serializes the current set.

Source: `src/listener/listener_metrics.hpp`, `src/listener/listener_support_bundle.hpp:34-51`.

---

## Transaction Cleanup Horizon

The **transaction cleanup horizon** is an internal marker indicating how far the MVCC garbage collector has advanced. When the horizon is healthy, old row versions are being cleaned up and storage pressure is manageable. When the horizon stalls — typically because a long-running transaction is holding the oldest transaction ID — cleanup cannot advance past it.

Operators should be aware of the cleanup horizon when observing storage growth that does not correspond to new data. A stalled cleanup horizon is a signal that:

1. A long-running or abandoned transaction is present.
2. Storage version cleanup agents cannot reclaim space past the oldest active transaction.

The `TransactionCleanupHorizonService` (`src/transaction/mga/transaction_cleanup_horizon_service.cpp`) maintains the authoritative horizon. The evidence key `dpc030_authoritative_cleanup_horizon_v1` identifies this horizon in observability output.

---

## Session and Connection Observability

The listener support bundle snapshot records:

| Field | What it tells you |
|---|---|
| `open_connections` | Current number of open sessions |
| `queue_depth` | Depth of the pending work queue |
| `pending_handoff_bindings` | Sessions in handoff negotiation |
| `handoff_complete_total` | Total successful handoffs since last restart |
| `reject_total` | Total rejected connection attempts since last restart |
| `last_accept_sequence` | Monotonic counter of the most recent accepted connection |
| `accepting_new_connections` | Whether the listener is currently accepting |

These fields together let an operator determine whether the listener is saturated, draining, or idle.

---

## Storage Health

Filespace health signals flow through the page-filespace handoff mechanism. The observable states are:

| State | Meaning |
|---|---|
| `approved` | Filespace request approved; operation can proceed |
| `in_flight` | Filespace operation in progress |
| `completed` | Filespace operation completed normally |
| `cancelled` | Filespace operation was cancelled |
| `refused` | Filespace request was explicitly denied |
| `recovery_required` | Filespace is in a state that requires recovery before it can accept new work |

Source: `src/storage/page/page_filespace_handoff.hpp:57-68`.

When `recovery_required` appears in logs or diagnostics, the affected database should not be opened for general use. See [Troubleshooting](troubleshooting.md) and [Database Lifecycle](database_lifecycle.md) for recovery procedures.

---

## Parser Pool Readiness

The listener maintains a pool of parser worker processes. The `ParserPoolStatus` in the support bundle snapshot describes the pool's current state. A pool that has no available workers will refuse new statements with `SBSQL.SERVER.UNAVAILABLE`. Monitor pool status alongside connection counts to identify whether statement unavailability is due to parser saturation or a configuration issue.

---

## Refusal States That Should Alert Operators

Some diagnostic codes indicate a condition that warrants immediate operator attention:

| Code | Urgency | Action |
|---|---|---|
| `FORMAT.VERSION_DOWNGRADE_REFUSED` | High | The database was written by a newer build. Do not open with this build. |
| `SB-STARTUP-STATE-FORMAT-DOWNGRADE-REFUSED` | High | Same; startup state format is from a newer build. |
| `recovery_required` (filespace/page) | High | Database requires recovery before normal operations can resume. |
| `SBSQL.SERVER.UNAVAILABLE` | Medium | Parser server is not reachable; check parser pool health. |
| `UDR.BRIDGE.UNLICENSED` | Medium | An operation requires a license gate; verify build configuration. |
| `SECURITY.AUTHENTICATION.TLS_DOWNGRADE_REFUSED` | High | A client attempted to downgrade TLS; connection refused. |

---

## What You Cannot Observe Without Source

The metrics framework uses named counters and gauges whose names are registered dynamically within the listener. The exact counter names available depend on what the listener records at runtime. Do not assume specific counter names are stable across builds without verifying against the build you are running.

Similarly, the health report state byte (`HealthReportPayload::state`) is a raw value whose interpretation requires the build-specific state enumeration. Treat it as an opaque indicator unless you are reading it with a tool built against the same version.

---

## Related Pages

- [Service Lifecycle](service_lifecycle.md)
- [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md)
- [Troubleshooting](troubleshooting.md)
- [Getting Started: Diagnostics And Support Bundles](../Getting_Started/administration/diagnostics_and_support_bundles.md)

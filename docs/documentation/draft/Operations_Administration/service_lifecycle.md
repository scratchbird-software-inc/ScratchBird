# Service Lifecycle

A running ScratchBird deployment is not a single process — it is a set of cooperating processes (engine server, listener, optional manager) that each move through defined lifecycle states. Understanding those states and the transitions between them is essential for operating the system correctly, diagnosing unexpected exits, and recovering from failures.

This chapter explains the lifecycle of each component, the control-plane operations that drive transitions, how stale endpoints are handled, and what refusal behavior looks like from an operator's perspective.

---

## Lifecycle State Concepts

A **lifecycle state** is a named point in a process's existence. States are written to durable files so that an operator (or a supervising process) can read the current state without needing access to the running process. State transitions are logged to a journal file. Both the state file and the journal are written atomically, with fsync, to reduce the risk of corruption from an unexpected shutdown.

A **terminal state** is one from which no further transitions are possible. A process in a terminal state will not restart itself; external action is required.

**Drain** is a transitional mode in which a service stops accepting new work but completes work already in progress. Draining is the recommended path to a clean shutdown because it allows in-flight sessions to finish. The duration of a drain is bounded by `graceful_drain_timeout_ms` in SBgate or `manager.drain_timeout_ms` in SBmgr.

A **message vector** is the structured diagnostic output produced when something goes wrong. Message vectors are emitted to logs and, in some cases, to support bundles. See [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md) for details.

---

## Manager (SBmgr) Lifecycle States

SBmgr has the most complex lifecycle because it supervises other processes. Its states are defined in `manager_lifecycle.cpp`.

### State Definitions

| State | Meaning |
| --- | --- |
| `created` | Manager object constructed; no action taken yet |
| `args_parsed` | Command-line arguments have been parsed |
| `config_loading` | Configuration file is being read |
| `config_validating` | Configuration has been loaded and is being validated |
| `runtime_preparing` | Runtime directories are being created |
| `owner_acquiring` | Acquiring the owner lock to ensure no other manager is running for this endpoint |
| `daemonizing` | Forking to background (if applicable) |
| `server_endpoint_resolving` | Determining which SBsrv endpoint to supervise |
| `server_supervision_starting` | Spawning SBsrv |
| `listener_endpoint_resolving` | Determining which SBgate endpoint to supervise |
| `listener_supervision_starting` | Spawning SBgate |
| `proxy_binding` | Binding the proxy TCP port (default `3090`) |
| `management_binding` | Binding the management interface |
| `server_heartbeat_starting` | Starting heartbeat to SBsrv |
| `ready` | All supervised processes are running; accepting work |
| `restricted` | Running but with some capability limited |
| `draining` | Stopping new work; waiting for in-flight sessions to complete |
| `stopping` | Shutting down supervised processes |
| `stopped` | *(terminal)* Clean shutdown complete |
| `startup_failed` | *(terminal)* Startup failed before reaching `ready` |
| `failed_terminal` | *(terminal)* Unrecoverable failure after startup |
| `quarantined` | Restart limit exceeded; awaiting operator action |

### Legal Transitions

Transitions follow a strict graph. A manager that attempts an illegal transition emits a `MANAGER.LIFECYCLE_INVALID_TRANSITION` diagnostic and does not complete the transition. The transition graph enforces that:

- Once in a terminal state (`stopped`, `startup_failed`, `failed_terminal`), no further transitions are possible.
- `quarantined` may transition to `draining` or `stopping` but not back to `ready`.
- `ready` and `restricted` may transition to `draining` or `stopping`.
- `draining` may transition to `stopping` or directly to `stopped`.

These constraints are implemented in `ManagerLifecycle::IsLegalTransition` in `manager_lifecycle.cpp`.

### State Persistence

The manager writes two artifacts to `manager.control_dir/`:

- **`sbmn_manager.lifecycle.state`**: the current state, written atomically with fsync and a checksum.
- **`sbmn_manager.lifecycle.journal`**: an append-only log of all state transitions, each record with a checksum.

An operator can inspect these files at any time to understand where a manager is or was in its lifecycle.

---

## IPC Server (SBsrv) Lifecycle

SBsrv's lifecycle is simpler. It starts, opens the database, optionally spawns SBgate, reaches ready, and eventually stops. The server's lifecycle artifacts are written to the `control_dir` configured in `[server.runtime]`.

### Key Artifacts

The `ServerLifecycleArtifacts` struct (from `server/lifecycle.hpp`) records:

| Artifact | Purpose |
| --- | --- |
| `pid_file` | PID of the running server |
| `owner_token_file` | Signed token asserting that this process owns the endpoint |
| `lifecycle_state_file` | Current lifecycle state |
| `lifecycle_journal_file` | Transition history |
| `sbps_endpoint` | Path to the SBPS Unix socket |

These files are written at startup and updated as state changes. On a clean shutdown (`WriteStoppedLifecycleArtifacts`), the lifecycle state is updated to a stopped state. If the server exits uncleanly, the state file will contain the last known state before the crash, which is useful for diagnosis.

### Server Modes

The `ServerMode` enum in `server/config.hpp` defines:

| Mode | Description |
| --- | --- |
| `foreground` | Run in the foreground; the default |
| `service` | Run as a service (behavior depends on the platform) |
| `validation_only` | Validate configuration and exit without opening the database |
| `maintenance` | Run in maintenance mode |
| `read_only` | Open the database in read-only mode |

The configuration template ships `mode = foreground`.

---

## Listener (SBgate) Lifecycle

SBgate has an owner token and a lifecycle state file, both in the `control_dir` it is configured with. The owner token records the listener's UUID, endpoint hash, generation, PID, and startup time. The lifecycle state file records the current state and parser pool status.

### Lifecycle State Files

SBgate writes two artifacts:

- **Owner token** (`*.owner`): a signed key-value file with tamper evidence (`signature_sha256_128`). Contains `pid`, `endpoint_hash`, `generation`, `control_socket`, and `management_socket` paths. Created atomically at startup.
- **Lifecycle state** (`*.lifecycle.state`): a signed file recording `state`, `requested_state`, `listener_uuid`, `profile`, `endpoint_hash`, `generation`, and a JSON snapshot of the parser pool state.

The file names are derived from a stable hash of the protocol family, server endpoint, and database selector, prefixed with the sanitized protocol family name. This means that two listeners with different endpoints or database selectors will produce different artifact filenames.

### Socket Identity and Stale Endpoints

When SBgate starts, it calls `BuildSocketIdentity` to compute paths for its control socket, management socket, owner file, and lifecycle file. The identity is stable across restarts as long as the configuration is unchanged.

Before claiming the owner token, SBgate checks whether a token from a previous instance already exists:

1. The existing token is read and its signature is verified.
2. The PID in the token is extracted.
3. The process is probed: on POSIX, `kill(pid, 0)` returns success if the process exists; on Windows, `WaitForSingleObject` with a zero timeout is used.
4. If the previous process is alive, SBgate refuses to start and emits an error: "live owner token exists for pid N". This prevents two instances from competing for the same endpoint.
5. If the previous process is gone (stale endpoint), SBgate proceeds. It removes stale temporary files (any `*.tmp.*` files adjacent to the owner file) and overwrites the token with its own.

The stale-endpoint check uses `flock(LOCK_EX | LOCK_NB)` on POSIX (or `LockFileEx(LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY)` on Windows) to serialize concurrent startups, preventing a race where two SBgate processes both observe the stale token at the same time and both proceed.

### Control Plane Operations

SBgate's control plane is accessible via the management socket in `control_dir`. Operations are identified by a `MANAGEMENT_COMMAND` frame containing a signed `ListenerManagementEnvelope`. The operation name is case-normalized and dispatched to one of three right categories:

| Right | Operations |
| --- | --- |
| Read (`kListenerManagementRightRead`) | `PING`, `STATUS`, `HEALTH`, `POOL_STATUS` |
| Lifecycle (`kListenerManagementRightLifecycle`) | `DRAIN`, `UNDRAIN`, `STOP`, `RELOAD` |
| Pool (`kListenerManagementRightPool`) | `POOL_RESIZE`, `POOL_RECYCLE`, `POOL_KILL`, `POOL_RESTART` |

Additional operations include `DBBT_VALIDATE`, `LPREFACE_VALIDATE`, and `SUPPORT_BUNDLE`.

### Drain Sequence

When a DRAIN command is received:

1. SBgate stops accepting new connections on the TCP socket (`enable_accept_loop` effectively becomes false).
2. Existing in-progress sessions are allowed to continue.
3. The listener waits for active sessions to complete, polling every `management_poll_ms`.
4. Once all sessions complete, or after `graceful_drain_timeout_ms` expires, SBgate transitions to stopped.

UNDRAIN reverses a DRAIN: the listener resumes accepting new connections without a restart.

### STOP Graceful vs. STOP Force

`STOP GRACEFUL` performs a drain before stopping. `STOP FORCE` skips the drain and terminates immediately. The `force` argument is encoded in the management envelope (`AddArg(&envelope, "force", normalized == "STOP FORCE" ? "true" : "false")`).

---

## Parser Worker Lifecycle

Parser workers are spawned by SBgate from the `parser_executable` path. Each worker goes through a startup sequence:

1. The worker starts and sends a HELLO frame to SBgate's control socket. The HELLO contains the worker's PID, worker ID, parser protocol version, profile ID, and bundle contract ID.
2. SBgate validates the bundle contract ID against its own configuration. Mismatches produce a HELLO_ACK with `accepted = false` and a reason string.
3. If accepted, the worker enters the warm pool and waits for a connection to be handed off.
4. When a client connects, SBgate selects a warm worker and sends a HANDOFF_SOCKET frame containing the client socket, connection metadata, and authentication context.
5. The worker sends a HANDOFF_ACK. If the engine accepts the session, the worker is committed to that session. If the engine rejects the handoff (non-zero status in HANDOFF_ACK), the connection is refused and the worker returns to the pool.

SBgate sends periodic HEALTH_CHECK frames to workers. Workers reply with a HEALTH_REPORT containing their current state byte and last error code. A worker that does not reply within the check timeout is considered unhealthy and is recycled.

### Parser Pool Backoff

If a worker exits unexpectedly, SBgate applies exponential backoff before respawning. The initial backoff is `child_restart_base_ms` (default 250 ms), doubling on each failure up to `child_restart_max_ms` (default 30 000 ms). If `child_quarantine_failures` failures occur within `child_quarantine_window_ms`, SBgate quarantines the pool and stops respawning. The parser pool state is included in the listener's lifecycle state file.

---

## Startup Validation and Refusal

Before any service reaches its ready state, it validates its configuration. Refusal points include:

- **Config format mismatch**: SBsrv refuses to start if `format` in `[config]` is not `SBCD1`.
- **Invalid key values**: SBgate refuses to start if a key value cannot be parsed (e.g. `port` is not a valid integer).
- **Live owner conflict**: SBgate refuses to start if a live process already owns the control endpoint.
- **Bundle contract mismatch**: SBgate refuses a parser worker that presents a bundle contract ID not matching `bundle_contract_id` in its configuration.
- **Engine handoff rejection**: The engine rejects a session handoff if the connection cannot be admitted (security check failure, resource limit, maintenance mode, etc.).
- **TLS not available**: If `tls_required = true` but TLS cannot be established, the connection is rejected.

In all these cases, refusal is accompanied by diagnostic output to the service's configured log destination. Collect those diagnostics before attempting remediation.

---

## Forced Shutdown and Recovery

If a service exits without performing a clean shutdown, the following artifacts may be present from the previous run:

| Artifact | Location | Action Required |
| --- | --- | --- |
| Stale owner token | Listener `control_dir` | Automatically resolved on next startup if previous process is gone |
| Stale lifecycle state file | Listener `runtime_dir` | Overwritten on next startup |
| Stale manager state file | `manager.control_dir/sbmn_manager.lifecycle.state` | Contains last-known state; useful for diagnosis; not automatically cleaned |
| Stale manager journal | `manager.control_dir/sbmn_manager.lifecycle.journal` | Retained across restarts; append-only |
| Stale SBPS socket | Configured `sbps_endpoint` path | Removed by SBsrv on next startup |

If a restart is blocked because SBmgr believes its state file shows an in-progress state from a previous unclean run, examine the state file and, if the previous manager process is confirmed gone, remove or reset the state file as appropriate for your environment. Always save a copy for diagnostic purposes before modifying.

---

## Related Pages

- [Monitoring, Health, And Readiness](monitoring_health_and_readiness.md)
- [Diagnostics, Message Vectors, And Support Bundles](diagnostics_message_vectors_and_support_bundles.md)
- [Operating Modes Runbook](operating_modes_runbook.md)
- [Getting Started: Standalone Server](../Getting_Started/operating_modes/standalone_server.md)

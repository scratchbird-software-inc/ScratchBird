# Operating Modes Runbook

ScratchBird can be deployed in several configurations, each suited to different needs. This chapter turns those configurations into concrete runbooks: what to configure, how to start, how to verify readiness, and how to shut down cleanly. Read the mode section that matches your deployment.

For a conceptual overview of the differences between modes, see [Getting Started: Choosing A Mode Summary](../Getting_Started/operating_modes/choosing_a_mode_summary.md). For deep dives on startup states and drain behavior, see [Service Lifecycle](service_lifecycle.md).

---

## Mode 1: Embedded Engine

The engine library (`libSBcore.so` on Linux, `SBcore` branded) can be linked directly into an application. In this mode there is no SBsrv process, no SBgate process, and no network port. The application calls the engine API directly and manages the database lifecycle itself.

### What This Mode Does Not Provide

Embedded mode does not run a listener, does not accept network connections, and does not use any configuration files from `etc/scratchbird/`. The embedding application is responsible for all resource and lifecycle management.

### Prerequisites

- The `libSBcore.so` shared library and its headers are available.
- The `share/scratchbird/resources/` directory is accessible at runtime.
- The embedding application is trusted: it runs in-process with the engine and has full access to all engine capabilities.

### Not Covered Here

Detailed embedded API usage is outside the scope of this guide. See the public API documentation in `share/scratchbird/docs/public_api/` for the embedded integration interface.

---

## Mode 2: IPC Server (SBsrv Standalone)

In this mode, SBsrv runs as a standalone process. It hosts the engine and exposes a Unix domain socket (the SBPS endpoint) that SBgate connects to. SBsrv can optionally supervise SBgate itself when `[server.listener.native] enabled = true`. This is the simplest networked deployment.

### Prerequisites

- The staged output is present and all binaries are executable.
- `etc/scratchbird/SBsrv.conf` has been copied and edited for the deployment.
- The `data_dir` and `control_dir` paths are writable by the process user.
- If `tls_required = true` (the default and recommended setting), TLS certificates are configured.
- The `share/scratchbird/resources/` tree is present.

### Configuration Inputs

Edit a copy of `etc/scratchbird/SBsrv.conf`. Key decisions:

1. Set `[server.runtime] data_dir` and `control_dir` to the deployment's runtime paths.
2. Set `[server.database] default_path` to the database file location.
3. Set `[server.database] auto_create = true` only for the initial setup run; reset it to `false` after the database exists.
4. Set `[server.listener.native] bind_host` and `port` (defaults: `127.0.0.1`, `3050`).
5. Verify `[server.listener.native] executable_path` and `parser_executable_path` resolve to the correct binaries.
6. Confirm `[server.parser] sbps_endpoint` will be writable and accessible to SBgate.

### Startup Sequence

1. Start SBsrv with the configuration file path as an argument.
2. SBsrv reads and validates configuration. Invalid configuration causes immediate exit with diagnostics to stderr.
3. SBsrv opens or creates the database, depending on `auto_create`.
4. SBsrv writes startup lifecycle artifacts (PID file, lifecycle state file, SBPS socket).
5. If `[server.listener.native] enabled = true`, SBsrv spawns SBgate and waits up to `ready_timeout_ms` (default 8 000 ms) for SBgate to report ready.
6. Once SBgate reports ready, SBsrv transitions to the `ready` state.

### Readiness Check

After startup, confirm readiness by:

- Verifying the SBPS socket exists at the configured `sbps_endpoint` path.
- Verifying SBgate's control socket is present in `control_dir`.
- Connecting with `SBsql` and running a simple query such as `SELECT 1;`.

### First Transaction Proof

```
bin/SBsql --host 127.0.0.1 --port 3050 --database default
SBsql> SELECT 1;
SBsql> \quit
```

A successful response confirms that the listener is accepting connections, the parser is active, and the engine is responding.

### Clean Shutdown

Issue a DRAIN command followed by a STOP command via the listener management interface, then stop SBsrv. The DRAIN command (see [Service Lifecycle](service_lifecycle.md)) causes SBgate to stop accepting new connections and wait for in-flight sessions to complete. Once all sessions have finished or `graceful_drain_timeout_ms` expires, STOP terminates the listener, and SBsrv performs a clean shutdown of the engine.

### Failure and Refusal

If SBsrv fails to open the database, it logs a diagnostic and exits with a non-zero code. If SBgate fails to start or does not report ready within `ready_timeout_ms`, SBsrv logs a diagnostic. In both cases the SBPS socket will not be present, which provides a reliable negative readiness indicator.

---

## Mode 3: Standalone Listener and Parser Route (SBgate Independent)

SBgate can be run independently of SBsrv's supervision. This mode is useful when a separate process (such as SBmgr) manages SBgate's lifecycle, or when you want to run SBgate against an already-running SBsrv.

### Prerequisites

- SBsrv is already running and its SBPS socket is accessible.
- `etc/scratchbird/SBgate.conf` has been copied and edited.
- `managed_by_server = false` and `managed_by_manager = false` (or `managed_by_manager = true` if SBmgr is used).

### Configuration Inputs

Edit a copy of `etc/scratchbird/SBgate.conf`. Key decisions:

1. Set `server_endpoint` to the SBPS socket path where SBsrv is listening.
2. Set `bind_address` and `port` for the client-facing TCP endpoint.
3. Set `control_dir` and `runtime_dir` to writable paths.
4. Set `warm_pool_min` and `warm_pool_max` based on expected concurrency.
5. Set `parser_executable` to the correct path for the parser binary.
6. Confirm `bundle_contract_id` matches the parser's contract.

### Annotated Walk-Through of SBgate.conf

The following annotates the key sections of a typical standalone SBgate configuration:

```
# Identity — who this listener is
listener_profile = default
lifecycle_generation = 1      # increment if you reconfigure and restart in place
protocol_family = sbsql
parser_package = SBParser
bundle_contract_id = sbp_sbsql@1

# Where to find the engine
server_endpoint = runtime/control/sb_server.sbps.sock

# Where to accept clients
bind_address = 127.0.0.1
port = 3050
tls_required = true           # do not set false on any network-exposed interface

# Parser pool — start with 1, allow up to 16
warm_pool_min = 1
warm_pool_max = 16
spawn_strategy = warm_pool

# Connection timeouts
preauth_timeout_ms = 30000    # drop unauthenticated connections after 30 s
handoff_ack_timeout_ms = 2000 # engine must ack session within 2 s
graceful_drain_timeout_ms = 30000  # allow 30 s for sessions to complete on drain

# Control infrastructure
control_dir = runtime/listener/control
runtime_dir = runtime/listener/runtime

# Supervision — running standalone
managed_by_server = false
managed_by_manager = false

# Safety flags — must be false in any real deployment
allow_dev_dbbt_env = false
allow_test_dbbt_builtin = false
```

### Startup Sequence

1. Start SBgate with the configuration file path as an argument.
2. SBgate validates configuration, acquires an owner token (a signed artifact in `control_dir`), and writes its lifecycle state to `runtime_dir`.
3. SBgate connects to SBsrv's SBPS endpoint and completes the HELLO handshake.
4. SBgate pre-spawns `warm_pool_min` parser worker processes.
5. Each parser worker sends a HELLO frame identifying its profile and bundle contract. SBgate validates the contract ID against its configuration.
6. SBgate begins accepting client connections.

### Readiness Check

- The lifecycle state file in `runtime_dir` should contain `state=ready`.
- The control socket in `control_dir` should be present.
- A `SBsql` connection attempt to `bind_address:port` should succeed.

### Stale Endpoint Handling

If SBgate finds an existing owner token when it starts up, it checks whether the previous owner process is still alive (via `kill(pid, 0)` on POSIX, or `WaitForSingleObject` on Windows). If the previous process is still alive, startup is refused. If the previous process is gone but the token is present, it represents a stale endpoint from a non-clean shutdown. SBgate removes the stale token, cleans up associated temporary files, and proceeds with startup. See [Service Lifecycle](service_lifecycle.md) for the full stale endpoint handling description.

---

## Mode 4: Managed Deployment (SBmgr)

SBmgr is the single-node manager. It supervises both SBsrv and SBgate, restarts them if they fail, exposes a proxy port for client connections, and provides a unified management interface. This is the recommended mode for any deployment where service continuity is important.

### Prerequisites

- `etc/scratchbird/SBmgr.conf` has been copied and edited.
- `etc/scratchbird/SBsrv.conf` and `etc/scratchbird/SBgate.conf` are configured for the managed deployment.
- `manager.management_auth_required = true` is retained (the default).
- The proxy port (`3090` by default) is accessible from clients.

### Configuration Inputs

Edit a copy of `etc/scratchbird/SBmgr.conf`. Key decisions:

1. Set `manager.control_dir` and `manager.runtime_dir` to writable paths.
2. Confirm `manager.listener_command` and `manager.server_command` resolve to the correct binaries.
3. Set `manager.restart_policy`, `manager.restart_max`, and `manager.restart_window_ms` to match operational expectations.
4. If you want to override the proxy port, add `manager.proxy.port = <value>`. The default is `3090`.

### Startup Sequence (Manager Lifecycle States)

SBmgr transitions through a well-defined sequence of lifecycle states on startup. These states are named in `manager_lifecycle.cpp` and are written to a state file in `manager.control_dir`:

1. `created` → `args_parsed` — arguments are parsed
2. `args_parsed` → `config_loading` — configuration file is read
3. `config_loading` → `config_validating` — configuration is validated
4. `config_validating` → `runtime_preparing` — runtime directories are created
5. `runtime_preparing` → `owner_acquiring` — owner token is acquired
6. `owner_acquiring` → `server_endpoint_resolving` — SBsrv endpoint is identified
7. `server_endpoint_resolving` → `server_supervision_starting` — SBsrv is spawned
8. `server_supervision_starting` → `listener_endpoint_resolving` — SBgate endpoint is identified
9. `listener_endpoint_resolving` → `listener_supervision_starting` — SBgate is spawned
10. `listener_supervision_starting` → `proxy_binding` — proxy TCP port is bound
11. `proxy_binding` → `management_binding` — management interface is bound
12. `management_binding` → `server_heartbeat_starting` → `ready` — heartbeat to SBsrv starts; manager reaches ready

If any step fails, the manager transitions to `startup_failed` or `failed_terminal`. The state file in `manager.control_dir/sbmn_manager.lifecycle.state` and the journal in `sbmn_manager.lifecycle.journal` record the full transition history.

### Readiness Check

- `manager.control_dir/sbmn_manager.lifecycle.state` contains `state=ready`.
- The proxy port is accepting connections.
- A `SBsql` connection to the proxy port succeeds.

### Restart Policy

SBmgr implements an exponential backoff restart policy. When a supervised process exits unexpectedly:

1. The manager counts the failure against the restart window.
2. The restart backoff starts at `restart_initial_backoff_ms` (default 1 000 ms in source) and doubles on each attempt, capped at `restart_max_backoff_ms` (default 60 000 ms in source), as implemented in `ComputeRestartBackoff` in `manager_restart_policy.cpp`.
3. If the restart count within `restart_window_ms` exceeds `restart_max_attempts` (source default: 3; template default: 5), the manager transitions to `quarantined` state. From `quarantined`, it will attempt to drain and stop cleanly rather than continuing to restart.

The `manager.restart_policy = bounded` template value selects this behavior.

### Clean Shutdown

Send a DRAIN command to the manager's management interface. The manager drains the listener (waiting up to `manager.drain_timeout_ms`), then stops SBsrv, then stops itself. Lifecycle state transitions are written to the journal throughout.

### Mode-Specific Diagnostics to Collect

If the managed deployment fails to start or becomes unhealthy, collect:

- `manager.control_dir/sbmn_manager.lifecycle.state`
- `manager.control_dir/sbmn_manager.lifecycle.journal`
- SBsrv stderr / log output
- SBgate lifecycle state file in its `runtime_dir`
- Any `.owner` or `.lifecycle.state` files in the listener's `control_dir`

---

## What Each Mode Does Not Imply

| Mode | Does Not Provide |
| --- | --- |
| Embedded | Network access, authentication enforcement, parser routing, separate process isolation |
| IPC Server (standalone) | Automatic restart on failure, proxy routing, manager management interface |
| Standalone Listener | Manager lifecycle supervision, automatic restart, proxy port |
| Managed (SBmgr) | Multi-node clustering, replication, distributed transaction management |

These boundaries exist by design. The managed mode is the highest-capability single-node configuration; it does not extend to distributed deployments.

---

## Related Pages

- [Service Lifecycle](service_lifecycle.md)
- [Monitoring, Health, And Readiness](monitoring_health_and_readiness.md)
- [Configuration Reference](configuration_reference.md)
- [Getting Started: Choosing A Mode Summary](../Getting_Started/operating_modes/choosing_a_mode_summary.md)
- [Getting Started: Standalone Server](../Getting_Started/operating_modes/standalone_server.md)

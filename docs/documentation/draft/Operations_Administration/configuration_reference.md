# Configuration Reference

Understanding ScratchBird's configuration files is a prerequisite for running any service. This chapter documents each file in detail — its location, its sections and keys, and the defaults that will be used if a key is omitted. It also explains how configuration is loaded and validated before a service accepts any work.

## How Configuration Is Loaded

Each service reads its own configuration file independently. There is no central configuration server or shared configuration database. The sequence, as reflected in the source, is:

1. **Command-line options** are parsed first. For SBsrv and SBmgr this determines which configuration file to load.
2. **The configuration file** is read and its key-value pairs (or TOML-style sections for SBsrv) are applied to an in-memory config struct.
3. **Validation** is performed. If a required value is missing, a value is out of range, or a security-sensitive key has an unsafe value, the service logs a diagnostic and refuses to start.
4. **Compiled defaults** fill in any keys that were not present in the file.

SBgate (`listener_config.cpp`) uses a flat key=value parser: each line is split on the first `=`, whitespace is trimmed, and keys are normalized to lowercase with hyphens replaced by underscores. SBsrv (`SBsrv.conf`) uses a `[section]` format with a `format = SBCD1` header. SBmgr uses a flat `manager.*` key namespace. SBParser uses a flat `parser.*` namespace.

Configuration files are read at startup only. In-place reloading is not described in the configuration templates; the `RELOAD` management command is available via the listener control plane (see [Service Lifecycle](service_lifecycle.md)).

Unknown keys are currently ignored by most parsers, which means typos in key names will not produce an error at load time. Always verify configuration takes effect by checking service logs or querying STATUS.

## SBsrv.conf — IPC Server

SBsrv is the IPC server that hosts the engine and manages database opens. Its configuration file uses TOML-like bracketed sections. The format identifier `SBCD1` appears in the `[config]` section and must match what the binary expects.

### `[config]`

| Key | Default | Notes |
| --- | --- | --- |
| `format` | *(required)* | Must be `SBCD1`. Any other value causes startup refusal. |

### `[server]`

| Key | Default | Notes |
| --- | --- | --- |
| `mode` | `foreground` | Run mode. `foreground` keeps the process in the foreground (no daemonization). The source also defines `service`, `validation_only`, `maintenance`, and `read_only` modes. |

### `[server.runtime]`

| Key | Default (template) | Notes |
| --- | --- | --- |
| `data_dir` | `runtime/data` | Directory for engine data files. Relative paths are resolved from the working directory at startup. |
| `control_dir` | `runtime/control` | Directory for control-plane sockets and PID/lifecycle files. |

### `[server.logging]`

| Key | Default (template) | Notes |
| --- | --- | --- |
| `log_file` | `stderr` | Log destination. `stderr` writes to standard error. |
| `log_level` | `info` | Verbosity. |

### `[server.security]`

| Key | Default (template) | Notes |
| --- | --- | --- |
| `provider_family` | `local_password` | Authentication provider family. |
| `provider_state` | `healthy` | Expected state of the security provider at startup. |
| `default_policy_installed` | `true` | Whether the default security policy has been installed. |

### `[server.database]`

| Key | Default (template) | Notes |
| --- | --- | --- |
| `default_path` | `data/default.sbdb` | Path to the default database file. |
| `auto_create` | `false` | Whether to create the database file if it does not exist. |
| `open_mode` | `normal` | Database open mode. |
| `daemon_scope` | `shared` | Scope under which the database is owned. |

### `[server.listener.native]`

This section configures the native SBsql listener that SBsrv will supervise. When `enabled = true`, SBsrv will spawn `SBgate` at the path given by `executable_path` and keep it alive.

| Key | Default (template) | Notes |
| --- | --- | --- |
| `enabled` | `true` | Whether to start the listener. |
| `bind_host` | `127.0.0.1` | Listener bind address. |
| `port` | `3050` | Listener TCP port. |
| `executable_path` | `bin/SBgate` | Path to the SBgate binary, relative to working directory. |
| `parser_executable_path` | `bin/SBParser` | Path to the parser worker binary. |
| `control_dir` | `runtime/listener/control` | Control-plane socket directory for the listener. |
| `runtime_dir` | `runtime/listener/runtime` | Runtime directory for the listener. |
| `tls_required` | `true` | Whether TLS is required for client connections. Setting this to `false` is strongly discouraged on any network-exposed interface. |
| `ready_timeout_ms` | `8000` | How long SBsrv will wait for SBgate to report ready before treating the startup as failed. |

### `[server.metrics]`

| Key | Default (template) | Notes |
| --- | --- | --- |
| `enabled` | `true` | Whether metrics collection is active. |
| `flush_interval_ms` | `1000` | How often accumulated metrics are flushed. |

### `[server.memory]`

The memory section governs the engine's allocation policy. The template ships a `production_gate` policy. Hard and soft limits are enforced by the allocator; exceeding the hard limit triggers `failure_mode`.

| Key | Default (template) | Notes |
| --- | --- | --- |
| `policy_name` | `production_gate` | Named policy applied to memory management. |
| `hard_limit_bytes` | `1073741824` (1 GiB) | Absolute memory ceiling. Allocations that would exceed this are rejected. |
| `soft_limit_bytes` | `805306368` (768 MiB) | Soft ceiling. When `reject_over_soft_limit = true`, allocations beyond this are refused. |
| `per_context_limit_bytes` | `134217728` (128 MiB) | Per-session or per-context allocation cap. |
| `page_buffer_pool_limit_bytes` | `536870912` (512 MiB) | Page buffer pool size. |
| `failure_mode` | `fail_closed` | How to respond when memory limits are hit. `fail_closed` rejects the operation. |
| `track_allocations` | `true` | Enable per-allocation tracking for diagnostics. |
| `zero_memory_on_allocate` | `false` | Zero memory on allocation (defensive, slower). |
| `zero_memory_on_release` | `true` | Zero memory on release (reduces information leakage). |
| `reject_over_soft_limit` | `true` | Refuse allocations once the soft limit is reached. |
| `policy_provenance` | `public_standalone_default` | Identifies where this policy came from. |
| `policy_generation` | `1` | Policy version counter. |
| `enable_platform_memory_probe` | `true` | Query the OS for available physical memory. |
| `require_platform_memory_ceiling` | `false` | Refuse startup if the OS memory probe fails. |

### `[server.parser]`

This section configures the SBPS (ScratchBird Parser Service) endpoint that SBgate uses to communicate with the engine.

| Key | Default (template) | Notes |
| --- | --- | --- |
| `sbps_enabled` | `true` | Whether the SBPS endpoint is active. |
| `sbps_endpoint` | `runtime/control/sb_server.sbps.sock` | Unix domain socket path for parser-to-server IPC. This path must match `server_endpoint` in `SBgate.conf`. |
| `sbps_max_frame_bytes` | `1048576` (1 MiB) | Maximum size of a single SBPS frame. |
| `sbps_max_streams` | `256` | Maximum number of concurrent SBPS streams. |
| `sbps_hello_timeout_ms` | `30000` (30 s) | How long to wait for a parser worker to send its HELLO frame. |
| `worker_restart_max` | `5` | Maximum number of times a parser worker may restart before being quarantined. |
| `worker_restart_window_ms` | `60000` (60 s) | The time window over which `worker_restart_max` is counted. |

---

## SBgate.conf — Listener

SBgate (the listener) accepts TCP connections from clients, manages a warm pool of parser worker processes, authenticates sessions, and hands established connections off to the engine via the SBPS socket. Its configuration file uses a flat `key = value` format.

### Identity and Protocol

| Key | Default (template) | Notes |
| --- | --- | --- |
| `listener_profile` | `default` | Profile name, recorded in lifecycle artifacts. |
| `lifecycle_generation` | `1` | Monotonically increasing counter used to distinguish restarts. Increment this whenever you intentionally reconfigure a listener that uses the same control socket location. |
| `controller_type` | `direct` | How the listener is controlled. `direct` means it is managed by the server it connects to. |
| `protocol_family` | `sbsql` | The client-facing protocol. |
| `parser_package` | `SBParser` | The parser package name. This must match a registered parser. |
| `bundle_contract_id` | `sbp_sbsql@1` | Identifies the bundle contract that the parser worker must satisfy. |
| `parser_executable` | `bin/SBParser` | Path to the parser worker binary. |
| `database_selector` | `server_database_default` | Which database to use for sessions that do not specify one. |

### Server Endpoint

| Key | Default (template) | Notes |
| --- | --- | --- |
| `server_endpoint` | `runtime/control/sb_server.sbps.sock` | Path to the SBPS socket exposed by SBsrv. Must match `[server.parser] sbps_endpoint` in `SBsrv.conf`. |

### Network

| Key | Default (template) | Notes |
| --- | --- | --- |
| `bind_address` | `127.0.0.1` | Address on which to accept client connections. |
| `port` | `3050` | TCP port. |
| `accept_backlog` | `128` | OS-level accept queue depth. |
| `tls_required` | `true` | Whether clients must use TLS. |
| `per_client_max_connections` | `0` | Maximum concurrent connections per client address. `0` means no limit. |
| `accept_rate_limit_per_second` | `0` | Incoming connection rate limit. `0` disables rate limiting. |
| `accept_rate_limit_burst` | `0` | Burst allowance for rate limiting. |

### Parser Worker Pool

The warm pool is a set of pre-spawned parser worker processes. When a new client connection arrives, the listener hands it to a warm worker immediately rather than incurring a process-spawn delay. The pool refills in the background.

| Key | Default (template) | Notes |
| --- | --- | --- |
| `spawn_strategy` | `warm_pool` | How parser workers are managed. |
| `warm_pool_min` | `1` | Minimum number of pre-warmed workers. |
| `warm_pool_max` | `16` | Maximum pool size. Connections beyond this count queue until a worker becomes free. |
| `child_restart_base_ms` | `250` | Base backoff when a parser worker exits unexpectedly. |
| `child_restart_max_ms` | `30000` (30 s) | Maximum backoff. Backoff doubles on each failure up to this ceiling. |
| `child_quarantine_failures` | `5` | Failures within `child_quarantine_window_ms` that trigger pool quarantine. |
| `child_quarantine_window_ms` | `60000` (60 s) | Window for failure counting. |

### Timeouts

| Key | Default (template) | Notes |
| --- | --- | --- |
| `preauth_timeout_ms` | `30000` (30 s) | How long a connection can remain unauthenticated before being dropped. |
| `handoff_ack_timeout_ms` | `2000` (2 s) | How long the listener waits for the engine to acknowledge a session handoff. If the engine does not respond within this window, the connection is refused. |
| `graceful_drain_timeout_ms` | `30000` (30 s) | How long the listener waits for in-flight connections to complete when a DRAIN command is issued. After this timeout, remaining connections are dropped. |
| `idle_poll_ms` | `250` | Polling interval for management loop. |
| `management_poll_ms` | `250` | Polling interval for management command processing. |
| `dbbt_ttl_ms` | `30000` (30 s) | Time-to-live for database bearer tokens. |

### Control Directories

| Key | Default (template) | Notes |
| --- | --- | --- |
| `control_dir` | `runtime/listener/control` | Directory for control-plane and management sockets and the owner token file. |
| `runtime_dir` | `runtime/listener/runtime` | Directory for lifecycle state files. |
| `metrics_namespace` | `sys.metrics.listener` | Metrics namespace prefix. |

### Supervision Flags

| Key | Default (template) | Notes |
| --- | --- | --- |
| `managed_by_server` | `true` | SBsrv supervises this listener. Set to `false` only when running SBgate independently. |
| `managed_by_manager` | `false` | SBmgr supervises this listener. Mutually exclusive with `managed_by_server` in typical deployments. |
| `enable_accept_loop` | `true` | Whether to accept new client connections. Set to `false` for a maintenance hold. |
| `read_only` | `false` | Restrict sessions to read-only operations. |
| `maintenance_mode` | `false` | Enable maintenance mode, which may restrict connection admission. |
| `allow_dev_dbbt_env` | `false` | Allow development-mode database bearer tokens from environment variables. Should be `false` in any deployment outside isolated development. |
| `allow_test_dbbt_builtin` | `false` | Allow built-in test bearer tokens. Must be `false` in any deployment. |

---

## SBmgr.conf — Single-Node Manager

SBmgr supervises SBsrv and SBgate as a pair. It restarts them if they fail, exposes a proxy port for client connections, and provides a management interface. Its configuration uses a flat `manager.*` key namespace.

| Key | Default (template) | Notes |
| --- | --- | --- |
| `manager.profile` | `default` | Profile name. |
| `manager.mode` | `foreground` | Run mode. |
| `manager.control_dir` | `runtime/manager/control` | Directory for control artifacts. |
| `manager.runtime_dir` | `runtime/manager/runtime` | Directory for runtime state files. |
| `manager.listener_command` | `bin/SBgate` | Path to the listener binary. SBmgr will spawn and supervise this process. |
| `manager.server_command` | `bin/SBsrv` | Path to the IPC server binary. SBmgr will spawn and supervise this process. |
| `manager.restart_policy` | `bounded` | Restart policy. `bounded` limits restarts to `restart_max` within `restart_window_ms`. |
| `manager.restart_max` | `5` | Maximum restart attempts within the window. |
| `manager.restart_window_ms` | `60000` (60 s) | Window for counting restart attempts. |
| `manager.drain_timeout_ms` | `30000` (30 s) | How long SBmgr waits for graceful shutdown before forcing termination. |
| `manager.metrics_enabled` | `true` | Whether metrics collection is active. |
| `manager.management_auth_required` | `true` | Whether management-plane connections require authentication. Setting this to `false` is unsafe except in isolated testing. |

The manager's proxy port defaults to `3090` and its bind address defaults to `0.0.0.0` (source: `manager_runtime.hpp:41`). These can be overridden via `manager.proxy.port` and `manager.proxy.bind`. The native management interface defaults to `127.0.0.1:3392`.

---

## SBParser.conf — Core Parser

SBParser is the native SBsql parser worker. It is spawned and managed by SBgate. Its configuration uses a flat `parser.*` namespace.

| Key | Default (template) | Notes |
| --- | --- | --- |
| `parser.profile` | `default` | Profile name. |
| `parser.family` | `sbsql` | Parser dialect family. |
| `parser.worker_binary` | `bin/SBParser` | Path to the parser worker binary itself. |
| `parser.bundle_contract_id` | `sbp_sbsql@1` | Bundle contract this parser satisfies. Must match `bundle_contract_id` in `SBgate.conf`. |
| `parser.protocol_min` | `1` | Minimum supported parser API version. |
| `parser.protocol_max` | `1` | Maximum supported parser API version. |
| `parser.cache.max_entries` | `1024` | Maximum parse-result cache entries. |
| `parser.resource.max_frame_bytes` | `1048576` (1 MiB) | Maximum SBPS frame size this parser worker will accept. |
| `parser.resource.max_streams` | `256` | Maximum concurrent streams. |
| `parser.security.auth_relay_required` | `true` | Whether the engine must relay authentication context to the parser. |
| `parser.execution.engine_authority_required` | `true` | Whether the engine must hold execution authority before the parser will proceed. |

---

## Dialect Parser Templates

Each dialect parser (e.g. `SB_PGSQL_Parser.conf`, `SB_MYSQL_Parser.conf`) follows the same structure as `SBParser.conf` but uses a different `parser.family` and `parser.bundle_contract_id`. They also include compatibility keys that constrain what the parser is allowed to do:

| Key | Meaning |
| --- | --- |
| `parser.compatibility.logical_backup_restore_streams` | Whether logical backup/restore is permitted and under what conditions |
| `parser.compatibility.server_local_file_access` | Whether server-side file access is permitted |
| `parser.compatibility.physical_page_backup_restore` | Whether physical page-level backup/restore is permitted |
| `parser.compatibility.low_level_repair_verify` | Whether low-level repair and verification operations are permitted |

For example, the PostgreSQL parser template sets `physical_page_backup_restore = denied` and `low_level_repair_verify = denied`, reflecting that those operations are not valid routes for dialect compatibility parsers.

---

## Configuration Principles

A few principles underlie the configuration design:

**Explicit over implicit.** Security-sensitive keys have explicit defaults that are safe. `tls_required = true`, `management_auth_required = true`, and `allow_dev_dbbt_env = false` are all default. If you change them, do so knowingly and document why.

**Fail closed.** The server memory policy defaults to `failure_mode = fail_closed`. A session that would exceed memory limits is refused, not allowed to proceed in a degraded state.

**Relative paths are relative to the working directory.** All paths in the configuration templates — `bin/SBgate`, `runtime/control/sb_server.sbps.sock`, and so on — are relative. Processes must be started from the output root for these to resolve correctly, or you must replace them with absolute paths.

**Secrets do not belong in configuration files.** The `mcp_secret_ref` key in SBmgr's runtime source is a reference to a secret, not the secret itself. Keep credential material out of configuration files.

## Related Pages

- [Identity, Security, And Policy](identity_security_and_policy.md)
- [Parser Registration And Routes](parser_registration_and_routes.md)
- [Service Lifecycle](service_lifecycle.md)
- [Getting Started: Configuration Basics](../Getting_Started/administration/configuration_basics.md)

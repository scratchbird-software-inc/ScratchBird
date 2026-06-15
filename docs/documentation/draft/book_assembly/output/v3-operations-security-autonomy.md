---
title: "ScratchBird — Operations, Security, and Autonomy"
---

# ScratchBird — Operations, Security, and Autonomy

*ScratchBird documentation — draft*

## Who this book is for

Operators, administrators, and security reviewers.

## About this book

This volume is for running ScratchBird in practice: installation and configuration, service and database lifecycle, backup and recovery, the security model, the autonomous agent runtime, and execution acceleration.

## Parts in this volume

- **Operations and Administration**
- **Security Guide**
- **Agent Runtime Guide**
- **Acceleration Guide**

> This is a **draft**. See *About This Documentation* at the end of this book for
> status and license. Confirm any specific behavior against the current build.

\newpage



# Operations and Administration




===== FILE SEPARATION =====

<!-- chapter source: Operations_Administration/README.md -->

<a id="ch-operations-administration-readme-md"></a>

# ScratchBird Operations And Administration Guide

This guide is the operator-facing companion to the Getting Started Guide and the SBsql Language Reference. It covers everything you need to install, configure, run, diagnose, and maintain ScratchBird deployments — from the moment you unpack a build output to the moment you decommission a database.

ScratchBird has a layered architecture: a core engine library, an IPC server that hosts it, a listener that accepts client connections and routes them through a parser package, and an optional single-node manager that supervises all three. Each layer has its own configuration file, its own lifecycle states, and its own failure behavior. That layering is intentional — it means you can embed the engine directly in a trusted application, or expose it over a network via a full managed stack, or anywhere in between. This guide follows that progression.

Every operational claim in this guide has been verified against the source tree. Build-configuration-dependent behavior is hedged explicitly. Generic advice — back up regularly, test restores — needs no source citation.

## Directory Map

| Chapter | Purpose |
| --- | --- |
| [Installation And Output Layout](#ch-operations-administration-installation-and-output-layout-md) | What a staged build output contains, where each file lives, and how to verify that the pieces match. |
| [Configuration Reference](#ch-operations-administration-configuration-reference-md) | Every configuration file with its real sections, keys, and defaults, plus an explanation of how configuration is loaded. |
| [Operating Modes Runbook](#ch-operations-administration-operating-modes-runbook-md) | Step-by-step runbooks for embedded, IPC-server, standalone-listener, and managed-group deployments. |
| [Service Lifecycle](#ch-operations-administration-service-lifecycle-md) | Start, readiness, drain, stop, restart, stale endpoint handling, and failure response. |
| [Identity, Security, And Policy](#ch-operations-administration-identity-security-and-policy-md) | Authentication, authorization, schema roots, workareas, protected material, and redaction policy. |
| [Parser Registration And Routes](#ch-operations-administration-parser-registration-and-routes-md) | Parser package registration, route selection, compatibility boundaries, and refusal behavior. |
| [Database Lifecycle](#ch-operations-administration-database-lifecycle-md) | Create, open, close, reopen, detach, attach, verify, refuse, and recover database lifecycle concepts. |
| [Filespaces And Storage](#ch-operations-administration-filespaces-and-storage-md) | Filespace identity, storage placement, primary filespace behavior, and storage diagnostics. |
| [Backup, Restore, And Data Movement](#ch-operations-administration-backup-restore-and-data-movement-md) | Logical backup and restore, import and export, migration, CDC, replication, ETL, and denied physical routes. |
| [External Git Catalog Versioning](#ch-operations-administration-external-git-catalog-versioning-md) | Opt-in export of the catalog as content-hashed artifacts for external Git versioning, diffing, and rollback planning — with engine authority preserved throughout. |
| [Diagnostics, Message Vectors, And Support Bundles](#ch-operations-administration-diagnostics-message-vectors-and-support-bundles-md) | Diagnostic classes, support-bundle content, redaction, and operator review. |
| [Monitoring, Health, And Readiness](#ch-operations-administration-monitoring-health-and-readiness-md) | Health checks, readiness checks, liveness checks, metrics, and operational state. |
| [Troubleshooting](#ch-operations-administration-troubleshooting-md) | Symptom-oriented diagnosis paths for startup, connection, parser, security, storage, and transaction issues. |
| [Upgrade And Compatibility Policy](#ch-operations-administration-upgrade-and-compatibility-policy-md) | Version policy, format compatibility, parser package compatibility, and unsupported downgrade refusal. |
| [Release Validation Checklist](#ch-operations-administration-release-validation-checklist-md) | Operator-facing checklist for validating a build before broader use. |

## Reading Model

If you are setting up ScratchBird for the first time, start with [Installation And Output Layout](#ch-operations-administration-installation-and-output-layout-md), then read [Configuration Reference](#ch-operations-administration-configuration-reference-md), then open the runbook section in [Operating Modes Runbook](#ch-operations-administration-operating-modes-runbook-md) that matches the deployment you are building.

If something has gone wrong, jump to [Service Lifecycle](#ch-operations-administration-service-lifecycle-md) for state-machine context, then [Monitoring, Health, And Readiness](#ch-operations-administration-monitoring-health-and-readiness-md) for health check commands, then [Troubleshooting](#ch-operations-administration-troubleshooting-md) for symptom-specific paths.

The Getting Started Guide (ScratchBird — Concepts and Getting Started, page XXX) is a better first read if you are new to ScratchBird's concepts. The Language Reference (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX) covers SBsql syntax, catalog details, and data types.

## Draft Status

This is a draft manual. Technical claims have been verified against the source tree and build outputs. Generic operational guidance is provided without source citation. Claims that could not be verified from source have been omitted.




===== FILE SEPARATION =====

<!-- chapter source: Operations_Administration/installation_and_output_layout.md -->

<a id="ch-operations-administration-installation-and-output-layout-md"></a>

# Installation And Output Layout

Before you can run ScratchBird you need to understand what a staged build output looks like — which files are required, which are optional, where they go, and what the relationships between them are. This chapter answers those questions. It is intentionally concrete: the paths, filenames, and groupings here reflect what the build actually produces, not an idealized layout.

## What A Staged Output Is

ScratchBird does not install into a system-wide prefix like `/usr/local`. Instead, the build system stages a self-contained output tree under `output/<platform>` within the build directory (where `<platform>` is a string such as `linux`). This approach means the entire deployment can be inspected, copied, and relocated as a directory tree. The `SB_BUILD_PUBLIC_STANDALONE_OUTPUT` CMake option, which is `ON` by default, drives this staging step.

A staged output is not a source tree, not a CMake install prefix, and not a live database location. The output tree contains only the files that belong to the runtime deployment; source files, build artifacts, and intermediate objects are not included.

## Top-Level Directory Structure

The staged output on Linux follows a conventional Unix-like layout:

```
output/linux/
  bin/          executable binaries
  lib/          shared libraries
  etc/          configuration templates
  share/        resources, documentation, and examples
  pdb/          platform debug information (where applicable)
  STANDALONE_OUTPUT_MANIFEST.json
```

Every path used in configuration files shipped with the output — such as `bin/SBgate` in `SBsrv.conf` — is relative to this root. An operator who relocates the tree should update those paths accordingly, or run the processes from the output root so that relative paths resolve correctly.

## Binaries

### Runtime Binaries

The core runtime components are placed in `bin/`:

| Binary | Internal Target | Role |
| --- | --- | --- |
| `SBgate` | `sb_listener` | Listener: accepts TCP connections, runs the parser pool, hands off authenticated sessions to the IPC server |
| `SBsrv` | `sb_server` | IPC server: hosts the engine, manages database opens, routes parser traffic |
| `SBmgr` | `sbmn_manager` | Single-node manager: supervises SBsrv and SBgate, exposes a proxy port, manages restart policy |
| `SBParser` | `sbp_sbsql` | Core parser worker for the native SBsql dialect |

Each binary is named by its public brand (`sb_public_brand_target` in `CMakeLists.txt:3733-3737`). The internal CMake target names are different and are not visible to administrators.

### Shared Library

`lib/libSBcore.so` (on Linux) is the ScratchBird engine shared library. Applications that embed the engine directly link against this library rather than running SBsrv. The library's public brand name is `SBcore`; the static variant is `SBcore_static` and is not placed in the staged output.

### Command-Line Utilities

Five administrative utilities are placed in `bin/`:

| Binary | Internal Target | Role |
| --- | --- | --- |
| `SBsql` | `sb_isql` | Interactive SQL client (SBsql CLI) |
| `SBadm` | `sb_admin` | Administrative operations |
| `SBbak` | `sb_backup` | Backup and restore manager |
| `SBsec` | `sb_security` | Security administration |
| `SBdoc` | `sb_verify` | Doctor: verification and diagnostics |
| `SBcop` | `sbdriver_conformance` | Conformance officer: driver conformance testing |

Not all utilities may be present in every build, depending on which CMake targets were enabled.

## Configuration Templates

The `etc/scratchbird/` directory contains configuration templates for every service. These are the canonical starting point for any deployment. The templates are copied from `project/config/templates/` during the staging step (`CMakeLists.txt:2827-2828`).

| File | Service |
| --- | --- |
| `SBsrv.conf` | IPC server (SBsrv) |
| `SBgate.conf` | Listener (SBgate) |
| `SBmgr.conf` | Single-node manager (SBmgr) |
| `SBParser.conf` | Core parser (SBParser) |
| `SB_PGSQL_Parser.conf` | PostgreSQL-compatibility parser worker |
| `SB_MYSQL_Parser.conf` | MySQL-compatibility parser worker |
| `SB_MARIADB_Parser.conf` | MariaDB-compatibility parser worker |
| `SB_SQLITE_Parser.conf` | SQLite-compatibility parser worker |
| `SB_FBSQL_Parser.conf` | Firebird-compatibility parser worker |
| `SB_DUCKDB_Parser.conf`, `SB_CLICKHOUSE_Parser.conf`, ... | Additional dialect workers |

The full set of parser templates corresponds to the set of compatibility parser-worker build options available in `CMakeLists.txt`. Not all parser workers are built by default; their configuration templates are installed regardless so that operators can prepare for future enablement.

See [Configuration Reference](#ch-operations-administration-configuration-reference-md) for the contents and keys of each file.

## Resources

`share/scratchbird/resources/` contains the data files that the engine loads at runtime. The resource tree is installed from `project/resources/` (`CMakeLists.txt:2824-2826`). The staged output includes subdirectories for:

- `cluster-catalog/` — cluster catalog seed data
- `policy-packs/` — policy pack definitions
- `seed-packs/` — seed data packs

These directories must be present and readable when the engine starts. If a resource file is missing or corrupt, the engine will refuse to start or will fall back to built-in defaults, depending on the resource type and the configuration.

Additional resource material may be present in `share/scratchbird/docs/` and `share/scratchbird/examples/` in builds where those targets are enabled.

## Runtime Directories

The staged output does **not** contain a `runtime/` directory. Runtime directories are created by the processes themselves the first time they run. The default paths used in the shipped configuration templates are relative paths like `runtime/data`, `runtime/control`, and `runtime/listener/control`. If you run a service from the output root, these will be created under the output root. In a production deployment you will typically want to redirect runtime directories to a dedicated location outside the output tree — such as a directory under `/var/lib/scratchbird` for data and `/run/scratchbird` for sockets and PID files — by editing the configuration files before starting any service.

## Manifest

`STANDALONE_OUTPUT_MANIFEST.json` documents the contents of the staged output. Operators can use this file to verify that expected binaries and resources are present and to cross-check the build configuration that produced the output.

## What Belongs Where

| Category | Lives In | Notes |
| --- | --- | --- |
| Executables and shared library | `bin/`, `lib/` | Do not modify after staging |
| Configuration | `etc/scratchbird/` | Copy and edit for deployment; do not edit in place |
| Resources | `share/scratchbird/resources/` | Do not modify; must match the binary version |
| Runtime sockets and PID files | Configured paths, not in output tree | Created by processes at startup |
| Database files | Configured `data_dir`, not in output tree | Created by the engine when a database is first opened |
| Log files | Configured `log_file`, not in output tree | Defaults to stderr |

## Verifying the Output

Before using a staged output for the first time, confirm that:

1. All expected binaries are present in `bin/` and are executable.
2. All four service configuration templates are present in `etc/scratchbird/`.
3. The `share/scratchbird/resources/` tree is present and non-empty.
4. The `STANDALONE_OUTPUT_MANIFEST.json` is readable.

The `SBdoc` (Doctor) utility and the `SBcop` (Conformance Officer) utility provide additional validation. Consult the [Release Validation Checklist](#ch-operations-administration-release-validation-checklist-md) for a structured pre-deployment checklist.

## Related Pages

- [Configuration Reference](#ch-operations-administration-configuration-reference-md)
- [Release Validation Checklist](#ch-operations-administration-release-validation-checklist-md)
- Getting Started: Configuration Basics (ScratchBird — Concepts and Getting Started, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Operations_Administration/configuration_reference.md -->

<a id="ch-operations-administration-configuration-reference-md"></a>

# Configuration Reference

Understanding ScratchBird's configuration files is a prerequisite for running any service. This chapter documents each file in detail — its location, its sections and keys, and the defaults that will be used if a key is omitted. It also explains how configuration is loaded and validated before a service accepts any work.

## How Configuration Is Loaded

Each service reads its own configuration file independently. There is no central configuration server or shared configuration database. The sequence, as reflected in the source, is:

1. **Command-line options** are parsed first. For SBsrv and SBmgr this determines which configuration file to load.
2. **The configuration file** is read and its key-value pairs (or TOML-style sections for SBsrv) are applied to an in-memory config struct.
3. **Validation** is performed. If a required value is missing, a value is out of range, or a security-sensitive key has an unsafe value, the service logs a diagnostic and refuses to start.
4. **Compiled defaults** fill in any keys that were not present in the file.

SBgate (`listener_config.cpp`) uses a flat key=value parser: each line is split on the first `=`, whitespace is trimmed, and keys are normalized to lowercase with hyphens replaced by underscores. SBsrv (`SBsrv.conf`) uses a `[section]` format with a `format = SBCD1` header. SBmgr uses a flat `manager.*` key namespace. SBParser uses a flat `parser.*` namespace.

Configuration files are read at startup only. In-place reloading is not described in the configuration templates; the `RELOAD` management command is available via the listener control plane (see [Service Lifecycle](#ch-operations-administration-service-lifecycle-md)).

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

- [Identity, Security, And Policy](#ch-operations-administration-identity-security-and-policy-md)
- [Parser Registration And Routes](#ch-operations-administration-parser-registration-and-routes-md)
- [Service Lifecycle](#ch-operations-administration-service-lifecycle-md)
- Getting Started: Configuration Basics (ScratchBird — Concepts and Getting Started, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Operations_Administration/operating_modes_runbook.md -->

<a id="ch-operations-administration-operating-modes-runbook-md"></a>

# Operating Modes Runbook

ScratchBird can be deployed in several configurations, each suited to different needs. This chapter turns those configurations into concrete runbooks: what to configure, how to start, how to verify readiness, and how to shut down cleanly. Read the mode section that matches your deployment.

For a conceptual overview of the differences between modes, see Getting Started: Choosing A Mode Summary (ScratchBird — Concepts and Getting Started, page XXX). For deep dives on startup states and drain behavior, see [Service Lifecycle](#ch-operations-administration-service-lifecycle-md).

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

Issue a DRAIN command followed by a STOP command via the listener management interface, then stop SBsrv. The DRAIN command (see [Service Lifecycle](#ch-operations-administration-service-lifecycle-md)) causes SBgate to stop accepting new connections and wait for in-flight sessions to complete. Once all sessions have finished or `graceful_drain_timeout_ms` expires, STOP terminates the listener, and SBsrv performs a clean shutdown of the engine.

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

If SBgate finds an existing owner token when it starts up, it checks whether the previous owner process is still alive (via `kill(pid, 0)` on POSIX, or `WaitForSingleObject` on Windows). If the previous process is still alive, startup is refused. If the previous process is gone but the token is present, it represents a stale endpoint from a non-clean shutdown. SBgate removes the stale token, cleans up associated temporary files, and proceeds with startup. See [Service Lifecycle](#ch-operations-administration-service-lifecycle-md) for the full stale endpoint handling description.

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

- [Service Lifecycle](#ch-operations-administration-service-lifecycle-md)
- [Monitoring, Health, And Readiness](#ch-operations-administration-monitoring-health-and-readiness-md)
- [Configuration Reference](#ch-operations-administration-configuration-reference-md)
- Getting Started: Choosing A Mode Summary (ScratchBird — Concepts and Getting Started, page XXX)
- Getting Started: Standalone Server (ScratchBird — Concepts and Getting Started, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Operations_Administration/service_lifecycle.md -->

<a id="ch-operations-administration-service-lifecycle-md"></a>

# Service Lifecycle

A running ScratchBird deployment is not a single process — it is a set of cooperating processes (engine server, listener, optional manager) that each move through defined lifecycle states. Understanding those states and the transitions between them is essential for operating the system correctly, diagnosing unexpected exits, and recovering from failures.

This chapter explains the lifecycle of each component, the control-plane operations that drive transitions, how stale endpoints are handled, and what refusal behavior looks like from an operator's perspective.

---

## Lifecycle State Concepts

A **lifecycle state** is a named point in a process's existence. States are written to durable files so that an operator (or a supervising process) can read the current state without needing access to the running process. State transitions are logged to a journal file. Both the state file and the journal are written atomically, with fsync, to reduce the risk of corruption from an unexpected shutdown.

A **terminal state** is one from which no further transitions are possible. A process in a terminal state will not restart itself; external action is required.

**Drain** is a transitional mode in which a service stops accepting new work but completes work already in progress. Draining is the recommended path to a clean shutdown because it allows in-flight sessions to finish. The duration of a drain is bounded by `graceful_drain_timeout_ms` in SBgate or `manager.drain_timeout_ms` in SBmgr.

A **message vector** is the structured diagnostic output produced when something goes wrong. Message vectors are emitted to logs and, in some cases, to support bundles. See [Diagnostics, Message Vectors, And Support Bundles](#ch-operations-administration-diagnostics-message-vectors-and-support-bundles-md) for details.

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

- [Monitoring, Health, And Readiness](#ch-operations-administration-monitoring-health-and-readiness-md)
- [Diagnostics, Message Vectors, And Support Bundles](#ch-operations-administration-diagnostics-message-vectors-and-support-bundles-md)
- [Operating Modes Runbook](#ch-operations-administration-operating-modes-runbook-md)
- Getting Started: Standalone Server (ScratchBird — Concepts and Getting Started, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Operations_Administration/identity_security_and_policy.md -->

<a id="ch-operations-administration-identity-security-and-policy-md"></a>

# Identity, Security, And Policy

## Purpose

This chapter defines the operator-facing model for identity, authentication, authorization, schema roots, protected material, policy, and redaction in ScratchBird. Understanding these concepts before configuring a production deployment prevents a class of hard-to-diagnose access failures that stem from conflating who a principal is (identity), how they prove it (authentication), and what they are allowed to do (authorization).

## The Three-Layer Model

ScratchBird separates identity concerns into three distinct layers that execute in sequence for every connection:

1. **Authentication** — proves that the client is who they claim to be, using a configured provider family.
2. **Authorization** — determines what the authenticated principal may do by evaluating grants, roles, and group memberships against the requested operation.
3. **Deep enforcement** — applies masking, row-level security, and audit obligations as the engine executes the work.

Parser routes do not grant authority by themselves. A parser accepted on a given route still passes through the full engine authorization chain before any data is read or written.

## Principal Kinds

ScratchBird recognizes three principal kinds, validated in `security_principal_lifecycle.cpp`:

| Kind | Typical use |
|------|-------------|
| `user` | Interactive or application accounts identified by name |
| `service` | Non-interactive workloads, background jobs, API integrations |
| `system_actor` | Internal engine subsystems; not creatable by operators |

Each principal has a lifecycle state of either `active` or `disabled`. The engine also accepts `enabled` as an alias for `active` and `disable` as an alias for `disabled` during mutations; these are normalized internally. A disabled principal will fail authentication regardless of credential validity.

Roles and groups are tracked separately from principals. A role is owned by a principal and may be granted to other principals. A group carries an `external_authority_ref` field that links it to an identity provider's group claim, enabling provider-sourced group membership to flow into the engine's authorization model.

## Authentication Providers

The engine supports a broad set of authentication provider families. The mapping between a provider family name and its required runtime dependency is maintained in `auth_provider_live_adapter.cpp`:

| Provider family | Required dependency |
|-----------------|---------------------|
| `ldap_ad` | `ldap_client` |
| `kerberos_pac` | `gssapi_krb5` |
| `pam` | `pam` |
| `radius` | `radius_client` |
| `oidc_jwt` | `oidc_jwt_client` |
| `saml` | `saml_xmlsig` |
| `webauthn` / `factor_chain` | `webauthn_fido2` |
| `workload_identity` / `managed_identity` | `spiffe_svid_or_workload_oidc` |
| `certificate_mtls` | `tls_x509` |
| `proxy_assertion` | `proxy_assertion_verifier` |
| `remote_security_database` | `remote_scratchbird_security` |
| `bearer_token` | _(none required)_ |
| `token_api_key` | _(none required)_ |

If a required dependency is absent at authentication time, the engine returns a denial with the detail `real_client_dependency_missing:<dependency>` rather than silently falling back to an insecure path.

### LDAP / Active Directory

LDAP authentication requires `starttls=true` — plain-text LDAP binds are refused with diagnostic `ldap_starttls_required`. Group materialization is also required; a successful bind without groups in the payload returns `ldap_group_materialization_required`.

### PAM

The PAM provider requires a hidden or secret prompt mode. A PAM conversation with an insecure prompt type is refused as `pam_insecure_prompt`. Both the account phase and the session-open phase must succeed.

### RADIUS

The RADIUS authenticator value must be a valid 64-character hex string. A result other than `accept` from the RADIUS server returns `radius_rejected`.

### Proxy Assertion

The proxy assertion provider (`proxy_assertion`) requires a verified source, confirmed manager trust, and a validated listener binding. All three must be `trusted` or `verified`; a failure on any returns the corresponding denial code. This provider is intended for trusted middle-tier components that forward a user's identity rather than for direct user authentication.

### Token Hardening

Across all token-based providers (OIDC JWT, SAML, WebAuthn, bearer token, API key), the engine enforces:

- Algorithm downgrade denial — algorithms `none`, `md5`, `sha1`, and `rs1` are refused with `provider_algorithm_downgrade_denied`.
- Replay detection — a payload marked as replayed returns `provider_replay_denied`.
- Token revocation — a revoked token returns `SECURITY.TOKEN_REVOKED`.

### Multi-Factor Authentication

The `factor_chain` provider enforces a multi-factor policy. A chain where `factor_results != allow` returns `SECURITY.MFA_REQUIRED`. The challenge transcript hash must be a valid 64-character hex value.

## Authentication Request Structure

The engine's `EngineAuthenticate` API accepts a `provider_family`, a `principal_claim`, and `credential_evidence`. The result carries a `ConnectionSecurityContextRecord` that becomes the security context for the session. A subsequent `EngineRefreshSecurityContext` call updates the context if the principal's grants or group memberships change mid-session.

Denied authentication always produces a diagnostic with code `SECURITY.AUTHENTICATION.FAILED` or a more specific code such as `SECURITY.TOKEN_REVOKED` or `SECURITY.MFA_REQUIRED`. These codes are safe to return to clients.

## Authorization and Grants

Authorization is materialized from durable grant records. The `EngineGrantRight` and `EngineRevokeRight` APIs record grant mutations into the catalog. The materialized authorization context for a connection lists its effective subjects — the principal's own UUID plus any roles and groups the principal is a member of. An operation is admitted only when the materialized context carries a matching privilege for the target object.

The default authorization policy at database creation is `default_deny_explicit_allow` (from the bootstrap policy `security.authorization_default`). No privilege is assumed unless explicitly granted.

## Deep Security Enforcement

The `EngineEvaluateDeepSecurity` API is the single authority point for executor, storage, and catalog callers. A single evaluation answers:

- Is the operation admitted by rights? (`admitted`, `authorized`)
- Is the object visible to this principal? (`visible`)
- Should masking be applied? (`masked`)
- Should row-level security filter the result? (`rls_applied`)
- Has audit been recorded before success? (`audit_written`)
- Is the side effect permitted? (`side_effect_permitted`)

This API is not a parser hook; it executes inside the engine after the parser has translated the statement.

## Schema Roots

When a database is created, the engine bootstraps a fixed set of schema root paths. These paths are defined in `bootstrap_schema_roots.hpp` and include:

| Path | Purpose |
|------|---------|
| `sys` | Engine system namespace root |
| `sys.catalog` | Catalog visibility |
| `sys.security` | Security catalog tables |
| `sys.metrics` | Metrics and observability |
| `sys.audit` | Audit event records |
| `sys.storage` | Storage and filespace management views |
| `sys.parser` | Parser registration and configuration |
| `sys.diagnostics` | Diagnostic output |
| `sys.information` / `sys.information_schema` | Standard information schema views |
| `users` | User home schema root |
| `users.public` | Default public schema |
| `remote` | Remote access schema surface |
| `emulated` | Emulated compatibility schema overlay |

Schema roots are created with fresh UUIDv7 identifiers at database creation time; the paths are fixed but the UUIDs are not pre-determined. User home schemas are created under the `users` tree by default (policy `security.user_home_schema`). The `kLocalUserHomePolicyRoot` constant (`"users"`) governs the default home root.

A parser workarea is the subset of schema roots that the parser process can resolve names against. Each parser operates inside this boundary; catalog objects outside the parser's assigned workarea are not resolvable from that parser.

## Protected Material

Protected material is the engine's term for encryption keys, secrets, and credential artifacts that must never appear in diagnostic output, logs, or support bundles. The `protected_material_api.hpp` enforces this contract: every result struct that could potentially return secret content includes a `protected_material_redacted = true` field and a `plaintext_material_returned = false` field. No call path that is expected to keep material protected returns plaintext.

Encryption keys are admitted into the engine's key cache via `EngineAdmitEncryptionKey`. The cache entry carries a configurable `cache_ttl_millis` (default 300,000 ms). On shutdown, the engine purges all key cache entries (`EngineShutdownProtectedMaterial`). Key rotation is supported through `EngineRotateEncryptionKey`, which records rotation metadata durably without persisting plaintext.

The `EngineOpenEncryptedFilespace` call gates opening a filespace that was created with encryption. If the required key is not in the cache or has expired, the open is refused.

Protected material versions support legal hold (`legal_hold = true` in `EngineProtectedMaterialPolicySet`). A purge operation that encounters a version under legal hold is refused by retention (`refused_by_retention`).

## Support Bundle Redaction

When a support bundle is generated, the `RedactManagerSupportBundleText` function in `manager_support_bundle.cpp` scans every line of every included file and replaces the values following these sensitive key names with `[redacted]`:

`password`, `passwd`, `secret`, `token`, `private_key`, `credential`, `verifier`, `encryption_key`, `decryption_key`, `key_handle`

In addition, filesystem paths are replaced with `[path-redacted]`. The redaction is applied before any file is written into the support bundle archive. Operators should not rely on the support bundle as a source of credential material.

## Audit Events

The audit subsystem records two event shapes: `EngineEmitAuditEvent` for general events (carrying an `event_class` and `outcome`), and `EngineEmitLifecycleAuditEvent` for lifecycle mutations (carrying an `operation_key`, `diagnostic_code`, and `correlation_uuid`). Lifecycle audit events also track whether a cache invalidation was recorded. All audit events default `redacted = true`; the emitting code must explicitly clear redaction only when it has confirmed the payload contains no protected material.

The bootstrap policy `security.audit` (`security_activity_audit_v1`) enables audit of security events, policy mutations, and database creation. Evidence retention duration is governed by the `evidence.retention` policy (`audit_minimum_v1`).

## Policy Admission

Policies are created as part of database bootstrap and can be mutated only through database commands, not through filesystem policy packs at runtime (`policy_api.hpp`). The `EngineMutatePolicy` API records each mutation into the MGA catalog with an audit event and increments the policy epoch. Filesystem packs are create-time seeds only.

## Refusal Behavior

Every denied access returns a controlled message vector. Raw strings are forbidden; all diagnostic codes reference a registered message key. The `diagnostics.message_vector` bootstrap policy (`canonical_redacted_v1`) mandates redaction and requires a correlation ID on every vector. Operators observing an access refusal should inspect the returned message vector code rather than the raw detail string.

## Operator Checklist

- Verify that the required runtime dependency for each configured provider family is installed before enabling that provider.
- Use `system_actor` principals only when directed to; do not attempt to create them.
- Confirm that `starttls` is set when configuring LDAP. Binds without TLS are refused at the engine level, not just at policy level.
- Review the support bundle redaction list before adding new configuration keys that may contain credentials.
- Do not place raw secret values in parser packets, scripts, or configuration. Use protected material handles instead.

## Related Pages

- [Configuration Reference](#ch-operations-administration-configuration-reference-md)
- [Diagnostics, Message Vectors, And Support Bundles](#ch-operations-administration-diagnostics-message-vectors-and-support-bundles-md)
- Language Reference: Security And Privilege Statements (SBsql Language Reference — Syntax, page XXX)
- Language Reference: Policy, Masking, And Row-Level Security (SBsql Language Reference — Syntax, page XXX)
- Getting Started: Identity, Authentication, And Authorization (ScratchBird — Concepts and Getting Started, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Operations_Administration/parser_registration_and_routes.md -->

<a id="ch-operations-administration-parser-registration-and-routes-md"></a>

# Parser Registration And Routes

## Purpose

ScratchBird separates language parsing from query execution. A parser is an isolated process that translates a client's SQL dialect into an internal representation called SBLR (ScratchBird Logical Representation). The engine then independently verifies, secures, and executes that representation. This separation means the engine never trusts parser output alone — authorization and transaction management always run inside the engine regardless of which parser generated the request.

This chapter explains how parser packages are registered, how the listener manages parser worker processes, how route selection works, and what operators see when a parser is missing, quarantined, or incompatible.

## Parser Package Concepts

A **parser package** is a versioned, self-contained binary plus a configuration file that together define one supported SQL dialect. Each package carries:

- A `parser_uuid` — a stable identifier for this particular package.
- A `dialect` string — the name of the SQL dialect.
- A `build_id` — the specific build of this package.
- A `protocol_version` — the IPC protocol version the parser speaks.
- A `metrics_schema_version` — the version of the metrics schema the parser publishes.

These fields are exchanged in the `ParserHello` message when a parser worker starts. The engine responds with a `ParserHelloResult`. If the engine refuses the hello, the result carries `accepted = false` and a `MessageVectorSet` describing the refusal. The function `RefusedHelloResult` in `parser_server_ipc.hpp` constructs these refusal messages from a code and reason string.

The current IPC protocol version is `kParserServerIpcProtocolCurrent = 1`, with supported range 1–1. A parser reporting a protocol version outside this range will have its hello refused.

The native SBsql v3 parser package is identified in code as `client_dialect = "sbsql_v3"`. Its package request struct (`NativeV3ParserPackageRequest`) includes the `parser_package_uuid`, `parser_package_version`, and a `registry_snapshot_uuid` that ties the parse to a specific catalog generation.

## Parser Configuration Templates

Twenty-five compatibility parser templates ship under `config/templates/` with names of the form `SB_<DIALECT>_Parser.conf`. The following dialects are covered by templates:

Apache Ignite, Cassandra, ClickHouse, CockroachDB, Dolt, DuckDB, Firebird (FBSQL), FoundationDB, ImmuDB, InfluxDB, MariaDB, Milvus, MongoDB, MySQL, Neo4j, OpenSearch, OpenSearch SQL/PPL, PostgreSQL (PGSQL), Redis, SQLite, TiDB, TiKV, Vitess, XTDB, YugabyteDB.

Every template follows the same structure. For example, the PostgreSQL parser template (`SB_PGSQL_Parser.conf`) reads:

```
parser.profile = default
parser.family = postgresql
parser.worker_binary = bin/SB_PGSQL_Parser
parser.bundle_contract_id = sbp_postgresql@1
parser.protocol_min = 1
parser.protocol_max = 1
parser.security.auth_relay_required = true
parser.execution.engine_authority_required = true
parser.compatibility.logical_backup_restore_streams = enabled_when_remote_logical_stream
parser.compatibility.server_local_file_access = denied_by_default
parser.compatibility.physical_page_backup_restore = denied
parser.compatibility.low_level_repair_verify = denied
```

The four compatibility keys have fixed semantics:

| Key | Meaning |
|-----|---------|
| `logical_backup_restore_streams` | Whether this parser can issue logical backup or restore stream requests |
| `server_local_file_access` | Whether this parser may request access to server-local filesystem paths |
| `physical_page_backup_restore` | Whether this parser may request physical page-level backup or restore |
| `low_level_repair_verify` | Whether this parser may request low-level repair or verify operations |

All four keys are `denied` or `denied_by_default` in every shipped compatibility template. Logical backup and restore is set to `enabled_when_remote_logical_stream`, meaning the stream must originate from a remote source rather than a local file shortcut. The engine enforces these constraints independently of what the parser requests, so a compatibility parser cannot escalate to physical backup or low-level repair even if the parser's own code attempts to do so.

`engine_authority_required = true` is set on all templates. This means the parser process cannot complete any statement without the engine's independent security and authorization check.

## Parser Worker Lifecycle

The listener manages parser workers through a `ParserPool`. Each worker is a separate process. Worker states are defined by `ParserWorkerState` in `parser_pool.hpp`:

| State | Meaning |
|-------|---------|
| `kCold` | Not yet started |
| `kStarting` | Process launching, hello not yet exchanged |
| `kIdlePreauth` | Hello accepted; waiting to be assigned a connection |
| `kAssigned` | Actively serving a client connection |
| `kDraining` | Finishing current work before stopping |
| `kStopped` | Cleanly stopped |
| `kQuarantined` | Suspended due to repeated failures |

A worker moves to `kQuarantined` after repeated failures. The quarantine entry in `ParserWorker` includes a `quarantine_until_ms` timestamp; the worker will not be restarted until that time has elapsed. The pool tracks a `fault_history` deque to support diagnostics.

The pool maintains a target minimum and maximum worker count (`target_min`, `target_max`). When workers are quarantined, the available pool shrinks. If the pool cannot serve incoming connections because all workers are quarantined or draining, new connection requests will be refused with a message vector indicating parser unavailability.

## Route Selection and the Database Route Check

When a client attaches, the engine performs a route match. If the database associated with the connection does not match the expected route, the session registry records a `database_route_mismatch` denial. This check occurs at three attachment points:

1. During authentication handoff (`auth_handoff`)
2. During standard database attachment (`attach_database`)
3. During embedded attachment (`embedded_attach`)

Each mismatch produces an `AddAttachAdmissionDenied` diagnostic with reason `database_route_mismatch` and a rejected payload. Operators who see this denial should verify that the client is connecting to the correct listener endpoint and that the database path matches the route configured for that endpoint.

## Parser-Visible Schema Roots and Workareas

A parser workarea is the schema root set that a particular parser is allowed to resolve names against. The parser submits a `NameResolutionRequest` to the engine with a `search_path` and a `presented_name`. The engine resolves the name in the context of the session's catalog and returns the object UUID or a refusal. The parser never resolves names directly from the catalog; all name resolution flows through the engine.

Schema roots visible to parsers are a subset of the full set documented in [Identity, Security, And Policy](#ch-operations-administration-identity-security-and-policy-md). The `sys.parser` schema root (`kLocalSysParserSchemaPath`) exposes parser registration and status information.

## Parser Package Isolation

Each parser worker is a separate operating system process. A crash, assertion failure, or memory corruption in the parser process does not affect the engine process. The worker exits, the pool records the fault event, and the pool attempts a restart with exponential backoff. The quarantine mechanism prevents a repeatedly crashing parser from consuming restart resources continuously.

Parser output (the SBLR envelope) is treated as translation evidence only until the engine independently validates it. The `NativeV3ParserPackageResult` flag `parser_is_trusted` is set by the engine after the envelope is validated, not by the parser itself.

## Unsupported Surface Refusal

If a compatibility parser receives a statement that its grammar does not support, it is expected to return a refusal rather than silently accept or partially execute. The `parser.package_admission` bootstrap policy (`registered_packages_only_v1`) governs this behavior: unregistered parsers are refused and the parser profile must match the registered package.

Statements that reference reference-tool emulations (such as `GBAK`, `GFIX`, `GSTAT`, `GSEC`, `FBSVCMGR`, `FBTRACEMGR`) are mapped to `sbsql.emulated.reference_tool_non_file` in `statement_catalog.cpp`. Statements attempting `BACKUP DATABASE` or `RESTORE DATABASE` without a full file path are mapped to `sbsql.emulated.backup_restore_non_file`. These emulated surfaces are not native administrative operations.

Similarly, shadow filespace operations (`CREATE SHADOW`, `ALTER SHADOW`, `DROP SHADOW`) that do not use the ScratchBird filespace model are mapped to `sbsql.emulated.shadow_non_file`. Operators expecting native storage operations should use the filespace lifecycle statements described in Language Reference: Filespace (SBsql Language Reference — Syntax, page XXX).

## Diagnosing Parser Problems

When a parser is missing, incompatible, or quarantined, the message vector will identify the problem. Useful fields to examine in `ParserPoolStatus`:

- `quarantine_active` — whether the pool is in a quarantine state
- `quarantine_until_ms` — when the quarantine expires
- `recent_failure_count` — how many failures occurred recently
- `fault_history` — a history of fault events with per-event diagnostics

The `ParserPoolFaultEvent` struct includes the `diagnostic` string for each failure and whether the fault was `intentional` (i.e., triggered by an operator stop command rather than a crash).

Operators can restart a specific worker with the pool's `RestartWorker` method or kill a misbehaving connection with `KillConnection`.

If a hello is refused, inspect the `MessageVectorSet` in the `ParserHelloResult`. The code and reason in the refused result indicate whether the rejection was a protocol version mismatch, an unrecognized dialect, or a policy refusal.

## Operator Checklist

- Confirm that the parser binary path in `parser.worker_binary` is correct before starting the listener.
- Set `parser.protocol_min` and `parser.protocol_max` consistently with the parser binary version.
- Do not modify the four `compatibility.*` keys in parser templates unless you understand the security implications; all four are `denied` by default for good reason.
- Monitor `quarantine_active` and `recent_failure_count` in the parser pool status to detect parser stability problems early.
- A `database_route_mismatch` denial is a configuration problem, not a credential problem. Check the listener endpoint and database route binding.

## Related Pages

- [Configuration Reference](#ch-operations-administration-configuration-reference-md)
- [Identity, Security, And Policy](#ch-operations-administration-identity-security-and-policy-md)
- Language Reference: Database (SBsql Language Reference — Syntax, page XXX)
- Language Reference: Filespace (SBsql Language Reference — Syntax, page XXX)
- Getting Started: Engine Parser Boundary (ScratchBird — Concepts and Getting Started, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Operations_Administration/database_lifecycle.md -->

<a id="ch-operations-administration-database-lifecycle-md"></a>

# Database Lifecycle

## Purpose

A ScratchBird database passes through a well-defined sequence of states from creation through normal operation to shutdown or drop. Understanding these states helps operators diagnose why an open is refused, why a database is in maintenance mode, or why the engine requires recovery before admitting ordinary work.

This chapter describes each lifecycle operation, the conditions that cause the engine to refuse or restrict an open, the durable evidence the engine records at each phase, and a smoke test you can run against a disposable database to confirm end-to-end behavior.

## Lifecycle Phases

The engine tracks each database's current phase in `DatabaseLifecyclePhase`:

| Phase | Meaning |
|-------|---------|
| `created` | Bootstrap transactions committed; not yet opened for ordinary work |
| `opened` | Available for ordinary connections |
| `closed` | Cleanly shut down |
| `maintenance` | Entered maintenance mode; ordinary attaches blocked |
| `restricted_open` | Restricted-open mode active |
| `inspected` | An INSPECT DATABASE command completed |
| `verified` | A VERIFY DATABASE command completed |
| `repaired` | A REPAIR DATABASE command completed |
| `dropped` | Drop evidence recorded |
| `quarantined` | Engine placed the database in quarantine due to ambiguous identity or integrity failure |
| `failed` | Lifecycle operation failed |

The engine also tracks a `StartupLifecycleDurablePhase` in the startup state record on disk. This persisted phase is distinct from the in-memory phase and is used during recovery to determine what operations were completed before a crash.

## Database Identity and Catalog Bootstrap

Every database has a stable UUID assigned at creation time. The startup state record (`startup_state.cpp`) stores both the `database_uuid` and the `first_filespace_uuid` in its on-disk header at fixed offsets, along with a startup magic byte sequence `SBSTV001`. These fields are checked at every open to confirm that the file being opened belongs to the expected database.

At creation, the engine runs two bootstrap transactions:

- **Transaction 1 (tx1)** seeds the catalog with all bootstrap schema roots (`sys`, `sys.catalog`, `sys.security`, `sys.metrics`, `sys.audit`, `sys.storage`, `sys.parser`, `sys.diagnostics`, `sys.information`, `sys.information_schema`, `users`, `users.public`, `remote`, `emulated`, and others), the built-in datatypes, default policies, agent configurations, resource seeds, and the initial security principal. Evidence flag `bootstrap_tx1_committed` is set after tx1 completes.
- **Transaction 2 (tx2)** activates the runtime: it starts agents, loads IPC, enables the server listener, and completes the first-open activation. Evidence flag `first_open_tx2_committed` is set after tx2 completes. The `lifecycle.first_open.tx2_activation` policy (`runtime_activation_v1`) governs this: ordinary work cannot proceed until tx2 commits.

The bootstrap transaction ID for tx1 is defined as `kBootstrapCatalogTransactionId = 1`. The first-open activation transaction is `kFirstOpenActivationLocalTransactionId = 2`.

## Recovery Classification

When the engine opens a database file, it reads the startup state record and classifies the prior session's cleanup:

| Classification | Meaning |
|----------------|---------|
| `clean_checkpoint_path` | Prior session shut down cleanly; no recovery needed |
| `checkpoint_rebuild_required` | Checkpoint must be rebuilt before ordinary work |
| `repaired_recovery` | File was previously repaired; verify state before proceeding |
| `fence_writes_until_safe` | Write admission must remain fenced until recovery completes |
| `corruption_stop` | Corruption detected; restricted open or repair required |
| `restricted_open_required` | Engine requires restricted-open mode |
| `operator_review_required` | Engine requires explicit operator action before opening |

The `lifecycle.recovery_dirty_open` bootstrap policy (`mga_recovery_first_v1`) governs the recovery path. A dirty open (prior session did not record `clean_shutdown`) triggers MGA (Multi-Generation Architecture) transaction recovery before any ordinary work is admitted. The write admission fence (`kFlagWriteAdmissionFenced` in the startup state flags) prevents new writes until recovery determines it is safe.

The startup state record also tracks the `kFlagCleanShutdown` and `kFlagStartupDirty` flags. A file where `kFlagStartupDirty` is set and `kFlagCleanShutdown` is not set indicates the database was not cleanly closed; recovery runs before new work is admitted.

## What Happens When an Open Is Refused

Several conditions cause the engine to refuse an ordinary open and return a diagnostic code:

**Format version downgrade refused.** If the file's format minor version is below `kDatabaseFormatMinorMinSupported`, the engine returns `FORMAT.VERSION_DOWNGRADE_REFUSED`. This prevents an older engine version from opening a file that was written by a newer build that advanced the minor version beyond the minimum the older build understands. The diagnostic includes both the file's format version and the supported minimum.

**Format version unsupported.** If the format major or minor version exceeds `kDatabaseFormatMajorMaxSupported` / `kDatabaseFormatMinorMaxSupported`, the engine returns `FORMAT.VERSION_UNSUPPORTED`. The file was created by a newer build that the current installation cannot read.

**Unknown required compatibility flag.** If the file's `compatibility_flags` field contains bits the engine does not recognize, the engine returns `FORMAT.UNKNOWN_REQUIRED_FLAG`. This prevents an older engine from silently ignoring features it does not implement.

**Database route mismatch.** If the database's route does not match the connection's expected route, the session registry denies attachment with `database_route_mismatch`. See [Parser Registration And Routes](#ch-operations-administration-parser-registration-and-routes-md) for details.

**Maintenance mode ordinary attach blocked.** The `lifecycle.maintenance_restricted` policy (`authorized_fence_v1`) sets `ordinary_attach_blocked` when the database is in maintenance mode. Ordinary sessions attempting to attach receive a denial. Only connections with the appropriate authority can attach in this state.

## Maintenance Mode

Maintenance mode is entered with `ENTER DATABASE MAINTENANCE` (or equivalent forms: `ENTER MAINTENANCE`, `SET DATABASE MAINTENANCE`, `ALTER DATABASE ... MAINTENANCE`). It is exited with `EXIT DATABASE MAINTENANCE` (or `EXIT MAINTENANCE`, `CLEAR DATABASE MAINTENANCE`, `ALTER DATABASE ... MAINTENANCE EXIT`).

Entering maintenance mode requires authority (`enter_requires_authority` in the policy). Once entered, ordinary attaches are blocked (`ordinary_attach_blocked`). Verify operations are still permitted in maintenance mode (`verify_allowed`). Repair operations require explicit authority even in maintenance mode (`repair_requires_explicit_authority`).

The durable lifecycle phase transitions to `maintenance_entered` when maintenance mode begins and to `maintenance_exited` when it ends. Evidence flag `maintenance_evidence_recorded` is set.

## Restricted Open Mode

Restricted-open mode is a lighter fence than maintenance mode. It is entered with `OPEN DATABASE ... RESTRICTED` or `ENTER RESTRICTED OPEN` and exited with `EXIT RESTRICTED OPEN`. The durable phase transitions to `restricted_open_entered` and `restricted_open_exited`.

The startup recovery classification `restricted_open_required` causes the engine to require restricted-open mode before ordinary work is admitted. This occurs when the engine detects a state that requires operator confirmation but does not require full maintenance-mode intervention.

## Inspect, Verify, and Repair

Three diagnostic operations are available after a database is opened:

- **INSPECT DATABASE** (also `DIAGNOSE DATABASE`) — reads and reports on the database state without modifying it. Phase transitions to `inspected`.
- **VERIFY DATABASE** — performs structural verification of catalog and storage consistency. Phase transitions to `verified`. Evidence flag `verify_evidence_recorded` is set.
- **REPAIR DATABASE** (also `ALTER DATABASE ... REPAIR`) — attempts to repair structural damage. Phase transitions to `repaired` on success or `repair_refused` on a policy refusal. Evidence flags `repair_evidence_recorded` and `repair_refusal_evidence_recorded` are used accordingly. Repair requires explicit authority even in maintenance mode.

## Shutdown

Graceful shutdown uses `SHUTDOWN DATABASE` and follows the `lifecycle.shutdown_graceful_drain` policy (`drain_then_close_v1`): new work is fenced, active connections are drained, components are notified, and the database is closed after all active transactions commit or roll back. A configurable timeout governs the drain. The `clean_shutdown_local_transaction_id` field and `kFlagCleanShutdown` flag are written to the startup state record on clean close. Evidence flag `clean_shutdown_tx_committed` is set.

Force shutdown uses `FORCE SHUTDOWN DATABASE` or `SHUTDOWN DATABASE ... FORCE` and follows the `lifecycle.shutdown_force` policy (`explicit_force_only_v1`). Force shutdown terminates only the scope of the target database; other databases are not affected (`terminate_target_database_scope_only`). MGA recovery evidence is preserved so that recovery can proceed on the next open.

`ACKNOWLEDGE SHUTDOWN DATABASE` (or `SHUTDOWN ACKNOWLEDGE DATABASE`) acknowledges a pending shutdown state.

## Drop

`DROP DATABASE` records drop evidence in the startup state (`drop_evidence_recorded` flag) before performing any physical cleanup. The phase transitions to `dropped`.

## Smoke Test

The following sequence exercises the essential lifecycle path on a disposable database. Replace `<path>` with a temporary location and adjust syntax to match your connected parser dialect (see Language Reference: Database (SBsql Language Reference — Syntax, page XXX)):

```sql
-- 1. Create a disposable database
CREATE DATABASE '<path>/smoke_test.sbd';

-- 2. Create schema and a table
CREATE SCHEMA test_schema;
CREATE TABLE test_schema.items (id INTEGER, label VARCHAR(100));

-- 3. Insert rows and commit
INSERT INTO test_schema.items VALUES (1, 'alpha');
INSERT INTO test_schema.items VALUES (2, 'beta');
COMMIT;

-- 4. Detach the session
DETACH DATABASE;

-- 5. Reopen
OPEN DATABASE '<path>/smoke_test.sbd';

-- 6. Verify committed rows survived the close/reopen cycle
SELECT id, label FROM test_schema.items ORDER BY id;
-- Expected: rows (1, 'alpha') and (2, 'beta')

-- 7. Test a controlled refusal: attempt maintenance without authority
-- (Expected: denied with ordinary_attach_blocked or enter_requires_authority)
ENTER DATABASE MAINTENANCE;

-- 8. Clean up
DROP DATABASE '<path>/smoke_test.sbd';
```

If step 6 does not return both rows, examine the startup state's `kFlagCleanShutdown` result from the prior detach. If step 7 does not produce a refusal diagnostic, the authority model for the current session may have more privilege than expected — review grant configuration.

## Related Pages

- [Filespaces And Storage](#ch-operations-administration-filespaces-and-storage-md)
- [Backup, Restore, And Data Movement](#ch-operations-administration-backup-restore-and-data-movement-md)
- Language Reference: Database (SBsql Language Reference — Syntax, page XXX)
- [Operating Modes Runbook](#ch-operations-administration-operating-modes-runbook-md)




===== FILE SEPARATION =====

<!-- chapter source: Operations_Administration/filespaces_and_storage.md -->

<a id="ch-operations-administration-filespaces-and-storage-md"></a>

# Filespaces And Storage

## Purpose

A filespace is the engine's unit of physical storage allocation. Every ScratchBird database is backed by at least one filespace (the active primary), and administrators can attach additional secondary filespaces to spread data across volumes, separate index storage from row storage, or hold overflow and history. This chapter explains how filespace identity works, which operations are native administrative operations versus emulated compatibility operations, how the engine responds when storage is unhealthy, and how a filespace move proceeds.

## The Active Primary Filespace

Every open database has exactly one active primary filespace. Its `physical_filespace_id` is always `kActivePrimaryPhysicalFilespaceId = 0` (defined in `filespace_identity.hpp`). The active primary:

- Holds the startup state record that encodes the `database_uuid` and `first_filespace_uuid`.
- Is the startup authority (`startup_authority = true` in `FilespaceDescriptor`).
- Is the catalog persistence owner (`catalog_persistence_owner = true`).
- Is the filespace manifest owner and recovery evidence owner.

A page ID in ScratchBird encodes both the physical filespace number (16 bits) and the page number (48 bits, up to `kMaxFilespacePageNumber = (1 << 48) - 1`). The reserved physical filespace ID `kReservedPhysicalFilespaceId = 1` is not allocatable.

The active primary is required to open the database. If it is missing, absent, or unreadable, the engine cannot open the database at all. Ordinary sessions cannot proceed without the primary filespace online.

## Filespace Roles

Each filespace carries a `FilespaceRole` that describes its purpose:

| Role | Meaning |
|------|---------|
| `active_primary` | The primary filespace; required for open |
| `primary_shadow` | A shadow copy of the primary for high availability |
| `primary_snapshot` | A snapshot of the primary |
| `primary_candidate` | A secondary being promoted to primary |
| `secondary_data` | Additional row storage |
| `secondary_index` | Dedicated index storage |
| `secondary_overflow` | Overflow data (large rows) |
| `secondary_history` | Historical versions for MGA |
| `secondary_shard` | Shard storage in partitioned configurations |
| `archive_history` | Archived historical data |
| `archive_log` | Archived transaction log segments |
| `archive_detached` | Archived filespace no longer online |
| `temporary` | Temporary spill storage |
| `import_candidate` | Being evaluated for import into the database |
| `drop_pending` | Scheduled for deletion |
| `forbidden` | Not usable |

## Filespace States

Each filespace also carries a `FilespaceState` describing its current availability:

| State | Meaning |
|-------|---------|
| `online` / `attached` | Active and writable |
| `read_only` | Attached but not writable |
| `detached` | Not currently attached |
| `archived` | Archived; not online |
| `deleted` / `dropped` | Removed |
| `creating` | Being created |
| `initializing` | Being initialized |
| `maintenance` | Under maintenance |
| `moving` | Physical relocation in progress |
| `relocating_objects` | Objects being moved between filespaces |
| `promoting` | Being promoted to primary |
| `demoting` | Being demoted from primary |
| `detaching` | Detach in progress |
| `drop_pending` | Scheduled for drop |
| `quarantine` | Suspended due to integrity or identity concern |
| `forbidden` | Not usable |

## Filespace Operations

Native filespace lifecycle operations are defined by the `FilespaceOperation` enum in `filespace_lifecycle.hpp`:

| Operation | SQL surface |
|-----------|-------------|
| `create_filespace` | `CREATE FILESPACE` |
| `attach_filespace` | `ALTER DATABASE ... ATTACH FILESPACE` |
| `detach_filespace` | `ALTER DATABASE ... DETACH FILESPACE` |
| `promote_filespace` | Promote a secondary to primary |
| `demote_filespace` | Demote the current primary |
| `set_read_only` | Mark a filespace read-only |
| `set_read_write` | Restore write access |
| `verify_filespace` | Structural verification |
| `compact_filespace` | Reclaim free space |
| `drop_filespace` | Remove filespace and catalog entry |
| `move_filespace` | Relocate physical files |
| `quarantine_filespace` | Operator-initiated quarantine |
| `repair_filespace` | Structural repair |
| `rebuild_filespace` | Full rebuild from sources |
| `salvage_filespace` | Best-effort recovery of salvageable data |

These are native ScratchBird operations. They are distinct from emulated compatibility operations such as `ALTER DATABASE ... FILE` (mapped to `sbsql.emulated.database_file_management` in the statement catalog). Operators should use native filespace operations for production storage management.

## Filespace Identity Verification

Each filespace carries a `writer_identity_uuid` in its descriptor. The foreign filespace quarantine mechanism (`foreign_filespace_quarantine.cpp`) checks whether a filespace being attached belongs to the database by verifying that the identity UUID matches. If the filespace was written by a different database instance, it is classified as foreign and placed in quarantine state (`FilespaceState::quarantine`) with the reason `import_into_foreign_filespace_quarantine`. The metric `sb_foreign_filespace_quarantine_total` is incremented for each such event.

This check prevents accidental or malicious attachment of a filespace from a different database. Quarantine is the safe-fail outcome when identity cannot be confirmed.

## Filespace Lifecycle Policy

The `FilespaceLifecyclePolicy` struct governs which operations are permitted at each stage. Key restrictions:

- Primary detach is disabled by default (`allow_primary_detach = false`).
- Physical deletion requires all retention and legal-hold conditions to be satisfied (`physical_delete_retention_satisfied`, `physical_delete_legal_hold_clear`).
- Most destructive operations (detach, promote, drop, quarantine, move, merge, repair, rebuild, salvage) require that no active pins are held on the filespace.
- A filespace move requires `allow_filespace_move = true`, `page_agent_relocation_complete_for_move = true`, and `startup_open_safe_for_move = true`.
- Attach requires a valid physical header by default (`require_physical_header_for_attach = true`).
- Evidence is recorded before the operation succeeds (`evidence_before_success = true`), not after.

## Filespace Pins

A `FilespacePin` holds a filespace in a particular state to prevent it from being dropped, moved, or detached while a long-running operation depends on it. Pin kinds are:

| Kind | Owner |
|------|-------|
| `page_owner` | Page cache or allocation system |
| `transaction` | Active transaction |
| `backup` | Backup operation |
| `archive` | Archive operation |
| `catalog` | Catalog access |
| `external` | Operator or external system hold |

Most lifecycle operations that could disrupt an active owner require that no pins of the relevant kind are present. A blocked operation returns a `FilespaceLifecycleBlocker` with the blocker kind, owner subsystem, reason, and evidence UUID.

## Filespace Growth and Preallocation

When insert pressure exceeds available free space, the engine requests filespace growth through `RequestInsertFilespaceGrowth`. Growth urgency is classified as `background`, `normal`, `high`, or `critical`. The wait policy for a growth request can be `no_wait`, `bounded_wait`, `background_only`, or `refused`.

The engine can also preallocate pages ahead of demand via `PreallocateFilespace`. Preallocation allows the physical file to grow incrementally rather than in large bursts at high-urgency moments. The preallocation state transitions through `absent`, `admitted_pending_allocation`, `allocation_complete`, `refused`, and `quarantine`.

Physical growth of the underlying file is tracked separately in `FilespacePhysicalGrowthEntry`. A physical growth operation extends the file, syncs it to disk, and updates the physical header before updating the metadata. The field `physical_extension_synced` must be `true` before the operation is considered complete. If the sync fails, the operation remains in an incomplete state and recovery must determine whether to retain or roll back the partial growth.

Growth operations that encounter an unresolvable problem transition to `quarantine` state. Recovery classification for growth (`ClassifyFilespaceGrowthForRecovery`) assigns one of `no_action`, `complete`, `roll_back`, `quarantine`, or `fail_closed`.

## What Happens When a Filespace Is Missing or Unavailable

If a secondary filespace is missing at startup, the database may still open if the primary is intact. The secondary's state will show as `absent` or `detached`. The engine applies the `FilespaceOpenSafetyMode` for the filespace:

| Safety mode | Meaning |
|-------------|---------|
| `normal` | No restriction; filespace is online |
| `read_only` | Attached in read-only mode |
| `maintenance` | Under maintenance |
| `restricted_open` | Restricted-open mode required |
| `recovery_required` | Recovery must complete before use |

A secondary filespace in `recovery_required` mode prevents the data it owns from being written until recovery completes. Reads may proceed depending on the specific data affected.

If the active primary filespace is missing, the database cannot open. The engine returns an identity or format error diagnostic. The startup state record cannot be read, so the engine cannot verify the database UUID or confirm which transaction was the last clean commit.

## How a Filespace Move Proceeds

A filespace move is a multi-phase operation governed by `FilespaceMovePlan` in `filespace_secondary.hpp`. The plan records:

- `source_path` — where the filespace currently lives
- `target_path` — where it will live after the move
- `operator_approved` — explicit operator approval recorded
- `page_agent_relocation_complete` — the page agent has finished moving all page references
- `startup_open_safe` — the engine has confirmed that the next startup can open the filespace at the target path

To verify a move is complete, inspect all three fields. A move where `page_agent_relocation_complete = false` means objects are still referencing the source location. A move where `startup_open_safe = false` means the startup authority has not yet confirmed the new path is safe to use on restart. Both must be true before the move is considered complete.

If a move is blocked, the result carries a `blockers` vector with one or more `FilespaceLifecycleBlocker` entries. Common blockers include active transactions, backup operations, and page allocations in progress.

## Low Space and Disk-Full Behavior

The `SecondaryFilespacePolicy` sets:

- `min_free_pages = 4` — the minimum number of free pages before further writes are refused.
- `target_free_pages = 8` — the target free page count that growth tries to maintain.
- `low_water_ratio = 0.50` — the ratio at which growth urgency escalates.

When free pages fall below the minimum, insert operations that would require new page allocations are refused. The growth urgency escalates from `background` to `normal` to `high` to `critical` as capacity tightens. At `critical` urgency, the engine uses the `bounded_wait` or `refused` wait policy depending on configuration.

`allow_auto_extend = true` by default. `allow_auto_shrink = false` by default — the engine does not automatically shrink filespaces.

## Storage Health Checks

The disk health API (`CheckDiskDeviceHealth` in `database_lifecycle.cpp`) checks the underlying file device before and during operations. The `DiskHealthSnapshot` captures the current health state. A filespace verify operation (`verify_filespace`) performs a structural check independent of the health snapshot.

## Relationship to Diagnostics and Support Bundles

Filespace state, quarantine events, and growth records are included in diagnostics. The support bundle excludes protected material (encryption keys associated with encrypted filespaces) but includes filespace path and state information. As noted in [Identity, Security, And Policy](#ch-operations-administration-identity-security-and-policy-md), paths in support bundles are replaced with `[path-redacted]`. Operators collecting a support bundle from a system with filespace problems should also capture the raw filespace directory listing via an administrative query to `sys.storage` before generating the bundle.

## Operator Questions Answered

**Where are durable files allowed to live?** The engine does not restrict the filesystem path for filespace files beyond what the operating system enforces. However, the storage filespace profile policy (`storage.filespace_profile`, `single_active_primary_v1`) requires exactly one active primary. Path is not identity — two filespaces at different paths with the same UUID will cause an ambiguous identity refusal.

**Which filespace is required to open the database?** The active primary (physical ID 0). It must be present, readable, and carry a valid format header. Secondary filespaces are optional for open but required for any data that lives exclusively in them.

**What happens if a filespace is missing or unavailable?** If secondary: the database may open with restricted capabilities; data on the missing filespace is inaccessible until it is reattached. If primary: the database cannot open.

**Which operations are native administrative operations rather than compatibility operations?** Native: `CREATE FILESPACE`, `ALTER DATABASE ATTACH FILESPACE`, `ALTER DATABASE DETACH FILESPACE`, promote/demote, verify, compact, drop, move, repair. Compatibility emulations: `ALTER DATABASE ... FILE`, `CREATE SHADOW` (non-filespace form), `BACKUP DATABASE` without a full path. Use native filespace operations in new deployments.

## Related Pages

- [Database Lifecycle](#ch-operations-administration-database-lifecycle-md)
- [Diagnostics, Message Vectors, And Support Bundles](#ch-operations-administration-diagnostics-message-vectors-and-support-bundles-md)
- Language Reference: Filespace (SBsql Language Reference — Syntax, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Operations_Administration/backup_restore_and_data_movement.md -->

<a id="ch-operations-administration-backup-restore-and-data-movement-md"></a>

# Backup, Restore, And Data Movement

## Purpose

This chapter explains how ScratchBird moves data in and out of a database: what operations are admitted, what is refused at the parser boundary, and why the distinction matters for operators who come from Firebird or PostgreSQL backgrounds.

## The Core Distinction: Logical vs Physical

ScratchBird performs **logical** data movement — everything flows through admitted SQL or UDR surfaces with engine authority, transaction semantics, and policy enforcement applied. It does **not** permit physical page-copy, server-local file manipulation, or the invocation of donor engine utilities. This is a deliberate security and isolation boundary, not a gap.

Administrators familiar with Firebird's `GBAK`/`GFIX`/`NBACKUP` or PostgreSQL's `COPY PROGRAM` / `COPY TO file` should expect those surfaces to be refused. ScratchBird parser compatibility layers receive those statements, recognize them, and emit a controlled diagnostic refusal rather than silently ignoring or mis-routing them.

### Allowed-vs-Denied Summary

| Operation | Admitted? | Notes |
|---|---|---|
| `BACKUP DATABASE TO <uri>` | Yes | Logical backup stream |
| `RESTORE DATABASE FROM <uri>` | Yes | Logical restore stream |
| `BACKUP DATABASE` (no `TO`) | No | Emulated: `sbsql.emulated.backup_restore_non_file`, code `SBSQL.EMULATION.NON_FILE_OPERATION` |
| `RESTORE DATABASE` (no `FROM`) | No | Same emulation boundary |
| `NBACKUP` | No | Physical page-copy tool; refused via `sbsql.emulated.backup_restore_non_file` |
| `GBAK` / `GFIX` / `GSTAT` / `GSEC` / `FBSVCMGR` / `FBTRACEMGR` | No | Reference native tools; refused via `sbsql.emulated.reference_tool_non_file`, code `SBSQL.EMULATION.REFERENCE_TOOL_NOT_EXECUTED` |
| `COPY PROGRAM <cmd>` (PostgreSQL compat) | No | Cannot spawn host programs from parser authority |
| `COPY TO <file>` (PostgreSQL compat) | No | Cannot perform compatibility filesystem writes |
| `COPY TO STDOUT` (PostgreSQL compat) | Yes | Remote logical export stream routed through trusted package policy |
| Stream ops: `stream_open`, `stream_read`, `stream_write`, `stream_close` | Yes | UDR bridge stream operations |
| CDC ops: `cdc_start`, `cdc_read`, `cdc_apply` | Yes | Change-data-capture operations |
| `proxy_live_migration` / `cutover` | Yes | Live migration with validated evidence requirement |
| ETL (`mysql.udr.etl.load_data_local_infile`) | Yes | ETL load through UDR surface |
| `ALTER DATABASE ... FILE` | No | Database file management; refused via `sbsql.emulated.database_file_management` |

Sources: `src/parsers/sbsql_worker/statement/statement_catalog.cpp:870-890`, `src/parsers/compatibility/postgresql/postgresql_dialect.cpp:28-54`, `src/parsers/compatibility/firebird/firebird_dialect.cpp:1380`.

---

## Logical Backup and Restore

### Syntax

```sql
BACKUP DATABASE TO '<uri>';
RESTORE DATABASE FROM '<uri>';
```

The `TO` and `FROM` clauses are required. A `BACKUP DATABASE` without `TO` is caught and refused with diagnostic code `SBSQL.EMULATION.NON_FILE_OPERATION` on channel `diagnostic.lifecycle.message_vector`. This protects operators from accidentally issuing a partial statement that would otherwise be ambiguous.

### What a Logical Backup Contains

A logical backup captures the committed, consistent state of database objects and data as a stream. It does not capture raw page images, and it is not equivalent to a filesystem-level snapshot of the data files. As a result:

- The restore can run on a build with a compatible schema format.
- The backup stream passes through policy enforcement; sensitive columns subject to protection policy are handled per the configured redaction and protection rules.
- Backup and restore are admitted through the engine, so they respect transaction cleanup horizons and do not capture uncommitted or partially cleaned row versions.

### Restore Drills

A backup that has never been successfully restored is an untested assumption. Before relying on a backup for recovery, perform a restore drill:

1. Restore to a staging database (not the live copy).
2. Run schema smoke queries to confirm catalog integrity.
3. Run a representative data sample query to confirm row-level data integrity.
4. Record the restore timestamp and outcome in your operations log.

See [Release Validation Checklist](#ch-operations-administration-release-validation-checklist-md) for the restore drill steps required before a build is trusted.

---

## Stream-Based Data Movement

ScratchBird's UDR bridge supports continuous and bulk data movement through a stream protocol. The supported operations are:

| Operation | Purpose |
|---|---|
| `stream_open` | Open a data stream between source and target |
| `stream_read` | Read a batch from the source stream |
| `stream_write` | Write a batch to the target stream |
| `stream_close` | Finalize and close the stream |
| `cdc_start` | Start a change-data-capture feed from a source |
| `cdc_read` | Read a CDC batch |
| `cdc_apply` | Apply a CDC batch to the target |
| `proxy_route` | Route a query through a live proxy |
| `compare_result` | Validate source and target agreement |
| `cutover` | Execute the final live migration cutover |

Source: `src/udr/sbu_sbsql_parser_support/sbu_sbsql_parser_support.cpp:216-266`.

### Supported Topology Types

The bridge capabilities declaration lists the following topology types:

- `outbound_federation`
- `inbound_cdc`
- `outbound_replication`
- `proxy_live_migration`
- `sb_to_sb`
- `logical_backup_restore`

Source: `src/udr/sbu_sbsql_parser_support/sbu_sbsql_parser_support.cpp:295-296`.

### Cutover Requirements

A `cutover` operation requires validated evidence before it is admitted. Specifically, the context packet must carry `cutover_evidence=validated`. If `compare_result` has not produced validated evidence and that evidence has not been placed in the context packet, the cutover is refused with `UDR.BRIDGE.CUTOVER_FAILED`. This requirement exists so that a live cutover cannot proceed without documented proof that source and target are in agreement.

**Cutover** means: the final act of switching active traffic from the source system to the migrated ScratchBird database. It is irreversible in the sense that once traffic is live on the target, the source system is no longer the authoritative copy. Do not cutover without a validated compare step.

Source: `src/udr/sbu_sbsql_parser_support/sbu_sbsql_parser_support.cpp:432-435`.

---

## CDC and Replication

**CDC (Change-Data Capture)** is the process of reading the change log from a source database and applying it incrementally to ScratchBird. This is how live migrations maintain low-latency sync between source and target during a cutover window.

**Replication** in the outbound direction (`outbound_replication`) streams committed changes from ScratchBird to a downstream consumer.

Both modes are stream operations routed through the UDR bridge and require the appropriate topology and operation capability to be present in the registered parser package.

---

## ETL Workflows

ETL (Extract, Transform, Load) workflows import data from external sources. The MySQL surface supports `load_data_local_infile` through the UDR ETL surface (`mysql.udr.etl.load_data_local_infile`). ETL operations are subject to the same policy enforcement and transaction semantics as any other admitted write.

---

## Denied Physical Operations: What Operators See

When a Firebird-syntax statement attempts to invoke `GBAK`, `GFIX`, `GSTAT`, `GSEC`, `FBSVCMGR`, or `FBTRACEMGR`, the SBsql parser catches it and emits:

```
diagnostic_code: SBSQL.EMULATION.REFERENCE_TOOL_NOT_EXECUTED
severity:        ERROR
text:            Reference native tools are not invoked by the SBSQL parser; use ScratchBird management routes.
channel:         diagnostic.lifecycle.message_vector
```

This is a controlled refusal, not a crash or timeout. The same pattern applies to `BACKUP DATABASE` without a `TO` clause and `NBACKUP`. The message explicitly tells operators to use ScratchBird management routes instead.

For the PostgreSQL compatibility surface, `COPY PROGRAM` and `COPY TO <file>` produce equivalent diagnostics stating that host-program spawning and compatibility filesystem writes are not permitted from parser authority.

---

## Related Pages

- [Diagnostics, Message Vectors, And Support Bundles](#ch-operations-administration-diagnostics-message-vectors-and-support-bundles-md)
- [Release Validation Checklist](#ch-operations-administration-release-validation-checklist-md)
- Language Reference: Backup, Restore, Replication, Migration (SBsql Language Reference — Syntax, page XXX)
- Language Reference: Refusal Vectors (SBsql Language Reference — Syntax, page XXX)
- Getting Started: Backup, Restore, And Data Movement Overview (ScratchBird — Concepts and Getting Started, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Operations_Administration/external_git_catalog_versioning.md -->

<a id="ch-operations-administration-external-git-catalog-versioning-md"></a>

# External Git Catalog Versioning

## Purpose

This chapter explains the operator workflow for the external Git catalog versioning feature: how to enable it, how to export a catalog snapshot, how to commit that snapshot to an external Git repository, how to diff the live catalog against a previously committed snapshot, how to read a rollback plan, and how to apply a reconciliation through the engine.

This is a review-and-versioning convenience. It lets a team maintain a Git-tracked history of catalog structure and plan rollbacks from it. It does not change who has authority: Git never executes against the database, and every catalog change must flow through the ScratchBird engine's authorized catalog API.

Related pages: Catalog Artifacts And External Git (language reference) (SBsql Language Reference — Syntax, page XXX), Git-Oriented Workflows (concept) (ScratchBird — Concepts and Getting Started, page XXX), [Backup, Restore, And Data Movement](#ch-operations-administration-backup-restore-and-data-movement-md), [Identity, Security, And Policy](#ch-operations-administration-identity-security-and-policy-md).

## The Authority Boundary

Before walking through the workflow, it is worth stating the boundary explicitly because it affects every step.

ScratchBird exports the catalog as content-hashed artifacts. An external Git repository can store and version those artifacts. You can diff the live catalog against a snapshot from Git and produce a rollback plan from the diff. But:

- Git is external storage for review artifacts. It has no catalog authority, no transaction authority, and no runtime authority over the database.
- Every result from an external-git operation carries explicit evidence: `git_runtime_authority = false`, `external_git_repository_authority = false`, `catalog_runtime_authority = ScratchBird_catalog_api`, `mga_transaction_authority = local_mga_transaction_inventory`.
- A diff row carries `requires_authorized_catalog_import = true`. A rollback plan row carries `apply_route = authorized_catalog_api` and `plan_runtime_authority = false`.
- The rollback result evidence carries `external_git_rollback_apply_route = authorized_catalog_api_not_git_repository`.
- The only admitted way to apply a catalog change derived from an artifact — whether from an export, a diff, or a rollback plan — is `IMPORT CATALOG ARTIFACT` through the engine under `right.catalog_mutate`.

Any request that tries to claim direct authority by carrying option `git_runtime_authority:true`, `external_git_direct_authority:true`, or `external_git_direct_apply:true` is refused with diagnostic `external_git_authority_forbidden`.

## Step 1: Enable The Policy Gate

The three engine API / SBLR opcode operations (`artifact.external_git.export_snapshot`, `artifact.external_git.diff_snapshot`, `artifact.external_git.rollback_plan`) require an explicit opt-in option on every request.

The request must include one of:

| Option | Form |
| --- | --- |
| `external_git_policy:enabled` | Preferred form. |
| `allow_external_git_versioning:true` | Alternative form. |

Without one of these, the engine refuses with `external_git_policy_required` before performing any catalog read. This is not a configuration file setting — it is a per-request option that the caller (a tool, agent, or platform integration layer) must include every time it invokes an external-git operation.

If you are building tooling, ensure the option is included in every SBLR envelope that invokes one of the three engine API operations. The two SBsql statements (`EXPORT CATALOG ARTIFACT`, `IMPORT CATALOG ARTIFACT`) do not require the policy option, though they still refuse the forbidden authority options.

## Step 2: Export A Catalog Snapshot

To obtain a snapshot suitable for Git storage, invoke `artifact.external_git.export_snapshot` through the engine artifact API or SBLR envelope with the policy option present.

The result contains:

- one **manifest row** (`snapshot_entry_kind = manifest`) carrying metadata: `database_uuid`, `local_transaction_id`, `entry_count`, `catalog_artifact_format`, and authority evidence;
- one **object row** (`snapshot_entry_kind = object`) per visible catalog object, sorted by `object_uuid`.

Each object row carries:

| Field | Content |
| --- | --- |
| `object_uuid` | Durable UUID of the catalog object. |
| `object_kind` | Type of the object (e.g. `schema`). |
| `default_name` | Resolver name. |
| `payload` | Serialized object definition. |
| `content_hash` | FNV-1a-64 hash of `object_uuid + object_kind + default_name + payload`. |
| `artifact_format` | `sb.external_git.catalog_snapshot.v1` |
| `catalog_artifact_format` | `sb.catalog.artifact.v1` |

The snapshot covers visible schema-tree records and visible API-behavior records.

Alternatively, you can use the SBsql statement `EXPORT CATALOG ARTIFACT` (requires `right.catalog_read`) to obtain the same catalog objects in `sb.catalog.artifact.v1` format without the external-git snapshot envelope or authority evidence. This form does not require the policy option.

## Step 3: Commit The Snapshot To An External Git Repository

The snapshot rows are your data. Serialization, file layout, and Git commit workflows are your team's responsibility — the engine produces the row data and does not manage the external repository.

Recommended practices for the external repository:

- Store the manifest row and all object rows as files or structured data in a form your team can review and diff in pull requests.
- Include the `content_hash` field for every object row. The engine recomputes the hash and rejects any snapshot row where the supplied hash does not match (`external_git_snapshot_hash_mismatch:<uuid>`), so preserving the hash ensures round-trip integrity.
- Do not strip the authority evidence fields (`git_runtime_authority`, `external_git_repository_authority`, etc.) from stored rows. They serve as a tamper-evident reminder of the boundary.
- Use `object_uuid` as the durable identity for catalog objects. Names can change; UUIDs do not. This means two snapshots of the same database taken before and after a rename will track the rename correctly.
- Keep secrets and protected material out of the repository. Catalog `payload` fields should not contain raw secrets; they carry protected references if secrets are involved.

What you commit to Git is a versioned, diffable history of your catalog structure. You can use ordinary Git tooling — `git diff`, pull requests, blame — to review catalog changes over time.

## Step 4: Diff The Live Catalog Against A Committed Snapshot

To compare the current live catalog against a snapshot you previously committed, invoke `artifact.external_git.diff_snapshot` through the engine API or SBLR envelope with the policy option and the snapshot rows as input.

The engine:

1. Validates the policy gate.
2. Parses and validates the submitted snapshot rows (format, presence of `object_uuid` and `object_kind`, uniqueness of UUIDs, hash integrity).
3. Takes a fresh snapshot of the live catalog.
4. Compares the two sets by `object_uuid`, using `object_kind + default_name + payload` as the comparison signature.
5. Returns diff rows.

#### Diff kinds

| `diff_kind` | Meaning |
| --- | --- |
| `unchanged` | Live catalog and candidate snapshot match entirely. A single sentinel row with this kind is returned. |
| `modified` | The object exists in both but its signature differs (the definition changed). |
| `added_in_candidate` | The object is in the submitted candidate snapshot but not in the live catalog. |
| `removed_from_candidate` | The object is in the live catalog but not in the submitted candidate snapshot. |

Every diff row carries `requires_authorized_catalog_import = true`. The diff is a review output; it does not apply anything.

The result evidence includes `external_git_diff_count` (the count of changed objects) and the full authority evidence block (`git_runtime_authority = false`, `external_git_repository_authority = false`, `catalog_runtime_authority = ScratchBird_catalog_api`, `mga_transaction_authority = local_mga_transaction_inventory`).

## Step 5: Read A Rollback Plan

If you want to understand what changes are needed to restore the live catalog to a previously committed state, invoke `artifact.external_git.rollback_plan` with the target snapshot rows as input.

The engine produces one plan row per object that requires action. Plan rows use format `sb.external_git.rollback_plan.v1` and carry:

| Field | Content |
| --- | --- |
| `rollback_action` | Action required (see table below). |
| `object_uuid` | UUID of the object. |
| `object_kind` | Object type. |
| `default_name` | Resolver name. |
| `payload` | Serialized object definition. |
| `restore_hash` | Hash of the live-catalog version of the object. |
| `target_hash` | Hash of the target snapshot version (empty when the object is absent from the target). |
| `apply_route` | `authorized_catalog_api` |
| `git_runtime_authority` | `false` |
| `plan_runtime_authority` | `false` |

#### Rollback actions

| `rollback_action` | Meaning | Remediation |
| --- | --- | --- |
| `restore_current_catalog_artifact` | The live object differs from or is absent from the target. | Import the live-catalog version via `IMPORT CATALOG ARTIFACT` to re-establish it in the target state, or use the target's payload to import the desired version. |
| `reject_candidate_only_object_until_authorized_catalog_create` | The target snapshot has an object the live catalog does not. | The object must be created through an authorized `IMPORT CATALOG ARTIFACT` if the team decides to recreate it. |
| `no_action_required` | Live catalog already matches the target. A single sentinel row with this action is returned. | No action needed. |

The rollback plan result evidence includes `external_git_rollback_apply_route = authorized_catalog_api_not_git_repository` and `external_git_rollback_plan_count`.

The plan is advisory. It tells you what the engine would need applied to reconcile the live catalog to the target. It does not apply those changes itself.

## Step 6: Apply A Reconciliation Through The Engine

To act on a diff or a rollback plan, use `IMPORT CATALOG ARTIFACT` — the only admitted route for applying catalog artifact changes. This requires `right.catalog_mutate`.

```sql
import catalog artifact;
```

Supply the input rows using format `sb.catalog.artifact.v1`. Key import options:

| Option | Default | Notes |
| --- | --- | --- |
| `conflict_policy:reject` | Default | Refuses if the target UUID is already visible. Use when you expect new objects only. |
| `conflict_policy:replace` | — | Overwrites an existing object with the same UUID. Use when reconciling modified objects. |
| `uuid_mode:preserve` | Default | Uses the `object_uuid` from each row as the target UUID. Correct for round-tripping existing catalog objects. |
| `uuid_mode:remap` | — | Uses the `remap_uuid` field as the target UUID. Use only when intentionally remapping identity. |
| `allow_name_conflict:true` | — | Suppresses schema-path conflict checking. Use only when you have independently verified that name conflicts are safe. |
| `external_git_policy:enabled` | — | Not required for import, but when present causes the result to emit `external_git_import_authority = authorized_catalog_api_not_git_repository`. |

When importing a reconciliation derived from a rollback plan:

- For `restore_current_catalog_artifact` rows where the live object needs to be replaced with the target version: use `conflict_policy:replace` and supply the target payload.
- For `reject_candidate_only_object_until_authorized_catalog_create` rows: decide whether to create the object; if yes, import it as a new object with `conflict_policy:reject`.
- For objects that should remain as they are: omit them from the import batch.

The import validates all rows before applying any of them and applies the batch atomically through MGA-governed transactions. If any row fails validation, the entire batch is refused with a per-row diagnostic. On success, the result evidence includes `catalog_artifact_import_count` and one `catalog_artifact_imported = <uuid>` entry per imported object.

## Snapshot Validation: What The Engine Checks On Input

When you submit snapshot rows for diff or rollback plan, the engine validates the rows before comparing. Understanding these checks helps diagnose submission problems:

| Diagnostic | Condition |
| --- | --- |
| `external_git_snapshot_rows_required` | No rows were submitted. |
| `external_git_snapshot_format_invalid` | A row has an `artifact_format` that is not `sb.catalog.artifact.v1` or `sb.external_git.catalog_snapshot.v1`. |
| `external_git_snapshot_object_required` | A non-manifest row has an empty `object_uuid` or `object_kind`. |
| `external_git_snapshot_duplicate_uuid:<uuid>` | The same UUID appears more than once in the submitted rows. |
| `external_git_snapshot_hash_mismatch:<uuid>` | A row supplies a `content_hash` that does not match the engine-recomputed hash. |

Hash mismatches indicate that the snapshot row was altered after the engine produced it. This can happen if rows were edited in the Git repository, if serialization changed whitespace or encoding in `payload` or `default_name`, or if the `content_hash` field was not preserved faithfully. To diagnose, re-export the snapshot from the same database state and compare.

Manifest rows (`snapshot_entry_kind = manifest`) are silently skipped; only object rows are compared.

## Complete Workflow Summary

```text
1. Include external_git_policy:enabled in every engine API request.

2. Invoke artifact.external_git.export_snapshot.
   - Result: manifest row + one object row per catalog object.
   - Each object row carries object_uuid, object_kind, default_name, payload, content_hash.

3. Store the snapshot rows in your external Git repository.
   - Commit them for review, diffing, and history.
   - Preserve content_hash fields faithfully.

4. (Later) Invoke artifact.external_git.diff_snapshot with stored rows as input.
   - Result: diff rows classified as unchanged / modified / added_in_candidate / removed_from_candidate.
   - Each diff row carries requires_authorized_catalog_import = true.

5. (If rollback is needed) Invoke artifact.external_git.rollback_plan with target rows as input.
   - Result: plan rows with rollback_action, object data, restore_hash, target_hash.
   - Result evidence: external_git_rollback_apply_route = authorized_catalog_api_not_git_repository.

6. Apply via IMPORT CATALOG ARTIFACT (right.catalog_mutate required).
   - Input rows in sb.catalog.artifact.v1 format.
   - Choose conflict_policy:reject or conflict_policy:replace as appropriate.
   - Batch commits atomically through MGA.
   - Never apply via Git directly; there is no engine-supported path to do so.
```

## Diagnostics Reference

| Diagnostic | Produced by | Meaning |
| --- | --- | --- |
| `external_git_policy_required` | All three engine API operations | Request did not include `external_git_policy:enabled` or `allow_external_git_versioning:true`. |
| `external_git_authority_forbidden` | All three engine API operations; `IMPORT CATALOG ARTIFACT` | Request included a forbidden option (`git_runtime_authority:true`, `external_git_direct_authority:true`, or `external_git_direct_apply:true`). |
| `external_git_snapshot_rows_required` | diff\_snapshot, rollback\_plan | No input rows supplied. |
| `external_git_snapshot_format_invalid` | diff\_snapshot, rollback\_plan | An input row has an unrecognized `artifact_format`. |
| `external_git_snapshot_object_required` | diff\_snapshot, rollback\_plan | A non-manifest input row is missing `object_uuid` or `object_kind`. |
| `external_git_snapshot_duplicate_uuid:<uuid>` | diff\_snapshot, rollback\_plan | The same UUID appears more than once in the input. |
| `external_git_snapshot_hash_mismatch:<uuid>` | diff\_snapshot, rollback\_plan | The supplied `content_hash` for an object does not match the engine-recomputed hash. |
| `artifact_format_invalid` | `IMPORT CATALOG ARTIFACT` | An input row does not carry `artifact_format = sb.catalog.artifact.v1`. |
| `artifact_object_uuid_required` | `IMPORT CATALOG ARTIFACT` | An input row has no `object_uuid`. |
| `artifact_object_kind_required` | `IMPORT CATALOG ARTIFACT` | An input row has no `object_kind`. |
| `artifact_uuid_conflict:<uuid>` | `IMPORT CATALOG ARTIFACT` | Target UUID already visible and `conflict_policy` is `reject`. |
| `artifact_policy_validation_failed` | `IMPORT CATALOG ARTIFACT` | Row payload contains `policy_status:invalid` or `unsafe_profile:true`. |
| `artifact_schema_path_conflict:<path>` | `IMPORT CATALOG ARTIFACT` | A schema object's name conflicts with an existing visible name. |
| `artifact_parent_schema_not_visible` | `IMPORT CATALOG ARTIFACT` | A schema object's parent UUID is not visible and not in the batch. |
| `artifact_rows_required` | `IMPORT CATALOG ARTIFACT` | No input rows supplied. |

## Security And Privilege Notes

| Concern | Rule |
| --- | --- |
| Export right | `EXPORT CATALOG ARTIFACT` requires `right.catalog_read`. |
| Import right | `IMPORT CATALOG ARTIFACT` requires `right.catalog_mutate`. This is a catalog mutation and should be granted only to operators who are authorized to change the catalog. |
| Engine API operations | Invoked through the engine internal API or SBLR envelope, not through SBsql text. Access controls are those of the invoking tool or agent. |
| Protected material | Catalog payload fields contain protected references, not raw secrets. Do not strip protection references when storing snapshot rows. |
| Sandbox | Import applies objects within the caller's sandbox root. Objects that would violate sandbox policy are refused. |
| Disclosure | Snapshot rows may contain schema structure details. Treat the Git repository as an appropriate level of confidentiality for that information. |




===== FILE SEPARATION =====

<!-- chapter source: Operations_Administration/diagnostics_message_vectors_and_support_bundles.md -->

<a id="ch-operations-administration-diagnostics-message-vectors-and-support-bundles-md"></a>

# Diagnostics, Message Vectors, And Support Bundles

## Purpose

When something goes wrong in ScratchBird — or when a parser refuses a statement, a database open is blocked, or the listener enters a degraded state — the engine produces structured diagnostic output. This chapter explains the diagnostic model, the channels through which diagnostics flow, the refusal classes operators can expect, the SBSQL diagnostic codes defined in the registry, and how to collect, review, and redact a support bundle before sharing it.

---

## The Message Vector Model

A **message vector** is a structured list of diagnostic records attached to an operation result. Rather than a single error string, ScratchBird operations produce a vector of records, each carrying:

- A `diagnostic_code` string (namespaced, dot-separated)
- A severity level (`ERROR`, `WARNING`, `INFO`, or similar)
- A human-readable detail string
- Optional structured fields

In the UDR call result struct, this is the `message_vector_json` field on `UdrCallResult`. A caller receives the full vector and can inspect each record independently.

Source: `src/udr/runtime/sb_udr_runtime.hpp:36-40`.

### Diagnostic Channels

Diagnostics are routed to named channels. Two channels are significant for operators:

| Channel | What flows through it |
|---|---|
| `diagnostic.canonical_message_vector` | General engine and parser diagnostics |
| `diagnostic.lifecycle.message_vector` | Lifecycle events, emulated statement boundaries, and startup/shutdown diagnostics |

The lifecycle channel is where refusals from emulated compatibility syntax appear. When you see `SBSQL.EMULATION.NON_FILE_OPERATION` or `SBSQL.EMULATION.REFERENCE_TOOL_NOT_EXECUTED`, they are routed through `diagnostic.lifecycle.message_vector`.

Source: `src/parsers/sbsql_worker/statement/statement_catalog.cpp:614,640,666,692`.

---

## Refusal Classes

A **refusal** is a controlled, expected operational outcome. It means the engine recognized a request, determined it cannot or should not fulfill it, and said so clearly. A refusal is not a crash, a timeout, or a silent drop.

ScratchBird defines several refusal classes for storage and page operations:

| Refusal State | Meaning |
|---|---|
| `refused` | Request was received and explicitly denied |
| `recovery_required` | The filespace or page agent is in a state that requires recovery before it can accept new work |
| `invalid_filespace_identity` | A boundary violation: the filespace identity presented is not valid for this operation |
| `invalid_page_family` | A boundary violation: the page family presented does not match the agent's domain |

Source: `src/storage/page/page_filespace_handoff.hpp:57-78`.

### UDR Bridge Refusal Codes

The SBsql parser support bridge declares the following refusal classes:

| Code | Trigger |
|---|---|
| `UDR.BRIDGE.CONTEXT_MISSING` | Required context packet absent |
| `UDR.BRIDGE.SECRET_MATERIAL_DENIED` | Secret material access not permitted from this surface |
| `UDR.BRIDGE.SANDBOX_DENIED` | Operation denied by sandbox policy |
| `UDR.BRIDGE.UNSUPPORTED` | Operation not supported by this provider |
| `UDR.BRIDGE.MISSING_CAPABILITY` | Required capability not registered |
| `UDR.BRIDGE.UNLICENSED` | Operation requires a license gate not satisfied |
| `UDR.BRIDGE.STREAM_INVALID` | Stream state is invalid for the requested operation |
| `UDR.BRIDGE.AUTH_FAILED` | Authentication check failed |
| `UDR.BRIDGE.IDEMPOTENCY_MISSING` | Idempotency token required but absent |
| `UDR.BRIDGE.CUTOVER_FAILED` | Cutover refused; validated compare evidence not present |

Source: `src/udr/sbu_sbsql_parser_support/sbu_sbsql_parser_support.cpp:312-322`.

---

## SBSQL Parser Diagnostic Codes

The SBSQL parser worker emits a defined set of diagnostic codes. Key codes operators encounter:

| Code | Condition |
|---|---|
| `SBSQL.EMULATION.NON_FILE_OPERATION` | Statement uses file-backed syntax (e.g., `BACKUP DATABASE` without `TO`, `NBACKUP`, shadow file operations) that has no filesystem side effect in SBsql |
| `SBSQL.EMULATION.REFERENCE_TOOL_NOT_EXECUTED` | Statement invokes a reference native tool (`GBAK`, `GFIX`, `GSTAT`, `GSEC`, `FBSVCMGR`, `FBTRACEMGR`) which is not executed by the SBsql parser |
| `SBSQL.SERVER.UNAVAILABLE` | Parser server is not available to process the request |
| `SBSQL.EXECUTION.REJECTED` | Execution was rejected by the engine |
| `SBSQL.RESOURCE.STATEMENT_TOO_LARGE` | Statement exceeds the allowed size limit |
| `SBSQL.RESOURCE.SBLR_ENVELOPE_TOO_LARGE` | SBLR envelope exceeds the allowed size limit |
| `SBSQL.RESOURCE.IDENTIFIER_TOO_LARGE` | Identifier exceeds the allowed size |
| `SBSQL.RESOURCE.LITERAL_TOO_LARGE` | Literal value exceeds the allowed size |
| `SBSQL.RESOURCE.PARAMETER_COUNT_EXCEEDED` | Statement has more parameters than permitted |
| `SBSQL.RESOURCE.AST_DEPTH_EXCEEDED` | Parse tree is too deeply nested |
| `SBSQL.COPY.TARGET_UUID_MISSING` | COPY stream operation is missing a target UUID |
| `SBSQL.COPY.DATA_ROW_INVALID` | A data row in a COPY stream is malformed |
| `SBSQL.COPY.NO_ROWS` | COPY stream completed with no rows |
| `SBSQL.COPY.EXECUTION_REJECTED` | COPY execution rejected by engine |
| `SBSQL.COPY.CLIENT_ABORTED` | Client aborted a COPY stream |
| `SBSQL.LIFECYCLE.MAPPED` | Statement successfully mapped to a lifecycle handler |
| `SBSQL.STATEMENT.EXACT_REFUSAL_REQUIRED` | Statement requires an exact refusal and no implicit fallback is permitted |
| `SBSQL.PARSER.EMPTY_STATEMENT` | Statement text is empty |
| `SBSQL.PARSER.STATEMENT_FAMILY_UNKNOWN` | Statement family is not recognized by the vertical-slice parser |

Sources: `src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp`, `src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp`, `src/parsers/sbsql_worker/statement/statement_catalog.cpp`, `src/parsers/sbsql_worker/ast/ast.cpp`, `src/parsers/sbsql_worker/binder/binder.cpp`.

### Lexer and Encoding Codes

The lexer emits additional codes for malformed input:

- `SBSQL.LEXER.STRING_UNCLOSED`, `SBSQL.LEXER.IDENTIFIER_UNCLOSED`, `SBSQL.LEXER.DIRECTIVE_UNCLOSED`
- `SBSQL.LEXER.DELIMITED_IDENTIFIER_UNCLOSED`
- `SBSQL.LEXER.UUID_LITERAL_INVALID`
- `SBSQL.LEXER.INVALID_CONTROL`
- `SBSQL.ENCODING.INVALID_UTF8`
- `SBSQL.UNICODE.BIDI_CONTROL_FORBIDDEN`
- `SBSQL.UNICODE.COMBINING_MARK_WITHOUT_BASE`

These indicate the input text is malformed at the lexical level, before any semantic analysis.

---

## Startup and Format Diagnostics

Database open failures produce specific codes:

| Code | Meaning |
|---|---|
| `FORMAT.VERSION_DOWNGRADE_REFUSED` | The database's format version is newer than this build supports; opening would be a downgrade |
| `ENGINE.DBLC_FORMAT_DOWNGRADE_REFUSED` | Same refusal observed at the engine lifecycle layer |
| `SB-STARTUP-STATE-FORMAT-DOWNGRADE-REFUSED` | Startup state format version is newer than this build; open refused |
| `SB-STARTUP-STATE-MAGIC-INVALID` | Startup state block has an invalid magic marker |
| `SB-STARTUP-STATE-BODY-TOO-SMALL` | Startup state block is truncated |

Sources: `src/storage/disk/database_format.cpp:194`, `src/storage/database/database_lifecycle.cpp:4361`, `src/storage/database/startup_state.cpp:214-374`.

---

## Support Bundles

A **support bundle** is a structured snapshot of operational state collected for diagnostic review. It is the first thing an escalation recipient will ask for when investigating a service problem.

### What a Bundle Contains

The listener support bundle (`ListenerSupportBundleSnapshot`) includes:

- Listener configuration (after redaction of sensitive paths)
- Socket identity
- Current lifecycle state, drain state, and stop-requested flag
- Whether new connections are being accepted
- Accept sequence, open connection count, queue depth, pending handoff bindings
- Handoff complete and reject totals
- Parser pool status
- Metrics snapshot (JSON)
- Management decision log (last up to 64 events, each with timestamp, operation, outcome, diagnostic code, and safe detail)
- Runtime event log

Source: `src/listener/listener_support_bundle.hpp:34-51`.

The manager support bundle (`GenerateManagerSupportBundle`) adds:

- Manager configuration summary (with paths redacted)
- Status JSON and metrics JSON
- Audit file reference, metrics file reference
- Lifecycle state file and lifecycle journal file
- Agent observability JSON

Source: `src/manager/node/manager_support_bundle.hpp:20-35`.

### Collecting a Bundle

The `collect_support_bundle` UDR operation is available across parser support packages. When invoked it assembles the current state and writes it to the configured bundle directory.

For integrated collection across multiple components, the Python tool `tools/ceic_integrated_support_bundle.py` can collect from listener and manager in a single pass.

### Redaction

Before a support bundle leaves your environment, verify that it has been redacted. ScratchBird applies automatic redaction:

- `RedactListenerSupportabilityText` replaces sensitive content in free-text fields.
- `RedactManagerSupportBundleText` replaces path values with `[path-redacted]` and secret refs with `<redacted-secret-ref-present>`.
- Local path policy is recorded as `local_path_policy=redacted` in the bundle manifest.
- Keyring paths and restart executable paths are replaced with `<redacted-path-present>` rather than omitted entirely, so the reviewer knows they are configured without seeing the actual values.

Source: `src/listener/listener_support_bundle.hpp:53`, `src/manager/node/manager_support_bundle.cpp:56-169`.

**Review a bundle before sharing it.** Automated redaction handles known sensitive fields. Custom configuration that places secrets or hostnames in unexpected fields may not be caught. Open the bundle files and scan for values you would not want in a support ticket.

### Late Payload Redaction

For page-level operations, `late_payload_fetch` enforces a redaction gate: a security snapshot and redaction policy must be bound before a payload is fetched. If `redaction_required` is set, any attempt to retrieve the unredacted bytes without explicit authorization produces `storage.page.late_payload_fetch.unredacted_protected`. This ensures that protected material at the page level cannot bypass the redaction policy even during diagnostic collection.

Source: `src/storage/page/late_payload_fetch.cpp:148-212`.

---

## Operator Review Checklist

Before sharing a support bundle externally:

1. Verify the `redaction_profile` line in the bundle manifest.
2. Scan `config-redacted.txt` for any path or secret values that should not be visible.
3. Confirm the audit log entries in the bundle do not contain user data payloads.
4. Confirm the metrics JSON does not contain values derived from protected content.
5. If the bundle was collected during a security incident, treat it as potentially sensitive until reviewed by your security team.

---

## Related Pages

- [Troubleshooting](#ch-operations-administration-troubleshooting-md)
- [Monitoring, Health, And Readiness](#ch-operations-administration-monitoring-health-and-readiness-md)
- Language Reference: Refusal Vectors (SBsql Language Reference — Syntax, page XXX)
- Getting Started: Diagnostics And Support Bundles (ScratchBird — Concepts and Getting Started, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Operations_Administration/monitoring_health_and_readiness.md -->

<a id="ch-operations-administration-monitoring-health-and-readiness-md"></a>

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

When `recovery_required` appears in logs or diagnostics, the affected database should not be opened for general use. See [Troubleshooting](#ch-operations-administration-troubleshooting-md) and [Database Lifecycle](#ch-operations-administration-database-lifecycle-md) for recovery procedures.

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

- [Service Lifecycle](#ch-operations-administration-service-lifecycle-md)
- [Diagnostics, Message Vectors, And Support Bundles](#ch-operations-administration-diagnostics-message-vectors-and-support-bundles-md)
- [Troubleshooting](#ch-operations-administration-troubleshooting-md)
- Getting Started: Diagnostics And Support Bundles (ScratchBird — Concepts and Getting Started, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Operations_Administration/troubleshooting.md -->

<a id="ch-operations-administration-troubleshooting-md"></a>

# Troubleshooting

## Purpose

This chapter is organized by symptom. For each problem an operator might observe, it describes the likely area, what evidence to collect, what diagnostic codes to look for, and where to go next. The goal is to get you to the right evidence quickly without guessing.

If you are seeing a controlled refusal rather than a crash, that is expected behavior — ScratchBird emits structured diagnostics rather than silent failures. A refusal with a diagnostic code is more useful than no response; the code tells you exactly which boundary was crossed.

---

## Symptom Index

1. [Service fails to start](#1-service-fails-to-start)
2. [Database open refused or format mismatch](#2-database-open-refused-or-format-mismatch)
3. [Cannot connect — IPC endpoint unavailable](#3-cannot-connect--ipc-endpoint-unavailable)
4. [Parser refused a statement](#4-parser-refused-a-statement)
5. [Authentication or authorization failure](#5-authentication-or-authorization-failure)
6. [Object not found or not visible](#6-object-not-found-or-not-visible)
7. [Transaction state invalid or recovery required](#7-transaction-state-invalid-or-recovery-required)
8. [Storage unavailable or quarantined](#8-storage-unavailable-or-quarantined)
9. [Listener is live but not ready](#9-listener-is-live-but-not-ready)
10. [Transaction pressure or slow cleanup](#10-transaction-pressure-or-slow-cleanup)
11. [Support bundle is insufficient or too broad](#11-support-bundle-is-insufficient-or-too-broad)

---

## 1. Service Fails to Start

**Symptoms**: The listener or manager process exits immediately. No IPC socket appears. Log lines appear but the process does not stabilize.

**Likely area**: Configuration, resource limits, IPC socket path, or format mismatch on a database that is opened at startup.

**Evidence to collect**:
- Startup log output up to the first error line.
- The configuration file that was passed.
- Any format-related diagnostic codes (look for `FORMAT.VERSION_DOWNGRADE_REFUSED`, `CONFIG.DOWNGRADE_REFUSED`, `IPC.LIFECYCLE.DOWNGRADE_REFUSED`).

**Safe checks to run**:
- Verify the configuration file is syntactically valid by running the validation step described in [Configuration Reference](#ch-operations-administration-configuration-reference-md).
- Check that the socket directory and runtime directory exist and the process has write permission.
- Verify the build version against any databases that are opened at startup.

**Expected diagnostic codes**:
- `CONFIG.DOWNGRADE_REFUSED` — a configuration value is incompatible with this build.
- `IPC.LIFECYCLE.DOWNGRADE_REFUSED` — the IPC protocol version is newer than this build supports.
- `FORMAT.VERSION_DOWNGRADE_REFUSED` — a database was written by a newer build.

**Escalation**: Collect a startup log and the configuration; see [Diagnostics, Message Vectors, And Support Bundles](#ch-operations-administration-diagnostics-message-vectors-and-support-bundles-md).

---

## 2. Database Open Refused or Format Mismatch

**Symptoms**: A database that previously opened now refuses to open. The error message mentions format version, downgrade refused, or startup state.

**Likely area**: The database was written by a newer build of ScratchBird than the one currently running, or the startup state block is corrupt.

**Why this happens**: ScratchBird refuses to open a database whose on-disk format version is newer than the build can interpret (`FORMAT.VERSION_DOWNGRADE_REFUSED`). This is a safety refusal — opening an unknown format silently could corrupt data. The same logic applies at the startup state level (`SB-STARTUP-STATE-FORMAT-DOWNGRADE-REFUSED`).

**Evidence to collect**:
- The exact diagnostic code from the open failure.
- The build version currently running.
- The build version that last wrote to the database (if known).

**Expected diagnostic codes**:
- `FORMAT.VERSION_DOWNGRADE_REFUSED` — database format version is newer than supported.
- `ENGINE.DBLC_FORMAT_DOWNGRADE_REFUSED` — same, observed at the engine lifecycle layer.
- `SB-STARTUP-STATE-FORMAT-DOWNGRADE-REFUSED` — startup state format is newer than supported.
- `SB-STARTUP-STATE-MAGIC-INVALID` — startup state block has an invalid magic marker (possible corruption or wrong file).
- `SB-STARTUP-STATE-BODY-TOO-SMALL` — startup state block is truncated.

**Safe checks**:
- Confirm the build version matches what wrote the database.
- If you need to upgrade the running build rather than downgrade the database, see [Upgrade And Compatibility Policy](#ch-operations-administration-upgrade-and-compatibility-policy-md).
- Do not attempt to open the database with an incompatible build. Do not attempt manual repair of the startup state block.

**Escalation**: Collect the diagnostic output and the database's format version evidence; consult [Upgrade And Compatibility Policy](#ch-operations-administration-upgrade-and-compatibility-policy-md).

---

## 3. Cannot Connect — IPC Endpoint Unavailable

**Symptoms**: A client cannot reach the listener. Connection attempts time out or are refused. The IPC socket does not exist or does not respond.

**Likely area**: Listener is not running, parser pool is not initialized, or the socket path does not match what the client is configured to use.

**Evidence to collect**:
- Check whether the listener process is running.
- Check whether the IPC socket file exists at the configured path.
- Run `listener.status` through the management interface to get the listener state snapshot.
- Look for `SBSQL.SERVER.UNAVAILABLE` in recent diagnostic output.

**Expected diagnostic codes**:
- `SBSQL.SERVER.UNAVAILABLE` — the parser server cannot accept the request.
- `PARSER_SERVER_IPC.PROTOCOL_VERSION_DOWNGRADE_REFUSED` — client is using a newer IPC protocol version than the server.

**Safe checks**:
- Verify the socket path in the listener configuration.
- Check that the listener process has not exited; check process management logs.
- If the listener is alive but `SBSQL.SERVER.UNAVAILABLE` is appearing, check parser pool status via the management interface.

**Escalation**: Collect a support bundle while the listener is alive, then see [Monitoring, Health, And Readiness](#ch-operations-administration-monitoring-health-and-readiness-md).

---

## 4. Parser Refused a Statement

**Symptoms**: A SQL statement fails with a diagnostic code, not a transaction error. The message mentions "not supported", "not executed", "emulation boundary", or similar.

**Likely area**: The statement is either unsupported in the current parser dialect, uses a syntax that is emulated-only (file-backed operations), or exceeds a resource limit.

**Understanding parser refusals**: ScratchBird's parser compatibility surfaces recognize many statement forms from Firebird, PostgreSQL, MySQL, and others. Some of those forms are admitted and executed. Others are recognized and refused with a structured diagnostic. A refusal is not a bug — it is a documented boundary.

**Common cases and their codes**:

| What the operator tried | Diagnostic code | Why |
|---|---|---|
| `BACKUP DATABASE` (no `TO`) | `SBSQL.EMULATION.NON_FILE_OPERATION` | File-backed backup syntax has no filesystem effect in SBsql |
| `NBACKUP` | `SBSQL.EMULATION.NON_FILE_OPERATION` | Physical page-copy tool not admitted |
| `GBAK`, `GFIX`, `GSTAT`, `GSEC` | `SBSQL.EMULATION.REFERENCE_TOOL_NOT_EXECUTED` | Reference native tools are not invoked from parser authority |
| `COPY PROGRAM <cmd>` | Parser refusal | Cannot spawn host programs from parser authority |
| `COPY TO <file>` | Parser refusal | Compatibility filesystem writes not permitted |
| Statement exceeds size limit | `SBSQL.RESOURCE.STATEMENT_TOO_LARGE` | Statement too large |
| Too many parameters | `SBSQL.RESOURCE.PARAMETER_COUNT_EXCEEDED` | Reduce parameter count |
| AST too deep | `SBSQL.RESOURCE.AST_DEPTH_EXCEEDED` | Simplify query nesting |

**For file-backed Firebird-style operations**: Use ScratchBird's logical backup/restore surfaces (`BACKUP DATABASE TO <uri>` / `RESTORE DATABASE FROM <uri>`) instead of native tool syntax. See [Backup, Restore, And Data Movement](#ch-operations-administration-backup-restore-and-data-movement-md).

**For PostgreSQL COPY**: Use `COPY TO STDOUT` (admitted as a logical export stream) rather than `COPY TO <file>` or `COPY PROGRAM`.

**Escalation**: If a statement you believe should be admitted is being refused, collect the full message vector (not just the top-level error) and report it with the exact statement text.

---

## 5. Authentication or Authorization Failure

**Symptoms**: A connection fails at authentication. A statement fails with a permission or role denial. A TLS negotiation is refused.

**Likely area**: Principal identity, role assignment, policy binding, or TLS downgrade attempt.

**Evidence to collect**:
- The diagnostic code from the failure.
- Whether TLS is involved.
- Whether this is a new user or a regression for an existing user.

**Expected diagnostic codes**:
- `SBSQL.AUTH.REQUIRED` — authentication is required but was not provided.
- `SECURITY.AUTHENTICATION.TLS_DOWNGRADE_REFUSED` — client attempted to downgrade TLS.
- `SECURITY_DENIED` (engine ABI status) — security policy denied the operation.

**Safe checks**:
- Confirm the principal exists and has the expected role bindings.
- Confirm TLS configuration matches on client and server.
- Check whether this is a new policy deployment that may have inadvertently tightened access.

**Escalation**: See [Identity, Security, And Policy](#ch-operations-administration-identity-security-and-policy-md).

---

## 6. Object Not Found or Not Visible

**Symptoms**: A query fails with "not found" or "not visible". An object that the operator can see in one context does not appear in another.

**Likely area**: Schema visibility, catalog session scope, or MGA (multi-generational architecture) visibility rules.

**Evidence to collect**:
- The exact object name and schema reference.
- The role and principal under which the query is running.
- Whether the object is visible under a different role.

**Expected diagnostic codes**:
- `SBSQL.NAME_RESOLUTION.NOT_FOUND_OR_NOT_VISIBLE` — name resolution found no accessible object.

**Safe checks**:
- Verify the schema path is correct.
- Verify the querying principal has the right to see the object under its current role.
- If this is a recent DDL change, confirm the DDL transaction committed before the query.

**Escalation**: See Language Reference: Core Paradigms — UUID Catalog Identity (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX) and [Identity, Security, And Policy](#ch-operations-administration-identity-security-and-policy-md).

---

## 7. Transaction State Invalid or Recovery Required

**Symptoms**: Operations fail with "transaction state invalid", "recovery required", or similar. A database open fails with a recovery classification error.

**Likely area**: A database was not closed cleanly (crash, power loss), or a transaction is in an inconsistent state that requires the recovery path before normal operations can continue.

**Evidence to collect**:
- The diagnostic code from the failure.
- Whether the previous shutdown was clean or unclean.
- The `storage.startup_state.recovery_classification_invalid` or `storage.startup_state.durable_lifecycle_phase_invalid` codes from startup state reading.

**Expected diagnostic codes**:
- `recovery_required` (page/filespace agent state) — the storage layer requires recovery.
- `storage.startup_state.recovery_classification_invalid` — startup state has an invalid recovery classification.
- `storage.startup_state.durable_lifecycle_phase_invalid` — startup state has an invalid lifecycle phase.

**Safe checks**:
- Do not force-open a database that requires recovery.
- Check whether the process that last held the database shut down cleanly.
- Follow the recovery procedures in [Database Lifecycle](#ch-operations-administration-database-lifecycle-md).

**Escalation**: Collect the startup diagnostic output and the database's repair event ledger if available. Do not write new data until recovery is confirmed complete.

---

## 8. Storage Unavailable or Quarantined

**Symptoms**: A filespace operation fails. Writes are refused. A storage diagnostic mentions `shrink_blocked`, `refuse`, or a boundary violation.

**Likely area**: Filespace is in a `refused` or `recovery_required` state, or a page-filespace handoff encountered a boundary violation.

**Evidence to collect**:
- The handoff state from storage diagnostics.
- Whether the affected filespace is a primary or shadow.
- The specific boundary violation code if present.

**Expected states and codes**:
- `refused` — filespace request explicitly denied.
- `recovery_required` — filespace cannot accept new work until recovery completes.
- `invalid_filespace_identity` — boundary violation: presented identity is invalid.
- `invalid_page_family` — boundary violation: page family does not match agent's domain.

**Safe checks**:
- If a shadow filespace is the problem, check whether the primary is healthy.
- Do not attempt to manually repair filespace structures.
- Check storage device health (disk full, I/O errors) which can surface as filespace refusals.

**Escalation**: See [Filespaces And Storage](#ch-operations-administration-filespaces-and-storage-md).

---

## 9. Listener Is Live but Not Ready

**Symptoms**: The listener process is running and the management interface responds, but client connections are being refused or queued. The `accepting_new_connections` field is false.

**Likely area**: Drain is active, parser pool is not yet initialized, or parser pool has no available workers.

**Evidence to collect**:
- Run `listener.status` to get the current state snapshot.
- Check the `draining` field (true if drain is active).
- Check `queue_depth` and `open_connections`.
- Check parser pool status.

**Safe checks**:
- If `draining` is true, the listener was intentionally drained. Issue `listener.undrain` if the drain was not intended.
- If the pool is initializing, wait for pool startup to complete.
- If the pool has no workers due to saturation, investigate whether the parser worker count is sufficient for the load.

**Escalation**: See [Monitoring, Health, And Readiness](#ch-operations-administration-monitoring-health-and-readiness-md) and [Service Lifecycle](#ch-operations-administration-service-lifecycle-md).

---

## 10. Transaction Pressure or Slow Cleanup

**Symptoms**: Storage grows unexpectedly. Query performance degrades over time. Diagnostics mention cleanup horizon stall.

**Likely area**: A long-running transaction is holding the oldest transaction ID, preventing the MVCC garbage collector from advancing.

**Evidence to collect**:
- Current cleanup horizon evidence (key: `dpc030_authoritative_cleanup_horizon_v1`).
- Whether any long-running transactions are visible in session management.

**Safe checks**:
- Identify and close or roll back any abandoned long-running transactions.
- Verify that application code commits or rolls back transactions promptly.
- Check that `storage_version_cleanup_agent` and `index_garbage_cleanup_agent` are running.

**What not to do**: Do not kill the listener to clear transaction pressure — this can leave more uncommitted state to recover. Close the specific transactions causing the stall.

**Escalation**: See [Database Lifecycle](#ch-operations-administration-database-lifecycle-md) for cleanup agent configuration.

---

## 11. Support Bundle Is Insufficient or Too Broad

**Symptoms**: A support bundle you collected does not have enough information to diagnose the problem. Or: a bundle you are about to share contains information that should not leave your environment.

**If the bundle is insufficient**:
- Check whether the bundle was collected during the failure window or after the service had already recovered.
- Bundles contain a ring buffer of recent management decisions and runtime events (up to 64 events by default). If the failure was older than the ring buffer, the relevant events may have rolled off.
- Collect a new bundle while the failure condition is still active.
- If the listener is no longer running, collect the lifecycle journal and audit files directly.

**If the bundle is too broad**:
- Review `config-redacted.txt` and the metrics JSON for sensitive values.
- Check the bundle manifest for the `redaction_profile` value.
- Consult your security team before sharing a bundle collected during a security-related incident.

**Escalation**: See [Diagnostics, Message Vectors, And Support Bundles](#ch-operations-administration-diagnostics-message-vectors-and-support-bundles-md) for redaction details.

---

## Related Pages

- [Diagnostics, Message Vectors, And Support Bundles](#ch-operations-administration-diagnostics-message-vectors-and-support-bundles-md)
- [Monitoring, Health, And Readiness](#ch-operations-administration-monitoring-health-and-readiness-md)
- [Configuration Reference](#ch-operations-administration-configuration-reference-md)
- [Service Lifecycle](#ch-operations-administration-service-lifecycle-md)
- [Database Lifecycle](#ch-operations-administration-database-lifecycle-md)
- [Upgrade And Compatibility Policy](#ch-operations-administration-upgrade-and-compatibility-policy-md)




===== FILE SEPARATION =====

<!-- chapter source: Operations_Administration/upgrade_and_compatibility_policy.md -->

<a id="ch-operations-administration-upgrade-and-compatibility-policy-md"></a>

# Upgrade And Compatibility Policy

## Purpose

Before you move a ScratchBird deployment to a newer build, you need to know which on-disk structures might change, which downgrade paths are refused, and what evidence the new build requires before it will open an existing database. This chapter covers all of those topics so that an upgrade does not catch you by surprise.

The short version: ScratchBird refuses to open databases with a format version it does not understand. It also refuses to downgrade IPC protocol, configuration, or parser API versions. Upgrades that advance format versions may be irreversible from the older build's perspective. Plan accordingly.

---

## Format Version Surfaces

ScratchBird has several independently versioned on-disk and in-memory format surfaces. A change to any one of them may affect whether an older or newer build can open the relevant artifact.

| Surface | Version fields | Where checked |
|---|---|---|
| Database file format (page header) | `format_major` (currently `kScratchBirdDatabaseFormatMajor = 1`) | `src/storage/disk/database_format.hpp:26` |
| Startup state block | `format_major` / `format_minor` (current: `1.0`; supported range: `1.0`–`1.0`) | `src/storage/database/startup_state.hpp:33-38` |
| Catalog manifest format | `catalog_manifest_format_version` (currently `kDatabaseCatalogManifestFormatCurrent = 1`) | `src/storage/database/database_lifecycle.cpp:206` |
| Parser API major | `kCurrentParserApiMajor = 1`; compatible range `1`–`1` | `src/listener/control_plane.hpp:46-48` |
| SBLR surface | Tested by `tests/sblr_surface` and the `sblr_surface_guardrail_gate` | Build-time gate |

### How Version Refusals Work

When a database is opened, the engine reads the page header and the startup state block. If either format version is outside the supported range for this build:

- Format version **too new** (database written by a newer build): `FORMAT.VERSION_DOWNGRADE_REFUSED` or `SB-STARTUP-STATE-FORMAT-DOWNGRADE-REFUSED`. The database will not be opened.
- Format version **too old** (database written by an older build, and no migration plan is present): `storage.startup_state.format_too_old` or `storage.startup_state.migration_required_without_plan`. The database will not be opened without a migration plan.
- Format version **in the future** (major/minor combination not yet reached): `storage.startup_state.format_future`. The database will not be opened.

This means that once a database has been opened and written by a newer build, you cannot safely open it with an older build. The older build will refuse.

Sources: `src/storage/disk/database_format.cpp:175-195`, `src/storage/database/startup_state.cpp:343-390`.

---

## The Downgrade Refusal Policy

ScratchBird applies a broad downgrade-refusal policy across multiple surfaces:

| Surface | Diagnostic code | Trigger |
|---|---|---|
| Database page format | `FORMAT.VERSION_DOWNGRADE_REFUSED` | Database format major is newer than this build's maximum |
| Engine database lifecycle | `ENGINE.DBLC_FORMAT_DOWNGRADE_REFUSED` | Same condition, observed at the lifecycle layer |
| Startup state | `SB-STARTUP-STATE-FORMAT-DOWNGRADE-REFUSED` | Startup state format major/minor is newer than this build's maximum supported |
| IPC protocol | `IPC.LIFECYCLE.DOWNGRADE_REFUSED` | Client is using a newer IPC protocol version |
| Configuration | `CONFIG.DOWNGRADE_REFUSED` | Configuration value is not compatible with this build |
| Parser server IPC | `PARSER_SERVER_IPC.PROTOCOL_VERSION_DOWNGRADE_REFUSED` | Client is using a newer parser IPC protocol version |
| Memory policy | `MEMORY.POLICY_DOWNGRADE_REFUSED` | Memory policy cannot be downgraded |
| TLS session | `SECURITY.AUTHENTICATION.TLS_DOWNGRADE_REFUSED` | TLS version downgrade attempt |

The consistent principle: ScratchBird will not silently accept an incompatible artifact. Every incompatibility produces a specific diagnostic code so the operator knows exactly what is mismatched.

---

## Upgrade Path

### Pre-Upgrade Checklist

Before upgrading:

1. **Identify all databases and their current format versions.** If you cannot determine the format version, open the database with the current build and observe the catalog evidence.
2. **Verify the new build passes its release gate tests.** See [Release Validation Checklist](#ch-operations-administration-release-validation-checklist-md).
3. **Back up all databases** using `BACKUP DATABASE TO <uri>`. A logical backup taken with the current build is readable by the new build regardless of whether the format version advances.
4. **Confirm the backup is restorable** with a restore drill on a staging system.
5. **Check whether the new build introduces a format version change.** Review the release notes or the `kScratchBirdDatabaseFormatMajor` constant in the new build's source.

### Upgrade Sequence

1. Drain active listeners with `listener.drain` to allow in-flight sessions to complete.
2. Wait for all sessions to close (observe `open_connections` reaching zero in the status snapshot).
3. Stop the listener and manager.
4. Replace binaries.
5. Start the new manager and listener.
6. Confirm the new build opens the database successfully (verify with `STATUS` and a smoke query).
7. Undrain with `listener.undrain` once confirmed healthy.

### After Upgrade

Once the new build has opened and written to a database, the database's format version may have advanced. Verify that:
- The old build is no longer needed for this database.
- Any disaster-recovery procedures are updated to reference the new build version.

---

## Catalog Manifest Compatibility

The catalog manifest (`catalog_manifest_format_version`) records the version of the catalog structure inside the database. The current value is `1`. The manifest is read during database open to verify the catalog is interpretable. If the manifest version is unknown, the open is refused.

---

## Parser Package Compatibility

Each parser package carries a `parser_package_version`. The engine uses this to confirm that a parser package is compatible with the engine ABI and the database it is being asked to serve. Parser package version mismatches appear as diagnostics during parser registration; see [Parser Registration And Routes](#ch-operations-administration-parser-registration-and-routes-md).

The parser API major version (`kCurrentParserApiMajor = 1`) is negotiated during the listener hello handshake. If a parser worker presents a major version outside the supported range, the listener refuses the hello with `HelloAckPayload::accepted = false`.

---

## SBLR Surface Compatibility

The SBLR (ScratchBird Logical Representation) surface is the bytecode/envelope format that carries query plans from parser to engine. SBLR surface compatibility is tested by the `tests/sblr_surface` test tree and is guarded by the `sblr_surface_guardrail_gate`. A build that fails these gates should not be shipped.

From an operator's perspective: if a parser package was compiled against an older SBLR surface, it may produce envelopes the engine cannot execute. The engine will refuse execution with a diagnostic rather than attempting to interpret an unknown SBLR version.

---

## Configuration Compatibility

Configuration files are validated at startup. A configuration value that was valid in an older build may be refused if:
- The key was removed or renamed.
- The value type changed.
- A range constraint was tightened.

The `CONFIG.DOWNGRADE_REFUSED` code signals that a configuration value is not acceptable to the new build. Review the [Configuration Reference](#ch-operations-administration-configuration-reference-md) for the new build before upgrading.

---

## Migration Notes for Firebird Operators

Operators migrating from Firebird should note:

- `GBAK`, `GFIX`, `GSTAT`, `GSEC`, `FBSVCMGR`, and `FBTRACEMGR` are not executed by the SBsql parser. Use ScratchBird logical backup/restore and management routes instead.
- `NBACKUP` (physical page-level incremental backup) is not supported. Use `BACKUP DATABASE TO <uri>` with logical streams.
- Shadow file management syntax is recognized but has no filesystem side effect in SBsql; it routes through `diagnostic.lifecycle.message_vector` with code `SBSQL.EMULATION.NON_FILE_OPERATION`.

---

## Related Pages

- [Database Lifecycle](#ch-operations-administration-database-lifecycle-md)
- [Release Validation Checklist](#ch-operations-administration-release-validation-checklist-md)
- [Backup, Restore, And Data Movement](#ch-operations-administration-backup-restore-and-data-movement-md)
- [Troubleshooting](#ch-operations-administration-troubleshooting-md)
- Language Reference (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Operations_Administration/release_validation_checklist.md -->

<a id="ch-operations-administration-release-validation-checklist-md"></a>

# Release Validation Checklist

## Purpose

This checklist is for operators and developers who need to confirm that a ScratchBird build is trustworthy before using it with real data. It is not a support contract or a guarantee of production readiness — it is a structured evidence-gathering exercise that replaces "I think it works" with "I observed it working under these specific conditions."

**This checklist validates a build, not a deployment configuration.** Run it against the actual build you intend to use, on a representative platform, with access to the same test infrastructure that the build tree provides.

Work through each section in order. Mark each item as PASS, FAIL, or SKIP (with justification). A single unaddressed FAIL in sections 1-6 should block release use.

---

## Section 1: Build Output Completeness

Run before doing anything else. A build with missing artifacts fails in unpredictable ways.

- [ ] **1.1** The output directory contains the listener binary, manager binary, and parser worker binaries for all parsers included in this build.
- [ ] **1.2** All UDR parser support shared libraries are present in the expected output locations.
- [ ] **1.3** Parser package definition files (capability manifests) are present and readable.
- [ ] **1.4** The `tools/ceic_integrated_support_bundle.py` script is present and executable.
- [ ] **1.5** The license header gate has passed. Every compiled source file with the ScratchBird copyright block carries an MPL-2.0 SPDX identifier. Verify against the CMakeLists license header gate.

**Test tree reference**: `tests/release/driver_release_gate_foundation.py`, `tests/release/driver_release_artifact_manifest_gate.py` (via `tools/release/`).

---

## Section 2: License and Notices

- [ ] **2.1** The build carries the MPL-2.0 license text in the expected location.
- [ ] **2.2** No third-party dependency has been introduced since the last license review without a corresponding license notice.
- [ ] **2.3** The `SPDX-License-Identifier: MPL-2.0` comment is present in ScratchBird source files as verified by the CMake license header gate.

---

## Section 3: Configuration Validation

- [ ] **3.1** A minimal listener configuration file passes validation without errors.
- [ ] **3.2** A minimal manager configuration file passes validation without errors.
- [ ] **3.3** Configuration values that were valid in the previous build remain valid in this build (no `CONFIG.DOWNGRADE_REFUSED` on unchanged configurations).
- [ ] **3.4** A configuration with an intentionally invalid value produces a diagnostic code (`LISTENER.CONFIG.INVALID_VALUE` or equivalent), not a crash.

**Test tree reference**: Static analysis tests in `tests/release/` (`authority_drift_static.py`, `catalog_identity_boundary_static.py`).

---

## Section 4: IPC Smoke Test

The IPC smoke test confirms the listener and manager can establish the control-plane connection and exchange hello/hello-ack frames.

- [ ] **4.1** Start the listener. Confirm the IPC socket appears at the configured path.
- [ ] **4.2** Run `listener.status` (or equivalent) via the manager and receive a valid state snapshot with no error codes.
- [ ] **4.3** Confirm the `kCurrentParserApiMajor` version negotiates correctly: the hello-ack is `accepted = true`.
- [ ] **4.4** Send a `PING` to the listener control plane and receive a valid `HEALTH_CHECK` acknowledgment.
- [ ] **4.5** Confirm `SBSQL.SERVER.UNAVAILABLE` is not present in the response.

**Test tree reference**: `tests/manager/ipc_tester.cpp`, `tests/manager/protocol_unit_tests.cpp`, `tests/listener/control_plane_probe.cpp`.

---

## Section 5: Parser Registration and Route Smoke Test

- [ ] **5.1** At least one parser package registers successfully with the listener (no `UDR.BRIDGE.MISSING_CAPABILITY`, no `SBSQL.SERVER.UNAVAILABLE` during registration).
- [ ] **5.2** The parser package capability declaration includes the expected `supported_topologies` list (at minimum `logical_backup_restore`).
- [ ] **5.3** A simple `SELECT 1` or equivalent smoke statement routes to the engine and returns a result.
- [ ] **5.4** A statement that should be refused (`GBAK` or similar) produces the expected diagnostic code (`SBSQL.EMULATION.REFERENCE_TOOL_NOT_EXECUTED`) and does not crash the parser worker.
- [ ] **5.5** The `beta_package_smoke_gate.py` passes if a beta parser package is included.

**Test tree reference**: `tests/release/beta_package_smoke_gate.py`, `tests/release/attach_auth_conformance.cpp`, `tests/release/capability_profile_conformance.cpp`.

---

## Section 6: Database Create, Open, and Reopen

- [ ] **6.1** Create a new database. The create operation completes without error.
- [ ] **6.2** Open the newly created database. The open returns healthy status. No `FORMAT.VERSION_DOWNGRADE_REFUSED`, `SB-STARTUP-STATE-FORMAT-DOWNGRADE-REFUSED`, or startup state errors.
- [ ] **6.3** Close and reopen the database. The reopen succeeds. The catalog manifest format version is present in the open evidence.
- [ ] **6.4** Attempt to open the database with an intentionally older (incompatible) build or a simulated future format version. Confirm the refused diagnostic code is produced rather than a crash or silent corruption.
- [ ] **6.5** Confirm the `catalog_manifest_format_version` in the open evidence matches the expected current value (`1` as of this build).

**Test tree reference**: `tests/database_lifecycle/database_lifecycle_manager_conformance.py`, `tests/release/catalog_object_conformance.cpp`, `tests/release/public_upgrade_migration_gate.cpp`.

---

## Section 7: Transaction Commit and Rollback

- [ ] **7.1** Open a transaction, write a row, commit. The committed row is visible in a subsequent read.
- [ ] **7.2** Open a transaction, write a row, roll back. The row is not visible after rollback.
- [ ] **7.3** A savepoint can be set, rolled back to, and released.
- [ ] **7.4** Confirm the transaction cleanup horizon advances after committed transactions complete (check `dpc030_authoritative_cleanup_horizon_v1` evidence).
- [ ] **7.5** A long-running transaction that is not committed does not permanently block cleanup of unrelated transactions.

**Test tree reference**: `tests/release/cache_checkpoint_conformance.cpp`, `tests/mga_transaction_regression/`, `tests/release/public_transaction_mga_cow_gate.cpp`, `tests/release/public_transaction_savepoint_limbo_cleanup_gate.cpp`.

---

## Section 8: Backup and Restore Drill

A backup that has never been tested is not a backup.

- [ ] **8.1** Run `BACKUP DATABASE TO <uri>` on a database with at least one committed schema object and at least one committed data row. The backup completes without error.
- [ ] **8.2** Restore from that backup to a new (empty) database path using `RESTORE DATABASE FROM <uri>`.
- [ ] **8.3** Open the restored database. Confirm the schema object is present.
- [ ] **8.4** Confirm the data row is present.
- [ ] **8.5** Attempt `BACKUP DATABASE` (without `TO`). Confirm `SBSQL.EMULATION.NON_FILE_OPERATION` is returned and the operation does not crash or hang.
- [ ] **8.6** Attempt `NBACKUP`. Confirm the same refusal code.

**Test tree reference**: `tests/release/backup_archive_restore_conformance.cpp`, `tests/release/backup_restore_export_admin_gate_conformance.cpp`, `tests/release/public_backup_forward_session_gate.cpp`, `tests/release/public_backup_update_coverage_gate.cpp`.

---

## Section 9: Diagnostics and Support Bundle

- [ ] **9.1** Collect a listener support bundle while the listener is running. The bundle directory is created, the bundle JSON is valid, and no errors are reported.
- [ ] **9.2** Collect a manager support bundle. The `config-redacted.txt` file is present. Path values are replaced with `[path-redacted]` and secret refs with `<redacted-secret-ref-present>`. No raw socket paths or secret values appear.
- [ ] **9.3** The `RedactListenerSupportabilityText` and `RedactManagerSupportBundleText` functions are exercised: confirm that a synthetic sensitive value placed in a known field is replaced in the bundle output.
- [ ] **9.4** The `agent_metrics_audit_support_bundle_gate` passes: agent observability data is correctly included.
- [ ] **9.5** The `diagnostic_registry_gate` passes: the diagnostic code registry is consistent.

**Test tree reference**: `tests/release/agent_metrics_audit_support_bundle_gate.cpp`, `tests/release/agent_evidence_audit_redaction_retention_gate.cpp`, `tests/listener/diagnostic_registry_gate.cpp`.

---

## Section 10: Drain Admission and Graceful Shutdown

- [ ] **10.1** Issue `listener.drain`. Confirm the listener stops accepting new connections (`accepting_new_connections = false`).
- [ ] **10.2** Confirm existing sessions complete normally during drain.
- [ ] **10.3** Issue `listener.undrain`. Confirm the listener resumes accepting connections.
- [ ] **10.4** Issue `listener.stop` (graceful). Confirm the listener exits within `graceful_drain_timeout_ms` milliseconds.
- [ ] **10.5** The `drain_admission_smoke` test passes.

**Test tree reference**: `tests/listener/drain_admission_smoke.cpp`.

---

## Section 11: SBLR Surface and Parser Compatibility

- [ ] **11.1** The `sblr_surface_guardrail_gate` passes (if `SB_BUILD_SBLR_SURFACE_GUARDRAIL_GATES` is enabled for this build type). Zero unreconciled rows in the reference SBLR fixture closure.
- [ ] **11.2** The `final_sblr_sbsql_enterprise_proof_closure_gate` passes (enterprise builds).
- [ ] **11.3** The compatibility parser dialect isolation audit passes (`parser_dialect_isolation_audit_gate`).
- [ ] **11.4** Compatibility surface refusals for `COPY PROGRAM` and `COPY TO <file>` produce the correct parser diagnostics (not crashes, not silent drops).

**Test tree reference**: `tests/sblr_surface/`, `tests/sbsql_parser_worker/final_sblr_sbsql_enterprise_proof_closure_gate.py`, `tests/compatibility/`.

---

## Section 12: Known Limitations Review

Before marking a build as release-ready, the following must be reviewed:

- [ ] **12.1** The SBsql language beta proof gate (`SBsql language beta proof gate`) for this build has been reviewed and any deferral is documented.
- [ ] **12.2** All `SKIP` items in this checklist have documented justifications. No item is skipped simply because it is inconvenient to test.
- [ ] **12.3** Platform-specific test results have been reviewed for the target platform(s). Tests that are not expected to pass on a given platform are explicitly listed.
- [ ] **12.4** Any new `DOWNGRADE_REFUSED` codes introduced since the last release are documented in the release notes so operators can plan accordingly.

---

## Recording Evidence

For each checklist item that passes, record:
- Build identifier (git commit hash or release tag)
- Platform and OS version
- Date and operator name
- Test invocation command or procedure
- Observed outcome (pass/fail and any diagnostic output)

A checklist with no recorded evidence is not a completed checklist.

---

## Related Pages

- [Installation And Output Layout](#ch-operations-administration-installation-and-output-layout-md)
- [Operating Modes Runbook](#ch-operations-administration-operating-modes-runbook-md)
- [Diagnostics, Message Vectors, And Support Bundles](#ch-operations-administration-diagnostics-message-vectors-and-support-bundles-md)
- [Backup, Restore, And Data Movement](#ch-operations-administration-backup-restore-and-data-movement-md)
- [Upgrade And Compatibility Policy](#ch-operations-administration-upgrade-and-compatibility-policy-md)




# Security Guide




===== FILE SEPARATION =====

<!-- chapter source: Security_Guide/README.md -->

<a id="ch-security-guide-readme-md"></a>

# Security Guide

## Purpose

This guide is the deep-dive companion to the overview material in the
Operations and Administration manual and the Language Reference. It covers the
implementation-level detail that operators, integrators, and security reviewers
need when configuring authentication providers, understanding the security
model, hardening cryptographic policy, or deploying ScratchBird in environments
that require platform-specific configuration.

This is a **draft**. No claims herein constitute a production security
certification or a promise of external audit compliance.

## Security By Design, Not Bolted On

ScratchBird was designed from the start to be operated to a high-assurance,
government-grade security posture **if the operator chooses to implement it** —
rather than requiring such controls to be retrofitted onto an existing system.
The trust-separation architecture (see
[trust_and_separation_architecture.md](#ch-security-guide-trust-and-separation-architecture-md))
assumes outer layers are hostile by default: the engine does not trust the
client driver, the listener, the parser, or the manager, and it revalidates and
fail-closes instead of extending trust. The authentication, authorization,
policy, masking, protected-material, and audit mechanisms documented here exist
so an operator can raise the posture to what their environment requires without
re-architecting.

The practical consequence: the strong controls are **available from the start**
but are largely **opt-in**. A minimal deployment and a hardened one run the same
engine; they differ in how much of this security surface the operator
configures. This guide documents the mechanisms and their source-level
enforcement; it asserts **no** specific accreditation (FIPS, Common Criteria, or
equivalent), which must be validated independently against the target build.

## Structure

This guide is organized in three parts. Part 1 covers the security model,
authentication architecture, plugin families, platform configuration, and
cryptographic policy. Part 2 covers authorization objects — roles, groups,
rights, grants, domain and column security, and protected material. Part 3
covers the trust and separation architecture that underpins the whole model.
All three parts are required reading for a complete security understanding.

## Part 1 — Authentication and Cryptographic Policy

| Page | Contents |
|------|----------|
| [security_model_overview.md](#ch-security-guide-security-model-overview-md) | Layered model, principal kinds, fail-closed principle, epochs |
| [authentication_and_providers.md](#ch-security-guide-authentication-and-providers-md) | Provider/plugin architecture, plugin trust, challenge/credential/token flow |
| [auth_plugin_families.md](#ch-security-guide-auth-plugin-families-md) | Per-family reference for all 18 plugin families declared in source |
| [platform_configuration.md](#ch-security-guide-platform-configuration-md) | Linux, Windows, and BSD configuration differences (only verified) |
| [security_policies_and_crypto.md](#ch-security-guide-security-policies-and-crypto-md) | Policy-pack model, cryptographic policy, token and credential hardening |

## Part 2 — Authorization Objects (Roles, Grants, Domain Security)

| Page | Contents |
|------|----------|
| [standard_roles_and_groups.md](#ch-security-guide-standard-roles-and-groups-md) | Built-in roles and groups seeded by the default policy pack |
| [system_management_rights.md](#ch-security-guide-system-management-rights-md) | System management privilege surfaces and right names |
| [grants_and_privileges.md](#ch-security-guide-grants-and-privileges-md) | GRANT/REVOKE model, privilege inheritance, deny edges |
| [domain_and_column_security.md](#ch-security-guide-domain-and-column-security-md) | Domain-level security, column-level masking and grants |
| [protected_material.md](#ch-security-guide-protected-material-md) | Protected material lifecycle, key cache, legal hold, rotation |

## Part 3 — Trust and Separation Architecture

| Page | Contents |
|------|----------|
| [trust_and_separation_architecture.md](#ch-security-guide-trust-and-separation-architecture-md) | The layered trust model: why a compromised driver, listener, parser, or manager still cannot reach data; SBLR revalidation; module/build separation; fail-closed controls; compromise scenarios |

## Cross-References

The pages in this guide assume familiarity with the overview material in:

- [Operations and Administration: Identity, Security, and Policy](#ch-operations-administration-identity-security-and-policy-md)
- Language Reference: Security and Sandboxing (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX)

Those pages define the operator-visible concepts (principals, grants, schema
roots, protected material, refusal classes). The pages here explain the source
structures, provider registry, platform behavior, and policy-pack format that
underpin those concepts.

## Reading Model

For a first read, follow this order:

1. `security_model_overview.md` — understand the three-layer model and
   fail-closed invariant before reading anything else.
2. `authentication_and_providers.md` — understand how a provider is declared,
   trusted, and invoked.
3. `auth_plugin_families.md` — select and review the plugin families relevant
   to your deployment.
4. `platform_configuration.md` — apply the platform-specific notes for your
   operating system.
5. `security_policies_and_crypto.md` — review and harden your cryptographic
   policy before deploying.
6. Part 2 pages — understand authorization objects once the authentication
   layer is configured.
7. `trust_and_separation_architecture.md` — understand why the whole model
   holds even when an outer layer is compromised. Readers focused on threat
   modelling or evaluating deployment isolation may read this first.




===== FILE SEPARATION =====

<!-- chapter source: Security_Guide/security_model_overview.md -->

<a id="ch-security-guide-security-model-overview-md"></a>

# Security Model Overview

## Purpose

This page describes the layered security model that ScratchBird applies to
every operation. Understanding the model is a prerequisite for configuring
authentication providers, writing grant policies, or diagnosing access
refusals. The invariants documented here are implemented in
`src/engine/internal_api/security/security_model.hpp` and enforced throughout
the engine's API surface.

## Definitions

**Principal** — any entity that can hold identity within the engine. Principals
include users, services, and system actors. Each principal has a durable UUID
identity recorded in the catalog. Names are resolver inputs only; the UUID is
the authority.

**Provider** — a plugin that supplies authentication evidence. Providers do not
grant rights. They supply evidence that the engine evaluates to produce a
`ConnectionSecurityContextRecord`.

**Claim** — a piece of evidence supplied by a provider (for example: a subject
identity, a set of group names, or a credential kind). Claims are normalized by
the engine; they are not treated as authority on their own.

**Materialization** — the process of evaluating the catalog's durable grant,
role, and group records against a principal UUID to produce an
`EngineMaterializedAuthorizationContext`. Authorization is materialized from
catalog policy, not inferred from claims.

**Fail-closed** — the invariant that when any required evidence is missing,
stale, ambiguous, or contradicted by an explicit denial, the engine refuses the
operation rather than guessing or defaulting to allow. This is a
source-documented invariant. The default security posture policy profile
(`deny_by_default`) reflects it.

**Epoch** — a monotonically increasing counter that tracks the generation of
security state. Three epochs are maintained: `security_epoch` (principal,
role, and group mutations), `policy_epoch` (policy and grant mutations), and
`catalog_generation_id` (catalog generation). Caches must be invalidated when
their observed epoch diverges from the current epoch.

**Security context** — the `ConnectionSecurityContextRecord` that is created at
authentication time and accompanies every request. It carries:
`effective_user_uuid`, `authority_uuid`, `policy_epoch`, `security_epoch`,
`active_roles`, `effective_groups`, `authorization_trace_tags`,
`external_provider_evidence`, `cache_expiry`, `transaction_start_policy`,
`disclosure_policy`, and `audit_policy_ref`.

## The Three-Layer Model

Source: `authentication_api.hpp`, `authorization_api.hpp`, `deep_enforcement_api.hpp`.

```
Layer 1: Authentication
  Client presents credential evidence
  Engine calls EngineAuthenticate → produces ConnectionSecurityContextRecord
  A failed authentication returns SECURITY.AUTHENTICATION.FAILED (or a more
  specific code). No further processing occurs.

Layer 2: Authorization
  Engine calls MaterializeDurableAuthorizationContext for the principal UUID
  Engine calls EvaluateMaterializedAuthorization for the requested right
  and target UUID
  Default decision is "deny" (fail-closed)
  Explicit denial wins over allow

Layer 3: Deep Enforcement
  Engine calls EngineEvaluateDeepSecurity for each executor/storage operation
  Answers: admitted, authorized, visible, masked, rls_applied, audit_written,
  side_effect_permitted
  This API is not a parser hook; it executes inside the engine after the
  parser has translated the statement.
```

Parser routes do not grant authority. A parser accepted on a given route still
passes through the full engine authorization chain before any data is read or
written.

## Principal Kinds

Source: `security_principal_lifecycle.hpp` — `EngineSecurityPrincipalRecord`.

| Kind | Lifecycle states | Notes |
|------|-----------------|-------|
| `user` | `active`, `disabled` | Interactive or application accounts identified by name |
| `service` | `active`, `disabled` | Non-interactive workloads, background jobs, API integrations |
| `system_actor` | Internal only | Engine subsystems; cannot be created by operators |

The `principal_kind` field defaults to `"user"`. A disabled principal fails
authentication regardless of credential validity (diagnostic:
`SECURITY.PRINCIPAL_DISABLED`). The engine also normalizes `"enabled"` as an
alias for `"active"` and `"disable"` as an alias for `"disabled"` during
mutations.

## Roles and Groups

Source: `security_principal_lifecycle.hpp` — `EngineSecurityRoleRecord`,
`EngineSecurityGroupRecord`.

**Roles** are owned by a principal (`owner_principal_uuid`) and may be granted
to other principals. A role's active privilege set is computed by materializing
the grants attached to its UUID.

**Groups** carry an `external_authority_ref` field. This field links the group
to an identity provider's group claim, enabling provider-sourced group
membership to flow into the engine's authorization model. An empty
`external_authority_ref` means the group is locally managed.

## Authorization Records

Source: `security_model.hpp` — `DurableAuthorizationState`.

The durable authorization state consists of:

| Record | Purpose |
|--------|---------|
| `DurableAuthorizationPrincipalRecord` | A principal's active status and security epoch |
| `DurableAuthorizationRoleRecord` | A role's active status and security epoch |
| `DurableAuthorizationGroupRecord` | A group's active status and security epoch |
| `DurableAuthorizationMembershipRecord` | An edge from a member (any kind) to a parent (role or group) |
| `DurableAuthorizationGrantRecord` | A right granted or denied to a subject for a target; carries `deny` flag |
| `DurableAuthorizationPolicyRecord` | A policy object with `requires_runtime_recheck` flag |

Materialization is requested via `DurableAuthorizationMaterializeRequest`,
which carries the principal UUID and the caller's observed epoch values. The
result is `DurableAuthorizationMaterializeResult`, which carries the
`EngineMaterializedAuthorizationContext`. If any epoch has advanced, the result
reflects the current catalog state.

## Authorization Decision

Source: `security_model.hpp` — `MaterializedAuthorizationDecision`.

```
authorized  — the principal holds the required right for the target
denied      — an explicit deny record was present
policy_recheck_required — a policy with requires_runtime_recheck was matched
decision    — "deny" (default), "allow", or "deny_explicit"
```

The default decision is `"deny"`. A decision of `"allow"` requires at least one
grant record to match and no deny record to override it.

## Security and Policy Epochs

Source: `security_principal_lifecycle.hpp`.

Each mutation to principals, roles, groups, memberships, grants, or policies
increments `security_generation` or `policy_generation` and returns a
`cache_invalidation_epoch`. Callers that cache authorization results must
validate their cached epoch via `EngineSecurityValidatePolicyCache` before
using cached data. A stale cache is refused (`SECURITY.POLICY.CACHE_STALE`).

## Offline and Stale Provider Behavior

Source: `security_model.hpp` — `SecurityAuthorityDescriptor`.

The `offline_behavior` field on a `SecurityAuthorityDescriptor` defaults to
`"deny_new_connections"`. This means that when the configured provider is
unreachable, new connection attempts are denied rather than bypassed. This is
a fail-closed behavior at the authority level.

The `AuthProviderPolicy` struct carries `stale_behavior = "deny"` and
`cache_bounds = "deny_when_expired"` as defaults. A provider whose evidence
cache has expired will deny authentication rather than serving stale claims.

## Diagnostic Codes

Source: `security_principal_lifecycle.hpp` — inline constants.

Key diagnostic codes emitted by the security principal lifecycle:

| Code | Meaning |
|------|---------|
| `SECURITY.PRINCIPAL.DATABASE_PATH_REQUIRED` | Operation requires a database path |
| `SECURITY.PRINCIPAL.MGA_TRANSACTION_REQUIRED` | Principal mutation requires an MGA transaction |
| `SECURITY.PRINCIPAL.AUTHORITY_REQUIRED` | No authority context was present |
| `SECURITY.PRINCIPAL.AUTHORITY_BYPASS_REFUSED` | An attempt to bypass authority was refused |
| `SECURITY.PRINCIPAL_INVALID` | The principal UUID is unknown or invalid |
| `SECURITY.PRINCIPAL_DISABLED` | The principal exists but is disabled |
| `SECURITY.PRINCIPAL.DUPLICATE` | A principal with this identity already exists |
| `SECURITY.ROLE_INVALID` | The role UUID is unknown or invalid |
| `SECURITY.GROUP_INVALID` | The group UUID is unknown or invalid |
| `SECURITY.GRANT_INVALID` | The grant is malformed or refers to unknown objects |
| `SECURITY.ACCESS_DENIED` | Access was denied by the authorization model |
| `SECURITY.PRIVILEGE.DEFAULT_DENY` | No matching grant was found; default-deny applied |
| `SECURITY.PRIVILEGE.GRANT_NOT_VISIBLE` | A grant was referenced but is not visible to this context |
| `SECURITY.POLICY_MISSING` | A required policy object is absent |
| `SECURITY.POLICY.STALE` | The policy epoch has advanced; recheck required |
| `SECURITY.POLICY.CACHE_STALE` | Cached authorization data has become stale |
| `SECURITY.AUDIT.EVIDENCE_REQUIRED` | An audit-before-success obligation was not met |
| `SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED` | A call path attempted to return plaintext protected material |

## Where Each Layer Lives in Source

| Concern | Source file |
|---------|------------|
| Security model types and epoch machinery | `security_model.hpp`, `security_model.cpp` |
| Authentication entry point | `authentication_api.hpp`, `authentication_api.cpp` |
| Authorization materialization | `authorization_api.hpp`, `authorization_api.cpp` |
| Deep enforcement (executor/storage) | `deep_enforcement_api.hpp`, `deep_enforcement_api.cpp` |
| Principal, role, group lifecycle | `security_principal_lifecycle.hpp`, `security_principal_lifecycle.cpp` |
| Provider model and registry | `auth_provider_model.hpp`, `auth_provider_model.cpp` |
| Live evidence validation per family | `auth_provider_live_adapter.hpp`, `auth_provider_live_adapter.cpp` |
| Cryptographic primitives | `security_crypto_policy.hpp`, `security_crypto_policy.cpp` |
| Plugin trust and manager admission | `plugin_trust_api.hpp`, `plugin_trust_api.cpp` |
| Identity create/alter | `identity_api.hpp`, `identity_api.cpp` |

## Related Pages

- [authentication_and_providers.md](#ch-security-guide-authentication-and-providers-md) — how a provider is declared, trusted, and invoked
- [auth_plugin_families.md](#ch-security-guide-auth-plugin-families-md) — per-family reference for all 18 plugin families
- [security_policies_and_crypto.md](#ch-security-guide-security-policies-and-crypto-md) — policy-pack model and cryptographic policy
- [Operations and Administration: Identity, Security, and Policy](#ch-operations-administration-identity-security-and-policy-md)
- Language Reference: Security and Sandboxing (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Security_Guide/authentication_and_providers.md -->

<a id="ch-security-guide-authentication-and-providers-md"></a>

# Authentication and Providers

## Purpose

This page describes the provider/plugin architecture that ScratchBird uses to
authenticate connections. It covers how a provider is declared, admitted to the
engine via plugin trust, and invoked. It also documents the challenge, credential,
and token lifecycle that provider plugins participate in. Every detail here is
grounded in the source at
`src/engine/internal_api/security/`.

## Overview

ScratchBird separates the authentication decision from the credential
verification work. Providers supply evidence. The engine owns the decision.

A provider plugin is registered with a family name, a set of declared
capabilities, and a trust state. The engine evaluates the provider's trust state
before allowing it to participate in authentication. At authentication time, the
engine calls into the provider (via the live adapter or a fixture path for
testing) and normalizes the result into an `AuthProviderLiveEvidenceResult`. The
engine then evaluates that result to produce a
`ConnectionSecurityContextRecord`, which becomes the security context for the
connection.

## Provider Descriptor

Source: `auth_provider_model.hpp` — `AuthProviderDescriptor`.

```
AuthProviderDescriptor
  provider_uuid          — UUIDv7 identity of this provider instance
  provider_family        — canonical family name (see auth_plugin_families.md)
  provider_version       — declared version of the provider
  implementation_version — implementation version (may differ from declared)
  capabilities           — SecurityProviderCapabilities (see below)
  policy_epoch           — epoch at which this descriptor was last evaluated (default: 1)
  trust_state            — "untrusted" (default), "admitted", or "revoked"
  rollout_state          — "disabled" (default), "enabled", or "deprecated"
  required_libraries     — runtime library dependencies
  allowed_policy_scopes  — policy scopes this provider may participate in
```

The `trust_state` starts as `"untrusted"`. A provider that has not been admitted
cannot be used for authentication. Admission is performed by
`EngineRegisterAuthProvider` after the trust evaluation in `plugin_trust_api`.

## Provider Capabilities

Source: `security_model.hpp` — `SecurityProviderCapabilities`.

Each provider declares a capability set. These are evaluated per-family by
`SecurityProviderCapabilitiesFor`.

| Capability | Meaning |
|------------|---------|
| `supports_authn` | Provider can authenticate connections |
| `supports_authz_claims` | Provider can supply authorization group claims |
| `supports_group_query` | Provider supports active group queries |
| `supports_transitive_group_expansion` | Provider can expand transitive group membership |
| `supports_membership_path_explain` | Provider can explain the path to group membership |
| `supports_mfa` | Provider participates in multi-factor authentication |
| `supports_token_introspection` | Provider can introspect tokens (e.g., OIDC introspection) |
| `supports_credential_rotation` | Provider supports in-band credential rotation |

## Plugin Trust API

Source: `plugin_trust_api.hpp`.

The plugin trust API has two entry points:

**`EngineEvaluateUdrTrust`** — evaluates whether a UDR (user-defined routine or
extension) plugin meets the trust requirements. Result: `admitted = true|false`.

**`EngineEvaluateManagerAdmission`** — evaluates whether a manager-level plugin
meets the admission requirements. Result: `admitted = true|false`.

The manifest_policy probe (`sb_auth_plugin_manifest_policy_probe`) tests four
trust rejection cases:

1. Provider with invalid signature is rejected (not admitted)
2. Provider with a missing required dependency is rejected
3. Provider whose ABI version is not supported is rejected
4. Provider with a stale implementation version is rejected

These checks occur at registration time via `EngineRegisterAuthProvider`, not at
authentication time. A provider that fails trust evaluation will not be admitted
and cannot be used.

## Provider Registration and Lifecycle

Source: `auth_provider_plugin_api.hpp`.

The provider plugin API exposes four operations:

| API | Purpose |
|-----|---------|
| `EngineRegisterAuthProvider` | Registers and admits a provider; performs trust evaluation |
| `EngineInspectAuthProvider` | Returns the provider's current descriptor and visibility status |
| `EngineDisableAuthProvider` | Disables a previously admitted provider |
| `EngineAuthenticateProvider` | Invokes the provider to authenticate a connection |

The registry probe (`sb_auth_plugin_registry_probe`) verifies:
- A valid provider can be registered and returns `admitted = true`
- Duplicate provider UUID is rejected
- An unknown provider family is rejected
- A provider with `provider:disabled` is rejected

## Provider Policy

Source: `auth_provider_model.hpp` — `AuthProviderPolicy`.

Each admitted provider is governed by a policy record:

```
AuthProviderPolicy
  enabled                — whether this provider is active (default: false)
  allow_password_compat  — whether cleartext-compatible fallback is allowed (default: false)
  require_mfa            — whether MFA is required (default: false)
  require_group_sync     — whether group synchronization is required (default: false)
  allow_cache_stale      — whether stale cache evidence is accepted (default: false)
  allow_fixture          — whether fixture-backed paths are allowed (default: true)
  stale_behavior         — what to do with stale evidence: "deny" (default)
  group_behavior         — how to handle group evidence: "none" (default)
  cache_bounds           — cache expiry behavior: "deny_when_expired" (default)
  audit_policy_ref       — reference to the audit policy for this provider
  redaction_policy_ref   — reference to the redaction policy for this provider
```

Policy can be reloaded without restarting via `EngineReloadAuthProviderPolicy`.
The default values enforce fail-closed behavior: `stale_behavior = "deny"` and
`cache_bounds = "deny_when_expired"` mean that a provider whose external service
is temporarily unavailable will deny new authentication attempts rather than
serving cached credentials.

## Authentication Request Flow

Source: `authentication_api.hpp` — `EngineAuthenticateRequest`,
`EngineAuthenticateResult`.

The `EngineAuthenticate` API accepts:

```
EngineAuthenticateRequest
  provider_family          — the canonical provider family to use
  principal_claim          — the claimed principal identity
  credential_evidence      — the serialized credential payload
  credential_evidence_present — whether credential_evidence is supplied
  credential_invalid_claim — set by caller if credential is known invalid (used in testing)
  mfa_evidence_present     — whether MFA evidence is included
```

The result carries a `ConnectionSecurityContextRecord` (see
[security_model_overview.md](#ch-security-guide-security-model-overview-md)) and an
`authenticated` boolean. A failed authentication returns `authenticated = false`
and a diagnostic code.

A mid-session `EngineRefreshSecurityContext` call re-evaluates the security
context without re-authenticating. This is used when a principal's grants or
group memberships change during an active session.

## Live Evidence Validation

Source: `auth_provider_live_adapter.hpp`, `auth_provider_live_adapter.cpp`.

The live adapter normalizes provider-specific payloads into a standard
`AuthProviderLiveEvidenceResult`:

```
AuthProviderLiveEvidenceResult
  evaluated              — was live evidence evaluation attempted?
  ok                     — did validation succeed?
  authenticated          — is the principal authenticated?
  groups_materialized    — did the provider return group evidence?
  membership_explainable — can group membership be explained (path available)?
  mfa_verified           — was MFA evidence present and verified?
  provider_family        — canonical family name
  principal              — the resolved principal identity
  credential_kind        — what kind of credential was used
```

The live adapter is activated when the request includes `adapter_mode:live`,
`provider_driver:live`, or a non-empty `provider_payload:` option. Without
these options the engine uses its fixture path (used in testing).

The adapter enforces a set of cross-family adversarial payload rejections before
dispatching to family-specific validation:
- Replay detected (`replay=true` or `replayed=true` in payload) → `provider_replay_denied`
- Expired assertion (`expired=true`) → `provider_assertion_expired`
- Algorithm downgrade (`alg` in `{none, md5, sha1, rs1}`) → `provider_algorithm_downgrade_denied`
- Key ID mismatch (`kid=mismatch`) → `provider_key_id_mismatch`
- Invalid signature (`signature=bad` or `sig=bad`) → `provider_signature_invalid`
- Revoked token (`revoked=true` or `token_revoked=true`) → `SECURITY.TOKEN_REVOKED`

These checks are enforced before family-specific validation runs, meaning they
apply to all provider families uniformly.

## External Client Dependency Gate

Source: `auth_provider_live_adapter.cpp` — `ValidateExternalClientGate`.

When a request is marked as using a real external client (`client_mode:external`,
`provider_client:real`, or `real_external_client:true`), the live adapter checks
whether the required runtime dependency for the provider family is available. If
the dependency is absent, the authentication is denied with
`real_client_dependency_missing:<dependency>`. If the external service is
unavailable, the denial is `real_client_service_unavailable`.

This gate ensures that a provider family that requires an external service
(e.g., `ldap_ad` requires `ldap_client`) cannot succeed if the service is not
reachable, rather than falling back to a less secure path.

## Challenge Flow

Source: `auth_challenge_api.hpp` — `EngineContinueAuthChallenge`.

Challenge-based authentication (for example, WebAuthn) uses a multi-step flow:

1. Initial authentication request issues a challenge.
2. The client returns a response via `EngineContinueAuthChallenge`.
3. The engine validates the challenge state and produces
   `challenge_accepted = true` on success.

The challenge_state probe (`sb_auth_plugin_challenge_state_probe`) verifies
four rejection cases:
- Expired challenge (`challenge_expired:true`) → rejected
- Replayed challenge (`challenge_replayed:true`) → rejected
- Attempt limit exceeded (`attempt_limit_exceeded:true`) → rejected
- A valid challenge → accepted (`challenge_accepted = true`)

## Credential Rotation

Source: `auth_credential_api.hpp` — `EngineRotateCredential`.

The engine supports in-band credential rotation via `EngineRotateCredential`.
This allows a provider to update a principal's stored credential (for example,
to rotate a SCRAM verifier) without requiring an external administrative action.

The secret_provider probe (`sb_auth_plugin_secret_provider_probe`, which tests
SCRAM rotation) verifies three properties:
- A valid rotation request succeeds (`rotated = true`)
- A rotation request that attempts to store reusable plaintext (`store_reusable_plaintext:true`) is rejected
- A rotation request with missing credential material is rejected

This confirms that the engine refuses to store credentials in a form that would
allow password reuse or plaintext extraction.

## Token Revocation

Source: `auth_token_api.hpp` — `EngineRevokeToken`.

Tokens (API keys, bearer tokens, OIDC tokens) can be explicitly revoked via
`EngineRevokeToken`. After revocation, subsequent authentication attempts with
the same token return `SECURITY.TOKEN_REVOKED`.

The token_authkey probe verifies:
- A valid token authentication succeeds
- A revoke operation sets `revoked = true`
- A subsequent authentication attempt with the revoked token (via the `unsynced_rejected` case) fails

## Provider Observability

Source: `auth_provider_observability_api.hpp` — `EngineInspectAuthProviderMetrics`.

Provider metrics can be inspected via `EngineInspectAuthProviderMetrics`. The
result carries `metrics_available = true|false`. Metrics are accessible only
to principals with the `OBS_METRICS_READ_FAMILY` right (as referenced in the
sblr_api probe).

## Related Pages

- [security_model_overview.md](#ch-security-guide-security-model-overview-md) — principal kinds, epochs, decision model
- [auth_plugin_families.md](#ch-security-guide-auth-plugin-families-md) — per-family requirements and status
- [security_policies_and_crypto.md](#ch-security-guide-security-policies-and-crypto-md) — policy-pack model and crypto hardening
- [platform_configuration.md](#ch-security-guide-platform-configuration-md) — OS-specific configuration for each family




===== FILE SEPARATION =====

<!-- chapter source: Security_Guide/auth_plugin_families.md -->

<a id="ch-security-guide-auth-plugin-families-md"></a>

# Authentication Plugin Families

## Purpose

This page is the per-family reference for all authentication plugin families
declared in the ScratchBird engine. It covers what each family is, what it
requires, whether it is live (fully wired) or carries a guard that prevents
normal use, and its security-critical validation requirements.

Sources: `src/engine/internal_api/security/auth_provider_model.cpp`
(the `kAuthFamilyRegistry` array), `src/engine/internal_api/security/auth_provider_live_adapter.cpp`
(family-specific `Validate*` functions), and the 18 `sb_auth_plugin_*_probe`
probe tools.

## Terminology

**Live (fully wired)** — the family has a `ValidateXxx` function in the live
adapter and the registry marks `live_evidence_required = true`. A real
deployment can use this family if the required dependency is present.

**Fixture/test path only** — the family is declared in the registry and has a
probe, but the live adapter does not have a corresponding `ValidateXxx`
function. Authentication is possible only via the internal fixture path (used
in unit and integration testing).

**Guarded** — the registry entry marks `guarded = true`. This means the family
has a fail-closed guard that prevents use until a specific engine-internal
condition is met (e.g., verifier storage is implemented and tested). Guarded
families return a documented error code rather than allowing authentication.

**Policy-gated** — the registry marks `policy_gated = true`. The family
requires an explicit policy option to be present before it can be used. Without
the policy option, authentication is denied.

**Validator-only** — the family is not a login mechanism. It supplies evidence
for an associated login family but cannot be used directly to authenticate a
connection.

**Forbidden** — the family is in the registry but is refused unconditionally.

## Summary Comparison Table

| Family | Probe | Live adapter | Guards / gates | Required dependency | Group evidence | Membership path explain | MFA |
|--------|-------|-------------|----------------|---------------------|---------------|------------------------|-----|
| `local_password` | scram_password | Yes (via scram_sha256 path) | None | None | No | No | No |
| `scram_sha256` | scram_password | No (fixture) | Channel binding required | None | No | No | No |
| `scram_sha512` | scram_password | No | **Guarded** | None | No | No | No |
| `peer` / `ident` | peer_ident | No (fixture) | None | None | No | No | No |
| `pam` | pam | Yes | Policy-gated | `pam` | No | No | No |
| `certificate_mtls` | certificate_mtls | Yes | Dependency required | `tls_x509` | Yes (required) | No | No |
| `ldap_ad` | ldap_ad | Yes | Dependency required | `ldap_client` | Yes (required) | Yes | No |
| `kerberos_pac` | kerberos_pac | Yes | Dependency required | `gssapi_krb5` | Yes (PAC groups) | No | No |
| `oidc_jwt` | oidc_jwt | Yes | Dependency required | `oidc_jwt_client` | Yes (required) | No | No |
| `saml` | saml | Yes | Dependency required | `saml_xmlsig` | Yes (attributes) | No | No |
| `webauthn` | webauthn_mfa | Yes | Policy-gated, MFA required | `webauthn_fido2` | No | No | Yes |
| `radius` | radius | Yes | Policy-gated, dependency required | `radius_client` | Yes (from attributes) | No | No |
| `proxy_assertion` | proxy_assertion | Yes | Policy-gated, channel binding | `proxy_assertion_verifier` | Optional | No | No |
| `token_api_key` | token_authkey | Yes | None | None | Yes (required) | No | No |
| `bearer_token` | (live adapter only) | Yes | None | None | No | No | No |
| `workload_identity` | workload_identity | Yes | Dependency required | `spiffe_svid_or_workload_oidc` | Yes (service_group) | No | No |
| `managed_identity` | (live adapter only) | Yes | Dependency required | `spiffe_svid_or_workload_oidc` | Yes (service_group) | No | No |
| `factor_chain` | webauthn_mfa | Yes | Policy-gated, MFA required | `webauthn_fido2` | No | No | Yes |
| `challenge_state` | challenge_state | Challenge continuation | Replay/expiry protection | (uses other family) | — | — | — |
| `manifest_policy` | manifest_policy | Registration trust | Signature/ABI/dependency checks | — | — | — | — |
| `sblr_api` | sblr_api | SBLR dispatch path | Admin rights required | — | — | — | — |
| `registry` | registry | Registration lifecycle | Duplicate/unknown/disabled rejection | — | — | — | — |
| `secret_provider` | secret_provider | Credential rotation | Plaintext storage refused | — | — | — | — |

The rows marked `challenge_state`, `manifest_policy`, `sblr_api`, `registry`,
and `secret_provider` correspond to the 18 probe tools listed in the task
specification. These probes test infrastructure cross-cutting concerns (trust
evaluation, challenge continuation, SBLR dispatch, registration lifecycle,
credential rotation) rather than discrete login-family authentication.

The 18 probe directories are:
`certificate_mtls`, `challenge_state`, `kerberos_pac`, `ldap_ad`,
`manifest_policy`, `oidc_jwt`, `pam`, `peer_ident`, `proxy_assertion`,
`radius`, `registry`, `saml`, `sblr_api`, `scram_password`, `secret_provider`,
`token_authkey`, `webauthn_mfa`, `workload_identity`.

---

## Per-Family Reference

### local_password

**What it is:** Built-in password authentication using the engine's local
credential verifier. This is the only provider enabled by default in the
`default-local-password` policy pack.

**Registry:** `supports_authn = true`, no guards, no policy gate, no dependency.

**How it works:** Credentials are stored as SCRAM-verifier artifacts in the
protected material catalog. The engine verifies the credential locally without
an external service call.

**Default policy state:** `enabled_by_default = true`, authority:
`durable_catalog_row`, `credential_verifier_policy:
local_password_verifier_required`.

**Security notes:**
- Plain-text credential storage is refused. The credential rotation probe
  confirms that `store_reusable_plaintext:true` is rejected.
- A principal with no stored credential cannot authenticate (the probe confirms
  `missing_material_rejected`).
- Password-compatible cleartext fallback (`password_compat` family) is
  policy-gated (`allow_password_compat` must be set) and is refused by default.

---

### scram_sha256

**What it is:** SCRAM-SHA-256 channel-binding authentication. A sub-variant of
the local password family.

**Registry:** `supports_authn = true`, channel binding required, no dependency.

**Live adapter:** No separate `ValidateScram` function in the live adapter.
Authentication is handled via the fixture/internal path. The scram_password
probe exercises this path.

**What the probe confirms:**
- `scram_ok` — SCRAM authentication succeeds on the fixture path
- `compat_default_rejected` — password-compat fallback is rejected by default
- `compat_allowed` — password-compat is allowed when `allow_password_compat:true` is set
- `downgrade_rejected` — a downgrade attempt to `scram_sha512` with `downgrade_attempt:true` is rejected

**Security notes:**
- The engine refuses algorithm downgrade attempts on this family.
- Channel binding is required by the registry entry.

---

### scram_sha512

**What it is:** SCRAM-SHA-512. Declared in the registry but **guarded** pending
verifier storage implementation and test coverage.

**Registry:** `guarded = true`, fail-closed code: `SECURITY.AUTH_METHOD_UNSUPPORTED`,
detail: `scram_sha512_guarded_until_verifier_storage_and_tests`.

**Security notes:** Do not attempt to configure this family in production. It
will return `SECURITY.AUTH_METHOD_UNSUPPORTED` regardless of credential state.
This guard is intentional and is a source-documented invariant.

---

### peer / ident

**What it is:** Operating-system process credential authentication. `peer`
verifies the client's OS UID via a socket credential exchange (POSIX
`SO_PEERCRED` or equivalent). `ident` queries an RFC 1413 ident server on the
network connection.

**Registry:** Both map to the `peer` base family in `BaseFamily()`. `ident` is
normalized to `peer` by `CanonicalAuthProviderFamily`. `supports_authn = true`,
no guards, no dependency declared in the registry.

**Live adapter:** No `ValidatePeer` or `ValidateIdent` function in the live
adapter. Uses fixture path.

**The peer_ident probe confirms:**
- `peer_ok` — peer authentication succeeds with `provider:peer` and `principal:local_user`
- `ident_ok` — ident authentication succeeds on the fixture path
- `spoof_rejected` — a stale ident response (`freshness:stale`) is rejected

**Platform notes:** `peer` requires a POSIX socket with `SO_PEERCRED` or
equivalent. See [platform_configuration.md](#ch-security-guide-platform-configuration-md).
`ident` relies on an RFC 1413 server on the network and is generally not
suitable for production deployments.

---

### pam

**What it is:** Pluggable Authentication Modules. Delegates authentication to
the OS PAM stack.

**Registry:** `supports_authn = true`, `policy_gated = true` (policy option:
`pam_policy_enabled`), dependency: `pam`, `live_evidence_required = true`.

**Live adapter:** `ValidatePam` function present. Required payload fields:
`user`, `service`, `module`, `password`, `prompt`, `account`, `session`.

**Validation rules:**
- `prompt` must be `"hidden"` or `"secret"` — insecure prompt types are refused
  with `pam_insecure_prompt`
- `account` must be `"allow"` — PAM account phase must succeed
- `session` must be `"open"` — PAM session-open phase must succeed

**The pam probe confirms:**
- `pam_auth` — authentication succeeds on the fixture path
- `pam_failure_rejected` — a fixture with `fixture_fail:true` is rejected

**Platform notes:** PAM is POSIX-only. It is not available on Windows without a
compatibility layer. On Linux and BSD, the PAM module name corresponds to a
file in `/etc/pam.d/`. See [platform_configuration.md](#ch-security-guide-platform-configuration-md).

**Security notes:** PAM conversations with non-hidden prompts are refused at
the engine level, not just at policy level. The `pam_policy_enabled` policy
option must be set to activate this family.

---

### certificate_mtls

**What it is:** Mutual TLS client certificate authentication.

**Registry:** `supports_authn = true`, dependency: `tls_x509`,
`live_evidence_required = true`, `group_materialization_required = true`,
`channel_binding_required = true`.

**Live adapter:** `ValidateCertificate` function present. Required payload fields:
`subject`, `san`, `chain`, `revoked`, `eku`, `fingerprint`.

**Validation rules:**
- `chain` must be `"trusted"` — untrusted chains return `certificate_chain_untrusted`
- `revoked` must be `false` — revoked certificates return `certificate_revoked`
- `eku` must be `"clientAuth"` — incorrect EKU returns `certificate_eku_invalid`
- `fingerprint` must be a 64-character hex string
- Group evidence (`groups` or `group`) must be present — `certificate_group_materialization_required`

**The certificate_mtls probe confirms:**
- `mtls_ok` — authentication succeeds with valid credential
- `group_materialization_required` — authentication without group evidence fails

**Security notes:** Certificate revocation status must be checked and confirmed
before the certificate is accepted. The engine will not accept a certificate
that is marked `revoked = true`. Group materialization is required; a certificate
that authenticates but does not produce group membership cannot complete
authentication.

---

### ldap_ad

**What it is:** LDAP / Active Directory bind authentication with group
synchronization.

**Registry:** `supports_authn = true`, dependency: `ldap_client`,
`live_evidence_required = true`, `group_materialization_required = true`.

**Live adapter:** `ValidateLdap` function present. Required payload fields:
`user`, `endpoint`, `starttls`, `bind`. One of `password`, `bind_secret`, or
`bind_token` must also be present.

**Validation rules:**
- `starttls` must be `true` — plain-text LDAP binds return `ldap_starttls_required`
- `bind` must be `"allow"` — a denied bind returns `ldap_bind_denied`
- Group evidence (`groups` or `group`) must be present — `ldap_group_materialization_required`

**The ldap_ad probe confirms:**
- `ldap_auth` — authentication succeeds on the fixture path
- `ldap_sync` — group synchronization via `EngineSyncExternalGroups` succeeds and sets `materialized = true`
- `ldap_explain` — membership path explanation via `EngineExplainMembership` is available and sets `explainable = true`

**Security notes:** StartTLS is enforced at the engine level. There is no
configuration option to bypass it. Group synchronization is required; a
principal whose LDAP groups cannot be resolved cannot authenticate.

---

### kerberos_pac

**What it is:** Kerberos ticket authentication with PAC (Privilege Attribute
Certificate) group extraction.

**Registry:** `supports_authn = true`, dependency: `gssapi_krb5`,
`live_evidence_required = true`, `group_materialization_required = true`.

**Live adapter:** `ValidateKerberos` function present. Required payload fields:
`spn`, `subject`, `nonce`, `kdc`, `sig`, `exp`, `pac_groups`.

**Validation rules:**
- `sig` must be a 64-character hex string — invalid signatures return `kerberos_signature_invalid`
- `exp` must be a future timestamp in milliseconds — expired tickets return `kerberos_ticket_expired`
- All required fields must be present

**The kerberos_pac probe confirms:**
- `kerberos_auth` — authentication succeeds on the fixture path
- `effective_only_no_path` — membership path explanation is not available
  (`!exp_r.ok`); the kerberos family provides effective groups from the PAC
  only and does not support path explanation

**Security notes:** The PAC provides effective group membership at ticket
issuance time. Group membership is not re-queried after authentication; a
principal's effective groups are those encoded in the ticket. Ticket expiry is
enforced.

---

### oidc_jwt

**What it is:** OpenID Connect JWT authentication. Validates ID tokens issued by
an OIDC provider.

**Registry:** `supports_authn = true`, dependency: `oidc_jwt_client`,
`live_evidence_required = true`, `group_materialization_required = true`.

**Live adapter:** `ValidateOidcJwt` function present. Required payload fields:
`iss`, `aud`, `sub`, `alg`, `exp`, `sig`, `groups`, `validator`.

**Validation rules:**
- `validator` must be `"jwks"` or `"introspection"` — other values return `oidc_jwt_validator_boundary_required`
- `alg` must not be `"none"` — returns `oidc_jwt_alg_none_forbidden`
- `sig` must be a 64-character hex string
- `exp` must be a future timestamp
- All required fields must be present

**The oidc_jwt probe confirms:**
- `oidc_auth` — authentication succeeds
- `overage_requires_sync` — when `groups_overage:true` is set (groups claim
  exceeds the token limit), authentication fails; a group sync is required before
  the principal can be admitted
- `oauth_not_login` — the `oauth_validator` sub-family cannot be used directly
  for login (validator-only)

**Security notes:** JWT algorithm `none` is explicitly rejected. Token
validation must use either JWKS or token introspection; in-process verification
without an external validator boundary is not supported. Groups overage
(when the IDP omits groups from the token due to claim size limits) requires
explicit group synchronization before login.

---

### saml

**What it is:** SAML 2.0 assertion authentication.

**Registry:** `supports_authn = true`, dependency: `saml_xmlsig`,
`live_evidence_required = true`, `group_materialization_required = true`.

**Live adapter:** `ValidateSaml` function present. Required payload fields:
`issuer`, `audience`, `subject`, `not_on_or_after`, `signature`, `attributes`.

**Validation rules:**
- `signature` must be a 64-character hex string — invalid signatures return `saml_signature_invalid`
- `not_on_or_after` must be a future timestamp — expired assertions return `saml_assertion_expired`

**The saml probe confirms:**
- `saml_auth` — authentication succeeds
- `stale_assertion_rejected` — a request with `freshness:stale` is rejected

**Security notes:** SAML assertion freshness is enforced. Stale assertions are
rejected at the engine level. XML signature validation requires the `saml_xmlsig`
dependency.

---

### webauthn

**What it is:** WebAuthn / FIDO2 authenticator-based authentication. Used as an
MFA second factor or as a standalone authenticator.

**Registry:** `supports_authn = true`, `policy_gated = true` (option:
`webauthn_policy_enabled`), `supports_mfa = true`, dependency: `webauthn_fido2`,
`live_evidence_required = true`.

**Live adapter:** `ValidateWebAuthn` function present. Required payload fields:
`challenge`, `rp`, `origin`, `credential`, `subject`, `uv`, `exp`, `signature`.

**Validation rules:**
- `uv` (user verification) must be `true` — `webauthn_user_verification_required`
- `signature` must be a 64-character hex string
- `exp` must be a future timestamp

**The webauthn_mfa probe confirms:**
- `webauthn_auth` — authentication succeeds when `mfa:present` is set
- `missing_mfa_rejected` — authentication without `mfa:present` fails
- `factor_not_login` — the `factor_chain` sub-family cannot be used directly for
  login (it is a multi-factor chain composition, not a standalone login)

**Security notes:** User verification is required. The `webauthn_policy_enabled`
policy option must be set. MFA evidence must be present in the request.

---

### factor_chain

**What it is:** Multi-factor authentication chain. Composes a primary
authentication context with a second factor.

**Registry:** `supports_authn = true`, `policy_gated = true` (option:
`factor_chain_policy_enabled`), `supports_mfa = true`, dependency: `webauthn_fido2`.

**Live adapter:** `ValidateFactorChain` function present. Required payload fields:
`primary_auth_context_uuid`, `factor_policy_uuid`, `factor_results`,
`challenge_transcript_hash`, `subject`.

**Validation rules:**
- `factor_results` must be `"allow"` — anything else returns `SECURITY.MFA_REQUIRED`
- `challenge_transcript_hash` must be a 64-character hex string

**Security notes:** The factor chain is a composition mechanism, not a standalone
login method. It requires a completed primary authentication context UUID. The
`factor_chain_policy_enabled` policy option must be set.

---

### radius

**What it is:** RADIUS protocol authentication.

**Registry:** `supports_authn = true`, `policy_gated = true` (option:
`radius_policy_enabled`), dependency: `radius_client`, `live_evidence_required = true`,
`group_materialization_required = true`.

**Live adapter:** `ValidateRadius` function present. Required payload fields:
`user`, `authenticator`, `result`, `attribute`, `shared_secret_handle`.

**Validation rules:**
- `authenticator` must be a 64-character hex string — invalid values return `radius_authenticator_invalid`
- `result` must be `"accept"` — any other result returns `radius_rejected`

**The radius probe confirms:**
- `radius_auth` — authentication succeeds
- `radius_no_path` — membership path explanation is not available for RADIUS
  principals (similar to kerberos_pac; effective groups only)

**Security notes:** RADIUS access-accept is the only admitted result. The
shared secret is handled via a `shared_secret_handle` (protected material
reference) rather than inline. The `radius_policy_enabled` policy option must
be set.

---

### proxy_assertion

**What it is:** Trusted middle-tier assertion. Used when a trusted proxy
component forwards a user's identity to the engine rather than requiring the
user to authenticate directly.

**Registry:** `supports_authn = true`, `policy_gated = true` (option:
`proxy_assertion_policy_enabled`), `channel_binding_required = true`,
dependency: `proxy_assertion_verifier`, `live_evidence_required = true`.

**Live adapter:** `ValidateProxy` function present. Required payload fields:
`iss`, `sub`, `aud`, `proxy`, `source`, `manager_trust`, `listener_binding`,
`exp`, `sig`.

**Validation rules:**
- `source` must be `"trusted"` — returns `proxy_source_untrusted`
- `manager_trust` must be `"trusted"` — returns `proxy_manager_trust_required`
- `listener_binding` must be `"verified"` — returns `proxy_listener_binding_required`
- `sig` must be a 64-character hex string
- `exp` must be a future timestamp

**The proxy_assertion probe confirms:**
- `proxy_ok` — assertion succeeds
- `replay_rejected` — a replayed assertion fails

**Security notes:** This provider is for trusted middle-tier components only,
not for direct user authentication. All three trust conditions (source,
manager_trust, listener_binding) must be satisfied simultaneously. Channel
binding is required. This family must not be exposed to untrusted clients.

---

### token_api_key

**What it is:** API key / authkey token authentication.

**Registry:** `supports_authn = true`, no guards, no policy gate, no dependency,
`live_evidence_required = true`, `group_materialization_required = true`.

**Live adapter:** `ValidateApiKey` function present. Required payload fields:
`key_id`, `proof`, `generation`, `subject`, `groups`.

**Validation rules:**
- `proof` must be a 64-character hex string — returns `api_key_proof_invalid`
- Group evidence (`groups`) must be present

**The token_authkey probe confirms:**
- `token_auth` — authentication succeeds
- `revoked` — revoke via `EngineRevokeToken` sets `revoked = true`
- `unsynced_rejected` — a token authentication without proper group state fails

**Security notes:** Token revocation is supported and must be performed before
issuing a replacement token. Group evidence is required; an API key without
associated group membership cannot complete authentication.

---

### bearer_token

**What it is:** Opaque bearer token. A simpler token format without the
structured proof of `token_api_key`.

**Registry:** `supports_authn = true`, no guards, no policy gate, no dependency.

**Live adapter:** `ValidateBearerToken` function present. Required payload fields:
`token_id`, `proof`, `exp`, `subject`.

**Validation rules:**
- `proof` must be a 64-character hex string
- `exp` must be a future timestamp

**Security notes:** No group evidence is required. This family is intended for
short-lived, scoped access tokens. No probe directory exists for this family
specifically; it is exercised through the live adapter only.

---

### workload_identity

**What it is:** Workload identity authentication for non-human service principals
using SPIFFE SVID or workload OIDC tokens.

**Registry:** `supports_authn = true`, dependency: `spiffe_svid_or_workload_oidc`,
`live_evidence_required = true`, `group_materialization_required = true`.

**Live adapter:** `ValidateWorkload` function present. Required payload fields:
`trust_bundle`, `exp`, `sig`, `service_group`, and at least one of `spiffe_id`
or `sub`.

**Validation rules:**
- If `spiffe_id` is present, it must begin with `spiffe://`
- `sig` must be a 64-character hex string
- `exp` must be a future timestamp
- `service_group` must be present

**The workload_identity probe confirms:**
- `workload_auth` — authentication succeeds
- `service_mapping_required` — authentication with `provider:workload_identity`
  but without proper service mapping fails

**Security notes:** Service group mapping is required. A workload that
authenticates but cannot be mapped to a service group cannot complete
authentication.

---

### managed_identity

**What it is:** Cloud-managed identity authentication (Azure Managed Identity,
GCP Workload Identity, or similar).

**Registry:** `supports_authn = true`, dependency: `spiffe_svid_or_workload_oidc`,
`live_evidence_required = true`, `group_materialization_required = true`.

**Live adapter:** `ValidateManagedIdentity` function present. Required payload
fields: `issuer`, `audience`, `subject`, `exp`, `sig`, `service_group`.

**Validation rules:**
- `sig` must be a 64-character hex string
- `exp` must be a future timestamp

**Security notes:** This family shares the `spiffe_svid_or_workload_oidc`
dependency with `workload_identity`. No probe directory exists for this family
specifically; it is exercised via the live adapter.

---

### Infrastructure Probes (Not Login Families)

The following probe tools test cross-cutting infrastructure rather than discrete
login flows. They do not correspond to standalone authentication families.

**challenge_state** — Tests the `EngineContinueAuthChallenge` API. Verifies
that expired, replayed, and rate-limited challenges are all rejected. Used by
WebAuthn and any other challenge-based family.

**manifest_policy** — Tests `EngineRegisterAuthProvider` trust evaluation.
Verifies four rejection cases: invalid signature, missing dependency, unsupported
ABI, and stale implementation version.

**registry** — Tests the provider registration lifecycle: valid registration,
duplicate UUID rejection, unknown family rejection, and disabled provider
rejection.

**sblr_api** — Tests the SBLR dispatch path for security operations. Confirms
that `security.register_auth_provider`, `security.authenticate_provider`, and
`security.revoke_token` are all accessible via the SBLR dispatch and require
the `AUTH_PROVIDER_ADMIN` right.

**secret_provider** — Tests `EngineRotateCredential`. Verifies that credential
rotation succeeds, that plaintext storage is refused, and that missing material
is rejected.

---

## Related Pages

- [authentication_and_providers.md](#ch-security-guide-authentication-and-providers-md) — provider architecture
- [platform_configuration.md](#ch-security-guide-platform-configuration-md) — OS-specific notes per family
- [security_policies_and_crypto.md](#ch-security-guide-security-policies-and-crypto-md) — policy-gated family configuration




===== FILE SEPARATION =====

<!-- chapter source: Security_Guide/platform_configuration.md -->

<a id="ch-security-guide-platform-configuration-md"></a>

# Platform Configuration

## Purpose

This page documents the authentication and security configuration differences
that are actually verified in the ScratchBird source tree for Linux, Windows,
and BSD targets. It explicitly distinguishes verified differences from
platform behavior that could not be confirmed from the available source.

The guiding principle: only document verified differences. If the source does
not differentiate a configuration option or plugin by OS, that option is
platform-neutral and is documented as such. Do not configure for differences
that are not present in the source.

## Platform Guard Verification

A search for `#ifdef _WIN32`, `#ifdef __linux__`, BSD macros, and POSIX guards
across `src/engine/internal_api/security/` yielded the following:

**Confirmed platform-specific code:**

- `protected_material_api.cpp` contains a `#if defined(_WIN32)` guard around
  the atomic file replace implementation:
  - On Windows: uses `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`
  - On non-Windows (Linux, BSD, macOS): uses `std::filesystem::rename`
  - This is an implementation detail of the protected material catalog's atomic
    write path. It is not operator-visible.

- `protected_material_api.cpp` includes `<windows.h>` under `#ifdef _WIN32` for
  the `MoveFileExW` call.

**No other platform guards were found** in the security API headers or
implementation files. The live adapter, authentication API, provider model,
plugin trust API, and crypto policy contain no `#ifdef _WIN32`, `#ifdef __linux__`,
or BSD-specific guards.

This means the security API surface itself is platform-neutral. Platform
differences arise from the runtime dependencies that specific plugin families
require, not from the engine's security API code.

## Configuration File Overview

Configuration for ScratchBird's security behavior lives in:

| File | Purpose |
|------|---------|
| `config/templates/SBsrv.conf` | Server process configuration; includes `[server.security]` section |
| `config/templates/SBgate.conf` | Listener/gateway configuration; includes TLS and auth options |
| `config/templates/SBmgr.conf` | Manager configuration; includes `management_auth_required` |

These files are cross-platform INI/flat-key format. Their keys are the same on
all supported operating systems. What differs by platform is whether the runtime
dependency for a given provider family is installed and reachable.

### SBsrv.conf Security Section

```ini
[server.security]
provider_family = local_password
provider_state = healthy
default_policy_installed = true
```

`provider_family` specifies the authentication provider family for the server
instance. The default is `local_password`. This key is platform-neutral.

`provider_state` is `healthy` in the template. `default_policy_installed = true`
indicates the default policy pack has been seeded.

### SBgate.conf TLS Setting

```ini
tls_required = true
```

TLS is required for the native listener by default. This setting is
platform-neutral. The TLS implementation uses the `tls_x509` dependency
(required by `certificate_mtls`). The underlying TLS library must be present on
the target platform.

### SBmgr.conf Management Auth

```ini
manager.management_auth_required = true
```

Management authentication is required by default. This is platform-neutral.

## Provider Families by Platform Orientation

The following table classifies each provider family by its platform orientation
based on its dependency and the behavior of the `peer`/`ident` family (which
uses POSIX socket credentials):

| Family | Platform orientation | Notes |
|--------|---------------------|-------|
| `local_password` | Platform-neutral | No external dependency |
| `scram_sha256` | Platform-neutral | No external dependency |
| `pam` | POSIX-oriented (Linux, BSD) | `pam` dependency; PAM is a POSIX standard. Windows lacks a native PAM subsystem. The `pam` dependency is expected to be absent on Windows without a compatibility layer. |
| `peer` / `ident` | POSIX-oriented (Linux, BSD) | `peer` uses OS socket credentials (`SO_PEERCRED` or equivalent). This mechanism is POSIX-specific. The source does not show a Windows implementation path for this family. |
| `certificate_mtls` | Platform-neutral | `tls_x509` dependency. TLS libraries are available on all supported platforms. |
| `ldap_ad` | Platform-neutral | `ldap_client` dependency. LDAP client libraries are available on Linux, BSD, and Windows. |
| `kerberos_pac` | Platform-neutral | `gssapi_krb5` dependency. Kerberos GSSAPI is available on Linux, BSD, and Windows (though library names differ). |
| `oidc_jwt` | Platform-neutral | `oidc_jwt_client` dependency. HTTP-based; platform-neutral. |
| `saml` | Platform-neutral | `saml_xmlsig` dependency. Platform-neutral. |
| `webauthn` | Platform-neutral | `webauthn_fido2` dependency. Platform-neutral. |
| `radius` | Platform-neutral | `radius_client` dependency. Platform-neutral. |
| `proxy_assertion` | Platform-neutral | `proxy_assertion_verifier` dependency. Platform-neutral. |
| `token_api_key` | Platform-neutral | No external dependency. |
| `bearer_token` | Platform-neutral | No external dependency. |
| `workload_identity` | Platform-neutral | `spiffe_svid_or_workload_oidc` dependency. Platform-neutral. |
| `managed_identity` | Platform-neutral | `spiffe_svid_or_workload_oidc` dependency. Platform-neutral. |

**Important caveat:** The source does not contain explicit `#ifdef _WIN32` or
`#ifdef __linux__` guards for any of the provider families. The
POSIX-orientation of `pam` and `peer`/`ident` is inferred from the nature of
their dependencies (the POSIX PAM API and POSIX socket credentials), not from
confirmed platform guards in the security code. Operators should verify against
the build system and their target platform before relying on these families.

## Linux-Specific Notes

### PAM Configuration

On Linux, the PAM provider requires:

1. The `pam` shared library to be present on the build.
2. A PAM service file in `/etc/pam.d/` whose name matches the `service` field
   in the provider payload.
3. The PAM conversation must complete with a `hidden` or `secret` prompt type.
   Any other prompt type is refused by the engine at the live adapter level.

The engine enforces `pam_policy_enabled` must be set in the provider policy
before PAM is active.

### Peer / Ident

On Linux, the `peer` family uses `SO_PEERCRED` to obtain the connecting
process's effective UID. The engine requires a successful credential exchange;
a stale or unavailable credential is rejected.

The `ident` family is available on Linux but is generally not recommended for
production deployments because RFC 1413 ident servers are not commonly
deployed and the protocol offers no cryptographic assurance.

### Protected Material File Path

On Linux, the protected material catalog atomic replacement uses
`std::filesystem::rename`. This is a single syscall on most Linux filesystems
and provides atomic replacement semantics within the same filesystem. The
`rename` call may not be atomic across filesystem boundaries. Place the
protected material catalog and its temporary write path on the same filesystem.

### Entropy / Random

The source does not contain explicit entropy source configuration for Linux.
The engine uses the C++ standard library and OpenSSL for cryptographic
operations, both of which use platform entropy sources (typically `/dev/urandom`
or `getrandom()` on Linux). This is not operator-configurable via the
SBsrv.conf security section.

## Windows-Specific Notes

### Protected Material Atomic Replacement

The source confirms one Windows-specific behavior: the atomic file replacement
in `protected_material_api.cpp` uses `MoveFileExW` with
`MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`. This is consistent with
atomic replacement semantics on Windows NTFS. The operator implication is the
same as on Linux: the temporary and target paths must be on the same volume.

### PAM and Peer/Ident

The `pam` and `peer`/`ident` families have POSIX-oriented dependencies. The
source does not contain a Windows implementation for either of these families.
Operators deploying on Windows should not configure these families. If they are
needed, verify independently that a compatible PAM subsystem or socket
credential mechanism is available on the target build.

### Kerberos on Windows

The `kerberos_pac` family uses the `gssapi_krb5` dependency. On Windows, MIT
Kerberos or another GSSAPI provider may be used. The source does not confirm
any SSPI (Security Support Provider Interface) integration. Do not assume that
native Windows SSPI Kerberos is supported unless verified against the build for
your target version.

### LDAP on Windows

The `ldap_ad` family uses the `ldap_client` dependency. On Windows, this is
expected to be a third-party LDAP client library rather than the Windows
LDAP API (`wldap32.dll`). The source does not confirm any wldap32 integration.
Verify the expected LDAP client library against your build.

## BSD-Specific Notes

### PAM on BSD

FreeBSD and OpenBSD both have native PAM implementations. The `pam` family is
expected to work on BSD with a PAM service file in the appropriate location
(typically `/etc/pam.d/` on FreeBSD). The engine's requirements (hidden prompt,
account phase, session phase) are the same on BSD as on Linux.

### Peer / Ident on BSD

The `peer` family's `SO_PEERCRED` equivalent on BSD is `LOCAL_PEERCRED`
(FreeBSD) or a similar mechanism. The source does not confirm platform-specific
handling for BSD socket credentials; the behavior at the live adapter level is
the same (the fixture path is used, and real deployment verification is
required).

### Protected Material File Path on BSD

On BSD, the `std::filesystem::rename` path is used (same as Linux). The same
filesystem-boundary constraint applies.

## Unverified Platform Behaviors

The following behaviors could not be confirmed from the source tree and should
be verified against the build before relying on them:

- Whether `peer` authentication via BSD `LOCAL_PEERCRED` is implemented in the
  real SBgate socket binding code (the source read here covers the engine
  internal API only).
- Whether the Kerberos GSSAPI integration uses MIT krb5 on all platforms or
  whether it adapts to the platform's native Kerberos library.
- Whether `ident` authentication is available as a production option on any
  platform (the probe confirms the fixture path works, not that a real RFC 1413
  client is implemented).
- Whether any platform requires additional TLS configuration (CA bundle path,
  certificate store location) beyond `tls_required = true` in `SBgate.conf`.
- Whether Windows Security Support Provider Interface (SSPI) is integrated for
  any provider family. The source shows no SSPI headers or guards.

Operators should treat these items as requiring independent verification against
the build for their target platform and ScratchBird version.

## Related Pages

- [auth_plugin_families.md](#ch-security-guide-auth-plugin-families-md) — per-family dependency and requirements
- [authentication_and_providers.md](#ch-security-guide-authentication-and-providers-md) — provider architecture
- [security_policies_and_crypto.md](#ch-security-guide-security-policies-and-crypto-md) — cryptographic policy




===== FILE SEPARATION =====

<!-- chapter source: Security_Guide/security_policies_and_crypto.md -->

<a id="ch-security-guide-security-policies-and-crypto-md"></a>

# Security Policies and Cryptographic Policy

## Purpose

This page describes how security policies are structured and applied in
ScratchBird: the policy-pack model, the `default-local-password` pack that is
seeded at database creation time, the cryptographic policy implemented in
`security_crypto_policy`, and the hardening options available for token and
credential management.

Sources:
- `resources/policy-packs/default-local-password/` — default policy pack
- `src/engine/internal_api/security/security_crypto_policy.hpp`, `.cpp`
- `src/engine/internal_api/security/auth_provider_model.hpp` — `AuthProviderPolicy`
- `src/engine/internal_api/security/auth_provider_policy_api.hpp`
- `config/templates/SBsrv.conf`

## The Policy-Pack Model

A **policy pack** is a signed bundle of JSON documents that seeds the security
policy of a new database at creation time. It is not applied at runtime; once
a database is created, its policy is owned by the catalog and can only be
mutated through database commands (via `EngineMutatePolicy`).

Source: `identity_security_and_policy.md` — "Policies are created as part of
database bootstrap and can be mutated only through database commands, not through
filesystem policy packs at runtime."

The policy pack format is described by the `POLICY_PACK_MANIFEST.json` file.

### Policy Pack Manifest Fields

```json
{
  "schema_version": 1,
  "policy_pack_uuid": "018f7a10-1280-7000-8000-000000000001",
  "policy_pack_id": "default-local-password",
  "policy_pack_version": "1.0.0",
  "description": "First public release default policy pack: local password provider only.",
  "content_hash_algorithm": "sha256",
  "content_sha256": "<hash>",
  "signature_status": "signature-ready-unsigned",
  "provenance": {
    "source": "public-project-tree",
    "generated": false,
    "private_inputs_required": false,
    "external_provider_runtime_required": false
  },
  "create_time_only": true,
  "post_create_filesystem_authority": false
}
```

Key fields:

- `signature_status: "signature-ready-unsigned"` — the pack is ready to
  sign but has not been signed in the public project tree. Operators deploying
  from the source tree should be aware of this status.
- `create_time_only: true` — the pack is applied only at database creation time.
- `post_create_filesystem_authority: false` — after creation, the filesystem
  pack has no authority over the database's policy.
- `content_sha256` — the pack's content integrity hash. Verify this against the
  files before use.

### Default-Local-Password Pack Contents

The `default-local-password` pack contains five policy files:

| File | Purpose |
|------|---------|
| `policies/security_providers.json` | Provider family declarations and their default enabled state |
| `policies/roles.json` | Standard role seed records |
| `policies/groups.json` | Standard group seed records |
| `policies/grants.json` | Standard privilege grant seed records |
| `policies/policy_profiles.json` | Profile-area-to-mode mappings governing default system behavior |
| `catalog_materialization.json` | Catalog materialization metadata |

### Provider Policy in the Pack

From `policies/security_providers.json`:

Only `local_password` is enabled by default:

```json
{
  "provider_family": "local_password",
  "enabled_by_default": true,
  "authority": "durable_catalog_row",
  "credential_verifier_policy": "local_password_verifier_required",
  "external_provider": false
}
```

All other provider families — `ldap_ad`, `oidc_jwt`, `saml`, `kerberos_pac`,
`pam`, `radius`, `webauthn`, `certificate_mtls`, `workload_identity`,
`managed_identity`, `custom_cpp_plugin` — are declared with
`"enabled_by_default": false` and `"authority": "unsupported_by_default"`.

This means a freshly created database will reject authentication attempts from
any provider family other than `local_password` until the operator explicitly
enables an additional family and satisfies its dependency requirements.

### Policy Profiles

From `policies/policy_profiles.json`:

Policy profiles govern the default posture for named areas of engine behavior.
The profiles seeded by the default pack are:

| Profile area | Mode | Meaning |
|-------------|------|---------|
| `security_provider_selection` | `local_password_only` | Only local password is active |
| `standard_roles_groups_grants` | `uuid_catalog_seed` | Roles/groups/grants are seeded from UUID catalog records |
| `default_security_posture` | `deny_by_default` | No access is assumed; all access requires explicit grant |
| `memory_resource_governance` | `configured_policy_required` | Memory policy must be explicitly configured |
| `storage_filespace_page_policy` | `local_durable_fail_closed` | Storage operations fail closed |
| `transaction_mga_cleanup_archive_backup_forward` | `mga_inventory_authority` | MGA owns transaction inventory |
| `optimizer_statistics_feedback` | `catalog_backed_or_diagnostic_only` | Optimizer stats are catalog-backed |
| `index_maintenance` | `provider_admission_required` | Index maintenance requires admission |
| `agent_policy` | `evidence_not_authority` | Agent evidence is not authority |
| `diagnostics` | `stable_redacted_diagnostics` | Diagnostics are redacted |
| `observability` | `redacted_evidence_only` | Observability output is redacted |
| `unsupported_feature_behavior` | `deterministic_fail_closed` | Unsupported features fail closed |
| `cluster_boundary` | `external_provider_required` | Cluster operations require an external provider |
| `release_default_configuration` | `secure_defaults` | Release defaults are secure |

The `deny_by_default` posture for `default_security_posture` is the source for
the fail-closed invariant described in [security_model_overview.md](#ch-security-guide-security-model-overview-md).

## Cryptographic Policy

Source: `security_crypto_policy.hpp`, `security_crypto_policy.cpp`.

The security crypto policy provides the approved cryptographic primitives for
authentication and security-event integrity. The source comment documents:

> OpenSSL supplies the approved SHA-256/HMAC-SHA-256 algorithms; equality
> checks stay constant-time with respect to input length.

### Approved Primitives

| Function | Algorithm | Use |
|----------|-----------|-----|
| `SecurityConstantTimeEqual` | Constant-time string comparison | Comparing credentials or tokens |
| `SecuritySha256Hex` | SHA-256 | Integrity of security payloads |
| `SecurityHmacSha256Hex` | HMAC-SHA-256 | Keyed integrity of security payloads |

### Cluster Evidence Integrity

The engine also supports cluster catalog evidence integrity via
`EvaluateClusterEvidenceIntegrity`. The supported protection modes are:

| Mode | Algorithm |
|------|-----------|
| `sha256` | SHA-256 digest (default for simple integrity) |
| `hmac_sha256` | HMAC-SHA-256 with a named key |
| `signature_ready_ed25519` | Signature-ready metadata with Ed25519 key reference |

The `ClusterEvidenceIntegrityResult` carries `fail_closed = true` and
`weak_evidence_rejected = false` (becomes `true` when a weak evidence attempt
is made). The source comment states: "Weak checksums cannot support catalog
authority claims." This means weak hash algorithms (MD5, SHA-1, CRC) will be
rejected if they are presented as catalog authority evidence.

### Algorithm Downgrade Denial

The live adapter enforces algorithm downgrade denial across all provider families.
From `auth_provider_live_adapter.cpp` — `RejectAdversarialPayload`:

```
alg in {none, md5, sha1, rs1} → provider_algorithm_downgrade_denied
```

This check runs before family-specific validation. It applies to all provider
families that present an `alg` field in their payload.

Operators configuring OIDC JWT providers must ensure the IDP issues tokens
with an approved algorithm. The `oidc_jwt` family additionally enforces
`alg != "none"` as a family-specific check.

## Provider Policy Hardening

Source: `auth_provider_model.hpp` — `AuthProviderPolicy`.

Each admitted provider's policy can be configured for stricter behavior:

### Disabling Password-Compat Fallback

```json
"allow_password_compat": false
```

The default is `false`. Cleartext password compatibility (`password_compat`
family) is refused unless this is explicitly set to `true`. The `password_compat`
family is also `policy_gated` and requires `allow_password_compat` to be set in
the provider policy. Do not set this to `true` in new deployments.

### Requiring MFA

```json
"require_mfa": true
```

When set, the provider policy requires MFA evidence to be present in every
authentication request. This works in conjunction with `webauthn` or
`factor_chain` families.

### Requiring Group Sync

```json
"require_group_sync": true
```

When set, the provider policy requires group synchronization to have been
performed recently. Combined with `stale_behavior: "deny"`, this prevents
principal authentication when group state cannot be verified.

### Stale Evidence Behavior

```json
"stale_behavior": "deny",
"allow_cache_stale": false,
"cache_bounds": "deny_when_expired"
```

These are the default values. They enforce fail-closed behavior: when the
provider's external service is unavailable, cached credentials expire and new
authentication attempts are denied rather than serving stale data.

### Audit and Redaction Policy

```json
"audit_policy_ref": "<policy-uuid>",
"redaction_policy_ref": "<policy-uuid>"
```

Each provider can reference a separate audit policy and redaction policy. These
should be configured to route provider-level authentication events to the audit
system and to apply appropriate redaction to authentication evidence in
diagnostic output.

## Token Hardening

The following token hardening behaviors are enforced uniformly across all
token-based provider families (OIDC JWT, SAML, WebAuthn, bearer token, API key):

| Hardening | Enforcement point | Diagnostic code |
|-----------|------------------|-----------------|
| Algorithm downgrade denial | `RejectAdversarialPayload` in live adapter | `provider_algorithm_downgrade_denied` |
| Replay detection | `RejectAdversarialPayload` in live adapter | `provider_replay_denied` |
| Token revocation | `RejectAdversarialPayload` in live adapter | `SECURITY.TOKEN_REVOKED` |
| Assertion freshness | Family-specific `Validate*` | Family-specific (e.g., `oidc_jwt_expired`, `saml_assertion_expired`) |
| User verification for WebAuthn | `ValidateWebAuthn` | `webauthn_user_verification_required` |
| OIDC algorithm `none` | `ValidateOidcJwt` | `oidc_jwt_alg_none_forbidden` |
| OIDC validator boundary | `ValidateOidcJwt` | `oidc_jwt_validator_boundary_required` |

These checks are not configurable; they are source-enforced invariants.

## Credential Hardening

From `security_principal_lifecycle.hpp` — `EngineSecurityCreatePrincipalResult`:

```
plaintext_material_stored = false   — confirmed by default
protected_material_redacted = true  — confirmed by default
```

The engine returns these flags to the caller after principal creation. When
`plaintext_material_stored = false` and `protected_material_redacted = true`,
no plaintext credential has been stored or returned. Operators should confirm
these flags in audit output after principal provisioning.

The diagnostic code `SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED` is emitted
if a call path attempts to return plaintext protected material.

## Policy Reload

Source: `auth_provider_policy_api.hpp` — `EngineReloadAuthProviderPolicy`.

Provider policy can be reloaded without restarting the server. The reload
operation returns the current `AuthProviderPolicy` and sets
`reloaded = true` on success. This allows operators to tighten or relax
provider policy (within the bounds of what the engine permits) without
a full server restart.

## Related Pages

- [security_model_overview.md](#ch-security-guide-security-model-overview-md) — fail-closed invariant, epochs
- [authentication_and_providers.md](#ch-security-guide-authentication-and-providers-md) — provider policy fields
- [auth_plugin_families.md](#ch-security-guide-auth-plugin-families-md) — per-family policy-gate requirements
- [platform_configuration.md](#ch-security-guide-platform-configuration-md) — platform-specific configuration
- [Operations and Administration: Identity, Security, and Policy](#ch-operations-administration-identity-security-and-policy-md)




===== FILE SEPARATION =====

<!-- chapter source: Security_Guide/standard_roles_and_groups.md -->

<a id="ch-security-guide-standard-roles-and-groups-md"></a>

# Standard Roles and Groups

## Purpose

This page documents the built-in roles and groups that ScratchBird seeds during
initial security bootstrap. Understanding these objects — what rights each one
conveys, how they are seeded, and how they can be extended — is necessary before
assigning principals to any of them.

This is a **draft**. No claims herein constitute a production security
certification or a promise of external audit compliance.

Source files: `src/engine/internal_api/security/standard_bundle_api.hpp`,
`standard_bundle_api.cpp`, `security_model.cpp`.

## Definitions

**Standard bundle** — the set of roles, groups, and policies seeded by
`EngineSeedStandardSecurityBundles`. This function is callable only by a
principal holding `SEC_GRANT_ADMIN` or by a caller carrying the internal
`security.bootstrap` trace tag.

**Seeded group** — a group whose name and UUID are materialized into the
catalog during bootstrap. Seeded groups are functional conventions, not
immutable built-ins. An operator with `SEC_GRANT_ADMIN` can add or remove
rights from them.

**Seeded role** — a role seeded during bootstrap that conveys a curated set of
rights. Role rights are materialized from the durable grant table; a role
conveys only what grants are recorded for it.

## Seeding and Bootstrap Model

Source: `standard_bundle_api.cpp` — `EngineSeedStandardSecurityBundles`.

`EngineSeedStandardSecurityBundles` is called during database creation and,
when the bootstrap tag is present, during recovery. It seeds:

| Count | Kind |
|-------|------|
| 12 | Standard groups |
| 5 | Standard roles |
| 10 | Standard policies |

The seed function uses `PersistApiBehaviorRecord` with `allow_existing = true`,
so re-running it on an already-bootstrapped database is idempotent.

The `EngineSeedStandardSecurityBundlesResult` struct carries
`groups_seeded`, `roles_seeded`, and `policies_seeded` counters that reflect
the number of records written or confirmed during the call.

## Standard Groups

The 12 seeded groups are identified by exact name. Each group maps to a set of
rights through the `GroupAllows` function in `security_model.cpp`.

### GROUP: `PUBLIC`

The `PUBLIC` pseudo-group is not assigned rights by `GroupAllows`. It is a
resolver convention for privileges that should apply to every attached session.
Granting a right `TO PUBLIC` creates a grant whose subject kind resolves to
every active principal. Use `PUBLIC` grants only for rights that are genuinely
intended for all callers.

### GROUP: `APP`

For application-tier principals — web servers, API gateways, and service
accounts that connect to the database on behalf of end users.

Rights conveyed:

| Right | Purpose |
|-------|---------|
| `CONNECT` | Allow database attachment |
| `OBS_RUNTIME_SELF` | Read own session metrics and runtime state |
| `EVENT_SUBSCRIBE` | Subscribe to event streams |
| `EVENT_PUBLISH` | Publish to event streams |
| `EVENT_DELIVERY_READ` | Read event delivery metadata |
| `EVENT_DELIVERY_ACK` | Acknowledge event delivery |

### GROUP: `DEV`

For developer accounts that need schema visibility and basic self-diagnostics.

Rights conveyed:

| Right | Purpose |
|-------|---------|
| `OBS_RUNTIME_SELF` | Read own session runtime state |
| `OBS_METRICS_READ_SELF` | Read own session metrics |
| `VISIBLE` | Assert that named objects are visible in name resolution |
| `DISCOVER` | Assert object discovery within the sandbox |
| `OBS_INDEX_PROFILE_READ` | Read index profiles (development diagnostic) |

Note: Granting `OBS_INDEX_PROFILE_READ` to a `DEV`-group principal triggers a
diagnostic advisory in `grant_api.cpp`. This right is intended as a development
diagnostic and may have performance implications.

### GROUP: `ANL`

For analytics and business intelligence principals that need read access.

Rights conveyed:

| Right | Purpose |
|-------|---------|
| `CONNECT` | Allow database attachment |
| `SELECT` | Read table and view rows |
| `VISIBLE` | Named object visibility |
| `DISCOVER` | Object discovery within the sandbox |

`ANL` principals still require explicit `GRANT SELECT ON TABLE ...` grants for
specific objects. Group membership does not bypass object-level grants.

### GROUP: `ETL` and GROUP: `SCH`

For data pipeline and scheduled-batch principals that execute routines and read
metrics.

Rights conveyed (both groups):

| Right | Purpose |
|-------|---------|
| `CONNECT` | Allow database attachment |
| `EXECUTE` | Invoke functions and procedures |
| `OBS_METRICS_READ_FAMILY` | Read metrics for a specific family |

`SCH` is intended for schema-maintenance automation. Both groups require
explicit object grants for the tables, views, and routines they need.

### GROUP: `DBA`

For database administrator principals. `DBA` conveys broad data access and
diagnostic rights but does not include security administration rights
(`SEC_GRANT_ADMIN`, `SEC_IDENTITY_ADMIN`, `SEC_MEMBERSHIP_ADMIN`).

Rights conveyed:

| Right | Purpose |
|-------|---------|
| `VISIBLE`, `DISCOVER`, `LIST_CHILD` | Full object visibility and traversal |
| `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `EXECUTE` | Full data manipulation |
| `CREATE`, `ALTER`, `DROP` | Schema lifecycle operations |
| `DOMAIN_USE`, `DOMAIN_CAST`, `DOMAIN_METHOD`, `DOMAIN_POLICY_ADMIN`, `DOMAIN_UNMASK` | Domain operations and masking override |
| `UDR_INSPECT`, `UDR_INVOKE` | Inspect and invoke user-defined routines |
| `BACKUP_CREATE`, `BACKUP_RESTORE`, `BACKUP_INSPECT` | Backup creation, restore, and inspection (not full `BACKUP_CONTROL`) |
| `EVENT_ADMIN`, `EVENT_CREATE`, `EVENT_ALTER`, `EVENT_DROP`, `EVENT_SUBSCRIBE`, `EVENT_PUBLISH`, `EVENT_DELIVERY_READ`, `EVENT_DELIVERY_ACK` | Full event lifecycle |
| `OBS_RUNTIME_ALL`, `OBS_METRICS_READ_ALL`, `OBS_METRICS_READ_FAMILY`, `OBS_METRICS_READ_DATABASE`, `OBS_METRICS_READ_NODE` | Full observability read |
| `OBS_INDEX_PROFILE_READ` | Index profile diagnostics |
| `OBS_CONFIG_INSPECT` | Read configuration state |
| `OBS_CLUSTER_HEALTH_INSPECT` | Read cluster health |
| `OBS_DATA_MOVEMENT_INSPECT` | Inspect data movement operations |
| `MGA_TRANSACTION_INSPECT`, `MGA_RECOVERY_INSPECT`, `MGA_CLEANUP_INSPECT`, `MGA_CLEANUP_CONTROL`, `MGA_HORIZON_INSPECT`, `MGA_LINEAGE_INSPECT`, `MGA_METRICS_READ` | MGA diagnostics and cleanup control |

### GROUP: `AUD`

For security audit principals who need to review security events, metrics, and
agent evidence. Does not include write or control rights.

Rights conveyed:

| Right | Purpose |
|-------|---------|
| `AUDIT_READ` | Read audit records |
| `OBS_RUNTIME_ALL` | Read all session runtime state |
| `OBS_METRICS_READ_ALL`, `OBS_METRICS_READ_FAMILY`, `OBS_METRICS_READ_DATABASE`, `OBS_METRICS_READ_NODE`, `OBS_METRICS_READ_CLUSTER`, `OBS_METRICS_EXPORT_READ` | Full metrics read |
| `SEC_AUTH_METRICS_READ` | Read authentication provider metrics |
| `OBS_POLICY_READ` | Read policy objects |
| `OBS_CONFIG_INSPECT` | Read configuration state |
| `OBS_CLUSTER_HEALTH_INSPECT` | Read cluster health |
| `SUPPORT_EXPORT` | Generate support bundles |
| `OBS_AGENT_STATE_READ`, `OBS_AGENT_EVIDENCE_READ` | Read agent state and evidence |
| `MGA_TRANSACTION_INSPECT`, `MGA_RECOVERY_INSPECT`, `MGA_HORIZON_INSPECT`, `MGA_LINEAGE_INSPECT`, `MGA_FORENSIC_INSPECT`, `MGA_METRICS_READ` | MGA audit and forensic read |

### GROUP: `SUP`

For ScratchBird support personnel or site reliability engineers with
read-only diagnostic access. A subset of `AUD`.

Rights conveyed:

| Right | Purpose |
|-------|---------|
| `OBS_RUNTIME_ALL` | Read all session runtime state |
| `OBS_METRICS_READ_ALL`, `OBS_METRICS_READ_FAMILY`, `OBS_METRICS_READ_DATABASE`, `OBS_METRICS_READ_NODE`, `OBS_METRICS_READ_CLUSTER`, `OBS_METRICS_EXPORT_READ` | Full metrics read |
| `OBS_MANAGEMENT_INSPECT` | Read management surface |
| `OBS_CLUSTER_HEALTH_INSPECT` | Read cluster health |
| `SUPPORT_EXPORT` | Generate support bundles |
| `OBS_AGENT_STATE_READ` | Read agent state |
| `MGA_TRANSACTION_INSPECT`, `MGA_RECOVERY_INSPECT`, `MGA_HORIZON_INSPECT`, `MGA_LINEAGE_INSPECT`, `MGA_METRICS_READ` | MGA diagnostics read |

`SUP` has fewer rights than `OPS`: it cannot control anything, cannot alter
configuration, and cannot manage backup operations.

### GROUP: `OPS`

For operations principals who manage the running database. `OPS` conveys broad
operational control including backup, cluster, configuration, and agent
management.

Rights conveyed:

| Right | Purpose |
|-------|---------|
| `OBS_RUNTIME_ALL` | Read all session runtime state |
| `OBS_METRICS_READ_ALL`, `OBS_METRICS_READ_FAMILY`, `OBS_METRICS_READ_DATABASE`, `OBS_METRICS_READ_NODE`, `OBS_METRICS_READ_CLUSTER`, `OBS_METRICS_EXPORT_READ` | Full metrics read |
| `OBS_MANAGEMENT_INSPECT`, `OBS_MANAGEMENT_CONTROL` | Read and control management surface |
| `OBS_CONFIG_INSPECT`, `OBS_CONFIG_CONTROL` | Read and control configuration |
| `OBS_CLUSTER_HEALTH_INSPECT`, `OBS_CLUSTER_CONTROL` | Read and control cluster |
| `OBS_DATA_MOVEMENT_INSPECT` | Inspect data movement |
| `OBS_METRICS_EXPORT`, `OBS_METRICS_EXPORT_CONTROL` | Export metrics and control export settings |
| `SUPPORT_EXPORT` | Generate support bundles |
| `BACKUP_CREATE`, `BACKUP_RESTORE`, `BACKUP_CONTROL`, `BACKUP_INSPECT` | Full backup lifecycle |
| `OBS_AGENT_STATE_READ`, `OBS_AGENT_EVIDENCE_READ`, `OBS_AGENT_CONTROL`, `OBS_AGENT_POLICY_CONTROL`, `OBS_AGENT_OVERRIDE` | Full agent management |
| `MANAGER_ADMISSION_ADMIN` | Administer manager admission policy |
| `MGA_TRANSACTION_INSPECT`, `MGA_RECOVERY_INSPECT`, `MGA_CLEANUP_INSPECT`, `MGA_CLEANUP_CONTROL`, `MGA_HORIZON_INSPECT`, `MGA_LINEAGE_INSPECT`, `MGA_METRICS_READ` | MGA operations and cleanup control |

### GROUP: `SEC`

For security administration principals. `SEC` conveys identity, grant, and
policy management but not broad data access.

Rights conveyed:

| Right | Purpose |
|-------|---------|
| `SEC_IDENTITY_ADMIN` | Create, alter, and drop principals |
| `SEC_MEMBERSHIP_ADMIN` | Manage role and group membership |
| `SEC_GRANT_ADMIN` | Issue and revoke grants |
| `POLICY_ADMIN` | Administer security policies |
| `AUDIT_READ` | Read audit records |
| `AUDIT_ADMIN` | Administer audit policy |
| `AUTH_PROVIDER_ADMIN` | Administer authentication providers |
| `UDR_TRUST_ADMIN` | Administer UDR trust policy |
| `MANAGER_ADMISSION_ADMIN` | Administer manager admission policy |
| `UDR_MANAGE`, `UDR_INSPECT`, `UDR_INVOKE` | UDR lifecycle |
| `BACKUP_CREATE`, `BACKUP_RESTORE`, `BACKUP_CONTROL`, `BACKUP_INSPECT` | Full backup lifecycle |
| `PROTECTED_MATERIAL_RELEASE`, `KEY_RELEASE_APPROVE` | Protected material release and key approval |
| `EVENT_ADMIN`, `EVENT_CREATE`, `EVENT_ALTER`, `EVENT_DROP`, `EVENT_SUBSCRIBE`, `EVENT_PUBLISH`, `EVENT_DELIVERY_READ`, `EVENT_DELIVERY_ACK` | Full event lifecycle |
| `SUPPORT_EXPORT` | Generate support bundles |
| `OBS_CONFIG_INSPECT`, `OBS_MANAGEMENT_INSPECT` | Read configuration and management |
| `OBS_METRICS_POLICY_INSPECT`, `OBS_METRICS_POLICY_CONTROL` | Metrics policy administration |
| `SEC_AUTH_METRICS_READ`, `OBS_POLICY_READ` | Security and policy read |
| `OBS_AGENT_EVIDENCE_READ`, `OBS_AGENT_POLICY_CONTROL`, `OBS_AGENT_OVERRIDE` | Agent security evidence and policy |
| `MGA_LINEAGE_INSPECT`, `MGA_FORENSIC_INSPECT`, `MGA_METRICS_READ` | MGA forensic read |

### GROUP: `ROOT`

`ROOT` is the all-rights group. A principal in `ROOT` is allowed any known
right. This is implemented in `GroupAllows` as a blanket `return true`.

Membership in `ROOT` must be treated as equivalent to full database
superuser access. It bypasses all group-level and role-level right checks.
Assign principals to `ROOT` only through deliberate, audited, and documented
processes. The engine does not prevent membership assignment from principals
holding `SEC_MEMBERSHIP_ADMIN`, so organizational controls are the only
guard.

## Standard Roles

The 5 seeded roles are seeded with display names but their privilege grants are
materialized from the durable grant table. The rights shown here reflect the
trace-tag fallback logic in `security_model.cpp`
(`SecurityContextHasRight`) and should be confirmed against the durable
grant records in a running database.

### ROLE: `ROLE_APP_RUNTIME`

Intended for application service accounts. No rights are conveyed by the role
record alone in the seeded state; explicit grants must be issued to this role
for the object classes the application needs.

### ROLE: `ROLE_DBA`

Intended as a broad DBA role. No rights are conveyed by the role record alone
in the seeded state. Operators typically grant this role the rights that match
the `DBA` group or a narrower subset.

### ROLE: `ROLE_SECURITY_ADMIN`

The trace-tag fallback in `security_model.cpp` maps this role to
`SEC_IDENTITY_ADMIN`, `SEC_MEMBERSHIP_ADMIN`, `SEC_GRANT_ADMIN`, and
`POLICY_ADMIN`. In a fully bootstrapped database, corresponding durable grants
should be present. Do not activate this role for principals that do not require
identity and grant administration authority.

### ROLE: `ROLE_AUDIT_READER`

The trace-tag fallback maps this role to `AUDIT_READ`, `MGA_LINEAGE_INSPECT`,
and `MGA_FORENSIC_INSPECT`. In a fully bootstrapped database, corresponding
durable grants should be present.

### ROLE: `ROLE_OPERATOR`

The trace-tag fallback maps this role to `OBS_MANAGEMENT_CONTROL`,
`OBS_CONFIG_CONTROL`, `OBS_CLUSTER_CONTROL`, and `MGA_CLEANUP_CONTROL`. In a
fully bootstrapped database, corresponding durable grants should be present.

## Standard Policies

The 10 seeded policy names are:

| Policy Name | Purpose (from source) |
|-------------|----------------------|
| `revoke_all_default` | Revoke-all default behavior policy |
| `bootstrap_handoff` | Bootstrap-to-runtime authority handoff |
| `external_group_sync` | External provider group synchronization |
| `stale_security_context` | Stale-context handling policy |
| `observability_control_baseline` | Baseline observability control policy |
| `audit_evidence_required` | Audit-before-success policy |
| `protected_material_purpose_bound` | Purpose-bound protected material release policy |
| `udr_trust` | UDR trust policy |
| `manager_admission` | Manager admission policy |
| `cluster_security_fail_closed` | Cluster security fail-closed policy |

Policy content is established at creation time. Policies seeded here exist as
catalog objects that other configuration can reference by name.

## External Group Synchronization

Source: `external_group_api.cpp` — `EngineSyncExternalGroups`,
`EngineExplainMembership`.

A group with a non-empty `external_authority_ref` field in its
`EngineSecurityGroupRecord` is linked to an identity provider group. The
`external_group_sync` policy governs how provider-supplied group claims are
materialized into the engine's authorization context.

Synchronization requires:

1. A known authentication provider family that supports `supports_group_query`
   or `supports_authz_claims`. Verified families with group query support:
   `ldap_ad` (full transitive group expansion and membership path explain),
   `remote_security_database`, `cluster_security`.
2. An `external_group` name and an `internal_group_uuid` passed via
   `EngineSyncExternalGroupsRequest`.
3. The `cluster_security` provider requires `cluster_authority_available` to
   be true; if the cluster path is absent, the call fails with
   `PROCESS.CLUSTER_PATH_ABSENT`.

Materialized external group membership flows into the
`EngineMaterializedAuthorizationContext` via `effective_subjects`, which means
rights granted to the internal group become available to the principal
whose claim matched the external group name.

`EngineExplainMembership` returns a human-readable explanation of the
membership path. This is supported only for providers with
`supports_membership_path_explain` (currently verified: `ldap_ad`) or when
`synchronized_graph_evidence` is set to true.

## Extending the Standard Bundles Safely

The standard groups and roles are starting points, not mandates. Safe
extension patterns:

1. **Create narrower groups** for specific application tiers rather than adding
   principals directly to `OPS`, `SEC`, or `ROOT`. This limits blast radius if
   a principal is compromised.

2. **Grant minimum necessary rights** to custom roles. The `KnownRights()` set
   in `security_model.cpp` is the authoritative enumeration; only rights in
   that set are accepted by `IsKnownSecurityRight`.

3. **Do not remove the standard policies** (the 10 seeded policy names).
   Removing `audit_evidence_required`, `protected_material_purpose_bound`, or
   `cluster_security_fail_closed` disables important safety behaviors.

4. **Separate DBA and SEC membership**. Principals in `SEC` can alter grants
   and identities but have limited data access. Principals in `DBA` have broad
   data access but cannot alter security policy. Keeping them separate
   implements the least-privilege and separation-of-duties goals.

5. **Audit `ROOT` membership** regularly. `ROOT` membership bypasses all
   group-level and role-level right checks and must be treated as a superuser
   assignment.

## Invariants

- Visibility is the intersection of MGA visibility and materialized security
  policy. A seeded group conveys no rights unless the object-level grants also
  admit the operation.
- The system is fail-closed. A principal in a named group still fails
  authorization if the object-level grant is absent or an explicit deny is
  present.
- Seeded group and role names are display identifiers. Durable identity is
  always the UUID recorded at seeding time.

## Related Pages

- [security_model_overview.md](#ch-security-guide-security-model-overview-md) — three-layer model
  and epoch machinery
- [system_management_rights.md](#ch-security-guide-system-management-rights-md) — OBS_* and
  authority right taxonomy
- [grants_and_privileges.md](#ch-security-guide-grants-and-privileges-md) — GRANT/REVOKE model
- Language Reference: Security and Privilege Statements (SBsql Language Reference — Syntax, page XXX)
- Language Reference: Security and Sandboxing (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Security_Guide/system_management_rights.md -->

<a id="ch-security-guide-system-management-rights-md"></a>

# System Management Rights

## Purpose

This page maps the operator-visible administrative tasks — backup, database and
filespace management, agent control, configuration, management surface, metrics
and observability, policy administration, and cluster operations — to the right
identifiers that the engine checks when those tasks are requested.

This is a **draft**. No claims herein constitute a production security
certification or a promise of external audit compliance.

Source: `src/engine/internal_api/security/security_model.cpp` —
`KnownRights()`, `GroupAllows()`. Backup-specific right checks verified in
`src/engine/internal_api/backup_archive/backup_archive_api.cpp`.
Agent-specific rights verified in `src/core/agents/`.

## Definitions

**Right** — a string identifier that the engine tests via
`SecurityContextHasRight`. Only identifiers present in `KnownRights()` in
`security_model.cpp` are accepted; unknown rights are refused with
`SECURITY.RIGHT.UNKNOWN`.

**OBS_* rights** — the observability and system management authority taxonomy.
These rights are NOT SQL object privileges. They are system-level rights checked
inside engine subsystems (observability, agent runtime, cluster management,
configuration).

**SEC_* rights** — identity, membership, and grant administration rights that
authorize mutations to principals, roles, groups, and the grant table.

**BACKUP_* rights** — backup and restore rights checked in the backup archive
and storage management subsystems.

**MGA_* rights** — multi-generation archive transaction and recovery diagnostic
rights checked in the MGA subsystem.

## Right Taxonomy Overview

```
System management rights
├── Backup and recovery
│   ├── BACKUP_CREATE
│   ├── BACKUP_RESTORE
│   ├── BACKUP_CONTROL
│   └── BACKUP_INSPECT
├── Filespace management
│   └── FILESPACE_LIFECYCLE_CONTROL (storage-layer, not in KnownRights)
├── Agent management
│   ├── OBS_AGENT_STATE_READ
│   ├── OBS_AGENT_EVIDENCE_READ
│   ├── OBS_AGENT_CONTROL
│   ├── OBS_AGENT_POLICY_CONTROL
│   └── OBS_AGENT_OVERRIDE
├── Configuration management
│   ├── OBS_CONFIG_INSPECT
│   └── OBS_CONFIG_CONTROL
├── Management surface
│   ├── OBS_MANAGEMENT_INSPECT
│   └── OBS_MANAGEMENT_CONTROL
├── Metrics and observability
│   ├── OBS_METRICS_READ_SELF
│   ├── OBS_METRICS_READ_ALL
│   ├── OBS_METRICS_READ_FAMILY
│   ├── OBS_METRICS_READ_DATABASE
│   ├── OBS_METRICS_READ_NODE
│   ├── OBS_METRICS_READ_CLUSTER
│   ├── OBS_METRICS_EXPORT
│   ├── OBS_METRICS_EXPORT_READ
│   ├── OBS_METRICS_EXPORT_CONTROL
│   ├── OBS_METRICS_POLICY_INSPECT
│   ├── OBS_METRICS_RETENTION_CONTROL
│   └── OBS_METRICS_POLICY_CONTROL
├── Policy administration
│   └── OBS_POLICY_READ
├── Runtime and session
│   ├── OBS_RUNTIME_SELF
│   └── OBS_RUNTIME_ALL
├── Cluster (provider-gated)
│   ├── OBS_CLUSTER_HEALTH_INSPECT
│   └── OBS_CLUSTER_CONTROL
├── Data movement
│   └── OBS_DATA_MOVEMENT_INSPECT
├── Index diagnostics
│   └── OBS_INDEX_PROFILE_READ
├── Security administration
│   ├── SEC_IDENTITY_ADMIN
│   ├── SEC_MEMBERSHIP_ADMIN
│   ├── SEC_GRANT_ADMIN
│   ├── SEC_AUTH_METRICS_READ
│   └── SEC_REDACTION_POLICY_EDIT (source verified, not in KnownRights)
├── MGA diagnostics and control
│   ├── MGA_TRANSACTION_INSPECT
│   ├── MGA_RECOVERY_INSPECT
│   ├── MGA_CLEANUP_INSPECT
│   ├── MGA_CLEANUP_CONTROL
│   ├── MGA_HORIZON_INSPECT
│   ├── MGA_LINEAGE_INSPECT
│   ├── MGA_FORENSIC_INSPECT
│   └── MGA_METRICS_READ
└── Support and protected material
    ├── SUPPORT_EXPORT
    ├── PROTECTED_MATERIAL_RELEASE
    └── KEY_RELEASE_APPROVE
```

## Backup and Recovery Rights

Checked in: `backup_archive_api.cpp`, `storage_management_api.cpp`,
`security_model.cpp`.

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Create a logical or physical backup | `BACKUP_CREATE` | Required to initiate any backup |
| Restore from a backup | `BACKUP_RESTORE` | Required to initiate any restore |
| Control backup scheduling and retention | `BACKUP_CONTROL` | Supersedes `BACKUP_CREATE` and `BACKUP_RESTORE` in admission checks |
| Inspect backup catalog and coverage | `BACKUP_INSPECT` | Read-only; does not permit create or restore |
| Any backup administrative operation | `SYS_BACKUP` | Superuser-equivalent backup right; not in `KnownRights()` — do not use in grants |

Source excerpt (admission logic from `backup_archive_api.cpp`):
```
// Create admission: BACKUP_CREATE || BACKUP_CONTROL || SYS_BACKUP
// Restore admission: BACKUP_RESTORE || BACKUP_CONTROL || SYS_BACKUP
```

`BACKUP_CONTROL` is the canonical operator backup right. A principal holding
`BACKUP_CONTROL` passes both create and restore admission checks. `BACKUP_CREATE`
and `BACKUP_RESTORE` are narrower alternatives for separation of duties.

Note: `FILESPACE_LIFECYCLE_CONTROL` is used in the storage management layer for
filespace create, drop, attach, detach, archive, and tier operations. It is
checked via `SecurityContextHasRight` in `storage_management_api.cpp` but does
not appear in `KnownRights()` at the security model layer. This right should be
treated as an internal storage-layer right; do not attempt to grant it through
the standard grant surface.

## Agent Control Rights

Checked in: `src/core/agents/` (agent action dispatch, approval, safety,
governance files).

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Read agent state and health | `OBS_AGENT_STATE_READ` | Read-only; no control |
| Read agent evidence and traces | `OBS_AGENT_EVIDENCE_READ` | Audit and diagnostic read |
| Send control signals to agents | `OBS_AGENT_CONTROL` | Start, stop, pause, and resume admitted agents |
| Approve agent actions requiring approval | `OBS_AGENT_ACTION_APPROVE` | Verified in agents directory; not in `KnownRights()` — may be checked at the agent dispatch layer before the security model layer |
| Cancel agent actions | `OBS_AGENT_ACTION_CANCEL` | Verified in agents directory; not in `KnownRights()` |
| Override agent safety limits | `OBS_AGENT_OVERRIDE` | Use only for controlled operational override |
| Administer agent execution policy | `OBS_AGENT_POLICY_CONTROL` | Policy and governance for agent execution |
| Read agent recommendations | `OBS_AGENT_RECOMMENDATION_READ` | Verified in agents directory; not in `KnownRights()` |
| Read agent audit events | `OBS_AGENT_AUDIT_READ` | Verified in agents directory; not in `KnownRights()` |
| Internal agent authority | `OBS_AGENT_INTERNAL` | Verified in agents directory; not in `KnownRights()` — reserved for engine-internal use |

Rights verified in `KnownRights()` and available for standard grants:
`OBS_AGENT_STATE_READ`, `OBS_AGENT_EVIDENCE_READ`, `OBS_AGENT_CONTROL`,
`OBS_AGENT_POLICY_CONTROL`, `OBS_AGENT_OVERRIDE`.

Rights found in `src/core/agents/` sources but not in `KnownRights()`:
`OBS_AGENT_ACTION_APPROVE`, `OBS_AGENT_ACTION_CANCEL`,
`OBS_AGENT_RECOMMENDATION_READ`, `OBS_AGENT_AUDIT_READ`,
`OBS_AGENT_INTERNAL`. These may be checked at the agent dispatch layer
independently of the `KnownRights()` set; they should not be used in standard
`GRANT` statements.

## Configuration Management Rights

Checked in: storage management, observability, and configuration subsystems.

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Read current configuration | `OBS_CONFIG_INSPECT` | View configuration state; does not allow mutation |
| Apply configuration changes | `OBS_CONFIG_CONTROL` | Allows configuration mutations; requires audit trail |

`OBS_CONFIG_CONTROL` is a significant operational right. Configuration changes
can affect all sessions on the node. Assign it only to principals in `OPS` or
`ROLE_OPERATOR`.

## Management Surface Rights

The management surface covers session management, connection admission, and
operator command dispatch.

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Read management surface state | `OBS_MANAGEMENT_INSPECT` | View sessions, admission state, and surface metadata |
| Control sessions and management commands | `OBS_MANAGEMENT_CONTROL` | Terminate sessions, apply admission changes, dispatch operator commands |
| Administer manager admission policy | `MANAGER_ADMISSION_ADMIN` | Administer the manager admission rules |

Note: `OBS_SESSION_ALL` appears in `manager_control.cpp` as an alias that is
mapped to `OBS_RUNTIME_ALL`. Do not use `OBS_SESSION_ALL` in grant statements;
use `OBS_RUNTIME_ALL` instead.

## Metrics and Observability Rights

ScratchBird has a granular metrics hierarchy. Read rights are scoped by
breadth; export and policy rights control the metrics pipeline.

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Read own session metrics | `OBS_METRICS_READ_SELF` | Minimal; safe for application accounts |
| Read metrics for a specific family | `OBS_METRICS_READ_FAMILY` | Scoped to a metric family |
| Read metrics for a specific database | `OBS_METRICS_READ_DATABASE` | Scoped to one database |
| Read metrics for a specific node | `OBS_METRICS_READ_NODE` | Node-wide read |
| Read cluster-level metrics | `OBS_METRICS_READ_CLUSTER` | Requires cluster path |
| Read all metrics | `OBS_METRICS_READ_ALL` | Broad read; for `OPS`, `AUD`, `DBA` groups |
| Read authentication provider metrics | `SEC_AUTH_METRICS_READ` | Security-sensitive; scoped to auth subsystem |
| Export metrics to external sinks | `OBS_METRICS_EXPORT` | Allows metrics export pipeline |
| Read export configuration and output | `OBS_METRICS_EXPORT_READ` | Read-only export pipeline visibility |
| Control metrics export configuration | `OBS_METRICS_EXPORT_CONTROL` | Administer export destinations and format |
| Inspect metrics policy | `OBS_METRICS_POLICY_INSPECT` | Read metrics policy settings |
| Control metrics retention policy | `OBS_METRICS_RETENTION_CONTROL` | Set retention periods and purge schedules |
| Administer metrics policy | `OBS_METRICS_POLICY_CONTROL` | Full policy write for metrics pipeline |

## Policy Read Right

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Read policy objects | `OBS_POLICY_READ` | Read durable policy catalog entries |

Additional policy lifecycle rights — `OBS_POLICY_APPLY`, `OBS_POLICY_APPROVE`,
`OBS_POLICY_DELETE`, `OBS_POLICY_EDIT_DRAFT`, `OBS_POLICY_ROLLBACK`,
`OBS_POLICY_SIMULATE`, `OBS_POLICY_VALIDATE`, `OBS_POLICY_READ_BODY` — are
verified in `src/core/agents/` sources but are not in `KnownRights()`. They
may be enforced at the agent dispatch layer. Do not use them in standard
`GRANT` statements.

## Runtime and Session Rights

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Read own session runtime state | `OBS_RUNTIME_SELF` | Minimal; safe for all attached sessions |
| Read all session runtime state | `OBS_RUNTIME_ALL` | Broad; for `OPS`, `AUD`, `DBA` groups |

## Cluster Rights (Provider-Gated)

Cluster rights require a cluster authority path. Operations that check
`require_cluster_authority = true` in `EngineAuthorizeRequest` will fail if the
cluster path is absent, even if the right is granted.

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Read cluster health and status | `OBS_CLUSTER_HEALTH_INSPECT` | Available without full cluster control |
| Inspect cluster topology | `OBS_CLUSTER_TOPOLOGY_INSPECT` | Verified in agents directory; not in `KnownRights()` |
| Apply cluster control operations | `OBS_CLUSTER_CONTROL` | Full cluster command dispatch; requires cluster authority |

`OBS_CLUSTER_CONTROL` is gated on cluster authority availability. Granting it
to a principal on a standalone (non-cluster) node has no practical effect but
does not cause an error.

## Data Movement Inspection

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Inspect data movement operations | `OBS_DATA_MOVEMENT_INSPECT` | Read-only; covers replication, migration, and ETL pipelines |

## Index Profile Diagnostics

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Read index profiles | `OBS_INDEX_PROFILE_READ` | Development diagnostic; advisory warning when granted to `DEV` group members |

## Security Administration Rights

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Create, alter, disable principals | `SEC_IDENTITY_ADMIN` | Identity lifecycle mutations |
| Manage role and group membership | `SEC_MEMBERSHIP_ADMIN` | Add or remove membership edges |
| Issue and revoke grants | `SEC_GRANT_ADMIN` | Required to call `EngineGrantRight` and `EngineRevokeRight` |
| Administer authentication providers | `AUTH_PROVIDER_ADMIN` | Add, alter, and remove provider registrations |
| Read authentication provider metrics | `SEC_AUTH_METRICS_READ` | Security-sensitive metrics |
| Administer UDR trust policy | `UDR_TRUST_ADMIN` | Control which UDR modules are trusted |
| Administer manager admission | `MANAGER_ADMISSION_ADMIN` | Control manager admission rules |

`SEC_GRANT_ADMIN` is the most consequential security administration right; a
principal holding it can expand or contract any principal's privileges.

## MGA Transaction and Recovery Rights

The MGA (multi-generation archive) subsystem has its own right taxonomy for
transaction inspection, recovery diagnostics, and cleanup control.

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Inspect transaction history | `MGA_TRANSACTION_INSPECT` | Read-only transaction log inspection |
| Inspect recovery state | `MGA_RECOVERY_INSPECT` | Read recovery log and state |
| Inspect cleanup horizon | `MGA_CLEANUP_INSPECT` | Read GC horizon and cleanup metadata |
| Apply cleanup operations | `MGA_CLEANUP_CONTROL` | Trigger GC and cleanup within policy |
| Inspect history horizon | `MGA_HORIZON_INSPECT` | Read version horizon and snapshot state |
| Inspect transaction lineage | `MGA_LINEAGE_INSPECT` | Read causal transaction chain |
| Read forensic transaction data | `MGA_FORENSIC_INSPECT` | Restricted forensic read for audit investigations |
| Read MGA metrics | `MGA_METRICS_READ` | MGA subsystem performance metrics |

## Support and Protected Material Rights

| Operator Task | Required Right(s) | Notes |
|---------------|------------------|-------|
| Generate support bundles | `SUPPORT_EXPORT` | Support bundle includes redacted diagnostics only |
| Release protected material for a purpose | `PROTECTED_MATERIAL_RELEASE` | Requires matching release policy to also be satisfied |
| Approve encryption key release | `KEY_RELEASE_APPROVE` | Key management approval step |
| Read audit records | `AUDIT_READ` | General audit record read |
| Administer audit policy | `AUDIT_ADMIN` | Audit policy lifecycle |

## Consolidated Operator Task Reference

| Operator Task | Minimum Required Right(s) |
|---------------|--------------------------|
| Create backup | `BACKUP_CREATE` or `BACKUP_CONTROL` |
| Restore backup | `BACKUP_RESTORE` or `BACKUP_CONTROL` |
| Inspect backup catalog | `BACKUP_INSPECT` |
| Full backup administration | `BACKUP_CONTROL` |
| Read agent state | `OBS_AGENT_STATE_READ` |
| Control agents | `OBS_AGENT_CONTROL` |
| Override agent safety | `OBS_AGENT_OVERRIDE` |
| Administer agent policy | `OBS_AGENT_POLICY_CONTROL` |
| Read configuration | `OBS_CONFIG_INSPECT` |
| Apply configuration | `OBS_CONFIG_CONTROL` |
| Read management surface | `OBS_MANAGEMENT_INSPECT` |
| Control management surface | `OBS_MANAGEMENT_CONTROL` |
| Read own metrics | `OBS_METRICS_READ_SELF` |
| Read all metrics | `OBS_METRICS_READ_ALL` |
| Export metrics | `OBS_METRICS_EXPORT` |
| Read cluster health | `OBS_CLUSTER_HEALTH_INSPECT` |
| Apply cluster control | `OBS_CLUSTER_CONTROL` (+ cluster authority) |
| Inspect data movement | `OBS_DATA_MOVEMENT_INSPECT` |
| Manage identities | `SEC_IDENTITY_ADMIN` |
| Manage grants | `SEC_GRANT_ADMIN` |
| Manage auth providers | `AUTH_PROVIDER_ADMIN` |
| Inspect MGA transactions | `MGA_TRANSACTION_INSPECT` |
| Apply MGA cleanup | `MGA_CLEANUP_CONTROL` |
| Generate support bundle | `SUPPORT_EXPORT` |
| Approve key release | `KEY_RELEASE_APPROVE` |

## Rights Not in KnownRights()

The following right identifiers appear in source code outside
`security_model.cpp` but are absent from `KnownRights()`:

| Identifier | Location | Status |
|-----------|----------|--------|
| `FILESPACE_LIFECYCLE_CONTROL` | `storage_management_api.cpp` | Storage-layer check; not grantable via standard GRANT |
| `SYS_BACKUP` | `backup_archive_api.cpp` | Backup superuser; internal use |
| `OBS_AGENT_ACTION_APPROVE` | `src/core/agents/` | Agent dispatch layer |
| `OBS_AGENT_ACTION_CANCEL` | `src/core/agents/` | Agent dispatch layer |
| `OBS_AGENT_RECOMMENDATION_READ` | `src/core/agents/` | Agent dispatch layer |
| `OBS_AGENT_AUDIT_READ` | `src/core/agents/` | Agent dispatch layer |
| `OBS_AGENT_INTERNAL` | `src/core/agents/` | Internal engine use |
| `OBS_CLUSTER_TOPOLOGY_INSPECT` | `src/core/agents/` | Agent dispatch layer |
| `OBS_POLICY_APPLY`, `OBS_POLICY_APPROVE`, `OBS_POLICY_DELETE`, `OBS_POLICY_EDIT_DRAFT`, `OBS_POLICY_ROLLBACK`, `OBS_POLICY_SIMULATE`, `OBS_POLICY_VALIDATE`, `OBS_POLICY_READ_BODY` | `src/core/agents/` | Agent dispatch layer |
| `OBS_SUPPORT_BUNDLE_READ` | `src/core/agents/` | Agent dispatch layer |
| `SEC_REDACTION_POLICY_EDIT` | `auth_provider_plugin_api.cpp` | Plugin API internal |
| `SEC_EXPORT_POLICY_APPROVE` | `auth_provider_plugin_api.cpp` | Plugin API internal |

Do not attempt to grant these identifiers through the standard `GRANT`
statement; the `IsKnownSecurityRight` check in `grant_api.cpp` will refuse the
operation for any right not in `KnownRights()`.

## Invariants

- Only rights in `KnownRights()` are accepted by `IsKnownSecurityRight`. Grants
  referencing unknown rights are refused with `SECURITY.RIGHT.UNKNOWN`.
- The system is fail-closed. The default authorization decision is `"deny"`.
  A right must have an active matching grant record; absence of a grant is not
  permission.
- Cluster rights require cluster authority path availability even when the
  right is granted.
- `OBS_SESSION_ALL` is an alias for `OBS_RUNTIME_ALL` in the manager control
  layer. Use `OBS_RUNTIME_ALL` in grant statements.

## Related Pages

- [standard_roles_and_groups.md](#ch-security-guide-standard-roles-and-groups-md) — which groups
  and roles convey which rights
- [grants_and_privileges.md](#ch-security-guide-grants-and-privileges-md) — how rights are
  granted and evaluated
- [security_model_overview.md](#ch-security-guide-security-model-overview-md) — authorization
  decision model
- Language Reference: Security and Sandboxing (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Security_Guide/grants_and_privileges.md -->

<a id="ch-security-guide-grants-and-privileges-md"></a>

# Grants and Privileges

## Purpose

This page explains the grant and revoke model in ScratchBird: how privileges
are granted to principals, how the grant table is materialized into an
authorization context, how explicit denials work, and how definer versus
invoker context affects privilege evaluation.

This is a **draft**. No claims herein constitute a production security
certification or a promise of external audit compliance.

Source: `src/engine/internal_api/security/grant_api.hpp`,
`grant_api.cpp`, `authorization_api.hpp`, `security_model.hpp`,
`security_model.cpp`, `security_principal_lifecycle.hpp`.

For syntax reference, see:
- Language Reference: Security and Privilege Statements (SBsql Language Reference — Syntax, page XXX)

That page documents the full SBsql `GRANT` and `REVOKE` syntax. This page
covers the underlying engine mechanics.

## Definitions

**Privilege** — a right granted to a grantee for a specific target object.
Stored as an `EngineSecurityPrivilegeGrantRecord` (or
`DurableAuthorizationGrantRecord` in the materialized model). The `privilege`
field carries the right name (must be in `KnownRights()`); the `grant_effect`
field defaults to `"allow"` but can be `"deny"` for explicit denials.

**Grantee** — the subject of the grant. The `grantee_kind` field on
`EngineSecurityPrivilegeGrantRecord` defaults to `"principal"`. Roles and
groups also accept grants; the `DurableAuthorizationGrantRecord` carries a
`subject_kind` field that can be `"principal"`, `"role"`, or `"group"`.

**Grant option** — the ability to re-grant a privilege. Expressed as `WITH
GRANT OPTION` in SBsql. Not separately tracked at the engine API level; the
engine checks that the grantor holds `SEC_GRANT_ADMIN`.

**Admin option** — the ability to administer a role membership edge. Expressed
as `WITH ADMIN OPTION` in SBsql for role grants.

**Definer context** — a callable routine that runs with the security context of
its definer principal rather than the caller's context. The
`EngineSecurityDefinerRightsCacheRecord` caches definer-privilege evaluations.

**Invoker context** — a callable routine that runs with the caller's security
context. The default for routines unless explicitly declared otherwise.

## Grant API

Source: `grant_api.cpp`.

```cpp
// Requires SEC_GRANT_ADMIN on the target object
EngineGrantRightResult  EngineGrantRight(const EngineGrantRightRequest&);
EngineRevokeRightResult EngineRevokeRight(const EngineRevokeRightRequest&);
```

Both functions require the calling principal to hold `SEC_GRANT_ADMIN` on the
target object. If the caller lacks this right, both functions return a security
failure with `SECURITY.AUTHORIZATION.DENIED` and detail `SEC_GRANT_ADMIN`.

A security context must be present (`security_context_present = true`).
Requests without a security context are refused with
`SECURITY.AUTHENTICATION.REQUEST_INVALID`.

`EngineGrantRight` additionally validates that the right string is in
`KnownRights()`. Granting an unknown right returns
`SECURITY.AUTHORIZATION.DENIED` with detail `unknown_right:<name>`.

Advisory diagnostics:
- Granting `OBS_INDEX_PROFILE_READ` to a `DEV` group member emits a
  `SB_ENGINE_API_DEV_ONLY_RIGHT_WARNING` advisory (non-fatal).
- Granting `DOMAIN_UNMASK` to a `DEV` group member emits the same advisory.

## Grantee Model

Grants can be issued to four principal kinds, reflected in `grantee_kind` or
`subject_kind`:

| Grantee Kind | Description |
|-------------|-------------|
| `principal` | A specific user or service principal identified by UUID |
| `role` | A role object; principals holding the role inherit the grant |
| `group` | A group object; principals in the group inherit the grant |
| `public` | The `PUBLIC` pseudo-principal; all attached sessions |

Granting to a role or group causes the right to be inherited by all principals
that are currently members of that role or group, as determined at
authorization materialization time.

Granting to `PUBLIC` should be used only for rights that are genuinely
intended for every session, such as `CONNECT` on a database meant to be
publicly accessible.

## Grant Targets

A grant target binds to an object UUID, not to a display name. The target class
determines which privileges are valid. The full list of target classes accepted
in the `GRANT` surface:

| Target Class | Example Rights |
|-------------|---------------|
| `DATABASE` | `CONNECT`, `CREATE SCHEMA`, `BACKUP`, `RESTORE`, `MANAGE SECURITY` |
| `SCHEMA` | `USAGE`, `CREATE TABLE`, `CREATE FUNCTION` |
| `TABLE` | `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `TRUNCATE` |
| `COLUMN` | `SELECT`, `INSERT`, `UPDATE` |
| `VIEW` | `SELECT`, `ALTER`, `DROP` |
| `MATERIALIZED VIEW` | `SELECT`, `REFRESH` |
| `SEQUENCE` | `USAGE`, `SELECT`, `UPDATE` |
| `FUNCTION` | `EXECUTE` |
| `PROCEDURE` | `EXECUTE` |
| `TRIGGER` | `ALTER`, `DROP`, `ENABLE`, `DISABLE` |
| `DOMAIN` | `USAGE`, `ALTER`, `DROP` |
| `TYPE DESCRIPTOR` | `USAGE`, `ALTER`, `DROP` |
| `POLICY`, `MASK`, `RLS` | `APPLY`, `ALTER`, `DROP`, `ENABLE`, `DISABLE` |
| `FILESPACE` | `USAGE`, `CREATE OBJECT`, `ALTER`, `DROP` |
| `BRIDGE` | `CONNECT`, `IMPORT`, `EXPORT`, `REPLICATE` |
| `SYSTEM` | Policy-defined administrative privileges |

See Language Reference: Security and Privilege Statements (SBsql Language Reference — Syntax, page XXX) for the authoritative list of privileges per object class.

## Durable Grant Records

Source: `security_principal_lifecycle.hpp` —
`EngineSecurityPrivilegeGrantRecord`.

A durable grant record carries:

| Field | Type | Purpose |
|-------|------|---------|
| `grant_uuid` | UUID | Stable grant identity |
| `grantee_uuid` | UUID | Subject of the grant |
| `grantee_kind` | string | `"principal"`, `"role"`, or `"group"` |
| `target_object_uuid` | UUID | Object the grant applies to |
| `target_object_kind` | string | Object class (e.g., `"table"`, `"schema"`) |
| `privilege` | string | Right name |
| `grantor_principal_uuid` | UUID | Principal who issued the grant |
| `grant_effect` | string | `"allow"` (default) or `"deny"` |
| `security_generation` | uint64 | Security epoch at grant creation |
| `revoked` | bool | True when the grant has been revoked |

Revoked grants are retained in the record store for audit purposes but are
not included in materialized authorization contexts.

## Authorization Materialization

Source: `security_model.cpp` — `MaterializeDurableAuthorizationContext`,
`EvaluateMaterializedAuthorization`.

When the engine needs to authorize an operation, it calls
`MaterializeDurableAuthorizationContext` with the principal UUID and the
caller's observed epoch values. The materialization process:

1. Verifies that the principal is present and active in the durable state.
2. Walks the membership graph (principal → roles → groups) to collect all
   effective subjects, detecting and refusing cycles.
3. For each effective subject, collects all active `DurableAuthorizationGrantRecord`
   records whose right is in `KnownRights()`.
4. Collects all active `DurableAuthorizationPolicyRecord` records for effective
   subjects.
5. Validates epoch consistency: grants and policies whose `security_epoch`
   does not match the state's `security_epoch` cause `SECURITY.CONTEXT.EXPIRED`.

The result is an `EngineMaterializedAuthorizationContext` carrying:
- `effective_subjects` — the principal plus all transitively reachable active
  roles and groups
- `grants` — the collected `DurableAuthorizationGrantRecord` set
- `policies` — the collected `DurableAuthorizationPolicyRecord` set

Authorization is then evaluated by `EvaluateMaterializedAuthorization`:

1. Explicit deny check: if any grant for the required right on the target has
   `deny = true`, the decision is `"deny"` and the function returns immediately.
2. Allow check: if at least one grant for the required right on the target has
   `deny = false`, `allowed` is set to true.
3. Policy check: if a policy matching the right and target has `deny = true`,
   the decision is `"deny"`. If a policy has `requires_runtime_recheck = true`,
   the decision becomes `"allow_recheck_required"`.
4. Final decision: `"allow"` if `allowed` is true and no deny was triggered.
   `"deny"` in all other cases.

The default decision when no matching grant is found is `"deny"`. This is the
fail-closed invariant.

## Explicit Denial

Explicit denial is the mechanism by which an `allow` grant can be overridden.
A `DurableAuthorizationGrantRecord` with `deny = true` causes
`EvaluateMaterializedAuthorization` to return `"deny_explicit"` before any
allow grant is evaluated.

This means: if a principal belongs to a group that has been granted a right,
but that specific principal also has an explicit deny record for the same right
and target, the denial wins.

The SQL form is not directly shown in the LR grant page syntax, but the
`grant_effect` field in the engine records admits `"deny"`. Operators
implementing explicit denial should confirm the supported SBsql surface for
their version.

## Revoke Semantics

Source: `grant_api.cpp` — `EngineRevokeRight`.

`EngineRevokeRight` sets `revoked = true` on the matching durable grant record.
The revocation is written through `PersistApiBehaviorRecord` and is subject to
MGA transaction finality: the revocation is visible after commit and is
reversed by rollback.

Revocation advances `security_generation` and returns a
`cache_invalidation_epoch`. Callers that cache authorization results must
validate their cached epoch via `EngineSecurityValidatePolicyCache` before
continuing to use cached data. A stale cache is refused with
`SECURITY.POLICY.CACHE_STALE`.

`REVOKE GRANT OPTION FOR` removes the re-grant ability while preserving the
privilege itself. `REVOKE ADMIN OPTION FOR` removes role administration
authority while preserving membership.

`RESTRICT` refuses revocation if dependent grants would become invalid.
`CASCADE` removes dependent grants through an explicit cascade plan.

## Grant Option and Admin Option

`WITH GRANT OPTION` on a privilege grant means the grantee can re-grant the
same privilege to another principal. The engine enforces this by checking
`SEC_GRANT_ADMIN` at re-grant time — the grantee's authority to re-grant
is not broader than the grantor's effective authority.

`WITH ADMIN OPTION` on a role grant means the grantee can administer the role
membership edge (add or remove members). This requires `SEC_MEMBERSHIP_ADMIN`.

Grantable authority is never broader than the grantor's effective authority.
The engine does not allow a principal to grant a right they do not themselves
hold.

## Definer vs. Invoker Context

Source: `security_principal_lifecycle.hpp` —
`EngineSecurityDefinerRightsCacheRecord`, `EngineSecurityPrimeDefinerRightsCache`,
`EngineSecurityValidateDefinerRightsCache`.

**Invoker context** (default): The callable routine executes with the caller's
materialized authorization context. Every access inside the routine is checked
against the caller's grants and policies.

**Definer context**: The routine executes with the security context of its
declared definer principal UUID. The
`EngineSecurityDefinerRightsCacheRecord` caches the definer's effective
privileges for a target object and privilege name. The cache entry carries a
`policy_generation` and a `cache_key`; callers must validate the cache using
`EngineSecurityValidateDefinerRightsCache` before using cached definer rights.

A definer-context routine does not bypass RLS or masks unless the definer's
context explicitly admits it. The engine rechecks definer authority at each
execution.

## Privilege Resolution Order

For each protected operation, the engine resolves effective privileges in this
order:

1. Authenticate and bind the effective principal UUID.
2. Establish sandbox root, attached database, current schema, and active role
   set.
3. Resolve the target name or UUID under sandbox and metadata visibility rules.
4. Collect direct grants to the principal.
5. Collect grants inherited through active roles and admitted groups via the
   membership graph traversal.
6. Apply object ownership policy where applicable.
7. Apply object-class privilege rules.
8. Apply column, element, row-level, mask, protected-material, bridge, and
   system policies.
9. Apply explicit deny or refusal policy. **Denial wins over allow.**
10. Produce an admitted operation descriptor or a canonical refusal message
    vector.

Authorization is rechecked at execution. A prepared statement that was valid
when prepared can be refused later if security state, schema state, policy
state, or object state changed.

## Epoch and Cache Management

Source: `security_principal_lifecycle.hpp`.

Every mutation to principals, roles, groups, memberships, grants, or policies
increments `security_generation` or `policy_generation` and returns a
`cache_invalidation_epoch` in the mutation result.

Callers that cache authorization results must call
`EngineSecurityValidatePolicyCache` with their observed
`policy_generation` and `cache_invalidation_epoch` before using cached data.
If the epoch has advanced, the cache is refused with `SECURITY.POLICY.CACHE_STALE`
and must be rebuilt.

Prepared statements, query plans, metadata projections, and driver metadata
caches must also be invalidated when the security epoch advances.

## Transaction Behavior

Grant and revoke operations are catalog mutations subject to MGA transaction
finality:

| Event | Outcome |
|-------|---------|
| Grant issued, commit | Right becomes effective from the new security epoch |
| Grant issued, rollback | Right is not visible; prior state is restored |
| Revoke issued, commit | Right is removed from the new security epoch; dependent caches are invalidated |
| Revoke issued, rollback | Revocation is not visible; prior grant state is restored |
| Crash before commit | Recovery restores the pre-grant or pre-revoke state |
| Crash after commit | Recovery exposes the committed grant state |

## Diagnostic Codes

Relevant diagnostic codes for grant and privilege operations:

| Code | Meaning |
|------|---------|
| `SECURITY.ACCESS_DENIED` | General access denial |
| `SECURITY.PRIVILEGE.DEFAULT_DENY` | No matching grant; default-deny applied |
| `SECURITY.PRIVILEGE.GRANT_NOT_VISIBLE` | Grant referenced but not visible to this context |
| `SECURITY.GRANT_INVALID` | Grant is malformed or references unknown objects |
| `SECURITY.AUTHORIZATION.DENIED` | Authorization decision was "deny" |
| `SECURITY.RIGHT.UNKNOWN` | Right name not in `KnownRights()` |
| `SECURITY.POLICY.CACHE_STALE` | Cached authorization data is stale |
| `SECURITY.POLICY.STALE` | Policy epoch has advanced |
| `SECURITY.AUTHORIZATION.MEMBERSHIP_CYCLE` | Role or group membership graph contains a cycle |

## Invariants

- The default authorization decision is `"deny"`. A right must have an active
  matching grant record.
- Explicit denial wins over allow. A deny record short-circuits the allow check.
- Visibility is the intersection of MGA visibility and materialized security
  policy. A grant does not make an object visible if it is hidden by sandbox,
  metadata policy, or recovery state.
- The system is fail-closed. Missing, stale, or ambiguous authorization
  evidence refuses the operation.
- Grant mutations advance the security epoch and invalidate dependent caches,
  prepared statements, and metadata projections.

## Related Pages

- [standard_roles_and_groups.md](#ch-security-guide-standard-roles-and-groups-md) — seeded roles
  and groups that receive grants
- [system_management_rights.md](#ch-security-guide-system-management-rights-md) — OBS_* and
  system right identifiers
- [domain_and_column_security.md](#ch-security-guide-domain-and-column-security-md) — column-level
  grants, domain rights, masking
- Language Reference: Security and Privilege Statements (SBsql Language Reference — Syntax, page XXX)
- [security_model_overview.md](#ch-security-guide-security-model-overview-md) — three-layer
  authorization model




===== FILE SEPARATION =====

<!-- chapter source: Security_Guide/domain_and_column_security.md -->

<a id="ch-security-guide-domain-and-column-security-md"></a>

# Domain and Column Security

## Purpose

This page covers the security surfaces below the table level: domain-level
constraints and rights, column-level grants, mask (value-rewriting) rules,
and row-level security (RLS). It explains how these layers compose, how
explicit denial works across them, and how security-epoch invalidation
propagates when they change.

This is a **draft**. No claims herein constitute a production security
certification or a promise of external audit compliance.

Source: `src/engine/internal_api/security/deep_enforcement_api.hpp`,
`deep_enforcement_api.cpp`, `security_model.cpp` (masking in
`EngineEvaluateDeepSecurity`), `security_principal_lifecycle.hpp`
(domain rights in `KnownRights()`).

For syntax reference, see:
- Language Reference: Policy, Mask, and RLS Lifecycle (SBsql Language Reference — Syntax, page XXX)
- Language Reference: Security and Privilege Statements (SBsql Language Reference — Syntax, page XXX)
- Language Reference: Domain Lifecycle (SBsql Language Reference — Syntax, page XXX)
- Language Reference: Data Types — Domains, Casts, and Coercion (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX)

## Definitions

**Domain** — a named type descriptor that carries constraints, cast rules,
method definitions, and optionally masking and policy configuration. A column
whose type is a domain inherits the domain's security behavior where the
descriptor says it applies.

**Mask** — a durable projection rule that rewrites, redacts, hashes, truncates,
nulls, or otherwise transforms a visible column or domain value before
returning it to the caller. The stored value is unchanged.

**RLS (Row-Level Security)** — a durable filter rule that limits which rows are
visible to the caller or which rows a caller can mutate.

**Deep enforcement** — the `EngineEvaluateDeepSecurity` call that is made
inside the engine executor and storage layer for each operation. Unlike the
parser layer, deep enforcement runs after the statement has been translated to
SBLR and is executing inside the engine.

**Security epoch** — the monotonically increasing counter that tracks mutations
to security state. Policy, mask, and RLS rule changes advance the
`policy_generation` counter and return a `cache_invalidation_epoch`, which
dependent caches must validate before reuse.

## Domain-Level Rights

Source: `security_model.cpp` — `KnownRights()`, `GroupAllows()`.

Domains carry their own right taxonomy, distinct from table rights:

| Right | Purpose |
|-------|---------|
| `DOMAIN_USE` | Use the domain as a column type or cast target |
| `DOMAIN_CAST` | Apply domain cast operations |
| `DOMAIN_METHOD` | Invoke domain methods |
| `DOMAIN_POLICY_ADMIN` | Administer domain-level policy |
| `DOMAIN_UNMASK` | Unmask domain-masked values; bypasses domain mask policy |

`DOMAIN_UNMASK` deserves particular attention. A principal holding
`DOMAIN_UNMASK` for a specific domain can see the raw value of columns whose
mask is applied through that domain. Granting `DOMAIN_UNMASK` to `DEV` group
members triggers an advisory warning from `grant_api.cpp`.

`DBA` group members hold all five domain rights. No other standard group
conveys domain rights by default.

The `UNMASK` right (without the `DOMAIN_` prefix) is a broader unmasking right
checked in `deep_enforcement_api.cpp` during the masking evaluation step. A
principal holding either `UNMASK` or `DOMAIN_UNMASK` for the target passes the
mask bypass check.

## Column-Level Grants

A table-level `GRANT SELECT ON TABLE` admits access to the relation but does
not automatically expose every column. Column-level grants are independent:

```sql
-- table grant: allows targeting the relation
grant select on table app.customer to app_support;

-- column grants: allow specific column projections
grant select on column app.customer.customer_id to app_support;
grant select on column app.customer.display_name to app_support;
grant select on column app.customer.email to app_support;
```

The final column set visible to the caller is the intersection of:
- the table-level grant (confirms the caller can target the relation),
- the column-level grant for each projected column,
- domain mask policy (if the column type is a domain with a mask),
- any standalone mask rule attached to the column,
- any RLS predicate that filters or hides the row containing the column.

Omitting a column grant for a column that a mask policy relies on does not
automatically expose the raw value; the mask still applies even when the column
grant is present.

## Deep Enforcement API

Source: `deep_enforcement_api.hpp`, `deep_enforcement_api.cpp`.

`EngineEvaluateDeepSecurity` is the unified enforcement point for executor and
storage operations. It is not a parser hook; it runs inside the engine after
the parser has translated the statement to SBLR.

Request fields:

| Field | Default | Purpose |
|-------|---------|---------|
| `phase` | `"executor"` | Execution phase: `"executor"`, `"storage"`, `"mutation"`, `"udr"`, `"catalog_discovery"`, `"name_resolution"` |
| `required_right` | `"SELECT"` | Right to check |
| `mutation` | `false` | Whether this is a mutation operation |
| `require_audit_before_success` | `false` | Whether an audit event must be written before the operation can succeed |

Result fields:

| Field | Meaning |
|-------|---------|
| `admitted` | Request reached deep enforcement evaluation |
| `authorized` | Principal holds the required right for the target |
| `visible` | Object is visible to this security context |
| `masked` | Masking policy is active for this value |
| `rls_applied` | A row-level security filter is active |
| `audit_written` | An audit event was written |
| `side_effect_permitted` | Mutation or side-effect is permitted |
| `decision` | `"admitted"`, `"refused"`, or `"hidden_as_missing"` |

The `hidden_as_missing` decision is used when the phase is `"catalog_discovery"`
or `"name_resolution"` and authorization fails. In those phases, the diagnostic
code is `SECURITY.OBJECT.NOT_FOUND_OR_NOT_VISIBLE` rather than
`SECURITY.AUTHORIZATION.DENIED`. This prevents a caller from learning whether
a hidden object exists.

## Masking Evaluation

Source: `deep_enforcement_api.cpp` — masking section.

When `masking_policy` is `"mask"`, the result's `masked = true` and the column
value is transformed by the mask expression before the caller sees it.

When `masking_policy` is `"unmask"`, the engine checks whether the principal
holds `UNMASK` or `DOMAIN_UNMASK` for the target object UUID:

```cpp
result.masked = !SecurityContextHasRight(context, "UNMASK", target_uuid) &&
                !SecurityContextHasRight(context, "DOMAIN_UNMASK", target_uuid);
```

If neither right is held, the value remains masked. If either right is held,
the value is unmasked.

When `masking_policy` is `"none"` (the default), the column value is not
masked.

Masks operate on values after authorization; they do not affect whether the
row is visible.

## RLS Evaluation

Source: `deep_enforcement_api.cpp` — RLS section.

When `rls_policy` is `"filter"`, `result.rls_applied = true` and the row set is
filtered by the RLS predicate. Rows not satisfying the predicate are invisible
to the caller; their existence is not revealed.

When `rls_policy` is `"deny"`, the entire operation is refused with
`SECURITY.RLS.DENIED`. This is the explicit-deny case for RLS: the caller is
not allowed to target this rowset at all under current policy.

When `rls_policy` is `"allow"` (the default), no row-level filtering is applied.

For mutation operations, RLS applies differently by operation type:

| Mutation | RLS Role |
|---------|---------|
| `INSERT` | `WITH CHECK` expression validates the new row image |
| `UPDATE` | `USING` predicate decides whether the old row can be targeted; `WITH CHECK` validates the new image |
| `DELETE` | `USING` predicate decides whether the row can be targeted |

## Composition Rules

Multiple policies, masks, and RLS rules can apply to the same target and
operation.

| Composition Rule | Effect |
|-----------------|--------|
| Explicit deny wins | A denying rule or failed release policy refuses access even when another rule would allow it |
| Restrictive composition | The row or value must satisfy every applicable restrictive rule |
| Permissive composition | The row or value may be admitted by one permissive rule only if no stronger rule denies it |
| Object-specific rules | Table, column, domain, and protected-material rules all apply where their scopes intersect |

Source: `policy_mask_and_rls.md` (Language Reference).

When composition is ambiguous, the operation fails closed. The engine does not
infer allow behavior from missing policy rows.

## Audit Before Success

Source: `deep_enforcement_api.cpp`.

When `require_audit_before_success` is true, or when a mutation (`mutation =
true`) is detected, the engine calls `AppendSecurityEvidenceEvent` to write an
audit event before the operation is permitted. If the audit write fails, the
operation is refused.

This is the `audit_evidence_required` policy in action: certain operations must
produce audit evidence before they are allowed to succeed. If the audit pathway
is unavailable, the operation fails closed.

## Security Epoch Invalidation

Policy, mask, and RLS mutations advance `policy_generation` (and return a
`cache_invalidation_epoch`). All of the following are invalidated when the
policy epoch advances:

- prepared statements
- query plans and optimizer evidence
- parser metadata caches
- driver metadata
- catalog projections
- support-bundle manifests
- diagnostic renderers
- security snapshots
- view and materialized-view readiness where applicable

This invalidation is not optional. Stale policy is refused, not silently
applied.

Source: `security_principal_lifecycle.hpp` — `EngineSecurityValidatePolicyCache`.

## Column Security Example

```sql
-- create a table with an email column
create table app.customer (
  customer_id   uuid not null,
  display_name  text not null,
  email         text not null
);

-- grant table and column access to the support role
grant select on table app.customer to role app_support;
grant select on column app.customer.customer_id to role app_support;
grant select on column app.customer.display_name to role app_support;
grant select on column app.customer.email to role app_support;

-- mask the email column: return raw value for privileged role,
-- redacted string for everyone else
create mask app.customer_email_mask
on column app.customer.email
using case
  when has_role('app_support_private') then email
  else 'redacted'
end
to role app_support
active;
```

The `app_support` role can project the `email` column but will see `'redacted'`
unless they are also a member of `app_support_private`. The mask expression
does not alter the stored value.

## RLS Example

```sql
-- tenant-isolated row visibility
create rls app.orders_tenant_rls
on table app.orders
for select, update, delete
to role app_user
using (tenant_uuid = current_tenant_uuid())
with check (tenant_uuid = current_tenant_uuid())
as restrictive
active;
```

The `USING` predicate limits which rows are visible for read and mutation
targeting. The `WITH CHECK` predicate limits which row images are allowed for
insert and update. Rows outside the caller's tenant are invisible; their
existence is not revealed.

## Interaction with Domains

A column defined with a domain type inherits the domain's masking and policy
configuration where the domain descriptor says it applies.

```sql
create domain app.tenant_id as uuid
  -- domain carries its own visibility and casting policy
  ;

create table app.orders (
  order_id  app.tenant_id not null,
  ...
);
```

If the `app.tenant_id` domain carries a mask policy, columns of that type in
`app.orders` will be masked accordingly. The domain mask applies in addition to
any column-specific mask. If both apply, the composed result follows the
restrictive composition rule: both masks must admit the value.

## Interaction with Materialized Views

Materialized views must record whether their stored rows already include
policy-filtered data, whether refresh runs as invoker or definer, and whether a
read from the materialized view rechecks caller policy. If that state is
ambiguous, refresh or read access fails closed.

A view grant is not a grant on the underlying base table. If a caller has
`SELECT` on a view but not on the base table, and the view does not run as the
definer, access to the underlying rows is still subject to the base table's
grants, column grants, masks, and RLS.

## Invariants

- Visibility is the intersection of MGA visibility and materialized security
  policy. A mask does not expose a row that RLS hides, and RLS does not expose
  a value that a mask rewrites.
- The system is fail-closed. Missing, stale, ambiguous, or corrupted policy
  state refuses the operation.
- Explicit denial wins over allow at every composition level.
- The `hidden_as_missing` decision in catalog_discovery and name_resolution
  phases prevents callers from learning whether a hidden object exists.
- Mask and RLS mutations advance the policy epoch and invalidate all dependent
  caches.

## Related Pages

- [grants_and_privileges.md](#ch-security-guide-grants-and-privileges-md) — table-level and
  object-level grants
- [standard_roles_and_groups.md](#ch-security-guide-standard-roles-and-groups-md) — which groups
  hold domain rights
- Language Reference: Policy, Mask, and RLS Lifecycle (SBsql Language Reference — Syntax, page XXX)
- Language Reference: Security and Privilege Statements (SBsql Language Reference — Syntax, page XXX)
- Language Reference: Security and Sandboxing (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Security_Guide/protected_material.md -->

<a id="ch-security-guide-protected-material-md"></a>

# Protected Material

## Purpose

This page covers protected material: engine-held values that are treated as
secrets, credentials, encryption keys, tokens, or other release-controlled
content. Protected material is distinct from ordinary column data. The engine
does not return plaintext protected values through any ordinary query path,
diagnostic surface, or support bundle. Release requires an admitted release
route, a declared purpose, and an audit event.

This is a **draft**. No claims herein constitute a production security
certification or a promise of external audit compliance.

Source: `src/engine/internal_api/security/protected_material_api.hpp`,
`protected_material_api.cpp`, `src/core/catalog/catalog_records.hpp`,
`src/manager/node/manager_support_bundle.cpp`,
`src/listener/listener_support_bundle.cpp`,
`src/storage/page/late_payload_fetch.cpp`.

For the catalog surfaces, see:
- Language Reference: sys.security.protected_material_catalog (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX)
- Language Reference: sys.security.protected_material_version (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX)
- Language Reference: sys.security.protected_material_policy_binding (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX)
- Language Reference: sys.security.protected_material_audit (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX)

For operator-visible concepts and the administrative surface, see:
- [Operations and Administration: Identity, Security, and Policy](#ch-operations-administration-identity-security-and-policy-md)
- [Operations and Administration: Diagnostics, Message Vectors, and Support Bundles](#ch-operations-administration-diagnostics-message-vectors-and-support-bundles-md)

## Definitions

**Protected material** — a secret, credential, encryption key, token,
protected binary, protected text, or other release-controlled value that the
engine owns. The engine holds the value and governs all access to it. Ordinary
SQL queries, driver metadata requests, diagnostic renderers, and support-bundle
generators cannot return plaintext protected material.

**Protected reference** — an opaque string stored in a version record that
identifies where the secret is held or how it is wrapped. It is not the secret
itself. The engine accepts this only in forms that do not embed plaintext (see
`PlaintextEvidenceRefused` gate below).

**Envelope reference** — an opaque string that identifies wrapping, key
derivation, or envelope metadata for a protected value. Like the protected
reference, it is never stored as plaintext.

**Payload hash** — a content-integrity hash computed over a protected payload.
It is kept after the protected reference is purged, enabling integrity checks
without retaining the value.

**Key handle** — a stable opaque identifier for an admitted encryption key
entry in the in-memory cache. The handle is an HMAC-SHA-256 token derived from
the key fingerprint, database UUID, filespace UUID, key UUID, and generation
counter. It never embeds the raw key.

**Key fingerprint** — an HMAC-SHA-256 token derived from the secret evidence,
the database UUID, the key UUID, and the filespace UUID. It uniquely identifies
a key admission without revealing the key.

**Release** — the admission of a protected material value for a specific
purpose. Release requires that the caller holds `PROTECTED_MATERIAL_RELEASE`,
that the purpose matches the material's `release_purposes` allowlist, and that
an audit event is written before the result is returned.

**Legal hold** — a flag on a protected material version (`legal_hold = true`
in `ProtectedMaterialVersionCatalogRecord`) that blocks all purge operations
regardless of retention policy. Legal hold must be explicitly cleared before
purge can proceed.

**Retention** — a time-based constraint (`retention_until_epoch_millis`) on a
version. Purge is refused if the current time is before this epoch and legal
hold is not the only reason.

**Rotation** — the addition of a new version that replaces the previous active
version. The prior version's `rotation_state` transitions from `"active"` to
`"rotated"`, and the new version becomes active. After purge, the
`rotation_state` becomes `"purged"`.

**Cache TTL** — the millisecond lifetime of an admitted encryption key entry in
the in-memory protected-material cache. The default is 300000 ms (5 minutes),
declared as the field default on `EngineAdmitEncryptionKeyRequest.cache_ttl_millis`
and `EngineRotateEncryptionKeyRequest.cache_ttl_millis`.

## What Protected Material Is Not

Protected material is not an ordinary column value. It does not appear in
`SELECT` results, `SHOW` output, `DESCRIBE` output, diagnostic messages,
support bundles, or audit rows in raw form. Every result struct in the API
carries `plaintext_material_returned = false` as a stated invariant, and
every inspect or export result carries `protected_material_redacted = true`.
Any call path that would require returning plaintext is refused with
`SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED` or
`SECURITY.KEY.PLAINTEXT_REFUSED` before the value is exposed.

## Catalog Model

Source: `catalog_records.hpp` — `CatalogRecordKind` enum values 94–97.

The engine maintains four catalog record kinds for protected material:

| Record Kind | Enum Value | Struct | Purpose |
|-------------|-----------|--------|---------|
| `protected_material` | 94 | `ProtectedMaterialCatalogRecord` | Durable identity, lifecycle state, active version pointer, policy UUIDs |
| `protected_material_version` | 95 | `ProtectedMaterialVersionCatalogRecord` | Per-version references, rotation state, retention, legal hold |
| `protected_material_policy_binding` | 96 | `ProtectedMaterialPolicyBindingCatalogRecord` | Policy-to-material and policy-to-version bindings |
| `protected_material_audit_event` | 97 | `ProtectedMaterialAuditEventCatalogRecord` | Redacted, append-only audit evidence |

The four record kinds form a layered model:

```
protected_material (catalog entry)
│   protected_material_uuid
│   lifecycle_state: "active" | "retained_no_active_version"
│   active_version_uuid → points to the currently active version
│   purpose_class, storage_class
│   policy UUIDs (retention, access, release, purge, audit)
│
├── protected_material_version (one or more)
│   │   protected_material_version_uuid
│   │   version_number (monotonically increasing)
│   │   rotation_state: "active" | "rotated" | "purged"
│   │   protected_reference (cleared on purge)
│   │   envelope_reference (cleared on purge)
│   │   payload_hash (retained after purge for integrity)
│   │   retention_until_epoch_millis, legal_hold
│   │   valid_from / valid_until transaction IDs (MGA visibility)
│   │
│   └── protected_material_policy_binding (per version, per policy kind)
│
├── protected_material_policy_binding (material-level, per policy kind)
│
└── protected_material_audit_event (append-only; never deleted by purge)
```

### Catalog Entry Fields

Source: `ProtectedMaterialCatalogRecord` (catalog_records.hpp) and
`EngineProtectedMaterialCatalogEntry` (protected_material_api.hpp).

| Field | Purpose |
|-------|---------|
| `protected_material_uuid` | Stable UUID identity |
| `object_class` | Material class; defaults to `"protected_material"` |
| `owner_scope_uuid` | Owning database, schema, principal, or security scope |
| `purpose_class` | Default purpose class for release policy checks |
| `storage_class` | `"direct"`, `"wrapped"`, `"split"`, `"external_reference"`, `"derived"`, or `"redacted"` |
| `lifecycle_state` | `"active"` or `"retained_no_active_version"` |
| `active_version_uuid` | UUID of the currently active version |
| `retention_policy_uuid` | Retention policy UUID |
| `access_policy_uuid` | Metadata visibility policy UUID |
| `release_policy_uuid` | Purpose-bound release policy UUID |
| `purge_policy_uuid` | Purge/destruction policy UUID |
| `audit_policy_uuid` | Audit evidence policy UUID |
| `catalog_generation_id` | Visible catalog generation |
| `security_epoch` | Security epoch for visibility and release |

### Version Record Fields

Source: `ProtectedMaterialVersionCatalogRecord` (catalog_records.hpp).

| Field | Purpose |
|-------|---------|
| `protected_material_version_uuid` | Stable version UUID |
| `protected_material_uuid` | Parent material |
| `version_number` | Monotonically increasing per material |
| `protected_reference_hash` | Digest of the protected reference; not the reference itself |
| `protected_envelope_hash` | Digest of envelope or wrapping metadata |
| `payload_hash` | Content-integrity hash (retained after purge) |
| `storage_class` | Version-level storage class |
| `rotation_state` | `"active"`, `"rotated"`, or `"purged"` |
| `valid_from_local_transaction_id` | MGA transaction that makes the version visible |
| `valid_until_local_transaction_id` | MGA transaction that ends active visibility (0 = still active) |
| `retention_until_epoch_millis` | Earliest permitted purge time |
| `legal_hold` | If true, purge is refused regardless of retention epoch |
| `purged` | True when protected reference reachability has been removed |
| `catalog_generation_id` | Visible catalog generation |
| `security_epoch` | Security epoch |

### Policy Binding Fields

Source: `ProtectedMaterialPolicyBindingCatalogRecord` (catalog_records.hpp).

| Field | Purpose |
|-------|---------|
| `policy_uuid` | Policy object UUID |
| `policy_kind` | `"retention"`, `"access"`, `"release"`, `"purge"`, or `"audit"` |
| `diagnostic_state` | Engine-assigned diagnostic state for the binding |

Policy bindings are evaluated before every protected-material operation. A
missing required policy binding is a refusal, not permission to proceed.

### Audit Event Fields

Source: `ProtectedMaterialAuditEventCatalogRecord` (catalog_records.hpp) and
`EngineProtectedMaterialAuditEvent` (protected_material_api.hpp).

| Field | Purpose |
|-------|---------|
| `audit_event_uuid` | Stable event identity (derived by SHA-256 of event parameters) |
| `actor_uuid` | Effective principal who triggered the event |
| `event_kind` | `"create"`, `"add_version"`, `"resolve"`, `"release"`, `"purge"`, `"inspect"`, or `"support_export"` |
| `decision` | `"allow"` or `"deny"` |
| `diagnostic_code` | Diagnostic code emitted for denied events |
| `redacted_detail` | Human-readable, policy-redacted detail |
| `event_epoch_millis` | Event time |
| `local_transaction_id` | MGA transaction ID |
| `redaction_applied` | Always `true` in the engine; set at audit-append time |

Audit events are append-only. Purging a version's protected reference
reachability does not delete the audit events for that version.

## Release Policy and Purpose-Binding

Release is the operation through which a caller obtains authority to use a
protected value. It is not the same as returning plaintext; the engine returns
an opaque `release_handle` rather than the raw value.

Source: `EngineReleaseProtectedMaterial` in `protected_material_api.cpp`.

The release path:

1. Requires `PROTECTED_MATERIAL_RELEASE` right.
2. Resolves the active version visible to the caller's MGA snapshot.
3. Calls `PurposeAllowed()` to test whether the requested purpose appears in
   the version's (or material's) `release_purposes` allowlist. If the allowlist
   is empty, the purpose must match `material.purpose_class` exactly.
4. On denial, writes a `"deny"` audit event with
   `SECURITY.PROTECTED_MATERIAL.POLICY_DENIED` and returns
   `policy_denied = true` with `plaintext_material_returned = false`.
5. On admission, writes an `"allow"` audit event, then returns a
   `release_handle` (an HMAC-SHA-256 token) and sets
   `plaintext_material_returned = false`.

The release handle is a stable, purpose-bound, generation-scoped token. It
does not embed the raw protected value.

`EngineRequestProtectedMaterial` follows a similar path but operates on cache
entries by key handle rather than catalog versions. It also requires an audit
evidence event (`AppendSecurityEvidenceEvent`) to be written before the result
is returned.

## Encryption Key Admission and Cache TTL

Source: `EngineAdmitEncryptionKey`, `EngineAdmitEncryptionKeyRequest` in
`protected_material_api.hpp` and `protected_material_api.cpp`.

Encryption key admission is the process of presenting secret evidence to the
engine and receiving a key handle and fingerprint in return.

Key admission fields:

| Field | Default | Purpose |
|-------|---------|---------|
| `key_uuid` | required | UUID of the key to admit |
| `key_label` | required | Human-readable label (stored as `"redacted-key"` if missing) |
| `filespace_uuid` | required | Filespace the key belongs to |
| `secret_evidence` | required | Opaque evidence material; refused if plaintext markers detected |
| `cache_ttl_millis` | **300000** | Cache lifetime in milliseconds (5 minutes) |

The `PlaintextEvidenceRefused()` gate tests the incoming evidence against a
set of known plaintext markers including `"plaintext:"`, `"cleartext:"`,
`"password:"`, `"password="`, `"passwd="`, `"secret="`, `"private_key="`,
`"key_material="`, `"raw_key="`, and `"kms_plaintext:"`. If any match is
found, admission is refused with `SECURITY.KEY.PLAINTEXT_REFUSED`.

On successful admission, the cache entry holds:

| Field | Contents |
|-------|---------|
| `key_fingerprint` | `"fingerprint:v1:hmac-sha256:<hmac>"` over database UUID, key UUID, filespace UUID, and evidence |
| `key_handle` | `"protected-material-handle:v1:hmac-sha256:<hmac>:<generation>"` |
| `admitted_at_epoch_millis` | Admission timestamp |
| `expires_at_epoch_millis` | `admitted_at_epoch_millis + max(1, cache_ttl_millis)` |
| `active` | `true` until expired or purged |

Cache expiry is checked on every cache-touching operation via
`ExpireActiveEntriesLocked()`. Expired entries have `active = false` and
`expired = true`. They are not removed from the cache but are excluded from
active lookups.

`EngineRotateEncryptionKey` uses the same 300000 ms TTL default for the
replacement key entry.

## Version States and Rotation

Source: `EngineAddProtectedMaterialVersion` in `protected_material_api.cpp`
and `ProtectedMaterialVersionCatalogRecord.rotation_state`.

Three rotation states are valid for a version:

| State | Meaning |
|-------|---------|
| `"active"` | This is the current active version for new releases and resolution |
| `"rotated"` | A newer version has become active; this version is retained for policy and audit |
| `"purged"` | Protected reference reachability has been removed; payload_hash and audit evidence are retained |

Adding a version via `EngineAddProtectedMaterialVersion` sets the new version
to `rotation_state = "active"` and closes the previous active version by
recording `valid_until_local_transaction_id`. The material's `active_version_uuid`
pointer is updated atomically in the same persisted catalog mutation.

After purge, the engine clears `protected_reference` and `envelope_reference`
from the version record and sets `rotation_state = "purged"`. If the purged
version was the active version and no other version is available, the material's
`lifecycle_state` transitions to `"retained_no_active_version"`.

## Legal Hold and Retention

Source: `EnginePurgeProtectedMaterialVersion` in `protected_material_api.cpp`.

Purge is refused at the engine level when either of the following is true:

```
version.policy.legal_hold == true
  OR
(version.policy.retention_until_epoch_millis != 0 AND
 version.policy.retention_until_epoch_millis > now)
```

Both conditions are checked before any physical erase or reference clearing is
attempted. The refusal produces `SECURITY.PROTECTED_MATERIAL.RETENTION_REQUIRED`
and writes a `"deny"` audit event with `refused_by_retention = true`.

Physical erase (zero-overwrite of the referenced file) requires four additional
flags to be set by an authorized caller:
- `physical_erase_authorized = true`
- `physical_erase_retention_satisfied = true`
- `physical_erase_legal_hold_clear = true`
- `physical_erase_path` must be non-empty

Physical erase is performed by overwriting the file with zeros in 8192-byte
chunks, flushing, and then verifying that every byte is zero before the
operation is declared complete. Missing any of the precondition flags produces
a specific diagnostic code.

## The No-Plaintext Guarantee

Every public entry point in the protected material API carries an explicit
`plaintext_material_returned = false` invariant in its result struct. The
engine enforces this in several ways:

1. **Admission gate**: `PlaintextEvidenceRefused()` and
   `ContainsPlaintextSecretMarker()` reject inputs that embed plaintext
   markers. Admission returns only a key handle and fingerprint.

2. **Storage gate**: `ProtectedPayloadInputRefused()` rejects create and
   version-add requests whose `protected_reference`, `envelope_reference`, or
   `payload_hash` embed known plaintext markers.

3. **Result gate**: All result structs set `plaintext_material_returned = false`
   and `protected_material_redacted = true`. The engine never populates a raw
   plaintext field in a public result.

4. **Redact-on-read**: `RedactedVersion()` replaces `protected_reference` and
   `envelope_reference` with `"<protected-material-redacted>"` before any
   version is placed in a public result or diagnostic row.

5. **Diagnostic gate**: `RedactProtectedMaterialForDiagnostics()` scans any
   string passed to a diagnostic or result row and replaces it with
   `"<protected-material-redacted>"` if it contains a known protected-material
   marker (`"secret"`, `"password"`, `"credential"`, `"private_key"`,
   `"key_material"`, `"plaintext"`, `"cleartext"`, `"encryption_key"`,
   `"decryption_key"`, `"protected_material"`, `"bearer "`, `"token="`,
   `"apikey"`, `"api_key"`, `"kms_plaintext"`).

6. **Late payload gate**: `FetchLateMaterializationPayload()` in
   `late_payload_fetch.cpp` refuses to expose payload bytes for any reference
   where `reference.redaction_required` is true or where
   `reference.protected_payload` is true and
   `reference.unredacted_payload_authorized_by_security` is false. The
   diagnostic is `SB_LATE_PAYLOAD_FETCH.UNREDACTED_PROTECTED_PAYLOAD` and
   `result.fail_closed = true`.

The diagnostic code `SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED` is also
documented in `security_model_overview.md` as a global invariant:
`SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED` fires when a call path
attempted to return plaintext protected material.

## Visibility: Intersection of MGA and Policy

Protected material visibility follows the same rule as all catalog objects in
ScratchBird:

> Visibility = intersection of MGA visibility and materialized policy.

MGA visibility is determined by the transaction snapshot. A version that was
created in a transaction whose `valid_from_local_transaction_id` is beyond the
reader's snapshot is not visible. Similarly, a version whose
`valid_until_local_transaction_id` is less than or equal to the snapshot is no
longer visible.

Policy visibility is enforced by the policy binding chain. A principal who has
not been granted access through the material's `access_policy_uuid` cannot see
metadata even if the MGA snapshot would include it.

`ResolveActiveVersionLocked()` implements this by combining:
- `ReadVisibilityPoint(context)` — the observer's snapshot boundary
- `VersionVisibleAt(version, visibility_point)` — tests `purged`,
  `valid_from`, and `valid_until`

If no version passes both tests, resolution fails with
`SECURITY.PROTECTED_MATERIAL.VERSION_NOT_VISIBLE`.

## Redaction in SELECT / SHOW / DESCRIBE

The engine does not have a dedicated SELECT path for protected material in the
manner of an ordinary table. The protected-material catalog tables
(`sys.security.protected_material_catalog`, `sys.security.protected_material_version`,
etc.) are engine-authority-only records (`engine_authority = true` in their
`CatalogRecordDescriptor`) and `parser_visible = false`. Ordinary SBsql `SELECT`
statements targeting these tables go through the full deep enforcement chain.

The engine's result row helpers always call `RedactProtectedMaterialForDiagnostics()`
on any value before it enters a result row (via `AddProtectedMaterialRow()`),
and always replace `protected_reference` and `envelope_reference` with
`"<protected-material-redacted>"` via `RedactedVersion()`.

The catalog inspect function (`EngineInspectProtectedMaterialCatalog`) calls
`RedactedVersion()` on every version before adding it to the result, so even
authorized callers who hold `PROTECTED_MATERIAL_RELEASE` see only
`"<protected-material-redacted>"` in place of the live protected reference.

## Redaction in Diagnostics and Support Bundles

Two separate redaction functions operate on diagnostic text and support-bundle
content:

### Engine Diagnostic Redaction

`RedactProtectedMaterialForDiagnostics(std::string text)` (in
`protected_material_api.cpp`) is called at every point where string data from
a protected-material operation enters a diagnostic row or result. It checks
`ContainsProtectedMaterialMarker()` and replaces the entire string with
`"<protected-material-redacted>"` if any marker is found.

### Manager Support-Bundle Redaction

`RedactManagerSupportBundleText(std::string text)` (in
`manager_support_bundle.cpp`) scans lines written to support-bundle files and
replaces values following sensitive keys with `"[redacted]"`. The sensitive
keys are:

| Key |
|-----|
| `password` |
| `passwd` |
| `secret` |
| `token` |
| `private_key` |
| `credential` |
| `verifier` |
| `encryption_key` |
| `decryption_key` |
| `key_handle` |

The same function also replaces filesystem paths (any token starting with `/`)
with `"[path-redacted]"`.

The manager support bundle manifest explicitly records:

```
excluded_protected_material=password,secret,token,private_key,credential,verifier,encryption_key,decryption_key,key_handle
```

### Listener Support-Bundle Redaction

`RedactListenerSupportabilityText(std::string_view text)` (in
`listener_support_bundle.cpp`) checks for the same sensitive key set plus
`"auth"`. If any match is found, the entire value is replaced with
`"[redacted:security]"`. Local filesystem paths are replaced with
`"[path-redacted]"`. The listener support bundle JSON explicitly declares:

```json
"excluded_protected_material": ["password","secret","token","private_key",
  "credential","verifier","encryption_key","decryption_key","key_handle"]
```

Both support-bundle generators also record `"redaction_profile"` and
`"local_path_policy": "redacted"` to document that redaction was applied.

## Audit Events

Every protected-material operation that reaches its conclusion appends an audit
event via `AppendMaterialAuditLocked()`. Denied operations also write audit
events before returning the failure result.

The audit event UUID is a stable, SHA-256-derived identifier:

```
"protected-material-audit:v1:sha256:<sha256-hex>"
```

where the hash input includes: `database_uuid | protected_material_uuid |
protected_material_version_uuid | event_kind | decision | event_epoch_millis |
sequence_number`.

The `redacted_detail` field of every audit event is processed through
`RedactProtectedMaterialForDiagnostics()` before the event is appended. The
`redaction_applied` flag is always `true` in the engine.

The current engine emits audit events for these operations:

| Operation | Event Kind | Decision |
|-----------|-----------|---------|
| Create protected material | `"create"` | `"allow"` |
| Add version (rotation) | `"add_version"` | `"allow"` |
| Resolve (reference lookup) | `"resolve"` | `"allow"` or `"deny"` |
| Release (purpose-bound) | `"release"` | `"allow"` or `"deny"` |
| Purge version | `"purge"` | `"allow"` or `"deny"` |
| Inspect catalog | `"inspect"` | `"allow"` |

Audit events are never deleted by version purge. They survive the lifecycle of
the protected material they describe, subject only to the audit retention
policy.

## Required Rights

| Operation | Required Right |
|-----------|---------------|
| Admit or rotate an encryption key | `KEY_RELEASE_APPROVE` |
| Resolve, release, or inspect catalog | `PROTECTED_MATERIAL_RELEASE` |
| Create protected material or add version | `KEY_RELEASE_APPROVE` |
| Purge a version | `KEY_RELEASE_APPROVE` |
| Inspect the in-memory cache | `PROTECTED_MATERIAL_RELEASE` |
| Purge the in-memory cache | `KEY_RELEASE_APPROVE` |
| Shutdown purge (engine shutdown) | `KEY_RELEASE_APPROVE` or shutdown authority tag |
| Export/import a package | `PROTECTED_MATERIAL_RELEASE` (export), `KEY_RELEASE_APPROVE` (import) |

All operations also require passing `ValidateEngineAuthorityBoundary()`, which
refuses any request that presents a non-engine authority prefix
(`auth_authority:`, `key_authority:`, etc.) or that carries a trace tag
implying parser, driver, reference, or SQLite authority.

## Package Export and Import

The engine supports exporting and importing a protected material package in a
binary format with the identifier
`"scratchbird.protected_material.reference_package.v1"`. The package contains
catalog entries, version records, and optionally audit events, all encoded in
the same record-based binary format used for the on-disk protected material
catalog file (`.sb.protected_material_catalog`).

Export does not include plaintext values: `plaintext_material_returned = false`
and `protected_material_redacted = true` are set on the export result. The
package digest is a `"sha256:<hex>"` string that callers can verify at import
time.

Import requires `import_authorized = true` and validates every version record
against `ProtectedPayloadInputRefused()` to ensure no plaintext leaks in.

## Diagnostic Codes

| Code | Meaning |
|------|---------|
| `SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED` | A call path attempted to return or store plaintext protected material |
| `SECURITY.KEY.PLAINTEXT_REFUSED` | Encryption key admission or rotation refused plaintext evidence |
| `SECURITY.KEY.UNAVAILABLE` | No active cache entry for the requested key UUID or handle |
| `SECURITY.KEY.EXPIRED` | A cache entry for the key exists but has expired |
| `SECURITY.KEY.WRONG` | The key handle references the wrong key |
| `SECURITY.KEY.SCOPE_MISMATCH` | Key handle is not valid for the requested database or filespace |
| `SECURITY.KEY.ADMISSION_INVALID` | Admission request missing required fields |
| `SECURITY.KEY.ROTATION_INVALID` | Rotation request missing required fields |
| `SECURITY.PROTECTED_MATERIAL.AUTHORITY_BYPASS_REFUSED` | A non-engine authority prefix was detected |
| `SECURITY.PROTECTED_MATERIAL.AUTHORITY_DENIED` | Caller lacks the required right for the operation |
| `SECURITY.PROTECTED_MATERIAL.CATALOG_INVALID` | Required catalog fields absent or malformed |
| `SECURITY.PROTECTED_MATERIAL.MGA_CONTEXT_REQUIRED` | Operation requires an active MGA transaction |
| `SECURITY.PROTECTED_MATERIAL.NOT_FOUND` | Protected material UUID not found in catalog |
| `SECURITY.PROTECTED_MATERIAL.VERSION_NOT_VISIBLE` | No version visible under the current MGA snapshot and policy |
| `SECURITY.PROTECTED_MATERIAL.VERSION_NOT_FOUND` | Specific version UUID not found |
| `SECURITY.PROTECTED_MATERIAL.VERSION_DUPLICATE` | A version with this UUID already exists |
| `SECURITY.PROTECTED_MATERIAL.VERSION_INVALID` | Version request missing required fields |
| `SECURITY.PROTECTED_MATERIAL.ALREADY_EXISTS` | Protected material UUID conflicts with existing entry |
| `SECURITY.PROTECTED_MATERIAL.POLICY_DENIED` | Release refused because the requested purpose is not in the allowlist |
| `SECURITY.PROTECTED_MATERIAL.DENIED` | General protected-material denial (e.g., purpose not provided) |
| `SECURITY.PROTECTED_MATERIAL.RETENTION_REQUIRED` | Purge refused by legal hold or retention epoch |
| `SECURITY.PROTECTED_MATERIAL.PHYSICAL_ERASE_AUTHORITY_REQUIRED` | Physical erase requires engine authority flag |
| `SECURITY.PROTECTED_MATERIAL.PHYSICAL_ERASE_RETENTION_PROOF_REQUIRED` | Physical erase requires retention satisfied flag |
| `SECURITY.PROTECTED_MATERIAL.PHYSICAL_ERASE_LEGAL_HOLD_PROOF_REQUIRED` | Physical erase requires legal hold cleared flag |
| `SECURITY.PROTECTED_MATERIAL.PHYSICAL_ERASE_PATH_REQUIRED` | Physical erase path not provided |
| `SECURITY.PROTECTED_MATERIAL.PHYSICAL_ERASE_FAILED` | Physical erase write or verify step failed |
| `SECURITY.PROTECTED_MATERIAL.DURABLE_CATALOG_INVALID` | On-disk catalog file is corrupt or mismatched |
| `SECURITY.PROTECTED_MATERIAL.PACKAGE_INVALID` | Export/import package is malformed or missing fields |
| `SECURITY.PROTECTED_MATERIAL.PACKAGE_DIGEST_MISMATCH` | Import package digest does not match expected value |
| `SECURITY.PROTECTED_MATERIAL.PACKAGE_UUID_CONFLICT` | Import package contains a UUID already present in the catalog |
| `SECURITY.PROTECTED_MATERIAL.PACKAGE_IMPORT_AUTHORITY_REQUIRED` | Import requires `import_authorized = true` |
| `SECURITY.FILESPACE.OPEN_INVALID` | Encrypted filespace open request missing required fields |
| `SB_LATE_PAYLOAD_FETCH.UNREDACTED_PROTECTED_PAYLOAD` | Late-fetch gate refused to expose protected payload bytes |
| `SB_LATE_PAYLOAD_FETCH.REDACTION_GATE_REQUIRED` | Security snapshot and redaction policy must be bound before late fetch |

## Invariants

- **Fail-closed**: Any missing, stale, ambiguous, or structurally invalid
  protected-material state refuses the operation rather than defaulting to
  allow. Missing policy bindings, missing active versions, and stale security
  epochs all produce refusals.

- **Plaintext never returned**: No public API entry point returns or stores a
  plaintext secret. Every result struct carries `plaintext_material_returned = false`
  as a stated invariant. The admission, storage, diagnostic, and late-payload
  gates enforce this independently.

- **Visibility = intersection of MGA visibility and materialized policy**: A
  version that is not visible to the caller's transaction snapshot cannot be
  resolved or released even if the caller holds the required rights. A version
  that is policy-hidden is similarly unreachable even when MGA-visible.

- **Purpose-bound release**: Release is refused unless the declared purpose
  appears in the version's or material's `release_purposes` allowlist, or
  matches the material's `purpose_class` when no explicit allowlist is set.
  Denied releases produce audit events before returning.

- **Legal hold blocks purge unconditionally**: If `legal_hold = true` on a
  version, purge is refused regardless of the retention epoch value. Both
  conditions must be clear before a physical erase can be requested.

- **Audit events are retained through version purge**: Purging a version's
  protected reference clears `protected_reference` and `envelope_reference`
  but does not delete audit events. Audit evidence survives the material it
  describes, subject to audit retention policy.

- **Support bundles are redacted**: Both the manager and listener support-bundle
  generators apply independent redaction functions before writing any line
  containing a sensitive key. The redaction coverage includes key handles,
  credentials, tokens, and private keys.

## Related Pages

- [security_model_overview.md](#ch-security-guide-security-model-overview-md) — three-layer model,
  fail-closed invariant, and the `SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED`
  diagnostic code
- [domain_and_column_security.md](#ch-security-guide-domain-and-column-security-md) — column-level
  grants, masking, and the `hidden_as_missing` decision used when protected
  catalog objects are not visible
- [grants_and_privileges.md](#ch-security-guide-grants-and-privileges-md) — how `KEY_RELEASE_APPROVE`
  and `PROTECTED_MATERIAL_RELEASE` rights are granted and revoked
- Language Reference: sys.security.protected_material_catalog (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX)
- Language Reference: sys.security.protected_material_version (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX)
- Language Reference: sys.security.protected_material_policy_binding (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX)
- Language Reference: sys.security.protected_material_audit (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX)
- [Operations and Administration: Identity, Security, and Policy](#ch-operations-administration-identity-security-and-policy-md)
- [Operations and Administration: Diagnostics, Message Vectors, and Support Bundles](#ch-operations-administration-diagnostics-message-vectors-and-support-bundles-md)




===== FILE SEPARATION =====

<!-- chapter source: Security_Guide/trust_and_separation_architecture.md -->

<a id="ch-security-guide-trust-and-separation-architecture-md"></a>

# Trust and Separation Architecture

## Purpose

This page describes the trust hierarchy and component separation that make
ScratchBird a Convergent Data Engine (CDE). The headline property is this:
**compromising any outer layer does not yield the ability to read, write, or
authorize data**, because durable authority lives only in the engine and is
never delegated outward through SQL text, parser output, driver calls, or
manager commands.

Understanding this architecture is a prerequisite for threat-modelling
ScratchBird deployments, evaluating what isolation a given deployment boundary
actually provides, and diagnosing why certain operations are refused even when
the outer layer appears to have accepted them.

### Security By Design, Not Bolted On

This separation is not a feature layered onto an existing database; it is the
premise the engine was built on. ScratchBird was designed so that a deployment
can be operated to a high-assurance, government-grade security posture **if the
operator chooses to implement it**, rather than requiring such controls to be
retrofitted later. The architecture assumes outer layers are hostile by
default: the engine does not trust the parser, the listener, the manager, or
the client driver, and it revalidates and fail-closes rather than extending
trust. The authentication, authorization, policy, masking, protected-material,
and audit mechanisms documented elsewhere in this guide exist precisely so that
an operator can dial the posture up to what their environment requires without
re-architecting the system.

Two consequences follow for readers. First, the strong controls are
**available from the start** but are largely **opt-in**: a minimal deployment
and a hardened one run the same engine, differing in how much of the security
surface the operator configures. Second, this guide documents the mechanisms
and their source-level enforcement; it makes **no certification claim** (no
FIPS, Common Criteria, or equivalent accreditation is asserted), and any
specific compliance regime must be validated independently against the target
build and configuration.

This page describes architectural intent as enforced by source-level controls
cited throughout. It is a **draft**; no claim here constitutes a production
security certification.

---

## Definitions

**Trust status toward data** — whether a component can directly read or write
durable database state. A component is "untrusted" toward data when it has no
direct path to storage: it can be compromised and still cannot reach or alter
the database files or the engine's internal authority records.

**Authority** — the set of source-documented invariants that decide whether a
request is admitted, executed, and committed. In ScratchBird, authority is held
exclusively by SBcore (the engine). Authority is not shared with, delegated to,
or overridable by drivers, listeners, parsers, or managers.

**SBLR** (ScratchBird Logical Representation) — the structured, typed,
engine-facing form that a parser produces for an accepted statement. SBLR is
_translation evidence_, not authority. The engine independently revalidates
every SBLR envelope before dispatching it.

**MGA** — ScratchBird's transaction and visibility authority model. MGA
transaction inventory is the engine's finality authority; parser packages and
drivers cannot modify or override it.

**Materialized authorization** — the process of evaluating the catalog's
durable grant, role, and group records against a principal UUID to produce an
`EngineMaterializedAuthorizationContext`. Authorization is materialized inside
the engine; it is not derived from parser context or driver claims.

**Fail-closed** — when required evidence is missing, stale, ambiguous, or
contradicted by an explicit denial, the engine refuses the operation. This is
the default posture at every authority boundary in ScratchBird.

---

## The Layered Model

The components form a nested pipeline from the network edge inward to the
engine. Each layer adds a trust boundary; only the innermost layer holds
durable authority.

| Layer | Product name | Role | Trust status toward data | Can do | Cannot do |
|-------|-------------|------|--------------------------|--------|-----------|
| Client driver / tool | Drivers listed in `project/drivers/DriverPackageManifest.csv` | Sends protocol frames; holds no durable state | **Untrusted** | Originate requests; hold credentials for the session handshake; render results | Write database files; commit or roll back transactions; bypass listener admission; escalate privilege beyond what the session's security context permits |
| Listener / network entry point | `SBgate` (`sb_listener`, CMake: `src/listener/`) | Accepts network connections; routes clients to a parser pool; manages parser worker processes | **Untrusted** | Route connections; spawn and supervise parser workers; enforce rate limits and TLS policy | Access storage; issue engine dispatch; override authorization; speak directly to the engine without routing through `SBsrv` |
| Parser worker package | `SBParser` (native SBsql v3, `sbp_native`); compatibility workers (`sbp_firebird`, `sbp_postgresql`, etc.) | Translates a client's language surface into an SBLR envelope; binds visible names through the session schema root | **Untrusted** (registered as `untrusted_translation_package_registration`; `ParserTrustMode::kUntrustedExternalProcess`) | Produce an SBLR envelope; render client-shaped diagnostics; request name resolution through `SBsrv` IPC | Write database files; decide transaction visibility; bypass server revalidation; authenticate locally (authentication must relay to `SBsrv`); promote SQL text to execution authority |
| IPC server / session boundary | `SBsrv` (`sb_server`, CMake: `src/server/`); session records in `ServerSessionRegistry` / `ServerSessionRecord` | Owns the IPC endpoint; performs SBLR revalidation; manages `ServerSessionRecord`s; dispatches admitted envelopes to the engine through `sb_server_engine_bridge` | Intermediary; performs **active revalidation** before the engine sees any parser output | Revalidate SBLR envelopes; enforce admission rules; host the engine via the private bridge adapter; mediate authentication relay; reject malformed, version-mismatched, or fail-closed envelopes | Hold storage state; own transaction finality; override engine authorization decisions |
| Engine | `SBcore` (`sb_engine`, public ABI frozen at `sb_engine_public_abi_1_0_0`) | Owns catalog UUID identity, descriptors, storage, MGA transaction inventory, materialized authorization, policy, diagnostics, and recovery | **Authority** | Execute admitted SBLR envelopes; materialize and enforce authorization; own MGA finality; own catalog state | Accept SQL text as authority; trust parser output without revalidation; expose private internal module targets as deployable products |
| Single-node manager | `SBmgr` (single-node manager; CMake target `sbmn_manager`, `src/manager/node/`) | Lifecycle, cluster discovery, admission, join/renewal, proxy-gate, management operations | **Untrusted toward data** (management channel, not storage authority) | Issue management commands; control listener orchestration; restart policy; config reload | Own transaction finality; speak directly to engine storage; escalate privilege beyond explicit `manager.auth` rights; bypass closed command validation |

The manager sits alongside the pipeline rather than inline. It controls the
deployment lifecycle but does not participate in the data path and has no path
to engine storage.

---

## The Authority Line

Durable authority in ScratchBird has four parts, all located inside SBcore.
Nothing outside SBcore holds, delegates, or overrides any of these.

### Execution authority: SBLR internal API only

Source: `project/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md`, Invariants
section; `project/README.md`.

> Engine execution authority is `engine_sblr_internal_api_only`.

The public C ABI entry point for execution is `sb_engine_dispatch_sblr`
(`src/engine/public_abi.cpp`). There is no path by which a driver, listener,
parser, or manager can invoke engine execution outside this controlled boundary.

### SQL text is never runtime authority

Source: `project/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md`; `project/README.md`
(forbidden list); `project/src/README.md` (forbidden: "SQL text as engine
execution authority").

> SQL text is never runtime authority inside the engine.

A parser may accept SQL text; the text itself confers nothing. The engine acts
only on a revalidated SBLR envelope. This makes it impossible to inject SQL
text at the IPC boundary and have the engine treat it as a direct execution
command.

### UUID identity and descriptor authority

Source: `project/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md`.

> UUID identity and descriptor/operand authority remain internal authority.

Object names visible to parsers and drivers are resolver inputs only.
Durable identity is the UUID assigned by SBcore. A parser that produces a
name-shaped reference does not thereby obtain authority over the named object;
that authority is resolved inside the engine against the catalog UUID.

### MGA transaction inventory as finality authority

Source: `project/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md`;
`src/storage/page/row_data_page.cpp` (`durable_mga_inventory_remains_authority=true`).

> MGA transaction inventory remains finality authority.

Transaction begin, commit, rollback, savepoint, visibility, cleanup, and
recovery state are all SBcore-owned. Drivers and parsers can _request_ these
operations; they cannot determine their outcome.

### Materialized authorization

Source: `security_model_overview.md`; `src/engine/internal_api/security/`.

Authorization is materialized from the catalog's durable grant, role, and group
records. A parser route that carries session identity does not grant rights; the
engine materializes and evaluates authorization independently at execution time.

---

## The Revalidation Boundary

The most concrete trust enforcement is the SBLR revalidation step that
`SBsrv` performs on every SBLR envelope it receives from a parser worker.

### Why revalidation is necessary

Parser workers run as `ParserTrustMode::kUntrustedExternalProcess`
(`src/listener/listener_config.hpp`, line 35–37, 56). The listener spawns them
as external processes. A compromised or maliciously crafted parser output is
therefore possible. Accepting parser-produced SBLR without independent
verification would mean the server's security model is only as strong as the
least-trusted parser.

Revalidation means the server/engine pair never trusts the parser's SBLR
uncritically. The admission step in `src/server/sblr_admission.cpp` re-decodes,
re-checks structure and version, evaluates family rules, and applies fail-closed
controls before dispatching to the engine.

### The diagnostic code

When a parser-submitted SBLR envelope fails server revalidation, the server
issues diagnostic `PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED`
(`src/server/sblr_admission.cpp`, multiple call sites including line 1375):

> "The SBLR envelope failed server revalidation."

Specific failure messages include version mismatches, missing or empty payloads,
unsupported envelope major version, and structural failures at the binary decode
step. None of these result in a fallback or a retry with reduced checks;
every case rejects the submission.

### What revalidation checks

The `sblr_admission.cpp` admission logic verifies:

- envelope version compatibility with this server;
- envelope structural integrity (binary decode must succeed);
- SBLR family admissibility (including fail-closed family rules via
  `IsFailClosedSblrFamily`);
- payload non-emptiness;
- absence of forbidden keys that would allow the parser to smuggle SQL text
  (`source_text`, `sql_text` — checked for duplicate injection).

### Authentication cannot be bypassed at the parser

Source: `src/parsers/sbsql_worker/auth/auth_relay.cpp`.

> "authentication must be relayed to sb_server; parser cannot authenticate
> locally"

A parser worker that attempts local authentication is refused. All
authentication relays through `SBsrv`.

---

## Module and Build Separation

### The server–engine bridge

Source: `src/server_engine_bridge/README.md`; CMake at
`src/server_engine_bridge/CMakeLists.txt`.

`sb_server_engine_bridge` is an INTERFACE library target (no compiled objects of
its own) that lets `sb_server` link the engine without exposing private engine
module names through the server product CMake boundary.

> "The public engine ABI remains `sb_engine`. Internal engine module targets
> remain private build dependencies and are not deployable products."

The implication for separation: the engine's internal modules are not
installable, not distributable, and not reachable except through the engine's
public ABI (`sb_engine`). A server-side change or compromise cannot bypass the
engine's internal authority model by linking a private engine module directly.

### The public engine ABI

The frozen public ABI (`sb_engine_public_abi_1_0_0`) is the only stable,
external-facing interface to the engine. Its C ABI symbols include
`sb_engine_dispatch_sblr` as the sole execution entry point. All other engine
interaction goes through session and transaction management symbols that also
enforce the SBLR-only execution invariant.

Packaged public headers are listed in
`project/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md` under "Packaged Public
Headers". No internal engine module header is in that list.

### Forbidden boundaries in source

Source: `project/src/README.md`.

The implementation source rules establish four forbidden combinations that
encode the separation architecture in build policy:

| Forbidden | Why it matters |
|-----------|----------------|
| Parser code inside IPC | Keeps the IPC boundary clean; parsers are external processes, not IPC participants |
| Compatibility-specific engine behavior inside core engine modules | Prevents compat parsers from having special engine privileges or divergent behavior |
| SQL text as engine execution authority | Closes the most direct injection vector |
| Private cluster authority in public package paths | Prevents cluster-authority escalation through public routes |

### Cluster behavior is outside core

Source: `project/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md`, Invariants.

> "Cluster-positive behavior is outside core and must route through a provider
> or fail closed in non-cluster builds."

The non-cluster refusal code `SBLR.CLUSTER.SUPPORT_NOT_ENABLED` is emitted by
`src/cluster_provider/cluster_provider.hpp` and `src/core/agents/agent_runtime.cpp`
when cluster-positive work is requested without an available provider. The
cluster private operation SBLR family (`sblr.cluster.private_operation.v3`) is
in the fail-closed family set (`src/server/sblr_admission.cpp`, line 87–89):
it cannot be admitted without cluster authority active.

The IPC lifecycle enforces the same boundary at runtime
(`src/server/server_ipc_lifecycle.cpp`, line 491–493):

> "The IPC endpoint is private to cluster authority and must fail closed."

---

## Fail-Closed Principle

ScratchBird's security model applies the fail-closed invariant at every
authority boundary: when required evidence is missing, unproven, stale, or
contradicted, the operation is refused rather than approximated or allowed.

The table below enumerates the verified fail-closed controls and their source
locations.

| Control identifier | Trigger | Source location |
|-------------------|---------|-----------------|
| `PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED` | SBLR envelope fails server revalidation (version mismatch, structural failure, empty payload, binary decode failure) | `src/server/sblr_admission.cpp` |
| `IsFailClosedSblrFamily` | SBLR envelope belongs to a fail-closed family; admitted only when corresponding authority is active | `src/server/sblr_admission.cpp` (line 724) |
| `sblr.cluster.private_operation.v3` | The only member of `kFailClosedSblrFamilies`; cluster-private operations fail closed without cluster authority | `src/server/sblr_admission.cpp` (line 87–89) |
| `ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED` | Cluster-positive work requested in a standalone (non-cluster) build | `src/storage/database/database_lifecycle.cpp`, `src/server/session_registry.cpp`, `src/server/config_policy_security_lifecycle.cpp`, others |
| `SERVER.SUPERVISION.FAIL_CLOSED` | Server supervision condition that requires closure | `src/server/config_policy_security_lifecycle.cpp` (line 459) |
| `decision:fail_closed_when_unproven` | Security lifecycle evidence tag: any unproven threat surface fails closed | `src/server/config_policy_security_lifecycle.cpp` (line 82) |
| `server_authority.unknown.fail_closed.v1` | Unknown server authority route contract; falls back to closed | `src/server/compatibility_server_authority.cpp` (line 331) |
| `server_authority.migration.unknown.fail_closed.v1` | Unknown migration route; fails closed | `src/server/compatibility_server_authority.cpp` (line 412) |
| `compatibility_function.unknown.fail_closed.v1` | Unknown compatibility function route; fails closed | `src/engine/functions/metadata/compatibility_function_surface_policy.cpp` (line 160) |
| `IPC.LIFECYCLE.CLUSTER_AUTHORITY_REQUIRED` | IPC endpoint is cluster-private but cluster authority is unavailable | `src/server/server_ipc_lifecycle.cpp` (line 492–493) |
| `SBLR.CLUSTER.SUPPORT_NOT_ENABLED` | Cluster-positive operation in a non-cluster build | `src/cluster_provider/cluster_provider.hpp`; `src/core/agents/agent_runtime.cpp` |
| `UDR.BRIDGE.SANDBOX_DENIED` | Bridge dispatch requests a physical page-copy, physical backup, or server-local file stream | `src/udr/sbu_sbsql_parser_support/sbu_sbsql_parser_support.cpp` (line 378) |

The common pattern is: "unproven means refuse". There is no escalating retry,
no degraded-mode fallback that reduces checking, and no implicit permit for
authority that was not positively established.

---

## Compromise Scenarios

The table below describes what is and is not reachable if a given layer is
compromised, grounded in the separation controls documented above. This is the
architecture's intent as established by the cited mechanisms; it does not claim
that all implementation paths have been independently audited.

| Compromised component | What is reachable from that position | What is not reachable | Controlling mechanism |
|----------------------|--------------------------------------|----------------------|----------------------|
| Client driver | Network connection to the listener; ability to send well-formed or malformed wire frames; credentials for the session that driver manages | Storage files; engine dispatch without listener/server admission; other sessions' transaction state; materialized authorization of other principals | Driver has no IPC path to the engine; no path to storage; all actions route through listener and server admission |
| `SBgate` listener | Parser worker pool; network connection handling; ability to spawn or kill parser workers; connection routing decisions | Direct IPC to engine; SBLR dispatch without server revalidation; authentication decisions (authentication relays to `SBsrv`); storage | Listener has no engine IPC endpoint; server (`SBsrv`) owns the IPC endpoint and the revalidation step; parser trust mode is `kUntrustedExternalProcess` regardless of listener state |
| Parser worker (`SBParser` or compatibility parser) | Ability to craft arbitrary SBLR envelopes submitted to `SBsrv` IPC; access to names visible in the session schema root | Bypassing `SBsrv` SBLR revalidation (`PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED`); authenticating locally; committing or rolling back transactions without engine admission; reading or writing storage pages; overriding materialized authorization | Parser is registered as `untrusted_translation_package_registration`; `ParserTrustMode::kUntrustedExternalProcess`; all submitted envelopes pass through `sblr_admission.cpp` checks before engine sees them; SQL text is not engine authority |
| `SBmgr` (single-node manager; CMake target `sbmn_manager`) | Lifecycle and config management commands; listener restart; management MCP surface (bounded by `manager.auth.mcp_secret_rights`) | Direct engine IPC; transaction finality; storage access; SBLR dispatch; overriding security contexts | Manager is a control-plane component, not an IPC server or storage authority; management commands require bounded idempotency keys and explicit rights; `ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED` refuses cluster-positive manager paths in standalone builds |

Note: these scenarios describe the trust boundary architecture. A compromise
that also involves physical access to storage files or the ability to inject
shared libraries into the engine process is outside the scope of this
architectural model.

---

## Cross-References

- [security_model_overview.md](#ch-security-guide-security-model-overview-md) — the three-layer
  model (authentication, authorization, deep enforcement) and the fail-closed
  invariant as applied to security principal operations
- [authentication_and_providers.md](#ch-security-guide-authentication-and-providers-md) —
  provider trust states, plugin admission, and how authentication evidence is
  normalized by the engine
- ../Getting_Started/architecture/engine_parser_boundary.md (ScratchBird — Concepts and Getting Started, page XXX) —
  end-user explanation of the parser/engine boundary and why parsers are
  translators, not storage authorities
- ../Language_Reference/core_paradigms/security_and_sandboxing.md (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX) —
  fail-closed security model from the SBsql Language Reference perspective;
  materialized security, sandbox roots, row-level security, and masking
- [../Operations_Administration/parser_registration_and_routes.md](#ch-operations-administration-parser-registration-and-routes-md) —
  how parser packages are registered, versioned, and routed; the `ParserHello`
  protocol and IPC protocol version enforcement




# Agent Runtime Guide




===== FILE SEPARATION =====

<!-- chapter source: Agent_Runtime_Guide/README.md -->

<a id="ch-agent-runtime-guide-readme-md"></a>

# ScratchBird Agent Runtime Guide

## Purpose

This guide is the authoritative deep-dive reference for the ScratchBird Agent Runtime — the subsystem by which the engine manages itself. Operators, advanced integrators, and anyone who needs to understand how ScratchBird maintains convergent multi-model data without constant human intervention should read it.

Audience: operators responsible for production deployments, integrators embedding ScratchBird, and security reviewers evaluating autonomous-action scope.

**Draft status.** Every concrete claim here has been verified against the project source tree. No claim constitutes a production readiness statement or an autonomous-behavior guarantee for any specific build configuration.

---

## What the Agent Runtime Is

ScratchBird is a Convergent Data Engine (CDE): a single engine that manages relational, key-value, document, search, vector, graph, and time-series models under a unified transactional foundation. The breadth of that convergence creates a management surface far larger than any single-model system. Keeping it operational — cleaning old row versions, draining index debt across all model families, managing storage capacity, running backup-readiness drills, monitoring node health, tuning approximate-search parameters, handling session pressure — would require continuous human attention if managed manually.

The Agent Runtime is the answer: a governed set of autonomous agents that observe engine state, decide what to do, and execute within strictly bounded authority. Agents are first-class engine citizens. They are declared in a single machine-readable manifest, each assigned a deployment scope, authority class, and default activation profile. Their actions are arbitrated, resource-governed, dry-run tested, evidence-backed, and optionally subject to operator approval before execution.

---

## Safety Philosophy

The central design principle of the Agent Runtime is that **autonomy is bounded**. Specifically:

**The engine owns. Agents never do.**

- **Transaction finality** belongs to the engine's Multi-Generational Architecture (MGA). No agent can commit or abort a transaction on behalf of a user.
- **Catalog visibility** is owned by the engine. An agent observing catalog state reads from the engine's catalog surfaces; it does not get to decide what other sessions can see.
- **Parser authority** belongs to the registered parser package. Agents cannot interpret or modify SQL text, cannot alter the parse-to-SBLR pipeline, and cannot route queries.
- **WAL/recovery authority** belongs to the engine's recovery subsystem. Agents can observe recovery state and recommend action; they cannot drive WAL replay or crash recovery.
- **Storage identity** is UUID-tracked by the engine. Agent persistence uses the engine's storage authority, not a write-ahead log of its own.

Beyond these hard prohibitions, agents operate under an authority ladder (observe only → recommend only → request action → direct bounded action), fail closed on any missing proof, produce tamper-evident evidence for every decision, and require explicit operator approval before transitioning from dry-run to live-action.

Conventional database background-maintenance workers typically operate with broad, implicit engine privilege and fail silently or cause indeterminate state when something goes wrong. ScratchBird agents instead carry no implicit privilege: every action is policy-gated, arbitrated, resource-budgeted, and evidence-recorded. This makes the autonomous subsystem auditable, reversible where possible, and observable without special tooling.

---

## Structure of This Guide

| Page | Contents |
|------|----------|
| [authority_and_activation_model.md](#ch-agent-runtime-guide-authority-and-activation-model-md) | 4-tier authority ladder, 5 activation profiles, 14 lifecycle states, action result classes, hard authority boundaries |
| [agent_catalog.md](#ch-agent-runtime-guide-agent-catalog-md) | The complete verified agent set (29 agents), grouped by domain, with deployment class, scope, authority, default activation |
| [action_lifecycle_and_arbitration.md](#ch-agent-runtime-guide-action-lifecycle-and-arbitration-md) | Action lifecycle from proposal to evidence, arbitration priority ordering, overrides, break-glass |
| [governance_and_resource_control.md](#ch-agent-runtime-guide-governance-and-resource-control-md) | 14-dimension resource budget, decision kinds, foreground protection, worker capacity, rollout profiles, tenant coordination |
| [evidence_explainability_and_safety.md](#ch-agent-runtime-guide-evidence-explainability-and-safety-md) | Tamper-evident evidence chain, dry-run, simulation, replay quarantine, explainability, fault injection, safe mode |
| [maintenance_and_tuning_agents.md](#ch-agent-runtime-guide-maintenance-and-tuning-agents-md) | Deep dives: transaction pressure manager, cleanup debt scheduler, online maintenance progress, adaptive tuning, restore drills |
| [observability_and_control.md](#ch-agent-runtime-guide-observability-and-control-md) | sys.information.* agent projections, sys.agents surface, SBsql control surface, operator levers |

---

## Cross-References

- **SBsql control surface** (agent statements, ALTER AGENT, SHOW AGENTS, override management): ../Language_Reference/syntax_reference/agent.md (SBsql Language Reference — Syntax, page XXX). This guide does not restate syntax; it explains the architecture behind it.
- **CDE-level summary** (why autonomous operation matters in a CDE, orientation): ../CDE_Concepts/autonomous_operation.md (ScratchBird — Concepts and Getting Started, page XXX)
- **Security Guide** (rights required for agent control, evidence redaction, protected material): [../Security_Guide/README.md](#ch-security-guide-readme-md)
- **Operations and Administration** (monitoring, health, readiness, backup/restore runbooks): [../Operations_Administration/README.md](#ch-operations-administration-readme-md)
- **Transaction pressure escalation ladder** (warn/restart/reauth/cancel/force thresholds): ../Getting_Started/core_concepts/understanding_mga.md (ScratchBird — Concepts and Getting Started, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Agent_Runtime_Guide/authority_and_activation_model.md -->

<a id="ch-agent-runtime-guide-authority-and-activation-model-md"></a>

# Authority and Activation Model

## Purpose

This page defines the authority and activation framework that governs every ScratchBird agent. Before reading about specific agents or their behavior, operators and integrators should understand the four-tier authority ladder, the five activation profiles, the fourteen lifecycle states, the action result classes, and the hard authority boundaries that no agent is permitted to cross.

All enumerations and field names on this page are verified against `project/src/core/agents/agent_runtime.hpp`.

---

## The Four-Tier Authority Ladder

Every agent in the canonical manifest is assigned exactly one `AgentAuthorityClass`. This class defines the ceiling of what the agent can do; the actual effective authority at runtime is further constrained by activation profile, lifecycle state, policy gates, resource budgets, dry-run requirements, and manual approval requirements.

| Authority Class | Token | What the agent may do |
|-----------------|-------|----------------------|
| Observe only | `observe_only` | Read metrics, health, and state. Produce no recommendations and initiate no actions. |
| Recommend only | `recommend_only` | Produce policy recommendations visible to operators. No binding actions. |
| Request action | `request_action` | Submit action requests to the engine for evaluation; the engine decides whether to admit them. |
| Direct bounded action | `direct_bounded_action` | Execute bounded, resource-governed actions within a pre-approved actuator contract; still subject to all safety, arbitration, and evidence requirements. |

Moving up the ladder always requires operator-level configuration. No agent self-promotes its authority class.

---

## The Five Activation Profiles

An agent's `AgentActivationProfile` is the configured operational posture. It controls what the agent will attempt when the lifecycle state is compatible.

| Profile | Token | Behavior |
|---------|-------|----------|
| Disabled | `disabled` | Agent does not run. Cluster-only agents default to this on non-cluster deployments. |
| Observe only | `observe_only` | Agent collects and publishes metric observations and health state. No recommendations or actions. |
| Recommend only | `recommend_only` | Agent produces recommendations that appear in the observability surface. No live mutations. |
| Dry run | `dry_run` | Agent evaluates actions and records what it would do, but mutations are suppressed. Evidence is recorded. |
| Live action | `live_action` | Agent may execute mutations, subject to all safety, arbitration, approval, and resource gates. |

Advancing from `dry_run` to `live_action` requires explicit operator approval. The function `ValidateRolloutTransition` enforces this: the `explicit_operator_approval` parameter must be true, and the transition is refused otherwise. This is the gated escalation mechanism — no agent accidentally goes live.

`EffectiveActivationForLifecycle` adjusts the configured profile based on current engine lifecycle mode (for example, reducing effective activation during backup, restore, crash recovery, or read-only modes). The effective activation is always the more restrictive of the configured profile and the lifecycle-derived constraint.

---

## The Fourteen Lifecycle States

Every agent instance moves through a state machine with exactly fourteen states. Transitions are validated by `AgentLifecycleTransitionAllowed` and `ValidateAgentLifecycleTransition`.

| State | Token | Meaning |
|-------|-------|---------|
| Created | `created` | Instance record allocated; not yet registered. |
| Registered | `registered` | Manifest and policy checks passed; instance is known to the runtime. |
| Disabled | `disabled` | Operator or policy has disabled this instance. |
| Observe only | `observe_only` | Running in metric-observation mode only. |
| Recommend only | `recommend_only` | Running; producing recommendations but no actions. |
| Dry run | `dry_run` | Running; evaluating actions and recording evidence without executing mutations. |
| Running | `running` | Running in live-action mode; mutations permitted within bounds. |
| Paused | `paused` | Temporarily suspended by operator request; can be resumed. |
| Safe mode | `safe_mode` | Engine or agent supervisor has detected a risk condition and reduced the agent to safe-mode scope. |
| Quarantined | `quarantined` | Multiple supervision failures or a security/integrity fault has isolated the instance. Recovery requires operator action. |
| Stopping | `stopping` | Graceful shutdown initiated; draining current work. |
| Stopped | `stopped` | Agent has completed shutdown. |
| Retired | `retired` | Instance is superseded by a new generation; evidence record is preserved. |
| Failed | `failed` | The agent encountered an unrecoverable error. |

Quarantine is reached through supervision failure accumulation (tracked in `AgentInstanceRecord.crash_loop_count`, `supervision_failure_count`, and `restart_attempts`) or through an explicit `QuarantineAgentInstance` call. Release from quarantine requires operator action and is not automatic.

---

## Action Result Classes

Every evaluated action resolves to one of seven `AgentActionResultClass` outcomes.

| Result | Token | Meaning |
|--------|-------|---------|
| Accepted | `accepted` | Action was evaluated and executed (or queued for execution). |
| Refused | `refused` | Action was actively declined by a safety check, policy gate, or authority constraint. |
| Suppressed | `suppressed` | Action was not executed because of a conflicting override or arbitration loss; evidence is recorded. |
| Dry run only | `dry_run_only` | Action was evaluated but mutation was suppressed because the profile is `dry_run`. |
| Approval required | `approval_required` | Action cannot proceed without manual operator approval. |
| Failed closed | `failed_closed` | A required gate (dependency, metric, policy, actuator) was unavailable and the agent defaulted to no-mutation. |
| Quarantined | `quarantined` | The requesting agent instance is in the quarantine state; all actions refused. |

`failed_closed` is the runtime default. When any required proof is absent — policy missing, metric stale, actuator route unregistered, security context absent — the action result is `failed_closed` rather than proceeding under uncertainty.

---

## Hard Authority Boundaries

These prohibitions are enforced in source through boolean flags on `AgentEvidenceRecord` and `AgentReplayControlRequest`, and explicitly stated in comments throughout the agent subsystem. No agent in the canonical manifest is permitted to claim or exercise:

- **Transaction finality authority** (`finality_authority = false` in evidence records) — agents cannot commit, abort, or finalize any user transaction.
- **Visibility authority** (`visibility_authority = false`) — agents cannot alter what rows or catalog objects are visible to other sessions.
- **Parser authority** (`parser_authority = false`) — agents cannot interpret SQL text, manipulate the parse-to-SBLR pipeline, or influence query routing.
- **Recovery authority** (`recovery_authority = false`) — agents cannot drive WAL replay, crash recovery, or PITR execution as authority holders. They may observe and recommend.
- **WAL authority** (`wal_authority = false` in replay control) — agent persistence uses the engine's storage authority, not a write-ahead log of its own.
- **Client authority** (`client_authority = false`) — agents cannot impersonate or act on behalf of client sessions.

The `AdaptiveTuningControllerRequest.safety` struct (`AdaptiveTuningSafetyPolicy`) makes these denials explicit for the adaptive tuning subsystem: `parser_or_reference_authority`, `provider_transaction_finality_authority`, `provider_visibility_authority`, `client_autocommit_authority`, and `wal_recovery_authority` are all false by design, and the tuning controller refuses any request that asserts otherwise.

---

## Manual Approval and Break-Glass

`AgentPolicy.require_manual_approval` and `AgentPolicy.require_dry_run_before_live` are the two primary gates for high-consequence actions. When `require_manual_approval` is true, the action produces an `approval_required` result and is held until an operator-level principal with the `obs_agent_action_approve` right explicitly approves it via `ValidateManualApproval`.

Break-glass overrides (`AgentArbitrationOverride`) allow an operator to explicitly allow or suppress specific action UUIDs within a bounded expiry window. Overrides carry their own evidence UUID, require the `obs_agent_override` right, and are recorded in the arbitration evidence chain.

---

## See Also

- [agent_catalog.md](#ch-agent-runtime-guide-agent-catalog-md) — how each agent is assigned to a tier
- [action_lifecycle_and_arbitration.md](#ch-agent-runtime-guide-action-lifecycle-and-arbitration-md) — the full action flow from proposal to evidence
- [governance_and_resource_control.md](#ch-agent-runtime-guide-governance-and-resource-control-md) — rollout profiles and the gated escalation mechanism
- ../Language_Reference/syntax_reference/agent.md (SBsql Language Reference — Syntax, page XXX) — SBsql syntax for controlling agents




===== FILE SEPARATION =====

<!-- chapter source: Agent_Runtime_Guide/agent_catalog.md -->

<a id="ch-agent-runtime-guide-agent-catalog-md"></a>

# Agent Catalog

## Purpose

This page enumerates the complete, verified set of agents declared in the canonical agent manifest (`project/src/core/agents/agent_runtime_manifest.def`). The manifest is the single source of truth; the runtime manifest builder and a manifest-drift gate both read it. No agent may operate in the engine without a manifest entry.

The total verified count is **29 agents**.

Columns: **Agent** (type_id token), **Deployment** (local / cluster / both), **Scope**, **Authority Class**, **Default Activation**, **Purpose**.

Cluster-only agents (`deployment = cluster`) are automatically `disabled` on deployments without a cluster provider. Attempting to activate them without cluster authority fails closed.

---

## Domain Groups

Agents are presented by functional domain for readability. The ordering within each domain follows the manifest file.

---

### Resource and Capacity

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `node_resource_agent` | local | `node` | `observe_only` | `observe_only` | Observes node-level resource state (CPU, memory, I/O) and publishes metrics for consumption by other agents. Takes no action. |
| `filespace_capacity_manager` | local | `node/database/filespace` | `request_action` | `recommend_only` | Monitors filespace utilization and requests capacity adjustments when available pages fall below configured thresholds. |
| `page_allocation_manager` | local | `database/filespace/page_family/page_type` | `request_action` | `recommend_only` | Coordinates page allocation and deallocation within a filespace; notifies the filespace capacity manager when free-page reserve falls to the notification threshold. |
| `cluster_autoscale_manager` | cluster | `cluster` | `request_action` | `disabled` | Requests cluster scaling operations based on aggregate workload metrics. Cluster-only; disabled without cluster authority. |

---

### Storage Health and Maintenance

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `storage_health_manager` | local | `node/database/filespace` | `recommend_only` | `recommend_only` | Evaluates storage health indicators and publishes recommendations for operator review. Does not initiate repair actions directly. |
| `storage_version_cleanup_agent` | local | `database/filespace/page_family/row_version` | `direct_bounded_action` | `dry_run` | Cleans up obsolete row versions (version chain debt) within policy-bounded work windows. This is the relational cleanup worker within the broader cleanup debt scheduler. |
| `cleanup_archive_manager` | both | `database/cluster` | `direct_bounded_action` | `dry_run` | Coordinates archiving of completed cleanup work and manages the lifecycle of cleanup-related catalog records across local and cluster scopes. |

---

### Memory

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `memory_governor` | local | `node/database/session/workload` | `direct_bounded_action` | `dry_run` | Monitors and governs memory usage across the engine, sessions, and workloads. Can apply direct bounded memory limits within policy. |

---

### Transaction and Session Pressure

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `transaction_pressure_manager` | both | `database/cluster` | `request_action` | `recommend_only` | Monitors long-idle transactions and applies a policy-configured escalation ladder: warn, request restart, request reauth, request cancel, and (if policy explicitly permits) force action. The engine owns the transaction; the agent requests. |
| `session_control_manager` | both | `database/cluster/session` | `request_action` | `recommend_only` | Monitors session health and enforces session-level policy controls (idle limits, reauth requirements) by requesting engine action. |

---

### Index Health and Maintenance

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `index_health_manager` | local | `database/index` | `recommend_only` | `recommend_only` | Evaluates index health, fragmentation, and freshness across all index types; publishes recommendations for rebuild, refresh, or operator review. |

---

### Policy, Learning, and Optimization

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `policy_recommendation_manager` | both | `database/cluster` | `recommend_only` | `recommend_only` | Evaluates engine state against policy templates and publishes candidate policy recommendations for operator review and approval before any application. |
| `runtime_learning_agent` | local | `database/optimizer` | `recommend_only` | `recommend_only` | Observes query patterns, optimizer decisions, and plan quality to produce optimizer-tuning recommendations. Advisory only. |
| `parser_interface_manager` | local | `node/parser/interface` | `request_action` | `recommend_only` | Monitors the parser interface and can request adjustments to parser registration or routing within the engine's parser authority framework. Cannot modify parser behavior directly. |

---

### Admission and Scheduling

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `admission_control_manager` | both | `database/cluster/workload` | `direct_bounded_action` | `dry_run` | Governs workload admission to protect foreground database work. Can apply direct bounded admission decisions within policy. |
| `metrics_registry_manager` | both | `node/database/cluster` | `direct_bounded_action` | `dry_run` | Maintains the health and freshness of the metrics registry across node, database, and cluster scopes. |
| `job_control_manager` | both | `database/cluster/jobs` | `request_action` | `recommend_only` | Manages lifecycle of background and scheduled jobs. Requests job scheduling and cancellation; does not own job finality. |
| `cluster_scheduler_manager` | cluster | `cluster/jobs` | `request_action` | `disabled` | Coordinates cluster-wide job scheduling. Cluster-only; disabled without cluster authority. |

---

### Backup, Restore, Archive, and PITR

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `backup_manager` | both | `database/cluster/backup` | `request_action` | `recommend_only` | Initiates and monitors backup operations; requests backup execution through the engine's backup subsystem. |
| `archive_manager` | both | `database/cluster/archive` | `direct_bounded_action` | `dry_run` | Manages archive lifecycle including retention, expiry, and transition of backup artifacts within policy. |
| `restore_drill_manager` | both | `database/cluster/restore` | `request_action` | `recommend_only` | Automates restore-readiness verification by running isolated restore drills. Requires target isolation, backup manifest availability, and restore-inspection authorization before proceeding. |
| `pitr_manager` | both | `database/cluster/pitr` | `request_action` | `recommend_only` | Manages point-in-time recovery readiness, retention window validation, and PITR execution requests. |

---

### Identity and Security

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `identity_manager` | both | `database/cluster/security` | `request_action` | `recommend_only` | Monitors identity and security policy state; requests security policy enforcement actions. Does not own authorization decisions. |

---

### Metrics, Alerting, and Observability

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `alert_manager` | both | `node/database/cluster` | `direct_bounded_action` | `dry_run` | Evaluates alert conditions across node, database, and cluster scopes and can take direct bounded alerting actions (for example, recording alert evidence or triggering notification routes) within policy. |
| `distributed_query_metrics_agent` | cluster | `cluster/query` | `observe_only` | `disabled` | Observes distributed query execution metrics across the cluster. Cluster-only; disabled without cluster authority. Takes no action. |

---

### Query Routing

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `remote_query_routing_agent` | cluster | `cluster/query/route` | `request_action` | `disabled` | Requests cluster-level query routing adjustments based on observed routing metrics. Cluster-only; disabled without cluster authority. |

---

### Diagnostics and Support

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `support_bundle_triage_agent` | both | `node/database/cluster/support` | `request_action` | `recommend_only` | Monitors the support bundle surface; can request triage evidence capture and bundle assembly. |
| `export_adapter_manager` | both | `node/database/cluster/export` | `request_action` | `recommend_only` | Manages export adapter lifecycle, routing, and health monitoring. |

---

### Cluster Lifecycle

| Agent | Deployment | Scope | Authority Class | Default Activation | Purpose |
|-------|-----------|-------|-----------------|-------------------|---------|
| `cluster_upgrade_manager` | cluster | `cluster/upgrade` | `request_action` | `disabled` | Coordinates cluster upgrade sequencing and readiness checks. Cluster-only; disabled without cluster authority. |

---

## Summary Statistics

| Deployment class | Count |
|-----------------|-------|
| `local` only | 8 |
| `cluster` only | 5 |
| `both` (local and cluster scope) | 16 |
| **Total** | **29** |

| Default activation | Count |
|-------------------|-------|
| `disabled` | 5 (all cluster-only) |
| `observe_only` | 1 |
| `recommend_only` | 16 |
| `dry_run` | 7 |
| `live_action` | 0 |

No agent ships with a default activation of `live_action`. Reaching live-action requires an explicit operator-approved rollout transition.

---

## Cluster-Only Agents

The following five agents are `cluster` deployment only and default to `disabled`. They are inoperative on standalone (non-cluster) deployments:

- `cluster_autoscale_manager`
- `distributed_query_metrics_agent`
- `remote_query_routing_agent`
- `cluster_scheduler_manager`
- `cluster_upgrade_manager`

Attempting to activate these agents without cluster authority fails closed with `unavailable_cluster_authority`.

---

## See Also

- [authority_and_activation_model.md](#ch-agent-runtime-guide-authority-and-activation-model-md) — definitions of authority classes, activation profiles, and lifecycle states
- [maintenance_and_tuning_agents.md](#ch-agent-runtime-guide-maintenance-and-tuning-agents-md) — deep dives on agents with rich verified behavior
- ../Language_Reference/syntax_reference/agent.md (SBsql Language Reference — Syntax, page XXX) — SBsql statements for inspecting and controlling agents




===== FILE SEPARATION =====

<!-- chapter source: Agent_Runtime_Guide/action_lifecycle_and_arbitration.md -->

<a id="ch-agent-runtime-guide-action-lifecycle-and-arbitration-md"></a>

# Action Lifecycle and Arbitration

## Purpose

This page traces the path of a single agent action from initial proposal through safety evaluation, dry-run, arbitration, optional manual approval, execution, and the recording of evidence. It also defines the full arbitration priority model — the ordered rules the engine uses to resolve competing actions — and explains how operators can influence that model through suppression and force-allow overrides.

All enumeration names and function signatures are verified against `project/src/core/agents/agent_runtime.hpp`.

---

## The Action Lifecycle

Every agent action follows the same ordered pipeline. A failure at any stage produces an `AgentActionResultClass` outcome and records evidence; nothing is silently dropped.

### Stage 1 — Proposal

An agent that holds `request_action` or `direct_bounded_action` authority constructs an `AgentActionRequest`. The request carries:

- A unique `action_uuid`
- The `agent_type_id` and `instance_uuid` of the submitting agent
- An `AgentActionClass` (for example, `direct_bounded_action` or `recommendation`)
- An `actuator_id` and `operation_id` identifying the registered actuator route
- An `idempotency_key`
- A `dry_run` flag (always `true` until all gates pass)
- Input parameters as a string map

At this point no mutation has occurred. The engine has received a candidate, nothing more.

### Stage 2 — Safety Precondition Evaluation

Before the request reaches arbitration, the engine checks:

- **Agent lifecycle state** — the instance must not be in `quarantined`, `stopped`, `retired`, or `failed` states.
- **Safe-mode gate** (`ValidateAgentSafeMode`) — if the instance is in `safe_mode`, the action scope is restricted.
- **Actuator route registration** — the actuator identified by `actuator_id` must be registered and not degraded.
- **Feature gate** (`EvaluateAgentFeatureAvailability`) — the capability must be `available` in the current runtime context.
- **Metric dependency resolution** — required metric families must be present, fresh, and at the required source quality.
- **Policy dependency state** — required policy families must be present, valid, and scope-compatible.
- **Security context** — the runtime context must include a security context; its absence causes `failed_closed`.
- **Action safety budget** (`ValidateActionSafetyBudget`) — the number of actions already used in the current window must not exceed `policy.action_budget_per_window`.
- **Overhead gate** (`ValidateAgentOverheadGate`) — runtime, metric-query, and evidence-write totals must remain within bounds.
- **Human command precedence** (`ValidateHumanCommandPrecedence`) — any directly issued human command takes precedence over agent proposals of the same class.

If any required gate is absent or fails, the action resolves to `failed_closed` immediately. This is not a refusal due to policy judgment; it is a proof-not-present condition.

### Stage 3 — Dry-Run Evaluation

An action is evaluated in dry-run mode first (`BuildDryRunDecision`) regardless of the agent's activation profile. In dry-run mode:

- The full decision logic executes
- Mutation is suppressed
- An `AgentEvidenceRecord` is written with result state `dry_run_only`
- The evidence record is tamper-chained (see [evidence_explainability_and_safety.md](#ch-agent-runtime-guide-evidence-explainability-and-safety-md))

If the agent's `AgentActivationProfile` is `dry_run`, the action resolves to `dry_run_only` and stops here. Live execution requires the profile to be `live_action`.

### Stage 4 — Arbitration

When multiple action candidates are active over the same scope, `ArbitrateAgentActionCandidates` selects at most one. The outcome is one of four `AgentArbitrationOutcome` values:

| Outcome | Token | Meaning |
|---------|-------|---------|
| Winner executes | `winner_executes` | One candidate won; the rest are suppressed. |
| Both denied | `both_denied` | No candidate satisfied the priority rules. |
| Operator review required | `operator_review_required` | The tie was unresolvable without human input. |
| Suppressed by override | `suppressed_by_override` | An active `AgentArbitrationOverride` forced the outcome. |

Arbitration is described in detail in the next section.

### Stage 5 — Manual Approval (when required)

If `AgentPolicy.require_manual_approval` is `true`, or if the action contract marks `manual_approval_required`, the action produces an `approval_required` result and enters the approval queue. It remains there until an operator-level principal holding the `obs_agent_action_approve` right calls `ValidateManualApproval` for that action UUID.

If `AgentPolicy.require_dry_run_before_live` is `true`, at least one successful dry-run cycle must appear in the evidence history before the action can proceed to live execution regardless of approval status.

**Break-glass.** An `AgentArbitrationOverride` with the relevant `action_uuid` in its `allowed_action_uuids` list can clear the approval gate for actions in the `approval_required` queue, subject to the override's own expiry and the `obs_agent_override` right requirement.

### Stage 6 — Resource Budget Evaluation

`EvaluateAgentResourceBudget` checks the 14-dimension resource budget (see [governance_and_resource_control.md](#ch-agent-runtime-guide-governance-and-resource-control-md)). If the budget decision is anything other than `allow`, the action is deferred, refused, or drained without mutation.

### Stage 7 — Execution

With all gates cleared, `EvaluateAgentAction` sets `dry_run = false` on the request and dispatches through the actuator route. The actuator operates with the engine's bounded-action authority; it does not acquire independent authorization.

### Stage 8 — Outcome Verification and Evidence

`VerifyActionOutcome` compares the actuator's reported success against whether the intended state is actually observed. An outcome that reports success without observed state change is recorded as `failed_closed`. An outcome that reports failure with observed state change triggers compensation logic.

The final `AgentEvidenceRecord` is written with the resolved `AgentActionResultClass`, the full tamper chain, and the `outcome_verification_evidence_uuid`. This record is the authoritative audit trail for the action.

---

## The Seven Action Result Classes

| Result | Token | Meaning |
|--------|-------|---------|
| Accepted | `accepted` | Evaluated and executed (or queued for execution). |
| Refused | `refused` | Actively declined by a safety check, policy gate, or authority constraint. |
| Suppressed | `suppressed` | Not executed due to a conflicting override or arbitration loss; evidence is recorded. |
| Dry run only | `dry_run_only` | Evaluated but mutation suppressed because the profile is `dry_run`. |
| Approval required | `approval_required` | Held pending explicit operator approval. |
| Failed closed | `failed_closed` | A required proof was absent; defaulted to no-mutation. |
| Quarantined | `quarantined` | The submitting agent instance is in the `quarantined` lifecycle state; all actions refused. |

`failed_closed` is the default. The engine never proceeds under uncertainty when a required gate is absent.

---

## Arbitration Priority Ordering

When two or more action candidates compete, the arbitrator walks a deterministic priority-rule ladder (`AgentArbitrationPriorityRule`). The first rule that resolves the competition terminates the walk.

### Priority-Rule Ladder

| Step | Rule | Token | What it does |
|------|------|-------|--------------|
| 1 | No actions | `no_actions` | If there are zero candidates, outcome is `both_denied`. |
| 2 | Safety precondition failed | `safety_precondition_failed` | Any candidate with `safety_preconditions_passed = false` is eliminated first. |
| 3 | Single action | `single_action` | If exactly one candidate remains, it wins without further comparison. |
| 4 | Override — suppression | `override_suppression` | Active overrides with suppressed action UUIDs eliminate matching candidates. |
| 5 | Override — right required | `override_right_required` | If an override is present but the requester lacks `obs_agent_override`, the override is invalid and both are denied. |
| 6 | Override — authority forbidden | `override_authority_forbidden` | An override cannot elevate a candidate beyond its authority ceiling; if attempted, it is rejected. |
| 7 | Action class priority | `action_class_priority` | Candidates are ranked by `AgentArbitrationActionClass` priority order (see below). Higher priority wins. |
| 8 | Evidence quality | `evidence_quality` | Among candidates at the same class, the one with the higher `evidence_quality` score wins. |
| 9 | Exact tie — operator review | `exact_tie_operator_review` | If two candidates are indistinguishable, outcome is `operator_review_required` and an operator-review action record is created. |

### Arbitration Action Class Priority Order

`AgentArbitrationActionClass` values are compared by `AgentArbitrationActionClassPriority`. Higher priority wins:

| Priority | Class | Token |
|----------|-------|-------|
| 1 (highest) | Protect correctness | `protect_correctness` |
| 2 | Protect security | `protect_security` |
| 3 | Protect durability | `protect_durability` |
| 4 | Protect availability | `protect_availability` |
| 5 | Reduce pressure | `reduce_pressure` |
| 6 | Optimize performance | `optimize_performance` |
| 7 (lowest) | Reduce cost | `reduce_cost` |

An action with class `protect_correctness` always beats one with class `reduce_pressure` at the same scope, regardless of evidence quality.

### Risk and Reversibility

Each arbitration candidate carries two additional attributes that qualify the winner's profile:

**AgentArbitrationRisk** — `low`, `medium`, `high`, or `critical`. Risk is informational for the arbitrator but is surfaced in the evidence record for operator review. High-risk or critical-risk winners may trigger additional approval requirements depending on policy configuration.

**AgentArbitrationReversibility** — `reversible`, `bounded_reversible`, or `irreversible`. Irreversible actions receive additional scrutiny in the evidence record. An irreversible action at high or critical risk is the canonical trigger for requiring manual approval.

---

## Overrides: Suppression and Force-Allow

Operators can install `AgentArbitrationOverride` records to influence arbitration without modifying policy. An override requires:

- The `obs_agent_override` right
- A specified `expires_at_microseconds` (overrides are always time-bounded)
- An `evidence_uuid` referencing the override-creation evidence record

An override can contain:

- `suppressed_action_uuids` — specific action UUIDs that the arbitrator will eliminate via `override_suppression`
- `allowed_action_uuids` — specific action UUIDs that can bypass the normal arbitration competition

A `renewal_rule` and `rollback_rule` can be attached. The rollback rule specifies what happens if an operator clears the override before it expires — for example, whether any queued suppressed actions are released.

Overrides are scoped to a `scope_uuid`. An override for one database scope does not affect another database. The `active` flag on the override record allows operators to deactivate an override without deleting it, preserving the evidence trail.

---

## See Also

- [authority_and_activation_model.md](#ch-agent-runtime-guide-authority-and-activation-model-md) — action result classes, lifecycle states, manual approval mechanics
- [governance_and_resource_control.md](#ch-agent-runtime-guide-governance-and-resource-control-md) — resource budget evaluation, rollout profiles
- [evidence_explainability_and_safety.md](#ch-agent-runtime-guide-evidence-explainability-and-safety-md) — tamper-evident evidence chain, dry-run mode, replay quarantine
- ../Language_Reference/syntax_reference/agent.md (SBsql Language Reference — Syntax, page XXX) — SBsql syntax for submitting approvals and managing overrides
- ../CDE_Concepts/autonomous_operation.md (ScratchBird — Concepts and Getting Started, page XXX) — why governed autonomous action matters in a convergent engine




===== FILE SEPARATION =====

<!-- chapter source: Agent_Runtime_Guide/governance_and_resource_control.md -->

<a id="ch-agent-runtime-guide-governance-and-resource-control-md"></a>

# Governance and Resource Control

## Purpose

This page describes the mechanisms that ensure agent activity remains bounded and cannot interfere with the engine's primary obligation to serve foreground work. It covers the 14-dimension resource budget, the six budget-decision outcomes, the foreground-protection principle, worker capacity planning, rollout profiles and the gated escalation from observe to live, the effect of engine lifecycle modes on effective activation, tenant coordination and metric quorum, and the feature-gate model.

All enumeration names, function signatures, and field names are verified against `project/src/core/agents/agent_runtime.hpp`, `project/src/core/agents/agent_rollout_profile.hpp`, `project/src/core/agents/agent_tenant_coordination.hpp`, and `project/src/core/agents/agent_feature_gates.hpp`.

---

## The 14-Dimension Resource Budget

Every agent action is evaluated against an `AgentResourceBudget` before execution is permitted. The budget is derived from the agent's active policy via `DefaultAgentResourceBudgetForPolicy`. The evaluation function is `EvaluateAgentResourceBudget`, which returns an `AgentResourceBudgetDecision` carrying a `decision` field from `AgentResourceBudgetDecisionKind`.

The 14 dimensions tracked by `AgentResourceBudgetDimension` are:

| Dimension | Token | What it tracks |
|-----------|-------|---------------|
| Foreground protection | `foreground_protection` | Whether foreground database work is currently active; overrides all other dimensions when active. |
| CPU time | `cpu_time` | Cumulative CPU microseconds consumed in the current run. Bounded by `max_cpu_time_microseconds`. |
| Memory bytes | `memory_bytes` | Peak memory bytes allocated by the agent. Bounded by `max_memory_bytes`. |
| I/O bytes | `io_bytes` | Cumulative I/O bytes transferred. Bounded by `max_io_bytes`. |
| I/O operations | `io_ops` | Cumulative I/O operation count. Bounded by `max_io_ops`. |
| Thread slots | `thread_slots` | Number of concurrent execution threads. Bounded by `max_thread_slots`. |
| Queue depth | `queue_depth` | Number of items in the agent's internal work queue. Bounded by `max_queue_depth`. |
| Cadence | `cadence` | Minimum run interval. Enforced via `min_run_interval_microseconds`. |
| Retry backoff | `retry_backoff` | Minimum time between retries after failure. Enforced via `retry_backoff_microseconds`. |
| Runtime timeout | `runtime_timeout` | Maximum wall-clock time for a single run. Enforced via `watchdog_timeout_microseconds`. |
| Cancellation drain | `cancellation_drain` | Whether a cancellation or drain has been requested; active drain blocks new work. |
| History rows | `history_rows` | Rows the agent may query from history or catalog tables in one run. Bounded by `max_history_query_rows`. |
| Evidence fanout | `evidence_fanout` | Number of evidence records the agent may emit per run. Bounded by `max_evidence_fanout`. |
| Label cardinality | `label_cardinality` | Number of distinct labels the agent may attach to emitted evidence. Bounded by `max_label_cardinality`. |

The budget struct `AgentResourceBudget` exposes all 14 corresponding limit fields. The current consumption is tracked in `AgentResourceUsage`.

### Budget Decision Outcomes

`EvaluateAgentResourceBudget` produces one of six `AgentResourceBudgetDecisionKind` outcomes:

| Decision | Token | Meaning |
|----------|-------|---------|
| Allow | `allow` | All dimensions are within budget; action may proceed. |
| Throttle and defer | `throttle_defer` | One or more soft limits exceeded; action is deferred to the next eligible interval. |
| Shed and refuse | `shed_refuse` | A hard limit was reached; action is refused for this cycle (not quarantined). |
| Fail closed | `fail_closed` | Budget data was unavailable or structurally invalid; action defaults to no-mutation. |
| Cancel and drain | `cancel_drain` | An active cancellation or drain was detected; all agent work stops. |
| Foreground protection | `foreground_protection` | Foreground database activity was detected; agent work is suspended until it clears. |

A `foreground_protection` decision is always raised before any other dimension is evaluated. The budget field `protect_foreground_work = true` is set by default on every agent resource budget. Agents never compete with foreground work.

The `AgentResourceBudgetDecision` struct also carries `action_allowed`, `mutation_allowed`, and `health_publish_allowed` flags. Health observation is permitted even when mutation is denied, so operators continue to see accurate status during resource-constrained periods.

---

## Foreground Protection

The foreground-protection principle appears in three places:

1. The `AgentResourceBudget` `protect_foreground_work = true` default — enforced by `EvaluateAgentResourceBudget`.
2. The `AgentResourceBudgetEvaluationInput.foreground_database_work_active` flag — populated by the runtime from live engine state.
3. The `DynamicCleanupDebtSchedulerPolicy.protect_foreground_work = true` default — the cleanup scheduler also independently suspends when foreground work is active.

All three operate independently. An agent cannot bypass foreground protection in one layer by winning at another.

---

## Worker Capacity Planning

`PlanAgentWorkerCapacity` produces an `AgentWorkerCapacitySnapshot` that shows how many background worker slots are available and which agent candidates can be assigned to them.

The inputs are:

- `AgentWorkerCapacityConfig` — `observed_cpu_count`, `configured_cpu_count`, `foreground_reserved_capacity`, `max_background_worker_slots`, and runtime context flags.
- A list of `AgentWorkerCapacityCandidate` records — each carrying a policy, current resource usage, and `requested_worker_slots`.

The snapshot output includes:

- `effective_cpu_count` — the lesser of observed and configured core counts.
- `foreground_reserved_capacity` — the number of CPU slots held exclusively for foreground work.
- `background_worker_slots` — slots available for agent work after the foreground reserve is subtracted.
- `foreground_work_active` — whether the engine is currently serving foreground demand.
- A per-candidate `AgentWorkerCapacityAssignment` with `selected`, `assigned`, `resource_decision`, and the `resource_dimension` that constrained the decision if applicable.

The snapshot is auditable (it carries evidence) and is not an authority for parser admission, transaction finality, catalog truth, or security decisions.

DML-prework agents (those flagged `dml_prework_agent = true` in their candidate record) may be scheduled ahead of foreground demand arrival; the snapshot marks these with `can_run_before_foreground_demand = true`.

---

## Rollout Profiles and Gated Escalation

### Activation Profile Progression

Activation follows a strictly ordered progression. `ValidateRolloutTransition` enforces the rules:

```
disabled → observe_only → recommend_only → dry_run → live_action
```

Any forward step is permitted except the `dry_run` → `live_action` transition. That step requires `explicit_operator_approval = true`. If the parameter is false, the transition is refused regardless of other conditions.

This means **no agent can reach live-action status without a documented operator decision**. The default activation for every agent in the canonical manifest is either `disabled`, `observe_only`, `recommend_only`, or `dry_run`. Zero agents ship with `live_action` as their default.

### Rollout Modes

The `AgentActionRolloutProfile` in `agent_rollout_profile.hpp` supports seven rollout modes:

| Mode | Token | Description |
|------|-------|-------------|
| Disabled | `disabled` | No execution. |
| Shadow | `shadow` | Execution occurs but results are not applied; used for correctness comparison. |
| Observe | `observe` | Metric-observation only. |
| Dry run | `dry_run` | Full decision logic; mutations suppressed. |
| Canary | `canary` | Live execution limited to `canary_percent` of eligible subjects, bounded by `canary_max_subjects`. |
| Phased | `phased` | Graduated rollout with a `phased_target_percent` ceiling. |
| Live | `live` | Full live execution. |

`AgentActionRolloutModeAllowsMutation` returns `true` only for `canary`, `phased`, and `live`. All other modes suppress mutation. `AgentActionRolloutModeRequiresDryRun` identifies modes that must complete a dry-run pass first.

The rollout profile carries a `failure_threshold` and `observed_failures` counter. If failures exceed the threshold, `quarantine_on_failure = true` (the default) causes the profile to transition to the `quarantined` rollout state, halting further progression.

### Rollout States

An `AgentActionRolloutProfile` can be in one of seven `AgentActionRolloutState` values: `disabled`, `pending`, `active`, `paused`, `completed`, `failed`, or `quarantined`.

---

## EffectiveActivationForLifecycle: Engine Mode Constraints

`EffectiveActivationForLifecycle(configured, mode)` computes the actual activation level from a configured profile and the current engine lifecycle mode. The effective activation is always the more restrictive of the two.

The `AgentLifecycleMode` values that constrain agent activation include (verified in `agent_runtime.hpp`):

| Mode | Effect on agents |
|------|-----------------|
| `normal` | No restriction; configured activation applies. |
| `backup` | Agents whose actions would interfere with backup consistency are restricted. |
| `restore` | Agents are restricted to observe-only or below during active restore. |
| `crash_recovery` | No agent mutations permitted; observe-only effective. |
| `read_only` | Mutations suppressed; observe and recommend profiles continue. |
| `maintenance` | Only maintenance-relevant agents may proceed; general agents are restricted. |
| `shutdown` | All agents enter stopping mode. |
| `archive_hold` | Archival agents restricted to avoid compounding an active hold. |
| `pitr` | Agents restricted during point-in-time recovery execution. |

Operators do not need to manually pause all agents when the engine enters a restricted lifecycle mode; `EffectiveActivationForLifecycle` handles it automatically. When the mode returns to `normal`, the configured activation resumes.

---

## Tenant Coordination and Metric Quorum

For deployments where multiple agent instances operate over the same tenant scope, `EvaluateAgentTenantWorkloadCoordination` governs admission. The request carries:

- An `AgentTenantCoordinationGroup` describing the group membership, roles, and whether follower live actions are permitted.
- An `AgentTenantWorkloadBudget` (per-tenant limits on live actions, queue depth, memory, worker slots, and I/O bytes).
- A `AgentTenantCoordinationLockRequest` for resources the action needs to hold exclusively.
- A list of `AgentTenantSharedMetricSnapshot` records and `required_metric_families`.
- `required_metric_quorum` — the minimum number of fresh, trusted metric sources that must agree before the coordination decision is admitted (default: **2**).

Coordination roles are `leader`, `follower`, and `observer`. By default, `require_single_leader = true` and `allow_follower_live_actions = false`. A follower can observe and recommend, but cannot execute live mutations unless the group policy explicitly permits it.

The coordination outcome is `admitted`, `queued`, or `refused`. A refused decision produces `fail_closed = true` on the coordination result.

If fewer than `required_metric_quorum` fresh metric sources are available, the coordination decision is refused regardless of other conditions. This is the metric quorum gate: the engine prefers to withhold action rather than act on a potentially stale or split metric picture.

---

## Feature Gates

`EvaluateAgentFeatureAvailability` checks whether a given agent type can run in the current runtime context. The result is one of five `AgentFeatureAvailability` values:

| Value | Token | Meaning |
|-------|-------|---------|
| Available | `available` | The feature is enabled and context requirements are met. |
| Unavailable — disabled stub | `unavailable_disabled_stub` | The agent exists in the manifest but is compiled as a stub. |
| Unavailable — edition | `unavailable_edition` | The current edition does not include this agent's capability. |
| Unavailable — cluster authority | `unavailable_cluster_authority` | The agent requires cluster authority, which is not present. |
| Unavailable — private feature | `unavailable_private_feature` | The feature is gated behind a private build configuration. |

The `InstalledCapabilityRecord` in `agent_feature_gates.hpp` tracks each capability's `edition_scope` (`community`, `private_build`, `enterprise`, `cluster`), its `lifecycle_state` (`installed`, `enabled`, `disabled`, `quarantined`, or `retired`), and whether it requires a parser package. Capability downgrades are rejected by `ValidateCapabilityNoDowngrade`.

Capabilities use an epoch-based policy model. `ValidateCapabilityPolicyEpoch` enforces that the agent's observed policy epoch is not behind the installed record's policy epoch, preventing stale policy from enabling a capability that has since been restricted.

---

## See Also

- [action_lifecycle_and_arbitration.md](#ch-agent-runtime-guide-action-lifecycle-and-arbitration-md) — resource budget in the full action pipeline
- [agent_catalog.md](#ch-agent-runtime-guide-agent-catalog-md) — each agent's default activation profile
- [authority_and_activation_model.md](#ch-agent-runtime-guide-authority-and-activation-model-md) — the five activation profiles and rollout gate
- [maintenance_and_tuning_agents.md](#ch-agent-runtime-guide-maintenance-and-tuning-agents-md) — cleanup scheduler foreground protection detail
- ../Language_Reference/syntax_reference/agent.md (SBsql Language Reference — Syntax, page XXX) — SBsql for adjusting activation profiles and resource budgets
- ../CDE_Concepts/autonomous_operation.md (ScratchBird — Concepts and Getting Started, page XXX) — CDE-level rationale for bounded autonomy




===== FILE SEPARATION =====

<!-- chapter source: Agent_Runtime_Guide/evidence_explainability_and_safety.md -->

<a id="ch-agent-runtime-guide-evidence-explainability-and-safety-md"></a>

# Evidence, Explainability, and Safety

## Purpose

This page describes how ScratchBird makes agent decisions auditable, verifiable, and safe against tampering or replay. It covers the tamper-evident evidence chain and its key-management model, the dry-run and simulation mechanisms, the replay-quarantine system that handles compensation when actions must be revisited, the explainability output that operators can query for any decision, the fault-injection scenario registry used for resilience validation, and the safe-mode and quarantine lifecycle states that constrain compromised instances.

All struct and enum names are verified against `project/src/core/agents/agent_runtime.hpp`, `project/src/core/agents/agent_replay_quarantine.hpp`, and related headers.

---

## The Tamper-Evident Evidence Chain

Every agent decision that produces an observable outcome writes an `AgentEvidenceRecord`. The record is the authoritative audit trail for that decision. Its tamper-protection fields ensure that any post-hoc modification is detectable.

### Chain Structure

Each `AgentEvidenceRecord` carries:

| Field | Purpose |
|-------|---------|
| `tamper_digest` | SHA-256 hash of the record's content fields. |
| `previous_tamper_digest` | Digest of the immediately preceding evidence record for this agent instance, forming a chain. |
| `tamper_chain_digest` | Accumulated chain digest, binding this record into the sequence since instance creation. |
| `tamper_signature` | HMAC-SHA-256 signature over `tamper_chain_digest` using the current key. |
| `tamper_signature_algorithm` | Always `hmac-sha256-v1` (verified in source). |
| `tamper_key_id` | Identifier of the signing key in use. |
| `tamper_key_generation` | Monotonically increasing generation counter for key rotation. |
| `tamper_key_not_before_microseconds` | Epoch timestamp before which the key is not valid. |
| `tamper_key_not_after_microseconds` | Epoch timestamp after which the key is expired. |
| `tamper_key_rotation_epoch` | The rotation boundary this key belongs to. |
| `tamper_key_provenance` | Provenance descriptor for the signing key. |
| `evidence_key_policy_id` | Identifier of the key policy controlling this key's lifecycle. |
| `key_residency_class` | Where the key material resides (for compliance reporting). |
| `data_residency_class` | Data residency category of the evidence record. |
| `tamper_evidence_generation` | Monotone counter incremented on each tamper-chain update. |
| `storage_linkage_digest` | Digest linking this evidence record to the durable storage artifact it describes. |

A verifier examining a chain of evidence records can confirm:

1. Each record's `tamper_digest` matches its content.
2. Each record's `tamper_signature` validates against `tamper_chain_digest` using the key identified by `tamper_key_id` within its `not_before` / `not_after` window.
3. Each record's `previous_tamper_digest` matches the preceding record's `tamper_digest`.
4. The `tamper_chain_digest` accumulates correctly across the sequence.

Any gap or mismatch signals that one or more records were inserted, removed, or modified after creation.

### Key Rotation

Keys rotate on a generation boundary defined by `tamper_key_rotation_epoch`. The `tamper_key_generation` field distinguishes which key signed which records, allowing historic verification even after rotation. The `production_key_material` and `test_key_material` boolean flags distinguish production signing keys from test fixtures; `key_material_exported` is `false` for production keys by design.

### Redaction, Retention, and Legal Hold

Each evidence record carries three compliance-relevant fields:

- `redaction_class` — defaults to `standard`. Controls which fields are visible to which security roles. `RedactAgentEvidenceForSecurity` applies the class before the record leaves the engine.
- `retention_class` — defaults to `operational`. Informs the archive and cleanup agents how long to retain the record.
- `legal_hold_active` — when `true`, the record is exempt from all retention-driven deletion regardless of `retention_class`.

The `protected_material_suppressed` flag (verified in `AgentEvidenceRecord`) indicates that the record was created in a context where protected material was present but has been suppressed from the evidence payload per the applicable redaction policy. The record exists and is chained; its sensitive content is omitted.

`redaction_applied_before_buffering` records whether redaction was applied before the payload was passed to the evidence buffer, relevant for compliance scenarios where material must never appear in an intermediate store even transiently.

---

## Dry-Run Mode

Dry-run mode is a first-class operational posture, not a debug flag. When an agent's `AgentActivationProfile` is `dry_run`:

- The full action-decision pipeline executes (all safety checks, metric evaluations, policy gates, arbitration).
- `BuildDryRunDecision` is called; the resulting `AgentActionDecision` carries `result_class = dry_run_only`.
- An `AgentEvidenceRecord` is written with `result_state = "dry_run_only"` and a full tamper chain entry.
- No mutation is dispatched to the actuator.

This means dry-run evidence accumulates in the same chain as live evidence. Operators reviewing the chain after a transition to live can compare the decision inputs that dry-run produced against the inputs available when live execution later occurred.

The `AgentPolicy.require_dry_run_before_live` gate (described in [authority_and_activation_model.md](#ch-agent-runtime-guide-authority-and-activation-model-md)) enforces that at least one dry-run cycle appears in evidence history before live execution is permitted for policies that require it.

---

## Simulation and Decision Replay

`ReplayAgentDecision` re-executes the decision logic for a given agent type, policy, and context, using a captured set of metric families from a prior point in time. This allows operators to:

- Confirm that a past live decision would have been reached again with the same inputs (correctness oracle use case).
- Test what a policy change would have produced against a historical metric snapshot without modifying any live agent.

Replay does not write live evidence; it produces a `AgentRunDecision` that includes the action decision and explanation lines. Evidence generated during replay is not chained into the main evidence chain unless the replay is explicitly promoted to a quarantine-release operation.

---

## Replay Quarantine: Digest Capture, Compensation, and Release

The replay-quarantine subsystem in `agent_replay_quarantine.hpp` handles the case where an action needs to be revisited after execution — because the outcome was uncertain, the action failed mid-way, or a policy change occurred that renders the original decision invalid.

### Digest Capture

Before any live action with quarantine implications is dispatched, `CaptureAgentReplayDigests` records an `AgentReplayDigestCapture` snapshot. This snapshot includes digests for:

| Captured item | Field |
|---------------|-------|
| Active policy (at action time) | `policy_digest` |
| Metric input snapshot | `metric_digest` |
| Catalog root at decision time | `catalog_root_digest` |
| Security context | `security_digest` |
| Resource reservation state | `resource_reservation_digest` |
| Agent binary package identity | `binary_package_digest` |
| Action input parameters | `action_input_digest` |
| Action evidence record | `action_evidence_digest` |
| Full evidence chain link | `evidence_chain_digest` |

These digests allow a post-hoc reviewer (or the engine itself during recovery) to determine whether any input has changed since the original decision.

### Replay Operations

`ApplyAgentReplayControl` accepts an `AgentReplayControlRequest` with one of five `AgentReplayOperationKind` values:

| Operation | Token | What it does |
|-----------|-------|-------------|
| Mark replay pending | `mark_replay_pending` | Flags the action for review without changing its state. |
| Schedule retry | `schedule_retry` | Schedules a re-attempt after the configured `retry_after_microseconds`. |
| Record compensation | `record_compensation` | Records a compensating action that partially or fully reverses the original. |
| Quarantine | `quarantine` | Moves the action to quarantine; no further execution permitted until released. |
| Release quarantine | `release_quarantine` | Operator-authorized release from quarantine, allowing retry or closure. |

The `AgentReplayControlRequest` explicitly asserts that all authority flags (`parser_authority`, `client_authority`, `reference_authority`, `wal_authority`, etc.) are `false`. A replay operation that inadvertently asserts one of these flags is refused.

---

## Explainability Output

`ExplainAgentDecision(descriptor, policy, decision)` produces a list of human-readable explanation lines for any `AgentActionDecision`. The output describes:

- Which activation profile was in effect and how it constrained the decision.
- Which metric families were consulted and whether they were fresh, trusted, and present.
- Which policy fields influenced the outcome.
- Which arbitration rule resolved the competition (if multiple candidates were present).
- What the resource budget evaluation found.
- The final `AgentActionResultClass` and its reason.

The explanation is surfaced through `AgentRunDecision.explanation_lines` and is also accessible through the `sys.information.*` agent observability projections (see [observability_and_control.md](#ch-agent-runtime-guide-observability-and-control-md)).

Explanation output does not expose redacted evidence fields. A caller without the `obs_agent_evidence_read` right sees explanation lines that describe the decision category and reason code, but not the underlying metric values or policy content that would be subject to redaction.

---

## Fault Injection and the Scenario Registry

The fault-injection subsystem provides a structured set of scenarios for validating that the agent runtime behaves correctly when components fail. `AgentFaultInjectionScenarioDescriptors()` returns the full registry; `EvaluateAgentFaultInjectionScenarioDetailed` runs a named scenario and returns an `AgentFaultInjectionResult`.

### Fault Classes

`AgentFaultInjectionClass` defines six fault categories:

| Class | Token | What it simulates |
|-------|-------|------------------|
| Supervision | `supervision` | Watchdog timeout, tick timeout, or exception during a supervised run. |
| Storage I/O | `storage_io` | I/O failure during evidence persistence or catalog read. |
| Metric input | `metric_input` | Missing, stale, or schema-incompatible metric sample. |
| Policy input | `policy_input` | Missing, invalid, or scope-incompatible policy. |
| Queue integrity | `queue_integrity` | Corruption or gap in the action-queue state. |
| Partial action | `partial_action` | The action dispatched to the actuator but completed only partially. |

### Recovery Responses

Each scenario descriptor carries an `AgentFaultInjectionRecoveryResponse` that specifies the expected recovery behavior:

| Response | Token | What the engine does |
|----------|-------|---------------------|
| Fail closed | `fail_closed` | Default to no-mutation; record evidence; do not proceed. |
| Reject metric sample | `reject_metric_sample` | Discard the suspect metric observation; re-evaluate without it. |
| Reject policy | `reject_policy` | Discard the invalid policy record; use baseline or fail closed. |
| Supervision restart backoff | `supervision_restart_backoff` | Increment restart counter; apply exponential backoff before re-attempting. |
| Supervision quarantine | `supervision_quarantine` | Accumulate failure count; quarantine the instance when threshold is reached. |

The `AgentFaultInjectionResult` confirms whether the scenario produced the expected `AgentActionResultClass`, the expected `AgentLifecycleState` after the fault, and whether `durable_state_changed = false` (confirming no unsafe mutation occurred) and `evidence_recorded_before_success = true` (confirming the evidence was written before the action was reported as successful).

---

## Safe Mode

`safe_mode` is the `AgentLifecycleState` the engine supervisor assigns to an agent instance when a risk condition has been detected but the instance has not yet accumulated enough failures to warrant full quarantine. In safe mode:

- `ValidateAgentSafeMode` restricts the action scope to a reduced set of operations.
- The agent continues to run (observations and recommendations may continue).
- Live mutations are blocked.
- The safe-mode condition is recorded in the evidence chain.

Safe mode is designed to be transient. The supervisor can clear it when the risk condition resolves, returning the instance to its normal lifecycle state.

---

## Quarantine Lifecycle

`quarantined` is a stronger isolation state. An instance reaches quarantine through one of two paths:

1. **Supervision failure accumulation** — tracked via `AgentInstanceRecord.crash_loop_count`, `supervision_failure_count`, and `restart_attempts`. When the threshold is reached, `RecordAgentSupervisionFailure` transitions the instance to `quarantined`.
2. **Explicit quarantine** — an operator or the engine calls `QuarantineAgentInstance` directly, for example in response to a security or integrity alert surfaced by `AgentFaultInjectionClass.supervision`.

In the quarantined state:

- All action requests receive `quarantined` as the `AgentActionResultClass`.
- The instance's run lease is cleared (`lease_cleared = true` in `AgentSupervisionDecision`).
- The `quarantined = true` flag is set on the `AgentInstanceRecord` and persisted.
- Evidence is written recording the quarantine event and the reason.

Release from quarantine requires an explicit operator action (via the SBsql control surface described in ../Language_Reference/syntax_reference/agent.md (SBsql Language Reference — Syntax, page XXX)). Release is not automatic, even if the underlying fault condition has resolved. This ensures that every quarantine/release cycle appears in the evidence history with a human decision record.

---

## See Also

- [action_lifecycle_and_arbitration.md](#ch-agent-runtime-guide-action-lifecycle-and-arbitration-md) — where dry-run and arbitration fit in the action pipeline
- [governance_and_resource_control.md](#ch-agent-runtime-guide-governance-and-resource-control-md) — resource budget and foreground protection
- [observability_and_control.md](#ch-agent-runtime-guide-observability-and-control-md) — how to query evidence and explanation output
- [authority_and_activation_model.md](#ch-agent-runtime-guide-authority-and-activation-model-md) — quarantine in the full 14-state lifecycle
- ../Language_Reference/syntax_reference/agent.md (SBsql Language Reference — Syntax, page XXX) — SBsql for releasing quarantine, reviewing evidence
- ../CDE_Concepts/autonomous_operation.md (ScratchBird — Concepts and Getting Started, page XXX) — CDE-level context for why tamper-evidence matters




===== FILE SEPARATION =====

<!-- chapter source: Agent_Runtime_Guide/maintenance_and_tuning_agents.md -->

<a id="ch-agent-runtime-guide-maintenance-and-tuning-agents-md"></a>

# Maintenance and Tuning Agents

## Purpose

This page provides deep dives into five agents that have rich, verifiable behavior in the source tree. Together they span the full breadth of what autonomous maintenance means in a Convergent Data Engine: managing session pressure, cleaning up stale data across every storage model, running long-horizon index operations safely, tuning performance knobs within strict bounds, and proving backup-readiness before it is needed.

All types and field names are verified against their respective source headers in `project/src/core/agents/`.

---

## Transaction Pressure Manager

**Source:** `project/src/core/agents/agents/transaction_pressure_manager.hpp`
**Agent token:** `transaction_pressure_manager`
**Authority class:** `request_action` — the engine owns the transaction; the agent requests.
**Default activation:** `recommend_only`

### What It Does

Long-idle transactions are one of the most common sources of storage pressure in a multi-generational engine. A transaction that stays open holds a cleanup horizon that prevents all older row versions from being reclaimed, regardless of whether they are still needed. The transaction pressure manager monitors idle transactions and applies an escalation ladder to request engine action.

The key design constraint is explicit in the source: `parser_finality_authority = false` and `client_state_authority = false` on the result struct. The engine owns the transaction. The agent requests.

### The Escalation Ladder

The `TransactionPressureManagerPolicy` defines five idle-time thresholds, all configurable (defaults verified in source):

| Step | Token | Default idle time | What the agent requests |
|------|-------|------------------|------------------------|
| 1 | `warn_notify` | **300 s** (5 min) | Emit a warning notification observable in the agent surface. |
| 2 | `request_restart` | **900 s** (15 min) | Request that the engine offer the session a clean restart. |
| 3 | `request_reauth` | **1 200 s** (20 min) | Request that the engine require the session to re-authenticate. |
| 4 | `request_cancel` | **1 500 s** (25 min) | Request that the engine cancel the idle transaction (requires `request_cancel_allowed = true` in policy, which defaults to `false`). |
| 5 | `force_*` | **1 800 s** (30 min) | Force action (rollback, commit, or restart) — requires explicit `force_authority_gate_present` and `force_authority_gate_allows` in the policy. Force is explicitly off by default. |

Force action requires two independent gates: `force_authority_gate_present` must be true (confirming the gate was provided), and `force_authority_gate_allows` must be true (confirming the gate permits force for this specific context). Missing either gate means the agent stops at `request_cancel` at most.

The decision struct `TransactionPressureManagerTickResult` also carries `denied_non_authoritative` — set when the session or transaction binding is not fully authoritative (for example, during a mid-flight ownership transfer). In that case the agent takes no action regardless of idle time.

For background on the MGA transaction architecture that makes cleanup horizons relevant, see ../Getting_Started/core_concepts/understanding_mga.md (ScratchBird — Concepts and Getting Started, page XXX).

---

## Dynamic Cleanup Debt Scheduler

**Source:** `project/src/core/agents/dynamic_cleanup_debt_scheduler.hpp`
**Used by:** `storage_version_cleanup_agent` and the broader cleanup subsystem.
**Authority class (cleanup agents):** `direct_bounded_action`
**Default activation (cleanup agents):** `dry_run`

### What It Does

`PlanDynamicCleanupDebt` is a prioritization and work-selection surface. Given a list of `DynamicCleanupDebtSource` records and a `DynamicCleanupDebtSchedulerPolicy`, it selects which cleanup items to schedule, how many work units to allocate, and which to defer or refuse — all without executing the cleanup itself. Execution happens through the actuator after the plan is approved by the full agent action pipeline.

Cleanup respects the MGA cleanup horizon: `requires_mga_cleanup_horizon = true` on most source records means the scheduler will not schedule cleanup beyond what the authoritative cleanup horizon service has confirmed is safe.

### The 12 Cleanup Debt Families

This is one of the clearest structural proofs that ScratchBird is a true convergent engine: a single cleanup scheduler manages debt across every data model in use.

| # | Family | Token | What it cleans |
|---|--------|-------|----------------|
| 1 | Version chain | `version_chain` | Obsolete row versions from the relational MGA version chain. |
| 2 | Exact index leaf | `exact_index_leaf` | Dead entries in exact (B-tree style) index leaf pages. |
| 3 | Secondary delta ledger | `secondary_delta_ledger` | Pending merges and tombstones in secondary index change buffers. |
| 4 | Summary page range | `summary_page_range` | Stale page-range summaries used for aggregate push-down. |
| 5 | Large value | `large_value` | Orphaned large-value (overflow) storage from dropped rows or updated blobs. |
| 6 | Hot leaf | `hot_leaf` | Pressure relief for hot B-tree leaf pages accumulating write contention. |
| 7 | NoSQL key-value | `nosql_key_value` | TTL expiry and generation compaction for the key-value model. |
| 8 | NoSQL document | `nosql_document` | Generation merges for the document model. |
| 9 | NoSQL search | `nosql_search` | Segment merges for the full-text search model. |
| 10 | NoSQL vector | `nosql_vector` | Generation retirement for the approximate-search vector model. |
| 11 | NoSQL graph | `nosql_graph` | Adjacency list compaction for the graph model. |
| 12 | NoSQL time series | `nosql_time_series` | Bucket retirement for the time-series model. |

Families 1–6 cover relational and shared storage structures. Families 7–12 cover each NoSQL model family supported by the engine. All 12 are scheduled by the same planner under the same foreground-protection and policy-budget rules.

### Scheduling Policy

`DynamicCleanupDebtSchedulerPolicy` controls:

- `max_total_work_units` (default 64) — total work units the scheduler may commit in one tick.
- `max_scheduled_items` (default 8) — maximum number of debt items scheduled per tick.
- `default_max_family_work_units` (default 16) — per-family work-unit cap (also configurable per family via `DynamicCleanupDebtFamilyCap`).
- `default_max_family_items` (default 2) — per-family item count cap.
- `protect_foreground_work = true` — suspends all scheduling when foreground activity is detected.
- Retry backoff range: 1 s minimum to 60 s maximum.
- Lease duration: 5 s (to prevent duplicate scheduling when workers overlap).

Each scheduled item receives a lease token and a next-eligible timestamp. If the lease is still active when the scheduler runs again, the item is deferred with `deferred_lease` rather than double-scheduled.

Failure modes are explicit: `fail_closed_retain_debt` (the default for most families) means the scheduler refuses to proceed when its source data is not authoritative rather than cleaning up data it cannot prove is safe to remove.

---

## Online Maintenance Progress

**Source:** `project/src/core/agents/online_maintenance_progress.hpp`, `project/src/core/agents/vector_maintenance_jobs.hpp`
**Used by:** index rebuild, vector maintenance, nosql compaction, optimizer stats refresh, and other long-horizon operations.

### What It Does

Long-running maintenance operations — rebuilding a vector index, refreshing optimizer statistics, compacting a large NoSQL segment — cannot be atomic. They run over time, may need to be cancelled, and must survive crashes without repeating completed work. `OnlineMaintenanceStateStore` and the functions that operate on it provide a crash-safe checkpoint-and-resume mechanism for these operations.

### Operation Phases

An `OnlineMaintenanceProgressSnapshot` moves through `OnlineMaintenancePhase` values:

| Phase | Token | Description |
|-------|-------|-------------|
| Requested | `requested` | Operation admitted; not yet running. |
| Running | `running` | Active; progress being recorded. |
| Cancel requested | `cancel_requested` | Operator or policy has requested cancellation; drain in progress. |
| Cancelled | `cancelled` | Cleanly cancelled; checkpoint written. |
| Resumable | `resumable` | Operation was cancelled or crashed but left a valid checkpoint. |
| Publish ready | `publish_ready` | Work is complete but the result has not yet been made visible. |
| Published | `published` | Result is visible; resources released. |
| Completed | `completed` | Full lifecycle done. |
| Failed closed | `failed_closed` | Unrecoverable error; checkpoint preserved for review. |

The `COMPLETED_UNPUBLISHED` pattern (evident from the `kOnlineMaintenanceCompletedUnpublished` constant in source) is a first-class phase: work completes successfully and the result is validated before being made visible. `PublishOnlineMaintenanceOperation` requires `authoritative_generation_validated = true` before transitioning to `published`. If this flag is not present, the publish is refused with `kOnlineMaintenanceUnsafePublishRefused`.

This validate-before-publish gate is critical for approximate-search vector indexes: a rebuilt index is not made available for search queries until the engine has verified the new training generation against the previous one. Partial visibility is controlled by `no_partial_visibility` in the publish request.

### Cancel and Resume

`CancelOnlineMaintenanceOperation` checkpoints the current progress before stopping. The checkpoint is crash-durable when `durable_checkpoint_persisted = true`. On restart, `RecoverOnlineMaintenanceOperation` reads the checkpoint and returns a `recovered_resumable` decision, allowing `ResumeOnlineMaintenanceOperation` to pick up from where the operation stopped rather than restarting from the beginning.

`kOnlineMaintenanceUnsafeResumeRefused` is raised if the checkpoint is structurally invalid or if the MGA cleanup horizon has advanced past the point the operation assumed when it was checkpointed — in which case the operation must restart rather than resume to avoid inconsistency.

### Vector-Specific Maintenance

`VectorMaintenanceJobRequest` in `vector_maintenance_jobs.hpp` integrates with `OnlineMaintenanceProgressSnapshot` for the three vector maintenance action kinds:

- `adaptive_tuning` — adjusting approximate-search parameters without rebuilding.
- `retrain` — retraining the index with updated data distribution.
- `rebuild` — full index rebuild with validate-before-publish.

`VectorMaintenancePublishState` tracks whether a rebuilt vector index is `waiting_validation`, `publish_after_validation`, `published`, or `refused`. The `runtime_correctness_unproven` failure class (`VectorMaintenanceFailureClass`) prevents publication of an index whose correctness properties cannot be verified against the current engine state.

---

## Adaptive Tuning Controller

**Source:** `project/src/core/agents/adaptive_tuning_controller.hpp`
**Used by:** agents that adjust bounded performance knobs based on observed metric evidence.

### What It Does

The adaptive tuning controller selects a value for one tuning knob at a time, using current metric evidence and resource governance state as inputs. It is strictly advisory: `advisory_only = true` in the result struct, and the safety policy explicitly denies all authority that would make it non-advisory.

### The Eight Tunable Knobs

`AdaptiveTuningKnob` defines eight knobs (verified in source):

| Knob | Token | What it controls |
|------|-------|-----------------|
| Prefetch depth | `kPrefetchDepth` | Read-ahead depth for sequential I/O patterns. |
| Merge workers | `kMergeWorkers` | Number of concurrent segment-merge workers. |
| Refresh interval | `kRefreshInterval` | How frequently a materialized or cached structure is refreshed. |
| Candidate budget | `kCandidateBudget` | Number of candidates evaluated during a selection or pruning step. |
| Cache partition | `kCachePartition` | Relative size of a cache partition for a given workload class. |
| Evidence sample rate | `kEvidenceSampleRate` | Fraction of decision events that produce a full evidence record. |
| Vector ef_search | `kVectorEfSearch` | The ef_search parameter for approximate nearest-neighbor search (HNSW-style traversal width). |
| Vector nprobe | `kVectorNprobe` | The nprobe parameter for partition-based approximate search (IVF-style probe count). |

The `kVectorEfSearch` and `kVectorNprobe` knobs are particularly notable: they allow the engine to adaptively balance recall quality against query latency for approximate-search workloads without requiring operator intervention or index rebuilds.

### Actions

`AdaptiveTuningActionClass` defines six outcomes per evaluation:

| Action | Token | Meaning |
|--------|-------|---------|
| Refuse | `kRefuse` | Safety or proof constraints prevent any change. |
| Hold | `kHold` | Evidence insufficient to recommend a direction; retain current value. |
| Increase | `kIncrease` | Metric evidence supports raising the knob value. |
| Decrease | `kDecrease` | Metric evidence supports lowering the knob value. |
| Reset | `kReset` | Return to the last operator-set value. |
| Default | `kDefault` | Return to the compiled-in default value. |

### The Authority-Denying Safety Policy

`AdaptiveTuningSafetyPolicy` is the most explicit statement of bounded authority in the adaptive tuning subsystem. All of the following fields are `false` by design:

- `parser_or_reference_authority` — the controller cannot affect parser behavior or reference resolution.
- `provider_transaction_finality_authority` — the controller cannot affect whether transactions commit or abort.
- `provider_visibility_authority` — the controller cannot change what rows or versions are visible.
- `client_autocommit_authority` — the controller cannot affect client autocommit semantics.
- `wal_recovery_authority` — the controller cannot influence WAL replay or crash recovery.

Additionally:

- `mga_recheck_required = true` — any knob change must be rechecked against the MGA state before the change is applied.
- `security_recheck_required = true` — a security policy recheck is mandatory before applying a change.
- `engine_mga_authoritative = false` — the controller does not assert MGA authority.

Any request that sets any of these authority flags to `true` is refused.

The controller selects from a bounded `[min_value, max_value]` range with a known `default_value`. It cannot produce a value outside this range, and the range is validated against the `semantics_neutral_proof` field: a proof that changing the knob within the range does not alter the logical meaning of any query result.

---

## Restore Drill Manager

**Source:** `project/src/core/agents/agents/restore_drill_manager.hpp`
**Agent token:** `restore_drill_manager`
**Authority class:** `request_action`
**Default activation:** `recommend_only`

### What It Does

Backup systems fail in the one moment they matter: when a restore is attempted. The restore drill manager automates restore-readiness verification by running isolated restore drills on a schedule, so that the evidence of a successful restore exists in the audit record before it is urgently needed.

### The Four Preconditions

`RestoreDrillManagerRequest` carries four boolean preconditions that must all be true before the manager will produce a `run_restore_drill` decision:

| Precondition | Field | What it confirms |
|-------------|-------|-----------------|
| Target isolated | `target_isolated` | The restore target is isolated from production data; a drill cannot contaminate live data. |
| Backup manifest available | `backup_manifest_available` | A valid backup manifest exists and is reachable; the drill has something to restore from. |
| Restore inspection open | `restore_inspection_open` | The inspection surface is open, allowing the drill to verify the restored state. |
| Intended state observed | `intended_state_observed` | After the restore, the intended catalog/data state was actually observed in the restored target. |

If any of these four is false, the decision is `refused` with `fail_closed = true`. The manager will not claim a drill succeeded unless all four conditions are independently verifiable.

This four-gate model means the restore drill manager produces `run_restore_drill` outcomes only when all of the following are provably true: there is something to restore from, the restore target is safe to use, the inspection channel is open, and the result matches expectations. The evidence record for a successful drill is a direct attestation of all four.

### Workflow Integration

The drill result integrates with `AgentLocalWorkflowRecord`. Each drill attempt — including refused attempts — produces a workflow record, ensuring that both successes and refused attempts appear in the observable evidence stream. Operators can query the frequency of refused drills to identify configuration gaps (for example, a backup manifest that is chronically unavailable or a restore target that is never isolated on schedule).

---

## See Also

- [agent_catalog.md](#ch-agent-runtime-guide-agent-catalog-md) — authority class and default activation for each of these agents
- [governance_and_resource_control.md](#ch-agent-runtime-guide-governance-and-resource-control-md) — foreground protection and resource budgets that bound all maintenance work
- [evidence_explainability_and_safety.md](#ch-agent-runtime-guide-evidence-explainability-and-safety-md) — how maintenance evidence is chained and made tamper-evident
- ../Getting_Started/core_concepts/understanding_mga.md (ScratchBird — Concepts and Getting Started, page XXX) — MGA cleanup horizon, which the cleanup scheduler respects
- ../CDE_Concepts/autonomous_operation.md (ScratchBird — Concepts and Getting Started, page XXX) — why convergent self-maintenance across all model families matters




===== FILE SEPARATION =====

<!-- chapter source: Agent_Runtime_Guide/observability_and_control.md -->

<a id="ch-agent-runtime-guide-observability-and-control-md"></a>

# Observability and Control

## Purpose

This page describes how operators can observe agent state and influence agent behavior. It covers the `sys.information.*` agent projections and the `sys.agents` surface that expose runtime health, metric dependency freshness, policy bindings, pending actions, and (redacted) evidence. It then summarizes the levers operators hold: activation control, policy management, override management, manual approval, and quarantine release.

SBsql syntax for exercising these levers is not restated here — see ../Language_Reference/syntax_reference/agent.md (SBsql Language Reference — Syntax, page XXX). This page explains what the surfaces expose and what operators can do with them.

All type names and field names are verified against `project/src/engine/internal_api/catalog/sys_information_projection.hpp` and `project/src/sys/agents_views.cpp`.

---

## The Observability Architecture

Agent observability in ScratchBird is implemented through two distinct layers:

1. **`sys.information.*` agent projections** — standard-style information-schema views that expose agent state through the engine's normal query surface. These follow the same redaction, authorization, and MGA-snapshot-visibility rules as all other `sys.information` views.
2. **`sys.agents` and `cluster.sys.agents`** — engine-owned surfaces implemented by `EngineSysAgents` and `EngineClusterSysAgents` (anchored in `src/sys/agents_views.cpp`). These surfaces provide direct agent management capabilities alongside observation.

Both layers are authorization-filtered. A caller without the appropriate right sees only what is visible at their grant level; sensitive fields are redacted, not omitted entirely, so that operators can see that a field exists but cannot read its content without the required right.

---

## SysInformationSourceKind: Agent Projection Types

The `SysInformationSourceKind` enumeration defines the five agent-specific source kinds that feed `sys.information.*` views (verified in `sys_information_projection.hpp`):

| Source kind | Token | What it exposes |
|-------------|-------|----------------|
| Agent runtime | `agent_runtime` | Per-agent health state, lifecycle state, activation profile, policy binding, queue depth, action backlog, failure count, quarantine count, last decision, and last evidence UUID. |
| Agent metric dependency | `agent_metric_dependency` | Per-agent metric dependency contracts — which metric families each agent requires, whether they are required or optional, the freshness limit, current freshness, quality state, and fail behavior. |
| Agent policy | `agent_policy` | Policy bindings — which policy families are attached to each agent, their attachment UUIDs, active/valid states, and catalog generation. |
| Agent action | `agent_action` | Pending and recent actions — action UUID, action ID, agent, state, risk class, approval-required flag, actor, and diagnostic code. |
| Agent evidence | `agent_evidence` | Evidence records — evidence UUID, type, associated action, redaction class, creation time, actor, payload digest, and whether the payload is redacted (`payload_redacted = "YES"` by default). |
| Storage agent state | `storage_agent_state` | Specialized state projections for storage agents (filespace capacity, page allocation). Provides filespace health state, last metric timestamps, last recommendation and refusal codes. |

---

## What Operators See Per Projection

### Agent Runtime State (`agent_runtime`)

The `SysInformationAgentSource` struct (the backing source for `agent_runtime` projections) exposes:

- `agent_type_id` — the canonical agent type token (e.g. `transaction_pressure_manager`).
- `scope_kind` and `scope_uuid` / `scope_ref` — which scope the instance is bound to.
- `state` — the observable lifecycle state (one of the 14 in `AgentLifecycleState`).
- `health_state` — derived health summary.
- `enabled` — `"YES"` or `"NO"`.
- `policy_uuid` / `policy_name` — the active policy.
- `last_transition_at` — when the current state was entered.
- `last_diagnostic_code` — the most recent diagnostic code, if any.
- `last_evidence_uuid` — the UUID of the most recent evidence record emitted.
- `last_decision` — human-readable description of the last decision outcome.
- `retry_not_before` — when the agent is next eligible to run (after backoff or cooldown).
- `queue_depth` and `action_backlog` — current work queue state.
- `failure_count` and `quarantine_count` — cumulative failure and quarantine history.
- `overhead_budget_units` — current overhead budget consumption.
- `diagnostic_redaction_state` — whether the diagnostic fields are redacted for this caller.

Callers without `obs_agent_state_read` see the row but with sensitive fields suppressed (the `hidden` flag controls row-level visibility for restricted scopes).

### Metric Dependency Freshness (`agent_metric_dependency`)

The `SysInformationAgentMetricDependencySource` exposes per-dependency rows:

- `metric_family` and `metric_namespace` — which metric family the dependency refers to.
- `required_or_optional` — whether this dependency is required or optional.
- `freshness_limit` — the maximum age the engine will accept for this metric.
- `current_freshness` — the age of the most recent observation.
- `quality_state` — `"fresh"`, `"stale"`, `"missing"`, or other states reflecting the current metric health.
- `fail_behavior` — what happens when this dependency is not satisfied (typically `"fail_closed"`).
- `metric_values_visible` — whether the metric values themselves are visible to this caller (metric values may require `sec_auth_metrics_read`).

This view is the primary diagnostic surface for the most common agent health problem: an agent that is `failed_closed` because a required metric family is stale or absent. Operators can identify exactly which metric family is the bottleneck and investigate the metric source.

### Policy Bindings (`agent_policy`)

The `SysInformationAgentPolicySource` exposes:

- `policy_family`, `policy_uuid`, and `policy_name` — the attached policy.
- `active_state` — whether the attachment is currently active.
- `validation_state` — whether the policy passed its last validation run.
- `attached_at` and `attached_by` — when and by whom the policy was attached.
- `catalog_generation_id` — the catalog generation at which the binding is recorded.

Callers require `obs_policy_read` to see policy body content. Callers without it see the binding metadata but not the policy configuration fields.

### Pending Actions (`agent_action`)

The `SysInformationAgentActionSource` exposes:

- `action_uuid` and `action_id` — the specific action candidate.
- `agent_uuid` and `agent_ref` — which agent submitted the action.
- `state` — e.g. `"recommended"`, `"approval_required"`, `"accepted"`, `"suppressed"`.
- `risk_class` — the arbitration risk class for this action.
- `approval_required` — `"YES"` or `"NO"`.
- `actor_uuid` / `actor_ref` — the principal associated with the action (visible only to callers with sufficient right).
- `diagnostic_code` — reason code if the action is in a non-accepted state.
- `expires_at` — when the action record expires from the observable surface.

Actions with `approval_required = "YES"` are the primary surface for the manual approval workflow. Operators query this view to identify actions awaiting approval, then use the SBsql approval command to proceed or cancel.

### Evidence Records (`agent_evidence`)

The `SysInformationAgentEvidenceSource` exposes:

- `evidence_uuid` and `evidence_type` — unique identifier and type of the evidence record.
- `action_uuid` / `action_ref` — the action this evidence is associated with, if any.
- `redaction_class` — the redaction class applied (defaults to `"summary"` in the projection source, meaning only a summary view is presented without `obs_agent_evidence_read`).
- `payload_digest` — the digest of the evidence payload.
- `payload_redacted` — `"YES"` by default; `"NO"` only for callers with the appropriate evidence-read right.
- `created_at` and `actor_uuid` / `actor_ref` — when the evidence was created and by which agent/principal (actor visibility gated by right).

The evidence projection is intentionally limited by default. The digest is always present, allowing external verifiers to confirm evidence continuity without needing the full payload. The full payload requires `obs_agent_evidence_read`.

---

## The `sys.agents` Surface

`sys.agents` and `cluster.sys.agents` are implemented by engine-owned surfaces (`EngineSysAgents` and `EngineClusterSysAgents`) and provide direct management alongside observation. The management interface is accessed through SBsql agent statements (see ../Language_Reference/syntax_reference/agent.md (SBsql Language Reference — Syntax, page XXX)).

The surface gives operators:

- **Activation control** — view current activation profiles and request profile transitions.
- **Policy management** — attach, detach, validate, and version-bump policies.
- **Override management** — create, renew, and deactivate `AgentArbitrationOverride` records.
- **Manual approval** — approve or cancel actions in the `approval_required` state.
- **Quarantine release** — release instances from the `quarantined` lifecycle state.
- **Explanation queries** — retrieve the `ExplainAgentDecision` output for any recent decision.
- **Fault injection** — inject a named fault scenario for resilience testing (requires appropriate right; not available in all edition scopes).

---

## Operator Levers Summary

| Lever | What it does | Right required |
|-------|-------------|----------------|
| View agent state | Query `sys.information.*` agent projections for lifecycle state, health, and last decision. | `obs_agent_state_read` |
| Read evidence | View the full evidence payload for agent decisions. | `obs_agent_evidence_read` |
| Read recommendations | View pending recommendations from `recommend_only` agents. | `obs_agent_recommendation_read` |
| Control agent | Change activation profile, pause, resume, disable. | `obs_agent_control` |
| Approve action | Approve an action in the `approval_required` queue. | `obs_agent_action_approve` |
| Cancel action | Cancel a pending or queued action. | `obs_agent_action_cancel` |
| Create/deactivate override | Create a suppression or force-allow override; deactivate an existing one. | `obs_agent_override` |
| Read policy | View policy configuration attached to agents. | `obs_policy_read` |
| Simulate policy | Run a policy decision simulation without applying it. | `obs_policy_simulate` |
| Apply policy | Attach or update a policy binding. | `obs_policy_apply` |
| Rollback policy | Revert a policy to a previous generation. | `obs_policy_rollback` |
| Delete policy | Remove a policy record. | `obs_policy_delete` |
| Read cluster health | View cluster-scope agent health. | `obs_cluster_health_inspect` |
| Cluster control | Exercise cluster-scope agent management. | `obs_cluster_control` |
| Read auth metrics | View authentication-related metrics (session security surface). | `sec_auth_metrics_read` |
| Edit redaction policy | Modify evidence redaction policy. | `sec_redaction_policy_edit` |

These rights correspond to the `AgentSecurityRight` enumeration in `agent_runtime.hpp` and are enforced by `EvaluateAgentCommandGrant`.

---

## Activation Control: Moving Through the Rollout Profile

Operators change an agent's activation profile using the SBsql ALTER AGENT statement. The profile transition is validated by `ValidateRolloutTransition`. The critical constraint — moving from `dry_run` to `live_action` requires `explicit_operator_approval = true` — means that the SBsql statement must carry an explicit approval acknowledgement; the engine refuses the transition if it is absent.

`EffectiveActivationForLifecycle` computes the actual activation at runtime. Operators querying the agent state surface see both the configured profile and the effective activation. If these differ (for example, because the engine is in `read_only` lifecycle mode), the effective activation explains why an agent that appears configured for live-action is currently running as recommend-only.

---

## Diagnosing Common Issues

### Agent is `failed_closed` and not running

1. Query `agent_runtime` projection: check `last_diagnostic_code`.
2. Query `agent_metric_dependency` projection: look for a metric family with `quality_state = "stale"` or `"missing"`.
3. Investigate the metric source for that family. If the metric provider itself is unhealthy, resolve that first.
4. If the diagnostic code points to a policy issue, query `agent_policy` projection: look for `validation_state = "invalid"` or `active_state = "inactive"`.

### Agent is `approval_required` and stalled

1. Query `agent_action` projection: find actions with `approval_required = "YES"`.
2. Use the SBsql approval command (see ../Language_Reference/syntax_reference/agent.md (SBsql Language Reference — Syntax, page XXX)) to approve or cancel.
3. If the action has `risk_class = "critical"` or `risk_class = "high"`, review the `agent_evidence` projection for the associated dry-run evidence before approving.

### Agent is `quarantined`

1. Query `agent_runtime` projection: note `quarantine_count` and `failure_count`.
2. Query `agent_evidence` projection: read the quarantine-event evidence to understand what failure class triggered it.
3. Resolve the underlying cause (metric staleness, policy invalidity, actuator degradation).
4. Use the SBsql quarantine-release command to return the instance to `registered` state. A new run cycle begins after the configured cooldown.

---

## See Also

- [authority_and_activation_model.md](#ch-agent-runtime-guide-authority-and-activation-model-md) — the lifecycle state machine and activation profiles
- [action_lifecycle_and_arbitration.md](#ch-agent-runtime-guide-action-lifecycle-and-arbitration-md) — how actions enter the `approval_required` state
- [evidence_explainability_and_safety.md](#ch-agent-runtime-guide-evidence-explainability-and-safety-md) — evidence chain structure, quarantine lifecycle
- [governance_and_resource_control.md](#ch-agent-runtime-guide-governance-and-resource-control-md) — resource budget decisions that appear in the diagnostic surface
- ../Language_Reference/syntax_reference/agent.md (SBsql Language Reference — Syntax, page XXX) — complete SBsql syntax for all agent control operations
- ../CDE_Concepts/autonomous_operation.md (ScratchBird — Concepts and Getting Started, page XXX) — CDE-level rationale for the observability design




# Acceleration Guide




===== FILE SEPARATION =====

<!-- chapter source: Acceleration_Guide/README.md -->

<a id="ch-acceleration-guide-readme-md"></a>

# ScratchBird Acceleration Guide

**Status: draft — subject to revision before stable release.**

## Purpose

This guide describes the layered execution-speed stack in ScratchBird, a
Convergent Data Engine (CDE). ScratchBird can accelerate query and computation
workloads through superinstruction fusion, LLVM-backed native compilation
(JIT and AOT), and GPU/SIMD scoring kernels. All of these mechanisms operate
under a single unifying safety principle described in this guide as the
**candidate-accelerator principle**.

Understanding acceleration at this level is not required for ordinary query
authoring or administration. It is intended for:

- Advanced operators tuning workloads or managing compiled artifacts
- Integrators building or certifying acceleration providers
- Security reviewers auditing the boundary between accelerated and authoritative
  execution paths

---

## The Candidate-Accelerator Principle

Acceleration in ScratchBird rests on one structural invariant:

> **Accelerators are candidates. The exact SBLR interpreter reference path is
> always the source of truth.**

Every acceleration layer — superinstruction fusion, native JIT/AOT compilation,
GPU/SIMD scoring kernels — produces candidate output. That output is only used
when it can be verified or substituted safely. If an accelerator is unavailable,
fails, is quarantined, or is disabled by policy, the engine falls back to the
interpreter. No query result, transaction outcome, security decision, or catalog
state ever depends exclusively on an accelerated path.

This is enforced by five structural properties, each described in detail in
[acceleration_authority_model.md](#ch-acceleration-guide-acceleration-authority-model-md):

1. **Never authority.** No accelerator holds semantic, transaction, visibility,
   security, redaction, recovery, parser, catalog, or cluster-decision authority.
2. **Exact reference is truth.** The scalar SBLR interpreter result is the
   correctness oracle. Accelerated paths are verified against it or replaced by it.
3. **Opt-in.** All acceleration is disabled by default. Operators explicitly
   enable it through policy profiles.
4. **Policy-gated.** Each tier has named policy profiles that control whether
   acceleration may run, is required, or is refused.
5. **Epoch-bound and fail-closed.** Compiled artifacts carry multi-epoch cache
   keys. Any change to a security, catalog, policy, or opcode-registry epoch
   invalidates dependent artifacts. On any verification failure or ambiguity,
   the engine fails closed and uses the interpreter.

---

## The Three Execution Tiers

ScratchBird's execution stack has three tiers, each building on the one below.

| Tier | Mechanism | Source reference |
|------|-----------|-----------------|
| 1 | SBLR interpreter — always available; semantic authority | `src/engine/sblr/sblr_runtime.hpp` |
| 2 | Superinstruction fusion and batch dispatch | `src/engine/sblr/sblr_hot_path_execution.hpp` |
| 3 | LLVM JIT/AOT native compilation; GPU/SIMD scoring kernels | `src/engine/native_compile/`, `src/engine/gpu_acceleration/` |

Tier 1 is never absent. Tiers 2 and 3 are optional accelerators. The engine
always has a Tier 1 path available and uses it whenever a higher tier is
unavailable, disabled, or fails verification.

---

## Guide Structure

| Document | Contents |
|----------|----------|
| [acceleration_authority_model.md](#ch-acceleration-guide-acceleration-authority-model-md) | The unifying safety model: authority boundaries, epoch binding, lowerability bans, fail-closed refusal |
| [execution_tiers.md](#ch-acceleration-guide-execution-tiers-md) | How the three tiers work, hotness detection, escalation, and fallback |
| [native_compilation.md](#ch-acceleration-guide-native-compilation-md) | LLVM JIT/AOT: policy profiles, cache key dimensions, AOT artifact format, specialization kinds |
| [gpu_and_simd_acceleration.md](#ch-acceleration-guide-gpu-and-simd-acceleration-md) | GPU gate, scoring kernels, policy profiles, refusal codes, SBsql surface |
| [operating_acceleration.md](#ch-acceleration-guide-operating-acceleration-md) | Operator workflow: enabling, quarantine, cache management, inspection, diagnostics |

---

## Cross-Links

- **Up (CDE concepts):** ../CDE_Concepts/adaptive_acceleration.md (ScratchBird — Concepts and Getting Started, page XXX)
- **EBNF grammar pages:**
  - ../Language_Reference/syntax_reference/ebnf/acceleration_statement.md (SBsql Language Reference — Syntax, page XXX)
  - ../Language_Reference/syntax_reference/ebnf/alter_acceleration.md (SBsql Language Reference — Syntax, page XXX)
  - ../Language_Reference/syntax_reference/ebnf/show_acceleration.md (SBsql Language Reference — Syntax, page XXX)
  - ../Language_Reference/syntax_reference/ebnf/alter_management.md (SBsql Language Reference — Syntax, page XXX)
- **SBLR envelope:** ../Embedding_API_Reference/sblr_envelope.md (ScratchBird — Application Development and Integration, page XXX)
- **Operations administration:** [../Operations_Administration/README.md](#ch-operations-administration-readme-md)
- **Security:** [../Security_Guide/trust_and_separation_architecture.md](#ch-security-guide-trust-and-separation-architecture-md)




===== FILE SEPARATION =====

<!-- chapter source: Acceleration_Guide/acceleration_authority_model.md -->

<a id="ch-acceleration-guide-acceleration-authority-model-md"></a>

# Acceleration Authority Model

**Status: draft — subject to revision before stable release.**

## Purpose

This document defines the authority model that governs every tier of
ScratchBird's acceleration stack. All acceleration mechanisms — superinstruction
fusion, LLVM JIT/AOT compilation, native SBLR specialization, and GPU/SIMD
scoring kernels — share this model. Understanding it is prerequisite to reading
the tier-specific documents.

The model answers one question: **when can the engine trust an accelerated
result instead of the interpreter?**

The short answer is: it can use an accelerated result when the accelerator is
not claiming any authority that belongs exclusively to the reference path, when
all epoch bindings match, and when the policy profile permits it. If any of
these conditions fails, the engine fails closed and uses the interpreter.

---

## The Candidate-Accelerator Principle

The source comment in
`src/engine/native_compile/native_compile.hpp` states the principle precisely:

> Native compilation is acceleration evidence only. SBLR interpreter semantics
> remain authoritative for values, diagnostics, transactions, MGA visibility,
> security, and side effects.

A parallel statement appears in
`src/engine/gpu_acceleration/scoring_kernel_acceleration.hpp`:

> SIMD/GPU scoring kernels are optional accelerators over materialized,
> authorized executor batches. Exact scalar executor evaluation remains the
> authority for results, transaction finality, MGA visibility, security,
> redaction, parser boundaries, reference compatibility, recovery, pages, and
> catalog state.

The GPU acceleration header (`src/engine/gpu_acceleration/gpu_acceleration.hpp`)
makes this concrete with a structured comment:

> GPU is never semantic, security, transaction, MGA, catalog, cleanup, recovery,
> parser, or cluster-decision authority.

These are not aspirational statements. They are enforced through the refusal
codes and manifest fields described below.

---

## Authority Boundaries by Domain

The following domains always belong to the reference path (SBLR interpreter and
engine-owned subsystems). No accelerator may claim, influence, or replace any
of these:

| Domain | What it means | Accelerator role |
|--------|---------------|-----------------|
| Semantic values | The correct output of any expression or query | Candidate — must match reference |
| Transaction finality | Whether a commit or rollback takes effect | Forbidden entirely |
| MGA visibility | Which data version is visible in a snapshot | Forbidden entirely |
| Security policy | Grant checks, privilege enforcement | Forbidden entirely |
| Redaction policy | Column masking, row filtering for redaction | Forbidden entirely |
| Parser authority | Parsing, AST construction, SQL text interpretation | Forbidden entirely |
| Reference compatibility | Determining what constitutes a correct result | Forbidden entirely |
| Recovery | Crash recovery, WAL application, redo | Forbidden entirely |
| Page or catalog access | Direct storage page reads, catalog mutation | Forbidden entirely |
| Cluster placement | Which node handles a workload | Forbidden entirely |

These boundaries are enforced structurally. The `ScoringKernelProviderManifest`
struct (verified in `src/engine/gpu_acceleration/scoring_kernel_acceleration.hpp`)
carries explicit boolean fields that a provider must set to `false`:

- `claims_transaction_finality_authority`
- `claims_visibility_authority`
- `claims_security_policy_authority`
- `claims_redaction_policy_authority`
- `claims_parser_or_reference_authority`
- `claims_recovery_authority`
- `claims_page_or_catalog_authority`

The same fields appear in `NativeSblrProviderManifest`
(`src/engine/native_compile/native_sblr_specialization.hpp`):

- `claims_transaction_finality_authority`
- `claims_visibility_authority`
- `claims_security_policy_authority`
- `claims_redaction_policy_authority`
- `claims_parser_or_reference_authority`

A provider manifest with any of these set to `true` is rejected.

---

## Exact Scalar Reference Verification

Every accelerator path carries a `scalar_reference` — a callable that produces
the exact interpreter result for any input batch. This reference is supplied by
the engine, not the provider.

For scoring kernels (`ScoringKernelRequest.scalar_reference`), the reference
function is of type `ScoringKernelScalarReference`, defined as:

```
std::function<ScoringKernelValueBatch(const ScoringKernelInputBatch&)>
```

For native SBLR specialization (`NativeSblrSpecializationRequest.scalar_reference`):

```
std::function<NativeSblrValueBatch(const NativeSblrInputBatch&)>
```

The engine uses this reference to verify accelerator output or to substitute
the correct result when the accelerator path is unavailable. The
`ScoringKernelResult.fail_closed` field records whether the engine fell back
to the reference after an accelerator failure.

Batch dispatch at Tier 2 verifies the result against a contract hash:
`SblrHotPathBatchPlan.result_contract_hash_matches` must be `true` before a
batched result is accepted as equivalent to the scalar interpretation.

---

## Epoch Binding and Artifact Invalidation

Compiled native artifacts (JIT modules and AOT artifacts) are bound to a
multi-epoch cache key. The key is computed in `native_compile.cpp` as a SHA-256
digest over the following dimensions (verified in `CacheKeyMaterial`):

| Dimension | Field |
|-----------|-------|
| SBLR module payload digest | SHA-256 of `module_payload` |
| SBLR version string | `sblr_version` (e.g., `sblr_v3`) |
| Opcode registry epoch | `opcode_registry_epoch` (e.g., `static_v3`) |
| Target object UUID | `target_object_uuid` |
| Principal UUID | `principal_uuid` |
| Catalog generation ID | `catalog_generation_id` |
| Security epoch | `security_epoch` |
| Policy epoch | `policy_epoch` |
| Resource epoch | `resource_epoch` |
| Engine ABI identifier | `engine_abi_id` (e.g., `sb_engine_abi_v3`) |
| Numeric backend profile | `numeric_backend_profile` |
| Backend provider and version | from `BackendInfo` |
| LLVM load mode | `llvm_load_mode` |
| LLVM library path digest | SHA-256 of library path |
| LLVM source root digest | SHA-256 of source root |
| LLVM tools root digest | SHA-256 of tools root |
| LLVM staging build directory digest | SHA-256 of staging build dir |
| Target triple | `target_triple` |
| Target feature set | comma-joined CPU feature flags |
| Compile mode | `jit` or `aot` |
| Unit kind | from lowerability classification |
| Descriptor set | per-descriptor UUID, kind, type, encoded digest |
| Policy profiles | all active policy profile strings |
| Physical profiles | all active physical profile strings |
| Option envelope hashes | SHA-256 per option envelope |

The final cache key is `"llvm-" + SHA256(material_string)`.

An artifact is invalidated whenever any dimension in its cache key material
changes. The function `NativeArtifactInvalidatedByDependency` performs this
check: if a dependency family and value do not appear in the cache key material,
the artifact is considered stale. Any change to `security_epoch`,
`catalog_generation_id`, `policy_epoch`, or `opcode_registry_epoch` invalidates
every artifact that was compiled under the previous values.

Native SBLR specialization carries a tighter per-invocation epoch check
(`NativeSblrEpochBinding`):

- `security_epoch` / `expected_security_epoch`
- `redaction_epoch` / `expected_redaction_epoch`

If the observed epoch does not match the expected epoch, the specialization
result is discarded and the scalar reference is used instead.

---

## Lowerability Bans

Before a unit is admitted for LLVM compilation, `ClassifyLowerability`
classifies it. The following ban codes cause an immediate refusal to compile
(verified in `native_compile.cpp`):

| Ban code | What triggers it |
|----------|-----------------|
| `sql_compile_forbidden` | Module payload contains SQL text (SELECT, INSERT, UPDATE, DELETE, or `sql:` prefix) |
| `parser_authority_forbidden` | Payload or options reference `parser_ast`, `parse_tree`, or `parser_authority` |
| `reference_authority_forbidden` | Payload or options reference `reference` authority, plan, or result |
| `protocol_or_client_authority_forbidden` | Payload or options reference `protocol_frame`, `wire_frame`, or `client_ir` |
| `engine_ir_validation_required` | Engine IR input present but not marked validated |
| `sblr_or_engine_ir_required` | Payload contains neither `sblr` nor `engine_ir` tokens |
| `authority_check_forbidden` | Payload contains `catalog_security`, `grant_check`, or `rls_check` |
| `mga_visibility_forbidden` | Payload contains `mga_visibility` |
| `mutation_side_effect_forbidden` | Payload contains `dml_mutation`, `commit`, or `rollback` |
| `udr_call_interpreter_only` | Payload contains `udr_call` |
| `logging_interpreter_only` | Payload contains logging function references |
| `cluster_operation_forbidden_noncluster` | Payload contains `cluster` references |

Units that pass lowerability are classified as `predicate`, `projection`, or
`expression`, with lowerability reason `llvm_safe`.

If a ban fires and `allow_interpreter_fallback` is `true` and the policy does
not require native compilation, the engine silently falls back to the
interpreter. If the policy is `jit_required_for_declared_units` or
`aot_package_required`, a hard refusal is returned instead.

---

## GPU Refusal Codes

The GPU gate (`EvaluateGpuAcceleration`, verified in
`src/engine/gpu_acceleration/gpu_acceleration.cpp`) produces the following
refusal codes:

| Code | Condition |
|------|-----------|
| `GPU.AUTHORITY_BYPASS_REFUSED` | Request attempted to claim authority over security, transaction, visibility, catalog, MGA, or cluster decisions |
| `GPU.UNSAFE_PAGE_ACCESS_REFUSED` | Request attempted direct page or catalog access |
| `GPU.SBLR_ONLY_KERNELS_REQUIRED` | Raw client or parser input requested; only validated SBLR or engine-internal kernel input is permitted |
| `GPU.CLUSTER_PLACEMENT_UNAVAILABLE` | Cluster dispatch requested but cluster authority is not available |
| `GPU.SECURITY_CONTEXT_REQUIRED` | GPU control requires an engine security context |
| `GPU.DISABLED_BY_POLICY` | The active policy profile is `disabled` |
| `GPU.WORKLOAD_UNSUPPORTED` | The workload class is not in the supported set |
| `GPU.INVALID_ACCELERATION_POLICY` | Structural policy violation (e.g., mismatched vector dimensions) |
| `GPU.DEVICE_MEMORY_POLICY_VIOLATION` | Materialized batch exceeds the device or pinned-memory budget |
| `GPU.BACKEND_UNAVAILABLE` | A non-optional profile requires a provider but none is available |
| `GPU.DETERMINISM_NOT_PROVEN` | An exact workload (aggregate, columnar scan, sort) requires CPU equivalence proof |
| `GPU.RUNTIME_COMPATIBILITY_REFUSED` | Runtime compatibility negotiation failed closed |
| `GPU.RUNTIME_COMPATIBILITY_FALLBACK` | Runtime compatibility negotiation requested scalar fallback |
| `GPU.FALLBACK_USED` | CPU reference path used by GPU policy (informational, not an error) |

---

## Policy Gates and Fail-Closed Guarantee

Acceleration is disabled by default. Every tier has a policy profile enum.
Setting the profile to `disabled` (the default) prevents any acceleration
from being attempted.

When an accelerator is attempted and fails for any reason not explicitly
permitted as a fallback, the engine fails closed: it returns an error or uses
the interpreter. It never silently returns an accelerated result that could
not be verified.

The `fail_closed` field in `ScoringKernelResult` and
`NativeSblrSpecializationResult` records when the engine detected a failure
and enforced the fail-closed path.

The operator-facing consequence is straightforward: disabling acceleration
cannot change query results. It can only change execution speed. This guarantee
holds for all tiers and for all combinations of policy settings.

---

## Cross-Links

- [README.md](#ch-acceleration-guide-readme-md) — guide overview
- [execution_tiers.md](#ch-acceleration-guide-execution-tiers-md) — three-tier stack, hotness, escalation
- [native_compilation.md](#ch-acceleration-guide-native-compilation-md) — LLVM policy profiles, cache key detail, AOT artifacts
- [gpu_and_simd_acceleration.md](#ch-acceleration-guide-gpu-and-simd-acceleration-md) — GPU policy profiles, refusal codes, scoring kernels
- [operating_acceleration.md](#ch-acceleration-guide-operating-acceleration-md) — operator workflow
- [../Security_Guide/trust_and_separation_architecture.md](#ch-security-guide-trust-and-separation-architecture-md)




===== FILE SEPARATION =====

<!-- chapter source: Acceleration_Guide/execution_tiers.md -->

<a id="ch-acceleration-guide-execution-tiers-md"></a>

# Execution Tiers

**Status: draft — subject to revision before stable release.**

## Purpose

This document describes the three-tier execution stack: how SBLR interpretation
works, how hot paths are detected and elevated to superinstruction fusion or
batch dispatch, and how those paths can be further elevated to native
compilation or GPU/SIMD scoring kernels. It also describes the fallback
discipline that ensures the interpreter is always the final safety net.

---

## Tier 1 — SBLR Interpreter

The SBLR (ScratchBird Bytecode Language Runtime) interpreter is the execution
baseline and the semantic authority. Every operation that enters the engine is
expressible as an SBLR instruction sequence, and every instruction in that
sequence can be evaluated by the interpreter.

The interpreter does not depend on any external library, hardware capability,
or build option. A build without any acceleration capability is a complete,
fully functional engine. It executes at interpreter speed, but it produces
correct results for all workloads.

The interpreter holds authority for:

- Expression values and query results
- Transaction side effects (begin, commit, rollback, savepoint)
- MGA (Multi-Generational Architecture) snapshot visibility
- Security policy checks and privilege enforcement
- Redaction and masking
- Parser boundary enforcement
- Catalog reads and mutation
- Error diagnostics

Higher tiers may produce the same values faster. They never produce different
values.

---

## Tier 2 — Superinstruction Fusion and Batch Dispatch

The second tier operates within the SBLR execution model. It does not cross
the boundary into native machine code or hardware acceleration. Its two
mechanisms are:

**Superinstruction fusion.** Adjacent SBLR opcodes that are safe to combine
into a single compound dispatch are fused into a superinstruction. The
superinstruction executes the same semantics as the original sequence but
reduces dispatch overhead. This is modeled by `SblrHotPathSuperinstructionPlan`
(verified in `src/engine/sblr/sblr_hot_path_execution.hpp`):

```
struct SblrHotPathSuperinstructionPlan {
  std::vector<std::string> fused_opcodes;   // opcodes that were combined
  std::string superinstruction_id;
  bool available;
  bool safe;
  bool exact_scalar_fallback_available;     // true when fallback is present
  std::uint64_t scalar_dispatches;          // per-iteration dispatches before fusion
  std::uint64_t fused_dispatches;           // per-iteration dispatches after fusion
};
```

Fusion is gated on `safe = true`. When fusion is not safe, the plan is not
applied and the interpreter dispatches each opcode individually.

**Batch dispatch.** When the same operation must be applied to many rows,
the dispatcher can accumulate a batch and dispatch once rather than once per
row. Batch dispatch is modeled by `SblrHotPathBatchPlan`:

```
struct SblrHotPathBatchPlan {
  std::uint64_t repeated_rows;
  std::uint64_t scalar_dispatches_per_row;
  std::uint64_t batched_dispatches_total;
  bool row_ordering_preserved;
  bool result_contract_hash_matches;        // must be true before result is accepted
  std::string expected_result_contract_hash;
  std::string observed_result_contract_hash;
};
```

The `result_contract_hash_matches` field is a structural safeguard. Before a
batched result is used, the engine verifies that the observed result contract
hash matches the expected value. If it does not match, the batch result is
discarded and the interpreter is used for each row individually.

**Savings counters.** The `SblrHotPathExecutionResult` struct records what
the tier achieved, in terms observable to the engine's profiling infrastructure:

- `dispatch_us_saved` — microseconds of dispatch overhead eliminated
- `opcode_dispatches_saved` — number of individual opcode dispatches avoided

These counters are evidence for further optimization decisions. They are not
performance guarantees or availability claims.

**Authority context.** The `SblrHotPathAuthorityContext` struct makes explicit
that no part of the hot path holds authority over security, MGA, transaction
visibility, or finality:

```
struct SblrHotPathAuthorityContext {
  bool parser_sql_execution_authority = false;
  bool reference_execution_authority = false;
  bool template_visibility_or_finality_authority = false;
  bool specialization_visibility_or_finality_authority = false;
  bool superinstruction_visibility_or_finality_authority = false;
  bool batch_visibility_or_finality_authority = false;
  // ...
};
```

All of these fields default to `false`. They are never set to `true` by the
engine for an accelerated path.

---

## Tier 3 — Native Compilation and GPU/SIMD Kernels

The third tier crosses into hardware-specific execution:

- **LLVM JIT compilation.** SBLR units are compiled to native machine code at
  runtime. The compiled module is loaded and used in place of the interpreter
  for that unit. Compilation is gated on lowerability classification, policy
  profile, epoch binding, and backend availability. Described in detail in
  [native_compilation.md](#ch-acceleration-guide-native-compilation-md).

- **LLVM AOT compilation.** Units are compiled ahead of time and stored as
  artifact files under `<database>.sb.native_aot/`. At runtime, the artifact
  is loaded and validated against the current multi-epoch cache key before use.

- **Native SBLR specialization.** The SBLR execution path itself can be
  specialized for specific kinds of hot operations (predicates, projections,
  row decode, path extraction, distance scoring, aggregates) using native
  machine code without a full LLVM compilation unit. Described in
  [native_compilation.md](#ch-acceleration-guide-native-compilation-md).

- **GPU/SIMD scoring kernels.** Materialized, authorized batches from the
  executor are offered to a scoring kernel provider. The kernel performs
  data-parallel computation on hardware-optimized code paths. The executor
  always holds the scalar reference and uses it for verification or fallback.
  Described in [gpu_and_simd_acceleration.md](#ch-acceleration-guide-gpu-and-simd-acceleration-md).

---

## Hotness Detection and Tier Escalation

A path becomes a candidate for Tier 3 when it crosses the hotness thresholds
defined in `NativeSblrHotness`
(verified in `src/engine/native_compile/native_sblr_specialization.hpp`):

```
struct NativeSblrHotness {
  std::uint64_t observed_invocations;
  std::uint64_t observed_rows;
  std::uint64_t observed_expressions;
  std::uint64_t minimum_invocations;   // threshold to cross
  std::uint64_t minimum_rows;
  std::uint64_t minimum_expressions;
};
```

When observed counts exceed the corresponding minimums, the path is eligible
for native specialization. The specific threshold values are configurable
through the policy profile system and are not hardcoded in the public surface.

For LLVM compilation, the `NativeCompileRequest` is submitted when a unit
crosses the hotness threshold. The engine selects the effective mode (JIT or
AOT) based on the active `NativeCompilePolicyProfile` and the requested mode.
If compilation succeeds, the artifact is used for subsequent invocations of
that unit. If compilation fails or the policy does not require native execution,
the interpreter continues to be used.

---

## Fallback Discipline

Every tier transitions downward on any failure:

1. If superinstruction fusion is unsafe or batch hash does not match, the
   interpreter handles each dispatch individually.
2. If native compilation fails and `allow_interpreter_fallback` is `true`, the
   engine records `fallback_used = true` and uses the interpreter.
3. If a GPU/SIMD scoring kernel is refused or fails, `fail_closed = true` is
   set in the result and the scalar reference is used.
4. If a policy profile is `disabled`, no tier above 1 is attempted.

In no case does a tier failure propagate as a query error unless the policy
explicitly requires native execution (profile `jit_required_for_declared_units`
or `aot_package_required`) and the backend is genuinely unavailable. In all
other configurations, a tier failure is silent to the query and the interpreter
takes over.

---

## Cross-Links

- [README.md](#ch-acceleration-guide-readme-md) — guide overview and candidate-accelerator principle
- [acceleration_authority_model.md](#ch-acceleration-guide-acceleration-authority-model-md) — authority boundaries, epoch binding
- [native_compilation.md](#ch-acceleration-guide-native-compilation-md) — LLVM policy profiles, hotness kinds, AOT artifacts
- [gpu_and_simd_acceleration.md](#ch-acceleration-guide-gpu-and-simd-acceleration-md) — GPU gate, scoring kernels
- [operating_acceleration.md](#ch-acceleration-guide-operating-acceleration-md) — operator workflow




===== FILE SEPARATION =====

<!-- chapter source: Acceleration_Guide/native_compilation.md -->

<a id="ch-acceleration-guide-native-compilation-md"></a>

# Native Compilation

**Status: draft — subject to revision before stable release.**

## Purpose

This document describes ScratchBird's LLVM-backed native compilation layer
(JIT and AOT) and the native SBLR specialization mechanism that feeds it. It
covers policy profiles, the multi-epoch SHA-256 cache key, AOT artifact format
and location, lowerability classification and ban codes, specialization kinds
and hotness thresholds, and the SBsql management surface.

Source references verified: `src/engine/native_compile/native_compile.hpp`,
`src/engine/native_compile/native_compile.cpp`,
`src/engine/native_compile/native_sblr_specialization.hpp`,
`src/engine/sblr/sblr_opcode_registry.cpp`,
`src/parsers/sbsql_worker/lowering/lowering.cpp`,
`src/engine/internal_api/extensibility/llvm_api.hpp`.

---

## Policy Profiles

The `NativeCompilePolicyProfile` enum controls whether and how native
compilation is attempted for a given execution context. The six profiles are
(verified in `native_compile.hpp`):

| Profile name | Constant | Behavior |
|-------------|----------|----------|
| `native_compile.disabled` | `disabled` | No native compilation. Interpreter always used. |
| `native_compile.jit_optional` | `jit_optional` | JIT compilation attempted when hotness thresholds are met; interpreter fallback permitted on failure. |
| `native_compile.jit_required_for_declared_units` | `jit_required_for_declared_units` | JIT must succeed for units that request it. If backend is unavailable, returns a hard refusal. |
| `native_compile.aot_optional` | `aot_optional` | AOT and JIT are both permitted; interpreter fallback permitted on failure. |
| `native_compile.aot_package_required` | `aot_package_required` | AOT package must be available. Hard refusal if artifact cannot be written or loaded. |
| `native_compile.dev_debug_ir_export` | `dev_debug_ir_export` | Development mode; IR export permitted. Not for production use. |

The `invalid` value is an error sentinel, not a valid operating profile.

The effective mode reported in `NativeCompileResult.effective_mode` reflects
what actually ran: `interpreter`, `jit`, `aot`, or `refused`. This may differ
from the requested mode when fallback or policy override applies.

Policy profiles are selected at request time based on the `policy_profiles`
vector in `NativeCompileRequest`. The selection order (verified in
`SelectPolicy`) is:

1. `dev_debug_ir_export` (highest priority)
2. `jit_required_for_declared_units`
3. `aot_package_required`
4. `aot_optional`
5. `jit_optional` (default when no profile or `policy=default` present)
6. `disabled`

---

## Multi-Epoch SHA-256 Cache Key

Every compiled artifact is identified by a cache key of the form
`"llvm-" + SHA256(material_string)`. The material string is a concatenation
of all the following dimensions (verified in `CacheKeyMaterial` in
`native_compile.cpp`):

**SBLR content dimensions:**
- SHA-256 digest of the SBLR module payload (`sblr_hash`)
- SBLR version string (e.g., `sblr_v3`)
- Opcode registry epoch (e.g., `static_v3`)
- Target object UUID and principal UUID

**Epoch dimensions — changes to any of these invalidate the artifact:**
- Catalog generation ID (`catalog_generation_id`)
- Security epoch (`security_epoch`)
- Policy epoch (`policy_epoch`)
- Resource epoch (`resource_epoch`)

**Engine identity dimensions:**
- Engine ABI identifier (e.g., `sb_engine_abi_v3`)
- Numeric backend profile (e.g., `sbl_numeric:int128,uint128,real128`)

**Backend provenance dimensions:**
- Backend provider string and version
- LLVM load mode (static or dynamic)
- SHA-256 of LLVM library path
- SHA-256 of LLVM source root
- SHA-256 of LLVM tools root
- SHA-256 of LLVM staging build directory
- Target triple (e.g., `x86_64-scratchbird-linux`)
- Target feature set (comma-joined CPU feature flags)

**Compilation mode and structural dimensions:**
- Compile mode (`jit` or `aot`)
- Unit kind (from lowerability classification)
- Descriptor set: per-descriptor UUID, kind, canonical type name, and
  SHA-256 of encoded descriptor content
- All active policy profile strings
- All active physical profile strings
- SHA-256 of each option envelope

An artifact is considered stale when any dimension in its cache key material
differs from the current values. The function `NativeArtifactInvalidatedByDependency`
performs dependency-specific invalidation: it checks whether a given
dependency family and value appear in the cached key material string. If they
do not, the artifact is invalidated.

The engine also computes a `provenance_hash` that covers the material string
combined with LLVM source root and library path digests. This hash allows
independent verification that an AOT artifact was produced by a known compiler
installation.

---

## AOT Artifact Format and Location

When the effective mode is `aot`, the engine writes an artifact to:

```
<database_path>.sb.native_aot/<cache_key>.native_aot.meta
```

The artifact file is a plain-text metadata record (verified in `WriteAotArtifact`
in `native_compile.cpp`) containing:

```
artifact_kind=metadata_only_aot_evidence
cache_key=<cache_key>
provenance_hash=<provenance_hash>
effective_mode=<effective_mode>
backend_provider=<backend_provider>
llvm_version=<llvm_version>
llvm_load_mode=<llvm_load_mode>
llvm_library_path_digest=<sha256>
llvm_source_root_digest=<sha256>
target_triple=<target_triple>
target_feature_set=<feature_set>
unit_kind=<unit_kind>
sblr_or_ir_digest=<sha256>
descriptor_set_digest=<sha256>
cache_key_material_hash=<sha256_of_material>
```

The `artifact_kind=metadata_only_aot_evidence` field identifies this as
planning and provenance evidence, not a binary machine-code image. It is used
to determine whether a unit has been compiled before and whether the artifact
remains valid under the current multi-epoch key.

If the artifact directory does not exist, `create_directories` is called.
If the write fails and `allow_interpreter_fallback` is `true`, the engine
falls back to the interpreter rather than returning an error.

---

## Lowerability Classification and Ban Codes

Before any unit is admitted for LLVM compilation, `ClassifyLowerability` runs
on the `module_payload` and `option_envelopes` fields of the request. The
following ban codes cause refusal (verified in `native_compile.cpp`):

| Ban code | Trigger condition |
|----------|-----------------|
| `module_payload_required` | Empty payload |
| `sql_compile_forbidden` | Payload contains SQL text (`SELECT`, `INSERT`, `UPDATE`, `DELETE`, or `sql:` prefix) |
| `parser_authority_forbidden` | Payload or options reference `parser_ast`, `parse_tree`, or `parser_authority` |
| `reference_authority_forbidden` | Payload or options reference reference authority, plan, or result |
| `protocol_or_client_authority_forbidden` | Payload or options reference `protocol_frame`, `wire_frame`, or `client_ir` |
| `engine_ir_validation_required` | Engine IR present but not marked validated via `engine_ir_validated` or `engine_ir:validated` option |
| `sblr_or_engine_ir_required` | Payload contains neither `sblr` nor `engine_ir` markers |
| `authority_check_forbidden` | Payload contains `catalog_security`, `grant_check`, or `rls_check` |
| `mga_visibility_forbidden` | Payload contains `mga_visibility` |
| `mutation_side_effect_forbidden` | Payload contains `dml_mutation`, `commit`, or `rollback` |
| `udr_call_interpreter_only` | Payload contains `udr_call` |
| `logging_interpreter_only` | Payload contains logging function references |
| `cluster_operation_forbidden_noncluster` | Payload contains `cluster` references |

Units that pass all checks are classified as `predicate`, `projection`, or
`expression` and assigned the lowerability reason `llvm_safe`.

If a ban fires on a non-required profile with `allow_interpreter_fallback`,
the engine silently uses the interpreter and records the ban code as the
`lowerability` field. On required profiles, the engine returns a hard refusal.

---

## Native SBLR Specialization

Native SBLR specialization is a lighter-weight form of native acceleration
that targets hot operation patterns within the SBLR execution path. It is
modeled separately from full LLVM compilation (verified in
`src/engine/native_compile/native_sblr_specialization.hpp`).

### Specialization Kinds

Six kinds of SBLR operations are eligible for native specialization:

| Kind constant | Description |
|---------------|-------------|
| `kPredicate` | Filter conditions (boolean expressions over rows) |
| `kProjection` | Column projection and expression evaluation |
| `kRowDecode` | Row format decoding |
| `kPathExtraction` | Path-based field extraction (e.g., from structured documents) |
| `kDistanceScoring` | Distance and similarity scoring expressions |
| `kAggregate` | Aggregate function evaluation |

An `kUnknown` value is the zero/error sentinel.

### Hotness Thresholds

Specialization is gated on observed invocation and data volume thresholds
in `NativeSblrHotness`:

- `observed_invocations` must exceed `minimum_invocations`
- `observed_rows` must exceed `minimum_rows`
- `observed_expressions` must exceed `minimum_expressions`

All three thresholds must be met before a unit becomes eligible. Thresholds
are set through the policy profile system.

### Epoch Binding

Each specialization request carries an `NativeSblrEpochBinding`:

- `security_epoch` / `expected_security_epoch`
- `redaction_epoch` / `expected_redaction_epoch`

If either observed epoch does not match the expected value, the specialization
result is discarded and the scalar reference is used instead. This ensures that
a specialization compiled under one security or redaction policy is never used
after those policies change.

### Routes

A specialization attempt produces one of three routes
(`NativeSblrSpecializationRoute`):

- `kNative` — native specialized execution was used
- `kScalarFallback` — scalar reference was used (success path with fallback)
- `kRefused` — specialization was refused (no fallback available or policy
  prevents fallback)

The `fail_closed` field in `NativeSblrSpecializationResult` records whether
the engine enforced the fail-closed path after detecting a specialization
failure.

---

## SBsql Management Surface

The following SBsql statements control and inspect the native compile subsystem.
All are verified against `lowering.cpp` and `sblr_opcode_registry.cpp`.

### Control Statements (ALTER NATIVE COMPILE)

These statements require `sysarch_authorized` security class and use the
`sblr.acceleration.llvm.v3` SBLR family.

| Statement | Operation ID | Opcode | Catalog table |
|-----------|-------------|--------|--------------|
| `ALTER NATIVE COMPILE AOT REBUILD <unit_ref>` | `alter.native_compile.aot_rebuild` | `SBLR_OP_NATIVE_COMPILE_AOT_REBUILD` | `sys.native_compile.aot_units` |
| `ALTER NATIVE COMPILE ARTIFACT <ref> QUARANTINE` | `alter.native_compile.artifact_quarantine` | `SBLR_OP_NATIVE_COMPILE_ARTIFACT_QUARANTINE` | `sys.native_compile.artifacts` |
| `ALTER NATIVE COMPILE CACHE INVALIDATE` | `alter.native_compile.cache_invalidate` | `SBLR_OP_NATIVE_COMPILE_CACHE_INVALIDATE` | `sys.native_compile.cache` |
| `ALTER NATIVE COMPILE PROFILE <ref> DISABLE` | `alter.native_compile.profile_disable` | `SBLR_OP_NATIVE_COMPILE_PROFILE_DISABLE` | `sys.native_compile.profiles` |
| `ALTER NATIVE COMPILE PROFILE <ref> ENABLE` | `alter.native_compile.profile_enable` | `SBLR_OP_NATIVE_COMPILE_PROFILE_ENABLE` | `sys.native_compile.profiles` |

All control statements return result shape `rs.acceleration.control.v1` and
route through `EngineControlNativeCompile`.

### Inspect Statements (SHOW)

These statements do not require a transaction context and route through
`EngineInspectNativeCompile` using the `sblr.acceleration.llvm.v3` SBLR family.

| Statement | Operation ID | Opcode | Catalog table |
|-----------|-------------|--------|--------------|
| `SHOW AOT ARTIFACTS` | `show.aot_artifacts` | `SBLR_OP_SHOW_AOT_ARTIFACTS` | `sys.native_compile.artifacts` |
| `SHOW LLVM` | `show.llvm` | `SBLR_OP_SHOW_LLVM` | `sys.native_compile.llvm` |
| `SHOW LLVM PROVENANCE` | `show.llvm_provenance` | `SBLR_OP_SHOW_LLVM_PROVENANCE` | `sys.native_compile.llvm` |
| `SHOW LLVM TARGETS` | `show.llvm_targets` | `SBLR_OP_SHOW_LLVM_TARGETS` | `sys.native_compile.llvm` |
| `SHOW NATIVE COMPILE` | `show.native_compile` | `SBLR_OP_SHOW_NATIVE_COMPILE` | `sys.native_compile.runtime` |
| `SHOW NATIVE COMPILE CACHE` | `show.native_compile_cache` | `SBLR_OP_SHOW_NATIVE_COMPILE_CACHE` | `sys.native_compile.cache` |

### Opcode Registry Entries

The SBLR opcode registry (verified in `sblr_opcode_registry.cpp`) defines four
acceleration management opcodes for the LLVM tier, all with category
`management` and security class `sysarch_authorized`:

- `acceleration.llvm.compile` → `SBLR_ACCEL_LLVM_COMPILE`
- `acceleration.llvm.inspect` → `SBLR_ACCEL_LLVM_INSPECT`
- `acceleration.llvm.invalidate` → `SBLR_ACCEL_LLVM_INVALIDATE`
- `acceleration.llvm.policy_set` → `SBLR_ACCEL_LLVM_POLICY_SET`

The op-level opcodes (used by the SBsql surface above) are category
`extensibility`:

- `op.native_compile.aot_rebuild` → `SBLR_OP_NATIVE_COMPILE_AOT_REBUILD`
- `op.native_compile.artifact_quarantine` → `SBLR_OP_NATIVE_COMPILE_ARTIFACT_QUARANTINE`
- `op.native_compile.cache_invalidate` → `SBLR_OP_NATIVE_COMPILE_CACHE_INVALIDATE`
- `op.native_compile.profile_disable` → `SBLR_OP_NATIVE_COMPILE_PROFILE_DISABLE`
- `op.native_compile.profile_enable` → `SBLR_OP_NATIVE_COMPILE_PROFILE_ENABLE`

### Engine Internal API

(Verified in `src/engine/internal_api/extensibility/llvm_api.hpp`.)

| Entry point | Purpose |
|-------------|---------|
| `EngineCompileLlvmModule` | Submit an SBLR or engine IR module for JIT or AOT compilation |
| `EngineControlNativeCompile` | Execute ALTER NATIVE COMPILE control statements |
| `EngineInspectNativeCompile` | Execute SHOW LLVM / SHOW AOT ARTIFACTS / SHOW NATIVE COMPILE inspect statements |

---

## Cross-Links

- [README.md](#ch-acceleration-guide-readme-md) — guide overview and candidate-accelerator principle
- [acceleration_authority_model.md](#ch-acceleration-guide-acceleration-authority-model-md) — epoch binding, lowerability bans in the authority context
- [execution_tiers.md](#ch-acceleration-guide-execution-tiers-md) — where native compilation fits in the three-tier stack
- [gpu_and_simd_acceleration.md](#ch-acceleration-guide-gpu-and-simd-acceleration-md) — GPU policy profiles and scoring kernels
- [operating_acceleration.md](#ch-acceleration-guide-operating-acceleration-md) — operator workflow for managing native compile
- ../Language_Reference/syntax_reference/ebnf/acceleration_statement.md (SBsql Language Reference — Syntax, page XXX)




===== FILE SEPARATION =====

<!-- chapter source: Acceleration_Guide/gpu_and_simd_acceleration.md -->

<a id="ch-acceleration-guide-gpu-and-simd-acceleration-md"></a>

# GPU and SIMD Acceleration

**Status: draft — subject to revision before stable release.**

## Purpose

This document describes ScratchBird's GPU/SIMD acceleration layer: the GPU
gate that controls whether and how batched workloads are offered to a hardware
accelerator, and the SIMD/GPU scoring kernels that provide data-parallel
execution for specific operation kinds. It covers policy profiles, memory
budgets, determinism requirements, refusal codes, scoring kernel kinds and
routes, provider manifest authority fields, and the full SBsql management
surface.

Source references verified: `src/engine/gpu_acceleration/gpu_acceleration.hpp`,
`src/engine/gpu_acceleration/gpu_acceleration.cpp`,
`src/engine/gpu_acceleration/scoring_kernel_acceleration.hpp`,
`src/engine/executor/scoring_kernel_executor.hpp`,
`src/engine/sblr/sblr_opcode_registry.cpp`,
`src/parsers/sbsql_worker/lowering/lowering.cpp`,
`src/engine/internal_api/extensibility/gpu_api.hpp`.

---

## The GPU Gate

The GPU gate (`EvaluateGpuAcceleration`, defined in `gpu_acceleration.hpp` and
implemented in `gpu_acceleration.cpp`) is the admission function for all GPU
workloads. It enforces authority boundaries before any batch is offered to a
hardware provider.

### What the GPU Layer Is Not

The header comment in `gpu_acceleration.hpp` states:

> GPU is never semantic, security, transaction, MGA, catalog, cleanup,
> recovery, parser, or cluster-decision authority.

The gate enforces this with pre-flight refusal checks that run before any
provider is consulted. See the refusal codes section below.

### Policy Profiles

The `GpuPolicyProfile` enum controls whether the GPU layer is active and
how failures are handled (verified in `gpu_acceleration.hpp` and named strings
verified in `GpuPolicyProfileName` in `gpu_acceleration.cpp`):

| Profile enum | Named string | Description |
|-------------|-------------|-------------|
| `disabled` | `gpu_accel.disabled` | No GPU execution. All requests fall back to CPU reference. Default. |
| `optional_batch` | `gpu_accel.optional_batch` | GPU attempted; CPU fallback permitted if provider is unavailable or workload fails. |
| `required_for_declared_workload` | `gpu_accel.required_for_declared_workload` | GPU required for workloads that declare it; hard refusal if backend unavailable. |
| `cluster_optional` | `gpu_accel.cluster_optional` | Cluster-aware GPU dispatch; CPU fallback permitted. |
| `cluster_required` | `gpu_accel.cluster_required` | Cluster-aware GPU dispatch required; hard refusal if cluster authority unavailable. |
| `dev_kernel_debug` | `gpu_accel.dev_kernel_debug` | Development and debug mode. Not for production use. Treated as optional. |

### Effective Paths

After the gate evaluates a request, it returns one of four effective paths
(`GpuEffectivePath`, verified in `gpu_acceleration.hpp` and
`GpuEffectivePathName`):

| Path | Meaning |
|------|---------|
| `inspect_only` | Request was informational; no execution attempted |
| `cpu_fallback` | Policy is optional and provider is unavailable, or workload requires CPU equivalence |
| `gpu_provider_admitted` | Request was passed to the GPU provider |
| `refused` | Request was refused due to a policy violation or authority check failure |

### Batch and Memory Budgets

Before a batch is offered to the GPU provider, the gate verifies that the batch
fits within memory budgets (verified in `BatchFitsBudget` in
`gpu_acceleration.cpp`):

- `device_memory_budget_bytes` — default 64 MiB; the batch (combined size of
  `values` and `rhs_values` as `double` arrays) must not exceed this limit.
- `pinned_host_memory_budget_bytes` — default 16 MiB; the same batch size
  must not exceed this limit.

Both checks use the same batch size calculation. If either limit is exceeded,
the gate returns `GPU.DEVICE_MEMORY_POLICY_VIOLATION`.

### Determinism Requirement

The `deterministic_equivalence_required` field on a request controls how the
gate handles workloads that cannot be proven equivalent between GPU and CPU
execution. By default it is `true`.

The gate applies a CPU-required check (`ExactWorkloadRequiresCpu`) for workloads
`aggregate`, `columnar_scan`, and `sort` when `deterministic_equivalence_required`
is `true` and `approximate_declared` is `false`. If a non-optional profile is
active and this check fires, the gate returns `GPU.DETERMINISM_NOT_PROVEN`. If
the profile is optional (including `cluster_optional` and `dev_kernel_debug`),
the gate silently uses the CPU reference path and reports `fallback_used = true`.

Supported workload classes (verified in `GpuWorkloadSupported`) are:
`vector`, `search`, `columnar_scan`, `aggregate`, `sort`, `index_build`,
`timeseries_transform`, `compression_transform`, `graph_batch`.

### Batch Operations

The `GpuBatchOperation` enum identifies the operation to execute on the batch
(verified in `gpu_acceleration.hpp` and `GpuBatchOperationName`):

| Operation | Description |
|-----------|-------------|
| `none` | No batch operation |
| `filter_positive` | Retain values greater than zero |
| `project_scale` | Multiply each value by a scale factor |
| `aggregate_sum` | Sum all values |
| `vector_dot` | Dot product of two equal-length vectors |

The `vector_dot` operation requires that `values` and `rhs_values` have equal
length; a mismatch produces `GPU.INVALID_ACCELERATION_POLICY`.

---

## GPU Refusal Codes

All refusal codes are verified in `EvaluateGpuAcceleration` in
`gpu_acceleration.cpp`. They are returned in `GpuAccelerationResult.diagnostic_code`.

| Code | Condition |
|------|-----------|
| `GPU.AUTHORITY_BYPASS_REFUSED` | Request set `authority_bypass_requested = true`; the GPU layer cannot be a transaction, security, visibility, catalog, MGA, or cluster authority |
| `GPU.UNSAFE_PAGE_ACCESS_REFUSED` | Request set `direct_page_or_catalog_access_requested = true`; only materialized, authorized batches are permitted |
| `GPU.SBLR_ONLY_KERNELS_REQUIRED` | Request set `raw_client_or_parser_input_requested = true`; kernels must originate from validated SBLR or engine-internal sources |
| `GPU.CLUSTER_PLACEMENT_UNAVAILABLE` | Cluster dispatch requested or a cluster profile is active, but cluster authority is not available |
| `GPU.SECURITY_CONTEXT_REQUIRED` | GPU control requires an engine security context (enforced when `security_context_present = false` and execution is requested) |
| `GPU.DISABLED_BY_POLICY` | Active profile is `disabled` |
| `GPU.WORKLOAD_UNSUPPORTED` | The workload class is not in the supported set |
| `GPU.INVALID_ACCELERATION_POLICY` | Structural policy violation (e.g., vector length mismatch for `vector_dot`) |
| `GPU.DEVICE_MEMORY_POLICY_VIOLATION` | Batch exceeds device or pinned-memory budget |
| `GPU.BACKEND_UNAVAILABLE` | A non-optional profile requires a provider but none is available |
| `GPU.DETERMINISM_NOT_PROVEN` | Exact workload requires CPU equivalence proof; non-optional profile active |
| `GPU.RUNTIME_COMPATIBILITY_REFUSED` | Runtime compatibility negotiation failed closed |
| `GPU.RUNTIME_COMPATIBILITY_FALLBACK` | Runtime compatibility negotiation requested scalar fallback (informational) |
| `GPU.FALLBACK_USED` | CPU reference path used by optional GPU policy (informational, not an error) |

---

## Scoring Kernels

Scoring kernels are the mechanism by which specific operation kinds are
accelerated using SIMD or GPU hardware. They are distinct from the full GPU
batch gate: they operate on materialized, authorized executor batches and
are routed through the executor hook `ExecuteOptionalScoringKernel`
(verified in `src/engine/executor/scoring_kernel_executor.hpp`).

### Kernel Kinds

The `ScoringKernelKind` enum defines six supported operation kinds (verified in
`src/engine/gpu_acceleration/scoring_kernel_acceleration.hpp`):

| Kind constant | Description |
|---------------|-------------|
| `kVectorDistance` | Vector similarity and distance computation |
| `kBm25` | BM25 text relevance scoring |
| `kBitmapIntersection` | Bitmap set intersection |
| `kTimeAggregate` | Time-series aggregation |
| `kJsonPath` | Structured document path evaluation |
| `kGraphMembership` | Graph reachability and membership queries |

The `kUnknown` value is the zero/error sentinel.

### Kernel Routes

After the executor evaluates a scoring kernel request, it returns one of three
routes (`ScoringKernelRoute`):

| Route | Meaning |
|-------|---------|
| `kAccelerator` | Hardware-accelerated kernel was used |
| `kScalarFallback` | Scalar reference path was used (success path with fallback) |
| `kRefused` | Kernel was refused; result is not available |

### Provider Manifest Authority Fields

Every scoring kernel provider must supply a `ScoringKernelProviderManifest`
(verified in `scoring_kernel_acceleration.hpp`). The manifest carries explicit
authority denial fields that must all be `false`:

| Field | Authority denied |
|-------|-----------------|
| `claims_transaction_finality_authority` | Transaction commit/rollback decisions |
| `claims_visibility_authority` | MGA snapshot visibility |
| `claims_security_policy_authority` | Privilege and access control |
| `claims_redaction_policy_authority` | Column masking and row redaction |
| `claims_parser_or_reference_authority` | SQL parsing and reference-result determination |
| `claims_recovery_authority` | Crash recovery and WAL application |
| `claims_page_or_catalog_authority` | Direct page access and catalog mutation |

A provider that claims any of these authorities is rejected.

The manifest also carries capability fields: `provider_id`, `engine_abi_id`
(must be `sb_engine_abi_v3`), `runtime_identity_id`, `architecture`,
`cpu_capabilities`, `gpu_capabilities`, and the set of `supported_kinds`.
The `safe_to_execute` field must be `true`.

### Scalar Reference

Every `ScoringKernelRequest` carries a `scalar_reference` — a callable of type:

```
std::function<ScoringKernelValueBatch(const ScoringKernelInputBatch&)>
```

This reference is supplied by the engine (via the executor), not the provider.
If the accelerated kernel produces output that cannot be verified or the
accelerator fails, the engine calls this reference and uses its output. This
is what makes scoring kernels fail-closed: the interpreter-level reference is
always available.

### Input Batch Authorization

The `ScoringKernelInputBatch` struct carries two safety flags (verified in
`scoring_kernel_acceleration.hpp`):

- `materialized_authorized_batch` — must be `true`; the batch has been
  through the executor's authorization path
- `raw_client_or_parser_input` — must be `false`; raw input is never
  offered to a scoring kernel provider
- `direct_page_or_catalog_access` — must be `false`; providers never
  receive direct storage access

---

## SBsql Management Surface

All statements below are verified against `lowering.cpp` and
`sblr_opcode_registry.cpp`.

### GPU Control Statements (ALTER GPU)

These statements require `sysarch_authorized` security class and use the
`sblr.acceleration.gpu.v3` SBLR family. All return result shape
`rs.acceleration.control.v1` and route through `EngineControlGpuAcceleration`.

| Statement | Operation ID | Opcode | Catalog table |
|-----------|-------------|--------|--------------|
| `ALTER GPU ARTIFACT <ref> QUARANTINE` | `alter.gpu.artifact_quarantine` | `SBLR_OP_GPU_ARTIFACT_QUARANTINE` | `sys.acceleration.gpu_artifacts` |
| `ALTER GPU CACHE CLEAR` | `alter.gpu.cache_clear` | `SBLR_OP_GPU_CACHE_CLEAR` | `sys.acceleration.gpu_cache` |
| `ALTER GPU DEVICE <ref> QUARANTINE` | `alter.gpu.device_quarantine` | `SBLR_OP_GPU_DEVICE_QUARANTINE` | `sys.acceleration.gpu_devices` |
| `ALTER GPU KERNEL <ref> QUARANTINE` | `alter.gpu.kernel_quarantine` | `SBLR_OP_GPU_KERNEL_QUARANTINE` | `sys.acceleration.gpu_kernels` |
| `ALTER GPU PROFILE <ref> DISABLE` | `alter.gpu.profile_disable` | `SBLR_OP_GPU_PROFILE_DISABLE` | `sys.acceleration.gpu_profiles` |
| `ALTER GPU PROFILE <ref> ENABLE` | `alter.gpu.profile_enable` | `SBLR_OP_GPU_PROFILE_ENABLE` | `sys.acceleration.gpu_profiles` |

### GPU Inspect Statements (SHOW GPU)

These statements do not require a transaction context. They route through
`EngineInspectGpuAcceleration` using the `sblr.acceleration.gpu.v3` SBLR family.

| Statement | Operation ID | Opcode | Catalog table |
|-----------|-------------|--------|--------------|
| `SHOW GPU` | `show.gpu` | `SBLR_OP_SHOW_GPU` | `sys.acceleration.gpu_runtime` |
| `SHOW GPU ARTIFACTS` | `show.gpu_artifacts` | `SBLR_OP_SHOW_GPU_ARTIFACTS` | `sys.acceleration.gpu_artifacts` |
| `SHOW GPU CAPABILITY` | `show.gpu_capability` | `SBLR_OP_SHOW_GPU_CAPABILITY` | `sys.acceleration.gpu_capability` |
| `SHOW GPU DEVICES` | `show.gpu_devices` | `SBLR_OP_SHOW_GPU_DEVICES` | `sys.acceleration.gpu_devices` |
| `SHOW GPU KERNELS` | `show.gpu_kernels` | `SBLR_OP_SHOW_GPU_KERNELS` | `sys.acceleration.gpu_kernels` |
| `SHOW GPU MEMORY` | `show.gpu_memory` | `SBLR_OP_SHOW_GPU_MEMORY` | `sys.acceleration.gpu_memory` |

The SHOW GPU statements use result shapes `rs.show.gpu.v1` (SHOW GPU,
SHOW GPU CAPABILITY, SHOW GPU DEVICES, SHOW GPU MEMORY) and
`rs.show.native_compile.v1` (SHOW GPU ARTIFACTS, SHOW GPU KERNELS).

`EngineInspectGpuCapability` is also used by inspect statements that query
hardware capability without requiring a full acceleration context (verified in
`gpu_api.hpp` and referenced in `lowering.cpp` at lines 4918, 4930, 4942).

### Opcode Registry Entries

The SBLR opcode registry defines four acceleration management opcodes for the
GPU tier, all with category `management` and security class `sysarch_authorized`
(verified in `sblr_opcode_registry.cpp`):

- `acceleration.gpu.compile` → `SBLR_ACCEL_GPU_COMPILE`
- `acceleration.gpu.inspect` → `SBLR_ACCEL_GPU_INSPECT`
- `acceleration.gpu.invalidate` → `SBLR_ACCEL_GPU_INVALIDATE`
- `acceleration.gpu.policy_set` → `SBLR_ACCEL_GPU_POLICY_SET`

The op-level opcodes (used by the SBsql surface above) are category
`extensibility`:

- `op.gpu.artifact_quarantine` → `SBLR_OP_GPU_ARTIFACT_QUARANTINE`
- `op.gpu.cache_clear` → `SBLR_OP_GPU_CACHE_CLEAR`
- `op.gpu.device_quarantine` → `SBLR_OP_GPU_DEVICE_QUARANTINE`
- `op.gpu.kernel_quarantine` → `SBLR_OP_GPU_KERNEL_QUARANTINE`
- `op.gpu.profile_disable` → `SBLR_OP_GPU_PROFILE_DISABLE`
- `op.gpu.profile_enable` → `SBLR_OP_GPU_PROFILE_ENABLE`
- `op.show.gpu` → `SBLR_OP_SHOW_GPU`
- `op.show.gpu_artifacts` → `SBLR_OP_SHOW_GPU_ARTIFACTS`
- `op.show.gpu_capability` → `SBLR_OP_SHOW_GPU_CAPABILITY`
- `op.show.gpu_devices` → `SBLR_OP_SHOW_GPU_DEVICES`
- `op.show.gpu_kernels` → `SBLR_OP_SHOW_GPU_KERNELS`
- `op.show.gpu_memory` → `SBLR_OP_SHOW_GPU_MEMORY`

### Engine Internal API

(Verified in `src/engine/internal_api/extensibility/gpu_api.hpp`.)

| Entry point | Purpose |
|-------------|---------|
| `EngineInspectGpuCapability` | Query hardware capability without full acceleration context |
| `EngineControlGpuAcceleration` | Execute ALTER GPU control statements |
| `EngineInspectGpuAcceleration` | Execute SHOW GPU inspect statements |

---

## Catalog Tables Reference

| Table | Contents |
|-------|----------|
| `sys.acceleration.gpu_runtime` | Current GPU runtime state |
| `sys.acceleration.gpu_artifacts` | Compiled GPU artifacts |
| `sys.acceleration.gpu_capability` | Detected hardware capabilities |
| `sys.acceleration.gpu_devices` | Known GPU devices and their status |
| `sys.acceleration.gpu_kernels` | Registered GPU kernels |
| `sys.acceleration.gpu_memory` | Device and pinned memory usage |
| `sys.acceleration.gpu_profiles` | Policy profile records |
| `sys.acceleration.gpu_cache` | GPU kernel cache state |

---

## Cross-Links

- [README.md](#ch-acceleration-guide-readme-md) — guide overview
- [acceleration_authority_model.md](#ch-acceleration-guide-acceleration-authority-model-md) — refusal codes in authority context
- [execution_tiers.md](#ch-acceleration-guide-execution-tiers-md) — where GPU fits in the three-tier stack
- [native_compilation.md](#ch-acceleration-guide-native-compilation-md) — LLVM JIT/AOT
- [operating_acceleration.md](#ch-acceleration-guide-operating-acceleration-md) — operator workflow for GPU management
- ../Language_Reference/syntax_reference/ebnf/acceleration_statement.md (SBsql Language Reference — Syntax, page XXX)
- [../Security_Guide/trust_and_separation_architecture.md](#ch-security-guide-trust-and-separation-architecture-md)




===== FILE SEPARATION =====

<!-- chapter source: Acceleration_Guide/operating_acceleration.md -->

<a id="ch-acceleration-guide-operating-acceleration-md"></a>

# Operating Acceleration

**Status: draft — subject to revision before stable release.**

## Purpose

This document is the operator-facing reference for managing ScratchBird's
acceleration stack. It covers the full workflow: enabling policy profiles,
quarantining problematic artifacts or devices, clearing caches, inspecting
current state through the SBsql surface, and understanding what the opt-in
posture means for operational safety.

The central guarantee for operators is stated here and detailed in
[acceleration_authority_model.md](#ch-acceleration-guide-acceleration-authority-model-md):

> **Disabling acceleration never changes query results. It changes only execution
> speed.**

All acceleration is opt-in and fail-closed. Removing or disabling any
accelerator causes the engine to use the SBLR interpreter, which always
produces correct results.

---

## The Opt-In Posture

Every acceleration mechanism is disabled by default. The policy profile for
GPU acceleration is `gpu_accel.disabled`. The policy profile for native
compilation is `native_compile.disabled`. Superinstruction fusion at Tier 2
operates within the SBLR execution model and does not require explicit opt-in,
but it too falls back safely when conditions are not met.

This means:

- A freshly configured instance has no acceleration active.
- An operator must explicitly enable a policy profile to permit acceleration.
- An operator can revert to full interpreter execution at any time by setting
  or restoring a disabled profile.
- Reversion is safe: interpreter results are always correct.

---

## Enabling and Disabling Policy Profiles

### Native Compile Profiles

To enable JIT compilation for units that qualify:

```sql
ALTER NATIVE COMPILE PROFILE <profile_ref> ENABLE;
```

To disable a profile:

```sql
ALTER NATIVE COMPILE PROFILE <profile_ref> DISABLE;
```

The named profiles (verified in `native_compile.hpp` and `lowering.cpp`) are:

| Profile | Effect when enabled |
|---------|---------------------|
| `native_compile.jit_optional` | JIT attempted; interpreter fallback on failure |
| `native_compile.jit_required_for_declared_units` | JIT required for declared units; hard refusal if backend unavailable |
| `native_compile.aot_optional` | AOT and JIT attempted; interpreter fallback on failure |
| `native_compile.aot_package_required` | AOT package required; hard refusal on artifact failure |
| `native_compile.dev_debug_ir_export` | Debug mode; not for production |

Disabling a profile causes subsequent compilations to use the interpreter.
Previously compiled artifacts remain in the cache under their cache keys but
are not loaded under the `disabled` profile.

### GPU Profiles

To enable GPU acceleration for batch workloads:

```sql
ALTER GPU PROFILE <profile_ref> ENABLE;
```

To disable:

```sql
ALTER GPU PROFILE <profile_ref> DISABLE;
```

Named GPU profiles (verified in `gpu_acceleration.cpp`):

| Profile | Effect when enabled |
|---------|---------------------|
| `gpu_accel.optional_batch` | GPU attempted; CPU fallback on failure |
| `gpu_accel.required_for_declared_workload` | GPU required for declared workloads |
| `gpu_accel.cluster_optional` | Cluster-aware GPU; CPU fallback |
| `gpu_accel.cluster_required` | Cluster-aware GPU required |
| `gpu_accel.dev_kernel_debug` | Debug mode; not for production |

The default is `gpu_accel.disabled`. Enabling any other profile takes effect
for workloads submitted after the profile is active.

---

## Quarantine Operations

Quarantine removes a specific artifact, device, or kernel from active use.
The quarantined object is not deleted; it is marked unavailable. The engine
falls back to the interpreter (for artifacts) or refuses GPU workloads that
require that device or kernel.

### Quarantine a Native Compile Artifact

```sql
ALTER NATIVE COMPILE ARTIFACT <artifact_ref> QUARANTINE;
```

Routes to `sys.native_compile.artifacts`. Opcode:
`SBLR_OP_NATIVE_COMPILE_ARTIFACT_QUARANTINE`.

Use this when an artifact is suspected of producing incorrect results or when
an artifact was built against a now-stale configuration. After quarantine, the
engine will recompile the unit or fall back to the interpreter on the next
invocation.

### Quarantine a GPU Artifact

```sql
ALTER GPU ARTIFACT <artifact_ref> QUARANTINE;
```

Routes to `sys.acceleration.gpu_artifacts`. Opcode:
`SBLR_OP_GPU_ARTIFACT_QUARANTINE`.

### Quarantine a GPU Device

```sql
ALTER GPU DEVICE <device_ref> QUARANTINE;
```

Routes to `sys.acceleration.gpu_devices`. Opcode:
`SBLR_OP_GPU_DEVICE_QUARANTINE`.

After a device is quarantined, batch workloads that would have dispatched to
that device will use the CPU reference path or another available device,
depending on the active policy profile.

### Quarantine a GPU Kernel

```sql
ALTER GPU KERNEL <kernel_ref> QUARANTINE;
```

Routes to `sys.acceleration.gpu_kernels`. Opcode:
`SBLR_OP_GPU_KERNEL_QUARANTINE`.

Kernels are the executable units within the GPU acceleration subsystem.
Quarantining a kernel prevents it from being dispatched. The scoring kernel
executor will use the scalar reference for any kernel kind that has been
quarantined.

---

## Cache Operations

### Invalidate the Native Compile Cache

```sql
ALTER NATIVE COMPILE CACHE INVALIDATE;
```

Routes to `sys.native_compile.cache`. Opcode:
`SBLR_OP_NATIVE_COMPILE_CACHE_INVALIDATE`.

This forces all cached compiled artifacts to be re-evaluated on next use.
It does not delete artifact files but marks them as requiring re-validation.
Use this after a change that the epoch system may not have detected, such as
a manual update to an LLVM installation.

### Clear the GPU Cache

```sql
ALTER GPU CACHE CLEAR;
```

Routes to `sys.acceleration.gpu_cache`. Opcode:
`SBLR_OP_GPU_CACHE_CLEAR`.

This clears the GPU kernel cache. Subsequent workloads recompile GPU kernels
as needed. The CPU reference path is used until the kernel cache is repopulated.

---

## AOT Rebuild

```sql
ALTER NATIVE COMPILE AOT REBUILD <unit_ref>;
```

Routes to `sys.native_compile.aot_units`. Opcode:
`SBLR_OP_NATIVE_COMPILE_AOT_REBUILD`.

This triggers a rebuild of the named AOT unit's compiled artifact. Use it
when an AOT artifact has been quarantined, when the LLVM installation has
changed, or when an artifact file has been manually removed. The rebuild
writes a new artifact file under `<database>.sb.native_aot/<cache_key>.native_aot.meta`.

---

## Inspection Surface

The inspection surface covers both the aggregate acceleration view and the
individual GPU and native compile subsystems.

### Aggregate Acceleration View

These statements produce a summary of all acceleration subsystems:

```sql
SHOW ACCELERATION;
SHOW ACCELERATION EXTENDED;
```

- `SHOW ACCELERATION` routes to operation ID
  `observability.show_acceleration`, opcode
  `SBLR_OBSERVABILITY_SHOW_ACCELERATION`.
- `SHOW ACCELERATION EXTENDED` routes to
  `observability.show_acceleration_extended`, opcode
  `SBLR_OBSERVABILITY_SHOW_ACCELERATION_EXTENDED`.

Both are read-only observability operations that do not require a transaction
context.

### GPU Inspection Statements

| Statement | What it shows | Catalog table |
|-----------|--------------|--------------|
| `SHOW GPU` | Current GPU runtime state and policy | `sys.acceleration.gpu_runtime` |
| `SHOW GPU CAPABILITY` | Detected hardware capabilities | `sys.acceleration.gpu_capability` |
| `SHOW GPU DEVICES` | Known devices and quarantine status | `sys.acceleration.gpu_devices` |
| `SHOW GPU KERNELS` | Registered kernels and their status | `sys.acceleration.gpu_kernels` |
| `SHOW GPU ARTIFACTS` | Compiled GPU artifacts | `sys.acceleration.gpu_artifacts` |
| `SHOW GPU MEMORY` | Device and pinned memory usage and budgets | `sys.acceleration.gpu_memory` |

### Native Compile Inspection Statements

| Statement | What it shows | Catalog table |
|-----------|--------------|--------------|
| `SHOW LLVM` | LLVM backend state: version, load mode, paths | `sys.native_compile.llvm` |
| `SHOW LLVM PROVENANCE` | SHA-256 provenance hashes for the LLVM installation | `sys.native_compile.llvm` |
| `SHOW LLVM TARGETS` | Available compilation target triples and feature sets | `sys.native_compile.llvm` |
| `SHOW AOT ARTIFACTS` | AOT artifact files: cache key, effective mode, provenance | `sys.native_compile.artifacts` |
| `SHOW NATIVE COMPILE` | Native compile subsystem runtime state | `sys.native_compile.runtime` |
| `SHOW NATIVE COMPILE CACHE` | Current cache state and contents | `sys.native_compile.cache` |

---

## Capacity and Budget Management

GPU memory limits are configured through the `device_memory_budget_bytes`
and `pinned_host_memory_budget_bytes` fields of `GpuAccelerationRequest`
(defaults: 64 MiB and 16 MiB respectively). Batches that exceed either limit
produce `GPU.DEVICE_MEMORY_POLICY_VIOLATION`.

Use `SHOW GPU MEMORY` to observe current device memory usage relative to these
budgets. If workloads are consistently hitting memory limits with an optional
profile, the engine will fall back to the CPU reference path. If a required
profile is active, workloads that exceed the budget will be refused.

LLVM memory accounting is tracked through the `LlvmMemoryAccountingRequest`
system. The `SHOW LLVM` output includes memory reservation counts and sizes
when memory accounting is active.

---

## Diagnostics

Acceleration failures appear in the `diagnostic_code` field of the relevant
result struct and are exposed through the observability surface.

For GPU failures, the refusal codes (listed in full in
[gpu_and_simd_acceleration.md](#ch-acceleration-guide-gpu-and-simd-acceleration-md)) indicate the
specific reason. Codes beginning with `GPU.` are GPU-gate refusals. The
`GPU.FALLBACK_USED` and `GPU.RUNTIME_COMPATIBILITY_FALLBACK` codes are
informational — they record that the engine chose the CPU reference path
without indicating an error.

For native compile failures, codes beginning with `NATIVE.` indicate the
specific failure. `NATIVE.COMPILE_FAILED_FALLBACK` records a successful
fallback to the interpreter; it is not an error in optional profiles.

Both subsystems emit metrics under the `sb_gpu_*` and `sb_native_compile_*`
metric families. These are accessible through `SHOW ACCELERATION EXTENDED`
and through the standard metrics surface.

---

## Operator Safety Summary

| Action | Effect on results |
|--------|------------------|
| Disable any acceleration profile | No effect on results; speed may decrease |
| Quarantine any artifact, device, or kernel | No effect on results; engine falls back |
| Clear or invalidate any cache | No effect on results; recompilation occurs on next use |
| Remove AOT artifact files | No effect on results; AOT rebuild or JIT occurs on next use |
| Revert to `disabled` profile (all tiers) | Interpreter used; results guaranteed correct |

The fundamental invariant is: **the SBLR interpreter result is always the
correct result**. Acceleration changes how that result is computed, never what
it is.

---

## Cross-Links

- [README.md](#ch-acceleration-guide-readme-md) — guide overview
- [acceleration_authority_model.md](#ch-acceleration-guide-acceleration-authority-model-md) — the fail-closed guarantee in detail
- [execution_tiers.md](#ch-acceleration-guide-execution-tiers-md) — three-tier stack and fallback discipline
- [native_compilation.md](#ch-acceleration-guide-native-compilation-md) — profile names, AOT artifact format
- [gpu_and_simd_acceleration.md](#ch-acceleration-guide-gpu-and-simd-acceleration-md) — GPU refusal codes, catalog tables
- [../Operations_Administration/README.md](#ch-operations-administration-readme-md)
- ../Language_Reference/syntax_reference/ebnf/alter_acceleration.md (SBsql Language Reference — Syntax, page XXX)
- ../Language_Reference/syntax_reference/ebnf/show_acceleration.md (SBsql Language Reference — Syntax, page XXX)




<a id="ch-glossary"></a>

# Glossary

## Purpose

This glossary defines terms used across the ScratchBird documentation set. The
definitions are written for end users, evaluators, operators, and developers.
They are intentionally concise and cautious: a term appearing here does not mean
the related feature is complete, enabled, or available in every build.

## ScratchBird Product Names

| Term | Meaning |
| --- | --- |
| ScratchBird | The project and product line described by this documentation. |
| ScratchBird Convergent Data Engine | The full product concept: engine, parsers, tools, resources, and operational surfaces. |
| SB | Short brand form used in names and examples. |
| SBcore | ScratchBird Engine. The embedded engine library that owns durable catalog identity, transactions, storage, security admission, recovery decisions, and engine diagnostics. |
| SBsql | ScratchBird SQL. The native ScratchBird command language and script runner surface. |
| SBParser | ScratchBird Core Parser. The native SBsql parser package that lowers SBsql requests to SBLR. |
| SBsrv | ScratchBird IPC Server. A local multi-user server process for same-machine clients. |
| SBgate | ScratchBird Listener. The listener and parser-facing entry point used for network-facing client traffic. |
| SBmgr | ScratchBird Single Node Manager. A single-node front door that can proxy authenticated connections to internal listener routes in managed deployments. |
| SBadm | ScratchBird Administrator. Administrative utility name for configuration, time zone, character set, collation, and policy management where present. |
| SBbak | ScratchBird Backup Manager. Utility name for backup and backup-set operations where present. |
| SBsec | ScratchBird Security. Utility name for security provider, user, role, group, and policy management where present. |
| SBdoc | ScratchBird Doctor. Utility name for analysis, diagnosis, and repair-oriented workflows where present and admitted. |
| SBcop | ScratchBird Conformance Officer. Utility name for conformance and comparison checks where present. |

## Architecture Terms

| Term | Meaning |
| --- | --- |
| Convergent Data Engine | An engine design that attempts to bring multiple data shapes, parser surfaces, transaction rules, security rules, and diagnostics under one shared engine authority model. |
| CDE | Abbreviation for Convergent Data Engine. |
| Engine authority | The rule that durable behavior belongs to SBcore: object identity, descriptors, transactions, security admission, storage, recovery, and diagnostics. |
| Parser boundary | The separation between a client language or wire protocol and engine execution authority. |
| Parser package | A component that accepts a specific language or protocol surface and lowers accepted work to ScratchBird execution requests. |
| Compatibility parser | A standalone parser package for one reference-system client family. It should not silently accept unrelated dialects. |
| SBsql language profile | A parser resource profile that can change user-facing SBsql spellings, phrase order, diagnostics, completion hints, and source rendering without changing SBLR, UUID identity, descriptors, security, storage, or MGA transaction authority. |
| Canonical element stream | The normalized parser output created before UUID binding. It records canonical token and surface identities rather than treating localized words as engine authority. |
| Standard SBsql fallback | A policy-controlled input fallback that lets a non-English session accept canonical English SBsql when the preferred language profile does not parse the statement. |
| Parser route | The configured path that determines which parser handles a client request. |
| SBLR | ScratchBird's bound engine-facing request representation. Parsers emit SBLR after parsing and binding accepted work. |
| Bound request | A structured request whose names, values, parameters, and types have been resolved enough to submit toward engine authority. |
| Raw text | The command text received from a client before parsing. Raw text is not durable engine authority. |
| Catalog projection | A view or metadata surface that presents engine catalog information in a particular shape for a parser, tool, or user. |
| Workarea | A schema-root area presented to a parser or user as its operating root. |
| Compatibility surface | The subset of behavior a parser or tool is designed and proven to accept, execute, or refuse clearly. |
| Refusal | A controlled response that says a request is unsupported, denied, unavailable, unsafe, or otherwise not admitted. |

## Database And Catalog Terms

| Term | Meaning |
| --- | --- |
| Database | A managed durable store of data, metadata, identity, transactions, security rules, diagnostics, and recovery behavior. |
| Metadata | Information that describes data, such as schemas, tables, columns, types, constraints, indexes, views, routines, grants, policies, and catalog rows. |
| Catalog | Engine-owned metadata that describes durable database objects and their relationships. |
| Catalog identity | The durable identity of a catalog object, separate from the user-facing name used to spell it. |
| UUID identity | Durable object identity based on UUIDs rather than only text names. |
| Object descriptor | Engine metadata describing an object shape, type, storage behavior, dependency, or operational capability. |
| Type descriptor | Metadata describing a datatype, its value behavior, binary representation, capabilities, and related rules. |
| Domain | A reusable constrained type definition. |
| Constraint | A rule attached to a table, column, domain, or related object. |
| Index | A search structure maintained for faster lookup, ordering, constraint enforcement, or query planning where implemented. |
| View | A named query projection. |
| Materialized view | A stored projection whose refresh and dependency behavior must be defined by the implementation. |
| Procedure | A stored routine that can perform controlled work and may return output parameters or result sets where supported. |
| Function | A routine that returns a value or result. |
| Package | A named grouping of routine definitions where supported. |
| Trigger | Routine behavior tied to table, database, transaction, or event-style actions where implemented. |
| Sequence | A database object that generates ordered values according to its definition. |
| Comment | Descriptive metadata attached to an object. Comments do not grant authority and should not contain secrets. |

## Schema And Name Terms

| Term | Meaning |
| --- | --- |
| Schema | A namespace branch that can contain objects and, where supported, child schemas. |
| Recursive schema tree | A schema model where schemas can contain child schemas, creating a tree rather than one flat namespace. |
| Database root | The top of the durable database tree. Not every session can see it directly. |
| Parser-visible root | The root of the namespace presented to the selected parser route. |
| Home schema | The schema associated with a user, identity, or configured workarea. |
| Current schema | The default schema used for unqualified names in a session. |
| Search path | An ordered lookup path used by commands that allow path-based name resolution. |
| Qualified name | A name that includes schema or path information, such as `app.notes`. |
| Unqualified name | A name without schema qualification, such as `notes`. |
| Name resolution | The process of turning a user-visible name into engine object identity. |
| Sandbox | The visible boundary that limits what a session or parser route can name, inspect, or access. |
| Schema branch | A subtree of the database namespace. |
| Object lifecycle | The create, alter, rename, comment, describe, use, refresh, validate, or drop actions that apply to an object type. |

## Transaction And Recovery Terms

| Term | Meaning |
| --- | --- |
| Transaction | A boundary around work that can commit, roll back, and participate in visibility rules. |
| Commit | Make a transaction's admitted changes final according to engine visibility rules. |
| Rollback | Discard uncommitted transaction changes. |
| Savepoint | A named point inside a transaction that can be rolled back without ending the whole transaction where supported. |
| Autocommit | A mode where each statement may be committed automatically according to session and parser rules. |
| MGA | ScratchBird's transaction and visibility authority model. In this documentation, the key rule is that transaction finality belongs to the engine. |
| Visibility | The rule that determines which transaction versions a session can see. |
| Cleanup | Engine-controlled work that reclaims or resolves old transaction state when it is safe. |
| Recovery | The process of reopening or refusing a database after shutdown, interruption, or uncertain durable state. |
| Recovery-required state | A state where the engine requires recovery handling before normal writes can proceed. |
| Fail closed | Refuse work when the safe outcome is uncertain instead of silently accepting it. |
| Reopen proof | A test that closes and reopens a database to verify committed state is still present. |

## Security Terms

| Term | Meaning |
| --- | --- |
| Identity | The authenticated user, service, or agent identity attached to a session or operation. |
| Principal | A user, role, group, service, or other authority-bearing identity. |
| Authentication | Establishing who the session or agent is. |
| Authorization | Deciding what an authenticated identity is allowed to do. |
| Grant | A permission given to a principal or object. |
| Revoke | Removal of a previously granted permission. |
| Role | A named set of privileges that can be granted and activated according to policy. |
| Policy | A rule that controls access, masking, row visibility, external access, operational admission, or protected material use. |
| Row-level security | Policy behavior that limits which rows a session can see or change. |
| Mask | A policy-controlled transformation that hides or changes protected values in query output. |
| Protected material | Secrets or sensitive values that require controlled storage, reference, redaction, and use. |
| Secret reference | A reference to protected material without placing the raw secret in a parser packet, script, or diagnostic. |
| Materialized authorization | Authorization information loaded into an engine-admissible form before work is executed. |
| Denied | A refusal because the authenticated identity, policy, or sandbox does not admit the operation. |

## Data And Type Terms

| Term | Meaning |
| --- | --- |
| Datatype | A named value category with storage, comparison, conversion, and validation behavior. |
| Scalar value | A single value such as an integer, timestamp, boolean, UUID, or text value. |
| Numeric type | A datatype for integer, unsigned integer, decimal, fixed-point, or floating-point values. |
| Text type | A datatype for character data governed by character set and collation rules. |
| Character set | The encoding rules for text values. |
| Collation | The comparison and ordering rules for text values. |
| Temporal type | A datatype for dates, times, timestamps, intervals, or time-zone-aware values. |
| UUID | A fixed-size identifier value commonly used for durable identity. |
| Binary value | A sequence of bytes with type-specific interpretation. |
| Protected value | A value governed by protected-material policy. |
| Document value | A structured value such as JSON-like data where implemented. |
| Graph value | A relationship-oriented value or model surface where implemented. |
| Vector value | A numeric vector used for similarity or embedding-style operations where implemented. |
| Time-series value | A value or record organized around time-oriented measurement behavior where implemented. |
| Coercion | An implicit or explicit conversion between compatible types. |
| Cast | An explicit type conversion requested by the user. |
| Null | A marker for absence of a value, distinct from zero, empty string, or false. |

## Query Terms

| Term | Meaning |
| --- | --- |
| DDL | Data definition language: commands that create, alter, describe, comment on, rename, or drop database objects. |
| DML | Data manipulation language: commands that read or change rows and values. |
| Query | A request that reads data and returns a result set or scalar result. |
| Result set | Rows and columns returned by a query or routine. |
| Projection | The selected output columns or expressions of a query. |
| Predicate | A condition used to filter rows or control logic. |
| Join | A query operation that combines rows from more than one source. |
| Grouping | A query operation that forms groups of rows for aggregate calculations. |
| Aggregate | A calculation over multiple rows, such as count or sum where implemented. |
| Window function | A calculation over a window of rows related to the current row where implemented. |
| CTE | Common table expression. A named temporary query expression inside a statement. |
| Recursive CTE | A CTE that refers to itself according to the rules of the language surface. |
| Ordering | The explicit sort order requested for result rows. |
| Limit | A request to return only a bounded number of rows. |
| Offset | A request to skip a number of rows before returning results. |
| Upsert | Insert-or-update behavior according to a conflict rule where supported. |
| Merge | A statement that conditionally inserts, updates, or deletes based on a source relation where supported. |
| Copy | A large or streaming data input or output surface where implemented and admitted. |

## Procedural Terms

| Term | Meaning |
| --- | --- |
| Procedural SQL | Stored routine language constructs such as blocks, variables, control flow, cursors, exceptions, and triggers where implemented. |
| Block | A procedural unit containing declarations and executable statements. |
| Variable | A named procedural value local to a routine or block. |
| Cursor | A controlled handle over a result set. |
| Result-set cursor | A cursor passed or returned as a routine-controlled result where supported. |
| Exception handler | Procedural logic that handles a diagnostic or error condition. |
| Event trigger | Trigger-style behavior tied to database, transaction, or event actions where implemented. |
| UDR | User-defined routine or parser-support routine package, depending on context. In parser documentation, it commonly means the package that supports bridge or extension behavior for that parser. |
| Bridge | A controlled connection or interface used by a parser-support routine to reach another database surface where configured and admitted. |

## Operations And Data Movement Terms

| Term | Meaning |
| --- | --- |
| Configuration | Settings that control startup, resource locations, parser registration, security providers, policy defaults, and runtime behavior. |
| Resource file | A staged file needed by the product, such as character set, collation, time zone, policy, or configuration data. |
| Health check | A diagnostic check that reports whether a component appears alive and able to answer. |
| Readiness check | A diagnostic check that reports whether a component is ready to accept intended work. |
| Liveness check | A diagnostic check that reports whether a component is still running. |
| Support bundle | A redacted package of diagnostic evidence for review or support. |
| Redaction | Removing or masking protected material before diagnostics are shown or bundled. |
| Message vector | Structured diagnostic output used for errors, refusals, and operational status. |
| Logical stream | Data movement represented as statements, rows, records, or events rather than physical page files. |
| Logical backup | A backup stream that represents database content as logical metadata and data operations. |
| Logical restore | Replaying a logical stream as admitted database operations. |
| Physical backup | A page-copy or file-copy backup shape. Compatibility parser routes should not treat physical page-copy formats as normal logical restore input. |
| Import | Bring external logical data into a database through an admitted parser or tool route. |
| Export | Write logical data from a database to an external stream or file according to policy. |
| CDC | Change data capture. A stream or record of changes suitable for replication, ETL, or integration where implemented. |
| Replication | Copying changes between systems according to an ordering and identity model where implemented. |
| ETL | Extract, transform, load. A data movement workflow that reads from one source, transforms, and writes to another target. |
| Migration | Moving schema, data, routines, security, or operational behavior from one database shape to another. |
| Quarantine | Holding questionable incoming records or events aside for review instead of applying them silently. |
| Cutover | The controlled switch from one active source or route to another. |
| Idempotency key | A value used to detect repeated events or operations so replay can be handled safely. |

## Build And Release Terms

| Term | Meaning |
| --- | --- |
| Build output | The generated binaries, libraries, parser packages, resources, and configuration artifacts for a target platform. |
| Output tree | The staged directory layout intended for testing or release packaging. |
| Target platform | The operating system and architecture being built or tested. |
| Proof gate | A test or validation step intended to prove that a behavior remains implemented and has not regressed. |
| CTest | The test runner integration used by many project tests. |
| Conformance test | A test that compares behavior against a declared specification, parser expectation, or compatibility target. |
| Smoke test | A small test proving that a basic workflow starts, runs, and stops. |
| Regression test | A test intended to prevent a previously handled behavior from breaking again. |
| Draft documentation | Documentation under active review. Draft status means users should verify commands and claims against the current build and tests. |



# About This Documentation

This book is part of the ScratchBird documentation set. ScratchBird is a
Convergent Data Engine (CDE).

**Draft status.** This is draft documentation. It describes the architecture and
intended behavior of the source tree. A topic appearing here does not by itself
guarantee that a feature is complete, enabled, performant, certified, or
available in any particular build. Always confirm against the current build,
configuration, tests, and release notes.

**License.** The ScratchBird engine is distributed under the Mozilla Public
License 2.0 (MPL-2.0). This documentation describes that open-source engine.

**No certification claim.** Nothing in this documentation constitutes a security
certification, performance benchmark, or compatibility guarantee.

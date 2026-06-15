# Architecture: Adapter and Bridge

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter describes how the ScratchBird AI layer connects to a running
ScratchBird server. It covers the three adapter modes, the local HTTP bridge,
the compile/execute split, and the transport/front-door/IPC mode options.

---

## Layered Architecture Overview

```
AI Client (MCP tool calls)
        |
        v
  MCP Server  (scratchbird_ai.mcp_server)
        |
        v
  AI Service  (scratchbird_ai.service / ScratchBirdAIService)
        |
        v
  Adapter Layer  (scratchbird_ai.adapters)
        |
     [mode]
   mock | http | hybrid
        |
        v (http / hybrid only)
  Local HTTP Bridge  (scratchbird_ai.http_bridge)
  port 3095 (default)
        |
        v
  ScratchBird Server
  (native listener, manager proxy, IPC, or embedded)
```

The adapter layer is the seam between the service and the ScratchBird server.
Which adapter mode is active determines whether requests go to a real server,
a stub, or a combination.

---

## Adapter Modes

The adapter mode is set by `SCRATCHBIRD_AI_ADAPTER_MODE`. Three values are
supported:

| Mode | Behavior |
| --- | --- |
| `mock` | All adapter calls return deterministic stub responses. No bridge or real server is needed. This is the default. Used for local testing and development without a live ScratchBird target. |
| `http` | All adapter calls are forwarded to the local HTTP bridge over `SCRATCHBIRD_AI_HTTP_BASE_URL`. A running bridge is required. Used for live integration and certification runs. |
| `hybrid` | Dialects listed in `SCRATCHBIRD_AI_HTTP_DIALECTS` are forwarded to the bridge; others fall back to mock. The default dialect CSV is `native`. |

The default is `mock`. Enabling `http` or `hybrid` requires a running bridge
instance (see below).

---

## The Local HTTP Bridge

The local HTTP bridge (`scratchbird_ai.http_bridge`) is a lightweight HTTP
server that exposes compile, execute, and metadata endpoints that the adapter
calls. It holds the actual ScratchBird Python driver connection and handles the
transport-mode details.

**Default bind address and port:**

- Host: `127.0.0.1` (controlled by `SCRATCHBIRD_AI_BRIDGE_HOST`)
- Port: `3095` (controlled by `SCRATCHBIRD_AI_BRIDGE_PORT`)

**Bridge endpoints exposed:**

- compile
- execute
- metadata (schemas, tables, describe)
- health/dialect checks

**Bridge auth:** The bridge optionally requires a Bearer token set by
`SCRATCHBIRD_AI_BRIDGE_API_TOKEN`. The adapter presents the matching token via
`SCRATCHBIRD_AI_HTTP_API_TOKEN`.

**Starting the bridge:**

```bash
# POSIX (Ubuntu/Linux)
PYTHONPATH=src tools/run_local_bridge.sh

# Direct Python module
python3 -m scratchbird_ai.http_bridge

# Windows PowerShell
python -m scratchbird_ai.http_bridge
# or: scratchbird-ai-http-bridge (if installed as console script)
```

---

## The Compile/Execute Split

The AI layer enforces a two-step contract for query execution:

1. **Compile** — `compile_query(dialect, query_text)` submits query text to
   the ScratchBird parser/compiler and returns a `compile_artifact_id`.
   The artifact identifier is stable and traceable.

2. **Execute** — `execute_compiled(compile_artifact_id, options, mode)` runs
   the pre-compiled form. It does not accept raw query text.

This split means:

- The compiler validates syntax before any execution attempt.
- Compiled artifacts carry trace IDs and feed into audit bundles.
- The execution mode (`ai_analysis` by default) can be changed at the execute
  step without recompiling.
- Bounded compile-repair can attempt to strip common wrapper noise from query
  text before compilation (see [governance_quotas_and_audit.md](./governance_quotas_and_audit.md)).

The `run_query` and `execute_readonly_query` tools combine compile and execute
in one call for convenience, but they still go through the same split
internally.

---

## Server Setup Profiles

The `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP` variable selects the ScratchBird
server connection mode the bridge will use. Valid values:

| Value | Maps to | Description |
| --- | --- | --- |
| `listener-only` | `listener_direct` transport | Direct TCP connection to the native ScratchBird listener. Default and primary Beta 1 certified path. |
| `managed` | `manager_proxy` transport | Connection through the ScratchBird manager proxy. Requires `SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN`. |
| `ipc-only` | `local_ipc` transport | Local IPC socket/pipe connection. Requires the Python driver to support IPC transport. |
| `embedded` | `embedded_local_only` transport | In-process embedded mode. Single-connection; non-shared. Requires driver/runtime support. |

Several aliases are accepted (for example, `listener`, `tcp`, `inet_listener`
all map to `listener-only`; `manager_proxy`, `mcp` map to `managed`).

**For the current Beta 1, use `listener-only` as the primary path.** The
`managed`, `ipc-only`, and `embedded` modes are admitted in ScratchBird core
and implemented, but live certification evidence for those modes is still
environment-dependent.

---

## Transport, Front-Door, and IPC Overrides

For advanced deployments, individual aspects of the connection can be overridden
independently of the server setup profile:

| Variable | Purpose |
| --- | --- |
| `SCRATCHBIRD_AI_BRIDGE_TRANSPORT_MODE` | Explicit transport override: `inet_listener`, `managed`, `local_ipc`, `embedded` |
| `SCRATCHBIRD_AI_BRIDGE_FRONT_DOOR_MODE` | Explicit front-door override: `direct` or `manager_proxy` |
| `SCRATCHBIRD_AI_BRIDGE_IPC_METHOD` | IPC method override: `auto`, `unix`, `pipe`, `tcp` |
| `SCRATCHBIRD_AI_BRIDGE_IPC_PATH` | IPC socket or pipe path for `ipc-only` mode |

These overrides are intended for environment-specific deployment needs. For
most installations, setting `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP` is sufficient.

**Note:** `ipc-only` and `embedded` require a Python driver/runtime that
supports those transport modes. Do not configure them until the target
environment and driver are confirmed to support them.

---

## DSN Configuration

The bridge uses a DSN (Data Source Name) to locate the ScratchBird server:

| Variable | Purpose |
| --- | --- |
| `SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN` | Fallback DSN for all enabled dialects |
| `SCRATCHBIRD_AI_BRIDGE_DSN_<DIALECT>` | Per-dialect DSN override (for example, `SCRATCHBIRD_AI_BRIDGE_DSN_NATIVE`) |

The DSN format used in the shipped example is:

```
scratchbird://user:password@127.0.0.1:3092/mydb
```

Endpoint state (host, port, database, user) is deployment-specific and must
be supplied via the configured profile or environment. These values are never
committed in the repository.

---

## Dialect Filtering

The bridge only serves dialects listed in `SCRATCHBIRD_AI_BRIDGE_DIALECTS`
(default `native`). Requests for a dialect not in the list return
`404 Dialect not enabled`. The adapter side mirrors this with
`SCRATCHBIRD_AI_HTTP_DIALECTS`.

For the current release, include `native` in both variables.

---

## Strict Compile Mode

When `SCRATCHBIRD_AI_BRIDGE_STRICT_COMPILE=1`, a compile probe failure at the
bridge endpoint returns HTTP 400 instead of a warning. In the default `0` state,
compile probe failures are logged but do not abort the bridge startup. Use
strict mode in environments where a failed compile probe should be treated as
a hard error.

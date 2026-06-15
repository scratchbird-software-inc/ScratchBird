# Runtime Configuration

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter is the complete reference for environment variables that configure
the ScratchBird AI adapter, bridge, and governance controls. It also describes
the runtime profile model and how secrets are supplied.

The canonical source for bridge variable names is
`project/ai/examples/http-bridge.env.example`.

---

## Runtime Profile Model

Connection metadata is policy-supplied at runtime, not hardcoded in the
repository. Two environment variables point to the active profile:

| Variable | Purpose |
| --- | --- |
| `SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH` | Path to the runtime environment file (`runtime.env`) that describes the active ScratchBird server and its published listener addresses |
| `SCRATCHBIRD_AI_CONNECTION_PROFILE_PATH` | Path to a `connections.json` profile file that maps profile names to connection parameters |

Endpoint state (host, port, database, user, secrets) is read from the
configured profile. These values are deployment-specific and must never be
committed in repository files.

Native ScratchBird lanes are in scope. Reference emulation lanes
(PostgreSQL/MySQL/Firebird) may appear in a deployment profile, but AI support
for non-native modes remains out of scope for this component.

Secrets are supplied by the configured secret provider or a test fixture. The
secret reference format used in direct native client probes is
`--password-ref <secret-ref>`.

---

## Adapter Environment Variables

These variables configure the adapter layer that sits between the AI service
and the bridge (or mock):

| Variable | Default | Description |
| --- | --- | --- |
| `SCRATCHBIRD_AI_ADAPTER_MODE` | `mock` | Adapter operation mode: `mock`, `http`, or `hybrid` |
| `SCRATCHBIRD_AI_HTTP_BASE_URL` | _(none)_ | Base URL for HTTP adapter calls (required for `http` and `hybrid` modes) |
| `SCRATCHBIRD_AI_HTTP_TIMEOUT_SEC` | _(unset)_ | Timeout in seconds for HTTP adapter requests |
| `SCRATCHBIRD_AI_HTTP_RETRY_ATTEMPTS` | _(unset)_ | Retry count for retryable bridge requests (`GET` and `compile`) |
| `SCRATCHBIRD_AI_HTTP_RETRY_BACKOFF_MS` | _(unset)_ | Retry backoff in milliseconds |
| `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_FAILURE_THRESHOLD` | _(unset)_ | Consecutive bridge failures before the circuit opens |
| `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_COOLDOWN_SEC` | _(unset)_ | Cooldown in seconds before the bridge circuit closes again |
| `SCRATCHBIRD_AI_HTTP_API_TOKEN` | _(none)_ | Optional Bearer token presented to the bridge |
| `SCRATCHBIRD_AI_HTTP_DIALECTS` | `native` | Dialect CSV used for `hybrid` mode |
| `SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH` | _platform user state dir_ | Path to the durable approval evidence ledger |
| `SCRATCHBIRD_AI_COMPILE_REPAIR_MAX_ATTEMPTS` | _(unset)_ | Maximum number of compile-repair attempts for recoverable wrapper noise |
| `SCRATCHBIRD_AI_OPERATION_WINDOW_SEC` | _(unset)_ | Rolling window size for quota enforcement (seconds) |
| `SCRATCHBIRD_AI_MAX_REQUESTS_PER_WINDOW` | _(unset)_ | Maximum requests per quota window |
| `SCRATCHBIRD_AI_MAX_MUTATIONS_PER_WINDOW` | _(unset)_ | Maximum mutation operations per quota window |
| `SCRATCHBIRD_AI_MAX_COST_UNITS_PER_WINDOW` | _(unset)_ | Maximum cost units per quota window |
| `SCRATCHBIRD_AI_SUPPORTED_SERVER_VERSIONS` | _(unset)_ | Comma-separated accepted ScratchBird server versions (fail-closed if set) |
| `SCRATCHBIRD_AI_SUPPORTED_PARSER_COMPILER_VERSIONS` | _(unset)_ | Accepted parser/compiler versions |
| `SCRATCHBIRD_AI_SUPPORTED_DRIVER_RUNTIME_VERSIONS` | _(unset)_ | Accepted driver runtime versions |

---

## Bridge Environment Variables

These variables configure the local HTTP bridge process:

### Core Bridge Settings

| Variable | Default | Description |
| --- | --- | --- |
| `SCRATCHBIRD_AI_BRIDGE_HOST` | `127.0.0.1` | Bridge bind host |
| `SCRATCHBIRD_AI_BRIDGE_PORT` | `3095` | Bridge bind port |
| `SCRATCHBIRD_AI_BRIDGE_API_TOKEN` | _(none)_ | Optional Bearer token that clients must present |
| `SCRATCHBIRD_AI_BRIDGE_DIALECTS` | `native` | Comma-separated list of enabled dialects |
| `SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN` | _(none)_ | Fallback DSN for all enabled dialects |
| `SCRATCHBIRD_AI_BRIDGE_DSN_<DIALECT>` | _(none)_ | Per-dialect DSN override; for example, `SCRATCHBIRD_AI_BRIDGE_DSN_NATIVE` |

### Server Setup and Transport

| Variable | Default | Description |
| --- | --- | --- |
| `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP` | `listener-only` | Server connection profile: `listener-only`, `managed`, `ipc-only`, or `embedded` |
| `SCRATCHBIRD_AI_BRIDGE_TRANSPORT_MODE` | _(derived)_ | Explicit transport override: `inet_listener`, `managed`, `local_ipc`, `embedded` |
| `SCRATCHBIRD_AI_BRIDGE_FRONT_DOOR_MODE` | _(derived)_ | Explicit front-door override: `direct` or `manager_proxy` |
| `SCRATCHBIRD_AI_BRIDGE_IPC_METHOD` | _(auto)_ | IPC method: `auto`, `unix`, `pipe`, `tcp` |
| `SCRATCHBIRD_AI_BRIDGE_IPC_PATH` | _(none)_ | IPC socket or pipe path for `ipc-only` mode |

### Managed (Manager Proxy) Settings

These variables are required when `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP=managed`.
The `MANAGER_` prefix and the `MCP_` prefix are aliases for the same setting:

| Variable | Default | Description |
| --- | --- | --- |
| `SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN` / `SCRATCHBIRD_AI_BRIDGE_MCP_AUTH_TOKEN` | _(none)_ | Manager signon token (required for managed mode) |
| `SCRATCHBIRD_AI_BRIDGE_MANAGER_USERNAME` / `SCRATCHBIRD_AI_BRIDGE_MCP_USERNAME` | _(none)_ | Manager username override |
| `SCRATCHBIRD_AI_BRIDGE_MANAGER_DATABASE` / `SCRATCHBIRD_AI_BRIDGE_MCP_DATABASE` | _(none)_ | Managed database override |
| `SCRATCHBIRD_AI_BRIDGE_MANAGER_CONNECTION_PROFILE` / `SCRATCHBIRD_AI_BRIDGE_MCP_CONNECTION_PROFILE` | `native_v3` | Managed connection profile |
| `SCRATCHBIRD_AI_BRIDGE_MANAGER_CLIENT_INTENT` / `SCRATCHBIRD_AI_BRIDGE_MCP_CLIENT_INTENT` | `native_v3` | Managed client intent |
| `SCRATCHBIRD_AI_BRIDGE_MANAGER_CLIENT_FLAGS` / `SCRATCHBIRD_AI_BRIDGE_MCP_CLIENT_FLAGS` | `0` | Managed client flags (0–65535) |
| `SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_FAST_PATH` / `SCRATCHBIRD_AI_BRIDGE_MCP_AUTH_FAST_PATH` | `true` | Managed fast-path auth toggle |

### Driver and Compile Settings

| Variable | Default | Description |
| --- | --- | --- |
| `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC` | _(none)_ | Path to the ScratchBird Python driver `src/` directory when the driver is not pip-installed |
| `SCRATCHBIRD_AI_BRIDGE_STRICT_COMPILE` | `0` | When `1`, compile probe failure returns HTTP 400 instead of a warning |

---

## Governance and Logging Variables

These variables configure audit attestation and structured logging paths:

| Variable | Default | Description |
| --- | --- | --- |
| `SCRATCHBIRD_AI_STRUCTURED_EVENT_LOG_PATH` | _platform user state dir_ | Path for JSONL structured event log |
| `SCRATCHBIRD_AI_OPERATOR_BUNDLE_OUTPUT_DIR` | _platform user state dir_ | Directory for operator runbook bundle output |
| `SCRATCHBIRD_AI_AUDIT_ATTESTATION_MODE` | _(unset)_ | Attestation mode: `hmac_sha256` or `external_reference` |
| `SCRATCHBIRD_AI_AUDIT_ATTESTATION_SECRET` | _(none)_ | Shared secret for HMAC attestation mode |
| `SCRATCHBIRD_AI_AUDIT_ATTESTATION_ATTESTOR_ID` | _(none)_ | Attestor identity included in attestation records |
| `SCRATCHBIRD_AI_RUNTIME_ROOT` | _(platform user state)_ | Override for the runtime root directory used for default ledger, log, and bundle paths |

---

## Live Certification Variables

These variables are used by the live certification harness
(`tools/run_live_native_conformance.py`):

| Variable | Description |
| --- | --- |
| `SCRATCHBIRD_AI_LIVE_NATIVE_ENABLED` | Set to `1` to enable live native certification runs |
| `SCRATCHBIRD_AI_LIVE_NATIVE_LAUNCH_BRIDGE` | Set to `1` to have the harness launch the bridge automatically |
| `SCRATCHBIRD_AI_LIVE_NATIVE_SCRATCHBIRD_SERVER_VERSION` | Declared server version for the certification run |
| `SCRATCHBIRD_AI_LIVE_NATIVE_PARSER_COMPILER_VERSION` | Declared parser/compiler version |
| `SCRATCHBIRD_AI_LIVE_NATIVE_TEST_DATASET_VERSION` | Declared test dataset version |
| `SCRATCHBIRD_AI_LIVE_NATIVE_SEED_VERSION` | Declared seed/fixture version |

---

## Recommended Development Baseline

The following baseline is drawn from
`docs/guides/RUNTIME_CONFIGURATION_AND_GOVERNED_OPERATION.md`. Use it as a
starting point for a local development or CI environment:

```bash
export SCRATCHBIRD_AI_ADAPTER_MODE=http
export SCRATCHBIRD_AI_HTTP_BASE_URL=http://127.0.0.1:3095
export SCRATCHBIRD_AI_HTTP_DIALECTS=native
export SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH=artifacts/runtime/approval_ledger.json
export SCRATCHBIRD_AI_STRUCTURED_EVENT_LOG_PATH=artifacts/runtime/structured_events.jsonl
export SCRATCHBIRD_AI_OPERATOR_BUNDLE_OUTPUT_DIR=artifacts/runtime/operator_bundle
export SCRATCHBIRD_AI_AUDIT_ATTESTATION_MODE=hmac_sha256
export SCRATCHBIRD_AI_AUDIT_ATTESTATION_SECRET=replace-with-local-attestation-secret
export SCRATCHBIRD_AI_COMPILE_REPAIR_MAX_ATTEMPTS=3
export SCRATCHBIRD_AI_OPERATION_WINDOW_SEC=60
export SCRATCHBIRD_AI_MAX_REQUESTS_PER_WINDOW=100
export SCRATCHBIRD_AI_MAX_MUTATIONS_PER_WINDOW=20
export SCRATCHBIRD_AI_MAX_COST_UNITS_PER_WINDOW=1000
export SCRATCHBIRD_AI_HTTP_RETRY_ATTEMPTS=1
export SCRATCHBIRD_AI_HTTP_RETRY_BACKOFF_MS=100
export SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_FAILURE_THRESHOLD=3
export SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_COOLDOWN_SEC=30
```

---

## Example Bridge Configuration File

The file `project/ai/examples/http-bridge.env.example` contains a fully
commented template for bridge variables. Copy and edit it for local use:

```bash
cp examples/http-bridge.env.example .env.bridge
```

Source it before launching the bridge:

```bash
set -a
source .env.bridge
set +a
PYTHONPATH=src tools/run_local_bridge.sh
```

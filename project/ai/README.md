# ScratchBird AI

ScratchBird AI is the AI integration layer for ScratchBird.
This repository contains the MCP-oriented service layer, dialect-aware query orchestration, HTTP adapter and bridge runtime, and deterministic governance helpers used to connect AI workflows to ScratchBird parser/compiler and execution paths.

Current release track: **current early beta / technical Beta 1 review baseline** (`0.1.0`)
Status timestamp: **April 20, 2026**

## Support Policy

ScratchBird AI supports **ScratchBird native engine workflows only**.

- Native-only AI support is in scope for this repository.
- Emulated external engines are out of scope for this repository's AI layer.
- Non-native dialect requests are rejected with explicit policy errors.
- ScratchBird engine execution boundary remains `ServerSession`; SQL must be compiled to SBLR before engine submission.

## Current Early-Beta Surface

Included in the current baseline:

- MCP-oriented service orchestration with canonical tool declarations.
- Safe-by-default policy path with read-only mode and approval-gated mutation mode.
- Compile/execute split orchestration with artifact identifiers, trace IDs, and audit bundles.
- Dialect capability matrix loader and native-only routing gates.
- HTTP adapter mode (`mock`, `http`, `hybrid`) for parser/executor integration.
- Local HTTP bridge implementation for adapter contract testing and live driver-backed access.
- Engine-free vector and hybrid retrieval helpers with deterministic ranking.
- Machine-readable publication of the completed bounded ScratchBird core AI surface packet.
- Bounded compile-repair for recoverable wrapped query text.
- Durable approval evidence ledger for governed mutation execution.
- Structured runtime event logging plus runtime diagnostics and operator runbook/SLO bundle generation.
- Approval-record operator workflow, audit bundle listing, and audit attestation issue/verify helpers.
- In-process quotas, rate limits, cost attribution, and HTTP retry/circuit-breaker controls.
- Fail-closed compatibility negotiation for declared ScratchBird server/parser/runtime versions.
- Deterministic plan hashing, execution-mode evaluation, audit replay, and cluster-routing helpers.
- ScratchBird-native control surface publication for graph operations, remote MCP, registry/routing controls, and bridge/runtime management families.
- Remote MCP session lifecycle with advertised auth families including bearer, OAuth2/JWT bearer, workload identity, proxy principal, LDAP bind, Kerberos/GSSAPI, RADIUS PAP, PAM conversation, and preauthenticated context.
- Expanded live-native harness depth for service-internal explain/workload/audit replay and retrieval contract probes.
- Release evidence generation and validation for the implemented early-beta surface.

Not included in this release:

- Production-grade authz depth, third-party signing infrastructure, and full multi-tenant hard isolation.
- AI support for non-native emulated engine modes.
- Automatic live certification for runtime modes not exposed in the active test environment.

Current truth note:

- ScratchBird core now owns a completed bounded current-tree AI surface over
  engine-owned retrieval, retrieval metadata discovery, and runtime-mode truth.
- ScratchBird AI publishes that core truth in capabilities and manifests.
- The AI repo now includes structured runtime governance packaging and
  runtime-mode-aware live-native certification metadata.
- Direct-listener live-native certification must be regenerated from the
  current checkout and target runtime before claiming a live-native profile.
- The active runtime profile is supplied by policy using `runtime.env` and
  `connections.json`; no user-local profile path is a default.
- Native ScratchBird endpoint, database, user, and secret values are deployment
  configuration, not repository constants.
- The PostgreSQL, MySQL, and Firebird emulation lanes are profile-defined on
  `127.0.0.1:15432`, `127.0.0.1:13306`, and `127.0.0.1:13050`, but they are
  stopped by default and are not published until those listeners are started.
- Compat/public identities exist for native and donor lanes in the shared
  profile, but donor-lane AI support remains out of scope for this repository.
- Live evidence still needs to be rerun whenever the target environment is
  refreshed or when certifying a runtime mode that is not currently exposed in
  the shared harness.

## Quick Start

Platform-specific install guides:

- Ubuntu/Linux reviewer path: `docs/guides/INSTALL_UBUNTU_BETA1.md`
- Windows reviewer path: `docs/guides/INSTALL_WINDOWS_BETA1.md`

### 1. Prerequisites

- Python `3.11+`
- Access to a ScratchBird server and Python driver for live bridge mode

### Runtime Profile

Canonical connection metadata is policy supplied at runtime:

- `SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH`
- `SCRATCHBIRD_AI_CONNECTION_PROFILE_PATH`

Endpoint state is read from the configured profile:

- Native ScratchBird lanes are in scope.
- Donor emulation lanes may appear in deployment profiles, but AI support for
  non-native emulated engine modes remains out of scope for this component.
- Secrets are supplied by the configured secret provider or test fixture, never
  committed in profile files.

Direct native client probe shape:

```bash
project/output/<platform>/bin/SBsql <database> -H <host> -p <port> -U <user> --password-ref <secret-ref> --sslmode=<mode>
```

### 2. Install

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install -U pip
pip install -e ".[mcp]"
```

### 3. Validate Locally

```bash
PYTHONPATH=src python3 -m unittest discover -s tests -p 'test_*.py'
PYTHONPATH=src python3 tools/validate_capability_matrix.py
PYTHONPATH=src python3 tools/smoke_http_contract.py --mode selftest
python3 tools/generate_ai_conformance_artifacts.py --repo-root . --artifact-root ./build/ai/artifacts
python3 tools/validate_evidence_gates.py --repo-root . --artifact-root ./build/ai/artifacts --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md
python3 tools/validate_release_candidate.py --repo-root . --artifact-root ./build/ai/artifacts
```

### 4. Run Bridge

```bash
PYTHONPATH=src tools/run_local_bridge.sh
```

### 5. Run Bridge + MCP Stack

```bash
PYTHONPATH=src tools/run_local_stack.sh
```

## Runtime Configuration

### Adapter Environment Variables

- `SCRATCHBIRD_AI_ADAPTER_MODE`: `mock`, `http`, or `hybrid` (default `mock`)
- `SCRATCHBIRD_AI_HTTP_BASE_URL`: HTTP base URL for adapter calls
- `SCRATCHBIRD_AI_HTTP_TIMEOUT_SEC`: timeout for HTTP adapter requests
- `SCRATCHBIRD_AI_HTTP_RETRY_ATTEMPTS`: retry count for retryable bridge requests (`GET` and `compile`)
- `SCRATCHBIRD_AI_HTTP_RETRY_BACKOFF_MS`: retry backoff for retryable bridge requests
- `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_FAILURE_THRESHOLD`: consecutive bridge failures before the circuit opens
- `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_COOLDOWN_SEC`: cooldown before the bridge circuit closes again
- `SCRATCHBIRD_AI_HTTP_API_TOKEN`: optional Bearer token
- `SCRATCHBIRD_AI_HTTP_DIALECTS`: dialect CSV used for `hybrid` mode (default `native`)
- `SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH`: durable approval ledger path
- `SCRATCHBIRD_AI_COMPILE_REPAIR_MAX_ATTEMPTS`: bounded compile-repair attempt count
- `SCRATCHBIRD_AI_OPERATION_WINDOW_SEC`: shared quota/rate-limit window size
- `SCRATCHBIRD_AI_MAX_REQUESTS_PER_WINDOW`: request budget per window
- `SCRATCHBIRD_AI_MAX_MUTATIONS_PER_WINDOW`: mutation budget per window
- `SCRATCHBIRD_AI_MAX_COST_UNITS_PER_WINDOW`: cost budget per window
- `SCRATCHBIRD_AI_SUPPORTED_SERVER_VERSIONS`: supported ScratchBird server version set
- `SCRATCHBIRD_AI_SUPPORTED_PARSER_COMPILER_VERSIONS`: supported parser/compiler version set
- `SCRATCHBIRD_AI_SUPPORTED_DRIVER_RUNTIME_VERSIONS`: supported local/bridge runtime version set

### Bridge Environment Variables

- `SCRATCHBIRD_AI_BRIDGE_HOST`: bridge bind host (default `127.0.0.1`)
- `SCRATCHBIRD_AI_BRIDGE_PORT`: bridge bind port (default `3095`)
- `SCRATCHBIRD_AI_BRIDGE_API_TOKEN`: optional Bearer token required by bridge
- `SCRATCHBIRD_AI_BRIDGE_DIALECTS`: enabled dialect CSV (default `native`)
- `SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN`: fallback DSN for enabled dialects
- `SCRATCHBIRD_AI_BRIDGE_DSN_<DIALECT>`: per-dialect DSN override
- `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP`: `listener-only`, `managed`, `ipc-only`, or `embedded` (default `listener-only`)
- `SCRATCHBIRD_AI_BRIDGE_TRANSPORT_MODE`: explicit transport override (`inet_listener`, `managed`, `local_ipc`, `embedded`)
- `SCRATCHBIRD_AI_BRIDGE_FRONT_DOOR_MODE`: explicit front-door override (`direct`, `manager_proxy`)
- `SCRATCHBIRD_AI_BRIDGE_IPC_METHOD`: IPC method override (`auto`, `unix`, `pipe`, `tcp`)
- `SCRATCHBIRD_AI_BRIDGE_IPC_PATH`: IPC socket/pipe path override for `ipc-only`
- `SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN` / `SCRATCHBIRD_AI_BRIDGE_MCP_AUTH_TOKEN`: managed signon token
- `SCRATCHBIRD_AI_BRIDGE_MANAGER_USERNAME` / `SCRATCHBIRD_AI_BRIDGE_MCP_USERNAME`: managed username override
- `SCRATCHBIRD_AI_BRIDGE_MANAGER_DATABASE` / `SCRATCHBIRD_AI_BRIDGE_MCP_DATABASE`: managed database override
- `SCRATCHBIRD_AI_BRIDGE_MANAGER_CONNECTION_PROFILE` / `SCRATCHBIRD_AI_BRIDGE_MCP_CONNECTION_PROFILE`: managed connection profile (default `native_v3`)
- `SCRATCHBIRD_AI_BRIDGE_MANAGER_CLIENT_INTENT` / `SCRATCHBIRD_AI_BRIDGE_MCP_CLIENT_INTENT`: managed client intent (default `native_v3`)
- `SCRATCHBIRD_AI_BRIDGE_MANAGER_CLIENT_FLAGS` / `SCRATCHBIRD_AI_BRIDGE_MCP_CLIENT_FLAGS`: managed client flags (`0..65535`)
- `SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_FAST_PATH` / `SCRATCHBIRD_AI_BRIDGE_MCP_AUTH_FAST_PATH`: managed fast-path auth toggle (default `true`)
- `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC`: path to ScratchBird Python driver `src/`
- `SCRATCHBIRD_AI_BRIDGE_STRICT_COMPILE`: fail compile endpoint if compile probe fails

Connection-mode note:

- ScratchBird AI forwards mode-aware transport and signon options to the driver.
- `ipc-only` and `embedded` require a Python driver/runtime that supports those transport modes.

Reference example:

- `examples/http-bridge.env.example`

## Repository Layout

- `docs/` - public release, status, and guide documentation
- `capability/` - public AI capability matrix
- `src/` - package source (`scratchbird_ai`)
- `tests/` - unit and integration tests
- `examples/` - runtime configuration examples
- `tools/` - local scripts for validation, evidence generation, and stack startup

## Documentation Map

- Start here: `docs/README.md`
- Current status: `docs/status/EARLY_BETA_STATUS_2026-03-07.md` (content refreshed on 2026-04-20)
- Known gaps: `docs/status/EARLY_BETA_KNOWN_GAPS_2026-03-07.md` (content refreshed on 2026-04-20)
- Release gate contract: `docs/releases/EARLY_BETA_CONFORMANCE_GATES.md`
- Getting started guide: `docs/guides/GETTING_STARTED_EARLY_BETA.md`
- Ubuntu install guide: `docs/guides/INSTALL_UBUNTU_BETA1.md`
- Windows install guide: `docs/guides/INSTALL_WINDOWS_BETA1.md`
- Runtime controls and governance guide: `docs/guides/RUNTIME_CONFIGURATION_AND_GOVERNED_OPERATION.md`
- Live bridge troubleshooting guide: `docs/guides/LIVE_BRIDGE_TROUBLESHOOTING.md`
- Beta 1 support matrix: `docs/releases/BETA1_SUPPORT_MATRIX_2026-04-18.md`
- Beta 1 live recertification runbook: `docs/releases/BETA1_REMAINING_LIVE_TASKS_2026-04-18.md`

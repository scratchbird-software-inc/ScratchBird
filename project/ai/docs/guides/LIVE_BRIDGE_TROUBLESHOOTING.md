# Live Bridge Troubleshooting

Status: Active
Last Updated: 2026-04-20

## 1. Purpose

Provide a practical troubleshooting guide for the current ScratchBird AI
direct-listener validation path and the local HTTP bridge runtime.

This guide is for the currently supportable early-beta surface:

- ScratchBird native workflows
- local HTTP bridge
- direct listener-backed validation

ScratchBird core now admits bounded `manager_proxy`, `local_ipc`, and
`embedded_local_only` runtime truth. This guide still treats direct-listener
validation as the primary support path in this checkout because it was the path
refreshed successfully on 2026-04-20; the other runtime modes remain
environment-dependent here.

## 2. First Checks

From the repo root, verify the local baseline first:

```bash
PYTHONPATH=src python3 -m unittest discover -s tests -p 'test_*.py'
PYTHONPATH=src python3 tools/validate_capability_matrix.py
PYTHONPATH=src python3 tools/smoke_http_contract.py --mode selftest
```

If these fail, fix the local repository/runtime before diagnosing the live
target.

### Runtime Profile

The active runtime profile is supplied by policy and normally points at:

- `SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH`
- `SCRATCHBIRD_AI_CONNECTION_PROFILE_PATH`

Expected listener state is deployment-specific:

- The configured native ScratchBird listener is published and reachable.
- Donor emulation listeners may appear in a profile, but AI support for
  non-native emulated engine modes remains out of scope.

Quick host-side check:

```bash
ss -ltn
```

Confirm the configured native listener address is present in `LISTEN`.

## 3. Bridge Boot Failures

### `ImportError: scratchbird`

Cause:

- the bridge cannot import the ScratchBird Python driver

Fix:

- set `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC` to the driver `src/` directory,
  or install the driver in the active environment

### `503 Connection failed`

Cause:

- bad DSN
- unreachable ScratchBird listener
- transport mode mismatch

Checks:

- verify `SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN`
- verify the ScratchBird listener is reachable from this host
- verify the configured runtime profile is actually publishing the native
  listener address before assuming the bridge or driver is broken
- prefer `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP=listener-only` for the current
  release-truth path
- do not expect donor emulation ports to answer unless those listeners were
  explicitly started and AI support for that lane is in scope for the release

### `08001 connect() failed` with engine endpoint diagnostics

Example:

- `08001 connect() failed: No such file or directory | engine endpoint diagnostics: engine_endpoint=...; base_socket=missing; parser_socket=stale_or_not_listening`
- `08001 connect() failed: Connection refused | engine endpoint diagnostics: engine_endpoint=...; base_socket=missing; parser_socket=stale_or_not_listening`

Cause:

- the ScratchBird listener/parser path is up far enough to accept the client,
  but the engine IPC endpoint published by the refreshed runtime is missing,
  stale, or otherwise not listening

Checks:

- inspect `artifacts/live_native_conformance/run_log.json`
- read `native_preflight.runtime_diagnostics`
- verify the engine endpoint published in `profiles/runtime_ownership.json`
  is a live Unix socket, not just a leftover path entry
- if `127.0.0.1:13092` is listening but the engine endpoint diagnostics still
  report a missing or stale socket, treat that as a ScratchBird runtime issue
- if the base socket is missing while only a `.parser_v1` sibling exists or is
  stale, record it as a ScratchBird runtime issue rather than an AI bridge
  issue

### `404 Dialect not enabled`

Cause:

- `native` is not present in the bridge dialect list

Fix:

- set `SCRATCHBIRD_AI_BRIDGE_DIALECTS=native`

## 4. Auth and Token Failures

### `401 Unauthorized`

Cause:

- bridge token mismatch between client and bridge

Fix:

- make `SCRATCHBIRD_AI_HTTP_API_TOKEN` and
  `SCRATCHBIRD_AI_BRIDGE_API_TOKEN` match

### `400 Managed setup requires manager_auth_token`

Cause:

- `managed` setup selected without the required manager token

Fix:

- set `SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN`

Current release-truth note:

- `managed` mode maps to the bounded ScratchBird core `manager_proxy` lane
- using it here still requires matching manager credentials, driver support,
  and an environment where the mode is exposed so live evidence can be rerun

## 5. Compatibility Failures

Common errors:

- `E_SERVER_RUNTIME_UNSUPPORTED`
- `E_COMPONENT_VERSION_UNSUPPORTED`
- `E_DRIVER_RUNTIME_UNSUPPORTED`
- `E_INTERFACE_PROFILE_UNSUPPORTED`
- `E_TRANSPORT_PROFILE_UNSUPPORTED`

Cause:

- the declared supported version/transport policy does not match the live
  environment

Checks:

- `SCRATCHBIRD_AI_SUPPORTED_SERVER_VERSIONS`
- `SCRATCHBIRD_AI_SUPPORTED_PARSER_COMPILER_VERSIONS`
- `SCRATCHBIRD_AI_SUPPORTED_DRIVER_RUNTIME_VERSIONS`
- requested interface profile and requested transport

Current behavior is fail-closed by design. Do not “fix” these by suppressing
the check unless the environment is genuinely inside the supported window.

## 6. Approval and Mutation Failures

### `E_APPROVAL_INVALID`

Cause:

- missing approval token
- mismatched tenant/actor/statement hash
- expired approval
- revoked approval

Checks:

- inspect the approval ledger at `SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH`
- verify the mutation was replayed with the same statement hash
- verify `expires_at` and `revoked_at`

### `E_LIMIT_EXCEEDED`

Cause:

- request, mutation, or cost window exceeded

Checks:

- `SCRATCHBIRD_AI_OPERATION_WINDOW_SEC`
- `SCRATCHBIRD_AI_MAX_REQUESTS_PER_WINDOW`
- `SCRATCHBIRD_AI_MAX_MUTATIONS_PER_WINDOW`
- `SCRATCHBIRD_AI_MAX_COST_UNITS_PER_WINDOW`

Rule IDs:

- `OPS-RATE-001`
- `OPS-MUTATION-001`
- `OPS-COST-001`

## 7. Compile and Bridge Resilience Failures

If wrapped query text causes compile failure:

- reduce prompt formatting noise
- keep `SCRATCHBIRD_AI_COMPILE_REPAIR_MAX_ATTEMPTS` bounded
- remember that compile-repair only strips simple wrappers such as markdown
  fences and `sql:` / `query:` prefixes

If the bridge starts flapping:

- inspect `SCRATCHBIRD_AI_HTTP_RETRY_ATTEMPTS`
- inspect `SCRATCHBIRD_AI_HTTP_RETRY_BACKOFF_MS`
- inspect `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_FAILURE_THRESHOLD`
- inspect `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_COOLDOWN_SEC`

Retry currently applies only to:

- `GET`
- compile endpoints

## 8. Live Recertification

For the current release packet, the live evidence path is:

```bash
export SCRATCHBIRD_AI_LIVE_NATIVE_ENABLED=1
export SCRATCHBIRD_AI_LIVE_NATIVE_LAUNCH_BRIDGE=1
export SCRATCHBIRD_AI_ADAPTER_MODE=http
export SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH="${SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH:-./profiles/runtime.env}"
PYTHONPATH=src python3 tools/run_live_native_conformance.py \
  --scratchbird-server-version "${SCRATCHBIRD_AI_LIVE_NATIVE_SCRATCHBIRD_SERVER_VERSION:-current-shared-node-2026-04-20}" \
  --parser-compiler-version "${SCRATCHBIRD_AI_LIVE_NATIVE_PARSER_COMPILER_VERSION:-current-v3-prebuild}" \
  --test-dataset-version "${SCRATCHBIRD_AI_LIVE_NATIVE_TEST_DATASET_VERSION:-shared-main}" \
  --seed-or-fixture-version "${SCRATCHBIRD_AI_LIVE_NATIVE_SEED_VERSION:-shared-node}" \
  --covered-profile mcp_local_v0 \
  --covered-profile mcp_remote_v0 \
  --covered-profile streaming_async_v0 \
  --covered-profile retrieval_ingest_v0
```

Then regenerate and validate the release artifacts:

```bash
PYTHONPATH=src python3 tools/generate_ai_conformance_artifacts.py --repo-root .
PYTHONPATH=src python3 tools/validate_evidence_gates.py --repo-root . --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md
```

## 9. Escalation Boundary

If the problem is specifically:

- regenerated live evidence for the refreshed ScratchBird target
- driver/runtime support for `local_ipc` or embedded execution
- environment-specific manager-proxy reachability or authentication
- a published engine endpoint that is missing, stale, or not actually
  listening

that is not a missing core release contract anymore. Treat it as a live
environment, driver, or runtime-packaging issue and record it against the
updated test environment.

If the live smoke reports `no table available for describe endpoint`:

- leave `SCRATCHBIRD_AI_SMOKE_SCHEMA` and `SCRATCHBIRD_AI_SMOKE_TABLE` unset
- the current harness now walks discovered schemas until it finds a describable
  native table
- only pin schema or table explicitly when validating a known object

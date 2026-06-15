# Troubleshooting

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter provides practical troubleshooting guidance for the ScratchBird AI
local HTTP bridge and the direct-listener validation path. It covers the
currently supportable early-beta surface: ScratchBird native workflows, the
local HTTP bridge, and direct-listener validation.

The primary certified path is `listener_direct`. `manager_proxy`, `local_ipc`,
and `embedded_local_only` are admitted in ScratchBird core and notes for those
modes appear where relevant, but the detailed steps below assume
`listener-only` as the server setup profile.

---

## First Checks

Before diagnosing live bridge or server issues, verify the local baseline:

```bash
PYTHONPATH=src python3 -m unittest discover -s tests -p 'test_*.py'
PYTHONPATH=src python3 tools/validate_capability_matrix.py
PYTHONPATH=src python3 tools/smoke_http_contract.py --mode selftest
```

If any of these fail, fix the local repository or runtime before investigating
the live target.

### Confirm the Runtime Profile

The active runtime profile is supplied by policy and normally points at:

- `SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH`
- `SCRATCHBIRD_AI_CONNECTION_PROFILE_PATH`

Check that the configured native ScratchBird listener is actually published and
reachable:

```bash
ss -ltn
```

Confirm the configured native listener address appears in `LISTEN` before
concluding that the bridge or driver is broken.

---

## Bridge Boot Failures

### `ImportError: scratchbird`

**Cause:** The bridge cannot import the ScratchBird Python driver.

**Fix:**

```bash
export SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC=/path/to/driver/src
```

Or install the driver in the active Python environment.

---

### `503 Connection failed`

**Cause:** Bad DSN, unreachable ScratchBird listener, or transport mode
mismatch.

**Checks:**

1. Verify `SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN` is correct.
2. Verify the ScratchBird listener is reachable from this host.
3. Verify the configured runtime profile is actually publishing the native
   listener address.
4. Use `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP=listener-only` for the current
   release-truth path.
5. Do not expect reference emulation ports to answer unless those listeners were
   explicitly started and AI support for that lane is in scope.

---

### `08001 connect() failed` with engine endpoint diagnostics

**Example messages:**

```
08001 connect() failed: No such file or directory | engine endpoint diagnostics:
  engine_endpoint=...; base_socket=missing; parser_socket=stale_or_not_listening
```

```
08001 connect() failed: Connection refused | engine endpoint diagnostics:
  engine_endpoint=...; base_socket=missing; parser_socket=stale_or_not_listening
```

**Cause:** The ScratchBird listener/parser path is up far enough to accept the
client, but the engine IPC endpoint is missing, stale, or not actually
listening.

**Checks:**

1. Inspect `artifacts/live_native_conformance/run_log.json`.
2. Read `native_preflight.runtime_diagnostics`.
3. Verify the engine endpoint published in `profiles/runtime_ownership.json`
   is a live Unix socket, not just a leftover path entry.
4. If `127.0.0.1:13092` is listening but the engine endpoint diagnostics still
   report a missing or stale socket, treat it as a ScratchBird runtime issue,
   not an AI bridge issue.
5. If the base socket is missing while only a `.parser_v1` sibling exists or
   is stale, record it as a ScratchBird runtime issue.

---

### `404 Dialect not enabled`

**Cause:** `native` is not present in the bridge dialect list.

**Fix:**

```bash
export SCRATCHBIRD_AI_BRIDGE_DIALECTS=native
```

---

## Authentication and Token Failures

### `401 Unauthorized`

**Cause:** Bridge token mismatch between the adapter and the bridge.

**Fix:** Make `SCRATCHBIRD_AI_HTTP_API_TOKEN` and
`SCRATCHBIRD_AI_BRIDGE_API_TOKEN` match.

---

### `400 Managed setup requires manager_auth_token`

**Cause:** `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP=managed` was selected without
the required manager token.

**Fix:**

```bash
export SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN=<token>
```

Note: `managed` mode maps to the bounded ScratchBird core `manager_proxy` lane.
Using it requires matching manager credentials, driver support, and a test
environment that exposes this mode so live evidence can be rerun.

---

## Compatibility Failures

These errors indicate a version or transport mismatch between the declared
compatibility policy and the live environment:

| Error code | Condition |
| --- | --- |
| `E_SERVER_RUNTIME_UNSUPPORTED` | Declared server version not in supported list |
| `E_COMPONENT_VERSION_UNSUPPORTED` | Declared component version not in supported list |
| `E_DRIVER_RUNTIME_UNSUPPORTED` | Declared driver runtime version not in supported list |
| `E_INTERFACE_PROFILE_UNSUPPORTED` | Requested interface profile not supported |
| `E_TRANSPORT_PROFILE_UNSUPPORTED` | Requested transport not supported |

**Checks:**

- `SCRATCHBIRD_AI_SUPPORTED_SERVER_VERSIONS`
- `SCRATCHBIRD_AI_SUPPORTED_PARSER_COMPILER_VERSIONS`
- `SCRATCHBIRD_AI_SUPPORTED_DRIVER_RUNTIME_VERSIONS`

The fail-closed behavior is by design. Do not suppress these checks unless the
environment is genuinely inside the supported window.

---

## Approval and Mutation Failures

### `E_APPROVAL_INVALID`

**Cause:** Missing approval token, mismatched tenant/actor/statement hash,
expired approval, or revoked approval.

**Checks:**

1. Inspect the approval ledger at `SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH`.
2. Verify the mutation was replayed with the same statement hash.
3. Verify `expires_at` is in the future.
4. Verify `revoked_at` is null.

---

### `E_LIMIT_EXCEEDED`

**Cause:** Request, mutation, or cost window exceeded.

**Checks:**

| Rule ID | Variable to inspect |
| --- | --- |
| `OPS-RATE-001` | `SCRATCHBIRD_AI_MAX_REQUESTS_PER_WINDOW`, `SCRATCHBIRD_AI_OPERATION_WINDOW_SEC` |
| `OPS-MUTATION-001` | `SCRATCHBIRD_AI_MAX_MUTATIONS_PER_WINDOW`, `SCRATCHBIRD_AI_OPERATION_WINDOW_SEC` |
| `OPS-COST-001` | `SCRATCHBIRD_AI_MAX_COST_UNITS_PER_WINDOW`, `SCRATCHBIRD_AI_OPERATION_WINDOW_SEC` |

Increase the window budget only if the higher budget is justified for the
current environment.

---

## Compile and Bridge Resilience Failures

### Compile failures

If wrapped query text causes a compile failure:

1. Reduce prompt formatting noise (avoid markdown fences and `sql:`/`query:`
   prefixes in the query text passed to the tool).
2. Check `SCRATCHBIRD_AI_COMPILE_REPAIR_MAX_ATTEMPTS`. The repair strategies
   are deterministic and bounded: only whitespace trimming, markdown fence
   stripping, and leading label stripping are attempted.
3. Remember that compile-repair does not rewrite query semantics.

### Bridge flapping

If the bridge becomes unstable:

1. Inspect `SCRATCHBIRD_AI_HTTP_RETRY_ATTEMPTS`
2. Inspect `SCRATCHBIRD_AI_HTTP_RETRY_BACKOFF_MS`
3. Inspect `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_FAILURE_THRESHOLD`
4. Inspect `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_COOLDOWN_SEC`

Retry applies only to `GET` and compile endpoints; execute and mutation
endpoints are not retried automatically.

---

## Live Recertification

When you need to regenerate live evidence:

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

Then regenerate and validate release artifacts:

```bash
PYTHONPATH=src python3 tools/generate_ai_conformance_artifacts.py --repo-root .
PYTHONPATH=src python3 tools/validate_evidence_gates.py \
  --repo-root . --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md
```

If the live harness reports `no table available for describe endpoint`:

- Leave `SCRATCHBIRD_AI_SMOKE_SCHEMA` and `SCRATCHBIRD_AI_SMOKE_TABLE` unset.
  The harness walks discovered schemas until it finds a describable native table.
- Only pin `schema` or `table` explicitly when validating a known object.

---

## Escalation Boundary

The following conditions are not missing core release contracts. Treat them as
live environment, driver, or runtime-packaging issues:

- Live evidence regeneration needed for a refreshed ScratchBird target
- Driver/runtime support for `local_ipc` or embedded execution
- Environment-specific `manager_proxy` reachability or authentication
- A published engine endpoint that is missing, stale, or not actually listening

Record these against the updated test environment, not as AI-layer defects.

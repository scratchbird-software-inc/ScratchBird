# Getting Started (Early Beta)

Status: Active
Last Updated: 2026-04-20

## 1. Goal

Bring up ScratchBird AI locally for current early-beta validation:

- run tests
- validate the capability matrix
- generate and validate release evidence
- run the local HTTP bridge
- run the MCP stack
- run HTTP contract smoke tests
- run live direct-listener certification against a real ScratchBird target

Support scope note:

- This repository currently supports **ScratchBird native** workflows only.
- ScratchBird core now exposes one bounded current-tree AI surface for
  engine-owned retrieval, retrieval metadata discovery, and runtime-mode truth.
- ScratchBird AI publishes that core packet. Direct-listener live evidence must
  be regenerated from the current checkout and target runtime before claiming
  live-native profile coverage.
- `manager-proxy`, `local-IPC`, and `embedded` are admitted current runtime
  modes in ScratchBird core; actual use here still depends on the local Python
  driver/runtime and a test environment that exposes those modes.

## 2. Prerequisites

- Ubuntu/Linux reviewer path or Windows companion-host path
- Python `3.11+`
- ScratchBird server reachable when using live bridge mode
- ScratchBird Python driver source or package when using live bridge mode

Platform-specific install guidance:

- Ubuntu/Linux: [INSTALL_UBUNTU_BETA1.md](./INSTALL_UBUNTU_BETA1.md)
- Windows companion host: [INSTALL_WINDOWS_BETA1.md](./INSTALL_WINDOWS_BETA1.md)

Use this guide as the general early-beta bring-up flow after the platform-local
Python environment is installed. The shell examples below are POSIX-oriented;
Windows-specific command forms are documented in the Windows install guide.

### Runtime Profile

The active test profile is supplied by policy and normally points at:

- `SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH`
- `SCRATCHBIRD_AI_CONNECTION_PROFILE_PATH`

Endpoint state is deployment-specific:

- Native ScratchBird lanes are in scope for this component.
- Donor emulation lanes may exist in a deployment profile, but AI support for
  non-native emulated engine modes remains out of scope.
- Users, databases, ports, and secrets are read from the configured profile and
  secret provider.

Direct native lane probe:

```bash
project/output/<platform>/bin/SBsql <database> -H <host> -p <port> -U <user> --password-ref <secret-ref> --sslmode=<mode>
```

## 3. Install

```bash
cd project/ai
python3 -m venv .venv
. .venv/bin/activate
pip install -U pip
pip install -e ".[mcp]"
```

## 4. Validate Baseline

```bash
PYTHONPATH=src python3 -m unittest discover -s tests -p 'test_*.py'
PYTHONPATH=src python3 tools/validate_capability_matrix.py
PYTHONPATH=src python3 tools/smoke_http_contract.py --mode selftest
PYTHONPATH=src python3 tools/generate_ai_conformance_artifacts.py --repo-root . --artifact-root ./build/ai/artifacts
PYTHONPATH=src python3 tools/validate_evidence_gates.py --repo-root . --artifact-root ./build/ai/artifacts --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md
```

Expected outcome:

- discovered test suite passes
- capability matrix validator exits `0`
- smoke script prints `[smoke] PASS`
- conformance artifacts are regenerated for the current commit
- evidence validator prints `OK: evidence gates valid ...`

## 5. Configure Bridge (Live Mode)

Copy and edit:

```bash
cp examples/http-bridge.env.example .env.bridge
```

Minimum required variables:

- `SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN`
- `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC` if the driver is not pip-installed
- `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP` when not using `listener-only` (valid:
  `managed`, `ipc-only`, `embedded`)

Managed setup additional requirement:

- `SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN` (or `mcp` alias)

Optional security:

- `SCRATCHBIRD_AI_BRIDGE_API_TOKEN`

Recommended direct-listener baseline for current release-truth testing:

- `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP=listener-only`
- `SCRATCHBIRD_AI_ADAPTER_MODE=http`
- `SCRATCHBIRD_AI_HTTP_DIALECTS=native`

Recommended local guardrails:

- `SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH=artifacts/runtime/approval_ledger.json`
- `SCRATCHBIRD_AI_COMPILE_REPAIR_MAX_ATTEMPTS=3`
- `SCRATCHBIRD_AI_OPERATION_WINDOW_SEC=60`
- `SCRATCHBIRD_AI_MAX_REQUESTS_PER_WINDOW=100`
- `SCRATCHBIRD_AI_MAX_MUTATIONS_PER_WINDOW=20`
- `SCRATCHBIRD_AI_MAX_COST_UNITS_PER_WINDOW=1000`
- `SCRATCHBIRD_AI_HTTP_RETRY_ATTEMPTS=1`
- `SCRATCHBIRD_AI_HTTP_RETRY_BACKOFF_MS=100`
- `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_FAILURE_THRESHOLD=3`
- `SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_COOLDOWN_SEC=30`

Optional compatibility pins:

- `SCRATCHBIRD_AI_SUPPORTED_SERVER_VERSIONS`
- `SCRATCHBIRD_AI_SUPPORTED_PARSER_COMPILER_VERSIONS`
- `SCRATCHBIRD_AI_SUPPORTED_DRIVER_RUNTIME_VERSIONS`

Current core packet note:

- the bounded ScratchBird core runtime modes are `listener_direct`,
  `manager_proxy`, `local_ipc`, and `embedded_local_only`
- the bounded retrieval metadata packet is exposed through the
  `opensearch_meta.*` catalog namespace
- this repository has refreshed direct-listener live-native evidence for the
  current shipped profile set
- additional runtime-mode certification is still environment-dependent when the
  active harness does not expose `manager_proxy`, `local_ipc`, or
  `embedded_local_only`

## 6. Run Bridge

```bash
set -a
source .env.bridge
set +a
PYTHONPATH=src tools/run_local_bridge.sh
```

## 7. Run Full Local Stack

In another shell:

```bash
set -a
source .env.bridge
set +a
export SCRATCHBIRD_AI_ADAPTER_MODE=http
export SCRATCHBIRD_AI_HTTP_BASE_URL="http://${SCRATCHBIRD_AI_BRIDGE_HOST:-127.0.0.1}:${SCRATCHBIRD_AI_BRIDGE_PORT:-3095}"
export SCRATCHBIRD_AI_HTTP_API_TOKEN="${SCRATCHBIRD_AI_BRIDGE_API_TOKEN:-}"
PYTHONPATH=src tools/run_local_stack.sh
```

## 8. Run Live Contract Smoke Test

The current shared static-example environment publishes only the native
ScratchBird listener by default. If you have not started donor emulation
listeners separately, expect only `127.0.0.1:13092` to answer.

```bash
PYTHONPATH=src python3 tools/run_live_native_conformance.py \
  --enable-live \
  --launch-bridge \
  --adapter-mode http \
  --runtime-env-path "${SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH:-$HOME/.scratchbird/static-example/profiles/runtime.env}" \
  --scratchbird-server-version "${SCRATCHBIRD_AI_LIVE_NATIVE_SCRATCHBIRD_SERVER_VERSION:-current-shared-node-2026-04-20}" \
  --parser-compiler-version "${SCRATCHBIRD_AI_LIVE_NATIVE_PARSER_COMPILER_VERSION:-current-v3-prebuild}" \
  --test-dataset-version "${SCRATCHBIRD_AI_LIVE_NATIVE_TEST_DATASET_VERSION:-shared-main}" \
  --seed-or-fixture-version "${SCRATCHBIRD_AI_LIVE_NATIVE_SEED_VERSION:-shared-node}"
```

## 9. Run Live Native Certification Harness

```bash
export SCRATCHBIRD_AI_LIVE_NATIVE_ENABLED=1
export SCRATCHBIRD_AI_ADAPTER_MODE=http
export SCRATCHBIRD_AI_LIVE_NATIVE_LAUNCH_BRIDGE=1
export SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH="${SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH:-$HOME/.scratchbird/static-example/profiles/runtime.env}"
export SCRATCHBIRD_AI_LIVE_NATIVE_SCRATCHBIRD_SERVER_VERSION="current-shared-node-2026-04-20"
export SCRATCHBIRD_AI_LIVE_NATIVE_PARSER_COMPILER_VERSION="current-v3-prebuild"
export SCRATCHBIRD_AI_LIVE_NATIVE_TEST_DATASET_VERSION="shared-main"
export SCRATCHBIRD_AI_LIVE_NATIVE_SEED_VERSION="shared-node"
PYTHONPATH=src python3 tools/run_live_native_conformance.py \
  --covered-profile mcp_local_v0 \
  --covered-profile mcp_remote_v0 \
  --covered-profile streaming_async_v0 \
  --covered-profile retrieval_ingest_v0
```

The direct-listener form of this harness was refreshed successfully on
2026-04-20 for the listed profiles. Rerun it whenever the shared environment is
updated or when certifying an additional runtime mode. On the current shared
profile, the expected published listener is the native lane at
`127.0.0.1:13092`.

The harness writes `summary.json`, `environment_manifest.json`, `run_log.json`,
and `test_report.junit.xml` under `artifacts/live_native_conformance/`.

## 10. Validate Release-Candidate Claims

```bash
PYTHONPATH=src python3 tools/validate_release_candidate.py \
  --claim-profile provider_tool_calling_v0 \
  --release-time-utc 2026-04-20T00:00:00Z
```

For live-native profile claims such as `service_internal_v0`, run `tools/run_live_native_conformance.py` first so `artifacts/live_native_conformance/` exists on the same commit.

For the current Beta 1 support boundary and the live recertification recipe,
see:

- [BETA1_SUPPORT_MATRIX_2026-04-18.md](../releases/BETA1_SUPPORT_MATRIX_2026-04-18.md)
- [BETA1_REMAINING_LIVE_TASKS_2026-04-18.md](../releases/BETA1_REMAINING_LIVE_TASKS_2026-04-18.md)

## 11. Governed Mutation Note

Mutation execution is no longer “inline token only”.

Current early-beta behavior:

- approval evidence is validated and persisted in the local approval ledger
- approval reuse is checked against tenant, actor, and statement hash
- expired or revoked approvals fail closed
- request, mutation, and cost windows are enforced before execution
- operator tooling can list and revoke approval records, replay audit bundles,
  issue or verify audit attestations, and generate runtime diagnostics/runbook
  bundles

For the full operator-facing control surface, see
`RUNTIME_CONFIGURATION_AND_GOVERNED_OPERATION.md`.

## 12. Common Failures

- `ImportError: scratchbird`: set `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC` to the driver `src` path.
- `401 Unauthorized`: set matching `SCRATCHBIRD_AI_HTTP_API_TOKEN` and `SCRATCHBIRD_AI_BRIDGE_API_TOKEN`.
- `404 Dialect not enabled`: include `native` in `SCRATCHBIRD_AI_BRIDGE_DIALECTS`.
- `400 Managed setup requires manager_auth_token`: set `SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN` or include `manager_auth_token` in DSN.
- `503 Connection failed` in `ipc-only` or `embedded`: verify the Python driver build supports those transport modes.
- `503 Connection failed`: verify DSN and ScratchBird server reachability.
- `E_LIMIT_EXCEEDED`: increase the request, mutation, or cost window settings
  only if the higher budget is justified for the current environment.
- `E_APPROVAL_INVALID`: inspect the durable approval ledger entry, statement
  hash, expiry, and revocation state.
- `E_SERVER_RUNTIME_UNSUPPORTED`, `E_COMPONENT_VERSION_UNSUPPORTED`, or
  `E_DRIVER_RUNTIME_UNSUPPORTED`: adjust the declared supported version lists or
  the live target so the runtime truth matches the pinned compatibility policy.

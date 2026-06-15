# Getting Started

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter guides you through the initial bring-up of ScratchBird AI for
Beta 1 evaluation: install the Python environment, run local validation,
configure the bridge, start the bridge and MCP server, and optionally run the
live certification harness.

This guide covers the general flow. For platform-specific detail, the source
install guides are at:

- Ubuntu/Linux: `project/ai/docs/guides/INSTALL_UBUNTU_BETA1.md`
- Windows: `project/ai/docs/guides/INSTALL_WINDOWS_BETA1.md`

---

## Support Scope

ScratchBird AI supports **ScratchBird native** workflows only. The current
primary certified path is `listener_direct` (direct TCP connection to the
native ScratchBird listener). The `manager_proxy`, `local_ipc`, and
`embedded_local_only` runtime modes are implemented and admitted in ScratchBird
core, but live certification evidence for those modes depends on the test
environment exposing them.

---

## Prerequisites

### All Platforms

- Python 3.11 or later
- A ScratchBird server reachable from the host when using live bridge mode
- The ScratchBird Python driver, either:
  - installed in the active Python environment, or
  - available as source with `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC` pointing
    to the driver `src/` directory

### Ubuntu / Linux Additional

```bash
sudo apt update
sudo apt install -y python3 python3-venv python3-pip git
```

### Windows Additional

- Windows PowerShell
- Optional: Windows Terminal

---

## Step 1: Install the Repository

**Ubuntu/Linux:**

```bash
cd ScratchBird/project/ai
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -U pip
python3 -m pip install -e ".[mcp]"
```

**Windows (PowerShell):**

```powershell
cd ScratchBird\project\ai
py -3.11 -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install -U pip
python -m pip install -e ".[mcp]"
```

If PowerShell blocks activation:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.venv\Scripts\Activate.ps1
```

---

## Step 2: Validate the Local Baseline

Run these validation steps from the `project/ai` directory. All should pass
before attempting live bridge mode.

**Ubuntu/Linux:**

```bash
PYTHONPATH=src python3 -m unittest discover -s tests -p 'test_*.py'
PYTHONPATH=src python3 tools/validate_capability_matrix.py
PYTHONPATH=src python3 tools/smoke_http_contract.py --mode selftest
PYTHONPATH=src python3 tools/generate_ai_conformance_artifacts.py \
  --repo-root . --artifact-root build/ai/artifacts
PYTHONPATH=src python3 tools/validate_evidence_gates.py \
  --repo-root . --artifact-root build/ai/artifacts \
  --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md
```

**Windows (PowerShell):**

```powershell
$env:PYTHONPATH = (Resolve-Path .\src).Path
python -m unittest discover -s tests -p 'test_*.py'
python tools\validate_capability_matrix.py
python tools\smoke_http_contract.py --mode selftest
python tools\generate_ai_conformance_artifacts.py `
  --repo-root . --artifact-root build\ai\artifacts
python tools\validate_evidence_gates.py `
  --repo-root . --artifact-root build\ai\artifacts `
  --spec docs\releases\EARLY_BETA_CONFORMANCE_GATES.md
```

Expected outcomes:

- Test suite passes
- Capability matrix validator exits `0`
- Selftest smoke prints `[smoke] PASS`
- Conformance artifacts regenerate successfully
- Evidence gate validator prints `OK: evidence gates valid ...`

---

## Step 3: Configure the Bridge for Live Mode

Copy the example configuration:

```bash
cp examples/http-bridge.env.example .env.bridge
```

Edit at minimum:

| Variable | Description |
| --- | --- |
| `SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN` | DSN pointing to your ScratchBird server (e.g. `scratchbird://user:password@127.0.0.1:3092/mydb`) |
| `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP` | Connection mode; use `listener-only` for first live bring-up |
| `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC` | Path to the driver `src/` if not pip-installed |

Recommended first live bring-up settings:

```bash
SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP=listener-only
SCRATCHBIRD_AI_ADAPTER_MODE=http
SCRATCHBIRD_AI_HTTP_DIALECTS=native
```

Optional local guardrails to add:

```bash
SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH=artifacts/runtime/approval_ledger.json
SCRATCHBIRD_AI_COMPILE_REPAIR_MAX_ATTEMPTS=3
SCRATCHBIRD_AI_OPERATION_WINDOW_SEC=60
SCRATCHBIRD_AI_MAX_REQUESTS_PER_WINDOW=100
SCRATCHBIRD_AI_MAX_MUTATIONS_PER_WINDOW=20
SCRATCHBIRD_AI_MAX_COST_UNITS_PER_WINDOW=1000
SCRATCHBIRD_AI_HTTP_RETRY_ATTEMPTS=1
SCRATCHBIRD_AI_HTTP_RETRY_BACKOFF_MS=100
SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_FAILURE_THRESHOLD=3
SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_COOLDOWN_SEC=30
```

For managed mode (`SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP=managed`), also set:

```bash
SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN=<token>
```

For `ipc-only` and `embedded` modes: confirm that the ScratchBird Python driver
and runtime support those transport modes before configuring them.

**Windows:** Set the same variables as PowerShell environment variables:

```powershell
$env:SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN = 'scratchbird://user:password@127.0.0.1:3092/mydb'
$env:SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP = 'listener-only'
$env:SCRATCHBIRD_AI_ADAPTER_MODE = 'http'
$env:SCRATCHBIRD_AI_HTTP_DIALECTS = 'native'
```

---

## Step 4: Start the HTTP Bridge

**Ubuntu/Linux (POSIX helper):**

```bash
set -a
source .env.bridge
set +a
PYTHONPATH=src tools/run_local_bridge.sh
```

**Ubuntu/Linux (direct module):**

```bash
set -a
source .env.bridge
set +a
export PYTHONPATH=src
python3 -m scratchbird_ai.http_bridge
```

**Windows (PowerShell):**

```powershell
$env:PYTHONPATH = (Resolve-Path .\src).Path
python -m scratchbird_ai.http_bridge
# or: scratchbird-ai-http-bridge
```

The bridge binds to `127.0.0.1:3095` by default. Confirm it is listening
before proceeding.

---

## Step 5: Start the MCP Server with the Bridge

In a second terminal (the bridge must remain running):

**Ubuntu/Linux (POSIX helper):**

```bash
set -a
source .env.bridge
set +a
export SCRATCHBIRD_AI_ADAPTER_MODE=http
export SCRATCHBIRD_AI_HTTP_BASE_URL="http://${SCRATCHBIRD_AI_BRIDGE_HOST:-127.0.0.1}:${SCRATCHBIRD_AI_BRIDGE_PORT:-3095}"
export SCRATCHBIRD_AI_HTTP_API_TOKEN="${SCRATCHBIRD_AI_BRIDGE_API_TOKEN:-}"
PYTHONPATH=src tools/run_local_stack.sh
```

**Ubuntu/Linux (direct module):**

```bash
export PYTHONPATH=src
export SCRATCHBIRD_AI_ADAPTER_MODE=http
export SCRATCHBIRD_AI_HTTP_BASE_URL="http://127.0.0.1:3095"
python3 -m scratchbird_ai.mcp_server
```

**Windows (PowerShell):**

```powershell
$env:PYTHONPATH = (Resolve-Path .\src).Path
$env:SCRATCHBIRD_AI_ADAPTER_MODE = 'http'
$env:SCRATCHBIRD_AI_HTTP_BASE_URL = 'http://127.0.0.1:3095'
python -m scratchbird_ai.mcp_server
# or: scratchbird-ai-mcp
```

---

## Step 6: Run the Live Contract Smoke Test (Optional)

When the bridge is running against a real ScratchBird server:

```bash
PYTHONPATH=src python3 tools/smoke_http_contract.py --mode live --dialect native
```

The current shared static-example environment publishes the native ScratchBird
listener by default. If you have not started reference emulation listeners
separately, expect only the native lane to answer.

---

## Step 7: Run the Live Certification Harness (Optional)

The direct-listener certification sequence was exercised successfully on
2026-04-20 against the shared ScratchBird node. Re-run it whenever the
environment is refreshed or when capturing new release evidence:

```bash
set -a
source .env.bridge
set +a
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

After the harness run, regenerate and validate release artifacts:

```bash
PYTHONPATH=src python3 tools/generate_ai_conformance_artifacts.py --repo-root .
PYTHONPATH=src python3 tools/validate_evidence_gates.py \
  --repo-root . --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md
```

The harness writes `summary.json`, `environment_manifest.json`, `run_log.json`,
and `test_report.junit.xml` under `artifacts/live_native_conformance/`.

---

## Step 8: Validate Release-Candidate Claims (Optional)

```bash
PYTHONPATH=src python3 tools/validate_release_candidate.py \
  --claim-profile provider_tool_calling_v0 \
  --release-time-utc 2026-04-20T00:00:00Z
```

For live-native profile claims such as `service_internal_v0`, run the
live certification harness first so `artifacts/live_native_conformance/`
exists on the same commit.

---

## Governed Mutation Note

Mutation execution requires prior approval. Current behavior:

- Approval evidence is validated and persisted in the local approval ledger
- Approval reuse is checked against tenant, actor, and statement hash
- Expired or revoked approvals fail closed
- Request, mutation, and cost windows are enforced before execution

For operator-facing controls, see [governance_quotas_and_audit.md](./governance_quotas_and_audit.md).

---

## Common Startup Failures

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| `ImportError: scratchbird` | Driver not installed or not on path | Set `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC` to the driver `src/` directory |
| `401 Unauthorized` | Token mismatch between adapter and bridge | Match `SCRATCHBIRD_AI_HTTP_API_TOKEN` and `SCRATCHBIRD_AI_BRIDGE_API_TOKEN` |
| `404 Dialect not enabled` | `native` not in bridge dialect list | Set `SCRATCHBIRD_AI_BRIDGE_DIALECTS=native` |
| `400 Managed setup requires manager_auth_token` | Managed mode without token | Set `SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN` |
| `503 Connection failed` | Bad DSN or server unreachable | Verify `SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN` and server reachability |

For detailed troubleshooting, see [troubleshooting.md](./troubleshooting.md).

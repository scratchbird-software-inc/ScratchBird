# Ubuntu Install Guide (Beta 1)

Status: Active
Last Updated: 2026-04-20

## 1. Purpose

Install and run ScratchBird AI on Ubuntu for Beta 1 evaluation.

This is the primary Linux reviewer path and the best-supported install flow in
the current repository.

## 2. Current Support Boundary

Supported now:

- local repository install
- local test and evidence validation
- HTTP bridge startup
- MCP server startup
- direct-listener live bring-up against the refreshed shared ScratchBird test
  environment

Supported in the ScratchBird core packet but still pending environment-specific
live recertification when the environment exposes them:

- `manager_proxy`
- `local_ipc`
- `embedded_local_only`

## 3. Prerequisites

Install the base packages:

```bash
sudo apt update
sudo apt install -y python3 python3-venv python3-pip git
```

You also need:

- a local checkout or release copy of ScratchBird
- a reachable ScratchBird server when using live bridge mode
- the ScratchBird Python driver, either:
  - installed in the same environment, or
  - available as source with `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC`

## 4. Install The Repository

```bash
git clone <your-scratchbird-repo-url>
cd ScratchBird/project/ai
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -U pip
python3 -m pip install -e ".[mcp]"
```

## 5. Validate The Local Baseline

```bash
PYTHONPATH=src python3 -m unittest discover -s tests -p 'test_*.py'
PYTHONPATH=src python3 tools/validate_capability_matrix.py
PYTHONPATH=src python3 tools/smoke_http_contract.py --mode selftest
PYTHONPATH=src python3 tools/generate_ai_conformance_artifacts.py --repo-root . --artifact-root build/ai/artifacts
PYTHONPATH=src python3 tools/validate_evidence_gates.py --repo-root . --artifact-root build/ai/artifacts --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md
```

Expected result:

- test suite passes
- capability-matrix validator exits `0`
- selftest smoke passes
- offline conformance artifacts regenerate successfully
- evidence gates validate successfully

## 6. Configure Live Bridge Mode

Start from the shipped example:

```bash
cp examples/http-bridge.env.example .env.bridge
```

Edit at least:

- `SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN`
- `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP`
- `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC` if the driver is not installed in
  the environment

Recommended first live bring-up:

- `SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP=listener-only`
- `SCRATCHBIRD_AI_ADAPTER_MODE=http`
- `SCRATCHBIRD_AI_HTTP_DIALECTS=native`

Use `managed`, `ipc-only`, or `embedded` only when the target runtime and
Python driver are confirmed to support those modes.

## 7. Run The HTTP Bridge

POSIX helper:

```bash
set -a
source .env.bridge
set +a
PYTHONPATH=src tools/run_local_bridge.sh
```

Equivalent direct module entrypoint:

```bash
set -a
source .env.bridge
set +a
export PYTHONPATH=src
python3 -m scratchbird_ai.http_bridge
```

## 8. Run The MCP Server With The Bridge

POSIX helper:

```bash
set -a
source .env.bridge
set +a
export SCRATCHBIRD_AI_ADAPTER_MODE=http
export SCRATCHBIRD_AI_HTTP_BASE_URL="http://${SCRATCHBIRD_AI_BRIDGE_HOST:-127.0.0.1}:${SCRATCHBIRD_AI_BRIDGE_PORT:-3095}"
export SCRATCHBIRD_AI_HTTP_API_TOKEN="${SCRATCHBIRD_AI_BRIDGE_API_TOKEN:-}"
PYTHONPATH=src tools/run_local_stack.sh
```

Equivalent direct module entrypoint:

```bash
set -a
source .env.bridge
set +a
export PYTHONPATH=src
export SCRATCHBIRD_AI_ADAPTER_MODE=http
export SCRATCHBIRD_AI_HTTP_BASE_URL="http://${SCRATCHBIRD_AI_BRIDGE_HOST:-127.0.0.1}:${SCRATCHBIRD_AI_BRIDGE_PORT:-3095}"
export SCRATCHBIRD_AI_HTTP_API_TOKEN="${SCRATCHBIRD_AI_BRIDGE_API_TOKEN:-}"
python3 -m scratchbird_ai.mcp_server
```

## 9. Live Certification Sequence

The direct-listener certification sequence below was exercised successfully on
2026-04-20 against the shared ScratchBird node. Re-run it after environment
refreshes or when capturing new release evidence:

```bash
set -a
source .env.bridge
set +a
export SCRATCHBIRD_AI_HTTP_BASE_URL="http://${SCRATCHBIRD_AI_BRIDGE_HOST:-127.0.0.1}:${SCRATCHBIRD_AI_BRIDGE_PORT:-3095}"
export SCRATCHBIRD_AI_HTTP_API_TOKEN="${SCRATCHBIRD_AI_BRIDGE_API_TOKEN:-}"
PYTHONPATH=src python3 tools/smoke_http_contract.py --mode live --dialect native
PYTHONPATH=src python3 tools/run_live_native_conformance.py \
  --runtime-env-path "${SCRATCHBIRD_AI_LIVE_NATIVE_RUNTIME_ENV_PATH:-$HOME/.scratchbird/static-example/profiles/runtime.env}" \
  --scratchbird-server-version "${SCRATCHBIRD_AI_LIVE_NATIVE_SCRATCHBIRD_SERVER_VERSION:-current-shared-node-2026-04-20}" \
  --parser-compiler-version "${SCRATCHBIRD_AI_LIVE_NATIVE_PARSER_COMPILER_VERSION:-current-v3-prebuild}" \
  --test-dataset-version "${SCRATCHBIRD_AI_LIVE_NATIVE_TEST_DATASET_VERSION:-shared-main}" \
  --seed-or-fixture-version "${SCRATCHBIRD_AI_LIVE_NATIVE_SEED_VERSION:-shared-node}" \
  --covered-profile mcp_local_v0 \
  --covered-profile mcp_remote_v0 \
  --covered-profile streaming_async_v0 \
  --covered-profile retrieval_ingest_v0
PYTHONPATH=src python3 tools/generate_ai_conformance_artifacts.py --repo-root .
PYTHONPATH=src python3 tools/validate_evidence_gates.py --repo-root . --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md
PYTHONPATH=src python3 tools/validate_release_candidate.py --repo-root .
```

For `manager_proxy`, `local_ipc`, and `embedded_local_only`, the remaining work
is exposing those runtime modes in the environment and rerunning the same
evidence sequence, not adding new AI-repo implementation.

## 10. Notes

- `examples/http-bridge.env.example` is the source of truth for bridge
  environment keys.
- `tools/run_local_bridge.sh` and `tools/run_local_stack.sh` are convenience
  wrappers for Ubuntu/Linux shells.
- For deeper runtime settings, see `RUNTIME_CONFIGURATION_AND_GOVERNED_OPERATION.md`.

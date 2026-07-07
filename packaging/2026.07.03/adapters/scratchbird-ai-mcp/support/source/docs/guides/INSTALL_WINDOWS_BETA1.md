# Windows Install Guide (Beta 1)

Status: Active
Last Updated: 2026-04-20

## 1. Purpose

Install and run ScratchBird AI on Windows for Beta 1 review.

This guide prepares the repository for Windows reviewer use and companion-host
testing. Live Windows bridge use still depends on the Windows-capable
ScratchBird Python driver/runtime.

## 2. Current Support Boundary

Supported now:

- repository install on Windows
- local test and evidence validation
- direct Python module entrypoints for the HTTP bridge and MCP server

Still dependent on the Windows driver/runtime port:

- live bridge connections to ScratchBird
- `manager_proxy`, `local_ipc`, or `embedded` runtime testing on Windows

Use `listener-only` as the first Windows live mode once the driver/runtime is
available.

## 3. Prerequisites

Required:

- Windows PowerShell
- Python `3.11+`
- Git if you are cloning the repository locally

Optional but recommended:

- Windows Terminal
- a local ScratchBird Python driver install or source tree

## 4. Install The Repository

From PowerShell:

```powershell
git clone <your-scratchbird-repo-url>
cd ScratchBird\project\ai
py -3.11 -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install -U pip
python -m pip install -e ".[mcp]"
```

If PowerShell blocks activation for the current session:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.venv\Scripts\Activate.ps1
```

## 5. Validate The Local Baseline

```powershell
$env:PYTHONPATH = (Resolve-Path .\src).Path
python -m unittest discover -s tests -p 'test_*.py'
python tools\validate_capability_matrix.py
python tools\smoke_http_contract.py --mode selftest
python tools\generate_ai_conformance_artifacts.py --repo-root . --artifact-root build\ai\artifacts
python tools\validate_evidence_gates.py --repo-root . --artifact-root build\ai\artifacts --spec docs\releases\EARLY_BETA_CONFORMANCE_GATES.md
```

Expected result:

- test suite passes
- capability-matrix validator exits `0`
- selftest smoke passes
- offline conformance artifacts regenerate successfully
- evidence gates validate successfully

## 6. Configure Live Bridge Variables

The shipped `examples/http-bridge.env.example` file is shell-oriented. On
Windows, copy the values you need into PowerShell environment variables.

Minimum live variables:

```powershell
$env:SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN = 'scratchbird://user:password@127.0.0.1:3092/mydb'
$env:SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP = 'listener-only'
$env:SCRATCHBIRD_AI_ADAPTER_MODE = 'http'
$env:SCRATCHBIRD_AI_HTTP_DIALECTS = 'native'
```

If the driver is available only as source:

```powershell
$env:SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC = '..\drivers\driver\python\src'
```

If bridge auth is enabled:

```powershell
$env:SCRATCHBIRD_AI_BRIDGE_API_TOKEN = '<token-from-secret-provider>'
$env:SCRATCHBIRD_AI_HTTP_API_TOKEN = '<token-from-secret-provider>'
```

## 7. Run The HTTP Bridge

PowerShell module entrypoint:

```powershell
$env:PYTHONPATH = (Resolve-Path .\src).Path
python -m scratchbird_ai.http_bridge
```

Installed console script form:

```powershell
scratchbird-ai-http-bridge
```

## 8. Run The MCP Server

Set the bridge base URL first:

```powershell
$env:PYTHONPATH = (Resolve-Path .\src).Path
$env:SCRATCHBIRD_AI_ADAPTER_MODE = 'http'
$env:SCRATCHBIRD_AI_HTTP_BASE_URL = 'http://127.0.0.1:3095'
python -m scratchbird_ai.mcp_server
```

Installed console script form:

```powershell
scratchbird-ai-mcp
```

## 9. Windows Notes

- `tools/run_local_bridge.sh` and `tools/run_local_stack.sh` are POSIX shell
  wrappers and are not the recommended Windows entrypoints.
- Prefer `python -m scratchbird_ai.http_bridge` and
  `python -m scratchbird_ai.mcp_server` on Windows.
- `ipc-only` on Windows depends on Windows-capable driver/runtime transport
  support and should not be treated as ready until that port is complete.

## 10. Windows Live Recertification Path

The repo-local implementation backlog is closed for the current claim surface.
Once the Windows driver/runtime path exists, the remaining Windows follow-up is
environment execution:

- live smoke against a real ScratchBird target
- live native conformance capture against the current shared or equivalent target
- artifact regeneration, evidence-gate validation, and release-candidate validation

Use `listener-only` first on Windows, then rerun the same evidence path for any
additional runtime modes that the Windows driver/runtime exposes.

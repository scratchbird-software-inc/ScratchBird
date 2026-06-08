# Initial Early Beta Release Notes

Release: `0.1.0`  
Track: Initial Early Beta  
Release Date: 2026-02-18  
Status: Published

## 1. Release Objective

Deliver the first usable integration baseline for AI-assisted database workflows on top of ScratchBird, with safe defaults and explicit extension points for parser/compiler integration.

## 2. Shipped Components

- MCP server scaffold (`scratchbird-ai-mcp`)
- Core orchestration service:
- dialect routing
- capability gating
- compile/execute split
- policy enforcement for read-only defaults
- HTTP adapter implementations and contract handling
- Local HTTP bridge (`scratchbird-ai-http-bridge`):
- compile endpoint
- execute endpoint
- metadata endpoints
- bridge auth/token support and payload guards
- Capability matrix schema + baseline matrix payload
- CI workflows for matrix validation, lint/type checks, packaging, and tests
- Local utility scripts:
- `tools/run_local_bridge.sh`
- `tools/run_local_stack.sh`
- `tools/smoke_http_contract.py`

## 3. Beta Dialect Focus

Supported AI dialect scope for this release:

- `native`

## 4. Validation Snapshot

Validation executed for this release:

- unit tests and integration tests (`21` tests passing)
- HTTP bridge contract smoke selftest (`PASS`)
- static checks (`ruff`, `mypy`)
- package build/import checks via CI

## 5. Known Constraints

- Bridge compile probing is intentionally conservative and does not provide a full parser-service contract for every mutation scenario.
- Native-only support policy is enforced for AI integrations in this repository.
- Production operations controls (rate limiting, quotas, formal approval evidence pipeline) are not finalized in this release.
- Compatibility contract with external parser-layer teams is still in draft/finalizing stages.

## 6. Upgrade and Compatibility Notes

- Runtime settings are environment-variable driven; no migration tooling is required for this initial release.
- Adapter mode defaults to `mock`; enabling `http`/`hybrid` requires bridge configuration.
- This release expects Python `3.11+`.

## 7. Release Acceptance Checklist

- [x] Core README and docs index updated for beta usage
- [x] Early beta getting-started guide available
- [x] Bridge contract smoke tool available
- [x] Status snapshot published
- [x] Public release/status materials cross-linked

## 8. Next Milestone Targets

- Harden native metadata and compile behavior to reduce fallback paths.
- Add parser/compiler compatibility matrix tied to external parser-agent releases.
- Add governed mutation flow with stronger audit evidence and policy rule IDs.

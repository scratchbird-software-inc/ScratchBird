# Release Status and Known Gaps

**DRAFT — Early Beta documentation. Subject to revision.**

## Purpose

This chapter consolidates the status snapshots, known gap reports, support
matrix, conformance gates, and remaining live tasks for the ScratchBird AI
component as of the current early-beta release. All dates and framing reflect
the early-beta state; this content does not claim production readiness.

---

## Release Identity

| Item | Value |
| --- | --- |
| Release version | `0.1.0` |
| Release track | Early beta / technical Beta 1 |
| Original release date | 2026-02-18 |
| Status timestamp | 2026-04-21 |
| Overall status | **Green-Yellow** |

**Green-Yellow** means: the current ScratchBird-only baseline is implemented
and test-green. Direct-listener live evidence was refreshed successfully on
2026-04-20. Live evidence for the `manager_proxy`, `local_ipc`, and
`embedded_local_only` runtime modes is still environment-dependent.

---

## What Was Shipped in the Initial Release (2026-02-18)

The initial `0.1.0` release delivered:

- MCP server scaffold (`scratchbird-ai-mcp`)
- Core orchestration service with dialect routing, capability gating,
  compile/execute split, and read-only policy defaults
- HTTP adapter implementations and contract handling
- Local HTTP bridge (`scratchbird-ai-http-bridge`) with compile, execute,
  metadata, and auth endpoints
- Capability matrix schema and baseline payload
- Local utility scripts: `run_local_bridge.sh`, `run_local_stack.sh`,
  `smoke_http_contract.py`
- `native` dialect as the only supported AI dialect scope

Initial validation: 21 tests passing, HTTP bridge contract smoke selftest
passed, static checks passed.

---

## Implemented Surface Since Initial Release

By April 2026 the implemented surface expanded to include:

- Durable approval ledger with persisted identifiers, expiry, revocation, and
  replay-safe reuse checks
- Bounded compile-repair for fenced/labeled recoverable query wrappers
- In-process quotas, rate limits, cost attribution, and HTTP retry/circuit-
  breaker controls
- Fail-closed compatibility checks for declared ScratchBird server, parser,
  and driver/runtime versions
- ScratchBird-native registry publication, route resolution, graph capability
  publication, and expanded remote-auth normalization
- Remote MCP session lifecycle with bearer, OAuth2/JWT, workload identity,
  proxy principal, LDAP, Kerberos/GSSAPI, RADIUS PAP, PAM, and
  preauthenticated context auth families
- Structured JSONL runtime event logging and operator runbook/SLO bundle
  generation
- Audit attestation (HMAC-SHA256 and external reference) issue/verify
- Live-native certification harness with machine-readable artifact capture
- Ubuntu and Windows Beta 1 install guides

---

## Current Readiness by Area (April 2026)

| Area | Status | Notes |
| --- | --- | --- |
| Core service orchestration | Green | `ScratchBirdAIService` covers compile, execute, read-only query, mutation gating, explain, and retrieval |
| Native-only routing and capability matrix | Green | Router and matrix enforced fail-closed for non-native dialects |
| HTTP adapter and bridge runtime | Green | Adapter, bridge endpoints, auth checks, and service round-trip tests pass |
| Tool schema and policy guardrails | Green | Strict payload validation, error envelopes, mode normalization |
| Retrieval helpers | Green | Engine-free vector and hybrid retrieval with deterministic ranking and tenant isolation |
| Deterministic plan/audit/routing helpers | Green | Plan hashing, audit replay, cluster-routing covered by tests and evidence |
| Native live-workload hardening | Yellow | Direct-listener live path certified; broader real-workload and additional runtime-mode coverage still limited |
| Mutation governance maturity | Green | Durable ledger validation with expiry, revocation, replay-safe checks |
| Operations hardening | Green | Quotas, rate limits, cost attribution, HTTP retry/circuit-breaker implemented and tested |
| ScratchBird core AI surface truth | Green | Core packet published in capabilities and manifests |
| ScratchBird-native control/auth publication | Green | Graph ops, registry/routing controls, remote MCP, auth-family advertisement implemented |

---

## Platform and Install Support Matrix

| Area | Status | Notes |
| --- | --- | --- |
| Ubuntu install | Supported | Primary Beta 1 reviewer path |
| Windows install | Partial | Repo-local install documented; live bridge depends on Windows driver/runtime |
| Local offline validation | Supported | Tests, capability validation, artifact generation, evidence gates |
| ScratchBird Python driver from source path | Supported | Use `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC` |
| Packaged public installer | Out of scope | Source-first Beta 1 only |

---

## Runtime Mode Support Matrix

| Runtime mode | Status | Notes |
| --- | --- | --- |
| `listener_direct` | Supported | Primary live certification target; direct-listener evidence refreshed 2026-04-20; native listener at `127.0.0.1:13092` in the shared environment |
| `manager_proxy` | Supported after environment-specific live refresh | Admitted in ScratchBird core; needs live environment that exposes this mode |
| `local_ipc` | Supported after environment-specific live refresh | Requires driver/runtime IPC support |
| `embedded_local_only` | Supported after environment-specific live refresh | Requires driver/runtime support; single-connection semantics |
| HTTP bridge selftest | Supported | Covered by repo validation |
| MCP local server | Supported | Available via `mcp` optional dependency |

---

## Interface and Profile Matrix

| Interface/profile | Status | Notes |
| --- | --- | --- |
| `service_internal_v0` | Supported | Covered by refreshed direct-listener live certification |
| `mcp_local_v0` | Supported | Covered by refreshed direct-listener live certification |
| `mcp_remote_v0` | Supported | Remote session/auth/streaming surface with live certification |
| `langchain_v0` | Supported | Framework adapter with parity artifacts |
| `llamaindex_v0` | Supported | Framework adapter and retrieval wrappers with parity artifacts |
| `semantic_kernel_v0` | Supported | Framework adapter with parity artifacts |
| `provider_tool_calling_v0` | Supported | OpenAI/Anthropic/Gemini payload normalization with parity artifacts |
| `streaming_async_v0` | Supported | Polling, continuation, cancellation, SSE surface with live certification |
| `retrieval_ingest_v0` | Supported | Retrieval lifecycle with live certification for baseline behavior |
| `governance_certification_v0` | Supported | Approval validation, listing/revocation, audit replay, attestation issue/verify |

---

## Conformance Gates

The release gate is evaluated by running:

```bash
python tools/generate_ai_conformance_artifacts.py \
  --repo-root . --artifact-root ./build/ai/artifacts
python tools/validate_evidence_gates.py \
  --repo-root . --artifact-root ./build/ai/artifacts \
  --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md
```

Summary of the 13 evidence IDs (`EVID-01` through `EVID-13`):

| Evidence ID | Bound area |
| --- | --- |
| `EVID-01` | Baseline repository readiness |
| `EVID-02` | HTTP adapter and bridge runtime |
| `EVID-03` | Service orchestration surface |
| `EVID-04` | Vector retrieval API |
| `EVID-05` | Hybrid retrieval |
| `EVID-06` | Tool contract and compatibility |
| `EVID-07` | Plan introspection determinism |
| `EVID-08` | Execution mode and policy |
| `EVID-09` | Audit bundle determinism |
| `EVID-10` | Cluster-aware routing |
| `EVID-11` | Release integrity and doc alignment |
| `EVID-12` | Framework adapter parity |
| `EVID-13` | Direct provider tool-calling parity |

Rules: all artifacts must share the same `git_commit`; artifacts older than 14
days from release evaluation time fail validation; template artifacts are not
allowed.

Passing these gates means the current early-beta implementation is internally
consistent and reproducible for its shipped feature set. It does not imply
production-grade authorization depth, non-native dialect support, or live
certification for runtime modes not exposed in the active harness.

---

## Known Gaps (April 2026)

### Functional Gaps

- Explain/trace data exists at the helper and service level, but broader live
  bridge-backed explain validation is still limited.
- Native live-workload coverage is narrower than in-process and fake-backend
  contract coverage beyond the refreshed direct-listener path.
- Engine-managed retrieval depth is incomplete beyond the baseline
  live-certified lifecycle/search path.

### Governance and Security Gaps

- Fine-grained authorization and tenant boundary policy are stronger than the
  February baseline but still not production-complete.
- Third-party signing and externally hosted approval products are not finished.
  The shipped surface stops at durable local approval evidence plus HMAC or
  external-reference attestation issue/verify.

### Operational Gaps

- Operator bundle generation, runtime diagnostics, and SLO summary generation
  are implemented, but the repository does not ship a pre-generated
  target-specific production dashboard/runbook package for every environment.
- Environment-specific live evidence for `manager_proxy`, `local_ipc`, and
  `embedded_local_only` still depends on the active test harness exposing those
  runtime modes.

### Documentation and Release Gaps

- Release readiness depends on the conformance gates contract remaining aligned
  with the actual code surface.
- The remaining documentation work is current completion scope rather than later
  cleanup.

---

## What Is Intentionally Outside Beta 1

- Non-ScratchBird AI/database support
- Third-party signing infrastructure beyond local HMAC attestation
- Public packaged installers
- Production-grade authorization depth
- Automatic live certification for runtime modes not currently exposed

---

## Exit Criteria Toward the Next Milestone

1. Certify additional runtime modes on environments that expose `manager_proxy`,
   `local_ipc`, or `embedded_local_only`.
2. Expand live workload, explain/trace, and retrieval-scale certification beyond
   the bounded current claim surface.
3. Finish third-party signing and externally hosted approval productization.
4. Keep release/status materials synchronized with generated evidence.
5. Publish target-specific operator dashboard/runbook packages for supported
   environments.

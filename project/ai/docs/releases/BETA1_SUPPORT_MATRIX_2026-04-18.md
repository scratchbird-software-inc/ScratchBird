# Beta 1 Support Matrix

Date: 2026-04-21
Scope: reviewer-facing support boundary for the ScratchBird AI component Beta 1 prep.

Status labels used here:

- `Supported`: ready now in-repo
- `Supported after environment-specific live refresh`: code/docs ready; the
  remaining task is live connection evidence for a runtime mode not currently
  exposed in the active test environment
- `Partial`: usable in a bounded/dev path, but not yet a complete Beta 1 claim
- `Draft`: coded or designed, but not a Beta 1 supported surface
- `Out of scope`: not part of the Beta 1 product boundary

## 1. Platform And Install Matrix

| Area | Status | Notes |
| --- | --- | --- |
| Ubuntu install | Supported | Primary Beta 1 reviewer path; see `docs/guides/INSTALL_UBUNTU_BETA1.md` |
| Windows install | Partial | Repo-local install is documented; live bridge depends on Windows driver/runtime availability |
| Local offline validation | Supported | Tests, capability validation, offline artifact generation, and evidence-gate validation are in place |
| ScratchBird Python driver from source path | Supported | Use `SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC` when the driver is not installed |
| Packaged public installer | Out of scope | Current repo ships source-first Beta 1 instructions, not a packaged installer |

## 2. Runtime Matrix

| Runtime surface | Status | Notes |
| --- | --- | --- |
| `listener_direct` | Supported | Primary live certification target; the refreshed static-example rerun now passes on the current checkout, the regenerated conformance artifacts/evidence gates are current, the shared profile currently publishes this lane on `127.0.0.1:13092`, and the `2026-04-21` runtime repair restored native management plus family listener or parser-pool control on the shared single-listener surface |
| `manager_proxy` | Supported after environment-specific live refresh | Admitted in the ScratchBird core packet; still needs a live environment that exposes the mode here |
| `local_ipc` | Supported after environment-specific live refresh | Requires driver/runtime support in the active environment |
| `embedded_local_only` | Supported after environment-specific live refresh | Requires driver/runtime support and single-connection semantics |
| HTTP bridge selftest | Supported | Covered by current repo validation |
| MCP local server | Supported | Available via optional `mcp` dependency |

## 3. Interface/Profile Matrix

| Interface/profile | Status | Notes |
| --- | --- | --- |
| `service_internal_v0` | Supported | Implemented and covered by refreshed direct-listener live certification |
| `mcp_local_v0` | Supported | Implemented and covered by refreshed direct-listener live certification |
| `mcp_remote_v0` | Supported | Implemented remote session/auth/streaming surface with refreshed direct-listener live certification |
| `langchain_v0` | Supported | Framework adapter is implemented with parity artifacts |
| `llamaindex_v0` | Supported | Framework adapter and retrieval wrappers are implemented with parity artifacts |
| `semantic_kernel_v0` | Supported | Framework adapter is implemented with parity artifacts |
| `provider_tool_calling_v0` | Supported | Canonical provider normalization and parity artifacts are present |
| `streaming_async_v0` | Supported | Implemented polling, continuation, cancellation, and SSE event surface with refreshed direct-listener live certification |
| `retrieval_ingest_v0` | Supported | Retrieval lifecycle is implemented and now has refreshed direct-listener live certification for baseline lifecycle/search behavior |
| `governance_certification_v0` | Supported | Approval validation, approval listing/revocation, audit replay, audit attestation issue/verify, release-claim validation, and registry/governance controls are implemented locally |

## 4. Functional Matrix

| Functional area | Status | Notes |
| --- | --- | --- |
| Native-only routing | Supported | Explicit Beta 1 boundary |
| Compile/execute split | Supported | Core service behavior is implemented and tested |
| Approval-gated mutation flow | Supported | Durable local approval ledger is implemented |
| Operational controls | Supported | Request, mutation, cost, retry, and circuit-breaker controls are in place |
| ScratchBird-native control/auth publication | Supported | Graph ops, registry/routing controls, remote MCP transport, and expanded auth-family advertisement are implemented and published through capability/compatibility surfaces |
| ScratchBird core AI surface packet publication | Supported | Published in capabilities and manifests |
| Engine-owned retrieval core truth | Supported | Core packet is code-verified and the refreshed direct-listener closeout now exists; broader corpus/planner depth remains separate hardening work |
| Retrieval metadata discovery truth | Supported | `opensearch_meta.*` packet is published and direct-listener live proof has been refreshed |
| Explain/trace live validation depth | Supported | The live-native harness now exercises service-internal read execution, explain output, workload repetition, and audit replay; rerun the harness whenever the target environment changes |
| Non-native dialect AI support | Out of scope | Explicitly not part of Beta 1 |

## 5. Documentation Matrix

| Document area | Status | Notes |
| --- | --- | --- |
| Root README | Supported | Current scope and quick-start path are aligned |
| General getting-started guide | Supported | Current early-beta bring-up path is documented |
| Ubuntu install guide | Supported | Added for Beta 1 reviewer prep |
| Windows install guide | Supported | Added for Beta 1 reviewer prep, with driver/runtime caveat |
| Runtime/governance guide | Supported | Current controls are documented |
| Troubleshooting guide | Supported | Covers selftest and live bridge issues |
| Support matrix | Supported | This document |
| Live recertification runbook | Supported | See `BETA1_REMAINING_LIVE_TASKS_2026-04-18.md` for the canonical rerun recipe |

## 6. Beta 1 Closeout Summary

The repository is now ready to be shown as a technical Beta 1 within the
declared scope, based on the implemented surface and the captured 2026-04-20
direct-listener live certification packet.

The repo-local implementation closeout is complete for the current claim
surface. Remaining follow-up is environment-specific runtime certification and
current mandatory completion work beyond the current Beta 1 statement, not
unfinished baseline AI functionality. The refreshed static-example runtime has
now been revalidated for the direct-listener lane; the remaining live follow-up
is the environment-dependent certification of additional runtime modes.

What is still intentionally outside that statement:

- non-ScratchBird AI/database support
- third-party signing infrastructure beyond the implemented local attestation path
- public packaged installers

Some of the items above are now tracked as current completion work for the
project even though they remain outside the current bounded Beta 1 support
statement.
- environment-specific certification for runtime modes not exposed in the
  current shared test harness

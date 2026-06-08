# Early Beta Status Snapshot

Snapshot Date: 2026-04-21
Scope: ScratchBird AI early beta (`0.1.0`)
Overall Status: **Green-Yellow** (current ScratchBird-only baseline is implemented and test-green; direct-listener live evidence must be regenerated for the configured runtime before any live-native claim)

## 1. Executive Summary

ScratchBird AI is materially ahead of the original February 18, 2026 snapshot
and the March 7, 2026 baseline.

Current verified baseline:

- active release truth is derived from generated evidence artifacts, not from
  ad hoc checked-in output
- tracked generated artifact layout is controlled under `artifacts/ai_conformance/`
- targeted regression and live-native verification pass on the current checkout
- release evidence was regenerated on 2026-04-20 against the live-validated
  packet and revalidated against the active early-beta release gate
- release-candidate claim validation now passes on the refreshed 2026-04-20
  checkout for the currently declared profile set
- a dedicated live-native certification harness now exists for real-server runs and machine-readable artifact capture
- a direct-listener live-native certification harness exists for
  `service_internal_v0`, `mcp_local_v0`, `mcp_remote_v0`,
  `streaming_async_v0`, and `retrieval_ingest_v0`
- live-native artifacts are valid only when regenerated from the current
  checkout against the policy-configured runtime profile
- donor emulation endpoints may be profile-defined, but non-native AI support
  remains out of scope for this component
- Ubuntu and Windows Beta 1 install guides now exist for reviewer setup
- compile-repair is implemented for bounded recoverable wrapper failures
- mutation governance uses a durable approval ledger with replay-safe validation
- quotas, rate limits, cost attribution, and HTTP retry/circuit-breaker controls are implemented locally
- compatibility negotiation now fails closed for declared unsupported
  ScratchBird server/parser/runtime versions
- ScratchBird-native graph/control publication, registry/routing controls, and
  remote MCP auth-family publication are implemented and test-backed

The codebase currently provides a coherent native-only AI stack around:

- MCP-oriented service orchestration
- compile/execute split with trace and audit bundle generation
- HTTP adapter and local bridge runtime
- remote MCP sessions, non-bearer auth normalization, and ScratchBird-native
  control-family publication
- deterministic retrieval, plan, execution-mode, audit, and cluster-routing helpers

## 2. Readiness by Area

| Area | Status | Notes |
| --- | --- | --- |
| Core service orchestration | Green | `ScratchBirdAIService` covers compile, execute, read-only query flow, mutation gating, explain, and retrieval surfaces |
| Native-only routing and capability matrix | Green | Router and matrix loader are enforced fail-closed for non-native dialects |
| HTTP adapter and bridge runtime | Green | Adapter, bridge endpoints, auth checks, and service round-trip tests pass |
| Tool schema and policy guardrails | Green | Strict payload validation, error envelopes, mode normalization, and hard limits are implemented |
| Retrieval helpers | Green | Engine-free vector and hybrid retrieval paths exist with deterministic ranking and tenant isolation checks |
| Deterministic plan/audit/routing helpers | Green | Plan hashing, audit replay, and cluster-routing/failover behavior are covered by tests and release evidence |
| Native live-workload hardening | Yellow | The refreshed direct-listener live path is now certified; broader real ScratchBird workload execution and additional runtime-mode coverage are still limited |
| Mutation governance maturity | Green | Approval-gated mutation now uses durable ledger validation with expiry, revocation, and replay-safe checks |
| Operations hardening | Green | Quotas, rate limits, cost attribution, and HTTP retry/circuit-breaker policy are implemented and tested for the local service surface |
| ScratchBird core AI surface truth | Green | ScratchBird core now closes the bounded current-tree retrieval, discovery, and runtime packets; this repo publishes that packet in capabilities and manifests |
| ScratchBird-native control/auth publication | Green | Graph ops, registry/routing controls, remote MCP transport, and expanded auth family advertisement are implemented and reflected in compatibility/capability output |

## 3. Verification Snapshot

Validated on 2026-04-21 from the current checkout:

- `PYTHONPATH=src pytest -q tests/test_tool_schema.py tests/test_remote_sessions.py tests/test_service.py tests/test_mcp_server.py tests/test_live_native_conformance.py tests/test_http_service_integration.py tests/test_local_launchers.py` passed (`69` tests)
- `PYTHONPATH=src python3 -m unittest discover -s tests` passed (`196` tests, `1` skipped)
- `PYTHONPATH=src python3 tools/run_live_native_conformance.py --enable-live --launch-bridge --adapter-mode http --runtime-env-path <runtime.env> --scratchbird-server-version <server-version> --parser-compiler-version <parser-version> --test-dataset-version <dataset-version> --seed-or-fixture-version <seed-version> --covered-profile mcp_remote_v0 --covered-profile streaming_async_v0 --covered-profile retrieval_ingest_v0` is the current live-native certification shape
- `python3 tools/generate_ai_conformance_artifacts.py --repo-root . --artifact-root <build-artifact-root>` regenerates conformance artifacts for the current checkout
- `python3 tools/validate_evidence_gates.py --repo-root . --artifact-root <build-artifact-root> --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md` validates the regenerated artifact packet
- `project/output/<platform>/bin/SBsql <database> -H <host> -p <port> -U <user> --password-ref <secret-ref> --sslmode=<mode>` is the direct native client probe shape

## 4. Implemented Surface Since The Initial Snapshot

The implemented code surface now includes verified modules for:

- retrieval: vector search and hybrid search with deterministic ordering
- plan introspection: deterministic plan hashing and stable payload normalization
- execution-mode governance: canonical modes, approvals, and hard resource ceilings
- deterministic audit bundles: replay and tamper detection helpers
- cluster-aware routing: shard selection, replica failover, and deterministic merge ordering
- bridge connectivity depth: managed, listener-only, `ipc-only`, and `embedded` option mapping through the Python driver interface
- live-native certification packaging: summary, environment manifest, structured run log, and JUnit output for real-server HTTP bridge runs
- bounded compile-repair for fenced/labeled recoverable query wrappers
- durable approval ledger with persisted approval identifiers and replay-safe reuse checks
- bounded operational controls: request, mutation, and cost windows
- fail-closed compatibility checks for declared ScratchBird server/parser/runtime versions
- retry-aware and circuit-breaker-aware HTTP bridge client behavior
- ScratchBird-native registry publication, route resolution, graph capability
  publication, and expanded remote-auth normalization

## 5. Key Risks

- Live native workload coverage is still thinner than the in-process and
  fake-backend coverage, even though the refreshed static-example
  direct-listener rerun now passes again after the `2026-04-21` runtime repair
  and should be repeated whenever the shared runtime is republished.
- Environment-specific runtime-mode evidence for `manager_proxy`, `local_ipc`,
  and `embedded_local_only` still depends on a harness that exposes those
  modes.
- Larger live-scale workload, explain, and retrieval-certification studies
  remain current completion work beyond the currently shipped claim surface.
- Operator packaging exists as implemented runtime diagnostics and generated
  runbook/SLO bundles; the repository still does not ship a target-specific
  production operations package for every deployment environment.

## 6. Current Release Gate Position

Release-gate status is now based on the implemented early-beta surface in [EARLY_BETA_CONFORMANCE_GATES.md](../releases/EARLY_BETA_CONFORMANCE_GATES.md), not on the older research-oriented adapter matrix.

That means the repository can produce truthful current evidence for what is actually shipped today.

The repo-local implementation closeout is closed. Remaining follow-up is now
treated as current mandatory completion work, primarily environment-specific
runtime-mode recertification beyond the current direct-listener lane, deeper
retrieval/live-depth closure, third-party approval/signing productization, and
operator packaging.

## 7. Recommended Next Actions

1. Complete the current runtime-mode recertification work whenever the shared environment exposes `manager_proxy`, `local_ipc`, and `embedded_local_only`.
2. Complete broader live workload, explain/trace, and deeper retrieval-scale validation beyond the current bounded Beta 1 claim surface.
3. Re-run `tools/run_live_native_conformance.py`, artifact regeneration, and evidence-gate validation whenever the shared ScratchBird runtime is republished or materially changed.

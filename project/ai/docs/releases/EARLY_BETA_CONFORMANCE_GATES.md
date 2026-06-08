# Early Beta Conformance Gates

Status: Active
Release Track: `0.1.0` early beta
Last Updated: 2026-04-20

## 1. Scope

This document defines the release-gating evidence contract for the implemented ScratchBird AI early-beta surface.

It is intentionally aligned to the current repository scope:

- native-only AI workflows
- MCP-oriented service orchestration
- HTTP adapter and bridge runtime
- framework-compatible adapter shims over the canonical service surface
- direct provider tool-calling compatibility profiles
- deterministic policy, retrieval, plan, audit, and routing helpers

## 2. Release Candidate Rules

1. Every `EVID-*` row in Section 3 MUST have current machine-readable proof artifacts.
2. All JSON proof artifacts MUST share the same `git_commit`.
3. Artifacts older than 14 days from release evaluation time MUST fail validation.
4. Template or placeholder artifacts are not allowed for release validation.
5. The release gate is evaluated with:

```bash
python tools/generate_ai_conformance_artifacts.py --repo-root . --artifact-root ./build/ai/artifacts
python tools/validate_evidence_gates.py --repo-root . --artifact-root ./build/ai/artifacts --spec docs/releases/EARLY_BETA_CONFORMANCE_GATES.md
```

## 3. Evidence IDs and Gates

| Evidence ID | Bound area | Required baseline references | Minimum parity gate | Exceed gate candidates | Required proof artifact(s) |
| --- | --- | --- | --- | --- | --- |
| `EVID-01` | Baseline repository readiness | `README.md`, `docs/status/EARLY_BETA_STATUS_2026-03-07.md` (content refreshed on 2026-04-20), `docs/guides/GETTING_STARTED_EARLY_BETA.md` | Current test suite, capability-matrix validation, and HTTP selftest pass on the target commit | Full release workflow auto-generates evidence with no manual artifact edits | `artifacts/ai_conformance/01/summary.json` |
| `EVID-02` | HTTP adapter and bridge runtime | `src/scratchbird_ai/adapters/`, `src/scratchbird_ai/http_bridge.py`, `tests/test_http_bridge.py` | HTTP adapter, bridge endpoints, auth checks, and service round-trip tests pass | Live bridge smoke against a real ScratchBird server passes on the same commit | `artifacts/ai_conformance/02/adapter_parity.json`, `artifacts/ai_conformance/02/test_report.junit.xml` |
| `EVID-03` | Service orchestration surface | `src/scratchbird_ai/service.py`, `src/scratchbird_ai/router.py`, `tests/test_service.py` | Service-layer routing, compile/execute split, native-only gating, and canonical tool declarations pass | Installed MCP runtime is exercised end-to-end against a real client harness | `artifacts/ai_conformance/03/service_surface.json`, `artifacts/ai_conformance/03/test_report.junit.xml` |
| `EVID-04` | Vector retrieval API | `src/scratchbird_ai/retrieval.py`, `tests/test_retrieval.py`, `capability/capability-matrix.v0.json` | Vector insert/search validation, tenant isolation, and deterministic ordering pass | Benchmarked retrieval latency and larger-corpus quality metrics are published per release | `artifacts/ai_conformance/04/vector_api_report.json`, `artifacts/ai_conformance/04/benchmark.csv` |
| `EVID-05` | Hybrid retrieval | `src/scratchbird_ai/retrieval.py`, `src/scratchbird_ai/service.py`, `tests/test_retrieval.py` | Hybrid filter handling, deterministic ranking, and relevance expectations pass | Live native retrieval corpus evaluation with query-class-specific weighting passes | `artifacts/ai_conformance/05/hybrid_report.json`, `artifacts/ai_conformance/05/relevance_eval.json` |
| `EVID-06` | Tool contract and compatibility | `src/scratchbird_ai/tool_schema.py`, `src/scratchbird_ai/compatibility.py`, `tests/test_tool_schema.py` | Schema validation, error envelope, canonical mode declarations, and legacy alias compatibility pass | Versioned tool-schema compatibility checks run automatically across releases | `artifacts/ai_conformance/06/schema_report.json`, `artifacts/ai_conformance/06/compat_report.json` |
| `EVID-07` | Plan introspection determinism | `src/scratchbird_ai/plan_introspection.py`, `tests/test_plan_introspection.py` | Equivalent operator trees hash identically and plan payloads contain required fields | Planner diff classification is integrated into regression detection for live native workloads | `artifacts/ai_conformance/07/plan_hash_report.json`, `artifacts/ai_conformance/07/diff_report.json` |
| `EVID-08` | Execution mode and policy | `src/scratchbird_ai/execution_mode.py`, `src/scratchbird_ai/policy.py`, `tests/test_execution_mode.py` | Execution-mode state machine, durable approval checks, and resource ceilings pass | Operator attestation workflows and deny/audit correlation are enforced end-to-end for the active shipped path | `artifacts/ai_conformance/08/mode_matrix.json`, `artifacts/ai_conformance/08/policy_simulation.json` |
| `EVID-09` | Audit bundle determinism | `src/scratchbird_ai/audit_bundle.py`, `tests/test_audit_bundle.py` | Audit bundles hash deterministically and replay detects tamper or policy mismatch | Signed or externally attested bundles are produced and verified in CI | `artifacts/ai_conformance/09/audit_replay_report.json`, `artifacts/ai_conformance/09/attestation_report.json` |
| `EVID-10` | Cluster-aware routing | `src/scratchbird_ai/cluster_routing.py`, `tests/test_cluster_routing.py` | Routing, failover, merge ordering, and shard filtering pass deterministically | Real multi-shard routing with SLO-aware policies is exercised under load | `artifacts/ai_conformance/10/routing_report.json`, `artifacts/ai_conformance/10/failover_report.json` |
| `EVID-11` | Release integrity and doc alignment | `docs/README.md`, `docs/status/README.md`, `tools/validate_evidence_gates.py` | Release docs, artifact layout, validator expectations, and certification environment manifest align on the current surface | CI publishes a release dashboard that summarizes blocker reasons without manual curation | `artifacts/ai_conformance/11/matrix_status.json`, `artifacts/ai_conformance/11/environment_manifest.json` |
| `EVID-12` | Framework adapter parity | `src/scratchbird_ai/framework_adapters.py`, `tests/test_framework_adapters.py` | LangChain, LlamaIndex, and Semantic Kernel wrappers execute canonical query, explain, and retrieval flows without semantic drift | Framework-specific live compatibility windows are exercised against installed runtimes on the release commit | `artifacts/ai_conformance/12/framework_parity.json`, `artifacts/ai_conformance/12/test_report.junit.xml` |
| `EVID-13` | Direct provider tool-calling parity | `src/scratchbird_ai/provider_profiles.py`, `tests/test_provider_profiles.py` | OpenAI-style, Anthropic-style, and Gemini-style provider payloads normalize to the same canonical execution results | Installed provider SDK/runtime compatibility profiles are exercised against pinned client versions on the release commit | `artifacts/ai_conformance/13/provider_parity.json`, `artifacts/ai_conformance/13/test_report.junit.xml` |

## 4. Proof Artifact Rules

1. JSON artifacts MUST include:
   - `generated_at_utc`
   - `git_commit`
   - `status`
   - `check_count`
   - `passed_checks`
   - `failed_checks`
2. `status=PASS` requires `passed_checks == check_count` and `failed_checks == []`.
3. `status=FAIL` blocks release validation.
4. CSV artifacts MUST contain a header row and at least one data row.
5. JUnit XML artifacts MUST contain at least one `<testcase>`.

## 5. Current Release Interpretation

Passing these gates means the current early-beta implementation is internally consistent and reproducible for its shipped feature set.

It does not imply:

- production-grade authorization depth
- non-native dialect support
- live-cluster or live-retrieval production certification
- automatic certification for runtime modes not exposed in the active harness

# Public P1 Protected Material and Cloud-Ops Foundation Closure Execution_Plan

Status: active
Created: 2026-05-10
Owner: ScratchBird public P1 closure coordinator
Search key: `PUBLIC-P1-PROTECTED-MATERIAL-CLOUD-OPS-FOUNDATION-CLOSURE`

## Purpose

Close the five remaining public `P1` / `not_implemented` rows that block the
next public zero-grey tranche:

| Gap | Registry row | Title |
| --- | --- | --- |
| `SB-PUBLIC-GAP-0008` | `SB-SPEC-IMPLEMENTATION-0047` | Protected Versioned Material Catalog Family |
| `SB-PUBLIC-GAP-0168` | `SB-SPEC-IMPLEMENTATION-0170` | Cloud Support Architecture and Provider Capability Registry |
| `SB-PUBLIC-GAP-0169` | `SB-SPEC-IMPLEMENTATION-0171` | Cloud Identity, KMS, and Secretless Authentication |
| `SB-PUBLIC-GAP-0170` | `SB-SPEC-IMPLEMENTATION-0172` | Kubernetes Operator CRDs and Lifecycle Automation |
| `SB-PUBLIC-GAP-0173` | `SB-SPEC-IMPLEMENTATION-0176` | Edge Cache and CDN Integration and Invalidation |

The expected release outcome is that the public gap registry moves from `29`
public-open rows to `24`, with all five target rows closed by evidence.

## Non-Negotiable Rules

- Contract authority comes first. Implementation cannot close a row unless
  the canonical spec and registry state describe the behavior.
- Protected material is catalog authority and security policy data. It must use
  UUID identity, versioned records, access/release/retention/purge policy, audit
  events, and MGA transaction visibility.
- No plaintext secret, password, key, token, credential, or provider secret may
  be stored in the engine. Engine-owned records store protected references,
  hashes, envelopes, policy metadata, and audit evidence only.
- Cloud identity and KMS behavior must be fail-closed. Static secret mode is
  forbidden by default and can be accepted only through explicit policy with
  audit evidence.
- Kubernetes operator assets are public single-node orchestration surfaces. They
  must not enable private cluster behavior or route transactions down cluster
  paths.
- Edge cache/CDN invalidation is advisory/export behavior. It cannot become
  transaction, visibility, durability, or recovery authority.
- MGA remains the only single-node transaction/recovery model. No WAL, reference
  storage, cloud provider journal, Kubernetes status, or CDN event stream can be
  treated as transaction finality.
- All external integration surfaces need deterministic refusal when unavailable,
  unconfigured, unauthenticated, out-of-policy, or unsupported.

## Implementation Order

```text
P0  Execution_Plan hardening, target evidence, authority baseline, and registry guard
P1  Protected versioned material catalog family
P2  Cloud provider capability registry and deployment profile selector
P3  Cloud identity, KMS, envelope, and secretless authentication
P4  Kubernetes operator CRDs, lifecycle automation, and idempotency evidence
P5  Edge cache/CDN tag registry, invalidation stream, and provider adapter
P6  Full-route validation, negative/refusal tests, inventory, and registry sync
```

## Required Artifacts

Implementation must not proceed beyond P0 until these files exist:

- `artifacts/TARGET_GAPS.csv`
- `artifacts/TARGET_EVIDENCE_MANIFEST.csv`
- `artifacts/AGENT_WRITE_SCOPE_MATRIX.csv`
- `artifacts/AGENT_STATUS.csv`
- `artifacts/AI_BUDGET_CONTINGENCY.md`
- `artifacts/HARDENING_REQUIREMENTS.md`
- `artifacts/PROTECTED_VERSIONED_MATERIAL_CATALOG_MODEL.md`
- `artifacts/CLOUD_PROVIDER_CAPABILITY_REGISTRY_MODEL.md`
- `artifacts/CLOUD_IDENTITY_KMS_SECRETLESS_MODEL.md`
- `artifacts/KUBERNETES_OPERATOR_LIFECYCLE_MODEL.md`
- `artifacts/EDGE_CACHE_CDN_INVALIDATION_MODEL.md`
- `artifacts/FULL_ROUTE_ACCEPTANCE_FIXTURE.md`
- `artifacts/THREAT_MODEL_AND_SECRET_REDACTION_MATRIX.md`
- `artifacts/DIAGNOSTIC_CODE_MATRIX.md`
- `artifacts/BOOTSTRAP_AND_DEFAULT_POLICY_MATRIX.md`
- `artifacts/LOCAL_EMULATOR_FIXTURE_POLICY.md`
- `artifacts/BACKUP_RESTORE_PITR_PROTECTED_MATERIAL_POLICY.md`
- `artifacts/PUBLIC_PRIVATE_BOUNDARY_MATRIX.md`
- `artifacts/METRICS_AND_AUDIT_EVIDENCE_MODEL.md`
- `artifacts/FINAL_RELEASE_DECLARATION_MODEL.md`
- `artifacts/MANAGEMENT_SURFACE_AND_SBLR_OPERATION_MATRIX.md`
- `artifacts/PERSISTED_FORMAT_AND_MIGRATION_POLICY.md`
- `artifacts/EXTERNAL_EFFECT_OUTBOX_AND_IDEMPOTENCY_POLICY.md`
- `artifacts/RESOURCE_LIMITS_AND_BACKPRESSURE_POLICY.md`
- `artifacts/PUBLIC_ABI_AND_PACKAGE_MANIFEST_MATRIX.md`
- `artifacts/CONFORMANCE_MANIFEST_AND_RELEASE_GATE_RECORDS.md`
- `artifacts/ADMIN_DOCS_AND_SAMPLE_FLOW.md`
- `artifacts/P0_EXECUTION_PLAN_CREATION_EVIDENCE.md`

## Required Implementation Slices

| Slice family | Required closure |
| --- | --- |
| Protected material catalog | Catalog descriptors, version descriptors, material identity, version identity, storage class, retention, access, release, purge, and audit policy references. |
| Protected material API | Engine internal API to create, version, resolve, release, purge, and audit protected material without exposing plaintext secret material. |
| Security integration | Auth/security policy must consume protected material records for credential history, provider verifier references, KMS references, release authorization, and audit. |
| Cloud provider registry | Provider capability profile records for identity modes, KMS/HSM modes, object storage, route, observability, operator, edge-cache, and refusal capabilities. |
| Cloud profile selection | Deployment profile selector and policy validation that bind provider capability UUIDs to database/server configuration. |
| KMS and secretless auth | Workload identity, OIDC federation, managed identity, IAM role, service account token, envelope references, rotation, and static-secret refusal. |
| Kubernetes CRDs | CRD schemas for the public single-node applicable resources plus exact refusal for cluster-only fields. |
| Operator reconciler | Idempotent reconciliation model, status/evidence records, lifecycle commands, diagnostic vectors, and dry-run validation. |
| Edge/CDN | Cache tag descriptors, finality-labeled invalidation events after MGA commit, redaction-safe payloads, signed stream metadata, provider adapter contract. |
| Threat model and redaction | Logs, diagnostics, audit events, traces, operator status, edge payloads, metrics labels, and failure messages must never expose secret material. |
| Diagnostics and refusal | Every target refusal path has stable diagnostic code, retryability, audit expectation, and transaction-finality behavior. |
| Bootstrap and defaults | Database create seeds protected-material catalog structures and default fail-closed cloud/KMS/operator/edge policies. |
| Local emulator fixtures | Cloud/KMS/operator/edge tests run without external provider dependencies and still prove configured success plus fail-closed paths. |
| Backup/restore/PITR | Protected material references, purge state, key-reference rotation, and audit lineage survive or refuse backup/restore/PITR exactly as policy specifies. |
| Public/private boundary | Cluster-only cloud/operator fields are absent or refused in public single-node builds with deterministic diagnostics. |
| Metrics and audit | `sys.metrics` and audit evidence cover access/release/deny/purge, KMS release/deny, operator reconcile, and edge invalidation. |
| Management/SBLR surface | SBSQL/native management commands, engine internal APIs, SBLR operations, CLI/admin hooks, and show/list surfaces are specified before implementation. |
| Persisted format and migration | New catalog/page/table families have format versioning, bootstrap migration, restricted-open, downgrade refusal, repair, and diagnostic behavior. |
| External-effect outbox | KMS release, operator reconcile, and CDN invalidation side effects are queued after commit, retried idempotently, audited, and never transaction authority. |
| Resource limits/backpressure | Protected history, edge tag fanout, invalidation storms, KMS retries, operator reconcile loops, and audit/metric volume are bounded. |
| ABI/package manifests | Public/internal APIs, package assets, operator manifests, and any UDR-facing symbols are declared and gated. |
| Conformance manifests | New CTest gates are registered in release-gate records and conformance manifests before final closure. |
| Admin docs/sample flow | Executable admin examples prove the full route and cite validated gates. |
| Tests and gates | Unit, conformance, negative/refusal, route, catalog, security, cloud emulator, operator schema, and edge invalidation tests in CTest. |

## Agent Execution Model

When implemented under agent management, use disjoint write scopes and five
minute heartbeat updates in `artifacts/AGENT_STATUS.csv`:

| Agent | Ownership |
| --- | --- |
| `catalog_security_agent` | Protected material catalog specs, records, APIs, security integration, and tests. |
| `cloud_registry_agent` | Cloud provider capability registry, deployment profiles, provider policy, and tests. |
| `identity_kms_agent` | Secretless identity, KMS/HSM/envelope contracts, static-secret refusal, and tests. |
| `operator_agent` | Kubernetes CRDs, reconciler model, operator diagnostics, packaging, and tests. |
| `edge_cache_agent` | Cache tag/invalidation registry, edge adapter contract, redaction, signed stream, and tests. |
| `verification_agent` | Target evidence manifest, CTest integration, registry sync, inventory update, final audit, and closure report. |

Agents must not edit another agent's owned paths unless coordination is recorded
in `artifacts/AGENT_STATUS.csv`.

## Definition Of Done

This execution_plan is complete only when:

- All five rows in `artifacts/TARGET_EVIDENCE_MANIFEST.csv` are
  `implemented_in_full`.
- Required canonical specs and implementation-expanded packets are updated
  before code claims are made.
- CTest includes the target gates named in `artifacts/TARGET_EVIDENCE_MANIFEST.csv`.
- Protected material tests prove versioning, policy, purge, access denial,
  audit, and MGA rollback/commit visibility.
- Cloud/KMS/operator/edge tests prove configured success and deterministic
  fail-closed behavior when credentials, providers, CRDs, or policies are
  missing or invalid.
- The full public registry regeneration uses
  `public_spec_gap_id_authority.csv`, both prior closed execution_plan overlays, and
  this execution_plan overlay.
- `public_spec_closed_execution_plan_regression_gate` still passes and
  `public_open_entries` is `24`.
- `public_audit_summary`,
  `public_audit_summary`, and this
  tracker are synchronized.
- Final release declaration records all five target rows, evidence artifacts,
  CTest gates, closure status, and remaining public-open row count.
- External provider side effects are emitted through post-commit outbox records
  with idempotency keys and cannot be treated as MGA commit, rollback,
  durability, or recovery evidence.

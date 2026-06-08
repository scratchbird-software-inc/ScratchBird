# Management Surface and SBLR Operation Matrix

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_MANAGEMENT_SURFACE_AND_SBLR_OPERATION_MATRIX`

The five target rows require explicit management surfaces before implementation
can claim closure. The engine remains SBLR-only; SQL text is lowered by parsers
to SBLR or engine-owned internal API calls.

## Required Surfaces

| Area | Required commands and APIs |
| --- | --- |
| Protected material | create, alter/rotate, release, purge, show/list, policy bind, audit query |
| Cloud provider profile | create, alter, drop/refuse-in-use, show/list, validate capability |
| KMS profile | create, alter/rotate, disable, show/list, validate release policy |
| Operator lifecycle | validate CRD, dry-run reconcile, reconcile request, support bundle request, show status |
| Edge cache | create tag, alter tag, drop tag, emit invalidation, show invalidation status |

Required lower-case audit anchors:

- protected material: `protected_material_catalog_gate`
- cloud provider: `cloud_provider_capability_registry_gate`
- cloud identity: `cloud_identity_kms_secretless_gate`
- Kubernetes: `kubernetes_operator_lifecycle_gate`
- edge cache: `edge_cache_cdn_invalidation_gate`

## Required Matrix Columns

The implementation phase must produce a machine-readable matrix with:

- management command or internal API name
- parser profile exposure
- SBLR operation or internal procedure identifier
- required authorization
- transaction mode
- diagnostics on refusal
- audit event
- CTest label

No management path may execute SQL inside the engine.

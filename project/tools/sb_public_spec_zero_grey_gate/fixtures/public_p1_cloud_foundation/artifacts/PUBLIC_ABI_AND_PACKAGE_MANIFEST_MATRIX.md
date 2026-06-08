# Public ABI and Package Manifest Matrix

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_PUBLIC_ABI_AND_PACKAGE_MANIFEST_MATRIX`

Any public/internal API, package asset, operator manifest, CLI command, UDR-facing
symbol, or management binary surface introduced by this execution_plan must be
declared and gated.

Required matrix entries:

- symbol or command name
- component owner
- public/private/internal classification
- package or install location
- version and compatibility rule
- refusal behavior if feature is unavailable
- CTest gate label

Required checks:

- public ABI symbol gate includes any new exported symbols
- package manifests include Kubernetes/operator assets where applicable
- CLI/admin tool commands are listed and tested
- no cluster-only package is included in public single-node output
- no UDR-facing API bypasses engine security or MGA authority

Required CTest gate labels:

- `protected_material_catalog_gate`
- `cloud_provider_capability_registry_gate`
- `cloud_identity_kms_secretless_gate`
- `kubernetes_operator_lifecycle_gate`
- `edge_cache_cdn_invalidation_gate`

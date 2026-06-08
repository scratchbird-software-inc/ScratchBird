# Conformance Manifest and Release Gate Records

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_CONFORMANCE_MANIFEST_AND_RELEASE_GATE_RECORDS`

The new CTest gates must be visible to the release framework, not only to this
execution_plan.

Required gate labels:

- `protected_material_catalog_gate`
- `cloud_provider_capability_registry_gate`
- `cloud_identity_kms_secretless_gate`
- `kubernetes_operator_lifecycle_gate`
- `edge_cache_cdn_invalidation_gate`
- `public_p1_cloud_management_sblr_abi_gate`
- `public_p1_cloud_operational_hardening_gate`
- `public_p1_cloud_foundation_full_route_gate`

Required closure:

- release-gate records cite all five target gap IDs
- conformance manifest cites every gate label
- final audit lists command lines and pass/fail results
- registry target evidence references the final CTest labels

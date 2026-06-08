# Bootstrap and Default Policy Matrix

Search key: `PUBLIC_P1_CLOUD_FOUNDATION_BOOTSTRAP_AND_DEFAULT_POLICY_MATRIX`

Database create must seed the target surfaces during transaction 1 so all later
transactions observe them through MGA rules.

| Bootstrap item | Default |
| --- | --- |
| `sys.catalog.protected_material` | present, UUID keyed, no user-facing names inside low-level rows |
| `sys.catalog.protected_material_version` | present, UUID keyed, versioned and policy-linked |
| `sys.security.protected_material_policy` | deny by default except engine bootstrap owner |
| `sys.security.kms_release_policy` | deny by default until provider/profile is configured |
| `sys.cloud.provider_capability_profile` | local emulator profile plus explicit unsupported cloud provider states |
| `sys.cloud.deployment_profile` | local single-node profile |
| `sys.cloud.kms_profile` | local emulator disabled-by-default profile |
| `sys.cloud.operator_policy` | dry-run only by default |
| `sys.cloud.edge_cache_policy` | disabled/fail-closed by default |
| `sys.metrics.cloud_ops` | metric descriptors registered but zero-valued until use |

All default policies must be created in one central bootstrap function or
registry so new defaults are not scattered across unrelated code paths.

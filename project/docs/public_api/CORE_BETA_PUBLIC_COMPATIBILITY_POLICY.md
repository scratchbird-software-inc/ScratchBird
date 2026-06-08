# Core Beta Public Compatibility Policy

PUBLIC_API_COMPATIBILITY_POLICY

Authority: public_release_evidence_only.

This policy describes the public compatibility promises checked for the first
Core Beta release gate. It is paired with
`CORE_BETA_PUBLIC_API_ABI_MANIFEST.json`; the manifest remains the
machine-readable API/ABI freeze source.

## Semantic Versioning

- PUBLIC_API_ABI_SURFACE is versioned by `SB_ENGINE_ABI_VERSION_MAJOR`,
  `SB_ENGINE_ABI_VERSION_MINOR`, and `SB_ENGINE_ABI_VERSION_PATCH`.
- Major versions may remove or break public ABI symbols only after a recorded
  removal gate exists.
- Minor versions may add compatible public headers, symbols, providers,
  extension points, config keys, diagnostics, and policy-pack schema rows.
- Patch versions must not add, remove, or reorder public ABI symbols.
- Deprecation requires a stable diagnostic or manifest row, a replacement path,
  and at least one release cycle before removal.

## Compatibility Surfaces

| Surface | Compatibility rule | Public evidence |
| --- | --- | --- |
| Public headers | Header inventory must match `public_headers` in the manifest. | `sb_engine_public_headers_api_docs_freeze_gate`; `PUBLIC_API_ABI_SURFACE` |
| C API symbols | Symbol inventory must match `c_api_symbols` in the manifest. | `sb_engine_public_abi_symbol_gate`; `sb_engine_abi_version_packed` |
| Provider ABI | Cluster provider and extension boundaries must stay provider-gated and fail closed without external authority. | `public_cluster_provider_handshake_gate`; `SBLR.CLUSTER.SUPPORT_NOT_ENABLED` |
| File format | Version metadata and upgrade gates must refuse unsupported or downgraded format transitions. | `public_release_version_metadata_gate`; `public_upgrade_migration_gate` |
| Diagnostics | Diagnostic shape, message keys, redaction class, and compatibility status must stay stable. | `public_diagnostic_stability_gate`; `PUBLIC_DIAGNOSTIC_MATRIX_GENERATOR` |
| Config | Public secure defaults and config schema checks must remain fail-closed. | `public_default_config_check`; `PUBLIC_DEFAULT_CONFIG_CHECK` |
| Policy pack schema | Default policy-pack schema versions and catalog import/mutation gates must remain compatible. | `public_policy_pack_manifest_gate`; `public_policy_pack_catalog_import_gate` |
| Extension points | Parser package, UDR runtime, cluster provider, metrics, agents, and manager boundaries must keep contract versions. | `cluster_provider.v1`; `sb_parser_package_v3`; `sb_udr_v1` |

## Removal Gate

Removal of a public header, C ABI symbol, provider contract, diagnostic code,
config key, policy-pack schema row, or extension boundary is forbidden for the
current Core Beta ABI version. A future removal requires a major-version gate,
explicit deprecation evidence, replacement guidance, and public tests proving
old clients fail closed with stable diagnostics.

# Compatibility and Stability Policy

## Purpose

This chapter summarizes the `CORE_BETA_PUBLIC_COMPATIBILITY_POLICY.md` and freeze invariants relevant to an embedder. It answers: which surfaces are stable, which may change, how to verify at runtime that the library version matches the headers, and what the removal gate requires.

The source authority for all claims in this chapter is `project/docs/public_api/CORE_BETA_PUBLIC_COMPATIBILITY_POLICY.md` and `project/docs/public_api/CORE_BETA_PUBLIC_API_ABI_MANIFEST.json`. The policy document's own authority is stated as `public_release_evidence_only`.

## Semantic Versioning Rules

The public ABI surface is versioned with `SB_ENGINE_ABI_VERSION_MAJOR`, `SB_ENGINE_ABI_VERSION_MINOR`, and `SB_ENGINE_ABI_VERSION_PATCH`, packed into `SB_ENGINE_ABI_VERSION_PACKED`. The rules from the policy:

| Version increment | Permitted changes |
| --- | --- |
| **Major** | May remove or break public ABI symbols, but only after a recorded removal gate exists |
| **Minor** | May add compatible public headers, symbols, providers, extension points, config keys, diagnostics, and policy-pack schema rows |
| **Patch** | Must not add, remove, or reorder public ABI symbols |

A deprecation requires three things before removal is permitted: a stable diagnostic or manifest row marking the deprecation, a replacement path, and at least one release cycle between deprecation and removal.

## Frozen ABI Version

| Attribute | Value |
| --- | --- |
| Family | `sb_engine_public_abi` |
| Major | 1 |
| Minor | 0 |
| Patch | 0 |
| Packed (decimal) | 65536 |
| Packed (hex) | `0x00010000` |
| Macro | `SB_ENGINE_ABI_VERSION_PACKED` |
| Freeze ID | `core_beta_public_api_abi_freeze_2026_05` |
| Freeze status | `frozen_for_core_beta_qa` |

## Compatibility Surfaces

The policy defines eight compatibility surfaces with their governing CTest gate and public evidence anchor:

| Surface | Rule | Evidence anchor |
| --- | --- | --- |
| Public headers | Header inventory must match `public_headers` in the manifest | `sb_engine_public_headers_api_docs_freeze_gate`; `PUBLIC_API_ABI_SURFACE` marker in each header |
| C API symbols | Symbol inventory must match `c_api_symbols` in the manifest | `sb_engine_public_abi_symbol_gate`; `sb_engine_abi_version_packed` |
| Provider ABI | Cluster provider and extension boundaries must stay provider-gated and fail closed without external authority | `public_cluster_provider_handshake_gate`; `SBLR.CLUSTER.SUPPORT_NOT_ENABLED` |
| File format | Version metadata and upgrade gates must refuse unsupported or downgraded format transitions | `public_release_version_metadata_gate`; `public_upgrade_migration_gate` |
| Diagnostics | Diagnostic shape, message keys, redaction class, and compatibility status must stay stable | `public_diagnostic_stability_gate`; `PUBLIC_DIAGNOSTIC_MATRIX_GENERATOR` |
| Config | Public secure defaults and config schema checks must remain fail-closed | `public_default_config_check`; `PUBLIC_DEFAULT_CONFIG_CHECK` |
| Policy pack schema | Default policy-pack schema versions and catalog import/mutation gates must remain compatible | `public_policy_pack_manifest_gate`; `public_policy_pack_catalog_import_gate` |
| Extension points | Parser package, UDR runtime, cluster provider, metrics, agents, and manager boundaries must keep contract versions | `cluster_provider.v1`; `sb_parser_package_v3`; `sb_udr_v1` |

## What Is Stable for Embedders

For an embedder using only the frozen public C and C++ surfaces:

**Stable (may not be removed without a major-version removal gate):**
- All 20 C ABI symbols listed in `CORE_BETA_PUBLIC_API_ABI.md`
- All 16 public header files in `project/include/scratchbird/engine/`
- All enum enumerators in `error.h`, `diagnostic.h`, `engine.h`, `execution_type_descriptor.hpp`, `descriptor.hpp`, `value.hpp`, and `sblr_envelope.hpp`
- All struct field layouts in `_v1_t` parameter structs (fields are not reordered; reserved fields stay reserved)
- The ABI version macro values (changing `SB_ENGINE_ABI_VERSION_PACKED` is a break)
- Diagnostic symbolic codes cited in this manual (e.g., `SBLR.EXECUTION.ADMISSION_ONLY`, `SBLR.CLUSTER.SUPPORT_NOT_ENABLED`)

**May be extended (minor version, additive only):**
- New public headers may be added to the install set
- New C ABI symbols may be added
- New enum enumerators may be added to extensible enums
- New `_v2_t` or later versioned parameter structs may be added

**May change (minor or patch):**
- The content of the `sb_engine_abi_build_id` string (it is opaque and version-specific)
- The byte encoding of `PlainValuePayload` canonical bytes and `ExecutionTypeDescriptor` canonical bytes, since the internal format is not specified in the public headers
- Engine-side behavior under `admission_only` operation families (these are not execution behavior claims)

## Runtime ABI Verification

Every embedder should verify the loaded library version matches the headers used at compile time before calling any other function:

```c
#include "scratchbird/engine/version.h"

uint32_t runtime = sb_engine_abi_version_packed();
if (runtime != SB_ENGINE_ABI_VERSION_PACKED) {
    /* version mismatch: do not proceed */
}
```

This check is performed by the public consumer smoke example (`project/examples/public_engine_consumer_smoke/main.c`, line 40).

The build ID string provides additional disambiguation when the version is correct but the precise build matters:

```c
const char* build_id = NULL;
uint64_t build_id_size = 0;
sb_engine_abi_build_id(&build_id, &build_id_size);
```

## Architectural Invariants

The manifest records these invariants as machine-checked (non-negotiable for any build claiming the frozen surface):

| Invariant | Value |
| --- | --- |
| Engine execution authority | `engine_sblr_internal_api_only` |
| SQL text as runtime authority | `false` |
| UUID identity as authoritative | `true` |
| MGA finality as authoritative | `true` |
| Cluster-positive behavior in core | `false` |

These are not documentation conventions; they are architecture-level properties enforced by the engine and verified by the CTest gate suite. An embedder can rely on these invariants holding for any library that passes the freeze gate.

## Removal Gate

From the policy:

> Removal of a public header, C ABI symbol, provider contract, diagnostic code, config key, policy-pack schema row, or extension boundary is **forbidden** for the current Core Beta ABI version.

A future removal requires all of:
1. A major-version gate (incrementing `SB_ENGINE_ABI_VERSION_MAJOR`)
2. Explicit deprecation evidence (a manifest row or stable diagnostic marking the item deprecated)
3. A documented replacement path
4. Public tests proving old clients fail closed with stable diagnostics

An embedder does not need to guard against removal of any currently frozen symbol for the lifetime of the `1.0.x` ABI version family.

## Extension Boundary Contract Versions

For embedders who also implement extension boundaries (not typical for pure embedders):

| Boundary | Type | Contract version | Core classification |
| --- | --- | --- | --- |
| `parser_package.sbsql_v3` | Parser package | `sb_parser_package_v3` | core |
| `udr_package.trusted_cpp_v1` | UDR package | `sb_udr_v1` | core |
| `cluster_provider.v1` | Cluster provider | `sb_cluster_provider_v1` | non_core_cluster |
| `cluster_metrics.v1` | Cluster metrics | `sb_cluster_metrics_v1` | non_core_cluster |
| `cluster_agents.v1` | Cluster agents | `sb_cluster_agents_v1` | non_core_cluster |
| `cluster_manager.v1` | Cluster manager | `sb_cluster_manager_v1` | non_core_cluster |

Non-cluster boundaries fail closed with their respective diagnostic codes when a cluster provider is absent.

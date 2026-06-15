# Diagnostics Reference

## Purpose

This page is a consolidated reference for the `SBSQL.LANG_RESOURCE.*` and
related language-resource diagnostic codes emitted by the ScratchBird parser
and renderer. All codes are verified against
`project/src/parsers/sbsql_worker/resources/language_resource_contract.cpp`
and `project/drivers/language/sbsql_language_surface_manifest.json`.

Codes are organized by functional area. For the rendering-layer diagnostics
that are formally declared in `common_resource_pack_metadata`, see the
[Rendering and Fallback](rendering_and_fallback.md) page for contextual detail.

## Session and Parse-Profile Diagnostics

These diagnostics relate to which parse profile was selected for a session or
statement.

| Code | Severity | Emitted when | Guidance |
|------|----------|-------------|---------|
| `SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH` | warning | Preferred-language renderer unavailable; canonical English renderer selected | Verify the preferred locale resource pack is correctly installed and admitted. The session language setting is preserved. |
| `SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH` | error | No parse succeeded with any profile; operation refused | Check that the active language resource pack matches the statement's locale. Confirm the pack is signed and admitted. |

## Rendering Diagnostics

These diagnostics are emitted per render operation (SBLR to SBsql text).

| Code | Severity | Emitted when | Guidance |
|------|----------|-------------|---------|
| `SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED` | info | Any render decision is made | Inspect the `renderer_lossiness` field of the diagnostic for the specific lossiness class (`lossless_canonical`, `canonical_equivalent`, `preferred_language_partial`, `canonical_english_fallback`, `not_renderable`). |
| `SBSQL.LANG_RESOURCE.RENDERER_SOURCE_RECONSTRUCTION_FORBIDDEN` | error | A render request with `source_reconstruction_requested: true` was received | Source reconstruction from SBLR is not permitted. The renderer produces canonical output, not original source text. |
| `SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE` | error | No admitted renderer (preferred or English fallback) is available | Install the required language resource pack or allow canonical English fallback. |
| `SBSQL.LANG_RESOURCE.MISSING_CANONICAL_AUTHORITY` | error | SBLR UUID authority cannot be validated | The SBLR presented for rendering does not have valid UUID authority. The server must admit the SBLR before it can be rendered. |

## Resource Lifecycle and Admission Diagnostics

These diagnostics relate to the lifecycle state of a language resource or bundle.

| Code | Severity | Emitted when | Guidance |
|------|----------|-------------|---------|
| `SBSQL.LANG_RESOURCE.MISSING` | error | Required language resource is unavailable | Install the required language resource pack. |
| `SBSQL.LANG_RESOURCE.UNSIGNED` | error | Language resource has no signature identity | Ensure the resource pack was generated and signed through the official toolchain. |
| `SBSQL.LANG_RESOURCE.SIGNING_KEY_MISSING` | error | Language resource has no signing key identity | The signing key identity is required in the manifest. |
| `SBSQL.LANG_RESOURCE.REVOKED` | error | Language resource or bundle has been revoked | Remove the revoked pack. Do not attempt to re-admit it. Obtain a replacement pack if needed. |
| `SBSQL.LANG_RESOURCE.EXPIRED` | error | Language resource has expired | Obtain and install a non-expired pack. |
| `SBSQL.LANG_RESOURCE.INCOMPATIBLE` | error | Language resource is incompatible with this parser version | Check parser version compatibility ranges in the bundle manifest. |
| `SBSQL.LANG_RESOURCE.UNSUPPORTED_CHANNEL` | error | Language resource channel is unsupported | Only `experimental`, `preview`, `beta`, `release_supported`, and `deprecated` channels are loadable. |
| `SBSQL.LANG_RESOURCE.AMBIGUOUS_FALLBACK` | error | Language resource fallback chain is ambiguous | Each profile may have at most one fallback parent; circular or multi-path fallback chains are refused. |
| `SBSQL.LANG_RESOURCE.REMOVED` | error | Language resource has been removed | The resource is permanently unavailable. Obtain a replacement. |
| `SBSQL.LANG_RESOURCE.TOPOLOGY_DIALECT_UNICODE_UNSUPPORTED` | error | Language resource topology or dialect Unicode profile is unsupported | Check topology and dialect compatibility with the parser version. |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_RESOURCE_REFUSED` | error | Predictive language resource was refused | Check predictive resource limits and admission criteria. |
| `SBSQL.LANG_RESOURCE.LOCAL_DRAFT_SBLR_REFUSED` | error | Local draft SBLR is untrusted and was refused by the server | The server revalidation of a client-produced draft SBLR failed. Submit for server revalidation and resolve any admission errors. |

## Lifecycle Channel Diagnostics

These diagnostics are warnings emitted when a resource with a non-release
channel is loaded. They do not prevent loading but indicate support limitations.

| Code | Severity | Emitted when | Guidance |
|------|----------|-------------|---------|
| `SBSQL.LANG_RESOURCE.EXPERIMENTAL_UNSUPPORTED` | warning | An experimental language resource is admitted | Experimental resources have no support commitment. Do not use in production. |
| `SBSQL.LANG_RESOURCE.PREVIEW_LIMITED_SUPPORT` | warning | A preview language resource is admitted | Preview resources require native review and are not release-supported. |
| `SBSQL.LANG_RESOURCE.BETA_LIMITED_SUPPORT` | warning | A beta language resource is admitted | Beta resources are machine-generated and require native review before release support. |
| `SBSQL.LANG_RESOURCE.RELEASE_SUPPORTED` | info | A release-supported language resource is admitted | Informational — the admitted resource meets release support criteria. |
| `SBSQL.LANG_RESOURCE.DEPRECATED` | warning | A deprecated language resource is admitted | The resource is still usable but has been deprecated. Plan migration to a supported replacement. |

## Validation Error Diagnostics

These error codes are produced at resource manifest validation time, not at
runtime. They appear in operator-facing admission logs.

| Code | Meaning |
|------|---------|
| `SBSQL.LANG_RESOURCE.PROFILE_UUID_MISSING` | Profile UUID not present in manifest |
| `SBSQL.LANG_RESOURCE.EXACT_TAG_MISSING` | Exact language tag not present |
| `SBSQL.LANG_RESOURCE.COMMON_HASH_MISSING` | Common resource hash not present |
| `SBSQL.LANG_RESOURCE.SURFACE_REGISTRY_HASH_MISSING` | Canonical surface registry hash not present |
| `SBSQL.LANG_RESOURCE.SBLR_REGISTRY_HASH_MISSING` | SBLR registry hash not present |
| `SBSQL.LANG_RESOURCE.SUPPORT_STATE_MISMATCH` | Release-supported channel requires release-supported support state |
| `SBSQL.LANG_RESOURCE.GOVERNANCE_EVIDENCE_MISSING` | Release resource missing full governance evidence |
| `SBSQL.LANG_RESOURCE.AUTHOR_MISSING` | Author identity required for release resources |
| `SBSQL.LANG_RESOURCE.REVIEWER_MISSING` | Reviewer identity required for release resources |
| `SBSQL.LANG_RESOURCE.NATIVE_TECHNICAL_REVIEWER_MISSING` | Native technical reviewer required for release resources |
| `SBSQL.LANG_RESOURCE.SECURITY_REVIEWER_MISSING` | Security reviewer required for release resources |
| `SBSQL.LANG_RESOURCE.SUPPORT_OWNER_MISSING` | Support owner required for release resources |
| `SBSQL.LANG_RESOURCE.RELEASE_APPROVAL_MISSING` | Release approval evidence required |
| `SBSQL.LANG_RESOURCE.REVOCATION_POLICY_MISSING` | Revocation policy evidence required |
| `SBSQL.LANG_RESOURCE.CONTRIBUTION_PROVENANCE_MISSING` | Contribution provenance evidence required |
| `SBSQL.LANG_RESOURCE.DEPRECATION_NOTICE_MISSING` | Deprecated resources require deprecation notice evidence |
| `SBSQL.LANG_RESOURCE.REVOCATION_NOTICE_MISSING` | Revoked resources require revocation notice evidence |
| `SBSQL.LANG_RESOURCE.REMOVAL_NOTICE_MISSING` | Removed resources require removal notice evidence |
| `SBSQL.LANG_RESOURCE.CYCLIC_FALLBACK_PARENT` | Fallback parent points to the same profile |
| `SBSQL.LANG_RESOURCE.RENDERER_RECURSION` | Renderer edge points to the same profile |
| `SBSQL.LANG_RESOURCE.DUPLICATE_CANONICAL_ID` | Canonical IDs must be unique |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_LIMIT_EXCEEDED` | Predictive resources exceed release safety limits |
| `SBSQL.LANG_RESOURCE.PROVENANCE_MISSING` | Release resources require provenance rows |
| `SBSQL.LANG_RESOURCE.PROVENANCE_INCOMPLETE` | Provenance rows require all required fields |
| `SBSQL.LANG_RESOURCE.REDISTRIBUTION_NOT_ALLOWED` | Release resource data must be redistributable |
| `SBSQL.LANG_RESOURCE.RECOVERY_PROFILE_REPLACEABLE` | Built-in recovery profile cannot be externally replaceable |
| `SBSQL.LANG_RESOURCE.EXPERIMENTAL_SUPPORT_STATE_MISMATCH` | Experimental resources must not claim reviewed or release support state |
| `SBSQL.LANG_RESOURCE.PREVIEW_REVIEW_REQUIRED` | Preview resources require native review |
| `SBSQL.LANG_RESOURCE.BETA_REVIEW_REQUIRED` | Beta resources require native review |
| `SBSQL.LANG_RESOURCE.DEPRECATED_SUPPORT_STATE_MISMATCH` | Deprecated resources must retain release-supported support state |
| `SBSQL.LANG_RESOURCE.DEPRECATED_GOVERNANCE_EVIDENCE_MISSING` | Deprecated resources require release governance evidence |

## Predictive Resource Validation Diagnostics

| Code | Meaning |
|------|---------|
| `SBSQL.LANG_RESOURCE.PREDICTIVE_RESOURCE_IDENTITY_MISSING` | Predictive resource identity is required |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_TABLE_SIZE_LIMIT` | Predictive table exceeds 8 MB limit |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_FANOUT_LIMIT` | Transition fanout exceeds 1024 limit |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_COMPLETION_LIMIT` | Completion result count exceeds 4096 limit |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_TIME_LIMIT` | Generation time exceeds 1000 ms limit |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_MEMORY_LIMIT` | Predictive memory exceeds 16 MB limit |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_DEPTH_LIMIT` | Nested expansion depth exceeds 64 limit |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_DETERMINISM_REQUIRED` | Limits must be enforced deterministically |
| `SBSQL.LANG_RESOURCE.PREDICTIVE_NO_DISCLOSURE_REQUIRED` | Completions must not disclose hidden objects |

## Bundle Admission Diagnostics

These codes are on the `SBSQL.LANG_BUNDLE.*` prefix and relate to bundle
(pack-level) rather than profile-level validation.

| Code | Meaning |
|------|---------|
| `SBSQL.LANG_BUNDLE.UNSIGNED` | Bundle must be signed unless it is a parser language library |
| `SBSQL.LANG_BUNDLE.REVOKED` | Revoked bundle fails closed |
| `SBSQL.LANG_BUNDLE.EXPIRED` | Expired bundle fails closed |
| `SBSQL.LANG_BUNDLE.REMOVED` | Removed bundle fails closed |
| `SBSQL.LANG_BUNDLE.ROLLED_BACK` | Rolled-back bundle admitted with warning |
| `SBSQL.LANG_BUNDLE.INCOMPATIBLE` | Bundle is not compatible with this parser version |
| `SBSQL.LANG_BUNDLE.SECURITY_ADMISSION_REQUIRED` | Bundle requires security admission before use |
| `SBSQL.LANG_BUNDLE.ACTIVE_PROFILE_IN_USE` | Active language profiles cannot be unloaded |
| `SBSQL.LANG_BUNDLE.REQUIRED_PROFILE` | Required language profiles cannot be unloaded |

## Canonical Stream Diagnostics

These codes relate to canonical element stream validation.

| Code | Meaning |
|------|---------|
| `SBSQL.CANONICAL_STREAM.RESOURCE_IDENTITY_MISSING` | Stream requires a resource identity |
| `SBSQL.CANONICAL_STREAM.LANGUAGE_PROFILE_MISSING` | Stream requires a language profile UUID |
| `SBSQL.CANONICAL_STREAM.EXACT_TAG_MISSING` | Stream requires an exact language tag |
| `SBSQL.CANONICAL_STREAM.DIALECT_PROFILE_MISSING` | Stream requires a dialect profile UUID |
| `SBSQL.CANONICAL_STREAM.TOPOLOGY_PROFILE_MISSING` | Stream requires a topology profile UUID |
| `SBSQL.CANONICAL_STREAM.COMMON_HASH_MISSING` | Stream requires the common resource pack hash |
| `SBSQL.CANONICAL_STREAM.SOURCE_HASH_MISSING` | Stream requires the localized source hash |
| `SBSQL.CANONICAL_STREAM.CANONICAL_ORDER_MISSING` | Stream requires a canonical order identifier |
| `SBSQL.CANONICAL_STREAM.POST_UUID_NORMALIZATION` | Topology normalization must occur before UUID resolution |
| `SBSQL.CANONICAL_STREAM.SERVER_REVALIDATION_REQUIRED` | Stream remains untrusted until server revalidation |
| `SBSQL.CANONICAL_STREAM.EMPTY` | Stream must contain at least one element |
| `SBSQL.CANONICAL_STREAM.ELEMENT_CANONICAL_TEXT_MISSING` | Each element requires canonical text |
| `SBSQL.CANONICAL_STREAM.ELEMENT_CANONICAL_ID_MISSING` | Each element requires a canonical ID |
| `SBSQL.CANONICAL_STREAM.ELEMENT_SOURCE_HASH_MISSING` | Each element must retain a localized source hash |
| `SBSQL.CANONICAL_STREAM.ELEMENT_SOURCE_SPAN_MISSING` | Each element must retain its localized source span |

## Parse Profile Validation

| Code | Meaning |
|------|---------|
| `SBSQL.PARSE_PROFILE.ORDER_UNSUPPORTED` | Parse profile order must be exactly: explicit profile, preferred language, canonical English fallback, fail closed |

## Editor Protocol Validation

| Code | Meaning |
|------|---------|
| `SBSQL.EDITOR_PROTOCOL.VERSION_UNSUPPORTED` | Unsupported editor protocol version (must be `sbsql.editor_tool.v1`) |
| `SBSQL.EDITOR_PROTOCOL.RESOURCE_IDENTITY_MISSING` | Resource identity is required |
| `SBSQL.EDITOR_PROTOCOL.REQUIRED_FEATURE_MISSING` | Common editor tool protocol must expose all required surfaces |
| `SBSQL.EDITOR_PROTOCOL.PARSE_PROFILE_ORDER_UNSUPPORTED` | Protocol must declare deterministic parse profile order |
| `SBSQL.EDITOR_PROTOCOL.RENDERER_LOSSINESS_CLASSES_MISSING` | Protocol must declare all renderer lossiness classes |
| `SBSQL.EDITOR_PROTOCOL.FALLBACK_DIAGNOSTICS_MISSING` | Protocol must declare canonical English fallback diagnostics |
| `SBSQL.EDITOR_PROTOCOL.RENDERING_DIAGNOSTICS_MISSING` | Protocol must declare renderer classification diagnostics |
| `SBSQL.EDITOR_PROTOCOL.AUTHORITY_METADATA_MISSING` | Protocol must fail closed and keep server revalidation authority |
| `SBSQL.EDITOR_PROTOCOL.AUTHORITY_BOUNDARY_UNSUPPORTED` | Protocol must preserve client-resource authority boundary |

## Cross-References

- Rendering diagnostic context: [rendering_and_fallback.md](rendering_and_fallback.md)
- Server parser validation context: [server_parser_language_support.md](server_parser_language_support.md)
- Client authority boundary: [client_and_editor_language_surface.md](client_and_editor_language_surface.md)
- Authority model: [overview_and_authority_model.md](overview_and_authority_model.md)

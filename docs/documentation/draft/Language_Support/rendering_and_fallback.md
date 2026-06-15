# Rendering and Fallback

## Purpose

This page describes how ScratchBird renders SBLR back to SBsql text for
display, what lossiness classes the renderer assigns, why rendered output is
canonical rather than a source reconstruction, and how the English fallback
mechanism works when the preferred-language renderer is unavailable.

## Renderer Templates

Rendering templates are carried in the language resource pack under
`resources/rendering/rendering-templates.json`. Each renderer entry in a
language element manifest requires a `renderer_id`, a `profile_uuid`, and a
`canonical_english_fallback_profile_uuid`. Every renderer must carry
`server_revalidation_required: true`; a renderer without server revalidation is
rejected with `SBSQL.LANG_ELEMENT_MANIFEST.RENDERER_REVALIDATION_REQUIRED`.

The rendering subsystem in `rendering.cpp` produces a diagnostic record for
each rendering decision. The diagnostic includes `render_decision`,
`renderer_lossiness`, `selected_profile`, `fallback_profile`,
`canonical_english_fallback`, and `server_revalidation_required` fields.

## Renderer Output Is Canonical, Not Source Reconstruction

A fundamental invariant stated in `common_resource_contract` is:

> `renderer_output_is_canonical_not_source_reconstruction: true`

When the renderer produces SBsql from SBLR, it outputs a canonical form of the
statement — keyword casing, structure, and whitespace are governed by the
renderer templates. It does not attempt to reproduce the original localized text
the user typed. The original source is not retained in SBLR; only a hash of it
is carried in the canonical element stream for diagnostic purposes.

This is a deliberate security and authority boundary. The original text may have
contained confusable characters, mixed scripts, or unusual encodings that were
sanitized during canonicalization. Reconstructing original source from SBLR
would bypass those security checks.

Source reconstruction is actively refused. If a render request sets
`source_reconstruction_requested: true`, the render decision is
`kRefuseSourceReconstruction` and the diagnostic
`SBSQL.LANG_RESOURCE.RENDERER_SOURCE_RECONSTRUCTION_FORBIDDEN` is emitted.

## Renderer Lossiness Classes

Every render operation produces a lossiness classification. The five lossiness
classes are defined in the `common_resource_pack_metadata.renderer_lossiness_classes`
field of the language surface manifest and in the
`ExpectedRendererLossinessClasses` function in `language_resource_contract.cpp`.

| Lossiness Class | Code Name | Meaning |
|----------------|-----------|---------|
| Lossless canonical | `lossless_canonical` | The preferred renderer is the canonical English profile itself. Output is identical to canonical form. |
| Canonical equivalent | `canonical_equivalent` | The preferred-language renderer covers the full statement. Output is semantically equivalent to canonical. |
| Preferred language partial | `preferred_language_partial` | The preferred-language renderer covers only part of the statement. Some elements fall back to canonical form. |
| Canonical English fallback | `canonical_english_fallback` | The preferred-language renderer was unavailable; canonical English renderer was used. |
| Not renderable | `not_renderable` | No admitted renderer can produce output. The statement cannot be rendered. |

The lossiness class is always emitted with the diagnostic
`SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED`.

## Render Decision Logic

The `ClassifySblrRenderRequest` function in `language_resource_contract.cpp`
evaluates render requests in this order:

| Condition checked | Outcome |
|------------------|---------|
| `sblr_uuid_authority_valid` is false | `kRefuseMissingCanonicalAuthority` — diagnostic `SBSQL.LANG_RESOURCE.MISSING_CANONICAL_AUTHORITY` |
| `source_reconstruction_requested` is true | `kRefuseSourceReconstruction` — diagnostic `SBSQL.LANG_RESOURCE.RENDERER_SOURCE_RECONSTRUCTION_FORBIDDEN` |
| `resource_revoked` is true | `kRefuseRevokedResource` — diagnostic `SBSQL.LANG_RESOURCE.REVOKED` |
| `resource_incompatible` is true | `kRefuseIncompatibleResource` — diagnostic `SBSQL.LANG_RESOURCE.INCOMPATIBLE` |
| `preferred_renderer_available` is true | `kPreferredLanguage` — use preferred renderer |
| `canonical_english_renderer_available` is true | `kCanonicalEnglishFallback` — use English fallback |
| None of the above | `kRefuseRendererUnavailable` — diagnostic `SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE` |

## English Fallback Behavior

When the preferred-language renderer is unavailable (step `kCanonicalEnglishFallback`
above), the system selects the canonical English renderer. The diagnostic
`SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH` is emitted with severity
`warning`. The render output carries `canonical_english_fallback: true` in its
diagnostic record.

The `common_resource_contract` invariant
`standard_english_fallback_preserves_preferred_language: true` means that using
the English renderer does not change the session's preferred language setting.
The session remains configured for the preferred locale; only the render output
for this particular SBLR is produced in English.

Fallback is distinct from fail-closed. Fallback occurs when the preferred
renderer is simply unavailable for rendering — the parse succeeded in the
preferred language, SBLR was produced, but rendering back to text must use
English because the preferred-language renderer template is not available.
Fail-closed occurs when no parse succeeds at all.

## Restore and Renderer Fallback

When a session restores SBLR from a prior operation (`ClassifyRestoreLanguageResourceState`
in `language_resource_contract.cpp`), the restoration decision also handles
renderer availability:

| State | Meaning |
|-------|---------|
| `exact_resource_available` | The exact resource pack and preferred renderer are available |
| `canonical_authority_valid_renderer_fallback` | UUID authority is valid but the exact renderer is unavailable; canonical English renderer will be used |
| `refuse_revoked_resource` | Refused — resource is revoked |
| `refuse_missing_canonical_authority` | Refused — SBLR UUID authority cannot be validated |
| `refuse_incompatible_resource` | Refused — resource is incompatible with current parser |

## Fallback and Rendering Diagnostics

The two categories of language diagnostics emitted from the rendering layer are
listed in `common_resource_pack_metadata` in the language surface manifest.

**Fallback diagnostics** (session-level):

| Code | When emitted |
|------|-------------|
| `SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH` | Preferred-language renderer unavailable; canonical English selected |
| `SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH` | No parse succeeded; operation refused |

**Rendering diagnostics** (per render operation):

| Code | When emitted |
|------|-------------|
| `SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED` | Always when a render decision is made (records which lossiness class applies) |
| `SBSQL.LANG_RESOURCE.RENDERER_SOURCE_RECONSTRUCTION_FORBIDDEN` | When `source_reconstruction_requested` is true |
| `SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE` | When no renderer is available |

For the full diagnostic reference, see [diagnostics_reference.md](diagnostics_reference.md).

## Cross-References

- Lossiness classes used in client capability declarations: [client_and_editor_language_surface.md](client_and_editor_language_surface.md)
- Authority model explaining why source reconstruction is forbidden: [overview_and_authority_model.md](overview_and_authority_model.md)
- Diagnostics table: [diagnostics_reference.md](diagnostics_reference.md)

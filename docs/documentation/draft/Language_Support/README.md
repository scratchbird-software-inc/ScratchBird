# ScratchBird Language Support Manual

## Purpose

This manual is the single authority on how ScratchBird implements localized and
multilingual SBsql. It covers both sides of the language surface: the
server-side parser that admits, validates, and normalizes localized SBsql
source; and the client and editor side that provides completion, canonical
preview, local draft SBLR, and localized diagnostics.

Other manuals — the Client and Driver Guide, the Operations and Administration
Guide, the Language Reference — refer to this manual rather than duplicating
its content.

## What Language Support Is

SBsql is the query language of ScratchBird, a Convergent Data Engine. Language
support means that the _syntax_ a user writes can be expressed in a supported
natural language (locale) while the _work_ the engine performs is
locale-independent. A French-speaking user may write SBsql using French
keywords; a German-speaking user may use German keywords. The engine executes
the same canonical representation regardless of the locale used to express it.

This separation is enforced by signed i18n resource packs that carry keyword
spellings, phrase structures, predictive completion tables, renderer templates,
and localized diagnostic messages for each supported locale. The pack is the
only trusted surface through which localization reaches the engine.

## The Two Sides

Language support spans two domains.

**Server parser side.** The parser worker (`sbsql_worker`) loads and validates
signed language resource packs. It performs keyword aliasing, topology-slot
phrase ordering, canonical element stream production, and security checks
(confusable, bidi, mixed-script, mirrored-punctuation). The server owns UUID
identity, descriptor authority, security policy, and MGA transaction finality.
It revalidates any client-produced canonical stream or local draft SBLR before
admitting it.

**Client and editor side.** Drivers and adaptors carry a common resource pack
to support local parsing, predictive completion, SBLR-to-SBsql rendering, and
localized diagnostics in the client process. These resources are explicitly
untrusted until the server revalidates them. Capability negotiation requires
exact resource identity match. Each driver and adaptor is listed by
`component_id` in the language surface manifest with its specific capability
posture.

## The Untrusted Client Resource Boundary

The core invariant, stated in the `common_resource_contract` field of the
language surface manifest, is:

- Client language resources are untrusted until server revalidation.
- Renderer output is canonical, not a reconstruction of the original source text.
- The server owns UUID, descriptor, security, and MGA authority.
- English fallback preserves the preferred language setting; it does not
  silently switch the session locale.
- The system fails closed on profile mismatch — a resource that cannot be
  validated is refused, not silently downgraded.

## Supported Locale Profiles

The following seven exact-tag locale profiles are listed in the i18n resource
pack manifest (`manifest.sblrp.json`) and in the language surface manifest
(`common_resource_pack_metadata.supported_exact_profiles`). Support state as
found in source is shown.

| Exact Tag | Profile UUID | Release Channel | Support State |
|-----------|-------------|-----------------|---------------|
| `en-US` | `sbsql.language.en-US.canonical-recovery.v1` | `release_supported` | `release_supported` |
| `en-CA` | `sbsql.language.en-CA.canonical-recovery.v1` | `release_supported` | `release_supported` |
| `fr-FR` | `sbsql.language.fr-FR.machine-beta.v1` | `beta` | `fully_populated_native_review_required` |
| `fr-CA` | `sbsql.language.fr-CA.machine-beta.v1` | `beta` | `fully_populated_native_review_required` |
| `de-DE` | `sbsql.language.de-DE.machine-beta.v1` | `beta` | `fully_populated_native_review_required` |
| `it-IT` | `sbsql.language.it-IT.machine-beta.v1` | `beta` | `fully_populated_native_review_required` |
| `es-ES` | `sbsql.language.es-ES.machine-beta.v1` | `beta` | `fully_populated_native_review_required` |

The built-in canonical English recovery profile (`sbsql.builtin.recovery.en`,
exact tag `en`) is always available in the parser as a fail-safe fallback. It
is not externally replaceable.

The `fr-FR`, `fr-CA`, `de-DE`, `it-IT`, and `es-ES` profiles carry
`native_review_state: native_technical_review_required_before_release_support`.
They are machine-generated beta resources and are not release-supported. Do not
use them in production deployments that require release-support guarantees.

## Directory Map

| Page | Contents |
|------|----------|
| [overview_and_authority_model.md](overview_and_authority_model.md) | End-to-end model: localized surface to canonical stream to UUID to SBLR; common_resource_contract invariants |
| [locale_profiles_and_resource_packs.md](locale_profiles_and_resource_packs.md) | Exact-tag locale profiles, i18n resource pack structure, signing, admission |
| [server_parser_language_support.md](server_parser_language_support.md) | Engine-side keyword aliasing, topology slots, predictive limits, canonical element stream, security checks |
| [rendering_and_fallback.md](rendering_and_fallback.md) | Renderer templates, lossiness classes, canonical-not-source-reconstruction rule, English fallback |
| [client_and_editor_language_surface.md](client_and_editor_language_surface.md) | Driver and adaptor language surface manifest, capability negotiation, editor tool protocol |
| [diagnostics_reference.md](diagnostics_reference.md) | Consolidated table of SBSQL.LANG_RESOURCE.* diagnostic codes with guidance |

## Reading Model

If you are a developer integrating a driver or adaptor, start with
[client_and_editor_language_surface.md](client_and_editor_language_surface.md),
then read [rendering_and_fallback.md](rendering_and_fallback.md) for lossiness
classification and diagnostic handling.

If you are an operator installing or updating resource packs, start with
[locale_profiles_and_resource_packs.md](locale_profiles_and_resource_packs.md),
then cross-reference the Operations and Administration Guide for the resource
pack install layout.

If you are auditing parser-side security, start with
[server_parser_language_support.md](server_parser_language_support.md) and
[overview_and_authority_model.md](overview_and_authority_model.md).

## Cross-References

- Engine-side language profile concepts: [../Language_Reference/core_paradigms/sbsql_language_profiles.md](../Language_Reference/core_paradigms/sbsql_language_profiles.md)
- Driver and adaptor integration: [../Client_Driver_Guide/README.md](../Client_Driver_Guide/README.md)
- Resource pack installation layout: [../Operations_Administration/installation_and_output_layout.md](../Operations_Administration/installation_and_output_layout.md)
- Parser registration: [../Operations_Administration/parser_registration_and_routes.md](../Operations_Administration/parser_registration_and_routes.md)

## Draft Status

This is a draft manual. All technical claims have been verified against the
source tree at `project/src/parsers/sbsql_worker/resources/language_resource_contract.cpp`,
`project/drivers/language/sbsql_language_surface_manifest.json`,
`project/drivers/language/sbsql_editor_tool_protocol.schema.json`, and
`project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack/manifest.sblrp.json`.
Claims that could not be verified from source have been omitted.

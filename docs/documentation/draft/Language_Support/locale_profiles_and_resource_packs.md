# Locale Profiles and Resource Packs

## Purpose

This page describes the exact-tag locale profiles supported by ScratchBird, the
structure of the i18n resource pack that carries those profiles, and the
signing, admission, and lifecycle model that governs when a pack may be used.

## Exact-Tag Locale Profiles

ScratchBird identifies a language profile by an _exact tag_: a BCP-47 locale
identifier that includes both language and region (for example `fr-CA`, not
`fr`). Using exact tags rather than partial tags prevents ambiguous fallback
chains and allows the parser to enforce deterministic resource identity.

The seven profiles listed in both the language surface manifest
(`common_resource_pack_metadata.supported_exact_profiles`) and the i18n
resource pack manifest (`profiles` array) are:

| Exact Tag | Profile UUID | Release Channel | Native Review State | Support State |
|-----------|-------------|-----------------|---------------------|---------------|
| `en-US` | `sbsql.language.en-US.canonical-recovery.v1` | `release_supported` | `source_authority_reviewed` | `release_supported` |
| `en-CA` | `sbsql.language.en-CA.canonical-recovery.v1` | `release_supported` | `source_authority_reviewed` | `release_supported` |
| `fr-FR` | `sbsql.language.fr-FR.machine-beta.v1` | `beta` | `native_technical_review_required_before_release_support` | `fully_populated_native_review_required` |
| `fr-CA` | `sbsql.language.fr-CA.machine-beta.v1` | `beta` | `native_technical_review_required_before_release_support` | `fully_populated_native_review_required` |
| `de-DE` | `sbsql.language.de-DE.machine-beta.v1` | `beta` | `native_technical_review_required_before_release_support` | `fully_populated_native_review_required` |
| `it-IT` | `sbsql.language.it-IT.machine-beta.v1` | `beta` | `fully_populated_native_review_required` | `fully_populated_native_review_required` |
| `es-ES` | `sbsql.language.es-ES.machine-beta.v1` | `beta` | `native_technical_review_required_before_release_support` | `fully_populated_native_review_required` |

In addition, the built-in canonical English recovery profile is always present
in the parser:

| Exact Tag | Profile UUID | Channel | Notes |
|-----------|-------------|---------|-------|
| `en` | `sbsql.builtin.recovery.en` | `release_supported` | Built-in, not externally replaceable, used as fallback only |

The `en` built-in profile is the English fallback used when the
preferred-language parse fails. It is built into the parser binary and cannot
be overridden by an external resource pack.

### Beta Profile Caution

The `fr-FR`, `fr-CA`, `de-DE`, `it-IT`, and `es-ES` profiles are in `beta`
channel and carry the support state `fully_populated_native_review_required`.
They are machine-generated and have not completed native technical review. They
are admitted as limited-support resources and emit a lifecycle warning diagnostic
`SBSQL.LANG_RESOURCE.BETA_LIMITED_SUPPORT` when loaded. Do not use them in
production deployments that require release-support guarantees.

## The i18n Resource Pack

The i18n resource pack is located at:

```
project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack/
```

It contains three top-level files:

| File | Purpose |
|------|---------|
| `manifest.sblrp.json` | Pack manifest: schema version, resource identity, profiles array, file list with per-file SHA-256 hashes, generation metadata |
| `hashes.sha256` | External hash file for pack integrity verification |
| `manifest.sblrp.sig` | Pack signature file |

### manifest.sblrp.json Structure

The manifest's top-level fields include:

| Field | Value (from source) |
|-------|---------------------|
| `schema_version` | `sbsql.language_resource_pack_manifest.v1` |
| `pack_schema_version` | `sbsql.language_resource_pack.v1` |
| `resource_identity` | `sbsql.common_resource_pack.v1` |
| `common_resource_hash` | `sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc` |
| `dialect_profile_uuid` | `sbsql.v3` |
| `topology_profile_uuid` | `topology.sbsql.canonical.v1` |
| `registry_row_count` | `2645` |
| `translation_source_row_count` | `2721` |
| `generated_by` | `project/tools/sb_parser_gen/generate_sbsql_language_resource_pack.py` |

The `files` array contains one entry per resource file with a relative `path`
and a `sha256` hash. The `profiles` array contains one entry per locale with
`exact_tag`, `profile_uuid`, `release_channel`, `support_state`,
`native_review_state`, and `resource_path`.

### Resource Subdirectory Layout

The `resources/` directory inside the pack is organized into functional
subdirectories:

| Subdirectory | Contents |
|-------------|---------|
| `canonical/` | Dialect baseline, system object name registry, translation source corpus, style guide |
| `conformance/` | Conformance corpus, online translation verification corpus |
| `diagnostics/` | Database message catalog, diagnostic messages |
| `dialects/` | Per-dialect profile (e.g., `sbsql-v3-dialect-profile.json`) |
| `languages/` | Per-locale language profile (`en-US/language-profile.json`, `fr-FR/language-profile.json`, etc.) |
| `phrases/` | Phrase table |
| `predictive/` | Predictive grammar |
| `provenance/` | Native review status, provenance records |
| `rendering/` | Rendering templates |
| `resolver/` | Resolver policy |
| `topology/` | Topology profiles |
| `unicode/` | Unicode policy |

## Signing and Admission

Every language resource pack must be signed before it can be admitted by the
server. The validation layer in `language_resource_contract.cpp` enforces this:

- A bundle missing `signature_id` produces `SBSQL.LANG_RESOURCE.UNSIGNED` and
  fails closed.
- A bundle without `signing_key_id` produces `SBSQL.LANG_RESOURCE.SIGNING_KEY_MISSING`.
- An unsigned bundle (where `signed_bundle` is false and it is not a parser
  language library) produces `SBSQL.LANG_BUNDLE.UNSIGNED`.

Release-supported and deprecated packs additionally require full governance
evidence: `author_id`, `reviewer_id`, `native_technical_reviewer_id`,
`security_reviewer_id`, `support_owner_id`, `release_approval_id`,
`revocation_policy_id`, `contribution_provenance_id`, and
`governance_evidence_id` must all be present. Missing any of these produces a
specific `SBSQL.LANG_RESOURCE.*_MISSING` error.

## Lifecycle States

A language resource bundle progresses through lifecycle states. The known
states, as validated by `IsKnownLanguagePackLifecycleState` in source, are:

`staged` → `generated` → `signed` → `published` → `admitted` → `downloaded`
→ `cached` → `delta_updated` → `rolled_back` → `revoked` → `expired`
→ `removed`

A bundle in `revoked`, `expired`, or `removed` state fails closed at load time.
A bundle in `rolled_back` state is admitted with a warning diagnostic
`SBSQL.LANG_BUNDLE.ROLLED_BACK`.

## Provenance Requirements

Every release-supported or deprecated resource requires provenance rows. Each
provenance row must carry `source_name`, `source_version`, `license_id`,
`transformation_id`, `sbom_component_id`, and `third_party_notice_id`. The
`redistribution_allowed` flag must be `true`. Missing or incomplete provenance
produces `SBSQL.LANG_RESOURCE.PROVENANCE_MISSING` or
`SBSQL.LANG_RESOURCE.PROVENANCE_INCOMPLETE`.

## Predictive Resource Limits

Language resource packs may carry predictive (completion) tables. Release
safety limits enforced by the validation layer are:

| Limit | Maximum |
|-------|---------|
| Predictive table size | 8 MB |
| Transition fanout | 1024 |
| Completion results | 4096 |
| Generation time | 1000 ms |
| Predictive memory | 16 MB |
| Nested expansion depth | 64 |

Exceeding any of these limits produces `SBSQL.LANG_RESOURCE.PREDICTIVE_LIMIT_EXCEEDED`.

## Installation Layout

For the operator-facing resource pack installation layout and how packs are
loaded into a running deployment, see:
[../Operations_Administration/installation_and_output_layout.md](../Operations_Administration/installation_and_output_layout.md)

and

[../Operations_Administration/parser_registration_and_routes.md](../Operations_Administration/parser_registration_and_routes.md)

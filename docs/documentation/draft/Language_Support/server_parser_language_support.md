# Server Parser Language Support

## Purpose

This page describes how the ScratchBird server-side parser worker handles
localized SBsql: how it aliases localized keywords to canonical forms, how
topology slots enforce phrase ordering, how the canonical element stream is
produced, what limits apply to predictive resources, and what security checks
are applied to localized identifiers.

For the conceptual overview of language profiles, see the Language Reference
page: [../Language_Reference/core_paradigms/sbsql_language_profiles.md](../Language_Reference/core_paradigms/sbsql_language_profiles.md).

## Keyword and Phrase Aliasing

The parser resolves localized tokens against a keyword alias table loaded from
the active language resource pack. Each entry in the alias table associates a
localized text form with a `canonical_id` and a `surface_id`.

From `cst.cpp`, the alias resolution logic:

- The alias table is searched for a case-insensitive ASCII match against the
  raw token text.
- If an alias is found, the canonical text is taken from `alias.canonical_text`
  and uppercased as ASCII.
- If no alias matches but the token carries a `canonical_text` value directly,
  that value is used.
- If neither match occurs, the token text itself is uppercased and used as the
  canonical form.

This means that a French keyword like `SÉLECTIONNER` (if present in the
language profile) maps to the same canonical ID as the English keyword
`SELECT`. The engine never sees the original localized token after the canonical
element stream is produced.

## Topology Slots and Phrase Ordering

Language profiles can define alternative phrase orderings. For example, some
languages place modifiers after the verb while English places them before.
Topology slots define how phrase components are sequenced.

Each topology slot requires:

- `slot_id` — unique slot identifier
- `phrase_id` — the phrase this slot belongs to
- `topology_role` — the role this slot plays (e.g., head, modifier)
- `canonical_id` — the canonical token or surface this slot resolves to
- `surface_id` — the surface declaration this slot references
- `min_elements` and `max_elements` — cardinality constraints (min must be > 0)

Topology normalization must occur **before** UUID resolution. This is enforced
by the `normalized_before_uuid_resolution` flag on the canonical element stream.
If a stream arrives without this flag set, the validation layer produces
`SBSQL.CANONICAL_STREAM.POST_UUID_NORMALIZATION` and refuses the stream.

## The Canonical Element Stream

The canonical element stream is the internal representation passed from the
parser to the UUID resolution and SBLR production stages. Its structure, as
validated by `ValidateCanonicalElementStream` in `language_resource_contract.cpp`,
requires:

| Field | Requirement |
|-------|-------------|
| `resource_identity` | Must be present — identifies which common resource pack produced this stream |
| `language_profile_uuid` | Must be present — identifies the locale profile used |
| `exact_tag` | Must be present — the exact locale tag |
| `dialect_profile_uuid` | Must be present |
| `topology_profile_uuid` | Must be present |
| `common_resource_hash` | Must be present — hash of the resource pack used |
| `source_hash` | Must be present — hash of the original localized source text |
| `canonical_order_id` | Must be present — identifies the canonical ordering rule applied |
| `normalized_before_uuid_resolution` | Must be `true` |
| `server_revalidation_required` | Must be `true` |
| `elements` | Must be non-empty |

Each element in the stream requires:

| Field | Requirement |
|-------|-------------|
| `canonical_text` | Canonical parser text for this token or phrase element |
| `canonical_id` | Canonical token or surface identifier |
| `localized_text_hash` | Hash of the original localized text (retained for diagnostics) |
| `source_span.length` | Must be > 0 — the span in the localized source |

The localized text hash is retained so that support bundles and diagnostics can
reference original source position without re-exposing the original text. The
canonical stream itself does not contain the original localized text.

## The `canonical_english_fallback_used` Flag

The CST document carries a `canonical_english_fallback_used` flag (set in
`cst.cpp`). When `true`, it indicates that the preferred-language parse failed
and canonical English was used instead. This flag is surfaced in diagnostics and
determines which `SBSQL.LANG_RESOURCE.*` diagnostic code is emitted.

## The `input_language_fallback_tag` Wire Field

The SBWP wire protocol (`sbsql_sbwp_wire.cpp`) carries an
`input_language_fallback_tag` field on the session state. When present, this
tag is included in the message vector so that diagnostics can report the
fallback locale applied during the session. The field is redacted in telemetry
and support bundle exports consistent with the no-disclosure contract.

## Parse Profile Order

The server applies language profile resolution in a fixed four-step order. No
other order is accepted. The `ValidateParseProfileOrder` function enforces this
at resource admission time.

| Step | Name | Behavior |
|------|------|---------|
| 1 | `explicit_syntax_profile` | Use a profile explicitly declared in the session or statement |
| 2 | `preferred_language_and_dialect` | Use the session's preferred locale resource pack |
| 3 | `canonical_english_fallback_when_preferred_fails` | Fall back to canonical English if step 2 fails; emit `SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH` |
| 4 | `fail_closed` | Refuse the operation; emit `SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH` |

## Predictive Resources and Limits

Language resource packs may carry predictive tables for completion. These tables
are subject to release safety limits. The limits are enforced by
`ValidatePredictiveTextResourceFootprint` and `ValidateLanguageResourceManifest`
in `language_resource_contract.cpp`:

| Resource dimension | Release safety limit |
|--------------------|---------------------|
| Table size | 8 MB (`max_predictive_table_bytes`) |
| Transition fanout | 1024 (`max_transition_fanout`) |
| Completion results | 4096 (`max_completion_results`) |
| Generation time | 1000 ms (`max_generation_millis`) |
| Memory use | 16 MB (`max_predictive_memory_bytes`) |
| Nested expansion depth | 64 (`max_nested_expansion_depth`) |

Predictive resources must enforce limits deterministically
(`deterministic_limit_enforcement` must be `true`). Completion results must
not disclose hidden or inaccessible objects
(`hidden_object_no_disclosure` must be `true`).

Every predictive state in a language element manifest must carry
`server_revalidation_required: true`. A predictive state without server
revalidation is rejected with
`SBSQL.LANG_ELEMENT_MANIFEST.PREDICTIVE_REVALIDATION_REQUIRED`.

## Security Checks

The parser applies several security checks to localized identifier and literal
text. These checks are implemented in `language_resource_contract.cpp`.

### Confusable and Mixed-Script Identifiers

The function `HasMixedScriptOrConfusableRisk` inspects identifier text for
security risks:

- **Bidi control characters** (Unicode code points U+202A–U+202E and
  U+2066–U+2069) are refused unless the confusable policy explicitly permits
  them (`allow_bidi_controls: true`).
- **Mirrored punctuation** (U+061B, U+061F, U+FD3E, U+FD3F) is refused unless
  the policy permits it (`allow_mirrored_punctuation: true`).
- **Mixed-script identifiers** — identifiers that contain characters from more
  than one non-ASCII script class — are refused unless
  `allow_mixed_script_identifiers: true`.
- **Transliteration aliases** using Greek or Cyrillic characters are refused
  unless `allow_transliteration_aliases: true`.

Script classes recognized are: ASCII, Latin, Greek, Cyrillic, Arabic, Hebrew,
and Other (any other non-ASCII character).

### Locale Literal Classification

The function `ClassifyLocaleLiteral` classifies localized literals for
admission:

- Localized digit forms (Arabic-Indic U+0660–U+0669, Extended Arabic-Indic
  U+06F0–U+06F9, Devanagari U+0966–U+096F) require an explicit locale policy
  that admits them; without one the literal produces
  `LocaleLiteralClassification::kRequiresExplicitProfile`.
- Decimal comma shape (digit-comma-digit) requires `admits_decimal_comma: true`.
- Localized month names (French month names are checked) require
  `admits_localized_month_names: true`.
- Bidi control characters in a literal cause `kRefuseAmbiguous`.
- Mirrored punctuation in a literal causes `kRefuseAmbiguous`.

### Resource Pack Admission Security

At pack admission time (`AdmitLanguageResourceBundleOperation`), the bundle
must pass security policy admission before it can be loaded:

- `admitted_by_security_policy` must be `true`.
- The bundle must be compatible with the current parser version
  (`compatible_with_parser` must be `true`).

A bundle that fails either check is refused at load time with
`SBSQL.LANG_BUNDLE.SECURITY_ADMISSION_REQUIRED` or
`SBSQL.LANG_BUNDLE.INCOMPATIBLE`.

## Cross-References

- Canonical element stream in the context of rendering: [rendering_and_fallback.md](rendering_and_fallback.md)
- Locale profiles and resource pack signing: [locale_profiles_and_resource_packs.md](locale_profiles_and_resource_packs.md)
- Diagnostic codes: [diagnostics_reference.md](diagnostics_reference.md)
- Language profile concepts: [../Language_Reference/core_paradigms/sbsql_language_profiles.md](../Language_Reference/core_paradigms/sbsql_language_profiles.md)

# Overview and Authority Model

## Purpose

This page describes the end-to-end model for how localized SBsql becomes
engine-executed SBLR, what the authority boundary between client and server is,
and why localization never changes engine authority over UUID identity,
descriptors, security policy, or MGA transaction finality.

## The Pipeline

The canonical pipeline is:

```
localized SBsql source
  -> canonical element stream
  -> UUID binding
  -> SBLR
  -> engine execution
```

Each stage has a defined trust boundary.

**Localized SBsql source.** The user or application produces SBsql text using
the keyword spellings and phrase structures of their locale (for example,
`fr-CA` or `en-US`). This text is untrusted at the point it arrives at the
parser. The locale is identified by an exact-tag profile (for example `fr-CA`)
associated with the active language resource pack.

**Canonical element stream.** The parser worker normalizes localized tokens
against keyword aliases loaded from the active language resource pack and
produces a canonical element stream. Each element carries its canonical text
and ID, a hash of the original localized text, and a source span. Topology
normalization — phrase ordering via topology slots — must occur before UUID
resolution. This ordering is enforced by the `normalized_before_uuid_resolution`
flag on every canonical element stream.

The canonical element stream requires server revalidation. A stream produced by
a client or a local draft parser is not trusted until the server has admitted it.

**UUID binding.** Once the stream is normalized, identifiers are resolved
against UUID-keyed catalog objects. The server owns UUID and descriptor
authority. No client resource can override this.

**SBLR.** The Standardized Binding Language Representation (SBLR) is the
admitted execution form. It carries UUID identity and is the only form the
engine executes. Localized source text does not persist into SBLR. The
canonical element stream's localized text hash is retained for diagnostics but
does not influence execution.

**Engine execution.** The engine executes admitted SBLR. MGA transaction
finality and security policy enforcement are server-side and are not influenced
by language profiles.

## The common_resource_contract Invariants

The `common_resource_contract` field of
`project/drivers/language/sbsql_language_surface_manifest.json` states the
invariants that every driver, adaptor, and editor tool must uphold:

| Field | Value | Meaning |
|-------|-------|---------|
| `draft_sblr_is_untrusted_until_server_admission` | `true` | A local draft SBLR produced by a client driver is untrusted until the server revalidates and admits it. |
| `predictive_text_must_not_infer_hidden_objects` | `true` | Completion suggestions must not disclose objects the current principal cannot access. |
| `renderer_output_is_canonical_not_source_reconstruction` | `true` | When the renderer produces SBsql from SBLR it outputs a canonical form, not the user's original source text. |
| `server_owns_mga_transaction_finality` | `true` | MGA transaction commit/abort authority rests entirely with the server. |
| `server_owns_uuid_descriptor_security_authority` | `true` | UUID resolution, descriptor validation, and security policy are server-only. |
| `server_revalidates_client_sblr` | `true` | Any SBLR a client produces is revalidated before the server executes it. |
| `standard_english_fallback_preserves_preferred_language` | `true` | When English fallback is used because the preferred-language renderer is unavailable, the session language setting is not changed. |

These invariants are not implementation notes — they are contract terms that
the validation layer in `language_resource_contract.cpp` enforces. A language
resource pack that violates them fails closed at admission time.

## Why Localization Never Changes Engine Authority

Localization operates entirely in the _surface layer_: keyword spelling,
phrase ordering, predictive completion, and diagnostic message language. None
of these surfaces has access to:

- UUID catalog resolution
- Descriptor or schema authority
- Security policy evaluation
- MGA transaction commit/abort

The canonical element stream passes these concerns upward unchanged. A
French-keyword query that references a table named `employees` produces the
same UUID reference as an English-keyword query. The language profile affects
what the user types; it does not affect what the engine does.

The server's enforcement of this separation is not optional. The parse profile
order is fixed at four deterministic steps:

1. `explicit_syntax_profile` — use a profile explicitly declared in the
   session or query
2. `preferred_language_and_dialect` — use the session's preferred language
   resource pack
3. `canonical_english_fallback_when_preferred_fails` — fall back to canonical
   English if the preferred-language parse fails; emit
   `SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH`
4. `fail_closed` — if no parse succeeds, emit
   `SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH` and refuse the
   operation

No other order is accepted. The validation function
`ValidateParseProfileOrder` in `language_resource_contract.cpp` enforces this
at resource admission time.

## Fail-Closed Semantics

The system fails closed, not open, on any language resource failure. A resource
that is missing, unsigned, revoked, expired, or incompatible produces an error
diagnostic and the operation is refused. The failure kinds and their diagnostic
codes are enumerated in [diagnostics_reference.md](diagnostics_reference.md).

Revoked or removed resources are refused at load time, not at use time. An
attempt to load a revoked bundle produces `SBSQL.LANG_BUNDLE.REVOKED`. An
attempt to use a revoked profile produces `SBSQL.LANG_RESOURCE.REVOKED`.

## Cross-References

- Locale profiles and pack structure: [locale_profiles_and_resource_packs.md](locale_profiles_and_resource_packs.md)
- Rendering and fallback behavior: [rendering_and_fallback.md](rendering_and_fallback.md)
- Parser security checks: [server_parser_language_support.md](server_parser_language_support.md)
- Language profile concepts in the Language Reference: [../Language_Reference/core_paradigms/sbsql_language_profiles.md](../Language_Reference/core_paradigms/sbsql_language_profiles.md)

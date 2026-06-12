# Locale Charset Collation Timezone Report

Status: complete
Search key: `FSPE-LOCALE_CHARSET_COLLATION_TIMEZONE_REPORT`
Owning slice: `FSPE-012E`

## Summary

FSPE-012E materialized `sbsql_locale_charset_collation_timezone_gate` as a parser, engine-name, resource-seed, reference temporal wire, and SBLR i18n/time CTest gate.

The gate verifies:

- Parser preserves UTF-8 localized identifiers, quoted identifier flags, localized comments, national strings, binary strings, `COLLATE`, date literals, timestamp-with-offset literals, and interval literals.
- Engine name registry preserves default language fallback, profile-specific unquoted case folding, quoted exact-match behavior, UTF-8 display names, and resource/name authority epochs.
- Initial resource seed pack loads from repo-local resources and exposes charset, charset alias, collation, timezone, transition, and leap-second rows.
- Charset, collation, and timezone aliases resolve from seed-pack authority, including `utf8mb4 -> UTF-8`, `c.utf8 -> C.utf8`, `US/Eastern -> America/New_York`, and `Etc/UTC`.
- Reference temporal wire profiles normalize offset timestamps, use timezone seed authority for named zones, and fail closed when timezone seed authority is absent or a date-only profile includes a timezone.
- SBLR string and temporal runtime preserve charset/collation metadata, enforce collation mismatch refusal, perform case-insensitive text matching when collation requires it, normalize timestamp arithmetic to UTC, and normalize ISO intervals to seconds.

## Gate

| Field | Value |
| --- | --- |
| CTest label | `sbsql_locale_charset_collation_timezone_gate` |
| Test source | `project/tests/sbsql_parser_worker/generated/i18n/sbsql_locale_charset_collation_timezone_gate.cpp` |
| CMake target | `sbsql_locale_charset_collation_timezone_gate` |
| Supporting labels | `sbsql_i18n_time_gate`; `sbsql_parser_worker` |
| Seed pack | `project/resources/seed-packs/initial-resource-pack` |
| Unexpected failures | 0 |

## Boundary

The gate does not make the parser the authority for locale, charset, collation, or timezone semantics. Parser output remains token/render evidence. Engine name registry, resource seed-pack loading, datatype temporal wire validation, and SBLR runtime checks retain the authority boundary.

## Result

Locale, charset, collation, timezone, reference temporal wire, and parser UTF-8 regression behavior now have a runnable validation gate and are included in the full parser-worker CTest label.

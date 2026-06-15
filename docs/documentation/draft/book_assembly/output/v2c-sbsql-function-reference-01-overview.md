

===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/functional_reference/index.md -->

<a id="ch-language-reference-functional-reference-index-md"></a>

# Functional Reference Index

The functional reference documents SBsql built-in operations by package namespace. Each package page gives the operation name, purpose, accepted call forms, parameter rules, return rule, behavior, error handling, example usage, and technical binding information.

These pages describe the SBsql language surface. They do not grant privileges and they do not bypass schema sandboxing, descriptor checks, policy admission, or MGA transaction authority.

Technical identifiers such as SBLR symbols, AST class names, UUIDs, entrypoints, optimizer notes, and conformance markers are diagnostic trace fields. They connect the documented surface to implementation evidence; they are not independent user contracts or release-support claims.

## Packages

| Package | File | Records | Scope |
| --- | --- | ---: | --- |
| `sb.core` | sb_core.md (SBsql Functions — SB Core Functional Reference, page XXX) | 605 | Core scalar, aggregate, window, session, catalog, diagnostic, and language helper surfaces used by ordinary SBsql expressions and procedural SQL. |
| `sb.crypto` | sb_crypto.md (SBsql Functions — SB Crypto Functional Reference, page XXX) | 27 | Cryptographic, hashing, random-value, armor, and bounded encryption helper functions. |
| `sb.cursor` | sb_cursor.md (SBsql Functions — SB Cursor Functional Reference, page XXX) | 15 | Cursor, stream, rowset-handle, and table-value conversion helpers used by procedural SQL and streaming execution. |
| `sb.fn.diagnostic` | sb_diagnostic.md (SBsql Functions — SB Diagnostic Functional Reference, page XXX) | 1 | Statement and procedural diagnostic helpers, including row-count context. |
| `sb.json` | sb_json.md (SBsql Functions — SB JSON Functional Reference, page XXX) | 44 | JSON and JSONB construction, extraction, path, aggregation, and table-shaping helpers. |
| `sb.lob` | sb_lob.md (SBsql Functions — SB LOB Functional Reference, page XXX) | 13 | Large-object and locator helpers for bounded LOB access through engine-managed handles. |
| `sb.operator` | sb_operator.md (SBsql Functions — SB Operator Functional Reference, page XXX) | 24 | Operator functions for arithmetic, comparison, boolean, pattern, JSON, array, and vector-like operator spellings. |
| `sb.scalar` (range) | sb_range.md (SBsql Functions — SB Range Functional Reference, page XXX) | 9 | Range-boundary and containment helpers for descriptor-backed range values; registered under `sb.scalar.range_*`. |
| `sb.regex` | sb_regex.md (SBsql Functions — SB Regex Functional Reference, page XXX) | 12 | Regular expression matching, search, counting, replacement, and split helpers. |
| `sb.rowset` | sb_rowset.md (SBsql Functions — SB Rowset Functional Reference, page XXX) | 16 | Rowset, set-returning, table-value, multiset, and series construction helpers. |
| `sb.spatial` | sb_spatial.md (SBsql Functions — SB Spatial Functional Reference, page XXX) | 93 | Bounded spatial geometry helpers for WKT, narrow GeoJSON, point WKB text, predicates, measurements, and construction. |
| `sb.temporal` | sb_temporal.md (SBsql Functions — SB Temporal Functional Reference, page XXX) | 39 | Date, time, timestamp, interval, timezone, and temporal context helpers. |
| `sb.timeseries` | sb_timeseries.md (SBsql Functions — SB Timeseries Functional Reference, page XXX) | 1 | Time-series bucketing, interpolation, downsampling, and aggregate helper surfaces. |
| `sb.uuid` | sb_uuid.md (SBsql Functions — SB UUID Functional Reference, page XXX) | 10 | UUID generation, parsing, inspection, and timestamp extraction helpers. |
| `sb.vector` | sb_vector.md (SBsql Functions — SB Vector Functional Reference, page XXX) | 14 | Dense vector construction, distance, normalization, dimension, and aggregate helpers. |
| `sb.xml` | sb_xml.md (SBsql Functions — SB XML Functional Reference, page XXX) | 20 | Bounded XML construction, query, serialization, validation, and XML-to-row helpers. |

## Entry Format

Each package entry follows the same structure:

- `Purpose`: user-facing intent of the operation.
- `Call Forms`: accepted function or operator spelling.
- `Parameters`: argument names or roles plus descriptor/coercion rules.
- `Returns`: descriptor and value rule.
- `Behavior`: NULL, volatility, determinism, side-effect, collation, timezone, and security notes.
- `Errors`: message-vector and refusal behavior.
- `Example`: representative SBsql use.
- `Technical Details`: SBLR, AST, entrypoint, UUID, optimizer, and cost metadata for diagnostics.

## Cross References

- Operators (SBsql Language Reference — Syntax, page XXX) explains symbolic operator precedence and result typing.
- Operator Type Result Matrix (SBsql Language Reference — Syntax, page XXX) gives type-result details for operator families.
- Type System Overview (SBsql Language Reference — Foundations, Data Types, and Catalog, page XXX) explains descriptors, domains, coercion, and NULL behavior.
- Procedural SQL (SBsql Language Reference — Syntax, page XXX) explains routine use of diagnostics, cursors, and function calls.


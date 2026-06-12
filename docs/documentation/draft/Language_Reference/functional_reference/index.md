# Functional Reference Index

The functional reference documents SBsql built-in operations by package namespace. Each package page gives the operation name, purpose, accepted call forms, parameter rules, return rule, behavior, error handling, example usage, and technical binding information.

These pages describe the SBsql language surface. They do not grant privileges and they do not bypass schema sandboxing, descriptor checks, policy admission, or MGA transaction authority.

Technical identifiers such as SBLR symbols, AST class names, UUIDs, entrypoints, optimizer notes, and conformance markers are diagnostic trace fields. They connect the documented surface to implementation evidence; they are not independent user contracts or release-support claims.

## Packages

| Package | File | Records | Scope |
| --- | --- | ---: | --- |
| `sb.core` | [sb_core.md](sb_core.md) | 605 | Core scalar, aggregate, window, session, catalog, diagnostic, and language helper surfaces used by ordinary SBsql expressions and procedural SQL. |
| `sb.crypto` | [sb_crypto.md](sb_crypto.md) | 27 | Cryptographic, hashing, random-value, armor, and bounded encryption helper functions. |
| `sb.cursor` | [sb_cursor.md](sb_cursor.md) | 15 | Cursor, stream, rowset-handle, and table-value conversion helpers used by procedural SQL and streaming execution. |
| `sb.fn.diagnostic` | [sb_diagnostic.md](sb_diagnostic.md) | 1 | Statement and procedural diagnostic helpers, including row-count context. |
| `sb.json` | [sb_json.md](sb_json.md) | 44 | JSON and JSONB construction, extraction, path, aggregation, and table-shaping helpers. |
| `sb.lob` | [sb_lob.md](sb_lob.md) | 13 | Large-object and locator helpers for bounded LOB access through engine-managed handles. |
| `sb.operator` | [sb_operator.md](sb_operator.md) | 24 | Operator functions for arithmetic, comparison, boolean, pattern, JSON, array, and vector-like operator spellings. |
| `sb.scalar` (range) | [sb_range.md](sb_range.md) | 9 | Range-boundary and containment helpers for descriptor-backed range values; registered under `sb.scalar.range_*`. |
| `sb.regex` | [sb_regex.md](sb_regex.md) | 12 | Regular expression matching, search, counting, replacement, and split helpers. |
| `sb.rowset` | [sb_rowset.md](sb_rowset.md) | 16 | Rowset, set-returning, table-value, multiset, and series construction helpers. |
| `sb.spatial` | [sb_spatial.md](sb_spatial.md) | 93 | Bounded spatial geometry helpers for WKT, narrow GeoJSON, point WKB text, predicates, measurements, and construction. |
| `sb.temporal` | [sb_temporal.md](sb_temporal.md) | 39 | Date, time, timestamp, interval, timezone, and temporal context helpers. |
| `sb.timeseries` | [sb_timeseries.md](sb_timeseries.md) | 1 | Time-series bucketing, interpolation, downsampling, and aggregate helper surfaces. |
| `sb.uuid` | [sb_uuid.md](sb_uuid.md) | 10 | UUID generation, parsing, inspection, and timestamp extraction helpers. |
| `sb.vector` | [sb_vector.md](sb_vector.md) | 14 | Dense vector construction, distance, normalization, dimension, and aggregate helpers. |
| `sb.xml` | [sb_xml.md](sb_xml.md) | 20 | Bounded XML construction, query, serialization, validation, and XML-to-row helpers. |

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

- [Operators](../syntax_reference/operators.md) explains symbolic operator precedence and result typing.
- [Operator Type Result Matrix](../syntax_reference/operator_type_result_matrix.md) gives type-result details for operator families.
- [Type System Overview](../data_types/type_system_overview.md) explains descriptors, domains, coercion, and NULL behavior.
- [Procedural SQL](../syntax_reference/procedural_sql.md) explains routine use of diagnostics, cursors, and function calls.

# Text, Collation, And Charset

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `data_types_text_collation`


## Purpose

Text values carry charset and collation descriptors. Comparisons, pattern matching, ordering, grouping, indexes, and generated columns use those descriptors rather than plain byte ordering.

String length functions distinguish character length from encoded byte length. Donor profiles may render donor-compatible spellings, but the binding target is still a ScratchBird descriptor and operation UUID.

Example:

```sql
select name
from app.customer
where lower(name) = lower(:search_name)
order by name collate default;
```

## Supported Text Types

| Canonical Type | Common Aliases | Length Unit | Payload | Value Bounds |
| --- | --- | --- | --- | --- |
| `char(n)` | `character(n)` | characters | Fixed-length encoded text, padded according to descriptor/profile | Exactly `n` characters after profile padding rules. Byte size depends on charset. |
| `varchar(n)` | `character varying(n)` | characters | Variable-length encoded text plus descriptor metadata | 0 through `n` characters. Byte size depends on charset and may be lower than row/page policy. |
| `text` | donor `text` spellings where admitted | characters | Variable-length encoded text | Policy bounded; large values may use overflow storage. |
| `clob` | `character large object` | characters or stream chunks | Character LOB/overflow stream | Policy bounded; stream, page, and transaction limits apply. |
| `nchar(n)` | national character fixed text | characters | Fixed-length text using the national character descriptor | Exactly `n` characters under the configured national charset. |
| `nvarchar(n)` | national character varying text | characters | Variable-length national text | 0 through `n` characters under the configured national charset. |
| `nclob` | national character large object | characters or stream chunks | National character LOB/overflow stream | Policy bounded; stream, page, and transaction limits apply. |

The declared length of character types is a character count, not a byte count. The storage footprint is the encoded byte count plus descriptor and row metadata. A value can satisfy the declared character count and still be refused if the encoded bytes cannot fit the active row, overflow, stream, or policy limit.

## Charset Contract

| Charset Class | Contract |
| --- | --- |
| Database default charset | Used when a declaration omits `character set` and the active donor profile does not override the default. |
| Column charset | Stored in the column descriptor and used by comparisons, functions, indexes, and result rendering. |
| Literal charset introducer | Binds a literal to a specific charset before coercion. Unsupported or lossy literal conversion is refused unless an explicit profile rule admits it. |
| National character set | Used by `nchar`, `nvarchar`, and `nclob` descriptors. |
| Binary data | Not a charset. `blob`, `bytea`, `binary`, and `varbinary` values require explicit conversion before text functions may operate on them. |

## Collation Contract

Collation is part of the descriptor and is therefore part of equality, ordering, grouping, indexing, and uniqueness behavior.

| Collation Rule | Behavior |
| --- | --- |
| Default collation | Applied when neither the column nor expression states a collation. |
| Explicit `collate` | Overrides the expression collation for that expression and for an index key if used in an index definition. |
| Case/accent sensitivity | Descriptor-owned. Do not infer byte order from display text. |
| Deterministic comparison | Required for ordinary equality, uniqueness, grouping, and B-tree ordering. Non-deterministic collations must be refused for features that require stable key order unless a provider-specific proof admits them. |
| Donor rendering | Donor parsers render donor collation names but bind them to ScratchBird descriptors. A Firebird collation name, PostgreSQL collation name, or MySQL collation name is not authority outside its active donor profile. |

## Text Donor Profile Notes

| Donor Profile | Text Compatibility Rule |
| --- | --- |
| Firebird | Preserves Firebird `CHAR`, `VARCHAR`, character set, collation, `BLOB SUB_TYPE TEXT`, fixed-length padding, and comparison behavior for admitted Firebird declarations. |
| PostgreSQL | Preserves PostgreSQL `text`, `varchar`, `char`, collation-visible ordering, pattern operators, and expression index collation behavior where surfaced. |
| MySQL and MariaDB | Preserves MySQL-family charset and collation defaults, per-column charset/collation clauses, and text/blob family distinctions. |
| SQLite | Preserves SQLite declared affinity and collation names where surfaced, while descriptor binding owns execution. |

## Syntax Productions

```ebnf
expression              ::= expression_atom (binary_operator expression_atom)* ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and profile options.
- SBLR admission maps the bound request to an operation family and result shape.
- The engine rechecks authority before durable state changes or result delivery.

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| currency | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| FULL | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| POSITION(substringINtext) | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| current_server | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| st_x | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| lock_timeout_default | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| bit_count | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| operation_evidence_required | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| USING | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| chr(integer) | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| element(multiset<T>) | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| st_makepoint | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| REAL | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| sb.special_form.coalesce | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| dearmor | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| regexp_like(string,pattern[,flags]) | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| IMAGE | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| CURSOR | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| jsonb_object_keys | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| client_min_messages | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| json_array_elements | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| sb.scalar.nullif | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| cos | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| boolean_cast_from_text | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |

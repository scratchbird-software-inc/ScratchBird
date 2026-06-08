# Operators

This page is part of the SBsql Language Reference Manual. It describes operator syntax, precedence, associativity, descriptor binding, and the boundary between portable SBsql operators and SBsql operator aliases.

## Purpose

Operators are expression syntax. They do not execute as text. The parser recognizes an operator form, the binder resolves operand descriptors and overloads, SBLR carries the canonical operation, and the engine evaluates the operation under descriptor, transaction, and security authority.

The detailed operand/result matrix is in [operator_type_result_matrix.md](operator_type_result_matrix.md).

## Binding Model

1. Parse the operator token or contextual operator phrase.
2. Resolve the operator to a canonical operation ID such as `sb.operator.add`.
3. Resolve operand descriptors, domains, collations, charsets, timezones, and SBsql policy options.
4. Apply the implicit conversion matrix only where the conversion is safe and admitted.
5. Derive the result descriptor.
6. Lower to SBLR expression operation.
7. Evaluate in the engine. Runtime errors such as overflow, divide-by-zero, invalid pattern, invalid JSON path, or unsupported descriptor combinations return diagnostics.

Names and tokens are not authority after binding. Descriptors and SBLR operation IDs are authority.

## Precedence And Associativity

The following table lists SBsql expression precedence from highest to lowest. Parentheses override all precedence rules.

| Level | Operators or Forms | Associativity | Notes |
| --- | --- | --- | --- |
| 1 | `(...)`, function calls, casts, literals, parameters, object references | n/a | Primary expressions bind first. |
| 2 | JSON/document access `->`, `->>`; array/subscript forms where admitted; vector distance forms such as `<->` in vector-search context | left | These bind tightly to the left expression. |
| 3 | unary `+`, unary `-` | right | Unary plus is descriptor-preserving where admitted. Unary minus requires a numeric descriptor. |
| 4 | exponentiation | policy-dependent | Portable SBsql uses `power(base, exponent)`. The token `^` is SBsql sensitive and is not assigned a single portable SBsql meaning. |
| 5 | `*`, `/`, `%` | left | Multiplication, numeric division, and modulo. |
| 6 | `+`, `-` | left | Numeric addition/subtraction, and temporal plus/minus interval forms. |
| 7 | `||` | left | Text concatenation. XML/document concatenation uses explicit functions or SBsql-specific forms. |
| 8 | comparison: `=`, `<>`, `!=`, `<`, `<=`, `>`, `>=`; `IS DISTINCT FROM`; `IS NOT DISTINCT FROM` | non-associative | Chained comparisons must be written with `AND`. |
| 9 | pattern and membership predicates: `LIKE`, `ILIKE`, regex match, `BETWEEN`, `IN`, `IS NULL`, `IS [NOT] TRUE`, `IS [NOT] FALSE`, `IS [NOT] UNKNOWN` | non-associative | These return `boolean` and use three-valued logic. |
| 10 | `NOT` | right | Logical negation. |
| 11 | `AND` | left | Three-valued logical conjunction. |
| 12 | `XOR` | left | Boolean exclusive-or. SBsql bitwise XOR is not this operator. |
| 13 | `OR` | left | Three-valued logical disjunction. |

Set operators such as `UNION`, `INTERSECT`, and `EXCEPT` are query operators, not scalar expression operators. Their rules are documented with query syntax.

## Portable Operator Catalog

| Surface | Symbol or Phrase | Canonical Operation | Operand Families | Result Family |
| --- | --- | --- | --- | --- |
| Unary minus | `-a` | `sb.operator.unary_minus` | numeric | numeric derived from operand descriptor |
| Addition | `a + b` | `sb.operator.add` | numeric; temporal plus interval | numeric or temporal derived from operands |
| Subtraction | `a - b` | `sb.operator.subtract` | numeric; temporal minus interval; temporal minus temporal | numeric, temporal, or interval derived from operands |
| Multiplication | `a * b` | `sb.operator.multiply` | numeric | numeric derived from operands |
| Division | `a / b` | `sb.operator.divide` | numeric | decimal or approximate numeric derived from operands |
| Modulo | `a % b` | `sb.operator.modulo` | numeric | numeric remainder descriptor |
| Power | `power(a,b)` | `sb.scalar.power` | numeric | `real64` unless a future descriptor-specific exact power overload is admitted |
| Concatenation | `a || b` | `sb.operator.concat` lowering to `sb.scalar.concat` | text-compatible values | character/text descriptor |
| Equality | `a = b` | `sb.operator.equal` | comparable descriptors | `boolean` |
| Inequality | `a <> b`, `a != b` | `sb.operator.not_equal` | comparable descriptors | `boolean` |
| Ordering | `<`, `<=`, `>`, `>=` | `sb.operator.less`, `less_equal`, `greater`, `greater_equal` | ordered descriptors | `boolean` |
| Distinctness | `IS DISTINCT FROM`, `IS NOT DISTINCT FROM` | `sb.operator.is_distinct_from` plus negation for `NOT` form | comparable descriptors, including nulls | `boolean` |
| Logical NOT | `NOT a` | `sb.operator.not` | boolean | `boolean` |
| Logical AND | `a AND b` | `sb.operator.and` | boolean | `boolean` |
| Logical XOR | `a XOR b` | `sb.operator.xor` | boolean | `boolean` |
| Logical OR | `a OR b` | `sb.operator.or` | boolean | `boolean` |
| LIKE | `a LIKE b [ESCAPE e]` | `sb.operator.like` | text, text pattern, optional text escape | `boolean` |
| ILIKE | `a ILIKE b [ESCAPE e]` | `sb.operator.ilike` | text, text pattern, optional text escape | `boolean` |
| Regex match | profile/operator form or `regexp_like(...)` | `sb.operator.regex_match` or `sb.regex.match` | text, pattern, optional flags | `boolean` |
| JSON get | `document -> path` | `sb.operator.json_get` | JSON/document plus path/key/index | `json_document` |
| JSON get text | `document ->> path` | `sb.operator.json_get_text` | JSON/document plus path/key/index | text |
| Array contains | profile form or `array_contains(array, value)` | `sb.operator.array_contains` or scalar range/array function | array/multiset and element | `boolean` |
| Vector distance | `<->` in vector search context | vector-search operation | vectors with matching descriptor dimensions | distance/rank evidence, normally `real64` |

## Bit Operators And Symbols

Portable SBsql exposes bit operations as functions so the type contract is unambiguous:

| Function | Result |
| --- | --- |
| `bit_count(value)` | `int64` |
| `bit_and(a,b)` | `int64` |
| `bit_or(a,b)` | `int64` |
| `bit_xor(a,b)` | `int64` |
| `bit_shift_left(value,n)` | `int64` |
| `bit_shift_right(value,n)` | `int64` |
| `bit_set(value,position)` | `int64` |
| `bit_clear(value,position)` | `int64` |
| `bit_toggle(value,position)` | `int64` |
| `bit_test(value,position)` | `boolean` |

The symbolic tokens `&`, `|`, `^`, `~`, `<<`, and `>>` are SBsql sensitive. A specific SBsql policy may assign a meaning to these tokens. Portable SBsql scripts should use named bit functions and `power()` instead of assuming a global meaning for `^` or `|`.

## Null Behavior

Most scalar operators are strict: if any required operand is `null`, the result is `null`. The main exceptions are logical and null-aware predicates:

| Form | Null Rule |
| --- | --- |
| `a IS DISTINCT FROM b` | Never returns `null`; treats null as a comparable state. |
| `a IS NOT DISTINCT FROM b` | Never returns `null`; negates distinctness. |
| `NOT` | `NOT null` returns `null`. |
| `AND` | `false AND null` returns `false`; `true AND null` returns `null`; `null AND null` returns `null`. |
| `OR` | `true OR null` returns `true`; `false OR null` returns `null`; `null OR null` returns `null`. |
| `XOR` | Returns `null` when either side is `null` unless a SBsql policy explicitly admits a different boolean XOR rule. |
| `LIKE`, `ILIKE`, regex | Returns `null` when the input, pattern, or required escape/flags are `null`. |

## SBsql Profile Operator Aliases

SBsql parsers may map SBsql-defined symbols to canonical SBsql operations or parser-support UDR calls, but only inside that SBsql policy. Examples:

| Token or Form | Why It Is Profile Sensitive |
| --- | --- |
| `^` | May mean exponentiation, bitwise XOR, or be unsupported depending on SBsql. Portable SBsql uses `power()` or `bit_xor()`. |
| `|` | May mean bitwise OR, pipe-related syntax, or SBsql-specific text/search syntax. Portable SBsql uses `bit_or()` or explicit functions. |
| `#`, `~`, `!~`, `~*`, `!~*` | Commonly regex, bitwise, or SBsql-defined operators. Portable SBsql uses named regex or bit functions unless a SBsql session policy admits the symbol. |
| `@>`, `<@`, `&&`, `<<`, `>>` | Commonly array/range/geometric operators in SBsql dialects. Portable SBsql uses named range, array, spatial, or graph functions unless the active SBsql policy admits the symbol. |
| `<->` | Vector distance in vector-search context. It is not a general numeric subtraction token sequence. |

## Diagnostics

Operator binding or execution must fail closed for:

- unresolved operator overload;
- unsupported operand descriptor combination;
- ambiguous implicit conversion;
- numeric overflow;
- divide-by-zero;
- invalid modulo divisor;
- invalid collation or non-deterministic comparison where deterministic order is required;
- invalid pattern, regex, JSON path, or vector descriptor;
- profile-only symbol used outside the profile that admits it;
- protected value release through an ordinary operator.

## Related Pages

- [operator_type_result_matrix.md](operator_type_result_matrix.md)
- [../functional_reference/sb_operator.md](../functional_reference/sb_operator.md)
- [data_types/conversion_matrix.md](../data_types/conversion_matrix.md)
- [data_types/numeric_types.md](../data_types/numeric_types.md)
- [data_types/text_collation_and_charset.md](../data_types/text_collation_and_charset.md)
- [ebnf/expression.md](ebnf/expression.md)

# Operator Type Result Matrix

This page is part of the SBsql Language Reference Manual. It gives the descriptor result rules for SBsql operators. The syntax and precedence rules are in [operators.md](operators.md).

## Reading The Matrix

The result type is a descriptor rule, not just a display name. Domains keep their domain identity only where the operation explicitly preserves the domain; most arithmetic returns the carrier descriptor derived from the operands.

`null` operands follow the null rules in [operators.md](operators.md). Unless a row below states otherwise, a required `null` operand produces `null`.

## Numeric Arithmetic

| Expression | Accepted Operand Families | Result Descriptor Rule | Diagnostics |
| --- | --- | --- | --- |
| `+a` | numeric | Operand descriptor if unary plus is admitted for that descriptor. | Unsupported descriptor. |
| `-a` | signed integer, decimal, real; unsigned only where value can be represented by an admitted signed descriptor | Signed/widened numeric descriptor derived from operand. | Unsupported descriptor, unsigned range overflow. |
| `a + b` | integer/integer | Narrowest exact signed or unsigned integer descriptor that can represent the derived result rule; may widen. | Overflow or ambiguous signed/unsigned widening. |
| `a + b` | decimal with integer or decimal | Decimal descriptor derived from precision and scale. | Derived precision/scale exceeds policy. |
| `a + b` | real with numeric | Wider approximate real descriptor, normally `real64` if either side is `real64` or exact-to-real coercion is required. | Invalid real value or refused exact-to-real coercion. |
| `a - b` | integer/integer | Exact integer descriptor derived from operands; may widen or become signed. | Overflow or ambiguous signed/unsigned widening. |
| `a - b` | decimal with integer or decimal | Decimal descriptor derived from precision and scale. | Derived precision/scale exceeds policy. |
| `a - b` | real with numeric | Wider approximate real descriptor. | Invalid real value or refused coercion. |
| `a * b` | integer/integer | Exact integer descriptor derived from operands; may widen. | Overflow or unsupported widening. |
| `a * b` | decimal with integer or decimal | Decimal descriptor derived from operand precision and scale. | Derived precision/scale exceeds policy. |
| `a * b` | real with numeric | Wider approximate real descriptor. | Invalid real value or refused coercion. |
| `a / b` | integer/integer | Exact numeric division descriptor. Portable SBsql derives a decimal result; use `div(a,b)` for integer quotient. | Divide-by-zero; derived precision/scale exceeds policy. |
| `a / b` | decimal with integer or decimal | Decimal descriptor with derived scale sufficient for the declared operation policy. | Divide-by-zero; derived precision/scale exceeds policy. |
| `a / b` | real with numeric | Wider approximate real descriptor. | Divide-by-zero; invalid real value. |
| `a % b` | integer/integer | Integer remainder descriptor derived from operands. | Modulo by zero; unsupported signed/unsigned mix. |
| `a % b` | decimal with integer or decimal | Decimal remainder descriptor derived from operands. | Modulo by zero; derived precision/scale exceeds policy. |
| `power(a,b)` | numeric/numeric | `real64` for the current portable SBsql function surface. | Domain error, invalid real value, overflow according to descriptor policy. |

## Numeric Promotion Summary

| Operand Pair | Ordinary Arithmetic Result |
| --- | --- |
| `int16` with `int16` | Exact integer, widened if required by result rule. |
| `int32` with `int64` | `int64` or wider exact integer when required. |
| signed integer with unsigned integer | Exact descriptor that can represent both operands and result, or diagnostic refusal. |
| integer with `decimal(p,s)` | Decimal with derived precision/scale. |
| decimal with decimal | Decimal with derived precision/scale. |
| exact numeric with `real`/`float4` | Approximate real, normally at least `real`. |
| exact numeric with `double precision`/`float8` | `double precision`/`real64`. |
| `decfloat(16)` with exact numeric | Decimal floating descriptor selected by profile. |
| `decfloat(34)` with exact numeric | Decimal floating descriptor selected by profile. |

## Text And Binary Operators

| Expression | Accepted Operand Families | Result Descriptor Rule | Diagnostics |
| --- | --- | --- | --- |
| `a || b` | text-compatible operands | Character/text descriptor derived from operand charsets, collations, and maximum length. | Unsupported charset/collation merge; result length exceeds policy. |
| `a LIKE pattern` | text, text pattern | `boolean`. Uses input collation/charset and pattern semantics. | Invalid pattern or unsupported collation. |
| `a LIKE pattern ESCAPE e` | text, text pattern, one-character text escape | `boolean`. | Escape length not one character; invalid escape usage. |
| `a ILIKE pattern` | text, text pattern | `boolean`. Case-insensitive behavior is collation/profile owned. | Unsupported case-folding/collation rule. |
| regex match | text, regex pattern, optional flags | `boolean`. | Invalid regex or unsupported flags. |
| binary with text operator | none by default | no implicit result | Explicit conversion required. |

`+` is not a portable text concatenation operator. Use `||` or `concat(...)`.

## Boolean Operators

| Expression | Accepted Operand Families | Result Descriptor Rule | Null Rule |
| --- | --- | --- | --- |
| `NOT a` | boolean | `boolean` | `NOT null` is `null`. |
| `a AND b` | boolean/boolean | `boolean` | SQL three-valued logic. |
| `a XOR b` | boolean/boolean | `boolean` | `null` on unknown operand unless profile says otherwise. |
| `a OR b` | boolean/boolean | `boolean` | SQL three-valued logic. |

Boolean operators do not coerce arbitrary text or numeric values to boolean in portable SBsql. Use explicit casts where a session policy requires non-default behavior.

## Comparison Operators

| Expression | Accepted Operand Families | Result Descriptor Rule | Notes |
| --- | --- | --- | --- |
| `a = b` | comparable descriptors | `boolean` | Text uses collation descriptor. |
| `a <> b`, `a != b` | comparable descriptors | `boolean` | Negated equality. |
| `a < b` | ordered descriptors | `boolean` | Requires an ordering contract. |
| `a <= b` | ordered descriptors | `boolean` | Requires an ordering contract. |
| `a > b` | ordered descriptors | `boolean` | Requires an ordering contract. |
| `a >= b` | ordered descriptors | `boolean` | Requires an ordering contract. |
| `a IS DISTINCT FROM b` | comparable descriptors | `boolean`, never `null` | Null-aware comparison. |
| `a IS NOT DISTINCT FROM b` | comparable descriptors | `boolean`, never `null` | Negated null-aware comparison. |

Comparable descriptors include numeric descriptors with admitted conversions, text descriptors with compatible collation rules, temporal descriptors with compatible precision/timezone rules, UUID descriptors, boolean descriptors, and profile-admitted JSON/document descriptors. Ordering requires a deterministic comparison contract.

## Temporal Operators

| Expression | Accepted Operand Families | Result Descriptor Rule | Diagnostics |
| --- | --- | --- | --- |
| `date + interval` | date, interval | `date` or `timestamp` according to interval fields and descriptor policy. | Ambiguous units or unsupported calendar policy. |
| `timestamp + interval` | timestamp, interval | Same timestamp family and precision unless descriptor policy derives a wider result. | Unsupported precision/timezone rule. |
| `time + interval` | time, interval | `time` when the interval is time-compatible. | Date-bearing interval applied to time-only descriptor. |
| `date - interval` | date, interval | `date` or `timestamp` according to interval fields and descriptor policy. | Ambiguous units or unsupported calendar policy. |
| `timestamp - interval` | timestamp, interval | Same timestamp family and precision unless descriptor policy derives a wider result. | Unsupported precision/timezone rule. |
| `timestamp - timestamp` | compatible timestamps | `interval`. | Incompatible timezone/precision policy. |
| `date - date` | compatible dates | day-count interval descriptor. | Incompatible calendar policy. |
| temporal comparison | compatible temporal descriptors | `boolean`. | Incompatible timezone, precision, or calendar policy. |

Portable scripts should use explicit casts when mixing `timestamp` and `timestamptz`.

## JSON And Document Operators

| Expression | Accepted Operand Families | Result Descriptor Rule | Diagnostics |
| --- | --- | --- | --- |
| `document -> path` | `json`, `jsonb`, or `document` plus path/key/index | `json_document`. Missing versus JSON null is preserved by the operation result. | Invalid path, unsupported path type, non-document input. |
| `document ->> path` | `json`, `jsonb`, or `document` plus path/key/index | text descriptor. Missing result is `null`; JSON null renders according to descriptor policy. | Invalid path, non-scalar extraction where scalar text is required. |
| JSON equality/order | profile-admitted document descriptors | `boolean` where comparison is admitted. | Unsupported comparison contract. |

JSON path text is parser input only. The bound path descriptor owns execution.

## Array, Multiset, Range, And Spatial Operators

| Form | Accepted Operand Families | Result Descriptor Rule | Portable SBsql Recommendation |
| --- | --- | --- | --- |
| `array_contains(array,value)` | array/multiset and element | `boolean` | Use named function unless the SBsql admits a symbol such as `@>`. |
| range contains | range and element/range | `boolean` | Use named range functions in portable SBsql. |
| range overlaps | range/range | `boolean` | Use named range functions in portable SBsql. |
| spatial predicate | geometry/geography descriptors | `boolean` | Use named `st_*` functions unless a SBsql policy admits spatial operator symbols. |
| vector distance `<->` | vector descriptors with matching dimensions | distance/rank evidence, normally `real64` | Valid in vector-search context; exact rerank/recheck rules still apply. |

SBsql symbols such as `@>`, `<@`, `&&`, `<<`, and `>>` are profile-sensitive. They are not portable SBsql unless the SBsql grammar explicitly admits a portable spelling for that operation.

## Bit Operations

| Portable Form | Accepted Operand Families | Result Descriptor Rule |
| --- | --- | --- |
| `bit_count(value)` | integer or bit string where admitted | `int64` |
| `bit_and(a,b)` | integer-compatible descriptors | `int64` |
| `bit_or(a,b)` | integer-compatible descriptors | `int64` |
| `bit_xor(a,b)` | integer-compatible descriptors | `int64` |
| `bit_shift_left(value,n)` | integer-compatible descriptor and non-negative shift count | `int64` |
| `bit_shift_right(value,n)` | integer-compatible descriptor and non-negative shift count | `int64` |
| `bit_set(value,position)` | integer-compatible descriptor and bit position | `int64` |
| `bit_clear(value,position)` | integer-compatible descriptor and bit position | `int64` |
| `bit_toggle(value,position)` | integer-compatible descriptor and bit position | `int64` |
| `bit_test(value,position)` | integer-compatible descriptor and bit position | `boolean` |
| `bit_string_position(needle, haystack)` | bit string descriptors | `int64` |
| `bit_string_substring(value, start, length)` | bit string descriptor and integer positions | bit string descriptor |

The symbolic forms `&`, `|`, `^`, `~`, `<<`, and `>>` are profile-sensitive and must not be assumed portable SBsql. In portable SBsql, `^` is not the power operator; use `power(a,b)`.

## Protected Values

Protected values cannot be converted to raw text or binary through ordinary operators. Equality, identity comparison, reachability checks, and metadata inspection are admitted only through protected-value surfaces with explicit authorization. A normal operator must not release protected material.

## Result Descriptor Examples

| Expression | Result |
| --- | --- |
| `cast(1 as int32) + cast(2 as int32)` | exact integer descriptor, widened if required by result policy. |
| `cast(1 as int64) / cast(2 as int64)` | decimal division descriptor. |
| `div(cast(5 as int64), cast(2 as int64))` | `int64` integer quotient function result. |
| `cast(5 as int64) % cast(2 as int64)` | integer remainder descriptor. |
| `cast(1.0 as double precision) / 2` | `double precision`/`real64`. |
| `'alpha' || '-' || 'beta'` | character/text descriptor. |
| `timestamp '2026-01-01 00:00:00' - timestamp '2025-01-01 00:00:00'` | interval descriptor. |
| `json_col -> '$.item'` | `json_document`. |
| `json_col ->> '$.item'` | text descriptor. |
| `uuid_col = uuid '018f0000-0000-7000-8000-000000000001'` | `boolean`. |
| `embedding <-> :query_vector` | vector distance/rank evidence in vector-search context. |

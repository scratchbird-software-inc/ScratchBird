# Numeric Types

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `data_types_numeric`


## Purpose

Numeric expressions bind through descriptor-aware overload resolution. Integer, decimal, fixed precision, floating, money-like, and donor-rendered numeric values select their result descriptor before execution.

Arithmetic operators are strict by default: invalid descriptors, overflow, divide-by-zero, unsupported precision, or ambiguous casts return diagnostics. Aggregates widen state according to their descriptor rule rather than assuming the client display type is the accumulator type.

Example:

```sql
select sum(invoice_total), avg(invoice_total)
from billing.invoice
where invoice_total > 0;
```

## Supported Numeric Types

| Canonical Type | Common Aliases | Family | Storage Payload | SQL-Visible Value Range |
| --- | --- | --- | --- | --- |
| `int8` | `tinyint` where admitted by profile | signed integer | 1 byte | -128 to 127 |
| `uint8` | `utinyint` where admitted by profile | unsigned integer | 1 byte | 0 to 255 |
| `int16` | `smallint` | signed integer | 2 bytes | -32768 to 32767 |
| `uint16` | `usmallint` where admitted by profile | unsigned integer | 2 bytes | 0 to 65535 |
| `int32` | `int`, `integer` | signed integer | 4 bytes | -2147483648 to 2147483647 |
| `uint32` | `uint`, `uinteger` where admitted by profile | unsigned integer | 4 bytes | 0 to 4294967295 |
| `int64` | `bigint` | signed integer | 8 bytes | -9223372036854775808 to 9223372036854775807 |
| `uint64` | `ubigint` where admitted by profile | unsigned integer | 8 bytes | 0 to 18446744073709551615 |
| `int128` | Firebird `INT128` where the Firebird profile is active | signed integer | 16 bytes | -170141183460469231731687303715884105728 to 170141183460469231731687303715884105727 |
| `decimal(p,s)` | `numeric(p,s)` | exact decimal | descriptor-dependent, normally fixed binary decimal for admitted precision | `p` total digits, `s` fractional digits; absolute value is less than `10^(p-s)`. |
| `decfloat(16)` | Firebird-compatible `DECFLOAT(16)` where admitted | decimal floating point | 8 bytes logical decimal-float payload | Decimal floating value with 16 decimal digits of precision; exponent and special value policy are descriptor-owned. |
| `decfloat(34)` | Firebird-compatible `DECFLOAT(34)` where admitted | decimal floating point | 16 bytes logical decimal-float payload | Decimal floating value with 34 decimal digits of precision; exponent and special value policy are descriptor-owned. |
| `real` | `float4` | approximate real | 4 bytes | IEEE binary32-style finite range, approximately 6 to 9 significant decimal digits. |
| `double precision` | `float8`, `double` | approximate real | 8 bytes | IEEE binary64-style finite range, approximately 15 to 17 significant decimal digits. |
| `float(p)` | profile-dependent `float` | approximate real | 4 or 8 bytes | `p` selects the admitted real descriptor according to profile policy. |
| `money` | `currency` | exact decimal domain/profile | descriptor-dependent | Money-like value over a decimal carrier; currency code, scale, rounding, and rendering are descriptor or domain policy. |

`decimal(p,s)` and `numeric(p,s)` admit precision and scale only when the active descriptor policy supports them. The portable baseline is precision `1` through `38` and scale `0` through `p`; higher precision may be admitted by a database or donor profile, but scripts that require portability should declare the precision they need and treat unsupported precision as a bind-time error.

## Arithmetic And Widening

| Operation Class | Result Descriptor Rule |
| --- | --- |
| Integer plus, minus, multiply | Uses the narrowest integer descriptor that can represent the declared result rule; overflow is diagnostic, not silent wrap. |
| Integer division | Uses exact or approximate result according to the operator form and profile; divide-by-zero is diagnostic. |
| Decimal arithmetic | Preserves exact decimal semantics; result precision and scale are derived from operand descriptors and may be refused if the derived descriptor exceeds policy. |
| Floating arithmetic | Uses the wider approximate descriptor of the operands unless an explicit cast fixes the result. |
| Aggregate `sum` | Uses a widened accumulator descriptor, not necessarily the display descriptor of the input column. |
| Aggregate `avg` | Uses a descriptor that can represent fractional results even when the input is integer. |
| Mixed signed and unsigned arithmetic | Requires a descriptor rule that can represent both operands, or an explicit cast. Ambiguous widening is refused. |

## Numeric Donor Profile Notes

| Donor Profile | Numeric Compatibility Rule |
| --- | --- |
| Firebird | Preserves Firebird integer widths, `INT128`, `NUMERIC`/`DECIMAL` precision and scale, `DECFLOAT(16)`, `DECFLOAT(34)`, and Firebird arithmetic diagnostics. |
| PostgreSQL | Preserves PostgreSQL exact `numeric`, `smallint`/`integer`/`bigint`, `real`, `double precision`, and operator-resolution behavior where surfaced by the PostgreSQL parser. |
| MySQL and MariaDB | Preserves signed and unsigned integer attributes, `DECIMAL(M,D)`, approximate `FLOAT`/`DOUBLE`, and profile-specific overflow or truncation diagnostics. |
| SQLite | Preserves declared affinity behavior while binding actual values to canonical descriptors for execution and storage. |

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

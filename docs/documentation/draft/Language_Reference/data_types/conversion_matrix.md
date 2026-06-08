# Conversion Matrix

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `data_types_conversion_matrix`


## Purpose

The conversion model is a matrix of source descriptor, target descriptor, cast class, lossiness, null behavior, collation/charset rule, timezone rule, and security policy. SBsql documentation groups these rules by behavior rather than by donor spelling.

Use explicit casts when a script crosses family boundaries: text to numeric, text to temporal, binary to UUID, JSON scalar to typed scalar, vector element conversion, or protected value rendering. Unsupported or ambiguous conversions must fail before execution.

Example:

```sql
select cast(payload->>'created_at' as timestamp with time zone)
from app.ingest_document;
```

## Core Conversion Matrix

| Source Family | Target Family | Implicit Assignment | Explicit Cast | Notes |
| --- | --- | --- | --- | --- |
| `null_value` | any nullable target | yes | yes | `null` adopts the target descriptor. Non-nullable targets reject it. |
| `boolean` | `text` | profile-dependent | yes | Renders canonical `true`/`false` unless donor profile overrides rendering. |
| `text` | `boolean` | no | yes | Accepted spellings are descriptor/profile controlled. Invalid text is diagnostic. |
| `integer` | wider signed integer | yes | yes | Refused if the value cannot fit the target width. |
| `integer` | unsigned integer | no | yes | Refused for negative values or out-of-range values. |
| `unsigned_integer` | signed integer | no | yes | Refused if the unsigned value cannot fit the signed target. |
| `integer` or `unsigned_integer` | `decimal` | yes when exact | yes | Precision/scale must admit the exact value. |
| `decimal` | integer | no | yes | Fractional values, overflow, or unsupported rounding are diagnostic. |
| exact numeric | approximate real | profile-dependent | yes | May be lossy. Portable scripts should cast explicitly. |
| approximate real | exact numeric | no | yes | NaN, infinity, out-of-range, and non-exact values are diagnostic unless profile admits a policy. |
| numeric | `text` | no | yes | Rendering is descriptor/profile controlled. |
| `text` | numeric | no | yes | Parsing is descriptor/profile controlled; invalid text is diagnostic. |
| `text` | `date`, `time`, `timestamp`, `interval` | no | yes | Requires accepted literal syntax for the target descriptor. |
| temporal | `text` | no | yes | Rendering is timezone/profile controlled. |
| temporal | temporal | profile-dependent | yes | Date/time/timestamp conversions require explicit timezone and precision rules when ambiguous. |
| `binary` | `text` | no | yes | Requires charset or encoding rule. Raw byte reinterpretation is not implicit. |
| `text` | `binary` | no | yes | Requires encoding rule. |
| `text` | `uuid` | no | yes | Text must be canonical or profile-admitted UUID text. |
| `uuid` | `text` | no | yes | Renders canonical UUID text unless profile overrides. |
| `json`/`jsonb` scalar | scalar target | no | yes | Path extraction must produce a scalar compatible with the target descriptor. |
| scalar | `json`/`jsonb` | no | yes | Creates a JSON scalar value. |
| `vector<T,n>` | `vector<U,n>` | no | yes | Dimension must match; element conversion must be admitted and may require quantization proof. |
| `array<T>` | `array<U>` | no | yes | Every element conversion must be admitted. |

## Implicit Conversion Policy

Implicit conversion is intentionally narrow. It exists for safe assignment and overload resolution only when the conversion is exact, deterministic, and cannot hide donor-specific behavior.

| Implicitly Safe Class | Examples |
| --- | --- |
| Widening exact integer | `int16` to `int32`, `int32` to `int64`, `uint16` to `uint32`. |
| Exact integer to admitted decimal | `int32` to `decimal(18,0)`. |
| `null` to nullable target | `null` assigned to `varchar(30)` or `timestamp`. |
| Domain to base carrier | A domain value used where its underlying carrier is required, subject to domain policy. |

All other family-crossing conversions should be written as `cast(...)`, `try_cast(...)`, a donor-specific conversion function, or a named SBsql conversion function.

## Lossy And Refused Conversions

| Conversion | Default Result |
| --- | --- |
| Out-of-range numeric cast | Diagnostic refusal. |
| Decimal to integer with fractional value | Diagnostic refusal unless an explicit rounding function is used. |
| Approximate real NaN or infinity to exact numeric | Diagnostic refusal unless a profile explicitly admits a sentinel mapping. |
| Text to numeric/date/UUID with invalid syntax | Diagnostic refusal. |
| Text to binary without charset/encoding rule | Diagnostic refusal. |
| Binary to text with invalid encoded bytes | Diagnostic refusal. |
| Vector dimension mismatch | Diagnostic refusal. |
| Protected value to raw text or raw binary | Denied. Protected values require authorized release surfaces and never ordinary casts. |

## Donor Coercion Rules

Donor parsers may accept donor-specific implicit casts, literal forms, or assignment behavior, but those rules are local to that parser profile. The parser must lower the donor behavior into a canonical descriptor decision before SBLR admission. The engine does not infer Firebird, PostgreSQL, MySQL, SQLite, or any other donor coercion from plain SBsql text.

## Syntax Productions

```ebnf
expression              ::= expression_atom (binary_operator expression_atom)* ;
```

```ebnf
literal                 ::= string_literal | numeric_literal | boolean_literal | null_literal | uuid_ref ;
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

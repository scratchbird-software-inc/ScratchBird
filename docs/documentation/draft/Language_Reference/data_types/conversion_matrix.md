# Conversion Matrix

This page is part of the SBsql Language Reference Manual. It defines descriptor
conversion behavior for implicit assignment, explicit casts, `try_cast`,
literal binding, domain validation, lossy conversion, protected values, and
diagnostics.

Generation task: `data_types_conversion_matrix`

## Purpose

This page is a quick reference for what the binder does when a value of one
type must be used where another type is expected. The short version: implicit
conversion is intentionally narrow — SBsql will not silently widen, truncate, or
re-encode a value across descriptor-family boundaries. When you need a value in
a different type, write an explicit `cast(... as ...)` so the conversion is
visible in the query and testable in isolation.

The full picture depends on the combination of source descriptor, target
descriptor, and conversion class (implicit assignment, explicit cast, try-cast,
named conversion function, domain validation, or protected release). The tables
below capture that matrix for the common cases.

## Conversion Classes

| Class | Meaning |
| --- | --- |
| Contextual literal binding | A literal adopts a target descriptor because the surrounding expression supplies one. |
| Implicit assignment | Safe assignment conversion used for storage, parameters, returns, variables, and generated values. |
| Explicit cast | User-requested conversion through `cast(value as type)`. |
| Try-cast | User-requested conversion that returns a documented failure value instead of raising the normal diagnostic. |
| Named conversion function | Function with explicit conversion semantics, such as decode, encode, rounding, parsing, or formatting. |
| Domain validation | Conversion to a domain carrier followed by domain null policy and constraints. |
| Protected release | Policy-controlled release of protected material; not an ordinary cast. |

## Core Matrix

| Source | Target | Implicit Assignment | Explicit Cast | Notes |
| --- | --- | --- | --- | --- |
| `null` | nullable target | yes | yes | `null` adopts the target descriptor. Non-nullable targets reject it. |
| `boolean` | `text` | no | yes | Renders canonical boolean text unless a profile renders otherwise. |
| `text` | `boolean` | no | yes | Accepted spellings are descriptor-owned. Invalid text is diagnostic. |
| signed integer | wider signed integer | yes | yes | Refused if value cannot fit. |
| unsigned integer | wider unsigned integer | yes | yes | Refused if value cannot fit. |
| signed integer | unsigned integer | no | yes | Negative or out-of-range values are refused. |
| unsigned integer | signed integer | no | yes | Refused if value cannot fit target signed range. |
| exact integer | decimal | yes when exact | yes | Target precision and scale must admit the exact value. |
| decimal | exact integer | no | yes | Fractional values require explicit rounding function; plain cast refuses. |
| exact numeric | approximate real | policy-dependent | yes | May be lossy. Portable scripts cast explicitly. |
| approximate real | exact numeric | no | yes | NaN, infinity, out-of-range, or non-exact values are refused unless policy admits them. |
| numeric | text | no | yes | Rendering is descriptor-owned. |
| text | numeric | no | yes | Invalid syntax or out-of-range values are diagnostics. |
| text | temporal | no | yes | Requires valid literal syntax for target descriptor. |
| temporal | text | no | yes | Rendering uses timezone/profile policy. |
| temporal | temporal | policy-dependent | yes | Precision, timezone, and calendar loss must be explicit. |
| binary | text | no | yes | Requires encoding or charset rule. |
| text | binary | no | yes | Requires encoding or decode rule. |
| text | UUID | no | yes | Text must be canonical or profile-admitted UUID text. |
| UUID | text | no | yes | Renders canonical UUID text unless policy says otherwise. |
| UUID | `binary(16)` | no | yes | Converts to 16-byte representation. |
| `binary(16)` | UUID | no | yes | Validates UUID descriptor policy. |
| document scalar | scalar target | no | yes | Extracted value must be scalar and compatible with target descriptor. |
| scalar | document | no | yes | Creates a document scalar value. |
| array element | another element descriptor | no | yes | Every element conversion must be admitted. |
| vector element | another vector element descriptor | no | yes | Dimension must match; quantization/lossiness must be explicit. |
| protected reference | raw text or binary | no | no ordinary cast | Requires protected release authority. |
| raw text or binary | protected reference | no | no ordinary cast | Requires admitted protect/store route. |
| domain | base carrier | policy-dependent | yes | Domain erasure must be admitted. |
| base carrier | domain | assignment only after validation | yes | Carrier conversion, null policy, and constraints all apply. |

## Implicit Assignment

Implicit assignment is allowed only when conversion is exact, deterministic, and
does not hide policy-sensitive behavior.

Safe default examples:

| Source | Target | Reason |
| --- | --- | --- |
| `int16` | `int32` | Exact widening. |
| `int32` | `int64` | Exact widening. |
| `int64` | `int128` | Exact widening. |
| `uint16` | `uint32` | Exact widening. |
| `uint64` | `uint128` | Exact widening. |
| exact integer | `decimal(p,0)` | Exact when precision admits the value. |
| `null` | nullable target | Null adopts target descriptor. |
| domain | same domain target | Same domain descriptor. |

Implicit assignment should not silently:

- truncate text or binary values;
- round decimals;
- lose timezone;
- lose fractional temporal precision;
- convert text to numeric;
- reinterpret binary as text;
- erase domain policy;
- release protected material;
- change vector dimension;
- coerce document missing values to null unless the operator says so.

## Explicit Cast

Explicit casts state user intent and make conversion auditable.

```sql
select cast(:amount_text as decimal(18,2)) as amount;

select cast(:event_text as timestamptz) as event_at;

select cast(:id_text as uuid) as object_id;
```

An explicit cast still fails when the target descriptor refuses the value.
Explicit does not mean unsafe.

## Try-Cast

`try_cast` follows the same conversion rules as `cast`, but returns the
documented failure result instead of raising the ordinary conversion diagnostic.
The failure result is descriptor-owned and must be distinguishable from a valid
value when the contract requires it.

Example:

```sql
select try_cast(payload->>'amount' as decimal(18,2)) as parsed_amount
from app.ingest_document;
```

`try_cast` must not hide protected-material denial, sandbox denial, recovery
fences, or operation admission failures.

## Lossy Conversions

| Conversion | Default Behavior |
| --- | --- |
| Decimal with fractional part to integer | Refuse. Use explicit rounding/truncation function when intended. |
| Decimal to lower precision/scale | Refuse unless explicit cast policy admits rounding. |
| Approximate real to exact numeric | Refuse for non-exact values, NaN, infinity, or out-of-range values. |
| Timestamp to lower precision | Refuse unless explicit cast policy admits precision loss. |
| `timestamptz` to `timestamp` | Refuse unless timezone-loss policy is explicit. |
| Text to shorter text | Refuse unless explicit truncation function is used. |
| Text charset conversion with unrepresentable characters | Refuse unless explicit replacement policy is used. |
| Binary to text with invalid encoded bytes | Refuse. |
| Vector `real32` element to `int8` element | Refuse unless quantization policy is explicit. |
| Document number to lower-precision numeric | Refuse unless explicit cast policy admits the loss. |

## Domain Conversion

Conversion to a domain follows the domain assignment pipeline:

1. convert value to the domain carrier descriptor;
2. apply domain null policy;
3. apply parent domain constraints;
4. apply target domain constraints;
5. apply element policy for compound values;
6. preserve the target domain descriptor where the operation says so.

Example:

```sql
select cast(:candidate as app.email_text) as email_value;
```

Domain validation is not optional merely because the carrier conversion
succeeded.

## Text, Binary, And UUID Conversions

| Conversion | Required Detail |
| --- | --- |
| Text to binary | Encoding, decode rule, or binary target policy. |
| Binary to text | Charset or encoding rule. |
| Text to UUID | Canonical or admitted UUID text syntax. |
| UUID to text | Rendering policy, default canonical UUID text. |
| UUID to binary | Explicit 16-byte representation. |
| Binary to UUID | Exactly 16 bytes and UUID descriptor validation. |

Text is never treated as a byte string without conversion. Binary is never
treated as text without conversion.

## Temporal Conversions

| Conversion | Rule |
| --- | --- |
| Text to date/time/timestamp/interval | Explicit cast or target context required. Invalid fields are diagnostic. |
| Timestamp to `timestamptz` | Requires timezone rule. |
| `timestamptz` to timestamp | Requires explicit timezone-loss rule. |
| Date to timestamp | Admitted when default time-of-day policy is explicit. |
| Timestamp to date | Refuses time loss unless explicit cast policy admits it. |
| Interval to numeric | Requires explicit operation defining units. |
| Numeric to interval | Requires explicit operation defining units. |

## Document And Collection Conversions

| Conversion | Rule |
| --- | --- |
| Document scalar to scalar | Requires path extraction and scalar descriptor compatibility. |
| Scalar to document scalar | Explicit cast creates document scalar value. |
| Document object to text | Rendering operation, not ordinary implicit conversion. |
| Array to array | Element-by-element conversion required. |
| Row to record | Shape and field descriptor compatibility required. |
| Record to row | Target field names/order and descriptor compatibility required. |
| Missing document path | Missing is not null unless the operator descriptor says so. |

## Vector, Spatial, Graph, Search, Time-Series, And Key-Value Conversions

| Conversion | Rule |
| --- | --- |
| Vector element type change | Explicit cast; dimension must match; quantization/lossiness must be explicit. |
| Vector dimension change | Refused unless a named operation defines padding, projection, or embedding conversion. |
| Geometry/geography conversion | Requires spatial reference and geodetic policy. |
| Graph node/edge/path conversion | Requires graph descriptor compatibility and identity preservation. |
| Search document conversion | Requires tokenization profile and rendering policy. |
| Time-series sample conversion | Requires time key and value descriptor compatibility. |
| Key-value conversion | Requires key and value descriptor compatibility. |

## Protected Material

Protected material cannot be released through ordinary casts.

| Request | Default Result |
| --- | --- |
| Protected reference to text | Denied unless a release surface admits it. |
| Protected reference to binary | Denied unless a release surface admits it. |
| Protected value in diagnostic | Redacted. |
| Protected value in support bundle | Redacted unless release policy admits specific evidence. |
| Protected value in backup/replication/migration stream | Requires explicit release/export policy. |

## Syntax Productions

```ebnf
cast_expression         ::= "cast" "(" expression "as" data_type ")" ;
```

```ebnf
try_cast_expression     ::= "try_cast" "(" expression "as" data_type ")" ;
```

```ebnf
conversion_function     ::= function_call ;
```

```ebnf
assignment_conversion   ::= expression target_descriptor ;
```

## Diagnostics

| Condition | Required Result |
| --- | --- |
| Unsupported conversion | Bind diagnostic or unsupported message vector. |
| Ambiguous conversion | Bind diagnostic requiring explicit cast. |
| Out-of-range value | Conversion diagnostic. |
| Lossy conversion without policy | Conversion diagnostic. |
| Invalid literal syntax | Parse or conversion diagnostic according to phase. |
| Domain validation failure | Domain diagnostic. |
| Protected release denied | Denied message vector. |
| Descriptor stale | Rebind or stale descriptor diagnostic. |
| Recovery state uncertain | Fail-closed diagnostic before conversion side effects. |

## Related Pages

- [Type System Overview](type_system_overview.md)
- [Numeric Types](numeric_types.md)
- [Text, Collation, And Charset](text_collation_and_charset.md)
- [Temporal Types](temporal_types.md)
- [Binary, UUID, And Protected Values](binary_uuid_and_protected_values.md)
- [Document, Graph, Vector, And Multimodel Types](document_graph_vector_and_multimodel_types.md)
- [Domains, Casts, And Coercion](domains_casts_and_coercion.md)
- [Operator Type Result Matrix](../syntax_reference/operator_type_result_matrix.md)

## Verification Checklist

The conversion proof suite should demonstrate:

- implicit conversions are limited to exact, deterministic cases;
- explicit casts do not bypass range, precision, timezone, charset, or policy
  checks;
- `try_cast` uses the same validation path as `cast`;
- decimal, temporal, text, binary, and vector lossy conversions are refused by
  default;
- domain conversion applies carrier conversion, null policy, parent checks, and
  target checks;
- binary/text conversions require explicit encoding or charset rules;
- protected material cannot be released by ordinary casts;
- document missing/null behavior is preserved through conversions;
- vector dimension mismatches are refused;
- stale descriptor and security epochs invalidate cached conversion decisions.

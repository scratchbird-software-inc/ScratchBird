# Numeric Types

This page is part of the SBsql Language Reference Manual. It defines the public
numeric descriptor families, ranges, literal binding rules, arithmetic result
rules, aggregate behavior, comparison behavior, indexing behavior, and
diagnostics.

Generation task: `data_types_numeric`

## Purpose

Numeric values bind through descriptor-aware overload resolution. Integers,
unsigned integers, decimals, decimal floating values, approximate reals, and
money-like domains choose their result descriptor before execution.

Arithmetic is strict by default. Overflow, underflow, divide-by-zero,
unsupported precision, ambiguous signed/unsigned widening, invalid casts, and
unsupported special values return diagnostics rather than silent wraparound or
silent truncation.

## Supported Numeric Types

| Canonical Type | Common Aliases | Family | Payload | Value Range |
| --- | --- | --- | --- | --- |
| `int8` | none | signed integer | 1 byte | -128 to 127 |
| `uint8` | none | unsigned integer | 1 byte | 0 to 255 |
| `int16` | `smallint` | signed integer | 2 bytes | -32768 to 32767 |
| `uint16` | none | unsigned integer | 2 bytes | 0 to 65535 |
| `int32` | `int`, `integer` | signed integer | 4 bytes | -2147483648 to 2147483647 |
| `uint32` | none | unsigned integer | 4 bytes | 0 to 4294967295 |
| `int64` | `bigint` | signed integer | 8 bytes | -9223372036854775808 to 9223372036854775807 |
| `uint64` | none | unsigned integer | 8 bytes | 0 to 18446744073709551615 |
| `int128` | none | signed integer | 16 bytes | -170141183460469231731687303715884105728 to 170141183460469231731687303715884105727 |
| `uint128` | none | unsigned integer | 16 bytes | 0 to 340282366920938463463374607431768211455 |
| `decimal(p,s)` | `numeric(p,s)` | exact decimal | descriptor-dependent | `p` total digits and `s` fractional digits. |
| `decimal_float` | `decfloat` in compatibility dialects | decimal floating | descriptor-defined | Up to 34 decimal digits of precision; exponent and special values are descriptor-owned. |
| `real` | none | approximate real | 4 bytes | Binary32-style finite range with approximately 6 to 9 significant decimal digits. |
| `double precision` | `double` | approximate real | 8 bytes | Binary64-style finite range with approximately 15 to 17 significant decimal digits. |
| `float(p)` | none | approximate real | 4 or 8 bytes | `p` selects an admitted real descriptor under database policy. |
| `money` | none | domain over exact decimal | descriptor-dependent | Currency, scale, rounding, and rendering are domain or descriptor policy. |

`decimal(p,s)` admits precision and scale only when the active descriptor policy
supports them. The portable baseline is precision `1` through `38` and scale
`0` through `p`. Higher precision can be admitted by policy, but portable
scripts should declare only the precision they require and treat unsupported
precision as a bind-time diagnostic.

## Literal Binding

Numeric literals are not final type authority until binding.

| Literal Form | Binding Rule |
| --- | --- |
| `123` | Binds by context, or to the smallest admitted signed integer descriptor that can represent it. |
| `123U` | Binds to the default unsigned integer descriptor for the active policy. |
| `123U8`, `123U16`, `123U32`, `123U64`, `123U128` | Binds to the named unsigned width and rejects values outside that width. |
| `123I8`, `123I16`, `123I32`, `123I64`, `123I128` | Binds to the named signed width and rejects values outside that width. |
| `123.45` | Binds to an exact decimal descriptor unless context selects another admitted numeric descriptor. |
| `1.2e10` | Binds to an admitted exact or approximate descriptor according to context and literal policy. |
| `-123` | Parses as unary minus applied to a positive literal descriptor. It cannot bind to an unsigned descriptor unless an explicit conversion policy admits the final value, which ordinary unsigned descriptors do not. |
| String-to-number | Requires an explicit cast or assignment conversion. Invalid text is diagnostic. |

Examples:

```sql
select 123U128 as exact_unsigned_value;

select cast('340282366920938463463374607431768211455' as uint128)
       as max_uint128;
```

Negative text or numeric input cannot bind to `uint128`. Values greater than
`340282366920938463463374607431768211455` are refused as overflow.

## Arithmetic Result Rules

| Operation Class | Result Descriptor Rule |
| --- | --- |
| Signed integer plus/minus/multiply | Uses the admitted result descriptor selected by operand widths and context. Overflow is diagnostic. |
| Unsigned integer plus/multiply | Uses an unsigned result descriptor when admitted. Overflow is diagnostic. |
| Unsigned subtraction | Requires a signed or wider descriptor if the result can be negative, or refuses underflow. |
| Mixed signed/unsigned arithmetic | Requires a descriptor that can represent both operands and the operation result, or an explicit cast. Ambiguous widening is refused. |
| Integer division | Uses the operator form and descriptor policy. Divide-by-zero is diagnostic. |
| Decimal arithmetic | Derives precision and scale from operands. Refuses when the derived descriptor exceeds policy. |
| Approximate real arithmetic | Uses the wider approximate descriptor unless an explicit cast fixes the result. |
| Decimal plus approximate real | Requires an explicit cast unless the context selects an admitted lossy conversion policy. |
| Unary minus | Refuses for unsigned values that cannot be represented by the target descriptor. |
| Modulo | Uses exact integer or exact decimal descriptors only where the operation policy admits it. |
| Power | Uses a descriptor-specific operation; exactness and overflow are policy-owned. |

Silent wraparound is not an SBsql numeric behavior.

## Comparison And Ordering

Numeric comparison uses descriptor-aware comparison.

| Case | Rule |
| --- | --- |
| Same exact descriptor | Compare exact numeric values. |
| Widenable exact descriptors | Compare after exact widening. |
| Signed and unsigned | Compare only after an exact descriptor can represent both values. |
| Decimal and integer | Compare exactly when the decimal descriptor can represent the integer. |
| Approximate real | Compare according to approximate descriptor rules. NaN and infinity behavior is descriptor-owned. |
| Domain values | Use domain operation policy first, then carrier comparison where admitted. |

Numeric index keys must use the same comparison rule as expression evaluation.
An index can produce candidates, but final row visibility and predicate truth
still require engine recheck.

## Aggregates

Aggregate state can be wider than the displayed result.

| Aggregate | Rule |
| --- | --- |
| `count(*)` | Returns an exact integer descriptor large enough for admitted row counts. |
| `sum(integer)` | Uses a widened exact accumulator descriptor. Overflow is diagnostic unless a documented wider accumulator is available. |
| `sum(decimal)` | Uses a decimal accumulator with derived precision/scale. Policy can refuse unsupported precision. |
| `avg(integer)` | Returns a descriptor capable of fractional results. |
| `avg(decimal)` | Uses decimal result rules with derived precision/scale. |
| `min` and `max` | Preserve comparison descriptor and return a compatible descriptor. |
| Statistical aggregates | Use function-specific descriptors and diagnostics for unsupported inputs. |

Example:

```sql
select sum(invoice_total), avg(invoice_total)
from billing.invoice
where invoice_total > cast(0 as decimal(18,2));
```

## Assignment And Casts

Assignment to a numeric target follows this order:

1. bind the source descriptor;
2. bind the target descriptor or domain;
3. apply exact widening where admitted;
4. apply explicit cast policy if the statement requested it;
5. validate range, scale, precision, rounding, and domain constraints;
6. store or return the descriptor-bound value.

Examples:

```sql
create table app.measurement (
    measurement_id uuid primary key,
    count_exact uint128,
    amount decimal(18,2) not null,
    ratio double precision
);

insert into app.measurement (measurement_id, count_exact, amount, ratio)
values (:id, cast(:count_text as uint128), cast(:amount as decimal(18,2)), :ratio);
```

## Diagnostics

| Condition | Required Result |
| --- | --- |
| Overflow or underflow | Numeric diagnostic before silent wrap or truncation. |
| Divide by zero | Numeric diagnostic. |
| Decimal precision/scale unsupported | Bind or execution diagnostic, depending on when the derived descriptor is known. |
| Ambiguous signed/unsigned result | Bind diagnostic requiring explicit cast. |
| Fractional value cast to integer | Diagnostic unless an explicit rounding function is used. |
| NaN/infinity to exact numeric | Diagnostic unless descriptor policy explicitly admits a mapping. |
| Invalid text-to-number cast | Conversion diagnostic. |
| Domain constraint failure | Domain diagnostic after carrier conversion. |

## Syntax Productions

```ebnf
numeric_type            ::= signed_integer_type
                          | unsigned_integer_type
                          | decimal_type
                          | real_type
                          | money_type ;
```

```ebnf
signed_integer_type     ::= "int8" | "int16" | "smallint"
                          | "int32" | "int" | "integer"
                          | "int64" | "bigint"
                          | "int128" ;
```

```ebnf
unsigned_integer_type   ::= "uint8" | "uint16" | "uint32"
                          | "uint64" | "uint128" ;
```

```ebnf
decimal_type            ::= ("decimal" | "numeric") "(" precision "," scale ")"
                          | "decimal_float" ;
```

```ebnf
real_type               ::= "real"
                          | "double" "precision"
                          | "float" "(" precision ")" ;
```

## Related Pages

- [Type System Overview](type_system_overview.md)
- [Conversion Matrix](conversion_matrix.md)
- [Domains, Casts, And Coercion](domains_casts_and_coercion.md)
- [Operator Type Result Matrix](../syntax_reference/operator_type_result_matrix.md)

## Verification Checklist

The numeric proof suite should demonstrate:

- every numeric spelling resolves to the expected descriptor;
- signed and unsigned ranges reject out-of-range values;
- `uint128` accepts the documented maximum and rejects maximum-plus-one;
- negative input refuses unsigned assignment;
- decimal precision and scale are enforced;
- integer, decimal, and approximate arithmetic use documented result rules;
- aggregate state widens according to descriptor rules;
- ambiguous mixed signed/unsigned arithmetic is refused;
- explicit casts and assignment conversions produce identical validation for the
  same target descriptor;
- numeric indexes use the same comparison rule as expression evaluation;
- diagnostics never silently wrap, truncate, or coerce lossy values.

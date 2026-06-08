# Temporal Types

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `data_types_temporal`


## Purpose

Temporal values are descriptor-bound with timezone policy. Date, time, timestamp, interval, extraction, truncation, formatting, and arithmetic functions must state whether they use session timezone, stored timezone, UTC-style normalization, or SBsql-specific rendering.

Portable scripts should avoid relying on client-local display rules. Use explicit casts, extract fields with named parts, and bind interval values with clear units.

Example:

```sql
select date_trunc('day', created_at), count(*)
from app.event_log
group by date_trunc('day', created_at);
```

## Supported Temporal Types

| Canonical Type | Common Aliases | Logical Payload | SQL-Visible Contract |
| --- | --- | --- | --- |
| `date` | SBsql `DATE` where admitted | 32-bit day ordinal in the default descriptor | Calendar date without time of day. Uses the descriptor calendar policy and has no timezone component. |
| `time(p)` | `time`, SBsql `TIME` | Time-of-day with fractional precision `p` | Time without date or timezone. Portable precision is `0` through `6`; higher precision is policy dependent. |
| `time(p) with time zone` | `time with time zone` | Time-of-day plus timezone/offset descriptor data | Time-of-day with timezone rendering and comparison rules owned by descriptor policy. |
| `timestamp(p)` | `datetime` where SBsql policy maps it | Date and time without timezone | Timestamp without timezone. Portable fractional precision is `0` through `6`; higher precision is policy dependent. |
| `timestamp(p) with time zone` | `timestamptz`, `timestamp_tz` | Instant plus timezone rendering policy | Stored and compared according to descriptor instant semantics; displayed through session/profile timezone policy. |
| `interval` | `interval year to month`, `interval day to second` | Duration fields selected by descriptor | Carries duration fields, not a calendar instant. Arithmetic must state or infer the target temporal descriptor. |

The default timestamp descriptor uses a signed 64-bit microsecond-style instant representation for portable execution. The exact encoded range and precision can be changed only by descriptor version or policy; scripts that require a specific precision should declare it explicitly.

## Temporal Precision And Timezone Rules

| Rule | Behavior |
| --- | --- |
| Fractional precision | `p` controls fractional second precision. If omitted, the SBsql default applies. Unsupported precision is refused at bind time. |
| `timestamp without time zone` | Represents date and time fields without timezone normalization. Session timezone does not change the stored value. |
| `timestamp with time zone` | Represents an instant. Session or profile timezone affects rendering, not transaction visibility or stored identity. |
| `date` arithmetic | Produces a `date`, `timestamp`, or `interval` according to operand descriptors and operator form. Ambiguous units are refused. |
| Leap seconds and calendar policy | Descriptor-owned. Portable scripts should not depend on SBsql-defined leap-second rendering unless the SBsql policy documents it. |
| Current time functions | Bound through engine expression operations so the transaction/session timestamp source is explicit and testable. |

## Temporal SBsql Profile Notes

| SBsql Profile | Temporal Compatibility Rule |
| --- | --- |
| SBsql | Preserves SBsql `DATE`, `TIME`, `TIMESTAMP`, `TIME WITH TIME ZONE`, `TIMESTAMP WITH TIME ZONE`, fractional precision, and rendering rules for admitted SBsql versions. |
| SBsql | Preserves SBsql `date`, `time`, `timestamp`, `timestamp with time zone`, `interval`, extraction, truncation, and timezone rendering where surfaced. |
| SBsql | Preserves SBsql `date`, `time`, `datetime`, `timestamp`, fractional precision, and zero-date/refusal policy selected by the profile. |
| SBsql | Preserves SBsql affinity and date/time function compatibility at the parser/profile boundary while binding stored values to canonical descriptors. |

## Syntax Productions

```ebnf
literal                 ::= string_literal | numeric_literal | boolean_literal | null_literal | uuid_ref ;
```

```ebnf
expression              ::= expression_atom (binary_operator expression_atom)* ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and SBsql execution options.
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

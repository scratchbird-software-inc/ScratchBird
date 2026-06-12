# Temporal Types

This page is part of the SBsql Language Reference Manual. It defines date,
time, timestamp, timezone, interval, precision, literal, arithmetic, comparison,
indexing, and diagnostic behavior for temporal descriptors.

Generation task: `data_types_temporal`

## Purpose

Temporal values are descriptor-bound. A temporal descriptor states whether a
value is a calendar date, clock time, timestamp without timezone, instant with
timezone rendering, or duration. It also owns precision, calendar policy,
timezone policy, comparison, arithmetic, indexing, and rendering.

Portable scripts should avoid relying on client-local display rules. Use
explicit casts, explicit precision, named extraction parts, and timezone-aware
types when the instant matters.

## Supported Temporal Types

| Canonical Type | Common Aliases | Logical Payload | SQL-Visible Contract |
| --- | --- | --- | --- |
| `date` | none | Day ordinal under descriptor calendar policy. | Calendar date without time of day or timezone. |
| `time(p)` | `time` | Time of day with fractional precision `p`. | Clock time without date or timezone. |
| `time(p) with time zone` | `time with time zone` | Time of day plus timezone or offset descriptor data. | Clock time with timezone rendering/comparison policy. |
| `timestamp(p)` | none | Date plus time fields with fractional precision `p`. | Timestamp fields without timezone normalization. |
| `timestamptz` | `timestamp_tz` | Instant plus timezone rendering policy. | Stored and compared as an instant; rendered through session/profile timezone policy. |
| `interval` | `interval year to month`, `interval day to second` | Duration fields selected by descriptor. | Duration, not a calendar instant. |

The portable fractional-second precision is `0` through `6`. Higher precision
can be admitted by database policy, but scripts that require portability should
declare the precision they require and treat unsupported precision as a
bind-time diagnostic.

## Date, Time, Timestamp, And Instant

| Type | Timezone Meaning |
| --- | --- |
| `date` | No timezone component. A date is not shifted by session timezone. |
| `time(p)` | No date and no timezone component. |
| `time(p) with time zone` | Includes timezone or offset behavior defined by descriptor policy. |
| `timestamp(p)` | Stores date/time fields without timezone normalization. Session timezone does not change the stored value. |
| `timestamptz` | Represents an instant. Session timezone affects rendering, not stored instant identity. |
| `interval` | Has no timezone; it is applied to a temporal value according to arithmetic rules. |

Use `timestamptz` when the value must represent a real instant across sessions.
Use `timestamp` when the stored date/time fields are intended to remain local fields.

## Precision

Precision `p` controls fractional seconds.

| Rule | Behavior |
| --- | --- |
| Omitted precision | Uses the SBsql default temporal descriptor. |
| Supported precision | Binds to the exact descriptor. |
| Unsupported precision | Refused at bind time. |
| Assignment to lower precision | Refused unless an explicit rounding/truncation policy or function is used. |
| Cast to lower precision | Uses explicit cast policy; silent precision loss is not default behavior. |

## Literals And Casts

String literals remain text until a temporal context or explicit cast binds
them to a temporal descriptor.

```sql
select cast('2026-06-08' as date) as business_date;

select cast('2026-06-08 14:30:00.123456' as timestamp(6))
       as local_event_time;

select cast('2026-06-08 14:30:00.123456-04:00'
            as timestamptz)
       as event_instant;
```

Invalid calendar fields, invalid time fields, unsupported timezone names,
unsupported precision, and ambiguous literal forms return diagnostics.

## Current Temporal Functions

Current-time functions bind to engine expression operations. The descriptor must
make the timestamp source explicit.

| Function Class | Rule |
| --- | --- |
| Transaction timestamp | Stable for the transaction where the function contract says so. |
| Statement timestamp | Stable for the statement where the function contract says so. |
| Clock timestamp | Reads a current clock source where admitted. |
| Current date/time | Derived from the session timezone and descriptor policy. |
| Timezone conversion | Uses named timezone descriptors or offset descriptors. |

Current-time functions must be testable. They should not silently use a client
display clock as engine authority.

## Temporal Arithmetic

| Operation | Result Rule |
| --- | --- |
| `date + interval` | Produces a date or timestamp according to interval fields and target descriptor. |
| `timestamp + interval` | Produces a timestamp descriptor compatible with the input timestamp. |
| `timestamptz + interval` | Produces an instant descriptor with timezone rendering policy preserved. |
| `date - date` | Produces an interval or integer day count according to operator descriptor. |
| `timestamp - timestamp` | Produces an interval descriptor. |
| `time - time` | Produces an interval descriptor where admitted. |
| `interval + interval` | Produces an interval descriptor when fields are compatible. |
| `interval * numeric` | Uses interval scaling policy and refuses unsupported fractional or overflow results. |

Ambiguous units are refused. For example, adding a month interval can depend on
calendar policy and target date. The descriptor must own that behavior.

## Extraction, Truncation, And Formatting

Extraction and truncation bind named parts to operation descriptors.

| Operation Class | Rule |
| --- | --- |
| Extract | Field names such as year, month, day, hour, minute, second, timezone offset, or epoch bind to a descriptor-owned operation. |
| Truncation | `date_trunc`-style operations return a descriptor compatible with the input and requested part. |
| Formatting | Text rendering is explicit and profile-owned. Formatting is not storage authority. |
| Parsing | Text parsing uses explicit cast or conversion functions and fails on invalid input. |

Example:

```sql
select date_trunc('day', created_at) as event_day,
       count(*) as event_count
from app.event_log
group by date_trunc('day', created_at);
```

## Comparison And Indexes

| Type | Comparison Rule |
| --- | --- |
| `date` | Compare calendar ordinal under descriptor calendar policy. |
| `time` | Compare time-of-day under descriptor precision. |
| `time with time zone` | Compare according to descriptor timezone policy. |
| `timestamp` | Compare stored date/time fields without timezone conversion. |
| `timestamptz` | Compare instants. Rendering timezone does not change ordering. |
| `interval` | Compare only where the interval descriptor admits a total ordering. |

Temporal indexes use the same comparison rule as expression evaluation. Indexes
produce candidate evidence; final row visibility still requires MGA, predicate,
descriptor, and security recheck.

## Timezone Policy

Timezone behavior is descriptor-owned.

| Concern | Rule |
| --- | --- |
| Session timezone | Used for rendering and current date/time functions where the descriptor says so. |
| Stored timezone | May be stored as offset, named zone, normalized instant, or descriptor metadata according to policy. |
| Timezone database | Versioned timezone data can change rendering and must be tracked as descriptor or resource metadata. |
| Ambiguous local time | Refused or resolved according to explicit policy. |
| Nonexistent local time | Refused or resolved according to explicit policy. |

## Diagnostics

| Condition | Required Result |
| --- | --- |
| Invalid date or time field | Conversion diagnostic. |
| Unsupported precision | Bind diagnostic. |
| Precision loss | Diagnostic unless explicit cast policy admits it. |
| Unsupported timezone | Bind or conversion diagnostic. |
| Ambiguous or nonexistent local time | Diagnostic unless descriptor policy explicitly resolves it. |
| Temporal arithmetic overflow | Diagnostic. |
| Interval field mismatch | Bind diagnostic. |
| Text parsed as temporal without explicit context | Ambiguous bind diagnostic. |

## Syntax Productions

```ebnf
temporal_type           ::= date_type
                          | time_type
                          | timestamp_type
                          | interval_type ;
```

```ebnf
date_type               ::= "date" ;
```

```ebnf
time_type               ::= "time" precision_clause? ;
timestamp_type          ::= "timestamp" precision_clause?
                          | "timestamptz" ;
```

```ebnf
interval_type           ::= "interval" interval_qualifier? ;
```

## Related Pages

- [Type System Overview](type_system_overview.md)
- [Conversion Matrix](conversion_matrix.md)
- [Domains, Casts, And Coercion](domains_casts_and_coercion.md)
- [Operator Type Result Matrix](../syntax_reference/operator_type_result_matrix.md)
- [Transaction Control](../syntax_reference/transaction_control.md)

## Verification Checklist

The temporal proof suite should demonstrate:

- each temporal spelling resolves to the expected descriptor;
- fractional precision is enforced;
- text literals require context or explicit cast;
- invalid dates, times, and timezones are refused;
- `timestamp` and `timestamptz` differ in storage and comparison behavior;
- session timezone affects rendering where documented and not stored instant
  identity;
- temporal arithmetic uses documented result descriptors;
- interval field compatibility is enforced;
- temporal indexes use the same comparison rule as execution;
- current-time functions use engine-owned timestamp sources;
- timezone-data changes invalidate dependent rendering or planning state where
  required.

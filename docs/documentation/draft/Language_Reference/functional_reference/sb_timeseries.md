# SB Timeseries Functional Reference

Generation task: `sb_timeseries`

Package namespace: `sb.timeseries`

Time-series bucketing, interpolation, downsampling, and aggregate helper surfaces.

## How To Read This Page

Provides scalar time-series helpers for bucketing, interpolation, simple series aggregation, and downsampling.

Each entry below is written for a user reading SBsql, not for a registry maintainer. The technical fields are retained so an operator can connect the language surface to SBLR and engine diagnostics when troubleshooting.

Privileges, policy admission, sandboxing, and descriptor compatibility are still checked by the surrounding statement. A function being listed here does not grant access to catalog objects, protected material, files, network targets, or external services.

Every operation entry includes:

- `Purpose`: what the operation is for.
- `Call forms`: the public spelling or overload shapes recognized by SBsql.
- `Parameters`: the argument roles and descriptor/coercion rules.
- `Returns`: the result descriptor and value rule.
- `Behavior`: NULL, volatility, collation, timezone, side-effect, and execution notes.
- `Errors`: the message-vector conditions raised for invalid input or denied execution.
- `Example`: a representative SBsql usage shape. Examples use ordinary schema names such as `app.orders` and are meant to show the function form, not prescribe a schema.

## Package Inventory

| Kind | Records |
| --- | ---: |
| aggregate | 1 |

## Operation Reference

### `aggregate`

**Purpose:** Computes a simple aggregate over a bounded numeric time-series payload.

**Call Forms:**

- `aggregate(aggregate_name, numeric_series)`
- Syntax category: `function_call`

**Parameters:**

- `aggregate_name`: Text value naming the aggregate to apply. Supported names are `count`, `min`, `max`, `sum`, `avg`, and `average`.
- `numeric_series`: Text or descriptor-supported value containing numeric samples. The current bounded helper accepts comma-separated values and bracketed list text such as `'[1,2,3,4]'`.
- Coercion: Both arguments are read through descriptor-aware scalar conversion. Invalid text, unsupported aggregate names, malformed numeric samples, or ambiguous casts are refused.
- NULL handling: If the parsed series contains no numeric values, the function returns SQL `NULL` with a `real64` descriptor. Other NULL behavior follows descriptor binding for the supplied arguments.

**Returns:**

`real64`. `count` returns the number of parsed samples as a `real64`; `min` and `max` return the lowest and highest parsed samples; `sum` returns the sample total; `avg` and `average` return the arithmetic mean.

**Behavior:**

- Volatility: immutable for the same aggregate name and numeric series payload.
- Determinism: deterministic for the same parsed sample sequence.
- Side effects: none.
- Collation/charset: text input is parsed byte-stably; aggregate names are matched as canonical lowercase tokens.
- Timezone: not applicable to this scalar aggregate helper.
- Security and authority: executes as a bounded scalar helper. It does not create, write, or manage persistent time-series objects.

**Errors:**

The function refuses invalid arity, non-numeric series values, malformed descriptor input, and unsupported aggregate names through SBsql message vectors.

**Example:**

```sql
select aggregate('avg', '[1,2,3,4]') as average_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | timeseries.aggregate |
| UUID | 019f0000-0000-7000-8000-000000063901 |
| Kind | aggregate |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.timeseries_aggregate.v3 |
| AST binding | ast.expr.timeseries_aggregate |
| Engine entrypoint | aggregate |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

# SB Range Functional Reference

Generation task: `sb_range`

Package namespace: `sb.range`

Range-boundary and containment helpers for descriptor-backed range values.

## How To Read This Page

Inspects descriptor-backed ranges and evaluates range relationships.

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
| scalar | 9 |

## Operation Reference

### `range_contains`

**Purpose:** Returns whether one range contains another range.

**Call Forms:**

- `range_contains(value_1, value_2)`
- Syntax category: `function_call`

**Parameters:**

- `value_1`: Bound using the declared descriptor rules for this overload.
- `value_2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: two deterministic range descriptors.
- Coercion: range arguments accept deterministic bracket text like [lower,upper) or flat JSON descriptors with lower, upper, lower_inc, upper_inc, and empty fields; element comparison uses numeric ordering when both sides parse as finite numbers, otherwise lexical text ordering.
- NULL handling: SQL null input returns SQL null boolean.

**Returns:**

boolean true when the right range is empty or wholly contained by the left range.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual bounds; boolean helpers return boolean descriptor.
- Timezone: not applicable.
- Security and authority: pure range scalar helper; accepts deterministic textual and flat-json range descriptors; no parser SQL execution, catalog/storage lookup, external plugin call, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity or malformed descriptor refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select range_contains(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_contains |
| UUID | 019dffbb-f000-79e8-8098-f3a7e1212ace |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_range_contains.v3 |
| AST binding | ast.expr.scalar_range_contains |
| Engine entrypoint | range_contains |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC035-range-contains-true;SBSFC035-range-contains-empty`.

### `range_contains_element`

**Purpose:** Returns whether a range contains a scalar element.

**Call Forms:**

- `range_contains_element(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: range descriptor and scalar element.
- Coercion: range arguments accept deterministic bracket text like [lower,upper) or flat JSON descriptors with lower, upper, lower_inc, upper_inc, and empty fields; element comparison uses numeric ordering when both sides parse as finite numbers, otherwise lexical text ordering.
- NULL handling: SQL null input returns SQL null boolean.

**Returns:**

boolean true when the element falls within the range bounds.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual bounds; boolean helpers return boolean descriptor.
- Timezone: not applicable.
- Security and authority: pure range scalar helper; accepts deterministic textual and flat-json range descriptors; no parser SQL execution, catalog/storage lookup, external plugin call, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity or malformed descriptor refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select range_contains_element(active_range, 42) from app.range_examples;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_contains_element |
| UUID | 019dffbb-f000-74ed-91c4-1b280b76ac8a |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_range_contains_element.v3 |
| AST binding | ast.expr.scalar_range_contains_element |
| Engine entrypoint | range_contains_element |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC035-range-contains-element;SBSFC035-range-json-contains-element`.

### `range_lower`

**Purpose:** Returns the lower bound of a range.

**Call Forms:**

- `range_lower(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: one deterministic range descriptor.
- Coercion: range arguments accept deterministic bracket text like [lower,upper) or flat JSON descriptors with lower, upper, lower_inc, upper_inc, and empty fields; element comparison uses numeric ordering when both sides parse as finite numbers, otherwise lexical text ordering.
- NULL handling: SQL null input returns SQL null character.

**Returns:**

character textual lower bound or SQL null when the range is empty or unbounded below.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual bounds; boolean helpers return boolean descriptor.
- Timezone: not applicable.
- Security and authority: pure range scalar helper; accepts deterministic textual and flat-json range descriptors; no parser SQL execution, catalog/storage lookup, external plugin call, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity or malformed descriptor refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select range_lower(active_range) from app.time_windows;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_lower |
| UUID | 019dffbb-f000-7228-a8fd-b333c6378a8f |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_range_lower.v3 |
| AST binding | ast.expr.scalar_range_lower |
| Engine entrypoint | range_lower |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC035-range-lower;SBSFC035-range-lower-empty-null`.

### `range_lower_inc`

**Purpose:** Returns whether the lower range bound is inclusive.

**Call Forms:**

- `range_lower_inc(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: one deterministic range descriptor.
- Coercion: range arguments accept deterministic bracket text like [lower,upper) or flat JSON descriptors with lower, upper, lower_inc, upper_inc, and empty fields; element comparison uses numeric ordering when both sides parse as finite numbers, otherwise lexical text ordering.
- NULL handling: SQL null input returns SQL null boolean.

**Returns:**

boolean true when the lower bound is present and inclusive.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual bounds; boolean helpers return boolean descriptor.
- Timezone: not applicable.
- Security and authority: pure range scalar helper; accepts deterministic textual and flat-json range descriptors; no parser SQL execution, catalog/storage lookup, external plugin call, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity or malformed descriptor refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select range_lower_inc(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_lower_inc |
| UUID | 019dffbb-f000-7d64-9a6c-3f0fb7806195 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_range_lower_inc.v3 |
| AST binding | ast.expr.scalar_range_lower_inc |
| Engine entrypoint | range_lower_inc |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC035-range-lower-inc`.

### `range_overlaps`

**Purpose:** Returns whether two ranges overlap.

**Call Forms:**

- `range_overlaps(value_1, value_2)`
- Syntax category: `function_call`

**Parameters:**

- `value_1`: Bound using the declared descriptor rules for this overload.
- `value_2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: two deterministic range descriptors.
- Coercion: range arguments accept deterministic bracket text like [lower,upper) or flat JSON descriptors with lower, upper, lower_inc, upper_inc, and empty fields; element comparison uses numeric ordering when both sides parse as finite numbers, otherwise lexical text ordering.
- NULL handling: SQL null input returns SQL null boolean.

**Returns:**

boolean true when non-empty ranges share at least one included point.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual bounds; boolean helpers return boolean descriptor.
- Timezone: not applicable.
- Security and authority: pure range scalar helper; accepts deterministic textual and flat-json range descriptors; no parser SQL execution, catalog/storage lookup, external plugin call, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity or malformed descriptor refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select range_overlaps(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_overlaps |
| UUID | 019dffbb-f000-70da-bb90-bf9233ce6a1a |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_range_overlaps.v3 |
| AST binding | ast.expr.scalar_range_overlaps |
| Engine entrypoint | range_overlaps |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC035-range-overlaps-boundary;SBSFC035-range-overlaps-invalid`.

### `range_strictly_left`

**Purpose:** Returns whether one range lies strictly before another range.

**Call Forms:**

- `range_strictly_left(value_1, value_2)`
- Syntax category: `function_call`

**Parameters:**

- `value_1`: Bound using the declared descriptor rules for this overload.
- `value_2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: two deterministic range descriptors.
- Coercion: range arguments accept deterministic bracket text like [lower,upper) or flat JSON descriptors with lower, upper, lower_inc, upper_inc, and empty fields; element comparison uses numeric ordering when both sides parse as finite numbers, otherwise lexical text ordering.
- NULL handling: SQL null input returns SQL null boolean.

**Returns:**

boolean true when the left range ends before the right range begins without overlap.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual bounds; boolean helpers return boolean descriptor.
- Timezone: not applicable.
- Security and authority: pure range scalar helper; accepts deterministic textual and flat-json range descriptors; no parser SQL execution, catalog/storage lookup, external plugin call, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity or malformed descriptor refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select range_strictly_left(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_strictly_left |
| UUID | 019dffbb-f000-7240-837d-0768c70f3346 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_range_strictly_left.v3 |
| AST binding | ast.expr.scalar_range_strictly_left |
| Engine entrypoint | range_strictly_left |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC035-range-strictly-left`.

### `range_strictly_right`

**Purpose:** Returns whether one range lies strictly after another range.

**Call Forms:**

- `range_strictly_right(value_1, value_2)`
- Syntax category: `function_call`

**Parameters:**

- `value_1`: Bound using the declared descriptor rules for this overload.
- `value_2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: two deterministic range descriptors.
- Coercion: range arguments accept deterministic bracket text like [lower,upper) or flat JSON descriptors with lower, upper, lower_inc, upper_inc, and empty fields; element comparison uses numeric ordering when both sides parse as finite numbers, otherwise lexical text ordering.
- NULL handling: SQL null input returns SQL null boolean.

**Returns:**

boolean true when the left range begins after the right range ends without overlap.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual bounds; boolean helpers return boolean descriptor.
- Timezone: not applicable.
- Security and authority: pure range scalar helper; accepts deterministic textual and flat-json range descriptors; no parser SQL execution, catalog/storage lookup, external plugin call, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity or malformed descriptor refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select range_strictly_right(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_strictly_right |
| UUID | 019dffbb-f000-7952-998a-6734177d6c8d |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_range_strictly_right.v3 |
| AST binding | ast.expr.scalar_range_strictly_right |
| Engine entrypoint | range_strictly_right |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC035-range-strictly-right`.

### `range_upper`

**Purpose:** Returns the upper bound of a range.

**Call Forms:**

- `range_upper(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: one deterministic range descriptor.
- Coercion: range arguments accept deterministic bracket text like [lower,upper) or flat JSON descriptors with lower, upper, lower_inc, upper_inc, and empty fields; element comparison uses numeric ordering when both sides parse as finite numbers, otherwise lexical text ordering.
- NULL handling: SQL null input returns SQL null character.

**Returns:**

character textual upper bound or SQL null when the range is empty or unbounded above.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual bounds; boolean helpers return boolean descriptor.
- Timezone: not applicable.
- Security and authority: pure range scalar helper; accepts deterministic textual and flat-json range descriptors; no parser SQL execution, catalog/storage lookup, external plugin call, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity or malformed descriptor refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select range_upper(active_range) from app.time_windows;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_upper |
| UUID | 019dffbb-f000-7c9b-bab4-4579be8d3fde |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_range_upper.v3 |
| AST binding | ast.expr.scalar_range_upper |
| Engine entrypoint | range_upper |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC035-range-upper`.

### `range_upper_inc`

**Purpose:** Returns whether the upper range bound is inclusive.

**Call Forms:**

- `range_upper_inc(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: one deterministic range descriptor.
- Coercion: range arguments accept deterministic bracket text like [lower,upper) or flat JSON descriptors with lower, upper, lower_inc, upper_inc, and empty fields; element comparison uses numeric ordering when both sides parse as finite numbers, otherwise lexical text ordering.
- NULL handling: SQL null input returns SQL null boolean.

**Returns:**

boolean true when the upper bound is present and inclusive.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic.
- Side effects: none.
- Collation/charset: implementation default character descriptor for textual bounds; boolean helpers return boolean descriptor.
- Timezone: not applicable.
- Security and authority: pure range scalar helper; accepts deterministic textual and flat-json range descriptors; no parser SQL execution, catalog/storage lookup, external plugin call, transaction finality, recovery, or cluster behavior.

**Errors:**

invalid arity or malformed descriptor refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select range_upper_inc(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_upper_inc |
| UUID | 019dffbb-f000-75dc-bf43-b957d26590a9 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.scalar_range_upper_inc.v3 |
| AST binding | ast.expr.scalar_range_upper_inc |
| Engine entrypoint | range_upper_inc |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC035-range-upper-inc;SBSFC035-range-upper-inc-null`.

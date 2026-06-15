

===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/functional_reference/sb_temporal.md -->

<a id="ch-language-reference-functional-reference-sb-temporal-md"></a>

# SB Temporal Functional Reference

Generation task: `sb_temporal`

Package namespace: `sb.temporal`

Date, time, timestamp, interval, timezone, and temporal context helpers.

## How To Read This Page

Creates, extracts, converts, and reads temporal values using session and descriptor temporal rules.

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
| scalar | 39 |

## Operation Reference

### `add_months`

**Purpose:** Evaluates `add_months` and returns date shifted by whole months with end-of-month clamp semantics; descriptor=date.

**Call Forms:**

- `add_months(date,n)`
- Syntax category: `function_call`

**Parameters:**

- `date`: Bound using the declared descriptor rules for this overload.
- `n`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

date shifted by whole months with end-of-month clamp semantics; descriptor=date.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select add_months(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.add_months |
| UUID | 019dffbb-f001-7040-8a00-000000000040 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_add_months.v3 |
| AST binding | ast.expr.temporal_add_months |
| Engine entrypoint | temporal_add_months |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-add_months-clamp;SBSFC012-add_months-null;SBSFC012-add_months-pos`.

### `age`

**Purpose:** Evaluates `age` and returns interval text.

**Call Forms:**

- `age(timestamp[,timestamp])`
- Syntax category: `function_call`

**Parameters:**

- `timestamp[`: Bound using the declared descriptor rules for this overload.
- `timestamp]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: timestamp and optional comparison timestamp.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

interval text.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select age(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.age |
| UUID | 019dffbb-f000-7e4e-91a1-6f7ddbbfc8b4 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_age.v3 |
| AST binding | ast.expr.temporal_age |
| Engine entrypoint | age |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC058-age-signature`.

### `age_in_days`

**Purpose:** Evaluates `age_in_days` and returns int64 complete days.

**Call Forms:**

- `age_in_days(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: timestamp and optional comparison timestamp.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

int64 complete days.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select age_in_days(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.age_in_days |
| UUID | 019dffbb-f000-7ee9-a83b-1b127ee5d637 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_age_in_days.v3 |
| AST binding | ast.expr.temporal_age_in_days |
| Engine entrypoint | age_in_days |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC058-age-in-days`.

### `age_in_months`

**Purpose:** Evaluates `age_in_months` and returns int64 complete months.

**Call Forms:**

- `age_in_months(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: timestamp and optional comparison timestamp.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

int64 complete months.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select age_in_months(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.age_in_months |
| UUID | 019dffbb-f000-7b1f-9b1f-a783c31821ba |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_age_in_months.v3 |
| AST binding | ast.expr.temporal_age_in_months |
| Engine entrypoint | age_in_months |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC058-age-in-months`.

### `age_in_years`

**Purpose:** Evaluates `age_in_years` and returns int64 complete years.

**Call Forms:**

- `age_in_years(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: timestamp and optional comparison timestamp.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

int64 complete years.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select age_in_years(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.age_in_years |
| UUID | 019dffbb-f000-7f51-ad71-634cb7fb593c |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_age_in_years.v3 |
| AST binding | ast.expr.temporal_age_in_years |
| Engine entrypoint | age_in_years |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC058-age-in-years`.

### `clock_timestamp`

**Purpose:** Returns a volatile clock timestamp from the execution context.

**Call Forms:**

- `clock_timestamp()`
- Syntax category: `function_call`

**Parameters:**

- This function takes no arguments.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

current timestamp provider value from SBLR execution context; descriptor=timestamp_tz.

**Behavior:**

- Volatility: volatile.
- Determinism: not foldable; value is supplied by statement/session/transaction execution context or volatile time provider.
- Side effects: volatile_time_read.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: timestamp context read from SBLR execution context only.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select clock_timestamp() as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.clock_timestamp |
| UUID | 019dffbb-f000-7290-a2c2-a59b23103c09 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.clock_timestamp.v3 |
| AST binding | ast.expr.clock_timestamp |
| Engine entrypoint | clock_timestamp |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-clock-timestamp-context`.

### `current_date`

**Purpose:** Reads the current `date` value from the session or statement context.

**Call Forms:**

- `current_date`
- Syntax category: `keyword_or_function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless noted by function-specific semantics.

**Returns:**

current date in session timezone.

**Behavior:**

- Volatility: stable_per_transaction.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where string semantics apply.
- Timezone: session timezone for temporal forms; not applicable otherwise.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/overflow/domain errors use builtin error compatibility matrix.

**Example:**

```sql
select current_date(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.current_date |
| UUID | 019de5fc-2400-783b-adda-0bc068736641 |
| Kind | scalar |
| Syntax forms | keyword_or_function_call |
| SBLR binding | sblr.expr.temporal_current_date.v3 |
| AST binding | ast.expr.temporal_current_date |
| Engine entrypoint | temporal_current_date |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | cpu_scalar |

Conformance evidence: `BIF-CONF-3f4f0afa2a2c`.

### `current_time`

**Purpose:** Reads the current `time` value from the session or statement context.

**Call Forms:**

- `current_time[(precision)]`
- Syntax category: `keyword_or_function_call`

**Parameters:**

- `precision`: Bound using the declared descriptor rules for this overload.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless noted by function-specific semantics.

**Returns:**

current time in session timezone.

**Behavior:**

- Volatility: stable_per_transaction.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where string semantics apply.
- Timezone: session timezone for temporal forms; not applicable otherwise.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/overflow/domain errors use builtin error compatibility matrix.

**Example:**

```sql
select current_time(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.current_time |
| UUID | 019de5fc-2400-73da-80a1-68fe1d19a070 |
| Kind | scalar |
| Syntax forms | keyword_or_function_call |
| SBLR binding | sblr.expr.temporal_current_time.v3 |
| AST binding | ast.expr.temporal_current_time |
| Engine entrypoint | temporal_current_time |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | cpu_scalar |

Conformance evidence: `BIF-CONF-a678fa486fe5`.

### `current_timestamp`

**Purpose:** Returns the current statement or transaction timestamp according to the selected temporal form.

**Call Forms:**

- `current_timestamp[(precision)]`
- Syntax category: `keyword_or_function_call`

**Parameters:**

- `precision`: Bound using the declared descriptor rules for this overload.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless noted by function-specific semantics.

**Returns:**

transaction-stable timestamp by default; statement-stable only by external dialect profile.

**Behavior:**

- Volatility: stable_per_transaction.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where string semantics apply.
- Timezone: session timezone for temporal forms; not applicable otherwise.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/overflow/domain errors use builtin error compatibility matrix.

**Example:**

```sql
select current_timestamp(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.current_timestamp |
| UUID | 019de5fc-2400-7e69-bb23-613f117660a1 |
| Kind | scalar |
| Syntax forms | keyword_or_function_call |
| SBLR binding | sblr.expr.temporal_current_timestamp.v3 |
| AST binding | ast.expr.temporal_current_timestamp |
| Engine entrypoint | temporal_current_timestamp |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | cpu_scalar |

Conformance evidence: `BIF-CONF-ba41340ab776`.

### `date_add`

**Purpose:** Evaluates `date_add` and returns date or timestamp text.

**Call Forms:**

- `date_add(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: date or timestamp and ISO-like interval.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

date or timestamp text.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select date_add(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.date_add |
| UUID | 019dffbb-f000-72f5-8bd5-2bfc9336a0e3 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_date_add.v3 |
| AST binding | ast.expr.temporal_date_add |
| Engine entrypoint | date_add |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC058-date-add-bare`.

### `date_bin`

**Purpose:** Evaluates `date_bin` and returns timestamp text.

**Call Forms:**

- `date_bin(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: positive day/time stride, source timestamp, and origin timestamp.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

timestamp text.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select date_bin(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.date_bin |
| UUID | 019dffbb-f000-74d7-9498-1b5f90313b22 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_date_bin.v3 |
| AST binding | ast.expr.temporal_date_bin |
| Engine entrypoint | date_bin |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC058-date-bin-bare`.

### `date_diff`

**Purpose:** Evaluates `date_diff` and returns int64 difference.

**Call Forms:**

- `date_diff(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: part, start timestamp, and end timestamp.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

int64 difference.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select date_diff(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.date_diff |
| UUID | 019dffbb-f000-7043-9028-c46010757c70 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_date_diff.v3 |
| AST binding | ast.expr.temporal_date_diff |
| Engine entrypoint | date_diff |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC058-date-diff-bare`.

### `date_part`

**Purpose:** Evaluates `date_part` and returns extract temporal part as numeric.

**Call Forms:**

- `date_part(part,timestamp)`
- Syntax category: `function_call`

**Parameters:**

- `part`: Bound using the declared descriptor rules for this overload.
- `timestamp`: Bound using the declared descriptor rules for this overload.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless noted by function-specific semantics.

**Returns:**

extract temporal part as numeric.

**Behavior:**

- Volatility: immutable_or_timezone_stable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where string semantics apply.
- Timezone: session timezone for temporal forms; not applicable otherwise.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/overflow/domain errors use builtin error compatibility matrix.

**Example:**

```sql
select date_part(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.date_part |
| UUID | 019de5fc-2400-7156-bba9-1aa94c5a9826 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_date_part.v3 |
| AST binding | ast.expr.temporal_date_part |
| Engine entrypoint | temporal_date_part |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `BIF-CONF-27f7ecb4d7d5;SBSFC012-extract-epoch;SBSFC012-extract-generic-year;SBSFC012-extract-part-temporal;SBSFC012-special-extract-year`.

### `date_sub`

**Purpose:** Evaluates `date_sub` and returns date or timestamp text.

**Call Forms:**

- `date_sub(date\|timestamp,interval)`
- Syntax category: `function_call`

**Parameters:**

- `date\|timestamp`: Bound using the declared descriptor rules for this overload.
- `interval`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: date or timestamp and ISO-like interval.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the the conformance fixtures fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

date or timestamp text.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: UTC epoch arithmetic only for epoch/date_sub fixture rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal text, unsupported locale, or sequence increment errors use canonical SBLR diagnostics.

**Example:**

```sql
select date_sub(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.date_sub |
| UUID | 019dffbb-f000-7e61-bd15-cdcef4c6db03 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_date_sub.v3 |
| AST binding | ast.expr.temporal_date_sub |
| Engine entrypoint | date_sub |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC059-date-sub-signature`.

### `date_trunc`

**Purpose:** Evaluates `date_trunc` and returns truncate temporal value to part.

**Call Forms:**

- `date_trunc(part,timestamp)`
- Syntax category: `function_call`

**Parameters:**

- `part`: Bound using the declared descriptor rules for this overload.
- `timestamp`: Bound using the declared descriptor rules for this overload.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless noted by function-specific semantics.

**Returns:**

truncate temporal value to part.

**Behavior:**

- Volatility: immutable_or_timezone_stable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where string semantics apply.
- Timezone: session timezone for temporal forms; not applicable otherwise.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/overflow/domain errors use builtin error compatibility matrix.

**Example:**

```sql
select date_trunc(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.date_trunc |
| UUID | 019de5fc-2400-7b13-a963-4628df379b25 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_date_trunc.v3 |
| AST binding | ast.expr.temporal_date_trunc |
| Engine entrypoint | temporal_date_trunc |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `BIF-CONF-42a9dd60ab91`.

### `day_name`

**Purpose:** Evaluates `day_name` and returns character weekday name.

**Call Forms:**

- `day_name(date[,locale])`
- Syntax category: `function_call`

**Parameters:**

- `date[`: Bound using the declared descriptor rules for this overload.
- `locale]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: date and optional English locale.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

character weekday name.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select day_name(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.day_name |
| UUID | 019dffbb-f000-7f1f-a8cb-7121827fb5ba |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_day_name.v3 |
| AST binding | ast.expr.temporal_day_name |
| Engine entrypoint | day_name |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC058-day-name`.

### `dow`

**Purpose:** Evaluates `dow` and returns day-of-week number extracted from date/timestamp argument; descriptor=int64.

**Call Forms:**

- `dow(date)`
- Syntax category: `function_call`

**Parameters:**

- `date`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

day-of-week number extracted from date/timestamp argument; descriptor=int64.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select dow(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.dow |
| UUID | 019dffbb-f001-703b-8a00-00000000003b |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_dow.v3 |
| AST binding | ast.expr.temporal_dow |
| Engine entrypoint | temporal_dow |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-dow-bare-alias;SBSFC012-dow-null;SBSFC012-dow-pos`.

### `doy`

**Purpose:** Evaluates `doy` and returns day-of-year number extracted from date/timestamp argument; descriptor=int64.

**Call Forms:**

- `doy(date)`
- Syntax category: `function_call`

**Parameters:**

- `date`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

day-of-year number extracted from date/timestamp argument; descriptor=int64.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select doy(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.doy |
| UUID | 019dffbb-f001-703c-8a00-00000000003c |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_doy.v3 |
| AST binding | ast.expr.temporal_doy |
| Engine entrypoint | temporal_doy |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-doy-bare-alias;SBSFC012-doy-null;SBSFC012-doy-pos`.

### `epoch`

**Purpose:** Evaluates `epoch` and returns int64 Unix epoch seconds.

**Call Forms:**

- `epoch(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: one ISO-like temporal value.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

int64 Unix epoch seconds.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select epoch(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.epoch |
| UUID | 019dffbb-f000-7285-8cb0-cc140ac2e632 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_epoch.v3 |
| AST binding | ast.expr.temporal_epoch |
| Engine entrypoint | epoch |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC058-epoch`.

### `from_unixtime`

**Purpose:** Evaluates `from_unixtime` and returns timestamp_tz text.

**Call Forms:**

- `from_unixtime(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: int64 Unix epoch seconds.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

timestamp_tz text.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select from_unixtime(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.from_unixtime |
| UUID | 019dffbb-f000-7458-b42c-f33748dac341 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_from_unixtime.v3 |
| AST binding | ast.expr.temporal_from_unixtime |
| Engine entrypoint | from_unixtime |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC058-from-unixtime-bare`.

### `isodow`

**Purpose:** Evaluates `isodow` and returns iSO day-of-week number extracted from date/timestamp argument; descriptor=int64.

**Call Forms:**

- `isodow(date)`
- Syntax category: `function_call`

**Parameters:**

- `date`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

ISO day-of-week number extracted from date/timestamp argument; descriptor=int64.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select isodow(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.isodow |
| UUID | 019dffbb-f001-703e-8a00-00000000003e |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_isodow.v3 |
| AST binding | ast.expr.temporal_isodow |
| Engine entrypoint | temporal_isodow |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-isodow-bare-alias;SBSFC012-isodow-null;SBSFC012-isodow-pos`.

### `last_day`

**Purpose:** Evaluates `last_day` and returns last calendar day of the input month; descriptor=date.

**Call Forms:**

- `last_day(date)`
- Syntax category: `function_call`

**Parameters:**

- `date`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

last calendar day of the input month; descriptor=date.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select last_day(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.last_day |
| UUID | 019dffbb-f001-7041-8a00-000000000041 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_last_day.v3 |
| AST binding | ast.expr.temporal_last_day |
| Engine entrypoint | temporal_last_day |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-last_day-leap;SBSFC012-last_day-null;SBSFC012-last_day-pos`.

### `localtime`

**Purpose:** Returns the current local time value according to session temporal rules.

**Call Forms:**

- `localtime`
- Syntax category: `keyword_or_function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

local time value derived from current timestamp context; descriptor=time.

**Behavior:**

- Volatility: stable_per_statement.
- Determinism: not foldable; value is supplied by statement/session/transaction execution context or volatile time provider.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: timestamp context read from SBLR execution context only.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select localtime(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.localtime |
| UUID | 019dffbb-f000-7f3f-97d7-dfbe123123cd |
| Kind | scalar |
| Syntax forms | keyword_or_function_call |
| SBLR binding | sblr.expr.localtime.v3 |
| AST binding | ast.expr.localtime |
| Engine entrypoint | localtime |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-localtime-context`.

### `localtimestamp`

**Purpose:** Returns the current local timestamp value according to session temporal rules.

**Call Forms:**

- `localtimestamp`
- Syntax category: `keyword_or_function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

local timestamp value derived from current timestamp context without timezone suffix; descriptor=timestamp.

**Behavior:**

- Volatility: stable_per_statement.
- Determinism: not foldable; value is supplied by statement/session/transaction execution context or volatile time provider.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: timestamp context read from SBLR execution context only.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select localtimestamp(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.localtimestamp |
| UUID | 019dffbb-f000-76e6-a33a-9f40d998a302 |
| Kind | scalar |
| Syntax forms | keyword_or_function_call |
| SBLR binding | sblr.expr.localtimestamp.v3 |
| AST binding | ast.expr.localtimestamp |
| Engine entrypoint | localtimestamp |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-localtimestamp-context`.

### `make_date`

**Purpose:** Evaluates `make_date` and returns date constructed from year/month/day integer arguments; descriptor=date.

**Call Forms:**

- `make_date(year,month,day)`
- Syntax category: `function_call`

**Parameters:**

- `year`: Bound using the declared descriptor rules for this overload.
- `month`: Bound using the declared descriptor rules for this overload.
- `day`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

date constructed from year/month/day integer arguments; descriptor=date.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select make_date(value_1, value_2, value_3) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.make_date |
| UUID | 019dffbb-f001-7015-8a00-000000000015 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_make_date.v3 |
| AST binding | ast.expr.temporal_make_date |
| Engine entrypoint | temporal_make_date |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-make_date-null;SBSFC012-make_date-pos;SBSFC012-make_date-stale-signature`.

### `make_interval`

**Purpose:** Evaluates `make_interval` and returns interval text.

**Call Forms:**

- `make_interval([years[,months[,...]]])`
- Syntax category: `function_call`

**Parameters:**

- `[years[`: Bound using the declared descriptor rules for this overload.
- `months[`: Bound using the declared descriptor rules for this overload.
- `...]]]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: up to seven int64 interval components.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

interval text.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select make_interval(value_1, value_2, value_3) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.make_interval |
| UUID | 019dffbb-f000-7ac7-aef0-879a6f98ec79 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_make_interval.v3 |
| AST binding | ast.expr.temporal_make_interval |
| Engine entrypoint | make_interval |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC058-make-interval-signature`.

### `make_time`

**Purpose:** Evaluates `make_time` and returns time constructed from hour/minute/second integer arguments; descriptor=time.

**Call Forms:**

- `make_time(hour,minute,second)`
- Syntax category: `function_call`

**Parameters:**

- `hour`: Bound using the declared descriptor rules for this overload.
- `minute`: Bound using the declared descriptor rules for this overload.
- `second`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

time constructed from hour/minute/second integer arguments; descriptor=time.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select make_time(value_1, value_2, value_3) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.make_time |
| UUID | 019dffbb-f001-7016-8a00-000000000016 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_make_time.v3 |
| AST binding | ast.expr.temporal_make_time |
| Engine entrypoint | temporal_make_time |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-make_time-null;SBSFC012-make_time-pos;SBSFC012-make_time-stale-signature`.

### `make_timestamp`

**Purpose:** Evaluates `make_timestamp` and returns timestamp constructed from date/time or integer date-time parts; descriptor=timestamp.

**Call Forms:**

- `make_timestamp(year,month,day,hour,minute,second)`
- Syntax category: `function_call`

**Parameters:**

- `year`: Bound using the declared descriptor rules for this overload.
- `month`: Bound using the declared descriptor rules for this overload.
- `day`: Bound using the declared descriptor rules for this overload.
- `hour`: Bound using the declared descriptor rules for this overload.
- `minute`: Bound using the declared descriptor rules for this overload.
- `second`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

timestamp constructed from date/time or integer date-time parts; descriptor=timestamp.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select make_timestamp(value_1, value_2, value_3) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.make_timestamp |
| UUID | 019dffbb-f001-7017-8a00-000000000017 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_make_timestamp.v3 |
| AST binding | ast.expr.temporal_make_timestamp |
| Engine entrypoint | temporal_make_timestamp |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-make_timestamp-null;SBSFC012-make_timestamp-pos;SBSFC012-make_timestamp-stale-sixint;SBSFC012-make_timestamp-stale-sixint-arity;SBSFC012-make_timestamp-stale-sixint-null;SBSFC012-make_timestamp-stale-sixint-type`.

### `make_timestamptz`

**Purpose:** Evaluates `make_timestamptz` and returns timestamp with timezone constructed from date/time and optional timezone text; descriptor=timestamp_tz.

**Call Forms:**

- `make_timestamptz(...,timezone)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Bound using the declared descriptor rules for this overload.
- `timezone`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

timestamp with timezone constructed from date/time and optional timezone text; descriptor=timestamp_tz.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select make_timestamptz(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.make_timestamptz |
| UUID | 019dffbb-f001-7018-8a00-000000000018 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_make_timestamptz.v3 |
| AST binding | ast.expr.temporal_make_timestamptz |
| Engine entrypoint | temporal_make_timestamptz |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-make_timestamptz-default;SBSFC012-make_timestamptz-null;SBSFC012-make_timestamptz-with_tz`.

### `month_name`

**Purpose:** Evaluates `month_name` and returns character month name.

**Call Forms:**

- `month_name(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: date and optional English locale.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

character month name.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select month_name(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.month_name |
| UUID | 019dffbb-f000-701f-b179-c1df989dad05 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_month_name.v3 |
| AST binding | ast.expr.temporal_month_name |
| Engine entrypoint | month_name |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC058-month-name`.

### `months_between`

**Purpose:** Evaluates `months_between` and returns real64 month delta.

**Call Forms:**

- `months_between(value_1, value_2)`
- Syntax category: `function_call`

**Parameters:**

- `value_1`: Bound using the declared descriptor rules for this overload.
- `value_2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: two date values.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

real64 month delta.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select months_between(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.months_between |
| UUID | 019dffbb-f000-7d12-9208-a5c602a4a418 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_months_between.v3 |
| AST binding | ast.expr.temporal_months_between |
| Engine entrypoint | months_between |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC058-months-between-bare`.

### `next_day`

**Purpose:** Evaluates `next_day` and returns date text.

**Call Forms:**

- `next_day(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: date and English weekday.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

date text.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select next_day(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.next_day |
| UUID | 019dffbb-f000-7e72-a696-1c258d52a978 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_next_day.v3 |
| AST binding | ast.expr.temporal_next_day |
| Engine entrypoint | next_day |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC058-next-day-bare`.

### `now`

**Purpose:** Evaluates `now` and returns transaction-stable timestamp by default.

**Call Forms:**

- `now()`
- Syntax category: `function_call`

**Parameters:**

- This function takes no arguments.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless noted by function-specific semantics.

**Returns:**

transaction-stable timestamp by default.

**Behavior:**

- Volatility: stable_per_transaction.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where string semantics apply.
- Timezone: session timezone for temporal forms; not applicable otherwise.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/overflow/domain errors use builtin error compatibility matrix.

**Example:**

```sql
select now() as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.now |
| UUID | 019de5fc-2400-7654-add6-ca942f8e7000 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_now.v3 |
| AST binding | ast.expr.temporal_now |
| Engine entrypoint | temporal_now |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | cpu_scalar |

Conformance evidence: `BIF-CONF-981fa51fefda`.

### `quarter`

**Purpose:** Evaluates `quarter` and returns calendar quarter number extracted from date/timestamp argument; descriptor=int64.

**Call Forms:**

- `quarter(date)`
- Syntax category: `function_call`

**Parameters:**

- `date`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

calendar quarter number extracted from date/timestamp argument; descriptor=int64.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select quarter(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.quarter |
| UUID | 019dffbb-f001-703d-8a00-00000000003d |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_quarter.v3 |
| AST binding | ast.expr.temporal_quarter |
| Engine entrypoint | temporal_quarter |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-quarter-bare-alias;SBSFC012-quarter-null;SBSFC012-quarter-pos`.

### `statement_timestamp`

**Purpose:** Returns the timestamp captured for the current statement.

**Call Forms:**

- `statement_timestamp`
- Syntax category: `keyword_or_function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

statement-start timestamp from SBLR execution context; descriptor=timestamp_tz.

**Behavior:**

- Volatility: stable_per_statement.
- Determinism: not foldable; value is supplied by statement/session/transaction execution context or volatile time provider.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: timestamp context read from SBLR execution context only.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select statement_timestamp(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.statement_timestamp |
| UUID | 019dffbb-f000-75ff-bc93-ef6201c8284d |
| Kind | scalar |
| Syntax forms | keyword_or_function_call |
| SBLR binding | sblr.expr.statement_timestamp.v3 |
| AST binding | ast.expr.statement_timestamp |
| Engine entrypoint | statement_timestamp |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-statement-timestamp-context`.

### `timeofday`

**Purpose:** Evaluates `timeofday` and returns current timestamp provider value rendered as character text; descriptor=character.

**Call Forms:**

- `timeofday()`
- Syntax category: `function_call`

**Parameters:**

- This function takes no arguments.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

current timestamp provider value rendered as character text; descriptor=character.

**Behavior:**

- Volatility: volatile.
- Determinism: not foldable; value is supplied by statement/session/transaction execution context or volatile time provider.
- Side effects: volatile_time_read.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: timestamp context read from SBLR execution context only.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select timeofday() as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.timeofday |
| UUID | 019dffbb-f000-7878-bebd-292b497534e9 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.timeofday.v3 |
| AST binding | ast.expr.timeofday |
| Engine entrypoint | timeofday |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-timeofday-context`.

### `timezone`

**Purpose:** Evaluates `timezone` and returns timestamp_tz text.

**Call Forms:**

- `timezone(zone,timestamp)`
- Syntax category: `function_call`

**Parameters:**

- `zone`: Bound using the declared descriptor rules for this overload.
- `timestamp`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: UTC/GMT or numeric offset zone and timestamp.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

timestamp_tz text.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select timezone(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.timezone |
| UUID | 019dffbb-f000-70f8-b4bb-84a07686bdb7 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_timezone.v3 |
| AST binding | ast.expr.temporal_timezone |
| Engine entrypoint | timezone |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC058-timezone-signature`.

### `transaction_timestamp`

**Purpose:** Returns the timestamp captured for the current transaction.

**Call Forms:**

- `transaction_timestamp`
- Syntax category: `keyword_or_function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

transaction-start timestamp from SBLR execution context when transaction context is present; descriptor=timestamp_tz.

**Behavior:**

- Volatility: stable_per_transaction.
- Determinism: not foldable; value is supplied by statement/session/transaction execution context or volatile time provider.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: transaction timestamp context read from SBLR execution context only.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select transaction_timestamp(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.transaction_timestamp |
| UUID | 019dffbb-f000-7187-b4d0-c11a728bef26 |
| Kind | scalar |
| Syntax forms | keyword_or_function_call |
| SBLR binding | sblr.expr.transaction_timestamp.v3 |
| AST binding | ast.expr.transaction_timestamp |
| Engine entrypoint | transaction_timestamp |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-transaction-timestamp-context`.

### `week`

**Purpose:** Evaluates `week` and returns iSO week number extracted from date/timestamp argument; descriptor=int64.

**Call Forms:**

- `week(date)`
- Syntax category: `function_call`

**Parameters:**

- `date`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless context provider returns typed null as specified by the conformance fixtures fixture evidence.

**Returns:**

ISO week number extracted from date/timestamp argument; descriptor=int64.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where character semantics apply; otherwise not applicable.
- Timezone: session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/domain/context errors use builtin error compatibility matrix and the conformance fixtures exact fixture evidence.

**Example:**

```sql
select week(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.temporal.week |
| UUID | 019dffbb-f001-703f-8a00-00000000003f |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.temporal_week.v3 |
| AST binding | ast.expr.temporal_week |
| Engine entrypoint | temporal_week |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC012-week-bare-alias;SBSFC012-week-null;SBSFC012-week-pos`.


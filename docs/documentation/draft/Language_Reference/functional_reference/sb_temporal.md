# Sb Temporal Functional Reference

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `sb_temporal`


## Package Boundary

`sb.temporal` contains 39 registered built-in operation records. Each record binds a public function, operator, aggregate, window function, or special form to descriptor-aware overloads, null behavior, optimizer properties, SBLR binding, and engine entrypoint metadata.

The package boundary is semantic, not a privilege boundary. A caller still needs object privileges, expression admission, descriptor compatibility, and policy admission for the statement that uses the operation.

## Package Inventory

| Kind | Records |
| --- | --- |
| scalar | 39 |

## Type Mechanics

- Argument descriptors select the overload.
- Return descriptors come from the declared return type rule.
- Null, collation, charset, timezone, and security behavior are part of binding.
- Optimizer properties may allow constant folding, generated column use, or index eligibility, but execution authority remains with the engine.
- Practical forms below are canonical usage shapes derived from declared overload signatures; statement admission still depends on the surrounding query, object privileges, and descriptor binding.

## Operation Records

### `add_months`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.add_months |
| UUID | 019dffbb-f001-7040-8a00-000000000040 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | add_months(date,n) |
| Return Type Rule | date shifted by whole months with end-of-month clamp semantics; descriptor=date |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | immutable |
| Determinism | foldable when all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_add_months.v3 |
| AST Binding | ast.expr.temporal_add_months |
| Engine Entrypoint | temporal_add_months |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select add_months(temporal_value_1, arg_2) from app.sample_values;
```

### `age`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.age |
| UUID | 019dffbb-f000-7e4e-91a1-6f7ddbbfc8b4 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | age(...) |
| Return Type Rule | runtime-defined by engine entrypoint age |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.temporal_age.v3 |
| AST Binding | ast.expr.temporal_age |
| Engine Entrypoint | age |
| Security Policy | follows engine runtime seed registry authority for data.scalar |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select age(arg_1) from app.sample_values;
```

### `age_in_days`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.age_in_days |
| UUID | 019dffbb-f000-7ee9-a83b-1b127ee5d637 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | age_in_days(...) |
| Return Type Rule | runtime-defined by engine entrypoint age_in_days |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.temporal_age_in_days.v3 |
| AST Binding | ast.expr.temporal_age_in_days |
| Engine Entrypoint | age_in_days |
| Security Policy | follows engine runtime seed registry authority for data.scalar |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select age_in_days(arg_1) from app.sample_values;
```

### `age_in_months`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.age_in_months |
| UUID | 019dffbb-f000-7b1f-9b1f-a783c31821ba |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | age_in_months(...) |
| Return Type Rule | runtime-defined by engine entrypoint age_in_months |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.temporal_age_in_months.v3 |
| AST Binding | ast.expr.temporal_age_in_months |
| Engine Entrypoint | age_in_months |
| Security Policy | follows engine runtime seed registry authority for data.scalar |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select age_in_months(arg_1) from app.sample_values;
```

### `age_in_years`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.age_in_years |
| UUID | 019dffbb-f000-7f51-ad71-634cb7fb593c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | age_in_years(...) |
| Return Type Rule | runtime-defined by engine entrypoint age_in_years |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.temporal_age_in_years.v3 |
| AST Binding | ast.expr.temporal_age_in_years |
| Engine Entrypoint | age_in_years |
| Security Policy | follows engine runtime seed registry authority for data.scalar |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select age_in_years(arg_1) from app.sample_values;
```

### `clock_timestamp`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.clock_timestamp |
| UUID | 019dffbb-f000-7290-a2c2-a59b23103c09 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | clock_timestamp() |
| Return Type Rule | current timestamp provider value from SBLR execution context; descriptor=timestamp_tz |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | volatile |
| Determinism | not foldable; value is supplied by statement/session/transaction execution context or volatile time provider |
| Side Effects | volatile_time_read |
| SBLR Binding | sblr.expr.clock_timestamp.v3 |
| AST Binding | ast.expr.clock_timestamp |
| Engine Entrypoint | clock_timestamp |
| Security Policy | timestamp context read from SBLR execution context only |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select clock_timestamp() from app.sample_values;
```

### `current_date`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.current_date |
| UUID | 019de5fc-2400-783b-adda-0bc068736641 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | current_date |
| Return Type Rule | current date in session timezone |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | stable_per_transaction |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_current_date.v3 |
| AST Binding | ast.expr.temporal_current_date |
| Engine Entrypoint | temporal_current_date |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select current_date() from app.sample_values;
```

### `current_time`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.current_time |
| UUID | 019de5fc-2400-73da-80a1-68fe1d19a070 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | current_time[(precision)] |
| Return Type Rule | current time in session timezone |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | stable_per_transaction |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_current_time.v3 |
| AST Binding | ast.expr.temporal_current_time |
| Engine Entrypoint | temporal_current_time |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select current_time(arg_1) from app.sample_values;
```

### `current_timestamp`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.current_timestamp |
| UUID | 019de5fc-2400-7e69-bb23-613f117660a1 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | current_timestamp[(precision)] |
| Return Type Rule | transaction-stable timestamp by default; statement-stable only by donor profile |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | stable_per_transaction |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_current_timestamp.v3 |
| AST Binding | ast.expr.temporal_current_timestamp |
| Engine Entrypoint | temporal_current_timestamp |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select current_timestamp(arg_1) from app.sample_values;
```

### `date_add`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.date_add |
| UUID | 019dffbb-f000-72f5-8bd5-2bfc9336a0e3 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | date_add(...) |
| Return Type Rule | runtime-defined by engine entrypoint date_add |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.temporal_date_add.v3 |
| AST Binding | ast.expr.temporal_date_add |
| Engine Entrypoint | date_add |
| Security Policy | follows engine runtime seed registry authority for data.scalar |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select date_add(arg_1) from app.sample_values;
```

### `date_bin`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.date_bin |
| UUID | 019dffbb-f000-74d7-9498-1b5f90313b22 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | date_bin(...) |
| Return Type Rule | runtime-defined by engine entrypoint date_bin |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.temporal_date_bin.v3 |
| AST Binding | ast.expr.temporal_date_bin |
| Engine Entrypoint | date_bin |
| Security Policy | follows engine runtime seed registry authority for data.scalar |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select date_bin(arg_1) from app.sample_values;
```

### `date_diff`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.date_diff |
| UUID | 019dffbb-f000-7043-9028-c46010757c70 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | date_diff(...) |
| Return Type Rule | runtime-defined by engine entrypoint date_diff |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.temporal_date_diff.v3 |
| AST Binding | ast.expr.temporal_date_diff |
| Engine Entrypoint | date_diff |
| Security Policy | follows engine runtime seed registry authority for data.scalar |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select date_diff(arg_1) from app.sample_values;
```

### `date_part`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.date_part |
| UUID | 019de5fc-2400-7156-bba9-1aa94c5a9826 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | date_part(part,timestamp) |
| Return Type Rule | extract temporal part as numeric |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable_or_timezone_stable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_date_part.v3 |
| AST Binding | ast.expr.temporal_date_part |
| Engine Entrypoint | temporal_date_part |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select date_part(arg_1, temporal_value_2) from app.sample_values;
```

### `date_sub`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.date_sub |
| UUID | 019dffbb-f000-7e61-bd15-cdcef4c6db03 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | date_sub(...) |
| Return Type Rule | runtime-defined by engine entrypoint date_sub |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.temporal_date_sub.v3 |
| AST Binding | ast.expr.temporal_date_sub |
| Engine Entrypoint | date_sub |
| Security Policy | follows engine runtime seed registry authority for data.scalar |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select date_sub(arg_1) from app.sample_values;
```

### `date_trunc`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.date_trunc |
| UUID | 019de5fc-2400-7b13-a963-4628df379b25 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | date_trunc(part,timestamp) |
| Return Type Rule | truncate temporal value to part |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable_or_timezone_stable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_date_trunc.v3 |
| AST Binding | ast.expr.temporal_date_trunc |
| Engine Entrypoint | temporal_date_trunc |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select date_trunc(arg_1, temporal_value_2) from app.sample_values;
```

### `day_name`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.day_name |
| UUID | 019dffbb-f000-7f1f-a8cb-7121827fb5ba |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | day_name(...) |
| Return Type Rule | runtime-defined by engine entrypoint day_name |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.temporal_day_name.v3 |
| AST Binding | ast.expr.temporal_day_name |
| Engine Entrypoint | day_name |
| Security Policy | follows engine runtime seed registry authority for data.scalar |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select day_name(arg_1) from app.sample_values;
```

### `dow`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.dow |
| UUID | 019dffbb-f001-703b-8a00-00000000003b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | dow(date) |
| Return Type Rule | day-of-week number extracted from date/timestamp argument; descriptor=int64 |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | immutable |
| Determinism | foldable when all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_dow.v3 |
| AST Binding | ast.expr.temporal_dow |
| Engine Entrypoint | temporal_dow |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select dow(temporal_value_1) from app.sample_values;
```

### `doy`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.doy |
| UUID | 019dffbb-f001-703c-8a00-00000000003c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | doy(date) |
| Return Type Rule | day-of-year number extracted from date/timestamp argument; descriptor=int64 |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | immutable |
| Determinism | foldable when all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_doy.v3 |
| AST Binding | ast.expr.temporal_doy |
| Engine Entrypoint | temporal_doy |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select doy(temporal_value_1) from app.sample_values;
```

### `epoch`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.epoch |
| UUID | 019dffbb-f000-7285-8cb0-cc140ac2e632 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | epoch(...) |
| Return Type Rule | runtime-defined by engine entrypoint epoch |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.temporal_epoch.v3 |
| AST Binding | ast.expr.temporal_epoch |
| Engine Entrypoint | epoch |
| Security Policy | follows engine runtime seed registry authority for data.scalar |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select epoch(arg_1) from app.sample_values;
```

### `from_unixtime`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.from_unixtime |
| UUID | 019dffbb-f000-7458-b42c-f33748dac341 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | from_unixtime(...) |
| Return Type Rule | runtime-defined by engine entrypoint from_unixtime |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.temporal_from_unixtime.v3 |
| AST Binding | ast.expr.temporal_from_unixtime |
| Engine Entrypoint | from_unixtime |
| Security Policy | follows engine runtime seed registry authority for data.scalar |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select from_unixtime(arg_1) from app.sample_values;
```

### `isodow`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.isodow |
| UUID | 019dffbb-f001-703e-8a00-00000000003e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | isodow(date) |
| Return Type Rule | ISO day-of-week number extracted from date/timestamp argument; descriptor=int64 |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | immutable |
| Determinism | foldable when all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_isodow.v3 |
| AST Binding | ast.expr.temporal_isodow |
| Engine Entrypoint | temporal_isodow |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select isodow(temporal_value_1) from app.sample_values;
```

### `last_day`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.last_day |
| UUID | 019dffbb-f001-7041-8a00-000000000041 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | last_day(date) |
| Return Type Rule | last calendar day of the input month; descriptor=date |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | immutable |
| Determinism | foldable when all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_last_day.v3 |
| AST Binding | ast.expr.temporal_last_day |
| Engine Entrypoint | temporal_last_day |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select last_day(temporal_value_1) from app.sample_values;
```

### `localtime`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.localtime |
| UUID | 019dffbb-f000-7f3f-97d7-dfbe123123cd |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | localtime |
| Return Type Rule | local time value derived from current timestamp context; descriptor=time |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | stable_per_statement |
| Determinism | not foldable; value is supplied by statement/session/transaction execution context or volatile time provider |
| Side Effects | none |
| SBLR Binding | sblr.expr.localtime.v3 |
| AST Binding | ast.expr.localtime |
| Engine Entrypoint | localtime |
| Security Policy | timestamp context read from SBLR execution context only |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select localtime() from app.sample_values;
```

### `localtimestamp`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.localtimestamp |
| UUID | 019dffbb-f000-76e6-a33a-9f40d998a302 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | localtimestamp |
| Return Type Rule | local timestamp value derived from current timestamp context without timezone suffix; descriptor=timestamp |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | stable_per_statement |
| Determinism | not foldable; value is supplied by statement/session/transaction execution context or volatile time provider |
| Side Effects | none |
| SBLR Binding | sblr.expr.localtimestamp.v3 |
| AST Binding | ast.expr.localtimestamp |
| Engine Entrypoint | localtimestamp |
| Security Policy | timestamp context read from SBLR execution context only |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select localtimestamp() from app.sample_values;
```

### `make_date`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.make_date |
| UUID | 019dffbb-f001-7015-8a00-000000000015 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | make_date(year,month,day) |
| Return Type Rule | date constructed from year/month/day integer arguments; descriptor=date |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | immutable |
| Determinism | foldable when all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_make_date.v3 |
| AST Binding | ast.expr.temporal_make_date |
| Engine Entrypoint | temporal_make_date |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select make_date(arg_1, arg_2, arg_3) from app.sample_values;
```

### `make_interval`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.make_interval |
| UUID | 019dffbb-f000-7ac7-aef0-879a6f98ec79 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | make_interval(...) |
| Return Type Rule | runtime-defined by engine entrypoint make_interval |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.temporal_make_interval.v3 |
| AST Binding | ast.expr.temporal_make_interval |
| Engine Entrypoint | make_interval |
| Security Policy | follows engine runtime seed registry authority for data.scalar |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select make_interval(arg_1) from app.sample_values;
```

### `make_time`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.make_time |
| UUID | 019dffbb-f001-7016-8a00-000000000016 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | make_time(hour,minute,second) |
| Return Type Rule | time constructed from hour/minute/second integer arguments; descriptor=time |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | immutable |
| Determinism | foldable when all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_make_time.v3 |
| AST Binding | ast.expr.temporal_make_time |
| Engine Entrypoint | temporal_make_time |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select make_time(arg_1, arg_2, arg_3) from app.sample_values;
```

### `make_timestamp`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.make_timestamp |
| UUID | 019dffbb-f001-7017-8a00-000000000017 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | make_timestamp(year,month,day,hour,minute,second) |
| Return Type Rule | timestamp constructed from date/time or integer date-time parts; descriptor=timestamp |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | immutable |
| Determinism | foldable when all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_make_timestamp.v3 |
| AST Binding | ast.expr.temporal_make_timestamp |
| Engine Entrypoint | temporal_make_timestamp |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select make_timestamp(arg_1, arg_2, arg_3, arg_4, arg_5, arg_6) from app.sample_values;
```

### `make_timestamptz`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.make_timestamptz |
| UUID | 019dffbb-f001-7018-8a00-000000000018 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | make_timestamptz(...,timezone) |
| Return Type Rule | timestamp with timezone constructed from date/time and optional timezone text; descriptor=timestamp_tz |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | immutable |
| Determinism | foldable when all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_make_timestamptz.v3 |
| AST Binding | ast.expr.temporal_make_timestamptz |
| Engine Entrypoint | temporal_make_timestamptz |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select make_timestamptz(arg_1, temporal_value_2) from app.sample_values;
```

### `month_name`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.month_name |
| UUID | 019dffbb-f000-701f-b179-c1df989dad05 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | month_name(...) |
| Return Type Rule | runtime-defined by engine entrypoint month_name |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.temporal_month_name.v3 |
| AST Binding | ast.expr.temporal_month_name |
| Engine Entrypoint | month_name |
| Security Policy | follows engine runtime seed registry authority for data.scalar |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select month_name(arg_1) from app.sample_values;
```

### `months_between`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.months_between |
| UUID | 019dffbb-f000-7d12-9208-a5c602a4a418 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | months_between(...) |
| Return Type Rule | runtime-defined by engine entrypoint months_between |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.temporal_months_between.v3 |
| AST Binding | ast.expr.temporal_months_between |
| Engine Entrypoint | months_between |
| Security Policy | follows engine runtime seed registry authority for data.scalar |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select months_between(arg_1) from app.sample_values;
```

### `next_day`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.next_day |
| UUID | 019dffbb-f000-7e72-a696-1c258d52a978 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | next_day(...) |
| Return Type Rule | runtime-defined by engine entrypoint next_day |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.temporal_next_day.v3 |
| AST Binding | ast.expr.temporal_next_day |
| Engine Entrypoint | next_day |
| Security Policy | follows engine runtime seed registry authority for data.scalar |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select next_day(arg_1) from app.sample_values;
```

### `now`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.now |
| UUID | 019de5fc-2400-7654-add6-ca942f8e7000 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | now() |
| Return Type Rule | transaction-stable timestamp by default |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | stable_per_transaction |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_now.v3 |
| AST Binding | ast.expr.temporal_now |
| Engine Entrypoint | temporal_now |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select now() from app.sample_values;
```

### `quarter`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.quarter |
| UUID | 019dffbb-f001-703d-8a00-00000000003d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | quarter(date) |
| Return Type Rule | calendar quarter number extracted from date/timestamp argument; descriptor=int64 |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | immutable |
| Determinism | foldable when all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_quarter.v3 |
| AST Binding | ast.expr.temporal_quarter |
| Engine Entrypoint | temporal_quarter |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select quarter(temporal_value_1) from app.sample_values;
```

### `statement_timestamp`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.statement_timestamp |
| UUID | 019dffbb-f000-75ff-bc93-ef6201c8284d |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | statement_timestamp |
| Return Type Rule | statement-start timestamp from SBLR execution context; descriptor=timestamp_tz |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | stable_per_statement |
| Determinism | not foldable; value is supplied by statement/session/transaction execution context or volatile time provider |
| Side Effects | none |
| SBLR Binding | sblr.expr.statement_timestamp.v3 |
| AST Binding | ast.expr.statement_timestamp |
| Engine Entrypoint | statement_timestamp |
| Security Policy | timestamp context read from SBLR execution context only |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select statement_timestamp() from app.sample_values;
```

### `timeofday`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.timeofday |
| UUID | 019dffbb-f000-7878-bebd-292b497534e9 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | timeofday() |
| Return Type Rule | current timestamp provider value rendered as character text; descriptor=character |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | volatile |
| Determinism | not foldable; value is supplied by statement/session/transaction execution context or volatile time provider |
| Side Effects | volatile_time_read |
| SBLR Binding | sblr.expr.timeofday.v3 |
| AST Binding | ast.expr.timeofday |
| Engine Entrypoint | timeofday |
| Security Policy | timestamp context read from SBLR execution context only |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select timeofday() from app.sample_values;
```

### `timezone`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.timezone |
| UUID | 019dffbb-f000-70f8-b4bb-84a07686bdb7 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | timezone(...) |
| Return Type Rule | runtime-defined by engine entrypoint timezone |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.temporal_timezone.v3 |
| AST Binding | ast.expr.temporal_timezone |
| Engine Entrypoint | timezone |
| Security Policy | follows engine runtime seed registry authority for data.scalar |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select timezone(arg_1) from app.sample_values;
```

### `transaction_timestamp`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.transaction_timestamp |
| UUID | 019dffbb-f000-7187-b4d0-c11a728bef26 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | transaction_timestamp |
| Return Type Rule | transaction-start timestamp from SBLR execution context when transaction context is present; descriptor=timestamp_tz |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | stable_per_transaction |
| Determinism | not foldable; value is supplied by statement/session/transaction execution context or volatile time provider |
| Side Effects | none |
| SBLR Binding | sblr.expr.transaction_timestamp.v3 |
| AST Binding | ast.expr.transaction_timestamp |
| Engine Entrypoint | transaction_timestamp |
| Security Policy | transaction timestamp context read from SBLR execution context only |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select transaction_timestamp() from app.sample_values;
```

### `week`

| Property | Value |
| --- | --- |
| Builtin ID | sb.temporal.week |
| UUID | 019dffbb-f001-703f-8a00-00000000003f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | week(date) |
| Return Type Rule | ISO week number extracted from date/timestamp argument; descriptor=int64 |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | immutable |
| Determinism | foldable when all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.temporal_week.v3 |
| AST Binding | ast.expr.temporal_week |
| Engine Entrypoint | temporal_week |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain/context errors use builtin error compatibility matrix and SBSFC-012 exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select week(temporal_value_1) from app.sample_values;
```


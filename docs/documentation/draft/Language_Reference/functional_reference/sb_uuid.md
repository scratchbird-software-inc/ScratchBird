# Sb Uuid Functional Reference

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `sb_uuid`


## Package Boundary

`sb.uuid` contains 10 registered built-in operation records. Each record binds a public function, operator, aggregate, window function, or special form to descriptor-aware overloads, null behavior, optimizer properties, SBLR binding, and engine entrypoint metadata.

The package boundary is semantic, not a privilege boundary. A caller still needs object privileges, expression admission, descriptor compatibility, and policy admission for the statement that uses the operation.

## Package Inventory

| Kind | Records |
| --- | --- |
| scalar | 10 |

## Type Mechanics

- Argument descriptors select the overload.
- Return descriptors come from the declared return type rule.
- Null, collation, charset, timezone, and security behavior are part of binding.
- Optimizer properties may allow constant folding, generated column use, or index eligibility, but execution authority remains with the engine.
- Practical forms below are canonical usage shapes derived from declared overload signatures; statement admission still depends on the surrounding query, object privileges, and descriptor binding.

## Operation Records

### `uuid_generate_v1`

| Property | Value |
| --- | --- |
| Builtin ID | sb.uuid.generate_v1 |
| UUID | 019dffbb-f000-7ebb-8eec-060c8b4adbb9 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | uuid_generate_v1() |
| Return Type Rule | UUID v1-compatible value; descriptor=uuid |
| Coercion Rule | use descriptor implicit cast matrix; UUID text inputs must canonicalize before UUID helper semantics apply |
| Null Behavior | not applicable; nullary function |
| Collation/Charset Rule | uses byte-stable canonical UUID text where text parsing is required; otherwise not applicable |
| Timezone Rule | UTC timestamp_tz rendering for UUID timestamp extraction; otherwise not applicable |
| Volatility | volatile |
| Determinism | not foldable; generates a new UUID unless deterministic fixture context overrides |
| Side Effects | volatile_value_generation |
| SBLR Binding | sblr.expr.uuid_generate_v1.v3 |
| AST Binding | ast.expr.uuid_generate_v1 |
| Engine Entrypoint | uuid_generate_v1 |
| Security Policy | none; helper executes under SBLR descriptor authority and does not allocate engine identity unless explicitly used by caller context |
| Error Semantics | arity, invalid UUID text, unsupported UUID timestamp version, and domain errors use canonical UUID helper diagnostics from SBSFC-028 fixtures |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | volatile_scalar |

#### Practical Form

```sql
select uuid_generate_v1() from app.sample_values;
```

### `uuid_generate_v3`

| Property | Value |
| --- | --- |
| Builtin ID | sb.uuid.generate_v3 |
| UUID | 019dffbb-f000-765c-a462-4dac8737d001 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | uuid_generate_v3(namespace,name) |
| Return Type Rule | deterministic UUID v3 from namespace UUID and name; descriptor=uuid |
| Coercion Rule | use descriptor implicit cast matrix; UUID text inputs must canonicalize before UUID helper semantics apply |
| Null Behavior | strict; NULL namespace or name returns NULL uuid |
| Collation/Charset Rule | uses byte-stable canonical UUID text where text parsing is required; otherwise not applicable |
| Timezone Rule | UTC timestamp_tz rendering for UUID timestamp extraction; otherwise not applicable |
| Volatility | immutable |
| Determinism | foldable when namespace and name are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.uuid_generate_v3.v3 |
| AST Binding | ast.expr.uuid_generate_v3 |
| Engine Entrypoint | uuid_generate_v3 |
| Security Policy | none; helper executes under SBLR descriptor authority and does not allocate engine identity unless explicitly used by caller context |
| Error Semantics | arity, invalid UUID text, unsupported UUID timestamp version, and domain errors use canonical UUID helper diagnostics from SBSFC-028 fixtures |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select uuid_generate_v3(arg_1, arg_2) from app.sample_values;
```

### `uuid_generate_v4`

| Property | Value |
| --- | --- |
| Builtin ID | sb.uuid.generate_v4 |
| UUID | 019dffbb-f000-7e63-b855-587564821ba2 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | uuid_generate_v4() |
| Return Type Rule | random UUID v4 value; descriptor=uuid |
| Coercion Rule | use descriptor implicit cast matrix; UUID text inputs must canonicalize before UUID helper semantics apply |
| Null Behavior | not applicable; nullary function |
| Collation/Charset Rule | uses byte-stable canonical UUID text where text parsing is required; otherwise not applicable |
| Timezone Rule | UTC timestamp_tz rendering for UUID timestamp extraction; otherwise not applicable |
| Volatility | volatile |
| Determinism | not foldable; generates a new UUID unless deterministic fixture context overrides |
| Side Effects | volatile_value_generation |
| SBLR Binding | sblr.expr.uuid_generate_v4.v3 |
| AST Binding | ast.expr.uuid_generate_v4 |
| Engine Entrypoint | uuid_generate_v4 |
| Security Policy | none; helper executes under SBLR descriptor authority and does not allocate engine identity unless explicitly used by caller context |
| Error Semantics | arity, invalid UUID text, unsupported UUID timestamp version, and domain errors use canonical UUID helper diagnostics from SBSFC-028 fixtures |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | volatile_scalar |

#### Practical Form

```sql
select uuid_generate_v4() from app.sample_values;
```

### `uuid_generate_v5`

| Property | Value |
| --- | --- |
| Builtin ID | sb.uuid.generate_v5 |
| UUID | 019dffbb-f000-7f42-b1e9-618f013887b3 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | uuid_generate_v5(namespace,name) |
| Return Type Rule | deterministic UUID v5 from namespace UUID and name; descriptor=uuid |
| Coercion Rule | use descriptor implicit cast matrix; UUID text inputs must canonicalize before UUID helper semantics apply |
| Null Behavior | strict; NULL namespace or name returns NULL uuid |
| Collation/Charset Rule | uses byte-stable canonical UUID text where text parsing is required; otherwise not applicable |
| Timezone Rule | UTC timestamp_tz rendering for UUID timestamp extraction; otherwise not applicable |
| Volatility | immutable |
| Determinism | foldable when namespace and name are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.uuid_generate_v5.v3 |
| AST Binding | ast.expr.uuid_generate_v5 |
| Engine Entrypoint | uuid_generate_v5 |
| Security Policy | none; helper executes under SBLR descriptor authority and does not allocate engine identity unless explicitly used by caller context |
| Error Semantics | arity, invalid UUID text, unsupported UUID timestamp version, and domain errors use canonical UUID helper diagnostics from SBSFC-028 fixtures |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select uuid_generate_v5(arg_1, arg_2) from app.sample_values;
```

### `uuid_nil`

| Property | Value |
| --- | --- |
| Builtin ID | sb.uuid.nil |
| UUID | 019dffbb-f000-761e-9280-aefd19556637 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | uuid_nil() |
| Return Type Rule | nil UUID constant; descriptor=uuid |
| Coercion Rule | use descriptor implicit cast matrix; UUID text inputs must canonicalize before UUID helper semantics apply |
| Null Behavior | not applicable; nullary function |
| Collation/Charset Rule | uses byte-stable canonical UUID text where text parsing is required; otherwise not applicable |
| Timezone Rule | UTC timestamp_tz rendering for UUID timestamp extraction; otherwise not applicable |
| Volatility | immutable |
| Determinism | foldable; returns a constant nil UUID |
| Side Effects | none |
| SBLR Binding | sblr.expr.uuid_nil.v3 |
| AST Binding | ast.expr.uuid_nil |
| Engine Entrypoint | uuid_nil |
| Security Policy | none; helper executes under SBLR descriptor authority and does not allocate engine identity unless explicitly used by caller context |
| Error Semantics | arity, invalid UUID text, unsupported UUID timestamp version, and domain errors use canonical UUID helper diagnostics from SBSFC-028 fixtures |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select uuid_nil() from app.sample_values;
```

### `uuid_timestamp`

| Property | Value |
| --- | --- |
| Builtin ID | sb.uuid.timestamp |
| UUID | 019dffbb-f000-7184-bffb-d4c74e43d38b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | uuid_timestamp(uuid) |
| Return Type Rule | timestamp_tz extracted from UUID v1 or v7 embedded timestamp |
| Coercion Rule | use descriptor implicit cast matrix; UUID text inputs must canonicalize before UUID helper semantics apply |
| Null Behavior | strict; NULL UUID returns NULL timestamp_tz |
| Collation/Charset Rule | uses byte-stable canonical UUID text where text parsing is required; otherwise not applicable |
| Timezone Rule | UTC timestamp_tz rendering for UUID timestamp extraction; otherwise not applicable |
| Volatility | immutable |
| Determinism | foldable when UUID argument is constant with stable descriptor |
| Side Effects | none |
| SBLR Binding | sblr.expr.uuid_timestamp.v3 |
| AST Binding | ast.expr.uuid_timestamp |
| Engine Entrypoint | uuid_timestamp |
| Security Policy | none; helper executes under SBLR descriptor authority and does not allocate engine identity unless explicitly used by caller context |
| Error Semantics | arity, invalid UUID text, unsupported UUID timestamp version, and domain errors use canonical UUID helper diagnostics from SBSFC-028 fixtures |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select uuid_timestamp(uuid_value_1) from app.sample_values;
```

### `uuid_v1`

| Property | Value |
| --- | --- |
| Builtin ID | sb.uuid.v1 |
| UUID | 019de5fc-2400-7feb-b0a9-3353de7091ed |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | uuid_v1() |
| Return Type Rule | compatibility UUID generator; not engine identity |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous SBsql coercion unless SBsql policy allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | volatile |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | volatile_value_generation |
| SBLR Binding | sblr.expr.uuid_v1.v3 |
| AST Binding | ast.expr.uuid_v1 |
| Engine Entrypoint | uuid_v1 |
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
select uuid_v1() from app.sample_values;
```

### `uuid_v4`

| Property | Value |
| --- | --- |
| Builtin ID | sb.uuid.v4 |
| UUID | 019de5fc-2400-7940-8ecf-0c79114eb869 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | uuid_v4() |
| Return Type Rule | random UUID generator; not engine identity |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous SBsql coercion unless SBsql policy allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | volatile |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | volatile_value_generation |
| SBLR Binding | sblr.expr.uuid_v4.v3 |
| AST Binding | ast.expr.uuid_v4 |
| Engine Entrypoint | uuid_v4 |
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
select uuid_v4() from app.sample_values;
```

### `uuid_v7`

| Property | Value |
| --- | --- |
| Builtin ID | sb.uuid.v7 |
| UUID | 019de5fc-2400-7fe3-a3d5-21174bd948db |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | uuid_v7() |
| Return Type Rule | engine identity UUID generator; v7 only for engine identities |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous SBsql coercion unless SBsql policy allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | volatile |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | volatile_value_generation |
| SBLR Binding | sblr.expr.uuid_v7.v3 |
| AST Binding | ast.expr.uuid_v7 |
| Engine Entrypoint | uuid_v7 |
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
select uuid_v7() from app.sample_values;
```

### `uuid_version`

| Property | Value |
| --- | --- |
| Builtin ID | sb.uuid.version |
| UUID | 019dffbb-f000-773e-929d-76fcd97ac981 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | uuid_version(uuid) |
| Return Type Rule | UUID version nibble as int64; nil UUID returns 0 |
| Coercion Rule | use descriptor implicit cast matrix; UUID text inputs must canonicalize before UUID helper semantics apply |
| Null Behavior | strict; NULL UUID returns NULL int64 |
| Collation/Charset Rule | uses byte-stable canonical UUID text where text parsing is required; otherwise not applicable |
| Timezone Rule | UTC timestamp_tz rendering for UUID timestamp extraction; otherwise not applicable |
| Volatility | immutable |
| Determinism | foldable when UUID argument is constant with stable descriptor |
| Side Effects | none |
| SBLR Binding | sblr.expr.uuid_version.v3 |
| AST Binding | ast.expr.uuid_version |
| Engine Entrypoint | uuid_version |
| Security Policy | none; helper executes under SBLR descriptor authority and does not allocate engine identity unless explicitly used by caller context |
| Error Semantics | arity, invalid UUID text, unsupported UUID timestamp version, and domain errors use canonical UUID helper diagnostics from SBSFC-028 fixtures |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select uuid_version(uuid_value_1) from app.sample_values;
```


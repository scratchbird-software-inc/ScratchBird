# Sb Core Functional Reference

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `sb_core`


## Package Boundary

`sb.core` contains 661 registered built-in operation records. Each record binds a public function, operator, aggregate, window function, or special form to descriptor-aware overloads, null behavior, optimizer properties, SBLR binding, and engine entrypoint metadata.

The package boundary is semantic, not a privilege boundary. A caller still needs object privileges, expression admission, descriptor compatibility, and policy admission for the statement that uses the operation.

## Package Inventory

| Kind | Records |
| --- | --- |
| aggregate | 86 |
| expression_descriptor | 1 |
| operator | 22 |
| scalar | 530 |
| special_form | 10 |
| type_descriptor | 1 |
| window | 11 |

## Type Mechanics

- Argument descriptors select the overload.
- Return descriptors come from the declared return type rule.
- Null, collation, charset, timezone, and security behavior are part of binding.
- Optimizer properties may allow constant folding, generated column use, or index eligibility, but execution authority remains with the engine.
- Practical forms below are canonical usage shapes derived from declared overload signatures; statement admission still depends on the surrounding query, object privileges, and descriptor binding.

## Operation Records

### `any_value`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.any_value |
| UUID | 019dffbb-f000-7b3c-a6a7-fa6778f3e107 |
| Kind | aggregate |
| Syntax Forms | function_call |
| Overloads | any_value(...) |
| Return Type Rule | runtime-defined by engine entrypoint any_value |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.aggregate_any_value.v3 |
| AST Binding | ast.expr.aggregate_any_value |
| Engine Entrypoint | any_value |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select any_value(arg_1) from app.orders group by account_id;
```

### `any_value_expr`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.any_value_expr |
| UUID | 019dffbb-f000-739f-b4b5-7845010a088c |
| Kind | aggregate |
| Syntax Forms | function_call |
| Overloads | any_value_expr(...) |
| Return Type Rule | runtime-defined by engine entrypoint any_value_expr |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.aggregate_any_value_expr.v3 |
| AST Binding | ast.expr.aggregate_any_value_expr |
| Engine Entrypoint | any_value_expr |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select any_value_expr(arg_1) from app.orders group by account_id;
```

### `approx_count_distinct`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.approx_count_distinct |
| UUID | 019dffbb-f000-7736-96f3-e20cbd532ba5 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | approx_count_distinct(...) |
| Return Type Rule | descriptor-authoritative approx_count_distinct aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_approx_count_distinct.v3 |
| AST Binding | ast.aggregate.aggregate_approx_count_distinct |
| Engine Entrypoint | aggregate_approx_count_distinct |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select approx_count_distinct(arg_1) from app.orders group by account_id;
```

### `approx_count_distinct`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.approx_count_distinct |
| UUID | 019dffbb-f000-7736-96f3-e20cbd532ba5 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | approx_count_distinct(...) |
| Return Type Rule | descriptor-authoritative approx_count_distinct aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_approx_count_distinct.v3 |
| AST Binding | ast.aggregate.aggregate_approx_count_distinct |
| Engine Entrypoint | aggregate_approx_count_distinct |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select approx_count_distinct(arg_1) from app.orders group by account_id;
```

### `approx_median`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.approx_median |
| UUID | 019dffbb-f000-7ce0-85a6-cbcd71f2c86e |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | approx_median(...) |
| Return Type Rule | descriptor-authoritative approx_median aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_approx_median.v3 |
| AST Binding | ast.aggregate.aggregate_approx_median |
| Engine Entrypoint | aggregate_approx_median |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select approx_median(arg_1) from app.orders group by account_id;
```

### `approx_median`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.approx_median |
| UUID | 019dffbb-f000-7ce0-85a6-cbcd71f2c86e |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | approx_median(...) |
| Return Type Rule | descriptor-authoritative approx_median aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_approx_median.v3 |
| AST Binding | ast.aggregate.aggregate_approx_median |
| Engine Entrypoint | aggregate_approx_median |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select approx_median(arg_1) from app.orders group by account_id;
```

### `approx_percentile_cont`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.approx_percentile_cont |
| UUID | 019dffbb-f000-76df-98a6-aa77d1a342f8 |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | approx_percentile_cont(...) |
| Return Type Rule | descriptor-authoritative approx_percentile_cont aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_approx_percentile_cont.v3 |
| AST Binding | ast.aggregate.aggregate_approx_percentile_cont |
| Engine Entrypoint | aggregate_approx_percentile_cont |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select approx_percentile_cont(arg_1) from app.orders group by account_id;
```

### `approx_percentile_cont`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.approx_percentile_cont |
| UUID | 019dffbb-f000-76df-98a6-aa77d1a342f8 |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | approx_percentile_cont(...) |
| Return Type Rule | descriptor-authoritative approx_percentile_cont aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_approx_percentile_cont.v3 |
| AST Binding | ast.aggregate.aggregate_approx_percentile_cont |
| Engine Entrypoint | aggregate_approx_percentile_cont |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select approx_percentile_cont(arg_1) from app.orders group by account_id;
```

### `approx_percentile_disc`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.approx_percentile_disc |
| UUID | 019dffbb-f000-7578-a88f-8db4bb649755 |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | approx_percentile_disc(...) |
| Return Type Rule | descriptor-authoritative approx_percentile_disc aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_approx_percentile_disc.v3 |
| AST Binding | ast.aggregate.aggregate_approx_percentile_disc |
| Engine Entrypoint | aggregate_approx_percentile_disc |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select approx_percentile_disc(arg_1) from app.orders group by account_id;
```

### `approx_percentile_disc`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.approx_percentile_disc |
| UUID | 019dffbb-f000-7578-a88f-8db4bb649755 |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | approx_percentile_disc(...) |
| Return Type Rule | descriptor-authoritative approx_percentile_disc aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_approx_percentile_disc.v3 |
| AST Binding | ast.aggregate.aggregate_approx_percentile_disc |
| Engine Entrypoint | aggregate_approx_percentile_disc |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select approx_percentile_disc(arg_1) from app.orders group by account_id;
```

### `approx_top_k`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.approx_top_k |
| UUID | 019dffbb-f000-7f47-8fe1-0c5e0ec87bf0 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | approx_top_k(...) |
| Return Type Rule | descriptor-authoritative approx_top_k aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_approx_top_k.v3 |
| AST Binding | ast.aggregate.aggregate_approx_top_k |
| Engine Entrypoint | aggregate_approx_top_k |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select approx_top_k(arg_1) from app.orders group by account_id;
```

### `approx_top_k`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.approx_top_k |
| UUID | 019dffbb-f000-7f47-8fe1-0c5e0ec87bf0 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | approx_top_k(...) |
| Return Type Rule | descriptor-authoritative approx_top_k aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_approx_top_k.v3 |
| AST Binding | ast.aggregate.aggregate_approx_top_k |
| Engine Entrypoint | aggregate_approx_top_k |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select approx_top_k(arg_1) from app.orders group by account_id;
```

### `array_agg`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.array_agg |
| UUID | 019dffbb-f000-7f17-8318-92065f8146e5 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | array_agg(...) |
| Return Type Rule | descriptor-authoritative array_agg aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_array_agg.v3 |
| AST Binding | ast.aggregate.aggregate_array_agg |
| Engine Entrypoint | aggregate_array_agg |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select array_agg(arg_1) from app.orders group by account_id;
```

### `array_agg`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.array_agg |
| UUID | 019de5fc-2400-7159-9f7b-915513b8c0d4 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | array_agg(expr ORDER BY expr) |
| Return Type Rule | list/array value; empty groups return NULL list |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | NULL inputs are included as NULL array/list elements; empty groups return NULL list |
| Collation/Charset Rule | uses descriptor collation for comparable/string states |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic only for parser-provided ordered input sequence |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_array_agg.v3 |
| AST Binding | ast.aggregate.aggregate_array_agg |
| Engine Entrypoint | aggregate_array_agg |
| Security Policy | inherits containing query rights |
| Error Semantics | DISTINCT, FILTER, window bridge, WITHIN GROUP, unordered grouped-route spelling, broad ORDER BY modifiers, and generic aggregate options are refused by the current bounded route before SBLR payload authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select array_agg(value_1) from app.orders group by account_id;
```

### `avg`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.avg |
| UUID | 019dffbb-f000-7fd3-b228-03bf40871b10 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | avg(...) |
| Return Type Rule | descriptor-authoritative avg aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_avg.v3 |
| AST Binding | ast.aggregate.aggregate_avg |
| Engine Entrypoint | aggregate_avg |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select avg(arg_1) from app.orders group by account_id;
```

### `avg`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.avg |
| UUID | 019de5fc-2400-78ac-b50c-45b832831004 |
| Kind | aggregate |
| Syntax Forms | aggregate_call, aggregate_filter, aggregate_order_by_when_allowed, aggregate_distinct_when_allowed |
| Overloads | avg(expr) |
| Return Type Rule | sum/count state with exact descriptor-specific return rule |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | ignores_null |
| Collation/Charset Rule | uses descriptor collation for comparable/string states |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | stable_per_group |
| Determinism | deterministic for deterministic input order; order-sensitive aggregates require ORDER BY for deterministic output |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_avg.v3 |
| AST Binding | ast.aggregate.aggregate_avg |
| Engine Entrypoint | aggregate_avg |
| Security Policy | inherits containing query rights |
| Error Semantics | see aggregate error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select avg(value_1) from app.orders group by account_id;
```

### `bool_and`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.bool_and |
| UUID | 019dffbb-f000-7e8f-bb98-ab35bc0bbeb6 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | bool_and(...) |
| Return Type Rule | descriptor-authoritative bool_and aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_bool_and.v3 |
| AST Binding | ast.aggregate.aggregate_bool_and |
| Engine Entrypoint | aggregate_bool_and |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select bool_and(arg_1) from app.orders group by account_id;
```

### `bool_and`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.bool_and |
| UUID | 019de5fc-2400-78b0-ad98-a681e93b4c49 |
| Kind | aggregate |
| Syntax Forms | aggregate_call, aggregate_filter, aggregate_order_by_when_allowed, aggregate_distinct_when_allowed |
| Overloads | bool_and(expr) |
| Return Type Rule | three-valued boolean aggregate |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | ignores_null |
| Collation/Charset Rule | uses descriptor collation for comparable/string states |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | stable_per_group |
| Determinism | deterministic for deterministic input order; order-sensitive aggregates require ORDER BY for deterministic output |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_bool_and.v3 |
| AST Binding | ast.aggregate.aggregate_bool_and |
| Engine Entrypoint | aggregate_bool_and |
| Security Policy | inherits containing query rights |
| Error Semantics | see aggregate error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select bool_and(value_1) from app.orders group by account_id;
```

### `bool_or`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.bool_or |
| UUID | 019dffbb-f000-78c5-9c0b-7d9685f9b4ad |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | bool_or(...) |
| Return Type Rule | descriptor-authoritative bool_or aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_bool_or.v3 |
| AST Binding | ast.aggregate.aggregate_bool_or |
| Engine Entrypoint | aggregate_bool_or |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select bool_or(arg_1) from app.orders group by account_id;
```

### `bool_or`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.bool_or |
| UUID | 019de5fc-2400-7c2a-a3f2-e4b9d36df403 |
| Kind | aggregate |
| Syntax Forms | aggregate_call, aggregate_filter, aggregate_order_by_when_allowed, aggregate_distinct_when_allowed |
| Overloads | bool_or(expr) |
| Return Type Rule | three-valued boolean aggregate |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | ignores_null |
| Collation/Charset Rule | uses descriptor collation for comparable/string states |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | stable_per_group |
| Determinism | deterministic for deterministic input order; order-sensitive aggregates require ORDER BY for deterministic output |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_bool_or.v3 |
| AST Binding | ast.aggregate.aggregate_bool_or |
| Engine Entrypoint | aggregate_bool_or |
| Security Policy | inherits containing query rights |
| Error Semantics | see aggregate error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select bool_or(value_1) from app.orders group by account_id;
```

### `collect`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.collect |
| UUID | 019dffbb-f000-761a-9f4d-ebdc4604f3d7 |
| Kind | aggregate |
| Syntax Forms | function_call |
| Overloads | collect(...) |
| Return Type Rule | runtime-defined by engine entrypoint collect |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.aggregate_collect.v3 |
| AST Binding | ast.expr.aggregate_collect |
| Engine Entrypoint | collect |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select collect(arg_1) from app.orders group by account_id;
```

### `collect_expr`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.collect_expr |
| UUID | 019dffbb-f000-7ebf-842f-2d93584f078f |
| Kind | aggregate |
| Syntax Forms | function_call |
| Overloads | collect_expr(...) |
| Return Type Rule | runtime-defined by engine entrypoint collect_expr |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.aggregate_collect_expr.v3 |
| AST Binding | ast.expr.aggregate_collect_expr |
| Engine Entrypoint | collect_expr |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select collect_expr(arg_1) from app.orders group by account_id;
```

### `corr`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.corr |
| UUID | 019dffbb-f000-77bb-ba9b-2e78acf84521 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | corr(...) |
| Return Type Rule | descriptor-authoritative corr aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_corr.v3 |
| AST Binding | ast.aggregate.aggregate_corr |
| Engine Entrypoint | aggregate_corr |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select corr(arg_1) from app.orders group by account_id;
```

### `corr`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.corr |
| UUID | 019dffbb-f000-77bb-ba9b-2e78acf84521 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | corr(...) |
| Return Type Rule | descriptor-authoritative corr aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_corr.v3 |
| AST Binding | ast.aggregate.aggregate_corr |
| Engine Entrypoint | aggregate_corr |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select corr(arg_1) from app.orders group by account_id;
```

### `count`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.count |
| UUID | 019dffbb-f000-7613-a71e-84b03ef18e1d |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | count(...) |
| Return Type Rule | descriptor-authoritative count aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_count.v3 |
| AST Binding | ast.aggregate.aggregate_count |
| Engine Entrypoint | aggregate_count |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select count(arg_1) from app.orders group by account_id;
```

### `count`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.count |
| UUID | 019de5fc-2400-784a-9aec-371f8b95b7ea |
| Kind | aggregate |
| Syntax Forms | aggregate_call, aggregate_filter, aggregate_order_by_when_allowed, aggregate_distinct_when_allowed |
| Overloads | count(*) or count(expr) |
| Return Type Rule | int128 count accumulator unless donor profile requires int64 rendering |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | ignores_null_except_star |
| Collation/Charset Rule | uses descriptor collation for comparable/string states |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | stable_per_group |
| Determinism | deterministic for deterministic input order; order-sensitive aggregates require ORDER BY for deterministic output |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_count.v3 |
| AST Binding | ast.aggregate.aggregate_count |
| Engine Entrypoint | aggregate_count |
| Security Policy | inherits containing query rights |
| Error Semantics | see aggregate error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select count(*) from app.orders group by account_id;
```

### `covar_pop`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.covar_pop |
| UUID | 019dffbb-f001-7048-8a00-000000000048 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | covar_pop(...) |
| Return Type Rule | descriptor-authoritative covar_pop aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_covar_pop.v3 |
| AST Binding | ast.aggregate.aggregate_covar_pop |
| Engine Entrypoint | aggregate_covar_pop |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select covar_pop(arg_1) from app.orders group by account_id;
```

### `covar_pop`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.covar_pop |
| UUID | 019dffbb-f000-7f09-8ceb-17ad4c70e99f |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | covar_pop(...) |
| Return Type Rule | descriptor-authoritative covar_pop aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_covar_pop.v3 |
| AST Binding | ast.aggregate.aggregate_covar_pop |
| Engine Entrypoint | aggregate_covar_pop |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select covar_pop(arg_1) from app.orders group by account_id;
```

### `covar_samp`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.covar_samp |
| UUID | 019dffbb-f001-7049-8a00-000000000049 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | covar_samp(...) |
| Return Type Rule | descriptor-authoritative covar_samp aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_covar_samp.v3 |
| AST Binding | ast.aggregate.aggregate_covar_samp |
| Engine Entrypoint | aggregate_covar_samp |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select covar_samp(arg_1) from app.orders group by account_id;
```

### `covar_samp`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.covar_samp |
| UUID | 019dffbb-f000-747d-bc01-caad9137d070 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | covar_samp(...) |
| Return Type Rule | descriptor-authoritative covar_samp aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_covar_samp.v3 |
| AST Binding | ast.aggregate.aggregate_covar_samp |
| Engine Entrypoint | aggregate_covar_samp |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select covar_samp(arg_1) from app.orders group by account_id;
```

### `cume_dist`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.cume_dist |
| UUID | 019dffbb-f000-7244-89fd-8fa66ae930d5 |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | cume_dist(...) |
| Return Type Rule | descriptor-authoritative cume_dist aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_hypothetical_cume_dist.v3 |
| AST Binding | ast.aggregate.aggregate_cume_dist |
| Engine Entrypoint | aggregate_hypothetical_cume_dist |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select cume_dist(arg_1) from app.orders group by account_id;
```

### `cume_dist`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.cume_dist |
| UUID | 019dffbb-f000-7244-89fd-8fa66ae930d5 |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | cume_dist(...) |
| Return Type Rule | descriptor-authoritative cume_dist aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_hypothetical_cume_dist.v3 |
| AST Binding | ast.aggregate.aggregate_cume_dist |
| Engine Entrypoint | aggregate_hypothetical_cume_dist |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select cume_dist(arg_1) from app.orders group by account_id;
```

### `dense_rank`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.dense_rank |
| UUID | 019dffbb-f000-7bd3-a731-1734581eb8ce |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | dense_rank(...) |
| Return Type Rule | descriptor-authoritative dense_rank aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_hypothetical_dense_rank.v3 |
| AST Binding | ast.aggregate.aggregate_dense_rank |
| Engine Entrypoint | aggregate_hypothetical_dense_rank |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select dense_rank(arg_1) from app.orders group by account_id;
```

### `dense_rank`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.dense_rank |
| UUID | 019dffbb-f000-7bd3-a731-1734581eb8ce |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | dense_rank(...) |
| Return Type Rule | descriptor-authoritative dense_rank aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_hypothetical_dense_rank.v3 |
| AST Binding | ast.aggregate.aggregate_dense_rank |
| Engine Entrypoint | aggregate_hypothetical_dense_rank |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select dense_rank(arg_1) from app.orders group by account_id;
```

### `every`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.every |
| UUID | 019dffbb-f000-7876-9644-ae83b363d3bc |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | every(...) |
| Return Type Rule | descriptor-authoritative every aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_every.v3 |
| AST Binding | ast.aggregate.aggregate_every |
| Engine Entrypoint | aggregate_every |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select every(arg_1) from app.orders group by account_id;
```

### `every`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.every |
| UUID | 019dffbb-f000-7876-9644-ae83b363d3bc |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | every(...) |
| Return Type Rule | descriptor-authoritative every aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_every.v3 |
| AST Binding | ast.aggregate.aggregate_every |
| Engine Entrypoint | aggregate_every |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select every(arg_1) from app.orders group by account_id;
```

### `listagg`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.listagg |
| UUID | 019dffbb-f000-7e93-8e4d-6063849de049 |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | listagg(...) |
| Return Type Rule | descriptor-authoritative listagg aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_listagg.v3 |
| AST Binding | ast.aggregate.aggregate_listagg |
| Engine Entrypoint | aggregate_listagg |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select listagg(arg_1) from app.orders group by account_id;
```

### `listagg`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.listagg |
| UUID | 019dffbb-f000-7e93-8e4d-6063849de049 |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | listagg(...) |
| Return Type Rule | descriptor-authoritative listagg aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_listagg.v3 |
| AST Binding | ast.aggregate.aggregate_listagg |
| Engine Entrypoint | aggregate_listagg |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select listagg(arg_1) from app.orders group by account_id;
```

### `max`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.max |
| UUID | 019dffbb-f000-77fd-8bea-6d4402284c30 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | max(...) |
| Return Type Rule | descriptor-authoritative max aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_max.v3 |
| AST Binding | ast.aggregate.aggregate_max |
| Engine Entrypoint | aggregate_max |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select max(arg_1) from app.orders group by account_id;
```

### `max`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.max |
| UUID | 019de5fc-2400-7d1e-8aa4-80bc647fbd9a |
| Kind | aggregate |
| Syntax Forms | aggregate_call, aggregate_filter, aggregate_order_by_when_allowed, aggregate_distinct_when_allowed |
| Overloads | max(expr) |
| Return Type Rule | descriptor comparison with collation if string |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | ignores_null |
| Collation/Charset Rule | uses descriptor collation for comparable/string states |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | stable_per_group |
| Determinism | deterministic for deterministic input order; order-sensitive aggregates require ORDER BY for deterministic output |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_max.v3 |
| AST Binding | ast.aggregate.aggregate_max |
| Engine Entrypoint | aggregate_max |
| Security Policy | inherits containing query rights |
| Error Semantics | see aggregate error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select max(value_1) from app.orders group by account_id;
```

### `min`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.min |
| UUID | 019dffbb-f000-7f2b-819b-018530302efe |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | min(...) |
| Return Type Rule | descriptor-authoritative min aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_min.v3 |
| AST Binding | ast.aggregate.aggregate_min |
| Engine Entrypoint | aggregate_min |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select min(arg_1) from app.orders group by account_id;
```

### `min`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.min |
| UUID | 019de5fc-2400-781c-881b-4af4d55d402b |
| Kind | aggregate |
| Syntax Forms | aggregate_call, aggregate_filter, aggregate_order_by_when_allowed, aggregate_distinct_when_allowed |
| Overloads | min(expr) |
| Return Type Rule | descriptor comparison with collation if string |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | ignores_null |
| Collation/Charset Rule | uses descriptor collation for comparable/string states |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | stable_per_group |
| Determinism | deterministic for deterministic input order; order-sensitive aggregates require ORDER BY for deterministic output |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_min.v3 |
| AST Binding | ast.aggregate.aggregate_min |
| Engine Entrypoint | aggregate_min |
| Security Policy | inherits containing query rights |
| Error Semantics | see aggregate error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select min(value_1) from app.orders group by account_id;
```

### `mode`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.mode |
| UUID | 019dffbb-f000-7150-9be6-bcf97f8facf5 |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | mode(...) |
| Return Type Rule | descriptor-authoritative mode aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_mode.v3 |
| AST Binding | ast.aggregate.aggregate_mode |
| Engine Entrypoint | aggregate_mode |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select mode(arg_1) from app.orders group by account_id;
```

### `mode`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.mode |
| UUID | 019dffbb-f000-7150-9be6-bcf97f8facf5 |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | mode(...) |
| Return Type Rule | descriptor-authoritative mode aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_mode.v3 |
| AST Binding | ast.aggregate.aggregate_mode |
| Engine Entrypoint | aggregate_mode |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select mode(arg_1) from app.orders group by account_id;
```

### `percent_rank`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.percent_rank |
| UUID | 019dffbb-f000-7817-911f-9f8b2e66ebec |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | percent_rank(...) |
| Return Type Rule | descriptor-authoritative percent_rank aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_hypothetical_percent_rank.v3 |
| AST Binding | ast.aggregate.aggregate_percent_rank |
| Engine Entrypoint | aggregate_hypothetical_percent_rank |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select percent_rank(arg_1) from app.orders group by account_id;
```

### `percent_rank`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.percent_rank |
| UUID | 019dffbb-f000-7817-911f-9f8b2e66ebec |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | percent_rank(...) |
| Return Type Rule | descriptor-authoritative percent_rank aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_hypothetical_percent_rank.v3 |
| AST Binding | ast.aggregate.aggregate_percent_rank |
| Engine Entrypoint | aggregate_hypothetical_percent_rank |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select percent_rank(arg_1) from app.orders group by account_id;
```

### `percentile_cont`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.percentile_cont |
| UUID | 019dffbb-f000-7cfd-83dd-15435fe55bf5 |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | percentile_cont(...) |
| Return Type Rule | descriptor-authoritative percentile_cont aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_percentile_cont.v3 |
| AST Binding | ast.aggregate.aggregate_percentile_cont |
| Engine Entrypoint | aggregate_percentile_cont |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select percentile_cont(arg_1) from app.orders group by account_id;
```

### `percentile_cont`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.percentile_cont |
| UUID | 019dffbb-f000-7cfd-83dd-15435fe55bf5 |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | percentile_cont(...) |
| Return Type Rule | descriptor-authoritative percentile_cont aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_percentile_cont.v3 |
| AST Binding | ast.aggregate.aggregate_percentile_cont |
| Engine Entrypoint | aggregate_percentile_cont |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select percentile_cont(arg_1) from app.orders group by account_id;
```

### `percentile_disc`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.percentile_disc |
| UUID | 019dffbb-f000-7081-b766-7db818a89c04 |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | percentile_disc(...) |
| Return Type Rule | descriptor-authoritative percentile_disc aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_percentile_disc.v3 |
| AST Binding | ast.aggregate.aggregate_percentile_disc |
| Engine Entrypoint | aggregate_percentile_disc |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select percentile_disc(arg_1) from app.orders group by account_id;
```

### `percentile_disc`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.percentile_disc |
| UUID | 019dffbb-f000-7081-b766-7db818a89c04 |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | percentile_disc(...) |
| Return Type Rule | descriptor-authoritative percentile_disc aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_percentile_disc.v3 |
| AST Binding | ast.aggregate.aggregate_percentile_disc |
| Engine Entrypoint | aggregate_percentile_disc |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select percentile_disc(arg_1) from app.orders group by account_id;
```

### `rank`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.rank |
| UUID | 019dffbb-f000-7336-ab53-fef5316220d7 |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | rank(...) |
| Return Type Rule | descriptor-authoritative rank aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_hypothetical_rank.v3 |
| AST Binding | ast.aggregate.aggregate_rank |
| Engine Entrypoint | aggregate_hypothetical_rank |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select rank(arg_1) from app.orders group by account_id;
```

### `rank`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.rank |
| UUID | 019dffbb-f000-7336-ab53-fef5316220d7 |
| Kind | aggregate |
| Syntax Forms | ordered_set_function_call |
| Overloads | rank(...) |
| Return Type Rule | descriptor-authoritative rank aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_hypothetical_rank.v3 |
| AST Binding | ast.aggregate.aggregate_rank |
| Engine Entrypoint | aggregate_hypothetical_rank |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select rank(arg_1) from app.orders group by account_id;
```

### `regr_avgx`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_avgx |
| UUID | 019dffbb-f000-7662-a816-d1df50e9b664 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_avgx(...) |
| Return Type Rule | descriptor-authoritative regr_avgx aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_avgx.v3 |
| AST Binding | ast.aggregate.aggregate_regr_avgx |
| Engine Entrypoint | aggregate_regr_avgx |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_avgx(arg_1) from app.orders group by account_id;
```

### `regr_avgx`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_avgx |
| UUID | 019dffbb-f000-7662-a816-d1df50e9b664 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_avgx(...) |
| Return Type Rule | descriptor-authoritative regr_avgx aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_avgx.v3 |
| AST Binding | ast.aggregate.aggregate_regr_avgx |
| Engine Entrypoint | aggregate_regr_avgx |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_avgx(arg_1) from app.orders group by account_id;
```

### `regr_avgy`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_avgy |
| UUID | 019dffbb-f000-7d03-ac2d-753cdb7744c0 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_avgy(...) |
| Return Type Rule | descriptor-authoritative regr_avgy aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_avgy.v3 |
| AST Binding | ast.aggregate.aggregate_regr_avgy |
| Engine Entrypoint | aggregate_regr_avgy |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_avgy(arg_1) from app.orders group by account_id;
```

### `regr_avgy`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_avgy |
| UUID | 019dffbb-f000-7d03-ac2d-753cdb7744c0 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_avgy(...) |
| Return Type Rule | descriptor-authoritative regr_avgy aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_avgy.v3 |
| AST Binding | ast.aggregate.aggregate_regr_avgy |
| Engine Entrypoint | aggregate_regr_avgy |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_avgy(arg_1) from app.orders group by account_id;
```

### `regr_count`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_count |
| UUID | 019dffbb-f001-704a-8a00-00000000004a |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_count(...) |
| Return Type Rule | descriptor-authoritative regr_count aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_count.v3 |
| AST Binding | ast.aggregate.aggregate_regr_count |
| Engine Entrypoint | aggregate_regr_count |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_count(arg_1) from app.orders group by account_id;
```

### `regr_count`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_count |
| UUID | 019dffbb-f000-75aa-bbe6-a4a67dacb81f |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_count(...) |
| Return Type Rule | descriptor-authoritative regr_count aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_count.v3 |
| AST Binding | ast.aggregate.aggregate_regr_count |
| Engine Entrypoint | aggregate_regr_count |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_count(arg_1) from app.orders group by account_id;
```

### `regr_intercept`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_intercept |
| UUID | 019dffbb-f000-7c7c-b576-d67ea9d4bcbb |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_intercept(...) |
| Return Type Rule | descriptor-authoritative regr_intercept aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_intercept.v3 |
| AST Binding | ast.aggregate.aggregate_regr_intercept |
| Engine Entrypoint | aggregate_regr_intercept |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_intercept(arg_1) from app.orders group by account_id;
```

### `regr_intercept`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_intercept |
| UUID | 019dffbb-f000-7c7c-b576-d67ea9d4bcbb |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_intercept(...) |
| Return Type Rule | descriptor-authoritative regr_intercept aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_intercept.v3 |
| AST Binding | ast.aggregate.aggregate_regr_intercept |
| Engine Entrypoint | aggregate_regr_intercept |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_intercept(arg_1) from app.orders group by account_id;
```

### `regr_r2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_r2 |
| UUID | 019dffbb-f000-7a43-9a28-a119b31d9c20 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_r2(...) |
| Return Type Rule | descriptor-authoritative regr_r2 aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_r2.v3 |
| AST Binding | ast.aggregate.aggregate_regr_r2 |
| Engine Entrypoint | aggregate_regr_r2 |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_r2(arg_1) from app.orders group by account_id;
```

### `regr_r2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_r2 |
| UUID | 019dffbb-f000-7a43-9a28-a119b31d9c20 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_r2(...) |
| Return Type Rule | descriptor-authoritative regr_r2 aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_r2.v3 |
| AST Binding | ast.aggregate.aggregate_regr_r2 |
| Engine Entrypoint | aggregate_regr_r2 |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_r2(arg_1) from app.orders group by account_id;
```

### `regr_slope`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_slope |
| UUID | 019dffbb-f000-7f80-b81a-5240a6dbab55 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_slope(...) |
| Return Type Rule | descriptor-authoritative regr_slope aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_slope.v3 |
| AST Binding | ast.aggregate.aggregate_regr_slope |
| Engine Entrypoint | aggregate_regr_slope |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_slope(arg_1) from app.orders group by account_id;
```

### `regr_slope`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_slope |
| UUID | 019dffbb-f000-7f80-b81a-5240a6dbab55 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_slope(...) |
| Return Type Rule | descriptor-authoritative regr_slope aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_slope.v3 |
| AST Binding | ast.aggregate.aggregate_regr_slope |
| Engine Entrypoint | aggregate_regr_slope |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_slope(arg_1) from app.orders group by account_id;
```

### `regr_sxx`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_sxx |
| UUID | 019dffbb-f000-735e-9e55-5f9243786403 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_sxx(...) |
| Return Type Rule | descriptor-authoritative regr_sxx aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_sxx.v3 |
| AST Binding | ast.aggregate.aggregate_regr_sxx |
| Engine Entrypoint | aggregate_regr_sxx |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_sxx(arg_1) from app.orders group by account_id;
```

### `regr_sxx`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_sxx |
| UUID | 019dffbb-f000-735e-9e55-5f9243786403 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_sxx(...) |
| Return Type Rule | descriptor-authoritative regr_sxx aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_sxx.v3 |
| AST Binding | ast.aggregate.aggregate_regr_sxx |
| Engine Entrypoint | aggregate_regr_sxx |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_sxx(arg_1) from app.orders group by account_id;
```

### `regr_sxy`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_sxy |
| UUID | 019dffbb-f000-788b-a249-866547a43ebe |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_sxy(...) |
| Return Type Rule | descriptor-authoritative regr_sxy aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_sxy.v3 |
| AST Binding | ast.aggregate.aggregate_regr_sxy |
| Engine Entrypoint | aggregate_regr_sxy |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_sxy(arg_1) from app.orders group by account_id;
```

### `regr_sxy`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_sxy |
| UUID | 019dffbb-f000-788b-a249-866547a43ebe |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_sxy(...) |
| Return Type Rule | descriptor-authoritative regr_sxy aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_sxy.v3 |
| AST Binding | ast.aggregate.aggregate_regr_sxy |
| Engine Entrypoint | aggregate_regr_sxy |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_sxy(arg_1) from app.orders group by account_id;
```

### `regr_syy`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_syy |
| UUID | 019dffbb-f000-74f7-98ba-c24ead6d30df |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_syy(...) |
| Return Type Rule | descriptor-authoritative regr_syy aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_syy.v3 |
| AST Binding | ast.aggregate.aggregate_regr_syy |
| Engine Entrypoint | aggregate_regr_syy |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_syy(arg_1) from app.orders group by account_id;
```

### `regr_syy`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.regr_syy |
| UUID | 019dffbb-f000-74f7-98ba-c24ead6d30df |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | regr_syy(...) |
| Return Type Rule | descriptor-authoritative regr_syy aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_regr_syy.v3 |
| AST Binding | ast.aggregate.aggregate_regr_syy |
| Engine Entrypoint | aggregate_regr_syy |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select regr_syy(arg_1) from app.orders group by account_id;
```

### `stddev`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.stddev |
| UUID | 019dffbb-f000-7475-8516-ff003b2bdad9 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | stddev(...) |
| Return Type Rule | descriptor-authoritative stddev aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_stddev.v3 |
| AST Binding | ast.aggregate.aggregate_stddev |
| Engine Entrypoint | aggregate_stddev |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select stddev(arg_1) from app.orders group by account_id;
```

### `stddev`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.stddev |
| UUID | 019dffbb-f000-7475-8516-ff003b2bdad9 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | stddev(...) |
| Return Type Rule | descriptor-authoritative stddev aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_stddev.v3 |
| AST Binding | ast.aggregate.aggregate_stddev |
| Engine Entrypoint | aggregate_stddev |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select stddev(arg_1) from app.orders group by account_id;
```

### `stddev_pop`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.stddev_pop |
| UUID | 019dffbb-f000-7518-93fc-0281bfa1d698 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | stddev_pop(...) |
| Return Type Rule | descriptor-authoritative stddev_pop aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_stddev_pop.v3 |
| AST Binding | ast.aggregate.aggregate_stddev_pop |
| Engine Entrypoint | aggregate_stddev_pop |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select stddev_pop(arg_1) from app.orders group by account_id;
```

### `stddev_pop`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.stddev_pop |
| UUID | 019de5fc-2400-73c9-ba10-4665f741215d |
| Kind | aggregate |
| Syntax Forms | aggregate_call, aggregate_filter, aggregate_order_by_when_allowed, aggregate_distinct_when_allowed |
| Overloads | stddev_pop(expr) |
| Return Type Rule | numeric statistical aggregate |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | ignores_null |
| Collation/Charset Rule | uses descriptor collation for comparable/string states |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | stable_per_group |
| Determinism | deterministic for deterministic input order; order-sensitive aggregates require ORDER BY for deterministic output |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_stddev_pop.v3 |
| AST Binding | ast.aggregate.aggregate_stddev_pop |
| Engine Entrypoint | aggregate_stddev_pop |
| Security Policy | inherits containing query rights |
| Error Semantics | see aggregate error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select stddev_pop(value_1) from app.orders group by account_id;
```

### `stddev_samp`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.stddev_samp |
| UUID | 019dffbb-f001-7046-8a00-000000000046 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | stddev_samp(...) |
| Return Type Rule | descriptor-authoritative stddev_samp aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_stddev_samp.v3 |
| AST Binding | ast.aggregate.aggregate_stddev_samp |
| Engine Entrypoint | aggregate_stddev_samp |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select stddev_samp(arg_1) from app.orders group by account_id;
```

### `stddev_samp`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.stddev_samp |
| UUID | 019dffbb-f000-7d99-a495-70f9c3b1b587 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | stddev_samp(...) |
| Return Type Rule | descriptor-authoritative stddev_samp aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_stddev_samp.v3 |
| AST Binding | ast.aggregate.aggregate_stddev_samp |
| Engine Entrypoint | aggregate_stddev_samp |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select stddev_samp(arg_1) from app.orders group by account_id;
```

### `string_agg`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.string_agg |
| UUID | 019dffbb-f000-7e4c-a693-39bb6632d887 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | string_agg(...) |
| Return Type Rule | descriptor-authoritative string_agg aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_string_agg.v3 |
| AST Binding | ast.aggregate.aggregate_string_agg |
| Engine Entrypoint | aggregate_string_agg |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select string_agg(arg_1) from app.orders group by account_id;
```

### `string_agg`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.string_agg |
| UUID | 019de5fc-2400-7243-abc6-4f6a777dff00 |
| Kind | aggregate |
| Syntax Forms | aggregate_call, aggregate_filter, aggregate_order_by_when_allowed, aggregate_distinct_when_allowed |
| Overloads | string_agg(expr,delimiter) |
| Return Type Rule | string builder state with collation/charset metadata |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | ignores_null |
| Collation/Charset Rule | uses descriptor collation for comparable/string states |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | stable_per_group |
| Determinism | deterministic for deterministic input order; order-sensitive aggregates require ORDER BY for deterministic output |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_string_agg.v3 |
| AST Binding | ast.aggregate.aggregate_string_agg |
| Engine Entrypoint | aggregate_string_agg |
| Security Policy | inherits containing query rights |
| Error Semantics | see aggregate error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select string_agg(value_1, arg_2) from app.orders group by account_id;
```

### `sum`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.sum |
| UUID | 019dffbb-f000-79d0-ae24-876730a8cd92 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | sum(...) |
| Return Type Rule | descriptor-authoritative sum aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_sum.v3 |
| AST Binding | ast.aggregate.aggregate_sum |
| Engine Entrypoint | aggregate_sum |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select sum(arg_1) from app.orders group by account_id;
```

### `sum`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.sum |
| UUID | 019de5fc-2400-72e4-8549-82b2eef5a777 |
| Kind | aggregate |
| Syntax Forms | aggregate_call, aggregate_filter, aggregate_order_by_when_allowed, aggregate_distinct_when_allowed |
| Overloads | sum(expr) |
| Return Type Rule | widened exact accumulator for integer/decimal; real128-aware floating accumulator |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | ignores_null |
| Collation/Charset Rule | uses descriptor collation for comparable/string states |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | stable_per_group |
| Determinism | deterministic for deterministic input order; order-sensitive aggregates require ORDER BY for deterministic output |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_sum.v3 |
| AST Binding | ast.aggregate.aggregate_sum |
| Engine Entrypoint | aggregate_sum |
| Security Policy | inherits containing query rights |
| Error Semantics | see aggregate error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select sum(value_1) from app.orders group by account_id;
```

### `variance`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.variance |
| UUID | 019dffbb-f000-7968-82c5-04cffbeb971b |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | variance(...) |
| Return Type Rule | descriptor-authoritative variance aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_variance.v3 |
| AST Binding | ast.aggregate.aggregate_variance |
| Engine Entrypoint | aggregate_variance |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select variance(arg_1) from app.orders group by account_id;
```

### `variance`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.variance |
| UUID | 019dffbb-f000-7968-82c5-04cffbeb971b |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | variance(...) |
| Return Type Rule | descriptor-authoritative variance aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_variance.v3 |
| AST Binding | ast.aggregate.aggregate_variance |
| Engine Entrypoint | aggregate_variance |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select variance(arg_1) from app.orders group by account_id;
```

### `variance_pop`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.variance_pop |
| UUID | 019dffbb-f000-739d-9d55-7830004712a6 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | variance_pop(...) |
| Return Type Rule | descriptor-authoritative variance_pop aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_variance_pop.v3 |
| AST Binding | ast.aggregate.aggregate_variance_pop |
| Engine Entrypoint | aggregate_variance_pop |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select variance_pop(arg_1) from app.orders group by account_id;
```

### `variance_pop`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.variance_pop |
| UUID | 019de5fc-2400-7fda-b470-e85414dcb314 |
| Kind | aggregate |
| Syntax Forms | aggregate_call, aggregate_filter, aggregate_order_by_when_allowed, aggregate_distinct_when_allowed |
| Overloads | variance_pop(expr) |
| Return Type Rule | numeric statistical aggregate |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | ignores_null |
| Collation/Charset Rule | uses descriptor collation for comparable/string states |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | stable_per_group |
| Determinism | deterministic for deterministic input order; order-sensitive aggregates require ORDER BY for deterministic output |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_variance_pop.v3 |
| AST Binding | ast.aggregate.aggregate_variance_pop |
| Engine Entrypoint | aggregate_variance_pop |
| Security Policy | inherits containing query rights |
| Error Semantics | see aggregate error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select variance_pop(value_1) from app.orders group by account_id;
```

### `variance_samp`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.variance_samp |
| UUID | 019dffbb-f001-7047-8a00-000000000047 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | variance_samp(...) |
| Return Type Rule | descriptor-authoritative variance_samp aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_variance_samp.v3 |
| AST Binding | ast.aggregate.aggregate_variance_samp |
| Engine Entrypoint | aggregate_variance_samp |
| Security Policy | none unless source expressions read session/security/system metadata |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select variance_samp(arg_1) from app.orders group by account_id;
```

### `variance_samp`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.variance_samp |
| UUID | 019dffbb-f000-732b-8a0c-2aa88b04f3c5 |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | variance_samp(...) |
| Return Type Rule | descriptor-authoritative variance_samp aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_variance_samp.v3 |
| AST Binding | ast.aggregate.aggregate_variance_samp |
| Engine Entrypoint | aggregate_variance_samp |
| Security Policy | inherits containing query rights |
| Error Semantics | aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select variance_samp(arg_1) from app.orders group by account_id;
```

### `expr_match_recognize_v1`

| Property | Value |
| --- | --- |
| Builtin ID | sb.expr.match_recognize.v1 |
| UUID | 019dffbb-f000-76cb-9d3f-d1c40e530ff0 |
| Kind | expression_descriptor |
| Syntax Forms | function_call |
| Overloads | expr_match_recognize_v1(...) |
| Return Type Rule | runtime-defined by engine entrypoint expr_match_recognize_v1 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.expr_match_recognize_v1.v3 |
| AST Binding | ast.expr.expr_match_recognize_v1 |
| Engine Entrypoint | expr_match_recognize_v1 |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select expr_match_recognize_v1(arg_1) from app.sample_values;
```

### `%`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.modulo |
| UUID | 019de5fc-2400-7d91-817e-9943a58111ae |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | % descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_modulo.v3 |
| AST Binding | ast.operator.operator_modulo |
| Engine Entrypoint | operator_modulo |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 % value_2 from app.sample_values;
```

### `*`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.multiply |
| UUID | 019de5fc-2400-7a44-b64f-4ff649be651a |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | * descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_multiply.v3 |
| AST Binding | ast.operator.operator_multiply |
| Engine Entrypoint | operator_multiply |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 * value_2 from app.sample_values;
```

### `+`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.add |
| UUID | 019de5fc-2400-78b9-8d99-e7d822726c30 |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | + descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_add.v3 |
| AST Binding | ast.operator.operator_add |
| Engine Entrypoint | operator_add |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 + value_2 from app.sample_values;
```

### `-`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.subtract |
| UUID | 019de5fc-2400-732a-afd0-934254e2b071 |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | - descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_subtract.v3 |
| AST Binding | ast.operator.operator_subtract |
| Engine Entrypoint | operator_subtract |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 - value_2 from app.sample_values;
```

### `-`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.unary_minus |
| UUID | 019de5fc-2400-74fb-8b10-1a06a556804f |
| Kind | operator |
| Syntax Forms | prefix |
| Overloads | - descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_unary_minus.v3 |
| AST Binding | ast.operator.operator_unary_minus |
| Engine Entrypoint | operator_unary_minus |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select - numeric_value_1 from app.sample_values;
```

### `->`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.json_get |
| UUID | 019de5fc-2400-7874-8b4e-3bf82a1c4ada |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | -> descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_json_get.v3 |
| AST Binding | ast.operator.operator_json_get |
| Engine Entrypoint | operator_json_get |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 -> value_2 from app.sample_values;
```

### `->>`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.json_get_text |
| UUID | 019de5fc-2400-7409-a7ea-229e45ee3973 |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | ->> descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_json_get_text.v3 |
| AST Binding | ast.operator.operator_json_get_text |
| Engine Entrypoint | operator_json_get_text |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 ->> value_2 from app.sample_values;
```

### `/`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.divide |
| UUID | 019de5fc-2400-7af4-811a-e6d167f7c31c |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | / descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_divide.v3 |
| AST Binding | ast.operator.operator_divide |
| Engine Entrypoint | operator_divide |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 / value_2 from app.sample_values;
```

### `<`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.less |
| UUID | 019de5fc-2400-7495-94fd-125e068c96ac |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | < descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_less.v3 |
| AST Binding | ast.operator.operator_less |
| Engine Entrypoint | operator_less |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 < value_2 from app.sample_values;
```

### `<=`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.less_equal |
| UUID | 019de5fc-2400-71cf-ba34-28547a4c5e0b |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | <= descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_less_equal.v3 |
| AST Binding | ast.operator.operator_less_equal |
| Engine Entrypoint | operator_less_equal |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 <= value_2 from app.sample_values;
```

### `<>`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.not_equal |
| UUID | 019de5fc-2400-7e04-90b2-22454376b5c2 |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | <> descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_not_equal.v3 |
| AST Binding | ast.operator.operator_not_equal |
| Engine Entrypoint | operator_not_equal |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 <> value_2 from app.sample_values;
```

### `=`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.equal |
| UUID | 019de5fc-2400-7b73-9c38-dcf10204dbde |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | = descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_equal.v3 |
| AST Binding | ast.operator.operator_equal |
| Engine Entrypoint | operator_equal |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 = value_2 from app.sample_values;
```

### `>`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.greater |
| UUID | 019de5fc-2400-7767-baeb-32e9ee73dc0e |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | > descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_greater.v3 |
| AST Binding | ast.operator.operator_greater |
| Engine Entrypoint | operator_greater |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 > value_2 from app.sample_values;
```

### `>=`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.greater_equal |
| UUID | 019de5fc-2400-70aa-ad1d-b14341f92dba |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | >= descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_greater_equal.v3 |
| AST Binding | ast.operator.operator_greater_equal |
| Engine Entrypoint | operator_greater_equal |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 >= value_2 from app.sample_values;
```

### `@>`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.array_contains |
| UUID | 019de5fc-2400-7092-a957-61d2c1a3300a |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | @> descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_array_contains.v3 |
| AST Binding | ast.operator.operator_array_contains |
| Engine Entrypoint | operator_array_contains |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 @> value_2 from app.sample_values;
```

### `and`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.and |
| UUID | 019de5fc-2400-7b1e-b271-322b2945f88a |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | and descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_and.v3 |
| AST Binding | ast.operator.operator_and |
| Engine Entrypoint | operator_and |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 and value_2 from app.sample_values;
```

### `is distinct from`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.is_distinct_from |
| UUID | 019de5fc-2400-798d-9da8-7d0f9cc70fbe |
| Kind | operator |
| Syntax Forms | special_infix |
| Overloads | is distinct from descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_is_distinct_from.v3 |
| AST Binding | ast.operator.operator_is_distinct_from |
| Engine Entrypoint | operator_is_distinct_from |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 is distinct from value_2 from app.sample_values;
```

### `like`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.like |
| UUID | 019de5fc-2400-7582-b396-50e182b94758 |
| Kind | operator |
| Syntax Forms | special_infix |
| Overloads | like descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_like.v3 |
| AST Binding | ast.operator.operator_like |
| Engine Entrypoint | operator_like |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 like value_2 from app.sample_values;
```

### `not`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.not |
| UUID | 019de5fc-2400-77f3-8e0d-d5ec48319124 |
| Kind | operator |
| Syntax Forms | prefix |
| Overloads | not descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_not.v3 |
| AST Binding | ast.operator.operator_not |
| Engine Entrypoint | operator_not |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select not numeric_value_1 from app.sample_values;
```

### `or`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.or |
| UUID | 019de5fc-2400-70ca-a67c-b1d440b4684a |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | or descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_or.v3 |
| AST Binding | ast.operator.operator_or |
| Engine Entrypoint | operator_or |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 or value_2 from app.sample_values;
```

### `||`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.concat |
| UUID | 019de5fc-2400-7cfe-a41d-5524e3d32799 |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | \|\| descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_concat.v3 |
| AST Binding | ast.operator.operator_concat |
| Engine Entrypoint | operator_concat |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 || value_2 from app.sample_values;
```

### `~`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.regex_match |
| UUID | 019de5fc-2400-7d91-9595-8aa65835b090 |
| Kind | operator |
| Syntax Forms | binary |
| Overloads | ~ descriptor operands |
| Return Type Rule | see operator family descriptor matrix |
| Coercion Rule | apply overload resolution matrix before implicit casts |
| Null Behavior | SQL three-valued logic for comparisons/boolean operators; strict for arithmetic unless special |
| Collation/Charset Rule | comparison/pattern operators use collation/charset descriptor rules |
| Timezone Rule | not applicable unless temporal operands require timezone conversion |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_regex_match.v3 |
| AST Binding | ast.operator.operator_regex_match |
| Engine Entrypoint | operator_regex_match |
| Security Policy | none |
| Error Semantics | see error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 ~ value_2 from app.sample_values;
```

### `a`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.a |
| UUID | 019dffbb-f001-7454-8a00-000000000454 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | a(...) |
| Return Type Rule | runtime-defined by engine entrypoint a |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_a.v3 |
| AST Binding | ast.expr.scalar_a |
| Engine Entrypoint | a |
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
select a(arg_1) from app.sample_values;
```

### `abs`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.abs |
| UUID | 019de5fc-2400-7671-9f1b-5350d134ab0f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | abs(number) |
| Return Type Rule | absolute value with overflow checks |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_abs.v3 |
| AST Binding | ast.expr.scalar_abs |
| Engine Entrypoint | scalar_abs |
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
select abs(numeric_value_1) from app.sample_values;
```

### `accept`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.accept |
| UUID | 019dffbb-f000-7391-b7ba-c3634d4be83b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | accept(...) |
| Return Type Rule | runtime-defined by engine entrypoint accept |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_accept.v3 |
| AST Binding | ast.expr.scalar_accept |
| Engine Entrypoint | accept |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select accept(arg_1) from app.sample_values;
```

### `accept_sql2016_timeseries`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.accept_sql2016_timeseries |
| UUID | 019dffbb-f000-724b-bd42-19f0a798df92 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | accept_sql2016_timeseries(...) |
| Return Type Rule | runtime-defined by engine entrypoint accept_sql2016_timeseries |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_accept_sql2016_timeseries.v3 |
| AST Binding | ast.expr.scalar_accept_sql2016_timeseries |
| Engine Entrypoint | accept_sql2016_timeseries |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select accept_sql2016_timeseries(arg_1) from app.sample_values;
```

### `acos`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.acos |
| UUID | 019dffbb-f001-7005-8a00-000000000005 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | acos(...) |
| Return Type Rule | descriptor-authoritative acos numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_acos.v3 |
| AST Binding | ast.expr.scalar_acos |
| Engine Entrypoint | scalar_acos |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select acos(arg_1) from app.sample_values;
```

### `acosd`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.acosd |
| UUID | 019dffbb-f001-7037-8a00-000000000037 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | acosd(...) |
| Return Type Rule | descriptor-authoritative acosd numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_acosd.v3 |
| AST Binding | ast.expr.scalar_acosd |
| Engine Entrypoint | scalar_acosd |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select acosd(arg_1) from app.sample_values;
```

### `acosh`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.acosh |
| UUID | 019dffbb-f001-702f-8a00-00000000002f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | acosh(...) |
| Return Type Rule | descriptor-authoritative acosh numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_acosh.v3 |
| AST Binding | ast.expr.scalar_acosh |
| Engine Entrypoint | scalar_acosh |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select acosh(arg_1) from app.sample_values;
```

### `alter`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.alter |
| UUID | 019dffbb-f000-725f-8f16-898c51756dd5 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | alter(...) |
| Return Type Rule | runtime-defined by engine entrypoint alter |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_alter.v3 |
| AST Binding | ast.expr.scalar_alter |
| Engine Entrypoint | alter |
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
select alter(arg_1) from app.sample_values;
```

### `application_name`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.application_name |
| UUID | 019dffbb-f000-7a68-93b3-fbdaa33c34c1 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | application_name |
| Return Type Rule | application name from SBLR execution context as character; descriptor=character |
| Coercion Rule | no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted |
| Null Behavior | context-backed typed null or diagnostic behavior follows SBSFC-016 row evidence |
| Collation/Charset Rule | character descriptors use byte-stable ASCII fixture values unless context supplies text |
| Timezone Rule | not applicable unless the context value is timezone-derived |
| Volatility | stable_per_session |
| Determinism | not foldable for execution-context reads or procedural diagnostics |
| Side Effects | none |
| SBLR Binding | sblr.expr.application_name.v3 |
| AST Binding | ast.expr.application_name |
| Engine Entrypoint | application_name |
| Security Policy | reads session application_name context only |
| Error Semantics | arity/type/context/procedural diagnostics use builtin error compatibility matrix and SBSFC-016 evidence; diagnostic=SBSQL.FUNCTION.INVALID_INPUT for arity/type/context violations where applicable |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select application_name() from app.sample_values;
```

### `array_max_dimension`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.array_max_dimension |
| UUID | 019dffbb-f000-7aaf-af64-9f214e0c09e7 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | array_max_dimension() |
| Return Type Rule | fixed policy limit for array dimensions; descriptor=uint64 |
| Coercion Rule | no input coercion |
| Null Behavior | never null |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | immutable_policy |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.array_max_dimension.v3 |
| AST Binding | ast.expr.array_max_dimension |
| Engine Entrypoint | array_max_dimension |
| Security Policy | fixed public parser/runtime policy, no catalog or storage authority |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select array_max_dimension() from app.sample_values;
```

### `array_max_element_count`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.array_max_element_count |
| UUID | 019dffbb-f000-7a6b-996e-bfca99ec1b4d |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | array_max_element_count() |
| Return Type Rule | fixed policy limit for array element count; descriptor=uint64 |
| Coercion Rule | no input coercion |
| Null Behavior | never null |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | immutable_policy |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.array_max_element_count.v3 |
| AST Binding | ast.expr.array_max_element_count |
| Engine Entrypoint | array_max_element_count |
| Security Policy | fixed public parser/runtime policy, no catalog or storage authority |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select array_max_element_count() from app.sample_values;
```

### `array_to_string`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.array_to_string |
| UUID | 019dffbb-f000-755a-a157-6f7f6c2f1831 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | array_to_string(...) |
| Return Type Rule | descriptor-authoritative array_to_string result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for native function/operator operands |
| Null Behavior | function/operator-specific NULL handling follows SBSFC-026 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | stable_by_arguments |
| Determinism | foldable only when inputs are constants with stable descriptors and function-specific context is fixed |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_array_to_string.v3 |
| AST Binding | ast.expr.scalar_array_to_string |
| Engine Entrypoint | array_to_string |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-026 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select array_to_string(arg_1) from app.sample_values;
```

### `as`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.as |
| UUID | 019dffbb-f000-7ae1-88fd-d0e4a50832b4 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | as(...) |
| Return Type Rule | runtime-defined by engine entrypoint as |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_as.v3 |
| AST Binding | ast.expr.scalar_as |
| Engine Entrypoint | as |
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
select as(arg_1) from app.sample_values;
```

### `ascii`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.ascii |
| UUID | 019dffbb-f001-7026-8a00-000000000026 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | ascii(...) |
| Return Type Rule | descriptor-authoritative ascii text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_ascii.v3 |
| AST Binding | ast.expr.scalar_ascii |
| Engine Entrypoint | scalar_ascii |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select ascii(arg_1) from app.sample_values;
```

### `asin`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.asin |
| UUID | 019dffbb-f001-7004-8a00-000000000004 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | asin(...) |
| Return Type Rule | descriptor-authoritative asin numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_asin.v3 |
| AST Binding | ast.expr.scalar_asin |
| Engine Entrypoint | scalar_asin |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select asin(arg_1) from app.sample_values;
```

### `asind`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.asind |
| UUID | 019dffbb-f001-7036-8a00-000000000036 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | asind(...) |
| Return Type Rule | descriptor-authoritative asind numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_asind.v3 |
| AST Binding | ast.expr.scalar_asind |
| Engine Entrypoint | scalar_asind |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select asind(arg_1) from app.sample_values;
```

### `asinh`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.asinh |
| UUID | 019dffbb-f001-702e-8a00-00000000002e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | asinh(...) |
| Return Type Rule | descriptor-authoritative asinh numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_asinh.v3 |
| AST Binding | ast.expr.scalar_asinh |
| Engine Entrypoint | scalar_asinh |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select asinh(arg_1) from app.sample_values;
```

### `asof`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.asof |
| UUID | 019dffbb-f001-742b-8a00-00000000042b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | asof(...) |
| Return Type Rule | runtime-defined by engine entrypoint asof |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_asof.v3 |
| AST Binding | ast.expr.scalar_asof |
| Engine Entrypoint | asof |
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
select asof(arg_1) from app.sample_values;
```

### `at_time_zone`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.at_time_zone |
| UUID | 019dffbb-f000-7059-b6f7-a77a92aae5ac |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | at_time_zone(...) |
| Return Type Rule | runtime-defined by engine entrypoint at_time_zone |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_at_time_zone.v3 |
| AST Binding | ast.expr.scalar_at_time_zone |
| Engine Entrypoint | at_time_zone |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select at_time_zone(arg_1) from app.sample_values;
```

### `atan`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.atan |
| UUID | 019dffbb-f001-7006-8a00-000000000006 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | atan(...) |
| Return Type Rule | descriptor-authoritative atan numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_atan.v3 |
| AST Binding | ast.expr.scalar_atan |
| Engine Entrypoint | scalar_atan |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select atan(arg_1) from app.sample_values;
```

### `atan2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.atan2 |
| UUID | 019dffbb-f001-7032-8a00-000000000032 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | atan2(...) |
| Return Type Rule | descriptor-authoritative atan2 numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_atan2.v3 |
| AST Binding | ast.expr.scalar_atan2 |
| Engine Entrypoint | scalar_atan2 |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select atan2(arg_1) from app.sample_values;
```

### `atan2d`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.atan2d |
| UUID | 019dffbb-f000-7674-b2ff-0df3baedf3ec |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | atan2d(...) |
| Return Type Rule | runtime-defined by engine entrypoint atan2d |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_atan2d.v3 |
| AST Binding | ast.expr.scalar_atan2d |
| Engine Entrypoint | atan2d |
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
select atan2d(arg_1) from app.sample_values;
```

### `atand`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.atand |
| UUID | 019dffbb-f001-7038-8a00-000000000038 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | atand(...) |
| Return Type Rule | descriptor-authoritative atand numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_atand.v3 |
| AST Binding | ast.expr.scalar_atand |
| Engine Entrypoint | scalar_atand |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select atand(arg_1) from app.sample_values;
```

### `atanh`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.atanh |
| UUID | 019dffbb-f001-7030-8a00-000000000030 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | atanh(...) |
| Return Type Rule | descriptor-authoritative atanh numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_atanh.v3 |
| AST Binding | ast.expr.scalar_atanh |
| Engine Entrypoint | scalar_atanh |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select atanh(arg_1) from app.sample_values;
```

### `begin`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.begin |
| UUID | 019dffbb-f000-7837-af15-67e628483bc4 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | begin(...) |
| Return Type Rule | runtime-defined by engine entrypoint begin |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_begin.v3 |
| AST Binding | ast.expr.scalar_begin |
| Engine Entrypoint | begin |
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
select begin(arg_1) from app.sample_values;
```

### `bit_and`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.bit_and |
| UUID | 019dffbb-f001-700d-8a00-00000000000d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | bit_and(...) |
| Return Type Rule | descriptor-authoritative bit_and numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_bit_and.v3 |
| AST Binding | ast.expr.scalar_bit_and |
| Engine Entrypoint | scalar_bit_and |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select bit_and(arg_1) from app.sample_values;
```

### `bit_clear`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.bit_clear |
| UUID | 019dffbb-f000-74ae-9d12-e20c350a7fb6 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | bit_clear(...) |
| Return Type Rule | runtime-defined by engine entrypoint bit_clear |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_bit_clear.v3 |
| AST Binding | ast.expr.scalar_bit_clear |
| Engine Entrypoint | bit_clear |
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
select bit_clear(arg_1) from app.sample_values;
```

### `bit_count`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.bit_count |
| UUID | 019dffbb-f000-7728-b9c4-23295ba8b93d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | bit_count(...) |
| Return Type Rule | descriptor-authoritative bit_count numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_bit_count.v3 |
| AST Binding | ast.expr.scalar_bit_count |
| Engine Entrypoint | scalar_bit_count |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select bit_count(arg_1) from app.sample_values;
```

### `bit_length`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.bit_length |
| UUID | 019dffbb-f000-7c9c-9e88-ac152edc12d2 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | bit_length(...) |
| Return Type Rule | descriptor-authoritative bit_length numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_bit_length.v3 |
| AST Binding | ast.expr.scalar_bit_length |
| Engine Entrypoint | scalar_bit_length |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select bit_length(arg_1) from app.sample_values;
```

### `bit_or`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.bit_or |
| UUID | 019dffbb-f001-700e-8a00-00000000000e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | bit_or(...) |
| Return Type Rule | descriptor-authoritative bit_or numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_bit_or.v3 |
| AST Binding | ast.expr.scalar_bit_or |
| Engine Entrypoint | scalar_bit_or |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select bit_or(arg_1) from app.sample_values;
```

### `bit_set`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.bit_set |
| UUID | 019dffbb-f000-7c8b-bbf9-9d62c702b019 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | bit_set(...) |
| Return Type Rule | runtime-defined by engine entrypoint bit_set |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_bit_set.v3 |
| AST Binding | ast.expr.scalar_bit_set |
| Engine Entrypoint | bit_set |
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
select bit_set(arg_1) from app.sample_values;
```

### `bit_shift_left`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.bit_shift_left |
| UUID | 019dffbb-f001-7010-8a00-000000000010 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | bit_shift_left(...) |
| Return Type Rule | descriptor-authoritative bit_shift_left numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_bit_shift_left.v3 |
| AST Binding | ast.expr.scalar_bit_shift_left |
| Engine Entrypoint | scalar_bit_shift_left |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select bit_shift_left(arg_1) from app.sample_values;
```

### `bit_shift_right`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.bit_shift_right |
| UUID | 019dffbb-f001-7011-8a00-000000000011 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | bit_shift_right(...) |
| Return Type Rule | descriptor-authoritative bit_shift_right numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_bit_shift_right.v3 |
| AST Binding | ast.expr.scalar_bit_shift_right |
| Engine Entrypoint | scalar_bit_shift_right |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select bit_shift_right(arg_1) from app.sample_values;
```

### `bit_string`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.bit_string |
| UUID | 019dffbb-f000-7778-b62f-d583eb530b96 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | bit_string(...) |
| Return Type Rule | runtime-defined by engine entrypoint bit_string |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_bit_string.v3 |
| AST Binding | ast.expr.scalar_bit_string |
| Engine Entrypoint | bit_string |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select bit_string(arg_1) from app.sample_values;
```

### `bit_string_position`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.bit_string_position |
| UUID | 019dffbb-f000-783c-a9bc-bc382ae9e0ee |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | bit_string_position(...) |
| Return Type Rule | runtime-defined by engine entrypoint bit_string_position |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_bit_string_position.v3 |
| AST Binding | ast.expr.scalar_bit_string_position |
| Engine Entrypoint | bit_string_position |
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
select bit_string_position(arg_1) from app.sample_values;
```

### `bit_string_substring`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.bit_string_substring |
| UUID | 019dffbb-f000-74d2-9af4-d400307b64b8 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | bit_string_substring(...) |
| Return Type Rule | runtime-defined by engine entrypoint bit_string_substring |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_bit_string_substring.v3 |
| AST Binding | ast.expr.scalar_bit_string_substring |
| Engine Entrypoint | bit_string_substring |
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
select bit_string_substring(arg_1) from app.sample_values;
```

### `bit_test`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.bit_test |
| UUID | 019dffbb-f000-7fff-ba70-bc0923e230f0 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | bit_test(...) |
| Return Type Rule | runtime-defined by engine entrypoint bit_test |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_bit_test.v3 |
| AST Binding | ast.expr.scalar_bit_test |
| Engine Entrypoint | bit_test |
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
select bit_test(arg_1) from app.sample_values;
```

### `bit_toggle`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.bit_toggle |
| UUID | 019dffbb-f000-746c-9af3-985ab3c57c79 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | bit_toggle(...) |
| Return Type Rule | runtime-defined by engine entrypoint bit_toggle |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_bit_toggle.v3 |
| AST Binding | ast.expr.scalar_bit_toggle |
| Engine Entrypoint | bit_toggle |
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
select bit_toggle(arg_1) from app.sample_values;
```

### `bit_xor`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.bit_xor |
| UUID | 019dffbb-f001-700f-8a00-00000000000f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | bit_xor(...) |
| Return Type Rule | descriptor-authoritative bit_xor numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_bit_xor.v3 |
| AST Binding | ast.expr.scalar_bit_xor |
| Engine Entrypoint | scalar_bit_xor |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select bit_xor(arg_1) from app.sample_values;
```

### `btrim`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.btrim |
| UUID | 019dffbb-f001-7022-8a00-000000000022 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | btrim(...) |
| Return Type Rule | descriptor-authoritative btrim text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_btrim.v3 |
| AST Binding | ast.expr.scalar_btrim |
| Engine Entrypoint | scalar_btrim |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select btrim(arg_1) from app.sample_values;
```

### `built_in_function_shadow_rule`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.built_in_function_shadow_rule |
| UUID | 019dffbb-f000-7840-85c2-a683b98fa9e0 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | built_in_function_shadow_rule(...) |
| Return Type Rule | runtime-defined by engine entrypoint built_in_function_shadow_rule |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_built_in_function_shadow_rule.v3 |
| AST Binding | ast.expr.scalar_built_in_function_shadow_rule |
| Engine Entrypoint | built_in_function_shadow_rule |
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
select built_in_function_shadow_rule(arg_1) from app.sample_values;
```

### `bulk_exceptions`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.bulk_exceptions |
| UUID | 019dffbb-f000-7034-8130-915e4ca36107 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | bulk_exceptions(...) |
| Return Type Rule | runtime-defined by engine entrypoint bulk_exceptions |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_bulk_exceptions.v3 |
| AST Binding | ast.expr.scalar_bulk_exceptions |
| Engine Entrypoint | bulk_exceptions |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select bulk_exceptions(arg_1) from app.sample_values;
```

### `canonical_function_idempotency_requirement`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.canonical_function_idempotency_requirement |
| UUID | 019dffbb-f001-744d-8a00-00000000044d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | canonical_function_idempotency_requirement(...) |
| Return Type Rule | runtime-defined by engine entrypoint canonical_function_idempotency_requirement |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_canonical_function_idempotency_requirement.v3 |
| AST Binding | ast.expr.scalar_canonical_function_idempotency_requirement |
| Engine Entrypoint | canonical_function_idempotency_requirement |
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
select canonical_function_idempotency_requirement(arg_1) from app.sample_values;
```

### `capability_required`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.capability_required |
| UUID | 019dffbb-f001-7444-8a00-000000000444 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | capability_required(...) |
| Return Type Rule | runtime-defined by engine entrypoint capability_required |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_capability_required.v3 |
| AST Binding | ast.expr.scalar_capability_required |
| Engine Entrypoint | capability_required |
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
select capability_required(arg_1) from app.sample_values;
```

### `cardinality`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.cardinality |
| UUID | 019dffbb-f001-7012-8a00-00000000c012 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cardinality(...) |
| Return Type Rule | descriptor-authoritative cardinality result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for native function/operator operands |
| Null Behavior | function/operator-specific NULL handling follows SBSFC-026 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | stable_by_arguments |
| Determinism | foldable only when inputs are constants with stable descriptors and function-specific context is fixed |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_cardinality.v3 |
| AST Binding | ast.expr.scalar_cardinality |
| Engine Entrypoint | scalar_cardinality |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-026 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select cardinality(arg_1) from app.sample_values;
```

### `cardinality_violation`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.cardinality_violation |
| UUID | 019dffbb-f000-7404-99bc-79fbb47ddc2c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cardinality_violation(...) |
| Return Type Rule | runtime-defined by engine entrypoint cardinality_violation |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_cardinality_violation.v3 |
| AST Binding | ast.expr.scalar_cardinality_violation |
| Engine Entrypoint | cardinality_violation |
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
select cardinality_violation(arg_1) from app.sample_values;
```

### `case_resolution_for_quoted_identifiers`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.case_resolution_for_quoted_identifiers |
| UUID | 019dffbb-f000-704d-9db6-9a552ad793c1 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | case_resolution_for_quoted_identifiers(...) |
| Return Type Rule | runtime-defined by engine entrypoint case_resolution_for_quoted_identifiers |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_case_resolution_for_quoted_identifiers.v3 |
| AST Binding | ast.expr.scalar_case_resolution_for_quoted_identifiers |
| Engine Entrypoint | case_resolution_for_quoted_identifiers |
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
select case_resolution_for_quoted_identifiers(arg_1) from app.sample_values;
```

### `case_when_max_branches`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.case_when_max_branches |
| UUID | 019dffbb-f000-7f8d-b82d-0b0842382ceb |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | case_when_max_branches() |
| Return Type Rule | fixed policy limit for CASE branches; descriptor=uint64 |
| Coercion Rule | no input coercion |
| Null Behavior | never null |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | immutable_policy |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.case_when_max_branches.v3 |
| AST Binding | ast.expr.case_when_max_branches |
| Engine Entrypoint | case_when_max_branches |
| Security Policy | fixed public parser/runtime policy, no catalog or storage authority |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select case_when_max_branches() from app.sample_values;
```

### `catalog`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.catalog |
| UUID | 019dffbb-f001-7424-8a00-000000000424 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | catalog(...) |
| Return Type Rule | runtime-defined by engine entrypoint catalog |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_catalog.v3 |
| AST Binding | ast.expr.scalar_catalog |
| Engine Entrypoint | catalog |
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
select catalog(arg_1) from app.sample_values;
```

### `catalog_object_class`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.catalog_object_class |
| UUID | 019dffbb-f000-76d1-878a-531764cdd04b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | catalog_object_class(...) |
| Return Type Rule | runtime-defined by engine entrypoint catalog_object_class |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_catalog_object_class.v3 |
| AST Binding | ast.expr.scalar_catalog_object_class |
| Engine Entrypoint | catalog_object_class |
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
select catalog_object_class(arg_1) from app.sample_values;
```

### `catalog_object_name`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.catalog_object_name |
| UUID | 019dffbb-f000-75ce-b5a7-713e92314df5 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | catalog_object_name(...) |
| Return Type Rule | runtime-defined by engine entrypoint catalog_object_name |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_catalog_object_name.v3 |
| AST Binding | ast.expr.scalar_catalog_object_name |
| Engine Entrypoint | catalog_object_name |
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
select catalog_object_name(arg_1) from app.sample_values;
```

### `catalog_object_owner`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.catalog_object_owner |
| UUID | 019dffbb-f000-7afe-923f-71c25233cbcf |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | catalog_object_owner(...) |
| Return Type Rule | runtime-defined by engine entrypoint catalog_object_owner |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_catalog_object_owner.v3 |
| AST Binding | ast.expr.scalar_catalog_object_owner |
| Engine Entrypoint | catalog_object_owner |
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
select catalog_object_owner(arg_1) from app.sample_values;
```

### `catalog_object_uuid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.catalog_object_uuid |
| UUID | 019dffbb-f000-7825-8e5f-6e0073818df5 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | catalog_object_uuid(...) |
| Return Type Rule | runtime-defined by engine entrypoint catalog_object_uuid |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_catalog_object_uuid.v3 |
| AST Binding | ast.expr.scalar_catalog_object_uuid |
| Engine Entrypoint | catalog_object_uuid |
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
select catalog_object_uuid(arg_1) from app.sample_values;
```

### `catalog_read`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.catalog_read |
| UUID | 019dffbb-f001-7410-8a00-000000000410 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | catalog_read(...) |
| Return Type Rule | runtime-defined by engine entrypoint catalog_read |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_catalog_read.v3 |
| AST Binding | ast.expr.scalar_catalog_read |
| Engine Entrypoint | catalog_read |
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
select catalog_read(arg_1) from app.sample_values;
```

### `cbrt`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.cbrt |
| UUID | 019dffbb-f000-7423-901a-247ff41a9141 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cbrt(...) |
| Return Type Rule | descriptor-authoritative cbrt numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_cbrt.v3 |
| AST Binding | ast.expr.scalar_cbrt |
| Engine Entrypoint | scalar_cbrt |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select cbrt(arg_1) from app.sample_values;
```

### `ceil`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.ceil |
| UUID | 019de5fc-2400-7f46-9f9f-6640297b05c0 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | ceil(number) |
| Return Type Rule | ceiling by numeric descriptor |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_ceil.v3 |
| AST Binding | ast.expr.scalar_ceil |
| Engine Entrypoint | scalar_ceil |
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
select ceil(numeric_value_1) from app.sample_values;
```

### `char_length`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.char_length |
| UUID | 019dffbb-f001-7021-8a00-000000000021 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | char_length(...) |
| Return Type Rule | descriptor-authoritative char_length text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_char_length.v3 |
| AST Binding | ast.expr.scalar_char_length |
| Engine Entrypoint | scalar_char_length |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select char_length(arg_1) from app.sample_values;
```

### `chr`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.chr |
| UUID | 019dffbb-f001-7025-8a00-000000000025 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | chr(...) |
| Return Type Rule | descriptor-authoritative chr text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_chr.v3 |
| AST Binding | ast.expr.scalar_chr |
| Engine Entrypoint | scalar_chr |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select chr(arg_1) from app.sample_values;
```

### `client_addr`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.client_addr |
| UUID | 019dffbb-f000-7996-becf-3029c11abfe2 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | client_addr(...) |
| Return Type Rule | runtime-defined by engine entrypoint client_addr |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_client_addr.v3 |
| AST Binding | ast.expr.scalar_client_addr |
| Engine Entrypoint | client_addr |
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
select client_addr(arg_1) from app.sample_values;
```

### `client_address`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.client_address |
| UUID | 019dffbb-f001-7430-8a00-000000000430 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | client_address(...) |
| Return Type Rule | runtime-defined by engine entrypoint client_address |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_client_address.v3 |
| AST Binding | ast.expr.scalar_client_address |
| Engine Entrypoint | client_address |
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
select client_address(arg_1) from app.sample_values;
```

### `client_min_messages`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.client_min_messages |
| UUID | 019dffbb-f001-7401-8a00-000000000401 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | client_min_messages(...) |
| Return Type Rule | runtime-defined by engine entrypoint client_min_messages |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_client_min_messages.v3 |
| AST Binding | ast.expr.scalar_client_min_messages |
| Engine Entrypoint | client_min_messages |
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
select client_min_messages(arg_1) from app.sample_values;
```

### `client_min_messages_default`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.client_min_messages_default |
| UUID | 019dffbb-f000-7602-a98e-d3aad900286c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | client_min_messages_default(...) |
| Return Type Rule | runtime-defined by engine entrypoint client_min_messages_default |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_client_min_messages_default.v3 |
| AST Binding | ast.expr.scalar_client_min_messages_default |
| Engine Entrypoint | client_min_messages_default |
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
select client_min_messages_default(arg_1) from app.sample_values;
```

### `client_port`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.client_port |
| UUID | 019dffbb-f000-7305-8e48-d006511f7298 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | client_port(...) |
| Return Type Rule | runtime-defined by engine entrypoint client_port |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_client_port.v3 |
| AST Binding | ast.expr.scalar_client_port |
| Engine Entrypoint | client_port |
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
select client_port(arg_1) from app.sample_values;
```

### `client_protocol`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.client_protocol |
| UUID | 019dffbb-f000-7c4e-b36c-a2faa59e4c27 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | client_protocol(...) |
| Return Type Rule | runtime-defined by engine entrypoint client_protocol |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_client_protocol.v3 |
| AST Binding | ast.expr.scalar_client_protocol |
| Engine Entrypoint | client_protocol |
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
select client_protocol(arg_1) from app.sample_values;
```

### `close`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.close |
| UUID | 019dffbb-f000-7972-9d40-50925831c7d0 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | close(...) |
| Return Type Rule | runtime-defined by engine entrypoint close |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_close.v3 |
| AST Binding | ast.expr.scalar_close |
| Engine Entrypoint | close |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select close(arg_1) from app.sample_values;
```

### `coalesce`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.coalesce |
| UUID | 019de5fc-2400-7b72-aff3-426c3851b937 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | coalesce(args...) |
| Return Type Rule | first non-null expression |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | stable_by_arguments |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_coalesce.v3 |
| AST Binding | ast.expr.scalar_coalesce |
| Engine Entrypoint | scalar_coalesce |
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
select coalesce(arg_1) from app.sample_values;
```

### `coalesce_strict`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.coalesce_strict |
| UUID | 019de5fc-2400-7bf0-8c12-026000000001 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | coalesce_strict(...) |
| Return Type Rule | descriptor-authoritative coalesce_strict result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for native function/operator operands |
| Null Behavior | function/operator-specific NULL handling follows SBSFC-026 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | stable_by_arguments |
| Determinism | foldable only when inputs are constants with stable descriptors and function-specific context is fixed |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_coalesce_strict.v3 |
| AST Binding | ast.expr.scalar_coalesce_strict |
| Engine Entrypoint | scalar_coalesce_strict |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-026 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select coalesce_strict(arg_1) from app.sample_values;
```

### `collation_for`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.collation_for |
| UUID | 019dffbb-f000-79cb-b25a-5fa3b068c504 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | collation_for(...) |
| Return Type Rule | runtime-defined by engine entrypoint collation_for |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_collation_for.v3 |
| AST Binding | ast.expr.scalar_collation_for |
| Engine Entrypoint | collation_for |
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
select collation_for(arg_1) from app.sample_values;
```

### `colocation`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.colocation |
| UUID | 019dffbb-f001-7403-8a00-000000000403 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | colocation(...) |
| Return Type Rule | runtime-defined by engine entrypoint colocation |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_colocation.v3 |
| AST Binding | ast.expr.scalar_colocation |
| Engine Entrypoint | colocation |
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
select colocation(arg_1) from app.sample_values;
```

### `column_descriptor`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.column_descriptor |
| UUID | 019dffbb-f000-72dd-ae48-cd39c1b9560b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | column_descriptor(...) |
| Return Type Rule | runtime-defined by engine entrypoint column_descriptor |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_column_descriptor.v3 |
| AST Binding | ast.expr.scalar_column_descriptor |
| Engine Entrypoint | column_descriptor |
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
select column_descriptor(arg_1) from app.sample_values;
```

### `comment_block`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.comment_block |
| UUID | 019dffbb-f000-7327-9d1b-c5d80438520a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | comment_block(...) |
| Return Type Rule | runtime-defined by engine entrypoint comment_block |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_comment_block.v3 |
| AST Binding | ast.expr.scalar_comment_block |
| Engine Entrypoint | comment_block |
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
select comment_block(arg_1) from app.sample_values;
```

### `comment_line`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.comment_line |
| UUID | 019dffbb-f000-7abd-a472-56a337d223b8 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | comment_line(...) |
| Return Type Rule | runtime-defined by engine entrypoint comment_line |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_comment_line.v3 |
| AST Binding | ast.expr.scalar_comment_line |
| Engine Entrypoint | comment_line |
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
select comment_line(arg_1) from app.sample_values;
```

### `comparison_collation_resolution`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.comparison_collation_resolution |
| UUID | 019dffbb-f000-7908-8b18-d1c961b3df2e |
| Kind | scalar |
| Syntax Forms | function_call, keyword_or_function_call |
| Overloads | comparison_collation_resolution(...) |
| Return Type Rule | descriptor-authoritative comparison_collation_resolution language/profile metadata result as implemented by the SBLR expression runtime |
| Coercion Rule | no input coercion unless the fixture surface supplies descriptor-authoritative arguments |
| Null Behavior | not applicable for nullary metadata forms; strict for argument-bearing forms |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | stable_per_catalog_profile |
| Determinism | profile-backed; foldable after binding when the active catalog/profile is fixed |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_comparison_collation_resolution.v3 |
| AST Binding | ast.expr.scalar_comparison_collation_resolution |
| Engine Entrypoint | scalar_comparison_collation_resolution |
| Security Policy | reads fixed language/catalog profile metadata only; no user data access |
| Error Semantics | arity/type errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select comparison_collation_resolution(arg_1) from app.sample_values;
```

### `concat`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.concat |
| UUID | 019de5fc-2400-708f-84cc-0dbfc983e6d9 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | concat(args...) |
| Return Type Rule | concatenate with donor-profile null behavior |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_concat.v3 |
| AST Binding | ast.expr.scalar_concat |
| Engine Entrypoint | scalar_concat |
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
select concat(arg_1) from app.sample_values;
```

### `concat_ws`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.concat_ws |
| UUID | 019dffbb-f000-7a6d-a36a-5ad2e4f3215e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | concat_ws(separator,value[,value...]) |
| Return Type Rule | bounded character value formed by joining non-NULL value arguments with the separator |
| Coercion Rule | separator and emitted values use scalar text representation through the descriptor implicit cast matrix |
| Null Behavior | NULL separator returns NULL character; NULL value arguments are skipped |
| Collation/Charset Rule | result uses implementation default character descriptor unless a stricter caller descriptor is supplied |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when separator and all value arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_concat_ws.v3 |
| AST Binding | ast.expr.scalar_concat_ws |
| Engine Entrypoint | concat_ws |
| Security Policy | pure string helper; no catalog, storage, security, transaction, donor, plugin, or cluster authority |
| Error Semantics | invalid arity or unsupported descriptor conversion refuses with SB_DIAG_FUNCTION_INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select concat_ws(arg_1, value_2) from app.sample_values;
```

### `context_ambiguous`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.context_ambiguous |
| UUID | 019dffbb-f001-744a-8a00-00000000044a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | context_ambiguous(...) |
| Return Type Rule | runtime-defined by engine entrypoint context_ambiguous |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_context_ambiguous.v3 |
| AST Binding | ast.expr.scalar_context_ambiguous |
| Engine Entrypoint | context_ambiguous |
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
select context_ambiguous(arg_1) from app.sample_values;
```

### `contextual_native_keyword`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.contextual_native_keyword |
| UUID | 019dffbb-f000-7ebe-8f1a-787d697be021 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | contextual_native_keyword(...) |
| Return Type Rule | runtime-defined by engine entrypoint contextual_native_keyword |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_contextual_native_keyword.v3 |
| AST Binding | ast.expr.scalar_contextual_native_keyword |
| Engine Entrypoint | contextual_native_keyword |
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
select contextual_native_keyword(arg_1) from app.sample_values;
```

### `convert`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.convert |
| UUID | 019dffbb-f000-78e0-ab08-a8bacd85e742 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | convert(...) |
| Return Type Rule | descriptor-authoritative convert result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for native function/operator operands |
| Null Behavior | function/operator-specific NULL handling follows SBSFC-026 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | stable_by_arguments |
| Determinism | foldable only when inputs are constants with stable descriptors and function-specific context is fixed |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_convert.v3 |
| AST Binding | ast.expr.scalar_convert |
| Engine Entrypoint | convert |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-026 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select convert(arg_1) from app.sample_values;
```

### `convert_from`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.convert_from |
| UUID | 019dffbb-f000-7560-bb18-5ddc1e00aa00 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | convert_from(binary,encoding) |
| Return Type Rule | character value decoded from a valid UTF-8 binary payload |
| Coercion Rule | first argument must be binary; encoding must be UTF8 or UTF-8 |
| Null Behavior | NULL input binary or encoding returns NULL character |
| Collation/Charset Rule | accepts UTF8 and UTF-8 spelling only in this native surface; no broad charset transcoding is claimed |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when binary and encoding arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_convert_from.v3 |
| AST Binding | ast.expr.scalar_convert_from |
| Engine Entrypoint | convert_from |
| Security Policy | pure encoding helper; no catalog, storage, security, transaction, donor, plugin, or cluster authority |
| Error Semantics | invalid arity, non-binary input, unsupported encoding, or invalid UTF-8 bytes refuse with SB_DIAG_FUNCTION_INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select convert_from(binary_value_1, arg_2) from app.sample_values;
```

### `convert_to`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.convert_to |
| UUID | 019dffbb-f000-71ea-a89f-1732a1752d34 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | convert_to(text,encoding) |
| Return Type Rule | binary payload containing the UTF-8 bytes of the input text |
| Coercion Rule | first argument uses scalar text representation; encoding must be UTF8 or UTF-8 |
| Null Behavior | NULL input text or encoding returns NULL binary |
| Collation/Charset Rule | accepts UTF8 and UTF-8 spelling only in this native surface; no broad charset transcoding is claimed |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when text and encoding arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_convert_to.v3 |
| AST Binding | ast.expr.scalar_convert_to |
| Engine Entrypoint | convert_to |
| Security Policy | pure encoding helper; no catalog, storage, security, transaction, donor, plugin, or cluster authority |
| Error Semantics | invalid arity, non-text input, or unsupported encoding refuses with SB_DIAG_FUNCTION_INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select convert_to(text_value_1, arg_2) from app.sample_values;
```

### `cos`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.cos |
| UUID | 019dffbb-f001-7002-8a00-000000000002 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cos(...) |
| Return Type Rule | descriptor-authoritative cos numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_cos.v3 |
| AST Binding | ast.expr.scalar_cos |
| Engine Entrypoint | scalar_cos |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select cos(arg_1) from app.sample_values;
```

### `cosd`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.cosd |
| UUID | 019dffbb-f001-7034-8a00-000000000034 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cosd(...) |
| Return Type Rule | descriptor-authoritative cosd numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_cosd.v3 |
| AST Binding | ast.expr.scalar_cosd |
| Engine Entrypoint | scalar_cosd |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select cosd(arg_1) from app.sample_values;
```

### `cosh`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.cosh |
| UUID | 019dffbb-f001-702c-8a00-00000000002c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cosh(...) |
| Return Type Rule | descriptor-authoritative cosh numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_cosh.v3 |
| AST Binding | ast.expr.scalar_cosh |
| Engine Entrypoint | scalar_cosh |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select cosh(arg_1) from app.sample_values;
```

### `cot`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.cot |
| UUID | 019dffbb-f001-7039-8a00-000000000039 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cot(...) |
| Return Type Rule | descriptor-authoritative cot numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_cot.v3 |
| AST Binding | ast.expr.scalar_cot |
| Engine Entrypoint | scalar_cot |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select cot(arg_1) from app.sample_values;
```

### `cotd`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.cotd |
| UUID | 019dffbb-f001-703a-8a00-00000000003a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cotd(...) |
| Return Type Rule | descriptor-authoritative cotd numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_cotd.v3 |
| AST Binding | ast.expr.scalar_cotd |
| Engine Entrypoint | scalar_cotd |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select cotd(arg_1) from app.sample_values;
```

### `count_distinct_includes_null`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.count_distinct_includes_null |
| UUID | 019dffbb-f000-71dd-88b9-7b26398e3d20 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | count_distinct_includes_null(...) |
| Return Type Rule | runtime-defined by engine entrypoint count_distinct_includes_null |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_count_distinct_includes_null.v3 |
| AST Binding | ast.expr.scalar_count_distinct_includes_null |
| Engine Entrypoint | count_distinct_includes_null |
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
select count_distinct_includes_null(arg_1) from app.sample_values;
```

### `crc32`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.crc32 |
| UUID | 019dffbb-f000-7379-ac72-6f60e7ac2f7e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | crc32(...) |
| Return Type Rule | runtime-defined by engine entrypoint crc32 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_crc32.v3 |
| AST Binding | ast.expr.scalar_crc32 |
| Engine Entrypoint | crc32 |
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
select crc32(arg_1) from app.sample_values;
```

### `crc32c`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.crc32c |
| UUID | 019dffbb-f000-7594-8e65-83befd1b30fd |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | crc32c(...) |
| Return Type Rule | runtime-defined by engine entrypoint crc32c |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_crc32c.v3 |
| AST Binding | ast.expr.scalar_crc32c |
| Engine Entrypoint | crc32c |
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
select crc32c(arg_1) from app.sample_values;
```

### `create`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.create |
| UUID | 019dffbb-f000-740d-ac37-771ee0b91cfb |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | create(...) |
| Return Type Rule | runtime-defined by engine entrypoint create |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_create.v3 |
| AST Binding | ast.expr.scalar_create |
| Engine Entrypoint | create |
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
select create(arg_1) from app.sample_values;
```

### `cross`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.cross |
| UUID | 019dffbb-f000-71d8-98c6-0cd40b22938f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cross(...) |
| Return Type Rule | runtime-defined by engine entrypoint cross |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_cross.v3 |
| AST Binding | ast.expr.scalar_cross |
| Engine Entrypoint | cross |
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
select cross(arg_1) from app.sample_values;
```

### `cte_max_count_per_statement`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.cte_max_count_per_statement |
| UUID | 019dffbb-f000-740b-ac63-e605e0c70e4f |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | cte_max_count_per_statement() |
| Return Type Rule | fixed policy limit for CTE count per statement; descriptor=uint64 |
| Coercion Rule | no input coercion |
| Null Behavior | never null |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | immutable_policy |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.cte_max_count_per_statement.v3 |
| AST Binding | ast.expr.cte_max_count_per_statement |
| Engine Entrypoint | cte_max_count_per_statement |
| Security Policy | fixed public parser/runtime policy, no catalog or storage authority |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select cte_max_count_per_statement() from app.sample_values;
```

### `currency`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.currency |
| UUID | 019dffbb-f001-7400-8a00-000000000400 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | currency(...) |
| Return Type Rule | runtime-defined by engine entrypoint currency |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_currency.v3 |
| AST Binding | ast.expr.scalar_currency |
| Engine Entrypoint | currency |
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
select currency(arg_1) from app.sample_values;
```

### `current_capability_set`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.current_capability_set |
| UUID | 019dffbb-f000-7aaa-88a3-9cee2d24206e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | current_capability_set(...) |
| Return Type Rule | runtime-defined by engine entrypoint current_capability_set |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_current_capability_set.v3 |
| AST Binding | ast.expr.scalar_current_capability_set |
| Engine Entrypoint | current_capability_set |
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
select current_capability_set(arg_1) from app.sample_values;
```

### `current_catalog`

| Property | Value |
| --- | --- |
| Builtin ID | sb.session.current_catalog |
| UUID | 019de5fc-2400-7144-a089-1dcc5afb0350 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | current_catalog |
| Return Type Rule | current database/catalog projection |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | stable_per_session |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.session_current_catalog.v3 |
| AST Binding | ast.expr.session_current_catalog |
| Engine Entrypoint | session_current_catalog |
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
select current_catalog() from app.sample_values;
```

### `current_database`

| Property | Value |
| --- | --- |
| Builtin ID | sb.session.current_database |
| UUID | 019dffbb-f001-701a-8a00-00000000001a |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | current_database |
| Return Type Rule | current database/catalog UUID from SBLR execution context; descriptor=uuid |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | stable_per_session |
| Determinism | not foldable; value is supplied by statement/session/transaction execution context or volatile time provider |
| Side Effects | none |
| SBLR Binding | sblr.expr.session_current_database.v3 |
| AST Binding | ast.expr.session_current_database |
| Engine Entrypoint | session_current_database |
| Security Policy | session/catalog context read from SBLR execution context only |
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
select current_database() from app.sample_values;
```

### `current_dialect_version`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.current_dialect_version |
| UUID | 019dffbb-f000-7639-ad8e-d0e80b2ccc1a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | current_dialect_version(...) |
| Return Type Rule | runtime-defined by engine entrypoint current_dialect_version |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_current_dialect_version.v3 |
| AST Binding | ast.expr.scalar_current_dialect_version |
| Engine Entrypoint | current_dialect_version |
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
select current_dialect_version(arg_1) from app.sample_values;
```

### `current_engine_version`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.current_engine_version |
| UUID | 019dffbb-f000-7ac8-a771-0b934399b674 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | current_engine_version(...) |
| Return Type Rule | runtime-defined by engine entrypoint current_engine_version |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_current_engine_version.v3 |
| AST Binding | ast.expr.scalar_current_engine_version |
| Engine Entrypoint | current_engine_version |
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
select current_engine_version(arg_1) from app.sample_values;
```

### `current_isolation_level`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.current_isolation_level |
| UUID | 019dffbb-f000-7af8-b840-8b4bbc9a627a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | current_isolation_level(...) |
| Return Type Rule | runtime-defined by engine entrypoint current_isolation_level |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_current_isolation_level.v3 |
| AST Binding | ast.expr.scalar_current_isolation_level |
| Engine Entrypoint | current_isolation_level |
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
select current_isolation_level(arg_1) from app.sample_values;
```

### `current_locale`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.current_locale |
| UUID | 019dffbb-f000-7683-b1f1-21914e8ead2c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | current_locale(...) |
| Return Type Rule | runtime-defined by engine entrypoint current_locale |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_current_locale.v3 |
| AST Binding | ast.expr.scalar_current_locale |
| Engine Entrypoint | current_locale |
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
select current_locale(arg_1) from app.sample_values;
```

### `current_request_uuid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.current_request_uuid |
| UUID | 019dffbb-f000-786d-92ab-eb08de430371 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | current_request_uuid(...) |
| Return Type Rule | runtime-defined by engine entrypoint current_request_uuid |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_current_request_uuid.v3 |
| AST Binding | ast.expr.scalar_current_request_uuid |
| Engine Entrypoint | current_request_uuid |
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
select current_request_uuid(arg_1) from app.sample_values;
```

### `current_role`

| Property | Value |
| --- | --- |
| Builtin ID | sb.session.current_role |
| UUID | 019dffbb-f001-701b-8a00-00000000001b |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | current_role |
| Return Type Rule | current active role UUID from SBLR execution context; descriptor=uuid |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless context provider returns typed null as specified by SBSFC-012 fixture evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply; otherwise not applicable |
| Timezone Rule | session timezone/current timestamp context where temporal conversion requires it; otherwise not applicable |
| Volatility | stable_per_session |
| Determinism | not foldable; value is supplied by statement/session/transaction execution context or volatile time provider |
| Side Effects | none |
| SBLR Binding | sblr.expr.session_current_role.v3 |
| AST Binding | ast.expr.session_current_role |
| Engine Entrypoint | session_current_role |
| Security Policy | session/security context read from SBLR execution context only |
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
select current_role() from app.sample_values;
```

### `current_schema`

| Property | Value |
| --- | --- |
| Builtin ID | sb.session.current_schema |
| UUID | 019de5fc-2400-7497-8f18-f637735d00ce |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | current_schema |
| Return Type Rule | current schema UUID/name projection |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | stable_per_statement |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.session_current_schema.v3 |
| AST Binding | ast.expr.session_current_schema |
| Engine Entrypoint | session_current_schema |
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
select current_schema() from app.sample_values;
```

### `current_server`

| Property | Value |
| --- | --- |
| Builtin ID | sb.session.current_server |
| UUID | 019dffbb-f000-7628-a3f0-51f0900aa4d8 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | current_server() |
| Return Type Rule | uuid text from SblrExecutionContext.node_uuid; null uuid when no node UUID is present |
| Coercion Rule | no input coercion |
| Null Behavior | not applicable |
| Collation/Charset Rule | UUID descriptor; no collation semantics |
| Timezone Rule | not applicable |
| Volatility | stable_per_session |
| Determinism | context-backed; not foldable |
| Side Effects | none |
| SBLR Binding | sblr.expr.current_server.v3 |
| AST Binding | ast.expr.current_server |
| Engine Entrypoint | current_server |
| Security Policy | reads local node identity already present in SBLR execution context; no cluster route or server catalog lookup is performed |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select current_server() from app.sample_values;
```

### `current_session_id`

| Property | Value |
| --- | --- |
| Builtin ID | sb.session.session_id |
| UUID | 019dffbb-f000-7935-a6ca-3a130e48b23b |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | current_session_id |
| Return Type Rule | session UUID from SBLR execution context; descriptor=uuid |
| Coercion Rule | no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted |
| Null Behavior | context-backed typed null or diagnostic behavior follows SBSFC-016 row evidence |
| Collation/Charset Rule | character descriptors use byte-stable ASCII fixture values unless context supplies text |
| Timezone Rule | not applicable unless the context value is timezone-derived |
| Volatility | stable_per_session |
| Determinism | not foldable for execution-context reads or procedural diagnostics |
| Side Effects | none |
| SBLR Binding | sblr.expr.session_id.v3 |
| AST Binding | ast.expr.session_id |
| Engine Entrypoint | session_id |
| Security Policy | reads session UUID context only |
| Error Semantics | arity/type/context/procedural diagnostics use builtin error compatibility matrix and SBSFC-016 evidence; diagnostic=SBSQL.FUNCTION.INVALID_INPUT for arity/type/context violations where applicable |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select current_session_id() from app.sample_values;
```

### `current_setting`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.current_setting |
| UUID | 019dffbb-f000-7bf7-b9ee-0185aaef7d6a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | current_setting(name[,missing_ok]) |
| Return Type Rule | character descriptor for known fixed-policy settings; null character when missing_ok is true and setting is unknown; recognized setting names in this slice are timezone and time zone |
| Coercion Rule | name coerces to text; missing_ok must be boolean-compatible |
| Null Behavior | null name or null missing_ok returns null character |
| Collation/Charset Rule | setting names are compared by ASCII lowercase spelling |
| Timezone Rule | current_setting('timezone') returns fixed UTC policy |
| Volatility | stable_per_session |
| Determinism | fixed policy for recognized names; unknown-name behavior is deterministic |
| Side Effects | none |
| SBLR Binding | sblr.expr.current_setting.v3 |
| AST Binding | ast.expr.current_setting |
| Engine Entrypoint | current_setting |
| Security Policy | no mutable server setting catalog is read in this slice |
| Error Semantics | arity must be 1 or 2; unknown setting refuses with SBSQL.FUNCTION.INVALID_INPUT unless missing_ok is true; invalid missing_ok refuses with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select current_setting(arg_1) from app.sample_values;
```

### `current_setting_timezone`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.current_setting_timezone |
| UUID | 019dffbb-f000-71f7-8d7e-6c551c444bad |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | current_setting_timezone() |
| Return Type Rule | fixed character descriptor value UTC for this implementation slice |
| Coercion Rule | literal-only surface binds to nullary engine entrypoint |
| Null Behavior | never null |
| Collation/Charset Rule | character descriptor with byte-stable ASCII content |
| Timezone Rule | exposes fixed UTC session timezone policy for standalone context-backed execution |
| Volatility | stable_per_session |
| Determinism | fixed policy constant in this slice; foldable after binding |
| Side Effects | none |
| SBLR Binding | sblr.expr.current_setting_timezone.v3 |
| AST Binding | ast.expr.current_setting_timezone |
| Engine Entrypoint | current_setting_timezone |
| Security Policy | none |
| Error Semantics | bound nullary form must have arity 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select current_setting_timezone() from app.sample_values;
```

### `current_statement_uuid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.session.current_statement_uuid |
| UUID | 019dffbb-f000-7c48-8b63-33eaa54acb6b |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | current_statement_uuid |
| Return Type Rule | statement UUID from SBLR execution context; descriptor=uuid |
| Coercion Rule | no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted |
| Null Behavior | context-backed typed null or diagnostic behavior follows SBSFC-016 row evidence |
| Collation/Charset Rule | character descriptors use byte-stable ASCII fixture values unless context supplies text |
| Timezone Rule | not applicable unless the context value is timezone-derived |
| Volatility | stable_per_statement |
| Determinism | not foldable for execution-context reads or procedural diagnostics |
| Side Effects | none |
| SBLR Binding | sblr.expr.current_statement_uuid.v3 |
| AST Binding | ast.expr.current_statement_uuid |
| Engine Entrypoint | current_statement_uuid |
| Security Policy | reads statement UUID context only |
| Error Semantics | arity/type/context/procedural diagnostics use builtin error compatibility matrix and SBSFC-016 evidence; diagnostic=SBSQL.FUNCTION.INVALID_INPUT for arity/type/context violations where applicable |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select current_statement_uuid() from app.sample_values;
```

### `current_transaction_id`

| Property | Value |
| --- | --- |
| Builtin ID | sb.session.transaction_id |
| UUID | 019dffbb-f000-7009-a202-80afad786275 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | current_transaction_id |
| Return Type Rule | local transaction id from SBLR transaction context as uint64; descriptor=uint64 |
| Coercion Rule | no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted |
| Null Behavior | context-backed typed null or diagnostic behavior follows SBSFC-016 row evidence |
| Collation/Charset Rule | character descriptors use byte-stable ASCII fixture values unless context supplies text |
| Timezone Rule | not applicable unless the context value is timezone-derived |
| Volatility | stable_per_transaction |
| Determinism | not foldable for execution-context reads or procedural diagnostics |
| Side Effects | none |
| SBLR Binding | sblr.expr.transaction_id.v3 |
| AST Binding | ast.expr.transaction_id |
| Engine Entrypoint | transaction_id |
| Security Policy | reads active transaction context only; no parser-side finality |
| Error Semantics | arity/type/context/procedural diagnostics use builtin error compatibility matrix and SBSFC-016 evidence; diagnostic=SBSQL.FUNCTION.INVALID_INPUT for arity/type/context violations where applicable |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select current_transaction_id() from app.sample_values;
```

### `current_user`

| Property | Value |
| --- | --- |
| Builtin ID | sb.session.current_user |
| UUID | 019de5fc-2400-7982-9548-2d05274e3769 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | current_user |
| Return Type Rule | session security principal |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | stable_per_session |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.session_current_user.v3 |
| AST Binding | ast.expr.session_current_user |
| Engine Entrypoint | session_current_user |
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
select current_user() from app.sample_values;
```

### `currval`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.currval |
| UUID | 019dffbb-f000-7904-b642-d27764dfbc40 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | currval(...) |
| Return Type Rule | runtime-defined by engine entrypoint currval |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_currval.v3 |
| AST Binding | ast.expr.scalar_currval |
| Engine Entrypoint | currval |
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
select currval(arg_1) from app.sample_values;
```

### `customer`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.customer |
| UUID | 019dffbb-f001-7458-8a00-000000000458 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | customer(...) |
| Return Type Rule | runtime-defined by engine entrypoint customer |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_customer.v3 |
| AST Binding | ast.expr.scalar_customer |
| Engine Entrypoint | customer |
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
select customer(arg_1) from app.sample_values;
```

### `customer_id`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.customer_id |
| UUID | 019dffbb-f001-7436-8a00-000000000436 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | customer_id(...) |
| Return Type Rule | runtime-defined by engine entrypoint customer_id |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_customer_id.v3 |
| AST Binding | ast.expr.scalar_customer_id |
| Engine Entrypoint | customer_id |
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
select customer_id(arg_1) from app.sample_values;
```

### `customers`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.customers |
| UUID | 019dffbb-f001-7437-8a00-000000000437 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | customers(...) |
| Return Type Rule | runtime-defined by engine entrypoint customers |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_customers.v3 |
| AST Binding | ast.expr.scalar_customers |
| Engine Entrypoint | customers |
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
select customers(arg_1) from app.sample_values;
```

### `damerau_levenshtein`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.damerau_levenshtein |
| UUID | 019dffbb-f000-73b9-bed4-06e7aabc6c02 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | damerau_levenshtein(...) |
| Return Type Rule | descriptor-authoritative damerau_levenshtein fuzzy/phonetic scalar result as implemented by the SBLR expression runtime; fixture result family int64 |
| Coercion Rule | descriptor implicit cast matrix for text and bounded integer arguments |
| Null Behavior | strict unless SBSFC-014 fuzzy/phonetic row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for text normalization where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and locale/regex versions |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_damerau_levenshtein.v3 |
| AST Binding | ast.expr.scalar_damerau_levenshtein |
| Engine Entrypoint | sb_engine_functions.damerau_levenshtein |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select damerau_levenshtein(arg_1) from app.sample_values;
```

### `decision_proof_required`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.decision_proof_required |
| UUID | 019dffbb-f000-7f7e-813e-2ac2b5dae626 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | decision_proof_required(...) |
| Return Type Rule | runtime-defined by engine entrypoint decision_proof_required |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_decision_proof_required.v3 |
| AST Binding | ast.expr.scalar_decision_proof_required |
| Engine Entrypoint | decision_proof_required |
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
select decision_proof_required(arg_1) from app.sample_values;
```

### `decode`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.decode |
| UUID | 019dffbb-f000-795e-97b8-a3e8c5745c2a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | decode(...) |
| Return Type Rule | descriptor-authoritative decode encoding/binary conversion result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary encoding inputs |
| Null Behavior | strict unless SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_decode.v3 |
| AST Binding | ast.expr.scalar_decode |
| Engine Entrypoint | scalar_decode |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/format errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select decode(arg_1) from app.sample_values;
```

### `default_charset`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.default_charset |
| UUID | 019dffbb-f000-7081-b6ba-548ddc5e3f55 |
| Kind | scalar |
| Syntax Forms | function_call, keyword_or_function_call |
| Overloads | default_charset(...) |
| Return Type Rule | descriptor-authoritative default_charset language/profile metadata result as implemented by the SBLR expression runtime |
| Coercion Rule | no input coercion unless the fixture surface supplies descriptor-authoritative arguments |
| Null Behavior | not applicable for nullary metadata forms; strict for argument-bearing forms |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | stable_per_catalog_profile |
| Determinism | profile-backed; foldable after binding when the active catalog/profile is fixed |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_default_charset.v3 |
| AST Binding | ast.expr.scalar_default_charset |
| Engine Entrypoint | scalar_default_charset |
| Security Policy | reads fixed language/catalog profile metadata only; no user data access |
| Error Semantics | arity/type errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select default_charset(arg_1) from app.sample_values;
```

### `default_collation`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.default_collation |
| UUID | 019dffbb-f000-73a7-b0c5-f86eb90bd426 |
| Kind | scalar |
| Syntax Forms | function_call, keyword_or_function_call |
| Overloads | default_collation(...) |
| Return Type Rule | descriptor-authoritative default_collation language/profile metadata result as implemented by the SBLR expression runtime |
| Coercion Rule | no input coercion unless the fixture surface supplies descriptor-authoritative arguments |
| Null Behavior | not applicable for nullary metadata forms; strict for argument-bearing forms |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | stable_per_catalog_profile |
| Determinism | profile-backed; foldable after binding when the active catalog/profile is fixed |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_default_collation.v3 |
| AST Binding | ast.expr.scalar_default_collation |
| Engine Entrypoint | scalar_default_collation |
| Security Policy | reads fixed language/catalog profile metadata only; no user data access |
| Error Semantics | arity/type errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select default_collation(arg_1) from app.sample_values;
```

### `default_decimal_division_scale`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.default_decimal_division_scale |
| UUID | 019dffbb-f000-7d9a-943d-794fcadbcac4 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | default_decimal_division_scale(...) |
| Return Type Rule | runtime-defined by engine entrypoint default_decimal_division_scale |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_default_decimal_division_scale.v3 |
| AST Binding | ast.expr.scalar_default_decimal_division_scale |
| Engine Entrypoint | default_decimal_division_scale |
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
select default_decimal_division_scale(arg_1) from app.sample_values;
```

### `default_schema_resolution`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.default_schema_resolution |
| UUID | 019dffbb-f000-7be2-ad03-c81971f43bf4 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | default_schema_resolution(...) |
| Return Type Rule | runtime-defined by engine entrypoint default_schema_resolution |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_default_schema_resolution.v3 |
| AST Binding | ast.expr.scalar_default_schema_resolution |
| Engine Entrypoint | default_schema_resolution |
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
select default_schema_resolution(arg_1) from app.sample_values;
```

### `degrees`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.degrees |
| UUID | 019dffbb-f000-7db5-8efa-d73a23a6e511 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | degrees(...) |
| Return Type Rule | descriptor-authoritative degrees numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_degrees.v3 |
| AST Binding | ast.expr.scalar_degrees |
| Engine Entrypoint | scalar_degrees |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select degrees(arg_1) from app.sample_values;
```

### `delimited_identifier_max_length_bytes`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.delimited_identifier_max_length_bytes |
| UUID | 019dffbb-f000-74a6-b510-9e63d9754b5e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | delimited_identifier_max_length_bytes(...) |
| Return Type Rule | runtime-defined by engine entrypoint delimited_identifier_max_length_bytes |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_delimited_identifier_max_length_bytes.v3 |
| AST Binding | ast.expr.scalar_delimited_identifier_max_length_bytes |
| Engine Entrypoint | delimited_identifier_max_length_bytes |
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
select delimited_identifier_max_length_bytes(arg_1) from app.sample_values;
```

### `deprecated`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.deprecated |
| UUID | 019dffbb-f001-740c-8a00-00000000040c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | deprecated(...) |
| Return Type Rule | runtime-defined by engine entrypoint deprecated |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_deprecated.v3 |
| AST Binding | ast.expr.scalar_deprecated |
| Engine Entrypoint | deprecated |
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
select deprecated(arg_1) from app.sample_values;
```

### `deprecated_keyword`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.deprecated_keyword |
| UUID | 019dffbb-f000-7829-8627-944a4e624d58 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | deprecated_keyword(...) |
| Return Type Rule | runtime-defined by engine entrypoint deprecated_keyword |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_deprecated_keyword.v3 |
| AST Binding | ast.expr.scalar_deprecated_keyword |
| Engine Entrypoint | deprecated_keyword |
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
select deprecated_keyword(arg_1) from app.sample_values;
```

### `deprecation_warning`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.deprecation_warning |
| UUID | 019dffbb-f001-744e-8a00-00000000044e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | deprecation_warning(...) |
| Return Type Rule | runtime-defined by engine entrypoint deprecation_warning |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_deprecation_warning.v3 |
| AST Binding | ast.expr.scalar_deprecation_warning |
| Engine Entrypoint | deprecation_warning |
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
select deprecation_warning(arg_1) from app.sample_values;
```

### `descriptor`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.descriptor |
| UUID | 019dffbb-f001-7422-8a00-000000000422 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | descriptor(...) |
| Return Type Rule | runtime-defined by engine entrypoint descriptor |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_descriptor.v3 |
| AST Binding | ast.expr.scalar_descriptor |
| Engine Entrypoint | descriptor |
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
select descriptor(arg_1) from app.sample_values;
```

### `descriptor_of`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.descriptor_of |
| UUID | 019dffbb-f000-7c57-9ab9-4d8c52b211d0 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | descriptor_of(...) |
| Return Type Rule | runtime-defined by engine entrypoint descriptor_of |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_descriptor_of.v3 |
| AST Binding | ast.expr.scalar_descriptor_of |
| Engine Entrypoint | descriptor_of |
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
select descriptor_of(arg_1) from app.sample_values;
```

### `descriptor_snapshot_id`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.descriptor_snapshot_id |
| UUID | 019dffbb-f000-7d36-bf61-72440e712805 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | descriptor_snapshot_id(...) |
| Return Type Rule | runtime-defined by engine entrypoint descriptor_snapshot_id |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_descriptor_snapshot_id.v3 |
| AST Binding | ast.expr.scalar_descriptor_snapshot_id |
| Engine Entrypoint | descriptor_snapshot_id |
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
select descriptor_snapshot_id(arg_1) from app.sample_values;
```

### `diag_sqlstate`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.diag_sqlstate |
| UUID | 019dffbb-f001-744c-8a00-00000000044c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | diag_sqlstate(...) |
| Return Type Rule | runtime-defined by engine entrypoint diag_sqlstate |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_diag_sqlstate.v3 |
| AST Binding | ast.expr.scalar_diag_sqlstate |
| Engine Entrypoint | diag_sqlstate |
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
select diag_sqlstate(arg_1) from app.sample_values;
```

### `diagnostic_count`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.diagnostic_count |
| UUID | 019dffbb-f000-7787-abfc-3eeeb5f6de45 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | diagnostic_count(...) |
| Return Type Rule | runtime-defined by engine entrypoint diagnostic_count |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_diagnostic_count.v3 |
| AST Binding | ast.expr.scalar_diagnostic_count |
| Engine Entrypoint | diagnostic_count |
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
select diagnostic_count(arg_1) from app.sample_values;
```

### `diagnostic_field`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.diagnostic_field |
| UUID | 019dffbb-f000-78ec-ba3d-6cf407c80d7a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | diagnostic_field(...) |
| Return Type Rule | runtime-defined by engine entrypoint diagnostic_field |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_diagnostic_field.v3 |
| AST Binding | ast.expr.scalar_diagnostic_field |
| Engine Entrypoint | diagnostic_field |
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
select diagnostic_field(arg_1) from app.sample_values;
```

### `dictionary_encoded`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.dictionary_encoded |
| UUID | 019dffbb-f001-741d-8a00-00000000041d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | dictionary_encoded(...) |
| Return Type Rule | runtime-defined by engine entrypoint dictionary_encoded |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_dictionary_encoded.v3 |
| AST Binding | ast.expr.scalar_dictionary_encoded |
| Engine Entrypoint | dictionary_encoded |
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
select dictionary_encoded(arg_1) from app.sample_values;
```

### `digest`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.digest |
| UUID | 019dffbb-f000-79df-817f-0d6e1cad7691 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | digest(...) |
| Return Type Rule | descriptor-authoritative digest digest result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary digest inputs |
| Null Behavior | strict unless SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_digest.v3 |
| AST Binding | ast.expr.scalar_digest |
| Engine Entrypoint | scalar_digest |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/format errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select digest(arg_1) from app.sample_values;
```

### `distinct`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.distinct |
| UUID | 019dffbb-f000-7e7c-b7b9-046bfd24ebb9 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | distinct(...) |
| Return Type Rule | runtime-defined by engine entrypoint distinct |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_distinct.v3 |
| AST Binding | ast.expr.scalar_distinct |
| Engine Entrypoint | distinct |
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
select distinct(arg_1) from app.sample_values;
```

### `div`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.div |
| UUID | 019dffbb-f000-710f-8fef-b023b20d1ee0 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | div(...) |
| Return Type Rule | descriptor-authoritative div numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_div.v3 |
| AST Binding | ast.expr.scalar_div |
| Engine Entrypoint | scalar_div |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select div(arg_1) from app.sample_values;
```

### `dmetaphone`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.dmetaphone |
| UUID | 019dffbb-f000-7df0-b957-2850e6eaf046 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | dmetaphone(...) |
| Return Type Rule | descriptor-authoritative dmetaphone fuzzy/phonetic scalar result as implemented by the SBLR expression runtime; fixture result family character |
| Coercion Rule | descriptor implicit cast matrix for text and bounded integer arguments |
| Null Behavior | strict unless SBSFC-014 fuzzy/phonetic row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for text normalization where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and locale/regex versions |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_dmetaphone.v3 |
| AST Binding | ast.expr.scalar_dmetaphone |
| Engine Entrypoint | sb_engine_functions.dmetaphone |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select dmetaphone(arg_1) from app.sample_values;
```

### `dmetaphone_alt`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.dmetaphone_alt |
| UUID | 019dffbb-f000-7725-a89d-5cf724b2ed97 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | dmetaphone_alt(...) |
| Return Type Rule | descriptor-authoritative dmetaphone_alt fuzzy/phonetic scalar result as implemented by the SBLR expression runtime; fixture result family character |
| Coercion Rule | descriptor implicit cast matrix for text and bounded integer arguments |
| Null Behavior | strict unless SBSFC-014 fuzzy/phonetic row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for text normalization where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and locale/regex versions |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_dmetaphone_alt.v3 |
| AST Binding | ast.expr.scalar_dmetaphone_alt |
| Engine Entrypoint | sb_engine_functions.dmetaphone_alt |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select dmetaphone_alt(arg_1) from app.sample_values;
```

### `domain_stack`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.domain_stack |
| UUID | 019dffbb-f000-7ce7-9a6e-3cd8e7e65fd1 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | domain_stack(...) |
| Return Type Rule | runtime-defined by engine entrypoint domain_stack |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_domain_stack.v3 |
| AST Binding | ast.expr.scalar_domain_stack |
| Engine Entrypoint | domain_stack |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select domain_stack(arg_1) from app.sample_values;
```

### `domain_stack_value`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.domain_stack_value |
| UUID | 019dffbb-f000-7eb3-92f5-f91b5ddb9769 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | domain_stack_value(...) |
| Return Type Rule | runtime-defined by engine entrypoint domain_stack_value |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_domain_stack_value.v3 |
| AST Binding | ast.expr.scalar_domain_stack_value |
| Engine Entrypoint | domain_stack_value |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select domain_stack_value(arg_1) from app.sample_values;
```

### `donor_contextual_keyword`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.donor_contextual_keyword |
| UUID | 019dffbb-f000-7219-b58a-93be3af6dcde |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | donor_contextual_keyword(...) |
| Return Type Rule | runtime-defined by engine entrypoint donor_contextual_keyword |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_donor_contextual_keyword.v3 |
| AST Binding | ast.expr.scalar_donor_contextual_keyword |
| Engine Entrypoint | donor_contextual_keyword |
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
select donor_contextual_keyword(arg_1) from app.sample_values;
```

### `donor_log_compatibility`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.donor_log_compatibility |
| UUID | 019dffbb-f001-7411-8a00-000000000411 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | donor_log_compatibility(...) |
| Return Type Rule | runtime-defined by engine entrypoint donor_log_compatibility |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_donor_log_compatibility.v3 |
| AST Binding | ast.expr.scalar_donor_log_compatibility |
| Engine Entrypoint | donor_log_compatibility |
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
select donor_log_compatibility(arg_1) from app.sample_values;
```

### `donor_only`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.donor_only |
| UUID | 019dffbb-f000-786b-9c4a-4dfb6f42b2dd |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | donor_only(...) |
| Return Type Rule | runtime-defined by engine entrypoint donor_only |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.native_surface.donor_only.v3 |
| AST Binding | ast.expr.donor_only |
| Engine Entrypoint | donor_only |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select donor_only(arg_1) from app.sample_values;
```

### `donor_only_rewrite`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.donor_only_rewrite |
| UUID | 019dffbb-f001-7447-8a00-000000000447 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | donor_only_rewrite(...) |
| Return Type Rule | runtime-defined by engine entrypoint donor_only_rewrite |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_donor_only_rewrite.v3 |
| AST Binding | ast.expr.scalar_donor_only_rewrite |
| Engine Entrypoint | donor_only_rewrite |
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
select donor_only_rewrite(arg_1) from app.sample_values;
```

### `donor_reserved_keyword`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.donor_reserved_keyword |
| UUID | 019dffbb-f000-7395-ae26-804f954c291b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | donor_reserved_keyword(...) |
| Return Type Rule | runtime-defined by engine entrypoint donor_reserved_keyword |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_donor_reserved_keyword.v3 |
| AST Binding | ast.expr.scalar_donor_reserved_keyword |
| Engine Entrypoint | donor_reserved_keyword |
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
select donor_reserved_keyword(arg_1) from app.sample_values;
```

### `donor_rewrite`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.donor_rewrite |
| UUID | 019dffbb-f000-7ac7-8951-deb86b0b8524 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | donor_rewrite(...) |
| Return Type Rule | runtime-defined by engine entrypoint donor_rewrite |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_donor_rewrite.v3 |
| AST Binding | ast.expr.scalar_donor_rewrite |
| Engine Entrypoint | donor_rewrite |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select donor_rewrite(arg_1) from app.sample_values;
```

### `drop`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.drop |
| UUID | 019dffbb-f000-7f8b-8211-b20e451bee76 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | drop(...) |
| Return Type Rule | runtime-defined by engine entrypoint drop |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_drop.v3 |
| AST Binding | ast.expr.scalar_drop |
| Engine Entrypoint | drop |
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
select drop(arg_1) from app.sample_values;
```

### `dynamic_sql_untrusted`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.dynamic_sql_untrusted |
| UUID | 019dffbb-f001-7450-8a00-000000000450 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | dynamic_sql_untrusted(...) |
| Return Type Rule | runtime-defined by engine entrypoint dynamic_sql_untrusted |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_dynamic_sql_untrusted.v3 |
| AST Binding | ast.expr.scalar_dynamic_sql_untrusted |
| Engine Entrypoint | dynamic_sql_untrusted |
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
select dynamic_sql_untrusted(arg_1) from app.sample_values;
```

### `else`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.else |
| UUID | 019dffbb-f000-7c14-829a-9a2cb8be6c33 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | else(...) |
| Return Type Rule | runtime-defined by engine entrypoint else |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_else.v3 |
| AST Binding | ast.expr.scalar_else |
| Engine Entrypoint | else |
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
select else(arg_1) from app.sample_values;
```

### `empty_string_equals_null`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.empty_string_equals_null |
| UUID | 019dffbb-f000-75e4-8340-18e6a14a340a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | empty_string_equals_null(...) |
| Return Type Rule | runtime-defined by engine entrypoint empty_string_equals_null |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_empty_string_equals_null.v3 |
| AST Binding | ast.expr.scalar_empty_string_equals_null |
| Engine Entrypoint | empty_string_equals_null |
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
select empty_string_equals_null(arg_1) from app.sample_values;
```

### `encode`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.encode |
| UUID | 019dffbb-f000-7070-8aa1-73cceede6ce4 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | encode(...) |
| Return Type Rule | descriptor-authoritative encode encoding/binary conversion result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary encoding inputs |
| Null Behavior | strict unless SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_encode.v3 |
| AST Binding | ast.expr.scalar_encode |
| Engine Entrypoint | scalar_encode |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/format errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select encode(arg_1) from app.sample_values;
```

### `end`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.end |
| UUID | 019dffbb-f000-7f96-98d7-cc415101b155 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | end(...) |
| Return Type Rule | runtime-defined by engine entrypoint end |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_end.v3 |
| AST Binding | ast.expr.scalar_end |
| Engine Entrypoint | end |
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
select end(arg_1) from app.sample_values;
```

### `engine`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.engine |
| UUID | 019dffbb-f001-7423-8a00-000000000423 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | engine(...) |
| Return Type Rule | runtime-defined by engine entrypoint engine |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_engine.v3 |
| AST Binding | ast.expr.scalar_engine |
| Engine Entrypoint | engine |
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
select engine(arg_1) from app.sample_values;
```

### `error_class`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.error_class |
| UUID | 019dffbb-f000-76db-a78e-a284e73015dc |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | error_class(...) |
| Return Type Rule | runtime-defined by engine entrypoint error_class |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_error_class.v3 |
| AST Binding | ast.expr.scalar_error_class |
| Engine Entrypoint | error_class |
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
select error_class(arg_1) from app.sample_values;
```

### `error_diagnostic_uuid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.error_diagnostic_uuid |
| UUID | 019dffbb-f001-745a-8a00-00000000045a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | error_diagnostic_uuid(...) |
| Return Type Rule | runtime-defined by engine entrypoint error_diagnostic_uuid |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_error_diagnostic_uuid.v3 |
| AST Binding | ast.expr.scalar_error_diagnostic_uuid |
| Engine Entrypoint | error_diagnostic_uuid |
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
select error_diagnostic_uuid(arg_1) from app.sample_values;
```

### `event_trigger_authority_unavailable`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.event_trigger_authority_unavailable |
| UUID | 019dffbb-f001-7443-8a00-000000000443 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | event_trigger_authority_unavailable(...) |
| Return Type Rule | runtime-defined by engine entrypoint event_trigger_authority_unavailable |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_event_trigger_authority_unavailable.v3 |
| AST Binding | ast.expr.scalar_event_trigger_authority_unavailable |
| Engine Entrypoint | event_trigger_authority_unavailable |
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
select event_trigger_authority_unavailable(arg_1) from app.sample_values;
```

### `events`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.events |
| UUID | 019dffbb-f000-705b-b1ff-1499e4e580ed |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | events(...) |
| Return Type Rule | runtime-defined by engine entrypoint events |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_events.v3 |
| AST Binding | ast.expr.scalar_events |
| Engine Entrypoint | events |
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
select events(arg_1) from app.sample_values;
```

### `evidence`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.evidence |
| UUID | 019dffbb-f001-7415-8a00-000000000415 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | evidence(...) |
| Return Type Rule | runtime-defined by engine entrypoint evidence |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_evidence.v3 |
| AST Binding | ast.expr.scalar_evidence |
| Engine Entrypoint | evidence |
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
select evidence(arg_1) from app.sample_values;
```

### `evidence_chain_uuid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.evidence_chain_uuid |
| UUID | 019dffbb-f001-7417-8a00-000000000417 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | evidence_chain_uuid(...) |
| Return Type Rule | runtime-defined by engine entrypoint evidence_chain_uuid |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_evidence_chain_uuid.v3 |
| AST Binding | ast.expr.scalar_evidence_chain_uuid |
| Engine Entrypoint | evidence_chain_uuid |
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
select evidence_chain_uuid(arg_1) from app.sample_values;
```

### `execution_type_descriptor`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.execution_type_descriptor |
| UUID | 019dffbb-f000-7af6-8910-83fad28f8cf5 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | execution_type_descriptor(...) |
| Return Type Rule | runtime-defined by engine entrypoint execution_type_descriptor |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_execution_type_descriptor.v3 |
| AST Binding | ast.expr.scalar_execution_type_descriptor |
| Engine Entrypoint | execution_type_descriptor |
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
select execution_type_descriptor(arg_1) from app.sample_values;
```

### `exists`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.exists |
| UUID | 019dffbb-f000-770b-85bc-b7256de8ca86 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | exists(...) |
| Return Type Rule | runtime-defined by engine entrypoint exists |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_exists.v3 |
| AST Binding | ast.expr.scalar_exists |
| Engine Entrypoint | exists |
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
select exists(arg_1) from app.sample_values;
```

### `exp`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.exp |
| UUID | 019dffbb-f001-7007-8a00-000000000007 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | exp(...) |
| Return Type Rule | descriptor-authoritative exp numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_exp.v3 |
| AST Binding | ast.expr.scalar_exp |
| Engine Entrypoint | scalar_exp |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select exp(arg_1) from app.sample_values;
```

### `expr`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.expr |
| UUID | 019dffbb-f001-743b-8a00-00000000043b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | expr(...) |
| Return Type Rule | runtime-defined by engine entrypoint expr |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_expr.v3 |
| AST Binding | ast.expr.scalar_expr |
| Engine Entrypoint | expr |
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
select expr(arg_1) from app.sample_values;
```

### `factorial`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.factorial |
| UUID | 019dffbb-f000-7aa2-9711-746738e3a402 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | factorial(...) |
| Return Type Rule | descriptor-authoritative factorial numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_factorial.v3 |
| AST Binding | ast.expr.scalar_factorial |
| Engine Entrypoint | scalar_factorial |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select factorial(arg_1) from app.sample_values;
```

### `fail_closed`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.fail_closed |
| UUID | 019dffbb-f001-7412-8a00-000000000412 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | fail_closed(...) |
| Return Type Rule | runtime-defined by engine entrypoint fail_closed |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_fail_closed.v3 |
| AST Binding | ast.expr.scalar_fail_closed |
| Engine Entrypoint | fail_closed |
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
select fail_closed(arg_1) from app.sample_values;
```

### `filesystem`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.filesystem |
| UUID | 019dffbb-f001-740d-8a00-00000000040d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | filesystem(...) |
| Return Type Rule | runtime-defined by engine entrypoint filesystem |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_filesystem.v3 |
| AST Binding | ast.expr.scalar_filesystem |
| Engine Entrypoint | filesystem |
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
select filesystem(arg_1) from app.sample_values;
```

### `floor`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.floor |
| UUID | 019de5fc-2400-7298-8611-8646279ac746 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | floor(number) |
| Return Type Rule | floor by numeric descriptor |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_floor.v3 |
| AST Binding | ast.expr.scalar_floor |
| Engine Entrypoint | scalar_floor |
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
select floor(numeric_value_1) from app.sample_values;
```

### `format`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.format |
| UUID | 019dffbb-f000-78cf-8273-dc9fb4763598 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | format(...) |
| Return Type Rule | descriptor-authoritative format result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for native function/operator operands |
| Null Behavior | function/operator-specific NULL handling follows SBSFC-026 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | stable_by_arguments |
| Determinism | foldable only when inputs are constants with stable descriptors and function-specific context is fixed |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_format.v3 |
| AST Binding | ast.expr.scalar_format |
| Engine Entrypoint | format |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-026 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select format(arg_1) from app.sample_values;
```

### `from_bytes`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.from_bytes |
| UUID | 019dffbb-f000-793c-a511-0643a66a24d6 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | from_bytes(...) |
| Return Type Rule | runtime-defined by engine entrypoint from_bytes |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_from_bytes.v3 |
| AST Binding | ast.expr.scalar_from_bytes |
| Engine Entrypoint | from_bytes |
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
select from_bytes(arg_1) from app.sample_values;
```

### `full`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.full |
| UUID | 019dffbb-f000-7cd8-9cea-0bd9021311f9 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | full(...) |
| Return Type Rule | runtime-defined by engine entrypoint full |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_full.v3 |
| AST Binding | ast.expr.scalar_full |
| Engine Entrypoint | full |
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
select full(arg_1) from app.sample_values;
```

### `future_version`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.future_version |
| UUID | 019dffbb-f000-751d-a38a-30359f7c29a6 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | future_version(...) |
| Return Type Rule | runtime-defined by engine entrypoint future_version |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_future_version.v3 |
| AST Binding | ast.expr.scalar_future_version |
| Engine Entrypoint | future_version |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select future_version(arg_1) from app.sample_values;
```

### `gap`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.gap |
| UUID | 019dffbb-f000-7ccf-b4c3-72113e50b9f3 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | gap(...) |
| Return Type Rule | runtime-defined by engine entrypoint gap |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_gap.v3 |
| AST Binding | ast.expr.scalar_gap |
| Engine Entrypoint | gap |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select gap(arg_1) from app.sample_values;
```

### `gcd`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.gcd |
| UUID | 019dffbb-f000-7124-8639-6bd5b3f42ea7 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | gcd(...) |
| Return Type Rule | descriptor-authoritative gcd numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_gcd.v3 |
| AST Binding | ast.expr.scalar_gcd |
| Engine Entrypoint | scalar_gcd |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select gcd(arg_1) from app.sample_values;
```

### `gdscode`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.gdscode |
| UUID | 019dffbb-f000-76d2-ad55-30d87813db45 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | gdscode(...) |
| Return Type Rule | runtime-defined by engine entrypoint gdscode |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_gdscode.v3 |
| AST Binding | ast.expr.scalar_gdscode |
| Engine Entrypoint | gdscode |
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
select gdscode(arg_1) from app.sample_values;
```

### `gen_id`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.gen_id |
| UUID | 019dffbb-f000-75e2-9ad5-770c12a23195 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | gen_id(...) |
| Return Type Rule | runtime-defined by engine entrypoint gen_id |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_gen_id.v3 |
| AST Binding | ast.expr.scalar_gen_id |
| Engine Entrypoint | gen_id |
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
select gen_id(arg_1) from app.sample_values;
```

### `greatest`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.greatest |
| UUID | 019dffbb-f001-7028-8a00-000000000028 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | greatest(...) |
| Return Type Rule | descriptor-authoritative greatest conditional/text result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix; common result descriptor selected by binder/lowering route |
| Null Behavior | conditional function-specific NULL handling follows SBSFC-011 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | stable_by_arguments |
| Determinism | foldable only when selected branch and arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_greatest.v3 |
| AST Binding | ast.expr.scalar_greatest |
| Engine Entrypoint | scalar_greatest |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/compatibility errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select greatest(arg_1) from app.sample_values;
```

### `has_column_privilege`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.has_column_privilege |
| UUID | 019dffbb-f000-7437-a6e4-cbb9d1d75a44 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | has_column_privilege(table_name,column_name,privilege); has_column_privilege(user,table_name,column_name,privilege) |
| Return Type Rule | boolean privilege predicate result |
| Coercion Rule | user, table_name, column_name, and privilege use scalar text representation |
| Null Behavior | any SQL NULL argument returns SQL NULL boolean |
| Collation/Charset Rule | privilege and catalog token comparison uses ASCII case-folding for canonical tokens |
| Timezone Rule | not applicable |
| Volatility | stable_per_transaction |
| Determinism | stable within current SBLR security and MGA relation metadata context |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_has_column_privilege.v3 |
| AST Binding | ast.expr.scalar_has_column_privilege |
| Engine Entrypoint | has_column_privilege |
| Security Policy | reads current SBLR principal/security context and engine-owned local MGA relation/column metadata; no parser SQL, donor backend, cluster path, WAL, or SQLite shortcut |
| Error Semantics | invalid arity refuses with SB_DIAG_FUNCTION_INVALID_INPUT; unknown user, relation, column, or unsupported privilege token returns false |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | catalog_security_context |

#### Practical Form

```sql
select has_column_privilege(arg_1, arg_2, arg_3) from app.sample_values;
```

### `has_function_privilege`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.has_function_privilege |
| UUID | 019dffbb-f000-7e3a-a0b9-7c75989b7e24 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | has_function_privilege(function_name,privilege); has_function_privilege(user,function_name,privilege) |
| Return Type Rule | boolean privilege predicate result |
| Coercion Rule | user, function_name, and privilege use scalar text representation |
| Null Behavior | any SQL NULL argument returns SQL NULL boolean |
| Collation/Charset Rule | privilege and builtin function token comparison uses ASCII case-folding for canonical tokens |
| Timezone Rule | not applicable |
| Volatility | stable_per_transaction |
| Determinism | stable within current SBLR security context and canonical builtin registry seed state |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_has_function_privilege.v3 |
| AST Binding | ast.expr.scalar_has_function_privilege |
| Engine Entrypoint | has_function_privilege |
| Security Policy | reads current SBLR principal/security context and canonical engine builtin metadata; no parser SQL, donor backend, cluster path, WAL, or SQLite shortcut |
| Error Semantics | invalid arity refuses with SB_DIAG_FUNCTION_INVALID_INPUT; unknown user, function, or unsupported privilege token returns false |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | catalog_security_context |

#### Practical Form

```sql
select has_function_privilege(arg_1, arg_2) from app.sample_values;
```

### `has_schema_privilege`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.has_schema_privilege |
| UUID | 019dffbb-f000-721e-9b45-dd414479d540 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | has_schema_privilege(schema_name,privilege); has_schema_privilege(user,schema_name,privilege) |
| Return Type Rule | boolean privilege predicate result |
| Coercion Rule | user, schema_name, and privilege use scalar text representation |
| Null Behavior | any SQL NULL argument returns SQL NULL boolean |
| Collation/Charset Rule | privilege and catalog token comparison uses ASCII case-folding for canonical tokens |
| Timezone Rule | not applicable |
| Volatility | stable_per_transaction |
| Determinism | stable within current SBLR security and schema context |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_has_schema_privilege.v3 |
| AST Binding | ast.expr.scalar_has_schema_privilege |
| Engine Entrypoint | has_schema_privilege |
| Security Policy | reads current SBLR principal/security context and current schema catalog context; no parser SQL, donor backend, cluster path, WAL, or SQLite shortcut |
| Error Semantics | invalid arity refuses with SB_DIAG_FUNCTION_INVALID_INPUT; unknown user, schema, or unsupported privilege token returns false |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | catalog_security_context |

#### Practical Form

```sql
select has_schema_privilege(arg_1, arg_2) from app.sample_values;
```

### `has_table_privilege`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.has_table_privilege |
| UUID | 019dffbb-f000-7da2-9e38-d4654d06dc78 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | has_table_privilege(table_name,privilege); has_table_privilege(user,table_name,privilege) |
| Return Type Rule | boolean privilege predicate result |
| Coercion Rule | user, table_name, and privilege use scalar text representation |
| Null Behavior | any SQL NULL argument returns SQL NULL boolean |
| Collation/Charset Rule | privilege and catalog token comparison uses ASCII case-folding for canonical tokens |
| Timezone Rule | not applicable |
| Volatility | stable_per_transaction |
| Determinism | stable within current SBLR security and MGA relation metadata context |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_has_table_privilege.v3 |
| AST Binding | ast.expr.scalar_has_table_privilege |
| Engine Entrypoint | has_table_privilege |
| Security Policy | reads current SBLR principal/security context and engine-owned local MGA relation metadata; no parser SQL, donor backend, cluster path, WAL, or SQLite shortcut |
| Error Semantics | invalid arity refuses with SB_DIAG_FUNCTION_INVALID_INPUT; unknown user, relation, or unsupported privilege token returns false |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | catalog_security_context |

#### Practical Form

```sql
select has_table_privilege(arg_1, arg_2) from app.sample_values;
```

### `hierarchyid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.hierarchyid |
| UUID | 019dffbb-f001-7432-8a00-000000000432 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | hierarchyid(...) |
| Return Type Rule | runtime-defined by engine entrypoint hierarchyid |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_hierarchyid.v3 |
| AST Binding | ast.expr.scalar_hierarchyid |
| Engine Entrypoint | hierarchyid |
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
select hierarchyid(arg_1) from app.sample_values;
```

### `hnsw`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.hnsw |
| UUID | 019dffbb-f001-7429-8a00-000000000429 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | hnsw(...) |
| Return Type Rule | runtime-defined by engine entrypoint hnsw |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_hnsw.v3 |
| AST Binding | ast.expr.scalar_hnsw |
| Engine Entrypoint | hnsw |
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
select hnsw(arg_1) from app.sample_values;
```

### `identifier_bare`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.identifier_bare |
| UUID | 019dffbb-f001-7404-8a00-000000000404 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | identifier_bare(...) |
| Return Type Rule | runtime-defined by engine entrypoint identifier_bare |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_identifier_bare.v3 |
| AST Binding | ast.expr.scalar_identifier_bare |
| Engine Entrypoint | identifier_bare |
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
select identifier_bare(arg_1) from app.sample_values;
```

### `identifier_max_length_bytes`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.identifier_max_length_bytes |
| UUID | 019dffbb-f000-71f2-96aa-c3c10539cc94 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | identifier_max_length_bytes(...) |
| Return Type Rule | runtime-defined by engine entrypoint identifier_max_length_bytes |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_identifier_max_length_bytes.v3 |
| AST Binding | ast.expr.scalar_identifier_max_length_bytes |
| Engine Entrypoint | identifier_max_length_bytes |
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
select identifier_max_length_bytes(arg_1) from app.sample_values;
```

### `identifier_max_length_chars`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.identifier_max_length_chars |
| UUID | 019dffbb-f000-73b3-b87a-3a23d5c1e488 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | identifier_max_length_chars(...) |
| Return Type Rule | runtime-defined by engine entrypoint identifier_max_length_chars |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_identifier_max_length_chars.v3 |
| AST Binding | ast.expr.scalar_identifier_max_length_chars |
| Engine Entrypoint | identifier_max_length_chars |
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
select identifier_max_length_chars(arg_1) from app.sample_values;
```

### `idle_in_transaction_session_timeout`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.idle_in_transaction_session_timeout |
| UUID | 019dffbb-f000-7586-adc7-5bbcd1852e36 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | idle_in_transaction_session_timeout() |
| Return Type Rule | fixed public idle-in-transaction timeout policy value; descriptor=uint64 |
| Coercion Rule | no input coercion |
| Null Behavior | never null |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | stable_per_session_policy |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.idle_in_transaction_session_timeout.v3 |
| AST Binding | ast.expr.idle_in_transaction_session_timeout |
| Engine Entrypoint | idle_in_transaction_session_timeout |
| Security Policy | exposes fixed standalone timeout policy, no mutable setting catalog read |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select idle_in_transaction_session_timeout() from app.sample_values;
```

### `idle_in_transaction_session_timeout_ms`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.idle_in_transaction_session_timeout_ms |
| UUID | 019dffbb-f000-7211-92c2-0be3cb30ce3f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | idle_in_transaction_session_timeout_ms(...) |
| Return Type Rule | runtime-defined by engine entrypoint idle_in_transaction_session_timeout_ms |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_idle_in_transaction_session_timeout_ms.v3 |
| AST Binding | ast.expr.scalar_idle_in_transaction_session_timeout_ms |
| Engine Entrypoint | idle_in_transaction_session_timeout_ms |
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
select idle_in_transaction_session_timeout_ms(arg_1) from app.sample_values;
```

### `idle_in_transaction_timeout_default`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.idle_in_transaction_timeout_default |
| UUID | 019dffbb-f000-7892-a249-c10bba36bbfb |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | idle_in_transaction_timeout_default() |
| Return Type Rule | fixed default idle-in-transaction timeout policy value; descriptor=uint64 |
| Coercion Rule | no input coercion |
| Null Behavior | never null |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | stable_per_session_policy |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.idle_in_transaction_timeout_default.v3 |
| AST Binding | ast.expr.idle_in_transaction_timeout_default |
| Engine Entrypoint | idle_in_transaction_timeout_default |
| Security Policy | exposes fixed standalone timeout policy, no mutable setting catalog read |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select idle_in_transaction_timeout_default() from app.sample_values;
```

### `ifnull`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.ifnull |
| UUID | 019de5fc-2400-7e49-b026-73abe683cb43 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | ifnull(a,b) |
| Return Type Rule | two-argument coalesce alias |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | stable_by_arguments |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_ifnull.v3 |
| AST Binding | ast.expr.scalar_ifnull |
| Engine Entrypoint | scalar_ifnull |
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
select ifnull(arg_1, arg_2) from app.sample_values;
```

### `iif`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.iif |
| UUID | 019dffbb-f001-702b-8a00-00000000002b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | iif(...) |
| Return Type Rule | descriptor-authoritative iif result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for native function/operator operands |
| Null Behavior | function/operator-specific NULL handling follows SBSFC-026 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | stable_by_arguments |
| Determinism | foldable only when inputs are constants with stable descriptors and function-specific context is fixed |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_iif.v3 |
| AST Binding | ast.expr.scalar_iif |
| Engine Entrypoint | scalar_iif |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-026 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select iif(arg_1) from app.sample_values;
```

### `immutable`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.immutable |
| UUID | 019dffbb-f000-763f-96a3-ce8d069c10be |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | immutable(...) |
| Return Type Rule | runtime-defined by engine entrypoint immutable |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_immutable.v3 |
| AST Binding | ast.expr.scalar_immutable |
| Engine Entrypoint | immutable |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select immutable(arg_1) from app.sample_values;
```

### `index`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.index |
| UUID | 019dffbb-f000-7608-8927-9358bb5ac392 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | index(...) |
| Return Type Rule | runtime-defined by engine entrypoint index |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_index.v3 |
| AST Binding | ast.expr.scalar_index |
| Engine Entrypoint | index |
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
select index(arg_1) from app.sample_values;
```

### `index_descriptor`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.index_descriptor |
| UUID | 019dffbb-f000-7f3b-a615-c36de09dfc99 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | index_descriptor(...) |
| Return Type Rule | runtime-defined by engine entrypoint index_descriptor |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_index_descriptor.v3 |
| AST Binding | ast.expr.scalar_index_descriptor |
| Engine Entrypoint | index_descriptor |
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
select index_descriptor(arg_1) from app.sample_values;
```

### `initcap`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.initcap |
| UUID | 019dffbb-f000-79a1-a28b-638ae91177fb |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | initcap(...) |
| Return Type Rule | descriptor-authoritative initcap text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_initcap.v3 |
| AST Binding | ast.expr.scalar_initcap |
| Engine Entrypoint | scalar_initcap |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select initcap(arg_1) from app.sample_values;
```

### `inner`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.inner |
| UUID | 019dffbb-f000-7509-a7ff-af21a9de0ce2 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | inner(...) |
| Return Type Rule | runtime-defined by engine entrypoint inner |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_inner.v3 |
| AST Binding | ast.expr.scalar_inner |
| Engine Entrypoint | inner |
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
select inner(arg_1) from app.sample_values;
```

### `innodb`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.innodb |
| UUID | 019dffbb-f001-7428-8a00-000000000428 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | innodb(...) |
| Return Type Rule | runtime-defined by engine entrypoint innodb |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_innodb.v3 |
| AST Binding | ast.expr.scalar_innodb |
| Engine Entrypoint | innodb |
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
select innodb(arg_1) from app.sample_values;
```

### `instr`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.instr |
| UUID | 019dffbb-f001-7042-8a00-000000000042 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | instr(...) |
| Return Type Rule | descriptor-authoritative instr text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_instr.v3 |
| AST Binding | ast.expr.scalar_instr |
| Engine Entrypoint | scalar_instr |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select instr(arg_1) from app.sample_values;
```

### `interval_default_precision`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.interval_default_precision |
| UUID | 019dffbb-f000-7a7f-86b3-b8dbae5a6f74 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | interval_default_precision(...) |
| Return Type Rule | runtime-defined by engine entrypoint interval_default_precision |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_interval_default_precision.v3 |
| AST Binding | ast.expr.scalar_interval_default_precision |
| Engine Entrypoint | interval_default_precision |
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
select interval_default_precision(arg_1) from app.sample_values;
```

### `is`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.is |
| UUID | 019dffbb-f000-7d09-89ab-8470879074d3 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | is(...) |
| Return Type Rule | runtime-defined by engine entrypoint is |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_is.v3 |
| AST Binding | ast.expr.scalar_is |
| Engine Entrypoint | is |
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
select is(arg_1) from app.sample_values;
```

### `is_alpha`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.is_alpha |
| UUID | 019dffbb-f000-7bba-b2f3-8c0035ab8176 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | is_alpha(...) |
| Return Type Rule | runtime-defined by engine entrypoint is_alpha |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_is_alpha.v3 |
| AST Binding | ast.expr.scalar_is_alpha |
| Engine Entrypoint | is_alpha |
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
select is_alpha(arg_1) from app.sample_values;
```

### `is_alphanumeric`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.is_alphanumeric |
| UUID | 019dffbb-f000-7c46-9ca4-0afff8cf596d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | is_alphanumeric(...) |
| Return Type Rule | runtime-defined by engine entrypoint is_alphanumeric |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_is_alphanumeric.v3 |
| AST Binding | ast.expr.scalar_is_alphanumeric |
| Engine Entrypoint | is_alphanumeric |
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
select is_alphanumeric(arg_1) from app.sample_values;
```

### `is_ascii`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.is_ascii |
| UUID | 019dffbb-f000-79d9-858e-950ca3dbc2a8 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | is_ascii(...) |
| Return Type Rule | runtime-defined by engine entrypoint is_ascii |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_is_ascii.v3 |
| AST Binding | ast.expr.scalar_is_ascii |
| Engine Entrypoint | is_ascii |
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
select is_ascii(arg_1) from app.sample_values;
```

### `is_digit`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.is_digit |
| UUID | 019dffbb-f000-7bcd-b729-a0427ce11a77 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | is_digit(...) |
| Return Type Rule | runtime-defined by engine entrypoint is_digit |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_is_digit.v3 |
| AST Binding | ast.expr.scalar_is_digit |
| Engine Entrypoint | is_digit |
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
select is_digit(arg_1) from app.sample_values;
```

### `is_space`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.is_space |
| UUID | 019dffbb-f000-7116-b2a5-508f945e44e4 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | is_space(...) |
| Return Type Rule | runtime-defined by engine entrypoint is_space |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_is_space.v3 |
| AST Binding | ast.expr.scalar_is_space |
| Engine Entrypoint | is_space |
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
select is_space(arg_1) from app.sample_values;
```

### `ivf_flat`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.ivf_flat |
| UUID | 019dffbb-f001-742e-8a00-00000000042e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | ivf_flat(...) |
| Return Type Rule | runtime-defined by engine entrypoint ivf_flat |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_ivf_flat.v3 |
| AST Binding | ast.expr.scalar_ivf_flat |
| Engine Entrypoint | ivf_flat |
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
select ivf_flat(arg_1) from app.sample_values;
```

### `jaro_similarity`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.jaro_similarity |
| UUID | 019dffbb-f000-7791-a750-9532dc9a8c04 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jaro_similarity(...) |
| Return Type Rule | descriptor-authoritative jaro_similarity fuzzy/phonetic scalar result as implemented by the SBLR expression runtime; fixture result family real64 |
| Coercion Rule | descriptor implicit cast matrix for text and bounded integer arguments |
| Null Behavior | strict unless SBSFC-014 fuzzy/phonetic row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for text normalization where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and locale/regex versions |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_jaro_similarity.v3 |
| AST Binding | ast.expr.scalar_jaro_similarity |
| Engine Entrypoint | sb_engine_functions.jaro_similarity |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jaro_similarity(arg_1) from app.sample_values;
```

### `jaro_winkler_similarity`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.jaro_winkler_similarity |
| UUID | 019dffbb-f000-7d26-8eee-6a801b0e75a1 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jaro_winkler_similarity(...) |
| Return Type Rule | descriptor-authoritative jaro_winkler_similarity fuzzy/phonetic scalar result as implemented by the SBLR expression runtime; fixture result family real64 |
| Coercion Rule | descriptor implicit cast matrix for text and bounded integer arguments |
| Null Behavior | strict unless SBSFC-014 fuzzy/phonetic row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for text normalization where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and locale/regex versions |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_jaro_winkler_similarity.v3 |
| AST Binding | ast.expr.scalar_jaro_winkler_similarity |
| Engine Entrypoint | sb_engine_functions.jaro_winkler_similarity |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jaro_winkler_similarity(arg_1) from app.sample_values;
```

### `keyword_case_rule`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.keyword_case_rule |
| UUID | 019dffbb-f000-7f1b-9435-bd4734aef998 |
| Kind | scalar |
| Syntax Forms | function_call, keyword_or_function_call |
| Overloads | keyword_case_rule(...) |
| Return Type Rule | descriptor-authoritative keyword_case_rule language/profile metadata result as implemented by the SBLR expression runtime |
| Coercion Rule | no input coercion unless the fixture surface supplies descriptor-authoritative arguments |
| Null Behavior | not applicable for nullary metadata forms; strict for argument-bearing forms |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | stable_per_catalog_profile |
| Determinism | profile-backed; foldable after binding when the active catalog/profile is fixed |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_keyword_case_rule.v3 |
| AST Binding | ast.expr.scalar_keyword_case_rule |
| Engine Entrypoint | scalar_keyword_case_rule |
| Security Policy | reads fixed language/catalog profile metadata only; no user data access |
| Error Semantics | arity/type errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select keyword_case_rule(arg_1) from app.sample_values;
```

### `last_error_position`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.last_error_position |
| UUID | 019dffbb-f000-7037-9191-aa04255cdafd |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | last_error_position(...) |
| Return Type Rule | runtime-defined by engine entrypoint last_error_position |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_last_error_position.v3 |
| AST Binding | ast.expr.scalar_last_error_position |
| Engine Entrypoint | last_error_position |
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
select last_error_position(arg_1) from app.sample_values;
```

### `lastval`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.lastval |
| UUID | 019dffbb-f000-70f5-b8c0-f4065ce74d35 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | lastval(...) |
| Return Type Rule | runtime-defined by engine entrypoint lastval |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_lastval.v3 |
| AST Binding | ast.expr.scalar_lastval |
| Engine Entrypoint | lastval |
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
select lastval(arg_1) from app.sample_values;
```

### `lateral`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.lateral |
| UUID | 019dffbb-f000-7139-b0c6-0522b6407dc2 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | lateral(...) |
| Return Type Rule | runtime-defined by engine entrypoint lateral |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_lateral.v3 |
| AST Binding | ast.expr.scalar_lateral |
| Engine Entrypoint | lateral |
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
select lateral(arg_1) from app.sample_values;
```

### `lcm`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.lcm |
| UUID | 019dffbb-f000-75a4-93f9-684aa8cee0f8 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | lcm(...) |
| Return Type Rule | descriptor-authoritative lcm numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_lcm.v3 |
| AST Binding | ast.expr.scalar_lcm |
| Engine Entrypoint | scalar_lcm |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select lcm(arg_1) from app.sample_values;
```

### `least`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.least |
| UUID | 019dffbb-f001-7029-8a00-000000000029 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | least(...) |
| Return Type Rule | descriptor-authoritative least conditional/text result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix; common result descriptor selected by binder/lowering route |
| Null Behavior | conditional function-specific NULL handling follows SBSFC-011 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | stable_by_arguments |
| Determinism | foldable only when selected branch and arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_least.v3 |
| AST Binding | ast.expr.scalar_least |
| Engine Entrypoint | scalar_least |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/compatibility errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select least(arg_1) from app.sample_values;
```

### `left`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.left |
| UUID | 019dffbb-f000-795b-9d58-23726bd739b5 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | left(...) |
| Return Type Rule | descriptor-authoritative left text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_left.v3 |
| AST Binding | ast.expr.scalar_left |
| Engine Entrypoint | scalar_left |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select left(arg_1) from app.sample_values;
```

### `length`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.length |
| UUID | 019de5fc-2400-7274-8dcd-afa47dab8613 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | length(value) |
| Return Type Rule | character length for strings; collection cardinality only through typed overloads |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_length.v3 |
| AST Binding | ast.expr.scalar_length |
| Engine Entrypoint | scalar_length |
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
select length(value_1) from app.sample_values;
```

### `levenshtein`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.levenshtein |
| UUID | 019dffbb-f000-71c0-b1fe-1745029892e7 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | levenshtein(...) |
| Return Type Rule | descriptor-authoritative levenshtein fuzzy/phonetic scalar result as implemented by the SBLR expression runtime; fixture result family int64 |
| Coercion Rule | descriptor implicit cast matrix for text and bounded integer arguments |
| Null Behavior | strict unless SBSFC-014 fuzzy/phonetic row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for text normalization where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and locale/regex versions |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_levenshtein.v3 |
| AST Binding | ast.expr.scalar_levenshtein |
| Engine Entrypoint | sb_engine_functions.levenshtein |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select levenshtein(arg_1) from app.sample_values;
```

### `levenshtein_le`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.levenshtein_le |
| UUID | 019dffbb-f000-7036-a7bc-f43a7c5488c1 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | levenshtein_le(...) |
| Return Type Rule | descriptor-authoritative levenshtein_le fuzzy/phonetic scalar result as implemented by the SBLR expression runtime; fixture result family int64 |
| Coercion Rule | descriptor implicit cast matrix for text and bounded integer arguments |
| Null Behavior | strict unless SBSFC-014 fuzzy/phonetic row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for text normalization where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and locale/regex versions |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_levenshtein_le.v3 |
| AST Binding | ast.expr.scalar_levenshtein_le |
| Engine Entrypoint | sb_engine_functions.levenshtein_le |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select levenshtein_le(arg_1) from app.sample_values;
```

### `ln`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.ln |
| UUID | 019dffbb-f001-7008-8a00-000000000008 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | ln(...) |
| Return Type Rule | descriptor-authoritative ln numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_ln.v3 |
| AST Binding | ast.expr.scalar_ln |
| Engine Entrypoint | scalar_ln |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select ln(arg_1) from app.sample_values;
```

### `locality`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.locality |
| UUID | 019dffbb-f001-7433-8a00-000000000433 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | locality(...) |
| Return Type Rule | runtime-defined by engine entrypoint locality |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_locality.v3 |
| AST Binding | ast.expr.scalar_locality |
| Engine Entrypoint | locality |
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
select locality(arg_1) from app.sample_values;
```

### `localized_label`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.localized_label |
| UUID | 019dffbb-f001-741a-8a00-00000000041a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | localized_label(...) |
| Return Type Rule | runtime-defined by engine entrypoint localized_label |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_localized_label.v3 |
| AST Binding | ast.expr.scalar_localized_label |
| Engine Entrypoint | localized_label |
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
select localized_label(arg_1) from app.sample_values;
```

### `localized_label_max_length_bytes`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.localized_label_max_length_bytes |
| UUID | 019dffbb-f000-7a29-b6f2-a2f943cbb0c8 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | localized_label_max_length_bytes(...) |
| Return Type Rule | runtime-defined by engine entrypoint localized_label_max_length_bytes |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_localized_label_max_length_bytes.v3 |
| AST Binding | ast.expr.scalar_localized_label_max_length_bytes |
| Engine Entrypoint | localized_label_max_length_bytes |
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
select localized_label_max_length_bytes(arg_1) from app.sample_values;
```

### `lock_timeout`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.lock_timeout |
| UUID | 019dffbb-f000-73b6-9b00-ab9904f2c4bf |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | lock_timeout() |
| Return Type Rule | fixed public lock-timeout policy value; descriptor=uint64 |
| Coercion Rule | no input coercion |
| Null Behavior | never null |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | stable_per_session_policy |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.lock_timeout.v3 |
| AST Binding | ast.expr.lock_timeout |
| Engine Entrypoint | lock_timeout |
| Security Policy | exposes fixed standalone timeout policy, no mutable setting catalog read |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select lock_timeout() from app.sample_values;
```

### `lock_timeout_default`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.lock_timeout_default |
| UUID | 019dffbb-f000-7884-8cb8-cad2c5b26037 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | lock_timeout_default() |
| Return Type Rule | fixed default lock-timeout policy value; descriptor=uint64 |
| Coercion Rule | no input coercion |
| Null Behavior | never null |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | stable_per_session_policy |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.lock_timeout_default.v3 |
| AST Binding | ast.expr.lock_timeout_default |
| Engine Entrypoint | lock_timeout_default |
| Security Policy | exposes fixed standalone timeout policy, no mutable setting catalog read |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select lock_timeout_default() from app.sample_values;
```

### `lock_timeout_ms`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.lock_timeout_ms |
| UUID | 019dffbb-f000-7ec9-8f02-0b61e0f4e701 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | lock_timeout_ms(...) |
| Return Type Rule | runtime-defined by engine entrypoint lock_timeout_ms |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_lock_timeout_ms.v3 |
| AST Binding | ast.expr.scalar_lock_timeout_ms |
| Engine Entrypoint | lock_timeout_ms |
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
select lock_timeout_ms(arg_1) from app.sample_values;
```

### `log`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.log |
| UUID | 019dffbb-f001-7045-8a00-000000000045 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | log(...) |
| Return Type Rule | descriptor-authoritative log numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_log.v3 |
| AST Binding | ast.expr.scalar_log |
| Engine Entrypoint | scalar_log |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select log(arg_1) from app.sample_values;
```

### `log10`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.log10 |
| UUID | 019dffbb-f001-7009-8a00-000000000009 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | log10(...) |
| Return Type Rule | descriptor-authoritative log10 numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_log10.v3 |
| AST Binding | ast.expr.scalar_log10 |
| Engine Entrypoint | scalar_log10 |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select log10(arg_1) from app.sample_values;
```

### `log2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.log2 |
| UUID | 019dffbb-f001-7031-8a00-000000000031 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | log2(...) |
| Return Type Rule | descriptor-authoritative log2 numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_log2.v3 |
| AST Binding | ast.expr.scalar_log2 |
| Engine Entrypoint | scalar_log2 |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select log2(arg_1) from app.sample_values;
```

### `lower`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.lower |
| UUID | 019de5fc-2400-7373-87ac-31583c8c2fe1 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | lower(string) |
| Return Type Rule | locale/collation-aware lowercase transform |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_lower.v3 |
| AST Binding | ast.expr.scalar_lower |
| Engine Entrypoint | scalar_lower |
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
select lower(text_value_1) from app.sample_values;
```

### `lpad`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.lpad |
| UUID | 019dffbb-f001-7023-8a00-000000000023 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | lpad(...) |
| Return Type Rule | descriptor-authoritative lpad text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_lpad.v3 |
| AST Binding | ast.expr.scalar_lpad |
| Engine Entrypoint | scalar_lpad |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select lpad(arg_1) from app.sample_values;
```

### `ltrim`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.ltrim |
| UUID | 019dffbb-f001-7012-8a00-000000000012 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | ltrim(...) |
| Return Type Rule | descriptor-authoritative ltrim text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_ltrim.v3 |
| AST Binding | ast.expr.scalar_ltrim |
| Engine Entrypoint | scalar_ltrim |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select ltrim(arg_1) from app.sample_values;
```

### `match_recognize`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.match_recognize |
| UUID | 019dffbb-f000-79c5-a788-e951e5331269 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | match_recognize(...) |
| Return Type Rule | runtime-defined by engine entrypoint match_recognize |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_match_recognize.v3 |
| AST Binding | ast.expr.scalar_match_recognize |
| Engine Entrypoint | match_recognize |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select match_recognize(arg_1) from app.sample_values;
```

### `md5`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.md5 |
| UUID | 019dffbb-f001-701d-8a00-00000000001d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | md5(...) |
| Return Type Rule | descriptor-authoritative md5 digest result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary digest inputs |
| Null Behavior | strict unless SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_md5.v3 |
| AST Binding | ast.expr.scalar_md5 |
| Engine Entrypoint | scalar_md5 |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/format errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select md5(arg_1) from app.sample_values;
```

### `merge_action`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.merge_action |
| UUID | 019dffbb-f001-7402-8a00-000000000402 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | merge_action(...) |
| Return Type Rule | runtime-defined by engine entrypoint merge_action |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_merge_action.v3 |
| AST Binding | ast.expr.scalar_merge_action |
| Engine Entrypoint | merge_action |
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
select merge_action(arg_1) from app.sample_values;
```

### `mergetree`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.mergetree |
| UUID | 019dffbb-f001-7426-8a00-000000000426 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | mergetree(...) |
| Return Type Rule | runtime-defined by engine entrypoint mergetree |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_mergetree.v3 |
| AST Binding | ast.expr.scalar_mergetree |
| Engine Entrypoint | mergetree |
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
select mergetree(arg_1) from app.sample_values;
```

### `meta_command_keyword`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.meta_command_keyword |
| UUID | 019dffbb-f000-70d2-a023-bf4fe5f4dd98 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | meta_command_keyword(...) |
| Return Type Rule | runtime-defined by engine entrypoint meta_command_keyword |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_meta_command_keyword.v3 |
| AST Binding | ast.expr.scalar_meta_command_keyword |
| Engine Entrypoint | meta_command_keyword |
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
select meta_command_keyword(arg_1) from app.sample_values;
```

### `metaphone`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.metaphone |
| UUID | 019dffbb-f000-768e-9bb3-d26932fe744f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | metaphone(...) |
| Return Type Rule | descriptor-authoritative metaphone fuzzy/phonetic scalar result as implemented by the SBLR expression runtime; fixture result family character |
| Coercion Rule | descriptor implicit cast matrix for text and bounded integer arguments |
| Null Behavior | strict unless SBSFC-014 fuzzy/phonetic row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for text normalization where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and locale/regex versions |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_metaphone.v3 |
| AST Binding | ast.expr.scalar_metaphone |
| Engine Entrypoint | sb_engine_functions.metaphone |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select metaphone(arg_1) from app.sample_values;
```

### `metrics`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.metrics |
| UUID | 019dffbb-f001-740f-8a00-00000000040f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | metrics(...) |
| Return Type Rule | runtime-defined by engine entrypoint metrics |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_metrics.v3 |
| AST Binding | ast.expr.scalar_metrics |
| Engine Entrypoint | metrics |
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
select metrics(arg_1) from app.sample_values;
```

### `mga_isolation_profile`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.mga_isolation_profile |
| UUID | 019dffbb-f000-7088-a176-9d566089d2d4 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | mga_isolation_profile |
| Return Type Rule | native MGA isolation profile mapped from transaction isolation context; descriptor=character |
| Coercion Rule | no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted |
| Null Behavior | context-backed typed null or diagnostic behavior follows SBSFC-016 row evidence |
| Collation/Charset Rule | character descriptors use byte-stable ASCII fixture values unless context supplies text |
| Timezone Rule | not applicable unless the context value is timezone-derived |
| Volatility | stable_per_transaction |
| Determinism | not foldable for execution-context reads or procedural diagnostics |
| Side Effects | none |
| SBLR Binding | sblr.expr.mga_isolation_profile.v3 |
| AST Binding | ast.expr.mga_isolation_profile |
| Engine Entrypoint | mga_isolation_profile |
| Security Policy | reads transaction isolation context only; preserves MGA authority |
| Error Semantics | arity/type/context/procedural diagnostics use builtin error compatibility matrix and SBSFC-016 evidence; diagnostic=SBSQL.FUNCTION.INVALID_INPUT for arity/type/context violations where applicable |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select mga_isolation_profile() from app.sample_values;
```

### `mga_snapshot_id`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.mga_snapshot_id |
| UUID | 019dffbb-f000-7a46-9e81-513cece671a9 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | mga_snapshot_id |
| Return Type Rule | current transaction snapshot visible-through local transaction id; descriptor=uint64 |
| Coercion Rule | no coercion for nullary transaction-context reads |
| Null Behavior | returns NULL uint64 without an active transaction context |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | stable_per_transaction |
| Determinism | not foldable; reads engine-owned MGA transaction context |
| Side Effects | none |
| SBLR Binding | sblr.expr.mga_snapshot_id.v3 |
| AST Binding | ast.expr.mga_snapshot_id |
| Engine Entrypoint | mga_snapshot_id |
| Security Policy | reads engine-owned MGA snapshot boundary only; no parser-side finality |
| Error Semantics | arity/context diagnostics use canonical transaction-context surface fixtures; value is non-mutating |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select mga_snapshot_id() from app.sample_values;
```

### `mod`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.mod |
| UUID | 019dffbb-f001-700b-8a00-00000000000b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | mod(...) |
| Return Type Rule | descriptor-authoritative mod numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_mod.v3 |
| AST Binding | ast.expr.scalar_mod |
| Engine Entrypoint | scalar_mod |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select mod(arg_1) from app.sample_values;
```

### `name`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.name |
| UUID | 019dffbb-f001-7457-8a00-000000000457 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | name(...) |
| Return Type Rule | runtime-defined by engine entrypoint name |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_name.v3 |
| AST Binding | ast.expr.scalar_name |
| Engine Entrypoint | name |
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
select name(arg_1) from app.sample_values;
```

### `name_resolution`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.name_resolution |
| UUID | 019dffbb-f000-7a6c-a699-9bba20da21c2 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | name_resolution(...) |
| Return Type Rule | runtime-defined by engine entrypoint name_resolution |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_name_resolution.v3 |
| AST Binding | ast.expr.scalar_name_resolution |
| Engine Entrypoint | name_resolution |
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
select name_resolution(arg_1) from app.sample_values;
```

### `native_future`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.native_future |
| UUID | 019dffbb-f000-7763-a650-eec7333d51c3 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | native_future(...) |
| Return Type Rule | runtime-defined by engine entrypoint native_future |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_native_future.v3 |
| AST Binding | ast.expr.scalar_native_future |
| Engine Entrypoint | native_future |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select native_future(arg_1) from app.sample_values;
```

### `native_now`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.native_now |
| UUID | 019dffbb-f000-7740-aa9f-8e51b515fe7b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | native_now(...) |
| Return Type Rule | runtime-defined by engine entrypoint native_now |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_native_now.v3 |
| AST Binding | ast.expr.scalar_native_now |
| Engine Entrypoint | native_now |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select native_now(arg_1) from app.sample_values;
```

### `natural`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.natural |
| UUID | 019dffbb-f000-7da0-ba8a-e1166968b648 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | natural(...) |
| Return Type Rule | runtime-defined by engine entrypoint natural |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_natural.v3 |
| AST Binding | ast.expr.scalar_natural |
| Engine Entrypoint | natural |
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
select natural(arg_1) from app.sample_values;
```

### `nested_subquery_max_depth`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.nested_subquery_max_depth |
| UUID | 019dffbb-f000-7ce1-9dc2-af0a73de8b9e |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | nested_subquery_max_depth() |
| Return Type Rule | fixed policy limit for nested subquery depth; descriptor=uint64 |
| Coercion Rule | no input coercion |
| Null Behavior | never null |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | immutable_policy |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.nested_subquery_max_depth.v3 |
| AST Binding | ast.expr.nested_subquery_max_depth |
| Engine Entrypoint | nested_subquery_max_depth |
| Security Policy | fixed public parser/runtime policy, no catalog or storage authority |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select nested_subquery_max_depth() from app.sample_values;
```

### `nextval`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.nextval |
| UUID | 019dffbb-f000-755e-99df-3c5aee1a11bd |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | nextval(...) |
| Return Type Rule | runtime-defined by engine entrypoint nextval |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_nextval.v3 |
| AST Binding | ast.expr.scalar_nextval |
| Engine Entrypoint | nextval |
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
select nextval(arg_1) from app.sample_values;
```

### `no_request`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.no_request |
| UUID | 019dffbb-f001-743d-8a00-00000000043d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | no_request(...) |
| Return Type Rule | runtime-defined by engine entrypoint no_request |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_no_request.v3 |
| AST Binding | ast.expr.scalar_no_request |
| Engine Entrypoint | no_request |
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
select no_request(arg_1) from app.sample_values;
```

### `no_statement`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.no_statement |
| UUID | 019dffbb-f001-743e-8a00-00000000043e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | no_statement(...) |
| Return Type Rule | runtime-defined by engine entrypoint no_statement |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_no_statement.v3 |
| AST Binding | ast.expr.scalar_no_statement |
| Engine Entrypoint | no_statement |
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
select no_statement(arg_1) from app.sample_values;
```

### `no_transaction`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.no_transaction |
| UUID | 019dffbb-f001-7441-8a00-000000000441 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | no_transaction(...) |
| Return Type Rule | runtime-defined by engine entrypoint no_transaction |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_no_transaction.v3 |
| AST Binding | ast.expr.scalar_no_transaction |
| Engine Entrypoint | no_transaction |
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
select no_transaction(arg_1) from app.sample_values;
```

### `none`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.none |
| UUID | 019dffbb-f001-7421-8a00-000000000421 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | none(...) |
| Return Type Rule | runtime-defined by engine entrypoint none |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_none.v3 |
| AST Binding | ast.expr.scalar_none |
| Engine Entrypoint | none |
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
select none(arg_1) from app.sample_values;
```

### `normalize`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.normalize |
| UUID | 019dffbb-f000-7493-ba3e-ea9d3d34c515 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | normalize |
| Return Type Rule | runtime-defined by engine entrypoint normalize |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_normalize.v3 |
| AST Binding | ast.expr.scalar_normalize |
| Engine Entrypoint | normalize |
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
select normalize() from app.sample_values;
```

### `normalize`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.normalize_text_form |
| UUID | 019dffbb-f000-7a5c-863a-286c6fb0f188 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | normalize(text[,NFC\|NFD\|NFKC\|NFKD]) |
| Return Type Rule | runtime-defined by engine entrypoint normalize |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_normalize_text_form.v3 |
| AST Binding | ast.expr.scalar_normalize_text_form |
| Engine Entrypoint | normalize |
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
select normalize(text_value_1) from app.sample_values;
```

### `not_found`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.not_found |
| UUID | 019dffbb-f000-7c69-8997-74d875b1db0e |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | not_found |
| Return Type Rule | boolean procedural condition derived from current SQLSTATE; descriptor=boolean |
| Coercion Rule | no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted |
| Null Behavior | context-backed typed null or diagnostic behavior follows SBSFC-016 row evidence |
| Collation/Charset Rule | character descriptors use byte-stable ASCII fixture values unless context supplies text |
| Timezone Rule | not applicable unless the context value is timezone-derived |
| Volatility | stable_per_statement |
| Determinism | not foldable for execution-context reads or procedural diagnostics |
| Side Effects | none |
| SBLR Binding | sblr.expr.not_found.v3 |
| AST Binding | ast.expr.not_found |
| Engine Entrypoint | not_found |
| Security Policy | reads procedural SQLSTATE context only |
| Error Semantics | arity/type/context/procedural diagnostics use builtin error compatibility matrix and SBSFC-016 evidence; diagnostic=SBSQL.FUNCTION.INVALID_INPUT for arity/type/context violations where applicable |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select not_found() from app.sample_values;
```

### `notice`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.notice |
| UUID | 019dffbb-f001-741c-8a00-00000000041c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | notice(...) |
| Return Type Rule | runtime-defined by engine entrypoint notice |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_notice.v3 |
| AST Binding | ast.expr.scalar_notice |
| Engine Entrypoint | notice |
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
select notice(arg_1) from app.sample_values;
```

### `null_concat_returns_null`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.null_concat_returns_null |
| UUID | 019dffbb-f000-7e00-950d-6a5839810f74 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | null_concat_returns_null(...) |
| Return Type Rule | runtime-defined by engine entrypoint null_concat_returns_null |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_null_concat_returns_null.v3 |
| AST Binding | ast.expr.scalar_null_concat_returns_null |
| Engine Entrypoint | null_concat_returns_null |
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
select null_concat_returns_null(arg_1) from app.sample_values;
```

### `null_in_aggregate_skipped`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.null_in_aggregate_skipped |
| UUID | 019dffbb-f000-78fd-a725-586e0fb25042 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | null_in_aggregate_skipped(...) |
| Return Type Rule | runtime-defined by engine entrypoint null_in_aggregate_skipped |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_null_in_aggregate_skipped.v3 |
| AST Binding | ast.expr.scalar_null_in_aggregate_skipped |
| Engine Entrypoint | null_in_aggregate_skipped |
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
select null_in_aggregate_skipped(arg_1) from app.sample_values;
```

### `null_in_unique_constraint`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.null_in_unique_constraint |
| UUID | 019dffbb-f000-7c0c-93ec-584365a461cc |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | null_in_unique_constraint(...) |
| Return Type Rule | runtime-defined by engine entrypoint null_in_unique_constraint |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_null_in_unique_constraint.v3 |
| AST Binding | ast.expr.scalar_null_in_unique_constraint |
| Engine Entrypoint | null_in_unique_constraint |
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
select null_in_unique_constraint(arg_1) from app.sample_values;
```

### `null_ordering_default_for_asc`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.null_ordering_default_for_asc |
| UUID | 019dffbb-f000-7484-a717-6882919c9e73 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | null_ordering_default_for_asc(...) |
| Return Type Rule | runtime-defined by engine entrypoint null_ordering_default_for_asc |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_null_ordering_default_for_asc.v3 |
| AST Binding | ast.expr.scalar_null_ordering_default_for_asc |
| Engine Entrypoint | null_ordering_default_for_asc |
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
select null_ordering_default_for_asc(arg_1) from app.sample_values;
```

### `null_ordering_default_for_desc`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.null_ordering_default_for_desc |
| UUID | 019dffbb-f000-7cc6-8e2d-a85a04413bde |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | null_ordering_default_for_desc(...) |
| Return Type Rule | runtime-defined by engine entrypoint null_ordering_default_for_desc |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_null_ordering_default_for_desc.v3 |
| AST Binding | ast.expr.scalar_null_ordering_default_for_desc |
| Engine Entrypoint | null_ordering_default_for_desc |
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
select null_ordering_default_for_desc(arg_1) from app.sample_values;
```

### `nullif`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.nullif |
| UUID | 019de5fc-2400-7603-9beb-123ee7f5f827 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | nullif(a,b) |
| Return Type Rule | null if equal using descriptor comparison semantics |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | stable_by_arguments |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_nullif.v3 |
| AST Binding | ast.expr.scalar_nullif |
| Engine Entrypoint | scalar_nullif |
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
select nullif(arg_1, arg_2) from app.sample_values;
```

### `numeric_division_by_zero`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.numeric_division_by_zero |
| UUID | 019dffbb-f000-7baa-9cad-ac6f384f8a9e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | numeric_division_by_zero(...) |
| Return Type Rule | runtime-defined by engine entrypoint numeric_division_by_zero |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_numeric_division_by_zero.v3 |
| AST Binding | ast.expr.scalar_numeric_division_by_zero |
| Engine Entrypoint | numeric_division_by_zero |
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
select numeric_division_by_zero(arg_1) from app.sample_values;
```

### `numeric_overflow_behavior`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.numeric_overflow_behavior |
| UUID | 019dffbb-f000-7145-b054-1c5191e853be |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | numeric_overflow_behavior(...) |
| Return Type Rule | runtime-defined by engine entrypoint numeric_overflow_behavior |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_numeric_overflow_behavior.v3 |
| AST Binding | ast.expr.scalar_numeric_overflow_behavior |
| Engine Entrypoint | numeric_overflow_behavior |
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
select numeric_overflow_behavior(arg_1) from app.sample_values;
```

### `nvl`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.nvl |
| UUID | 019dffbb-f000-799b-8d41-698142ec6713 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | nvl(...) |
| Return Type Rule | runtime-defined by engine entrypoint nvl |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.native_surface.nvl.v3 |
| AST Binding | ast.expr.nvl |
| Engine Entrypoint | nvl |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select nvl(arg_1) from app.sample_values;
```

### `nvl2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.nvl2 |
| UUID | 019dffbb-f001-702a-8a00-00000000002a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | nvl2(...) |
| Return Type Rule | descriptor-authoritative nvl2 conditional/text result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix; common result descriptor selected by binder/lowering route |
| Null Behavior | conditional function-specific NULL handling follows SBSFC-011 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | stable_by_arguments |
| Determinism | foldable only when selected branch and arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_nvl2.v3 |
| AST Binding | ast.expr.scalar_nvl2 |
| Engine Entrypoint | scalar_nvl2 |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/compatibility errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select nvl2(arg_1) from app.sample_values;
```

### `object_resolution_failed`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.object_resolution_failed |
| UUID | 019dffbb-f001-7448-8a00-000000000448 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | object_resolution_failed(...) |
| Return Type Rule | runtime-defined by engine entrypoint object_resolution_failed |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_object_resolution_failed.v3 |
| AST Binding | ast.expr.scalar_object_resolution_failed |
| Engine Entrypoint | object_resolution_failed |
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
select object_resolution_failed(arg_1) from app.sample_values;
```

### `octet_length`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.octet_length |
| UUID | 019de5fc-2400-708e-b598-9f4c1ace95a3 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | octet_length(value) |
| Return Type Rule | byte length of encoded value |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_octet_length.v3 |
| AST Binding | ast.expr.scalar_octet_length |
| Engine Entrypoint | scalar_octet_length |
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
select octet_length(value_1) from app.sample_values;
```

### `on`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.on |
| UUID | 019dffbb-f000-74fc-83ea-962ee87ffe47 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | on(...) |
| Return Type Rule | runtime-defined by engine entrypoint on |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_on.v3 |
| AST Binding | ast.expr.scalar_on |
| Engine Entrypoint | True |
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
select on(arg_1) from app.sample_values;
```

### `open`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.open |
| UUID | 019dffbb-f000-75aa-a9d8-595a0982e40c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | open(...) |
| Return Type Rule | runtime-defined by engine entrypoint open |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_open.v3 |
| AST Binding | ast.expr.scalar_open |
| Engine Entrypoint | open |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select open(arg_1) from app.sample_values;
```

### `operation_evidence_required`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.operation_evidence_required |
| UUID | 019dffbb-f000-7355-ab2f-42ebcecc60e8 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | operation_evidence_required(...) |
| Return Type Rule | runtime-defined by engine entrypoint operation_evidence_required |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_operation_evidence_required.v3 |
| AST Binding | ast.expr.scalar_operation_evidence_required |
| Engine Entrypoint | operation_evidence_required |
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
select operation_evidence_required(arg_1) from app.sample_values;
```

### `operator_overload_unresolved`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.operator_overload_unresolved |
| UUID | 019dffbb-f001-7440-8a00-000000000440 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | operator_overload_unresolved(...) |
| Return Type Rule | runtime-defined by engine entrypoint operator_overload_unresolved |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_operator_overload_unresolved.v3 |
| AST Binding | ast.expr.scalar_operator_overload_unresolved |
| Engine Entrypoint | operator_overload_unresolved |
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
select operator_overload_unresolved(arg_1) from app.sample_values;
```

### `oracle_decode`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.oracle_decode |
| UUID | 019dffbb-f000-7750-becc-e2c2bca4a98f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | oracle_decode(...) |
| Return Type Rule | descriptor-authoritative oracle_decode conditional/text result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix; common result descriptor selected by binder/lowering route |
| Null Behavior | conditional function-specific NULL handling follows SBSFC-011 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | stable_by_arguments |
| Determinism | foldable only when selected branch and arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_oracle_decode.v3 |
| AST Binding | ast.expr.scalar_oracle_decode |
| Engine Entrypoint | scalar_oracle_decode |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/compatibility errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select oracle_decode(arg_1) from app.sample_values;
```

### `outer`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.outer |
| UUID | 019dffbb-f000-744f-bb5e-02c2ee9e1a1b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | outer(...) |
| Return Type Rule | runtime-defined by engine entrypoint outer |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_outer.v3 |
| AST Binding | ast.expr.scalar_outer |
| Engine Entrypoint | outer |
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
select outer(arg_1) from app.sample_values;
```

### `overlay`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.overlay |
| UUID | 019de5fc-2400-7390-9528-049a6e15ad82 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | overlay(string placing string from start [for length]) |
| Return Type Rule | SQL overlay semantics |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_overlay.v3 |
| AST Binding | ast.expr.scalar_overlay |
| Engine Entrypoint | scalar_overlay |
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
select overlay(text_value_1) from app.sample_values;
```

### `parameter_marker`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.parameter_marker |
| UUID | 019dffbb-f001-7418-8a00-000000000418 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | parameter_marker(...) |
| Return Type Rule | runtime-defined by engine entrypoint parameter_marker |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_parameter_marker.v3 |
| AST Binding | ast.expr.scalar_parameter_marker |
| Engine Entrypoint | parameter_marker |
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
select parameter_marker(arg_1) from app.sample_values;
```

### `parameter_marker_max_count`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.parameter_marker_max_count |
| UUID | 019dffbb-f000-7fa7-a65e-aeec32feaeba |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | parameter_marker_max_count(...) |
| Return Type Rule | runtime-defined by engine entrypoint parameter_marker_max_count |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_parameter_marker_max_count.v3 |
| AST Binding | ast.expr.scalar_parameter_marker_max_count |
| Engine Entrypoint | parameter_marker_max_count |
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
select parameter_marker_max_count(arg_1) from app.sample_values;
```

### `parser_only`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.parser_only |
| UUID | 019dffbb-f001-740b-8a00-00000000040b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | parser_only(...) |
| Return Type Rule | runtime-defined by engine entrypoint parser_only |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_parser_only.v3 |
| AST Binding | ast.expr.scalar_parser_only |
| Engine Entrypoint | parser_only |
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
select parser_only(arg_1) from app.sample_values;
```

### `part`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.part |
| UUID | 019dffbb-f001-7439-8a00-000000000439 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | part(...) |
| Return Type Rule | runtime-defined by engine entrypoint part |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_part.v3 |
| AST Binding | ast.expr.scalar_part |
| Engine Entrypoint | part |
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
select part(arg_1) from app.sample_values;
```

### `performance`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.performance |
| UUID | 019dffbb-f001-740a-8a00-00000000040a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | performance(...) |
| Return Type Rule | runtime-defined by engine entrypoint performance |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_performance.v3 |
| AST Binding | ast.expr.scalar_performance |
| Engine Entrypoint | performance |
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
select performance(arg_1) from app.sample_values;
```

### `pg_advisory_lock`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.pg_advisory_lock |
| UUID | 019dffbb-f000-7e62-b00a-e220f1a97aa6 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pg_advisory_lock |
| Return Type Rule | runtime-defined by engine entrypoint pg_advisory_lock |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_pg_advisory_lock.v3 |
| AST Binding | ast.expr.scalar_pg_advisory_lock |
| Engine Entrypoint | pg_advisory_lock |
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
select pg_advisory_lock() from app.sample_values;
```

### `pg_advisory_lock`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.pg_advisory_lock_key |
| UUID | 019dffbb-f000-7679-88f6-9fb6958295eb |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pg_advisory_lock(key) |
| Return Type Rule | runtime-defined by engine entrypoint pg_advisory_lock |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_pg_advisory_lock_key.v3 |
| AST Binding | ast.expr.scalar_pg_advisory_lock_key |
| Engine Entrypoint | pg_advisory_lock |
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
select pg_advisory_lock(arg_1) from app.sample_values;
```

### `pg_advisory_unlock`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.pg_advisory_unlock |
| UUID | 019dffbb-f000-7d33-90a6-2d5bf09e58f5 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pg_advisory_unlock |
| Return Type Rule | runtime-defined by engine entrypoint pg_advisory_unlock |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_pg_advisory_unlock.v3 |
| AST Binding | ast.expr.scalar_pg_advisory_unlock |
| Engine Entrypoint | pg_advisory_unlock |
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
select pg_advisory_unlock() from app.sample_values;
```

### `pg_advisory_unlock`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.pg_advisory_unlock_key |
| UUID | 019dffbb-f000-70f1-b555-9a326605ec2f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pg_advisory_unlock(key) |
| Return Type Rule | runtime-defined by engine entrypoint pg_advisory_unlock |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_pg_advisory_unlock_key.v3 |
| AST Binding | ast.expr.scalar_pg_advisory_unlock_key |
| Engine Entrypoint | pg_advisory_unlock |
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
select pg_advisory_unlock(arg_1) from app.sample_values;
```

### `pg_advisory_xact_lock`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.pg_advisory_xact_lock |
| UUID | 019dffbb-f000-70a8-8fa9-5767fb871532 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pg_advisory_xact_lock |
| Return Type Rule | runtime-defined by engine entrypoint pg_advisory_xact_lock |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_pg_advisory_xact_lock.v3 |
| AST Binding | ast.expr.scalar_pg_advisory_xact_lock |
| Engine Entrypoint | pg_advisory_xact_lock |
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
select pg_advisory_xact_lock() from app.sample_values;
```

### `pg_advisory_xact_lock`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.pg_advisory_xact_lock_key |
| UUID | 019dffbb-f000-79c9-83b8-39046c9a6142 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pg_advisory_xact_lock(key) |
| Return Type Rule | runtime-defined by engine entrypoint pg_advisory_xact_lock |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_pg_advisory_xact_lock_key.v3 |
| AST Binding | ast.expr.scalar_pg_advisory_xact_lock_key |
| Engine Entrypoint | pg_advisory_xact_lock |
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
select pg_advisory_xact_lock(arg_1) from app.sample_values;
```

### `pg_cancel_backend`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.pg_cancel_backend |
| UUID | 019dffbb-f000-785c-9f0b-3d80dde164f6 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pg_cancel_backend |
| Return Type Rule | runtime-defined by engine entrypoint pg_cancel_backend |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_pg_cancel_backend.v3 |
| AST Binding | ast.expr.scalar_pg_cancel_backend |
| Engine Entrypoint | pg_cancel_backend |
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
select pg_cancel_backend() from app.sample_values;
```

### `pg_cancel_backend`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.pg_cancel_backend_pid |
| UUID | 019dffbb-f000-7ade-a48b-881f56660f50 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pg_cancel_backend(pid) |
| Return Type Rule | runtime-defined by engine entrypoint pg_cancel_backend |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_pg_cancel_backend_pid.v3 |
| AST Binding | ast.expr.scalar_pg_cancel_backend_pid |
| Engine Entrypoint | pg_cancel_backend |
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
select pg_cancel_backend(arg_1) from app.sample_values;
```

### `pg_terminate_backend`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.pg_terminate_backend |
| UUID | 019dffbb-f000-7086-9dd0-64ffd62787f5 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pg_terminate_backend |
| Return Type Rule | runtime-defined by engine entrypoint pg_terminate_backend |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_pg_terminate_backend.v3 |
| AST Binding | ast.expr.scalar_pg_terminate_backend |
| Engine Entrypoint | pg_terminate_backend |
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
select pg_terminate_backend() from app.sample_values;
```

### `pg_terminate_backend`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.pg_terminate_backend_pid |
| UUID | 019dffbb-f000-785f-8546-242367d0d3d3 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pg_terminate_backend(pid) |
| Return Type Rule | runtime-defined by engine entrypoint pg_terminate_backend |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_pg_terminate_backend_pid.v3 |
| AST Binding | ast.expr.scalar_pg_terminate_backend_pid |
| Engine Entrypoint | pg_terminate_backend |
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
select pg_terminate_backend(arg_1) from app.sample_values;
```

### `pg_trgm`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.pg_trgm |
| UUID | 019dffbb-f000-7b42-a170-e02186cfb49b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pg_trgm(...) |
| Return Type Rule | runtime-defined by engine entrypoint pg_trgm |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_pg_trgm.v3 |
| AST Binding | ast.expr.scalar_pg_trgm |
| Engine Entrypoint | pg_trgm |
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
select pg_trgm(arg_1) from app.sample_values;
```

### `pg_try_advisory_lock`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.pg_try_advisory_lock |
| UUID | 019dffbb-f000-7023-98ab-6977caf6fe6a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pg_try_advisory_lock |
| Return Type Rule | runtime-defined by engine entrypoint pg_try_advisory_lock |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_pg_try_advisory_lock.v3 |
| AST Binding | ast.expr.scalar_pg_try_advisory_lock |
| Engine Entrypoint | pg_try_advisory_lock |
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
select pg_try_advisory_lock() from app.sample_values;
```

### `pg_try_advisory_lock`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.pg_try_advisory_lock_key |
| UUID | 019dffbb-f000-7040-bb2c-84260ef9488e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pg_try_advisory_lock(key) |
| Return Type Rule | runtime-defined by engine entrypoint pg_try_advisory_lock |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_pg_try_advisory_lock_key.v3 |
| AST Binding | ast.expr.scalar_pg_try_advisory_lock_key |
| Engine Entrypoint | pg_try_advisory_lock |
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
select pg_try_advisory_lock(arg_1) from app.sample_values;
```

### `pg_typeof`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.pg_typeof |
| UUID | 019dffbb-f000-7115-af00-f00a23306cc8 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pg_typeof(...) |
| Return Type Rule | runtime-defined by engine entrypoint pg_typeof |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_pg_typeof.v3 |
| AST Binding | ast.expr.scalar_pg_typeof |
| Engine Entrypoint | pg_typeof |
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
select pg_typeof(arg_1) from app.sample_values;
```

### `pg_xact_status`

| Property | Value |
| --- | --- |
| Builtin ID | sb.session.pg_xact_status |
| UUID | 019dffbb-f000-71cf-af82-3d43e462b2a6 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pg_xact_status([transaction_id]) |
| Return Type Rule | current local transaction status label as character; returns in_progress for current local transaction, unknown for non-current ids, or NULL without transaction context |
| Coercion Rule | optional transaction id argument must be int64-compatible; no parser-side finality or donor transaction authority |
| Null Behavior | NULL argument returns NULL character; no current transaction context returns NULL for nullary call |
| Collation/Charset Rule | returns byte-stable ASCII status labels |
| Timezone Rule | not applicable |
| Volatility | stable_per_transaction |
| Determinism | not foldable; reads active transaction context |
| Side Effects | none |
| SBLR Binding | sblr.expr.pg_xact_status.v3 |
| AST Binding | ast.expr.pg_xact_status |
| Engine Entrypoint | pg_xact_status |
| Security Policy | reads active local transaction id status only; no parser-side finality and no transaction state mutation |
| Error Semantics | arity/type/context diagnostics use canonical transaction-context fixtures; status is context-backed and non-mutating |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select pg_xact_status(arg_1) from app.sample_values;
```

### `pi`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.pi |
| UUID | 019dffbb-f000-7f4c-a47c-899017f5f5e1 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pi(...) |
| Return Type Rule | descriptor-authoritative pi numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_pi.v3 |
| AST Binding | ast.expr.scalar_pi |
| Engine Entrypoint | scalar_pi |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select pi(arg_1) from app.sample_values;
```

### `pid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.pid |
| UUID | 019dffbb-f000-7636-93d6-21eb588ac105 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pid(...) |
| Return Type Rule | runtime-defined by engine entrypoint pid |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_pid.v3 |
| AST Binding | ast.expr.scalar_pid |
| Engine Entrypoint | pid |
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
select pid(arg_1) from app.sample_values;
```

### `policy_blocked`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.policy_blocked |
| UUID | 019dffbb-f001-741b-8a00-00000000041b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | policy_blocked(...) |
| Return Type Rule | runtime-defined by engine entrypoint policy_blocked |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_policy_blocked.v3 |
| AST Binding | ast.expr.scalar_policy_blocked |
| Engine Entrypoint | policy_blocked |
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
select policy_blocked(arg_1) from app.sample_values;
```

### `policy_blocked_diagnostic`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.policy_blocked_diagnostic |
| UUID | 019dffbb-f001-744b-8a00-00000000044b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | policy_blocked_diagnostic(...) |
| Return Type Rule | runtime-defined by engine entrypoint policy_blocked_diagnostic |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_policy_blocked_diagnostic.v3 |
| AST Binding | ast.expr.scalar_policy_blocked_diagnostic |
| Engine Entrypoint | policy_blocked_diagnostic |
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
select policy_blocked_diagnostic(arg_1) from app.sample_values;
```

### `policy_refusal_checkpoint_donor_log`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_checkpoint_donor_log |
| UUID | 019dffbb-f001-735f-8a00-00000000165f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | policy_refusal_checkpoint_donor_log(...) |
| Return Type Rule | runtime-defined by engine entrypoint policy_refusal_checkpoint_donor_log |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_refusal_checkpoint_donor_log.v3 |
| AST Binding | ast.expr.scalar_refusal_checkpoint_donor_log |
| Engine Entrypoint | policy_refusal_checkpoint_donor_log |
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
select policy_refusal_checkpoint_donor_log(arg_1) from app.sample_values;
```

### `policy_refusal_donor_log_mode`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_donor_log_mode |
| UUID | 019dffbb-f001-735e-8a00-00000000165e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | policy_refusal_donor_log_mode(...) |
| Return Type Rule | runtime-defined by engine entrypoint policy_refusal_donor_log_mode |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_refusal_donor_log_mode.v3 |
| AST Binding | ast.expr.scalar_refusal_donor_log_mode |
| Engine Entrypoint | policy_refusal_donor_log_mode |
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
select policy_refusal_donor_log_mode(arg_1) from app.sample_values;
```

### `position`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.position |
| UUID | 019de5fc-2400-73c0-bfb2-dbe2642698be |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | position(substring in string) |
| Return Type Rule | one-based position, zero/not-found behavior profile-gated |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_position.v3 |
| AST Binding | ast.expr.scalar_position |
| Engine Entrypoint | scalar_position |
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
select position(text_value_1) from app.sample_values;
```

### `post_event`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.post_event |
| UUID | 019dffbb-f001-742c-8a00-00000000042c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | post_event(...) |
| Return Type Rule | runtime-defined by engine entrypoint post_event |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_post_event.v3 |
| AST Binding | ast.expr.scalar_post_event |
| Engine Entrypoint | post_event |
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
select post_event(arg_1) from app.sample_values;
```

### `power`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.power |
| UUID | 019de5fc-2400-7712-ae50-b18f31eb386d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | power(base,exponent) |
| Return Type Rule | numeric exponentiation |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_power.v3 |
| AST Binding | ast.expr.scalar_power |
| Engine Entrypoint | scalar_power |
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
select power(arg_1, arg_2) from app.sample_values;
```

### `private_only`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.private_only |
| UUID | 019dffbb-f000-7fb9-99be-8ec4e5d63513 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | private_only(...) |
| Return Type Rule | runtime-defined by engine entrypoint private_only |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_private_only.v3 |
| AST Binding | ast.expr.scalar_private_only |
| Engine Entrypoint | private_only |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select private_only(arg_1) from app.sample_values;
```

### `private_only_keyword`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.private_only_keyword |
| UUID | 019dffbb-f000-7865-8f1c-1ceb0e6694e3 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | private_only_keyword(...) |
| Return Type Rule | runtime-defined by engine entrypoint private_only_keyword |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_private_only_keyword.v3 |
| AST Binding | ast.expr.scalar_private_only_keyword |
| Engine Entrypoint | private_only_keyword |
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
select private_only_keyword(arg_1) from app.sample_values;
```

### `private_profile_active`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.private_profile_active |
| UUID | 019dffbb-f000-79af-9a49-72c05396c0fe |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | private_profile_active(...) |
| Return Type Rule | runtime-defined by engine entrypoint private_profile_active |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_private_profile_active.v3 |
| AST Binding | ast.expr.scalar_private_profile_active |
| Engine Entrypoint | private_profile_active |
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
select private_profile_active(arg_1) from app.sample_values;
```

### `private_profile_read`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.private_profile_read |
| UUID | 019dffbb-f001-7416-8a00-000000000416 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | private_profile_read(...) |
| Return Type Rule | runtime-defined by engine entrypoint private_profile_read |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_private_profile_read.v3 |
| AST Binding | ast.expr.scalar_private_profile_read |
| Engine Entrypoint | private_profile_read |
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
select private_profile_read(arg_1) from app.sample_values;
```

### `private_surface_refused`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.private_surface_refused |
| UUID | 019dffbb-f001-743c-8a00-00000000043c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | private_surface_refused(...) |
| Return Type Rule | runtime-defined by engine entrypoint private_surface_refused |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_private_surface_refused.v3 |
| AST Binding | ast.expr.scalar_private_surface_refused |
| Engine Entrypoint | private_surface_refused |
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
select private_surface_refused(arg_1) from app.sample_values;
```

### `psql_case_not_found`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.psql_case_not_found |
| UUID | 019dffbb-f001-7445-8a00-000000000445 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | psql_case_not_found(...) |
| Return Type Rule | runtime-defined by engine entrypoint psql_case_not_found |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_psql_case_not_found.v3 |
| AST Binding | ast.expr.scalar_psql_case_not_found |
| Engine Entrypoint | psql_case_not_found |
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
select psql_case_not_found(arg_1) from app.sample_values;
```

### `public`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.public |
| UUID | 019dffbb-f001-741f-8a00-00000000041f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | public(...) |
| Return Type Rule | runtime-defined by engine entrypoint public |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_public.v3 |
| AST Binding | ast.expr.scalar_public |
| Engine Entrypoint | public |
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
select public(arg_1) from app.sample_values;
```

### `qualified_name_max_segments`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.qualified_name_max_segments |
| UUID | 019dffbb-f000-7659-8e00-14ae0d2a8679 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | qualified_name_max_segments(...) |
| Return Type Rule | runtime-defined by engine entrypoint qualified_name_max_segments |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_qualified_name_max_segments.v3 |
| AST Binding | ast.expr.scalar_qualified_name_max_segments |
| Engine Entrypoint | qualified_name_max_segments |
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
select qualified_name_max_segments(arg_1) from app.sample_values;
```

### `quote_ident`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.quote_ident |
| UUID | 019dffbb-f000-76f0-aec9-9876a0c0d839 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | quote_ident(...) |
| Return Type Rule | runtime-defined by engine entrypoint quote_ident |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_quote_ident.v3 |
| AST Binding | ast.expr.scalar_quote_ident |
| Engine Entrypoint | quote_ident |
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
select quote_ident(arg_1) from app.sample_values;
```

### `quote_literal`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.quote_literal |
| UUID | 019dffbb-f000-74cb-a153-346985aaa8da |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | quote_literal(...) |
| Return Type Rule | runtime-defined by engine entrypoint quote_literal |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_quote_literal.v3 |
| AST Binding | ast.expr.scalar_quote_literal |
| Engine Entrypoint | quote_literal |
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
select quote_literal(arg_1) from app.sample_values;
```

### `quote_nullable`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.quote_nullable |
| UUID | 019dffbb-f000-75a9-b704-ca4dedf93373 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | quote_nullable(...) |
| Return Type Rule | runtime-defined by engine entrypoint quote_nullable |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_quote_nullable.v3 |
| AST Binding | ast.expr.scalar_quote_nullable |
| Engine Entrypoint | quote_nullable |
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
select quote_nullable(arg_1) from app.sample_values;
```

### `quoted_identifier_case_rule`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.quoted_identifier_case_rule |
| UUID | 019dffbb-f000-7b8f-bf62-356190e6e131 |
| Kind | scalar |
| Syntax Forms | function_call, keyword_or_function_call |
| Overloads | quoted_identifier_case_rule(...) |
| Return Type Rule | descriptor-authoritative quoted_identifier_case_rule language/profile metadata result as implemented by the SBLR expression runtime |
| Coercion Rule | no input coercion unless the fixture surface supplies descriptor-authoritative arguments |
| Null Behavior | not applicable for nullary metadata forms; strict for argument-bearing forms |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | stable_per_catalog_profile |
| Determinism | profile-backed; foldable after binding when the active catalog/profile is fixed |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_quoted_identifier_case_rule.v3 |
| AST Binding | ast.expr.scalar_quoted_identifier_case_rule |
| Engine Entrypoint | scalar_quoted_identifier_case_rule |
| Security Policy | reads fixed language/catalog profile metadata only; no user data access |
| Error Semantics | arity/type errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select quoted_identifier_case_rule(arg_1) from app.sample_values;
```

### `radians`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.radians |
| UUID | 019dffbb-f000-7763-beb0-f3c603cf1636 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | radians(...) |
| Return Type Rule | descriptor-authoritative radians numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_radians.v3 |
| AST Binding | ast.expr.scalar_radians |
| Engine Entrypoint | scalar_radians |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select radians(arg_1) from app.sample_values;
```

### `raise`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.raise |
| UUID | 019dffbb-f000-7cca-9b59-f2dc0648f954 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | raise |
| Return Type Rule | procedural diagnostic route that emits SB_DIAG_PROCEDURAL_RAISE; descriptor=character |
| Coercion Rule | no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted |
| Null Behavior | context-backed typed null or diagnostic behavior follows SBSFC-016 row evidence |
| Collation/Charset Rule | character descriptors use byte-stable ASCII fixture values unless context supplies text |
| Timezone Rule | not applicable unless the context value is timezone-derived |
| Volatility | volatile_diagnostic |
| Determinism | not foldable for execution-context reads or procedural diagnostics |
| Side Effects | procedural_diagnostic |
| SBLR Binding | sblr.expr.procedural_raise.v3 |
| AST Binding | ast.expr.procedural_raise |
| Engine Entrypoint | raise |
| Security Policy | emits procedural diagnostic only; no storage or transaction finality authority |
| Error Semantics | arity/type/context/procedural diagnostics use builtin error compatibility matrix and SBSFC-016 evidence; diagnostic=SB_DIAG_PROCEDURAL_RAISE |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select raise() from app.sample_values;
```

### `random`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.random |
| UUID | 019de5fc-2400-776b-9c82-df50eb14f361 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | random() |
| Return Type Rule | random value; not foldable or index-eligible |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | volatile |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | volatile_value_generation |
| SBLR Binding | sblr.expr.scalar_random.v3 |
| AST Binding | ast.expr.scalar_random |
| Engine Entrypoint | scalar_random |
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
select random() from app.sample_values;
```

### `random_seed`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.random_seed |
| UUID | 019dffbb-f001-7408-8a00-000000000408 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | random_seed(...) |
| Return Type Rule | runtime-defined by engine entrypoint random_seed |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_random_seed.v3 |
| AST Binding | ast.expr.scalar_random_seed |
| Engine Entrypoint | random_seed |
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
select random_seed(arg_1) from app.sample_values;
```

### `random_seed_control`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.random_seed_control |
| UUID | 019dffbb-f001-7414-8a00-000000000414 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | random_seed_control(...) |
| Return Type Rule | runtime-defined by engine entrypoint random_seed_control |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_random_seed_control.v3 |
| AST Binding | ast.expr.scalar_random_seed_control |
| Engine Entrypoint | random_seed_control |
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
select random_seed_control(arg_1) from app.sample_values;
```

### `read_only_session`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.read_only_session |
| UUID | 019dffbb-f000-7d0b-90ef-f87e92540dae |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | read_only_session(...) |
| Return Type Rule | runtime-defined by engine entrypoint read_only_session |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_read_only_session.v3 |
| AST Binding | ast.expr.scalar_read_only_session |
| Engine Entrypoint | read_only_session |
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
select read_only_session(arg_1) from app.sample_values;
```

### `recursion_limit`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.recursion_limit |
| UUID | 019dffbb-f001-7409-8a00-000000000409 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | recursion_limit(...) |
| Return Type Rule | runtime-defined by engine entrypoint recursion_limit |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_recursion_limit.v3 |
| AST Binding | ast.expr.scalar_recursion_limit |
| Engine Entrypoint | recursion_limit |
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
select recursion_limit(arg_1) from app.sample_values;
```

### `recursion_max_depth`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.recursion_max_depth |
| UUID | 019dffbb-f000-7920-b6c9-d48d1dd5a32c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | recursion_max_depth(...) |
| Return Type Rule | runtime-defined by engine entrypoint recursion_max_depth |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_recursion_max_depth.v3 |
| AST Binding | ast.expr.scalar_recursion_max_depth |
| Engine Entrypoint | recursion_max_depth |
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
select recursion_max_depth(arg_1) from app.sample_values;
```

### `recursive_cte_max_depth`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.recursive_cte_max_depth |
| UUID | 019dffbb-f000-790e-ba11-949fac3dfad7 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | recursive_cte_max_depth() |
| Return Type Rule | fixed policy limit for recursive CTE depth; descriptor=uint64 |
| Coercion Rule | no input coercion |
| Null Behavior | never null |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | immutable_policy |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.recursive_cte_max_depth.v3 |
| AST Binding | ast.expr.recursive_cte_max_depth |
| Engine Entrypoint | recursive_cte_max_depth |
| Security Policy | fixed public parser/runtime policy, no catalog or storage authority |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select recursive_cte_max_depth() from app.sample_values;
```

### `recursive_schema_path_separator`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.recursive_schema_path_separator |
| UUID | 019dffbb-f000-78bf-b959-b18674f097eb |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | recursive_schema_path_separator(...) |
| Return Type Rule | runtime-defined by engine entrypoint recursive_schema_path_separator |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_recursive_schema_path_separator.v3 |
| AST Binding | ast.expr.scalar_recursive_schema_path_separator |
| Engine Entrypoint | recursive_schema_path_separator |
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
select recursive_schema_path_separator(arg_1) from app.sample_values;
```

### `refusal_array_subquery`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_array_subquery |
| UUID | 019dffbb-f001-7356-8a00-000000001656 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_array_subquery |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_array_subquery() from app.sample_values;
```

### `refusal_at`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_at |
| UUID | 019dffbb-f001-735a-8a00-00000000165a |
| Kind | scalar |
| Syntax Forms | variable_reference |
| Overloads | sb.scalar.refusal_at |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_at() from app.sample_values;
```

### `refusal_at_at`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_at_at |
| UUID | 019dffbb-f001-7358-8a00-000000001658 |
| Kind | scalar |
| Syntax Forms | operator_expression |
| Overloads | sb.scalar.refusal_at_at |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_at_at() from app.sample_values;
```

### `refusal_atomic`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_atomic |
| UUID | 019dffbb-f001-7337-8a00-000000001637 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_atomic |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_atomic() from app.sample_values;
```

### `refusal_attach`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_attach |
| UUID | 019dffbb-f001-7340-8a00-000000001640 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_attach |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_attach() from app.sample_values;
```

### `refusal_call`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_call |
| UUID | 019dffbb-f001-7338-8a00-000000001638 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_call |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_call() from app.sample_values;
```

### `refusal_collate`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_collate |
| UUID | 019dffbb-f001-7350-8a00-000000001650 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_collate |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_collate() from app.sample_values;
```

### `refusal_current_query`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_current_query |
| UUID | 019dffbb-f001-7307-8a00-000000001607 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_current_query |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_current_query() from app.sample_values;
```

### `refusal_customer_table`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_customer_table |
| UUID | 019dffbb-f001-7357-8a00-000000001657 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_customer_table |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_customer_table() from app.sample_values;
```

### `refusal_desc`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_desc |
| UUID | 019dffbb-f001-7351-8a00-000000001651 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_desc |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_desc() from app.sample_values;
```

### `refusal_describe`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_describe |
| UUID | 019dffbb-f001-7341-8a00-000000001641 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_describe |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_describe() from app.sample_values;
```

### `refusal_detach`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_detach |
| UUID | 019dffbb-f001-7344-8a00-000000001644 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_detach |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_detach() from app.sample_values;
```

### `refusal_event`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_event |
| UUID | 019dffbb-f001-7327-8a00-000000001627 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_event |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_event() from app.sample_values;
```

### `refusal_exclude`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_exclude |
| UUID | 019dffbb-f001-7349-8a00-000000001649 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_exclude |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_exclude() from app.sample_values;
```

### `refusal_execute_dynamic_sql`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_execute_dynamic_sql |
| UUID | 019dffbb-f001-7342-8a00-000000001642 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_execute_dynamic_sql |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_execute_dynamic_sql() from app.sample_values;
```

### `refusal_existsnode`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_existsnode |
| UUID | 019dffbb-f001-7323-8a00-000000001623 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_existsnode |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_existsnode() from app.sample_values;
```

### `refusal_explain_donor_log_compatibilitytrue`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_explain_donor_log_compatibilitytrue |
| UUID | 019dffbb-f001-7334-8a00-000000001634 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_explain_donor_log_compatibilitytrue |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_explain_donor_log_compatibilitytrue() from app.sample_values;
```

### `refusal_explain_evidencetrue`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_explain_evidencetrue |
| UUID | 019dffbb-f001-7335-8a00-000000001635 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_explain_evidencetrue |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_explain_evidencetrue() from app.sample_values;
```

### `refusal_explain_waltrue`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_explain_waltrue |
| UUID | 019dffbb-f001-7333-8a00-000000001633 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_explain_waltrue |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_explain_waltrue() from app.sample_values;
```

### `refusal_forall`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_forall |
| UUID | 019dffbb-f001-7339-8a00-000000001639 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_forall |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_forall() from app.sample_values;
```

### `refusal_idempotency`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_idempotency |
| UUID | 019dffbb-f001-7322-8a00-000000001622 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_idempotency |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_idempotency() from app.sample_values;
```

### `refusal_identity`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_identity |
| UUID | 019dffbb-f001-7355-8a00-000000001655 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_identity |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_identity() from app.sample_values;
```

### `refusal_into`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_into |
| UUID | 019dffbb-f001-7345-8a00-000000001645 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_into |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_into() from app.sample_values;
```

### `refusal_lo_export`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_lo_export |
| UUID | 019dffbb-f001-7313-8a00-000000001613 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_lo_export |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_lo_export() from app.sample_values;
```

### `refusal_lo_import`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_lo_import |
| UUID | 019dffbb-f001-7312-8a00-000000001612 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_lo_import |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_lo_import() from app.sample_values;
```

### `refusal_model`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_model |
| UUID | 019dffbb-f001-7326-8a00-000000001626 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_model |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_model() from app.sample_values;
```

### `refusal_named_argument`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_named_argument |
| UUID | 019dffbb-f001-7359-8a00-000000001659 |
| Kind | scalar |
| Syntax Forms | operator_expression |
| Overloads | sb.scalar.refusal_named_argument |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_named_argument() from app.sample_values;
```

### `refusal_only_keyword`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_only_keyword |
| UUID | 019dffbb-f000-7772-9c96-f918199d80fc |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | refusal_only_keyword(...) |
| Return Type Rule | runtime-defined by engine entrypoint refusal_only_keyword |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_refusal_only_keyword.v3 |
| AST Binding | ast.expr.scalar_refusal_only_keyword |
| Engine Entrypoint | refusal_only_keyword |
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
select refusal_only_keyword(arg_1) from app.sample_values;
```

### `refusal_operator`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_operator |
| UUID | 019dffbb-f001-7328-8a00-000000001628 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_operator |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_operator() from app.sample_values;
```

### `refusal_operator_manifest_csv`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_operator_manifest_csv |
| UUID | 019dffbb-f001-7343-8a00-000000001643 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_operator_manifest_csv |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_operator_manifest_csv() from app.sample_values;
```

### `refusal_over`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_over |
| UUID | 019dffbb-f001-7348-8a00-000000001648 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_over |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_over() from app.sample_values;
```

### `refusal_overlaps`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_overlaps |
| UUID | 019dffbb-f001-7354-8a00-000000001654 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_overlaps |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_overlaps() from app.sample_values;
```

### `refusal_pg_backend_pid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_pg_backend_pid |
| UUID | 019dffbb-f001-7320-8a00-000000001620 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_pg_backend_pid |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_pg_backend_pid() from app.sample_values;
```

### `refusal_pg_cron`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_pg_cron |
| UUID | 019dffbb-f001-7319-8a00-000000001619 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_pg_cron |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_pg_cron() from app.sample_values;
```

### `refusal_pg_ls_dir`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_pg_ls_dir |
| UUID | 019dffbb-f001-7311-8a00-000000001611 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_pg_ls_dir |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_pg_ls_dir() from app.sample_values;
```

### `refusal_pg_read_binary_file`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_pg_read_binary_file |
| UUID | 019dffbb-f001-7308-8a00-000000001608 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_pg_read_binary_file |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_pg_read_binary_file() from app.sample_values;
```

### `refusal_pg_read_file`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_pg_read_file |
| UUID | 019dffbb-f001-7309-8a00-000000001609 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_pg_read_file |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_pg_read_file() from app.sample_values;
```

### `refusal_pg_read_server_files`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_pg_read_server_files |
| UUID | 019dffbb-f001-7310-8a00-000000001610 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_pg_read_server_files |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_pg_read_server_files() from app.sample_values;
```

### `refusal_pg_reload_conf`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_pg_reload_conf |
| UUID | 019dffbb-f001-7314-8a00-000000001614 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_pg_reload_conf |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_pg_reload_conf() from app.sample_values;
```

### `refusal_pg_rotate_logfile`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_pg_rotate_logfile |
| UUID | 019dffbb-f001-7315-8a00-000000001615 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_pg_rotate_logfile |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_pg_rotate_logfile() from app.sample_values;
```

### `refusal_pg_sleep`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_pg_sleep |
| UUID | 019dffbb-f001-7317-8a00-000000001617 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_pg_sleep |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_pg_sleep() from app.sample_values;
```

### `refusal_pg_terminate_session`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_pg_terminate_session |
| UUID | 019dffbb-f001-7318-8a00-000000001618 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_pg_terminate_session |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_pg_terminate_session() from app.sample_values;
```

### `refusal_rdb_get_context_client_pid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_rdb_get_context_client_pid |
| UUID | 019dffbb-f001-7330-8a00-000000001630 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_rdb_get_context_client_pid |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_rdb_get_context_client_pid() from app.sample_values;
```

### `refusal_rdb_get_context_user_session_name`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_rdb_get_context_user_session_name |
| UUID | 019dffbb-f001-7353-8a00-000000001653 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_rdb_get_context_user_session_name |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_rdb_get_context_user_session_name() from app.sample_values;
```

### `refusal_returning`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_returning |
| UUID | 019dffbb-f001-7347-8a00-000000001647 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_returning |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_returning() from app.sample_values;
```

### `refusal_sql_bulk_exceptions`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_sql_bulk_exceptions |
| UUID | 019dffbb-f001-7321-8a00-000000001621 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_sql_bulk_exceptions |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_sql_bulk_exceptions() from app.sample_values;
```

### `refusal_suspend`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_suspend |
| UUID | 019dffbb-f001-7346-8a00-000000001646 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_suspend |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_suspend() from app.sample_values;
```

### `refusal_sys_context_client_info`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_sys_context_client_info |
| UUID | 019dffbb-f001-7329-8a00-000000001629 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_sys_context_client_info |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_sys_context_client_info() from app.sample_values;
```

### `refusal_sys_context_userenv_name`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_sys_context_userenv_name |
| UUID | 019dffbb-f001-7352-8a00-000000001652 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_sys_context_userenv_name |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_sys_context_userenv_name() from app.sample_values;
```

### `refusal_system_variable_autocommit`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_system_variable_autocommit |
| UUID | 019dffbb-f001-7301-8a00-000000001601 |
| Kind | scalar |
| Syntax Forms | operator_expression |
| Overloads | sb.scalar.refusal_system_variable_autocommit |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_system_variable_autocommit() from app.sample_values;
```

### `refusal_system_variable_global_var`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_system_variable_global_var |
| UUID | 019dffbb-f001-7304-8a00-000000001604 |
| Kind | scalar |
| Syntax Forms | operator_expression |
| Overloads | sb.scalar.refusal_system_variable_global_var |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_system_variable_global_var() from app.sample_values;
```

### `refusal_system_variable_hostname`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_system_variable_hostname |
| UUID | 019dffbb-f001-7305-8a00-000000001605 |
| Kind | scalar |
| Syntax Forms | operator_expression |
| Overloads | sb.scalar.refusal_system_variable_hostname |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_system_variable_hostname() from app.sample_values;
```

### `refusal_system_variable_servername`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_system_variable_servername |
| UUID | 019dffbb-f001-7306-8a00-000000001606 |
| Kind | scalar |
| Syntax Forms | operator_expression |
| Overloads | sb.scalar.refusal_system_variable_servername |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_system_variable_servername() from app.sample_values;
```

### `refusal_system_variable_session_autocommit`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_system_variable_session_autocommit |
| UUID | 019dffbb-f001-7302-8a00-000000001602 |
| Kind | scalar |
| Syntax Forms | operator_expression |
| Overloads | sb.scalar.refusal_system_variable_session_autocommit |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_system_variable_session_autocommit() from app.sample_values;
```

### `refusal_system_variable_session_var`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_system_variable_session_var |
| UUID | 019dffbb-f001-7303-8a00-000000001603 |
| Kind | scalar |
| Syntax Forms | operator_expression |
| Overloads | sb.scalar.refusal_system_variable_session_var |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_system_variable_session_var() from app.sample_values;
```

### `refusal_system_variable_trancount`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_system_variable_trancount |
| UUID | 019dffbb-f001-7336-8a00-000000001636 |
| Kind | scalar |
| Syntax Forms | operator_expression |
| Overloads | sb.scalar.refusal_system_variable_trancount |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_system_variable_trancount() from app.sample_values;
```

### `refusal_updatexml`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_updatexml |
| UUID | 019dffbb-f001-7324-8a00-000000001624 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_updatexml |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_updatexml() from app.sample_values;
```

### `refusal_wal`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_wal |
| UUID | 019dffbb-f001-7316-8a00-000000001616 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_wal |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_wal() from app.sample_values;
```

### `refusal_with_nolock`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_with_nolock |
| UUID | 019dffbb-f001-7332-8a00-000000001632 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_with_nolock |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_with_nolock() from app.sample_values;
```

### `refusal_with_readpast`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_with_readpast |
| UUID | 019dffbb-f001-7331-8a00-000000001631 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_with_readpast |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_with_readpast() from app.sample_values;
```

### `refusal_xmlelement`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refusal_xmlelement |
| UUID | 019dffbb-f001-7325-8a00-000000001625 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sb.scalar.refusal_xmlelement |
| Return Type Rule | no successful value; runtime always emits SB_DIAG_FUNCTION_RUNTIME_REFUSAL before side effects |
| Coercion Rule | no coercion is applied; policy refusal is selected by parser/lowering route before argument coercion can authorize behavior |
| Null Behavior | not applicable; refusal occurs before value materialization |
| Collation/Charset Rule | not applicable; no text value is materialized |
| Timezone Rule | not applicable; no temporal value is materialized |
| Volatility | policy_refusal |
| Determinism | deterministic diagnostic for the canonical refused surface |
| Side Effects | none |
| SBLR Binding | sblr.expr.public_policy_refusal.v3 |
| AST Binding | ast.expr.public_policy_refusal |
| Engine Entrypoint | policy_runtime_refusal |
| Security Policy | public SBsql policy refusal; emits canonical runtime refusal before filesystem, process, WAL/donor-log, mutable session, extension, or private client-context side effects |
| Error Semantics | canonical message vector includes SB_DIAG_FUNCTION_RUNTIME_REFUSAL; no source SQL text or sensitive argument payload is execution authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | diagnostic_refusal |

#### Practical Form

```sql
select refusal_xmlelement() from app.sample_values;
```

### `refuse`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refuse |
| UUID | 019dffbb-f001-740e-8a00-00000000040e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | refuse(...) |
| Return Type Rule | runtime-defined by engine entrypoint refuse |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_refuse.v3 |
| AST Binding | ast.expr.scalar_refuse |
| Engine Entrypoint | refuse |
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
select refuse(arg_1) from app.sample_values;
```

### `refused`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.refused |
| UUID | 019dffbb-f001-7427-8a00-000000000427 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | refused(...) |
| Return Type Rule | runtime-defined by engine entrypoint refused |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_refused.v3 |
| AST Binding | ast.expr.scalar_refused |
| Engine Entrypoint | refused |
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
select refused(arg_1) from app.sample_values;
```

### `regional`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.regional |
| UUID | 019dffbb-f001-742d-8a00-00000000042d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | regional(...) |
| Return Type Rule | runtime-defined by engine entrypoint regional |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_regional.v3 |
| AST Binding | ast.expr.scalar_regional |
| Engine Entrypoint | regional |
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
select regional(arg_1) from app.sample_values;
```

### `relation_row_estimate`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.relation_row_estimate |
| UUID | 019dffbb-f000-7611-9e22-1b05b9a07f27 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | relation_row_estimate(); relation_row_estimate(table_uuid) |
| Return Type Rule | uint64 visible row-version estimate |
| Coercion Rule | table_uuid argument uses UUID text representation |
| Null Behavior | SQL NULL table_uuid returns SQL NULL |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | stable_per_transaction |
| Determinism | stable within current MGA transaction visibility context |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_relation_row_estimate.v3 |
| AST Binding | ast.expr.scalar_relation_row_estimate |
| Engine Entrypoint | relation_row_estimate |
| Security Policy | reads engine-owned local MGA relation metadata and row-version sidecars; no parser SQL, donor backend, cluster path, WAL, or SQLite finality shortcut |
| Error Semantics | invalid arity, missing database context, or MGA catalog load failure refuses with SB_DIAG_FUNCTION_INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | catalog_metadata_io |

#### Practical Form

```sql
select relation_row_estimate() from app.sample_values;
```

### `repeat`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.repeat |
| UUID | 019dffbb-f001-7019-8a00-000000000019 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | repeat(...) |
| Return Type Rule | descriptor-authoritative repeat text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_repeat.v3 |
| AST Binding | ast.expr.scalar_repeat |
| Engine Entrypoint | scalar_repeat |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select repeat(arg_1) from app.sample_values;
```

### `replace`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.replace |
| UUID | 019de5fc-2400-74b2-9533-7b90a832ebde |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | replace(string,from,to) |
| Return Type Rule | replace all non-overlapping matches |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_replace.v3 |
| AST Binding | ast.expr.scalar_replace |
| Engine Entrypoint | scalar_replace |
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
select replace(text_value_1, arg_2, arg_3) from app.sample_values;
```

### `request_key_required`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.request_key_required |
| UUID | 019dffbb-f000-7d13-9746-ca3d917efa06 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | request_key_required(...) |
| Return Type Rule | runtime-defined by engine entrypoint request_key_required |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_request_key_required.v3 |
| AST Binding | ast.expr.scalar_request_key_required |
| Engine Entrypoint | request_key_required |
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
select request_key_required(arg_1) from app.sample_values;
```

### `requires_function_authoring`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.requires_function_authoring |
| UUID | 019dffbb-f001-7442-8a00-000000000442 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | requires_function_authoring(...) |
| Return Type Rule | runtime-defined by engine entrypoint requires_function_authoring |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_requires_function_authoring.v3 |
| AST Binding | ast.expr.scalar_requires_function_authoring |
| Engine Entrypoint | requires_function_authoring |
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
select requires_function_authoring(arg_1) from app.sample_values;
```

### `requires_new_function`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.requires_new_function |
| UUID | 019dffbb-f001-7413-8a00-000000000413 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | requires_new_function(...) |
| Return Type Rule | runtime-defined by engine entrypoint requires_new_function |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_requires_new_function.v3 |
| AST Binding | ast.expr.scalar_requires_new_function |
| Engine Entrypoint | requires_new_function |
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
select requires_new_function(arg_1) from app.sample_values;
```

### `reserved`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.reserved |
| UUID | 019dffbb-f000-7027-9391-48d0c70775ac |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | reserved(...) |
| Return Type Rule | runtime-defined by engine entrypoint reserved |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_reserved.v3 |
| AST Binding | ast.expr.scalar_reserved |
| Engine Entrypoint | reserved |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select reserved(arg_1) from app.sample_values;
```

### `reserved_native_keyword`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.reserved_native_keyword |
| UUID | 019dffbb-f000-786d-aa5d-9b5de652c665 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | reserved_native_keyword(...) |
| Return Type Rule | runtime-defined by engine entrypoint reserved_native_keyword |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_reserved_native_keyword.v3 |
| AST Binding | ast.expr.scalar_reserved_native_keyword |
| Engine Entrypoint | reserved_native_keyword |
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
select reserved_native_keyword(arg_1) from app.sample_values;
```

### `resignal`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.resignal |
| UUID | 019dffbb-f000-7d41-b9a5-3e53cb0cc093 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | resignal |
| Return Type Rule | procedural diagnostic route that emits SB_DIAG_PROCEDURAL_RESIGNAL; descriptor=character |
| Coercion Rule | no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted |
| Null Behavior | context-backed typed null or diagnostic behavior follows SBSFC-016 row evidence |
| Collation/Charset Rule | character descriptors use byte-stable ASCII fixture values unless context supplies text |
| Timezone Rule | not applicable unless the context value is timezone-derived |
| Volatility | volatile_diagnostic |
| Determinism | not foldable for execution-context reads or procedural diagnostics |
| Side Effects | procedural_diagnostic |
| SBLR Binding | sblr.expr.procedural_resignal.v3 |
| AST Binding | ast.expr.procedural_resignal |
| Engine Entrypoint | resignal |
| Security Policy | emits procedural diagnostic only; no storage or transaction finality authority |
| Error Semantics | arity/type/context/procedural diagnostics use builtin error compatibility matrix and SBSFC-016 evidence; diagnostic=SB_DIAG_PROCEDURAL_RESIGNAL |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select resignal() from app.sample_values;
```

### `result_set_max_columns`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.result_set_max_columns |
| UUID | 019dffbb-f000-7726-9b0c-40be3bd93d8e |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | result_set_max_columns() |
| Return Type Rule | fixed policy limit for result columns; descriptor=uint64 |
| Coercion Rule | no input coercion |
| Null Behavior | never null |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | immutable_policy |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.result_set_max_columns.v3 |
| AST Binding | ast.expr.result_set_max_columns |
| Engine Entrypoint | result_set_max_columns |
| Security Policy | fixed public parser/runtime policy, no catalog or storage authority |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select result_set_max_columns() from app.sample_values;
```

### `result_set_max_rows_in_response`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.result_set_max_rows_in_response |
| UUID | 019dffbb-f000-7ebe-a81e-e658ad2c2cb5 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | result_set_max_rows_in_response(...) |
| Return Type Rule | runtime-defined by engine entrypoint result_set_max_rows_in_response |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_result_set_max_rows_in_response.v3 |
| AST Binding | ast.expr.scalar_result_set_max_rows_in_response |
| Engine Entrypoint | result_set_max_rows_in_response |
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
select result_set_max_rows_in_response(arg_1) from app.sample_values;
```

### `reverse`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.reverse |
| UUID | 019dffbb-f001-7014-8a00-000000000014 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | reverse(...) |
| Return Type Rule | descriptor-authoritative reverse text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_reverse.v3 |
| AST Binding | ast.expr.scalar_reverse |
| Engine Entrypoint | scalar_reverse |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select reverse(arg_1) from app.sample_values;
```

### `right`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.right |
| UUID | 019dffbb-f000-7a46-beee-8dcba999e3e3 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | right(...) |
| Return Type Rule | descriptor-authoritative right text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_right.v3 |
| AST Binding | ast.expr.scalar_right |
| Engine Entrypoint | scalar_right |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select right(arg_1) from app.sample_values;
```

### `round`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.round |
| UUID | 019de5fc-2400-70be-b269-d3cfed154d3b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | round(number[,scale]) |
| Return Type Rule | rounding mode descriptor/profile-gated |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_round.v3 |
| AST Binding | ast.expr.scalar_round |
| Engine Entrypoint | scalar_round |
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
select round(numeric_value_1) from app.sample_values;
```

### `rpad`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.rpad |
| UUID | 019dffbb-f001-7024-8a00-000000000024 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | rpad(...) |
| Return Type Rule | descriptor-authoritative rpad text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_rpad.v3 |
| AST Binding | ast.expr.scalar_rpad |
| Engine Entrypoint | scalar_rpad |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select rpad(arg_1) from app.sample_values;
```

### `rtrim`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.rtrim |
| UUID | 019dffbb-f001-7013-8a00-000000000013 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | rtrim(...) |
| Return Type Rule | descriptor-authoritative rtrim text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_rtrim.v3 |
| AST Binding | ast.expr.scalar_rtrim |
| Engine Entrypoint | scalar_rtrim |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select rtrim(arg_1) from app.sample_values;
```

### `safe_cast`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.safe_cast |
| UUID | 019dffbb-f000-7811-93e2-60cf11e4c6dc |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | safe_cast(...) |
| Return Type Rule | runtime-defined by engine entrypoint safe_cast |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_safe_cast.v3 |
| AST Binding | ast.expr.scalar_safe_cast |
| Engine Entrypoint | safe_cast |
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
select safe_cast(arg_1) from app.sample_values;
```

### `savepoint_active`

| Property | Value |
| --- | --- |
| Builtin ID | sb.session.savepoint_active |
| UUID | 019dffbb-f000-7a48-95d6-d055840554ec |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | savepoint_active; savepoint_active(name) |
| Return Type Rule | boolean active-savepoint predicate derived from engine-owned MGA savepoint markers; descriptor=boolean |
| Coercion Rule | optional savepoint name argument uses text representation; no parser-owned transaction or savepoint state |
| Null Behavior | NULL savepoint name returns NULL boolean; absent transaction/savepoint context returns false |
| Collation/Charset Rule | savepoint name matching is byte-stable over the engine savepoint marker name |
| Timezone Rule | not applicable |
| Volatility | stable_per_transaction |
| Determinism | not foldable; reads active MGA savepoint marker context |
| Side Effects | none |
| SBLR Binding | sblr.expr.savepoint_active.v3 |
| AST Binding | ast.expr.savepoint_active |
| Engine Entrypoint | savepoint_active |
| Security Policy | reads engine-owned MGA savepoint marker state only; no parser-side savepoint authority |
| Error Semantics | arity/type/context diagnostics use canonical transaction-context fixtures; predicate is non-mutating |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select savepoint_active() from app.sample_values;
```

### `sbsql_psql`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sbsql_psql |
| UUID | 019dffbb-f001-7406-8a00-000000000406 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sbsql_psql(...) |
| Return Type Rule | runtime-defined by engine entrypoint sbsql_psql |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_sbsql_psql.v3 |
| AST Binding | ast.expr.scalar_sbsql_psql |
| Engine Entrypoint | sbsql_psql |
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
select sbsql_psql(arg_1) from app.sample_values;
```

### `sbsql_syntax_future_version`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sbsql_syntax_future_version |
| UUID | 019dffbb-f000-72a2-aece-ab49525f1d92 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sbsql_syntax_future_version(...) |
| Return Type Rule | runtime-defined by engine entrypoint sbsql_syntax_future_version |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_sbsql_syntax_future_version.v3 |
| AST Binding | ast.expr.scalar_sbsql_syntax_future_version |
| Engine Entrypoint | sbsql_syntax_future_version |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select sbsql_syntax_future_version(arg_1) from app.sample_values;
```

### `sbsql_syntax_reserved`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sbsql_syntax_reserved |
| UUID | 019dffbb-f000-7595-964c-f4cfdf96c676 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sbsql_syntax_reserved(...) |
| Return Type Rule | runtime-defined by engine entrypoint sbsql_syntax_reserved |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_sbsql_syntax_reserved.v3 |
| AST Binding | ast.expr.scalar_sbsql_syntax_reserved |
| Engine Entrypoint | sbsql_syntax_reserved |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select sbsql_syntax_reserved(arg_1) from app.sample_values;
```

### `sbsql_v3`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sbsql_v3 |
| UUID | 019dffbb-f000-7566-b31c-8612ab0eb4ea |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sbsql_v3(...) |
| Return Type Rule | runtime-defined by engine entrypoint sbsql_v3 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_sbsql_v3.v3 |
| AST Binding | ast.expr.scalar_sbsql_v3 |
| Engine Entrypoint | sbsql_v3 |
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
select sbsql_v3(arg_1) from app.sample_values;
```

### `search_path`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.search_path |
| UUID | 019dffbb-f001-7405-8a00-000000000405 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | search_path(...) |
| Return Type Rule | runtime-defined by engine entrypoint search_path |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_search_path.v3 |
| AST Binding | ast.expr.scalar_search_path |
| Engine Entrypoint | search_path |
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
select search_path(arg_1) from app.sample_values;
```

### `security`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.security |
| UUID | 019dffbb-f001-7419-8a00-000000000419 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | security(...) |
| Return Type Rule | runtime-defined by engine entrypoint security |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_security.v3 |
| AST Binding | ast.expr.scalar_security |
| Engine Entrypoint | security |
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
select security(arg_1) from app.sample_values;
```

### `sep`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sep |
| UUID | 019dffbb-f001-7438-8a00-000000000438 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sep(...) |
| Return Type Rule | runtime-defined by engine entrypoint sep |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_sep.v3 |
| AST Binding | ast.expr.scalar_sep |
| Engine Entrypoint | sep |
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
select sep(arg_1) from app.sample_values;
```

### `sequence`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sequence |
| UUID | 019dffbb-f000-7636-a0c0-0fe8f694558b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sequence(...) |
| Return Type Rule | runtime-defined by engine entrypoint sequence |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_sequence.v3 |
| AST Binding | ast.expr.scalar_sequence |
| Engine Entrypoint | sequence |
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
select sequence(arg_1) from app.sample_values;
```

### `server_version`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.server_version |
| UUID | 019dffbb-f000-78a5-bd48-99786bcfbb1b |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | server_version |
| Return Type Rule | fixed ScratchBird server version string for this seed/runtime slice; descriptor=character |
| Coercion Rule | no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted |
| Null Behavior | context-backed typed null or diagnostic behavior follows SBSFC-016 row evidence |
| Collation/Charset Rule | character descriptors use byte-stable ASCII fixture values unless context supplies text |
| Timezone Rule | not applicable unless the context value is timezone-derived |
| Volatility | stable_per_release |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.server_version.v3 |
| AST Binding | ast.expr.server_version |
| Engine Entrypoint | server_version |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/context/procedural diagnostics use builtin error compatibility matrix and SBSFC-016 evidence; diagnostic=SBSQL.FUNCTION.INVALID_INPUT for arity/type/context violations where applicable |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select server_version() from app.sample_values;
```

### `server_version_num`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.server_version_num |
| UUID | 019dffbb-f000-76ea-b697-2336dae53893 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | server_version_num |
| Return Type Rule | fixed numeric ScratchBird server version for this seed/runtime slice; descriptor=uint64 |
| Coercion Rule | no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted |
| Null Behavior | context-backed typed null or diagnostic behavior follows SBSFC-016 row evidence |
| Collation/Charset Rule | character descriptors use byte-stable ASCII fixture values unless context supplies text |
| Timezone Rule | not applicable unless the context value is timezone-derived |
| Volatility | stable_per_release |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.server_version_num.v3 |
| AST Binding | ast.expr.server_version_num |
| Engine Entrypoint | server_version_num |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/context/procedural diagnostics use builtin error compatibility matrix and SBSFC-016 evidence; diagnostic=SBSQL.FUNCTION.INVALID_INPUT for arity/type/context violations where applicable |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select server_version_num() from app.sample_values;
```

### `session`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.session |
| UUID | 019dffbb-f001-7459-8a00-000000000459 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | session(...) |
| Return Type Rule | runtime-defined by engine entrypoint session |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_session.v3 |
| AST Binding | ast.expr.scalar_session |
| Engine Entrypoint | session |
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
select session(arg_1) from app.sample_values;
```

### `session_user`

| Property | Value |
| --- | --- |
| Builtin ID | sb.session.session_user |
| UUID | 019dffbb-f000-7df3-9feb-babec5813649 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | session_user |
| Return Type Rule | session user UUID from SBLR execution context; descriptor=uuid |
| Coercion Rule | no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted |
| Null Behavior | context-backed typed null or diagnostic behavior follows SBSFC-016 row evidence |
| Collation/Charset Rule | character descriptors use byte-stable ASCII fixture values unless context supplies text |
| Timezone Rule | not applicable unless the context value is timezone-derived |
| Volatility | stable_per_session |
| Determinism | not foldable for execution-context reads or procedural diagnostics |
| Side Effects | none |
| SBLR Binding | sblr.expr.session_user.v3 |
| AST Binding | ast.expr.session_user |
| Engine Entrypoint | session_user |
| Security Policy | reads session user UUID context only |
| Error Semantics | arity/type/context/procedural diagnostics use builtin error compatibility matrix and SBSFC-016 evidence; diagnostic=SBSQL.FUNCTION.INVALID_INPUT for arity/type/context violations where applicable |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select session_user() from app.sample_values;
```

### `sessions`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sessions |
| UUID | 019dffbb-f001-742a-8a00-00000000042a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sessions(...) |
| Return Type Rule | runtime-defined by engine entrypoint sessions |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_sessions.v3 |
| AST Binding | ast.expr.scalar_sessions |
| Engine Entrypoint | sessions |
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
select sessions(arg_1) from app.sample_values;
```

### `set_config`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.set_config |
| UUID | 019dffbb-f000-7a3a-b91c-c410a41e4bee |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | set_config |
| Return Type Rule | runtime-defined by engine entrypoint set_config |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_set_config.v3 |
| AST Binding | ast.expr.scalar_set_config |
| Engine Entrypoint | set_config |
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
select set_config() from app.sample_values;
```

### `set_config`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.set_config_name_value_is_local |
| UUID | 019dffbb-f000-7181-b8d3-b8a0930f9358 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | set_config(name,value,is_local) |
| Return Type Rule | runtime-defined by engine entrypoint set_config |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_set_config_name_value_is_local.v3 |
| AST Binding | ast.expr.scalar_set_config_name_value_is_local |
| Engine Entrypoint | set_config |
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
select set_config(arg_1, value_2, arg_3) from app.sample_values;
```

### `setval`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.setval |
| UUID | 019dffbb-f000-737c-938d-9f8dcf296ff9 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | setval(...) |
| Return Type Rule | runtime-defined by engine entrypoint setval |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_setval.v3 |
| AST Binding | ast.expr.scalar_setval |
| Engine Entrypoint | setval |
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
select setval(arg_1) from app.sample_values;
```

### `sha1`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sha1 |
| UUID | 019dffbb-f001-701e-8a00-00000000001e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sha1(...) |
| Return Type Rule | descriptor-authoritative sha1 digest result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary digest inputs |
| Null Behavior | strict unless SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_sha1.v3 |
| AST Binding | ast.expr.scalar_sha1 |
| Engine Entrypoint | scalar_sha1 |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/format errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select sha1(arg_1) from app.sample_values;
```

### `sha224`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sha224 |
| UUID | 019dffbb-f000-754a-bb3f-374cd61c7463 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sha224(...) |
| Return Type Rule | descriptor-authoritative sha224 digest result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary digest inputs |
| Null Behavior | strict unless SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_sha224.v3 |
| AST Binding | ast.expr.scalar_sha224 |
| Engine Entrypoint | scalar_sha224 |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/format errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select sha224(arg_1) from app.sample_values;
```

### `sha256`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sha256 |
| UUID | 019dffbb-f001-701f-8a00-00000000001f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sha256(...) |
| Return Type Rule | descriptor-authoritative sha256 digest result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary digest inputs |
| Null Behavior | strict unless SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_sha256.v3 |
| AST Binding | ast.expr.scalar_sha256 |
| Engine Entrypoint | scalar_sha256 |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/format errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select sha256(arg_1) from app.sample_values;
```

### `sha384`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sha384 |
| UUID | 019dffbb-f000-7596-a0e4-d04b85eeee8b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sha384(...) |
| Return Type Rule | descriptor-authoritative sha384 digest result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary digest inputs |
| Null Behavior | strict unless SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_sha384.v3 |
| AST Binding | ast.expr.scalar_sha384 |
| Engine Entrypoint | scalar_sha384 |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/format errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select sha384(arg_1) from app.sample_values;
```

### `sha512`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sha512 |
| UUID | 019dffbb-f001-7020-8a00-000000000020 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sha512(...) |
| Return Type Rule | descriptor-authoritative sha512 digest result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary digest inputs |
| Null Behavior | strict unless SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_sha512.v3 |
| AST Binding | ast.expr.scalar_sha512 |
| Engine Entrypoint | scalar_sha512 |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/format errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select sha512(arg_1) from app.sample_values;
```

### `show`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.show |
| UUID | 019dffbb-f000-7e93-a5a2-cd19e006ed01 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | show(...) |
| Return Type Rule | runtime-defined by engine entrypoint show |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_show.v3 |
| AST Binding | ast.expr.scalar_show |
| Engine Entrypoint | show |
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
select show(arg_1) from app.sample_values;
```

### `show_trgm`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.show_trgm |
| UUID | 019dffbb-f000-782d-9f6f-782288b11717 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | show_trgm(...) |
| Return Type Rule | runtime-defined by engine entrypoint show_trgm |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_show_trgm.v3 |
| AST Binding | ast.expr.scalar_show_trgm |
| Engine Entrypoint | show_trgm |
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
select show_trgm(arg_1) from app.sample_values;
```

### `sign`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sign |
| UUID | 019dffbb-f001-700c-8a00-00000000000c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sign(...) |
| Return Type Rule | descriptor-authoritative sign numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_sign.v3 |
| AST Binding | ast.expr.scalar_sign |
| Engine Entrypoint | scalar_sign |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select sign(arg_1) from app.sample_values;
```

### `signal`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.signal |
| UUID | 019dffbb-f000-7fc6-9cd9-9f09c27cb8c2 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | signal |
| Return Type Rule | procedural diagnostic route that emits SB_DIAG_PROCEDURAL_SIGNAL; descriptor=character |
| Coercion Rule | no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted |
| Null Behavior | context-backed typed null or diagnostic behavior follows SBSFC-016 row evidence |
| Collation/Charset Rule | character descriptors use byte-stable ASCII fixture values unless context supplies text |
| Timezone Rule | not applicable unless the context value is timezone-derived |
| Volatility | volatile_diagnostic |
| Determinism | not foldable for execution-context reads or procedural diagnostics |
| Side Effects | procedural_diagnostic |
| SBLR Binding | sblr.expr.procedural_signal.v3 |
| AST Binding | ast.expr.procedural_signal |
| Engine Entrypoint | signal |
| Security Policy | emits procedural diagnostic only; no storage or transaction finality authority |
| Error Semantics | arity/type/context/procedural diagnostics use builtin error compatibility matrix and SBSFC-016 evidence; diagnostic=SB_DIAG_PROCEDURAL_SIGNAL |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select signal() from app.sample_values;
```

### `similar`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.similar |
| UUID | 019dffbb-f000-7207-8986-f4b5818bdd26 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | similar(...) |
| Return Type Rule | runtime-defined by engine entrypoint similar |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_similar.v3 |
| AST Binding | ast.expr.scalar_similar |
| Engine Entrypoint | similar |
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
select similar(arg_1) from app.sample_values;
```

### `similar_to_escape`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.similar_to_escape |
| UUID | 019dffbb-f000-707c-b2fd-720fea5eb1e3 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | similar_to_escape(...) |
| Return Type Rule | runtime-defined by engine entrypoint similar_to_escape |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_similar_to_escape.v3 |
| AST Binding | ast.expr.scalar_similar_to_escape |
| Engine Entrypoint | similar_to_escape |
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
select similar_to_escape(arg_1) from app.sample_values;
```

### `similarity`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.similarity |
| UUID | 019dffbb-f000-7b87-ae5a-905420654cd9 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | similarity(...) |
| Return Type Rule | descriptor-authoritative similarity fuzzy/phonetic scalar result as implemented by the SBLR expression runtime; fixture result family real64 |
| Coercion Rule | descriptor implicit cast matrix for text and bounded integer arguments |
| Null Behavior | strict unless SBSFC-014 fuzzy/phonetic row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for text normalization where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and locale/regex versions |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_trigram_similarity.v3 |
| AST Binding | ast.expr.scalar_trigram_similarity |
| Engine Entrypoint | sb_engine_functions.similarity |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select similarity(arg_1) from app.sample_values;
```

### `sin`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sin |
| UUID | 019dffbb-f001-7001-8a00-000000000001 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sin(...) |
| Return Type Rule | descriptor-authoritative sin numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_sin.v3 |
| AST Binding | ast.expr.scalar_sin |
| Engine Entrypoint | scalar_sin |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select sin(arg_1) from app.sample_values;
```

### `sind`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sind |
| UUID | 019dffbb-f001-7033-8a00-000000000033 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sind(...) |
| Return Type Rule | descriptor-authoritative sind numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_sind.v3 |
| AST Binding | ast.expr.scalar_sind |
| Engine Entrypoint | scalar_sind |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select sind(arg_1) from app.sample_values;
```

### `sinh`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sinh |
| UUID | 019dffbb-f001-704b-8a00-00000000004b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sinh(...) |
| Return Type Rule | descriptor-authoritative sinh numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_sinh.v3 |
| AST Binding | ast.expr.scalar_sinh |
| Engine Entrypoint | scalar_sinh |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select sinh(arg_1) from app.sample_values;
```

### `sortop`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sortop |
| UUID | 019dffbb-f001-7435-8a00-000000000435 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sortop(...) |
| Return Type Rule | runtime-defined by engine entrypoint sortop |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_sortop.v3 |
| AST Binding | ast.expr.scalar_sortop |
| Engine Entrypoint | sortop |
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
select sortop(arg_1) from app.sample_values;
```

### `soundex`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.soundex |
| UUID | 019dffbb-f000-7769-a73d-d443e47e6a3a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | soundex(...) |
| Return Type Rule | descriptor-authoritative soundex fuzzy/phonetic scalar result as implemented by the SBLR expression runtime; fixture result family character |
| Coercion Rule | descriptor implicit cast matrix for text and bounded integer arguments |
| Null Behavior | strict unless SBSFC-014 fuzzy/phonetic row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for text normalization where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and locale/regex versions |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_soundex.v3 |
| AST Binding | ast.expr.scalar_soundex |
| Engine Entrypoint | sb_engine_functions.soundex |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select soundex(arg_1) from app.sample_values;
```

### `split_part`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.split_part |
| UUID | 019dffbb-f000-78b1-80a8-00f5fb985a6c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | split_part(text,delimiter,field_index) |
| Return Type Rule | character field selected by one-based index; out-of-range fields return an empty character value |
| Coercion Rule | text and delimiter use scalar text representation; field index must be int64-compatible |
| Null Behavior | NULL input text, delimiter, or index returns NULL character |
| Collation/Charset Rule | delimiter matching uses byte-stable character comparison for the supplied descriptor text |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_split_part.v3 |
| AST Binding | ast.expr.scalar_split_part |
| Engine Entrypoint | split_part |
| Security Policy | pure string helper; no catalog, storage, security, transaction, donor, plugin, or cluster authority |
| Error Semantics | invalid arity, non-text delimiter, or non-positive field index refuses with SB_DIAG_FUNCTION_INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select split_part(text_value_1, arg_2, arg_3) from app.sample_values;
```

### `sql_variant`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sql_variant |
| UUID | 019dffbb-f001-7407-8a00-000000000407 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sql_variant(...) |
| Return Type Rule | runtime-defined by engine entrypoint sql_variant |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_sql_variant.v3 |
| AST Binding | ast.expr.scalar_sql_variant |
| Engine Entrypoint | sql_variant |
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
select sql_variant(arg_1) from app.sample_values;
```

### `sqlcode`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sqlcode |
| UUID | 019dffbb-f000-74fb-9258-28914b0b9e2a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sqlcode(...) |
| Return Type Rule | runtime-defined by engine entrypoint sqlcode |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_sqlcode.v3 |
| AST Binding | ast.expr.scalar_sqlcode |
| Engine Entrypoint | sqlcode |
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
select sqlcode(arg_1) from app.sample_values;
```

### `sqlerrm`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sqlerrm |
| UUID | 019dffbb-f000-77d0-a92d-2bc57bb6de07 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sqlerrm(...) |
| Return Type Rule | runtime-defined by engine entrypoint sqlerrm |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_sqlerrm.v3 |
| AST Binding | ast.expr.scalar_sqlerrm |
| Engine Entrypoint | sqlerrm |
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
select sqlerrm(arg_1) from app.sample_values;
```

### `sqlstate`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sqlstate |
| UUID | 019dffbb-f000-7371-83f6-59e784781544 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sqlstate(...) |
| Return Type Rule | runtime-defined by engine entrypoint sqlstate |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_sqlstate.v3 |
| AST Binding | ast.expr.scalar_sqlstate |
| Engine Entrypoint | sqlstate |
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
select sqlstate(arg_1) from app.sample_values;
```

### `sqrt`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.sqrt |
| UUID | 019de5fc-2400-7e96-afc9-a9423408a720 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sqrt(number) |
| Return Type Rule | square root; domain errors profile-gated |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_sqrt.v3 |
| AST Binding | ast.expr.scalar_sqrt |
| Engine Entrypoint | scalar_sqrt |
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
select sqrt(numeric_value_1) from app.sample_values;
```

### `stable`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.stable |
| UUID | 019dffbb-f000-7004-9815-eac44685f4fb |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | stable(...) |
| Return Type Rule | runtime-defined by engine entrypoint stable |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_stable.v3 |
| AST Binding | ast.expr.scalar_stable |
| Engine Entrypoint | stable |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select stable(arg_1) from app.sample_values;
```

### `statement`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.statement |
| UUID | 019dffbb-f001-7452-8a00-000000000452 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | statement(...) |
| Return Type Rule | runtime-defined by engine entrypoint statement |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_statement.v3 |
| AST Binding | ast.expr.scalar_statement |
| Engine Entrypoint | statement |
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
select statement(arg_1) from app.sample_values;
```

### `statement_max_length_bytes`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.statement_max_length_bytes |
| UUID | 019dffbb-f000-7df3-ab38-5b59121ab317 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | statement_max_length_bytes(...) |
| Return Type Rule | runtime-defined by engine entrypoint statement_max_length_bytes |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_statement_max_length_bytes.v3 |
| AST Binding | ast.expr.scalar_statement_max_length_bytes |
| Engine Entrypoint | statement_max_length_bytes |
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
select statement_max_length_bytes(arg_1) from app.sample_values;
```

### `statement_terminator`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.statement_terminator |
| UUID | 019dffbb-f000-7eb6-adab-4674eff40e42 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | statement_terminator(...) |
| Return Type Rule | runtime-defined by engine entrypoint statement_terminator |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_statement_terminator.v3 |
| AST Binding | ast.expr.scalar_statement_terminator |
| Engine Entrypoint | statement_terminator |
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
select statement_terminator(arg_1) from app.sample_values;
```

### `statement_timeout`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.statement_timeout |
| UUID | 019dffbb-f000-7144-b05f-63599d6904aa |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | statement_timeout() |
| Return Type Rule | fixed public statement-timeout policy value; descriptor=uint64 |
| Coercion Rule | no input coercion |
| Null Behavior | never null |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | stable_per_session_policy |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.statement_timeout.v3 |
| AST Binding | ast.expr.statement_timeout |
| Engine Entrypoint | statement_timeout |
| Security Policy | exposes fixed standalone timeout policy, no mutable setting catalog read |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select statement_timeout() from app.sample_values;
```

### `statement_timeout_default`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.statement_timeout_default |
| UUID | 019dffbb-f000-79aa-8450-37b7d52453ad |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | statement_timeout_default() |
| Return Type Rule | fixed default statement-timeout policy value; descriptor=uint64 |
| Coercion Rule | no input coercion |
| Null Behavior | never null |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | stable_per_session_policy |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.statement_timeout_default.v3 |
| AST Binding | ast.expr.statement_timeout_default |
| Engine Entrypoint | statement_timeout_default |
| Security Policy | exposes fixed standalone timeout policy, no mutable setting catalog read |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select statement_timeout_default() from app.sample_values;
```

### `statement_timeout_ms`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.statement_timeout_ms |
| UUID | 019dffbb-f000-7f23-bbe8-bd396318d8f5 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | statement_timeout_ms(...) |
| Return Type Rule | runtime-defined by engine entrypoint statement_timeout_ms |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_statement_timeout_ms.v3 |
| AST Binding | ast.expr.scalar_statement_timeout_ms |
| Engine Entrypoint | statement_timeout_ms |
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
select statement_timeout_ms(arg_1) from app.sample_values;
```

### `stmt_null`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.stmt_null |
| UUID | 019dffbb-f000-7017-803d-3ffa636db7c6 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | stmt_null(...) |
| Return Type Rule | runtime-defined by engine entrypoint stmt_null |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_stmt_null.v3 |
| AST Binding | ast.expr.scalar_stmt_null |
| Engine Entrypoint | stmt_null |
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
select stmt_null(arg_1) from app.sample_values;
```

### `string_to_array`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.string_to_array |
| UUID | 019dffbb-f000-75ad-ba8f-6a18b2388ac9 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | string_to_array(text,delimiter[,null_string]) |
| Return Type Rule | array descriptor containing a deterministic JSON text array payload |
| Coercion Rule | text, delimiter, and null-string marker use scalar text representation through descriptor implicit casts |
| Null Behavior | NULL input text returns NULL array; matching null_string elements are emitted as JSON nulls |
| Collation/Charset Rule | delimiter and null-string matching use byte-stable character comparison for supplied descriptor text |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_string_to_array.v3 |
| AST Binding | ast.expr.scalar_string_to_array |
| Engine Entrypoint | string_to_array |
| Security Policy | pure string helper; no catalog, storage, security, transaction, donor, plugin, or cluster authority |
| Error Semantics | invalid arity or unsupported descriptor conversion refuses with SB_DIAG_FUNCTION_INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select string_to_array(text_value_1, text_value_2) from app.sample_values;
```

### `string_truncation_behavior`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.string_truncation_behavior |
| UUID | 019dffbb-f000-7947-9f3f-e7caf34a05ee |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | string_truncation_behavior(...) |
| Return Type Rule | runtime-defined by engine entrypoint string_truncation_behavior |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_string_truncation_behavior.v3 |
| AST Binding | ast.expr.scalar_string_truncation_behavior |
| Engine Entrypoint | string_truncation_behavior |
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
select string_truncation_behavior(arg_1) from app.sample_values;
```

### `strpos`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.strpos |
| UUID | 019dffbb-f001-7043-8a00-000000000043 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | strpos(...) |
| Return Type Rule | descriptor-authoritative strpos text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_strpos.v3 |
| AST Binding | ast.expr.scalar_strpos |
| Engine Entrypoint | scalar_strpos |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select strpos(arg_1) from app.sample_values;
```

### `stuff`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.stuff |
| UUID | 019dffbb-f001-7013-8a00-00000000c013 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | stuff(...) |
| Return Type Rule | descriptor-authoritative stuff result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for native function/operator operands |
| Null Behavior | function/operator-specific NULL handling follows SBSFC-026 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | stable_by_arguments |
| Determinism | foldable only when inputs are constants with stable descriptors and function-specific context is fixed |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_stuff.v3 |
| AST Binding | ast.expr.scalar_stuff |
| Engine Entrypoint | scalar_stuff |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-026 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select stuff(arg_1) from app.sample_values;
```

### `substr`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.substr |
| UUID | 019dffbb-f001-7027-8a00-000000000027 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | substr(...) |
| Return Type Rule | descriptor-authoritative substr text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_substr.v3 |
| AST Binding | ast.expr.scalar_substr |
| Engine Entrypoint | scalar_substr |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select substr(arg_1) from app.sample_values;
```

### `substring`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.substring |
| UUID | 019de5fc-2400-7801-8969-95f395b818b6 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | substring(string,start[,length]) |
| Return Type Rule | text/string slice by character semantics unless donor profile selects byte semantics |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_substring.v3 |
| AST Binding | ast.expr.scalar_substring |
| Engine Entrypoint | scalar_substring |
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
select substring(text_value_1, arg_2) from app.sample_values;
```

### `syntax_parser_only`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.syntax_parser_only |
| UUID | 019dffbb-f001-7446-8a00-000000000446 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | syntax_parser_only(...) |
| Return Type Rule | runtime-defined by engine entrypoint syntax_parser_only |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_syntax_parser_only.v3 |
| AST Binding | ast.expr.scalar_syntax_parser_only |
| Engine Entrypoint | syntax_parser_only |
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
select syntax_parser_only(arg_1) from app.sample_values;
```

### `syntax_unsupported`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.syntax_unsupported |
| UUID | 019dffbb-f001-744f-8a00-00000000044f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | syntax_unsupported(...) |
| Return Type Rule | runtime-defined by engine entrypoint syntax_unsupported |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_syntax_unsupported.v3 |
| AST Binding | ast.expr.scalar_syntax_unsupported |
| Engine Entrypoint | syntax_unsupported |
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
select syntax_unsupported(arg_1) from app.sample_values;
```

### `system_user`

| Property | Value |
| --- | --- |
| Builtin ID | sb.session.system_user |
| UUID | 019dffbb-f000-7a07-b81c-d252fe37a519 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | system_user |
| Return Type Rule | system user UUID from SBLR execution context; descriptor=uuid |
| Coercion Rule | no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted |
| Null Behavior | context-backed typed null or diagnostic behavior follows SBSFC-016 row evidence |
| Collation/Charset Rule | character descriptors use byte-stable ASCII fixture values unless context supplies text |
| Timezone Rule | not applicable unless the context value is timezone-derived |
| Volatility | stable_per_session |
| Determinism | not foldable for execution-context reads or procedural diagnostics |
| Side Effects | none |
| SBLR Binding | sblr.expr.system_user.v3 |
| AST Binding | ast.expr.system_user |
| Engine Entrypoint | system_user |
| Security Policy | reads system user UUID context only |
| Error Semantics | arity/type/context/procedural diagnostics use builtin error compatibility matrix and SBSFC-016 evidence; diagnostic=SBSQL.FUNCTION.INVALID_INPUT for arity/type/context violations where applicable |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select system_user() from app.sample_values;
```

### `t`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.t |
| UUID | 019dffbb-f001-7456-8a00-000000000456 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | t(...) |
| Return Type Rule | runtime-defined by engine entrypoint t |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_t.v3 |
| AST Binding | ast.expr.scalar_t |
| Engine Entrypoint | t |
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
select t(arg_1) from app.sample_values;
```

### `table`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.table |
| UUID | 019dffbb-f001-7453-8a00-000000000453 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | table(...) |
| Return Type Rule | runtime-defined by engine entrypoint table |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_table.v3 |
| AST Binding | ast.expr.scalar_table |
| Engine Entrypoint | table |
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
select table(arg_1) from app.sample_values;
```

### `table_size`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.table_size |
| UUID | 019dffbb-f000-7732-b768-741b96827ed2 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | table_size(); table_size(table_uuid[,include_indexes]) |
| Return Type Rule | uint64 stable byte estimate derived from MGA relation metadata, row versions, and optional index sidecars |
| Coercion Rule | table_uuid argument uses UUID text representation; include_indexes uses boolean-compatible scalar text/value |
| Null Behavior | SQL NULL table_uuid or include_indexes returns SQL NULL |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | stable_per_transaction |
| Determinism | stable within current MGA transaction visibility context and persisted sidecar payloads |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_table_size.v3 |
| AST Binding | ast.expr.scalar_table_size |
| Engine Entrypoint | table_size |
| Security Policy | reads engine-owned local MGA relation metadata, row-version, and index sidecars; no parser SQL, donor backend, cluster path, WAL, or SQLite finality shortcut |
| Error Semantics | invalid arity, invalid include_indexes value, missing database context, or MGA catalog load failure refuses with SB_DIAG_FUNCTION_INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | catalog_metadata_io |

#### Practical Form

```sql
select table_size() from app.sample_values;
```

### `tablegroup`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.tablegroup |
| UUID | 019dffbb-f001-7420-8a00-000000000420 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | tablegroup(...) |
| Return Type Rule | runtime-defined by engine entrypoint tablegroup |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_tablegroup.v3 |
| AST Binding | ast.expr.scalar_tablegroup |
| Engine Entrypoint | tablegroup |
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
select tablegroup(arg_1) from app.sample_values;
```

### `tabular`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.tabular |
| UUID | 019dffbb-f000-799e-a479-4ba12ceba4a4 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | tabular(...) |
| Return Type Rule | runtime-defined by engine entrypoint tabular |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_tabular.v3 |
| AST Binding | ast.expr.scalar_tabular |
| Engine Entrypoint | tabular |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select tabular(arg_1) from app.sample_values;
```

### `tan`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.tan |
| UUID | 019dffbb-f001-7003-8a00-000000000003 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | tan(...) |
| Return Type Rule | descriptor-authoritative tan numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_tan.v3 |
| AST Binding | ast.expr.scalar_tan |
| Engine Entrypoint | scalar_tan |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select tan(arg_1) from app.sample_values;
```

### `tand`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.tand |
| UUID | 019dffbb-f001-7035-8a00-000000000035 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | tand(...) |
| Return Type Rule | descriptor-authoritative tand numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_tand.v3 |
| AST Binding | ast.expr.scalar_tand |
| Engine Entrypoint | scalar_tand |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select tand(arg_1) from app.sample_values;
```

### `tanh`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.tanh |
| UUID | 019dffbb-f001-702d-8a00-00000000002d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | tanh(...) |
| Return Type Rule | descriptor-authoritative tanh numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_tanh.v3 |
| AST Binding | ast.expr.scalar_tanh |
| Engine Entrypoint | scalar_tanh |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select tanh(arg_1) from app.sample_values;
```

### `temp_buffer_size_default`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.temp_buffer_size_default |
| UUID | 019dffbb-f000-7fea-9e88-f1a6e939362b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | temp_buffer_size_default(...) |
| Return Type Rule | runtime-defined by engine entrypoint temp_buffer_size_default |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_temp_buffer_size_default.v3 |
| AST Binding | ast.expr.scalar_temp_buffer_size_default |
| Engine Entrypoint | temp_buffer_size_default |
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
select temp_buffer_size_default(arg_1) from app.sample_values;
```

### `temp_buffers`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.temp_buffers |
| UUID | 019dffbb-f000-701b-ae8e-e1c771393007 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | temp_buffers(...) |
| Return Type Rule | runtime-defined by engine entrypoint temp_buffers |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_temp_buffers.v3 |
| AST Binding | ast.expr.scalar_temp_buffers |
| Engine Entrypoint | temp_buffers |
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
select temp_buffers(arg_1) from app.sample_values;
```

### `temporal_default_precision`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.temporal_default_precision |
| UUID | 019dffbb-f000-7f39-a729-540c271b5228 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | temporal_default_precision(...) |
| Return Type Rule | runtime-defined by engine entrypoint temporal_default_precision |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_temporal_default_precision.v3 |
| AST Binding | ast.expr.scalar_temporal_default_precision |
| Engine Entrypoint | temporal_default_precision |
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
select temporal_default_precision(arg_1) from app.sample_values;
```

### `then`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.then |
| UUID | 019dffbb-f000-7c12-81b0-f6ad7033a124 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | then(...) |
| Return Type Rule | runtime-defined by engine entrypoint then |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_then.v3 |
| AST Binding | ast.expr.scalar_then |
| Engine Entrypoint | then |
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
select then(arg_1) from app.sample_values;
```

### `timezone_resolution`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.timezone_resolution |
| UUID | 019dffbb-f000-72f3-bc6f-241b86dfec0e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | timezone_resolution(...) |
| Return Type Rule | runtime-defined by engine entrypoint timezone_resolution |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_timezone_resolution.v3 |
| AST Binding | ast.expr.scalar_timezone_resolution |
| Engine Entrypoint | timezone_resolution |
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
select timezone_resolution(arg_1) from app.sample_values;
```

### `to_ascii`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.to_ascii |
| UUID | 019dffbb-f000-7752-a0b6-2a0a9ccbf053 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | to_ascii(...) |
| Return Type Rule | descriptor-authoritative to_ascii result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for native function/operator operands |
| Null Behavior | function/operator-specific NULL handling follows SBSFC-026 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | stable_by_arguments |
| Determinism | foldable only when inputs are constants with stable descriptors and function-specific context is fixed |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_to_ascii.v3 |
| AST Binding | ast.expr.scalar_to_ascii |
| Engine Entrypoint | to_ascii |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-026 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select to_ascii(arg_1) from app.sample_values;
```

### `to_bigint`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.to_bigint |
| UUID | 019dffbb-f000-7559-a463-1c32e48d9990 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | to_bigint(...) |
| Return Type Rule | runtime-defined by engine entrypoint to_bigint |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_to_bigint.v3 |
| AST Binding | ast.expr.scalar_to_bigint |
| Engine Entrypoint | to_bigint |
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
select to_bigint(arg_1) from app.sample_values;
```

### `to_boolean`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.to_boolean |
| UUID | 019dffbb-f000-7e33-96dc-12df35c0eecb |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | to_boolean(...) |
| Return Type Rule | runtime-defined by engine entrypoint to_boolean |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_to_boolean.v3 |
| AST Binding | ast.expr.scalar_to_boolean |
| Engine Entrypoint | to_boolean |
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
select to_boolean(arg_1) from app.sample_values;
```

### `to_bytes`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.to_bytes |
| UUID | 019dffbb-f000-78d6-aebd-d0a523429bde |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | to_bytes(...) |
| Return Type Rule | runtime-defined by engine entrypoint to_bytes |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_to_bytes.v3 |
| AST Binding | ast.expr.scalar_to_bytes |
| Engine Entrypoint | to_bytes |
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
select to_bytes(arg_1) from app.sample_values;
```

### `to_char`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.to_char |
| UUID | 019dffbb-f000-75de-bbff-7b0d19b47c7a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | to_char(...) |
| Return Type Rule | runtime-defined by engine entrypoint to_char |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_to_char.v3 |
| AST Binding | ast.expr.scalar_to_char |
| Engine Entrypoint | to_char |
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
select to_char(arg_1) from app.sample_values;
```

### `to_date`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.to_date |
| UUID | 019dffbb-f000-7c50-a67a-7f774aa6317d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | to_date(...) |
| Return Type Rule | runtime-defined by engine entrypoint to_date |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_to_date.v3 |
| AST Binding | ast.expr.scalar_to_date |
| Engine Entrypoint | to_date |
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
select to_date(arg_1) from app.sample_values;
```

### `to_double`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.to_double |
| UUID | 019dffbb-f000-766a-afc2-d327c5acf21a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | to_double(...) |
| Return Type Rule | runtime-defined by engine entrypoint to_double |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_to_double.v3 |
| AST Binding | ast.expr.scalar_to_double |
| Engine Entrypoint | to_double |
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
select to_double(arg_1) from app.sample_values;
```

### `to_hex`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.to_hex |
| UUID | 019dffbb-f001-7044-8a00-000000000044 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | to_hex(...) |
| Return Type Rule | descriptor-authoritative to_hex encoding/binary conversion result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary encoding inputs |
| Null Behavior | strict unless SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_to_hex.v3 |
| AST Binding | ast.expr.scalar_to_hex |
| Engine Entrypoint | scalar_to_hex |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/format errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select to_hex(arg_1) from app.sample_values;
```

### `to_integer`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.to_integer |
| UUID | 019dffbb-f000-7d16-979e-ce962673fea6 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | to_integer(...) |
| Return Type Rule | runtime-defined by engine entrypoint to_integer |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_to_integer.v3 |
| AST Binding | ast.expr.scalar_to_integer |
| Engine Entrypoint | to_integer |
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
select to_integer(arg_1) from app.sample_values;
```

### `to_number`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.to_number |
| UUID | 019dffbb-f000-7aa2-9965-9094f01f31bd |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | to_number(...) |
| Return Type Rule | runtime-defined by engine entrypoint to_number |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_to_number.v3 |
| AST Binding | ast.expr.scalar_to_number |
| Engine Entrypoint | to_number |
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
select to_number(arg_1) from app.sample_values;
```

### `to_numeric`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.to_numeric |
| UUID | 019dffbb-f000-7be5-b99b-c1757e6541c8 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | to_numeric(...) |
| Return Type Rule | runtime-defined by engine entrypoint to_numeric |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_to_numeric.v3 |
| AST Binding | ast.expr.scalar_to_numeric |
| Engine Entrypoint | to_numeric |
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
select to_numeric(arg_1) from app.sample_values;
```

### `to_real`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.to_real |
| UUID | 019dffbb-f000-7e70-9853-2b849a36b908 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | to_real(...) |
| Return Type Rule | runtime-defined by engine entrypoint to_real |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_to_real.v3 |
| AST Binding | ast.expr.scalar_to_real |
| Engine Entrypoint | to_real |
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
select to_real(arg_1) from app.sample_values;
```

### `to_timestamp`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.to_timestamp |
| UUID | 019dffbb-f000-7721-b3b5-190ce1e21d6c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | to_timestamp(...) |
| Return Type Rule | runtime-defined by engine entrypoint to_timestamp |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_to_timestamp.v3 |
| AST Binding | ast.expr.scalar_to_timestamp |
| Engine Entrypoint | to_timestamp |
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
select to_timestamp(arg_1) from app.sample_values;
```

### `transaction`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.transaction |
| UUID | 019dffbb-f001-7449-8a00-000000000449 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | transaction(...) |
| Return Type Rule | runtime-defined by engine entrypoint transaction |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_transaction.v3 |
| AST Binding | ast.expr.scalar_transaction |
| Engine Entrypoint | transaction |
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
select transaction(arg_1) from app.sample_values;
```

### `transaction_timeout`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.transaction_timeout |
| UUID | 019dffbb-f000-771e-94d5-bede9abcd06c |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | transaction_timeout() |
| Return Type Rule | fixed public transaction-timeout policy value; descriptor=uint64 |
| Coercion Rule | no input coercion |
| Null Behavior | never null |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | stable_per_session_policy |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.transaction_timeout.v3 |
| AST Binding | ast.expr.transaction_timeout |
| Engine Entrypoint | transaction_timeout |
| Security Policy | exposes fixed standalone timeout policy, no mutable setting catalog read |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select transaction_timeout() from app.sample_values;
```

### `transaction_timeout_default`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.transaction_timeout_default |
| UUID | 019dffbb-f000-7a14-9aa4-478036ca184a |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | transaction_timeout_default() |
| Return Type Rule | fixed default transaction-timeout policy value; descriptor=uint64 |
| Coercion Rule | no input coercion |
| Null Behavior | never null |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | stable_per_session_policy |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.transaction_timeout_default.v3 |
| AST Binding | ast.expr.transaction_timeout_default |
| Engine Entrypoint | transaction_timeout_default |
| Security Policy | exposes fixed standalone timeout policy, no mutable setting catalog read |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select transaction_timeout_default() from app.sample_values;
```

### `transaction_uuid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.session.transaction_uuid |
| UUID | 019dffbb-f000-740e-a1df-d98d529dd441 |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | transaction_uuid |
| Return Type Rule | transaction UUID from SBLR transaction context; descriptor=uuid |
| Coercion Rule | no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted |
| Null Behavior | context-backed typed null or diagnostic behavior follows SBSFC-016 row evidence |
| Collation/Charset Rule | character descriptors use byte-stable ASCII fixture values unless context supplies text |
| Timezone Rule | not applicable unless the context value is timezone-derived |
| Volatility | stable_per_transaction |
| Determinism | not foldable for execution-context reads or procedural diagnostics |
| Side Effects | none |
| SBLR Binding | sblr.expr.transaction_uuid.v3 |
| AST Binding | ast.expr.transaction_uuid |
| Engine Entrypoint | transaction_uuid |
| Security Policy | reads active transaction UUID context only; no parser-side finality |
| Error Semantics | arity/type/context/procedural diagnostics use builtin error compatibility matrix and SBSFC-016 evidence; diagnostic=SBSQL.FUNCTION.INVALID_INPUT for arity/type/context violations where applicable |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select transaction_uuid() from app.sample_values;
```

### `translate`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.translate |
| UUID | 019dffbb-f000-7df7-9350-5a82e16abe1e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | translate(...) |
| Return Type Rule | descriptor-authoritative translate text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_translate.v3 |
| AST Binding | ast.expr.scalar_translate |
| Engine Entrypoint | scalar_translate |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select translate(arg_1) from app.sample_values;
```

### `treat`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.treat |
| UUID | 019dffbb-f000-79b2-9194-525d7a13ebc0 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | treat(...) |
| Return Type Rule | runtime-defined by engine entrypoint treat |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_treat.v3 |
| AST Binding | ast.expr.scalar_treat |
| Engine Entrypoint | treat |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select treat(arg_1) from app.sample_values;
```

### `treat_typed`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.treat_typed |
| UUID | 019dffbb-f000-7c7a-b60b-1d00033b9430 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | treat_typed(...) |
| Return Type Rule | runtime-defined by engine entrypoint treat_typed |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_treat_typed.v3 |
| AST Binding | ast.expr.scalar_treat_typed |
| Engine Entrypoint | treat_typed |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select treat_typed(arg_1) from app.sample_values;
```

### `trim`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.trim |
| UUID | 019de5fc-2400-7c55-9a17-3c442a28f018 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | trim([spec chars from] string) |
| Return Type Rule | trim codepoint/grapheme behavior profile-gated |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_trim.v3 |
| AST Binding | ast.expr.scalar_trim |
| Engine Entrypoint | scalar_trim |
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
select trim(text_value_1) from app.sample_values;
```

### `trunc`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.trunc |
| UUID | 019dffbb-f001-700a-8a00-00000000000a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | trunc(...) |
| Return Type Rule | descriptor-authoritative trunc numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_trunc.v3 |
| AST Binding | ast.expr.scalar_trunc |
| Engine Entrypoint | scalar_trunc |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select trunc(arg_1) from app.sample_values;
```

### `truncate`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.truncate |
| UUID | 019dffbb-f001-701c-8a00-00000000001c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | truncate(...) |
| Return Type Rule | descriptor-authoritative truncate numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_truncate.v3 |
| AST Binding | ast.expr.scalar_truncate |
| Engine Entrypoint | scalar_truncate |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select truncate(arg_1) from app.sample_values;
```

### `try_cast`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.try_cast |
| UUID | 019dffbb-f000-7de1-850f-3699becc27a4 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | try_cast(...) |
| Return Type Rule | runtime-defined by engine entrypoint try_cast |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_try_cast.v3 |
| AST Binding | ast.expr.scalar_try_cast |
| Engine Entrypoint | try_cast |
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
select try_cast(arg_1) from app.sample_values;
```

### `tsquery`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.tsquery |
| UUID | 019dffbb-f001-742f-8a00-00000000042f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | tsquery(...) |
| Return Type Rule | runtime-defined by engine entrypoint tsquery |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_tsquery.v3 |
| AST Binding | ast.expr.scalar_tsquery |
| Engine Entrypoint | tsquery |
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
select tsquery(arg_1) from app.sample_values;
```

### `tx_read_only`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.tx_read_only |
| UUID | 019dffbb-f000-7df4-bc18-eace57affb4c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | tx_read_only(...) |
| Return Type Rule | runtime-defined by engine entrypoint tx_read_only |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_tx_read_only.v3 |
| AST Binding | ast.expr.scalar_tx_read_only |
| Engine Entrypoint | tx_read_only |
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
select tx_read_only(arg_1) from app.sample_values;
```

### `txid_status`

| Property | Value |
| --- | --- |
| Builtin ID | sb.session.txid_status |
| UUID | 019dffbb-f000-7504-977b-ff2a195cd52e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | txid_status([transaction_id]) |
| Return Type Rule | current local transaction status label as character; returns in_progress for current local transaction, unknown for non-current ids, or NULL without transaction context |
| Coercion Rule | optional transaction id argument must be int64-compatible; no parser-side finality or donor transaction authority |
| Null Behavior | NULL argument returns NULL character; no current transaction context returns NULL for nullary call |
| Collation/Charset Rule | returns byte-stable ASCII status labels |
| Timezone Rule | not applicable |
| Volatility | stable_per_transaction |
| Determinism | not foldable; reads active transaction context |
| Side Effects | none |
| SBLR Binding | sblr.expr.txid_status.v3 |
| AST Binding | ast.expr.txid_status |
| Engine Entrypoint | txid_status |
| Security Policy | reads active local transaction id status only; no parser-side finality and no transaction state mutation |
| Error Semantics | arity/type/context diagnostics use canonical txid surface fixtures; status is context-backed and non-mutating |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select txid_status(arg_1) from app.sample_values;
```

### `txn`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.txn |
| UUID | 019dffbb-f001-7431-8a00-000000000431 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | txn(...) |
| Return Type Rule | runtime-defined by engine entrypoint txn |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_txn.v3 |
| AST Binding | ast.expr.scalar_txn |
| Engine Entrypoint | txn |
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
select txn(arg_1) from app.sample_values;
```

### `udr_admission_denied`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.udr_admission_denied |
| UUID | 019dffbb-f001-7451-8a00-000000000451 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | udr_admission_denied(...) |
| Return Type Rule | runtime-defined by engine entrypoint udr_admission_denied |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_udr_admission_denied.v3 |
| AST Binding | ast.expr.scalar_udr_admission_denied |
| Engine Entrypoint | udr_admission_denied |
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
select udr_admission_denied(arg_1) from app.sample_values;
```

### `unicode`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.unicode |
| UUID | 019dffbb-f000-7143-a924-df5440bf50f1 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | unicode(...) |
| Return Type Rule | descriptor-authoritative unicode text/binary scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for text/binary/character scalar inputs |
| Null Behavior | strict unless function-specific semantics or SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_unicode.v3 |
| AST Binding | ast.expr.scalar_unicode |
| Engine Entrypoint | scalar_unicode |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select unicode(arg_1) from app.sample_values;
```

### `unicode_normalize`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.unicode_normalize |
| UUID | 019dffbb-f000-79c1-bca2-85816d298a8f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | unicode_normalize(...) |
| Return Type Rule | runtime-defined by engine entrypoint unicode_normalize |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_unicode_normalize.v3 |
| AST Binding | ast.expr.scalar_unicode_normalize |
| Engine Entrypoint | unicode_normalize |
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
select unicode_normalize(arg_1) from app.sample_values;
```

### `unicode_root`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.unicode_root |
| UUID | 019dffbb-f000-77b8-8057-e31310e1db41 |
| Kind | scalar |
| Syntax Forms | function_call, keyword_or_function_call |
| Overloads | unicode_root(...) |
| Return Type Rule | descriptor-authoritative unicode_root language/profile metadata result as implemented by the SBLR expression runtime |
| Coercion Rule | no input coercion unless the fixture surface supplies descriptor-authoritative arguments |
| Null Behavior | not applicable for nullary metadata forms; strict for argument-bearing forms |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | stable_per_catalog_profile |
| Determinism | profile-backed; foldable after binding when the active catalog/profile is fixed |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_unicode_root.v3 |
| AST Binding | ast.expr.scalar_unicode_root |
| Engine Entrypoint | scalar_unicode_root |
| Security Policy | reads fixed language/catalog profile metadata only; no user data access |
| Error Semantics | arity/type errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select unicode_root(arg_1) from app.sample_values;
```

### `union_max_arms`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.union_max_arms |
| UUID | 019dffbb-f000-737c-b02e-175645feabab |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | union_max_arms() |
| Return Type Rule | fixed policy limit for UNION arms; descriptor=uint64 |
| Coercion Rule | no input coercion |
| Null Behavior | never null |
| Collation/Charset Rule | not applicable |
| Timezone Rule | not applicable |
| Volatility | immutable_policy |
| Determinism | foldable for fixed policy constants |
| Side Effects | none |
| SBLR Binding | sblr.expr.union_max_arms.v3 |
| AST Binding | ast.expr.union_max_arms |
| Engine Entrypoint | union_max_arms |
| Security Policy | fixed public parser/runtime policy, no catalog or storage authority |
| Error Semantics | arity must be 0 otherwise refuse with SBSQL.FUNCTION.INVALID_INPUT |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select union_max_arms() from app.sample_values;
```

### `unknown`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.unknown |
| UUID | 019dffbb-f001-7434-8a00-000000000434 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | unknown(...) |
| Return Type Rule | runtime-defined by engine entrypoint unknown |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_unknown.v3 |
| AST Binding | ast.expr.scalar_unknown |
| Engine Entrypoint | unknown |
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
select unknown(arg_1) from app.sample_values;
```

### `unquoted_identifier_case_rule`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.unquoted_identifier_case_rule |
| UUID | 019dffbb-f000-75c8-8f4a-c71a77d00b82 |
| Kind | scalar |
| Syntax Forms | function_call, keyword_or_function_call |
| Overloads | unquoted_identifier_case_rule(...) |
| Return Type Rule | descriptor-authoritative unquoted_identifier_case_rule language/profile metadata result as implemented by the SBLR expression runtime |
| Coercion Rule | no input coercion unless the fixture surface supplies descriptor-authoritative arguments |
| Null Behavior | not applicable for nullary metadata forms; strict for argument-bearing forms |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | stable_per_catalog_profile |
| Determinism | profile-backed; foldable after binding when the active catalog/profile is fixed |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_unquoted_identifier_case_rule.v3 |
| AST Binding | ast.expr.scalar_unquoted_identifier_case_rule |
| Engine Entrypoint | scalar_unquoted_identifier_case_rule |
| Security Policy | reads fixed language/catalog profile metadata only; no user data access |
| Error Semantics | arity/type errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select unquoted_identifier_case_rule(arg_1) from app.sample_values;
```

### `unresolved`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.unresolved |
| UUID | 019dffbb-f001-741e-8a00-00000000041e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | unresolved(...) |
| Return Type Rule | runtime-defined by engine entrypoint unresolved |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_unresolved.v3 |
| AST Binding | ast.expr.scalar_unresolved |
| Engine Entrypoint | unresolved |
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
select unresolved(arg_1) from app.sample_values;
```

### `unsupported`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.unsupported |
| UUID | 019dffbb-f001-7425-8a00-000000000425 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | unsupported(...) |
| Return Type Rule | runtime-defined by engine entrypoint unsupported |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_unsupported.v3 |
| AST Binding | ast.expr.scalar_unsupported |
| Engine Entrypoint | unsupported |
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
select unsupported(arg_1) from app.sample_values;
```

### `unsupported_refused_by_design`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.unsupported_refused_by_design |
| UUID | 019dffbb-f001-743f-8a00-00000000043f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | unsupported_refused_by_design(...) |
| Return Type Rule | runtime-defined by engine entrypoint unsupported_refused_by_design |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_unsupported_refused_by_design.v3 |
| AST Binding | ast.expr.scalar_unsupported_refused_by_design |
| Engine Entrypoint | unsupported_refused_by_design |
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
select unsupported_refused_by_design(arg_1) from app.sample_values;
```

### `upper`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.upper |
| UUID | 019de5fc-2400-7f4f-a75a-97f18565ad84 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | upper(string) |
| Return Type Rule | locale/collation-aware uppercase transform |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_upper.v3 |
| AST Binding | ast.expr.scalar_upper |
| Engine Entrypoint | scalar_upper |
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
select upper(text_value_1) from app.sample_values;
```

### `upsert`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.upsert |
| UUID | 019dffbb-f000-71f0-a44b-7a9c1aabee8e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | upsert(...) |
| Return Type Rule | runtime-defined by engine entrypoint upsert |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_upsert.v3 |
| AST Binding | ast.expr.scalar_upsert |
| Engine Entrypoint | upsert |
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
select upsert(arg_1) from app.sample_values;
```

### `user`

| Property | Value |
| --- | --- |
| Builtin ID | sb.session.user |
| UUID | 019dffbb-f000-7ecd-8a01-7054cca0e52b |
| Kind | scalar |
| Syntax Forms | keyword_or_function_call |
| Overloads | user |
| Return Type Rule | current user UUID from SBLR execution context; descriptor=uuid |
| Coercion Rule | no coercion for nullary context reads; procedural/context arguments are descriptor-authoritative where accepted |
| Null Behavior | context-backed typed null or diagnostic behavior follows SBSFC-016 row evidence |
| Collation/Charset Rule | character descriptors use byte-stable ASCII fixture values unless context supplies text |
| Timezone Rule | not applicable unless the context value is timezone-derived |
| Volatility | stable_per_session |
| Determinism | not foldable for execution-context reads or procedural diagnostics |
| Side Effects | none |
| SBLR Binding | sblr.expr.user.v3 |
| AST Binding | ast.expr.user |
| Engine Entrypoint | user |
| Security Policy | reads user UUID context only |
| Error Semantics | arity/type/context/procedural diagnostics use builtin error compatibility matrix and SBSFC-016 evidence; diagnostic=SBSQL.FUNCTION.INVALID_INPUT for arity/type/context violations where applicable |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select user() from app.sample_values;
```

### `using`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.using |
| UUID | 019dffbb-f000-74ab-879f-893e753621c2 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | using(...) |
| Return Type Rule | runtime-defined by engine entrypoint using |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_using.v3 |
| AST Binding | ast.expr.scalar_using |
| Engine Entrypoint | using |
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
select using(arg_1) from app.sample_values;
```

### `uuid_from_string`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.uuid_from_string |
| UUID | 019dffbb-f000-741b-8ec0-33bc11cbd646 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | uuid_from_string(...) |
| Return Type Rule | descriptor-authoritative uuid_from_string UUID/text conversion result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for UUID/text conversion inputs |
| Null Behavior | strict unless SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_uuid_from_string.v3 |
| AST Binding | ast.expr.scalar_uuid_from_string |
| Engine Entrypoint | scalar_uuid_from_string |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/format errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select uuid_from_string(arg_1) from app.sample_values;
```

### `uuid_to_string`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.uuid_to_string |
| UUID | 019dffbb-f000-7a88-8da9-79da4f0ba7bd |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | uuid_to_string(...) |
| Return Type Rule | descriptor-authoritative uuid_to_string UUID/text conversion result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for UUID/text conversion inputs |
| Null Behavior | strict unless SBSFC-011 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for character semantics; not applicable for binary/UUID metadata forms |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_uuid_to_string.v3 |
| AST Binding | ast.expr.scalar_uuid_to_string |
| Engine Entrypoint | scalar_uuid_to_string |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/format errors use builtin error compatibility matrix and SBSFC-011 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select uuid_to_string(arg_1) from app.sample_values;
```

### `value`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.value |
| UUID | 019dffbb-f001-743a-8a00-00000000043a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | value(...) |
| Return Type Rule | runtime-defined by engine entrypoint value |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_value.v3 |
| AST Binding | ast.expr.scalar_value |
| Engine Entrypoint | value |
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
select value(arg_1) from app.sample_values;
```

### `value_state`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.value_state |
| UUID | 019dffbb-f000-7757-b91d-e2df358fea29 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | value_state(...) |
| Return Type Rule | runtime-defined by engine entrypoint value_state |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_value_state.v3 |
| AST Binding | ast.expr.scalar_value_state |
| Engine Entrypoint | value_state |
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
select value_state(arg_1) from app.sample_values;
```

### `view`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.view |
| UUID | 019dffbb-f000-7f76-8efc-c42e00ee5a34 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | view(...) |
| Return Type Rule | runtime-defined by engine entrypoint view |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_view.v3 |
| AST Binding | ast.expr.scalar_view |
| Engine Entrypoint | view |
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
select view(arg_1) from app.sample_values;
```

### `void`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.void |
| UUID | 019dffbb-f000-78a2-a28f-14198930cbdd |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | void(...) |
| Return Type Rule | runtime-defined by engine entrypoint void |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_void.v3 |
| AST Binding | ast.expr.scalar_void |
| Engine Entrypoint | void |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select void(arg_1) from app.sample_values;
```

### `volatile`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.volatile |
| UUID | 019dffbb-f000-7bce-be47-11ce203ad209 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | volatile(...) |
| Return Type Rule | runtime-defined by engine entrypoint volatile |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_volatile.v3 |
| AST Binding | ast.expr.scalar_volatile |
| Engine Entrypoint | volatile |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select volatile(arg_1) from app.sample_values;
```

### `wait`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.wait |
| UUID | 019dffbb-f000-7513-86ab-fe23f7d7cb03 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | wait(...) |
| Return Type Rule | runtime-defined by engine entrypoint wait |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_wait.v3 |
| AST Binding | ast.expr.scalar_wait |
| Engine Entrypoint | wait |
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
select wait(arg_1) from app.sample_values;
```

### `when`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.when |
| UUID | 019dffbb-f000-7b6f-a7af-f28c7afcd961 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | when(...) |
| Return Type Rule | runtime-defined by engine entrypoint when |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_when.v3 |
| AST Binding | ast.expr.scalar_when |
| Engine Entrypoint | when |
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
select when(arg_1) from app.sample_values;
```

### `width_bucket`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.width_bucket |
| UUID | 019dffbb-f000-75ed-8097-552db6faee8d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | width_bucket(...) |
| Return Type Rule | descriptor-authoritative width_bucket numeric scalar result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | strict unless function-specific semantics or SBSFC-010 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | not applicable for numeric scalar descriptors |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_width_bucket.v3 |
| AST Binding | ast.expr.scalar_width_bucket |
| Engine Entrypoint | scalar_width_bucket |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/overflow/domain errors use builtin error compatibility matrix and SBSFC-010 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select width_bucket(arg_1) from app.sample_values;
```

### `with`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.with |
| UUID | 019dffbb-f000-709d-9458-e6ff6ea9137f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | with(...) |
| Return Type Rule | runtime-defined by engine entrypoint with |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_with.v3 |
| AST Binding | ast.expr.scalar_with |
| Engine Entrypoint | with |
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
select with(arg_1) from app.sample_values;
```

### `word_similarity`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.word_similarity |
| UUID | 019dffbb-f000-707d-b7e3-88cbba113a97 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | word_similarity(...) |
| Return Type Rule | descriptor-authoritative word_similarity fuzzy/phonetic scalar result as implemented by the SBLR expression runtime; fixture result family real64 |
| Coercion Rule | descriptor implicit cast matrix for text and bounded integer arguments |
| Null Behavior | strict unless SBSFC-014 fuzzy/phonetic row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for text normalization where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and locale/regex versions |
| Side Effects | none |
| SBLR Binding | sblr.expr.scalar_word_trigram_similarity.v3 |
| AST Binding | ast.expr.scalar_word_trigram_similarity |
| Engine Entrypoint | sb_engine_functions.word_similarity |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select word_similarity(arg_1) from app.sample_values;
```

### `x`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.x |
| UUID | 019dffbb-f001-7455-8a00-000000000455 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | x(...) |
| Return Type Rule | runtime-defined by engine entrypoint x |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_x.v3 |
| AST Binding | ast.expr.scalar_x |
| Engine Entrypoint | x |
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
select x(arg_1) from app.sample_values;
```

### `array`

| Property | Value |
| --- | --- |
| Builtin ID | sb.special.array_constructor |
| UUID | 019de5fc-2400-711c-ba73-f33bc70d50ff |
| Kind | special_form |
| Syntax Forms | grammar_special_form |
| Overloads | ARRAY[expr,...] |
| Return Type Rule | array descriptor constructor |
| Coercion Rule | special-form-specific descriptor coercion; never stringly typed |
| Null Behavior | special-form-specific |
| Collation/Charset Rule | uses operand descriptors when strings are compared or transformed |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | derived_from_operands_or_form |
| Determinism | derived from lowered expression and dependencies |
| Side Effects | none |
| SBLR Binding | sblr.expr.special_array_constructor.v3 |
| AST Binding | ast.special.special_array_constructor |
| Engine Entrypoint | special_array_constructor |
| Security Policy | inherits containing expression unless form reads session/system state |
| Error Semantics | see special-form error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | derived |
| index_eligible | derived |
| generated_column_eligible | derived |
| cost_class | special_form |

#### Practical Form

```sql
select array(value_1) from app.sample_values;
```

### `between`

| Property | Value |
| --- | --- |
| Builtin ID | sb.special.between |
| UUID | 019de5fc-2400-74c6-b95b-03156ae43585 |
| Kind | special_form |
| Syntax Forms | grammar_special_form |
| Overloads | expr BETWEEN low AND high |
| Return Type Rule | comparison expansion with donor null semantics |
| Coercion Rule | special-form-specific descriptor coercion; never stringly typed |
| Null Behavior | special-form-specific |
| Collation/Charset Rule | uses operand descriptors when strings are compared or transformed |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | derived_from_operands_or_form |
| Determinism | derived from lowered expression and dependencies |
| Side Effects | none |
| SBLR Binding | sblr.expr.special_between.v3 |
| AST Binding | ast.special.special_between |
| Engine Entrypoint | special_between |
| Security Policy | inherits containing expression unless form reads session/system state |
| Error Semantics | see special-form error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | derived |
| index_eligible | derived |
| generated_column_eligible | derived |
| cost_class | special_form |

#### Practical Form

```sql
select between(value_1) from app.sample_values;
```

### `case`

| Property | Value |
| --- | --- |
| Builtin ID | sb.special.case |
| UUID | 019de5fc-2400-7a17-8724-4009e1cb3105 |
| Kind | special_form |
| Syntax Forms | grammar_special_form |
| Overloads | CASE WHEN ... THEN ... ELSE ... END |
| Return Type Rule | conditional special form with branch descriptor unification |
| Coercion Rule | special-form-specific descriptor coercion; never stringly typed |
| Null Behavior | special-form-specific |
| Collation/Charset Rule | uses operand descriptors when strings are compared or transformed |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | derived_from_operands_or_form |
| Determinism | derived from lowered expression and dependencies |
| Side Effects | none |
| SBLR Binding | sblr.expr.special_case.v3 |
| AST Binding | ast.special.special_case |
| Engine Entrypoint | special_case |
| Security Policy | inherits containing expression unless form reads session/system state |
| Error Semantics | see special-form error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | derived |
| index_eligible | derived |
| generated_column_eligible | derived |
| cost_class | special_form |

#### Practical Form

```sql
select case(value_1) from app.sample_values;
```

### `cast`

| Property | Value |
| --- | --- |
| Builtin ID | sb.special.cast |
| UUID | 019de5fc-2400-7506-980f-55eff24901d1 |
| Kind | special_form |
| Syntax Forms | grammar_special_form |
| Overloads | CAST(expr AS type) |
| Return Type Rule | descriptor-authoritative cast special form |
| Coercion Rule | special-form-specific descriptor coercion; never stringly typed |
| Null Behavior | special-form-specific |
| Collation/Charset Rule | uses operand descriptors when strings are compared or transformed |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | derived_from_operands_or_form |
| Determinism | derived from lowered expression and dependencies |
| Side Effects | none |
| SBLR Binding | sblr.expr.special_cast.v3 |
| AST Binding | ast.special.special_cast |
| Engine Entrypoint | special_cast |
| Security Policy | inherits containing expression unless form reads session/system state |
| Error Semantics | see special-form error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | derived |
| index_eligible | derived |
| generated_column_eligible | derived |
| cost_class | special_form |

#### Practical Form

```sql
select cast(value_1 as integer) from app.sample_values;
```

### `current_timestamp`

| Property | Value |
| --- | --- |
| Builtin ID | sb.special.current_timestamp_keyword |
| UUID | 019de5fc-2400-79a1-be7b-8cc25ba7f84f |
| Kind | special_form |
| Syntax Forms | grammar_special_form |
| Overloads | CURRENT_TIMESTAMP[(precision)] |
| Return Type Rule | keyword temporal current timestamp |
| Coercion Rule | special-form-specific descriptor coercion; never stringly typed |
| Null Behavior | special-form-specific |
| Collation/Charset Rule | uses operand descriptors when strings are compared or transformed |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | derived_from_operands_or_form |
| Determinism | derived from lowered expression and dependencies |
| Side Effects | none |
| SBLR Binding | sblr.expr.special_current_timestamp_keyword.v3 |
| AST Binding | ast.special.special_current_timestamp_keyword |
| Engine Entrypoint | special_current_timestamp_keyword |
| Security Policy | inherits containing expression unless form reads session/system state |
| Error Semantics | see special-form error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | derived |
| index_eligible | derived |
| generated_column_eligible | derived |
| cost_class | special_form |

#### Practical Form

```sql
select current_timestamp(value_1) from app.sample_values;
```

### `extract`

| Property | Value |
| --- | --- |
| Builtin ID | sb.special.extract |
| UUID | 019de5fc-2400-766a-8421-7fb8cd577ded |
| Kind | special_form |
| Syntax Forms | grammar_special_form |
| Overloads | EXTRACT(part FROM temporal) |
| Return Type Rule | temporal field extraction special form |
| Coercion Rule | special-form-specific descriptor coercion; never stringly typed |
| Null Behavior | special-form-specific |
| Collation/Charset Rule | uses operand descriptors when strings are compared or transformed |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | derived_from_operands_or_form |
| Determinism | derived from lowered expression and dependencies |
| Side Effects | none |
| SBLR Binding | sblr.expr.special_extract.v3 |
| AST Binding | ast.special.special_extract |
| Engine Entrypoint | special_extract |
| Security Policy | inherits containing expression unless form reads session/system state |
| Error Semantics | see special-form error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | derived |
| index_eligible | derived |
| generated_column_eligible | derived |
| cost_class | special_form |

#### Practical Form

```sql
select extract(day from temporal_value_1) from app.sample_values;
```

### `in`

| Property | Value |
| --- | --- |
| Builtin ID | sb.special.in |
| UUID | 019de5fc-2400-7c61-8800-2dc3365c3a31 |
| Kind | special_form |
| Syntax Forms | grammar_special_form |
| Overloads | expr IN (list\|subquery) |
| Return Type Rule | list/subquery membership |
| Coercion Rule | special-form-specific descriptor coercion; never stringly typed |
| Null Behavior | special-form-specific |
| Collation/Charset Rule | uses operand descriptors when strings are compared or transformed |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | derived_from_operands_or_form |
| Determinism | derived from lowered expression and dependencies |
| Side Effects | none |
| SBLR Binding | sblr.expr.special_in.v3 |
| AST Binding | ast.special.special_in |
| Engine Entrypoint | special_in |
| Security Policy | inherits containing expression unless form reads session/system state |
| Error Semantics | see special-form error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | derived |
| index_eligible | derived |
| generated_column_eligible | derived |
| cost_class | special_form |

#### Practical Form

```sql
select in(value_1) from app.sample_values;
```

### `row`

| Property | Value |
| --- | --- |
| Builtin ID | sb.special.row_constructor |
| UUID | 019de5fc-2400-77f7-9d65-8e938144198a |
| Kind | special_form |
| Syntax Forms | grammar_special_form |
| Overloads | ROW(expr,...) |
| Return Type Rule | row descriptor constructor |
| Coercion Rule | special-form-specific descriptor coercion; never stringly typed |
| Null Behavior | special-form-specific |
| Collation/Charset Rule | uses operand descriptors when strings are compared or transformed |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | derived_from_operands_or_form |
| Determinism | derived from lowered expression and dependencies |
| Side Effects | none |
| SBLR Binding | sblr.expr.special_row_constructor.v3 |
| AST Binding | ast.special.special_row_constructor |
| Engine Entrypoint | special_row_constructor |
| Security Policy | inherits containing expression unless form reads session/system state |
| Error Semantics | see special-form error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | derived |
| index_eligible | derived |
| generated_column_eligible | derived |
| cost_class | special_form |

#### Practical Form

```sql
select row(value_1) from app.sample_values;
```

### `substring`

| Property | Value |
| --- | --- |
| Builtin ID | sb.special.substring_keyword |
| UUID | 019de5fc-2400-7865-ac60-cc7da933fc36 |
| Kind | special_form |
| Syntax Forms | grammar_special_form |
| Overloads | SUBSTRING(expr FROM start [FOR length]) |
| Return Type Rule | keyword-form substring |
| Coercion Rule | special-form-specific descriptor coercion; never stringly typed |
| Null Behavior | special-form-specific |
| Collation/Charset Rule | uses operand descriptors when strings are compared or transformed |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | derived_from_operands_or_form |
| Determinism | derived from lowered expression and dependencies |
| Side Effects | none |
| SBLR Binding | sblr.expr.special_substring_keyword.v3 |
| AST Binding | ast.special.special_substring_keyword |
| Engine Entrypoint | special_substring_keyword |
| Security Policy | inherits containing expression unless form reads session/system state |
| Error Semantics | see special-form error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | derived |
| index_eligible | derived |
| generated_column_eligible | derived |
| cost_class | special_form |

#### Practical Form

```sql
select substring(text_value_1 from 1 for 8) from app.sample_values;
```

### `trim`

| Property | Value |
| --- | --- |
| Builtin ID | sb.special.trim_keyword |
| UUID | 019de5fc-2400-75be-a69e-54e179826f6b |
| Kind | special_form |
| Syntax Forms | grammar_special_form |
| Overloads | TRIM([LEADING\|TRAILING\|BOTH] [chars] FROM expr) |
| Return Type Rule | keyword-form trim |
| Coercion Rule | special-form-specific descriptor coercion; never stringly typed |
| Null Behavior | special-form-specific |
| Collation/Charset Rule | uses operand descriptors when strings are compared or transformed |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | derived_from_operands_or_form |
| Determinism | derived from lowered expression and dependencies |
| Side Effects | none |
| SBLR Binding | sblr.expr.special_trim_keyword.v3 |
| AST Binding | ast.special.special_trim_keyword |
| Engine Entrypoint | special_trim_keyword |
| Security Policy | inherits containing expression unless form reads session/system state |
| Error Semantics | see special-form error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | derived |
| index_eligible | derived |
| generated_column_eligible | derived |
| cost_class | special_form |

#### Practical Form

```sql
select trim(value_1) from app.sample_values;
```

### `integer`

| Property | Value |
| --- | --- |
| Builtin ID | sb.type.integer |
| UUID | 019dffbb-f000-7d6b-9b68-cea235fd4dd2 |
| Kind | type_descriptor |
| Syntax Forms | function_call |
| Overloads | integer(...) |
| Return Type Rule | runtime-defined by engine entrypoint integer |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.type_integer.v3 |
| AST Binding | ast.expr.type_integer |
| Engine Entrypoint | integer |
| Security Policy | follows engine runtime seed registry authority for surface.scalar |
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
select integer(arg_1) from app.sample_values;
```

### `cume_dist`

| Property | Value |
| --- | --- |
| Builtin ID | sb.window.cume_dist |
| UUID | 019de5fc-2400-721c-be64-2568b64a02b9 |
| Kind | window |
| Syntax Forms | window_function_call |
| Overloads | cume_dist() |
| Return Type Rule | cumulative distribution |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | function-specific |
| Collation/Charset Rule | ORDER BY descriptor collation controls peer groups |
| Timezone Rule | not applicable unless temporal ordering/conversion applies |
| Volatility | stable_per_window |
| Determinism | deterministic when partition/order/frame are deterministic |
| Side Effects | none |
| SBLR Binding | sblr.expr.window_cume_dist.v3 |
| AST Binding | ast.window.window_cume_dist |
| Engine Entrypoint | window_cume_dist |
| Security Policy | inherits containing query rights |
| Error Semantics | see window error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | window_state |

#### Practical Form

```sql
select cume_dist() over (partition by account_id order by created_at) from app.orders;
```

### `dense_rank`

| Property | Value |
| --- | --- |
| Builtin ID | sb.window.dense_rank |
| UUID | 019de5fc-2400-741d-bef0-f079fd3ba494 |
| Kind | window |
| Syntax Forms | window_function_call |
| Overloads | dense_rank() |
| Return Type Rule | dense peer rank |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | function-specific |
| Collation/Charset Rule | ORDER BY descriptor collation controls peer groups |
| Timezone Rule | not applicable unless temporal ordering/conversion applies |
| Volatility | stable_per_window |
| Determinism | deterministic when partition/order/frame are deterministic |
| Side Effects | none |
| SBLR Binding | sblr.expr.window_dense_rank.v3 |
| AST Binding | ast.window.window_dense_rank |
| Engine Entrypoint | window_dense_rank |
| Security Policy | inherits containing query rights |
| Error Semantics | see window error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | window_state |

#### Practical Form

```sql
select dense_rank() over (partition by account_id order by created_at) from app.orders;
```

### `first_value`

| Property | Value |
| --- | --- |
| Builtin ID | sb.window.first_value |
| UUID | 019de5fc-2400-7264-90fb-d25bd0f806f2 |
| Kind | window |
| Syntax Forms | bare_function_name, canonical_builtin_id, window_function_call |
| Overloads | first_value(expr) |
| Return Type Rule | value from the first row in the evaluated frame; NULL when that source value is NULL |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | preserves the first frame row value, including NULL |
| Collation/Charset Rule | ORDER BY descriptor collation controls peer groups |
| Timezone Rule | not applicable unless temporal ordering/conversion applies |
| Volatility | stable_per_window |
| Determinism | deterministic when partition/order/frame are deterministic |
| Side Effects | none |
| SBLR Binding | sblr.expr.window_first_value.v3 |
| AST Binding | ast.window.window_first_value |
| Engine Entrypoint | window_first_value |
| Security Policy | inherits containing query rights |
| Error Semantics | invalid or empty window frame requests refuse with SB_DIAG_WINDOW_FRAME_INVALID; unsupported lowered ids refuse with SB_DIAG_WINDOW_FUNCTION_UNSUPPORTED |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | window_state |

#### Practical Form

```sql
select first_value(value_1) over (partition by account_id order by created_at) from app.orders;
```

### `lag`

| Property | Value |
| --- | --- |
| Builtin ID | sb.window.lag |
| UUID | 019de5fc-2400-782c-8436-9ac310301738 |
| Kind | window |
| Syntax Forms | bare_function_name, canonical_builtin_id, window_function_call |
| Overloads | lag(expr[,offset[,default]]) |
| Return Type Rule | value from current row minus offset in the ordered partition; default or NULL when target row is outside the partition |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | preserves NULL source row values; returns explicit default when target is outside the partition and a default is present, otherwise NULL |
| Collation/Charset Rule | ORDER BY descriptor collation controls peer groups |
| Timezone Rule | not applicable unless temporal ordering/conversion applies |
| Volatility | stable_per_window |
| Determinism | deterministic when partition/order/frame are deterministic |
| Side Effects | none |
| SBLR Binding | sblr.expr.window_lag.v3 |
| AST Binding | ast.window.window_lag |
| Engine Entrypoint | window_lag |
| Security Policy | inherits containing query rights |
| Error Semantics | invalid or empty window frame requests refuse with SB_DIAG_WINDOW_FRAME_INVALID; unsupported lowered ids refuse with SB_DIAG_WINDOW_FUNCTION_UNSUPPORTED |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | window_state |

#### Practical Form

```sql
select lag(value_1) over (partition by account_id order by created_at) from app.orders;
```

### `last_value`

| Property | Value |
| --- | --- |
| Builtin ID | sb.window.last_value |
| UUID | 019de5fc-2400-7d23-a5be-7ed3f1a5c3ec |
| Kind | window |
| Syntax Forms | bare_function_name, canonical_builtin_id, window_function_call |
| Overloads | last_value(expr) |
| Return Type Rule | value from the last row in the evaluated frame; NULL when the frame is empty or that source value is NULL |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | preserves the last frame row value, including NULL; empty frames return NULL |
| Collation/Charset Rule | ORDER BY descriptor collation controls peer groups |
| Timezone Rule | not applicable unless temporal ordering/conversion applies |
| Volatility | stable_per_window |
| Determinism | deterministic when partition/order/frame are deterministic |
| Side Effects | none |
| SBLR Binding | sblr.expr.window_last_value.v3 |
| AST Binding | ast.window.window_last_value |
| Engine Entrypoint | window_last_value |
| Security Policy | inherits containing query rights |
| Error Semantics | invalid current-row requests refuse with SB_DIAG_WINDOW_FRAME_INVALID; unsupported lowered ids refuse with SB_DIAG_WINDOW_FUNCTION_UNSUPPORTED |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | window_state |

#### Practical Form

```sql
select last_value(value_1) over (partition by account_id order by created_at) from app.orders;
```

### `lead`

| Property | Value |
| --- | --- |
| Builtin ID | sb.window.lead |
| UUID | 019de5fc-2400-7a06-bc3c-6747cf5be66f |
| Kind | window |
| Syntax Forms | bare_function_name, canonical_builtin_id, window_function_call |
| Overloads | lead(expr[,offset[,default]]) |
| Return Type Rule | value from current row plus offset in the ordered partition; default or NULL when target row is outside the partition |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | preserves NULL source row values; returns explicit default when target is outside the partition and a default is present, otherwise NULL |
| Collation/Charset Rule | ORDER BY descriptor collation controls peer groups |
| Timezone Rule | not applicable unless temporal ordering/conversion applies |
| Volatility | stable_per_window |
| Determinism | deterministic when partition/order/frame are deterministic |
| Side Effects | none |
| SBLR Binding | sblr.expr.window_lead.v3 |
| AST Binding | ast.window.window_lead |
| Engine Entrypoint | window_lead |
| Security Policy | inherits containing query rights |
| Error Semantics | invalid or empty window frame requests refuse with SB_DIAG_WINDOW_FRAME_INVALID; unsupported lowered ids refuse with SB_DIAG_WINDOW_FUNCTION_UNSUPPORTED |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | window_state |

#### Practical Form

```sql
select lead(value_1) over (partition by account_id order by created_at) from app.orders;
```

### `nth_value`

| Property | Value |
| --- | --- |
| Builtin ID | sb.window.nth_value |
| UUID | 019de5fc-2400-7dc9-80e6-9f2ccf08076f |
| Kind | window |
| Syntax Forms | bare_function_name, canonical_builtin_id, window_function_call |
| Overloads | nth_value(expr,n) |
| Return Type Rule | value from the nth row in the evaluated frame; NULL when the frame has fewer than n rows or that source value is NULL |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | preserves the selected frame row value, including NULL; empty or too-short frames return NULL |
| Collation/Charset Rule | ORDER BY descriptor collation controls peer groups |
| Timezone Rule | not applicable unless temporal ordering/conversion applies |
| Volatility | stable_per_window |
| Determinism | deterministic when partition/order/frame are deterministic |
| Side Effects | none |
| SBLR Binding | sblr.expr.window_nth_value.v3 |
| AST Binding | ast.window.window_nth_value |
| Engine Entrypoint | window_nth_value |
| Security Policy | inherits containing query rights |
| Error Semantics | invalid current-row requests refuse with SB_DIAG_WINDOW_FRAME_INVALID; n=0 refuses with SB_DIAG_WINDOW_NTH_VALUE_INVALID; unsupported lowered ids refuse with SB_DIAG_WINDOW_FUNCTION_UNSUPPORTED |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | window_state |

#### Practical Form

```sql
select nth_value(value_1, arg_2) over (partition by account_id order by created_at) from app.orders;
```

### `ntile`

| Property | Value |
| --- | --- |
| Builtin ID | sb.window.ntile |
| UUID | 019de5fc-2400-7047-9474-232ca488c094 |
| Kind | window |
| Syntax Forms | bare_function_name, canonical_builtin_id, window_function_call |
| Overloads | ntile(n) |
| Return Type Rule | int64 1-based bucket assignment across the ordered partition |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | not value-dependent; NULL input rules are not applicable because ntile has no value argument |
| Collation/Charset Rule | ORDER BY descriptor collation controls peer groups |
| Timezone Rule | not applicable unless temporal ordering/conversion applies |
| Volatility | stable_per_window |
| Determinism | deterministic when partition/order/frame are deterministic |
| Side Effects | none |
| SBLR Binding | sblr.expr.window_ntile.v3 |
| AST Binding | ast.window.window_ntile |
| Engine Entrypoint | window_ntile |
| Security Policy | inherits containing query rights |
| Error Semantics | invalid or empty window frame requests refuse with SB_DIAG_WINDOW_FRAME_INVALID; zero bucket count refuses with SB_DIAG_WINDOW_NTILE_BUCKET_INVALID; unsupported lowered ids refuse with SB_DIAG_WINDOW_FUNCTION_UNSUPPORTED |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | window_state |

#### Practical Form

```sql
select ntile(arg_1) over (partition by account_id order by created_at) from app.orders;
```

### `percent_rank`

| Property | Value |
| --- | --- |
| Builtin ID | sb.window.percent_rank |
| UUID | 019de5fc-2400-7d86-86fe-96f3f27b5dd6 |
| Kind | window |
| Syntax Forms | window_function_call |
| Overloads | percent_rank() |
| Return Type Rule | rank percentile |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | function-specific |
| Collation/Charset Rule | ORDER BY descriptor collation controls peer groups |
| Timezone Rule | not applicable unless temporal ordering/conversion applies |
| Volatility | stable_per_window |
| Determinism | deterministic when partition/order/frame are deterministic |
| Side Effects | none |
| SBLR Binding | sblr.expr.window_percent_rank.v3 |
| AST Binding | ast.window.window_percent_rank |
| Engine Entrypoint | window_percent_rank |
| Security Policy | inherits containing query rights |
| Error Semantics | see window error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | window_state |

#### Practical Form

```sql
select percent_rank() over (partition by account_id order by created_at) from app.orders;
```

### `rank`

| Property | Value |
| --- | --- |
| Builtin ID | sb.window.rank |
| UUID | 019de5fc-2400-7b94-870d-0dd789ca70ab |
| Kind | window |
| Syntax Forms | window_function_call |
| Overloads | rank() |
| Return Type Rule | peer group rank |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | function-specific |
| Collation/Charset Rule | ORDER BY descriptor collation controls peer groups |
| Timezone Rule | not applicable unless temporal ordering/conversion applies |
| Volatility | stable_per_window |
| Determinism | deterministic when partition/order/frame are deterministic |
| Side Effects | none |
| SBLR Binding | sblr.expr.window_rank.v3 |
| AST Binding | ast.window.window_rank |
| Engine Entrypoint | window_rank |
| Security Policy | inherits containing query rights |
| Error Semantics | see window error compatibility matrix |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | window_state |

#### Practical Form

```sql
select rank() over (partition by account_id order by created_at) from app.orders;
```

### `row_number`

| Property | Value |
| --- | --- |
| Builtin ID | sb.window.row_number |
| UUID | 019de5fc-2400-7539-bcce-00eef3ae7220 |
| Kind | window |
| Syntax Forms | bare_function_name, canonical_builtin_id, window_function_call |
| Overloads | row_number() |
| Return Type Rule | int64 1-based ordinal within the ordered partition; requires ORDER BY for deterministic portable output |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | not value-dependent; NULL input rules are not applicable because row_number has no value argument |
| Collation/Charset Rule | ORDER BY descriptor collation controls peer groups |
| Timezone Rule | not applicable unless temporal ordering/conversion applies |
| Volatility | stable_per_window |
| Determinism | deterministic when partition/order/frame are deterministic |
| Side Effects | none |
| SBLR Binding | sblr.expr.window_row_number.v3 |
| AST Binding | ast.window.window_row_number |
| Engine Entrypoint | window_row_number |
| Security Policy | inherits containing query rights |
| Error Semantics | invalid or empty window frame requests refuse with SB_DIAG_WINDOW_FRAME_INVALID; unsupported lowered ids refuse with SB_DIAG_WINDOW_FUNCTION_UNSUPPORTED |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | window_state |

#### Practical Form

```sql
select row_number() over (partition by account_id order by created_at) from app.orders;
```


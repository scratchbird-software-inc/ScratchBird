# Sb Range Functional Reference

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `sb_range`


## Package Boundary

`sb.range` contains 9 registered built-in operation records. Each record binds a public function, operator, aggregate, window function, or special form to descriptor-aware overloads, null behavior, optimizer properties, SBLR binding, and engine entrypoint metadata.

The package boundary is semantic, not a privilege boundary. A caller still needs object privileges, expression admission, descriptor compatibility, and policy admission for the statement that uses the operation.

## Package Inventory

| Kind | Records |
| --- | --- |
| scalar | 9 |

## Type Mechanics

- Argument descriptors select the overload.
- Return descriptors come from the declared return type rule.
- Null, collation, charset, timezone, and security behavior are part of binding.
- Optimizer properties may allow constant folding, generated column use, or index eligibility, but execution authority remains with the engine.
- Practical forms below are canonical usage shapes derived from declared overload signatures; statement admission still depends on the surrounding query, object privileges, and descriptor binding.

## Operation Records

### `range_contains`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_contains |
| UUID | 019dffbb-f000-79e8-8098-f3a7e1212ace |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | range_contains(...) |
| Return Type Rule | runtime-defined by engine entrypoint range_contains |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_range_contains.v3 |
| AST Binding | ast.expr.scalar_range_contains |
| Engine Entrypoint | range_contains |
| Security Policy | follows engine runtime seed registry authority for range |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select range_contains(arg_1) from app.sample_values;
```

### `range_contains_element`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_contains_element |
| UUID | 019dffbb-f000-74ed-91c4-1b280b76ac8a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | range_contains_element(...) |
| Return Type Rule | runtime-defined by engine entrypoint range_contains_element |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_range_contains_element.v3 |
| AST Binding | ast.expr.scalar_range_contains_element |
| Engine Entrypoint | range_contains_element |
| Security Policy | follows engine runtime seed registry authority for range |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select range_contains_element(arg_1) from app.sample_values;
```

### `range_lower`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_lower |
| UUID | 019dffbb-f000-7228-a8fd-b333c6378a8f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | range_lower(...) |
| Return Type Rule | runtime-defined by engine entrypoint range_lower |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_range_lower.v3 |
| AST Binding | ast.expr.scalar_range_lower |
| Engine Entrypoint | range_lower |
| Security Policy | follows engine runtime seed registry authority for range |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select range_lower(arg_1) from app.sample_values;
```

### `range_lower_inc`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_lower_inc |
| UUID | 019dffbb-f000-7d64-9a6c-3f0fb7806195 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | range_lower_inc(...) |
| Return Type Rule | runtime-defined by engine entrypoint range_lower_inc |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_range_lower_inc.v3 |
| AST Binding | ast.expr.scalar_range_lower_inc |
| Engine Entrypoint | range_lower_inc |
| Security Policy | follows engine runtime seed registry authority for range |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select range_lower_inc(arg_1) from app.sample_values;
```

### `range_overlaps`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_overlaps |
| UUID | 019dffbb-f000-70da-bb90-bf9233ce6a1a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | range_overlaps(...) |
| Return Type Rule | runtime-defined by engine entrypoint range_overlaps |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_range_overlaps.v3 |
| AST Binding | ast.expr.scalar_range_overlaps |
| Engine Entrypoint | range_overlaps |
| Security Policy | follows engine runtime seed registry authority for range |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select range_overlaps(arg_1) from app.sample_values;
```

### `range_strictly_left`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_strictly_left |
| UUID | 019dffbb-f000-7240-837d-0768c70f3346 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | range_strictly_left(...) |
| Return Type Rule | runtime-defined by engine entrypoint range_strictly_left |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_range_strictly_left.v3 |
| AST Binding | ast.expr.scalar_range_strictly_left |
| Engine Entrypoint | range_strictly_left |
| Security Policy | follows engine runtime seed registry authority for range |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select range_strictly_left(arg_1) from app.sample_values;
```

### `range_strictly_right`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_strictly_right |
| UUID | 019dffbb-f000-7952-998a-6734177d6c8d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | range_strictly_right(...) |
| Return Type Rule | runtime-defined by engine entrypoint range_strictly_right |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_range_strictly_right.v3 |
| AST Binding | ast.expr.scalar_range_strictly_right |
| Engine Entrypoint | range_strictly_right |
| Security Policy | follows engine runtime seed registry authority for range |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select range_strictly_right(arg_1) from app.sample_values;
```

### `range_upper`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_upper |
| UUID | 019dffbb-f000-7c9b-bab4-4579be8d3fde |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | range_upper(...) |
| Return Type Rule | runtime-defined by engine entrypoint range_upper |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_range_upper.v3 |
| AST Binding | ast.expr.scalar_range_upper |
| Engine Entrypoint | range_upper |
| Security Policy | follows engine runtime seed registry authority for range |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select range_upper(arg_1) from app.sample_values;
```

### `range_upper_inc`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.range_upper_inc |
| UUID | 019dffbb-f000-75dc-bf43-b957d26590a9 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | range_upper_inc(...) |
| Return Type Rule | runtime-defined by engine entrypoint range_upper_inc |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_range_upper_inc.v3 |
| AST Binding | ast.expr.scalar_range_upper_inc |
| Engine Entrypoint | range_upper_inc |
| Security Policy | follows engine runtime seed registry authority for range |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select range_upper_inc(arg_1) from app.sample_values;
```


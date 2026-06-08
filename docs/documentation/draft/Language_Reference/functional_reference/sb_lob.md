# Sb Lob Functional Reference

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `sb_lob`


## Package Boundary

`sb.lob` contains 13 registered built-in operation records. Each record binds a public function, operator, aggregate, window function, or special form to descriptor-aware overloads, null behavior, optimizer properties, SBLR binding, and engine entrypoint metadata.

The package boundary is semantic, not a privilege boundary. A caller still needs object privileges, expression admission, descriptor compatibility, and policy admission for the statement that uses the operation.

## Package Inventory

| Kind | Records |
| --- | --- |
| scalar | 13 |

## Type Mechanics

- Argument descriptors select the overload.
- Return descriptors come from the declared return type rule.
- Null, collation, charset, timezone, and security behavior are part of binding.
- Optimizer properties may allow constant folding, generated column use, or index eligibility, but execution authority remains with the engine.
- Practical forms below are canonical usage shapes derived from declared overload signatures; statement admission still depends on the surrounding query, object privileges, and descriptor binding.

## Operation Records

### `current_row_locator`

| Property | Value |
| --- | --- |
| Builtin ID | sb.locator.current_row |
| UUID | 019dffbb-f000-7b96-9888-7749d95abd28 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | current_row_locator |
| Return Type Rule | runtime-defined by engine entrypoint current_row_locator |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.locator_current_row.v3 |
| AST Binding | ast.expr.locator_current_row |
| Engine Entrypoint | current_row_locator |
| Security Policy | follows engine runtime seed registry authority for lob.locator |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select current_row_locator() from app.sample_values;
```

### `lob_append`

| Property | Value |
| --- | --- |
| Builtin ID | sb.lob.append |
| UUID | 019dffbb-f000-74aa-a021-045a7d62e136 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | lob_append(...) |
| Return Type Rule | runtime-defined by engine entrypoint lob_append |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.lob_append.v3 |
| AST Binding | ast.expr.lob_append |
| Engine Entrypoint | lob_append |
| Security Policy | follows engine runtime seed registry authority for lob.locator |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select lob_append(arg_1) from app.sample_values;
```

### `lob_close`

| Property | Value |
| --- | --- |
| Builtin ID | sb.lob.close |
| UUID | 019dffbb-f000-7a11-a540-e6c1f5bffd0f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | lob_close(...) |
| Return Type Rule | runtime-defined by engine entrypoint lob_close |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.lob_close.v3 |
| AST Binding | ast.expr.lob_close |
| Engine Entrypoint | lob_close |
| Security Policy | follows engine runtime seed registry authority for lob.locator |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select lob_close(arg_1) from app.sample_values;
```

### `lob_create`

| Property | Value |
| --- | --- |
| Builtin ID | sb.lob.create |
| UUID | 019dffbb-f000-7769-9c32-920cb2a44d38 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | lob_create(...) |
| Return Type Rule | runtime-defined by engine entrypoint lob_create |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.lob_create.v3 |
| AST Binding | ast.expr.lob_create |
| Engine Entrypoint | lob_create |
| Security Policy | follows engine runtime seed registry authority for lob.locator |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select lob_create(arg_1) from app.sample_values;
```

### `lob_locator_to_binary`

| Property | Value |
| --- | --- |
| Builtin ID | sb.lob.locator_to_binary |
| UUID | 019dffbb-f000-71a7-8d88-378d891fdecd |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | lob_locator_to_binary(...) |
| Return Type Rule | runtime-defined by engine entrypoint lob_locator_to_binary |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.lob_locator_to_binary.v3 |
| AST Binding | ast.expr.lob_locator_to_binary |
| Engine Entrypoint | lob_locator_to_binary |
| Security Policy | follows engine runtime seed registry authority for lob.locator |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select lob_locator_to_binary(arg_1) from app.sample_values;
```

### `lob_locator_to_text`

| Property | Value |
| --- | --- |
| Builtin ID | sb.lob.locator_to_text |
| UUID | 019dffbb-f000-703a-bc67-17bdf02e4614 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | lob_locator_to_text(...) |
| Return Type Rule | runtime-defined by engine entrypoint lob_locator_to_text |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.lob_locator_to_text.v3 |
| AST Binding | ast.expr.lob_locator_to_text |
| Engine Entrypoint | lob_locator_to_text |
| Security Policy | follows engine runtime seed registry authority for lob.locator |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select lob_locator_to_text(arg_1) from app.sample_values;
```

### `lob_open`

| Property | Value |
| --- | --- |
| Builtin ID | sb.lob.open |
| UUID | 019dffbb-f000-7050-93e3-09da21b5e350 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | lob_open(...) |
| Return Type Rule | runtime-defined by engine entrypoint lob_open |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.lob_open.v3 |
| AST Binding | ast.expr.lob_open |
| Engine Entrypoint | lob_open |
| Security Policy | follows engine runtime seed registry authority for lob.locator |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select lob_open(arg_1) from app.sample_values;
```

### `lob_read`

| Property | Value |
| --- | --- |
| Builtin ID | sb.lob.read |
| UUID | 019dffbb-f000-7873-af4d-bad73e1e5c43 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | lob_read(...) |
| Return Type Rule | runtime-defined by engine entrypoint lob_read |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.lob_read.v3 |
| AST Binding | ast.expr.lob_read |
| Engine Entrypoint | lob_read |
| Security Policy | follows engine runtime seed registry authority for lob.locator |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select lob_read(arg_1) from app.sample_values;
```

### `lob_size`

| Property | Value |
| --- | --- |
| Builtin ID | sb.lob.size |
| UUID | 019dffbb-f000-7617-bb0e-7a9985ff6ecb |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | lob_size(...) |
| Return Type Rule | runtime-defined by engine entrypoint lob_size |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.lob_size.v3 |
| AST Binding | ast.expr.lob_size |
| Engine Entrypoint | lob_size |
| Security Policy | follows engine runtime seed registry authority for lob.locator |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select lob_size(arg_1) from app.sample_values;
```

### `lob_truncate`

| Property | Value |
| --- | --- |
| Builtin ID | sb.lob.truncate |
| UUID | 019dffbb-f000-72a7-ba33-d3e216220898 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | lob_truncate(...) |
| Return Type Rule | runtime-defined by engine entrypoint lob_truncate |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.lob_truncate.v3 |
| AST Binding | ast.expr.lob_truncate |
| Engine Entrypoint | lob_truncate |
| Security Policy | follows engine runtime seed registry authority for lob.locator |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select lob_truncate(arg_1) from app.sample_values;
```

### `lob_write`

| Property | Value |
| --- | --- |
| Builtin ID | sb.lob.write |
| UUID | 019dffbb-f000-7e0a-b791-07758dd38a0e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | lob_write(...) |
| Return Type Rule | runtime-defined by engine entrypoint lob_write |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.lob_write.v3 |
| AST Binding | ast.expr.lob_write |
| Engine Entrypoint | lob_write |
| Security Policy | follows engine runtime seed registry authority for lob.locator |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select lob_write(arg_1) from app.sample_values;
```

### `locator`

| Property | Value |
| --- | --- |
| Builtin ID | sb.locator.locator |
| UUID | 019dffbb-f000-7c6d-89a8-7ef94e5d0d29 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | locator(...) |
| Return Type Rule | runtime-defined by engine entrypoint locator |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.locator_locator.v3 |
| AST Binding | ast.expr.locator_locator |
| Engine Entrypoint | locator |
| Security Policy | follows engine runtime seed registry authority for lob.locator |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select locator(arg_1) from app.sample_values;
```

### `locator_validity`

| Property | Value |
| --- | --- |
| Builtin ID | sb.locator.validity |
| UUID | 019dffbb-f000-721e-9794-a0b7d2899ba1 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | locator_validity(...) |
| Return Type Rule | runtime-defined by engine entrypoint locator_validity |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.locator_validity.v3 |
| AST Binding | ast.expr.locator_validity |
| Engine Entrypoint | locator_validity |
| Security Policy | follows engine runtime seed registry authority for lob.locator |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select locator_validity(arg_1) from app.sample_values;
```


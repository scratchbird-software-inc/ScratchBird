# Sb Cursor Functional Reference

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `sb_cursor`


## Package Boundary

`sb.cursor` contains 15 registered built-in operation records. Each record binds a public function, operator, aggregate, window function, or special form to descriptor-aware overloads, null behavior, optimizer properties, SBLR binding, and engine entrypoint metadata.

The package boundary is semantic, not a privilege boundary. A caller still needs object privileges, expression admission, descriptor compatibility, and policy admission for the statement that uses the operation.

## Package Inventory

| Kind | Records |
| --- | --- |
| scalar | 15 |

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
| Builtin ID | sb.cursor.current_row_locator |
| UUID | 019dffbb-f000-70a0-ae1b-b1c57c9b32e7 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | current_row_locator(cursor) |
| Return Type Rule | runtime-defined by engine entrypoint current_row_locator |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.cursor_current_row_locator.v3 |
| AST Binding | ast.expr.cursor_current_row_locator |
| Engine Entrypoint | current_row_locator |
| Security Policy | follows engine runtime seed registry authority for cursor.stream |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select current_row_locator(arg_1) from app.sample_values;
```

### `cursor_active`

| Property | Value |
| --- | --- |
| Builtin ID | sb.cursor.active |
| UUID | 019dffbb-f000-7783-a337-566f9fad37d2 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cursor_active(...) |
| Return Type Rule | runtime-defined by engine entrypoint cursor_active |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.cursor_active.v3 |
| AST Binding | ast.expr.cursor_active |
| Engine Entrypoint | cursor_active |
| Security Policy | follows engine runtime seed registry authority for cursor.stream |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select cursor_active(arg_1) from app.sample_values;
```

### `cursor_close`

| Property | Value |
| --- | --- |
| Builtin ID | sb.cursor.close |
| UUID | 019dffbb-f000-7c51-8503-c6f3d4045f94 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cursor_close(...) |
| Return Type Rule | runtime-defined by engine entrypoint cursor_close |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.cursor_close.v3 |
| AST Binding | ast.expr.cursor_close |
| Engine Entrypoint | cursor_close |
| Security Policy | follows engine runtime seed registry authority for cursor.stream |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select cursor_close(arg_1) from app.sample_values;
```

### `cursor_holdability`

| Property | Value |
| --- | --- |
| Builtin ID | sb.cursor.holdability |
| UUID | 019dffbb-f000-7ca2-a240-a967ab4e34fc |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cursor_holdability(...) |
| Return Type Rule | runtime-defined by engine entrypoint cursor_holdability |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.cursor_holdability.v3 |
| AST Binding | ast.expr.cursor_holdability |
| Engine Entrypoint | cursor_holdability |
| Security Policy | follows engine runtime seed registry authority for cursor.stream |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select cursor_holdability(arg_1) from app.sample_values;
```

### `cursor_lifetime_class`

| Property | Value |
| --- | --- |
| Builtin ID | sb.cursor.lifetime_class |
| UUID | 019dffbb-f000-744e-8752-29a69c6ca5fb |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cursor_lifetime_class(...) |
| Return Type Rule | runtime-defined by engine entrypoint cursor_lifetime_class |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.cursor_lifetime_class.v3 |
| AST Binding | ast.expr.cursor_lifetime_class |
| Engine Entrypoint | cursor_lifetime_class |
| Security Policy | follows engine runtime seed registry authority for cursor.stream |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select cursor_lifetime_class(arg_1) from app.sample_values;
```

### `cursor_open`

| Property | Value |
| --- | --- |
| Builtin ID | sb.cursor.open |
| UUID | 019dffbb-f000-75d7-b434-542598b842ec |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cursor_open(...) |
| Return Type Rule | runtime-defined by engine entrypoint cursor_open |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.cursor_open.v3 |
| AST Binding | ast.expr.cursor_open |
| Engine Entrypoint | cursor_open |
| Security Policy | follows engine runtime seed registry authority for cursor.stream |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select cursor_open(arg_1) from app.sample_values;
```

### `cursor_position`

| Property | Value |
| --- | --- |
| Builtin ID | sb.cursor.position |
| UUID | 019dffbb-f000-7457-b674-79e5e9801f8a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cursor_position(...) |
| Return Type Rule | runtime-defined by engine entrypoint cursor_position |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.cursor_position.v3 |
| AST Binding | ast.expr.cursor_position |
| Engine Entrypoint | cursor_position |
| Security Policy | follows engine runtime seed registry authority for cursor.stream |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select cursor_position(arg_1) from app.sample_values;
```

### `cursor_scrollability`

| Property | Value |
| --- | --- |
| Builtin ID | sb.cursor.scrollability |
| UUID | 019dffbb-f000-7967-8cca-e48403bb663b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cursor_scrollability(...) |
| Return Type Rule | runtime-defined by engine entrypoint cursor_scrollability |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.cursor_scrollability.v3 |
| AST Binding | ast.expr.cursor_scrollability |
| Engine Entrypoint | cursor_scrollability |
| Security Policy | follows engine runtime seed registry authority for cursor.stream |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select cursor_scrollability(arg_1) from app.sample_values;
```

### `cursor_state`

| Property | Value |
| --- | --- |
| Builtin ID | sb.cursor.state |
| UUID | 019dffbb-f000-7f46-845b-79faabb48810 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cursor_state(...) |
| Return Type Rule | runtime-defined by engine entrypoint cursor_state |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.cursor_state.v3 |
| AST Binding | ast.expr.cursor_state |
| Engine Entrypoint | cursor_state |
| Security Policy | follows engine runtime seed registry authority for cursor.stream |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select cursor_state(arg_1) from app.sample_values;
```

### `cursor_to_rowset`

| Property | Value |
| --- | --- |
| Builtin ID | sb.cursor.to_rowset |
| UUID | 019dffbb-f000-7e9c-9854-eaabf1cab19b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cursor_to_rowset(...) |
| Return Type Rule | runtime-defined by engine entrypoint cursor_to_rowset |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.cursor_to_rowset.v3 |
| AST Binding | ast.expr.cursor_to_rowset |
| Engine Entrypoint | cursor_to_rowset |
| Security Policy | follows engine runtime seed registry authority for cursor.stream |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select cursor_to_rowset(arg_1) from app.sample_values;
```

### `handle_kind`

| Property | Value |
| --- | --- |
| Builtin ID | sb.handle.kind |
| UUID | 019dffbb-f000-78d1-817e-ba6064284df9 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | handle_kind(...) |
| Return Type Rule | runtime-defined by engine entrypoint handle_kind |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.handle_kind.v3 |
| AST Binding | ast.expr.handle_kind |
| Engine Entrypoint | handle_kind |
| Security Policy | follows engine runtime seed registry authority for cursor.stream |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select handle_kind(arg_1) from app.sample_values;
```

### `rowset_to_cursor`

| Property | Value |
| --- | --- |
| Builtin ID | sb.cursor.rowset_to_cursor |
| UUID | 019dffbb-f000-7f64-93a7-b921be1ae581 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | rowset_to_cursor(...) |
| Return Type Rule | runtime-defined by engine entrypoint rowset_to_cursor |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.cursor_rowset_to_cursor.v3 |
| AST Binding | ast.expr.cursor_rowset_to_cursor |
| Engine Entrypoint | rowset_to_cursor |
| Security Policy | follows engine runtime seed registry authority for cursor.stream |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select rowset_to_cursor(arg_1) from app.sample_values;
```

### `stream_close`

| Property | Value |
| --- | --- |
| Builtin ID | sb.stream.close |
| UUID | 019dffbb-f000-7883-9e52-f39d10c0e0f3 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | stream_close(...) |
| Return Type Rule | runtime-defined by engine entrypoint stream_close |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.stream_close.v3 |
| AST Binding | ast.expr.stream_close |
| Engine Entrypoint | stream_close |
| Security Policy | follows engine runtime seed registry authority for cursor.stream |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select stream_close(arg_1) from app.sample_values;
```

### `stream_to_rowset`

| Property | Value |
| --- | --- |
| Builtin ID | sb.stream.to_rowset |
| UUID | 019dffbb-f000-742f-9f33-bad0bf5a0fb1 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | stream_to_rowset(...) |
| Return Type Rule | runtime-defined by engine entrypoint stream_to_rowset |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.stream_to_rowset.v3 |
| AST Binding | ast.expr.stream_to_rowset |
| Engine Entrypoint | stream_to_rowset |
| Security Policy | follows engine runtime seed registry authority for cursor.stream |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select stream_to_rowset(arg_1) from app.sample_values;
```

### `table_value_to_cursor`

| Property | Value |
| --- | --- |
| Builtin ID | sb.cursor.table_value_to_cursor |
| UUID | 019dffbb-f000-7621-8bcb-dc9af37c15a9 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | table_value_to_cursor(...) |
| Return Type Rule | runtime-defined by engine entrypoint table_value_to_cursor |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.cursor_table_value_to_cursor.v3 |
| AST Binding | ast.expr.cursor_table_value_to_cursor |
| Engine Entrypoint | table_value_to_cursor |
| Security Policy | follows engine runtime seed registry authority for cursor.stream |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select table_value_to_cursor(arg_1) from app.sample_values;
```


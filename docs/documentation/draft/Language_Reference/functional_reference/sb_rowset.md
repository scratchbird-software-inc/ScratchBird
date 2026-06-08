# Sb Rowset Functional Reference

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `sb_rowset`


## Package Boundary

`sb.rowset` contains 16 registered built-in operation records. Each record binds a public function, operator, aggregate, window function, or special form to descriptor-aware overloads, null behavior, optimizer properties, SBLR binding, and engine entrypoint metadata.

The package boundary is semantic, not a privilege boundary. A caller still needs object privileges, expression admission, descriptor compatibility, and policy admission for the statement that uses the operation.

## Package Inventory

| Kind | Records |
| --- | --- |
| scalar | 16 |

## Type Mechanics

- Argument descriptors select the overload.
- Return descriptors come from the declared return type rule.
- Null, collation, charset, timezone, and security behavior are part of binding.
- Optimizer properties may allow constant folding, generated column use, or index eligibility, but execution authority remains with the engine.
- Practical forms below are canonical usage shapes derived from declared overload signatures; statement admission still depends on the surrounding query, object privileges, and descriptor binding.

## Operation Records

### `element`

| Property | Value |
| --- | --- |
| Builtin ID | sb.multiset.element |
| UUID | 019dffbb-f000-7c3e-a192-e79f26db9b80 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | element(...) |
| Return Type Rule | runtime-defined by engine entrypoint element |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.multiset_element.v3 |
| AST Binding | ast.expr.multiset_element |
| Engine Entrypoint | element |
| Security Policy | follows engine runtime seed registry authority for rowset.table |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select element(arg_1) from app.sample_values;
```

### `fusion`

| Property | Value |
| --- | --- |
| Builtin ID | sb.multiset.fusion |
| UUID | 019dffbb-f000-70b2-9c5e-e8db048341ca |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | fusion(...) |
| Return Type Rule | runtime-defined by engine entrypoint fusion |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.multiset_fusion.v3 |
| AST Binding | ast.expr.multiset_fusion |
| Engine Entrypoint | fusion |
| Security Policy | follows engine runtime seed registry authority for rowset.table |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select fusion(arg_1) from app.sample_values;
```

### `generate_series`

| Property | Value |
| --- | --- |
| Builtin ID | sb.rowset.generate_series |
| UUID | 019dffbb-f000-7e2c-b437-ebbbc2d4f35b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | generate_series(...) |
| Return Type Rule | runtime-defined by engine entrypoint generate_series |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.rowset_generate_series.v3 |
| AST Binding | ast.expr.rowset_generate_series |
| Engine Entrypoint | generate_series |
| Security Policy | follows engine runtime seed registry authority for rowset.table |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select generate_series(arg_1) from app.sample_values;
```

### `intersection`

| Property | Value |
| --- | --- |
| Builtin ID | sb.multiset.intersection |
| UUID | 019dffbb-f000-7968-9b55-2bdbfa60c297 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | intersection(...) |
| Return Type Rule | runtime-defined by engine entrypoint intersection |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.multiset_intersection.v3 |
| AST Binding | ast.expr.multiset_intersection |
| Engine Entrypoint | intersection |
| Security Policy | follows engine runtime seed registry authority for rowset.table |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select intersection(arg_1) from app.sample_values;
```

### `rowset`

| Property | Value |
| --- | --- |
| Builtin ID | sb.rowset.rowset |
| UUID | 019dffbb-f000-78cb-8907-f0afcc0374f1 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | rowset(...) |
| Return Type Rule | runtime-defined by engine entrypoint rowset |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.rowset_rowset.v3 |
| AST Binding | ast.expr.rowset_rowset |
| Engine Entrypoint | rowset |
| Security Policy | follows engine runtime seed registry authority for rowset.table |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select rowset(arg_1) from app.sample_values;
```

### `rowset_append`

| Property | Value |
| --- | --- |
| Builtin ID | sb.rowset.append |
| UUID | 019dffbb-f000-7275-bd16-ba5ae55214da |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | rowset_append(...) |
| Return Type Rule | runtime-defined by engine entrypoint rowset_append |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.rowset_append.v3 |
| AST Binding | ast.expr.rowset_append |
| Engine Entrypoint | rowset_append |
| Security Policy | follows engine runtime seed registry authority for rowset.table |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select rowset_append(arg_1) from app.sample_values;
```

### `rowset_new`

| Property | Value |
| --- | --- |
| Builtin ID | sb.rowset.new |
| UUID | 019dffbb-f000-77b1-8fef-7b509cc646ae |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | rowset_new(...) |
| Return Type Rule | runtime-defined by engine entrypoint rowset_new |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.rowset_new.v3 |
| AST Binding | ast.expr.rowset_new |
| Engine Entrypoint | rowset_new |
| Security Policy | follows engine runtime seed registry authority for rowset.table |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select rowset_new(arg_1) from app.sample_values;
```

### `rowset_size`

| Property | Value |
| --- | --- |
| Builtin ID | sb.rowset.size |
| UUID | 019dffbb-f000-7aa8-a950-773cbbd414ec |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | rowset_size(...) |
| Return Type Rule | runtime-defined by engine entrypoint rowset_size |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.rowset_size.v3 |
| AST Binding | ast.expr.rowset_size |
| Engine Entrypoint | rowset_size |
| Security Policy | follows engine runtime seed registry authority for rowset.table |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select rowset_size(arg_1) from app.sample_values;
```

### `rowset_to_array`

| Property | Value |
| --- | --- |
| Builtin ID | sb.rowset.to_array |
| UUID | 019dffbb-f000-780f-9abf-9905eff4d97f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | rowset_to_array(...) |
| Return Type Rule | runtime-defined by engine entrypoint rowset_to_array |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.rowset_to_array.v3 |
| AST Binding | ast.expr.rowset_to_array |
| Engine Entrypoint | rowset_to_array |
| Security Policy | follows engine runtime seed registry authority for rowset.table |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select rowset_to_array(arg_1) from app.sample_values;
```

### `setof`

| Property | Value |
| --- | --- |
| Builtin ID | sb.setof.generic |
| UUID | 019dffbb-f000-70f5-b358-f82a3542467b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | setof(...) |
| Return Type Rule | runtime-defined by engine entrypoint setof |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.setof_generic.v3 |
| AST Binding | ast.expr.setof_generic |
| Engine Entrypoint | setof |
| Security Policy | follows engine runtime seed registry authority for rowset.table |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select setof(arg_1) from app.sample_values;
```

### `setof_key_text_value_document`

| Property | Value |
| --- | --- |
| Builtin ID | sb.setof.key_text_value_document |
| UUID | 019dffbb-f000-7cd3-a7bf-a84f1da87607 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | setof_key_text_value_document(...) |
| Return Type Rule | runtime-defined by engine entrypoint setof_key_text_value_document |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.setof_key_text_value_document.v3 |
| AST Binding | ast.expr.setof_key_text_value_document |
| Engine Entrypoint | setof_key_text_value_document |
| Security Policy | follows engine runtime seed registry authority for rowset.table |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select setof_key_text_value_document(arg_1) from app.sample_values;
```

### `setof_key_text_value_text`

| Property | Value |
| --- | --- |
| Builtin ID | sb.setof.key_text_value_text |
| UUID | 019dffbb-f000-7d76-a1e4-ac2ea892d178 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | setof_key_text_value_text(...) |
| Return Type Rule | runtime-defined by engine entrypoint setof_key_text_value_text |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.setof_key_text_value_text.v3 |
| AST Binding | ast.expr.setof_key_text_value_text |
| Engine Entrypoint | setof_key_text_value_text |
| Security Policy | follows engine runtime seed registry authority for rowset.table |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select setof_key_text_value_text(arg_1) from app.sample_values;
```

### `table_value`

| Property | Value |
| --- | --- |
| Builtin ID | sb.table_value.value |
| UUID | 019dffbb-f000-7db1-8865-e6a826478d12 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | table_value(...) |
| Return Type Rule | runtime-defined by engine entrypoint table_value |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.table_value_value.v3 |
| AST Binding | ast.expr.table_value_value |
| Engine Entrypoint | table_value |
| Security Policy | follows engine runtime seed registry authority for rowset.table |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select table_value(arg_1) from app.sample_values;
```

### `table_value_append`

| Property | Value |
| --- | --- |
| Builtin ID | sb.table_value.append |
| UUID | 019dffbb-f000-7d07-aa7b-1f120bd32150 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | table_value_append(...) |
| Return Type Rule | runtime-defined by engine entrypoint table_value_append |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.table_value_append.v3 |
| AST Binding | ast.expr.table_value_append |
| Engine Entrypoint | table_value_append |
| Security Policy | follows engine runtime seed registry authority for rowset.table |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select table_value_append(arg_1) from app.sample_values;
```

### `table_value_new`

| Property | Value |
| --- | --- |
| Builtin ID | sb.table_value.new |
| UUID | 019dffbb-f000-769f-af69-9f68356bf2cf |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | table_value_new(...) |
| Return Type Rule | runtime-defined by engine entrypoint table_value_new |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.table_value_new.v3 |
| AST Binding | ast.expr.table_value_new |
| Engine Entrypoint | table_value_new |
| Security Policy | follows engine runtime seed registry authority for rowset.table |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select table_value_new(arg_1) from app.sample_values;
```

### `unnest`

| Property | Value |
| --- | --- |
| Builtin ID | sb.rowset.unnest |
| UUID | 019dffbb-f000-7713-a8f2-6cda4672b5c5 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | unnest(...) |
| Return Type Rule | runtime-defined by engine entrypoint unnest |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.rowset_unnest.v3 |
| AST Binding | ast.expr.rowset_unnest |
| Engine Entrypoint | unnest |
| Security Policy | follows engine runtime seed registry authority for rowset.table |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select unnest(arg_1) from app.sample_values;
```


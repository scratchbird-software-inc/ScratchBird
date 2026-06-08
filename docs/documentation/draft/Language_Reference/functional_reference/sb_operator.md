# Sb Operator Functional Reference

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `sb_operator`


## Package Boundary

`sb.operator` contains 23 registered built-in operation records. Each record binds a public function, operator, aggregate, window function, or special form to descriptor-aware overloads, null behavior, optimizer properties, SBLR binding, and engine entrypoint metadata.

The package boundary is semantic, not a privilege boundary. A caller still needs object privileges, expression admission, descriptor compatibility, and policy admission for the statement that uses the operation.

For symbolic syntax, precedence, associativity, donor-profile operator aliases, and result descriptor rules, see [../syntax_reference/operators.md](../syntax_reference/operators.md) and [../syntax_reference/operator_type_result_matrix.md](../syntax_reference/operator_type_result_matrix.md).

## Package Inventory

| Kind | Records |
| --- | --- |
| operator | 23 |

## Type Mechanics

- Argument descriptors select the overload.
- Return descriptors come from the declared return type rule.
- Null, collation, charset, timezone, and security behavior are part of binding.
- Optimizer properties may allow constant folding, generated column use, or index eligibility, but execution authority remains with the engine.
- Practical forms below are canonical usage shapes derived from declared overload signatures; statement admission still depends on the surrounding query, object privileges, and descriptor binding.

## Operation Records

### `add`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.add |
| UUID | 019dffbb-f000-736d-a32f-578b52136cfc |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | add(...) |
| Return Type Rule | descriptor-authoritative add operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_add.v3 |
| AST Binding | ast.operator.operator_add |
| Engine Entrypoint | operator_add |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 add value_2 from app.sample_values;
```

### `and`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.and |
| UUID | 019dffbb-f000-7184-9845-fe5b75231c3e |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | and(...) |
| Return Type Rule | descriptor-authoritative and operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_and.v3 |
| AST Binding | ast.operator.operator_and |
| Engine Entrypoint | operator_and |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

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

### `array_contains`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.array_contains |
| UUID | 019dffbb-f000-7e5d-ab20-ae6502c0aef3 |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | array_contains(...) |
| Return Type Rule | descriptor-authoritative array_contains operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_array_contains.v3 |
| AST Binding | ast.operator.operator_array_contains |
| Engine Entrypoint | operator_array_contains |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 array_contains value_2 from app.sample_values;
```

### `divide`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.divide |
| UUID | 019dffbb-f000-7f6c-a735-c88297635dac |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | divide(...) |
| Return Type Rule | descriptor-authoritative divide operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_divide.v3 |
| AST Binding | ast.operator.operator_divide |
| Engine Entrypoint | operator_divide |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 divide value_2 from app.sample_values;
```

### `equal`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.equal |
| UUID | 019dffbb-f000-7bc5-acb4-2d63211337cc |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | equal(...) |
| Return Type Rule | descriptor-authoritative equal operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_equal.v3 |
| AST Binding | ast.operator.operator_equal |
| Engine Entrypoint | operator_equal |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 equal value_2 from app.sample_values;
```

### `greater`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.greater |
| UUID | 019dffbb-f000-7ca9-ae06-ac2539b92994 |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | greater(...) |
| Return Type Rule | descriptor-authoritative greater operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_greater.v3 |
| AST Binding | ast.operator.operator_greater |
| Engine Entrypoint | operator_greater |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 greater value_2 from app.sample_values;
```

### `greater_equal`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.greater_equal |
| UUID | 019dffbb-f000-7ae9-9e20-824a87d2ba62 |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | greater_equal(...) |
| Return Type Rule | descriptor-authoritative greater_equal operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_greater_equal.v3 |
| AST Binding | ast.operator.operator_greater_equal |
| Engine Entrypoint | operator_greater_equal |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 greater_equal value_2 from app.sample_values;
```

### `ilike`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.ilike |
| UUID | 019dffbb-f000-7341-a890-14cb79a0b41a |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | ilike(...) |
| Return Type Rule | descriptor-authoritative ilike operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_ilike.v3 |
| AST Binding | ast.expr.operator_ilike |
| Engine Entrypoint | operator_ilike |
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
select value_1 ilike value_2 from app.sample_values;
```

### `is_distinct_from`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.is_distinct_from |
| UUID | 019dffbb-f000-7af6-8918-d8551dc2f34b |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | is_distinct_from(...) |
| Return Type Rule | descriptor-authoritative is_distinct_from operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_is_distinct_from.v3 |
| AST Binding | ast.operator.operator_is_distinct_from |
| Engine Entrypoint | operator_is_distinct_from |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 is_distinct_from value_2 from app.sample_values;
```

### `json_get`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.json_get |
| UUID | 019dffbb-f000-71ea-9744-3c99eb020678 |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | json_get(...) |
| Return Type Rule | descriptor-authoritative json_get operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_json_get.v3 |
| AST Binding | ast.operator.operator_json_get |
| Engine Entrypoint | operator_json_get |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 json_get value_2 from app.sample_values;
```

### `json_get_text`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.json_get_text |
| UUID | 019dffbb-f000-7bed-b971-c2f027b3dd45 |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | json_get_text(...) |
| Return Type Rule | descriptor-authoritative json_get_text operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_json_get_text.v3 |
| AST Binding | ast.operator.operator_json_get_text |
| Engine Entrypoint | operator_json_get_text |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 json_get_text value_2 from app.sample_values;
```

### `less`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.less |
| UUID | 019dffbb-f000-7fe9-ba37-5a2a95a2c62c |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | less(...) |
| Return Type Rule | descriptor-authoritative less operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_less.v3 |
| AST Binding | ast.operator.operator_less |
| Engine Entrypoint | operator_less |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 less value_2 from app.sample_values;
```

### `less_equal`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.less_equal |
| UUID | 019dffbb-f000-752e-bdd4-0d76297583b4 |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | less_equal(...) |
| Return Type Rule | descriptor-authoritative less_equal operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_less_equal.v3 |
| AST Binding | ast.operator.operator_less_equal |
| Engine Entrypoint | operator_less_equal |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 less_equal value_2 from app.sample_values;
```

### `like`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.like |
| UUID | 019dffbb-f000-7b5f-adfa-d93b9775ebf4 |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | like(...) |
| Return Type Rule | descriptor-authoritative like operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_like.v3 |
| AST Binding | ast.operator.operator_like |
| Engine Entrypoint | operator_like |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

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

### `modulo`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.modulo |
| UUID | 019dffbb-f000-72aa-a8a1-eec01cf5b997 |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | modulo(...) |
| Return Type Rule | descriptor-authoritative modulo operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_modulo.v3 |
| AST Binding | ast.operator.operator_modulo |
| Engine Entrypoint | operator_modulo |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 modulo value_2 from app.sample_values;
```

### `multiply`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.multiply |
| UUID | 019dffbb-f000-736c-97c7-c8250907109f |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | multiply(...) |
| Return Type Rule | descriptor-authoritative multiply operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_multiply.v3 |
| AST Binding | ast.operator.operator_multiply |
| Engine Entrypoint | operator_multiply |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 multiply value_2 from app.sample_values;
```

### `not`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.not |
| UUID | 019dffbb-f000-7513-8e77-83deace81805 |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | not(...) |
| Return Type Rule | descriptor-authoritative not operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_not.v3 |
| AST Binding | ast.operator.operator_not |
| Engine Entrypoint | operator_not |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 not value_2 from app.sample_values;
```

### `not_equal`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.not_equal |
| UUID | 019dffbb-f000-7361-9fd3-f01338e9ee35 |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | not_equal(...) |
| Return Type Rule | descriptor-authoritative not_equal operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_not_equal.v3 |
| AST Binding | ast.operator.operator_not_equal |
| Engine Entrypoint | operator_not_equal |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 not_equal value_2 from app.sample_values;
```

### `or`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.or |
| UUID | 019dffbb-f000-7f93-b028-8161d1eee2f0 |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | or(...) |
| Return Type Rule | descriptor-authoritative or operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_or.v3 |
| AST Binding | ast.operator.operator_or |
| Engine Entrypoint | operator_or |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

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

### `regex_match`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.regex_match |
| UUID | 019dffbb-f000-7272-8555-dc403503398b |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | regex_match(...) |
| Return Type Rule | descriptor-authoritative regex_match operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_regex_match.v3 |
| AST Binding | ast.operator.operator_regex_match |
| Engine Entrypoint | operator_regex_match |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 regex_match value_2 from app.sample_values;
```

### `subtract`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.subtract |
| UUID | 019dffbb-f000-7b48-b1ac-bffb0c75b1e1 |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | subtract(...) |
| Return Type Rule | descriptor-authoritative subtract operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_subtract.v3 |
| AST Binding | ast.operator.operator_subtract |
| Engine Entrypoint | operator_subtract |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 subtract value_2 from app.sample_values;
```

### `unary_minus`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.unary_minus |
| UUID | 019dffbb-f000-7219-a08f-5b2e49ba7e93 |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | unary_minus(...) |
| Return Type Rule | descriptor-authoritative unary_minus operator/function result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for operator/function operands |
| Null Behavior | operator/function-specific NULL and three-valued logic handling follows SBSFC-014 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset for text/pattern operands where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when operands are constants and collation/version dependencies are stable |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_unary_minus.v3 |
| AST Binding | ast.operator.operator_unary_minus |
| Engine Entrypoint | operator_unary_minus |
| Security Policy | none |
| Error Semantics | arity/type/domain errors use builtin error compatibility matrix and SBSFC-014 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | False |
| generated_column_eligible | True |
| cost_class | cpu_operator |

#### Practical Form

```sql
select value_1 unary_minus value_2 from app.sample_values;
```

### `xor`

| Property | Value |
| --- | --- |
| Builtin ID | sb.operator.xor |
| UUID | 019dffbb-f000-724f-8a03-f9ed4772186f |
| Kind | operator |
| Syntax Forms | operator_expression, function_call |
| Overloads | xor(...) |
| Return Type Rule | descriptor-authoritative xor result as implemented by the SBLR expression runtime |
| Coercion Rule | descriptor implicit cast matrix for native function/operator operands |
| Null Behavior | function/operator-specific NULL handling follows SBSFC-026 row evidence |
| Collation/Charset Rule | uses input descriptor collation/charset where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | stable_by_arguments |
| Determinism | foldable only when inputs are constants with stable descriptors and function-specific context is fixed |
| Side Effects | none |
| SBLR Binding | sblr.expr.operator_xor.v3 |
| AST Binding | ast.operator.operator_xor |
| Engine Entrypoint | operator_xor |
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
select value_1 xor value_2 from app.sample_values;
```

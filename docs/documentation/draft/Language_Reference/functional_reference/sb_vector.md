# Sb Vector Functional Reference

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `sb_vector`


## Package Boundary

`sb.vector` contains 14 registered built-in operation records. Each record binds a public function, operator, aggregate, window function, or special form to descriptor-aware overloads, null behavior, optimizer properties, SBLR binding, and engine entrypoint metadata.

The package boundary is semantic, not a privilege boundary. A caller still needs object privileges, expression admission, descriptor compatibility, and policy admission for the statement that uses the operation.

## Package Inventory

| Kind | Records |
| --- | --- |
| scalar | 14 |

## Type Mechanics

- Argument descriptors select the overload.
- Return descriptors come from the declared return type rule.
- Null, collation, charset, timezone, and security behavior are part of binding.
- Optimizer properties may allow constant folding, generated column use, or index eligibility, but execution authority remains with the engine.
- Practical forms below are canonical usage shapes derived from declared overload signatures; statement admission still depends on the surrounding query, object privileges, and descriptor binding.

## Operation Records

### `cosine_distance`

| Property | Value |
| --- | --- |
| Builtin ID | sb.vector.cosine_distance |
| UUID | 019dffbb-f000-78af-951f-945d34ee1ffb |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | cosine_distance(vector,vector) |
| Return Type Rule | return cosine distance between non-zero dense vectors as real64; descriptor=real64 |
| Coercion Rule | use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family |
| Null Behavior | strict null propagation unless function-specific vector diagnostic fires before result materialization |
| Collation/Charset Rule | not text-locale sensitive except for vector text parsing diagnostics |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when all vector arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.vector_cosine_distance.v3 |
| AST Binding | ast.expr.vector_cosine_distance |
| Engine Entrypoint | sb_engine_functions.cosine_distance |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and SBSFC-014 vector exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | specialized_vector |

#### Practical Form

```sql
select cosine_distance(vector_1, vector_2) from app.sample_values;
```

### `hamming_distance`

| Property | Value |
| --- | --- |
| Builtin ID | sb.vector.hamming_distance |
| UUID | 019dffbb-f000-773b-8a09-d06656a63ccf |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | hamming_distance(bit_vector,bit_vector) |
| Return Type Rule | return differing bit count between equal-dimension bit vectors as int64; descriptor=int64 |
| Coercion Rule | use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family |
| Null Behavior | strict null propagation unless function-specific vector diagnostic fires before result materialization |
| Collation/Charset Rule | not text-locale sensitive except for vector text parsing diagnostics |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when all vector arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.vector_hamming_distance.v3 |
| AST Binding | ast.expr.vector_hamming_distance |
| Engine Entrypoint | sb_engine_functions.hamming_distance |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and SBSFC-014 vector exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | specialized_vector |

#### Practical Form

```sql
select hamming_distance(bit_vector_1, bit_vector_2) from app.sample_values;
```

### `inner_product`

| Property | Value |
| --- | --- |
| Builtin ID | sb.vector.inner_product |
| UUID | 019dffbb-f000-7b97-a032-d398c47b436e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | inner_product(vector,vector) |
| Return Type Rule | return dot product between equal-dimension dense vectors as real64; descriptor=real64 |
| Coercion Rule | use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family |
| Null Behavior | strict null propagation unless function-specific vector diagnostic fires before result materialization |
| Collation/Charset Rule | not text-locale sensitive except for vector text parsing diagnostics |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when all vector arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.vector_inner_product.v3 |
| AST Binding | ast.expr.vector_inner_product |
| Engine Entrypoint | sb_engine_functions.inner_product |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and SBSFC-014 vector exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | specialized_vector |

#### Practical Form

```sql
select inner_product(vector_1, vector_2) from app.sample_values;
```

### `l2_distance`

| Property | Value |
| --- | --- |
| Builtin ID | sb.vector.l2_distance |
| UUID | 019dffbb-f000-7dda-a342-4135426035b6 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | l2_distance(vector,vector) |
| Return Type Rule | return Euclidean distance between equal-dimension dense vectors as real64; descriptor=real64 |
| Coercion Rule | use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family |
| Null Behavior | strict null propagation unless function-specific vector diagnostic fires before result materialization |
| Collation/Charset Rule | not text-locale sensitive except for vector text parsing diagnostics |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when all vector arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.vector_l2_distance.v3 |
| AST Binding | ast.expr.vector_l2_distance |
| Engine Entrypoint | sb_engine_functions.l2_distance |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and SBSFC-014 vector exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | specialized_vector |

#### Practical Form

```sql
select l2_distance(vector_1, vector_2) from app.sample_values;
```

### `negative_inner_product`

| Property | Value |
| --- | --- |
| Builtin ID | sb.vector.negative_inner_product |
| UUID | 019dffbb-f000-744d-aaf6-0a5295c07008 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | negative_inner_product(vector,vector) |
| Return Type Rule | return negative dot product between equal-dimension dense vectors as real64; descriptor=real64 |
| Coercion Rule | use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family |
| Null Behavior | strict null propagation unless function-specific vector diagnostic fires before result materialization |
| Collation/Charset Rule | not text-locale sensitive except for vector text parsing diagnostics |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when all vector arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.vector_negative_inner_product.v3 |
| AST Binding | ast.expr.vector_negative_inner_product |
| Engine Entrypoint | sb_engine_functions.negative_inner_product |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and SBSFC-014 vector exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | specialized_vector |

#### Practical Form

```sql
select negative_inner_product(vector_1, vector_2) from app.sample_values;
```

### `subvector`

| Property | Value |
| --- | --- |
| Builtin ID | sb.vector.subvector |
| UUID | 019dffbb-f000-7273-a459-55a3556d74e4 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | subvector(vector,start,length) |
| Return Type Rule | return one-based vector slice by start and length; descriptor=dense_vector |
| Coercion Rule | use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family |
| Null Behavior | strict null propagation unless function-specific vector diagnostic fires before result materialization |
| Collation/Charset Rule | not text-locale sensitive except for vector text parsing diagnostics |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when all vector arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.vector_subvector.v3 |
| AST Binding | ast.expr.vector_subvector |
| Engine Entrypoint | sb_engine_functions.subvector |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and SBSFC-014 vector exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | specialized_vector |

#### Practical Form

```sql
select subvector(vector_1, arg_2, arg_3) from app.sample_values;
```

### `vector`

| Property | Value |
| --- | --- |
| Builtin ID | sb.vector.vector |
| UUID | 019dffbb-f000-76a0-a0af-0994b4cc9f31 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | vector(...) |
| Return Type Rule | construct dense vector from numeric arguments or dense-vector text; descriptor=dense_vector |
| Coercion Rule | use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family |
| Null Behavior | strict null propagation unless function-specific vector diagnostic fires before result materialization |
| Collation/Charset Rule | not text-locale sensitive except for vector text parsing diagnostics |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when all vector arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.vector_construct.v3 |
| AST Binding | ast.expr.vector_construct |
| Engine Entrypoint | sb_engine_functions.vector |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and SBSFC-014 vector exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | specialized_vector |

#### Practical Form

```sql
select vector(arg_1) from app.sample_values;
```

### `vector_avg`

| Property | Value |
| --- | --- |
| Builtin ID | sb.vector.vector_avg |
| UUID | 019dffbb-f000-7a7f-a278-dfeae7140aa1 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | vector_avg(vector) |
| Return Type Rule | return elementwise dense-vector average; descriptor=dense_vector |
| Coercion Rule | use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family |
| Null Behavior | strict null propagation to dense_vector unless function-specific vector diagnostic fires before result materialization |
| Collation/Charset Rule | not text-locale sensitive except for vector text parsing diagnostics |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when all vector arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.vector_avg.v3 |
| AST Binding | ast.expr.vector_avg |
| Engine Entrypoint | sb_engine_functions.vector_avg |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/dimension/domain errors use builtin error compatibility matrix and SBSFC-014 vector exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | specialized_vector |

#### Practical Form

```sql
select vector_avg(vector_1) from app.sample_values;
```

### `vector_cast_float16`

| Property | Value |
| --- | --- |
| Builtin ID | sb.vector.vector_cast_float16 |
| UUID | 019dffbb-f000-7f37-bc17-0848422018f0 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | vector_cast_float16(vector) |
| Return Type Rule | return float16 vector by finite-range half-precision quantization; descriptor=float16_vector |
| Coercion Rule | use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family |
| Null Behavior | strict null propagation unless function-specific vector diagnostic fires before result materialization |
| Collation/Charset Rule | not text-locale sensitive except for vector text parsing diagnostics |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when all vector arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.vector_cast_float16.v3 |
| AST Binding | ast.expr.vector_cast_float16 |
| Engine Entrypoint | sb_engine_functions.vector_cast_float16 |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and SBSFC-014 vector exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | specialized_vector |

#### Practical Form

```sql
select vector_cast_float16(vector_1) from app.sample_values;
```

### `vector_cast_int8`

| Property | Value |
| --- | --- |
| Builtin ID | sb.vector.vector_cast_int8 |
| UUID | 019dffbb-f000-77f0-b4e6-b4c3a7bc3a8c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | vector_cast_int8(vector) |
| Return Type Rule | return int8 vector by rounding and clamping dense-vector elements; descriptor=int8_vector |
| Coercion Rule | use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family |
| Null Behavior | strict null propagation unless function-specific vector diagnostic fires before result materialization |
| Collation/Charset Rule | not text-locale sensitive except for vector text parsing diagnostics |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when all vector arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.vector_cast_int8.v3 |
| AST Binding | ast.expr.vector_cast_int8 |
| Engine Entrypoint | sb_engine_functions.vector_cast_int8 |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and SBSFC-014 vector exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | specialized_vector |

#### Practical Form

```sql
select vector_cast_int8(vector_1) from app.sample_values;
```

### `vector_dims`

| Property | Value |
| --- | --- |
| Builtin ID | sb.vector.vector_dims |
| UUID | 019dffbb-f000-79eb-a2b4-d1c4cd23ce0c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | vector_dims(vector) |
| Return Type Rule | return vector dimensionality as int64; descriptor=int64 |
| Coercion Rule | use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family |
| Null Behavior | strict null propagation unless function-specific vector diagnostic fires before result materialization |
| Collation/Charset Rule | not text-locale sensitive except for vector text parsing diagnostics |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when all vector arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.vector_dims.v3 |
| AST Binding | ast.expr.vector_dims |
| Engine Entrypoint | sb_engine_functions.vector_dims |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and SBSFC-014 vector exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | specialized_vector |

#### Practical Form

```sql
select vector_dims(vector_1) from app.sample_values;
```

### `vector_l2_normalize`

| Property | Value |
| --- | --- |
| Builtin ID | sb.vector.vector_l2_normalize |
| UUID | 019dffbb-f000-7977-a0a5-75471a393bba |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | vector_l2_normalize(vector) |
| Return Type Rule | return unit-length dense vector with L2 norm 1; descriptor=dense_vector |
| Coercion Rule | use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family |
| Null Behavior | strict null propagation unless function-specific vector diagnostic fires before result materialization |
| Collation/Charset Rule | not text-locale sensitive except for vector text parsing diagnostics |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when all vector arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.vector_l2_normalize.v3 |
| AST Binding | ast.expr.vector_l2_normalize |
| Engine Entrypoint | sb_engine_functions.vector_l2_normalize |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and SBSFC-014 vector exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | specialized_vector |

#### Practical Form

```sql
select vector_l2_normalize(vector_1) from app.sample_values;
```

### `vector_norm`

| Property | Value |
| --- | --- |
| Builtin ID | sb.vector.vector_norm |
| UUID | 019dffbb-f000-76c4-b0f0-7dd8729e47ce |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | vector_norm(vector) |
| Return Type Rule | return L2 norm of dense vector as real64; descriptor=real64 |
| Coercion Rule | use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family |
| Null Behavior | strict null propagation unless function-specific vector diagnostic fires before result materialization |
| Collation/Charset Rule | not text-locale sensitive except for vector text parsing diagnostics |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when all vector arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.vector_norm.v3 |
| AST Binding | ast.expr.vector_norm |
| Engine Entrypoint | sb_engine_functions.vector_norm |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and SBSFC-014 vector exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | specialized_vector |

#### Practical Form

```sql
select vector_norm(vector_1) from app.sample_values;
```

### `vector_sum`

| Property | Value |
| --- | --- |
| Builtin ID | sb.vector.vector_sum |
| UUID | 019dffbb-f000-7f59-9fa7-f633e088665e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | vector_sum(vector) |
| Return Type Rule | return elementwise dense-vector sum; descriptor=dense_vector |
| Coercion Rule | use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family |
| Null Behavior | strict null propagation to dense_vector unless function-specific vector diagnostic fires before result materialization |
| Collation/Charset Rule | not text-locale sensitive except for vector text parsing diagnostics |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable when all vector arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.vector_sum.v3 |
| AST Binding | ast.expr.vector_sum |
| Engine Entrypoint | sb_engine_functions.vector_sum |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/dimension/domain errors use builtin error compatibility matrix and SBSFC-014 vector exact fixture evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | specialized_vector |

#### Practical Form

```sql
select vector_sum(vector_1) from app.sample_values;
```


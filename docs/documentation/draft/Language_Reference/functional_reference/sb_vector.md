# SB Vector Functional Reference

Generation task: `sb_vector`

Package namespace: `sb.vector`

Dense vector construction, distance, normalization, dimension, and aggregate helpers.

## How To Read This Page

Creates and compares vector values for vector search and analytic expressions.

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
| scalar | 14 |

## Operation Reference

### `cosine_distance`

**Purpose:** Returns the cosine distance between two equal-dimension vectors.

**Call Forms:**

- `cosine_distance(vector,vector)`
- Syntax category: `function_call`

**Parameters:**

- `vector`: Bound using the declared descriptor rules for this overload.
- `vector`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative vector arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family.
- NULL handling: strict null propagation unless function-specific vector diagnostic fires before result materialization.

**Returns:**

return cosine distance between non-zero dense vectors as real64; descriptor=real64.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all vector arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: not text-locale sensitive except for vector text parsing diagnostics.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and the conformance fixtures vector exact fixture evidence.

**Example:**

```sql
select cosine_distance(vector('[1,0]'), vector('[0,1]')) as distance;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.vector.cosine_distance |
| UUID | 019dffbb-f000-78af-951f-945d34ee1ffb |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.vector_cosine_distance.v3 |
| AST binding | ast.expr.vector_cosine_distance |
| Engine entrypoint | sb_engine_functions.cosine_distance |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | specialized_vector |

Conformance evidence: `SBSFC014-cosine-distance-name;SBSFC014-cosine-distance-null;SBSFC014-cosine-distance-signature;SBSFC014-cosine-distance-zero-refusal`.

### `hamming_distance`

**Purpose:** Returns the Hamming distance between compatible vector or bit-vector values.

**Call Forms:**

- `hamming_distance(bit_vector,bit_vector)`
- Syntax category: `function_call`

**Parameters:**

- `bit_vector`: Bound using the declared descriptor rules for this overload.
- `bit_vector`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative vector arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family.
- NULL handling: strict null propagation unless function-specific vector diagnostic fires before result materialization.

**Returns:**

return differing bit count between equal-dimension bit vectors as int64; descriptor=int64.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all vector arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: not text-locale sensitive except for vector text parsing diagnostics.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and the conformance fixtures vector exact fixture evidence.

**Example:**

```sql
select hamming_distance(vector('[1,0,1]'), vector('[1,1,0]')) as distance;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.vector.hamming_distance |
| UUID | 019dffbb-f000-773b-8a09-d06656a63ccf |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.vector_hamming_distance.v3 |
| AST binding | ast.expr.vector_hamming_distance |
| Engine entrypoint | sb_engine_functions.hamming_distance |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | specialized_vector |

Conformance evidence: `SBSFC014-hamming-distance-dimension-refusal;SBSFC014-hamming-distance-name;SBSFC014-hamming-distance-null;SBSFC014-hamming-distance-signature`.

### `inner_product`

**Purpose:** Returns the inner product of two equal-dimension vectors.

**Call Forms:**

- `inner_product(vector,vector)`
- Syntax category: `function_call`

**Parameters:**

- `vector`: Bound using the declared descriptor rules for this overload.
- `vector`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative vector arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family.
- NULL handling: strict null propagation unless function-specific vector diagnostic fires before result materialization.

**Returns:**

return dot product between equal-dimension dense vectors as real64; descriptor=real64.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all vector arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: not text-locale sensitive except for vector text parsing diagnostics.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and the conformance fixtures vector exact fixture evidence.

**Example:**

```sql
select inner_product(vector('[1,2]'), vector('[3,4]')) as product_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.vector.inner_product |
| UUID | 019dffbb-f000-7b97-a032-d398c47b436e |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.vector_inner_product.v3 |
| AST binding | ast.expr.vector_inner_product |
| Engine entrypoint | sb_engine_functions.inner_product |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | specialized_vector |

Conformance evidence: `SBSFC014-inner-product-name;SBSFC014-inner-product-null;SBSFC014-inner-product-signature`.

### `l2_distance`

**Purpose:** Returns the Euclidean distance between two equal-dimension vectors.

**Call Forms:**

- `l2_distance(vector,vector)`
- Syntax category: `function_call`

**Parameters:**

- `vector`: Bound using the declared descriptor rules for this overload.
- `vector`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative vector arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family.
- NULL handling: strict null propagation unless function-specific vector diagnostic fires before result materialization.

**Returns:**

return Euclidean distance between equal-dimension dense vectors as real64; descriptor=real64.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all vector arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: not text-locale sensitive except for vector text parsing diagnostics.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and the conformance fixtures vector exact fixture evidence.

**Example:**

```sql
select l2_distance(vector('[1,2,3]'), vector('[1,2,4]')) as distance;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.vector.l2_distance |
| UUID | 019dffbb-f000-7dda-a342-4135426035b6 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.vector_l2_distance.v3 |
| AST binding | ast.expr.vector_l2_distance |
| Engine entrypoint | sb_engine_functions.l2_distance |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | specialized_vector |

Conformance evidence: `SBSFC014-l2-distance-dimension-refusal;SBSFC014-l2-distance-name;SBSFC014-l2-distance-null;SBSFC014-l2-distance-signature`.

### `negative_inner_product`

**Purpose:** Returns the negative inner product of two equal-dimension vectors.

**Call Forms:**

- `negative_inner_product(vector,vector)`
- Syntax category: `function_call`

**Parameters:**

- `vector`: Bound using the declared descriptor rules for this overload.
- `vector`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative vector arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family.
- NULL handling: strict null propagation unless function-specific vector diagnostic fires before result materialization.

**Returns:**

return negative dot product between equal-dimension dense vectors as real64; descriptor=real64.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all vector arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: not text-locale sensitive except for vector text parsing diagnostics.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and the conformance fixtures vector exact fixture evidence.

**Example:**

```sql
select negative_inner_product(vector('[1,2]'), vector('[3,4]')) as score;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.vector.negative_inner_product |
| UUID | 019dffbb-f000-744d-aaf6-0a5295c07008 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.vector_negative_inner_product.v3 |
| AST binding | ast.expr.vector_negative_inner_product |
| Engine entrypoint | sb_engine_functions.negative_inner_product |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | specialized_vector |

Conformance evidence: `SBSFC014-negative-inner-product-name;SBSFC014-negative-inner-product-null;SBSFC014-negative-inner-product-signature`.

### `subvector`

**Purpose:** Evaluates `subvector` and returns return one-based vector slice by start and length; descriptor=dense_vector.

**Call Forms:**

- `subvector(vector,start,length)`
- Syntax category: `function_call`

**Parameters:**

- `vector`: Bound using the declared descriptor rules for this overload.
- `start`: Bound using the declared descriptor rules for this overload.
- `length`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative vector arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family.
- NULL handling: strict null propagation unless function-specific vector diagnostic fires before result materialization.

**Returns:**

return one-based vector slice by start and length; descriptor=dense_vector.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all vector arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: not text-locale sensitive except for vector text parsing diagnostics.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and the conformance fixtures vector exact fixture evidence.

**Example:**

```sql
select subvector(value_1, value_2, value_3) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.vector.subvector |
| UUID | 019dffbb-f000-7273-a459-55a3556d74e4 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.vector_subvector.v3 |
| AST binding | ast.expr.vector_subvector |
| Engine entrypoint | sb_engine_functions.subvector |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | specialized_vector |

Conformance evidence: `SBSFC014-subvector-name;SBSFC014-subvector-null;SBSFC014-subvector-range-refusal;SBSFC014-subvector-signature`.

### `vector`

**Purpose:** Constructs or normalizes a vector value from descriptor-supported input.

**Call Forms:**

- `vector(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative vector arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family.
- NULL handling: strict null propagation unless function-specific vector diagnostic fires before result materialization.

**Returns:**

construct dense vector from numeric arguments or dense-vector text; descriptor=dense_vector.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all vector arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: not text-locale sensitive except for vector text parsing diagnostics.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and the conformance fixtures vector exact fixture evidence.

**Example:**

```sql
select vector('[0.25,0.50,0.75]') as embedding;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.vector.vector |
| UUID | 019dffbb-f000-76a0-a0af-0994b4cc9f31 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.vector_construct.v3 |
| AST binding | ast.expr.vector_construct |
| Engine entrypoint | sb_engine_functions.vector |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | specialized_vector |

Conformance evidence: `SBSFC014-vector-empty-refusal;SBSFC014-vector-null;SBSFC014-vector-signature-constructor;SBSFC014-vector-upper-constructor`.

### `vector_avg`

**Purpose:** Aggregates vectors by averaging corresponding dimensions.

**Call Forms:**

- `vector_avg(vector)`
- Syntax category: `function_call`

**Parameters:**

- `vector`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one or more descriptor_authoritative dense-vector arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family.
- NULL handling: strict null propagation to dense_vector unless function-specific vector diagnostic fires before result materialization.

**Returns:**

return elementwise dense-vector average; descriptor=dense_vector.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all vector arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: not text-locale sensitive except for vector text parsing diagnostics.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/dimension/domain errors use builtin error compatibility matrix and the conformance fixtures vector exact fixture evidence.

**Example:**

```sql
select vector_avg(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.vector.vector_avg |
| UUID | 019dffbb-f000-7a7f-a278-dfeae7140aa1 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.vector_avg.v3 |
| AST binding | ast.expr.vector_avg |
| Engine entrypoint | sb_engine_functions.vector_avg |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | specialized_vector |

Conformance evidence: `SBSFC014-vector-avg-name;SBSFC014-vector-avg-null;SBSFC014-vector-avg-signature`.

### `vector_cast_float16`

**Purpose:** Performs the `vector cast float16` vector helper using descriptor-validated vector values.

**Call Forms:**

- `vector_cast_float16(vector)`
- Syntax category: `function_call`

**Parameters:**

- `vector`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative vector arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family.
- NULL handling: strict null propagation unless function-specific vector diagnostic fires before result materialization.

**Returns:**

return float16 vector by finite-range half-precision quantization; descriptor=float16_vector.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all vector arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: not text-locale sensitive except for vector text parsing diagnostics.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and the conformance fixtures vector exact fixture evidence.

**Example:**

```sql
select vector_cast_float16(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.vector.vector_cast_float16 |
| UUID | 019dffbb-f000-7f37-bc17-0848422018f0 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.vector_cast_float16.v3 |
| AST binding | ast.expr.vector_cast_float16 |
| Engine entrypoint | sb_engine_functions.vector_cast_float16 |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | specialized_vector |

Conformance evidence: `SBSFC014-vector-cast-float16-name;SBSFC014-vector-cast-float16-null;SBSFC014-vector-cast-float16-overflow-refusal;SBSFC014-vector-cast-float16-signature`.

### `vector_cast_int8`

**Purpose:** Performs the `vector cast int8` vector helper using descriptor-validated vector values.

**Call Forms:**

- `vector_cast_int8(vector)`
- Syntax category: `function_call`

**Parameters:**

- `vector`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative vector arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family.
- NULL handling: strict null propagation unless function-specific vector diagnostic fires before result materialization.

**Returns:**

return int8 vector by rounding and clamping dense-vector elements; descriptor=int8_vector.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all vector arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: not text-locale sensitive except for vector text parsing diagnostics.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and the conformance fixtures vector exact fixture evidence.

**Example:**

```sql
select vector_cast_int8(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.vector.vector_cast_int8 |
| UUID | 019dffbb-f000-77f0-b4e6-b4c3a7bc3a8c |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.vector_cast_int8.v3 |
| AST binding | ast.expr.vector_cast_int8 |
| Engine entrypoint | sb_engine_functions.vector_cast_int8 |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | specialized_vector |

Conformance evidence: `SBSFC014-vector-cast-int8-name;SBSFC014-vector-cast-int8-null;SBSFC014-vector-cast-int8-signature`.

### `vector_dims`

**Purpose:** Returns the dimension count of a vector value.

**Call Forms:**

- `vector_dims(vector)`
- Syntax category: `function_call`

**Parameters:**

- `vector`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative vector arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family.
- NULL handling: strict null propagation unless function-specific vector diagnostic fires before result materialization.

**Returns:**

return vector dimensionality as int64; descriptor=int64.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all vector arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: not text-locale sensitive except for vector text parsing diagnostics.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and the conformance fixtures vector exact fixture evidence.

**Example:**

```sql
select vector_dims(vector('[1,2,3]')) as dimensions;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.vector.vector_dims |
| UUID | 019dffbb-f000-79eb-a2b4-d1c4cd23ce0c |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.vector_dims.v3 |
| AST binding | ast.expr.vector_dims |
| Engine entrypoint | sb_engine_functions.vector_dims |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | specialized_vector |

Conformance evidence: `SBSFC014-vector-dims-name;SBSFC014-vector-dims-null;SBSFC014-vector-dims-signature`.

### `vector_l2_normalize`

**Purpose:** Returns a vector normalized to unit L2 length.

**Call Forms:**

- `vector_l2_normalize(vector)`
- Syntax category: `function_call`

**Parameters:**

- `vector`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative vector arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family.
- NULL handling: strict null propagation unless function-specific vector diagnostic fires before result materialization.

**Returns:**

return unit-length dense vector with L2 norm 1; descriptor=dense_vector.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all vector arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: not text-locale sensitive except for vector text parsing diagnostics.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and the conformance fixtures vector exact fixture evidence.

**Example:**

```sql
select vector_l2_normalize(vector('[3,4]')) as unit_vector;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.vector.vector_l2_normalize |
| UUID | 019dffbb-f000-7977-a0a5-75471a393bba |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.vector_l2_normalize.v3 |
| AST binding | ast.expr.vector_l2_normalize |
| Engine entrypoint | sb_engine_functions.vector_l2_normalize |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | specialized_vector |

Conformance evidence: `SBSFC014-vector-l2-normalize-name;SBSFC014-vector-l2-normalize-null;SBSFC014-vector-l2-normalize-signature;SBSFC014-vector-l2-normalize-zero-refusal`.

### `vector_norm`

**Purpose:** Returns the norm of a vector value.

**Call Forms:**

- `vector_norm(vector)`
- Syntax category: `function_call`

**Parameters:**

- `vector`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative vector arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family.
- NULL handling: strict null propagation unless function-specific vector diagnostic fires before result materialization.

**Returns:**

return L2 norm of dense vector as real64; descriptor=real64.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all vector arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: not text-locale sensitive except for vector text parsing diagnostics.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/dimension/domain/overflow errors use builtin error compatibility matrix and the conformance fixtures vector exact fixture evidence.

**Example:**

```sql
select vector_norm(vector('[3,4]')) as magnitude;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.vector.vector_norm |
| UUID | 019dffbb-f000-76c4-b0f0-7dd8729e47ce |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.vector_norm.v3 |
| AST binding | ast.expr.vector_norm |
| Engine entrypoint | sb_engine_functions.vector_norm |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | specialized_vector |

Conformance evidence: `SBSFC014-vector-norm-name;SBSFC014-vector-norm-null;SBSFC014-vector-norm-signature`.

### `vector_sum`

**Purpose:** Aggregates vectors by summing corresponding dimensions.

**Call Forms:**

- `vector_sum(vector)`
- Syntax category: `function_call`

**Parameters:**

- `vector`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one or more descriptor_authoritative dense-vector arguments bound by parser/lowering route.
- Coercion: use descriptor implicit cast matrix; vector text and element coercions are runtime-validated by vector function family.
- NULL handling: strict null propagation to dense_vector unless function-specific vector diagnostic fires before result materialization.

**Returns:**

return elementwise dense-vector sum; descriptor=dense_vector.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when all vector arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: not text-locale sensitive except for vector text parsing diagnostics.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/dimension/domain errors use builtin error compatibility matrix and the conformance fixtures vector exact fixture evidence.

**Example:**

```sql
select vector_sum(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.vector.vector_sum |
| UUID | 019dffbb-f000-7f59-9fa7-f633e088665e |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.vector_sum.v3 |
| AST binding | ast.expr.vector_sum |
| Engine entrypoint | sb_engine_functions.vector_sum |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | specialized_vector |

Conformance evidence: `SBSFC014-vector-sum-name;SBSFC014-vector-sum-null;SBSFC014-vector-sum-signature`.

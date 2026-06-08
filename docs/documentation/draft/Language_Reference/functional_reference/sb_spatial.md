# Sb Spatial Functional Reference

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `sb_spatial`


## Package Boundary

`sb.spatial` contains 93 registered built-in operation records. Each record binds a public function, operator, aggregate, window function, or special form to descriptor-aware overloads, null behavior, optimizer properties, SBLR binding, and engine entrypoint metadata.

The package boundary is semantic, not a privilege boundary. A caller still needs object privileges, expression admission, descriptor compatibility, and policy admission for the statement that uses the operation.

## Package Inventory

| Kind | Records |
| --- | --- |
| scalar | 93 |

## Type Mechanics

- Argument descriptors select the overload.
- Return descriptors come from the declared return type rule.
- Null, collation, charset, timezone, and security behavior are part of binding.
- Optimizer properties may allow constant folding, generated column use, or index eligibility, but execution authority remains with the engine.
- Practical forms below are canonical usage shapes derived from declared overload signatures; statement admission still depends on the surrounding query, object privileges, and descriptor binding.

## Operation Records

### `geom_collect`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.geom_collect |
| UUID | 019dffbb-f000-759b-847c-9f3ed02b3742 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | geom_collect(...) |
| Return Type Rule | runtime-defined by engine entrypoint geom_collect |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_geom_collect.v3 |
| AST Binding | ast.expr.scalar_geom_collect |
| Engine Entrypoint | geom_collect |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select geom_collect(arg_1) from app.sample_values;
```

### `geom_collect_geometry`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.geom_collect_geometry |
| UUID | 019dffbb-f002-7012-8a00-000000000012 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | geom_collect_geometry(...) |
| Return Type Rule | runtime-defined by engine entrypoint geom_collect_geometry |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_geom_collect_geometry.v3 |
| AST Binding | ast.expr.scalar_geom_collect_geometry |
| Engine Entrypoint | geom_collect_geometry |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select geom_collect_geometry(arg_1) from app.sample_values;
```

### `geom_extent`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.geom_extent |
| UUID | 019dffbb-f002-7021-8a00-000000000021 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | geom_extent(...) |
| Return Type Rule | runtime-defined by engine entrypoint geom_extent |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_geom_extent.v3 |
| AST Binding | ast.expr.scalar_geom_extent |
| Engine Entrypoint | geom_extent |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select geom_extent(arg_1) from app.sample_values;
```

### `geom_extent_geometry`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.geom_extent_geometry |
| UUID | 019dffbb-f000-7597-a198-f3ea41f8b4c6 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | geom_extent_geometry(...) |
| Return Type Rule | runtime-defined by engine entrypoint geom_extent_geometry |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_geom_extent_geometry.v3 |
| AST Binding | ast.expr.scalar_geom_extent_geometry |
| Engine Entrypoint | geom_extent_geometry |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select geom_extent_geometry(arg_1) from app.sample_values;
```

### `geom_union`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.geom_union |
| UUID | 019dffbb-f002-7011-8a00-000000000011 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | geom_union(...) |
| Return Type Rule | runtime-defined by engine entrypoint geom_union |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_geom_union.v3 |
| AST Binding | ast.expr.scalar_geom_union |
| Engine Entrypoint | geom_union |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select geom_union(arg_1) from app.sample_values;
```

### `geom_union_geometry`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.geom_union_geometry |
| UUID | 019dffbb-f000-799f-9304-e40550ef7024 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | geom_union_geometry(...) |
| Return Type Rule | runtime-defined by engine entrypoint geom_union_geometry |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_geom_union_geometry.v3 |
| AST Binding | ast.expr.scalar_geom_union_geometry |
| Engine Entrypoint | geom_union_geometry |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select geom_union_geometry(arg_1) from app.sample_values;
```

### `st_area`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_area |
| UUID | 019dffbb-f002-7007-8a00-000000000007 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_area(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_area |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_area.v3 |
| AST Binding | ast.expr.scalar_st_area |
| Engine Entrypoint | st_area |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_area(arg_1) from app.sample_values;
```

### `st_area_geometry`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_area_geometry |
| UUID | 019dffbb-f000-7341-91a0-334a4b21864e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_area_geometry(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_area_geometry |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_area_geometry.v3 |
| AST Binding | ast.expr.scalar_st_area_geometry |
| Engine Entrypoint | st_area_geometry |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_area_geometry(arg_1) from app.sample_values;
```

### `st_asbinary`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_asbinary |
| UUID | 019dffbb-f000-7114-9a87-b2411f025c18 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_asbinary(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_asbinary |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_asbinary.v3 |
| AST Binding | ast.expr.scalar_st_asbinary |
| Engine Entrypoint | st_asbinary |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_asbinary(arg_1) from app.sample_values;
```

### `st_asbinary_geometry`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_asbinary_geometry |
| UUID | 019dffbb-f000-77cb-b492-64a374f16a59 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_asbinary_geometry(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_asbinary_geometry |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_asbinary_geometry.v3 |
| AST Binding | ast.expr.scalar_st_asbinary_geometry |
| Engine Entrypoint | st_asbinary_geometry |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_asbinary_geometry(arg_1) from app.sample_values;
```

### `st_asgeojson`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_asgeojson |
| UUID | 019dffbb-f002-7023-8a00-000000000023 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_asgeojson(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_asgeojson |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_asgeojson.v3 |
| AST Binding | ast.expr.scalar_st_asgeojson |
| Engine Entrypoint | st_asgeojson |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_asgeojson(arg_1) from app.sample_values;
```

### `st_asgeojson_geometry_maxdecimaldigits`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_asgeojson_geometry_maxdecimaldigits |
| UUID | 019dffbb-f000-7321-bd11-811d7b30c4ea |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_asgeojson_geometry_maxdecimaldigits(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_asgeojson_geometry_maxdecimaldigits |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_asgeojson_geometry_maxdecimaldigits.v3 |
| AST Binding | ast.expr.scalar_st_asgeojson_geometry_maxdecimaldigits |
| Engine Entrypoint | st_asgeojson_geometry_maxdecimaldigits |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_asgeojson_geometry_maxdecimaldigits(arg_1) from app.sample_values;
```

### `st_asmvtgeom`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_asmvtgeom |
| UUID | 019dffbb-f002-7008-8a00-000000000008 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_asmvtgeom(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_asmvtgeom |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_asmvtgeom.v3 |
| AST Binding | ast.expr.scalar_st_asmvtgeom |
| Engine Entrypoint | st_asmvtgeom |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_asmvtgeom(arg_1) from app.sample_values;
```

### `st_asmvtgeom_geometry_bbox_extent_buffer_clip`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_asmvtgeom_geometry_bbox_extent_buffer_clip |
| UUID | 019dffbb-f002-7043-8a00-000000000043 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_asmvtgeom_geometry_bbox_extent_buffer_clip(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_asmvtgeom_geometry_bbox_extent_buffer_clip |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_asmvtgeom_geometry_bbox_extent_buffer_clip.v3 |
| AST Binding | ast.expr.scalar_st_asmvtgeom_geometry_bbox_extent_buffer_clip |
| Engine Entrypoint | st_asmvtgeom_geometry_bbox_extent_buffer_clip |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_asmvtgeom_geometry_bbox_extent_buffer_clip(arg_1) from app.sample_values;
```

### `st_assvg`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_assvg |
| UUID | 019dffbb-f000-768d-bd20-e5f65f615cb7 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_assvg(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_assvg |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_assvg.v3 |
| AST Binding | ast.expr.scalar_st_assvg |
| Engine Entrypoint | st_assvg |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_assvg(arg_1) from app.sample_values;
```

### `st_assvg_geometry`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_assvg_geometry |
| UUID | 019dffbb-f000-7fef-b05c-27c465af1bd4 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_assvg_geometry(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_assvg_geometry |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_assvg_geometry.v3 |
| AST Binding | ast.expr.scalar_st_assvg_geometry |
| Engine Entrypoint | st_assvg_geometry |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_assvg_geometry(arg_1) from app.sample_values;
```

### `st_astext`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_astext |
| UUID | 019dffbb-f000-7eee-8972-19163693f63b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_astext(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_astext |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_astext.v3 |
| AST Binding | ast.expr.scalar_st_astext |
| Engine Entrypoint | st_astext |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_astext(arg_1) from app.sample_values;
```

### `st_astext_geometry`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_astext_geometry |
| UUID | 019dffbb-f000-7c52-9fd4-74b35cb69d96 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_astext_geometry(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_astext_geometry |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_astext_geometry.v3 |
| AST Binding | ast.expr.scalar_st_astext_geometry |
| Engine Entrypoint | st_astext_geometry |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_astext_geometry(arg_1) from app.sample_values;
```

### `st_buffer`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_buffer |
| UUID | 019dffbb-f000-799a-97a0-73b50e7f1dae |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_buffer(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_buffer |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_buffer.v3 |
| AST Binding | ast.expr.scalar_st_buffer |
| Engine Entrypoint | st_buffer |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_buffer(arg_1) from app.sample_values;
```

### `st_buffer_geometry_distance`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_buffer_geometry_distance |
| UUID | 019dffbb-f000-7569-9eb7-eee7184c8fab |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_buffer_geometry_distance(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_buffer_geometry_distance |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_buffer_geometry_distance.v3 |
| AST Binding | ast.expr.scalar_st_buffer_geometry_distance |
| Engine Entrypoint | st_buffer_geometry_distance |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_buffer_geometry_distance(arg_1) from app.sample_values;
```

### `st_centroid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_centroid |
| UUID | 019dffbb-f000-704c-8f3e-718258778236 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_centroid(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_centroid |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_centroid.v3 |
| AST Binding | ast.expr.scalar_st_centroid |
| Engine Entrypoint | st_centroid |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_centroid(arg_1) from app.sample_values;
```

### `st_centroid_geometry`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_centroid_geometry |
| UUID | 019dffbb-f002-7016-8a00-000000000016 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_centroid_geometry(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_centroid_geometry |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_centroid_geometry.v3 |
| AST Binding | ast.expr.scalar_st_centroid_geometry |
| Engine Entrypoint | st_centroid_geometry |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_centroid_geometry(arg_1) from app.sample_values;
```

### `st_contains`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_contains |
| UUID | 019dffbb-f000-7b07-8acb-89483aca493f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_contains(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_contains |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_contains.v3 |
| AST Binding | ast.expr.scalar_st_contains |
| Engine Entrypoint | st_contains |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_contains(arg_1) from app.sample_values;
```

### `st_contains_g1_g2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_contains_g1_g2 |
| UUID | 019dffbb-f000-7796-9fc8-bc433eb2f949 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_contains_g1_g2(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_contains_g1_g2 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_contains_g1_g2.v3 |
| AST Binding | ast.expr.scalar_st_contains_g1_g2 |
| Engine Entrypoint | st_contains_g1_g2 |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_contains_g1_g2(arg_1) from app.sample_values;
```

### `st_convexhull`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_convexhull |
| UUID | 019dffbb-f002-7032-8a00-000000000032 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_convexhull(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_convexhull |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_convexhull.v3 |
| AST Binding | ast.expr.scalar_st_convexhull |
| Engine Entrypoint | st_convexhull |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_convexhull(arg_1) from app.sample_values;
```

### `st_convexhull_geometry`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_convexhull_geometry |
| UUID | 019dffbb-f002-7030-8a00-000000000030 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_convexhull_geometry(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_convexhull_geometry |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_convexhull_geometry.v3 |
| AST Binding | ast.expr.scalar_st_convexhull_geometry |
| Engine Entrypoint | st_convexhull_geometry |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_convexhull_geometry(arg_1) from app.sample_values;
```

### `st_covers`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_covers |
| UUID | 019dffbb-f002-7039-8a00-000000000039 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_covers(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_covers |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_covers.v3 |
| AST Binding | ast.expr.scalar_st_covers |
| Engine Entrypoint | st_covers |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_covers(arg_1) from app.sample_values;
```

### `st_covers_g1_g2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_covers_g1_g2 |
| UUID | 019dffbb-f002-7027-8a00-000000000027 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_covers_g1_g2(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_covers_g1_g2 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_covers_g1_g2.v3 |
| AST Binding | ast.expr.scalar_st_covers_g1_g2 |
| Engine Entrypoint | st_covers_g1_g2 |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_covers_g1_g2(arg_1) from app.sample_values;
```

### `st_crosses`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_crosses |
| UUID | 019dffbb-f000-79fe-bda9-b3ff8ae8d7db |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_crosses(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_crosses |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_crosses.v3 |
| AST Binding | ast.expr.scalar_st_crosses |
| Engine Entrypoint | st_crosses |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_crosses(arg_1) from app.sample_values;
```

### `st_crosses_g1_g2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_crosses_g1_g2 |
| UUID | 019dffbb-f000-7ead-bdc4-ae2c3d42fbb8 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_crosses_g1_g2(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_crosses_g1_g2 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_crosses_g1_g2.v3 |
| AST Binding | ast.expr.scalar_st_crosses_g1_g2 |
| Engine Entrypoint | st_crosses_g1_g2 |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_crosses_g1_g2(arg_1) from app.sample_values;
```

### `st_difference`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_difference |
| UUID | 019dffbb-f002-7009-8a00-000000000009 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_difference(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_difference |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_difference.v3 |
| AST Binding | ast.expr.scalar_st_difference |
| Engine Entrypoint | st_difference |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_difference(arg_1) from app.sample_values;
```

### `st_difference_g1_g2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_difference_g1_g2 |
| UUID | 019dffbb-f002-7005-8a00-000000000005 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_difference_g1_g2(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_difference_g1_g2 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_difference_g1_g2.v3 |
| AST Binding | ast.expr.scalar_st_difference_g1_g2 |
| Engine Entrypoint | st_difference_g1_g2 |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_difference_g1_g2(arg_1) from app.sample_values;
```

### `st_disjoint`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_disjoint |
| UUID | 019dffbb-f000-700c-8fbd-cea5012faf3f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_disjoint(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_disjoint |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_disjoint.v3 |
| AST Binding | ast.expr.scalar_st_disjoint |
| Engine Entrypoint | st_disjoint |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_disjoint(arg_1) from app.sample_values;
```

### `st_disjoint_g1_g2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_disjoint_g1_g2 |
| UUID | 019dffbb-f002-7029-8a00-000000000029 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_disjoint_g1_g2(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_disjoint_g1_g2 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_disjoint_g1_g2.v3 |
| AST Binding | ast.expr.scalar_st_disjoint_g1_g2 |
| Engine Entrypoint | st_disjoint_g1_g2 |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_disjoint_g1_g2(arg_1) from app.sample_values;
```

### `st_distance`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_distance |
| UUID | 019dffbb-f000-70cc-960f-177f0b06f890 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_distance(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_distance |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_distance.v3 |
| AST Binding | ast.expr.scalar_st_distance |
| Engine Entrypoint | st_distance |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_distance(arg_1) from app.sample_values;
```

### `st_distance_g1_g2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_distance_g1_g2 |
| UUID | 019dffbb-f000-7ae2-a9dd-4381816a119f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_distance_g1_g2(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_distance_g1_g2 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_distance_g1_g2.v3 |
| AST Binding | ast.expr.scalar_st_distance_g1_g2 |
| Engine Entrypoint | st_distance_g1_g2 |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_distance_g1_g2(arg_1) from app.sample_values;
```

### `st_dwithin`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_dwithin |
| UUID | 019dffbb-f002-7024-8a00-000000000024 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_dwithin(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_dwithin |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_dwithin.v3 |
| AST Binding | ast.expr.scalar_st_dwithin |
| Engine Entrypoint | st_dwithin |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_dwithin(arg_1) from app.sample_values;
```

### `st_dwithin_g1_g2_distance`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_dwithin_g1_g2_distance |
| UUID | 019dffbb-f002-7002-8a00-000000000002 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_dwithin_g1_g2_distance(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_dwithin_g1_g2_distance |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_dwithin_g1_g2_distance.v3 |
| AST Binding | ast.expr.scalar_st_dwithin_g1_g2_distance |
| Engine Entrypoint | st_dwithin_g1_g2_distance |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_dwithin_g1_g2_distance(arg_1) from app.sample_values;
```

### `st_envelope`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_envelope |
| UUID | 019dffbb-f000-7801-b48f-c90782cef66c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_envelope(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_envelope |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_envelope.v3 |
| AST Binding | ast.expr.scalar_st_envelope |
| Engine Entrypoint | st_envelope |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_envelope(arg_1) from app.sample_values;
```

### `st_envelope_geometry`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_envelope_geometry |
| UUID | 019dffbb-f000-7db8-b038-25c899a67993 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_envelope_geometry(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_envelope_geometry |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_envelope_geometry.v3 |
| AST Binding | ast.expr.scalar_st_envelope_geometry |
| Engine Entrypoint | st_envelope_geometry |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_envelope_geometry(arg_1) from app.sample_values;
```

### `st_equals`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_equals |
| UUID | 019dffbb-f002-7014-8a00-000000000014 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_equals(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_equals |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_equals.v3 |
| AST Binding | ast.expr.scalar_st_equals |
| Engine Entrypoint | st_equals |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_equals(arg_1) from app.sample_values;
```

### `st_equals_g1_g2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_equals_g1_g2 |
| UUID | 019dffbb-f000-7bd7-939c-9670142ff560 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_equals_g1_g2(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_equals_g1_g2 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_equals_g1_g2.v3 |
| AST Binding | ast.expr.scalar_st_equals_g1_g2 |
| Engine Entrypoint | st_equals_g1_g2 |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_equals_g1_g2(arg_1) from app.sample_values;
```

### `st_geogfromtext`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geogfromtext |
| UUID | 019dffbb-f000-7868-ac24-4d0a09a7d7c0 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_geogfromtext(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_geogfromtext |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_geogfromtext.v3 |
| AST Binding | ast.expr.scalar_st_geogfromtext |
| Engine Entrypoint | st_geogfromtext |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_geogfromtext(arg_1) from app.sample_values;
```

### `st_geogfromtext_wkt`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geogfromtext_wkt |
| UUID | 019dffbb-f002-7041-8a00-000000000041 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_geogfromtext_wkt(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_geogfromtext_wkt |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_geogfromtext_wkt.v3 |
| AST Binding | ast.expr.scalar_st_geogfromtext_wkt |
| Engine Entrypoint | st_geogfromtext_wkt |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_geogfromtext_wkt(arg_1) from app.sample_values;
```

### `st_geometrytype`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geometrytype |
| UUID | 019dffbb-f002-7017-8a00-000000000017 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_geometrytype(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_geometrytype |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_geometrytype.v3 |
| AST Binding | ast.expr.scalar_st_geometrytype |
| Engine Entrypoint | st_geometrytype |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_geometrytype(arg_1) from app.sample_values;
```

### `st_geometrytype_geometry`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geometrytype_geometry |
| UUID | 019dffbb-f000-77dd-ac54-85767abb9926 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_geometrytype_geometry(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_geometrytype_geometry |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_geometrytype_geometry.v3 |
| AST Binding | ast.expr.scalar_st_geometrytype_geometry |
| Engine Entrypoint | st_geometrytype_geometry |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_geometrytype_geometry(arg_1) from app.sample_values;
```

### `st_geomfromgeojson`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geomfromgeojson |
| UUID | 019dffbb-f002-7018-8a00-000000000018 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_geomfromgeojson(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_geomfromgeojson |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_geomfromgeojson.v3 |
| AST Binding | ast.expr.scalar_st_geomfromgeojson |
| Engine Entrypoint | st_geomfromgeojson |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_geomfromgeojson(arg_1) from app.sample_values;
```

### `st_geomfromgeojson_text`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geomfromgeojson_text |
| UUID | 019dffbb-f000-72b5-b56a-b7993e3a9844 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_geomfromgeojson_text(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_geomfromgeojson_text |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_geomfromgeojson_text.v3 |
| AST Binding | ast.expr.scalar_st_geomfromgeojson_text |
| Engine Entrypoint | st_geomfromgeojson_text |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_geomfromgeojson_text(arg_1) from app.sample_values;
```

### `st_geomfromtext`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geomfromtext |
| UUID | 019dffbb-f002-7036-8a00-000000000036 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_geomfromtext(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_geomfromtext |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_geomfromtext.v3 |
| AST Binding | ast.expr.scalar_st_geomfromtext |
| Engine Entrypoint | st_geomfromtext |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_geomfromtext(arg_1) from app.sample_values;
```

### `st_geomfromtext_wkt_srid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geomfromtext_wkt_srid |
| UUID | 019dffbb-f002-7020-8a00-000000000020 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_geomfromtext_wkt_srid(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_geomfromtext_wkt_srid |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_geomfromtext_wkt_srid.v3 |
| AST Binding | ast.expr.scalar_st_geomfromtext_wkt_srid |
| Engine Entrypoint | st_geomfromtext_wkt_srid |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_geomfromtext_wkt_srid(arg_1) from app.sample_values;
```

### `st_geomfromwkb`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geomfromwkb |
| UUID | 019dffbb-f000-76ee-a6f7-dcfc7057e7e9 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_geomfromwkb(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_geomfromwkb |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_geomfromwkb.v3 |
| AST Binding | ast.expr.scalar_st_geomfromwkb |
| Engine Entrypoint | st_geomfromwkb |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_geomfromwkb(arg_1) from app.sample_values;
```

### `st_geomfromwkb_wkb_srid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_geomfromwkb_wkb_srid |
| UUID | 019dffbb-f000-7b44-a0c1-6bcaca00d38a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_geomfromwkb_wkb_srid(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_geomfromwkb_wkb_srid |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_geomfromwkb_wkb_srid.v3 |
| AST Binding | ast.expr.scalar_st_geomfromwkb_wkb_srid |
| Engine Entrypoint | st_geomfromwkb_wkb_srid |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_geomfromwkb_wkb_srid(arg_1) from app.sample_values;
```

### `st_intersection`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_intersection |
| UUID | 019dffbb-f000-7c44-ae0e-b6225140db16 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_intersection(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_intersection |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_intersection.v3 |
| AST Binding | ast.expr.scalar_st_intersection |
| Engine Entrypoint | st_intersection |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_intersection(arg_1) from app.sample_values;
```

### `st_intersection_g1_g2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_intersection_g1_g2 |
| UUID | 019dffbb-f002-7015-8a00-000000000015 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_intersection_g1_g2(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_intersection_g1_g2 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_intersection_g1_g2.v3 |
| AST Binding | ast.expr.scalar_st_intersection_g1_g2 |
| Engine Entrypoint | st_intersection_g1_g2 |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_intersection_g1_g2(arg_1) from app.sample_values;
```

### `st_intersects`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_intersects |
| UUID | 019dffbb-f000-776b-a82f-edc543ad8574 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_intersects(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_intersects |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_intersects.v3 |
| AST Binding | ast.expr.scalar_st_intersects |
| Engine Entrypoint | st_intersects |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_intersects(arg_1) from app.sample_values;
```

### `st_intersects_g1_g2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_intersects_g1_g2 |
| UUID | 019dffbb-f000-70c2-86a8-116036bfee0a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_intersects_g1_g2(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_intersects_g1_g2 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_intersects_g1_g2.v3 |
| AST Binding | ast.expr.scalar_st_intersects_g1_g2 |
| Engine Entrypoint | st_intersects_g1_g2 |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_intersects_g1_g2(arg_1) from app.sample_values;
```

### `st_length`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_length |
| UUID | 019dffbb-f002-7010-8a00-000000000010 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_length(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_length |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_length.v3 |
| AST Binding | ast.expr.scalar_st_length |
| Engine Entrypoint | st_length |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_length(arg_1) from app.sample_values;
```

### `st_length_geometry`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_length_geometry |
| UUID | 019dffbb-f002-7031-8a00-000000000031 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_length_geometry(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_length_geometry |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_length_geometry.v3 |
| AST Binding | ast.expr.scalar_st_length_geometry |
| Engine Entrypoint | st_length_geometry |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_length_geometry(arg_1) from app.sample_values;
```

### `st_m`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_m |
| UUID | 019dffbb-f002-7003-8a00-000000000003 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_m(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_m |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_m.v3 |
| AST Binding | ast.expr.scalar_st_m |
| Engine Entrypoint | st_m |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_m(arg_1) from app.sample_values;
```

### `st_makeline`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_makeline |
| UUID | 019dffbb-f002-7034-8a00-000000000034 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_makeline(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_makeline |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_makeline.v3 |
| AST Binding | ast.expr.scalar_st_makeline |
| Engine Entrypoint | st_makeline |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_makeline(arg_1) from app.sample_values;
```

### `st_makeline_point_point_array`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_makeline_point_point_array |
| UUID | 019dffbb-f002-7019-8a00-000000000019 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_makeline_point_point_array(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_makeline_point_point_array |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_makeline_point_point_array.v3 |
| AST Binding | ast.expr.scalar_st_makeline_point_point_array |
| Engine Entrypoint | st_makeline_point_point_array |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_makeline_point_point_array(arg_1) from app.sample_values;
```

### `st_makepoint`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_makepoint |
| UUID | 019dffbb-f000-7dc2-b9e7-8a2bbe620fd2 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_makepoint(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_makepoint |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_makepoint.v3 |
| AST Binding | ast.expr.scalar_st_makepoint |
| Engine Entrypoint | st_makepoint |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_makepoint(arg_1) from app.sample_values;
```

### `st_makepoint_x_y_z_m`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_makepoint_x_y_z_m |
| UUID | 019dffbb-f002-7013-8a00-000000000013 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_makepoint_x_y_z_m(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_makepoint_x_y_z_m |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_makepoint_x_y_z_m.v3 |
| AST Binding | ast.expr.scalar_st_makepoint_x_y_z_m |
| Engine Entrypoint | st_makepoint_x_y_z_m |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_makepoint_x_y_z_m(arg_1) from app.sample_values;
```

### `st_makepolygon`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_makepolygon |
| UUID | 019dffbb-f002-7035-8a00-000000000035 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_makepolygon(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_makepolygon |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_makepolygon.v3 |
| AST Binding | ast.expr.scalar_st_makepolygon |
| Engine Entrypoint | st_makepolygon |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_makepolygon(arg_1) from app.sample_values;
```

### `st_makepolygon_linestring_holesarray`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_makepolygon_linestring_holesarray |
| UUID | 019dffbb-f000-7d31-8c78-4b381463878a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_makepolygon_linestring_holesarray(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_makepolygon_linestring_holesarray |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_makepolygon_linestring_holesarray.v3 |
| AST Binding | ast.expr.scalar_st_makepolygon_linestring_holesarray |
| Engine Entrypoint | st_makepolygon_linestring_holesarray |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_makepolygon_linestring_holesarray(arg_1) from app.sample_values;
```

### `st_npoints`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_npoints |
| UUID | 019dffbb-f000-7cf2-ad7c-7d46b446d664 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_npoints(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_npoints |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_npoints.v3 |
| AST Binding | ast.expr.scalar_st_npoints |
| Engine Entrypoint | st_npoints |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_npoints(arg_1) from app.sample_values;
```

### `st_npoints_geometry`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_npoints_geometry |
| UUID | 019dffbb-f002-7033-8a00-000000000033 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_npoints_geometry(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_npoints_geometry |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_npoints_geometry.v3 |
| AST Binding | ast.expr.scalar_st_npoints_geometry |
| Engine Entrypoint | st_npoints_geometry |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_npoints_geometry(arg_1) from app.sample_values;
```

### `st_numpoints`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_numpoints |
| UUID | 019dffbb-f000-7c6d-bf11-10d57aa799e0 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_numpoints(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_numpoints |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_numpoints.v3 |
| AST Binding | ast.expr.scalar_st_numpoints |
| Engine Entrypoint | st_numpoints |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_numpoints(arg_1) from app.sample_values;
```

### `st_numpoints_geometry`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_numpoints_geometry |
| UUID | 019dffbb-f000-7daf-9cac-8bc57ad35954 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_numpoints_geometry(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_numpoints_geometry |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_numpoints_geometry.v3 |
| AST Binding | ast.expr.scalar_st_numpoints_geometry |
| Engine Entrypoint | st_numpoints_geometry |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_numpoints_geometry(arg_1) from app.sample_values;
```

### `st_overlaps`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_overlaps |
| UUID | 019dffbb-f000-78be-81ec-61a6d1cc9c74 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_overlaps(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_overlaps |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_overlaps.v3 |
| AST Binding | ast.expr.scalar_st_overlaps |
| Engine Entrypoint | st_overlaps |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_overlaps(arg_1) from app.sample_values;
```

### `st_overlaps_g1_g2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_overlaps_g1_g2 |
| UUID | 019dffbb-f002-7004-8a00-000000000004 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_overlaps_g1_g2(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_overlaps_g1_g2 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_overlaps_g1_g2.v3 |
| AST Binding | ast.expr.scalar_st_overlaps_g1_g2 |
| Engine Entrypoint | st_overlaps_g1_g2 |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_overlaps_g1_g2(arg_1) from app.sample_values;
```

### `st_perimeter`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_perimeter |
| UUID | 019dffbb-f000-7f31-a657-c857f289e81c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_perimeter(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_perimeter |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_perimeter.v3 |
| AST Binding | ast.expr.scalar_st_perimeter |
| Engine Entrypoint | st_perimeter |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_perimeter(arg_1) from app.sample_values;
```

### `st_perimeter_geometry`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_perimeter_geometry |
| UUID | 019dffbb-f000-7dcd-8c79-2345c86a1ff1 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_perimeter_geometry(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_perimeter_geometry |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_perimeter_geometry.v3 |
| AST Binding | ast.expr.scalar_st_perimeter_geometry |
| Engine Entrypoint | st_perimeter_geometry |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_perimeter_geometry(arg_1) from app.sample_values;
```

### `st_setsrid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_setsrid |
| UUID | 019dffbb-f002-7001-8a00-000000000001 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_setsrid(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_setsrid |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_setsrid.v3 |
| AST Binding | ast.expr.scalar_st_setsrid |
| Engine Entrypoint | st_setsrid |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_setsrid(arg_1) from app.sample_values;
```

### `st_setsrid_geometry_srid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_setsrid_geometry_srid |
| UUID | 019dffbb-f000-7b35-b88b-e0324f8946ee |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_setsrid_geometry_srid(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_setsrid_geometry_srid |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_setsrid_geometry_srid.v3 |
| AST Binding | ast.expr.scalar_st_setsrid_geometry_srid |
| Engine Entrypoint | st_setsrid_geometry_srid |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_setsrid_geometry_srid(arg_1) from app.sample_values;
```

### `st_simplify`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_simplify |
| UUID | 019dffbb-f000-7bd7-97f4-f66f545606cd |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_simplify(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_simplify |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_simplify.v3 |
| AST Binding | ast.expr.scalar_st_simplify |
| Engine Entrypoint | st_simplify |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_simplify(arg_1) from app.sample_values;
```

### `st_simplify_geometry_tolerance`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_simplify_geometry_tolerance |
| UUID | 019dffbb-f000-774b-a0c7-2289630c180c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_simplify_geometry_tolerance(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_simplify_geometry_tolerance |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_simplify_geometry_tolerance.v3 |
| AST Binding | ast.expr.scalar_st_simplify_geometry_tolerance |
| Engine Entrypoint | st_simplify_geometry_tolerance |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_simplify_geometry_tolerance(arg_1) from app.sample_values;
```

### `st_srid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_srid |
| UUID | 019dffbb-f002-7028-8a00-000000000028 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_srid(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_srid |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_srid.v3 |
| AST Binding | ast.expr.scalar_st_srid |
| Engine Entrypoint | st_srid |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_srid(arg_1) from app.sample_values;
```

### `st_srid_geometry`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_srid_geometry |
| UUID | 019dffbb-f000-7043-ad9b-036862729b51 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_srid_geometry(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_srid_geometry |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_srid_geometry.v3 |
| AST Binding | ast.expr.scalar_st_srid_geometry |
| Engine Entrypoint | st_srid_geometry |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_srid_geometry(arg_1) from app.sample_values;
```

### `st_symdifference`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_symdifference |
| UUID | 019dffbb-f002-7038-8a00-000000000038 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_symdifference(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_symdifference |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_symdifference.v3 |
| AST Binding | ast.expr.scalar_st_symdifference |
| Engine Entrypoint | st_symdifference |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_symdifference(arg_1) from app.sample_values;
```

### `st_symdifference_g1_g2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_symdifference_g1_g2 |
| UUID | 019dffbb-f002-7022-8a00-000000000022 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_symdifference_g1_g2(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_symdifference_g1_g2 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_symdifference_g1_g2.v3 |
| AST Binding | ast.expr.scalar_st_symdifference_g1_g2 |
| Engine Entrypoint | st_symdifference_g1_g2 |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_symdifference_g1_g2(arg_1) from app.sample_values;
```

### `st_touches`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_touches |
| UUID | 019dffbb-f002-7025-8a00-000000000025 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_touches(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_touches |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_touches.v3 |
| AST Binding | ast.expr.scalar_st_touches |
| Engine Entrypoint | st_touches |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_touches(arg_1) from app.sample_values;
```

### `st_touches_g1_g2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_touches_g1_g2 |
| UUID | 019dffbb-f000-7293-a7b2-2c424b503190 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_touches_g1_g2(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_touches_g1_g2 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_touches_g1_g2.v3 |
| AST Binding | ast.expr.scalar_st_touches_g1_g2 |
| Engine Entrypoint | st_touches_g1_g2 |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_touches_g1_g2(arg_1) from app.sample_values;
```

### `st_transform`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_transform |
| UUID | 019dffbb-f002-7026-8a00-000000000026 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_transform(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_transform |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_transform.v3 |
| AST Binding | ast.expr.scalar_st_transform |
| Engine Entrypoint | st_transform |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_transform(arg_1) from app.sample_values;
```

### `st_transform_geometry_target_srid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_transform_geometry_target_srid |
| UUID | 019dffbb-f000-717b-b421-4f72a9d57a45 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_transform_geometry_target_srid(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_transform_geometry_target_srid |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_transform_geometry_target_srid.v3 |
| AST Binding | ast.expr.scalar_st_transform_geometry_target_srid |
| Engine Entrypoint | st_transform_geometry_target_srid |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_transform_geometry_target_srid(arg_1) from app.sample_values;
```

### `st_union`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_union |
| UUID | 019dffbb-f002-7040-8a00-000000000040 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_union(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_union |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_union.v3 |
| AST Binding | ast.expr.scalar_st_union |
| Engine Entrypoint | st_union |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_union(arg_1) from app.sample_values;
```

### `st_union_g1_g2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_union_g1_g2 |
| UUID | 019dffbb-f002-7042-8a00-000000000042 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_union_g1_g2(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_union_g1_g2 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_union_g1_g2.v3 |
| AST Binding | ast.expr.scalar_st_union_g1_g2 |
| Engine Entrypoint | st_union_g1_g2 |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_union_g1_g2(arg_1) from app.sample_values;
```

### `st_within`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_within |
| UUID | 019dffbb-f002-7037-8a00-000000000037 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_within(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_within |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_within.v3 |
| AST Binding | ast.expr.scalar_st_within |
| Engine Entrypoint | st_within |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_within(arg_1) from app.sample_values;
```

### `st_within_g1_g2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_within_g1_g2 |
| UUID | 019dffbb-f000-7684-bd0e-dfd71b44d2f7 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_within_g1_g2(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_within_g1_g2 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_within_g1_g2.v3 |
| AST Binding | ast.expr.scalar_st_within_g1_g2 |
| Engine Entrypoint | st_within_g1_g2 |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_within_g1_g2(arg_1) from app.sample_values;
```

### `st_x`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_x |
| UUID | 019dffbb-f000-7273-aa82-e684d9b32184 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_x(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_x |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_x.v3 |
| AST Binding | ast.expr.scalar_st_x |
| Engine Entrypoint | st_x |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_x(arg_1) from app.sample_values;
```

### `st_x_point`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_x_point |
| UUID | 019dffbb-f000-749e-a8e4-ecd9f14ac67f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_x_point(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_x_point |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_x_point.v3 |
| AST Binding | ast.expr.scalar_st_x_point |
| Engine Entrypoint | st_x_point |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_x_point(arg_1) from app.sample_values;
```

### `st_y`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_y |
| UUID | 019dffbb-f000-7dfa-b709-97ed7fc33a6c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_y(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_y |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_y.v3 |
| AST Binding | ast.expr.scalar_st_y |
| Engine Entrypoint | st_y |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_y(arg_1) from app.sample_values;
```

### `st_z`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.st_z |
| UUID | 019dffbb-f002-7006-8a00-000000000006 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | st_z(...) |
| Return Type Rule | runtime-defined by engine entrypoint st_z |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.scalar_st_z.v3 |
| AST Binding | ast.expr.scalar_st_z |
| Engine Entrypoint | st_z |
| Security Policy | follows engine runtime seed registry authority for spatial |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select st_z(arg_1) from app.sample_values;
```


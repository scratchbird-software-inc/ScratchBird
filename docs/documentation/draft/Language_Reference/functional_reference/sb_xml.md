# Sb Xml Functional Reference

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `sb_xml`


## Package Boundary

`sb.xml` contains 20 registered built-in operation records. Each record binds a public function, operator, aggregate, window function, or special form to descriptor-aware overloads, null behavior, optimizer properties, SBLR binding, and engine entrypoint metadata.

The package boundary is semantic, not a privilege boundary. A caller still needs object privileges, expression admission, descriptor compatibility, and policy admission for the statement that uses the operation.

## Package Inventory

| Kind | Records |
| --- | --- |
| scalar | 20 |

## Type Mechanics

- Argument descriptors select the overload.
- Return descriptors come from the declared return type rule.
- Null, collation, charset, timezone, and security behavior are part of binding.
- Optimizer properties may allow constant folding, generated column use, or index eligibility, but execution authority remains with the engine.
- Practical forms below are canonical usage shapes derived from declared overload signatures; statement admission still depends on the surrounding query, object privileges, and descriptor binding.

## Operation Records

### `xml_attrs`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.attrs |
| UUID | 019dffbb-f003-7008-8a00-000000000008 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xml_attrs(...) |
| Return Type Rule | runtime-defined by engine entrypoint xml_attrs |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_attrs.v3 |
| AST Binding | ast.expr.xml_attrs |
| Engine Entrypoint | xml_attrs |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xml_attrs(arg_1) from app.sample_values;
```

### `xml_ns`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.ns |
| UUID | 019dffbb-f003-7009-8a00-000000000009 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xml_ns(...) |
| Return Type Rule | runtime-defined by engine entrypoint xml_ns |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_ns.v3 |
| AST Binding | ast.expr.xml_ns |
| Engine Entrypoint | xml_ns |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xml_ns(arg_1) from app.sample_values;
```

### `xmlagg`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.agg |
| UUID | 019dffbb-f000-7b98-9ff9-18050f0ac8ac |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmlagg(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmlagg |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_agg.v3 |
| AST Binding | ast.expr.xml_agg |
| Engine Entrypoint | xmlagg |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmlagg(arg_1) from app.sample_values;
```

### `xmlattributes`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.attributes |
| UUID | 019dffbb-f000-7403-9b7d-be5431e418ed |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmlattributes(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmlattributes |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_attributes.v3 |
| AST Binding | ast.expr.xml_attributes |
| Engine Entrypoint | xmlattributes |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmlattributes(arg_1) from app.sample_values;
```

### `xmlcast`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.cast |
| UUID | 019dffbb-f000-7b48-afa6-134b032a0e11 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmlcast(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmlcast |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_cast.v3 |
| AST Binding | ast.expr.xml_cast |
| Engine Entrypoint | xmlcast |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmlcast(arg_1) from app.sample_values;
```

### `xmlcomment`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.comment |
| UUID | 019dffbb-f000-7830-a706-102315c7a34a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmlcomment(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmlcomment |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_comment.v3 |
| AST Binding | ast.expr.xml_comment |
| Engine Entrypoint | xmlcomment |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmlcomment(arg_1) from app.sample_values;
```

### `xmlconcat`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.concat |
| UUID | 019dffbb-f000-7970-825c-8ace8ab8b77e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmlconcat(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmlconcat |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_concat.v3 |
| AST Binding | ast.expr.xml_concat |
| Engine Entrypoint | xmlconcat |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmlconcat(arg_1) from app.sample_values;
```

### `xmldocument`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.document |
| UUID | 019dffbb-f003-7001-8a00-000000000001 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmldocument(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmldocument |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_document.v3 |
| AST Binding | ast.expr.xml_document |
| Engine Entrypoint | xmldocument |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmldocument(arg_1) from app.sample_values;
```

### `xmlelement`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.element |
| UUID | 019dffbb-f000-7ecd-ac64-30f2e5814c94 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmlelement(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmlelement |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_element.v3 |
| AST Binding | ast.expr.xml_element |
| Engine Entrypoint | xmlelement |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmlelement(arg_1) from app.sample_values;
```

### `xmlexists`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.exists |
| UUID | 019dffbb-f000-7f8e-8d18-5a968f021f07 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmlexists(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmlexists |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_exists.v3 |
| AST Binding | ast.expr.xml_exists |
| Engine Entrypoint | xmlexists |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmlexists(arg_1) from app.sample_values;
```

### `xmlforest`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.forest |
| UUID | 019dffbb-f000-770d-ad5a-9652416b8eb8 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmlforest(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmlforest |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_forest.v3 |
| AST Binding | ast.expr.xml_forest |
| Engine Entrypoint | xmlforest |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmlforest(arg_1) from app.sample_values;
```

### `xmlnamespaces`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.namespaces |
| UUID | 019dffbb-f003-7002-8a00-000000000002 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmlnamespaces(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmlnamespaces |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_namespaces.v3 |
| AST Binding | ast.expr.xml_namespaces |
| Engine Entrypoint | xmlnamespaces |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmlnamespaces(arg_1) from app.sample_values;
```

### `xmlparse`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.parse |
| UUID | 019dffbb-f003-7003-8a00-000000000003 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmlparse(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmlparse |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_parse.v3 |
| AST Binding | ast.expr.xml_parse |
| Engine Entrypoint | xmlparse |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmlparse(arg_1) from app.sample_values;
```

### `xmlpi`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.pi |
| UUID | 019dffbb-f000-7c5d-9d3d-92c0b78fb021 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmlpi(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmlpi |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_pi.v3 |
| AST Binding | ast.expr.xml_pi |
| Engine Entrypoint | xmlpi |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmlpi(arg_1) from app.sample_values;
```

### `xmlquery`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.query |
| UUID | 019dffbb-f003-7004-8a00-000000000004 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmlquery(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmlquery |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_query.v3 |
| AST Binding | ast.expr.xml_query |
| Engine Entrypoint | xmlquery |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmlquery(arg_1) from app.sample_values;
```

### `xmlroot`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.root |
| UUID | 019dffbb-f000-771b-9c03-aea00ea3719e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmlroot(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmlroot |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_root.v3 |
| AST Binding | ast.expr.xml_root |
| Engine Entrypoint | xmlroot |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmlroot(arg_1) from app.sample_values;
```

### `xmlserialize`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.serialize |
| UUID | 019dffbb-f003-7005-8a00-000000000005 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmlserialize(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmlserialize |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_serialize.v3 |
| AST Binding | ast.expr.xml_serialize |
| Engine Entrypoint | xmlserialize |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmlserialize(arg_1) from app.sample_values;
```

### `xmltable`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.table |
| UUID | 019dffbb-f000-7f47-8a74-4acf46ce946c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmltable(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmltable |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_table.v3 |
| AST Binding | ast.expr.xml_table |
| Engine Entrypoint | xmltable |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmltable(arg_1) from app.sample_values;
```

### `xmltext`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.text |
| UUID | 019dffbb-f003-7006-8a00-000000000006 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmltext(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmltext |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_text.v3 |
| AST Binding | ast.expr.xml_text |
| Engine Entrypoint | xmltext |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmltext(arg_1) from app.sample_values;
```

### `xmlvalidate`

| Property | Value |
| --- | --- |
| Builtin ID | sb.xml.validate |
| UUID | 019dffbb-f003-7007-8a00-000000000007 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xmlvalidate(...) |
| Return Type Rule | runtime-defined by engine entrypoint xmlvalidate |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.xml_validate.v3 |
| AST Binding | ast.expr.xml_validate |
| Engine Entrypoint | xmlvalidate |
| Security Policy | follows engine runtime seed registry authority for nosql.document |
| Error Semantics | engine runtime guard diagnostics |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | runtime_seed |

#### Practical Form

```sql
select xmlvalidate(arg_1) from app.sample_values;
```


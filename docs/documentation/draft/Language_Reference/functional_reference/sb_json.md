# Sb Json Functional Reference

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `sb_json`


## Package Boundary

`sb.json` contains 44 registered built-in operation records. Each record binds a public function, operator, aggregate, window function, or special form to descriptor-aware overloads, null behavior, optimizer properties, SBLR binding, and engine entrypoint metadata.

The package boundary is semantic, not a privilege boundary. A caller still needs object privileges, expression admission, descriptor compatibility, and policy admission for the statement that uses the operation.

## Package Inventory

| Kind | Records |
| --- | --- |
| aggregate | 5 |
| scalar | 39 |

## Type Mechanics

- Argument descriptors select the overload.
- Return descriptors come from the declared return type rule.
- Null, collation, charset, timezone, and security behavior are part of binding.
- Optimizer properties may allow constant folding, generated column use, or index eligibility, but execution authority remains with the engine.
- Practical forms below are canonical usage shapes derived from declared overload signatures; statement admission still depends on the surrounding query, object privileges, and descriptor binding.

## Operation Records

### `json_agg`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.json_agg |
| UUID | 019dffbb-f000-76b7-8855-aa3a860b130f |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | json_agg(...) |
| Return Type Rule | descriptor-authoritative json_agg aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_json_agg.v3 |
| AST Binding | ast.aggregate.aggregate_json_agg |
| Engine Entrypoint | aggregate_json_agg |
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
select json_agg(arg_1) from app.orders group by account_id;
```

### `json_agg`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.json_agg |
| UUID | 019dffbb-f001-7021-8a00-000000000023 |
| Kind | aggregate |
| Syntax Forms | aggregate_call, aggregate_order_by_when_allowed |
| Overloads | json_agg(expr ORDER BY expr) |
| Return Type Rule | json array; empty groups return NULL json |
| Coercion Rule | descriptor payloads are converted to JSON scalar representation |
| Null Behavior | includes_null |
| Collation/Charset Rule | text input is JSON string escaped under descriptor text bytes |
| Timezone Rule | not applicable unless temporal argument descriptors require future JSON rendering rules |
| Volatility | stable_per_group |
| Determinism | deterministic only for parser-provided ordered input sequence |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_json_agg.v3 |
| AST Binding | ast.aggregate.aggregate_json_agg |
| Engine Entrypoint | aggregate_json_agg |
| Security Policy | inherits containing query rights |
| Error Semantics | DISTINCT, FILTER, window bridge, WITHIN GROUP, unordered grouped-route spelling, and generic aggregate options are refused by the current bounded route before payload authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select json_agg(value_1) from app.orders group by account_id;
```

### `json_object_agg`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.json_object_agg |
| UUID | 019dffbb-f000-7d2c-8833-cde197e21a7f |
| Kind | aggregate |
| Syntax Forms | aggregate_function_call |
| Overloads | json_object_agg(...) |
| Return Type Rule | descriptor-authoritative json_object_agg aggregate result as implemented by the SBLR aggregate/window runtime |
| Coercion Rule | descriptor implicit cast matrix |
| Null Behavior | aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures |
| Collation/Charset Rule | uses input descriptor collation/charset for comparable or text aggregate states when applicable |
| Timezone Rule | not applicable unless temporal argument descriptors require it |
| Volatility | immutable |
| Determinism | deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_json_object_agg.v3 |
| AST Binding | ast.aggregate.aggregate_json_object_agg |
| Engine Entrypoint | aggregate_json_object_agg |
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
select json_object_agg(arg_1) from app.orders group by account_id;
```

### `json_object_agg`

| Property | Value |
| --- | --- |
| Builtin ID | sb.aggregate.json_object_agg |
| UUID | 019dffbb-f001-7021-8a00-000000000024 |
| Kind | aggregate |
| Syntax Forms | aggregate_call, aggregate_order_by_when_allowed |
| Overloads | json_object_agg(key,value ORDER BY expr) |
| Return Type Rule | json object; empty groups return NULL json |
| Coercion Rule | key is converted to JSON object member text; value is converted to JSON scalar representation |
| Null Behavior | NULL key is refused with canonical diagnostic before/at engine route; NULL value is emitted as JSON null |
| Collation/Charset Rule | text input is JSON string escaped under descriptor text bytes |
| Timezone Rule | not applicable unless temporal argument descriptors require future JSON rendering rules |
| Volatility | stable_per_group |
| Determinism | deterministic only for parser-provided ordered input sequence |
| Side Effects | none |
| SBLR Binding | sblr.expr.aggregate_json_object_agg.v3 |
| AST Binding | ast.aggregate.aggregate_json_object_agg |
| Engine Entrypoint | aggregate_json_object_agg |
| Security Policy | inherits containing query rights |
| Error Semantics | DISTINCT, FILTER, window bridge, WITHIN GROUP, unordered grouped-route spelling, broad ORDER BY modifiers, generic aggregate options, and NULL keys are refused by the current bounded route before or at payload authority |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | False |
| index_eligible | False |
| generated_column_eligible | False |
| cost_class | aggregate_state |

#### Practical Form

```sql
select json_object_agg(arg_1, value_2) from app.orders group by account_id;
```

### `jsonb_agg`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_agg |
| UUID | 019dffbb-f000-769f-9eac-f2622414c13f |
| Kind | aggregate |
| Syntax Forms | function_call |
| Overloads | jsonb_agg(...) |
| Return Type Rule | runtime-defined by engine entrypoint jsonb_agg |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.json_jsonb_agg.v3 |
| AST Binding | ast.expr.json_jsonb_agg |
| Engine Entrypoint | jsonb_agg |
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
select jsonb_agg(arg_1) from app.orders group by account_id;
```

### `array_to_json`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.array_to_json |
| UUID | 019dffbb-f000-7b62-a578-bf3c272ee775 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | array_to_json(...) |
| Return Type Rule | runtime-defined by engine entrypoint array_to_json |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.json_array_to_json.v3 |
| AST Binding | ast.expr.json_array_to_json |
| Engine Entrypoint | array_to_json |
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
select array_to_json(arg_1) from app.sample_values;
```

### `json_array_elements`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.array_elements |
| UUID | 019dffbb-f000-7766-9c2a-b4fb9b51cf4a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_array_elements(...) |
| Return Type Rule | descriptor-authoritative json_array_elements JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_array_elements.v3 |
| AST Binding | ast.expr.json_array_elements |
| Engine Entrypoint | json_array_elements |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select json_array_elements(arg_1) from app.sample_values;
```

### `json_array_elements_text`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.array_elements_text |
| UUID | 019dffbb-f000-71c1-af8c-5fa2c2706299 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_array_elements_text(...) |
| Return Type Rule | descriptor-authoritative json_array_elements_text JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_array_elements_text.v3 |
| AST Binding | ast.expr.json_array_elements_text |
| Engine Entrypoint | json_array_elements_text |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select json_array_elements_text(arg_1) from app.sample_values;
```

### `json_array_length`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.array_length |
| UUID | 019e18f0-1300-7007-9f01-000000000013 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_array_length(...) |
| Return Type Rule | descriptor-authoritative json_array_length JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_array_length.v3 |
| AST Binding | ast.expr.json_array_length |
| Engine Entrypoint | json_array_length |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select json_array_length(arg_1) from app.sample_values;
```

### `json_build_array`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.build_array |
| UUID | 019e18f0-1300-7008-9f01-000000000013 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_build_array(...) |
| Return Type Rule | descriptor-authoritative json_build_array JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_build_array.v3 |
| AST Binding | ast.expr.json_build_array |
| Engine Entrypoint | json_build_array |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select json_build_array(arg_1) from app.sample_values;
```

### `json_build_object`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.build_object |
| UUID | 019e18f0-1300-7009-9f01-000000000013 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_build_object(...) |
| Return Type Rule | descriptor-authoritative json_build_object JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_build_object.v3 |
| AST Binding | ast.expr.json_build_object |
| Engine Entrypoint | json_build_object |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select json_build_object(arg_1) from app.sample_values;
```

### `json_each`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.each |
| UUID | 019dffbb-f000-7f4b-bc9c-e5514af42232 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_each(...) |
| Return Type Rule | descriptor-authoritative json_each JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_each.v3 |
| AST Binding | ast.expr.json_each |
| Engine Entrypoint | json_each |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select json_each(arg_1) from app.sample_values;
```

### `json_each_text`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.each_text |
| UUID | 019dffbb-f000-7781-ac0e-16252382545b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_each_text(...) |
| Return Type Rule | descriptor-authoritative json_each_text JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_each_text.v3 |
| AST Binding | ast.expr.json_each_text |
| Engine Entrypoint | json_each_text |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select json_each_text(arg_1) from app.sample_values;
```

### `json_exists`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.exists |
| UUID | 019e18f0-1300-7001-9f01-000000000013 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_exists(...) |
| Return Type Rule | descriptor-authoritative json_exists JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_exists.v3 |
| AST Binding | ast.expr.json_exists |
| Engine Entrypoint | json_exists |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select json_exists(arg_1) from app.sample_values;
```

### `json_extract`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.extract |
| UUID | 019de5fc-2400-7e24-878d-0b67cc0acc2f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_extract(document,path) |
| Return Type Rule | JSON/path extraction |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous SBsql coercion unless SBsql policy allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_extract.v3 |
| AST Binding | ast.expr.json_extract |
| Engine Entrypoint | json_extract |
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
select json_extract(document_1, arg_2) from app.sample_values;
```

### `json_insert`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.insert |
| UUID | 019dffbb-f000-77f8-afca-320e3246bd12 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_insert(...) |
| Return Type Rule | descriptor-authoritative json_insert JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_insert.v3 |
| AST Binding | ast.expr.json_insert |
| Engine Entrypoint | json_insert |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select json_insert(arg_1) from app.sample_values;
```

### `json_object`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.object |
| UUID | 019dffbb-f000-7069-9983-2ef3fb6e5c81 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_object(...) |
| Return Type Rule | descriptor-authoritative json_object JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_object.v3 |
| AST Binding | ast.expr.json_object |
| Engine Entrypoint | json_object |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select json_object(arg_1) from app.sample_values;
```

### `json_object_keys`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.object_keys |
| UUID | 019dffbb-f000-70b8-8258-c6287a9048e5 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_object_keys(...) |
| Return Type Rule | descriptor-authoritative json_object_keys JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_object_keys.v3 |
| AST Binding | ast.expr.json_object_keys |
| Engine Entrypoint | json_object_keys |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select json_object_keys(arg_1) from app.sample_values;
```

### `json_object_text_array`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.object_text_array |
| UUID | 019dffbb-f000-7ca5-b599-7ba03c7f2c8d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_object_text_array(...) |
| Return Type Rule | runtime-defined by engine entrypoint json_object_text_array |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.json_object_text_array.v3 |
| AST Binding | ast.expr.json_object_text_array |
| Engine Entrypoint | json_object_text_array |
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
select json_object_text_array(arg_1) from app.sample_values;
```

### `json_query`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.query |
| UUID | 019e18f0-1300-7003-9f01-000000000013 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_query(...) |
| Return Type Rule | descriptor-authoritative json_query JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_query.v3 |
| AST Binding | ast.expr.json_query |
| Engine Entrypoint | json_query |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select json_query(arg_1) from app.sample_values;
```

### `json_remove`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.remove |
| UUID | 019e18f0-1300-7005-9f01-000000000013 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_remove(...) |
| Return Type Rule | descriptor-authoritative json_remove JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_remove.v3 |
| AST Binding | ast.expr.json_remove |
| Engine Entrypoint | json_remove |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select json_remove(arg_1) from app.sample_values;
```

### `json_replace`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.replace |
| UUID | 019e18f0-1300-7006-9f01-000000000013 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_replace(...) |
| Return Type Rule | descriptor-authoritative json_replace JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_replace.v3 |
| AST Binding | ast.expr.json_replace |
| Engine Entrypoint | json_replace |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select json_replace(arg_1) from app.sample_values;
```

### `json_set`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.set |
| UUID | 019e18f0-1300-7004-9f01-000000000013 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_set(...) |
| Return Type Rule | descriptor-authoritative json_set JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_set.v3 |
| AST Binding | ast.expr.json_set |
| Engine Entrypoint | json_set |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select json_set(arg_1) from app.sample_values;
```

### `json_table`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.table |
| UUID | 019dffbb-f000-7800-af0a-cb636c6fb514 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_table(...) |
| Return Type Rule | runtime-defined by engine entrypoint json_table |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.json_table.v3 |
| AST Binding | ast.expr.json_table |
| Engine Entrypoint | json_table |
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
select json_table(arg_1) from app.sample_values;
```

### `json_typeof`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.typeof |
| UUID | 019de5fc-2400-7d02-adb4-da4a6570dc6e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_typeof(value) |
| Return Type Rule | JSON type classifier |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous SBsql coercion unless SBsql policy allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_typeof.v3 |
| AST Binding | ast.expr.json_typeof |
| Engine Entrypoint | json_typeof |
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
select json_typeof(value_1) from app.sample_values;
```

### `json_value`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.value |
| UUID | 019e18f0-1300-7002-9f01-000000000013 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | json_value(...) |
| Return Type Rule | descriptor-authoritative json_value JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.json_value.v3 |
| AST Binding | ast.expr.json_value |
| Engine Entrypoint | json_value |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select json_value(arg_1) from app.sample_values;
```

### `jsonb_array_length`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_array_length |
| UUID | 019dffbb-f000-789c-bede-bf3de23f5365 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jsonb_array_length(...) |
| Return Type Rule | descriptor-authoritative jsonb_array_length JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.jsonb_array_length.v3 |
| AST Binding | ast.expr.jsonb_array_length |
| Engine Entrypoint | jsonb_array_length |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jsonb_array_length(arg_1) from app.sample_values;
```

### `jsonb_build_array`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_build_array |
| UUID | 019dffbb-f000-794b-af4b-ef5ded9604c6 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jsonb_build_array(...) |
| Return Type Rule | descriptor-authoritative jsonb_build_array JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.jsonb_build_array.v3 |
| AST Binding | ast.expr.jsonb_build_array |
| Engine Entrypoint | jsonb_build_array |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jsonb_build_array(arg_1) from app.sample_values;
```

### `jsonb_build_object`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_build_object |
| UUID | 019dffbb-f000-7bd9-9a2b-4e7fa4678978 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jsonb_build_object(...) |
| Return Type Rule | descriptor-authoritative jsonb_build_object JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.jsonb_build_object.v3 |
| AST Binding | ast.expr.jsonb_build_object |
| Engine Entrypoint | jsonb_build_object |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jsonb_build_object(arg_1) from app.sample_values;
```

### `jsonb_insert`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_insert |
| UUID | 019dffbb-f000-73b2-93d0-ac97321ecaaf |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jsonb_insert(...) |
| Return Type Rule | descriptor-authoritative jsonb_insert JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.jsonb_insert.v3 |
| AST Binding | ast.expr.jsonb_insert |
| Engine Entrypoint | jsonb_insert |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jsonb_insert(arg_1) from app.sample_values;
```

### `jsonb_object`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_object |
| UUID | 019dffbb-f000-724d-a766-7e894b545539 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jsonb_object(...) |
| Return Type Rule | descriptor-authoritative jsonb_object JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.jsonb_object.v3 |
| AST Binding | ast.expr.jsonb_object |
| Engine Entrypoint | jsonb_object |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jsonb_object(arg_1) from app.sample_values;
```

### `jsonb_object_keys`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_object_keys |
| UUID | 019dffbb-f000-77d0-b41e-8377357107cb |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jsonb_object_keys(...) |
| Return Type Rule | descriptor-authoritative jsonb_object_keys JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.jsonb_object_keys.v3 |
| AST Binding | ast.expr.jsonb_object_keys |
| Engine Entrypoint | jsonb_object_keys |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jsonb_object_keys(arg_1) from app.sample_values;
```

### `jsonb_path_exists`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_path_exists |
| UUID | 019dffbb-f000-7ea2-9bcc-674a1eb9a59b |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jsonb_path_exists(...) |
| Return Type Rule | descriptor-authoritative jsonb_path_exists JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.jsonb_path_exists.v3 |
| AST Binding | ast.expr.jsonb_path_exists |
| Engine Entrypoint | jsonb_path_exists |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jsonb_path_exists(arg_1) from app.sample_values;
```

### `jsonb_path_match`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_path_match |
| UUID | 019dffbb-f000-742b-a0b9-608e0e2e5c90 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jsonb_path_match(...) |
| Return Type Rule | descriptor-authoritative jsonb_path_match JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.jsonb_path_match.v3 |
| AST Binding | ast.expr.jsonb_path_match |
| Engine Entrypoint | jsonb_path_match |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jsonb_path_match(arg_1) from app.sample_values;
```

### `jsonb_path_query`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_path_query |
| UUID | 019dffbb-f000-7f6b-afdc-960616319c88 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jsonb_path_query(...) |
| Return Type Rule | descriptor-authoritative jsonb_path_query JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.jsonb_path_query.v3 |
| AST Binding | ast.expr.jsonb_path_query |
| Engine Entrypoint | jsonb_path_query |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jsonb_path_query(arg_1) from app.sample_values;
```

### `jsonb_path_query_array`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_path_query_array |
| UUID | 019dffbb-f000-7525-830b-255572ed5a0f |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jsonb_path_query_array(...) |
| Return Type Rule | descriptor-authoritative jsonb_path_query_array JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.jsonb_path_query_array.v3 |
| AST Binding | ast.expr.jsonb_path_query_array |
| Engine Entrypoint | jsonb_path_query_array |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jsonb_path_query_array(arg_1) from app.sample_values;
```

### `jsonb_path_query_first`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_path_query_first |
| UUID | 019dffbb-f000-7432-8ac5-75c7e22f4d15 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jsonb_path_query_first(...) |
| Return Type Rule | descriptor-authoritative jsonb_path_query_first JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.jsonb_path_query_first.v3 |
| AST Binding | ast.expr.jsonb_path_query_first |
| Engine Entrypoint | jsonb_path_query_first |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jsonb_path_query_first(arg_1) from app.sample_values;
```

### `jsonb_pretty`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_pretty |
| UUID | 019dffbb-f000-7b1b-b1d7-013acfdcb345 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jsonb_pretty(...) |
| Return Type Rule | descriptor-authoritative jsonb_pretty JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.jsonb_pretty.v3 |
| AST Binding | ast.expr.jsonb_pretty |
| Engine Entrypoint | jsonb_pretty |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jsonb_pretty(arg_1) from app.sample_values;
```

### `jsonb_set`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_set |
| UUID | 019dffbb-f000-7655-ae18-a53f11c240f5 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jsonb_set(...) |
| Return Type Rule | descriptor-authoritative jsonb_set JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.jsonb_set.v3 |
| AST Binding | ast.expr.jsonb_set |
| Engine Entrypoint | jsonb_set |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jsonb_set(arg_1) from app.sample_values;
```

### `jsonb_strip_nulls`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_strip_nulls |
| UUID | 019dffbb-f000-72dd-bab3-ac7237116ff6 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jsonb_strip_nulls(...) |
| Return Type Rule | descriptor-authoritative jsonb_strip_nulls JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.jsonb_strip_nulls.v3 |
| AST Binding | ast.expr.jsonb_strip_nulls |
| Engine Entrypoint | jsonb_strip_nulls |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jsonb_strip_nulls(arg_1) from app.sample_values;
```

### `jsonb_typeof`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_typeof |
| UUID | 019e18f0-1300-7010-9f01-000000000013 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | jsonb_typeof(...) |
| Return Type Rule | descriptor-authoritative jsonb_typeof JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.jsonb_typeof.v3 |
| AST Binding | ast.expr.jsonb_typeof |
| Engine Entrypoint | jsonb_typeof |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select jsonb_typeof(arg_1) from app.sample_values;
```

### `row_to_json`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.row_to_json |
| UUID | 019dffbb-f000-7b93-87c6-0a817949054a |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | row_to_json(...) |
| Return Type Rule | runtime-defined by engine entrypoint row_to_json |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.json_row_to_json.v3 |
| AST Binding | ast.expr.json_row_to_json |
| Engine Entrypoint | row_to_json |
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
select row_to_json(arg_1) from app.sample_values;
```

### `to_json`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.to_json |
| UUID | 019dffbb-f000-771d-b937-1fafd31b87c0 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | to_json(...) |
| Return Type Rule | descriptor-authoritative to_json JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.to_json.v3 |
| AST Binding | ast.expr.to_json |
| Engine Entrypoint | to_json |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select to_json(arg_1) from app.sample_values;
```

### `to_jsonb`

| Property | Value |
| --- | --- |
| Builtin ID | sb.json.to_jsonb |
| UUID | 019dffbb-f000-7d72-91b3-6cc44f83bc70 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | to_jsonb(...) |
| Return Type Rule | descriptor-authoritative to_jsonb JSON/document scalar result as implemented by the SBLR document expression runtime |
| Coercion Rule | descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments |
| Null Behavior | strict unless JSON/document function-specific semantics or SBSFC-013 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset for JSON path/text values where character semantics apply |
| Timezone Rule | not applicable |
| Volatility | immutable |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.to_jsonb.v3 |
| AST Binding | ast.expr.to_jsonb |
| Engine Entrypoint | to_jsonb |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/path/document-format errors use builtin error compatibility matrix and SBSFC-013 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select to_jsonb(arg_1) from app.sample_values;
```


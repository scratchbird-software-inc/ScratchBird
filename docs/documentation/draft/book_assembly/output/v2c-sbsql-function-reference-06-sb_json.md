

===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/functional_reference/sb_json.md -->

<a id="ch-language-reference-functional-reference-sb-json-md"></a>

# SB JSON Functional Reference

Generation task: `sb_json`

Package namespace: `sb.json`

JSON and JSONB construction, extraction, path, aggregation, and table-shaping helpers.

## How To Read This Page

Constructs, inspects, modifies, and aggregates JSON values under descriptor-aware JSON rules.

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
| aggregate | 5 |
| scalar | 39 |

## Operation Reference

### `json_agg`

**Purpose:** Aggregates input rows into a JSON array.

**Call Forms:**

- `json_agg(...)`
- Syntax category: `aggregate_function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative aggregate arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix.
- NULL handling: aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures.

**Returns:**

descriptor-authoritative json_agg aggregate result as implemented by the SBLR aggregate/window runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for comparable or text aggregate states when applicable.
- Timezone: not applicable unless temporal argument descriptors require it.
- Security and authority: none unless source expressions read session/security/system metadata.

**Errors:**

aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector.

**Example:**

```sql
select json_agg(order_id order by order_id) from app.orders group by account_id;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.aggregate.json_agg |
| UUID | 019dffbb-f000-76b7-8855-aa3a860b130f |
| Kind | aggregate |
| Syntax forms | aggregate_function_call |
| SBLR binding | sblr.expr.aggregate_json_agg.v3 |
| AST binding | ast.aggregate.aggregate_json_agg |
| Engine entrypoint | aggregate_json_agg |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | aggregate_state |

Conformance evidence: `SBSFC015_AGGREGATE_WINDOW_FIXTURES`.

### `json_agg`

**Purpose:** Aggregates input rows into a JSON array.

**Call Forms:**

- `json_agg(expr ORDER BY expr)`
- Syntax category: `aggregate_call, aggregate_order_by_when_allowed`

**Parameters:**

- `expr ORDER BY expr`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative aggregate arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix.
- NULL handling: aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures.

**Returns:**

descriptor-authoritative json_agg aggregate result as implemented by the SBLR aggregate/window runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for comparable or text aggregate states when applicable.
- Timezone: not applicable unless temporal argument descriptors require it.
- Security and authority: none unless source expressions read session/security/system metadata.

**Errors:**

aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector.

**Example:**

```sql
select json_agg(order_id order by order_id) from app.orders group by account_id;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.aggregate.json_agg |
| UUID | 019dffbb-f001-7021-8a00-000000000023 |
| Kind | aggregate |
| Syntax forms | aggregate_call, aggregate_order_by_when_allowed |
| SBLR binding | sblr.expr.aggregate_json_agg.v3 |
| AST binding | ast.aggregate.aggregate_json_agg |
| Engine entrypoint | aggregate_json_agg |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | aggregate_state |

Conformance evidence: `SBSFC015_AGGREGATE_WINDOW_FIXTURES`.

### `json_object_agg`

**Purpose:** Aggregates key/value input rows into a JSON object.

**Call Forms:**

- `json_object_agg(...)`
- Syntax category: `aggregate_function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative aggregate arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix.
- NULL handling: aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures.

**Returns:**

descriptor-authoritative json_object_agg aggregate result as implemented by the SBLR aggregate/window runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for comparable or text aggregate states when applicable.
- Timezone: not applicable unless temporal argument descriptors require it.
- Security and authority: none unless source expressions read session/security/system metadata.

**Errors:**

aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector.

**Example:**

```sql
select json_object_agg(order_id, status) from app.orders group by account_id;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.aggregate.json_object_agg |
| UUID | 019dffbb-f000-7d2c-8833-cde197e21a7f |
| Kind | aggregate |
| Syntax forms | aggregate_function_call |
| SBLR binding | sblr.expr.aggregate_json_object_agg.v3 |
| AST binding | ast.aggregate.aggregate_json_object_agg |
| Engine entrypoint | aggregate_json_object_agg |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | aggregate_state |

Conformance evidence: `SBSFC015_AGGREGATE_WINDOW_FIXTURES`.

### `json_object_agg`

**Purpose:** Aggregates key/value input rows into a JSON object.

**Call Forms:**

- `json_object_agg(key,value ORDER BY expr)`
- Syntax category: `aggregate_call, aggregate_order_by_when_allowed`

**Parameters:**

- `key`: Bound using the declared descriptor rules for this overload.
- `value ORDER BY expr`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative aggregate arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix.
- NULL handling: aggregate-specific NULL handling follows the SBLR aggregate/window runtime and row evidence fixtures.

**Returns:**

descriptor-authoritative json_object_agg aggregate result as implemented by the SBLR aggregate/window runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for stable MGA snapshot input and deterministic parser-provided ordering where required.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for comparable or text aggregate states when applicable.
- Timezone: not applicable unless temporal argument descriptors require it.
- Security and authority: none unless source expressions read session/security/system metadata.

**Errors:**

aggregate arity/type/domain errors use the aggregate/window runtime diagnostic vector.

**Example:**

```sql
select json_object_agg(order_id, status) from app.orders group by account_id;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.aggregate.json_object_agg |
| UUID | 019dffbb-f001-7021-8a00-000000000024 |
| Kind | aggregate |
| Syntax forms | aggregate_call, aggregate_order_by_when_allowed |
| SBLR binding | sblr.expr.aggregate_json_object_agg.v3 |
| AST binding | ast.aggregate.aggregate_json_object_agg |
| Engine entrypoint | aggregate_json_object_agg |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | aggregate_state |

Conformance evidence: `SBSFC015_AGGREGATE_WINDOW_FIXTURES`.

### `jsonb_agg`

**Purpose:** Performs the JSONB form of the `agg` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `jsonb_agg(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: zero or more scalar harness arguments.
- Coercion: JSON document inputs are bounded UTF-8 document scalars; scalar inputs are converted through the existing JSON literal conversion rules.
- NULL handling: zero supplied rows in the scalar harness returns an empty JSON array.

**Returns:**

json_document array with SQL NULL arguments represented as JSON null.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied JSON/scalar inputs.
- Side effects: none.
- Collation/charset: JSON text is emitted as UTF-8 json_document with deterministic key/order preservation for supplied arguments.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR nosql.document runtime using bounded in-core JSON helpers; no parser SQL, external execution, storage scan, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, invalid pretty flag, or malformed text-array json_object input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select jsonb_agg(amount) as result_value from app.orders group by account_id;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_agg |
| UUID | 019dffbb-f000-769f-9eac-f2622414c13f |
| Kind | aggregate |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_jsonb_agg.v3 |
| AST binding | ast.expr.json_jsonb_agg |
| Engine entrypoint | jsonb_agg |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC052-jsonb-agg`.

### `array_to_json`

**Purpose:** Evaluates `array_to_json` and returns json_document array.

**Call Forms:**

- `array_to_json(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: one JSON array/document, array-like descriptor, or scalar argument.
- Coercion: JSON document inputs are bounded UTF-8 document scalars; scalar inputs are converted through the existing JSON literal conversion rules.
- NULL handling: SQL NULL input returns SQL NULL json_document.

**Returns:**

json_document array.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied JSON/scalar inputs.
- Side effects: none.
- Collation/charset: JSON text is emitted as UTF-8 json_document with deterministic key/order preservation for supplied arguments.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR nosql.document runtime using bounded in-core JSON helpers; no parser SQL, external execution, storage scan, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, invalid pretty flag, or malformed text-array json_object input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select array_to_json(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.array_to_json |
| UUID | 019dffbb-f000-7b62-a578-bf3c272ee775 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_array_to_json.v3 |
| AST binding | ast.expr.json_array_to_json |
| Engine entrypoint | array_to_json |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC052-array-to-json`.

### `json_array_elements`

**Purpose:** Performs the `array elements` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `json_array_elements(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative json_array_elements JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select json_array_elements(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.array_elements |
| UUID | 019dffbb-f000-7766-9c2a-b4fb9b51cf4a |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_array_elements.v3 |
| AST binding | ast.expr.json_array_elements |
| Engine entrypoint | json_array_elements |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `json_array_elements_text`

**Purpose:** Performs the `array elements text` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `json_array_elements_text(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative json_array_elements_text JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select json_array_elements_text(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.array_elements_text |
| UUID | 019dffbb-f000-71c1-af8c-5fa2c2706299 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_array_elements_text.v3 |
| AST binding | ast.expr.json_array_elements_text |
| Engine entrypoint | json_array_elements_text |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `json_array_length`

**Purpose:** Returns the number of elements in a JSON array.

**Call Forms:**

- `json_array_length(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative json_array_length JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select json_array_length(cast('[1,2,3]' as json_document)) as item_count;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.array_length |
| UUID | 019e18f0-1300-7007-9f01-000000000013 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_array_length.v3 |
| AST binding | ast.expr.json_array_length |
| Engine entrypoint | json_array_length |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `json_build_array`

**Purpose:** Builds a JSON array from the supplied arguments.

**Call Forms:**

- `json_build_array(args...)`
- Syntax category: `function_call`

**Parameters:**

- `args...`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative json_build_array JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select json_build_array(1, 2, 3) as document_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.build_array |
| UUID | 019e18f0-1300-7008-9f01-000000000013 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_build_array.v3 |
| AST binding | ast.expr.json_build_array |
| Engine entrypoint | json_build_array |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `json_build_object`

**Purpose:** Builds a JSON object from alternating key and value arguments.

**Call Forms:**

- `json_build_object(k1,v1,...)`
- Syntax category: `function_call`

**Parameters:**

- `k1`: Bound using the declared descriptor rules for this overload.
- `v1`: Bound using the declared descriptor rules for this overload.
- `...`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative json_build_object JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select json_build_object('name', 'Ada', 'active', true) as document_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.build_object |
| UUID | 019e18f0-1300-7009-9f01-000000000013 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_build_object.v3 |
| AST binding | ast.expr.json_build_object |
| Engine entrypoint | json_build_object |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `json_each`

**Purpose:** Performs the `each` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `json_each(document)`
- Syntax category: `function_call`

**Parameters:**

- `document`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative json_each JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select json_each(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.each |
| UUID | 019dffbb-f000-7f4b-bc9c-e5514af42232 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_each.v3 |
| AST binding | ast.expr.json_each |
| Engine entrypoint | json_each |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `json_each_text`

**Purpose:** Performs the `each text` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `json_each_text(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative json_each_text JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select json_each_text(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.each_text |
| UUID | 019dffbb-f000-7781-ac0e-16252382545b |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_each_text.v3 |
| AST binding | ast.expr.json_each_text |
| Engine entrypoint | json_each_text |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `json_exists`

**Purpose:** Performs the `exists` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `json_exists(document,jsonpath)`
- Syntax category: `function_call`

**Parameters:**

- `document`: Bound using the declared descriptor rules for this overload.
- `jsonpath`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative json_exists JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select json_exists(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.exists |
| UUID | 019e18f0-1300-7001-9f01-000000000013 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_exists.v3 |
| AST binding | ast.expr.json_exists |
| Engine entrypoint | json_exists |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `json_extract`

**Purpose:** Extracts a JSON value using a descriptor-supported path or key.

**Call Forms:**

- `json_extract(document,path)`
- Syntax category: `function_call`

**Parameters:**

- `document`: Bound using the declared descriptor rules for this overload.
- `path`: Bound using the declared descriptor rules for this overload.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless noted by function-specific semantics.

**Returns:**

JSON/path extraction.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where string semantics apply.
- Timezone: session timezone for temporal forms; not applicable otherwise.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/overflow/domain errors use builtin error compatibility matrix.

**Example:**

```sql
select json_extract(cast('{"name":"Ada"}' as json_document), '$.name') as extracted_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.extract |
| UUID | 019de5fc-2400-7e24-878d-0b67cc0acc2f |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_extract.v3 |
| AST binding | ast.expr.json_extract |
| Engine entrypoint | json_extract |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `BIF-CONF-049c83c28e29;SBSFC013-json-extract-canonical`.

### `json_insert`

**Purpose:** Returns JSON with a value inserted where the path is absent.

**Call Forms:**

- `json_insert(document,path,value)`
- Syntax category: `function_call`

**Parameters:**

- `document`: Bound using the declared descriptor rules for this overload.
- `path`: Bound using the declared descriptor rules for this overload.
- `value`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative json_insert JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select json_insert(value_1, value_2, value_3) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.insert |
| UUID | 019dffbb-f000-77f8-afca-320e3246bd12 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_insert.v3 |
| AST binding | ast.expr.json_insert |
| Engine entrypoint | json_insert |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `json_object`

**Purpose:** Performs the `object` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `json_object(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative json_object JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select json_object(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.object |
| UUID | 019dffbb-f000-7069-9983-2ef3fb6e5c81 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_object.v3 |
| AST binding | ast.expr.json_object |
| Engine entrypoint | json_object |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `json_object_keys`

**Purpose:** Returns object keys from a JSON object value.

**Call Forms:**

- `json_object_keys(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative json_object_keys JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select json_object_keys(cast('{"name":"Ada"}' as json_document)) as key_name;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.object_keys |
| UUID | 019dffbb-f000-70b8-8258-c6287a9048e5 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_object_keys.v3 |
| AST binding | ast.expr.json_object_keys |
| Engine entrypoint | json_object_keys |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `json_object_text_array`

**Purpose:** Performs the `object text array` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `json_object(text[][,text[]])`
- Syntax category: `function_call`

**Parameters:**

- `text[][`: Bound using the declared descriptor rules for this overload.
- `text[]]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one even-length text array or matching key and value text arrays.
- Coercion: JSON document inputs are bounded UTF-8 document scalars; scalar inputs are converted through the existing JSON literal conversion rules.
- NULL handling: SQL NULL text-array input returns SQL NULL json_document.

**Returns:**

json_document object.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied JSON/scalar inputs.
- Side effects: none.
- Collation/charset: JSON text is emitted as UTF-8 json_document with deterministic key/order preservation for supplied arguments.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR nosql.document runtime using bounded in-core JSON helpers; no parser SQL, external execution, storage scan, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, invalid pretty flag, or malformed text-array json_object input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select json_object_text_array(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.object_text_array |
| UUID | 019dffbb-f000-7ca5-b599-7ba03c7f2c8d |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_object_text_array.v3 |
| AST binding | ast.expr.json_object_text_array |
| Engine entrypoint | json_object_text_array |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC052-json-object-text-arrays`.

### `json_query`

**Purpose:** Evaluates a JSON query/path expression and returns JSON output.

**Call Forms:**

- `json_query(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative json_query JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select json_query(cast('{"items":[1,2]}' as json_document), '$.items') as items_json;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.query |
| UUID | 019e18f0-1300-7003-9f01-000000000013 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_query.v3 |
| AST binding | ast.expr.json_query |
| Engine entrypoint | json_query |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `json_remove`

**Purpose:** Returns JSON with a path removed.

**Call Forms:**

- `json_remove(document,path)`
- Syntax category: `function_call`

**Parameters:**

- `document`: Bound using the declared descriptor rules for this overload.
- `path`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative json_remove JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select json_remove(cast('{"debug":true,"name":"Ada"}' as json_document), '$.debug') as cleaned_json;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.remove |
| UUID | 019e18f0-1300-7005-9f01-000000000013 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_remove.v3 |
| AST binding | ast.expr.json_remove |
| Engine entrypoint | json_remove |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `json_replace`

**Purpose:** Returns JSON with a value replaced where the path exists.

**Call Forms:**

- `json_replace(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative json_replace JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select json_replace(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.replace |
| UUID | 019e18f0-1300-7006-9f01-000000000013 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_replace.v3 |
| AST binding | ast.expr.json_replace |
| Engine entrypoint | json_replace |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `json_set`

**Purpose:** Returns JSON with a value set at a path.

**Call Forms:**

- `json_set(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative json_set JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select json_set(cast('{"name":"Ada"}' as json_document), '$.active', true) as updated_json;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.set |
| UUID | 019e18f0-1300-7004-9f01-000000000013 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_set.v3 |
| AST binding | ast.expr.json_set |
| Engine entrypoint | json_set |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `json_table`

**Purpose:** Performs the `table` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `JSON_TABLE(document,jsonpathCOLUMNS(...))`
- Syntax category: `function_call`

**Parameters:**

- `document`: Bound using the declared descriptor rules for this overload.
- `jsonpathCOLUMNS(...)`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: document and SQL/JSON path descriptor.
- Coercion: JSON document inputs are bounded UTF-8 document scalars; scalar inputs are converted through the existing JSON literal conversion rules.
- NULL handling: SQL NULL document or path returns SQL NULL json_document.

**Returns:**

json_document descriptor payload with zero or more bounded column descriptors.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied JSON/scalar inputs.
- Side effects: none.
- Collation/charset: JSON text is emitted as UTF-8 json_document with deterministic key/order preservation for supplied arguments.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR nosql.document runtime using bounded in-core JSON helpers; no parser SQL, external execution, storage scan, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, invalid pretty flag, or malformed text-array json_object input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select json_table(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.table |
| UUID | 019dffbb-f000-7800-af0a-cb636c6fb514 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_table.v3 |
| AST binding | ast.expr.json_table |
| Engine entrypoint | json_table |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC052-json-table-document-jsonpath`.

### `json_typeof`

**Purpose:** Returns the JSON type name for a JSON value.

**Call Forms:**

- `json_typeof(value)`
- Syntax category: `function_call`

**Parameters:**

- `value`: Bound using the declared descriptor rules for this overload.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless noted by function-specific semantics.

**Returns:**

JSON type classifier.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where string semantics apply.
- Timezone: session timezone for temporal forms; not applicable otherwise.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/overflow/domain errors use builtin error compatibility matrix.

**Example:**

```sql
select json_typeof(cast('{"name":"Ada"}' as json_document)) as json_kind;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.typeof |
| UUID | 019de5fc-2400-7d02-adb4-da4a6570dc6e |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_typeof.v3 |
| AST binding | ast.expr.json_typeof |
| Engine entrypoint | json_typeof |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `BIF-CONF-a427e7c340cb;SBSFC013-json-typeof-canonical`.

### `json_value`

**Purpose:** Evaluates a JSON query/path expression and returns a scalar value.

**Call Forms:**

- `JSON_VALUE(doc,path[PASSING...][RETURNING...][ONEMPTY][ONERROR])`
- Syntax category: `function_call`

**Parameters:**

- `doc`: Bound using the declared descriptor rules for this overload.
- `path[PASSING...][RETURNING...][ONEMPTY][ONERROR]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative json_value JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select json_value(cast('{"name":"Ada"}' as json_document), '$.name') as name_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.value |
| UUID | 019e18f0-1300-7002-9f01-000000000013 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_value.v3 |
| AST binding | ast.expr.json_value |
| Engine entrypoint | json_value |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `jsonb_array_length`

**Purpose:** Performs the JSONB form of the `array length` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `jsonb_array_length(document)`
- Syntax category: `function_call`

**Parameters:**

- `document`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative jsonb_array_length JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select jsonb_array_length(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_array_length |
| UUID | 019dffbb-f000-789c-bede-bf3de23f5365 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.jsonb_array_length.v3 |
| AST binding | ast.expr.jsonb_array_length |
| Engine entrypoint | jsonb_array_length |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `jsonb_build_array`

**Purpose:** Performs the JSONB form of the `build array` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `jsonb_build_array(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative jsonb_build_array JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select jsonb_build_array(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_build_array |
| UUID | 019dffbb-f000-794b-af4b-ef5ded9604c6 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.jsonb_build_array.v3 |
| AST binding | ast.expr.jsonb_build_array |
| Engine entrypoint | jsonb_build_array |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `jsonb_build_object`

**Purpose:** Performs the JSONB form of the `build object` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `jsonb_build_object(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative jsonb_build_object JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select jsonb_build_object(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_build_object |
| UUID | 019dffbb-f000-7bd9-9a2b-4e7fa4678978 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.jsonb_build_object.v3 |
| AST binding | ast.expr.jsonb_build_object |
| Engine entrypoint | jsonb_build_object |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `jsonb_insert`

**Purpose:** Performs the JSONB form of the `insert` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `jsonb_insert(document,path,value[,insert_after])`
- Syntax category: `function_call`

**Parameters:**

- `document`: Bound using the declared descriptor rules for this overload.
- `path`: Bound using the declared descriptor rules for this overload.
- `value[`: Bound using the declared descriptor rules for this overload.
- `insert_after]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative jsonb_insert JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select jsonb_insert(value_1, value_2, value_3) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_insert |
| UUID | 019dffbb-f000-73b2-93d0-ac97321ecaaf |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.jsonb_insert.v3 |
| AST binding | ast.expr.jsonb_insert |
| Engine entrypoint | jsonb_insert |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `jsonb_object`

**Purpose:** Performs the JSONB form of the `object` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `jsonb_object(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative jsonb_object JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select jsonb_object(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_object |
| UUID | 019dffbb-f000-724d-a766-7e894b545539 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.jsonb_object.v3 |
| AST binding | ast.expr.jsonb_object |
| Engine entrypoint | jsonb_object |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `jsonb_object_keys`

**Purpose:** Performs the JSONB form of the `object keys` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `jsonb_object_keys(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative jsonb_object_keys JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select jsonb_object_keys(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_object_keys |
| UUID | 019dffbb-f000-77d0-b41e-8377357107cb |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.jsonb_object_keys.v3 |
| AST binding | ast.expr.jsonb_object_keys |
| Engine entrypoint | jsonb_object_keys |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `jsonb_path_exists`

**Purpose:** Performs the JSONB form of the `path exists` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `jsonb_path_exists(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative jsonb_path_exists JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select jsonb_path_exists(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_path_exists |
| UUID | 019dffbb-f000-7ea2-9bcc-674a1eb9a59b |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.jsonb_path_exists.v3 |
| AST binding | ast.expr.jsonb_path_exists |
| Engine entrypoint | jsonb_path_exists |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `jsonb_path_match`

**Purpose:** Performs the JSONB form of the `path match` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `jsonb_path_match(document,jsonpath[,vars])`
- Syntax category: `function_call`

**Parameters:**

- `document`: Bound using the declared descriptor rules for this overload.
- `jsonpath[`: Bound using the declared descriptor rules for this overload.
- `vars]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative jsonb_path_match JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select jsonb_path_match(value_1, value_2, value_3) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_path_match |
| UUID | 019dffbb-f000-742b-a0b9-608e0e2e5c90 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.jsonb_path_match.v3 |
| AST binding | ast.expr.jsonb_path_match |
| Engine entrypoint | jsonb_path_match |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `jsonb_path_query`

**Purpose:** Performs the JSONB form of the `path query` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `jsonb_path_query(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative jsonb_path_query JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select jsonb_path_query(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_path_query |
| UUID | 019dffbb-f000-7f6b-afdc-960616319c88 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.jsonb_path_query.v3 |
| AST binding | ast.expr.jsonb_path_query |
| Engine entrypoint | jsonb_path_query |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `jsonb_path_query_array`

**Purpose:** Performs the JSONB form of the `path query array` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `jsonb_path_query_array(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative jsonb_path_query_array JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select jsonb_path_query_array(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_path_query_array |
| UUID | 019dffbb-f000-7525-830b-255572ed5a0f |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.jsonb_path_query_array.v3 |
| AST binding | ast.expr.jsonb_path_query_array |
| Engine entrypoint | jsonb_path_query_array |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `jsonb_path_query_first`

**Purpose:** Performs the JSONB form of the `path query first` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `jsonb_path_query_first(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative jsonb_path_query_first JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select jsonb_path_query_first(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_path_query_first |
| UUID | 019dffbb-f000-7432-8ac5-75c7e22f4d15 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.jsonb_path_query_first.v3 |
| AST binding | ast.expr.jsonb_path_query_first |
| Engine entrypoint | jsonb_path_query_first |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `jsonb_pretty`

**Purpose:** Performs the JSONB form of the `pretty` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `jsonb_pretty(document)`
- Syntax category: `function_call`

**Parameters:**

- `document`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative jsonb_pretty JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select jsonb_pretty(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_pretty |
| UUID | 019dffbb-f000-7b1b-b1d7-013acfdcb345 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.jsonb_pretty.v3 |
| AST binding | ast.expr.jsonb_pretty |
| Engine entrypoint | jsonb_pretty |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `jsonb_set`

**Purpose:** Performs the JSONB form of the `set` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `jsonb_set(document,path,value[,create_missing])`
- Syntax category: `function_call`

**Parameters:**

- `document`: Bound using the declared descriptor rules for this overload.
- `path`: Bound using the declared descriptor rules for this overload.
- `value[`: Bound using the declared descriptor rules for this overload.
- `create_missing]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative jsonb_set JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select jsonb_set(value_1, value_2, value_3) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_set |
| UUID | 019dffbb-f000-7655-ae18-a53f11c240f5 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.jsonb_set.v3 |
| AST binding | ast.expr.jsonb_set |
| Engine entrypoint | jsonb_set |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `jsonb_strip_nulls`

**Purpose:** Performs the JSONB form of the `strip nulls` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `jsonb_strip_nulls(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative jsonb_strip_nulls JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select jsonb_strip_nulls(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_strip_nulls |
| UUID | 019dffbb-f000-72dd-bab3-ac7237116ff6 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.jsonb_strip_nulls.v3 |
| AST binding | ast.expr.jsonb_strip_nulls |
| Engine entrypoint | jsonb_strip_nulls |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `jsonb_typeof`

**Purpose:** Returns the JSONB type name for a JSON value.

**Call Forms:**

- `jsonb_typeof(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative jsonb_typeof JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select jsonb_typeof(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.jsonb_typeof |
| UUID | 019e18f0-1300-7010-9f01-000000000013 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.jsonb_typeof.v3 |
| AST binding | ast.expr.jsonb_typeof |
| Engine entrypoint | jsonb_typeof |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `row_to_json`

**Purpose:** Evaluates `row_to_json` and returns json_document object, optionally pretty rendered.

**Call Forms:**

- `row_to_json(row[,pretty])`
- Syntax category: `function_call`

**Parameters:**

- `row[`: Bound using the declared descriptor rules for this overload.
- `pretty]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: row/object argument plus optional boolean-compatible pretty flag.
- Coercion: JSON document inputs are bounded UTF-8 document scalars; scalar inputs are converted through the existing JSON literal conversion rules.
- NULL handling: SQL NULL row input returns SQL NULL json_document; SQL NULL pretty flag refuses as invalid input.

**Returns:**

json_document object, optionally pretty rendered.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied JSON/scalar inputs.
- Side effects: none.
- Collation/charset: JSON text is emitted as UTF-8 json_document with deterministic key/order preservation for supplied arguments.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR nosql.document runtime using bounded in-core JSON helpers; no parser SQL, external execution, storage scan, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, invalid pretty flag, or malformed text-array json_object input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select row_to_json(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.row_to_json |
| UUID | 019dffbb-f000-7b93-87c6-0a817949054a |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.json_row_to_json.v3 |
| AST binding | ast.expr.json_row_to_json |
| Engine entrypoint | row_to_json |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC052-row-to-json-pretty`.

### `to_json`

**Purpose:** Evaluates `to_json` and returns descriptor-authoritative to_json JSON/document scalar result as implemented by the SBLR document expression runtime.

**Call Forms:**

- `to_json(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative to_json JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select to_json(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.to_json |
| UUID | 019dffbb-f000-771d-b937-1fafd31b87c0 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.to_json.v3 |
| AST binding | ast.expr.to_json |
| Engine entrypoint | to_json |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.

### `to_jsonb`

**Purpose:** Evaluates `to_jsonb` and returns descriptor-authoritative to_jsonb JSON/document scalar result as implemented by the SBLR document expression runtime.

**Call Forms:**

- `to_jsonb(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative JSON/document arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for JSON document, JSON path, text, boolean, and scalar value arguments.
- NULL handling: strict unless JSON/document function-specific semantics or the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative to_jsonb JSON/document scalar result as implemented by the SBLR document expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for JSON path/text values where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/path/document-format errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select to_jsonb(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.json.to_jsonb |
| UUID | 019dffbb-f000-7d72-91b3-6cc44f83bc70 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.to_jsonb.v3 |
| AST binding | ast.expr.to_jsonb |
| Engine entrypoint | to_jsonb |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC013_DOCUMENT_COLLECTION_FIXTURES`.


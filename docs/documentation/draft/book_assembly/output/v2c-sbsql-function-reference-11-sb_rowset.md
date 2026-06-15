

===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/functional_reference/sb_rowset.md -->

<a id="ch-language-reference-functional-reference-sb-rowset-md"></a>

# SB Rowset Functional Reference

Generation task: `sb_rowset`

Package namespace: `sb.rowset`

Rowset, set-returning, table-value, multiset, and series construction helpers.

## How To Read This Page

Creates and transforms rowset, table-value, array, multiset, and generated-series values.

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
| scalar | 16 |

## Operation Reference

### `element`

**Purpose:** Evaluates `element` and returns single JSON-backed element or SQL NULL for empty multiset; non-singleton multiset refuses with invalid input.

**Call Forms:**

- `element(multiset<T>)`
- Syntax category: `function_call`

**Parameters:**

- `multiset<T>`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one singleton array-backed multiset.
- Coercion: rowset/table_value arguments use bounded JSON descriptors; set-returning helpers emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed.

**Returns:**

single JSON-backed element or SQL NULL for empty multiset; non-singleton multiset refuses with invalid input.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: none.
- Collation/charset: descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; no parser SQL, external execution, storage scan, cursor lifecycle authority, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select element(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.multiset.element |
| UUID | 019dffbb-f000-7c3e-a192-e79f26db9b80 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.multiset_element.v3 |
| AST binding | ast.expr.multiset_element |
| Engine entrypoint | element |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC053-multiset-element`.

### `fusion`

**Purpose:** Evaluates `fusion` and returns array JSON with fused multiset elements.

**Call Forms:**

- `fusion(multiset<T>)`
- Syntax category: `function_call`

**Parameters:**

- `multiset<T>`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one or more array-backed multisets.
- Coercion: rowset/table_value arguments use bounded JSON descriptors; set-returning helpers emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed.

**Returns:**

array JSON with fused multiset elements.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: none.
- Collation/charset: descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; no parser SQL, external execution, storage scan, cursor lifecycle authority, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select fusion(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.multiset.fusion |
| UUID | 019dffbb-f000-70b2-9c5e-e8db048341ca |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.multiset_fusion.v3 |
| AST binding | ast.expr.multiset_fusion |
| Engine entrypoint | fusion |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC053-multiset-fusion`.

### `generate_series`

**Purpose:** Returns a generated sequence of values as a rowset.

**Call Forms:**

- `generate_series(start,stop[,step])`
- Syntax category: `function_call`

**Parameters:**

- `start`: Bound using the declared descriptor rules for this overload.
- `stop[`: Bound using the declared descriptor rules for this overload.
- `step]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: int64 start, stop, and optional nonzero int64 step.
- Coercion: rowset/table_value arguments use bounded JSON descriptors; set-returning helpers emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed.

**Returns:**

bounded integer array JSON.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: none.
- Collation/charset: descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; no parser SQL, external execution, storage scan, cursor lifecycle authority, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select value from generate_series(1, 5) as value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.rowset.generate_series |
| UUID | 019dffbb-f000-7e2c-b437-ebbbc2d4f35b |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.rowset_generate_series.v3 |
| AST binding | ast.expr.rowset_generate_series |
| Engine entrypoint | generate_series |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC053-generate-series-start-stop-step`.

### `intersection`

**Purpose:** Evaluates `intersection` and returns array JSON with common multiset elements.

**Call Forms:**

- `intersection(multiset<T>)`
- Syntax category: `function_call`

**Parameters:**

- `multiset<T>`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one or more array-backed multisets.
- Coercion: rowset/table_value arguments use bounded JSON descriptors; set-returning helpers emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed.

**Returns:**

array JSON with common multiset elements.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: none.
- Collation/charset: descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; no parser SQL, external execution, storage scan, cursor lifecycle authority, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select intersection(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.multiset.intersection |
| UUID | 019dffbb-f000-7968-9b55-2bdbfa60c297 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.multiset_intersection.v3 |
| AST binding | ast.expr.multiset_intersection |
| Engine entrypoint | intersection |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC053-multiset-intersection`.

### `rowset`

**Purpose:** Evaluates `rowset` and returns rowset JSON descriptor with zero rows.

**Call Forms:**

- `rowset(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: zero or one optional row-shape descriptor.
- Coercion: rowset/table_value arguments use bounded JSON descriptors; set-returning helpers emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed.

**Returns:**

rowset JSON descriptor with zero rows.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: none.
- Collation/charset: descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; no parser SQL, external execution, storage scan, cursor lifecycle authority, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select rowset(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.rowset.rowset |
| UUID | 019dffbb-f000-78cb-8907-f0afcc0374f1 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.rowset_rowset.v3 |
| AST binding | ast.expr.rowset_rowset |
| Engine entrypoint | rowset |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC053-rowset-marker`.

### `rowset_append`

**Purpose:** Evaluates `rowset_append` and returns rowset JSON descriptor with one appended row.

**Call Forms:**

- `rowset_append(rowset,expr[,expr...])`
- Syntax category: `function_call`

**Parameters:**

- `rowset`: Bound using the declared descriptor rules for this overload.
- `expr[`: Bound using the declared descriptor rules for this overload.
- `expr...]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: rowset descriptor and one or more row values.
- Coercion: rowset/table_value arguments use bounded JSON descriptors; set-returning helpers emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed.

**Returns:**

rowset JSON descriptor with one appended row.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: none.
- Collation/charset: descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; no parser SQL, external execution, storage scan, cursor lifecycle authority, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select rowset_append(value_1, value_2, value_3) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.rowset.append |
| UUID | 019dffbb-f000-7275-bd16-ba5ae55214da |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.rowset_append.v3 |
| AST binding | ast.expr.rowset_append |
| Engine entrypoint | rowset_append |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC053-rowset-append-exprs`.

### `rowset_new`

**Purpose:** Evaluates `rowset_new` and returns rowset JSON descriptor with zero rows.

**Call Forms:**

- `rowset_new(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: optional row-shape descriptor.
- Coercion: rowset/table_value arguments use bounded JSON descriptors; set-returning helpers emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed.

**Returns:**

rowset JSON descriptor with zero rows.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: none.
- Collation/charset: descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; no parser SQL, external execution, storage scan, cursor lifecycle authority, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select rowset_new(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.rowset.new |
| UUID | 019dffbb-f000-77b1-8fef-7b509cc646ae |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.rowset_new.v3 |
| AST binding | ast.expr.rowset_new |
| Engine entrypoint | rowset_new |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC053-rowset-new-empty`.

### `rowset_size`

**Purpose:** Evaluates `rowset_size` and returns int64 descriptor row count.

**Call Forms:**

- `rowset_size(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: one rowset descriptor.
- Coercion: rowset/table_value arguments use bounded JSON descriptors; set-returning helpers emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed.

**Returns:**

int64 descriptor row count.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: none.
- Collation/charset: descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; no parser SQL, external execution, storage scan, cursor lifecycle authority, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select rowset_size(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.rowset.size |
| UUID | 019dffbb-f000-7aa8-a950-773cbbd414ec |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.rowset_size.v3 |
| AST binding | ast.expr.rowset_size |
| Engine entrypoint | rowset_size |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC053-rowset-size-marker`.

### `rowset_to_array`

**Purpose:** Evaluates `rowset_to_array` and returns array JSON containing descriptor rows.

**Call Forms:**

- `rowset_to_array(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: one rowset descriptor.
- Coercion: rowset/table_value arguments use bounded JSON descriptors; set-returning helpers emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed.

**Returns:**

array JSON containing descriptor rows.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: none.
- Collation/charset: descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; no parser SQL, external execution, storage scan, cursor lifecycle authority, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select rowset_to_array(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.rowset.to_array |
| UUID | 019dffbb-f000-780f-9abf-9905eff4d97f |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.rowset_to_array.v3 |
| AST binding | ast.expr.rowset_to_array |
| Engine entrypoint | rowset_to_array |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC053-rowset-to-array-marker`.

### `setof`

**Purpose:** Evaluates `setof` and returns setof JSON descriptor with columns, one row, and row_count.

**Call Forms:**

- `setof(T,...,ordinalitybigint)`
- Syntax category: `function_call`

**Parameters:**

- `T`: Bound using the declared descriptor rules for this overload.
- `...`: Bound using the declared descriptor rules for this overload.
- `ordinalitybigint`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: zero or more scalar values.
- Coercion: rowset/table_value arguments use bounded JSON descriptors; set-returning helpers emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed.

**Returns:**

setof JSON descriptor with columns, one row, and row_count.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: none.
- Collation/charset: descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; no parser SQL, external execution, storage scan, cursor lifecycle authority, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select setof(value_1, value_2, value_3) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.setof.generic |
| UUID | 019dffbb-f000-70f5-b358-f82a3542467b |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.setof_generic.v3 |
| AST binding | ast.expr.setof_generic |
| Engine entrypoint | setof |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC053-setof-generic`.

### `setof_key_text_value_document`

**Purpose:** Evaluates `setof_key_text_value_document` and returns setof JSON descriptor containing one key/document row.

**Call Forms:**

- `setof(keytext,valuedocument)`
- Syntax category: `function_call`

**Parameters:**

- `keytext`: Bound using the declared descriptor rules for this overload.
- `valuedocument`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: non-null text key and nullable document value.
- Coercion: rowset/table_value arguments use bounded JSON descriptors; set-returning helpers emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed.

**Returns:**

setof JSON descriptor containing one key/document row.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: none.
- Collation/charset: descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; no parser SQL, external execution, storage scan, cursor lifecycle authority, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select setof_key_text_value_document(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.setof.key_text_value_document |
| UUID | 019dffbb-f000-7cd3-a7bf-a84f1da87607 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.setof_key_text_value_document.v3 |
| AST binding | ast.expr.setof_key_text_value_document |
| Engine entrypoint | setof_key_text_value_document |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC053-setof-key-text-value-document`.

### `setof_key_text_value_text`

**Purpose:** Evaluates `setof_key_text_value_text` and returns setof JSON descriptor containing one key/value text row.

**Call Forms:**

- `setof(keytext,valuetext)`
- Syntax category: `function_call`

**Parameters:**

- `keytext`: Bound using the declared descriptor rules for this overload.
- `valuetext`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: non-null text key and nullable text value.
- Coercion: rowset/table_value arguments use bounded JSON descriptors; set-returning helpers emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed.

**Returns:**

setof JSON descriptor containing one key/value text row.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: none.
- Collation/charset: descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; no parser SQL, external execution, storage scan, cursor lifecycle authority, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select setof_key_text_value_text(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.setof.key_text_value_text |
| UUID | 019dffbb-f000-7d76-a1e4-ac2ea892d178 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.setof_key_text_value_text.v3 |
| AST binding | ast.expr.setof_key_text_value_text |
| Engine entrypoint | setof_key_text_value_text |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC053-setof-key-text-value-text`.

### `table_value`

**Purpose:** Evaluates `table_value` and returns table_value JSON descriptor with zero rows.

**Call Forms:**

- `table_value(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: zero or one optional row-shape descriptor.
- Coercion: rowset/table_value arguments use bounded JSON descriptors; set-returning helpers emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed.

**Returns:**

table_value JSON descriptor with zero rows.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: none.
- Collation/charset: descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; no parser SQL, external execution, storage scan, cursor lifecycle authority, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select table_value(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.table_value.value |
| UUID | 019dffbb-f000-7db1-8865-e6a826478d12 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.table_value_value.v3 |
| AST binding | ast.expr.table_value_value |
| Engine entrypoint | table_value |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC053-table-value-marker`.

### `table_value_append`

**Purpose:** Evaluates `table_value_append` and returns table_value JSON descriptor with one appended row.

**Call Forms:**

- `table_value_append(tv,row)`
- Syntax category: `function_call`

**Parameters:**

- `tv`: Bound using the declared descriptor rules for this overload.
- `row`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: table_value descriptor and one row payload.
- Coercion: rowset/table_value arguments use bounded JSON descriptors; set-returning helpers emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed.

**Returns:**

table_value JSON descriptor with one appended row.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: none.
- Collation/charset: descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; no parser SQL, external execution, storage scan, cursor lifecycle authority, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select table_value_append(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.table_value.append |
| UUID | 019dffbb-f000-7d07-aa7b-1f120bd32150 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.table_value_append.v3 |
| AST binding | ast.expr.table_value_append |
| Engine entrypoint | table_value_append |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC053-table-value-append-row`.

### `table_value_new`

**Purpose:** Evaluates `table_value_new` and returns table_value JSON descriptor with zero rows.

**Call Forms:**

- `table_value_new(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: optional row-shape descriptor.
- Coercion: rowset/table_value arguments use bounded JSON descriptors; set-returning helpers emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed.

**Returns:**

table_value JSON descriptor with zero rows.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: none.
- Collation/charset: descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; no parser SQL, external execution, storage scan, cursor lifecycle authority, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select table_value_new(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.table_value.new |
| UUID | 019dffbb-f000-769f-af69-9f68356bf2cf |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.table_value_new.v3 |
| AST binding | ast.expr.table_value_new |
| Engine entrypoint | table_value_new |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC053-table-value-new-empty`.

### `unnest`

**Purpose:** Expands an array or descriptor-supported collection into a rowset.

**Call Forms:**

- `unnest(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: one array descriptor.
- Coercion: rowset/table_value arguments use bounded JSON descriptors; set-returning helpers emit array JSON payloads; scalar arguments are converted through SBLR scalar encodings.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor-dependent routes; SQL NULL scalar elements are represented as JSON null where rows or arrays are constructed.

**Returns:**

array JSON preserving input elements.

**Behavior:**

- Volatility: immutable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: none.
- Collation/charset: descriptor and array payloads are deterministic UTF-8 JSON text; no collation-sensitive comparison except exact multiset token matching.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR rowset.table runtime using bounded in-core descriptor helpers; no parser SQL, external execution, storage scan, cursor lifecycle authority, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed descriptor, scalar non-array unnest, step-zero generate_series, or non-array multiset input refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select item from unnest(array[1, 2, 3]) as item;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.rowset.unnest |
| UUID | 019dffbb-f000-7713-a8f2-6cda4672b5c5 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.rowset_unnest.v3 |
| AST binding | ast.expr.rowset_unnest |
| Engine entrypoint | unnest |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC053-unnest-marker`.


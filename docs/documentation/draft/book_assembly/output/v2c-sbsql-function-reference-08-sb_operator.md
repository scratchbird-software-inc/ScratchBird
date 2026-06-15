

===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/functional_reference/sb_operator.md -->

<a id="ch-language-reference-functional-reference-sb-operator-md"></a>

# SB Operator Functional Reference

Generation task: `sb_operator`

Package namespace: `sb.operator`

Operator functions for arithmetic, comparison, boolean, pattern, JSON, array, and vector-like operator spellings.

## How To Read This Page

Documents operator-backed expression surfaces. SBsql usually uses symbolic syntax; the function names here identify the bound operation.

Each entry below is written for a user reading SBsql, not for a registry maintainer. The technical fields are retained so an operator can connect the language surface to SBLR and engine diagnostics when troubleshooting.

Privileges, policy admission, sandboxing, and descriptor compatibility are still checked by the surrounding statement. A function being listed here does not grant access to catalog objects, protected material, files, network targets, or external services.

Most operator entries are normally written using symbolic or keyword syntax. The function-style names document the operation that the parser binds after precedence and type-resolution rules are applied.

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
| operator | 24 |

## Operation Reference

### `add`

**Purpose:** Implements the `+` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `add(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative add operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 6 + 3 as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.add |
| UUID | 019dffbb-f000-736d-a32f-578b52136cfc |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_add.v3 |
| AST binding | ast.operator.operator_add |
| Engine entrypoint | operator_add |
| Optimizer foldable | True |
| Index eligible | False |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-add-int64;SBSFCOP-add-null`.

### `and`

**Purpose:** Implements the `AND` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `and(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative and operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select true and false as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.and |
| UUID | 019dffbb-f000-7184-9845-fe5b75231c3e |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_and.v3 |
| AST binding | ast.operator.operator_and |
| Engine entrypoint | operator_and |
| Optimizer foldable | True |
| Index eligible | False |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-and-3vl-false;SBSFCOP-and-3vl-unknown`.

### `array_contains`

**Purpose:** Implements the `@>` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `array_contains(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative array_contains operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 6 @> 3 as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.array_contains |
| UUID | 019dffbb-f000-7e5d-ab20-ae6502c0aef3 |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_array_contains.v3 |
| AST binding | ast.operator.operator_array_contains |
| Engine entrypoint | operator_array_contains |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-array-contains-positive;SBSFCOP-array-contains-negative`.

### `concat`

**Purpose:** Implements the `||` string concatenation operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `concat(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative text/binary operand arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for text/character operands.
- NULL handling: operator/function-specific NULL handling follows the conformance fixtures row evidence; null_concat_returns_null dialect parameter controls whether NULL operands propagate NULL or are treated as empty string.

**Returns:**

descriptor-authoritative concatenated character result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 'Hello' || ', ' || 'World' as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.concat |
| UUID | 019dffbb-f000-7977-afb8-130dd8d3b751 |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_concat.v3 |
| AST binding | ast.operator.operator_concat |
| Engine entrypoint | operator_concat |
| Optimizer foldable | True |
| Index eligible | False |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-concat-text`.

### `divide`

**Purpose:** Implements the `/` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `divide(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative divide operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 6 / 3 as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.divide |
| UUID | 019dffbb-f000-7f6c-a735-c88297635dac |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_divide.v3 |
| AST binding | ast.operator.operator_divide |
| Engine entrypoint | operator_divide |
| Optimizer foldable | True |
| Index eligible | False |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-divide-int64;SBSFCOP-divide-by-zero`.

### `equal`

**Purpose:** Implements the `=` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `equal(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative equal operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 6 = 3 as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.equal |
| UUID | 019dffbb-f000-7bc5-acb4-2d63211337cc |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_equal.v3 |
| AST binding | ast.operator.operator_equal |
| Engine entrypoint | operator_equal |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-equal-int64;SBSFCOP-equal-null`.

### `greater`

**Purpose:** Implements the `>` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `greater(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative greater operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 6 > 3 as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.greater |
| UUID | 019dffbb-f000-7ca9-ae06-ac2539b92994 |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_greater.v3 |
| AST binding | ast.operator.operator_greater |
| Engine entrypoint | operator_greater |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-greater-int64;SBSFCOP-greater-null`.

### `greater_equal`

**Purpose:** Implements the `>=` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `greater_equal(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative greater_equal operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 6 >= 3 as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.greater_equal |
| UUID | 019dffbb-f000-7ae9-9e20-824a87d2ba62 |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_greater_equal.v3 |
| AST binding | ast.operator.operator_greater_equal |
| Engine entrypoint | operator_greater_equal |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-greater-equal-int64;SBSFCOP-greater-equal-null`.

### `ilike`

**Purpose:** Implements the `ILIKE` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `ilike(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative ilike operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable_locale_versioned.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 'ScratchBird' ilike 'scratch%' as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.ilike |
| UUID | 019dffbb-f000-7341-a890-14cb79a0b41a |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_ilike.v3 |
| AST binding | ast.expr.operator_ilike |
| Engine entrypoint | operator_ilike |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFCOP-ilike-positive;SBSFCOP-ilike-negative;SBSFCOP-ilike-null;SBSFCOP-ilike-invalid-escape;SBSFCOP-ilike-non-text-left`.

### `is_distinct_from`

**Purpose:** Implements the `IS DISTINCT FROM` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `is_distinct_from(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative is_distinct_from operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 1 is distinct from null as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.is_distinct_from |
| UUID | 019dffbb-f000-7af6-8918-d8551dc2f34b |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_is_distinct_from.v3 |
| AST binding | ast.operator.operator_is_distinct_from |
| Engine entrypoint | operator_is_distinct_from |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-is-distinct-null;SBSFCOP-is-distinct-both-null`.

### `json_get`

**Purpose:** Performs the `get` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `json_get(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative json_get operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select cast('{"a":1}' as json_document) -> 'a' as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.json_get |
| UUID | 019dffbb-f000-71ea-9744-3c99eb020678 |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_json_get.v3 |
| AST binding | ast.operator.operator_json_get |
| Engine entrypoint | operator_json_get |
| Optimizer foldable | True |
| Index eligible | False |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-json-get-document`.

### `json_get_text`

**Purpose:** Performs the `get text` JSON helper under descriptor-aware JSON rules.

**Call Forms:**

- `json_get_text(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative json_get_text operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select cast('{"a":1}' as json_document) -> 'a' as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.json_get_text |
| UUID | 019dffbb-f000-7bed-b971-c2f027b3dd45 |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_json_get_text.v3 |
| AST binding | ast.operator.operator_json_get_text |
| Engine entrypoint | operator_json_get_text |
| Optimizer foldable | True |
| Index eligible | False |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-json-get-text-string;SBSFCOP-json-get-text-missing`.

### `less`

**Purpose:** Implements the `<` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `less(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative less operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 6 < 3 as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.less |
| UUID | 019dffbb-f000-7fe9-ba37-5a2a95a2c62c |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_less.v3 |
| AST binding | ast.operator.operator_less |
| Engine entrypoint | operator_less |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-less-int64;SBSFCOP-less-null`.

### `less_equal`

**Purpose:** Implements the `<=` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `less_equal(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative less_equal operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 6 <= 3 as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.less_equal |
| UUID | 019dffbb-f000-752e-bdd4-0d76297583b4 |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_less_equal.v3 |
| AST binding | ast.operator.operator_less_equal |
| Engine entrypoint | operator_less_equal |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-less-equal-int64;SBSFCOP-less-equal-null`.

### `like`

**Purpose:** Implements the `LIKE` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `like(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative like operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 'ScratchBird' like 'Scratch%' as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.like |
| UUID | 019dffbb-f000-7b5f-adfa-d93b9775ebf4 |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_like.v3 |
| AST binding | ast.operator.operator_like |
| Engine entrypoint | operator_like |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-like-positive;SBSFCOP-like-null;SBSFCOP-like-descriptor-collation;SBSFCOP-like-refusal`.

### `modulo`

**Purpose:** Implements the `%` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `modulo(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative modulo operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 6 % 3 as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.modulo |
| UUID | 019dffbb-f000-72aa-a8a1-eec01cf5b997 |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_modulo.v3 |
| AST binding | ast.operator.operator_modulo |
| Engine entrypoint | operator_modulo |
| Optimizer foldable | True |
| Index eligible | False |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-modulo-int64;SBSFCOP-modulo-by-zero`.

### `multiply`

**Purpose:** Implements the `*` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `multiply(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative multiply operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 6 * 3 as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.multiply |
| UUID | 019dffbb-f000-736c-97c7-c8250907109f |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_multiply.v3 |
| AST binding | ast.operator.operator_multiply |
| Engine entrypoint | operator_multiply |
| Optimizer foldable | True |
| Index eligible | False |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-multiply-int64;SBSFCOP-multiply-null`.

### `not`

**Purpose:** Implements the `NOT` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `NOT(xLIKEy)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `xLIKEy`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative not operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select not false as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.not |
| UUID | 019dffbb-f000-7513-8e77-83deace81805 |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_not.v3 |
| AST binding | ast.operator.operator_not |
| Engine entrypoint | operator_not |
| Optimizer foldable | True |
| Index eligible | False |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-not-3vl;SBSFCOP-not-like-composed;SBSFCOP-not-like-refusal`.

### `not_equal`

**Purpose:** Implements the `<>` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `not_equal(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative not_equal operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 6 <> 3 as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.not_equal |
| UUID | 019dffbb-f000-7361-9fd3-f01338e9ee35 |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_not_equal.v3 |
| AST binding | ast.operator.operator_not_equal |
| Engine entrypoint | operator_not_equal |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-not-equal-int64;SBSFCOP-not-equal-null`.

### `or`

**Purpose:** Implements the `OR` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `or(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative or operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select true or false as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.or |
| UUID | 019dffbb-f000-7f93-b028-8161d1eee2f0 |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_or.v3 |
| AST binding | ast.operator.operator_or |
| Engine entrypoint | operator_or |
| Optimizer foldable | True |
| Index eligible | False |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-or-3vl-true;SBSFCOP-or-3vl-unknown`.

### `regex_match`

**Purpose:** Implements the `~` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `regex_match(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative regex_match operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 6 ~ 3 as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.regex_match |
| UUID | 019dffbb-f000-7272-8555-dc403503398b |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_regex_match.v3 |
| AST binding | ast.operator.operator_regex_match |
| Engine entrypoint | operator_regex_match |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-regex-match-positive;SBSFCOP-regex-match-invalid-pattern`.

### `subtract`

**Purpose:** Implements the `-` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `subtract(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative subtract operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 6 - 3 as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.subtract |
| UUID | 019dffbb-f000-7b48-b1ac-bffb0c75b1e1 |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_subtract.v3 |
| AST binding | ast.operator.operator_subtract |
| Engine entrypoint | operator_subtract |
| Optimizer foldable | True |
| Index eligible | False |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-subtract-int64;SBSFCOP-subtract-null`.

### `unary_minus`

**Purpose:** Implements the `unary -` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `unary_minus(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative operator/function arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for operator/function operands.
- NULL handling: operator/function-specific NULL and three-valued logic handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative unary_minus operator/function result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when operands are constants and collation/version dependencies are stable.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset for text/pattern operands where character semantics apply.
- Timezone: not applicable.
- Security and authority: none.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select -42 as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.unary_minus |
| UUID | 019dffbb-f000-7219-a08f-5b2e49ba7e93 |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_unary_minus.v3 |
| AST binding | ast.operator.operator_unary_minus |
| Engine entrypoint | operator_unary_minus |
| Optimizer foldable | True |
| Index eligible | False |
| Generated-column eligible | True |
| Cost class | cpu_operator |

Conformance evidence: `SBSFCOP-unary-minus-int64;SBSFCOP-unary-minus-null;SBSFCOP-unary-minus-overflow`.

### `xor`

**Purpose:** Implements the `XOR` operator after descriptor binding selects compatible operand types.

**Call Forms:**

- `xor(...)`
- Syntax category: `operator_expression, function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for native function/operator operands.
- NULL handling: function/operator-specific NULL handling follows the conformance fixtures row evidence.

**Returns:**

descriptor-authoritative xor result as implemented by the SBLR expression runtime.

**Behavior:**

- Volatility: stable_by_arguments.
- Determinism: foldable only when inputs are constants with stable descriptors and function-specific context is fixed.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where character semantics apply.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/domain errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select 6 XOR 3 as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.operator.xor |
| UUID | 019dffbb-f000-724f-8a03-f9ed4772186f |
| Kind | operator |
| Syntax forms | operator_expression, function_call |
| SBLR binding | sblr.expr.operator_xor.v3 |
| AST binding | ast.operator.operator_xor |
| Engine entrypoint | operator_xor |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC026-native-xor`.


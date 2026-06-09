# SB LOB Functional Reference

Generation task: `sb_lob`

Package namespace: `sb.lob`

Large-object and locator helpers for bounded LOB access through engine-managed handles.

## How To Read This Page

Works with engine-managed large object locators. The locator is authority evidence only; the session still needs privileges on the object being read or written.

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
| scalar | 13 |

## Operation Reference

### `current_row_locator`

**Purpose:** Operates on an engine-managed large-object locator for `current row locator` behavior.

**Call Forms:**

- `current_row_locator`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: no arguments.
- Coercion: LOB locator arguments are bounded JSON descriptors; data accepts binary or text scalar values; offsets and lengths are int64.
- NULL handling: SQL NULL locator input refuses for required-locator operations and returns empty marker values for marker forms.

**Returns:**

current row locator descriptor.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for supplied descriptors.
- Side effects: bounded descriptor-only LOB state transition; no persistent mutation.
- Collation/charset: text LOB payloads use UTF-8 character descriptor; binary payloads use binary descriptor.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR lob.locator runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed locator descriptor, non-positive offset, negative length, or unsupported open mode refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select current_row_locator(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.locator.current_row |
| UUID | 019dffbb-f000-7b96-9888-7749d95abd28 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.locator_current_row.v3 |
| AST binding | ast.expr.locator_current_row |
| Engine entrypoint | current_row_locator |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC055-current-row-locator-marker`.

### `lob_append`

**Purpose:** Operates on an engine-managed large-object locator for `lob append` behavior.

**Call Forms:**

- `lob_append(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: data or locator descriptor plus data.
- Coercion: LOB locator arguments are bounded JSON descriptors; data accepts binary or text scalar values; offsets and lengths are int64.
- NULL handling: SQL NULL locator input refuses for required-locator operations and returns empty marker values for marker forms.

**Returns:**

LOB locator descriptor with appended bytes.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for supplied descriptors.
- Side effects: bounded descriptor-only LOB state transition; no persistent mutation.
- Collation/charset: text LOB payloads use UTF-8 character descriptor; binary payloads use binary descriptor.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR lob.locator runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed locator descriptor, non-positive offset, negative length, or unsupported open mode refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select lob_append(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.lob.append |
| UUID | 019dffbb-f000-74aa-a021-045a7d62e136 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.lob_append.v3 |
| AST binding | ast.expr.lob_append |
| Engine entrypoint | lob_append |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC055-lob-append-marker`.

### `lob_close`

**Purpose:** Operates on an engine-managed large-object locator for `lob close` behavior.

**Call Forms:**

- `lob_close(locator)`
- Syntax category: `function_call`

**Parameters:**

- `locator`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one locator descriptor.
- Coercion: LOB locator arguments are bounded JSON descriptors; data accepts binary or text scalar values; offsets and lengths are int64.
- NULL handling: SQL NULL locator input refuses for required-locator operations and returns empty marker values for marker forms.

**Returns:**

LOB locator descriptor in closed state.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for supplied descriptors.
- Side effects: bounded descriptor-only LOB state transition; no persistent mutation.
- Collation/charset: text LOB payloads use UTF-8 character descriptor; binary payloads use binary descriptor.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR lob.locator runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed locator descriptor, non-positive offset, negative length, or unsupported open mode refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select lob_close(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.lob.close |
| UUID | 019dffbb-f000-7a11-a540-e6c1f5bffd0f |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.lob_close.v3 |
| AST binding | ast.expr.lob_close |
| Engine entrypoint | lob_close |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC055-lob-close-arg`.

### `lob_create`

**Purpose:** Operates on an engine-managed large-object locator for `lob create` behavior.

**Call Forms:**

- `lob_create(class,[media])`
- Syntax category: `function_call`

**Parameters:**

- `class`: Bound using the declared descriptor rules for this overload.
- `[media]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: optional class and media type.
- Coercion: LOB locator arguments are bounded JSON descriptors; data accepts binary or text scalar values; offsets and lengths are int64.
- NULL handling: SQL NULL locator input refuses for required-locator operations and returns empty marker values for marker forms.

**Returns:**

new LOB locator descriptor.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for supplied descriptors.
- Side effects: bounded descriptor-only LOB state transition; no persistent mutation.
- Collation/charset: text LOB payloads use UTF-8 character descriptor; binary payloads use binary descriptor.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR lob.locator runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed locator descriptor, non-positive offset, negative length, or unsupported open mode refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select lob_create(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.lob.create |
| UUID | 019dffbb-f000-7769-9c32-920cb2a44d38 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.lob_create.v3 |
| AST binding | ast.expr.lob_create |
| Engine entrypoint | lob_create |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC055-lob-create-arg`.

### `lob_locator_to_binary`

**Purpose:** Operates on an engine-managed large-object locator for `lob locator to binary` behavior.

**Call Forms:**

- `lob_locator_to_binary(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: optional locator descriptor.
- Coercion: LOB locator arguments are bounded JSON descriptors; data accepts binary or text scalar values; offsets and lengths are int64.
- NULL handling: SQL NULL locator input refuses for required-locator operations and returns empty marker values for marker forms.

**Returns:**

binary locator payload or empty binary marker.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for supplied descriptors.
- Side effects: bounded descriptor-only LOB state transition; no persistent mutation.
- Collation/charset: text LOB payloads use UTF-8 character descriptor; binary payloads use binary descriptor.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR lob.locator runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed locator descriptor, non-positive offset, negative length, or unsupported open mode refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select lob_locator_to_binary(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.lob.locator_to_binary |
| UUID | 019dffbb-f000-71a7-8d88-378d891fdecd |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.lob_locator_to_binary.v3 |
| AST binding | ast.expr.lob_locator_to_binary |
| Engine entrypoint | lob_locator_to_binary |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC055-lob-locator-to-binary-marker`.

### `lob_locator_to_text`

**Purpose:** Operates on an engine-managed large-object locator for `lob locator to text` behavior.

**Call Forms:**

- `lob_locator_to_text(locator)`
- Syntax category: `function_call`

**Parameters:**

- `locator`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one locator descriptor.
- Coercion: LOB locator arguments are bounded JSON descriptors; data accepts binary or text scalar values; offsets and lengths are int64.
- NULL handling: SQL NULL locator input refuses for required-locator operations and returns empty marker values for marker forms.

**Returns:**

text locator payload.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for supplied descriptors.
- Side effects: bounded descriptor-only LOB state transition; no persistent mutation.
- Collation/charset: text LOB payloads use UTF-8 character descriptor; binary payloads use binary descriptor.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR lob.locator runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed locator descriptor, non-positive offset, negative length, or unsupported open mode refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select lob_locator_to_text(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.lob.locator_to_text |
| UUID | 019dffbb-f000-703a-bc67-17bdf02e4614 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.lob_locator_to_text.v3 |
| AST binding | ast.expr.lob_locator_to_text |
| Engine entrypoint | lob_locator_to_text |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC055-lob-locator-to-text-arg`.

### `lob_open`

**Purpose:** Operates on an engine-managed large-object locator for `lob open` behavior.

**Call Forms:**

- `lob_open(locator,mode)`
- Syntax category: `function_call`

**Parameters:**

- `locator`: Bound using the declared descriptor rules for this overload.
- `mode`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: locator descriptor and mode token.
- Coercion: LOB locator arguments are bounded JSON descriptors; data accepts binary or text scalar values; offsets and lengths are int64.
- NULL handling: SQL NULL locator input refuses for required-locator operations and returns empty marker values for marker forms.

**Returns:**

LOB locator descriptor in requested open mode.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for supplied descriptors.
- Side effects: bounded descriptor-only LOB state transition; no persistent mutation.
- Collation/charset: text LOB payloads use UTF-8 character descriptor; binary payloads use binary descriptor.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR lob.locator runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed locator descriptor, non-positive offset, negative length, or unsupported open mode refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select lob_open(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.lob.open |
| UUID | 019dffbb-f000-7050-93e3-09da21b5e350 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.lob_open.v3 |
| AST binding | ast.expr.lob_open |
| Engine entrypoint | lob_open |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC055-lob-open-arg`.

### `lob_read`

**Purpose:** Operates on an engine-managed large-object locator for `lob read` behavior.

**Call Forms:**

- `lob_read(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: locator descriptor, 1-based offset, and non-negative length.
- Coercion: LOB locator arguments are bounded JSON descriptors; data accepts binary or text scalar values; offsets and lengths are int64.
- NULL handling: SQL NULL locator input refuses for required-locator operations and returns empty marker values for marker forms.

**Returns:**

binary or text slice from locator payload.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for supplied descriptors.
- Side effects: bounded descriptor-only LOB state transition; no persistent mutation.
- Collation/charset: text LOB payloads use UTF-8 character descriptor; binary payloads use binary descriptor.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR lob.locator runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed locator descriptor, non-positive offset, negative length, or unsupported open mode refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select lob_read(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.lob.read |
| UUID | 019dffbb-f000-7873-af4d-bad73e1e5c43 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.lob_read.v3 |
| AST binding | ast.expr.lob_read |
| Engine entrypoint | lob_read |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC055-lob-read-marker`.

### `lob_size`

**Purpose:** Operates on an engine-managed large-object locator for `lob size` behavior.

**Call Forms:**

- `lob_size(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: optional locator descriptor.
- Coercion: LOB locator arguments are bounded JSON descriptors; data accepts binary or text scalar values; offsets and lengths are int64.
- NULL handling: SQL NULL locator input refuses for required-locator operations and returns empty marker values for marker forms.

**Returns:**

int64 byte count.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for supplied descriptors.
- Side effects: bounded descriptor-only LOB state transition; no persistent mutation.
- Collation/charset: text LOB payloads use UTF-8 character descriptor; binary payloads use binary descriptor.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR lob.locator runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed locator descriptor, non-positive offset, negative length, or unsupported open mode refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select lob_size(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.lob.size |
| UUID | 019dffbb-f000-7617-bb0e-7a9985ff6ecb |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.lob_size.v3 |
| AST binding | ast.expr.lob_size |
| Engine entrypoint | lob_size |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC055-lob-size-marker`.

### `lob_truncate`

**Purpose:** Operates on an engine-managed large-object locator for `lob truncate` behavior.

**Call Forms:**

- `lob_truncate(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: locator descriptor and non-negative length.
- Coercion: LOB locator arguments are bounded JSON descriptors; data accepts binary or text scalar values; offsets and lengths are int64.
- NULL handling: SQL NULL locator input refuses for required-locator operations and returns empty marker values for marker forms.

**Returns:**

LOB locator descriptor truncated to length.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for supplied descriptors.
- Side effects: bounded descriptor-only LOB state transition; no persistent mutation.
- Collation/charset: text LOB payloads use UTF-8 character descriptor; binary payloads use binary descriptor.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR lob.locator runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed locator descriptor, non-positive offset, negative length, or unsupported open mode refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select lob_truncate(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.lob.truncate |
| UUID | 019dffbb-f000-72a7-ba33-d3e216220898 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.lob_truncate.v3 |
| AST binding | ast.expr.lob_truncate |
| Engine entrypoint | lob_truncate |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC055-lob-truncate-marker`.

### `lob_write`

**Purpose:** Operates on an engine-managed large-object locator for `lob write` behavior.

**Call Forms:**

- `lob_write(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: locator descriptor, 1-based offset, and data.
- Coercion: LOB locator arguments are bounded JSON descriptors; data accepts binary or text scalar values; offsets and lengths are int64.
- NULL handling: SQL NULL locator input refuses for required-locator operations and returns empty marker values for marker forms.

**Returns:**

LOB locator descriptor with bytes written at offset.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for supplied descriptors.
- Side effects: bounded descriptor-only LOB state transition; no persistent mutation.
- Collation/charset: text LOB payloads use UTF-8 character descriptor; binary payloads use binary descriptor.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR lob.locator runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed locator descriptor, non-positive offset, negative length, or unsupported open mode refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select lob_write(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.lob.write |
| UUID | 019dffbb-f000-7e0a-b791-07758dd38a0e |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.lob_write.v3 |
| AST binding | ast.expr.lob_write |
| Engine entrypoint | lob_write |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC055-lob-write-marker`.

### `locator`

**Purpose:** Operates on an engine-managed large-object locator for `locator` behavior.

**Call Forms:**

- `locator(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: no arguments.
- Coercion: LOB locator arguments are bounded JSON descriptors; data accepts binary or text scalar values; offsets and lengths are int64.
- NULL handling: SQL NULL locator input refuses for required-locator operations and returns empty marker values for marker forms.

**Returns:**

generic locator descriptor.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for supplied descriptors.
- Side effects: bounded descriptor-only LOB state transition; no persistent mutation.
- Collation/charset: text LOB payloads use UTF-8 character descriptor; binary payloads use binary descriptor.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR lob.locator runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed locator descriptor, non-positive offset, negative length, or unsupported open mode refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select locator(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.locator.locator |
| UUID | 019dffbb-f000-7c6d-89a8-7ef94e5d0d29 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.locator_locator.v3 |
| AST binding | ast.expr.locator_locator |
| Engine entrypoint | locator |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC055-locator-marker`.

### `locator_validity`

**Purpose:** Operates on an engine-managed large-object locator for `locator validity` behavior.

**Call Forms:**

- `locator_validity(locator)`
- Syntax category: `function_call`

**Parameters:**

- `locator`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one locator descriptor.
- Coercion: LOB locator arguments are bounded JSON descriptors; data accepts binary or text scalar values; offsets and lengths are int64.
- NULL handling: SQL NULL locator input refuses for required-locator operations and returns empty marker values for marker forms.

**Returns:**

boolean locator validity.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for supplied descriptors.
- Side effects: bounded descriptor-only LOB state transition; no persistent mutation.
- Collation/charset: text LOB payloads use UTF-8 character descriptor; binary payloads use binary descriptor.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR lob.locator runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed locator descriptor, non-positive offset, negative length, or unsupported open mode refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select locator_validity(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.locator.validity |
| UUID | 019dffbb-f000-721e-9794-a0b7d2899ba1 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.locator_validity.v3 |
| AST binding | ast.expr.locator_validity |
| Engine entrypoint | locator_validity |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC055-locator-validity-arg`.

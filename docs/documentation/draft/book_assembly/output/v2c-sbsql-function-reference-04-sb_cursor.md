

===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/functional_reference/sb_cursor.md -->

<a id="ch-language-reference-functional-reference-sb-cursor-md"></a>

# SB Cursor Functional Reference

Generation task: `sb_cursor`

Package namespace: `sb.cursor`

Cursor, stream, rowset-handle, and table-value conversion helpers used by procedural SQL and streaming execution.

## How To Read This Page

Exposes cursor and stream state to procedural SQL and table-function bridges. Handles are scoped to the owning session or routine context.

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
| scalar | 15 |

## Operation Reference

### `current_row_locator`

**Purpose:** Operates on an engine-managed large-object locator for `current row locator` behavior.

**Call Forms:**

- `current_row_locator(cursor)`
- Syntax category: `function_call`

**Parameters:**

- `cursor`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one cursor descriptor.
- Coercion: cursor, stream, rowset, table_value, and locator arguments use bounded JSON execution-handle descriptors; optional max_rows arguments are int64 descriptors.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor conversion routes; cursor_active(NULL) returns false.

**Returns:**

locator JSON descriptor for current cursor row.

**Behavior:**

- Volatility: stable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: bounded descriptor-only state transition for close/open helpers; no persistent mutation.
- Collation/charset: descriptor payloads are deterministic UTF-8 JSON text; handle attribute values use exact text tokens.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR cursor.stream runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage scan, cursor backend, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed handle descriptor, wrong descriptor kind, or negative max_rows refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select current_row_locator(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.cursor.current_row_locator |
| UUID | 019dffbb-f000-70a0-ae1b-b1c57c9b32e7 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.cursor_current_row_locator.v3 |
| AST binding | ast.expr.cursor_current_row_locator |
| Engine entrypoint | current_row_locator |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC054-current-row-locator-cursor`.

### `cursor_active`

**Purpose:** Reads or transforms cursor/stream state for `cursor active` behavior.

**Call Forms:**

- `cursor_active(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: optional cursor name or descriptor.
- Coercion: cursor, stream, rowset, table_value, and locator arguments use bounded JSON execution-handle descriptors; optional max_rows arguments are int64 descriptors.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor conversion routes; cursor_active(NULL) returns false.

**Returns:**

boolean active state.

**Behavior:**

- Volatility: stable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: bounded descriptor-only state transition for close/open helpers; no persistent mutation.
- Collation/charset: descriptor payloads are deterministic UTF-8 JSON text; handle attribute values use exact text tokens.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR cursor.stream runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage scan, cursor backend, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed handle descriptor, wrong descriptor kind, or negative max_rows refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select cursor_active(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.cursor.active |
| UUID | 019dffbb-f000-7783-a337-566f9fad37d2 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.cursor_active.v3 |
| AST binding | ast.expr.cursor_active |
| Engine entrypoint | cursor_active |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC054-cursor-active-marker`.

### `cursor_close`

**Purpose:** Reads or transforms cursor/stream state for `cursor close` behavior.

**Call Forms:**

- `cursor_close(cursor)`
- Syntax category: `function_call`

**Parameters:**

- `cursor`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one cursor descriptor.
- Coercion: cursor, stream, rowset, table_value, and locator arguments use bounded JSON execution-handle descriptors; optional max_rows arguments are int64 descriptors.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor conversion routes; cursor_active(NULL) returns false.

**Returns:**

cursor JSON descriptor in closed state.

**Behavior:**

- Volatility: stable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: bounded descriptor-only state transition for close/open helpers; no persistent mutation.
- Collation/charset: descriptor payloads are deterministic UTF-8 JSON text; handle attribute values use exact text tokens.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR cursor.stream runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage scan, cursor backend, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed handle descriptor, wrong descriptor kind, or negative max_rows refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select cursor_close(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.cursor.close |
| UUID | 019dffbb-f000-7c51-8503-c6f3d4045f94 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.cursor_close.v3 |
| AST binding | ast.expr.cursor_close |
| Engine entrypoint | cursor_close |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC054-cursor-close-arg`.

### `cursor_holdability`

**Purpose:** Reads or transforms cursor/stream state for `cursor holdability` behavior.

**Call Forms:**

- `cursor_holdability(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: optional cursor descriptor.
- Coercion: cursor, stream, rowset, table_value, and locator arguments use bounded JSON execution-handle descriptors; optional max_rows arguments are int64 descriptors.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor conversion routes; cursor_active(NULL) returns false.

**Returns:**

character holdability token.

**Behavior:**

- Volatility: stable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: bounded descriptor-only state transition for close/open helpers; no persistent mutation.
- Collation/charset: descriptor payloads are deterministic UTF-8 JSON text; handle attribute values use exact text tokens.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR cursor.stream runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage scan, cursor backend, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed handle descriptor, wrong descriptor kind, or negative max_rows refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select cursor_holdability(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.cursor.holdability |
| UUID | 019dffbb-f000-7ca2-a240-a967ab4e34fc |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.cursor_holdability.v3 |
| AST binding | ast.expr.cursor_holdability |
| Engine entrypoint | cursor_holdability |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC054-cursor-holdability-marker`.

### `cursor_lifetime_class`

**Purpose:** Reads or transforms cursor/stream state for `cursor lifetime class` behavior.

**Call Forms:**

- `cursor_lifetime_class(cursor)`
- Syntax category: `function_call`

**Parameters:**

- `cursor`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one cursor descriptor.
- Coercion: cursor, stream, rowset, table_value, and locator arguments use bounded JSON execution-handle descriptors; optional max_rows arguments are int64 descriptors.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor conversion routes; cursor_active(NULL) returns false.

**Returns:**

character lifetime token.

**Behavior:**

- Volatility: stable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: bounded descriptor-only state transition for close/open helpers; no persistent mutation.
- Collation/charset: descriptor payloads are deterministic UTF-8 JSON text; handle attribute values use exact text tokens.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR cursor.stream runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage scan, cursor backend, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed handle descriptor, wrong descriptor kind, or negative max_rows refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select cursor_lifetime_class(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.cursor.lifetime_class |
| UUID | 019dffbb-f000-744e-8752-29a69c6ca5fb |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.cursor_lifetime_class.v3 |
| AST binding | ast.expr.cursor_lifetime_class |
| Engine entrypoint | cursor_lifetime_class |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC054-cursor-lifetime-class-arg`.

### `cursor_open`

**Purpose:** Reads or transforms cursor/stream state for `cursor open` behavior.

**Call Forms:**

- `cursor_open(<select>)`
- Syntax category: `function_call`

**Parameters:**

- `<select>`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: optional select descriptor.
- Coercion: cursor, stream, rowset, table_value, and locator arguments use bounded JSON execution-handle descriptors; optional max_rows arguments are int64 descriptors.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor conversion routes; cursor_active(NULL) returns false.

**Returns:**

cursor JSON descriptor in open state.

**Behavior:**

- Volatility: stable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: bounded descriptor-only state transition for close/open helpers; no persistent mutation.
- Collation/charset: descriptor payloads are deterministic UTF-8 JSON text; handle attribute values use exact text tokens.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR cursor.stream runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage scan, cursor backend, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed handle descriptor, wrong descriptor kind, or negative max_rows refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select cursor_open(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.cursor.open |
| UUID | 019dffbb-f000-75d7-b434-542598b842ec |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.cursor_open.v3 |
| AST binding | ast.expr.cursor_open |
| Engine entrypoint | cursor_open |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC054-cursor-open-select`.

### `cursor_position`

**Purpose:** Reads or transforms cursor/stream state for `cursor position` behavior.

**Call Forms:**

- `cursor_position(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: optional cursor descriptor.
- Coercion: cursor, stream, rowset, table_value, and locator arguments use bounded JSON execution-handle descriptors; optional max_rows arguments are int64 descriptors.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor conversion routes; cursor_active(NULL) returns false.

**Returns:**

int64 cursor position.

**Behavior:**

- Volatility: stable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: bounded descriptor-only state transition for close/open helpers; no persistent mutation.
- Collation/charset: descriptor payloads are deterministic UTF-8 JSON text; handle attribute values use exact text tokens.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR cursor.stream runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage scan, cursor backend, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed handle descriptor, wrong descriptor kind, or negative max_rows refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select cursor_position(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.cursor.position |
| UUID | 019dffbb-f000-7457-b674-79e5e9801f8a |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.cursor_position.v3 |
| AST binding | ast.expr.cursor_position |
| Engine entrypoint | cursor_position |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC054-cursor-position-marker`.

### `cursor_scrollability`

**Purpose:** Reads or transforms cursor/stream state for `cursor scrollability` behavior.

**Call Forms:**

- `cursor_scrollability(cursor)`
- Syntax category: `function_call`

**Parameters:**

- `cursor`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one cursor descriptor.
- Coercion: cursor, stream, rowset, table_value, and locator arguments use bounded JSON execution-handle descriptors; optional max_rows arguments are int64 descriptors.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor conversion routes; cursor_active(NULL) returns false.

**Returns:**

character scrollability token.

**Behavior:**

- Volatility: stable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: bounded descriptor-only state transition for close/open helpers; no persistent mutation.
- Collation/charset: descriptor payloads are deterministic UTF-8 JSON text; handle attribute values use exact text tokens.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR cursor.stream runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage scan, cursor backend, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed handle descriptor, wrong descriptor kind, or negative max_rows refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select cursor_scrollability(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.cursor.scrollability |
| UUID | 019dffbb-f000-7967-8cca-e48403bb663b |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.cursor_scrollability.v3 |
| AST binding | ast.expr.cursor_scrollability |
| Engine entrypoint | cursor_scrollability |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC054-cursor-scrollability-arg`.

### `cursor_state`

**Purpose:** Reads or transforms cursor/stream state for `cursor state` behavior.

**Call Forms:**

- `cursor_state(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: optional cursor descriptor.
- Coercion: cursor, stream, rowset, table_value, and locator arguments use bounded JSON execution-handle descriptors; optional max_rows arguments are int64 descriptors.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor conversion routes; cursor_active(NULL) returns false.

**Returns:**

character state token.

**Behavior:**

- Volatility: stable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: bounded descriptor-only state transition for close/open helpers; no persistent mutation.
- Collation/charset: descriptor payloads are deterministic UTF-8 JSON text; handle attribute values use exact text tokens.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR cursor.stream runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage scan, cursor backend, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed handle descriptor, wrong descriptor kind, or negative max_rows refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select cursor_state(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.cursor.state |
| UUID | 019dffbb-f000-7f46-845b-79faabb48810 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.cursor_state.v3 |
| AST binding | ast.expr.cursor_state |
| Engine entrypoint | cursor_state |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC054-cursor-state-marker`.

### `cursor_to_rowset`

**Purpose:** Reads or transforms cursor/stream state for `cursor to rowset` behavior.

**Call Forms:**

- `cursor_to_rowset(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: cursor descriptor and optional non-negative max_rows.
- Coercion: cursor, stream, rowset, table_value, and locator arguments use bounded JSON execution-handle descriptors; optional max_rows arguments are int64 descriptors.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor conversion routes; cursor_active(NULL) returns false.

**Returns:**

rowset JSON descriptor containing cursor rows.

**Behavior:**

- Volatility: stable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: bounded descriptor-only state transition for close/open helpers; no persistent mutation.
- Collation/charset: descriptor payloads are deterministic UTF-8 JSON text; handle attribute values use exact text tokens.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR cursor.stream runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage scan, cursor backend, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed handle descriptor, wrong descriptor kind, or negative max_rows refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select cursor_to_rowset(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.cursor.to_rowset |
| UUID | 019dffbb-f000-7e9c-9854-eaabf1cab19b |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.cursor_to_rowset.v3 |
| AST binding | ast.expr.cursor_to_rowset |
| Engine entrypoint | cursor_to_rowset |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC054-cursor-to-rowset-marker`.

### `handle_kind`

**Purpose:** Evaluates `handle_kind` and returns character handle-kind token.

**Call Forms:**

- `handle_kind(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: optional handle descriptor.
- Coercion: cursor, stream, rowset, table_value, and locator arguments use bounded JSON execution-handle descriptors; optional max_rows arguments are int64 descriptors.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor conversion routes; cursor_active(NULL) returns false.

**Returns:**

character handle-kind token.

**Behavior:**

- Volatility: stable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: bounded descriptor-only state transition for close/open helpers; no persistent mutation.
- Collation/charset: descriptor payloads are deterministic UTF-8 JSON text; handle attribute values use exact text tokens.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR cursor.stream runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage scan, cursor backend, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed handle descriptor, wrong descriptor kind, or negative max_rows refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select handle_kind(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.handle.kind |
| UUID | 019dffbb-f000-78d1-817e-ba6064284df9 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.handle_kind.v3 |
| AST binding | ast.expr.handle_kind |
| Engine entrypoint | handle_kind |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC054-handle-kind-marker`.

### `rowset_to_cursor`

**Purpose:** Reads or transforms cursor/stream state for `rowset to cursor` behavior.

**Call Forms:**

- `rowset_to_cursor(rowset)`
- Syntax category: `function_call`

**Parameters:**

- `rowset`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one rowset descriptor.
- Coercion: cursor, stream, rowset, table_value, and locator arguments use bounded JSON execution-handle descriptors; optional max_rows arguments are int64 descriptors.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor conversion routes; cursor_active(NULL) returns false.

**Returns:**

cursor JSON descriptor backed by rowset rows.

**Behavior:**

- Volatility: stable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: bounded descriptor-only state transition for close/open helpers; no persistent mutation.
- Collation/charset: descriptor payloads are deterministic UTF-8 JSON text; handle attribute values use exact text tokens.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR cursor.stream runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage scan, cursor backend, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed handle descriptor, wrong descriptor kind, or negative max_rows refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select rowset_to_cursor(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.cursor.rowset_to_cursor |
| UUID | 019dffbb-f000-7f64-93a7-b921be1ae581 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.cursor_rowset_to_cursor.v3 |
| AST binding | ast.expr.cursor_rowset_to_cursor |
| Engine entrypoint | rowset_to_cursor |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC054-rowset-to-cursor-arg`.

### `stream_close`

**Purpose:** Reads or transforms cursor/stream state for `stream close` behavior.

**Call Forms:**

- `stream_close(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: optional stream descriptor.
- Coercion: cursor, stream, rowset, table_value, and locator arguments use bounded JSON execution-handle descriptors; optional max_rows arguments are int64 descriptors.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor conversion routes; cursor_active(NULL) returns false.

**Returns:**

stream JSON descriptor in closed state.

**Behavior:**

- Volatility: stable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: bounded descriptor-only state transition for close/open helpers; no persistent mutation.
- Collation/charset: descriptor payloads are deterministic UTF-8 JSON text; handle attribute values use exact text tokens.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR cursor.stream runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage scan, cursor backend, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed handle descriptor, wrong descriptor kind, or negative max_rows refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select stream_close(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.stream.close |
| UUID | 019dffbb-f000-7883-9e52-f39d10c0e0f3 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.stream_close.v3 |
| AST binding | ast.expr.stream_close |
| Engine entrypoint | stream_close |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC054-stream-close-marker`.

### `stream_to_rowset`

**Purpose:** Reads or transforms cursor/stream state for `stream to rowset` behavior.

**Call Forms:**

- `stream_to_rowset(stream[,max_rows])`
- Syntax category: `function_call`

**Parameters:**

- `stream[`: Bound using the declared descriptor rules for this overload.
- `max_rows]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: stream descriptor and optional non-negative max_rows.
- Coercion: cursor, stream, rowset, table_value, and locator arguments use bounded JSON execution-handle descriptors; optional max_rows arguments are int64 descriptors.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor conversion routes; cursor_active(NULL) returns false.

**Returns:**

rowset JSON descriptor containing stream rows.

**Behavior:**

- Volatility: stable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: bounded descriptor-only state transition for close/open helpers; no persistent mutation.
- Collation/charset: descriptor payloads are deterministic UTF-8 JSON text; handle attribute values use exact text tokens.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR cursor.stream runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage scan, cursor backend, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed handle descriptor, wrong descriptor kind, or negative max_rows refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select stream_to_rowset(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.stream.to_rowset |
| UUID | 019dffbb-f000-742f-9f33-bad0bf5a0fb1 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.stream_to_rowset.v3 |
| AST binding | ast.expr.stream_to_rowset |
| Engine entrypoint | stream_to_rowset |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC054-stream-to-rowset-args`.

### `table_value_to_cursor`

**Purpose:** Reads or transforms cursor/stream state for `table value to cursor` behavior.

**Call Forms:**

- `table_value_to_cursor(tv)`
- Syntax category: `function_call`

**Parameters:**

- `tv`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one table_value descriptor.
- Coercion: cursor, stream, rowset, table_value, and locator arguments use bounded JSON execution-handle descriptors; optional max_rows arguments are int64 descriptors.
- NULL handling: SQL NULL descriptor input returns SQL NULL for descriptor conversion routes; cursor_active(NULL) returns false.

**Returns:**

cursor JSON descriptor backed by table_value rows.

**Behavior:**

- Volatility: stable.
- Determinism: deterministic for supplied descriptor/scalar inputs.
- Side effects: bounded descriptor-only state transition for close/open helpers; no persistent mutation.
- Collation/charset: descriptor payloads are deterministic UTF-8 JSON text; handle attribute values use exact text tokens.
- Timezone: not applicable.
- Security and authority: executes inside the SBLR cursor.stream runtime using bounded in-core descriptor helpers; no parser SQL execution, external execution, storage scan, cursor backend, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed handle descriptor, wrong descriptor kind, or negative max_rows refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select table_value_to_cursor(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.cursor.table_value_to_cursor |
| UUID | 019dffbb-f000-7621-8bcb-dc9af37c15a9 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.cursor_table_value_to_cursor.v3 |
| AST binding | ast.expr.cursor_table_value_to_cursor |
| Engine entrypoint | table_value_to_cursor |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC054-table-value-to-cursor-arg`.


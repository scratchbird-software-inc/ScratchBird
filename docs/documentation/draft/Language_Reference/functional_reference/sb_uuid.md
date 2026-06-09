# SB UUID Functional Reference

Generation task: `sb_uuid`

Package namespace: `sb.uuid`

UUID generation, parsing, inspection, and timestamp extraction helpers.

## How To Read This Page

Generates and inspects UUID values without assigning catalog identity unless the caller uses the value in a catalog operation.

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
| scalar | 10 |

## Operation Reference

### `uuid_generate_v1`

**Purpose:** Generates a UUID value using the version-1-compatible generator surface.

**Call Forms:**

- `uuid_generate_v1()`
- Syntax category: `function_call`

**Parameters:**

- This function takes no arguments.
- Descriptor rule: descriptor_authoritative UUID compatibility helper arguments.
- Coercion: use descriptor implicit cast matrix; UUID text inputs must canonicalize before UUID helper semantics apply.
- NULL handling: not applicable; nullary function.

**Returns:**

UUID v1-compatible value; descriptor=uuid.

**Behavior:**

- Volatility: volatile.
- Determinism: not foldable; generates a new UUID unless deterministic fixture context overrides.
- Side effects: volatile_value_generation.
- Collation/charset: uses byte-stable canonical UUID text where text parsing is required; otherwise not applicable.
- Timezone: UTC timestamp_tz rendering for UUID timestamp extraction; otherwise not applicable.
- Security and authority: none; helper executes under SBLR descriptor authority and does not allocate engine identity unless explicitly used by caller context.

**Errors:**

arity, invalid UUID text, unsupported UUID timestamp version, and domain errors use canonical UUID helper diagnostics from the conformance fixtures fixtures.

**Example:**

```sql
select uuid_generate_v1() as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.uuid.generate_v1 |
| UUID | 019dffbb-f000-7ebb-8eec-060c8b4adbb9 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.uuid_generate_v1.v3 |
| AST binding | ast.expr.uuid_generate_v1 |
| Engine entrypoint | uuid_generate_v1 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | volatile_scalar |

Conformance evidence: `SBSFC028-uuid-generate-v1`.

### `uuid_generate_v3`

**Purpose:** Generates a deterministic UUID from a namespace UUID and name using the version-3-compatible surface.

**Call Forms:**

- `uuid_generate_v3(namespace,name)`
- Syntax category: `function_call`

**Parameters:**

- `namespace`: Bound using the declared descriptor rules for this overload.
- `name`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative UUID compatibility helper arguments.
- Coercion: use descriptor implicit cast matrix; UUID text inputs must canonicalize before UUID helper semantics apply.
- NULL handling: strict; NULL namespace or name returns NULL uuid.

**Returns:**

deterministic UUID v3 from namespace UUID and name; descriptor=uuid.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when namespace and name are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses byte-stable canonical UUID text where text parsing is required; otherwise not applicable.
- Timezone: UTC timestamp_tz rendering for UUID timestamp extraction; otherwise not applicable.
- Security and authority: none; helper executes under SBLR descriptor authority and does not allocate engine identity unless explicitly used by caller context.

**Errors:**

arity, invalid UUID text, unsupported UUID timestamp version, and domain errors use canonical UUID helper diagnostics from the conformance fixtures fixtures.

**Example:**

```sql
select uuid_generate_v3(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.uuid.generate_v3 |
| UUID | 019dffbb-f000-765c-a462-4dac8737d001 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.uuid_generate_v3.v3 |
| AST binding | ast.expr.uuid_generate_v3 |
| Engine entrypoint | uuid_generate_v3 |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC028-uuid-generate-v3-arity;SBSFC028-uuid-generate-v3-dns-example;SBSFC028-uuid-generate-v3-null;SBSFC028-uuid-generate-v3-invalid-namespace`.

### `uuid_generate_v4`

**Purpose:** Generates a random UUID value using the version-4-compatible generator surface.

**Call Forms:**

- `uuid_generate_v4()`
- Syntax category: `function_call`

**Parameters:**

- This function takes no arguments.
- Descriptor rule: descriptor_authoritative UUID compatibility helper arguments.
- Coercion: use descriptor implicit cast matrix; UUID text inputs must canonicalize before UUID helper semantics apply.
- NULL handling: not applicable; nullary function.

**Returns:**

random UUID v4 value; descriptor=uuid.

**Behavior:**

- Volatility: volatile.
- Determinism: not foldable; generates a new UUID unless deterministic fixture context overrides.
- Side effects: volatile_value_generation.
- Collation/charset: uses byte-stable canonical UUID text where text parsing is required; otherwise not applicable.
- Timezone: UTC timestamp_tz rendering for UUID timestamp extraction; otherwise not applicable.
- Security and authority: none; helper executes under SBLR descriptor authority and does not allocate engine identity unless explicitly used by caller context.

**Errors:**

arity, invalid UUID text, unsupported UUID timestamp version, and domain errors use canonical UUID helper diagnostics from the conformance fixtures fixtures.

**Example:**

```sql
select uuid_generate_v4() as new_uuid;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.uuid.generate_v4 |
| UUID | 019dffbb-f000-7e63-b855-587564821ba2 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.uuid_generate_v4.v3 |
| AST binding | ast.expr.uuid_generate_v4 |
| Engine entrypoint | uuid_generate_v4 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | volatile_scalar |

Conformance evidence: `SBSFC028-uuid-generate-v4`.

### `uuid_generate_v5`

**Purpose:** Generates a deterministic UUID from a namespace UUID and name using the version-5-compatible surface.

**Call Forms:**

- `uuid_generate_v5(namespace,name)`
- Syntax category: `function_call`

**Parameters:**

- `namespace`: Bound using the declared descriptor rules for this overload.
- `name`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative UUID compatibility helper arguments.
- Coercion: use descriptor implicit cast matrix; UUID text inputs must canonicalize before UUID helper semantics apply.
- NULL handling: strict; NULL namespace or name returns NULL uuid.

**Returns:**

deterministic UUID v5 from namespace UUID and name; descriptor=uuid.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when namespace and name are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses byte-stable canonical UUID text where text parsing is required; otherwise not applicable.
- Timezone: UTC timestamp_tz rendering for UUID timestamp extraction; otherwise not applicable.
- Security and authority: none; helper executes under SBLR descriptor authority and does not allocate engine identity unless explicitly used by caller context.

**Errors:**

arity, invalid UUID text, unsupported UUID timestamp version, and domain errors use canonical UUID helper diagnostics from the conformance fixtures fixtures.

**Example:**

```sql
select uuid_generate_v5(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.uuid.generate_v5 |
| UUID | 019dffbb-f000-7f42-b1e9-618f013887b3 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.uuid_generate_v5.v3 |
| AST binding | ast.expr.uuid_generate_v5 |
| Engine entrypoint | uuid_generate_v5 |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC028-uuid-generate-v5-arity;SBSFC028-uuid-generate-v5-dns-example;SBSFC028-uuid-generate-v5-null;SBSFC028-uuid-generate-v5-invalid-namespace`.

### `uuid_nil`

**Purpose:** Returns the nil UUID value.

**Call Forms:**

- `uuid_nil()`
- Syntax category: `function_call`

**Parameters:**

- This function takes no arguments.
- Descriptor rule: descriptor_authoritative UUID compatibility helper arguments.
- Coercion: use descriptor implicit cast matrix; UUID text inputs must canonicalize before UUID helper semantics apply.
- NULL handling: not applicable; nullary function.

**Returns:**

nil UUID constant; descriptor=uuid.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable; returns a constant nil UUID.
- Side effects: none.
- Collation/charset: uses byte-stable canonical UUID text where text parsing is required; otherwise not applicable.
- Timezone: UTC timestamp_tz rendering for UUID timestamp extraction; otherwise not applicable.
- Security and authority: none; helper executes under SBLR descriptor authority and does not allocate engine identity unless explicitly used by caller context.

**Errors:**

arity, invalid UUID text, unsupported UUID timestamp version, and domain errors use canonical UUID helper diagnostics from the conformance fixtures fixtures.

**Example:**

```sql
select uuid_nil() as nil_uuid;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.uuid.nil |
| UUID | 019dffbb-f000-761e-9280-aefd19556637 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.uuid_nil.v3 |
| AST binding | ast.expr.uuid_nil |
| Engine entrypoint | uuid_nil |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC028-uuid-nil;SBSFC028-uuid-nil-arity`.

### `uuid_timestamp`

**Purpose:** Extracts the timestamp component from UUID versions that carry one.

**Call Forms:**

- `uuid_timestamp(uuid)`
- Syntax category: `function_call`

**Parameters:**

- `uuid`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative UUID compatibility helper arguments.
- Coercion: use descriptor implicit cast matrix; UUID text inputs must canonicalize before UUID helper semantics apply.
- NULL handling: strict; NULL UUID returns NULL timestamp_tz.

**Returns:**

timestamp_tz extracted from UUID v1 or v7 embedded timestamp.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when UUID argument is constant with stable descriptor.
- Side effects: none.
- Collation/charset: uses byte-stable canonical UUID text where text parsing is required; otherwise not applicable.
- Timezone: UTC timestamp_tz rendering for UUID timestamp extraction; otherwise not applicable.
- Security and authority: none; helper executes under SBLR descriptor authority and does not allocate engine identity unless explicitly used by caller context.

**Errors:**

arity, invalid UUID text, unsupported UUID timestamp version, and domain errors use canonical UUID helper diagnostics from the conformance fixtures fixtures.

**Example:**

```sql
select uuid_timestamp(uuid_generate_v1()) as uuid_time;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.uuid.timestamp |
| UUID | 019dffbb-f000-7184-bffb-d4c74e43d38b |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.uuid_timestamp.v3 |
| AST binding | ast.expr.uuid_timestamp |
| Engine entrypoint | uuid_timestamp |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC028-uuid-timestamp-v1;SBSFC028-uuid-timestamp-v7;SBSFC028-uuid-timestamp-null;SBSFC028-uuid-timestamp-invalid;SBSFC028-uuid-timestamp-unsupported-version;SBSFC028-uuid-timestamp-arity`.

### `uuid_v1`

**Purpose:** Generates or normalizes a version-1-compatible UUID value.

**Call Forms:**

- `uuid_v1()`
- Syntax category: `function_call`

**Parameters:**

- This function takes no arguments.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless noted by function-specific semantics.

**Returns:**

compatibility UUID generator; not engine identity.

**Behavior:**

- Volatility: volatile.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: volatile_value_generation.
- Collation/charset: uses input descriptor collation/charset where string semantics apply.
- Timezone: session timezone for temporal forms; not applicable otherwise.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/overflow/domain errors use builtin error compatibility matrix.

**Example:**

```sql
select uuid_v1() as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.uuid.v1 |
| UUID | 019de5fc-2400-7feb-b0a9-3353de7091ed |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.uuid_v1.v3 |
| AST binding | ast.expr.uuid_v1 |
| Engine entrypoint | uuid_v1 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | cpu_scalar |

Conformance evidence: `BIF-CONF-454332ee9ff7;SBSFC012-sb-uuid-v1-wrapper`.

### `uuid_v4`

**Purpose:** Generates a version-4 random UUID value.

**Call Forms:**

- `uuid_v4()`
- Syntax category: `function_call`

**Parameters:**

- This function takes no arguments.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless noted by function-specific semantics.

**Returns:**

random UUID generator; not engine identity.

**Behavior:**

- Volatility: volatile.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: volatile_value_generation.
- Collation/charset: uses input descriptor collation/charset where string semantics apply.
- Timezone: session timezone for temporal forms; not applicable otherwise.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/overflow/domain errors use builtin error compatibility matrix.

**Example:**

```sql
select uuid_v4() as result_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.uuid.v4 |
| UUID | 019de5fc-2400-7940-8ecf-0c79114eb869 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.uuid_v4.v3 |
| AST binding | ast.expr.uuid_v4 |
| Engine entrypoint | uuid_v4 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | cpu_scalar |

Conformance evidence: `BIF-CONF-3e9192b7d40c;SBSFC012-sb-uuid-v4-wrapper`.

### `uuid_v7`

**Purpose:** Generates a version-7 time-ordered UUID value.

**Call Forms:**

- `uuid_v7()`
- Syntax category: `function_call`

**Parameters:**

- This function takes no arguments.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless noted by function-specific semantics.

**Returns:**

engine identity UUID generator; v7 only for engine identities.

**Behavior:**

- Volatility: volatile.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: volatile_value_generation.
- Collation/charset: uses input descriptor collation/charset where string semantics apply.
- Timezone: session timezone for temporal forms; not applicable otherwise.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/overflow/domain errors use builtin error compatibility matrix.

**Example:**

```sql
select uuid_v7() as ordered_uuid;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.uuid.v7 |
| UUID | 019de5fc-2400-7fe3-a3d5-21174bd948db |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.uuid_v7.v3 |
| AST binding | ast.expr.uuid_v7 |
| Engine entrypoint | uuid_v7 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | cpu_scalar |

Conformance evidence: `BIF-CONF-87b293759d19;SBSFC012-sb-uuid-v7-wrapper`.

### `uuid_version`

**Purpose:** Returns the version field from a UUID value.

**Call Forms:**

- `uuid_version(uuid)`
- Syntax category: `function_call`

**Parameters:**

- `uuid`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative UUID compatibility helper arguments.
- Coercion: use descriptor implicit cast matrix; UUID text inputs must canonicalize before UUID helper semantics apply.
- NULL handling: strict; NULL UUID returns NULL int64.

**Returns:**

UUID version nibble as int64; nil UUID returns 0.

**Behavior:**

- Volatility: immutable.
- Determinism: foldable when UUID argument is constant with stable descriptor.
- Side effects: none.
- Collation/charset: uses byte-stable canonical UUID text where text parsing is required; otherwise not applicable.
- Timezone: UTC timestamp_tz rendering for UUID timestamp extraction; otherwise not applicable.
- Security and authority: none; helper executes under SBLR descriptor authority and does not allocate engine identity unless explicitly used by caller context.

**Errors:**

arity, invalid UUID text, unsupported UUID timestamp version, and domain errors use canonical UUID helper diagnostics from the conformance fixtures fixtures.

**Example:**

```sql
select uuid_version(cast('00000000-0000-4000-8000-000000000000' as uuid)) as version_number;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.uuid.version |
| UUID | 019dffbb-f000-773e-929d-76fcd97ac981 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.uuid_version.v3 |
| AST binding | ast.expr.uuid_version |
| Engine entrypoint | uuid_version |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC028-uuid-version-v1;SBSFC028-uuid-version-v4;SBSFC028-uuid-version-nil;SBSFC028-uuid-version-null;SBSFC028-uuid-version-invalid`.

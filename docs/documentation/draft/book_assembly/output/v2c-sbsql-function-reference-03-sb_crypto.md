

===== FILE SEPARATION =====

<!-- chapter source: Language_Reference/functional_reference/sb_crypto.md -->

<a id="ch-language-reference-functional-reference-sb-crypto-md"></a>

# SB Crypto Functional Reference

Generation task: `sb_crypto`

Package namespace: `sb.crypto`

Cryptographic, hashing, random-value, armor, and bounded encryption helper functions.

## How To Read This Page

Executes bounded cryptographic helper work inside the engine runtime. These functions do not grant secret access by themselves; statements still require descriptor, policy, and privilege admission.

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
| scalar | 27 |

## Operation Reference

### `argon2`

**Purpose:** Evaluates `argon2` and returns dependency-unavailable diagnostic.

**Call Forms:**

- `argon2(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: provider-gated password/hash inputs.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

dependency-unavailable diagnostic.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

provider-gated row refuses with SBSQL.FUNCTION.DEPENDENCY_UNAVAILABLE.

**Example:**

```sql
select argon2(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.argon2 |
| UUID | 019dffbb-f000-7efc-b382-c6063ea9c79e |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_argon2.v3 |
| AST binding | ast.expr.crypto_argon2 |
| Engine entrypoint | argon2 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-argon2-provider-refuses`.

### `armor`

**Purpose:** Evaluates `armor` and returns deterministic ScratchBird ASCII armor text.

**Call Forms:**

- `armor(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: one text or binary value.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

deterministic ScratchBird ASCII armor text.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select armor(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.armor |
| UUID | 019dffbb-f000-74ec-bbd9-4ae6d355fd05 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_armor.v3 |
| AST binding | ast.expr.crypto_armor |
| Engine entrypoint | armor |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-armor-text`.

### `armor_binary`

**Purpose:** Evaluates `armor_binary` and returns deterministic ScratchBird ASCII armor text.

**Call Forms:**

- `armor(binary)`
- Syntax category: `function_call`

**Parameters:**

- `binary`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one binary value.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

deterministic ScratchBird ASCII armor text.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select armor_binary(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.armor_binary |
| UUID | 019dffbb-f000-77be-91e5-aeeac55d9b74 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_armor_binary.v3 |
| AST binding | ast.expr.crypto_armor_binary |
| Engine entrypoint | armor_binary |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-armor-binary`.

### `bcrypt`

**Purpose:** Evaluates `bcrypt` and returns dependency-unavailable diagnostic.

**Call Forms:**

- `bcrypt(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: provider-gated password/hash inputs.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

dependency-unavailable diagnostic.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

provider-gated row refuses with SBSQL.FUNCTION.DEPENDENCY_UNAVAILABLE.

**Example:**

```sql
select bcrypt(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.bcrypt |
| UUID | 019dffbb-f000-7f76-b620-8f7df70d8e21 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_bcrypt.v3 |
| AST binding | ast.expr.crypto_bcrypt |
| Engine entrypoint | bcrypt |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-bcrypt-provider-refuses`.

### `blake2b`

**Purpose:** Evaluates `blake2b` and returns openSSL EVP BLAKE2b-512 hex text.

**Call Forms:**

- `blake2b(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: zero or one text/binary value.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

OpenSSL EVP BLAKE2b-512 hex text.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select blake2b(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.blake2b |
| UUID | 019dffbb-f000-748d-902f-4c382da1b5c4 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_blake2b.v3 |
| AST binding | ast.expr.crypto_blake2b |
| Engine entrypoint | blake2b |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-blake2b-digest`.

### `blake3`

**Purpose:** Evaluates `blake3` and returns dependency-unavailable diagnostic.

**Call Forms:**

- `blake3(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: provider-gated text/binary input.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

dependency-unavailable diagnostic.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

provider-gated row refuses with SBSQL.FUNCTION.DEPENDENCY_UNAVAILABLE.

**Example:**

```sql
select blake3(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.blake3 |
| UUID | 019dffbb-f000-7445-ae37-ff1d4dd2b543 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_blake3.v3 |
| AST binding | ast.expr.crypto_blake3 |
| Engine entrypoint | blake3 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-blake3-provider-refuses`.

### `crypt`

**Purpose:** Evaluates `crypt` and returns dependency-unavailable diagnostic.

**Call Forms:**

- `crypt(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: provider-gated password/hash inputs.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

dependency-unavailable diagnostic.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

provider-gated row refuses with SBSQL.FUNCTION.DEPENDENCY_UNAVAILABLE.

**Example:**

```sql
select crypt(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.crypt |
| UUID | 019dffbb-f000-7245-8496-182b6c947da2 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_crypt.v3 |
| AST binding | ast.expr.crypto_crypt |
| Engine entrypoint | crypt |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-crypt-provider-refuses`.

### `crypt_password_salt`

**Purpose:** Evaluates `crypt_password_salt` and returns dependency-unavailable diagnostic.

**Call Forms:**

- `crypt(password,salt)`
- Syntax category: `function_call`

**Parameters:**

- `password`: Bound using the declared descriptor rules for this overload.
- `salt`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: password and salt.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

dependency-unavailable diagnostic.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

provider-gated row refuses with SBSQL.FUNCTION.DEPENDENCY_UNAVAILABLE.

**Example:**

```sql
select crypt_password_salt(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.crypt_password_salt |
| UUID | 019dffbb-f000-7bc4-a953-87f55445bf3d |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_crypt_password_salt.v3 |
| AST binding | ast.expr.crypto_crypt_password_salt |
| Engine entrypoint | crypt_password_salt |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-crypt-password-salt-provider-refuses`.

### `dearmor`

**Purpose:** Evaluates `dearmor` and returns binary payload bytes.

**Call Forms:**

- `dearmor(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: one ScratchBird armor/base64 text.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

binary payload bytes.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select dearmor(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.dearmor |
| UUID | 019dffbb-f000-79d8-898d-abd8629b348c |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_dearmor.v3 |
| AST binding | ast.expr.crypto_dearmor |
| Engine entrypoint | dearmor |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-dearmor-armor`.

### `dearmor_text`

**Purpose:** Evaluates `dearmor_text` and returns binary payload bytes.

**Call Forms:**

- `dearmor(text)`
- Syntax category: `function_call`

**Parameters:**

- `text`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: one ScratchBird armor/base64 text.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

binary payload bytes.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select dearmor_text(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.dearmor_text |
| UUID | 019dffbb-f000-7d35-8581-6f080dd3fdca |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_dearmor_text.v3 |
| AST binding | ast.expr.crypto_dearmor_text |
| Engine entrypoint | dearmor_text |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-dearmor-base64`.

### `gen_random_bytes`

**Purpose:** Evaluates `gen_random_bytes` and returns binary random bytes.

**Call Forms:**

- `gen_random_bytes(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: optional bounded byte count.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

binary random bytes.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select gen_random_bytes(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.gen_random_bytes |
| UUID | 019dffbb-f000-7c39-a9f4-ac2b7f953aa4 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_gen_random_bytes.v3 |
| AST binding | ast.expr.crypto_gen_random_bytes |
| Engine entrypoint | gen_random_bytes |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-gen-random-bytes-default`.

### `gen_random_bytes_n`

**Purpose:** Evaluates `gen_random_bytes_n` and returns binary random bytes.

**Call Forms:**

- `gen_random_bytes(n)`
- Syntax category: `function_call`

**Parameters:**

- `n`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: bounded byte count.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

binary random bytes.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select gen_random_bytes_n(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.gen_random_bytes_n |
| UUID | 019dffbb-f000-7fd1-9f51-e0fbf6fb2b05 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_gen_random_bytes_n.v3 |
| AST binding | ast.expr.crypto_gen_random_bytes_n |
| Engine entrypoint | gen_random_bytes_n |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-gen-random-bytes-n`.

### `gen_random_uuid`

**Purpose:** Evaluates `gen_random_uuid` and returns uuid text.

**Call Forms:**

- `gen_random_uuid(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: no arguments.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

uuid text.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select gen_random_uuid(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.gen_random_uuid |
| UUID | 019dffbb-f000-7615-ba9c-4da476322745 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_gen_random_uuid.v3 |
| AST binding | ast.expr.crypto_gen_random_uuid |
| Engine entrypoint | gen_random_uuid |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-gen-random-uuid`.

### `gen_salt`

**Purpose:** Evaluates `gen_salt` and returns deterministic bf salt descriptor.

**Call Forms:**

- `gen_salt(value_1, value_2)`
- Syntax category: `function_call`

**Parameters:**

- `value_1`: Bound using the declared descriptor rules for this overload.
- `value_2`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: zero or two bounded salt arguments.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

deterministic bf salt descriptor.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select gen_salt(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.gen_salt |
| UUID | 019dffbb-f000-7d06-9aa1-b20c2b865445 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_gen_salt.v3 |
| AST binding | ast.expr.crypto_gen_salt |
| Engine entrypoint | gen_salt |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-gen-salt-default`.

### `gen_salt_algo`

**Purpose:** Evaluates `gen_salt_algo` and returns deterministic md5/bf salt descriptor.

**Call Forms:**

- `gen_salt(algo[,rounds])`
- Syntax category: `function_call`

**Parameters:**

- `algo[`: Bound using the declared descriptor rules for this overload.
- `rounds]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: algorithm and optional rounds.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

deterministic md5/bf salt descriptor.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select gen_salt_algo(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.gen_salt_algo |
| UUID | 019dffbb-f000-798e-9bf8-4e9d1e8a050e |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_gen_salt_algo.v3 |
| AST binding | ast.expr.crypto_gen_salt_algo |
| Engine entrypoint | gen_salt_algo |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-gen-salt-md5`.

### `hmac`

**Purpose:** Evaluates `hmac` and returns marker or OpenSSL HMAC hex text.

**Call Forms:**

- `hmac(value_1, value_2, value_3)`
- Syntax category: `function_call`

**Parameters:**

- `value_1`: Bound using the declared descriptor rules for this overload.
- `value_2`: Bound using the declared descriptor rules for this overload.
- `value_3`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: zero or three value/key/algorithm arguments.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

marker or OpenSSL HMAC hex text.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select hmac(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.hmac |
| UUID | 019dffbb-f000-763a-8f17-c4b8f7b287bc |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_hmac.v3 |
| AST binding | ast.expr.crypto_hmac |
| Engine entrypoint | hmac |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-hmac-marker`.

### `hmac_value_key_algo`

**Purpose:** Evaluates `hmac_value_key_algo` and returns openSSL HMAC hex text.

**Call Forms:**

- `hmac(text\|binary,key,algo)`
- Syntax category: `function_call`

**Parameters:**

- `text\|binary`: Bound using the declared descriptor rules for this overload.
- `key`: Bound using the declared descriptor rules for this overload.
- `algo`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: value, key, and algorithm.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

OpenSSL HMAC hex text.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select hmac_value_key_algo(value_1, value_2, value_3) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.hmac_value_key_algo |
| UUID | 019dffbb-f000-7879-8721-bdbd655a9290 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_hmac_value_key_algo.v3 |
| AST binding | ast.expr.crypto_hmac_value_key_algo |
| Engine entrypoint | hmac_value_key_algo |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-hmac-sha256`.

### `pgcrypto`

**Purpose:** Evaluates `pgcrypto` and returns compatibility envelope marker.

**Call Forms:**

- `pgcrypto(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: no arguments.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

compatibility envelope marker.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select pgcrypto(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.pgcrypto |
| UUID | 019dffbb-f000-725d-94c8-999bca25ec17 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_pgcrypto.v3 |
| AST binding | ast.expr.crypto_pgcrypto |
| Engine entrypoint | pgcrypto |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-pgcrypto-marker`.

### `pgp_pub_decrypt`

**Purpose:** Evaluates `pgp_pub_decrypt` and returns decrypted character payload.

**Call Forms:**

- `pgp_pub_decrypt(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: ScratchBird pub envelope and key.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

decrypted character payload.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select pgp_pub_decrypt(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.pgp_pub_decrypt |
| UUID | 019dffbb-f000-77a1-b3d5-ea26727ce8d8 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_pgp_pub_decrypt.v3 |
| AST binding | ast.expr.crypto_pgp_pub_decrypt |
| Engine entrypoint | pgp_pub_decrypt |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-pgp-pub-decrypt`.

### `pgp_pub_encrypt`

**Purpose:** Evaluates `pgp_pub_encrypt` and returns bounded ScratchBird pub envelope text.

**Call Forms:**

- `pgp_pub_encrypt(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: value and public key descriptor.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

bounded ScratchBird pub envelope text.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select pgp_pub_encrypt(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.pgp_pub_encrypt |
| UUID | 019dffbb-f000-7dd4-968c-1dd3cc62b20c |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_pgp_pub_encrypt.v3 |
| AST binding | ast.expr.crypto_pgp_pub_encrypt |
| Engine entrypoint | pgp_pub_encrypt |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-pgp-pub-encrypt`.

### `pgp_sym_decrypt`

**Purpose:** Evaluates `pgp_sym_decrypt` and returns decrypted character payload.

**Call Forms:**

- `pgp_sym_decrypt(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: ScratchBird sym envelope and key.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

decrypted character payload.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select pgp_sym_decrypt(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.pgp_sym_decrypt |
| UUID | 019dffbb-f000-7307-b5fa-143dc6de25d0 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_pgp_sym_decrypt.v3 |
| AST binding | ast.expr.crypto_pgp_sym_decrypt |
| Engine entrypoint | pgp_sym_decrypt |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-pgp-sym-decrypt`.

### `pgp_sym_encrypt`

**Purpose:** Evaluates `pgp_sym_encrypt` and returns bounded ScratchBird sym envelope text.

**Call Forms:**

- `pgp_sym_encrypt(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: value and symmetric key.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

bounded ScratchBird sym envelope text.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select pgp_sym_encrypt(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.pgp_sym_encrypt |
| UUID | 019dffbb-f000-765b-b72e-a39ab95aade5 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_pgp_sym_encrypt.v3 |
| AST binding | ast.expr.crypto_pgp_sym_encrypt |
| Engine entrypoint | pgp_sym_encrypt |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-pgp-sym-encrypt`.

### `scrypt`

**Purpose:** Evaluates `scrypt` and returns openSSL EVP_PBE_scrypt hex text.

**Call Forms:**

- `scrypt(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: password, salt, and optional bounded parameters.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

OpenSSL EVP_PBE_scrypt hex text.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select scrypt(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.scrypt |
| UUID | 019dffbb-f000-7ea8-b7b5-9b4deee60808 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_scrypt.v3 |
| AST binding | ast.expr.crypto_scrypt |
| Engine entrypoint | scrypt |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-scrypt`.

### `sha3_256`

**Purpose:** Evaluates `sha3_256` and returns openSSL SHA3-256 hex text.

**Call Forms:**

- `sha3_256(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: zero or one text/binary value.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

OpenSSL SHA3-256 hex text.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select sha3_256(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.sha3_256 |
| UUID | 019dffbb-f000-702e-b9a5-28aa6b997f82 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_sha3_256.v3 |
| AST binding | ast.expr.crypto_sha3_256 |
| Engine entrypoint | sha3_256 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-sha3-256`.

### `sha3_512`

**Purpose:** Evaluates `sha3_512` and returns openSSL SHA3-512 hex text.

**Call Forms:**

- `sha3_512(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: zero or one text/binary value.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

OpenSSL SHA3-512 hex text.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select sha3_512(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.sha3_512 |
| UUID | 019dffbb-f000-7f5e-8666-07a0756c9405 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_sha3_512.v3 |
| AST binding | ast.expr.crypto_sha3_512 |
| Engine entrypoint | sha3_512 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-sha3-512`.

### `xxhash64`

**Purpose:** Evaluates `xxhash64` and returns uint64 xxhash64 value.

**Call Forms:**

- `xxhash64(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: zero or one text/binary value.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

uint64 xxhash64 value.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xxhash64(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.xxhash64 |
| UUID | 019dffbb-f000-73ad-b373-adec35c35f74 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_xxhash64.v3 |
| AST binding | ast.expr.crypto_xxhash64 |
| Engine entrypoint | xxhash64 |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-xxhash64`.

### `xxhash64_value_seed`

**Purpose:** Evaluates `xxhash64_value_seed` and returns uint64 xxhash64 value.

**Call Forms:**

- `xxhash64(text\|binary[,seed])`
- Syntax category: `function_call`

**Parameters:**

- `text\|binary[`: Bound using the declared descriptor rules for this overload.
- `seed]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: text/binary value and optional uint64 seed.
- Coercion: bounded text/binary/uint64 argument coercion through SBLR scalar values only.
- NULL handling: NULL inputs preserve SQL NULL for value-returning helpers; provider-gated rows fail closed before data disclosure.

**Returns:**

uint64 xxhash64 value.

**Behavior:**

- Volatility: volatile_value for random-byte/uuid helpers; stable_statement for deterministic crypto descriptors.
- Determinism: deterministic under fixture random/uuid overrides; random helpers use OpenSSL RAND otherwise.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for returned character descriptors; binary payloads are byte-stable.
- Timezone: not timezone-sensitive.
- Security and authority: executes inside the SBLR crypto.hash runtime using OpenSSL EVP/RAND/HMAC/scrypt where locally available, in-core xxhash64, deterministic ScratchBird armor/dearmor, and bounded ScratchBird PGP envelopes that do not claim OpenPGP compatibility; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed armor/envelope, unsupported algorithm, or out-of-budget request refuses with SBSQL.FUNCTION.INVALID_INPUT.

**Example:**

```sql
select xxhash64_value_seed(value_1, value_2) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.crypto.xxhash64_value_seed |
| UUID | 019dffbb-f000-7677-96ef-016bc0a5d9c5 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.crypto_xxhash64_value_seed.v3 |
| AST binding | ast.expr.crypto_xxhash64_value_seed |
| Engine entrypoint | xxhash64_value_seed |
| Optimizer foldable | False |
| Index eligible | False |
| Generated-column eligible | False |
| Cost class | runtime_seed |

Conformance evidence: `SBSFC057-xxhash64-seed`.


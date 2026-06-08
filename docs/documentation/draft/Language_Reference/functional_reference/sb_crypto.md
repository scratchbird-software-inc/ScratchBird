# Sb Crypto Functional Reference

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `sb_crypto`


## Package Boundary

`sb.crypto` contains 27 registered built-in operation records. Each record binds a public function, operator, aggregate, window function, or special form to descriptor-aware overloads, null behavior, optimizer properties, SBLR binding, and engine entrypoint metadata.

The package boundary is semantic, not a privilege boundary. A caller still needs object privileges, expression admission, descriptor compatibility, and policy admission for the statement that uses the operation.

## Package Inventory

| Kind | Records |
| --- | --- |
| scalar | 27 |

## Type Mechanics

- Argument descriptors select the overload.
- Return descriptors come from the declared return type rule.
- Null, collation, charset, timezone, and security behavior are part of binding.
- Optimizer properties may allow constant folding, generated column use, or index eligibility, but execution authority remains with the engine.
- Practical forms below are canonical usage shapes derived from declared overload signatures; statement admission still depends on the surrounding query, object privileges, and descriptor binding.

## Operation Records

### `argon2`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.argon2 |
| UUID | 019dffbb-f000-7efc-b382-c6063ea9c79e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | argon2(...) |
| Return Type Rule | runtime-defined by engine entrypoint argon2 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_argon2.v3 |
| AST Binding | ast.expr.crypto_argon2 |
| Engine Entrypoint | argon2 |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select argon2(arg_1) from app.sample_values;
```

### `armor`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.armor |
| UUID | 019dffbb-f000-74ec-bbd9-4ae6d355fd05 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | armor(...) |
| Return Type Rule | runtime-defined by engine entrypoint armor |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_armor.v3 |
| AST Binding | ast.expr.crypto_armor |
| Engine Entrypoint | armor |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select armor(arg_1) from app.sample_values;
```

### `armor_binary`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.armor_binary |
| UUID | 019dffbb-f000-77be-91e5-aeeac55d9b74 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | armor_binary(...) |
| Return Type Rule | runtime-defined by engine entrypoint armor_binary |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_armor_binary.v3 |
| AST Binding | ast.expr.crypto_armor_binary |
| Engine Entrypoint | armor_binary |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select armor_binary(arg_1) from app.sample_values;
```

### `bcrypt`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.bcrypt |
| UUID | 019dffbb-f000-7f76-b620-8f7df70d8e21 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | bcrypt(...) |
| Return Type Rule | runtime-defined by engine entrypoint bcrypt |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_bcrypt.v3 |
| AST Binding | ast.expr.crypto_bcrypt |
| Engine Entrypoint | bcrypt |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select bcrypt(arg_1) from app.sample_values;
```

### `blake2b`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.blake2b |
| UUID | 019dffbb-f000-748d-902f-4c382da1b5c4 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | blake2b(...) |
| Return Type Rule | runtime-defined by engine entrypoint blake2b |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_blake2b.v3 |
| AST Binding | ast.expr.crypto_blake2b |
| Engine Entrypoint | blake2b |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select blake2b(arg_1) from app.sample_values;
```

### `blake3`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.blake3 |
| UUID | 019dffbb-f000-7445-ae37-ff1d4dd2b543 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | blake3(...) |
| Return Type Rule | runtime-defined by engine entrypoint blake3 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_blake3.v3 |
| AST Binding | ast.expr.crypto_blake3 |
| Engine Entrypoint | blake3 |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select blake3(arg_1) from app.sample_values;
```

### `crypt`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.crypt |
| UUID | 019dffbb-f000-7245-8496-182b6c947da2 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | crypt(...) |
| Return Type Rule | runtime-defined by engine entrypoint crypt |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_crypt.v3 |
| AST Binding | ast.expr.crypto_crypt |
| Engine Entrypoint | crypt |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select crypt(arg_1) from app.sample_values;
```

### `crypt_password_salt`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.crypt_password_salt |
| UUID | 019dffbb-f000-7bc4-a953-87f55445bf3d |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | crypt_password_salt(...) |
| Return Type Rule | runtime-defined by engine entrypoint crypt_password_salt |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_crypt_password_salt.v3 |
| AST Binding | ast.expr.crypto_crypt_password_salt |
| Engine Entrypoint | crypt_password_salt |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select crypt_password_salt(arg_1) from app.sample_values;
```

### `dearmor`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.dearmor |
| UUID | 019dffbb-f000-79d8-898d-abd8629b348c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | dearmor(...) |
| Return Type Rule | runtime-defined by engine entrypoint dearmor |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_dearmor.v3 |
| AST Binding | ast.expr.crypto_dearmor |
| Engine Entrypoint | dearmor |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select dearmor(arg_1) from app.sample_values;
```

### `dearmor_text`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.dearmor_text |
| UUID | 019dffbb-f000-7d35-8581-6f080dd3fdca |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | dearmor_text(...) |
| Return Type Rule | runtime-defined by engine entrypoint dearmor_text |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_dearmor_text.v3 |
| AST Binding | ast.expr.crypto_dearmor_text |
| Engine Entrypoint | dearmor_text |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select dearmor_text(arg_1) from app.sample_values;
```

### `gen_random_bytes`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.gen_random_bytes |
| UUID | 019dffbb-f000-7c39-a9f4-ac2b7f953aa4 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | gen_random_bytes(...) |
| Return Type Rule | runtime-defined by engine entrypoint gen_random_bytes |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_gen_random_bytes.v3 |
| AST Binding | ast.expr.crypto_gen_random_bytes |
| Engine Entrypoint | gen_random_bytes |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select gen_random_bytes(arg_1) from app.sample_values;
```

### `gen_random_bytes_n`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.gen_random_bytes_n |
| UUID | 019dffbb-f000-7fd1-9f51-e0fbf6fb2b05 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | gen_random_bytes_n(...) |
| Return Type Rule | runtime-defined by engine entrypoint gen_random_bytes_n |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_gen_random_bytes_n.v3 |
| AST Binding | ast.expr.crypto_gen_random_bytes_n |
| Engine Entrypoint | gen_random_bytes_n |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select gen_random_bytes_n(arg_1) from app.sample_values;
```

### `gen_random_uuid`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.gen_random_uuid |
| UUID | 019dffbb-f000-7615-ba9c-4da476322745 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | gen_random_uuid(...) |
| Return Type Rule | runtime-defined by engine entrypoint gen_random_uuid |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_gen_random_uuid.v3 |
| AST Binding | ast.expr.crypto_gen_random_uuid |
| Engine Entrypoint | gen_random_uuid |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select gen_random_uuid(arg_1) from app.sample_values;
```

### `gen_salt`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.gen_salt |
| UUID | 019dffbb-f000-7d06-9aa1-b20c2b865445 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | gen_salt(...) |
| Return Type Rule | runtime-defined by engine entrypoint gen_salt |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_gen_salt.v3 |
| AST Binding | ast.expr.crypto_gen_salt |
| Engine Entrypoint | gen_salt |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select gen_salt(arg_1) from app.sample_values;
```

### `gen_salt_algo`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.gen_salt_algo |
| UUID | 019dffbb-f000-798e-9bf8-4e9d1e8a050e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | gen_salt_algo(...) |
| Return Type Rule | runtime-defined by engine entrypoint gen_salt_algo |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_gen_salt_algo.v3 |
| AST Binding | ast.expr.crypto_gen_salt_algo |
| Engine Entrypoint | gen_salt_algo |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select gen_salt_algo(arg_1) from app.sample_values;
```

### `hmac`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.hmac |
| UUID | 019dffbb-f000-763a-8f17-c4b8f7b287bc |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | hmac(...) |
| Return Type Rule | runtime-defined by engine entrypoint hmac |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_hmac.v3 |
| AST Binding | ast.expr.crypto_hmac |
| Engine Entrypoint | hmac |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select hmac(arg_1) from app.sample_values;
```

### `hmac_value_key_algo`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.hmac_value_key_algo |
| UUID | 019dffbb-f000-7879-8721-bdbd655a9290 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | hmac_value_key_algo(...) |
| Return Type Rule | runtime-defined by engine entrypoint hmac_value_key_algo |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_hmac_value_key_algo.v3 |
| AST Binding | ast.expr.crypto_hmac_value_key_algo |
| Engine Entrypoint | hmac_value_key_algo |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select hmac_value_key_algo(arg_1) from app.sample_values;
```

### `pgcrypto`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.pgcrypto |
| UUID | 019dffbb-f000-725d-94c8-999bca25ec17 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pgcrypto(...) |
| Return Type Rule | runtime-defined by engine entrypoint pgcrypto |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_pgcrypto.v3 |
| AST Binding | ast.expr.crypto_pgcrypto |
| Engine Entrypoint | pgcrypto |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select pgcrypto(arg_1) from app.sample_values;
```

### `pgp_pub_decrypt`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.pgp_pub_decrypt |
| UUID | 019dffbb-f000-77a1-b3d5-ea26727ce8d8 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pgp_pub_decrypt(...) |
| Return Type Rule | runtime-defined by engine entrypoint pgp_pub_decrypt |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_pgp_pub_decrypt.v3 |
| AST Binding | ast.expr.crypto_pgp_pub_decrypt |
| Engine Entrypoint | pgp_pub_decrypt |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select pgp_pub_decrypt(arg_1) from app.sample_values;
```

### `pgp_pub_encrypt`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.pgp_pub_encrypt |
| UUID | 019dffbb-f000-7dd4-968c-1dd3cc62b20c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pgp_pub_encrypt(...) |
| Return Type Rule | runtime-defined by engine entrypoint pgp_pub_encrypt |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_pgp_pub_encrypt.v3 |
| AST Binding | ast.expr.crypto_pgp_pub_encrypt |
| Engine Entrypoint | pgp_pub_encrypt |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select pgp_pub_encrypt(arg_1) from app.sample_values;
```

### `pgp_sym_decrypt`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.pgp_sym_decrypt |
| UUID | 019dffbb-f000-7307-b5fa-143dc6de25d0 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pgp_sym_decrypt(...) |
| Return Type Rule | runtime-defined by engine entrypoint pgp_sym_decrypt |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_pgp_sym_decrypt.v3 |
| AST Binding | ast.expr.crypto_pgp_sym_decrypt |
| Engine Entrypoint | pgp_sym_decrypt |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select pgp_sym_decrypt(arg_1) from app.sample_values;
```

### `pgp_sym_encrypt`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.pgp_sym_encrypt |
| UUID | 019dffbb-f000-765b-b72e-a39ab95aade5 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | pgp_sym_encrypt(...) |
| Return Type Rule | runtime-defined by engine entrypoint pgp_sym_encrypt |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_pgp_sym_encrypt.v3 |
| AST Binding | ast.expr.crypto_pgp_sym_encrypt |
| Engine Entrypoint | pgp_sym_encrypt |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select pgp_sym_encrypt(arg_1) from app.sample_values;
```

### `scrypt`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.scrypt |
| UUID | 019dffbb-f000-7ea8-b7b5-9b4deee60808 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | scrypt(...) |
| Return Type Rule | runtime-defined by engine entrypoint scrypt |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_scrypt.v3 |
| AST Binding | ast.expr.crypto_scrypt |
| Engine Entrypoint | scrypt |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select scrypt(arg_1) from app.sample_values;
```

### `sha3_256`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.sha3_256 |
| UUID | 019dffbb-f000-702e-b9a5-28aa6b997f82 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sha3_256(...) |
| Return Type Rule | runtime-defined by engine entrypoint sha3_256 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_sha3_256.v3 |
| AST Binding | ast.expr.crypto_sha3_256 |
| Engine Entrypoint | sha3_256 |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select sha3_256(arg_1) from app.sample_values;
```

### `sha3_512`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.sha3_512 |
| UUID | 019dffbb-f000-7f5e-8666-07a0756c9405 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | sha3_512(...) |
| Return Type Rule | runtime-defined by engine entrypoint sha3_512 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_sha3_512.v3 |
| AST Binding | ast.expr.crypto_sha3_512 |
| Engine Entrypoint | sha3_512 |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select sha3_512(arg_1) from app.sample_values;
```

### `xxhash64`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.xxhash64 |
| UUID | 019dffbb-f000-73ad-b373-adec35c35f74 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xxhash64(...) |
| Return Type Rule | runtime-defined by engine entrypoint xxhash64 |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_xxhash64.v3 |
| AST Binding | ast.expr.crypto_xxhash64 |
| Engine Entrypoint | xxhash64 |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select xxhash64(arg_1) from app.sample_values;
```

### `xxhash64_value_seed`

| Property | Value |
| --- | --- |
| Builtin ID | sb.crypto.xxhash64_value_seed |
| UUID | 019dffbb-f000-7677-96ef-016bc0a5d9c5 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | xxhash64_value_seed(...) |
| Return Type Rule | runtime-defined by engine entrypoint xxhash64_value_seed |
| Coercion Rule | descriptor implicit cast matrix or runtime guard |
| Null Behavior | engine runtime entrypoint semantics |
| Collation/Charset Rule | engine runtime entrypoint semantics where text descriptors apply |
| Timezone Rule | engine runtime entrypoint semantics where temporal descriptors apply |
| Volatility | runtime_defined_by_engine_entrypoint |
| Determinism | follows engine runtime entrypoint semantics for stable inputs |
| Side Effects | none unless the engine runtime entrypoint documents otherwise |
| SBLR Binding | sblr.expr.crypto_xxhash64_value_seed.v3 |
| AST Binding | ast.expr.crypto_xxhash64_value_seed |
| Engine Entrypoint | xxhash64_value_seed |
| Security Policy | follows engine runtime seed registry authority for crypto.hash |
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
select xxhash64_value_seed(arg_1) from app.sample_values;
```


# Sb Regex Functional Reference

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `sb_regex`


## Package Boundary

`sb.regex` contains 12 registered built-in operation records. Each record binds a public function, operator, aggregate, window function, or special form to descriptor-aware overloads, null behavior, optimizer properties, SBLR binding, and engine entrypoint metadata.

The package boundary is semantic, not a privilege boundary. A caller still needs object privileges, expression admission, descriptor compatibility, and policy admission for the statement that uses the operation.

## Package Inventory

| Kind | Records |
| --- | --- |
| scalar | 12 |

## Type Mechanics

- Argument descriptors select the overload.
- Return descriptors come from the declared return type rule.
- Null, collation, charset, timezone, and security behavior are part of binding.
- Optimizer properties may allow constant folding, generated column use, or index eligibility, but execution authority remains with the engine.
- Practical forms below are canonical usage shapes derived from declared overload signatures; statement admission still depends on the surrounding query, object privileges, and descriptor binding.

## Operation Records

### `occurrences_regex`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.occurrences_regex |
| UUID | 019dffbb-f000-7b44-8ced-ebb34f783c6e |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | occurrences_regex(...) |
| Return Type Rule | descriptor-authoritative occurrences_regex regex/text-pattern scalar result as implemented by the SBLR expression runtime; fixture descriptors int64 |
| Coercion Rule | descriptor implicit cast matrix for text, regex pattern, flags, occurrence, and group arguments |
| Null Behavior | strict unless SBSFC-017 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset and versioned regex engine semantics for character matching |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and regex engine version |
| Side Effects | none |
| SBLR Binding | sblr.expr.regex_count.v3 |
| AST Binding | ast.expr.occurrences_regex |
| Engine Entrypoint | occurrences_regex |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/regex-pattern errors use builtin error compatibility matrix and SBSFC-017 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select occurrences_regex(arg_1) from app.sample_values;
```

### `position_regex`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.position_regex |
| UUID | 019dffbb-f000-7b96-a548-8a2131a2a3ff |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | position_regex(...) |
| Return Type Rule | descriptor-authoritative position_regex regex/text-pattern scalar result as implemented by the SBLR expression runtime; fixture descriptors int64 |
| Coercion Rule | descriptor implicit cast matrix for text, regex pattern, flags, occurrence, and group arguments |
| Null Behavior | strict unless SBSFC-017 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset and versioned regex engine semantics for character matching |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and regex engine version |
| Side Effects | none |
| SBLR Binding | sblr.expr.regex_position.v3 |
| AST Binding | ast.expr.position_regex |
| Engine Entrypoint | position_regex |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/regex-pattern errors use builtin error compatibility matrix and SBSFC-017 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select position_regex(arg_1) from app.sample_values;
```

### `regexp_count`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.regexp_count |
| UUID | 019dffbb-f000-739b-b315-f6ecf36919ef |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | regexp_count(...) |
| Return Type Rule | descriptor-authoritative regexp_count regex/text-pattern scalar result as implemented by the SBLR expression runtime; fixture descriptors int64 |
| Coercion Rule | descriptor implicit cast matrix for text, regex pattern, flags, occurrence, and group arguments |
| Null Behavior | strict unless SBSFC-017 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset and versioned regex engine semantics for character matching |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and regex engine version |
| Side Effects | none |
| SBLR Binding | sblr.expr.regex_count.v3 |
| AST Binding | ast.expr.regex_count |
| Engine Entrypoint | regexp_count |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/regex-pattern errors use builtin error compatibility matrix and SBSFC-017 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select regexp_count(arg_1) from app.sample_values;
```

### `regexp_like`

| Property | Value |
| --- | --- |
| Builtin ID | sb.regex.match |
| UUID | 019de5fc-2400-76a3-9af7-b2ed28df4682 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | regexp_like(string,pattern[,flags]) |
| Return Type Rule | regex match using versioned regex engine |
| Coercion Rule | use descriptor implicit cast matrix; reject ambiguous donor-profile coercion unless profile gate allows it |
| Null Behavior | strict unless noted by function-specific semantics |
| Collation/Charset Rule | uses input descriptor collation/charset where string semantics apply |
| Timezone Rule | session timezone for temporal forms; not applicable otherwise |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors |
| Side Effects | none |
| SBLR Binding | sblr.expr.regex_match.v3 |
| AST Binding | ast.expr.regex_match |
| Engine Entrypoint | regex_match |
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
select regexp_like(text_value_1, arg_2) from app.sample_values;
```

### `regexp_match`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.regexp_match |
| UUID | 019dffbb-f000-78a1-b997-3b057ae06c2c |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | regexp_match(...) |
| Return Type Rule | descriptor-authoritative regexp_match regex/text-pattern scalar result as implemented by the SBLR expression runtime; fixture descriptors array |
| Coercion Rule | descriptor implicit cast matrix for text, regex pattern, flags, occurrence, and group arguments |
| Null Behavior | strict unless SBSFC-017 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset and versioned regex engine semantics for character matching |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and regex engine version |
| Side Effects | none |
| SBLR Binding | sblr.expr.regex_match_array.v3 |
| AST Binding | ast.expr.regex_match_array |
| Engine Entrypoint | regexp_match |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/regex-pattern errors use builtin error compatibility matrix and SBSFC-017 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select regexp_match(arg_1) from app.sample_values;
```

### `regexp_matches`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.regexp_matches |
| UUID | 019dffbb-f000-76b2-be48-22dbe15e24d2 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | regexp_matches(...) |
| Return Type Rule | descriptor-authoritative regexp_matches regex/text-pattern scalar result as implemented by the SBLR expression runtime; fixture descriptors array |
| Coercion Rule | descriptor implicit cast matrix for text, regex pattern, flags, occurrence, and group arguments |
| Null Behavior | strict unless SBSFC-017 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset and versioned regex engine semantics for character matching |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and regex engine version |
| Side Effects | none |
| SBLR Binding | sblr.expr.regex_match_array.v3 |
| AST Binding | ast.expr.regexp_matches |
| Engine Entrypoint | regexp_matches |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/regex-pattern errors use builtin error compatibility matrix and SBSFC-017 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select regexp_matches(arg_1) from app.sample_values;
```

### `regexp_replace`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.regexp_replace |
| UUID | 019dffbb-f000-7077-b07f-23d53db4cd24 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | regexp_replace(...) |
| Return Type Rule | descriptor-authoritative regexp_replace regex/text-pattern scalar result as implemented by the SBLR expression runtime; fixture descriptors character |
| Coercion Rule | descriptor implicit cast matrix for text, regex pattern, flags, occurrence, and group arguments |
| Null Behavior | strict unless SBSFC-017 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset and versioned regex engine semantics for character matching |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and regex engine version |
| Side Effects | none |
| SBLR Binding | sblr.expr.regex_replace.v3 |
| AST Binding | ast.expr.regex_replace |
| Engine Entrypoint | regexp_replace |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/regex-pattern errors use builtin error compatibility matrix and SBSFC-017 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select regexp_replace(arg_1) from app.sample_values;
```

### `regexp_split_to_array`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.regexp_split_to_array |
| UUID | 019dffbb-f000-7452-8ea1-2ded0fc33632 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | regexp_split_to_array(...) |
| Return Type Rule | descriptor-authoritative regexp_split_to_array regex/text-pattern scalar result as implemented by the SBLR expression runtime; fixture descriptors array |
| Coercion Rule | descriptor implicit cast matrix for text, regex pattern, flags, occurrence, and group arguments |
| Null Behavior | strict unless SBSFC-017 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset and versioned regex engine semantics for character matching |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and regex engine version |
| Side Effects | none |
| SBLR Binding | sblr.expr.regex_split_to_array.v3 |
| AST Binding | ast.expr.regex_split_to_array |
| Engine Entrypoint | regexp_split_to_array |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/regex-pattern errors use builtin error compatibility matrix and SBSFC-017 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select regexp_split_to_array(arg_1) from app.sample_values;
```

### `regexp_split_to_table`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.regexp_split_to_table |
| UUID | 019dffbb-f000-7cee-b7f9-1a495f927737 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | regexp_split_to_table(...) |
| Return Type Rule | descriptor-authoritative regexp_split_to_table regex/text-pattern scalar result as implemented by the SBLR expression runtime; fixture descriptors array |
| Coercion Rule | descriptor implicit cast matrix for text, regex pattern, flags, occurrence, and group arguments |
| Null Behavior | strict unless SBSFC-017 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset and versioned regex engine semantics for character matching |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and regex engine version |
| Side Effects | none |
| SBLR Binding | sblr.expr.regex_split_to_array.v3 |
| AST Binding | ast.expr.regexp_split_to_table |
| Engine Entrypoint | regexp_split_to_table |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/regex-pattern errors use builtin error compatibility matrix and SBSFC-017 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select regexp_split_to_table(arg_1) from app.sample_values;
```

### `regexp_substr`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.regexp_substr |
| UUID | 019dffbb-f000-7395-a58c-59cbad29d4f4 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | regexp_substr(...) |
| Return Type Rule | descriptor-authoritative regexp_substr regex/text-pattern scalar result as implemented by the SBLR expression runtime; fixture descriptors character |
| Coercion Rule | descriptor implicit cast matrix for text, regex pattern, flags, occurrence, and group arguments |
| Null Behavior | strict unless SBSFC-017 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset and versioned regex engine semantics for character matching |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and regex engine version |
| Side Effects | none |
| SBLR Binding | sblr.expr.regex_substr.v3 |
| AST Binding | ast.expr.regex_substr |
| Engine Entrypoint | regexp_substr |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/regex-pattern errors use builtin error compatibility matrix and SBSFC-017 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select regexp_substr(arg_1) from app.sample_values;
```

### `substring_regex`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.substring_regex |
| UUID | 019dffbb-f000-7485-8ae8-900e757d3f67 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | substring_regex(...) |
| Return Type Rule | descriptor-authoritative substring_regex regex/text-pattern scalar result as implemented by the SBLR expression runtime; fixture descriptors character |
| Coercion Rule | descriptor implicit cast matrix for text, regex pattern, flags, occurrence, and group arguments |
| Null Behavior | strict unless SBSFC-017 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset and versioned regex engine semantics for character matching |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and regex engine version |
| Side Effects | none |
| SBLR Binding | sblr.expr.regex_substr.v3 |
| AST Binding | ast.expr.substring_regex |
| Engine Entrypoint | substring_regex |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/regex-pattern errors use builtin error compatibility matrix and SBSFC-017 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select substring_regex(arg_1) from app.sample_values;
```

### `translate_regex`

| Property | Value |
| --- | --- |
| Builtin ID | sb.scalar.translate_regex |
| UUID | 019dffbb-f000-7bbb-9d9f-2aefb6bea146 |
| Kind | scalar |
| Syntax Forms | function_call |
| Overloads | translate_regex(...) |
| Return Type Rule | descriptor-authoritative translate_regex regex/text-pattern scalar result as implemented by the SBLR expression runtime; fixture descriptors character |
| Coercion Rule | descriptor implicit cast matrix for text, regex pattern, flags, occurrence, and group arguments |
| Null Behavior | strict unless SBSFC-017 row evidence defines an exact diagnostic/null result |
| Collation/Charset Rule | uses input descriptor collation/charset and versioned regex engine semantics for character matching |
| Timezone Rule | not applicable |
| Volatility | immutable_locale_versioned |
| Determinism | foldable only when volatility is immutable and all arguments are constants with stable descriptors and regex engine version |
| Side Effects | none |
| SBLR Binding | sblr.expr.regex_replace.v3 |
| AST Binding | ast.expr.translate_regex |
| Engine Entrypoint | translate_regex |
| Security Policy | none unless session/security/system metadata is read |
| Error Semantics | arity/type/regex-pattern errors use builtin error compatibility matrix and SBSFC-017 exact diagnostic row evidence |

#### Optimizer Properties

| Property | Value |
| --- | --- |
| foldable | True |
| index_eligible | True |
| generated_column_eligible | True |
| cost_class | cpu_scalar |

#### Practical Form

```sql
select translate_regex(arg_1) from app.sample_values;
```


# SB Regex Functional Reference

Generation task: `sb_regex`

Package namespace: `sb.regex`

Regular expression matching, search, counting, replacement, and split helpers.

## How To Read This Page

This package contains registered SBsql built-in operations.

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
| scalar | 12 |

## Operation Reference

### `occurrences_regex`

**Purpose:** Performs the `occurrences regex` regular-expression helper on text input.

**Call Forms:**

- `OCCURRENCES_REGEX(patternINstring[FLAGflags])`
- Syntax category: `function_call`

**Parameters:**

- `patternINstring[FLAGflags]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: pattern, string, and optional flags.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

int64 match count.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select occurrences_regex(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.occurrences_regex |
| UUID | 019dffbb-f000-7b44-8ced-ebb34f783c6e |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.regex_count.v3 |
| AST binding | ast.expr.occurrences_regex |
| Engine entrypoint | occurrences_regex |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC058-occurrences-regex-signature`.

### `position_regex`

**Purpose:** Performs the `position regex` regular-expression helper on text input.

**Call Forms:**

- `POSITION_REGEX([START\|AFTER]patternINstring[OCCURRENCEn][FLAGflags])`
- Syntax category: `function_call`

**Parameters:**

- `[START\|AFTER]patternINstring[OCCURRENCEn][FLAGflags]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: pattern, string, optional occurrence, flags, and mode.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

int64 one-based position or 0.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select position_regex(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.position_regex |
| UUID | 019dffbb-f000-7b96-a548-8a2131a2a3ff |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.regex_position.v3 |
| AST binding | ast.expr.position_regex |
| Engine entrypoint | position_regex |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC058-position-regex`.

### `regexp_count`

**Purpose:** Counts regular expression matches in text.

**Call Forms:**

- `regexp_count(string,pattern[,flags])`
- Syntax category: `function_call`

**Parameters:**

- `string`: Bound using the declared descriptor rules for this overload.
- `pattern[`: Bound using the declared descriptor rules for this overload.
- `flags]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative regex/text-pattern arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for text, regex pattern, flags, occurrence, and group arguments.
- NULL handling: strict unless the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative regexp_count regex/text-pattern scalar result as implemented by the SBLR expression runtime; fixture descriptors int64.

**Behavior:**

- Volatility: immutable_locale_versioned.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors and regex engine version.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset and versioned regex engine semantics for character matching.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/regex-pattern errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select regexp_count('banana', 'an') as match_count;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.regexp_count |
| UUID | 019dffbb-f000-739b-b315-f6ecf36919ef |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.regex_count.v3 |
| AST binding | ast.expr.regex_count |
| Engine entrypoint | regexp_count |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC017_REGEX_TEXT_PATTERN_FIXTURES`.

### `regexp_like`

**Purpose:** Returns whether text matches a regular expression pattern.

**Call Forms:**

- `regexp_like(string,pattern[,flags])`
- Syntax category: `function_call`

**Parameters:**

- `string`: Bound using the declared descriptor rules for this overload.
- `pattern[`: Bound using the declared descriptor rules for this overload.
- `flags]`: Bound using the declared descriptor rules for this overload.
- Coercion: use descriptor implicit cast matrix; reject ambiguous external dialect-profile coercion unless profile gate allows it.
- NULL handling: strict unless noted by function-specific semantics.

**Returns:**

regex match using versioned regex engine.

**Behavior:**

- Volatility: immutable_locale_versioned.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset where string semantics apply.
- Timezone: session timezone for temporal forms; not applicable otherwise.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/overflow/domain errors use builtin error compatibility matrix.

**Example:**

```sql
select regexp_like('ScratchBird', 'Bird$') as is_match;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.regex.match |
| UUID | 019de5fc-2400-76a3-9af7-b2ed28df4682 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.regex_match.v3 |
| AST binding | ast.expr.regex_match |
| Engine entrypoint | regex_match |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFCOP-regex-function-positive;SBSFCOP-regex-function-invalid-pattern`.

### `regexp_match`

**Purpose:** Returns match information for a regular expression pattern.

**Call Forms:**

- `regexp_match(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative regex/text-pattern arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for text, regex pattern, flags, occurrence, and group arguments.
- NULL handling: strict unless the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative regexp_match regex/text-pattern scalar result as implemented by the SBLR expression runtime; fixture descriptors array.

**Behavior:**

- Volatility: immutable_locale_versioned.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors and regex engine version.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset and versioned regex engine semantics for character matching.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/regex-pattern errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select regexp_match('ScratchBird', 'Bird$') as match_value;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.regexp_match |
| UUID | 019dffbb-f000-78a1-b997-3b057ae06c2c |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.regex_match_array.v3 |
| AST binding | ast.expr.regex_match_array |
| Engine entrypoint | regexp_match |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC017_REGEX_TEXT_PATTERN_FIXTURES`.

### `regexp_matches`

**Purpose:** Returns match rows for a regular expression pattern.

**Call Forms:**

- `regexp_matches(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: string, pattern, and optional flags.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

array descriptor containing captures.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select regexp_matches(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.regexp_matches |
| UUID | 019dffbb-f000-76b2-be48-22dbe15e24d2 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.regex_match_array.v3 |
| AST binding | ast.expr.regexp_matches |
| Engine entrypoint | regexp_matches |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC058-regexp-matches-bare`.

### `regexp_replace`

**Purpose:** Returns text with regular expression matches replaced.

**Call Forms:**

- `regexp_replace(string,pattern,replacement[,flags])`
- Syntax category: `function_call`

**Parameters:**

- `string`: Bound using the declared descriptor rules for this overload.
- `pattern`: Bound using the declared descriptor rules for this overload.
- `replacement[`: Bound using the declared descriptor rules for this overload.
- `flags]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative regex/text-pattern arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for text, regex pattern, flags, occurrence, and group arguments.
- NULL handling: strict unless the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative regexp_replace regex/text-pattern scalar result as implemented by the SBLR expression runtime; fixture descriptors character.

**Behavior:**

- Volatility: immutable_locale_versioned.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors and regex engine version.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset and versioned regex engine semantics for character matching.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/regex-pattern errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select regexp_replace('ScratchBird', 'Bird$', 'Core') as changed_text;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.regexp_replace |
| UUID | 019dffbb-f000-7077-b07f-23d53db4cd24 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.regex_replace.v3 |
| AST binding | ast.expr.regex_replace |
| Engine entrypoint | regexp_replace |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC017_REGEX_TEXT_PATTERN_FIXTURES`.

### `regexp_split_to_array`

**Purpose:** Splits text into an array using a regular expression delimiter.

**Call Forms:**

- `regexp_split_to_array(text,pattern)`
- Syntax category: `function_call`

**Parameters:**

- `text`: Bound using the declared descriptor rules for this overload.
- `pattern`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: descriptor_authoritative regex/text-pattern arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for text, regex pattern, flags, occurrence, and group arguments.
- NULL handling: strict unless the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative regexp_split_to_array regex/text-pattern scalar result as implemented by the SBLR expression runtime; fixture descriptors array.

**Behavior:**

- Volatility: immutable_locale_versioned.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors and regex engine version.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset and versioned regex engine semantics for character matching.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/regex-pattern errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select regexp_split_to_array('a,b,c', ',') as parts;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.regexp_split_to_array |
| UUID | 019dffbb-f000-7452-8ea1-2ded0fc33632 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.regex_split_to_array.v3 |
| AST binding | ast.expr.regex_split_to_array |
| Engine entrypoint | regexp_split_to_array |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC017_REGEX_TEXT_PATTERN_FIXTURES`.

### `regexp_split_to_table`

**Purpose:** Splits text into rows using a regular expression delimiter.

**Call Forms:**

- `regexp_split_to_table(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: string and pattern.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

array descriptor containing split text values.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select regexp_split_to_table(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.regexp_split_to_table |
| UUID | 019dffbb-f000-7cee-b7f9-1a495f927737 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.regex_split_to_array.v3 |
| AST binding | ast.expr.regexp_split_to_table |
| Engine entrypoint | regexp_split_to_table |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC058-regexp-split-to-table-bare`.

### `regexp_substr`

**Purpose:** Returns the substring matched by a regular expression.

**Call Forms:**

- `regexp_substr(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: descriptor_authoritative regex/text-pattern arguments bound by the parser/lowering route.
- Coercion: descriptor implicit cast matrix for text, regex pattern, flags, occurrence, and group arguments.
- NULL handling: strict unless the conformance fixtures row evidence defines an exact diagnostic/null result.

**Returns:**

descriptor-authoritative regexp_substr regex/text-pattern scalar result as implemented by the SBLR expression runtime; fixture descriptors character.

**Behavior:**

- Volatility: immutable_locale_versioned.
- Determinism: foldable only when volatility is immutable and all arguments are constants with stable descriptors and regex engine version.
- Side effects: none.
- Collation/charset: uses input descriptor collation/charset and versioned regex engine semantics for character matching.
- Timezone: not applicable.
- Security and authority: none unless session/security/system metadata is read.

**Errors:**

arity/type/regex-pattern errors use builtin error compatibility matrix and the conformance fixtures exact diagnostic row evidence.

**Example:**

```sql
select regexp_substr('order-123', '[0-9]+') as matched_text;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.regexp_substr |
| UUID | 019dffbb-f000-7395-a58c-59cbad29d4f4 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.regex_substr.v3 |
| AST binding | ast.expr.regex_substr |
| Engine entrypoint | regexp_substr |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC017_REGEX_TEXT_PATTERN_FIXTURES`.

### `substring_regex`

**Purpose:** Performs the `substring regex` regular-expression helper on text input.

**Call Forms:**

- `substring_regex(...)`
- Syntax category: `function_call`

**Parameters:**

- `...`: Arguments are selected by the overload and descriptor binding rules.
- Descriptor rule: pattern, string, optional occurrence, group, and flags.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

character text or SQL NULL.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select substring_regex(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.substring_regex |
| UUID | 019dffbb-f000-7485-8ae8-900e757d3f67 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.regex_substr.v3 |
| AST binding | ast.expr.substring_regex |
| Engine entrypoint | substring_regex |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC058-substring-regex-bare`.

### `translate_regex`

**Purpose:** Performs the `translate regex` regular-expression helper on text input.

**Call Forms:**

- `TRANSLATE_REGEX(patternINstringWITHreplacement[OCCURRENCEn\|ALL][FLAGflags])`
- Syntax category: `function_call`

**Parameters:**

- `patternINstringWITHreplacement[OCCURRENCEn\|ALL][FLAGflags]`: Bound using the declared descriptor rules for this overload.
- Descriptor rule: pattern, string, replacement, optional occurrence/ALL, and flags.
- Coercion: bounded descriptor-to-SBLR scalar coercion for the fixture arguments only.
- NULL handling: SQL NULL propagates through the SBLR scalar runtime for nullable value arguments.

**Returns:**

character text.

**Behavior:**

- Volatility: stable_statement.
- Determinism: deterministic for the the conformance fixtures fixture inputs except sequence advancement state.
- Side effects: none; no mutation or transaction finality change.
- Collation/charset: UTF-8/unicode_root for character descriptors; regex rows use the bounded C++ regex route.
- Timezone: numeric offset or UTC/GMT only for timezone rows; other rows are not timezone-sensitive.
- Security and authority: executes inside the SBLR expression runtime using in-core scalar helpers and the existing SBLR sequence runtime; no parser SQL execution, external execution, storage finality, recovery shortcut, cluster provider authority, or external service is used.

**Errors:**

invalid arity, malformed regex/temporal/numeric text, unsupported timezone, or sequence current-state errors use canonical SBLR diagnostics.

**Example:**

```sql
select translate_regex(value_1) as result_value from app.sample_values;
```

**Technical Details:**

| Field | Value |
| --- | --- |
| Builtin ID | sb.scalar.translate_regex |
| UUID | 019dffbb-f000-7bbb-9d9f-2aefb6bea146 |
| Kind | scalar |
| Syntax forms | function_call |
| SBLR binding | sblr.expr.regex_replace.v3 |
| AST binding | ast.expr.translate_regex |
| Engine entrypoint | translate_regex |
| Optimizer foldable | True |
| Index eligible | True |
| Generated-column eligible | True |
| Cost class | cpu_scalar |

Conformance evidence: `SBSFC058-translate-regex-signature`.

# Text, Collation, And Charset

This page is part of the SBsql Language Reference Manual. It defines SBsql text
descriptors, character set rules, collation behavior, length semantics,
comparison behavior, index behavior, casts, and diagnostics.

Generation task: `data_types_text_collation`

## Purpose

Storing and comparing text correctly requires knowing more than the number of
characters. Every text value in SBsql carries two descriptor properties: a
_character set_ (the encoding used to store bytes) and a _collation_ (the rules
for ordering, comparing, and making values unique). The descriptor, not the
spelling of the SQL type, controls storage encoding, character length,
comparison, ordering, grouping, uniqueness, pattern matching, hash keys, index
keys, generated columns, masks, and result rendering.

If you compare two text values from columns with different collations without an
explicit collation clause, the engine reports a collation mismatch rather than
silently choosing one. This keeps results deterministic and avoids surprising
behavior when collations differ between databases or sessions.

Binary values are not text. A byte string can become text only through an
explicit conversion that states an encoding or through an admitted assignment
policy.

## Supported Text Types

| Canonical Type | Common Aliases | Length Unit | Payload | Value Bounds |
| --- | --- | --- | --- | --- |
| `char(n)` | `character(n)` | characters | Fixed-length encoded text padded according to descriptor policy. | Exactly `n` characters after padding rules. |
| `varchar(n)` | `character varying(n)` | characters | Variable-length encoded text. | 0 through `n` characters. |
| `text` | none | characters | Variable-length encoded text with overflow where admitted. | Policy bounded by row, page, overflow, and stream limits. |
| `clob` | `character large object` | characters or stream chunks | Character large-value stream. | Policy bounded by large-value and stream limits. |
| `nchar(n)` | national fixed character | characters | Fixed-length national-character text. | Exactly `n` characters under the national charset descriptor. |
| `nvarchar(n)` | national varying character | characters | Variable-length national-character text. | 0 through `n` characters under the national charset descriptor. |
| `nclob` | national character large object | characters or stream chunks | National-character large-value stream. | Policy bounded by large-value and stream limits. |

Declared length is a character count. Storage uses encoded bytes plus row,
descriptor, and overflow metadata. A value can satisfy the declared character
count and still be refused if the encoded byte count, collation key, row size,
index key size, stream limit, or policy limit cannot admit it.

## Character Sets

Every text descriptor has a character set.

| Charset Source | Rule |
| --- | --- |
| Database default charset | Used when a declaration omits `character set`. |
| Column charset | Stored in the column descriptor and used by assignment, comparison, functions, indexes, and rendering. |
| Literal introducer | Binds a literal to a charset before coercion. Unsupported or lossy conversion is refused unless policy admits it. |
| National character set | Used by `nchar`, `nvarchar`, and `nclob`. |
| Result expression | Derived from operands, casts, functions, and collation/charset rules. Ambiguity is refused. |

Example:

```sql
create table app.customer (
    customer_id uuid primary key,
    display_name varchar(120) character set utf8 collate default,
    legal_name nvarchar(240)
);
```

The spelling of `utf8` and the national-character descriptor bind to catalog
descriptors. The engine does not rely on the text spelling after binding.

## Collations

Collation is part of the descriptor and affects equality, ordering, grouping,
uniqueness, joins, indexes, and some string functions.

| Collation Concern | Behavior |
| --- | --- |
| Default collation | Applied when neither the column nor expression states a collation. |
| Explicit `collate` | Overrides expression collation for that expression and can define an index key collation. |
| Deterministic comparison | Required for equality, uniqueness, grouping, and B-tree ordering unless a provider proof admits otherwise. |
| Case sensitivity | Descriptor-owned. Do not infer it from display spelling. |
| Accent sensitivity | Descriptor-owned. |
| Normalization | Descriptor-owned. Comparisons must not silently normalize outside the collation contract. |
| Null ordering | Owned by query/index ordering rules, not by text collation alone. |

Example:

```sql
select customer_id, display_name
from app.customer
where display_name collate default = :name
order by display_name collate default;
```

## Length Functions

Text has multiple length concepts.

| Function Class | Meaning |
| --- | --- |
| Character length | Number of characters under the descriptor character set. |
| Octet length | Number of encoded bytes. |
| Collation key length | Internal comparison key length where the collation uses one. |
| Large-value length | Logical character length and/or stream byte length according to descriptor policy. |

Portable scripts should use the function that matches the intended limit rather
than assuming one length measure implies another.

## Assignment And Padding

Assignment to a text target follows this order:

1. bind source descriptor;
2. bind target text or domain descriptor;
3. convert charset if admitted;
4. apply target length rule;
5. apply padding or trimming only where the descriptor says it is allowed;
6. validate domain constraints and policy;
7. store or return the descriptor-bound value.

`char(n)` padding is descriptor-owned. `varchar(n)` and `text` do not imply
padding. Silent truncation is not admitted by default.

## Text Comparison And Indexes

Text indexes use descriptor-aware keys. A text index can accelerate candidate
selection, but final result admission still requires engine recheck.

| Feature | Rule |
| --- | --- |
| Equality | Uses the active text descriptor's collation and normalization rules. |
| Ordering | Uses the active collation and query/index null-order rules. |
| Grouping | Uses the same equality semantics as the descriptor. |
| Unique indexes | Use deterministic collation keys; non-deterministic collations must be refused unless explicitly admitted. |
| Prefix/range indexes | Must preserve the descriptor's ordering and exact recheck requirements. |
| Pattern matching | Uses the descriptor, function/operator policy, and optional collation profile. |
| Full-text/search | Uses search descriptors and tokenization profiles, not ordinary text equality alone. |

Example:

```sql
create index app.customer_name_ix
on app.customer (display_name collate default);
```

## Text Literals

| Literal Form | Binding Rule |
| --- | --- |
| `'text'` | Binds as text under the active literal charset until context chooses a target descriptor. |
| `N'text'` | Binds as national-character text. |
| Character set introducer | Binds the literal to the stated charset before target coercion. |
| Escaped literal | Escape rules are parser-profile controlled and must lower to canonical text bytes. |
| Concatenation | Result descriptor derives charset, collation, and length from operands and operation policy. |

When the target is `uuid`, numeric, temporal, binary, document, or protected
material, a string literal remains text until an explicit cast or assignment
conversion admits the target.

## Text And Protected Values

Text can carry sensitive data. Protected material policy can block rendering,
logging, support-bundle output, casts, concatenation, export, backup,
replication, bridge output, or diagnostic text.

Masking a text column does not change the stored value. It changes the rendered
result under policy.

## Diagnostics

| Condition | Required Result |
| --- | --- |
| Invalid encoded bytes | Conversion diagnostic. |
| Unsupported charset | Bind diagnostic. |
| Unsupported collation | Bind diagnostic. |
| Lossy charset conversion | Diagnostic unless explicitly admitted. |
| Character length exceeded | Assignment diagnostic. |
| Encoded byte limit exceeded | Storage or stream diagnostic. |
| Index key too large | DDL or DML diagnostic according to when the value is known. |
| Non-deterministic collation used for unique key | DDL refusal unless admitted by policy. |
| Binary passed to text function without conversion | Bind diagnostic. |
| Protected value rendered without release authority | Denied message vector. |

## Syntax Productions

```ebnf
text_type               ::= fixed_text_type
                          | varying_text_type
                          | large_text_type
                          | national_text_type ;
```

```ebnf
fixed_text_type         ::= ("char" | "character") "(" length ")" text_type_options? ;
varying_text_type       ::= ("varchar" | "character" "varying") "(" length ")" text_type_options? ;
large_text_type         ::= "text" text_type_options?
                          | "clob" text_type_options? ;
national_text_type      ::= "nchar" "(" length ")"
                          | "nvarchar" "(" length ")"
                          | "nclob" ;
```

```ebnf
text_type_options       ::= character_set_clause? collation_clause? ;
character_set_clause    ::= "character" "set" charset_ref ;
collation_clause        ::= "collate" collation_ref ;
```

## Related Pages

- [Type System Overview](type_system_overview.md)
- [Conversion Matrix](conversion_matrix.md)
- [Domains, Casts, And Coercion](domains_casts_and_coercion.md)
- [Operator Type Result Matrix](../syntax_reference/operator_type_result_matrix.md)
- [Policy, Mask, And RLS Lifecycle](../syntax_reference/policy_mask_and_rls.md)

## Verification Checklist

The text proof suite should demonstrate:

- declared character length is enforced separately from encoded byte length;
- charset conversion refuses unsupported or lossy conversions by default;
- collation affects equality, ordering, grouping, and index behavior
  consistently;
- quoted and unquoted literals bind to the expected descriptors;
- `char(n)` padding follows descriptor policy;
- `varchar(n)` rejects over-length values without silent truncation;
- text indexes use the same comparison rule as expression evaluation;
- non-deterministic collations are refused for features requiring deterministic
  keys unless an admitted proof exists;
- binary values require explicit conversion before text operations;
- protected text is redacted or denied according to policy.

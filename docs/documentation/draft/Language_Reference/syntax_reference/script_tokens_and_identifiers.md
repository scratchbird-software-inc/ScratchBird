# Scripts, Tokens, And Identifiers

This page is part of the SBsql Language Reference Manual. It documents script structure, statement termination, whitespace and comments, contextual command words, identifiers, qualified names, UUID references, literals, parameter markers, punctuation, and parser-to-binder boundaries.

Generation task: `syntax_reference_script_and_tokens`

Related pages: [Schema Tree And Name Resolution](schema_tree_and_name_resolution.md), [Operators And Precedence](operators.md), [Operator Type And Result Matrix](operator_type_result_matrix.md), [Type System Overview](../data_types/type_system_overview.md), [Text, Collation, And Character Sets](../data_types/text_collation_and_charset.md), [Numeric Types](../data_types/numeric_types.md), [Temporal Types](../data_types/temporal_types.md), [Conversion Matrix](../data_types/conversion_matrix.md), [Refusal Vectors](refusal_vectors.md), and [Parser To SBLR Pipeline](../core_paradigms/parser_to_sblr_pipeline.md).

## Purpose

An SBsql script is a sequence of statements. Tokenization turns script text into contextual tokens, parsing builds statement and expression trees, binding resolves names and descriptors, and SBLR admission maps the bound request to an engine operation. None of those early stages make text authoritative. Durable object identity, type behavior, security, policy, transaction finality, and recovery decisions are engine-owned.

SBsql is intentionally context sensitive. Most command words are recognized only where the grammar expects that command. Outside that context, they may be ordinary identifiers when the surrounding grammar expects a name.

Example:

```sql
create schema app;
create table app."select" (
  id uuid primary key,
  "from" text
);

insert into app."select" (id, "from")
values (uuid '019d0000-0000-7000-8000-000000000001', 'contextual names');
```

`select` and `from` are command words in query contexts. Quoted here, they are exact identifiers.

## Script Structure

```ebnf
script ::=
    statement_list EOF ;

statement_list ::=
    statement (statement_terminator statement)* statement_terminator? ;

statement_terminator ::=
    ";" ;
```

A script may contain one statement or many statements:

```sql
create schema app;
create table app.orders (
  order_id uuid primary key,
  order_total decimal(18,2)
);
select order_id, order_total from app.orders;
```

The semicolon is the portable SBsql statement terminator. A client protocol may also submit a single prepared statement without a trailing semicolon when the protocol frame already supplies the boundary.

## Lexical Phases

SBsql processing is layered:

| Phase | Input | Output | Authority Boundary |
| --- | --- | --- | --- |
| Character decoding | Script bytes and declared encoding. | Unicode scalar stream. | Invalid encoding is a diagnostic before parsing. |
| Lexing | Character stream. | Tokens with text span, token class, quote flags, and source location. | Tokens are evidence only. |
| Parsing | Token stream. | Concrete syntax tree and abstract statement tree. | Grammar recognition does not grant authority. |
| Binding | Statement tree plus session context. | Bound descriptors, object UUIDs, parameter descriptors, and result shape. | Resolver and security checks start here. |
| SBLR lowering | Bound statement. | SBLR envelope and operation identity. | Operation must pass verifier and admission. |
| Execution | Admitted SBLR request. | Rows, diagnostics, catalog mutation, stream route, or management result. | Engine owns durable effects and MGA finality. |

Clients should not infer object identity, privilege, or type behavior from token text alone.

## Whitespace

Whitespace separates tokens where two adjacent tokens would otherwise merge. It is otherwise insignificant outside string literals, quoted identifiers, delimited text, and stream payloads.

The portable whitespace set is:

| Character | Meaning |
| --- | --- |
| Space | Ordinary token separator. |
| Horizontal tab | Ordinary token separator. |
| Line feed | Ordinary token separator and source-location increment. |
| Carriage return plus line feed | One line break for source-location purposes. |

Tools that normalize scripts should preserve source locations when they intend to report diagnostics against the original text.

## Comments

SBsql accepts line comments and block comments in script text. Comments are not tokens in the statement grammar, but source-preserving tools may keep them in a concrete syntax tree for formatting, diagnostics, or documentation extraction.

```ebnf
line_comment  ::= "--" any_character_until_line_end ;
block_comment ::= "/*" block_comment_body "*/" ;
```

Example:

```sql
-- Create the application schema.
create schema app;

/*
  The table name binds through the schema resolver.
  The comment text is ignored by statement execution.
*/
create table app.orders (
  order_id uuid primary key
);
```

Comments must not appear inside numeric literals, string literals, quoted identifiers, or binary payloads unless that construct explicitly treats the characters as data.

## Contextual Command Words

SBsql has a small hard-reserved lexical core and a large contextual vocabulary. Contextual words are recognized as command tokens only in grammar positions that require them.

| Category | Examples | Rule |
| --- | --- | --- |
| Statement introducers | `CREATE`, `ALTER`, `DROP`, `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `MERGE`, `UPSERT`, `SHOW`, `DESCRIBE` | Command tokens when a statement is expected. |
| Clause introducers | `FROM`, `WHERE`, `GROUP`, `HAVING`, `ORDER`, `LIMIT`, `OFFSET`, `FETCH`, `RETURNING` | Clause tokens inside the statement families that admit them. |
| Lifecycle modifiers | `IF`, `EXISTS`, `RESTRICT`, `CASCADE`, `RECREATE`, `COMMENT`, `RENAME` | Contextual inside DDL and inspection surfaces. |
| Security words | `GRANT`, `REVOKE`, `ROLE`, `USER`, `GROUP`, `POLICY`, `MASK`, `RLS` | Contextual in security and policy statements. |
| Transaction words | `BEGIN`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`, `RELEASE`, `SNAPSHOT` | Contextual in transaction statements. |
| Literal words | `NULL`, `TRUE`, `FALSE`, `UNKNOWN`, `DEFAULT` | Literal or special value only in expression or assignment contexts. |

Because command words are contextual, this is valid when the grammar expects identifiers:

```sql
create table app.statement_words (
  "select" integer,
  from_value text,
  order_value integer
);
```

Portable scripts should still avoid unquoted identifiers that match common command words when the name will be read by people or emitted through multiple tools.

## Identifier Forms

```ebnf
identifier ::=
      regular_identifier
    | delimited_identifier
    | localized_name_literal ;

regular_identifier ::=
    identifier_start identifier_continue* ;

delimited_identifier ::=
    '"' delimited_identifier_character* '"' ;

localized_name_literal ::=
    localized_name_prefix string_literal ;
```

Regular identifiers are convenient display names. Delimited identifiers are exact labels. Localized-name literals attach language-aware labels where admitted by a statement.

### Regular Identifiers

A regular identifier starts with an admitted identifier-start character and continues with admitted identifier-continue characters.

Portable regular identifiers should use:

```text
[A-Za-z_][A-Za-z0-9_]*
```

SBsql implementations may admit broader Unicode identifier classes. A script that needs maximum portability across tools should use the portable subset or delimited identifiers.

Regular identifiers are profile-folded before lookup. The active identifier profile determines whether unquoted text folds toward upper case, lower case, or case-insensitive lookup. The resolver stores and compares durable identity by UUID, not by display spelling.

### Delimited Identifiers

Delimited identifiers use double quotes and preserve exact spelling according to the active quoted-identifier profile:

```sql
create table app."Case Sensitive Name" (
  "Column With Spaces" text
);

select "Column With Spaces"
from app."Case Sensitive Name";
```

Inside a delimited identifier, a doubled quote represents one quote character:

```sql
create table app."quote""inside" (
  id integer
);
```

Delimited identifiers should be used when a label contains spaces, punctuation, mixed case that must be exact, or contextual command words.

### Localized Names

Localized labels let a catalog object carry display names for a language or language-independent context. They are metadata and resolver labels where admitted; they are not a security bypass.

Portable examples:

```sql
comment on schema app is 'Application root';
```

When a statement admits localized labels, binding must carry language tag, fallback behavior, exact-text flag, and catalog identity evidence. The default language is `en`; `und` may be used for language-independent metadata where admitted.

Localized labels are different from an SBsql language profile. A label is
catalog metadata for a durable object. A language profile is a parser resource
that can change source spelling, phrase order, diagnostic text, completion
hints, or rendering templates before UUID binding. Neither form bypasses
authorization or makes display text durable identity.

## Qualified Names

```ebnf
qualified_name ::=
    name_part ("." name_part)* ;

name_part ::=
      regular_identifier
    | delimited_identifier
    | localized_name_literal ;
```

Qualified names are paths through a resolver context. They may name schemas, tables, views, functions, procedures, domains, policies, filespaces, bridges, packages, or other catalog objects depending on the surrounding statement.

Examples:

```sql
app.orders
app.reporting.monthly_totals
sys.fn.current_timestamp
```

A qualified name does not use the search path for its parent components. The resolver walks the explicit path, applies identifier folding and quoted-name rules to each part, checks object class, checks transaction visibility, and applies materialized authorization.

Unqualified names bind through the current schema, search path, default root, and sandbox rules described in [Schema Tree And Name Resolution](schema_tree_and_name_resolution.md).

## UUID References

```ebnf
uuid_ref ::=
    UUID string_literal ;
```

UUID references are direct identity evidence:

```sql
describe schema uuid '019d0000-0000-7000-8000-000000000001';
```

The UUID must parse, match the expected object class, be visible in the active transaction snapshot, and pass authorization. UUID syntax bypasses name search, but it does not bypass security, sandboxing, recovery fences, or object-class validation.

## Literals

```ebnf
literal ::=
      string_literal
    | numeric_literal
    | boolean_literal
    | null_literal
    | typed_literal
    | uuid_literal ;
```

Literals create typed or initially untyped values. The final descriptor is selected by the expression context, explicit cast, target column, function signature, operator rule, or parameter descriptor.

| Literal Form | Example | Initial Meaning |
| --- | --- | --- |
| String | `'hello'` | Text value or untyped string pending context. |
| Escaped string | `'it''s done'` | One quote is represented by two quote characters. |
| Integer | `42` | Exact integer candidate. |
| Unsigned 128-bit integer | `123U128`, `123UINT128` | Exact `uint128` literal. |
| Decimal | `42.50` | Exact numeric candidate with scale evidence. |
| Approximate numeric | `1.25e3` | Approximate or exact numeric candidate according to descriptor context. |
| Boolean | `true`, `false` | Boolean value. |
| Unknown | `unknown` | Three-valued logic marker where admitted. |
| Null | `null` | Null marker typed by context. |
| UUID | `uuid '019d0000-0000-7000-8000-000000000001'` | UUID value. |
| Date | `date '2026-06-08'` | Date value. |
| Time | `time '13:45:00'` | Time value. |
| Timestamp | `timestamp '2026-06-08 13:45:00'` | Timestamp value. |
| Binary | `binary '010203'` | Binary value under an admitted binary literal profile. |

Invalid literal text, overflow, unsupported precision, lossy conversion, invalid encoding, or ambiguous target typing produces a diagnostic before execution.

## String Literals

```ebnf
string_literal ::=
    "'" string_character* "'" ;
```

A single quote inside the string is represented by two single quotes:

```sql
select 'Alice''s order';
```

String literals are decoded under the active character set and conversion policy. When a statement needs a specific character set, collation, or binary interpretation, use an explicit cast, typed literal, column descriptor, or function signature rather than relying on display text.

## Numeric Literals

```ebnf
numeric_literal ::=
      integer_literal
    | decimal_literal
    | approximate_numeric_literal ;
```

Numeric literal text carries evidence about exactness, scale, exponent, sign, and width. The target descriptor decides the final type:

```sql
insert into app.invoice (invoice_id, subtotal)
values (1, 42.50);
```

`42.50` binds through the `subtotal` descriptor. If the descriptor cannot represent the value without violating precision, scale, rounding, or overflow rules, binding or execution returns a diagnostic according to the expression context.

Integer suffixes can request an unsigned descriptor before contextual coercion.
`U128` and `UINT128` request `uint128`; overflow or negative input is refused
before execution.

Operator result rules are documented in [Operator Type And Result Matrix](operator_type_result_matrix.md).

## Parameters

Prepared statements use parameter markers. A parameter marker is not a literal; it is a placeholder with a descriptor supplied by the prepare/bind protocol, surrounding expression, or explicit cast.

```ebnf
parameter_marker ::=
      "?"
    | ":" identifier
    | "$" integer_literal ;
```

Examples:

```sql
select order_id, order_total
from app.orders
where customer_id = :customer_id
  and order_total >= :minimum_total;
```

```sql
insert into app.orders (order_id, customer_id, order_total)
values (?, ?, ?);
```

Rules:

- the same named parameter in one statement refers to one parameter descriptor;
- positional markers are ordered by appearance;
- parameters must bind to expected descriptors before execution;
- a null parameter still needs a target type;
- raw parameter bytes are not SQL text and must not be reparsed as SQL;
- parameter descriptors must be included in SBLR lowering and proof evidence.

## Punctuation And Separators

| Token | Use |
| --- | --- |
| `;` | Statement terminator. |
| `,` | List separator. |
| `.` | Qualified-name separator and member/path separator where admitted by a specific expression. |
| `(` `)` | Grouping, function calls, row constructors, subqueries, and column lists. |
| `[` `]` | Collection, array, vector, path, or profile-specific indexing where admitted. |
| `{` `}` | Document, object, or profile-specific payload syntax where admitted. |
| `:` | Named parameters, labels, slice notation, or profile-specific constructs where admitted. |
| `::` | Cast shorthand only where the active SBsql profile admits it. Portable scripts should use `CAST`. |
| `=>` | Named argument binding where admitted. |
| `*` | Projection wildcard or multiplication according to context. |

The same character can have different meanings in different contexts. The parser records context; the binder resolves the meaning to descriptors and operation IDs.

## Operators

Operators are tokens or contextual phrases inside expressions. Precedence, associativity, operand descriptors, result descriptors, null behavior, and refusal cases are documented in [Operators And Precedence](operators.md) and [Operator Type And Result Matrix](operator_type_result_matrix.md).

Example:

```sql
select order_id,
       (subtotal + tax_total) / item_count as average_item_total
from app.orders
where item_count > 0;
```

Do not infer numeric result type from the operator token alone. The descriptor rules decide whether an operation is exact, approximate, widened, rejected for overflow, or rejected for unsupported conversion.

## Labels, Aliases, And Scope-Local Names

Some names exist only inside one statement or block:

| Name Kind | Scope |
| --- | --- |
| Projection alias | Current query block result descriptor. |
| Table alias | Current `FROM` item scope and child expressions. |
| CTE name | Statement-local `WITH` scope. |
| Window name | Current query block window clause. |
| Savepoint name | Current transaction. |
| Routine parameter | Current function, procedure, or trigger body. |
| Block label | Current procedural block where admitted. |

Scope-local names are not catalog objects unless the surrounding statement explicitly creates a durable object. They can shadow catalog names only according to the scope rules for that statement.

Example:

```sql
with recent_orders as (
  select order_id, customer_id
  from app.orders
  where created_at >= :start_at
)
select recent_orders.order_id
from recent_orders;
```

`recent_orders` is a CTE name, not a schema object.

## Case, Folding, And Exact Matching

Regular identifiers are normalized by the active identifier profile before lookup. Quoted identifiers preserve exact text. UUID references bypass name folding.

| Input | Binding Rule |
| --- | --- |
| `orders` | Fold according to active identifier profile, then resolve. |
| `ORDERS` | Same regular-identifier profile as other unquoted text. |
| `"orders"` | Exact quoted label. |
| `"Orders"` | Different exact label from `"orders"` when exact matching is active. |
| `uuid '...'` | Parse UUID and validate object class, visibility, and authorization. |

Changing an identifier profile changes how future bindings resolve unquoted names. It does not change existing object UUIDs.

## Error And Refusal Boundaries

| Condition | Result |
| --- | --- |
| Invalid byte sequence for declared script encoding | Decode diagnostic before lexing. |
| Unterminated string, quoted identifier, or block comment | Parse diagnostic. |
| Unknown token sequence | Parse diagnostic. |
| Contextual word used where no grammar admits it | Parse diagnostic. |
| Ambiguous visible name | Bind diagnostic. |
| Hidden or out-of-sandbox name | Not-visible or denied result according to policy. |
| UUID has wrong object class | Bind diagnostic or denied result according to disclosure policy. |
| Unsupported literal precision or token form | Unsupported refusal or bind diagnostic according to route. |
| Parameter descriptor missing | Bind diagnostic before execution. |
| SQL text accepted but operation not available | Refusal vector. |

## Formatter And Tooling Guidance

Tools that rewrite SBsql should preserve:

- statement boundaries;
- exact quoted-identifier text;
- localized labels and language tags;
- comments when operating as a source formatter;
- parameter names and positions;
- source spans for diagnostics;
- normalized command-word casing only when doing so does not change quoted text;
- UUID references exactly as values, not as object names.

Tools should not:

- fold quoted identifiers;
- replace parameters with literal text;
- treat comments as executable directives;
- assume command words are globally reserved;
- infer object identity from names after a rename;
- expose hidden object names in diagnostics.

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Script splitting | Semicolon-delimited statements parse independently while preserving source spans. |
| Comments | Line and block comments are ignored by execution and preserved where source tooling requires it. |
| Contextual words | Command words are accepted as identifiers where the grammar expects identifiers. |
| Quoted identifiers | Exact text and embedded doubled quotes round-trip. |
| Regular identifiers | Folding follows the active identifier profile. |
| Qualified names | Explicit paths bind without search-path fallback for parent components. |
| UUID references | UUID syntax validates object class, visibility, and authorization. |
| Literals | Text, numeric, temporal, boolean, null, UUID, and binary literals bind through descriptors. |
| Parameters | Named and positional markers produce stable parameter descriptors. |
| Operators | Operator tokens route through precedence and descriptor result rules. |
| Refusals | Recognized but unavailable surfaces return message vectors rather than parse errors. |
| Proof | Full rebuild tests regenerate parser, SBLR, descriptor, resolver, and diagnostic evidence. |

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| `script` | grammar production | script | yes | `sblr.statement.batch.v3` |
| `statement_list` | grammar production | script | yes | `sblr.statement.batch.v3` |
| `statement_terminator` | grammar production | script | yes | structural boundary |
| `qualified_name` | grammar production | resolver | yes | bound object reference |
| `name_part` | grammar production | resolver | yes | bound name atom |
| `regular_identifier` | lexical token | resolver | yes | folded name evidence |
| `delimited_identifier` | lexical token | resolver | yes | exact name evidence |
| `localized_name_literal` | lexical token | resolver | yes | localized name evidence |
| `uuid_ref` | grammar production | resolver | yes | UUID object evidence |
| `literal` | grammar production | expression | yes | descriptor-bound value |
| `parameter_marker` | grammar production | expression | yes | parameter descriptor |
| `option_list` | grammar production | statement options | yes | option descriptor list |

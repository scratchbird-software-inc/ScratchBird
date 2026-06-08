# Type System Overview

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `data_types_type_system_overview`


## Purpose

SBsql values are descriptor-bound. A textual type name, literal, cast, or parameter is resolved into a descriptor that controls storage representation, comparison, null behavior, collation, timezone, hashing, indexing, and result rendering.

The type system separates canonical carriers from domains. A carrier says how a value can be stored and operated on. A domain layers null policy, constraints, defaults, masking, metadata rendering, and element addressing on top of that carrier.

The catalog tables `sys.catalog.type_descriptor`, `sys.catalog.domain_descriptor`, and `sys.catalog.type_capability` are the core metadata surfaces for this model.

## Supported Type Families

The public type contract is organized by descriptor family. A descriptor family is the engine-visible carrier class; SBsql spellings, aliases, and domains bind to one of these families plus a canonical descriptor payload. Exact on-page encoding can change only through descriptor and file-format versioning. SQL-visible comparison, cast, null, collation, timezone, and indexing behavior must remain stable for a descriptor version.

| Descriptor Family | Canonical SBsql Names | Primary Use | Fixed Payload Size | Variable or Policy Bounds |
| --- | --- | --- | --- | --- |
| `null_value` | `null` | Unknown or absent value marker | none | Carries no value; target descriptor is inferred from context or must be stated with `cast`. |
| `boolean` | `boolean`, `bool` | Three-valued logic | 1 byte payload | Values are `true`, `false`, and SQL `null`. |
| `integer` | `int16`, `smallint`, `int32`, `int`, `integer`, `int64`, `bigint`, `int128` | Exact signed integers | 2, 4, 8, or 16 bytes | Range is fixed by width. |
| `unsigned_integer` | `uint8`, `uint16`, `uint32`, `uint64`, `uint128` | Exact unsigned integers | 1, 2, 4, 8, or 16 bytes | Unsigned use must be explicit. |
| `decimal` | `decimal(p,s)`, `numeric(p,s)`, `decfloat(16)`, `decfloat(34)`, `money`, `currency` | Exact base-10 and declared money-like values | descriptor-dependent | Precision, scale, rounding, overflow, and display are descriptor-owned. |
| `real` | `real`, `float4`, `double precision`, `float8`, `float(p)` | Approximate numeric values | 4 or 8 bytes | IEEE-style finite, infinity, and NaN handling is descriptor-policy controlled. |
| `text` | `char(n)`, `character(n)`, `varchar(n)`, `character varying(n)`, `text`, `clob`, `nchar(n)`, `nvarchar(n)`, `nclob` | Character data | descriptor header plus encoded bytes | Character count, byte count, charset, collation, and overflow policy are descriptor-owned. |
| `binary` | `binary(n)`, `varbinary(n)`, `bytea`, `blob`, `image` | Byte data and large binary objects | descriptor header plus bytes | Length is byte-counted; large values may use overflow storage when admitted. |
| `temporal` | `date`, `time`, `time with time zone`, `timestamp`, `timestamp with time zone`, `interval` | Calendar, clock, instant, and duration values | 4, 8, 12, or 16 bytes depending on descriptor | Timezone, precision, calendar, and SBsql rendering are descriptor-owned. |
| `uuid` | `uuid`, `uuid '<text>'` | Object identity and application UUIDs | 16 bytes | Text rendering is canonical lower-case UUID unless an explicit SBsql rendering policy says otherwise. |
| `json_document` | `json`, `jsonb`, `document` | JSON/document values and path-indexable payloads | descriptor header plus document bytes | Text-preserving or normalized binary representation is selected by descriptor. |
| `vector` | `vector<T,n>`, `embedding<T,n>` | Fixed-dimension numerical vectors | `n * element_size` plus descriptor metadata | Element profile, dimension, metric, and exact recheck requirements are descriptor-owned. |
| `graph` | `graph`, `node`, `edge`, `path` | Graph records and traversal payloads | descriptor-dependent | Node identity, edge identity, path ordering, and traversal result shape are descriptor-owned. |
| `domain` | user-defined domain names | Named constrained type overlay | underlying carrier size | Domain constraints, defaults, masking, type aliasing, and null policy are catalog-owned. |

## Canonical Names And Aliases

The canonical descriptor name is what the binder and SBLR envelope carry. SBsql type spellings and aliases lower to the same descriptor model without making the original text authoritative.

| SBsql family | Examples of accepted spellings | Binding rule |
| --- | --- | --- |
| Exact numeric | `int64`, `uint128`, `decimal(18,2)`, `numeric(18,2)`, `decfloat(34)` | Binds to signed integer, unsigned integer, decimal, or decimal-floating descriptors. |
| Approximate numeric | `real`, `float4`, `double precision`, `float8`, `float(p)` | Binds to approximate real descriptors with explicit precision policy. |
| Text | `char(20)`, `character(20)`, `varchar(200)`, `character varying(200)`, `text`, `clob` | Binds to text descriptors with charset, collation, and overflow policy. |
| Binary | `binary(16)`, `varbinary(1024)`, `blob` | Binds to byte descriptors. |
| Temporal | `date`, `time`, `timestamp`, `timestamp with time zone`, `interval` | Binds to temporal descriptors with precision and timezone policy. |
| Multimodel | `json`, `jsonb`, `document`, `vector<float32,1536>`, `graph`, `node`, `edge`, `path` | Binds to document, vector, graph, or related descriptors. |

## Range And Limit Rules

The fixed-width type ranges are part of the language contract. Variable-width limits are not a single global number because they are governed by row-page size, overflow/TOAST policy, character set, collation keys, stream limits, and database policy.

When a manual table says "policy bounded", the binder must still have a concrete descriptor bound before execution. The engine may refuse a value before execution, during row construction, or during overflow allocation if the descriptor, row, transaction, or storage policy cannot admit it.

## Syntax Productions

```ebnf
literal                 ::= string_literal | numeric_literal | boolean_literal | null_literal | uuid_ref ;
```

```ebnf
expression              ::= expression_atom (binary_operator expression_atom)* ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and SBsql execution options.
- SBLR admission maps the bound request to an operation family and result shape.
- The engine rechecks authority before durable state changes or result delivery.

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| currency | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| FULL | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| POSITION(substringINtext) | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| current_server | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| st_x | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| lock_timeout_default | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| bit_count | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| operation_evidence_required | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| USING | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| chr(integer) | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| element(multiset<T>) | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| st_makepoint | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| REAL | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| sb.special_form.coalesce | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| dearmor | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| regexp_like(string,pattern[,flags]) | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| IMAGE | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| CURSOR | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| jsonb_object_keys | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| client_min_messages | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| json_array_elements | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| sb.scalar.nullif | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| cos | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |
| boolean_cast_from_text | function | expression_runtime | yes | rs.sbsql.scalar_value.v1 |

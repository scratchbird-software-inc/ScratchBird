# Type Descriptor Lifecycle

This page is part of the SBsql Language Reference Manual. It documents type descriptor creation, alteration, capability control, comparison and cast contracts, storage profile binding, inspection, comments, recreation, and drop behavior.

Generation task: `syntax_reference_type_descriptor_lifecycle`

Related pages: [Type System Overview](../data_types/type_system_overview.md), [Domains, Casts, And Coercion](../data_types/domains_casts_and_coercion.md), [Conversion Matrix](../data_types/conversion_matrix.md), [Text, Collation, And Character Sets](../data_types/text_collation_and_charset.md), [Numeric Types](../data_types/numeric_types.md), [Temporal Types](../data_types/temporal_types.md), [Document, Graph, Vector, And Multimodel Types](../data_types/document_graph_vector_and_multimodel_types.md), [Domain Lifecycle](domain.md), [Table Lifecycle](table.md), [Index Lifecycle](index.md), [Function Lifecycle](function.md), [Refusal Vectors](refusal_vectors.md), [sys.catalog.type_descriptor](../catalog_reference/sys_catalog_type_descriptor.md), [sys.catalog.type_capability](../catalog_reference/sys_catalog_type_capability.md), and [sys.catalog.operation_descriptor](../catalog_reference/sys_catalog_operation_descriptor.md).

## Purpose

A type descriptor is the catalog object that defines carrier behavior for values. It controls representation, size policy, precision, scale, charset, collation, timezone, comparison, hashing, ordering, casts, storage codec, index compatibility, wire rendering, protected-value behavior, backup and replication eligibility, acceleration eligibility, and operation binding.

SBsql type names are not the authority after binding. A textual spelling such as `decimal(18,2)`, `varchar(80)`, or `vector<float32,768>` is resolved to a descriptor UUID and descriptor payload. Tables, domains, parameters, expressions, casts, indexes, routines, streams, and SBLR envelopes carry descriptor evidence so the engine can recheck the operation.

Type descriptors are high-impact catalog objects. Changing one can affect stored data, expression results, index keys, sort order, grouping, hash joins, backup rendering, stream descriptors, driver metadata, and support diagnostics. For that reason, most descriptor changes are either immutable, versioned, or require explicit dependency validation.

## Descriptor Versus Domain

Descriptors and domains are related but separate.

| Object | Owns | Typical Use |
| --- | --- | --- |
| Type descriptor | Carrier representation and base operation behavior. | `bigint`, `decimal(18,2)`, `varchar(200)`, `timestamptz`, `json`, `vector<float32,768>`. |
| Domain | A named policy layer over a descriptor or another domain. | `app.email_text`, `app.positive_amount`, `app.customer_code`. |
| Domain stack | Ordered domain layers plus the base descriptor. | Assignment validation, defaults, masks, constraints, and operation preservation rules. |

A domain may wrap a type descriptor. A type descriptor must not rely on a domain for its core storage, comparison, or cast authority.

## Lifecycle Surface

| Operation | Surface | Contract |
| --- | --- | --- |
| Create | `CREATE TYPE DESCRIPTOR` | Creates a descriptor UUID, carrier family, modifier profile, capabilities, operation contracts, storage profile, and dependency metadata. |
| Alter | `ALTER TYPE DESCRIPTOR` | Changes only admitted descriptor metadata. Unsafe representation changes require a new descriptor version or explicit dependency migration. |
| Rename | `RENAME TYPE DESCRIPTOR ... TO ...` | Changes resolver label only; descriptor UUID and dependent object bindings remain stable. |
| Recreate | `RECREATE TYPE DESCRIPTOR` | Replaces a descriptor only when no unsafe dependent storage or operation binding exists. |
| Comment | `COMMENT ON TYPE DESCRIPTOR ... IS ...` | Stores or clears authorized descriptive metadata. |
| Show | `SHOW TYPE DESCRIPTOR`, `SHOW TYPE DESCRIPTORS` | Lists authorized descriptor metadata, capabilities, and dependency summaries. |
| Describe | `DESCRIBE TYPE DESCRIPTOR` | Returns one descriptor's carrier shape, modifiers, capabilities, casts, comparison contracts, indexes, storage, and dependencies. |
| Drop | `DROP TYPE DESCRIPTOR` | Retires a descriptor after domain, column, routine, index, stream, and stored-data dependencies are handled. |

## Syntax

```ebnf
type_descriptor_lifecycle_statement ::=
      create_type_descriptor_statement
    | alter_type_descriptor_statement
    | rename_type_descriptor_statement
    | recreate_type_descriptor_statement
    | comment_on_type_descriptor_statement
    | show_type_descriptor_statement
    | describe_type_descriptor_statement
    | drop_type_descriptor_statement ;
```

```ebnf
create_type_descriptor_statement ::=
    CREATE TYPE DESCRIPTOR if_not_exists? type_descriptor_ref
    AS descriptor_carrier
    type_descriptor_clause* ;

descriptor_carrier ::=
      scalar_descriptor
    | temporal_descriptor
    | text_descriptor
    | binary_descriptor
    | document_descriptor
    | vector_descriptor
    | graph_descriptor
    | spatial_descriptor
    | opaque_descriptor ;
```

```ebnf
type_descriptor_clause ::=
      MODIFIER PROFILE modifier_profile_ref
    | CHARACTER SET charset_ref
    | COLLATION collation_ref
    | TIMEZONE POLICY timezone_policy_ref
    | STORAGE CODEC storage_codec_ref
    | COMPARISON CONTRACT comparison_contract_ref
    | CAPABILITY type_capability_list
    | CAST POLICY cast_policy_ref
    | INDEX COMPATIBILITY index_compatibility_ref
    | WIRE PROFILE wire_profile_ref
    | BACKUP PROFILE backup_profile_ref
    | REPLICATION PROFILE replication_profile_ref
    | ACCELERATION acceleration_profile_ref
    | PROTECTED VALUE protected_value_policy_ref
    | VERSION integer_literal
    | COMMENT string_literal ;
```

```ebnf
alter_type_descriptor_statement ::=
    ALTER TYPE DESCRIPTOR type_descriptor_ref alter_type_descriptor_action+ ;

alter_type_descriptor_action ::=
      SET MODIFIER PROFILE modifier_profile_ref
    | SET CHARACTER SET charset_ref
    | SET COLLATION collation_ref
    | SET TIMEZONE POLICY timezone_policy_ref
    | SET STORAGE CODEC storage_codec_ref
    | SET COMPARISON CONTRACT comparison_contract_ref
    | SET CAPABILITY type_capability_list
    | SET CAST POLICY cast_policy_ref
    | SET INDEX COMPATIBILITY index_compatibility_ref
    | SET WIRE PROFILE wire_profile_ref
    | SET BACKUP PROFILE backup_profile_ref
    | SET REPLICATION PROFILE replication_profile_ref
    | SET ACCELERATION acceleration_profile_ref
    | SET PROTECTED VALUE protected_value_policy_ref
    | SET OWNER principal_ref
    | SET VERSION integer_literal
    | RESET CHARACTER SET
    | RESET COLLATION
    | RESET TIMEZONE POLICY
    | RESET STORAGE CODEC
    | RESET COMPARISON CONTRACT
    | RESET CAPABILITY
    | RESET CAST POLICY
    | RESET INDEX COMPATIBILITY
    | RESET WIRE PROFILE
    | RESET BACKUP PROFILE
    | RESET REPLICATION PROFILE
    | RESET ACCELERATION
    | RESET PROTECTED VALUE ;
```

```ebnf
rename_type_descriptor_statement ::=
    RENAME TYPE DESCRIPTOR type_descriptor_ref TO identifier ;

recreate_type_descriptor_statement ::=
    RECREATE TYPE DESCRIPTOR type_descriptor_ref
    AS descriptor_carrier
    type_descriptor_clause* ;

comment_on_type_descriptor_statement ::=
    COMMENT ON TYPE DESCRIPTOR type_descriptor_ref IS (string_literal | NULL) ;
```

```ebnf
show_type_descriptor_statement ::=
      SHOW TYPE DESCRIPTORS show_type_descriptor_filter?
    | SHOW TYPE DESCRIPTOR type_descriptor_ref show_type_descriptor_option_list? ;

show_type_descriptor_filter ::=
      LIKE string_literal
    | WHERE predicate ;

show_type_descriptor_option_list ::=
    WITH show_type_descriptor_option ("," show_type_descriptor_option)* ;

show_type_descriptor_option ::=
      CARRIER
    | MODIFIERS
    | CAPABILITIES
    | CASTS
    | OPERATIONS
    | INDEXES
    | STORAGE
    | TRANSPORT
    | DEPENDENCIES
    | UUIDS ;
```

```ebnf
describe_type_descriptor_statement ::=
    DESCRIBE TYPE DESCRIPTOR type_descriptor_ref describe_type_descriptor_option_list? ;

describe_type_descriptor_option_list ::=
    WITH describe_type_descriptor_option ("," describe_type_descriptor_option)* ;

describe_type_descriptor_option ::=
      CARRIER
    | MODIFIERS
    | CAPABILITIES
    | CASTS
    | OPERATIONS
    | INDEXES
    | STORAGE
    | TRANSPORT
    | DEPENDENCIES
    | UUIDS ;
```

```ebnf
drop_type_descriptor_statement ::=
    DROP TYPE DESCRIPTOR if_exists? type_descriptor_ref drop_type_descriptor_behavior? ;

drop_type_descriptor_behavior ::=
      RESTRICT
    | CASCADE ;
```

SBsql is context sensitive. Type descriptor words are command words inside type descriptor statements and should not be treated as globally reserved identifiers outside those contexts.

## Descriptor Identity

A descriptor has several pieces of identity:

| Identity | Meaning |
| --- | --- |
| Resolver name | User-visible catalog name, such as `sys.fn.int64` or `app.money_amount`. |
| Descriptor UUID | Durable descriptor identity carried by catalog rows, expressions, domains, columns, routines, and SBLR envelopes. |
| Descriptor version | Compatibility boundary for representation and operation semantics. |
| Carrier family | Engine-visible type family, such as integer, decimal, text, temporal, UUID, document, vector, graph, spatial, binary, or protected value. |
| Modifier profile | Precision, scale, length, dimension, SRID, timezone, charset, collation, or other parameter payload. |

Renaming a descriptor changes only the resolver name. Stored data, domains, columns, operation bindings, and index metadata remain bound to the descriptor UUID.

## Carrier Families

`CREATE TYPE DESCRIPTOR` starts from a carrier family. The carrier family defines the minimum representation and operation model.

| Carrier | Examples | Descriptor Concerns |
| --- | --- | --- |
| Boolean | `boolean` | Three-valued logic, display rendering, null interaction. |
| Signed integer | `int16`, `int32`, `int64`, `int128` | Width, signed range, overflow, widening. |
| Unsigned integer | `uint8`, `uint16`, `uint32`, `uint64` | Width, unsigned range, explicit casts. |
| Decimal | `decimal(18,2)`, `numeric(38,10)`, `decimal_float` | Precision, scale, rounding, overflow, special values where admitted. |
| Real | `float32`, `float64` | Approximation policy, NaN/infinity handling, comparison policy. |
| Text | `char`, `varchar`, `text`, `clob` | Character count, byte count, charset, collation, padding, overflow. |
| Binary | `binary`, `varbinary`, `blob` | Byte count, large-value storage, rendering, byte ordering. |
| Temporal | `date`, `time`, `timestamp`, `interval` | Precision, timezone, calendar, rendering, arithmetic. |
| UUID | `uuid` | 16-byte identity value, canonical text rendering, ordering. |
| Document | `json`, `jsonb`, `document` | Text-preserving or normalized form, path model, missing/null policy. |
| Vector | `vector<float32,768>` | Element descriptor, dimension, metric, exact recheck. |
| Graph | `node`, `edge`, `path`, `graph` | Identity model, traversal payload, path ordering. |
| Spatial | `geometry`, `geography` | SRID, coordinate model, exact predicate recheck. |
| Opaque or protected | protected values, handles, locator types | Release policy, rendering policy, operation allow-list. |

The supported built-in spellings and ranges are summarized in [Type System Overview](../data_types/type_system_overview.md). This page covers the lifecycle surface that creates or mutates descriptor catalog objects.

## Modifier Profiles

Descriptor modifiers are structured metadata, not free-form text.

| Modifier | Applies To | Examples |
| --- | --- | --- |
| Precision | Decimal, temporal, real profile | `decimal(18,2)`, `timestamp(6)`. |
| Scale | Decimal | `numeric(38,10)`. |
| Length | Text and binary | `varchar(200)`, `varbinary(1024)`. |
| Character set | Text | `utf8`, national character set profiles. |
| Collation | Text | Case-sensitive, case-insensitive, locale-aware, binary-like ordering. |
| Timezone policy | Temporal | Session-rendered instant, fixed-offset time, local date/time. |
| Dimension | Vector | `vector<float32,1536>`. |
| Element descriptor | Vector, arrays, structured values | `float32`, `int8`, nested descriptor UUID. |
| SRID/profile | Spatial | Coordinate reference and validation policy. |
| Storage codec | Large text, binary, document, protected material | Inline, overflow, compressed, encrypted, or protected. |

Modifier changes are safe only when every dependent value, index, cast, and operation remains compatible. Otherwise create a new descriptor version and migrate dependents explicitly.

## Capabilities

Capabilities describe where the descriptor can be used.

| Capability | Meaning |
| --- | --- |
| `comparable` | Equality comparison is defined. |
| `orderable` | Total or policy-defined ordering is defined. |
| `hashable` | Hash keys can be produced for joins, grouping, or hash indexes. |
| `groupable` | `GROUP BY`, `DISTINCT`, and related operations can use the descriptor. |
| `indexable` | At least one index family can use the descriptor. |
| `storable` | Values can be stored in ordinary table rows. |
| `wire_renderable` | Values can be rendered through parser, driver, or stream protocols. |
| `backup_safe` | Values can participate in admitted backup rendering. |
| `replication_safe` | Values can participate in logical replication routes. |
| `transport_safe` | Values can move through admitted inter-process or provider boundaries. |
| `udr_safe` | Values can cross trusted UDR boundaries. |
| `acceleration_eligible` | Values can be used by admitted native or compiled execution paths. |
| `protected_value_capable` | Values have protected-material rules. |
| `element_addressable` | Values support path, element, or field addressing. |

Capabilities are not grants. A type can be indexable while a user still lacks `CREATE INDEX`; a type can be wire-renderable while a protected-value policy redacts the value.

## Comparison, Hashing, And Ordering

Comparison behavior is descriptor-owned.

| Contract | Used By |
| --- | --- |
| Equality | `=`, `<>`, uniqueness checks, joins, grouping. |
| Ordering | `<`, `>`, `ORDER BY`, ordered indexes, range scans. |
| Hashing | Hash joins, hash aggregates, hash indexes. |
| Canonicalization | Normalized comparison keys, text folding, document normalization. |
| Null and missing behavior | Three-valued logic, document paths, optional fields, partial indexes. |
| Special values | NaN, infinity, temporal ambiguity, invalid encodings, protected values. |

Indexes, query plans, materialized views, constraints, and optimizer rewrites must use the same comparison contract that expression binding uses. An index cannot become final row authority; executor recheck still applies where the operation requires it.

## Cast Policy

Cast policy defines how values move between descriptors.

| Cast Class | Meaning |
| --- | --- |
| Assignment cast | Used for table columns, routine parameters, variables, and return values. |
| Explicit cast | Used by `CAST`, `TRY_CAST`, and named conversion functions. |
| Widening cast | Preserves all values from the source descriptor. |
| Narrowing cast | May lose range, precision, scale, timezone, charset, or representation; usually requires explicit syntax. |
| Rendering cast | Produces text or binary display form. It is not a storage conversion unless explicitly admitted. |
| Protected release | Converts protected values only through release authority. |

Implicit coercion must be conservative. A descriptor change that modifies cast policy invalidates dependent prepared statements, generated expressions, constraints, functions, indexes, and metadata rendering that rely on old coercion behavior.

## Storage And Transport Profiles

A descriptor may bind to storage and transport metadata.

| Profile | Controls |
| --- | --- |
| Storage codec | Inline, overflow, compression, encryption, page layout, and value encoding. |
| Row-page eligibility | Whether a value can be stored inline or must use overflow. |
| Stream profile | Client/server frame rendering, binary/text encoding, null markers, and size limits. |
| Backup profile | Logical backup rendering and restore validation. |
| Replication profile | Logical delta rendering, identity, ordering evidence, and idempotency. |
| Driver metadata | Type code, precision, scale, display size, nullability, and rendering hints. |
| Support profile | Redacted diagnostics, summaries, and safe bundle evidence. |

Changing storage or transport profiles is a compatibility event. Existing data must either remain readable through the old version or be migrated through an explicit operation.

## Index Compatibility

Index compatibility answers two questions:

1. Which index families may store keys for this descriptor?
2. What exact recheck is required before returning rows?

Examples:

| Descriptor | Possible Index Compatibility |
| --- | --- |
| Integer | B-tree, hash, zone map, columnar statistics. |
| Text | B-tree under collation, full-text, ngram, trigram, hash. |
| UUID | B-tree and hash. |
| Document | Path, inverted, expression, generated-column indexes. |
| Vector | Vector index families with metric and exact-recheck policy. |
| Spatial | Spatial index families with exact predicate recheck. |

The descriptor's comparison and canonicalization contracts must match index behavior. If they drift, the index must be rebuilt, refused, or marked unusable until reconciled.

## Create Type Descriptor

Example for a money-like descriptor:

```sql
create type descriptor app.money_amount
as decimal(18, 2)
  comparison contract sys.fn.numeric_order
  storage codec sys.fn.decimal_packed
  capability comparable, orderable, hashable, groupable, indexable, storable, wire_renderable
  comment 'Exact application money amount';
```

Example for a text descriptor with explicit charset and collation:

```sql
create type descriptor app.customer_label_text
as varchar(120)
  character set utf8
  collation sys.fn.unicode_ci
  capability comparable, orderable, hashable, groupable, indexable, storable, wire_renderable;
```

Example for a vector descriptor:

```sql
create type descriptor app.search_embedding
as vector<float32, 768>
  capability comparable, indexable, storable, wire_renderable
  index compatibility sys.fn.vector_cosine_exact_recheck;
```

The binder must prove:

- the effective principal has authority to create a descriptor in the target schema;
- the carrier family and modifiers form a coherent descriptor;
- referenced charset, collation, timezone, storage, comparison, cast, index, and policy objects exist and are visible;
- capability flags are compatible with the carrier and referenced contracts;
- storage and transport profiles are available in the current build/profile;
- protected-value policies do not expose raw protected material;
- the catalog mutation route is admitted by SBLR and engine verification;
- recovery state admits catalog writes.

## Alter Type Descriptor

Safe descriptor alterations are generally metadata or compatibility-preserving changes:

```sql
alter type descriptor app.customer_label_text
  set wire profile sys.fn.text_wire_utf8
  set backup profile sys.fn.text_logical_backup;
```

High-risk changes require dependency validation:

```sql
alter type descriptor app.customer_label_text
  set collation sys.fn.unicode_cs;
```

Changing collation can change equality, ordering, uniqueness, grouping, and index keys. The operation must either:

- prove no dependent stored data, indexes, constraints, plans, materialized views, or routines are affected;
- mark dependents invalid and require rebuild/revalidation;
- create a new descriptor version and require explicit migration;
- or refuse.

Representation-changing alterations are refused unless an explicit migration route is admitted.

## Rename And Versioning

```sql
rename type descriptor app.money_amount to money_amount_v1;
```

Rename changes the resolver label only. Descriptor UUID and version remain stable.

Versioning is the correct path for behavior changes that must coexist with old data:

```sql
create type descriptor app.money_amount_v2
as decimal(19, 4)
  comparison contract sys.fn.numeric_order
  storage codec sys.fn.decimal_packed
  version 2;
```

Applications can then migrate domains, columns, routine signatures, and indexes explicitly. Versioning must preserve the ability to read old descriptor versions until the database no longer contains dependent values.

## Recreate Type Descriptor

`RECREATE TYPE DESCRIPTOR` is a controlled replacement surface:

```sql
recreate type descriptor app.stage_payload
as document
  capability comparable, storable, wire_renderable, element_addressable;
```

If the descriptor does not exist, it is created. If it exists, replacement is admitted only when no unsafe dependency remains or the dependency plan explicitly handles every affected object. It must not silently reinterpret stored data.

## Comment, Show, And Describe

```sql
comment on type descriptor app.money_amount is 'Exact monetary amount descriptor';
comment on type descriptor app.money_amount is null;
```

Inspection:

```sql
show type descriptors;
show type descriptor app.money_amount with carrier, capabilities, casts, indexes;
describe type descriptor app.money_amount with operations, storage, dependencies, uuids;
```

Inspection is metadata access, not mutation. Policies may redact protected-value contracts, internal implementation references, support evidence, or dependency details.

## Drop Type Descriptor

```sql
drop type descriptor app.stage_payload restrict;
```

`RESTRICT` refuses the drop while any dependency remains, including:

- domains;
- table columns;
- generated columns;
- constraints;
- indexes;
- views and materialized views;
- functions, procedures, triggers, and packages;
- casts and operation overloads;
- stream descriptors;
- backup, replication, or migration plans;
- prepared statement and driver metadata;
- stored values requiring the descriptor for decoding.

`CASCADE` requires an explicit authorized dependency plan. It must not remove data-bearing descriptors by surprise.

## Dependency And Invalidation Rules

Any descriptor lifecycle mutation may invalidate:

- expression bindings;
- prepared statements;
- table and domain descriptors;
- generated-column plans;
- check constraints;
- index definitions and index data;
- optimizer statistics;
- materialized views;
- routines and triggers;
- driver metadata;
- stream and backup profiles;
- support-bundle projections.

After commit, stale descriptor evidence must rebind or fail closed. Rollback must restore the prior descriptor state and dependent visibility.

## Security

Descriptor privileges are separate from ordinary table privileges.

| Privilege | Meaning |
| --- | --- |
| `CREATE TYPE DESCRIPTOR` | Create a descriptor in an authorized schema. |
| `ALTER` | Change admitted descriptor metadata. |
| `DROP` | Drop or retire a descriptor after dependency checks. |
| `COMMENT` | Set or clear comments. |
| `DESCRIBE` | Inspect descriptor metadata. |
| `USAGE` | Use the descriptor in columns, domains, routine signatures, casts, or parameters where policy admits it. |

Using a descriptor in a table column requires descriptor `USAGE` plus the relevant table or schema privileges. Inspecting a descriptor does not grant authority to decode protected values or view hidden implementation details.

## Refusal Conditions

| Condition | Result |
| --- | --- |
| Carrier family is unknown or unavailable | `unsupported`. |
| Modifier profile is incoherent | Bind diagnostic. |
| Precision, scale, length, dimension, or SRID is out of range | Bind diagnostic. |
| Charset and collation are incompatible | Bind diagnostic or `denied` according to disclosure policy. |
| Capability conflicts with carrier behavior | `unsupported` or bind diagnostic. |
| Operation contract missing for requested capability | `unsupported`. |
| Caller lacks descriptor privilege | `denied`. |
| Descriptor is outside session sandbox root | `denied`. |
| Protected-value policy would expose raw protected material | `denied`. |
| Alteration would invalidate stored data without migration | `denied`. |
| Drop has remaining dependencies under `RESTRICT` | `denied`. |
| Recovery fences catalog writes | `denied` with recovery stage. |
| Product profile omits a gated descriptor route | `unsupported` or `unlicensed` according to route admission. |

## Practical Patterns

Descriptor plus domain:

```sql
create type descriptor app.email_text_descriptor
as varchar(320)
  character set utf8
  collation sys.fn.unicode_ci
  capability comparable, orderable, hashable, groupable, indexable, storable, wire_renderable;

create domain app.email_text as app.email_text_descriptor
  not null
  check (regexp_like(value, '^[^@]+@[^@]+$'));
```

Descriptor for document payloads:

```sql
create type descriptor app.event_document_descriptor
as document
  capability comparable, storable, wire_renderable, element_addressable
  index compatibility sys.fn.document_path_exact_recheck;
```

Descriptor for a vector embedding:

```sql
create type descriptor app.embedding_768
as vector<float32, 768>
  capability comparable, indexable, storable, wire_renderable
  index compatibility sys.fn.vector_cosine_exact_recheck;
```

## Verification Checklist

| Check | Required Outcome |
| --- | --- |
| Parse | Every lifecycle shape is recognized as SBsql type descriptor DDL or inspection. |
| Bind | Carrier, modifiers, referenced policies, capabilities, and dependencies resolve. |
| Create | Catalog rows persist descriptor UUID, carrier family, version, modifiers, capabilities, and contracts. |
| Alter | Compatibility-preserving changes commit; unsafe changes refuse or require explicit migration. |
| Rename | Resolver label changes while descriptor UUID and dependent bindings remain stable. |
| Version | Old and new descriptor versions coexist where stored data requires both. |
| Casts | Cast policy controls implicit, explicit, lossy, rendering, and protected release conversions. |
| Comparison | Equality, order, hash, grouping, uniqueness, and index behavior use the same contract. |
| Storage | Storage codec and transport profiles are validated before values use them. |
| Indexes | Index compatibility requires exact recheck where descriptor policy requires it. |
| Show/describe | Metadata is authorized and redacted according to policy. |
| Drop | `RESTRICT` refuses dependencies; `CASCADE` requires explicit authorized dependency handling. |
| Transaction | Rollback restores old descriptor state; commit invalidates stale descriptor evidence. |
| Recovery | Reopen after crash does not leave partial descriptor mutations visible. |
| Proof | Full rebuild tests regenerate parser, SBLR, catalog, security, descriptor, cast, index, transaction, and refusal evidence. |

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| `create_type_descriptor_statement` | statement | DDL | yes | catalog mutation |
| `alter_type_descriptor_statement` | statement | DDL | yes | catalog mutation |
| `rename_type_descriptor_statement` | statement | DDL | yes | catalog mutation |
| `recreate_type_descriptor_statement` | statement | DDL | yes | catalog mutation |
| `drop_type_descriptor_statement` | statement | DDL | yes | catalog mutation |
| `comment_on_type_descriptor_statement` | statement | metadata | yes | catalog mutation |
| `show_type_descriptor_statement` | statement | inspection | yes | metadata rowset |
| `describe_type_descriptor_statement` | statement | inspection | yes | metadata rowset |
| `descriptor_carrier` | grammar production | type binding | yes | descriptor payload |
| `type_capability_list` | grammar production | type binding | yes | capability payload |

# Binary, UUID, And Protected Values

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `data_types_binary_uuid_protected`


## Purpose

UUID literals are identity evidence and must match the expected object class. Binary values bind through byte-oriented descriptors and are not interchangeable with text unless an explicit conversion surface admits it.

Protected material uses catalog and security policy. Raw secret material must not be exposed through ordinary parser packets, support bundles, diagnostics, or bridge messages. SBsql can inspect redacted metadata when authorized, but protected value reachability and release remain engine/security decisions.

Example:

```sql
comment on table uuid '018f0000-0000-7000-8000-000000000001' is 'stable automation target';
```

## Supported Binary And Identity Types

| Canonical Type | Common Aliases | Length Unit | Payload | Value Bounds |
| --- | --- | --- | --- | --- |
| `binary(n)` | fixed byte string | bytes | Exactly `n` bytes plus descriptor metadata | Exactly `n` bytes. |
| `varbinary(n)` | `binary varying(n)` | bytes | 0 through `n` bytes plus descriptor metadata | 0 through `n` bytes. |
| `blob` | `binary large object`, Firebird `BLOB SUB_TYPE BINARY` where admitted | bytes or stream chunks | Byte LOB/overflow stream | Policy bounded by stream, row, page, overflow, and transaction limits. |
| `bytea` | PostgreSQL-compatible byte array where admitted | bytes | Variable byte payload | Policy bounded; PostgreSQL parser renders PostgreSQL byte syntax. |
| `image` | donor-compatible binary/image alias where admitted | bytes or stream chunks | Binary LOB/overflow stream | Policy bounded; not portable outside profiles that admit the alias. |
| `uuid` | `uuid '<text>'` literal form | 16 bytes | RFC-style UUID bytes | 16 bytes; canonical text form is 36 characters with hyphens. |
| `secret_ref` | protected value reference | UUID plus metadata | Reference only | Raw secret value is not carried in ordinary parser, diagnostic, bridge, or support-bundle packets. |
| `protected_blob_ref` | protected binary reference | UUID plus metadata | Reference only | Protected payload release is engine/security authority only. |

Binary values are byte sequences. They do not carry charset or collation descriptors, so text functions, pattern matching, text indexes, and donor text rendering require an explicit conversion or a donor-profile rule.

## UUID Contract

| Rule | Behavior |
| --- | --- |
| Stored size | UUID values store as 16 bytes. |
| Literal syntax | `uuid '<canonical-text>'` binds a UUID value. A bare string is text until cast or context forces a UUID descriptor. |
| Object identity | When a UUID references a catalog object, binder and authorization must verify the expected object class. A syntactically valid UUID is only identity evidence. |
| Rendering | Default rendering is lower-case canonical UUID text. Donor profiles may render compatible text but cannot change the underlying 16-byte identity. |
| Comparison and indexes | UUID equality and ordering use the descriptor comparison rule and remain subject to MGA and security recheck. |

## Protected Value Contract

Protected values are never ordinary binary or text values. SBsql may refer to protected material by authorized reference, but the raw material must not appear in:

- parser packets;
- SBLR payloads that are not explicitly protected-value envelopes;
- bridge messages;
- support bundles;
- diagnostics;
- logs;
- donor compatibility catalog rows.

Authorized inspection surfaces may return redacted metadata, reachability, owner, policy, expiry, rotation status, or audit identity. They must not return the raw secret.

## Binary Donor Profile Notes

| Donor Profile | Binary Compatibility Rule |
| --- | --- |
| Firebird | Preserves `BLOB SUB_TYPE BINARY`, segment/stream behavior where surfaced, and denies server-local file access unless a policy-admitted SBsql-only operation explicitly allows it. |
| PostgreSQL | Preserves `bytea` syntax and binary literal rendering where surfaced. |
| MySQL and MariaDB | Preserves `BINARY`, `VARBINARY`, `BLOB` family aliases, byte length semantics, and profile rendering. |
| SQLite | Preserves BLOB affinity and literal behavior while binding stored values to binary descriptors. |

## Syntax Productions

```ebnf
uuid_ref                ::= "UUID" string_literal ;
```

```ebnf
literal                 ::= string_literal | numeric_literal | boolean_literal | null_literal | uuid_ref ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and profile options.
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

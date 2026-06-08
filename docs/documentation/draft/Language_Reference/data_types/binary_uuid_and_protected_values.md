# Binary, UUID, And Protected Values

This page is part of the SBsql Language Reference Manual. It defines binary
descriptors, UUID descriptors, catalog UUID references, large binary values,
protected-material references, conversion behavior, security behavior, and
diagnostics.

Generation task: `data_types_binary_uuid_protected`

## Purpose

Binary values are byte sequences. They do not carry character set, collation, or
text rendering behavior. UUID values are 16-byte descriptors that can be used as
application values or as catalog identity references. Protected values are
security-controlled references to sensitive material and must not be treated as
ordinary text or binary data.

The binder must know which of these categories a value belongs to before SBLR
admission.

## Supported Binary And Identity Types

| Canonical Type | Common Aliases | Unit | Payload | Bounds |
| --- | --- | --- | --- | --- |
| `binary(n)` | fixed byte string | bytes | Exactly `n` bytes plus descriptor metadata. | Exactly `n` bytes. |
| `varbinary(n)` | `binary varying(n)` | bytes | 0 through `n` bytes plus descriptor metadata. | 0 through `n` bytes. |
| `blob` | `binary large object` | bytes or stream chunks | Binary large-value stream or overflow value. | Policy bounded by row, page, overflow, stream, and transaction limits. |
| `bytea` | byte array alias where admitted | bytes | Variable byte payload. | Policy bounded. |
| `uuid` | UUID literal and UUID-valued columns | 16 bytes | RFC-style UUID bytes. | Exactly 16 bytes. |
| `secret_ref` | protected secret reference | UUID plus metadata | Reference to protected material. | Raw secret is not carried in ordinary values. |
| `protected_blob_ref` | protected binary reference | UUID plus metadata | Reference to protected binary material. | Raw payload release requires policy. |

## Binary Values

Binary values are byte descriptors. They can be compared, hashed, stored,
indexed, streamed, and rendered only through binary-aware operations.

| Operation | Rule |
| --- | --- |
| Equality | Byte-wise descriptor comparison unless a domain or operation policy overrides it. |
| Ordering | Byte-ordering only where a binary descriptor admits ordering. |
| Hashing | Uses binary descriptor hash rules. |
| Text functions | Refused unless an explicit conversion supplies charset/encoding. |
| Pattern matching | Requires a binary-pattern operation, not text collation. |
| Indexing | Uses binary descriptor keys and exact recheck where required. |
| Large values | Use overflow or stream descriptors; inline row storage is not assumed. |

Example:

```sql
create table app.file_store (
    file_id uuid primary key,
    digest binary(32) not null,
    payload blob
);
```

## Binary Literals And Encoding

Binary literal syntax binds to byte descriptors. Text literal syntax does not
become binary without an explicit conversion.

| Form | Binding Rule |
| --- | --- |
| Binary literal | Binds as bytes under the active binary literal profile. |
| Hex text converted to binary | Requires explicit decode or cast function. |
| Binary converted to text | Requires explicit encoding or charset conversion. |
| Large binary stream | Requires a stream descriptor and policy-admitted frame limits. |

Invalid hex, invalid base encoding, unsupported encoding, over-length values,
and binary/text confusion return diagnostics.

## UUID Values

UUID values store as 16 bytes. Default rendering is canonical lower-case text
with hyphens.

| Rule | Behavior |
| --- | --- |
| Scalar UUID | Application data value with UUID descriptor. |
| Catalog UUID reference | Identity evidence for a catalog object. Requires object-class, visibility, sandbox, authorization, and policy checks. |
| Literal syntax | `uuid '<canonical-text>'` binds a UUID value. |
| String literal | Remains text until a cast or target descriptor binds it as UUID. |
| Comparison | Uses UUID descriptor comparison. |
| Indexing | UUID indexes use UUID descriptor keys and still require MGA/security recheck. |

Example:

```sql
select cast('018f0000-0000-7000-8000-000000000001' as uuid) as object_id;
```

Example catalog reference:

```sql
describe table uuid '018f0000-0000-7000-8000-000000000001';
```

Knowing a UUID does not grant access. The resolved object must be visible and
authorized.

## Protected Values

Protected values are not ordinary binary or text values. They are references to
material whose release is controlled by security policy.

Protected material can include:

- secrets;
- credentials;
- keys;
- tokens;
- encrypted payload handles;
- protected binary values;
- protected text values;
- sensitive diagnostic fields;
- support-bundle evidence references.

Rules:

- raw secret material must not appear in ordinary parser packets;
- raw secret material must not appear in SBLR payloads except in an explicitly
  protected envelope admitted by policy;
- support bundles, logs, diagnostics, catalog display, and bridge messages
  redact protected material by default;
- casts from protected material to raw text or raw binary are denied unless an
  explicit release surface admits them;
- export, backup, replication, migration, bridge, and stream routes are release
  surfaces when protected values can cross a boundary;
- release should produce audit evidence where policy requires it.

## Protected References

A protected reference can be stored or passed without exposing the raw value.

| Reference | Meaning |
| --- | --- |
| `secret_ref` | Reference to secret material managed by an admitted provider or protected catalog. |
| `protected_blob_ref` | Reference to protected binary material. |
| Protected descriptor UUID | Descriptor controlling release, masking, rotation, expiry, and audit behavior. |
| Policy binding | Rule that decides who can resolve, rotate, release, export, or inspect the protected material. |

Authorized inspection can return redacted metadata such as owner, status,
rotation time, expiry time, reachability, and policy identity. It must not
return the raw protected value unless release authority is explicitly admitted.

## Conversion Rules

| Conversion | Default Rule |
| --- | --- |
| `binary` to `text` | Explicit encoding or charset conversion required. |
| `text` to `binary` | Explicit encoding, decode function, or binary assignment policy required. |
| `text` to `uuid` | Explicit cast or UUID target required; invalid text is diagnostic. |
| `uuid` to `text` | Explicit cast renders canonical UUID text. |
| `uuid` to `binary(16)` | Explicit cast required. |
| `binary(16)` to `uuid` | Explicit cast required and validates UUID descriptor policy. |
| Protected reference to raw value | Denied unless an explicit release route admits it. |
| Raw value to protected reference | Requires an admitted protect/store route, not an ordinary cast. |

## Large Binary Values And Streams

Large binary values use overflow or stream descriptors. They are not guaranteed
to fit inline in a row.

Stream contracts define:

- frame type;
- maximum frame size;
- maximum in-flight bytes;
- timeout;
- cancellation behavior;
- retry behavior;
- checksum or digest policy where required;
- transaction ownership;
- protected-material release behavior;
- completion diagnostics.

## Diagnostics

| Condition | Required Result |
| --- | --- |
| Binary length mismatch for `binary(n)` | Assignment diagnostic. |
| `varbinary(n)` length exceeded | Assignment diagnostic. |
| Large binary exceeds stream or overflow policy | Stream/storage diagnostic. |
| Invalid binary literal encoding | Parse or conversion diagnostic. |
| Binary used as text without conversion | Bind diagnostic. |
| UUID text invalid | Conversion diagnostic. |
| UUID object reference wrong class | Bind/admission diagnostic. |
| UUID object hidden or outside sandbox | Denied or redacted not-visible diagnostic. |
| Protected material rendered without authority | Denied message vector. |
| Protected material appears in support output | Test failure; output must be redacted. |

## Syntax Productions

```ebnf
binary_type             ::= fixed_binary_type
                          | varying_binary_type
                          | large_binary_type ;
```

```ebnf
fixed_binary_type       ::= "binary" "(" length ")" ;
varying_binary_type     ::= "varbinary" "(" length ")"
                          | "binary" "varying" "(" length ")" ;
large_binary_type       ::= "blob"
                          | "binary" "large" "object"
                          | "bytea" ;
```

```ebnf
uuid_type               ::= "uuid" ;
uuid_literal            ::= "uuid" string_literal ;
uuid_ref                ::= "UUID" string_literal ;
```

```ebnf
protected_type          ::= "secret_ref"
                          | "protected_blob_ref" ;
```

## Related Pages

- [Type System Overview](type_system_overview.md)
- [UUID Catalog Identity](../core_paradigms/uuid_catalog_identity.md)
- [Security And Sandboxing](../core_paradigms/security_and_sandboxing.md)
- [Conversion Matrix](conversion_matrix.md)
- [COPY Streaming Import And Export](../syntax_reference/copy.md)
- [Backup, Restore, Replication, And Migration](../syntax_reference/backup_restore_replication_migration.md)

## Verification Checklist

The binary/UUID/protected-value proof suite should demonstrate:

- fixed binary values require exactly the declared byte count;
- variable binary values reject values above the declared byte count;
- binary values do not accidentally use text charset or collation;
- binary-to-text conversion requires an explicit encoding or charset rule;
- UUID values store and compare as 16-byte descriptors;
- UUID catalog references require object-class and authorization checks;
- knowing an object UUID does not bypass sandboxing;
- large binary streams enforce frame and transaction limits;
- protected references do not expose raw values in ordinary result sets;
- logs, diagnostics, support bundles, bridge messages, and catalog projections
  redact protected material by default;
- release routes produce explicit authorization and audit evidence where policy
  requires it.

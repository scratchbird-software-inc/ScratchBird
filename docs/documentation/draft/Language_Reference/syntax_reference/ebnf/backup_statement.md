# Backup Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the grammar production for `BACKUP` while preserving the ScratchBird authority model: parsing recognizes shape, binding resolves descriptors and UUID catalog identity, SBLR admits the operation route, and the engine owns snapshots and recovery.

Generation task: `ebnf_backup_statement`

Parent reference: [Backup, Restore, Replication, And Migration](../backup_restore_replication_migration.md)

## Production

```ebnf
backup_statement ::=
    BACKUP backup_target backup_destination backup_option_list? ;

backup_target ::=
      DATABASE database_ref
    | SCHEMA schema_ref
    | TABLE table_ref
    | QUERY "(" query_statement ")" ;

backup_destination ::=
      TO STREAM parameter_ref
    | TO BACKUP SET qualified_name
    | TO LOCATION location_ref ;

backup_option_list ::=
    WITH backup_option ("," backup_option)* ;

backup_option ::=
      FORMAT LOGICAL
    | FORMAT NATIVE
    | SNAPSHOT snapshot_option
    | INCLUDE DATA
    | INCLUDE METADATA
    | INCLUDE SECURITY
    | INCLUDE STATISTICS
    | EXCLUDE SECURITY
    | COMPRESS compression_profile
    | ENCRYPT WITH key_ref
    | MANIFEST qualified_name
    | LABEL string_literal
    | COMMENT string_literal ;
```

## Meaning

`backup_statement` recognizes an export request. The target may be a database, schema, table, or query rowset. The destination may be a stream parameter, engine-owned backup set, or policy-admitted location. The grammar does not authorize reading data, creating artifacts, or disclosing metadata.

## Binding Requirements

| Element | Binding requirement |
| --- | --- |
| Target | Authorized catalog scope or query result descriptor. |
| Destination | Stream handle, backup set name, or location descriptor admitted by policy. |
| Format | Logical or native route supported by the running product profile. |
| Snapshot | Engine-owned snapshot descriptor. |
| Security options | Privileges for including security metadata and protected references. |
| Manifest | Manifest descriptor and write privilege where requested. |

## Used By

| Parent production | Purpose |
| --- | --- |
| `archive_replication_migration_statement` | Places `BACKUP` in the administrative data-movement family. |

## Admission Notes

- Logical backup exports typed descriptors and row streams.
- Native backup set creation is engine-owned and policy-gated.
- Raw server-local paths should be represented by named locations and are denied unless policy admits them.
- A backup stream is evidence, not commit authority.

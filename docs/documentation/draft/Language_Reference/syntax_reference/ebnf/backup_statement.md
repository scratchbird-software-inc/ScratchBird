# Backup Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the grammar production for `BACKUP` while preserving the ScratchBird authority model: parsing recognizes shape, binding resolves descriptors and UUID catalog identity, SBLR admits the operation route, and the engine owns snapshots and recovery.

Generation task: `ebnf_backup_statement`

Parent reference: [Backup, Restore, Replication, And Migration](../backup_restore_replication_migration.md)

## Production

```ebnf
backup_stmt ::=
    BACKUP DATABASE database_ref TO backup_destination backup_options? ;

backup_destination ::=
    manifest_uri ;

backup_options ::=
    WITH backup_option ("," backup_option)* ;
```

## Meaning

`backup_stmt` recognizes a database-level logical backup export request. The target is always a DATABASE reference. The destination is a manifest URI (file or location reference). The grammar does not authorize reading data, creating artifacts, or disclosing metadata. Evidence: conformance test SBSQL-01F52A6E564D with sql_fixture `BACKUP DATABASE TO '<manifest>.sblbak'`.

## Binding Requirements

| Element | Binding requirement |
| --- | --- |
| Target | Authorized database UUID from session context. |
| Destination | Manifest URI descriptor admitted by policy. |
| Options | Archive-level options where admitted. |

## Used By

| Parent production | Purpose |
| --- | --- |
| `archive_replication_stmt` | Places `BACKUP` in the administrative data-movement family. |

## Admission Notes

- Logical backup exports typed descriptors and row streams.
- Native backup set creation is engine-owned and policy-gated.
- Raw server-local paths should be represented by named locations and are denied unless policy admits them.
- A backup stream is evidence, not commit authority.

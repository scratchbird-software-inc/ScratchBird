# Archive Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the grammar production for `ARCHIVE` while preserving the ScratchBird authority model: parsing recognizes shape, binding resolves descriptors and UUID catalog identity, SBLR admits the operation route, and the engine owns durable archive catalog state.

Generation task: `ebnf_archive_statement`

Parent reference: [Backup, Restore, Replication, And Migration](../backup_restore_replication_migration.md)

## Production

```ebnf
archive_statement ::=
    ARCHIVE archive_action archive_payload? archive_option_list? ;

archive_action ::=
      SHOW
    | DESCRIBE
    | REGISTER
    | VERIFY
    | PIN
    | UNPIN
    | EXPIRE
    | DROP ;

archive_payload ::=
      BACKUP SET qualified_name
    | MANIFEST qualified_name
    | DATABASE database_ref ;

archive_option_list ::=
    WITH archive_option ("," archive_option)* ;
```

## Meaning

`archive_statement` recognizes operations over backup-set and manifest metadata. Archive metadata is evidence and retention state. It does not override object authorization, stream validation, manifest checks, or MGA transaction finality.

## Binding Requirements

| Element | Binding requirement |
| --- | --- |
| Action | Authorized archive operation. |
| Payload | Visible backup set, manifest, or database scope. |
| Options | Retention, comment, label, verification, and policy descriptors where admitted. |
| Result | Authorized report shape with redaction policy. |

## Used By

| Parent production | Purpose |
| --- | --- |
| `archive_replication_migration_statement` | Places `ARCHIVE` in the administrative data-movement family. |

## Admission Notes

- `VERIFY` validates evidence; it does not repair data.
- `PIN` and `UNPIN` affect retention policy only.
- `DROP` must not remove managed artifacts unless policy admits that action explicitly.
- Inspection surfaces must redact hidden locations, credentials, and object details.

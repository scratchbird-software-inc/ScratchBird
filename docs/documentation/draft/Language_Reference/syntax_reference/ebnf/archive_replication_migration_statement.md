# Archive Replication Migration Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the top-level grammar production for backup, restore, archive, replication, changefeed, and migration statements. Parsing recognizes shape; binding resolves descriptors and UUID catalog identity; SBLR admits the operation route; the engine owns transaction finality and recovery.

Generation task: `ebnf_archive_replication_migration_statement`

Parent reference: [Backup, Restore, Replication, And Migration](../backup_restore_replication_migration.md)

## Production

```ebnf
archive_replication_migration_statement ::=
      archive_statement
    | backup_statement
    | restore_statement
    | replication_statement
    | migration_statement ;
```

## Meaning

This production groups administrative data-movement statements. It does not authorize backup, restore, archive, replication, or migration work by itself. Each child production must bind to an admitted operation family, administrative privilege, object scope, stream or location descriptor, and transaction or job context.

## Used By

| Parent production | Purpose |
| --- | --- |
| `native_statement` | Allows the statement family in ordinary SBsql statement streams. |
| `script_statement` | Allows scripted administration where policy admits it. |

## Child Productions

| Child production | Role |
| --- | --- |
| `backup_statement` | Exports logical streams or native backup sets. |
| `restore_statement` | Validates or applies logical streams and admitted backup sets. |
| `archive_statement` | Manages backup-set and manifest metadata. |
| `replication_statement` | Manages replication routes and changefeeds. |
| `migration_statement` | Plans, imports, replays, compares, validates, and cuts over migration routes. |

## Admission Notes

- Every child statement is administrative and must pass explicit privilege checks.
- Stream or location handles must bind to policy-admitted descriptors.
- External ordering evidence is not transaction finality authority.
- Local applied work commits or rolls back through MGA.
- Unsafe server-local file manipulation, low-level repair, and page verification are separate surfaces.

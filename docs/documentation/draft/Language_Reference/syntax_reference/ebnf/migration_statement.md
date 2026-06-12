# Migration Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the grammar production for `MIGRATION` while preserving the ScratchBird authority model: parsing recognizes shape, binding resolves descriptors and UUID catalog identity, SBLR admits the operation route, and local apply work commits or rolls back through MGA.

Generation task: `ebnf_migration_statement`

Parent reference: [Backup, Restore, Replication, And Migration](../backup_restore_replication_migration.md)

## Production

```ebnf
migration_statement ::=
    MIGRATION migration_action migration_payload? migration_option_list? ;

migration_action ::=
      PLAN
    | CREATE
    | IMPORT
    | REPLAY
    | COMPARE
    | VALIDATE
    | CUTOVER
    | ABORT
    | SHOW
    | DESCRIBE
    | DROP ;

migration_payload ::=
      ROUTE qualified_name
    | PLAN qualified_name
    | SOURCE migration_endpoint
    | TARGET migration_endpoint ;

migration_option_list ::=
    WITH migration_option ("," migration_option)* ;

migration_option ::=
      SOURCE migration_endpoint
    | TARGET migration_endpoint
    | MAP NAMES mapping_ref
    | MAP TYPES mapping_ref
    | MAP SECURITY mapping_ref
    | MODE SNAPSHOT
    | MODE CONTINUOUS
    | VALIDATE ONLY
    | QUARANTINE INVALID ROWS
    | CUTOVER WHEN migration_cutover_condition ;
```

## Meaning

`migration_statement` recognizes planning, import, replay, compare, validation, cutover, abort, inspection, and drop actions for a migration route or plan. Migration statements coordinate data movement; they do not let source descriptors, bridge state, or stream tokens override local catalog or transaction authority.

## Binding Requirements

| Element | Binding requirement |
| --- | --- |
| Plan or route | Durable migration metadata visible to the caller. |
| Source | Endpoint, stream, database, schema, table, or query descriptor admitted by policy. |
| Target | Database, schema, table, or workarea descriptor admitted by policy. |
| Mappings | Name, type, security, filespace, and identity mapping descriptors where required. |
| Validation | Descriptor, row count, checksum, sample, comparison, or policy evidence. |
| Cutover | Explicit cutover condition and safe boundary. |

## Used By

| Parent production | Purpose |
| --- | --- |
| `archive_replication_stmt` | Places `MIGRATION` in the administrative data-movement family. |

## Admission Notes

- `VALIDATE ONLY` must not apply data.
- Import and replay apply through engine-owned routes and local transactions.
- Invalid or ambiguous rows should be quarantined when policy admits it.
- Cutover must fail closed when source, target, validation, or replay state is uncertain.

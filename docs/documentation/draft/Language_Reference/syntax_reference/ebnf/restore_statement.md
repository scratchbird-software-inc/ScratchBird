# Restore Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It documents the grammar production for `RESTORE` while preserving the ScratchBird authority model: parsing recognizes shape, binding resolves descriptors and UUID catalog identity, SBLR admits the operation route, and the engine owns transaction finality and recovery.

Generation task: `ebnf_restore_statement`

Parent reference: [Backup, Restore, Replication, And Migration](../backup_restore_replication_migration.md)

## Production

```ebnf
restore_stmt ::=
    RESTORE restore_target restore_source restore_option_list? ;

restore_target ::=
      DATABASE database_ref
    | SCHEMA schema_ref
    | TABLE table_ref
    | WORKAREA qualified_name ;

restore_source ::=
      FROM STREAM parameter_ref
    | FROM BACKUP SET qualified_name
    | FROM LOCATION location_ref ;

restore_option_list ::=
    WITH restore_option ("," restore_option)* ;

restore_option ::=
      FORMAT LOGICAL
    | FORMAT NATIVE
    | VALIDATE ONLY
    | REPLACE
    | NO REPLACE
    | MAP NAMES mapping_ref
    | MAP SECURITY mapping_ref
    | TARGET SCHEMA schema_ref
    | TARGET FILESPACE filespace_ref
    | RECOVER UNTIL recovery_boundary
    | QUARANTINE INVALID ROWS
    | REQUIRE MANIFEST qualified_name ;
```

## Meaning

`restore_stmt` recognizes an import or validation request. The grammar accepts target and source descriptors plus restore options. It does not apply data by itself. Applying restore data must go through engine-owned catalog and row routes, and applied work commits or rolls back through MGA.

## Binding Requirements

| Element | Binding requirement |
| --- | --- |
| Target | Existing or creatable database, schema, table, or workarea scope. |
| Source | Stream handle, backup set, or location descriptor admitted by policy. |
| Format | Logical or native restore route supported by the running product profile. |
| Mapping | Name, type, security, or filespace mapping descriptors where required. |
| Manifest | Manifest descriptor and validation evidence where required. |
| Apply mode | Validate-only, replace, no-replace, quarantine, and recovery boundary rules. |

## Used By

| Parent production | Purpose |
| --- | --- |
| `archive_replication_stmt` | Places `RESTORE` in the administrative data-movement family. |

## Admission Notes

- `VALIDATE ONLY` must not apply user data.
- `REPLACE` is destructive and should be policy-gated.
- Raw page mutation and repair behavior are outside this statement family.
- Restore apply operations use local transaction finality.

# Backup, Restore, Replication, And Migration

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `syntax_reference_backup_restore_replication_migration`


## Purpose

Logical backup, restore, replication, migration, CDC, and archive commands operate through policy-bound streams. A remote logical stream can be admitted where the surface and policy allow it. Server-local file manipulation is denied by default for parser routes and policy-controlled for SBsql.

Physical page-copy backup/restore and low-level repair/verify behavior are not SBsql-parser operations. SBsql-only maintenance surfaces must still pass authorization and recovery checks.

Example:

```sql
backup database app to stream :client_stream;
restore database app from stream :client_stream;
```

## Syntax Productions

```ebnf
archive_replication_migration_statement ::= archive_statement | backup_statement | restore_statement | replication_statement | migration_statement ;
```

```ebnf
backup_statement        ::= "BACKUP" backup_target backup_options? ;
```

```ebnf
restore_statement       ::= "RESTORE" restore_source restore_options? ;
```

```ebnf
replication_statement   ::= "REPLICATION" replication_action replication_payload? ;
```

```ebnf
migration_statement     ::= "MIGRATION" migration_action migration_payload? ;
```

```ebnf
archive_statement       ::= "ARCHIVE" archive_action archive_payload? ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and SBsql execution options.
- SBLR admission maps the bound request to an operation family and result shape.
- The engine rechecks authority before durable state changes or result delivery.

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| backup_stmt | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |
| cluster_publish_options | grammar_production | archive_replication | yes | rs.sbsql.cluster_private_refusal.v1 |
| restore_options | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |
| restore_stmt | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |
| backup_options | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |
| changefeed_options | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |
| archive_stmt | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |
| archive_replication_stmt | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |
| changefeed_stmt | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |
| replication_stmt | grammar_production | archive_replication | yes | rs.sbsql.admin_command_or_report.v1 |

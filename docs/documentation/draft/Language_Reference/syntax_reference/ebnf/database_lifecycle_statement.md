# Database Lifecycle Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It expands the database lifecycle statement family used by database creation, open/attach/use/detach, lifecycle modes, verification, repair, shutdown, drop, and authorized inspection.

Related pages: [Database Lifecycle](../database.md), [Filespace Lifecycle](../filespace.md), [Transaction Control](../transaction_control.md), and [Security And Privileges](../security_and_privilege_statements.md).

## Production

```ebnf
database_lifecycle_statement ::=
      create_database_statement
    | open_database_statement
    | attach_database_statement
    | use_database_statement
    | detach_database_statement
    | alter_database_statement
    | maintenance_database_statement
    | inspect_database_statement
    | verify_database_statement
    | repair_database_statement
    | shutdown_database_statement
    | drop_database_statement
    | rename_database_statement
    | comment_database_statement
    | recreate_database_statement
    | show_database_statement
    | describe_database_statement ;
```

```ebnf
create_database_statement ::= CREATE DATABASE database_name database_create_options? ;
open_database_statement   ::= OPEN DATABASE database_ref (RESTRICTED OPEN?)? ;
attach_database_statement ::= ATTACH DATABASE storage_ref AS database_alias attach_options? ;
use_database_statement    ::= USE DATABASE database_alias | USE database_alias ;
detach_database_statement ::= DETACH DATABASE database_alias detach_options? ;
```

```ebnf
alter_database_statement ::=
    ALTER DATABASE database_ref alter_database_action+ ;

alter_database_action ::=
      SET DEFAULT SCHEMA schema_name
    | SET DEFAULT FILESPACE filespace_name
    | SET DEFAULT CHARACTER SET charset_name
    | SET DEFAULT COLLATION collation_name
    | SET MAINTENANCE maintenance_options?
    | CLEAR MAINTENANCE
    | ENTER MAINTENANCE maintenance_options?
    | EXIT MAINTENANCE
    | ENTER RESTRICTED OPEN restricted_open_options?
    | EXIT RESTRICTED OPEN
    | VERIFY verify_options?
    | REPAIR repair_options
    | SHUTDOWN shutdown_options? ;
```

```ebnf
maintenance_database_statement ::=
      MAINTENANCE DATABASE database_ref maintenance_options?
    | ENTER DATABASE MAINTENANCE database_ref maintenance_options?
    | EXIT DATABASE MAINTENANCE database_ref ;

inspect_database_statement ::=
      INSPECT DATABASE database_ref inspect_options?
    | DIAGNOSE DATABASE database_ref inspect_options? ;

verify_database_statement ::= VERIFY DATABASE database_ref verify_options? ;
repair_database_statement ::= REPAIR DATABASE database_ref repair_options ;
```

```ebnf
shutdown_database_statement ::=
      SHUTDOWN DATABASE database_ref shutdown_options?
    | FORCE SHUTDOWN DATABASE database_ref force_shutdown_options
    | ACKNOWLEDGE SHUTDOWN DATABASE database_ref
    | SHUTDOWN ACKNOWLEDGE DATABASE database_ref ;

drop_database_statement ::=
    DROP DATABASE database_ref drop_database_options? ;

show_database_statement ::=
      SHOW DATABASE database_ref?
    | SHOW DATABASES ;
```

## Meaning

This production is a lifecycle family, not ordinary parser-side file access. Each statement must bind to a database UUID, session alias, storage reference, or lifecycle descriptor and lower to an admitted SBLR lifecycle or observability operation.

Database lifecycle execution belongs to the engine lifecycle API. Catalog mutations inside an open database still obey MGA transaction finality, but database create/open/attach/shutdown/drop behavior is lifecycle-controlled and fail-closed.

## Examples

```sql
create database tenant_a with default schema app, filespace primary_data;
attach database 'policy://databases/tenant_a' as tenant_a;
use database tenant_a;
verify database tenant_a with checksum;
shutdown database tenant_a;
drop database tenant_a restrict;
```

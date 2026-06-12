# Database Lifecycle Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It expands the database lifecycle statement family used by database creation, open/attach/use/detach, lifecycle modes, verification, repair, shutdown, drop, and authorized inspection.

Related pages: [Database Lifecycle](../database.md), [Filespace Lifecycle](../filespace.md), [Transaction Control](../transaction_control.md), and [Security And Privileges](../security_and_privilege_statements.md).

## Production

```ebnf
database_lifecycle_stmt ::=
      create_database_stmt
    | open_database_stmt
    | attach_database_stmt
    | use_database_alias
    | detach_database_stmt
    | alter_database_stmt
    | maintenance_database_stmt
    | inspect_database_stmt
    | verify_database_stmt
    | repair_database_stmt
    | shutdown_database_stmt
    | drop_database_stmt
    | rename_database_stmt
    | show_database_stmt
    | describe_database_stmt ;
```

```ebnf
create_database_stmt ::= CREATE DATABASE database_name database_create_options? ;
open_database_stmt   ::= OPEN DATABASE database_ref (RESTRICTED OPEN?)? ;
attach_database_stmt ::= ATTACH DATABASE storage_ref AS database_alias attach_options? ;
use_database_alias   ::= USE DATABASE database_alias | USE database_alias ;
detach_database_stmt ::= DETACH DATABASE database_alias detach_options? ;
```

```ebnf
alter_database_stmt ::=
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
maintenance_database_stmt ::=
      MAINTENANCE DATABASE database_ref maintenance_options?
    | ENTER DATABASE MAINTENANCE database_ref maintenance_options?
    | EXIT DATABASE MAINTENANCE database_ref ;

inspect_database_stmt ::=
      INSPECT DATABASE database_ref inspect_options?
    | DIAGNOSE DATABASE database_ref inspect_options? ;

verify_database_stmt ::= VERIFY DATABASE database_ref verify_options? ;
repair_database_stmt ::= REPAIR DATABASE database_ref repair_options ;
```

```ebnf
shutdown_database_stmt ::=
      SHUTDOWN DATABASE database_ref shutdown_options?
    | FORCE SHUTDOWN DATABASE database_ref force_shutdown_options
    | ACKNOWLEDGE SHUTDOWN DATABASE database_ref
    | SHUTDOWN ACKNOWLEDGE DATABASE database_ref ;

drop_database_stmt ::=
    DROP DATABASE database_ref drop_database_options? ;

show_database_stmt ::=
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

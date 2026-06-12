# Filespace Lifecycle Statement EBNF Production

This page is part of the SBsql Language Reference Manual. It expands the filespace lifecycle statement family used by storage placement descriptors, filespace agents, policy bindings, health and capacity reporting, and safe retirement.

Related pages: [Filespace Lifecycle](../filespace.md), [Database Lifecycle](../database.md), [Index Lifecycle](../index.md), and [Security And Privileges](../security_and_privilege_statements.md).

## Production

```ebnf
filespace_lifecycle_statement ::=
      create_filespace_stmt
    | create_filespace_agent_stmt
    | alter_filespace_stmt
    | attach_filespace_stmt
    | show_filespace_extended
    | describe_stmt
    | rename_object_stmt
    | comment_on_stmt
    | drop_filespace_stmt ;
```

```ebnf
create_filespace_stmt ::=
    CREATE FILESPACE filespace_name filespace_create_options? ;

create_filespace_agent_stmt ::=
    CREATE FILESPACE AGENT filespace_agent_name
    FOR FILESPACE filespace_name?
    filespace_agent_options? ;
```

```ebnf
alter_filespace_stmt ::=
    ALTER FILESPACE filespace_name alter_filespace_action+ ;

alter_filespace_action ::=
      SET LOCATION storage_ref
    | SET ROLE filespace_role
    | SET MAX SIZE size_literal
    | SET GROW BY size_literal
    | SET RESERVE size_literal
    | SET LOW RESERVE THRESHOLD size_literal
    | SET SYNC sync_policy_name
    | SET CHECKSUM checksum_policy_name
    | SET READ ONLY
    | SET READ WRITE
    | SET ONLINE
    | SET OFFLINE
    | SET MAINTENANCE
    | CLEAR MAINTENANCE
    | REQUEST GROWTH filespace_growth_request
    | NOTIFY SHRINK READINESS filespace_shrink_descriptor ;
```

```ebnf
attach_filespace_stmt ::=
    ATTACH POLICY policy_name TO FILESPACE filespace_name
    (ROLE role_name)? ;

show_filespace_extended ::=
      SHOW FILESPACES
    | SHOW FILESPACE filespace_name
    | SHOW FILESPACE EXTENDED
    | SHOW FILESPACE filespace_name HEALTH
    | SHOW FILESPACE filespace_name CAPACITY
    | SHOW FILESPACE filespace_name SHRINK READINESS ;

drop_filespace_stmt ::=
    DROP FILESPACE filespace_name drop_filespace_options? ;
```

## Meaning

Filespace statements describe storage policy and request storage lifecycle work. They do not give the parser authority to open, create, delete, grow, or repair files. Allocation, growth, relocation, sync, integrity checking, and recovery fencing are engine storage behavior.

## Examples

```sql
create filespace primary_data
  location 'policy://storage/primary-data'
  role data
  max size 500 gb;

show filespace primary_data capacity;
show filespace primary_data shrink readiness;
drop filespace primary_data restrict;
```

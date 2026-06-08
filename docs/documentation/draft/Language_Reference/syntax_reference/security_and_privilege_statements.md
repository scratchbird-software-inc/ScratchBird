# Security And Privilege Statements

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `syntax_reference_security_privilege`


## Purpose

Security statements create identities, roles, groups, grants, revokes, and policy objects. The visible syntax is not enough: the active user or agent UUID must be authorized to perform the change.

Sandboxing applies before object visibility. Donor parser sessions see their emulated database branch unless authorized catalog projections expose additional metadata.

Example:

```sql
create role app_reader;
grant select on table app.orders to app_reader;
```

## Syntax Productions

```ebnf
security_statement      ::= create_identity | alter_identity | drop_identity | grant_statement | revoke_statement | show_security ;
```

```ebnf
create_identity         ::= "CREATE" ("USER" | "ROLE" | "GROUP") principal_ref identity_option_list? ;
```

```ebnf
alter_identity          ::= "ALTER" ("USER" | "ROLE" | "GROUP") principal_ref identity_option_list? ;
```

```ebnf
drop_identity           ::= "DROP" ("USER" | "ROLE" | "GROUP") principal_ref ;
```

```ebnf
grant_statement         ::= "GRANT" grant_payload "TO" principal_ref grant_option_list? ;
```

```ebnf
revoke_statement        ::= "REVOKE" grant_payload "FROM" principal_ref revoke_option_list? ;
```

```ebnf
policy_statement        ::= create_policy | alter_policy | drop_policy | show_policy ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and profile options.
- SBLR admission maps the bound request to an operation family and result shape.
- The engine rechecks authority before durable state changes or result delivery.

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| grantee_list | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| attach_policy_stmt | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| set_role_stmt | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| user_name | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| revoke_stmt | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| policy_name | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| activate_policy_stmt | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| policy_stmt | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| deactivate_policy_stmt | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| validate_policy_stmt | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| dcl_security_stmt | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| filespace_role | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| revoke | canonical_surface | security | yes | rs.sbsql.security_command_or_report.v1 |
| grant | canonical_surface | security | yes | rs.sbsql.security_command_or_report.v1 |
| grant_target | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| placement_policy_name | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| show_security_policy | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| role_name | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| member_role | grammar_production | security | yes | rs.sbsql.cluster_private_refusal.v1 |
| grantee | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| event_trigger_security_clause | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |
| policy_ref | grammar_production | security | yes | rs.sbsql.cluster_private_refusal.v1 |
| grant_stmt | grammar_production | security | yes | rs.sbsql.security_command_or_report.v1 |

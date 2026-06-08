# Security And Sandboxing

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `core_paradigms_security_and_sandboxing`


## Purpose

Security statements change durable authorization and policy state. The parser does not grant authority by recognizing a command. The effective user or agent UUID, active role context, group membership, policy snapshot, sandbox root, and object visibility rules decide admission.

Donor parser sessions are sandboxed to the emulated database/workarea that they attach to. SBsql can administer the broader schema tree when authorized. Donor compatibility catalog views may expose projected metadata, but those views operate through grants and policy, not through parser privilege.

Schema sandbox roots and resolver visibility are detailed in [../syntax_reference/schema_tree_and_name_resolution.md](../syntax_reference/schema_tree_and_name_resolution.md).

Explicit denial wins over allow. Hidden rows remain hidden unless a catalog projection has its own authority to render them.

## Syntax Productions

```ebnf
security_statement      ::= create_identity | alter_identity | drop_identity | grant_statement | revoke_statement | show_security ;
```

```ebnf
grant_statement         ::= "GRANT" grant_payload "TO" principal_ref grant_option_list? ;
```

```ebnf
revoke_statement        ::= "REVOKE" grant_payload "FROM" principal_ref revoke_option_list? ;
```

```ebnf
principal_ref           ::= uuid_ref | qualified_name ;
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

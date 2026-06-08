# Management And Operations Statements

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `syntax_reference_management_operations`


## Purpose

Management statements inspect or control runtime state. They are still ordinary parser surfaces: parse, bind, authorize, route, and then return either a result or a message vector.

Operational commands include SHOW, EXPLAIN, configuration reload/history, support bundles, package management, jobs, storage maintenance, and acceleration controls.

Example:

```sql
show management listeners;
explain analyze select * from app.orders where order_id = :id;
```

## Syntax Productions

```ebnf
management_statement    ::= show_management | alter_management | config_statement | support_bundle_statement ;
```

```ebnf
show_statement          ::= "SHOW" show_target show_option_list? ;
```

```ebnf
explain_statement       ::= "EXPLAIN" "ANALYZE"? query_statement ;
```

```ebnf
config_statement        ::= "CONFIG" ("RELOAD" | "HISTORY" | config_action) ;
```

```ebnf
support_bundle_statement::= "SUPPORT" "BUNDLE" support_bundle_action ;
```

```ebnf
show_acceleration       ::= "SHOW" ("LLVM" | "NATIVE" "COMPILE" | "AOT" "ARTIFACTS" | "GPU") acceleration_target? ;
```

```ebnf
alter_acceleration      ::= "ALTER" ("NATIVE" "COMPILE" | "GPU") acceleration_action ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and SBsql execution options.
- SBLR admission maps the bound request to an operation family and result shape.
- The engine rechecks authority before durable state changes or result delivery.

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| udr_binary_ref | grammar_production | runtime_management | yes | rs.sbsql.admin_command_or_report.v1 |
| udr_capability | grammar_production | runtime_management | yes | rs.sbsql.admin_command_or_report.v1 |
| agent_control_stmt | grammar_production | runtime_management | yes | rs.sbsql.admin_command_or_report.v1 |
| agent_filter | grammar_production | runtime_management | yes | rs.sbsql.admin_command_or_report.v1 |
| udr_entry_point | grammar_production | runtime_management | yes | rs.sbsql.admin_command_or_report.v1 |
| agent_name | grammar_production | runtime_management | yes | rs.sbsql.admin_command_or_report.v1 |
| agent_stmt | grammar_production | runtime_management | yes | rs.sbsql.admin_command_or_report.v1 |
| parser_package_stmt | grammar_production | runtime_management | yes | rs.sbsql.admin_command_or_report.v1 |
| udr_package_name | grammar_production | runtime_management | yes | rs.sbsql.admin_command_or_report.v1 |
| parser_name | grammar_production | runtime_management | yes | rs.sbsql.admin_command_or_report.v1 |
| udr_name | grammar_production | runtime_management | yes | rs.sbsql.admin_command_or_report.v1 |
| agent_lifecycle_stmt | grammar_production | runtime_management | yes | rs.sbsql.admin_command_or_report.v1 |
| listener_stmt | grammar_production | runtime_management | yes | rs.sbsql.admin_command_or_report.v1 |
| udr_package_stmt | grammar_production | runtime_management | yes | rs.sbsql.admin_command_or_report.v1 |
| explain | canonical_surface | observability | yes | rs.sbsql.observability_report.v1 |
| show_diagnostics | grammar_production | observability | yes | rs.sbsql.observability_report.v1 |
| explain_options | grammar_production | observability | yes | rs.sbsql.observability_report.v1 |
| show | canonical_surface | observability | yes | rs.sbsql.observability_report.v1 |
| psql_statement | grammar_production | observability | yes | rs.sbsql.observability_report.v1 |
| show_cluster_extended | grammar_production | observability | yes | rs.sbsql.cluster_private_refusal.v1 |
| statement | grammar_production | observability | yes | rs.sbsql.observability_report.v1 |
| show_acceleration_extended | grammar_production | observability | yes | rs.sbsql.observability_report.v1 |
| show_management | grammar_production | observability | yes | rs.sbsql.observability_report.v1 |
| show_stmt | grammar_production | observability | yes | rs.sbsql.observability_report.v1 |

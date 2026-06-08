# Syntax Reference

This directory contains SBsql statement, expression, lifecycle, and clause reference pages.

## Core Expression Pages

| Topic | File |
| --- | --- |
| Operators, precedence, associativity, symbolic forms, and donor-profile aliases | [operators.md](operators.md) |
| Operator operand and result descriptor matrix | [operator_type_result_matrix.md](operator_type_result_matrix.md) |
| Expression EBNF production | [ebnf/expression.md](ebnf/expression.md) |
| Projection expressions | [projection.md](projection.md) |
| Predicates and filtering | [where.md](where.md) |

## Statement And Object Pages

| Topic | File |
| --- | --- |
| SELECT | [select.md](select.md) |
| INSERT | [insert.md](insert.md) |
| UPDATE | [update.md](update.md) |
| DELETE | [delete.md](delete.md) |
| WITH and recursive CTEs | [with.md](with.md) |
| Tables | [table.md](table.md) |
| Schema tree, schema variables, name resolution, and sandbox roots | [schema_tree_and_name_resolution.md](schema_tree_and_name_resolution.md) |
| Indexes | [index.md](index.md) |
| Views | [view.md](view.md) |
| Functions | [function.md](function.md) |
| Procedures | [procedure.md](procedure.md) |
| Triggers | [trigger.md](trigger.md) |
| Domains | [domain.md](domain.md) |
| Type descriptors | [type_descriptor.md](type_descriptor.md) |
| Security and privileges | [security_and_privilege_statements.md](security_and_privilege_statements.md) |
| Transactions | [transaction_control.md](transaction_control.md) |
| Backup, restore, replication, and migration | [backup_restore_replication_migration.md](backup_restore_replication_migration.md) |

## Procedural SQL Pages

| Topic | File |
| --- | --- |
| Procedural SQL overview, routine forms, authority model, and proof expectations | [procedural_sql.md](procedural_sql.md) |
| Blocks, declarations, variables, parameters, assignment, and executable-body storage | [procedural_sql_blocks.md](procedural_sql_blocks.md) |
| Control flow, conditional logic, loops, returns, row emission, and dynamic execution boundaries | [procedural_sql_control_flow.md](procedural_sql_control_flow.md) |
| Cursors, row streams, positioned operations, and cursor metadata | [procedural_sql_cursors.md](procedural_sql_cursors.md) |
| Exceptions, diagnostics, conditions, `SIGNAL`, `RESIGNAL`, and handler behavior | [procedural_sql_exceptions.md](procedural_sql_exceptions.md) |
| Table triggers, event triggers, transition values, `WHEN` filters, event capture, and trigger execution context | [procedural_sql_triggers_and_events.md](procedural_sql_triggers_and_events.md) |

# Transaction Control

This page is part of the SBsql Language Reference Manual. It is generated from the SBsql grammar, surface registry, SBLR routing matrix, built-in operation registries, catalog-definition material, and parser/engine proof fixtures. It explains the user-facing language contract without treating SQL text as engine authority.

Generation task: `syntax_reference_transaction_control`


## Purpose

Transaction statements change the session transaction state. MGA remains the final authority for commit order, rollback behavior, snapshots, cleanup horizons, and recovery state.

Savepoints are scoped inside an active transaction. Retain and chain behavior must be explicit in transaction options where supported.

Procedural use of savepoints, execute blocks, autonomous blocks, and transaction restrictions is covered in [procedural_sql.md](procedural_sql.md), [procedural_sql_blocks.md](procedural_sql_blocks.md), and [procedural_sql_control_flow.md](procedural_sql_control_flow.md).

Example:

```sql
begin transaction;
insert into app.audit_event(event_id, event_text) values (:id, :text);
savepoint after_audit;
commit;
```

## Syntax Productions

```ebnf
transaction_statement   ::= begin_transaction | commit_transaction | rollback_transaction | savepoint_statement | set_transaction | show_transaction ;
```

```ebnf
begin_transaction       ::= "BEGIN" ("TRANSACTION" | "WORK")? transaction_option_list? ;
```

```ebnf
commit_transaction      ::= "COMMIT" ("TRANSACTION" | "WORK")? ;
```

```ebnf
rollback_transaction    ::= "ROLLBACK" ("TRANSACTION" | "WORK")? rollback_target? ;
```

```ebnf
savepoint_statement     ::= "SAVEPOINT" identifier | "RELEASE" "SAVEPOINT" identifier | "ROLLBACK" "TO" "SAVEPOINT"? identifier ;
```

```ebnf
set_transaction         ::= "SET" "TRANSACTION" transaction_option_list ;
```

## Binding And Execution

- The parser recognizes the syntax and builds a statement or expression tree.
- Binding resolves catalog names, UUID references, parameter descriptors, result descriptors, security context, transaction context, and profile options.
- SBLR admission maps the bound request to an operation family and result shape.
- The engine rechecks authority before durable state changes or result delivery.

## Related Surface Rows

| Surface | Kind | Family | Lowering | Result Shape |
| --- | --- | --- | --- | --- |
| rollback_stmt | grammar_production | transaction | yes | rs.sbsql.transaction_finality.v1 |
| begin_stmt | grammar_production | transaction | yes | rs.sbsql.transaction_finality.v1 |
| transaction_mode | grammar_production | transaction | yes | rs.sbsql.transaction_finality.v1 |
| set_transaction_stmt | grammar_production | transaction | yes | rs.sbsql.transaction_finality.v1 |
| show_transaction_runtime | grammar_production | transaction | yes | rs.sbsql.transaction_finality.v1 |
| commit_options | grammar_production | transaction | yes | rs.sbsql.transaction_finality.v1 |
| savepoint_stmt | grammar_production | transaction | yes | rs.sbsql.transaction_finality.v1 |
| lock_table | canonical_surface | transaction | yes | rs.sbsql.command_completion.v1 |
| commit | canonical_surface | transaction | yes | rs.sbsql.transaction_finality.v1 |
| cluster_commit_options | grammar_production | transaction | yes | rs.sbsql.cluster_private_refusal.v1 |
| rollback_to_savepoint_stmt | grammar_production | transaction | yes | rs.sbsql.transaction_finality.v1 |
| cluster_rollback_options | grammar_production | transaction | yes | rs.sbsql.cluster_private_refusal.v1 |
| begin_transaction | canonical_surface | transaction | yes | rs.sbsql.transaction_finality.v1 |
| lock_table_stmt | grammar_production | transaction | yes | rs.sbsql.command_completion.v1 |
| transaction_mode_list | grammar_production | transaction | yes | rs.sbsql.transaction_finality.v1 |
| psql_autonomous_block | grammar_production | transaction | yes | rs.sbsql.transaction_finality.v1 |
| commit_stmt | grammar_production | transaction | yes | rs.sbsql.transaction_finality.v1 |
| lock_mode | grammar_production | transaction | yes | rs.sbsql.transaction_finality.v1 |
| psql_execute_block | grammar_production | transaction | yes | rs.sbsql.transaction_finality.v1 |
| release_savepoint_stmt | grammar_production | transaction | yes | rs.sbsql.transaction_finality.v1 |
| savepoint | canonical_surface | transaction | yes | rs.sbsql.transaction_finality.v1 |
| savepoint_name | grammar_production | transaction | yes | rs.sbsql.transaction_finality.v1 |
| execute_block | canonical_surface | transaction | yes | rs.sbsql.transaction_finality.v1 |
| transaction_ref | grammar_production | transaction | yes | rs.sbsql.transaction_finality.v1 |

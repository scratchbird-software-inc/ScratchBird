# SBsql Language Reference Manual

This directory contains the draft SBsql Language Reference Manual. It is organized as section files rather than one large manuscript so editors can revise, test, and promote sections independently.

## Directory Map

| Directory | Purpose |
| --- | --- |
| core_paradigms | Architecture and authority rules needed to read the language reference correctly. |
| data_types | Descriptor, domain, conversion, collation, temporal, vector, document, and protected-value mechanics. |
| syntax_reference | Statement syntax, object lifecycles, DML, query clauses, procedural SQL, operational commands, refusal vectors, and per-production EBNF files. |
| functional_reference | Built-in functions, operators, aggregates, windows, and special forms grouped by package namespace. |
| catalog_reference | System catalog tables and schema-tree metadata surfaces. |

## Reading Model

SBsql text is parser input. ScratchBird execution authority is SBLR, UUID catalog identity, descriptors, MGA transaction state, and materialized security policy. Every section in this manual follows that boundary.

## Key Entry Points

| Topic | File |
| --- | --- |
| Syntax reference index | [syntax_reference/README.md](syntax_reference/README.md) |
| SBsql language profiles | [core_paradigms/sbsql_language_profiles.md](core_paradigms/sbsql_language_profiles.md) |
| Schema tree and name resolution | [syntax_reference/schema_tree_and_name_resolution.md](syntax_reference/schema_tree_and_name_resolution.md) |
| Procedural SQL overview | [syntax_reference/procedural_sql.md](syntax_reference/procedural_sql.md) |
| Procedural blocks and declarations | [syntax_reference/procedural_sql_blocks.md](syntax_reference/procedural_sql_blocks.md) |
| Procedural control flow | [syntax_reference/procedural_sql_control_flow.md](syntax_reference/procedural_sql_control_flow.md) |
| Procedural cursors | [syntax_reference/procedural_sql_cursors.md](syntax_reference/procedural_sql_cursors.md) |
| Procedural exceptions and diagnostics | [syntax_reference/procedural_sql_exceptions.md](syntax_reference/procedural_sql_exceptions.md) |
| Procedural triggers and event capture | [syntax_reference/procedural_sql_triggers_and_events.md](syntax_reference/procedural_sql_triggers_and_events.md) |

## Generation State

Progress and recovery data is kept in `.generation_state.json` in this directory. The state file is intentionally local to the generated manual tree.

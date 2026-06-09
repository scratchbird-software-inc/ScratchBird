# ScratchBird Getting Started Guide

This directory contains the draft ScratchBird Getting Started Guide for end users, evaluators, and operators who need a high-level understanding before reading the SBsql language reference or building the source tree.

This guide is intentionally cautious. It describes the architecture and intended use of the public source tree without promising production readiness, compatibility completeness, platform support, performance, security certification, or suitability for any particular workload. Always verify the current release notes, build configuration, test results, and component status before treating a feature as available.

## Directory Map

| Directory | Purpose |
| --- | --- |
| core_concepts | Plain-language explanations of databases, Convergent Data Engines, and the ScratchBird architecture. |
| operating_modes | How ScratchBird is intended to run as an embedded engine, IPC server, standalone server, or managed group deployment. |
| architecture | End-user architecture topics: parser separation, recursive schema, SBsql/SBLR, Git-oriented workflows, identity, and recovery. |
| using_scratchbird | First tasks: creating a database, connecting with SBsql, understanding schemas and donor compatibility. |
| administration | Deployment choice, configuration basics, diagnostics, support bundles, backup, restore, and data movement. |
| reference | Glossary and document map. |

## Reading Model

Start with [core_concepts/what_is_a_database.md](core_concepts/what_is_a_database.md) if you are new to database systems. Start with [operating_modes/choosing_a_mode_summary.md](operating_modes/choosing_a_mode_summary.md) if you already know databases and need to decide how ScratchBird fits into an application.

ScratchBird uses several branded component names:

| Name | Role In This Guide |
| --- | --- |
| SBcore | The embedded engine library. |
| SBsrv | The local IPC server. |
| SBgate | The listener and parser-facing network entry point. |
| SBmgr | The single-node manager front door. |
| SBsql | The native ScratchBird SQL language and command-line interface. |
| SBParser | The native SBsql parser package. |
| Donor parser | A parser package that accepts a donor database protocol or dialect and lowers it to ScratchBird execution requests. |

## First Reading Path

1. [What Is A Database?](core_concepts/what_is_a_database.md)
2. [What Is A Convergent Data Engine?](core_concepts/what_is_a_convergent_data_engine.md)
3. [How ScratchBird Implements A CDE](core_concepts/how_scratchbird_implements_a_cde.md)
4. [Choosing A Mode Summary](operating_modes/choosing_a_mode_summary.md)
5. [First Database](using_scratchbird/first_database.md)
6. [First SBsql Session](using_scratchbird/first_sbsql_session.md)

## Related Manuals

| Topic | Manual |
| --- | --- |
| SBsql grammar, syntax, functions, operators, catalog tables, procedural SQL, and EBNF | [../Language_Reference/README.md](../Language_Reference/README.md) |
| Installation, configuration, service lifecycle, diagnostics, backup, restore, and release validation | [../Operations_Administration/README.md](../Operations_Administration/README.md) |
| Schema tree and name resolution details | [../Language_Reference/syntax_reference/schema_tree_and_name_resolution.md](../Language_Reference/syntax_reference/schema_tree_and_name_resolution.md) |
| Operator precedence and result types | [../Language_Reference/syntax_reference/operators.md](../Language_Reference/syntax_reference/operators.md) |
| Procedural SQL | [../Language_Reference/syntax_reference/procedural_sql.md](../Language_Reference/syntax_reference/procedural_sql.md) |

## Draft Status

This is a draft overview. It should be read as orientation material, not as a support contract, compatibility statement, or production deployment recommendation.

# Document Map

## Purpose

This document map helps readers move through the draft ScratchBird documentation without having to know the directory layout first. It is organized by reading goal: learn the concepts, choose an operating mode, run a first session, understand architecture, administer a deployment, or continue into the SBsql Language Reference.

This map is a navigation aid. It is not a release checklist, support statement, compatibility guarantee, or production-readiness claim.

## Start Here

| Reader Goal | Start With | Then Read |
| --- | --- | --- |
| I am new to database systems. | [What Is A Database?](../core_concepts/what_is_a_database.md) | [What Is A Convergent Data Engine?](../core_concepts/what_is_a_convergent_data_engine.md) |
| I know databases and want the ScratchBird model. | [How ScratchBird Implements A CDE](../core_concepts/how_scratchbird_implements_a_cde.md) | [Engine Parser Boundary](../architecture/engine_parser_boundary.md) |
| I need to choose how to run it. | [Choosing A Mode Summary](../operating_modes/choosing_a_mode_summary.md) | The operating-mode page for the mode you intend to test. |
| I want to create a first database. | [First Database](../using_scratchbird/first_database.md) | [First SBsql Session](../using_scratchbird/first_sbsql_session.md) |
| I am trying to understand names and schemas. | [Schemas, Objects, And Names](../using_scratchbird/schemas_objects_and_names.md) | [Recursive Schema Tree](../architecture/recursive_schema_tree.md) |
| I need detailed SBsql syntax. | [Language Reference](../../Language_Reference/README.md) | The specific syntax, datatype, function, or catalog page for the command you are using. |

## Recommended First Reading Path

Read these pages in order if you are evaluating ScratchBird for the first time.

1. [What Is A Database?](../core_concepts/what_is_a_database.md)
2. [What Is A Convergent Data Engine?](../core_concepts/what_is_a_convergent_data_engine.md)
3. [How ScratchBird Implements A CDE](../core_concepts/how_scratchbird_implements_a_cde.md)
4. [Choosing A Mode Summary](../operating_modes/choosing_a_mode_summary.md)
5. [First Database](../using_scratchbird/first_database.md)
6. [First SBsql Session](../using_scratchbird/first_sbsql_session.md)
7. [Schemas, Objects, And Names](../using_scratchbird/schemas_objects_and_names.md)
8. [Configuration Basics](../administration/configuration_basics.md)
9. [Diagnostics And Support Bundles](../administration/diagnostics_and_support_bundles.md)

## Core Concepts

| Page | Use It For |
| --- | --- |
| [What Is A Database?](../core_concepts/what_is_a_database.md) | Plain-language explanation of durable data, metadata, transactions, identity, and why a database is more than a file. |
| [What Is A Convergent Data Engine?](../core_concepts/what_is_a_convergent_data_engine.md) | Explanation of the CDE category and the difference between shared engine authority and one universal syntax. |
| [How ScratchBird Implements A CDE](../core_concepts/how_scratchbird_implements_a_cde.md) | ScratchBird-specific overview of SBcore, parsers, SBLR, recursive schema, sandboxing, resources, operating modes, and refusal behavior. |

## Operating Modes

| Page | Use It For |
| --- | --- |
| [Choosing A Mode Summary](../operating_modes/choosing_a_mode_summary.md) | Compare embedded, single-node IPC, standalone server, and managed group deployment at a high level. |
| [Embedded Engine](../operating_modes/embedded_engine.md) | Understand direct SBcore use inside an application process. |
| [Single-Node IPC Server](../operating_modes/single_node_ipc_server.md) | Understand local multi-user access through SBsrv without a network listener. |
| [Standalone Server](../operating_modes/standalone_server.md) | Understand client access through SBgate, parser routing, and server process boundaries. |
| [Managed Group Deployment](../operating_modes/group_deployment.md) | Understand SBmgr as a managed single-node front door with shared identity or policy integration. |

## Architecture Topics

| Page | Use It For |
| --- | --- |
| [Engine Parser Boundary](../architecture/engine_parser_boundary.md) | Understand what parsers do, what SBcore owns, and why raw SQL text is not durable authority. |
| [Recursive Schema Tree](../architecture/recursive_schema_tree.md) | Understand schema branches, visible roots, workareas, catalog projections, and name resolution. |
| [SBsql And SBLR](../architecture/sbsql_and_sblr.md) | Understand the native language surface and the bound request representation submitted to engine authority. |
| [Git-Oriented Workflows](../architecture/git_support.md) | Understand how Git-oriented lifecycle concepts fit around database authority without replacing transactions or recovery. |
| [Identity, Authentication, And Authorization](../architecture/identity_authentication_and_authorization.md) | Understand users, principals, groups, grants, policy checks, and sandboxed access at a high level. |
| [Storage, Transactions, And Recovery](../architecture/storage_transactions_and_recovery.md) | Understand the role of storage, transaction visibility, recovery, and refusal when a database state is uncertain. |

## Using ScratchBird

| Page | Use It For |
| --- | --- |
| [First Database](../using_scratchbird/first_database.md) | Create or open a disposable first database, run a smoke workload, test one controlled refusal, and verify reopen behavior. |
| [First SBsql Session](../using_scratchbird/first_sbsql_session.md) | Run a first native SBsql session with schema creation, table creation, multi-row insert, query, commit, error handling, and cleanup. |
| [Schemas, Objects, And Names](../using_scratchbird/schemas_objects_and_names.md) | Understand current schema, home schema, qualified names, durable UUID identity, object lifecycle, comments, and compatibility workareas. |
| [Donor Database Compatibility](../using_scratchbird/donor_database_compatibility.md) | Understand standalone parser packages, compatibility scope, sandboxing, logical streams, denied physical or low-level actions, and refusal behavior. |

## Administration

| Page | Use It For |
| --- | --- |
| [Choosing A Deployment Mode](../administration/choosing_a_deployment_mode.md) | Translate application needs into an operating mode and deployment shape. |
| [Configuration Basics](../administration/configuration_basics.md) | Understand configuration, resource files, policy defaults, parser registration, and output-tree expectations. |
| [Diagnostics And Support Bundles](../administration/diagnostics_and_support_bundles.md) | Understand message vectors, logs, redaction, support bundles, and troubleshooting evidence. |
| [Backup, Restore, And Data Movement Overview](../administration/backup_restore_and_data_movement_overview.md) | Understand logical backup and restore streams, denied physical copy behavior, import/export, replication, ETL, and migration boundaries. |

## Language Reference Entry Points

Use the Language Reference when you need exact syntax, types, operators, functions, catalog views, or procedural SQL details.

| Area | Entry Point |
| --- | --- |
| Main language index | [Language Reference](../../Language_Reference/README.md) |
| Core engine concepts | [Intro And MGA](../../Language_Reference/core_paradigms/intro_and_mga.md) |
| Parser pipeline | [Parser To SBLR Pipeline](../../Language_Reference/core_paradigms/parser_to_sblr_pipeline.md) |
| Security and sandboxing | [Security And Sandboxing](../../Language_Reference/core_paradigms/security_and_sandboxing.md) |
| UUID catalog identity | [UUID Catalog Identity](../../Language_Reference/core_paradigms/uuid_catalog_identity.md) |
| Transactions and recovery | [Transactions And Recovery](../../Language_Reference/core_paradigms/transactions_and_recovery.md) |
| Type system | [Type System Overview](../../Language_Reference/data_types/type_system_overview.md) |
| Numeric types | [Numeric Types](../../Language_Reference/data_types/numeric_types.md) |
| Text, collation, and character sets | [Text, Collation, And Charset](../../Language_Reference/data_types/text_collation_and_charset.md) |
| Temporal types | [Temporal Types](../../Language_Reference/data_types/temporal_types.md) |
| Documents, graph, vector, and multimodel values | [Document, Graph, Vector, And Multimodel Types](../../Language_Reference/data_types/document_graph_vector_and_multimodel_types.md) |
| Operators | [Operators](../../Language_Reference/syntax_reference/operators.md) |
| Operator precedence and result types | [Operator Type Result Matrix](../../Language_Reference/syntax_reference/operator_type_result_matrix.md) |
| Procedural SQL overview | [Procedural SQL](../../Language_Reference/syntax_reference/procedural_sql.md) |
| Procedural blocks | [Procedural SQL Blocks](../../Language_Reference/syntax_reference/procedural_sql_blocks.md) |
| Procedural control flow | [Procedural SQL Control Flow](../../Language_Reference/syntax_reference/procedural_sql_control_flow.md) |
| Procedural cursors | [Procedural SQL Cursors](../../Language_Reference/syntax_reference/procedural_sql_cursors.md) |
| Procedural exceptions | [Procedural SQL Exceptions](../../Language_Reference/syntax_reference/procedural_sql_exceptions.md) |
| Triggers and events | [Procedural SQL Triggers And Events](../../Language_Reference/syntax_reference/procedural_sql_triggers_and_events.md) |
| Functional reference index | [Functional Reference](../../Language_Reference/functional_reference/index.md) |
| Catalog reference index | [Catalog Reference](../../Language_Reference/catalog_reference/index.md) |

## Syntax Reference By Task

| Task | Pages |
| --- | --- |
| Create or manage databases | [Database](../../Language_Reference/syntax_reference/database.md), [Filespace](../../Language_Reference/syntax_reference/filespace.md) |
| Create or manage schemas | [Schema](../../Language_Reference/syntax_reference/schema.md), [Schema Tree And Name Resolution](../../Language_Reference/syntax_reference/schema_tree_and_name_resolution.md) |
| Create or manage tables | [Table](../../Language_Reference/syntax_reference/table.md), [Domain](../../Language_Reference/syntax_reference/domain.md), [Type Descriptor](../../Language_Reference/syntax_reference/type_descriptor.md) |
| Create or manage views | [View](../../Language_Reference/syntax_reference/view.md), [Projection](../../Language_Reference/syntax_reference/projection.md) |
| Create or manage indexes | [Index](../../Language_Reference/syntax_reference/index.md) |
| Create or manage routines | [Function](../../Language_Reference/syntax_reference/function.md), [Procedure](../../Language_Reference/syntax_reference/procedure.md), [Trigger](../../Language_Reference/syntax_reference/trigger.md) |
| Create or manage sequences | [Sequence](../../Language_Reference/syntax_reference/sequence.md) |
| Query data | [Select](../../Language_Reference/syntax_reference/select.md), [From](../../Language_Reference/syntax_reference/from.md), [Where](../../Language_Reference/syntax_reference/where.md), [Group By And Having](../../Language_Reference/syntax_reference/group_by_and_having.md), [Order By Limit Offset](../../Language_Reference/syntax_reference/order_by_limit_offset.md), [Window](../../Language_Reference/syntax_reference/window.md), [With](../../Language_Reference/syntax_reference/with.md) |
| Change data | [Insert](../../Language_Reference/syntax_reference/insert.md), [Update](../../Language_Reference/syntax_reference/update.md), [Delete](../../Language_Reference/syntax_reference/delete.md), [Merge And Upsert](../../Language_Reference/syntax_reference/merge_and_upsert.md), [Copy](../../Language_Reference/syntax_reference/copy.md) |
| Control transactions | [Transaction Control](../../Language_Reference/syntax_reference/transaction_control.md) |
| Manage security | [Security And Privilege Statements](../../Language_Reference/syntax_reference/security_and_privilege_statements.md), [Policy, Mask, And RLS](../../Language_Reference/syntax_reference/policy_mask_and_rls.md) |
| Move data or migrate | [Backup, Restore, Replication, Migration](../../Language_Reference/syntax_reference/backup_restore_replication_migration.md) |
| Work with multimodel statements | [Multimodel Statements](../../Language_Reference/syntax_reference/multimodel_statements.md) |
| Understand diagnostics | [Refusal Vectors](../../Language_Reference/syntax_reference/refusal_vectors.md), [Management And Operations](../../Language_Reference/syntax_reference/management_and_operations.md) |
| Work with agents | [Agent](../../Language_Reference/syntax_reference/agent.md) |
| Understand scripts and identifiers | [Script Tokens And Identifiers](../../Language_Reference/syntax_reference/script_tokens_and_identifiers.md) |

## Functional Reference By Namespace

| Namespace | Page |
| --- | --- |
| Core functions | [sb_core](../../Language_Reference/functional_reference/sb_core.md) |
| Cryptographic and protected-material helpers | [sb_crypto](../../Language_Reference/functional_reference/sb_crypto.md) |
| Cursor helpers | [sb_cursor](../../Language_Reference/functional_reference/sb_cursor.md) |
| Diagnostics | [sb_diagnostic](../../Language_Reference/functional_reference/sb_diagnostic.md) |
| JSON | [sb_json](../../Language_Reference/functional_reference/sb_json.md) |
| Large objects | [sb_lob](../../Language_Reference/functional_reference/sb_lob.md) |
| Operator helpers | [sb_operator](../../Language_Reference/functional_reference/sb_operator.md) |
| Range values | [sb_range](../../Language_Reference/functional_reference/sb_range.md) |
| Regular expressions | [sb_regex](../../Language_Reference/functional_reference/sb_regex.md) |
| Row sets | [sb_rowset](../../Language_Reference/functional_reference/sb_rowset.md) |
| Spatial values | [sb_spatial](../../Language_Reference/functional_reference/sb_spatial.md) |
| Temporal values | [sb_temporal](../../Language_Reference/functional_reference/sb_temporal.md) |
| Time-series values | [sb_timeseries](../../Language_Reference/functional_reference/sb_timeseries.md) |
| UUID values | [sb_uuid](../../Language_Reference/functional_reference/sb_uuid.md) |
| Vector values | [sb_vector](../../Language_Reference/functional_reference/sb_vector.md) |
| XML values | [sb_xml](../../Language_Reference/functional_reference/sb_xml.md) |

## If You Are Looking For A Term

Use [Glossary](glossary.md) for short definitions of component names, authority concepts, session concepts, data movement terms, diagnostics, and parser terminology.

## Draft Status

These files are draft end-user documentation. Verify every operational command against the current build output and current Language Reference before relying on it for real data.

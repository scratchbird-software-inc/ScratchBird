# Neo4j Compatibility Parser Status

<!-- AUTO-GENERATED: compatibility parser status. Regenerate with
python3 project/tests/reference_regression/generate_compatibility_parser_docs.py --repo-root . --write
-->

Parser family: `neo4j`

Reference profile: `2026.04.0`

Release batch: `nosql`

Retained pre-hold beta evidence status: `beta_gate_passed_mapped_or_explicit_refusal`

Declared public surfaces covered: `63`

Surface digest: `7d3b9cc0cd275f9ba51b3f0d60304f5ad02d0c524326797e44bd06e7f55f4796`

This page is generated from the public compatibility parser remap matrix. Its status and support wording records the last verified pre-hold SBLR baseline; it is historical evidence and is not a claim of executable conformance to the in-progress SBLR contract. Every row below is a declared beta parser surface and states whether it was supported through ScratchBird SBLR/parser-support routing, routed to a cluster/provider boundary, documented as presentation-only behavior, or explicitly refused with a deterministic diagnostic.

The ScratchBird engine remains SBLR/UUID-only. Compatibility SQL is parsed outside the engine, and accepted work is still revalidated by ScratchBird authority before execution.

## Summary

| Runtime disposition | Count | Meaning |
| --- | ---: | --- |
| `admitted_normalized_cluster_sblr_provider_boundary` | 4 | Routed to cluster provider boundary |
| `admitted_sblr_or_parser_support_route` | 48 | Supported through ScratchBird SBLR or parser-support route |
| `documentation_evidence_only` | 8 | Documented compatibility behavior |
| `exact_fail_closed_refusal` | 3 | Explicit fail-closed refusal |

| Classification | Count |
| --- | ---: |
| `ARCHITECTURE_REFUSAL` | 3 |
| `DOCUMENTATION_ONLY` | 8 |
| `IMPLEMENT_NONCLUSTER` | 17 |
| `NORMALIZE_CLUSTER` | 4 |
| `PARSER_REMAP_ONLY` | 31 |

## Surface Status

| Row | Functionality | Implementation status | Runtime disposition | Route or SBLR | Diagnostic/refusal policy | Proof status |
| --- | --- | --- | --- | --- | --- | --- |
| `FPR-P3-DECLARED-01178` | `neo4j.admin.show_servers` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `sblr.cluster.report.v3:cluster.admin.inspect_status` | `NEO4J.AUTHORITY.CLUSTER_CONTROL_RESERVED` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-01179` | `neo4j.admin.enable_server` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `sblr.cluster.control.v3:cluster.job.start_controlled` | `NEO4J.AUTHORITY.CLUSTER_CONTROL_RESERVED` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-01180` | `neo4j.admin.reallocate_databases` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `sblr.cluster.control.v3:cluster.job.start_controlled` | `NEO4J.AUTHORITY.CLUSTER_CONTROL_RESERVED` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-01181` | `neo4j.query.match` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_query_match.f36bcf4aa0` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01182` | `neo4j.query.return` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_query_return.b986b43fb1` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01183` | `neo4j.schema.constraint.create` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_schema_constraint_create.d90487bfcb` | `NEO4J.EMULATION.SCHEMA_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01184` | `neo4j.schema.constraint.drop` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_schema_constraint_drop.a5a89c3b9e` | `NEO4J.EMULATION.SCHEMA_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01185` | `neo4j.schema.index.create` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_schema_index_create.c2a7c7d8a4` | `NEO4J.EMULATION.INDEX_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01186` | `neo4j.schema.index.drop` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_schema_index_drop.3ad66b7a6c` | `NEO4J.EMULATION.INDEX_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01187` | `neo4j.security.create_user` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_security_create_user.a8507f080a` | `NEO4J.EMULATION.SECURITY_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01188` | `neo4j.security.create_role` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_security_create_role.86b57a7150` | `NEO4J.EMULATION.SECURITY_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01189` | `neo4j.security.grant` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_security_grant.3050883085` | `NEO4J.EMULATION.SECURITY_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01190` | `neo4j.security.revoke` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_security_revoke.f39e34b70a` | `NEO4J.EMULATION.SECURITY_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01191` | `neo4j.procedure.call` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_procedure_call.f01abb8a42` | `NEO4J.EMULATION.PROCEDURE_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01192` | `neo4j.client_file.load_csv` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.storage_import_export_backup_restore.neo4j.neo4j_client_file_load_csv.3b76bce3c5` | `NEO4J.EMULATION.CLIENT_FILE_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01193` | `neo4j.graph.merge` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_graph_merge.acb8b9d4e3` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01194` | `neo4j.graph.create` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_graph_create.d9ed652275` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01195` | `neo4j.graph.detach_delete` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_graph_detach_delete.05ea050f43` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01196` | `neo4j.graph.delete` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_graph_delete.86053feda2` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01197` | `neo4j.graph.set` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_graph_set.0fa874d8fa` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01198` | `neo4j.graph.remove` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_graph_remove.b2e97f9b2f` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01199` | `neo4j.query.unwind` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_query_unwind.033e13e86c` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01200` | `neo4j.catalog.show` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_catalog_show.6045a13b09` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01201` | `neo4j.session.use` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_session_use.37f7a09a31` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01202` | `neo4j.transaction.begin` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_transaction_begin.f9012f109d` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01203` | `neo4j.transaction.commit` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_transaction_commit.e3c50b61b4` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01204` | `neo4j.transaction.rollback` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.neo4j_transaction_rollback.519dc5fe48` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01205` | `datatype_surface:graph` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01206` | `datatype_surface:scalar` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01207` | `datatype_surface:temporal` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01208` | `datatype_surface:spatial` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.datatype_surface_spatial.43c7840957` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01209` | `datatype_surface:collection` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.datatype_surface_collection.13bc3f57e5` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01210` | `datatype_surface:null` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01211` | `builtin_function_surface:predicate` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.builtin_function_surface_predicate.cda51320c8` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01212` | `builtin_function_surface:aggregate` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.builtin_function_surface_aggregate.b918de1252` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01213` | `builtin_function_surface:graph` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.builtin_function_surface_graph.9fbf53cbf2` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01214` | `builtin_function_surface:string` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.builtin_function_surface_string.a7c14d1af5` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01215` | `builtin_function_surface:temporal` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.builtin_function_surface_temporal.95bdfd3c06` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01216` | `builtin_function_surface:spatial` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.builtin_function_surface_spatial.4ae443a60e` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01217` | `builtin_function_surface:list` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.builtin_function_surface_list.fd0ac4a230` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01218` | `builtin_function_surface:procedure` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.builtin_function_surface_procedure.9ef30201e5` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01219` | `catalog_overlay_surface:system_graph` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.catalog_overlay_surface_system_graph.17cae78eb9` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01220` | `catalog_overlay_surface:labels` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.catalog_overlay_surface_labels.8d52184928` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01221` | `catalog_overlay_surface:relationship_types` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.catalog_overlay_surface_relationship_types.14bc1fb3ec` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01222` | `catalog_overlay_surface:property_keys` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.catalog_overlay_surface_property_keys.caabe67eb8` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01223` | `catalog_overlay_surface:indexes` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.catalog_overlay_surface_indexes.34393d97f8` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01224` | `catalog_overlay_surface:constraints` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.catalog_overlay_surface_constraints.f6c058853a` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01225` | `catalog_overlay_surface:users` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.catalog_overlay_surface_users.34412174a0` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01226` | `catalog_overlay_surface:roles` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.catalog_overlay_surface_roles.4b5df6e954` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01227` | `catalog_overlay_surface:procedures` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.catalog_overlay_surface_procedures.886eddbfcf` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01228` | `diagnostic_surface:parse` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.diagnostic_surface_parse.f68ee9c64f` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01229` | `diagnostic_surface:policy` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.neo4j.diagnostic_surface_policy.be0689c21b` | `exact_architecture_refusal;no_provider_call;no_parser_or_compatibility_execution` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-01230` | `diagnostic_surface:udr` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.diagnostic_surface_udr.f62eabf1e7` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01231` | `diagnostic_surface:catalog` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.diagnostic_surface_catalog.6c3e63789f` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01232` | `diagnostic_surface:session` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.diagnostic_surface_session.aa8e70966c` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01233` | `diagnostic_surface:transaction` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.file_connector_and_external_resource_acc.neo4j.diagnostic_surface_transaction.aaeb96588e` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01234` | `diagnostic_surface:file_effects` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01235` | `diagnostic_surface:compatibility_execution` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01236` | `diagnostic_surface:mga` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01237` | `diagnostic_surface:support_bundle` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01238` | `source_marker:unsupported` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.neo4j.source_marker_unsupported.414275a64e` | `exact_architecture_refusal;no_provider_call;no_parser_or_compatibility_execution` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-02056` | `udr_management_operation_set` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `sblr.replication.consumer.v3:cluster.replication.consume_cluster_event` | `compile_flag_disabled_returns_functionality_unsupported;public_stub_enabled_returns_functionality_unlicensed;private_provider_required_for_execution` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-02057` | `udr_diagnostic_vector_set` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.neo4j.udr_diagnostic_vector_set.ba08d0df9c` | `exact_architecture_refusal;no_provider_call;no_parser_or_compatibility_execution` | `p3_architecture_refusal_preserved` |

## Source Anchors

These anchors identify the source-backed declaration used to generate each row. They are included so a developer or auditor can trace the public status back to the implementation declaration without using private notes.

| Row | Source anchor | Parser package |
| --- | --- | --- |
| `FPR-P3-DECLARED-01178` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.admin.show_servers` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01179` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.admin.enable_server` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01180` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.admin.reallocate_databases` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01181` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.query.match` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01182` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.query.return` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01183` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.schema.constraint.create` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01184` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.schema.constraint.drop` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01185` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.schema.index.create` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01186` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.schema.index.drop` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01187` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.security.create_user` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01188` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.security.create_role` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01189` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.security.grant` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01190` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.security.revoke` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01191` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.procedure.call` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01192` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.client_file.load_csv` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01193` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.graph.merge` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01194` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.graph.create` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01195` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.graph.detach_delete` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01196` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.graph.delete` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01197` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.graph.set` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01198` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.graph.remove` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01199` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.query.unwind` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01200` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.catalog.show` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01201` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.session.use` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01202` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.transaction.begin` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01203` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.transaction.commit` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01204` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kPatterns:neo4j.transaction.rollback` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01205` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kDatatypeSurfaces:graph` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01206` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kDatatypeSurfaces:scalar` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01207` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kDatatypeSurfaces:temporal` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01208` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kDatatypeSurfaces:spatial` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01209` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kDatatypeSurfaces:collection` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01210` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kDatatypeSurfaces:null` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01211` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kBuiltinSurfaces:predicate` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01212` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kBuiltinSurfaces:aggregate` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01213` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kBuiltinSurfaces:graph` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01214` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kBuiltinSurfaces:string` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01215` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kBuiltinSurfaces:temporal` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01216` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kBuiltinSurfaces:spatial` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01217` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kBuiltinSurfaces:list` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01218` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kBuiltinSurfaces:procedure` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01219` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kCatalogSurfaces:system_graph` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01220` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kCatalogSurfaces:labels` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01221` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kCatalogSurfaces:relationship_types` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01222` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kCatalogSurfaces:property_keys` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01223` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kCatalogSurfaces:indexes` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01224` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kCatalogSurfaces:constraints` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01225` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kCatalogSurfaces:users` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01226` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kCatalogSurfaces:roles` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01227` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kCatalogSurfaces:procedures` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01228` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kDiagnosticSurfaces:parse` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01229` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kDiagnosticSurfaces:policy` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01230` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kDiagnosticSurfaces:udr` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01231` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kDiagnosticSurfaces:catalog` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01232` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kDiagnosticSurfaces:session` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01233` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kDiagnosticSurfaces:transaction` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01234` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kDiagnosticSurfaces:file_effects` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01235` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kDiagnosticSurfaces:compatibility_execution` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01236` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kDiagnosticSurfaces:mga` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01237` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#kDiagnosticSurfaces:support_bundle` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-01238` | `project/src/parsers/compatibility/neo4j/neo4j_dialect.cpp#marker:unsupported` | `project/src/parsers/compatibility/neo4j` |
| `FPR-P3-DECLARED-02056` | `project/src/udr/sbu_neo4j_parser_support/sbu_neo4j_parser_support.cpp#kManagementOperations` | `project/src/udr/sbu_neo4j_parser_support` |
| `FPR-P3-DECLARED-02057` | `project/src/udr/sbu_neo4j_parser_support/sbu_neo4j_parser_support.cpp#diagnostic_vectors` | `project/src/udr/sbu_neo4j_parser_support` |

# Milvus Compatibility Parser Status

<!-- AUTO-GENERATED: compatibility parser status. Regenerate with
python3 project/tests/reference_regression/generate_compatibility_parser_docs.py --repo-root . --write
-->

Parser family: `milvus`

Reference profile: `2.6.5`

Release batch: `analytic`

Retained pre-hold beta evidence status: `beta_gate_passed_mapped_or_explicit_refusal`

Declared public surfaces covered: `58`

Surface digest: `51fe71d78db9818a88e03b5bb8261e20d37449cab417000cd67070952ce6ce5a`

This page is generated from the public compatibility parser remap matrix. Its status and support wording records the last verified pre-hold SBLR baseline; it is historical evidence and is not a claim of executable conformance to the in-progress SBLR contract. Every row below is a declared beta parser surface and states whether it was supported through ScratchBird SBLR/parser-support routing, routed to a cluster/provider boundary, documented as presentation-only behavior, or explicitly refused with a deterministic diagnostic.

The ScratchBird engine remains SBLR/UUID-only. Compatibility SQL is parsed outside the engine, and accepted work is still revalidated by ScratchBird authority before execution.

## Summary

| Runtime disposition | Count | Meaning |
| --- | ---: | --- |
| `admitted_normalized_cluster_sblr_provider_boundary` | 4 | Routed to cluster provider boundary |
| `admitted_sblr_or_parser_support_route` | 46 | Supported through ScratchBird SBLR or parser-support route |
| `documentation_evidence_only` | 5 | Documented compatibility behavior |
| `exact_fail_closed_refusal` | 3 | Explicit fail-closed refusal |

| Classification | Count |
| --- | ---: |
| `ARCHITECTURE_REFUSAL` | 3 |
| `DOCUMENTATION_ONLY` | 5 |
| `IMPLEMENT_NONCLUSTER` | 20 |
| `NORMALIZE_CLUSTER` | 4 |
| `PARSER_REMAP_ONLY` | 26 |

## Surface Status

| Row | Functionality | Implementation status | Runtime disposition | Route or SBLR | Diagnostic/refusal policy | Proof status |
| --- | --- | --- | --- | --- | --- | --- |
| `FPR-P3-DECLARED-00952` | `milvus.admin.transfer_replica` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `required_new:sblr.cluster.transaction.v1:cluster.tx.begin_distributed` | `MILVUS.AUTHORITY.CLUSTER_CONTROL_RESERVED` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-00953` | `milvus.admin.load_balance` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `sblr.cluster.control.v3:cluster.job.start_controlled` | `MILVUS.AUTHORITY.CLUSTER_CONTROL_RESERVED` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-00954` | `milvus.admin.show_replicas` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `required_new:sblr.cluster.transaction.v1:cluster.tx.begin_distributed` | `MILVUS.AUTHORITY.CLUSTER_CONTROL_RESERVED` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-00955` | `milvus.collection.create` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.milvus_collection_create.8b65059492` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00956` | `milvus.collection.drop` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.milvus_collection_drop.c631258ffd` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00957` | `milvus.collection.has` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.milvus_collection_has.23cba8aec0` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00958` | `milvus.collection.describe` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.milvus_collection_describe.7b2d4d6f10` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00959` | `milvus.partition.create` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.milvus_partition_create.9a819be1cf` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00960` | `milvus.partition.drop` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.milvus_partition_drop.99e4bed1bd` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00961` | `milvus.index.create` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.milvus_index_create.b023d44b67` | `MILVUS.EMULATION.INDEX_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00962` | `milvus.index.drop` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.milvus_index_drop.0b92153f16` | `MILVUS.EMULATION.INDEX_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00963` | `milvus.collection.load` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.storage_import_export_backup_restore.milvus.milvus_collection_load.551f52456a` | `MILVUS.EMULATION.LOAD_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00964` | `milvus.collection.release` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.milvus_collection_release.d7c779c41d` | `MILVUS.EMULATION.LOAD_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00965` | `milvus.dml.insert` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.milvus_dml_insert.a0713bb488` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00966` | `milvus.dml.upsert` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.milvus_dml_upsert.4f04a23abe` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00967` | `milvus.dml.delete` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.milvus_dml_delete.c6386f3872` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00968` | `milvus.query.hybrid_search` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.milvus_query_hybrid_search.ade3fe2334` | `MILVUS.EMULATION.HYBRID_SEARCH_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00969` | `milvus.query.search` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.milvus_query_search.8755e470b2` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00970` | `milvus.query.scalar` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.milvus_query_scalar.e9d1308aba` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00971` | `milvus.query.rerank` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.milvus_query_rerank.6afe360b07` | `MILVUS.EMULATION.RERANK_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00972` | `milvus.security.create_user` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.security_auth_policy_and_materialized_au.milvus.milvus_security_create_user.8d2891294d` | `MILVUS.EMULATION.SECURITY_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00973` | `milvus.security.create_role` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.security_auth_policy_and_materialized_au.milvus.milvus_security_create_role.a06e3f8a4e` | `MILVUS.EMULATION.SECURITY_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00974` | `milvus.security.grant` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.security_auth_policy_and_materialized_au.milvus.milvus_security_grant.d5a6665217` | `MILVUS.EMULATION.SECURITY_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00975` | `milvus.security.revoke` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.security_auth_policy_and_materialized_au.milvus.milvus_security_revoke.5517bb7b73` | `MILVUS.EMULATION.SECURITY_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00976` | `datatype_surface:scalar` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00977` | `datatype_surface:json` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.datatype_surface_json.03010c433b` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00978` | `datatype_surface:array` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.datatype_surface_array.6e4c22f477` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00979` | `datatype_surface:vector` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.datatype_surface_vector.01bb2f954c` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00980` | `datatype_surface:geometry` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.datatype_surface_geometry.3682a6c59b` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00981` | `datatype_surface:dynamic` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.milvus.datatype_surface_dynamic.47bb34d098` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00982` | `builtin_function_surface:metric` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.milvus.builtin_function_surface_metric.9c5ccbca46` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00983` | `builtin_function_surface:index` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.milvus.builtin_function_surface_index.34e61e1201` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00984` | `builtin_function_surface:filter` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.milvus.builtin_function_surface_filter.c69284098a` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00985` | `builtin_function_surface:search` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.milvus.builtin_function_surface_search.0621fd599a` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00986` | `builtin_function_surface:load` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.storage_import_export_backup_restore.milvus.builtin_function_surface_load.7e856ed242` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00987` | `builtin_function_surface:security` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.milvus.builtin_function_surface_security.2c42ec735c` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00988` | `catalog_overlay_surface:collections` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.milvus.catalog_overlay_surface_collections.f63c178790` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00989` | `catalog_overlay_surface:partitions` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.milvus.catalog_overlay_surface_partitions.f5480aeb9a` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00990` | `catalog_overlay_surface:indexes` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.milvus.catalog_overlay_surface_indexes.c98ccb0962` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00991` | `catalog_overlay_surface:segments` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.milvus.catalog_overlay_surface_segments.f50932498b` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00992` | `catalog_overlay_surface:load_states` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.storage_import_export_backup_restore.milvus.catalog_overlay_surface_load_states.1a4b8f018a` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00993` | `catalog_overlay_surface:users` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.milvus.catalog_overlay_surface_users.c75c681d3e` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00994` | `catalog_overlay_surface:roles` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.milvus.catalog_overlay_surface_roles.fac882988f` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00995` | `catalog_overlay_surface:privileges` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.milvus.catalog_overlay_surface_privileges.35c971344f` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00996` | `catalog_overlay_surface:aliases` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.milvus.catalog_overlay_surface_aliases.3595661aee` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00997` | `diagnostic_surface:parse` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.milvus.diagnostic_surface_parse.799cba365c` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00998` | `diagnostic_surface:policy` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.milvus.diagnostic_surface_policy.f2c3200840` | `exact_architecture_refusal;no_provider_call;no_parser_or_compatibility_execution` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-00999` | `diagnostic_surface:udr` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.milvus.diagnostic_surface_udr.38adf744cb` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01000` | `diagnostic_surface:catalog` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.milvus.diagnostic_surface_catalog.72240aa0bb` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01001` | `diagnostic_surface:session` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.milvus.diagnostic_surface_session.0296ee621b` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01002` | `diagnostic_surface:transaction` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.milvus.diagnostic_surface_transaction.29f2f832b0` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01003` | `diagnostic_surface:file_effects` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01004` | `diagnostic_surface:compatibility_execution` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01005` | `diagnostic_surface:mga` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01006` | `diagnostic_surface:support_bundle` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01007` | `source_marker:unsupported` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.milvus.source_marker_unsupported.19de41aef8` | `exact_architecture_refusal;no_provider_call;no_parser_or_compatibility_execution` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-02050` | `udr_management_operation_set` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `sblr.replication.consumer.v3:cluster.replication.consume_cluster_event` | `compile_flag_disabled_returns_functionality_unsupported;public_stub_enabled_returns_functionality_unlicensed;private_provider_required_for_execution` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-02051` | `udr_diagnostic_vector_set` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.milvus.udr_diagnostic_vector_set.ebaaa65e19` | `exact_architecture_refusal;no_provider_call;no_parser_or_compatibility_execution` | `p3_architecture_refusal_preserved` |

## Source Anchors

These anchors identify the source-backed declaration used to generate each row. They are included so a developer or auditor can trace the public status back to the implementation declaration without using private notes.

| Row | Source anchor | Parser package |
| --- | --- | --- |
| `FPR-P3-DECLARED-00952` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.admin.transfer_replica` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00953` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.admin.load_balance` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00954` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.admin.show_replicas` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00955` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.collection.create` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00956` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.collection.drop` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00957` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.collection.has` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00958` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.collection.describe` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00959` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.partition.create` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00960` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.partition.drop` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00961` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.index.create` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00962` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.index.drop` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00963` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.collection.load` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00964` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.collection.release` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00965` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.dml.insert` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00966` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.dml.upsert` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00967` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.dml.delete` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00968` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.query.hybrid_search` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00969` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.query.search` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00970` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.query.scalar` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00971` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.query.rerank` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00972` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.security.create_user` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00973` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.security.create_role` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00974` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.security.grant` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00975` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kPatterns:milvus.security.revoke` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00976` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kDatatypeSurfaces:scalar` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00977` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kDatatypeSurfaces:json` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00978` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kDatatypeSurfaces:array` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00979` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kDatatypeSurfaces:vector` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00980` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kDatatypeSurfaces:geometry` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00981` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kDatatypeSurfaces:dynamic` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00982` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kBuiltinSurfaces:metric` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00983` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kBuiltinSurfaces:index` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00984` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kBuiltinSurfaces:filter` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00985` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kBuiltinSurfaces:search` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00986` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kBuiltinSurfaces:load` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00987` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kBuiltinSurfaces:security` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00988` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kCatalogSurfaces:collections` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00989` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kCatalogSurfaces:partitions` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00990` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kCatalogSurfaces:indexes` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00991` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kCatalogSurfaces:segments` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00992` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kCatalogSurfaces:load_states` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00993` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kCatalogSurfaces:users` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00994` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kCatalogSurfaces:roles` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00995` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kCatalogSurfaces:privileges` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00996` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kCatalogSurfaces:aliases` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00997` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kDiagnosticSurfaces:parse` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00998` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kDiagnosticSurfaces:policy` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-00999` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kDiagnosticSurfaces:udr` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-01000` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kDiagnosticSurfaces:catalog` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-01001` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kDiagnosticSurfaces:session` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-01002` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kDiagnosticSurfaces:transaction` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-01003` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kDiagnosticSurfaces:file_effects` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-01004` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kDiagnosticSurfaces:compatibility_execution` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-01005` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kDiagnosticSurfaces:mga` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-01006` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#kDiagnosticSurfaces:support_bundle` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-01007` | `project/src/parsers/compatibility/milvus/milvus_dialect.cpp#marker:unsupported` | `project/src/parsers/compatibility/milvus` |
| `FPR-P3-DECLARED-02050` | `project/src/udr/sbu_milvus_parser_support/sbu_milvus_parser_support.cpp#kManagementOperations` | `project/src/udr/sbu_milvus_parser_support` |
| `FPR-P3-DECLARED-02051` | `project/src/udr/sbu_milvus_parser_support/sbu_milvus_parser_support.cpp#diagnostic_vectors` | `project/src/udr/sbu_milvus_parser_support` |

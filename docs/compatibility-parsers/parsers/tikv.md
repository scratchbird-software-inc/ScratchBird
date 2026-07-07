# TiKV Compatibility Parser Status

<!-- AUTO-GENERATED: compatibility parser status. Regenerate with
python3 project/tests/reference_regression/generate_compatibility_parser_docs.py --repo-root . --write
-->

Parser family: `tikv`

Reference profile: `8.5.6`

Release batch: `distributed`

Public beta status: `beta_gate_passed_mapped_or_explicit_refusal`

Declared public surfaces covered: `58`

Surface digest: `d4ce129727f065a29319235b4c56f380afe54bf1c01539906477e05ebe586ed5`

This page is generated from the public compatibility parser remap matrix. Every row below is a declared beta parser surface and states whether it is supported through ScratchBird SBLR/parser-support routing, routed to a cluster/provider boundary, documented as presentation-only behavior, or explicitly refused with a deterministic diagnostic.

The ScratchBird engine remains SBLR/UUID-only. Compatibility SQL is parsed outside the engine, and accepted work is still revalidated by ScratchBird authority before execution.

## Summary

| Runtime disposition | Count | Meaning |
| --- | ---: | --- |
| `admitted_normalized_cluster_sblr_provider_boundary` | 7 | Routed to cluster provider boundary |
| `admitted_sblr_or_parser_support_route` | 41 | Supported through ScratchBird SBLR or parser-support route |
| `documentation_evidence_only` | 7 | Documented compatibility behavior |
| `exact_fail_closed_refusal` | 3 | Explicit fail-closed refusal |

| Classification | Count |
| --- | ---: |
| `ARCHITECTURE_REFUSAL` | 3 |
| `DOCUMENTATION_ONLY` | 7 |
| `IMPLEMENT_NONCLUSTER` | 9 |
| `NORMALIZE_CLUSTER` | 7 |
| `PARSER_REMAP_ONLY` | 32 |

## Surface Status

| Row | Functionality | Implementation status | Runtime disposition | Route or SBLR | Diagnostic/refusal policy | Proof status |
| --- | --- | --- | --- | --- | --- | --- |
| `FPR-P3-DECLARED-01723` | `tikv.admin.split_region` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `sblr.cluster.control.v3:cluster.placement.place_object` | `TIKV.AUTHORITY.CLUSTER_CONTROL_RESERVED` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-01724` | `tikv.admin.merge_region` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `required_new:sblr.cluster.reconciliation.v1:cluster.reconcile.apply_merge_policy` | `TIKV.AUTHORITY.CLUSTER_CONTROL_RESERVED` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-01725` | `tikv.admin.transfer_leader` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `sblr.cluster.control.v3:cluster.node.set_role` | `TIKV.AUTHORITY.CLUSTER_CONTROL_RESERVED` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-01726` | `tikv.admin.change_peer` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `sblr.cluster.control.v3:cluster.job.start_controlled` | `TIKV.AUTHORITY.CLUSTER_CONTROL_RESERVED` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-01727` | `tikv.admin.pd_request` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `sblr.cluster.control.v3:cluster.job.start_controlled` | `TIKV.AUTHORITY.CLUSTER_CONTROL_RESERVED` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-01728` | `tikv.import_sst` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.storage_import_export_backup_restore.tikv.tikv_import_sst.8dce7bd398` | `TIKV.EMULATION.IMPORT_SST_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01729` | `tikv.raw.batch_get` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.tikv.tikv_raw_batch_get.2ead1049cd` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01730` | `tikv.raw.get` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.tikv.tikv_raw_get.eb002eee99` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01731` | `tikv.raw.put` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.tikv.tikv_raw_put.304e018d88` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01732` | `tikv.raw.delete` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.tikv.tikv_raw_delete.7ec65cc741` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01733` | `tikv.raw.scan` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.tikv.tikv_raw_scan.4a2fabe932` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01734` | `tikv.txn.prewrite` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.tikv.tikv_txn_prewrite.5abfc6bd9d` | `TIKV.EMULATION.TXN_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01735` | `tikv.txn.commit` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.tikv.tikv_txn_commit.c369c4178f` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01736` | `tikv.txn.rollback` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.tikv.tikv_txn_rollback.a3a624c80c` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01737` | `tikv.txn.get` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.tikv.tikv_txn_get.1e3fb5fd72` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01738` | `tikv.txn.scan` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.tikv.tikv_txn_scan.de2e03c4d9` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01739` | `tikv.coprocessor.request` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.tikv.tikv_coprocessor_request.847bf82861` | `TIKV.EMULATION.COPROCESSOR_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01740` | `tikv.catalog.region_info` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.tikv_catalog_region_info.f36c0f475c` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01741` | `tikv.catalog.store_info` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.tikv_catalog_store_info.c17da994f6` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01742` | `tikv.catalog.mvcc_info` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.tikv_catalog_mvcc_info.5dafc0ae92` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01743` | `tikv.catalog.lock_info` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.tikv_catalog_lock_info.6f0ec34a01` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01744` | `datatype_surface:key` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01745` | `datatype_surface:value` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01746` | `datatype_surface:timestamp` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01747` | `datatype_surface:ttl` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.tikv.datatype_surface_ttl.b739802519` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01748` | `datatype_surface:lock` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.tikv.datatype_surface_lock.10498e9707` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01749` | `datatype_surface:region` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.tikv.datatype_surface_region.85f3492e35` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01750` | `datatype_surface:coprocessor` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.tikv.datatype_surface_coprocessor.d5a8948b21` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01751` | `datatype_surface:sst` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.tikv.datatype_surface_sst.655efdfe34` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01752` | `builtin_function_surface:raw_kv` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.tikv.builtin_function_surface_raw_kv.1f4ea2960c` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01753` | `builtin_function_surface:txn_kv` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.tikv.builtin_function_surface_txn_kv.f2c70bfc80` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01754` | `builtin_function_surface:coprocessor` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.tikv.builtin_function_surface_coprocessor.8f7548d738` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01755` | `builtin_function_surface:import` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.storage_import_export_backup_restore.tikv.builtin_function_surface_import.f5efd20a3a` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01756` | `builtin_function_surface:catalog` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.builtin_function_surface_catalog.fd0f882ccc` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01757` | `builtin_function_surface:cluster` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `sblr.cluster.control.v3:cluster.job.start_controlled` | `compile_flag_disabled_returns_functionality_unsupported;public_stub_enabled_returns_functionality_unlicensed;private_provider_required_for_execution` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-01758` | `builtin_function_surface:transaction` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.tikv.builtin_function_surface_transaction.c8eec605f8` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01759` | `builtin_function_surface:security` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.tikv.builtin_function_surface_security.3fb590ebe6` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01760` | `catalog_overlay_surface:regions` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.catalog_overlay_surface_regions.6214d0962f` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01761` | `catalog_overlay_surface:stores` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.catalog_overlay_surface_stores.856496ab41` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01762` | `catalog_overlay_surface:locks` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.catalog_overlay_surface_locks.82a3706813` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01763` | `catalog_overlay_surface:mvcc` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.catalog_overlay_surface_mvcc.ec0990adf2` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01764` | `catalog_overlay_surface:raw_cf` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.catalog_overlay_surface_raw_cf.fc81da82f1` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01765` | `catalog_overlay_surface:scheduler` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.catalog_overlay_surface_scheduler.00b677043d` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01766` | `catalog_overlay_surface:coprocessor` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.catalog_overlay_surface_coprocessor.315dc7f481` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01767` | `catalog_overlay_surface:import_jobs` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.storage_import_export_backup_restore.tikv.catalog_overlay_surface_import_jobs.a192f92d44` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01768` | `diagnostic_surface:parse` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.diagnostic_surface_parse.cbf7c14000` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01769` | `diagnostic_surface:policy` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.tikv.diagnostic_surface_policy.e3a5e9ee43` | `exact_architecture_refusal;no_provider_call;no_parser_or_compatibility_execution` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-01770` | `diagnostic_surface:udr` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.diagnostic_surface_udr.a9981060f9` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01771` | `diagnostic_surface:catalog` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.diagnostic_surface_catalog.2bc6a96951` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01772` | `diagnostic_surface:session` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.diagnostic_surface_session.b7b73554ca` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01773` | `diagnostic_surface:transaction` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.tikv.diagnostic_surface_transaction.c59b7d7165` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-01774` | `diagnostic_surface:file_effects` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01775` | `diagnostic_surface:compatibility_execution` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01776` | `diagnostic_surface:mga` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01777` | `diagnostic_surface:support_bundle` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-01778` | `source_marker:unsupported` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.tikv.source_marker_unsupported.eb3c52578c` | `exact_architecture_refusal;no_provider_call;no_parser_or_compatibility_execution` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-02070` | `udr_management_operation_set` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `sblr.replication.consumer.v3:cluster.replication.consume_cluster_event` | `compile_flag_disabled_returns_functionality_unsupported;public_stub_enabled_returns_functionality_unlicensed;private_provider_required_for_execution` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-02071` | `udr_diagnostic_vector_set` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.tikv.udr_diagnostic_vector_set.8f7f68c06d` | `exact_architecture_refusal;no_provider_call;no_parser_or_compatibility_execution` | `p3_architecture_refusal_preserved` |

## Source Anchors

These anchors identify the source-backed declaration used to generate each row. They are included so a developer or auditor can trace the public status back to the implementation declaration without using private notes.

| Row | Source anchor | Parser package |
| --- | --- | --- |
| `FPR-P3-DECLARED-01723` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.admin.split_region` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01724` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.admin.merge_region` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01725` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.admin.transfer_leader` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01726` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.admin.change_peer` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01727` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.admin.pd_request` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01728` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.import_sst` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01729` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.raw.batch_get` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01730` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.raw.get` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01731` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.raw.put` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01732` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.raw.delete` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01733` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.raw.scan` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01734` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.txn.prewrite` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01735` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.txn.commit` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01736` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.txn.rollback` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01737` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.txn.get` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01738` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.txn.scan` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01739` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.coprocessor.request` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01740` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.catalog.region_info` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01741` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.catalog.store_info` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01742` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.catalog.mvcc_info` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01743` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kPatterns:tikv.catalog.lock_info` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01744` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDatatypeSurfaces:key` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01745` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDatatypeSurfaces:value` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01746` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDatatypeSurfaces:timestamp` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01747` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDatatypeSurfaces:ttl` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01748` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDatatypeSurfaces:lock` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01749` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDatatypeSurfaces:region` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01750` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDatatypeSurfaces:coprocessor` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01751` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDatatypeSurfaces:sst` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01752` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kBuiltinSurfaces:raw_kv` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01753` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kBuiltinSurfaces:txn_kv` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01754` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kBuiltinSurfaces:coprocessor` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01755` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kBuiltinSurfaces:import` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01756` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kBuiltinSurfaces:catalog` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01757` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kBuiltinSurfaces:cluster` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01758` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kBuiltinSurfaces:transaction` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01759` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kBuiltinSurfaces:security` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01760` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kCatalogSurfaces:regions` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01761` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kCatalogSurfaces:stores` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01762` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kCatalogSurfaces:locks` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01763` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kCatalogSurfaces:mvcc` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01764` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kCatalogSurfaces:raw_cf` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01765` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kCatalogSurfaces:scheduler` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01766` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kCatalogSurfaces:coprocessor` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01767` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kCatalogSurfaces:import_jobs` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01768` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDiagnosticSurfaces:parse` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01769` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDiagnosticSurfaces:policy` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01770` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDiagnosticSurfaces:udr` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01771` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDiagnosticSurfaces:catalog` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01772` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDiagnosticSurfaces:session` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01773` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDiagnosticSurfaces:transaction` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01774` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDiagnosticSurfaces:file_effects` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01775` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDiagnosticSurfaces:compatibility_execution` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01776` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDiagnosticSurfaces:mga` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01777` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#kDiagnosticSurfaces:support_bundle` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-01778` | `project/src/parsers/compatibility/tikv/tikv_dialect.cpp#marker:unsupported` | `project/src/parsers/compatibility/tikv` |
| `FPR-P3-DECLARED-02070` | `project/src/udr/sbu_tikv_parser_support/sbu_tikv_parser_support.cpp#kManagementOperations` | `project/src/udr/sbu_tikv_parser_support` |
| `FPR-P3-DECLARED-02071` | `project/src/udr/sbu_tikv_parser_support/sbu_tikv_parser_support.cpp#diagnostic_vectors` | `project/src/udr/sbu_tikv_parser_support` |

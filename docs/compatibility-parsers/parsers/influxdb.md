# InfluxDB Compatibility Parser Status

<!-- AUTO-GENERATED: compatibility parser status. Regenerate with
python3 project/tests/reference_regression/generate_compatibility_parser_docs.py --repo-root . --write
-->

Parser family: `influxdb`

Reference profile: `3.9.0`

Release batch: `analytic`

Public beta status: `beta_gate_passed_mapped_or_explicit_refusal`

Declared public surfaces covered: `54`

Surface digest: `a2776e5243b827bdc5582e06de4193b1a18ea1ca062aa2e7039743f29eeb0570`

This page is generated from the public compatibility parser remap matrix. Every row below is a declared beta parser surface and states whether it is supported through ScratchBird SBLR/parser-support routing, routed to a cluster/provider boundary, documented as presentation-only behavior, or explicitly refused with a deterministic diagnostic.

The ScratchBird engine remains SBLR/UUID-only. Compatibility SQL is parsed outside the engine, and accepted work is still revalidated by ScratchBird authority before execution.

## Summary

| Runtime disposition | Count | Meaning |
| --- | ---: | --- |
| `admitted_normalized_cluster_sblr_provider_boundary` | 3 | Routed to cluster provider boundary |
| `admitted_sblr_or_parser_support_route` | 38 | Supported through ScratchBird SBLR or parser-support route |
| `documentation_evidence_only` | 10 | Documented compatibility behavior |
| `exact_fail_closed_refusal` | 3 | Explicit fail-closed refusal |

| Classification | Count |
| --- | ---: |
| `ARCHITECTURE_REFUSAL` | 3 |
| `DOCUMENTATION_ONLY` | 10 |
| `IMPLEMENT_NONCLUSTER` | 15 |
| `NORMALIZE_CLUSTER` | 3 |
| `PARSER_REMAP_ONLY` | 23 |

## Surface Status

| Row | Functionality | Implementation status | Runtime disposition | Route or SBLR | Diagnostic/refusal policy | Proof status |
| --- | --- | --- | --- | --- | --- | --- |
| `FPR-P3-DECLARED-00777` | `influxdb.admin.show_servers` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `sblr.cluster.report.v3:cluster.admin.inspect_status` | `INFLUXDB.AUTHORITY.CLUSTER_CONTROL_RESERVED` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-00778` | `influxdb.admin.show_cluster` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `sblr.cluster.report.v3:cluster.admin.inspect_status` | `INFLUXDB.AUTHORITY.CLUSTER_CONTROL_RESERVED` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-00779` | `influxdb.flux.from` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.influxdb.influxdb_flux_from.bbe2e2e761` | `INFLUXDB.EMULATION.FLUX_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00780` | `influxdb.flux.range` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.influxdb.influxdb_flux_range.1cc759e507` | `INFLUXDB.EMULATION.FLUX_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00781` | `influxdb.flux.filter` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.influxdb.influxdb_flux_filter.5ad2505bf2` | `INFLUXDB.EMULATION.FLUX_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00782` | `influxdb.write.insert` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.influxdb.influxdb_write_insert.30aa89dfbe` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00783` | `influxdb.write.line_protocol` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.influxdb.influxdb_write_line_protocol.b17bbe383b` | `INFLUXDB.EMULATION.LINE_PROTOCOL_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00784` | `influxdb.query.select` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.influxdb.influxdb_query_select.d882d5821a` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00785` | `influxdb.dml.delete` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.influxdb.influxdb_dml_delete.7b54ee2455` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00786` | `influxdb.database.create` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.influxdb.influxdb_database_create.eb5bd8aed7` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00787` | `influxdb.database.drop` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.influxdb.influxdb_database_drop.2ec01ae536` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00788` | `influxdb.retention_policy.create` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.security_auth_policy_and_materialized_au.influxdb.influxdb_retention_policy_create.5f1484a3cf` | `INFLUXDB.EMULATION.RETENTION_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00789` | `influxdb.retention_policy.alter` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.security_auth_policy_and_materialized_au.influxdb.influxdb_retention_policy_alter.33459a138e` | `INFLUXDB.EMULATION.RETENTION_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00790` | `influxdb.retention_policy.drop` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.security_auth_policy_and_materialized_au.influxdb.influxdb_retention_policy_drop.121993b20e` | `INFLUXDB.EMULATION.RETENTION_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00791` | `influxdb.continuous_query.create` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.influxdb.influxdb_continuous_query_create.930407a9cc` | `INFLUXDB.EMULATION.CONTINUOUS_QUERY_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00792` | `influxdb.continuous_query.drop` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.influxdb.influxdb_continuous_query_drop.f4659e3c66` | `INFLUXDB.EMULATION.CONTINUOUS_QUERY_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00793` | `influxdb.subscription.create` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.influxdb.influxdb_subscription_create.805c4c8aba` | `INFLUXDB.EMULATION.SUBSCRIPTION_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00794` | `influxdb.catalog.show` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.influxdb.influxdb_catalog_show.9eebf3de83` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00795` | `datatype_surface:numeric` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00796` | `datatype_surface:string` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00797` | `datatype_surface:boolean` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00798` | `datatype_surface:timestamp` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00799` | `datatype_surface:tag` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00800` | `datatype_surface:field` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00801` | `datatype_surface:line_protocol` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.influxdb.datatype_surface_line_protocol.97d9d8c7b6` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00802` | `builtin_function_surface:aggregate` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.influxdb.builtin_function_surface_aggregate.9dfcfc8151` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00803` | `builtin_function_surface:selector` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.influxdb.builtin_function_surface_selector.75727d0776` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00804` | `builtin_function_surface:transform` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.influxdb.builtin_function_surface_transform.b2e160a371` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00805` | `builtin_function_surface:time` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.influxdb.builtin_function_surface_time.b277776be0` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00806` | `builtin_function_surface:flux` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.influxdb.builtin_function_surface_flux.a52208bff4` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00807` | `builtin_function_surface:math` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.influxdb.builtin_function_surface_math.bb3a67e5a6` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00808` | `builtin_function_surface:fill` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.influxdb.builtin_function_surface_fill.d2f650e3f4` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00809` | `catalog_overlay_surface:databases` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.influxdb.catalog_overlay_surface_databases.449f73d4e5` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00810` | `catalog_overlay_surface:buckets` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.influxdb.catalog_overlay_surface_buckets.a1322cb955` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00811` | `catalog_overlay_surface:measurements` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.influxdb.catalog_overlay_surface_measurements.7f28e1a187` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00812` | `catalog_overlay_surface:tag_keys` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.influxdb.catalog_overlay_surface_tag_keys.98e6d76ffa` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00813` | `catalog_overlay_surface:field_keys` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.influxdb.catalog_overlay_surface_field_keys.12593cf166` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00814` | `catalog_overlay_surface:retention_policies` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.influxdb.catalog_overlay_surface_retention_policies.1979173ea1` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00815` | `catalog_overlay_surface:continuous_queries` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.influxdb.catalog_overlay_surface_continuous_queries.c2bb290520` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00816` | `catalog_overlay_surface:subscriptions` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.influxdb.catalog_overlay_surface_subscriptions.2e7de93714` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00817` | `catalog_overlay_surface:tasks` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.influxdb.catalog_overlay_surface_tasks.8793f5bba3` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00818` | `diagnostic_surface:parse` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.influxdb.diagnostic_surface_parse.15c16bd490` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00819` | `diagnostic_surface:policy` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.influxdb.diagnostic_surface_policy.59f8e37942` | `exact_architecture_refusal;no_provider_call;no_parser_or_compatibility_execution` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-00820` | `diagnostic_surface:udr` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.influxdb.diagnostic_surface_udr.530ac3a1cf` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00821` | `diagnostic_surface:catalog` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.influxdb.diagnostic_surface_catalog.c267835c0f` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00822` | `diagnostic_surface:session` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.influxdb.diagnostic_surface_session.a044c11488` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00823` | `diagnostic_surface:transaction` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.influxdb.diagnostic_surface_transaction.1b3af997cd` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00824` | `diagnostic_surface:file_effects` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00825` | `diagnostic_surface:compatibility_execution` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00826` | `diagnostic_surface:mga` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00827` | `diagnostic_surface:support_bundle` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00828` | `source_marker:unsupported` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.influxdb.source_marker_unsupported.2eb6efdc63` | `exact_architecture_refusal;no_provider_call;no_parser_or_compatibility_execution` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-02046` | `udr_management_operation_set` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `sblr.replication.consumer.v3:cluster.replication.consume_cluster_event` | `compile_flag_disabled_returns_functionality_unsupported;public_stub_enabled_returns_functionality_unlicensed;private_provider_required_for_execution` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-02047` | `udr_diagnostic_vector_set` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.influxdb.udr_diagnostic_vector_set.9b617fdd4a` | `exact_architecture_refusal;no_provider_call;no_parser_or_compatibility_execution` | `p3_architecture_refusal_preserved` |

## Source Anchors

These anchors identify the source-backed declaration used to generate each row. They are included so a developer or auditor can trace the public status back to the implementation declaration without using private notes.

| Row | Source anchor | Parser package |
| --- | --- | --- |
| `FPR-P3-DECLARED-00777` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.admin.show_servers` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00778` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.admin.show_cluster` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00779` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.flux.from` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00780` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.flux.range` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00781` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.flux.filter` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00782` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.write.insert` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00783` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.write.line_protocol` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00784` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.query.select` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00785` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.dml.delete` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00786` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.database.create` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00787` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.database.drop` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00788` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.retention_policy.create` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00789` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.retention_policy.alter` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00790` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.retention_policy.drop` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00791` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.continuous_query.create` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00792` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.continuous_query.drop` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00793` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.subscription.create` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00794` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kPatterns:influxdb.catalog.show` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00795` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDatatypeSurfaces:numeric` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00796` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDatatypeSurfaces:string` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00797` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDatatypeSurfaces:boolean` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00798` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDatatypeSurfaces:timestamp` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00799` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDatatypeSurfaces:tag` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00800` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDatatypeSurfaces:field` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00801` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDatatypeSurfaces:line_protocol` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00802` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kBuiltinSurfaces:aggregate` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00803` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kBuiltinSurfaces:selector` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00804` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kBuiltinSurfaces:transform` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00805` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kBuiltinSurfaces:time` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00806` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kBuiltinSurfaces:flux` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00807` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kBuiltinSurfaces:math` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00808` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kBuiltinSurfaces:fill` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00809` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kCatalogSurfaces:databases` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00810` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kCatalogSurfaces:buckets` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00811` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kCatalogSurfaces:measurements` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00812` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kCatalogSurfaces:tag_keys` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00813` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kCatalogSurfaces:field_keys` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00814` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kCatalogSurfaces:retention_policies` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00815` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kCatalogSurfaces:continuous_queries` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00816` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kCatalogSurfaces:subscriptions` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00817` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kCatalogSurfaces:tasks` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00818` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDiagnosticSurfaces:parse` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00819` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDiagnosticSurfaces:policy` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00820` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDiagnosticSurfaces:udr` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00821` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDiagnosticSurfaces:catalog` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00822` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDiagnosticSurfaces:session` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00823` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDiagnosticSurfaces:transaction` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00824` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDiagnosticSurfaces:file_effects` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00825` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDiagnosticSurfaces:compatibility_execution` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00826` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDiagnosticSurfaces:mga` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00827` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#kDiagnosticSurfaces:support_bundle` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-00828` | `project/src/parsers/compatibility/influxdb/influxdb_dialect.cpp#marker:unsupported` | `project/src/parsers/compatibility/influxdb` |
| `FPR-P3-DECLARED-02046` | `project/src/udr/sbu_influxdb_parser_support/sbu_influxdb_parser_support.cpp#kManagementOperations` | `project/src/udr/sbu_influxdb_parser_support` |
| `FPR-P3-DECLARED-02047` | `project/src/udr/sbu_influxdb_parser_support/sbu_influxdb_parser_support.cpp#diagnostic_vectors` | `project/src/udr/sbu_influxdb_parser_support` |

# Dolt Compatibility Parser Status

<!-- AUTO-GENERATED: compatibility parser status. Regenerate with
python3 project/tests/reference_regression/generate_compatibility_parser_docs.py --repo-root . --write
-->

Parser family: `dolt`

Reference profile: `1.86.6`

Release batch: `distributed`

Retained pre-hold beta evidence status: `beta_gate_passed_mapped_or_explicit_refusal`

Declared public surfaces covered: `78`

Surface digest: `d353b3f6aa8a613c2343ed08530a50c528639eab858605900af4453ce6d3d4ff`

This page is generated from the public compatibility parser remap matrix. Its status and support wording records the last verified pre-hold SBLR baseline; it is historical evidence and is not a claim of executable conformance to the in-progress SBLR contract. Every row below is a declared beta parser surface and states whether it was supported through ScratchBird SBLR/parser-support routing, routed to a cluster/provider boundary, documented as presentation-only behavior, or explicitly refused with a deterministic diagnostic.

The ScratchBird engine remains SBLR/UUID-only. Compatibility SQL is parsed outside the engine, and accepted work is still revalidated by ScratchBird authority before execution.

## Summary

| Runtime disposition | Count | Meaning |
| --- | ---: | --- |
| `admitted_normalized_cluster_sblr_provider_boundary` | 1 | Routed to cluster provider boundary |
| `admitted_sblr_or_parser_support_route` | 56 | Supported through ScratchBird SBLR or parser-support route |
| `documentation_evidence_only` | 10 | Documented compatibility behavior |
| `exact_fail_closed_refusal` | 11 | Explicit fail-closed refusal |

| Classification | Count |
| --- | ---: |
| `ARCHITECTURE_REFUSAL` | 11 |
| `DOCUMENTATION_ONLY` | 10 |
| `IMPLEMENT_NONCLUSTER` | 13 |
| `NORMALIZE_CLUSTER` | 1 |
| `PARSER_REMAP_ONLY` | 43 |

## Surface Status

| Row | Functionality | Implementation status | Runtime disposition | Route or SBLR | Diagnostic/refusal policy | Proof status |
| --- | --- | --- | --- | --- | --- | --- |
| `FPR-P3-DECLARED-00336` | `dolt.remote.push` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.dolt.dolt_remote_push.76b85613e7` | `DOLT.AUTHORITY.REMOTE_DENIED` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-00337` | `dolt.remote.pull` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.dolt.dolt_remote_pull.a39c537462` | `DOLT.AUTHORITY.REMOTE_DENIED` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-00338` | `dolt.remote.fetch` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.dolt.dolt_remote_fetch.1715e9cc09` | `DOLT.AUTHORITY.REMOTE_DENIED` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-00339` | `dolt.remote.clone` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.dolt.dolt_remote_clone.a97ba6bb00` | `DOLT.AUTHORITY.REMOTE_DENIED` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-00340` | `dolt.remote.fetch_sql` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.dolt.dolt_remote_fetch_sql.a8c671afdd` | `DOLT.AUTHORITY.REMOTE_DENIED` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-00341` | `dolt.remote.push_sql` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.dolt.dolt_remote_push_sql.802e8c5b50` | `DOLT.AUTHORITY.REMOTE_DENIED` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-00342` | `dolt.remote.pull_sql` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.dolt.dolt_remote_pull_sql.c89c173027` | `DOLT.AUTHORITY.REMOTE_DENIED` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-00343` | `dolt.version.commit` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_version_commit.6e19bb2ec7` | `DOLT.EMULATION.VERSION_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00344` | `dolt.version.branch` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_version_branch.02c57549ae` | `DOLT.EMULATION.BRANCH_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00345` | `dolt.version.checkout` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_version_checkout.c48cdcf9c3` | `DOLT.EMULATION.BRANCH_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00346` | `dolt.version.merge` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_version_merge.126c9b45b1` | `DOLT.EMULATION.MERGE_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00347` | `dolt.version.stash` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_version_stash.85b71c35ca` | `DOLT.EMULATION.VERSION_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00348` | `dolt.version.diff` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_version_diff.b4a2e6d52a` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00349` | `dolt.version.log` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.dolt_version_log.1303867db4` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00350` | `dolt.version.status` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.dolt_version_status.0969f0fea1` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00351` | `dolt.version.branches` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_version_branches.1a74033e02` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00352` | `dolt.catalog.show` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.dolt_catalog_show.7862490185` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00353` | `dolt.catalog.describe` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.dolt_catalog_describe.351775553c` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00354` | `dolt.lifecycle.create_database` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_lifecycle_create_database.1b18256a50` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00355` | `dolt.lifecycle.drop_database` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_lifecycle_drop_database.cc8d287f50` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00356` | `dolt.security.create_user` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.security_auth_policy_and_materialized_au.dolt.dolt_security_create_user.cd7487dd97` | `DOLT.EMULATION.SECURITY_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00357` | `dolt.security.grant` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.security_auth_policy_and_materialized_au.dolt.dolt_security_grant.db2708a484` | `DOLT.EMULATION.SECURITY_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00358` | `dolt.security.revoke` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.security_auth_policy_and_materialized_au.dolt.dolt_security_revoke.ec9961a2af` | `DOLT.EMULATION.SECURITY_ROUTE` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00359` | `dolt.session.set` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_session_set.aeb738d2f7` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00360` | `dolt.session.use_database` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_session_use_database.e4c628ac22` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00361` | `dolt.ddl.create` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_ddl_create.e26a18e617` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00362` | `dolt.ddl.alter` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_ddl_alter.bdde6758da` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00363` | `dolt.ddl.drop` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_ddl_drop.686e27ee0b` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00364` | `dolt.dml.insert` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_dml_insert.0f20216ceb` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00365` | `dolt.dml.update` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_dml_update.1645096d16` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00366` | `dolt.dml.delete` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_dml_delete.b29d60c3f3` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00367` | `dolt.query.select` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_query_select.139c135af9` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00368` | `dolt.query.with` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_query_with.72d6224e8b` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00369` | `dolt.transaction.start` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_transaction_start.9456bfb36d` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00370` | `dolt.transaction.commit` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_transaction_commit.73e38ee741` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00371` | `dolt.transaction.rollback` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.dolt_transaction_rollback.45170f483e` | `none` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00372` | `datatype_surface:numeric` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00373` | `datatype_surface:text` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00374` | `datatype_surface:binary` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00375` | `datatype_surface:temporal` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00376` | `datatype_surface:boolean` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00377` | `datatype_surface:json` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00378` | `datatype_surface:enum_set` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.datatype_surface_enum_set.fb3f0457dd` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00379` | `datatype_surface:spatial` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.datatype_surface_spatial.4043556d05` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00380` | `datatype_surface:version_hash` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.datatype_surface_version_hash.26239815df` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00381` | `datatype_surface:revision_selector` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.compatibility_specific_noncluster_control.dolt.datatype_surface_revision_selector.f7baa05ab1` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00382` | `builtin_function_surface:aggregate` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.dolt.builtin_function_surface_aggregate.be329c56f0` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00383` | `builtin_function_surface:string` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.dolt.builtin_function_surface_string.464cd3cb66` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00384` | `builtin_function_surface:json` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.dolt.builtin_function_surface_json.e044cb4e01` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00385` | `builtin_function_surface:version_control` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.dolt.builtin_function_surface_version_control.7650e482b8` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00386` | `builtin_function_surface:diff` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.dolt.builtin_function_surface_diff.7af6912e3a` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00387` | `builtin_function_surface:history` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.dolt.builtin_function_surface_history.94ed212afd` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00388` | `builtin_function_surface:remote` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.dolt.builtin_function_surface_remote.26efeacba1` | `exact_architecture_refusal;no_provider_call;no_parser_or_compatibility_execution` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-00389` | `builtin_function_surface:security` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.dolt.builtin_function_surface_security.4fee167ba1` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00390` | `builtin_function_surface:session` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.extension_udr_connector_management.dolt.builtin_function_surface_session.338d630461` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00391` | `catalog_overlay_surface:branches` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.catalog_overlay_surface_branches.1f2adb60cc` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00392` | `catalog_overlay_surface:commits` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.catalog_overlay_surface_commits.21c07a81f1` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00393` | `catalog_overlay_surface:status` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.catalog_overlay_surface_status.6f24436f24` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00394` | `catalog_overlay_surface:diff` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.catalog_overlay_surface_diff.806a8e1cf5` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00395` | `catalog_overlay_surface:conflicts` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.catalog_overlay_surface_conflicts.4a3a8e4383` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00396` | `catalog_overlay_surface:schemas` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.catalog_overlay_surface_schemas.5081a70976` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00397` | `catalog_overlay_surface:docs` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.catalog_overlay_surface_docs.78102c3143` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00398` | `catalog_overlay_surface:remotes` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.catalog_overlay_surface_remotes.a956439e1d` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00399` | `catalog_overlay_surface:tags` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.catalog_overlay_surface_tags.30fae884b5` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00400` | `catalog_overlay_surface:procedures` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.catalog_overlay_surface_procedures.ebf9d187bd` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00401` | `diagnostic_surface:parse` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.diagnostic_surface_parse.4ebe1ee9ce` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00402` | `diagnostic_surface:policy` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.dolt.diagnostic_surface_policy.dcb8e1edde` | `exact_architecture_refusal;no_provider_call;no_parser_or_compatibility_execution` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-00403` | `diagnostic_surface:udr` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.diagnostic_surface_udr.a202a72ffb` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00404` | `diagnostic_surface:catalog` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.diagnostic_surface_catalog.6896337e0f` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00405` | `diagnostic_surface:session` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.diagnostic_surface_session.6c94f09ad9` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00406` | `diagnostic_surface:transaction` | Supported through ScratchBird SBLR or parser-support route | `admitted_sblr_or_parser_support_route` | `sblr.noncluster.observability_metrics_tracing_events.dolt.diagnostic_surface_transaction.33081c9d90` | `not_refusal;parser_emits_sblr_only;compatibility_execution_storage_transaction_finality_forbidden` | `p3_noncluster_declared_route_joined_to_fpr_p1` |
| `FPR-P3-DECLARED-00407` | `diagnostic_surface:file_effects` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00408` | `diagnostic_surface:compatibility_execution` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00409` | `diagnostic_surface:mga` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00410` | `diagnostic_surface:support_bundle` | Documented compatibility behavior | `documentation_evidence_only` | `documentation_only_not_executable` | `not_runtime_route;not_execution_claim` | `p3_documentation_only_preserved` |
| `FPR-P3-DECLARED-00411` | `source_marker:unsupported` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.dolt.source_marker_unsupported.7d841f2da8` | `exact_architecture_refusal;no_provider_call;no_parser_or_compatibility_execution` | `p3_architecture_refusal_preserved` |
| `FPR-P3-DECLARED-02036` | `udr_management_operation_set` | Routed to cluster provider boundary | `admitted_normalized_cluster_sblr_provider_boundary` | `sblr.replication.consumer.v3:cluster.replication.consume_cluster_event` | `compile_flag_disabled_returns_functionality_unsupported;public_stub_enabled_returns_functionality_unlicensed;private_provider_required_for_execution` | `p3_cluster_declared_route_joined_to_fpr_p2` |
| `FPR-P3-DECLARED-02037` | `udr_diagnostic_vector_set` | Explicit fail-closed refusal | `exact_fail_closed_refusal` | `sblr.refusal.dolt.udr_diagnostic_vector_set.c4507f0a1f` | `exact_architecture_refusal;no_provider_call;no_parser_or_compatibility_execution` | `p3_architecture_refusal_preserved` |

## Source Anchors

These anchors identify the source-backed declaration used to generate each row. They are included so a developer or auditor can trace the public status back to the implementation declaration without using private notes.

| Row | Source anchor | Parser package |
| --- | --- | --- |
| `FPR-P3-DECLARED-00336` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.remote.push` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00337` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.remote.pull` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00338` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.remote.fetch` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00339` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.remote.clone` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00340` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.remote.fetch_sql` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00341` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.remote.push_sql` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00342` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.remote.pull_sql` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00343` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.version.commit` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00344` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.version.branch` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00345` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.version.checkout` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00346` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.version.merge` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00347` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.version.stash` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00348` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.version.diff` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00349` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.version.log` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00350` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.version.status` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00351` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.version.branches` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00352` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.catalog.show` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00353` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.catalog.describe` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00354` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.lifecycle.create_database` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00355` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.lifecycle.drop_database` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00356` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.security.create_user` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00357` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.security.grant` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00358` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.security.revoke` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00359` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.session.set` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00360` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.session.use_database` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00361` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.ddl.create` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00362` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.ddl.alter` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00363` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.ddl.drop` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00364` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.dml.insert` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00365` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.dml.update` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00366` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.dml.delete` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00367` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.query.select` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00368` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.query.with` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00369` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.transaction.start` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00370` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.transaction.commit` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00371` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kPatterns:dolt.transaction.rollback` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00372` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDatatypeSurfaces:numeric` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00373` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDatatypeSurfaces:text` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00374` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDatatypeSurfaces:binary` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00375` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDatatypeSurfaces:temporal` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00376` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDatatypeSurfaces:boolean` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00377` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDatatypeSurfaces:json` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00378` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDatatypeSurfaces:enum_set` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00379` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDatatypeSurfaces:spatial` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00380` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDatatypeSurfaces:version_hash` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00381` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDatatypeSurfaces:revision_selector` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00382` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kBuiltinSurfaces:aggregate` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00383` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kBuiltinSurfaces:string` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00384` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kBuiltinSurfaces:json` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00385` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kBuiltinSurfaces:version_control` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00386` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kBuiltinSurfaces:diff` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00387` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kBuiltinSurfaces:history` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00388` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kBuiltinSurfaces:remote` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00389` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kBuiltinSurfaces:security` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00390` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kBuiltinSurfaces:session` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00391` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kCatalogSurfaces:branches` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00392` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kCatalogSurfaces:commits` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00393` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kCatalogSurfaces:status` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00394` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kCatalogSurfaces:diff` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00395` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kCatalogSurfaces:conflicts` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00396` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kCatalogSurfaces:schemas` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00397` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kCatalogSurfaces:docs` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00398` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kCatalogSurfaces:remotes` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00399` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kCatalogSurfaces:tags` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00400` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kCatalogSurfaces:procedures` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00401` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDiagnosticSurfaces:parse` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00402` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDiagnosticSurfaces:policy` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00403` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDiagnosticSurfaces:udr` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00404` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDiagnosticSurfaces:catalog` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00405` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDiagnosticSurfaces:session` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00406` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDiagnosticSurfaces:transaction` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00407` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDiagnosticSurfaces:file_effects` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00408` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDiagnosticSurfaces:compatibility_execution` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00409` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDiagnosticSurfaces:mga` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00410` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#kDiagnosticSurfaces:support_bundle` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-00411` | `project/src/parsers/compatibility/dolt/dolt_dialect.cpp#marker:unsupported` | `project/src/parsers/compatibility/dolt` |
| `FPR-P3-DECLARED-02036` | `project/src/udr/sbu_dolt_parser_support/sbu_dolt_parser_support.cpp#kManagementOperations` | `project/src/udr/sbu_dolt_parser_support` |
| `FPR-P3-DECLARED-02037` | `project/src/udr/sbu_dolt_parser_support/sbu_dolt_parser_support.cpp#diagnostic_vectors` | `project/src/udr/sbu_dolt_parser_support` |

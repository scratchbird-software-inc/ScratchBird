# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(SB_BUILD_QOW_CLOSURE_TESTS)
  if(NOT TARGET sb_core_platform)
    add_subdirectory(
      ${CMAKE_CURRENT_SOURCE_DIR}/../../core/platform
      ${CMAKE_CURRENT_BINARY_DIR}/qow_core_platform
    )
  endif()
  if(NOT TARGET sb_core_datatypes)
    add_subdirectory(
      ${CMAKE_CURRENT_SOURCE_DIR}/../../core/datatypes
      ${CMAKE_CURRENT_BINARY_DIR}/qow_core_datatypes
    )
  endif()
  # QOW-CMAKE-QRY-002-V1
  # QOW-CMAKE-QRY-015-V1
  # QOW-CMAKE-OPT-008-V1
  # QOW-CMAKE-OPT-015-V1
  # QOW-CMAKE-IAS-005-V1
  # QOW-CMAKE-QRY-022-V1
  if(NOT TARGET sb_engine_executor)
    add_subdirectory(
      ${CMAKE_CURRENT_SOURCE_DIR}/../executor
      ${CMAKE_CURRENT_BINARY_DIR}/qow_engine_executor
    )
  endif()
  add_library(sb_qow_relational_dag_contract STATIC query/plan_api.cpp)
  target_compile_features(sb_qow_relational_dag_contract PUBLIC cxx_std_20)
  target_compile_definitions(sb_qow_relational_dag_contract PRIVATE
    SCRATCHBIRD_QOW_RELATIONAL_DAG_CONTRACT_ONLY=1
  )
  target_include_directories(sb_qow_relational_dag_contract PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../planner
  )
  target_link_libraries(sb_qow_relational_dag_contract PUBLIC
    sb_engine_executor
  )
  # QOW-CMAKE-OPT-004-V1
  add_executable(qow_opt_004_test_v1
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/optimizer/qow_opt_004.cpp
  )
  target_compile_features(qow_opt_004_test_v1 PRIVATE cxx_std_20)
  target_link_libraries(qow_opt_004_test_v1 PRIVATE
    sb_qow_relational_dag_contract
  )
  add_test(NAME qow_opt_004_v1 COMMAND qow_opt_004_test_v1)
  set_tests_properties(qow_opt_004_v1 PROPERTIES
    LABELS "qow_closure;QOW-306;QOW-LEAF-OPT-004;development_candidate"
  )
  # QOW-CMAKE-QRY-008-DESC-V1
  # QOW-CMAKE-QRY-008-NULL-V1
  # QOW-CMAKE-QRY-008-COERCE-V1
  # QOW-CMAKE-QRY-008-COLLATION-V1
  # QOW-CMAKE-QRY-008-TIMEZONE-V1
  # QOW-CMAKE-QRY-008-OVERFLOW-V1
  # QOW-CMAKE-QRY-017-V1
  add_library(sb_qow_typed_scalar_descriptor_contract ALIAS
    sb_engine_typed_scalar_descriptor_contract)
  # Focused projection contract library shared by the QRY-026/QRY-027 proofs.
  add_library(sb_qow_typed_parameter_contract STATIC
    query/projection_api.cpp
  )
  target_compile_features(sb_qow_typed_parameter_contract PUBLIC cxx_std_20)
  target_compile_definitions(sb_qow_typed_parameter_contract PRIVATE
    SCRATCHBIRD_QOW_TYPED_PARAMETER_CONTRACT_ONLY=1
  )
  target_include_directories(sb_qow_typed_parameter_contract PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  )
  target_link_libraries(sb_qow_typed_parameter_contract PRIVATE
    sb_core_datatypes
  )
  # QOW-CMAKE-QRY-025-V1
  add_executable(qow_qry_025_test_v1
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/sbsql_parser_worker/qow_qry_025.cpp
  )
  target_compile_features(qow_qry_025_test_v1 PRIVATE cxx_std_20)
  target_link_libraries(qow_qry_025_test_v1 PRIVATE
    sb_qow_typed_scalar_descriptor_contract
  )
  add_test(NAME qow_qry_025_v1 COMMAND qow_qry_025_test_v1)
  set_tests_properties(qow_qry_025_v1 PROPERTIES
    LABELS "qow_closure;QOW-102;QOW-LEAF-QRY-025;development_candidate"
  )
  # QOW-CMAKE-QRY-026-V1
  add_executable(qow_qry_026_test_v1
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/sbsql_parser_worker/qow_qry_026.cpp
  )
  target_compile_features(qow_qry_026_test_v1 PRIVATE cxx_std_20)
  target_link_libraries(qow_qry_026_test_v1 PRIVATE
    sb_qow_typed_parameter_contract
  )
  add_test(NAME qow_qry_026_v1 COMMAND qow_qry_026_test_v1)
  set_tests_properties(qow_qry_026_v1 PROPERTIES
    LABELS "qow_closure;QOW-102;QOW-LEAF-QRY-026;development_candidate"
  )
  # QOW-CMAKE-QRY-027-V1
  add_executable(qow_qry_027_test_v1
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/sbsql_parser_worker/qow_qry_027.cpp
  )
  target_compile_features(qow_qry_027_test_v1 PRIVATE cxx_std_20)
  target_link_libraries(qow_qry_027_test_v1 PRIVATE
    sb_qow_typed_parameter_contract
  )
  add_test(NAME qow_qry_027_v1 COMMAND qow_qry_027_test_v1)
  set_tests_properties(qow_qry_027_v1 PROPERTIES
    LABELS "qow_closure;QOW-103;QOW-LEAF-QRY-027;development_candidate"
  )
  # QOW-CMAKE-QRY-028-V1
  add_executable(qow_qry_028_test_v1
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/sbsql_parser_worker/qow_qry_028.cpp
  )
  target_compile_features(qow_qry_028_test_v1 PRIVATE cxx_std_20)
  target_link_libraries(qow_qry_028_test_v1 PRIVATE
    sb_qow_typed_scalar_descriptor_contract
  )
  add_test(NAME qow_qry_028_v1 COMMAND qow_qry_028_test_v1)
  set_tests_properties(qow_qry_028_v1 PROPERTIES
    LABELS "qow_closure;QOW-102;QOW-LEAF-QRY-028;development_candidate"
  )
  set(SB_QOW_INTERNAL_API_TESTS_ENROLLED TRUE PARENT_SCOPE)
  return()
endif()

if(NOT TARGET sb_core_platform)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/platform ${CMAKE_CURRENT_BINARY_DIR}/core_platform)
endif()
if(NOT TARGET sb_core_uuid)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/uuid ${CMAKE_CURRENT_BINARY_DIR}/core_uuid)
endif()
if(NOT TARGET sb_core_datatypes)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/datatypes ${CMAKE_CURRENT_BINARY_DIR}/core_datatypes)
endif()
if(NOT TARGET sb_core_hash)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/hash ${CMAKE_CURRENT_BINARY_DIR}/core_hash)
endif()
if(NOT TARGET sb_core_resources)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/resources ${CMAKE_CURRENT_BINARY_DIR}/core_resources)
endif()
if(NOT TARGET sb_core_metrics)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/metrics ${CMAKE_CURRENT_BINARY_DIR}/core_metrics)
endif()
if(NOT TARGET sb_core_agents)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/agents ${CMAKE_CURRENT_BINARY_DIR}/core_agents)
endif()
if(NOT TARGET sb_core_memory)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/memory ${CMAKE_CURRENT_BINARY_DIR}/core_memory)
endif()
if(NOT TARGET sb_storage_disk)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../storage/disk ${CMAKE_CURRENT_BINARY_DIR}/storage_disk)
endif()
if(NOT TARGET sb_transaction_mga)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../transaction/mga ${CMAKE_CURRENT_BINARY_DIR}/transaction_mga)
endif()
if(NOT TARGET sb_storage_page)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../storage/page ${CMAKE_CURRENT_BINARY_DIR}/storage_page)
endif()
if(NOT TARGET sb_storage_filespace)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../storage/filespace ${CMAKE_CURRENT_BINARY_DIR}/storage_filespace)
endif()
if(NOT TARGET sb_core_index)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/index ${CMAKE_CURRENT_BINARY_DIR}/core_index)
endif()
if(NOT TARGET sb_core_bulk_load)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/bulk_load ${CMAKE_CURRENT_BINARY_DIR}/core_bulk_load)
endif()
if(NOT TARGET sb_core_catalog)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/catalog ${CMAKE_CURRENT_BINARY_DIR}/core_catalog)
endif()
if(NOT TARGET sb_storage_database)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../storage/database ${CMAKE_CURRENT_BINARY_DIR}/storage_database)
endif()
if(NOT TARGET sb_engine_executor)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../executor ${CMAKE_CURRENT_BINARY_DIR}/engine_executor)
endif()
if(NOT TARGET sb_engine_native_compile)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../native_compile ${CMAKE_CURRENT_BINARY_DIR}/engine_native_compile)
endif()
if(NOT TARGET sb_engine_gpu_acceleration)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../gpu_acceleration ${CMAKE_CURRENT_BINARY_DIR}/engine_gpu_acceleration)
endif()
if(NOT TARGET sb_engine_plan_shapes)
  add_library(sb_engine_plan_shapes
    ${CMAKE_CURRENT_SOURCE_DIR}/../planner/dml_plan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../planner/expression_plan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../planner/logical_plan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../planner/nosql_plan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../planner/operation_plan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../planner/plan_diagnostics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../planner/relational_plan.cpp
  )
  target_include_directories(sb_engine_plan_shapes PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../planner
  )
  target_compile_features(sb_engine_plan_shapes PUBLIC cxx_std_20)
endif()
if(NOT TARGET sb_engine_optimizer_contract_only)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../optimizer ${CMAKE_CURRENT_BINARY_DIR}/engine_optimizer)
endif()
if(NOT TARGET sb_udr_runtime)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../udr/runtime ${CMAKE_CURRENT_BINARY_DIR}/udr_runtime)
endif()

set_source_files_properties(query/expression_api.cpp PROPERTIES
  COMPILE_DEFINITIONS
    SCRATCHBIRD_TYPED_SCALAR_DESCRIPTOR_CONTRACT_EXTERNAL=1
)

add_library(sb_engine_internal_api
  artifacts/artifact_api.cpp
  behavior_support/api_behavior_store.cpp
  crud_support/crud_store.cpp
  domain_support/domain_store.cpp
  api_unsupported.cpp
  cloud/cloud_deployment_profile.cpp
  cloud/cloud_provider_capability.cpp
  cloud/cloud_identity_kms.cpp
  cloud/edge_cache_cdn.cpp
  cloud/external_effect_outbox.cpp
  engine_internal_api.cpp
  engine_builtin_operations.cpp
  engine_database_runtime.cpp
  agents/agent_action_hooks_api.cpp
  agents/agent_action_dispatch_store_api.cpp
  agents/agent_durable_catalog_store_api.cpp
  agents/agent_enterprise_decision_store_api.cpp
  agents/agent_support_bundle_triage_route_api.cpp
  agents/agent_local_workflow_store_api.cpp
  agents/agent_management_api.cpp
  agents/agent_runtime_service_store_api.cpp
  catalog/catalog_object_lifecycle.cpp
  catalog/catalog_lookup_api.cpp
  catalog/agent_catalog_runtime_schema_versioning.cpp
  catalog/datatype_transport_api.cpp
  catalog/ddl_support_service.cpp
  catalog/descriptor_mutation_api.cpp
  catalog/descriptor_api.cpp
  catalog/datatype_index_optimizer_admission_api.cpp
  catalog/global_aggregate_view.cpp
  catalog/ipar_fast_path_support.cpp
  catalog/name_registry.cpp
  catalog/name_resolution_api.cpp
  catalog/relation_descriptor_projection.cpp
  catalog/relation_projection_view.cpp
  catalog/pinned_descriptor_cache.cpp
  catalog/schema_tree_api.cpp
  catalog/sbsql_language_elements_catalog.cpp
  catalog/structured_type_api.cpp
  catalog/sys_information_projection.cpp
  catalog/wire_driver_metadata_api.cpp
  cluster/cluster_control_api.cpp
  cluster/cluster_inspect_api.cpp
  cluster/cluster_insert_route_api.cpp
  cluster/placement_api.cpp
  cluster/profile_operation_api.cpp
  cluster/replication_api.cpp
  cluster/remote_participant_insert_api.cpp
  ddl/alter_api.cpp
  ddl/comment_api.cpp
  ddl/create_api.cpp
  ddl/drop_api.cpp
  diagnostics/diagnostic_rendering.cpp
  diagnostics/diagnostic_template_cache.cpp
  dml/constraint_enforcement.cpp
  dml/delete_api.cpp
  dml/delete_batch.cpp
  dml/dml_ingestion_pipeline.cpp
  dml/dml_index_write_path.cpp
  dml/dml_operation_path_proof.cpp
  dml/dml_row_locator_stream.cpp
  dml/dml_target_access_plan.cpp
  dml/global_aggregate_projection.cpp
  dml/hot_cold_row_split_api.cpp
  dml/index_apply_locality_bridge.cpp
  dml/insert_batch.cpp
  dml/insert_physical_integration.cpp
  dml/insert_api.cpp
  dml/import_api.cpp
  dml/import_execution_api.cpp
  dml/import_reject_model.cpp
  dml/import_resume_checkpoint.cpp
  dml/native_bulk_ingest_api.cpp
  dml/write_result_policy.cpp
  dml/merge_api.cpp
  dml/page_allocation_runtime_bridge.cpp
  dml/select_api.cpp
  dml/serializable_mutation_guard.cpp
  dml/temporal_bitemporal_api.cpp
  dml/update_batch.cpp
  dml/update_delete_optimized.cpp
  dml/update_api.cpp
  extensibility/executable_object_lifecycle.cpp
  extensibility/extension_boundary_manifest.cpp
  extensibility/gpu_api.cpp
  extensibility/llvm_api.cpp
  extensibility/parser_package_api.cpp
  extensibility/udr_api.cpp
  management/config_api.cpp
  management/index_management_api.cpp
  management/management_api.cpp
  management/memory_management_api.cpp
  management/migration_management_api.cpp
  management/support_bundle_api.cpp
  mga_relation_store/mga_relation_descriptor.cpp
  mga_relation_store/mga_physical_cleanup_coordinator.cpp
  mga_relation_store/mga_relation_store.cpp
  nosql/document_api.cpp
  nosql/document_path_physical_provider.cpp
  nosql/graph_api.cpp
  nosql/key_value_api.cpp
  nosql/nosql_backpressure_debt_api.cpp
  nosql/nosql_family_maintenance_api.cpp
  nosql/nosql_statistics_api.cpp
  nosql/nosql_provider_generation_store.cpp
  nosql/search_api.cpp
  nosql/spatial_api.cpp
  nosql/columnar_api.cpp
  nosql/time_series_api.cpp
  nosql/vector_api.cpp
  notification/notification_api.cpp
  observability/cleanup_diagnostics_api.cpp
  observability/explain_api.cpp
  observability/agent_evidence_retention_api.cpp
  observability/agent_observability_api.cpp
  observability/cluster_support_bundle_redaction_api.cpp
  observability/metrics_api.cpp
  observability/optimizer_metric_support_bundle.cpp
  observability/dml_summary_counters.cpp
  observability/performance_metric_event.cpp
  observability/performance_optimization_surface.cpp
  observability/repair_history_api.cpp
  observability/show_api.cpp
  procedural/procedural_api.cpp
  query/canonical_heap_optimizer_admission.cpp
  query/expression_api.cpp
  query/optimizer_plan_lifecycle.cpp
  query/plan_api.cpp
  query/predicate_api.cpp
  query/projection_api.cpp
  lifecycle/engine_lifecycle_api.cpp
  lifecycle/sequence_generator_lifecycle.cpp
  security/audit_api.cpp
  security/auth_challenge_api.cpp
  security/auth_credential_api.cpp
  security/auth_provider_live_adapter.cpp
  security/auth_provider_model.cpp
  security/auth_provider_observability_api.cpp
  security/auth_provider_plugin_api.cpp
  security/auth_provider_policy_api.cpp
  security/auth_token_api.cpp
  security/authentication_api.cpp
  security/authorization_api.cpp
  security/authority_api.cpp
  security/deep_enforcement_api.cpp
  security/external_group_api.cpp
  security/grant_api.cpp
  security/identity_api.cpp
  security/policy_api.cpp
  security/plugin_trust_api.cpp
  security/protected_material_api.cpp
  security/security_crypto_policy.cpp
  security/security_principal_lifecycle.cpp
  security/security_model.cpp
  security/standard_bundle_api.cpp
  backup_archive/backup_archive_api.cpp
  security/visibility_api.cpp
  storage/storage_management_api.cpp
  transaction/savepoint_api.cpp
  transaction/transaction_inspect_api.cpp
  transaction/transaction_api.cpp
  ../sblr/sblr_runtime.cpp
  ../sblr/sblr_sequence_runtime.cpp
  ../sblr/sblr_autonomous_frame_runtime.cpp
  sblr_executor_availability_registry.cpp
  sblr_parameter_set_registry.cpp
  sblr_prepared_coordination_registry.cpp
  sblr_variable_descriptor_registry.cpp
  sblr_variable_frame_coordinator.cpp
  sblr_source_map_descriptor_registry.cpp
    sblr_error_vector_descriptor_registry.cpp
    sblr_diagnostic_identity_registry.cpp
    sblr_transaction_begin_authority.cpp
  sblr_savepoint_coordinator.cpp
  sblr_autonomous_frame_coordinator.cpp
  sblr_reservation_release_coordinator.cpp
  sblr_temporary_instance_cleanup_coordinator.cpp
    sblr_cursor_open_coordinator.cpp
    sblr_read_by_key_coordinator.cpp
    sblr_read_range_coordinator.cpp
    sblr_read_stream_coordinator.cpp
    sblr_result_set_pass_coordinator.cpp
    sblr_access_cursor_open_coordinator.cpp
    sblr_insert_coordinator.cpp
    sblr_update_coordinator.cpp
    sblr_delete_coordinator.cpp
    sblr_merge_coordinator.cpp
    sblr_table_truncate_coordinator.cpp
  sblr_table_analyze_coordinator.cpp
  sblr_bulk_import_stream_coordinator.cpp
  sblr_bulk_export_stream_coordinator.cpp
  sblr_statement_batch_coordinator.cpp
  sblr_atomic_cas_coordinator.cpp
  sblr_atomic_rmw_coordinator.cpp
  sblr_advisory_lock_coordinator.cpp
  sblr_advisory_lock_release_coordinator.cpp
  sblr_function_call_coordinator.cpp
  sblr_operator_call_coordinator.cpp
  sblr_cast_coordinator.cpp
  sblr_compare_coordinator.cpp
  sblr_domain_operation_coordinator.cpp
  sblr_udr_invoke_coordinator.cpp
  sblr_procedure_invoke_coordinator.cpp
  sblr_function_invoke_coordinator.cpp
  sblr_aggregate_invoke_coordinator.cpp
  sblr_sequence_nextval_coordinator.cpp
  sblr_sequence_setval_coordinator.cpp
  sblr_query_numeric_coordinator.cpp
  sblr_advanced_datatype_family_coordinator.cpp
  sblr_project_coordinator.cpp
  sblr_catalog_introspect_coordinator.cpp
  sblr_kv_structured_read_coordinator.cpp
  sblr_kv_structured_mutate_coordinator.cpp
  sblr_kv_structured_scan_coordinator.cpp
  sblr_kv_structured_stream_read_coordinator.cpp
  sblr_kv_structured_stream_append_coordinator.cpp
  sblr_kv_structured_timeseries_coordinator.cpp
  sblr_system_config_set_coordinator.cpp
  sblr_ddl_create_domain_coordinator.cpp
  sblr_ddl_create_schema_coordinator.cpp
  sblr_ddl_create_table_coordinator.cpp
  sblr_ddl_create_table_as_query_coordinator.cpp
  sblr_ddl_create_index_coordinator.cpp
  sblr_ddl_drop_index_coordinator.cpp
  sblr_ddl_alter_domain_coordinator.cpp
  sblr_ddl_create_view_coordinator.cpp
  sblr_ddl_create_publication_coordinator.cpp
  sblr_ddl_alter_publication_coordinator.cpp
  sblr_ddl_drop_publication_coordinator.cpp
  sblr_ddl_create_subscription_coordinator.cpp
  sblr_ddl_alter_subscription_coordinator.cpp
  sblr_ddl_drop_subscription_coordinator.cpp
  sblr_ddl_create_operator_coordinator.cpp
  sblr_ddl_create_materialized_view_coordinator.cpp
  sblr_ddl_alter_view_coordinator.cpp
  sblr_ddl_drop_view_coordinator.cpp
  sblr_ddl_create_trigger_coordinator.cpp
  sblr_ddl_alter_trigger_coordinator.cpp
  sblr_ddl_create_procedure_coordinator.cpp
  sblr_ddl_alter_procedure_coordinator.cpp
  sblr_ddl_drop_procedure_coordinator.cpp
  sblr_ddl_create_function_coordinator.cpp
  sblr_ddl_alter_function_coordinator.cpp
  sblr_ddl_create_package_coordinator.cpp
  sblr_sec_alter_user_coordinator.cpp
  sblr_sec_drop_role_coordinator.cpp
  sblr_sec_create_role_coordinator.cpp
  sblr_sec_alter_role_coordinator.cpp
  sblr_sec_create_group_mapping_coordinator.cpp
  sblr_sec_drop_group_mapping_coordinator.cpp
  sblr_sec_grant_coordinator.cpp
  sblr_sec_revoke_coordinator.cpp
  sblr_sec_alter_policy_coordinator.cpp
  sblr_sec_create_policy_coordinator.cpp
  sblr_sec_drop_policy_coordinator.cpp
  sblr_sec_drop_user_coordinator.cpp
  sblr_sec_authenticate_coordinator.cpp
  sblr_sec_deauthenticate_coordinator.cpp
  sblr_session_role_switch_coordinator.cpp
  sblr_session_setting_set_coordinator.cpp
  sblr_ddl_drop_package_coordinator.cpp
  sblr_ddl_alter_package_coordinator.cpp
  sblr_ddl_alter_sequence_coordinator.cpp
  sblr_ddl_drop_sequence_coordinator.cpp
  sblr_ddl_drop_materialized_view_coordinator.cpp
  sblr_ddl_drop_table_coordinator.cpp
  sblr_ddl_create_type_coordinator.cpp
  sblr_ddl_alter_type_coordinator.cpp
  sblr_ddl_drop_type_coordinator.cpp
  sblr_ddl_refresh_materialized_view_coordinator.cpp
  sblr_ddl_create_temporary_table_coordinator.cpp
  sblr_ddl_drop_temporary_table_coordinator.cpp
  sblr_ddl_rename_object_vector_coordinator.cpp
  sblr_ddl_create_or_replace_srs_coordinator.cpp
  sblr_ddl_drop_srs_coordinator.cpp
  sblr_ddl_create_rewrite_rule_coordinator.cpp
  sblr_ddl_drop_rewrite_rule_coordinator.cpp
  sblr_ddl_validate_constraint_coordinator.cpp
  sblr_security_create_privilege_template_coordinator.cpp
  sblr_security_alter_privilege_template_coordinator.cpp
  sblr_security_drop_privilege_template_coordinator.cpp
  sblr_database_create_template_clone_coordinator.cpp
  sblr_ddl_create_aggregate_coordinator.cpp
  sblr_ddl_alter_aggregate_coordinator.cpp
  sblr_ddl_drop_aggregate_coordinator.cpp
  sblr_ddl_drop_function_coordinator.cpp
  sblr_ddl_purge_system_history_coordinator.cpp
  sblr_ddl_set_index_optimizer_eligibility_coordinator.cpp
  sblr_ddl_set_table_type_enforcement_coordinator.cpp
  sblr_database_serialize_logical_snapshot_coordinator.cpp
  sblr_database_deserialize_logical_snapshot_coordinator.cpp
  sblr_ddl_create_macro_coordinator.cpp
  sblr_ddl_drop_macro_coordinator.cpp
  sblr_admin_register_external_relation_resolver_coordinator.cpp
  sblr_admin_unregister_external_relation_resolver_coordinator.cpp
  sblr_ddl_create_dictionary_coordinator.cpp
  sblr_ddl_drop_dictionary_coordinator.cpp
  sblr_ddl_alter_dictionary_coordinator.cpp
  sblr_ddl_create_continuous_view_coordinator.cpp
  sblr_ddl_alter_continuous_view_coordinator.cpp
  sblr_ddl_drop_continuous_view_coordinator.cpp
  sblr_dml_async_insert_submit_coordinator.cpp
  sblr_dml_async_insert_status_coordinator.cpp
  sblr_dml_async_insert_cancel_coordinator.cpp
  sblr_dml_conditional_mutate_coordinator.cpp
  sblr_dml_counter_add_coordinator.cpp
  sblr_ddl_timeseries_series_cardinality_policy_coordinator.cpp
  sblr_ddl_create_timeseries_value_cache_coordinator.cpp
  sblr_dml_timeseries_schema_write_coordinator.cpp
  sblr_ddl_alter_rewrite_rule_coordinator.cpp
  sblr_ddl_drop_trigger_coordinator.cpp
  sblr_aggregate_coordinator.cpp
  sblr_group_coordinator.cpp
  sblr_sort_coordinator.cpp
  sblr_limit_coordinator.cpp
  sblr_window_coordinator.cpp
  sblr_return_result_set_coordinator.cpp
  sblr_sequence_currval_coordinator.cpp
)

if(NOT TARGET sb_cluster_provider)
  if(SB_ENABLE_CLUSTER_PROVIDER AND SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY)
    add_library(sb_cluster_provider UNKNOWN IMPORTED GLOBAL)
    set_target_properties(sb_cluster_provider PROPERTIES
      IMPORTED_LOCATION "${SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY}"
      INTERFACE_COMPILE_DEFINITIONS SCRATCHBIRD_CLUSTER_PROVIDER_EXTERNAL=1
      INTERFACE_LINK_LIBRARIES sb_engine_internal_api
    )
    target_include_directories(sb_cluster_provider INTERFACE
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/src/engine/sblr
    )
    if(SB_CLUSTER_PROVIDER_EXTERNAL_INCLUDE_DIR)
      target_include_directories(sb_cluster_provider INTERFACE
        ${SB_CLUSTER_PROVIDER_EXTERNAL_INCLUDE_DIR}
      )
    endif()
  elseif(SB_ENABLE_CLUSTER_PROVIDER)
    if(SB_CLUSTER_PROVIDER_STUB)
      add_subdirectory(${CMAKE_SOURCE_DIR}/src/cluster_provider_stub ${CMAKE_CURRENT_BINARY_DIR}/cluster_provider_stub)
    else()
      message(FATAL_ERROR "SB_ENABLE_CLUSTER_PROVIDER=ON requires SB_CLUSTER_PROVIDER_STUB=ON or SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY=/path/to/lib")
    endif()
  else()
    add_subdirectory(${CMAKE_SOURCE_DIR}/src/cluster_provider ${CMAKE_CURRENT_BINARY_DIR}/cluster_provider)
  endif()
endif()

target_include_directories(sb_engine_internal_api PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(sb_engine_internal_api PUBLIC cxx_std_20)

find_package(OpenSSL REQUIRED)

target_link_libraries(sb_engine_internal_api PUBLIC
  sb_sblr_parameter_wire_codec
  sb_core_datatypes
  sb_core_hash
  sb_core_agents
  sb_core_memory
  sb_core_metrics
  sb_core_resources
  sb_core_uuid
  sb_core_index
  sb_core_catalog
  sb_core_bulk_load
  sb_storage_filespace
  sb_storage_page
  sb_storage_database
  sb_engine_typed_scalar_descriptor_contract
  sb_engine_executor
  sb_engine_native_compile
  sb_engine_gpu_acceleration
  sb_engine_optimizer_contract_only
  sb_udr_runtime
  OpenSSL::Crypto
)

if(TARGET sb_cluster_provider)
  target_link_libraries(sb_engine_internal_api PRIVATE sb_cluster_provider)
endif()

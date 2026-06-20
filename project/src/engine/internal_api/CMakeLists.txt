# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

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
  catalog/ipar_fast_path_support.cpp
  catalog/name_registry.cpp
  catalog/name_resolution_api.cpp
  catalog/pinned_descriptor_cache.cpp
  catalog/schema_tree_api.cpp
  catalog/sbsql_language_elements_catalog.cpp
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
  dml/dml_index_write_path.cpp
  dml/dml_operation_path_proof.cpp
  dml/dml_row_locator_stream.cpp
  dml/dml_target_access_plan.cpp
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

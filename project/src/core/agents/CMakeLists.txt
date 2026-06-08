# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET sb_core_uuid)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../uuid" "${CMAKE_CURRENT_BINARY_DIR}/core_uuid")
endif()
if(NOT TARGET sb_core_memory)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../memory" "${CMAKE_CURRENT_BINARY_DIR}/core_memory")
endif()
if(NOT TARGET sb_core_datatypes)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../datatypes" "${CMAKE_CURRENT_BINARY_DIR}/core_datatypes")
endif()
if(NOT TARGET sb_storage_disk)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../storage/disk" "${CMAKE_CURRENT_BINARY_DIR}/storage_disk")
endif()
if(NOT TARGET sb_transaction_mga)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../transaction/mga" "${CMAKE_CURRENT_BINARY_DIR}/transaction_mga")
endif()
if(NOT TARGET sb_storage_page)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../storage/page" "${CMAKE_CURRENT_BINARY_DIR}/storage_page")
endif()
find_package(OpenSSL REQUIRED)

if(NOT TARGET sb_core_resource_governance)
  add_library(sb_core_resource_governance
    resource_governance_admission.cpp
  )
  target_compile_features(sb_core_resource_governance PUBLIC cxx_std_17)
  target_include_directories(sb_core_resource_governance PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(sb_core_resource_governance PUBLIC sb_core_platform sb_core_metrics)
endif()

add_library(sb_core_agents
  agent_action_dispatch.cpp
  agent_action_safety.cpp
  agent_commercial_evidence.cpp
  agent_cluster_projection_guard.cpp
  agent_durable_catalog.cpp
  agent_enterprise_evidence.cpp
  agent_local_workflow.cpp
  agent_runtime_service.cpp
  agent_runtime_manifest.cpp
  agent_production_classification.cpp
  agent_system_profile.cpp
  agent_production_fixture_separation.cpp
  agent_metric_runtime.cpp
  agent_policy_schema.cpp
  agent_policy_lifecycle.cpp
  agent_policy_override_resolution.cpp
  agent_rollout_profile.cpp
  agent_execution_lane_governance.cpp
  agent_manual_approval.cpp
  agent_dependency_lifecycle.cpp
  agent_tenant_coordination.cpp
  agent_package_provenance.cpp
  agent_replay_quarantine.cpp
  agent_optimizer_recommendation.cpp
  agent_policy_recommendation_application.cpp
  agent_runtime.cpp
  agent_memory_coupling.cpp
  agent_runtime_manager.cpp
  agent_engine_lifecycle.cpp
  agent_feature_gates.cpp
  agent_background_jobs.cpp
  online_maintenance_progress.cpp
  dynamic_cleanup_debt_scheduler.cpp
  agent_workload_resource_quota.cpp
  optimized_path_resource_governance.cpp
  adaptive_batch_policy_evidence.cpp
  adaptive_tuning_controller.cpp
  vector_maintenance_jobs.cpp
  agents/node_resource_agent.cpp
  agents/metrics_registry_manager.cpp
  agents/storage_health_manager.cpp
  agents/filespace_capacity_manager.cpp
  agents/page_allocation_manager.cpp
  agents/memory_governor.cpp
  agents/index_health_manager.cpp
  agents/cluster_autoscale_manager.cpp
  agents/admission_control_manager.cpp
  agents/parser_interface_manager.cpp
  agents/transaction_pressure_manager.cpp
  agents/storage_version_cleanup_agent.cpp
  agents/nosql_family_maintenance_agent.cpp
  agents/nosql_backpressure_debt_agent.cpp
  agents/index_garbage_cleanup_agent.cpp
  agents/shadow_index_build_agent.cpp
  agents/cleanup_archive_manager.cpp
  agents/policy_recommendation_manager.cpp
  agents/distributed_query_metrics_agent.cpp
  agents/remote_query_routing_agent.cpp
  agents/runtime_learning_agent.cpp
  agents/support_bundle_triage_agent.cpp
  agents/cluster_scheduler_manager.cpp
  agents/job_control_manager.cpp
  agents/backup_manager.cpp
  agents/archive_manager.cpp
  agents/restore_drill_manager.cpp
  agents/pitr_manager.cpp
  agents/identity_manager.cpp
  agents/session_control_manager.cpp
  agents/alert_manager.cpp
  agents/export_adapter_manager.cpp
  agents/cluster_upgrade_manager.cpp
)

target_compile_features(sb_core_agents PUBLIC cxx_std_17)
target_include_directories(sb_core_agents PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sb_core_agents PUBLIC
  sb_core_resource_governance
  sb_core_memory
  sb_core_metrics
  sb_core_uuid
  sb_core_index
  sb_storage_page
  sb_transaction_mga
  OpenSSL::Crypto
)

if(TARGET sb_cluster_provider AND NOT TARGET sb_core_agents_cluster_boundary)
  add_library(sb_core_agents_cluster_boundary
    agent_cluster_boundary.cpp
  )
  target_compile_features(sb_core_agents_cluster_boundary PUBLIC cxx_std_17)
  target_include_directories(sb_core_agents_cluster_boundary PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(sb_core_agents_cluster_boundary PUBLIC sb_core_agents)
  target_link_libraries(sb_core_agents_cluster_boundary PRIVATE sb_cluster_provider)
endif()

# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SB_PRIVATE_REPO_ROOT "${PROJECT_SOURCE_DIR}/..")
set(SB_AGENT_RUNTIME_STATIC_GATES
  "${CMAKE_CURRENT_SOURCE_DIR}/agent_runtime_static_gates.py"
)

# SEARCH_KEY: AEIC_PER_AGENT_ROUTE_TESTS

function(sb_agent_runtime_static_gate test_name gate_mode gate_labels)
  add_test(
    NAME ${test_name}
    COMMAND "${Python3_EXECUTABLE}" "${SB_AGENT_RUNTIME_STATIC_GATES}"
            --repo-root "${SB_PRIVATE_REPO_ROOT}"
            --mode "${gate_mode}"
  )
  set_tests_properties(${test_name} PROPERTIES
    LABELS "${test_name};agents;agent_runtime;${gate_labels}"
  )
endfunction()

sb_agent_runtime_static_gate(
  agent_no_implicit_defaults_cross_reference_gate
  no-implicit-defaults
  "PFAR-000A;cross_reference"
)

sb_agent_runtime_static_gate(
  agent_management_framework_gate
  management-framework
  "PFAR-000B;management_framework"
)

sb_agent_runtime_static_gate(
  agent_responsibility_index_gate
  responsibility-index
  "PFAR-000B;responsibility_index"
)

sb_agent_runtime_static_gate(
  agent_implementation_anchor_search_key_gate
  implementation-anchors
  "PFAR-001;implementation_anchors"
)

sb_agent_runtime_static_gate(
  agent_enterprise_production_separation_gate
  enterprise-production-separation
  "AEIC-017;AEIC-030;production_fixture_separation;enterprise_no_anchor"
)

sb_agent_runtime_static_gate(
  agent_enterprise_metrics_resource_audit_gate
  enterprise-metrics-resource-audit
  "AEIC-015;strict_metric_snapshots;resource_reservations;production_action_audit"
)

sb_agent_runtime_static_gate(
  agent_enterprise_management_surfaces_audit_gate
  enterprise-management-surfaces-audit
  "AEIC-016;durable_management_api;support_bundle;observability_retention"
)

sb_agent_runtime_static_gate(
  agent_enterprise_node_metrics_audit_gate
  enterprise-node-metrics-audit
  "AEIC-020;node_resource_agent;metrics_registry_manager;metric_rollup_export_shed"
)

sb_agent_runtime_static_gate(
  agent_enterprise_storage_agents_audit_gate
  enterprise-storage-agents-audit
  "AEIC-021;storage_health_manager;filespace_capacity_manager;page_allocation_manager"
)

sb_agent_runtime_static_gate(
  agent_enterprise_cleanup_archive_audit_gate
  enterprise-cleanup-archive-audit
  "AEIC-022;storage_version_cleanup_agent;cleanup_archive_manager;mga_cleanup_horizon"
)

sb_agent_runtime_static_gate(
  agent_enterprise_memory_admission_alert_audit_gate
  enterprise-memory-admission-alert-audit
  "AEIC-023;memory_governor;admission_control_manager;alert_manager;strict_metrics;resource_pressure"
)

sb_agent_runtime_static_gate(
  agent_enterprise_index_agents_audit_gate
  enterprise-index-agents-audit
  "AEIC-024;index_health_manager;index_garbage_cleanup_agent;shadow_index_build_agent"
)

sb_agent_runtime_static_gate(
  agent_enterprise_transaction_optimizer_policy_audit_gate
  enterprise-transaction-optimizer-policy-audit
  "AEIC-025;transaction_pressure_manager;runtime_learning_agent;policy_recommendation_manager;optimizer_recommendation"
)

sb_agent_runtime_static_gate(
  agent_enterprise_parser_support_audit_gate
  enterprise-parser-support-audit
  "AEIC-026;parser_interface_manager;support_bundle_triage_agent;parser_lifecycle;support_bundle"
)

sb_agent_runtime_static_gate(
  agent_enterprise_nosql_route_audit_gate
  enterprise-nosql-route-audit
  "AEIC-027;nosql_family_maintenance_agent;nosql_backpressure_debt_agent;nosql_engine_routes"
)

sb_agent_runtime_static_gate(
  agent_enterprise_operational_backup_audit_gate
  enterprise-operational-backup-audit
  "AEIC-028;backup_manager;archive_manager;restore_drill_manager;pitr_manager;export_adapter_manager;backup_archive_routes"
)

sb_agent_runtime_static_gate(
  agent_enterprise_identity_session_job_audit_gate
  enterprise-identity-session-job-audit
  "AEIC-029;identity_manager;session_control_manager;job_control_manager;security_session_job_routes"
)

sb_agent_runtime_static_gate(
  agent_enterprise_per_agent_route_matrix_gate
  enterprise-per-agent-route-matrix
  "AEIC-040;per_agent_route_tests;live_or_exact_refusal"
)

sb_agent_runtime_static_gate(
  agent_manifest_drift_gate
  manifest-drift
  "AEIC-002;AEIC-051;manifest_generation;documentation_drift"
)

sb_agent_runtime_static_gate(
  agent_enterprise_substrate_refactor_gate
  enterprise-substrate-refactor
  "AEIC-050;substrate_refactor;runtime_modules;behavior_drift"
)

add_custom_target(pfar_standalone_test_coverage_gate
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/pfar_standalone_test_coverage_gate.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
  COMMENT "Validating PFAR standalone CTest coverage registrations"
)
add_test(
  NAME pfar_standalone_test_coverage_gate
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/pfar_standalone_test_coverage_gate.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
)
set_tests_properties(pfar_standalone_test_coverage_gate PROPERTIES
  LABELS "pfar_standalone_test_coverage_gate;PFAR-018;agents;agent_runtime;standalone_test_coverage;mga_transaction_regression"
)

if(NOT TARGET sb_core_platform)
  add_subdirectory("${PROJECT_SOURCE_DIR}/src/core/platform" "${CMAKE_CURRENT_BINARY_DIR}/core_platform")
endif()
if(NOT TARGET sb_core_metrics)
  add_subdirectory("${PROJECT_SOURCE_DIR}/src/core/metrics" "${CMAKE_CURRENT_BINARY_DIR}/core_metrics")
endif()
if(NOT TARGET sb_core_agents)
  add_subdirectory("${PROJECT_SOURCE_DIR}/src/core/agents" "${CMAKE_CURRENT_BINARY_DIR}/core_agents")
endif()
if(NOT TARGET sb_engine_internal_api)
  add_subdirectory("${PROJECT_SOURCE_DIR}/src/engine/internal_api" "${CMAKE_CURRENT_BINARY_DIR}/engine_internal_api")
endif()
if(NOT TARGET sb_engine_optimizer_contract_only)
  add_subdirectory("${PROJECT_SOURCE_DIR}/src/engine/optimizer" "${CMAKE_CURRENT_BINARY_DIR}/engine_optimizer")
endif()
if(NOT TARGET sb_engine_sblr)
  add_subdirectory("${PROJECT_SOURCE_DIR}/src/engine/sblr" "${CMAKE_CURRENT_BINARY_DIR}/engine_sblr")
endif()
if(NOT TARGET sbl_sbsql_parser_worker_core)
  add_subdirectory("${PROJECT_SOURCE_DIR}/src/parsers/sbsql_worker" "${CMAKE_CURRENT_BINARY_DIR}/sbsql_worker")
endif()
if(NOT TARGET sb_storage_page)
  add_subdirectory("${PROJECT_SOURCE_DIR}/src/storage/page" "${CMAKE_CURRENT_BINARY_DIR}/storage_page")
endif()
if(NOT TARGET sb_core_agents_cluster_boundary)
  add_library(sb_core_agents_cluster_boundary
    "${PROJECT_SOURCE_DIR}/src/core/agents/agent_cluster_boundary.cpp"
  )
  target_compile_features(sb_core_agents_cluster_boundary PUBLIC cxx_std_17)
  target_include_directories(sb_core_agents_cluster_boundary PUBLIC
    "${PROJECT_SOURCE_DIR}/src/core/agents"
  )
  target_link_libraries(sb_core_agents_cluster_boundary PUBLIC sb_core_agents)
  target_link_libraries(sb_core_agents_cluster_boundary PRIVATE sb_cluster_provider)
endif()

function(sb_agent_runtime_cpp_gate test_name source_file gate_labels)
  add_executable(${test_name} ${source_file})
  target_compile_features(${test_name} PRIVATE cxx_std_17)
  target_compile_definitions(${test_name} PRIVATE
    SB_PRIVATE_REPO_ROOT="${SB_PRIVATE_REPO_ROOT}"
  )
  target_link_libraries(${test_name} PRIVATE sb_core_agents sb_core_agents_cluster_boundary)
  if(TARGET sb_cluster_provider)
    target_link_libraries(${test_name} PRIVATE sb_cluster_provider)
  endif()
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES
    LABELS "${test_name};agents;agent_runtime;${gate_labels}"
  )
endfunction()

sb_agent_runtime_cpp_gate(
  agent_runtime_manifest_consistency_gate
  agent_runtime_manifest_consistency_gate.cpp
  "ARHC-000;ARHC-001;ARHC-010;ARHC-011;manifest_consistency"
)

sb_agent_runtime_cpp_gate(
  agent_durable_runtime_catalog_gate
  agent_durable_runtime_catalog_gate.cpp
  "ARHC-020;ARHC-021;ARHC-022;ARHC-030;durable_catalog;runtime_service;leases"
)

add_executable(agent_enterprise_durable_catalog_store_gate
  agent_enterprise_durable_catalog_store_gate.cpp)
target_compile_features(agent_enterprise_durable_catalog_store_gate PRIVATE cxx_std_20)
target_link_libraries(agent_enterprise_durable_catalog_store_gate PRIVATE
  sb_engine_internal_api
)
add_test(NAME agent_enterprise_durable_catalog_store_gate
  COMMAND agent_enterprise_durable_catalog_store_gate)
set_tests_properties(agent_enterprise_durable_catalog_store_gate PROPERTIES
  LABELS "agent_enterprise_durable_catalog_store_gate;AEIC-010;AEIC-011;AEIC-014;agents;agent_runtime;mga_catalog_store;mga_transaction_regression"
)

add_executable(agent_runtime_service_store_gate
  agent_runtime_service_store_gate.cpp)
target_compile_features(agent_runtime_service_store_gate PRIVATE cxx_std_20)
target_link_libraries(agent_runtime_service_store_gate PRIVATE
  sb_engine_internal_api
)
add_test(NAME agent_runtime_service_store_gate
  COMMAND agent_runtime_service_store_gate)
set_tests_properties(agent_runtime_service_store_gate PROPERTIES
  LABELS "agent_runtime_service_store_gate;AEIC-011;AEIC-012;agents;agent_runtime;mga_catalog_store;runtime_service;leases;mga_transaction_regression"
)

add_executable(agent_action_dispatch_store_gate
  agent_action_dispatch_store_gate.cpp)
target_compile_features(agent_action_dispatch_store_gate PRIVATE cxx_std_20)
target_link_libraries(agent_action_dispatch_store_gate PRIVATE
  sb_engine_internal_api
)
add_test(NAME agent_action_dispatch_store_gate
  COMMAND agent_action_dispatch_store_gate)
set_tests_properties(agent_action_dispatch_store_gate PROPERTIES
  LABELS "agent_action_dispatch_store_gate;AEIC-010;AEIC-013;AEIC-014;AEIC-041;AEIC-042;agents;agent_runtime;mga_catalog_store;actuator_provider;action_dispatch;security_negative;crash_replay_concurrency;mga_transaction_regression"
)

add_executable(agent_local_workflow_store_gate
  agent_local_workflow_store_gate.cpp)
target_compile_features(agent_local_workflow_store_gate PRIVATE cxx_std_20)
target_link_libraries(agent_local_workflow_store_gate PRIVATE
  sb_engine_internal_api
)
add_test(NAME agent_local_workflow_store_gate
  COMMAND agent_local_workflow_store_gate)
set_tests_properties(agent_local_workflow_store_gate PROPERTIES
  LABELS "agent_local_workflow_store_gate;AEIC-010;AEIC-012;AEIC-014;AEIC-028;AEIC-029;agents;agent_runtime;mga_catalog_store;local_workflow;mga_transaction_regression"
)

add_executable(agent_enterprise_decision_store_gate
  agent_enterprise_decision_store_gate.cpp)
target_compile_features(agent_enterprise_decision_store_gate PRIVATE cxx_std_20)
target_link_libraries(agent_enterprise_decision_store_gate PRIVATE
  sb_engine_internal_api
)
add_test(NAME agent_enterprise_decision_store_gate
  COMMAND agent_enterprise_decision_store_gate)
set_tests_properties(agent_enterprise_decision_store_gate PROPERTIES
  LABELS "agent_enterprise_decision_store_gate;AEIC-010;AEIC-014;AEIC-020;AEIC-024;AEIC-025;AEIC-026;agents;agent_runtime;mga_catalog_store;enterprise_decision;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_action_dispatch_idempotency_gate
  agent_action_dispatch_idempotency_gate.cpp
  "ARHC-031;ARHC-040;ARHC-041;authority_provenance;actuator_provider;idempotent_dispatch"
)

sb_agent_runtime_cpp_gate(
  agent_metric_resource_runtime_gate
  agent_metric_resource_runtime_gate.cpp
  "ARHC-050;ARHC-051;strict_metric_snapshots;resource_reservations;foreground_protection"
)

sb_agent_runtime_cpp_gate(
  agent_production_exposure_gate
  agent_production_exposure_gate.cpp
  "ARHC-060;ARHC-061;ARHC-062;ARHC-063;production_exposure;agent_route_truthfulness"
)

sb_agent_runtime_cpp_gate(
  agent_system_profile_claim_gate
  agent_system_profile_claim_gate.cpp
  "CEIC-070;AgentSystemProfile;durable_claim_levels;production_live_claims;cluster_external_provider_only;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_policy_lifecycle_gate
  agent_policy_lifecycle_gate.cpp
  "CEIC-071;signed_typed_policy_lifecycle;policy_schema;policy_signature;policy_approval;cluster_external_provider_only;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_policy_override_resolution_gate
  agent_policy_override_resolution_gate.cpp
  "CEIC-072;policy_override_resolution;policy_inheritance;cluster_external_provider_only;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_execution_lane_governance_gate
  agent_execution_lane_governance_gate.cpp
  "CEIC-073;execution_lanes;slo_governance;cost_governance;cluster_external_provider_only;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_metric_quorum_source_attestation_gate
  agent_metric_quorum_source_attestation_gate.cpp
  "CEIC-074;metric_quorum;source_attestation;redaction_provenance;cluster_external_provider_only;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_action_safety_rollout_gate
  agent_action_safety_rollout_gate.cpp
  "CEIC-075;agent_action_safety;rollout_modes;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_approval_break_glass_workflow_gate
  agent_approval_break_glass_workflow_gate.cpp
  "CEIC-076;approval_workflow;break_glass;notification_escalation;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_commercial_evidence_gate
  agent_commercial_evidence_gate.cpp
  "ARHC-070;ARHC-071;commercial_evidence;redaction_retention_tamper"
)

sb_agent_runtime_cpp_gate(
  agent_evidence_key_privacy_tamper_chain_gate
  agent_evidence_key_privacy_tamper_chain_gate.cpp
  "CEIC-077;evidence_key_management;privacy;tamper_chain;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_dependency_lifecycle_dr_restore_clone_gate
  agent_dependency_lifecycle_dr_restore_clone_gate.cpp
  "CEIC-078;dependency_lifecycle;dr_restore_clone;cluster_external_provider_only;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_tenant_coordination_gate
  agent_tenant_coordination_gate.cpp
  "CEIC-079;tenant_workload_isolation;agent_coordination;cluster_external_provider_only;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_package_plugin_actuator_provenance_gate
  agent_package_plugin_actuator_provenance_gate.cpp
  "CEIC-080;agent_package_provenance;plugin_signing;actuator_signing;sbom;sandbox;cluster_external_provider_only;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_replay_compensation_quarantine_gate
  agent_replay_compensation_quarantine_gate.cpp
  "CEIC-081;deterministic_replay;retry;compensation;quarantine;cluster_external_provider_only;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_security_crash_fixture_hardening_gate
  agent_security_crash_fixture_hardening_gate.cpp
  "ARHC-080;ARHC-081;ARHC-082;AEIC-042;security_negative;crash_restart_race;production_fixture_separation;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_policy_schema_gate
  agent_policy_schema_gate.cpp
  "ARHC-090;ARHC-091;substrate_refactor;typed_policy_schema;mga_transaction_regression"
)

add_executable(agent_management_route_evidence_gate
  agent_management_route_evidence_gate.cpp)
target_compile_features(agent_management_route_evidence_gate PRIVATE cxx_std_17)
target_link_libraries(agent_management_route_evidence_gate PRIVATE
  sb_engine_internal_api
)
if(TARGET sb_cluster_provider)
  target_link_libraries(agent_management_route_evidence_gate PRIVATE sb_cluster_provider)
endif()
add_test(NAME agent_management_route_evidence_gate
  COMMAND agent_management_route_evidence_gate)
set_tests_properties(agent_management_route_evidence_gate PROPERTIES
  LABELS "agent_management_route_evidence_gate;ARHC-092;agents;agent_runtime;management_route_evidence;mga_transaction_regression"
)

add_executable(agent_optimizer_recommendation_evidence_gate
  agent_optimizer_recommendation_evidence_gate.cpp)
target_compile_features(agent_optimizer_recommendation_evidence_gate PRIVATE cxx_std_20)
target_link_libraries(agent_optimizer_recommendation_evidence_gate PRIVATE
  sb_engine_optimizer_contract_only
)
add_test(NAME agent_optimizer_recommendation_evidence_gate
  COMMAND agent_optimizer_recommendation_evidence_gate)
set_tests_properties(agent_optimizer_recommendation_evidence_gate PROPERTIES
  LABELS "agent_optimizer_recommendation_evidence_gate;ARHC-093;agents;agent_runtime;optimizer_recommendation_evidence;mga_transaction_regression"
)

add_executable(agent_index_optimizer_boundary_gate
  agent_index_optimizer_boundary_gate.cpp)
target_compile_features(agent_index_optimizer_boundary_gate PRIVATE cxx_std_20)
target_link_libraries(agent_index_optimizer_boundary_gate PRIVATE
  sb_engine_optimizer_contract_only
)
add_test(NAME agent_index_optimizer_boundary_gate
  COMMAND agent_index_optimizer_boundary_gate)
set_tests_properties(agent_index_optimizer_boundary_gate PROPERTIES
  LABELS "agent_index_optimizer_boundary_gate;CEIC-084;agents;agent_runtime;index_optimizer_boundary;optimizer_recommendation_evidence;mga_transaction_regression"
)

add_executable(agent_management_durable_mutation_gate
  agent_management_durable_mutation_gate.cpp)
target_compile_features(agent_management_durable_mutation_gate PRIVATE cxx_std_17)
target_link_libraries(agent_management_durable_mutation_gate PRIVATE
  sb_engine_internal_api
)
if(TARGET sb_cluster_provider)
  target_link_libraries(agent_management_durable_mutation_gate PRIVATE sb_cluster_provider)
endif()
add_test(NAME agent_management_durable_mutation_gate
  COMMAND agent_management_durable_mutation_gate)
set_tests_properties(agent_management_durable_mutation_gate PROPERTIES
  LABELS "agent_management_durable_mutation_gate;ARHC-042;agents;agent_runtime;durable_management_api;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_runtime_lifecycle_gate
  agent_runtime_lifecycle_gate.cpp
  "PFAR-002;runtime_manager"
)

sb_agent_runtime_cpp_gate(
  noncluster_agent_runtime_matrix_gate
  noncluster_agent_runtime_matrix_gate.cpp
  "PFAR-002A;noncluster_agent_matrix"
)

sb_agent_runtime_cpp_gate(
  noncluster_agent_tick_health_gate
  noncluster_agent_tick_health_gate.cpp
  "PFAR-006A;noncluster_agent_tick_health"
)

sb_agent_runtime_cpp_gate(
  agent_action_contract_matrix_gate
  agent_action_contract_matrix_gate.cpp
  "PFAR-006B;action_contract_matrix"
)

sb_agent_runtime_cpp_gate(
  agent_metric_dependency_fail_closed_gate
  agent_metric_dependency_fail_closed_gate.cpp
  "PFAR-006C;metric_dependency_fail_closed"
)

sb_agent_runtime_cpp_gate(
  agent_metric_dependency_contract_gate
  agent_metric_dependency_contract_gate.cpp
  "PFAR-006G;metric_dependency_contract"
)

sb_agent_runtime_cpp_gate(
  agent_resource_budget_backpressure_gate
  agent_resource_budget_backpressure_gate.cpp
  "PFAR-006D;resource_budget_backpressure"
)

sb_agent_runtime_cpp_gate(
  cdp030_agent_worker_capacity_gate
  cdp030_agent_worker_capacity_gate.cpp
  "CDP-030;worker_capacity;foreground_reserved_capacity;dml_prework"
)

sb_agent_runtime_cpp_gate(
  cdp031_agent_resource_governor_gate
  cdp031_agent_resource_governor_gate.cpp
  "CDP-031;resource_governor;foreground_protection;quota_backpressure"
)

sb_agent_runtime_cpp_gate(
  agent_memory_coupling_hardening_gate
  agent_memory_coupling_hardening_gate.cpp
  "MMCH-084;MMCH-085;MMCH-086;MMCH-087;MMCH-088;MMCH-089;memory_coupled_agents"
)

sb_agent_runtime_cpp_gate(
  agent_memory_pressure_metric_integration_gate
  agent_memory_pressure_metric_integration_gate.cpp
  "CEIC-083;agent_memory_pressure;trusted_metrics;memory_reservations;cluster_external_provider_only;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_enterprise_local_resource_agents_gate
  agent_enterprise_local_resource_agents_gate.cpp
  "AEIC-020;AEIC-023;local_resource_agents;enterprise_no_anchor"
)

sb_agent_runtime_cpp_gate(
  agent_enterprise_storage_agents_gate
  agent_enterprise_storage_agents_gate.cpp
  "AEIC-021;storage_health_manager;filespace_capacity_manager;page_allocation_manager;enterprise_storage_agents"
)

sb_agent_runtime_cpp_gate(
  agent_enterprise_cleanup_archive_agents_gate
  agent_enterprise_cleanup_archive_agents_gate.cpp
  "AEIC-022;storage_version_cleanup_agent;cleanup_archive_manager;mga_cleanup_horizon"
)

sb_agent_runtime_cpp_gate(
  agent_enterprise_memory_admission_alert_gate
  agent_enterprise_memory_admission_alert_gate.cpp
  "AEIC-023;memory_governor;admission_control_manager;alert_manager;strict_metrics;resource_pressure"
)

sb_agent_runtime_cpp_gate(
  agent_enterprise_soak_performance_gate
  agent_enterprise_soak_performance_gate.cpp
  "AEIC-043;ELER-081;ELER-GATE-081;ELER-102;ELER-GATE-102;soak;high_concurrency;performance_overhead;p50;p95;p99;queue_depth;action_backlog;memory_pressure;mga_transaction_regression;linux_proof"
)

sb_agent_runtime_cpp_gate(
  agent_enterprise_index_agents_gate
  agent_enterprise_index_agents_gate.cpp
  "AEIC-024;index_health_manager;index_garbage_cleanup_agent;shadow_index_build_agent;enterprise_index_agents"
)

add_executable(agent_enterprise_transaction_optimizer_policy_gate
  agent_enterprise_transaction_optimizer_policy_gate.cpp)
target_compile_features(agent_enterprise_transaction_optimizer_policy_gate PRIVATE cxx_std_20)
target_compile_definitions(agent_enterprise_transaction_optimizer_policy_gate PRIVATE
  SB_PRIVATE_REPO_ROOT="${SB_PRIVATE_REPO_ROOT}"
)
target_link_libraries(agent_enterprise_transaction_optimizer_policy_gate PRIVATE
  sb_engine_optimizer_contract_only
)
add_test(NAME agent_enterprise_transaction_optimizer_policy_gate
  COMMAND agent_enterprise_transaction_optimizer_policy_gate)
set_tests_properties(agent_enterprise_transaction_optimizer_policy_gate PROPERTIES
  LABELS "agent_enterprise_transaction_optimizer_policy_gate;AEIC-025;agents;agent_runtime;transaction_pressure_manager;runtime_learning_agent;policy_recommendation_manager;optimizer_recommendation"
)

add_executable(agent_enterprise_parser_support_gate
  agent_enterprise_parser_support_gate.cpp)
target_compile_features(agent_enterprise_parser_support_gate PRIVATE cxx_std_20)
target_compile_definitions(agent_enterprise_parser_support_gate PRIVATE
  SB_PRIVATE_REPO_ROOT="${SB_PRIVATE_REPO_ROOT}"
)
target_link_libraries(agent_enterprise_parser_support_gate PRIVATE
  sb_engine_internal_api
  sbl_sbsql_parser_worker_core
)
add_test(NAME agent_enterprise_parser_support_gate
  COMMAND agent_enterprise_parser_support_gate)
set_tests_properties(agent_enterprise_parser_support_gate PROPERTIES
  LABELS "agent_enterprise_parser_support_gate;AEIC-026;agents;agent_runtime;parser_interface_manager;support_bundle_triage_agent;parser_lifecycle;support_bundle"
)

sb_agent_runtime_cpp_gate(
  agent_enterprise_advisory_agents_gate
  agent_enterprise_advisory_agents_gate.cpp
  "AEIC-024;AEIC-025;AEIC-026;advisory_agents;enterprise_no_anchor"
)

sb_agent_runtime_cpp_gate(
  agent_enterprise_operational_agents_gate
  agent_enterprise_operational_agents_gate.cpp
  "AEIC-028;AEIC-029;operational_agents;enterprise_no_anchor"
)

sb_agent_runtime_cpp_gate(
  agent_enterprise_nosql_agents_gate
  agent_enterprise_nosql_agents_gate.cpp
  "AEIC-027;nosql_agents;enterprise_internal_helper_route"
)

add_executable(agent_enterprise_nosql_route_gate
  agent_enterprise_nosql_route_gate.cpp)
target_compile_features(agent_enterprise_nosql_route_gate PRIVATE cxx_std_20)
target_compile_definitions(agent_enterprise_nosql_route_gate PRIVATE
  SB_PRIVATE_REPO_ROOT="${SB_PRIVATE_REPO_ROOT}"
)
target_link_libraries(agent_enterprise_nosql_route_gate PRIVATE
  sb_engine_internal_api
)
add_test(NAME agent_enterprise_nosql_route_gate
  COMMAND agent_enterprise_nosql_route_gate)
set_tests_properties(agent_enterprise_nosql_route_gate PROPERTIES
  LABELS "agent_enterprise_nosql_route_gate;AEIC-027;agents;agent_runtime;nosql_family_maintenance_agent;nosql_backpressure_debt_agent;nosql_engine_routes;mga_transaction_regression"
)

add_executable(agent_enterprise_identity_session_job_route_gate
  agent_enterprise_identity_session_job_route_gate.cpp)
target_compile_features(agent_enterprise_identity_session_job_route_gate PRIVATE cxx_std_20)
target_compile_definitions(agent_enterprise_identity_session_job_route_gate PRIVATE
  SB_PRIVATE_REPO_ROOT="${SB_PRIVATE_REPO_ROOT}"
)
target_link_libraries(agent_enterprise_identity_session_job_route_gate PRIVATE
  sb_engine_internal_api
  sb_server_core
)
add_test(NAME agent_enterprise_identity_session_job_route_gate
  COMMAND agent_enterprise_identity_session_job_route_gate)
set_tests_properties(agent_enterprise_identity_session_job_route_gate PROPERTIES
  LABELS "agent_enterprise_identity_session_job_route_gate;AEIC-029;agents;agent_runtime;identity_manager;session_control_manager;job_control_manager;security_session_job_routes;mga_transaction_regression"
)

add_executable(dpc_agent_scheduler_fairness_starvation_gate
  dpc_agent_scheduler_fairness_starvation_gate.cpp
  "${PROJECT_SOURCE_DIR}/src/server/server_agent_runtime.cpp"
)
target_compile_features(dpc_agent_scheduler_fairness_starvation_gate PRIVATE cxx_std_23)
target_include_directories(dpc_agent_scheduler_fairness_starvation_gate PRIVATE
  "${PROJECT_SOURCE_DIR}/src/server"
)
target_link_libraries(dpc_agent_scheduler_fairness_starvation_gate PRIVATE
  sb_core_agents
  sb_core_agents_cluster_boundary
  sb_engine_internal_api
)
if(TARGET sb_cluster_provider)
  target_link_libraries(dpc_agent_scheduler_fairness_starvation_gate PRIVATE sb_cluster_provider)
endif()
add_test(NAME dpc_agent_scheduler_fairness_starvation_gate
  COMMAND dpc_agent_scheduler_fairness_starvation_gate)
set_tests_properties(dpc_agent_scheduler_fairness_starvation_gate PROPERTIES
  LABELS "dpc_agent_scheduler_fairness_starvation_gate;DPC-036;agents;agent_runtime;scheduler_fairness;foreground_protection;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_security_grant_matrix_gate
  agent_security_grant_matrix_gate.cpp
  "PFAR-006E;security_grant_matrix"
)

sb_agent_runtime_cpp_gate(
  agent_runtime_uuid_authority_gate
  agent_runtime_uuid_authority_gate.cpp
  "PFAR-016AA;uuid_authority;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_action_actuator_authority_gate
  agent_action_actuator_authority_gate.cpp
  "PFAR-006F;action_actuator_authority"
)

sb_agent_runtime_cpp_gate(
  agent_cluster_provider_boundary_gate
  agent_cluster_provider_boundary_gate.cpp
  "PFAR-002B;cluster_provider_boundary"
)

sb_agent_runtime_cpp_gate(
  agent_cluster_provider_build_matrix_gate
  agent_cluster_provider_build_matrix_gate.cpp
  "PFAR-002B;cluster_provider_build_matrix"
)

add_executable(agent_cluster_route_api_provider_gate
  agent_cluster_route_api_provider_gate.cpp)
target_compile_features(agent_cluster_route_api_provider_gate PRIVATE cxx_std_17)
target_link_libraries(agent_cluster_route_api_provider_gate PRIVATE
  sb_engine_internal_api
  sb_engine_sblr
)
if(TARGET sb_cluster_provider)
  target_link_libraries(agent_cluster_route_api_provider_gate PRIVATE sb_cluster_provider)
endif()
add_test(NAME agent_cluster_route_api_provider_gate
  COMMAND agent_cluster_route_api_provider_gate)
set_tests_properties(agent_cluster_route_api_provider_gate PROPERTIES
  LABELS "agent_cluster_route_api_provider_gate;agents;agent_runtime;PFAR-013A;cluster_provider_boundary;sblr_agent_management_route_gate;mga_transaction_regression"
)

add_executable(agent_management_authorization_policy_gate
  agent_management_authorization_policy_gate.cpp)
target_compile_features(agent_management_authorization_policy_gate PRIVATE cxx_std_17)
target_link_libraries(agent_management_authorization_policy_gate PRIVATE
  sb_engine_internal_api
  sb_engine_sblr
)
if(TARGET sb_cluster_provider)
  target_link_libraries(agent_management_authorization_policy_gate PRIVATE sb_cluster_provider)
endif()
add_test(NAME agent_management_authorization_policy_gate
  COMMAND agent_management_authorization_policy_gate)
set_tests_properties(agent_management_authorization_policy_gate PROPERTIES
  LABELS "agent_management_authorization_policy_gate;agents;agent_runtime;PFAR-014;AEIC-042;management_authorization_policy;security_negative;sblr_agent_management_route_gate;mga_transaction_regression"
)

add_executable(agent_third_party_management_request_gate
  agent_third_party_management_request_gate.cpp)
target_compile_features(agent_third_party_management_request_gate PRIVATE cxx_std_17)
target_link_libraries(agent_third_party_management_request_gate PRIVATE
  sb_engine_internal_api
)
if(TARGET sb_cluster_provider)
  target_link_libraries(agent_third_party_management_request_gate PRIVATE sb_cluster_provider)
endif()
add_test(NAME agent_third_party_management_request_gate
  COMMAND agent_third_party_management_request_gate)
set_tests_properties(agent_third_party_management_request_gate PROPERTIES
  LABELS "agent_third_party_management_request_gate;agents;agent_runtime;PFAR-016B;AEIC-042;third_party_management_request;observability;security;security_negative;redaction;supportability;mga_transaction_regression"
)

add_executable(agent_zero_grey_output_contract_gate
  agent_zero_grey_output_contract_gate.cpp)
target_compile_features(agent_zero_grey_output_contract_gate PRIVATE cxx_std_17)
target_link_libraries(agent_zero_grey_output_contract_gate PRIVATE
  sb_engine_internal_api
)
if(TARGET sb_cluster_provider)
  target_link_libraries(agent_zero_grey_output_contract_gate PRIVATE sb_cluster_provider)
endif()
add_test(NAME agent_zero_grey_output_contract_gate
  COMMAND agent_zero_grey_output_contract_gate)
set_tests_properties(agent_zero_grey_output_contract_gate PROPERTIES
  LABELS "agent_zero_grey_output_contract_gate;PFAR-016C;agents;agent_runtime;observability;supportability;mga_transaction_regression"
)

add_executable(agent_open_state_mode_api_part
  agent_open_state_mode_gate.cpp)
target_compile_features(agent_open_state_mode_api_part PRIVATE cxx_std_17)
target_link_libraries(agent_open_state_mode_api_part PRIVATE
  sb_core_agents
  sb_engine_internal_api
  sb_engine_sblr
)
if(TARGET sb_cluster_provider)
  target_link_libraries(agent_open_state_mode_api_part PRIVATE sb_cluster_provider)
endif()

add_executable(agent_open_state_mode_runtime_part
  agent_open_state_mode_runtime_gate_part.cpp)
target_compile_features(agent_open_state_mode_runtime_part PRIVATE cxx_std_17)
target_link_libraries(agent_open_state_mode_runtime_part PRIVATE
  sb_engine_internal_api
)

add_custom_target(agent_open_state_mode_gate
  DEPENDS agent_open_state_mode_api_part agent_open_state_mode_runtime_part)
add_test(NAME agent_open_state_mode_gate
  COMMAND "${CMAKE_COMMAND}"
          -DOPEN_STATE_API_PART=$<TARGET_FILE:agent_open_state_mode_api_part>
          -DOPEN_STATE_RUNTIME_PART=$<TARGET_FILE:agent_open_state_mode_runtime_part>
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_agent_open_state_mode_gate.cmake")
set_tests_properties(agent_open_state_mode_gate PROPERTIES
  LABELS "agent_open_state_mode_gate;PFAR-017A;agents;agent_runtime;open_state_modes;management_authorization_policy;mga_transaction_regression"
)

add_executable(embedded_no_background_agent_gate
  embedded_no_background_agent_gate.cpp)
target_compile_features(embedded_no_background_agent_gate PRIVATE cxx_std_17)
target_link_libraries(embedded_no_background_agent_gate PRIVATE
  sb_engine_internal_api
)
add_test(NAME embedded_no_background_agent_gate
  COMMAND embedded_no_background_agent_gate)
set_tests_properties(embedded_no_background_agent_gate PROPERTIES
  LABELS "embedded_no_background_agent_gate;PFAR-017;agents;agent_runtime;embedded_direct_mode;mga_transaction_regression"
)

sb_agent_runtime_cpp_gate(
  agent_policy_config_bootstrap_gate
  agent_policy_config_bootstrap_gate.cpp
  "PFAR-002C;policy_config_bootstrap"
)

sb_agent_runtime_cpp_gate(
  agent_lifecycle_state_machine_gate
  agent_lifecycle_state_machine_gate.cpp
  "PFAR-002D;lifecycle_state_machine"
)

sb_agent_runtime_cpp_gate(
  agent_policy_attachment_baseline_gate
  agent_policy_attachment_baseline_gate.cpp
  "PFAR-002E;policy_attachment_baseline"
)

sb_agent_runtime_cpp_gate(
  agent_cluster_leadership_boundary_gate
  agent_cluster_leadership_boundary_gate.cpp
  "PFAR-002F;cluster_leadership_boundary"
)

sb_agent_runtime_cpp_gate(
  agent_instance_identity_persistence_gate
  agent_instance_identity_persistence_gate.cpp
  "PFAR-003A;instance_identity_persistence"
)

sb_agent_runtime_cpp_gate(
  agent_runtime_shutdown_recovery_gate
  agent_runtime_shutdown_recovery_gate.cpp
  "PFAR-003;shutdown_recovery"
)

sb_agent_runtime_cpp_gate(
  agent_supervision_failure_quarantine_gate
  agent_supervision_failure_quarantine_gate.cpp
  "PFAR-003B;supervision_failure_quarantine"
)

sb_agent_runtime_cpp_gate(
  agent_arbitration_override_suppression_gate
  agent_arbitration_override_suppression_gate.cpp
  "PFAR-003C;arbitration_override_suppression"
)

sb_agent_runtime_cpp_gate(
  agent_fault_injection_gate
  agent_fault_injection_gate.cpp
  "PFAR-003D;fault_injection;partial_failure_safety"
)

add_executable(page_filespace_agent_handoff_gate page_filespace_agent_handoff_gate.cpp)
target_compile_features(page_filespace_agent_handoff_gate PRIVATE cxx_std_17)
target_link_libraries(page_filespace_agent_handoff_gate PRIVATE sb_storage_page)
add_test(NAME page_filespace_agent_handoff_gate COMMAND page_filespace_agent_handoff_gate)
set_tests_properties(page_filespace_agent_handoff_gate PROPERTIES
  LABELS "page_filespace_agent_handoff_gate;agents;agent_runtime;PFAR-004;PFAR-005;durable_request_queue;handoff_queue_integration"
)

add_executable(page_allocation_manager_live_gate page_allocation_manager_live_gate.cpp)
target_compile_features(page_allocation_manager_live_gate PRIVATE cxx_std_17)
target_link_libraries(page_allocation_manager_live_gate PRIVATE sb_core_agents sb_storage_page)
add_test(NAME page_allocation_manager_live_gate COMMAND page_allocation_manager_live_gate)
set_tests_properties(page_allocation_manager_live_gate PROPERTIES
  LABELS "page_allocation_manager_live_gate;agents;agent_runtime;PFAR-006;page_allocation_manager;active_loop"
)

add_executable(page_allocation_manager_direct_preallocation_gate
  page_allocation_manager_direct_preallocation_gate.cpp)
target_compile_features(page_allocation_manager_direct_preallocation_gate PRIVATE cxx_std_17)
target_link_libraries(page_allocation_manager_direct_preallocation_gate PRIVATE sb_core_agents sb_storage_page)
add_test(NAME page_allocation_manager_direct_preallocation_gate
  COMMAND page_allocation_manager_direct_preallocation_gate)
set_tests_properties(page_allocation_manager_direct_preallocation_gate PROPERTIES
  LABELS "page_allocation_manager_direct_preallocation_gate;agents;agent_runtime;PFAR-007;page_allocation_manager;direct_preallocation"
)

add_executable(page_allocation_preallocation_preference_gate
  page_allocation_preallocation_preference_gate.cpp)
target_compile_features(page_allocation_preallocation_preference_gate PRIVATE cxx_std_17)
target_link_libraries(page_allocation_preallocation_preference_gate PRIVATE sb_core_agents sb_storage_page)
add_test(NAME page_allocation_preallocation_preference_gate
  COMMAND page_allocation_preallocation_preference_gate)
set_tests_properties(page_allocation_preallocation_preference_gate PROPERTIES
  LABELS "page_allocation_preallocation_preference_gate;PFAR-011;agents;agent_runtime;page_allocation_manager;mga_transaction_regression"
)

add_executable(filespace_capacity_manager_live_gate
  filespace_capacity_manager_live_gate.cpp)
target_compile_features(filespace_capacity_manager_live_gate PRIVATE cxx_std_17)
target_link_libraries(filespace_capacity_manager_live_gate PRIVATE sb_core_agents sb_storage_page)
add_test(NAME filespace_capacity_manager_live_gate
  COMMAND filespace_capacity_manager_live_gate)
set_tests_properties(filespace_capacity_manager_live_gate PROPERTIES
  LABELS "filespace_capacity_manager_live_gate;PFAR-008;agents;agent_runtime;filespace_capacity_manager;active_loop"
)

add_executable(filespace_capacity_manager_authority_gate
  filespace_capacity_manager_authority_gate.cpp)
target_compile_features(filespace_capacity_manager_authority_gate PRIVATE cxx_std_17)
target_link_libraries(filespace_capacity_manager_authority_gate PRIVATE sb_core_agents sb_storage_page)
add_test(NAME filespace_capacity_manager_authority_gate
  COMMAND filespace_capacity_manager_authority_gate)
set_tests_properties(filespace_capacity_manager_authority_gate PROPERTIES
  LABELS "filespace_capacity_manager_authority_gate;PFAR-008A;agents;agent_runtime;filespace_capacity_manager;authority"
)

add_executable(storage_health_manager_authority_gate
  storage_health_manager_authority_gate.cpp)
target_compile_features(storage_health_manager_authority_gate PRIVATE cxx_std_17)
target_link_libraries(storage_health_manager_authority_gate PRIVATE sb_core_agents)
add_test(NAME storage_health_manager_authority_gate
  COMMAND storage_health_manager_authority_gate)
set_tests_properties(storage_health_manager_authority_gate PROPERTIES
  LABELS "storage_health_manager_authority_gate;PFAR-008B;agents;agent_runtime;storage_health_manager;authority"
)

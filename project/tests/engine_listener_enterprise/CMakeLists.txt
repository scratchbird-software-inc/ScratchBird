# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(NOT TARGET sb_storage_disk)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/storage/disk
                   ${CMAKE_CURRENT_BINARY_DIR}/src_storage_disk)
endif()
if(NOT TARGET sb_storage_page)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/storage/page
                   ${CMAKE_CURRENT_BINARY_DIR}/src_storage_page)
endif()
if(NOT TARGET sb_storage_filespace)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/storage/filespace
                   ${CMAKE_CURRENT_BINARY_DIR}/src_storage_filespace)
endif()

include_directories(
  ${PROJECT_SOURCE_DIR}/src/listener
  ${PROJECT_SOURCE_DIR}/src/manager/protocol
)

set(SB_ENGINE_LISTENER_ENTERPRISE_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/engine_listener_enterprise_gate.py"
)

if(NOT SB_BUILD_DATABASE_LIFECYCLE_TESTS)
  include("${PROJECT_SOURCE_DIR}/tests/database_lifecycle/ReleaseProvenanceSbomLicenseGate.cmake")
  sb_add_database_lifecycle_release_provenance_sbom_license_gate()
endif()

function(sb_add_engine_listener_enterprise_gate test_name gate_mode gate_labels)
  add_test(
    NAME "${test_name}"
    COMMAND "${Python3_EXECUTABLE}" "${SB_ENGINE_LISTENER_ENTERPRISE_GATE}"
            --project-root "${PROJECT_SOURCE_DIR}"
            --build-root "${CMAKE_BINARY_DIR}"
            --mode "${gate_mode}"
            --output "${CMAKE_CURRENT_BINARY_DIR}/${test_name}.json"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "${gate_labels};engine_listener_enterprise;release_proof_substrate"
  )
endfunction()

add_custom_target(engine_listener_enterprise_foundation_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_ENGINE_LISTENER_ENTERPRISE_GATE}"
          --project-root "${PROJECT_SOURCE_DIR}"
          --build-root "${CMAKE_BINARY_DIR}"
          --mode all
          --output "${CMAKE_CURRENT_BINARY_DIR}/engine_listener_enterprise_foundation_summary.json"
  COMMENT "Validating engine/listener enterprise foundation release gates"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_prior_execution_plan_semantics_gate
  prior-execution_plan-semantics
  "ELER-001;ELER-GATE-002;prior_completion_semantics;no_file_presence_completion"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_declared_feature_inventory_gate
  declared-feature-inventory
  "ELER-002;ELER-GATE-001;declared_feature_inventory;generated_matrix"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_anti_skeleton_classifier_gate
  anti-skeleton-classifier
  "ELER-003;ELER-GATE-002;anti_skeleton_classifier;no_skip_waiver"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_project_tests_proof_location_gate
  project-tests-proof-location
  "ELER-004;ELER-GATE-003;project_tests_only_proof;public_tree_inputs"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_platform_matrix_gate
  platform-matrix
  "ELER-005;ELER-GATE-004;platform_matrix;linux;windows;freebsd;native_runner_pending;no_platform_overclaim;linux_proof"
)
set_tests_properties(engine_listener_platform_matrix_gate PROPERTIES
  DEPENDS "public_platform_matrix_gate;engine_listener_storage_io_conformance;engine_listener_storage_io_platform_source_gate;engine_listener_management_envelope_conformance;engine_listener_ipv6_dual_stack_transport_conformance;engine_listener_parser_pool_lifecycle_conformance;engine_listener_tls_channel_binding_conformance;engine_listener_owner_lifecycle_artifact_conformance;engine_listener_windows_transport_source_gate;engine_listener_bsd_peer_owner_management_gate;engine_listener_owner_lifecycle_platform_source_gate;public_crypto_entropy_policy_gate;public_install_service_hardening_gate;engine_listener_no_skip_waiver_xfail_release_gate;public_no_skip_waiver_xfail_gate"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_native_platform_handoff_gate
  native-platform-handoff
  "ELER-005;ELER-GATE-004;ELER-090;ELER-GATE-090;ELER-112;ELER-GATE-112;native_platform_handoff;windows;freebsd;native_runner_pending;cross_platform_pending;no_platform_overclaim;project_tests_only;linux_proof"
)
set_tests_properties(engine_listener_native_platform_handoff_gate PROPERTIES
  DEPENDS "engine_listener_platform_matrix_gate;public_platform_matrix_gate;engine_listener_no_skip_waiver_xfail_release_gate;public_no_skip_waiver_xfail_gate"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_native_platform_proof_package_import_gate
  native-platform-proof-package-import
  "ELER-005;ELER-GATE-004;ELER-090;ELER-GATE-090;ELER-112;ELER-GATE-112;native_platform_proof_package_import;windows;freebsd;native_runner_pending;cross_platform_pending;no_platform_overclaim;project_tests_only;linux_manager_import_contract;linux_proof"
)
set_tests_properties(engine_listener_native_platform_proof_package_import_gate PROPERTIES
  DEPENDS "engine_listener_native_platform_handoff_gate;engine_listener_no_skip_waiver_xfail_release_gate;public_no_skip_waiver_xfail_gate"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_no_skip_waiver_xfail_release_gate
  no-skip-waiver-xfail-release-proof
  "ELER-083;ELER-GATE-083;no_skip_waiver_xfail_completion;declared_feature_result_classifier;linux_proof"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_row_level_traceability_manifest_gate
  row-level-traceability-manifest
  "ELER-084;ELER-GATE-084;row_level_traceability_manifest;declared_feature_traceability;reference_sblr_traceability;linux_proof"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_crash_fault_campaign_gate
  crash-fault-campaign
  "ELER-080;ELER-GATE-080;ELER-100;ELER-GATE-100;cross_cutting_crash_fault_campaign;crash_recovery;fault_injection;fail_closed;linux_proof"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_integrated_product_proof_gate
  integrated-product-proof
  "ELER-100;ELER-GATE-100;integrated_product_proof;full_stack_product_path;listener_engine_sblr_mga;linux_proof"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_crash_recovery_certification_gate
  crash-recovery-certification
  "ELER-101;ELER-GATE-101;crash_recovery_certification;critical_transition_matrix;fail_closed;linux_proof"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_adversarial_security_validation_gate
  adversarial-security-validation
  "ELER-103;ELER-GATE-103;adversarial_security_validation;threat_abuse_matrix;fail_closed;stable_diagnostics;linux_proof"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_fuzz_property_invariant_suite_gate
  fuzz-property-invariant-suite
  "ELER-104;ELER-GATE-104;fuzz_property_invariant_suite;listener_engine_boundaries;fail_closed;stable_diagnostics;linux_proof"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_performance_scalability_baseline_gate
  performance-scalability-baseline
  "ELER-105;ELER-GATE-105;performance_scalability_baseline;latency_percentiles;runtime_health_counters;resource_high_water;benchmark_clean;linux_proof"
)
set_tests_properties(engine_listener_performance_scalability_baseline_gate PROPERTIES
  DEPENDS "scratchbird_beta_performance_baseline_conformance"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_soak_certification_gate
  soak-certification
  "ELER-081;ELER-GATE-081;ELER-102;ELER-GATE-102;long_duration_soak;bounded_soak;high_concurrency;memory_pressure;crash_fault;security_negative;artifact_retention;linux_proof"
)
set_tests_properties(engine_listener_soak_certification_gate PROPERTIES
  DEPENDS "public_release_soak_lane;public_release_soak_gate;memory_sanitizer_soak_concurrency_gate;dpc_soak_leak_resource_stability_gate;agent_enterprise_soak_performance_gate;ceic_093_reliability_security_suite_gate"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_compatibility_upgrade_downgrade_gate
  compatibility-upgrade-downgrade
  "ELER-106;ELER-GATE-106;compatibility_upgrade_downgrade;format_version_policy;protocol_version_policy;downgrade_refusal;cluster_stub_boundary;linux_proof"
)
set_tests_properties(engine_listener_compatibility_upgrade_downgrade_gate PROPERTIES
  DEPENDS "public_upgrade_migration_gate;database_lifecycle_upgrade_migration_conformance;database_lifecycle_protocol_versioning_conformance;optimizer_enterprise_plan_cache_gate"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_parser_contract_freeze_gate
  parser-facing-contract-freeze
  "ELER-085;ELER-GATE-085;ELER-106;ELER-GATE-106;parser_facing_contract_freeze;versioned_parser_contract;compatibility_version_policy;linux_proof"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_generated_resource_determinism_gate
  generated-resource-determinism
  "ELER-086;ELER-GATE-086;generated_resource_determinism;fixture_hash_manifest;two_pass_generation;linux_proof"
)
set_tests_properties(engine_listener_generated_resource_determinism_gate PROPERTIES
  RESOURCE_LOCK generated_resource_determinism_workdir
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_sbsql_parser_sync_gate
  sbsql-parser-spec-execution_plan-sync
  "ELER-089;ELER-GATE-089;sbsql_parser_spec_execution_plan_sync;row_expanded_sync;compatibility_dialect_paste_through_rejection;linux_proof"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_release_profile_completeness_gate
  release-profile-completeness
  "ELER-087;ELER-GATE-087;release_profile;configuration_completeness;production_build;secure_defaults;cluster_boundary;linux_proof"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_operational_readiness_gate
  operational-readiness
  "ELER-107;ELER-GATE-107;operational_readiness;health_readiness_liveness;diagnostics;supportability;linux_proof"
)

set_tests_properties(engine_listener_operational_readiness_gate PROPERTIES
  DEPENDS "engine_listener_management_envelope_conformance;engine_listener_graceful_drain_stop_conformance;engine_listener_support_bundle_conformance;database_lifecycle_observability_conformance;database_lifecycle_supportability_evidence_conformance;database_lifecycle_maintenance_repair_conformance"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_release_artifact_trust_gate
  release-artifact-trust
  "ELER-108;ELER-GATE-108;release_artifact_trust;provenance;sbom;vulnerability_scan;signature_ready;reproducible_build;debug_symbols;cluster_stub_boundary;linux_proof"
)
set_tests_properties(engine_listener_release_artifact_trust_gate PROPERTIES
  DEPENDS "public_release_attestation_gate;public_artifact_reproducibility_gate;public_artifact_signature_gate;public_dependency_sbom_gate;public_platform_matrix_gate;public_sanitizer_static_analysis_gate;public_release_version_metadata_gate;public_crypto_entropy_policy_gate;public_generated_provenance_gate;public_install_service_hardening_gate;public_admin_runbook_gate;public_api_abi_compat_gate;public_cluster_provider_boundary_cleanup_gate;public_cluster_boundary_cleanup_audit_gate;database_lifecycle_release_provenance_sbom_license_gate"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_enterprise_documentation_gate
  enterprise-documentation
  "ELER-109;ELER-GATE-109;enterprise_documentation;runbooks;trust_boundaries;architecture;operations;support_bundle;upgrade;backup_restore;limitations;support_states;linux_proof"
)
set_tests_properties(engine_listener_enterprise_documentation_gate PROPERTIES
  DEPENDS "public_enterprise_documentation_gate;public_doc_consistency_gate;public_admin_runbook_gate;public_api_abi_compat_gate"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_support_maintenance_policy_gate
  support-maintenance-policy
  "ELER-111;ELER-GATE-111;support_policy;maintenance_policy;security_update;cve;disclosure;patch_release;compatibility;data_loss_escalation;rollback;diagnostics;sla;linux_proof"
)
set_tests_properties(engine_listener_support_maintenance_policy_gate PROPERTIES
  DEPENDS "public_support_policy_gate;public_support_bundle_incident_gate;public_disaster_recovery_gate;public_audit_privacy_gate;public_api_abi_compat_gate"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_project_only_implementation_proof_gate
  project-only-implementation-proof
  "ELER-082;ELER-GATE-082;final_project_only_implementation_proof;project_tests_only;no_private_dependencies;reference_sblr_closure;sbsql_sync;parser_contract_freeze;deterministic_resources;linux_proof"
)
set_tests_properties(engine_listener_project_only_implementation_proof_gate PROPERTIES
  DEPENDS "engine_listener_no_skip_waiver_xfail_release_gate;engine_listener_row_level_traceability_manifest_gate;engine_listener_parser_contract_freeze_gate;engine_listener_generated_resource_determinism_gate;engine_listener_sbsql_parser_sync_gate;engine_listener_release_profile_completeness_gate;engine_listener_support_bundle_redaction_gate;public_project_export_gate;sblr_surface_reference_interface_closure_gate;sblr_surface_sbsql_sync_guardrail_gate;sblr_surface_function_registry_dispatch_conformance"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_external_review_closure_package_gate
  external-review-closure-package
  "ELER-110;ELER-GATE-110;external_review_closure_package;security_review;database_correctness_review;operational_reliability_review;reference_sblr_parser_boundary_review;cluster_boundary_review;external_validation_pending;linux_proof"
)
set_tests_properties(engine_listener_external_review_closure_package_gate PROPERTIES
  DEPENDS "engine_listener_adversarial_security_validation_gate;engine_listener_fuzz_property_invariant_suite_gate;engine_listener_crash_recovery_certification_gate;engine_listener_operational_readiness_gate;engine_listener_release_artifact_trust_gate;engine_listener_enterprise_documentation_gate;engine_listener_support_maintenance_policy_gate;engine_listener_parser_contract_freeze_gate;engine_listener_sbsql_parser_sync_gate;sblr_surface_reference_interface_closure_gate;sblr_surface_sbsql_sync_guardrail_gate;public_cluster_provider_boundary_cleanup_gate;public_cluster_boundary_cleanup_audit_gate"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_gold_enterprise_readiness_gate
  gold-enterprise-readiness
  "ELER-112;ELER-GATE-112;linux_gold_enterprise_release_candidate;enterprise_readiness_aggregation;cross_platform_pending;external_zero_issue_audit_pending;no_product_completion_claim;linux_proof"
)
set_tests_properties(engine_listener_gold_enterprise_readiness_gate PROPERTIES
  DEPENDS "engine_listener_project_only_implementation_proof_gate;engine_listener_external_review_closure_package_gate;engine_listener_integrated_product_proof_gate;engine_listener_crash_recovery_certification_gate;engine_listener_soak_certification_gate;engine_listener_performance_scalability_baseline_gate;engine_listener_compatibility_upgrade_downgrade_gate;engine_listener_operational_readiness_gate;engine_listener_release_artifact_trust_gate;engine_listener_enterprise_documentation_gate;engine_listener_support_maintenance_policy_gate;engine_listener_native_platform_handoff_gate;engine_listener_native_platform_proof_package_import_gate;public_platform_matrix_gate"
)

sb_add_engine_listener_enterprise_gate(
  engine_listener_third_audit_evidence_package_gate
  third-audit-evidence-package
  "ELER-090;ELER-GATE-090;third_audit_evidence_package;audit_ready;external_zero_issue_audit_pending;project_tests_only;cross_platform_pending;linux_proof"
)
set_tests_properties(engine_listener_third_audit_evidence_package_gate PROPERTIES
  DEPENDS "engine_listener_gold_enterprise_readiness_gate;engine_listener_project_only_implementation_proof_gate;engine_listener_external_review_closure_package_gate;engine_listener_no_skip_waiver_xfail_release_gate;engine_listener_generated_resource_determinism_gate;public_project_export_gate"
)

if(NOT WIN32)
  add_executable(engine_listener_dbbt_lpreface_binding_conformance
    engine_listener_dbbt_lpreface_binding_conformance.cpp
  )
  target_compile_features(engine_listener_dbbt_lpreface_binding_conformance PRIVATE cxx_std_23)
  target_link_libraries(engine_listener_dbbt_lpreface_binding_conformance PRIVATE
    sbl_listener_runtime
  )
  add_test(
    NAME engine_listener_dbbt_lpreface_binding_conformance
    COMMAND engine_listener_dbbt_lpreface_binding_conformance
  )
  set_tests_properties(engine_listener_dbbt_lpreface_binding_conformance PROPERTIES
    LABELS "ELER-060;ELER-GATE-060;ELER-100;ELER-GATE-100;ELER-101;ELER-GATE-101;ELER-103;ELER-GATE-103;ELER-104;ELER-GATE-104;ELER-105;ELER-GATE-105;ELER-106;ELER-GATE-106;engine_listener_enterprise;listener;dbbt;lpreface;exact_client_binding;fifo_theft_refusal;handoff_race;handoff_binding_property;listener_accept_to_handoff_latency;listener_control_protocol_version;linux_proof"
  )

  add_executable(engine_listener_management_envelope_conformance
    engine_listener_management_envelope_conformance.cpp
  )
  target_compile_features(engine_listener_management_envelope_conformance PRIVATE cxx_std_23)
  target_link_libraries(engine_listener_management_envelope_conformance PRIVATE
    sbl_listener_runtime
  )
  if(TARGET sb_listener AND TARGET sb_parser_dummy)
    add_dependencies(engine_listener_management_envelope_conformance sb_listener sb_parser_dummy)
    add_test(
      NAME engine_listener_management_envelope_conformance
      COMMAND engine_listener_management_envelope_conformance
              $<TARGET_FILE:sb_listener>
              $<TARGET_FILE:sb_parser_dummy>
    )
    set_tests_properties(engine_listener_management_envelope_conformance PROPERTIES
      LABELS "ELER-061;ELER-GATE-061;ELER-080;ELER-GATE-080;ELER-100;ELER-GATE-100;ELER-101;ELER-GATE-101;ELER-103;ELER-GATE-103;ELER-104;ELER-GATE-104;ELER-106;ELER-GATE-106;ELER-107;ELER-GATE-107;engine_listener_enterprise;listener;management;sbme;peer_owner;replay;rights;management_protocol_compatibility;management_socket_abuse;control_frame_fuzz_property;operational_readiness;linux_proof"
    )
  endif()
endif()

add_test(
  NAME engine_listener_bsd_peer_owner_management_gate
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/engine_listener_bsd_peer_owner_management_gate.py"
          --project-root "${PROJECT_SOURCE_DIR}"
)
set_tests_properties(engine_listener_bsd_peer_owner_management_gate PROPERTIES
  LABELS "ELER-061;ELER-GATE-061;ELER-065;ELER-GATE-065;ELER-103;ELER-GATE-103;ELER-107;ELER-GATE-107;engine_listener_enterprise;listener;management;peer_owner;bsd;freebsd;platform_transport;fail_closed;linux_source_proof"
)

add_test(
  NAME engine_listener_windows_transport_source_gate
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/engine_listener_windows_transport_source_gate.py"
          --project-root "${PROJECT_SOURCE_DIR}"
)
set_tests_properties(engine_listener_windows_transport_source_gate PROPERTIES
  LABELS "ELER-061;ELER-GATE-061;ELER-062;ELER-GATE-062;ELER-065;ELER-GATE-065;ELER-103;ELER-GATE-103;ELER-107;ELER-GATE-107;engine_listener_enterprise;listener;management;windows;winsock;platform_transport;parser_handoff_duplicate_socket;mingw_cross_compile;linux_source_proof"
)

add_test(
  NAME engine_listener_owner_lifecycle_platform_source_gate
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/engine_listener_owner_lifecycle_platform_source_gate.py"
          --project-root "${PROJECT_SOURCE_DIR}"
)
set_tests_properties(engine_listener_owner_lifecycle_platform_source_gate PROPERTIES
  LABELS "ELER-067;ELER-GATE-067;ELER-103;ELER-GATE-103;ELER-107;ELER-GATE-107;engine_listener_enterprise;listener;owner_lifecycle;windows;bsd;durable_artifacts;mingw_cross_compile;linux_source_proof"
)

if(NOT WIN32)
  add_executable(engine_listener_parser_pool_lifecycle_conformance
    engine_listener_parser_pool_lifecycle_conformance.cpp
  )
  target_compile_features(engine_listener_parser_pool_lifecycle_conformance PRIVATE cxx_std_23)
  target_link_libraries(engine_listener_parser_pool_lifecycle_conformance PRIVATE
    sbl_listener_runtime
  )
  if(TARGET sb_listener AND TARGET sb_parser_dummy)
    add_dependencies(engine_listener_parser_pool_lifecycle_conformance sb_listener sb_parser_dummy)
    add_test(
      NAME engine_listener_parser_pool_lifecycle_conformance
      COMMAND engine_listener_parser_pool_lifecycle_conformance
              $<TARGET_FILE:sb_listener>
              $<TARGET_FILE:sb_parser_dummy>
    )
    set_tests_properties(engine_listener_parser_pool_lifecycle_conformance PROPERTIES
      LABELS "ELER-062;ELER-GATE-062;ELER-103;ELER-GATE-103;ELER-105;ELER-GATE-105;ELER-107;ELER-GATE-107;engine_listener_enterprise;listener;parser_pool;on_demand;restart_backoff;quarantine;fault_history;parser_worker_compromise;pool_exhaustion_pressure;operational_readiness;linux_proof"
    )
  endif()

  add_executable(engine_listener_ipv6_dual_stack_transport_conformance
    engine_listener_ipv6_dual_stack_transport_conformance.cpp
  )
  target_compile_features(engine_listener_ipv6_dual_stack_transport_conformance PRIVATE cxx_std_23)
  target_link_libraries(engine_listener_ipv6_dual_stack_transport_conformance PRIVATE
    sbl_listener_runtime
  )
  if(TARGET sb_listener AND TARGET sb_parser_dummy)
    add_dependencies(engine_listener_ipv6_dual_stack_transport_conformance sb_listener sb_parser_dummy)
    add_test(
      NAME engine_listener_ipv6_dual_stack_transport_conformance
      COMMAND engine_listener_ipv6_dual_stack_transport_conformance
              $<TARGET_FILE:sb_listener>
              $<TARGET_FILE:sb_parser_dummy>
    )
    set_tests_properties(engine_listener_ipv6_dual_stack_transport_conformance PROPERTIES
      LABELS "ELER-065;ELER-GATE-065;ELER-100;ELER-GATE-100;ELER-101;ELER-GATE-101;ELER-104;ELER-GATE-104;ELER-105;ELER-GATE-105;ELER-107;ELER-GATE-107;engine_listener_enterprise;listener;ipv6;dual_stack;network_transport;parser_handoff;management_status;linux_proof"
    )
  endif()

  add_executable(engine_listener_graceful_drain_stop_conformance
    engine_listener_graceful_drain_stop_conformance.cpp
  )
  target_compile_features(engine_listener_graceful_drain_stop_conformance PRIVATE cxx_std_23)
  target_link_libraries(engine_listener_graceful_drain_stop_conformance PRIVATE
    sbl_listener_runtime
  )
  if(TARGET sb_listener AND TARGET sb_parser_dummy)
    add_dependencies(engine_listener_graceful_drain_stop_conformance sb_listener sb_parser_dummy)
    add_test(
      NAME engine_listener_graceful_drain_stop_conformance
      COMMAND engine_listener_graceful_drain_stop_conformance
              $<TARGET_FILE:sb_listener>
              $<TARGET_FILE:sb_parser_dummy>
    )
    set_tests_properties(engine_listener_graceful_drain_stop_conformance PROPERTIES
      LABELS "ELER-064;ELER-GATE-064;ELER-101;ELER-GATE-101;ELER-107;ELER-GATE-107;engine_listener_enterprise;listener;drain;graceful_stop;active_worker_accounting;timeout;operational_readiness;linux_proof"
    )
  endif()
endif()

if(NOT WIN32)
  add_executable(engine_listener_owner_lifecycle_artifact_conformance
    engine_listener_owner_lifecycle_artifact_conformance.cpp
  )
  target_compile_features(engine_listener_owner_lifecycle_artifact_conformance PRIVATE cxx_std_23)
  target_link_libraries(engine_listener_owner_lifecycle_artifact_conformance PRIVATE
    sbl_listener_runtime
  )
  add_test(
    NAME engine_listener_owner_lifecycle_artifact_conformance
    COMMAND engine_listener_owner_lifecycle_artifact_conformance
  )
  set_tests_properties(engine_listener_owner_lifecycle_artifact_conformance PROPERTIES
    LABELS "ELER-067;ELER-GATE-067;ELER-080;ELER-GATE-080;ELER-104;ELER-GATE-104;ELER-107;ELER-GATE-107;engine_listener_enterprise;listener;owner_token;lifecycle_file;durable_artifact;signature;permissions;lifecycle_file_property;operational_readiness;linux_proof"
  )
endif()

if(NOT WIN32)
  add_executable(engine_listener_support_bundle_conformance
    engine_listener_support_bundle_conformance.cpp
  )
  target_compile_features(engine_listener_support_bundle_conformance PRIVATE cxx_std_23)
  target_link_libraries(engine_listener_support_bundle_conformance PRIVATE
    sbl_listener_runtime
  )
  if(TARGET sb_listener AND TARGET sb_parser_dummy)
    add_dependencies(engine_listener_support_bundle_conformance
      sb_listener
      sb_parser_dummy
    )
    add_test(
      NAME engine_listener_support_bundle_conformance
      COMMAND engine_listener_support_bundle_conformance
              $<TARGET_FILE:sb_listener>
              $<TARGET_FILE:sb_parser_dummy>
    )
    set_tests_properties(engine_listener_support_bundle_conformance PROPERTIES
      LABELS "ELER-068;ELER-GATE-068;ELER-080;ELER-GATE-080;ELER-100;ELER-GATE-100;ELER-107;ELER-GATE-107;engine_listener_enterprise;listener;support_bundle;fault_history;redaction;management;operational_readiness;linux_proof"
    )
  endif()
endif()

add_executable(engine_listener_support_bundle_redaction_gate
  engine_listener_support_bundle_redaction_gate.cpp
)
target_compile_features(engine_listener_support_bundle_redaction_gate PRIVATE cxx_std_23)
target_link_libraries(engine_listener_support_bundle_redaction_gate PRIVATE
  sbl_listener_runtime
  sb_engine_internal_api
  sb_core_memory
  sb_core_uuid
)
add_test(
  NAME engine_listener_support_bundle_redaction_gate
  COMMAND engine_listener_support_bundle_redaction_gate
          ${CMAKE_CURRENT_BINARY_DIR}/engine_listener_support_bundle_redaction_gate_artifacts/ELER_088_SUPPORT_BUNDLE_SECRET_CANARY_MATRIX.csv
)
set_tests_properties(engine_listener_support_bundle_redaction_gate PROPERTIES
  LABELS "ELER-088;ELER-GATE-088;ELER-103;ELER-GATE-103;ELER-105;ELER-GATE-105;ELER-107;ELER-GATE-107;engine_listener_enterprise;support_bundle;secret_canary;redaction;security;listener;engine;support_bundle_leakage;support_bundle_generation_overhead;operational_readiness;linux_proof"
)

if(TARGET sb_server_core)
  add_executable(engine_listener_tls_channel_binding_conformance
    engine_listener_tls_channel_binding_conformance.cpp
  )
  target_compile_features(engine_listener_tls_channel_binding_conformance PRIVATE cxx_std_23)
  target_link_libraries(engine_listener_tls_channel_binding_conformance PRIVATE
    sbl_listener_runtime
    sb_server_core
  )
  add_test(
    NAME engine_listener_tls_channel_binding_conformance
    COMMAND engine_listener_tls_channel_binding_conformance
  )
  set_tests_properties(engine_listener_tls_channel_binding_conformance PROPERTIES
    LABELS "ELER-066;ELER-GATE-066;ELER-103;ELER-GATE-103;engine_listener_enterprise;listener;tls;channel_binding;downgrade;security;downgrade_attack;linux_proof"
  )

  add_executable(engine_listener_session_binding_takeover_conformance
    engine_listener_session_binding_takeover_conformance.cpp
  )
  target_compile_features(engine_listener_session_binding_takeover_conformance PRIVATE cxx_std_23)
  target_link_libraries(engine_listener_session_binding_takeover_conformance PRIVATE
    sb_server_core
    sbl_listener_control_plane
  )
  add_test(
    NAME engine_listener_session_binding_takeover_conformance
    COMMAND engine_listener_session_binding_takeover_conformance
  )
  set_tests_properties(engine_listener_session_binding_takeover_conformance PROPERTIES
    LABELS "ELER-063;ELER-GATE-063;ELER-103;ELER-GATE-103;engine_listener_enterprise;listener;server;session_binding;takeover;replay;rights;replay_attack;linux_proof"
  )
endif()

add_executable(engine_listener_storage_io_conformance
  engine_listener_storage_io_conformance.cpp
)
target_compile_features(engine_listener_storage_io_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_storage_io_conformance PRIVATE
  sb_storage_disk
  sb_storage_page
)
add_test(
  NAME engine_listener_storage_io_conformance
  COMMAND engine_listener_storage_io_conformance
)
set_tests_properties(engine_listener_storage_io_conformance PROPERTIES
  LABELS "ELER-010;ELER-011;ELER-GATE-010;ELER-GATE-011;ELER-080;ELER-GATE-080;ELER-101;ELER-GATE-101;engine_listener_enterprise;storage;disk;checked_offsets;durable_io;linux_proof"
)

add_test(
  NAME engine_listener_storage_io_platform_source_gate
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/engine_listener_storage_io_platform_source_gate.py"
          --project-root "${PROJECT_SOURCE_DIR}"
)
set_tests_properties(engine_listener_storage_io_platform_source_gate PROPERTIES
  LABELS "ELER-005;ELER-GATE-004;ELER-010;ELER-011;ELER-GATE-010;ELER-GATE-011;ELER-080;ELER-GATE-080;ELER-101;ELER-GATE-101;engine_listener_enterprise;storage;disk;checked_offsets;durable_io;windows;bsd;native_platform_io;mingw_cross_compile;linux_source_proof"
)

add_executable(engine_listener_filespace_growth_conformance
  engine_listener_filespace_growth_conformance.cpp
)
target_compile_features(engine_listener_filespace_growth_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_filespace_growth_conformance PRIVATE
  sb_storage_filespace
)
add_test(
  NAME engine_listener_filespace_growth_conformance
  COMMAND engine_listener_filespace_growth_conformance
)
set_tests_properties(engine_listener_filespace_growth_conformance PROPERTIES
  LABELS "ELER-012;ELER-GATE-011;ELER-080;ELER-GATE-080;ELER-101;ELER-GATE-101;engine_listener_enterprise;storage;filespace;physical_growth;metadata_only_refusal;linux_proof"
)

add_executable(engine_listener_filespace_registry_manifest_conformance
  engine_listener_filespace_registry_manifest_conformance.cpp
)
target_compile_features(engine_listener_filespace_registry_manifest_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_filespace_registry_manifest_conformance PRIVATE
  sb_storage_filespace
  sb_core_memory
)
add_test(
  NAME engine_listener_filespace_registry_manifest_conformance
  COMMAND engine_listener_filespace_registry_manifest_conformance
)
set_tests_properties(engine_listener_filespace_registry_manifest_conformance PROPERTIES
  LABELS "ELER-013;ELER-GATE-011;ELER-080;ELER-GATE-080;ELER-101;ELER-GATE-101;engine_listener_enterprise;storage;filespace;temp_workspace;durable_manifest;checksum;writer_identity;linux_proof"
)

add_executable(engine_listener_page_body_registry_conformance
  engine_listener_page_body_registry_conformance.cpp
)
target_compile_features(engine_listener_page_body_registry_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_page_body_registry_conformance PRIVATE
  sb_storage_page
)
add_test(
  NAME engine_listener_page_body_registry_conformance
  COMMAND engine_listener_page_body_registry_conformance
)
set_tests_properties(engine_listener_page_body_registry_conformance PROPERTIES
  LABELS "ELER-014;ELER-GATE-012;ELER-103;ELER-GATE-103;ELER-104;ELER-GATE-104;engine_listener_enterprise;storage;page_body;structured_body;production_admission;malformed_database_open;catalog_corruption;page_catalog_property;linux_proof"
)

add_executable(engine_listener_allocation_map_page_conformance
  engine_listener_allocation_map_page_conformance.cpp
)
target_compile_features(engine_listener_allocation_map_page_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_allocation_map_page_conformance PRIVATE
  sb_storage_page
)
add_test(
  NAME engine_listener_allocation_map_page_conformance
  COMMAND engine_listener_allocation_map_page_conformance
)
set_tests_properties(engine_listener_allocation_map_page_conformance PROPERTIES
  LABELS "ELER-015;ELER-GATE-012;engine_listener_enterprise;storage;page_body;allocation_map;checksum;mutation;rebuild;linux_proof"
)

add_executable(engine_listener_overflow_blob_page_persistence_conformance
  engine_listener_overflow_blob_page_persistence_conformance.cpp
)
target_compile_features(engine_listener_overflow_blob_page_persistence_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_overflow_blob_page_persistence_conformance PRIVATE
  sb_storage_disk
  sb_storage_page
)
add_test(
  NAME engine_listener_overflow_blob_page_persistence_conformance
  COMMAND engine_listener_overflow_blob_page_persistence_conformance
)
set_tests_properties(engine_listener_overflow_blob_page_persistence_conformance PROPERTIES
  LABELS "ELER-016;ELER-GATE-013;ELER-080;ELER-GATE-080;ELER-100;ELER-GATE-100;ELER-101;ELER-GATE-101;ELER-104;ELER-GATE-104;ELER-106;ELER-GATE-106;engine_listener_enterprise;storage;page_body;overflow;toast;blob;physical_pages;reopen;corruption_refusal;reclaim;toast_format_compatibility;toast_reachability_property;linux_proof"
)

add_executable(engine_listener_checksum_mac_profile_conformance
  engine_listener_checksum_mac_profile_conformance.cpp
)
target_compile_features(engine_listener_checksum_mac_profile_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_checksum_mac_profile_conformance PRIVATE
  sb_storage_page
  sb_core_hash
)
add_test(
  NAME engine_listener_checksum_mac_profile_conformance
  COMMAND engine_listener_checksum_mac_profile_conformance
)
set_tests_properties(engine_listener_checksum_mac_profile_conformance PROPERTIES
  LABELS "ELER-017;ELER-GATE-012;engine_listener_enterprise;storage;page_body;checksum;mac;sha256;hmac_sha256;datatype_descriptor;transaction_inventory;weak_digest_refusal;linux_proof"
)

add_executable(engine_listener_datatype_numeric_runtime_conformance
  engine_listener_datatype_numeric_runtime_conformance.cpp
)
target_compile_features(engine_listener_datatype_numeric_runtime_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_datatype_numeric_runtime_conformance PRIVATE
  sb_engine_internal_api
  sb_storage_database
  sb_core_memory
  sb_core_uuid
)
add_test(
  NAME engine_listener_datatype_numeric_runtime_conformance
  COMMAND engine_listener_datatype_numeric_runtime_conformance
)
set_tests_properties(engine_listener_datatype_numeric_runtime_conformance PROPERTIES
  LABELS "ELER-030;ELER-GATE-030;engine_listener_enterprise;datatypes;exact_numeric;sbl_numeric;sort_key;stable_hash;domain;linux_proof"
)

add_executable(engine_listener_domain_binary_catalog_conformance
  engine_listener_domain_binary_catalog_conformance.cpp
)
target_compile_features(engine_listener_domain_binary_catalog_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_domain_binary_catalog_conformance PRIVATE
  sb_engine_internal_api
  sb_storage_database
  sb_core_memory
  sb_core_uuid
)
add_test(
  NAME engine_listener_domain_binary_catalog_conformance
  COMMAND engine_listener_domain_binary_catalog_conformance
)
set_tests_properties(engine_listener_domain_binary_catalog_conformance PROPERTIES
  LABELS "ELER-031;ELER-GATE-031;ELER-080;ELER-GATE-080;engine_listener_enterprise;domain;binary_catalog;sha256;transaction_binding;crash_replay;tamper_fail_closed;linux_proof"
)

add_executable(engine_listener_materialized_authorization_conformance
  engine_listener_materialized_authorization_conformance.cpp
)
target_compile_features(engine_listener_materialized_authorization_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_materialized_authorization_conformance PRIVATE
  sb_engine_internal_api
  sb_core_uuid
)
add_test(
  NAME engine_listener_materialized_authorization_conformance
  COMMAND engine_listener_materialized_authorization_conformance
)
set_tests_properties(engine_listener_materialized_authorization_conformance PROPERTIES
  LABELS "ELER-032;ELER-GATE-032;ELER-100;ELER-GATE-100;ELER-103;ELER-GATE-103;ELER-104;ELER-GATE-104;engine_listener_enterprise;security;authorization;materialized_context;trace_fallback_refusal;fixture_fence;deny_override;epoch_fail_closed;auth_bypass;privilege_escalation;authorization_property;linux_proof"
)

add_executable(engine_listener_protected_material_durable_catalog_conformance
  engine_listener_protected_material_durable_catalog_conformance.cpp
)
target_compile_features(engine_listener_protected_material_durable_catalog_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_protected_material_durable_catalog_conformance PRIVATE
  sb_engine_internal_api
  sb_core_uuid
)
add_test(
  NAME engine_listener_protected_material_durable_catalog_conformance
  COMMAND engine_listener_protected_material_durable_catalog_conformance
)
set_tests_properties(engine_listener_protected_material_durable_catalog_conformance PROPERTIES
  LABELS "ELER-033;ELER-GATE-033;ELER-080;ELER-GATE-080;ELER-103;ELER-GATE-103;ELER-106;ELER-GATE-106;engine_listener_enterprise;security;protected_material;binary_catalog;sha256;atomic_publish;stale_temp;tamper_fail_closed;security_catalog_format_policy;catalog_corruption;privilege_escalation;linux_proof"
)

add_executable(engine_listener_index_family_dml_route_conformance
  engine_listener_index_family_dml_route_conformance.cpp
)
target_compile_features(engine_listener_index_family_dml_route_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_index_family_dml_route_conformance PRIVATE
  sb_engine_internal_api
  sb_core_index
  sb_core_uuid
)
add_test(
  NAME engine_listener_index_family_dml_route_conformance
  COMMAND engine_listener_index_family_dml_route_conformance
          ${CMAKE_CURRENT_BINARY_DIR}/engine_listener_index_family_dml_route_conformance_artifacts/ELER_040_INDEX_DML_ROUTE_MATRIX.csv
)
set_tests_properties(engine_listener_index_family_dml_route_conformance PROPERTIES
  LABELS "ELER-040;ELER-GATE-040;ELER-080;ELER-GATE-080;ELER-100;ELER-GATE-100;ELER-101;ELER-GATE-101;ELER-104;ELER-GATE-104;ELER-106;ELER-GATE-106;engine_listener_enterprise;index;dml;route_capability;physical_tree;secondary_delta_ledger;metadata_format_policy;maintenance;recovery_reopen;index_authority_property;linux_proof"
)

add_executable(engine_listener_optimizer_integrated_route_conformance
  engine_listener_optimizer_integrated_route_conformance.cpp
)
target_compile_features(engine_listener_optimizer_integrated_route_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_optimizer_integrated_route_conformance PRIVATE
  sb_engine_internal_api
  sb_engine_optimizer_contract_only
  sb_core_uuid
)
add_test(
  NAME engine_listener_optimizer_integrated_route_conformance
  COMMAND engine_listener_optimizer_integrated_route_conformance
          ${CMAKE_CURRENT_BINARY_DIR}/engine_listener_optimizer_integrated_route_conformance_artifacts/ELER_041_OPTIMIZER_INTEGRATED_ROUTE_MATRIX.csv
)
set_tests_properties(engine_listener_optimizer_integrated_route_conformance PROPERTIES
  LABELS "ELER-041;ELER-GATE-041;ELER-100;ELER-GATE-100;ELER-104;ELER-GATE-104;engine_listener_enterprise;optimizer;executor;catalog_backed;physical_plan;plan_cache;feedback;explain;correctness_oracle;optimizer_property;linux_proof"
)

add_executable(engine_listener_agent_production_action_conformance
  engine_listener_agent_production_action_conformance.cpp
)
target_compile_features(engine_listener_agent_production_action_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_agent_production_action_conformance PRIVATE
  sb_engine_internal_api
  sb_core_uuid
)
add_test(
  NAME engine_listener_agent_production_action_conformance
  COMMAND engine_listener_agent_production_action_conformance
          ${CMAKE_CURRENT_BINARY_DIR}/engine_listener_agent_production_action_conformance_artifacts/ELER_042_AGENT_PRODUCTION_ACTION_MATRIX.csv
)
set_tests_properties(engine_listener_agent_production_action_conformance PROPERTIES
  LABELS "ELER-042;ELER-GATE-042;ELER-101;ELER-GATE-101;ELER-104;ELER-GATE-104;engine_listener_enterprise;agents;production_action;durable_catalog;real_actuator;replay;compensation;support_bundle;crash_recovery;agent_record_property;linux_proof"
)

add_executable(engine_listener_memory_integrated_conformance
  engine_listener_memory_integrated_conformance.cpp
)
target_compile_features(engine_listener_memory_integrated_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_memory_integrated_conformance PRIVATE
  sb_core_memory
)
add_test(
  NAME engine_listener_memory_integrated_conformance
  COMMAND engine_listener_memory_integrated_conformance
          ${CMAKE_CURRENT_BINARY_DIR}/engine_listener_memory_integrated_conformance_artifacts/ELER_050_MEMORY_INTEGRATED_MATRIX.csv
)
set_tests_properties(engine_listener_memory_integrated_conformance PROPERTIES
  LABELS "ELER-050;ELER-GATE-050;engine_listener_enterprise;memory;resources;production_startup;query_reservation;pressure_executor;support_bundle;concurrency;linux_proof"
)

add_executable(engine_listener_physical_mga_cow_conformance
  engine_listener_physical_mga_cow_conformance.cpp
)
target_compile_features(engine_listener_physical_mga_cow_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_physical_mga_cow_conformance PRIVATE
  sb_storage_database
)
add_test(
  NAME engine_listener_physical_mga_cow_conformance
  COMMAND engine_listener_physical_mga_cow_conformance
)
set_tests_properties(engine_listener_physical_mga_cow_conformance PROPERTIES
  LABELS "ELER-020;ELER-GATE-020;ELER-080;ELER-GATE-080;ELER-100;ELER-GATE-100;ELER-101;ELER-GATE-101;ELER-104;ELER-GATE-104;engine_listener_enterprise;transaction;mga;cow;row_data;transaction_inventory;rollback;reopen;visibility;mga_visibility_property;linux_proof"
)

add_executable(engine_listener_serializable_isolation_conformance
  engine_listener_serializable_isolation_conformance.cpp
)
target_compile_features(engine_listener_serializable_isolation_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_serializable_isolation_conformance PRIVATE
  sb_transaction_mga
  sb_engine_internal_api
)
add_test(
  NAME engine_listener_serializable_isolation_conformance
  COMMAND engine_listener_serializable_isolation_conformance
)
set_tests_properties(engine_listener_serializable_isolation_conformance PROPERTIES
  LABELS "ELER-021;ELER-GATE-021;engine_listener_enterprise;transaction;mga;serializable;predicate_range_tracking;phantom_refusal;retry_classification;linux_proof"
)

add_executable(engine_listener_savepoint_physical_undo_conformance
  engine_listener_savepoint_physical_undo_conformance.cpp
)
target_compile_features(engine_listener_savepoint_physical_undo_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_savepoint_physical_undo_conformance PRIVATE
  sb_transaction_mga
  sb_storage_disk
)
add_test(
  NAME engine_listener_savepoint_physical_undo_conformance
  COMMAND engine_listener_savepoint_physical_undo_conformance
)
set_tests_properties(engine_listener_savepoint_physical_undo_conformance PROPERTIES
  LABELS "ELER-023;ELER-GATE-023;ELER-101;ELER-GATE-101;engine_listener_enterprise;transaction;mga;savepoint;physical_undo;crash_replay;data_page;catalog;index;overflow;metrics;linux_proof"
)

add_executable(engine_listener_mga_integrated_physical_cleanup_conformance
  engine_listener_mga_integrated_physical_cleanup_conformance.cpp
)
target_compile_features(engine_listener_mga_integrated_physical_cleanup_conformance PRIVATE cxx_std_23)
target_link_libraries(engine_listener_mga_integrated_physical_cleanup_conformance PRIVATE
  sb_engine_internal_api
  sb_storage_disk
  sb_storage_page
  sb_transaction_mga
)
add_test(
  NAME engine_listener_mga_integrated_physical_cleanup_conformance
  COMMAND engine_listener_mga_integrated_physical_cleanup_conformance
)
set_tests_properties(engine_listener_mga_integrated_physical_cleanup_conformance PROPERTIES
  LABELS "ELER-024;ELER-GATE-024;ELER-101;ELER-GATE-101;engine_listener_enterprise;transaction;mga;physical_cleanup;row_data;relation_store;secondary_index;overflow;current_row_map;page_finality;allocation_map;linux_proof"
)

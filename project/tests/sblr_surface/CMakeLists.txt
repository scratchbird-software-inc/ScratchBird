# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SB_PRIVATE_REPO_ROOT "${PROJECT_SOURCE_DIR}/..")
set(SB_SBLR_SURFACE_GUARDRAIL_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/sblr_surface_guardrail_gate.py"
)
set(SB_SBLR_SURFACE_FIXTURE_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/reference_sblr_interface_gap_2026_06_03"
)

function(sb_add_sblr_surface_guardrail_gate test_name gate_mode gate_labels)
  add_test(
    NAME "${test_name}"
    COMMAND "${Python3_EXECUTABLE}" "${SB_SBLR_SURFACE_GUARDRAIL_GATE}"
            --repo-root "${SB_PRIVATE_REPO_ROOT}"
            --fixture-root "${SB_SBLR_SURFACE_FIXTURE_ROOT}"
            --mode "${gate_mode}"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "${gate_labels};sblr_surface;guardrail_inventory;release_proof_substrate"
  )
endfunction()

add_custom_target(sblr_surface_public_guardrail_gates_check
  COMMAND "${Python3_EXECUTABLE}" "${SB_SBLR_SURFACE_GUARDRAIL_GATE}"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --fixture-root "${SB_SBLR_SURFACE_FIXTURE_ROOT}"
          --mode all
  COMMENT "Validating project-owned reference/SBLR surface guardrail fixtures"
)

sb_add_sblr_surface_guardrail_gate(
  sblr_surface_reference_inventory_guardrail_gate
  inventory
  "ELER-070;reference_sblr_fixture_promotion;reference_sblr_count_guardrail"
)
sb_add_sblr_surface_guardrail_gate(
  sblr_surface_no_skip_completion_guardrail_gate
  completion-policy
  "ELER-083;no_skip_waiver_xfail_completion;completion_policy_guardrail"
)
sb_add_sblr_surface_guardrail_gate(
  sblr_surface_traceability_manifest_guardrail_gate
  traceability
  "ELER-084;row_level_traceability_manifest;traceability_schema_guardrail"
)
sb_add_sblr_surface_guardrail_gate(
  sblr_surface_fixture_determinism_drift_gate
  determinism
  "ELER-086;generated_resource_determinism;fixture_drift_guardrail"
)
sb_add_sblr_surface_guardrail_gate(
  sblr_surface_sbsql_family_reconciliation_guardrail_gate
  family-reconciliation
  "ELER-071;family_reconciliation;sbsql_sblr_family_reconciliation"
)
sb_add_sblr_surface_guardrail_gate(
  sblr_surface_reference_interface_closure_gate
  interface-closure
  "ELER-079;reference_sblr_interface_closure;zero_unreconciled_reference_sblr_rows"
)
if(TARGET sb_server_core)
  add_executable(sblr_surface_server_authority_route_conformance
    sblr_surface_server_authority_route_conformance.cpp
  )
  target_compile_features(sblr_surface_server_authority_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(sblr_surface_server_authority_route_conformance PRIVATE
    sb_server_core
  )
  add_test(
    NAME sblr_surface_server_authority_route_conformance
    COMMAND sblr_surface_server_authority_route_conformance
            "${SB_SBLR_SURFACE_FIXTURE_ROOT}/SERVER_AUTHORITY_SURFACE_MATRIX.csv"
  )
  set_tests_properties(sblr_surface_server_authority_route_conformance PROPERTIES
    LABELS "ELER-072;ELER-104;ELER-GATE-104;server_authority_exact_routing;non_sblr_authority_no_sblr_execution;sblr_surface;release_proof_substrate;sblr_parser_authority_property"
  )
  add_executable(sblr_surface_migration_route_contract_conformance
    sblr_surface_migration_route_contract_conformance.cpp
  )
  target_compile_features(sblr_surface_migration_route_contract_conformance PRIVATE cxx_std_23)
  target_link_libraries(sblr_surface_migration_route_contract_conformance PRIVATE
    sb_server_core
  )
  add_test(
    NAME sblr_surface_migration_route_contract_conformance
    COMMAND sblr_surface_migration_route_contract_conformance
            "${SB_SBLR_SURFACE_FIXTURE_ROOT}/SERVER_AUTHORITY_SURFACE_MATRIX.csv"
  )
  set_tests_properties(sblr_surface_migration_route_contract_conformance PROPERTIES
    LABELS "ELER-073;migration_route_contract;live_replication_route_contract;no_reference_storage_authority;sblr_surface;release_proof_substrate"
  )
  add_executable(sblr_surface_fse_p3_decoder_encoder_contract_gate
    sblr_surface_fse_p3_decoder_encoder_contract_gate.cpp
  )
  target_compile_features(sblr_surface_fse_p3_decoder_encoder_contract_gate PRIVATE cxx_std_23)
  target_link_libraries(sblr_surface_fse_p3_decoder_encoder_contract_gate PRIVATE
    sb_engine_sblr
    sb_server_core
  )
  add_test(
    NAME sblr_surface_fse_p3_decoder_encoder_contract_gate
    COMMAND sblr_surface_fse_p3_decoder_encoder_contract_gate
            "${SB_PRIVATE_REPO_ROOT}"
  )
  set_tests_properties(sblr_surface_fse_p3_decoder_encoder_contract_gate PROPERTIES
    LABELS "FSE-P3;sblr_decoder_encoder_contract;parser_admission_revalidation;sblr_surface;release_proof_substrate"
  )
  add_executable(sblr_surface_fse_p4_engine_route_matrix_gate
    sblr_surface_fse_p4_engine_route_matrix_gate.cpp
  )
  target_compile_features(sblr_surface_fse_p4_engine_route_matrix_gate PRIVATE cxx_std_23)
  target_link_libraries(sblr_surface_fse_p4_engine_route_matrix_gate PRIVATE
    sb_engine_sblr
    sb_server_core
  )
  add_test(
    NAME sblr_surface_fse_p4_engine_route_matrix_gate
    COMMAND sblr_surface_fse_p4_engine_route_matrix_gate
            "${SB_PRIVATE_REPO_ROOT}"
  )
  set_tests_properties(sblr_surface_fse_p4_engine_route_matrix_gate PROPERTIES
    LABELS "FSE-P4;sblr_engine_internal_api_routes;parser_reference_execution_forbidden;sblr_surface;release_proof_substrate"
  )
  if(TARGET sb_cluster_provider)
    add_executable(sblr_surface_fse_p5_cluster_external_authority_policy_gate
      sblr_surface_fse_p5_cluster_external_authority_policy_gate.cpp
    )
    target_compile_features(sblr_surface_fse_p5_cluster_external_authority_policy_gate PRIVATE cxx_std_23)
    target_link_libraries(sblr_surface_fse_p5_cluster_external_authority_policy_gate PRIVATE
      sb_engine_sblr
      sb_server_core
      sb_cluster_provider
    )
    add_test(
      NAME sblr_surface_fse_p5_cluster_external_authority_policy_gate
      COMMAND sblr_surface_fse_p5_cluster_external_authority_policy_gate
              "${SB_PRIVATE_REPO_ROOT}"
    )
    set_tests_properties(sblr_surface_fse_p5_cluster_external_authority_policy_gate PROPERTIES
      LABELS "FSE-P5;sblr_cluster_external_authority_policy;cluster_provider_boundary;external_authority_fail_closed;sblr_surface;release_proof_substrate"
    )
    add_executable(sblr_surface_fse_p6_result_vector_diagnostic_gate
      sblr_surface_fse_p6_result_vector_diagnostic_gate.cpp
    )
    target_compile_features(sblr_surface_fse_p6_result_vector_diagnostic_gate PRIVATE cxx_std_23)
    target_link_libraries(sblr_surface_fse_p6_result_vector_diagnostic_gate PRIVATE
      sb_engine_sblr
      sb_server_core
      sb_cluster_provider
    )
    add_test(
      NAME sblr_surface_fse_p6_result_vector_diagnostic_gate
      COMMAND sblr_surface_fse_p6_result_vector_diagnostic_gate
              "${SB_PRIVATE_REPO_ROOT}"
    )
    set_tests_properties(sblr_surface_fse_p6_result_vector_diagnostic_gate PROPERTIES
      LABELS "FSE-P6;sblr_result_vector_diagnostics;redaction_support_evidence;cluster_external_fail_closed;sblr_surface;release_proof_substrate"
    )
    add_executable(sblr_surface_fse_p7_execution_proof_gate
      sblr_surface_fse_p7_execution_proof_gate.cpp
    )
    target_compile_features(sblr_surface_fse_p7_execution_proof_gate PRIVATE cxx_std_23)
    target_link_libraries(sblr_surface_fse_p7_execution_proof_gate PRIVATE
      sb_engine_sblr
      sb_server_core
      sb_cluster_provider
    )
    add_test(
      NAME sblr_surface_fse_p7_execution_proof_gate
      COMMAND sblr_surface_fse_p7_execution_proof_gate
              "${SB_PRIVATE_REPO_ROOT}"
    )
    set_tests_properties(sblr_surface_fse_p7_execution_proof_gate PROPERTIES
      LABELS "FSE-P7;sblr_final_execution_proof;matrix_cross_check;authority_boundary_closure;sblr_surface;release_proof_substrate"
    )
  endif()
endif()
if(TARGET sb_engine_functions)
  add_executable(sblr_surface_catalog_projection_seed_conformance
    sblr_surface_catalog_projection_seed_conformance.cpp
  )
  target_compile_features(sblr_surface_catalog_projection_seed_conformance PRIVATE cxx_std_20)
  target_link_libraries(sblr_surface_catalog_projection_seed_conformance PRIVATE
    sb_engine_functions
  )
  add_test(
    NAME sblr_surface_catalog_projection_seed_conformance
    COMMAND sblr_surface_catalog_projection_seed_conformance
            "${SB_SBLR_SURFACE_FIXTURE_ROOT}/NON_DIRECT_FUNCTION_SURFACE_MATRIX.csv"
  )
  set_tests_properties(sblr_surface_catalog_projection_seed_conformance PROPERTIES
    LABELS "ELER-076;reference_catalog_projection_seed_rowsets;system_catalog_projection_contract;visibility_redaction;sblr_surface;release_proof_substrate"
  )
  add_executable(sblr_surface_non_direct_function_lane_conformance
    sblr_surface_non_direct_function_lane_conformance.cpp
  )
  target_compile_features(sblr_surface_non_direct_function_lane_conformance PRIVATE cxx_std_20)
  target_link_libraries(sblr_surface_non_direct_function_lane_conformance PRIVATE
    sb_engine_functions
  )
  add_test(
    NAME sblr_surface_non_direct_function_lane_conformance
    COMMAND sblr_surface_non_direct_function_lane_conformance
            "${SB_SBLR_SURFACE_FIXTURE_ROOT}/NON_DIRECT_FUNCTION_SURFACE_MATRIX.csv"
            "${SB_SBLR_SURFACE_FIXTURE_ROOT}/EXPLICIT_UNSUPPORTED_SURFACE_MATRIX.csv"
  )
  set_tests_properties(sblr_surface_non_direct_function_lane_conformance PROPERTIES
    LABELS "ELER-074;ELER-075;ELER-104;ELER-GATE-104;non_direct_function_lane_contract;explicit_unsupported_refusal;no_parser_or_reference_authority;sblr_surface;release_proof_substrate;sblr_parser_authority_property"
  )
  add_executable(sblr_surface_function_registry_dispatch_conformance
    sblr_surface_function_registry_dispatch_conformance.cpp
  )
  target_compile_features(sblr_surface_function_registry_dispatch_conformance PRIVATE cxx_std_20)
  target_link_libraries(sblr_surface_function_registry_dispatch_conformance PRIVATE
    sb_engine_functions
  )
  add_test(
    NAME sblr_surface_function_registry_dispatch_conformance
    COMMAND sblr_surface_function_registry_dispatch_conformance
  )
  set_tests_properties(sblr_surface_function_registry_dispatch_conformance PROPERTIES
    LABELS "ELER-074;ELER-079;ELER-104;ELER-GATE-104;final_function_registry_dispatch;no_missing_function_handler;sblr_surface;release_proof_substrate;sblr_parser_authority_property"
  )
endif()
add_test(
  NAME sblr_surface_catalog_seed_manifest_gate
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/sblr_surface_catalog_seed_manifest_gate.py"
          --repo-root "${SB_PRIVATE_REPO_ROOT}"
          --fixture-root "${SB_SBLR_SURFACE_FIXTURE_ROOT}"
)
set_tests_properties(sblr_surface_catalog_seed_manifest_gate PROPERTIES
  LABELS "ELER-076;reference_catalog_seed_manifest_json_parse;visibility_redaction;sblr_surface;release_proof_substrate"
)
sb_add_sblr_surface_guardrail_gate(
  sblr_surface_registry_cleanup_guardrail_gate
  registry-cleanup
  "ELER-077;ELER-078;DSIA-006;sblr_registry_cleanup_guardrail"
)
sb_add_sblr_surface_guardrail_gate(
  sblr_surface_sbsql_sync_guardrail_gate
  sbsql-sync
  "ELER-089;sbsql_sync_guardrail;compatibility_dialect_paste_through_rejection"
)
sb_add_sblr_surface_guardrail_gate(
  sblr_surface_public_substrate_guardrail_gate
  all
  "ELER-070;ELER-071;ELER-072;ELER-073;ELER-074;ELER-075;ELER-076;ELER-077;ELER-078;ELER-079;ELER-083;ELER-084;ELER-086;ELER-089;public_proof_substrate_guardrail"
)

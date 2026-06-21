# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(sbp_sbsql_pipeline_probe parser_pipeline_probe.cpp)
target_compile_features(sbp_sbsql_pipeline_probe PRIVATE cxx_std_23)
target_link_libraries(sbp_sbsql_pipeline_probe PRIVATE sbl_sbsql_parser_pipeline)
add_test(NAME sbp_sbsql_pipeline_probe COMMAND sbp_sbsql_pipeline_probe)

add_executable(sbsql_canonical_element_stream_runtime_conformance
  sbsql_canonical_element_stream_runtime_conformance.cpp
)
target_compile_features(sbsql_canonical_element_stream_runtime_conformance PRIVATE cxx_std_23)
target_link_libraries(sbsql_canonical_element_stream_runtime_conformance PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbsql_canonical_element_stream_runtime_conformance
  COMMAND sbsql_canonical_element_stream_runtime_conformance
)
set_tests_properties(sbsql_canonical_element_stream_runtime_conformance PROPERTIES
  LABELS "sbsql_canonical_element_stream;sbsql_parser_worker;SML-005;SML-006;SML-082;SML-083"
)

add_executable(sbp_sbsql_ipc_schema_probe parser_ipc_schema_probe.cpp)
target_compile_features(sbp_sbsql_ipc_schema_probe PRIVATE cxx_std_23)
target_link_libraries(sbp_sbsql_ipc_schema_probe PRIVATE sbl_sbsql_parser_pipeline)
add_test(NAME sbp_sbsql_ipc_schema_probe COMMAND sbp_sbsql_ipc_schema_probe)

add_executable(sbsql_language_resource_contract_conformance
  sbsql_language_resource_contract_conformance.cpp
)
target_compile_features(sbsql_language_resource_contract_conformance PRIVATE cxx_std_23)
target_link_libraries(sbsql_language_resource_contract_conformance PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbsql_language_resource_contract_conformance
  COMMAND sbsql_language_resource_contract_conformance
)
set_tests_properties(sbsql_language_resource_contract_conformance PROPERTIES
  LABELS "sbsql_language_resource_contract;sbsql_parser_worker;SML-005;SML-010;SML-082;SML-083;SML-084;SML-086;SML-089;SML-091;SML-092;SML-093;SML-094;SML-095;SML-096;SML-097;SML-098;SML-099;SML-100;SML-101"
)

add_executable(sbsql_language_resource_governance_lifecycle_conformance
  sbsql_language_resource_governance_lifecycle_conformance.cpp
)
target_compile_features(sbsql_language_resource_governance_lifecycle_conformance PRIVATE cxx_std_23)
target_link_libraries(sbsql_language_resource_governance_lifecycle_conformance PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbsql_language_resource_governance_lifecycle_conformance
  COMMAND sbsql_language_resource_governance_lifecycle_conformance
)
set_tests_properties(sbsql_language_resource_governance_lifecycle_conformance PROPERTIES
  LABELS "sbsql_language_resource_contract;sbsql_language_resource_governance_lifecycle;sbsql_parser_worker;SML-085;SML-088"
)

add_executable(sbsql_language_locale_profile_matrix_conformance
  sbsql_language_locale_profile_matrix_conformance.cpp
)
target_compile_features(sbsql_language_locale_profile_matrix_conformance PRIVATE cxx_std_23)
target_link_libraries(sbsql_language_locale_profile_matrix_conformance PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbsql_language_locale_profile_matrix_conformance
  COMMAND sbsql_language_locale_profile_matrix_conformance
)
set_tests_properties(sbsql_language_locale_profile_matrix_conformance PROPERTIES
  LABELS "sbsql_language_locale_profile_matrix;sbsql_language_resource_contract;sbsql_parser_worker;SML-013;SML-014;SML-GATE-013;SML-GATE-014"
)

add_executable(sbsql_sml087_unicode_predictive_resource_conformance
  sbsql_sml087_unicode_predictive_resource_conformance.cpp
)
target_compile_features(sbsql_sml087_unicode_predictive_resource_conformance PRIVATE cxx_std_23)
target_link_libraries(sbsql_sml087_unicode_predictive_resource_conformance PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbsql_sml087_unicode_predictive_resource_conformance
  COMMAND sbsql_sml087_unicode_predictive_resource_conformance
)
set_tests_properties(sbsql_sml087_unicode_predictive_resource_conformance PROPERTIES
  LABELS "sbsql_sml087_unicode_predictive_resource;sbsql_language_resource_contract;sbsql_parser_worker;SML-087"
)

add_executable(sbsql_language_control_surface_conformance
  sbsql_language_control_surface_conformance.cpp
)
target_compile_features(sbsql_language_control_surface_conformance PRIVATE cxx_std_23)
target_link_libraries(sbsql_language_control_surface_conformance PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbsql_language_control_surface_conformance
  COMMAND sbsql_language_control_surface_conformance
)
set_tests_properties(sbsql_language_control_surface_conformance PROPERTIES
  LABELS "sbsql_language_control;sbsql_parser_worker;SML-003;SML-008;SML-009"
)

add_executable(sbsql_multilingual_passthrough_runtime_conformance
  sbsql_multilingual_passthrough_runtime_conformance.cpp
)
target_compile_features(sbsql_multilingual_passthrough_runtime_conformance PRIVATE cxx_std_23)
target_link_libraries(
  sbsql_multilingual_passthrough_runtime_conformance
  PRIVATE
    sbl_sbsql_parser_worker_core
)
add_test(
  NAME sbsql_multilingual_passthrough_runtime_conformance
  COMMAND sbsql_multilingual_passthrough_runtime_conformance
)
set_tests_properties(sbsql_multilingual_passthrough_runtime_conformance PROPERTIES
  LABELS "sbsql_multilingual_passthrough_runtime;sbsql_parser_worker;SML-018;SML-020;SML-090;frontdoor_lowering_cache;no_execution_plan_runtime"
)

add_executable(sbp_sbsql_registry_generation_probe sbsql_registry_generation_probe.cpp)
target_compile_features(sbp_sbsql_registry_generation_probe PRIVATE cxx_std_23)
target_link_libraries(sbp_sbsql_registry_generation_probe PRIVATE sbl_sbsql_parser_pipeline)
set(SBSQL_REGRESSION_ARTIFACT_ROOT
  "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts"
)
set(SBSQL_SURFACE_TO_SBLR_ARTIFACT_ROOT
  "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
)
set(SBSQL_SURFACE_TO_SBLR_FIXTURE_ROOT
  "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/fixtures/surface_to_sblr"
)
set(SBSQL_CANONICALIZATION_ROOT
  "${PROJECT_SOURCE_DIR}/../public_input_snapshot"
)
add_test(
  NAME sbp_sbsql_registry_generation_probe
  COMMAND sbp_sbsql_registry_generation_probe "${SBSQL_REGRESSION_ARTIFACT_ROOT}"
)
set_tests_properties(sbp_sbsql_registry_generation_probe PROPERTIES
  LABELS "registry_linter_tests;sbsql_parser_worker"
)

add_executable(
  sbp_sbsql_generated_full_surface_conformance_probe
  generated/full_surface/sbsql_generated_full_surface_conformance_probe.cpp
)
target_compile_features(sbp_sbsql_generated_full_surface_conformance_probe PRIVATE cxx_std_23)
target_link_libraries(
  sbp_sbsql_generated_full_surface_conformance_probe
  PRIVATE
    sbl_sbsql_parser_pipeline
)
add_test(
  NAME sbp_sbsql_generated_full_surface_conformance_probe
  COMMAND sbp_sbsql_generated_full_surface_conformance_probe
          "${SBSQL_REGRESSION_ARTIFACT_ROOT}"
          "${SBSQL_CANONICALIZATION_ROOT}"
)
set_tests_properties(sbp_sbsql_generated_full_surface_conformance_probe PROPERTIES
  LABELS "sbsql_generated_full_surface_conformance;parser_v3_closure_gate;parser_conformance_oracle_gate;sbsql_parser_worker"
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SBSQL_PYTHON3 "${Python3_EXECUTABLE}")

add_test(
  NAME sbsql_multilingual_resource_edge_safety_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_multilingual_resource_edge_safety_harness.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_multilingual_resource_edge_safety_gate PROPERTIES
  LABELS "sbsql_multilingual_resource_edge_safety;sbsql_parser_worker;SML-094;SML-095;SML-096;SML-097;SML-098;SML-099;SML-100;SML-101"
)

add_test(
  NAME sbsql_driver_language_surface_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tools/release/sbsql_driver_language_surface_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_driver_language_surface_gate PROPERTIES
  LABELS "sbsql_driver_language_surface;sbsql_parser_worker;SML-018;SML-020;SML-083;SML-086;SML-090;SML-091;SML-093;SML-101"
)

add_test(
  NAME sbsql_language_golden_trace_oracle_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_language_golden_trace_oracle_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_language_golden_trace_oracle_gate PROPERTIES
  LABELS "sbsql_language_golden_trace_oracle;sbsql_parser_worker;SML-089;SML-GATE-089"
)

add_test(
  NAME sbsql_sml_026_027_028_route_script_replay_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/route_script_replay/sbsql_sml_026_027_028_route_script_replay_gate.py"
)
set_tests_properties(sbsql_sml_026_027_028_route_script_replay_gate PROPERTIES
  LABELS "sbsql_route_script_replay;sbsql_parser_worker;SML-026;SML-027;SML-028;SML-GATE-026;SML-GATE-027;SML-GATE-028;mga_transaction_regression"
)

add_test(
  NAME sbsql_sml_029_032_feature_closure_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_sml_029_032_feature_closure_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sml_029_032_feature_closure_gate PROPERTIES
  LABELS "sbsql_sql_feature_closure;sbsql_parser_worker;SML-029;SML-030;SML-031;SML-032;SML-GATE-029;SML-GATE-030;SML-GATE-031;SML-GATE-032;mga_transaction_regression"
)

add_test(
  NAME sbsql_security_regression_matrix_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/security/sbsql_security_regression_matrix_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_security_regression_matrix_gate PROPERTIES
  LABELS "sbsql_security_regression_matrix;sbsql_parser_worker;SML-033;SML-034;SML-035;SML-036;SML-037;SML-038;SML-039;SML-040;SML-GATE-033;SML-GATE-034;SML-GATE-035;SML-GATE-036;SML-GATE-037;SML-GATE-038;SML-GATE-039;SML-GATE-040;mga_transaction_regression"
)

add_test(
  NAME sbsql_sml_041_046_product_qa_manifest_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/product_regression/sml_041_046/sbsql_sml_041_046_product_qa_manifest_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sml_041_046_product_qa_manifest_gate PROPERTIES
  LABELS "sbsql_product_qa_manifest;sbsql_parser_worker;SML-041;SML-042;SML-043;SML-044;SML-045;SML-046;SML-GATE-041;SML-GATE-042;SML-GATE-043;SML-GATE-044;SML-GATE-045;SML-GATE-046;mga_transaction_regression"
)

add_test(
  NAME sbsql_sml_044_046_hardening_oracle_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/hardening/sml_041_046/sbsql_sml_044_046_hardening_oracle_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sml_044_046_hardening_oracle_gate PROPERTIES
  LABELS "sbsql_hardening_oracle;sbsql_parser_worker;SML-044;SML-046;SML-GATE-044;SML-GATE-046;mga_transaction_regression"
)

add_test(
  NAME sbsql_sml_049_052_filespace_relocation_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/filespace_relocation/sbsql_sml_049_052_filespace_relocation_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sml_049_052_filespace_relocation_gate PROPERTIES
  LABELS "sbsql_filespace_relocation;sbsql_parser_worker;SML-049;SML-050;SML-051;SML-052;SML-GATE-049;SML-GATE-050;SML-GATE-051;SML-GATE-052;mga_transaction_regression"
)

add_test(
  NAME sbsql_sml_054_057_062_multimodel_capability_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/multimodel_capability/sbsql_sml_054_057_062_multimodel_capability_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sml_054_057_062_multimodel_capability_gate PROPERTIES
  LABELS "sbsql_multimodel_capability;sbsql_parser_worker;SML-054;SML-055;SML-056;SML-057;SML-062;SML-GATE-054;SML-GATE-055;SML-GATE-056;SML-GATE-057;SML-GATE-062;mga_transaction_regression"
)

add_test(
  NAME sbsql_sml_059_061_encrypted_database_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/encrypted_database/sbsql_sml_059_061_encrypted_database_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sml_059_061_encrypted_database_gate PROPERTIES
  LABELS "sbsql_encrypted_database_matrix;sbsql_parser_worker;SML-059;SML-060;SML-061;SML-GATE-059;SML-GATE-060;SML-GATE-061;mga_transaction_regression"
)

add_test(
  NAME sbsql_sml_064_066_native_compile_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/native_compile/sbsql_sml_064_066_native_compile_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sml_064_066_native_compile_gate PROPERTIES
  LABELS "sbsql_native_compile_jit_aot;sbsql_parser_worker;SML-064;SML-065;SML-066;SML-GATE-064;SML-GATE-065;SML-GATE-066;mga_transaction_regression"
)

add_test(
  NAME sbsql_sml_047_release_qa_package_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/release_qa/sbsql_sml_047_release_qa_package_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sml_047_release_qa_package_gate PROPERTIES
  LABELS "sbsql_release_qa_package;sbsql_parser_worker;SML-047;SML-GATE-047;mga_transaction_regression"
)

add_test(
  NAME sbsql_sml_067_081_release_certification_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/release_certification/sbsql_sml_067_081_release_certification_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sml_067_081_release_certification_gate PROPERTIES
  LABELS "sbsql_release_certification;sbsql_parser_worker;release_gate;SML-067;SML-068;SML-069;SML-070;SML-071;SML-072;SML-073;SML-074;SML-075;SML-076;SML-077;SML-078;SML-079;SML-080;SML-081;SML-GATE-067;SML-GATE-068;SML-GATE-069;SML-GATE-070;SML-GATE-071;SML-GATE-072;SML-GATE-073;SML-GATE-074;SML-GATE-075;SML-GATE-076;SML-GATE-077;SML-GATE-078;SML-GATE-079;SML-GATE-080;SML-GATE-081;mga_transaction_regression"
)

if(NOT TARGET sb_isql AND EXISTS "${PROJECT_SOURCE_DIR}/drivers/tool/cli/CMakeLists.txt")
  add_subdirectory("${PROJECT_SOURCE_DIR}/drivers/tool/cli" "${CMAKE_BINARY_DIR}/drivers/tool/cli")
endif()

set(SBSQL_SBLR_FINAL_CLEANUP_BOOTSTRAP_LABELS
  sbsql_public_exact_commands
  sbsql_sblr_api_routes
  sbsql_engine_behavior_complete
  sbsql_udr_support
  sbsql_result_shape_contracts
  sbsql_security_privilege_contracts
  sbsql_catalog_system_view_sync
  sblr_family_admission_conformance
  sblr_opcode_registry_conformance
  sblr_version_compatibility
  sbsql_cluster_fail_closed_noncluster
  sbsql_cluster_stub_provider
  sbsql_cluster_provider_abi_contract
  sbsql_public_private_cluster_boundary
  sbsql_message_vector_rendering
  sbsql_no_stub_noncluster_integrity
  sbsql_catalog_seed_upgrade_restart
  sbsql_spec_doc_sync
  sbsql_sblr_final_cleanup_full_route
)

function(add_sbsql_sblr_final_cleanup_bootstrap_label final_cleanup_label)
  set(test_name "sbsql_sblr_final_cleanup_label_${final_cleanup_label}")
  add_test(
    NAME "${test_name}"
    COMMAND "${SBSQL_PYTHON3}"
            "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_sblr_final_cleanup_bootstrap_gate.py"
            --repo-root "${PROJECT_SOURCE_DIR}/.."
            --label "${final_cleanup_label}"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "${final_cleanup_label};sbsql_sblr_final_cleanup_bootstrap;sbsql_parser_worker"
  )
endfunction()

foreach(final_cleanup_label IN LISTS SBSQL_SBLR_FINAL_CLEANUP_BOOTSTRAP_LABELS)
  add_sbsql_sblr_final_cleanup_bootstrap_label("${final_cleanup_label}")
endforeach()

add_test(
  NAME sbsql_sblr_final_zero_unimplemented_audit_bootstrap
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tools/sbsql_sblr_final_cleanup_audit.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --mode baseline
)
set_tests_properties(sbsql_sblr_final_zero_unimplemented_audit_bootstrap PROPERTIES
  LABELS "sbsql_sblr_final_zero_unimplemented_audit;sbsql_sblr_final_cleanup_bootstrap;sbsql_parser_worker"
)

add_test(
  NAME ctest_no_execution_plan_runtime_dependency_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/ctest_no_execution_plan_runtime_dependency_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --build-root "${CMAKE_BINARY_DIR}"
)
set_tests_properties(ctest_no_execution_plan_runtime_dependency_gate PROPERTIES
  LABELS "ctest_no_execution_plan_runtime_dependency_gate;sbsql_parser_worker;release_gate"
)

add_test(
  NAME sbsql_final_language_expansion_closure_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_final_language_expansion_closure_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --build-root "${CMAKE_BINARY_DIR}"
)
set_tests_properties(sbsql_final_language_expansion_closure_gate PROPERTIES
  LABELS "FSL-P7;final_sblr_sbsql_language_expansion_closure;sbsql_parser_worker;parser_v3_closure_gate;sblr_operation_matrix_gate;mga_transaction_regression"
  DEPENDS "sbsql_surface_to_sblr_function_coverage_gate;sbsql_no_stub_source_integrity_gate;sbsql_sblr_binary_round_trip_fixture_gate"
)

add_test(
  NAME final_sblr_sbsql_enterprise_proof_closure_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/final_sblr_sbsql_enterprise_proof_closure_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --build-root "${CMAKE_BINARY_DIR}"
)
set_tests_properties(final_sblr_sbsql_enterprise_proof_closure_gate PROPERTIES
  LABELS "FEP-P7;final_sblr_sbsql_enterprise_proof_closure;release_gate;sbsql_parser_worker;sblr_surface;reference_parser_release_evidence;mga_transaction_regression"
  DEPENDS "sbsql_final_language_expansion_closure_gate;ctest_no_execution_plan_runtime_dependency_gate;parser_dialect_isolation_audit_gate;sblr_surface_fse_p7_execution_proof_gate"
)

add_test(
  NAME final_sblr_sbsql_master_closure_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/final_sblr_sbsql_master_closure_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(final_sblr_sbsql_master_closure_gate PROPERTIES
  LABELS "FSM-P7;final_sblr_sbsql_master_closure;release_gate;sbsql_parser_worker;sblr_surface;reference_parser_release_evidence;mga_transaction_regression"
  DEPENDS "final_sblr_sbsql_enterprise_proof_closure_gate;sbsql_final_language_expansion_closure_gate;parser_dialect_isolation_audit_gate"
)

add_test(
  NAME sbsql_missing_functionality_scope_freeze_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_missing_functionality_scope_registry_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --mode scope
)
set_tests_properties(sbsql_missing_functionality_scope_freeze_gate PROPERTIES
  LABELS "sbsql_missing_functionality_implementation_closure;SBSQL-MISS-GATE-000;sbsql_parser_worker"
)

add_test(
  NAME sbsql_missing_functionality_registry_allocation_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_missing_functionality_scope_registry_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --mode registry
)
set_tests_properties(sbsql_missing_functionality_registry_allocation_gate PROPERTIES
  LABELS "sbsql_missing_functionality_implementation_closure;SBSQL-MISS-GATE-001;sbsql_parser_worker"
)

add_test(
  NAME dpc_workload_corpus_closure_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/dpc_workload_corpus_closure_gate.py"
          --output-dir "${CMAKE_BINARY_DIR}/dpc_workload_corpus_closure_gate"
)
set_tests_properties(dpc_workload_corpus_closure_gate PROPERTIES
  LABELS "dpc_workload_corpus_closure_gate;DPC-007;DPC_TEST_WORKLOAD_CORPUS;benchmark_corpus;mga_transaction_regression;sbsql_parser_worker"
)

add_test(
  NAME sbsql_surface_to_sblr_function_coverage_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_surface_to_sblr_function_coverage_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --artifact-root "${SBSQL_REGRESSION_ARTIFACT_ROOT}"
)
set_tests_properties(sbsql_surface_to_sblr_function_coverage_gate PROPERTIES
  LABELS "sbsql_surface_to_sblr_function_coverage_gate;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_no_grey_row_coverage_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_no_grey_row_coverage_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --artifact-root "${SBSQL_SURFACE_TO_SBLR_ARTIFACT_ROOT}"
)
set_tests_properties(sbsql_no_grey_row_coverage_gate PROPERTIES
  LABELS "sbsql_no_grey_row_coverage_gate;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_no_stub_source_integrity_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_no_stub_source_integrity_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_no_stub_source_integrity_gate PROPERTIES
  LABELS "sbsql_no_stub_source_integrity_gate;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_per_row_evidence_manifest_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_per_row_evidence_manifest_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --artifact-root "${SBSQL_SURFACE_TO_SBLR_ARTIFACT_ROOT}"
)
set_tests_properties(sbsql_per_row_evidence_manifest_gate PROPERTIES
  LABELS "sbsql_per_row_evidence_manifest_gate;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_bridge_command_surface_tracking_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_bridge_command_surface_tracking_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --artifact-root "${SBSQL_SURFACE_TO_SBLR_ARTIFACT_ROOT}"
)
# SEARCH_KEY: SBSQL_BRIDGE_COMMAND_SURFACE_FULL_TRACKING
set_tests_properties(sbsql_bridge_command_surface_tracking_gate PROPERTIES
  LABELS "sbsql_bridge_command_surface_tracking_gate;sbsql_surface_to_sblr_full_implementation_closure;sbsql_sblr_api_routes;trusted_udr_bridge_gate;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_agent_heartbeat_recovery_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_agent_heartbeat_recovery_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --fixture-root "${SBSQL_SURFACE_TO_SBLR_FIXTURE_ROOT}"
)
set_tests_properties(sbsql_agent_heartbeat_recovery_gate PROPERTIES
  LABELS "sbsql_agent_heartbeat_recovery_gate;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_authenticated_route_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_authenticated_route_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --artifact-root "${SBSQL_SURFACE_TO_SBLR_ARTIFACT_ROOT}"
)
set_tests_properties(sbsql_authenticated_route_fixture_gate PROPERTIES
  LABELS "sbsql_authenticated_route_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sblr_binary_round_trip_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sblr_binary_round_trip_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --artifact-root "${SBSQL_SURFACE_TO_SBLR_ARTIFACT_ROOT}"
)
set_tests_properties(sbsql_sblr_binary_round_trip_fixture_gate PROPERTIES
  LABELS "sbsql_sblr_binary_round_trip_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_route_round_trip_fixture_promotion_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tools/sb_parser_gen/promote_route_and_round_trip_fixtures.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --artifact-root "${SBSQL_SURFACE_TO_SBLR_ARTIFACT_ROOT}"
          --dry-run
)
set_tests_properties(sbsql_route_round_trip_fixture_promotion_gate PROPERTIES
  LABELS "sbsql_route_round_trip_fixture_promotion_gate;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_status_change_authority_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_status_change_authority_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --artifact-root "${SBSQL_SURFACE_TO_SBLR_ARTIFACT_ROOT}"
)
set_tests_properties(sbsql_status_change_authority_gate PROPERTIES
  LABELS "sbsql_status_change_authority_gate;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_deterministic_generation_no_network_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_deterministic_generation_no_network_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --artifact-root "${SBSQL_SURFACE_TO_SBLR_ARTIFACT_ROOT}"
)
set_tests_properties(sbsql_deterministic_generation_no_network_gate PROPERTIES
  LABELS "sbsql_deterministic_generation_no_network_gate;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_language_element_manifest_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_language_element_manifest_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --artifact-root "${SBSQL_SURFACE_TO_SBLR_ARTIFACT_ROOT}"
)
set_tests_properties(sbsql_language_element_manifest_gate PROPERTIES
  LABELS "sbsql_language_element_manifest_gate;sbsql_language_resource_contract;sbsql_parser_worker;SML-010;SML-092"
)

add_test(
  NAME sbsql_language_resource_pack_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_language_resource_pack_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_language_resource_pack_gate PROPERTIES
  LABELS "sbsql_language_resource_pack_gate;sbsql_language_resource_contract;sbsql_parser_worker;SML-010;SML-014;SML-087;SML-089;SML-092;SML-094;SML-098;SML-099;SML-101"
)

add_test(
  NAME sbsql_language_beta_release_proof_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_language_beta_release_proof_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_language_beta_release_proof_gate PROPERTIES
  LABELS "sbsql_language_beta_release_proof_gate;sbsql_language_resource_contract;sbsql_parser_worker;SML-010;SML-014;SML-018;SML-020;SML-087;SML-089;SML-092;SML-094;SML-098;SML-099;SML-101"
)

add_test(
  NAME sbsql_sml_010_012_023_025_parser_closure_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/parser_closure/sbsql_sml_010_012_023_025_parser_closure_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sml_010_012_023_025_parser_closure_gate PROPERTIES
  LABELS "sbsql_parser_closure;sbsql_parser_worker;SML-010;SML-011;SML-012;SML-023;SML-025;SML-GATE-010;SML-GATE-011;SML-GATE-012;SML-GATE-023;SML-GATE-025"
)

add_test(
  NAME sbsql_sbsfc_010_numeric_builtin_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_010_numeric_builtin_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_010_numeric_builtin_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_010_numeric_builtin_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_010_numeric_builtin_runtime_conformance
  generated/full_surface/sbsql_sbsfc_010_numeric_builtin_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_010_numeric_builtin_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_010_numeric_builtin_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_010_numeric_builtin_runtime_conformance
  COMMAND sbsql_sbsfc_010_numeric_builtin_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_010_numeric_builtin_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_010_numeric_builtin_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_011_text_builtin_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_011_text_builtin_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_011_text_builtin_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_011_text_builtin_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_011_text_builtin_runtime_conformance
  generated/full_surface/sbsql_sbsfc_011_text_builtin_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_011_text_builtin_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_011_text_builtin_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_011_text_builtin_runtime_conformance
  COMMAND sbsql_sbsfc_011_text_builtin_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_011_text_builtin_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_011_text_builtin_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_012_temporal_session_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_012_temporal_session_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_012_temporal_session_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_012_temporal_session_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_012_temporal_session_runtime_conformance
  generated/full_surface/sbsql_sbsfc_012_temporal_session_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_012_temporal_session_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_012_temporal_session_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_012_temporal_session_runtime_conformance
  COMMAND sbsql_sbsfc_012_temporal_session_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_012_temporal_session_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_012_temporal_session_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_013_document_collection_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_013_document_collection_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_013_document_collection_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_013_document_collection_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_013_document_collection_runtime_conformance
  generated/full_surface/sbsql_sbsfc_013_document_collection_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_013_document_collection_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_013_document_collection_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_013_document_collection_runtime_conformance
  COMMAND sbsql_sbsfc_013_document_collection_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_013_document_collection_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_013_document_collection_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_014_fuzzy_text_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_014_fuzzy_text_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_014_fuzzy_text_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_014_fuzzy_text_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_014_fuzzy_text_runtime_conformance
  generated/full_surface/sbsql_sbsfc_014_fuzzy_text_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_014_fuzzy_text_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_014_fuzzy_text_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_014_fuzzy_text_runtime_conformance
  COMMAND sbsql_sbsfc_014_fuzzy_text_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_014_fuzzy_text_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_014_fuzzy_text_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_014_vector_runtime_conformance
  generated/full_surface/sbsql_sbsfc_014_vector_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_014_vector_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_014_vector_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_014_vector_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_014_vector_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_014_vector_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_014_vector_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)
add_test(
  NAME sbsql_sbsfc_014_vector_runtime_conformance
  COMMAND sbsql_sbsfc_014_vector_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_014_vector_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_014_vector_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_015_aggregate_window_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_015_aggregate_window_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_015_aggregate_window_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_015_aggregate_window_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
)

if(TARGET sb_engine_sblr)
  add_executable(
    sbsql_sbsfc_015_aggregate_window_runtime_conformance
    generated/full_surface/sbsql_sbsfc_015_aggregate_window_runtime_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_015_aggregate_window_runtime_conformance PRIVATE cxx_std_20)
  target_link_libraries(sbsql_sbsfc_015_aggregate_window_runtime_conformance PRIVATE sb_engine_sblr)
  add_test(
    NAME sbsql_sbsfc_015_aggregate_window_runtime_conformance
    COMMAND sbsql_sbsfc_015_aggregate_window_runtime_conformance
  )
  set_tests_properties(sbsql_sbsfc_015_aggregate_window_runtime_conformance PROPERTIES
    LABELS "sbsql_sbsfc_015_aggregate_window_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
  )
endif()

if(TARGET sb_engine_sblr)
  add_executable(
    sbsql_sbsfc_operator_binding_runtime_conformance
    generated/full_surface/sbsql_sbsfc_operator_binding_runtime_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_operator_binding_runtime_conformance PRIVATE cxx_std_20)
  target_link_libraries(sbsql_sbsfc_operator_binding_runtime_conformance PRIVATE sb_engine_sblr)
  add_test(
    NAME sbsql_sbsfc_operator_binding_runtime_conformance
    COMMAND sbsql_sbsfc_operator_binding_runtime_conformance
  )
  set_tests_properties(sbsql_sbsfc_operator_binding_runtime_conformance PROPERTIES
    LABELS "sbsql_sbsfc_operator_binding_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sbsql_parser_worker"
  )
endif()

add_test(
  NAME sbsql_sbsfc_016_procedural_context_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_016_procedural_context_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_016_procedural_context_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_016_procedural_context_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-016R-G;SBSFC-016R-H;SBSFC-016R-I;SBSFC-016R-J;SBSFC-016R-K;SBSFC-016R-L;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_016_procedural_context_runtime_conformance
  generated/full_surface/sbsql_sbsfc_016_procedural_context_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_016_procedural_context_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_016_procedural_context_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_016_procedural_context_runtime_conformance
  COMMAND sbsql_sbsfc_016_procedural_context_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_016_procedural_context_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_016_procedural_context_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-016R-G;SBSFC-016R-H;SBSFC-016R-I;SBSFC-016R-J;SBSFC-016R-K;SBSFC-016R-L;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_017_regex_text_pattern_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_017_regex_text_pattern_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_017_regex_text_pattern_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_017_regex_text_pattern_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-017R;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_017_regex_text_pattern_runtime_conformance
  generated/full_surface/sbsql_sbsfc_017_regex_text_pattern_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_017_regex_text_pattern_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_017_regex_text_pattern_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_017_regex_text_pattern_runtime_conformance
  COMMAND sbsql_sbsfc_017_regex_text_pattern_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_017_regex_text_pattern_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_017_regex_text_pattern_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-017R;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_026_text_conversion_helper_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_026_text_conversion_helper_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_026_text_conversion_helper_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_026_text_conversion_helper_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-026R-K;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_026_text_conversion_helper_runtime_conformance
  generated/full_surface/sbsql_sbsfc_026_text_conversion_helper_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_026_text_conversion_helper_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_026_text_conversion_helper_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_026_text_conversion_helper_runtime_conformance
  COMMAND sbsql_sbsfc_026_text_conversion_helper_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_026_text_conversion_helper_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_026_text_conversion_helper_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-026R-K;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_027_string_encoding_helper_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_027_string_encoding_helper_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_027_string_encoding_helper_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_027_string_encoding_helper_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-027;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_027_string_encoding_helper_runtime_conformance
  generated/full_surface/sbsql_sbsfc_027_string_encoding_helper_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_027_string_encoding_helper_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_027_string_encoding_helper_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_027_string_encoding_helper_runtime_conformance
  COMMAND sbsql_sbsfc_027_string_encoding_helper_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_027_string_encoding_helper_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_027_string_encoding_helper_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-027;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_028_uuid_compat_helper_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_028_uuid_compat_helper_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_028_uuid_compat_helper_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_028_uuid_compat_helper_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-028;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_028_uuid_compat_helper_runtime_conformance
  generated/full_surface/sbsql_sbsfc_028_uuid_compat_helper_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_028_uuid_compat_helper_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_028_uuid_compat_helper_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_028_uuid_compat_helper_runtime_conformance
  COMMAND sbsql_sbsfc_028_uuid_compat_helper_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_028_uuid_compat_helper_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_028_uuid_compat_helper_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-028;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_029_sequence_generator_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_029_sequence_generator_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_029_sequence_generator_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_029_sequence_generator_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-029;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_029_sequence_generator_runtime_conformance
  generated/full_surface/sbsql_sbsfc_029_sequence_generator_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_029_sequence_generator_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_029_sequence_generator_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_029_sequence_generator_runtime_conformance
  COMMAND sbsql_sbsfc_029_sequence_generator_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_029_sequence_generator_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_029_sequence_generator_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-029;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_030_temporal_format_epoch_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_030_temporal_format_epoch_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_030_temporal_format_epoch_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_030_temporal_format_epoch_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-030;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_030_temporal_format_epoch_runtime_conformance
  generated/full_surface/sbsql_sbsfc_030_temporal_format_epoch_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_030_temporal_format_epoch_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_030_temporal_format_epoch_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_030_temporal_format_epoch_runtime_conformance
  COMMAND sbsql_sbsfc_030_temporal_format_epoch_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_030_temporal_format_epoch_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_030_temporal_format_epoch_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-030;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_031_txid_surface_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_031_txid_surface_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_031_txid_surface_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_031_txid_surface_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-031;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_031_txid_surface_runtime_conformance
  generated/full_surface/sbsql_sbsfc_031_txid_surface_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_031_txid_surface_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_031_txid_surface_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_031_txid_surface_runtime_conformance
  COMMAND sbsql_sbsfc_031_txid_surface_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_031_txid_surface_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_031_txid_surface_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-031;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_032_scalar_utility_conversion_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_032_scalar_utility_conversion_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_032_scalar_utility_conversion_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_032_scalar_utility_conversion_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-032;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_032_scalar_utility_conversion_runtime_conformance
  generated/full_surface/sbsql_sbsfc_032_scalar_utility_conversion_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_032_scalar_utility_conversion_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_032_scalar_utility_conversion_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_032_scalar_utility_conversion_runtime_conformance
  COMMAND sbsql_sbsfc_032_scalar_utility_conversion_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_032_scalar_utility_conversion_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_032_scalar_utility_conversion_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-032;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_033_catalog_descriptor_diagnostic_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_033_catalog_descriptor_diagnostic_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_033_catalog_descriptor_diagnostic_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_033_catalog_descriptor_diagnostic_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-033;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_033_catalog_descriptor_diagnostic_runtime_conformance
  generated/full_surface/sbsql_sbsfc_033_catalog_descriptor_diagnostic_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_033_catalog_descriptor_diagnostic_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_033_catalog_descriptor_diagnostic_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_033_catalog_descriptor_diagnostic_runtime_conformance
  COMMAND sbsql_sbsfc_033_catalog_descriptor_diagnostic_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_033_catalog_descriptor_diagnostic_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_033_catalog_descriptor_diagnostic_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-033;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_034_text_trigram_bit_string_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_034_text_trigram_bit_string_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_034_text_trigram_bit_string_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_034_text_trigram_bit_string_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-034;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_034_text_trigram_bit_string_runtime_conformance
  generated/full_surface/sbsql_sbsfc_034_text_trigram_bit_string_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_034_text_trigram_bit_string_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_034_text_trigram_bit_string_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_034_text_trigram_bit_string_runtime_conformance
  COMMAND sbsql_sbsfc_034_text_trigram_bit_string_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_034_text_trigram_bit_string_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_034_text_trigram_bit_string_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-034;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_035_range_scalar_helper_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_035_range_scalar_helper_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_035_range_scalar_helper_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_035_range_scalar_helper_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-035;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_035_range_scalar_helper_runtime_conformance
  generated/full_surface/sbsql_sbsfc_035_range_scalar_helper_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_035_range_scalar_helper_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_035_range_scalar_helper_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_035_range_scalar_helper_runtime_conformance
  COMMAND sbsql_sbsfc_035_range_scalar_helper_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_035_range_scalar_helper_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_035_range_scalar_helper_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-035;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_036_spatial_geometry_scalar_helper_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_036_spatial_geometry_scalar_helper_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_036_spatial_geometry_scalar_helper_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_036_spatial_geometry_scalar_helper_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-036;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_036_spatial_geometry_scalar_helper_runtime_conformance
  generated/full_surface/sbsql_sbsfc_036_spatial_geometry_scalar_helper_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_036_spatial_geometry_scalar_helper_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_036_spatial_geometry_scalar_helper_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_036_spatial_geometry_scalar_helper_runtime_conformance
  COMMAND sbsql_sbsfc_036_spatial_geometry_scalar_helper_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_036_spatial_geometry_scalar_helper_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_036_spatial_geometry_scalar_helper_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-036;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_037_xml_multimodel_scalar_helper_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_037_xml_multimodel_scalar_helper_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_037_xml_multimodel_scalar_helper_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_037_xml_multimodel_scalar_helper_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-037;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_037_xml_multimodel_scalar_helper_runtime_conformance
  generated/full_surface/sbsql_sbsfc_037_xml_multimodel_scalar_helper_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_037_xml_multimodel_scalar_helper_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_037_xml_multimodel_scalar_helper_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_037_xml_multimodel_scalar_helper_runtime_conformance
  COMMAND sbsql_sbsfc_037_xml_multimodel_scalar_helper_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_037_xml_multimodel_scalar_helper_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_037_xml_multimodel_scalar_helper_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-037;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_038_spatial_tail_scalar_helper_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_038_spatial_tail_scalar_helper_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_038_spatial_tail_scalar_helper_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_038_spatial_tail_scalar_helper_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-038;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_038_spatial_tail_scalar_helper_runtime_conformance
  generated/full_surface/sbsql_sbsfc_038_spatial_tail_scalar_helper_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_038_spatial_tail_scalar_helper_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_038_spatial_tail_scalar_helper_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_038_spatial_tail_scalar_helper_runtime_conformance
  COMMAND sbsql_sbsfc_038_spatial_tail_scalar_helper_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_038_spatial_tail_scalar_helper_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_038_spatial_tail_scalar_helper_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-038;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_039_xml_document_query_scalar_helper_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_039_xml_document_query_scalar_helper_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_039_xml_document_query_scalar_helper_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_039_xml_document_query_scalar_helper_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-039;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_039_xml_document_query_scalar_helper_runtime_conformance
  generated/full_surface/sbsql_sbsfc_039_xml_document_query_scalar_helper_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_039_xml_document_query_scalar_helper_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_039_xml_document_query_scalar_helper_runtime_conformance PRIVATE sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_039_xml_document_query_scalar_helper_runtime_conformance
  COMMAND sbsql_sbsfc_039_xml_document_query_scalar_helper_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_039_xml_document_query_scalar_helper_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_039_xml_document_query_scalar_helper_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-039;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_044_catalog_statistics_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_044_catalog_statistics_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_044_catalog_statistics_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_044_catalog_statistics_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-044;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_044_catalog_statistics_runtime_conformance
  generated/full_surface/sbsql_sbsfc_044_catalog_statistics_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_044_catalog_statistics_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_044_catalog_statistics_runtime_conformance PRIVATE sb_engine_sblr sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_044_catalog_statistics_runtime_conformance
  COMMAND sbsql_sbsfc_044_catalog_statistics_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_044_catalog_statistics_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_044_catalog_statistics_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-044;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_045_privilege_predicate_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_045_privilege_predicate_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_045_privilege_predicate_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_045_privilege_predicate_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-045;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_045_privilege_predicate_runtime_conformance
  generated/full_surface/sbsql_sbsfc_045_privilege_predicate_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_045_privilege_predicate_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_045_privilege_predicate_runtime_conformance PRIVATE sb_engine_sblr sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_045_privilege_predicate_runtime_conformance
  COMMAND sbsql_sbsfc_045_privilege_predicate_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_045_privilege_predicate_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_045_privilege_predicate_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-045;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_046_session_admin_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_046_session_admin_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_046_session_admin_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_046_session_admin_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-046;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_046_session_admin_runtime_conformance
  generated/full_surface/sbsql_sbsfc_046_session_admin_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_046_session_admin_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_046_session_admin_runtime_conformance PRIVATE sb_engine_sblr sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_046_session_admin_runtime_conformance
  COMMAND sbsql_sbsfc_046_session_admin_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_046_session_admin_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_046_session_admin_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-046;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_047_advisory_lock_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_047_advisory_lock_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_047_advisory_lock_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_047_advisory_lock_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-047;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_047_advisory_lock_runtime_conformance
  generated/full_surface/sbsql_sbsfc_047_advisory_lock_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_047_advisory_lock_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_047_advisory_lock_runtime_conformance PRIVATE sb_engine_sblr sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_047_advisory_lock_runtime_conformance
  COMMAND sbsql_sbsfc_047_advisory_lock_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_047_advisory_lock_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_047_advisory_lock_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-047;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_048_advisory_lock_release_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_048_advisory_lock_release_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_048_advisory_lock_release_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_048_advisory_lock_release_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-048;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_048_advisory_lock_release_runtime_conformance
  generated/full_surface/sbsql_sbsfc_048_advisory_lock_release_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_048_advisory_lock_release_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_048_advisory_lock_release_runtime_conformance PRIVATE sb_engine_sblr sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_048_advisory_lock_release_runtime_conformance
  COMMAND sbsql_sbsfc_048_advisory_lock_release_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_048_advisory_lock_release_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_048_advisory_lock_release_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-048;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_049_unicode_text_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_049_unicode_text_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_049_unicode_text_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_049_unicode_text_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-049;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_049_unicode_text_runtime_conformance
  generated/full_surface/sbsql_sbsfc_049_unicode_text_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_049_unicode_text_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_049_unicode_text_runtime_conformance PRIVATE sb_engine_sblr sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_049_unicode_text_runtime_conformance
  COMMAND sbsql_sbsfc_049_unicode_text_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_049_unicode_text_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_049_unicode_text_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-049;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_052_json_document_construction_table_aggregate_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_052_json_document_construction_table_aggregate_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_052_json_document_construction_table_aggregate_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_052_json_document_construction_table_aggregate_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-052;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_052_json_document_construction_table_aggregate_runtime_conformance
  generated/full_surface/sbsql_sbsfc_052_json_document_construction_table_aggregate_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_052_json_document_construction_table_aggregate_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_052_json_document_construction_table_aggregate_runtime_conformance PRIVATE sb_engine_sblr sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_052_json_document_construction_table_aggregate_runtime_conformance
  COMMAND sbsql_sbsfc_052_json_document_construction_table_aggregate_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_052_json_document_construction_table_aggregate_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_052_json_document_construction_table_aggregate_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-052;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_053_rowset_table_value_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_053_rowset_table_value_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_053_rowset_table_value_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_053_rowset_table_value_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-053;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_053_rowset_table_value_runtime_conformance
  generated/full_surface/sbsql_sbsfc_053_rowset_table_value_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_053_rowset_table_value_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_053_rowset_table_value_runtime_conformance PRIVATE sb_engine_sblr sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_053_rowset_table_value_runtime_conformance
  COMMAND sbsql_sbsfc_053_rowset_table_value_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_053_rowset_table_value_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_053_rowset_table_value_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-053;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_054_cursor_stream_handle_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_054_cursor_stream_handle_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_054_cursor_stream_handle_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_054_cursor_stream_handle_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-054;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_054_cursor_stream_handle_runtime_conformance
  generated/full_surface/sbsql_sbsfc_054_cursor_stream_handle_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_054_cursor_stream_handle_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_054_cursor_stream_handle_runtime_conformance PRIVATE sb_engine_sblr sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_054_cursor_stream_handle_runtime_conformance
  COMMAND sbsql_sbsfc_054_cursor_stream_handle_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_054_cursor_stream_handle_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_054_cursor_stream_handle_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-054;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_055_lob_locator_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_055_lob_locator_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_055_lob_locator_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_055_lob_locator_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-055;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_055_lob_locator_runtime_conformance
  generated/full_surface/sbsql_sbsfc_055_lob_locator_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_055_lob_locator_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_055_lob_locator_runtime_conformance PRIVATE sb_engine_sblr sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_055_lob_locator_runtime_conformance
  COMMAND sbsql_sbsfc_055_lob_locator_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_055_lob_locator_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_055_lob_locator_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-055;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_056_native_surface_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_056_native_surface_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_056_native_surface_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_056_native_surface_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-056;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_056_native_surface_runtime_conformance
  generated/full_surface/sbsql_sbsfc_056_native_surface_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_056_native_surface_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_056_native_surface_runtime_conformance PRIVATE sb_engine_sblr sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_056_native_surface_runtime_conformance
  COMMAND sbsql_sbsfc_056_native_surface_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_056_native_surface_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_056_native_surface_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-056;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_057_crypto_hash_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_057_crypto_hash_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sbsfc_057_crypto_hash_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_057_crypto_hash_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-057;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_057_crypto_hash_runtime_conformance
  generated/full_surface/sbsql_sbsfc_057_crypto_hash_runtime_conformance.cpp
)
target_compile_features(sbsql_sbsfc_057_crypto_hash_runtime_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_057_crypto_hash_runtime_conformance PRIVATE sb_engine_sblr sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_057_crypto_hash_runtime_conformance
  COMMAND sbsql_sbsfc_057_crypto_hash_runtime_conformance
)
set_tests_properties(sbsql_sbsfc_057_crypto_hash_runtime_conformance PROPERTIES
  LABELS "sbsql_sbsfc_057_crypto_hash_runtime_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-057;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_058_expression_runtime_function_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_058_expression_runtime_function_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --artifact-root "${SBSQL_SURFACE_TO_SBLR_ARTIFACT_ROOT}"
)
set_tests_properties(sbsql_sbsfc_058_expression_runtime_function_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_058_expression_runtime_function_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-058;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_058_expression_runtime_function_conformance
  generated/full_surface/sbsql_sbsfc_058_expression_runtime_function_conformance.cpp
)
target_compile_features(sbsql_sbsfc_058_expression_runtime_function_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_058_expression_runtime_function_conformance PRIVATE sb_engine_sblr sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_058_expression_runtime_function_conformance
  COMMAND sbsql_sbsfc_058_expression_runtime_function_conformance
)
set_tests_properties(sbsql_sbsfc_058_expression_runtime_function_conformance PROPERTIES
  LABELS "sbsql_sbsfc_058_expression_runtime_function_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-058;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sbsfc_059_expression_runtime_function_cleanup_fixture_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_059_expression_runtime_function_cleanup_fixture_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --artifact-root "${SBSQL_SURFACE_TO_SBLR_ARTIFACT_ROOT}"
)
set_tests_properties(sbsql_sbsfc_059_expression_runtime_function_cleanup_fixture_gate PROPERTIES
  LABELS "sbsql_sbsfc_059_expression_runtime_function_cleanup_fixture_gate;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-059;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_sbsfc_059_expression_runtime_function_cleanup_conformance
  generated/full_surface/sbsql_sbsfc_059_expression_runtime_function_cleanup_conformance.cpp
)
target_compile_features(sbsql_sbsfc_059_expression_runtime_function_cleanup_conformance PRIVATE cxx_std_20)
target_link_libraries(sbsql_sbsfc_059_expression_runtime_function_cleanup_conformance PRIVATE sb_engine_sblr sb_engine_functions)
add_test(
  NAME sbsql_sbsfc_059_expression_runtime_function_cleanup_conformance
  COMMAND sbsql_sbsfc_059_expression_runtime_function_cleanup_conformance
)
set_tests_properties(sbsql_sbsfc_059_expression_runtime_function_cleanup_conformance PROPERTIES
  LABELS "sbsql_sbsfc_059_expression_runtime_function_cleanup_conformance;sbsql_surface_to_sblr_full_implementation_closure;SBSFC-059;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbp_sbsql_regression_test_bed_generation_gate
  generated/regression/sbsql_regression_test_bed_generation_gate.cpp
)
target_compile_features(sbp_sbsql_regression_test_bed_generation_gate PRIVATE cxx_std_23)
set(SBSQL_REGRESSION_TEST_BED_MANIFEST
  "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/regression/REGRESSION_TEST_BED_MANIFEST.csv"
)
add_test(
  NAME sbp_sbsql_regression_test_bed_generation_gate
  COMMAND sbp_sbsql_regression_test_bed_generation_gate
          "${SBSQL_REGRESSION_ARTIFACT_ROOT}"
          "${SBSQL_REGRESSION_TEST_BED_MANIFEST}"
)
set_tests_properties(sbp_sbsql_regression_test_bed_generation_gate PROPERTIES
  LABELS "sbsql_regression_test_bed_generation_gate;sbsql_parser_worker"
)

add_executable(
  sbp_sbsql_reference_alias_rendering_conformance
  generated/reference_alias/sbsql_reference_alias_rendering_conformance.cpp
)
target_compile_features(sbp_sbsql_reference_alias_rendering_conformance PRIVATE cxx_std_23)
set(SBSQL_REFERENCE_ALIAS_RENDERING_FIXTURES
  "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/reference_alias/REFERENCE_ALIAS_RENDERING_FIXTURES.csv"
)
add_test(
  NAME sbp_sbsql_reference_alias_rendering_conformance
  COMMAND sbp_sbsql_reference_alias_rendering_conformance
          "${SBSQL_REGRESSION_ARTIFACT_ROOT}"
          "${SBSQL_CANONICALIZATION_ROOT}"
          "${SBSQL_REFERENCE_ALIAS_RENDERING_FIXTURES}"
)
set_tests_properties(sbp_sbsql_reference_alias_rendering_conformance PROPERTIES
  LABELS "sbsql_reference_alias_mapping_conformance;sbsql_reference_alias_rendering_conformance;sbsql_parser_worker"
)

add_executable(
  sbp_sbsql_semantic_oracle_authority_gate
  generated/oracle/sbsql_semantic_oracle_authority_gate.cpp
)
target_compile_features(sbp_sbsql_semantic_oracle_authority_gate PRIVATE cxx_std_23)
add_test(
  NAME sbp_sbsql_semantic_oracle_authority_gate
  COMMAND sbp_sbsql_semantic_oracle_authority_gate
          "${SBSQL_REGRESSION_ARTIFACT_ROOT}"
          "${SBSQL_CANONICALIZATION_ROOT}"
          "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbp_sbsql_semantic_oracle_authority_gate PROPERTIES
  LABELS "sbsql_semantic_oracle_authority_gate;parser_v3_closure_gate;parser_conformance_oracle_gate;sbsql_parser_worker"
)

add_executable(
  sbp_sbsql_differential_replay_harness_gate
  generated/replay/sbsql_differential_replay_harness_gate.cpp
)
target_compile_features(sbp_sbsql_differential_replay_harness_gate PRIVATE cxx_std_23)
set(SBSQL_DIFFERENTIAL_REPLAY_ROOT
  "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/replay"
)
add_test(
  NAME sbp_sbsql_differential_replay_harness_gate
  COMMAND sbp_sbsql_differential_replay_harness_gate
          "${SBSQL_REGRESSION_ARTIFACT_ROOT}"
          "${SBSQL_CANONICALIZATION_ROOT}"
          "${SBSQL_DIFFERENTIAL_REPLAY_ROOT}"
          "${SBSQL_REFERENCE_ALIAS_RENDERING_FIXTURES}"
)
set_tests_properties(sbp_sbsql_differential_replay_harness_gate PROPERTIES
  LABELS "sbsql_differential_replay_harness_gate;sbsql_replay_parser_only;sbsql_replay_parser_bind_lower;sbsql_replay_udr;sbsql_replay_server_admission;sbsql_replay_engine_behavior;sbsql_replay_full_route;sbsql_replay_reference_alias;sbsql_replay_diagnostic;parser_v3_closure_gate;parser_conformance_oracle_gate;sbsql_parser_worker"
)

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sbu_sbsql_parser_support AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_bridge_command_route_conformance
    sbsql_bridge_command_route_conformance.cpp
  )
  target_compile_features(sbsql_bridge_command_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_bridge_command_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sbu_sbsql_parser_support
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_bridge_command_route_conformance
    COMMAND sbsql_bridge_command_route_conformance
  )
  set_tests_properties(sbsql_bridge_command_route_conformance PROPERTIES
    LABELS "sbsql_bridge_command_route_conformance;trusted_udr_bridge_gate;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_parser_worker;mga_transaction_regression"
  )

  add_executable(
    sbsql_exhaustive_e2e_regression_gate
    generated/exhaustive_e2e/sbsql_exhaustive_e2e_regression_gate.cpp
  )
  target_compile_features(sbsql_exhaustive_e2e_regression_gate PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_exhaustive_e2e_regression_gate
    PRIVATE
      sbl_sbsql_parser_pipeline
      sbu_sbsql_parser_support
      sb_server_core
      sb_engine_sblr
      sb_core_memory
  )
  add_test(
    NAME sbsql_exhaustive_e2e_regression_gate
    COMMAND sbsql_exhaustive_e2e_regression_gate
            "${SBSQL_REGRESSION_ARTIFACT_ROOT}"
            "${SBSQL_CANONICALIZATION_ROOT}"
            "${SBSQL_DIFFERENTIAL_REPLAY_ROOT}"
            "${SBSQL_REFERENCE_ALIAS_RENDERING_FIXTURES}"
            "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/exhaustive_e2e/EXHAUSTIVE_E2E_VARIATION_MATRIX.csv"
  )
  set_tests_properties(sbsql_exhaustive_e2e_regression_gate PROPERTIES
    LABELS "sbsql_exhaustive_e2e_regression;sbsql_emulation_parser_regression;sbsql_dynamic_udr_sblr_regression;sbsql_parser_worker"
  )
endif()

add_executable(
  sbp_sbsql_canary_vertical_slice_probe
  generated/canary/sbsql_canary_vertical_slice_probe.cpp
)
target_compile_features(sbp_sbsql_canary_vertical_slice_probe PRIVATE cxx_std_23)
target_link_libraries(sbp_sbsql_canary_vertical_slice_probe PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbp_sbsql_canary_vertical_slice_probe
  COMMAND sbp_sbsql_canary_vertical_slice_probe "${SBSQL_REGRESSION_ARTIFACT_ROOT}"
)
set_tests_properties(sbp_sbsql_canary_vertical_slice_probe PROPERTIES
  LABELS "sbsql_canary_vertical_slice_gate;sbsql_parser_worker"
)

add_executable(
  sbp_sbsql_lexer_conformance_probe
  generated/lexer/sbsql_lexer_conformance_probe.cpp
)
target_compile_features(sbp_sbsql_lexer_conformance_probe PRIVATE cxx_std_23)
target_link_libraries(sbp_sbsql_lexer_conformance_probe PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbp_sbsql_lexer_conformance_probe
  COMMAND sbp_sbsql_lexer_conformance_probe "${SBSQL_REGRESSION_ARTIFACT_ROOT}"
)
set_tests_properties(sbp_sbsql_lexer_conformance_probe PROPERTIES
  LABELS "sbsql_lexer_conformance;sbsql_parser_worker"
)

add_executable(
  sbp_sbsql_cst_ast_conformance_probe
  generated/cst_ast/sbsql_cst_ast_conformance_probe.cpp
)
target_compile_features(sbp_sbsql_cst_ast_conformance_probe PRIVATE cxx_std_23)
target_link_libraries(sbp_sbsql_cst_ast_conformance_probe PRIVATE sbl_sbsql_parser_pipeline)
add_test(NAME sbp_sbsql_cst_ast_conformance_probe COMMAND sbp_sbsql_cst_ast_conformance_probe)
set_tests_properties(sbp_sbsql_cst_ast_conformance_probe PROPERTIES
  LABELS "sbsql_cst_ast_conformance;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbp_sbsql_expression_builtin_conformance_probe
  generated/expression/sbsql_expression_builtin_conformance_probe.cpp
)
target_compile_features(sbp_sbsql_expression_builtin_conformance_probe PRIVATE cxx_std_23)
target_link_libraries(sbp_sbsql_expression_builtin_conformance_probe PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbp_sbsql_expression_builtin_conformance_probe
  COMMAND sbp_sbsql_expression_builtin_conformance_probe
)
set_tests_properties(sbp_sbsql_expression_builtin_conformance_probe PROPERTIES
  LABELS "sbsql_expression_builtin_conformance;sbsql_parser_worker"
)

add_executable(
  sbp_sbsql_statement_family_conformance_probe
  generated/statement/sbsql_statement_family_conformance_probe.cpp
)
target_compile_features(sbp_sbsql_statement_family_conformance_probe PRIVATE cxx_std_23)
target_link_libraries(sbp_sbsql_statement_family_conformance_probe PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbp_sbsql_statement_family_conformance_probe
  COMMAND sbp_sbsql_statement_family_conformance_probe
)
set_tests_properties(sbp_sbsql_statement_family_conformance_probe PROPERTIES
  LABELS "sbsql_statement_family_conformance;sbsql_parser_worker"
)

add_executable(
  sbp_sbsql_binder_authority_conformance_probe
  generated/binder/sbsql_binder_authority_conformance_probe.cpp
)
target_compile_features(sbp_sbsql_binder_authority_conformance_probe PRIVATE cxx_std_23)
target_link_libraries(sbp_sbsql_binder_authority_conformance_probe PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbp_sbsql_binder_authority_conformance_probe
  COMMAND sbp_sbsql_binder_authority_conformance_probe
)
set_tests_properties(sbp_sbsql_binder_authority_conformance_probe PROPERTIES
  LABELS "sbsql_binder_authority_conformance;sbsql_parser_worker"
)

add_executable(
  sbp_sbsql_sblr_lowering_verifier_conformance_probe
  generated/lowering/sbsql_sblr_lowering_verifier_conformance_probe.cpp
)
target_compile_features(sbp_sbsql_sblr_lowering_verifier_conformance_probe PRIVATE cxx_std_23)
target_link_libraries(sbp_sbsql_sblr_lowering_verifier_conformance_probe PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbp_sbsql_sblr_lowering_verifier_conformance_probe
  COMMAND sbp_sbsql_sblr_lowering_verifier_conformance_probe
)
set_tests_properties(sbp_sbsql_sblr_lowering_verifier_conformance_probe PROPERTIES
  LABELS "sbsql_sblr_lowering_verifier_conformance;sblr_operation_matrix_gate;sblr_lowering_runtime_gate;parser_v3_closure_gate;sbsql_parser_worker"
)

add_executable(
  sbsql_constraint_ddl_lowering_conformance
  constraint_ddl_lowering_conformance.cpp
)
target_compile_features(sbsql_constraint_ddl_lowering_conformance PRIVATE cxx_std_23)
target_link_libraries(sbsql_constraint_ddl_lowering_conformance PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbsql_constraint_ddl_lowering_conformance
  COMMAND sbsql_constraint_ddl_lowering_conformance
)
set_tests_properties(sbsql_constraint_ddl_lowering_conformance PROPERTIES
  LABELS "constraint_ddl_gate;PRF_G06_CONSTRAINTS_READY;sbsql_parser_worker"
)

add_executable(
  sbsql_synonym_ddl_lowering_conformance
  sbsql_synonym_ddl_lowering_conformance.cpp
)
target_compile_features(sbsql_synonym_ddl_lowering_conformance PRIVATE cxx_std_23)
target_link_libraries(sbsql_synonym_ddl_lowering_conformance PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbsql_synonym_ddl_lowering_conformance
  COMMAND sbsql_synonym_ddl_lowering_conformance
)
set_tests_properties(sbsql_synonym_ddl_lowering_conformance PROPERTIES
  LABELS "sbsql_synonym_ddl_lowering_conformance;sbsql_catalog_synonym_gate;sbsql_parser_worker"
)

if(TARGET sb_engine_sblr)
  add_executable(
    sbsql_sblr_source_artifact_runtime_conformance
    sbsql_sblr_source_artifact_runtime_conformance.cpp
  )
  target_compile_features(sbsql_sblr_source_artifact_runtime_conformance PRIVATE cxx_std_23)
  target_link_libraries(sbsql_sblr_source_artifact_runtime_conformance PRIVATE sb_engine_sblr)
  add_test(
    NAME sbsql_sblr_source_artifact_runtime_conformance
    COMMAND sbsql_sblr_source_artifact_runtime_conformance
  )
  set_tests_properties(sbsql_sblr_source_artifact_runtime_conformance PROPERTIES
    LABELS "sbsql_sblr_source_artifact_runtime_conformance;sblr_source_artifact_reversal;sbsql_parser_worker"
  )

  add_executable(
    cdp_sblr_registry_round_trip_gate
    cdp_sblr_registry_round_trip_gate.cpp
  )
  target_compile_features(cdp_sblr_registry_round_trip_gate PRIVATE cxx_std_23)
  target_link_libraries(
    cdp_sblr_registry_round_trip_gate
    PRIVATE
      sb_engine_sblr
      sb_core_uuid
  )
  add_test(
    NAME cdp_sblr_registry_round_trip_gate
    COMMAND cdp_sblr_registry_round_trip_gate
  )
  set_tests_properties(cdp_sblr_registry_round_trip_gate PROPERTIES
    LABELS "cdp_sblr_registry_round_trip_gate;CDP-025;CDP_P2_SBLR_REGISTRY_ROUND_TRIP_READY;sblr_opcode_registry_conformance;sblr_to_sbsql_conversion;sbsql_parser_worker;mga_transaction_regression"
  )

  if(TARGET sbu_sbsql_parser_support)
    add_executable(
      sbsql_sblr_to_sbsql_conversion_route_conformance
      sbsql_sblr_to_sbsql_conversion_route_conformance.cpp
    )
    target_compile_features(sbsql_sblr_to_sbsql_conversion_route_conformance PRIVATE cxx_std_23)
    target_link_libraries(
      sbsql_sblr_to_sbsql_conversion_route_conformance
      PRIVATE
      sb_engine_sblr
      sbu_sbsql_parser_support
    )
    add_test(
      NAME sbsql_sblr_to_sbsql_conversion_route_conformance
      COMMAND sbsql_sblr_to_sbsql_conversion_route_conformance
    )
    set_tests_properties(sbsql_sblr_to_sbsql_conversion_route_conformance PROPERTIES
      LABELS "sbsql_sblr_to_sbsql_conversion_route_conformance;sblr_to_sbsql_conversion;sbsql_parser_worker"
    )

    add_executable(
      sbsql_sblr_source_roundtrip_closure_conformance
      sbsql_sblr_source_roundtrip_closure_conformance.cpp
    )
    target_compile_features(sbsql_sblr_source_roundtrip_closure_conformance PRIVATE cxx_std_23)
    target_include_directories(
      sbsql_sblr_source_roundtrip_closure_conformance
      PRIVATE
      ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(
      sbsql_sblr_source_roundtrip_closure_conformance
      PRIVATE
      sb_engine_sblr
      sbu_sbsql_parser_support
    )
    add_test(
      NAME sbsql_sblr_source_roundtrip_closure_conformance
      COMMAND sbsql_sblr_source_roundtrip_closure_conformance
    )
    set_tests_properties(sbsql_sblr_source_roundtrip_closure_conformance PROPERTIES
      LABELS "sbsql_sblr_source_roundtrip_closure_conformance;sblr_source_artifact_reversal;sblr_to_sbsql_conversion;sbsql_parser_worker"
    )
  endif()

  add_executable(
    sbsql_cluster_provider_conformance
    sbsql_cluster_provider_conformance.cpp
  )
  target_compile_features(sbsql_cluster_provider_conformance PRIVATE cxx_std_23)
  target_link_libraries(sbsql_cluster_provider_conformance PRIVATE sb_engine_sblr)
  if(SB_ENABLE_CLUSTER_PROVIDER AND SB_CLUSTER_PROVIDER_STUB)
    set(SBSQL_CLUSTER_PROVIDER_CONFORMANCE_TEST_NAME sbsql_cluster_provider_stub_conformance)
  elseif(SB_ENABLE_CLUSTER_PROVIDER AND SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY)
    set(SBSQL_CLUSTER_PROVIDER_CONFORMANCE_TEST_NAME sbsql_cluster_provider_external_conformance)
  else()
    set(SBSQL_CLUSTER_PROVIDER_CONFORMANCE_TEST_NAME cluster_provider_no_cluster_error_vector_conformance)
  endif()
  add_test(
    NAME ${SBSQL_CLUSTER_PROVIDER_CONFORMANCE_TEST_NAME}
    COMMAND sbsql_cluster_provider_conformance
  )
  set_tests_properties(${SBSQL_CLUSTER_PROVIDER_CONFORMANCE_TEST_NAME} PROPERTIES
    LABELS "sbsql_cluster_provider_conformance;sbsql_surface_to_sblr_full_implementation_closure;sblr_operation_matrix_gate;sbsql_parser_worker"
  )

  if(TARGET sbu_sbsql_parser_support AND TARGET sb_udr_runtime)
    add_executable(
      sbsql_extension_boundary_abi_manifest_conformance
      sbsql_extension_boundary_abi_manifest_conformance.cpp
    )
    target_compile_features(sbsql_extension_boundary_abi_manifest_conformance PRIVATE cxx_std_23)
    target_link_libraries(
      sbsql_extension_boundary_abi_manifest_conformance
      PRIVATE
      sb_engine_sblr
      sbu_sbsql_parser_support
      sb_udr_runtime
    )
    add_test(
      NAME sbsql_extension_boundary_abi_manifest_conformance
      COMMAND sbsql_extension_boundary_abi_manifest_conformance
    )
    set_tests_properties(sbsql_extension_boundary_abi_manifest_conformance PROPERTIES
      LABELS "sbsql_extension_boundary_abi_manifest_conformance;extension_boundary;extension_abi_manifest;parser_package;udr_package;cluster_provider;sbsql_parser_worker"
    )
  endif()

  add_executable(
    sbsql_synonym_sblr_dispatch_conformance
    sbsql_synonym_sblr_dispatch_conformance.cpp
  )
  target_compile_features(sbsql_synonym_sblr_dispatch_conformance PRIVATE cxx_std_23)
  target_link_libraries(sbsql_synonym_sblr_dispatch_conformance PRIVATE sb_engine_sblr)
  add_test(
    NAME sbsql_synonym_sblr_dispatch_conformance
    COMMAND sbsql_synonym_sblr_dispatch_conformance
  )
  set_tests_properties(sbsql_synonym_sblr_dispatch_conformance PROPERTIES
    LABELS "sbsql_synonym_sblr_dispatch_conformance;sbsql_catalog_synonym_gate;sblr_operation_matrix_gate;sbsql_parser_worker"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr AND TARGET sb_storage_database AND TARGET sb_core_uuid)
  add_executable(
    sbsql_create_schema_exact_route_conformance
    sbsql_create_schema_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_create_schema_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_create_schema_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
      sb_storage_database
      sb_core_uuid
  )
  add_test(
    NAME sbsql_create_schema_exact_route_conformance
    COMMAND sbsql_create_schema_exact_route_conformance
  )
  set_tests_properties(sbsql_create_schema_exact_route_conformance PROPERTIES
    LABELS "sbsql_create_schema_exact_route_conformance;SBSFC-020R-G;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_cast_value_exact_route_conformance
    sbsql_cast_value_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_cast_value_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_cast_value_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
      sb_engine_functions
  )
  add_test(
    NAME sbsql_cast_value_exact_route_conformance
    COMMAND sbsql_cast_value_exact_route_conformance
  )
  set_tests_properties(sbsql_cast_value_exact_route_conformance PROPERTIES
    LABELS "sbsql_cast_value_exact_route_conformance;SBSFC-011R-N;SBSFC-011R-S;SBSFC-011R-T;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_between_exact_route_conformance
    sbsql_between_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_between_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_between_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
      sb_core_memory
  )
  add_test(
    NAME sbsql_between_exact_route_conformance
    COMMAND sbsql_between_exact_route_conformance
  )
  set_tests_properties(sbsql_between_exact_route_conformance PROPERTIES
    LABELS "sbsql_between_exact_route_conformance;SBSFC-011R-O;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_in_exact_route_conformance
    sbsql_in_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_in_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_in_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_in_exact_route_conformance
    COMMAND sbsql_in_exact_route_conformance
  )
  set_tests_properties(sbsql_in_exact_route_conformance PROPERTIES
    LABELS "sbsql_in_exact_route_conformance;SBSFC-011R-P;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_case_exact_route_conformance
    sbsql_case_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_case_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_case_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_case_exact_route_conformance
    COMMAND sbsql_case_exact_route_conformance
  )
  set_tests_properties(sbsql_case_exact_route_conformance PROPERTIES
    LABELS "sbsql_case_exact_route_conformance;SBSFC-011R-Q;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_simple_case_exact_route_conformance
    sbsql_simple_case_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_simple_case_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_simple_case_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_simple_case_exact_route_conformance
    COMMAND sbsql_simple_case_exact_route_conformance
  )
  set_tests_properties(sbsql_simple_case_exact_route_conformance PROPERTIES
    LABELS "sbsql_simple_case_exact_route_conformance;SBSFC-011R-R;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_multiplicative_expr_exact_route_conformance
    sbsql_multiplicative_expr_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_multiplicative_expr_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_multiplicative_expr_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_multiplicative_expr_exact_route_conformance
    COMMAND sbsql_multiplicative_expr_exact_route_conformance
  )
  set_tests_properties(sbsql_multiplicative_expr_exact_route_conformance PROPERTIES
    LABELS "sbsql_multiplicative_expr_exact_route_conformance;SBSFC-014R-K;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_additive_expr_exact_route_conformance
    sbsql_additive_expr_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_additive_expr_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_additive_expr_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_additive_expr_exact_route_conformance
    COMMAND sbsql_additive_expr_exact_route_conformance
  )
  set_tests_properties(sbsql_additive_expr_exact_route_conformance PROPERTIES
    LABELS "sbsql_additive_expr_exact_route_conformance;SBSFC-014R-L;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_unary_expr_exact_route_conformance
    sbsql_unary_expr_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_unary_expr_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_unary_expr_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_unary_expr_exact_route_conformance
    COMMAND sbsql_unary_expr_exact_route_conformance
  )
  set_tests_properties(sbsql_unary_expr_exact_route_conformance PROPERTIES
    LABELS "sbsql_unary_expr_exact_route_conformance;SBSFC-014R-M;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_primary_expr_exact_route_conformance
    sbsql_primary_expr_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_primary_expr_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_primary_expr_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_primary_expr_exact_route_conformance
    COMMAND sbsql_primary_expr_exact_route_conformance
  )
  set_tests_properties(sbsql_primary_expr_exact_route_conformance PROPERTIES
    LABELS "sbsql_primary_expr_exact_route_conformance;SBSFC-014R-N;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_expression_exact_route_conformance
    sbsql_expression_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_expression_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_expression_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_expression_exact_route_conformance
    COMMAND sbsql_expression_exact_route_conformance
  )
  set_tests_properties(sbsql_expression_exact_route_conformance PROPERTIES
    LABELS "sbsql_expression_exact_route_conformance;SBSFC-014R-O;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_scalar_syntax_exact_route_conformance
    sbsql_scalar_syntax_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_scalar_syntax_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_scalar_syntax_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_scalar_syntax_exact_route_conformance
    COMMAND sbsql_scalar_syntax_exact_route_conformance
  )
  set_tests_properties(sbsql_scalar_syntax_exact_route_conformance PROPERTIES
    LABELS "sbsql_scalar_syntax_exact_route_conformance;SBSFC-014R-P;SBSFC-014R-Q;SBSFC-014R-R;SBSFC-014R-S;SBSFC-014R-T;SBSFC-014R-U;SBSFC-014R-V;SBSFC-014R-W;SBSFC-014R-X;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_060r_xml_grammar_exact_route_conformance
    sbsql_sbsfc_060r_xml_grammar_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_060r_xml_grammar_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_060r_xml_grammar_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_060r_xml_grammar_exact_route_conformance
    COMMAND sbsql_sbsfc_060r_xml_grammar_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_060r_xml_grammar_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_060r_xml_grammar_exact_route_conformance;SBSFC-060R;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_061_json_grammar_exact_route_conformance
    sbsql_sbsfc_061_json_grammar_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_061_json_grammar_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_061_json_grammar_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_061_json_grammar_exact_route_conformance
    COMMAND sbsql_sbsfc_061_json_grammar_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_061_json_grammar_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_061_json_grammar_exact_route_conformance;SBSFC-061;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_062_vector_grammar_exact_route_conformance
    sbsql_sbsfc_062_vector_grammar_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_062_vector_grammar_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_062_vector_grammar_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_062_vector_grammar_exact_route_conformance
    COMMAND sbsql_sbsfc_062_vector_grammar_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_062_vector_grammar_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_062_vector_grammar_exact_route_conformance;SBSFC-062;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_063_window_grammar_exact_route_conformance
    sbsql_sbsfc_063_window_grammar_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_063_window_grammar_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_063_window_grammar_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_063_window_grammar_exact_route_conformance
    COMMAND sbsql_sbsfc_063_window_grammar_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_063_window_grammar_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_063_window_grammar_exact_route_conformance;SBSFC-063;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_064_parameter_sample_grammar_exact_route_conformance
    sbsql_sbsfc_064_parameter_sample_grammar_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_064_parameter_sample_grammar_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_064_parameter_sample_grammar_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_064_parameter_sample_grammar_exact_route_conformance
    COMMAND sbsql_sbsfc_064_parameter_sample_grammar_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_064_parameter_sample_grammar_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_064_parameter_sample_grammar_exact_route_conformance;SBSFC-064;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_065_pivot_unpivot_grammar_exact_route_conformance
    sbsql_sbsfc_065_pivot_unpivot_grammar_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_065_pivot_unpivot_grammar_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_065_pivot_unpivot_grammar_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_065_pivot_unpivot_grammar_exact_route_conformance
    COMMAND sbsql_sbsfc_065_pivot_unpivot_grammar_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_065_pivot_unpivot_grammar_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_065_pivot_unpivot_grammar_exact_route_conformance;SBSFC-065;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_066_case_grammar_exact_route_conformance
    sbsql_sbsfc_066_case_grammar_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_066_case_grammar_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_066_case_grammar_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_066_case_grammar_exact_route_conformance
    COMMAND sbsql_sbsfc_066_case_grammar_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_066_case_grammar_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_066_case_grammar_exact_route_conformance;SBSFC-066;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_067_on_conflict_exact_route_conformance
    sbsql_sbsfc_067_on_conflict_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_067_on_conflict_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_067_on_conflict_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_067_on_conflict_exact_route_conformance
    COMMAND sbsql_sbsfc_067_on_conflict_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_067_on_conflict_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_067_on_conflict_exact_route_conformance;SBSFC-067;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_068_prepared_statement_control_conformance
    sbsql_sbsfc_068_prepared_statement_control_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_068_prepared_statement_control_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_068_prepared_statement_control_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_068_prepared_statement_control_conformance
    COMMAND sbsql_sbsfc_068_prepared_statement_control_conformance
  )
  set_tests_properties(sbsql_sbsfc_068_prepared_statement_control_conformance PROPERTIES
    LABELS "sbsql_sbsfc_068_prepared_statement_control_conformance;SBSFC-068;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_069_comment_trivia_conformance
    sbsql_sbsfc_069_comment_trivia_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_069_comment_trivia_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_069_comment_trivia_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_069_comment_trivia_conformance
    COMMAND sbsql_sbsfc_069_comment_trivia_conformance
  )
  set_tests_properties(sbsql_sbsfc_069_comment_trivia_conformance PROPERTIES
    LABELS "sbsql_sbsfc_069_comment_trivia_conformance;SBSFC-069;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_070_psql_cursor_control_conformance
    sbsql_sbsfc_070_psql_cursor_control_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_070_psql_cursor_control_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_070_psql_cursor_control_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_070_psql_cursor_control_conformance
    COMMAND sbsql_sbsfc_070_psql_cursor_control_conformance
  )
  set_tests_properties(sbsql_sbsfc_070_psql_cursor_control_conformance PROPERTIES
    LABELS "sbsql_sbsfc_070_psql_cursor_control_conformance;SBSFC-070;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_071_jobs_scheduler_conformance
    sbsql_sbsfc_071_jobs_scheduler_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_071_jobs_scheduler_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_071_jobs_scheduler_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_071_jobs_scheduler_conformance
    COMMAND sbsql_sbsfc_071_jobs_scheduler_conformance
  )
  set_tests_properties(sbsql_sbsfc_071_jobs_scheduler_conformance PROPERTIES
    LABELS "sbsql_sbsfc_071_jobs_scheduler_conformance;SBSFC-071;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_072_jobs_schedule_conformance
    sbsql_sbsfc_072_jobs_schedule_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_072_jobs_schedule_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_072_jobs_schedule_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_072_jobs_schedule_conformance
    COMMAND sbsql_sbsfc_072_jobs_schedule_conformance
  )
  set_tests_properties(sbsql_sbsfc_072_jobs_schedule_conformance PROPERTIES
    LABELS "sbsql_sbsfc_072_jobs_schedule_conformance;SBSFC-072;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_073_archive_replication_conformance
    sbsql_sbsfc_073_archive_replication_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_073_archive_replication_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_073_archive_replication_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_073_archive_replication_conformance
    COMMAND sbsql_sbsfc_073_archive_replication_conformance
  )
  set_tests_properties(sbsql_sbsfc_073_archive_replication_conformance PROPERTIES
    LABELS "sbsql_sbsfc_073_archive_replication_conformance;SBSFC-073;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_074_multimodel_grammar_exact_route_conformance
    sbsql_sbsfc_074_multimodel_grammar_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_074_multimodel_grammar_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_074_multimodel_grammar_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_074_multimodel_grammar_exact_route_conformance
    COMMAND sbsql_sbsfc_074_multimodel_grammar_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_074_multimodel_grammar_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_074_multimodel_grammar_exact_route_conformance;SBSFC-074;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_create_table_exact_route_conformance
    sbsql_create_table_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_create_table_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_create_table_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_create_table_exact_route_conformance
    COMMAND sbsql_create_table_exact_route_conformance
  )
  set_tests_properties(sbsql_create_table_exact_route_conformance PROPERTIES
    LABELS "sbsql_create_table_exact_route_conformance;SBSFC-020R-A;SBSFC-020R-H;SBSFC-020R-I;SBSFC-020R-J;SBSFC-020R-K;SBSFC-020R-L;SBSFC-020R-M;SBSFC-020R-N;SBSFC-020R-O;SBSFC-020R-P;SBSFC-020R-Q;SBSFC-020R-R;SBSFC-020R-S;SBSFC-020R-T;SBSFC-020R-U;SBSFC-020R-V;SBSFC-020R-W;SBSFC-020R-X;SBSFC-020R-Y;SBSFC-020R-Z;SBSFC-020R-AA;SBSFC-020R-AB;SBSFC-020R-AC;SBSFC-020R-AD;SBSFC-020R-AE;SBSFC-020R-AF;SBSFC-020R-AG;SBSFC-020R-AH;SBSFC-020R-AI;SBSFC-020R-AJ;SBSFC-020R-AK;SBSFC-020R-AL;SBSFC-020R-AM;SBSFC-020R-AN;SBSFC-020R-AO;SBSFC-020R-AP;SBSFC-020R-AQ;SBSFC-020R-AR;SBSFC-020R-AS;SBSFC-020R-AT;SBSFC-020R-AU;SBSFC-020R-AV;SBSFC-020R-AW;SBSFC-020R-AX;SBSFC-020R-AY;SBSFC-020R-AZ;SBSFC-020R-BA;SBSFC-020R-BB;SBSFC-020R-BC;SBSFC-020R-BD;SBSFC-020R-BE;SBSFC-020R-BF;SBSFC-020R-BG;SBSFC-020R-BH;SBSFC-020R-BI;SBSFC-020R-BJ;SBSFC-020R-BK;SBSFC-020R-BL;SBSFC-020R-BM;SBSFC-020R-BN;SBSFC-020R-BO;SBSFC-020R-BP;SBSFC-020R-BQ;SBSFC-020R-BR;SBSFC-020R-BS;SBSFC-020R-BT;SBSFC-020R-BU;SBSFC-020R-BV;SBSFC-020R-BW;SBSFC-020R-BX;SBSFC-020R-BY;SBSFC-020R-BZ;SBSFC-020R-CA;SBSFC-020R-CB;SBSFC-020R-CC;SBSFC-020R-CD;SBSFC-020R-CE;SBSFC-020R-CF;SBSFC-020R-CG;SBSFC-020R-CH;SBSFC-020R-CI;SBSFC-020R-CJ;SBSFC-020R-CK;SBSFC-020R-CL;SBSFC-020R-CM;SBSFC-020R-CN;SBSFC-020R-CO;SBSFC-020R-CP;SBSFC-020R-CQ;SBSFC-020R-CR;SBSFC-020R-CS;SBSFC-020R-CT;SBSFC-020R-CU;SBSFC-020R-CV;SBSFC-020R-CW;SBSFC-020R-CX;SBSFC-020R-CY;SBSFC-020R-CZ;SBSFC-020R-DA;SBSFC-020R-DB;SBSFC-020R-DC;SBSFC-020R-DD;SBSFC-020R-DE;SBSFC-020R-DF;SBSFC-020R-DG;SBSFC-020R-DH;SBSFC-020R-DI;SBSFC-020R-DJ;SBSFC-020R-DK;SBSFC-020R-DL;SBSFC-020R-DM;SBSFC-020R-DN;SBSFC-020R-DO;SBSFC-020R-DP;SBSFC-020R-DQ;SBSFC-020R-DR;SBSFC-020R-DS;SBSFC-020R-DT;SBSFC-020R-DU;SBSFC-020R-DV;SBSFC-020R-DW;SBSFC-020R-DX;SBSFC-020R-DY;SBSFC-020R-DZ;SBSFC-020R-EA;SBSFC-020R-EB;SBSFC-020R-EC;SBSFC-020R-ED;SBSFC-020R-EE;SBSFC-020R-EF;SBSFC-020R-EG;SBSFC-020R-EH;SBSFC-020R-EI;SBSFC-020R-EJ;SBSFC-020R-EK;SBSFC-020R-EL;SBSFC-020R-EM;SBSFC-020R-EN;SBSFC-020R-EO;SBSFC-020R-EP;SBSFC-020R-EQ;SBSFC-020R-ER;SBSFC-020R-ES;SBSFC-020R-ET;SBSFC-020R-EU;SBSFC-020R-EV;SBSFC-020R-EW;SBSFC-020R-EX;SBSFC-020R-EY;SBSFC-020R-EZ;SBSFC-020R-FA;SBSFC-020R-FB;SBSFC-020R-FC;SBSFC-020R-FD;SBSFC-020R-FE;SBSFC-020R-FF;SBSFC-020R-FG;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_temporary_table_proof_closure_conformance
    sbsql_temporary_table_proof_closure_conformance.cpp
  )
  target_compile_features(sbsql_temporary_table_proof_closure_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_temporary_table_proof_closure_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_temporary_table_proof_closure_conformance
    COMMAND sbsql_temporary_table_proof_closure_conformance
  )
  set_tests_properties(sbsql_temporary_table_proof_closure_conformance PROPERTIES
    LABELS "sbsql_temporary_table_proof_closure_conformance;SBSQL-TEMPORARY-TABLE-PROOF-CLOSURE;TEMP-TABLE-GATE-001;TEMP-TABLE-GATE-002;TEMP-TABLE-GATE-003;TEMP-TABLE-GATE-004;TEMP-TABLE-GATE-005;TEMP-TABLE-GATE-006;TEMP-TABLE-GATE-007;TEMP-TABLE-GATE-008;TEMP-TABLE-GATE-009;TEMP-TABLE-GATE-010;TEMP-TABLE-GATE-011;TEMP-TABLE-GATE-012;TEMP-TABLE-GATE-013;TEMP-TABLE-GATE-014;TEMP-TABLE-GATE-015;TEMP-TABLE-GATE-016;TEMP-TABLE-GATE-017;sblr_operation_matrix_gate;mga_transaction_regression;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_create_statistics_exact_route_conformance
    sbsql_create_statistics_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_create_statistics_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_create_statistics_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_create_statistics_exact_route_conformance
    COMMAND sbsql_create_statistics_exact_route_conformance
  )
  set_tests_properties(sbsql_create_statistics_exact_route_conformance PROPERTIES
    LABELS "sbsql_create_statistics_exact_route_conformance;SBSFC-020R-QL;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_create_index_exact_route_conformance
    sbsql_create_index_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_create_index_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_create_index_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_create_index_exact_route_conformance
    COMMAND sbsql_create_index_exact_route_conformance
  )
  set_tests_properties(sbsql_create_index_exact_route_conformance PROPERTIES
    LABELS "sbsql_create_index_exact_route_conformance;SBSFC-020R-QP;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_create_index_template_exact_route_conformance
    sbsql_create_index_template_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_create_index_template_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_create_index_template_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_create_index_template_exact_route_conformance
    COMMAND sbsql_create_index_template_exact_route_conformance
  )
  set_tests_properties(sbsql_create_index_template_exact_route_conformance PROPERTIES
    LABELS "sbsql_create_index_template_exact_route_conformance;SBSFC-020R-QQ;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_comment_on_exact_route_conformance
    sbsql_comment_on_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_comment_on_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_comment_on_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_comment_on_exact_route_conformance
    COMMAND sbsql_comment_on_exact_route_conformance
  )
  set_tests_properties(sbsql_comment_on_exact_route_conformance PROPERTIES
    LABELS "sbsql_comment_on_exact_route_conformance;SBSFC-020R-QR;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_alter_rename_exact_route_conformance
    sbsql_alter_rename_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_alter_rename_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_alter_rename_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_alter_rename_exact_route_conformance
    COMMAND sbsql_alter_rename_exact_route_conformance
  )
  set_tests_properties(sbsql_alter_rename_exact_route_conformance PROPERTIES
    LABELS "sbsql_alter_rename_exact_route_conformance;SBSFC-020R-QS;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_drop_object_exact_route_conformance
    sbsql_drop_object_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_drop_object_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_drop_object_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_drop_object_exact_route_conformance
    COMMAND sbsql_drop_object_exact_route_conformance
  )
  set_tests_properties(sbsql_drop_object_exact_route_conformance PROPERTIES
    LABELS "sbsql_drop_object_exact_route_conformance;SBSFC-020R-QT;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_create_sequence_exact_route_conformance
    sbsql_create_sequence_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_create_sequence_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_create_sequence_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_create_sequence_exact_route_conformance
    COMMAND sbsql_create_sequence_exact_route_conformance
  )
  set_tests_properties(sbsql_create_sequence_exact_route_conformance PROPERTIES
    LABELS "sbsql_create_sequence_exact_route_conformance;SBSFC-020R-QM;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_create_domain_exact_route_conformance
    sbsql_create_domain_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_create_domain_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_create_domain_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_create_domain_exact_route_conformance
    COMMAND sbsql_create_domain_exact_route_conformance
  )
  set_tests_properties(sbsql_create_domain_exact_route_conformance PROPERTIES
    LABELS "sbsql_create_domain_exact_route_conformance;SBSFC-020R-QO;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_domain_policy_predicate_closure_conformance
    sbsql_domain_policy_predicate_closure_conformance.cpp
  )
  target_compile_features(sbsql_domain_policy_predicate_closure_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_domain_policy_predicate_closure_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_domain_policy_predicate_closure_conformance
    COMMAND sbsql_domain_policy_predicate_closure_conformance
  )
  set_tests_properties(sbsql_domain_policy_predicate_closure_conformance PROPERTIES
    LABELS "sbsql_domain_policy_predicate_closure_conformance;CBQ-006-DOMAIN-POLICY-PREDICATE-CLOSURE;domain_policy_predicate_closure;sbsql_parser_worker"
  )

  add_executable(
    sbsql_domain_language_expansion_conformance
    sbsql_domain_language_expansion_conformance.cpp
  )
  target_compile_features(sbsql_domain_language_expansion_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_domain_language_expansion_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_domain_language_expansion_conformance
    COMMAND sbsql_domain_language_expansion_conformance
  )
  set_tests_properties(sbsql_domain_language_expansion_conformance PROPERTIES
    LABELS "sbsql_domain_language_expansion_conformance;EDR-036;EDR-GATE-036;EDR-DOMAIN-LANGUAGE-EXPANSION;mga_transaction_regression;sbsql_parser_worker"
  )

  add_executable(
    sbsql_transaction_policy_closure_conformance
    sbsql_transaction_policy_closure_conformance.cpp
  )
  target_compile_features(sbsql_transaction_policy_closure_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_transaction_policy_closure_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_transaction_policy_closure_conformance
    COMMAND sbsql_transaction_policy_closure_conformance
  )
  set_tests_properties(sbsql_transaction_policy_closure_conformance PROPERTIES
    LABELS "sbsql_transaction_policy_closure_conformance;CBQ-007-TRANSACTION-POLICY-CLOSURE;transaction_policy_closure;sbsql_parser_worker"
  )

  add_executable(
    sbsql_advanced_datatype_operation_closure_conformance
    sbsql_advanced_datatype_operation_closure_conformance.cpp
  )
  target_compile_features(sbsql_advanced_datatype_operation_closure_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_advanced_datatype_operation_closure_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
      sb_engine_executor
  )
  add_test(
    NAME sbsql_advanced_datatype_operation_closure_conformance
    COMMAND sbsql_advanced_datatype_operation_closure_conformance
  )
  set_tests_properties(sbsql_advanced_datatype_operation_closure_conformance PROPERTIES
    LABELS "sbsql_advanced_datatype_operation_closure_conformance;CBQ-008-ADVANCED-DATATYPE-OPERATION-CLOSURE;advanced_datatype_operation_closure;sbsql_parser_worker"
  )

  add_executable(
    sbsql_numeric_backend_reconciliation_conformance
    sbsql_numeric_backend_reconciliation_conformance.cpp
  )
  target_compile_features(sbsql_numeric_backend_reconciliation_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_numeric_backend_reconciliation_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
      sbl_numeric
  )
  add_test(
    NAME sbsql_numeric_backend_reconciliation_conformance
    COMMAND sbsql_numeric_backend_reconciliation_conformance
  )
  set_tests_properties(sbsql_numeric_backend_reconciliation_conformance PROPERTIES
    LABELS "sbsql_numeric_backend_reconciliation_conformance;CBQ-008;CBQ-021-NUMERIC-BACKEND-STRATEGY;numeric_128_backend_gate;sbsql_parser_worker"
  )

  add_executable(
    sbsql_query_pivot_having_closure_conformance
    sbsql_query_pivot_having_closure_conformance.cpp
  )
  target_compile_features(sbsql_query_pivot_having_closure_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_query_pivot_having_closure_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
      sb_engine_executor
  )
  add_test(
    NAME sbsql_query_pivot_having_closure_conformance
    COMMAND sbsql_query_pivot_having_closure_conformance
  )
  set_tests_properties(sbsql_query_pivot_having_closure_conformance PROPERTIES
    LABELS "sbsql_query_pivot_having_closure_conformance;CBQ-009-QUERY-PIVOT-HAVING-CLOSURE;query_pivot_having_closure;sbsql_parser_worker"
  )

  add_executable(
    sbsql_index_family_runtime_closure_conformance
    sbsql_index_family_runtime_closure_conformance.cpp
  )
  target_compile_features(sbsql_index_family_runtime_closure_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_index_family_runtime_closure_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
      sb_core_index
  )
  add_test(
    NAME sbsql_index_family_runtime_closure_conformance
    COMMAND sbsql_index_family_runtime_closure_conformance
  )
  set_tests_properties(sbsql_index_family_runtime_closure_conformance PROPERTIES
    LABELS "sbsql_index_family_runtime_closure_conformance;CBQ-005;CBQ_GATE_INDEX_FAMILY_COMPLETE;index_family_runtime_closure;sbsql_parser_worker"
  )

  add_executable(
    sbsql_create_view_exact_route_conformance
    sbsql_create_view_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_create_view_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_create_view_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_create_view_exact_route_conformance
    COMMAND sbsql_create_view_exact_route_conformance
  )
  set_tests_properties(sbsql_create_view_exact_route_conformance PROPERTIES
    LABELS "sbsql_create_view_exact_route_conformance;SBSFC-020R-QN;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_database_lifecycle_exact_route_conformance
    sbsql_database_lifecycle_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_database_lifecycle_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_database_lifecycle_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_database_lifecycle_exact_route_conformance
    COMMAND sbsql_database_lifecycle_exact_route_conformance
  )
  set_tests_properties(sbsql_database_lifecycle_exact_route_conformance PROPERTIES
    LABELS "sbsql_database_lifecycle_exact_route_conformance;SBSFC-020R-QV;SBSFC-020R-QW;SBSFC-020R-QX;SBSFC-020R-QY;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed;database_lifecycle"
  )

  add_executable(
    sbsql_create_executable_exact_route_conformance
    sbsql_create_executable_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_create_executable_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_create_executable_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_create_executable_exact_route_conformance
    COMMAND sbsql_create_executable_exact_route_conformance
  )
  set_tests_properties(sbsql_create_executable_exact_route_conformance PROPERTIES
    LABELS "sbsql_create_executable_exact_route_conformance;SBSFC-020R-QU;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_routine_cursor_argument_conformance
    sbsql_routine_cursor_argument_conformance.cpp
  )
  target_compile_features(sbsql_routine_cursor_argument_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_routine_cursor_argument_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
  )
  add_test(
    NAME sbsql_routine_cursor_argument_conformance
    COMMAND sbsql_routine_cursor_argument_conformance
  )
  set_tests_properties(sbsql_routine_cursor_argument_conformance PROPERTIES
    LABELS "sbsql_routine_cursor_argument_conformance;ROUTINE-CURSOR-PROOF-CLOSURE;EDR-038;ROUTINE-CURSOR-GATE-001;ROUTINE-CURSOR-GATE-002;ROUTINE-CURSOR-GATE-003;ROUTINE-CURSOR-GATE-004;ROUTINE-CURSOR-GATE-005;ROUTINE-CURSOR-GATE-006;ROUTINE-CURSOR-GATE-007;ROUTINE-CURSOR-GATE-008;ROUTINE-CURSOR-GATE-009;sblr_operation_matrix_gate;mga_transaction_regression;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_075_catalog_descriptor_mutation_conformance
    sbsql_sbsfc_075_catalog_descriptor_mutation_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_075_catalog_descriptor_mutation_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_075_catalog_descriptor_mutation_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_075_catalog_descriptor_mutation_conformance
    COMMAND sbsql_sbsfc_075_catalog_descriptor_mutation_conformance
  )
  set_tests_properties(sbsql_sbsfc_075_catalog_descriptor_mutation_conformance PROPERTIES
    LABELS "sbsql_sbsfc_075_catalog_descriptor_mutation_conformance;SBSFC-075;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_076_dml_residual_exact_route_conformance
    sbsql_sbsfc_076_dml_residual_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_076_dml_residual_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_076_dml_residual_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_076_dml_residual_exact_route_conformance
    COMMAND sbsql_sbsfc_076_dml_residual_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_076_dml_residual_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_076_dml_residual_exact_route_conformance;SBSFC-076;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_077_non_general_residual_exact_route_conformance
    sbsql_sbsfc_077_non_general_residual_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_077_non_general_residual_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_077_non_general_residual_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
      sbu_sbsql_parser_support
  )
  add_test(
    NAME sbsql_sbsfc_077_non_general_residual_exact_route_conformance
    COMMAND sbsql_sbsfc_077_non_general_residual_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_077_non_general_residual_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_077_non_general_residual_exact_route_conformance;SBSFC-077;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  if(TARGET sb_engine_internal_api)
    add_executable(
      sbsql_non_core_optional_provider_classification_conformance
      sbsql_non_core_optional_provider_classification_conformance.cpp
    )
    target_compile_features(sbsql_non_core_optional_provider_classification_conformance PRIVATE cxx_std_23)
    target_link_libraries(
      sbsql_non_core_optional_provider_classification_conformance
      PRIVATE
        sb_engine_internal_api
    )
    add_test(
      NAME sbsql_non_core_optional_provider_classification_conformance
      COMMAND sbsql_non_core_optional_provider_classification_conformance
    )
    set_tests_properties(sbsql_non_core_optional_provider_classification_conformance PROPERTIES
      LABELS "sbsql_non_core_optional_provider_classification_conformance;CBQ-004A;CBQ_GATE_NON_CORE_SURFACES_CLASSIFIED;sbsql_parser_worker"
    )
  endif()

  add_executable(
    sbsql_sbsfc_078_procedural_general_residual_exact_route_conformance
    sbsql_sbsfc_078_procedural_general_residual_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_078_procedural_general_residual_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_078_procedural_general_residual_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_078_procedural_general_residual_exact_route_conformance
    COMMAND sbsql_sbsfc_078_procedural_general_residual_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_078_procedural_general_residual_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_078_procedural_general_residual_exact_route_conformance;SBSFC-078;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_079_multimodel_general_residual_exact_route_conformance
    sbsql_sbsfc_079_multimodel_general_residual_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_079_multimodel_general_residual_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_079_multimodel_general_residual_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_079_multimodel_general_residual_exact_route_conformance
    COMMAND sbsql_sbsfc_079_multimodel_general_residual_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_079_multimodel_general_residual_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_079_multimodel_general_residual_exact_route_conformance;SBSFC-079;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_080_operational_general_residual_exact_route_conformance
    sbsql_sbsfc_080_operational_general_residual_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_080_operational_general_residual_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_080_operational_general_residual_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_080_operational_general_residual_exact_route_conformance
    COMMAND sbsql_sbsfc_080_operational_general_residual_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_080_operational_general_residual_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_080_operational_general_residual_exact_route_conformance;SBSFC-080;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_memory_management_exact_route_conformance
    sbsql_memory_management_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_memory_management_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_memory_management_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_memory_management_exact_route_conformance
    COMMAND sbsql_memory_management_exact_route_conformance
  )
  set_tests_properties(sbsql_memory_management_exact_route_conformance PROPERTIES
    LABELS "sbsql_memory_management_exact_route_conformance;MEMORY-MANAGEMENT-SBSQL-ROUTE-IMPLEMENTED;sbsql_public_exact_commands;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_parser_worker;sbsql_e2e_passed;mga_transaction_regression"
  )

  add_executable(
    sbsql_storage_tier_exact_route_conformance
    sbsql_storage_tier_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_storage_tier_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_storage_tier_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_storage_tier_exact_route_conformance
    COMMAND sbsql_storage_tier_exact_route_conformance
  )
  set_tests_properties(sbsql_storage_tier_exact_route_conformance PROPERTIES
    LABELS "sbsql_storage_tier_exact_route_conformance;STORAGE-TIER-SBSQL-ROUTE-IMPLEMENTED;sbsql_public_exact_commands;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_parser_worker;sbsql_e2e_passed;mga_transaction_regression"
  )

  add_executable(
    sbsql_filespace_discovery_exact_route_conformance
    sbsql_filespace_discovery_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_filespace_discovery_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_filespace_discovery_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_filespace_discovery_exact_route_conformance
    COMMAND sbsql_filespace_discovery_exact_route_conformance
  )
  set_tests_properties(sbsql_filespace_discovery_exact_route_conformance PROPERTIES
    LABELS "sbsql_filespace_discovery_exact_route_conformance;FILESPACE-DISCOVERY-SBSQL-ROUTE-IMPLEMENTED;sbsql_public_exact_commands;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_parser_worker;sbsql_e2e_passed;mga_transaction_regression"
  )

  add_executable(
    sbsql_filespace_package_exact_route_conformance
    sbsql_filespace_package_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_filespace_package_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_filespace_package_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_filespace_package_exact_route_conformance
    COMMAND sbsql_filespace_package_exact_route_conformance
  )
  set_tests_properties(sbsql_filespace_package_exact_route_conformance PROPERTIES
    LABELS "sbsql_filespace_package_exact_route_conformance;FILESPACE-PACKAGE-SBSQL-ROUTE-IMPLEMENTED;sbsql_public_exact_commands;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_parser_worker;sbsql_e2e_passed;mga_transaction_regression"
  )

  add_executable(
    sbsql_shard_placement_exact_route_conformance
    sbsql_shard_placement_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_shard_placement_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_shard_placement_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_shard_placement_exact_route_conformance
    COMMAND sbsql_shard_placement_exact_route_conformance
  )
  set_tests_properties(sbsql_shard_placement_exact_route_conformance PROPERTIES
    LABELS "sbsql_shard_placement_exact_route_conformance;SHARD-PLACEMENT-SBSQL-ROUTE-IMPLEMENTED;sbsql_public_exact_commands;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_parser_worker;sbsql_e2e_passed;mga_transaction_regression"
  )

  add_executable(
    sbsql_encryption_maintenance_exact_route_conformance
    sbsql_encryption_maintenance_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_encryption_maintenance_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_encryption_maintenance_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
      sb_engine_internal_api
  )
  add_test(
    NAME sbsql_encryption_maintenance_exact_route_conformance
    COMMAND sbsql_encryption_maintenance_exact_route_conformance
  )
  set_tests_properties(sbsql_encryption_maintenance_exact_route_conformance PROPERTIES
    LABELS "sbsql_encryption_maintenance_exact_route_conformance;ENCRYPTION-MAINTENANCE-SBSQL-ROUTE-IMPLEMENTED;sbsql_public_exact_commands;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_parser_worker;sbsql_e2e_passed;mga_transaction_regression"
  )

  add_executable(
    sbsql_protected_material_exact_route_conformance
    sbsql_protected_material_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_protected_material_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_protected_material_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
      sb_engine_internal_api
  )
  add_test(
    NAME sbsql_protected_material_exact_route_conformance
    COMMAND sbsql_protected_material_exact_route_conformance
  )
  set_tests_properties(sbsql_protected_material_exact_route_conformance PROPERTIES
    LABELS "sbsql_protected_material_exact_route_conformance;PROTECTED-MATERIAL-SBSQL-ROUTE-IMPLEMENTED;sbsql_public_exact_commands;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_parser_worker;sbsql_e2e_passed;mga_transaction_regression"
  )

  add_executable(
    sbsql_sbsfc_081_descriptor_expression_residual_exact_route_conformance
    sbsql_sbsfc_081_descriptor_expression_residual_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_081_descriptor_expression_residual_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_081_descriptor_expression_residual_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_081_descriptor_expression_residual_exact_route_conformance
    COMMAND sbsql_sbsfc_081_descriptor_expression_residual_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_081_descriptor_expression_residual_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_081_descriptor_expression_residual_exact_route_conformance;SBSFC-081;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_082_surface_descriptor_exact_route_conformance
    sbsql_sbsfc_082_surface_descriptor_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_082_surface_descriptor_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_082_surface_descriptor_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_082_surface_descriptor_exact_route_conformance
    COMMAND sbsql_sbsfc_082_surface_descriptor_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_082_surface_descriptor_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_082_surface_descriptor_exact_route_conformance;SBSFC-082;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_083_grammar_surface_exact_route_conformance
    sbsql_sbsfc_083_grammar_surface_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_083_grammar_surface_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_083_grammar_surface_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_083_grammar_surface_exact_route_conformance
    COMMAND sbsql_sbsfc_083_grammar_surface_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_083_grammar_surface_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_083_grammar_surface_exact_route_conformance;SBSFC-083;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_084_grammar_surface_exact_route_conformance
    sbsql_sbsfc_084_grammar_surface_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_084_grammar_surface_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_084_grammar_surface_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_084_grammar_surface_exact_route_conformance
    COMMAND sbsql_sbsfc_084_grammar_surface_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_084_grammar_surface_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_084_grammar_surface_exact_route_conformance;SBSFC-084;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )

  add_executable(
    sbsql_sbsfc_085_grammar_surface_exact_route_conformance
    sbsql_sbsfc_085_grammar_surface_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sbsfc_085_grammar_surface_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sbsfc_085_grammar_surface_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sbsfc_085_grammar_surface_exact_route_conformance
    COMMAND sbsql_sbsfc_085_grammar_surface_exact_route_conformance
  )
  set_tests_properties(sbsql_sbsfc_085_grammar_surface_exact_route_conformance PROPERTIES
    LABELS "sbsql_sbsfc_085_grammar_surface_exact_route_conformance;SBSFC-085;SBSFC-030;sbsql_surface_to_sblr_full_implementation_closure;parser_v3_closure_gate;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr AND TARGET sb_engine_functions)
  add_executable(
    sbsql_column_constraint_exact_route_conformance
    sbsql_column_constraint_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_column_constraint_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_column_constraint_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
      sb_engine_functions
  )
  add_test(
    NAME sbsql_column_constraint_exact_route_conformance
    COMMAND sbsql_column_constraint_exact_route_conformance
  )
  set_tests_properties(sbsql_column_constraint_exact_route_conformance PROPERTIES
    LABELS "sbsql_column_constraint_exact_route_conformance;SBSFC-021R-X;SBSFC-021R-Y;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_observability_exact_route_conformance
    sbsql_observability_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_observability_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_observability_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_observability_exact_route_conformance
    COMMAND sbsql_observability_exact_route_conformance
  )
  set_tests_properties(sbsql_observability_exact_route_conformance PROPERTIES
    LABELS "sbsql_observability_exact_route_conformance;SBSFC-024;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_show_create_exact_route_conformance
    sbsql_show_create_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_show_create_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_show_create_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  target_compile_definitions(
    sbsql_show_create_exact_route_conformance
    PRIVATE
      SB_SBSFC021_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
  )
  add_test(
    NAME sbsql_show_create_exact_route_conformance
    COMMAND sbsql_show_create_exact_route_conformance
  )
  set_tests_properties(sbsql_show_create_exact_route_conformance PROPERTIES
    LABELS "sbsql_show_create_exact_route_conformance;SBSFC-024R-M;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_agent_management_exact_route_conformance
    sbsql_agent_management_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_agent_management_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_agent_management_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_agent_management_exact_route_conformance
    COMMAND sbsql_agent_management_exact_route_conformance
  )
  set_tests_properties(sbsql_agent_management_exact_route_conformance PROPERTIES
    LABELS "sbsql_agent_management_exact_route_conformance;SBSFC-024R-J;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_agent_command_surface_matrix_gate
    sbsql_agent_command_surface_matrix_gate.cpp
  )
  target_compile_features(sbsql_agent_command_surface_matrix_gate PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_agent_command_surface_matrix_gate
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  if(TARGET sb_cluster_provider)
    target_link_libraries(sbsql_agent_command_surface_matrix_gate PRIVATE sb_cluster_provider)
  endif()
  add_test(
    NAME sbsql_agent_command_surface_matrix_gate
    COMMAND sbsql_agent_command_surface_matrix_gate
  )
  set_tests_properties(sbsql_agent_command_surface_matrix_gate PROPERTIES
    LABELS "sbsql_agent_command_surface_matrix_gate;PFAR-013B;sblr_agent_management_route_gate;sblr_operation_matrix_gate;sbsql_parser_worker;mga_transaction_regression"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    agent_examples_evidence_payload_gate
    agent_examples_evidence_payload_gate.cpp
  )
  target_compile_features(agent_examples_evidence_payload_gate PRIVATE cxx_std_23)
  target_link_libraries(
    agent_examples_evidence_payload_gate
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
      sb_engine_internal_api
      sb_core_uuid
  )
  if(TARGET sb_cluster_provider)
    target_link_libraries(agent_examples_evidence_payload_gate PRIVATE sb_cluster_provider)
  endif()
  add_test(
    NAME agent_examples_evidence_payload_gate
    COMMAND agent_examples_evidence_payload_gate
            "${PROJECT_SOURCE_DIR}/../public_contract_snapshot"
            "${PROJECT_SOURCE_DIR}/../docs/reference/management-ui/agent-examples-evidence-payload-pack.md"
  )
  set_tests_properties(agent_examples_evidence_payload_gate PROPERTIES
    LABELS "agent_examples_evidence_payload_gate;PFAR-020A;agents;agent_runtime;examples;evidence_payload;documentation;sb_isql;sbsql_parser_worker;mga_transaction_regression"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_parser_listener_management_exact_route_conformance
    sbsql_parser_listener_management_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_parser_listener_management_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_parser_listener_management_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_parser_listener_management_exact_route_conformance
    COMMAND sbsql_parser_listener_management_exact_route_conformance
  )
  set_tests_properties(sbsql_parser_listener_management_exact_route_conformance PROPERTIES
    LABELS "sbsql_parser_listener_management_exact_route_conformance;SBSFC-024R-K;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_sblr_final_cleanup_b001_exact_route_conformance
    sbsql_sblr_final_cleanup_b001_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sblr_final_cleanup_b001_exact_route_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    sbsql_sblr_final_cleanup_b001_exact_route_conformance
    PRIVATE
      SCRATCHBIRD_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  target_link_libraries(
    sbsql_sblr_final_cleanup_b001_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sblr_final_cleanup_b001_exact_route_conformance
    COMMAND sbsql_sblr_final_cleanup_b001_exact_route_conformance
  )
  set_tests_properties(sbsql_sblr_final_cleanup_b001_exact_route_conformance PROPERTIES
    LABELS "sbsql_sblr_final_cleanup_b001_exact_route_conformance;sbsql_public_exact_commands;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_result_shape_contracts;sbsql_security_privilege_contracts;sbsql_message_vector_rendering;sbsql_sblr_final_cleanup_full_route;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_sblr_final_cleanup_b002_exact_route_conformance
    sbsql_sblr_final_cleanup_b002_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_sblr_final_cleanup_b002_exact_route_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    sbsql_sblr_final_cleanup_b002_exact_route_conformance
    PRIVATE
      SCRATCHBIRD_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  target_link_libraries(
    sbsql_sblr_final_cleanup_b002_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sblr_final_cleanup_b002_exact_route_conformance
    COMMAND sbsql_sblr_final_cleanup_b002_exact_route_conformance
  )
  set_tests_properties(sbsql_sblr_final_cleanup_b002_exact_route_conformance PROPERTIES
    LABELS "sbsql_sblr_final_cleanup_b002_exact_route_conformance;sbsql_public_exact_commands;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_result_shape_contracts;sbsql_security_privilege_contracts;sbsql_message_vector_rendering;sbsql_sblr_final_cleanup_full_route;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

add_executable(
  sbsql_sblr_final_cleanup_b003_engine_api_route_conformance
  sbsql_sblr_final_cleanup_b003_engine_api_route_conformance.cpp
)
target_compile_features(sbsql_sblr_final_cleanup_b003_engine_api_route_conformance PRIVATE cxx_std_23)
target_compile_definitions(
  sbsql_sblr_final_cleanup_b003_engine_api_route_conformance
  PRIVATE
    SCRATCHBIRD_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
target_link_libraries(
  sbsql_sblr_final_cleanup_b003_engine_api_route_conformance
  PRIVATE
    sbl_sbsql_parser_pipeline
    sb_server_core
    sb_engine_sblr
)
add_test(
  NAME sbsql_sblr_final_cleanup_b003_engine_api_route_conformance
  COMMAND sbsql_sblr_final_cleanup_b003_engine_api_route_conformance
)
set_tests_properties(sbsql_sblr_final_cleanup_b003_engine_api_route_conformance PROPERTIES
  LABELS "sbsql_sblr_final_cleanup_b003_engine_api_route_conformance;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_result_shape_contracts;sbsql_security_privilege_contracts;sbsql_message_vector_rendering;sbsql_sblr_final_cleanup_full_route;sbsql_parser_worker;sbsql_e2e_passed"
)

if(TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_sblr_final_cleanup_b007_family_admission_conformance
    sbsql_sblr_final_cleanup_b007_family_admission_conformance.cpp
  )
  target_compile_features(sbsql_sblr_final_cleanup_b007_family_admission_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    sbsql_sblr_final_cleanup_b007_family_admission_conformance
    PRIVATE
      SCRATCHBIRD_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  target_link_libraries(
    sbsql_sblr_final_cleanup_b007_family_admission_conformance
    PRIVATE
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sblr_final_cleanup_b007_family_admission_conformance
    COMMAND sbsql_sblr_final_cleanup_b007_family_admission_conformance
  )
  set_tests_properties(sbsql_sblr_final_cleanup_b007_family_admission_conformance PROPERTIES
    LABELS "sbsql_sblr_final_cleanup_b007_family_admission_conformance;sblr_family_admission_conformance;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_result_shape_contracts;sbsql_security_privilege_contracts;sbsql_message_vector_rendering;sbsql_sblr_final_cleanup_full_route;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sb_engine_sblr)
  add_executable(
    sbsql_sblr_final_cleanup_b008_opcode_registry_conformance
    sbsql_sblr_final_cleanup_b008_opcode_registry_conformance.cpp
  )
  target_compile_features(sbsql_sblr_final_cleanup_b008_opcode_registry_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    sbsql_sblr_final_cleanup_b008_opcode_registry_conformance
    PRIVATE
      SCRATCHBIRD_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  target_link_libraries(
    sbsql_sblr_final_cleanup_b008_opcode_registry_conformance
    PRIVATE
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sblr_final_cleanup_b008_opcode_registry_conformance
    COMMAND sbsql_sblr_final_cleanup_b008_opcode_registry_conformance
  )
  set_tests_properties(sbsql_sblr_final_cleanup_b008_opcode_registry_conformance PROPERTIES
    LABELS "sbsql_sblr_final_cleanup_b008_opcode_registry_conformance;sblr_opcode_registry_conformance;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_result_shape_contracts;sbsql_security_privilege_contracts;sbsql_message_vector_rendering;sbsql_sblr_final_cleanup_full_route;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sb_engine_sblr)
  add_executable(
    sbsql_sblr_final_cleanup_b009_opcode_registry_conformance
    sbsql_sblr_final_cleanup_b009_opcode_registry_conformance.cpp
  )
  target_compile_features(sbsql_sblr_final_cleanup_b009_opcode_registry_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    sbsql_sblr_final_cleanup_b009_opcode_registry_conformance
    PRIVATE
      SCRATCHBIRD_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  target_link_libraries(
    sbsql_sblr_final_cleanup_b009_opcode_registry_conformance
    PRIVATE
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sblr_final_cleanup_b009_opcode_registry_conformance
    COMMAND sbsql_sblr_final_cleanup_b009_opcode_registry_conformance
  )
  set_tests_properties(sbsql_sblr_final_cleanup_b009_opcode_registry_conformance PROPERTIES
    LABELS "sbsql_sblr_final_cleanup_b009_opcode_registry_conformance;sblr_opcode_registry_conformance;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_result_shape_contracts;sbsql_security_privilege_contracts;sbsql_message_vector_rendering;sbsql_sblr_final_cleanup_full_route;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sb_engine_sblr)
  add_executable(
    sbsql_sblr_final_cleanup_b010_opcode_registry_conformance
    sbsql_sblr_final_cleanup_b010_opcode_registry_conformance.cpp
  )
  target_compile_features(sbsql_sblr_final_cleanup_b010_opcode_registry_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    sbsql_sblr_final_cleanup_b010_opcode_registry_conformance
    PRIVATE
      SCRATCHBIRD_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  target_link_libraries(
    sbsql_sblr_final_cleanup_b010_opcode_registry_conformance
    PRIVATE
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sblr_final_cleanup_b010_opcode_registry_conformance
    COMMAND sbsql_sblr_final_cleanup_b010_opcode_registry_conformance
  )
  set_tests_properties(sbsql_sblr_final_cleanup_b010_opcode_registry_conformance PROPERTIES
    LABELS "sbsql_sblr_final_cleanup_b010_opcode_registry_conformance;sblr_opcode_registry_conformance;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_result_shape_contracts;sbsql_security_privilege_contracts;sbsql_message_vector_rendering;sbsql_sblr_final_cleanup_full_route;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sb_engine_sblr)
  add_executable(
    sbsql_sblr_final_cleanup_b011_opcode_registry_conformance
    sbsql_sblr_final_cleanup_b011_opcode_registry_conformance.cpp
  )
  target_compile_features(sbsql_sblr_final_cleanup_b011_opcode_registry_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    sbsql_sblr_final_cleanup_b011_opcode_registry_conformance
    PRIVATE
      SCRATCHBIRD_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  target_link_libraries(
    sbsql_sblr_final_cleanup_b011_opcode_registry_conformance
    PRIVATE
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sblr_final_cleanup_b011_opcode_registry_conformance
    COMMAND sbsql_sblr_final_cleanup_b011_opcode_registry_conformance
  )
  set_tests_properties(sbsql_sblr_final_cleanup_b011_opcode_registry_conformance PROPERTIES
    LABELS "sbsql_sblr_final_cleanup_b011_opcode_registry_conformance;sblr_opcode_registry_conformance;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_result_shape_contracts;sbsql_security_privilege_contracts;sbsql_message_vector_rendering;sbsql_sblr_final_cleanup_full_route;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sb_engine_sblr)
  add_executable(
    sbsql_sblr_final_cleanup_b012_opcode_registry_conformance
    sbsql_sblr_final_cleanup_b012_opcode_registry_conformance.cpp
  )
  target_compile_features(sbsql_sblr_final_cleanup_b012_opcode_registry_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    sbsql_sblr_final_cleanup_b012_opcode_registry_conformance
    PRIVATE
      SCRATCHBIRD_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  target_link_libraries(
    sbsql_sblr_final_cleanup_b012_opcode_registry_conformance
    PRIVATE
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sblr_final_cleanup_b012_opcode_registry_conformance
    COMMAND sbsql_sblr_final_cleanup_b012_opcode_registry_conformance
  )
  set_tests_properties(sbsql_sblr_final_cleanup_b012_opcode_registry_conformance PROPERTIES
    LABELS "sbsql_sblr_final_cleanup_b012_opcode_registry_conformance;sblr_opcode_registry_conformance;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_result_shape_contracts;sbsql_security_privilege_contracts;sbsql_message_vector_rendering;sbsql_sblr_final_cleanup_full_route;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sb_engine_sblr)
  add_executable(
    sbsql_sblr_final_cleanup_b013_opcode_registry_conformance
    sbsql_sblr_final_cleanup_b013_opcode_registry_conformance.cpp
  )
  target_compile_features(sbsql_sblr_final_cleanup_b013_opcode_registry_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    sbsql_sblr_final_cleanup_b013_opcode_registry_conformance
    PRIVATE
      SCRATCHBIRD_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  target_link_libraries(
    sbsql_sblr_final_cleanup_b013_opcode_registry_conformance
    PRIVATE
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sblr_final_cleanup_b013_opcode_registry_conformance
    COMMAND sbsql_sblr_final_cleanup_b013_opcode_registry_conformance
  )
  set_tests_properties(sbsql_sblr_final_cleanup_b013_opcode_registry_conformance PROPERTIES
    LABELS "sbsql_sblr_final_cleanup_b013_opcode_registry_conformance;sblr_opcode_registry_conformance;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_result_shape_contracts;sbsql_security_privilege_contracts;sbsql_message_vector_rendering;sbsql_sblr_final_cleanup_full_route;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sb_engine_sblr)
  add_executable(
    sbsql_sblr_final_cleanup_b014_opcode_registry_conformance
    sbsql_sblr_final_cleanup_b014_opcode_registry_conformance.cpp
  )
  target_compile_features(sbsql_sblr_final_cleanup_b014_opcode_registry_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    sbsql_sblr_final_cleanup_b014_opcode_registry_conformance
    PRIVATE
      SCRATCHBIRD_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  target_link_libraries(
    sbsql_sblr_final_cleanup_b014_opcode_registry_conformance
    PRIVATE
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sblr_final_cleanup_b014_opcode_registry_conformance
    COMMAND sbsql_sblr_final_cleanup_b014_opcode_registry_conformance
  )
  set_tests_properties(sbsql_sblr_final_cleanup_b014_opcode_registry_conformance PROPERTIES
    LABELS "sbsql_sblr_final_cleanup_b014_opcode_registry_conformance;sblr_opcode_registry_conformance;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_result_shape_contracts;sbsql_security_privilege_contracts;sbsql_message_vector_rendering;sbsql_sblr_final_cleanup_full_route;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_sblr_final_cleanup_b015_cluster_provider_route_conformance
    sbsql_sblr_final_cleanup_b015_cluster_provider_route_conformance.cpp
  )
  target_compile_features(
    sbsql_sblr_final_cleanup_b015_cluster_provider_route_conformance
    PRIVATE
      cxx_std_23
  )
  target_compile_definitions(
    sbsql_sblr_final_cleanup_b015_cluster_provider_route_conformance
    PRIVATE
      SCRATCHBIRD_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  target_link_libraries(
    sbsql_sblr_final_cleanup_b015_cluster_provider_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sblr_final_cleanup_b015_cluster_provider_route_conformance
    COMMAND sbsql_sblr_final_cleanup_b015_cluster_provider_route_conformance
  )
  set_tests_properties(sbsql_sblr_final_cleanup_b015_cluster_provider_route_conformance PROPERTIES
    LABELS "sbsql_sblr_final_cleanup_b015_cluster_provider_route_conformance;sbsql_cluster_provider_split;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_result_shape_contracts;sbsql_security_privilege_contracts;sbsql_message_vector_rendering;sbsql_sblr_final_cleanup_full_route;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_sblr_final_cleanup_b016_cluster_provider_evidence_conformance
    sbsql_sblr_final_cleanup_b015_cluster_provider_route_conformance.cpp
  )
  target_compile_features(
    sbsql_sblr_final_cleanup_b016_cluster_provider_evidence_conformance
    PRIVATE
      cxx_std_23
  )
  target_compile_definitions(
    sbsql_sblr_final_cleanup_b016_cluster_provider_evidence_conformance
    PRIVATE
      SCRATCHBIRD_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  target_link_libraries(
    sbsql_sblr_final_cleanup_b016_cluster_provider_evidence_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_sblr_final_cleanup_b016_cluster_provider_evidence_conformance
    COMMAND sbsql_sblr_final_cleanup_b016_cluster_provider_evidence_conformance
  )
  set_tests_properties(sbsql_sblr_final_cleanup_b016_cluster_provider_evidence_conformance PROPERTIES
    LABELS "sbsql_sblr_final_cleanup_b016_cluster_provider_evidence_conformance;sbsql_cluster_provider_split;sbsql_sblr_api_routes;sbsql_engine_behavior_complete;sbsql_result_shape_contracts;sbsql_security_privilege_contracts;sbsql_message_vector_rendering;sbsql_sblr_final_cleanup_full_route;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_event_notification_exact_route_conformance
    sbsql_event_notification_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_event_notification_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_event_notification_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_event_notification_exact_route_conformance
    COMMAND sbsql_event_notification_exact_route_conformance
  )
  set_tests_properties(sbsql_event_notification_exact_route_conformance PROPERTIES
    LABELS "sbsql_event_notification_exact_route_conformance;SBSFC-024R-O;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr AND TARGET sb_udr_runtime)
  add_executable(
    sbsql_udr_package_management_exact_route_conformance
    sbsql_udr_package_management_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_udr_package_management_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_udr_package_management_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
      sb_udr_runtime
  )
  add_test(
    NAME sbsql_udr_package_management_exact_route_conformance
    COMMAND sbsql_udr_package_management_exact_route_conformance
  )
  set_tests_properties(sbsql_udr_package_management_exact_route_conformance PROPERTIES
    LABELS "sbsql_udr_package_management_exact_route_conformance;SBSFC-024R-L;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_security_exact_route_conformance
    sbsql_security_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_security_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_security_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_security_exact_route_conformance
    COMMAND sbsql_security_exact_route_conformance
  )
  set_tests_properties(sbsql_security_exact_route_conformance PROPERTIES
    LABELS "sbsql_security_exact_route_conformance;SBSFC-023;SBSFC-023R-C;SBSFC-023R-D;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_dml_exact_route_conformance
    sbsql_dml_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_dml_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_dml_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_dml_exact_route_conformance
    COMMAND sbsql_dml_exact_route_conformance
  )
  set_tests_properties(sbsql_dml_exact_route_conformance PROPERTIES
    LABELS "sbsql_dml_exact_route_conformance;SBSFC-021;SBSFC-021R-Z;SBSFC-021R-AA;SBSFC-021R-AB;SBSFC-021R-AC;SBSFC-021R-AD;SBSFC-021R-AE;SBSFC-021R-AF;SBSFC-021R-AG;SBSFC-021R-AH;SBSFC-021R-AI;SBSFC-021R-AJ;SBSFC-021R-AK;SBSFC-021R-AL;SBSFC-021R-AM;SBSFC-021R-AN;SBSFC-021R-AO;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_query_scalar_projection_conformance
    sbsql_query_scalar_projection_conformance.cpp
  )
  target_compile_features(sbsql_query_scalar_projection_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_query_scalar_projection_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_query_scalar_projection_conformance
    COMMAND sbsql_query_scalar_projection_conformance
  )
  set_tests_properties(sbsql_query_scalar_projection_conformance PROPERTIES
    LABELS "sbsql_query_scalar_projection_conformance;SBSFC-010;SBSFC-011;SBSFC-012;SBSFC-013;SBSFC-014;SBSFC-014R-Y;SBSFC-016R-G;SBSFC-016R-H;SBSFC-016R-I;SBSFC-016R-J;SBSFC-016R-K;SBSFC-016R-L;SBSFC-021;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_missing_functionality_system_variable_contract_conformance
    sbsql_missing_functionality_system_variable_contract_conformance.cpp
  )
  target_compile_features(
    sbsql_missing_functionality_system_variable_contract_conformance
    PRIVATE
      cxx_std_23
  )
  target_link_libraries(
    sbsql_missing_functionality_system_variable_contract_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_missing_functionality_system_variable_contract_conformance
    COMMAND sbsql_missing_functionality_system_variable_contract_conformance
  )
  set_tests_properties(
    sbsql_missing_functionality_system_variable_contract_conformance
    PROPERTIES
      LABELS "sbsql_missing_functionality_implementation_closure;SBSQL-MISS-GATE-010;sbsql_parser_worker;mga_transaction_regression"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_missing_functionality_migration_management_conformance
    sbsql_missing_functionality_migration_management_conformance.cpp
  )
  target_compile_features(
    sbsql_missing_functionality_migration_management_conformance
    PRIVATE
      cxx_std_23
  )
  target_link_libraries(
    sbsql_missing_functionality_migration_management_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_missing_functionality_migration_management_conformance
    COMMAND sbsql_missing_functionality_migration_management_conformance
  )
  set_tests_properties(
    sbsql_missing_functionality_migration_management_conformance
    PROPERTIES
      LABELS "sbsql_missing_functionality_implementation_closure;SBSQL-MISS-GATE-004;sbsql_parser_worker;mga_transaction_regression"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr AND TARGET sb_engine_functions)
  add_executable(
    sbsql_missing_functionality_lock_mga_policy_conformance
    sbsql_missing_functionality_lock_mga_policy_conformance.cpp
  )
  target_compile_features(
    sbsql_missing_functionality_lock_mga_policy_conformance
    PRIVATE
      cxx_std_23
  )
  target_link_libraries(
    sbsql_missing_functionality_lock_mga_policy_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
      sb_engine_functions
  )
  add_test(
    NAME sbsql_missing_functionality_lock_mga_policy_conformance
    COMMAND sbsql_missing_functionality_lock_mga_policy_conformance
  )
  set_tests_properties(
    sbsql_missing_functionality_lock_mga_policy_conformance
    PROPERTIES
      LABELS "sbsql_missing_functionality_implementation_closure;SBSQL-MISS-GATE-011;sbsql_parser_worker;mga_transaction_regression"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr AND TARGET sb_engine_functions)
  add_executable(
    sbsql_missing_functionality_builtin_acceleration_contract_conformance
    sbsql_missing_functionality_builtin_acceleration_contract_conformance.cpp
  )
  target_compile_features(
    sbsql_missing_functionality_builtin_acceleration_contract_conformance
    PRIVATE
      cxx_std_23
  )
  target_link_libraries(
    sbsql_missing_functionality_builtin_acceleration_contract_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
      sb_engine_functions
  )
  add_test(
    NAME sbsql_missing_functionality_builtin_acceleration_contract_conformance
    COMMAND sbsql_missing_functionality_builtin_acceleration_contract_conformance
  )
  set_tests_properties(
    sbsql_missing_functionality_builtin_acceleration_contract_conformance
    PROPERTIES
      LABELS "sbsql_missing_functionality_implementation_closure;SBSQL-MISS-GATE-012;sbsql_parser_worker;mga_transaction_regression"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_constructor_exact_route_conformance
    sbsql_constructor_exact_route_conformance.cpp
  )
  target_compile_features(sbsql_constructor_exact_route_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_constructor_exact_route_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_constructor_exact_route_conformance
    COMMAND sbsql_constructor_exact_route_conformance
  )
  set_tests_properties(sbsql_constructor_exact_route_conformance PROPERTIES
    LABELS "sbsql_constructor_exact_route_conformance;SBSFC-014R-Z;SBSFC-014R-AA;SBSFC-014R-AB;SBSFC-014R-AC;SBSFC-014R-AD;SBSFC-014R-AE;SBSFC-014R-AF;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker;sbsql_e2e_passed"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_values_rowset_conformance
    sbsql_values_rowset_conformance.cpp
  )
  target_compile_features(sbsql_values_rowset_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_values_rowset_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
      sb_engine_sblr
  )
  add_test(
    NAME sbsql_values_rowset_conformance
    COMMAND sbsql_values_rowset_conformance
  )
  set_tests_properties(sbsql_values_rowset_conformance PROPERTIES
    LABELS "sbsql_values_rowset_conformance;SBSFC-021;SBSFC-030;sblr_operation_matrix_gate;sbsql_parser_worker"
  )
endif()

if(TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_dml_mga_row_result_conformance
    sbsql_dml_mga_row_result_conformance.cpp
  )
  target_compile_features(sbsql_dml_mga_row_result_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_dml_mga_row_result_conformance
    PRIVATE
      sb_server_core
      sb_engine_sblr
  )
  target_compile_definitions(
    sbsql_dml_mga_row_result_conformance
    PRIVATE
      SB_SBSFC021_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
  )
  add_test(
    NAME sbsql_dml_mga_row_result_conformance
    COMMAND sbsql_dml_mga_row_result_conformance
  )
  set_tests_properties(sbsql_dml_mga_row_result_conformance PROPERTIES
    LABELS "sbsql_dml_mga_row_result_conformance;SBSFC-021;SBSFC-021R-Z;sbsql_query_dml_full_route_conformance;sblr_operation_matrix_gate;sbsql_parser_worker"
  )
endif()

if(TARGET sb_server_core AND TARGET sb_engine_sblr)
  add_executable(
    sbsql_merge_multi_action_odfr_020_conformance
    sbsql_merge_multi_action_odfr_020_conformance.cpp
  )
  target_compile_features(sbsql_merge_multi_action_odfr_020_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_merge_multi_action_odfr_020_conformance
    PRIVATE
      sb_server_core
      sb_engine_sblr
  )
  target_compile_definitions(
    sbsql_merge_multi_action_odfr_020_conformance
    PRIVATE
      SB_ODFR020_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
  )
  add_test(
    NAME sbsql_merge_multi_action_odfr_020_conformance
    COMMAND sbsql_merge_multi_action_odfr_020_conformance
  )
  set_tests_properties(sbsql_merge_multi_action_odfr_020_conformance PROPERTIES
    LABELS "sbsql_merge_multi_action_odfr_020_conformance;ODFR-020;sbsql_parser_worker;merge_multi_action;dml_physical_path;benchmark_clean;mga_transaction_regression;no_execution_plan_runtime"
  )

  add_executable(
    optimizer_runtime_hot_path_orh_040_041_050_gate
    sbsql_merge_multi_action_odfr_020_conformance.cpp
  )
  target_compile_features(optimizer_runtime_hot_path_orh_040_041_050_gate PRIVATE cxx_std_23)
  target_link_libraries(
    optimizer_runtime_hot_path_orh_040_041_050_gate
    PRIVATE
      sb_server_core
      sb_engine_sblr
  )
  target_compile_definitions(
    optimizer_runtime_hot_path_orh_040_041_050_gate
    PRIVATE
      SB_ODFR020_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
  )
  add_test(
    NAME optimizer_runtime_hot_path_orh_040_041_050_gate
    COMMAND optimizer_runtime_hot_path_orh_040_041_050_gate
  )
  set_tests_properties(optimizer_runtime_hot_path_orh_040_041_050_gate PROPERTIES
    LABELS "optimizer_runtime_hot_path_orh_040_041_050_gate;ORH-040;ORH-041;ORH-050;sbsql_parser_worker;merge_multi_action;dml_hot_point_cache;benchmark_clean;mga_transaction_regression;no_execution_plan_runtime"
  )
endif()

add_executable(sbp_sbsql_no_spin_gate parser_no_spin_gate.cpp)
target_compile_features(sbp_sbsql_no_spin_gate PRIVATE cxx_std_23)
add_test(NAME sbp_sbsql_no_spin_gate COMMAND sbp_sbsql_no_spin_gate ${PROJECT_SOURCE_DIR}/src/parsers/sbsql_worker)

add_executable(
  sbsql_no_spin_no_wal_no_direct_db_gate
  generated/hardening/sbsql_no_spin_no_wal_no_direct_db_gate.cpp
)
target_compile_features(sbsql_no_spin_no_wal_no_direct_db_gate PRIVATE cxx_std_23)
target_link_libraries(sbsql_no_spin_no_wal_no_direct_db_gate PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbsql_no_spin_no_wal_no_direct_db_gate
  COMMAND sbsql_no_spin_no_wal_no_direct_db_gate
          "${PROJECT_SOURCE_DIR}"
          "${SBSQL_REGRESSION_ARTIFACT_ROOT}"
)
set_tests_properties(sbsql_no_spin_no_wal_no_direct_db_gate PROPERTIES
  LABELS "sbsql_no_spin_no_wal_no_direct_db_gate;parser_diagnostics_security_gate;sbsql_parser_worker"
)

add_executable(sbsql_cache_epoch_correctness_conformance sbsql_cache_epoch_correctness_conformance.cpp)
target_compile_features(sbsql_cache_epoch_correctness_conformance PRIVATE cxx_std_23)
target_link_libraries(sbsql_cache_epoch_correctness_conformance PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbsql_cache_epoch_correctness_conformance
  COMMAND sbsql_cache_epoch_correctness_conformance
)
set_tests_properties(sbsql_cache_epoch_correctness_conformance PROPERTIES
  LABELS "sbsql_cache_epoch_correctness_conformance;parser_cache_invalidation_gate;parser_v3_closure_gate;sbsql_parser_worker;SML-007"
)

add_executable(
  sbsql_frontdoor_cache_orh_030_031_conformance
  sbsql_frontdoor_cache_orh_030_031_conformance.cpp
)
target_compile_features(sbsql_frontdoor_cache_orh_030_031_conformance PRIVATE cxx_std_23)
target_link_libraries(sbsql_frontdoor_cache_orh_030_031_conformance PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbsql_frontdoor_cache_orh_030_031_conformance
  COMMAND sbsql_frontdoor_cache_orh_030_031_conformance
)
set_tests_properties(sbsql_frontdoor_cache_orh_030_031_conformance PROPERTIES
  LABELS "sbsql_frontdoor_cache_orh_030_031_conformance;ORH-030;ORH-031;ORH-GATE-030;sbsql_parser_worker;frontdoor_lowering_cache;mga_transaction_regression;no_execution_plan_runtime"
)

add_executable(
  sbsql_frontdoor_lowering_cache_odfr_011_conformance
  sbsql_frontdoor_lowering_cache_odfr_011_conformance.cpp
)
target_compile_features(sbsql_frontdoor_lowering_cache_odfr_011_conformance PRIVATE cxx_std_23)
target_link_libraries(
  sbsql_frontdoor_lowering_cache_odfr_011_conformance
  PRIVATE
    sbl_sbsql_parser_worker_core
)
add_test(
  NAME sbsql_frontdoor_lowering_cache_odfr_011_conformance
  COMMAND sbsql_frontdoor_lowering_cache_odfr_011_conformance
)
set_tests_properties(sbsql_frontdoor_lowering_cache_odfr_011_conformance PROPERTIES
  LABELS "sbsql_frontdoor_lowering_cache_odfr_011_conformance;ODFR-011;sbsql_parser_worker;frontdoor_lowering_cache;benchmark_clean;mga_transaction_regression;no_execution_plan_runtime;SML-007;SML-090"
)

add_executable(sbsql_parser_v3_profile_meta_conformance sbsql_parser_v3_profile_meta_conformance.cpp)
target_compile_features(sbsql_parser_v3_profile_meta_conformance PRIVATE cxx_std_23)
target_link_libraries(sbsql_parser_v3_profile_meta_conformance PRIVATE sbl_sbsql_parser_pipeline)
add_test(
  NAME sbsql_parser_v3_profile_meta_conformance
  COMMAND sbsql_parser_v3_profile_meta_conformance
)
set_tests_properties(sbsql_parser_v3_profile_meta_conformance PROPERTIES
  LABELS "parser_v3_management_syntax_gate;parser_profile_authority_gate;parser_v3_closure_gate;parser_diagnostics_security_gate;sbsql_parser_worker"
)

add_test(
  NAME sbsql_sblr_operation_matrix_static_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/matrix/sbsql_sblr_operation_matrix_static_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sbsql_sblr_operation_matrix_static_gate PROPERTIES
  LABELS "sblr_operation_matrix_gate;sblr_lowering_runtime_gate;trusted_udr_bridge_gate;parser_v3_closure_gate;sbsql_parser_worker"
)

add_test(
  NAME sblr_operation_matrix_authority_gate
  COMMAND "${SBSQL_PYTHON3}"
          "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/matrix/sblr_operation_matrix_authority_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(sblr_operation_matrix_authority_gate PROPERTIES
  LABELS "sblr_operation_matrix_gate;sblr_operation_matrix_authority_gate;parser_v3_closure_gate;sbsql_parser_worker"
)

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sbu_sbsql_parser_support AND TARGET sb_server_core)
  add_executable(
    sbsql_fuzz_malicious_input_gate
    generated/hardening/sbsql_fuzz_malicious_input_gate.cpp
  )
  target_compile_features(sbsql_fuzz_malicious_input_gate PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_fuzz_malicious_input_gate
    PRIVATE
      sbl_sbsql_parser_pipeline
      sbl_sbsql_parser_worker_core
      sbu_sbsql_parser_support
      sb_server_core
  )
  set(SBSQL_MALICIOUS_INPUT_FIXTURES
    "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/hardening/MALICIOUS_INPUT_FIXTURES.csv"
  )
  add_test(
    NAME sbsql_fuzz_malicious_input_gate
    COMMAND sbsql_fuzz_malicious_input_gate "${SBSQL_MALICIOUS_INPUT_FIXTURES}"
  )
  set_tests_properties(sbsql_fuzz_malicious_input_gate PROPERTIES
    LABELS "sbsql_fuzz_malicious_input_gate;sbsql_fuzz_smoke;sbsql_malicious_input_smoke;parser_diagnostics_security_gate;sbsql_parser_worker"
  )
endif()

if(NOT WIN32 AND TARGET sbl_sbsql_parser_worker_core AND TARGET sb_server_core)
  add_executable(
    sbsql_native_wire_p1_conformance
    sbsql_native_wire_p1_conformance.cpp
  )
  target_compile_features(sbsql_native_wire_p1_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_native_wire_p1_conformance
    PRIVATE
      sbl_sbsql_parser_worker_core
      sb_core_datatypes
      sb_server_core
  )
  add_test(
    NAME sbsql_native_wire_p1_conformance
    COMMAND sbsql_native_wire_p1_conformance
  )
  set_tests_properties(sbsql_native_wire_p1_conformance PROPERTIES
    LABELS "sbsql_native_wire_p1_conformance;sbsql_parser_worker"
  )
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SBSQL_PYTHON3 "${Python3_EXECUTABLE}")
find_program(SBSQL_OPENSSL_EXECUTABLE openssl REQUIRED)
set(SBSQL_DETERMINISTIC_NO_NETWORK_GATE
  "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/repro/sbsql_deterministic_no_network_gate.py"
)
set(SBSQL_DETERMINISTIC_ARTIFACT_MANIFEST
  "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/repro/DETERMINISTIC_ARTIFACT_MANIFEST.csv"
)
add_custom_target(
  sbsql_deterministic_no_network_gate
  DEPENDS
    "${SBSQL_DETERMINISTIC_NO_NETWORK_GATE}"
    "${SBSQL_DETERMINISTIC_ARTIFACT_MANIFEST}"
)
add_test(
  NAME sbsql_deterministic_no_network_gate
  COMMAND "${SBSQL_PYTHON3}" "${SBSQL_DETERMINISTIC_NO_NETWORK_GATE}"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --artifact-root "${SBSQL_REGRESSION_ARTIFACT_ROOT}"
          --work-dir "${CMAKE_CURRENT_BINARY_DIR}/sbsql_deterministic_no_network_gate"
          --manifest "${SBSQL_DETERMINISTIC_ARTIFACT_MANIFEST}"
)
set_tests_properties(sbsql_deterministic_no_network_gate PROPERTIES
  LABELS "sbsql_deterministic_no_network_gate;sbsql_reproducibility_gate;sbsql_no_network_gate;sbsql_parser_worker"
)

if(TARGET sbl_listener_runtime AND TARGET sbl_sbsql_parser_worker_core AND TARGET sb_server_core)
  add_executable(
    sbsql_cross_platform_ipc_gate
    generated/platform/sbsql_cross_platform_ipc_gate.cpp
  )
  target_compile_features(sbsql_cross_platform_ipc_gate PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_cross_platform_ipc_gate
    PRIVATE
      sbl_listener_runtime
      sbl_sbsql_parser_worker_core
      sb_server_core
  )
  add_test(
    NAME sbsql_cross_platform_ipc_gate
    COMMAND sbsql_cross_platform_ipc_gate "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(sbsql_cross_platform_ipc_gate PROPERTIES
    LABELS "sbsql_cross_platform_ipc_gate;sbsql_platform_ipc_gate;sbsql_parser_worker"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_engine_internal_api AND TARGET sb_engine_sblr AND TARGET sb_core_resources AND TARGET sb_core_datatypes)
  add_executable(
    sbsql_locale_charset_collation_timezone_gate
    generated/i18n/sbsql_locale_charset_collation_timezone_gate.cpp
  )
  target_compile_features(sbsql_locale_charset_collation_timezone_gate PRIVATE cxx_std_23)
  target_compile_definitions(
    sbsql_locale_charset_collation_timezone_gate
    PRIVATE
      SB_FSP012E_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
  )
  target_link_libraries(
    sbsql_locale_charset_collation_timezone_gate
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_engine_internal_api
      sb_engine_sblr
      sb_core_resources
      sb_core_datatypes
  )
  add_test(
    NAME sbsql_locale_charset_collation_timezone_gate
    COMMAND sbsql_locale_charset_collation_timezone_gate
  )
  set_tests_properties(sbsql_locale_charset_collation_timezone_gate PROPERTIES
    LABELS "sbsql_locale_charset_collation_timezone_gate;sbsql_i18n_time_gate;sbsql_parser_worker"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core)
  add_executable(
    sbsql_metadata_result_shape_gate
    generated/result_shape/sbsql_metadata_result_shape_gate.cpp
  )
  target_compile_features(sbsql_metadata_result_shape_gate PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_metadata_result_shape_gate
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
  )
  add_test(
    NAME sbsql_metadata_result_shape_gate
    COMMAND sbsql_metadata_result_shape_gate
  )
  set_tests_properties(sbsql_metadata_result_shape_gate PROPERTIES
    LABELS "sbsql_metadata_result_shape_gate;sbsql_result_shape_gate;sbsql_parser_worker"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sb_server_core)
  add_executable(
    sbsql_security_redaction_side_channel_gate
    generated/security/sbsql_security_redaction_side_channel_gate.cpp
  )
  target_compile_features(sbsql_security_redaction_side_channel_gate PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_security_redaction_side_channel_gate
    PRIVATE
      sbl_sbsql_parser_pipeline
      sb_server_core
  )
  add_test(
    NAME sbsql_security_redaction_side_channel_gate
    COMMAND sbsql_security_redaction_side_channel_gate
            "${PROJECT_SOURCE_DIR}/.."
            "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/generated/security/SECURITY_REDACTION_SIDE_CHANNEL_FIXTURES.csv"
  )
  set_tests_properties(sbsql_security_redaction_side_channel_gate PROPERTIES
    LABELS "sbsql_security_redaction_side_channel_gate;sbsql_security_redaction_gate;sbsql_side_channel_gate;parser_diagnostics_security_gate;sbsql_parser_worker"
  )
endif()

if(NOT TARGET sb_engine_functions AND TARGET sb_engine_sblr)
  add_subdirectory(
    "${PROJECT_SOURCE_DIR}/src/engine/functions"
    "${CMAKE_CURRENT_BINARY_DIR}/sb_engine_functions_for_tests"
  )
endif()

if(TARGET sb_engine_functions)
  add_executable(
    sbsql_fixed_uuid_catalog_seed_gate
    generated/catalog/sbsql_fixed_uuid_catalog_seed_gate.cpp
  )
  target_compile_features(sbsql_fixed_uuid_catalog_seed_gate PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_fixed_uuid_catalog_seed_gate
    PRIVATE
      sb_engine_functions
  )
  add_test(
    NAME sbsql_fixed_uuid_catalog_seed_gate
    COMMAND sbsql_fixed_uuid_catalog_seed_gate "${PROJECT_SOURCE_DIR}/.."
  )
  set_tests_properties(sbsql_fixed_uuid_catalog_seed_gate PROPERTIES
    LABELS "sbsql_fixed_uuid_catalog_seed_gate;sbsql_parser_worker"
  )
endif()

add_executable(
  sbsql_upgrade_migration_compatibility_gate
  generated/upgrade/sbsql_upgrade_migration_compatibility_gate.cpp
)
target_compile_features(sbsql_upgrade_migration_compatibility_gate PRIVATE cxx_std_23)
add_test(
  NAME sbsql_upgrade_migration_compatibility_gate
  COMMAND sbsql_upgrade_migration_compatibility_gate
          "${PROJECT_SOURCE_DIR}/.."
          "${SBSQL_REGRESSION_ARTIFACT_ROOT}"
)
set_tests_properties(sbsql_upgrade_migration_compatibility_gate PROPERTIES
  LABELS "sbsql_upgrade_migration_compatibility_gate;sbsql_parser_worker"
)

if(TARGET sb_listener AND TARGET sbp_probe)
  add_test(NAME sbp_probe_product_exists COMMAND sbp_probe --help)
endif()

if(TARGET sb_engine_sblr AND TARGET sb_storage_database)
  add_executable(sbsql_database_create_schema_bootstrap_gate sbsql_database_create_schema_bootstrap_gate.cpp)
  target_compile_features(sbsql_database_create_schema_bootstrap_gate PRIVATE cxx_std_23)
  target_compile_definitions(
    sbsql_database_create_schema_bootstrap_gate
    PRIVATE
      SB_BOOTSTRAP_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
  )
  target_link_libraries(sbsql_database_create_schema_bootstrap_gate PRIVATE sb_engine_sblr sb_storage_database sb_core_uuid)
  add_test(NAME sbsql_database_create_schema_bootstrap_gate COMMAND sbsql_database_create_schema_bootstrap_gate)
  set_tests_properties(sbsql_database_create_schema_bootstrap_gate PROPERTIES
    LABELS "sbsql_database_create_schema_bootstrap_gate;sbsql_parser_worker;schema_bootstrap;engine_uuid_return;sys_catalog_table_materialization_gate;catalog_common_descriptor_gate;catalog_generation_mga_gate"
  )
endif()

if(TARGET sb_listener AND TARGET sbp_sbsql AND TARGET sb_listener_runtime_handoff_smoke)
  add_executable(sbp_sbsql_handoff_smoke sbsql_handoff_smoke.cpp)
  target_compile_features(sbp_sbsql_handoff_smoke PRIVATE cxx_std_23)
  add_test(
    NAME sb_listener_sbp_sbsql_handoff_smoke
    COMMAND sbp_sbsql_handoff_smoke
            $<TARGET_FILE:sb_listener>
            $<TARGET_FILE:sbp_sbsql>
  )
endif()

if(NOT WIN32 AND TARGET sb_server AND TARGET sb_listener AND TARGET sbp_sbsql)
  if(TARGET sb_engine_sblr AND TARGET sb_storage_database)
    add_executable(sbsql_example_database_seed sbsql_example_database_seed.cpp)
    target_compile_features(sbsql_example_database_seed PRIVATE cxx_std_23)
    target_link_libraries(sbsql_example_database_seed PRIVATE sb_engine_sblr sb_storage_database sb_core_uuid)
  endif()

  add_executable(sbp_sbsql_full_route_execution_smoke sbsql_full_route_execution_smoke.cpp)
  target_compile_features(sbp_sbsql_full_route_execution_smoke PRIVATE cxx_std_23)
  add_test(
    NAME sb_listener_sbp_sbsql_server_engine_execution_smoke
    COMMAND sbp_sbsql_full_route_execution_smoke
            $<TARGET_FILE:sb_server>
            $<TARGET_FILE:sb_listener>
            $<TARGET_FILE:sbp_sbsql>
            $<$<TARGET_EXISTS:sbsql_example_database_seed>:$<TARGET_FILE:sbsql_example_database_seed>>
  )
  set_tests_properties(sb_listener_sbp_sbsql_server_engine_execution_smoke PROPERTIES
    LABELS "public_single_node_full_route_gate;sbsql_canary_vertical_slice_gate;sb_server_sbsql_admission_conformance;sbsql_full_route_streaming_conformance;sbps_chunked_payload_conformance;sbsql_copy_streaming_conformance;sbsql_multi_result_conformance;sbsql_warning_partial_result_conformance;sbsql_stream_finality_conformance;SBSQL-TEMPORARY-TABLE-PROOF-CLOSURE;TEMP-TABLE-GATE-015;ROUTINE-CURSOR-PROOF-CLOSURE;ROUTINE-CURSOR-GATE-009;sbsql_parser_worker;database_lifecycle;database_lifecycle_server_route;database_lifecycle_parser_route;server_lifecycle_gate;listener_sbct_pool_gate;local_ipc_gate;sbwp_wire_gate;routing_reconnect_finality_gate"
  )
  if(TARGET sb_isql)
    add_test(
      NAME sbsql_sb_isql_live_route_smoke
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_sb_isql_live_route_smoke.py"
              --server $<TARGET_FILE:sb_server>
              --listener $<TARGET_FILE:sb_listener>
              --parser-worker $<TARGET_FILE:sbp_sbsql>
              --sb-isql $<TARGET_FILE:sb_isql>
              --work-dir "${CMAKE_BINARY_DIR}/sb_isql_live"
    )
    set_tests_properties(sbsql_sb_isql_live_route_smoke PROPERTIES
      LABELS "public_single_node_full_route_gate;sbsql_parser_worker;sb_isql;cli_tool_gate;database_lifecycle;database_lifecycle_server_route;database_lifecycle_parser_route;security_auth_gate;listener_sbct_pool_gate;local_ipc_gate;sbwp_wire_gate;routing_reconnect_finality_gate;mga_transaction_regression"
    )
  endif()
  if(TARGET sb_isql AND TARGET sbmn_manager AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(
      NAME sbsql_linux_manager_listener_parser_ipc_server_full_path_smoke
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_linux_manager_full_path_smoke.py"
              --server $<TARGET_FILE:sb_server>
              --listener $<TARGET_FILE:sb_listener>
              --manager $<TARGET_FILE:sbmn_manager>
              --parser-worker $<TARGET_FILE:sbp_sbsql>
              --sb-isql $<TARGET_FILE:sb_isql>
              --work-dir "${CMAKE_BINARY_DIR}/sbsql_linux_manager_full_path"
              --evidence-file "${CMAKE_BINARY_DIR}/tests/sbsql_parser_worker/sbsql_linux_manager_full_path_smoke.json"
    )
    set_tests_properties(sbsql_linux_manager_listener_parser_ipc_server_full_path_smoke PROPERTIES
      LABELS "public_single_node_full_route_gate;linux_manager_full_path_gate;sbsql_parser_worker;sb_isql;cli_tool_gate;manager_proxy_gate;dbbt_lpreface_gate;database_lifecycle;database_lifecycle_server_route;database_lifecycle_parser_route;security_auth_gate;listener_sbct_pool_gate;local_ipc_gate;sbwp_wire_gate;routing_reconnect_finality_gate;mga_transaction_regression;engine_listener_enterprise"
    )
  endif()
  if(TARGET sb_isql AND TARGET sb_ipc_tester)
    add_custom_target(live_server_agent_storage_benchmark_gate
      DEPENDS
        sb_server
        sb_listener
        sbp_sbsql
        sb_ipc_tester
        sb_isql
    )
    add_test(
      NAME live_server_agent_storage_benchmark_gate
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/live_server_agent_storage_benchmark_gate.py"
              --server $<TARGET_FILE:sb_server>
              --listener $<TARGET_FILE:sb_listener>
              --parser-worker $<TARGET_FILE:sbp_sbsql>
              --sb-isql $<TARGET_FILE:sb_isql>
              --ipc-tester $<TARGET_FILE:sb_ipc_tester>
              --work-dir "${CMAKE_BINARY_DIR}/live_server_agent_storage"
    )
    set_tests_properties(live_server_agent_storage_benchmark_gate PROPERTIES
      LABELS "live_server_agent_storage_benchmark_gate;PFAR-019;live_server;fresh_database;database_lifecycle;database_lifecycle_server_route;database_lifecycle_parser_route;agent_runtime;storage_agents;supportability;listener_sbct_pool_gate;local_ipc_gate;sbwp_wire_gate;mga_transaction_regression"
    )
    add_custom_target(all_noncluster_agents_live_gate
      DEPENDS
        sb_server
        sb_listener
        sbp_sbsql
        sb_ipc_tester
        sb_isql
    )
    add_test(
      NAME all_noncluster_agents_live_gate
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/all_noncluster_agents_live_gate.py"
              --server $<TARGET_FILE:sb_server>
              --listener $<TARGET_FILE:sb_listener>
              --parser-worker $<TARGET_FILE:sbp_sbsql>
              --sb-isql $<TARGET_FILE:sb_isql>
              --ipc-tester $<TARGET_FILE:sb_ipc_tester>
              --agent-runtime-source "${PROJECT_SOURCE_DIR}/src/core/agents/agent_runtime.cpp"
              --work-dir "${CMAKE_BINARY_DIR}/all_noncluster_agents_live"
    )
    set_tests_properties(all_noncluster_agents_live_gate PROPERTIES
      LABELS "all_noncluster_agents_live_gate;PFAR-019A;live_server;fresh_database;database_lifecycle;database_lifecycle_server_route;database_lifecycle_parser_route;agent_runtime;storage_agents;supportability;listener_sbct_pool_gate;local_ipc_gate;sbwp_wire_gate;mga_transaction_regression"
    )
    add_custom_target(server_agent_thread_runtime_gate
      DEPENDS
        sb_server
        sb_ipc_tester
    )
    add_test(
      NAME server_agent_thread_runtime_gate
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/server_agent_thread_runtime_gate.py"
              --server $<TARGET_FILE:sb_server>
              --ipc-tester $<TARGET_FILE:sb_ipc_tester>
              --work-dir "${CMAKE_BINARY_DIR}/server_agent_thread_runtime"
    )
    set_tests_properties(server_agent_thread_runtime_gate PROPERTIES
      LABELS "server_agent_thread_runtime_gate;PFAR-019B;live_server;fresh_database;database_lifecycle;database_lifecycle_server_route;agent_runtime;storage_agents;local_ipc_gate;mga_transaction_regression"
    )
    add_custom_target(server_agent_runtime_process_kill_replay_gate
      DEPENDS
        sb_server
        sb_ipc_tester
    )
    add_test(
      NAME server_agent_runtime_process_kill_replay_gate
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/server_agent_runtime_process_kill_replay_gate.py"
              --server $<TARGET_FILE:sb_server>
              --ipc-tester $<TARGET_FILE:sb_ipc_tester>
              --work-dir "${CMAKE_BINARY_DIR}/server_agent_runtime_process_kill_replay"
    )
    set_tests_properties(server_agent_runtime_process_kill_replay_gate PROPERTIES
      LABELS "server_agent_runtime_process_kill_replay_gate;AEIC-011;AEIC-012;AEIC-041;live_server;fresh_database;database_lifecycle;database_lifecycle_server_route;agent_runtime;process_kill;durable_replay;local_ipc_gate;mga_transaction_regression"
    )
  endif()
  if(TARGET sb_isql)
    add_test(
      NAME sbsql_sb_isql_local_ipc_route_smoke
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_sb_isql_local_ipc_route_smoke.py"
              --server $<TARGET_FILE:sb_server>
              --sb-isql $<TARGET_FILE:sb_isql>
              --work-dir "${CMAKE_BINARY_DIR}/sb_isql_local_ipc"
    )
    set_tests_properties(sbsql_sb_isql_local_ipc_route_smoke PROPERTIES
      LABELS "public_single_node_full_route_gate;sbsql_parser_worker;sb_isql;cli_tool_gate;database_lifecycle;database_lifecycle_server_route;database_lifecycle_parser_route;security_auth_gate;local_ipc_gate;sbwp_wire_gate;routing_reconnect_finality_gate;mga_transaction_regression"
    )
    add_test(
      NAME sbsql_sb_isql_embedded_route_smoke
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_sb_isql_embedded_route_smoke.py"
              --sb-isql $<TARGET_FILE:sb_isql>
              --work-dir "${CMAKE_BINARY_DIR}/sb_isql_embedded"
    )
    set_tests_properties(sbsql_sb_isql_embedded_route_smoke PROPERTIES
      LABELS "public_single_node_full_route_gate;sbsql_parser_worker;sb_isql;cli_tool_gate;database_lifecycle;database_lifecycle_embedded_route;embedded_engine_gate;sbwp_wire_gate;mga_transaction_regression"
    )
    add_test(
      NAME cdp_route_split_benchmark_gate
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/cdp_route_split_benchmark_gate.py"
              --server $<TARGET_FILE:sb_server>
              --listener $<TARGET_FILE:sb_listener>
              --parser-worker $<TARGET_FILE:sbp_sbsql>
              --sb-isql $<TARGET_FILE:sb_isql>
              --work-dir "${CMAKE_BINARY_DIR}/cdp_rs"
              --build-mode "configured"
    )
    set_tests_properties(cdp_route_split_benchmark_gate PROPERTIES
      LABELS "cdp_route_split_benchmark_gate;CDP-001;CDP-003;CDP-004;CDP-005;CDP-009;CDP-048;benchmark;performance;sbsql_parser_worker;sb_isql;database_lifecycle;database_lifecycle_server_route;database_lifecycle_parser_route;database_lifecycle_embedded_route;local_ipc_gate;sbwp_wire_gate;mga_transaction_regression"
    )
    add_test(
      NAME cdp_copy_route_gate
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/cdp_copy_route_gate.py"
              --server $<TARGET_FILE:sb_server>
              --listener $<TARGET_FILE:sb_listener>
              --parser-worker $<TARGET_FILE:sbp_sbsql>
              --sb-isql $<TARGET_FILE:sb_isql>
              --work-dir "${CMAKE_BINARY_DIR}/cdp_copy_route"
    )
    set_tests_properties(cdp_copy_route_gate PROPERTIES
      LABELS "cdp_copy_route_gate;CDP-012;benchmark;performance;sbsql_parser_worker;sb_isql;database_lifecycle;database_lifecycle_server_route;database_lifecycle_parser_route;database_lifecycle_embedded_route;local_ipc_gate;sbwp_wire_gate;mga_transaction_regression"
    )
    add_test(
      NAME sbsql_copy_persistence_full_route_gate
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_copy_persistence_full_route_gate.py"
              --server $<TARGET_FILE:sb_server>
              --listener $<TARGET_FILE:sb_listener>
              --parser-worker $<TARGET_FILE:sbp_sbsql>
              --sb-isql $<TARGET_FILE:sb_isql>
              --fixture-root "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/fixtures/copy_persistence"
              --openssl ${SBSQL_OPENSSL_EXECUTABLE}
              --work-dir "${CMAKE_BINARY_DIR}/sbsql_copy_persistence_full_route"
    )
    set_tests_properties(sbsql_copy_persistence_full_route_gate PROPERTIES
      LABELS "sbsql_copy_persistence_full_route_gate;copy_persistence;public_single_node_full_route_gate;public_release_foundation_full_route_gate;sbsql_parser_worker;sb_isql;database_lifecycle;database_lifecycle_server_route;database_lifecycle_parser_route;security_auth_gate;sbwp_tls_server_listener_gate;tls_regression_gate;local_ipc_gate;sbwp_wire_gate;routing_reconnect_finality_gate;mga_transaction_regression"
    )
    add_custom_target(cdp_native_bulk_ingest_cli_gate
      DEPENDS sb_isql sb_server sb_listener sbp_sbsql
    )
    add_test(
      NAME cdp_native_bulk_ingest_cli_gate
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/cdp_native_bulk_ingest_cli_gate.py"
              --server $<TARGET_FILE:sb_server>
              --listener $<TARGET_FILE:sb_listener>
              --parser-worker $<TARGET_FILE:sbp_sbsql>
              --sb-isql $<TARGET_FILE:sb_isql>
              --work-dir "${CMAKE_BINARY_DIR}/cdp041"
    )
    set_tests_properties(cdp_native_bulk_ingest_cli_gate PROPERTIES
      LABELS "cdp_native_bulk_ingest_cli_gate;CDP-041;performance;sbsql_parser_worker;sb_isql;database_lifecycle;database_lifecycle_server_route;database_lifecycle_parser_route;database_lifecycle_embedded_route;local_ipc_gate;sbwp_wire_gate;mga_transaction_regression"
    )
    add_test(
      NAME cdp_config_defaults_rollback_gate
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/cdp_config_defaults_rollback_gate.py"
              --repo-root "${PROJECT_SOURCE_DIR}/.."
              --server $<TARGET_FILE:sb_server>
              --listener $<TARGET_FILE:sb_listener>
              --parser-worker $<TARGET_FILE:sbp_sbsql>
              --sb-isql $<TARGET_FILE:sb_isql>
              --work-dir "${CMAKE_BINARY_DIR}/cdp049"
    )
    set_tests_properties(cdp_config_defaults_rollback_gate PROPERTIES
      LABELS "cdp_config_defaults_rollback_gate;CDP-049;CDP_P5_CONFIG_ROLLBACK_READY;config_defaults;rollback_disable;benchmark;performance;sbsql_parser_worker;sb_isql;database_lifecycle;local_ipc_gate;sbwp_wire_gate;mga_transaction_regression"
    )
    add_test(
      NAME cdp_profiler_evidence_gate
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/cdp_profiler_evidence_gate.py"
              --server $<TARGET_FILE:sb_server>
              --listener $<TARGET_FILE:sb_listener>
              --parser-worker $<TARGET_FILE:sbp_sbsql>
              --sb-isql $<TARGET_FILE:sb_isql>
              --work-dir "${CMAKE_BINARY_DIR}/cdp_profiler_evidence"
              --build-mode "configured"
    )
    set_tests_properties(cdp_profiler_evidence_gate PROPERTIES
      LABELS "cdp_profiler_evidence_gate;CDP-046;CDP_P5_PROFILER_EVIDENCE_READY;benchmark;performance;sbsql_parser_worker;sb_isql;database_lifecycle;local_ipc_gate;sbwp_wire_gate;mga_transaction_regression"
    )
    add_test(
      NAME cdp_soak_leak_stability_gate
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/cdp_soak_leak_stability_gate.py"
              --server $<TARGET_FILE:sb_server>
              --listener $<TARGET_FILE:sb_listener>
              --parser-worker $<TARGET_FILE:sbp_sbsql>
              --sb-isql $<TARGET_FILE:sb_isql>
              --work-dir "${CMAKE_BINARY_DIR}/cdp_soak_leak_stability"
              --build-mode "configured"
    )
    set_tests_properties(cdp_soak_leak_stability_gate PROPERTIES
      LABELS "cdp_soak_leak_stability_gate;CDP-047;CDP_P5_SOAK_LEAK_READY;soak;stability;benchmark;performance;sbsql_parser_worker;sb_isql;database_lifecycle;local_ipc_gate;sbwp_wire_gate;mga_transaction_regression"
    )
    add_test(
      NAME cdp_final_regression_benchmark_gate
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/cdp_final_regression_benchmark_gate.py"
              --repo-root "${PROJECT_SOURCE_DIR}/.."
              --build-root "${CMAKE_BINARY_DIR}"
              --server $<TARGET_FILE:sb_server>
              --listener $<TARGET_FILE:sb_listener>
              --parser-worker $<TARGET_FILE:sbp_sbsql>
              --sb-isql $<TARGET_FILE:sb_isql>
              --work-dir "${CMAKE_BINARY_DIR}/cdp050"
    )
    set_tests_properties(cdp_final_regression_benchmark_gate PROPERTIES
      LABELS "cdp_final_regression_benchmark_gate;CDP-050;CDP_P5_REGRESSION_READY;benchmark;performance;release_gate;sbsql_parser_worker;sb_isql;local_ipc_gate;sbwp_wire_gate;mga_transaction_regression"
    )
    add_test(
      NAME cdp_security_api_abi_gate
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/cdp_security_api_abi_gate.py"
              --repo-root "${PROJECT_SOURCE_DIR}/.."
              --server $<TARGET_FILE:sb_server>
              --listener $<TARGET_FILE:sb_listener>
              --parser-worker $<TARGET_FILE:sbp_sbsql>
              --sb-isql $<TARGET_FILE:sb_isql>
              --work-dir "${CMAKE_BINARY_DIR}/cdp_security_api_abi"
    )
    set_tests_properties(cdp_security_api_abi_gate PROPERTIES
      LABELS "cdp_security_api_abi_gate;CDP-007;CDP-008;security_auth_gate;api_abi_gate;route_semantic_smoke;sbsql_parser_worker;sb_isql;database_lifecycle;database_lifecycle_server_route;database_lifecycle_parser_route;database_lifecycle_embedded_route;local_ipc_gate;sbwp_wire_gate;mga_transaction_regression"
    )
    add_test(
      NAME sbsql_sb_isql_embedded_server_authority_smoke
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_sb_isql_embedded_server_authority_smoke.py"
              --server $<TARGET_FILE:sb_server>
              --sb-isql $<TARGET_FILE:sb_isql>
              --work-dir "${CMAKE_BINARY_DIR}/sb_isql_embedded_authority"
    )
    set_tests_properties(sbsql_sb_isql_embedded_server_authority_smoke PROPERTIES
      LABELS "public_single_node_full_route_gate;sbsql_parser_worker;sb_isql;cli_tool_gate;database_lifecycle;database_lifecycle_server_route;database_lifecycle_embedded_route;embedded_engine_gate;security_auth_gate;local_ipc_gate;sbwp_wire_gate;routing_reconnect_finality_gate;mga_transaction_regression"
    )
  endif()
  set(SBSQL_SBWP_TLS_ENGINE_AUTH_ROUTE_COMMAND
    "${SBSQL_PYTHON3}"
    "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_sbwp_tls_engine_auth_route_smoke.py"
    --repo-root "${PROJECT_SOURCE_DIR}/.."
    --server $<TARGET_FILE:sb_server>
    --listener $<TARGET_FILE:sb_listener>
    --parser-worker $<TARGET_FILE:sbp_sbsql>
    --openssl ${SBSQL_OPENSSL_EXECUTABLE}
    --work-dir "${CMAKE_BINARY_DIR}/tls/s1"
    $<$<TARGET_EXISTS:sbsql_example_database_seed>:--example-db-seeder>
    $<$<TARGET_EXISTS:sbsql_example_database_seed>:$<TARGET_FILE:sbsql_example_database_seed>>
  )
  add_test(
    NAME sb_listener_sbp_sbsql_sbwp_tls_engine_auth_route_smoke
    COMMAND ${SBSQL_SBWP_TLS_ENGINE_AUTH_ROUTE_COMMAND}
  )
  set_tests_properties(sb_listener_sbp_sbsql_sbwp_tls_engine_auth_route_smoke PROPERTIES
    LABELS "public_single_node_full_route_gate;public_release_foundation;public_release_foundation_full_route_gate;sbwp_tls_server_listener_gate;sbwp_tls_engine_auth_gate;sbwp_tls_negative_policy_gate;tls_fixture_policy_gate;tls_regression_gate;sbsql_sbwp_tls_route;sbsql_engine_auth_authority;sbsql_parser_worker;sb_server_sbsql_admission_conformance;database_lifecycle;database_lifecycle_full_route_conformance;database_lifecycle_server_route;database_lifecycle_parser_route;database_lifecycle_security;security_auth_gate;server_lifecycle_gate;listener_sbct_pool_gate;local_ipc_gate;sbwp_wire_gate;routing_reconnect_finality_gate;mga_transaction_regression"
  )
  add_test(
    NAME database_lifecycle_full_route_conformance
    COMMAND "${SBSQL_PYTHON3}"
            "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_database_lifecycle_full_route_conformance.py"
            --repo-root "${PROJECT_SOURCE_DIR}/.."
            --server $<TARGET_FILE:sb_server>
            --listener $<TARGET_FILE:sb_listener>
            --parser-worker $<TARGET_FILE:sbp_sbsql>
            --openssl ${SBSQL_OPENSSL_EXECUTABLE}
            --work-dir "${CMAKE_BINARY_DIR}/tls/s2"
            $<$<TARGET_EXISTS:sbsql_example_database_seed>:--example-db-seeder>
            $<$<TARGET_EXISTS:sbsql_example_database_seed>:$<TARGET_FILE:sbsql_example_database_seed>>
  )
  set_tests_properties(database_lifecycle_full_route_conformance PROPERTIES
    LABELS "public_single_node_full_route_gate;public_release_foundation;public_release_foundation_full_route_gate;sbwp_tls_server_listener_gate;sbwp_tls_engine_auth_gate;tls_fixture_policy_gate;tls_regression_gate;database_lifecycle_full_route_conformance;database_lifecycle;database_lifecycle_server_route;database_lifecycle_parser_route;database_lifecycle_security;sbsql_parser_worker;sbsql_sbwp_tls_route;sbsql_engine_auth_authority;security_auth_gate;server_lifecycle_gate;listener_sbct_pool_gate;local_ipc_gate;sbwp_wire_gate;routing_reconnect_finality_gate;mga_transaction_regression"
  )

  if(TARGET sb_isql AND TARGET sbsql_example_database_seed)
    add_custom_target(legacy_execution_plan10_live_scratchbird_benchmark_gate
      DEPENDS
        sb_server
        sb_listener
        sbp_sbsql
        sbsql_example_database_seed
        sb_isql
    )
    add_test(
      NAME legacy_execution_plan10_live_scratchbird_benchmark_gate
      COMMAND "${SBSQL_PYTHON3}"
              "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/legacy_execution_plan10_live_scratchbird_benchmark_gate.py"
              --repo-root "${PROJECT_SOURCE_DIR}/.."
              --server $<TARGET_FILE:sb_server>
              --listener $<TARGET_FILE:sb_listener>
              --parser-worker $<TARGET_FILE:sbp_sbsql>
              --example-db-seeder $<TARGET_FILE:sbsql_example_database_seed>
              --sb-isql $<TARGET_FILE:sb_isql>
              --openssl ${SBSQL_OPENSSL_EXECUTABLE}
              --work-dir "${CMAKE_BINARY_DIR}/live_benchmarks"
    )
    set_tests_properties(legacy_execution_plan10_live_scratchbird_benchmark_gate PROPERTIES
      LABELS "legacy_execution_plan10_live_scratchbird_benchmark_gate;PFAR-020;benchmark;performance;legacy_execution_plan10;scratchbird_dialect;live_server;fresh_database;sbsql_sbwp_tls_route;mga_transaction_regression"
    )
  endif()
endif()

if(WIN32 AND TARGET sb_server AND TARGET sb_listener AND TARGET sbp_sbsql AND TARGET sb_isql)
  add_test(
    NAME sbsql_sb_isql_live_route_smoke
    COMMAND "${SBSQL_PYTHON3}"
            "${PROJECT_SOURCE_DIR}/tests/sbsql_parser_worker/sbsql_sb_isql_live_route_smoke.py"
            --server $<TARGET_FILE:sb_server>
            --listener $<TARGET_FILE:sb_listener>
            --parser-worker $<TARGET_FILE:sbp_sbsql>
            --sb-isql $<TARGET_FILE:sb_isql>
            --work-dir "${CMAKE_BINARY_DIR}/sb_isql_live"
  )
  set_tests_properties(sbsql_sb_isql_live_route_smoke PROPERTIES
    LABELS "public_single_node_full_route_gate;sbsql_parser_worker;sb_isql;cli_tool_gate;database_lifecycle;database_lifecycle_server_route;database_lifecycle_parser_route;security_auth_gate;listener_sbct_pool_gate;local_ipc_gate;sbwp_wire_gate;routing_reconnect_finality_gate;mga_transaction_regression;windows_live_route_gate"
  )
endif()

if(TARGET sb_server_core)
  add_executable(
    sbsql_database_lifecycle_parser_conformance
    sbsql_database_lifecycle_parser_conformance.cpp
  )
  target_compile_features(sbsql_database_lifecycle_parser_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_database_lifecycle_parser_conformance
    PRIVATE
      sbl_sbsql_parser_worker_core
      sb_server_core
  )
  add_test(
    NAME database_lifecycle_parser_conformance
    COMMAND sbsql_database_lifecycle_parser_conformance
  )
  set_tests_properties(database_lifecycle_parser_conformance PROPERTIES
    LABELS "database_lifecycle_parser_conformance;database_lifecycle_parser;database_lifecycle;sbsql_parser_worker;parser_lifecycle;mga_transaction_regression"
  )

  add_executable(sb_server_sbsql_admission_conformance sbsql_server_admission_conformance.cpp)
  target_compile_features(sb_server_sbsql_admission_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sb_server_sbsql_admission_conformance
    PRIVATE
      sb_server_core
      sbl_sbsql_parser_pipeline
  )
  if(TARGET sb_cluster_provider)
    target_link_libraries(sb_server_sbsql_admission_conformance PRIVATE sb_cluster_provider)
  endif()
  add_test(
    NAME sb_server_sbsql_admission_conformance
    COMMAND sb_server_sbsql_admission_conformance
  )
  set_tests_properties(sb_server_sbsql_admission_conformance PROPERTIES
    LABELS "sb_server_sbsql_admission_conformance;SBSFC-025;sbsql_parser_worker"
  )

  add_executable(
    sbsql_server_language_session_control_conformance
    sbsql_server_language_session_control_conformance.cpp
  )
  target_compile_features(sbsql_server_language_session_control_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_server_language_session_control_conformance
    PRIVATE
      sb_server_core
      sbl_sbsql_parser_pipeline
  )
  add_test(
    NAME sbsql_server_language_session_control_conformance
    COMMAND sbsql_server_language_session_control_conformance
  )
  set_tests_properties(sbsql_server_language_session_control_conformance PROPERTIES
    LABELS "sbsql_server_language_session_control;SML-008;SML-009;SML-018;SML-020;sbsql_parser_worker;server_session_language_context"
  )

  add_executable(
    sbsql_server_language_resource_directory_conformance
    sbsql_server_language_resource_directory_conformance.cpp
  )
  target_compile_features(sbsql_server_language_resource_directory_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_server_language_resource_directory_conformance
    PRIVATE
      sb_server_core
  )
  add_test(
    NAME sbsql_server_language_resource_directory_conformance
    COMMAND sbsql_server_language_resource_directory_conformance
  )
  set_tests_properties(sbsql_server_language_resource_directory_conformance PROPERTIES
    LABELS "sbsql_server_language_resource_directory;SML-015;SML-083;SML-085;SML-093;sbsql_parser_worker;server_session_language_context"
  )

  add_executable(
    sbsql_sml022_shared_parser_runtime_authority_conformance
    sbsql_sml022_shared_parser_runtime_authority_conformance.cpp
  )
  target_compile_features(sbsql_sml022_shared_parser_runtime_authority_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sbsql_sml022_shared_parser_runtime_authority_conformance
    PRIVATE
      sb_server_core
      sbl_sbsql_parser_pipeline
  )
  add_test(
    NAME sbsql_sml022_shared_parser_runtime_authority_conformance
    COMMAND sbsql_sml022_shared_parser_runtime_authority_conformance
  )
  set_tests_properties(sbsql_sml022_shared_parser_runtime_authority_conformance PROPERTIES
    LABELS "sbsql_shared_parser_runtime_authority;sbsql_parser_worker;SML-022;SML-GATE-022;mga_transaction_regression"
  )

  add_test(
    NAME sbsql_cluster_private_fail_closed_conformance
    COMMAND sb_server_sbsql_admission_conformance
  )
  set_tests_properties(sbsql_cluster_private_fail_closed_conformance PROPERTIES
    LABELS "sbsql_cluster_private_fail_closed_conformance;SBSFC-025;sbsql_parser_worker"
  )

  add_executable(sbsql_tls_transport_policy_conformance sbsql_tls_transport_policy_conformance.cpp)
  target_compile_features(sbsql_tls_transport_policy_conformance PRIVATE cxx_std_23)
  target_link_libraries(sbsql_tls_transport_policy_conformance PRIVATE sb_server_core)
  add_test(
    NAME sbsql_tls_transport_policy_conformance
    COMMAND sbsql_tls_transport_policy_conformance
  )
  set_tests_properties(sbsql_tls_transport_policy_conformance PROPERTIES
    LABELS "public_release_foundation;sbwp_tls_engine_auth_gate;sbwp_tls_negative_policy_gate;tls_regression_gate;sbsql_engine_auth_authority;sbsql_parser_worker;database_lifecycle_security;security_auth_gate"
  )
endif()

set(SBSQL_PARSER_LIFECYCLE_TESTS
  database_lifecycle_parser_conformance
  database_lifecycle_full_route_conformance
  sb_listener_sbp_sbsql_handoff_smoke
  sb_listener_sbp_sbsql_server_engine_execution_smoke
  sb_listener_sbp_sbsql_sbwp_tls_engine_auth_route_smoke
  sb_server_sbsql_admission_conformance
  sbsql_tls_transport_policy_conformance
  sbsql_stream_finality_conformance
)

foreach(test_name IN LISTS SBSQL_PARSER_LIFECYCLE_TESTS)
  if(TEST ${test_name})
    set_property(TEST ${test_name} APPEND PROPERTY LABELS
      database_lifecycle_parser database_lifecycle parser_lifecycle
    )
  endif()
endforeach()

if(TARGET sb_server_core)
  add_executable(sb_streaming_result_protocol_conformance sbsql_streaming_result_protocol_conformance.cpp)
  target_compile_features(sb_streaming_result_protocol_conformance PRIVATE cxx_std_23)
  target_link_libraries(sb_streaming_result_protocol_conformance PRIVATE sb_server_core)
  add_test(
    NAME sb_streaming_result_protocol_conformance
    COMMAND sb_streaming_result_protocol_conformance
  )
  set_tests_properties(sb_streaming_result_protocol_conformance PROPERTIES
    LABELS "sb_streaming_result_protocol_conformance;sbsql_parser_worker"
  )
endif()

if(TARGET sb_server_core)
  add_executable(sb_engine_backed_streaming_conformance sbsql_engine_backed_streaming_conformance.cpp)
  target_compile_features(sb_engine_backed_streaming_conformance PRIVATE cxx_std_23)
  target_link_libraries(sb_engine_backed_streaming_conformance PRIVATE sb_server_core)
  add_test(
    NAME sb_engine_backed_streaming_conformance
    COMMAND sb_engine_backed_streaming_conformance
  )
  set_tests_properties(sb_engine_backed_streaming_conformance PROPERTIES
    LABELS "sb_engine_backed_streaming_conformance;sbsql_parser_worker"
  )
endif()

if(TARGET sb_server_core)
  add_executable(sb_server_cursor_protocol_conformance sbsql_server_cursor_protocol_conformance.cpp)
  target_compile_features(sb_server_cursor_protocol_conformance PRIVATE cxx_std_23)
  target_link_libraries(sb_server_cursor_protocol_conformance PRIVATE sb_server_core)
  add_test(
    NAME sb_server_cursor_protocol_conformance
    COMMAND sb_server_cursor_protocol_conformance
  )
  set_tests_properties(sb_server_cursor_protocol_conformance PROPERTIES
    LABELS "sb_server_cursor_protocol_conformance;sbsql_parser_worker"
  )
endif()

if(TARGET sb_server_core)
  add_executable(sbps_chunked_payload_conformance sbsql_sbps_chunked_payload_conformance.cpp)
  target_compile_features(sbps_chunked_payload_conformance PRIVATE cxx_std_23)
  target_link_libraries(sbps_chunked_payload_conformance PRIVATE sb_server_core)
  add_test(
    NAME sbps_chunked_payload_conformance
    COMMAND sbps_chunked_payload_conformance
  )
  set_tests_properties(sbps_chunked_payload_conformance PROPERTIES
    LABELS "sbps_chunked_payload_conformance;sbsql_parser_worker"
  )
endif()

if(TARGET sb_server_core)
  add_executable(
    sbsql_negative_chunk_sequence_protocol_conformance
    sbsql_negative_chunk_sequence_protocol_conformance.cpp
  )
  target_compile_features(sbsql_negative_chunk_sequence_protocol_conformance PRIVATE cxx_std_23)
  target_link_libraries(sbsql_negative_chunk_sequence_protocol_conformance PRIVATE sb_server_core)
  add_test(
    NAME sbsql_negative_chunk_sequence_protocol_conformance
    COMMAND sbsql_negative_chunk_sequence_protocol_conformance
  )
  set_tests_properties(sbsql_negative_chunk_sequence_protocol_conformance PROPERTIES
    LABELS "sbsql_negative_chunk_sequence_protocol_conformance;sbsql_parser_worker;negative_protocol_gate"
  )
endif()

if(TARGET sb_server_core)
  add_executable(sbsql_copy_streaming_conformance sbsql_copy_streaming_conformance.cpp)
  target_compile_features(sbsql_copy_streaming_conformance PRIVATE cxx_std_23)
  target_link_libraries(sbsql_copy_streaming_conformance PRIVATE sb_server_core sb_core_memory)
  target_compile_definitions(
    sbsql_copy_streaming_conformance
    PRIVATE
      SB_SBSFC021_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
  )
  add_test(
    NAME sbsql_copy_streaming_conformance
    COMMAND sbsql_copy_streaming_conformance
  )
  set_tests_properties(sbsql_copy_streaming_conformance PROPERTIES
    LABELS "sbsql_copy_streaming_conformance;sbsql_parser_worker"
  )
endif()

if(TARGET sb_server_core)
  add_executable(sbsql_multi_result_conformance sbsql_multi_result_conformance.cpp)
  target_compile_features(sbsql_multi_result_conformance PRIVATE cxx_std_23)
  target_link_libraries(sbsql_multi_result_conformance PRIVATE sb_server_core)
  add_test(
    NAME sbsql_multi_result_conformance
    COMMAND sbsql_multi_result_conformance
  )
  set_tests_properties(sbsql_multi_result_conformance PROPERTIES
    LABELS "sbsql_multi_result_conformance;sbsql_parser_worker"
  )
endif()

if(TARGET sb_server_core)
  add_executable(sbsql_warning_partial_result_conformance sbsql_warning_partial_result_conformance.cpp)
  target_compile_features(sbsql_warning_partial_result_conformance PRIVATE cxx_std_23)
  target_link_libraries(sbsql_warning_partial_result_conformance PRIVATE sb_server_core)
  add_test(
    NAME sbsql_warning_partial_result_conformance
    COMMAND sbsql_warning_partial_result_conformance
  )
  set_tests_properties(sbsql_warning_partial_result_conformance PROPERTIES
    LABELS "sbsql_warning_partial_result_conformance;sbsql_parser_worker"
  )
endif()

if(TARGET sb_server_core)
  add_executable(sbsql_stream_finality_conformance sbsql_stream_finality_conformance.cpp)
  target_compile_features(sbsql_stream_finality_conformance PRIVATE cxx_std_23)
  target_link_libraries(sbsql_stream_finality_conformance PRIVATE sb_server_core)
  add_test(
    NAME sbsql_stream_finality_conformance
    COMMAND sbsql_stream_finality_conformance
  )
  set_tests_properties(sbsql_stream_finality_conformance PROPERTIES
    LABELS "sbsql_stream_finality_conformance;sbsql_parser_worker;database_lifecycle_parser;database_lifecycle;parser_lifecycle;sbwp_wire_gate;routing_reconnect_finality_gate;mga_transaction_regression"
  )
endif()

if(TARGET sb_engine_sblr AND TARGET sb_server_core)
  add_executable(sbsql_persistence_restart_conformance sbsql_persistence_restart_conformance.cpp)
  target_compile_features(sbsql_persistence_restart_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    sbsql_persistence_restart_conformance
    PRIVATE
      SB_FSP011D_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
  )
  target_link_libraries(sbsql_persistence_restart_conformance PRIVATE sb_engine_sblr sb_server_core)
  add_test(
    NAME sbsql_persistence_restart_conformance
    COMMAND sbsql_persistence_restart_conformance
  )
  set_tests_properties(sbsql_persistence_restart_conformance PROPERTIES
    LABELS "sbsql_persistence_restart_conformance;sbsql_parser_worker"
  )
endif()

if(TARGET sb_engine_sblr AND TARGET sb_server_core AND TARGET sbl_sbsql_parser_pipeline AND TARGET sb_transaction_mga)
  add_executable(sbsql_concurrent_session_transaction_conformance sbsql_concurrent_session_transaction_conformance.cpp)
  target_compile_features(sbsql_concurrent_session_transaction_conformance PRIVATE cxx_std_23)
  target_compile_definitions(
    sbsql_concurrent_session_transaction_conformance
    PRIVATE
      SB_FSP011E_SEED_PACK_ROOT="${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
  )
  target_link_libraries(
    sbsql_concurrent_session_transaction_conformance
    PRIVATE
      sb_engine_sblr
      sb_server_core
      sbl_sbsql_parser_pipeline
      sb_transaction_mga
  )
  add_test(
    NAME sbsql_concurrent_session_transaction_conformance
    COMMAND sbsql_concurrent_session_transaction_conformance
  )
  set_tests_properties(sbsql_concurrent_session_transaction_conformance PROPERTIES
    LABELS "sbsql_concurrent_session_transaction_conformance;cdp_concurrency_transaction_stress_gate;CDP-032;mga;transaction;concurrency;sbsql_parser_worker;mga_transaction_regression"
  )
  add_test(
    NAME cdp_concurrency_transaction_stress_gate
    COMMAND sbsql_concurrent_session_transaction_conformance
  )
  set_tests_properties(cdp_concurrency_transaction_stress_gate PROPERTIES
    LABELS "cdp_concurrency_transaction_stress_gate;CDP-032;CDP_P3_CONCURRENCY_TRANSACTION_READY;mga;transaction;concurrency;sbsql_parser_worker;mga_transaction_regression"
  )
  add_test(
    NAME sbsql_concurrency_isolation_deadlock_cancellation_conformance
    COMMAND sbsql_concurrent_session_transaction_conformance
  )
  set_tests_properties(sbsql_concurrency_isolation_deadlock_cancellation_conformance PROPERTIES
    LABELS "sbsql_concurrency_isolation_deadlock_cancellation_conformance;cdp_concurrency_transaction_stress_gate;CDP-032;CBQ-038;CBQ_GATE_CONCURRENCY_ISOLATION_DEADLOCK;sbsql_parser_worker;mga_transaction_regression"
  )
endif()

if(TARGET sbl_sbsql_parser_pipeline AND TARGET sbu_sbsql_parser_support AND TARGET sb_server_core AND TARGET sb_engine)
  add_executable(
    sb_message_vector_error_surface_conformance
    generated/diagnostics/sbsql_message_vector_error_surface_conformance.cpp
  )
  target_compile_features(sb_message_vector_error_surface_conformance PRIVATE cxx_std_23)
  target_link_libraries(
    sb_message_vector_error_surface_conformance
    PRIVATE
      sbl_sbsql_parser_pipeline
      sbu_sbsql_parser_support
      sb_server_core
      sb_engine
  )
  add_test(
    NAME sb_message_vector_error_surface_conformance
    COMMAND sb_message_vector_error_surface_conformance "${SBSQL_REGRESSION_ARTIFACT_ROOT}"
  )
  set_tests_properties(sb_message_vector_error_surface_conformance PROPERTIES
    LABELS "sb_message_vector_error_surface_conformance;sbsql_no_raw_string_diagnostic_gate;parser_diagnostics_security_gate;sbsql_parser_worker"
  )
endif()

if(TARGET sbu_sbsql_parser_support)
  add_executable(sbu_sbsql_parser_support_probe sbu_sbsql_parser_support_probe.cpp)
  target_compile_features(sbu_sbsql_parser_support_probe PRIVATE cxx_std_23)
  target_link_libraries(sbu_sbsql_parser_support_probe PRIVATE sbu_sbsql_parser_support)
  add_test(NAME sbu_sbsql_parser_support_probe COMMAND sbu_sbsql_parser_support_probe)
  set_tests_properties(sbu_sbsql_parser_support_probe PROPERTIES
    LABELS "sbu_sbsql_parser_support_conformance;trusted_udr_bridge_gate;parser_v3_closure_gate;sbsql_parser_worker"
  )
endif()

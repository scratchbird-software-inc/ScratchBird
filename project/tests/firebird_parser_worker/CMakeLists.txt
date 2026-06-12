# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(firebird_parser_pipeline_probe firebird_parser_pipeline_probe.cpp)
target_compile_features(firebird_parser_pipeline_probe PRIVATE cxx_std_23)
target_link_libraries(firebird_parser_pipeline_probe PRIVATE sbl_firebird_parser_pipeline)
add_test(NAME firebird_parser_pipeline_probe COMMAND firebird_parser_pipeline_probe)
set_tests_properties(firebird_parser_pipeline_probe PROPERTIES
  LABELS "firebird_lexer_conformance;firebird_cst_ast_conformance;firebird_datatype_descriptor_conformance;firebird_expression_builtin_conformance;firebird_dml_query_conformance;firebird_ddl_catalog_overlay_conformance;firebird_psql_dynamic_sql_conformance;firebird_non_file_emulation_gate;firebird_status_vector_diagnostic_conformance;firebird_parser_worker"
)

add_executable(firebird_binder_sblr_probe firebird_binder_sblr_probe.cpp)
target_compile_features(firebird_binder_sblr_probe PRIVATE cxx_std_23)
target_link_libraries(firebird_binder_sblr_probe PRIVATE sbl_firebird_parser_pipeline)
add_test(NAME firebird_binder_sblr_probe COMMAND firebird_binder_sblr_probe)
set_tests_properties(firebird_binder_sblr_probe PROPERTIES
  LABELS "firebird_binder_descriptor_uuid_conformance;firebird_sblr_lowering_verifier;firebird_parser_worker"
)

add_executable(firebird_isql_original_regression_probe firebird_isql_original_regression_probe.cpp)
target_compile_features(firebird_isql_original_regression_probe PRIVATE cxx_std_23)
target_link_libraries(firebird_isql_original_regression_probe PRIVATE sbl_firebird_parser_pipeline)

add_executable(firebird_service_tool_regression_probe firebird_service_tool_regression_probe.cpp)
target_compile_features(firebird_service_tool_regression_probe PRIVATE cxx_std_23)
target_link_libraries(firebird_service_tool_regression_probe PRIVATE sbl_firebird_parser_pipeline)

add_executable(firebird_wire_api_scope_probe firebird_wire_api_scope_probe.cpp)
target_compile_features(firebird_wire_api_scope_probe PRIVATE cxx_std_23)
target_link_libraries(firebird_wire_api_scope_probe PRIVATE sbl_firebird_parser_pipeline)
add_test(NAME firebird_wire_api_scope_probe COMMAND firebird_wire_api_scope_probe)
set_tests_properties(firebird_wire_api_scope_probe PROPERTIES
  LABELS "firebird_wire_api_scope_gate;firebird_bridge_service_surface_conformance;firebird_blr_parameter_buffer_conformance;firebird_parser_worker"
)

add_executable(firebird_parameter_buffer_decoder_probe firebird_parameter_buffer_decoder_probe.cpp)
target_compile_features(firebird_parameter_buffer_decoder_probe PRIVATE cxx_std_23)
target_link_libraries(firebird_parameter_buffer_decoder_probe PRIVATE sbl_firebird_parser_pipeline)
add_test(NAME firebird_parameter_buffer_decoder_probe COMMAND firebird_parameter_buffer_decoder_probe)
set_tests_properties(firebird_parameter_buffer_decoder_probe PROPERTIES
  LABELS "firebird_blr_parameter_buffer_conformance;firebird_bridge_service_surface_conformance;firebird_malicious_input_fuzz_gate;firebird_parser_worker"
)

find_program(FIREBIRD_PYTHON3 python3 REQUIRED)
add_test(
  NAME firebird_parameter_buffer_cli_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_parameter_buffer_cli_gate.py"
          --parser-worker $<TARGET_FILE:sbp_firebird>
)
set_tests_properties(firebird_parameter_buffer_cli_gate PROPERTIES
  LABELS "firebird_parser_worker_ipc_conformance;firebird_blr_parameter_buffer_conformance;firebird_bridge_service_surface_conformance;firebird_malicious_input_fuzz_gate;firebird_parser_worker"
)

add_executable(firebird_package_boundary_probe firebird_package_boundary_probe.cpp)
target_compile_features(firebird_package_boundary_probe PRIVATE cxx_std_23)
target_link_libraries(firebird_package_boundary_probe PRIVATE sbl_firebird_parser_pipeline)
add_test(
  NAME firebird_package_boundary_probe
  COMMAND firebird_package_boundary_probe "${PROJECT_SOURCE_DIR}"
)
set_tests_properties(firebird_package_boundary_probe PROPERTIES
  LABELS "firebird_package_boundary_gate;firebird_parser_worker"
)

add_executable(firebird_runtime_absence_probe firebird_runtime_absence_probe.cpp)
target_compile_features(firebird_runtime_absence_probe PRIVATE cxx_std_23)
target_link_libraries(firebird_runtime_absence_probe PRIVATE sbl_firebird_parser_pipeline)
add_test(NAME firebird_runtime_absence_gate COMMAND firebird_runtime_absence_probe)
set_tests_properties(firebird_runtime_absence_gate PROPERTIES
  LABELS "firebird_runtime_absence_gate;firebird_package_boundary_gate;firebird_parser_worker"
)

add_executable(firebird_listener_worker_probe firebird_listener_worker_probe.cpp)
target_compile_features(firebird_listener_worker_probe PRIVATE cxx_std_23)
target_link_libraries(firebird_listener_worker_probe PRIVATE sbl_listener_control_plane)
add_test(
  NAME firebird_listener_worker_probe
  COMMAND firebird_listener_worker_probe $<TARGET_FILE:sbp_firebird>
)
set_tests_properties(firebird_listener_worker_probe PROPERTIES
  LABELS "firebird_parser_worker_ipc_conformance;firebird_runtime_absence_gate;firebird_parser_worker"
)

add_executable(firebird_worker_session_probe firebird_worker_session_probe.cpp)
target_compile_features(firebird_worker_session_probe PRIVATE cxx_std_23)
target_link_libraries(firebird_worker_session_probe PRIVATE sbl_firebird_parser_pipeline)
add_test(NAME firebird_worker_session_probe COMMAND firebird_worker_session_probe)
set_tests_properties(firebird_worker_session_probe PROPERTIES
  LABELS "firebird_parser_worker_ipc_conformance;firebird_wire_api_scope_gate;firebird_bridge_service_surface_conformance;firebird_parser_worker"
)

if(TARGET sbu_firebird_parser_support)
  add_executable(sbu_firebird_parser_support_probe sbu_firebird_parser_support_probe.cpp)
  target_compile_features(sbu_firebird_parser_support_probe PRIVATE cxx_std_23)
  target_link_libraries(
    sbu_firebird_parser_support_probe
    PRIVATE
      sbu_firebird_parser_support
  )
  add_test(NAME sbu_firebird_parser_support_probe COMMAND sbu_firebird_parser_support_probe)
  set_tests_properties(sbu_firebird_parser_support_probe PROPERTIES
    LABELS "firebird_status_vector_diagnostic_conformance;firebird_udr_dynamic_sql_conformance;firebird_udr_environment_installer_conformance;firebird_universal_bridge_udr_conformance;firebird_parser_worker"
  )

  add_executable(firebird_upgrade_install_drift_probe firebird_upgrade_install_drift_probe.cpp)
  target_compile_features(firebird_upgrade_install_drift_probe PRIVATE cxx_std_23)
  target_link_libraries(
    firebird_upgrade_install_drift_probe
    PRIVATE
      sbu_firebird_parser_support
  )
  add_test(NAME firebird_upgrade_install_drift_gate COMMAND firebird_upgrade_install_drift_probe)
  set_tests_properties(firebird_upgrade_install_drift_gate PROPERTIES
    LABELS "firebird_upgrade_install_drift_gate;firebird_udr_environment_installer_conformance;firebird_parser_worker"
  )
endif()

add_executable(firebird_malicious_input_fuzz_probe firebird_malicious_input_fuzz_probe.cpp)
target_compile_features(firebird_malicious_input_fuzz_probe PRIVATE cxx_std_23)
target_link_libraries(firebird_malicious_input_fuzz_probe PRIVATE sbl_firebird_parser_pipeline)
add_test(NAME firebird_malicious_input_fuzz_gate COMMAND firebird_malicious_input_fuzz_probe)
set_tests_properties(firebird_malicious_input_fuzz_gate PROPERTIES
  LABELS "firebird_malicious_input_fuzz_gate;firebird_parser_worker"
)

set(FIREBIRD_RUNTIME_ISOLATION_BINARIES
  $<TARGET_FILE:sbp_firebird>
  $<TARGET_FILE:firebird_listener_worker_probe>
)
if(TARGET sbu_firebird_parser_support_probe)
  list(APPEND FIREBIRD_RUNTIME_ISOLATION_BINARIES
    $<TARGET_FILE:sbu_firebird_parser_support_probe>
  )
endif()
add_test(
  NAME firebird_tool_runtime_isolation_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_runtime_isolation_gate.py"
          --source-root "${PROJECT_SOURCE_DIR}/src/parsers/compatibility/firebird"
          --source-root "${PROJECT_SOURCE_DIR}/src/udr/sbu_firebird_parser_support"
          --binary ${FIREBIRD_RUNTIME_ISOLATION_BINARIES}
)
set_tests_properties(firebird_tool_runtime_isolation_gate PROPERTIES
  LABELS "firebird_tool_runtime_isolation_gate;firebird_package_boundary_gate;firebird_parser_worker"
)

set(FIREBIRD_SURFACE_FIXTURE_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/full_firebirdsql_parser_udr_emulation_closure"
)
add_test(
  NAME firebird_clean_room_provenance_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_clean_room_provenance_gate.py"
          --policy "${FIREBIRD_SURFACE_FIXTURE_ROOT}/artifacts/CLEAN_ROOM_PROVENANCE_POLICY.md"
          --audit "${FIREBIRD_SURFACE_FIXTURE_ROOT}/artifacts/CLEAN_ROOM_SOURCE_USE_AUDIT.md"
          --source-root "${PROJECT_SOURCE_DIR}/src/parsers/compatibility/firebird"
          --source-root "${PROJECT_SOURCE_DIR}/src/udr/sbu_firebird_parser_support"
          --binary ${FIREBIRD_RUNTIME_ISOLATION_BINARIES}
)
set_tests_properties(firebird_clean_room_provenance_gate PROPERTIES
  LABELS "firebird_clean_room_provenance_gate;firebird_package_boundary_gate;firebird_parser_worker"
)

add_test(
  NAME firebird_execution_plan_artifact_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_execution_plan_artifact_gate.py"
          --fixture-root "${FIREBIRD_SURFACE_FIXTURE_ROOT}"
)
set_tests_properties(firebird_execution_plan_artifact_gate PROPERTIES
  LABELS "firebird_surface_registry_lint;firebird_agent_orchestration_gate;firebird_parser_worker"
)

set(FIREBIRD_REGRESSION_EVIDENCE_ROOT
  "${PROJECT_SOURCE_DIR}/../project/tests/reference_regression/reference_release_acquisition/firebird/5.0.4/regression"
)
add_test(
  NAME firebird_qa_candidate_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_qa_candidate_gate.py"
          --regression-root "${FIREBIRD_REGRESSION_EVIDENCE_ROOT}"
)
set_tests_properties(firebird_qa_candidate_gate PROPERTIES
  LABELS "firebird_original_regression_replay_gate;firebird_reference_native;firebird_parser_worker"
)

add_test(
  NAME firebird_qa_replay_manifest_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_qa_replay_manifest_gate.py"
          --candidate-index "${FIREBIRD_REGRESSION_EVIDENCE_ROOT}/FIREBIRD_QA_CANDIDATE_TEST_INDEX.csv"
          --replay-manifest "${FIREBIRD_REGRESSION_EVIDENCE_ROOT}/FIREBIRD_QA_REFERENCE_REPLAY_MANIFEST.csv"
          --family-manifest "${FIREBIRD_REGRESSION_EVIDENCE_ROOT}/FIREBIRD_QA_REFERENCE_REPLAY_FAMILY_MANIFEST.csv"
          --asset-manifest "${FIREBIRD_REGRESSION_EVIDENCE_ROOT}/FIREBIRD_QA_CANDIDATE_ASSET_HASH_MANIFEST.csv"
)
set_tests_properties(firebird_qa_replay_manifest_gate PROPERTIES
  LABELS "firebird_original_regression_replay_gate;firebird_reference_native;firebird_parser_worker"
)

add_test(
  NAME firebird_catalog_rowset_hash_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_catalog_rowset_hash_gate.py"
          --catalog-inventory "${PROJECT_SOURCE_DIR}/../project/tests/reference_regression/reference_catalog_seeds/firebird/firebird_5_catalog_inventory.csv"
          --builtin-inventory "${PROJECT_SOURCE_DIR}/../project/tests/reference_regression/reference_catalog_seeds/firebird/firebird_5_builtin_inventory.csv"
          --extraction-status "${PROJECT_SOURCE_DIR}/../project/tests/reference_regression/reference_catalog_seeds/firebird/firebird_5_exact_rowset_extraction_status.md"
          --output-dir "${CMAKE_CURRENT_BINARY_DIR}/firebird_catalog_rowset_hash_gate"
)
set_tests_properties(firebird_catalog_rowset_hash_gate PROPERTIES
  LABELS "firebird_ddl_catalog_overlay_conformance;firebird_expression_builtin_conformance;firebird_reference_native;firebird_parser_worker"
)

add_test(
  NAME firebird_version_profile_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_version_profile_gate.py"
          --matrix "${FIREBIRD_SURFACE_FIXTURE_ROOT}/artifacts/FIREBIRD_VERSION_PROFILE_MATRIX.csv"
)
set_tests_properties(firebird_version_profile_gate PROPERTIES
  LABELS "firebird_version_profile_gate;firebird_parser_worker"
)

set(FIREBIRD_REFERENCE_BUILD_ROOT
  "${PROJECT_SOURCE_DIR}/../build/reference/firebird-5.0.4-release-src"
)
set(FIREBIRD_REFERENCE_TOOL_MANIFEST
  "${FIREBIRD_SURFACE_FIXTURE_ROOT}/artifacts/FIREBIRD_REFERENCE_TOOL_BUILD_MANIFEST.csv"
)
add_test(
  NAME firebird_reference_tool_build_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_reference_tool_gate.py"
          --mode build
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --reference-root "${FIREBIRD_REFERENCE_BUILD_ROOT}"
          --manifest "${FIREBIRD_REFERENCE_TOOL_MANIFEST}"
          --build-if-missing
          --jobs 2
)
set_tests_properties(firebird_reference_tool_build_gate PROPERTIES
  LABELS "firebird_reference_tool_build_gate;firebird_reference_native;firebird_parser_worker"
  TIMEOUT 1800
)

add_test(
  NAME firebird_reference_tool_sandbox_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_reference_tool_gate.py"
          --mode sandbox
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --reference-root "${FIREBIRD_REFERENCE_BUILD_ROOT}"
          --manifest "${FIREBIRD_REFERENCE_TOOL_MANIFEST}"
)
set_tests_properties(firebird_reference_tool_sandbox_gate PROPERTIES
  DEPENDS firebird_reference_tool_build_gate
  LABELS "firebird_reference_tool_sandbox_gate;firebird_reference_native;firebird_parser_worker"
  TIMEOUT 120
)

add_test(
  NAME firebird_reference_native_result_normalization_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_reference_native_result_normalization_gate.py"
          --policy "${FIREBIRD_SURFACE_FIXTURE_ROOT}/artifacts/REFERENCE_NATIVE_RESULT_NORMALIZATION_POLICY.md"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(firebird_reference_native_result_normalization_gate PROPERTIES
  LABELS "firebird_reference_native_result_normalization_gate;firebird_reference_native;firebird_parser_worker"
)

add_test(
  NAME firebird_operational_failure_inventory_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_operational_failure_inventory_gate.py"
          --policy "${FIREBIRD_SURFACE_FIXTURE_ROOT}/artifacts/OPERATIONAL_FAILURE_INVENTORY_POLICY.md"
)
set_tests_properties(firebird_operational_failure_inventory_gate PROPERTIES
  LABELS "firebird_operational_failure_inventory_gate;firebird_reference_native;firebird_parser_worker"
)

set(FIREBIRD_QA_CANDIDATE_ROOT
  "${PROJECT_SOURCE_DIR}/../../firebird-qa"
)
add_test(
  NAME firebird_isql_original_regression_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_isql_original_regression_gate.py"
          --candidate-root "${FIREBIRD_QA_CANDIDATE_ROOT}"
          --replay-manifest "${FIREBIRD_REGRESSION_EVIDENCE_ROOT}/FIREBIRD_QA_REFERENCE_REPLAY_MANIFEST.csv"
          --firebird-home "${FIREBIRD_REFERENCE_BUILD_ROOT}/gen/Release/firebird"
          --parser-probe $<TARGET_FILE:firebird_isql_original_regression_probe>
          --output-dir "${CMAKE_CURRENT_BINARY_DIR}/firebird_isql_original_regression_gate"
          --sample-count 22
)
set_tests_properties(firebird_isql_original_regression_gate PROPERTIES
  DEPENDS firebird_reference_tool_build_gate
  LABELS "firebird_isql_original_regression_gate;firebird_original_regression_replay_gate;firebird_reference_native;firebird_parser_worker"
  TIMEOUT 120
)

add_test(
  NAME fbqa_full_original_regression_inventory_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_full_original_regression_replay_gate.py"
          --candidate-root "${FIREBIRD_QA_CANDIDATE_ROOT}"
          --replay-manifest "${FIREBIRD_REGRESSION_EVIDENCE_ROOT}/FIREBIRD_QA_REFERENCE_REPLAY_MANIFEST.csv"
          --firebird-home "${FIREBIRD_REFERENCE_BUILD_ROOT}/gen/Release/firebird"
          --parser-probe $<TARGET_FILE:firebird_isql_original_regression_probe>
          --output-dir "${CMAKE_CURRENT_BINARY_DIR}/fbqa_full_original_regression_inventory_gate"
          --ctest-name fbqa_full_original_regression_inventory_gate
          --mode inventory
)
set_tests_properties(fbqa_full_original_regression_inventory_gate PROPERTIES
  DEPENDS firebird_reference_tool_build_gate
  LABELS "fbqa_full_original_regression_inventory_gate;firebird_original_regression_replay_gate;firebird_reference_native;firebird_parser_worker"
  RESOURCE_LOCK firebird_reference_native
  RUN_SERIAL TRUE
  TIMEOUT 1800
)

add_test(
  NAME fbqa_full_original_regression_final_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_full_original_regression_replay_gate.py"
          --candidate-root "${FIREBIRD_QA_CANDIDATE_ROOT}"
          --replay-manifest "${FIREBIRD_REGRESSION_EVIDENCE_ROOT}/FIREBIRD_QA_REFERENCE_REPLAY_MANIFEST.csv"
          --firebird-home "${FIREBIRD_REFERENCE_BUILD_ROOT}/gen/Release/firebird"
          --parser-probe $<TARGET_FILE:firebird_isql_original_regression_probe>
          --output-dir "${CMAKE_CURRENT_BINARY_DIR}/fbqa_full_original_regression_final_gate"
          --ctest-name fbqa_full_original_regression_final_gate
          --mode final
)
set_tests_properties(fbqa_full_original_regression_final_gate PROPERTIES
  DEPENDS firebird_reference_tool_build_gate
  LABELS "fbqa_full_original_regression_final_gate;firebird_original_regression_replay_gate;firebird_reference_native;firebird_parser_worker"
  RESOURCE_LOCK firebird_reference_native
  RUN_SERIAL TRUE
  TIMEOUT 1800
)

add_test(
  NAME firebird_service_tool_regression_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_service_tool_regression_gate.py"
          --manifest "${FIREBIRD_REFERENCE_TOOL_MANIFEST}"
          --firebird-home "${FIREBIRD_REFERENCE_BUILD_ROOT}/gen/Release/firebird"
          --parser-probe $<TARGET_FILE:firebird_service_tool_regression_probe>
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --output-dir "${CMAKE_CURRENT_BINARY_DIR}/firebird_service_tool_regression_gate"
)
set_tests_properties(firebird_service_tool_regression_gate PROPERTIES
  DEPENDS firebird_reference_tool_build_gate
  LABELS "firebird_service_tool_regression_gate;firebird_reference_native;firebird_parser_worker"
  TIMEOUT 120
)

add_test(
  NAME firebird_reference_tool_diff_oracle_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_reference_tool_diff_oracle_gate.py"
          --repo-root "${PROJECT_SOURCE_DIR}/.."
          --output-dir "${CMAKE_CURRENT_BINARY_DIR}/firebird_reference_tool_diff_oracle_gate"
)
set_tests_properties(firebird_reference_tool_diff_oracle_gate PROPERTIES
  LABELS "firebird_reference_tool_diff_oracle_gate;firebird_reference_native;firebird_parser_worker"
)

add_test(
  NAME firebird_exhaustive_e2e_regression_gate
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_final_surface_audit_gate.py"
          --fixture-root "${FIREBIRD_SURFACE_FIXTURE_ROOT}"
          --mode exhaustive
)
set_tests_properties(firebird_exhaustive_e2e_regression_gate PROPERTIES
  LABELS "firebird_exhaustive_e2e_regression_gate;firebird_parser_worker"
)

add_test(
  NAME firebird_final_zero_open_surface_audit
  COMMAND "${FIREBIRD_PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/firebird_final_surface_audit_gate.py"
          --fixture-root "${FIREBIRD_SURFACE_FIXTURE_ROOT}"
          --mode final
)
set_tests_properties(firebird_final_zero_open_surface_audit PROPERTIES
  LABELS "firebird_final_zero_open_surface_audit;firebird_parser_worker"
)

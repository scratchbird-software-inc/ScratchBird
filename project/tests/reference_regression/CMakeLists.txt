# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.25)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SB_REFERENCE_COMPATIBILITY_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/reference_compatibility_gate.py"
)
set(SB_REFERENCE_PARSER_START_EVIDENCE_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/generate_reference_parser_start_evidence.py"
)
set(SB_REFERENCE_TEST_TREE_SELF_CONTAINED_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/reference_test_tree_self_contained_gate.py"
)
set(SB_REFERENCE_PUBLIC_PAYLOAD_POLICY_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/reference_public_payload_policy_gate.py"
)
set(SB_REFERENCE_NATIVE_TOOL_HARNESS_CONTRACT_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/reference_native_tool_harness_contract_gate.py"
)
set(SB_REFERENCE_ORIGINAL_REPLAY_PLAN_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/reference_original_replay_plan_gate.py"
)
set(SB_REFERENCE_REPLAY_RUNTIME_SMOKE
  "${CMAKE_CURRENT_SOURCE_DIR}/reference_replay_runtime_smoke.py"
)
set(SB_REFERENCE_ORIGINAL_NATIVE_REPLAY_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/reference_original_native_replay_gate.py"
)
set(SB_REFERENCE_ORIGINAL_CANONICAL_DATABASE_REPLAY_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/reference_original_canonical_database_replay_gate.py"
)
set(SB_REFERENCE_POSTGRESQL_REGRESS_REPLAY_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/reference_postgresql_regress_replay_gate.py"
)
set(SB_REFERENCE_RESULT_NORMALIZATION_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/reference_result_normalization_gate.py"
)
set(SB_REFERENCE_REPLAY_POLICY_MATRIX_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/reference_replay_policy_matrix_gate.py"
)
set(SB_REFERENCE_SUITE_SCOPE_DRIFT_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/reference_suite_scope_drift_gate.py"
)
set(SB_REFERENCE_RELEASE_COMPLETION_BLOCKER_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/reference_release_completion_blocker_gate.py"
)
set(SB_COMPATIBILITY_PARSER_DOCS_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/generate_compatibility_parser_docs.py"
)
set(SB_REFERENCE_FIREBIRD_QA_CTEST_TIMEOUT "43200" CACHE STRING
  "CTest timeout in seconds for the full original Firebird QA replay gate"
)

add_test(
  NAME reference_firebird_qa_replay_scope_gate_unit
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/reference_firebird_qa_replay_gate_test.py"
)
set_tests_properties(reference_firebird_qa_replay_scope_gate_unit PROPERTIES
  ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}"
  LABELS "reference_original_regression_gate;reference_firebird_qa_replay_gate;reference_suite_scope_gate"
)

add_test(
  NAME reference_firebird_qa_shard_aggregate_unit
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/reference_firebird_qa_shard_aggregate_test.py"
)
set_tests_properties(reference_firebird_qa_shard_aggregate_unit PROPERTIES
  ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}"
  LABELS "reference_original_regression_gate;reference_firebird_qa_replay_gate;reference_suite_scope_gate"
)

function(sb_add_reference_gate test_name mode labels)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_REFERENCE_COMPATIBILITY_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "${mode}"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "${labels};reference_compatibility;${test_name}"
  )
endfunction()

function(sb_add_reference_policy_matrix_gate test_name matrix labels)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${SB_REFERENCE_REPLAY_POLICY_MATRIX_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--matrix" "${matrix}"
      "--evidence-file"
      "${CMAKE_BINARY_DIR}/tests/reference_regression/${test_name}_evidence.json"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "${labels};reference_original_regression_gate;reference_release_regression_gate;mga_transaction_regression"
  )
endfunction()

function(sb_add_reference_native_replay_family family parser_target)
  if(TARGET sb_server AND TARGET sb_listener AND TARGET "${parser_target}")
    set(native_test_name "reference_original_native_replay_${family}_conformance")
    set(normalize_test_name "reference_result_normalization_${family}_conformance")
    add_test(
      NAME "${native_test_name}"
      COMMAND
        "${Python3_EXECUTABLE}"
        "${SB_REFERENCE_ORIGINAL_NATIVE_REPLAY_GATE}"
        "--repo-root" "${PROJECT_SOURCE_DIR}/.."
        "--server" "$<TARGET_FILE:sb_server>"
        "--listener" "$<TARGET_FILE:sb_listener>"
        "--parser-worker" "$<TARGET_FILE:${parser_target}>"
        "--family" "${family}"
        "--work-root" "/tmp/sbrpr-native-replay-${family}"
        "--evidence-file" "${CMAKE_BINARY_DIR}/tests/reference_regression/${native_test_name}_evidence.json"
    )
    set_tests_properties("${native_test_name}" PROPERTIES
      LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_original_tool_runner_gate;reference_replay_result_normalization_gate;reference_original_native_replay_gate;mga_transaction_regression"
      SKIP_RETURN_CODE 77
    )

    add_test(
      NAME "${normalize_test_name}"
      COMMAND
        "${Python3_EXECUTABLE}"
        "${SB_REFERENCE_RESULT_NORMALIZATION_GATE}"
        "--evidence-file" "${CMAKE_BINARY_DIR}/tests/reference_regression/${native_test_name}_evidence.json"
        "--output-file" "${CMAKE_BINARY_DIR}/tests/reference_regression/${normalize_test_name}_evidence.json"
    )
    set_tests_properties("${normalize_test_name}" PROPERTIES
      LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_replay_result_normalization_gate;reference_result_canonicalization_gate;mga_transaction_regression"
      DEPENDS "${native_test_name}"
    )
  endif()
endfunction()

sb_add_reference_gate(
  reference_core_framework_conformance
  core
  "reference_core_framework_gate;reference_seed_gate;reference_datatype_gate;reference_index_translation_gate;reference_diagnostic_rendering_gate;reference_metadata_overlay_gate;reference_migration_cdc_gate;reference_sandbox_bridge_gate;reference_release_regression_gate"
)
sb_add_reference_gate(
  reference_seed_manifest_conformance
  seed
  "reference_seed_gate;reference_core_framework_gate"
)
sb_add_reference_gate(
  reference_surface_mapping_conformance
  surface
  "reference_datatype_gate;reference_index_translation_gate;reference_diagnostic_rendering_gate;reference_metadata_overlay_gate"
)
sb_add_reference_gate(
  reference_runtime_bridge_release_conformance
  runtime
  "reference_migration_cdc_gate;reference_sandbox_bridge_gate;reference_release_regression_gate;reference_original_regression_gate"
)
sb_add_reference_gate(
  reference_relational_family_conformance
  relational
  "reference_relational_family_gate;reference_original_regression_gate"
)
sb_add_reference_gate(
  reference_analytic_family_conformance
  analytic
  "reference_analytic_family_gate;reference_original_regression_gate"
)
sb_add_reference_gate(
  reference_nosql_family_conformance
  nosql
  "reference_nosql_family_gate;reference_original_regression_gate"
)
sb_add_reference_gate(
  reference_distributed_family_conformance
  distributed
  "reference_distributed_family_gate;reference_original_regression_gate"
)
sb_add_reference_gate(
  reference_capability_reference_conformance
  capref
  "reference_capability_reference_gate"
)
sb_add_reference_gate(
  reference_original_regression_import_conformance
  original-regression
  "reference_original_regression_gate;reference_release_regression_gate"
)

add_test(
  NAME reference_test_tree_self_contained_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${SB_REFERENCE_TEST_TREE_SELF_CONTAINED_GATE}"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(reference_test_tree_self_contained_conformance PROPERTIES
  LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_test_asset_gate"
)

add_test(
  NAME reference_public_payload_policy_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${SB_REFERENCE_PUBLIC_PAYLOAD_POLICY_GATE}"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
    "--evidence-file"
    "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_public_payload_policy_evidence.json"
)
set_tests_properties(reference_public_payload_policy_conformance PROPERTIES
  LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_test_asset_gate;public_release_payload_gate;reference_public_payload_scanner_policy_gate;reference_secret_redaction_artifact_gate"
)

add_test(
  NAME reference_native_tool_harness_contract_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${SB_REFERENCE_NATIVE_TOOL_HARNESS_CONTRACT_GATE}"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
    "--evidence-file"
    "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_native_tool_harness_contract_evidence.json"
)
set_tests_properties(reference_native_tool_harness_contract_conformance PROPERTIES
  LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_native_tool_harness_contract_gate;reference_test_asset_gate;mga_transaction_regression"
)

add_test(
  NAME reference_original_replay_plan_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${SB_REFERENCE_ORIGINAL_REPLAY_PLAN_GATE}"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
    "--mode" "public-no-payload"
    "--evidence-file"
    "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_original_replay_plan_evidence.json"
)
set_tests_properties(reference_original_replay_plan_conformance PROPERTIES
  LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_original_tool_suite_map_gate;reference_replay_execution_order_gate;reference_ctest_mode_split_gate;reference_test_asset_gate;mga_transaction_regression"
)

if(TARGET sb_server AND TARGET sb_listener AND TARGET sbp_firebird)
  add_test(
    NAME reference_replay_runtime_smoke_firebird_conformance
    COMMAND
      "${Python3_EXECUTABLE}"
      "${SB_REFERENCE_REPLAY_RUNTIME_SMOKE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--server" "$<TARGET_FILE:sb_server>"
      "--listener" "$<TARGET_FILE:sb_listener>"
      "--parser-worker" "$<TARGET_FILE:sbp_firebird>"
      "--family" "firebird"
      "--work-root" "/tmp/sbrpr-runtime-smoke"
      "--evidence-file" "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_replay_runtime_smoke_firebird_evidence.json"
  )
set_tests_properties(reference_replay_runtime_smoke_firebird_conformance PROPERTIES
    LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_replay_database_lifecycle_gate;reference_replay_listener_lifecycle_gate;reference_runtime_smoke_gate;mga_transaction_regression"
  )

  add_test(
    NAME reference_original_native_replay_firebird_conformance
    COMMAND
      "${Python3_EXECUTABLE}"
      "${SB_REFERENCE_ORIGINAL_NATIVE_REPLAY_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--server" "$<TARGET_FILE:sb_server>"
      "--listener" "$<TARGET_FILE:sb_listener>"
      "--parser-worker" "$<TARGET_FILE:sbp_firebird>"
      "--family" "firebird"
      "--work-root" "/tmp/sbrpr-native-replay"
      "--evidence-file" "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_original_native_replay_firebird_evidence.json"
  )
  set_tests_properties(reference_original_native_replay_firebird_conformance PROPERTIES
    LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_original_tool_runner_gate;reference_replay_result_normalization_gate;reference_original_native_replay_gate;mga_transaction_regression"
    SKIP_RETURN_CODE 77
  )

  add_test(
    NAME reference_result_normalization_firebird_conformance
    COMMAND
      "${Python3_EXECUTABLE}"
      "${SB_REFERENCE_RESULT_NORMALIZATION_GATE}"
      "--evidence-file" "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_original_native_replay_firebird_evidence.json"
      "--output-file" "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_result_normalization_firebird_evidence.json"
  )
  set_tests_properties(reference_result_normalization_firebird_conformance PROPERTIES
    LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_replay_result_normalization_gate;reference_result_canonicalization_gate;mga_transaction_regression"
    DEPENDS reference_original_native_replay_firebird_conformance
  )

  add_test(
    NAME reference_firebird_qa_original_suite_conformance
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/reference_firebird_qa_replay_gate.py"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--server" "$<TARGET_FILE:sb_server>"
      "--listener" "$<TARGET_FILE:sb_listener>"
      "--parser-worker" "$<TARGET_FILE:sbp_firebird>"
      "--family" "firebird"
      "--work-root" "/tmp/sbrpr-firebird-qa"
      "--evidence-file" "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_firebird_qa_original_suite_evidence.json"
      "--failure-ledger-file" "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_firebird_qa_failure_ledger.csv"
      "--tool-timeout" "1800"
  )
  set_tests_properties(reference_firebird_qa_original_suite_conformance PROPERTIES
    LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_original_tool_runner_gate;reference_replay_result_normalization_gate;reference_replay_failure_ledger_gate;reference_firebird_qa_replay_gate;mga_transaction_regression"
    SKIP_RETURN_CODE 77
    TIMEOUT "${SB_REFERENCE_FIREBIRD_QA_CTEST_TIMEOUT}"
  )

  add_test(
    NAME reference_firebird_qa_result_normalization_conformance
    COMMAND
      "${Python3_EXECUTABLE}"
      "${SB_REFERENCE_RESULT_NORMALIZATION_GATE}"
      "--evidence-file" "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_firebird_qa_original_suite_evidence.json"
      "--output-file" "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_firebird_qa_result_normalization_evidence.json"
  )
  set_tests_properties(reference_firebird_qa_result_normalization_conformance PROPERTIES
    LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_replay_result_normalization_gate;reference_result_canonicalization_gate;reference_firebird_qa_replay_gate;mga_transaction_regression"
    DEPENDS reference_firebird_qa_original_suite_conformance
  )
endif()

sb_add_reference_native_replay_family(apache_ignite sbp_apache_ignite)
sb_add_reference_native_replay_family(cassandra sbp_cassandra)
sb_add_reference_native_replay_family(clickhouse sbp_clickhouse)
sb_add_reference_native_replay_family(cockroachdb sbp_cockroachdb)
sb_add_reference_native_replay_family(dolt sbp_dolt)
sb_add_reference_native_replay_family(duckdb sbp_duckdb)
sb_add_reference_native_replay_family(foundationdb sbp_foundationdb)
sb_add_reference_native_replay_family(immudb sbp_immudb)
sb_add_reference_native_replay_family(influxdb sbp_influxdb)
sb_add_reference_native_replay_family(mariadb sbp_mariadb)
sb_add_reference_native_replay_family(milvus sbp_milvus)
sb_add_reference_native_replay_family(mongodb sbp_mongodb)
sb_add_reference_native_replay_family(mysql sbp_mysql)
sb_add_reference_native_replay_family(mysql_lts sbp_mysql)
sb_add_reference_native_replay_family(neo4j sbp_neo4j)
sb_add_reference_native_replay_family(opensearch sbp_opensearch)
sb_add_reference_native_replay_family(opensearch_sql_ppl sbp_opensearch_sql_ppl)
sb_add_reference_native_replay_family(postgresql sbp_postgresql)
sb_add_reference_native_replay_family(redis sbp_redis)
sb_add_reference_native_replay_family(sqlite sbp_sqlite)
sb_add_reference_native_replay_family(tidb sbp_tidb)
sb_add_reference_native_replay_family(tikv sbp_tikv)
sb_add_reference_native_replay_family(vitess sbp_vitess)
sb_add_reference_native_replay_family(xtdb sbp_xtdb)
sb_add_reference_native_replay_family(yugabytedb sbp_yugabytedb)

if(TARGET sb_server AND TARGET sb_listener AND TARGET sbp_postgresql)
  add_test(
    NAME reference_postgresql_pg_regress_original_suite_conformance
    COMMAND
      "${Python3_EXECUTABLE}"
      "${SB_REFERENCE_POSTGRESQL_REGRESS_REPLAY_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--server" "$<TARGET_FILE:sb_server>"
      "--listener" "$<TARGET_FILE:sb_listener>"
      "--parser-worker" "$<TARGET_FILE:sbp_postgresql>"
      "--family" "postgresql"
      "--work-root" "/tmp/sbrpr-postgresql-pg-regress"
      "--evidence-file" "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_postgresql_pg_regress_original_suite_evidence.json"
      "--failure-ledger-file" "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_postgresql_pg_regress_failure_ledger.csv"
      "--tool-timeout" "1800"
  )
  set_tests_properties(reference_postgresql_pg_regress_original_suite_conformance PROPERTIES
    LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_original_tool_runner_gate;reference_replay_result_normalization_gate;reference_replay_failure_ledger_gate;reference_postgresql_pg_regress_gate;mga_transaction_regression"
    SKIP_RETURN_CODE 77
    TIMEOUT 2100
  )

  add_test(
    NAME reference_postgresql_pg_regress_result_normalization_conformance
    COMMAND
      "${Python3_EXECUTABLE}"
      "${SB_REFERENCE_RESULT_NORMALIZATION_GATE}"
      "--evidence-file" "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_postgresql_pg_regress_original_suite_evidence.json"
      "--output-file" "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_postgresql_pg_regress_result_normalization_evidence.json"
  )
  set_tests_properties(reference_postgresql_pg_regress_result_normalization_conformance PROPERTIES
    LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_replay_result_normalization_gate;reference_result_canonicalization_gate;reference_postgresql_pg_regress_gate;mga_transaction_regression"
    DEPENDS reference_postgresql_pg_regress_original_suite_conformance
  )
endif()

if(TARGET sb_server AND TARGET sb_listener AND TARGET sbp_firebird)
  add_test(
    NAME reference_original_canonical_database_replay_conformance
    COMMAND
      "${Python3_EXECUTABLE}"
      "${SB_REFERENCE_ORIGINAL_CANONICAL_DATABASE_REPLAY_GATE}"
      "--repo-root" "${PROJECT_SOURCE_DIR}/.."
      "--server" "$<TARGET_FILE:sb_server>"
      "--listener" "$<TARGET_FILE:sb_listener>"
      "--parser-bin-root" "$<TARGET_FILE_DIR:sbp_firebird>"
      "--work-root" "/tmp/sbrpr-canonical-native-replay"
      "--evidence-file" "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_original_canonical_database_replay_evidence.json"
  )
  set_tests_properties(reference_original_canonical_database_replay_conformance PROPERTIES
    LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_original_tool_runner_gate;reference_replay_database_lifecycle_gate;reference_replay_listener_lifecycle_gate;reference_replay_database_contamination_gate;reference_replay_result_normalization_gate;mga_transaction_regression"
    SKIP_RETURN_CODE 77
  )
endif()

add_test(
  NAME reference_suite_scope_drift_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${SB_REFERENCE_SUITE_SCOPE_DRIFT_GATE}"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
    "--evidence-file"
    "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_suite_scope_drift_evidence.json"
)
set_tests_properties(reference_suite_scope_drift_conformance PROPERTIES
  LABELS "reference_original_regression_gate;reference_release_regression_gate;reference_suite_scope_classification_gate;reference_upstream_version_drift_gate;reference_original_tool_suite_map_gate;reference_test_asset_gate;mga_transaction_regression"
)

sb_add_reference_policy_matrix_gate(
  reference_replay_database_contamination_conformance
  database-contamination
  "reference_replay_database_contamination_gate;reference_replay_database_lifecycle_gate"
)
sb_add_reference_policy_matrix_gate(
  reference_original_tool_adapter_contract_conformance
  tool-adapter-contract
  "reference_original_tool_adapter_contract_gate;reference_original_tool_runner_gate"
)
sb_add_reference_policy_matrix_gate(
  reference_security_authorization_replay_conformance
  security-authorization
  "reference_security_authorization_replay_gate;reference_parser_route_authorization_gate"
)
sb_add_reference_policy_matrix_gate(
  reference_result_canonicalization_policy_conformance
  result-canonicalization
  "reference_result_canonicalization_gate;reference_replay_result_normalization_gate"
)
sb_add_reference_policy_matrix_gate(
  reference_retry_quarantine_conformance
  retry-quarantine
  "reference_retry_quarantine_gate;reference_replay_result_normalization_gate"
)
sb_add_reference_policy_matrix_gate(
  reference_resource_timeout_policy_conformance
  resource-timeout
  "reference_resource_timeout_policy_gate;reference_original_tool_runner_gate"
)
sb_add_reference_policy_matrix_gate(
  reference_failure_ledger_schema_conformance
  failure-ledger-schema
  "reference_failure_ledger_schema_gate;reference_replay_failure_ledger_gate"
)
sb_add_reference_policy_matrix_gate(
  reference_example_database_source_ledger_conformance
  example-source-ledger
  "reference_example_database_source_ledger_gate;reference_example_database_build_gate"
)
sb_add_reference_policy_matrix_gate(
  reference_example_database_source_map_conformance
  example-database-matrix
  "reference_example_source_map_gate;reference_example_database_build_gate"
)
sb_add_reference_policy_matrix_gate(
  reference_reproduction_command_transcript_conformance
  reproduction-transcript
  "reference_reproduction_command_transcript_gate"
)
sb_add_reference_policy_matrix_gate(
  reference_secret_redaction_artifact_conformance
  secret-redaction
  "reference_secret_redaction_artifact_gate"
)
sb_add_reference_policy_matrix_gate(
  reference_platform_toolchain_variance_conformance
  platform-variance
  "reference_platform_toolchain_variance_gate"
)

add_test(
  NAME reference_release_completion_blocker_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${SB_REFERENCE_RELEASE_COMPLETION_BLOCKER_GATE}"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
    "--evidence-file"
    "${CMAKE_BINARY_DIR}/tests/reference_regression/reference_release_completion_blocker_evidence.json"
)
set_tests_properties(reference_release_completion_blocker_conformance PROPERTIES
  LABELS "reference_release_regression_gate;reference_release_completion_blocker_gate;reference_original_regression_gate;mga_transaction_regression"
)

add_test(
  NAME compatibility_parser_public_docs_surface_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${SB_COMPATIBILITY_PARSER_DOCS_GATE}"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
    "--check"
    "--evidence-file"
    "${CMAKE_BINARY_DIR}/tests/reference_regression/compatibility_parser_public_docs_surface_evidence.json"
)
set_tests_properties(compatibility_parser_public_docs_surface_conformance PROPERTIES
  LABELS "reference_release_regression_gate;reference_parser_docs_gate;reference_original_regression_gate"
)

sb_add_reference_gate(
  reference_target_evidence_conformance
  target-evidence
  "reference_core_framework_gate;reference_relational_family_gate;reference_analytic_family_gate;reference_nosql_family_gate;reference_distributed_family_gate;reference_capability_reference_gate"
)

add_test(
  NAME reference_parser_start_evidence_manifest_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${SB_REFERENCE_PARSER_START_EVIDENCE_GATE}"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
    "--check"
)
set_tests_properties(reference_parser_start_evidence_manifest_conformance PROPERTIES
  LABELS "reference_parser_gate_evidence;reference_original_regression_gate;reference_release_regression_gate;reference_implementation_start_gate"
)

add_test(
  NAME reference_parser_implementation_start_handoff_conformance
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/reference_parser_gate_evidence_closure_gate.py"
    "--repo-root" "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(reference_parser_implementation_start_handoff_conformance PROPERTIES
  LABELS "reference_parser_gate_evidence;reference_original_regression_gate;reference_release_regression_gate;reference_implementation_start_gate"
)

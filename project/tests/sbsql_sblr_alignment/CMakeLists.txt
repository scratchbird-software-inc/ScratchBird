# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

# Process E2E fixtures use the same approved public database seeder as the
# release gates. Keep the harness usable when the optional release test group
# is disabled, without defining a duplicate target when it is enabled.
if(NOT TARGET public_driver_test_database_seed)
  add_executable(public_driver_test_database_seed
    ${CMAKE_SOURCE_DIR}/tools/release/public_driver_test_database_seed.cpp)
  target_compile_features(public_driver_test_database_seed PRIVATE cxx_std_23)
  target_link_libraries(public_driver_test_database_seed PRIVATE
    sb_engine_internal_api sb_storage_database sb_core_memory sb_core_uuid)
endif()

add_executable(sbsql_sblr_alignment_ia01_package_server_admission
  ia01_package_server_admission_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_package_server_admission
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_package_server_admission
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_package_server_admission
  PRIVATE sb_server_core sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia01_package_server_admission
  COMMAND sbsql_sblr_alignment_ia01_package_server_admission)
set_tests_properties(sbsql_sblr_alignment_ia01_package_server_admission PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;IA-01;SBLR_PACKAGE_BEGIN_END;integration")

add_executable(sbsql_sblr_alignment_cluster_root_gateway_stub
  ia01_cluster_root_gateway_stub_test.cpp)
target_compile_features(sbsql_sblr_alignment_cluster_root_gateway_stub
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_cluster_root_gateway_stub
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_cluster_root_gateway_stub
  PRIVATE sb_server_sblr_local_gateway sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_cluster_root_gateway_stub
  COMMAND sbsql_sblr_alignment_cluster_root_gateway_stub)
set_tests_properties(sbsql_sblr_alignment_cluster_root_gateway_stub PROPERTIES
  LABELS "sbsql_sblr_alignment;cluster_gateway;cluster_stub;SBLR_CLUSTER;external_provider_boundary;exact_refusal")

add_executable(sbsql_sblr_alignment_ia08a_database_filespace_profile_refusal
  ia08a_database_filespace_profile_refusal_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia08a_database_filespace_profile_refusal
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia08a_database_filespace_profile_refusal
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08a_database_filespace_profile_refusal
  PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08a_database_filespace_profile_refusal
  COMMAND sbsql_sblr_alignment_ia08a_database_filespace_profile_refusal)
set_tests_properties(sbsql_sblr_alignment_ia08a_database_filespace_profile_refusal PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-08;IA-08A;SBLR_FILESPACE_CREATE;SBLR_FILESPACE_DROP;SBLR_DATABASE_CHECKPOINT;SBLR_DATABASE_ALTER;SBLR_LIFECYCLE_CREATE_DATABASE;SBLR_LIFECYCLE_DROP_DATABASE;CSC-TEST-003025;CSC-TEST-003057;CSC-TEST-003641;CSC-TEST-003649;CSC-TEST-003653;CSC-TEST-003709;exact_refusal;no_side_effect;contract"
  TIMEOUT 30)

add_executable(sbsql_sblr_alignment_ia08b_catalog_profile_refusal
  ia08b_catalog_profile_refusal_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia08b_catalog_profile_refusal
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia08b_catalog_profile_refusal
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08b_catalog_profile_refusal
  PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08b_catalog_profile_refusal
  COMMAND sbsql_sblr_alignment_ia08b_catalog_profile_refusal)
set_tests_properties(sbsql_sblr_alignment_ia08b_catalog_profile_refusal PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-08;IA-08B;SBLR_DDL_CREATE_SCHEMA;SBLR_DDL_CREATE_TABLE;SBLR_DDL_ALTER_TABLE;SBLR_DDL_DROP_TABLE;SBLR_DDL_CREATE_DOMAIN;SBLR_DDL_DROP_DOMAIN;SBLR_DDL_DROP_SCHEMA;SBLR_DDL_ALTER_SCHEMA;SBLR_DDL_ALTER_DOMAIN;CSC-TEST-002561;CSC-TEST-002565;CSC-TEST-002569;CSC-TEST-002573;CSC-TEST-002585;CSC-TEST-002589;CSC-TEST-002593;CSC-TEST-002597;CSC-TEST-002605;exact_refusal;no_side_effect;contract"
  TIMEOUT 30)

add_executable(sbsql_sblr_alignment_ia08c_index_profile_refusal
  ia08c_index_profile_refusal_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia08c_index_profile_refusal
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia08c_index_profile_refusal
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08c_index_profile_refusal
  PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08c_index_profile_refusal
  COMMAND sbsql_sblr_alignment_ia08c_index_profile_refusal)
set_tests_properties(sbsql_sblr_alignment_ia08c_index_profile_refusal PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-08;IA-08C;SBLR_DDL_CREATE_INDEX;SBLR_DDL_DROP_INDEX;SBLR_DDL_ALTER_INDEX;SBLR_INDEX_REBUILD;SBLR_INDEX_REBALANCE;SBLR_INDEX_VERIFY;SBLR_INDEX_GATHER_STATISTICS;SBLR_INDEX_CLEANUP_MGA_VERSIONS;CSC-TEST-000561;CSC-TEST-001069;CSC-TEST-000081;CSC-TEST-003069;CSC-TEST-003073;CSC-TEST-003077;CSC-TEST-003081;CSC-TEST-003085;exact_refusal;no_side_effect;contract"
  TIMEOUT 30)

add_executable(sbsql_sblr_alignment_ia08d_routine_udr_profile_refusal
  ia08d_routine_udr_profile_refusal_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia08d_routine_udr_profile_refusal
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia08d_routine_udr_profile_refusal
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08d_routine_udr_profile_refusal
  PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08d_routine_udr_profile_refusal
  COMMAND sbsql_sblr_alignment_ia08d_routine_udr_profile_refusal)
set_tests_properties(sbsql_sblr_alignment_ia08d_routine_udr_profile_refusal PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-08;IA-08D;SBLR_DDL_CREATE_TRIGGER;SBLR_DDL_ALTER_TRIGGER;SBLR_DDL_DROP_TRIGGER;SBLR_DDL_CREATE_PROCEDURE;SBLR_DDL_ALTER_PROCEDURE;SBLR_DDL_DROP_PROCEDURE;SBLR_DDL_CREATE_FUNCTION;SBLR_DDL_ALTER_FUNCTION;SBLR_DDL_DROP_FUNCTION;CSC-TEST-002621;CSC-TEST-002625;CSC-TEST-002629;CSC-TEST-002633;CSC-TEST-002637;CSC-TEST-002641;CSC-TEST-002645;CSC-TEST-002649;CSC-TEST-002653;exact_refusal;no_side_effect;contract"
  TIMEOUT 30)

add_executable(sbsql_sblr_alignment_ia08e_catalog_cross_object_profile_refusal
  ia08e_catalog_cross_object_profile_refusal_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia08e_catalog_cross_object_profile_refusal
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia08e_catalog_cross_object_profile_refusal
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08e_catalog_cross_object_profile_refusal
  PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08e_catalog_cross_object_profile_refusal
  COMMAND sbsql_sblr_alignment_ia08e_catalog_cross_object_profile_refusal)
set_tests_properties(sbsql_sblr_alignment_ia08e_catalog_cross_object_profile_refusal PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-08;IA-08E;catalog_ddl;cross_object;exact_refusal;no_side_effect;contract"
  TIMEOUT 30)

add_executable(sbsql_sblr_alignment_ia09_security_profile_refusal
  ia09_security_profile_refusal_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia09_security_profile_refusal
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia09_security_profile_refusal
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia09_security_profile_refusal
  PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia09_security_profile_refusal
  COMMAND sbsql_sblr_alignment_ia09_security_profile_refusal)
set_tests_properties(sbsql_sblr_alignment_ia09_security_profile_refusal PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-09;security;exact_refusal;no_side_effect;contract"
  TIMEOUT 30)

add_executable(sbsql_sblr_alignment_ia01_literal_runtime
  ia01_literal_runtime_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_literal_runtime PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_literal_runtime
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_literal_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia01_literal_runtime
  COMMAND sbsql_sblr_alignment_ia01_literal_runtime)
set_tests_properties(sbsql_sblr_alignment_ia01_literal_runtime PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-01;SBLR_LITERAL;CSC-TEST-002326;malformed_operand;contract"
  TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia01_literal_protocol_fuzz
  ia01_literal_protocol_fuzz_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_literal_protocol_fuzz PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_literal_protocol_fuzz
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_literal_protocol_fuzz PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia01_literal_protocol_fuzz
  COMMAND sbsql_sblr_alignment_ia01_literal_protocol_fuzz)
set_tests_properties(sbsql_sblr_alignment_ia01_literal_protocol_fuzz PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fuzz;IA-01;SBLR_LITERAL;protocol_binary;property;mutation;fuzz"
  TIMEOUT 15)

add_executable(sbsql_sblr_alignment_ia01_executor_availability_state_fuzz
  ia01_executor_availability_state_fuzz_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_executor_availability_state_fuzz PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_executor_availability_state_fuzz
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include
          ${CMAKE_SOURCE_DIR}/src/engine/internal_api)
target_link_libraries(sbsql_sblr_alignment_ia01_executor_availability_state_fuzz
  PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia01_executor_availability_state_fuzz
  COMMAND sbsql_sblr_alignment_ia01_executor_availability_state_fuzz)
set_tests_properties(sbsql_sblr_alignment_ia01_executor_availability_state_fuzz PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fuzz;IA-01;SBLR_LITERAL;executor_availability_registry;state_machine;mutation;fuzz"
  TIMEOUT 15)

add_executable(sbsql_sblr_alignment_ia01_parameter_runtime
  ia01_parameter_runtime_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_parameter_runtime PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_parameter_runtime
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_parameter_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia01_parameter_runtime
  COMMAND sbsql_sblr_alignment_ia01_parameter_runtime)
set_tests_properties(sbsql_sblr_alignment_ia01_parameter_runtime PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-01;SBLR_PARAMETER;CSC-TEST-002330;malformed_operand;contract"
  TIMEOUT 30)

add_executable(sbsql_sblr_alignment_ia01_literal_missing_executor_integration
  ia01_literal_missing_executor_integration_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_literal_missing_executor_integration PRIVATE cxx_std_23)
target_include_directories(sbsql_sblr_alignment_ia01_literal_missing_executor_integration PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_literal_missing_executor_integration PRIVATE sb_server_core sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia01_literal_missing_executor_integration COMMAND sbsql_sblr_alignment_ia01_literal_missing_executor_integration)
set_tests_properties(sbsql_sblr_alignment_ia01_literal_missing_executor_integration PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;IA-01;SBLR_LITERAL;CSC-TEST-002327;missing_executor_evidence;integration" TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia01_literal_cancellation_fault
  ia01_literal_cancellation_fault_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_literal_cancellation_fault PRIVATE cxx_std_23)
target_include_directories(sbsql_sblr_alignment_ia01_literal_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_literal_cancellation_fault PRIVATE sb_server_core sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia01_literal_cancellation_fault COMMAND sbsql_sblr_alignment_ia01_literal_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia01_literal_cancellation_fault PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fault;IA-01;SBLR_LITERAL;CSC-TEST-002328;cancellation;fault" TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia01_query_execute_result_contract
  ia01_query_execute_result_contract_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_query_execute_result_contract
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_query_execute_result_contract
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_query_execute_result_contract
  PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia01_query_execute_result_contract
  COMMAND sbsql_sblr_alignment_ia01_query_execute_result_contract)
set_tests_properties(sbsql_sblr_alignment_ia01_query_execute_result_contract PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-01;SBLR_QUERY_EXECUTE;result_shape")

add_executable(sbsql_sblr_alignment_ia01_package_cancellation_fault
  ia01_package_cancellation_fault_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_package_cancellation_fault
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_package_cancellation_fault
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_package_cancellation_fault
  PRIVATE sb_server_core sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia01_package_cancellation_fault
  COMMAND sbsql_sblr_alignment_ia01_package_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia01_package_cancellation_fault PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fault;IA-01;SBLR_PACKAGE_BEGIN_END;fault"
  TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia01_bigint_identity_migration
  ia01_bigint_identity_migration_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_bigint_identity_migration
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_bigint_identity_migration
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include
          ${CMAKE_SOURCE_DIR}/src/engine/internal_api)
target_link_libraries(sbsql_sblr_alignment_ia01_bigint_identity_migration
  PRIVATE sb_engine_internal_api sb_storage_database sb_transaction_mga
          sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia01_bigint_identity_migration
  COMMAND sbsql_sblr_alignment_ia01_bigint_identity_migration)
set_tests_properties(sbsql_sblr_alignment_ia01_bigint_identity_migration PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;sbsql_sblr_alignment_fault;sbsql_sblr_alignment_recovery;IA-01;SBLR_LITERAL;integration"
  TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia01_executor_availability_registry
  ia01_executor_availability_registry_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_executor_availability_registry PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_executor_availability_registry
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include
          ${CMAKE_SOURCE_DIR}/src/engine/internal_api)
target_link_libraries(sbsql_sblr_alignment_ia01_executor_availability_registry
  PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia01_executor_availability_registry
  COMMAND sbsql_sblr_alignment_ia01_executor_availability_registry)
set_tests_properties(sbsql_sblr_alignment_ia01_executor_availability_registry PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;sbsql_sblr_alignment_fault;sbsql_sblr_alignment_recovery;IA-01;SBLR_LITERAL;executor_availability_registry"
  TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia01_package_resource_governance
  ia01_package_resource_governance_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_package_resource_governance
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_package_resource_governance
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include
          ${CMAKE_SOURCE_DIR}/src/server_engine_bridge
          ${CMAKE_SOURCE_DIR}/src/engine/internal_api)
target_link_libraries(sbsql_sblr_alignment_ia01_package_resource_governance
  PRIVATE sb_core_agents)
add_test(NAME sbsql_sblr_alignment_ia01_package_resource_governance
  COMMAND sbsql_sblr_alignment_ia01_package_resource_governance)
set_tests_properties(sbsql_sblr_alignment_ia01_package_resource_governance PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fault;IA-01;SBLR_PACKAGE_BEGIN_END;resource_governance;fault"
  TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia01_parameter_set_registry
  ia01_parameter_set_registry_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_parameter_set_registry
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_parameter_set_registry
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include
          ${CMAKE_SOURCE_DIR}/src/engine/internal_api)
target_link_libraries(sbsql_sblr_alignment_ia01_parameter_set_registry
  PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia01_parameter_set_registry
  COMMAND sbsql_sblr_alignment_ia01_parameter_set_registry)
set_tests_properties(sbsql_sblr_alignment_ia01_parameter_set_registry PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;sbsql_sblr_alignment_recovery;IA-01;SBLR_PARAMETER"
  TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia01_prepared_coordination_registry
  ia01_prepared_coordination_registry_test.cpp)
target_compile_features(
  sbsql_sblr_alignment_ia01_prepared_coordination_registry PRIVATE cxx_std_20)
target_include_directories(
  sbsql_sblr_alignment_ia01_prepared_coordination_registry PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src/engine/internal_api)
target_link_libraries(
  sbsql_sblr_alignment_ia01_prepared_coordination_registry PRIVATE
  sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia01_prepared_coordination_registry
  COMMAND sbsql_sblr_alignment_ia01_prepared_coordination_registry)
set_tests_properties(
  sbsql_sblr_alignment_ia01_prepared_coordination_registry PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;sbsql_sblr_alignment_integration;sbsql_sblr_alignment_recovery;IA-01;SBLR_PARAMETER;prepared_coordination"
  TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia01_package_missing_executor_integration
  ia01_package_missing_executor_integration_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_package_missing_executor_integration
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_package_missing_executor_integration
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_package_missing_executor_integration
  PRIVATE sb_server_core sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia01_package_missing_executor_integration
  COMMAND sbsql_sblr_alignment_ia01_package_missing_executor_integration)
set_tests_properties(sbsql_sblr_alignment_ia01_package_missing_executor_integration PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;IA-01;SBLR_PACKAGE_BEGIN_END;missing_executor_evidence;integration"
  TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia01_cursor_stream_descriptor_integration
  ia01_cursor_stream_descriptor_integration_test.cpp)
target_compile_features(
  sbsql_sblr_alignment_ia01_cursor_stream_descriptor_integration
  PRIVATE cxx_std_20)
target_include_directories(
  sbsql_sblr_alignment_ia01_cursor_stream_descriptor_integration
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(
  sbsql_sblr_alignment_ia01_cursor_stream_descriptor_integration
  PRIVATE sb_server_core sb_engine_sblr)
add_test(
  NAME sbsql_sblr_alignment_ia01_cursor_stream_descriptor_integration
  COMMAND sbsql_sblr_alignment_ia01_cursor_stream_descriptor_integration)
set_tests_properties(
  sbsql_sblr_alignment_ia01_cursor_stream_descriptor_integration PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;IA-01;SBLR_PACKAGE_BEGIN_END;cursor_stream_descriptor;integration"
  TIMEOUT 60)

function(sbsql_sblr_alignment_register_ia01_literal_process_e2e)
  if(TARGET sb_server AND TARGET sb_listener AND TARGET sbp_sbsql AND TARGET sb_isql)
    add_test(
      NAME sbsql_sblr_alignment_ia01_literal_process_e2e
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_package_process_e2e.py"
              --server $<TARGET_FILE:sb_server>
              --listener $<TARGET_FILE:sb_listener>
              --parser-worker $<TARGET_FILE:sbp_sbsql>
              --sb-isql $<TARGET_FILE:sb_isql>
              --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia01_literal_e2e")
    set_tests_properties(
      sbsql_sblr_alignment_ia01_literal_process_e2e
      PROPERTIES
        LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_PACKAGE_BEGIN_END;SBLR_LITERAL;CSC-TEST-002325;e2e"
        TIMEOUT 240)
endif()
endfunction()

# Parser-worker and CLI targets are declared after this directory. Register at
# the end of the product root, when all four real process dependencies exist.
cmake_language(
  DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
  CALL sbsql_sblr_alignment_register_ia01_literal_process_e2e)

function(sbsql_sblr_alignment_register_ia01_source_map_process_e2e)
  if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
    add_test(
      NAME sbsql_sblr_alignment_ia01_source_map_process_e2e
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py"
              --server $<TARGET_FILE:sb_server>
              --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
              --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia01_source_map_e2e")
    set_tests_properties(
      sbsql_sblr_alignment_ia01_source_map_process_e2e PROPERTIES
      LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_SOURCE_MAP;CSC-TEST-002337;e2e"
      TIMEOUT 240)
  endif()
endfunction()
cmake_language(
  DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
  CALL sbsql_sblr_alignment_register_ia01_source_map_process_e2e)

function(sbsql_sblr_alignment_register_ia00_show_version_process_e2e)
  if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
    add_test(NAME sbsql_sblr_alignment_ia00_show_version_process_e2e
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py"
        --server $<TARGET_FILE:sb_server>
        --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
        --operation show-version
        --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia00_show_version_e2e")
    set_tests_properties(sbsql_sblr_alignment_ia00_show_version_process_e2e PROPERTIES
      LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-00;SBLR_OBSERVABILITY_SHOW_VERSION;CSC-TEST-002701;e2e"
      TIMEOUT 240)
  endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
  CALL sbsql_sblr_alignment_register_ia00_show_version_process_e2e)

function(sbsql_sblr_alignment_register_ia00_show_wait_events_process_e2e)
  if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
    add_test(NAME sbsql_sblr_alignment_ia00_show_wait_events_process_e2e
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py"
        --server $<TARGET_FILE:sb_server>
        --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
        --operation show-wait-events
        --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia00_show_wait_events_e2e")
    set_tests_properties(sbsql_sblr_alignment_ia00_show_wait_events_process_e2e PROPERTIES
      LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-00;SBLR_READ_METRICS;CSC-TEST-002702;e2e"
      TIMEOUT 240)
  endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
  CALL sbsql_sblr_alignment_register_ia00_show_wait_events_process_e2e)

function(sbsql_sblr_alignment_register_ia01_error_vector_process_e2e)
  if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
    add_test(NAME sbsql_sblr_alignment_ia01_error_vector_process_e2e
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py"
        --server $<TARGET_FILE:sb_server>
        --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
        --operation error-vector
        --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia01_error_vector_e2e")
    set_tests_properties(sbsql_sblr_alignment_ia01_error_vector_process_e2e PROPERTIES
      LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_ERROR_VECTOR;CSC-TEST-002341;e2e"
      TIMEOUT 240)
  endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
  CALL sbsql_sblr_alignment_register_ia01_error_vector_process_e2e)

function(sbsql_sblr_alignment_register_ia05_txn_begin_process_e2e)
  if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
    add_test(NAME sbsql_sblr_alignment_ia05_txn_begin_process_e2e
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py"
        --server $<TARGET_FILE:sb_server>
        --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
        --operation txn-begin
        --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia05_txn_begin_e2e")
    set_tests_properties(sbsql_sblr_alignment_ia05_txn_begin_process_e2e PROPERTIES
      LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-05;SBLR_TXN_BEGIN;CSC-TEST-002345;e2e"
      TIMEOUT 240)
  endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
  CALL sbsql_sblr_alignment_register_ia05_txn_begin_process_e2e)

function(sbsql_sblr_alignment_register_ia05_txn_commit_process_e2e)
  if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
    add_test(NAME sbsql_sblr_alignment_ia05_txn_commit_process_e2e
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py"
        --server $<TARGET_FILE:sb_server>
        --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
        --operation txn-commit
        --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia05_txn_commit_e2e")
    set_tests_properties(sbsql_sblr_alignment_ia05_txn_commit_process_e2e PROPERTIES
      LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-05;SBLR_TXN_COMMIT;CSC-TEST-002349;e2e"
      TIMEOUT 240)
  endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
  CALL sbsql_sblr_alignment_register_ia05_txn_commit_process_e2e)

function(sbsql_sblr_alignment_register_ia05_txn_rollback_process_e2e)
  if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
    add_test(NAME sbsql_sblr_alignment_ia05_txn_rollback_process_e2e
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py"
        --server $<TARGET_FILE:sb_server>
        --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
        --operation txn-rollback
        --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia05_txn_rollback_e2e")
    set_tests_properties(sbsql_sblr_alignment_ia05_txn_rollback_process_e2e PROPERTIES
      LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-05;SBLR_TXN_ROLLBACK;CSC-TEST-002353;e2e"
      TIMEOUT 240)
  endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
  CALL sbsql_sblr_alignment_register_ia05_txn_rollback_process_e2e)

function(sbsql_sblr_alignment_register_ia05_txn_savepoint_process_e2e)
  if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
    add_test(NAME sbsql_sblr_alignment_ia05_txn_savepoint_process_e2e
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py"
        --server $<TARGET_FILE:sb_server>
        --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
        --operation txn-savepoint
        --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia05_txn_savepoint_e2e")
    set_tests_properties(sbsql_sblr_alignment_ia05_txn_savepoint_process_e2e PROPERTIES
      LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-05;SBLR_TXN_SAVEPOINT;CSC-TEST-002357;e2e"
      TIMEOUT 240)
  endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
  CALL sbsql_sblr_alignment_register_ia05_txn_savepoint_process_e2e)

function(sbsql_sblr_alignment_register_ia05_txn_release_savepoint_process_e2e)
  if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
    add_test(NAME sbsql_sblr_alignment_ia05_txn_release_savepoint_process_e2e
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py"
        --server $<TARGET_FILE:sb_server>
        --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
        --operation txn-release-savepoint
        --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia05_txn_release_savepoint_e2e")
    set_tests_properties(sbsql_sblr_alignment_ia05_txn_release_savepoint_process_e2e PROPERTIES
      LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-05;SBLR_TXN_RELEASE_SAVEPOINT;CSC-TEST-002361;e2e"
      TIMEOUT 240)
  endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
  CALL sbsql_sblr_alignment_register_ia05_txn_release_savepoint_process_e2e)

function(sbsql_sblr_alignment_register_ia05_txn_rollback_to_savepoint_process_e2e)
  if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
    add_test(NAME sbsql_sblr_alignment_ia05_txn_rollback_to_savepoint_process_e2e COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation txn-rollback-to-savepoint --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia05_txn_rollback_to_savepoint_e2e")
    set_tests_properties(sbsql_sblr_alignment_ia05_txn_rollback_to_savepoint_process_e2e PROPERTIES
      LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-05;SBLR_TXN_ROLLBACK_TO_SAVEPOINT;CSC-TEST-002365;e2e" TIMEOUT 240)
  endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia05_txn_rollback_to_savepoint_process_e2e)

function(sbsql_sblr_alignment_register_ia05_psql_autonomous_frame_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia05_psql_autonomous_frame_process_e2e COMMAND "${Python3_EXECUTABLE}"
   "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server>
   --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation psql-autonomous-frame
   --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia05_psql_autonomous_frame_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia05_psql_autonomous_frame_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-05;SBLR_PSQL_AUTONOMOUS_FRAME;CSC-TEST-002369;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia05_psql_autonomous_frame_process_e2e)

function(sbsql_sblr_alignment_register_ia05_transaction_reservation_release_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia05_transaction_reservation_release_process_e2e COMMAND "${Python3_EXECUTABLE}"
   "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server>
   --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation transaction-reservation-release
   --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia05_transaction_reservation_release_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia05_transaction_reservation_release_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-05;SBLR_TRANSACTION_RESERVATION_RELEASE;CSC-TEST-002373;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia05_transaction_reservation_release_process_e2e)
function(sbsql_sblr_alignment_register_ia05_temporary_instance_cleanup_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia05_temporary_instance_cleanup_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation temporary-instance-cleanup --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia05_temporary_instance_cleanup_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia05_temporary_instance_cleanup_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-05;SBLR_TEMPORARY_INSTANCE_CLEANUP;CSC-TEST-002377;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia05_temporary_instance_cleanup_process_e2e)

function(sbsql_sblr_alignment_register_ia06_cursor_open_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia06_cursor_open_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation cursor-open --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_cursor_open_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_cursor_open_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_CURSOR_OPEN;CSC-TEST-002381;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia06_cursor_open_process_e2e)
function(sbsql_sblr_alignment_register_ia06_cursor_fetch_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia06_cursor_fetch_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation cursor-fetch --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_cursor_fetch_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_cursor_fetch_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_CURSOR_FETCH;CSC-TEST-002385;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia06_cursor_fetch_process_e2e)
function(sbsql_sblr_alignment_register_ia06_cursor_close_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia06_cursor_close_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation cursor-close --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_cursor_close_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_cursor_close_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_CURSOR_CLOSE;CSC-TEST-002389;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia06_cursor_close_process_e2e)
function(sbsql_sblr_alignment_register_ia06_read_by_key_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia06_read_by_key_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation read-by-key --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_read_by_key_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_read_by_key_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_READ_BY_KEY;CSC-TEST-002393;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia06_read_by_key_process_e2e)
function(sbsql_sblr_alignment_register_ia06_read_range_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia06_read_range_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation read-range --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_read_range_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_read_range_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_READ_RANGE;CSC-TEST-002397;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia06_read_range_process_e2e)
function(sbsql_sblr_alignment_register_ia06_read_stream_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia06_read_stream_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation read-stream --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_read_stream_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_read_stream_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_READ_STREAM;CSC-TEST-002401;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia06_read_stream_process_e2e)
function(sbsql_sblr_alignment_register_ia06_result_set_pass_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia06_result_set_pass_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation result-set-pass --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_result_set_pass_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_result_set_pass_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_RESULT_SET_PASS;CSC-TEST-002405;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia06_result_set_pass_process_e2e)
function(sbsql_sblr_alignment_register_ia06_access_cursor_open_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia06_access_cursor_open_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation access-cursor-open --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_access_cursor_open_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_access_cursor_open_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_ACCESS_CURSOR_OPEN;CSC-TEST-002409;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia06_access_cursor_open_process_e2e)
function(sbsql_sblr_alignment_register_ia06_access_cursor_fetch_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia06_access_cursor_fetch_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation access-cursor-fetch --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_access_cursor_fetch_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_access_cursor_fetch_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_ACCESS_CURSOR_FETCH;CSC-TEST-002413;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia06_access_cursor_fetch_process_e2e)
function(sbsql_sblr_alignment_register_ia06_access_cursor_close_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia06_access_cursor_close_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation access-cursor-close --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_access_cursor_close_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_access_cursor_close_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_ACCESS_CURSOR_CLOSE;CSC-TEST-002417;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia06_access_cursor_close_process_e2e)
function(sbsql_sblr_alignment_register_ia07_insert_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia07_insert_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation insert --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_insert_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia07_insert_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-07;SBLR_INSERT;CSC-TEST-002421;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia07_insert_process_e2e)
function(sbsql_sblr_alignment_register_ia07_update_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia07_update_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation update --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_update_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia07_update_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-07;SBLR_UPDATE;CSC-TEST-002425;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia07_update_process_e2e)
function(sbsql_sblr_alignment_register_ia07_delete_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia07_delete_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation delete --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_delete_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia07_delete_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-07;SBLR_DELETE;CSC-TEST-002429;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia07_delete_process_e2e)
function(sbsql_sblr_alignment_register_ia07_merge_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia07_merge_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation merge --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_merge_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia07_merge_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-07;SBLR_MERGE;CSC-TEST-002433;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia07_merge_process_e2e)
function(sbsql_sblr_alignment_register_ia07_table_truncate_process_e2e)
 if(TARGET sb_server AND TARGET sbsql_sblr_alignment_ia01_source_map_process_client)
  add_test(NAME sbsql_sblr_alignment_ia07_table_truncate_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation table-truncate --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_table_truncate_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia07_table_truncate_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-07;SBLR_TABLE_TRUNCATE;CSC-TEST-002441;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia07_table_analyze_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation table-analyze --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_table_analyze_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia07_table_analyze_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-07;SBLR_TABLE_ANALYZE;CSC-TEST-002445;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia07_bulk_import_stream_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation bulk-import-stream --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_bulk_import_stream_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia07_bulk_import_stream_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-07;SBLR_BULK_IMPORT_STREAM;CSC-TEST-002449;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia07_bulk_export_stream_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation bulk-export-stream --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_bulk_export_stream_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia07_bulk_export_stream_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-07;SBLR_BULK_EXPORT_STREAM;CSC-TEST-002453;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia07_statement_batch_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation statement-batch --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_statement_batch_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia07_statement_batch_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-07;SBLR_STATEMENT_BATCH;CSC-TEST-002457;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia07_atomic_cas_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation atomic-cas --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_atomic_cas_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia07_atomic_cas_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-07;SBLR_ATOMIC_CAS;CSC-TEST-002461;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia07_atomic_rmw_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation atomic-rmw --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_atomic_rmw_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia07_atomic_rmw_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-07;SBLR_ATOMIC_READ_MODIFY_WRITE;CSC-TEST-002465;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia07_advisory_lock_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation advisory-lock --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_advisory_lock_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia07_advisory_lock_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-07;SBLR_ADVISORY_LOCK_ACQUIRE;CSC-TEST-002469;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia07_advisory_lock_release_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation advisory-lock-release --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_advisory_lock_release_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia07_advisory_lock_release_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-07;SBLR_ADVISORY_LOCK_RELEASE;CSC-TEST-002473;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia06_function_call_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation function-call --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_function_call_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_function_call_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_FUNCTION_CALL;CSC-TEST-002477;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia06_operator_call_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation operator-call --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_operator_call_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_operator_call_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_OPERATOR_CALL;CSC-TEST-002481;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia06_cast_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation cast --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_cast_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_cast_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_CAST;CSC-TEST-002485;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia06_compare_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation compare --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_compare_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_compare_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_COMPARE;CSC-TEST-002489;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia06_domain_operation_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation domain-operation --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_domain_operation_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_domain_operation_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_DOMAIN_OPERATION;CSC-TEST-002493;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia06_udr_invoke_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation udr-invoke --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_udr_invoke_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_udr_invoke_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_UDR_INVOKE;CSC-TEST-002497;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia06_procedure_invoke_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation procedure-invoke --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_procedure_invoke_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_procedure_invoke_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_PROCEDURE_INVOKE;CSC-TEST-002501;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia06_function_invoke_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation function-invoke --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_function_invoke_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_function_invoke_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_FUNCTION_INVOKE;CSC-TEST-002505;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia06_aggregate_invoke_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation aggregate-invoke --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_aggregate_invoke_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_aggregate_invoke_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_AGGREGATE_INVOKE;CSC-TEST-002509;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia06_sequence_nextval_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation sequence-nextval --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_sequence_nextval_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_sequence_nextval_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_SEQUENCE_NEXTVAL;CSC-TEST-002513;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia06_sequence_currval_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation sequence-currval --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_sequence_currval_e2e")
  add_test(NAME sbsql_sblr_alignment_ia06_sequence_setval_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation sequence-setval --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_sequence_setval_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_sequence_setval_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_SEQUENCE_SETVAL;CSC-TEST-002521;e2e")
  add_test(NAME sbsql_sblr_alignment_ia06_query_numeric_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation query-numeric --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_query_numeric_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_query_numeric_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_QUERY_APPLY_NUMERIC_OPERATION;CSC-TEST-002525;e2e")
  add_test(NAME sbsql_sblr_alignment_ia06_advanced_datatype_family_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation advanced-datatype-family --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia06_advanced_datatype_family_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia06_advanced_datatype_family_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_QUERY_EVALUATE_ADVANCED_DATATYPE_FAMILY;CSC-TEST-002529;e2e")
  add_test(NAME sbsql_sblr_alignment_ia01_project_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation project --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia01_project_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia01_project_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_PROJECT;CSC-TEST-002533;e2e")
  add_test(NAME sbsql_sblr_alignment_ia01_aggregate_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation aggregate --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia01_aggregate_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia01_aggregate_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_AGGREGATE;CSC-TEST-002537;e2e")
  add_test(NAME sbsql_sblr_alignment_ia01_group_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation group --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia01_group_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia01_group_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_GROUP;CSC-TEST-002541;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia01_sort_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation sort --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia01_sort_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia01_sort_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_SORT;CSC-TEST-002545;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia01_limit_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation limit --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia01_limit_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia01_limit_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_LIMIT;CSC-TEST-002549;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia01_window_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation window --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia01_window_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia01_window_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_WINDOW;CSC-TEST-002553;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia01_return_result_set_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation return-result-set --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia01_return_result_set_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia01_return_result_set_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_RETURN_RESULT_SET;CSC-TEST-002557;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_read_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation kv-structured-read --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_kv_structured_read_e2e")
  add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_mutate_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation kv-structured-mutate --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_kv_structured_mutate_e2e")
  add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_scan_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation kv-structured-scan --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_kv_structured_scan_e2e")
  add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_stream_read_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation kv-structured-stream-read --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_kv_structured_stream_read_e2e")
  add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_stream_append_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation kv-structured-stream-append --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_kv_structured_stream_append_e2e")
  add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_timeseries_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation kv-structured-timeseries --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia07_kv_structured_timeseries_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_system_config_set_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation system-config-set --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_system_config_set_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_system_config_get_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation system-config-get --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_system_config_get_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_system_config_get_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_SYSTEM_CONFIG_GET;CSC-TEST-003081;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_system_config_reset_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation system-config-reset --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_system_config_reset_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_system_config_reset_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_SYSTEM_CONFIG_RESET;CSC-TEST-003084;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_rule_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-rule --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_rule_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_rule_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_DDL_CREATE_RULE;CSC-TEST-003087;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_rule_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-rule --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_rule_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_rule_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_DDL_DROP_RULE;CSC-TEST-003090;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_publication_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-publication --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_publication_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_publication_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_DDL_CREATE_PUBLICATION;CSC-TEST-003091;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_publication_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-alter-publication --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_alter_publication_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_publication_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_DDL_ALTER_PUBLICATION;CSC-TEST-003092;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_publication_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-publication --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_publication_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_publication_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_DDL_DROP_PUBLICATION;CSC-TEST-003096;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_subscription_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-subscription --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_subscription_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_subscription_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-08;SBLR_DDL_CREATE_SUBSCRIPTION;CSC-TEST-003989;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_subscription_cluster_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-alter-subscription --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_alter_subscription_cluster_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_subscription_cluster_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-08;SBLR_DDL_ALTER_SUBSCRIPTION;CSC-TEST-003993;cluster_required;e2e" TIMEOUT 240 DISABLED TRUE)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_subscription_cluster_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-subscription --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_subscription_cluster_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_subscription_cluster_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-08;SBLR_DDL_DROP_SUBSCRIPTION;CSC-TEST-003997;cluster_required;e2e" TIMEOUT 240 DISABLED TRUE)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_operator_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-operator --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_operator_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_operator_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-08;SBLR_DDL_CREATE_OPERATOR;CSC-TEST-004001;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_operator_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-operator --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_operator_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_operator_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-08;SBLR_DDL_DROP_OPERATOR;CSC-TEST-004005;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_operator_class_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-operator-class --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_operator_class_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_operator_class_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-08;SBLR_DDL_CREATE_OPERATOR_CLASS;CSC-TEST-004009;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_operator_class_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-operator-class --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_operator_class_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_operator_class_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-08;SBLR_DDL_DROP_OPERATOR_CLASS;CSC-TEST-004013;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_operator_family_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-operator-family --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_operator_family_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_operator_family_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-08;SBLR_DDL_CREATE_OPERATOR_FAMILY;CSC-TEST-004017;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_operator_family_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-alter-operator-family --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_alter_operator_family_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_operator_family_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-08;SBLR_DDL_ALTER_OPERATOR_FAMILY;CSC-TEST-004021;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_cast_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-cast --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_cast_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_cast_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-08;SBLR_DDL_DROP_CAST;CSC-TEST-004033;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_domain_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-domain --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_domain_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_domain_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-alter-domain --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_alter_domain_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_domain_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_DOMAIN;CSC-TEST-002605;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_view_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-view --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_view_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_view_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-alter-view --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_alter_view_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_view_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-view --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_view_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-refresh-materialized-view --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-materialized-view --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_type_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-type --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_type_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_type_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-08;SBLR_DDL_CREATE_TYPE;CSC-TEST-002673;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_type_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-alter-type --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_alter_type_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_type_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-08;SBLR_DDL_ALTER_TYPE;CSC-TEST-002675;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_type_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-type --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_type_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_type_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-08;SBLR_DDL_DROP_TYPE;CSC-TEST-002677;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_table_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-table --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_table_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_table_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-08;SBLR_DDL_DROP_TABLE;CSC-TEST-002801;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_REFRESH_MATERIALIZED_VIEW;CSC-TEST-002953;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_MATERIALIZED_VIEW;CSC-TEST-002954;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_trigger_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-trigger --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_trigger_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_trigger_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TRIGGER;CSC-TEST-002621;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_trigger_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-alter-trigger --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_alter_trigger_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_trigger_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_TRIGGER;CSC-TEST-002625;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_trigger_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-trigger --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_trigger_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_trigger_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_TRIGGER;CSC-TEST-002629;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_procedure_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-procedure --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_procedure_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_procedure_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_PROCEDURE;CSC-TEST-002633;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_procedure_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-alter-procedure --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_alter_procedure_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_procedure_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_PROCEDURE;CSC-TEST-002637;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_procedure_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-procedure --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_procedure_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_procedure_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_PROCEDURE;CSC-TEST-002641;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_function_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-function --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_function_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_function_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_FUNCTION;CSC-TEST-002645;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_function_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-alter-function --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_alter_function_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_function_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_FUNCTION;CSC-TEST-002649;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_function_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-function --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_function_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_function_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_FUNCTION;CSC-TEST-002653;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_package_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-package --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_package_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_package_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_PACKAGE;CSC-TEST-002657;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_package_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-package --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_package_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_package_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_PACKAGE;CSC-TEST-002893;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_package_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-alter-package --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_alter_package_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_package_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_PACKAGE;CSC-TEST-002889;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_sequence_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-alter-sequence --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_alter_sequence_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_sequence_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-sequence --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_sequence_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_sequence_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_SEQUENCE;CSC-TEST-002905;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_sequence_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_SEQUENCE;CSC-TEST-002901;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_temporary_table_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-temporary-table --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_temporary_table_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-temporary-table --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_rename_object_vector_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-rename-object-vector --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_rename_object_vector_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_rename_object_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-rename-object --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_rename_object_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-or-replace-srs --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_srs_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-srs --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_srs_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-rewrite-rule --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-alter-rewrite-rule --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_REWRITE_RULE;CSC-TEST-002685;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-rewrite-rule --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_validate_constraint_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-validate-constraint --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_validate_constraint_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_security_create_privilege_template_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-create-privilege-template --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_security_create_privilege_template_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia09_security_create_privilege_template_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SECURITY_CREATE_PRIVILEGE_TEMPLATE;CSC-TEST-002697;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia09_security_create_user_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-create-user --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_security_create_user_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia09_security_create_user_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SEC_CREATE_USER;CSC-TEST-002965;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia09_security_alter_user_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-alter-user --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_security_alter_user_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia09_security_alter_user_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SEC_ALTER_USER;CSC-TEST-002969;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia09_security_create_role_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-create-role --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_security_create_role_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_security_create_policy_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-create-policy --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_security_create_policy_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_security_drop_policy_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-drop-policy --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_security_drop_policy_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_security_alter_policy_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-alter-policy --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_security_alter_policy_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_security_drop_user_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-drop-user --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_security_drop_user_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_security_authenticate_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-authenticate --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_security_authenticate_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_security_deauthenticate_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-deauthenticate --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_security_deauthenticate_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_session_role_switch_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation session-role-switch --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_session_role_switch_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_session_setting_set_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation session-setting-set --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_session_setting_set_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_session_setting_reset_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation session-setting-reset --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_session_setting_reset_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_session_setting_get_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation session-setting-get --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_session_setting_get_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_session_default_qualifier_set_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation session-default-qualifier-set --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_session_default_qualifier_set_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_session_discard_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation session-discard --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_session_discard_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_session_snapshot_handle_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation session-snapshot-handle --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_session_snapshot_handle_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_context_set_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation context-set --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_context_set_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_context_unset_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation context-unset --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_context_unset_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_context_get_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation context-get --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_context_get_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_stmt_prepare_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation stmt-prepare --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_stmt_prepare_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_security_alter_role_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-alter-role --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_security_alter_role_e2e")
  add_test(NAME sbsql_sblr_alignment_ia09_security_drop_role_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-drop-role --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_security_drop_role_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia09_security_create_role_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SEC_CREATE_ROLE;CSC-TEST-002973;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia09_security_alter_privilege_template_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-alter-privilege-template --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_security_alter_privilege_template_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia09_security_alter_privilege_template_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SECURITY_ALTER_PRIVILEGE_TEMPLATE;CSC-TEST-002701;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia09_security_drop_privilege_template_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-drop-privilege-template --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia09_security_drop_privilege_template_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia09_security_drop_privilege_template_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SECURITY_DROP_PRIVILEGE_TEMPLATE;CSC-TEST-002705;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_database_create_template_clone_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation database-create-template-clone --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_database_create_template_clone_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_database_create_template_clone_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DATABASE_CREATE_TEMPLATE_CLONE;CSC-TEST-002709;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_aggregate_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-aggregate --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_aggregate_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_macro_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-macro --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_macro_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_dictionary_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-dictionary --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_dictionary_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_dictionary_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-dictionary --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_dictionary_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_dictionary_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_DICTIONARY;CSC-TEST-002765;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_dictionary_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-alter-dictionary --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_alter_dictionary_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_dictionary_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_DICTIONARY;CSC-TEST-002765;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_continuous_view_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-continuous-view --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_continuous_view_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_continuous_view_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_CONTINUOUS_VIEW;CSC-TEST-002773;e2e" TIMEOUT 240)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_continuous_view_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-alter-continuous-view --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_alter_continuous_view_e2e")
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_continuous_view_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_CONTINUOUS_VIEW;CSC-TEST-002777;e2e" TIMEOUT 240)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_continuous_view_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-continuous-view --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_continuous_view_e2e")
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_continuous_view_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_CONTINUOUS_VIEW;CSC-TEST-002781;e2e" TIMEOUT 240)
add_test(NAME sbsql_sblr_alignment_ia08_dml_async_insert_submit_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation dml-async-insert-submit --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_dml_async_insert_submit_e2e")
set_tests_properties(sbsql_sblr_alignment_ia08_dml_async_insert_submit_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_ASYNC_INSERT_SUBMIT;CSC-TEST-002785;e2e" TIMEOUT 240)
add_test(NAME sbsql_sblr_alignment_ia08_dml_async_insert_cancel_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation dml-async-insert-cancel --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_dml_async_insert_cancel_e2e")
set_tests_properties(sbsql_sblr_alignment_ia08_dml_async_insert_cancel_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_ASYNC_INSERT_CANCEL;CSC-TEST-002793;e2e" TIMEOUT 240)
add_test(NAME sbsql_sblr_alignment_ia08_dml_async_insert_status_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation dml-async-insert-status --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_dml_async_insert_status_e2e")
add_test(NAME sbsql_sblr_alignment_ia08_dml_counter_add_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation dml-counter-add --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_dml_counter_add_e2e")
add_test(NAME sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation dml-timeseries-schema-write --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_e2e")
set_tests_properties(sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_TIMESERIES_SCHEMA_WRITE;CSC-TEST-002805;e2e" TIMEOUT 240)
set_tests_properties(sbsql_sblr_alignment_ia08_dml_counter_add_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_COUNTER_ADD;CSC-TEST-002801;e2e" TIMEOUT 240)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-timeseries-series-cardinality-policy --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_e2e")
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_SET_TIMESERIES_SERIES_CARDINALITY_POLICY;CSC-TEST-2809;e2e" TIMEOUT 240)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-timeseries-value-cache --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_e2e")
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TIMESERIES_VALUE_CACHE;CSC-TEST-2813;e2e" TIMEOUT 240)
set_tests_properties(sbsql_sblr_alignment_ia08_dml_async_insert_status_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_ASYNC_INSERT_STATUS;CSC-TEST-002789;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_dictionary_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_DICTIONARY;CSC-TEST-002761;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_macro_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002745;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_macro_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-macro --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_macro_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_macro_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002749;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_admin_register_external_relation_resolver_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation admin-register-external-relation-resolver --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_admin_register_external_relation_resolver_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_admin_register_external_relation_resolver_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002753;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_admin_unregister_external_relation_resolver_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation admin-unregister-external-relation-resolver --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_admin_unregister_external_relation_resolver_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_admin_unregister_external_relation_resolver_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002757;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_aggregate_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_AGGREGATE;CSC-TEST-002713;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_aggregate_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-alter-aggregate --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_alter_aggregate_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_aggregate_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_AGGREGATE;CSC-TEST-002717;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_aggregate_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-aggregate --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_aggregate_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_aggregate_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_AGGREGATE;CSC-TEST-002721;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_purge_system_history_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-purge-system-history --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_purge_system_history_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_purge_system_history_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_PURGE_SYSTEM_HISTORY;CSC-TEST-002725;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-set-index-optimizer-eligibility --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_SET_INDEX_OPTIMIZER_ELIGIBILITY;CSC-TEST-002729;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-set-table-type-enforcement --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_e2e")
  add_executable(sbsql_sblr_alignment_ia08_database_serialize_logical_snapshot_runtime "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia08_database_serialize_logical_snapshot_runtime_test.cpp")
  add_executable(sbsql_sblr_alignment_ia08_database_deserialize_logical_snapshot_runtime "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia08_database_deserialize_logical_snapshot_runtime_test.cpp")
  add_executable(sbsql_sblr_alignment_ia08_ddl_create_macro_runtime "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia08_ddl_create_macro_runtime_test.cpp")
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_macro_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_macro_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_macro_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_macro_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002746")
  add_executable(sbsql_sblr_alignment_ia08_ddl_drop_macro_runtime "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia08_ddl_drop_macro_runtime_test.cpp")
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_macro_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_macro_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_macro_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_macro_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002750")
  add_executable(sbsql_sblr_alignment_ia08_ddl_drop_macro_coordinator "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia08_ddl_drop_macro_coordinator_test.cpp")
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_macro_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_macro_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_drop_macro_coordinator)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_macro_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002751;CSC-TEST-002752")
  add_executable(sbsql_sblr_alignment_ia08_admin_register_external_relation_resolver_runtime "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia08_admin_register_external_relation_resolver_runtime_test.cpp")
  target_link_libraries(sbsql_sblr_alignment_ia08_admin_register_external_relation_resolver_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_admin_register_external_relation_resolver_runtime COMMAND sbsql_sblr_alignment_ia08_admin_register_external_relation_resolver_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_admin_register_external_relation_resolver_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002754")
  add_executable(sbsql_sblr_alignment_ia08_admin_register_external_relation_resolver_coordinator "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia08_admin_register_external_relation_resolver_coordinator_test.cpp")
  target_link_libraries(sbsql_sblr_alignment_ia08_admin_register_external_relation_resolver_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_admin_register_external_relation_resolver_coordinator COMMAND sbsql_sblr_alignment_ia08_admin_register_external_relation_resolver_coordinator)
  set_tests_properties(sbsql_sblr_alignment_ia08_admin_register_external_relation_resolver_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002755;CSC-TEST-002756")
  add_executable(sbsql_sblr_alignment_ia08_admin_unregister_external_relation_resolver_runtime "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia08_admin_unregister_external_relation_resolver_runtime_test.cpp")
  target_link_libraries(sbsql_sblr_alignment_ia08_admin_unregister_external_relation_resolver_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_admin_unregister_external_relation_resolver_runtime COMMAND sbsql_sblr_alignment_ia08_admin_unregister_external_relation_resolver_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_admin_unregister_external_relation_resolver_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002758")
  add_executable(sbsql_sblr_alignment_ia08_admin_unregister_external_relation_resolver_coordinator "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia08_admin_unregister_external_relation_resolver_coordinator_test.cpp")
  target_link_libraries(sbsql_sblr_alignment_ia08_admin_unregister_external_relation_resolver_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_admin_unregister_external_relation_resolver_coordinator COMMAND sbsql_sblr_alignment_ia08_admin_unregister_external_relation_resolver_coordinator)
  set_tests_properties(sbsql_sblr_alignment_ia08_admin_unregister_external_relation_resolver_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002759;CSC-TEST-002760")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_dictionary_runtime "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia08_ddl_create_dictionary_runtime_test.cpp")
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_dictionary_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_dictionary_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_dictionary_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_dictionary_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002762")
  add_executable(sbsql_sblr_alignment_ia08_ddl_alter_dictionary_runtime "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia08_ddl_alter_dictionary_runtime_test.cpp")
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_dictionary_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_dictionary_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_alter_dictionary_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_dictionary_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_DICTIONARY;CSC-TEST-002766;malformed_operand")
  add_executable(sbsql_sblr_alignment_ia08_ddl_alter_dictionary_coordinator "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia08_ddl_alter_dictionary_coordinator_test.cpp")
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_dictionary_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_dictionary_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_alter_dictionary_coordinator)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_dictionary_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_DICTIONARY;CSC-TEST-002767;availability;CSC-TEST-002768;cancellation_fault")
  add_executable(sbsql_sblr_alignment_ia08_ddl_create_macro_coordinator "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia08_ddl_create_macro_coordinator_test.cpp")
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_macro_coordinator PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_macro_coordinator PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/engine/internal_api")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_macro_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_create_macro_coordinator)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_macro_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002747;CSC-TEST-002748")
  add_executable(sbsql_sblr_alignment_ia08_database_deserialize_logical_snapshot_coordinator "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia08_database_deserialize_logical_snapshot_coordinator_test.cpp")
  target_link_libraries(sbsql_sblr_alignment_ia08_database_deserialize_logical_snapshot_coordinator PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
  target_include_directories(sbsql_sblr_alignment_ia08_database_deserialize_logical_snapshot_coordinator PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/engine/internal_api")
  add_test(NAME sbsql_sblr_alignment_ia08_database_deserialize_logical_snapshot_coordinator COMMAND sbsql_sblr_alignment_ia08_database_deserialize_logical_snapshot_coordinator)
  set_tests_properties(sbsql_sblr_alignment_ia08_database_deserialize_logical_snapshot_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002743;CSC-TEST-002744")
  target_link_libraries(sbsql_sblr_alignment_ia08_database_deserialize_logical_snapshot_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_database_deserialize_logical_snapshot_runtime COMMAND sbsql_sblr_alignment_ia08_database_deserialize_logical_snapshot_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_database_deserialize_logical_snapshot_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002742")
  target_link_libraries(sbsql_sblr_alignment_ia08_database_serialize_logical_snapshot_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_database_serialize_logical_snapshot_runtime COMMAND sbsql_sblr_alignment_ia08_database_serialize_logical_snapshot_runtime)
  add_executable(sbsql_sblr_alignment_ia08_database_serialize_logical_snapshot_coordinator "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia08_database_serialize_logical_snapshot_coordinator_test.cpp")
  target_link_libraries(sbsql_sblr_alignment_ia08_database_serialize_logical_snapshot_coordinator PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
  target_include_directories(sbsql_sblr_alignment_ia08_database_serialize_logical_snapshot_coordinator PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/engine/internal_api")
  add_test(NAME sbsql_sblr_alignment_ia08_database_serialize_logical_snapshot_coordinator COMMAND sbsql_sblr_alignment_ia08_database_serialize_logical_snapshot_coordinator)
  set_tests_properties(sbsql_sblr_alignment_ia08_database_serialize_logical_snapshot_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002739;CSC-TEST-002740")
  set_tests_properties(sbsql_sblr_alignment_ia08_database_serialize_logical_snapshot_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002738")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_SET_TABLE_TYPE_ENFORCEMENT;CSC-TEST-002733;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_database_serialize_logical_snapshot_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation database-serialize-logical-snapshot --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_database_serialize_logical_snapshot_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_database_deserialize_logical_snapshot_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation database-deserialize-logical-snapshot --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_database_deserialize_logical_snapshot_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_database_deserialize_logical_snapshot_process_e2e PROPERTIES LABELS "ia08;CSC-TEST-002741;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_database_serialize_logical_snapshot_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;CSC-TEST-002737;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_REWRITE_RULE;CSC-TEST-002689;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_OR_REPLACE_SRS;CSC-TEST-002673;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_rename_object_vector_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_RENAME_OBJECT_VECTOR;CSC-TEST-002669;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_rename_object_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_RENAME_OBJECT;CSC-TEST-002933;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_synonym_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-synonym --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_synonym_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_foreign_table_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-foreign-table --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_foreign_table_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_fdw_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-fdw --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_fdw_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_fdw_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-fdw --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_fdw_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_synonym_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-synonym --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_synonym_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-foreign-table --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_synonym_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_SYNONYM;CSC-TEST-002941;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_foreign_table_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_FOREIGN_TABLE;CSC-TEST-002949;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_fdw_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_FDW;CSC-TEST-002957;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_fdw_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_FDW;CSC-TEST-002961;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_FOREIGN_TABLE;CSC-TEST-002953;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_process_e2e PROPERTIES LABELS "SBLR_DDL_DROP_TEMPORARY_TABLE;CSC-TEST-002665")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_temporary_table_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TEMPORARY_TABLE;CSC-TEST-002661;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_domain_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_DOMAIN;CSC-TEST-002589" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_schema_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-schema --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_schema_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_schema_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_SCHEMA;CSC-TEST-002593" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_table_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-table --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_table_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_table_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TABLE;CSC-TEST-002597" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_sequence_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-sequence --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_sequence_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_sequence_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_SEQUENCE;CSC-TEST-002897;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_table_as_query_with_data_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-table-as-query-with-data --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ctas_with_data_e2e")
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_materialized_view_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-materialized-view --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_create_materialized_view_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_materialized_view_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_MATERIALIZED_VIEW;CSC-TEST-002909;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_table_as_query_with_no_data_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-table-as-query-with-no-data --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ctas_with_no_data_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_table_as_query_with_data_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TABLE_AS_QUERY;CSC-TEST-002823;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_table_as_query_with_no_data_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TABLE_AS_QUERY;CSC-TEST-002824;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_index_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-index --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_index_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_index_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_INDEX;CSC-TEST-002601" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_index_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-index --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_index_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_index_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_INDEX;CSC-TEST-002605" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_alter_gpu_profile_disable_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation alter-gpu-profile-disable --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_alter_gpu_profile_disable_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_alter_gpu_profile_disable_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DIAGNOSTIC_REFUSAL;alter.gpu.profile_disable;CSC-TEST-002913;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia08_system_config_set_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SYSTEM_CONFIG_SET;CSC-TEST-002585" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_timeseries_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_TIMESERIES;CSC-TEST-002581" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_stream_append_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_STREAM_APPEND;CSC-TEST-002577" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_stream_read_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_STREAM_READ;CSC-TEST-002573")
  set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_scan_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_SCAN;CSC-TEST-002569")
  set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_read_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-07;SBLR_KV_STRUCTURED_READ;CSC-TEST-002561;e2e" TIMEOUT 240)
  set_tests_properties(sbsql_sblr_alignment_ia06_sequence_currval_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-06;SBLR_SEQUENCE_CURRVAL;CSC-TEST-002517;e2e" TIMEOUT 240)
 endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sbsql_sblr_alignment_register_ia07_table_truncate_process_e2e)

add_executable(sbsql_sblr_alignment_ia01_source_map_process_client
  ia01_source_map_process_client.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_source_map_process_client
  PRIVATE cxx_std_23)
target_link_libraries(sbsql_sblr_alignment_ia01_source_map_process_client
  PRIVATE sbl_sbsql_parser_worker_core)

add_executable(sbsql_sblr_alignment_ia01_parameter_process_client
  ia01_parameter_process_client.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_parameter_process_client
  PRIVATE cxx_std_20)
target_link_libraries(sbsql_sblr_alignment_ia01_parameter_process_client
  PRIVATE scratchbird_client)

add_executable(sbsql_sblr_alignment_ia01_parameter_value_carrier
  ia01_parameter_value_carrier_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_parameter_value_carrier
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_parameter_value_carrier
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_parameter_value_carrier
  PRIVATE sbl_sbsql_parser_worker_core)
add_test(NAME sbsql_sblr_alignment_ia01_parameter_value_carrier
  COMMAND sbsql_sblr_alignment_ia01_parameter_value_carrier)
set_tests_properties(sbsql_sblr_alignment_ia01_parameter_value_carrier PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-01;SBLR_PARAMETER;value_carrier;CSC-TEST-002330"
  TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia01_variable_runtime
  ia01_variable_runtime_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_variable_runtime
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_variable_runtime
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_variable_runtime
  PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_variable_runtime
  COMMAND sbsql_sblr_alignment_ia01_variable_runtime)
set_tests_properties(sbsql_sblr_alignment_ia01_variable_runtime PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-01;SBLR_VARIABLE;malformed_operand;CSC-TEST-002334"
  TIMEOUT 60)

function(sbsql_sblr_alignment_register_ia01_variable_process_e2e)
  if(TARGET sb_server AND TARGET sb_listener AND TARGET sbp_sbsql AND
     TARGET sb_isql)
    add_test(NAME sbsql_sblr_alignment_ia01_variable_process_e2e
      COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_variable_process_e2e.py"
        --server $<TARGET_FILE:sb_server>
        --listener $<TARGET_FILE:sb_listener>
        --parser-worker $<TARGET_FILE:sbp_sbsql>
        --sb-isql $<TARGET_FILE:sb_isql>
        --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia01_variable_e2e")
    set_tests_properties(sbsql_sblr_alignment_ia01_variable_process_e2e PROPERTIES
      LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_e2e;IA-01;SBLR_VARIABLE;CSC-TEST-002333;e2e"
      TIMEOUT 240)
  endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
  CALL sbsql_sblr_alignment_register_ia01_variable_process_e2e)

foreach(variable_registry_test IN ITEMS descriptor_registry frame_coordinator)
  add_executable(sbsql_sblr_alignment_ia01_variable_${variable_registry_test}
    ia01_variable_${variable_registry_test}_test.cpp)
  target_compile_features(
    sbsql_sblr_alignment_ia01_variable_${variable_registry_test}
    PRIVATE cxx_std_20)
  target_include_directories(
    sbsql_sblr_alignment_ia01_variable_${variable_registry_test}
    PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(
    sbsql_sblr_alignment_ia01_variable_${variable_registry_test}
    PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia01_variable_${variable_registry_test}
    COMMAND sbsql_sblr_alignment_ia01_variable_${variable_registry_test})
  set_tests_properties(
    sbsql_sblr_alignment_ia01_variable_${variable_registry_test} PROPERTIES
    LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;sbsql_sblr_alignment_recovery;IA-01;SBLR_VARIABLE;CSC-TEST-002335"
    TIMEOUT 60)
endforeach()

add_executable(sbsql_sblr_alignment_ia01_variable_executor_availability
  ia01_variable_executor_availability_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_variable_executor_availability
  PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_variable_executor_availability
  PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_variable_executor_availability
  PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia01_variable_executor_availability
  COMMAND sbsql_sblr_alignment_ia01_variable_executor_availability)
set_tests_properties(sbsql_sblr_alignment_ia01_variable_executor_availability PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;IA-01;SBLR_VARIABLE;missing_executor_evidence;CSC-TEST-002335"
  TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia01_variable_cancellation_fault
  ia01_variable_cancellation_fault_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_variable_cancellation_fault PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_variable_cancellation_fault PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sbsql_sblr_alignment_ia01_variable_cancellation_fault PRIVATE
  sb_server_core sb_engine sb_engine_internal_api sb_engine_sblr
  sb_sblr_parameter_wire_codec sb_storage_database sb_transaction_mga sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia01_variable_cancellation_fault
  COMMAND sbsql_sblr_alignment_ia01_variable_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia01_variable_cancellation_fault PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fault;sbsql_sblr_alignment_integration;IA-01;SBLR_VARIABLE;CSC-TEST-002336"
  TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia01_source_map_runtime ia01_source_map_runtime_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_source_map_runtime PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_source_map_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(sbsql_sblr_alignment_ia01_source_map_runtime PRIVATE sb_sblr_parameter_wire_codec sb_core_hash)
add_test(NAME sbsql_sblr_alignment_ia01_source_map_runtime COMMAND sbsql_sblr_alignment_ia01_source_map_runtime)
set_tests_properties(sbsql_sblr_alignment_ia01_source_map_runtime PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-01;SBLR_SOURCE_MAP;CSC-TEST-002338;malformed_operand" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia01_source_map_registry ia01_source_map_registry_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_source_map_registry PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_source_map_registry PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_source_map_registry PRIVATE
  sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_source_map_registry COMMAND sbsql_sblr_alignment_ia01_source_map_registry)
set_tests_properties(sbsql_sblr_alignment_ia01_source_map_registry PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;sbsql_sblr_alignment_recovery;IA-01;SBLR_SOURCE_MAP;CSC-TEST-002337" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia01_source_map_executor_availability ia01_source_map_executor_availability_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_source_map_executor_availability PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_source_map_executor_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_source_map_executor_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia01_source_map_executor_availability COMMAND sbsql_sblr_alignment_ia01_source_map_executor_availability)
set_tests_properties(sbsql_sblr_alignment_ia01_source_map_executor_availability PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;IA-01;SBLR_SOURCE_MAP;CSC-TEST-002339;missing_executor_evidence" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia01_source_map_cancellation_fault ia01_source_map_cancellation_fault_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_source_map_cancellation_fault PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_source_map_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_source_map_cancellation_fault PRIVATE sb_engine sb_server_core sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_source_map_cancellation_fault COMMAND sbsql_sblr_alignment_ia01_source_map_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia01_source_map_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fault;IA-01;SBLR_SOURCE_MAP;CSC-TEST-002340" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia01_error_vector_runtime ia01_error_vector_runtime_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_error_vector_runtime PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_error_vector_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(sbsql_sblr_alignment_ia01_error_vector_runtime PRIVATE sb_sblr_parameter_wire_codec sb_core_hash)
add_test(NAME sbsql_sblr_alignment_ia01_error_vector_runtime COMMAND sbsql_sblr_alignment_ia01_error_vector_runtime)
set_tests_properties(sbsql_sblr_alignment_ia01_error_vector_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-01;SBLR_ERROR_VECTOR;CSC-TEST-002342;malformed_operand" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia01_error_vector_executor_availability ia01_error_vector_executor_availability_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_error_vector_executor_availability PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_error_vector_executor_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_error_vector_executor_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia01_error_vector_executor_availability COMMAND sbsql_sblr_alignment_ia01_error_vector_executor_availability)
set_tests_properties(sbsql_sblr_alignment_ia01_error_vector_executor_availability PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;IA-01;SBLR_ERROR_VECTOR;CSC-TEST-002343;missing_executor_evidence" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia01_error_vector_registry ia01_error_vector_registry_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_error_vector_registry PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_error_vector_registry PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_error_vector_registry PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_error_vector_registry COMMAND sbsql_sblr_alignment_ia01_error_vector_registry)
set_tests_properties(sbsql_sblr_alignment_ia01_error_vector_registry PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;sbsql_sblr_alignment_recovery;IA-01;SBLR_ERROR_VECTOR" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia01_error_vector_cancellation_fault ia01_error_vector_cancellation_fault_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia01_error_vector_cancellation_fault PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia01_error_vector_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_error_vector_cancellation_fault PRIVATE sb_engine sb_server_core sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_error_vector_cancellation_fault COMMAND sbsql_sblr_alignment_ia01_error_vector_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia01_error_vector_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fault;IA-01;SBLR_ERROR_VECTOR;CSC-TEST-002344" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia05_transaction_begin_runtime ia05_transaction_begin_runtime_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia05_transaction_begin_runtime PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_begin_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_begin_runtime PRIVATE sb_sblr_parameter_wire_codec sb_core_hash sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_begin_runtime COMMAND sbsql_sblr_alignment_ia05_transaction_begin_runtime)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_begin_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-05;SBLR_TXN_BEGIN;CSC-TEST-002346;malformed_operand" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia05_transaction_begin_executor_availability ia05_transaction_begin_executor_availability_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia05_transaction_begin_executor_availability PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_begin_executor_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_begin_executor_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_begin_executor_availability COMMAND sbsql_sblr_alignment_ia05_transaction_begin_executor_availability)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_begin_executor_availability PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;IA-05;SBLR_TXN_BEGIN;CSC-TEST-002347;missing_executor_evidence" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia05_transaction_begin_cancellation_fault ia05_transaction_begin_cancellation_fault_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia05_transaction_begin_cancellation_fault PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_begin_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_begin_cancellation_fault PRIVATE sb_engine_sblr sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_begin_cancellation_fault COMMAND sbsql_sblr_alignment_ia05_transaction_begin_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_begin_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fault;IA-05;SBLR_TXN_BEGIN;CSC-TEST-002348;cancellation_atomicity" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia05_transaction_commit_runtime ia05_transaction_commit_runtime_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia05_transaction_commit_runtime PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_commit_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_commit_runtime PRIVATE sb_sblr_parameter_wire_codec sb_core_hash)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_commit_runtime COMMAND sbsql_sblr_alignment_ia05_transaction_commit_runtime)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_commit_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-05;SBLR_TXN_COMMIT;CSC-TEST-002350;malformed_operand" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia05_transaction_commit_executor_availability ia05_transaction_commit_executor_availability_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia05_transaction_commit_executor_availability PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_commit_executor_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_commit_executor_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_commit_executor_availability COMMAND sbsql_sblr_alignment_ia05_transaction_commit_executor_availability)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_commit_executor_availability PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;IA-05;SBLR_TXN_COMMIT;CSC-TEST-002351;missing_executor_evidence" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia05_transaction_commit_cancellation_fault ia05_transaction_commit_cancellation_fault_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia05_transaction_commit_cancellation_fault PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_commit_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_commit_cancellation_fault PRIVATE sb_engine_sblr sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_commit_cancellation_fault COMMAND sbsql_sblr_alignment_ia05_transaction_commit_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_commit_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fault;IA-05;SBLR_TXN_COMMIT;CSC-TEST-002352;cancellation_atomicity" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia05_transaction_rollback_runtime ia05_transaction_rollback_runtime_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia05_transaction_rollback_runtime PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_rollback_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_rollback_runtime PRIVATE sb_sblr_parameter_wire_codec sb_core_hash)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_rollback_runtime COMMAND sbsql_sblr_alignment_ia05_transaction_rollback_runtime)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_rollback_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-05;SBLR_TXN_ROLLBACK;CSC-TEST-002354;malformed_operand" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia05_transaction_rollback_executor_availability ia05_transaction_rollback_executor_availability_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia05_transaction_rollback_executor_availability PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_rollback_executor_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_rollback_executor_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_rollback_executor_availability COMMAND sbsql_sblr_alignment_ia05_transaction_rollback_executor_availability)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_rollback_executor_availability PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;IA-05;SBLR_TXN_ROLLBACK;CSC-TEST-002355;missing_executor_evidence" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia05_transaction_rollback_cancellation_fault ia05_transaction_rollback_cancellation_fault_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia05_transaction_rollback_cancellation_fault PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_rollback_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_rollback_cancellation_fault PRIVATE sb_engine_sblr sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_rollback_cancellation_fault COMMAND sbsql_sblr_alignment_ia05_transaction_rollback_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_rollback_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fault;IA-05;SBLR_TXN_ROLLBACK;CSC-TEST-002356;cancellation_atomicity" TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia05_transaction_savepoint_runtime
  ia05_transaction_savepoint_runtime_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia05_transaction_savepoint_runtime PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_savepoint_runtime PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_savepoint_runtime PRIVATE
  sb_sblr_parameter_wire_codec sb_core_hash)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_savepoint_runtime
  COMMAND sbsql_sblr_alignment_ia05_transaction_savepoint_runtime)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_savepoint_runtime PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-05;SBLR_TXN_SAVEPOINT;CSC-TEST-002358;malformed_operand"
  TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia05_transaction_release_savepoint_runtime
  ia05_transaction_release_savepoint_runtime_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia05_transaction_release_savepoint_runtime PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_release_savepoint_runtime PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_release_savepoint_runtime PRIVATE
  sb_sblr_parameter_wire_codec sb_core_hash)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_release_savepoint_runtime
  COMMAND sbsql_sblr_alignment_ia05_transaction_release_savepoint_runtime)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_release_savepoint_runtime PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-05;SBLR_TXN_RELEASE_SAVEPOINT;CSC-TEST-002362;malformed_operand"
  TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia05_transaction_release_savepoint_availability ia05_transaction_release_savepoint_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_release_savepoint_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/engine/internal_api)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_release_savepoint_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_release_savepoint_availability COMMAND sbsql_sblr_alignment_ia05_transaction_release_savepoint_availability)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_release_savepoint_availability PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;IA-05;SBLR_TXN_RELEASE_SAVEPOINT;CSC-TEST-002363;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia05_transaction_release_savepoint_cancellation_fault ia05_transaction_release_savepoint_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_release_savepoint_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_release_savepoint_cancellation_fault PRIVATE sb_engine_sblr sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_release_savepoint_cancellation_fault COMMAND sbsql_sblr_alignment_ia05_transaction_release_savepoint_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_release_savepoint_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fault;IA-05;SBLR_TXN_RELEASE_SAVEPOINT;CSC-TEST-002364;cancellation_atomicity")
foreach(pair IN ITEMS "runtime;ia05_transaction_rollback_to_savepoint_runtime_test.cpp" "availability;ia05_transaction_rollback_to_savepoint_availability_test.cpp" "cancellation_fault;ia05_transaction_rollback_to_savepoint_cancellation_fault_test.cpp")
  list(GET pair 0 suffix)
  list(GET pair 1 source)
  add_executable(sbsql_sblr_alignment_ia05_transaction_rollback_to_savepoint_${suffix} ${source})
  target_include_directories(sbsql_sblr_alignment_ia05_transaction_rollback_to_savepoint_${suffix} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/engine/internal_api)
endforeach()
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_rollback_to_savepoint_runtime PRIVATE sb_sblr_parameter_wire_codec)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_rollback_to_savepoint_availability PRIVATE sb_engine_internal_api sb_core_uuid)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_rollback_to_savepoint_cancellation_fault PRIVATE sb_engine_sblr sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_rollback_to_savepoint_runtime COMMAND sbsql_sblr_alignment_ia05_transaction_rollback_to_savepoint_runtime)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_rollback_to_savepoint_availability COMMAND sbsql_sblr_alignment_ia05_transaction_rollback_to_savepoint_availability)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_rollback_to_savepoint_cancellation_fault COMMAND sbsql_sblr_alignment_ia05_transaction_rollback_to_savepoint_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_rollback_to_savepoint_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-05;SBLR_TXN_ROLLBACK_TO_SAVEPOINT;CSC-TEST-002366;malformed_operand")
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_rollback_to_savepoint_availability PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;IA-05;SBLR_TXN_ROLLBACK_TO_SAVEPOINT;CSC-TEST-002367;missing_executor_evidence")
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_rollback_to_savepoint_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fault;IA-05;SBLR_TXN_ROLLBACK_TO_SAVEPOINT;CSC-TEST-002368;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia05_psql_autonomous_frame_runtime ia05_psql_autonomous_frame_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia05_psql_autonomous_frame_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_psql_autonomous_frame_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia05_psql_autonomous_frame_runtime COMMAND sbsql_sblr_alignment_ia05_psql_autonomous_frame_runtime)
set_tests_properties(sbsql_sblr_alignment_ia05_psql_autonomous_frame_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-05;SBLR_PSQL_AUTONOMOUS_FRAME;CSC-TEST-002370;malformed_operand")
add_executable(sbsql_sblr_alignment_ia05_psql_autonomous_frame_coordinator ia05_psql_autonomous_frame_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia05_psql_autonomous_frame_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_psql_autonomous_frame_coordinator PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia05_psql_autonomous_frame_coordinator COMMAND sbsql_sblr_alignment_ia05_psql_autonomous_frame_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia05_psql_autonomous_frame_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;IA-05;SBLR_PSQL_AUTONOMOUS_FRAME;CSC-TEST-002371;missing_executor_evidence" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia05_psql_autonomous_frame_cancellation_fault ia05_psql_autonomous_frame_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia05_psql_autonomous_frame_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_psql_autonomous_frame_cancellation_fault PRIVATE sb_engine_sblr sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia05_psql_autonomous_frame_cancellation_fault COMMAND sbsql_sblr_alignment_ia05_psql_autonomous_frame_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia05_psql_autonomous_frame_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fault;IA-05;SBLR_PSQL_AUTONOMOUS_FRAME;CSC-TEST-002372;cancellation_atomicity" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia05_transaction_reservation_release_runtime ia05_transaction_reservation_release_runtime_test.cpp)
add_executable(sbsql_sblr_alignment_ia05_temporary_instance_cleanup_runtime ia05_temporary_instance_cleanup_runtime_test.cpp)
add_executable(sbsql_sblr_alignment_ia06_cursor_open_runtime ia06_cursor_open_runtime_test.cpp)
add_executable(sbsql_sblr_alignment_ia06_cursor_fetch_runtime ia06_cursor_fetch_runtime_test.cpp)
add_executable(sbsql_sblr_alignment_ia06_cursor_close_runtime ia06_cursor_close_runtime_test.cpp)
add_executable(sbsql_sblr_alignment_ia06_read_by_key_runtime ia06_read_by_key_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_read_by_key_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_read_by_key_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_read_by_key_runtime COMMAND sbsql_sblr_alignment_ia06_read_by_key_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_read_by_key_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-06;SBLR_READ_BY_KEY;CSC-TEST-002394;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_read_range_runtime ia06_read_range_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_read_range_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_read_range_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_read_range_runtime COMMAND sbsql_sblr_alignment_ia06_read_range_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_read_range_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-06;SBLR_READ_RANGE;CSC-TEST-002398;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_read_range_availability ia06_read_range_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_read_range_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_read_range_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia06_read_range_availability COMMAND sbsql_sblr_alignment_ia06_read_range_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_read_range_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_READ_RANGE;CSC-TEST-002399;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_read_range_cancellation_fault ia06_read_range_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_read_range_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_read_range_cancellation_fault PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia06_read_range_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_read_range_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_read_range_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_READ_RANGE;CSC-TEST-002400;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_read_stream_runtime ia06_read_stream_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_read_stream_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_read_stream_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_read_stream_runtime COMMAND sbsql_sblr_alignment_ia06_read_stream_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_read_stream_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-06;SBLR_READ_STREAM;CSC-TEST-002402;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_read_stream_availability ia06_read_stream_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_read_stream_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_read_stream_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia06_read_stream_availability COMMAND sbsql_sblr_alignment_ia06_read_stream_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_read_stream_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_READ_STREAM;CSC-TEST-002403;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_read_stream_cancellation_fault ia06_read_stream_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_read_stream_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_read_stream_cancellation_fault PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia06_read_stream_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_read_stream_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_read_stream_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_READ_STREAM;CSC-TEST-002404;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_result_set_pass_runtime ia06_result_set_pass_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_result_set_pass_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_result_set_pass_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_result_set_pass_runtime COMMAND sbsql_sblr_alignment_ia06_result_set_pass_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_result_set_pass_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-06;SBLR_RESULT_SET_PASS;CSC-TEST-002406;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_result_set_pass_availability ia06_result_set_pass_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_result_set_pass_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_result_set_pass_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia06_result_set_pass_availability COMMAND sbsql_sblr_alignment_ia06_result_set_pass_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_result_set_pass_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_RESULT_SET_PASS;CSC-TEST-002407;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_result_set_pass_cancellation_fault ia06_result_set_pass_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_result_set_pass_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_result_set_pass_cancellation_fault PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia06_result_set_pass_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_result_set_pass_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_result_set_pass_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_RESULT_SET_PASS;CSC-TEST-002408;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_access_cursor_open_runtime ia06_access_cursor_open_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_access_cursor_open_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_access_cursor_open_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_access_cursor_open_runtime COMMAND sbsql_sblr_alignment_ia06_access_cursor_open_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_access_cursor_open_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-06;SBLR_ACCESS_CURSOR_OPEN;CSC-TEST-002410;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_access_cursor_open_availability ia06_access_cursor_open_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_access_cursor_open_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_access_cursor_open_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia06_access_cursor_open_availability COMMAND sbsql_sblr_alignment_ia06_access_cursor_open_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_access_cursor_open_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_ACCESS_CURSOR_OPEN;CSC-TEST-002411;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_access_cursor_open_cancellation_fault ia06_access_cursor_open_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_access_cursor_open_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_access_cursor_open_cancellation_fault PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia06_access_cursor_open_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_access_cursor_open_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_access_cursor_open_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_ACCESS_CURSOR_OPEN;CSC-TEST-002412;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_access_cursor_fetch_runtime ia06_access_cursor_fetch_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_access_cursor_fetch_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_access_cursor_fetch_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_access_cursor_fetch_runtime COMMAND sbsql_sblr_alignment_ia06_access_cursor_fetch_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_access_cursor_fetch_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-06;SBLR_ACCESS_CURSOR_FETCH;CSC-TEST-002414;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_access_cursor_fetch_availability ia06_access_cursor_fetch_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_access_cursor_fetch_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_access_cursor_fetch_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia06_access_cursor_fetch_availability COMMAND sbsql_sblr_alignment_ia06_access_cursor_fetch_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_access_cursor_fetch_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_ACCESS_CURSOR_FETCH;CSC-TEST-002415;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_access_cursor_fetch_cancellation_fault ia06_access_cursor_fetch_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_access_cursor_fetch_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_access_cursor_fetch_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_access_cursor_fetch_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_access_cursor_fetch_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_access_cursor_fetch_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_ACCESS_CURSOR_FETCH;CSC-TEST-002416;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_access_cursor_close_runtime ia06_access_cursor_close_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_access_cursor_close_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_access_cursor_close_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_access_cursor_close_runtime COMMAND sbsql_sblr_alignment_ia06_access_cursor_close_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_access_cursor_close_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-06;SBLR_ACCESS_CURSOR_CLOSE;CSC-TEST-002418;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_access_cursor_close_availability ia06_access_cursor_close_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_access_cursor_close_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_access_cursor_close_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia06_access_cursor_close_availability COMMAND sbsql_sblr_alignment_ia06_access_cursor_close_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_access_cursor_close_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_ACCESS_CURSOR_CLOSE;CSC-TEST-002419;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_access_cursor_close_cancellation_fault ia06_access_cursor_close_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_access_cursor_close_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_access_cursor_close_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_access_cursor_close_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_access_cursor_close_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_access_cursor_close_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_ACCESS_CURSOR_CLOSE;CSC-TEST-002420;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_insert_runtime ia07_insert_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_insert_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_insert_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_insert_runtime COMMAND sbsql_sblr_alignment_ia07_insert_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_insert_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-07;SBLR_INSERT;CSC-TEST-002422;malformed_operand")
add_executable(sbsql_sblr_alignment_ia07_insert_availability ia07_insert_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_insert_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_insert_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia07_insert_availability COMMAND sbsql_sblr_alignment_ia07_insert_availability)
set_tests_properties(sbsql_sblr_alignment_ia07_insert_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_INSERT;CSC-TEST-002423;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia07_insert_cancellation_fault ia07_insert_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_insert_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_insert_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_insert_cancellation_fault COMMAND sbsql_sblr_alignment_ia07_insert_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia07_insert_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_INSERT;CSC-TEST-002424;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_update_runtime ia07_update_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_update_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_update_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_update_runtime COMMAND sbsql_sblr_alignment_ia07_update_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_update_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-07;SBLR_UPDATE;CSC-TEST-002426;malformed_operand")
add_executable(sbsql_sblr_alignment_ia07_update_availability ia07_update_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_update_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_update_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia07_update_availability COMMAND sbsql_sblr_alignment_ia07_update_availability)
set_tests_properties(sbsql_sblr_alignment_ia07_update_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_UPDATE;CSC-TEST-002427;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia07_update_cancellation_fault ia07_update_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_update_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_update_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_update_cancellation_fault COMMAND sbsql_sblr_alignment_ia07_update_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia07_update_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_UPDATE;CSC-TEST-002428;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_delete_runtime ia07_delete_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_delete_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_delete_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_delete_runtime COMMAND sbsql_sblr_alignment_ia07_delete_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_delete_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-07;SBLR_DELETE;CSC-TEST-002430;malformed_operand")
add_executable(sbsql_sblr_alignment_ia07_merge_runtime ia07_merge_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_merge_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_merge_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_merge_runtime COMMAND sbsql_sblr_alignment_ia07_merge_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_merge_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-07;SBLR_MERGE;CSC-TEST-002434;malformed_operand")
add_executable(sbsql_sblr_alignment_ia07_merge_availability ia07_merge_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_merge_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_merge_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia07_merge_availability COMMAND sbsql_sblr_alignment_ia07_merge_availability)
set_tests_properties(sbsql_sblr_alignment_ia07_merge_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_MERGE;CSC-TEST-002435;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia07_merge_cancellation_fault ia07_merge_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_merge_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_merge_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_merge_cancellation_fault COMMAND sbsql_sblr_alignment_ia07_merge_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia07_merge_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_MERGE;CSC-TEST-002436;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_table_truncate_runtime ia07_table_truncate_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_table_truncate_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_table_truncate_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_table_truncate_runtime COMMAND sbsql_sblr_alignment_ia07_table_truncate_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_table_truncate_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-07;SBLR_TABLE_TRUNCATE;CSC-TEST-002442;malformed_operand")
add_executable(sbsql_sblr_alignment_ia07_table_truncate_availability ia07_table_truncate_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_table_truncate_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_table_truncate_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia07_table_truncate_availability COMMAND sbsql_sblr_alignment_ia07_table_truncate_availability)
set_tests_properties(sbsql_sblr_alignment_ia07_table_truncate_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_TABLE_TRUNCATE;CSC-TEST-002443;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia07_table_truncate_cancellation_fault ia07_table_truncate_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_table_truncate_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_table_truncate_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_table_truncate_cancellation_fault COMMAND sbsql_sblr_alignment_ia07_table_truncate_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia07_table_truncate_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_TABLE_TRUNCATE;CSC-TEST-002444;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_table_analyze_runtime ia07_table_analyze_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_table_analyze_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_table_analyze_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_table_analyze_runtime COMMAND sbsql_sblr_alignment_ia07_table_analyze_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_table_analyze_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-07;SBLR_TABLE_ANALYZE;CSC-TEST-002446;malformed_operand")
add_executable(sbsql_sblr_alignment_ia07_table_analyze_availability ia07_table_analyze_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_table_analyze_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_table_analyze_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia07_table_analyze_availability COMMAND sbsql_sblr_alignment_ia07_table_analyze_availability)
set_tests_properties(sbsql_sblr_alignment_ia07_table_analyze_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_TABLE_ANALYZE;CSC-TEST-002447;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia07_table_analyze_cancellation_fault ia07_table_analyze_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_table_analyze_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_table_analyze_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_table_analyze_cancellation_fault COMMAND sbsql_sblr_alignment_ia07_table_analyze_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia07_table_analyze_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_TABLE_ANALYZE;CSC-TEST-002448;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_bulk_import_stream_runtime ia07_bulk_import_stream_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_bulk_import_stream_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_bulk_import_stream_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_bulk_import_stream_runtime COMMAND sbsql_sblr_alignment_ia07_bulk_import_stream_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_bulk_import_stream_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-07;SBLR_BULK_IMPORT_STREAM;CSC-TEST-002450;malformed_operand")
add_executable(sbsql_sblr_alignment_ia07_bulk_import_stream_availability ia07_bulk_import_stream_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_bulk_import_stream_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_bulk_import_stream_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia07_bulk_import_stream_availability COMMAND sbsql_sblr_alignment_ia07_bulk_import_stream_availability)
set_tests_properties(sbsql_sblr_alignment_ia07_bulk_import_stream_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_BULK_IMPORT_STREAM;CSC-TEST-002451;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia07_bulk_import_stream_cancellation_fault ia07_bulk_import_stream_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_bulk_import_stream_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_bulk_import_stream_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_bulk_import_stream_cancellation_fault COMMAND sbsql_sblr_alignment_ia07_bulk_import_stream_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia07_bulk_import_stream_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_BULK_IMPORT_STREAM;CSC-TEST-002452;cancellation_atomicity")
foreach(k runtime availability cancellation_fault)
 add_executable(sbsql_sblr_alignment_ia07_bulk_export_stream_${k} ia07_bulk_export_stream_${k}_test.cpp)
 target_include_directories(sbsql_sblr_alignment_ia07_bulk_export_stream_${k} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
 target_link_libraries(sbsql_sblr_alignment_ia07_bulk_export_stream_${k} PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
 add_test(NAME sbsql_sblr_alignment_ia07_bulk_export_stream_${k} COMMAND sbsql_sblr_alignment_ia07_bulk_export_stream_${k})
endforeach()
set_tests_properties(sbsql_sblr_alignment_ia07_bulk_export_stream_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_BULK_EXPORT_STREAM;CSC-TEST-002454;malformed_operand")
set_tests_properties(sbsql_sblr_alignment_ia07_bulk_export_stream_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_BULK_EXPORT_STREAM;CSC-TEST-002455;missing_executor_evidence")
set_tests_properties(sbsql_sblr_alignment_ia07_bulk_export_stream_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_BULK_EXPORT_STREAM;CSC-TEST-002456;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_statement_batch_runtime ia07_statement_batch_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_statement_batch_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_statement_batch_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_statement_batch_runtime COMMAND sbsql_sblr_alignment_ia07_statement_batch_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_statement_batch_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_STATEMENT_BATCH;CSC-TEST-002458;malformed_operand")
foreach(k availability cancellation_fault)
 add_executable(sbsql_sblr_alignment_ia07_statement_batch_${k} ia07_statement_batch_${k}_test.cpp)
 target_include_directories(sbsql_sblr_alignment_ia07_statement_batch_${k} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
 target_link_libraries(sbsql_sblr_alignment_ia07_statement_batch_${k} PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
 add_test(NAME sbsql_sblr_alignment_ia07_statement_batch_${k} COMMAND sbsql_sblr_alignment_ia07_statement_batch_${k})
endforeach()
set_tests_properties(sbsql_sblr_alignment_ia07_statement_batch_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_STATEMENT_BATCH;CSC-TEST-002459;missing_executor_evidence")
set_tests_properties(sbsql_sblr_alignment_ia07_statement_batch_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_STATEMENT_BATCH;CSC-TEST-002460;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_atomic_cas_runtime ia07_atomic_cas_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_atomic_cas_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_atomic_cas_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_atomic_cas_runtime COMMAND sbsql_sblr_alignment_ia07_atomic_cas_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_atomic_cas_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_ATOMIC_CAS;CSC-TEST-002462;malformed_operand")

add_executable(sbsql_sblr_alignment_ia07_atomic_rmw_runtime ia07_atomic_rmw_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_atomic_rmw_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_atomic_rmw_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_atomic_rmw_runtime COMMAND sbsql_sblr_alignment_ia07_atomic_rmw_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_atomic_rmw_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_ATOMIC_READ_MODIFY_WRITE;CSC-TEST-002466;malformed_operand")

add_executable(sbsql_sblr_alignment_ia07_advisory_lock_runtime ia07_advisory_lock_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_advisory_lock_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_advisory_lock_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_advisory_lock_runtime COMMAND sbsql_sblr_alignment_ia07_advisory_lock_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_advisory_lock_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_ADVISORY_LOCK_ACQUIRE;CSC-TEST-002470;malformed_operand")
add_executable(sbsql_sblr_alignment_ia07_advisory_lock_release_runtime ia07_advisory_lock_release_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_advisory_lock_release_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_advisory_lock_release_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_advisory_lock_release_runtime COMMAND sbsql_sblr_alignment_ia07_advisory_lock_release_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_advisory_lock_release_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_ADVISORY_LOCK_RELEASE;CSC-TEST-002474;malformed_operand")

add_executable(sbsql_sblr_alignment_ia06_function_call_runtime ia06_function_call_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_function_call_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_function_call_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_function_call_runtime COMMAND sbsql_sblr_alignment_ia06_function_call_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_function_call_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_FUNCTION_CALL;CSC-TEST-002478;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_operator_call_runtime ia06_operator_call_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_operator_call_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_operator_call_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_operator_call_runtime COMMAND sbsql_sblr_alignment_ia06_operator_call_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_operator_call_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_OPERATOR_CALL;CSC-TEST-002482;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_cast_runtime ia06_cast_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_cast_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_cast_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_cast_runtime COMMAND sbsql_sblr_alignment_ia06_cast_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_cast_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_CAST;CSC-TEST-002486;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_compare_runtime ia06_compare_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_compare_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_compare_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_compare_runtime COMMAND sbsql_sblr_alignment_ia06_compare_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_compare_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_COMPARE;CSC-TEST-002490;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_domain_operation_runtime ia06_domain_operation_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_domain_operation_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_domain_operation_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_domain_operation_runtime COMMAND sbsql_sblr_alignment_ia06_domain_operation_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_domain_operation_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_DOMAIN_OPERATION;CSC-TEST-002494;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_udr_invoke_runtime ia06_udr_invoke_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_udr_invoke_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_udr_invoke_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_udr_invoke_runtime COMMAND sbsql_sblr_alignment_ia06_udr_invoke_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_udr_invoke_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_UDR_INVOKE;CSC-TEST-002498;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_procedure_invoke_runtime ia06_procedure_invoke_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_procedure_invoke_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_procedure_invoke_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_procedure_invoke_runtime COMMAND sbsql_sblr_alignment_ia06_procedure_invoke_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_procedure_invoke_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_PROCEDURE_INVOKE;CSC-TEST-002502;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_function_invoke_runtime ia06_function_invoke_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_function_invoke_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_function_invoke_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_function_invoke_runtime COMMAND sbsql_sblr_alignment_ia06_function_invoke_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_function_invoke_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_FUNCTION_INVOKE;CSC-TEST-002506;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_aggregate_invoke_runtime ia06_aggregate_invoke_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_aggregate_invoke_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_aggregate_invoke_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_aggregate_invoke_runtime COMMAND sbsql_sblr_alignment_ia06_aggregate_invoke_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_aggregate_invoke_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_AGGREGATE_INVOKE;CSC-TEST-002510;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_sequence_nextval_runtime ia06_sequence_nextval_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_sequence_nextval_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_sequence_nextval_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_sequence_nextval_runtime COMMAND sbsql_sblr_alignment_ia06_sequence_nextval_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_sequence_nextval_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_SEQUENCE_NEXTVAL;CSC-TEST-002514;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_sequence_setval_runtime ia06_sequence_setval_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_sequence_setval_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_sequence_setval_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_sequence_setval_runtime COMMAND sbsql_sblr_alignment_ia06_sequence_setval_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_sequence_setval_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_SEQUENCE_SETVAL;CSC-TEST-002522;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_query_numeric_runtime ia06_query_numeric_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_query_numeric_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_query_numeric_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_query_numeric_runtime COMMAND sbsql_sblr_alignment_ia06_query_numeric_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_query_numeric_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_QUERY_APPLY_NUMERIC_OPERATION;CSC-TEST-002526;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_advanced_datatype_family_runtime ia06_advanced_datatype_family_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_advanced_datatype_family_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_advanced_datatype_family_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_advanced_datatype_family_runtime COMMAND sbsql_sblr_alignment_ia06_advanced_datatype_family_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_advanced_datatype_family_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_QUERY_EVALUATE_ADVANCED_DATATYPE_FAMILY;CSC-TEST-002530;malformed_operand")
add_executable(sbsql_sblr_alignment_ia01_project_runtime ia01_project_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_project_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_project_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_project_runtime COMMAND sbsql_sblr_alignment_ia01_project_runtime)
set_tests_properties(sbsql_sblr_alignment_ia01_project_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_PROJECT;CSC-TEST-002534;malformed_operand")
add_executable(sbsql_sblr_alignment_ia01_aggregate_runtime ia01_aggregate_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_aggregate_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_aggregate_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_aggregate_runtime COMMAND sbsql_sblr_alignment_ia01_aggregate_runtime)
set_tests_properties(sbsql_sblr_alignment_ia01_aggregate_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_AGGREGATE;CSC-TEST-002538;malformed_operand")
add_executable(sbsql_sblr_alignment_ia01_group_runtime ia01_group_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_group_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_group_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_group_runtime COMMAND sbsql_sblr_alignment_ia01_group_runtime)
set_tests_properties(sbsql_sblr_alignment_ia01_group_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_GROUP;CSC-TEST-002542;malformed_operand")
add_executable(sbsql_sblr_alignment_ia01_group_availability ia01_group_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_group_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_group_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia01_group_availability COMMAND sbsql_sblr_alignment_ia01_group_availability)
set_tests_properties(sbsql_sblr_alignment_ia01_group_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_GROUP;CSC-TEST-002543;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia01_group_cancellation_fault ia01_group_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_group_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_group_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_group_cancellation_fault COMMAND sbsql_sblr_alignment_ia01_group_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia01_group_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_GROUP;CSC-TEST-002544;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia01_sort_runtime ia01_sort_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_sort_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_sort_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_sort_runtime COMMAND sbsql_sblr_alignment_ia01_sort_runtime)
set_tests_properties(sbsql_sblr_alignment_ia01_sort_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_SORT;CSC-TEST-002546;malformed_operand")

add_executable(sbsql_sblr_alignment_ia01_limit_runtime ia01_limit_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_limit_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_limit_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_limit_runtime COMMAND sbsql_sblr_alignment_ia01_limit_runtime)
set_tests_properties(sbsql_sblr_alignment_ia01_limit_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_LIMIT;CSC-TEST-002550;malformed_operand")
add_executable(sbsql_sblr_alignment_ia01_window_runtime ia01_window_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_window_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_window_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_window_runtime COMMAND sbsql_sblr_alignment_ia01_window_runtime)
set_tests_properties(sbsql_sblr_alignment_ia01_window_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_WINDOW;CSC-TEST-002554;malformed_operand")
add_executable(sbsql_sblr_alignment_ia01_return_result_set_runtime ia01_return_result_set_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_return_result_set_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_return_result_set_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_return_result_set_runtime COMMAND sbsql_sblr_alignment_ia01_return_result_set_runtime)
set_tests_properties(sbsql_sblr_alignment_ia01_return_result_set_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_RETURN_RESULT_SET;CSC-TEST-002558;malformed_operand")
add_executable(sbsql_sblr_alignment_ia01_return_result_set_availability ia01_return_result_set_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_return_result_set_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_return_result_set_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia01_return_result_set_availability COMMAND sbsql_sblr_alignment_ia01_return_result_set_availability)
set_tests_properties(sbsql_sblr_alignment_ia01_return_result_set_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_RETURN_RESULT_SET;CSC-TEST-002559;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia01_return_result_set_cancellation_fault ia01_return_result_set_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_return_result_set_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_return_result_set_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_return_result_set_cancellation_fault COMMAND sbsql_sblr_alignment_ia01_return_result_set_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia01_return_result_set_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_RETURN_RESULT_SET;CSC-TEST-002560;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_read_runtime ia01_kv_structured_read_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_read_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_read_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_read_runtime COMMAND sbsql_sblr_alignment_ia07_kv_structured_read_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_read_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_READ;CSC-TEST-002562;malformed_operand")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_read_availability ia07_kv_structured_read_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_read_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_read_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_read_availability COMMAND sbsql_sblr_alignment_ia07_kv_structured_read_availability)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_read_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_READ;CSC-TEST-002563;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_read_cancellation_fault ia07_kv_structured_read_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_read_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_read_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_read_cancellation_fault COMMAND sbsql_sblr_alignment_ia07_kv_structured_read_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_read_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_READ;CSC-TEST-002564;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_mutate_runtime ia07_kv_structured_mutate_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_mutate_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_mutate_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_mutate_runtime COMMAND sbsql_sblr_alignment_ia07_kv_structured_mutate_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_mutate_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_MUTATE;CSC-TEST-002566;malformed_operand")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_mutate_availability ia07_kv_structured_mutate_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_mutate_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_mutate_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_mutate_availability COMMAND sbsql_sblr_alignment_ia07_kv_structured_mutate_availability)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_mutate_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_MUTATE;CSC-TEST-002567;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_mutate_cancellation_fault ia07_kv_structured_mutate_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_mutate_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_mutate_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_mutate_cancellation_fault COMMAND sbsql_sblr_alignment_ia07_kv_structured_mutate_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_mutate_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_MUTATE;CSC-TEST-002568;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_scan_runtime ia07_kv_structured_scan_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_scan_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_scan_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_scan_runtime COMMAND sbsql_sblr_alignment_ia07_kv_structured_scan_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_scan_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_SCAN;CSC-TEST-002570;malformed_operand")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_scan_availability ia07_kv_structured_scan_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_scan_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_scan_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_scan_availability COMMAND sbsql_sblr_alignment_ia07_kv_structured_scan_availability)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_scan_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_SCAN;CSC-TEST-002571;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_scan_cancellation_fault ia07_kv_structured_scan_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_scan_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_scan_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_scan_cancellation_fault COMMAND sbsql_sblr_alignment_ia07_kv_structured_scan_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_scan_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_SCAN;CSC-TEST-002572;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_stream_read_runtime ia07_kv_structured_stream_read_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_stream_read_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_stream_read_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_stream_read_runtime COMMAND sbsql_sblr_alignment_ia07_kv_structured_stream_read_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_stream_read_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_STREAM_READ;CSC-TEST-002574;malformed_operand")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_stream_read_availability ia07_kv_structured_stream_read_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_stream_read_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_stream_read_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_stream_read_availability COMMAND sbsql_sblr_alignment_ia07_kv_structured_stream_read_availability)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_stream_read_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_STREAM_READ;CSC-TEST-002575;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_stream_read_cancellation_fault ia07_kv_structured_stream_read_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_stream_read_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_stream_read_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_stream_read_cancellation_fault COMMAND sbsql_sblr_alignment_ia07_kv_structured_stream_read_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_stream_read_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_STREAM_READ;CSC-TEST-002576;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_stream_append_runtime ia07_kv_structured_stream_append_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_stream_append_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_stream_append_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_stream_append_runtime COMMAND sbsql_sblr_alignment_ia07_kv_structured_stream_append_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_stream_append_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_STREAM_APPEND;CSC-TEST-002578;malformed_operand")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_stream_append_availability ia07_kv_structured_stream_append_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_stream_append_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_stream_append_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_stream_append_availability COMMAND sbsql_sblr_alignment_ia07_kv_structured_stream_append_availability)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_stream_append_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_STREAM_APPEND;CSC-TEST-002579;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_stream_append_cancellation_fault ia07_kv_structured_stream_append_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_stream_append_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_stream_append_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_stream_append_cancellation_fault COMMAND sbsql_sblr_alignment_ia07_kv_structured_stream_append_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_stream_append_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_STREAM_APPEND;CSC-TEST-002580;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_timeseries_runtime ia07_kv_structured_timeseries_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_timeseries_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_timeseries_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_timeseries_runtime COMMAND sbsql_sblr_alignment_ia07_kv_structured_timeseries_runtime)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_timeseries_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_TIMESERIES;CSC-TEST-002582;malformed_operand")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_timeseries_availability ia07_kv_structured_timeseries_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_timeseries_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_timeseries_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_timeseries_availability COMMAND sbsql_sblr_alignment_ia07_kv_structured_timeseries_availability)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_timeseries_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_TIMESERIES;CSC-TEST-002583;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia07_kv_structured_timeseries_cancellation_fault ia07_kv_structured_timeseries_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_kv_structured_timeseries_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_kv_structured_timeseries_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_kv_structured_timeseries_cancellation_fault COMMAND sbsql_sblr_alignment_ia07_kv_structured_timeseries_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia07_kv_structured_timeseries_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_KV_STRUCTURED_TIMESERIES;CSC-TEST-002584;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_system_config_set_runtime ia08_system_config_set_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_system_config_set_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_system_config_set_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_system_config_set_runtime COMMAND sbsql_sblr_alignment_ia08_system_config_set_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_system_config_set_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_SYSTEM_CONFIG_SET;CSC-TEST-002586;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_system_config_get_runtime ia08_system_config_get_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_system_config_get_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_system_config_get_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_system_config_get_runtime COMMAND sbsql_sblr_alignment_ia08_system_config_get_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_system_config_get_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_SYSTEM_CONFIG_GET;CSC-TEST-003081;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_system_config_get_availability ia08_system_config_get_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_system_config_get_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_system_config_get_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_system_config_get_availability COMMAND sbsql_sblr_alignment_ia08_system_config_get_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_system_config_get_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_SYSTEM_CONFIG_GET;CSC-TEST-003082;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_system_config_reset_runtime ia08_system_config_reset_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_system_config_reset_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_system_config_reset_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_system_config_reset_runtime COMMAND sbsql_sblr_alignment_ia08_system_config_reset_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_system_config_reset_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_SYSTEM_CONFIG_RESET;CSC-TEST-003082;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_system_config_reset_availability ia08_system_config_reset_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_system_config_reset_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_system_config_reset_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_system_config_reset_availability COMMAND sbsql_sblr_alignment_ia08_system_config_reset_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_system_config_reset_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_SYSTEM_CONFIG_RESET;CSC-TEST-003083;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_rule_runtime ia08_ddl_create_rule_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_rule_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_rule_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_rule_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_rule_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_rule_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_DDL_CREATE_RULE;CSC-TEST-003085;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_rule_availability ia08_ddl_create_rule_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_rule_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_rule_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_rule_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_rule_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_rule_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_DDL_CREATE_RULE;CSC-TEST-003086;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_rule_runtime ia08_ddl_drop_rule_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_rule_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_rule_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_rule_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_rule_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_rule_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_DDL_DROP_RULE;CSC-TEST-003088;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_rule_availability ia08_ddl_drop_rule_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_rule_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_rule_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_rule_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_rule_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_rule_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_DDL_DROP_RULE;CSC-TEST-003089;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_system_config_set_availability ia08_system_config_set_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_system_config_set_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_system_config_set_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_system_config_set_availability COMMAND sbsql_sblr_alignment_ia08_system_config_set_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_system_config_set_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_SYSTEM_CONFIG_SET;CSC-TEST-002587;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_system_config_set_cancellation_fault ia08_system_config_set_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_system_config_set_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_system_config_set_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_system_config_set_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_system_config_set_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_system_config_set_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_SYSTEM_CONFIG_SET;CSC-TEST-002588;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_domain_runtime ia08_ddl_create_domain_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_domain_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_domain_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_domain_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_domain_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_domain_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_DOMAIN;CSC-TEST-002590;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_domain_availability ia08_ddl_create_domain_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_domain_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_domain_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_domain_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_domain_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_domain_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_DOMAIN;CSC-TEST-002591;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_domain_cancellation_fault ia08_ddl_create_domain_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_domain_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_domain_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_domain_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_domain_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_domain_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_DOMAIN;CSC-TEST-002592;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_schema_runtime ia08_ddl_create_schema_runtime_test.cpp)
add_executable(sbsql_sblr_alignment_ia08_ddl_create_table_runtime ia08_ddl_create_table_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_table_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_table_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_table_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_table_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_table_runtime PROPERTIES LABELS CSC-TEST-002598)

add_executable(sbsql_sblr_alignment_ia08_ddl_create_table_availability ia08_ddl_create_table_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_table_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_table_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_table_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_table_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_table_availability PROPERTIES LABELS CSC-TEST-002599)

add_executable(sbsql_sblr_alignment_ia08_ddl_create_table_cancellation_fault ia08_ddl_create_table_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_table_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_table_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_table_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_table_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_table_cancellation_fault PROPERTIES LABELS CSC-TEST-002600)
add_executable(sbsql_sblr_alignment_ia08_ddl_create_index_runtime ia08_ddl_create_index_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_index_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_index_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_index_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_index_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_index_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_INDEX;CSC-TEST-002602;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_index_availability ia08_ddl_create_index_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_index_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_index_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_index_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_index_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_index_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_INDEX;CSC-TEST-002603;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_index_cancellation_fault ia08_ddl_create_index_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_index_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_index_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_index_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_index_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_index_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_INDEX;CSC-TEST-002604;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_index_runtime ia08_ddl_drop_index_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_index_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_index_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_index_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_index_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_index_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_INDEX;CSC-TEST-002606;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_index_availability ia08_ddl_drop_index_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_index_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_index_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_index_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_index_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_index_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_INDEX;CSC-TEST-002607;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_index_cancellation_fault ia08_ddl_drop_index_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_index_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_index_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_index_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_drop_index_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_index_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_INDEX;CSC-TEST-002608;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_domain_runtime ia08_ddl_alter_domain_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_domain_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_domain_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_domain_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_alter_domain_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_domain_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_DOMAIN;CSC-TEST-002610;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_domain_availability ia08_ddl_alter_domain_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_domain_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_domain_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_domain_availability COMMAND sbsql_sblr_alignment_ia08_ddl_alter_domain_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_domain_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_DOMAIN;CSC-TEST-002611;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_domain_cancellation_fault ia08_ddl_alter_domain_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_domain_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_domain_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_domain_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_alter_domain_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_domain_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_DOMAIN;CSC-TEST-002612;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_view_runtime ia08_ddl_create_view_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_view_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_view_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_view_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_view_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_view_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_VIEW;CSC-TEST-002610;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_view_availability ia08_ddl_create_view_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_view_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_view_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_view_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_view_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_view_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_VIEW;CSC-TEST-002611;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_view_cancellation_fault ia08_ddl_create_view_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_view_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_view_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_view_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_view_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_view_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_VIEW;CSC-TEST-002612;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_view_runtime ia08_ddl_alter_view_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_view_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_view_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_view_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_alter_view_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_view_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_VIEW;CSC-TEST-002614;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_view_availability ia08_ddl_alter_view_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_view_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_view_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_view_availability COMMAND sbsql_sblr_alignment_ia08_ddl_alter_view_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_view_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_VIEW;CSC-TEST-002615;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_view_cancellation_fault ia08_ddl_alter_view_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_view_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_view_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_view_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_alter_view_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_view_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_VIEW;CSC-TEST-002616;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_view_runtime ia08_ddl_drop_view_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_view_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_view_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_view_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_view_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_view_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_VIEW;CSC-TEST-002618;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_type_runtime ia08_ddl_drop_type_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_type_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_type_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_type_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_type_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_type_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_TYPE;CSC-TEST-002926;malformed_operand")
foreach(_type IN ITEMS create alter)
  add_executable(sbsql_sblr_alignment_ia08_ddl_${_type}_type_runtime ia08_ddl_${_type}_type_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_${_type}_type_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_${_type}_type_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_${_type}_type_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_${_type}_type_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_${_type}_type_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_${_type}_TYPE;malformed_operand")
endforeach()
add_executable(sbsql_sblr_alignment_ia08_ddl_create_type_availability ia08_ddl_create_type_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_type_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_type_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_type_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_type_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_type_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TYPE;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_type_availability ia08_ddl_alter_type_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_type_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_type_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_type_availability COMMAND sbsql_sblr_alignment_ia08_ddl_alter_type_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_type_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_TYPE;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_type_availability ia08_ddl_drop_type_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_type_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_type_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_type_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_type_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_type_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_TYPE;missing_executor_evidence")
foreach(_type IN ITEMS create alter drop)
  add_executable(sbsql_sblr_alignment_ia08_ddl_${_type}_type_cancellation_fault ia08_ddl_${_type}_type_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_${_type}_type_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_${_type}_type_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_${_type}_type_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_${_type}_type_cancellation_fault)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_${_type}_type_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_${_type}_TYPE;cancellation_atomicity")
endforeach()
add_executable(sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_runtime ia08_ddl_refresh_materialized_view_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_REFRESH_MATERIALIZED_VIEW;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_cancellation_fault ia08_ddl_refresh_materialized_view_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_REFRESH_MATERIALIZED_VIEW;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_availability ia08_ddl_refresh_materialized_view_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_availability COMMAND sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_refresh_materialized_view_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_REFRESH_MATERIALIZED_VIEW;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_materialized_view_runtime ia08_ddl_create_materialized_view_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_materialized_view_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_materialized_view_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_materialized_view_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_materialized_view_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_materialized_view_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_MATERIALIZED_VIEW;CSC-TEST-002953;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_materialized_view_availability ia08_ddl_create_materialized_view_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_materialized_view_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_materialized_view_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_materialized_view_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_materialized_view_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_materialized_view_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_MATERIALIZED_VIEW;CSC-TEST-002954;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_materialized_view_coordinator ia08_ddl_create_materialized_view_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_materialized_view_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_materialized_view_coordinator PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_materialized_view_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_create_materialized_view_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_materialized_view_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_MATERIALIZED_VIEW;CSC-TEST-002955;coordinator_replay_cancellation")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_view_availability ia08_ddl_drop_view_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_view_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_view_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_view_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_view_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_view_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_VIEW;CSC-TEST-002619;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_view_cancellation_fault ia08_ddl_drop_view_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_view_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_view_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_view_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_drop_view_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_view_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_VIEW;CSC-TEST-002620;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_runtime ia08_ddl_drop_materialized_view_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_MATERIALIZED_VIEW;CSC-TEST-002950;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_availability ia08_ddl_drop_materialized_view_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_MATERIALIZED_VIEW;CSC-TEST-002951;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_cancellation_fault ia08_ddl_drop_materialized_view_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_materialized_view_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_MATERIALIZED_VIEW;CSC-TEST-002952;exact_refusal")
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_schema_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_schema_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_schema_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_schema_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_schema_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_SCHEMA;CSC-TEST-002594;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_schema_availability ia08_ddl_create_schema_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_schema_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_schema_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_schema_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_schema_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_schema_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_SCHEMA;CSC-TEST-002595;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_schema_cancellation_fault ia08_ddl_create_schema_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_schema_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_schema_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_schema_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_schema_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_schema_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_SCHEMA;CSC-TEST-002596;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia01_window_availability ia01_window_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_window_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_window_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia01_window_availability COMMAND sbsql_sblr_alignment_ia01_window_availability)
set_tests_properties(sbsql_sblr_alignment_ia01_window_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_WINDOW;CSC-TEST-002555;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia01_window_cancellation_fault ia01_window_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_window_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_window_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_window_cancellation_fault COMMAND sbsql_sblr_alignment_ia01_window_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia01_window_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_WINDOW;CSC-TEST-002556;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia01_limit_availability ia01_limit_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_limit_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_limit_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia01_limit_availability COMMAND sbsql_sblr_alignment_ia01_limit_availability)
set_tests_properties(sbsql_sblr_alignment_ia01_limit_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_LIMIT;CSC-TEST-002551;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia01_limit_cancellation_fault ia01_limit_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_limit_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_limit_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_limit_cancellation_fault COMMAND sbsql_sblr_alignment_ia01_limit_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia01_limit_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_LIMIT;CSC-TEST-002552;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia01_sort_availability ia01_sort_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_sort_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_sort_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia01_sort_availability COMMAND sbsql_sblr_alignment_ia01_sort_availability)
set_tests_properties(sbsql_sblr_alignment_ia01_sort_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_SORT;CSC-TEST-002547;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia01_sort_cancellation_fault ia01_sort_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_sort_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_sort_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_sort_cancellation_fault COMMAND sbsql_sblr_alignment_ia01_sort_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia01_sort_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_SORT;CSC-TEST-002548;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia01_aggregate_availability ia01_aggregate_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_aggregate_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_aggregate_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia01_aggregate_availability COMMAND sbsql_sblr_alignment_ia01_aggregate_availability)
set_tests_properties(sbsql_sblr_alignment_ia01_aggregate_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_AGGREGATE;CSC-TEST-002539;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia01_aggregate_cancellation_fault ia01_aggregate_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_aggregate_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_aggregate_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_aggregate_cancellation_fault COMMAND sbsql_sblr_alignment_ia01_aggregate_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia01_aggregate_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_AGGREGATE;CSC-TEST-002540;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia01_project_availability ia01_project_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_project_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_project_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia01_project_availability COMMAND sbsql_sblr_alignment_ia01_project_availability)
set_tests_properties(sbsql_sblr_alignment_ia01_project_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_PROJECT;CSC-TEST-002535;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia01_project_cancellation_fault ia01_project_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_project_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_project_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia01_project_cancellation_fault COMMAND sbsql_sblr_alignment_ia01_project_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia01_project_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_PROJECT;CSC-TEST-002536;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_advanced_datatype_family_availability ia06_advanced_datatype_family_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_advanced_datatype_family_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_advanced_datatype_family_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia06_advanced_datatype_family_availability COMMAND sbsql_sblr_alignment_ia06_advanced_datatype_family_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_advanced_datatype_family_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_QUERY_EVALUATE_ADVANCED_DATATYPE_FAMILY;CSC-TEST-002531;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_advanced_datatype_family_cancellation_fault ia06_advanced_datatype_family_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_advanced_datatype_family_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_advanced_datatype_family_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_advanced_datatype_family_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_advanced_datatype_family_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_advanced_datatype_family_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_QUERY_EVALUATE_ADVANCED_DATATYPE_FAMILY;CSC-TEST-002532;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_query_numeric_availability ia06_query_numeric_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_query_numeric_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_query_numeric_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia06_query_numeric_availability COMMAND sbsql_sblr_alignment_ia06_query_numeric_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_query_numeric_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_QUERY_APPLY_NUMERIC_OPERATION;CSC-TEST-002527;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_query_numeric_cancellation_fault ia06_query_numeric_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_query_numeric_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_query_numeric_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_query_numeric_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_query_numeric_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_query_numeric_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_QUERY_APPLY_NUMERIC_OPERATION;CSC-TEST-002528;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_sequence_setval_availability ia06_sequence_setval_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_sequence_setval_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_sequence_setval_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia06_sequence_setval_availability COMMAND sbsql_sblr_alignment_ia06_sequence_setval_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_sequence_setval_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_SEQUENCE_SETVAL;CSC-TEST-002523;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_sequence_setval_cancellation_fault ia06_sequence_setval_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_sequence_setval_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_sequence_setval_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_sequence_setval_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_sequence_setval_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_sequence_setval_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_SEQUENCE_SETVAL;CSC-TEST-002524;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_sequence_currval_runtime ia06_sequence_currval_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_sequence_currval_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_sequence_currval_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_sequence_currval_runtime COMMAND sbsql_sblr_alignment_ia06_sequence_currval_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_sequence_currval_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_SEQUENCE_CURRVAL;CSC-TEST-002518;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_udr_invoke_availability ia06_udr_invoke_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_udr_invoke_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_udr_invoke_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia06_udr_invoke_availability COMMAND sbsql_sblr_alignment_ia06_udr_invoke_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_udr_invoke_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_UDR_INVOKE;CSC-TEST-002499;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_udr_invoke_cancellation_fault ia06_udr_invoke_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_udr_invoke_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_udr_invoke_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_udr_invoke_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_udr_invoke_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_udr_invoke_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_UDR_INVOKE;CSC-TEST-002500;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_procedure_invoke_availability ia06_procedure_invoke_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_procedure_invoke_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_procedure_invoke_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia06_procedure_invoke_availability COMMAND sbsql_sblr_alignment_ia06_procedure_invoke_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_procedure_invoke_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_PROCEDURE_INVOKE;CSC-TEST-002503;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_procedure_invoke_cancellation_fault ia06_procedure_invoke_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_procedure_invoke_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_procedure_invoke_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_procedure_invoke_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_procedure_invoke_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_procedure_invoke_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_PROCEDURE_INVOKE;CSC-TEST-002504;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_function_invoke_availability ia06_function_invoke_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_function_invoke_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_function_invoke_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia06_function_invoke_availability COMMAND sbsql_sblr_alignment_ia06_function_invoke_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_function_invoke_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_FUNCTION_INVOKE;CSC-TEST-002507;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_function_invoke_cancellation_fault ia06_function_invoke_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_function_invoke_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_function_invoke_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_function_invoke_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_function_invoke_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_function_invoke_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_FUNCTION_INVOKE;CSC-TEST-002508;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_aggregate_invoke_availability ia06_aggregate_invoke_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_aggregate_invoke_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_aggregate_invoke_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia06_aggregate_invoke_availability COMMAND sbsql_sblr_alignment_ia06_aggregate_invoke_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_aggregate_invoke_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_AGGREGATE_INVOKE;CSC-TEST-002511;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_aggregate_invoke_cancellation_fault ia06_aggregate_invoke_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_aggregate_invoke_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_aggregate_invoke_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_aggregate_invoke_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_aggregate_invoke_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_aggregate_invoke_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_AGGREGATE_INVOKE;CSC-TEST-002512;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_sequence_nextval_availability ia06_sequence_nextval_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_sequence_nextval_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_sequence_nextval_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia06_sequence_nextval_availability COMMAND sbsql_sblr_alignment_ia06_sequence_nextval_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_sequence_nextval_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_SEQUENCE_NEXTVAL;CSC-TEST-002515;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_sequence_nextval_cancellation_fault ia06_sequence_nextval_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_sequence_nextval_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_sequence_nextval_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_sequence_nextval_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_sequence_nextval_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_sequence_nextval_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_SEQUENCE_NEXTVAL;CSC-TEST-002516;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_sequence_currval_availability ia06_sequence_currval_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_sequence_currval_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_sequence_currval_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia06_sequence_currval_availability COMMAND sbsql_sblr_alignment_ia06_sequence_currval_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_sequence_currval_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_SEQUENCE_CURRVAL;CSC-TEST-002519;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_sequence_currval_cancellation_fault ia06_sequence_currval_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_sequence_currval_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_sequence_currval_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_sequence_currval_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_sequence_currval_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_sequence_currval_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_SEQUENCE_CURRVAL;CSC-TEST-002520;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_domain_operation_availability ia06_domain_operation_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_domain_operation_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_domain_operation_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia06_domain_operation_availability COMMAND sbsql_sblr_alignment_ia06_domain_operation_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_domain_operation_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_DOMAIN_OPERATION;CSC-TEST-002495;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_domain_operation_cancellation_fault ia06_domain_operation_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_domain_operation_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_domain_operation_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_domain_operation_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_domain_operation_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_domain_operation_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_DOMAIN_OPERATION;CSC-TEST-002496;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_compare_availability ia06_compare_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_compare_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_compare_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia06_compare_availability COMMAND sbsql_sblr_alignment_ia06_compare_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_compare_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_COMPARE;CSC-TEST-002491;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_compare_cancellation_fault ia06_compare_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_compare_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_compare_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_compare_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_compare_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_compare_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_COMPARE;CSC-TEST-002492;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_cast_availability ia06_cast_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_cast_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_cast_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia06_cast_availability COMMAND sbsql_sblr_alignment_ia06_cast_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_cast_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_CAST;CSC-TEST-002487;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_cast_cancellation_fault ia06_cast_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_cast_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_cast_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_cast_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_cast_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_cast_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_CAST;CSC-TEST-002488;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_operator_call_availability ia06_operator_call_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_operator_call_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_operator_call_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia06_operator_call_availability COMMAND sbsql_sblr_alignment_ia06_operator_call_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_operator_call_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_OPERATOR_CALL;CSC-TEST-002483;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_operator_call_cancellation_fault ia06_operator_call_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_operator_call_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_operator_call_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_operator_call_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_operator_call_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_operator_call_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_OPERATOR_CALL;CSC-TEST-002484;cancellation_atomicity")

add_executable(sbsql_sblr_alignment_ia06_function_call_availability ia06_function_call_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_function_call_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_function_call_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia06_function_call_availability COMMAND sbsql_sblr_alignment_ia06_function_call_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_function_call_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_FUNCTION_CALL;CSC-TEST-002479;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_function_call_cancellation_fault ia06_function_call_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_function_call_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_function_call_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_function_call_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_function_call_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_function_call_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_FUNCTION_CALL;CSC-TEST-002480;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_advisory_lock_release_availability ia07_advisory_lock_release_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_advisory_lock_release_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_advisory_lock_release_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia07_advisory_lock_release_availability COMMAND sbsql_sblr_alignment_ia07_advisory_lock_release_availability)
set_tests_properties(sbsql_sblr_alignment_ia07_advisory_lock_release_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_ADVISORY_LOCK_RELEASE;CSC-TEST-002475;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia07_advisory_lock_release_cancellation_fault ia07_advisory_lock_release_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_advisory_lock_release_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_advisory_lock_release_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_advisory_lock_release_cancellation_fault COMMAND sbsql_sblr_alignment_ia07_advisory_lock_release_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia07_advisory_lock_release_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_ADVISORY_LOCK_RELEASE;CSC-TEST-002476;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_advisory_lock_availability ia07_advisory_lock_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_advisory_lock_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_advisory_lock_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia07_advisory_lock_availability COMMAND sbsql_sblr_alignment_ia07_advisory_lock_availability)
set_tests_properties(sbsql_sblr_alignment_ia07_advisory_lock_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_ADVISORY_LOCK_ACQUIRE;CSC-TEST-002471;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia07_advisory_lock_cancellation_fault ia07_advisory_lock_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_advisory_lock_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_advisory_lock_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_advisory_lock_cancellation_fault COMMAND sbsql_sblr_alignment_ia07_advisory_lock_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia07_advisory_lock_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_ADVISORY_LOCK_ACQUIRE;CSC-TEST-002472;cancellation_atomicity")

add_executable(sbsql_sblr_alignment_ia07_atomic_rmw_availability ia07_atomic_rmw_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_atomic_rmw_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_atomic_rmw_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia07_atomic_rmw_availability COMMAND sbsql_sblr_alignment_ia07_atomic_rmw_availability)
set_tests_properties(sbsql_sblr_alignment_ia07_atomic_rmw_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_ATOMIC_READ_MODIFY_WRITE;CSC-TEST-002467;missing_executor_evidence")

add_executable(sbsql_sblr_alignment_ia07_atomic_rmw_cancellation_fault ia07_atomic_rmw_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_atomic_rmw_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_atomic_rmw_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_atomic_rmw_cancellation_fault COMMAND sbsql_sblr_alignment_ia07_atomic_rmw_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia07_atomic_rmw_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_ATOMIC_READ_MODIFY_WRITE;CSC-TEST-002468;cancellation_atomicity")
foreach(k availability cancellation_fault)
 add_executable(sbsql_sblr_alignment_ia07_atomic_cas_${k} ia07_atomic_cas_${k}_test.cpp)
 target_include_directories(sbsql_sblr_alignment_ia07_atomic_cas_${k} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
 target_link_libraries(sbsql_sblr_alignment_ia07_atomic_cas_${k} PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
 add_test(NAME sbsql_sblr_alignment_ia07_atomic_cas_${k} COMMAND sbsql_sblr_alignment_ia07_atomic_cas_${k})
endforeach()
set_tests_properties(sbsql_sblr_alignment_ia07_atomic_cas_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_ATOMIC_CAS;CSC-TEST-002463;missing_executor_evidence")
set_tests_properties(sbsql_sblr_alignment_ia07_atomic_cas_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_ATOMIC_CAS;CSC-TEST-002464;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia07_delete_availability ia07_delete_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_delete_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_delete_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia07_delete_availability COMMAND sbsql_sblr_alignment_ia07_delete_availability)
set_tests_properties(sbsql_sblr_alignment_ia07_delete_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_DELETE;CSC-TEST-002431;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia07_delete_cancellation_fault ia07_delete_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia07_delete_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia07_delete_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia07_delete_cancellation_fault COMMAND sbsql_sblr_alignment_ia07_delete_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia07_delete_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-07;SBLR_DELETE;CSC-TEST-002432;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia06_read_by_key_availability ia06_read_by_key_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_read_by_key_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_read_by_key_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia06_read_by_key_availability COMMAND sbsql_sblr_alignment_ia06_read_by_key_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_read_by_key_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_READ_BY_KEY;CSC-TEST-002395;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_read_by_key_cancellation_fault ia06_read_by_key_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_read_by_key_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_read_by_key_cancellation_fault PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia06_read_by_key_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_read_by_key_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_read_by_key_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_READ_BY_KEY;CSC-TEST-002396;cancellation_atomicity")
target_include_directories(sbsql_sblr_alignment_ia06_cursor_close_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_cursor_close_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_cursor_close_runtime COMMAND sbsql_sblr_alignment_ia06_cursor_close_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_cursor_close_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-06;SBLR_CURSOR_CLOSE;CSC-TEST-002390;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_cursor_close_availability ia06_cursor_close_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_cursor_close_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_cursor_close_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia06_cursor_close_availability COMMAND sbsql_sblr_alignment_ia06_cursor_close_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_cursor_close_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_CURSOR_CLOSE;CSC-TEST-002391;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_cursor_close_cancellation_fault ia06_cursor_close_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_cursor_close_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_cursor_close_cancellation_fault PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia06_cursor_close_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_cursor_close_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_cursor_close_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_CURSOR_CLOSE;CSC-TEST-002392;cancellation_atomicity")
target_include_directories(sbsql_sblr_alignment_ia06_cursor_fetch_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_cursor_fetch_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_cursor_fetch_runtime COMMAND sbsql_sblr_alignment_ia06_cursor_fetch_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_cursor_fetch_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-06;SBLR_CURSOR_FETCH;CSC-TEST-002386;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_cursor_fetch_availability ia06_cursor_fetch_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_cursor_fetch_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_cursor_fetch_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia06_cursor_fetch_availability COMMAND sbsql_sblr_alignment_ia06_cursor_fetch_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_cursor_fetch_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_CURSOR_FETCH;CSC-TEST-002387;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_cursor_fetch_cancellation_fault ia06_cursor_fetch_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_cursor_fetch_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_cursor_fetch_cancellation_fault PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia06_cursor_fetch_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_cursor_fetch_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_cursor_fetch_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-06;SBLR_CURSOR_FETCH;CSC-TEST-002388;cancellation_atomicity")
target_include_directories(sbsql_sblr_alignment_ia06_cursor_open_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_cursor_open_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_cursor_open_runtime COMMAND sbsql_sblr_alignment_ia06_cursor_open_runtime)
set_tests_properties(sbsql_sblr_alignment_ia06_cursor_open_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-06;SBLR_CURSOR_OPEN;CSC-TEST-002382;malformed_operand")
add_executable(sbsql_sblr_alignment_ia06_cursor_open_availability ia06_cursor_open_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_cursor_open_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_cursor_open_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia06_cursor_open_availability COMMAND sbsql_sblr_alignment_ia06_cursor_open_availability)
set_tests_properties(sbsql_sblr_alignment_ia06_cursor_open_availability PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;IA-06;SBLR_CURSOR_OPEN;CSC-TEST-002383;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia06_cursor_open_cancellation_fault ia06_cursor_open_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia06_cursor_open_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia06_cursor_open_cancellation_fault PRIVATE sb_engine_sblr sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia06_cursor_open_cancellation_fault COMMAND sbsql_sblr_alignment_ia06_cursor_open_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia06_cursor_open_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fault;IA-06;SBLR_CURSOR_OPEN;CSC-TEST-002384;cancellation_atomicity")
target_include_directories(sbsql_sblr_alignment_ia05_temporary_instance_cleanup_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_temporary_instance_cleanup_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia05_temporary_instance_cleanup_runtime COMMAND sbsql_sblr_alignment_ia05_temporary_instance_cleanup_runtime)
set_tests_properties(sbsql_sblr_alignment_ia05_temporary_instance_cleanup_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-05;SBLR_TEMPORARY_INSTANCE_CLEANUP;CSC-TEST-002378;malformed_operand")
add_executable(sbsql_sblr_alignment_ia05_temporary_instance_cleanup_availability ia05_temporary_instance_cleanup_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia05_temporary_instance_cleanup_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_temporary_instance_cleanup_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia05_temporary_instance_cleanup_availability COMMAND sbsql_sblr_alignment_ia05_temporary_instance_cleanup_availability)
set_tests_properties(sbsql_sblr_alignment_ia05_temporary_instance_cleanup_availability PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;IA-05;SBLR_TEMPORARY_INSTANCE_CLEANUP;CSC-TEST-002379;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia05_temporary_instance_cleanup_cancellation_fault ia05_temporary_instance_cleanup_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia05_temporary_instance_cleanup_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_temporary_instance_cleanup_cancellation_fault PRIVATE sb_engine_sblr sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia05_temporary_instance_cleanup_cancellation_fault COMMAND sbsql_sblr_alignment_ia05_temporary_instance_cleanup_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia05_temporary_instance_cleanup_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fault;IA-05;SBLR_TEMPORARY_INSTANCE_CLEANUP;CSC-TEST-002380;cancellation_atomicity")
target_include_directories(sbsql_sblr_alignment_ia05_transaction_reservation_release_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_reservation_release_runtime PRIVATE sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_reservation_release_runtime COMMAND sbsql_sblr_alignment_ia05_transaction_reservation_release_runtime)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_reservation_release_runtime PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-05;SBLR_TRANSACTION_RESERVATION_RELEASE;CSC-TEST-002374;malformed_operand" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia05_transaction_reservation_release_availability ia05_transaction_reservation_release_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_reservation_release_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_reservation_release_availability PRIVATE sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_reservation_release_availability COMMAND sbsql_sblr_alignment_ia05_transaction_reservation_release_availability)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_reservation_release_availability PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;IA-05;SBLR_TRANSACTION_RESERVATION_RELEASE;CSC-TEST-002375;missing_executor_evidence" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia05_transaction_reservation_release_cancellation_fault ia05_transaction_reservation_release_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_reservation_release_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_reservation_release_cancellation_fault PRIVATE sb_engine_sblr sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_reservation_release_cancellation_fault COMMAND sbsql_sblr_alignment_ia05_transaction_reservation_release_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_reservation_release_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fault;IA-05;SBLR_TRANSACTION_RESERVATION_RELEASE;CSC-TEST-002376;cancellation_atomicity" TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia05_transaction_savepoint_coordinator
  ia05_transaction_savepoint_coordinator_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia05_transaction_savepoint_coordinator PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_savepoint_coordinator PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_savepoint_coordinator PRIVATE
  sb_engine_internal_api sb_core_uuid)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_savepoint_coordinator
  COMMAND sbsql_sblr_alignment_ia05_transaction_savepoint_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_savepoint_coordinator PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_integration;sbsql_sblr_alignment_recovery;IA-05;SBLR_TXN_SAVEPOINT;CSC-TEST-002359;missing_executor_evidence"
  TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia05_transaction_savepoint_cancellation_fault
  ia05_transaction_savepoint_cancellation_fault_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia05_transaction_savepoint_cancellation_fault PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia05_transaction_savepoint_cancellation_fault PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia05_transaction_savepoint_cancellation_fault PRIVATE
  sb_engine_sblr sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia05_transaction_savepoint_cancellation_fault
  COMMAND sbsql_sblr_alignment_ia05_transaction_savepoint_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia05_transaction_savepoint_cancellation_fault PROPERTIES
  LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_fault;IA-05;SBLR_TXN_SAVEPOINT;CSC-TEST-002360;cancellation_atomicity"
  TIMEOUT 60)

add_executable(sbsql_sblr_alignment_ia08_ddl_create_trigger_runtime ia08_ddl_create_trigger_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_trigger_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_trigger_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_trigger_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_trigger_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_trigger_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TRIGGER;CSC-TEST-002622;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_trigger_availability ia08_ddl_create_trigger_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_trigger_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_trigger_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_trigger_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_trigger_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_trigger_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TRIGGER;CSC-TEST-002623;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_trigger_cancellation_fault ia08_ddl_create_trigger_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_trigger_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_trigger_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_trigger_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_trigger_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_trigger_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TRIGGER;CSC-TEST-002624;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_trigger_runtime ia08_ddl_alter_trigger_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_trigger_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_trigger_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_trigger_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_alter_trigger_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_trigger_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_TRIGGER;CSC-TEST-002626;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_procedure_runtime ia08_ddl_create_procedure_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_procedure_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_procedure_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_procedure_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_procedure_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_procedure_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_PROCEDURE;CSC-TEST-002634;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_procedure_availability ia08_ddl_create_procedure_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_procedure_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_procedure_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_procedure_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_procedure_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_procedure_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_PROCEDURE;CSC-TEST-002635;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_procedure_cancellation_fault ia08_ddl_create_procedure_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_procedure_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_procedure_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_procedure_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_procedure_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_procedure_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_PROCEDURE;CSC-TEST-002636;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_procedure_runtime ia08_ddl_drop_procedure_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_procedure_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_procedure_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_procedure_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_procedure_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_procedure_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_PROCEDURE;CSC-TEST-002642;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_procedure_availability ia08_ddl_drop_procedure_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_procedure_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_procedure_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_procedure_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_procedure_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_procedure_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_PROCEDURE;CSC-TEST-002643;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_procedure_cancellation_fault ia08_ddl_drop_procedure_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_procedure_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_procedure_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_procedure_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_drop_procedure_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_procedure_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_PROCEDURE;CSC-TEST-002644;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_function_runtime ia08_ddl_create_function_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_function_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_function_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_function_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_function_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_function_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_FUNCTION;CSC-TEST-002646;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_function_availability ia08_ddl_create_function_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_function_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_function_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_function_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_function_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_function_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_FUNCTION;CSC-TEST-002647;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_function_cancellation_fault ia08_ddl_create_function_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_function_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_function_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_function_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_function_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_function_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_FUNCTION;CSC-TEST-002648;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_function_runtime ia08_ddl_alter_function_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_function_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_function_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_function_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_alter_function_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_function_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_FUNCTION;CSC-TEST-002650;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_function_availability ia08_ddl_alter_function_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_function_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_function_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_function_availability COMMAND sbsql_sblr_alignment_ia08_ddl_alter_function_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_function_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_FUNCTION;CSC-TEST-002651;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_function_cancellation_fault ia08_ddl_alter_function_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_function_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_function_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_function_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_alter_function_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_function_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_FUNCTION;CSC-TEST-002652;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_trigger_availability ia08_ddl_alter_trigger_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_trigger_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_trigger_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_trigger_availability COMMAND sbsql_sblr_alignment_ia08_ddl_alter_trigger_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_trigger_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_TRIGGER;CSC-TEST-002627;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_trigger_cancellation_fault ia08_ddl_alter_trigger_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_trigger_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_trigger_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_trigger_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_alter_trigger_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_trigger_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_TRIGGER;CSC-TEST-002628;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_trigger_runtime ia08_ddl_drop_trigger_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_trigger_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_trigger_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_trigger_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_trigger_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_trigger_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_TRIGGER;CSC-TEST-002630;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_trigger_availability ia08_ddl_drop_trigger_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_trigger_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_trigger_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_trigger_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_trigger_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_trigger_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_TRIGGER;CSC-TEST-002631;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_trigger_cancellation_fault ia08_ddl_drop_trigger_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_trigger_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_trigger_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_trigger_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_drop_trigger_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_trigger_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_TRIGGER;CSC-TEST-002632;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_function_runtime ia08_ddl_drop_function_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_function_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_function_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_function_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_function_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_function_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_FUNCTION;CSC-TEST-002654;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_function_availability ia08_ddl_drop_function_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_function_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_function_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_function_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_function_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_function_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_FUNCTION;CSC-TEST-002655;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_function_cancellation_fault ia08_ddl_drop_function_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_function_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_function_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_function_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_drop_function_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_function_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_FUNCTION;CSC-TEST-002656;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_package_runtime ia08_ddl_create_package_runtime_test.cpp)
add_executable(sbsql_sblr_alignment_ia08_ddl_create_sequence_runtime ia08_ddl_create_sequence_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_sequence_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_sequence_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_sequence_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_sequence_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_sequence_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_SEQUENCE;CSC-TEST-002693;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_sequence_availability ia08_ddl_create_sequence_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_sequence_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_sequence_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_sequence_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_sequence_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_sequence_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_SEQUENCE;CSC-TEST-002694;availability")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_sequence_cancellation_fault ia08_ddl_create_sequence_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_sequence_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_sequence_cancellation_fault PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_sequence_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_sequence_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_sequence_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_SEQUENCE;CSC-TEST-002695;cancellation")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_sequence_coordinator ia08_ddl_create_sequence_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_sequence_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_sequence_coordinator PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_sequence_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_create_sequence_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_sequence_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_SEQUENCE;CSC-TEST-002696;coordinator")
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_package_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_package_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_package_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_package_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_package_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_PACKAGE;CSC-TEST-002658;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_package_availability ia08_ddl_create_package_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_package_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_package_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_package_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_package_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_package_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_PACKAGE;CSC-TEST-002659;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_package_cancellation_fault ia08_ddl_create_package_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_package_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_package_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_package_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_package_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_package_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_PACKAGE;CSC-TEST-002660;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_runtime ia08_ddl_drop_temporary_table_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_TEMPORARY_TABLE;CSC-TEST-002666;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_availability ia08_ddl_drop_temporary_table_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_TEMPORARY_TABLE;CSC-TEST-002667;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_cancellation_fault ia08_ddl_drop_temporary_table_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_temporary_table_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_TEMPORARY_TABLE;CSC-TEST-002668;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_rename_object_vector_runtime ia08_ddl_rename_object_vector_runtime_test.cpp)
add_executable(sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_runtime ia08_ddl_create_or_replace_srs_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_OR_REPLACE_SRS;CSC-TEST-002674;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_srs_runtime ia08_ddl_drop_srs_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_srs_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_srs_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_srs_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_srs_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_srs_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_SRS;CSC-TEST-002678;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_availability ia08_ddl_create_or_replace_srs_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_OR_REPLACE_SRS;CSC-TEST-002675;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_cancellation_fault ia08_ddl_create_or_replace_srs_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_or_replace_srs_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_OR_REPLACE_SRS;CSC-TEST-002676;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_srs_availability ia08_ddl_drop_srs_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_srs_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_srs_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_srs_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_srs_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_srs_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_SRS;CSC-TEST-002679;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_srs_cancellation_fault ia08_ddl_drop_srs_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_srs_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_srs_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_srs_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_drop_srs_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_srs_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_SRS;CSC-TEST-002680;cancellation_atomicity")
target_include_directories(sbsql_sblr_alignment_ia08_ddl_rename_object_vector_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_rename_object_vector_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_rename_object_vector_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_rename_object_vector_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_rename_object_vector_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_RENAME_OBJECT_VECTOR;CSC-TEST-002670;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_rename_object_vector_availability ia08_ddl_rename_object_vector_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_rename_object_vector_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_rename_object_vector_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_rename_object_vector_availability COMMAND sbsql_sblr_alignment_ia08_ddl_rename_object_vector_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_rename_object_vector_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_RENAME_OBJECT_VECTOR;CSC-TEST-002671;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_rename_object_vector_cancellation_fault ia08_ddl_rename_object_vector_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_rename_object_vector_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_rename_object_vector_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_rename_object_vector_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_rename_object_vector_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_rename_object_vector_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_RENAME_OBJECT_VECTOR;CSC-TEST-002672;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_rename_object_runtime ia08_ddl_rename_object_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_rename_object_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_rename_object_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_rename_object_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_rename_object_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_rename_object_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_RENAME_OBJECT;CSC-TEST-002934;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_rename_object_availability ia08_ddl_rename_object_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_rename_object_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_rename_object_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_rename_object_availability COMMAND sbsql_sblr_alignment_ia08_ddl_rename_object_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_rename_object_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_RENAME_OBJECT;CSC-TEST-002935;availability")
add_executable(sbsql_sblr_alignment_ia08_ddl_rename_object_cancellation_fault ia08_ddl_rename_object_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_rename_object_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_rename_object_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_rename_object_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_rename_object_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_rename_object_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_RENAME_OBJECT;CSC-TEST-002936;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_temporary_table_runtime ia08_ddl_create_temporary_table_runtime_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia08_ddl_create_temporary_table_runtime PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_temporary_table_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_temporary_table_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_temporary_table_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_temporary_table_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_temporary_table_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TEMPORARY_TABLE;CSC-TEST-002662;malformed_operand;contract")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_temporary_table_availability ia08_ddl_create_temporary_table_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_temporary_table_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_temporary_table_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_temporary_table_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_temporary_table_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_temporary_table_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TEMPORARY_TABLE;CSC-TEST-002663;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_temporary_table_cancellation_fault ia08_ddl_create_temporary_table_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_temporary_table_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_temporary_table_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_temporary_table_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_temporary_table_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_temporary_table_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TEMPORARY_TABLE;CSC-TEST-002664;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_runtime ia08_ddl_create_rewrite_rule_runtime_test.cpp)
target_compile_features(sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_runtime PRIVATE cxx_std_20)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_REWRITE_RULE;CSC-TEST-002682;malformed_operand;contract")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_availability ia08_ddl_create_rewrite_rule_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_REWRITE_RULE;CSC-TEST-002683;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_cancellation_fault ia08_ddl_create_rewrite_rule_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_rewrite_rule_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_REWRITE_RULE;CSC-TEST-002684;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_runtime ia08_ddl_alter_rewrite_rule_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_REWRITE_RULE;CSC-TEST-002686;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_availability ia08_ddl_alter_rewrite_rule_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_availability COMMAND sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_REWRITE_RULE;CSC-TEST-002687;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_cancellation_fault ia08_ddl_alter_rewrite_rule_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_rewrite_rule_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_REWRITE_RULE;CSC-TEST-002688;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_runtime ia08_ddl_drop_rewrite_rule_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_REWRITE_RULE;CSC-TEST-002690;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_validate_constraint_runtime ia08_ddl_validate_constraint_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_validate_constraint_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_validate_constraint_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_validate_constraint_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_validate_constraint_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_validate_constraint_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_VALIDATE_CONSTRAINT;CSC-TEST-002694;malformed_operand")
add_executable(sbsql_sblr_alignment_ia09_security_create_privilege_template_runtime ia09_security_create_privilege_template_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia09_security_create_privilege_template_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia09_security_create_privilege_template_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia09_security_create_privilege_template_runtime COMMAND sbsql_sblr_alignment_ia09_security_create_privilege_template_runtime)
set_tests_properties(sbsql_sblr_alignment_ia09_security_create_privilege_template_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SECURITY_CREATE_PRIVILEGE_TEMPLATE;CSC-TEST-002698;malformed_operand")
add_executable(sbsql_sblr_alignment_ia09_security_alter_privilege_template_runtime ia09_security_alter_privilege_template_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia09_security_alter_privilege_template_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia09_security_alter_privilege_template_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia09_security_alter_privilege_template_runtime COMMAND sbsql_sblr_alignment_ia09_security_alter_privilege_template_runtime)
set_tests_properties(sbsql_sblr_alignment_ia09_security_alter_privilege_template_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SECURITY_ALTER_PRIVILEGE_TEMPLATE;CSC-TEST-002702;malformed_operand")
add_executable(sbsql_sblr_alignment_ia09_security_drop_privilege_template_availability ia09_security_drop_privilege_template_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia09_security_drop_privilege_template_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia09_security_drop_privilege_template_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia09_security_drop_privilege_template_availability COMMAND sbsql_sblr_alignment_ia09_security_drop_privilege_template_availability)
set_tests_properties(sbsql_sblr_alignment_ia09_security_drop_privilege_template_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SECURITY_DROP_PRIVILEGE_TEMPLATE;CSC-TEST-002707;availability")
add_executable(sbsql_sblr_alignment_ia09_security_drop_privilege_template_cancellation_fault ia09_security_drop_privilege_template_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia09_security_drop_privilege_template_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia09_security_drop_privilege_template_cancellation_fault PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia09_security_drop_privilege_template_cancellation_fault COMMAND sbsql_sblr_alignment_ia09_security_drop_privilege_template_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia09_security_drop_privilege_template_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SECURITY_DROP_PRIVILEGE_TEMPLATE;CSC-TEST-002708;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia09_security_drop_privilege_template_runtime ia09_security_drop_privilege_template_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia09_security_drop_privilege_template_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia09_security_drop_privilege_template_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia09_security_drop_privilege_template_runtime COMMAND sbsql_sblr_alignment_ia09_security_drop_privilege_template_runtime)
set_tests_properties(sbsql_sblr_alignment_ia09_security_drop_privilege_template_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SECURITY_DROP_PRIVILEGE_TEMPLATE;CSC-TEST-002706;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_aggregate_runtime ia08_ddl_create_aggregate_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_aggregate_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_aggregate_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_aggregate_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_aggregate_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_aggregate_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_AGGREGATE;CSC-TEST-002714;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_aggregate_availability ia08_ddl_create_aggregate_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_aggregate_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_aggregate_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_aggregate_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_aggregate_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_aggregate_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_AGGREGATE;CSC-TEST-002715;availability")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_aggregate_cancellation_fault ia08_ddl_create_aggregate_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_aggregate_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_aggregate_cancellation_fault PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_aggregate_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_aggregate_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_aggregate_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_AGGREGATE;CSC-TEST-002716;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_aggregate_runtime ia08_ddl_alter_aggregate_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_aggregate_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_aggregate_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_aggregate_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_alter_aggregate_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_aggregate_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_AGGREGATE;CSC-TEST-002718;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_aggregate_availability ia08_ddl_alter_aggregate_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_aggregate_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_aggregate_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_aggregate_availability COMMAND sbsql_sblr_alignment_ia08_ddl_alter_aggregate_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_aggregate_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_AGGREGATE;CSC-TEST-002719;availability")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_aggregate_cancellation_fault ia08_ddl_alter_aggregate_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_aggregate_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_aggregate_cancellation_fault PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_aggregate_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_alter_aggregate_cancellation_fault)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_aggregate_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_AGGREGATE;CSC-TEST-002720;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_aggregate_runtime ia08_ddl_drop_aggregate_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_aggregate_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_aggregate_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_aggregate_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_aggregate_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_aggregate_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_AGGREGATE;CSC-TEST-002722;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_aggregate_availability ia08_ddl_drop_aggregate_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_aggregate_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_aggregate_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_aggregate_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_aggregate_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_aggregate_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_AGGREGATE;CSC-TEST-002723;availability")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_aggregate_cancellation_fault ia08_ddl_drop_aggregate_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_aggregate_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_aggregate_cancellation_fault PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_aggregate_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_drop_aggregate_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_aggregate_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_AGGREGATE;CSC-TEST-002724;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_purge_system_history_runtime ia08_ddl_purge_system_history_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_purge_system_history_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_purge_system_history_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_purge_system_history_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_purge_system_history_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_purge_system_history_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_PURGE_SYSTEM_HISTORY;CSC-TEST-002726;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_purge_system_history_availability ia08_ddl_purge_system_history_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_purge_system_history_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_purge_system_history_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_purge_system_history_availability COMMAND sbsql_sblr_alignment_ia08_ddl_purge_system_history_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_purge_system_history_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_PURGE_SYSTEM_HISTORY;CSC-TEST-002727;availability")
add_executable(sbsql_sblr_alignment_ia08_ddl_purge_system_history_cancellation_fault ia08_ddl_purge_system_history_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_purge_system_history_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_purge_system_history_cancellation_fault PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_purge_system_history_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_purge_system_history_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_purge_system_history_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_PURGE_SYSTEM_HISTORY;CSC-TEST-002728;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_runtime ia08_ddl_set_index_optimizer_eligibility_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_SET_INDEX_OPTIMIZER_ELIGIBILITY;CSC-TEST-002730;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_availability ia08_ddl_set_index_optimizer_eligibility_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_availability COMMAND sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_SET_INDEX_OPTIMIZER_ELIGIBILITY;CSC-TEST-002731;availability")
add_executable(sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_cancellation_fault ia08_ddl_set_index_optimizer_eligibility_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_cancellation_fault PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_set_index_optimizer_eligibility_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_SET_INDEX_OPTIMIZER_ELIGIBILITY;CSC-TEST-002732;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_runtime ia08_ddl_set_table_type_enforcement_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_SET_TABLE_TYPE_ENFORCEMENT;CSC-TEST-002734;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_availability ia08_ddl_set_table_type_enforcement_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_availability COMMAND sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_SET_TABLE_TYPE_ENFORCEMENT;CSC-TEST-002735;availability")
add_executable(sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_cancellation_fault ia08_ddl_set_table_type_enforcement_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_cancellation_fault PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_set_table_type_enforcement_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_SET_TABLE_TYPE_ENFORCEMENT;CSC-TEST-002736;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_dictionary_coordinator ia08_ddl_create_dictionary_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_dictionary_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_dictionary_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_dictionary_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_create_dictionary_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_dictionary_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_DICTIONARY;CSC-TEST-002763;availability")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_publication_runtime ia08_ddl_create_publication_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_publication_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_publication_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_publication_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_publication_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_publication_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_PUBLICATION;CSC-TEST-003089;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_publication_coordinator ia08_ddl_create_publication_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_publication_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_publication_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_publication_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_create_publication_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_publication_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_PUBLICATION;CSC-TEST-003090;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_publication_runtime ia08_ddl_alter_publication_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_publication_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_publication_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_publication_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_alter_publication_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_publication_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_PUBLICATION;CSC-TEST-003093;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_publication_coordinator ia08_ddl_alter_publication_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_publication_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_publication_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_publication_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_alter_publication_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_publication_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_PUBLICATION;CSC-TEST-003094;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_publication_runtime ia08_ddl_drop_publication_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_publication_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_publication_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_publication_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_publication_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_publication_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_PUBLICATION;CSC-TEST-003097;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_publication_coordinator ia08_ddl_drop_publication_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_publication_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_publication_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_publication_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_drop_publication_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_publication_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_PUBLICATION;CSC-TEST-003098;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_subscription_runtime ia08_ddl_create_subscription_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_subscription_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_subscription_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_subscription_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_subscription_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_subscription_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_SUBSCRIPTION;CSC-TEST-003101;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_subscription_coordinator ia08_ddl_create_subscription_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_subscription_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_subscription_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_subscription_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_create_subscription_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_subscription_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_SUBSCRIPTION;CSC-TEST-003102;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_subscription_runtime ia08_ddl_alter_subscription_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_subscription_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_subscription_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_subscription_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_alter_subscription_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_subscription_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_SUBSCRIPTION;CSC-TEST-003994;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_subscription_coordinator ia08_ddl_alter_subscription_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_subscription_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_subscription_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_subscription_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_alter_subscription_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_subscription_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_SUBSCRIPTION;CSC-TEST-003996;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_subscription_runtime ia08_ddl_drop_subscription_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_subscription_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_subscription_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_subscription_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_subscription_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_subscription_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_SUBSCRIPTION;CSC-TEST-003998;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_operator_runtime ia08_ddl_create_operator_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_operator_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_operator_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_operator_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_operator_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_operator_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_OPERATOR;CSC-TEST-004002;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_operator_coordinator ia08_ddl_create_operator_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_operator_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_operator_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_operator_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_create_operator_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_operator_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_OPERATOR;CSC-TEST-004003;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_operator_runtime ia08_ddl_drop_operator_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_operator_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_operator_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_operator_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_operator_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_operator_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_OPERATOR;CSC-TEST-004006;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_operator_coordinator ia08_ddl_drop_operator_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_operator_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_operator_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_operator_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_drop_operator_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_operator_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_OPERATOR;CSC-TEST-004007;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_operator_class_runtime ia08_ddl_create_operator_class_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_operator_class_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_operator_class_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_operator_class_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_operator_class_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_operator_class_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_OPERATOR_CLASS;CSC-TEST-004010;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_operator_class_coordinator ia08_ddl_create_operator_class_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_operator_class_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_operator_class_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_operator_class_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_create_operator_class_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_operator_class_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_OPERATOR_CLASS;CSC-TEST-004011;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_operator_class_runtime ia08_ddl_drop_operator_class_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_operator_class_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_operator_class_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_operator_class_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_operator_class_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_operator_class_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_OPERATOR_CLASS;CSC-TEST-004014;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_operator_class_coordinator ia08_ddl_drop_operator_class_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_operator_class_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_operator_class_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_operator_class_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_drop_operator_class_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_operator_class_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_OPERATOR_CLASS;CSC-TEST-004015;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_operator_family_runtime ia08_ddl_create_operator_family_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_operator_family_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_operator_family_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_operator_family_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_operator_family_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_operator_family_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_OPERATOR_FAMILY;CSC-TEST-004018;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_operator_family_coordinator ia08_ddl_create_operator_family_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_operator_family_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_operator_family_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_operator_family_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_create_operator_family_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_operator_family_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_OPERATOR_FAMILY;CSC-TEST-004019;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_operator_family_runtime ia08_ddl_alter_operator_family_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_operator_family_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_operator_family_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_operator_family_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_alter_operator_family_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_operator_family_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_OPERATOR_FAMILY;CSC-TEST-004022;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_operator_family_coordinator ia08_ddl_alter_operator_family_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_operator_family_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_operator_family_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_operator_family_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_alter_operator_family_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_operator_family_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_OPERATOR_FAMILY;CSC-TEST-004023;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_operator_family_runtime ia08_ddl_drop_operator_family_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_operator_family_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_operator_family_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_operator_family_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_operator_family_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_operator_family_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_OPERATOR_FAMILY;CSC-TEST-004026;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_operator_family_coordinator ia08_ddl_drop_operator_family_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_operator_family_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_operator_family_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_operator_family_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_drop_operator_family_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_operator_family_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_OPERATOR_FAMILY;CSC-TEST-004027;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_cast_runtime ia08_ddl_create_cast_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_cast_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_cast_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_cast_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_cast_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_cast_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_CAST;CSC-TEST-004030;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_cast_coordinator ia08_ddl_create_cast_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_cast_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_cast_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_cast_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_create_cast_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_cast_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_CAST;CSC-TEST-004031;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_cast_runtime ia08_ddl_drop_cast_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_cast_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_cast_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_cast_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_cast_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_cast_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_CAST;CSC-TEST-004034;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_cast_coordinator ia08_ddl_drop_cast_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_cast_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_cast_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_cast_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_drop_cast_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_cast_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_CAST;CSC-TEST-004035;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_collation_runtime ia08_ddl_create_collation_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_collation_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_collation_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_collation_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_collation_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_collation_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_COLLATION;CSC-TEST-004038;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_collation_coordinator ia08_ddl_create_collation_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_collation_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_collation_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_collation_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_create_collation_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_collation_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_COLLATION;CSC-TEST-004039;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_collation_runtime ia08_ddl_alter_collation_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_collation_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_collation_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_collation_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_alter_collation_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_collation_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_COLLATION;CSC-TEST-004042;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_extension_runtime ia08_ddl_alter_extension_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_extension_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_extension_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_extension_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_alter_extension_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_extension_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_EXTENSION;CSC-TEST-004054;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_extension_coordinator ia08_ddl_alter_extension_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_extension_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_extension_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_extension_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_alter_extension_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_extension_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_EXTENSION;CSC-TEST-004055;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_extension_runtime ia08_ddl_drop_extension_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_extension_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_extension_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_extension_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_extension_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_extension_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_EXTENSION;CSC-TEST-004058;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_extension_coordinator ia08_ddl_drop_extension_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_extension_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_extension_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_extension_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_drop_extension_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_extension_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_EXTENSION;CSC-TEST-004059;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_event_trigger_runtime ia08_ddl_create_event_trigger_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_event_trigger_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_event_trigger_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_event_trigger_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_event_trigger_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_event_trigger_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_EVENT_TRIGGER;CSC-TEST-004062;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_event_trigger_coordinator ia08_ddl_create_event_trigger_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_event_trigger_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_event_trigger_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_event_trigger_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_create_event_trigger_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_event_trigger_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_EVENT_TRIGGER;CSC-TEST-004063;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_event_trigger_runtime ia08_ddl_alter_event_trigger_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_event_trigger_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_event_trigger_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_event_trigger_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_alter_event_trigger_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_event_trigger_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_EVENT_TRIGGER;CSC-TEST-004066;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_event_trigger_coordinator ia08_ddl_alter_event_trigger_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_event_trigger_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_event_trigger_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_event_trigger_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_alter_event_trigger_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_event_trigger_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_EVENT_TRIGGER;CSC-TEST-004067;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_event_trigger_runtime ia08_ddl_drop_event_trigger_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_event_trigger_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_event_trigger_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_event_trigger_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_event_trigger_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_event_trigger_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_EVENT_TRIGGER;CSC-TEST-004070;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_event_trigger_coordinator ia08_ddl_drop_event_trigger_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_event_trigger_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_event_trigger_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_event_trigger_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_drop_event_trigger_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_event_trigger_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_EVENT_TRIGGER;CSC-TEST-004071;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_cluster_create_placement_policy_runtime ia08_cluster_create_placement_policy_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_cluster_create_placement_policy_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_cluster_create_placement_policy_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_cluster_create_placement_policy_runtime COMMAND sbsql_sblr_alignment_ia08_cluster_create_placement_policy_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_cluster_create_placement_policy_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_CLUSTER_CREATE_PLACEMENT_POLICY;CSC-TEST-004074;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_cluster_create_placement_policy_coordinator ia08_cluster_create_placement_policy_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_cluster_create_placement_policy_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_cluster_create_placement_policy_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_cluster_create_placement_policy_coordinator COMMAND sbsql_sblr_alignment_ia08_cluster_create_placement_policy_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_cluster_create_placement_policy_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_CLUSTER_CREATE_PLACEMENT_POLICY;CSC-TEST-004075;cluster_refusal")
add_executable(sbsql_sblr_alignment_ia08_cluster_alter_placement_policy_runtime ia08_cluster_alter_placement_policy_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_cluster_alter_placement_policy_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_cluster_alter_placement_policy_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_cluster_alter_placement_policy_runtime COMMAND sbsql_sblr_alignment_ia08_cluster_alter_placement_policy_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_cluster_alter_placement_policy_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_CLUSTER_ALTER_PLACEMENT_POLICY;CSC-TEST-004078;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_cluster_alter_placement_policy_coordinator ia08_cluster_alter_placement_policy_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_cluster_alter_placement_policy_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_cluster_alter_placement_policy_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_cluster_alter_placement_policy_coordinator COMMAND sbsql_sblr_alignment_ia08_cluster_alter_placement_policy_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_cluster_alter_placement_policy_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_CLUSTER_ALTER_PLACEMENT_POLICY;CSC-TEST-004079;cluster_refusal")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_collation_coordinator ia08_ddl_alter_collation_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_collation_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_collation_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_collation_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_alter_collation_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_collation_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_COLLATION;CSC-TEST-004043;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_collation_runtime ia08_ddl_drop_collation_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_collation_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_collation_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_collation_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_collation_runtime)
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_collation_coordinator ia08_ddl_drop_collation_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_collation_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_collation_coordinator PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_collation_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_drop_collation_coordinator)
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_subscription_coordinator ia08_ddl_drop_subscription_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_subscription_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_subscription_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_subscription_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_drop_subscription_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_subscription_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_SUBSCRIPTION;CSC-TEST-004000;availability;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_dictionary_cancellation_fault ia08_ddl_create_dictionary_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_dictionary_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_dictionary_cancellation_fault PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_dictionary_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_dictionary_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_dictionary_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_DICTIONARY;CSC-TEST-002764;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_dictionary_runtime ia08_ddl_drop_dictionary_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_dictionary_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_dictionary_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_dictionary_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_dictionary_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_dictionary_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_DICTIONARY;CSC-TEST-002770;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_continuous_view_runtime ia08_ddl_create_continuous_view_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_continuous_view_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_continuous_view_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_continuous_view_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_continuous_view_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_continuous_view_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_CONTINUOUS_VIEW;CSC-TEST-002774;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_continuous_view_coordinator ia08_ddl_create_continuous_view_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_continuous_view_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_continuous_view_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_continuous_view_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_create_continuous_view_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_continuous_view_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_CONTINUOUS_VIEW;CSC-TEST-002775;availability;CSC-TEST-002776;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_continuous_view_runtime ia08_ddl_alter_continuous_view_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_continuous_view_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_continuous_view_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_continuous_view_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_alter_continuous_view_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_continuous_view_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_CONTINUOUS_VIEW;CSC-TEST-002778;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_alter_continuous_view_coordinator ia08_ddl_alter_continuous_view_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_alter_continuous_view_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_alter_continuous_view_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_continuous_view_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_alter_continuous_view_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_continuous_view_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_CONTINUOUS_VIEW;CSC-TEST-002779;availability;CSC-TEST-002780;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_continuous_view_runtime ia08_ddl_drop_continuous_view_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_continuous_view_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_continuous_view_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_continuous_view_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_continuous_view_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_continuous_view_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_CONTINUOUS_VIEW;CSC-TEST-002782;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_continuous_view_coordinator ia08_ddl_drop_continuous_view_coordinator_test.cpp)
add_executable(sbsql_sblr_alignment_ia08_dml_async_insert_submit_runtime ia08_dml_async_insert_submit_runtime_test.cpp)
add_executable(sbsql_sblr_alignment_ia08_dml_async_insert_submit_coordinator ia08_dml_async_insert_submit_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_dml_async_insert_submit_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_dml_async_insert_submit_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_dml_async_insert_submit_coordinator COMMAND sbsql_sblr_alignment_ia08_dml_async_insert_submit_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_dml_async_insert_submit_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_ASYNC_INSERT_SUBMIT;CSC-TEST-002787;availability;CSC-TEST-002788;cancellation_fault")
target_include_directories(sbsql_sblr_alignment_ia08_dml_async_insert_submit_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_dml_async_insert_submit_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_dml_async_insert_submit_runtime COMMAND sbsql_sblr_alignment_ia08_dml_async_insert_submit_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_dml_async_insert_submit_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_ASYNC_INSERT_SUBMIT;CSC-TEST-002786;malformed_operand")

add_executable(sbsql_sblr_alignment_ia08_dml_async_insert_status_runtime ia08_dml_async_insert_status_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_dml_async_insert_status_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_dml_async_insert_status_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_dml_async_insert_status_runtime COMMAND sbsql_sblr_alignment_ia08_dml_async_insert_status_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_dml_async_insert_status_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_ASYNC_INSERT_STATUS;CSC-TEST-002790;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_dml_async_insert_status_coordinator ia08_dml_async_insert_status_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_dml_async_insert_status_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_dml_async_insert_status_coordinator PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_dml_async_insert_status COMMAND sbsql_sblr_alignment_ia08_dml_async_insert_status_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_dml_async_insert_status PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_ASYNC_INSERT_STATUS;CSC-TEST-002791;availability;CSC-TEST-002792;cancellation_fault")
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_continuous_view_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_continuous_view_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_continuous_view_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_drop_continuous_view_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_continuous_view_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_CONTINUOUS_VIEW;CSC-TEST-002783;availability;CSC-TEST-002784;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_dictionary_coordinator ia08_ddl_drop_dictionary_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_dictionary_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_dictionary_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_dictionary_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_drop_dictionary_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_dictionary_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_DICTIONARY;CSC-TEST-002771;availability")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_dictionary_cancellation_fault ia08_ddl_drop_dictionary_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_dictionary_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_dictionary_cancellation_fault PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_dictionary_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_drop_dictionary_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_dictionary_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_DICTIONARY;CSC-TEST-002772;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_ddl_validate_constraint_availability ia08_ddl_validate_constraint_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_validate_constraint_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_validate_constraint_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_validate_constraint_availability COMMAND sbsql_sblr_alignment_ia08_ddl_validate_constraint_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_validate_constraint_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_VALIDATE_CONSTRAINT;CSC-TEST-002695;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_validate_constraint_cancellation_fault ia08_ddl_validate_constraint_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_validate_constraint_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_validate_constraint_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_validate_constraint_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_validate_constraint_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_validate_constraint_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_VALIDATE_CONSTRAINT;CSC-TEST-002696;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_availability ia08_ddl_drop_rewrite_rule_availability_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_availability PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_availability)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_REWRITE_RULE;CSC-TEST-002691;missing_executor_evidence")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_cancellation_fault ia08_ddl_drop_rewrite_rule_cancellation_fault_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_cancellation_fault PRIVATE sb_engine_internal_api sb_sblr_parameter_wire_codec)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_cancellation_fault)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_rewrite_rule_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_REWRITE_RULE;CSC-TEST-002692;cancellation_atomicity")
add_executable(sbsql_sblr_alignment_ia01_catalog_introspect_runtime ia01_catalog_introspect_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_catalog_introspect_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_catalog_introspect_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia01_catalog_introspect_runtime COMMAND sbsql_sblr_alignment_ia01_catalog_introspect_runtime)
set_tests_properties(sbsql_sblr_alignment_ia01_catalog_introspect_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_CATALOG_INTROSPECT;CSC-TEST-002142;malformed_operand" TIMEOUT 30)
add_executable(sbsql_sblr_alignment_ia01_catalog_introspect_coordinator ia01_catalog_introspect_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia01_catalog_introspect_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia01_catalog_introspect_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia01_catalog_introspect_coordinator COMMAND sbsql_sblr_alignment_ia01_catalog_introspect_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia01_catalog_introspect_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_CATALOG_INTROSPECT;CSC-TEST-002143;availability;CSC-TEST-002144;cancellation" TIMEOUT 30)
add_test(NAME sbsql_sblr_alignment_ia01_catalog_introspect_process_e2e
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py
    --server $<TARGET_FILE:sb_server>
    --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
    --operation show-object-detail
    --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia01_catalog_introspect_e2e")
set_tests_properties(sbsql_sblr_alignment_ia01_catalog_introspect_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-01;SBLR_CATALOG_INTROSPECT;CSC-TEST-002144;e2e" TIMEOUT 60)
add_executable(sbsql_sblr_alignment_ia08_dml_async_insert_cancel_runtime ia08_dml_async_insert_cancel_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_dml_async_insert_cancel_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_dml_async_insert_cancel_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_dml_async_insert_cancel_runtime COMMAND sbsql_sblr_alignment_ia08_dml_async_insert_cancel_runtime)

add_executable(sbsql_sblr_alignment_ia08_dml_conditional_mutate_runtime ia08_dml_conditional_mutate_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_dml_conditional_mutate_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_dml_conditional_mutate_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_dml_conditional_mutate_runtime COMMAND sbsql_sblr_alignment_ia08_dml_conditional_mutate_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_dml_conditional_mutate_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_CONDITIONAL_MUTATE;CSC-TEST-2798;malformed_operand")

add_executable(sbsql_sblr_alignment_ia08_dml_conditional_mutate_coordinator ia08_dml_conditional_mutate_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_dml_conditional_mutate_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_dml_conditional_mutate_coordinator PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_dml_conditional_mutate_availability COMMAND sbsql_sblr_alignment_ia08_dml_conditional_mutate_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_dml_conditional_mutate_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_CONDITIONAL_MUTATE;CSC-TEST-2799;availability")
add_test(NAME sbsql_sblr_alignment_ia08_dml_conditional_mutate_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_dml_conditional_mutate_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_dml_conditional_mutate_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_CONDITIONAL_MUTATE;CSC-TEST-2800;cancellation_fault")

add_executable(sbsql_sblr_alignment_ia08_dml_counter_add_runtime ia08_dml_counter_add_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_dml_counter_add_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_dml_counter_add_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_dml_counter_add_runtime COMMAND sbsql_sblr_alignment_ia08_dml_counter_add_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_dml_counter_add_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_COUNTER_ADD;CSC-TEST-2802;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_dml_counter_add_coordinator ia08_dml_counter_add_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_dml_counter_add_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_dml_counter_add_coordinator PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_dml_counter_add_availability COMMAND sbsql_sblr_alignment_ia08_dml_counter_add_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_dml_counter_add_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_COUNTER_ADD;CSC-TEST-2803;availability")
add_test(NAME sbsql_sblr_alignment_ia08_dml_counter_add_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_dml_counter_add_coordinator)
add_executable(sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_runtime ia08_dml_timeseries_schema_write_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_runtime COMMAND sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_TIMESERIES_SCHEMA_WRITE;CSC-TEST-2806;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_coordinator ia08_dml_timeseries_schema_write_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_coordinator PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_availability COMMAND sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_TIMESERIES_SCHEMA_WRITE;CSC-TEST-2807;availability")
add_test(NAME sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_dml_timeseries_schema_write_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_TIMESERIES_SCHEMA_WRITE;CSC-TEST-2808;cancellation_fault")

add_executable(sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_runtime ia08_ddl_timeseries_series_cardinality_policy_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_SET_TIMESERIES_SERIES_CARDINALITY_POLICY;CSC-TEST-2810;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_coordinator ia08_ddl_timeseries_series_cardinality_policy_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_coordinator PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_availability COMMAND sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_SET_TIMESERIES_SERIES_CARDINALITY_POLICY;CSC-TEST-2811;availability")
add_test(NAME sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_timeseries_series_cardinality_policy_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_SET_TIMESERIES_SERIES_CARDINALITY_POLICY;CSC-TEST-2812;cancellation_fault")

add_executable(sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_runtime ia08_ddl_create_timeseries_value_cache_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TIMESERIES_VALUE_CACHE;CSC-TEST-2814;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_coordinator ia08_ddl_create_timeseries_value_cache_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_coordinator PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TIMESERIES_VALUE_CACHE;CSC-TEST-2815;availability")
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_timeseries_value_cache_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TIMESERIES_VALUE_CACHE;CSC-TEST-2816;cancellation_fault")
set_tests_properties(sbsql_sblr_alignment_ia08_dml_counter_add_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_COUNTER_ADD;CSC-TEST-2804;cancellation_fault")
set_tests_properties(sbsql_sblr_alignment_ia08_dml_async_insert_cancel_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_ASYNC_INSERT_CANCEL;CSC-TEST-002794;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_dml_async_insert_cancel_coordinator ia08_dml_async_insert_cancel_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_dml_async_insert_cancel_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_dml_async_insert_cancel_coordinator PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_dml_async_insert_cancel COMMAND sbsql_sblr_alignment_ia08_dml_async_insert_cancel_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_dml_async_insert_cancel PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DML_ASYNC_INSERT_CANCEL;CSC-TEST-002795;availability;CSC-TEST-002796;cancellation_fault")
add_executable(sbsql_sblr_alignment_ia08_refusal_alter_group_localized_name ia08_refusal_alter_group_localized_name_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_refusal_alter_group_localized_name PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_refusal_alter_group_localized_name PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_refusal_alter_group_localized_name COMMAND sbsql_sblr_alignment_ia08_refusal_alter_group_localized_name)
set_tests_properties(sbsql_sblr_alignment_ia08_refusal_alter_group_localized_name PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DIAGNOSTIC_REFUSAL;alter.group;alter.localized_name")
add_executable(sbsql_sblr_alignment_ia08_refusal_gpu_profile ia08_refusal_gpu_profile_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_refusal_gpu_profile PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_refusal_gpu_profile PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_refusal_gpu_profile COMMAND sbsql_sblr_alignment_ia08_refusal_gpu_profile)
set_tests_properties(sbsql_sblr_alignment_ia08_refusal_gpu_profile PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DIAGNOSTIC_REFUSAL;alter.gpu.profile_disable;CSC-TEST-002914;contract")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_table_as_query_runtime ia08_ddl_create_table_as_query_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_table_as_query_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_table_as_query_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_table_as_query_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_table_as_query_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_table_as_query_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TABLE_AS_QUERY;CSC-TEST-002820;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_table_runtime ia08_ddl_drop_table_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_table_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_table_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_table_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_table_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_table_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_TABLE;CSC-TEST-002802;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_drop_table_coordinator ia08_ddl_drop_table_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_table_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_table_coordinator PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_table_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_table_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_table_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_TABLE;CSC-TEST-002803;availability")
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_table_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_drop_table_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_table_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_TABLE;CSC-TEST-002804;cancellation_fault;replay")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_table_as_query_coordinator ia08_ddl_create_table_as_query_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_table_as_query_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_table_as_query_coordinator PRIVATE sb_engine_internal_api)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_table_as_query_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_table_as_query_coordinator)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_table_as_query_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_table_as_query_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_table_as_query_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TABLE_AS_QUERY;CSC-TEST-002821;availability")
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_table_as_query_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_TABLE_AS_QUERY;CSC-TEST-002822;cancellation_fault")
  add_executable(sbsql_sblr_alignment_ia08_ddl_drop_sequence_runtime ia08_ddl_drop_sequence_runtime_test.cpp)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_sequence_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_sequence_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_sequence_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_sequence_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_SEQUENCE;CSC-TEST-002906")
  add_executable(sbsql_sblr_alignment_ia08_ddl_drop_sequence_coordinator ia08_ddl_drop_sequence_coordinator_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_sequence_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_sequence_coordinator PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_sequence_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_drop_sequence_coordinator)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_sequence_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_SEQUENCE;CSC-TEST-002907")
  add_executable(sbsql_sblr_alignment_ia08_ddl_drop_sequence_availability ia08_ddl_drop_sequence_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_sequence_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_sequence_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_sequence_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_sequence_availability)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_sequence_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_SEQUENCE;CSC-TEST-002908;availability")
  add_executable(sbsql_sblr_alignment_ia08_ddl_drop_synonym_runtime ia08_drop_synonym_codec_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_synonym_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_synonym_runtime PRIVATE sb_sblr_parameter_wire_codec)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_synonym_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_synonym_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_synonym_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_SYNONYM;CSC-TEST-002946;malformed_operand")
  add_executable(sbsql_sblr_alignment_ia08_ddl_create_foreign_table_runtime ${CMAKE_CURRENT_SOURCE_DIR}/ia08_ddl_create_foreign_table_codec_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_foreign_table_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_foreign_table_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_foreign_table_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_foreign_table_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_foreign_table_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_FOREIGN_TABLE;CSC-TEST-002950;malformed_operand")
  add_executable(sbsql_sblr_alignment_ia08_ddl_create_foreign_table_availability ${CMAKE_CURRENT_SOURCE_DIR}/ia08_ddl_create_foreign_table_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_foreign_table_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_foreign_table_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_foreign_table_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_foreign_table_availability)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_foreign_table_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_FOREIGN_TABLE;CSC-TEST-002951;availability")
  add_executable(sbsql_sblr_alignment_ia08_ddl_create_foreign_table_cancellation_fault ${CMAKE_CURRENT_SOURCE_DIR}/ia08_ddl_create_foreign_table_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_foreign_table_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_foreign_table_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_foreign_table_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_foreign_table_cancellation_fault)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_foreign_table_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_FOREIGN_TABLE;CSC-TEST-002952;cancellation_fault")
  add_executable(sbsql_sblr_alignment_ia08_ddl_create_fdw_runtime ${CMAKE_CURRENT_SOURCE_DIR}/ia08_ddl_create_fdw_codec_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_fdw_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_fdw_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_fdw_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_fdw_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_fdw_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_FDW;CSC-TEST-002958;malformed_operand")
  add_executable(sbsql_sblr_alignment_ia08_ddl_create_fdw_availability ${CMAKE_CURRENT_SOURCE_DIR}/ia08_ddl_create_fdw_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_fdw_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_fdw_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_fdw_availability COMMAND sbsql_sblr_alignment_ia08_ddl_create_fdw_availability)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_fdw_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_FDW;CSC-TEST-002959;availability")
  add_executable(sbsql_sblr_alignment_ia08_ddl_create_fdw_cancellation_fault ${CMAKE_CURRENT_SOURCE_DIR}/ia08_ddl_create_fdw_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_fdw_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_fdw_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_fdw_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_create_fdw_cancellation_fault)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_fdw_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_FDW;CSC-TEST-002960;cancellation_fault")
  add_executable(sbsql_sblr_alignment_ia08_ddl_drop_fdw_runtime ${CMAKE_CURRENT_SOURCE_DIR}/ia08_ddl_drop_fdw_codec_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_fdw_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_fdw_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_fdw_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_fdw_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_fdw_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_FDW;CSC-TEST-002962;malformed_operand")
  add_executable(sbsql_sblr_alignment_ia08_ddl_drop_fdw_availability ${CMAKE_CURRENT_SOURCE_DIR}/ia08_ddl_drop_fdw_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_fdw_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_fdw_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_fdw_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_fdw_availability)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_fdw_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_FDW;CSC-TEST-002963;availability")
  add_executable(sbsql_sblr_alignment_ia08_ddl_drop_fdw_cancellation_fault ${CMAKE_CURRENT_SOURCE_DIR}/ia08_ddl_drop_fdw_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_fdw_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_fdw_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_fdw_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_drop_fdw_cancellation_fault)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_fdw_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_FDW;CSC-TEST-002964;cancellation_fault")
  add_executable(sbsql_sblr_alignment_ia08_security_create_user_runtime ${CMAKE_CURRENT_SOURCE_DIR}/ia08_security_create_user_codec_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_create_user_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_create_user_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_security_create_user_runtime COMMAND sbsql_sblr_alignment_ia08_security_create_user_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_create_user_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_CREATE_USER;CSC-TEST-002966;malformed_operand")
  add_executable(sbsql_sblr_alignment_ia08_security_create_user_availability ${CMAKE_CURRENT_SOURCE_DIR}/ia08_security_create_user_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_create_user_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_create_user_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_create_user_availability COMMAND sbsql_sblr_alignment_ia08_security_create_user_availability)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_create_user_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_CREATE_USER;CSC-TEST-002967;availability")
  add_executable(sbsql_sblr_alignment_ia08_security_create_user_cancellation_fault ${CMAKE_CURRENT_SOURCE_DIR}/ia08_security_create_user_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_create_user_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_create_user_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_create_user_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_security_create_user_cancellation_fault)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_create_user_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_CREATE_USER;CSC-TEST-002968;cancellation_fault")
  add_executable(sbsql_sblr_alignment_ia08_security_alter_user_runtime ${CMAKE_CURRENT_SOURCE_DIR}/ia08_security_alter_user_codec_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_alter_user_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_alter_user_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_security_alter_user_runtime COMMAND sbsql_sblr_alignment_ia08_security_alter_user_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_alter_user_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_ALTER_USER;CSC-TEST-002970;malformed_operand")
  add_executable(sbsql_sblr_alignment_ia08_security_alter_user_availability ${CMAKE_CURRENT_SOURCE_DIR}/ia08_security_alter_user_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_alter_user_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_alter_user_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_alter_user_availability COMMAND sbsql_sblr_alignment_ia08_security_alter_user_availability)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_alter_user_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_ALTER_USER;CSC-TEST-002971;availability")
  add_executable(sbsql_sblr_alignment_ia08_security_alter_user_cancellation_fault ${CMAKE_CURRENT_SOURCE_DIR}/ia08_security_alter_user_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_alter_user_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_alter_user_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_alter_user_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_security_alter_user_cancellation_fault)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_alter_user_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_ALTER_USER;CSC-TEST-002972;cancellation_fault")
  add_executable(sbsql_sblr_alignment_ia08_security_drop_role_availability ${CMAKE_CURRENT_SOURCE_DIR}/ia08_security_drop_role_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_drop_role_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_drop_role_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_drop_role_availability COMMAND sbsql_sblr_alignment_ia08_security_drop_role_availability)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_drop_role_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_DROP_ROLE;CSC-TEST-002970;availability")
  add_executable(sbsql_sblr_alignment_ia08_security_drop_role_cancellation_fault ${CMAKE_CURRENT_SOURCE_DIR}/ia08_security_drop_role_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_drop_role_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_drop_role_cancellation_fault PRIVATE sb_sblr_parameter_wire_codec)
  add_test(NAME sbsql_sblr_alignment_ia08_security_drop_role_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_security_drop_role_cancellation_fault)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_drop_role_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_DROP_ROLE;CSC-TEST-002971;cancellation_fault")
  add_executable(sbsql_sblr_alignment_ia08_security_create_role_availability ${CMAKE_CURRENT_SOURCE_DIR}/ia08_security_create_role_availability_test.cpp)
  add_executable(sbsql_sblr_alignment_ia08_security_drop_policy_runtime ${CMAKE_CURRENT_SOURCE_DIR}/ia08_security_drop_policy_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_drop_policy_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_drop_policy_runtime PRIVATE sb_sblr_parameter_wire_codec)
  add_test(NAME sbsql_sblr_alignment_ia08_security_drop_policy_runtime COMMAND sbsql_sblr_alignment_ia08_security_drop_policy_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_drop_policy_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_DROP_POLICY;CSC-TEST-003010;malformed_operand")
  add_executable(sbsql_sblr_alignment_ia08_security_drop_policy_availability ${CMAKE_CURRENT_SOURCE_DIR}/ia08_security_drop_policy_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_drop_policy_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_drop_policy_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_drop_policy_availability COMMAND sbsql_sblr_alignment_ia08_security_drop_policy_availability)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_drop_policy_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_DROP_POLICY;CSC-TEST-003011;availability")
  add_executable(sbsql_sblr_alignment_ia08_security_drop_policy_cancellation_fault ${CMAKE_CURRENT_SOURCE_DIR}/ia08_security_drop_policy_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_drop_policy_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_drop_policy_cancellation_fault PRIVATE sb_sblr_parameter_wire_codec)
  add_test(NAME sbsql_sblr_alignment_ia08_security_drop_policy_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_security_drop_policy_cancellation_fault)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_drop_policy_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_DROP_POLICY;CSC-TEST-003012;cancellation_fault")
  target_include_directories(sbsql_sblr_alignment_ia08_security_create_role_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_create_role_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_create_role_availability COMMAND sbsql_sblr_alignment_ia08_security_create_role_availability)
  add_executable(sbsql_sblr_alignment_ia08_security_create_role_cancellation_fault ${CMAKE_CURRENT_SOURCE_DIR}/ia08_security_create_role_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_create_role_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_create_role_cancellation_fault PRIVATE sb_sblr_parameter_wire_codec)
  add_test(NAME sbsql_sblr_alignment_ia08_security_create_role_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_security_create_role_cancellation_fault)
  add_executable(sbsql_sblr_alignment_ia08_security_create_policy_runtime ${CMAKE_CURRENT_SOURCE_DIR}/ia08_security_create_policy_codec_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_create_policy_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_create_policy_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_security_create_policy_runtime COMMAND sbsql_sblr_alignment_ia08_security_create_policy_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_create_policy_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_CREATE_POLICY;CSC-TEST-003006;malformed_operand")
  add_executable(sbsql_sblr_alignment_ia08_security_create_policy_availability ${CMAKE_CURRENT_SOURCE_DIR}/ia08_security_create_policy_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_create_policy_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_create_policy_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_create_policy_availability COMMAND sbsql_sblr_alignment_ia08_security_create_policy_availability)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_create_policy_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_CREATE_POLICY;CSC-TEST-003007;availability")
  add_executable(sbsql_sblr_alignment_ia08_security_create_policy_cancellation_fault ${CMAKE_CURRENT_SOURCE_DIR}/ia08_security_create_policy_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_create_policy_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_create_policy_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_create_policy_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_security_create_policy_cancellation_fault)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_create_policy_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_CREATE_POLICY;CSC-TEST-003008;cancellation_fault")
  add_executable(sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_runtime ia08_ddl_drop_foreign_table_codec_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_runtime PRIVATE sb_sblr_parameter_wire_codec)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_FOREIGN_TABLE;CSC-TEST-002954;malformed_operand")
  add_executable(sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_availability ia08_ddl_drop_foreign_table_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_availability)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_FOREIGN_TABLE;CSC-TEST-002955;availability")
  add_executable(sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_cancellation_fault ia08_ddl_drop_foreign_table_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_cancellation_fault)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_foreign_table_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_FOREIGN_TABLE;CSC-TEST-002956;cancellation_fault")
  add_executable(sbsql_sblr_alignment_ia08_ddl_drop_synonym_availability ia08_ddl_drop_synonym_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_synonym_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_synonym_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_synonym_availability COMMAND sbsql_sblr_alignment_ia08_ddl_drop_synonym_availability)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_synonym_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_SYNONYM;CSC-TEST-002947;availability")
  add_executable(sbsql_sblr_alignment_ia08_ddl_drop_synonym_cancellation_fault ia08_ddl_drop_synonym_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_ddl_drop_synonym_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_ddl_drop_synonym_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_synonym_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_ddl_drop_synonym_cancellation_fault)
  set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_synonym_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_SYNONYM;CSC-TEST-002948;cancellation_fault")
  add_executable(sbsql_sblr_alignment_ia08_security_alter_role_runtime ia08_security_alter_role_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_alter_role_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_alter_role_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_security_alter_role_runtime COMMAND sbsql_sblr_alignment_ia08_security_alter_role_runtime)
  add_executable(sbsql_sblr_alignment_ia08_security_alter_role_availability ia08_security_alter_role_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_alter_role_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_alter_role_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_alter_role_availability COMMAND sbsql_sblr_alignment_ia08_security_alter_role_availability)
  add_executable(sbsql_sblr_alignment_ia08_security_alter_role_cancellation_fault ia08_security_alter_role_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_alter_role_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_alter_role_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_alter_role_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_security_alter_role_cancellation_fault)
  add_executable(sbsql_sblr_alignment_ia08_security_create_group_mapping_runtime ia08_security_create_group_mapping_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_create_group_mapping_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_create_group_mapping_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_security_create_group_mapping_runtime COMMAND sbsql_sblr_alignment_ia08_security_create_group_mapping_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_create_group_mapping_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_CREATE_GROUP_MAPPING;CSC-TEST-002986;malformed_operand")
  add_executable(sbsql_sblr_alignment_ia08_security_create_group_mapping_availability ia08_security_create_group_mapping_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_create_group_mapping_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_create_group_mapping_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_create_group_mapping_availability COMMAND sbsql_sblr_alignment_ia08_security_create_group_mapping_availability)
  add_executable(sbsql_sblr_alignment_ia08_security_create_group_mapping_cancellation_fault ia08_security_create_group_mapping_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_create_group_mapping_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_create_group_mapping_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_create_group_mapping_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_security_create_group_mapping_cancellation_fault)
  add_test(NAME sbsql_sblr_alignment_ia08_security_create_group_mapping_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-create-group-mapping --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_security_create_group_mapping_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_security_create_group_mapping_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_CREATE_GROUP_MAPPING;CSC-TEST-002985;e2e" TIMEOUT 240)
  add_executable(sbsql_sblr_alignment_ia08_security_drop_group_mapping_runtime ia08_security_drop_group_mapping_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_drop_group_mapping_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_drop_group_mapping_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_security_drop_group_mapping_runtime COMMAND sbsql_sblr_alignment_ia08_security_drop_group_mapping_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_drop_group_mapping_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_DROP_GROUP_MAPPING;CSC-TEST-003022;malformed_operand")
  add_executable(sbsql_sblr_alignment_ia08_security_drop_group_mapping_availability ia08_security_drop_group_mapping_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_drop_group_mapping_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_drop_group_mapping_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_drop_group_mapping_availability COMMAND sbsql_sblr_alignment_ia08_security_drop_group_mapping_availability)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_drop_group_mapping_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_DROP_GROUP_MAPPING;CSC-TEST-003023;availability")
  add_executable(sbsql_sblr_alignment_ia08_security_drop_group_mapping_cancellation_fault ia08_security_drop_group_mapping_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_drop_group_mapping_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_drop_group_mapping_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_drop_group_mapping_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_security_drop_group_mapping_cancellation_fault)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_drop_group_mapping_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_DROP_GROUP_MAPPING;CSC-TEST-003024;cancellation_fault")
  add_test(NAME sbsql_sblr_alignment_ia08_security_drop_group_mapping_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-drop-group-mapping --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_security_drop_group_mapping_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_security_drop_group_mapping_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SEC_DROP_GROUP_MAPPING;CSC-TEST-003021;e2e" TIMEOUT 240)
  add_executable(sbsql_sblr_alignment_ia08_security_grant_runtime ia08_security_grant_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_grant_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_grant_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_security_grant_runtime COMMAND sbsql_sblr_alignment_ia08_security_grant_runtime)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_grant_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SEC_GRANT;CSC-TEST-002978;malformed_operand")
  add_executable(sbsql_sblr_alignment_ia08_security_grant_availability ia08_security_grant_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_grant_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_grant_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_grant_availability COMMAND sbsql_sblr_alignment_ia08_security_grant_availability)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_grant_availability PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SEC_GRANT;CSC-TEST-002979;availability")
  add_executable(sbsql_sblr_alignment_ia08_security_grant_cancellation_fault ia08_security_grant_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_grant_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_grant_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_grant_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_security_grant_cancellation_fault)
  set_tests_properties(sbsql_sblr_alignment_ia08_security_grant_cancellation_fault PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SEC_GRANT;CSC-TEST-002980;cancellation_fault")
  add_test(NAME sbsql_sblr_alignment_ia08_security_grant_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-grant --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_security_grant_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_security_grant_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SEC_GRANT;CSC-TEST-002977;e2e" TIMEOUT 240)
  add_executable(sbsql_sblr_alignment_ia08_security_revoke_runtime ia08_security_revoke_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_revoke_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_revoke_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_security_revoke_runtime COMMAND sbsql_sblr_alignment_ia08_security_revoke_runtime)
  add_executable(sbsql_sblr_alignment_ia08_security_revoke_availability ia08_security_revoke_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_revoke_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_revoke_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_revoke_availability COMMAND sbsql_sblr_alignment_ia08_security_revoke_availability)
  add_executable(sbsql_sblr_alignment_ia08_security_revoke_cancellation_fault ia08_security_revoke_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_revoke_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_revoke_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_revoke_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_security_revoke_cancellation_fault)
  add_test(NAME sbsql_sblr_alignment_ia08_security_revoke_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation security-revoke --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_security_revoke_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia08_security_revoke_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-09;SBLR_SEC_REVOKE;CSC-TEST-002981;e2e" TIMEOUT 240)
  add_executable(sbsql_sblr_alignment_ia08_security_alter_policy_runtime ia08_security_alter_policy_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_alter_policy_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_alter_policy_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_security_alter_policy_runtime COMMAND sbsql_sblr_alignment_ia08_security_alter_policy_runtime)
  add_executable(sbsql_sblr_alignment_ia08_security_alter_policy_availability ia08_security_alter_policy_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_alter_policy_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_alter_policy_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_alter_policy_availability COMMAND sbsql_sblr_alignment_ia08_security_alter_policy_availability)
  add_executable(sbsql_sblr_alignment_ia08_security_alter_policy_cancellation_fault ia08_security_alter_policy_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_alter_policy_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_alter_policy_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_alter_policy_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_security_alter_policy_cancellation_fault)
  add_executable(sbsql_sblr_alignment_ia08_security_drop_user_runtime ia08_security_drop_user_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_drop_user_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_drop_user_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_security_drop_user_runtime COMMAND sbsql_sblr_alignment_ia08_security_drop_user_runtime)
  add_executable(sbsql_sblr_alignment_ia08_security_drop_user_availability ia08_security_drop_user_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_drop_user_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_drop_user_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_drop_user_availability COMMAND sbsql_sblr_alignment_ia08_security_drop_user_availability)
  add_executable(sbsql_sblr_alignment_ia08_security_drop_user_cancellation_fault ia08_security_drop_user_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_drop_user_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_drop_user_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_drop_user_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_security_drop_user_cancellation_fault)
  add_executable(sbsql_sblr_alignment_ia08_security_authenticate_runtime ia08_security_authenticate_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_authenticate_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_authenticate_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_security_authenticate_runtime COMMAND sbsql_sblr_alignment_ia08_security_authenticate_runtime)
  add_executable(sbsql_sblr_alignment_ia08_security_authenticate_availability ia08_security_authenticate_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_authenticate_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_authenticate_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_authenticate_availability COMMAND sbsql_sblr_alignment_ia08_security_authenticate_availability)
  add_executable(sbsql_sblr_alignment_ia08_security_authenticate_cancellation_fault ia08_security_authenticate_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_authenticate_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_authenticate_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_authenticate_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_security_authenticate_cancellation_fault)
  add_executable(sbsql_sblr_alignment_ia08_security_deauthenticate_runtime ia08_security_deauthenticate_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_deauthenticate_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_deauthenticate_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_security_deauthenticate_runtime COMMAND sbsql_sblr_alignment_ia08_security_deauthenticate_runtime)
  add_executable(sbsql_sblr_alignment_ia08_security_deauthenticate_availability ia08_security_deauthenticate_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_deauthenticate_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_deauthenticate_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_deauthenticate_availability COMMAND sbsql_sblr_alignment_ia08_security_deauthenticate_availability)
  add_executable(sbsql_sblr_alignment_ia08_security_deauthenticate_cancellation_fault ia08_security_deauthenticate_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_security_deauthenticate_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_security_deauthenticate_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_security_deauthenticate_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_security_deauthenticate_cancellation_fault)
  add_executable(sbsql_sblr_alignment_ia08_session_role_switch_runtime ia08_session_role_switch_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_role_switch_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_role_switch_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_session_role_switch_runtime COMMAND sbsql_sblr_alignment_ia08_session_role_switch_runtime)
  add_executable(sbsql_sblr_alignment_ia08_session_role_switch_availability ia08_session_role_switch_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_role_switch_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_role_switch_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_session_role_switch_availability COMMAND sbsql_sblr_alignment_ia08_session_role_switch_availability)
  add_executable(sbsql_sblr_alignment_ia08_session_role_switch_cancellation_fault ia08_session_role_switch_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_role_switch_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_role_switch_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_session_role_switch_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_session_role_switch_cancellation_fault)
  add_executable(sbsql_sblr_alignment_ia08_session_setting_set_runtime ia08_session_setting_set_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_setting_set_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_setting_set_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_session_setting_set_runtime COMMAND sbsql_sblr_alignment_ia08_session_setting_set_runtime)
  add_executable(sbsql_sblr_alignment_ia08_session_setting_set_availability ia08_session_setting_set_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_setting_set_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_setting_set_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_session_setting_set_availability COMMAND sbsql_sblr_alignment_ia08_session_setting_set_availability)
  add_executable(sbsql_sblr_alignment_ia08_session_setting_set_cancellation_fault ia08_session_setting_set_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_setting_set_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_setting_set_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_session_setting_set_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_session_setting_set_cancellation_fault)
  add_executable(sbsql_sblr_alignment_ia08_session_setting_reset_runtime ia08_session_setting_reset_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_setting_reset_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_setting_reset_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_session_setting_reset_runtime COMMAND sbsql_sblr_alignment_ia08_session_setting_reset_runtime)
  add_executable(sbsql_sblr_alignment_ia08_session_setting_reset_availability ia08_session_setting_reset_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_setting_reset_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_setting_reset_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_session_setting_reset_availability COMMAND sbsql_sblr_alignment_ia08_session_setting_reset_availability)
  add_executable(sbsql_sblr_alignment_ia08_session_setting_reset_cancellation_fault ia08_session_setting_reset_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_setting_reset_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_setting_reset_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_session_setting_reset_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_session_setting_reset_cancellation_fault)
  add_executable(sbsql_sblr_alignment_ia08_session_setting_get_runtime ia08_session_setting_get_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_setting_get_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_setting_get_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_session_setting_get_runtime COMMAND sbsql_sblr_alignment_ia08_session_setting_get_runtime)
  add_executable(sbsql_sblr_alignment_ia08_session_setting_get_availability ia08_session_setting_get_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_setting_get_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_setting_get_availability PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_session_setting_get_availability COMMAND sbsql_sblr_alignment_ia08_session_setting_get_availability)
  add_executable(sbsql_sblr_alignment_ia08_session_setting_get_cancellation_fault ia08_session_setting_get_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_setting_get_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_setting_get_cancellation_fault PRIVATE sb_engine_internal_api)
  add_test(NAME sbsql_sblr_alignment_ia08_session_setting_get_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_session_setting_get_cancellation_fault)
  add_executable(sbsql_sblr_alignment_ia08_session_default_qualifier_runtime ia08_session_default_qualifier_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_default_qualifier_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_default_qualifier_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_session_default_qualifier_runtime COMMAND sbsql_sblr_alignment_ia08_session_default_qualifier_runtime)
  add_executable(sbsql_sblr_alignment_ia08_session_default_qualifier_availability ia08_session_default_qualifier_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_default_qualifier_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_default_qualifier_availability PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_session_default_qualifier_availability COMMAND sbsql_sblr_alignment_ia08_session_default_qualifier_availability)
  add_executable(sbsql_sblr_alignment_ia08_session_default_qualifier_cancellation_fault ia08_session_default_qualifier_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_default_qualifier_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_default_qualifier_cancellation_fault PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_session_default_qualifier_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_session_default_qualifier_cancellation_fault)
  add_executable(sbsql_sblr_alignment_ia08_session_discard_runtime ia08_session_discard_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_discard_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_discard_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_session_discard_runtime COMMAND sbsql_sblr_alignment_ia08_session_discard_runtime)
  add_executable(sbsql_sblr_alignment_ia08_session_discard_availability ia08_session_discard_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_discard_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_discard_availability PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_session_discard_availability COMMAND sbsql_sblr_alignment_ia08_session_discard_availability)
  add_executable(sbsql_sblr_alignment_ia08_session_discard_cancellation_fault ia08_session_discard_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_discard_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_discard_cancellation_fault PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_session_discard_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_session_discard_cancellation_fault)
  add_executable(sbsql_sblr_alignment_ia08_session_snapshot_handle_runtime ia08_session_snapshot_handle_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_snapshot_handle_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_snapshot_handle_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_session_snapshot_handle_runtime COMMAND sbsql_sblr_alignment_ia08_session_snapshot_handle_runtime)
  add_executable(sbsql_sblr_alignment_ia08_session_snapshot_handle_availability ia08_session_snapshot_handle_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_snapshot_handle_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_snapshot_handle_availability PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_session_snapshot_handle_availability COMMAND sbsql_sblr_alignment_ia08_session_snapshot_handle_availability)
  add_executable(sbsql_sblr_alignment_ia08_session_snapshot_handle_cancellation_fault ia08_session_snapshot_handle_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_session_snapshot_handle_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_session_snapshot_handle_cancellation_fault PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_session_snapshot_handle_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_session_snapshot_handle_cancellation_fault)
  add_executable(sbsql_sblr_alignment_ia08_context_set_runtime ia08_context_set_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_context_set_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_context_set_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_context_set_runtime COMMAND sbsql_sblr_alignment_ia08_context_set_runtime)
  add_executable(sbsql_sblr_alignment_ia08_context_set_availability ia08_context_set_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_context_set_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_context_set_availability PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_context_set_availability COMMAND sbsql_sblr_alignment_ia08_context_set_availability)
  add_executable(sbsql_sblr_alignment_ia08_context_set_cancellation_fault ia08_context_set_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_context_set_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_context_set_cancellation_fault PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_context_set_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_context_set_cancellation_fault)
  add_executable(sbsql_sblr_alignment_ia08_context_unset_runtime ia08_context_unset_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_context_unset_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_context_unset_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_context_unset_runtime COMMAND sbsql_sblr_alignment_ia08_context_unset_runtime)
  add_executable(sbsql_sblr_alignment_ia08_context_unset_availability ia08_context_unset_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_context_unset_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_context_unset_availability PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_context_unset_availability COMMAND sbsql_sblr_alignment_ia08_context_unset_availability)
  add_executable(sbsql_sblr_alignment_ia08_context_unset_cancellation_fault ia08_context_unset_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_context_unset_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_context_unset_cancellation_fault PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_context_unset_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_context_unset_cancellation_fault)
  add_executable(sbsql_sblr_alignment_ia08_context_get_runtime ia08_context_get_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_context_get_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_context_get_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_context_get_runtime COMMAND sbsql_sblr_alignment_ia08_context_get_runtime)
  add_executable(sbsql_sblr_alignment_ia08_context_get_availability ia08_context_get_availability_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_context_get_availability PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_context_get_availability PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_context_get_availability COMMAND sbsql_sblr_alignment_ia08_context_get_availability)
  add_executable(sbsql_sblr_alignment_ia08_context_get_cancellation_fault ia08_context_get_cancellation_fault_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_context_get_cancellation_fault PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_context_get_cancellation_fault PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_context_get_cancellation_fault COMMAND sbsql_sblr_alignment_ia08_context_get_cancellation_fault)
  add_executable(sbsql_sblr_alignment_ia08_stmt_prepare_runtime ia08_stmt_prepare_runtime_test.cpp)
  target_include_directories(sbsql_sblr_alignment_ia08_stmt_prepare_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia08_stmt_prepare_runtime PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia08_stmt_prepare_runtime COMMAND sbsql_sblr_alignment_ia08_stmt_prepare_runtime)
  add_test(NAME sbsql_sblr_alignment_ia10_stmt_execute_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation stmt-execute
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia10_stmt_execute_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia10_stmt_execute_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_STMT_EXECUTE;CSC-TEST-003009;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia11_stmt_execute_direct_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation stmt-execute-direct
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia11_stmt_execute_direct_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia11_stmt_execute_direct_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_STMT_EXECUTE_DIRECT;CSC-TEST-003010;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia12_stmt_free_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation stmt-free
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia12_stmt_free_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia12_stmt_free_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_STMT_FREE;CSC-TEST-003011;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia13_stmt_cancel_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation stmt-cancel
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia13_stmt_cancel_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia13_stmt_cancel_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_STMT_CANCEL;CSC-TEST-003012;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia14_parameter_bind_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation parameter-bind
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia14_parameter_bind_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia14_parameter_bind_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_PARAMETER_BIND;CSC-TEST-003013;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia15_result_page_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation result-page
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia15_result_page_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia15_result_page_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_RESULT_PAGE;CSC-TEST-003014;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia16_query_execute_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation query-execute
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia16_query_execute_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia16_query_execute_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_QUERY_EXECUTE;CSC-TEST-003015;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia17_query_explain_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation query-explain
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia17_query_explain_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia17_query_explain_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_QUERY_EXPLAIN;CSC-TEST-003016;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia18_name_resolve_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation name-resolve
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia18_name_resolve_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia18_name_resolve_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_NAME_RESOLVE;CSC-TEST-003017;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia19_parse_text_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation parse-text
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia19_parse_text_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia19_parse_text_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_PARSE_TEXT;CSC-TEST-003018;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia20_catalog_epoch_check_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation catalog-epoch-check
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia20_catalog_epoch_check_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia20_catalog_epoch_check_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_CATALOG_EPOCH_CHECK;CSC-TEST-003019;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia21_database_attach_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation database-attach
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia21_database_attach_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia21_database_attach_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_DATABASE_ATTACH;CSC-TEST-003020;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia22_database_detach_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation database-detach
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia22_database_detach_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia22_database_detach_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_DATABASE_DETACH;CSC-TEST-003021;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia23_database_checkpoint_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation database-checkpoint
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia23_database_checkpoint_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia23_database_checkpoint_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_DATABASE_CHECKPOINT;CSC-TEST-003022;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia24_database_vacuum_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation database-vacuum
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia24_database_vacuum_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia24_database_vacuum_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_DATABASE_VACUUM;CSC-TEST-003023;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia25_database_alter_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation database-alter
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia25_database_alter_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia25_database_alter_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_DATABASE_ALTER;CSC-TEST-003024;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia26_lifecycle_create_database_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation lifecycle-create-database
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia26_lifecycle_create_database_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia26_lifecycle_create_database_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_LIFECYCLE_CREATE_DATABASE;CSC-TEST-003025;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia27_lifecycle_open_database_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation lifecycle-open-database
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia27_lifecycle_open_database_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia27_lifecycle_open_database_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_LIFECYCLE_OPEN_DATABASE;CSC-TEST-003026;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia28_lifecycle_attach_database_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation lifecycle-attach-database
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia28_lifecycle_attach_database_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia28_lifecycle_attach_database_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_LIFECYCLE_ATTACH_DATABASE;CSC-TEST-003027;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia29_lifecycle_detach_database_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation lifecycle-detach-database
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia29_lifecycle_detach_database_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia29_lifecycle_detach_database_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_LIFECYCLE_DETACH_DATABASE;CSC-TEST-003028;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia30_lifecycle_enter_maintenance_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation lifecycle-enter-maintenance
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia30_lifecycle_enter_maintenance_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia30_lifecycle_enter_maintenance_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_LIFECYCLE_ENTER_MAINTENANCE;CSC-TEST-003029;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia31_lifecycle_exit_maintenance_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation lifecycle-exit-maintenance
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia31_lifecycle_exit_maintenance_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia31_lifecycle_exit_maintenance_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_LIFECYCLE_EXIT_MAINTENANCE;CSC-TEST-003030;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia32_lifecycle_enter_restricted_open_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation lifecycle-enter-restricted-open
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia32_lifecycle_enter_restricted_open_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia32_lifecycle_enter_restricted_open_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_LIFECYCLE_ENTER_RESTRICTED_OPEN;CSC-TEST-003031;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia33_lifecycle_exit_restricted_open_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation lifecycle-exit-restricted-open
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia33_lifecycle_exit_restricted_open_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia33_lifecycle_exit_restricted_open_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_LIFECYCLE_EXIT_RESTRICTED_OPEN;CSC-TEST-003032;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia34_lifecycle_inspect_database_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation lifecycle-inspect-database
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia34_lifecycle_inspect_database_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia34_lifecycle_inspect_database_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_LIFECYCLE_INSPECT_DATABASE;CSC-TEST-003033;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia35_lifecycle_verify_database_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation lifecycle-verify-database
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia35_lifecycle_verify_database_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia35_lifecycle_verify_database_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_LIFECYCLE_VERIFY_DATABASE;CSC-TEST-003034;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia36_lifecycle_repair_database_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation lifecycle-repair-database
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia36_lifecycle_repair_database_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia36_lifecycle_repair_database_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_LIFECYCLE_REPAIR_DATABASE;CSC-TEST-003035;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia37_lifecycle_shutdown_database_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation lifecycle-shutdown-database
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia37_lifecycle_shutdown_database_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia37_lifecycle_shutdown_database_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_LIFECYCLE_SHUTDOWN_DATABASE;CSC-TEST-003036;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia38_lifecycle_shutdown_force_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation lifecycle-shutdown-force
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia38_lifecycle_shutdown_force_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia38_lifecycle_shutdown_force_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_LIFECYCLE_SHUTDOWN_FORCE;CSC-TEST-003037;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia39_lifecycle_shutdown_acknowledge_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation lifecycle-shutdown-acknowledge
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia39_lifecycle_shutdown_acknowledge_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia39_lifecycle_shutdown_acknowledge_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_LIFECYCLE_SHUTDOWN_ACKNOWLEDGE;CSC-TEST-003038;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia40_lifecycle_drop_database_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation lifecycle-drop-database
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia40_lifecycle_drop_database_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia40_lifecycle_drop_database_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_LIFECYCLE_DROP_DATABASE;CSC-TEST-003039;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia41_repl_consumer_subscribe_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation repl-consumer-subscribe
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia41_repl_consumer_subscribe_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia41_repl_consumer_subscribe_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_REPL_CONSUMER_SUBSCRIBE;CSC-TEST-003040;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia42_repl_consumer_resume_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation repl-consumer-resume
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia42_repl_consumer_resume_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia42_repl_consumer_resume_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_REPL_CONSUMER_RESUME;CSC-TEST-003041;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia43_repl_consumer_pause_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation repl-consumer-pause
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia43_repl_consumer_pause_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia43_repl_consumer_pause_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_REPL_CONSUMER_PAUSE;CSC-TEST-003042;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia44_repl_consumer_cancel_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation repl-consumer-cancel
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia44_repl_consumer_cancel_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia44_repl_consumer_cancel_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_REPL_CONSUMER_CANCEL;CSC-TEST-003043;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia45_repl_cdc_receive_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation repl-cdc-receive
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia45_repl_cdc_receive_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia45_repl_cdc_receive_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_REPL_CDC_RECEIVE;CSC-TEST-003044;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia46_repl_cdc_ack_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation repl-cdc-ack
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia46_repl_cdc_ack_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia46_repl_cdc_ack_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_REPL_CDC_ACK;CSC-TEST-003045;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia47_repl_2pc_prewrite_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation repl-2pc-prewrite
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia47_repl_2pc_prewrite_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia47_repl_2pc_prewrite_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_REPL_2PC_PREWRITE;CSC-TEST-003046;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia48_repl_2pc_commit_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation repl-2pc-commit
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia48_repl_2pc_commit_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia48_repl_2pc_commit_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_REPL_2PC_COMMIT;CSC-TEST-003047;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia49_repl_2pc_cleanup_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation repl-2pc-cleanup
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia49_repl_2pc_cleanup_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia49_repl_2pc_cleanup_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_REPL_2PC_CLEANUP;CSC-TEST-003048;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia50_repl_2pc_resolve_lock_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation repl-2pc-resolve-lock
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia50_repl_2pc_resolve_lock_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia50_repl_2pc_resolve_lock_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_REPL_2PC_RESOLVE_LOCK;CSC-TEST-003049;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia51_repl_2pc_pessimistic_lock_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation repl-2pc-pessimistic-lock
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia51_repl_2pc_pessimistic_lock_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia51_repl_2pc_pessimistic_lock_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_REPL_2PC_PESSIMISTIC_LOCK;CSC-TEST-003050;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia52_repl_2pc_pessimistic_rollback_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation repl-2pc-pessimistic-rollback
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia52_repl_2pc_pessimistic_rollback_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia52_repl_2pc_pessimistic_rollback_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_REPL_2PC_PESSIMISTIC_ROLLBACK;CSC-TEST-003051;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia53_repl_2pc_heartbeat_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation repl-2pc-heartbeat
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia53_repl_2pc_heartbeat_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia53_repl_2pc_heartbeat_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_REPL_2PC_HEARTBEAT;CSC-TEST-003052;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia54_repl_2pc_check_status_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation repl-2pc-check-status
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia54_repl_2pc_check_status_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia54_repl_2pc_check_status_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_REPL_2PC_CHECK_STATUS;CSC-TEST-003053;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia55_graph_traverse_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation graph-traverse
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia55_graph_traverse_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia55_graph_traverse_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_GRAPH_TRAVERSE;CSC-TEST-003054;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia56_graph_optional_match_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation graph-optional-match
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia56_graph_optional_match_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia56_graph_optional_match_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_GRAPH_OPTIONAL_MATCH;CSC-TEST-003055;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia57_graph_create_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation graph-create
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia57_graph_create_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia57_graph_create_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_GRAPH_CREATE;CSC-TEST-003056;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia58_graph_merge_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation graph-merge
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia58_graph_merge_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia58_graph_merge_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_GRAPH_MERGE;CSC-TEST-003057;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia59_graph_set_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation graph-set
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia59_graph_set_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia59_graph_set_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_GRAPH_SET;CSC-TEST-003058;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia60_graph_remove_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation graph-remove
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia60_graph_remove_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia60_graph_remove_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_GRAPH_REMOVE;CSC-TEST-003059;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia61_graph_delete_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation graph-delete
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia61_graph_delete_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia61_graph_delete_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_GRAPH_DELETE;CSC-TEST-003060;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia62_graph_detach_delete_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation graph-detach-delete
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia62_graph_detach_delete_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia62_graph_detach_delete_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_GRAPH_DETACH_DELETE;CSC-TEST-003061;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia63_fulltext_score_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation fulltext-score
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia63_fulltext_score_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia63_fulltext_score_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_FULLTEXT_SCORE;CSC-TEST-003062;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia64_fulltext_phrase_score_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation fulltext-phrase-score
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia64_fulltext_phrase_score_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia64_fulltext_phrase_score_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_FULLTEXT_PHRASE_SCORE;CSC-TEST-003063;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia65_fulltext_multi_field_score_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation fulltext-multi-field-score
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia65_fulltext_multi_field_score_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia65_fulltext_multi_field_score_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_FULLTEXT_MULTI_FIELD_SCORE;CSC-TEST-003064;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia66_fulltext_regex_match_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation fulltext-regex-match
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia66_fulltext_regex_match_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia66_fulltext_regex_match_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_FULLTEXT_REGEX_MATCH;CSC-TEST-003065;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia67_fulltext_wildcard_match_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation fulltext-wildcard-match
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia67_fulltext_wildcard_match_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia67_fulltext_wildcard_match_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_FULLTEXT_WILDCARD_MATCH;CSC-TEST-003066;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia68_fulltext_prefix_match_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation fulltext-prefix-match
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia68_fulltext_prefix_match_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia68_fulltext_prefix_match_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_FULLTEXT_PREFIX_MATCH;CSC-TEST-003067;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia69_fulltext_analyzer_apply_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation fulltext-analyzer-apply
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia69_fulltext_analyzer_apply_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia69_fulltext_analyzer_apply_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_FULLTEXT_ANALYZER_APPLY;CSC-TEST-003068;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia70_diagnostic_refusal_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation diagnostic-refusal
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia70_diagnostic_refusal_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia70_diagnostic_refusal_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_DIAGNOSTIC_REFUSAL;CSC-TEST-003069;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia71_diagnostic_reset_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation diagnostic-reset
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia71_diagnostic_reset_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia71_diagnostic_reset_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_DIAGNOSTIC_RESET;CSC-TEST-003070;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia72_descriptor_transform_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation descriptor-transform
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia72_descriptor_transform_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia72_descriptor_transform_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_DESCRIPTOR_TRANSFORM;CSC-TEST-003071;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia73_migration_begin_donor_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation migration-begin-donor
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia73_migration_begin_donor_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia73_migration_begin_donor_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_MIGRATION_BEGIN_DONOR;CSC-TEST-003072;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia74_migration_alter_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation migration-alter
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia74_migration_alter_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia74_migration_alter_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_MIGRATION_ALTER;CSC-TEST-003073;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia75_show_migration_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation show-migration
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia75_show_migration_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia75_show_migration_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_SHOW_MIGRATION;CSC-TEST-003074;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia76_migration_cutover_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation migration-cutover
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia76_migration_cutover_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia76_migration_cutover_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_MIGRATION_CUTOVER;CSC-TEST-003075;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia77_migration_rollback_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation migration-rollback
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia77_migration_rollback_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia77_migration_rollback_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_MIGRATION_ROLLBACK;CSC-TEST-003076;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia78_migration_retain_evidence_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation migration-retain-evidence
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia78_migration_retain_evidence_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia78_migration_retain_evidence_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_MIGRATION_RETAIN_EVIDENCE;CSC-TEST-003077;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia79_internal_trigger_dispatch_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation internal-trigger-dispatch
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia79_internal_trigger_dispatch_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia79_internal_trigger_dispatch_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_INTERNAL_TRIGGER_DISPATCH;CSC-TEST-003078;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia80_internal_exception_raise_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation internal-exception-raise
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia80_internal_exception_raise_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia80_internal_exception_raise_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_INTERNAL_EXCEPTION_RAISE;CSC-TEST-003079;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia81_internal_exception_resignal_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation internal-exception-resignal
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia81_internal_exception_resignal_e2e")
  set_tests_properties(sbsql_sblr_alignment_ia81_internal_exception_resignal_process_e2e PROPERTIES
    LABELS "sbsql_sblr_alignment;IA-08;SBLR_INTERNAL_EXCEPTION_RESIGNAL;CSC-TEST-003080;e2e" TIMEOUT 240)
  add_test(NAME sbsql_sblr_alignment_ia08_security_alter_role_process_e2e
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py"
      --server $<TARGET_FILE:sb_server>
      --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client>
      --operation security-alter-role
      --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_security_alter_role_e2e")
set_tests_properties(sbsql_sblr_alignment_ia08_security_alter_role_process_e2e PROPERTIES
      LABELS "sbsql_sblr_alignment;IA-08;SBLR_SEC_ALTER_ROLE;CSC-TEST-002997;e2e" TIMEOUT 240)

add_executable(sbsql_sblr_alignment_ia08_ddl_create_extension_runtime ia08_ddl_create_extension_runtime_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_extension_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_extension_runtime PRIVATE sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_extension_runtime COMMAND sbsql_sblr_alignment_ia08_ddl_create_extension_runtime)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_extension_runtime PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_EXTENSION;CSC-TEST-004050;malformed_operand")
add_executable(sbsql_sblr_alignment_ia08_ddl_create_extension_coordinator ia08_ddl_create_extension_coordinator_test.cpp)
target_include_directories(sbsql_sblr_alignment_ia08_ddl_create_extension_coordinator PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sbsql_sblr_alignment_ia08_ddl_create_extension_coordinator PRIVATE sb_engine_internal_api sb_engine_sblr)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_extension_coordinator COMMAND sbsql_sblr_alignment_ia08_ddl_create_extension_coordinator)
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_extension_coordinator PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_EXTENSION;CSC-TEST-004051;availability;cancellation_fault")
add_test(NAME sbsql_sblr_alignment_ia08_ddl_create_extension_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-create-extension --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_create_extension_e2e")
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_create_extension_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_CREATE_EXTENSION;CSC-TEST-004049;e2e" TIMEOUT 240)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_alter_extension_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-alter-extension --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_alter_extension_e2e")
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_alter_extension_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_ALTER_EXTENSION;CSC-TEST-004053;e2e" TIMEOUT 240)
add_test(NAME sbsql_sblr_alignment_ia08_ddl_drop_extension_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation ddl-drop-extension --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_ddl_drop_extension_e2e")
set_tests_properties(sbsql_sblr_alignment_ia08_ddl_drop_extension_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_DDL_DROP_EXTENSION;CSC-TEST-004057;e2e" TIMEOUT 240)
add_test(NAME sbsql_sblr_alignment_ia08_cluster_create_placement_policy_process_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ia01_source_map_process_e2e.py" --server $<TARGET_FILE:sb_server> --client $<TARGET_FILE:sbsql_sblr_alignment_ia01_source_map_process_client> --operation cluster-create-placement-policy --work-dir "${CMAKE_BINARY_DIR}/sbsql_sblr_alignment_ia08_cluster_create_placement_policy_e2e")
set_tests_properties(sbsql_sblr_alignment_ia08_cluster_create_placement_policy_process_e2e PROPERTIES LABELS "sbsql_sblr_alignment;IA-08;SBLR_CLUSTER_CREATE_PLACEMENT_POLICY;CSC-TEST-004073;e2e" TIMEOUT 240)

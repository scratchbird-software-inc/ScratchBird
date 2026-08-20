# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(SB_BUILD_QOW_CLOSURE_TESTS)
  if(NOT TARGET sb_qow_optimizer_admission)
    add_subdirectory(
      ${CMAKE_CURRENT_SOURCE_DIR}/../optimizer
      ${CMAKE_CURRENT_BINARY_DIR}/qow_engine_optimizer
    )
  endif()
  # QOW-CMAKE-QRY-003-V1
  # QOW-ROUTE-STAGE-QRY-003-V1
  add_library(sb_qow_sblr_query_route_contract STATIC
    canonical_relational_expression.cpp
    canonical_query_execute.cpp
    sblr_dispatch.cpp
    sblr_engine_envelope.cpp
    sblr_opcode_registry.cpp
  )
  target_compile_features(sb_qow_sblr_query_route_contract PUBLIC cxx_std_20)
  target_compile_definitions(sb_qow_sblr_query_route_contract PRIVATE
    SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY=1
  )
  target_include_directories(sb_qow_sblr_query_route_contract PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
  )
  target_link_libraries(sb_qow_sblr_query_route_contract PUBLIC
    sb_qow_relational_dag_contract
    sb_qow_typed_scalar_descriptor_contract
    sb_qow_optimizer_admission
  )
  set(SB_QOW_SBLR_TESTS_ENROLLED TRUE PARENT_SCOPE)
  return()
endif()

# Pure canonical parameter wire codecs shared by parser producers and by the
# server/engine's independent revalidation path. This target contains no
# catalog, MGA, admission, executor, or optimizer authority.
add_library(sb_sblr_parameter_wire_codec STATIC
  sblr_parameter_runtime.cpp
  sblr_variable_runtime.cpp
  sblr_source_map_runtime.cpp
  sblr_error_vector_runtime.cpp
    sblr_transaction_begin_runtime.cpp
    sblr_transaction_commit_runtime.cpp
    sblr_transaction_rollback_runtime.cpp
  sblr_savepoint_runtime.cpp
  sblr_autonomous_frame_runtime.cpp
  sblr_reservation_release_runtime.cpp
  sblr_temporary_instance_cleanup_runtime.cpp
  sblr_cursor_open_runtime.cpp
    sblr_cursor_fetch_runtime.cpp
    sblr_cursor_close_runtime.cpp
    sblr_read_by_key_runtime.cpp
    sblr_read_range_runtime.cpp
    sblr_read_stream_runtime.cpp
    sblr_result_set_pass_runtime.cpp
    sblr_access_cursor_open_runtime.cpp
    sblr_access_cursor_fetch_runtime.cpp
    sblr_access_cursor_close_runtime.cpp
    sblr_insert_runtime.cpp
    sblr_update_runtime.cpp
    sblr_delete_runtime.cpp
    sblr_merge_runtime.cpp
    sblr_table_truncate_runtime.cpp
  sblr_table_analyze_runtime.cpp
  sblr_bulk_import_stream_runtime.cpp
  sblr_bulk_export_stream_runtime.cpp
  sblr_statement_batch_runtime.cpp
  sblr_atomic_cas_runtime.cpp
  sblr_atomic_read_modify_write_runtime.cpp
  sblr_advisory_lock_runtime.cpp
  sblr_advisory_lock_release_runtime.cpp
  sblr_function_call_runtime.cpp
  sblr_operator_call_runtime.cpp
  sblr_cast_runtime.cpp
  sblr_compare_runtime.cpp
  sblr_domain_operation_runtime.cpp
  sblr_udr_invoke_runtime.cpp
  sblr_procedure_invoke_runtime.cpp
  sblr_function_invoke_runtime.cpp
  sblr_aggregate_invoke_runtime.cpp
  sblr_sequence_nextval_runtime.cpp
  sblr_sequence_setval_runtime.cpp
  sblr_query_numeric_runtime.cpp
  sblr_advanced_datatype_family_runtime.cpp
  sblr_project_runtime.cpp
  sblr_aggregate_runtime.cpp
  sblr_group_runtime.cpp
  sblr_sort_runtime.cpp
  sblr_limit_runtime.cpp
  sblr_window_runtime.cpp
  sblr_kv_structured_read_runtime.cpp
  sblr_kv_structured_mutate_runtime.cpp
  sblr_kv_structured_scan_runtime.cpp
  sblr_kv_structured_stream_read_runtime.cpp
  sblr_kv_structured_stream_append_runtime.cpp
  sblr_kv_structured_timeseries_runtime.cpp
  sblr_system_config_set_runtime.cpp
  sblr_ddl_create_domain_runtime.cpp
  sblr_ddl_create_schema_runtime.cpp
  sblr_ddl_create_table_runtime.cpp
  sblr_ddl_create_index_runtime.cpp
  sblr_ddl_drop_index_runtime.cpp
  sblr_ddl_alter_domain_runtime.cpp
  sblr_ddl_create_view_runtime.cpp
  sblr_ddl_alter_view_runtime.cpp
  sblr_ddl_drop_view_runtime.cpp
  sblr_ddl_create_trigger_runtime.cpp
  sblr_ddl_alter_trigger_runtime.cpp
  sblr_ddl_create_procedure_runtime.cpp
  sblr_ddl_alter_procedure_runtime.cpp
  sblr_ddl_drop_procedure_runtime.cpp
  sblr_ddl_create_function_runtime.cpp
  sblr_ddl_alter_function_runtime.cpp
  sblr_ddl_drop_function_runtime.cpp
  sblr_ddl_create_package_runtime.cpp
  sblr_ddl_create_temporary_table_runtime.cpp
  sblr_ddl_drop_temporary_table_runtime.cpp
  sblr_ddl_rename_object_vector_runtime.cpp
  sblr_ddl_create_or_replace_srs_runtime.cpp
  sblr_ddl_drop_srs_runtime.cpp
  sblr_ddl_create_rewrite_rule_runtime.cpp
  sblr_ddl_alter_rewrite_rule_runtime.cpp
  sblr_ddl_drop_rewrite_rule_runtime.cpp
  sblr_ddl_validate_constraint_runtime.cpp
  sblr_security_create_privilege_template_runtime.cpp
  sblr_security_alter_privilege_template_runtime.cpp
  sblr_security_drop_privilege_template_runtime.cpp
  sblr_database_create_template_clone_runtime.cpp
  sblr_ddl_create_aggregate_runtime.cpp
  sblr_ddl_alter_aggregate_runtime.cpp
  sblr_ddl_drop_aggregate_runtime.cpp
  sblr_ddl_purge_system_history_runtime.cpp
  sblr_ddl_set_index_optimizer_eligibility_runtime.cpp
  sblr_ddl_set_table_type_enforcement_runtime.cpp
  sblr_database_serialize_logical_snapshot_runtime.cpp
  sblr_database_serialize_logical_snapshot_runtime.cpp
  sblr_ddl_drop_trigger_runtime.cpp
  sblr_return_result_set_runtime.cpp
  sblr_sequence_currval_runtime.cpp
    sblr_read_range_runtime.cpp
  sblr_parameter_negotiation.cpp
)
target_compile_features(sb_sblr_parameter_wire_codec PUBLIC cxx_std_20)
target_include_directories(sb_sblr_parameter_wire_codec PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(sb_sblr_parameter_wire_codec PUBLIC sb_core_hash)

# Pure canonical SBOP producer/decoder shared with parser producers.  It owns
# wire validation only and carries no admission, catalog, MGA, or executor
# authority.
add_library(sb_sblr_operation_wire_codec STATIC
  sblr_engine_envelope.cpp
  sblr_literal_runtime.cpp
  sblr_opcode_registry.cpp
  sblr_opcode_stream.cpp
)
target_compile_features(sb_sblr_operation_wire_codec PUBLIC cxx_std_20)
target_include_directories(sb_sblr_operation_wire_codec PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/engine/internal_api
)
target_link_libraries(sb_sblr_operation_wire_codec PUBLIC
  sb_sblr_parameter_wire_codec sb_core_hash)

add_library(sb_engine_sblr
  aggregates/sblr_aggregate_family_landing_zone.cpp
  canonical_relational_expression.cpp
  canonical_query_execute.cpp
  dispatch/sblr_family_dispatch_landing_zone.cpp
  operators/sblr_operator_family_landing_zone.cpp
  special_forms/sblr_special_form_family_landing_zone.cpp
  # QOW-CMAKE-WIN-003-V1
  # QOW-CMAKE-WIN-013-V1
  # QOW-CMAKE-IAS-004-V1
  # QOW-CMAKE-WIN-006-V1
  # QOW-CMAKE-WIN-001-V1
  # QOW-CMAKE-WIN-002-V1
  # The index/sentinel evaluator below is compatibility-only. Canonical frame
  # construction enters through the linked sb_engine_executor target, and the
  # legacy evaluator is a compiled default-refusal only.
  sblr_aggregate_window_runtime.cpp
  sblr_api_trace.cpp
  sblr_assignment_runtime.cpp
  sblr_block_runtime.cpp
  sblr_context_variables.cpp
  sblr_domain_runtime.cpp
  sblr_engine_envelope.cpp
  sblr_event_notification.cpp
  sblr_execution_metrics.cpp
  ipar_sblr_support_services.cpp
  sblr_dispatch.cpp
  sblr_hot_path_execution.cpp
  sblr_llvm_seam.cpp
  sblr_literal_runtime.cpp
  sblr_local_metrics_read.cpp
  sblr_local_backup_archive.cpp
  sblr_management_envelope.cpp
  sblr_native_specialization.cpp
  sblr_operator_runtime.cpp
  sblr_opcode_registry.cpp
  sblr_opcode_stream.cpp
  sblr_parser_memory_handoff.cpp
  sblr_parser_authority_guards.cpp
  sblr_parser_projection.cpp
  sblr_prepared_template.cpp
  sblr_procedural_block_runtime.cpp
  sblr_refusal.cpp
  sblr_resource_governance.cpp
  sblr_routine_runtime.cpp
  sblr_runtime_logging.cpp
  sblr_special_forms.cpp
  sblr_test_vectors.cpp
  sblr_to_sbsql.cpp
  sblr_transaction_hooks.cpp
  window/sblr_window_family_landing_zone.cpp
)

target_link_libraries(sb_engine_sblr PUBLIC sb_sblr_parameter_wire_codec)

if(NOT TARGET sb_engine_functions)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../functions ${CMAKE_CURRENT_BINARY_DIR}/engine_functions)
endif()

if(NOT TARGET sb_engine_executor)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../executor ${CMAKE_CURRENT_BINARY_DIR}/engine_executor)
endif()

if(NOT TARGET sb_engine_native_compile)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../native_compile ${CMAKE_CURRENT_BINARY_DIR}/engine_native_compile)
endif()

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

target_include_directories(sb_engine_sblr PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/core/datatypes
)

target_compile_features(sb_engine_sblr PUBLIC cxx_std_20)

target_link_libraries(sb_engine_sblr PUBLIC sb_engine_internal_api sb_engine_executor sb_engine_native_compile sb_engine_optimizer_contract_only sb_core_datatypes)
if(TARGET sb_engine_functions)
  target_link_libraries(sb_engine_sblr PRIVATE sb_engine_functions)
endif()
if(TARGET sb_cluster_provider)
  target_link_libraries(sb_engine_sblr PRIVATE sb_cluster_provider)
endif()

if(BUILD_TESTING)
  add_executable(sbsql_sblr_alignment_ia11_local_backup_archive ${CMAKE_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia11_local_backup_archive_test.cpp)
  target_compile_features(sbsql_sblr_alignment_ia11_local_backup_archive PRIVATE cxx_std_20)
  target_include_directories(sbsql_sblr_alignment_ia11_local_backup_archive PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia11_local_backup_archive PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia11_local_backup_archive COMMAND sbsql_sblr_alignment_ia11_local_backup_archive)
  set_tests_properties(sbsql_sblr_alignment_ia11_local_backup_archive PROPERTIES LABELS "IA-11;backup-archive;CSC-TEST-003342;CSC-TEST-003343;CSC-TEST-003344;CSC-TEST-003345;protocol_binary" TIMEOUT 30)
  add_executable(sbsql_sblr_alignment_ia10c_event_notification
    ${CMAKE_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia10c_event_notification_test.cpp)
  target_compile_features(sbsql_sblr_alignment_ia10c_event_notification PRIVATE cxx_std_20)
  target_include_directories(sbsql_sblr_alignment_ia10c_event_notification PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia10c_event_notification PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia10c_event_notification COMMAND sbsql_sblr_alignment_ia10c_event_notification)
  set_tests_properties(sbsql_sblr_alignment_ia10c_event_notification PROPERTIES LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-10;IA-10C;event-notification;CSC-TEST-003338;CSC-TEST-003339;CSC-TEST-003340;CSC-TEST-003341;protocol_binary;contract" TIMEOUT 30)

  add_executable(sbsql_sblr_alignment_ia10b_local_metrics_read
    ${CMAKE_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia10b_local_metrics_read_test.cpp
  )
  target_compile_features(sbsql_sblr_alignment_ia10b_local_metrics_read PRIVATE cxx_std_20)
  target_include_directories(sbsql_sblr_alignment_ia10b_local_metrics_read
    PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia10b_local_metrics_read PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia10b_local_metrics_read
    COMMAND sbsql_sblr_alignment_ia10b_local_metrics_read)
  set_tests_properties(sbsql_sblr_alignment_ia10b_local_metrics_read PROPERTIES
    LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-10;IA-10B;SBLR_READ_METRICS;CSC-TEST-003334;CSC-TEST-003335;CSC-TEST-003336;CSC-TEST-003337;protocol_binary;contract"
    TIMEOUT 30)

  add_executable(sbsql_sblr_alignment_ia10a_management_envelope
    ${CMAKE_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia10a_management_envelope_test.cpp
  )
  target_compile_features(sbsql_sblr_alignment_ia10a_management_envelope PRIVATE cxx_std_20)
  target_include_directories(sbsql_sblr_alignment_ia10a_management_envelope
    PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(sbsql_sblr_alignment_ia10a_management_envelope PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia10a_management_envelope
    COMMAND sbsql_sblr_alignment_ia10a_management_envelope)
  set_tests_properties(sbsql_sblr_alignment_ia10a_management_envelope PROPERTIES
    LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-10;IA-10A;SBLR_MGMT_OPERATION;SBLR_MGMT_PAYLOAD;SBLR_MGMT_RESULT;SBLR_MGMT_PROGRESS;SBLR_MGMT_DIAGNOSTIC;SBLR_MGMT_METRIC_SNAPSHOT_REF;CSC-TEST-003328;CSC-TEST-003329;CSC-TEST-003330;CSC-TEST-003331;CSC-TEST-003332;CSC-TEST-003333;protocol_binary;contract"
    TIMEOUT 30)

  add_executable(sbsql_sblr_alignment_ia01_opcode_contract
    ${CMAKE_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_opcode_contract_test.cpp
  )
  target_compile_features(sbsql_sblr_alignment_ia01_opcode_contract PRIVATE cxx_std_20)
  target_link_libraries(sbsql_sblr_alignment_ia01_opcode_contract PRIVATE sb_engine_sblr)
  add_test(NAME sbsql_sblr_alignment_ia01_opcode_contract
    COMMAND sbsql_sblr_alignment_ia01_opcode_contract)
  set_tests_properties(sbsql_sblr_alignment_ia01_opcode_contract PROPERTIES
    LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-01;SBLR_PACKAGE_BEGIN_END;contract")
endif()

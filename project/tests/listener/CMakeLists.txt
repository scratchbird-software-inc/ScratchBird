# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(sb_listener_control_plane_probe control_plane_probe.cpp)
target_link_libraries(sb_listener_control_plane_probe PRIVATE sbl_listener_runtime)
add_test(NAME sb_listener_control_plane_probe COMMAND sb_listener_control_plane_probe)

add_executable(sb_listener_no_spin_gate no_spin_gate.cpp)
add_test(NAME sb_listener_no_spin_gate COMMAND sb_listener_no_spin_gate ${PROJECT_SOURCE_DIR}/src/listener)

add_executable(sb_listener_diagnostic_registry_gate diagnostic_registry_gate.cpp)
target_compile_features(sb_listener_diagnostic_registry_gate PRIVATE cxx_std_23)
add_test(NAME sb_listener_diagnostic_registry_gate
  COMMAND sb_listener_diagnostic_registry_gate ${PROJECT_SOURCE_DIR})

add_executable(sb_listener_native_worker_storage_policy_gate native_worker_storage_policy_gate.cpp)
target_compile_features(sb_listener_native_worker_storage_policy_gate PRIVATE cxx_std_23)
add_test(NAME sb_listener_native_worker_storage_policy_gate
  COMMAND sb_listener_native_worker_storage_policy_gate
          ${PROJECT_SOURCE_DIR}/src/parsers/native/v3/worker/main.cpp
          ${PROJECT_SOURCE_DIR}/src/parsers/native/v3/worker/CMakeLists.txt)

find_program(SB_POLICY_PYTHON3 python3 REQUIRED)
add_test(NAME sb_listener_mga_transaction_authority_gate
  COMMAND ${SB_POLICY_PYTHON3}
          ${CMAKE_CURRENT_SOURCE_DIR}/mga_transaction_authority_gate.py
          ${PROJECT_SOURCE_DIR})

add_test(NAME sb_benchmark_runtime_storage_policy_gate
  COMMAND ${SB_POLICY_PYTHON3}
          ${CMAKE_CURRENT_SOURCE_DIR}/benchmark_runtime_storage_policy_gate.py
          ${PROJECT_SOURCE_DIR}/..)

add_test(NAME legacy_execution_plan10_scratchbird_benchmark_compat_gate
  COMMAND ${SB_POLICY_PYTHON3}
          ${CMAKE_CURRENT_SOURCE_DIR}/legacy_execution_plan10_scratchbird_benchmark_compat_gate.py
          ${PROJECT_SOURCE_DIR}/..)
set_tests_properties(legacy_execution_plan10_scratchbird_benchmark_compat_gate PROPERTIES
  LABELS "benchmark;performance;legacy_execution_plan10;scratchbird_dialect"
)

add_executable(sb_listener_protocol_profile_defaults_probe protocol_profile_defaults_probe.cpp)
target_compile_features(sb_listener_protocol_profile_defaults_probe PRIVATE cxx_std_23)
target_link_libraries(sb_listener_protocol_profile_defaults_probe PRIVATE sbl_listener_runtime)
add_test(NAME sb_listener_protocol_profile_defaults_probe
  COMMAND sb_listener_protocol_profile_defaults_probe)
set_tests_properties(sb_listener_protocol_profile_defaults_probe PROPERTIES
  LABELS "firebird_runtime_absence_gate;firebird_parser_worker;listener"
)

if(NOT WIN32)
add_executable(sb_listener_runtime_handoff_smoke runtime_handoff_smoke.cpp)
target_compile_features(sb_listener_runtime_handoff_smoke PRIVATE cxx_std_23)
add_test(
  NAME sb_listener_runtime_handoff_smoke
  COMMAND sb_listener_runtime_handoff_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)

add_executable(sb_listener_management_commands_smoke management_commands_smoke.cpp)
target_compile_features(sb_listener_management_commands_smoke PRIVATE cxx_std_23)
target_link_libraries(sb_listener_management_commands_smoke PRIVATE sbl_listener_runtime)
add_test(
  NAME sb_listener_management_commands_smoke
  COMMAND sb_listener_management_commands_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)

if(TARGET sbl_manager_node_runtime)
  add_executable(sb_listener_manager_active_status_smoke manager_active_status_smoke.cpp)
  target_compile_features(sb_listener_manager_active_status_smoke PRIVATE cxx_std_23)
  target_link_libraries(sb_listener_manager_active_status_smoke PRIVATE sbl_manager_node_runtime)
  add_test(
    NAME sb_listener_manager_active_status_smoke
    COMMAND sb_listener_manager_active_status_smoke
            $<TARGET_FILE:sb_listener>
            $<TARGET_FILE:sb_parser_dummy>
  )

  add_executable(sb_listener_manager_drain_active_smoke manager_drain_active_smoke.cpp)
  target_compile_features(sb_listener_manager_drain_active_smoke PRIVATE cxx_std_23)
  target_link_libraries(sb_listener_manager_drain_active_smoke PRIVATE sbl_manager_node_runtime)
  add_test(
    NAME sb_listener_manager_drain_active_smoke
    COMMAND sb_listener_manager_drain_active_smoke
            $<TARGET_FILE:sb_listener>
            $<TARGET_FILE:sb_parser_dummy>
  )
endif()

add_executable(sb_listener_management_stop_smoke management_stop_smoke.cpp)
target_compile_features(sb_listener_management_stop_smoke PRIVATE cxx_std_23)
target_link_libraries(sb_listener_management_stop_smoke PRIVATE sbl_listener_runtime)
add_test(
  NAME sb_listener_management_stop_smoke
  COMMAND sb_listener_management_stop_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)
add_test(
  NAME sb_listener_management_stop_force_smoke
  COMMAND sb_listener_management_stop_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
          "STOP FORCE"
)

add_executable(sb_listener_dbbt_lpreface_smoke dbbt_lpreface_smoke.cpp)
target_compile_features(sb_listener_dbbt_lpreface_smoke PRIVATE cxx_std_23)
target_link_libraries(sb_listener_dbbt_lpreface_smoke PRIVATE sbl_listener_runtime)
add_dependencies(sb_listener_dbbt_lpreface_smoke sb_listener sb_parser_dummy)
add_test(
  NAME sb_listener_dbbt_lpreface_smoke
  COMMAND sb_listener_dbbt_lpreface_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)

add_executable(sb_listener_management_malformed_frame_smoke management_malformed_frame_smoke.cpp)
target_compile_features(sb_listener_management_malformed_frame_smoke PRIVATE cxx_std_23)
target_link_libraries(sb_listener_management_malformed_frame_smoke PRIVATE sbl_listener_runtime)
add_test(
  NAME sb_listener_management_malformed_frame_smoke
  COMMAND sb_listener_management_malformed_frame_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)

add_executable(sb_listener_management_socket_removed_smoke management_socket_removed_smoke.cpp)
target_compile_features(sb_listener_management_socket_removed_smoke PRIVATE cxx_std_23)
add_test(
  NAME sb_listener_management_socket_removed_smoke
  COMMAND sb_listener_management_socket_removed_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)

add_executable(sb_listener_management_socket_permission_smoke management_socket_permission_smoke.cpp)
target_compile_features(sb_listener_management_socket_permission_smoke PRIVATE cxx_std_23)
add_test(
  NAME sb_listener_management_socket_permission_smoke
  COMMAND sb_listener_management_socket_permission_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)

add_executable(sb_listener_owner_token_collision_smoke owner_token_collision_smoke.cpp)
target_compile_features(sb_listener_owner_token_collision_smoke PRIVATE cxx_std_23)
add_test(
  NAME sb_listener_owner_token_collision_smoke
  COMMAND sb_listener_owner_token_collision_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)

add_executable(sb_listener_owner_token_stale_corrupt_smoke owner_token_stale_corrupt_smoke.cpp)
target_compile_features(sb_listener_owner_token_stale_corrupt_smoke PRIVATE cxx_std_23)
target_link_libraries(sb_listener_owner_token_stale_corrupt_smoke PRIVATE sbl_listener_runtime)
add_test(
  NAME sb_listener_owner_token_stale_corrupt_smoke
  COMMAND sb_listener_owner_token_stale_corrupt_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)

add_executable(sb_listener_drain_admission_smoke drain_admission_smoke.cpp)
target_compile_features(sb_listener_drain_admission_smoke PRIVATE cxx_std_23)
target_link_libraries(sb_listener_drain_admission_smoke PRIVATE sbl_listener_runtime)
add_test(
  NAME sb_listener_drain_admission_smoke
  COMMAND sb_listener_drain_admission_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)

add_executable(sb_listener_tls_required_fail_closed_smoke tls_required_fail_closed_smoke.cpp)
target_compile_features(sb_listener_tls_required_fail_closed_smoke PRIVATE cxx_std_23)
add_test(
  NAME sb_listener_tls_required_fail_closed_smoke
  COMMAND sb_listener_tls_required_fail_closed_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)
set_tests_properties(sb_listener_tls_required_fail_closed_smoke PROPERTIES
  LABELS "public_release_foundation;sbwp_tls_negative_policy_gate;tls_regression_gate;sbwp_wire_gate"
)

find_program(SB_OPENSSL_EXECUTABLE openssl REQUIRED)
add_executable(sb_listener_ipar_tls_non_tls_policy_route_smoke ipar_tls_non_tls_policy_route_smoke.cpp)
target_compile_features(sb_listener_ipar_tls_non_tls_policy_route_smoke PRIVATE cxx_std_23)
add_dependencies(sb_listener_ipar_tls_non_tls_policy_route_smoke sb_listener sb_parser_dummy sbp_sbsql)
add_test(
  NAME sb_listener_ipar_tls_non_tls_policy_route_smoke
  COMMAND sb_listener_ipar_tls_non_tls_policy_route_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
          $<TARGET_FILE:sbp_sbsql>
          ${SB_OPENSSL_EXECUTABLE}
          ${CMAKE_CURRENT_BINARY_DIR}/ipar-tls-nontls-fixtures
)
set_tests_properties(sb_listener_ipar_tls_non_tls_policy_route_smoke PROPERTIES
  LABELS "IPAR-P7-03;IPAR-G013;sbwp_tls_negative_policy_gate;explicit_test_nontls_route;tls_regression_gate;sbwp_wire_gate"
)

if(TARGET sbp_native)
  find_package(OpenSSL REQUIRED)
  add_executable(sb_listener_native_sbwp_tls_smoke native_sbwp_tls_smoke.cpp)
  target_compile_features(sb_listener_native_sbwp_tls_smoke PRIVATE cxx_std_23)
  target_link_libraries(sb_listener_native_sbwp_tls_smoke PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  add_dependencies(sb_listener_native_sbwp_tls_smoke sb_listener sbp_native)
  add_test(
    NAME sb_listener_native_sbwp_tls_smoke
    COMMAND sb_listener_native_sbwp_tls_smoke
            $<TARGET_FILE:sb_listener>
            $<TARGET_FILE:sbp_native>
            ${SB_OPENSSL_EXECUTABLE}
            ${CMAKE_CURRENT_BINARY_DIR}/tls-fixtures
  )
  set_tests_properties(sb_listener_native_sbwp_tls_smoke PROPERTIES
    LABELS "public_release_foundation;sbwp_tls_server_listener_gate;sbwp_tls_negative_policy_gate;tls_fixture_policy_gate;tls_regression_gate;listener_sbct_pool_gate;sbwp_wire_gate"
  )
endif()

add_executable(sb_listener_rate_limit_smoke rate_limit_smoke.cpp)
target_compile_features(sb_listener_rate_limit_smoke PRIVATE cxx_std_23)
add_test(
  NAME sb_listener_rate_limit_smoke
  COMMAND sb_listener_rate_limit_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)

add_executable(sb_listener_per_client_cap_smoke per_client_cap_smoke.cpp)
target_compile_features(sb_listener_per_client_cap_smoke PRIVATE cxx_std_23)
add_test(
  NAME sb_listener_per_client_cap_smoke
  COMMAND sb_listener_per_client_cap_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)

add_executable(sb_listener_slow_client_nonblocking_smoke slow_client_nonblocking_smoke.cpp)
target_compile_features(sb_listener_slow_client_nonblocking_smoke PRIVATE cxx_std_23)
add_test(
  NAME sb_listener_slow_client_nonblocking_smoke
  COMMAND sb_listener_slow_client_nonblocking_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)

add_executable(sb_listener_parser_pool_exhaustion_smoke parser_pool_exhaustion_smoke.cpp)
target_compile_features(sb_listener_parser_pool_exhaustion_smoke PRIVATE cxx_std_23)
add_test(
  NAME sb_listener_parser_pool_exhaustion_smoke
  COMMAND sb_listener_parser_pool_exhaustion_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)

add_executable(sb_listener_dummy_fault_modes_smoke dummy_fault_modes_smoke.cpp)
target_compile_features(sb_listener_dummy_fault_modes_smoke PRIVATE cxx_std_23)
add_test(
  NAME sb_listener_dummy_fault_modes_smoke
  COMMAND sb_listener_dummy_fault_modes_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)

add_executable(sb_listener_parser_crash_recovery_smoke parser_crash_recovery_smoke.cpp)
target_compile_features(sb_listener_parser_crash_recovery_smoke PRIVATE cxx_std_23)
add_test(
  NAME sb_listener_parser_crash_recovery_smoke
  COMMAND sb_listener_parser_crash_recovery_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)

add_executable(sb_listener_kill_after_handoff_smoke listener_kill_after_handoff_smoke.cpp)
target_compile_features(sb_listener_kill_after_handoff_smoke PRIVATE cxx_std_23)
add_test(
  NAME sb_listener_kill_after_handoff_smoke
  COMMAND sb_listener_kill_after_handoff_smoke
          $<TARGET_FILE:sb_listener>
          $<TARGET_FILE:sb_parser_dummy>
)

if(TARGET sb_server)
  add_executable(sb_server_managed_listener_smoke server_managed_listener_smoke.cpp)
  target_compile_features(sb_server_managed_listener_smoke PRIVATE cxx_std_23)
  add_dependencies(sb_server_managed_listener_smoke sb_server sb_listener sb_parser_dummy)
  add_test(
    NAME sb_server_managed_listener_smoke
    COMMAND sb_server_managed_listener_smoke
            $<TARGET_FILE:sb_server>
            $<TARGET_FILE:sb_listener>
            $<TARGET_FILE:sb_parser_dummy>
  )
  set_property(TEST sb_server_managed_listener_smoke APPEND PROPERTY LABELS
    database_lifecycle_server_daemon server_lifecycle_gate listener_sbct_pool_gate
  )

  add_executable(sb_server_shutdown_stops_listener_smoke server_shutdown_stops_listener_smoke.cpp)
  target_compile_features(sb_server_shutdown_stops_listener_smoke PRIVATE cxx_std_23)
  add_dependencies(sb_server_shutdown_stops_listener_smoke sb_server sb_listener sb_parser_dummy)
  add_test(
    NAME sb_server_shutdown_stops_listener_smoke
    COMMAND sb_server_shutdown_stops_listener_smoke
            $<TARGET_FILE:sb_server>
            $<TARGET_FILE:sb_listener>
            $<TARGET_FILE:sb_parser_dummy>
  )
  set_property(TEST sb_server_shutdown_stops_listener_smoke APPEND PROPERTY LABELS
    database_lifecycle_server_daemon server_lifecycle_gate listener_sbct_pool_gate
  )

  if(TARGET sb_ipc_tester AND TARGET sb_storage_database AND TARGET sb_engine_internal_api AND TARGET sb_core_uuid)
    add_executable(sb_server_restart_killed_listener_smoke server_restart_killed_listener_smoke.cpp)
    target_compile_features(sb_server_restart_killed_listener_smoke PRIVATE cxx_std_23)
    target_link_libraries(sb_server_restart_killed_listener_smoke PRIVATE
      sb_storage_database
      sb_engine_internal_api
      sb_core_uuid
      sb_core_memory
    )
    add_dependencies(sb_server_restart_killed_listener_smoke sb_server sb_listener sb_parser_dummy sb_ipc_tester)
    add_test(
      NAME sb_server_restart_killed_listener_smoke
      COMMAND sb_server_restart_killed_listener_smoke
              $<TARGET_FILE:sb_server>
              $<TARGET_FILE:sb_listener>
              $<TARGET_FILE:sb_parser_dummy>
              $<TARGET_FILE:sb_ipc_tester>
    )
    set_property(TEST sb_server_restart_killed_listener_smoke APPEND PROPERTY LABELS
      database_lifecycle_server_daemon server_lifecycle_gate listener_sbct_pool_gate
    )
  endif()
endif()
endif()

add_test(NAME database_lifecycle_listener_conformance
  COMMAND ${SB_POLICY_PYTHON3}
          ${CMAKE_CURRENT_SOURCE_DIR}/database_lifecycle_listener_conformance.py
          ${PROJECT_SOURCE_DIR})
set_property(TEST database_lifecycle_listener_conformance APPEND PROPERTY LABELS
  database_lifecycle_listener database_lifecycle listener_lifecycle listener_sbct_pool_gate
)

set(SB_LISTENER_LIFECYCLE_TESTS
  sb_listener_control_plane_probe
  sb_listener_no_spin_gate
  sb_listener_diagnostic_registry_gate
  sb_listener_native_worker_storage_policy_gate
  sb_listener_mga_transaction_authority_gate
  sb_listener_protocol_profile_defaults_probe
  sb_listener_runtime_handoff_smoke
  sb_listener_management_commands_smoke
  sb_listener_manager_active_status_smoke
  sb_listener_manager_drain_active_smoke
  sb_listener_management_stop_smoke
  sb_listener_management_stop_force_smoke
  sb_listener_dbbt_lpreface_smoke
  sb_listener_management_malformed_frame_smoke
  sb_listener_management_socket_removed_smoke
  sb_listener_management_socket_permission_smoke
  sb_listener_owner_token_collision_smoke
  sb_listener_owner_token_stale_corrupt_smoke
  sb_listener_drain_admission_smoke
  sb_listener_tls_required_fail_closed_smoke
  sb_listener_ipar_tls_non_tls_policy_route_smoke
  sb_listener_native_sbwp_tls_smoke
  sb_listener_rate_limit_smoke
  sb_listener_per_client_cap_smoke
  sb_listener_slow_client_nonblocking_smoke
  sb_listener_parser_pool_exhaustion_smoke
  sb_listener_dummy_fault_modes_smoke
  sb_listener_parser_crash_recovery_smoke
  sb_listener_kill_after_handoff_smoke
  sb_server_managed_listener_smoke
  sb_server_shutdown_stops_listener_smoke
  sb_server_restart_killed_listener_smoke
)

foreach(test_name IN LISTS SB_LISTENER_LIFECYCLE_TESTS)
  if(TEST ${test_name})
    set_property(TEST ${test_name} APPEND PROPERTY LABELS
      database_lifecycle_listener database_lifecycle listener_lifecycle listener_sbct_pool_gate
    )
  endif()
endforeach()

set(SB_LISTENER_IPC_LIFECYCLE_TESTS
  sb_listener_management_commands_smoke
  sb_listener_manager_active_status_smoke
  sb_listener_manager_drain_active_smoke
  sb_listener_management_stop_smoke
  sb_listener_management_stop_force_smoke
  sb_listener_management_malformed_frame_smoke
  sb_listener_management_socket_removed_smoke
  sb_listener_management_socket_permission_smoke
  sb_server_managed_listener_smoke
  sb_server_shutdown_stops_listener_smoke
  sb_server_restart_killed_listener_smoke
)

foreach(test_name IN LISTS SB_LISTENER_IPC_LIFECYCLE_TESTS)
  if(TEST ${test_name})
    set_property(TEST ${test_name} APPEND PROPERTY LABELS
      database_lifecycle_ipc local_ipc_gate
    )
  endif()
endforeach()

# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(sbmn_manager_no_spin_gate
  no_spin_gate.cpp
)

target_compile_features(sbmn_manager_no_spin_gate PRIVATE cxx_std_23)

add_test(NAME sbmn_manager_no_spin_gate
  COMMAND sbmn_manager_no_spin_gate "${PROJECT_SOURCE_DIR}/src/manager"
)
set_tests_properties(sbmn_manager_no_spin_gate PROPERTIES
  LABELS "database_lifecycle_manager;database_lifecycle;manager_no_spin;sbmn_manager"
)

add_executable(sbmn_manager_protocol_unit_tests
  protocol_unit_tests.cpp
)

target_compile_features(sbmn_manager_protocol_unit_tests PRIVATE cxx_std_23)

target_include_directories(sbmn_manager_protocol_unit_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src/manager/node
)

target_link_libraries(sbmn_manager_protocol_unit_tests PRIVATE
  sbl_manager_node_runtime
)

add_test(NAME sbmn_manager_protocol_unit_tests COMMAND sbmn_manager_protocol_unit_tests)
set_tests_properties(sbmn_manager_protocol_unit_tests PROPERTIES
  LABELS "ELER-106;ELER-GATE-106;database_lifecycle_manager;database_lifecycle;database_lifecycle_ipc;manager_protocol;listener_control_protocol_version;management_protocol_compatibility;sbmn_manager"
)

add_executable(sbmn_manager_protocol_fuzz_gate
  protocol_fuzz_gate.cpp
)

target_compile_features(sbmn_manager_protocol_fuzz_gate PRIVATE cxx_std_23)

target_link_libraries(sbmn_manager_protocol_fuzz_gate PRIVATE
  sbl_manager_protocol
)

add_test(NAME sbmn_manager_protocol_fuzz_gate COMMAND sbmn_manager_protocol_fuzz_gate)
set_tests_properties(sbmn_manager_protocol_fuzz_gate PROPERTIES
  LABELS "ELER-104;ELER-GATE-104;database_lifecycle_manager;database_lifecycle;database_lifecycle_ipc;manager_protocol;manager_fuzz;sbmn_manager;listener_control_plane_frame_fuzz;handoff_binding_property"
)

add_dependencies(sbmn_manager_protocol_unit_tests sbmn_manager_protocol_fuzz_gate)

add_executable(sbmn_manager_runtime_integration_tests
  runtime_integration_tests.cpp
)

target_compile_features(sbmn_manager_runtime_integration_tests PRIVATE cxx_std_23)

target_include_directories(sbmn_manager_runtime_integration_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src/manager/node
)

target_link_libraries(sbmn_manager_runtime_integration_tests PRIVATE
  sbl_manager_node_runtime
)

add_test(NAME sbmn_manager_runtime_integration_tests COMMAND sbmn_manager_runtime_integration_tests)
set_tests_properties(sbmn_manager_runtime_integration_tests PROPERTIES
  LABELS "ELER-106;ELER-GATE-106;database_lifecycle_manager;database_lifecycle;database_lifecycle_ipc;manager_runtime;management_protocol_compatibility;sbmn_manager"
)

add_dependencies(sbmn_manager_protocol_unit_tests sbmn_manager_runtime_integration_tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
  NAME database_lifecycle_manager_conformance
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/database_lifecycle_manager_conformance.py"
          "--repo-root" "${PROJECT_SOURCE_DIR}"
          "--build-root" "${PROJECT_BINARY_DIR}"
          "--manager-exe" "$<TARGET_FILE:sbmn_manager>"
          "--protocol-unit" "$<TARGET_FILE:sbmn_manager_protocol_unit_tests>"
          "--runtime-integration" "$<TARGET_FILE:sbmn_manager_runtime_integration_tests>"
          "--fuzz-gate" "$<TARGET_FILE:sbmn_manager_protocol_fuzz_gate>"
          "--no-spin-gate" "$<TARGET_FILE:sbmn_manager_no_spin_gate>"
          "--no-spin-source" "${PROJECT_SOURCE_DIR}/src/manager"
)
set_tests_properties(database_lifecycle_manager_conformance PROPERTIES
  LABELS "database_lifecycle_manager;database_lifecycle;database_lifecycle_ipc;manager_runtime;manager_protocol;sbmn_manager;sbmc_manager_private_boundary"
)

# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(Threads REQUIRED)

add_library(sbl_manager_node_runtime STATIC
  manager_lifecycle.cpp
  manager_listener_control.cpp
  manager_mcp_payload.cpp
  manager_restart_policy.cpp
  manager_runtime_snapshot.cpp
  manager_runtime.cpp
  manager_support_bundle.cpp
)

target_include_directories(sbl_manager_node_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(sbl_manager_node_runtime PUBLIC cxx_std_23)

target_link_libraries(sbl_manager_node_runtime PUBLIC
  sbl_manager_protocol
  sbl_listener_control_plane
  Threads::Threads
)
if(WIN32)
  target_link_libraries(sbl_manager_node_runtime PUBLIC ws2_32)
endif()

add_executable(sbmn_manager
  main.cpp
)

target_compile_features(sbmn_manager PRIVATE cxx_std_23)

target_link_libraries(sbmn_manager PRIVATE
  sbl_manager_node_runtime
)

add_executable(sbmn_manager_ipc_tester
  ${PROJECT_SOURCE_DIR}/tests/manager/ipc_tester.cpp
)

target_compile_features(sbmn_manager_ipc_tester PRIVATE cxx_std_23)

target_link_libraries(sbmn_manager_ipc_tester PRIVATE
  sbl_manager_protocol
)

if(NOT TARGET sbmn_manager_no_spin_gate)
  add_subdirectory(${CMAKE_SOURCE_DIR}/tests/manager ${CMAKE_BINARY_DIR}/tests_manager)
endif()

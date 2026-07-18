# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET sbl_listener_control_plane)
  if(NOT TARGET sbl_manager_protocol)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/manager/protocol ${CMAKE_CURRENT_BINARY_DIR}/manager_protocol_for_firebird)
  endif()
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/listener ${CMAKE_CURRENT_BINARY_DIR}/listener_control_for_firebird)
endif()

if(NOT TARGET sbl_parser_server_ipc_client)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/wire/parser_server_ipc ${CMAKE_CURRENT_BINARY_DIR}/parser_server_ipc_for_firebird)
endif()

add_library(sbl_firebird_transaction_policy STATIC
  firebird_transaction_policy.cpp
)
target_compile_features(sbl_firebird_transaction_policy PUBLIC cxx_std_23)
target_include_directories(sbl_firebird_transaction_policy PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

add_library(sbl_firebird_parser_pipeline STATIC
  firebird_catalog_projection.cpp
  firebird_derived_table_diagnostics.cpp
  firebird_dialect.cpp
  firebird_execution_session.cpp
  firebird_global_aggregate_projection.cpp
  firebird_package_policy.cpp
  firebird_procedural_block.cpp
  firebird_relation_projection_view.cpp
  firebird_scalar_projection.cpp
  firebird_semantic_evidence.cpp
  firebird_wire_descriptor.cpp
  firebird_worker_session.cpp
)

target_compile_features(sbl_firebird_parser_pipeline PUBLIC cxx_std_23)
target_include_directories(sbl_firebird_parser_pipeline PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(sbl_firebird_parser_pipeline PUBLIC
  sbl_firebird_transaction_policy
  sbl_parser_server_ipc_client
)

if(UNIX AND NOT APPLE)
  find_library(SB_FIREBIRD_CRYPT_LIBRARY crypt)
  if(SB_FIREBIRD_CRYPT_LIBRARY)
    target_compile_definitions(sbl_firebird_parser_pipeline PRIVATE
      SB_FIREBIRD_HAS_SYSTEM_CRYPT=1
    )
    target_link_libraries(sbl_firebird_parser_pipeline PUBLIC
      ${SB_FIREBIRD_CRYPT_LIBRARY}
    )
  endif()
endif()

add_executable(sbp_firebird main.cpp)
target_compile_features(sbp_firebird PRIVATE cxx_std_23)
target_link_libraries(sbp_firebird PRIVATE
  sbl_firebird_parser_pipeline
  sbl_listener_control_plane
)

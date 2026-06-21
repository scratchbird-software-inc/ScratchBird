# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

if(NOT TARGET sbl_parser_server_ipc_schema)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/wire/parser_server_ipc ${CMAKE_CURRENT_BINARY_DIR}/parser_server_ipc_schema)
endif()

add_library(sbl_sbsql_parser_pipeline STATIC
  common/common.cpp
  registry/generated/sbsql_generated_registry.cpp
  registry/sbsql_surface_registry.cpp
  auth/auth_relay.cpp
  metrics/parser_metrics.cpp
  cache/sblr_template_cache.cpp
  meta/meta_command_surface.cpp
  lexer/lexer.cpp
  cst/cst.cpp
  ast/ast.cpp
  expression/expression_catalog.cpp
  expression/reference_variable_compatibility.cpp
  resources/language_resource_contract.cpp
  statement/statement_catalog.cpp
  binder/binder.cpp
  lowering/lowering.cpp
  rendering/rendering.cpp
)

target_compile_features(sbl_sbsql_parser_pipeline PUBLIC cxx_std_23)
target_include_directories(sbl_sbsql_parser_pipeline PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(sbl_sbsql_parser_pipeline PUBLIC sbl_parser_server_ipc_schema)

add_library(sbl_sbsql_parser_worker_core STATIC
  embedded/embedded_engine_client.cpp
  ipc/sbps_client.cpp
  lifecycle/agent_parser_interface_bridge.cpp
  lifecycle/parser_lifecycle.cpp
  runtime/parser_runtime.cpp
  wire/sbsql_sbwp_wire.cpp
  wire/sbsql_test_wire.cpp
)

if(NOT TARGET sb_core_datatypes)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/core/datatypes
                   ${CMAKE_BINARY_DIR}/src/core/datatypes)
endif()

target_compile_features(sbl_sbsql_parser_worker_core PUBLIC cxx_std_23)
target_include_directories(sbl_sbsql_parser_worker_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/src/listener
)
target_link_libraries(sbl_sbsql_parser_worker_core PUBLIC
  sbl_sbsql_parser_pipeline
  sbl_listener_control_plane
  sb_core_agents
  sb_core_datatypes
  sb_core_uuid
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
)
if(WIN32)
  target_link_libraries(sbl_sbsql_parser_worker_core PUBLIC ws2_32)
endif()

if(TARGET sb_server_core)
  target_compile_definitions(sbl_sbsql_parser_worker_core PUBLIC
    SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  target_link_libraries(sbl_sbsql_parser_worker_core PUBLIC sb_server_core)
endif()

add_executable(sbp_sbsql main.cpp)
target_compile_features(sbp_sbsql PRIVATE cxx_std_23)
target_link_libraries(sbp_sbsql PRIVATE sbl_sbsql_parser_worker_core)

add_executable(sbp_probe probe_main.cpp)
target_compile_features(sbp_probe PRIVATE cxx_std_23)
target_link_libraries(sbp_probe PRIVATE sbl_sbsql_parser_worker_core)

if(SB_INSTALL_NON_ENGINE_COMPONENTS)
  install(TARGETS sbp_sbsql
    RUNTIME DESTINATION bin
  )
endif()

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
  # QOW-CMAKE-QRY-001-AST-V1
  parser/parser.cpp
  ast/ast.cpp
  expression/expression_catalog.cpp
  expression/reference_variable_compatibility.cpp
  resources/language_resource_contract.cpp
  statement/statement_catalog.cpp
  # QOW-CMAKE-QRY-001-BINDING-V1
  binder/binder.cpp
  # QOW-ROUTE-STAGE-QRY-005-V1
  # QOW-CMAKE-QRY-005-V1
  # QOW-ROUTE-STAGE-QRY-006-V1
  # QOW-CMAKE-QRY-006-V1
  # QOW-CMAKE-WIN-015-NAMED-V1
  # QOW-CMAKE-WIN-015-MULTIPLE-V1
  # QOW-CMAKE-WIN-015-QUALIFY-V1
  # QOW-CMAKE-WIN-015-COMPOSITION-V1
  # QOW-CMAKE-SBLR-VERIFIER-V1
  lowering/lowering.cpp
  rendering/rendering.cpp
)

target_compile_features(sbl_sbsql_parser_pipeline PUBLIC cxx_std_23)
target_include_directories(sbl_sbsql_parser_pipeline PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(sbl_sbsql_parser_pipeline PUBLIC sbl_parser_server_ipc_schema)

set(SBSQL_PARSER_WORKER_CORE_SOURCES
  embedded/embedded_engine_client.cpp
  lifecycle/parser_lifecycle.cpp
  runtime/parser_runtime.cpp
  wire/sbsql_sbwp_wire.cpp
  wire/sbsql_test_wire.cpp
)

function(sbsql_configure_parser_worker_core target_name)
  target_compile_features(${target_name} PUBLIC cxx_std_23)
  target_include_directories(${target_name} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/listener
  )
  target_link_libraries(${target_name} PUBLIC
    sbl_sbsql_parser_pipeline
    sbl_parser_server_ipc_client
    sbl_listener_control_plane
    sb_core_datatypes
    sb_core_uuid
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
  )
  if(WIN32)
    target_link_libraries(${target_name} PUBLIC ws2_32)
  endif()
endfunction()

function(sbsql_assert_standalone_parser_link_closure target_name)
  set(_pending "${target_name}")
  set(_visited "")
  set(_allowed_parser_targets
    sbl_parser_server_ipc_schema
    sbl_parser_server_ipc_client
    sbl_sbsql_parser_pipeline
    sbl_sbsql_parser_worker_core
    sbp_sbsql
  )
  while(_pending)
    list(POP_FRONT _pending _candidate)
    if(_candidate IN_LIST _visited OR NOT TARGET "${_candidate}")
      continue()
    endif()
    list(APPEND _visited "${_candidate}")

    get_target_property(_direct_links "${_candidate}" LINK_LIBRARIES)
    get_target_property(_interface_links "${_candidate}" INTERFACE_LINK_LIBRARIES)
    foreach(_dependency IN LISTS _direct_links _interface_links)
      set(_dependency_target "${_dependency}")
      if(_dependency_target MATCHES
          "^\\$<(LINK_ONLY|BUILD_INTERFACE):([^>]+)>$")
        set(_dependency_target "${CMAKE_MATCH_2}")
      endif()
      if(_dependency_target MATCHES
          "^(sb_server|sb_engine|sb_storage|sb_transaction|sb_core_agents|sb_core_catalog|sb_core_index)")
        message(FATAL_ERROR
          "SBParser standalone worker link closure reaches forbidden engine "
          "target '${_dependency_target}' through '${_candidate}'")
      endif()
      if(_dependency_target MATCHES "^(sbp_|sbu_|sbl_.*parser)" AND
          NOT _dependency_target IN_LIST _allowed_parser_targets)
        message(FATAL_ERROR
          "SBParser standalone worker link closure reaches forbidden "
          "cross-parser target '${_dependency_target}' through '${_candidate}'")
      endif()
      if(TARGET "${_dependency_target}" AND
          NOT _dependency_target IN_LIST _visited)
        list(APPEND _pending "${_dependency_target}")
      endif()
    endforeach()
  endwhile()
endfunction()

if(NOT TARGET sb_core_datatypes)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/core/datatypes
                   ${CMAKE_BINARY_DIR}/src/core/datatypes)
endif()

# The network-facing parser worker is intentionally engine-free.  Every
# production SBParser request reaches the engine only over the SBPS IPC
# endpoint owned by SBsrv.
add_library(sbl_sbsql_parser_worker_core STATIC
  ${SBSQL_PARSER_WORKER_CORE_SOURCES}
)
sbsql_configure_parser_worker_core(sbl_sbsql_parser_worker_core)
sbsql_assert_standalone_parser_link_closure(sbl_sbsql_parser_worker_core)

# Applying engine-side agent decisions to a parser lifecycle is supervisor
# functionality, not parser functionality.  Keep that adapter in a separate
# target whose dependency direction points toward the standalone worker.  In
# particular, SBParser must never acquire the agent, catalog, storage, index,
# or transaction authority graph through this bridge.
if(TARGET sb_core_agents)
  add_library(sb_engine_parser_lifecycle_bridge STATIC
    lifecycle/agent_parser_interface_bridge.cpp
  )
  target_compile_features(sb_engine_parser_lifecycle_bridge PUBLIC cxx_std_23)
  target_include_directories(sb_engine_parser_lifecycle_bridge PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src/core/agents
  )
  target_link_libraries(sb_engine_parser_lifecycle_bridge PUBLIC
    sbl_sbsql_parser_worker_core
    sb_core_agents
  )
endif()

# Embedded client mode is a separate client-side bridge.  Keeping it in a
# distinct target prevents the standalone SBParser executable from acquiring
# an in-process engine/server dependency merely because SBsrv is built in the
# same CMake graph.
if(TARGET sb_server_core)
  add_library(sbl_sbsql_embedded_client_bridge STATIC
    ${SBSQL_PARSER_WORKER_CORE_SOURCES}
  )
  sbsql_configure_parser_worker_core(sbl_sbsql_embedded_client_bridge)
  target_compile_definitions(sbl_sbsql_embedded_client_bridge PUBLIC
    SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  target_link_libraries(sbl_sbsql_embedded_client_bridge PUBLIC sb_server_core)
endif()

add_executable(sbp_sbsql main.cpp)
target_compile_features(sbp_sbsql PRIVATE cxx_std_23)
target_link_libraries(sbp_sbsql PRIVATE sbl_sbsql_parser_worker_core)
sbsql_assert_standalone_parser_link_closure(sbp_sbsql)

add_executable(sbp_probe probe_main.cpp)
target_compile_features(sbp_probe PRIVATE cxx_std_23)
target_link_libraries(sbp_probe PRIVATE sbl_sbsql_parser_worker_core)

if(SB_INSTALL_NON_ENGINE_COMPONENTS)
  install(TARGETS sbp_sbsql
    RUNTIME DESTINATION bin
  )
endif()

# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(SB_BUILD_QOW_CLOSURE_TESTS)
  if(NOT TARGET sb_qow_sblr_query_route_contract)
    add_subdirectory(
      ${CMAKE_CURRENT_SOURCE_DIR}/../engine/sblr
      ${CMAKE_CURRENT_BINARY_DIR}/qow_engine_sblr
    )
  endif()
  # QOW-CMAKE-SBLR-CANONICAL-ADMISSION-V1
  add_library(sb_qow_sblr_admission_contract STATIC
    sblr_admission.cpp
    sblr_local_gateway.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine/sblr/sblr_local_metrics_read.cpp
  )
  target_compile_features(sb_qow_sblr_admission_contract PUBLIC cxx_std_20)
  target_include_directories(sb_qow_sblr_admission_contract PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine/sblr
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/hash
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  target_link_libraries(sb_qow_sblr_admission_contract PUBLIC
    sb_qow_sblr_query_route_contract
    sb_sblr_operation_wire_codec
    sb_core_hash
  )
  set(SB_QOW_SBLR_ADMISSION_TESTS_ENROLLED TRUE PARENT_SCOPE)
  return()
endif()

if(NOT TARGET sbl_manager_protocol)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../manager/protocol ${CMAKE_CURRENT_BINARY_DIR}/manager_protocol)
endif()
if(NOT TARGET sb_udr_runtime)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../udr/runtime ${CMAKE_CURRENT_BINARY_DIR}/udr_runtime)
endif()
if(NOT TARGET sb_server_engine_bridge)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../server_engine_bridge ${CMAKE_CURRENT_BINARY_DIR}/server_engine_bridge)
endif()
if(NOT TARGET sb_local_ipc)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../ipc ${CMAKE_CURRENT_BINARY_DIR}/local_ipc)
endif()
if(NOT TARGET sb_wire_result_batch)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../wire ${CMAKE_CURRENT_BINARY_DIR}/wire_result_batch)
endif()
if(NOT TARGET sb_core_resource_governance)
  add_library(sb_core_resource_governance
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/agents/resource_governance_admission.cpp
  )
  target_compile_features(sb_core_resource_governance PUBLIC cxx_std_17)
  target_include_directories(sb_core_resource_governance PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/agents
  )
  target_link_libraries(sb_core_resource_governance PUBLIC sb_core_platform sb_core_metrics)
endif()

add_library(sb_server_sbps_private_narrow_profile STATIC
  sbps_private_narrow_profile.cpp
)
target_compile_features(sb_server_sbps_private_narrow_profile PUBLIC cxx_std_23)
target_include_directories(sb_server_sbps_private_narrow_profile PUBLIC
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(sb_server_sbps_private_narrow_profile PUBLIC
  sb_core_platform
)

add_library(sb_server_resource_governance
  server_resource_governance.cpp
)

target_include_directories(sb_server_resource_governance PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(sb_server_resource_governance PUBLIC cxx_std_20)

target_link_libraries(sb_server_resource_governance PUBLIC
  sb_core_resource_governance
)

add_library(sb_server_sblr_local_gateway
  sblr_local_gateway.cpp
)
target_include_directories(sb_server_sblr_local_gateway PUBLIC
  ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(sb_server_sblr_local_gateway PUBLIC cxx_std_20)
target_link_libraries(sb_server_sblr_local_gateway PUBLIC
  sb_core_hash
  sb_engine_sblr
)

if(BUILD_TESTING)
  add_executable(sbsql_sblr_alignment_ia01_local_gateway_contract
    ${CMAKE_SOURCE_DIR}/tests/sbsql_sblr_alignment/ia01_local_gateway_contract_test.cpp
  )
  target_compile_features(sbsql_sblr_alignment_ia01_local_gateway_contract PRIVATE cxx_std_20)
  target_link_libraries(sbsql_sblr_alignment_ia01_local_gateway_contract PRIVATE
    sb_server_sblr_local_gateway
  )
  add_test(NAME sbsql_sblr_alignment_ia01_local_gateway_contract
    COMMAND sbsql_sblr_alignment_ia01_local_gateway_contract)
  set_tests_properties(sbsql_sblr_alignment_ia01_local_gateway_contract PROPERTIES
    LABELS "sbsql_sblr_alignment;sbsql_sblr_alignment_contract;IA-01;SBLR_PACKAGE_BEGIN_END;cluster_gateway")
endif()

add_library(sb_local_parser_shared_memory_transport
  local_parser_shared_memory_transport.cpp
)

target_include_directories(sb_local_parser_shared_memory_transport PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(sb_local_parser_shared_memory_transport PUBLIC cxx_std_20)

target_link_libraries(sb_local_parser_shared_memory_transport PUBLIC
  sb_local_ipc
  sb_wire_result_batch
)

add_library(sb_server_event_notifications
  event_notification_router.cpp
  parser_server_event_frame_dispatcher.cpp
  parser_server_event_ipc.cpp
)

target_include_directories(sb_server_event_notifications PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(sb_server_event_notifications PUBLIC cxx_std_20)

target_link_libraries(sb_server_event_notifications PUBLIC
  sb_engine
  sb_server_engine_bridge
)

# SEARCH_KEY: SB_SERVER_PRODUCT_CMAKE
if(SB_BUILD_SERVER)
  add_library(sb_server_core
    boundary_guard.cpp
    agent_session_control_route_bridge.cpp
    cli.cpp
    compatibility_server_authority.cpp
    config.cpp
    config_policy_security_lifecycle.cpp
    database_ownership.cpp
    diagnostics.cpp
    engine_host.cpp
    ipar_protocol_support.cpp
    ipar_security_policy_support.cpp
    ipc_server.cpp
    lifecycle.cpp
    listener_orchestrator.cpp
    maintenance_coordinator.cpp
    manager_control.cpp
    parser_admission.cpp
    parser_package_registry.cpp
    process_association_registry.cpp
    product_identity.cpp
    sbps.cpp
    server_observability.cpp
    server_agent_runtime.cpp
    server_daemon_lifecycle.cpp
    server_ipc_lifecycle.cpp
    session_registry.cpp
    sblr_admission.cpp
    sblr_dispatch_server.cpp
    startup.cpp
    windows_service_runtime.cpp
  )

  target_include_directories(sb_server_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  )

  target_compile_features(sb_server_core PUBLIC cxx_std_23)

  target_link_libraries(sb_server_core PUBLIC
    sb_core_agents
    sb_core_memory
    sb_server_resource_governance
    sb_server_sblr_local_gateway
    sb_engine
    sb_server_engine_bridge
    sb_server_event_notifications
    sb_local_parser_shared_memory_transport
    sb_udr_runtime
    sbl_manager_protocol
    sbl_listener_control_plane
  )
  if(WIN32)
    target_link_libraries(sb_server_core PUBLIC ws2_32 advapi32)
  endif()

  add_executable(sb_server
    main.cpp
  )

  target_compile_features(sb_server PRIVATE cxx_std_23)

  target_link_libraries(sb_server PRIVATE
    sb_server_core
  )

  add_executable(sb_ipc_tester
    ipc_tester.cpp
  )

  target_compile_features(sb_ipc_tester PRIVATE cxx_std_23)

  target_link_libraries(sb_ipc_tester PRIVATE
    sb_server_core
    sb_server_event_notifications
    sbl_parser_server_ipc_client
  )
endif()

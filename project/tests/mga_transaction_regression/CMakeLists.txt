# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(mga_transaction_regression_fixture
  mga_transaction_regression_fixture.cpp
)

target_compile_features(mga_transaction_regression_fixture PRIVATE cxx_std_23)
target_link_libraries(mga_transaction_regression_fixture
  PRIVATE
    sb_core_platform
    sb_core_uuid
    sb_transaction_mga
)

set(MGA_TRANSACTION_REGRESSION_CASES
  inventory_visibility
  savepoint_rollback
  lock_concurrency
  recovery_classification
  stress_invariant
)

foreach(regression_case IN LISTS MGA_TRANSACTION_REGRESSION_CASES)
  add_test(
    NAME "mga_transaction_regression_${regression_case}"
    COMMAND mga_transaction_regression_fixture --case "${regression_case}"
  )
  set_tests_properties("mga_transaction_regression_${regression_case}" PROPERTIES
    LABELS "mga;transaction;regression"
  )
endforeach()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
  NAME mga_transaction_regression_authority_policy_gate
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../listener/mga_transaction_authority_gate.py"
    "${PROJECT_SOURCE_DIR}"
)
set_tests_properties(mga_transaction_regression_authority_policy_gate PROPERTIES
  LABELS "mga;transaction;regression;authority"
)

add_test(
  NAME mga_transaction_regression_benchmark_storage_policy_gate
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../listener/benchmark_runtime_storage_policy_gate.py"
    "${PROJECT_SOURCE_DIR}/.."
)
set_tests_properties(mga_transaction_regression_benchmark_storage_policy_gate PROPERTIES
  LABELS "mga;transaction;regression;authority"
)

if(TARGET sb_engine_api_transaction_semantics_probe)
  add_test(
    NAME mga_transaction_regression_engine_api_e2e
    COMMAND
      $<TARGET_FILE:sb_engine_api_transaction_semantics_probe>
      --path "${CMAKE_CURRENT_BINARY_DIR}/mga_transaction_regression_engine_api.sbdb"
      --creation-ms 1779000000100
      --overwrite
  )
  set_tests_properties(mga_transaction_regression_engine_api_e2e PROPERTIES
    LABELS "mga;transaction;regression;engine_api"
  )
endif()

if(TARGET sb_single_node_transaction_probe)
  add_test(
    NAME mga_transaction_regression_single_node_inventory
    COMMAND
      $<TARGET_FILE:sb_single_node_transaction_probe>
      --path "${CMAKE_CURRENT_BINARY_DIR}/mga_transaction_regression_single_node.sbdb"
      --seed-pack-root "${PROJECT_SOURCE_DIR}/resources/seed-packs/initial-resource-pack"
      --creation-ms 1779000000200
      --overwrite
  )
  set_tests_properties(mga_transaction_regression_single_node_inventory PROPERTIES
    LABELS "mga;transaction;regression;single_node"
  )
endif()

if(TARGET sb_transaction_recovery_probe)
  add_test(
    NAME mga_transaction_regression_recovery_probe
    COMMAND $<TARGET_FILE:sb_transaction_recovery_probe>
  )
  set_tests_properties(mga_transaction_regression_recovery_probe PROPERTIES
    LABELS "mga;transaction;regression;recovery"
  )
endif()

if(NOT WIN32 AND TARGET sb_server AND TARGET sb_listener AND TARGET sbp_sbsql AND TARGET sb_storage_database)
  add_executable(sbsql_mga_transaction_full_route_conformance
    sbsql_mga_transaction_full_route_conformance.cpp
  )
  target_compile_features(sbsql_mga_transaction_full_route_conformance PRIVATE cxx_std_23)
  target_include_directories(sbsql_mga_transaction_full_route_conformance
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src/engine/internal_api"
  )
  target_link_libraries(sbsql_mga_transaction_full_route_conformance
    PRIVATE
      sb_core_platform
      sb_core_uuid
      sb_engine_internal_api
      sbl_sbsql_parser_pipeline
      sb_storage_database
      sb_transaction_mga
  )
  add_custom_target(mga_transaction_regression_sbsql_ipc_server_engine
    DEPENDS
      sbsql_mga_transaction_full_route_conformance
      sb_server
      sb_listener
      sbp_sbsql
  )

  add_test(
    NAME mga_transaction_regression_sbsql_ipc_server_engine
    COMMAND
      $<TARGET_FILE:sbsql_mga_transaction_full_route_conformance>
      $<TARGET_FILE:sb_server>
      $<TARGET_FILE:sb_listener>
      $<TARGET_FILE:sbp_sbsql>
  )
  set_tests_properties(mga_transaction_regression_sbsql_ipc_server_engine PROPERTIES
    LABELS "public_single_node_full_route_gate;mga;transaction;regression;sbsql;ipc;server;engine"
  )
endif()

if(TARGET sb_server_core)
  add_executable(mga_driver_transaction_reconciliation
    driver_transaction_reconciliation.cpp
  )
  target_compile_features(mga_driver_transaction_reconciliation PRIVATE cxx_std_23)
  target_link_libraries(mga_driver_transaction_reconciliation
    PRIVATE
      sb_server_core
  )

  add_test(
    NAME mga_transaction_regression_driver_reconciliation
    COMMAND mga_driver_transaction_reconciliation
  )
  set_tests_properties(mga_transaction_regression_driver_reconciliation PROPERTIES
    LABELS "mga;transaction;regression;driver;fault_injection;routing_reconnect_finality_gate"
  )
endif()

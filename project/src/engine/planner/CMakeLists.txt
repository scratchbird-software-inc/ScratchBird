# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# QOW-CMAKE-OPT-001-V1
# QOW-CMAKE-OPT-002-V1
# QOW-CMAKE-QRY-030-V1
# QOW-CMAKE-IAS-015-V1
# QOW-CMAKE-OPT-003-V1
# QOW-CMAKE-IAS-006-V1
if(NOT TARGET sb_qow_canonical_logical_graph)
  add_library(sb_qow_canonical_logical_graph INTERFACE)
  target_compile_features(sb_qow_canonical_logical_graph INTERFACE cxx_std_20)
  target_include_directories(sb_qow_canonical_logical_graph INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
  )
endif()

if(NOT TARGET sb_engine_plan_shapes)
  add_library(sb_engine_plan_shapes
    dml_plan.cpp
    expression_plan.cpp
    logical_plan.cpp
    nosql_plan.cpp
    operation_plan.cpp
    plan_diagnostics.cpp
    relational_plan.cpp
  )

  target_include_directories(sb_engine_plan_shapes PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  )

  target_compile_features(sb_engine_plan_shapes PUBLIC cxx_std_20)
endif()

if(NOT TARGET sb_core_memory)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/memory ${CMAKE_CURRENT_BINARY_DIR}/core_memory)
endif()

if(NOT TARGET sb_engine_planner)
  add_library(sb_engine_planner
    reservation_backed_planner_memory_bridge.cpp
    # Compiled removal guard for the former operation-prefix rule planner.
    rule_planner.cpp
  )

  target_include_directories(sb_engine_planner PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  )

  target_compile_features(sb_engine_planner PUBLIC cxx_std_20)

  target_link_libraries(sb_engine_planner PUBLIC
    sb_engine_plan_shapes
    sb_core_memory
    sb_engine_internal_api
    sb_engine_sblr
  )
endif()

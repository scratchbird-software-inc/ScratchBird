# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sb_engine_sblr
  aggregates/sblr_aggregate_family_landing_zone.cpp
  dispatch/sblr_family_dispatch_landing_zone.cpp
  operators/sblr_operator_family_landing_zone.cpp
  special_forms/sblr_special_form_family_landing_zone.cpp
  sblr_aggregate_window_runtime.cpp
  sblr_api_trace.cpp
  sblr_assignment_runtime.cpp
  sblr_block_runtime.cpp
  sblr_context_variables.cpp
  sblr_domain_runtime.cpp
  sblr_engine_envelope.cpp
  sblr_execution_metrics.cpp
  ipar_sblr_support_services.cpp
  sblr_dispatch.cpp
  sblr_hot_path_execution.cpp
  sblr_llvm_seam.cpp
  sblr_native_specialization.cpp
  sblr_operator_runtime.cpp
  sblr_opcode_registry.cpp
  sblr_parser_memory_handoff.cpp
  sblr_parser_authority_guards.cpp
  sblr_parser_projection.cpp
  sblr_prepared_template.cpp
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

target_link_libraries(sb_engine_sblr PUBLIC sb_engine_internal_api sb_engine_executor sb_engine_native_compile sb_core_datatypes)
if(TARGET sb_engine_functions)
  target_link_libraries(sb_engine_sblr PRIVATE sb_engine_functions)
endif()
if(TARGET sb_cluster_provider)
  target_link_libraries(sb_engine_sblr PRIVATE sb_cluster_provider)
endif()

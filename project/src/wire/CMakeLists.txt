# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(OpenSSL REQUIRED)

if(NOT TARGET sb_core_index)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core/index ${CMAKE_CURRENT_BINARY_DIR}/core_index)
endif()
if(NOT TARGET sb_core_memory)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core/memory ${CMAKE_CURRENT_BINARY_DIR}/core_memory)
endif()
if(NOT TARGET sb_core_datatypes)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core/datatypes ${CMAKE_CURRENT_BINARY_DIR}/core_datatypes)
endif()
if(NOT TARGET sb_core_hash)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core/hash ${CMAKE_CURRENT_BINARY_DIR}/core_hash)
endif()

if(NOT TARGET sb_wire_typed_update_carrier_codec)
  add_library(sb_wire_typed_update_carrier_codec STATIC
    typed_update_carrier_codec.cpp)
  target_compile_features(sb_wire_typed_update_carrier_codec PUBLIC cxx_std_20)
  target_include_directories(sb_wire_typed_update_carrier_codec PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(sb_wire_typed_update_carrier_codec PUBLIC
    sb_core_platform
    sb_core_hash)
endif()

if(NOT TARGET sb_wire_typed_result_transport_codec)
  add_library(sb_wire_typed_result_transport_codec STATIC
    typed_result_transport_carrier.cpp
    typed_result_transport_codec.cpp)
  target_compile_features(sb_wire_typed_result_transport_codec PUBLIC cxx_std_20)
  target_include_directories(sb_wire_typed_result_transport_codec PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(sb_wire_typed_result_transport_codec PUBLIC
    sb_core_platform
    sb_core_datatypes
    sb_core_hash)
endif()

if(NOT TARGET sb_wire_narrow_query_binding_demand_codec)
  add_library(sb_wire_narrow_query_binding_demand_codec STATIC
    narrow_query_binding_demand_codec.cpp)
  target_compile_features(sb_wire_narrow_query_binding_demand_codec
    PUBLIC cxx_std_20)
  target_include_directories(sb_wire_narrow_query_binding_demand_codec PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(sb_wire_narrow_query_binding_demand_codec PUBLIC
    sb_core_platform)
endif()

add_library(sb_wire_result_batch STATIC
  direct_binary_result_frame.cpp
  narrow_query_binding_codec.cpp
  result_batch_transfer.cpp
  streaming_cursor_manager.cpp
  streaming_result_window.cpp
)

target_compile_features(sb_wire_result_batch PUBLIC cxx_std_20)
target_include_directories(sb_wire_result_batch PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(sb_wire_result_batch PUBLIC
  sb_wire_narrow_query_binding_demand_codec
  sb_wire_typed_result_transport_codec
  sb_wire_typed_update_carrier_codec
  sb_core_platform
  sb_core_datatypes
  sb_core_index
  sb_core_memory
  sb_engine_executor
  OpenSSL::Crypto
)

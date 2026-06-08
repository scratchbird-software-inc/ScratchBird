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

add_library(sb_wire_result_batch STATIC
  direct_binary_result_frame.cpp
  result_batch_transfer.cpp
  streaming_cursor_manager.cpp
  streaming_result_window.cpp
)

target_compile_features(sb_wire_result_batch PUBLIC cxx_std_20)
target_include_directories(sb_wire_result_batch PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(sb_wire_result_batch PUBLIC
  sb_core_platform
  sb_core_index
  sb_core_memory
  sb_engine_executor
  OpenSSL::Crypto
)

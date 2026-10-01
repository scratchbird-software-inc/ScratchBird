# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
if(NOT TARGET sb_core_memory)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../memory" "${CMAKE_CURRENT_BINARY_DIR}/core_memory")
endif()

add_library(sb_core_concurrency INTERFACE)
target_compile_features(sb_core_concurrency INTERFACE cxx_std_20)
target_include_directories(sb_core_concurrency INTERFACE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(sb_core_concurrency INTERFACE sb_core_memory)

# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sb_udr_runtime STATIC
  sb_udr_runtime.cpp
)

if(NOT TARGET sb_core_memory)
  add_subdirectory(${CMAKE_SOURCE_DIR}/src/core/memory ${CMAKE_CURRENT_BINARY_DIR}/core_memory)
endif()

target_compile_features(sb_udr_runtime PUBLIC cxx_std_23)
target_include_directories(sb_udr_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(sb_udr_runtime PUBLIC sb_core_memory)

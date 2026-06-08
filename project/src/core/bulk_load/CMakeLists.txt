# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET sb_core_index)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../index ${CMAKE_CURRENT_BINARY_DIR}/core_index)
endif()

add_library(sb_core_bulk_load
  bulk_constraint_proof.cpp
  parallel_copy_pipeline_route.cpp
  strict_bulk_load_lifecycle.cpp
)

target_compile_features(sb_core_bulk_load PUBLIC cxx_std_17)
target_include_directories(sb_core_bulk_load PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sb_core_bulk_load PUBLIC sb_core_platform sb_core_uuid sb_core_index)

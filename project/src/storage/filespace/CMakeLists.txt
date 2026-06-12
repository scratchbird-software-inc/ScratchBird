# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET sb_core_metrics)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/metrics ${CMAKE_CURRENT_BINARY_DIR}/core_metrics)
endif()

add_library(sb_storage_filespace
  cloud_filespace_provider.cpp
  filespace_header.cpp
  filespace_identity.cpp
  filespace_discovery.cpp
  final_deferred_storage_workflows.cpp
  filespace_growth.cpp
  filespace_lifecycle.cpp
  filespace_package.cpp
  filespace_secondary.cpp
  foreign_filespace_quarantine.cpp
)

target_compile_features(sb_storage_filespace PUBLIC cxx_std_17)
target_include_directories(sb_storage_filespace PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sb_storage_filespace PUBLIC sb_core_platform sb_core_uuid sb_core_metrics sb_storage_disk)

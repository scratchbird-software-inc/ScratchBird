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

add_library(sb_catalog_bootstrap
  catalog_bootstrap.cpp
  catalog_identity.cpp
  catalog_persistence.cpp
  metric_retention_bootstrap.cpp
)

target_compile_features(sb_catalog_bootstrap PUBLIC cxx_std_17)
target_include_directories(sb_catalog_bootstrap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sb_catalog_bootstrap PUBLIC sb_core_platform sb_core_uuid sb_core_metrics)

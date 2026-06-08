# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sb_cluster_provider
  stub_cluster_provider.cpp
)

target_compile_features(sb_cluster_provider PUBLIC cxx_std_20)

target_include_directories(sb_cluster_provider PUBLIC
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/engine/sblr
)

target_link_libraries(sb_cluster_provider PUBLIC
  sb_engine_internal_api
)

target_compile_definitions(sb_cluster_provider PUBLIC
  SCRATCHBIRD_CLUSTER_PROVIDER_STUB=1
)

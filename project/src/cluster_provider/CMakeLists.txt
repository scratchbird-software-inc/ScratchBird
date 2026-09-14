# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sb_cluster_provider
  no_cluster_provider.cpp
)

target_compile_features(sb_cluster_provider PUBLIC cxx_std_20)

target_include_directories(sb_cluster_provider PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../engine/sblr
)

target_link_libraries(sb_cluster_provider PUBLIC
  sb_engine_api_diagnostics
)

target_compile_definitions(sb_cluster_provider PUBLIC
  SCRATCHBIRD_CLUSTER_PROVIDER_NO_CLUSTER=1
)

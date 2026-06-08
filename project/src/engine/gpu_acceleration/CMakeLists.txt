# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sb_engine_gpu_acceleration
  gpu_acceleration.cpp
  scoring_kernel_acceleration.cpp
)

if(NOT TARGET sb_core_resource_governance)
  add_library(sb_core_resource_governance
    ${CMAKE_CURRENT_SOURCE_DIR}/../../core/agents/resource_governance_admission.cpp
  )
  target_compile_features(sb_core_resource_governance PUBLIC cxx_std_17)
  target_include_directories(sb_core_resource_governance PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../core/agents
  )
  target_link_libraries(sb_core_resource_governance PUBLIC sb_core_platform sb_core_metrics)
endif()

target_include_directories(sb_engine_gpu_acceleration PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(sb_engine_gpu_acceleration PUBLIC cxx_std_20)

target_link_libraries(sb_engine_gpu_acceleration PUBLIC
  sb_core_platform
  sb_core_resource_governance
  sb_core_metrics
)

# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sb_core_metrics
  adaptive_tuning_metrics_evidence.cpp
  cluster_metric_descriptors.cpp
  metric_contracts.cpp
  metric_export.cpp
  metric_history.cpp
  metric_producer.cpp
  metric_registry.cpp
  metric_retention_policy.cpp
)

target_compile_features(sb_core_metrics PUBLIC cxx_std_17)
target_include_directories(sb_core_metrics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sb_core_metrics PUBLIC sb_core_platform)

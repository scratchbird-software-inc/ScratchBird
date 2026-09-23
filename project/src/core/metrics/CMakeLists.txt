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
  metric_history.cpp
  metric_history_record.cpp
  metric_producer.cpp
  metric_registry.cpp
  metric_scalar.cpp
  metric_value_update.cpp
  metric_value_codec.cpp
  metric_sample_codec.cpp
  metric_observation_queue.cpp
  metric_retention_policy.cpp
  storage_metrics_management.cpp
)

target_compile_features(sb_core_metrics PUBLIC cxx_std_23)
target_include_directories(sb_core_metrics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(sb_core_metrics PRIVATE ${SBL_NUMERIC_BOOST_INCLUDE_DIR})
target_link_libraries(sb_core_metrics PUBLIC sb_core_platform sb_core_uuid)
target_link_libraries(sb_core_metrics PRIVATE sbl_numeric sb_core_hash)

# Human-readable metrics rendering belongs to clients, never the engine target.
add_library(sb_client_metric_export ../../../tools/metrics_client/metric_export.cpp)
target_compile_features(sb_client_metric_export PUBLIC cxx_std_23)
target_include_directories(sb_client_metric_export PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/metrics_client)
target_include_directories(sb_client_metric_export PRIVATE ${SBL_NUMERIC_BOOST_INCLUDE_DIR})
target_link_libraries(sb_client_metric_export PUBLIC sb_core_metrics PRIVATE sbl_numeric)

# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(sb_metrics_cluster_history_probe main.cpp)
target_compile_features(sb_metrics_cluster_history_probe PRIVATE cxx_std_20)
target_include_directories(sb_metrics_cluster_history_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(sb_metrics_cluster_history_probe PRIVATE sb_engine_internal_api sb_core_metrics)

# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET sb_server_legacy_diagnostic_rendering)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/server/diagnostic_rendering
                   ${CMAKE_CURRENT_BINARY_DIR}/server_legacy_diagnostic_rendering)
endif()

add_executable(sb_sbsql_v3_diagnostic_rendering_probe main.cpp)
target_link_libraries(sb_sbsql_v3_diagnostic_rendering_probe PRIVATE sb_server_legacy_diagnostic_rendering)
target_compile_features(sb_sbsql_v3_diagnostic_rendering_probe PRIVATE cxx_std_20)

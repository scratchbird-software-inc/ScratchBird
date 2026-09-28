# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
add_library(sb_legacy_diagnostic_projection_bridge STATIC
  ../legacy_diagnostic_projection.cpp)
target_compile_features(sb_legacy_diagnostic_projection_bridge PUBLIC cxx_std_23)
target_include_directories(sb_legacy_diagnostic_projection_bridge PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(sb_legacy_diagnostic_projection_bridge
  PUBLIC sb_server_legacy_diagnostic_rendering
  PRIVATE sb_engine_internal_api)

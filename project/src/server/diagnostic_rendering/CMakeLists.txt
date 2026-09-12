# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

# Server-side legacy adapter, deliberately never linked back into the engine.
add_library(sb_server_legacy_diagnostic_rendering STATIC diagnostic_rendering.cpp)
target_compile_features(sb_server_legacy_diagnostic_rendering PUBLIC cxx_std_23)
target_include_directories(sb_server_legacy_diagnostic_rendering PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(sb_server_legacy_diagnostic_rendering PUBLIC sb_engine_internal_api)

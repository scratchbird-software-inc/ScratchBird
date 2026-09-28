# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
# Core-only owned projection renderer, never linked back into the engine.
add_library(sb_server_legacy_diagnostic_rendering STATIC diagnostic_rendering.cpp)
target_compile_features(sb_server_legacy_diagnostic_rendering PUBLIC cxx_std_23)
target_include_directories(sb_server_legacy_diagnostic_rendering PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(sb_server_legacy_diagnostic_rendering PUBLIC
  sb_core_uuid sb_core_diagnostic_catalog)
if(NOT TARGET sb_legacy_diagnostic_projection_bridge)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../server_engine_bridge/legacy_diagnostic_projection
    ${CMAKE_CURRENT_BINARY_DIR}/legacy_diagnostic_projection_bridge)
endif()

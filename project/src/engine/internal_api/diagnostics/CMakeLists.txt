# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

# Shared diagnostic construction has no dependency on executable engine APIs.
# In-tree cluster providers must remain callable during fenced node startup.
add_library(sb_engine_api_diagnostics ../api_unsupported.cpp)
target_compile_features(sb_engine_api_diagnostics PUBLIC cxx_std_20)
target_include_directories(sb_engine_api_diagnostics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(sb_engine_api_diagnostics PUBLIC sb_core_diagnostic_catalog sb_core_uuid)

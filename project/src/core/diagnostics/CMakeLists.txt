# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
add_library(sb_core_diagnostic_catalog canonical_diagnostic_catalog.cpp)
target_compile_features(sb_core_diagnostic_catalog PUBLIC cxx_std_17)
target_include_directories(sb_core_diagnostic_catalog PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

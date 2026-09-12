# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
# Shared transport structure only: no engine execution or authorization dependency.
if(NOT TARGET sb_wire_diagnostic_identity_projection_codec)
  add_library(sb_wire_diagnostic_identity_projection_codec STATIC
    ${PROJECT_SOURCE_DIR}/src/wire/diagnostic_identity_projection_codec.cpp)
  target_compile_features(sb_wire_diagnostic_identity_projection_codec PUBLIC cxx_std_23)
  target_include_directories(sb_wire_diagnostic_identity_projection_codec PRIVATE
    ${PROJECT_SOURCE_DIR}/src/wire)
  target_link_libraries(sb_wire_diagnostic_identity_projection_codec PUBLIC sb_core_hash)
endif()

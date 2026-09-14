# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

# Internal public-gateway transport checks; this is not the six-export gateway
# library and must never be packaged or advertised as that library.
add_library(sb_cluster_gateway_result_transport STATIC gateway_result_transport.cpp)
target_compile_features(sb_cluster_gateway_result_transport PUBLIC cxx_std_17)
target_include_directories(sb_cluster_gateway_result_transport PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../include/shared)
set_target_properties(sb_cluster_gateway_result_transport PROPERTIES
  POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)

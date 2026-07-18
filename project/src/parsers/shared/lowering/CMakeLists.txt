# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sb_parser_sblr_lowering STATIC
  engine_api_bridge.cpp
  sblr_envelope.cpp
)

target_compile_features(sb_parser_sblr_lowering PUBLIC cxx_std_23)

target_link_libraries(sb_parser_sblr_lowering
  PUBLIC
    sb_engine_internal_api
    sb_parser_bound_ast_model
)

target_include_directories(sb_parser_sblr_lowering
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

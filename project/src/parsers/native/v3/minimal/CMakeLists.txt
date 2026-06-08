# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sb_native_v3_minimal_parser STATIC
  native_minimal_parser.cpp
)

target_compile_features(sb_native_v3_minimal_parser PUBLIC cxx_std_23)

target_link_libraries(sb_native_v3_minimal_parser
  PUBLIC
    sb_parser_ast_model
)

target_include_directories(sb_native_v3_minimal_parser
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

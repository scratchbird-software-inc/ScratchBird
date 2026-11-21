# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# SEARCH_KEY: SB_PARSER_NAME_RESOLUTION. The parser-side name resolver
# (Part B step S1). It depends on the C++ standard library only: a parser links
# it, the engine never does.
add_library(sb_parser_name_resolution STATIC
  name_resolution.cpp
)

target_compile_features(sb_parser_name_resolution PUBLIC cxx_std_20)

# Consumers include "name_resolution/name_resolution.hpp".
target_include_directories(sb_parser_name_resolution
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

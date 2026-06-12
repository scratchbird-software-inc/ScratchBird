# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sbu_firebird_parser_support STATIC
  sbu_firebird_parser_support.cpp
)

if(NOT TARGET sb_udr_runtime)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../runtime ${CMAKE_CURRENT_BINARY_DIR}/udr_runtime)
endif()

target_compile_features(sbu_firebird_parser_support PUBLIC cxx_std_23)
target_include_directories(sbu_firebird_parser_support PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/src/parsers/compatibility/firebird
)
target_link_libraries(sbu_firebird_parser_support PUBLIC
  sb_udr_runtime
  sbl_firebird_parser_pipeline
)

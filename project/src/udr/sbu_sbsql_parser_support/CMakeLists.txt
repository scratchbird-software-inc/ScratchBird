# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sbu_sbsql_parser_support STATIC
  sbu_sbsql_parser_support.cpp
)

if(NOT TARGET sb_udr_runtime)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../runtime ${CMAKE_CURRENT_BINARY_DIR}/udr_runtime)
endif()

target_compile_features(sbu_sbsql_parser_support PUBLIC cxx_std_23)
target_include_directories(sbu_sbsql_parser_support PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/src/parsers/sbsql_worker
)
target_link_libraries(sbu_sbsql_parser_support PUBLIC
  sb_udr_runtime
  sbl_sbsql_parser_pipeline
  sb_engine_sblr
)

if(SB_INSTALL_NON_ENGINE_COMPONENTS)
  install(TARGETS sbu_sbsql_parser_support
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
  )
endif()

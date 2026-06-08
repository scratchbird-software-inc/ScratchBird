# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET sb_udr_runtime)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/udr/runtime ${CMAKE_CURRENT_BINARY_DIR}/udr_runtime_for_immudb)
endif()

if(NOT TARGET sbl_immudb_parser_pipeline)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/parsers/donor/immudb ${CMAKE_CURRENT_BINARY_DIR}/immudb_parser_for_udr)
endif()

add_library(sbu_immudb_parser_support STATIC
  sbu_immudb_parser_support.cpp
)

target_compile_features(sbu_immudb_parser_support PUBLIC cxx_std_23)
target_include_directories(sbu_immudb_parser_support PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/src/parsers/donor/immudb
)
target_link_libraries(sbu_immudb_parser_support PUBLIC
  sb_udr_runtime
  sbl_immudb_parser_pipeline
)

if(SB_INSTALL_NON_ENGINE_COMPONENTS)
  install(TARGETS sbu_immudb_parser_support
          ARCHIVE DESTINATION lib/scratchbird/udr
          LIBRARY DESTINATION lib/scratchbird/udr
          RUNTIME DESTINATION bin)
endif()

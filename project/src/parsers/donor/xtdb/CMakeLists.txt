# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET sbl_donor_parser_common)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/parsers/donor/common ${CMAKE_CURRENT_BINARY_DIR}/donor_parser_common_for_xtdb)
endif()

if(NOT TARGET sbl_listener_control_plane)
  if(NOT TARGET sbl_manager_protocol)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/manager/protocol ${CMAKE_CURRENT_BINARY_DIR}/manager_protocol_for_xtdb)
  endif()
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/listener ${CMAKE_CURRENT_BINARY_DIR}/listener_control_for_xtdb)
endif()

add_library(sbl_xtdb_parser_pipeline STATIC
  xtdb_dialect.cpp
)

target_compile_features(sbl_xtdb_parser_pipeline PUBLIC cxx_std_23)
target_include_directories(sbl_xtdb_parser_pipeline PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(sbl_xtdb_parser_pipeline PUBLIC
  sbl_donor_parser_common
)

add_executable(sbp_xtdb main.cpp)
target_compile_features(sbp_xtdb PRIVATE cxx_std_23)
target_link_libraries(sbp_xtdb PRIVATE
  sbl_xtdb_parser_pipeline
  sbl_donor_parser_common
  sbl_listener_control_plane
)

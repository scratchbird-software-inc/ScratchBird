# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(OpenSSL REQUIRED)
if(NOT TARGET sb_core_platform)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/core/platform
                   ${CMAKE_CURRENT_BINARY_DIR}/core_platform)
endif()
if(NOT TARGET sb_core_uuid)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/core/uuid
                   ${CMAKE_CURRENT_BINARY_DIR}/core_uuid)
endif()

add_library(sbl_manager_protocol
  manager_protocol.cpp
)

target_include_directories(sbl_manager_protocol PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(sbl_manager_protocol PUBLIC cxx_std_23)

target_link_libraries(sbl_manager_protocol PRIVATE
  sb_core_uuid
  OpenSSL::Crypto
)

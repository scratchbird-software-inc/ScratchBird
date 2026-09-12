# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
if(NOT TARGET sb_core_time)
  add_subdirectory(${PROJECT_SOURCE_DIR}/src/core/time
                   ${CMAKE_CURRENT_BINARY_DIR}/core_time)
endif()

add_library(sb_core_uuid
  uuid.cpp
  runtime_uuid.cpp
)

target_compile_features(sb_core_uuid PUBLIC cxx_std_17)
target_include_directories(sb_core_uuid PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sb_core_uuid PUBLIC sb_core_platform sb_core_time OpenSSL::Crypto Threads::Threads)

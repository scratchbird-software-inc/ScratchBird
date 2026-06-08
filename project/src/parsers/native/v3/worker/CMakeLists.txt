# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

if(NOT TARGET sb_transaction_mga)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../transaction/mga ${CMAKE_CURRENT_BINARY_DIR}/transaction_mga)
endif()

add_executable(sbp_native main.cpp)
target_compile_features(sbp_native PRIVATE cxx_std_23)
target_include_directories(sbp_native PRIVATE
  ${PROJECT_SOURCE_DIR}/src/listener
)
target_link_libraries(sbp_native PRIVATE
  sbl_listener_control_plane
  sb_transaction_mga
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
)

# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.20)

find_package(OpenSSL REQUIRED)

add_library(scratchbird_mojo_client_bridge SHARED
    src/scratchbird_mojo_client_bridge.cpp
)

target_compile_features(scratchbird_mojo_client_bridge PRIVATE cxx_std_17)
target_link_libraries(scratchbird_mojo_client_bridge PRIVATE scratchbird_client OpenSSL::Crypto)

target_include_directories(scratchbird_mojo_client_bridge
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../cpp/include"
)

set_target_properties(scratchbird_mojo_client_bridge PROPERTIES
    OUTPUT_NAME "scratchbird_mojo_client_bridge"
    POSITION_INDEPENDENT_CODE ON
)

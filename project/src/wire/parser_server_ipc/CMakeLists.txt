# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sbl_parser_server_ipc_schema STATIC
  parser_ipc_common.cpp
  parser_server_ipc.cpp
)

target_compile_features(sbl_parser_server_ipc_schema PUBLIC cxx_std_23)
target_include_directories(sbl_parser_server_ipc_schema PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

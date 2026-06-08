# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# SEARCH_KEY: SB_SERVER_BOUNDARY_GUARD_CMAKE

add_executable(sb_server_boundary_guard
  main.cpp
)

target_compile_features(sb_server_boundary_guard PRIVATE cxx_std_23)

add_test(NAME sb_server_boundary_guard
  COMMAND sb_server_boundary_guard ${PROJECT_SOURCE_DIR}/src/server)

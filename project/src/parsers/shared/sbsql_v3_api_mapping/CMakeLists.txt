# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sb_sbsql_v3_api_mapping_catalog STATIC
  sbsql_v3_api_mapping_catalog.cpp
)

target_compile_features(sb_sbsql_v3_api_mapping_catalog PUBLIC cxx_std_20)

target_include_directories(sb_sbsql_v3_api_mapping_catalog PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

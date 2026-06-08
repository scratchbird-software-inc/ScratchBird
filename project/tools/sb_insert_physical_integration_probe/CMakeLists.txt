# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(sb_insert_physical_integration_probe
  main.cpp
  ${CMAKE_SOURCE_DIR}/src/engine/internal_api/dml/insert_physical_integration.cpp
)

target_compile_features(sb_insert_physical_integration_probe PRIVATE cxx_std_17)
target_include_directories(sb_insert_physical_integration_probe PRIVATE
  ${CMAKE_SOURCE_DIR}/src/engine/internal_api/dml
)
target_link_libraries(sb_insert_physical_integration_probe PRIVATE
  sb_storage_page
  sb_storage_filespace
  sb_core_index
  sb_core_bulk_load
  sb_core_uuid
)

add_test(NAME sb_insert_physical_integration_probe COMMAND sb_insert_physical_integration_probe)

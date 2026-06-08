# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(sb_single_node_transaction_probe
  main.cpp
)

target_compile_features(sb_single_node_transaction_probe PRIVATE cxx_std_23)
target_link_libraries(sb_single_node_transaction_probe
  PRIVATE
    sb_core_memory
    sb_core_platform
    sb_core_uuid
    sb_storage_database
    sb_storage_page
    sb_transaction_mga
)

add_test(NAME sb_single_node_transaction_probe
  COMMAND sb_single_node_transaction_probe
    --path ${CMAKE_CURRENT_BINARY_DIR}/sb_single_node_transaction_probe.sbdb
    --seed-pack-root ${CMAKE_SOURCE_DIR}/resources/seed-packs/initial-resource-pack
    --creation-ms 1770000000000
    --overwrite
)

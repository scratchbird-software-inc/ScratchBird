# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(sb_overflow_insert_persistence_probe
  main.cpp
)

target_compile_features(sb_overflow_insert_persistence_probe PRIVATE cxx_std_17)
target_link_libraries(sb_overflow_insert_persistence_probe PRIVATE sb_storage_page sb_core_uuid)

add_test(NAME sb_overflow_insert_persistence_probe COMMAND sb_overflow_insert_persistence_probe)

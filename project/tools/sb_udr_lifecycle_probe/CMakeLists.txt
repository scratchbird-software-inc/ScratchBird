# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(sb_udr_lifecycle_probe main.cpp)
target_compile_features(sb_udr_lifecycle_probe PRIVATE cxx_std_20)
target_link_libraries(sb_udr_lifecycle_probe PRIVATE sb_engine_internal_api sb_storage_database sb_core_uuid)
add_test(NAME sb_udr_lifecycle_probe COMMAND sb_udr_lifecycle_probe)

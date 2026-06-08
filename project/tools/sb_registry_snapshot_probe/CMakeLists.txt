# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(sb_registry_snapshot_probe main.cpp)

target_compile_features(sb_registry_snapshot_probe PRIVATE cxx_std_17)
target_link_libraries(sb_registry_snapshot_probe PRIVATE sb_parser_registry_snapshot)

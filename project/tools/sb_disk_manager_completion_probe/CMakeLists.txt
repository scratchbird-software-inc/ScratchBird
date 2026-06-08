# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(sb_disk_manager_completion_probe
  main.cpp
)

target_compile_features(sb_disk_manager_completion_probe PRIVATE cxx_std_23)
target_link_libraries(sb_disk_manager_completion_probe
  PRIVATE
    sb_core_platform
    sb_core_uuid
    sb_storage_disk
)

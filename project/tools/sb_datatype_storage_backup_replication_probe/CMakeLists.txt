# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(sb_datatype_storage_backup_replication_probe main.cpp)
target_link_libraries(sb_datatype_storage_backup_replication_probe PRIVATE sb_engine_internal_api)
target_compile_features(sb_datatype_storage_backup_replication_probe PRIVATE cxx_std_20)

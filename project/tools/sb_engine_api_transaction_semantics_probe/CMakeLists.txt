# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_executable(sb_engine_api_transaction_semantics_probe main.cpp)
target_link_libraries(sb_engine_api_transaction_semantics_probe PRIVATE sb_engine_internal_api sb_storage_database sb_core_uuid)
target_compile_features(sb_engine_api_transaction_semantics_probe PRIVATE cxx_std_20)
add_test(NAME sb_engine_api_transaction_semantics_probe COMMAND sb_engine_api_transaction_semantics_probe
  --path /tmp/sb_engine_api_transaction_semantics_probe.sbdb
  --creation-ms 1777936201035
  --overwrite)

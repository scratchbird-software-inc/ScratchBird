# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sb_local_ipc
  local_ipc_contract.cpp
  shared_memory_ipc_ring.cpp
)

target_include_directories(sb_local_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(sb_local_ipc PUBLIC cxx_std_23)
target_link_libraries(sb_local_ipc PUBLIC sb_core_platform)

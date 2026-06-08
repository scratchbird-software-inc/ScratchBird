# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET sb_core_memory)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/memory ${CMAKE_CURRENT_BINARY_DIR}/core_memory)
endif()
if(NOT TARGET sb_core_metrics)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/metrics ${CMAKE_CURRENT_BINARY_DIR}/core_metrics)
endif()
if(NOT TARGET sb_core_agents)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/agents ${CMAKE_CURRENT_BINARY_DIR}/core_agents)
endif()

add_library(sb_storage_database
  database_lifecycle.cpp
  database_dirty_manifest.cpp
  local_transaction_store.cpp
  physical_mga_cow_store.cpp
  repair_event_ledger.cpp
  repair_history_inspection.cpp
  startup_state.cpp
  write_path_batching.cpp
)

target_compile_features(sb_storage_database PUBLIC cxx_std_17)
target_include_directories(sb_storage_database PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sb_storage_database PUBLIC sb_core_platform sb_core_uuid sb_core_memory sb_core_metrics sb_core_agents sb_core_resources sb_core_catalog sb_storage_disk sb_storage_page)

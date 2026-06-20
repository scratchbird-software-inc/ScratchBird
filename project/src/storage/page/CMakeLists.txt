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
if(NOT TARGET sb_core_hash)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/hash ${CMAKE_CURRENT_BINARY_DIR}/core_hash)
endif()
if(NOT TARGET sb_transaction_mga)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../transaction/mga ${CMAKE_CURRENT_BINARY_DIR}/transaction_mga)
endif()
if(NOT TARGET sb_core_resource_governance)
  add_library(sb_core_resource_governance
    ${CMAKE_CURRENT_SOURCE_DIR}/../../core/agents/resource_governance_admission.cpp
  )
  target_compile_features(sb_core_resource_governance PUBLIC cxx_std_17)
  target_include_directories(sb_core_resource_governance PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../core/agents
  )
  target_link_libraries(sb_core_resource_governance PUBLIC sb_core_platform sb_core_metrics)
endif()
if(NOT TARGET sb_storage_filespace)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../filespace ${CMAKE_CURRENT_BINARY_DIR}/storage_filespace)
endif()

add_library(sb_storage_page
  async_page_io.cpp
  allocation_map_page.cpp
  buffer_prefetch_readahead.cpp
  catalog_page.cpp
  cluster_transaction_page.cpp
  damaged_page_quarantine.cpp
  hot_cold_row_split.cpp
  index_btree_page.cpp
  index_hash_page.cpp
  index_page_family.cpp
  index_specialized_pages.cpp
  ipar_integrity_verifier.cpp
  ipar_storage_support_services.cpp
  late_payload_fetch.cpp
  large_payload.cpp
  metric_history_page.cpp
  mga_page_finality_map.cpp
  ordered_ingest.cpp
  overflow_persistence.cpp
  optimizer_storage_metrics.cpp
  parallel_physical_storage_routes.cpp
  page_filespace_handoff.cpp
  page_allocation_lifecycle.cpp
  page_body_integrity.cpp
  page_cache.cpp
  page_layout.cpp
  page_manager.cpp
  plan_aware_prefetch.cpp
  page_reservation.cpp
  page_selection.cpp
  page_skeleton.cpp
  page_registry.cpp
  reserved_page_family_body.cpp
  row_data_page.cpp
  row_data_physical_sweep.cpp
  row_version_cleanup_batch.cpp
  repair_identity_rules.cpp
  toast_page.cpp
  structured_page_body.cpp
  transaction_inventory_page.cpp
)

target_compile_features(sb_storage_page PUBLIC cxx_std_17)
target_include_directories(sb_storage_page PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sb_storage_page PUBLIC sb_core_platform sb_core_uuid sb_core_hash sb_core_datatypes sb_core_memory sb_core_metrics sb_core_resource_governance sb_storage_disk sb_storage_filespace sb_transaction_mga)

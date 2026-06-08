# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if(NOT TARGET sb_core_metrics)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../metrics ${CMAKE_CURRENT_BINARY_DIR}/core_metrics)
endif()
if(NOT TARGET sb_core_catalog)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../catalog ${CMAKE_CURRENT_BINARY_DIR}/core_catalog)
endif()
if(NOT TARGET sb_transaction_mga)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../transaction/mga ${CMAKE_CURRENT_BINARY_DIR}/transaction_mga)
endif()
if(NOT TARGET sb_storage_page)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../storage/page ${CMAKE_CURRENT_BINARY_DIR}/storage_page)
endif()
if(NOT TARGET sb_core_memory)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../memory ${CMAKE_CURRENT_BINARY_DIR}/core_memory)
endif()

add_library(sb_core_index
  batch_point_lookup.cpp
  btree_unique_durable_provider_closure.cpp
  ceic_036_canonical_key_ordering_fuzz.cpp
  cluster_catalog_index_profiles.cpp
  ceic_034_unique_reservation_finality_protocol.cpp
  covering_index_payload.cpp
  candidate_set.cpp
  compressed_bitmap_spill.cpp
  compression_policy.cpp
  deferred_secondary_index_runtime_policy.cpp
  donor_emulated_index_mapping.cpp
  exact_index_leaf_cleanup.cpp
  expression_index_extractor.cpp
  full_text_runtime.cpp
  gin_physical_provider.cpp
  gist_physical_provider.cpp
  graph_adjacency_physical_provider.cpp
  hash_durable_provider_closure.cpp
  heavy_immutable_generation_publication.cpp
  hot_point_lookup_cache.cpp
  in_memory_index_runtime.cpp
  index_acceleration.cpp
  index_access_method.cpp
  index_apply_planner.cpp
  index_agent_coordination.cpp
  index_api_sblr.cpp
  index_artifacts.cpp
  index_backup_restore.cpp
  index_family_registry.cpp
  index_family_benchmark_gate.cpp
  index_family_management_surface.cpp
  index_fault_injection_matrix.cpp
  index_filter_access.cpp
  index_compact_encoding.cpp
  index_family_route_classification.cpp
  inverted_search_segment_publication.cpp
  index_key_encoding.cpp
  index_lineage.cpp
  index_localization.cpp
  index_maintenance.cpp
  index_management.cpp
  index_metrics.cpp
  index_mga_recovery_contract.cpp
  index_optimizer_runtime_metrics.cpp
  index_metapage.cpp
  index_ordered_access.cpp
  index_ownership.cpp
  index_policy.cpp
  index_posting.cpp
  index_bulk_publish_recovery.cpp
  index_root_generation_publish.cpp
  index_quarantine.cpp
  index_recheck.cpp
  index_residency.cpp
  index_resource_boundary.cpp
  index_route_capability.cpp
  index_spatial_vector_graph_access.cpp
  index_statistics_lifecycle.cpp
  index_text_document_access.cpp
  index_transaction.cpp
  index_optimizer_integration.cpp
  index_verification.cpp
  index_validation_repair_tooling.cpp
  ngram_physical_provider.cpp
  page_extent_summary.cpp
  parallel_index_constraint_publication.cpp
  partial_index_implication.cpp
  physical_bloom_filter.cpp
  physical_columnar_zone.cpp
  physical_zone_summary.cpp
  policy_blocked_index_admission.cpp
  row_page_compact_encoding.cpp
  secondary_index_garbage_cleanup.cpp
  shadow_index_build_lifecycle.cpp
  shadow_index_delta_drain_recovery.cpp
  secondary_index_delta_ledger.cpp
  secondary_index_delta_merge.cpp
  secondary_index_delta_overlay.cpp
  spatial_rtree_physical_provider.cpp
  spgist_physical_provider.cpp
  sorted_bulk_index_build.cpp
  specialized_persistent_provider_closure.cpp
  temporary_work_index_runtime.cpp
  text_inverted_segment.cpp
  time_range_summary_pruning.cpp
  typed_slab_index_work_area.cpp
  unique_index_deferral_policy.cpp
  unique_index_reservation_ledger.cpp
  uuid_v7_index_encoding.cpp
  vector_exact_physical_provider.cpp
  vector_hnsw_physical_provider.cpp
  vector_ivf_pq_physical_provider.cpp
  vector_provider_maintenance.cpp
  vector_index_generation_publication.cpp
  vector_training_recall_lifecycle.cpp
)

target_compile_features(sb_core_index PUBLIC cxx_std_17)
target_include_directories(sb_core_index PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sb_core_index PUBLIC sb_core_platform sb_core_uuid sb_core_metrics sb_core_catalog sb_transaction_mga sb_storage_page sb_core_memory)

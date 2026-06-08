// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-028_FRAGMENTATION_PROFILER_DIFF
#include "metric_registry.hpp"
#include "typed_slab_pool.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::memory {

// CEIC-028_TYPED_POOL_ARENA_OBJECT_COVERAGE
enum class MemoryFragmentationObjectClass {
  plan_node,
  expression_node,
  predicate_node,
  row_locator,
  candidate_set,
  posting_list,
  hash_bucket,
  sort_descriptor,
  vector_scratch,
  diagnostic_record,
  metric_label,
  page_cache_metadata
};

enum class MemoryFragmentationSourceKind {
  typed_slab_pool,
  typed_arena,
  page_cache_frame_pool,
  temp_workspace,
  support_bundle
};

struct MemoryFragmentationProfileKey {
  std::string database_id;
  std::string tenant_id;
  std::string user_id;
  std::string session_id;
  std::string transaction_id;
  std::string statement_id;
  std::string query_id;
  std::string operator_id;
  std::string category;
  std::string memory_class;
  std::string callsite;
};

struct MemoryFragmentationProfileRecord {
  MemoryFragmentationProfileKey key;
  MemoryFragmentationObjectClass object_class =
      MemoryFragmentationObjectClass::plan_node;
  MemoryFragmentationSourceKind source_kind =
      MemoryFragmentationSourceKind::typed_arena;
  u64 allocated_bytes = 0;
  u64 retained_bytes = 0;
  u64 reusable_bytes = 0;
  u64 returned_to_os_bytes = 0;
  u64 slab_active_slots = 0;
  u64 slab_total_slots = 0;
  u64 arena_waste_bytes = 0;
  u64 page_cache_frame_reuse_count = 0;
  u64 page_cache_frame_allocate_count = 0;
  u64 temp_workspace_active_bytes = 0;
  u64 allocation_count = 0;
  u64 release_count = 0;
  u64 reset_count = 0;
  u64 move_count = 0;
  u64 teardown_count = 0;
  u64 allocation_latency_p95_ns = 0;
  u64 allocation_latency_p99_ns = 0;
  bool reset_order_observed = false;
  bool move_semantics_observed = false;
  bool leak_free_teardown_observed = false;
  std::vector<std::string> evidence;
};

struct MemoryFragmentationSupportBundleRow {
  std::string key;
  std::string value;
  std::string redaction_class = "public";
  bool redacted = false;
};

struct MemoryFragmentationProfileRow {
  MemoryFragmentationProfileKey key;
  MemoryFragmentationObjectClass object_class =
      MemoryFragmentationObjectClass::plan_node;
  MemoryFragmentationSourceKind source_kind =
      MemoryFragmentationSourceKind::typed_arena;
  u64 allocated_bytes = 0;
  u64 retained_bytes = 0;
  u64 reusable_bytes = 0;
  u64 returned_to_os_bytes = 0;
  u64 fragmentation_basis_points = 0;
  u64 slab_active_slots = 0;
  u64 slab_total_slots = 0;
  u64 slab_occupancy_basis_points = 0;
  u64 arena_waste_bytes = 0;
  u64 page_cache_frame_reuse_count = 0;
  u64 page_cache_frame_allocate_count = 0;
  u64 page_cache_frame_reuse_basis_points = 0;
  u64 temp_workspace_active_bytes = 0;
  u64 allocation_count = 0;
  u64 release_count = 0;
  u64 reset_count = 0;
  u64 move_count = 0;
  u64 teardown_count = 0;
  u64 allocation_latency_p95_ns = 0;
  u64 allocation_latency_p99_ns = 0;
  bool reset_order_observed = false;
  bool move_semantics_observed = false;
  bool leak_free_teardown_observed = false;
};

struct MemoryFragmentationProfilerSnapshot {
  std::vector<MemoryFragmentationProfileRow> rows;
  std::vector<MemoryFragmentationObjectClass> missing_object_classes;
  std::vector<MemoryFragmentationSupportBundleRow> support_bundle_rows;
  std::vector<scratchbird::core::metrics::MetricValue> metrics;
  std::vector<std::string> evidence;
  u64 allocated_bytes = 0;
  u64 retained_bytes = 0;
  u64 reusable_bytes = 0;
  u64 returned_to_os_bytes = 0;
  u64 fragmentation_basis_points = 0;
  u64 slab_occupancy_basis_points = 0;
  u64 arena_waste_bytes = 0;
  u64 page_cache_frame_reuse_basis_points = 0;
  u64 temp_workspace_active_bytes = 0;
  u64 allocation_count = 0;
  u64 allocation_latency_p95_ns = 0;
  u64 allocation_latency_p99_ns = 0;
  bool required_object_coverage_complete = false;
  bool reset_order_observed = false;
  bool move_semantics_observed = false;
  bool leak_free_teardown_observed = false;
  bool support_bundle_ready = false;
  bool metrics_ready = false;
};

struct MemoryFragmentationProfilerDiffRow {
  MemoryFragmentationProfileKey key;
  MemoryFragmentationObjectClass object_class =
      MemoryFragmentationObjectClass::plan_node;
  MemoryFragmentationSourceKind source_kind =
      MemoryFragmentationSourceKind::typed_arena;
  long long allocated_bytes_delta = 0;
  long long retained_bytes_delta = 0;
  long long reusable_bytes_delta = 0;
  long long returned_to_os_bytes_delta = 0;
  long long fragmentation_basis_points_delta = 0;
  long long slab_occupancy_basis_points_delta = 0;
  long long arena_waste_bytes_delta = 0;
  long long page_cache_frame_reuse_basis_points_delta = 0;
  long long temp_workspace_active_bytes_delta = 0;
  long long allocation_count_delta = 0;
  long long allocation_latency_p95_ns_delta = 0;
  long long allocation_latency_p99_ns_delta = 0;
};

struct MemoryFragmentationProfilerDiff {
  std::vector<MemoryFragmentationProfilerDiffRow> rows;
  std::vector<MemoryFragmentationSupportBundleRow> support_bundle_rows;
  std::vector<scratchbird::core::metrics::MetricValue> metrics;
  std::vector<std::string> evidence;
  u64 grouped_row_count = 0;
  bool grouped_by_database_tenant_user_session_transaction_statement_query_operator_category_class_callsite = false;
};

const char* MemoryFragmentationObjectClassName(
    MemoryFragmentationObjectClass object_class);
const char* MemoryFragmentationSourceKindName(
    MemoryFragmentationSourceKind source_kind);
std::vector<MemoryFragmentationObjectClass>
RequiredMemoryFragmentationObjectClasses();
MemoryFragmentationObjectClass MemoryFragmentationObjectClassFromTypedSlabKind(
    TypedSlabPoolObjectKind kind);
const char* MemoryFragmentationAuthorityScope();

class MemoryFragmentationProfiler {
 public:
  MemoryFragmentationProfiler() = default;
  MemoryFragmentationProfiler(const MemoryFragmentationProfiler&) = delete;
  MemoryFragmentationProfiler& operator=(const MemoryFragmentationProfiler&) =
      delete;
  MemoryFragmentationProfiler(MemoryFragmentationProfiler&&) noexcept = default;
  MemoryFragmentationProfiler& operator=(MemoryFragmentationProfiler&&) noexcept =
      default;

  void AddRecord(MemoryFragmentationProfileRecord record);
  void AddTypedSlabSnapshot(MemoryFragmentationProfileKey key,
                            const SizeClassPoolSnapshot& snapshot);
  void Reset();
  MemoryFragmentationProfilerSnapshot Snapshot() const;

  static MemoryFragmentationProfilerDiff Diff(
      const MemoryFragmentationProfilerSnapshot& before,
      const MemoryFragmentationProfilerSnapshot& after);

 private:
  std::vector<MemoryFragmentationProfileRecord> records_;
  u64 reset_count_ = 0;
};

}  // namespace scratchbird::core::memory

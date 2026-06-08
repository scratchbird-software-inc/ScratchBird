// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory_fragmentation_profiler.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::metrics::MetricLabel;
using scratchbird::core::metrics::MetricType;
using scratchbird::core::metrics::MetricUnit;
using scratchbird::core::metrics::MetricValue;

constexpr const char* kAnchor = "CEIC-028_FRAGMENTATION_PROFILER_DIFF";
constexpr const char* kAuthorityScope =
    "memory_fragmentation_profiler.authority_scope=evidence_only_not_transaction_finality_visibility_authorization_security_recovery_parser_donor_wal_benchmark_optimizer_plan_index_finality_cluster_or_agent_action_authority";

u64 BasisPoints(u64 numerator, u64 denominator) {
  if (denominator == 0 || numerator == 0) {
    return 0;
  }
  if (numerator > std::numeric_limits<u64>::max() / 10000ull) {
    return 10000;
  }
  return std::min<u64>(10000, (numerator * 10000ull) / denominator);
}

long long Delta(u64 before, u64 after) {
  if (after >= before) {
    return static_cast<long long>(after - before);
  }
  return -static_cast<long long>(before - after);
}

std::string EmptyAsDash(const std::string& value) {
  return value.empty() ? "-" : value;
}

using RowKey = std::tuple<std::string,
                          std::string,
                          std::string,
                          std::string,
                          std::string,
                          std::string,
                          std::string,
                          std::string,
                          std::string,
                          std::string,
                          std::string,
                          MemoryFragmentationObjectClass,
                          MemoryFragmentationSourceKind>;

RowKey MakeRowKey(const MemoryFragmentationProfileKey& key,
                  MemoryFragmentationObjectClass object_class,
                  MemoryFragmentationSourceKind source_kind) {
  return {key.database_id,
          key.tenant_id,
          key.user_id,
          key.session_id,
          key.transaction_id,
          key.statement_id,
          key.query_id,
          key.operator_id,
          key.category,
          key.memory_class,
          key.callsite,
          object_class,
          source_kind};
}

MemoryFragmentationProfileKey ProfileKeyFromRowKey(const RowKey& key) {
  MemoryFragmentationProfileKey result;
  result.database_id = std::get<0>(key);
  result.tenant_id = std::get<1>(key);
  result.user_id = std::get<2>(key);
  result.session_id = std::get<3>(key);
  result.transaction_id = std::get<4>(key);
  result.statement_id = std::get<5>(key);
  result.query_id = std::get<6>(key);
  result.operator_id = std::get<7>(key);
  result.category = std::get<8>(key);
  result.memory_class = std::get<9>(key);
  result.callsite = std::get<10>(key);
  return result;
}

void AddSupportRow(std::vector<MemoryFragmentationSupportBundleRow>* rows,
                   std::string key,
                   std::string value,
                   std::string redaction_class = "public") {
  MemoryFragmentationSupportBundleRow row;
  row.key = std::move(key);
  row.value = std::move(value);
  row.redaction_class = std::move(redaction_class);
  rows->push_back(std::move(row));
}

MetricValue GaugeMetric(const std::string& family,
                        double value,
                        const MemoryFragmentationProfileKey& key,
                        MemoryFragmentationObjectClass object_class,
                        MemoryFragmentationSourceKind source_kind,
                        MetricUnit unit = MetricUnit::bytes) {
  MetricValue metric;
  metric.family = family;
  metric.type = MetricType::gauge;
  metric.value = value;
  metric.labels = {
      MetricLabel{"database", EmptyAsDash(key.database_id)},
      MetricLabel{"tenant", EmptyAsDash(key.tenant_id)},
      MetricLabel{"user", EmptyAsDash(key.user_id)},
      MetricLabel{"session", EmptyAsDash(key.session_id)},
      MetricLabel{"transaction", EmptyAsDash(key.transaction_id)},
      MetricLabel{"statement", EmptyAsDash(key.statement_id)},
      MetricLabel{"query", EmptyAsDash(key.query_id)},
      MetricLabel{"operator", EmptyAsDash(key.operator_id)},
      MetricLabel{"category", EmptyAsDash(key.category)},
      MetricLabel{"memory_class", EmptyAsDash(key.memory_class)},
      MetricLabel{"callsite", EmptyAsDash(key.callsite)},
      MetricLabel{"object_class",
                  MemoryFragmentationObjectClassName(object_class)},
      MetricLabel{"source_kind", MemoryFragmentationSourceKindName(source_kind)},
      MetricLabel{"unit", scratchbird::core::metrics::MetricUnitName(unit)}};
  return metric;
}

void RecomputeDerived(MemoryFragmentationProfileRow* row) {
  row->fragmentation_basis_points =
      BasisPoints(row->reusable_bytes + row->arena_waste_bytes,
                  row->retained_bytes + row->allocated_bytes);
  row->slab_occupancy_basis_points =
      BasisPoints(row->slab_active_slots, row->slab_total_slots);
  row->page_cache_frame_reuse_basis_points = BasisPoints(
      row->page_cache_frame_reuse_count,
      row->page_cache_frame_reuse_count +
          row->page_cache_frame_allocate_count);
}

MemoryFragmentationProfileRow RowFromRecord(
    const MemoryFragmentationProfileRecord& record) {
  MemoryFragmentationProfileRow row;
  row.key = record.key;
  row.object_class = record.object_class;
  row.source_kind = record.source_kind;
  row.allocated_bytes = record.allocated_bytes;
  row.retained_bytes = record.retained_bytes;
  row.reusable_bytes = record.reusable_bytes;
  row.returned_to_os_bytes = record.returned_to_os_bytes;
  row.slab_active_slots = record.slab_active_slots;
  row.slab_total_slots = record.slab_total_slots;
  row.arena_waste_bytes = record.arena_waste_bytes;
  row.page_cache_frame_reuse_count = record.page_cache_frame_reuse_count;
  row.page_cache_frame_allocate_count =
      record.page_cache_frame_allocate_count;
  row.temp_workspace_active_bytes = record.temp_workspace_active_bytes;
  row.allocation_count = record.allocation_count;
  row.release_count = record.release_count;
  row.reset_count = record.reset_count;
  row.move_count = record.move_count;
  row.teardown_count = record.teardown_count;
  row.allocation_latency_p95_ns = record.allocation_latency_p95_ns;
  row.allocation_latency_p99_ns = record.allocation_latency_p99_ns;
  row.reset_order_observed = record.reset_order_observed;
  row.move_semantics_observed = record.move_semantics_observed;
  row.leak_free_teardown_observed = record.leak_free_teardown_observed;
  RecomputeDerived(&row);
  return row;
}

void AccumulateRow(MemoryFragmentationProfileRow* target,
                   const MemoryFragmentationProfileRow& row) {
  target->allocated_bytes += row.allocated_bytes;
  target->retained_bytes += row.retained_bytes;
  target->reusable_bytes += row.reusable_bytes;
  target->returned_to_os_bytes += row.returned_to_os_bytes;
  target->slab_active_slots += row.slab_active_slots;
  target->slab_total_slots += row.slab_total_slots;
  target->arena_waste_bytes += row.arena_waste_bytes;
  target->page_cache_frame_reuse_count += row.page_cache_frame_reuse_count;
  target->page_cache_frame_allocate_count +=
      row.page_cache_frame_allocate_count;
  target->temp_workspace_active_bytes += row.temp_workspace_active_bytes;
  target->allocation_count += row.allocation_count;
  target->release_count += row.release_count;
  target->reset_count += row.reset_count;
  target->move_count += row.move_count;
  target->teardown_count += row.teardown_count;
  target->allocation_latency_p95_ns =
      std::max(target->allocation_latency_p95_ns, row.allocation_latency_p95_ns);
  target->allocation_latency_p99_ns =
      std::max(target->allocation_latency_p99_ns, row.allocation_latency_p99_ns);
  target->reset_order_observed =
      target->reset_order_observed || row.reset_order_observed;
  target->move_semantics_observed =
      target->move_semantics_observed || row.move_semantics_observed;
  target->leak_free_teardown_observed =
      target->leak_free_teardown_observed || row.leak_free_teardown_observed;
  RecomputeDerived(target);
}

void AddRowMetrics(MemoryFragmentationProfilerSnapshot* snapshot,
                   const MemoryFragmentationProfileRow& row) {
  snapshot->metrics.push_back(
      GaugeMetric("memory_fragmentation_allocated_bytes",
                  static_cast<double>(row.allocated_bytes),
                  row.key,
                  row.object_class,
                  row.source_kind));
  snapshot->metrics.push_back(
      GaugeMetric("memory_fragmentation_retained_bytes",
                  static_cast<double>(row.retained_bytes),
                  row.key,
                  row.object_class,
                  row.source_kind));
  snapshot->metrics.push_back(
      GaugeMetric("memory_fragmentation_reusable_bytes",
                  static_cast<double>(row.reusable_bytes),
                  row.key,
                  row.object_class,
                  row.source_kind));
  snapshot->metrics.push_back(
      GaugeMetric("memory_fragmentation_returned_to_os_bytes",
                  static_cast<double>(row.returned_to_os_bytes),
                  row.key,
                  row.object_class,
                  row.source_kind));
  snapshot->metrics.push_back(
      GaugeMetric("memory_fragmentation_ratio_basis_points",
                  static_cast<double>(row.fragmentation_basis_points),
                  row.key,
                  row.object_class,
                  row.source_kind,
                  MetricUnit::ratio));
  snapshot->metrics.push_back(
      GaugeMetric("memory_slab_occupancy_basis_points",
                  static_cast<double>(row.slab_occupancy_basis_points),
                  row.key,
                  row.object_class,
                  row.source_kind,
                  MetricUnit::ratio));
  snapshot->metrics.push_back(
      GaugeMetric("memory_arena_waste_bytes",
                  static_cast<double>(row.arena_waste_bytes),
                  row.key,
                  row.object_class,
                  row.source_kind));
  snapshot->metrics.push_back(
      GaugeMetric("memory_page_cache_frame_reuse_basis_points",
                  static_cast<double>(row.page_cache_frame_reuse_basis_points),
                  row.key,
                  row.object_class,
                  row.source_kind,
                  MetricUnit::ratio));
  snapshot->metrics.push_back(
      GaugeMetric("memory_temp_workspace_active_bytes",
                  static_cast<double>(row.temp_workspace_active_bytes),
                  row.key,
                  row.object_class,
                  row.source_kind));
}

void AddSnapshotSupportRows(MemoryFragmentationProfilerSnapshot* snapshot) {
  AddSupportRow(&snapshot->support_bundle_rows,
                "memory_fragmentation.authority_scope",
                kAuthorityScope);
  AddSupportRow(&snapshot->support_bundle_rows,
                "memory_fragmentation.allocated_bytes",
                std::to_string(snapshot->allocated_bytes));
  AddSupportRow(&snapshot->support_bundle_rows,
                "memory_fragmentation.retained_bytes",
                std::to_string(snapshot->retained_bytes));
  AddSupportRow(&snapshot->support_bundle_rows,
                "memory_fragmentation.reusable_bytes",
                std::to_string(snapshot->reusable_bytes));
  AddSupportRow(&snapshot->support_bundle_rows,
                "memory_fragmentation.returned_to_os_bytes",
                std::to_string(snapshot->returned_to_os_bytes));
  AddSupportRow(&snapshot->support_bundle_rows,
                "memory_fragmentation.fragmentation_basis_points",
                std::to_string(snapshot->fragmentation_basis_points));
  AddSupportRow(&snapshot->support_bundle_rows,
                "memory_fragmentation.slab_occupancy_basis_points",
                std::to_string(snapshot->slab_occupancy_basis_points));
  AddSupportRow(&snapshot->support_bundle_rows,
                "memory_fragmentation.arena_waste_bytes",
                std::to_string(snapshot->arena_waste_bytes));
  AddSupportRow(&snapshot->support_bundle_rows,
                "memory_fragmentation.page_cache_frame_reuse_basis_points",
                std::to_string(snapshot->page_cache_frame_reuse_basis_points));
  AddSupportRow(&snapshot->support_bundle_rows,
                "memory_fragmentation.temp_workspace_active_bytes",
                std::to_string(snapshot->temp_workspace_active_bytes));
}

}  // namespace

const char* MemoryFragmentationObjectClassName(
    MemoryFragmentationObjectClass object_class) {
  switch (object_class) {
    case MemoryFragmentationObjectClass::plan_node: return "plan_node";
    case MemoryFragmentationObjectClass::expression_node:
      return "expression_node";
    case MemoryFragmentationObjectClass::predicate_node:
      return "predicate_node";
    case MemoryFragmentationObjectClass::row_locator: return "row_locator";
    case MemoryFragmentationObjectClass::candidate_set: return "candidate_set";
    case MemoryFragmentationObjectClass::posting_list: return "posting_list";
    case MemoryFragmentationObjectClass::hash_bucket: return "hash_bucket";
    case MemoryFragmentationObjectClass::sort_descriptor:
      return "sort_descriptor";
    case MemoryFragmentationObjectClass::vector_scratch:
      return "vector_scratch";
    case MemoryFragmentationObjectClass::diagnostic_record:
      return "diagnostic_record";
    case MemoryFragmentationObjectClass::metric_label: return "metric_label";
    case MemoryFragmentationObjectClass::page_cache_metadata:
      return "page_cache_metadata";
  }
  return "unknown";
}

const char* MemoryFragmentationSourceKindName(
    MemoryFragmentationSourceKind source_kind) {
  switch (source_kind) {
    case MemoryFragmentationSourceKind::typed_slab_pool:
      return "typed_slab_pool";
    case MemoryFragmentationSourceKind::typed_arena: return "typed_arena";
    case MemoryFragmentationSourceKind::page_cache_frame_pool:
      return "page_cache_frame_pool";
    case MemoryFragmentationSourceKind::temp_workspace:
      return "temp_workspace";
    case MemoryFragmentationSourceKind::support_bundle:
      return "support_bundle";
  }
  return "unknown";
}

std::vector<MemoryFragmentationObjectClass>
RequiredMemoryFragmentationObjectClasses() {
  return {MemoryFragmentationObjectClass::plan_node,
          MemoryFragmentationObjectClass::expression_node,
          MemoryFragmentationObjectClass::predicate_node,
          MemoryFragmentationObjectClass::row_locator,
          MemoryFragmentationObjectClass::candidate_set,
          MemoryFragmentationObjectClass::posting_list,
          MemoryFragmentationObjectClass::hash_bucket,
          MemoryFragmentationObjectClass::sort_descriptor,
          MemoryFragmentationObjectClass::vector_scratch,
          MemoryFragmentationObjectClass::diagnostic_record,
          MemoryFragmentationObjectClass::metric_label,
          MemoryFragmentationObjectClass::page_cache_metadata};
}

MemoryFragmentationObjectClass MemoryFragmentationObjectClassFromTypedSlabKind(
    TypedSlabPoolObjectKind kind) {
  switch (kind) {
    case TypedSlabPoolObjectKind::planner_node:
    case TypedSlabPoolObjectKind::plan_node:
      return MemoryFragmentationObjectClass::plan_node;
    case TypedSlabPoolObjectKind::expression_node:
      return MemoryFragmentationObjectClass::expression_node;
    case TypedSlabPoolObjectKind::predicate_node:
      return MemoryFragmentationObjectClass::predicate_node;
    case TypedSlabPoolObjectKind::row_locator:
      return MemoryFragmentationObjectClass::row_locator;
    case TypedSlabPoolObjectKind::candidate_chunk:
    case TypedSlabPoolObjectKind::candidate_set:
      return MemoryFragmentationObjectClass::candidate_set;
    case TypedSlabPoolObjectKind::posting_list_chunk:
      return MemoryFragmentationObjectClass::posting_list;
    case TypedSlabPoolObjectKind::hash_bucket:
      return MemoryFragmentationObjectClass::hash_bucket;
    case TypedSlabPoolObjectKind::sort_descriptor:
      return MemoryFragmentationObjectClass::sort_descriptor;
    case TypedSlabPoolObjectKind::vector_scratch:
      return MemoryFragmentationObjectClass::vector_scratch;
    case TypedSlabPoolObjectKind::diagnostic_record:
      return MemoryFragmentationObjectClass::diagnostic_record;
    case TypedSlabPoolObjectKind::metric_label:
      return MemoryFragmentationObjectClass::metric_label;
    case TypedSlabPoolObjectKind::page_cache_metadata:
      return MemoryFragmentationObjectClass::page_cache_metadata;
    case TypedSlabPoolObjectKind::executor_frame:
    case TypedSlabPoolObjectKind::row_batch:
    case TypedSlabPoolObjectKind::index_cursor:
      return MemoryFragmentationObjectClass::row_locator;
  }
  return MemoryFragmentationObjectClass::plan_node;
}

const char* MemoryFragmentationAuthorityScope() {
  return kAuthorityScope;
}

void MemoryFragmentationProfiler::AddRecord(
    MemoryFragmentationProfileRecord record) {
  record.evidence.push_back(kAnchor);
  record.evidence.push_back(kAuthorityScope);
  record.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.transaction_finality=true");
  record.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.visibility=true");
  record.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.authorization_security=true");
  record.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.recovery=true");
  record.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.parser_donor_wal=true");
  record.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.benchmark_optimizer_index_cluster_agent=true");
  records_.push_back(std::move(record));
}

void MemoryFragmentationProfiler::AddTypedSlabSnapshot(
    MemoryFragmentationProfileKey key,
    const SizeClassPoolSnapshot& snapshot) {
  MemoryFragmentationProfileRecord record;
  record.key = std::move(key);
  if (record.key.category.empty()) {
    record.key.category = MemoryCategoryName(snapshot.category);
  }
  if (record.key.memory_class.empty()) {
    record.key.memory_class = snapshot.memory_class;
  }
  record.object_class =
      MemoryFragmentationObjectClassFromTypedSlabKind(snapshot.object_kind);
  record.source_kind = MemoryFragmentationSourceKind::typed_slab_pool;
  record.allocated_bytes = snapshot.active_requested_bytes;
  record.retained_bytes = snapshot.retained_bytes;
  record.reusable_bytes = snapshot.reusable_payload_bytes;
  record.slab_active_slots = snapshot.active_slots;
  record.slab_total_slots = snapshot.total_slots;
  record.allocation_count = snapshot.allocation_count;
  record.release_count = snapshot.release_count;
  record.reset_count = snapshot.reset_count;
  record.allocation_latency_p95_ns = snapshot.allocation_latency_p95_ns;
  record.allocation_latency_p99_ns = snapshot.allocation_latency_p99_ns;
  record.reset_order_observed = snapshot.reset_count != 0;
  record.leak_free_teardown_observed =
      !snapshot.active || snapshot.active_slots == 0;
  record.evidence.push_back("ceic_013_typed_slab_snapshot_ingested=true");
  AddRecord(std::move(record));
}

void MemoryFragmentationProfiler::Reset() {
  records_.clear();
  ++reset_count_;
}

MemoryFragmentationProfilerSnapshot MemoryFragmentationProfiler::Snapshot()
    const {
  MemoryFragmentationProfilerSnapshot snapshot;
  snapshot.evidence.push_back(kAnchor);
  snapshot.evidence.push_back(kAuthorityScope);
  snapshot.evidence.push_back("CEIC-010_SHARDED_ACCOUNTING_TIE=true");
  snapshot.evidence.push_back("CEIC-013_TYPED_SLAB_POOLS_TIE=true");
  snapshot.evidence.push_back("CEIC-023_SUPPORT_BUNDLE_TIE=true");
  snapshot.evidence.push_back("memory_fragmentation_profiler.reset_count=" +
                              std::to_string(reset_count_));
  snapshot.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.transaction_finality=true");
  snapshot.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.visibility=true");
  snapshot.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.authorization_security=true");
  snapshot.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.recovery=true");
  snapshot.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.parser_donor_wal=true");
  snapshot.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.benchmark_optimizer_index_cluster_agent=true");

  std::map<RowKey, MemoryFragmentationProfileRow> grouped;
  std::set<MemoryFragmentationObjectClass> observed_classes;
  for (const auto& record : records_) {
    auto row = RowFromRecord(record);
    observed_classes.insert(row.object_class);
    const auto key = MakeRowKey(row.key, row.object_class, row.source_kind);
    auto position = grouped.find(key);
    if (position == grouped.end()) {
      grouped.emplace(key, std::move(row));
    } else {
      AccumulateRow(&position->second, row);
    }
  }

  snapshot.rows.reserve(grouped.size());
  for (const auto& entry : grouped) {
    snapshot.rows.push_back(entry.second);
  }

  for (const auto& row : snapshot.rows) {
    snapshot.allocated_bytes += row.allocated_bytes;
    snapshot.retained_bytes += row.retained_bytes;
    snapshot.reusable_bytes += row.reusable_bytes;
    snapshot.returned_to_os_bytes += row.returned_to_os_bytes;
    snapshot.arena_waste_bytes += row.arena_waste_bytes;
    snapshot.temp_workspace_active_bytes += row.temp_workspace_active_bytes;
    snapshot.allocation_count += row.allocation_count;
    snapshot.allocation_latency_p95_ns =
        std::max(snapshot.allocation_latency_p95_ns,
                 row.allocation_latency_p95_ns);
    snapshot.allocation_latency_p99_ns =
        std::max(snapshot.allocation_latency_p99_ns,
                 row.allocation_latency_p99_ns);
    snapshot.reset_order_observed =
        snapshot.reset_order_observed || row.reset_order_observed;
    snapshot.move_semantics_observed =
        snapshot.move_semantics_observed || row.move_semantics_observed;
    snapshot.leak_free_teardown_observed =
        snapshot.leak_free_teardown_observed || row.leak_free_teardown_observed;
    AddRowMetrics(&snapshot, row);
  }

  u64 total_slab_active_slots = 0;
  u64 total_slab_slots = 0;
  u64 total_page_cache_reuse = 0;
  u64 total_page_cache_allocations = 0;
  for (const auto& row : snapshot.rows) {
    total_slab_active_slots += row.slab_active_slots;
    total_slab_slots += row.slab_total_slots;
    total_page_cache_reuse += row.page_cache_frame_reuse_count;
    total_page_cache_allocations += row.page_cache_frame_allocate_count;
  }
  snapshot.fragmentation_basis_points =
      BasisPoints(snapshot.reusable_bytes + snapshot.arena_waste_bytes,
                  snapshot.retained_bytes + snapshot.allocated_bytes);
  snapshot.slab_occupancy_basis_points =
      BasisPoints(total_slab_active_slots, total_slab_slots);
  snapshot.page_cache_frame_reuse_basis_points = BasisPoints(
      total_page_cache_reuse,
      total_page_cache_reuse + total_page_cache_allocations);

  for (auto object_class : RequiredMemoryFragmentationObjectClasses()) {
    if (observed_classes.find(object_class) == observed_classes.end()) {
      snapshot.missing_object_classes.push_back(object_class);
    }
  }
  snapshot.required_object_coverage_complete =
      snapshot.missing_object_classes.empty();
  AddSnapshotSupportRows(&snapshot);
  snapshot.support_bundle_ready = true;
  snapshot.metrics_ready = !snapshot.metrics.empty();
  return snapshot;
}

MemoryFragmentationProfilerDiff MemoryFragmentationProfiler::Diff(
    const MemoryFragmentationProfilerSnapshot& before,
    const MemoryFragmentationProfilerSnapshot& after) {
  MemoryFragmentationProfilerDiff diff;
  diff.evidence.push_back(kAnchor);
  diff.evidence.push_back(kAuthorityScope);
  diff.evidence.push_back(
      "memory_fragmentation_profiler.diff_mode=before_after");
  diff.evidence.push_back(
      "memory_fragmentation_profiler.diff_grouping=database,tenant,user,session,transaction,statement,query,operator,category,memory_class,callsite");
  diff.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.transaction_finality=true");
  diff.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.visibility=true");
  diff.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.authorization_security=true");
  diff.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.recovery=true");
  diff.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.parser_donor_wal=true");
  diff.evidence.push_back(
      "memory_fragmentation_profiler.no_authority.benchmark_optimizer_index_cluster_agent=true");

  std::map<RowKey, MemoryFragmentationProfileRow> before_rows;
  std::map<RowKey, MemoryFragmentationProfileRow> after_rows;
  std::set<RowKey> keys;
  for (const auto& row : before.rows) {
    const auto key = MakeRowKey(row.key, row.object_class, row.source_kind);
    before_rows[key] = row;
    keys.insert(key);
  }
  for (const auto& row : after.rows) {
    const auto key = MakeRowKey(row.key, row.object_class, row.source_kind);
    after_rows[key] = row;
    keys.insert(key);
  }

  diff.rows.reserve(keys.size());
  for (const auto& key : keys) {
    const auto before_it = before_rows.find(key);
    const auto after_it = after_rows.find(key);
    MemoryFragmentationProfileRow empty;
    empty.key = ProfileKeyFromRowKey(key);
    empty.object_class = std::get<11>(key);
    empty.source_kind = std::get<12>(key);
    const auto& left = before_it == before_rows.end() ? empty : before_it->second;
    const auto& right = after_it == after_rows.end() ? empty : after_it->second;

    MemoryFragmentationProfilerDiffRow row;
    row.key = ProfileKeyFromRowKey(key);
    row.object_class = std::get<11>(key);
    row.source_kind = std::get<12>(key);
    row.allocated_bytes_delta =
        Delta(left.allocated_bytes, right.allocated_bytes);
    row.retained_bytes_delta = Delta(left.retained_bytes, right.retained_bytes);
    row.reusable_bytes_delta = Delta(left.reusable_bytes, right.reusable_bytes);
    row.returned_to_os_bytes_delta =
        Delta(left.returned_to_os_bytes, right.returned_to_os_bytes);
    row.fragmentation_basis_points_delta =
        Delta(left.fragmentation_basis_points,
              right.fragmentation_basis_points);
    row.slab_occupancy_basis_points_delta =
        Delta(left.slab_occupancy_basis_points,
              right.slab_occupancy_basis_points);
    row.arena_waste_bytes_delta =
        Delta(left.arena_waste_bytes, right.arena_waste_bytes);
    row.page_cache_frame_reuse_basis_points_delta =
        Delta(left.page_cache_frame_reuse_basis_points,
              right.page_cache_frame_reuse_basis_points);
    row.temp_workspace_active_bytes_delta =
        Delta(left.temp_workspace_active_bytes,
              right.temp_workspace_active_bytes);
    row.allocation_count_delta =
        Delta(left.allocation_count, right.allocation_count);
    row.allocation_latency_p95_ns_delta =
        Delta(left.allocation_latency_p95_ns, right.allocation_latency_p95_ns);
    row.allocation_latency_p99_ns_delta =
        Delta(left.allocation_latency_p99_ns, right.allocation_latency_p99_ns);
    diff.metrics.push_back(
        GaugeMetric("memory_fragmentation_allocated_bytes_delta",
                    static_cast<double>(row.allocated_bytes_delta),
                    row.key,
                    row.object_class,
                    row.source_kind));
    diff.rows.push_back(std::move(row));
  }

  diff.grouped_row_count = static_cast<u64>(diff.rows.size());
  diff.grouped_by_database_tenant_user_session_transaction_statement_query_operator_category_class_callsite =
      true;
  AddSupportRow(&diff.support_bundle_rows,
                "memory_fragmentation.diff.authority_scope",
                kAuthorityScope);
  AddSupportRow(&diff.support_bundle_rows,
                "memory_fragmentation.diff.grouped_row_count",
                std::to_string(diff.grouped_row_count));
  AddSupportRow(&diff.support_bundle_rows,
                "memory_fragmentation.diff.grouping",
                "database,tenant,user,session,transaction,statement,query,operator,category,memory_class,callsite");
  return diff;
}

}  // namespace scratchbird::core::memory

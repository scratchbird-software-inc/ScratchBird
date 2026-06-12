// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-028 focused validation for fragmentation profiler snapshots and diffs.
#include "hierarchical_memory_budget_ledger.hpp"
#include "memory_fragmentation_profiler.hpp"
#include "memory_support_bundle.hpp"
#include "typed_slab_pool.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace memory = scratchbird::core::memory;
using scratchbird::core::platform::u64;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& row : evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool SupportRowsHave(
    const std::vector<memory::MemoryFragmentationSupportBundleRow>& rows,
    std::string_view key,
    std::string_view value) {
  for (const auto& row : rows) {
    if (row.key.find(key) != std::string::npos &&
        row.value.find(value) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool BundleRowsHave(const std::vector<memory::MemorySupportBundleRow>& rows,
                    std::string_view key,
                    std::string_view value) {
  for (const auto& row : rows) {
    if (row.key.find(key) != std::string::npos &&
        row.value.find(value) != std::string::npos) {
      return true;
    }
  }
  return false;
}

memory::HierarchicalMemoryBudgetProvenance Provenance() {
  memory::HierarchicalMemoryBudgetProvenance provenance;
  provenance.source =
      memory::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  provenance.source_label = "ceic_028_fragmentation_profiler_policy";
  provenance.engine_mga_authoritative = true;
  provenance.memory_evidence_only = true;
  return provenance;
}

void SetBudget(memory::HierarchicalMemoryBudgetLedger* ledger,
               memory::HierarchicalMemoryScopeKind kind,
               const std::string& scope_id,
               u64 hard_limit) {
  memory::HierarchicalMemoryBudget budget;
  budget.scope = {kind, scope_id};
  budget.hard_limit_bytes = hard_limit;
  budget.provenance = Provenance();
  Require(ledger->SetBudget(std::move(budget)).ok(),
          "CEIC-028 budget setup failed");
}

std::vector<memory::HierarchicalMemoryScopeRef> ScopeChain(
    std::string leaf_id) {
  return {{memory::HierarchicalMemoryScopeKind::process, "ceic-028-process"},
          {memory::HierarchicalMemoryScopeKind::database, "ceic-028-db"},
          {memory::HierarchicalMemoryScopeKind::tenant, "ceic-028-tenant"},
          {memory::HierarchicalMemoryScopeKind::user, "ceic-028-user"},
          {memory::HierarchicalMemoryScopeKind::session, "ceic-028-session"},
          {memory::HierarchicalMemoryScopeKind::transaction,
           "ceic-028-transaction"},
          {memory::HierarchicalMemoryScopeKind::statement,
           "ceic-028-statement"},
          {memory::HierarchicalMemoryScopeKind::query, "ceic-028-query"},
          {memory::HierarchicalMemoryScopeKind::operator_scope,
           std::move(leaf_id)}};
}

memory::MemoryFragmentationProfileKey ProfileKey(std::string suffix) {
  memory::MemoryFragmentationProfileKey key;
  key.database_id = "ceic-028-db";
  key.tenant_id = "ceic-028-tenant";
  key.user_id = "ceic-028-user";
  key.session_id = "ceic-028-session";
  key.transaction_id = "ceic-028-transaction";
  key.statement_id = "ceic-028-statement";
  key.query_id = "ceic-028-query";
  key.operator_id = "ceic-028-operator-" + suffix;
  key.category = "executor_query_reserved";
  key.memory_class = "ceic_028." + suffix;
  key.callsite = "ceic_028.callsite." + suffix;
  return key;
}

memory::MemoryFragmentationProfileRecord RecordFor(
    memory::MemoryFragmentationObjectClass object_class,
    memory::MemoryFragmentationSourceKind source_kind,
    int index,
    bool after) {
  const std::string suffix =
      memory::MemoryFragmentationObjectClassName(object_class);
  memory::MemoryFragmentationProfileRecord record;
  record.key = ProfileKey(suffix);
  record.object_class = object_class;
  record.source_kind = source_kind;
  record.allocated_bytes = static_cast<u64>(128 + index);
  record.retained_bytes = static_cast<u64>((after ? 192 : 256) + index);
  record.reusable_bytes = static_cast<u64>((after ? 24 : 64) + index);
  record.returned_to_os_bytes = after ? static_cast<u64>(64 + index) : 0;
  record.slab_active_slots = static_cast<u64>(1 + (index % 3));
  record.slab_total_slots = 8;
  record.arena_waste_bytes = after ? 8 : 32;
  record.page_cache_frame_reuse_count =
      object_class == memory::MemoryFragmentationObjectClass::page_cache_metadata
          ? 12
          : 0;
  record.page_cache_frame_allocate_count =
      object_class == memory::MemoryFragmentationObjectClass::page_cache_metadata
          ? 3
          : 0;
  record.temp_workspace_active_bytes =
      object_class == memory::MemoryFragmentationObjectClass::sort_descriptor
          ? 4096
          : 0;
  record.allocation_count = static_cast<u64>(10 + index);
  record.release_count = after ? static_cast<u64>(4 + index) : 0;
  record.reset_count = after ? 1 : 0;
  record.move_count = 1;
  record.teardown_count = after ? 1 : 0;
  record.allocation_latency_p95_ns = static_cast<u64>(900 + index);
  record.allocation_latency_p99_ns = static_cast<u64>(1200 + index);
  record.reset_order_observed = after;
  record.move_semantics_observed = true;
  record.leak_free_teardown_observed = after;
  record.evidence.push_back("ceic_028_record_fixture_memory_only=true");
  return record;
}

std::unique_ptr<memory::SizeClassAllocator> MakeExpandedTypedSlabPool(
    memory::HierarchicalMemoryBudgetLedger* ledger,
    memory::MemoryManager* manager) {
  SetBudget(ledger,
            memory::HierarchicalMemoryScopeKind::operator_scope,
            "ceic-028-expression-pool",
            32768);

  memory::SizeClassAllocatorRequest request;
  request.reservation_ledger = ledger;
  request.memory_manager = manager;
  request.scope_chain = ScopeChain("ceic-028-expression-pool");
  request.object_kind = memory::TypedSlabPoolObjectKind::expression_node;
  request.category = memory::TypedSlabPoolDefaultCategory(request.object_kind);
  request.memory_class = "ceic_028.expression_node";
  request.reservation_bytes = 16384;
  request.owner_id = "ceic-028-expression-owner";
  request.route_label = "ceic-028-expression-pool";
  request.operation_id = "ceic-028-expression-operation";
  request.purpose = "ceic-028-expression-typed-slab";
  request.size_classes = {{64, 4}, {128, 4}, {256, 2}};
  request.provenance = Provenance();
  request.authority.engine_mga_authoritative = true;
  request.authority.transaction_inventory_authoritative = true;
  request.authority.security_or_policy_checked = true;
  auto acquired = memory::CreateSizeClassAllocator(std::move(request));
  Require(acquired.ok(), "CEIC-028 expanded typed slab pool acquisition failed");
  Require(EvidenceHas(acquired.evidence, "typed_slab_pool.reservation_first=true"),
          "CEIC-028 typed slab pool did not reserve first");
  return std::move(acquired.allocator);
}

std::vector<memory::MemoryFragmentationObjectClass> RequiredClasses() {
  return memory::RequiredMemoryFragmentationObjectClasses();
}

void ValidateProfilerCoverageAndDiff() {
  memory::MemoryFragmentationProfiler before;
  memory::MemoryFragmentationProfiler after;
  int index = 0;
  for (auto object_class : RequiredClasses()) {
    memory::MemoryFragmentationSourceKind source_kind =
        memory::MemoryFragmentationSourceKind::typed_arena;
    if (object_class ==
        memory::MemoryFragmentationObjectClass::page_cache_metadata) {
      source_kind =
          memory::MemoryFragmentationSourceKind::page_cache_frame_pool;
    } else if (object_class ==
               memory::MemoryFragmentationObjectClass::sort_descriptor) {
      source_kind = memory::MemoryFragmentationSourceKind::temp_workspace;
    } else if (object_class ==
               memory::MemoryFragmentationObjectClass::diagnostic_record) {
      source_kind = memory::MemoryFragmentationSourceKind::support_bundle;
    }
    before.AddRecord(RecordFor(object_class, source_kind, index, false));
    after.AddRecord(RecordFor(object_class, source_kind, index, true));
    ++index;
  }

  const auto before_snapshot = before.Snapshot();
  const auto after_snapshot = after.Snapshot();
  Require(before_snapshot.required_object_coverage_complete,
          "CEIC-028 before snapshot missing required object classes");
  Require(after_snapshot.required_object_coverage_complete,
          "CEIC-028 after snapshot missing required object classes");
  Require(after_snapshot.rows.size() == RequiredClasses().size(),
          "CEIC-028 snapshot did not group to one row per object class");
  Require(after_snapshot.allocated_bytes > 0 &&
              after_snapshot.retained_bytes > 0 &&
              after_snapshot.reusable_bytes > 0 &&
              after_snapshot.returned_to_os_bytes > 0,
          "CEIC-028 byte counters missing");
  Require(after_snapshot.fragmentation_basis_points > 0 &&
              after_snapshot.slab_occupancy_basis_points > 0 &&
              after_snapshot.arena_waste_bytes > 0 &&
              after_snapshot.page_cache_frame_reuse_basis_points > 0 &&
              after_snapshot.temp_workspace_active_bytes > 0,
          "CEIC-028 fragmentation occupancy page-cache or temp counters missing");
  Require(after_snapshot.allocation_count > 0 &&
              after_snapshot.allocation_latency_p95_ns > 0 &&
              after_snapshot.allocation_latency_p99_ns > 0,
          "CEIC-028 allocation count or percentile evidence missing");
  Require(after_snapshot.reset_order_observed &&
              after_snapshot.move_semantics_observed &&
              after_snapshot.leak_free_teardown_observed,
          "CEIC-028 reset move or teardown evidence missing");
  Require(EvidenceHas(after_snapshot.evidence, "CEIC-010_SHARDED_ACCOUNTING_TIE") &&
              EvidenceHas(after_snapshot.evidence,
                          "CEIC-013_TYPED_SLAB_POOLS_TIE") &&
              EvidenceHas(after_snapshot.evidence,
                          "CEIC-023_SUPPORT_BUNDLE_TIE"),
          "CEIC-028 integration anchors missing");
  Require(EvidenceHas(after_snapshot.evidence,
                      "no_authority.transaction_finality=true") &&
              EvidenceHas(after_snapshot.evidence,
                          "no_authority.visibility=true") &&
              EvidenceHas(after_snapshot.evidence,
                          "no_authority.authorization_security=true") &&
              EvidenceHas(after_snapshot.evidence,
                          "no_authority.recovery=true") &&
              EvidenceHas(after_snapshot.evidence,
                          "no_authority.parser_reference_wal=true"),
          "CEIC-028 no-authority markers missing");
  Require(after_snapshot.support_bundle_ready && after_snapshot.metrics_ready,
          "CEIC-028 support or metric rows not ready");
  Require(SupportRowsHave(after_snapshot.support_bundle_rows,
                          "memory_fragmentation.authority_scope",
                          "evidence_only"),
          "CEIC-028 support bundle authority row missing");

  const auto diff =
      memory::MemoryFragmentationProfiler::Diff(before_snapshot, after_snapshot);
  Require(diff.grouped_by_database_tenant_user_session_transaction_statement_query_operator_category_class_callsite,
          "CEIC-028 diff did not declare full grouping dimensions");
  Require(diff.rows.size() == RequiredClasses().size(),
          "CEIC-028 diff row count mismatch");
  Require(EvidenceHas(diff.evidence,
                      "diff_grouping=database,tenant,user,session,transaction,statement,query,operator,category,memory_class,callsite"),
          "CEIC-028 diff grouping evidence missing");
  bool saw_retained_drop = false;
  bool saw_returned_growth = false;
  bool saw_full_key = false;
  for (const auto& row : diff.rows) {
    saw_retained_drop = saw_retained_drop || row.retained_bytes_delta < 0;
    saw_returned_growth =
        saw_returned_growth || row.returned_to_os_bytes_delta > 0;
    saw_full_key = saw_full_key ||
                   (!row.key.database_id.empty() &&
                    !row.key.tenant_id.empty() &&
                    !row.key.user_id.empty() &&
                    !row.key.session_id.empty() &&
                    !row.key.transaction_id.empty() &&
                    !row.key.statement_id.empty() &&
                    !row.key.query_id.empty() &&
                    !row.key.operator_id.empty() &&
                    !row.key.category.empty() &&
                    !row.key.memory_class.empty() &&
                    !row.key.callsite.empty());
  }
  Require(saw_retained_drop && saw_returned_growth && saw_full_key,
          "CEIC-028 before/after diff did not prove deltas and full group key");

  memory::MemoryFragmentationProfiler movable;
  movable.AddRecord(RecordFor(memory::MemoryFragmentationObjectClass::metric_label,
                              memory::MemoryFragmentationSourceKind::support_bundle,
                              77,
                              true));
  memory::MemoryFragmentationProfiler moved(std::move(movable));
  const auto moved_snapshot = moved.Snapshot();
  Require(moved_snapshot.rows.size() == 1 &&
              moved_snapshot.move_semantics_observed,
          "CEIC-028 profiler move semantics were not preserved");
  moved.Reset();
  Require(moved.Snapshot().rows.empty(),
          "CEIC-028 profiler reset did not clear moved records");
}

void ValidateTypedSlabExpansionAndSupportBundleTie() {
  memory::AllocationPolicy policy;
  policy.policy_name = "ceic_028_fragmentation_profiler_diff_gate";
  policy.byte_limit = 4ull * 1024ull * 1024ull;
  policy.hard_limit_bytes = 4ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  memory::MemoryManager manager(policy);
  memory::HierarchicalMemoryBudgetLedger ledger;
  SetBudget(&ledger,
            memory::HierarchicalMemoryScopeKind::process,
            "ceic-028-process",
            4ull * 1024ull * 1024ull);

  auto pool = MakeExpandedTypedSlabPool(&ledger, &manager);
  auto first = pool->Allocate({48, 0, "expression-node-a"});
  auto second = pool->Allocate({80, 0, "expression-node-b"});
  Require(first.ok() && second.ok(), "CEIC-028 expanded typed slab allocation failed");
  Require(pool->Free(first.pointer).ok(), "CEIC-028 expanded typed slab free failed");
  auto reused = pool->Allocate({48, 0, "expression-node-reuse"});
  Require(reused.ok() && reused.reused,
          "CEIC-028 expanded typed slab reuse not observed");

  memory::MemoryFragmentationProfiler profiler;
  auto key = ProfileKey("expression_node");
  key.callsite = "ceic_028.typed_slab.expression_node";
  profiler.AddTypedSlabSnapshot(key, pool->Snapshot());
  const auto typed_snapshot = profiler.Snapshot();
  Require(!typed_snapshot.rows.empty(),
          "CEIC-028 typed slab snapshot was not ingested");
  Require(typed_snapshot.rows.front().object_class ==
              memory::MemoryFragmentationObjectClass::expression_node,
          "CEIC-028 typed slab object class mapping failed");
  Require(typed_snapshot.rows.front().allocation_count >= 3 &&
              typed_snapshot.rows.front().slab_occupancy_basis_points > 0 &&
              typed_snapshot.rows.front().fragmentation_basis_points > 0,
          "CEIC-028 typed slab metrics missing");

  memory::MemorySupportBundleRequest bundle_request;
  bundle_request.snapshot = manager.Snapshot();
  bundle_request.mode = memory::MemorySupportBundleMode::low_memory;
  bundle_request.limits.max_rows = 128;
  bundle_request.limits.max_output_bytes = 16ull * 1024ull;
  bundle_request.memory_fragmentation_rows.reserve(
      typed_snapshot.support_bundle_rows.size());
  for (const auto& row : typed_snapshot.support_bundle_rows) {
    memory::MemorySupportBundleRow bundle_row;
    bundle_row.key = row.key;
    bundle_row.value = row.value;
    bundle_row.redaction_class = row.redaction_class;
    bundle_row.redacted = row.redacted;
    bundle_request.memory_fragmentation_rows.push_back(std::move(bundle_row));
  }
  const auto bundle = memory::BuildMemorySupportBundleEvidence(bundle_request);
  Require(bundle.ok(), "CEIC-028 support bundle integration failed");
  Require(EvidenceHas(bundle.evidence, "CEIC-028_FRAGMENTATION_PROFILER_DIFF"),
          "CEIC-028 support bundle evidence anchor missing");
  Require(bundle.memory_fragmentation_row_count > 0,
          "CEIC-028 support bundle fragmentation rows missing");
  Require(BundleRowsHave(bundle.rows,
                         "memory_fragmentation",
                         "evidence_only"),
          "CEIC-028 support bundle no-authority row missing");

  Require(pool->Release().ok(), "CEIC-028 expanded typed slab release failed");
  const auto ledger_snapshot = ledger.Snapshot();
  Require(ledger_snapshot.current_bytes == 0,
          "CEIC-028 leaked CEIC-011 ledger bytes");
  Require(ledger_snapshot.active_allocation_count == 0,
          "CEIC-028 leaked CEIC-011 reservations");
  Require(manager.Snapshot().current_bytes == 0,
          "CEIC-028 leaked MemoryManager bytes");
}

void ValidateExpandedEnumNames() {
  std::set<std::string> names;
  for (auto kind : {memory::TypedSlabPoolObjectKind::plan_node,
                    memory::TypedSlabPoolObjectKind::expression_node,
                    memory::TypedSlabPoolObjectKind::predicate_node,
                    memory::TypedSlabPoolObjectKind::row_locator,
                    memory::TypedSlabPoolObjectKind::candidate_set,
                    memory::TypedSlabPoolObjectKind::posting_list_chunk,
                    memory::TypedSlabPoolObjectKind::hash_bucket,
                    memory::TypedSlabPoolObjectKind::sort_descriptor,
                    memory::TypedSlabPoolObjectKind::vector_scratch,
                    memory::TypedSlabPoolObjectKind::diagnostic_record,
                    memory::TypedSlabPoolObjectKind::metric_label,
                    memory::TypedSlabPoolObjectKind::page_cache_metadata}) {
    names.insert(memory::TypedSlabPoolObjectKindName(kind));
  }
  Require(names.size() == RequiredClasses().size(),
          "CEIC-028 expanded typed slab enum coverage mismatch");
  Require(memory::TypedSlabPoolObjectKindName(
              memory::TypedSlabPoolObjectKind::planner_node) ==
              std::string("planner_node"),
          "CEIC-028 broke CEIC-013 planner_node compatibility");
  Require(memory::TypedSlabPoolObjectKindName(
              memory::TypedSlabPoolObjectKind::candidate_chunk) ==
              std::string("candidate_chunk"),
          "CEIC-028 broke CEIC-013 candidate_chunk compatibility");
}

}  // namespace

int main() {
  ValidateExpandedEnumNames();
  ValidateProfilerCoverageAndDiff();
  ValidateTypedSlabExpansionAndSupportBundleTie();
  std::cout << "CEIC-028 fragmentation profiler diff gate passed\n";
  return 0;
}

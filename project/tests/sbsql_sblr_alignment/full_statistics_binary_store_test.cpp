// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/optimizer/optimizer_statistics_full.hpp"
#include "../../src/engine/optimizer/access_path_full.hpp"
#include "binary_uuid_fixture.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace o = scratchbird::engine::optimizer;
using Uuid = scratchbird::engine::planner::CanonicalPlannerUuid;
static_assert(sizeof(Uuid) == 16);
namespace {
unsigned checks = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
Uuid Id(unsigned suffix) {
  auto id = scratchbird::tests::BinaryUuid("019f0000-0000-7600-8000-000000000000");
  id.bytes[14] = suffix >> 8; id.bytes[15] = suffix;
  return id;
}
o::OptimizerStatsIdentity Identity(unsigned object, unsigned statistic) {
  return {Id(object), Id(statistic), 7, 8, 9,
      o::OptimizerStatsFreshnessState::kFresh,
      o::StatisticSource::kCatalogExact, o::CostConfidence::kExact};
}
void StoreAndCounts() {
  o::OptimizerStatisticsStore store;
  o::TableCardinalityStats table{Identity(1, 2), 0, 0, 0, 0};
  Check(o::OptimizerTableStatsAreUsable(table), "empty table metadata is usable");
  store.UpsertTable(table);
  auto view = store.ToLegacyCatalog();
  Check(view.has_value(), "empty table publishes complete scalar view");
  const auto target = o::OptimizerStatisticTarget::Object(Id(1));
  Check(view->TryEstimateUnsigned("row_count", target) == 0, "measured zero survives");
  for (auto count : {UINT64_C(9007199254740993), UINT64_MAX}) {
    table.row_count = count; table.visible_row_count = count;
    table.page_count = 10; table.average_row_bytes = 8;
    store.UpsertTable(table); view = store.ToLegacyCatalog();
    Check(view && view->TryEstimateUnsigned("row_count", target) == count,
          "full uint64 count survives scalar projection");
    Check(store.FindTable(Id(1))->row_count == count, "binary store exact count");
  }
  Check(!store.FindTable(Id(3)), "unrelated object cannot match");
  table.visible_row_count = 0;
  Check(o::OptimizerTableStatsAreUsable(table), "zero visible rows is not absent statistics");
  table.page_count = 0;
  Check(!o::OptimizerTableStatsAreUsable(table), "positive physical count requires storage metadata");
  auto snapshot = store.Snapshot(Id(4));
  Check(snapshot.snapshot_id == Id(4) && snapshot.tables.size() == 1,
        "snapshot preserves supplied binary identity and actual rows");
  store.MarkStaleByObject(Id(1), 10);
  Check(!o::OptimizerTableStatsAreUsable(*store.FindTable(Id(1))), "stale table not usable");
  view = store.ToLegacyCatalog();
  Check(view && !view->TryEstimateUnsigned("row_count", target), "stale projection is unavailable");
}
void CorrelationsAndIndexes() {
  o::OptimizerStatisticsStore store;
  o::ColumnStats column;
  column.identity = Identity(1, 2); column.column_uuid = Id(3);
  column.correlation = -0.75;
  store.UpsertColumn(column);
  auto view = store.ToLegacyCatalog();
  Check(view.has_value(), "negative correlation is valid data");
  auto row = view->Find("column_correlation", o::OptimizerStatisticTarget::Object(Id(3)));
  Check(row && row->value == -0.75, "signed correlation preserved");
  Check(!o::CheckedOptimizerStatisticUnsigned(*row), "correlation not a count");
  o::IndexStats index;
  index.identity = Identity(1, 5); index.index_uuid = Id(6); index.relation_uuid = Id(1);
  index.descriptor_digest = std::string(64, 'a'); index.route_benchmark_clean = true;
  Check(o::OptimizerIndexStatsAreUsable(index, Id(1), index.descriptor_digest),
        "zero index counts do not mean absent metadata");
  Check(!o::OptimizerIndexStatsAreUsable(index, Id(9), index.descriptor_digest),
        "index cannot cross relation binding");
  Check(!o::OptimizerIndexStatsAreUsable(index, Id(1), "changed"), "descriptor binding required");
  index.mga_recheck_required = false;
  Check(!o::OptimizerIndexStatsAreUsable(index, Id(1), index.descriptor_digest), "MGA recheck required");
}
void BinaryKeys() {
  o::OptimizerPinnedStatsDescriptorKey key;
  key.catalog_epoch = 1; key.security_epoch = 2; key.resource_policy_epoch = 3;
  key.name_resolution_epoch = 4; key.stats_epoch = 5;
  key.descriptor_set_digest = std::string(64, 'b');
  key.object_uuids = {Id(1), Id(2)}; key.index_uuids = {Id(3)};
  key.security_policy_identity = Id(4); key.redaction_policy_identity = Id(5);
  Check(o::ValidateOptimizerPinnedStatsDescriptorKey(key).ok, "valid binary pinned key");
  const auto original = o::OptimizerPinnedStatsDescriptorCacheKeyText(key);
  std::reverse(key.object_uuids.begin(), key.object_uuids.end());
  Check(o::OptimizerPinnedStatsDescriptorCacheKeyText(key) == original, "object set ordering canonical");
  for (unsigned bit = 0; bit < 128; ++bit) {
    auto changed = key;
    changed.security_policy_identity.bytes[bit / 8] ^= 1U << (bit % 8);
    Check(o::OptimizerPinnedStatsDescriptorCacheKeyText(changed) != original,
          "all 128 policy identity bits participate in key");
  }
  key.object_uuids.push_back(Id(1));
  Check(!o::ValidateOptimizerPinnedStatsDescriptorKey(key).ok, "duplicate objects refuse admission");
  key.object_uuids = {Uuid{}};
  Check(!o::ValidateOptimizerPinnedStatsDescriptorKey(key).ok, "nil object refuses admission");
}

void AccessBindings() {
  o::IndexStats index;
  index.identity = Identity(1, 2); index.index_uuid = Id(3); index.relation_uuid = Id(1);
  index.covering = true;
  Check(!o::IndexCanCoverProjection(index, {Id(4)}), "empty covering list is not universal coverage");
  index.covered_column_uuids = {Id(4), Id(5)};
  Check(o::IndexCanCoverProjection(index, {Id(5), Id(4)}), "exact binary covering set");
  Check(!o::IndexCanCoverProjection(index, {Id(6)}), "uncovered column refuses");
  Check(!o::IndexCanCoverProjection(index, {}), "missing projection refuses");
  index.covered_column_uuids.push_back(Uuid{});
  Check(!o::IndexCanCoverProjection(index, {Uuid{}}), "nil cannot supply covering proof");
  index.key_column_uuids = {Id(4), Id(5)};
  index.ordered_range_supported = true; index.equality_lookup_supported = true;
  Check(o::IndexCanSatisfyPredicate(index, "scalar_eq"), "empty index still supports equality");
  Check(o::IndexCanSatisfyPredicate(index, "scalar_range"), "empty index still supports range");
  o::OrderedLimitPlanningRequest ordered{true, {Id(4)}, 0};
  Check(o::IndexCanSatisfyOrdering(index, ordered), "zero limit with exact binary ordering");
  ordered.order_by_column_uuids = {Id(5)};
  Check(!o::IndexCanSatisfyOrdering(index, ordered), "ordering must be leading key prefix");
  index.covered_column_uuids = {Id(4)};
  for (unsigned bit = 0; bit < 128; ++bit) {
    auto different = Id(4); different.bytes[bit / 8] ^= 1U << (bit % 8);
    Check(!o::IndexCanCoverProjection(index, {different}), "all128 covering identity bits compared");
    ordered.order_by_column_uuids = {different};
    Check(!o::IndexCanSatisfyOrdering(index, ordered), "all128 ordering identity bits compared");
  }
  o::PlanCandidate candidate;
  candidate.candidate_id = "CAND-OPT-ORDERED-LIMIT";
  candidate.relation_uuid = Id(1); candidate.index_uuid = Id(3);
  candidate.access_kind = scratchbird::engine::planner::PhysicalAccessKind::kScalarBtreeRange;
  candidate.ordered_limit_evidence = {true, Id(3), {Id(4), Id(5)}, 0, true, true};
  auto physical = o::PhysicalPlanNodeFromCandidate(candidate, "scalar_btree_range", std::string(64, 'a'));
  Check(physical.relation_uuid == Id(1) && physical.index_uuid == Id(3), "physical node retains binary source");
  Check(physical.ordered_limit_evidence.index_uuid == Id(3) &&
        physical.ordered_limit_evidence.order_by_column_uuids == std::vector<Uuid>{Id(4), Id(5)},
        "physical node retains typed ordered-limit binding");
  Check(o::PhysicalPlanContainsCandidateBinding(physical, candidate), "actual candidate binding retained");
  for (unsigned bit = 0; bit < 128; ++bit) {
    auto changed = candidate;
    changed.index_uuid.bytes[bit / 8] ^= 1U << (bit % 8);
    Check(!o::PhysicalPlanContainsCandidateBinding(physical, changed), "shared label cannot mask changed binary index");
    changed = candidate; changed.relation_uuid.bytes[bit / 8] ^= 1U << (bit % 8);
    Check(!o::PhysicalPlanContainsCandidateBinding(physical, changed), "shared label cannot mask changed binary relation");
  }
  auto changed_limit = candidate; changed_limit.ordered_limit_evidence.limit_count = 1;
  Check(!o::PhysicalPlanContainsCandidateBinding(physical, changed_limit), "ordered bound is part of retained binding");
  auto parent = o::PhysicalPlanNode{}; parent.children.push_back(physical);
  Check(o::PhysicalPlanContainsCandidateBinding(parent, candidate), "retained binding found through real tree");
  candidate.index_uuid = Id(6);
  Check(physical.index_uuid == Id(3), "physical node owns independent source binding");
}
}
int main() {
  try {
    StoreAndCounts(); CorrelationsAndIndexes(); BinaryKeys(); AccessBindings();
    std::cout << "PASS full statistics binary store checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n'; return 1;
  }
}

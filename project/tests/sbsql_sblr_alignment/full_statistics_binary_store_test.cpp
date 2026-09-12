// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/optimizer/optimizer_statistics_full.hpp"
#include "../../src/engine/optimizer/access_path_full.hpp"
#include "../../src/engine/optimizer/selectivity_model.hpp"
#include "binary_uuid_fixture.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
}
void* operator new(std::size_t n) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = 0; fault::hit = true; throw std::bad_alloc();
  }
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace o = scratchbird::engine::optimizer;
using Uuid = scratchbird::engine::planner::CanonicalPlannerUuid;
static_assert(sizeof(Uuid) == 16);
namespace {
unsigned checks = 0;
unsigned faults = 0;
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

void ExtendedSelectivity() {
  o::ExtendedStatsSelectivityRequest request;
  request.relation_uuid = Id(1); request.column_uuids = {Id(4), Id(5)};
  request.children = {{0.1, o::CostConfidence::kExact, {}, false},
                      {0.2, o::CostConfidence::kExact, {}, false}};
  o::ExtendedOptimizerStatistic stats;
  stats.identity = Identity(1, 6); stats.relation_uuid = Id(1);
  stats.column_uuids = request.column_uuids; stats.multi_column_distinct_count = 100;
  auto result = o::EstimateCorrelatedConjunctionSelectivity(request, {stats});
  Check(result.used_extended_stats && result.estimate.selectivity == 0.01, "binary NDV shape selected");
  Check(result.selected_statistic_uuids == std::vector<Uuid>{Id(6)}, "selected statistics retain binary identities");
  for (unsigned bit = 0; bit < 128; ++bit) {
    auto changed = request; changed.relation_uuid.bytes[bit / 8] ^= 1U << (bit % 8);
    Check(!o::EstimateCorrelatedConjunctionSelectivity(changed, {stats}).used_extended_stats,
          "all128 relation bits isolate statistics");
    changed = request; changed.column_uuids[0].bytes[bit / 8] ^= 1U << (bit % 8);
    Check(!o::EstimateCorrelatedConjunctionSelectivity(changed, {stats}).used_extended_stats,
          "all128 column bits isolate statistics");
  }
  auto wrong_owner = stats; wrong_owner.identity.object_uuid = Id(2);
  Check(!o::EstimateCorrelatedConjunctionSelectivity(request, {wrong_owner}).used_extended_stats,
        "relation match cannot override different statistics owner");
  auto duplicate = stats; duplicate.column_uuids = {Id(4), Id(4)};
  Check(!o::EstimateCorrelatedConjunctionSelectivity(request, {duplicate}).used_extended_stats,
        "duplicate dimensions cannot claim double coverage");
  duplicate = stats; duplicate.multi_column_distinct_count = 50;
  Check(!o::EstimateCorrelatedConjunctionSelectivity(request, {stats, duplicate}).used_extended_stats,
        "duplicate statistic identities cannot publish order-dependent conflicting values");
  auto bad_request = request; bad_request.children[0].selectivity = std::numeric_limits<double>::quiet_NaN();
  const auto invalid_request = o::EstimateCorrelatedConjunctionSelectivity(bad_request, {stats});
  Check(!invalid_request.used_extended_stats && invalid_request.estimate.confidence == o::CostConfidence::kRejected,
        "malformed request cannot inherit exact fallback confidence");
  auto bad = stats; bad.histogram_selectivity = std::numeric_limits<double>::quiet_NaN();
  Check(!o::EstimateCorrelatedConjunctionSelectivity(request, {bad}).used_extended_stats, "NaN statistics refuse");
  bad = stats; bad.kind = static_cast<o::ExtendedOptimizerStatisticKind>(255);
  Check(!o::EstimateCorrelatedConjunctionSelectivity(request, {bad}).used_extended_stats, "unknown statistic kind refuses");
  auto tied = stats; tied.identity.statistic_uuid = Id(7); tied.multi_column_distinct_count = 50;
  result = o::EstimateCorrelatedConjunctionSelectivity(request, {tied, stats});
  Check(result.selected_statistic_uuids == std::vector<Uuid>{Id(6)} && result.estimate.selectivity == 0.01,
        "binary identity deterministically breaks statistic ties");
  stats.kind = o::ExtendedOptimizerStatisticKind::kJointMcv;
  stats.joint_mcv = {{{"A", "B"}, 0.3}};
  request.column_uuids = {Id(5), Id(4)}; request.value_encodings = {"B", "A"};
  result = o::EstimateCorrelatedConjunctionSelectivity(request, {stats});
  Check(result.used_extended_stats && result.estimate.selectivity == 0.3,
        "MCV values follow statistic column ordering after query permutation");
  stats.kind = o::ExtendedOptimizerStatisticKind::kFkPkJoinCardinality;
  stats.fk_pk_shortcut = true; stats.fk_pk_estimated_rows = 0;
  request.join_cardinality_request = true;
  result = o::EstimateCorrelatedConjunctionSelectivity(request, {stats});
  Check(result.used_extended_stats && result.estimate.exact_rows_known && result.estimate.exact_rows == 0,
        "known zero join estimate is not a missing value");
  o::SelectivityEstimate estimate{1, o::CostConfidence::kExact, {}, false};
  o::PredicateSelectivityInput equality;
  equality.predicate_kind = "scalar_eq"; equality.input_rows = 100;
  equality.has_mcv = true; equality.input_confidence = o::CostConfidence::kExact;
  auto equality_result = o::EstimatePredicateSelectivity(equality);
  Check(equality_result.conservative && equality_result.confidence == o::CostConfidence::kLow,
        "MCV existence without frequency cannot invent an exact measured frequency");
  equality.has_mcv_frequency = true; equality.mcv_frequency = 0;
  equality_result = o::EstimatePredicateSelectivity(equality);
  Check(equality_result.selectivity == 0 && !equality_result.conservative, "measured zero MCV frequency preserved");
  equality.has_mcv_frequency = false; equality.distinct_values = 10; equality.null_fraction = 1;
  Check(o::EstimatePredicateSelectivity(equality).selectivity == 0, "all-null column equality has zero non-null selectivity");
  o::PredicateSelectivityInput empty_unique;
  empty_unique.predicate_kind = "unique_eq";
  const auto zero_unique = o::EstimatePredicateSelectivity(empty_unique);
  Check(zero_unique.exact_rows_known && zero_unique.exact_rows == 0,
        "unique lookup does not claim a row in known empty input");
  for (auto rows : {UINT64_C(0), UINT64_C(1), UINT64_C(9007199254740993), UINT64_MAX}) {
    Check(o::EstimateRowsAfterSelectivity(rows, estimate) == rows, "unit selectivity retains exact uint64 count");
    Check(o::EstimateJoinRowsAfterSelectivity(rows, 1, estimate) == rows, "single-partner join exact uint64 count");
  }
  estimate.selectivity = 0.5;
  Check(o::EstimateRowsAfterSelectivity(5, estimate) == 3, "cardinality rounds upward");
  Check(o::EstimateJoinRowsAfterSelectivity(UINT64_MAX, 2, estimate) == UINT64_MAX,
        "join multiplication saturates after applying selectivity");
  estimate.selectivity = std::numeric_limits<double>::quiet_NaN();
  Check(o::EstimateRowsAfterSelectivity(17, estimate) == 17, "invalid estimate conservatively preserves input bound");
  Check(o::EstimateJoinRowsAfterSelectivity(17, 3, estimate) == 51, "invalid join estimate preserves pair bound");
  stats.kind = o::ExtendedOptimizerStatisticKind::kJointMcv;
  request.join_cardinality_request = false;
  const std::vector<o::ExtendedOptimizerStatistic> sources{stats};
  bool finished = false;
  for (long allocation = 0; allocation < 4096; ++allocation) {
    std::optional<o::ExtendedStatsSelectivityResult> published;
    fault::remaining = allocation; fault::hit = false;
    try { published = o::EstimateCorrelatedConjunctionSelectivity(request, sources); }
    catch (const std::bad_alloc&) {}
    const bool hit = fault::hit; fault::remaining = -1;
    if (!hit) {
      Check(published && published->used_extended_stats && published->estimate.selectivity == 0.3,
            "unfaulted selection publishes complete actual result");
      finished = true; break;
    }
    ++faults;
    Check(!published, "allocation failure publishes no partial selected-statistics result");
    Check(request.value_encodings == std::vector<std::string>{"B", "A"} &&
          sources[0].joint_mcv[0].value_encodings == std::vector<std::string>{"A", "B"},
          "failed selection preserves source and query ordering");
  }
  Check(finished, "selection allocation sweep exhausted");
}
}
int main() {
  try {
    StoreAndCounts(); CorrelationsAndIndexes(); BinaryKeys(); AccessBindings(); ExtendedSelectivity();
    std::cout << "PASS full statistics binary store checks=" << checks << " faults=" << faults << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n'; return 1;
  }
}

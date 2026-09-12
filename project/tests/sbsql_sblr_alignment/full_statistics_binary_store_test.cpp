// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/optimizer/optimizer_statistics_full.hpp"
#include "../../src/engine/optimizer/access_path_full.hpp"
#include "../../src/engine/optimizer/selectivity_model.hpp"
#include "../../src/engine/optimizer/optimizer_contract.hpp"
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
namespace {
void CanonicalBinaryExpressions() {
  o::CanonicalSblrExpressionNode leaf;
  leaf.operator_id = "column"; leaf.descriptor_digest = "d"; leaf.object_uuid = Id(1);
  const auto result = o::CanonicalizeSblrExpressionTree(leaf);
  std::string expected;
  const auto number = [&](std::uint64_t n) {
    for (unsigned i = 0; i != 8; ++i) expected.push_back(static_cast<char>(n >> (8 * i)));
  };
  const auto field = [&](std::string_view value) { number(value.size()); expected.append(value); };
  field("optimizer-sblr-expression-v2"); field("column"); field("d");
  for (const auto byte : Id(1).bytes) expected.push_back(static_cast<char>(byte));
  expected.append(16, '\0'); field(""); expected.push_back('\0'); number(0);
  Check(result.ok && result.canonical_text == expected, "independent binary expression frame oracle");
  // Independently calculated from that explicit byte frame using Linux sha256sum.
  Check(result.digest == "sblrexpr256:538fc114a6509908c3c30a552d30888fa8977cde252c5e83cd8084229a9600e6",
        "full SHA256 expression known-answer test");
  for (unsigned bit = 0; bit != 128; ++bit) {
    auto changed = leaf;
    changed.object_uuid.bytes[bit / 8] ^= 1u << (bit % 8);
    const auto value = o::CanonicalizeSblrExpressionTree(changed);
    const bool valid = (changed.object_uuid.bytes[6] >> 4) == 7 &&
                       (changed.object_uuid.bytes[8] & 0xc0) == 0x80;
    Check(value.ok == valid, "binary expression UUIDv7 admission covers every bit");
    if (valid)
      Check(value.canonical_text != result.canonical_text && value.digest != result.digest,
            "every admitted object bit affects full expression identity");
    else
      Check(value.canonical_text.empty() && value.digest.empty() &&
            value.searchable_expression_digests.empty(), "refused expression publishes no digest");
    changed = leaf;
    changed.function_uuid = changed.object_uuid = Id(1);
    changed.function_uuid.bytes[bit / 8] ^= 1u << (bit % 8);
    const auto function = o::CanonicalizeSblrExpressionTree(changed);
    Check(function.ok == valid, "function identity uses the same binary admission");
  }
  o::CanonicalSblrExpressionNode sum;
  sum.operator_id = "add"; sum.commutative = true; sum.children = {leaf, leaf};
  const auto twice = o::CanonicalizeSblrExpressionTree(sum);
  sum.children.pop_back();
  Check(twice.digest != o::CanonicalizeSblrExpressionTree(sum).digest,
        "commutativity never deletes repeated operands");
  auto other = leaf; other.object_uuid = Id(3);
  sum.children = {leaf, other};
  const auto forward = o::CanonicalizeSblrExpressionTree(sum);
  std::reverse(sum.children.begin(), sum.children.end());
  Check(forward.digest == o::CanonicalizeSblrExpressionTree(sum).digest,
        "commutative permutation retains identical canonical frame");
  sum.commutative = false;
  const auto ordered = o::CanonicalizeSblrExpressionTree(sum);
  std::reverse(sum.children.begin(), sum.children.end());
  Check(ordered.digest != o::CanonicalizeSblrExpressionTree(sum).digest,
        "ordered operands remain order-sensitive");
  sum.children.back().raw_sql_text_present = true;
  const auto rejected = o::CanonicalizeSblrExpressionTree(sum);
  Check(!rejected.ok && rejected.canonical_text.empty() && rejected.digest.empty(),
        "invalid nested child cannot publish a partial expression identity");
}

void OptimizerBinaryTrees() {
  namespace p = scratchbird::engine::planner;
  p::LogicalPlan plan;
  plan.ok = true;
  auto scan = p::MakeLogicalPlanNode(p::LogicalPlanNodeKind::kDmlRead,
      p::PhysicalAccessKind::kNone, "query.scan", "base_scan");
  scan.required_object_uuids = {Id(1)};
  scan.required_descriptors = {std::string(64, 'a')};
  plan.nodes = {scan};
  auto statistics = o::DefaultLocalStatisticsCatalog();
  Check(statistics.Add(o::MakeUnsignedStatistic("row_count", "relation",
      o::OptimizerStatisticTarget::Object(Id(1)), 100, o::StatisticSource::kCatalogExact,
      7, 0, o::CostConfidence::kExact)), "actual count admitted");
  Check(statistics.Add(o::MakeUnsignedStatistic("visible_row_count", "relation",
      o::OptimizerStatisticTarget::Object(Id(1)), 0, o::StatisticSource::kCatalogExact,
      7, 0, o::CostConfidence::kExact)), "actual zero visible count admitted");
  auto optimized = o::OptimizeLogicalPlanWithStatistics(plan, statistics);
  Check(optimized.ok && optimized.has_physical_plan, "binary single-relation physical tree");
  Check(optimized.physical_root.relation_uuid == Id(1), "binary scan binding retained in tree");
  for (const auto& candidate : optimized.candidates)
    if (candidate.selected)
      Check(candidate.statistics_version.find("epoch1") == std::string::npos,
            "selected statistics do not fabricate epoch1");

  auto window = p::MakeLogicalPlanNode(p::LogicalPlanNodeKind::kDmlRead,
      p::PhysicalAccessKind::kSortThenWindow, "query.window", "window");
  window.required_object_uuids = {Id(1)};
  window.required_descriptors = scan.required_descriptors;
  plan.nodes.push_back(window);
  optimized = o::OptimizeLogicalPlanWithStatistics(plan, statistics);
  Check(optimized.ok, "zero visible rows retain a window plan");
  const auto window_candidate = std::find_if(optimized.candidates.begin(), optimized.candidates.end(),
      [](const auto& candidate) { return candidate.node.operation_id == "query.window"; });
  Check(window_candidate != optimized.candidates.end() &&
        window_candidate->plan_candidate.estimated_rows == 0,
        "known zero beats positive physical count and policy fallback");

  plan.nodes = {scan};
  o::TableCardinalityStats table{Identity(1, 2), 100, 100, 10, 8};
  o::AccessPathPlanningRequest request;
  request.relation_uuid = Id(1);
  request.descriptor_digest = scan.required_descriptors.front();
  request.table_stats = table;
  request.visibility_proven = true;
  request.grants_proven = true;
  auto limit = p::MakeLogicalPlanNode(p::LogicalPlanNodeKind::kDmlRead,
      p::PhysicalAccessKind::kTopN, "query.limit", "limit");
  limit.required_object_uuids = {Id(1)};
  limit.required_descriptors = scan.required_descriptors;
  plan.nodes.push_back(limit);
  request.ordered_limit.present = true;
  request.ordered_limit.limit_count = 0;
  optimized = o::OptimizeLogicalPlanWithAccessPathRequest(plan, request);
  Check(optimized.ok && optimized.physical_root.estimated_rows == 0,
        "request LIMIT zero remains zero through candidate and physical composition");
  table.identity.source = o::StatisticSource::kCatalogSample;
  table.identity.confidence = o::CostConfidence::kLow;
  plan.nodes = {scan, window};
  table.row_count = 0; table.visible_row_count = 0; table.page_count = 0;
  request.table_stats = table;
  request.ordered_limit.present = false;
  optimized = o::OptimizeLogicalPlanWithAccessPathRequest(plan, request);
  Check(optimized.ok && optimized.physical_root.estimated_rows == 0,
        "sample-backed empty relation is not replaced by a default estimate");
  for (const auto& candidate : optimized.candidates) {
    if (candidate.node.operation_id != "query.scan") continue;
    Check(candidate.plan_candidate.statistic_inputs.size() == 2,
          "full access retains consumed base count and page inputs");
    for (const auto& statistic : candidate.plan_candidate.statistic_inputs)
      Check(statistic.source == o::StatisticSource::kCatalogSample &&
            statistic.confidence == o::CostConfidence::kLow &&
            statistic.stats_epoch == 7 && statistic.exact_unsigned_value == 0 &&
            statistic.target.object_uuid == Id(1),
            "sample provenance and exact zero survive full access costing");
  }
  Check(!o::ValidateBenchmarkCleanOptimizedPlan(optimized).ok,
        "unbound upper statistics cannot be certified by a primary leaf");

  auto join = p::MakeLogicalPlanNode(p::LogicalPlanNodeKind::kDmlRead,
      p::PhysicalAccessKind::kJoinNestedLoop, "query.join", "join");
  join.required_object_uuids = {Id(1), Id(3)};
  join.required_descriptors = scan.required_descriptors;
  plan.nodes = {join};
  optimized = o::OptimizeLogicalPlanWithStatistics(plan, o::DefaultLocalStatisticsCatalog());
  Check(optimized.ok && optimized.physical_root.children.size() == 2,
        "binary graph composes actual two-child physical tree");
  auto order = optimized.physical_root.ordered_relation_uuids;
  std::sort(order.begin(), order.end());
  Check(order == std::vector<Uuid>{Id(1), Id(3)}, "physical join retains binary relation order");
  const auto json = o::SerializePhysicalPlanNodeToJson(optimized.physical_root);
  Check(json.find("ordered_relation_uuid_bytes") != std::string::npos &&
        json.find("019f0000-") == std::string::npos, "diagnostics project byte arrays, not UUID text");
  join.required_object_uuids = {Id(1), Id(1)};
  plan.nodes = {join};
  optimized = o::OptimizeLogicalPlanWithStatistics(plan, statistics);
  Check(!optimized.ok && !optimized.has_physical_plan,
        "invalid duplicate graph cannot fall through to a successful physical plan");

  auto source_free_left = scan;
  source_free_left.required_object_uuids.clear();
  source_free_left.operation_id = "values.left";
  auto source_free_right = source_free_left;
  source_free_right.operation_id = "values.right";
  auto set = p::MakeLogicalPlanNode(p::LogicalPlanNodeKind::kDmlRead,
      p::PhysicalAccessKind::kSetOperation, "query.union", "union");
  set.required_descriptors = scan.required_descriptors;
  plan.nodes = {source_free_left, source_free_right, set};
  optimized = o::OptimizeLogicalPlanWithStatistics(plan, o::DefaultLocalStatisticsCatalog());
  Check(optimized.ok && optimized.physical_root.children.size() == 2,
        "absent source identities do not collapse separate source-free set inputs");
  Check(optimized.physical_root.children[0].runtime_evidence !=
        optimized.physical_root.children[1].runtime_evidence,
        "source-free set composition retains distinct logical operands");

  plan.nodes = {scan};
  request.table_stats = o::TableCardinalityStats{Identity(1, 2), 100, 100, 10, 8};
  bool finished = false;
  for (long failure = 0; failure < 4096 && !finished; ++failure) {
    std::optional<o::OptimizedPlan> published;
    fault::remaining = failure; fault::hit = false;
    try { published = o::OptimizeLogicalPlanWithAccessPathRequest(plan, request); }
    catch (const std::bad_alloc&) {}
    fault::remaining = -1;
    if (!fault::hit) {
      Check(published && published->ok, "optimizer allocation sweep reaches actual success");
      finished = true;
      continue;
    }
    ++faults;
    Check(!published || (!published->ok && !published->has_physical_plan),
          "allocation failure cannot publish a successful physical plan");
    Check(plan.nodes[0].required_object_uuids == std::vector<Uuid>{Id(1)} &&
          request.table_stats->visible_row_count == 100,
          "failed optimization preserves original bindings and statistics");
  }
  Check(finished, "optimizer persistent allocation sweep exhausted");
}
}
int main() {
  try {
    StoreAndCounts(); CorrelationsAndIndexes(); BinaryKeys(); AccessBindings(); ExtendedSelectivity();
    OptimizerBinaryTrees();
    CanonicalBinaryExpressions();
    std::cout << "PASS full statistics binary store checks=" << checks << " faults=" << faults << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n'; return 1;
  }
}

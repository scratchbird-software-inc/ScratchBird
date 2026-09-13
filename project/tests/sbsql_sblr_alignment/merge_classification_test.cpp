// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "dml/merge_classification.hpp"
#include "../common/single_tu_allocation_fault.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <tuple>

namespace api = scratchbird::engine::internal_api;
using Kind = api::MergeActionKind;
using Policy = api::MergeMatchPolicy;
using Sources = std::vector<api::MergeSourceClassification>;
unsigned checks = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
api::EngineUuid Id(unsigned suffix) {
  api::EngineUuid id;
  id.bytes = {0x01, 0x9e, 0x87, 0x65, 0x43, 0x21, 0x70, 0, 0x80, 0, 0, 0, 0, 0,
      static_cast<std::uint8_t>(suffix >> 8), static_cast<std::uint8_t>(suffix)};
  return id;
}
void Refused(const Sources& sources, Policy policy, const char* error) {
  const auto result = api::ClassifyMergeMatchSets(sources, policy);
  Check(!result.ok && result.error && std::string(result.error) == error, "wrong classification refusal");
  Check(result.actions.empty() && !result.matched_pairs && !result.matched_sources && !result.unmatched_sources,
      "refusal published partial classification");
}
void Examples() {
  Sources sources = {{0, {{Id(3), Kind::update}, {Id(1), Kind::delete_row}}, Kind::insert},
      {1, {}, Kind::insert}, {2, {{Id(3), Kind::no_action}}, Kind::no_action}};
  const auto result = api::ClassifyMergeMatchSets(sources, Policy::all_targets);
  Check(result.ok && result.matched_sources == 2 && result.unmatched_sources == 1 &&
      result.matched_pairs == 3 && result.actions.size() == 4, "incomplete match classification");
  Check(result.actions[0].target_row_uuid == Id(1) && result.actions[0].action == Kind::delete_row &&
      result.actions[1].target_row_uuid == Id(3) && result.actions[2].source_ordinal == 1 &&
      result.actions[2].target_row_uuid.is_nil() && result.actions[2].action == Kind::insert &&
      result.actions[3].action == Kind::no_action, "wrong source/target action ordering");
  Check(sources[0].targets[0].row_uuid == Id(3), "classification reordered caller input");
  Refused(sources, Policy::single_target, "merge_multiple_targets_forbidden");
  sources[2].targets[0].action = Kind::delete_row;
  Refused(sources, Policy::all_targets, "merge_target_mutated_by_multiple_sources");
  sources[2].targets[0].action = Kind::no_action;
  sources[0].targets.push_back({Id(3), Kind::no_action});
  Refused(sources, Policy::all_targets, "merge_duplicate_target_in_source");
  sources[0].targets.pop_back();
  sources[1].source_ordinal = 0;
  Refused(sources, Policy::all_targets, "merge_source_ordinal_invalid");
  sources[1].source_ordinal = 1;
  sources[0].unmatched_action = Kind::update;
  Refused(sources, Policy::all_targets, "merge_unmatched_action_invalid");
  sources[0].unmatched_action = Kind::insert;
  sources[0].targets[0].action = Kind::insert;
  Refused(sources, Policy::all_targets, "merge_matched_action_invalid");
  sources[0].targets[0].action = Kind::update;
  Refused(sources, static_cast<Policy>(0), "merge_match_policy_invalid");
  Refused(sources, static_cast<Policy>(255), "merge_match_policy_invalid");
  sources[0].targets[0].action = static_cast<Kind>(255);
  Refused(sources, Policy::all_targets, "merge_matched_action_invalid");
  const auto empty = api::ClassifyMergeMatchSets({}, Policy::all_targets);
  Check(empty.ok && empty.actions.empty() && !empty.matched_pairs && !empty.unmatched_sources,
      "admitted empty source is not zero actions");
  const auto sparse = api::ClassifyMergeMatchSets({sources.data(), 0}, Policy::single_target);
  Check(sparse.ok, "empty single-target source refused");
  const Sources sparse_sources = {{7, {{Id(9), Kind::update}}, Kind::insert},
      {std::numeric_limits<std::size_t>::max(), {}, Kind::insert}};
  const auto sparse_result = api::ClassifyMergeMatchSets(sparse_sources, Policy::single_target);
  Check(sparse_result.ok && sparse_result.actions[0].source_ordinal == 7 &&
      sparse_result.actions[1].source_ordinal == std::numeric_limits<std::size_t>::max(),
      "source occurrence ordinal was narrowed or renumbered");
  for (unsigned version = 0; version < 16; ++version) {
    if (version == 7) continue;
    auto id = Id(9); id.bytes[6] = static_cast<std::uint8_t>(version << 4);
    Refused({{0, {{Id(1), Kind::update}, {id, Kind::update}}, Kind::insert}},
        Policy::all_targets, "merge_target_row_identity_invalid");
  }
  auto id = Id(9); id.bytes[8] = 0x40;
  Refused({{0, {{id, Kind::no_action}}, Kind::insert}}, Policy::all_targets,
      "merge_target_row_identity_invalid");
  Refused({{0, {{{}, Kind::no_action}}, Kind::insert}}, Policy::all_targets,
      "merge_target_row_identity_invalid");
}
void AllocationFailures() {
  Sources sources;
  for (unsigned s = 0; s < 8; ++s) {
    api::MergeSourceClassification source;
    source.source_ordinal = s;
    for (unsigned r = 0; r < 17; ++r) source.targets.push_back({Id(1000 + 17 * s + r), Kind::update});
    sources.push_back(std::move(source));
  }
  allocation_attempts = 0;
  allocations_before_failure = std::numeric_limits<std::ptrdiff_t>::max();
  auto success = api::ClassifyMergeMatchSets(sources, Policy::all_targets);
  allocations_before_failure = -1;
  const auto points = allocation_attempts;
  Check(success.ok && success.actions.size() == 136 && points > 136, "allocation coverage setup failed");
  for (std::size_t point = 0; point < points; ++point) {
    allocations_before_failure = static_cast<std::ptrdiff_t>(point);
    bool threw = false;
    try { (void)api::ClassifyMergeMatchSets(sources, Policy::all_targets); }
    catch (const std::bad_alloc&) { threw = true; }
    allocations_before_failure = -1;
    Check(threw, "allocation failure did not propagate");
    Check(sources.size() == 8 && sources.back().targets.back().row_uuid == Id(1135), "failure changed input");
    Check(api::ClassifyMergeMatchSets(sources, Policy::all_targets).actions.size() == 136,
        "allocation failure changed subsequent classification");
  }
  std::cout << "merge_classification allocation_points=" << points << '\n';
}
// Independent exhaustive small-set oracle uses linear identity lists and a
// sorted tuple sequence, not the production mutation-set/classification loop.
void SmallSetOracle() {
  for (unsigned bits = 0; bits < 4096; ++bits) {
    Sources input = {{0, {}, Kind::insert}, {1, {}, Kind::no_action}, {2, {}, Kind::insert}};
    for (unsigned slot = 0; slot < 6; ++slot) {
      const auto value = (bits >> (2 * slot)) & 3u;
      if (value) input[slot / 2].targets.push_back({Id(value), slot & 1 ? Kind::no_action : Kind::update});
    }
    bool valid = true;
    unsigned matched_sources = 0, unmatched_sources = 0, pairs = 0;
    std::vector<api::EngineUuid> writes;
    using Tuple = std::tuple<std::size_t, api::EngineUuid, Kind>;
    std::vector<Tuple> expected;
    for (const auto& source : input) {
      if (source.targets.empty()) {
        ++unmatched_sources;
        expected.emplace_back(source.source_ordinal, api::EngineUuid{}, source.unmatched_action);
      } else ++matched_sources;
      pairs += source.targets.size();
      for (std::size_t i = 0; i < source.targets.size(); ++i) {
        const auto& target = source.targets[i];
        for (std::size_t j = 0; j < i; ++j)
          if (target.row_uuid == source.targets[j].row_uuid) valid = false;
        if (target.action == Kind::update) {
          if (std::find(writes.begin(), writes.end(), target.row_uuid) != writes.end()) valid = false;
          writes.push_back(target.row_uuid);
        }
        expected.emplace_back(source.source_ordinal, target.row_uuid, target.action);
      }
    }
    const auto actual = api::ClassifyMergeMatchSets(input, Policy::all_targets);
    Check(actual.ok == valid, "small-set oracle admission mismatch");
    if (!valid) { Check(actual.actions.empty() && !actual.matched_pairs, "oracle refusal leaked prefix"); continue; }
    Check(actual.matched_sources == matched_sources && actual.unmatched_sources == unmatched_sources &&
        actual.matched_pairs == pairs, "small-set oracle count mismatch");
    std::sort(expected.begin(), expected.end());
    std::vector<Tuple> observed;
    for (const auto& action : actual.actions) observed.emplace_back(action.source_ordinal, action.target_row_uuid, action.action);
    Check(observed == expected, "small-set oracle action mismatch");
  }
}
int main() {
  Examples();
  AllocationFailures();
  SmallSetOracle();
  std::cout << "merge_classification checks=" << checks << " failures=0\n";
}

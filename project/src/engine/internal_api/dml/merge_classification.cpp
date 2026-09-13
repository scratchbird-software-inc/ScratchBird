// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/merge_classification.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace scratchbird::engine::internal_api {
namespace {
MergeClassificationResult Fail(const char* error) {
  MergeClassificationResult result;
  result.error = error;
  return result;
}
bool IsMatchedAction(MergeActionKind action) {
  return action == MergeActionKind::no_action || action == MergeActionKind::update ||
      action == MergeActionKind::delete_row;
}
}  // namespace

MergeClassificationResult ClassifyMergeMatchSets(
    std::span<const MergeSourceClassification> sources, MergeMatchPolicy policy) {
  if (policy != MergeMatchPolicy::all_targets && policy != MergeMatchPolicy::single_target)
    return Fail("merge_match_policy_invalid");
  MergeClassificationResult staged;
  std::set<EngineUuid> mutation_targets;
  for (std::size_t ordinal = 0; ordinal < sources.size(); ++ordinal) {
    const auto& source = sources[ordinal];
    if (ordinal != 0 && source.source_ordinal <= sources[ordinal - 1].source_ordinal)
      return Fail("merge_source_ordinal_invalid");
    if (source.unmatched_action != MergeActionKind::no_action &&
        source.unmatched_action != MergeActionKind::insert)
      return Fail("merge_unmatched_action_invalid");
    if (policy == MergeMatchPolicy::single_target && source.targets.size() > 1)
      return Fail("merge_multiple_targets_forbidden");
    if (source.targets.empty()) {
      ++staged.unmatched_sources;
      staged.actions.push_back({source.source_ordinal, {}, source.unmatched_action});
      continue;
    }
    if (source.targets.size() > std::numeric_limits<EngineApiU64>::max() - staged.matched_pairs)
      return Fail("merge_match_count_overflow");
    auto targets = source.targets;
    std::sort(targets.begin(), targets.end(), [](const auto& left, const auto& right) {
      return left.row_uuid < right.row_uuid;
    });
    EngineUuid previous;
    for (const auto& target : targets) {
      if (!scratchbird::core::uuid::IsEngineIdentityUuid(target.row_uuid))
        return Fail("merge_target_row_identity_invalid");
      if (!IsMatchedAction(target.action)) return Fail("merge_matched_action_invalid");
      if (target.row_uuid == previous) return Fail("merge_duplicate_target_in_source");
      previous = target.row_uuid;
      if (target.action != MergeActionKind::no_action &&
          !mutation_targets.insert(target.row_uuid).second)
        return Fail("merge_target_mutated_by_multiple_sources");
      staged.actions.push_back({source.source_ordinal, target.row_uuid, target.action});
    }
    ++staged.matched_sources;
    staged.matched_pairs += targets.size();
  }
  staged.ok = true;
  return staged;
}
}  // namespace scratchbird::engine::internal_api

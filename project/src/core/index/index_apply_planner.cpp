// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_apply_planner.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace scratchbird::core::index {
namespace {

std::uint64_t Fnva64(const std::string& value) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char ch : value) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string FamilyProfileKey(const CommitGroupLocalityIndexApplyItem& item) {
  return item.family + "|" + item.profile;
}

std::string UniqueLocalityKey(const CommitGroupLocalityIndexApplyItem& item) {
  return "unique_order:" + item.index_uuid;
}

std::string NonUniqueLocalityKey(const CommitGroupLocalityIndexApplyItem& item) {
  std::ostringstream stable;
  stable << item.index_uuid << '\0' << item.family << '\0' << item.profile;
  for (const auto& key : item.target_keys) {
    stable << '\0' << key;
  }
  const std::uint64_t bucket = Fnva64(stable.str()) % 64u;
  return "leaf_bucket:" + item.index_uuid + ":" + std::to_string(bucket);
}

PageAwareSecondaryChangeBufferRequest ChangeBufferRequestForItem(
    const CommitGroupLocalityIndexApplyItem& item) {
  PageAwareSecondaryChangeBufferRequest request =
      item.secondary_change_buffer_request_present
          ? item.secondary_change_buffer_request
          : PageAwareSecondaryChangeBufferRequest{};
  request.index_kind =
      item.unique ? SecondaryIndexKind::unique : SecondaryIndexKind::non_unique;
  return request;
}

}  // namespace

std::string CommitGroupLocalityTargetKey(
    const CommitGroupLocalityIndexApplyItem& item) {
  return item.unique ? UniqueLocalityKey(item) : NonUniqueLocalityKey(item);
}

CommitGroupLocalityIndexApplyPlan PlanCommitGroupLocalityIndexApply(
    const std::vector<CommitGroupLocalityIndexApplyItem>& items) {
  CommitGroupLocalityIndexApplyPlan plan;
  plan.pending_item_count = static_cast<std::uint64_t>(items.size());
  for (const auto& item : items) {
    if (item.index_uuid.empty()) {
      plan.refusal_reason = "index_uuid_required";
      return plan;
    }
    if (item.family.empty() || item.profile.empty()) {
      plan.refusal_reason = "index_family_profile_required";
      return plan;
    }
    if (item.target_keys.empty()) {
      plan.refusal_reason = "target_leaf_page_key_required";
      return plan;
    }
  }

  std::set<std::string> family_profile_keys;
  std::map<std::string, CommitGroupLocalityIndexApplyGroup> unique_groups;
  std::map<std::string, CommitGroupLocalityIndexApplyGroup> locality_groups;
  plan.secondary_change_buffer_decisions.reserve(items.size());
  for (std::size_t ordinal = 0; ordinal < items.size(); ++ordinal) {
    const auto& item = items[ordinal];
    plan.secondary_change_buffer_decisions.push_back(
        SelectPageAwareSecondaryChangeBufferV2(ChangeBufferRequestForItem(item)));
    const std::string family_profile_key = FamilyProfileKey(item);
    family_profile_keys.insert(family_profile_key);
    if (item.unique) {
      const std::string group_key =
          "unique|" + std::to_string(item.source_batch_ordinal);
      auto& group = unique_groups[group_key];
      group.family_profile_key = family_profile_key;
      group.target_leaf_page_locality_key = UniqueLocalityKey(item);
      group.unique_order_preserved = true;
      group.item_ordinals.push_back(ordinal);
      continue;
    }

    const std::string locality_key = NonUniqueLocalityKey(item);
    const std::string group_key = family_profile_key + "|" + locality_key;
    auto& group = locality_groups[group_key];
    group.family_profile_key = family_profile_key;
    group.target_leaf_page_locality_key = locality_key;
    group.item_ordinals.push_back(ordinal);
  }

  for (auto& entry : unique_groups) {
    auto& ordinals = entry.second.item_ordinals;
    std::sort(ordinals.begin(), ordinals.end(), [&items](std::size_t lhs,
                                                         std::size_t rhs) {
      const auto& left = items[lhs];
      const auto& right = items[rhs];
      if (left.source_batch_ordinal != right.source_batch_ordinal) {
        return left.source_batch_ordinal < right.source_batch_ordinal;
      }
      return left.source_row_ordinal < right.source_row_ordinal;
    });
    plan.groups.push_back(std::move(entry.second));
  }
  for (auto& entry : locality_groups) {
    auto& ordinals = entry.second.item_ordinals;
    std::sort(ordinals.begin(), ordinals.end(), [&items](std::size_t lhs,
                                                         std::size_t rhs) {
      const auto& left = items[lhs];
      const auto& right = items[rhs];
      if (left.index_uuid != right.index_uuid) {
        return left.index_uuid < right.index_uuid;
      }
      if (left.source_batch_ordinal != right.source_batch_ordinal) {
        return left.source_batch_ordinal < right.source_batch_ordinal;
      }
      return left.source_row_ordinal < right.source_row_ordinal;
    });
    plan.groups.push_back(std::move(entry.second));
  }

  std::set<std::string> locality_keys;
  for (const auto& group : plan.groups) {
    locality_keys.insert(group.target_leaf_page_locality_key);
  }
  plan.family_profile_keys.assign(family_profile_keys.begin(),
                                  family_profile_keys.end());
  plan.target_leaf_page_locality_keys.assign(locality_keys.begin(),
                                             locality_keys.end());
  plan.grouped_family_count =
      static_cast<std::uint64_t>(plan.family_profile_keys.size());
  plan.locality_group_count =
      static_cast<std::uint64_t>(plan.target_leaf_page_locality_keys.size());
  plan.planned_before_append = true;
  plan.accepted = true;
  return plan;
}

}  // namespace scratchbird::core::index

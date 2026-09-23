// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "deferred_secondary_index_runtime_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::core::index {

struct CommitGroupLocalityIndexApplyItem {
  std::size_t source_batch_ordinal = 0;
  std::size_t source_row_ordinal = 0;
  scratchbird::core::platform::Uuid index_uuid;
  std::string family;
  std::string profile;
  bool unique = false;
  std::vector<std::string> target_keys;
  bool secondary_change_buffer_request_present = false;
  PageAwareSecondaryChangeBufferRequest secondary_change_buffer_request;
};

struct IndexApplyLocalityKey {
  scratchbird::core::platform::Uuid index_uuid;
  std::uint8_t bucket = 0;
  bool unique_order = false;
  auto operator<=>(const IndexApplyLocalityKey&) const = default;
};

struct CommitGroupLocalityIndexApplyGroup {
  std::string family_profile_key;
  IndexApplyLocalityKey target_leaf_page_locality_key;
  bool unique_order_preserved = false;
  std::vector<std::size_t> item_ordinals;
};

struct CommitGroupLocalityIndexApplyPlan {
  bool accepted = false;
  std::string refusal_reason;
  std::uint64_t pending_item_count = 0;
  std::uint64_t grouped_family_count = 0;
  std::uint64_t locality_group_count = 0;
  bool planned_before_append = false;
  bool unique_order_preserved = true;
  std::vector<std::string> family_profile_keys;
  std::vector<IndexApplyLocalityKey> target_leaf_page_locality_keys;
  std::vector<CommitGroupLocalityIndexApplyGroup> groups;
  std::vector<PageAwareSecondaryChangeBufferDecision>
      secondary_change_buffer_decisions;
};

CommitGroupLocalityIndexApplyPlan PlanCommitGroupLocalityIndexApply(
    const std::vector<CommitGroupLocalityIndexApplyItem>& items);

IndexApplyLocalityKey CommitGroupLocalityTargetKey(
    const CommitGroupLocalityIndexApplyItem& item);

}  // namespace scratchbird::core::index

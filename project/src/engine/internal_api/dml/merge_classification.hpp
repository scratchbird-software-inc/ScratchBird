// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace scratchbird::engine::internal_api {

enum class MergeMatchPolicy : std::uint8_t { all_targets = 1, single_target = 2 };
enum class MergeActionKind : std::uint8_t { no_action = 1, insert = 2, update = 3, delete_row = 4 };

struct MergeTargetClassification {
  EngineUuid row_uuid;
  MergeActionKind action = MergeActionKind::no_action;
};

struct MergeSourceClassification {
  std::size_t source_ordinal = 0;
  std::vector<MergeTargetClassification> targets;
  MergeActionKind unmatched_action = MergeActionKind::no_action;
};

struct MergeClassifiedAction {
  std::size_t source_ordinal = 0;
  EngineUuid target_row_uuid;
  MergeActionKind action = MergeActionKind::no_action;
};

struct MergeClassificationResult {
  bool ok = false;
  const char* error = nullptr;
  EngineApiU64 matched_sources = 0;
  EngineApiU64 unmatched_sources = 0;
  EngineApiU64 matched_pairs = 0;
  std::vector<MergeClassifiedAction> actions;
};

// Classifies the complete input before any owning mutation. Input snapshots,
// predicates, security, constraints and statement rollback retain their owners;
// this function neither acquires nor asserts any of those authorities.
// Failure returns no partial action sequence or counts. Allocation exceptions
// propagate before publication and leave the input unchanged.
MergeClassificationResult ClassifyMergeMatchSets(
    std::span<const MergeSourceClassification> sources, MergeMatchPolicy policy);

}  // namespace scratchbird::engine::internal_api

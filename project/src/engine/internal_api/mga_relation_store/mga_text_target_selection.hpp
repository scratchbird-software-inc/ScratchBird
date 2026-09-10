// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "mga_relation_store/mga_relation_store.hpp"

namespace scratchbird::engine::internal_api {

// Engine-only metadata lookup key. Query occurrences, literal tokens and
// parser spellings are deliberately absent. UUIDs are canonical binary16.
struct MgaTextColumnKeyV2 {
  sblr::ContextualTextUuidV2 relation_uuid{};
  sblr::ContextualTextUuidV2 relation_descriptor_uuid{};
  std::uint64_t relation_descriptor_generation = 0;
  sblr::ContextualTextUuidV2 column_uuid{};
  std::uint32_t column_ordinal = 0;

  bool operator==(const MgaTextColumnKeyV2&) const = default;
};

// Read-only selection of the same MGA-visible sealed TEXT metadata used by
// contextual queries. This is not assignment, budget or mutation admission.
MgaContextualTextTargetSelectionResultV2 SelectVisibleMgaTextColumnV2(
    const EngineRequestContext& context,
    const MgaTextColumnKeyV2& key,
    const EngineContextualTextPolicyRowSetV2& exact_policy_rows);

// Assignment does not imply collation/comparison authority. This variant also
// admits exact canonical TEXT without locale metadata, but still validates the
// complete sealed descriptor set (including every contextual sidecar present).
MgaContextualTextTargetSelectionResultV2 SelectVisibleMgaTextAssignmentColumnV2(
    const EngineRequestContext& context,
    const MgaTextColumnKeyV2& key,
    const EngineContextualTextPolicyRowSetV2& exact_policy_rows);

}  // namespace scratchbird::engine::internal_api

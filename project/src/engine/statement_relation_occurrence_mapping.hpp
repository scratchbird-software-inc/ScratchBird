// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "server_engine_bridge/statement_context.hpp"
#include "engine/sblr/sblr_literal_runtime.hpp"
#include "uuid.hpp"

namespace scratchbird::engine {

// Caller owns the live receipt lock and has checked its lifecycle. This only
// publishes an already engine-bound mapping; it does not resolve authority.
// Invalid fields or allocation failure cannot change either projection.
inline bool PublishStatementRelationOccurrenceMappingsV1(
    const std::vector<server_engine_bridge::StatementRelationOccurrenceMappingV1>& input,
    std::vector<server_engine_bridge::StatementRelationOccurrenceMappingV1>& view,
    std::vector<sblr::SblrLiteralPersistedDescriptorMappingV1>& executor) {
  std::uint64_t previous = 0;
  for (const auto& mapping : input) {
    if (mapping.occurrence_id <= previous ||
        !core::uuid::IsEngineIdentityUuid(mapping.persisted_descriptor_uuid) ||
        mapping.persisted_descriptor_generation == 0) {
      return false;
    }
    previous = mapping.occurrence_id;
  }

  auto staged_view = input;
  std::vector<sblr::SblrLiteralPersistedDescriptorMappingV1> staged_executor;
  staged_executor.reserve(input.size());
  for (const auto& mapping : input) {
    staged_executor.push_back({mapping.occurrence_id,
                              mapping.persisted_descriptor_uuid.bytes,
                              mapping.persisted_descriptor_generation});
  }
  static_assert(noexcept(view.swap(staged_view)));
  static_assert(noexcept(executor.swap(staged_executor)));
  view.swap(staged_view);
  executor.swap(staged_executor);
  return true;
}

}  // namespace scratchbird::engine

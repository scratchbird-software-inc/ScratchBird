// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../internal_api/api_types.hpp"
#include "../../core/uuid/uuid.hpp"

#include <optional>

namespace scratchbird::engine::executor {

// Issues an identifier, not permission or a content-binding digest. The caller
// must retain the identified object and its exact statement/owner bindings.
// Repeating the same plan or result does not reissue a hash-shaped identity.
inline std::optional<internal_api::EngineUuid> IssueRuntimeIdentityV7() noexcept {
  return scratchbird::core::uuid::IssueRuntimeIdentityV7();
}

}  // namespace scratchbird::engine::executor

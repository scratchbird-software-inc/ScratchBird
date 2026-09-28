// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "uuid.hpp"

namespace scratchbird::parser::sbsql {
// Parser-local field grouping, never the durable identity of an inserted row.
// Nil on failure is rejected before canonical submission. The engine's own
// CRUD allocator issues the durable row identity after statement admission.
inline core::platform::Uuid IssueCopyRowGroupIdentity() noexcept {
  return core::uuid::IssueRuntimeIdentityV7().value_or(core::platform::Uuid{});
}
}

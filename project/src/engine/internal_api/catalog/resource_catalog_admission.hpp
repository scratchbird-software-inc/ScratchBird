// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include "database_lifecycle.hpp"
#include <optional>

namespace scratchbird::engine::internal_api {
// Shared by name binding, bound-UUID lookup and timezone admission. The
// pathname locates storage; only the binary node/transaction/cohort is authority.
struct EngineResourceCatalogAdmission {
  EngineApiDiagnostic diagnostic;
  std::optional<scratchbird::storage::database::DatabaseLifecycleState> state;
  bool ok() const noexcept { return state.has_value() && !diagnostic.error; }
};
EngineResourceCatalogAdmission OpenEngineResourceCatalog(const EngineRequestContext& context);
}  // namespace scratchbird::engine::internal_api

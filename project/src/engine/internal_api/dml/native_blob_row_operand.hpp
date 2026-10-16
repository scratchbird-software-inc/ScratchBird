// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "runtime_platform.hpp"
#include <cstddef>

namespace scratchbird::engine::internal_api {
class StatementBlobRowValueV3;
class NativeBlobBulkLifetimeDiagnosticV3;

// Engine-private synchronous borrowed input, never a wire or cached carrier.
// Keep every source wrapper stable/alive and quiesce Release until return.
// Binary identity/generation observations are checked against fresh catalog
// admission; they are not permissions. The actual source must pass its native
// lifetime gate under the exact current statement. Generic row fields carry
// only the bound descriptor and explicit state, never a second payload.
struct NativeBlobRowOperandV3 {
  std::size_t row_index = 0;
  std::size_t field_index = 0;
  core::platform::Uuid column_uuid;
  std::uint64_t relation_generation = 0;
  std::uint64_t column_generation = 0;
  StatementBlobRowValueV3* source = nullptr;
};
} // namespace scratchbird::engine::internal_api

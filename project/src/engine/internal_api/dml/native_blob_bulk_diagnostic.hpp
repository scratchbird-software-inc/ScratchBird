// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "datatype_blob.hpp"
#include <string>

namespace scratchbird::engine::internal_api {
// Immutable, fully owned native cause. Its textual rendering views refer only
// to this stable owner; the typed fact retains binary UUIDs and all operands.
// This is failure evidence, never lifetime, row or publication authority.
class NativeBlobBulkLifetimeDiagnosticV3 final {
 private:
  std::string code_, reason_;
 public:
  const std::size_t row_index, field_index;
  const core::datatypes::BlobLifetimeRuntimeResultV3 lifetime;
  NativeBlobBulkLifetimeDiagnosticV3(std::size_t row, std::size_t field,
      const core::datatypes::BlobLifetimeRuntimeResultV3& source)
      : code_(source.diagnostic_code), reason_(source.reason), row_index(row), field_index(field),
        lifetime([&] { auto result=source; result.diagnostic_code=code_; result.reason=reason_; return result; }()) {}
  NativeBlobBulkLifetimeDiagnosticV3(const NativeBlobBulkLifetimeDiagnosticV3&) = delete;
  NativeBlobBulkLifetimeDiagnosticV3& operator=(const NativeBlobBulkLifetimeDiagnosticV3&) = delete;
};
} // namespace scratchbird::engine::internal_api

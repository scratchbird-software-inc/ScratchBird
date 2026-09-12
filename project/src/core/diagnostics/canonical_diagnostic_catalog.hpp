// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace scratchbird::core::diagnostics {
// Complete consolidated vocabulary; values match ERROR_VECTOR private metadata,
// not public ABI or MessageVector bytes. No severity authorizes disclosure.
enum class CanonicalSeverity : std::uint8_t {
  informational=1, warning=2, error=3, fatal=4, security=5, critical=6,
  corruption=7, panic=8, debug=9, notice=10, audit=11, support=12, internal=13
};
struct DiagnosticCodeDefinition {
  std::string_view code;
  CanonicalSeverity severity;
  bool is_failure;
  std::string_view sqlstate;
  std::string_view numeric_binding;
  std::string_view retry_class;
  std::string_view required_outcome;
  std::string_view diagnostic_class;
};
struct DiagnosticCodeCatalogView {
  const DiagnosticCodeDefinition* data;
  std::size_t size;
  const DiagnosticCodeDefinition* begin() const noexcept { return data; }
  const DiagnosticCodeDefinition* end() const noexcept { return data+size; }
};

// Registration data only: not a renderer, policy, runtime row identity,
// activation epoch, transaction decision, or authority to emit a vector.
// not_specified is retained verbatim and must be resolved by an owning
// specialized contract before the corresponding runtime behavior is admitted.
DiagnosticCodeCatalogView CanonicalDiagnosticCodeCatalog() noexcept;
const DiagnosticCodeDefinition* FindCanonicalDiagnosticCode(std::string_view code) noexcept;
const std::array<std::uint8_t,32>& CanonicalDiagnosticCodeSourceSha256() noexcept;
} // namespace scratchbird::core::diagnostics

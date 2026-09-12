// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "canonical_diagnostic_catalog.hpp"
#include <algorithm>

namespace scratchbird::core::diagnostics {
namespace {
#include "canonical_diagnostic_catalog_data.inc"
}
DiagnosticCodeCatalogView CanonicalDiagnosticCodeCatalog() noexcept {
  return {kDefinitions.data(),kDefinitions.size()};
}
const DiagnosticCodeDefinition* FindCanonicalDiagnosticCode(std::string_view code) noexcept {
  const auto found=std::lower_bound(kDefinitions.begin(),kDefinitions.end(),code,
      [](const DiagnosticCodeDefinition& row,std::string_view key){return row.code<key;});
  return found!=kDefinitions.end() && found->code==code ? &*found : nullptr;
}
const std::array<std::uint8_t,32>& CanonicalDiagnosticCodeSourceSha256() noexcept {
  return kSourceSha256;
}
} // namespace scratchbird::core::diagnostics

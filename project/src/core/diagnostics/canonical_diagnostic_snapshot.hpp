// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "canonical_diagnostic_catalog.hpp"
#include "../platform/runtime_platform.hpp"

#include <optional>
#include <string>

namespace scratchbird::core::diagnostics {

// Owned registration facts for trusted in-process transport, not a validated
// DiagnosticVector or permission to disclose, retry, or finalize a transaction.
// Unspecified registry fields remain unspecified; absence means unregistered.
struct CanonicalDiagnosticMetadata {
  std::string code;
  CanonicalSeverity severity = CanonicalSeverity::internal;
  bool is_failure = false;
  std::string sqlstate;
  std::string numeric_binding;
  std::string retry_class;
  std::string required_outcome;
  std::string diagnostic_class;
};

inline std::optional<CanonicalDiagnosticMetadata>
CaptureCanonicalDiagnosticMetadata(std::string_view code) {
  const auto* row = FindCanonicalDiagnosticCode(code);
  if (row == nullptr) return std::nullopt;
  return CanonicalDiagnosticMetadata{
      std::string(row->code), row->severity, row->is_failure, std::string(row->sqlstate),
      std::string(row->numeric_binding), std::string(row->retry_class),
      std::string(row->required_outcome), std::string(row->diagnostic_class)};
}

// Preserve a lower-level source separately from the owning API diagnostic.
// A security wrapper must not be replaced by its private storage cause. Native
// Status and canonical metadata are distinct representations, never enum casts.
struct NativeDiagnosticSource {
  platform::DiagnosticRecord record;
  std::optional<CanonicalDiagnosticMetadata> canonical_metadata;
};

}  // namespace scratchbird::core::diagnostics

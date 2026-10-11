// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "canonical_diagnostic_catalog.hpp"
#include "../platform/runtime_platform.hpp"

#include <optional>
#include <array>
#include <cstdint>
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

// Owned copy of a bounded datatype fact. Numeric and UUID operands are data,
// including rejected inputs, not strings or newly admitted system identities.
enum class NativeDatatypeParameterKind : std::uint8_t {
  none = 0, unsigned_u64 = 1, signed_i64 = 2, uuid = 3, token = 4
};
struct NativeDatatypeDiagnosticParameter {
  NativeDatatypeParameterKind kind = NativeDatatypeParameterKind::none;
  std::string name;
  std::uint64_t unsigned_value = 0;
  std::int64_t signed_value = 0;
  platform::Uuid uuid_value;
  std::string token_value;
  bool operator==(const NativeDatatypeDiagnosticParameter&) const = default;
};
struct NativeDatatypeDiagnosticFact {
  platform::Status status;
  std::string diagnostic_code;
  std::string detail;
  std::array<NativeDatatypeDiagnosticParameter, 4> parameters;
  std::uint8_t parameter_count = 0;
};

// Preserve a lower-level source separately from the owning API diagnostic.
// A security wrapper must not be replaced by its private storage cause. Native
// Status and canonical metadata are distinct representations, never enum casts.
struct NativeDiagnosticSource {
  platform::DiagnosticRecord record;
  std::optional<CanonicalDiagnosticMetadata> canonical_metadata;
  // May precede a dominant compensation/cleanup error in record. Keeping this
  // cause never replaces that error, its occurrence or recovery obligation.
  std::optional<NativeDatatypeDiagnosticFact> datatype_cause;
};

}  // namespace scratchbird::core::diagnostics

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/engine.h"
#include "core/diagnostics/canonical_diagnostic_snapshot.hpp"

#include <cstddef>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::server_engine_bridge {

// Private, family-neutral server-to-engine diagnostic transport. Structured
// This owned snapshot is private; it does not reinterpret public C ABI bytes.
struct EngineDiagnosticField {
  std::string key;
  std::string value;
};

struct EngineDiagnosticSnapshot {
  std::array<std::uint8_t, 16> occurrence_uuid{};
  std::uint32_t numeric_code = 0;
  sb_engine_diagnostic_severity_t severity = SB_ENGINE_DIAGNOSTIC_ERROR;
  std::string code;
  std::string message_key;
  // Private source detail, not permission to disclose it to a caller.
  std::string safe_detail;
  std::vector<EngineDiagnosticField> fields;
  // Exact trusted metadata. The C ABI view preserves the same severity class
  // through its independently numbered enum, not by an enum cast.
  // Neither this metadata nor the native cause authorizes parser disclosure.
  std::optional<scratchbird::core::diagnostics::CanonicalDiagnosticMetadata>
      canonical_metadata;
  std::optional<scratchbird::core::diagnostics::NativeDiagnosticSource>
      native_source;
};

// Registration consistency only, never source/channel or disclosure authority.
// In particular the wrapper numeric status need not equal a registry binding,
// and a private native cause does not replace the owning diagnostic.
inline bool IsConsistentEngineDiagnosticSnapshot(
    const EngineDiagnosticSnapshot& source) noexcept {
  if (source.code.empty() || source.message_key.empty() ||
      (source.occurrence_uuid[6] & 0xf0) != 0x70 ||
      (source.occurrence_uuid[8] & 0xc0) != 0x80) return false;
  switch (source.severity) {
    case SB_ENGINE_DIAGNOSTIC_INFO: case SB_ENGINE_DIAGNOSTIC_WARNING:
    case SB_ENGINE_DIAGNOSTIC_ERROR: case SB_ENGINE_DIAGNOSTIC_FATAL:
    case SB_ENGINE_DIAGNOSTIC_SECURITY: case SB_ENGINE_DIAGNOSTIC_CRITICAL:
    case SB_ENGINE_DIAGNOSTIC_CORRUPTION: case SB_ENGINE_DIAGNOSTIC_PANIC:
    case SB_ENGINE_DIAGNOSTIC_DEBUG: case SB_ENGINE_DIAGNOSTIC_NOTICE:
    case SB_ENGINE_DIAGNOSTIC_AUDIT: case SB_ENGINE_DIAGNOSTIC_SUPPORT:
    case SB_ENGINE_DIAGNOSTIC_INTERNAL: break;
    default: return false;
  }
  const auto* registered = core::diagnostics::FindCanonicalDiagnosticCode(source.code);
  if (!registered) return !source.canonical_metadata;
  if (!source.canonical_metadata) return false;
  const auto& metadata = *source.canonical_metadata;
  if (metadata.code != registered->code || metadata.severity != registered->severity ||
      metadata.is_failure != registered->is_failure || metadata.sqlstate != registered->sqlstate ||
      metadata.numeric_binding != registered->numeric_binding ||
      metadata.retry_class != registered->retry_class ||
      metadata.required_outcome != registered->required_outcome ||
      metadata.diagnostic_class != registered->diagnostic_class) return false;
  using S = core::diagnostics::CanonicalSeverity;
  switch (metadata.severity) {
    case S::informational: return source.severity == SB_ENGINE_DIAGNOSTIC_INFO;
    case S::warning: return source.severity == SB_ENGINE_DIAGNOSTIC_WARNING;
    case S::error: return source.severity == SB_ENGINE_DIAGNOSTIC_ERROR;
    case S::fatal: return source.severity == SB_ENGINE_DIAGNOSTIC_FATAL;
    case S::security: return source.severity == SB_ENGINE_DIAGNOSTIC_SECURITY;
    case S::critical: return source.severity == SB_ENGINE_DIAGNOSTIC_CRITICAL;
    case S::corruption: return source.severity == SB_ENGINE_DIAGNOSTIC_CORRUPTION;
    case S::panic: return source.severity == SB_ENGINE_DIAGNOSTIC_PANIC;
    case S::debug: return source.severity == SB_ENGINE_DIAGNOSTIC_DEBUG;
    case S::notice: return source.severity == SB_ENGINE_DIAGNOSTIC_NOTICE;
    case S::audit: return source.severity == SB_ENGINE_DIAGNOSTIC_AUDIT;
    case S::support: return source.severity == SB_ENGINE_DIAGNOSTIC_SUPPORT;
    case S::internal: return source.severity == SB_ENGINE_DIAGNOSTIC_INTERNAL;
  }
  return false;
}

// The result must remain alive during the copy. On any failure the output is
// unchanged; a successful snapshot remains valid after result release.
bool CopyEngineDiagnosticSnapshot(
    sb_engine_result_t result,
    std::size_t diagnostic_index,
    EngineDiagnosticSnapshot* snapshot);

bool CopyEngineDiagnosticFields(
    sb_engine_result_t result,
    std::size_t diagnostic_index,
    std::vector<EngineDiagnosticField>* fields);

}  // namespace scratchbird::server_engine_bridge

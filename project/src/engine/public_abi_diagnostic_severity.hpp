// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "scratchbird/engine/error.h"
#include "core/diagnostics/canonical_diagnostic_snapshot.hpp"

namespace scratchbird::engine {

// Classification only. Neither failure/finality nor disclosure is derived here.
// The owning metadata is selected by the publisher before this projection;
// a private native cause is deliberately not an input.
inline sb_engine_diagnostic_severity_t PublicDiagnosticSeverity(
    const std::optional<core::diagnostics::CanonicalDiagnosticMetadata>& metadata,
    sb_engine_diagnostic_severity_t absent_metadata_severity) noexcept {
  if (!metadata) return absent_metadata_severity;
  using S = core::diagnostics::CanonicalSeverity;
  switch (metadata->severity) {
    case S::informational: return SB_ENGINE_DIAGNOSTIC_INFO;
    case S::warning: return SB_ENGINE_DIAGNOSTIC_WARNING;
    case S::error: return SB_ENGINE_DIAGNOSTIC_ERROR;
    case S::fatal: return SB_ENGINE_DIAGNOSTIC_FATAL;
    case S::security: return SB_ENGINE_DIAGNOSTIC_SECURITY;
    case S::critical: return SB_ENGINE_DIAGNOSTIC_CRITICAL;
    case S::corruption: return SB_ENGINE_DIAGNOSTIC_CORRUPTION;
    case S::panic: return SB_ENGINE_DIAGNOSTIC_PANIC;
    case S::debug: return SB_ENGINE_DIAGNOSTIC_DEBUG;
    case S::notice: return SB_ENGINE_DIAGNOSTIC_NOTICE;
    case S::audit: return SB_ENGINE_DIAGNOSTIC_AUDIT;
    case S::support: return SB_ENGINE_DIAGNOSTIC_SUPPORT;
    case S::internal: return SB_ENGINE_DIAGNOSTIC_INTERNAL;
  }
  // Do not label malformed source metadata informational or invent its class.
  // Canonical-vector validation remains mandatory at its publication boundary.
  return SB_ENGINE_DIAGNOSTIC_INTERNAL;
}

}  // namespace scratchbird::engine

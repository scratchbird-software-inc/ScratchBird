// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/public_abi_diagnostic_severity.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <new>

namespace { bool allocation_forbidden = false; }
void* operator new(std::size_t n) {
  if (allocation_forbidden) throw std::bad_alloc();
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
namespace d = scratchbird::core::diagnostics;
using S = d::CanonicalSeverity;
using scratchbird::engine::PublicDiagnosticSeverity;
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) { ++failures; std::cerr << message << '\n'; }
}
struct Binding { S source; sb_engine_diagnostic_severity_t target; unsigned number; };
// Independent table of semantic and numeric public bindings, not enum arithmetic.
constexpr std::array<Binding, 13> bindings{{
  {S::informational, SB_ENGINE_DIAGNOSTIC_INFO, 0},
  {S::warning, SB_ENGINE_DIAGNOSTIC_WARNING, 1},
  {S::error, SB_ENGINE_DIAGNOSTIC_ERROR, 2},
  {S::fatal, SB_ENGINE_DIAGNOSTIC_FATAL, 3},
  {S::security, SB_ENGINE_DIAGNOSTIC_SECURITY, 4},
  {S::critical, SB_ENGINE_DIAGNOSTIC_CRITICAL, 5},
  {S::corruption, SB_ENGINE_DIAGNOSTIC_CORRUPTION, 6},
  {S::panic, SB_ENGINE_DIAGNOSTIC_PANIC, 7},
  {S::debug, SB_ENGINE_DIAGNOSTIC_DEBUG, 8},
  {S::notice, SB_ENGINE_DIAGNOSTIC_NOTICE, 9},
  {S::audit, SB_ENGINE_DIAGNOSTIC_AUDIT, 10},
  {S::support, SB_ENGINE_DIAGNOSTIC_SUPPORT, 11},
  {S::internal, SB_ENGINE_DIAGNOSTIC_INTERNAL, 12}
}};
}

int main() {
  std::optional<d::CanonicalDiagnosticMetadata> metadata{std::in_place};
  for (const auto& binding : bindings) {
    Check(static_cast<unsigned>(binding.target) == binding.number, "public enum binding changed");
    for (const auto& fallback : bindings) {
      for (const bool failure : {false, true}) {
        metadata->severity = binding.source;
        metadata->is_failure = failure;
        allocation_forbidden = true;
        const auto projected = PublicDiagnosticSeverity(metadata, fallback.target);
        allocation_forbidden = false;
        Check(projected == binding.target, "severity was flattened or inferred from failure/fallback");
      }
    }
  }
  for (const auto& fallback : bindings) {
    allocation_forbidden = true;
    const auto projected = PublicDiagnosticSeverity(std::nullopt, fallback.target);
    allocation_forbidden = false;
    Check(projected == fallback.target, "absent metadata changed explicit classification");
  }
  for (unsigned value = 0; value <= 255; ++value) {
    if (value >= 1 && value <= 13) continue;
    metadata->severity = static_cast<S>(value);
    allocation_forbidden = true;
    const auto projected = PublicDiagnosticSeverity(metadata, SB_ENGINE_DIAGNOSTIC_INFO);
    allocation_forbidden = false;
    Check(projected == SB_ENGINE_DIAGNOSTIC_INTERNAL, "malformed source severity acquired another class");
  }
  const auto catalog = d::CanonicalDiagnosticCodeCatalog();
  Check(catalog.size == 1397, "exact canonical registry not exercised");
  for (const auto& row : catalog) {
    metadata = d::CaptureCanonicalDiagnosticMetadata(row.code);
    bool found = false;
    for (const auto& binding : bindings) {
      if (binding.source != row.severity) continue;
      found = true;
      allocation_forbidden = true;
      const auto projected = PublicDiagnosticSeverity(metadata, SB_ENGINE_DIAGNOSTIC_ERROR);
      allocation_forbidden = false;
      Check(projected == binding.target, "registered source lost severity at public projection");
    }
    Check(found, "registry severity has no public class");
  }
  // Security wrapper classification must not become its private storage cause.
  metadata = d::CaptureCanonicalDiagnosticMetadata("DIAG.REDACTION_POLICY_INVALID");
  const auto native = d::CaptureCanonicalDiagnosticMetadata("STORAGE.PAGE_CHECKSUM_FAILED");
  Check(metadata && native && native->severity == S::corruption &&
        PublicDiagnosticSeverity(metadata, SB_ENGINE_DIAGNOSTIC_ERROR) == SB_ENGINE_DIAGNOSTIC_SECURITY,
        "owning security classification not retained");
  Check(!d::CaptureCanonicalDiagnosticMetadata("unregistered_error_fatal_security") &&
        PublicDiagnosticSeverity(std::nullopt, SB_ENGINE_DIAGNOSTIC_ERROR) == SB_ENGINE_DIAGNOSTIC_ERROR,
        "unknown code acquired inferred metadata");
  std::cout << "public_diagnostic_severity checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}

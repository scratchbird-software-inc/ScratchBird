// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "server/diagnostics.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <new>

namespace { long fail_after = -1; unsigned consumed_faults = 0; }
void* operator new(std::size_t n) {
  if (fail_after == 0) { ++consumed_faults; throw std::bad_alloc(); }
  if (fail_after > 0) --fail_after;
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
namespace d = scratchbird::core::diagnostics;
namespace b = scratchbird::server_engine_bridge;
namespace s = scratchbird::server;
using S = d::CanonicalSeverity;
using Snapshot = b::EngineDiagnosticSnapshot;
unsigned checks = 0, failures = 0;
void Check(bool condition, const char* why) {
  ++checks;
  if (!condition && ++failures <= 20) std::cerr << why << '\n';
}
// Independent semantic bindings, not a cast of the separately numbered enums.
constexpr std::array<std::pair<S, sb_engine_diagnostic_severity_t>, 13> bindings{{
  {S::informational, SB_ENGINE_DIAGNOSTIC_INFO}, {S::warning, SB_ENGINE_DIAGNOSTIC_WARNING},
  {S::error, SB_ENGINE_DIAGNOSTIC_ERROR}, {S::fatal, SB_ENGINE_DIAGNOSTIC_FATAL},
  {S::security, SB_ENGINE_DIAGNOSTIC_SECURITY}, {S::critical, SB_ENGINE_DIAGNOSTIC_CRITICAL},
  {S::corruption, SB_ENGINE_DIAGNOSTIC_CORRUPTION}, {S::panic, SB_ENGINE_DIAGNOSTIC_PANIC},
  {S::debug, SB_ENGINE_DIAGNOSTIC_DEBUG}, {S::notice, SB_ENGINE_DIAGNOSTIC_NOTICE},
  {S::audit, SB_ENGINE_DIAGNOSTIC_AUDIT}, {S::support, SB_ENGINE_DIAGNOSTIC_SUPPORT},
  {S::internal, SB_ENGINE_DIAGNOSTIC_INTERNAL}
}};
Snapshot Source(const d::DiagnosticCodeDefinition& row) {
  Snapshot result;
  result.occurrence_uuid = {0x01,0x9e,0x15,0x0f,0,0,0x70,0,0x80,0,0,0,0,0,0,0x23};
  result.code = row.code;
  result.message_key = "fixture.emission.key.not.a.public.template";
  result.canonical_metadata = d::CaptureCanonicalDiagnosticMetadata(row.code);
  for (const auto& [source, public_value] : bindings)
    if (source == row.severity) result.severity = public_value;
  result.numeric_code = 987654321; // Wrapper status is not native numeric binding.
  result.safe_detail = "private detail must not enter a public renderer";
  result.fields = {{"private-path", "/private/catalog/path"}};
  return result;
}
s::ServerDiagnostic Target(const std::string& code) {
  s::ServerDiagnostic result;
  result.code = code;
  result.message_key = "server.retained.public.key";
  result.safe_message = "unchanged safe presentation";
  result.fields = {{"safe", "retained"}};
  result.retryable = false;
  result.internal_audit_key = "retained-audit";
  return result;
}
bool Untouched(const s::ServerDiagnostic& a, const s::ServerDiagnostic& before) {
  return a.code == before.code && a.message_key == before.message_key &&
      a.safe_message == before.safe_message && a.occurrence_uuid == before.occurrence_uuid &&
      a.fields.size() == 1 && a.fields[0].key == "safe" && a.fields[0].value == "retained" &&
      a.internal_audit_key == before.internal_audit_key && a.retryable == before.retryable &&
      a.engine_source_snapshot.has_value() == before.engine_source_snapshot.has_value();
}
void Refused(const Snapshot& input) {
  auto target = Target(input.code);
  const auto before = target;
  Check(!b::IsConsistentEngineDiagnosticSnapshot(input), "inconsistent source admitted");
  Check(!s::AdoptEngineDiagnosticSource(input, &target), "server adopted inconsistent source");
  Check(Untouched(target, before), "source refusal changed destination");
}
void RegistryCases() {
  const auto catalog = d::CanonicalDiagnosticCodeCatalog();
  Check(catalog.size == 1397, "canonical registry coverage changed; update reviewed oracle count");
  for (const auto& row : catalog) {
    const auto input = Source(row);
    const auto previous_faults = consumed_faults;
    fail_after = 0;
    const bool consistent = b::IsConsistentEngineDiagnosticSnapshot(input);
    fail_after = -1;
    Check(consistent && consumed_faults == previous_faults, "registry check allocated or refused exact row");
    auto target = Target(input.code);
    const auto before = target;
    Check(s::AdoptEngineDiagnosticSource(input, &target), "exact registered source refused");
    Check(target.engine_source_snapshot && target.occurrence_uuid == input.occurrence_uuid &&
          target.engine_source_snapshot->numeric_code == input.numeric_code &&
          target.engine_source_snapshot->canonical_metadata->retry_class == row.retry_class &&
          target.engine_source_snapshot->canonical_metadata->required_outcome == row.required_outcome &&
          target.message_key == before.message_key && target.safe_message == before.safe_message &&
          target.fields[0].value == "retained", "exact facts or separate presentation lost");
    for (const auto& [ignored, alternative] : bindings) {
      (void)ignored;
      if (alternative == input.severity) continue;
      auto bad = input;
      bad.severity = alternative;
      Refused(bad);
    }
    for (unsigned field = 0; field < 11; ++field) {
      auto bad = input;
      auto& m = *bad.canonical_metadata;
      switch (field) {
        case 0: m.code += ".contradiction"; break;
        case 1: m.severity = static_cast<S>(255); break;
        case 2: m.is_failure = !m.is_failure; break;
        case 3: m.sqlstate += "X"; break;
        case 4: m.numeric_binding += "X"; break;
        case 5: m.retry_class += "X"; break;
        case 6: m.required_outcome += "X"; break;
        case 7: m.diagnostic_class += "X"; break;
        case 8: bad.canonical_metadata.reset(); break;
        case 9: bad.message_key.clear(); break;
        case 10: bad.code += ".unregistered"; break;
      }
      Refused(bad);
    }
  }
}
void PrivateAndFaultCases() {
  const auto* row = d::FindCanonicalDiagnosticCode("DIAG.REDACTION_POLICY_INVALID");
  Check(row != nullptr, "registered security fixture");
  if (!row) return;
  auto input = Source(*row);
  input.native_source.emplace();
  input.native_source->record.diagnostic_code = "private.native.cause";
  input.native_source->record.message_key = "private.native.key";
  input.native_source->canonical_metadata = d::CaptureCanonicalDiagnosticMetadata("ENGINE.ABI.PARAMETER_NULL");
  auto target = Target(input.code);
  Check(s::AdoptEngineDiagnosticSource(input, &target) && target.engine_source_snapshot &&
      target.engine_source_snapshot->native_source->record.message_key == "private.native.key" &&
      target.engine_source_snapshot->severity == SB_ENGINE_DIAGNOSTIC_SECURITY,
      "native cause replaced owner or was lost");
  const auto rendered = s::ToMessageVectorJsonLine(target);
  Check(rendered.find("private.native.key") == std::string::npos &&
        rendered.find("/private/catalog/path") == std::string::npos &&
        rendered.find(input.message_key) == std::string::npos,
        "private source copied into legacy public text");
  Check(target.engine_source_snapshot &&
        s::AdoptEngineDiagnosticSource(*target.engine_source_snapshot, &target) &&
        target.engine_source_snapshot->fields[0].value == "/private/catalog/path",
        "source aliasing the destination was damaged");
  for (unsigned version = 0; version < 16; ++version) {
    for (unsigned variant = 0; variant < 4; ++variant) {
      auto changed = input;
      changed.occurrence_uuid[6] = static_cast<unsigned char>(version << 4);
      changed.occurrence_uuid[8] = static_cast<unsigned char>(variant << 6);
      if (version == 7 && variant == 2) Check(b::IsConsistentEngineDiagnosticSnapshot(changed), "UUIDv7 refused");
      else Refused(changed);
    }
  }
  auto unknown = input;
  unknown.code = "PRIVATE.UNREGISTERED.SNAPSHOT.CONSISTENCY.FIXTURE";
  unknown.canonical_metadata.reset();
  Check(d::FindCanonicalDiagnosticCode(unknown.code) == nullptr, "private code became registered");
  for (const auto& [ignored, severity] : bindings) {
    (void)ignored;
    unknown.severity = severity;
    auto out = Target(unknown.code);
    Check(s::AdoptEngineDiagnosticSource(unknown, &out) && out.engine_source_snapshot &&
          !out.engine_source_snapshot->canonical_metadata && out.engine_source_snapshot->severity == severity,
          "unregistered private source acquired invented facts");
  }
  // 13 is outside the defined public C enum, within its underlying integer range.
  unknown.severity = static_cast<sb_engine_diagnostic_severity_t>(13);
  Refused(unknown);
  auto mismatched = Target("DIFFERENT.OWNER");
  const auto retained = mismatched;
  Check(!s::AdoptEngineDiagnosticSource(input, &mismatched) && Untouched(mismatched, retained),
        "different owner accepted");
  Check(!s::AdoptEngineDiagnosticSource(input, nullptr), "null destination accepted");
  bool completed = false;
  for (long fail = 0; fail < 256; ++fail) {
    target = Target(input.code);
    const auto before = target;
    fail_after = fail;
    const auto accepted = s::AdoptEngineDiagnosticSource(input, &target);
    fail_after = -1;
    if (accepted) {
      Check(target.engine_source_snapshot && target.engine_source_snapshot->native_source &&
            target.engine_source_snapshot->fields[0].value == "/private/catalog/path",
            "successful fault-campaign copy lost source");
      completed = true;
      break;
    }
    Check(Untouched(target, before), "allocation failure partially changed destination");
  }
  Check(completed && consumed_faults >= 8, "incomplete actual adoption fault campaign");
}
} // namespace
int main() {
  try {
    RegistryCases();
    PrivateAndFaultCases();
    std::cout << "checks=" << checks << " faults=" << consumed_faults << " failures=" << failures << '\n';
    return failures ? 1 : 0;
  } catch (const std::exception& error) {
    fail_after = -1;
    std::cerr << error.what() << '\n';
    return 2;
  }
}

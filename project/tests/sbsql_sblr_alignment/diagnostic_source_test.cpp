// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "api_diagnostics.hpp"
#include "copy_on_write.hpp"

#include <cstdlib>
#include <iostream>
#include <new>
#include <type_traits>

namespace { long fail_after = -1; }
void* operator new(std::size_t n) {
  if (fail_after == 0) throw std::bad_alloc();
  if (fail_after > 0) --fail_after;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace api = scratchbird::engine::internal_api;
namespace d = scratchbird::core::diagnostics;
namespace p = scratchbird::core::platform;
namespace mga = scratchbird::transaction::mga;
namespace {
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok && ++failures < 20) std::cerr << message << '\n';
}
bool Metadata(const std::optional<d::CanonicalDiagnosticMetadata>& value,
              const d::DiagnosticCodeDefinition& row) {
  return value && value->code == row.code && value->severity == row.severity &&
      value->is_failure == row.is_failure && value->sqlstate == row.sqlstate &&
      value->numeric_binding == row.numeric_binding && value->retry_class == row.retry_class &&
      value->required_outcome == row.required_outcome &&
      value->diagnostic_class == row.diagnostic_class;
}
bool Record(const p::DiagnosticRecord& a, const p::DiagnosticRecord& b) {
  if (a.status.code != b.status.code || a.status.severity != b.status.severity ||
      a.status.subsystem != b.status.subsystem || a.diagnostic_code != b.diagnostic_code ||
      a.message_key != b.message_key || a.arguments.size() != b.arguments.size() ||
      a.trace_id != b.trace_id || a.source_component != b.source_component ||
      a.remediation_hint != b.remediation_hint) return false;
  for (std::size_t i = 0; i < a.arguments.size(); ++i)
    if (a.arguments[i].key != b.arguments[i].key || a.arguments[i].value != b.arguments[i].value)
      return false;
  return true;
}
bool V7(const std::array<std::uint8_t,16>& value) {
  return (value[6] & 0xf0) == 0x70 && (value[8] & 0xc0) == 0x80;
}
}

int main() {
  static_assert(std::is_nothrow_move_assignable_v<api::EngineApiDiagnostic>);
  // Full current registration population: copying facts is the tested contract,
  // not proof that every registered operation emits a complete valid vector.
  const auto catalog = d::CanonicalDiagnosticCodeCatalog();
  Check(catalog.size == 1378, "expected registration population changed");
  std::size_t copied = 0;
  for (const auto& row : catalog) {
    auto value = api::MakeEngineApiDiagnostic(std::string(row.code),
        "test.private.source", "retry timeout FATAL WARNING", row.is_failure);
    Check(Metadata(value.canonical_metadata, row), "registered source metadata lost");
    Check(value.error == row.is_failure && !value.native_source,
          "source failure or native absence changed");
    Check(V7(value.occurrence_uuid), "source is not binary UUIDv7");
    const auto duplicate = value;
    Check(duplicate.occurrence_uuid == value.occurrence_uuid &&
              Metadata(duplicate.canonical_metadata, row), "copy changed identity or metadata");
    ++copied;
  }
  for (const auto* code : {"", "unregistered.timeout.retry.WARNING.FATAL",
                            "MGA.COW.INVALID_KIND.extra"}) {
    const auto value = api::MakeEngineApiDiagnostic(code, "test.key", "retry");
    Check(!value.canonical_metadata && !value.native_source,
          "unknown source manufactured metadata from text");
  }

  mga::TransactionInventoryEntry entry;
  entry.state = mga::TransactionState::read_only_active;
  const auto refused = mga::ValidateCopyOnWriteTransactionState(entry);
  Check(!refused.ok() && refused.diagnostic.diagnostic_code == "MGA.COW.READ_ONLY_TRANSACTION",
        "actual COW readonly guard changed");
  const auto before = refused.diagnostic;
  auto source = api::MakeEngineApiDiagnosticFromNative(before, before.diagnostic_code,
                                                       before.message_key, "batch_rows=2");
  Check(source.error && source.native_source && Record(source.native_source->record, before),
        "native COW status or fields lost");
  Check(source.canonical_metadata && source.canonical_metadata->sqlstate == "25006" &&
        source.canonical_metadata->numeric_binding == "101014" &&
        source.canonical_metadata->retry_class == "never_retry_without_input_or_authority_change" &&
        source.canonical_metadata->required_outcome == "reject_without_mutation_preserve_inventory",
        "exact COW condition semantics lost");
  Check(Record(refused.diagnostic, before), "conversion mutated native input");
  auto wrapped = api::MakeEngineApiDiagnosticFromNative(before, "SECURITY.ACCESS_DENIED",
                                                        "test.security.wrapper", "private operation");
  Check(wrapped.code == "SECURITY.ACCESS_DENIED" && wrapped.detail == "private operation" &&
        wrapped.native_source && Record(wrapped.native_source->record, before) &&
        wrapped.canonical_metadata && wrapped.canonical_metadata->code == wrapped.code &&
        wrapped.native_source->canonical_metadata &&
        wrapped.native_source->canonical_metadata->code == before.diagnostic_code,
        "wrapper collapsed into native cause or source code flattened into text");
  Check(wrapped.fields.empty(), "native private source leaked into public fields");
  const auto retained = wrapped;
  if (wrapped.native_source) {
    wrapped.native_source->record.arguments.clear();
    wrapped.native_source->record.message_key = "changed";
  }
  Check(retained.native_source && Record(retained.native_source->record, before),
        "native source copy aliases mutable input");

  auto info = before;
  info.status = {p::StatusCode::ok, p::Severity::info, p::Subsystem::transaction_mga};
  info.diagnostic_code = "MGA.COW.CLEANUP_AUTHORITY_REQUIRED";
  auto assessment = api::MakeEngineApiDiagnosticFromNative(info, info.diagnostic_code,
                                                         "test.assessment", "timeout retry", false);
  Check(!assessment.error && assessment.canonical_metadata &&
        assessment.canonical_metadata->severity == d::CanonicalSeverity::informational &&
        assessment.canonical_metadata->required_outcome == "retain_versions_require_cleanup_authority",
        "informational assessment turned into failure or reclaim permission");
  const auto operation_failed = api::MakeEngineApiDiagnosticFromNative(
      info, "SECURITY.ACCESS_DENIED", "test.owner.failure", "unchanged owner result", true);
  Check(operation_failed.error && operation_failed.native_source &&
        operation_failed.native_source->record.status.ok(),
        "native assessment changed owning operation failure");

  unsigned allocation_failures = 0;
  bool completed = false;
  for (long i = 0; i < 128; ++i) {
    auto output = retained;
    fail_after = i;
    try {
      output = api::MakeEngineApiDiagnosticFromNative(before, before.diagnostic_code,
                                                     before.message_key, "batch_rows=2");
      fail_after = -1;
      Check(output.native_source && Record(output.native_source->record, before),
            "successful allocation campaign changed native source");
      completed = true;
      break;
    } catch (const std::bad_alloc&) {
      fail_after = -1;
      ++allocation_failures;
      Check(output.occurrence_uuid == retained.occurrence_uuid &&
            output.code == retained.code && output.native_source &&
            Record(output.native_source->record, before), "allocation failure partially published source");
    }
  }
  Check(completed && allocation_failures >= 8, "native copy allocations not exercised");
  std::cout << "registered_cases=" << copied << " checks=" << checks
            << " allocation_failures=" << allocation_failures << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}

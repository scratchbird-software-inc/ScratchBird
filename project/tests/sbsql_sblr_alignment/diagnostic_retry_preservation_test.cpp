// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "server/diagnostics.hpp"
#include "server/server_observability.hpp"
#include "server/sbps.hpp"
#include "core/diagnostics/canonical_diagnostic_catalog.hpp"

#include <array>
#include <iostream>
#include <string>
#include <string_view>

namespace server = scratchbird::server;
namespace catalog = scratchbird::core::diagnostics;
namespace {
unsigned checks = 0, failures = 0;
void Check(bool condition, std::string_view message, std::string_view code) {
  ++checks;
  if (!condition && ++failures <= 20)
    std::cerr << message << ": " << code << '\n';
}
bool RetryFlag(const std::string& json, bool expected) {
  const std::string key = "\"retryable\":";
  const auto at = json.find(key);
  return at != std::string::npos &&
      json.find(key, at + key.size()) == std::string::npos &&
      json.compare(at + key.size(), expected ? 4 : 5, expected ? "true" : "false") == 0;
}
std::size_t render_cases = 0, lifecycle_cases = 0;
void Exercise(std::string_view code) {
  // Explicit source data is the oracle. This test does not authorize emission
  // of arbitrary code/flag pairs, nor evaluate their owning retry policy.
  for (const bool source_retry : {false, true}) {
    for (const auto severity : {server::ServerDiagnosticSeverity::kInfo,
                                server::ServerDiagnosticSeverity::kWarning,
                                server::ServerDiagnosticSeverity::kError}) {
      server::ServerDiagnostic diagnostic;
      diagnostic.code = code;
      diagnostic.message_key = "retry.timeout.stale.ack";
      diagnostic.safe_message = "retry timeout busy unavailable serialization";
      diagnostic.severity = severity;
      diagnostic.retryable = source_retry;
      const auto occurrence = diagnostic.occurrence_uuid;
      Check(RetryFlag(server::ToMessageVectorJsonLine(diagnostic), source_retry),
            "public renderer changed source retry", code);
      Check(RetryFlag(server::ToPrivateMessageVectorJsonLine(diagnostic), source_retry),
            "private renderer changed source retry", code);
      const auto wire = server::sbps::EncodeMessageVectorSet({diagnostic}, {});
      Check(wire.size() >= 176 && wire[64 + 108] == unsigned(source_retry),
            "legacy wire changed source retry", code);
      Check(diagnostic.retryable == source_retry && diagnostic.occurrence_uuid == occurrence,
            "rendering mutated source", code);
      ++render_cases;
    }
    // Actual lifecycle consumer and its retained event, not a fake recorder.
    // Empty persistence paths deliberately select in-memory observations only;
    // this is not durable audit/metrics acceptance.
    server::ServerObservabilityState state;
    server::ServerLifecycleObservabilityEvent event;
    event.operation_key = "open_database";
    event.outcome = "refused";
    event.diagnostic_code = code;
    event.retryable = source_retry;
    event.private_detail = "timeout retry stale";
    const auto record = server::RecordServerLifecycleObservability(&state, event);
    Check(record.recorded && state.lifecycle_events.size() == 1 &&
              state.lifecycle_events.front().retryable == source_retry,
          "lifecycle recorder changed source retry", code);
    Check(RetryFlag(record.message_vector_public_json, source_retry) &&
              RetryFlag(record.message_vector_private_json, source_retry),
          "lifecycle rendering changed source retry", code);
    Check(event.retryable == source_retry && state.audit_events.size() == 1,
          "lifecycle source changed or observation omitted", code);
    ++lifecycle_cases;
  }
}
}

int main() {
  const auto rows = catalog::CanonicalDiagnosticCodeCatalog();
  Check(rows.size == 1378, "registration population changed", "catalog");
  for (const auto& row : rows) Exercise(row.code);
  // Unknown, case-variant and substring collisions cannot manufacture policy.
  constexpr std::array<std::string_view, 12> bait = {
      "", "timeout", "busy", "retry", "stale", "unavailable", "drain",
      "ack", "lock_timeout", "serialization", "ROLLBACK_COMPLETE",
      "unregistered.no_retry_TIMEOUT"};
  for (const auto code : bait) Exercise(code);
  Check(render_cases == 8340 && lifecycle_cases == 2780,
        "expected finite tuple denominator changed", "matrix");
  std::cout << "registered_codes=" << rows.size << " bait_codes=" << bait.size()
            << " render_cases=" << render_cases << " lifecycle_cases=" << lifecycle_cases
            << " checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}

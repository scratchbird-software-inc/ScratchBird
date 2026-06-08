// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory_support_bundle.hpp"
#include "metric_producer.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace mem = scratchbird::core::memory;
namespace metrics = scratchbird::core::metrics;
namespace platform = scratchbird::core::platform;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool EvidenceHas(const std::vector<std::string>& evidence, std::string_view token) {
  for (const auto& row : evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool RowValueContains(const mem::MemorySupportBundleResult& result,
                      std::string_view needle) {
  for (const auto& row : result.rows) {
    if (row.value.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool HasRowKey(const mem::MemorySupportBundleResult& result,
               std::string_view key) {
  for (const auto& row : result.rows) {
    if (row.key == key) {
      return true;
    }
  }
  return false;
}

mem::MemoryAccountingSnapshot Snapshot() {
  mem::MemoryAccountingSnapshot snapshot;
  snapshot.current_bytes = 100;
  snapshot.peak_bytes = 200;
  snapshot.allocation_count = 4;
  snapshot.deallocation_count = 2;
  snapshot.failure_count = 1;
  snapshot.leak_candidate_count = 2;
  snapshot.page_buffer_current_bytes = 16;
  snapshot.arena_current_bytes = 32;
  snapshot.contexts.push_back({"query", "query-secret-token-raw", 90, 120, 3, 1, 0, 2});
  snapshot.contexts.push_back({"session", "session-visible", 10, 20, 1, 1, 0, 0});
  snapshot.categories.push_back({mem::MemoryCategory::executor_query_reserved,
                                 80, 100, 2, 1, 0, 1});
  snapshot.categories.push_back({mem::MemoryCategory::diagnostics,
                                 20, 40, 2, 1, 1, 1});
  return snapshot;
}

platform::DiagnosticRecord Diagnostic() {
  return platform::MakeDiagnostic(platform::StatusCode::memory_limit_exceeded,
                                  platform::Severity::error,
                                  platform::Subsystem::memory,
                                  "memory_test_failure",
                                  "core.memory.test_failure",
                                  {{"reason", "query memory limit exceeded"},
                                   {"password", "cleartext-secret-password"},
                                   {"seed", "test-seed-should-redact"}},
                                  {},
                                  "memory_support_bundle_gate",
                                  {});
}

void SupportBundleRedactsAndReportsMemoryEvidence() {
  (void)metrics::SetGauge(
      "sb_memory_allocated_bytes",
      metrics::Labels({{"component", "core.memory"}, {"operation", "snapshot"}}),
      100.0,
      "core_memory");

  mem::MemorySupportBundleRequest request;
  request.snapshot = Snapshot();
  request.diagnostics.push_back(Diagnostic());
  request.metrics = metrics::DefaultMetricRegistry().SnapshotCurrent();
  request.max_top_contexts = 2;
  request.max_top_categories = 2;
  request.allow_protected_material = false;

  const auto result = mem::BuildMemorySupportBundleEvidence(std::move(request));
  Require(result.ok(), "MMCH-080 memory support bundle failed");
  Require(EvidenceHas(result.evidence, "MMCH_MEMORY_SUPPORT_BUNDLE_EVIDENCE"),
          "MMCH-080 evidence marker missing");
  Require(EvidenceHas(
              result.evidence,
              "memory_support_bundle.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_wal_or_benchmark_authority"),
          "MMCH-080 authority boundary evidence missing");
  Require(result.top_context_count == 2 && result.top_category_count == 2,
          "MMCH-080 top context/category counts missing");
  Require(result.failure_reason_count == 1 && result.metric_count > 0,
          "MMCH-080 failure or metric rows missing");
  Require(result.redacted_row_count >= 3,
          "MMCH-080 protected material was not redacted");
  Require(HasRowKey(result, "snapshot.leak_candidate_count"),
          "MMCH-080 leak candidate row missing");
  Require(HasRowKey(result, "snapshot.peak_bytes"),
          "MMCH-080 high-water row missing");
  Require(!RowValueContains(result, "cleartext-secret-password") &&
              !RowValueContains(result, "test-seed-should-redact") &&
              !RowValueContains(result, "query-secret-token-raw"),
          "MMCH-080 protected material leaked into support bundle rows");
  for (const auto& row : result.rows) {
    Require(!row.tamper_evidence_digest.empty(),
            "MMCH-080 row missing tamper-evidence digest");
  }
}

}  // namespace

int main() {
  SupportBundleRedactsAndReportsMemoryEvidence();
  std::cout << "MMCH-080 authority_note=memory_support_bundle_evidence_only;"
            << " support_bundle_rows_are_not_finality_visibility_authorization_recovery_or_benchmark_authority\n";
  return EXIT_SUCCESS;
}

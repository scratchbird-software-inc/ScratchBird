// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-023 focused validation for bounded low-memory memory support bundles
// and protected-memory security review evidence.
#include "memory.hpp"
#include "memory_support_bundle.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

namespace memory = scratchbird::core::memory;
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

bool RowKeyHas(const memory::MemorySupportBundleResult& bundle,
               std::string_view token) {
  for (const auto& row : bundle.rows) {
    if (row.key.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool AnyRowValueHas(const memory::MemorySupportBundleResult& bundle,
                    std::string_view token) {
  for (const auto& row : bundle.rows) {
    if (row.value.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

memory::MemoryAccountingSnapshot Snapshot() {
  memory::MemoryAccountingSnapshot snapshot;
  snapshot.current_bytes = 8192;
  snapshot.peak_bytes = 16384;
  snapshot.allocation_count = 20;
  snapshot.deallocation_count = 12;
  snapshot.failure_count = 3;
  snapshot.policy_rejection_count = 2;
  snapshot.unknown_pointer_failure_count = 1;
  snapshot.leak_candidate_count = 1;
  snapshot.page_buffer_current_bytes = 2048;
  snapshot.arena_current_bytes = 4096;
  snapshot.contexts.push_back({"query", "query-secret-token-raw", 4096, 8192, 8, 4, 1, 4});
  snapshot.contexts.push_back({"session", "session-normal", 2048, 4096, 5, 3, 1, 2});
  snapshot.contexts.push_back({"page_cache", "page-cache-hot", 1024, 2048, 4, 4, 0, 0});
  snapshot.categories.push_back({memory::MemoryCategory::executor_query_reserved,
                                 4096, 8192, 8, 4, 1, 4});
  snapshot.categories.push_back({memory::MemoryCategory::page_buffer,
                                 2048, 4096, 4, 2, 1, 2});
  snapshot.categories.push_back({memory::MemoryCategory::diagnostics,
                                 1024, 2048, 3, 2, 1, 1});
  return snapshot;
}

platform::DiagnosticRecord SecretDiagnostic() {
  return platform::MakeDiagnostic(
      platform::StatusCode::memory_limit_exceeded,
      platform::Severity::error,
      platform::Subsystem::memory,
      "SB-MEMORY-CEIC-023-SECRET-DIAGNOSTIC",
      "memory.ceic_023.secret_diagnostic",
      {{"reason", "low memory under pressure"},
       {"password", "cleartext-secret-password"},
       {"seed", "test-seed-should-redact"},
       {"kms_plaintext", "kms_plaintext:raw-material"}});
}

memory::ProtectedBufferResult ProtectedAllocation(memory::MemoryManager* manager) {
  memory::ProtectedMemoryRequest request;
  request.bytes = 64;
  request.alignment = 16;
  request.material_class = "hsm_kms_plugin_secret_material";
  request.platform_policy = memory::ProtectedMemoryPlatformPolicy::best_effort;
  request.tag.owner = "ceic-023-protected-review";
  request.tag.context_id = "ceic-023-protected-context";
  request.tag.category = memory::MemoryCategory::resource_seed;
  auto result = manager->AllocateProtected(std::move(request));
  Require(result.ok(), "CEIC-023 protected allocation failed");
  Require(result.evidence.protected_material_redacted,
          "CEIC-023 protected allocation did not mark redaction");
  Require(result.evidence.zero_on_release,
          "CEIC-023 protected allocation missing zero-on-release evidence");
  Require(result.evidence.platform_lock_attempted,
          "CEIC-023 protected allocation missing lock attempt evidence");
  Require(result.evidence.no_dump_attempted,
          "CEIC-023 protected allocation missing no-dump attempt evidence");
  std::memset(result.buffer.data(), 0x5a, result.buffer.size());
  result.buffer.Zeroize();
  return result;
}

memory::MemorySupportBundleRequest BaseRequest(
    const memory::ProtectedMemoryEvidence& protected_evidence) {
  memory::MemorySupportBundleRequest request;
  request.mode = memory::MemorySupportBundleMode::low_memory;
  request.snapshot = Snapshot();
  request.diagnostics.push_back(SecretDiagnostic());
  request.metrics.push_back({"sb_memory_allocated_bytes", {}, metrics::MetricType::gauge, 8192.0});
  request.metrics.push_back({"sb_memory_secret_token_bytes", {}, metrics::MetricType::gauge, 7.0});
  request.pressure_transitions.push_back(
      {"NORMAL", "HIGH_PRESSURE", "high_threshold", 8192, 10000, false});
  request.pressure_transitions.push_back(
      {"HIGH_PRESSURE", "EMERGENCY_PRESSURE", "emergency_threshold", 9800, 10000, true});
  request.allocation_classes.push_back(
      {"query_scratch", 4096, 8192, 8, 4, 1});
  request.allocation_classes.push_back(
      {"protected", 64, 64, 1, 0, 0});
  request.limits.max_rows = 96;
  request.limits.max_output_bytes = 12ull * 1024ull;
  request.limits.max_value_bytes = 96;
  request.limits.max_diagnostic_arguments = 8;
  request.limits.max_metrics = 8;
  request.allow_protected_material = false;
  request.redaction_before_buffering = true;
  request.exclude_protected_material = true;
  request.protected_memory_review.enabled = true;
  request.protected_memory_review.diagnostics_log_exception_scan_complete = true;
  request.protected_memory_review.support_bundle_scan_complete = true;
  request.protected_memory_review.zeroization_not_optimized_away = true;
  request.protected_memory_review.protected_buffer_zero_on_release = true;
  request.protected_memory_review.hsm_kms_plugin_routes_use_protected_buffers = true;
  request.protected_memory_review.protected_buffer_evidence.push_back(protected_evidence);
  request.protected_memory_review.secret_routes.push_back(
      {"hsm", "hsm-reference-only-route", true, true, false});
  request.protected_memory_review.secret_routes.push_back(
      {"kms", "kms-reference-only-route", true, true, false});
  request.protected_memory_review.secret_routes.push_back(
      {"plugin", "plugin-secret-handle-route", true, true, false});
  return request;
}

void LowMemoryBundleIsBoundedRedactedAndNonAuthoritative() {
  memory::MemoryManager manager(memory::DefaultLocalEngineMemoryPolicy());
  auto protected_result = ProtectedAllocation(&manager);

  auto request = BaseRequest(protected_result.evidence);
  const auto bundle = memory::BuildMemorySupportBundleEvidence(std::move(request));
  Require(bundle.ok(), "CEIC-023 low-memory support bundle failed");
  Require(bundle.low_memory_mode, "CEIC-023 low-memory mode flag missing");
  Require(bundle.redaction_before_buffering,
          "CEIC-023 redaction-before-buffering flag missing");
  Require(bundle.protected_material_excluded,
          "CEIC-023 protected-material exclusion flag missing");
  Require(bundle.output_bytes <= bundle.output_byte_limit,
          "CEIC-023 output byte limit exceeded");
  Require(bundle.rows.size() <= bundle.row_limit,
          "CEIC-023 row limit exceeded");
  Require(bundle.top_context_count <= 4,
          "CEIC-023 low-memory top context cap not enforced");
  Require(bundle.pressure_transition_count == 2,
          "CEIC-023 pressure transition rows missing");
  Require(bundle.allocation_class_count == 2,
          "CEIC-023 allocation class rows missing");
  Require(bundle.protected_buffer_evidence_count == 1,
          "CEIC-023 protected buffer evidence missing");
  Require(bundle.protected_secret_route_count == 3,
          "CEIC-023 HSM/KMS/plugin protected route evidence missing");
  Require(bundle.protected_memory_review_passed,
          "CEIC-023 protected-memory security review did not pass");
  Require(bundle.redacted_row_count >= 4,
          "CEIC-023 expected protected rows to be redacted/excluded");
  Require(EvidenceHas(bundle.evidence, "CEIC-023_MEMORY_SUPPORT_BUNDLE_LOW_MEMORY"),
          "CEIC-023 evidence anchor missing");
  Require(EvidenceHas(bundle.evidence,
                      "CEIC-023_PROTECTED_MEMORY_SECURITY_REVIEW"),
          "CEIC-023 protected review evidence anchor missing");
  Require(EvidenceHas(
              bundle.evidence,
              "memory_support_bundle.authority_scope=evidence_only_not_transaction_finality_visibility_security_authorization_recovery_parser_donor_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority"),
          "CEIC-023 expanded authority boundary missing");
  Require(RowKeyHas(bundle, "pressure_transition.0.new_state"),
          "CEIC-023 pressure transition row absent");
  Require(RowKeyHas(bundle, "allocation_class.0.class"),
          "CEIC-023 allocation class row absent");
  Require(RowKeyHas(bundle, "protected_buffer.0.lock_attempted"),
          "CEIC-023 protected platform evidence row absent");
  Require(!AnyRowValueHas(bundle, "cleartext-secret-password") &&
              !AnyRowValueHas(bundle, "test-seed-should-redact") &&
              !AnyRowValueHas(bundle, "kms_plaintext:raw-material") &&
              !AnyRowValueHas(bundle, "query-secret-token-raw"),
          "CEIC-023 protected material leaked into bundle rows");
}

void EmergencySummaryUsesFixedBoundedRows() {
  memory::MemoryManager manager(memory::DefaultLocalEngineMemoryPolicy());
  auto protected_result = ProtectedAllocation(&manager);
  auto request = BaseRequest(protected_result.evidence);
  request.mode = memory::MemorySupportBundleMode::emergency_summary;
  request.limits.max_rows = 32;
  const auto summary = memory::BuildMemorySupportBundleEmergencySummary(request);
  Require(summary.emitted, "CEIC-023 emergency summary not emitted");
  Require(summary.allocation_free_model,
          "CEIC-023 emergency summary not allocation-free modeled");
  Require(summary.bounded, "CEIC-023 emergency summary not bounded");
  Require(summary.redaction_before_buffering,
          "CEIC-023 emergency summary missing redaction-before-buffering");
  Require(summary.protected_material_excluded,
          "CEIC-023 emergency summary missing protected-material exclusion");
  Require(summary.row_count <= summary.max_rows,
          "CEIC-023 emergency summary exceeded row cap");
  for (std::size_t i = 0; i < summary.row_count; ++i) {
    const std::string_view value(summary.rows[i].value.data());
    Require(value.find("query-secret-token-raw") == std::string_view::npos,
            "CEIC-023 emergency summary leaked protected context");
  }
  Require(std::string_view(summary.authority_scope.data()).find(
              "not_transaction_finality_visibility_security_authorization") !=
              std::string_view::npos,
          "CEIC-023 emergency summary authority boundary missing");
}

void UnsafeProtectedReviewFailsClosed() {
  memory::MemoryManager manager(memory::DefaultLocalEngineMemoryPolicy());
  auto protected_result = ProtectedAllocation(&manager);
  auto request = BaseRequest(protected_result.evidence);
  request.protected_memory_review.hsm_kms_plugin_routes_use_protected_buffers = false;
  request.protected_memory_review.secret_routes[1].routed_through_protected_buffer = false;
  request.protected_memory_review.secret_routes[1].plaintext_material_observed = true;
  const auto bundle = memory::BuildMemorySupportBundleEvidence(std::move(request));
  Require(!bundle.ok(), "CEIC-023 unsafe protected review was accepted");
  Require(EvidenceHas(bundle.evidence, "memory_support_bundle.fail_closed=true"),
          "CEIC-023 unsafe protected review missing fail-closed evidence");
}

void ZeroLimitsDoNotBecomeUnboundedOutput() {
  memory::MemoryManager manager(memory::DefaultLocalEngineMemoryPolicy());
  auto protected_result = ProtectedAllocation(&manager);
  auto request = BaseRequest(protected_result.evidence);
  request.limits.max_rows = 0;
  request.limits.max_output_bytes = 0;
  request.limits.max_key_bytes = 0;
  request.limits.max_value_bytes = 0;
  const auto bundle = memory::BuildMemorySupportBundleEvidence(std::move(request));
  Require(bundle.ok(), "CEIC-023 zero-limit bundle request failed unexpectedly");
  Require(bundle.row_limit == 1,
          "CEIC-023 zero row limit was not normalized to bounded one-row cap");
  Require(bundle.output_byte_limit == 1,
          "CEIC-023 zero output byte limit was not normalized to one-byte cap");
  Require(bundle.rows.size() <= 1,
          "CEIC-023 zero limits allowed unbounded row output");
  Require(bundle.output_bytes <= bundle.output_byte_limit,
          "CEIC-023 zero limits allowed unbounded byte output");
  Require(bundle.dropped_row_count > 0,
          "CEIC-023 zero limits did not record dropped bounded rows");
}

void RedactionHelperExcludesProtectedValues() {
  const auto redacted =
      memory::RedactMemorySupportBundleValue("token=raw-secret-value", false);
  Require(redacted == "<protected-material-excluded>",
          "CEIC-023 redaction helper did not exclude protected value");
}

}  // namespace

int main() {
  LowMemoryBundleIsBoundedRedactedAndNonAuthoritative();
  EmergencySummaryUsesFixedBoundedRows();
  UnsafeProtectedReviewFailsClosed();
  ZeroLimitsDoNotBecomeUnboundedOutput();
  RedactionHelperExcludesProtectedValues();
  std::cout << "CEIC-023 memory support bundle protected gate passed\n";
  return EXIT_SUCCESS;
}

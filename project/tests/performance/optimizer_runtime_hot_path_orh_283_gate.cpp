// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "temp_spill_executor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ORH-283 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view needle) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.find(needle) != std::string::npos;
  });
}

std::vector<exec::TempSpillInputRow> Rows() {
  std::vector<exec::TempSpillInputRow> rows;
  for (std::uint64_t i = 0; i < 48; ++i) {
    rows.push_back({"k" + std::to_string((47 - i) % 7),
                    static_cast<std::int64_t>((i % 5) + 1),
                    i + 1});
  }
  return rows;
}

exec::TempSpillRequest Request(const std::filesystem::path& root,
                               exec::TempSpillRouteKind kind,
                               std::string route_label) {
  exec::TempSpillRequest request;
  request.route_kind = kind;
  request.route_label = std::move(route_label);
  request.spill_directory = root / request.route_label;
  request.runtime_generation = 283;
  request.memory_quota_bytes = 512;
  request.rows = Rows();
  request.top_n = 5;
  request.authority.engine_mga_snapshot_bound = true;
  request.authority.transaction_inventory_authoritative = true;
  request.authority.security_recheck_required = true;
  request.authority.security_context_bound = true;
  request.authority.exact_recheck_required = true;
  return request;
}

void RequireAccepted(const exec::TempSpillResult& result,
                     std::string_view route_kind) {
  Require(result.ok && result.benchmark_clean,
          "temp-spill route was not benchmark-clean");
  Require(result.runtime_consumed && result.spilled,
          "temp-spill runtime was not consumed");
  Require(result.cleanup_proven && result.reopen_recovery_proven,
          "cleanup/reopen recovery evidence missing");
  Require(!result.result_hash.empty(), "result hash missing");
  Require(HasEvidence(result.evidence, "orh283.route_kind=" + std::string(route_kind)),
          "route-kind evidence missing");
  Require(HasEvidence(result.evidence, "orh283.result_equivalence=true"),
          "result equivalence evidence missing");
  Require(HasEvidence(result.evidence, "orh283.memory_grant_bytes="),
          "memory grant evidence missing");
  Require(HasEvidence(result.evidence, "orh283.spill_generation=283"),
          "spill generation evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh283.restart_reopen_recovery_proven=true"),
          "restart/reopen recovery evidence missing");
  Require(HasEvidence(result.evidence, "orh283.cleanup_proven=true"),
          "cleanup evidence missing");
  Require(HasEvidence(result.evidence,
                      "temporary_work.spill_payload_checksum=validated"),
          "physical spill checksum evidence missing");
  Require(HasEvidence(result.evidence,
                      "temporary_work.cleanup.cancel_safe=true"),
          "cancel-safe cleanup evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh283.temp_metadata.row_identity_authority=false"),
          "row identity non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh283.temp_metadata.finality_authority=false"),
          "finality non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh283.temp_metadata.recovery_authority=false"),
          "recovery non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh283.mga_finality_authority=engine_transaction_inventory"),
          "MGA authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh283.security_recheck_required=true"),
          "security recheck evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh283.exact_recheck_required=true"),
          "exact recheck evidence missing");
}

void RequireRejected(const exec::TempSpillRequest& request,
                     std::string_view diagnostic,
                     std::string_view evidence) {
  const auto result = exec::ExecuteBoundedTempSpillRoute(request);
  Require(!result.benchmark_clean,
          "negative temp-spill case was benchmark-clean");
  Require(result.diagnostic_code == diagnostic,
          "negative temp-spill diagnostic mismatch");
  Require(HasEvidence(result.evidence, evidence),
          "negative temp-spill evidence missing");
}

void VerifyPositiveRoutes(const std::filesystem::path& root) {
  const auto sort = exec::ExecuteBoundedTempSpillRoute(
      Request(root, exec::TempSpillRouteKind::kSort, "orh283.sort"));
  RequireAccepted(sort, "sort");
  Require(sort.output_rows.front() <= sort.output_rows.back(),
          "sort output was not ordered");

  const auto top_n = exec::ExecuteBoundedTempSpillRoute(
      Request(root, exec::TempSpillRouteKind::kTopN, "orh283.top_n"));
  RequireAccepted(top_n, "top_n");
  Require(top_n.output_rows.size() == 5, "top-N output size mismatch");

  const auto distinct = exec::ExecuteBoundedTempSpillRoute(
      Request(root, exec::TempSpillRouteKind::kDistinct, "orh283.distinct"));
  RequireAccepted(distinct, "distinct");
  Require(distinct.output_rows.size() == 7, "DISTINCT output size mismatch");

  const auto group_by = exec::ExecuteBoundedTempSpillRoute(
      Request(root, exec::TempSpillRouteKind::kGroupBy, "orh283.group_by"));
  RequireAccepted(group_by, "group_by");
  Require(group_by.output_rows.size() == 7, "GROUP BY output size mismatch");

  const auto hash_aggregate = exec::ExecuteBoundedTempSpillRoute(
      Request(root,
              exec::TempSpillRouteKind::kHashAggregate,
              "orh283.hash_aggregate"));
  RequireAccepted(hash_aggregate, "hash_aggregate");
  Require(hash_aggregate.output_rows == group_by.output_rows,
          "hash aggregate did not match GROUP BY result");
}

void VerifyNegativeRoutes(const std::filesystem::path& root) {
  auto parser_authority =
      Request(root, exec::TempSpillRouteKind::kSort, "orh283.parser_authority");
  parser_authority.authority.parser_client_or_reference_spill_authority = true;
  RequireRejected(parser_authority,
                  "ORH_SORT_SPILL_UNSAFE_AUTHORITY",
                  "temp_spill_metadata_is_advisory_only");

  auto metadata_finality =
      Request(root, exec::TempSpillRouteKind::kSort, "orh283.metadata_finality");
  metadata_finality.authority.temp_metadata_visibility_or_finality_authority =
      true;
  RequireRejected(metadata_finality,
                  "ORH_SORT_SPILL_UNSAFE_AUTHORITY",
                  "temp_spill_metadata_is_advisory_only");

  auto metadata_recovery =
      Request(root, exec::TempSpillRouteKind::kSort, "orh283.metadata_recovery");
  metadata_recovery.authority.temp_metadata_recovery_authority = true;
  RequireRejected(metadata_recovery,
                  "ORH_SORT_SPILL_UNSAFE_AUTHORITY",
                  "temp_spill_metadata_is_advisory_only");

  auto stale_generation =
      Request(root, exec::TempSpillRouteKind::kSort, "orh283.stale_generation");
  stale_generation.reopen_runtime_generation = 284;
  RequireRejected(stale_generation,
                  "ORH_SORT_SPILL_STALE_GENERATION",
                  "INDEX.TEMPORARY_WORK.STALE_RUNTIME_GENERATION");

  auto missing_accounting =
      Request(root, exec::TempSpillRouteKind::kSort, "orh283.no_accounting");
  missing_accounting.temp_accounting_available = false;
  RequireRejected(missing_accounting,
                  "ORH_SORT_SPILL_TEMP_ACCOUNTING_MISSING",
                  "temp_accounting_required");

  auto cancel_missing_cleanup =
      Request(root, exec::TempSpillRouteKind::kSort, "orh283.cancel_missing_cleanup");
  cancel_missing_cleanup.cancellation_requested = true;
  cancel_missing_cleanup.cleanup_after_cancellation = false;
  RequireRejected(cancel_missing_cleanup,
                  "ORH_SORT_SPILL_CANCEL_CLEANUP_MISSING",
                  "cancellation_cleanup_proof_missing");

  auto cancel_cleanup =
      Request(root, exec::TempSpillRouteKind::kSort, "orh283.cancel_cleanup");
  cancel_cleanup.cancellation_requested = true;
  const auto cancelled = exec::ExecuteBoundedTempSpillRoute(cancel_cleanup);
  Require(!cancelled.benchmark_clean &&
              cancelled.diagnostic_code == "ORH_SORT_SPILL_CANCELLED" &&
              cancelled.cleanup_proven &&
              HasEvidence(cancelled.evidence,
                          "temporary_work.cancel.completed=true"),
          "cancelled route did not prove cleanup");

  auto missing_recovery =
      Request(root, exec::TempSpillRouteKind::kSort, "orh283.no_recovery");
  missing_recovery.restart_recovery_proof_available = false;
  RequireRejected(missing_recovery,
                  "ORH_SORT_SPILL_RECOVERY_PROOF_MISSING",
                  "INDEX.TEMPORARY_WORK.MISSING_RECHECK_PROOF");

  auto mismatch =
      Request(root, exec::TempSpillRouteKind::kSort, "orh283.result_mismatch");
  mismatch.expected_result_hash = "fnv1a64:wrong";
  RequireRejected(mismatch,
                  "ORH_SORT_SPILL_RESULT_MISMATCH",
                  "spilled_result_hash_mismatch");

  auto missing_mga =
      Request(root, exec::TempSpillRouteKind::kSort, "orh283.missing_mga");
  missing_mga.authority.transaction_inventory_authoritative = false;
  RequireRejected(missing_mga,
                  "ORH_SORT_SPILL_MGA_UNPROVEN",
                  "engine_mga_transaction_inventory_required");

  auto missing_security =
      Request(root, exec::TempSpillRouteKind::kSort, "orh283.missing_security");
  missing_security.authority.security_context_bound = false;
  RequireRejected(missing_security,
                  "ORH_SORT_SPILL_SECURITY_UNPROVEN",
                  "security_and_exact_recheck_required");

  auto no_fallback =
      Request(root, exec::TempSpillRouteKind::kSort, "orh283.no_fallback");
  no_fallback.exact_fallback_available = false;
  RequireRejected(no_fallback,
                  "ORH_SORT_SPILL_EXACT_FALLBACK_UNAVAILABLE",
                  "exact_fallback_required");

  auto memory_pressure =
      Request(root, exec::TempSpillRouteKind::kSort, "orh283.memory_pressure");
  memory_pressure.memory_quota_bytes = 8;
  memory_pressure.spill_allowed = false;
  RequireRejected(memory_pressure,
                  "ORH_SORT_SPILL_MEMORY_PRESSURE_FALLBACK",
                  "temporary_memory_grant_denied");

  auto no_runtime =
      Request(root, exec::TempSpillRouteKind::kSort, "orh283.no_runtime");
  no_runtime.runtime_enabled = false;
  RequireRejected(no_runtime,
                  "ORH_SORT_SPILL_NO_RUNTIME",
                  "runtime_consumption_missing");
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("scratchbird_orh283_temp_spill_" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  VerifyPositiveRoutes(root);
  VerifyNegativeRoutes(root);
  std::filesystem::remove_all(root, ignored);
  std::cout << "ORH-283 temp spill execution gate passed\n";
  return EXIT_SUCCESS;
}

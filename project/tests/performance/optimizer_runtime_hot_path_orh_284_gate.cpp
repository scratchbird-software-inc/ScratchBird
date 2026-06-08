// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "write_path_batching.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ORH-284 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind,
                                  platform::u64 millis,
                                  platform::byte suffix) {
  const auto generated = uuid::GenerateCompatibilityUnixTimeV7(millis);
  Require(generated.ok(), "uuidv7 generation failed");
  auto value = generated.value;
  value.bytes[15] = suffix;
  const auto typed = uuid::MakeTypedUuid(kind, value);
  Require(typed.ok(), "typed uuid creation failed");
  return typed.value;
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view needle) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.find(needle) != std::string::npos;
  });
}

db::WritePathBatchingRequest Request(const std::filesystem::path& root,
                                     std::string route_label) {
  db::WritePathBatchingRequest request;
  request.route_label = std::move(route_label);
  request.scratch_directory = root / request.route_label;
  request.database_uuid = GeneratedUuid(platform::UuidKind::database, 284000, 1);
  request.filespace_uuid =
      GeneratedUuid(platform::UuidKind::filespace, 284001, 2);
  request.transaction_uuid =
      GeneratedUuid(platform::UuidKind::transaction, 284002, 3);
  request.local_transaction_id = 7;
  request.batching_generation = 284;
  request.expected_batching_generation = 284;
  request.dirty_page_count = 6;
  request.page_generation = 33;
  request.extent_page_count = 6;
  request.authority.engine_mga_tip_authoritative = true;
  request.authority.durable_transaction_inventory_proven = true;
  return request;
}

void RequireAccepted(const db::WritePathBatchingResult& result) {
  if (!result.ok || !result.benchmark_clean) {
    Fail("write-path batching was not benchmark-clean: " +
         result.diagnostic_code + " fallback=" + result.fallback_reason);
  }
  Require(result.runtime_consumed, "write-path batching did not consume runtime");
  Require(result.dirty_page_accounting_proven,
          "dirty page accounting evidence missing");
  Require(result.extent_allocation_proven,
          "extent allocation evidence missing");
  Require(result.fsync_open_proven, "fsync/open evidence missing");
  Require(result.crash_reopen_recovery_proven,
          "crash/reopen/recovery evidence missing");
  Require(result.unbatched_flush_operations == 6 &&
              result.batched_flush_operations == 1 &&
              result.flushed_pages == 6,
          "flush batching counters did not prove deterministic reduction");
  Require(!result.state_hash.empty(), "state hash missing");
  Require(HasEvidence(result.evidence, "orh284.route_label=orh284.write_batch"),
          "route label evidence missing");
  Require(HasEvidence(result.evidence, "orh284.batching_generation=284"),
          "batch generation evidence missing");
  Require(HasEvidence(result.evidence, "orh284.dirty_pages_before=6"),
          "dirty page before evidence missing");
  Require(HasEvidence(result.evidence, "orh284.dirty_pages_after=0"),
          "dirty page after evidence missing");
  Require(HasEvidence(result.evidence, "orh284.flushed_pages=6"),
          "flushed page evidence missing");
  Require(HasEvidence(result.evidence, "orh284.dirty_page_accounting_proven=true"),
          "dirty-page accounting proof missing");
  Require(HasEvidence(result.evidence, "orh284.extent_allocation_proven=true"),
          "extent allocation proof missing");
  Require(HasEvidence(result.evidence, "orh284.fsync_open_proven=true"),
          "fsync/open proof missing");
  Require(HasEvidence(result.evidence,
                      "orh284.crash_reopen_recovery_proven=true"),
          "crash/reopen recovery proof missing");
  Require(HasEvidence(result.evidence,
                      "orh284.recovery_evidence_completed=true"),
          "recovery run evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh284.checkpoint_writeback_complete=true"),
          "checkpoint writeback evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh284.write_batch_metadata.finality_authority=false"),
          "finality non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh284.write_batch_metadata.visibility_authority=false"),
          "visibility non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh284.write_batch_metadata.row_identity_authority=false"),
          "row identity non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh284.write_batch_metadata.authorization_authority=false"),
          "authorization non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh284.write_batch_metadata.recovery_authority=false"),
          "recovery non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh284.mga_recovery_authority=durable_transaction_inventory"),
          "MGA/TIP recovery authority evidence missing");
}

void RequireRejected(const db::WritePathBatchingRequest& request,
                     std::string_view diagnostic,
                     std::string_view evidence) {
  const auto result = db::ExecuteDurabilityWritePathBatch(request);
  Require(!result.benchmark_clean,
          "negative write-path batching case was benchmark-clean");
  Require(result.diagnostic_code == diagnostic,
          "negative write-path batching diagnostic mismatch");
  Require(HasEvidence(result.evidence, evidence),
          "negative write-path batching evidence missing");
}

void VerifyNegativeCases(const std::filesystem::path& root) {
  auto parser_authority = Request(root, "orh284.parser_authority");
  parser_authority.authority.parser_client_or_donor_write_batch_authority =
      true;
  RequireRejected(parser_authority,
                  "ORH_WRITE_BATCHING_UNSAFE_AUTHORITY",
                  "write_batch_metadata_is_advisory_only");

  auto metadata_finality = Request(root, "orh284.metadata_finality");
  metadata_finality.authority.batch_metadata_finality_or_visibility_authority =
      true;
  RequireRejected(metadata_finality,
                  "ORH_WRITE_BATCHING_UNSAFE_AUTHORITY",
                  "write_batch_metadata_is_advisory_only");

  auto metadata_recovery = Request(root, "orh284.metadata_recovery");
  metadata_recovery.authority.batch_metadata_recovery_authority = true;
  RequireRejected(metadata_recovery,
                  "ORH_WRITE_BATCHING_UNSAFE_AUTHORITY",
                  "write_batch_metadata_is_advisory_only");

  auto missing_tip = Request(root, "orh284.missing_tip");
  missing_tip.authority.durable_transaction_inventory_proven = false;
  RequireRejected(missing_tip,
                  "ORH_WRITE_BATCHING_MGA_TIP_UNPROVEN",
                  "durable_transaction_inventory_required");

  auto stale_generation = Request(root, "orh284.stale_generation");
  stale_generation.batching_generation = 283;
  RequireRejected(stale_generation,
                  "ORH_WRITE_BATCHING_STALE_GENERATION",
                  "batching_generation_mismatch");

  auto missing_dirty = Request(root, "orh284.missing_dirty_accounting");
  missing_dirty.dirty_page_accounting_available = false;
  RequireRejected(missing_dirty,
                  "ORH_WRITE_BATCHING_DIRTY_ACCOUNTING_MISSING",
                  "dirty_page_accounting_required");

  auto missing_fsync = Request(root, "orh284.missing_fsync");
  missing_fsync.fsync_open_proof_available = false;
  RequireRejected(missing_fsync,
                  "ORH_WRITE_BATCHING_FSYNC_OPEN_MISSING",
                  "fsync_open_proof_required");

  auto extent_mismatch = Request(root, "orh284.extent_mismatch");
  extent_mismatch.extent_allocation_matches = false;
  RequireRejected(extent_mismatch,
                  "ORH_WRITE_BATCHING_EXTENT_MISMATCH",
                  "extent_allocation_mismatch");

  auto crash_mismatch = Request(root, "orh284.crash_mismatch");
  crash_mismatch.expected_state_hash = "fnv1a64:wrong";
  RequireRejected(crash_mismatch,
                  "ORH_WRITE_BATCHING_CRASH_REOPEN_MISMATCH",
                  "state_hash_mismatch_after_reopen");

  auto metadata_only = Request(root, "orh284.metadata_only_recovery");
  metadata_only.authority.recovery_from_batch_metadata_alone = true;
  RequireRejected(metadata_only,
                  "ORH_WRITE_BATCHING_METADATA_ONLY_RECOVERY_REFUSED",
                  "durable_mga_tip_inventory_required_for_recovery");

  auto no_fallback = Request(root, "orh284.no_fallback");
  no_fallback.exact_fallback_available = false;
  RequireRejected(no_fallback,
                  "ORH_WRITE_BATCHING_NO_EXACT_FALLBACK",
                  "exact_unbatched_fallback_required");

  auto resource_pressure = Request(root, "orh284.resource_pressure");
  resource_pressure.resource_pressure = true;
  RequireRejected(resource_pressure,
                  "ORH_WRITE_BATCHING_RESOURCE_PRESSURE_FALLBACK",
                  "resource_pressure_unbatched_fallback");

  auto no_runtime = Request(root, "orh284.no_runtime");
  no_runtime.runtime_enabled = false;
  RequireRejected(no_runtime,
                  "ORH_WRITE_BATCHING_NO_RUNTIME",
                  "runtime_consumption_missing");
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("scratchbird_orh284_write_batching_" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);

  const auto accepted =
      db::ExecuteDurabilityWritePathBatch(Request(root, "orh284.write_batch"));
  RequireAccepted(accepted);
  VerifyNegativeCases(root);

  std::filesystem::remove_all(root, ignored);
  std::cout << "ORH-284 durability write-path batching gate passed\n";
  return EXIT_SUCCESS;
}

// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "resource_governance_admission.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;

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
  for (const auto& item : evidence) {
    if (item.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

agents::ResourceGovernanceQuotaVector Limits() {
  agents::ResourceGovernanceQuotaVector limits;
  limits.memory_bytes = 100;
  limits.device_memory_bytes = 1;
  limits.pinned_memory_bytes = 1;
  limits.io_bytes = 1000;
  limits.io_ops = 10;
  limits.worker_threads = 8;
  limits.backlog_items = 16;
  limits.candidate_rows = 1000;
  limits.cache_entries = 16;
  limits.batch_rows = 100;
  limits.fragments = 16;
  limits.lanes = 4;
  limits.time_budget_microseconds = 1000000;
  return limits;
}

agents::ResourceGovernanceAdmissionRequest Admission(std::string operation_id,
                                                     std::int64_t memory_bytes) {
  agents::ResourceGovernanceAdmissionRequest request;
  request.operation_id = std::move(operation_id);
  request.expected_family = agents::ResourceGovernanceFamily::kQueryMemoryArena;
  request.descriptor.descriptor_id = "mmch030.query_memory.runtime_quota";
  request.descriptor.family = agents::ResourceGovernanceFamily::kQueryMemoryArena;
  request.descriptor.source = agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.descriptor.source_path_or_label = "runtime.policy.mmch030.query_memory";
  request.descriptor.descriptor_generation = 30;
  request.descriptor.expected_generation = 30;
  request.descriptor.limits = Limits();
  request.descriptor.over_limit_action = agents::ResourceGovernanceAction::kFailClosed;
  request.descriptor.benchmark_clean = true;
  request.descriptor.runtime_dependency_present = true;
  request.requested.memory_bytes = memory_bytes;
  request.requested.worker_threads = 1;
  request.requested.backlog_items = 1;
  request.requested.time_budget_microseconds = 1000;
  return request;
}

agents::ResourceGovernanceReservationAcquireRequest Reservation(
    std::string operation_id,
    std::string owner_scope,
    std::int64_t memory_bytes,
    std::uint64_t deadline = 0) {
  agents::ResourceGovernanceReservationAcquireRequest request;
  request.admission = Admission(std::move(operation_id), memory_bytes);
  request.owner_scope = std::move(owner_scope);
  request.lease_deadline_tick = deadline;
  return request;
}

void RequireAuthorityEvidence(const std::vector<std::string>& evidence) {
  Require(EvidenceHas(evidence, "MMCH_RESOURCE_RESERVATION_LIFECYCLE"),
          "MMCH-030 lifecycle marker missing");
  Require(EvidenceHas(
              evidence,
              "resource_reservation.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority"),
          "MMCH-030 authority scope evidence missing");
}

void AcquireReleaseAndCumulativeRefusal() {
  agents::ResourceGovernanceReservationLedger ledger("mmch030-ledger");

  auto first = ledger.Acquire(Reservation("op-first", "session-A", 40));
  Require(first.ok && first.reservation_created,
          "MMCH-030 first reservation was not created");
  Require(first.snapshot.active.memory_bytes == 40,
          "MMCH-030 first reservation active memory mismatch");
  RequireAuthorityEvidence(first.evidence);

  auto second = ledger.Acquire(Reservation("op-second", "session-A", 50));
  Require(second.ok && second.reservation_created,
          "MMCH-030 second reservation was not created");
  Require(second.snapshot.active.memory_bytes == 90,
          "MMCH-030 second reservation active memory mismatch");

  auto refused = ledger.Acquire(Reservation("op-third", "session-B", 20));
  Require(!refused.ok && refused.fail_closed,
          "MMCH-030 cumulative over-limit reservation did not fail closed");
  Require(refused.diagnostic_code ==
              "SB_RESOURCE_GOVERNANCE.RESERVATION_LEDGER_LIMIT_EXCEEDED",
          "MMCH-030 cumulative over-limit diagnostic changed");
  Require(refused.exceeded_quota == "memory_bytes",
          "MMCH-030 cumulative over-limit quota mismatch");
  Require(refused.snapshot.active.memory_bytes == 90,
          "MMCH-030 refused reservation changed active usage");
  RequireAuthorityEvidence(refused.evidence);

  auto released = ledger.Release(first.reservation.token_id);
  Require(released.ok && released.released,
          "MMCH-030 reservation release failed");
  Require(released.snapshot.active.memory_bytes == 50,
          "MMCH-030 release did not debit active memory");
  RequireAuthorityEvidence(released.evidence);

  auto double_release = ledger.Release(first.reservation.token_id);
  Require(!double_release.ok && double_release.not_found,
          "MMCH-030 double release did not fail closed");
  Require(double_release.diagnostic_code ==
              "SB_RESOURCE_GOVERNANCE.RESERVATION_NOT_FOUND",
          "MMCH-030 double-release diagnostic changed");
}

void OwnerDisconnectAndTimeoutCleanup() {
  agents::ResourceGovernanceReservationLedger ledger("mmch030-cleanup");
  auto keep = ledger.Acquire(Reservation("op-keep", "session-keep", 10, 100));
  auto expired = ledger.Acquire(Reservation("op-expired", "session-timeout", 10, 5));
  auto disconnect_a = ledger.Acquire(Reservation("op-disc-a", "session-drop", 15, 0));
  auto disconnect_b = ledger.Acquire(Reservation("op-disc-b", "session-drop", 15, 0));
  Require(keep.ok && expired.ok && disconnect_a.ok && disconnect_b.ok,
          "MMCH-030 cleanup setup reservations failed");
  Require(ledger.Snapshot().active.memory_bytes == 50,
          "MMCH-030 cleanup setup active memory mismatch");

  auto timeout = ledger.ExpireReservations(6);
  Require(timeout.ok && timeout.released_count == 1,
          "MMCH-030 timeout cleanup did not release exactly one reservation");
  Require(timeout.snapshot.active.memory_bytes == 40,
          "MMCH-030 timeout cleanup active memory mismatch");
  RequireAuthorityEvidence(timeout.evidence);

  auto disconnect = ledger.ReleaseOwnerReservations(
      "session-drop",
      agents::ResourceGovernanceReservationReleaseReason::kDisconnect);
  Require(disconnect.ok && disconnect.released_count == 2,
          "MMCH-030 disconnect cleanup did not release owned reservations");
  Require(disconnect.snapshot.active.memory_bytes == 10,
          "MMCH-030 disconnect cleanup active memory mismatch");
  RequireAuthorityEvidence(disconnect.evidence);

  auto keep_release = ledger.Release(keep.reservation.token_id,
                                     agents::ResourceGovernanceReservationReleaseReason::kCancel);
  Require(keep_release.ok && keep_release.snapshot.active.memory_bytes == 0,
          "MMCH-030 cancel release did not empty ledger");
}

void AdmissionAndOwnerValidation() {
  agents::ResourceGovernanceReservationLedger ledger("mmch030-validation");

  auto missing_owner = ledger.Acquire(Reservation("op-missing-owner", "", 10));
  Require(!missing_owner.ok && missing_owner.fail_closed,
          "MMCH-030 missing owner reservation was accepted");
  Require(missing_owner.diagnostic_code ==
              "SB_RESOURCE_GOVERNANCE.RESERVATION_OWNER_REQUIRED",
          "MMCH-030 missing owner diagnostic changed");
  Require(missing_owner.snapshot.active_reservation_count == 0,
          "MMCH-030 missing owner created an active reservation");

  auto stale = Reservation("op-stale", "session-stale", 10);
  stale.admission.descriptor.expected_generation = 31;
  auto stale_result = ledger.Acquire(stale);
  Require(!stale_result.ok,
          "MMCH-030 stale admission was accepted by reservation ledger");
  Require(stale_result.diagnostic_code ==
              "SB_RESOURCE_GOVERNANCE.STALE_DESCRIPTOR_REFUSED",
          "MMCH-030 stale admission diagnostic changed");
  Require(stale_result.snapshot.active_reservation_count == 0,
          "MMCH-030 stale admission created an active reservation");
  RequireAuthorityEvidence(stale_result.evidence);
}

}  // namespace

int main() {
  std::cout << "MMCH-030 authority_note=resource_reservation_evidence_only;"
               "not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority"
            << '\n';
  AcquireReleaseAndCumulativeRefusal();
  OwnerDisconnectAndTimeoutCleanup();
  AdmissionAndOwnerValidation();
  return EXIT_SUCCESS;
}

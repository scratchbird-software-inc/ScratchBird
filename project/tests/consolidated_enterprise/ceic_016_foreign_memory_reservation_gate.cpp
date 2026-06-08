// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-016 focused validation for foreign/native memory reservations.
#include "foreign_memory_reservation.hpp"
#include "memory_support_bundle.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace memory = scratchbird::core::memory;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::u64;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(const std::vector<std::string>& evidence, std::string_view needle) {
  for (const auto& entry : evidence) {
    if (entry.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool HasRow(const memory::MemorySupportBundleResult& bundle,
            std::string_view key,
            std::string_view value) {
  for (const auto& row : bundle.rows) {
    if (row.key == key && row.value == value) {
      return true;
    }
  }
  return false;
}

memory::HierarchicalMemoryBudgetProvenance Provenance(
    std::string label = "ceic_016_foreign_memory_reservation_gate") {
  memory::HierarchicalMemoryBudgetProvenance provenance;
  provenance.source = memory::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  provenance.source_label = std::move(label);
  provenance.engine_mga_authoritative = true;
  provenance.memory_evidence_only = true;
  return provenance;
}

std::vector<memory::HierarchicalMemoryScopeRef> ScopeChain(
    const std::string& suffix) {
  return {{memory::HierarchicalMemoryScopeKind::process, "ceic-016-process"},
          {memory::HierarchicalMemoryScopeKind::database, "ceic-016-database"},
          {memory::HierarchicalMemoryScopeKind::session, "ceic-016-session-" + suffix},
          {memory::HierarchicalMemoryScopeKind::statement, "ceic-016-statement-" + suffix},
          {memory::HierarchicalMemoryScopeKind::plugin, "ceic-016-plugin-" + suffix}};
}

void SetBudget(memory::HierarchicalMemoryBudgetLedger* ledger,
               memory::HierarchicalMemoryScopeKind kind,
               const std::string& scope_id,
               u64 hard_limit) {
  memory::HierarchicalMemoryBudget budget;
  budget.scope = {kind, scope_id};
  budget.hard_limit_bytes = hard_limit;
  budget.provenance = Provenance();
  Require(ledger->SetBudget(std::move(budget)).ok(),
          "CEIC-016 budget setup failed");
}

memory::ForeignMemoryReservationRequest Request(
    memory::HierarchicalMemoryBudgetLedger* ledger,
    memory::ForeignMemorySource source,
    std::string suffix,
    u64 estimated_bytes = 512,
    u64 observed_bytes = 64) {
  memory::ForeignMemoryReservationRequest request;
  request.source = source;
  request.reservation_ledger = ledger;
  request.scope_chain = ScopeChain(suffix);
  request.estimated_bytes = estimated_bytes;
  request.observed_bytes = observed_bytes;
  request.owner_id = "ceic-016-owner-" + suffix;
  request.owning_scope = "ceic-016-scope-" + suffix;
  request.operation_id = "ceic-016-operation-" + suffix;
  request.native_callsite =
      std::string("ceic_016.native.") + memory::ForeignMemorySourceName(source);
  request.confidence = memory::ForeignMemoryConfidence::conservative;
  request.expected_release_event =
      memory::ForeignMemoryReleaseEvent::explicit_release;
  request.over_limit_action = memory::ForeignMemoryOverLimitAction::deny;
  request.provenance = Provenance();
  request.authority.evidence_label = "ceic_016_foreign_memory_gate";
  request.authority.authority_generation = "ceic-016-runtime";
  request.evidence = {
      std::string("modeled_source=") + memory::ForeignMemorySourceName(source),
      "reserve_before_native_call=true",
      "live_route_claim=false"};
  if (source == memory::ForeignMemorySource::llvm) {
    request.linkage_mode = memory::ForeignMemoryLinkageMode::dynamic_library;
    request.evidence.push_back("llvm_source_path=~/local workspace/llvm");
    request.evidence.push_back("llvm_build_not_invoked_by_ceic_016=true");
  }
  return request;
}

std::string SerializeSnapshot(
    const memory::ForeignMemoryReservationSnapshot& snapshot) {
  std::ostringstream out;
  out << "active=" << snapshot.active_reservation_count
      << ";current_estimated=" << snapshot.current_estimated_bytes
      << ";peak_estimated=" << snapshot.peak_estimated_bytes
      << ";current_observed=" << snapshot.current_observed_bytes
      << ";peak_observed=" << snapshot.peak_observed_bytes
      << ";reservation_count=" << snapshot.reservation_count
      << ";release_count=" << snapshot.release_count
      << ";sources=" << snapshot.sources.size()
      << ";scopes=" << snapshot.owning_scopes.size()
      << ";active_records=" << snapshot.active_reservations.size();
  for (const auto& source : snapshot.sources) {
    out << "|source:" << memory::ForeignMemorySourceName(source.source)
        << ':' << source.active_reservation_count
        << ':' << source.current_estimated_bytes
        << ':' << source.current_observed_bytes
        << ':' << source.peak_estimated_bytes
        << ':' << source.peak_observed_bytes;
  }
  for (const auto& scope : snapshot.owning_scopes) {
    out << "|scope:" << scope.owning_scope
        << ':' << scope.active_reservation_count
        << ':' << scope.current_estimated_bytes
        << ':' << scope.current_observed_bytes
        << ':' << scope.peak_estimated_bytes
        << ':' << scope.peak_observed_bytes;
  }
  for (const auto& active : snapshot.active_reservations) {
    out << "|active:" << active.token.reservation_id
        << ':' << memory::ForeignMemorySourceName(active.source)
        << ':' << active.owner_id
        << ':' << active.owning_scope
        << ':' << active.estimated_bytes
        << ':' << active.observed_bytes
        << ':' << memory::ForeignMemoryLinkageModeName(active.linkage_mode);
  }
  return out.str();
}

std::vector<memory::ForeignMemorySource> AllModeledSources() {
  return {memory::ForeignMemorySource::llvm,
          memory::ForeignMemorySource::icu,
          memory::ForeignMemorySource::crypto,
          memory::ForeignMemorySource::compression,
          memory::ForeignMemorySource::regex,
          memory::ForeignMemorySource::json,
          memory::ForeignMemorySource::mmap,
          memory::ForeignMemorySource::thread_stack,
          memory::ForeignMemorySource::os_buffer,
          memory::ForeignMemorySource::plugin_udr,
          memory::ForeignMemorySource::driver_native,
          memory::ForeignMemorySource::gpu_optional};
}

void ValidateModeledSourcesAndSupportBundle(
    memory::HierarchicalMemoryBudgetLedger* budget_ledger,
    memory::ForeignMemoryReservationLedger* foreign_ledger) {
  std::vector<std::unique_ptr<memory::ForeignMemoryReservation>> reservations;
  int index = 0;
  for (auto source : AllModeledSources()) {
    auto request =
        Request(budget_ledger,
                source,
                std::string(memory::ForeignMemorySourceName(source)) + "-" +
                    std::to_string(index++));
    request.authority.provider_available = true;
    if (source == memory::ForeignMemorySource::gpu_optional) {
      request.evidence.push_back("gpu_provider_fixture_available_for_model_only=true");
    }
    auto acquired =
        memory::AcquireForeignMemoryReservation(foreign_ledger, std::move(request));
    Require(acquired.ok(), "CEIC-016 modeled source acquisition failed");
    Require(Contains(acquired.evidence,
                     "CEIC-016_FOREIGN_MEMORY_RESERVATION_COVERAGE"),
            "CEIC-016 evidence anchor missing");
    auto observed = acquired.reservation->UpdateObservedBytes(
        128, memory::ForeignMemoryConfidence::observed);
    Require(observed.ok(), "CEIC-016 observed byte update failed");
    reservations.push_back(std::move(acquired.reservation));
  }

  auto llvm_static =
      Request(budget_ledger, memory::ForeignMemorySource::llvm, "llvm-static");
  llvm_static.linkage_mode = memory::ForeignMemoryLinkageMode::static_library;
  llvm_static.evidence.push_back("llvm_linkage_selection=static_library");
  auto static_acquired =
      memory::AcquireForeignMemoryReservation(foreign_ledger, std::move(llvm_static));
  Require(static_acquired.ok(), "CEIC-016 LLVM static linkage surface failed");
  Require(Contains(static_acquired.evidence,
                   "foreign_memory.linkage_mode=static_library"),
          "CEIC-016 LLVM static linkage evidence missing");
  reservations.push_back(std::move(static_acquired.reservation));

  auto snapshot = foreign_ledger->Snapshot();
  Require(snapshot.active_reservation_count == reservations.size(),
          "CEIC-016 active reservation count mismatch");
  Require(snapshot.current_estimated_bytes >= 13 * 512,
          "CEIC-016 current estimated bytes missing");
  Require(snapshot.current_observed_bytes >= 12 * 128,
          "CEIC-016 current observed bytes missing");
  Require(snapshot.sources.size() == AllModeledSources().size(),
          "CEIC-016 modeled source snapshot coverage mismatch");

  memory::MemoryManager manager(memory::DefaultLocalEngineMemoryPolicy());
  memory::MemorySupportBundleRequest bundle_request;
  bundle_request.snapshot = manager.Snapshot();
  bundle_request.foreign_memory_snapshot = snapshot;
  bundle_request.include_foreign_memory = true;
  auto bundle =
      memory::BuildMemorySupportBundleEvidence(std::move(bundle_request));
  Require(bundle.ok(), "CEIC-016 support bundle build failed");
  Require(bundle.foreign_source_count == AllModeledSources().size(),
          "CEIC-016 support bundle foreign source count mismatch");
  Require(bundle.foreign_owning_scope_count >= reservations.size(),
          "CEIC-016 support bundle owning scope count mismatch");
  Require(HasRow(bundle,
                 "foreign_memory.snapshot.active_reservation_count",
                 std::to_string(reservations.size())),
          "CEIC-016 support bundle active reservation row missing");
  Require(Contains(bundle.evidence,
                   "CEIC-016_FOREIGN_MEMORY_RESERVATION_COVERAGE"),
          "CEIC-016 support bundle evidence anchor missing");

  const std::string serialized_a = SerializeSnapshot(foreign_ledger->Snapshot());
  const std::string serialized_b = SerializeSnapshot(foreign_ledger->Snapshot());
  Require(serialized_a == serialized_b,
          "CEIC-016 foreign snapshot output is not deterministic");

  for (auto& reservation : reservations) {
    Require(reservation->Release().ok(),
            "CEIC-016 modeled source release failed");
  }
  Require(foreign_ledger->Snapshot().current_estimated_bytes == 0,
          "CEIC-016 modeled source estimated bytes leaked");
}

void ValidateCleanupPaths(memory::HierarchicalMemoryBudgetLedger* budget_ledger,
                          memory::ForeignMemoryReservationLedger* foreign_ledger) {
  auto cancel_request =
      Request(budget_ledger, memory::ForeignMemorySource::json, "cancel");
  auto cancel_acquired =
      memory::AcquireForeignMemoryReservation(foreign_ledger, std::move(cancel_request));
  Require(cancel_acquired.ok(), "CEIC-016 cancel acquisition failed");
  Require(cancel_acquired.reservation->Cancel().ok(),
          "CEIC-016 cancel cleanup failed");
  Require(foreign_ledger->Snapshot().cancel_cleanup_count >= 1,
          "CEIC-016 cancel cleanup count missing");

  auto owner_a =
      Request(budget_ledger, memory::ForeignMemorySource::regex, "owner-a");
  owner_a.owner_id = "ceic-016-owner-cleanup";
  auto owner_b =
      Request(budget_ledger, memory::ForeignMemorySource::compression, "owner-b");
  owner_b.owner_id = "ceic-016-owner-cleanup";
  auto acquired_a =
      memory::AcquireForeignMemoryReservation(foreign_ledger, std::move(owner_a));
  auto acquired_b =
      memory::AcquireForeignMemoryReservation(foreign_ledger, std::move(owner_b));
  Require(acquired_a.ok() && acquired_b.ok(),
          "CEIC-016 owner cleanup setup failed");
  auto cleanup = foreign_ledger->CleanupOwner("ceic-016-owner-cleanup");
  Require(cleanup.ok(), "CEIC-016 owner cleanup failed");
  Require(cleanup.cleaned_reservation_count == 2,
          "CEIC-016 owner cleanup count mismatch");
  Require(cleanup.cleaned_estimated_bytes == 1024,
          "CEIC-016 owner cleanup estimated bytes mismatch");
  Require(foreign_ledger->Snapshot().owner_cleanup_count >= 2,
          "CEIC-016 owner cleanup snapshot missing");
}

void ValidateRefusals(memory::HierarchicalMemoryBudgetLedger* budget_ledger,
                      memory::ForeignMemoryReservationLedger* foreign_ledger) {
  auto stale =
      Request(budget_ledger, memory::ForeignMemorySource::icu, "stale-authority");
  stale.authority.evidence_fresh = false;
  auto stale_refused =
      memory::AcquireForeignMemoryReservation(foreign_ledger, std::move(stale));
  Require(!stale_refused.ok() && stale_refused.fail_closed,
          "CEIC-016 stale authority was accepted");
  Require(stale_refused.status.code == StatusCode::memory_invalid_request,
          "CEIC-016 stale authority status mismatch");

  auto unsafe =
      Request(budget_ledger, memory::ForeignMemorySource::crypto, "unsafe-authority");
  unsafe.authority.transaction_finality_authority = true;
  auto unsafe_refused =
      memory::AcquireForeignMemoryReservation(foreign_ledger, std::move(unsafe));
  Require(!unsafe_refused.ok() && unsafe_refused.fail_closed,
          "CEIC-016 unsafe authority was accepted");

  auto untracked =
      Request(budget_ledger, memory::ForeignMemorySource::driver_native, "untracked");
  untracked.untracked_high_risk_native_call = true;
  untracked.conservative_reservation = false;
  auto untracked_refused =
      memory::AcquireForeignMemoryReservation(foreign_ledger, std::move(untracked));
  Require(!untracked_refused.ok() && untracked_refused.fail_closed,
          "CEIC-016 untracked high-risk native call was accepted");

  auto conservative =
      Request(budget_ledger, memory::ForeignMemorySource::driver_native, "conservative");
  conservative.untracked_high_risk_native_call = true;
  conservative.conservative_reservation = true;
  conservative.confidence = memory::ForeignMemoryConfidence::conservative;
  auto conservative_acquired =
      memory::AcquireForeignMemoryReservation(foreign_ledger, std::move(conservative));
  Require(conservative_acquired.ok(),
          "CEIC-016 conservative reservation was refused");
  Require(conservative_acquired.reservation->Release().ok(),
          "CEIC-016 conservative reservation release failed");

  SetBudget(budget_ledger,
            memory::HierarchicalMemoryScopeKind::plugin,
            "ceic-016-plugin-over-limit",
            128);
  auto over_limit =
      Request(budget_ledger,
              memory::ForeignMemorySource::mmap,
              "over-limit",
              512,
              0);
  over_limit.scope_chain = ScopeChain("over-limit");
  over_limit.over_limit_action = memory::ForeignMemoryOverLimitAction::cancel;
  auto limit_refused =
      memory::AcquireForeignMemoryReservation(foreign_ledger, std::move(over_limit));
  Require(!limit_refused.ok() && limit_refused.fail_closed,
          "CEIC-016 over-limit reservation was accepted");
  Require(limit_refused.status.code == StatusCode::memory_limit_exceeded,
          "CEIC-016 over-limit status mismatch");
  Require(foreign_ledger->Snapshot().over_limit_refusal_count >= 1,
          "CEIC-016 over-limit refusal count missing");

  auto gpu =
      Request(budget_ledger, memory::ForeignMemorySource::gpu_optional, "gpu-blocked");
  gpu.authority.provider_available = false;
  gpu.live_route_claim = true;
  auto gpu_refused =
      memory::AcquireForeignMemoryReservation(foreign_ledger, std::move(gpu));
  Require(!gpu_refused.ok() && gpu_refused.fail_closed,
          "CEIC-016 optional GPU route without provider was accepted");

  auto observed =
      Request(budget_ledger, memory::ForeignMemorySource::os_buffer, "observed-limit");
  auto observed_acquired =
      memory::AcquireForeignMemoryReservation(foreign_ledger, std::move(observed));
  Require(observed_acquired.ok(), "CEIC-016 observed-limit setup failed");
  auto observed_refused = observed_acquired.reservation->UpdateObservedBytes(
      2048, memory::ForeignMemoryConfidence::observed);
  Require(!observed_refused.ok() && observed_refused.fail_closed,
          "CEIC-016 observed bytes over reservation was accepted");
  Require(observed_refused.status.code == StatusCode::memory_limit_exceeded,
          "CEIC-016 observed bytes over reservation status mismatch");
  Require(observed_acquired.reservation->Release().ok(),
          "CEIC-016 observed-limit release failed");
}

}  // namespace

int main() {
  memory::HierarchicalMemoryBudgetLedger budget_ledger;
  memory::ForeignMemoryReservationLedger foreign_ledger;
  SetBudget(&budget_ledger,
            memory::HierarchicalMemoryScopeKind::process,
            "ceic-016-process",
            8ull * 1024ull * 1024ull);

  ValidateModeledSourcesAndSupportBundle(&budget_ledger, &foreign_ledger);
  ValidateCleanupPaths(&budget_ledger, &foreign_ledger);
  ValidateRefusals(&budget_ledger, &foreign_ledger);

  const auto foreign_snapshot = foreign_ledger.Snapshot();
  Require(foreign_snapshot.current_estimated_bytes == 0,
          "CEIC-016 leaked foreign estimated bytes");
  Require(foreign_snapshot.current_observed_bytes == 0,
          "CEIC-016 leaked foreign observed bytes");
  Require(foreign_snapshot.active_reservation_count == 0,
          "CEIC-016 leaked active foreign reservations");
  const auto budget_snapshot = budget_ledger.Snapshot();
  Require(budget_snapshot.current_bytes == 0,
          "CEIC-016 leaked CEIC-011 current bytes");
  Require(budget_snapshot.active_allocation_count == 0,
          "CEIC-016 leaked CEIC-011 active allocations");

  std::cout << "CEIC-016 foreign memory reservation gate passed\n";
  return 0;
}

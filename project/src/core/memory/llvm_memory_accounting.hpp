// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-061: LLVM dynamic/static linkage memory accounting.
#include "foreign_memory_reservation.hpp"
#include "metric_registry.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace scratchbird::core::memory {

// SEARCH_KEY: CEIC_061_LLVM_MEMORY_ACCOUNTING
enum class LlvmMemoryReservationPhase {
  dynamic_loader,
  static_linkage_metadata,
  code,
  data,
  jit_native,
  aot_native
};

struct LlvmMemoryAccountingRequest {
  HierarchicalMemoryBudgetLedger* reservation_ledger = nullptr;
  ForeignMemoryReservationLedger* foreign_ledger = nullptr;
  std::vector<HierarchicalMemoryScopeRef> scope_chain;
  std::string owner_id;
  std::string owning_scope;
  std::string operation_id;
  std::string native_callsite;
  std::string provider_label;
  ForeignMemoryLinkageMode linkage_mode =
      ForeignMemoryLinkageMode::dynamic_library;
  bool production_like = true;
  bool explicit_test_fixture = false;
  bool provider_available = true;
  bool reserve_loader_or_link_metadata = true;
  bool reserve_code = true;
  bool reserve_data = true;
  bool reserve_native = true;
  bool aot = false;
  u64 loader_bytes = 64ull * 1024ull;
  u64 static_link_metadata_bytes = 32ull * 1024ull;
  u64 code_bytes = 256ull * 1024ull;
  u64 data_bytes = 128ull * 1024ull;
  u64 native_bytes = 96ull * 1024ull;
  ForeignMemoryConfidence confidence = ForeignMemoryConfidence::conservative;
  ForeignMemoryOverLimitAction over_limit_action =
      ForeignMemoryOverLimitAction::deny;
  HierarchicalMemoryBudgetProvenance provenance;
  ForeignMemoryAuthority authority;
  std::vector<std::string> evidence;
};

struct LlvmMemoryAccountingSnapshot {
  bool active = false;
  ForeignMemoryLinkageMode linkage_mode =
      ForeignMemoryLinkageMode::not_applicable;
  bool production_like = true;
  bool explicit_test_fixture = false;
  bool provider_available = false;
  u64 reservation_count = 0;
  u64 reserved_bytes = 0;
  std::vector<ForeignMemoryReservationToken> tokens;
  std::vector<LlvmMemoryReservationPhase> phases;
  std::vector<std::string> evidence;
};

struct LlvmMemoryAccountingReleaseResult {
  Status status;
  bool fail_closed = false;
  bool released = false;
  u64 released_reservation_count = 0;
  u64 released_bytes = 0;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && !fail_closed;
  }
};

struct LlvmMemoryAccountingAcquireResult;

class LlvmMemoryAccountingReservation {
 public:
  LlvmMemoryAccountingReservation(const LlvmMemoryAccountingReservation&) = delete;
  LlvmMemoryAccountingReservation& operator=(
      const LlvmMemoryAccountingReservation&) = delete;
  ~LlvmMemoryAccountingReservation();

  LlvmMemoryAccountingReleaseResult Release(
      ForeignMemoryReleaseEvent event =
          ForeignMemoryReleaseEvent::explicit_release);
  LlvmMemoryAccountingSnapshot Snapshot() const;
  bool active() const;
  u64 reserved_bytes() const;
  u64 reservation_count() const;

 private:
  struct PhaseReservation {
    LlvmMemoryReservationPhase phase = LlvmMemoryReservationPhase::data;
    u64 bytes = 0;
    std::unique_ptr<ForeignMemoryReservation> reservation;
  };

  struct HandleState {
    std::atomic<bool> released{false};
  };

 public:
  LlvmMemoryAccountingReservation(
      LlvmMemoryAccountingRequest request,
      std::vector<PhaseReservation> reservations,
      std::shared_ptr<HandleState> state);

 private:
  LlvmMemoryAccountingRequest request_;
  std::vector<PhaseReservation> reservations_;
  std::shared_ptr<HandleState> state_;

  friend LlvmMemoryAccountingAcquireResult AcquireLlvmMemoryAccountingReservation(
      LlvmMemoryAccountingRequest request);
};

struct LlvmMemoryAccountingAcquireResult {
  Status status;
  bool fail_closed = false;
  std::unique_ptr<LlvmMemoryAccountingReservation> reservation;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<scratchbird::core::metrics::MetricValue> metrics;

  bool ok() const {
    return status.ok() && !fail_closed && reservation != nullptr;
  }
};

const char* LlvmMemoryReservationPhaseName(
    LlvmMemoryReservationPhase phase);

LlvmMemoryAccountingAcquireResult AcquireLlvmMemoryAccountingReservation(
    LlvmMemoryAccountingRequest request);

}  // namespace scratchbird::core::memory

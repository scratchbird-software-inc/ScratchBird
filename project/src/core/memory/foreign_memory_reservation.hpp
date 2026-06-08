// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-016: reservation-first governance for foreign/native memory.
#include "hierarchical_memory_budget_ledger.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace scratchbird::core::memory {

enum class ForeignMemorySource {
  unknown,
  llvm,
  icu,
  crypto,
  compression,
  regex,
  json,
  mmap,
  thread_stack,
  os_buffer,
  plugin_udr,
  driver_native,
  gpu_optional
};

enum class ForeignMemoryConfidence {
  unknown,
  exact,
  observed,
  estimated,
  conservative
};

enum class ForeignMemoryReleaseEvent {
  explicit_release,
  scope_exit,
  cancel_cleanup,
  owner_cleanup,
  adapter_shutdown
};

enum class ForeignMemoryOverLimitAction {
  deny,
  spill,
  cancel,
  degrade
};

enum class ForeignMemoryLinkageMode {
  not_applicable,
  dynamic_library,
  static_library
};

struct ForeignMemoryAuthority {
  bool engine_mga_authoritative = true;
  bool memory_evidence_only = true;
  bool security_or_policy_checked = true;
  bool evidence_fresh = true;
  bool provider_available = true;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool donor_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool support_bundle_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool agent_action_authority = false;
  bool authorization_authority = false;
  bool cluster_authority = false;
  bool debug_or_relaxed_path = false;
  std::string authority_generation = "runtime";
  std::string evidence_label;
};

struct ForeignMemoryReservationRequest {
  ForeignMemorySource source = ForeignMemorySource::unknown;
  HierarchicalMemoryBudgetLedger* reservation_ledger = nullptr;
  std::vector<HierarchicalMemoryScopeRef> scope_chain;
  MemoryCategory category = MemoryCategory::unknown;
  std::string memory_class;
  u64 estimated_bytes = 0;
  u64 observed_bytes = 0;
  std::string owner_id;
  std::string owning_scope;
  std::string operation_id;
  std::string native_callsite;
  ForeignMemoryConfidence confidence = ForeignMemoryConfidence::estimated;
  ForeignMemoryReleaseEvent expected_release_event =
      ForeignMemoryReleaseEvent::explicit_release;
  ForeignMemoryOverLimitAction over_limit_action =
      ForeignMemoryOverLimitAction::deny;
  ForeignMemoryLinkageMode linkage_mode = ForeignMemoryLinkageMode::not_applicable;
  bool production_like = true;
  bool live_route_claim = false;
  bool untracked_high_risk_native_call = false;
  bool conservative_reservation = false;
  HierarchicalMemoryBudgetProvenance provenance;
  ForeignMemoryAuthority authority;
  std::vector<std::string> evidence;
};

struct ForeignMemoryReservationToken {
  u64 reservation_id = 0;
  HierarchicalMemoryReservationToken ledger_token;

  bool valid() const {
    return reservation_id != 0 && ledger_token.valid();
  }
};

struct ForeignMemoryActiveReservationSnapshot {
  ForeignMemoryReservationToken token;
  ForeignMemorySource source = ForeignMemorySource::unknown;
  MemoryCategory category = MemoryCategory::unknown;
  std::string owner_id;
  std::string owning_scope;
  std::string operation_id;
  std::string memory_class;
  u64 estimated_bytes = 0;
  u64 observed_bytes = 0;
  ForeignMemoryConfidence confidence = ForeignMemoryConfidence::unknown;
  ForeignMemoryReleaseEvent expected_release_event =
      ForeignMemoryReleaseEvent::explicit_release;
  ForeignMemoryOverLimitAction over_limit_action =
      ForeignMemoryOverLimitAction::deny;
  ForeignMemoryLinkageMode linkage_mode = ForeignMemoryLinkageMode::not_applicable;
  std::vector<std::string> evidence;
};

struct ForeignMemorySourceSnapshot {
  ForeignMemorySource source = ForeignMemorySource::unknown;
  u64 active_reservation_count = 0;
  u64 current_estimated_bytes = 0;
  u64 peak_estimated_bytes = 0;
  u64 current_observed_bytes = 0;
  u64 peak_observed_bytes = 0;
  u64 reservation_count = 0;
  u64 release_count = 0;
  u64 cancel_cleanup_count = 0;
  u64 owner_cleanup_count = 0;
  u64 over_limit_refusal_count = 0;
};

struct ForeignMemoryOwningScopeSnapshot {
  std::string owning_scope;
  u64 active_reservation_count = 0;
  u64 current_estimated_bytes = 0;
  u64 peak_estimated_bytes = 0;
  u64 current_observed_bytes = 0;
  u64 peak_observed_bytes = 0;
  u64 reservation_count = 0;
  u64 release_count = 0;
  u64 cancel_cleanup_count = 0;
  u64 owner_cleanup_count = 0;
  u64 over_limit_refusal_count = 0;
};

struct ForeignMemoryReservationSnapshot {
  u64 active_reservation_count = 0;
  u64 current_estimated_bytes = 0;
  u64 peak_estimated_bytes = 0;
  u64 current_observed_bytes = 0;
  u64 peak_observed_bytes = 0;
  u64 reservation_count = 0;
  u64 release_count = 0;
  u64 cancel_cleanup_count = 0;
  u64 owner_cleanup_count = 0;
  u64 over_limit_refusal_count = 0;
  u64 fail_closed_refusal_count = 0;
  u64 failed_release_count = 0;
  std::vector<ForeignMemorySourceSnapshot> sources;
  std::vector<ForeignMemoryOwningScopeSnapshot> owning_scopes;
  std::vector<ForeignMemoryActiveReservationSnapshot> active_reservations;
};

struct ForeignMemoryReservationReleaseResult {
  Status status;
  bool fail_closed = false;
  bool released = false;
  DiagnosticRecord diagnostic;
  ForeignMemoryActiveReservationSnapshot snapshot;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && !fail_closed;
  }
};

struct ForeignMemoryReservationObservationResult {
  Status status;
  bool fail_closed = false;
  DiagnosticRecord diagnostic;
  ForeignMemoryActiveReservationSnapshot snapshot;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && !fail_closed;
  }
};

struct ForeignMemoryReservationCleanupResult {
  Status status;
  DiagnosticRecord diagnostic;
  u64 cleaned_reservation_count = 0;
  u64 cleaned_estimated_bytes = 0;
  u64 cleaned_observed_bytes = 0;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

class ForeignMemoryReservationLedger;

class ForeignMemoryReservation {
 public:
  ForeignMemoryReservation(const ForeignMemoryReservation&) = delete;
  ForeignMemoryReservation& operator=(const ForeignMemoryReservation&) = delete;
  ~ForeignMemoryReservation();

  ForeignMemoryReservationReleaseResult Release(
      ForeignMemoryReleaseEvent event = ForeignMemoryReleaseEvent::explicit_release);
  ForeignMemoryReservationReleaseResult Cancel();
  ForeignMemoryReservationObservationResult UpdateObservedBytes(
      u64 observed_bytes,
      ForeignMemoryConfidence confidence = ForeignMemoryConfidence::observed);
  ForeignMemoryActiveReservationSnapshot Snapshot() const;
  bool active() const;
  const ForeignMemoryReservationToken& token() const;
  const ForeignMemoryReservationRequest& request() const;

 private:
  struct HandleState {
    std::atomic<bool> released{false};
  };

  ForeignMemoryReservation(ForeignMemoryReservationLedger* owner,
                           ForeignMemoryReservationRequest request,
                           ForeignMemoryReservationToken token,
                           std::shared_ptr<HandleState> state);

  ForeignMemoryReservationLedger* owner_ = nullptr;
  ForeignMemoryReservationRequest request_;
  ForeignMemoryReservationToken token_;
  std::shared_ptr<HandleState> state_;

  friend class ForeignMemoryReservationLedger;
};

struct ForeignMemoryReservationAcquireResult {
  Status status;
  bool fail_closed = false;
  std::unique_ptr<ForeignMemoryReservation> reservation;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && !fail_closed && reservation != nullptr;
  }
};

class ForeignMemoryReservationLedger {
 public:
  ForeignMemoryReservationLedger() = default;
  ForeignMemoryReservationLedger(const ForeignMemoryReservationLedger&) = delete;
  ForeignMemoryReservationLedger& operator=(const ForeignMemoryReservationLedger&) = delete;

  ForeignMemoryReservationAcquireResult Reserve(ForeignMemoryReservationRequest request);
  ForeignMemoryReservationCleanupResult CleanupOwner(std::string owner_id);
  ForeignMemoryReservationSnapshot Snapshot() const;

 private:
  struct BucketAccounting {
    u64 active_reservation_count = 0;
    u64 current_estimated_bytes = 0;
    u64 peak_estimated_bytes = 0;
    u64 current_observed_bytes = 0;
    u64 peak_observed_bytes = 0;
    u64 reservation_count = 0;
    u64 release_count = 0;
    u64 cancel_cleanup_count = 0;
    u64 owner_cleanup_count = 0;
    u64 over_limit_refusal_count = 0;
  };

  struct ReservationRecord {
    ForeignMemoryReservationRequest request;
    ForeignMemoryReservationToken token;
    u64 observed_bytes = 0;
    ForeignMemoryConfidence confidence = ForeignMemoryConfidence::unknown;
    std::shared_ptr<ForeignMemoryReservation::HandleState> state;
  };

  ForeignMemoryReservationReleaseResult ReleaseReservation(
      ForeignMemoryReservationToken token,
      ForeignMemoryReleaseEvent event);
  ForeignMemoryReservationObservationResult UpdateObservedBytes(
      ForeignMemoryReservationToken token,
      u64 observed_bytes,
      ForeignMemoryConfidence confidence);

  void ApplyReservationLocked(const ReservationRecord& record);
  void ApplyObservedDeltaLocked(ReservationRecord* record,
                                u64 observed_bytes,
                                ForeignMemoryConfidence confidence);
  void ApplyReleaseLocked(const ReservationRecord& record,
                          ForeignMemoryReleaseEvent event);
  ForeignMemoryActiveReservationSnapshot SnapshotForRecordLocked(
      const ReservationRecord& record) const;

  mutable std::mutex mutex_;
  std::map<u64, ReservationRecord> records_;
  std::map<ForeignMemorySource, BucketAccounting> source_accounting_;
  std::map<std::string, BucketAccounting> owning_scope_accounting_;
  std::atomic<u64> next_reservation_id_{1};
  u64 current_estimated_bytes_ = 0;
  u64 peak_estimated_bytes_ = 0;
  u64 current_observed_bytes_ = 0;
  u64 peak_observed_bytes_ = 0;
  u64 reservation_count_ = 0;
  u64 release_count_ = 0;
  u64 cancel_cleanup_count_ = 0;
  u64 owner_cleanup_count_ = 0;
  u64 over_limit_refusal_count_ = 0;
  u64 fail_closed_refusal_count_ = 0;
  u64 failed_release_count_ = 0;

  friend class ForeignMemoryReservation;
};

const char* ForeignMemorySourceName(ForeignMemorySource source);
const char* ForeignMemoryConfidenceName(ForeignMemoryConfidence confidence);
const char* ForeignMemoryReleaseEventName(ForeignMemoryReleaseEvent event);
const char* ForeignMemoryOverLimitActionName(ForeignMemoryOverLimitAction action);
const char* ForeignMemoryLinkageModeName(ForeignMemoryLinkageMode mode);
MemoryCategory DefaultForeignMemoryCategory(ForeignMemorySource source);

ForeignMemoryReservationAcquireResult AcquireForeignMemoryReservation(
    ForeignMemoryReservationLedger* ledger,
    ForeignMemoryReservationRequest request);

}  // namespace scratchbird::core::memory

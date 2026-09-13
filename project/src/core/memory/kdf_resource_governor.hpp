// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "hierarchical_memory_budget_ledger.hpp"
#include "../common/scrypt_work_estimate.hpp"

namespace scratchbird::core::memory {

// Private runtime interfaces, never parser/transport authority. A cost is
// supplied by the owning algorithm adapter, not by a SQL parameter or client.
struct KdfResourceCost { u64 memory_bytes = 0, work_units = 0; };
using ScryptWorkEstimate = scratchbird::core::crypto::ScryptWorkEstimate;
using ScryptEstimateCode = scratchbird::core::crypto::ScryptEstimateCode;
using scratchbird::core::crypto::EstimateScryptWork;

struct KdfResourcePolicy {
  u64 call_memory_bytes = 0, call_work_units = 0;
  u64 aggregate_memory_bytes = 0, aggregate_work_units = 0;
  u64 statement_work_units = 0, active_calls = 0;
};
struct KdfResourceOwner {
  MemoryBinaryUuid process{}, database{}, session{}, statement{}, receipt{};
  bool operator==(const KdfResourceOwner& other) const {
    return process == other.process && database == other.database && session == other.session &&
        statement == other.statement && receipt == other.receipt;
  }
};
enum class KdfAdmissionCode { ok, invalid_owner, revoked, invalid_cost, budget_exceeded };
struct KdfResourceObservation {
  u64 active_calls = 0, memory_bytes = 0, work_units = 0;
};

class KdfResourceReceipt;
class KdfResourceGovernor;
class KdfResourceGrant final {
 public:
  struct State;
  KdfResourceGrant();
  ~KdfResourceGrant();
  KdfResourceGrant(KdfResourceGrant&&) noexcept;
  KdfResourceGrant& operator=(KdfResourceGrant&&) noexcept;
  KdfResourceGrant(const KdfResourceGrant&) = delete;
  KdfResourceGrant& operator=(const KdfResourceGrant&) = delete;
  bool live() const;
  // The caller must quiesce and clear/free all charged payloads first.
  void Reset() noexcept;
 private:
  std::unique_ptr<State> state_;
  friend class KdfResourceReceipt;
};
struct KdfResourceAdmission {
  KdfAdmissionCode code = KdfAdmissionCode::budget_exceeded;
  KdfResourceGrant grant;
};

class KdfResourceReceipt final : public std::enable_shared_from_this<KdfResourceReceipt> {
 public:
  struct State;
  KdfResourceReceipt(const KdfResourceReceipt&) = delete;
  KdfResourceReceipt& operator=(const KdfResourceReceipt&) = delete;
  KdfResourceAdmission Acquire(const KdfResourceOwner&, KdfResourceCost);
  void Revoke() noexcept;
  u64 consumed_work_units() const;
 private:
  explicit KdfResourceReceipt(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
  friend class KdfResourceGovernor;
};

class KdfResourceGovernor final {
 public:
  struct State;
  KdfResourceGovernor(const KdfResourceGovernor&) = delete;
  KdfResourceGovernor& operator=(const KdfResourceGovernor&) = delete;
  // No implicit ledger/default policy: the engine must supply its shared
  // parent ledger and an explicit policy. Invalid configuration returns null.
  static std::shared_ptr<KdfResourceGovernor> Create(
      std::shared_ptr<HierarchicalMemoryBudgetLedger> shared_ledger,
      MemoryBinaryUuid process, MemoryBinaryUuid database, KdfResourcePolicy);
  // Engine statement owner only. All callers for a live statement share its
  // cumulative budget; changing its session or receipt cannot reset that budget.
  std::shared_ptr<KdfResourceReceipt> Issue(const KdfResourceOwner&);
  void StopAdmission() noexcept;
  KdfResourceObservation Observe() const;
 private:
  explicit KdfResourceGovernor(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};

} // namespace scratchbird::core::memory

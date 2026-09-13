// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "kdf_resource_governor.hpp"

#include <utility>

namespace scratchbird::core::memory {
namespace {
bool Valid(const KdfResourceOwner& owner) {
  return MemorySystemUuidValid(owner.process) && MemorySystemUuidValid(owner.database) &&
      MemorySystemUuidValid(owner.session) && MemorySystemUuidValid(owner.statement) &&
      MemorySystemUuidValid(owner.receipt);
}
} // namespace

struct KdfResourceGovernor::State {
  std::mutex mutex;
  std::shared_ptr<HierarchicalMemoryBudgetLedger> ledger;
  MemoryBinaryUuid process{}, database{};
  KdfResourcePolicy policy;
  bool active = true;
  KdfResourceObservation used;
  std::map<MemoryBinaryUuid, std::weak_ptr<KdfResourceReceipt>> receipts;
};
struct KdfResourceReceipt::State {
  std::shared_ptr<KdfResourceGovernor::State> runtime;
  KdfResourceOwner owner;
  bool active = true;
  u64 consumed = 0;
};
struct KdfResourceGrant::State {
  std::shared_ptr<KdfResourceReceipt::State> receipt;
  std::shared_ptr<KdfResourceReceipt> control_owner;
  HierarchicalMemoryReservationToken token;
  HierarchicalMemoryReservationLease retained;
  KdfResourceCost cost;
  bool charged = false;
  ~State() {
    if (!receipt) return;
    auto& runtime = *receipt->runtime;
    // Revoke the token, then drop its payload retention. Both operations are
    // allocation-free; ownership stays charged until after retained cleanup.
    if (token.valid()) (void)runtime.ledger->ReleaseNoAlloc(token);
    (void)retained.Reset();
    if (charged) {
      std::lock_guard lock(runtime.mutex);
      --runtime.used.active_calls;
      runtime.used.memory_bytes -= cost.memory_bytes;
      runtime.used.work_units -= cost.work_units;
    }
  }
};

KdfResourceGrant::KdfResourceGrant() = default;
KdfResourceGrant::~KdfResourceGrant() = default;
KdfResourceGrant::KdfResourceGrant(KdfResourceGrant&&) noexcept = default;
KdfResourceGrant& KdfResourceGrant::operator=(KdfResourceGrant&&) noexcept = default;
void KdfResourceGrant::Reset() noexcept { state_.reset(); }
bool KdfResourceGrant::live() const {
  if (!state_) return false;
  std::lock_guard lock(state_->receipt->runtime->mutex);
  return state_->charged && state_->receipt->active && state_->receipt->runtime->active &&
      state_->retained.live();
}

KdfResourceGovernor::KdfResourceGovernor(std::shared_ptr<State> state) : state_(std::move(state)) {}
std::shared_ptr<KdfResourceGovernor> KdfResourceGovernor::Create(
    std::shared_ptr<HierarchicalMemoryBudgetLedger> ledger,
    MemoryBinaryUuid process, MemoryBinaryUuid database, KdfResourcePolicy policy) {
  if (!ledger || !MemorySystemUuidValid(process) || !MemorySystemUuidValid(database) ||
      !policy.call_memory_bytes || !policy.call_work_units || !policy.aggregate_memory_bytes ||
      !policy.aggregate_work_units || !policy.statement_work_units || !policy.active_calls ||
      policy.call_memory_bytes > policy.aggregate_memory_bytes ||
      policy.call_work_units > policy.aggregate_work_units ||
      policy.call_work_units > policy.statement_work_units) return {};
  auto state = std::make_shared<State>();
  state->ledger = std::move(ledger); state->process = process; state->database = database; state->policy = policy;
  return std::shared_ptr<KdfResourceGovernor>(new KdfResourceGovernor(std::move(state)));
}
KdfResourceReceipt::KdfResourceReceipt(std::shared_ptr<State> state) : state_(std::move(state)) {}
std::shared_ptr<KdfResourceReceipt> KdfResourceGovernor::Issue(const KdfResourceOwner& owner) {
  if (!Valid(owner) || owner.process != state_->process || owner.database != state_->database) return {};
  std::lock_guard lock(state_->mutex);
  if (!state_->active) return {};
  auto existing = state_->receipts.find(owner.statement);
  if (existing != state_->receipts.end()) {
    if (auto live = existing->second.lock()) {
      return live->state_->owner == owner && live->state_->active ? live : nullptr;
    }
  }
  // Prune only expired control entries, never a live statement's work budget.
  for (auto it = state_->receipts.begin(); it != state_->receipts.end();) {
    if (it->second.expired()) it = state_->receipts.erase(it); else ++it;
  }
  auto receipt_state = std::make_shared<KdfResourceReceipt::State>();
  receipt_state->runtime = state_; receipt_state->owner = owner;
  auto receipt = std::shared_ptr<KdfResourceReceipt>(new KdfResourceReceipt(std::move(receipt_state)));
  state_->receipts.emplace(owner.statement, receipt);
  return receipt;
}
void KdfResourceGovernor::StopAdmission() noexcept {
  std::lock_guard lock(state_->mutex); state_->active = false;
}
KdfResourceObservation KdfResourceGovernor::Observe() const {
  std::lock_guard lock(state_->mutex); return state_->used;
}
void KdfResourceReceipt::Revoke() noexcept {
  std::lock_guard lock(state_->runtime->mutex); state_->active = false;
}
u64 KdfResourceReceipt::consumed_work_units() const {
  std::lock_guard lock(state_->runtime->mutex); return state_->consumed;
}
KdfResourceAdmission KdfResourceReceipt::Acquire(const KdfResourceOwner& owner, KdfResourceCost cost) {
  KdfResourceAdmission result;
  auto& runtime = *state_->runtime;
  if (!(owner == state_->owner)) { result.code = KdfAdmissionCode::invalid_owner; return result; }
  if (!cost.memory_bytes || !cost.work_units) { result.code = KdfAdmissionCode::invalid_cost; return result; }
  // Destruction of an uncharged provisional grant never takes the runtime
  // mutex, so every allocating admission step may safely unwind under it.
  auto grant = std::make_unique<KdfResourceGrant::State>();
  grant->receipt = state_; grant->cost = cost;
  grant->control_owner = shared_from_this();
  std::lock_guard lock(runtime.mutex);
  if (!runtime.active || !state_->active) { result.code = KdfAdmissionCode::revoked; return result; }
  const auto& policy = runtime.policy;
  if (cost.memory_bytes > policy.call_memory_bytes || cost.work_units > policy.call_work_units ||
      cost.memory_bytes > policy.aggregate_memory_bytes - runtime.used.memory_bytes ||
      cost.work_units > policy.aggregate_work_units - runtime.used.work_units ||
      cost.work_units > policy.statement_work_units - state_->consumed ||
      runtime.used.active_calls >= policy.active_calls) return result;
  HierarchicalMemoryReservationRequest request;
  request.scope_chain = {
    {HierarchicalMemoryScopeKind::process, {}, owner.process},
    {HierarchicalMemoryScopeKind::database, {}, owner.database},
    {HierarchicalMemoryScopeKind::session, {}, owner.session},
    {HierarchicalMemoryScopeKind::statement, {}, owner.statement}};
  request.category = MemoryCategory::executor_query_reserved;
  request.memory_class = "kdf_working_extent";
  request.requested_bytes = cost.memory_bytes; request.binary_owner_uuid = owner.receipt;
  request.cancelable = true;
  request.provenance.source = HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  request.provenance.source_label = "engine_kdf_resource_governor";
  auto reservation = runtime.ledger->Reserve(std::move(request));
  if (!reservation.ok()) return result;
  grant->token = reservation.token;
  if (!runtime.ledger->Commit(grant->token).ok()) return result;
  auto retained = runtime.ledger->Retain(grant->token);
  if (!retained.ok()) return result;
  grant->retained = std::move(retained.lease);
  // All fallible work precedes both CPU debit and admission publication.
  ++runtime.used.active_calls;
  runtime.used.memory_bytes += cost.memory_bytes;
  runtime.used.work_units += cost.work_units;
  state_->consumed += cost.work_units;
  grant->charged = true;
  result.grant.state_ = std::move(grant);
  result.code = KdfAdmissionCode::ok;
  return result;
}
} // namespace scratchbird::core::memory

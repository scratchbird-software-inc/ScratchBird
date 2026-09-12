// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "dml/update_resource_authority_provider.hpp"
#include "api_diagnostics.hpp"
#include "local_transaction_store.hpp"
#include "resource_governance_admission.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <tuple>

namespace scratchbird::engine::internal_api {
namespace wire = scratchbird::wire;
namespace resources = scratchbird::core::agents;
namespace mga = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

struct EngineDmlUpdateResourceHandleV1::Lease {
  std::weak_ptr<EngineDmlUpdateResourceGovernorV1::State> owner;
  EngineRequestContext context;
  wire::TypedUpdateResourceBudgetCarrier carrier;
  std::string reservation_token;
  bool cancelled = false;
  bool publication_pending = false;
  bool descriptor_published = false;
  bool released = false;
  EngineDmlUpdateResourceReleaseV1 release_reason{};
};

struct EngineDmlUpdateResourceGovernorV1::State {
  mutable std::mutex mutex;
  std::atomic<bool> accepting{true};
  EngineDmlUpdateResourcePolicyV1 policy;
  std::uint64_t generation = 0;
  std::uint64_t next_grant_generation = 0;
  std::uint64_t released_grants = 0;
  std::unique_ptr<resources::HierarchicalMemoryBudgetLedger> ledger;
  std::map<wire::TypedUpdateUuid, std::shared_ptr<EngineDmlUpdateResourceHandleV1::Lease>> active;
};

struct EngineDmlUpdateResourcePublicationV1::Pending {
  std::shared_ptr<EngineDmlUpdateResourceGovernorV1::State> owner;
  std::shared_ptr<EngineDmlUpdateResourceHandleV1::Lease> lease;
  bool completed = false;  // All mutable fields protected by owner->mutex.
  EngineDmlUpdateResourcePublicationOutcomeV1 outcome{};
};

EngineDmlUpdateResourcePublicationCodeV1 EngineDmlUpdateResourcePublicationV1::CompleteNoAlloc(
    EngineDmlUpdateResourcePublicationOutcomeV1 outcome) const noexcept {
  using Code = EngineDmlUpdateResourcePublicationCodeV1;
  using Outcome = EngineDmlUpdateResourcePublicationOutcomeV1;
  if (!pending_) return Code::invalid_handle;
  if (outcome != Outcome::published && outcome != Outcome::not_published)
    return Code::invalid_disposition;
  try {
    std::lock_guard lock(pending_->owner->mutex);
    if (pending_->completed)
      return pending_->outcome == outcome ? Code::already_completed : Code::conflicting_disposition;
    auto& lease = *pending_->lease;
    if (!lease.publication_pending || lease.descriptor_published || lease.released)
      return Code::phase_mismatch;
    // These are resource bookkeeping changes, not a live-work authorization.
    // The durable owner decides the outcome, including after revocation/stop.
    lease.descriptor_published = outcome == Outcome::published;
    lease.publication_pending = false;
    pending_->outcome = outcome;
    pending_->completed = true;
    return Code::completed;
  } catch (...) {
    return Code::synchronization_failed;
  }
}

namespace {
EngineApiDiagnostic Refuse(std::string detail, std::string code = "RESOURCE.BUDGET_EXCEEDED") {
  return MakeEngineApiDiagnostic(std::move(code), "sblr.dml_update_rows.resource_authority",
                                 std::move(detail), true);
}
EngineApiDiagnostic Ok() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}
bool ExactUuid(const std::string& text) {
  const auto parsed = uuid::ParseUuid(text);
  return parsed.ok() && !parsed.value.is_nil() && uuid::UuidToString(parsed.value) == text;
}
wire::TypedUpdateUuid BinaryUuid(const std::string& text) {
  const auto parsed = uuid::ParseUuid(text);
  wire::TypedUpdateUuid result{};
  if (parsed.ok()) std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), result.begin());
  return result;
}
bool IssueUuid(wire::TypedUpdateUuid* out) {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  const auto issued = uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object, static_cast<std::uint64_t>(now));
  if (!issued.ok()) return false;
  std::copy(issued.value.value.bytes.begin(), issued.value.value.bytes.end(), out->begin());
  return true;
}
auto ContextKey(const EngineRequestContext& c) {
  return std::tie(c.database_path, c.database_uuid, c.session_uuid,
      c.principal_uuid, c.transaction_uuid, c.local_transaction_id,
      c.statement_uuid, c.statement_receipt_uuid,
      c.statement_snapshot_uuid, c.statement_snapshot_generation,
      c.statement_metadata_snapshot_uuid, c.statement_metadata_snapshot_engine_owned,
      c.snapshot_visible_through_local_transaction_id,
      c.statement_metadata_snapshot_visible_through_local_transaction_id,
      c.statement_metadata_snapshot_active_excluded_local_transaction_ids,
      c.statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids,
      c.catalog_generation_id, c.catalog_epoch_uuid, c.resource_epoch,
      c.resource_admission_uuid, c.security_context_present, c.security_epoch,
      c.authorization_context.present, c.authorization_context.authority_uuid,
      c.authorization_context.security_context_generation, c.authorization_context.principal_uuid,
      c.authorization_context.catalog_generation_id, c.authorization_context.security_epoch,
      c.authorization_context.policy_epoch, c.current_role_uuid,
      c.transaction_policy_snapshot_uuid, c.transaction_policy_snapshot_generation,
      c.datatype_catalog_snapshot_uuid, c.datatype_catalog_generation,
      c.datatype_registry_generation, c.transaction_isolation_level,
      c.read_only_mode, c.cluster_transaction_active, c.route_fence_present);
}
EngineApiDiagnostic ValidateContext(const EngineRequestContext& c) {
  if (c.read_only_mode || c.cluster_transaction_active || c.route_fence_present)
    return Refuse("read-write unfenced local MGA context required", "SBLR.OPERATION_UNSUPPORTED");
  if (!c.security_context_present || !c.authorization_context.present ||
      !c.statement_metadata_snapshot_engine_owned || !c.local_transaction_id || !c.resource_epoch ||
      !ExactUuid(c.database_uuid) || !ExactUuid(c.transaction_uuid) ||
      !ExactUuid(c.statement_uuid) || !ExactUuid(c.session_uuid) ||
      !ExactUuid(c.principal_uuid) || !ExactUuid(c.statement_receipt_uuid) ||
      !ExactUuid(c.statement_snapshot_uuid) ||
      !ExactUuid(c.statement_metadata_snapshot_uuid) ||
      !ExactUuid(c.resource_admission_uuid) ||
      !ExactUuid(c.authorization_context.authority_uuid) ||
      !c.authorization_context.security_context_generation || !c.security_epoch ||
      !c.authorization_context.policy_epoch || !c.catalog_generation_id ||
      c.authorization_context.principal_uuid != c.principal_uuid ||
      c.authorization_context.catalog_generation_id != c.catalog_generation_id ||
      c.authorization_context.security_epoch != c.security_epoch)
    return Refuse("authenticated receipt resource context is incomplete", "SECURITY.ACCESS_DENIED");
  const auto loaded = scratchbird::storage::database::AcquireLocalTransactionInventorySnapshot(c.database_path);
  if (!loaded.ok()) return Refuse("MGA inventory unavailable", "DML.UPDATE_FAILED");
  const auto txn = mga::LookupLocalTransaction(loaded.snapshot->inventory,
                                              mga::MakeLocalTransactionId(c.local_transaction_id));
  if (!txn.ok() || txn.entry.state != mga::TransactionState::active || txn.entry.rollback_only ||
      txn.entry.identity.transaction_uuid.value != uuid::ParseUuid(c.transaction_uuid).value)
    return Refuse("owning MGA transaction is not active", "MGA.TRANSACTION.STALE");
  return Ok();
}
bool Match(const EngineRequestContext& c, const std::shared_ptr<EngineDmlUpdateResourceGovernorV1::State>& s,
           const std::shared_ptr<EngineDmlUpdateResourceHandleV1::Lease>& lease) {
  return lease && lease->owner.lock() == s && ContextKey(c) == ContextKey(lease->context);
}
bool Cancelled(const EngineRequestContext& c) {
  try { return c.query_cancellation_requested && c.query_cancellation_requested(); }
  catch (...) { return true; }  // A failed engine probe cannot authorize work.
}
bool ReceiptMatches(const EngineRequestContext& c, const EngineDmlUpdateResourceGovernorV1* governor) {
  const std::weak_ptr<EngineDmlUpdateResourceReceiptV1> empty;
  const auto& binding = c.dml_update_resource_receipt;
  if (!binding.owner_before(empty) && !empty.owner_before(binding)) return true;
  const auto receipt = binding.lock();
  return receipt && receipt->Matches(c, governor);
}
}  // namespace

EngineDmlUpdateResourceReceiptV1::EngineDmlUpdateResourceReceiptV1(
    const EngineRequestContext& context, std::shared_ptr<EngineDmlUpdateResourceGovernorV1> governor)
    : context_(context), governor_(std::move(governor)) {}

bool EngineDmlUpdateResourceReceiptV1::Matches(const EngineRequestContext& c,
    const EngineDmlUpdateResourceGovernorV1* governor) const noexcept {
  return active_.load(std::memory_order_acquire) && governor && governor_.get() == governor &&
         ContextKey(c) == ContextKey(context_);
}
EngineDmlUpdateResourceCaptureV1 EngineDmlUpdateResourceReceiptV1::Capture(const EngineRequestContext& c) {
  if (!Matches(c, governor_.get()) || c.dml_update_resource_receipt.lock().get() != this) {
    EngineDmlUpdateResourceCaptureV1 result;
    result.diagnostic = Refuse("resource receipt is revoked or owned by another context", "MGA.TRANSACTION.STALE");
    return result;
  }
  auto admitted = c;
  // Context copies cannot replace the callback installed by receipt issuance.
  admitted.query_cancellation_requested = context_.query_cancellation_requested;
  auto result = governor_->Capture(admitted);
  if (result.ok && !active_.load(std::memory_order_acquire)) {
    const auto released = governor_->ReleaseNoAlloc(c, result.handle,
        EngineDmlUpdateResourceReleaseV1::abandoned_before_publication);
    if (released == EngineDmlUpdateResourceReleaseCodeV1::released ||
        released == EngineDmlUpdateResourceReleaseCodeV1::already_released) result.handle = {};
    result.ok = false;
    result.diagnostic = Refuse("resource receipt revoked during capture", "MGA.TRANSACTION.STALE");
  }
  return result;
}
void EngineDmlUpdateResourceReceiptV1::Revoke() noexcept {
  // No lock/callback: cancellation probes may reentrantly release a receipt.
  active_.store(false, std::memory_order_release);
}
bool EngineDmlUpdateResourceReceiptV1::IsRevoked() const noexcept {
  return !active_.load(std::memory_order_acquire);
}
bool EngineDmlUpdateResourceReceiptV1::AuthenticatesContext(const EngineRequestContext& c) const noexcept {
  return governor_ && Matches(c, governor_.get()) && c.dml_update_resource_receipt.lock().get() == this;
}
bool EngineDmlUpdateResourceReceiptV1::SharesRuntimeWith(
    const EngineDmlUpdateResourceReceiptV1& other) const noexcept {
  return governor_ && governor_ == other.governor_;
}
bool EngineDmlUpdateResourceReceiptV1::BelongsToRuntime(
    const EngineDmlUpdateResourceGovernorV1* governor) const noexcept {
  return governor != nullptr && governor_.get() == governor;
}
EngineApiDiagnostic EngineDmlUpdateResourceReceiptV1::Revalidate(
    const EngineRequestContext& c, const EngineDmlUpdateResourceHandleV1& h) const {
  if (!governor_ || !h.valid() || c.dml_update_resource_receipt.lock().get() != this)
    return Refuse("resource receipt handle differs", "MGA.TRANSACTION.STALE");
  return governor_->Revalidate(c, h, *h.carrier());
}
EngineDmlUpdateResourceReleaseCodeV1 EngineDmlUpdateResourceReceiptV1::ReleaseNoAlloc(
    const EngineRequestContext& c, const EngineDmlUpdateResourceHandleV1& h,
    EngineDmlUpdateResourceReleaseV1 reason) noexcept {
  if (!governor_ || ContextKey(c) != ContextKey(context_) ||
      c.dml_update_resource_receipt.lock().get() != this)
    return EngineDmlUpdateResourceReleaseCodeV1::owner_mismatch;
  return governor_->ReleaseNoAlloc(c, h, reason);
}
EngineDmlUpdateResourcePublicationPreparationV1
EngineDmlUpdateResourceReceiptV1::PrepareDescriptorPublication(
    const EngineRequestContext& c, const EngineDmlUpdateResourceHandleV1& h) {
  if (!governor_ || !Matches(c, governor_.get()) || c.dml_update_resource_receipt.lock().get() != this) {
    EngineDmlUpdateResourcePublicationPreparationV1 result;
    result.diagnostic = Refuse("resource receipt differs or was revoked", "MGA.TRANSACTION.STALE");
    return result;
  }
  return governor_->PrepareDescriptorPublication(c, h);
}

const wire::TypedUpdateResourceBudgetCarrier* EngineDmlUpdateResourceHandleV1::carrier() const noexcept {
  return lease_ ? &lease_->carrier : nullptr;
}
EngineDmlUpdateResourceGovernorV1::EngineDmlUpdateResourceGovernorV1() : state_(std::make_shared<State>()) {}
void EngineDmlUpdateResourceGovernorV1::StopAdmission() noexcept {
  state_->accepting.store(false, std::memory_order_release);
}

EngineApiDiagnostic EngineDmlUpdateResourceGovernorV1::Configure(const EngineDmlUpdateResourcePolicyV1& p) {
  if (!p.maximum_assignments || p.maximum_assignments > wire::kTypedUpdateMaximumAssignments ||
      !p.maximum_predicate_nodes || p.maximum_predicate_nodes > wire::kTypedUpdateMaximumPredicateNodes ||
      !p.maximum_candidate_rows || p.maximum_candidate_rows > wire::kTypedUpdateMaximumCandidateRows ||
      !p.maximum_trigger_depth || p.maximum_trigger_depth > wire::kTypedUpdateMaximumTriggerDepth ||
      !p.maximum_effects || p.maximum_effects > wire::kTypedUpdateMaximumEffects ||
      !p.maximum_total_canonical_value_bytes || p.maximum_total_canonical_value_bytes > wire::kTypedUpdateMaximumCanonicalValueBytes ||
      p.aggregate_canonical_value_bytes < p.maximum_total_canonical_value_bytes ||
      !p.maximum_active_grants || p.maximum_active_grants > 1024 ||
      p.aggregate_canonical_value_bytes > p.maximum_total_canonical_value_bytes * p.maximum_active_grants)
    return Refuse("governor policy is missing or exceeds the admitted bounds");
  std::lock_guard lock(state_->mutex);
  if (!state_->accepting.load(std::memory_order_acquire) || !state_->active.empty() ||
      state_->generation == std::numeric_limits<std::uint64_t>::max())
    return Refuse("policy replacement requires drained grants and an available generation");
  auto ledger = std::make_unique<resources::HierarchicalMemoryBudgetLedger>("dml.update_rows.canonical_values");
  resources::HierarchicalMemoryBudgetScope scope;
  scope.scope_id = "canonical_values";
  scope.kind = resources::HierarchicalMemoryBudgetScopeKind::kOperator;
  scope.limit_bytes = p.aggregate_canonical_value_bytes;
  if (!ledger->RegisterScope(scope).ok) return Refuse("governor capacity registration failed");
  state_->ledger = std::move(ledger);
  state_->policy = p;
  ++state_->generation;
  return Ok();
}

EngineDmlUpdateResourceCaptureV1 EngineDmlUpdateResourceGovernorV1::Capture(const EngineRequestContext& c) {
  EngineDmlUpdateResourceCaptureV1 result;
  if (!state_->accepting.load(std::memory_order_acquire) || !ReceiptMatches(c, this)) {
    result.diagnostic = Refuse("resource receipt is stale or has a different governor", "MGA.TRANSACTION.STALE");
    return result;
  }
  result.diagnostic = ValidateContext(c);
  if (result.diagnostic.error) return result;
  if (Cancelled(c)) {
    result.diagnostic = Refuse("engine cancellation probe refused capture", "PROCESS.CANCELLED");
    return result;
  }
  auto lease = std::make_shared<EngineDmlUpdateResourceHandleV1::Lease>();
  lease->owner = state_;
  lease->context = c;
  auto& b = lease->carrier;
  if (!IssueUuid(&b.resource_budget_uuid) || !IssueUuid(&b.grant_receipt_uuid) ||
      !IssueUuid(&b.cancellation_token_uuid)) {
    result.diagnostic = Refuse("resource identity issuance failed");
    return result;
  }
  std::lock_guard lock(state_->mutex);
  if (!state_->accepting.load(std::memory_order_acquire) || !state_->ledger ||
      state_->active.size() >= state_->policy.maximum_active_grants ||
      state_->next_grant_generation == std::numeric_limits<std::uint64_t>::max()) {
    result.diagnostic = Refuse("governor is unconfigured or exhausted");
    return result;
  }
  b.resource_budget_generation = state_->generation;
  b.grant_receipt_generation = ++state_->next_grant_generation;
  b.cancellation_generation = b.grant_receipt_generation;
  b.authenticated_statement_receipt_uuid = BinaryUuid(c.statement_receipt_uuid);
  b.owning_transaction_uuid = BinaryUuid(c.transaction_uuid);
  const auto& p = state_->policy;
  b.maximum_assignments = p.maximum_assignments;
  b.maximum_predicate_nodes = p.maximum_predicate_nodes;
  b.maximum_candidate_rows = p.maximum_candidate_rows;
  b.maximum_trigger_depth = p.maximum_trigger_depth;
  b.maximum_effects = p.maximum_effects;
  b.maximum_total_canonical_value_bytes = p.maximum_total_canonical_value_bytes;
  std::vector<std::uint8_t> encoded;
  wire::TypedUpdateCarrierError error;
  if (!wire::EncodeTypedUpdateResourceBudget(b, &encoded, &error) ||
      !wire::DecodeAndValidateTypedUpdateResourceBudget(encoded, &b, &error)) {
    result.diagnostic = Refuse("governor DUBR encoding failed");
    return result;
  }
  resources::HierarchicalMemoryBudgetReserveRequest request;
  request.operation_id = "dml.update_rows";
  request.owner_scope = c.statement_receipt_uuid;
  request.leaf_scope_id = "canonical_values";
  request.bytes = b.maximum_total_canonical_value_bytes;
  // Prepare request bookkeeping first, then retain the owner before debiting.
  // The ledger's strong exception guarantee permits removal of this provisional
  // owner if Reserve throws; no capacity or live token exists in that case.
  const auto [owner, inserted] = state_->active.emplace(b.resource_budget_uuid, lease);
  if (!inserted) {
    result.diagnostic = Refuse("resource identity collision");
    return result;
  }
  resources::HierarchicalMemoryBudgetReserveResult reserved;
  try {
    reserved = state_->ledger->Reserve(std::move(request));
  } catch (...) {
    state_->active.erase(owner);
    throw;
  }
  if (!reserved.ok) {
    state_->active.erase(b.resource_budget_uuid);
    result.diagnostic = Refuse("aggregate canonical-value capacity exhausted");
    return result;
  }
  lease->reservation_token = std::move(reserved.reservation.token_id);
  result.handle.lease_ = std::move(lease);
  result.ok = true;
  return result;
}

EngineApiDiagnostic EngineDmlUpdateResourceGovernorV1::Revalidate(const EngineRequestContext& c,
    const EngineDmlUpdateResourceHandleV1& h, const wire::TypedUpdateResourceBudgetCarrier& supplied) const {
  if (!state_->accepting.load(std::memory_order_acquire) ||
      !ReceiptMatches(c, this) || (h.lease_ && !ReceiptMatches(h.lease_->context, this)))
    return Refuse("resource receipt was revoked", "MGA.TRANSACTION.STALE");
  auto diagnostic = ValidateContext(c);
  if (diagnostic.error) return diagnostic;
  std::unique_lock lock(state_->mutex);
  if (!Match(c, state_, h.lease_) || h.lease_->released || h.lease_->publication_pending ||
      h.lease_->carrier.resource_budget_generation != state_->generation)
    return Refuse("resource handle is absent, stale or owned by another receipt");
  std::vector<std::uint8_t> encoded;
  wire::TypedUpdateCarrierError error;
  if (!wire::EncodeTypedUpdateResourceBudget(supplied, &encoded, &error) ||
      encoded != supplied.exact_bytes || encoded != h.lease_->carrier.exact_bytes ||
      supplied.evidence_sha256 != h.lease_->carrier.evidence_sha256)
    return Refuse("DUBR fields and retained live governor bytes differ");
  lock.unlock();
  const bool cancelled = Cancelled(h.lease_->context);
  lock.lock();
  if (h.lease_->released || h.lease_->publication_pending)
    return Refuse("resource grant was released or entered publication during revalidation");
  if (!state_->accepting.load(std::memory_order_acquire) ||
      !ReceiptMatches(c, this) || !ReceiptMatches(h.lease_->context, this))
    return Refuse("resource receipt revoked during revalidation", "MGA.TRANSACTION.STALE");
  h.lease_->cancelled = h.lease_->cancelled || cancelled;
  if (h.lease_->cancelled) return Refuse("resource grant cancelled", "PROCESS.CANCELLED");
  return Ok();
}

EngineApiDiagnostic EngineDmlUpdateResourceGovernorV1::Cancel(const EngineRequestContext& c,
    const EngineDmlUpdateResourceHandleV1& h) {
  std::lock_guard lock(state_->mutex);
  if (!Match(c, state_, h.lease_) || h.lease_->released) return Refuse("cancellation owner is stale");
  h.lease_->cancelled = true;
  return Ok();
}
EngineDmlUpdateResourcePublicationPreparationV1
EngineDmlUpdateResourceGovernorV1::PrepareDescriptorPublication(const EngineRequestContext& c,
    const EngineDmlUpdateResourceHandleV1& h) {
  EngineDmlUpdateResourcePublicationPreparationV1 result;
  if (!h.valid()) {
    result.diagnostic = Refuse("resource handle is absent");
    return result;
  }
  result.diagnostic = Revalidate(c, h, *h.carrier());
  if (result.diagnostic.error) return result;
  // Finish every allocation before pinning the publication window. Exceptions
  // leave the captured grant unpublished and available for ordinary abandonment.
  auto pending = std::make_shared<EngineDmlUpdateResourcePublicationV1::Pending>();
  pending->owner = state_;
  pending->lease = h.lease_;
  std::lock_guard lock(state_->mutex);
  if (h.lease_->released || h.lease_->cancelled || h.lease_->publication_pending ||
      h.lease_->descriptor_published || !state_->accepting.load(std::memory_order_acquire) ||
      !ReceiptMatches(c, this) || !ReceiptMatches(h.lease_->context, this)) {
    result.diagnostic = Refuse("resource grant is no longer publishable");
    return result;
  }
  result.publication.pending_ = std::move(pending);
  h.lease_->publication_pending = true;
  result.ok = true;
  return result;
}
EngineApiDiagnostic EngineDmlUpdateResourceGovernorV1::PublishDescriptor(const EngineRequestContext& c,
    const EngineDmlUpdateResourceHandleV1& h) {
  auto prepared = PrepareDescriptorPublication(c, h);
  if (!prepared.ok) return std::move(prepared.diagnostic);
  const auto completed = prepared.publication.CompleteNoAlloc(
      EngineDmlUpdateResourcePublicationOutcomeV1::published);
  if (completed != EngineDmlUpdateResourcePublicationCodeV1::completed)
    return Refuse("resource publication bookkeeping failed");
  return std::move(prepared.diagnostic);
}
EngineApiDiagnostic EngineDmlUpdateResourceGovernorV1::Release(const EngineRequestContext& c,
    const EngineDmlUpdateResourceHandleV1& h, EngineDmlUpdateResourceReleaseV1 reason) {
  // Convenience API for allocating callers. Prepare success before mutation;
  // refusal diagnostics are built only when no resource state changed.
  auto success = Ok();
  using Code = EngineDmlUpdateResourceReleaseCodeV1;
  switch (ReleaseNoAlloc(c, h, reason)) {
    case Code::released:
    case Code::already_released: return success;
    case Code::owner_mismatch: return Refuse("release owner differs");
    case Code::invalid_disposition: return Refuse("invalid resource release disposition");
    case Code::conflicting_disposition: return Refuse("conflicting terminal resource disposition");
    case Code::phase_mismatch: return Refuse("resource release disposition does not match publication phase");
    default: return Refuse("resource reservation release failed");
  }
}
EngineDmlUpdateResourceReleaseCodeV1 EngineDmlUpdateResourceGovernorV1::ReleaseNoAlloc(
    const EngineRequestContext& c, const EngineDmlUpdateResourceHandleV1& h,
    EngineDmlUpdateResourceReleaseV1 reason) noexcept {
  using Code = EngineDmlUpdateResourceReleaseCodeV1;
  try {
    std::lock_guard lock(state_->mutex);
    if (!Match(c, state_, h.lease_)) return Code::owner_mismatch;
    if (reason != EngineDmlUpdateResourceReleaseV1::abandoned_before_publication &&
        reason != EngineDmlUpdateResourceReleaseV1::published && reason != EngineDmlUpdateResourceReleaseV1::aborted)
      return Code::invalid_disposition;
    if (h.lease_->released)
      return h.lease_->release_reason == reason ? Code::already_released : Code::conflicting_disposition;
    if (h.lease_->publication_pending) return Code::phase_mismatch;
    if ((reason == EngineDmlUpdateResourceReleaseV1::abandoned_before_publication) == h.lease_->descriptor_published)
      return Code::phase_mismatch;
    const auto owner = state_->active.find(h.lease_->carrier.resource_budget_uuid);
    if (!state_->ledger || owner == state_->active.end() || owner->second != h.lease_)
      return Code::ledger_failure;
    if (state_->ledger->ReleaseNoAlloc(h.lease_->reservation_token) !=
        resources::HierarchicalMemoryBudgetReleaseCode::released) return Code::ledger_failure;
    h.lease_->released = true;
    h.lease_->release_reason = reason;
    ++state_->released_grants;
    state_->active.erase(owner);
    return Code::released;
  } catch (...) {
    return Code::synchronization_failed;
  }
}
EngineDmlUpdateResourceObservationV1 EngineDmlUpdateResourceGovernorV1::Observe() const {
  std::lock_guard lock(state_->mutex);
  EngineDmlUpdateResourceObservationV1 result;
  result.policy_generation = state_->generation;
  result.active_grants = state_->active.size();
  result.released_grants = state_->released_grants;
  if (state_->ledger) {
    for (const auto& scope : state_->ledger->Snapshot()) result.reserved_canonical_bytes += scope.current_bytes;
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api

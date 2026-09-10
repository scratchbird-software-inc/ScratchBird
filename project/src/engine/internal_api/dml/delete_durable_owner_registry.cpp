// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/delete_durable_owner_registry.hpp"
#include "mga_relation_store/mga_relation_store_internal_support.hpp"
#include "dml/datatype_operator_registry_projection.hpp"
#include "api_diagnostics.hpp"
#include "transaction/transaction_api.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "mga_relation_store/mga_savepoint_marker_codec.hpp"
#include <map>
#include <mutex>
#include <tuple>

namespace scratchbird::engine::internal_api {
namespace {
namespace w = scratchbird::wire;
using Disposition = EngineDmlDeleteRecoveryDispositionV1;
using Publication = MgaDmlDeletePublicationStatusV1;
using Release = EngineDmlUpdateResourceReleaseV1;
using PublicationOutcome = EngineDmlUpdateResourcePublicationOutcomeV1;
using Key = std::tuple<std::string, w::TypedUpdateUuid, std::uint64_t>;
struct Owner {
  EngineRequestContext context;
  EngineDmlDeleteBindingAuthorityV1 binding;
  EngineDmlUpdateResourceHandleV1 resource;
  EngineDmlUpdateResourcePublicationV1 ticket;
  std::shared_ptr<EngineDmlUpdateResourceReceiptV1> receipt;
  w::TypedUpdateUuid descriptor_uuid{};
  std::uint64_t generation = 0;
  std::mutex mutex;
  bool released = false;
};
std::mutex owners_mutex;
std::map<Key, std::shared_ptr<Owner>> owners;
EngineApiDiagnostic Error(std::string detail, std::string code = "DML.DELETE_FAILED") {
  return MakeEngineApiDiagnostic(std::move(code), "sblr.dml_delete_rows.durable_owner", std::move(detail), true);
}
EngineApiDiagnostic Ok() { return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false); }
EngineRequestContext PhaseContext(const EngineRequestContext& c, const char* phase) {
  auto value = c;
  std::erase_if(value.trace_tags, [](const auto& tag) { return tag.starts_with("private_dml_delete_rows_"); });
  value.trace_tags.emplace_back(phase); return value;
}
bool Phase(const EngineRequestContext& c, std::string_view expected) {
  bool found = false;
  for (const auto& tag : c.trace_tags) {
    if (tag == expected) found = true;
    else if (tag.starts_with("private_dml_delete_rows_") || tag.starts_with("private_dml_update_rows_")) return false;
  }
  return found;
}
bool FinishTicket(Owner& owner, PublicationOutcome outcome) noexcept {
  if (!owner.ticket.valid()) return outcome == PublicationOutcome::not_published;
  const auto result = owner.ticket.CompleteNoAlloc(outcome);
  return result == EngineDmlUpdateResourcePublicationCodeV1::completed ||
      result == EngineDmlUpdateResourcePublicationCodeV1::already_completed;
}
bool ReleaseOwner(Owner& owner, Release reason) noexcept {
  if (owner.released) return true;
  const auto code = owner.receipt->ReleaseNoAlloc(owner.context, owner.resource, reason);
  if (code != EngineDmlUpdateResourceReleaseCodeV1::released &&
      code != EngineDmlUpdateResourceReleaseCodeV1::already_released) return false;
  owner.released = true; return true;
}
// Call only after releasing an execution lease/owner lock. Keeping a released
// owner briefly is harmless; dropping an unresolved owner is not.
void PruneReleased() {
  std::lock_guard guard(owners_mutex);
  std::erase_if(owners, [](const auto& item) {
    std::unique_lock lock(item.second->mutex, std::try_to_lock);
    return lock.owns_lock() && item.second->released;
  });
}
EngineDmlDeleteRecoveryObservationV1 ReconcileStore(const EngineRequestContext& context,
    MgaDmlDeleteDurableStoreV1& store, Owner* owner) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(context.database_path);
  EngineDmlDeleteRecoveryObservationV1 result;
  if (store.chain().empty()) {
    if (owner && FinishTicket(*owner, PublicationOutcome::not_published))
      (void)ReleaseOwner(*owner, Release::abandoned_before_publication);
    result.diagnostic = Error("no_bound_operation"); return result;
  }
  DmlDeleteDurableAuthorityBundleV1 bundle;
  if (!store.LoadAuthorityBundle(&bundle, &result.diagnostic)) return result;
  if (owner && !FinishTicket(*owner, PublicationOutcome::published)) {
    result.diagnostic = Error("publication_handoff_unresolved"); return result;
  }
  result = ObserveDmlDeleteRecoveryAuthorityV1(context, store.chain());
  if (!result.ok) return result;
  if (result.disposition == Disposition::abandon_unexecuted) {
    const auto marker = ObserveUniqueMgaSavepointMarkerV1(context,
        MgaSavepointUuidKey(datatype_operator_projection::UuidText(bundle.reserved_statement_savepoint_uuid)));
    if (!marker.ok || marker.lifecycle == MgaSavepointMarkerLifecycle::released) {
      result.ok = false; result.diagnostic = Error("unbound_reserved_marker_history_invalid"); return result;
    }
    if (marker.lifecycle == MgaSavepointMarkerLifecycle::active) {
      if (marker.rolled_back) {
        result.ok = false; result.diagnostic = Error("unbound_reserved_marker_was_directly_rewound"); return result;
      }
      auto intent = result.head;
      intent.lifecycle_state = w::TypedUpdateJournalState::intent; ++intent.journal_sequence;
      intent.prior_record_sha256 = result.head.record_evidence_sha256;
      intent.statement_savepoint_uuid = bundle.reserved_statement_savepoint_uuid;
      intent.statement_savepoint_generation = marker.creation_ordinal;
      if (!store.Append(intent, &result.diagnostic)) { result.ok = false; return result; }
      result = ObserveDmlDeleteRecoveryAuthorityV1(context, store.chain());
      if (!result.ok) return result;
    }
  }
  switch (result.disposition) {
    case Disposition::abandon_unexecuted:
    case Disposition::rollback_statement:
    case Disposition::statement_already_rewound:
      if (store.AbortBeforePublication() != MgaDmlDeleteAbortStatusV1::aborted) {
        result.ok = false; result.diagnostic = Error("statement_abort_requires_retry"); return result;
      }
      result = ObserveDmlDeleteRecoveryAuthorityV1(context, store.chain());
      break;
    case Disposition::publish_prepared_result:
      if (store.CompleteReleasedPublication() != Publication::published) {
        result.ok = false; result.diagnostic = Error("publication_requires_retry"); return result;
      }
      result = ObserveDmlDeleteRecoveryAuthorityV1(context, store.chain());
      break;
    default: break;
  }
  if (result.ok && owner) {
    if (result.disposition == Disposition::published) (void)ReleaseOwner(*owner, Release::published);
    else if (result.disposition == Disposition::aborted || result.disposition == Disposition::transaction_rolled_back)
      (void)ReleaseOwner(*owner, Release::aborted);
  }
  return result;
}
void Retire(const std::shared_ptr<Owner>& owner) noexcept {
  try {
    std::unique_lock lock(owner->mutex, std::try_to_lock);
    if (!lock.owns_lock() || owner->released) return;
    EngineApiDiagnostic diagnostic;
    auto store = MgaDmlDeleteDurableStoreV1::Open(owner->context, owner->descriptor_uuid, owner->generation, &diagnostic);
    if (store) (void)ReconcileStore(owner->context, *store, owner.get());
  } catch (...) {} // Retained original owner/ticket/grant remains retryable.
}
}  // namespace

DmlDeleteBoundPublicationV1 PublishDmlDeleteBoundAuthorityV1(const EngineRequestContext& context,
    const EngineDmlDeleteBindingAuthorityV1& binding, const EngineDmlUpdateResourceHandleV1& resource) {
  DmlDeleteBoundPublicationV1 result;
  // Prepare failure/success diagnostics before taking ownership or writing.
  auto failure = Error("publication_interrupted_owner_retained");
  auto success = Ok();
  std::shared_ptr<Owner> owner;
  try {
    if (!Phase(context, "private_dml_delete_rows_binder") || !binding.valid() || !resource.valid()) {
      result.diagnostic = Error("private_complete_binding_required", "SECURITY.ACCESS_DENIED"); return result;
    }
    const auto& bundle = *binding.bundle();
    result.diagnostic = RevalidateDmlDeleteBindingAuthorityV1(
        PhaseContext(context, "private_dml_delete_rows_consumer"), binding);
    if (result.diagnostic.error) return result;
    if (resource.carrier()->exact_bytes != bundle.resource_budget.exact_bytes) {
      result.diagnostic = Error("original_resource_handle_required"); return result;
    }
    owner = std::make_shared<Owner>();
    owner->context = context; owner->binding = binding; owner->resource = resource;
    owner->receipt = context.dml_update_resource_receipt.lock();
    owner->descriptor_uuid = bundle.descriptor.descriptor_uuid;
    owner->generation = bundle.descriptor.descriptor_generation;
    std::unique_lock lock(owner->mutex);
    {
      std::lock_guard registry_lock(owners_mutex);
      if (!owners.emplace(Key{context.database_path, owner->descriptor_uuid, owner->generation}, owner).second) {
        result.diagnostic = Error("descriptor_owner_already_registered"); return result;
      }
      result.resource_ownership_taken = true;
    }
    const auto prepared = owner->receipt->PrepareDescriptorPublication(context, resource);
    if (!prepared.ok) { result.diagnostic = prepared.diagnostic; lock.unlock(); Retire(owner); return result; }
    owner->ticket = prepared.publication;
    auto store = MgaDmlDeleteDurableStoreV1::Open(context, owner->descriptor_uuid, owner->generation, &result.diagnostic);
    if (store && !store->chain().empty()) result.diagnostic = Error("bound_identity_already_exists");
    if (!store || !store->chain().empty() || !store->StoreAuthorityBundle(bundle, &result.diagnostic)) {
      store.reset(); lock.unlock(); Retire(owner); return result;
    }
    w::TypedDeleteJournalRecord bound;
    bound.descriptor = bundle.descriptor; bound.database_uuid = bundle.database_uuid;
    bound.journal_sequence = 1;
    bound.authenticated_statement_receipt_uuid = bundle.descriptor.authenticated_statement_receipt_uuid;
    bound.owning_transaction_uuid = bundle.descriptor.owning_transaction_uuid;
    bound.owning_local_transaction_id = bundle.descriptor.owning_local_transaction_id;
    bound.operation_uuid = bundle.descriptor.operation_uuid;
    bound.recovery_token_uuid = bundle.descriptor.recovery_token_uuid;
    bound.recovery_generation = bundle.descriptor.recovery_generation;
    if (!store->Append(bound, &result.diagnostic)) {
      store.reset(); lock.unlock(); Retire(owner); return result;
    }
    if (!FinishTicket(*owner, PublicationOutcome::published)) {
      result.diagnostic = std::move(failure); return result;
    }
    result.ok = true; result.diagnostic = std::move(success); return result;
  } catch (...) {
    result.diagnostic = std::move(failure);
    if (result.resource_ownership_taken) Retire(owner);
    return result;
  }
}

struct DmlDeleteExecutionLeaseV1::Impl {
  EngineRequestContext context;
  DmlDeleteDurableAuthorityBundleV1 bundle;
  std::shared_ptr<Owner> owner;
  std::unique_lock<std::mutex> lock;
  std::unique_ptr<MgaDmlDeleteDurableStoreV1> store;
};
DmlDeleteExecutionLeaseV1::DmlDeleteExecutionLeaseV1(std::unique_ptr<Impl> value) : impl_(std::move(value)) {}
DmlDeleteExecutionLeaseV1::~DmlDeleteExecutionLeaseV1() = default;
const DmlDeleteDurableAuthorityBundleV1& DmlDeleteExecutionLeaseV1::bundle() const { return impl_->bundle; }
MgaDmlDeleteDurableStoreV1& DmlDeleteExecutionLeaseV1::store() { return *impl_->store; }
bool DmlDeleteExecutionLeaseV1::has_live_binding() const noexcept {
  return impl_->owner && !impl_->owner->released && impl_->owner->binding.valid();
}
EngineApiDiagnostic DmlDeleteExecutionLeaseV1::RevalidateLive(const EngineRequestContext& c) const {
  if (!has_live_binding() || !MatchesDmlDeleteDurableAuthorityOwnerV1(c, impl_->bundle))
    return Error("live_original_binding_required", "MGA.TRANSACTION.STALE");
  return RevalidateDmlDeleteBindingAuthorityV1(c, impl_->owner->binding);
}
EngineApiDiagnostic DmlDeleteExecutionLeaseV1::RevalidateResource(const EngineRequestContext& c) const {
  if (!has_live_binding()) return Error("original_resource_owner_required", "MGA.TRANSACTION.STALE");
  return impl_->owner->receipt->Revalidate(c, impl_->owner->resource);
}
MgaVisibleHeapRelationStreamResult DmlDeleteExecutionLeaseV1::StreamCandidates(
    const EngineRequestContext& c, const MgaVisibleHeapRelationStreamRequest& request) const {
  const auto verified = RevalidateLive(c);
  if (verified.error) {
    MgaVisibleHeapRelationStreamResult failure; failure.diagnostic = verified; return failure;
  }
  return StreamMgaHeapForDmlDeleteV1(c, request, impl_->owner->binding);
}
EngineApiDiagnostic DmlDeleteExecutionLeaseV1::RevalidateRecovered(const EngineRequestContext& context) const {
  const auto receipt = context.dml_update_resource_receipt.lock();
  if (!receipt || !receipt->AuthenticatesContext(context) ||
      !MatchesDmlDeleteDurableAuthorityOwnerV1(context, impl_->bundle))
    return Error("authenticated_recovery_owner_required", "SECURITY.ACCESS_DENIED");
  const auto c = PhaseContext(context, "private_dml_delete_rows_recovery");
  const auto& b = impl_->bundle;
  auto diagnostic = RevalidateRecoveredDmlDeleteDatatypeAuthorityV1(c, b.descriptor, b.predicate, b.datatypes, b.operators);
  if (diagnostic.error) return diagnostic;
  diagnostic = RevalidateRecoveredDmlDeleteSecurityProjectionV1(c, b.security, b.matched_grant_uuids);
  if (diagnostic.error) return diagnostic;
  diagnostic = RevalidateRecoveredDmlDeleteEffectProjectionV1(c, b.effects);
  if (diagnostic.error) return diagnostic;
  SblrExecutorAvailabilitySnapshot current;
  return RevalidateSblrExecutorAvailability(c,
      {"dml.delete_rows", 784, "1.0", "dml_delete_rows_descriptor", "mutation_result", 1}, b.executor, &current);
}
EngineDmlDeleteRecoveryObservationV1 DmlDeleteExecutionLeaseV1::Reconcile() {
  // A failed rename/fence can leave the original storage object uncertain.
  // Reopen while retaining the same descriptor lock and resource owner.
  impl_->store.reset();
  EngineDmlDeleteRecoveryObservationV1 failure;
  impl_->store = MgaDmlDeleteDurableStoreV1::Open(impl_->context,
      impl_->bundle.descriptor.descriptor_uuid, impl_->bundle.descriptor.descriptor_generation, &failure.diagnostic);
  if (!impl_->store) return failure;
  return ReconcileStore(impl_->context, *impl_->store, impl_->owner.get());
}
MgaDmlDeletePublicationStatusV1 DmlDeleteExecutionLeaseV1::PublishAndRelease() noexcept {
  try {
    const auto status = impl_->store->ReleaseAndPublish();
    if (status == Publication::published && impl_->owner) (void)ReleaseOwner(*impl_->owner, Release::published);
    return status;
  } catch (...) { return Publication::publication_uncertain; }
}
std::unique_ptr<DmlDeleteExecutionLeaseV1> AcquireDmlDeleteExecutionLeaseV1(
    const EngineRequestContext& context, const w::TypedUpdateUuid& descriptor, std::uint64_t generation,
    std::uint64_t occurrence, EngineApiDiagnostic* diagnostic) {
  const auto fail = [&](const char* detail) -> std::unique_ptr<DmlDeleteExecutionLeaseV1> {
    if (diagnostic) *diagnostic = Error(detail, "MGA.TRANSACTION.STALE"); return {};
  };
  const auto receipt = context.dml_update_resource_receipt.lock();
  if (!Phase(context, "private_dml_delete_rows_consumer") || !receipt ||
      !receipt->AuthenticatesContext(context) || !generation || !occurrence)
    return fail("private_authenticated_consumer_required");
  PruneReleased();
  auto value = std::make_unique<DmlDeleteExecutionLeaseV1::Impl>(); value->context = context;
  {
    std::lock_guard lock(owners_mutex);
    const auto found = owners.find(Key{context.database_path, descriptor, generation});
    if (found != owners.end()) value->owner = found->second;
  }
  if (value->owner) {
    value->lock = std::unique_lock(value->owner->mutex, std::try_to_lock);
    if (!value->lock.owns_lock() || value->owner->receipt != receipt)
      return fail("descriptor_busy_or_foreign_runtime");
  }
  value->store = MgaDmlDeleteDurableStoreV1::Open(context, descriptor, generation, diagnostic);
  if (!value->store || value->store->chain().empty() ||
      !value->store->LoadAuthorityBundle(&value->bundle, diagnostic)) return {};
  if (value->bundle.descriptor.structural_occurrence_id != occurrence) return fail("occurrence_mismatch");
  if (value->owner && value->owner->binding.valid() &&
      value->owner->binding.bundle()->exact_bytes != value->bundle.exact_bytes)
    return fail("live_and_durable_binding_mismatch");
  if (diagnostic) *diagnostic = Ok();
  return std::unique_ptr<DmlDeleteExecutionLeaseV1>(new DmlDeleteExecutionLeaseV1(std::move(value)));
}
void RetireDmlDeleteResourceReceiptDescriptorsV1(const EngineRequestContext& context) noexcept {
  try {
    const auto receipt = context.dml_update_resource_receipt.lock();
    if (!receipt || !receipt->IsRevoked()) return;
    std::vector<std::shared_ptr<Owner>> pending;
    {
      std::lock_guard lock(owners_mutex);
      for (const auto& [key, owner] : owners)
        if (owner->receipt == receipt && owner->context.database_path == context.database_path) pending.push_back(owner);
    }
    for (const auto& owner : pending) Retire(owner);
    PruneReleased();
  } catch (...) {}
}
void RetryRetiredDmlDeleteResourceOwnersV1(const EngineRequestContext& context) noexcept {
  try {
    const auto scope = context.dml_update_resource_receipt.lock();
    if (!scope) return;
    std::vector<std::shared_ptr<Owner>> pending;
    {
      std::lock_guard lock(owners_mutex);
      for (const auto& [key, owner] : owners)
        if (owner->context.database_path == context.database_path && owner->receipt->SharesRuntimeWith(*scope) &&
            owner->receipt->IsRevoked()) pending.push_back(owner);
    }
    for (const auto& owner : pending) Retire(owner);
    PruneReleased();
  } catch (...) {}
}
bool DrainDmlDeleteResourceOwnersForRuntimeV1(const std::shared_ptr<EngineDmlUpdateResourceGovernorV1>& governor) noexcept {
  if (!governor) return true;
  governor->StopAdmission();
  try {
    std::vector<std::shared_ptr<Owner>> pending;
    {
      std::lock_guard lock(owners_mutex);
      for (const auto& [key, owner] : owners) {
        if (!owner->receipt->BelongsToRuntime(governor.get())) continue;
        owner->receipt->Revoke(); pending.push_back(owner);
      }
    }
    for (const auto& owner : pending) Retire(owner);
    PruneReleased();
    std::lock_guard lock(owners_mutex);
    return std::none_of(owners.begin(), owners.end(), [&](const auto& item) {
      return item.second->receipt->BelongsToRuntime(governor.get());
    });
  } catch (...) { return false; }
}
void ResetDmlDeleteBindingCacheForTestV1() {
  std::lock_guard lock(owners_mutex);
  for (const auto& [key, owner] : owners) {
    std::unique_lock owner_lock(owner->mutex, std::try_to_lock);
    if (owner_lock.owns_lock()) owner->binding = {};
  }
}
bool PrepareDmlDeleteTransactionFinalityV1(const EngineRequestContext& context) noexcept {
  try {
    std::vector<std::shared_ptr<Owner>> pending;
    {
      std::lock_guard lock(owners_mutex);
      for (const auto& [key, owner] : owners) {
        if (owner->context.database_path != context.database_path ||
            owner->context.local_transaction_id != context.local_transaction_id) continue;
        if (owner->context.transaction_uuid.canonical != context.transaction_uuid.canonical ||
            owner->context.session_uuid.canonical != context.session_uuid.canonical ||
            owner->context.principal_uuid.canonical != context.principal_uuid.canonical) return false;
        pending.push_back(owner);
      }
    }
    for (const auto& owner : pending) {
      Retire(owner);
      std::unique_lock lock(owner->mutex, std::try_to_lock);
      if (!lock.owns_lock() || !owner->released) return false;
    }
    PruneReleased();
    return true;
  } catch (...) { return false; }
}
}  // namespace scratchbird::engine::internal_api

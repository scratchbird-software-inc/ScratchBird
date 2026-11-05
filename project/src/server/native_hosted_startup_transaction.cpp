// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_hosted_database_runtime.hpp"
#include "engine/internal_api/transaction/native_local_transaction_candidate.hpp"
#include "engine/internal_api/transaction/native_snapshot_inventory.hpp"
#include "native_construction_audit.hpp"
#include "native_allocation_lease_reader.hpp"
#include "native_horizon_root.hpp"
#include "native_retention_root.hpp"
#include "engine/internal_api/verification/native_startup_catalog_allocation.hpp"
#include "engine/internal_api/verification/native_startup_root_allocation.hpp"
#include "engine/internal_api/verification/native_startup_initial_control_allocation.hpp"
#include <charconv>

namespace scratchbird::server {
namespace {
namespace api=engine::internal_api;namespace db=storage::database;
using E=api::NativeStartupTransactionError;using P=api::NativeStartupTransactionPhase;
using Uuid=core::platform::Uuid;using byte=core::platform::byte;using u64=core::platform::u64;
E LifetimeError(api::NativeStartupLifetimeError error) noexcept {
  using L=api::NativeStartupLifetimeError;
  switch(error){
    case L::none:return E::none;
    case L::transaction_expired:return E::transaction_expired;
    case L::snapshot_expired:return E::snapshot_expired;
    default:return E::clock_failure;
  }
}
bool SameLimits(const api::NativeStartupTransactionLimits& a,const api::NativeStartupTransactionLimits& b) noexcept {
  const auto& x=a.publication;const auto& y=b.publication;
  return a.maximum_publication_memory_bytes==b.maximum_publication_memory_bytes&&
      x.selected.maximum_verification_image_bytes==y.selected.maximum_verification_image_bytes&&
      x.selected.maximum_backing_bytes==y.selected.maximum_backing_bytes&&
      x.history.maximum_verification_image_bytes==y.history.maximum_verification_image_bytes&&x.history.maximum_backing_bytes==y.history.maximum_backing_bytes&&
      x.control.maximum_verification_image_bytes==y.control.maximum_verification_image_bytes&&x.control.maximum_backing_bytes==y.control.maximum_backing_bytes&&
      x.graph_backing_bytes==y.graph_backing_bytes&&x.workspace_backing_bytes==y.workspace_backing_bytes&&
      x.validation_backing_bytes==y.validation_backing_bytes&&x.maximum_image_bytes==y.maximum_image_bytes;
}
const char* DescriptorRefusal(const core::catalog::CatalogPolicyDescriptor& d,u64 transaction_number) noexcept {
  // Registry enforcement bit7 is transaction_start. A declaration for a
  // different boundary does not authorize this first startup transaction.
  if(!(d.enforcement_points&(u64{1}<<7)))return "transaction_start_not_configured";
  if(d.effective_transaction_number&&*d.effective_transaction_number>transaction_number)return "policy_not_effective";
  if(d.superseding_transaction_number&&*d.superseding_transaction_number<=transaction_number)return "policy_superseded";
  if(d.default_decision!=core::catalog::CatalogPolicyDefault::allow)return "policy_decision_refused";
  return nullptr;
}
const char* StartupPolicyRefusal(const api::NativeTransactionPolicyDeclarations& p,u64 transaction_number) noexcept {
  namespace c=core::catalog;
  if(const auto refusal=DescriptorRefusal(p.descriptor,transaction_number))return refusal;
  for(const auto& lock:p.locks)if(const auto refusal=DescriptorRefusal(lock.descriptor,transaction_number))return refusal;
  const auto& b=p.body;const auto& isolation=p.startup_isolation;
  if(!(b.read_mode_mask&2))return "startup_read_write_required";
  if(b.cluster_eligibility!=c::TransactionClusterEligibility::local_only)return "startup_cluster_authority_required";
  // Statement autocommit defaults apply outside an explicit transaction. The
  // coordinator owns this one explicit TX2 across verification and activation.
  if(isolation.profile_kind!=c::IsolationProfileKind::maintenance_operation_isolation||
      isolation.snapshot_construction_point!=c::IsolationSnapshotPoint::operation_start||isolation.snapshot_refresh_mask!=1||
      isolation.own_write_visibility!=c::IsolationOwnWrites::visible_until_savepoint_rollback||
      isolation.active_other_visibility!=c::IsolationActiveOthers::never_visible||
      isolation.limbo_visibility!=c::IsolationLimbo::ordinary_hidden||!(isolation.write_conflict_detection_mask&2)||
      isolation.metadata_visibility!=c::IsolationMetadataVisibility::transaction_epoch||
      u64(isolation.cluster_participant_mode)>2)return "startup_isolation_unsupported";
  return nullptr;
}
}

api::NativeStartupTransactionResult NativeHostedDatabaseRuntime::PrepareStartupTransaction(
    CredentialRead& read,api::NativeStartupTransactionLimits limits) noexcept {
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return {E::busy};
  if(!read.ok()||!read.owner_||!read.session_||!read.authentication_)return {E::not_ready};
  const auto cancelled=[&]{return read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested();};
  if(cancelled())return {E::cancelled};
  if(read.startup_transaction_.state_)return SameLimits(read.startup_transaction_.state_->limits_,limits)?
      read.startup_transaction_.state_->result_:api::NativeStartupTransactionResult{E::invalid_limits};
  if(!read.transaction_policies_.ok()||!read.management_definition_.ok()||!read.verification_policies_.ok()||!read.catalog_metadata_.ok()||
      !read.agent_policies_.ok())return {E::policy_required};
  if(!read.startup_transactions_.ok()||!read.history_.ok()||!read.control_.ok()||!read.publication_.ok())return {E::not_ready};
  const auto& credential=*read.credentials_->comparison->lease;
  if(credential.has_selected_source()||!credential.has_retained_operation()||
      read.session_->observation_.engine_instance_uuid!=read.owner_->engine_instance_||
      !core::uuid::IsEngineIdentityUuid(read.session_->observation_.attachment_uuid)||
      credential.writer_operation_uuid()!=read.session_->observation_.original_operation_uuid)return {E::source_mismatch};
  const auto& source=*read.catalog_capture_.selection();
  using api::native_startup_transaction_history_detail::SameSelection;
  if(!SameSelection(source,read.session_->observation_.source)||!SameSelection(source,*read.management_definition_.selection)||
      !SameSelection(source,*read.verification_policies_.selection)||!SameSelection(source,*read.transaction_policies_.selection)||
      !SameSelection(source,*read.startup_transactions_.selection)||
      !SameSelection(source,read.publication_.snapshot->selection))return {E::source_mismatch};
  // This is the minimal private first-open bootstrap, never maintenance rights
  // for an ordinary session or arbitrary management operation. Active admission
  // and all verification/profile authorization still precede executable work.
  const auto& access=read.verification_policies_.access_body;
  const bool permit_bootstrap=(access.flags&64)!=0;
  if(!read.session_->authorization_.Allows("CONNECT",read.owner_->database_,permit_bootstrap)||
      !read.session_->authorization_.Allows("CATALOG_MUTATE",read.owner_->database_,permit_bootstrap)||
      !read.session_->authorization_.Allows("OBS_AGENT_CONTROL",read.owner_->database_,permit_bootstrap))return {E::authorization_refused};
  if(!read.session_->authorization_.WithinLimits({access.maximum_records,access.maximum_membership_depth,access.maximum_work}))
    return {E::authorization_refused};
  const auto captured=read.catalog_capture_.inventory();
  if(!captured)return {E::not_ready};
  const auto& inventory=captured->inventory;
  if(source.selection_generation!=1||source.previous_checkpoint||!read.history_.history.entries.empty()||
      !read.startup_transactions_.transactions.empty()||read.startup_transactions_.pending_binding||
      read.publication_.snapshot->watermark.intent||read.publication_.snapshot->watermark.watermark!=1||
      inventory.next_local_transaction_id!=2||inventory.next_commit_sequence!=2||inventory.entries.size()!=1)
    return {E::recovery_required};
  const auto& creation=inventory.entries[0];
  if(creation.identity.local_id.value!=1||creation.identity.scope!=transaction::mga::TransactionScope::local_node||
      creation.state!=transaction::mga::TransactionState::committed||creation.commit_sequence!=1||
      !creation.evidence_record_written||creation.rollback_only)return {E::recovery_required};
  // Check the actual frozen policy before allocating an identity or writing a
  // starting slot. This does not acquire snapshots, locks or active admission.
  if(const auto reason=StartupPolicyRefusal(read.transaction_policies_,inventory.next_local_transaction_id)) {
    api::NativeStartupTransactionResult refused;refused.error=E::policy_refused;refused.detail=reason;return refused;
  }
  const auto& l=limits.publication;
  if(!limits.maximum_publication_memory_bytes||!l.selected.maximum_verification_image_bytes||!l.selected.maximum_backing_bytes||
      !l.history.maximum_verification_image_bytes||!l.history.maximum_backing_bytes||!l.control.maximum_verification_image_bytes||!l.control.maximum_backing_bytes||
      !l.graph_backing_bytes||!l.workspace_backing_bytes||!l.validation_backing_bytes||!l.maximum_image_bytes)
    return {E::invalid_limits};
  const auto files=read.catalog_capture_.filespaces();
  // Reserve the concurrent high-water mark, not the caller's ceiling. Control
  // inspection retires before history/graph construction; installed control
  // verification runs after the selected-source read has retired.
  const auto watermark_bytes=db::NativeOwnedPublicationWatermarkWorkspaceBytes(source.header.page_size_profile_uuid,files.size());
  const auto sum=[](std::initializer_list<std::size_t> values)->std::optional<std::size_t>{
    std::size_t total=0;for(const auto value:values){if(value>SIZE_MAX-total)return {};total+=value;}return total;
  };
  const auto inspect_peak=sum({watermark_bytes,l.selected.maximum_backing_bytes,l.control.maximum_backing_bytes});
  const auto build_peak=sum({watermark_bytes,l.selected.maximum_backing_bytes,l.history.maximum_backing_bytes,
      l.workspace_backing_bytes,l.graph_backing_bytes,l.validation_backing_bytes});
  const auto publish_peak=sum({watermark_bytes,l.workspace_backing_bytes,l.graph_backing_bytes,l.control.maximum_backing_bytes});
  if(!watermark_bytes||!inspect_peak||!build_peak||!publish_peak)return {E::invalid_limits};
  const auto publication_bytes=std::max({*inspect_peak,*build_peak,*publish_peak});
  if(publication_bytes>limits.maximum_publication_memory_bytes)return {E::invalid_limits};
  constexpr auto padding=8*alignof(std::max_align_t);
  constexpr auto state_prefix=256+api::kNativeStartupTransactionDetailMaxBytes+api::kNativeAdmissionEvidenceMaximumBytes+512;
  constexpr auto fixed=sizeof(api::NativeStartupTransactionOwner)+320+2*state_prefix+padding;
  if(files.empty()||files.size()>(SIZE_MAX-fixed)/(4*sizeof(Uuid)))return {E::invalid_limits};
  const auto bytes=fixed+4*sizeof(Uuid)*files.size();
  // SB_OPEN_VERIFICATION_READ_ONLY (owner direction 2026-11-05): nothing is
  // written before a clean read-only verification of this same selected file
  // (VerifyStartupDatabaseFile). Checked after every earlier refusal, so each
  // keeps its own error, and before the first allocation or effect.
  if(!read.open_verification_||!SameSelection(read.open_verification_->identity.selection,source)){
    api::NativeStartupTransactionResult refused;refused.error=E::verification_failure;
    refused.detail="open_verification_required";return refused;
  }
  auto& self=*read.owner_;const auto& config=*read.configuration_.configuration;
  if(!self.process_->HasNativeDatabaseAllocationBehavior(self.database_,config.memory,config.memory_version_uuid))return {E::policy_required};
  read.mutating_=true;self.entering_=true;
  struct Leave{bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}}leave{read.mutating_,self.entering_};
  const auto memory_failure=[](const char* detail,core::platform::Status status={}){
    api::NativeStartupTransactionResult result;result.error=E::resource_exhausted;result.detail=detail;
    result.allocation_status=status;return result;
  };
  try {
    auto allocated=self.Acquire(bytes,NativeBootstrapMemoryKind::storage);
    if(!allocated.ok())return memory_failure("state_reservation",allocated.admission.status);
    auto memory=db::AdoptNativeStorageMemory(StorageBinding(*allocated.admission.resource),allocated.admission.resource);
    if(!memory.ok())return memory_failure("state_adoption");
    auto arena=memory.memory.CreateArena(memory.memory.binding(),bytes,alignof(std::max_align_t));
    if(!arena.ok())return memory_failure("state_arena",arena.backing.status);
    api::NativeStartupTransactionHandle owned;owned.backing_=std::move(arena.arena);
    const auto state_memory=owned.backing_.Allocate(sizeof(api::NativeStartupTransactionOwner),alignof(api::NativeStartupTransactionOwner));
    const auto targets_memory=owned.backing_.Allocate(files.size()*sizeof(Uuid),alignof(Uuid));
    const auto request_memory=owned.backing_.Allocate(320+files.size()*16,alignof(byte));
    const auto state_bytes=owned.backing_.Allocate(state_prefix+files.size()*16,alignof(byte));
    const auto boundary_bytes=owned.backing_.Allocate(state_prefix+files.size()*16,alignof(byte));
    if(!state_memory.ok()||!targets_memory.ok()||!request_memory.ok()||!state_bytes.ok()||!boundary_bytes.ok())return memory_failure("state_layout",
        !state_memory.ok()?state_memory.status:!targets_memory.ok()?targets_memory.status:!request_memory.ok()?request_memory.status:
        !state_bytes.ok()?state_bytes.status:boundary_bytes.status);
    // The credential lease has retired its selected source, and watermark
    // inspection has released its reader too. Their immutable observations
    // live in separate admitted backing under the original writer custody.
    // Release idle bootstrap credit before admitting successor reads; keeping
    // both full reader reservations would exhaust the database page budget.
    // Refusal preserves the original custody and permits a later retry.
    if(self.reading_.Snapshot().allocated_bytes)return {E::source_mismatch};
    self.reading_={};
    auto publication_grant=self.Acquire(publication_bytes,NativeBootstrapMemoryKind::storage);
    if(!publication_grant.ok())return memory_failure("publication_reservation",publication_grant.admission.status);
    auto publication_memory=db::AdoptNativeStorageMemory(StorageBinding(*publication_grant.admission.resource),publication_grant.admission.resource);
    if(!publication_memory.ok())return memory_failure("publication_adoption");
    E error=E::identity_failure;
    const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
      const auto snapshot=core::uuid::IssueRuntimeIdentityV7();if(!snapshot)return;
      // The snapshot has a real, private, governed lifetime retaining the exact
      // source and policy set. It is distinct from a persisted policy version.
      owned.state_=new(state_memory.pointer)api::NativeStartupTransactionOwner(*snapshot,read.session_->clock_policy_,limits);
      auto& owner=*owned.state_;owner.source_=source;owner.policies_=read.verification_policies_;owner.transaction_policies_=read.transaction_policies_;
      owner.management_=read.management_definition_;owner.frozen_rows_=read.catalog_capture_.rows();
      owner.state_storage_={std::span{static_cast<byte*>(state_bytes.pointer),state_bytes.bytes},
          std::span{static_cast<byte*>(boundary_bytes.pointer),boundary_bytes.bytes}};
      owner.publication_memory_=std::move(publication_memory.memory);owner.memory_binding_=owner.publication_memory_.binding();
      const auto operation=owner.issuer_.Issue(core::platform::UuidKind::object);
      const auto transaction=owner.issuer_.Issue(core::platform::UuidKind::transaction);
      const auto event=owner.issuer_.Issue(core::platform::UuidKind::object);
      if(!operation.ok()||!transaction.ok()||!event.ok())return;
      const auto elapsed=core::time::ReadNativeMonotonicTime();
      if(!elapsed.ok()){error=E::clock_failure;return;}
      owner.started_native_nanoseconds_=owner.last_native_nanoseconds_=elapsed.nanoseconds;
      const auto clock=transaction.observation->wall_clock;
      if(clock.unix_seconds<=0||u64(clock.unix_seconds)>UINT64_MAX/1000000||clock.nanoseconds>=1000000000)return;
      const auto micros=u64(clock.unix_seconds)*1000000+clock.nanoseconds/1000;
      auto targets=std::span{static_cast<Uuid*>(targets_memory.pointer),files.size()};
      for(std::size_t n=0;n<files.size();++n)targets[n]=files[n].bootstrap.filespace_uuid;
      std::sort(targets.begin(),targets.end());
      owner.request_.binding={operation.value->value,read.session_->observation_.session_uuid,transaction.value->value,2,1};
      auto& initial=owner.request_.initial;initial.operation_uuid=operation.value->value;initial.database_uuid=self.database_;
      // Native genesis has the original database identity as its creation
      // lineage. No later/foreign inventory is admitted by this first-open path.
      initial.database_lineage_uuid=self.database_;initial.acting_subject_uuid=read.session_->observation_.principal_uuid;
      const auto& p=owner.policies_.request;
      initial.repair_policy_uuid=p.repair_policy_uuid;initial.quarantine_policy_uuid=p.quarantine_policy_uuid;
      initial.salvage_policy_uuid=p.salvage_policy_uuid;initial.evidence_policy_uuid=p.evidence_policy_uuid;
      initial.created_at_micros=initial.updated_at_micros=micros;initial.verification_class=p.verification_class;
      initial.authority_class=p.authority_class;initial.verification_levels=p.verification_levels;initial.filespaces=targets;
      const auto encoded=api::EncodeNativeStartupVerificationRequestInto(owner.request_,request_memory.bytes,
          {static_cast<byte*>(request_memory.pointer),request_memory.bytes});
      const auto state=api::EncodeNativeVerificationOperationStateInto(initial,state_bytes.bytes,
          {static_cast<byte*>(state_bytes.pointer),state_bytes.bytes});
      if(!encoded.ok()||!state.ok()){error=E::request_failure;return;}
      owner.verification_state_=initial;
      auto& record=owner.record_;record.database_uuid=self.database_;record.bootstrap_uuid=source.bootstrap_uuid;
      record.uuid=operation.value->value;record.descriptor_uuid=owner.management_.descriptor.descriptor_uuid;
      record.family_uuid=owner.management_.profiles[0].profile_uuid;record.target_type_uuid=owner.management_.profiles[2].profile_uuid;
      record.target_uuid=self.database_;record.initiator_uuid=initial.acting_subject_uuid;
      record.request_context_uuid=owner.request_.binding.session_uuid;record.policy_snapshot_uuid=*snapshot;
      record.security_snapshot_uuid=read.authentication_->observation_.decision_uuid;
      record.created_at=record.updated_at=event.value->value;record.initiator_kind=4;
      record.evidence_required=record.metrics_required=true;record.normalized_request_bytes=encoded.bytes;
      record.operation_state_bytes=state.bytes;
      const auto hash=core::hash::ComputeSha256DigestNative(encoded.bytes.data(),encoded.bytes.size());
      if(!hash.ok()){error=E::crypto_failure;return;}record.normalized_request_sha256=hash.digest;
      constexpr std::string_view prefix="startup.verify:";constexpr char hex[]="0123456789abcdef";
      std::copy(prefix.begin(),prefix.end(),owner.idempotency_.begin());
      for(std::size_t n=0;n<hash.digest.size();++n){owner.idempotency_[prefix.size()+2*n]=hex[hash.digest[n]>>4];
        owner.idempotency_[prefix.size()+2*n+1]=hex[hash.digest[n]&15];}
      record.idempotency_key={owner.idempotency_.data(),prefix.size()+64};
      std::array<Uuid,7> identities;
      if(db::ValidateNativeManagementOperationView(record,identities)!=db::NativeManagementOperationError::none){error=E::request_failure;return;}
      std::array<std::size_t,1> indices;std::array<byte,1> markers;
      const auto candidate=api::BuildNativeLocalTransactionCandidate(inventory,
          {api::NativeLocalTransactionChange::allocate_starting,*transaction.value,2,micros/1000,false},
          {owner.entries_,indices,markers});
      if(!candidate.ok()){error=E::request_failure;return;}owner.candidate_=candidate.inventory;
      owner.result_.binding=owner.request_.binding;owner.result_.original_publication_uuid=*credential.writer_operation_uuid();
      owner.result_.current_publication_uuid=owner.result_.original_publication_uuid;
      owner.result_.publication_reserved_bytes=publication_bytes;
      owner.result_.phase=P::prepared;owner.result_.error=E::none;error=E::none;
    });
    if(!crypto.ok())return {E::crypto_failure};
    if(error!=E::none)return {error};
    if(cancelled())return {E::cancelled};
    read.startup_transaction_=std::move(owned);
    // The immutable observation stays available as provenance, but its reader
    // must not retain a second physical-publication pin. The credential owner
    // now fences the source and replacement reads are blocked by startup state.
    read.publication_preparation_={};read.publication_memory_={};
    return read.startup_transaction_.state_->result_;
  }catch(const std::bad_alloc&){return {E::resource_exhausted};}
   catch(...){return {E::request_failure};}
}

api::NativeStartupTransactionResult NativeHostedDatabaseRuntime::PublishStartupTransaction(CredentialRead& read) noexcept {
  return PublishStartupOwned(read,P::starting_selected);
}

api::NativeStartupTransactionResult NativeHostedDatabaseRuntime::PublishStartupOwned(CredentialRead& read,P selected_phase) noexcept {
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return {E::busy};
  if(!read.ok()||!read.owner_||!read.startup_transaction_.state_)return {E::not_ready};
  auto& self=*read.owner_;auto& owner=*read.startup_transaction_.state_;
  auto result=[&](E error){auto r=owner.result_;r.error=error;return r;};
  if(owner.expected_selected_phase_!=selected_phase)return result(E::busy);
  if(read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested())return result(E::cancelled);
  if(owner.result_.phase==selected_phase)return result(E::none);
  auto& credential=*read.credentials_->comparison->lease;
  if(!read.session_||!read.authentication_||credential.has_selected_source()||!credential.has_retained_operation()||
      credential.writer_operation_uuid()!=owner.result_.current_publication_uuid)return result(E::source_mismatch);
  // A new physical attempt needs a live lifetime. An already uncertain write
  // must retain and reconcile its original bytes; expiry is not rollback or a
  // reason to abandon custody. Selected starting state still grants no work.
  if(owner.result_.phase==P::prepared)
    if(const auto e=LifetimeError(owner.CheckLifetime());e!=E::none)return result(e);
  read.mutating_=true;self.entering_=true;
  struct Leave{bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}}leave{read.mutating_,self.entering_};
  E error=E::publication_failure;
  const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
    auto& operation=credential.retained_operation_;auto& memory=owner.publication_memory_;
    const auto& limits=owner.limits_.publication;
    if(owner.result_.phase==P::prepared){
      if(!owner.watermark_.ok())owner.watermark_=db::PrepareNativeOwnedPublicationWatermark(
          owner.source_.header.page_size_profile_uuid,read.catalog_capture_.filespaces().size(),memory,owner.memory_binding_);
      if(!owner.watermark_.ok()){error=E::resource_exhausted;return;}
      auto observed=owner.watermark_.workspace.Inspect(operation,limits.selected,memory,owner.memory_binding_,owner.request_.binding);
      owner.result_.read_writer_error=observed.writer_error;owner.result_.read_memory_error=observed.source.memory_source_error;
      owner.result_.read_selection_error=observed.source.selection_error;owner.result_.read_checkpoint_error=observed.source.checkpoint_error;
      owner.result_.read_inventory_error=observed.source.inventory_error;owner.result_.read_allocation_error=observed.source.allocation_error;
      owner.result_.allocation_status=observed.source.allocation_status;
      if(!observed.ok()){
        owner.result_.reservation_error=observed.error;owner.result_.reservation_phase=observed.phase;
        owner.result_.effects=operation.effects();owner.result_.io=observed.io;
        owner.watermark_result_=std::move(observed);return;
      }
      if(!api::native_startup_transaction_history_detail::SameSelection(owner.source_,observed.snapshot->selection)){
        error=E::source_mismatch;return;}
      // Source verification may consume time. Recheck before the first write.
      if(const auto e=LifetimeError(owner.CheckLifetime());e!=E::none){error=e;return;}
      db::NativePublicationIntent intent{owner.record_.initiator_uuid,owner.record_.request_context_uuid,
        owner.record_.policy_snapshot_uuid,owner.record_.normalized_request_sha256,owner.record_.initiator_kind};
      intent.recovery_profile=2;intent.startup_binding=owner.request_.binding;
      owner.watermark_result_=owner.watermark_.workspace.Reserve(operation,*observed.snapshot,intent);
      if(owner.watermark_result_.phase!=db::NativeOwnedWatermarkPhase::inspected)owner.result_.phase=P::reserving;
    }else if(owner.result_.phase==P::reserving)owner.watermark_result_=owner.watermark_.workspace.Resume(operation);
    const auto& reserved=owner.watermark_result_;
    owner.result_.reservation_error=reserved.error;owner.result_.reservation_phase=reserved.phase;
    owner.result_.effects=operation.effects();owner.result_.io=reserved.io;
    if(!reserved.ok())return;
    if(owner.result_.phase==P::reserving)owner.result_.phase=P::building;
    db::NativeOwnedInventoryPublicationResult published;
    if(owner.result_.phase==P::building){
      if(!owner.publisher_){owner.publisher_.emplace();
        published=owner.publisher_->Prepare(operation,std::move(owner.watermark_.workspace),owner.record_,owner.candidate_,
            owner.issuer_,limits,memory,owner.memory_binding_);
      }else published=owner.publisher_->RetryPreparation(owner.record_,owner.candidate_,owner.issuer_);
      owner.result_.publication_error=published.error;owner.result_.publication_phase=published.phase;
      owner.result_.effects=operation.effects();owner.result_.io=published.io;
      if(!published.ok())return;
      owner.result_.phase=P::publishing;
    }
    published=owner.publisher_->Publish();
    owner.result_.publication_error=published.error;owner.result_.publication_phase=published.phase;
    owner.result_.effects=operation.effects();owner.result_.io=published.io;
    if(!published.ok()||!published.selected_graph_verified||!published.snapshot)return;
    owner.result_.selected=published.snapshot;owner.result_.phase=selected_phase;
    owner.result_.verification_phase=owner.verification_state_.phase;error=E::none;
  });
  if(!crypto.ok())error=E::crypto_failure;
  owner.result_.error=error;return owner.result_;
}

api::NativeStartupTransactionResult NativeHostedDatabaseRuntime::EstablishStartupVerificationBoundary(CredentialRead& read) noexcept {
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return {E::busy};
  if(!read.ok()||!read.owner_||!read.startup_transaction_.state_||!read.session_||!read.authentication_)return {E::not_ready};
  auto& self=*read.owner_;auto& owner=*read.startup_transaction_.state_;
  const auto result=[&](E error){auto r=owner.result_;r.error=error;return r;};
  if(read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested())return result(E::cancelled);
  if(owner.expected_selected_phase_==P::boundary_selected)return PublishStartupOwned(read,P::boundary_selected);
  if(owner.result_.phase!=P::starting_selected||!owner.publisher_||!owner.result_.selected)return result(E::not_ready);
  if(const auto e=LifetimeError(owner.CheckLifetime());e!=E::none)return result(e);
  auto& credential=*read.credentials_->comparison->lease;
  if(credential.has_selected_source()||!credential.has_retained_operation()||
      credential.writer_operation_uuid()!=owner.result_.current_publication_uuid||
      read.session_->observation_.session_uuid!=owner.request_.binding.session_uuid||
      read.authentication_->observation_.decision_uuid!=owner.record_.security_snapshot_uuid)return result(E::source_mismatch);
  // This private startup profile uses the real local-service grants, frozen
  // catalog policies and installed resource configuration. It cannot accept an
  // ordinary session or grant access to data, repair, quarantine or salvage.
  const auto& authorization=read.session_->authorization_;
  if(!owner.policies_.ok()||!owner.management_.ok()||
      owner.management_.profiles[5].code!=core::catalog::CatalogManagementProfileCode::maintenance_authorization||
      owner.policies_.request.authority_class!=api::NativeVerificationAuthority::startup_recovery)return result(E::policy_required);
  const auto& access=owner.policies_.access_body;
  const bool permit_bootstrap=(access.flags&64)!=0;
  if(!authorization.Allows("CONNECT",self.database_,permit_bootstrap)||
      !authorization.Allows("CATALOG_MUTATE",self.database_,permit_bootstrap)||
      !authorization.Allows("OBS_AGENT_CONTROL",self.database_,permit_bootstrap))return result(E::authorization_refused);
  if(!authorization.WithinLimits({access.maximum_records,access.maximum_membership_depth,access.maximum_work}))
    return result(E::authorization_refused);
  const auto& config=*read.configuration_.configuration;
  if(!self.process_->HasNativeDatabaseAllocationBehavior(self.database_,config.memory,config.memory_version_uuid))
    return result(E::policy_required);
  read.mutating_=true;self.entering_=true;
  E error=E::identity_failure;
  {
    struct Leave{bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}}leave{read.mutating_,self.entering_};
    const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
      if(!owner.successor_preparation_){
        const auto physical=owner.issuer_.Issue(core::platform::UuidKind::object);
        const auto boundary=owner.issuer_.Issue(core::platform::UuidKind::object);
        const auto event=owner.issuer_.Issue(core::platform::UuidKind::object);
        if(!physical.ok()||!boundary.ok()||!event.ok())return;
        const auto clock=event.observation->wall_clock;
        if(clock.unix_seconds<=0||u64(clock.unix_seconds)>UINT64_MAX/1000000||clock.nanoseconds>=1000000000)return;
        api::NativeStartupTransactionOwner::BoundaryPreparation next;
        next.publication_uuid=physical.value->value;next.state=owner.verification_state_;
        next.state.maintenance_boundary_uuid=boundary.value->value;
        next.state.phase=api::NativeVerificationPhase::boundary_established;
        next.state.updated_at_micros=u64(clock.unix_seconds)*1000000+clock.nanoseconds/1000;
        if(api::ValidateNativeVerificationOperationEvolution(owner.verification_state_,next.state)!=api::NativeVerificationStateError::none){
          error=E::request_failure;return;
        }
        const auto state=api::EncodeNativeVerificationOperationStateInto(next.state,owner.state_storage_[1].size(),owner.state_storage_[1]);
        if(!state.ok()){error=E::request_failure;return;}
        next.record=owner.record_;++next.record.revision;next.record.updated_at=event.value->value;
        next.record.state=db::NativeManagementState::authorized;next.record.boundary_uuid=boundary.value->value;
        next.record.operation_state_bytes=state.bytes;
        // The configuration guard names the actual native memory configuration
        // used by this bootstrap operation. Other policy rows remain bound to
        // the frozen policy snapshot, not an invented global runtime epoch.
        next.record.generation_guards={credential.selected_catalog().catalog_generation,
            config.memory.generation,credential.binding().security_epoch,std::nullopt};
        std::array<Uuid,7> identities;
        if(db::ValidateNativeManagementOperationEvolutionView(owner.record_,next.record,identities)!=db::NativeManagementOperationError::none){
          error=E::request_failure;return;
        }
        owner.successor_preparation_=next;
      }
      const auto& next=*owner.successor_preparation_;
      const auto advanced=owner.publisher_->Advance(credential.retained_operation_,next.publication_uuid);
      owner.result_.handoff_error=advanced;
      if(advanced!=db::NativeWriterError::none){error=advanced==db::NativeWriterError::resource_exhausted?E::resource_exhausted:E::publication_failure;return;}
      // The completed publisher transfers the actual registration atomically.
      // Its observed selected source, never the original genesis cache, is the
      // only base for the next physical attempt.
      owner.source_=owner.result_.selected->selection;owner.publisher_.reset();owner.watermark_={};owner.watermark_result_={};
      owner.record_=next.record;owner.verification_state_=next.state;
      owner.result_.current_publication_uuid=next.publication_uuid;
      owner.result_.maintenance_boundary_uuid=next.state.maintenance_boundary_uuid;
      owner.result_.phase=P::prepared;owner.result_.selected.reset();owner.expected_selected_phase_=P::boundary_selected;
      owner.successor_preparation_.reset();error=E::none;
    });
    if(!crypto.ok())error=E::crypto_failure;
  }
  if(error!=E::none)return result(error);
  return PublishStartupOwned(read,P::boundary_selected);
}
api::NativeStartupSnapshotResult NativeHostedDatabaseRuntime::EstablishStartupDataSnapshot(CredentialRead& read) noexcept {
  using SE=api::NativeStartupSnapshotError;
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return {SE::busy};
  if(!read.ok()||!read.owner_||!read.startup_transaction_.state_||!read.session_||!read.authentication_)return {SE::not_ready};
  if(read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested())return {SE::cancelled};
  auto& self=*read.owner_;auto& owner=*read.startup_transaction_.state_;
  if((owner.result_.phase!=P::boundary_selected&&owner.result_.phase!=P::detail_selected&&owner.result_.phase!=P::admission_evidence_selected&&
      owner.result_.phase!=P::active_selected)||
      !owner.publisher_||!owner.result_.selected)return {SE::not_ready};
  const auto& credential=*read.credentials_->comparison->lease;
  const auto& session=read.session_->observation_;
  if(credential.has_selected_source()||!credential.has_retained_operation()||
      credential.writer_operation_uuid()!=owner.result_.current_publication_uuid||
      session.session_uuid!=owner.request_.binding.session_uuid||session.engine_instance_uuid!=self.engine_instance_||
      session.authentication_decision_uuid!=owner.record_.security_snapshot_uuid||
      read.authentication_->observation_.decision_uuid!=owner.record_.security_snapshot_uuid||
      !owner.result_.selected->watermark.intent||
      owner.result_.selected->watermark.intent->startup_binding!=owner.request_.binding||
      owner.frozen_rows_.data()!=read.catalog_capture_.rows().data()||
      owner.frozen_rows_.size()!=read.catalog_capture_.rows().size())return {SE::source_mismatch};
  const auto& config=*read.configuration_.configuration;
  if(!self.process_->HasNativeDatabaseAllocationBehavior(self.database_,config.memory,config.memory_version_uuid)||
      !owner.transaction_policies_.ok())return {SE::policy_required};
  const auto lifetime=LifetimeError(owner.CheckLifetime());
  if(lifetime!=E::none)return {lifetime==E::transaction_expired?SE::transaction_expired:
      lifetime==E::snapshot_expired?SE::snapshot_expired:SE::clock_failure};
  if(owner.snapshot_)return {SE::none,owner.snapshot_};
  // The completed owned publisher independently validated and read back this
  // exact candidate at the selected boundary. Its continuing exclusive owner
  // prevents any writer or cleanup from changing/recycling these generations.
  // The frozen catalog is the original admitted catalog, not the newer
  // inventory checkpoint or an invented common policy/security generation.
  if(owner.candidate_.entries.size()!=2||owner.candidate_.next_local_transaction_id!=3||
      owner.candidate_.entries[1].state!=transaction::mga::TransactionState::created)return {SE::inventory_failure};
  std::array<u64,2> active{},in_doubt{};std::array<std::size_t,2> indices{};std::array<byte,2> markers{};
  const core::platform::TypedUuid transaction{core::platform::UuidKind::transaction,owner.request_.binding.transaction_uuid};
  const auto projected=api::ProjectNativeSnapshotInventory(owner.candidate_,transaction,owner.request_.binding.local_transaction_id,
      {active,in_doubt,indices,markers});
  if(!projected.ok())return {SE::inventory_failure};
  read.mutating_=true;self.entering_=true;
  struct Leave{bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}}leave{read.mutating_,self.entering_};
  SE error=SE::identity_failure;
  const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
    const auto snapshot=owner.issuer_.Issue(core::platform::UuidKind::object);
    const auto set=owner.issuer_.Issue(core::platform::UuidKind::object);
    const auto epoch=owner.issuer_.Issue(core::platform::UuidKind::object);
    if(!snapshot.ok()||!set.ok()||!epoch.ok())return;
    const auto elapsed=core::time::ReadNativeMonotonicTime();
    if(!elapsed.ok()||elapsed.nanoseconds<owner.last_native_nanoseconds_){error=SE::clock_failure;return;}
    const auto age=api::CheckNativeStartupLifetime(owner.started_native_nanoseconds_,owner.last_native_nanoseconds_,
        std::nullopt,elapsed.nanoseconds,owner.transaction_policies_.body.maximum_transaction_microseconds,
        owner.transaction_policies_.body.maximum_snapshot_microseconds);
    if(age!=api::NativeStartupLifetimeError::none){error=age==api::NativeStartupLifetimeError::transaction_expired?
        SE::transaction_expired:SE::clock_failure;return;}
    const auto clock=snapshot.observation->wall_clock;
    if(clock.unix_seconds<=0||u64(clock.unix_seconds)>UINT64_MAX/1000000||clock.nanoseconds>=1000000000)return;
    api::NativeStartupSnapshotObservation next;
    next.snapshot_uuid=snapshot.value->value;next.active_set_uuid=set.value->value;next.catalog_visibility_epoch_uuid=epoch.value->value;
    next.database_uuid=self.database_;next.engine_instance_uuid=self.engine_instance_;
    next.attachment_uuid=session.attachment_uuid;next.session_uuid=session.session_uuid;
    next.owning_transaction_uuid=transaction.value;next.operation_uuid=owner.request_.binding.operation_uuid;
    next.policy_snapshot_uuid=owner.result_.policy_snapshot_uuid;next.security_snapshot_uuid=owner.record_.security_snapshot_uuid;
    const auto& isolation=owner.transaction_policies_.startup_isolation;
    next.isolation_profile_uuid=isolation.profile_uuid;next.isolation_generation=isolation.generation;
    next.isolation_version_uuid=owner.transaction_policies_.startup_isolation_version_uuid;next.limbo_policy=isolation.limbo_visibility;
    next.owning_transaction_number=next.created_at_transaction_number=projected.owning_transaction_number;
    next.created_at_micros=u64(clock.unix_seconds)*1000000+clock.nanoseconds/1000;
    next.maximum_snapshot_microseconds=owner.transaction_policies_.body.maximum_snapshot_microseconds;
    next.next_transaction_number=projected.next_transaction_number;
    next.visible_committed_high_watermark=projected.visible_committed_high_watermark;
    next.visible_commit_sequence=projected.visible_commit_sequence;
    next.oldest_interesting=projected.horizons.oldest_interesting_transaction.value;
    next.oldest_active=projected.horizons.oldest_active_transaction.value;
    next.oldest_snapshot=projected.horizons.oldest_snapshot_transaction.value;next.retention_horizon=projected.retention_horizon;
    next.active_count=projected.active.size();next.in_doubt_count=projected.in_doubt.size();
    std::copy(projected.active.begin(),projected.active.end(),next.active.begin());
    std::copy(projected.in_doubt.begin(),projected.in_doubt.end(),next.in_doubt.begin());
    next.inventory_source=owner.result_.selected->selection;next.catalog_source=credential.selected_catalog();
    // A fresh catalog epoch names the actual retained root/version lifetime;
    // neither a root UUID nor a checkpoint counter is reused as that identity.
    owner.snapshot_=next;owner.snapshot_native_nanoseconds_=elapsed.nanoseconds;
    owner.last_native_nanoseconds_=elapsed.nanoseconds;error=SE::none;
  });
  if(!crypto.ok())return {SE::crypto_failure};
  if(error!=SE::none)return {error};
  return {SE::none,owner.snapshot_};
}

api::NativeStartupTransactionResult NativeHostedDatabaseRuntime::PersistStartupTransactionDetail(CredentialRead& read) noexcept {
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return {E::busy};
  if(!read.ok()||!read.owner_||!read.startup_transaction_.state_||!read.session_||!read.authentication_)return {E::not_ready};
  auto& self=*read.owner_;auto& owner=*read.startup_transaction_.state_;
  const auto result=[&](E error){auto r=owner.result_;r.error=error;return r;};
  if(read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested())return result(E::cancelled);
  if(owner.expected_selected_phase_==P::detail_selected)return PublishStartupOwned(read,P::detail_selected);
  if(owner.result_.phase!=P::boundary_selected||!owner.publisher_||!owner.result_.selected||!owner.snapshot_)return result(E::not_ready);
  if(const auto e=LifetimeError(owner.CheckLifetime());e!=E::none)return result(e);
  auto& credential=*read.credentials_->comparison->lease;const auto& session=read.session_->observation_;
  const auto& snapshot=*owner.snapshot_;
  if(credential.has_selected_source()||!credential.has_retained_operation()||
      credential.writer_operation_uuid()!=owner.result_.current_publication_uuid||
      session.session_uuid!=owner.request_.binding.session_uuid||session.engine_instance_uuid!=self.engine_instance_||
      read.authentication_->observation_.decision_uuid!=owner.record_.security_snapshot_uuid||
      snapshot.database_uuid!=self.database_||snapshot.owning_transaction_uuid!=owner.request_.binding.transaction_uuid||
      snapshot.owning_transaction_number!=owner.request_.binding.local_transaction_id||
      !api::native_startup_transaction_history_detail::SameSelection(snapshot.inventory_source,owner.result_.selected->selection))
    return result(E::source_mismatch);
  const auto& access=owner.policies_.access_body;const bool permit_bootstrap=(access.flags&64)!=0;
  const auto& authorization=read.session_->authorization_;
  if(!authorization.Allows("CONNECT",self.database_,permit_bootstrap)||
      !authorization.Allows("CATALOG_MUTATE",self.database_,permit_bootstrap)||
      !authorization.Allows("OBS_AGENT_CONTROL",self.database_,permit_bootstrap)||
      !authorization.WithinLimits({access.maximum_records,access.maximum_membership_depth,access.maximum_work}))return result(E::authorization_refused);
  const auto& config=*read.configuration_.configuration;
  if(!self.process_->HasNativeDatabaseAllocationBehavior(self.database_,config.memory,config.memory_version_uuid))return result(E::policy_required);
  read.mutating_=true;self.entering_=true;E error=E::identity_failure;
  {
    struct Leave{bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}}leave{read.mutating_,self.entering_};
    const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
      if(!owner.successor_preparation_){
        const auto physical=owner.issuer_.Issue(core::platform::UuidKind::object);
        const auto event=owner.issuer_.Issue(core::platform::UuidKind::object);
        if(!physical.ok()||!event.ok())return;
        const auto clock=event.observation->wall_clock;
        if(clock.unix_seconds<=0||u64(clock.unix_seconds)>UINT64_MAX/1000000||clock.nanoseconds>=1000000000)return;
        api::NativeStartupTransactionDetail detail;
        detail.transaction_uuid=snapshot.owning_transaction_uuid;detail.database_uuid=self.database_;
        detail.engine_instance_uuid=session.engine_instance_uuid;detail.principal_uuid=session.principal_uuid;
        detail.attachment_uuid=session.attachment_uuid;detail.session_uuid=session.session_uuid;
        detail.policy_snapshot_uuid=snapshot.policy_snapshot_uuid;detail.security_snapshot_uuid=snapshot.security_snapshot_uuid;
        detail.visibility_epoch_uuid=snapshot.catalog_visibility_epoch_uuid;detail.isolation_profile_uuid=snapshot.isolation_profile_uuid;
        detail.operation_uuid=owner.request_.binding.operation_uuid;detail.data_snapshot_uuid=snapshot.snapshot_uuid;
        detail.isolation_version_uuid=snapshot.isolation_version_uuid;detail.transaction_policy_uuid=owner.transaction_policies_.descriptor.policy_uuid;
        detail.transaction_policy_version_uuid=owner.transaction_policies_.body_version_uuid;
        detail.local_transaction_id=snapshot.owning_transaction_number;detail.started_at_micros=owner.request_.initial.created_at_micros;
        detail.isolation_generation=snapshot.isolation_generation;detail.transaction_policy_generation=owner.transaction_policies_.body.generation;
        // Fence the admission-effects marker before acquiring transaction-owned
        // locks or mutable resources. The marker records the cleanup obligation,
        // not successful acquisition, active inventory or completed admission.
        detail.admission_side_effect_started=true;
        api::NativeStartupTransactionOwner::BoundaryPreparation next;
        next.publication_uuid=physical.value->value;next.state=owner.verification_state_;
        next.state.transaction_detail=detail;next.state.snapshot_boundary_uuid=snapshot.snapshot_uuid;
        next.state.updated_at_micros=u64(clock.unix_seconds)*1000000+clock.nanoseconds/1000;
        if(api::ValidateNativeVerificationOperationEvolution(owner.verification_state_,next.state)!=api::NativeVerificationStateError::none){
          error=E::request_failure;return;}
        const auto state=api::EncodeNativeVerificationOperationStateInto(next.state,owner.state_storage_[0].size(),owner.state_storage_[0]);
        if(!state.ok()){error=E::request_failure;return;}
        next.record=owner.record_;++next.record.revision;next.record.updated_at=event.value->value;next.record.operation_state_bytes=state.bytes;
        std::array<Uuid,7> identities;
        if(db::ValidateNativeManagementOperationEvolutionView(owner.record_,next.record,identities)!=db::NativeManagementOperationError::none){
          error=E::request_failure;return;}
        owner.successor_preparation_=next;
      }
      const auto& next=*owner.successor_preparation_;
      const auto advanced=owner.publisher_->Advance(credential.retained_operation_,next.publication_uuid);owner.result_.handoff_error=advanced;
      if(advanced!=db::NativeWriterError::none){error=advanced==db::NativeWriterError::resource_exhausted?E::resource_exhausted:E::publication_failure;return;}
      owner.source_=owner.result_.selected->selection;owner.publisher_.reset();owner.watermark_={};owner.watermark_result_={};
      owner.record_=next.record;owner.verification_state_=next.state;owner.result_.current_publication_uuid=next.publication_uuid;
      owner.result_.phase=P::prepared;owner.result_.selected.reset();owner.expected_selected_phase_=P::detail_selected;
      owner.successor_preparation_.reset();error=E::none;
    });
    if(!crypto.ok())error=E::crypto_failure;
  }
  if(error!=E::none)return result(error);
  return PublishStartupOwned(read,P::detail_selected);
}
api::NativeStartupPolicyResult NativeHostedDatabaseRuntime::EvaluateStartupTransactionPolicies(CredentialRead& read) noexcept {
  using PE=api::NativeStartupPolicyReadError;
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return {PE::busy};
  if(!read.ok()||!read.owner_||!read.startup_transaction_.state_||!read.session_||!read.authentication_)return {PE::not_ready};
  if(read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested())return {PE::cancelled};
  auto& self=*read.owner_;auto& owner=*read.startup_transaction_.state_;
  if((owner.result_.phase!=P::detail_selected&&owner.result_.phase!=P::admission_evidence_selected&&owner.result_.phase!=P::active_selected)||
      !owner.result_.selected||!owner.publisher_||!owner.snapshot_||
      !owner.verification_state_.transaction_detail||owner.verification_state_.transaction_detail->admission_side_effect_started!=true)
    return {PE::not_ready};
  const auto& credential=*read.credentials_->comparison->lease;const auto& session=read.session_->observation_;
  if(credential.has_selected_source()||!credential.has_retained_operation()||
      credential.writer_operation_uuid()!=owner.result_.current_publication_uuid||
      session.engine_instance_uuid!=self.engine_instance_||session.session_uuid!=owner.request_.binding.session_uuid||
      read.authentication_->observation_.decision_uuid!=owner.record_.security_snapshot_uuid||
      !owner.result_.selected->watermark.intent||owner.result_.selected->watermark.intent->startup_binding!=owner.request_.binding||
      owner.frozen_rows_.data()!=read.catalog_capture_.rows().data()||owner.frozen_rows_.size()!=read.catalog_capture_.rows().size())return {PE::source_mismatch};
  const auto& config=*read.configuration_.configuration;
  if(!self.process_->HasNativeDatabaseAllocationBehavior(self.database_,config.memory,config.memory_version_uuid)||
      !owner.transaction_policies_.ok())return {PE::policy_required};
  const auto lifetime=owner.CheckLifetime();
  if(lifetime!=api::NativeStartupLifetimeError::none)return {PE::lifetime_refused,api::NativeStartupPolicyEvaluationError::none,{},lifetime};
  if(owner.policy_observation_)return {PE::none,api::NativeStartupPolicyEvaluationError::none,owner.policy_observation_};
  const auto& authorization=read.session_->authorization_;const auto& access=owner.policies_.access_body;
  const bool bootstrap=(access.flags&64)!=0;
  if(!authorization.WithinLimits({access.maximum_records,access.maximum_membership_depth,access.maximum_work})||
      !authorization.Allows("CONNECT",self.database_,bootstrap)||!authorization.Allows("CATALOG_MUTATE",self.database_,bootstrap)||
      !authorization.Allows("OBS_AGENT_CONTROL",self.database_,bootstrap))return {PE::authorization_refused};
  const auto subjects=authorization.subjects();const auto files=read.catalog_capture_.filespaces();
  if(subjects.size()>owner.policy_subjects_.size()||files.size()>=owner.policy_targets_.size())return {PE::resource_exhausted};
  read.mutating_=true;self.entering_=true;
  struct Leave{bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}}leave{read.mutating_,self.entering_};
  api::NativeStartupPolicyResult result;result.error=PE::identity_failure;
  const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
    if(!owner.policy_evaluation_request_){
      const auto evaluation=owner.issuer_.Issue(core::platform::UuidKind::object);
      const auto decision=owner.issuer_.Issue(core::platform::UuidKind::object);
      if(!evaluation.ok()||!decision.ok())return;
      auto actors=std::span(owner.policy_subjects_).first(subjects.size());
      for(std::size_t i=0;i<subjects.size();++i)actors[i]=subjects[i].subject_uuid;
      std::sort(actors.begin(),actors.end());
      auto targets=std::span(owner.policy_targets_).first(files.size()+1);targets[0]=self.database_;
      for(std::size_t i=0;i<files.size();++i)targets[i+1]=files[i].bootstrap.filespace_uuid;
      std::sort(targets.begin(),targets.end());
      api::NativeStartupPolicyRequest request;
      request.evaluation_uuid=evaluation.value->value;request.database_uuid=self.database_;
      request.transaction_uuid=owner.request_.binding.transaction_uuid;request.session_uuid=session.session_uuid;
      request.policy_snapshot_uuid=owner.result_.policy_snapshot_uuid;request.transaction_number=owner.request_.binding.local_transaction_id;
      request.policy_generation_required=owner.transaction_policies_.descriptor.generation;
      request.acting_subject_uuids=actors;request.target_object_uuids=targets;
      owner.policy_evaluation_request_=request;owner.policy_evaluation_decision_uuid_=decision.value->value;
    }
    const auto& request=*owner.policy_evaluation_request_;
    owner.policy_evaluation_=api::EvaluateNativeStartupPolicies(read.catalog_capture_,owner.transaction_policies_,authorization,
        request,owner.policy_evaluation_decision_uuid_,owner.evaluated_policy_bindings_);
    const auto& evaluated=*owner.policy_evaluation_;result.evaluation_error=evaluated.error;
    if(!evaluated.evaluated()){result.error=PE::evaluation_failure;return;}
    api::NativeStartupPolicyObservation observation;
    observation.evaluation_uuid=request.evaluation_uuid;observation.decision_uuid=evaluated.decision_uuid;
    observation.database_uuid=self.database_;observation.transaction_uuid=request.transaction_uuid;
    observation.session_uuid=request.session_uuid;observation.policy_snapshot_uuid=request.policy_snapshot_uuid;
    observation.transaction_number=request.transaction_number;observation.decisive_policy_uuid=evaluated.decisive_policy_uuid;
    observation.effective_generation=evaluated.effective_generation;observation.subject_count=request.acting_subject_uuids.size();
    observation.target_count=request.target_object_uuids.size();observation.policy_count=evaluated.effective_policies.size();
    observation.outcome=evaluated.outcome;observation.reason=evaluated.reason;
    observation.cache_revalidation_required=evaluated.cache_revalidation_required;
    observation.audit_required=evaluated.audit_required;observation.evidence_required=evaluated.evidence_required;
    owner.policy_observation_=observation;result.observation=observation;result.error=PE::none;
  });
  if(!crypto.ok())return {PE::crypto_failure};
  return result;
}

api::NativeStartupTransactionResult NativeHostedDatabaseRuntime::PersistStartupAdmissionEvidence(CredentialRead& read) noexcept {
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return {E::busy};
  if(!read.ok()||!read.owner_||!read.startup_transaction_.state_||!read.session_||!read.authentication_)return {E::not_ready};
  auto& self=*read.owner_;auto& owner=*read.startup_transaction_.state_;
  const auto result=[&](E error){auto r=owner.result_;r.error=error;return r;};
  if(read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested())return result(E::cancelled);
  // An uncertain original attempt resumes its exact bytes even if the clock
  // has since expired. No replacement transaction/decision hides its effects.
  if(owner.expected_selected_phase_==P::admission_evidence_selected)return PublishStartupOwned(read,P::admission_evidence_selected);
  if(owner.result_.phase!=P::detail_selected||!owner.result_.selected||!owner.publisher_||!owner.snapshot_)return result(E::not_ready);
  const auto evaluated=EvaluateStartupTransactionPolicies(read);
  if(!evaluated.evaluated())return result(evaluated.error==api::NativeStartupPolicyReadError::lifetime_refused?
      LifetimeError(evaluated.lifetime_error):evaluated.error==api::NativeStartupPolicyReadError::authorization_refused?
      E::authorization_refused:E::policy_required);
  if(owner.candidate_.entries.size()!=2||owner.candidate_.entries[1].state!=transaction::mga::TransactionState::created||
      owner.candidate_.entries[1].identity.transaction_uuid.value!=owner.request_.binding.transaction_uuid||
      owner.candidate_.entries[1].identity.local_id.value!=owner.request_.binding.local_transaction_id)return result(E::source_mismatch);
  namespace c=core::catalog;using I=api::NativeAdmissionIdentity;using N=api::NativeAdmissionNumber;
  const auto& evidence_policy=owner.policies_.evidence;
  const auto has=[](const c::CatalogEvidenceStringSet& values,std::string_view key){
    return std::find(values.entries.begin(),values.entries.begin()+values.count,key)!=values.entries.begin()+values.count;
  };
  const auto primary=std::find_if(read.catalog_capture_.filespaces().begin(),read.catalog_capture_.filespaces().end(),
      [&](const auto& f){return f.bootstrap.filespace_uuid==evidence_policy.storage_filespace_uuid&&f.bootstrap.filespace_role==1;});
  if(evidence_policy.storage_filespace_uuid.is_nil()||primary==read.catalog_capture_.filespaces().end()||
      !has(evidence_policy.required_evidence_classes,"verification")||!has(evidence_policy.required_search_keys,"SB-MGA-VERIFY")||
      evidence_policy.retention_class!=c::EvidenceRetentionClass::audit||evidence_policy.redaction_rule!=c::EvidenceRedactionRule::structured_redaction||
      evidence_policy.digest_rule!=c::EvidenceDigestRule::sha256||evidence_policy.external_export_rule!=c::EvidenceExportRule::deny||
      evidence_policy.chain_of_custody_rule!=c::EvidenceCustodyRule::internal_mga||
      evidence_policy.cluster_replication_rule!=c::EvidenceReplicationRule::standalone||evidence_policy.cluster_uuid||
      evidence_policy.flags!=31)return result(E::policy_refused);
  auto& credential=*read.credentials_->comparison->lease;const auto& session=read.session_->observation_;
  const auto& authorization=read.session_->authorization_;const auto& config=*read.configuration_.configuration;
  const auto version=[&](const Uuid& id){Uuid found;
    for(const auto& r:owner.frozen_rows_)if(!r.row.metadata.record.header.deleted&&r.row.metadata.record.header.object_uuid.value==id){
      if(!found.is_nil())return Uuid{};found=r.row.version_uuid;}
    return found;
  };
  read.mutating_=true;self.entering_=true;E error=E::identity_failure;
  {
    struct Leave{bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}}leave{read.mutating_,self.entering_};
    const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
      if(!owner.admission_input_){
        api::NativeStartupAdmissionEvidenceInput input;
        input.Identity(I::database)=self.database_;input.Identity(I::engine)=self.engine_instance_;
        input.Identity(I::operation)=owner.request_.binding.operation_uuid;input.Identity(I::transaction)=owner.request_.binding.transaction_uuid;
        input.Identity(I::session)=session.session_uuid;input.Identity(I::actor)=session.principal_uuid;
        input.Identity(I::policy_snapshot)=owner.result_.policy_snapshot_uuid;input.Identity(I::security_snapshot)=owner.record_.security_snapshot_uuid;
        input.Identity(I::data_snapshot)=owner.snapshot_->snapshot_uuid;
        input.Identity(I::evaluation)=owner.policy_evaluation_request_->evaluation_uuid;input.Identity(I::decision)=owner.policy_evaluation_->decision_uuid;
        input.Identity(I::decisive_policy)=owner.policy_evaluation_->decisive_policy_uuid;
        const auto& access=owner.policies_.access_descriptor;
        input.Identity(I::access_policy)=access.policy_uuid;input.Identity(I::access_body)=access.body_uuid;
        input.Identity(I::access_descriptor_version)=version(access.policy_uuid);input.Identity(I::access_body_version)=version(access.body_uuid);
        const auto& evidence=owner.policies_.evidence_descriptor;
        input.Identity(I::evidence_policy)=evidence.policy_uuid;input.Identity(I::evidence_body)=evidence.body_uuid;
        input.Identity(I::evidence_descriptor_version)=version(evidence.policy_uuid);input.Identity(I::evidence_body_version)=version(evidence.body_uuid);
        input.Identity(I::storage_filespace)=evidence_policy.storage_filespace_uuid;
        std::size_t count=0;
        const auto add=[&](const Uuid& id){
          if(!core::uuid::IsEngineIdentityUuid(id))return false;
          if(std::find(owner.admission_versions_.begin(),owner.admission_versions_.begin()+count,id)!=owner.admission_versions_.begin()+count)return true;
          if(count==owner.admission_versions_.size())return false;owner.admission_versions_[count++]=id;return true;
        };
        error=E::source_mismatch;
        for(const auto id:{I::access_descriptor_version,I::access_body_version,I::evidence_descriptor_version,I::evidence_body_version})
          if(!add(input.Identity(id)))return;
        for(const auto& actor:authorization.subjects())if(!add(actor.version_uuid))return;
        for(const auto& edge:authorization.memberships())if(!add(edge.version_uuid))return;
        for(const auto& grant:authorization.grants())if(!add(grant.version_uuid))return;
        for(const auto& policy:owner.policy_evaluation_->effective_policies){
          if(!add(policy.descriptor_version_uuid))return;
          const auto body=version(policy.body_uuid);if(!body.is_nil()&&!add(body))return;
        }
        const auto& txn=owner.transaction_policies_;
        for(const auto id:{txn.body_version_uuid,txn.default_isolation_version_uuid,txn.startup_isolation_version_uuid,
            txn.selection_version_uuid,config.memory_version_uuid,owner.management_.descriptor_version_uuid})if(!add(id))return;
        for(const auto& lock:txn.locks)if(!add(lock.descriptor_version_uuid)||!add(lock.body_version_uuid))return;
        for(const auto& id:owner.management_.profile_versions)if(!add(id))return;
        auto versions=std::span(owner.admission_versions_).first(count);std::sort(versions.begin(),versions.end());
        input.metadata_versions=versions;input.subjects=owner.policy_evaluation_request_->acting_subject_uuids;
        input.targets=owner.policy_evaluation_request_->target_object_uuids;input.policies=owner.policy_evaluation_->effective_policies;
        const auto size=640+16*(input.metadata_versions.size()+input.subjects.size()+input.targets.size())+80*input.policies.size();
        if(size>evidence_policy.maximum_record_bytes||size>evidence_policy.maximum_batch_bytes||size>owner.admission_storage_.size()){
          error=E::resource_exhausted;return;
        }
        const auto& source=*read.catalog_capture_.selection();input.source_checkpoint_sha256=source.checkpoint_sha256;
        input.Number(N::transaction_number)=owner.request_.binding.local_transaction_id;
        input.Number(N::catalog_generation)=credential.selected_catalog().catalog_generation;
        input.Number(N::security_epoch)=authorization.binding().security_epoch;
        input.Number(N::effective_policy_generation)=owner.policy_evaluation_->effective_generation;
        input.Number(N::access_generation)=access.generation;input.Number(N::access_body_generation)=access.body_generation;
        input.Number(N::evidence_generation)=evidence.generation;input.Number(N::evidence_body_generation)=evidence.body_generation;
        input.Number(N::selection_generation)=source.selection_generation;input.Number(N::checkpoint_generation)=source.checkpoint_generation;
        input.outcome=owner.policy_evaluation_->outcome;
        const std::string_view reason=owner.policy_evaluation_->reason;
        if(reason=="startup_policy_allow")input.reason=api::NativeAdmissionReason::startup_policy_allow;
        else if(reason=="applicable_policy_refusal")input.reason=api::NativeAdmissionReason::applicable_policy_refusal;
        else if(reason=="unresolved_applicable_body")input.reason=api::NativeAdmissionReason::unresolved_applicable_body;
        else if(reason=="policy_generation_changed")input.reason=api::NativeAdmissionReason::policy_generation_changed;
        else {error=E::request_failure;return;}
        error=E::identity_failure;
        const auto authorization_id=owner.issuer_.Issue(core::platform::UuidKind::object);
        const auto authorized=owner.issuer_.Issue(core::platform::UuidKind::object);
        const auto decided=owner.issuer_.Issue(core::platform::UuidKind::object);
        const auto evidence_id=owner.issuer_.Issue(core::platform::UuidKind::object);
        const auto physical=owner.issuer_.Issue(core::platform::UuidKind::object);
        if(!authorization_id.ok()||!authorized.ok()||!decided.ok()||!evidence_id.ok()||!physical.ok())return;
        const auto authorization_time=db::NativeConstructionAuditTimeMicros(authorized.observation->wall_clock);
        const auto policy_time=db::NativeConstructionAuditTimeMicros(decided.observation->wall_clock);
        if(!authorization_time||!policy_time){error=E::clock_failure;return;}
        input.Identity(I::authorization_decision)=authorization_id.value->value;
        input.Identity(I::authorization_event)=authorized.value->value;input.Identity(I::policy_event)=decided.value->value;
        input.Identity(I::evidence)=evidence_id.value->value;input.Number(N::authorization_time_utc_us)=*authorization_time;
        input.Number(N::policy_time_utc_us)=*policy_time;
        owner.admission_publication_uuid_=physical.value->value;owner.admission_input_=input;
      }
      if(owner.admission_bytes_.empty()){
        const auto encoded=api::EncodeNativeStartupAdmissionEvidenceInto(*owner.admission_input_,
            std::min(evidence_policy.maximum_record_bytes,evidence_policy.maximum_batch_bytes),owner.admission_storage_);
        if(!encoded.ok()){error=encoded.error==api::NativeAdmissionEvidenceError::hash_failure?E::crypto_failure:E::request_failure;return;}
        owner.admission_bytes_=encoded.bytes;
      }
      if(!owner.successor_preparation_){
        api::NativeStartupTransactionOwner::BoundaryPreparation next;next.publication_uuid=owner.admission_publication_uuid_;
        next.state=owner.verification_state_;next.state.startup_admission_evidence=owner.admission_bytes_;
        next.state.updated_at_micros=owner.admission_input_->Number(N::policy_time_utc_us);
        if(api::ValidateNativeVerificationOperationEvolution(owner.verification_state_,next.state)!=api::NativeVerificationStateError::none){error=E::request_failure;return;}
        const auto encoded=api::EncodeNativeVerificationOperationStateInto(next.state,owner.state_storage_[1].size(),owner.state_storage_[1]);
        if(!encoded.ok()){error=E::request_failure;return;}
        next.record=owner.record_;++next.record.revision;next.record.updated_at=owner.admission_input_->Identity(I::policy_event);
        next.record.operation_state_bytes=encoded.bytes;
        std::array<Uuid,7> identities;
        if(db::ValidateNativeManagementOperationEvolutionView(owner.record_,next.record,identities)!=db::NativeManagementOperationError::none){error=E::request_failure;return;}
        owner.successor_preparation_=next;
      }
      const auto& next=*owner.successor_preparation_;
      const auto advanced=owner.publisher_->Advance(credential.retained_operation_,next.publication_uuid);owner.result_.handoff_error=advanced;
      if(advanced!=db::NativeWriterError::none){error=advanced==db::NativeWriterError::resource_exhausted?E::resource_exhausted:E::publication_failure;return;}
      owner.source_=owner.result_.selected->selection;owner.publisher_.reset();owner.watermark_={};owner.watermark_result_={};
      owner.record_=next.record;owner.verification_state_=next.state;owner.result_.current_publication_uuid=next.publication_uuid;
      owner.result_.phase=P::prepared;owner.result_.selected.reset();owner.expected_selected_phase_=P::admission_evidence_selected;
      owner.successor_preparation_.reset();error=E::none;
    });
    if(!crypto.ok())error=E::crypto_failure;
  }
  if(error!=E::none)return result(error);
  return PublishStartupOwned(read,P::admission_evidence_selected);
}

api::NativeStartupTransactionResult NativeHostedDatabaseRuntime::ActivateStartupTransaction(CredentialRead& read) noexcept {
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return {E::busy};
  if(!read.ok()||!read.owner_||!read.startup_transaction_.state_||!read.session_||!read.authentication_)return {E::not_ready};
  auto& self=*read.owner_;auto& owner=*read.startup_transaction_.state_;
  const auto result=[&](E error){auto r=owner.result_;r.error=error;return r;};
  if(read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested())return result(E::cancelled);
  auto& lock=owner.maintenance_lock_;
  const auto owns_lock=[&]{return lock.table_==&self.startup_locks_&&self.startup_locks_.registration_==&lock&&
      lock.observation_.grant_generation==self.startup_locks_.generation_&&
      lock.observation_.transaction_uuid==owner.request_.binding.transaction_uuid;};
  if(owner.expected_selected_phase_==P::active_selected){
    if(!owns_lock())return result(E::source_mismatch);
    // Uncertain writes reconcile the original attempt even after expiry. An
    // already selected active transaction still needs its live elapsed clock.
    if(owner.result_.phase==P::active_selected)
      if(const auto e=LifetimeError(owner.CheckLifetime());e!=E::none)return result(e);
    return PublishStartupOwned(read,P::active_selected);
  }
  if(owner.result_.phase!=P::admission_evidence_selected||!owner.publisher_||!owner.result_.selected||!owner.snapshot_||
      !owner.verification_state_.transaction_detail||owner.verification_state_.transaction_detail->admission_side_effect_started!=true||
      owner.verification_state_.startup_admission_evidence.empty())return result(E::not_ready);
  const auto evaluated=EvaluateStartupTransactionPolicies(read);
  if(!evaluated.evaluated())return result(evaluated.error==api::NativeStartupPolicyReadError::lifetime_refused?
      LifetimeError(evaluated.lifetime_error):E::authorization_refused);
  if(!owner.policy_evaluation_||owner.policy_evaluation_->outcome!=api::NativeStartupPolicyOutcome::allow)
    return result(E::policy_refused);
  if(!owner.admission_input_||owner.admission_input_->outcome!=api::NativeStartupPolicyOutcome::allow)
    return result(E::source_mismatch);
  const auto& policies=owner.transaction_policies_;
  if(const auto refusal=StartupPolicyRefusal(policies,owner.request_.binding.local_transaction_id)){
    auto r=result(E::policy_refused);r.detail=refusal;return r;
  }
  // All memory for this one registration and its active candidate was charged
  // at owner allocation. No queue, graph, escalation or hidden fallback is
  // required for the private uncontended no-wait maintenance profile.
  for(const auto& selected:policies.locks){const auto& p=selected.body;
    // A zero lock count or memory bound is no limit (SB_DRIFT_LOCK_POLICY_ZERO_IS_NO_LIMIT).
    if(p.wait_behavior!=core::catalog::LockWaitBehavior::no_wait||
        (p.maximum_lock_memory_bytes&&p.maximum_lock_memory_bytes<sizeof(api::NativeStartupLockRegistration))||
        p.flags!=core::catalog::kLockPolicyMandatoryFlags)return result(E::policy_refused);
  }
  std::size_t active=0;
  for(const auto& entry:owner.candidate_.entries)
    if(entry.state==transaction::mga::TransactionState::active||entry.state==transaction::mga::TransactionState::read_only_active)++active;
  // Zero is no limit (SB_DRIFT_TRANSACTION_POLICY_ZERO_IS_NO_LIMIT).
  if(policies.body.maximum_active_transactions&&active>=policies.body.maximum_active_transactions)
    return result(E::resource_exhausted);
  if(self.startup_locks_.registration_&&!owns_lock())return result(E::busy);
  if(!owns_lock()&&self.startup_locks_.generation_==UINT64_MAX)return result(E::resource_exhausted);
  auto& credential=*read.credentials_->comparison->lease;
  read.mutating_=true;self.entering_=true;E error=E::identity_failure;
  {
    struct Leave{bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}}leave{read.mutating_,self.entering_};
    const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
      if(!owns_lock()){
        std::array<Uuid,4> ids;
        for(auto& id:ids){const auto issued=owner.issuer_.Issue(core::platform::UuidKind::object);if(!issued.ok())return;id=issued.value->value;}
        if(const auto e=LifetimeError(owner.CheckLifetime());e!=E::none){error=e;return;}
        api::NativeStartupLockObservation grant;
        grant.request_id=ids[0];grant.requested_at_uuidv7=ids[1];grant.grant_id=ids[2];grant.granted_at_uuidv7=ids[3];
        grant.owner_uuid=grant.transaction_uuid=owner.request_.binding.transaction_uuid;
        grant.operation_id=owner.request_.binding.operation_uuid;grant.scope_uuid=grant.database_uuid=self.database_;
        grant.policy_epoch=read.session_->authorization_.binding().policy_epoch;
        grant.grant_generation=++self.startup_locks_.generation_;
        lock.observation_=grant;lock.table_=&self.startup_locks_;self.startup_locks_.registration_=&lock;
      }
      std::array<std::size_t,2> indices;std::array<byte,2> markers;
      api::NativeLocalTransactionCandidateRequest request;
      request.change=api::NativeLocalTransactionChange::activate;
      request.transaction_uuid={core::platform::UuidKind::transaction,owner.request_.binding.transaction_uuid};
      request.local_transaction_id=owner.request_.binding.local_transaction_id;
      const auto candidate=api::BuildNativeLocalTransactionCandidate(owner.candidate_,request,{owner.active_entries_,indices,markers});
      if(!candidate.ok()){error=E::request_failure;return;}
      if(!owner.successor_preparation_){
        const auto physical=owner.issuer_.Issue(core::platform::UuidKind::object);
        const auto event=owner.issuer_.Issue(core::platform::UuidKind::object);
        if(!physical.ok()||!event.ok())return;
        const auto time=db::NativeConstructionAuditTimeMicros(event.observation->wall_clock);
        if(!time)return;
        api::NativeStartupTransactionOwner::BoundaryPreparation next;
        next.publication_uuid=physical.value->value;next.state=owner.verification_state_;next.state.updated_at_micros=*time;
        if(api::ValidateNativeVerificationOperationEvolution(owner.verification_state_,next.state)!=api::NativeVerificationStateError::none){error=E::request_failure;return;}
        // Audit used slot 1. Keep those exact selected bytes intact until the
        // atomic handoff, and retain the original request and admission audit.
        const auto encoded=api::EncodeNativeVerificationOperationStateInto(next.state,owner.state_storage_[0].size(),owner.state_storage_[0]);
        if(!encoded.ok()){error=E::request_failure;return;}
        next.record=owner.record_;++next.record.revision;next.record.updated_at=event.value->value;next.record.operation_state_bytes=encoded.bytes;
        std::array<Uuid,7> identities;
        if(db::ValidateNativeManagementOperationEvolutionView(owner.record_,next.record,identities)!=db::NativeManagementOperationError::none){error=E::request_failure;return;}
        owner.successor_preparation_=next;
      }
      const auto& next=*owner.successor_preparation_;
      if(const auto e=LifetimeError(owner.CheckLifetime());e!=E::none){error=e;return;}
      const auto advanced=owner.publisher_->Advance(credential.retained_operation_,next.publication_uuid);owner.result_.handoff_error=advanced;
      if(advanced!=db::NativeWriterError::none){error=advanced==db::NativeWriterError::resource_exhausted?E::resource_exhausted:E::publication_failure;return;}
      owner.source_=owner.result_.selected->selection;owner.publisher_.reset();owner.watermark_={};owner.watermark_result_={};
      owner.candidate_=candidate.inventory;owner.record_=next.record;owner.verification_state_=next.state;
      owner.result_.current_publication_uuid=next.publication_uuid;owner.result_.phase=P::prepared;owner.result_.selected.reset();
      owner.expected_selected_phase_=P::active_selected;owner.successor_preparation_.reset();error=E::none;
    });
    if(!crypto.ok())error=E::crypto_failure;
  }
  if(error!=E::none)return result(error);
  return PublishStartupOwned(read,P::active_selected);
}

api::NativeStartupStorageIdentityResult NativeHostedDatabaseRuntime::VerifyStartupStorageIdentity(CredentialRead& read) noexcept {
  using IE=api::NativeStartupIdentityError;namespace disk=storage::disk;namespace mga=transaction::mga;
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return {IE::busy};
  if(!read.ok()||!read.owner_||!read.startup_transaction_.state_||!read.session_||!read.authentication_)return {IE::not_ready};
  auto& self=*read.owner_;auto& owner=*read.startup_transaction_.state_;
  const auto cancelled=[&]{return read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested();};
  if(cancelled())return {IE::cancelled};
  if(owner.result_.phase!=P::active_selected||!owner.publisher_||!owner.result_.selected||!owner.snapshot_||
      self.startup_locks_.registration_!=&owner.maintenance_lock_||owner.maintenance_lock_.table_!=&self.startup_locks_||
      self.startup_locks_.generation_!=owner.maintenance_lock_.observation_.grant_generation)return {IE::not_ready};
  const auto evaluated=EvaluateStartupTransactionPolicies(read);
  if(!evaluated.evaluated())return {evaluated.error==api::NativeStartupPolicyReadError::lifetime_refused?
      IE::lifetime_refused:IE::policy_refused,evaluated.lifetime_error};
  if(evaluated.observation->outcome!=api::NativeStartupPolicyOutcome::allow)return {IE::policy_refused};
  if(owner.verified_identity_)return {IE::none,api::NativeStartupLifetimeError::none,owner.verified_identity_};
  auto& credential=*read.credentials_->comparison->lease;
  if(credential.has_selected_source()||!credential.has_retained_operation()||
      credential.writer_operation_uuid()!=owner.result_.current_publication_uuid)return {IE::source_mismatch};
  const auto files=read.catalog_capture_.filespaces();
  std::size_t maximum_page=0;
  for(const auto& file:files){const auto* profile=disk::FindCanonicalFilespacePageProfile(file.bootstrap.page_size_profile_uuid);
    if(!profile||profile->page_size_bytes!=file.bootstrap.page_size_bytes)return {IE::source_mismatch};
    maximum_page=std::max<std::size_t>(maximum_page,profile->page_size_bytes);
  }
  if(files.empty()||!maximum_page)return {IE::source_mismatch};
  const auto image_work=maximum_page+32*sizeof(disk::FilespaceRootReference)+2*alignof(std::max_align_t);
  const auto source_work=owner.limits_.publication.selected.maximum_backing_bytes;
  if(source_work>SIZE_MAX-image_work)return {IE::resource_exhausted};
  // The same governed operation grant owns retained publication images as
  // well as this transient read. Charge its whole admitted ceiling plus the
  // actual transaction arena, rather than counting only the new page buffer.
  const auto state_bytes=read.startup_transaction_.backing_.Snapshot().retained_bytes;
  const auto publication_bytes=owner.result_.publication_reserved_bytes;
  if(source_work+image_work>publication_bytes||state_bytes>SIZE_MAX-publication_bytes)return {IE::resource_exhausted};
  const auto workspace_bytes=publication_bytes+state_bytes;
  for(const auto& policy:owner.policies_.bodies)
    if(policy.maximum_work_bytes<workspace_bytes||policy.maximum_scan_pages<files.size()+1)return {IE::resource_exhausted};
  read.mutating_=true;self.entering_=true;
  struct Leave{bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}}leave{read.mutating_,self.entering_};
  api::NativeStartupStorageIdentityResult result;result.error=IE::resource_exhausted;
  const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
    auto arena=owner.publication_memory_.CreateArena(owner.memory_binding_,image_work,maximum_page);
    if(!arena.ok())return;
    const auto image_block=arena.arena.Allocate(maximum_page,maximum_page);
    const auto roots_block=arena.arena.Allocate(32*sizeof(disk::FilespaceRootReference),alignof(disk::FilespaceRootReference));
    if(!image_block.ok()||!roots_block.ok())return;
    auto* roots=static_cast<disk::FilespaceRootReference*>(roots_block.pointer);std::uninitialized_value_construct_n(roots,32);
    auto& operation=credential.retained_operation_;
    auto source=operation.ObserveWithMemory(owner.limits_.publication.selected,owner.publication_memory_,owner.memory_binding_);
    if(!source.ok()){result.error=IE::source_failure;return;}
    const auto& selection=source.observation.selection();const auto& directory=source.observation.directory();
    if(!api::native_startup_transaction_history_detail::SameSelection(*selection.selection,owner.result_.selected->selection)){
      result.error=IE::source_mismatch;return;
    }
    const auto& inventory=selection.checkpoint_inventory.inventory;
    const auto& expected_inventory=owner.candidate_;
    if(inventory.next_local_transaction_id!=expected_inventory.next_local_transaction_id||
        inventory.next_commit_sequence!=expected_inventory.next_commit_sequence||inventory.entries.size()!=expected_inventory.entries.size()){
      result.error=IE::source_mismatch;return;
    }
    const auto own=std::find_if(inventory.entries.begin(),inventory.entries.end(),[&](const auto& e){return e.identity.local_id.value==owner.request_.binding.local_transaction_id;});
    if(own==inventory.entries.end()||own->identity.transaction_uuid.value!=owner.request_.binding.transaction_uuid||
        own->state!=mga::TransactionState::active||own->rollback_only||own->commit_sequence||own->evidence_record_written){result.error=IE::source_mismatch;return;}
    api::NativeStartupStorageIdentityObservation observation;
    observation.selection=*selection.selection;observation.transaction_uuid=own->identity.transaction_uuid.value;
    observation.snapshot_uuid=owner.snapshot_->snapshot_uuid;observation.workspace_bytes=workspace_bytes;
    observation.physical_bytes_read=source.physical_bytes_read;
    Uuid creation_operation;
    const auto live=[&]{
      if(cancelled()){result.error=IE::cancelled;return false;}
      result.lifetime_error=owner.CheckLifetime();
      if(result.lifetime_error!=api::NativeStartupLifetimeError::none){result.error=IE::lifetime_refused;return false;}
      return true;
    };
    for(const auto& page:directory.directory.pages)for(const auto& file:page.directory.records){
      if(!live())return;
      const auto& b=file.bootstrap;
      const auto expected=std::find_if(files.begin(),files.end(),[&](const auto& f){return f.bootstrap.filespace_uuid==b.filespace_uuid;});
      if(expected==files.end()||expected->bootstrap.page_size_profile_uuid!=b.page_size_profile_uuid||b.page_size_bytes>maximum_page||
          b.database_uuid!=self.database_||b.flags){result.error=IE::source_mismatch;return;}
      std::span<byte> image{static_cast<byte*>(image_block.pointer),b.page_size_bytes};
      const auto io=operation.ReadAtBounded(b.filespace_uuid,b.page_size_profile_uuid,0,image);
      if(!io.ok()){result.error=IE::io_failure;return;}
      observation.physical_bytes_read+=image.size();
      const disk::FilespaceBootstrapBinding binding{self.database_,b.filespace_uuid,b.page_size_profile_uuid};
      const auto zero=disk::DecodeFilespacePageZeroInto(image,{roots,32},&binding);
      if(!zero.ok()){result.error=IE::invalid_identity;return;}
      const auto& z=*zero.record;
      if(z.page_uuid!=file.page_zero_uuid||z.page_generation!=file.page_zero_generation||z.root_set_generation!=file.root_set_generation||
          z.total_pages!=file.total_pages||z.bootstrap.flags!=b.flags||z.bootstrap.checksum_profile_uuid!=b.checksum_profile_uuid||
          z.bootstrap.encryption_profile_uuid!=b.encryption_profile_uuid||z.bootstrap.durable_format_generation!=b.durable_format_generation||
          z.bootstrap.filespace_role!=b.filespace_role||z.bootstrap.lifecycle_state!=b.lifecycle_state){result.error=IE::invalid_identity;return;}
      if(b.filespace_uuid==self.primary_){
        creation_operation=z.creation_operation_uuid;
        observation.primary_filespace_uuid=b.filespace_uuid;observation.primary_page_uuid=z.page_uuid;
        observation.primary_profile_uuid=b.page_size_profile_uuid;observation.primary_page_generation=z.page_generation;
        if(z.roots.size()>observation.primary_roots.size()){result.error=IE::resource_exhausted;return;}
        std::copy(z.roots.begin(),z.roots.end(),observation.primary_roots.begin());observation.primary_root_count=z.roots.size();
      }
      ++observation.filespaces;++observation.identity_pages;
    }
    if(observation.filespaces!=files.size()){result.error=IE::source_mismatch;return;}
    if(!live())return;
    const auto& checkpoint=*selection.checkpoint_inventory.checkpoint;
    const auto target=std::find_if(checkpoint.roots.begin(),checkpoint.roots.end(),[](const auto& root){return root.role==8;});
    if(target==checkpoint.roots.end()||target->page_type!=8){result.error=IE::source_mismatch;return;}
    const auto allocated=api::FindNativeStartupRootAllocation(selection.allocation,*target);
    if(!allocated||!allocated->creator_operation_uuid.is_nil()){result.error=IE::invalid_identity;return;}
    const auto& ref=target->page;const auto* profile=disk::FindCanonicalFilespacePageProfile(ref.page_size_profile_uuid);
    if(!profile||profile->page_size_bytes>maximum_page||ref.page_number>UINT64_MAX/profile->page_size_bytes){result.error=IE::source_mismatch;return;}
    std::span<byte> image{static_cast<byte*>(image_block.pointer),profile->page_size_bytes};
    const auto io=operation.ReadAtBounded(ref.filespace_uuid,ref.page_size_profile_uuid,ref.page_number*u64{profile->page_size_bytes},image);
    if(!io.ok()){result.error=IE::io_failure;return;}
    observation.physical_bytes_read+=image.size();
    const auto digest=core::hash::ComputeSha256DigestNative(image.data(),image.size());
    if(!digest.ok()){result.error=IE::crypto_failure;return;}
    if(digest.digest!=target->sha256){result.error=IE::invalid_identity;return;}
    const disk::NativeCommonPageHeaderBinding binding{{self.database_,ref.filespace_uuid,ref.page_size_profile_uuid},
        ref.page_number,ref.page_generation,8,allocated->page_uuid};
    const auto header=disk::DecodeNativeCommonPageHeader(image.data(),128,&binding);
    const auto decoded=db::DecodeNativeSystemStateValue(image);
    if(!header.ok()||!decoded.ok()){result.error=IE::invalid_identity;return;}
    const auto& state=*decoded.state;
    const auto& genesis=*read.catalog_capture_.selection();
    // This first-create profile is still fenced. Startup verification must not
    // infer ready/clean or overwrite the constructor's original observation.
    if(state.object_uuid!=target->object_uuid||state.creator_transaction_uuid!=allocated->creator_transaction_uuid||
        state.creator_local_transaction_id!=allocated->creator_local_transaction_id||state.lifecycle!=db::NativeSystemLifecycle::creating||
        state.flags!=(db::NativeSystemFlag::dirty|db::NativeSystemFlag::write_fenced)||
        !state.checkpoint||*state.checkpoint!=genesis.checkpoint||state.checkpoint_generation!=genesis.checkpoint_generation||
        state.checkpoint_object_uuid!=genesis.checkpoint_object_uuid||state.transition_operation_uuid!=creation_operation||
        state.clean_local_transaction_id||!state.clean_transaction_uuid.is_nil()){
      result.error=IE::invalid_identity;return;
    }
    const auto creator=std::find_if(inventory.entries.begin(),inventory.entries.end(),[&](const auto& e){return e.identity.local_id.value==state.creator_local_transaction_id;});
    if(creator==inventory.entries.end()||creator->identity.transaction_uuid.value!=state.creator_transaction_uuid||
        creator->identity.scope!=mga::TransactionScope::local_node||!mga::HasCommittedInventoryOutcome(*creator)){
      result.error=IE::invalid_identity;return;
    }
    observation.system_state=state;++observation.identity_pages;
    if(!live())return;
    owner.verified_identity_=observation;result.observation=observation;result.error=IE::none;
  });
  if(!crypto.ok())return {IE::crypto_failure};
  return result;
}
api::NativeStartupTransactionResult NativeHostedDatabaseRuntime::AdvanceStartupVerification(
    CredentialRead& read,api::NativeVerificationPhase phase,std::string_view boundary) noexcept {
  // Private: callers have already performed the work for this exact edge.
  // No external phase/UUID/result can submit a verification transition.
  auto& self=*read.owner_;auto& owner=*read.startup_transaction_.state_;
  const auto result=[&](E error){auto r=owner.result_;r.error=error;return r;};
  if(owner.result_.phase!=P::active_selected||!owner.publisher_||!owner.result_.selected||
      boundary.empty()||boundary.size()>owner.verified_boundary_storage_[0].size())return result(E::not_ready);
  if(const auto e=LifetimeError(owner.CheckLifetime());e!=E::none)return result(e);
  auto& credential=*read.credentials_->comparison->lease;
  E error=E::identity_failure;
  {
    read.mutating_=true;self.entering_=true;
    struct Leave{bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}}leave{read.mutating_,self.entering_};
    const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
      if(!owner.successor_preparation_){
        const auto physical=owner.issuer_.Issue(core::platform::UuidKind::object);
        const auto event=owner.issuer_.Issue(core::platform::UuidKind::object);
        if(!physical.ok()||!event.ok())return;
        const auto time=db::NativeConstructionAuditTimeMicros(event.observation->wall_clock);if(!time)return;
        const unsigned slot=owner.record_.operation_state_bytes.data()==owner.state_storage_[0].data()?1:0;
        auto& text=owner.verified_boundary_storage_[slot];std::copy(boundary.begin(),boundary.end(),text.begin());
        api::NativeStartupTransactionOwner::BoundaryPreparation next;
        next.publication_uuid=physical.value->value;next.state=owner.verification_state_;
        next.state.phase=phase;next.state.updated_at_micros=*time;next.state.last_verified_boundary={text.data(),boundary.size()};
        if(api::ValidateNativeVerificationOperationEvolution(owner.verification_state_,next.state)!=api::NativeVerificationStateError::none){error=E::request_failure;return;}
        const auto encoded=api::EncodeNativeVerificationOperationStateInto(next.state,owner.state_storage_[slot].size(),owner.state_storage_[slot]);
        if(!encoded.ok()){error=E::request_failure;return;}
        next.record=owner.record_;++next.record.revision;next.record.updated_at=event.value->value;next.record.operation_state_bytes=encoded.bytes;
        std::array<Uuid,7> identities;
        if(db::ValidateNativeManagementOperationEvolutionView(owner.record_,next.record,identities)!=db::NativeManagementOperationError::none){error=E::request_failure;return;}
        owner.successor_preparation_=next;
      }
      const auto& next=*owner.successor_preparation_;
      if(next.state.phase!=phase||next.state.last_verified_boundary!=boundary){error=E::source_mismatch;return;}
      if(const auto e=LifetimeError(owner.CheckLifetime());e!=E::none){error=e;return;}
      const auto advanced=owner.publisher_->Advance(credential.retained_operation_,next.publication_uuid);owner.result_.handoff_error=advanced;
      if(advanced!=db::NativeWriterError::none){error=advanced==db::NativeWriterError::resource_exhausted?E::resource_exhausted:E::publication_failure;return;}
      owner.source_=owner.result_.selected->selection;owner.publisher_.reset();owner.watermark_={};owner.watermark_result_={};
      owner.record_=next.record;owner.verification_state_=next.state;owner.result_.current_publication_uuid=next.publication_uuid;
      owner.result_.phase=P::prepared;owner.result_.selected.reset();owner.expected_selected_phase_=P::active_selected;
      owner.successor_preparation_.reset();error=E::none;
    });
    if(!crypto.ok())error=E::crypto_failure;
  }
  if(error!=E::none)return result(error);
  return PublishStartupOwned(read,P::active_selected);
}

api::NativeStartupTransactionError NativeHostedDatabaseRuntime::ScanStartupStorageStructures(CredentialRead& read,bool inventory_scan) noexcept {
  namespace mga=transaction::mga;
  auto& self=*read.owner_;auto& owner=*read.startup_transaction_.state_;
  auto& retained=inventory_scan?owner.inventory_scan_:owner.allocation_scan_;
  if(retained)return E::none;
  const auto cancelled=[&]{return read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested();};
  const auto live=[&]{return cancelled()?E::cancelled:LifetimeError(owner.CheckLifetime());};
  if(const auto e=live();e!=E::none)return e;
  const auto map_bytes=owner.limits_.publication.graph_backing_bytes;
  const auto source_bytes=owner.limits_.publication.selected.maximum_backing_bytes;
  if(map_bytes>SIZE_MAX-source_bytes||map_bytes+source_bytes>owner.result_.publication_reserved_bytes)return E::resource_exhausted;
  const auto state_bytes=read.startup_transaction_.backing_.Snapshot().retained_bytes;
  if(state_bytes>SIZE_MAX-owner.result_.publication_reserved_bytes)return E::resource_exhausted;
  for(const auto& p:owner.policies_.bodies)
    if(p.maximum_work_bytes<state_bytes+owner.result_.publication_reserved_bytes)return E::resource_exhausted;
  read.mutating_=true;self.entering_=true;
  struct Leave{bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}}leave{read.mutating_,self.entering_};
  E error=E::resource_exhausted;
  const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
    // Allocate the reusable secondary-chain backing before acquiring any
    // physical read fences. The primary chain is already in selected source.
    db::NativeAllocationPreparedResult maps;
    db::NativeStorageArena image_arena;std::span<byte> image;
    if(inventory_scan){
      std::size_t maximum_page=0;
      for(const auto& file:read.catalog_capture_.filespaces())maximum_page=std::max<std::size_t>(maximum_page,file.bootstrap.page_size_bytes);
      if(!maximum_page||maximum_page>map_bytes){error=E::resource_exhausted;return;}
      auto prepared=owner.publication_memory_.CreateArena(owner.memory_binding_,maximum_page,maximum_page);
      if(!prepared.ok())return;
      const auto block=prepared.arena.Allocate(maximum_page,maximum_page);if(!block.ok())return;
      image={static_cast<byte*>(block.pointer),maximum_page};image_arena=std::move(prepared.arena);
    }else{
      maps=db::PrepareNativeAllocationRead(map_bytes,owner.publication_memory_,owner.memory_binding_);
      if(!maps.ok())return;
    }
    auto source=read.credentials_->comparison->lease->retained_operation_.ReadWithMemory(
        owner.limits_.publication.selected,owner.publication_memory_,owner.memory_binding_);
    if(!source.ok()){error=E::verification_failure;return;}
    const auto& lease=source.lease.source();const auto& selected=lease.selection();
    if(!api::native_startup_transaction_history_detail::SameSelection(*selected.selection,owner.result_.selected->selection)){
      error=E::source_mismatch;return;
    }
    api::NativeStartupStorageScanObservation observed;observed.selection=*selected.selection;
    observed.physical_bytes_read=source.physical_bytes_read;
    const auto& inventory=selected.checkpoint_inventory.inventory;
    // This private first-create owner has only the committed creator and its
    // one active startup transaction; ordinary work remains fenced.
    if(inventory.entries.size()!=owner.active_entries_.size()||inventory.next_local_transaction_id!=owner.candidate_.next_local_transaction_id||
        inventory.next_commit_sequence!=owner.candidate_.next_commit_sequence){error=E::source_mismatch;return;}
    const auto& creator=inventory.entries.front();const auto& active=inventory.entries.back();
    if(!mga::HasCommittedInventoryOutcome(creator)||creator.identity.transaction_uuid.value!=owner.active_entries_[0].identity.transaction_uuid.value||
        active.identity.transaction_uuid.value!=owner.request_.binding.transaction_uuid||active.identity.local_id.value!=owner.request_.binding.local_transaction_id||
        active.state!=mga::TransactionState::active||active.rollback_only||active.commit_sequence||active.evidence_record_written){error=E::source_mismatch;return;}
    std::array<u64,2> active_numbers,in_doubt;std::array<std::size_t,2> indices;std::array<byte,2> markers;
    const auto projection=api::ProjectNativeSnapshotInventory(
        {inventory.next_local_transaction_id,inventory.next_commit_sequence,inventory.entries},
        active.identity.transaction_uuid,active.identity.local_id.value,{active_numbers,in_doubt,indices,markers});
    if(!projection.ok()){error=E::verification_failure;return;}
    observed.horizons=projection.horizons;observed.transactions=inventory.entries.size();
    observed.active_transactions=projection.active.size();observed.in_doubt_transactions=projection.in_doubt.size();
    observed.inventory_pages=selected.checkpoint_inventory.inventory_pages.size();
    for(const auto& directory:lease.directory().directory.pages)for(const auto& member:directory.directory.records){
      if(const auto e=live();e!=E::none){error=e;return;}
      ++observed.filespaces;
      if(inventory_scan)continue;
      const auto allocation=db::NativeAllocationLeaseReader::Read(lease,member.bootstrap.filespace_uuid,
          owner.limits_.publication.selected.maximum_verification_image_bytes,maps.workspace);
      if(!allocation.ok()){error=E::verification_failure;return;}
      const auto coverage=api::CheckNativeStartupCatalogAllocation(read.catalog_capture_,allocation.chain);
      if(!coverage.ok()){error=E::verification_failure;return;}
      if(!owner.verified_identity_){error=E::not_ready;return;}
      const auto initial=api::CheckNativeStartupInitialControlAllocation(read.catalog_capture_,*owner.verified_identity_,allocation.chain);
      if(!initial.ok()){error=E::verification_failure;return;}
      observed.initial_control_pages+=initial.control_pages;
      observed.catalog_pages+=coverage.matched_pages;
      observed.physical_bytes_read+=allocation.physical_bytes_read;
      if(allocation.chain.pages.size()>UINT64_MAX-observed.allocation_pages||member.total_pages>UINT64_MAX-observed.capacity_pages){error=E::verification_failure;return;}
      observed.allocation_pages+=allocation.chain.pages.size();observed.capacity_pages+=member.total_pages;
      for(std::size_t n=0;n<observed.allocation_states.size();++n){
        if(allocation.chain.state_counts[n]>UINT64_MAX-observed.allocation_states[n]){error=E::verification_failure;return;}
        observed.allocation_states[n]+=allocation.chain.state_counts[n];
      }
      for(const auto& policy:owner.policies_.bodies)
        if(observed.allocation_pages>policy.maximum_scan_pages){error=E::resource_exhausted;return;}
    }
    if(observed.filespaces!=read.catalog_capture_.filespaces().size()){error=E::source_mismatch;return;}
    if(!inventory_scan&&(!read.catalog_capture_.has_physical_pages()||
        observed.catalog_pages!=read.catalog_capture_.physical_pages().size())){error=E::verification_failure;return;}
    for(const auto& policy:owner.policies_.bodies)
      if(observed.inventory_pages+(inventory_scan?2:0)>policy.maximum_scan_pages){error=E::resource_exhausted;return;}
    if(inventory_scan){
      const auto& roots=selected.checkpoint_inventory.checkpoint->roots;
      const auto h=std::find_if(roots.begin(),roots.end(),[](const auto& root){return root.role==2;});
      const auto r=std::find_if(roots.begin(),roots.end(),[](const auto& root){return root.role==10;});
      if(h==roots.end()||r==roots.end()||h->page_type!=0x302||r->page_type!=0x303){error=E::verification_failure;return;}
      const auto horizon=*h,retention=*r;const auto origin=creator.identity;
      const auto horizon_allocation=api::FindNativeStartupRootAllocation(selected.allocation,horizon);
      const auto retention_allocation=api::FindNativeStartupRootAllocation(selected.allocation,retention);
      if(!horizon_allocation||!retention_allocation){error=E::verification_failure;return;}
      for(const auto* record:{&*horizon_allocation,&*retention_allocation})
        if(!record->creator_operation_uuid.is_nil()||record->creator_transaction_uuid!=origin.transaction_uuid.value||
            record->creator_local_transaction_id!=origin.local_id.value){error=E::verification_failure;return;}
      // Copy the selected references before retiring the read backing. The
      // original exclusive operation still owns every bounded physical read.
      source.lease.Reset();
      const auto load=[&](const db::NativeCheckpointRootReference& target,const storage::page::NativeAllocationRecord& allocation)->std::span<const byte>{
        if(const auto e=live();e!=E::none){error=e;return {};}
        const auto& ref=target.page;const auto* profile=storage::disk::FindCanonicalFilespacePageProfile(ref.page_size_profile_uuid);
        if(!profile||profile->page_size_bytes>image.size()||ref.page_number>UINT64_MAX/profile->page_size_bytes){error=E::source_mismatch;return {};}
        auto bytes=image.first(profile->page_size_bytes);
        const auto io=read.credentials_->comparison->lease->retained_operation_.ReadAtBounded(
            ref.filespace_uuid,ref.page_size_profile_uuid,ref.page_number*u64{profile->page_size_bytes},bytes);
        if(!io.ok()){error=E::verification_failure;return {};}
        observed.physical_bytes_read+=bytes.size();
        const auto digest=core::hash::ComputeSha256DigestNative(bytes.data(),bytes.size());
        if(!digest.ok()){error=E::crypto_failure;return {};}
        const storage::disk::NativeCommonPageHeaderBinding binding{{self.database_,ref.filespace_uuid,ref.page_size_profile_uuid},
            ref.page_number,ref.page_generation,target.page_type,allocation.page_uuid};
        if(digest.digest!=target.sha256||!storage::disk::DecodeNativeCommonPageHeader(bytes.data(),128,&binding).ok()){
          error=E::verification_failure;return {};
        }
        return bytes;
      };
      namespace page=storage::page;
      const auto retained_image=load(retention,*retention_allocation);if(retained_image.empty())return;
      const auto pins=page::DecodeNativeRetentionPageInto(retained_image,{});
      if(!pins.ok()||pins.page->object_uuid!=retention.object_uuid||pins.page->creator_transaction_uuid!=origin.transaction_uuid.value||
          pins.page->creator_local_transaction_id!=origin.local_id.value||pins.page->flags||pins.page->total_pins||
          pins.page->first_record||pins.page->next||!pins.page->records.empty()){
        // A nonempty/chained retention owner is outside this exact first-create
        // profile. It cannot be discarded or silently treated as no blocker.
        error=E::verification_failure;return;
      }
      const auto retention_epoch=pins.page->epoch;
      const auto horizon_image=load(horizon,*horizon_allocation);if(horizon_image.empty())return;
      std::array<page::NativeHorizonRecord,3> records;
      std::array<page::NativeHorizonIdentityScratch,8> ids;
      std::array<page::NativeHorizonReferenceScratch,6> references;
      const auto decoded=page::DecodeNativeHorizonRootInto(horizon_image,records,ids,references);
      if(!decoded.ok()){error=E::verification_failure;return;}
      const auto& root=*decoded.root;
      if(root.object_uuid!=horizon.object_uuid||root.creator_transaction_uuid!=origin.transaction_uuid.value||
          root.creator_local_transaction_id!=origin.local_id.value||root.flags||root.total_records!=3||root.first_record||root.next||
          root.epoch!=retention_epoch||root.records.size()!=3||root.retention!=retention.page||
          root.retention_object_uuid!=retention.object_uuid||root.retention_sha256!=retention.sha256){error=E::verification_failure;return;}
      const std::array boundaries{observed.horizons.oldest_interesting_transaction.value,
          observed.horizons.oldest_active_transaction.value,observed.horizons.oldest_snapshot_transaction.value};
      unsigned seen=0;
      for(const auto& record:root.records){const auto kind=static_cast<unsigned>(record.kind);
        if(kind<1||kind>3||(seen&(1u<<kind))||record.owner_kind!=page::NativeHorizonOwner::engine||record.flags||
            record.local_boundary!=boundaries[kind-1]||record.timeline_uuid!=observed.selection.timeline_uuid||
            !record.pin_uuid.is_nil()||record.checkpoint||record.checkpoint_generation||!record.checkpoint_object_uuid.is_nil()){
          error=E::verification_failure;return;
        }
        seen|=1u<<kind;
      }
      if(seen!=14){error=E::verification_failure;return;}
      observed.horizon_records=root.records.size();observed.retention_pins=pins.page->total_pins;
    }
    if(const auto e=live();e!=E::none){error=e;return;}
    retained=observed;error=E::none;
  });
  return crypto.ok()?error:E::crypto_failure;
}

api::NativeStartupTransactionError NativeHostedDatabaseRuntime::ScanStartupCatalogRoutes(CredentialRead& read) noexcept {
  namespace disk=storage::disk;namespace page=storage::page;
  auto& self=*read.owner_;auto& owner=*read.startup_transaction_.state_;
  if(owner.catalog_routes_)return E::none;
  if(!owner.inventory_scan_||!read.catalogs_.ok()||!read.catalog_capture_.has_physical_pages()||
      !read.catalog_metadata_.ok()||!owner.policies_.selection||
      owner.frozen_rows_.data()!=read.catalog_capture_.rows().data()||owner.frozen_rows_.size()!=read.catalog_capture_.rows().size()||
      !api::native_startup_transaction_history_detail::SameSelection(
          *read.catalog_capture_.selection(),*owner.policies_.selection))return E::source_mismatch;
  const auto live=[&]{return read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested()?
      E::cancelled:LifetimeError(owner.CheckLifetime());};
  if(const auto e=live();e!=E::none)return e;
  std::size_t maximum_page=0;
  for(const auto& file:read.catalog_capture_.filespaces())maximum_page=std::max<std::size_t>(maximum_page,file.bootstrap.page_size_bytes);
  const auto source_bytes=owner.limits_.publication.selected.maximum_backing_bytes;
  const auto state_bytes=read.startup_transaction_.backing_.Snapshot().retained_bytes;
  if(!maximum_page||maximum_page>SIZE_MAX-source_bytes||source_bytes+maximum_page>owner.result_.publication_reserved_bytes||
      state_bytes>SIZE_MAX-owner.result_.publication_reserved_bytes)return E::resource_exhausted;
  for(const auto& p:owner.policies_.bodies)
    if(p.maximum_scan_pages<2||p.maximum_work_bytes<state_bytes+owner.result_.publication_reserved_bytes)return E::resource_exhausted;
  read.mutating_=true;self.entering_=true;
  struct Leave{bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}}leave{read.mutating_,self.entering_};
  E error=E::resource_exhausted;
  const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
    auto image_arena=owner.publication_memory_.CreateArena(owner.memory_binding_,maximum_page,maximum_page);
    if(!image_arena.ok())return;
    const auto block=image_arena.arena.Allocate(maximum_page,maximum_page);if(!block.ok())return;
    auto& operation=read.credentials_->comparison->lease->retained_operation_;
    auto source=operation.ReadWithMemory(owner.limits_.publication.selected,owner.publication_memory_,owner.memory_binding_);
    if(!source.ok()){error=E::verification_failure;return;}
    const auto& selected=source.lease.source().selection();
    if(!api::native_startup_transaction_history_detail::SameSelection(*selected.selection,owner.result_.selected->selection)){
      error=E::source_mismatch;return;
    }
    api::NativeStartupCatalogRouteObservation observation{*selected.selection,0,source.physical_bytes_read};
    std::array<db::NativeCheckpointRootReference,2> targets;
    std::array<page::NativeAllocationRecord,2> allocations;
    std::array<page::NativeCatalogRootReference,6> references;
    const auto& roots=selected.checkpoint_inventory.checkpoint->roots;
    const auto& inventory=selected.checkpoint_inventory.inventory;
    if(inventory.entries.size()!=owner.active_entries_.size()||
        !transaction::mga::HasCommittedInventoryOutcome(inventory.entries.front())||
        inventory.entries.front().identity.transaction_uuid.value!=owner.active_entries_[0].identity.transaction_uuid.value){
      error=E::source_mismatch;return;
    }
    const auto creator=inventory.entries.front().identity;
    u64 additional_bytes=0;
    for(std::size_t i=0;i<targets.size();++i){
      const auto at=std::find_if(roots.begin(),roots.end(),[&](const auto& r){return r.role==6+i;});
      if(at==roots.end()||at->page_type!=10+i||at->page.filespace_uuid!=self.primary_){error=E::verification_failure;return;}
      targets[i]=*at;
      const auto* profile=disk::FindCanonicalFilespacePageProfile(at->page.page_size_profile_uuid);
      if(!profile||profile->page_size_bytes>maximum_page||at->page.page_number>UINT64_MAX/profile->page_size_bytes){error=E::source_mismatch;return;}
      additional_bytes+=profile->page_size_bytes;
      const auto allocated=api::FindNativeStartupRootAllocation(selected.allocation,*at);
      if(!allocated||!allocated->creator_operation_uuid.is_nil()||allocated->creator_transaction_uuid!=creator.transaction_uuid.value||
          allocated->creator_local_transaction_id!=creator.local_id.value){error=E::verification_failure;return;}
      allocations[i]=*allocated;
    }
    const auto limit=owner.limits_.publication.selected.maximum_verification_image_bytes;
    if(source.lease.source().retained_image_bytes()>limit||additional_bytes>limit-source.lease.source().retained_image_bytes()){
      error=E::resource_exhausted;return;
    }
    // Only copied, verified references survive the source fence. All subsequent
    // reads still use the original exclusive operation; no new file is opened.
    source.lease.Reset();
    const auto physical=read.catalog_capture_.physical_pages();
    for(std::size_t i=0;i<targets.size();++i){
      if(const auto e=live();e!=E::none){error=e;return;}
      const auto& target=targets[i];const auto& ref=target.page;
      const auto* profile=disk::FindCanonicalFilespacePageProfile(ref.page_size_profile_uuid);
      std::span<byte> bytes{static_cast<byte*>(block.pointer),profile->page_size_bytes};
      const auto io=operation.ReadAtBounded(ref.filespace_uuid,ref.page_size_profile_uuid,ref.page_number*u64{profile->page_size_bytes},bytes);
      if(!io.ok()){error=E::verification_failure;return;}
      observation.physical_bytes_read+=bytes.size();
      const auto digest=core::hash::ComputeSha256DigestNative(bytes.data(),bytes.size());
      if(!digest.ok()){error=E::crypto_failure;return;}
      const auto decoded=page::DecodeNativeCatalogRootInto(bytes,references);
      if(digest.digest!=target.sha256||!decoded.ok()){error=E::verification_failure;return;}
      const auto& root=*decoded.root;const auto& h=root.header;
      if(h.database_uuid!=self.database_||h.filespace_uuid!=ref.filespace_uuid||h.page_size_profile_uuid!=ref.page_size_profile_uuid||
          h.page_number!=ref.page_number||h.page_generation!=ref.page_generation||h.page_type!=target.page_type||h.flags||
          h.page_uuid!=allocations[i].page_uuid||root.root_kind!=6+i||root.object_uuid!=target.object_uuid||
          root.creator_transaction_uuid!=creator.transaction_uuid.value||root.creator_local_transaction_id!=creator.local_id.value||
          root.predecessor||root.roots.size()!=1){error=E::verification_failure;return;}
      const auto& route=root.roots.front();const auto expected_role=i==0?5u:4u;
      if(route.role!=expected_role){error=E::verification_failure;return;}
      const db::NativeCatalogRelationInspection* relation=nullptr;
      for(std::size_t n=0;n<read.catalogs_.relation_count;++n){const auto& candidate=read.catalogs_.relations[n];
        if(candidate.selector==2&&candidate.role==expected_role){
          if(relation){error=E::verification_failure;return;}relation=&candidate;
        }
      }
      if(!relation||route.page!=relation->root||root.catalog_generation!=relation->catalog_generation||
          root.schema_epoch!=relation->schema_epoch||root.security_epoch!=relation->security_epoch||root.resource_epoch!=relation->resource_epoch){
        error=E::verification_failure;return;
      }
      const auto page=std::find_if(physical.begin(),physical.end(),[&](const auto& p){
        return p.header.filespace_uuid==route.page.filespace_uuid&&p.header.page_number==route.page.page_number;});
      if(page==physical.end()||page->header.page_generation!=route.page.page_generation||
          page->header.page_size_profile_uuid!=route.page.page_size_profile_uuid||page->header.page_type!=route.page_type||
          page->owner_uuid!=route.object_uuid){error=E::verification_failure;return;}
      ++observation.routes;
    }
    if(const auto e=live();e!=E::none){error=e;return;}
    owner.catalog_routes_=observation;error=E::none;
  });
  return crypto.ok()?error:E::crypto_failure;
}

api::NativeStartupTransactionResult NativeHostedDatabaseRuntime::VerifyStartupStorageStructures(CredentialRead& read) noexcept {
  using V=api::NativeVerificationPhase;
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return {E::busy};
  if(!read.ok()||!read.owner_||!read.startup_transaction_.state_||!read.session_||!read.authentication_)return {E::not_ready};
  auto& owner=*read.startup_transaction_.state_;
  const auto result=[&](E error){auto r=owner.result_;r.error=error;return r;};
  if(read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested())return result(E::cancelled);
  if(owner.expected_selected_phase_!=P::active_selected||!owner.snapshot_||!ObserveStartupLock(read))return result(E::not_ready);
  // Finish an already reserved attempt with its original bytes before applying
  // the live clock gate to further work. Expiry cannot invent rollback.
  if(owner.result_.phase!=P::active_selected){const auto resumed=PublishStartupOwned(read,P::active_selected);if(!resumed.ok())return resumed;}
  const auto policy=EvaluateStartupTransactionPolicies(read);
  if(!policy.evaluated())return result(policy.error==api::NativeStartupPolicyReadError::lifetime_refused?LifetimeError(policy.lifetime_error):E::policy_refused);
  if(policy.observation->outcome!=api::NativeStartupPolicyOutcome::allow)return result(E::policy_refused);
  for(;;){
    const auto phase=owner.verification_state_.phase;
    if(phase==V::catalog_verifying)return result(ScanStartupCatalogRoutes(read));
    std::array<char,512> text{};std::size_t used=0;bool formatted=true;
    const auto append=[&](std::string_view value){if(value.size()>text.size()-used){formatted=false;return;}
      std::copy(value.begin(),value.end(),text.begin()+used);used+=value.size();};
    const auto number=[&](std::string_view key,u64 value){append(key);const auto encoded=std::to_chars(text.data()+used,text.data()+text.size(),value);
      if(encoded.ec!=std::errc{})formatted=false;else used=encoded.ptr-text.data();};
    const auto source=[&](const db::NativeCheckpointSelection& selected){number(" selection=",selected.selection_generation);number(" checkpoint=",selected.checkpoint_generation);
      append(" sha256=");constexpr char hex[]="0123456789abcdef";for(const auto b:selected.checkpoint_sha256){char pair[]{hex[b>>4],hex[b&15]};append({pair,2});}};
    V next;
    if(phase==V::boundary_established){
      const auto identity=VerifyStartupStorageIdentity(read);
      if(!identity.ok())return result(identity.error==api::NativeStartupIdentityError::lifetime_refused?LifetimeError(identity.lifetime_error):E::verification_failure);
      append("identity verified");source(identity.observation->selection);number(" filespaces=",identity.observation->filespaces);
      number(" identity_pages=",identity.observation->identity_pages);append(" findings=0");next=V::identity_verified;
    }else if(phase==V::identity_verified){
      append("allocation scan begins");source(owner.result_.selected->selection);next=V::allocation_verifying;
    }else if(phase==V::allocation_verifying||phase==V::transaction_inventory_verifying){
      const bool inventory=phase==V::transaction_inventory_verifying;
      const auto scanned=ScanStartupStorageStructures(read,inventory);if(scanned!=E::none)return result(scanned);
      const auto& scan=*(inventory?owner.inventory_scan_:owner.allocation_scan_);
      append(inventory?"inventory verified":"allocation verified");source(scan.selection);number(" filespaces=",scan.filespaces);
      if(inventory){number(" pages=",scan.inventory_pages);number(" transactions=",scan.transactions);number(" active=",scan.active_transactions);
        number(" in_doubt=",scan.in_doubt_transactions);number(" OIT=",scan.horizons.oldest_interesting_transaction.value);
        number(" OAT=",scan.horizons.oldest_active_transaction.value);number(" OST=",scan.horizons.oldest_snapshot_transaction.value);
        number(" horizon_records=",scan.horizon_records);number(" retention_pins=",scan.retention_pins);
        next=V::catalog_verifying;
      }else{number(" maps=",scan.allocation_pages);number(" capacity_pages=",scan.capacity_pages);number(" catalog_pages=",scan.catalog_pages);
        number(" initial_control_pages=",scan.initial_control_pages);
        for(const auto count:scan.allocation_states)number(" state=",count);next=V::transaction_inventory_verifying;}
      append(" findings=0");
    }else return result(E::not_ready);
    if(!formatted)return result(E::resource_exhausted);
    const auto advanced=AdvanceStartupVerification(read,next,{text.data(),used});if(!advanced.ok())return advanced;
  }
}
} // namespace scratchbird::server

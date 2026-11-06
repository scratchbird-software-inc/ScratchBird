// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_hosted_database_runtime.hpp"

namespace scratchbird::server {
// SEARCH_KEY: NATIVE_HOSTED_OPEN_UNDER_HELD_PROCESS_ADMISSION
// Open reserves the single process-to-database admission itself, so a caller
// that already holds it can never call Open -- TryReserve is a one-shot
// compare-exchange from unbound. StartHostedEngine is exactly such a caller.
//
// It must not simply hand the reservation over, either: StartHostedEngine
// commits its own reservation at the end of a successful open and
// std::terminate()s if that commit fails, so transferring ownership would
// abort the server rather than degrade. OpenUnderHeldAdmission therefore
// BORROWS: it neither reserves nor stores, taking a const reference to the
// caller's reservation purely as proof the caller holds one. process_admission_
// stays empty, so CloseImpl's release is a no-op and the caller's commit
// remains the single authority over the gate.
NativeHostedRuntimeOpenResult NativeHostedDatabaseRuntime::Open(ServerProcessCryptoOwner& process,
    const DatabaseOwnershipRequest& request, NativeHostedRuntimeLimits limits,
    std::vector<std::unique_ptr<storage::disk::FileDevice>>& secondary,
    NativeBootstrapLineagePolicy lineage) noexcept {
  using E=NativeHostedRuntimeError;
  auto admission=ServerHostedProcessAdmission().TryReserve();
  if(!admission.ok()) {
    NativeHostedRuntimeOpenResult out;
    out.error=E::process_admission_failure;
    out.process_admission_error=admission.error;
    return out;
  }
  return OpenImpl(process,request,limits,secondary,
                  std::optional<HostedProcessAdmission::Reservation>(
                      std::move(admission.reservation)),
                  nullptr,lineage);
}

NativeHostedRuntimeOpenResult NativeHostedDatabaseRuntime::OpenUnderHeldAdmission(
    ServerProcessCryptoOwner& process,
    const DatabaseOwnershipRequest& request, NativeHostedRuntimeLimits limits,
    std::vector<std::unique_ptr<storage::disk::FileDevice>>& secondary,
    const HostedProcessAdmission::Reservation& held,
    std::shared_ptr<DatabaseOwnershipLock> held_ownership,
    NativeBootstrapLineagePolicy lineage) noexcept {
  using E=NativeHostedRuntimeError;
  if (!held) {
    NativeHostedRuntimeOpenResult out;
    out.error=E::process_admission_failure;
    out.process_admission_error=HostedProcessAdmissionError::opening;
    return out;
  }
  return OpenImpl(process,request,limits,secondary,std::nullopt,
                  std::move(held_ownership),lineage);
}

NativeHostedRuntimeOpenResult NativeHostedDatabaseRuntime::OpenImpl(
    ServerProcessCryptoOwner& process,
    const DatabaseOwnershipRequest& request, NativeHostedRuntimeLimits limits,
    std::vector<std::unique_ptr<storage::disk::FileDevice>>& secondary,
    std::optional<HostedProcessAdmission::Reservation>&& owned_admission,
    std::shared_ptr<DatabaseOwnershipLock> held_ownership,
    NativeBootstrapLineagePolicy lineage) noexcept {
  namespace db=storage::database;using E=NativeHostedRuntimeError;
  NativeHostedRuntimeOpenResult out;
  if (!process.installed()) { out.error=E::process_unavailable; return out; }
  if (request.storage_access!=storage::disk::RouteStorageAccess::read_write ||
      !limits.writer.maximum_owner_backing_bytes || !limits.writer.source.maximum_backing_bytes ||
      !limits.writer.source.maximum_verification_image_bytes || !limits.database_crypto_bytes ||
      !NativeRuntimePrincipalCredentialWorkspaceBytes(limits.catalog)||
      !engine::internal_api::NativeRuntimeAuthorizationWorkspaceBytes(limits.authorization)) return out;
  try {
    const DatabaseOwnershipRequest command=request;
    // Allocate result/close capacity before adopting any native device.
    out.runtime=std::shared_ptr<NativeHostedDatabaseRuntime>(new NativeHostedDatabaseRuntime(process));
    auto& self=*out.runtime;self.limits_=limits;self.entering_=true;
    // Empty when borrowing: the caller's reservation governs the gate.
    if (owned_admission) self.process_admission_.emplace(std::move(*owned_admission));
    struct Leave {bool& flag;~Leave(){flag=false;}} leave{self.entering_};
    // Identity of this actual native owner lifetime, issued before any device
    // guard. It neither reuses a memory-reservation UUID nor admits readiness.
    const auto instance=core::uuid::IssueRuntimeIdentityV7();
    if(!instance){out.error=E::identity_failure;return out;}
    self.engine_instance_=*instance;
    self.close_receipts_.resize(secondary.size()+1);
    // SEARCH_KEY: NATIVE_HOSTED_OPEN_UNDER_HELD_DATABASE_OWNERSHIP
    // Database ownership is the SECOND shared resource this open competes for.
    // AcquireDatabaseOwnership takes the exclusive database lock, which a
    // caller already serving this database necessarily holds -- that collision
    // is ownership_failure, and it is what broke the live server when this
    // path was first activated. When the caller supplies its lock, adopt it
    // instead of acquiring a second one. Custody still drives the route
    // transition from here, so the caller must not drive it concurrently.
    // SEARCH_KEY: NATIVE_PROBE_ACCEPTS_PRODUCTION_LINEAGE
    // Production-lineage acceptance is the caller's STATED policy. It was
    // previously inferred from whether the caller supplied its own ownership
    // lock, which coupled lineage acceptance to resource provenance: correct
    // only while the sole production caller was the borrow path. A native-first
    // open supplies no lock and so would have silently reverted to the
    // native-only probe and refused every production database with
    // bootstrap_failure -- the same symptom as the original format mismatch,
    // with a different cause. The default is native_only, so a caller that
    // states nothing cannot widen what the engine will mount.
    const bool production_lineage =
        lineage == NativeBootstrapLineagePolicy::accept_production_lineage;
    std::shared_ptr<DatabaseOwnershipLock> route_lock;
    if (held_ownership) {
      route_lock=std::move(held_ownership);
      out.ownership.acquired=true;
    } else {
      out.ownership=AcquireDatabaseOwnership(command);
      if (!out.ownership.acquired) {out.error=E::ownership_failure;return out;}
      route_lock=std::move(out.ownership.lock);
    }
    auto begun=NativeHostedDatabaseCustody::Begin(std::move(route_lock));
    out.custody_error=begun.error;out.route_error=begun.route_error;
    if (!begun.ok()) {out.error=E::custody_failure;return out;}
    self.custody_=std::move(begun.custody);
    out.probe=NativeHostedDatabaseCustody::ProbeBootstrap(self.custody_,production_lineage);
    if (!out.probe.ok()) {out.error=E::bootstrap_failure;return out;}
    // Probed identities select memory admission only. Adoption below validates
    // the complete actual native graph against this same retained inode.
    self.database_=out.probe.source.bootstrap->database_uuid;
    self.primary_=out.probe.source.bootstrap->filespace_uuid;
    out.crypto=process.PrepareNativeLocalPasswordAlgorithms();
    if (!out.crypto.ok()) {out.error=E::crypto_failure;return out;}
    out.memory=self.Acquire(limits.database_crypto_bytes,NativeBootstrapMemoryKind::database_crypto);
    if (!out.memory.ok()) {out.error=E::memory_failure;return out;}
    const auto crypto=StorageBinding(*out.memory.admission.resource);
    self.crypto_binding_={crypto.database_uuid,crypto.operation_uuid,crypto.owner_uuid,crypto.context_uuid};
    out.crypto=self.database_crypto_.Adopt(self.crypto_binding_,out.memory.admission.resource,
        limits.database_crypto_bytes,core::memory::kDefaultNativeCryptoProtectedMemoryPolicy);
    if (!out.crypto.ok()) {out.error=E::crypto_failure;return out;}
    db::NativeStorageMemory persistent,transient;
    for (auto [bytes,destination] : {
        std::pair{limits.writer.maximum_owner_backing_bytes,&persistent},
        std::pair{limits.writer.source.maximum_backing_bytes,&transient},
        std::pair{limits.writer.source.maximum_backing_bytes,&self.reading_},
        std::pair{NativeRuntimePrincipalCredentialWorkspaceBytes(limits.catalog),&self.catalog_},
        std::pair{engine::internal_api::NativeRuntimeAuthorizationWorkspaceBytes(limits.authorization),&self.authorization_}}) {
      out.memory=self.Acquire(bytes,NativeBootstrapMemoryKind::storage);
      if (!out.memory.ok()) {out.error=E::memory_failure;return out;}
      auto adopted=db::AdoptNativeStorageMemory(StorageBinding(*out.memory.admission.resource),out.memory.admission.resource);
      out.memory_binding_error=adopted.error;
      if (!adopted.ok()) {out.error=E::memory_failure;return out;}
      *destination=std::move(adopted.memory);
    }
    out.crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&] {
      out.adoption=NativeHostedDatabaseCustody::AdoptWriter(self.custody_,self.database_,self.primary_,secondary,
          limits.writer,persistent,persistent.binding(),transient,transient.binding());
    });
    if (!out.crypto.ok()) {out.error=E::crypto_failure;return out;}
    if (!out.adoption.ok()) {out.error=E::adoption_failure;return out;}
    self.writer_adopted_=true;out.error=E::none;return out;
  } catch (const std::bad_alloc&) {out.error=E::resource_exhausted;return out;}
    catch (const std::system_error& error) {
      out.error=E::synchronization_failure;out.synchronization_error=error.code();return out;
    }
}

NativeHostedRuntimeCloseResult NativeHostedDatabaseRuntime::Close(
    std::shared_ptr<NativeHostedDatabaseRuntime> owner) noexcept {
  if (!owner) return {};
  return CloseImpl(*owner);
}

NativeHostedRuntimeCloseResult NativeHostedDatabaseRuntime::CloseImpl(
    NativeHostedDatabaseRuntime& self) noexcept {
  using E=NativeHostedRuntimeError;NativeHostedRuntimeCloseResult out;
  auto* owner=&self;
  if (!owner->SameOwner()) {out.error=E::wrong_owner;return out;}
  if (owner->entering_) {out.error=E::busy;return out;}
  if (owner->closed_) {out.error=E::none;out.original_route_released=true;return out;}
  owner->entering_=true;struct Leave {bool& flag;~Leave(){flag=false;}} leave{owner->entering_};
  if (owner->writer_adopted_&&!owner->writer_retired_) {
    out.custody=NativeHostedDatabaseCustody::RetireWriter(owner->custody_,owner->close_receipts_);
    if (!out.custody.ok()) {out.error=E::custody_failure;return out;}
    out.custody.error=NativeHostedDatabaseCustody::RetireWriterBacking(owner->custody_);
    if (!out.custody.ok()) {out.error=E::custody_failure;return out;}
    owner->writer_retired_=true;
  }
  out.crypto=owner->database_crypto_.Close();
  if (!out.crypto.ok()) {out.error=E::crypto_failure;return out;}
  owner->reading_={};owner->catalog_={};owner->authorization_={};
  out.memory_envelope_error=owner->process_->RetireNativeDatabaseMemory(owner->database_);
  if(out.memory_envelope_error!=NativeDatabaseMemoryEnvelopeError::none){
    out.error=E::memory_envelope_failure;return out;
  }
  owner->custody_.reset();owner->closed_=true;
  // Physical-only admission has never published an activated hosted engine.
  // Release this provisional gate after the original OS owner actually retires.
  owner->process_admission_.reset();
  out.error=E::none;out.original_route_released=true;return out;
}

NativeHostedDatabaseRuntime::CredentialRead NativeHostedDatabaseRuntime::ReadStartupCredentials(
    std::shared_ptr<NativeHostedDatabaseRuntime> owner, NativeLocalServiceStartupCredentialsRequest request,
    std::shared_ptr<StartupCredentialSource> source, const core::memory::MemoryTag& tag,
    const core::memory::ScopedProtectedBuffer& derived) {
  using E=NativeHostedRuntimeError;CredentialRead out;
  if (!owner || !source) return out;
  if (!owner->SameOwner()) {out.error=E::wrong_owner;return out;}
  if (owner->entering_||owner->read_active_) {out.error=E::busy;return out;}
  if (owner->closed_||owner->writer_retired_) {out.error=E::closed;return out;}
  if (!owner->writer_adopted_||request.credential.security.database_uuid!=owner->database_ ||
      request.checkpoint_limits.maximum_backing_bytes>owner->limits_.writer.source.maximum_backing_bytes ||
      request.checkpoint_limits.maximum_verification_image_bytes>owner->limits_.writer.source.maximum_verification_image_bytes)
    return out;
  out.owner_=std::move(owner);out.source_=std::move(source);auto& self=*out.owner_;
  self.read_active_=true;self.entering_=true;
  struct Leave {bool& flag;~Leave(){flag=false;}} leave{self.entering_};
  try {out.inputs_=std::make_unique<CredentialRead::Inputs>(std::move(request),tag);}
  catch(const std::bad_alloc&){out.error=E::resource_exhausted;return out;}
  // Startup publication retires the now-idle bootstrap reader reservation.
  // A subsequent credential read obtains its own credit before any source or
  // device guard; the old reservation is never kept merely for possible reuse.
  if(!self.reading_.Matches(self.reading_.binding())) {
    auto allocation=self.Acquire(self.limits_.writer.source.maximum_backing_bytes,NativeBootstrapMemoryKind::storage);
    if(!allocation.ok()){out.error=E::memory_failure;return out;}
    auto memory=storage::database::AdoptNativeStorageMemory(StorageBinding(*allocation.admission.resource),allocation.admission.resource);
    if(!memory.ok()){out.error=E::memory_failure;return out;}
    self.reading_=std::move(memory.memory);
  }
  out.preparation_=PrepareNativeRuntimePrincipalCredential(self.limits_.catalog,self.catalog_,self.catalog_.binding());
  if (!out.preparation_.ok()) {out.error=E::preparation_failure;return out;}
  // Both readers and all graph/vocabulary/schema/output backing are funded by
  // this actual host before acquiring the protected-source or device guards.
  out.authorization_preparation_=engine::internal_api::PrepareNativeRuntimeAuthorizationProjection(
      self.limits_.authorization,self.authorization_,self.authorization_.binding());
  if(!out.authorization_preparation_.ok()){out.error=E::authorization_preparation_failure;return out;}
  const auto id=core::uuid::IssueRuntimeIdentityV7();
  if (!id) {out.error=E::identity_failure;return out;}
  auto operation=NativeHostedDatabaseCustody::TryBeginOperation(self.custody_,*id);
  out.custody_error=operation.error;out.writer_error=operation.writer_error;out.route_error=operation.route_error;
  if (!operation.ok()) {out.error=E::custody_failure;return out;}
  out.crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&] {
    out.credentials_.emplace(ReadNativeLocalServiceStartupCredentials(out.inputs_->request,std::move(operation.operation),
        out.preparation_.workspace,self.reading_,self.reading_.binding(),*out.source_,
        core::memory::DefaultMemoryManager(),out.inputs_->tag,derived));
  });
  out.error=!out.crypto.ok()?E::crypto_failure:out.credentials_->ok()?E::none:E::credential_failure;
  if(out.error==E::none){
    out.configuration_=SelectNativeLocalServiceStartupConfiguration(*out.credentials_->comparison->lease);
    if(!out.configuration_.ok())out.error=E::configuration_failure;
  }
  if(out.error==E::none){
    out.agents_=SelectNativeLocalServiceAgentConfiguration(*out.credentials_->comparison->lease);
    if(!out.agents_.ok())out.error=E::agent_configuration_failure;
  }
  if(out.error==E::none){
    out.authorization_=out.credentials_->comparison->lease->ProjectAuthorization(out.authorization_preparation_.workspace);
    if(!out.authorization_.ok())out.error=E::authorization_failure;
  }
  return out;
}

NativeHostedRuntimeError NativeHostedDatabaseRuntime::RetainStartupObservation(
    CredentialRead& read) noexcept {
  using E=NativeHostedRuntimeError;
  read.CheckOwner();
  if (read.mutating_) return E::busy;
  if (!read.ok()) return read.error==E::none?E::invalid_request:read.error;
  if (read.owner_->entering_) return E::busy;
  read.owner_->entering_=true;
  read.mutating_=true;
  struct Leave {bool& read_flag;bool& host_flag;~Leave(){read_flag=false;host_flag=false;}}
      leave{read.mutating_,read.owner_->entering_};
  auto& credentials=*read.credentials_;
  if(!credentials.comparison->lease->RetainHostedOperationAndReleaseDeviceGuards(credentials.operation))
    return E::custody_failure;
  // Schema/registry validation allocates only after the actual selected device
  // guards retire. Refusal preserves the exact source and operation for retry.
  if(!read.agent_policies_.ok())read.agent_policies_=ValidateAgentPolicies(read.agents_);
  if(!read.agent_policies_.ok())return E::agent_policy_failure;
  if(read.publication_.ok())return E::none;
  auto& self=*read.owner_;
  namespace db=storage::database;
  // Device guards are now retired, while the same original operation,
  // protected generation and full route/cohort custody remain privately held.
  // Admit all counter scratch BEFORE acquiring another native selected source.
  const auto bytes=db::NativeOwnedPublicationWatermarkWorkspaceBytes(
      self.limits_.catalog.maximum_profile,self.close_receipts_.size());
  if(!bytes)return E::invalid_request;
  auto allocation=self.Acquire(bytes,NativeBootstrapMemoryKind::storage);
  if(!allocation.ok())return E::memory_failure;
  auto adopted=db::AdoptNativeStorageMemory(StorageBinding(*allocation.admission.resource),allocation.admission.resource);
  if(!adopted.ok())return E::memory_failure;
  read.publication_preparation_.workspace={};read.publication_memory_=std::move(adopted.memory);
  read.publication_preparation_=db::PrepareNativeOwnedPublicationWatermark(self.limits_.catalog.maximum_profile,
      self.close_receipts_.size(),read.publication_memory_,read.publication_memory_.binding());
  if(!read.publication_preparation_.ok())return E::preparation_failure;
  const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
    read.publication_=read.publication_preparation_.workspace.Inspect(
        credentials.comparison->lease->retained_operation_,read.inputs_->request.checkpoint_limits,
        self.reading_,self.reading_.binding());
  });
  // This is a read of actual durable counter state, never implicit repair,
  // provider activation, authenticated session creation or agent BEGIN.
  return !crypto.ok()?E::crypto_failure:read.publication_.ok()?E::none:E::custody_failure;
}

NativeHostedRuntimeError NativeHostedDatabaseRuntime::ReadStartupCatalogs(CredentialRead& read,
    storage::database::NativeSelectedCheckpointMemoryLimits limits,
    const storage::database::NativeCatalogRelationMemoryLimits& requested_catalog_limits,
    storage::database::NativeCatalogCaptureLimits capture_limits) noexcept {
  namespace db=storage::database;using E=NativeHostedRuntimeError;
  const auto catalog_limits=requested_catalog_limits;
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return E::busy;
  if(!read.ok()||!read.owner_)return E::closed;
  auto& credential=*read.credentials_->comparison->lease;
  if(credential.has_selected_source()||!credential.has_retained_operation()||!read.publication_.ok())return E::custody_failure;
  if(!read.authentication_)return E::credential_failure;
  const auto& request=read.inputs_->request;
  if(!limits.maximum_backing_bytes||!limits.maximum_verification_image_bytes||
      limits.maximum_backing_bytes>request.checkpoint_limits.maximum_backing_bytes||
      limits.maximum_verification_image_bytes>request.checkpoint_limits.maximum_verification_image_bytes)return E::invalid_request;
  auto& self=*read.owner_;read.mutating_=true;self.entering_=true;
  struct Leave {bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}} leave{read.mutating_,self.entering_};
  const auto cancelled=[&]{return request.credential.stop.stop_requested()||request.source.stop.stop_requested();};
  if(cancelled())return E::cancelled;
  const auto workspace=db::NativeCatalogCohortWorkspaceBytes(catalog_limits);
  if(!workspace||workspace>SIZE_MAX-limits.maximum_backing_bytes||catalog_limits.maximum_devices<self.close_receipts_.size())return E::invalid_request;
  const bool capture_requested=capture_limits.maximum_rows||capture_limits.maximum_text_bytes||capture_limits.maximum_filespaces||
      capture_limits.maximum_transactions||capture_limits.maximum_physical_pages||capture_limits.maximum_initial_control_pages;
  const auto capture_bytes=capture_requested?db::NativeCatalogCaptureWorkspaceBytes(capture_limits):0;
  if(capture_requested&&!capture_bytes)return E::invalid_request;
  if(read.catalogs_.ok()){
    if(read.catalog_backing_bytes_>limits.maximum_backing_bytes||read.catalogs_.verified_image_bytes>limits.maximum_verification_image_bytes)return E::invalid_request;
    const auto* profile=storage::disk::FindCanonicalFilespacePageProfile(catalog_limits.maximum_profile);
    if(!profile)return E::invalid_request;
    for(std::size_t i=0;i<read.catalogs_.relation_count;++i){const auto& r=read.catalogs_.relations[i];
      if(r.base_pages>catalog_limits.maximum_base_pages||r.versions>catalog_limits.maximum_rows||r.bindings>catalog_limits.maximum_bindings||
          r.maximum_page_bytes>profile->page_size_bytes||r.navigation_pages>catalog_limits.navigation.maximum_pages||
          r.navigation_image_bytes>catalog_limits.navigation.maximum_retained_image_bytes)return E::invalid_request;}
    if(!capture_requested)return E::none;
    if(read.catalog_capture_.ok()){
      if(read.catalog_capture_.rows().size()>capture_limits.maximum_rows||
          read.catalog_capture_.text_bytes()>capture_limits.maximum_text_bytes||
          read.catalog_capture_.filespaces().size()>capture_limits.maximum_filespaces)return E::invalid_request;
      const auto inventory=read.catalog_capture_.inventory();
      if(capture_limits.maximum_transactions&&inventory&&inventory->inventory.entries.size()>capture_limits.maximum_transactions)return E::invalid_request;
      if(capture_limits.maximum_physical_pages&&read.catalog_capture_.physical_pages().size()>capture_limits.maximum_physical_pages)return E::invalid_request;
      if(capture_limits.maximum_initial_control_pages&&read.catalog_capture_.initial_control_pages().size()>capture_limits.maximum_initial_control_pages)return E::invalid_request;
      if((!capture_limits.maximum_transactions||inventory)&&
          (!capture_limits.maximum_physical_pages||read.catalog_capture_.has_physical_pages())&&
          (!capture_limits.maximum_initial_control_pages||read.catalog_capture_.has_initial_controls()))return E::none;
    }
  }
  const auto& config=*read.configuration_.configuration;
  if(!self.process_->HasNativeDatabaseAllocationBehavior(self.database_,config.memory,config.memory_version_uuid))return E::memory_failure;
  db::NativeCatalogCapture capture;
  if(capture_requested){
    // Persisted row backing has its own lifetime and accounted grant; the
    // much larger temporary selected/relation workspace retires after scanning.
    auto retained=self.Acquire(capture_bytes,NativeBootstrapMemoryKind::storage);
    if(!retained.ok())return E::memory_failure;
    auto memory=db::AdoptNativeStorageMemory(StorageBinding(*retained.admission.resource),retained.admission.resource);
    if(!memory.ok())return E::memory_failure;
    auto prepared=db::PrepareNativeCatalogCapture(capture_limits,memory.memory,memory.memory.binding());
    if(!prepared.ok())return E::memory_failure;
    capture=std::move(prepared.capture);
  }
  auto allocation=self.Acquire(workspace+limits.maximum_backing_bytes,NativeBootstrapMemoryKind::storage);
  if(!allocation.ok())return E::memory_failure;
  auto adopted=db::AdoptNativeStorageMemory(StorageBinding(*allocation.admission.resource),allocation.admission.resource);
  if(!adopted.ok())return E::memory_failure;
  auto prepared=db::PrepareNativeCatalogCohortRead(catalog_limits,adopted.memory,adopted.memory.binding());
  if(!prepared.ok())return E::memory_failure;
  E result=E::catalog_failure;db::NativeCatalogCohortInspection inspection;
  const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
    auto source=credential.retained_operation_.ReadWithMemory(limits,adopted.memory,adopted.memory.binding());
    if(!source.ok())return;
    const auto& actual=*source.lease.source().selection().selection;
    const auto& expected=read.publication_.snapshot->selection;
    if(source.lease.operation_uuid()!=read.authentication_->observation_.operation_uuid||
        actual.header.database_uuid!=self.database_||actual.header.filespace_uuid!=self.primary_||
        actual.object_uuid!=expected.object_uuid||actual.checkpoint!=expected.checkpoint||
        actual.checkpoint_sha256!=expected.checkpoint_sha256||actual.selection_generation!=expected.selection_generation||
        actual.publication_uuid!=expected.publication_uuid||actual.checkpoint_object_uuid!=expected.checkpoint_object_uuid||
        actual.timeline_uuid!=expected.timeline_uuid||actual.bootstrap_uuid!=expected.bootstrap_uuid||
        actual.checkpoint_generation!=expected.checkpoint_generation||actual.root_set_generation!=expected.root_set_generation||
        actual.previous_selection_generation!=expected.previous_selection_generation||
        actual.previous_checkpoint!=expected.previous_checkpoint||actual.previous_checkpoint_object_uuid!=expected.previous_checkpoint_object_uuid||
        actual.previous_checkpoint_sha256!=expected.previous_checkpoint_sha256){
      inspection.error=db::NativeCatalogCohortError::binding_mismatch;return;}
    inspection=db::NativeCatalogCohortLeaseReader::Read(source.lease.source(),limits.maximum_verification_image_bytes,
        prepared.workspace,capture_requested?&capture:nullptr);
    inspection.physical_bytes_read+=source.physical_bytes_read;
    if(inspection.ok())result=E::none;
    source.lease.Reset();
  });
  if(!crypto.ok())return E::crypto_failure;
  if(cancelled())return E::cancelled;
  if(result==E::none&&capture_requested){
    read.session_.reset();
    read.verification_policies_={};read.management_definition_={};read.transaction_policies_={};read.catalog_metadata_={};read.catalog_capture_=std::move(capture);
  }
  read.catalogs_=inspection;read.catalog_backing_bytes_=limits.maximum_backing_bytes;return result;
}

NativeHostedRuntimeError NativeHostedDatabaseRuntime::VerifyStartupCatalogMetadata(CredentialRead& read) noexcept {
  namespace db=storage::database;using E=NativeHostedRuntimeError;
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return E::busy;
  if(!read.ok()||!read.owner_)return E::closed;
  auto& credential=*read.credentials_->comparison->lease;
  if(credential.has_selected_source()||!credential.has_retained_operation()||!read.publication_.ok())return E::custody_failure;
  if(!read.authentication_)return E::credential_failure;
  if(!read.catalogs_.ok()||!read.catalog_capture_.ok())return E::catalog_failure;
  auto& self=*read.owner_;const auto& request=read.inputs_->request;
  read.mutating_=true;self.entering_=true;
  struct Leave {bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}} leave{read.mutating_,self.entering_};
  const auto cancelled=[&]{return request.credential.stop.stop_requested()||request.source.stop.stop_requested();};
  if(cancelled())return E::cancelled;
  if(read.catalog_metadata_.ok())return E::none;
  const auto& config=*read.configuration_.configuration;
  if(!self.process_->HasNativeDatabaseAllocationBehavior(self.database_,config.memory,config.memory_version_uuid))return E::memory_failure;
  const auto count=read.catalog_capture_.rows().size();
  const auto capacity=std::max<std::size_t>(count,1);
  constexpr std::size_t per_row=2*sizeof(std::size_t)+sizeof(core::platform::byte);
  constexpr std::size_t padding=3*(alignof(std::max_align_t)-1);
  if(capacity>(SIZE_MAX-padding)/per_row)return E::invalid_request;
  const auto bytes=capacity*per_row+padding;
  auto allocation=self.Acquire(bytes,NativeBootstrapMemoryKind::storage);
  if(!allocation.ok())return E::memory_failure;
  auto adopted=db::AdoptNativeStorageMemory(StorageBinding(*allocation.admission.resource),allocation.admission.resource);
  if(!adopted.ok())return E::memory_failure;
  auto workspace=adopted.memory.CreateArena(adopted.memory.binding(),bytes,alignof(std::size_t));
  if(!workspace.ok())return E::memory_failure;
  const auto order=workspace.arena.Allocate(capacity*sizeof(std::size_t),alignof(std::size_t));
  const auto parents=workspace.arena.Allocate(capacity*sizeof(std::size_t),alignof(std::size_t));
  const auto colors=workspace.arena.Allocate(capacity,alignof(core::platform::byte));
  if(!order.ok()||!parents.ok()||!colors.ok())return E::memory_failure;
  engine::internal_api::NativeCatalogMetadataVerification result;
  const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
    result=engine::internal_api::VerifyNativeCatalogMetadata(read.catalog_capture_,
        {{static_cast<std::size_t*>(order.pointer),count},{static_cast<std::size_t*>(parents.pointer),count},
         {static_cast<core::platform::byte*>(colors.pointer),count}});
  });
  if(!crypto.ok())return E::crypto_failure;
  if(cancelled())return E::cancelled;
  read.catalog_metadata_=result;
  return result.ok()?E::none:E::catalog_verification_failure;
}

NativeHostedRuntimeError NativeHostedDatabaseRuntime::ResolveStartupManagementDefinition(CredentialRead& read) noexcept {
  namespace db=storage::database;using E=NativeHostedRuntimeError;
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return E::busy;
  if(!read.ok()||!read.owner_)return E::closed;
  const auto& credential=*read.credentials_->comparison->lease;
  if(credential.has_selected_source()||!credential.has_retained_operation()||!read.publication_.ok())return E::custody_failure;
  if(!read.authentication_)return E::credential_failure;
  if(!read.catalog_metadata_.ok())return E::catalog_verification_failure;
  auto& self=*read.owner_;read.mutating_=true;self.entering_=true;
  struct Leave {bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}} leave{read.mutating_,self.entering_};
  const auto cancelled=[&]{return read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested();};
  if(cancelled())return E::cancelled;
  if(read.management_definition_.ok())return E::none;
  const auto& config=*read.configuration_.configuration;
  if(!self.process_->HasNativeDatabaseAllocationBehavior(self.database_,config.memory,config.memory_version_uuid))return E::memory_failure;
  const auto count=read.catalog_capture_.rows().size();
  if(!count||count>SIZE_MAX/sizeof(std::size_t))return E::invalid_request;
  const auto bytes=count*sizeof(std::size_t);
  auto allocation=self.Acquire(bytes,NativeBootstrapMemoryKind::storage);
  if(!allocation.ok())return E::memory_failure;
  auto adopted=db::AdoptNativeStorageMemory(StorageBinding(*allocation.admission.resource),allocation.admission.resource);
  if(!adopted.ok())return E::memory_failure;
  auto workspace=adopted.memory.CreateArena(adopted.memory.binding(),bytes,alignof(std::size_t));
  if(!workspace.ok())return E::memory_failure;
  const auto backing=workspace.arena.Allocate(bytes,alignof(std::size_t));
  if(!backing.ok())return E::memory_failure;
  auto result=engine::internal_api::ResolveNativeStartupManagementDefinition(read.catalog_capture_,
      {static_cast<std::size_t*>(backing.pointer),count});
  if(cancelled())return E::cancelled;
  read.management_definition_=result;
  return result.ok()?E::none:E::management_definition_failure;
}

NativeHostedRuntimeError NativeHostedDatabaseRuntime::ResolveStartupTransactionPolicies(CredentialRead& read) noexcept {
  namespace db=storage::database;using E=NativeHostedRuntimeError;
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return E::busy;
  if(!read.ok()||!read.owner_)return E::closed;
  const auto& credential=*read.credentials_->comparison->lease;
  if(credential.has_selected_source()||!credential.has_retained_operation()||!read.publication_.ok())return E::custody_failure;
  if(!read.authentication_)return E::credential_failure;
  if(!read.catalog_metadata_.ok())return E::catalog_verification_failure;
  auto& self=*read.owner_;read.mutating_=true;self.entering_=true;
  struct Leave {bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}} leave{read.mutating_,self.entering_};
  const auto cancelled=[&]{return read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested();};
  if(cancelled())return E::cancelled;
  if(read.transaction_policies_.ok())return E::none;
  const auto& config=*read.configuration_.configuration;
  if(!self.process_->HasNativeDatabaseAllocationBehavior(self.database_,config.memory,config.memory_version_uuid))return E::memory_failure;
  const auto count=read.catalog_capture_.rows().size();
  if(!count||count>SIZE_MAX/sizeof(std::size_t))return E::invalid_request;
  const auto bytes=count*sizeof(std::size_t);
  auto allocation=self.Acquire(bytes,NativeBootstrapMemoryKind::storage);
  if(!allocation.ok())return E::memory_failure;
  auto adopted=db::AdoptNativeStorageMemory(StorageBinding(*allocation.admission.resource),allocation.admission.resource);
  if(!adopted.ok())return E::memory_failure;
  auto workspace=adopted.memory.CreateArena(adopted.memory.binding(),bytes,alignof(std::size_t));
  if(!workspace.ok())return E::memory_failure;
  const auto backing=workspace.arena.Allocate(bytes,alignof(std::size_t));
  if(!backing.ok())return E::memory_failure;
  const engine::internal_api::NativeTransactionPolicyRequest request{config.declarations.pack.policy_pack_uuid,config.declarations.pack.generation};
  auto result=engine::internal_api::ResolveNativeTransactionPolicyDeclarations(read.catalog_capture_,request,
      {static_cast<std::size_t*>(backing.pointer),count});
  if(cancelled())return E::cancelled;
  read.transaction_policies_=result;
  return result.ok()?E::none:E::transaction_policy_failure;
}

NativeHostedRuntimeError NativeHostedDatabaseRuntime::ResolveConfiguredStartupVerificationPolicies(CredentialRead& read) noexcept {
  using E=NativeHostedRuntimeError;namespace c=core::catalog;namespace ia=engine::internal_api;
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return E::busy;
  if(!read.ok()||!read.owner_)return E::closed;
  if(!read.agent_policies_.ok())return E::agent_policy_failure;
  const c::CatalogAgentBootstrapDefinition* selected=nullptr;
  for(const auto& entry:read.agents_.entries()){
    const auto& definition=entry.definition;
    if(definition.kind!=c::AgentBootstrapDefinitionKind::policy||definition.agent_type!="agent_coordinator")continue;
    if(selected)return E::agent_policy_failure;
    selected=&definition;
  }
  if(!selected||!selected->startup_verification)return E::verification_policy_failure;
  const auto& v=*selected->startup_verification;
  ia::NativeVerificationPolicyRequest request{v.repair_policy_uuid,v.quarantine_policy_uuid,v.salvage_policy_uuid,v.evidence_policy_uuid};
  request.verification_levels=static_cast<core::platform::u32>(v.verification_levels);
  const auto klass=native_agent_configuration_detail::Property(selected->properties,"config.startup_verification_class");
  if(klass=="startup_full_verify")request.verification_class=ia::NativeVerificationClass::startup_full;
  else if(klass=="startup_light_verify")request.verification_class=ia::NativeVerificationClass::startup_light;
  else return E::agent_policy_failure;
  request.authority_class=ia::NativeVerificationAuthority::startup_recovery;
  return ResolveStartupVerificationPolicies(read,request);
}

NativeHostedRuntimeError NativeHostedDatabaseRuntime::ResolveStartupVerificationPolicies(
    CredentialRead& read,const engine::internal_api::NativeVerificationPolicyRequest& requested) noexcept {
  namespace db=storage::database;using E=NativeHostedRuntimeError;
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return E::busy;
  if(!read.ok()||!read.owner_)return E::closed;
  const auto& credential=*read.credentials_->comparison->lease;
  if(credential.has_selected_source()||!credential.has_retained_operation()||!read.publication_.ok())return E::custody_failure;
  if(!read.authentication_)return E::credential_failure;
  if(!read.catalog_metadata_.ok())return E::catalog_verification_failure;
  const auto request=requested;
  auto& self=*read.owner_;read.mutating_=true;self.entering_=true;
  struct Leave {bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}} leave{read.mutating_,self.entering_};
  const auto cancelled=[&]{return read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested();};
  if(cancelled())return E::cancelled;
  if(read.verification_policies_.ok())return read.verification_policies_.request==request?E::none:E::invalid_request;
  const auto& config=*read.configuration_.configuration;
  if(!self.process_->HasNativeDatabaseAllocationBehavior(self.database_,config.memory,config.memory_version_uuid))return E::memory_failure;
  const auto count=read.catalog_capture_.rows().size();
  if(!count||count>SIZE_MAX/sizeof(std::size_t))return E::invalid_request;
  const auto bytes=count*sizeof(std::size_t);
  auto allocation=self.Acquire(bytes,NativeBootstrapMemoryKind::storage);
  if(!allocation.ok())return E::memory_failure;
  auto adopted=db::AdoptNativeStorageMemory(StorageBinding(*allocation.admission.resource),allocation.admission.resource);
  if(!adopted.ok())return E::memory_failure;
  auto workspace=adopted.memory.CreateArena(adopted.memory.binding(),bytes,alignof(std::size_t));
  if(!workspace.ok())return E::memory_failure;
  const auto backing=workspace.arena.Allocate(bytes,alignof(std::size_t));
  if(!backing.ok())return E::memory_failure;
  const auto result=engine::internal_api::ResolveNativeVerificationPolicyDeclarations(read.catalog_capture_,request,
      {static_cast<std::size_t*>(backing.pointer),count});
  if(cancelled())return E::cancelled;
  read.verification_policies_=result;
  return result.ok()?E::none:E::verification_policy_failure;
}

NativeDatabaseMemoryEnvelopeResult NativeHostedDatabaseRuntime::ConstrainStartupMemory(
    CredentialRead& read) noexcept {
  using E=NativeDatabaseMemoryEnvelopeError;
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return {E::busy};
  if(!read.ok()||!read.owner_)return {E::not_ready};
  const auto& credential=*read.credentials_->comparison->lease;
  if(credential.has_selected_source())return {E::selected_guards_held};
  if(!credential.has_retained_operation()||!read.publication_.ok()||!read.agent_policies_.ok())return {E::not_ready};
  if(!read.authorization_.projection.Allows("CONNECT",read.owner_->database_))return {E::authorization_refused};
  read.mutating_=true;read.owner_->entering_=true;
  struct Leave {bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}}
      leave{read.mutating_,read.owner_->entering_};
  const auto& selected=*read.configuration_.configuration;
  return read.owner_->process_->ConstrainNativeDatabaseMemory(read.owner_->database_,
      selected.memory,selected.memory_version_uuid);
}
NativeDatabaseAllocationBehaviorResult NativeHostedDatabaseRuntime::InstallStartupAllocationBehavior(
    CredentialRead& read) noexcept {
  using E=NativeDatabaseMemoryEnvelopeError;
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return {E::busy};
  if(!read.ok()||!read.owner_)return {E::not_ready};
  const auto& credential=*read.credentials_->comparison->lease;
  if(credential.has_selected_source())return {E::selected_guards_held};
  if(!credential.has_retained_operation()||!read.publication_.ok()||!read.agent_policies_.ok())return {E::not_ready};
  if(!read.authorization_.projection.Allows("CONNECT",read.owner_->database_))return {E::authorization_refused};
  read.mutating_=true;read.owner_->entering_=true;
  struct Leave {bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}}
      leave{read.mutating_,read.owner_->entering_};
  const auto& selected=*read.configuration_.configuration;
  return read.owner_->process_->InstallNativeDatabaseAllocationBehavior(read.owner_->database_,
      selected.memory,selected.memory_version_uuid);
}

engine::internal_api::NativeLocalServiceSessionResult NativeHostedDatabaseRuntime::AttachStartupServiceSession(
    CredentialRead& read) noexcept {
  namespace api=engine::internal_api;using E=api::NativeLocalServiceSessionError;
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return {E::busy};
  if(!read.ok()||!read.owner_||!read.authentication_)return {E::not_ready};
  const auto& credential=*read.credentials_->comparison->lease;
  if(credential.has_selected_source()||!credential.has_retained_operation()||!read.publication_.ok()||
      !read.catalog_capture_.ok()||!read.catalog_metadata_.ok())return {E::not_ready};
  const auto policies=ResolveConfiguredStartupVerificationPolicies(read);
  if(policies!=NativeHostedRuntimeError::none){
    if(policies==NativeHostedRuntimeError::cancelled){read.session_.reset();return {E::cancelled};}
    return {E::policy_required};
  }
  if(!storage::database::SameReadSource(*read.catalog_capture_.source(),
      storage::database::NativeReadSourceIdentityFromSelection(read.publication_.snapshot->selection)))
    return {E::source_mismatch};
  auto& self=*read.owner_;const auto& config=*read.configuration_.configuration;
  if(!self.process_->HasNativeDatabaseAllocationBehavior(self.database_,config.memory,config.memory_version_uuid))return {E::resource_policy_required};
  read.mutating_=true;self.entering_=true;
  struct Leave {bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}} leave{read.mutating_,self.entering_};
  const auto cancelled=[&]{return read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested();};
  if(cancelled()){read.session_.reset();return {E::cancelled};}
  if(read.session_){
    if(read.session_->observation_.authentication_decision_uuid!=read.authentication_->observation_.decision_uuid||
        !storage::database::SameReadSource(read.session_->observation_.source,*read.catalog_capture_.source()))
      return {E::source_mismatch};
    return {E::none,read.session_->observation_};
  }
  api::NativeLocalServiceSessionResult result;
  std::optional<api::NativeLocalServiceSession> issued;
  core::agents::AgentStartupClockPolicySelection clock;
  for(const auto& entry:read.agents_.entries())if(entry.definition.kind==core::catalog::AgentBootstrapDefinitionKind::policy&&
      entry.definition.agent_type=="agent_coordinator")clock=core::agents::SelectAgentStartupClockPolicy(entry.definition,entry.version_uuid);
  if(!clock.ok())return {E::policy_required};
  const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
    result=api::NativeLocalServiceSession::Attach(*read.authentication_,*read.catalog_capture_.source(),clock,
        self.engine_instance_,issued);
  });
  if(!crypto.ok())return {E::crypto_scope_unavailable};
  if(cancelled())return {E::cancelled};
  if(result.ok()&&issued)read.session_.emplace(std::move(*issued));
  return result;
}

engine::internal_api::NativeLocalServiceAuthenticationResult
NativeHostedDatabaseRuntime::AuthenticateStartupService(
    CredentialRead& read,const core::memory::ScopedProtectedBuffer& derived) noexcept {
  namespace api=engine::internal_api;
  using E=api::NativeLocalServiceAuthenticationError;
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return {E::busy};
  if(!read.ok()||!read.owner_)return {E::not_ready};
  auto& credential=*read.credentials_->comparison->lease;
  if(credential.has_selected_source())return {E::selected_guards_held};
  if(!credential.has_retained_operation()||!read.publication_.ok()||!read.agent_policies_.ok())return {E::not_ready};
  if(!read.authorization_.projection.Allows("CONNECT",read.owner_->database_))return {E::authorization_refused};
  const auto& selected=*read.configuration_.configuration;
  auto& owner=*read.owner_;
  if(!owner.process_->HasNativeDatabaseAllocationBehavior(owner.database_,selected.memory,selected.memory_version_uuid))
    return {E::resource_policy_required};
  read.mutating_=true;owner.entering_=true;
  struct Leave {bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}}
      leave{read.mutating_,owner.entering_};
  const auto& request=read.inputs_->request;
  if(request.credential.stop.stop_requested()||request.source.stop.stop_requested()){
    read.session_.reset();read.authentication_.reset();return {E::cancelled};
  }
  if(read.authentication_){
    api::NativeLocalServiceAuthenticationResult out;
    out.error=E::none;out.observation=read.authentication_->observation_;return out;
  }
  // All inputs come from the private, still fenced comparison owner. Never
  // accept a caller's provider rows, grant flags, identity or epoch claims.
  const auto& binding=credential.binding();
  const auto source=credential.credential_.binding();
  const auto operation=credential.writer_operation_uuid();
  if(!source||!operation||source->database!=binding.database_uuid||
      source->principal!=binding.service_principal_uuid||source->provider!=binding.provider_uuid||
      !binding.credential_reference_uuid||source->credential_reference!=*binding.credential_reference_uuid)
    return {E::principal_mismatch};
  std::optional<std::string_view> verifier;
  for(const auto& row:credential.security_records())if(row.object_uuid==binding.service_principal_uuid)
    verifier=row.record.Attribute("credential_fingerprint");
  if(!verifier)return {E::credential_rejected};
  api::NativeLocalServiceAuthenticationObservation identity;
  identity.database_uuid=binding.database_uuid;identity.principal_uuid=binding.service_principal_uuid;
  identity.provider_uuid=binding.provider_uuid;identity.provider_version_uuid=credential.provider_version_uuid();
  identity.binding_version_uuid=credential.binding_version_uuid();
  identity.policy_uuid=binding.policy_uuid;identity.policy_version_uuid=selected.declarations.posture_version_uuid;
  identity.selection_version_uuid=selected.provider_selection_version_uuid;
  identity.operation_uuid=*operation;identity.credential_reference_uuid=*binding.credential_reference_uuid;
  identity.provider_generation=credential.provider().generation;identity.credential_source_generation=source->generation;
  api::NativeLocalServiceAuthenticationResult out;
  std::optional<api::NativeLocalServiceAuthentication> authenticated;
  const auto crypto=owner.database_crypto_.WithMemoryScope(owner.crypto_binding_,[&]{
    out=api::NativeLocalServiceAuthentication::Authenticate({binding,credential.principal(),credential.provider(),
        selected.declarations.posture,selected.provider_selection,read.authorization_.projection,identity,
        *verifier,credential.credential_.credential(),derived,request.credential.stop,request.source.stop},authenticated);
  });
  if(!crypto.ok())return {E::crypto_scope_unavailable,api::LocalPasswordCredentialError::none,crypto.adapter_error};
  if(out.ok()){
    if(!authenticated)return {E::provider_failure};
    credential.credential_.EraseMaterial();
    if(request.credential.stop.stop_requested()||request.source.stop.stop_requested())return {E::cancelled};
    read.authentication_.emplace(std::move(*authenticated));
  }
  return out;
}
} // namespace scratchbird::server

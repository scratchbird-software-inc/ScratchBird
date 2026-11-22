// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_hosted_database_runtime.hpp"

#include "engine/internal_api/security/authentication_api.hpp"
#include "engine/internal_api/security/local_password_credential.hpp"
#include "catalog_authorization_policy.hpp"
#include "catalog_identifier_profile.hpp"
#include "native_catalog_visibility_lease_reader.hpp"
#include "native_catalog_visibility_stream_reader.hpp"
#include <set>

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

NativeHostedRuntimeOpenResult NativeHostedDatabaseRuntime::OpenAdmissionHeldByCaller(
    ServerProcessCryptoOwner& process,
    const DatabaseOwnershipRequest& request, NativeHostedRuntimeLimits limits,
    std::vector<std::unique_ptr<storage::disk::FileDevice>>& secondary,
    std::shared_ptr<DatabaseOwnershipLock> held_ownership) noexcept {
  if (!held_ownership) {
    NativeHostedRuntimeOpenResult out;
    out.error=NativeHostedRuntimeError::ownership_failure;
    return out;
  }
  return OpenImpl(process,request,limits,secondary,std::nullopt,
                  std::move(held_ownership),NativeBootstrapLineagePolicy::native_only);
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
      // SEARCH_KEY: SB_NATIVE_DATA_FILE_OWNERSHIP (slice step S1, owner
      // 2026-11-13). This runtime is the native owner, so it states the native
      // authority itself: the exclusive flock on the retained data-file
      // descriptor, no .sb.route.owner.lock / .sb.owner.lock and no descriptor
      // text (Q94). The held branch above adopts the caller's (legacy server)
      // lock unchanged until the production cut (Q93).
      auto native_command=command;
      native_command.ownership=storage::disk::DatabaseFileOwnership::native_data_file;
      out.ownership=AcquireDatabaseOwnership(native_command);
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

NativeHostedDatabaseCustody::OperationResult NativeHostedDatabaseRuntime::TryBeginWriterOperation(
    std::shared_ptr<NativeHostedDatabaseRuntime> runtime, const Uuid& operation_identity) noexcept {
  NativeHostedDatabaseCustody::OperationResult out;
  if (!runtime || !runtime->SameOwner() || runtime->closed_ || runtime->writer_retired_ || !runtime->custody_)
    return out;
  return NativeHostedDatabaseCustody::TryBeginOperation(runtime->custody_, operation_identity);
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
      request.fixed_root_limits.maximum_backing_bytes>owner->limits_.writer.source.maximum_backing_bytes ||
      request.fixed_root_limits.maximum_verification_image_bytes>owner->limits_.writer.source.maximum_verification_image_bytes)
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

// SEARCH_KEY: SB_NATIVE_SERVICE_IDENTITY_DISCOVERY (thin-slice S3b).
NativeServiceIdentityResult NativeHostedDatabaseRuntime::ReadStartupServiceIdentity(
    std::shared_ptr<NativeHostedDatabaseRuntime> owner,
    storage::database::NativeFixedRootMemoryLimits limits) noexcept {
  using E=NativeServiceIdentityError;namespace api=engine::internal_api;namespace db=storage::database;
  namespace c=core::catalog;
  NativeServiceIdentityResult out;
  if (!owner) return out;
  if (!owner->SameOwner()) {out.error=E::wrong_owner;return out;}
  auto& self=*owner;
  if (self.entering_||self.read_active_) {out.error=E::busy;return out;}
  if (self.closed_||self.writer_retired_) {out.error=E::closed;return out;}
  if (!self.writer_adopted_||!limits.maximum_backing_bytes||!limits.maximum_verification_image_bytes||
      limits.maximum_backing_bytes>self.limits_.writer.source.maximum_backing_bytes||
      limits.maximum_verification_image_bytes>self.limits_.writer.source.maximum_verification_image_bytes) {
    out.error=E::invalid_request;return out;
  }
  try {
    self.entering_=true;
    struct Leave {bool& flag;~Leave(){flag=false;}} leave{self.entering_};
    if(!self.reading_.Matches(self.reading_.binding())) {
      auto allocation=self.Acquire(self.limits_.writer.source.maximum_backing_bytes,NativeBootstrapMemoryKind::storage);
      if(!allocation.ok()){out.error=E::memory_failure;return out;}
      auto memory=storage::database::AdoptNativeStorageMemory(StorageBinding(*allocation.admission.resource),allocation.admission.resource);
      if(!memory.ok()){out.error=E::memory_failure;return out;}
      self.reading_=std::move(memory.memory);
    }
    // The two governed readers are prepared before any device guard exists and
    // retire (reverse order) after the lease and operation below.
    auto preparation=PrepareNativeRuntimePrincipalCredential(self.limits_.catalog,self.catalog_,self.catalog_.binding());
    if (!preparation.ok()) {out.error=E::preparation_failure;return out;}
    const auto id=core::uuid::IssueRuntimeIdentityV7();
    if (!id) {out.error=E::custody_failure;return out;}
    auto operation=NativeHostedDatabaseCustody::TryBeginOperation(self.custody_,*id);
    out.custody_error=operation.error;
    if (!operation.ok()) {out.error=E::custody_failure;return out;}
    const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&] {
      auto source=operation.operation.ReadFixedRootWithMemory(limits,self.reading_,self.reading_.binding());
      if (!source.ok()) {out.error=E::custody_failure;return;}
      const auto& lease=source.lease.source();
      // 1. The one committed, active, database-local runtime authority binding.
      const auto rows=db::NativeCatalogVisibilityLeaseReader::ReadCommittedSelected(lease,2,5,
          limits.maximum_verification_image_bytes,preparation.workspace.provider);
      if (!rows.ok()) {out.source_error=rows.error;out.error=E::configuration_source_failure;source.lease.Reset();return;}
      const db::NativeCatalogVisibleRow* found=nullptr;
      for (const auto& row:rows.rows) {
        if (row.metadata.record.header.deleted||!c::CatalogRuntimeAuthorityBindingMatchesMetadata(row.metadata)) continue;
        if (found) {out.error=E::binding_ambiguous;source.lease.Reset();return;}
        found=&row;
      }
      if (!found) {out.error=E::binding_absent;source.lease.Reset();return;}
      if (found->provisional||found->effective_lifecycle!=c::CatalogObjectLifecycle::active||
          found->effective_status!=c::CatalogObjectStatus::active) {out.error=E::binding_not_active;source.lease.Reset();return;}
      const auto decoded=c::DecodeCatalogRuntimeAuthorityBinding(found->metadata.record.payload);
      if (!decoded.ok()||decoded.record->database_uuid!=self.database_||
          decoded.record->binding_uuid!=found->metadata.record.header.object_uuid.value) {
        out.error=E::binding_invalid;source.lease.Reset();return;
      }
      if (decoded.record->authority_mode!=c::RuntimeAuthorityMode::database_local) {
        out.error=E::external_authority_required;source.lease.Reset();return;
      }
      if (!decoded.record->credential_reference_uuid) {out.error=E::credential_reference_absent;source.lease.Reset();return;}
      NativeServiceIdentity identity;
      identity.database_uuid=self.database_;
      identity.runtime_binding_uuid=decoded.record->binding_uuid;
      identity.runtime_binding_generation=decoded.record->generation;
      identity.service_principal_uuid=decoded.record->service_principal_uuid;
      identity.provider_uuid=decoded.record->provider_uuid;
      identity.credential_reference_uuid=*decoded.record->credential_reference_uuid;
      // 2. The principal's committed security row and its stored seed.
      api::NativeRuntimeSecurityReadRequest request;
      request.database_uuid=self.database_;request.runtime_binding_uuid=identity.runtime_binding_uuid;
      request.runtime_binding_generation=identity.runtime_binding_generation;
      request.maximum_retained_image_bytes=limits.maximum_verification_image_bytes;
      request.catalog_selection=api::NativeRuntimeCatalogSelection::discover_selected;
      const auto observed=api::ReadCommittedNativeRuntimeSecurityOnLease(lease,request,preparation.workspace.security);
      using R=api::NativeRuntimeSecurityReadError;
      if (!observed.ok()) {
        out.security_error=observed.error;out.source_error=observed.source_error;
        out.error=observed.error==R::principal_absent?E::principal_absent
            :observed.error==R::principal_wrong_family?E::principal_wrong_family
            :observed.error==R::principal_not_active?E::principal_not_active
            :observed.error==R::principal_not_service?E::principal_not_service
            :observed.error==R::external_authority_required?E::external_authority_required
            :E::security_source_failure;
        source.lease.Reset();return;
      }
      const c::NativeSecurityPrincipal* principal=nullptr;
      for (const auto& record:observed.records)
        if (record.object_uuid==identity.service_principal_uuid) principal=record.record.principal();
      if (!principal) {out.error=E::principal_absent;source.lease.Reset();return;}
      if (!principal->credential) {out.error=E::principal_credential_absent;source.lease.Reset();return;}
      if (!principal->password_seed) {out.error=E::seed_absent;source.lease.Reset();return;}
      identity.password_seed=*principal->password_seed;
      source.lease.Reset();
      out.identity.emplace(identity);
      out.error=E::none;
    });
    if (!crypto.ok()) {out.identity.reset();out.error=E::crypto_failure;}
    return out;
  } catch (const std::bad_alloc&) {out.identity.reset();out.error=E::memory_failure;return out;}
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
  if(read.anchored())return E::none;
  auto& self=*read.owner_;
  namespace db=storage::database;
  // Page zero's identity, read again under the retained operation, must be
  // the one the credential comparison read; it then anchors every later read
  // of this runtime. A read only: no watermark, repair, provider activation,
  // session or agent BEGIN.
  const auto& limits=read.inputs_->request.fixed_root_limits;
  auto& lease=*credentials.comparison->lease;
  const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{
    auto source=lease.retained_operation_.ReadFixedRootWithMemory(
        {limits.maximum_verification_image_bytes,limits.maximum_backing_bytes},self.reading_,self.reading_.binding());
    if(!source.ok())return;
    const auto identity=source.lease.source().identity();
    source.lease.Reset();
    if(identity.database_uuid!=self.database_||identity.filespace_uuid!=self.primary_||
        !db::SameReadSource(db::NativeReadSourceIdentityFromFixedRoot(identity),lease.source_identity()))return;
    read.page_zero_anchor_=identity;
  });
  return !crypto.ok()?E::crypto_failure:read.page_zero_anchor_?E::none:E::custody_failure;
}

NativeHostedRuntimeError NativeHostedDatabaseRuntime::ReadStartupCatalogs(CredentialRead& read,
    storage::database::NativeFixedRootMemoryLimits limits,
    const storage::database::NativeCatalogRelationMemoryLimits& requested_catalog_limits,
    storage::database::NativeCatalogCaptureLimits capture_limits) noexcept {
  namespace db=storage::database;using E=NativeHostedRuntimeError;
  const auto catalog_limits=requested_catalog_limits;
  read.CheckOwner();
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return E::busy;
  if(!read.ok()||!read.owner_)return E::closed;
  auto& credential=*read.credentials_->comparison->lease;
  if(credential.has_selected_source()||!credential.has_retained_operation()||!read.anchored())return E::custody_failure;
  if(!read.authentication_)return E::credential_failure;
  const auto& request=read.inputs_->request;
  if(!limits.maximum_backing_bytes||!limits.maximum_verification_image_bytes||
      limits.maximum_backing_bytes>request.fixed_root_limits.maximum_backing_bytes||
      limits.maximum_verification_image_bytes>request.fixed_root_limits.maximum_verification_image_bytes)return E::invalid_request;
  auto& self=*read.owner_;read.mutating_=true;self.entering_=true;
  struct Leave {bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}} leave{read.mutating_,self.entering_};
  const auto cancelled=[&]{return request.credential.stop.stop_requested()||request.source.stop.stop_requested();};
  if(cancelled())return E::cancelled;
  const auto workspace=db::NativeCatalogCohortWorkspaceBytes(catalog_limits);
  if(!workspace||workspace>SIZE_MAX-limits.maximum_backing_bytes||catalog_limits.maximum_devices<self.close_receipts_.size())return E::invalid_request;
  const bool capture_requested=capture_limits.maximum_rows||capture_limits.maximum_text_bytes||capture_limits.maximum_filespaces||
      capture_limits.maximum_transactions||capture_limits.maximum_physical_pages||capture_limits.maximum_initial_control_pages||
      capture_limits.maximum_name_tree_cells;
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
    // The same page zero the startup observation retained.
    auto source=credential.retained_operation_.ReadFixedRootWithMemory(
        {limits.maximum_verification_image_bytes,limits.maximum_backing_bytes},adopted.memory,adopted.memory.binding());
    if(!source.ok())return;
    if(source.lease.operation_uuid()!=read.authentication_->observation_.operation_uuid||
        source.lease.source().identity()!=*read.page_zero_anchor_){
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
  if(credential.has_selected_source()||!credential.has_retained_operation()||!read.anchored())return E::custody_failure;
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
  if(credential.has_selected_source()||!credential.has_retained_operation()||!read.anchored())return E::custody_failure;
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
  if(credential.has_selected_source()||!credential.has_retained_operation()||!read.anchored())return E::custody_failure;
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
  // SB_CATALOG_NATIVE_AGENT_RECORDS (3c-3): the coordinator is selected by its
  // constant; its selection and verification class come from its typed policy
  // and config records.
  const NativeStartupAgentDefinition* selected=nullptr;
  for(const auto& entry:read.agents_.entries()){
    if(!entry.is_policy()||entry.agent_type!=c::CatalogAgentType::agent_coordinator)continue;
    if(selected)return E::agent_policy_failure;
    selected=&entry;
  }
  if(!selected)return E::verification_policy_failure;
  const auto policy=selected->Policy();
  const auto config=selected->Config<c::NativeAgentConfig_agent_coordinator_policy>();
  if(!policy.ok()||!config.ok())return E::agent_policy_failure;
  if(!policy.record->startup_verification)return E::verification_policy_failure;
  const auto& v=*policy.record->startup_verification;
  ia::NativeVerificationPolicyRequest request{v.repair_policy_uuid,v.quarantine_policy_uuid,v.salvage_policy_uuid,v.evidence_policy_uuid};
  request.verification_levels=static_cast<core::platform::u32>(v.verification_levels);
  using Class=c::NativeAgentConfigEnum_agent_coordinator_policy_startup_verification_class;
  const auto klass=config.record->startup_verification_class;
  if(klass==Class::startup_full_verify)request.verification_class=ia::NativeVerificationClass::startup_full;
  else if(klass==Class::startup_light_verify)request.verification_class=ia::NativeVerificationClass::startup_light;
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
  if(credential.has_selected_source()||!credential.has_retained_operation()||!read.anchored())return E::custody_failure;
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
  if(!credential.has_retained_operation()||!read.anchored()||!read.agent_policies_.ok())return {E::not_ready};
  if(!read.authorization_.projection.Allows(core::catalog::CatalogSecurityRight::CONNECT,read.owner_->database_))return {E::authorization_refused};
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
  if(!credential.has_retained_operation()||!read.anchored()||!read.agent_policies_.ok())return {E::not_ready};
  if(!read.authorization_.projection.Allows(core::catalog::CatalogSecurityRight::CONNECT,read.owner_->database_))return {E::authorization_refused};
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
  if(credential.has_selected_source()||!credential.has_retained_operation()||!read.anchored()||
      !read.catalog_capture_.ok()||!read.catalog_metadata_.ok())return {E::not_ready};
  const auto policies=ResolveConfiguredStartupVerificationPolicies(read);
  if(policies!=NativeHostedRuntimeError::none){
    if(policies==NativeHostedRuntimeError::cancelled){read.session_.reset();return {E::cancelled};}
    return {E::policy_required};
  }
  if(!storage::database::SameReadSource(*read.catalog_capture_.source(),
      storage::database::NativeReadSourceIdentityFromFixedRoot(*read.page_zero_anchor_)))
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
  for(const auto& entry:read.agents_.entries())if(entry.is_policy()&&entry.agent_type==core::catalog::CatalogAgentType::agent_coordinator){
    const auto policy=entry.Policy();
    const auto config=entry.Config<core::catalog::NativeAgentConfig_agent_coordinator_policy>();
    if(policy.ok()&&config.ok())clock=core::agents::SelectAgentStartupClockPolicy(*policy.record,*config.record,entry.version_uuid);
  }
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
  if(!credential.has_retained_operation()||!read.anchored()||!read.agent_policies_.ok())return {E::not_ready};
  if(!read.authorization_.projection.Allows(core::catalog::CatalogSecurityRight::CONNECT,read.owner_->database_))return {E::authorization_refused};
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
  // 3a-2: the typed principal's binary verifier parts (no text fingerprint).
  std::optional<api::LocalPasswordStoredVerifier> verifier;
  for(const auto& row:credential.security_records())if(row.object_uuid==binding.service_principal_uuid)
    if(const auto* principal=row.record.principal();principal&&principal->credential)
      verifier=api::LocalPasswordStoredVerifier{principal->credential->iterations,
          std::span<const unsigned char,16>(principal->credential->salt),
          std::span<const unsigned char,32>(principal->credential->verifier)};
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

// SEARCH_KEY: SB_NATIVE_LOGIN_MATERIAL (thin-slice S4a). See the declaration and NativeLoginMaterialError.
// SB_NATIVE_LOGIN_SOURCE (S4c): NativeLoginMaterialErrorName / NativeLoginMaterialErrorCode moved, inline, to
// engine/internal_api/native_login_material.hpp with the enum.

namespace {
// What phase A (the file read) copies out before the lease and the custody operation end. Owning values only.
struct NativeLoginSnapshot {
  bool found = false, family_ok = false, active = false, had_credential = false;
  core::catalog::NativeSecurityPrincipal principal;  // meaningful when family_ok
  core::platform::Uuid version_uuid;
  core::platform::u64 definition_generation = 0;
  NativeLoginMaterial material;                      // filled for an active principal
};
// A stand-in verifier for the paths with no usable real one. Public, fixed, non-secret bytes (never all zero).
// Its iteration count is the minimum a stored verifier may carry, which is what the create writes.
constexpr core::platform::u64 kDummyIterations = core::catalog::kNativeSecurityCredentialMinIterations;
constexpr std::array<unsigned char,16> kDummySalt{0x53,0x42,0x4c,0x4f,0x47,0x49,0x4e,0x44,0x55,0x4d,0x4d,0x59,0x53,0x41,0x4c,0x54};
constexpr std::array<unsigned char,32> kDummyVerifier{0x53,0x42,0x4c,0x4f,0x47,0x49,0x4e,0x44,0x55,0x4d,0x4d,0x59,0x56,0x45,0x52,0x46,
                                                      0x53,0x42,0x4c,0x4f,0x47,0x49,0x4e,0x44,0x55,0x4d,0x4d,0x59,0x56,0x45,0x52,0x46};
}  // namespace

NativeLoginMaterialResult NativeHostedDatabaseRuntime::ReadNativeLoginMaterial(
    std::shared_ptr<NativeHostedDatabaseRuntime> owner, const NativeLoginMaterialRequest& request,
    const core::memory::ScopedProtectedBuffer& password, core::platform::usize password_length,
    const core::memory::ScopedProtectedBuffer& derived, const transaction::mga::VisibilitySnapshot* view) noexcept {
  using E=NativeLoginMaterialError;namespace api=engine::internal_api;namespace db=storage::database;
  namespace c=core::catalog;
  NativeLoginMaterialResult out;
  // The password and the workspace are CONSUMED: zeroed on every exit, whatever was decided.
  struct Consume {
    const core::memory::ScopedProtectedBuffer& a;const core::memory::ScopedProtectedBuffer& b;
    ~Consume() {
      if(a.valid())OPENSSL_cleanse(a.data(),a.size());
      if(b.valid())OPENSSL_cleanse(b.data(),b.size());
    }
  } consume{password,derived};
  try {
    if(!owner||!core::uuid::IsEngineIdentityUuid(request.principal_uuid)||
        !core::uuid::IsEngineIdentityUuid(request.runtime_binding_uuid)||!request.runtime_binding_generation||
        !request.limits.maximum_backing_bytes||!request.limits.maximum_verification_image_bytes||
        !password.valid()||password_length>password.size()) {out.error=E::invalid_request;return out;}
    // A password that cannot be a local password is decided from the input alone, before the file is read.
    {
      const auto* bytes=static_cast<const unsigned char*>(password.data());
      bool admissible=password_length>=1&&password_length<=1024;
      for(core::platform::usize i=0;admissible&&i<password_length;++i)if(bytes[i]==0)admissible=false;
      if(!admissible) {out.error=E::password_inadmissible;return out;}
    }
    auto& self=*owner;
    if(!self.SameOwner()) {out.error=E::wrong_owner;return out;}
    if(!derived.valid()||derived.size()!=32||!derived.evidence().platform_lock_succeeded||
        !derived.evidence().no_dump_succeeded||
        derived.tag().binary_ownership[core::memory::MemoryBinaryScopeKind::database]!=self.database_.bytes||
        !password.evidence().platform_lock_succeeded||!password.evidence().no_dump_succeeded) {
      out.error=E::protected_workspace_required;return out;
    }
    if(self.entering_||self.read_active_) {out.error=E::busy;return out;}
    if(self.closed_||self.writer_retired_) {out.error=E::closed;return out;}
    if(!self.writer_adopted_||
        request.limits.maximum_backing_bytes>self.limits_.writer.source.maximum_backing_bytes||
        request.limits.maximum_verification_image_bytes>self.limits_.writer.source.maximum_verification_image_bytes) {
      out.error=E::invalid_request;return out;
    }
    self.entering_=true;
    struct Leave {bool& flag;~Leave(){flag=false;}} leave{self.entering_};

    // ---- phase A: the committed security rows, read-only, no lock; everything needed is copied out, then the
    //      lease and the custody operation end BEFORE the (long) PBKDF2 below. ----
    NativeLoginSnapshot snapshot;
    E phase_a=E::none;
    {
      if(!self.reading_.Matches(self.reading_.binding())) {
        auto allocation=self.Acquire(self.limits_.writer.source.maximum_backing_bytes,NativeBootstrapMemoryKind::storage);
        if(!allocation.ok()) {out.error=E::memory_failure;return out;}
        auto memory=storage::database::AdoptNativeStorageMemory(StorageBinding(*allocation.admission.resource),allocation.admission.resource);
        if(!memory.ok()) {out.error=E::memory_failure;return out;}
        self.reading_=std::move(memory.memory);
      }
      auto preparation=PrepareNativeRuntimePrincipalCredential(self.limits_.catalog,self.catalog_,self.catalog_.binding());
      if(!preparation.ok()) {out.error=E::preparation_failure;return out;}
      const auto id=core::uuid::IssueRuntimeIdentityV7();
      if(!id) {out.error=E::custody_failure;return out;}
      auto operation=NativeHostedDatabaseCustody::TryBeginOperation(self.custody_,*id);
      out.custody_error=operation.error;
      if(!operation.ok()) {out.error=E::custody_failure;return out;}
      const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&] {
        auto source=operation.operation.ReadFixedRootWithMemory(request.limits,self.reading_,self.reading_.binding());
        if(!source.ok()) {phase_a=E::custody_failure;return;}
        const auto& lease=source.lease.source();
        api::NativeRuntimeSecurityReadRequest read;
        read.database_uuid=self.database_;read.runtime_binding_uuid=request.runtime_binding_uuid;
        read.runtime_binding_generation=request.runtime_binding_generation;
        read.maximum_retained_image_bytes=request.limits.maximum_verification_image_bytes;
        read.catalog_selection=api::NativeRuntimeCatalogSelection::discover_selected;
        const auto observed=api::ReadCommittedNativeRuntimeSecurityOnLease(lease,read,preparation.workspace.security,view);
        using R=api::NativeRuntimeSecurityReadError;
        if(!observed.ok()) {
          out.security_error=observed.error;out.source_error=observed.source_error;
          // The reader judges the BINDING's principal (L5) as well; those verdicts are about L5, never about the
          // principal being logged in, so they are reported as a binding failure, not as principal_absent etc.
          switch(observed.error) {
            case R::backing_unavailable: case R::resource_exhausted: phase_a=E::memory_failure;break;
            case R::binding_source_failure: case R::binding_absent: case R::binding_ambiguous: case R::binding_invalid:
            case R::binding_not_active: case R::binding_generation_mismatch: case R::external_authority_required:
            case R::binding_catalog_generation_mismatch: case R::binding_security_epoch_mismatch:
            case R::principal_absent: case R::principal_wrong_family: case R::principal_not_active:
            case R::principal_not_service: case R::principal_generation_invalid:
              phase_a=E::security_binding_failure;break;
            default: phase_a=E::security_source_failure;break;
          }
          source.lease.Reset();return;
        }
        // Everything below is copied into `snapshot`; nothing borrowed from the lease leaves this block.
        for(const auto& record:observed.records) {
          if(record.object_uuid!=request.principal_uuid) continue;
          snapshot.found=true;
          const auto* principal=record.record.principal();
          if(!principal) break;
          snapshot.family_ok=true;snapshot.principal=*principal;snapshot.version_uuid=record.version_uuid;
          snapshot.had_credential=principal->credential.has_value();
          snapshot.definition_generation=record.definition_generation;
          snapshot.active=!record.deleted&&record.lifecycle==c::CatalogObjectLifecycle::active&&
              record.status==c::CatalogObjectStatus::active&&principal->active;
          auto& m=snapshot.material;
          m.database_uuid=self.database_;m.principal_uuid=record.object_uuid;m.principal_version_uuid=record.version_uuid;
          m.principal_kind=principal->principal_kind;m.authority_class=principal->authority_class;
          m.bootstrap_principal=principal->bootstrap_principal;m.definition_generation=record.definition_generation;
          m.security_context_authority_version=principal->security_context_authority_version;
          m.security_context_generation=principal->security_context_generation;
          m.security_generation=principal->security_generation;m.policy_generation=principal->policy_generation;
          if(observed.selected_catalog) {
            m.catalog_generation=observed.selected_catalog->catalog_generation;
            m.schema_epoch=observed.selected_catalog->schema_epoch;m.security_epoch=observed.selected_catalog->security_epoch;
          }
          break;
        }
        if(snapshot.found&&snapshot.family_ok&&snapshot.active) {
          // Direct grant-family rows of the principal (observed records are sorted by object UUID: edge order is
          // that order). Retired, dropped and not-active rows are not edges. S4e Part 1 (owner S4c Q1 = extend,
          // 2026-11-21): the effective role / group sets are the transitive closure of the principal's allow-effect
          // membership edges through ACTIVE role / group rows, and the membership and privilege rows of those
          // roles / groups are edges too, so privileges granted to a role reach the login (CONNECT at attach).
          const auto row_live=[&](const auto& rec) {
            return !rec.deleted&&rec.lifecycle==c::CatalogObjectLifecycle::active&&rec.status==c::CatalogObjectStatus::active&&
                rec.retired_transaction_uuid.is_nil();
          };
          std::set<core::platform::Uuid> live_roles,live_groups,closure;
          for(const auto& record:observed.records) {
            if(!row_live(record)) continue;
            if(const auto* role=record.record.role()) {if(role->active)live_roles.insert(record.object_uuid);}
            else if(record.record.group()) live_groups.insert(record.object_uuid);  // a group row has no active flag: the row lifecycle decides
          }
          closure.insert(request.principal_uuid);
          for(bool grew=true;grew;) {
            grew=false;
            for(const auto& record:observed.records) {
              const auto* grant=record.record.grant();
              if(!grant||!row_live(record)) continue;
              const bool membership=grant->grant_class==c::CatalogSecurityGrantClass::role_membership||
                  grant->grant_class==c::CatalogSecurityGrantClass::group_membership;
              if(!membership||!grant->member_uuid||!grant->parent_uuid||grant->effect==c::CatalogSecurityEffect::deny) continue;
              if(!closure.contains(*grant->member_uuid)) continue;
              // The member's kind must match what the closure holds (the principal, or a role / group of the set).
              const auto member_kind=grant->member_kind.value_or(c::CatalogSecuritySubjectKind::none);
              if(*grant->member_uuid==request.principal_uuid?member_kind!=c::CatalogSecuritySubjectKind::principal:
                  (member_kind==c::CatalogSecuritySubjectKind::role?!live_roles.contains(*grant->member_uuid):
                   member_kind==c::CatalogSecuritySubjectKind::group?!live_groups.contains(*grant->member_uuid):true)) continue;
              const auto parent_kind=grant->parent_kind.value_or(c::CatalogSecuritySubjectKind::none);
              const bool parent_live=(parent_kind==c::CatalogSecuritySubjectKind::role&&live_roles.contains(*grant->parent_uuid))||
                  (parent_kind==c::CatalogSecuritySubjectKind::group&&live_groups.contains(*grant->parent_uuid));
              if(!parent_live||closure.contains(*grant->parent_uuid)) continue;
              closure.insert(*grant->parent_uuid);grew=true;
            }
          }
          for(const auto& record:observed.records) {   // the engine-owned SYSARCH role, verified as the native projection verifies it
            const auto* role=record.record.role();
            if(role&&row_live(record)&&record.object_uuid==storage::database::kCanonicalSysarchRoleIdentity&&closure.contains(record.object_uuid)&&
                role->active&&role->engine_owned&&role->immutable&&role->authority_class==c::CatalogSecurityAuthorityClass::engine_owned_sysarch)
              snapshot.material.sysarch_role_uuid=record.object_uuid;
          }
          for(const auto& id:closure) {
            if(live_roles.contains(id))snapshot.material.effective_role_uuids.push_back(id);
            else if(live_groups.contains(id))snapshot.material.effective_group_uuids.push_back(id);
          }
          for(const auto& record:observed.records) {
            const auto* grant=record.record.grant();
            if(!grant||record.deleted||record.lifecycle!=c::CatalogObjectLifecycle::active||
                record.status!=c::CatalogObjectStatus::active||!record.retired_transaction_uuid.is_nil()) continue;
            const bool names_principal=(grant->subject_uuid&&*grant->subject_uuid==request.principal_uuid)||
                (grant->member_uuid&&*grant->member_uuid==request.principal_uuid)||
                (grant->principal_uuid&&*grant->principal_uuid==request.principal_uuid);
            const bool membership_class=grant->grant_class==c::CatalogSecurityGrantClass::role_membership||
                grant->grant_class==c::CatalogSecurityGrantClass::group_membership;
            // A role / group of the effective sets: its own membership rows (to a parent that is in the sets) and
            // its privilege rows.
            const bool of_effective_set=
                (membership_class&&grant->member_uuid&&closure.contains(*grant->member_uuid)&&grant->parent_uuid&&
                    closure.contains(*grant->parent_uuid))||
                (grant->grant_class==c::CatalogSecurityGrantClass::privilege&&grant->subject_uuid&&
                    closure.contains(*grant->subject_uuid));
            if(!names_principal&&!of_effective_set) continue;
            NativeLoginGrantEdge edge;
            edge.grant_uuid=record.object_uuid;edge.grant_class=grant->grant_class;
            if(grant->subject_uuid)edge.subject_uuid=*grant->subject_uuid;
            if(grant->member_uuid)edge.member_uuid=*grant->member_uuid;
            if(grant->parent_uuid)edge.parent_uuid=*grant->parent_uuid;
            if(grant->target_uuid)edge.target_uuid=*grant->target_uuid;
            if(grant->subject_kind)edge.subject_kind=*grant->subject_kind;
            if(grant->member_kind)edge.member_kind=*grant->member_kind;
            if(grant->parent_kind)edge.parent_kind=*grant->parent_kind;
            edge.effect=grant->effect;edge.right=grant->right;edge.security_generation=grant->security_generation;
            snapshot.material.edges.push_back(edge);
            const bool membership=grant->grant_class==c::CatalogSecurityGrantClass::role_membership||
                grant->grant_class==c::CatalogSecurityGrantClass::group_membership;
            if(membership&&grant->member_uuid&&*grant->member_uuid==request.principal_uuid&&
                grant->member_kind==c::CatalogSecuritySubjectKind::principal&&grant->parent_uuid&&
                grant->effect!=c::CatalogSecurityEffect::deny) {
              // S4e Part 1: a direct membership counts only when the role / group row is itself active.
              if(grant->parent_kind==c::CatalogSecuritySubjectKind::role&&live_roles.contains(*grant->parent_uuid))
                snapshot.material.role_uuids.push_back(*grant->parent_uuid);
              if(grant->parent_kind==c::CatalogSecuritySubjectKind::group&&live_groups.contains(*grant->parent_uuid))
                snapshot.material.group_uuids.push_back(*grant->parent_uuid);
            }
          }
        }
        source.lease.Reset();
      });
      if(!crypto.ok()) {out.error=E::crypto_failure;return out;}
      if(phase_a!=E::none) {out.error=phase_a;return out;}
    }

    // ---- phase B: exactly ONE PBKDF2 on every path from here. The real verifier when the principal has a
    //      usable one; otherwise a fixed dummy verifier (public, non-secret bytes) at the minimum iteration
    //      count, so that an absent, inactive, wrong-family, credential-less or unsupported principal costs the
    //      same KDF as a wrong password. The refusals below are decided AFTER it and never from its result
    //      except the one verdict that is about the password; an agent / service principal is refused
    //      identically whether or not its password matched (no oracle for a derived password). Unavoidable
    //      remaining differences (the row read is identical for all; the decision itself is a few compares) are
    //      not made worse by the code. ----
    const bool usable=snapshot.found&&snapshot.family_ok&&snapshot.active&&snapshot.principal.credential&&
        snapshot.principal.credential->scheme==c::CatalogSecurityCredentialScheme::pbkdf2_sha256_v1;
    std::array<unsigned char,16> salt=kDummySalt;
    std::array<unsigned char,32> verifier=kDummyVerifier;
    core::platform::u64 iterations=kDummyIterations;
    if(usable) {
      const auto& credential=*snapshot.principal.credential;
      for(std::size_t i=0;i<salt.size();++i)salt[i]=static_cast<unsigned char>(credential.salt[i]);
      for(std::size_t i=0;i<verifier.size();++i)verifier[i]=static_cast<unsigned char>(credential.verifier[i]);
      iterations=credential.iterations;
    }
    snapshot.principal.credential.reset();  // the copy's verifier is not needed again
    struct Wipe {
      std::array<unsigned char,16>& salt;std::array<unsigned char,32>& verifier;
      ~Wipe() {OPENSSL_cleanse(salt.data(),salt.size());OPENSSL_cleanse(verifier.data(),verifier.size());}
    } wipe{salt,verifier};
    api::LocalPasswordCredentialError verdict=api::LocalPasswordCredentialError::invalid_verifier;
    bool scope_failed=false;
    const auto verify=[&](bool real) {
      const api::LocalPasswordStoredVerifier stored{real?iterations:kDummyIterations,
          std::span<const unsigned char,16>(real?salt:kDummySalt),std::span<const unsigned char,32>(real?verifier:kDummyVerifier)};
      return api::VerifyLocalPasswordCredential(stored,std::string_view(static_cast<const char*>(password.data()),password_length),
          std::span<std::byte,32>{static_cast<std::byte*>(derived.data()),32}).error;
    };
    const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&] {
      if(core::hash::CheckExplicitCryptoMemoryScope(self.database_)!=core::hash::CryptoMemoryError::none) {scope_failed=true;return;}
      verdict=verify(usable);
      // A real verifier the KDF refused before running (parameters it will not accept) spent no KDF: spend the
      // one on the dummy so the cost is the same. Its result is discarded.
      if(usable&&verdict==api::LocalPasswordCredentialError::invalid_verifier)(void)verify(false);
    });
    if(!crypto.ok()||scope_failed) {out.error=E::crypto_failure;return out;}
    // NOTE: `verdict` is copied to out.verifier_error ONLY on the paths below that are about the password or the
    // verifier itself. It is never reported with an agent / absent / inactive refusal (it would be an oracle).

    // ---- the verdict, in a fixed order. Nothing but the password comparison depends on the password. ----
    if(!snapshot.found) {out.error=E::principal_absent;return out;}
    if(!snapshot.family_ok) {out.error=E::principal_wrong_family;return out;}
    if(!snapshot.active) {out.error=E::principal_not_active;return out;}
    {
      // Q274: agents are refused on every external path, whether or not the password matched. The check is the
      // engine's own (CheckExternalAuthenticationBinding takes the kind as a name for the diagnostic mapping); a
      // principal that carries an agent password seed is an agent whatever its kind constant says (defensive).
      const char* kind_name=snapshot.principal.principal_kind==c::CatalogSecurityPrincipalKind::service?"service":
          snapshot.principal.principal_kind==c::CatalogSecurityPrincipalKind::user?"user":"unknown";
      if(api::CheckExternalAuthenticationBinding(kind_name).error||snapshot.principal.password_seed) {
        if(snapshot.principal.password_seed)OPENSSL_cleanse(snapshot.principal.password_seed->data(),16);
        out.error=E::external_binding_refused;return out;
      }
    }
    if(!usable) {
      // The row has no verifier at all, or one of a scheme this build does not verify.
      out.error=snapshot.had_credential?E::credential_unsupported:E::credential_absent;return out;
    }
    if(verdict==api::LocalPasswordCredentialError::invalid_verifier||verdict==api::LocalPasswordCredentialError::invalid_workspace) {
      out.verifier_error=verdict;
      out.error=verdict==api::LocalPasswordCredentialError::invalid_workspace?E::protected_workspace_required:E::credential_unsupported;
      return out;
    }
    out.verifier_error=verdict;
    if(verdict==api::LocalPasswordCredentialError::invalid_credential) {out.error=E::password_inadmissible;return out;}
    if(verdict==api::LocalPasswordCredentialError::provider_failure) {out.error=E::verifier_provider_failure;return out;}
    if(verdict!=api::LocalPasswordCredentialError::none) {out.error=E::password_mismatch;return out;}
    const auto& m=snapshot.material;
    if(m.security_context_authority_version!=1||!m.security_context_generation||!m.security_generation||!m.policy_generation) {
      out.error=E::principal_generation_invalid;return out;
    }
    out.material.emplace(snapshot.material);
    out.error=E::none;
    return out;
  } catch(const std::bad_alloc&) {out.material.reset();out.error=E::memory_failure;return out;}
    catch(...) {out.material.reset();out.error=E::security_source_failure;return out;}
}
// SEARCH_KEY: SB_NATIVE_NAMES_SOURCE (S4b rework "Part A"). See the declaration.
namespace {
// A live, committed, unprovisional row of the visibility reader (the S4b liveness predicate, kept).
bool NativeNameRowLive(const storage::database::NativeCatalogVisibleRow& row) noexcept {
  namespace c=core::catalog;
  return !row.provisional&&!row.metadata.record.header.deleted&&
      row.effective_lifecycle==c::CatalogObjectLifecycle::active&&row.effective_status==c::CatalogObjectStatus::active;
}
}  // namespace

engine::internal_api::NativeNamesSnapshot NativeHostedDatabaseRuntime::ReadNativeNamesSnapshot(
    std::shared_ptr<NativeHostedDatabaseRuntime> owner, const NativeNamesSnapshotRequest& request,
    const transaction::mga::VisibilitySnapshot* view) noexcept {
  using E=engine::internal_api::NativeNamesSnapshotError;namespace db=storage::database;namespace c=core::catalog;
  engine::internal_api::NativeNamesSnapshot out;
  try {
    // SB_NATIVE_CATALOG_STREAM: the names are read by the streaming reader; the catalog limits supply the profile and device count.
    db::NativeCatalogStreamLimits stream_limits;
    stream_limits.maximum_profile=request.catalog.maximum_profile;stream_limits.maximum_devices=request.catalog.maximum_devices;
    stream_limits.window_pages=request.stream_window_pages;stream_limits.history_versions=request.stream_history_versions;
    if(!owner||!request.limits.maximum_backing_bytes||!request.limits.maximum_verification_image_bytes||
        !db::NativeCatalogStreamWorkspaceBytes(stream_limits)) {
      out.error=E::invalid_request;return out;
    }
    auto& self=*owner;
    if(!self.SameOwner()) {out.error=E::wrong_owner;return out;}
    if(self.entering_||self.read_active_) {out.error=E::busy;return out;}
    if(self.closed_||self.writer_retired_) {out.error=E::closed;return out;}
    if(!self.writer_adopted_||
        request.limits.maximum_backing_bytes>self.limits_.writer.source.maximum_backing_bytes||
        request.limits.maximum_verification_image_bytes>self.limits_.writer.source.maximum_verification_image_bytes) {
      out.error=E::invalid_request;return out;
    }
    self.entering_=true;
    struct Leave {bool& flag;~Leave(){flag=false;}} leave{self.entering_};

    // The stream workspace (a window of leaves, the tree cursor and the history registry: small, independent of the relation's
    // size) is acquired for this call and released at its end.
    auto workspace_allocation=self.Acquire(db::NativeCatalogStreamWorkspaceBytes(stream_limits),NativeBootstrapMemoryKind::storage);
    if(!workspace_allocation.ok()) {out.error=E::memory_failure;return out;}
    auto workspace_memory=db::AdoptNativeStorageMemory(StorageBinding(*workspace_allocation.admission.resource),
                                                       workspace_allocation.admission.resource);
    if(!workspace_memory.ok()) {out.error=E::memory_failure;return out;}
    auto prepared=db::PrepareNativeCatalogStreamRead(stream_limits,workspace_memory.memory,workspace_memory.memory.binding());
    if(!prepared.ok()) {out.error=E::preparation_failure;return out;}
    if(!self.reading_.Matches(self.reading_.binding())) {
      auto allocation=self.Acquire(self.limits_.writer.source.maximum_backing_bytes,NativeBootstrapMemoryKind::storage);
      if(!allocation.ok()) {out.error=E::memory_failure;return out;}
      auto memory=db::AdoptNativeStorageMemory(StorageBinding(*allocation.admission.resource),allocation.admission.resource);
      if(!memory.ok()) {out.error=E::memory_failure;return out;}
      self.reading_=std::move(memory.memory);
    }
    const auto id=core::uuid::IssueRuntimeIdentityV7();
    if(!id) {out.error=E::custody_failure;return out;}
    auto operation=NativeHostedDatabaseCustody::TryBeginOperation(self.custody_,*id);
    out.custody_error=static_cast<std::uint32_t>(operation.error);
    if(!operation.ok()) {out.error=E::custody_failure;return out;}

    // ---- the committed read, no lock, nothing written; everything is copied out before the lease ends ----
    // SB_NAME_SNAPSHOT_OWN_TXN: no transaction exists yet (S4e). `view` null: committed state at read time; non-null: the captured snapshot value.
    E phase=E::none;
    const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&] {
      auto source=operation.operation.ReadFixedRootWithMemory(request.limits,self.reading_,self.reading_.binding());
      if(!source.ok()) {phase=E::custody_failure;return;}
      const auto& lease=source.lease.source();
      // The refusal a stream's error is: the governed registry, an exhausted allocation, or a source failure with the detail.
      const auto refuse=[&](const db::NativeCatalogStreamResult& r,E source_phase) {
        out.source_error=static_cast<std::uint32_t>(r.error);out.relation_error=static_cast<std::uint32_t>(r.roots_error);
        phase=r.error==db::NativeCatalogStreamError::history_exhausted?E::names_history_exhausted:source_phase;
        source.lease.Reset();
      };
      // 1. The configuration relation (role 5): identifier profiles, and the security schema the principals live under.
      {
        const auto visit_config=[&](const db::NativeCatalogVisibleRow& row) {
          if(!NativeNameRowLive(row)) return true;
          const auto& m=row.metadata;
          if(c::IsCatalogIdentifierProfilePayload(m.record.payload)&&c::CatalogIdentifierProfileMatchesMetadata(m)&&
              c::CatalogIdentifierProfileMatchesDatabase(m,self.database_)) {
            auto decoded=c::DecodeCatalogIdentifierProfile(m.record.payload);
            if(decoded.ok()) out.profiles.push_back({decoded.record->profile_uuid,static_cast<std::uint64_t>(decoded.record->unquoted_fold_mode)});
          } else if(c::IsCatalogAuthorizationPolicyPayload(m.record.payload)&&
              c::CatalogAuthorizationPolicyMatchesMetadata(m)&&c::CatalogAuthorizationPolicyMatchesDatabase(m,self.database_)) {
            const auto body=c::DecodeCatalogAuthorizationPolicy(m.record.payload);
            if(body.record&&std::find(out.principal_schemas.begin(),out.principal_schemas.end(),body.record->user_schema_uuid)==out.principal_schemas.end())
              out.principal_schemas.push_back(body.record->user_schema_uuid);
          }
          return true;
        };
        const auto config=view?db::NativeCatalogVisibilityStreamReader::StreamCommittedSelectedAt(lease,*view,2,5,
            request.limits.maximum_verification_image_bytes,prepared.workspace,visit_config)
            :db::NativeCatalogVisibilityStreamReader::StreamCommittedSelected(lease,2,5,
            request.limits.maximum_verification_image_bytes,prepared.workspace,visit_config);
        if(!config.ok()) {refuse(config,E::configuration_source_failure);return;}
      }
      // 2. The names relation (role 2; the same workspace). Rows are copied out as the stream passes them: a row's views are
      // valid only during the visit. The bytes this snapshot retains are bounded by the request's retained budget.
      {
        std::uint64_t retained=0;bool over=false;
        const auto visit_names=[&](const db::NativeCatalogVisibleRow& row) {
          if(!row.name_payload||!NativeNameRowLive(row)) return true;
          const auto* entry=std::get_if<c::CatalogNameEntryView>(&*row.name_payload);
          if(!entry||entry->lifecycle_state!=c::CatalogNameLifecycle::active||entry->dropped_transaction_uuid) return true;
          retained+=sizeof(engine::internal_api::NativeNameRowValue)+entry->raw_name_text.size();
          if(retained>request.maximum_retained_bytes) {over=true;return false;}
          engine::internal_api::NativeNameRowValue v;
          v.name_entry_uuid=entry->name_entry_uuid.value;v.object_uuid=entry->object_uuid.value;v.scope_uuid=entry->scope_uuid.value;
          v.language_uuid=entry->language_uuid.value;v.dialect_profile_uuid=entry->dialect_profile_uuid.value;
          v.identifier_profile_uuid=entry->identifier_profile_uuid.value;
          if(entry->parent_object_uuid) {v.has_parent_object=true;v.parent_object_uuid=entry->parent_object_uuid->value;}
          if(entry->parent_schema_uuid) {v.has_parent_schema=true;v.parent_schema_uuid=entry->parent_schema_uuid->value;}
          v.object_kind=static_cast<std::uint16_t>(entry->object_kind);
          v.name_class=static_cast<std::uint8_t>(entry->name_class);v.quote_style=static_cast<std::uint8_t>(entry->quote_style);
          v.was_quoted=entry->was_quoted;v.requires_exact_match=entry->requires_exact_match;
          v.default_for_language=entry->default_for_language;v.default_for_object=entry->default_for_object;
          v.raw_name_text.assign(entry->raw_name_text.data(),entry->raw_name_text.size());
          v.catalog_generation_id=entry->catalog_generation_id;v.resource_epoch=entry->resource_epoch;
          v.name_resolution_epoch=entry->name_resolution_epoch;
          out.rows.push_back(std::move(v));
          return true;
        };
        const auto names=view?db::NativeCatalogVisibilityStreamReader::StreamCommittedSelectedAt(lease,*view,2,2,
            request.limits.maximum_verification_image_bytes,prepared.workspace,visit_names)
            :db::NativeCatalogVisibilityStreamReader::StreamCommittedSelected(lease,2,2,
            request.limits.maximum_verification_image_bytes,prepared.workspace,visit_names);
        if(over) {phase=E::names_retained_budget_exceeded;source.lease.Reset();return;}
        if(!names.ok()) {refuse(names,E::names_source_failure);return;}
      }
      source.lease.Reset();
    });
    if(!crypto.ok()) {out.error=E::crypto_failure;out.rows.clear();return out;}
    if(phase!=E::none) {out.error=phase;out.rows.clear();return out;}
    out.database_uuid=self.database_;
    out.error=E::none;
    return out;
  } catch(const std::bad_alloc&) {out.rows.clear();out.error=E::memory_failure;return out;}
    catch(...) {out.rows.clear();out.error=E::names_source_failure;return out;}
}

} // namespace scratchbird::server

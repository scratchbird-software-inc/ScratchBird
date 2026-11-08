// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// SEARCH_KEY: SB_OPEN_VERIFICATION_READ_ONLY (owner direction 2026-11-05).
// Read-only verification of the primary database file on open: L5's first
// startup duty. It runs before any startup transaction exists and writes
// nothing (no publication, phase record, allocation or repair), because
// writing to a corrupt database may corrupt it further. Each check is the
// integrity check the startup transaction's storage scans perform, compared
// against the file as this open selected it instead of against that
// transaction's own state. Spec: SB_DRIFT_STARTUP_VERIFICATION_NON_ALTERING.
#include "native_hosted_database_runtime.hpp"
#include "native_allocation_lease_reader.hpp"
#include "native_horizon_root.hpp"
#include "native_retention_root.hpp"
#include "transaction/mga/transaction_horizon_projection.hpp"
#include "transaction_inventory_reader.hpp"
#include "engine/internal_api/verification/native_startup_catalog_allocation.hpp"
#include "engine/internal_api/verification/native_startup_root_allocation.hpp"
#include "engine/internal_api/verification/native_startup_initial_control_allocation.hpp"
#include <memory>

namespace scratchbird::server {
namespace {
namespace api=engine::internal_api;namespace db=storage::database;namespace disk=storage::disk;
namespace page=storage::page;namespace mga=transaction::mga;
using VE=NativeOpenVerificationError;using VS=NativeOpenVerificationStage;
using byte=core::platform::byte;using u64=core::platform::u64;
// The same classification the snapshot projection uses (native_snapshot_inventory.hpp).
bool ActiveState(mga::TransactionState s) noexcept {
  return s==mga::TransactionState::active||s==mga::TransactionState::read_only_active;
}
bool UncertainState(mga::TransactionState s) noexcept {
  using S=mga::TransactionState;
  return s==S::created||s==S::preparing||s==S::prepared||s==S::committing||s==S::rolling_back||s==S::limbo||s==S::recovering;
}
// A read that cannot decode the file's own structures is a finding. I/O,
// memory, hash-provider, custody and authority refusals are operational, not
// evidence of damage, so their cause is looked for first, in every
// diagnostic the read carries.
VE SourceRefusal(const db::NativeOwnedSourceDiagnostics& d,db::NativeWriterError writer) noexcept {
  using S=db::NativeOwnedSourceError;
  using B=disk::FilespaceBootstrapError;using A=page::NativeAllocationError;
  using D=page::NativeDirectoryError;using I=page::NativeInventoryError;using T=disk::RouteSourceTransitionError;
  switch(d.error){
    case S::none:return writer==db::NativeWriterError::none?VE::none:VE::source_failure;
    case S::resource_exhausted:case S::memory_failure:case S::memory_required:return VE::memory_failure;
    case S::io_failure:return VE::io_failure;
    case S::invalid_request:return VE::invalid_request;
    case S::device_ownership:case S::lock_failure:case S::withdrawn:case S::wrong_process:case S::route_admission:
      return VE::source_failure;
    default:break; // bootstrap_failure, source_failure, incomplete_cohort: classify by cause
  }
  // SB_NATIVE_FIXED_ROOT_READ: a read through page zero's fixed roots.
  using F=db::NativeFixedRootMemoryError;using Z=disk::FilespacePageZeroError;
  switch(d.fixed_root_error){
    case F::io_failure:return VE::io_failure;
    case F::memory_binding_failure:case F::memory_allocation_failure:case F::resource_exhausted:return VE::memory_failure;
    case F::invalid_request:case F::invalid_device:case F::lock_failure:case F::encrypted_requires_authority:return VE::source_failure;
    default:break; // none, or a finding about the file: classified by cause
  }
  if(d.page_zero_error==Z::io_failure)return VE::io_failure;
  if(d.page_zero_error==Z::resource_exhausted)return VE::memory_failure;
  if(d.page_zero_error==Z::hash_provider_failure)return VE::crypto_failure;
  if(d.page_zero_error==Z::device_not_open||d.page_zero_error==Z::invalid_backing)return VE::source_failure;
  if(d.bootstrap_error==B::io_failure||d.allocation_error==A::io_failure||d.directory_error==D::io_failure||
      d.inventory_error==I::io_failure||d.route_error==T::io_failure)return VE::io_failure;
  if(d.bootstrap_error==B::resource_exhausted||d.allocation_error==A::resource_exhausted||d.directory_error==D::resource_exhausted||
      d.inventory_error==I::resource_exhausted||d.route_error==T::resource_exhausted)return VE::memory_failure;
  if(d.bootstrap_error==B::hash_provider_failure||
      d.allocation_error==A::hash_failure||d.directory_error==D::hash_failure||d.inventory_error==I::hash_failure)return VE::crypto_failure;
  if(d.bootstrap_error==B::device_not_open||d.route_error!=T::none||
      d.allocation_error==A::cluster_requires_authority||d.allocation_error==A::invalid_workspace||
      d.directory_error==D::invalid_backing||d.inventory_error==I::encrypted_requires_crypto_authority||
      d.inventory_error==I::invalid_backing)return VE::source_failure;
  return VE::finding;
}
// The same rule for one filespace's allocation-chain read.
VE ChainRefusal(const db::NativeAllocationLeaseReadResult& r) noexcept {
  using L=db::NativeAllocationLeaseError;using A=page::NativeAllocationError;
  if(r.chain_error==A::io_failure||!r.io_status.ok())return VE::io_failure;
  switch(r.error){
    case L::memory_failure:case L::resource_exhausted:return VE::memory_failure;
    case L::hash_failure:return VE::crypto_failure;
    case L::lock_failure:return VE::source_failure;
    case L::invalid_request:return VE::invalid_request;
    default:break;
  }
  if(r.chain_error==A::resource_exhausted||r.chain_error==A::invalid_workspace)return VE::memory_failure;
  if(r.chain_error==A::hash_failure)return VE::crypto_failure;
  if(r.chain_error==A::cluster_requires_authority)return VE::source_failure;
  return VE::finding;
}
} // namespace

NativeOpenVerificationResult NativeHostedDatabaseRuntime::VerifyStartupDatabaseFile(
    CredentialRead& read,const NativeOpenVerificationLimits& limits) noexcept {
  using db::SameReadSource;
  NativeOpenVerificationResult result;
  const auto fail=[&](VE error,VS stage){result.error=error;result.stage=stage;result.observation.reset();return result;};
  read.CheckOwner();
  // Verification precedes the startup transaction. Once one exists, this
  // read-only verification is no longer the first duty and cannot run.
  if(read.mutating_||(read.owner_&&read.owner_->entering_))return fail(VE::busy,VS::none);
  if(!read.ok()||!read.owner_)return fail(VE::not_ready,VS::none);
  if(read.open_verification_){result.error=VE::none;result.stage=VS::complete;result.observation=read.open_verification_;return result;}
  auto& credential=*read.credentials_->comparison->lease;
  if(credential.has_selected_source()||!credential.has_retained_operation()||!read.anchored()||!read.authentication_||
      !read.catalog_metadata_.ok()||!read.verification_policies_.ok()||
      !read.catalogs_.ok()||!read.catalog_capture_.has_physical_pages())return fail(VE::not_ready,VS::none);
  const auto& genesis_source=read.catalog_capture_.source();
  const auto captured=read.catalog_capture_.inventory();
  if(!genesis_source||!captured||!SameReadSource(*genesis_source,*read.verification_policies_.source))
    return fail(VE::source_mismatch,VS::none);
  const auto& genesis=*genesis_source;
  auto& self=*read.owner_;
  const auto files=read.catalog_capture_.filespaces();
  std::size_t maximum_page=0;
  for(const auto& file:files){const auto* profile=disk::FindCanonicalFilespacePageProfile(file.bootstrap.page_size_profile_uuid);
    if(!profile||profile->page_size_bytes!=file.bootstrap.page_size_bytes)return fail(VE::source_mismatch,VS::none);
    maximum_page=std::max<std::size_t>(maximum_page,profile->page_size_bytes);
  }
  if(files.empty()||!maximum_page)return fail(VE::source_mismatch,VS::none);
  // One source read plus the largest per-stage scratch: the allocation-chain
  // workspace, or one page image with the page-zero root array.
  constexpr std::size_t root_slots=32;
  const std::size_t roots_bytes=root_slots*sizeof(disk::FilespaceRootReference);
  const std::size_t slack=4*alignof(std::max_align_t);
  if(!limits.fixed_root.maximum_backing_bytes||!limits.fixed_root.maximum_verification_image_bytes||
      limits.map_backing_bytes<maximum_page||maximum_page>SIZE_MAX-roots_bytes-slack)return fail(VE::invalid_request,VS::none);
  const auto scratch=std::max(limits.map_backing_bytes,maximum_page+roots_bytes)+slack;
  if(limits.fixed_root.maximum_backing_bytes>SIZE_MAX-scratch)return fail(VE::invalid_request,VS::none);
  const auto workspace=limits.fixed_root.maximum_backing_bytes+scratch;
  for(const auto& policy:read.verification_policies_.bodies)
    if(policy.maximum_work_bytes<workspace||policy.maximum_scan_pages<files.size()+1)return fail(VE::policy_refused,VS::none);
  const auto& config=*read.configuration_.configuration;
  if(!self.process_->HasNativeDatabaseAllocationBehavior(self.database_,config.memory,config.memory_version_uuid))
    return fail(VE::memory_failure,VS::none);
  const auto cancelled=[&]{return read.inputs_->request.credential.stop.stop_requested()||read.inputs_->request.source.stop.stop_requested();};
  if(cancelled())return fail(VE::cancelled,VS::none);
  auto allocation=self.Acquire(workspace,NativeBootstrapMemoryKind::storage);
  if(!allocation.ok())return fail(VE::memory_failure,VS::none);
  auto adopted=db::AdoptNativeStorageMemory(StorageBinding(*allocation.admission.resource),allocation.admission.resource);
  if(!adopted.ok())return fail(VE::memory_failure,VS::none);
  auto& memory=adopted.memory;
  read.mutating_=true;self.entering_=true;
  struct Leave{bool& reading;bool& hosting;~Leave(){reading=false;hosting=false;}}leave{read.mutating_,self.entering_};
  auto& operation=credential.retained_operation_;
  NativeOpenVerificationObservation observed;observed.workspace_bytes=workspace;
  VE error=VE::memory_failure;VS stage=VS::identity;
  db::NativeOwnedSourceError source_error=db::NativeOwnedSourceError::none;
  const auto refuse=[&](const auto& source){source_error=source.error;error=SourceRefusal(source,source.writer_error);};
  // SB_NATIVE_FIXED_ROOT_READ: every stage reads page zero's fixed roots.
  // Page zero carries no digest: each content page read here is checked by
  // its own seal when decoded, and bound to its allocation record and common
  // header. No checkpoint, selector, watermark or horizon page is read; the
  // horizons are checked against the counters the first transaction-state
  // page stores.
  const db::NativeFixedRootMemoryLimits fixed_limits{limits.fixed_root.maximum_verification_image_bytes,
      limits.fixed_root.maximum_backing_bytes};
  const auto same_source=[&](const db::NativeFixedRootIdentity& id){
    return read.page_zero_anchor_&&id==*read.page_zero_anchor_&&
        SameReadSource(db::NativeReadSourceIdentityFromFixedRoot(id),genesis);
  };
  // One root's page, its common header bound to the root and its allocation
  // record; the caller's decode checks the page's own seal.
  const auto load_root=[&](const disk::FilespaceRootReference& root,const page::NativeAllocationRecord& owner,
      std::span<byte> buffer,u64& bytes_read)->std::span<const byte>{
    if(cancelled()){error=VE::cancelled;return {};}
    const auto* profile=disk::FindCanonicalFilespacePageProfile(root.page_size_profile_uuid);
    if(!profile||profile->page_size_bytes>buffer.size()||root.page_number>UINT64_MAX/profile->page_size_bytes){
      error=VE::source_mismatch;return {};}
    auto bytes=buffer.first(profile->page_size_bytes);
    const auto io=operation.ReadAtBounded(root.filespace_uuid,root.page_size_profile_uuid,
        root.page_number*u64{profile->page_size_bytes},bytes);
    if(!io.ok()){error=VE::io_failure;return {};}
    bytes_read+=bytes.size();
    const disk::NativeCommonPageHeaderBinding binding{{self.database_,root.filespace_uuid,root.page_size_profile_uuid},
        root.page_number,root.page_generation,root.page_type,owner.page_uuid};
    if(!disk::DecodeNativeCommonPageHeader(bytes.data(),128,&binding).ok()){error=VE::finding;return {};}
    return bytes;
  };
  const auto verify_fixed_root=[&]{
    // Identity: every filespace's page zero against the directory, and the
    // system-state page (page-zero kind 1) against its allocation and creator.
    {
      stage=VS::identity;
      auto arena=memory.CreateArena(memory.binding(),maximum_page+roots_bytes+slack,maximum_page);
      if(!arena.ok())return;
      const auto image_block=arena.arena.Allocate(maximum_page,maximum_page);
      const auto roots_block=arena.arena.Allocate(roots_bytes,alignof(disk::FilespaceRootReference));
      if(!image_block.ok()||!roots_block.ok())return;
      auto* roots=static_cast<disk::FilespaceRootReference*>(roots_block.pointer);std::uninitialized_value_construct_n(roots,root_slots);
      auto source=operation.ObserveFixedRootWithMemory(fixed_limits,memory,memory.binding());
      if(!source.ok()){refuse(source);return;}
      const auto& o=source.observation;
      if(!same_source(o.identity())){error=VE::source_mismatch;return;}
      const auto& inventory=o.inventory();
      if(inventory.next_local_transaction_id!=captured->inventory.next_local_transaction_id||
          inventory.next_commit_sequence!=captured->inventory.next_commit_sequence||
          inventory.entries.size()!=captured->inventory.entries.size()){error=VE::source_mismatch;return;}
      api::NativeStartupStorageIdentityObservation identity;
      identity.source=genesis;identity.workspace_bytes=workspace;
      identity.physical_bytes_read=source.physical_bytes_read;
      for(const auto& directory_page:o.directory().pages)for(const auto& file:directory_page.directory.records){
        if(cancelled()){error=VE::cancelled;return;}
        const auto& b=file.bootstrap;
        const auto expected=std::find_if(files.begin(),files.end(),[&](const auto& f){return f.bootstrap.filespace_uuid==b.filespace_uuid;});
        if(expected==files.end()||expected->bootstrap.page_size_profile_uuid!=b.page_size_profile_uuid||b.page_size_bytes>maximum_page||
            b.database_uuid!=self.database_||b.flags){error=VE::source_mismatch;return;}
        std::span<byte> image{static_cast<byte*>(image_block.pointer),b.page_size_bytes};
        const auto io=operation.ReadAtBounded(b.filespace_uuid,b.page_size_profile_uuid,0,image);
        if(!io.ok()){error=VE::io_failure;return;}
        identity.physical_bytes_read+=image.size();
        const disk::FilespaceBootstrapBinding binding{self.database_,b.filespace_uuid,b.page_size_profile_uuid};
        const auto zero=disk::DecodeFilespacePageZeroInto(image,{roots,root_slots},&binding);
        if(!zero.ok()){error=VE::finding;return;}
        const auto& z=*zero.record;
        if(z.page_uuid!=file.page_zero_uuid||z.page_generation!=file.page_zero_generation||z.root_set_generation!=file.root_set_generation||
            z.total_pages!=file.total_pages||z.bootstrap.flags!=b.flags||z.bootstrap.checksum_profile_uuid!=b.checksum_profile_uuid||
            z.bootstrap.encryption_profile_uuid!=b.encryption_profile_uuid||z.bootstrap.durable_format_generation!=b.durable_format_generation||
            z.bootstrap.filespace_role!=b.filespace_role||z.bootstrap.lifecycle_state!=b.lifecycle_state){error=VE::finding;return;}
        if(b.filespace_uuid==self.primary_){
          // The page zero read now is the one the source was taken from.
          if(z.page_uuid!=o.identity().page_zero_uuid||z.page_generation!=o.identity().page_generation||
              z.root_set_generation!=o.identity().root_set_generation){error=VE::source_mismatch;return;}
          identity.primary_filespace_uuid=b.filespace_uuid;identity.primary_page_uuid=z.page_uuid;
          identity.primary_profile_uuid=b.page_size_profile_uuid;identity.primary_page_generation=z.page_generation;
          if(z.roots.size()>identity.primary_roots.size()){error=VE::invalid_request;return;}
          std::copy(z.roots.begin(),z.roots.end(),identity.primary_roots.begin());identity.primary_root_count=z.roots.size();
        }
        ++identity.filespaces;++identity.identity_pages;
      }
      if(identity.filespaces!=files.size()){error=VE::source_mismatch;return;}
      if(cancelled()){error=VE::cancelled;return;}
      const auto* target=o.Root(1);
      if(!target||target->page_type!=8){error=VE::finding;return;}
      const auto allocated=api::FindNativeStartupRootAllocation(o.allocation(),*target);
      if(!allocated||!allocated->creator_operation_uuid.is_nil()){error=VE::finding;return;}
      const auto state_image=load_root(*target,*allocated,{static_cast<byte*>(image_block.pointer),maximum_page},
          identity.physical_bytes_read);
      if(state_image.empty())return;
      // SB_MGA_DATABASE_STATE (owner 2026-11-07): the database's administrative
      // mode. Any valid mode verifies; whether a mode admits service is the
      // server's decision after verification, not a finding here.
      const auto decoded=db::DecodeNativeDatabaseStateValue(state_image);
      if(!decoded.ok()){error=VE::finding;return;}
      const auto& state=*decoded.state;
      if(state.object_uuid!=target->object_uuid||state.creator_transaction_uuid!=allocated->creator_transaction_uuid||
          state.creator_local_transaction_id!=allocated->creator_local_transaction_id){error=VE::finding;return;}
      const auto creator=std::find_if(inventory.entries.begin(),inventory.entries.end(),[&](const auto& e){
        return e.identity.local_id.value==state.creator_local_transaction_id;});
      if(creator==inventory.entries.end()||creator->identity.transaction_uuid.value!=state.creator_transaction_uuid||
          creator->identity.scope!=mga::TransactionScope::local_node||!mga::HasCommittedInventoryOutcome(*creator)){error=VE::finding;return;}
      identity.system_state=state;++identity.identity_pages;
      observed.identity=identity;
    }
    // Allocation: every filespace's allocation chain covers the captured
    // catalog pages and the creator's pages; unreachable ones are wasted.
    {
      stage=VS::allocation;
      auto maps=db::PrepareNativeAllocationRead(limits.map_backing_bytes,memory,memory.binding());
      if(!maps.ok()){error=VE::memory_failure;return;}
      auto source=operation.ReadFixedRootWithMemory(fixed_limits,memory,memory.binding());
      if(!source.ok()){refuse(source);return;}
      const auto& lease=source.lease.source();
      if(!same_source(lease.identity())){error=VE::source_mismatch;return;}
      api::NativeStartupStorageScanObservation scan;scan.source=genesis;
      scan.physical_bytes_read=source.physical_bytes_read;
      for(const auto& directory_page:lease.directory().pages)for(const auto& member:directory_page.directory.records){
        if(cancelled()){error=VE::cancelled;return;}
        ++scan.filespaces;
        const auto chain=db::NativeAllocationLeaseReader::Read(lease,member.bootstrap.filespace_uuid,
            limits.fixed_root.maximum_verification_image_bytes,maps.workspace);
        if(!chain.ok()){error=ChainRefusal(chain);return;}
        const auto coverage=api::CheckNativeStartupCatalogAllocation(read.catalog_capture_,chain.chain);
        if(!coverage.ok()){error=VE::finding;return;}
        const auto initial=api::CheckNativeStartupInitialControlAllocation(read.catalog_capture_,observed.identity,chain.chain);
        if(!initial.ok()){error=VE::finding;return;}
        scan.initial_control_pages+=initial.control_pages;scan.wasted_pages+=initial.wasted_pages;
        scan.catalog_pages+=coverage.matched_pages;
        scan.physical_bytes_read+=chain.physical_bytes_read;
        if(chain.chain.pages.size()>UINT64_MAX-scan.allocation_pages||member.total_pages>UINT64_MAX-scan.capacity_pages){error=VE::finding;return;}
        scan.allocation_pages+=chain.chain.pages.size();scan.capacity_pages+=member.total_pages;
        for(std::size_t n=0;n<scan.allocation_states.size();++n){
          if(chain.chain.state_counts[n]>UINT64_MAX-scan.allocation_states[n]){error=VE::finding;return;}
          scan.allocation_states[n]+=chain.chain.state_counts[n];
        }
        for(const auto& policy:read.verification_policies_.bodies)
          if(scan.allocation_pages>policy.maximum_scan_pages){error=VE::policy_refused;return;}
      }
      if(scan.filespaces!=files.size()){error=VE::source_mismatch;return;}
      if(scan.catalog_pages!=read.catalog_capture_.physical_pages().size()){error=VE::finding;return;}
      observed.allocation=scan;
    }
    // Inventory: structure, and the projected horizons against the ones the
    // first transaction-state page stores; then the retention root.
    {
      stage=VS::inventory;
      auto source=operation.ReadFixedRootWithMemory(fixed_limits,memory,memory.binding());
      if(!source.ok()){refuse(source);return;}
      const auto& lease=source.lease.source();
      if(!same_source(lease.identity())){error=VE::source_mismatch;return;}
      const auto& inventory=lease.inventory();
      const auto count=inventory.entries.size();
      if(!count||count>(SIZE_MAX-slack-maximum_page)/(sizeof(std::size_t)+1)){error=VE::finding;return;}
      auto arena=memory.CreateArena(memory.binding(),maximum_page+count*(sizeof(std::size_t)+1)+slack,maximum_page);
      if(!arena.ok())return;
      const auto image_block=arena.arena.Allocate(maximum_page,maximum_page);
      const auto index_block=arena.arena.Allocate(count*sizeof(std::size_t),alignof(std::size_t));
      const auto marker_block=arena.arena.Allocate(count,1);
      if(!image_block.ok()||!index_block.ok()||!marker_block.ok())return;
      const page::NativeTransactionInventoryConstView view{inventory.next_local_transaction_id,inventory.next_commit_sequence,inventory.entries};
      const auto valid=page::ValidateNativeTransactionInventoryConstView(view,
          {static_cast<std::size_t*>(index_block.pointer),count},{static_cast<byte*>(marker_block.pointer),count});
      if(!valid.ok()){error=VE::finding;return;}
      api::NativeStartupStorageScanObservation scan;scan.source=genesis;
      scan.physical_bytes_read=source.physical_bytes_read;
      if(mga::detail::ProjectValidatedLocalHorizons(view,{},scan.horizons)){error=VE::finding;return;}
      // SB_TRANSACTION_INVENTORY_WRITER: the stored horizons are written at
      // BEGIN and trail the projection after COMMIT, ROLLBACK, restart
      // dead-marking or a crash between BEGIN's writes; stored <= projected.
      if(!page::StoredTransactionHorizonsAdmissible(lease.counters(),inventory.next_local_transaction_id,scan.horizons)){
        error=VE::finding;return;}
      for(const auto& entry:inventory.entries){
        if(ActiveState(entry.state))++scan.active_transactions;
        if(UncertainState(entry.state))++scan.in_doubt_transactions;
      }
      scan.transactions=count;scan.inventory_pages=lease.inventory_pages().size();
      for(const auto& directory_page:lease.directory().pages)scan.filespaces+=directory_page.directory.records.size();
      for(const auto& policy:read.verification_policies_.bodies)
        if(scan.inventory_pages+2>policy.maximum_scan_pages){error=VE::policy_refused;return;}
      const auto& creator=inventory.entries.front();
      if(!mga::HasCommittedInventoryOutcome(creator)||creator.identity.scope!=mga::TransactionScope::local_node){error=VE::finding;return;}
      const auto* r=lease.Root(10);
      if(!r||r->page_type!=0x303){error=VE::finding;return;}
      const auto retention=*r;const auto origin=creator.identity;
      const auto retention_allocation=api::FindNativeStartupRootAllocation(lease.allocation(),retention);
      if(!retention_allocation||!retention_allocation->creator_operation_uuid.is_nil()||
          retention_allocation->creator_transaction_uuid!=origin.transaction_uuid.value||
          retention_allocation->creator_local_transaction_id!=origin.local_id.value){error=VE::finding;return;}
      // Copy the references before retiring the read backing. The retained
      // operation still owns every bounded physical read.
      const auto owner=*retention_allocation;
      source.lease.Reset();
      const auto retained_image=load_root(retention,owner,{static_cast<byte*>(image_block.pointer),maximum_page},
          scan.physical_bytes_read);
      if(retained_image.empty())return;
      const auto pins=page::DecodeNativeRetentionPageInto(retained_image,{});
      if(!pins.ok()||pins.page->object_uuid!=retention.object_uuid||pins.page->creator_transaction_uuid!=origin.transaction_uuid.value||
          pins.page->creator_local_transaction_id!=origin.local_id.value||pins.page->flags||pins.page->total_pins||
          pins.page->first_record||pins.page->next||!pins.page->records.empty()){error=VE::finding;return;}
      scan.horizon_records=0;scan.retention_pins=pins.page->total_pins;
      observed.inventory=scan;
    }
    // Catalog routes: the configuration and security roots (page-zero kinds
    // 6 and 7) resolve to the catalog relations this open already traversed.
    {
      stage=VS::catalog_routes;
      auto arena=memory.CreateArena(memory.binding(),maximum_page+slack,maximum_page);
      if(!arena.ok())return;
      const auto block=arena.arena.Allocate(maximum_page,maximum_page);if(!block.ok())return;
      auto source=operation.ReadFixedRootWithMemory(fixed_limits,memory,memory.binding());
      if(!source.ok()){refuse(source);return;}
      const auto& lease=source.lease.source();
      if(!same_source(lease.identity())){error=VE::source_mismatch;return;}
      api::NativeStartupCatalogRouteObservation routes{genesis,0,source.physical_bytes_read};
      std::array<disk::FilespaceRootReference,2> targets;
      std::array<page::NativeAllocationRecord,2> allocations;
      std::array<page::NativeCatalogRootReference,6> references;
      const auto& inventory=lease.inventory();
      if(inventory.entries.empty()||!mga::HasCommittedInventoryOutcome(inventory.entries.front())){error=VE::finding;return;}
      const auto creator=inventory.entries.front().identity;
      u64 additional_bytes=0;
      for(std::size_t i=0;i<targets.size();++i){
        const auto* at=lease.Root(static_cast<core::platform::u16>(6+i));
        if(!at||at->page_type!=10+i||at->filespace_uuid!=self.primary_){error=VE::finding;return;}
        targets[i]=*at;
        const auto* profile=disk::FindCanonicalFilespacePageProfile(at->page_size_profile_uuid);
        if(!profile||profile->page_size_bytes>maximum_page||at->page_number>UINT64_MAX/profile->page_size_bytes){error=VE::source_mismatch;return;}
        additional_bytes+=profile->page_size_bytes;
        const auto allocated=api::FindNativeStartupRootAllocation(lease.allocation(),*at);
        if(!allocated||!allocated->creator_operation_uuid.is_nil()||allocated->creator_transaction_uuid!=creator.transaction_uuid.value||
            allocated->creator_local_transaction_id!=creator.local_id.value){error=VE::finding;return;}
        allocations[i]=*allocated;
      }
      const auto image_limit=limits.fixed_root.maximum_verification_image_bytes;
      const auto retained=lease.retained_image_bytes();
      if(retained>image_limit||additional_bytes>image_limit-retained){error=VE::invalid_request;return;}
      source.lease.Reset();
      const auto physical=read.catalog_capture_.physical_pages();
      for(std::size_t i=0;i<targets.size();++i){
        const auto& target=targets[i];
        const auto bytes=load_root(target,allocations[i],{static_cast<byte*>(block.pointer),maximum_page},routes.physical_bytes_read);
        if(bytes.empty())return;
        const auto decoded=page::DecodeNativeCatalogRootInto(bytes,references);
        if(!decoded.ok()){error=VE::finding;return;}
        const auto& root=*decoded.root;const auto& h=root.header;
        if(h.database_uuid!=self.database_||h.flags||root.root_kind!=6+i||root.object_uuid!=target.object_uuid||
            root.creator_transaction_uuid!=creator.transaction_uuid.value||root.creator_local_transaction_id!=creator.local_id.value||
            root.predecessor||root.roots.size()!=1){error=VE::finding;return;}
        const auto& route=root.roots.front();const auto expected_role=i==0?5u:4u;
        if(route.role!=expected_role){error=VE::finding;return;}
        const db::NativeCatalogRelationInspection* relation=nullptr;
        for(std::size_t n=0;n<read.catalogs_.relation_count;++n){const auto& candidate=read.catalogs_.relations[n];
          if(candidate.selector==2&&candidate.role==expected_role){
            if(relation){error=VE::finding;return;}relation=&candidate;
          }
        }
        // SB_CATALOG_ROOT_ADVANCE (owner Q24 = 1): the catalog root is
        // rewritten in place after COMMIT, then each route root; a crash
        // between them leaves a route root behind the catalog root, never
        // ahead of it. Behind is admitted (the next advance levels it).
        if(!relation||route.page!=relation->root||root.catalog_generation>relation->catalog_generation||
            root.schema_epoch>relation->schema_epoch||root.security_epoch>relation->security_epoch||
            root.resource_epoch>relation->resource_epoch){error=VE::finding;return;}
        const auto found=std::find_if(physical.begin(),physical.end(),[&](const auto& p){
          return p.header.filespace_uuid==route.page.filespace_uuid&&p.header.page_number==route.page.page_number;});
        if(found==physical.end()||found->header.page_generation!=route.page.page_generation||
            found->header.page_size_profile_uuid!=route.page.page_size_profile_uuid||found->header.page_type!=route.page_type||
            found->owner_uuid!=route.object_uuid){error=VE::finding;return;}
        ++routes.routes;
      }
      observed.routes=routes;
    }
    if(cancelled()){error=VE::cancelled;return;}
    stage=VS::complete;error=VE::none;
  };
  const auto crypto=self.database_crypto_.WithMemoryScope(self.crypto_binding_,[&]{verify_fixed_root();});
  if(!crypto.ok())return fail(VE::crypto_failure,stage);
  if(error!=VE::none){result.source_error=source_error;return fail(error,stage);}
  read.open_verification_=observed;
  result.error=VE::none;result.stage=VS::complete;result.observation=observed;
  return result;
}
} // namespace scratchbird::server

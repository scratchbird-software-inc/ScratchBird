// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_creation_workspace.hpp"
#include "native_publication_watermark.hpp"
#include "disk_device.hpp"
#include "hash_digest.hpp"
#include "physical_mga_cow_store.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>

namespace scratchbird::storage::database {
namespace {
using namespace core::platform;
using E=NativeCreationWorkspaceError;
namespace mga=transaction::mga;
NativeCreationWorkspaceResult Fail(E e) {NativeCreationWorkspaceResult r;r.error=e;return r;}

// Control offsets after the last allocation-map page. Page zero is separate.
enum Control : unsigned {checkpoint=1, inventory, directory, system, retention,
  horizon, catalog, configuration, security, first_leaf, selector_first=16,
  selector_second=17, watermark_first=18, watermark_second=19, count=20};
constexpr std::array<u32,count> types{0,0x300,0x301,9,8,0x303,0x302,5,10,11,
  6,6,6,6,6,6,0x30e,0x30e,0x500,0x500};
struct Image {
  disk::NativeCommonPageHeader header;
  Uuid owner, allocation;
  std::vector<byte> bytes;
  std::array<byte,32> sha256{};
};
u64 Coverage(u64 size) {
  u64 n=(size-384)/128;
  while (((384+(n+1)/2+7)&~u64{7})+128*n>size) --n;
  return n;
}
}  // namespace

NativeCreationWorkspaceResult InitializeNativeCreationWorkspaceOnOpenDevice(
    disk::FileDevice& device,const NativeFilespaceInitializationRequest& request,u64 budget) noexcept {
  try {
    const auto guard=device.AcquireOperationGuard();
    const auto& b=request.bootstrap;
    const auto v7=[](const Uuid& id){return core::uuid::IsEngineIdentityUuid(id);};
    if(disk::ValidateFilespaceBootstrap(b)!=disk::FilespaceBootstrapError::none||
       b.lifecycle_state!=7||b.filespace_role>4||!v7(request.operation_uuid)||
       !v7(request.writer_uuid)||!v7(request.creator.transaction_uuid.value)||
       request.creator.transaction_uuid.kind!=UuidKind::transaction||request.creator.local_id.value!=1||
       request.creator.scope!=mga::TransactionScope::local_node||!request.creation_utc_millis||
       request.creation_utc_millis>0xffffffffffffULL) return Fail(E::invalid_request);
    if(b.flags&disk::FilespaceBootstrapFlag::cluster_authority_required) return Fail(E::cluster_requires_authority);
    if(b.flags&disk::FilespaceBootstrapFlag::payload_encrypted) return Fail(E::encrypted_requires_crypto_authority);
    if(!device.is_open()||device.read_only()) return Fail(E::invalid_device);
    const auto existing=device.Size();
    if(!existing.ok()) return Fail(E::io_failure);
    if(existing.size_bytes) return Fail(E::device_not_empty);
    const u64 size=b.page_size_bytes,total=request.total_pages,coverage=Coverage(size);
    if(!total||total>std::numeric_limits<u64>::max()/size||
       !disk::CheckFileDeviceExtent(total*size,0).ok()) return Fail(E::invalid_capacity);
    const u64 maps=total/coverage+(total%coverage!=0);
    if(maps>total||total-maps<count) return Fail(E::invalid_capacity);
    // Full graph + one in-flight encoder image + one physical readback image.
    if(budget/size<count+2||maps>budget/size-count-2||
       maps>std::numeric_limits<std::size_t>::max()-count) return Fail(E::resource_exhausted);

    std::set<Uuid> ids;
    for(const auto& id:{b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid,
        b.checksum_profile_uuid,request.operation_uuid,request.writer_uuid,request.creator.transaction_uuid.value})
      if(!ids.insert(id).second) return Fail(E::invalid_request);
    const auto issue=[&](UuidKind kind)->Uuid {
      const auto id=core::uuid::GenerateDurableEngineIdentityV7(kind,request.creation_utc_millis);
      if(!id.ok()||!ids.insert(id.value.value).second) throw E::identity_failure;
      return id.value.value;
    };
    const Uuid map_object=issue(UuidKind::object), timeline=issue(UuidKind::object),
      locator=issue(UuidKind::object), publication=issue(UuidKind::object), selector=issue(UuidKind::object),
      watermark=issue(UuidKind::object);
    std::vector<Image> images(maps+count);
    for(u64 n=0;n<images.size();++n) {
      auto& image=images[n];
      const u32 type=n==0?1:n<=maps?3:types[n-maps];
      image.header={b.page_size_bytes,type,b.database_uuid,b.filespace_uuid,
        issue(UuidKind::page),n,1,0,b.page_size_profile_uuid};
      image.allocation=issue(UuidKind::object);
      image.owner=n==0?b.filespace_uuid:n<=maps?map_object:
        n>=maps+watermark_first?watermark:n>=maps+selector_first?selector:issue(UuidKind::object);
    }
    const auto ref=[&](u64 n){return disk::NativePageReference{b.filespace_uuid,n,1,b.page_size_profile_uuid};};
    const auto root=[&](u16 kind,u64 n){return disk::FilespaceRootReference{
      kind,images[n].header.page_type,b.filespace_uuid,n,1,b.page_size_profile_uuid,images[n].owner};};
    const auto save=[&](u64 n,auto encoded) {
      if(!encoded.ok()) throw E::encoding_failure;
      images[n].bytes=std::move(encoded.bytes);
      const auto hash=core::hash::ComputeSha256Digest(images[n].bytes);
      if(!hash.ok()) throw E::hash_failure;
      images[n].sha256=hash.digest;
    };
    const Uuid creator=request.creator.transaction_uuid.value;
    auto begun=mga::BeginLocalTransaction(mga::MakeEmptyLocalTransactionInventory(),
      request.creator.transaction_uuid,request.creation_utc_millis);
    if(!begun.ok()) return Fail(E::encoding_failure);
    auto committed=mga::CommitLocalTransaction(std::move(begun.inventory),request.creator.local_id,request.creation_utc_millis);
    if(!committed.ok()) return Fail(E::encoding_failure);
    page::NativeTransactionInventoryPage inv;
    inv.header=images[maps+inventory].header;inv.object_uuid=images[maps+inventory].owner;
    inv.inventory_generation=1;inv.inventory=std::move(committed.inventory);
    save(maps+inventory,page::EncodeNativeTransactionInventoryPage(inv));

    NativeCreationWorkspaceReceipt receipt;
    receipt.database_uuid=b.database_uuid;receipt.filespace_uuid=b.filespace_uuid;
    receipt.operation_uuid=request.operation_uuid;receipt.page_zero_uuid=images[0].header.page_uuid;
    receipt.timeline_uuid=timeline;receipt.publication_uuid=publication;receipt.construction_transaction=request.creator;
    receipt.total_pages=total;receipt.free_pages=total-images.size();receipt.map_pages=maps;
    receipt.checkpoint=root(9,maps+checkpoint);
    for(unsigned i=0;i<6;++i) {
      const u64 n=maps+first_leaf+i;
      NativeCatalogLeafPage leaf;leaf.header=images[n].header;
      leaf.body.relation_uuid={UuidKind::object,images[n].owner};
      leaf.body.segment_id=leaf.body.segment_generation=leaf.body.page_generation=leaf.body.compaction_generation=1;
      leaf.body.page_number=n;
      save(n,EncodeNativeCatalogLeaf(leaf));
      receipt.relations[i]={static_cast<u16>(i+1),6,ref(n),images[n].owner};
    }
    for(const auto control:{catalog,configuration,security}) {
      const u64 n=maps+control;
      page::NativeCatalogRoot c;c.header=images[n].header;c.object_uuid=images[n].owner;
      c.root_kind=control==catalog?2:control==configuration?6:7;
      c.creator_transaction_uuid=creator;c.creator_local_transaction_id=1;
      c.catalog_generation=c.schema_epoch=c.security_epoch=1;
      if(control==catalog)c.roots.assign(receipt.relations.begin(),receipt.relations.end());
      else c.roots.push_back(receipt.relations[control==configuration?4:3]);
      save(n,page::EncodeNativeCatalogRoot(c));
    }
    page::NativeFilespaceDirectory d;d.header=images[maps+directory].header;
    d.object_uuid=images[maps+directory].owner;d.directory_generation=1;
    d.creator_transaction_uuid=creator;d.creator_local_transaction_id=1;d.total_records=1;
    d.records.push_back({b,locator,receipt.page_zero_uuid,1,1,total,1,{}});
    save(maps+directory,page::EncodeNativeFilespaceDirectory(d));
    NativeSystemState state;state.header=images[maps+system].header;state.object_uuid=images[maps+system].owner;
    state.state_generation=state.restart_generation=state.startup_counter=1;
    state.creator_transaction_uuid=creator;state.creator_local_transaction_id=1;
    state.checkpoint_generation=1;state.checkpoint=ref(maps+checkpoint);
    state.checkpoint_object_uuid=images[maps+checkpoint].owner;state.transition_operation_uuid=request.operation_uuid;
    save(maps+system,EncodeNativeSystemState(state));
    page::NativeRetentionPage pins;pins.header=images[maps+retention].header;pins.object_uuid=images[maps+retention].owner;
    pins.epoch=1;pins.creator_transaction_uuid=creator;pins.creator_local_transaction_id=1;
    save(maps+retention,page::EncodeNativeRetentionPage(pins));
    const auto derived=mga::ComputeLocalTransactionHorizons(inv.inventory);
    if(!derived.ok()) return Fail(E::encoding_failure);
    page::NativeHorizonRoot h;h.header=images[maps+horizon].header;h.object_uuid=images[maps+horizon].owner;
    h.epoch=1;h.creator_transaction_uuid=creator;h.creator_local_transaction_id=1;h.total_records=3;
    h.retention=ref(maps+retention);h.retention_object_uuid=pins.object_uuid;h.retention_sha256=images[maps+retention].sha256;
    const std::array<u64,3> boundaries{derived.horizons.oldest_interesting_transaction.value,
      derived.horizons.oldest_active_transaction.value,derived.horizons.oldest_snapshot_transaction.value};
    for(unsigned i=0;i<3;++i) {
      page::NativeHorizonRecord r;r.kind=static_cast<page::NativeHorizonKind>(i+1);
      r.local_boundary=boundaries[i];r.owner_uuid=request.writer_uuid;r.horizon_uuid=issue(UuidKind::object);r.timeline_uuid=timeline;
      h.records.push_back(r);
    }
    save(maps+horizon,page::EncodeNativeHorizonRoot(h));
    for(u64 i=maps;i>0;--i) {
      page::NativeAllocationMap map;map.header=images[i].header;map.object_uuid=map_object;
      map.map_generation=map.capacity_generation=1;map.total_pages=total;map.first_page=(i-1)*coverage;
      map.creator_transaction_uuid=creator;map.creator_local_transaction_id=1;
      const auto covered=std::min(coverage,total-map.first_page);
      map.states.assign(covered,page::NativeAllocationState::free);
      const auto end=std::min(map.first_page+covered,static_cast<u64>(images.size()));
      for(u64 n=map.first_page;n<end;++n) {
        const auto& image=images[n];map.states[n-map.first_page]=page::NativeAllocationState::allocated;
        map.records.push_back({n,image.allocation,image.header.page_uuid,image.owner,creator,1,1,0,image.header.page_type});
      }
      if(i<maps){map.next=ref(i+1);map.next_sha256=images[i+1].sha256;}
      save(i,page::EncodeNativeAllocationMap(map));
    }
    NativeCheckpointRoot cp;cp.header=images[maps+checkpoint].header;cp.object_uuid=images[maps+checkpoint].owner;
    cp.checkpoint_generation=cp.root_set_generation=cp.selected_local_transaction_id=1;
    cp.stable_local_transaction_id=cp.local_durable_transaction_id=cp.creator_local_transaction_id=1;
    cp.timeline_uuid=timeline;cp.creator_transaction_uuid=creator;cp.completed=true;
    const std::array<u64,10> roots{maps+inventory,maps+horizon,maps+directory,1,maps+catalog,
      maps+configuration,maps+security,maps+system,maps+catalog,maps+retention};
    for(unsigned i=0;i<roots.size();++i) {
      const auto n=roots[i];const auto& image=images[n];
      cp.roots.push_back({static_cast<u16>(i+1),image.header.page_type,ref(n),image.owner,image.sha256});
    }
    save(maps+checkpoint,EncodeNativeCheckpointRoot(cp));receipt.checkpoint_sha256=images[maps+checkpoint].sha256;
    for(const auto control:{watermark_first,watermark_second}) {
      const auto n=maps+control;NativePublicationWatermark w;w.header=images[n].header;
      w.object_uuid=watermark;w.bootstrap_uuid=receipt.page_zero_uuid;w.timeline_uuid=timeline;
      w.operation_uuid=request.operation_uuid;w.watermark=w.base_checkpoint_generation=w.base_root_set_generation=1;
      w.base_checkpoint=ref(maps+checkpoint);w.base_checkpoint_object_uuid=cp.object_uuid;
      w.base_checkpoint_sha256=receipt.checkpoint_sha256;
      save(n,EncodeNativePublicationWatermark(w));
    }
    for(const auto control:{selector_first,selector_second}) {
      const auto n=maps+control;NativeCheckpointSelection s;s.header=images[n].header;
      s.object_uuid=selector;s.bootstrap_uuid=receipt.page_zero_uuid;s.publication_uuid=publication;
      s.selection_generation=s.checkpoint_generation=s.root_set_generation=1;s.checkpoint=ref(maps+checkpoint);
      s.checkpoint_object_uuid=cp.object_uuid;s.checkpoint_sha256=receipt.checkpoint_sha256;s.timeline_uuid=timeline;
      save(n,EncodeNativeCheckpointSelection(s));
    }
    disk::FilespacePageZero zero;zero.bootstrap=b;zero.page_uuid=receipt.page_zero_uuid;
    zero.creation_operation_uuid=request.operation_uuid;zero.writer_identity_uuid=request.writer_uuid;
    zero.page_generation=zero.root_set_generation=1;zero.total_pages=total;zero.free_pages=receipt.free_pages;
    zero.creation_utc_millis=request.creation_utc_millis;
    const std::array<u64,10> zero_roots{maps+system,maps+catalog,1,maps+inventory,maps+directory,
      maps+configuration,maps+security,maps+catalog,maps+checkpoint,maps+retention};
    for(unsigned i=0;i<zero_roots.size();++i)zero.roots.push_back(root(i+1,zero_roots[i]));
    zero.roots.push_back(root(18,maps+selector_first));zero.roots.push_back(root(19,maps+selector_second));
    zero.roots.push_back(root(20,maps+watermark_first));zero.roots.push_back(root(21,maps+watermark_second));
    auto encoded_zero=disk::EncodeFilespacePageZero(zero);
    if(!encoded_zero.ok()) return Fail(E::encoding_failure);
    images[0].bytes=std::move(*encoded_zero.bytes);

    // No writes before the complete graph, identities and receipt are prepared.
    std::vector<byte> scratch(size,0);
    const auto write=[&](u64 n,const auto& bytes) {
      const auto io=device.WriteAt(n*size,bytes.data(),bytes.size());
      return io.ok()&&io.bytes_transferred==bytes.size();
    };
    const auto compare=[&](u64 n,const std::vector<byte>* expected)->E {
      const auto io=device.ReadAt(n*size,scratch.data(),scratch.size());
      if(!io.ok()||io.bytes_transferred!=scratch.size())return E::io_failure;
      if(expected?scratch!=*expected:!std::all_of(scratch.begin(),scratch.end(),[](byte b){return !b;}))return E::readback_mismatch;
      return E::none;
    };
    for(u64 n=0;n<total;++n)if(!write(n,scratch))return Fail(E::io_failure);
    for(u64 n=1;n<maps+selector_first;++n)if(!write(n,images[n].bytes))return Fail(E::io_failure);
    for(u64 n=maps+watermark_first;n<=maps+watermark_second;++n)if(!write(n,images[n].bytes))return Fail(E::io_failure);
    if(!write(0,images[0].bytes)||!device.Sync().ok())return Fail(E::io_failure);
    for(u64 n=0;n<total;++n) {
      const bool dependency=n<images.size()&&(n<maps+selector_first||n>maps+selector_second);
      const auto error=compare(n,dependency?&images[n].bytes:nullptr);
      if(error!=E::none)return Fail(error);
    }
    for(u64 n=maps+selector_first;n<=maps+selector_second;++n) {
      if(!write(n,images[n].bytes)||!device.Sync().ok())return Fail(E::io_failure);
      const auto error=compare(n,&images[n].bytes);if(error!=E::none)return Fail(error);
    }
    images.clear();std::vector<byte>().swap(scratch);
    const std::vector<disk::NativeFilespaceDevice> devices{{b.filespace_uuid,b.page_size_profile_uuid,&device}};
    {const auto selected=ReadNativeBoundCheckpointSelectionFromOpenDevices(b.database_uuid,devices,b.filespace_uuid,budget);
      if(!selected.ok()){auto r=Fail(E::graph_failure);r.selection_error=selected.error;r.checkpoint_error=selected.checkpoint_error;return r;}
      // Selector admission owns checkpoint/inventory/slot allocations. Bind
      // every other root and leaf to its actual physical allocation as well.
      const auto allocated=[&](const auto& target,u32 type,const Uuid& owner) {
        if(target.filespace_uuid!=b.filespace_uuid||target.page_size_profile_uuid!=b.page_size_profile_uuid)return false;
        std::vector<byte> bytes(size);
        const auto io=device.ReadAt(target.page_number*size,bytes.data(),bytes.size());
        if(!io.ok()||io.bytes_transferred!=bytes.size())return false;
        const disk::NativeCommonPageHeaderBinding binding{{b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid},
          target.page_number,target.page_generation,type,{}};
        const auto header=disk::DecodeNativeCommonPageHeader(bytes.data(),128,&binding);
        if(!header.ok())return false;
        for(const auto& image:selected.allocation.pages)for(const auto& record:image.map->records)
          if(record.page_number==target.page_number)return
            image.map->states[record.page_number-image.map->first_page]==page::NativeAllocationState::allocated&&
            record.owner_uuid==owner&&record.page_uuid==header.header->page_uuid&&record.page_type==type&&
            record.page_generation==target.page_generation&&record.creator_transaction_uuid==creator&&record.creator_local_transaction_id==1;
        return false;
      };
      for(const auto& r:selected.checkpoint_inventory.checkpoint->roots)
        if(!allocated(r.page,r.page_type,r.object_uuid))return Fail(E::graph_failure);
      for(const auto& r:receipt.relations)
        if(!allocated(r.page,r.page_type,r.object_uuid))return Fail(E::graph_failure);
      for(const auto control:{watermark_first,watermark_second}) {
        const auto n=maps+control;std::vector<byte> bytes(size);
        const auto io=device.ReadAt(n*size,bytes.data(),bytes.size());
        if(!io.ok()||io.bytes_transferred!=bytes.size())return Fail(E::io_failure);
        const auto actual=DecodeNativePublicationWatermark(bytes);
        if(!actual.ok())return Fail(actual.error==NativePublicationWatermarkError::hash_failure?E::hash_failure:
          actual.error==NativePublicationWatermarkError::resource_exhausted?E::resource_exhausted:E::graph_failure);
        NativePublicationWatermark expected;expected.header=actual.state->header;
        expected.object_uuid=watermark;expected.bootstrap_uuid=receipt.page_zero_uuid;expected.timeline_uuid=timeline;
        expected.operation_uuid=request.operation_uuid;expected.watermark=expected.base_checkpoint_generation=expected.base_root_set_generation=1;
        expected.base_checkpoint=ref(maps+checkpoint);expected.base_checkpoint_object_uuid=cp.object_uuid;
        expected.base_checkpoint_sha256=receipt.checkpoint_sha256;
        const auto encoded=EncodeNativePublicationWatermark(expected);
        if(!encoded.ok())return Fail(encoded.error==NativePublicationWatermarkError::hash_failure?E::hash_failure:
          encoded.error==NativePublicationWatermarkError::resource_exhausted?E::resource_exhausted:E::graph_failure);
        if(encoded.bytes!=bytes||!allocated(ref(n),0x500,watermark))return Fail(E::graph_failure);
      }
    }
    // Each owning reader obtains evidence from the actual selected graph;
    // results are released between families instead of multiplying the budget.
    const auto graph_error=[&](const auto& result)->std::optional<NativeCreationWorkspaceResult> {
      if(result.ok())return {};
      auto r=Fail(E::graph_failure);r.checkpoint_error=result.error;return r;
    };
    if(auto r=graph_error(VerifyCurrentNativeCheckpointAllocationFromOpenDevices(b.database_uuid,devices,receipt.checkpoint,budget)))return *r;
    if(auto r=graph_error(VerifyNativeCheckpointPolicyRootsFromOpenDevices(b.database_uuid,devices,receipt.checkpoint,budget)))return *r;
    if(auto r=graph_error(VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(b.database_uuid,devices,receipt.checkpoint,budget)))return *r;
    if(auto r=graph_error(VerifyCurrentNativeCheckpointSystemStateFromOpenDevices(b.database_uuid,devices,receipt.checkpoint,budget)))return *r;
    if(auto r=graph_error(VerifyCurrentNativeCheckpointHorizonFromOpenDevices(b.database_uuid,devices,receipt.checkpoint,budget)))return *r;
    for(const auto& relation:receipt.relations) {
      const auto leaf=ReadNativeCatalogLeafFromOpenDevice(device,b.database_uuid,relation);
      if(!leaf.ok()||!leaf.page->body.rows.empty())return Fail(E::graph_failure);
    }
    NativeCreationWorkspaceResult result;result.error=E::none;result.receipt=std::move(receipt);return result;
  }catch(E error){return Fail(error);}
   catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
   catch(...){return Fail(E::io_failure);}
}
}  // namespace scratchbird::storage::database

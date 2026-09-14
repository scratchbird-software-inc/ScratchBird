// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_creation_workspace_recovery.hpp"
#include "disk_device.hpp"
#include "hash_digest.hpp"
#include "physical_mga_cow_store.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>

namespace scratchbird::storage::database {
namespace {
using namespace core::platform;
using E=NativeCreationRecoveryError;
namespace mga=transaction::mga;
NativeCreationRecoveryResult Fail(E e){NativeCreationRecoveryResult r;r.error=e;return r;}
void Require(bool b,E e=E::graph_failure){if(!b)throw e;}
template<class R> void Valid(const R& r){
  if(r.ok())return;
  if(r.error==decltype(r.error)::hash_failure)throw E::hash_failure;
  if(r.error==decltype(r.error)::resource_exhausted)throw E::resource_exhausted;
  throw E::graph_failure;
}
struct Image {
  std::vector<byte> bytes;
  disk::NativeCommonPageHeader header;
  Uuid owner;
  page::NativeAllocationRecord allocation;
  std::array<byte,32> sha256{};
};
constexpr std::array<u32,18> types{0,0x300,0x301,9,8,0x303,0x302,5,10,11,6,6,6,6,6,6,0x30e,0x30e};
}

NativeCreationRecoveryResult RecoverNativeCreationWorkspaceSelectionOnOpenDevice(
    disk::FileDevice& device,const disk::FilespaceBootstrapBinding& binding,
    const Uuid& operation,u64 budget) noexcept {
  try {
    const auto guard=device.AcquireOperationGuard();
    Require(core::uuid::IsEngineIdentityUuid(operation)&&core::uuid::IsEngineIdentityUuid(binding.database_uuid)&&
      core::uuid::IsEngineIdentityUuid(binding.filespace_uuid)&&disk::FindCanonicalFilespacePageProfile(binding.page_size_profile_uuid),E::invalid_request);
    Require(device.is_open()&&!device.read_only(),E::invalid_device);
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(device,&binding);
    if(!zero.ok()){
      if(zero.error==disk::FilespacePageZeroError::hash_provider_failure)throw E::hash_failure;
      if(zero.error==disk::FilespacePageZeroError::resource_exhausted)throw E::resource_exhausted;
      throw E::graph_failure;
    }
    const auto& z=*zero.record;const auto& b=z.bootstrap;
    Require(!(b.flags&disk::FilespaceBootstrapFlag::cluster_authority_required),E::cluster_requires_authority);
    Require(!(b.flags&disk::FilespaceBootstrapFlag::payload_encrypted),E::encrypted_requires_crypto_authority);
    Require(b.lifecycle_state==7&&b.filespace_role>=1&&b.filespace_role<=4&&z.creation_operation_uuid==operation&&
      z.page_generation==1&&z.root_set_generation==1&&!z.preallocated_pages&&z.creation_utc_millis&&z.creation_utc_millis<=0xffffffffffffULL);
    const u64 size=b.page_size_bytes,total=z.total_pages;
    u64 coverage=(size-384)/128;
    while(((384+(coverage+1)/2+7)&~u64{7})+128*coverage>size)--coverage;
    const u64 maps=total/coverage+(total%coverage!=0);
    Require(total&&maps<=total&&total-maps>=18&&total<=std::numeric_limits<u64>::max()/size&&
      disk::CheckFileDeviceExtent(total*size,0).ok()&&z.free_pages==total-maps-18);
    Require(budget/size>=24&&maps<=budget/size-24&&maps<=std::numeric_limits<std::size_t>::max()-18,E::resource_exhausted);
    const auto extent=device.Size();Require(extent.ok(),E::io_failure);Require(extent.size_bytes==total*size);
    std::vector<Image> images(maps+18);
    const auto read=[&](u64 n,std::vector<byte>& bytes){
      bytes.resize(size);const auto io=device.ReadAt(n*size,bytes.data(),bytes.size());
      Require(io.ok()&&io.bytes_transferred==bytes.size(),E::io_failure);
    };
    for(u64 n=0;n<images.size();++n){auto& image=images[n];read(n,image.bytes);
      const auto digest=core::hash::ComputeSha256Digest(image.bytes);Require(digest.ok(),E::hash_failure);image.sha256=digest.digest;
      if(n>=maps+16)continue; // Damaged slot headers are reconstructed only from verified allocations.
      const disk::NativeCommonPageHeaderBinding expected{binding,n,1,n==0?1:n<=maps?3:types[n-maps],{}};
      const auto header=disk::DecodeNativeCommonPageHeader(image.bytes.data()+(n==0?4096:0),128,&expected);
      Require(header.ok()&&!header.header->flags);image.header=*header.header;
    }
    {std::vector<byte> bytes;for(u64 n=images.size();n<total;++n){read(n,bytes);
      Require(std::all_of(bytes.begin(),bytes.end(),[](byte v){return !v;}));}}
    const auto ref=[&](u64 n){return disk::NativePageReference{b.filespace_uuid,n,1,b.page_size_profile_uuid};};
    const auto root=[&](u16 kind,u64 n){return disk::FilespaceRootReference{kind,images[n].header.page_type,
      b.filespace_uuid,n,1,b.page_size_profile_uuid,images[n].owner};};
    const auto same=[&](u64 n,auto encoded){Valid(encoded);Require(encoded.bytes==images[n].bytes);};
    Uuid creator,timeline;
    {const auto decoded=DecodeNativeCheckpointRoot(images[maps+1].bytes);Valid(decoded);
      creator=decoded.root->creator_transaction_uuid;timeline=decoded.root->timeline_uuid;images[maps+1].owner=decoded.root->object_uuid;}
    std::set<Uuid> ids;
    const auto unique=[&](const Uuid& id){Require(core::uuid::IsEngineIdentityUuid(id)&&ids.insert(id).second);};
    for(const auto& id:{b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid,b.checksum_profile_uuid,
      operation,z.writer_identity_uuid,creator,timeline})unique(id);
    images[0].owner=b.filespace_uuid;

    // Every expected image is derived from genesis rules and actual original
    // identities. Complete byte comparison rejects extra fields and padding.
    {const auto decoded=page::DecodeNativeTransactionInventoryPage(images[maps+2].bytes);Valid(decoded);
      auto begun=mga::BeginLocalTransaction(mga::MakeEmptyLocalTransactionInventory(),{UuidKind::transaction,creator},z.creation_utc_millis);
      Require(begun.ok());auto committed=mga::CommitLocalTransaction(std::move(begun.inventory),mga::MakeLocalTransactionId(1),z.creation_utc_millis);
      Require(committed.ok());page::NativeTransactionInventoryPage inv;
      inv.header=images[maps+2].header;inv.object_uuid=images[maps+2].owner=decoded.page->object_uuid;
      inv.inventory_generation=1;inv.inventory=std::move(committed.inventory);
      same(maps+2,page::EncodeNativeTransactionInventoryPage(inv));}
    NativeCreationWorkspaceReceipt receipt;
    receipt.database_uuid=b.database_uuid;receipt.filespace_uuid=b.filespace_uuid;receipt.operation_uuid=operation;
    receipt.page_zero_uuid=z.page_uuid;receipt.timeline_uuid=timeline;
    receipt.construction_transaction.transaction_uuid={UuidKind::transaction,creator};
    receipt.construction_transaction.local_id=mga::MakeLocalTransactionId(1);
    receipt.construction_transaction.scope=mga::TransactionScope::local_node;
    receipt.total_pages=total;receipt.free_pages=z.free_pages;receipt.map_pages=maps;
    for(unsigned i=0;i<6;++i){const u64 n=maps+10+i;
      const auto decoded=DecodeNativeCatalogLeaf(images[n].bytes);Valid(decoded);
      NativeCatalogLeafPage leaf;leaf.header=images[n].header;leaf.body.relation_uuid=decoded.page->body.relation_uuid;
      images[n].owner=leaf.body.relation_uuid.value;
      leaf.body.segment_id=leaf.body.segment_generation=leaf.body.page_generation=leaf.body.compaction_generation=1;leaf.body.page_number=n;
      same(n,EncodeNativeCatalogLeaf(leaf));receipt.relations[i]={static_cast<u16>(i+1),6,ref(n),images[n].owner};
    }
    for(unsigned control:{7u,8u,9u}){const u64 n=maps+control;
      const auto decoded=page::DecodeNativeCatalogRoot(images[n].bytes);Valid(decoded);
      page::NativeCatalogRoot c;c.header=images[n].header;c.object_uuid=images[n].owner=decoded.root->object_uuid;
      c.root_kind=control==7?2:control==8?6:7;c.creator_transaction_uuid=creator;c.creator_local_transaction_id=1;
      c.catalog_generation=c.schema_epoch=c.security_epoch=1;
      if(control==7)c.roots.assign(receipt.relations.begin(),receipt.relations.end());
      else c.roots.push_back(receipt.relations[control==8?4:3]);
      same(n,page::EncodeNativeCatalogRoot(c));
    }
    {const u64 n=maps+3;const auto decoded=page::DecodeNativeFilespaceDirectory(images[n].bytes);Valid(decoded);
      Require(decoded.directory->records.size()==1);const auto locator=decoded.directory->records.front().locator_uuid;unique(locator);
      page::NativeFilespaceDirectory d;d.header=images[n].header;d.object_uuid=images[n].owner=decoded.directory->object_uuid;
      d.directory_generation=1;d.creator_transaction_uuid=creator;d.creator_local_transaction_id=1;d.total_records=1;
      d.records.push_back({b,locator,z.page_uuid,1,1,total,1,{}});same(n,page::EncodeNativeFilespaceDirectory(d));}
    {const u64 n=maps+4;const auto decoded=DecodeNativeSystemState(images[n].bytes);Valid(decoded);
      NativeSystemState s;s.header=images[n].header;s.object_uuid=images[n].owner=decoded.state->object_uuid;
      s.state_generation=s.restart_generation=s.startup_counter=1;s.creator_transaction_uuid=creator;s.creator_local_transaction_id=1;
      s.checkpoint_generation=1;s.checkpoint=ref(maps+1);s.checkpoint_object_uuid=images[maps+1].owner;s.transition_operation_uuid=operation;
      same(n,EncodeNativeSystemState(s));}
    {const u64 n=maps+5;const auto decoded=page::DecodeNativeRetentionPage(images[n].bytes);Valid(decoded);
      page::NativeRetentionPage r;r.header=images[n].header;r.object_uuid=images[n].owner=decoded.page->object_uuid;
      r.epoch=1;r.creator_transaction_uuid=creator;r.creator_local_transaction_id=1;same(n,page::EncodeNativeRetentionPage(r));}
    {const u64 n=maps+6;const auto decoded=page::DecodeNativeHorizonRoot(images[n].bytes);Valid(decoded);Require(decoded.root->records.size()==3);
      page::NativeHorizonRoot h;h.header=images[n].header;h.object_uuid=images[n].owner=decoded.root->object_uuid;
      h.epoch=1;h.creator_transaction_uuid=creator;h.creator_local_transaction_id=1;h.total_records=3;
      h.retention=ref(maps+5);h.retention_object_uuid=images[maps+5].owner;h.retention_sha256=images[maps+5].sha256;
      for(unsigned i=0;i<3;++i){page::NativeHorizonRecord r;r.kind=static_cast<page::NativeHorizonKind>(i+1);
        r.local_boundary=2;r.owner_uuid=z.writer_identity_uuid;r.timeline_uuid=timeline;
        r.horizon_uuid=decoded.root->records[i].horizon_uuid;unique(r.horizon_uuid);h.records.push_back(r);}
      same(n,page::EncodeNativeHorizonRoot(h));}

    Uuid map_owner;
    for(u64 n=1;n<=maps;++n){const auto decoded=page::DecodeNativeAllocationMap(images[n].bytes);Valid(decoded);const auto& actual=*decoded.map;
      if(n==1)map_owner=actual.object_uuid;images[n].owner=map_owner;
      page::NativeAllocationMap m;m.header=images[n].header;m.object_uuid=map_owner;m.map_generation=m.capacity_generation=1;
      m.total_pages=total;m.first_page=(n-1)*coverage;m.creator_transaction_uuid=creator;m.creator_local_transaction_id=1;
      const auto covered=std::min(coverage,total-m.first_page);m.states.assign(covered,page::NativeAllocationState::free);
      const auto end=std::min(m.first_page+covered,static_cast<u64>(images.size()));
      Require(actual.records.size()==(end>m.first_page?end-m.first_page:0));
      for(u64 p=m.first_page;p<end;++p){const auto& r=actual.records[p-m.first_page];
        Require(r.page_number==p&&r.creator_transaction_uuid==creator&&r.creator_local_transaction_id==1&&
          r.page_generation==1&&!r.reuse_horizon&&r.page_type==(p==0?1:p<=maps?3:types[p-maps]));
        unique(r.allocation_uuid);unique(r.page_uuid);images[p].allocation=r;
        m.states[p-m.first_page]=page::NativeAllocationState::allocated;m.records.push_back(r);
      }
      if(n<maps){m.next=ref(n+1);m.next_sha256=images[n+1].sha256;}same(n,page::EncodeNativeAllocationMap(m));
    }
    // Validate selectors only after the complete non-selector graph is known.
    std::optional<NativeCheckpointSelection> survivor;
    std::array<bool,2> valid{};
    for(unsigned i=0;i<2;++i){auto& image=images[maps+16+i];const auto& allocation=image.allocation;
      image.header={b.page_size_bytes,0x30e,b.database_uuid,b.filespace_uuid,allocation.page_uuid,maps+16+i,1,0,b.page_size_profile_uuid};
      image.owner=allocation.owner_uuid;
      const auto decoded=DecodeNativeCheckpointSelection(image.bytes);
      if(decoded.error==NativeCheckpointSelectionError::hash_failure)throw E::hash_failure;
      if(decoded.error==NativeCheckpointSelectionError::resource_exhausted)throw E::resource_exhausted;
      if(decoded.ok()){valid[i]=true;if(!survivor)survivor=*decoded.selection;}
    }
    Require(survivor.has_value(),E::selector_ambiguity);unique(survivor->publication_uuid);
    Require(images[maps+16].owner==images[maps+17].owner);
    unique(map_owner);unique(images[maps+16].owner);
    for(u64 n=maps+1;n<maps+16;++n)unique(images[n].owner);
    for(u64 n=0;n<images.size();++n){const auto& image=images[n];const auto& a=image.allocation;
      Require(a.owner_uuid==image.owner&&a.page_uuid==image.header.page_uuid&&a.page_type==image.header.page_type);}
    Require(images[0].header.page_uuid==z.page_uuid);

    {NativeCheckpointRoot cp;cp.header=images[maps+1].header;cp.object_uuid=images[maps+1].owner;
      cp.checkpoint_generation=cp.root_set_generation=cp.selected_local_transaction_id=1;
      cp.stable_local_transaction_id=cp.local_durable_transaction_id=cp.creator_local_transaction_id=1;
      cp.timeline_uuid=timeline;cp.creator_transaction_uuid=creator;cp.completed=true;
      const std::array<u64,10> roots{maps+2,maps+6,maps+3,1,maps+7,maps+8,maps+9,maps+4,maps+7,maps+5};
      for(unsigned i=0;i<roots.size();++i){const auto n=roots[i];cp.roots.push_back({static_cast<u16>(i+1),
        images[n].header.page_type,ref(n),images[n].owner,images[n].sha256});}
      same(maps+1,EncodeNativeCheckpointRoot(cp));}
    {auto expected=z;expected.roots.clear();const std::array<u64,12> roots{
        maps+4,maps+7,1,maps+2,maps+3,maps+8,maps+9,maps+7,maps+1,maps+5,maps+16,maps+17};
      for(unsigned i=0;i<roots.size();++i)expected.roots.push_back(root(i<10?i+1:i+8,roots[i]));
      const auto encoded=disk::EncodeFilespacePageZero(expected);
      if(!encoded.ok())throw encoded.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:
        encoded.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:E::encoding_failure;
      Require(*encoded.bytes==images[0].bytes);}
    std::array<std::vector<byte>,2> expected;
    receipt.publication_uuid=survivor->publication_uuid;receipt.checkpoint=root(9,maps+1);receipt.checkpoint_sha256=images[maps+1].sha256;
    unsigned repaired=0;
    for(unsigned i=0;i<2;++i){const auto& image=images[maps+16+i];NativeCheckpointSelection s;
      s.header=image.header;s.object_uuid=image.owner;s.bootstrap_uuid=z.page_uuid;s.publication_uuid=receipt.publication_uuid;
      s.selection_generation=s.checkpoint_generation=s.root_set_generation=1;s.checkpoint=ref(maps+1);
      s.checkpoint_object_uuid=receipt.checkpoint.object_uuid;s.checkpoint_sha256=receipt.checkpoint_sha256;s.timeline_uuid=timeline;
      auto encoded=EncodeNativeCheckpointSelection(s);Valid(encoded);expected[i]=std::move(encoded.bytes);
      Require(!valid[i]||image.bytes==expected[i],E::selector_ambiguity);
      if(image.bytes!=expected[i])repaired|=1u<<i;
    }
    // Neither guessed headers nor valid-but-conflicting history may reach writes.
    std::vector<byte> scratch;
    for(unsigned i=0;i<2;++i){read(maps+16+i,scratch);Require(scratch==images[maps+16+i].bytes,E::preimage_changed);}
    Require(device.Sync().ok(),E::io_failure);
    for(unsigned i=0;i<2;++i){const u64 n=maps+16+i;
      if(repaired&(1u<<i)){const auto io=device.WriteAt(n*size,expected[i].data(),expected[i].size());
        Require(io.ok()&&io.bytes_transferred==expected[i].size(),E::io_failure);}
      Require(device.Sync().ok(),E::io_failure);read(n,scratch);Require(scratch==expected[i],E::readback_mismatch);
    }
    // Release all prepared images before the normal actual-device admission.
    images.clear();for(auto& bytes:expected)std::vector<byte>().swap(bytes);std::vector<byte>().swap(scratch);
    const std::vector<disk::NativeFilespaceDevice> devices{{b.filespace_uuid,b.page_size_profile_uuid,&device}};
    const auto admitted=ReadNativeBoundCheckpointSelectionFromOpenDevices(b.database_uuid,devices,b.filespace_uuid,budget);
    Require(admitted.ok(),E::admission_failure);
    NativeCreationRecoveryResult result;result.error=E::none;result.receipt=std::move(receipt);result.repaired_slots=repaired;return result;
  }catch(E error){return Fail(error);}
   catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
   catch(...){return Fail(E::io_failure);}
}
}  // namespace scratchbird::storage::database

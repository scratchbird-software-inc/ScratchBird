// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_control_authority.hpp"
#include "native_management_control_allocation.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "transaction_inventory_validation.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <mutex>
#include <set>
#include <stdexcept>

namespace scratchbird::storage::database {
namespace {
using E=NativeManagementControlAuthorityError;
using Bytes=std::vector<byte>;
using Pages=std::vector<Bytes>;
namespace mga=transaction::mga;
using State=page::NativeAllocationState;
void Require(bool ok,E e){if(!ok)throw e;}
NativeManagementControlAuthority Fail(E e){NativeManagementControlAuthority r;r.error=e;return r;}
auto Self(const disk::NativeCommonPageHeader& h){return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
auto Hash(const Bytes& b){const auto r=core::hash::ComputeSha256Digest(b);Require(r.ok(),E::hash_failure);return r.digest;}
const NativeCheckpointRootReference& Root(const NativeCheckpointRoot& cp,core::platform::u16 role){const auto r=std::find_if(cp.roots.begin(),cp.roots.end(),[&](const auto& v){return v.role==role;});Require(r!=cp.roots.end(),E::binding_mismatch);return *r;}
void PlanError(NativePublicationPlanError e){if(e==NativePublicationPlanError::none)return;throw e==NativePublicationPlanError::hash_failure?E::hash_failure:e==NativePublicationPlanError::resource_exhausted?E::resource_exhausted:e==NativePublicationPlanError::cluster_requires_authority?E::cluster_requires_authority:E::binding_mismatch;}
void MapError(page::NativeAllocationError e){if(e==page::NativeAllocationError::none)return;throw e==page::NativeAllocationError::hash_failure?E::hash_failure:e==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:e==page::NativeAllocationError::io_failure?E::io_failure:e==page::NativeAllocationError::cluster_requires_authority?E::cluster_requires_authority:E::allocation_failure;}
struct Checkpoint {NativeCheckpointRoot root;Bytes bytes;std::array<byte,32> sha;};
struct Context {
  Uuid database,primary;u64 budget=0,used=0;
  std::vector<disk::NativeFilespaceDevice> files;
  std::vector<std::unique_lock<std::recursive_mutex>> guards;
  void Charge(u64 count,u64 size=1){Require(size&&count<=(budget-used)/size,E::resource_exhausted);used+=count*size;}
  const disk::NativeFilespaceDevice& File(const Uuid& id){const auto it=std::lower_bound(files.begin(),files.end(),id,[](const auto& f,const auto& v){return f.filespace_uuid<v;});Require(it!=files.end()&&it->filespace_uuid==id,E::invalid_request);return *it;}
  Checkpoint Read(const disk::NativePageReference& ref,const Uuid& object,const std::array<byte,32>& sha){
    const auto& f=File(ref.filespace_uuid);Require(f.page_size_profile_uuid==ref.page_size_profile_uuid,E::binding_mismatch);
    Charge(3,disk::FindCanonicalFilespacePageProfile(ref.page_size_profile_uuid)->page_size_bytes);
    const disk::FilespaceRootReference r{9,0x300,ref.filespace_uuid,ref.page_number,ref.page_generation,ref.page_size_profile_uuid,object};
    auto cp=ReadNativeCheckpointRootFromOpenDevice(*f.device,database,r);
    if(!cp.ok())throw cp.error==NativeCheckpointError::hash_failure?E::hash_failure:cp.error==NativeCheckpointError::resource_exhausted?E::resource_exhausted:cp.error==NativeCheckpointError::io_failure?E::io_failure:cp.error==NativeCheckpointError::encrypted_requires_crypto_authority?E::encrypted_requires_authority:E::checkpoint_failure;
    Require(cp.root->completed,E::checkpoint_failure);Require(Hash(cp.bytes)==sha,E::binding_mismatch);return {std::move(*cp.root),std::move(cp.bytes),sha};
  }
  page::NativeAllocationChainResult Maps(const Uuid& fs,const NativeCheckpointRootReference* ref=nullptr){
    const auto& f=File(fs);const disk::FilespaceBootstrapBinding binding{database,fs,f.page_size_profile_uuid};
    page::NativeAllocationChainResult out;
    if(ref){Require(ref->page.filespace_uuid==fs&&ref->page.page_size_profile_uuid==f.page_size_profile_uuid,E::binding_mismatch);
      const disk::FilespaceRootReference r{3,3,fs,ref->page.page_number,ref->page.page_generation,ref->page.page_size_profile_uuid,ref->object_uuid};out=page::ReadNativeAllocationChainAtRootFromOpenDevice(*f.device,binding,r,budget-used);
    }else out=page::ReadNativeAllocationChainFromOpenDevice(*f.device,binding,budget-used);
    MapError(out.error);Charge(out.retained_image_bytes);if(ref)Require(Hash(out.pages.front().bytes)==ref->sha256,E::binding_mismatch);return out;
  }
  page::NativeFilespaceDirectoryChainResult Directory(const NativeCheckpointRoot& cp){
    const auto& root=Root(cp,3);const disk::FilespaceRootReference r{5,root.page_type,root.page.filespace_uuid,
      root.page.page_number,root.page.page_generation,root.page.page_size_profile_uuid,root.object_uuid};
    auto out=page::ReadNativeFilespaceDirectoryFromOpenDevices(database,files,r,budget-used);
    if(!out.ok()){using D=page::NativeDirectoryError;throw out.error==D::resource_exhausted?E::resource_exhausted:
      out.error==D::hash_failure?E::hash_failure:out.error==D::io_failure?E::io_failure:E::binding_mismatch;}
    Charge(out.retained_image_bytes);Require(Hash(out.pages.front().bytes)==root.sha256,E::binding_mismatch);return out;
  }
  page::NativeTransactionInventoryChainResult Inventory(const NativeCheckpointRoot& cp){
    const auto& root=Root(cp,1);const disk::FilespaceRootReference r{4,root.page_type,root.page.filespace_uuid,root.page.page_number,root.page.page_generation,root.page.page_size_profile_uuid,root.object_uuid};
    auto out=page::ReadNativeTransactionInventoryChainFromOpenDevices(database,files,r,budget-used);
    if(!out.ok())throw out.error==page::NativeInventoryError::hash_failure?E::hash_failure:out.error==page::NativeInventoryError::resource_exhausted?E::resource_exhausted:out.error==page::NativeInventoryError::io_failure?E::io_failure:out.error==page::NativeInventoryError::encrypted_requires_crypto_authority?E::encrypted_requires_authority:E::inventory_failure;
    Charge(out.retained_image_bytes);Require(Hash(out.pages.front().bytes)==root.sha256&&out.inventory.next_local_transaction_id&&out.inventory.next_local_transaction_id-1==cp.selected_local_transaction_id,E::binding_mismatch);return out;
  }
};
void Transaction(const mga::LocalTransactionInventory& inventory,const Uuid& id,u64 local,bool committed){
  const auto tx=mga::LookupLocalTransaction(inventory,mga::MakeLocalTransactionId(local));Require(tx.ok()&&tx.entry.identity.transaction_uuid.value==id&&tx.entry.identity.scope==mga::TransactionScope::local_node&&(!committed||mga::HasCommittedInventoryOutcome(tx.entry)),E::creator_mismatch);
}
const page::NativeAllocationRecord& Allocated(const page::NativeAllocationChainResult& chain,const disk::NativeCommonPageHeader& h,const Uuid& owner){
  auto m=std::upper_bound(chain.pages.begin(),chain.pages.end(),h.page_number,[](u64 n,const auto& p){return n<p.map->first_page;});Require(m!=chain.pages.begin(),E::allocation_failure);--m;const auto& map=*m->map;
  Require(h.page_number-map.first_page<map.states.size()&&map.states[h.page_number-map.first_page]==State::allocated,E::allocation_failure);
  const auto r=std::lower_bound(map.records.begin(),map.records.end(),h.page_number,[](const auto& r,u64 n){return r.page_number<n;});Require(r!=map.records.end()&&r->page_number==h.page_number&&r->page_uuid==h.page_uuid&&r->page_generation==h.page_generation&&r->page_type==h.page_type&&r->owner_uuid==owner,E::binding_mismatch);return *r;
}
void Owners(const page::NativeAllocationChainResult& chain,const mga::LocalTransactionInventory& inventory,const NativeManagementControlAuthority& proof){
  for(const auto& image:chain.pages){const auto& m=*image.map;
    if(m.creator_operation_uuid.is_nil())Transaction(inventory,m.creator_transaction_uuid,m.creator_local_transaction_id,true);
    else Require(MatchesNativeManagementControlMap(proof,m),E::creator_mismatch);
    for(const auto& r:m.records){if(r.creator_operation_uuid.is_nil())Transaction(inventory,r.creator_transaction_uuid,r.creator_local_transaction_id,false);
      else Require(MatchesNativeManagementControlAllocation(proof,m.header.filespace_uuid,r,m.states[r.page_number-m.first_page]),E::creator_mismatch);}
  }
}
page::NativeTransactionInventoryChainResult Controls(Context& c,const Checkpoint& cp,const page::NativeAllocationChainResult& maps,const NativeManagementControlAuthority& proof,const Pages* expected_inventory=nullptr){
  auto inventory=c.Inventory(cp.root);
  if(expected_inventory){Require(inventory.pages.size()==expected_inventory->size(),E::binding_mismatch);for(std::size_t i=0;i<inventory.pages.size();++i)Require(inventory.pages[i].bytes==(*expected_inventory)[i],E::binding_mismatch);}
  if(cp.root.creator_operation_uuid.is_nil())Transaction(inventory.inventory,cp.root.creator_transaction_uuid,cp.root.creator_local_transaction_id,true);
  else Require(MatchesNativeManagementPublishedCheckpoint(proof,cp.root,cp.sha),E::creator_mismatch);
  Owners(maps,inventory.inventory,proof);
  std::set<Uuid> allocation_ids,page_ids;
  const auto unique=[&](const page::NativeAllocationChainResult& chain){for(const auto& image:chain.pages)for(const auto& r:image.map->records){
    Require(allocation_ids.insert(r.allocation_uuid).second,E::binding_mismatch);
    if(!r.page_uuid.is_nil())Require(page_ids.insert(r.page_uuid).second,E::binding_mismatch);}};
  unique(maps);
  std::vector<NativeInventoryPageBinding> required{{cp.root.header,cp.root.object_uuid}};
  for(const auto& image:inventory.pages)required.push_back({image.page->header,image.page->object_uuid});
  auto directory=c.Directory(cp.root);const auto& creator=*directory.pages.front().directory;
  if(creator.creator_operation_uuid.is_nil())Transaction(inventory.inventory,creator.creator_transaction_uuid,creator.creator_local_transaction_id,true);
  else{
    Require(MatchesNativeManagementPublishedDirectory(proof,directory,Root(cp.root,3).sha256),E::creator_mismatch);
    for(const auto& image:directory.pages)required.push_back({image.directory->header,image.directory->object_uuid});
  }
    for(const auto& image:directory.pages)for(const auto& entry:image.directory->records){if(entry.bootstrap.filespace_uuid!=c.primary||!entry.allocation_root)continue;
      const auto& r=*entry.allocation_root;const auto& root=Root(cp.root,4);const auto& map=*maps.pages.front().map;
      Require(r.page==root.page&&r.object_uuid==root.object_uuid&&r.sha256==root.sha256&&
        r.map_generation==map.map_generation&&r.capacity_generation==map.capacity_generation&&entry.total_pages==map.total_pages,E::binding_mismatch);}
  std::size_t retained_allocations=0;
  for(const auto& file:c.files){if(std::none_of(required.begin(),required.end(),[&](const auto& v){return v.header.filespace_uuid==file.filespace_uuid;})&&
      std::none_of(proof.allocations.begin(),proof.allocations.end(),[&](const auto& entry){return entry.first.first==file.filespace_uuid;}))continue;
    page::NativeAllocationChainResult other;const auto* chain=&maps;if(file.filespace_uuid!=c.primary){
      const page::NativeFilespaceDirectoryRecord* member=nullptr;
      for(const auto& image:directory.pages)for(const auto& entry:image.directory->records)if(entry.bootstrap.filespace_uuid==file.filespace_uuid)member=&entry;
      Require(member,E::binding_mismatch);
      if(member->allocation_root){const auto& r=*member->allocation_root;const NativeCheckpointRootReference root{4,3,r.page,r.object_uuid,r.sha256};
        other=c.Maps(file.filespace_uuid,&root);const auto& map=*other.pages.front().map;
        Require(map.map_generation==r.map_generation&&map.capacity_generation==r.capacity_generation&&map.total_pages==member->total_pages,E::binding_mismatch);
      }else other=c.Maps(file.filespace_uuid);
      Owners(other,inventory.inventory,proof);unique(other);chain=&other;}
    for(const auto& target:required)if(target.header.filespace_uuid==file.filespace_uuid){const auto& r=Allocated(*chain,target.header,target.object_uuid);
      if(r.creator_operation_uuid.is_nil())Transaction(inventory.inventory,r.creator_transaction_uuid,r.creator_local_transaction_id,true);
      else Require(MatchesNativeManagementControlAllocation(proof,file.filespace_uuid,r,State::allocated),E::creator_mismatch);}
    for(const auto& [key,record]:proof.allocations){if(key.first!=file.filespace_uuid)continue;auto expected=chain->pages.front().map->header;
      expected.page_number=record.page_number;expected.page_uuid=record.page_uuid;expected.page_generation=record.page_generation;expected.page_type=record.page_type;
      Require(Allocated(*chain,expected,record.owner_uuid)==record,E::binding_mismatch);++retained_allocations;}
  }
  Require(retained_allocations==proof.allocations.size(),E::binding_mismatch);
  return inventory;
}
Pages Images(const page::NativeAllocationChainResult& maps){Pages out;out.reserve(maps.pages.size());for(const auto& p:maps.pages)out.push_back(p.bytes);return out;}
}
bool MatchesNativeManagementPublishedCheckpoint(const NativeManagementControlGraph& proof,const NativeCheckpointRoot& cp,const std::array<byte,32>& sha) noexcept {
  const auto p=proof.publications.find(cp.creator_operation_uuid);return p!=proof.publications.end()&&p->second.page==Self(cp.header)&&p->second.object_uuid==cp.object_uuid&&p->second.sha256==sha&&cp.creator_transaction_uuid.is_nil()&&!cp.creator_local_transaction_id;
}
bool MatchesNativeManagementPublishedDirectory(const NativeManagementControlGraph& proof,const page::NativeFilespaceDirectoryChainResult& chain,const std::array<byte,32>& sha) noexcept {
  if(!chain.ok()||!chain.pages.front().ok())return false;
  const auto& head=*chain.pages.front().directory;const auto operation=head.creator_operation_uuid;
  if(!core::uuid::IsEngineIdentityUuid(operation))return false;
  const auto p=proof.publications.find(operation);if(p==proof.publications.end())return false;
  const auto& root=p->second.directory_root;
  if(root.role!=3||root.page_type!=9||root.page!=Self(head.header)||root.object_uuid!=head.object_uuid||root.sha256!=sha)return false;
  for(const auto& image:chain.pages){if(!image.ok())return false;const auto& d=*image.directory;const auto& h=d.header;
    if(d.creator_operation_uuid!=operation||!d.creator_transaction_uuid.is_nil()||d.creator_local_transaction_id||h.page_type!=9)return false;
    const auto a=proof.allocations.find({h.filespace_uuid,h.page_number});if(a==proof.allocations.end())return false;const auto& r=a->second;
    if(r.page_number!=h.page_number||r.page_uuid!=h.page_uuid||r.page_generation!=h.page_generation||r.page_type!=9||r.owner_uuid!=d.object_uuid||
       r.creator_operation_uuid!=operation||!r.creator_transaction_uuid.is_nil()||r.creator_local_transaction_id)return false;
  }
  return true;
}
bool MatchesNativeManagementControlAllocation(const NativeManagementControlGraph& proof,const Uuid& fs,const page::NativeAllocationRecord& r,State state) noexcept {
  const auto p=proof.allocations.find({fs,r.page_number});return state==State::allocated&&p!=proof.allocations.end()&&p->second==r&&!r.creator_operation_uuid.is_nil();
}
bool MatchesNativeManagementControlMap(const NativeManagementControlGraph& proof,const page::NativeAllocationMap& map) noexcept {
  const auto p=proof.allocations.find({map.header.filespace_uuid,map.header.page_number});if(p==proof.allocations.end())return false;const auto& r=p->second;
  return !map.creator_operation_uuid.is_nil()&&map.creator_transaction_uuid.is_nil()&&!map.creator_local_transaction_id&&r.creator_operation_uuid==map.creator_operation_uuid&&r.page_uuid==map.header.page_uuid&&r.page_generation==map.header.page_generation&&r.page_type==map.header.page_type&&r.owner_uuid==map.object_uuid;
}
namespace {
NativeManagementControlAuthority ReadControlGraph(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& supplied,const Uuid& primary,u64 budget,const NativeManagementCheckpointAnchor* requested) noexcept {
  try{
    Require(core::uuid::IsEngineIdentityUuid(database)&&core::uuid::IsEngineIdentityUuid(primary)&&!supplied.empty(),E::invalid_request);Context c;c.database=database;c.primary=primary;c.budget=budget;c.files=supplied;
    std::sort(c.files.begin(),c.files.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});std::set<disk::FileDevice*> devices;c.guards.reserve(c.files.size());
    for(std::size_t i=0;i<c.files.size();++i){const auto& f=c.files[i];Require(core::uuid::IsEngineIdentityUuid(f.filespace_uuid)&&disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid)&&f.device&&devices.insert(f.device).second&&(!i||c.files[i-1].filespace_uuid!=f.filespace_uuid),E::invalid_request);c.guards.push_back(f.device->AcquireOperationGuard());}
    NativeManagementGraphHistory history;std::optional<NativeCheckpointSelection> selected;
    if(requested)history=ReadNativeManagementGraphHistoryFromOpenDevices(database,c.files,primary,*requested,budget);
    else{auto actual=ReadNativeManagementHistoryFromOpenDevices(database,c.files,primary,budget);selected=actual.selection;history=std::move(static_cast<NativeManagementGraphHistory&>(actual));}
    if(!history.ok())throw history.error==NativeManagementHistoryError::hash_failure?E::hash_failure:history.error==NativeManagementHistoryError::resource_exhausted?E::resource_exhausted:history.error==NativeManagementHistoryError::io_failure?E::io_failure:history.error==NativeManagementHistoryError::encrypted_requires_authority?E::encrypted_requires_authority:history.error==NativeManagementHistoryError::cluster_requires_authority?E::cluster_requires_authority:E::history_failure;
    c.Charge(history.verified_image_bytes);NativeManagementControlAuthority result;const u64 size=disk::FindCanonicalFilespacePageProfile(c.File(primary).page_size_profile_uuid)->page_size_bytes;
    const Pages* expected_inventory=nullptr;
    for(const auto& entry:history.entries){const auto& p=entry.plan;Require(p.control_bundle&&p.management_extent,E::binding_mismatch);
      auto base=c.Read(p.base_checkpoint,p.base_checkpoint_object_uuid,p.base_checkpoint_sha256);auto target=c.Read(p.target_checkpoint,p.target_checkpoint_object_uuid,entry.checkpoint_sha256);
      auto before=c.Maps(primary,&Root(base.root,4));auto before_inventory=Controls(c,base,before,result,expected_inventory);auto after=c.Maps(primary,&Root(target.root,4));
      Require(after.pages.size()==entry.control_allocation_images.size(),E::binding_mismatch);for(std::size_t i=0;i<after.pages.size();++i)Require(after.pages[i].bytes==entry.control_allocation_images[i],E::binding_mismatch);
      c.Charge(2,size);auto plan=EncodeNativePublicationPlan(p);PlanError(plan.error);Require(plan.sha256==entry.plan_sha256,E::binding_mismatch);
      auto start=c.used;c.Charge(p.management_extent->page_count,size);c.Charge(p.management_extent->aggregate_bytes,4);c.Charge(2,size);
      auto extent=EncodeNativeManagementExtent(entry.record,p.management_extent->object_uuid,entry.extent_pages,c.used-start);
      if(!extent.ok())throw extent.error==NativeManagementExtentError::hash_failure?E::hash_failure:extent.error==NativeManagementExtentError::resource_exhausted?E::resource_exhausted:E::binding_mismatch;
      Require(extent.root==p.management_extent,E::binding_mismatch);
      start=c.used;c.Charge(p.control_bundle->page_count,size);c.Charge(p.control_bundle->map_count,4*size);c.Charge(p.control_bundle->inventory_count,4*size);c.Charge(2,size);
      auto bundle=EncodeNativeManagementControlBundle(entry.control_allocation_images,database,p.bootstrap_uuid,p.control_bundle->object_uuid,p.operation_uuid,entry.bundle_pages,c.used-start,entry.control_inventory_images);
      if(!bundle.ok())throw bundle.error==NativeManagementControlBundleError::hash_failure?E::hash_failure:bundle.error==NativeManagementControlBundleError::resource_exhausted?E::resource_exhausted:E::binding_mismatch;
      Require(bundle.root==p.control_bundle,E::binding_mismatch);
      c.Charge(before.retained_image_bytes);c.Charge(after.retained_image_bytes);auto before_bytes=Images(before);auto after_bytes=Images(after);
      Pages before_inventory_bytes;
      if(p.intent.recovery_profile==2){for(const auto& raw:before_inventory.pages)c.Charge(4,raw.bytes.size());for(const auto& raw:entry.control_inventory_images)c.Charge(4,raw.size());before_inventory_bytes.reserve(before_inventory.pages.size());for(const auto& raw:before_inventory.pages)before_inventory_bytes.push_back(raw.bytes);expected_inventory=&entry.control_inventory_images;}
      const auto delta=ValidateNativeManagementControlAllocation(base.bytes,target.bytes,plan.bytes,extent.pages,before_bytes,after_bytes,budget,bundle.pages,before_inventory_bytes);
      if(delta!=NativeManagementControlAllocationError::none)throw delta==NativeManagementControlAllocationError::hash_failure?E::hash_failure:delta==NativeManagementControlAllocationError::resource_exhausted?E::resource_exhausted:delta==NativeManagementControlAllocationError::cluster_requires_authority?E::cluster_requires_authority:delta==NativeManagementControlAllocationError::encrypted_requires_authority?E::encrypted_requires_authority:E::binding_mismatch;
      Require(result.publications.emplace(p.operation_uuid,NativeManagementPublishedCheckpoint{p.target_checkpoint,p.target_checkpoint_object_uuid,entry.checkpoint_sha256,Root(target.root,3)}).second,E::binding_mismatch);
      for(const auto& image:after.pages)for(const auto& record:image.map->records)if(record.creator_operation_uuid==p.operation_uuid)Require(result.allocations.emplace(std::make_pair(primary,record.page_number),record).second,E::binding_mismatch);
      Controls(c,target,after,result,expected_inventory);
    }
    const auto& anchor=*history.anchor;auto current=c.Read(anchor.checkpoint,anchor.checkpoint_object_uuid,anchor.checkpoint_sha256);auto maps=c.Maps(primary,&Root(current.root,4));Controls(c,current,maps,result,expected_inventory);
    result.anchor=anchor;result.selection=selected;result.verified_image_bytes=c.used;result.error=E::none;return result;
  }catch(E e){return Fail(e);}catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::binding_mismatch);}
}
} // namespace
NativeManagementControlAuthority ReadNativeManagementControlAuthorityFromOpenDevices(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,u64 budget) noexcept {
  return ReadControlGraph(database,devices,primary,budget,nullptr);
}
NativeManagementControlGraph ReadNativeManagementControlGraphFromOpenDevices(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,const NativeManagementCheckpointAnchor& anchor,u64 budget) noexcept {
  auto result=ReadControlGraph(database,devices,primary,budget,&anchor);
  return std::move(static_cast<NativeManagementControlGraph&>(result));
}
} // namespace scratchbird::storage::database

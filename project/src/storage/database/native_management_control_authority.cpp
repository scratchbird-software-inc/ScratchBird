// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_control_authority.hpp"
#include "native_control_allocation_backing.hpp"
#include "native_management_history_backing.hpp"
#include "native_management_control_authority_backing.hpp"
#include "native_metadata_decode_scratch.hpp"
#include "native_allocation_chain_backing.hpp"
#include "native_directory_chain_backing.hpp"
#include "native_inventory_chain_backing.hpp"
#include "native_decoded_storage_ranges.hpp"
#include "hash_digest_parts.hpp"
#include "transaction_inventory_validation.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <iterator>
#include <mutex>
#include <set>
#include <stdexcept>

namespace scratchbird::storage::database {
namespace {
using E=NativeManagementControlAuthorityError;
using Image=std::span<const byte>;
using Images=std::span<const Image>;
using Pages=std::pmr::vector<Image>;
using Scratch=detail::NativeMetadataScratch;
namespace mga=transaction::mga;
using State=page::NativeAllocationState;
using Batch=disk::FileDevice::ReadLatencyBatch;
using Key=std::pair<Uuid,u64>;
void Require(bool ok,E error){if(!ok)throw error;}
NativeManagementControlAuthority Fail(E e){NativeManagementControlAuthority r;r.error=e;return r;}
NativeManagementControlAuthorityView FailView(E e){NativeManagementControlAuthorityView r;r.error=e;return r;}
auto Self(const disk::NativeCommonPageHeader& h){return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
auto Hash(Image b){const auto r=core::hash::ComputeSha256DigestNative(b.data(),b.size());Require(r.ok(),E::hash_failure);return r.digest;}
bool Equal(Image a,Image b){return std::equal(a.begin(),a.end(),b.begin(),b.end());}
bool EqualPages(const auto& a,const auto& b){return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin(),[](Image x,Image y){return Equal(x,y);});}
const NativeCheckpointRootReference& Root(const auto& cp,core::platform::u16 role){const auto r=std::find_if(cp.roots.begin(),cp.roots.end(),[&](const auto& v){return v.role==role;});Require(r!=cp.roots.end(),E::binding_mismatch);return *r;}
void MapError(page::NativeAllocationError e){if(e==page::NativeAllocationError::none)return;throw e==page::NativeAllocationError::hash_failure?E::hash_failure:e==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:e==page::NativeAllocationError::io_failure?E::io_failure:e==page::NativeAllocationError::cluster_requires_authority?E::cluster_requires_authority:E::allocation_failure;}
template<class Values,class K> auto Find(const Values& values,const K& key){
  if constexpr(requires{values.find(key);})return values.find(key);
  else{auto at=std::lower_bound(values.begin(),values.end(),key,[](const auto& v,const auto& k){return v.first<k;});
    return at!=values.end()&&at->first==key?at:values.end();}
}
const auto& DirectoryValue(const auto& image){if constexpr(requires{image.directory.has_value();})return *image.directory;else return image.directory;}
bool PageValid(const auto& image){if constexpr(requires{image.ok();})return image.ok();else return true;}
struct Proof{
  std::pmr::map<Uuid,NativeManagementPublishedCheckpoint> publications;
  std::pmr::map<Key,page::NativeAllocationRecord> allocations,preallocations;
  explicit Proof(std::pmr::memory_resource* r):publications(r),allocations(r),preallocations(r){}
};
bool MatchesCheckpoint(const auto& proof,const auto& cp,const std::array<byte,32>& sha) noexcept {
  const auto p=Find(proof.publications,cp.creator_operation_uuid);return p!=proof.publications.end()&&p->second.page==Self(cp.header)&&p->second.object_uuid==cp.object_uuid&&p->second.sha256==sha&&cp.creator_transaction_uuid.is_nil()&&!cp.creator_local_transaction_id;
}
bool MatchesDirectory(const auto& proof,const auto& chain,const std::array<byte,32>& sha) noexcept {
  if(!chain.ok()||!PageValid(chain.pages.front()))return false;
  const auto& head=DirectoryValue(chain.pages.front());const auto operation=head.creator_operation_uuid;
  if(!core::uuid::IsEngineIdentityUuid(operation))return false;
  const auto p=Find(proof.publications,operation);if(p==proof.publications.end())return false;
  const auto& root=p->second.directory_root;
  if(root.role!=3||root.page_type!=9||root.page!=Self(head.header)||root.object_uuid!=head.object_uuid||root.sha256!=sha)return false;
  for(const auto& image:chain.pages){if(!PageValid(image))return false;const auto& d=DirectoryValue(image);const auto& h=d.header;
    if(d.creator_operation_uuid!=operation||!d.creator_transaction_uuid.is_nil()||d.creator_local_transaction_id||h.page_type!=9)return false;
    const auto a=Find(proof.allocations,std::make_pair(h.filespace_uuid,h.page_number));if(a==proof.allocations.end())return false;const auto& r=a->second;
    if(r.page_number!=h.page_number||r.page_uuid!=h.page_uuid||r.page_generation!=h.page_generation||r.page_type!=9||r.owner_uuid!=d.object_uuid||
       r.creator_operation_uuid!=operation||!r.creator_transaction_uuid.is_nil()||r.creator_local_transaction_id)return false;
  }
  return true;
}
bool MatchesAllocation(const auto& proof,const Uuid& fs,const page::NativeAllocationRecord& r,State state) noexcept {
  if(state!=State::allocated&&state!=State::preallocated)return false;
  const auto& records=state==State::allocated?proof.allocations:proof.preallocations;
  const auto p=Find(records,std::make_pair(fs,r.page_number));return p!=records.end()&&p->second==r&&!r.creator_operation_uuid.is_nil();
}
bool MatchesMap(const auto& proof,const auto& map) noexcept {
  const auto p=Find(proof.allocations,std::make_pair(map.header.filespace_uuid,map.header.page_number));if(p==proof.allocations.end())return false;const auto& r=p->second;
  return !map.creator_operation_uuid.is_nil()&&map.creator_transaction_uuid.is_nil()&&!map.creator_local_transaction_id&&r.creator_operation_uuid==map.creator_operation_uuid&&r.page_uuid==map.header.page_uuid&&r.page_generation==map.header.page_generation&&r.page_type==map.header.page_type&&r.owner_uuid==map.object_uuid;
}
struct Checkpoint {NativeCheckpointRootView root;Image bytes;std::array<byte,32> sha;};
struct Context {
  Uuid database,primary;u64 budget=0,used=0;
  Scratch& scratch;NativeManagementControlAuthorityDeviceRead& receipt;
  std::pmr::vector<disk::NativeFilespaceDevice> files;
  std::pmr::vector<Batch*> batches;
  std::pmr::vector<std::unique_lock<std::recursive_mutex>> guards;
  std::pmr::map<disk::FileDevice*,Batch*> observations;
  std::pmr::map<Uuid,Image> zero_images;
  std::pmr::map<Uuid,disk::FilespacePageZeroView> zeros;
  bool historical=false;
  std::pmr::map<Uuid,u64> physical_sizes;
  Context(Scratch& s,NativeManagementControlAuthorityDeviceRead& r):scratch(s),receipt(r),
    files(s.resource),batches(s.resource),guards(s.resource),observations(s.resource),
    zero_images(s.resource),zeros(s.resource),physical_sizes(s.resource){}
  void Charge(u64 count,u64 size=1){Require(size&&count<=(budget-used)/size,E::resource_exhausted);used+=count*size;}
  const disk::NativeFilespaceDevice& File(const Uuid& id){const auto it=std::lower_bound(files.begin(),files.end(),id,[](const auto& f,const auto& v){return f.filespace_uuid<v;});Require(it!=files.end()&&it->filespace_uuid==id,E::invalid_request);return *it;}
  template<class R> void Receipt(R& result){
    receipt.io_status=result.io_status;receipt.io_diagnostic=std::move(result.io_diagnostic);
    Require(result.physical_bytes_read<=std::numeric_limits<u64>::max()-receipt.physical_bytes_read,E::binding_mismatch);
    receipt.physical_bytes_read+=result.physical_bytes_read;
  }
  auto ReadAt(disk::FileDevice& device,u64 offset,void* data,std::size_t bytes){
    auto io=observations.at(&device)->ReadAt(offset,data,bytes);
    receipt.io_status=io.status;receipt.io_diagnostic=std::move(io.diagnostic);
    Require(io.bytes_transferred<=std::numeric_limits<u64>::max()-receipt.physical_bytes_read,E::binding_mismatch);
    receipt.physical_bytes_read+=io.bytes_transferred;return io;
  }
  auto Size(disk::FileDevice& device){auto io=device.Size();receipt.io_status=io.status;receipt.io_diagnostic=std::move(io.diagnostic);return io;}
  auto PageZero(const disk::NativeFilespaceDevice& f,Image& raw){
    const disk::FilespaceBootstrapBinding binding{database,f.filespace_uuid,f.page_size_profile_uuid};
    return detail::ReadNativeMetadataPageZero(binding,scratch,
      [&](u64 offset,void* data,std::size_t size){return ReadAt(*f.device,offset,data,size);},
      [&]{return Size(*f.device);},raw);
  }
  void SetContext(Image input){auto raw=scratch.Copy<byte>(input);const auto decoded=scratch.PageZero(raw);
    if(!decoded.ok())throw decoded.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:decoded.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:E::binding_mismatch;
    const auto& z=*decoded.record;const auto& f=File(z.bootstrap.filespace_uuid);Require(z.bootstrap.database_uuid==database&&z.bootstrap.page_size_profile_uuid==f.page_size_profile_uuid,E::binding_mismatch);
    Charge(raw.size());zero_images[z.bootstrap.filespace_uuid]=raw;zeros[z.bootstrap.filespace_uuid]=z;
  }
  void Observe(bool final){for(const auto& f:files){const auto& z=zeros.at(f.filespace_uuid);const auto& image=zero_images.at(f.filespace_uuid);
    const auto size=Size(*f.device);Require(size.ok(),E::io_failure);if(final)Require(size.size_bytes==physical_sizes.at(f.filespace_uuid),E::binding_mismatch);
    else{Require(size.size_bytes>=z.total_pages*u64{z.bootstrap.page_size_bytes},E::binding_mismatch);physical_sizes.emplace(f.filespace_uuid,size.size_bytes);}
    Charge(image.size());auto raw=scratch.Array<byte>(image.size());const auto io=ReadAt(*f.device,0,raw.data(),raw.size());Require(io.ok()&&io.bytes_transferred==raw.size(),E::io_failure);
    Require(std::equal(image.begin(),image.begin()+4096,raw.begin())&&std::equal(image.begin()+4480,image.end(),raw.begin()+4480),E::binding_mismatch);}}
  void Historical(const NativeManagementHistoryView& history,const std::span<const NativeManagementHistoricalPageZero>* contexts){
    historical=contexts!=nullptr;
    if(!contexts&&std::none_of(history.entries.begin(),history.entries.end(),[](const auto& e){return !e.control_directory_images.empty();}))return;
    if(contexts){Require(contexts->size()==files.size(),E::invalid_request);
      for(const auto& f:files){const auto at=std::find_if(contexts->begin(),contexts->end(),[&](const auto& v){return v.filespace_uuid==f.filespace_uuid;});Require(at!=contexts->end(),E::invalid_request);SetContext(at->image);}Observe(false);}
    else for(const auto& f:files){const auto size=disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid)->page_size_bytes;Charge(size);auto raw=scratch.Array<byte>(size);
      const auto io=ReadAt(*f.device,0,raw.data(),raw.size());Require(io.ok()&&io.bytes_transferred==raw.size(),E::io_failure);SetContext(raw);}
    for(auto it=history.entries.rbegin();it!=history.entries.rend();++it)if(!it->control_growth_images.empty()){
      const auto& images=it->control_growth_images;const auto decoded=scratch.PageZero(images.back());
      if(!decoded.ok())throw decoded.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:decoded.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:E::binding_mismatch;
      Require(Equal(zero_images.at(decoded.record->bootstrap.filespace_uuid),images.back()),E::binding_mismatch);SetContext(images.front());}
  }
  Checkpoint Read(const disk::NativePageReference& ref,const Uuid& object,const std::array<byte,32>& sha){
    const auto& f=File(ref.filespace_uuid);Require(f.page_size_profile_uuid==ref.page_size_profile_uuid,E::binding_mismatch);
    const auto size=disk::FindCanonicalFilespacePageProfile(ref.page_size_profile_uuid)->page_size_bytes;
    Charge(3,size);std::optional<disk::FilespacePageZeroView> actual;
    if(historical)Require(ref.page_number<zeros.at(ref.filespace_uuid).total_pages,E::binding_mismatch);
    else{
      Require(core::uuid::IsEngineIdentityUuid(database)&&core::uuid::IsEngineIdentityUuid(object)&&
        core::uuid::IsEngineIdentityUuid(ref.filespace_uuid)&&ref.page_number&&ref.page_generation,E::checkpoint_failure);
      Image observed;const auto zero=PageZero(f,observed);
      if(!zero.ok())throw zero.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:
        zero.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:
        zero.error==disk::FilespacePageZeroError::io_failure?E::io_failure:E::checkpoint_failure;
      actual=zero.record;
      Require(actual->bootstrap.filespace_role<=4&&ref.page_number<actual->total_pages,E::checkpoint_failure);
      Require(!(actual->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted),E::encrypted_requires_authority);
    }
    auto raw=scratch.Array<byte>(size);const auto io=ReadAt(*f.device,ref.page_number*u64{size},raw.data(),raw.size());
    Require(io.ok()&&io.bytes_transferred==raw.size(),E::io_failure);
    const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);Require(h.ok(),E::checkpoint_failure);Require(!(h.header->flags&1u),E::encrypted_requires_authority);
    auto cp=scratch.Checkpoint(raw);
    if(!cp.ok())throw cp.error==NativeCheckpointError::hash_failure?E::hash_failure:cp.error==NativeCheckpointError::resource_exhausted?E::resource_exhausted:E::checkpoint_failure;
    Require(cp.root->header.database_uuid==database&&Self(cp.root->header)==ref&&cp.root->object_uuid==object,historical?E::binding_mismatch:E::checkpoint_failure);
    if(actual){
      if(cp.root->predecessor&&cp.root->predecessor->filespace_uuid==ref.filespace_uuid)Require(cp.root->predecessor->page_number<actual->total_pages,E::checkpoint_failure);
      for(const auto& root:cp.root->roots)if(root.page.filespace_uuid==ref.filespace_uuid)Require(root.page.page_number<actual->total_pages,E::checkpoint_failure);
    }
    Require(cp.root->completed,E::checkpoint_failure);Require(Hash(raw)==sha,E::binding_mismatch);return {*cp.root,raw,sha};
  }
  page::NativeAllocationChainView Maps(const Uuid& fs,const NativeCheckpointRootReference* ref=nullptr){
    const auto& f=File(fs);const disk::FilespaceBootstrapBinding binding{database,fs,f.page_size_profile_uuid};
    using C=page::NativeAllocationChainReadContext;
    C context=C::bootstrap;disk::FilespaceRootReference root;std::array<byte,32> sha{};Image historical_image;
    if(!zero_images.empty()){
      context=C::historical;historical_image=zero_images.at(fs);
      if(ref){Require(ref->page.filespace_uuid==fs&&ref->page.page_size_profile_uuid==f.page_size_profile_uuid,E::binding_mismatch);
        root={3,3,fs,ref->page.page_number,ref->page.page_generation,ref->page.page_size_profile_uuid,ref->object_uuid};sha=ref->sha256;
      }else{const auto& z=zeros.at(fs);const auto at=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& v){return v.kind==3;});Require(at!=z.roots.end(),E::binding_mismatch);root=*at;
        Charge(z.bootstrap.page_size_bytes);auto raw=scratch.Array<byte>(z.bootstrap.page_size_bytes);const auto io=ReadAt(*f.device,root.page_number*u64{raw.size()},raw.data(),raw.size());Require(io.ok()&&io.bytes_transferred==raw.size(),E::io_failure);sha=Hash(raw);}
    }else if(ref){Require(ref->page.filespace_uuid==fs&&ref->page.page_size_profile_uuid==f.page_size_profile_uuid,E::binding_mismatch);
      context=C::selected;root={3,3,fs,ref->page.page_number,ref->page.page_generation,ref->page.page_size_profile_uuid,ref->object_uuid};
    }
    auto read=page::detail::ReadNativeAllocationChainBacked(*f.device,binding,budget-used,context,
      context==C::bootstrap?nullptr:&root,context==C::historical?&sha:nullptr,historical_image,
      *observations.at(f.device),*scratch.resource);Receipt(read);auto out=read.chain;
    MapError(out.error);Charge(out.retained_image_bytes);if(ref)Require(Hash(out.pages.front().image)==ref->sha256,E::binding_mismatch);return out;
  }
  page::NativeDirectoryChainView Directory(const NativeCheckpointRootView& cp){
    const auto& root=Root(cp,3);const disk::FilespaceRootReference r{5,root.page_type,root.page.filespace_uuid,
      root.page.page_number,root.page.page_generation,root.page.page_size_profile_uuid,root.object_uuid};
    auto contexts=scratch.Array<page::NativeHistoricalFilespaceImageView>(zero_images.size());std::size_t n=0;
    for(const auto& [fs,raw]:zero_images){Charge(raw.size());contexts[n++]={fs,raw};}
    using C=page::NativeDirectoryChainReadContext;
    auto read=page::detail::ReadNativeDirectoryChainBacked(database,files,r,budget-used,
      zero_images.empty()?C::current:C::historical,zero_images.empty()?nullptr:&root.sha256,contexts,batches,*scratch.resource);
    Receipt(read);auto out=read.chain;
    if(!out.ok()){using D=page::NativeDirectoryError;throw out.error==D::resource_exhausted?E::resource_exhausted:
      out.error==D::hash_failure?E::hash_failure:out.error==D::io_failure?E::io_failure:E::binding_mismatch;}
    Charge(out.retained_image_bytes);Require(Hash(out.pages.front().image)==root.sha256,E::binding_mismatch);return out;
  }
  page::NativeInventoryChainView Inventory(const NativeCheckpointRootView& cp){
    const auto& root=Root(cp,1);const disk::FilespaceRootReference r{4,root.page_type,root.page.filespace_uuid,root.page.page_number,root.page.page_generation,root.page.page_size_profile_uuid,root.object_uuid};
    auto contexts=scratch.Array<page::NativeInventoryHistoricalPageZero>(zero_images.size());std::size_t n=0;
    for(const auto& [fs,raw]:zero_images)contexts[n++]={fs,raw};
    using C=page::NativeInventoryChainReadContext;
    auto read=page::detail::ReadNativeInventoryChainBacked(database,files,r,budget-used,
      zero_images.empty()?C::current:C::historical,zero_images.empty()?nullptr:&root.sha256,contexts,batches,*scratch.resource);
    Receipt(read);auto out=read.chain;
    if(!out.ok())throw out.error==page::NativeInventoryError::hash_failure?E::hash_failure:out.error==page::NativeInventoryError::resource_exhausted?E::resource_exhausted:out.error==page::NativeInventoryError::io_failure?E::io_failure:out.error==page::NativeInventoryError::encrypted_requires_crypto_authority?E::encrypted_requires_authority:E::inventory_failure;
    Charge(out.retained_image_bytes);Require(Hash(out.pages.front().image)==root.sha256&&out.inventory.next_local_transaction_id&&out.inventory.next_local_transaction_id-1==cp.selected_local_transaction_id,E::binding_mismatch);return out;
  }
};
void Transaction(const page::NativeTransactionInventoryView& inventory,const Uuid& id,u64 local,bool committed){
  Require(mga::MakeLocalTransactionId(local).valid(),E::creator_mismatch);
  const auto at=std::find_if(inventory.entries.begin(),inventory.entries.end(),[&](const auto& e){return e.identity.local_id.value==local;});
  Require(at!=inventory.entries.end()&&at->identity.transaction_uuid.value==id&&at->identity.scope==mga::TransactionScope::local_node&&
    (!committed||mga::HasCommittedInventoryOutcome(*at)),E::creator_mismatch);
}
const page::NativeAllocationRecord& Allocated(const page::NativeAllocationChainView& chain,const disk::NativeCommonPageHeader& h,const Uuid& owner){
  auto m=std::upper_bound(chain.pages.begin(),chain.pages.end(),h.page_number,[](u64 n,const auto& p){return n<p.map.first_page;});Require(m!=chain.pages.begin(),E::allocation_failure);--m;const auto& map=m->map;
  Require(h.page_number-map.first_page<map.states.size()&&map.states[h.page_number-map.first_page]==State::allocated,E::allocation_failure);
  const auto r=std::lower_bound(map.records.begin(),map.records.end(),h.page_number,[](const auto& r,u64 n){return r.page_number<n;});Require(r!=map.records.end()&&r->page_number==h.page_number&&r->page_uuid==h.page_uuid&&r->page_generation==h.page_generation&&r->page_type==h.page_type&&r->owner_uuid==owner,E::binding_mismatch);return *r;
}
void Owners(const page::NativeAllocationChainView& chain,const page::NativeTransactionInventoryView& inventory,const Proof& proof){
  for(const auto& image:chain.pages){const auto& m=image.map;
    if(m.creator_operation_uuid.is_nil())Transaction(inventory,m.creator_transaction_uuid,m.creator_local_transaction_id,true);
    else Require(MatchesMap(proof,m),E::creator_mismatch);
    for(const auto& r:m.records){if(r.creator_operation_uuid.is_nil())Transaction(inventory,r.creator_transaction_uuid,r.creator_local_transaction_id,false);
      else Require(MatchesAllocation(proof,m.header.filespace_uuid,r,m.states[r.page_number-m.first_page]),E::creator_mismatch);}
  }
}
page::NativeInventoryChainView Controls(Context& c,const Checkpoint& cp,const page::NativeAllocationChainView& maps,const Proof& proof,const Images* expected_inventory=nullptr){
  auto inventory=c.Inventory(cp.root);
  if(expected_inventory){Require(inventory.pages.size()==expected_inventory->size(),E::binding_mismatch);for(std::size_t i=0;i<inventory.pages.size();++i)Require(Equal(inventory.pages[i].image,(*expected_inventory)[i]),E::binding_mismatch);}
  if(cp.root.creator_operation_uuid.is_nil())Transaction(inventory.inventory,cp.root.creator_transaction_uuid,cp.root.creator_local_transaction_id,true);
  else Require(MatchesCheckpoint(proof,cp.root,cp.sha),E::creator_mismatch);
  Owners(maps,inventory.inventory,proof);
  std::pmr::set<Uuid> allocation_ids(c.scratch.resource),page_ids(c.scratch.resource);
  const auto unique=[&](const page::NativeAllocationChainView& chain){for(const auto& image:chain.pages)for(const auto& r:image.map.records){
    Require(allocation_ids.insert(r.allocation_uuid).second,E::binding_mismatch);
    if(!r.page_uuid.is_nil())Require(page_ids.insert(r.page_uuid).second,E::binding_mismatch);}};
  unique(maps);
  std::pmr::vector<NativeInventoryPageBinding> required(c.scratch.resource);required.push_back({cp.root.header,cp.root.object_uuid});
  for(const auto& image:inventory.pages)required.push_back({image.page.header,image.page.object_uuid});
  auto directory=c.Directory(cp.root);const auto& creator=directory.pages.front().directory;
  if(creator.creator_operation_uuid.is_nil())Transaction(inventory.inventory,creator.creator_transaction_uuid,creator.creator_local_transaction_id,true);
  else{
    Require(MatchesDirectory(proof,directory,Root(cp.root,3).sha256),E::creator_mismatch);
  }
  for(const auto& image:directory.pages)required.push_back({image.directory.header,image.directory.object_uuid});
    for(const auto& image:directory.pages)for(const auto& entry:image.directory.records){if(entry.bootstrap.filespace_uuid!=c.primary||!entry.allocation_root)continue;
      const auto& r=*entry.allocation_root;const auto& root=Root(cp.root,4);const auto& map=maps.pages.front().map;
      Require(r.page==root.page&&r.object_uuid==root.object_uuid&&r.sha256==root.sha256&&
        r.map_generation==map.map_generation&&r.capacity_generation==map.capacity_generation&&entry.total_pages==map.total_pages,E::binding_mismatch);}
  std::size_t retained_allocations=0,retained_preallocations=0;
  for(const auto& file:c.files){if(std::none_of(required.begin(),required.end(),[&](const auto& v){return v.header.filespace_uuid==file.filespace_uuid;})&&
      std::none_of(proof.allocations.begin(),proof.allocations.end(),[&](const auto& entry){return entry.first.first==file.filespace_uuid;})&&
      std::none_of(proof.preallocations.begin(),proof.preallocations.end(),[&](const auto& entry){return entry.first.first==file.filespace_uuid;}))continue;
    page::NativeAllocationChainView other;const auto* chain=&maps;if(file.filespace_uuid!=c.primary){
      const page::NativeFilespaceDirectoryRecord* member=nullptr;
      for(const auto& image:directory.pages)for(const auto& entry:image.directory.records)if(entry.bootstrap.filespace_uuid==file.filespace_uuid)member=&entry;
      Require(member,E::binding_mismatch);
      if(member->allocation_root){const auto& r=*member->allocation_root;const NativeCheckpointRootReference root{4,3,r.page,r.object_uuid,r.sha256};
        other=c.Maps(file.filespace_uuid,&root);const auto& map=other.pages.front().map;
        Require(map.map_generation==r.map_generation&&map.capacity_generation==r.capacity_generation&&map.total_pages==member->total_pages,E::binding_mismatch);
      }else other=c.Maps(file.filespace_uuid);
      Owners(other,inventory.inventory,proof);unique(other);chain=&other;}
    for(const auto& target:required)if(target.header.filespace_uuid==file.filespace_uuid){const auto& r=Allocated(*chain,target.header,target.object_uuid);
      if(r.creator_operation_uuid.is_nil())Transaction(inventory.inventory,r.creator_transaction_uuid,r.creator_local_transaction_id,true);
      else Require(MatchesAllocation(proof,file.filespace_uuid,r,State::allocated),E::creator_mismatch);}
    for(const auto& [key,record]:proof.allocations){if(key.first!=file.filespace_uuid)continue;auto expected=chain->pages.front().map.header;
      expected.page_number=record.page_number;expected.page_uuid=record.page_uuid;expected.page_generation=record.page_generation;expected.page_type=record.page_type;
      Require(Allocated(*chain,expected,record.owner_uuid)==record,E::binding_mismatch);++retained_allocations;}
    for(const auto& [key,record]:proof.preallocations){if(key.first!=file.filespace_uuid)continue;
      auto image=std::upper_bound(chain->pages.begin(),chain->pages.end(),record.page_number,[](u64 n,const auto& p){return n<p.map.first_page;});
      Require(image!=chain->pages.begin(),E::binding_mismatch);const auto& map=std::prev(image)->map;
      Require(record.page_number-map.first_page<map.states.size()&&map.states[record.page_number-map.first_page]==State::preallocated,E::binding_mismatch);
      const auto actual=std::lower_bound(map.records.begin(),map.records.end(),record.page_number,[](const auto& r,u64 n){return r.page_number<n;});
      Require(actual!=map.records.end()&&*actual==record,E::binding_mismatch);++retained_preallocations;
    }
  }
  Require(retained_allocations==proof.allocations.size()&&retained_preallocations==proof.preallocations.size(),E::binding_mismatch);
  return inventory;
}
Pages ChainImages(const page::NativeAllocationChainView& maps,Scratch& scratch){Pages out(scratch.resource);out.reserve(maps.pages.size());for(const auto& p:maps.pages)out.push_back(p.image);return out;}
NativeManagementControlAuthorityView ReadControlView(const Uuid& database,std::span<const disk::NativeFilespaceDevice> supplied,
    const Uuid& primary,u64 budget,NativeManagementHistoryReadContext context,const NativeManagementCheckpointAnchor* requested,
    std::span<const NativeManagementHistoricalPageZero> historical,Scratch& scratch,
    std::span<Batch* const> observations,NativeManagementControlAuthorityDeviceRead& receipt) noexcept {
  try{
    Require(core::uuid::IsEngineIdentityUuid(database)&&core::uuid::IsEngineIdentityUuid(primary)&&!supplied.empty(),E::invalid_request);
    Require(observations.size()==supplied.size(),E::invalid_request);
    Context c(scratch,receipt);c.database=database;c.primary=primary;c.budget=budget;c.files.assign(supplied.begin(),supplied.end());
    for(std::size_t i=0;i<supplied.size();++i){Require(supplied[i].device&&observations[i]&& &observations[i]->device()==supplied[i].device,E::invalid_request);
      c.observations.emplace(supplied[i].device,observations[i]);}
    std::sort(c.files.begin(),c.files.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    std::pmr::set<disk::FileDevice*> devices(scratch.resource);c.guards.reserve(c.files.size());c.batches.reserve(c.files.size());
    for(std::size_t i=0;i<c.files.size();++i){const auto& f=c.files[i];Require(core::uuid::IsEngineIdentityUuid(f.filespace_uuid)&&disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid)&&f.device&&devices.insert(f.device).second&&(!i||c.files[i-1].filespace_uuid!=f.filespace_uuid),E::invalid_request);c.batches.push_back(c.observations.at(f.device));}
    for(const auto& f:c.files)c.guards.push_back(f.device->AcquireOperationGuard());
    const auto* contexts=context==NativeManagementHistoryReadContext::historical_result?&historical:nullptr;
    auto read=detail::ReadNativeManagementHistoryBacked(database,c.files,primary,budget,context,requested,historical,c.batches,*scratch.resource);
    c.Receipt(read);const auto& history=read.history;const auto selected=history.selection;
    if(!history.ok())throw history.error==NativeManagementHistoryError::hash_failure?E::hash_failure:history.error==NativeManagementHistoryError::resource_exhausted?E::resource_exhausted:history.error==NativeManagementHistoryError::io_failure?E::io_failure:history.error==NativeManagementHistoryError::encrypted_requires_authority?E::encrypted_requires_authority:history.error==NativeManagementHistoryError::cluster_requires_authority?E::cluster_requires_authority:E::history_failure;
    c.Charge(history.verified_image_bytes);c.Historical(history,contexts);Proof result(scratch.resource);const u64 size=disk::FindCanonicalFilespacePageProfile(c.File(primary).page_size_profile_uuid)->page_size_bytes;
    const Images* expected_inventory=nullptr;
    for(const auto& entry:history.entries){const auto& p=entry.plan;Require(p.control_bundle&&p.management_extent,E::binding_mismatch);
      auto base=c.Read(p.base_checkpoint,p.base_checkpoint_object_uuid,p.base_checkpoint_sha256);auto target=c.Read(p.target_checkpoint,p.target_checkpoint_object_uuid,entry.checkpoint_sha256);
      auto before=c.Maps(primary,&Root(base.root,4));auto before_inventory=Controls(c,base,before,result,expected_inventory);
      NativeManagementDirectoryBaseView directory_base;Pages directory_images(scratch.resource),zero_images(scratch.resource);std::pmr::set<Uuid> changed(scratch.resource);
      Pages before_bytes(scratch.resource);
      if(!entry.control_directory_images.empty()){
        const auto directory=c.Directory(base.root);for(const auto& image:directory.pages){c.Charge(image.image.size());directory_images.push_back(image.image);}
        for(const auto& raw:entry.control_allocation_images){const auto h=disk::DecodeNativeCommonPageHeader(raw.data(),128);Require(h.ok(),E::binding_mismatch);changed.insert(h.header->filespace_uuid);}
        Require(changed.contains(primary),E::binding_mismatch);
        for(const auto& fs:changed){c.Charge(c.zero_images.at(fs).size());zero_images.push_back(c.zero_images.at(fs));
          if(fs==primary){c.Charge(before.retained_image_bytes);auto images=ChainImages(before,scratch);before_bytes.insert(before_bytes.end(),std::make_move_iterator(images.begin()),std::make_move_iterator(images.end()));continue;}
          const page::NativeFilespaceDirectoryRecord* member=nullptr;for(const auto& image:directory.pages)for(const auto& r:image.directory.records)if(r.bootstrap.filespace_uuid==fs)member=&r;
          Require(member,E::binding_mismatch);page::NativeAllocationChainView chain;
          if(member->allocation_root){const auto& r=*member->allocation_root;const NativeCheckpointRootReference root{4,3,r.page,r.object_uuid,r.sha256};chain=c.Maps(fs,&root);}
          else chain=c.Maps(fs);
          c.Charge(chain.retained_image_bytes);auto images=ChainImages(chain,scratch);before_bytes.insert(before_bytes.end(),std::make_move_iterator(images.begin()),std::make_move_iterator(images.end()));
        }
      }else{c.Charge(before.retained_image_bytes);before_bytes=ChainImages(before,scratch);changed.insert(primary);}
      if(!entry.control_growth_images.empty()){const auto& images=entry.control_growth_images;const auto decoded=scratch.PageZero(images.front());
        if(!decoded.ok())throw decoded.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:decoded.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:E::binding_mismatch;
        Require(Equal(c.zero_images.at(decoded.record->bootstrap.filespace_uuid),images.front()),E::binding_mismatch);c.SetContext(images.back());}
      auto after=c.Maps(primary,&Root(target.root,4));std::pmr::map<Uuid,page::NativeAllocationChainView> after_chains(scratch.resource);
      Pages after_bytes(scratch.resource);
      if(!entry.control_directory_images.empty()){
        const auto directory=c.Directory(target.root);
        Require(directory.pages.size()==entry.control_directory_images.size(),E::binding_mismatch);
        for(std::size_t i=0;i<directory.pages.size();++i)Require(Equal(directory.pages[i].image,entry.control_directory_images[i]),E::binding_mismatch);
        for(const auto& fs:changed){if(fs==primary){c.Charge(after.retained_image_bytes);auto images=ChainImages(after,scratch);after_bytes.insert(after_bytes.end(),std::make_move_iterator(images.begin()),std::make_move_iterator(images.end()));continue;}
          const page::NativeFilespaceDirectoryRecord* member=nullptr;for(const auto& image:directory.pages)for(const auto& r:image.directory.records)if(r.bootstrap.filespace_uuid==fs)member=&r;
          Require(member&&member->allocation_root,E::binding_mismatch);const auto& r=*member->allocation_root;const NativeCheckpointRootReference root{4,3,r.page,r.object_uuid,r.sha256};
          auto chain=c.Maps(fs,&root);c.Charge(chain.retained_image_bytes);auto images=ChainImages(chain,scratch);after_bytes.insert(after_bytes.end(),std::make_move_iterator(images.begin()),std::make_move_iterator(images.end()));after_chains.emplace(fs,std::move(chain));
        }
      }else{c.Charge(after.retained_image_bytes);after_bytes=ChainImages(after,scratch);}
      Require(EqualPages(after_bytes,entry.control_allocation_images),E::binding_mismatch);
      // History retained exact canonical observed bytes, not synthetic images.
      // The full delta below decodes and rebinds every plan/extent/bundle again.
      Require(!entry.plan_image.empty()&&entry.extent_images.size()==p.management_extent->page_count&&
        entry.bundle_images.size()==p.control_bundle->page_count,E::binding_mismatch);
      Require(Hash(entry.plan_image)==entry.plan_sha256,E::binding_mismatch);
      c.Charge(2,size);c.Charge(p.management_extent->page_count,size);
      c.Charge(p.management_extent->aggregate_bytes,4);c.Charge(2,size);
      c.Charge(p.control_bundle->page_count,size);
      if(p.control_bundle->directory_count)c.Charge(p.control_bundle->payload_bytes,4);
      else{c.Charge(p.control_bundle->map_count,4*size);c.Charge(p.control_bundle->inventory_count,4*size);}c.Charge(2,size);
      Pages before_inventory_bytes(scratch.resource);
      if(p.intent.recovery_profile==2){for(const auto& raw:before_inventory.pages)c.Charge(4,raw.image.size());for(const auto& raw:entry.control_inventory_images)c.Charge(4,raw.size());before_inventory_bytes.reserve(before_inventory.pages.size());for(const auto& raw:before_inventory.pages)before_inventory_bytes.push_back(raw.image);expected_inventory=&entry.control_inventory_images;}
      directory_base={directory_images,zero_images};
      const NativeManagementControlAllocationInputs delta_inputs{base.bytes,target.bytes,entry.plan_image,entry.extent_images,
        before_bytes,after_bytes,entry.bundle_images,before_inventory_bytes,
        entry.control_directory_images.empty()?nullptr:&directory_base,budget};
      const auto delta=detail::ValidateNativeManagementControlAllocationBacked(delta_inputs,*scratch.resource);
      if(delta!=NativeManagementControlAllocationError::none)throw delta==NativeManagementControlAllocationError::hash_failure?E::hash_failure:delta==NativeManagementControlAllocationError::resource_exhausted?E::resource_exhausted:delta==NativeManagementControlAllocationError::cluster_requires_authority?E::cluster_requires_authority:delta==NativeManagementControlAllocationError::encrypted_requires_authority?E::encrypted_requires_authority:E::binding_mismatch;
      Require(result.publications.emplace(p.operation_uuid,NativeManagementPublishedCheckpoint{p.target_checkpoint,p.target_checkpoint_object_uuid,entry.checkpoint_sha256,Root(target.root,3)}).second,E::binding_mismatch);
      const auto retain=[&](const Uuid& fs,const page::NativeAllocationChainView& chain){for(const auto& image:chain.pages)for(const auto& record:image.map.records)if(record.creator_operation_uuid==p.operation_uuid){
        const auto state=image.map.states[record.page_number-image.map.first_page];
        Require(state==State::allocated||state==State::preallocated,E::binding_mismatch);
        auto& records=state==State::allocated?result.allocations:result.preallocations;
        Require(records.emplace(std::make_pair(fs,record.page_number),record).second,E::binding_mismatch);
      }};retain(primary,after);for(const auto& [fs,chain]:after_chains)retain(fs,chain);
      if(!entry.control_growth_images.empty()){
        const auto decoded=scratch.PageZero(entry.control_growth_images.back());
        if(!decoded.ok())throw decoded.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:decoded.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:E::binding_mismatch;
        const auto& z=*decoded.record;const auto retained=result.allocations.find({z.bootstrap.filespace_uuid,0});
        if(retained!=result.allocations.end())retained->second.page_generation=z.page_generation;
      }
      Controls(c,target,after,result,expected_inventory);
    }
    const auto& anchor=*history.anchor;auto current=c.Read(anchor.checkpoint,anchor.checkpoint_object_uuid,anchor.checkpoint_sha256);auto maps=c.Maps(primary,&Root(current.root,4));Controls(c,current,maps,result,expected_inventory);
    if(c.historical)c.Observe(true);
    NativeManagementControlAuthorityView out;out.anchor=anchor;out.selection=selected;out.verified_image_bytes=c.used;
    auto publications=scratch.Array<NativeManagementPublicationEntry>(result.publications.size());std::size_t n=0;
    for(const auto& [key,value]:result.publications)publications[n++]={key,value};out.publications=publications;
    const auto retain=[&](const auto& source){auto values=scratch.Array<NativeManagementAllocationEntry>(source.size());std::size_t n=0;
      for(const auto& [key,value]:source)values[n++]={key,value};return values;};
    out.allocations=retain(result.allocations);out.preallocations=retain(result.preallocations);out.error=E::none;return out;
  }catch(E e){return FailView(e);}catch(const std::bad_alloc&){return FailView(E::resource_exhausted);}
   catch(const std::length_error&){return FailView(E::resource_exhausted);}catch(...){return FailView(E::binding_mismatch);}
}
NativeManagementControlAuthority Materialize(const NativeManagementControlAuthorityView& view){
  if(!view.ok())return Fail(view.error);NativeManagementControlAuthority result;
  result.anchor=view.anchor;result.selection=view.selection;result.verified_image_bytes=view.verified_image_bytes;
  for(const auto& v:view.publications)result.publications.emplace(v);
  for(const auto& v:view.allocations)result.allocations.emplace(v);
  for(const auto& v:view.preallocations)result.preallocations.emplace(v);
  result.error=E::none;return result;
}
NativeManagementControlAuthority ReadControlGraph(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& files,
    const Uuid& primary,u64 budget,const NativeManagementCheckpointAnchor* anchor,
    const std::map<Uuid,std::vector<byte>>* historical=nullptr) noexcept{
  try{
    detail::NativeMetadataHeapMemory heap;std::pmr::monotonic_buffer_resource resource(&heap);Scratch scratch{&resource};
    auto contexts=scratch.Array<NativeManagementHistoricalPageZero>(historical?historical->size():0);std::size_t n=0;
    if(historical)for(const auto& [id,raw]:*historical)contexts[n++]={id,raw};
    auto pointers=scratch.Array<Batch*>(files.size());
    if(files.size()>std::numeric_limits<std::size_t>::max()/sizeof(Batch))throw std::bad_alloc();
    auto* batches=static_cast<Batch*>(resource.allocate(files.size()*sizeof(Batch),alignof(Batch)));
    NativeManagementControlAuthorityDeviceRead read;
    {
      struct Cleanup{Batch* batches;std::size_t count=0;~Cleanup(){while(count)std::destroy_at(batches+--count);}} cleanup{batches};
      for(std::size_t i=0;i<files.size();++i){if(!files[i].device)return Fail(E::invalid_request);
        pointers[i]=std::construct_at(batches+i,*files[i].device);++cleanup.count;}
      read=detail::ReadNativeManagementControlAuthorityBacked(database,files,primary,budget,
        historical?NativeManagementHistoryReadContext::historical_result:
          anchor?NativeManagementHistoryReadContext::anchored:NativeManagementHistoryReadContext::selected,
        anchor,contexts,pointers,resource);
    }
    return Materialize(read.authority);
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
   catch(...){return Fail(E::binding_mismatch);}
}
} // namespace
NativeManagementControlAuthorityDeviceRead detail::ReadNativeManagementControlAuthorityBacked(
    const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,const Uuid& primary,u64 budget,
    NativeManagementHistoryReadContext context,const NativeManagementCheckpointAnchor* anchor,
    std::span<const NativeManagementHistoricalPageZero> historical,
    std::span<Batch* const> observations,std::pmr::memory_resource& resource) noexcept{
  NativeManagementControlAuthorityDeviceRead out;
  if((context!=NativeManagementHistoryReadContext::selected&&context!=NativeManagementHistoryReadContext::anchored&&
      context!=NativeManagementHistoryReadContext::historical_result)||
     (context==NativeManagementHistoryReadContext::selected?anchor!=nullptr:anchor==nullptr)||
     (context==NativeManagementHistoryReadContext::historical_result?historical.size()!=files.size():!historical.empty())||
     files.empty()||observations.size()!=files.size())return out;
  for(std::size_t i=0;i<files.size();++i)if(!files[i].device||!observations[i]||&observations[i]->device()!=files[i].device)return out;
  Scratch scratch{&resource};out.authority=ReadControlView(database,files,primary,budget,context,anchor,historical,scratch,observations,out);
  return out;
}
NativeManagementControlAuthorityDeviceRead ReadNativeManagementControlAuthorityInto(
    const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,const Uuid& primary,u64 budget,
    NativeManagementHistoryReadContext context,const NativeManagementCheckpointAnchor* anchor,
    std::span<const NativeManagementHistoricalPageZero> historical,
    std::span<Batch* const> observations,std::span<byte> backing) noexcept{
  NativeManagementControlAuthorityDeviceRead out;
  const auto disjoint=[&](auto v){return disk::detail::DisjointNativeDecodeRegions(backing,v);};
  bool valid=disjoint(std::span{&database,1})&&disjoint(std::span{&primary,1})&&disjoint(files)&&disjoint(historical)&&
    disjoint(observations)&&(!anchor||disjoint(std::span{anchor,1}));
  for(const auto& f:files)valid=valid&&(!f.device||disjoint(std::span{f.device,1}));
  for(const auto* observation:observations)valid=valid&&(!observation||disjoint(std::span{observation,1}));
  for(const auto& h:historical)valid=valid&&disjoint(h.image);
  if(!valid){out.authority.error=E::invalid_workspace;return out;}
  detail::NativeMetadataMemory resource(backing);
  out=detail::ReadNativeManagementControlAuthorityBacked(database,files,primary,budget,context,anchor,historical,observations,resource);
  if(out.ok())out.authority.backing_bytes_used=resource.used();return out;
}
bool MatchesNativeManagementPublishedCheckpoint(const NativeManagementControlGraph& p,const NativeCheckpointRoot& c,const std::array<byte,32>& sha) noexcept{return MatchesCheckpoint(p,c,sha);}
bool MatchesNativeManagementPublishedDirectory(const NativeManagementControlGraph& p,const page::NativeFilespaceDirectoryChainResult& c,const std::array<byte,32>& sha) noexcept{return MatchesDirectory(p,c,sha);}
bool MatchesNativeManagementControlAllocation(const NativeManagementControlGraph& p,const Uuid& fs,const page::NativeAllocationRecord& r,State s) noexcept{return MatchesAllocation(p,fs,r,s);}
bool MatchesNativeManagementControlMap(const NativeManagementControlGraph& p,const page::NativeAllocationMap& m) noexcept{return MatchesMap(p,m);}
bool MatchesNativeManagementPublishedCheckpoint(const NativeManagementControlAuthorityView& p,const NativeCheckpointRootView& c,const std::array<byte,32>& sha) noexcept{return MatchesCheckpoint(p,c,sha);}
bool MatchesNativeManagementPublishedDirectory(const NativeManagementControlAuthorityView& p,const page::NativeDirectoryChainView& c,const std::array<byte,32>& sha) noexcept{return MatchesDirectory(p,c,sha);}
bool MatchesNativeManagementControlAllocation(const NativeManagementControlAuthorityView& p,const Uuid& fs,const page::NativeAllocationRecord& r,State s) noexcept{return MatchesAllocation(p,fs,r,s);}
bool MatchesNativeManagementControlMap(const NativeManagementControlAuthorityView& p,const page::NativeAllocationMapView& m) noexcept{return MatchesMap(p,m);}
NativeManagementControlAuthority ReadNativeManagementControlAuthorityFromOpenDevices(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,u64 budget) noexcept {
  return ReadControlGraph(database,devices,primary,budget,nullptr);
}
NativeManagementControlGraph ReadNativeManagementControlGraphFromOpenDevices(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,const NativeManagementCheckpointAnchor& anchor,u64 budget) noexcept {
  auto result=ReadControlGraph(database,devices,primary,budget,&anchor);
  return std::move(static_cast<NativeManagementControlGraph&>(result));
}
NativeManagementControlGraph ReadNativeManagementControlGraphAtHistoricalContextFromOpenDevices(const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,const Uuid& primary,
    const NativeManagementCheckpointAnchor& anchor,const std::map<Uuid,std::vector<byte>>& contexts,u64 budget) noexcept {
  auto result=ReadControlGraph(database,devices,primary,budget,&anchor,&contexts);
  return std::move(static_cast<NativeManagementControlGraph&>(result));
}
} // namespace scratchbird::storage::database

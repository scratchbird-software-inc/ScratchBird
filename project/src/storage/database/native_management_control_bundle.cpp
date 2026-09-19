// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_control_bundle.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "uuid.hpp"
#include "transaction_inventory_page.hpp"
#include "transaction_inventory_validation.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace scratchbird::storage::database {
namespace {
using namespace core::platform;
using E=NativeManagementControlBundleError;
using Root=NativeManagementControlBundleRoot;
using Bytes=std::vector<byte>;
using Pages=std::vector<Bytes>;
using Header=disk::NativeCommonPageHeader;
void Require(bool ok,E error){if(!ok)throw error;}
bool V7(const Uuid& id){return core::uuid::IsEngineIdentityUuid(id);}
bool Zero(const byte* b,std::size_t n){return std::all_of(b,b+n,[](byte v){return !v;});}
void Put(byte* b,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b);}
Uuid Get(const byte* b){Uuid id;std::copy_n(b,16,id.bytes.begin());return id;}
template<class T>T Fail(E e){T r;r.error=e;return r;}
auto Hash(const Bytes& b){const auto h=core::hash::ComputeSha256Digest(b);Require(h.ok(),E::hash_failure);return h.digest;}
auto Seal(const Bytes& b){const std::array<byte,32> zero{};const core::hash::HashDigestSegment parts[]={{b.data(),320},{zero.data(),32},{b.data()+352,b.size()-352}};
  const auto h=core::hash::ComputeSha256DigestParts(parts,3);Require(h.ok(),E::hash_failure);return h.digest;}
auto Self(const Header& h){return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
u32 Shape(const Root& r,const Uuid& database,const Uuid& bootstrap,u64 budget,bool digests=true){
  const auto* profile=disk::FindCanonicalFilespacePageProfile(r.first.page_size_profile_uuid);
  Require(profile&&V7(database)&&V7(bootstrap)&&V7(r.object_uuid)&&V7(r.operation_uuid)&&V7(r.first.filespace_uuid),E::invalid_identity);
  const std::array<Uuid,4> ids{database,bootstrap,r.object_uuid,r.operation_uuid};for(std::size_t i=0;i<ids.size();++i)for(std::size_t j=0;j<i;++j)Require(ids[i]!=ids[j],E::invalid_identity);
  const u64 size=profile->page_size_bytes,capacity=size-384,limit=std::numeric_limits<u64>::max();
  Require(r.map_count&&r.inventory_count<=limit-r.map_count&&r.map_count+r.inventory_count<=limit/size&&r.first.page_number&&r.first.page_generation,E::invalid_extent);
  const u64 bytes=(r.map_count+r.inventory_count)*size;Require(r.page_count==bytes/capacity+(bytes%capacity!=0),E::invalid_extent);
  Require(r.first.page_number<=limit/size&&r.page_count-1<=limit/size-r.first.page_number&&disk::CheckFileDeviceExtent((r.first.page_number+r.page_count-1)*size,size).ok(),E::invalid_extent);
  Require(!digests||(!Zero(r.aggregate_sha256.data(),32)&&!Zero(r.first_page_sha256.data(),32)),E::invalid_integrity);
  Require(bytes<=std::numeric_limits<std::size_t>::max()&&budget>=2*size,E::resource_exhausted);
  u64 remaining=budget-2*size;Require(r.page_count<=remaining/size,E::resource_exhausted);remaining-=r.page_count*size;Require(bytes<=remaining/4,E::resource_exhausted);
  return static_cast<u32>(size);
}
void Common(const Header& h,const Root& r,const Uuid& db,const Uuid& bootstrap,u32 size,u64 i,std::set<Uuid>& ids){
  Require(h.database_uuid==db&&h.filespace_uuid==r.first.filespace_uuid&&h.page_size_profile_uuid==r.first.page_size_profile_uuid&&h.page_size_bytes==size&&h.page_number==r.first.page_number+i&&h.page_generation==r.first.page_generation&&h.page_type==0x500&&!h.flags,E::binding_mismatch);
  Require(V7(h.page_uuid)&&h.page_uuid!=db&&h.page_uuid!=bootstrap&&h.page_uuid!=r.object_uuid&&h.page_uuid!=r.operation_uuid&&ids.insert(h.page_uuid).second,E::invalid_identity);
}
u64 Maps(const Pages& images,const Pages& inventory_images,const std::vector<Header>& headers,const Root& root,const Uuid& db,u32 size){
  Require(images.size()==root.map_count&&inventory_images.size()==root.inventory_count&&headers.size()==root.page_count,E::invalid_allocation);
  std::vector<page::NativeAllocationMap> maps;maps.reserve(images.size());std::set<u64> slots;std::set<Uuid> page_ids,allocation_ids,record_ids;
  for(const auto& h:headers){Require(slots.insert(h.page_number).second&&page_ids.insert(h.page_uuid).second,E::invalid_identity);}
  u64 covered=0;
  for(const auto& image:images){Require(image.size()==size,E::invalid_allocation);auto decoded=page::DecodeNativeAllocationMap(image);
    if(!decoded.ok())throw decoded.error==page::NativeAllocationError::resource_exhausted?E::resource_exhausted:decoded.error==page::NativeAllocationError::hash_failure?E::hash_failure:E::invalid_allocation;
    auto m=std::move(*decoded.map);const auto& h=m.header;Require(!h.flags,E::cluster_requires_authority);
    Require(h.database_uuid==db&&h.filespace_uuid==root.first.filespace_uuid&&h.page_size_profile_uuid==root.first.page_size_profile_uuid&&h.page_generation==root.first.page_generation&&m.map_generation==root.first.page_generation&&m.creator_transaction_uuid.is_nil()&&!m.creator_local_transaction_id&&m.creator_operation_uuid==root.operation_uuid&&m.first_page==covered,E::binding_mismatch);
    Require(slots.insert(h.page_number).second&&page_ids.insert(h.page_uuid).second,E::invalid_identity);
    if(!maps.empty()){const auto& first=maps.front();const auto& last=maps.back();Require(m.object_uuid==first.object_uuid&&m.total_pages==first.total_pages&&m.capacity_generation==first.capacity_generation&&last.next&&*last.next==Self(h)&&last.next_sha256==Hash(image),E::binding_mismatch);}
    for(const auto& record:m.records)Require(allocation_ids.insert(record.allocation_uuid).second&&(record.page_uuid.is_nil()||record_ids.insert(record.page_uuid).second),E::invalid_identity);
    covered+=m.states.size();maps.push_back(std::move(m));
  }
  Require(covered==maps.front().total_pages&&!maps.back().next,E::binding_mismatch);
  const auto allocated=[&](const Header& h,const Uuid& owner){auto map=std::upper_bound(maps.begin(),maps.end(),h.page_number,[](u64 n,const auto& m){return n<m.first_page;});Require(map!=maps.begin(),E::binding_mismatch);--map;
    Require(h.page_number-map->first_page<map->states.size()&&map->states[h.page_number-map->first_page]==page::NativeAllocationState::allocated,E::binding_mismatch);
    const auto r=std::lower_bound(map->records.begin(),map->records.end(),h.page_number,[](const auto& r,u64 n){return r.page_number<n;});
    Require(r!=map->records.end()&&r->page_number==h.page_number&&r->page_uuid==h.page_uuid&&r->page_generation==h.page_generation&&r->page_type==h.page_type&&r->owner_uuid==owner&&r->creator_transaction_uuid.is_nil()&&!r->creator_local_transaction_id&&r->creator_operation_uuid==root.operation_uuid&&!r->reuse_horizon,E::binding_mismatch);};
  for(const auto& m:maps)allocated(m.header,m.object_uuid);
  for(const auto& h:headers)allocated(h,root.object_uuid);
  transaction::mga::LocalTransactionInventory inventory;
  std::optional<disk::NativePageReference> previous,next;
  Uuid object;
  for(std::size_t i=0;i<inventory_images.size();++i){const auto& raw=inventory_images[i];Require(raw.size()==size,E::invalid_allocation);
    const auto common=disk::DecodeNativeCommonPageHeader(raw.data(),128);Require(common.ok(),E::invalid_header);
    if(common.header->flags&1u)throw E::encrypted_requires_authority;
    auto decoded=page::DecodeNativeTransactionInventoryPage(raw);
    if(!decoded.ok())throw decoded.error==page::NativeInventoryError::resource_exhausted?E::resource_exhausted:decoded.error==page::NativeInventoryError::hash_failure?E::hash_failure:E::invalid_allocation;
    const auto& p=*decoded.page;const auto& h=p.header;
    Require(!h.flags&&h.database_uuid==db&&h.filespace_uuid==root.first.filespace_uuid&&h.page_size_profile_uuid==root.first.page_size_profile_uuid&&h.page_generation==root.first.page_generation&&p.inventory_generation==root.first.page_generation,E::binding_mismatch);
    Require(slots.insert(h.page_number).second&&page_ids.insert(h.page_uuid).second,E::invalid_identity);
    Require(p.previous==previous&&(!i||next==Self(h)),E::binding_mismatch);
    if(!i){object=p.object_uuid;inventory.next_local_transaction_id=p.inventory.next_local_transaction_id;inventory.next_commit_sequence=p.inventory.next_commit_sequence;}
    else Require(p.object_uuid==object&&p.inventory.next_local_transaction_id==inventory.next_local_transaction_id&&p.inventory.next_commit_sequence==inventory.next_commit_sequence,E::binding_mismatch);
    if(!inventory.entries.empty()&&!p.inventory.entries.empty())Require(inventory.entries.back().identity.local_id.value<p.inventory.entries.front().identity.local_id.value,E::binding_mismatch);
    inventory.entries.insert(inventory.entries.end(),p.inventory.entries.begin(),p.inventory.entries.end());
    previous=Self(h);next=p.next;allocated(h,p.object_uuid);
  }
  Require(!next,E::binding_mismatch);
  if(!inventory_images.empty())Require(!*transaction::mga::ValidateLocalTransactionInventoryStructure(inventory),E::invalid_allocation);
  return maps.front().total_pages;
}
NativeManagementControlBundleRead Decode(const Pages& pages,const Root& r,const Uuid& db,const Uuid& bootstrap,u64 budget){
  const auto size=Shape(r,db,bootstrap,budget),capacity=size-384;Require(pages.size()==r.page_count,E::invalid_extent);const u64 total=(r.map_count+r.inventory_count)*size;
  Bytes payload;payload.reserve(static_cast<std::size_t>(total));std::vector<Header> headers;headers.reserve(pages.size());std::set<Uuid> ids;auto next=r.first_page_sha256;
  for(std::size_t i=0;i<pages.size();++i){const auto& b=pages[i];Require(b.size()==size,E::invalid_header);const auto h=disk::DecodeNativeCommonPageHeader(b.data(),128);Require(h.ok(),E::invalid_header);Common(*h.header,r,db,bootstrap,size,i,ids);headers.push_back(*h.header);
    Require(Hash(b)==next,E::invalid_integrity);const auto seal=Seal(b);Require(std::equal(seal.begin(),seal.end(),b.begin()+320),E::invalid_integrity);
    const u64 offset=u64{i}*capacity;const u32 length=static_cast<u32>(std::min<u64>(capacity,total-offset));const auto* f=b.data()+128;
    Require(std::string_view(reinterpret_cast<const char*>(f),8)==(r.inventory_count?"SBMCB002":"SBMCB001")&&LoadLittle16(f+8)==(r.inventory_count?2:1)&&LoadLittle16(f+10)==256&&LoadLittle32(f+12)==384+length&&LoadLittle64(f+64)==r.map_count&&LoadLittle64(f+72)==total&&LoadLittle64(f+80)==offset&&LoadLittle64(f+88)==i&&LoadLittle64(f+96)==r.page_count&&LoadLittle64(f+104)==r.first.page_number&&LoadLittle32(f+144)==length&&LoadLittle64(f+148)==r.inventory_count&&Zero(f+156,4)&&Zero(f+224,32)&&Zero(b.data()+384+length,b.size()-384-length),E::invalid_header);
    Require(Get(f+16)==r.object_uuid&&Get(f+32)==bootstrap&&Get(f+48)==r.operation_uuid&&std::equal(r.aggregate_sha256.begin(),r.aggregate_sha256.end(),f+112),E::binding_mismatch);
    std::copy_n(f+160,32,next.begin());Require((i+1==pages.size())==Zero(next.data(),32),E::invalid_integrity);payload.insert(payload.end(),b.begin()+384,b.begin()+384+length);
  }
  Require(Hash(payload)==r.aggregate_sha256,E::invalid_integrity);Pages images,inventory;images.reserve(static_cast<std::size_t>(r.map_count));inventory.reserve(static_cast<std::size_t>(r.inventory_count));
  for(std::size_t at=0;at<payload.size();at+=size)(at/size<r.map_count?images:inventory).emplace_back(payload.begin()+at,payload.begin()+at+size);
  const auto total_pages=Maps(images,inventory,headers,r,db,size);return {E::none,std::move(images),std::move(headers),total_pages,std::move(inventory)};
}
} // namespace
NativeManagementControlBundleError ValidateNativeManagementControlBundleRoot(const Root& r,const Uuid& db,const Uuid& bootstrap,u64 budget) noexcept {
  try{Shape(r,db,bootstrap,budget);return E::none;}catch(E e){return e;}catch(const std::bad_alloc&){return E::resource_exhausted;}catch(const std::length_error&){return E::resource_exhausted;}catch(...){return E::invalid_extent;}
}
NativeManagementControlBundleImage EncodeNativeManagementControlBundle(const Pages& maps,const Uuid& db,const Uuid& bootstrap,const Uuid& object,const Uuid& attempt,const std::vector<Header>& headers,u64 budget,const Pages& inventory) noexcept {
  try{
    Require(!headers.empty()&&!maps.empty(),E::invalid_request);Root root;root.first=Self(headers.front());root.object_uuid=object;root.operation_uuid=attempt;root.map_count=maps.size();root.inventory_count=inventory.size();root.page_count=headers.size();
    const auto size=Shape(root,db,bootstrap,budget,false),capacity=size-384;std::set<Uuid> ids;for(std::size_t i=0;i<headers.size();++i)Common(headers[i],root,db,bootstrap,size,i,ids);Maps(maps,inventory,headers,root,db,size);
    Bytes payload;payload.reserve(static_cast<std::size_t>((root.map_count+root.inventory_count)*size));for(const auto* chain:{&maps,&inventory})for(const auto& b:*chain)payload.insert(payload.end(),b.begin(),b.end());root.aggregate_sha256=Hash(payload);
    Pages pages(headers.size());std::array<byte,32> next{};
    for(std::size_t left=pages.size();left;--left){const auto i=left-1;auto& b=pages[i];b.resize(size);const auto header=disk::EncodeNativeCommonPageHeader(headers[i]);Require(header.ok(),E::invalid_header);std::copy(header.bytes->begin(),header.bytes->end(),b.begin());auto* f=b.data()+128;const u64 offset=u64{i}*capacity;const u32 length=static_cast<u32>(std::min<u64>(capacity,payload.size()-offset));
      std::copy_n(root.inventory_count?"SBMCB002":"SBMCB001",8,f);StoreLittle16(f+8,root.inventory_count?2:1);StoreLittle16(f+10,256);StoreLittle32(f+12,384+length);Put(f+16,object);Put(f+32,bootstrap);Put(f+48,attempt);StoreLittle64(f+64,root.map_count);StoreLittle64(f+72,payload.size());StoreLittle64(f+80,offset);StoreLittle64(f+88,i);StoreLittle64(f+96,root.page_count);StoreLittle64(f+104,root.first.page_number);std::copy(root.aggregate_sha256.begin(),root.aggregate_sha256.end(),f+112);StoreLittle32(f+144,length);StoreLittle64(f+148,root.inventory_count);std::copy(next.begin(),next.end(),f+160);std::copy_n(payload.begin()+offset,length,b.begin()+384);
      const auto seal=Seal(b);std::copy(seal.begin(),seal.end(),f+192);next=Hash(b);
    }
    root.first_page_sha256=next;return {E::none,root,std::move(pages)};
  }catch(E e){return Fail<NativeManagementControlBundleImage>(e);}catch(const std::bad_alloc&){return Fail<NativeManagementControlBundleImage>(E::resource_exhausted);}catch(const std::length_error&){return Fail<NativeManagementControlBundleImage>(E::resource_exhausted);}catch(...){return Fail<NativeManagementControlBundleImage>(E::invalid_extent);}
}
NativeManagementControlBundleRead DecodeNativeManagementControlBundle(const Pages& pages,const Root& r,const Uuid& db,const Uuid& bootstrap,u64 budget) noexcept {
  try{return Decode(pages,r,db,bootstrap,budget);}catch(E e){return Fail<NativeManagementControlBundleRead>(e);}catch(const std::bad_alloc&){return Fail<NativeManagementControlBundleRead>(E::resource_exhausted);}catch(const std::length_error&){return Fail<NativeManagementControlBundleRead>(E::resource_exhausted);}catch(...){return Fail<NativeManagementControlBundleRead>(E::invalid_extent);}
}
NativeManagementControlBundleRead ReadNativeManagementControlBundleFromOpenDevice(const disk::NativeFilespaceDevice& file,const Root& r,const Uuid& db,const Uuid& bootstrap,u64 budget) noexcept {
  try{
    const auto size=Shape(r,db,bootstrap,budget);Require(file.device&&file.filespace_uuid==r.first.filespace_uuid&&file.page_size_profile_uuid==r.first.page_size_profile_uuid,E::invalid_request);const auto guard=file.device->AcquireOperationGuard();Require(file.device->is_open(),E::invalid_request);
    const disk::FilespaceBootstrapBinding binding{db,file.filespace_uuid,file.page_size_profile_uuid};const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*file.device,&binding);
    if(!zero.ok())throw zero.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:zero.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:zero.error==disk::FilespacePageZeroError::io_failure?E::io_failure:E::bootstrap_failure;
    const auto& z=*zero.record;Require(z.page_uuid==bootstrap,E::binding_mismatch);Require(!(z.bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted),E::encrypted_requires_authority);Require(!(z.bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required),E::cluster_requires_authority);
    Require(std::any_of(z.roots.begin(),z.roots.end(),[](const auto& v){return v.kind==20;})&&std::any_of(z.roots.begin(),z.roots.end(),[](const auto& v){return v.kind==21;}),E::bootstrap_failure);
    Require(r.first.page_number<z.total_pages&&r.page_count<=z.total_pages-r.first.page_number,E::invalid_extent);
    for(const auto& ref:z.roots)if(ref.filespace_uuid==file.filespace_uuid)Require(ref.page_number<r.first.page_number||ref.page_number-r.first.page_number>=r.page_count,E::invalid_extent);
    Pages pages(static_cast<std::size_t>(r.page_count));for(std::size_t i=0;i<pages.size();++i){auto& b=pages[i];b.resize(size);const auto io=file.device->ReadAt((r.first.page_number+i)*u64{size},b.data(),b.size());Require(io.ok()&&io.bytes_transferred==b.size(),E::io_failure);}
    auto decoded=Decode(pages,r,db,bootstrap,budget);Require(decoded.total_pages==z.total_pages,E::binding_mismatch);return decoded;
  }catch(E e){return Fail<NativeManagementControlBundleRead>(e);}catch(const std::bad_alloc&){return Fail<NativeManagementControlBundleRead>(E::resource_exhausted);}catch(const std::length_error&){return Fail<NativeManagementControlBundleRead>(E::resource_exhausted);}catch(...){return Fail<NativeManagementControlBundleRead>(E::io_failure);}
}
} // namespace scratchbird::storage::database

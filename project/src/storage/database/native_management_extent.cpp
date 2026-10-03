// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_extent.hpp"
#include "filespace_page_zero.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "uuid.hpp"
#include "../disk/native_decoded_storage_ranges.hpp"
#include <type_traits>
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace scratchbird::storage::database {
namespace {
using E=NativeManagementExtentError;
using Root=NativeManagementExtentRoot;
using Bytes=std::vector<byte>;
using namespace core::platform;
void Require(bool ok,E e){if(!ok)throw e;}
bool V7(const Uuid& id){return core::uuid::IsEngineIdentityUuid(id);}
bool Zero(const byte* p,std::size_t n){return std::all_of(p,p+n,[](byte b){return !b;});}
void Put(byte* p,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),p);}
Uuid Get(const byte* p){Uuid id;std::copy_n(p,16,id.bytes.begin());return id;}
E RecordError(NativeManagementOperationError e){return e==NativeManagementOperationError::resource_exhausted?E::resource_exhausted:e==NativeManagementOperationError::hash_failure?E::hash_failure:E::invalid_record;}
template<class T>T Fail(E e){T r;r.error=e;return r;}
auto Hash(const Bytes& b){const auto r=core::hash::ComputeSha256Digest(b);Require(r.ok(),E::hash_failure);return r.digest;}
auto Seal(const Bytes& b){const std::array<byte,32> zero{};const core::hash::HashDigestSegment parts[]={{b.data(),320},{zero.data(),32},{b.data()+352,b.size()-352}};
  const auto r=core::hash::ComputeSha256DigestParts(parts,3);Require(r.ok(),E::hash_failure);return r.digest;}
E CheckShape(const Root& r,const Uuid& database,const Uuid& bootstrap,u64 budget,u32& size,bool require_root_digest=true){
  const auto* profile=disk::FindCanonicalFilespacePageProfile(r.first.page_size_profile_uuid);
  if(!profile||!V7(database)||!V7(bootstrap)||!V7(r.object_uuid)||!V7(r.operation_uuid)||
     !V7(r.first.filespace_uuid))return E::invalid_identity;
  const std::array ids{database,bootstrap,r.object_uuid,r.operation_uuid};
  for(std::size_t i=0;i<ids.size();++i)for(std::size_t j=0;j<i;++j)
    if(ids[i]==ids[j])return E::invalid_identity;
  const u64 p=profile->page_size_bytes,c=p-384;
  if(r.aggregate_bytes<513||!r.revision||!r.first.page_number||!r.first.page_generation||
     r.page_count!=(u64{r.aggregate_bytes}+c-1)/c||!r.page_count)return E::invalid_extent;
  if(Zero(r.aggregate_sha256.data(),32)||(require_root_digest&&Zero(r.first_page_sha256.data(),32)))
    return E::invalid_integrity;
  if(r.first.page_number>std::numeric_limits<u64>::max()/p||
     u64{r.page_count}-1>std::numeric_limits<u64>::max()/p-r.first.page_number)
    return E::invalid_extent;
  const u64 offset=(r.first.page_number+r.page_count-1)*p;
  // Same signed stream extent limits as CheckFileDeviceExtent, without its
  // owning diagnostic rendering on a rejected untrusted physical reference.
  const u64 max_offset=static_cast<u64>(std::numeric_limits<std::streamoff>::max());
  const u64 max_bytes=static_cast<u64>(std::numeric_limits<std::streamsize>::max());
  if(p>max_bytes||offset>max_offset||p>max_offset-offset)return E::invalid_extent;
  const u64 need=u64{r.page_count}*p+4*u64{r.aggregate_bytes}+2*p;
  if(need>budget)return E::resource_exhausted;
  size=static_cast<u32>(p);return E::none;
}
u32 Shape(const Root& r,const Uuid& database,const Uuid& bootstrap,u64 budget,bool require_root_digest=true){
  u32 size=0;const auto error=CheckShape(r,database,bootstrap,budget,size,require_root_digest);
  Require(error==E::none,error);return size;
}
struct OwnedIdentities {
  std::set<Uuid> values;
  bool Insert(const Uuid& id){return values.insert(id).second;}
};
struct BoundedIdentities {
  std::span<Uuid> values;
  explicit BoundedIdentities(std::span<Uuid> slots):values(slots){
    std::fill(values.begin(),values.end(),Uuid{});
  }
  bool Insert(const Uuid& id){
    u64 hash=14695981039346656037ULL;
    for(auto b:id.bytes){hash^=b;hash*=1099511628211ULL;}
    auto at=hash%values.size();
    for(std::size_t n=0;n<values.size();++n){
      if(values[at].is_nil()){values[at]=id;return true;}
      if(values[at]==id)return false;
      if(++at==values.size())at=0;
    }
    return false;
  }
};
template<class Ids>
E Common(const disk::NativeCommonPageHeader& h,const Root& r,const Uuid& database,
    const Uuid& bootstrap,u32 size,u32 index,Ids& ids){
  if(h.database_uuid!=database||h.filespace_uuid!=r.first.filespace_uuid||
     h.page_size_profile_uuid!=r.first.page_size_profile_uuid||h.page_size_bytes!=size||
     h.page_number!=r.first.page_number+index||h.page_generation!=r.first.page_generation||
     h.page_type!=0x500||h.flags)return E::binding_mismatch;
  if(!V7(h.page_uuid)||h.page_uuid==database||h.page_uuid==bootstrap||
     h.page_uuid==r.object_uuid||h.page_uuid==r.operation_uuid||!ids.Insert(h.page_uuid))
    return E::invalid_identity;
  return E::none;
}
bool WorkspaceValid(std::span<const std::span<const byte>> pages,const Root& root,
    const Uuid& database,const Uuid& bootstrap,const NativeManagementExtentViewWorkspace& w){
  using disk::detail::DisjointNativeDecodeRegions;
  if((reinterpret_cast<std::uintptr_t>(w.page_headers.data())%alignof(disk::NativeCommonPageHeader))||
     (reinterpret_cast<std::uintptr_t>(w.steps.data())%alignof(NativeManagementStepView))||
     (reinterpret_cast<std::uintptr_t>(w.identities.data())%alignof(Uuid)))return false;
  const auto excludes=[&](auto input){
    return DisjointNativeDecodeRegions(input,w.aggregate,w.page_headers,w.steps,w.identities);
  };
  if(!excludes(pages)||!excludes(std::span(&root,1))||
     !excludes(std::span(&database,1))||!excludes(std::span(&bootstrap,1)))return false;
  for(const auto page:pages)if(!excludes(page))return false;
  return true;
}
template<bool Borrowed,class Pages>
auto Decode(const Pages& pages,const Root& r,const Uuid& database,const Uuid& bootstrap,u64 budget,
            NativeManagementExtentViewWorkspace workspace={})
 -> std::conditional_t<Borrowed,NativeManagementExtentViewRead,NativeManagementExtentRead> {
  using Result=std::conditional_t<Borrowed,NativeManagementExtentViewRead,NativeManagementExtentRead>;
  const auto fail=[](E e){return Fail<Result>(e);};
  u32 size=0;const auto shape=CheckShape(r,database,bootstrap,budget,size);
  if(shape!=E::none)return fail(shape);
  const auto capacity=size-384;
  if(pages.size()!=r.page_count)return fail(E::invalid_extent);
  using Aggregate=std::conditional_t<Borrowed,std::span<byte>,Bytes>;
  using Headers=std::conditional_t<Borrowed,std::span<disk::NativeCommonPageHeader>,
                                  std::vector<disk::NativeCommonPageHeader>>;
  Aggregate aggregate;Headers headers;
  if constexpr(Borrowed){
    if(!WorkspaceValid(pages,r,database,bootstrap,workspace))return fail(E::invalid_workspace);
    const u64 count=2*u64{r.page_count}+1;
    if(workspace.aggregate.size()<r.aggregate_bytes||workspace.page_headers.size()<r.page_count||
       count>workspace.identities.size())return fail(E::resource_exhausted);
    aggregate=workspace.aggregate.first(r.aggregate_bytes);
    headers=workspace.page_headers.first(r.page_count);
  }else{
    aggregate.resize(r.aggregate_bytes);headers.resize(r.page_count);
  }
  auto ids=[&](){
    if constexpr(Borrowed)return BoundedIdentities(workspace.identities.first(2*std::size_t{r.page_count}+1));
    else return OwnedIdentities{};
  }();
  auto expected=r.first_page_sha256;
  for(u32 i=0;i<r.page_count;++i){
    const std::span<const byte> b=pages[i];
    if(b.size()!=size)return fail(E::invalid_header);
    const auto h=disk::DecodeNativeCommonPageHeader(b.data(),128);
    if(!h.ok())return fail(E::invalid_header);
    const auto common=Common(*h.header,r,database,bootstrap,size,i,ids);
    if(common!=E::none)return fail(common);
    headers[i]=*h.header;
    const auto digest=core::hash::ComputeSha256DigestNative(b.data(),b.size());
    if(!digest.ok())return fail(E::hash_failure);
    if(digest.digest!=expected)return fail(E::invalid_integrity);
    const std::array<byte,32> zero{};
    const core::hash::HashDigestSegment parts[]={{b.data(),320},{zero.data(),32},{b.data()+352,b.size()-352}};
    const auto seal=core::hash::ComputeSha256DigestPartsNative(parts,3);
    if(!seal.ok())return fail(E::hash_failure);
    if(!std::equal(seal.digest.begin(),seal.digest.end(),b.begin()+320))return fail(E::invalid_integrity);
    const auto* f=b.data()+128;const u64 offset=u64{i}*capacity;
    const u32 length=std::min<u64>(capacity,u64{r.aggregate_bytes}-offset);
    if(std::string_view(reinterpret_cast<const char*>(f),8)!="SBMGP001"||
       LoadLittle16(f+8)!=1||LoadLittle16(f+10)!=256||LoadLittle32(f+12)!=384+length||
       LoadLittle32(f+72)!=r.aggregate_bytes||LoadLittle32(f+76)!=offset||
       LoadLittle32(f+80)!=i||LoadLittle32(f+84)!=r.page_count||
       LoadLittle64(f+88)!=r.first.page_number||LoadLittle32(f+128)!=length||
       !Zero(f+132,28)||!Zero(f+224,32)||!Zero(b.data()+384+length,b.size()-384-length))
      return fail(E::invalid_header);
    if(Get(f+16)!=r.object_uuid||Get(f+32)!=bootstrap||Get(f+48)!=r.operation_uuid||
       LoadLittle64(f+64)!=r.revision||!std::equal(r.aggregate_sha256.begin(),r.aggregate_sha256.end(),f+96))
      return fail(E::binding_mismatch);
    std::copy_n(f+160,32,expected.begin());
    if((i+1==r.page_count)!=Zero(expected.data(),32))return fail(E::invalid_integrity);
    std::copy_n(b.begin()+384,length,aggregate.begin()+offset);
  }
  auto decoded=[&](){
    if constexpr(Borrowed)return DecodeNativeManagementOperationInto(aggregate,r.aggregate_bytes,{workspace.steps,workspace.identities});
    else return DecodeNativeManagementOperation(aggregate,r.aggregate_bytes);
  }();
  if(!decoded.ok())return fail(RecordError(decoded.error));
  if(decoded.sha256!=r.aggregate_sha256||decoded.record->database_uuid!=database||
     decoded.record->bootstrap_uuid!=bootstrap||decoded.record->uuid!=r.operation_uuid||
     decoded.record->revision!=r.revision)return fail(E::binding_mismatch);
  if constexpr(Borrowed)return {E::none,std::move(decoded.record),headers,aggregate};
  else return {E::none,std::move(decoded.record),std::move(headers)};
}
} // namespace
NativeManagementExtentError ValidateNativeManagementExtentRoot(const Root& root,const Uuid& database,const Uuid& bootstrap,u64 budget) noexcept {
  u32 size=0;return CheckShape(root,database,bootstrap,budget,size);
}
NativeManagementExtentImage EncodeNativeManagementExtent(const NativeManagementOperation& record,const Uuid& object,
    const std::vector<disk::NativeCommonPageHeader>& headers,u64 budget) noexcept {
  try{
    Require(!headers.empty(),E::invalid_request);
    auto aggregate=EncodeNativeManagementOperation(record,budget);Require(aggregate.ok(),RecordError(aggregate.error));
    const auto& h=headers.front();Root root;root.first={h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};root.object_uuid=object;
    root.operation_uuid=record.uuid;root.revision=record.revision;root.aggregate_bytes=aggregate.bytes.size();
    Require(headers.size()<=std::numeric_limits<u32>::max(),E::resource_exhausted);root.page_count=headers.size();root.aggregate_sha256=aggregate.sha256;
    // The root commitment is produced only after all actual page hashes exist.
    const auto size=Shape(root,record.database_uuid,record.bootstrap_uuid,budget,false),capacity=size-384;
    OwnedIdentities ids;for(u32 i=0;i<root.page_count;++i){const auto error=Common(headers[i],root,record.database_uuid,record.bootstrap_uuid,size,i,ids);Require(error==E::none,error);}
    std::vector<Bytes> pages(root.page_count);std::array<byte,32> next{};
    for(u32 remaining=root.page_count;remaining;--remaining){const auto i=remaining-1;const auto encoded=disk::EncodeNativeCommonPageHeader(headers[i]);Require(encoded.ok(),E::invalid_header);
      auto& b=pages[i];b.resize(size);std::copy(encoded.bytes->begin(),encoded.bytes->end(),b.begin());auto* f=b.data()+128;
      const u64 offset=u64{i}*capacity;const u32 length=std::min<u64>(capacity,u64{root.aggregate_bytes}-offset);
      std::copy_n("SBMGP001",8,f);StoreLittle16(f+8,1);StoreLittle16(f+10,256);StoreLittle32(f+12,384+length);
      Put(f+16,object);Put(f+32,record.bootstrap_uuid);Put(f+48,record.uuid);StoreLittle64(f+64,record.revision);
      StoreLittle32(f+72,root.aggregate_bytes);StoreLittle32(f+76,offset);StoreLittle32(f+80,i);StoreLittle32(f+84,root.page_count);StoreLittle64(f+88,h.page_number);
      std::copy(aggregate.sha256.begin(),aggregate.sha256.end(),f+96);StoreLittle32(f+128,length);std::copy(next.begin(),next.end(),f+160);
      std::copy_n(aggregate.bytes.begin()+offset,length,b.begin()+384);const auto seal=Seal(b);std::copy(seal.begin(),seal.end(),f+192);next=Hash(b);
    }
    root.first_page_sha256=next;return {E::none,root,std::move(pages)};
  }catch(E e){return Fail<NativeManagementExtentImage>(e);}catch(const std::bad_alloc&){return Fail<NativeManagementExtentImage>(E::resource_exhausted);}
  catch(const std::length_error&){return Fail<NativeManagementExtentImage>(E::resource_exhausted);}catch(...){return Fail<NativeManagementExtentImage>(E::invalid_record);}
}
NativeManagementExtentRead DecodeNativeManagementExtent(const std::vector<Bytes>& pages,const Root& r,const Uuid& database,const Uuid& bootstrap,u64 budget) noexcept {
  try{return Decode<false>(pages,r,database,bootstrap,budget);}
  catch(E e){return Fail<NativeManagementExtentRead>(e);}catch(const std::bad_alloc&){return Fail<NativeManagementExtentRead>(E::resource_exhausted);}
  catch(const std::length_error&){return Fail<NativeManagementExtentRead>(E::resource_exhausted);}catch(...){return Fail<NativeManagementExtentRead>(E::invalid_record);}
}
NativeManagementExtentViewRead DecodeNativeManagementExtentInto(
    std::span<const std::span<const byte>> pages,const Root& r,const Uuid& database,
    const Uuid& bootstrap,u64 budget,NativeManagementExtentViewWorkspace workspace) noexcept {
  return Decode<true>(pages,r,database,bootstrap,budget,workspace);
}
NativeManagementExtentRead ReadNativeManagementExtentFromOpenDevice(const disk::NativeFilespaceDevice& file,const Root& r,
    const Uuid& database,const Uuid& bootstrap,u64 budget) noexcept {
  try{
    const auto size=Shape(r,database,bootstrap,budget);
    Require(file.device&&file.filespace_uuid==r.first.filespace_uuid&&file.page_size_profile_uuid==r.first.page_size_profile_uuid,E::invalid_request);
    auto guard=file.device->AcquireOperationGuard();Require(file.device->is_open(),E::invalid_request);
    const disk::FilespaceBootstrapBinding binding{database,file.filespace_uuid,file.page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*file.device,&binding);
    if(!zero.ok())throw zero.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:
      zero.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:zero.error==disk::FilespacePageZeroError::io_failure?E::io_failure:E::bootstrap_failure;
    const auto& z=*zero.record;Require(z.page_uuid==bootstrap,E::binding_mismatch);
    Require(!(z.bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted),E::encrypted_requires_authority);
    Require(!(z.bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required),E::cluster_requires_authority);
    Require(std::any_of(z.roots.begin(),z.roots.end(),[](const auto& v){return v.kind==20;})&&
      std::any_of(z.roots.begin(),z.roots.end(),[](const auto& v){return v.kind==21;}),E::bootstrap_failure);
    Require(r.first.page_number<z.total_pages&&r.page_count<=z.total_pages-r.first.page_number,E::invalid_extent);
    for(const auto& root:z.roots)if(root.filespace_uuid==file.filespace_uuid)
      Require(root.page_number<r.first.page_number||root.page_number-r.first.page_number>=r.page_count,E::invalid_extent);
    std::vector<Bytes> pages(r.page_count);
    for(u32 i=0;i<r.page_count;++i){auto& b=pages[i];b.resize(size);const auto read=file.device->ReadAt((r.first.page_number+i)*u64{size},b.data(),b.size());
      Require(read.ok()&&read.bytes_transferred==b.size(),E::io_failure);}
    return Decode<false>(pages,r,database,bootstrap,budget);
  }catch(E e){return Fail<NativeManagementExtentRead>(e);}catch(const std::bad_alloc&){return Fail<NativeManagementExtentRead>(E::resource_exhausted);}
  catch(const std::length_error&){return Fail<NativeManagementExtentRead>(E::resource_exhausted);}catch(...){return Fail<NativeManagementExtentRead>(E::io_failure);}
}
} // namespace scratchbird::storage::database

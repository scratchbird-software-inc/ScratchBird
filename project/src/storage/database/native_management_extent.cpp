// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_extent.hpp"
#include "filespace_page_zero.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "uuid.hpp"
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
u32 Shape(const Root& r,const Uuid& database,const Uuid& bootstrap,u64 budget,bool require_root_digest=true){
  const auto* profile=disk::FindCanonicalFilespacePageProfile(r.first.page_size_profile_uuid);
  Require(profile&&V7(database)&&V7(bootstrap)&&V7(r.object_uuid)&&V7(r.operation_uuid)&&V7(r.first.filespace_uuid),E::invalid_identity);
  const std::set<Uuid> ids{database,bootstrap,r.object_uuid,r.operation_uuid};Require(ids.size()==4,E::invalid_identity);
  const u64 p=profile->page_size_bytes,c=p-384;
  Require(r.aggregate_bytes>=513&&r.revision&&r.first.page_number&&r.first.page_generation&&
    r.page_count==(u64{r.aggregate_bytes}+c-1)/c&&r.page_count,E::invalid_extent);
  Require(!Zero(r.aggregate_sha256.data(),32)&&(!require_root_digest||!Zero(r.first_page_sha256.data(),32)),E::invalid_integrity);
  Require(r.first.page_number<=std::numeric_limits<u64>::max()/p&&
    u64{r.page_count}-1<=std::numeric_limits<u64>::max()/p-r.first.page_number&&
    disk::CheckFileDeviceExtent((r.first.page_number+r.page_count-1)*p,static_cast<std::size_t>(p)).ok(),E::invalid_extent);
  const u64 need=u64{r.page_count}*p+4*u64{r.aggregate_bytes}+2*p;
  Require(need<=budget,E::resource_exhausted);return static_cast<u32>(p);
}
void Common(const disk::NativeCommonPageHeader& h,const Root& r,const Uuid& database,
    const Uuid& bootstrap,u32 size,u32 index,std::set<Uuid>& ids){
  Require(h.database_uuid==database&&h.filespace_uuid==r.first.filespace_uuid&&h.page_size_profile_uuid==r.first.page_size_profile_uuid&&
    h.page_size_bytes==size&&h.page_number==r.first.page_number+index&&h.page_generation==r.first.page_generation&&h.page_type==0x500&&!h.flags,E::binding_mismatch);
  Require(V7(h.page_uuid)&&h.page_uuid!=database&&h.page_uuid!=bootstrap&&h.page_uuid!=r.object_uuid&&h.page_uuid!=r.operation_uuid&&ids.insert(h.page_uuid).second,E::invalid_identity);
}
NativeManagementExtentRead Decode(const std::vector<Bytes>& pages,const Root& r,const Uuid& database,const Uuid& bootstrap,u64 budget){
  const auto size=Shape(r,database,bootstrap,budget),capacity=size-384;
  Require(pages.size()==r.page_count,E::invalid_extent);
  Bytes aggregate;aggregate.reserve(r.aggregate_bytes);std::set<Uuid> ids;auto expected=r.first_page_sha256;
  std::vector<disk::NativeCommonPageHeader> headers;headers.reserve(r.page_count);
  for(u32 i=0;i<r.page_count;++i){const auto& b=pages[i];Require(b.size()==size,E::invalid_header);
    const auto h=disk::DecodeNativeCommonPageHeader(b.data(),128);Require(h.ok(),E::invalid_header);Common(*h.header,r,database,bootstrap,size,i,ids);headers.push_back(*h.header);
    Require(Hash(b)==expected,E::invalid_integrity);const auto seal=Seal(b);Require(std::equal(seal.begin(),seal.end(),b.begin()+320),E::invalid_integrity);
    const auto* f=b.data()+128;const u64 offset=u64{i}*capacity;const u32 length=std::min<u64>(capacity,u64{r.aggregate_bytes}-offset);
    Require(std::string_view(reinterpret_cast<const char*>(f),8)=="SBMGP001"&&LoadLittle16(f+8)==1&&LoadLittle16(f+10)==256&&
      LoadLittle32(f+12)==384+length&&LoadLittle32(f+72)==r.aggregate_bytes&&LoadLittle32(f+76)==offset&&LoadLittle32(f+80)==i&&
      LoadLittle32(f+84)==r.page_count&&LoadLittle64(f+88)==r.first.page_number&&LoadLittle32(f+128)==length&&
      Zero(f+132,28)&&Zero(f+224,32)&&Zero(b.data()+384+length,b.size()-384-length),E::invalid_header);
    Require(Get(f+16)==r.object_uuid&&Get(f+32)==bootstrap&&Get(f+48)==r.operation_uuid&&LoadLittle64(f+64)==r.revision&&
      std::equal(r.aggregate_sha256.begin(),r.aggregate_sha256.end(),f+96),E::binding_mismatch);
    std::copy_n(f+160,32,expected.begin());Require((i+1==r.page_count)==Zero(expected.data(),32),E::invalid_integrity);
    aggregate.insert(aggregate.end(),b.begin()+384,b.begin()+384+length);
  }
  auto decoded=DecodeNativeManagementOperation(aggregate,r.aggregate_bytes);Require(decoded.ok(),RecordError(decoded.error));
  Require(decoded.sha256==r.aggregate_sha256&&decoded.record->database_uuid==database&&decoded.record->bootstrap_uuid==bootstrap&&
    decoded.record->uuid==r.operation_uuid&&decoded.record->revision==r.revision,E::binding_mismatch);
  return {E::none,std::move(decoded.record),std::move(headers)};
}
} // namespace
NativeManagementExtentError ValidateNativeManagementExtentRoot(const Root& root,const Uuid& database,const Uuid& bootstrap,u64 budget) noexcept {
  try{Shape(root,database,bootstrap,budget);return E::none;}catch(E e){return e;}
  catch(const std::bad_alloc&){return E::resource_exhausted;}catch(const std::length_error&){return E::resource_exhausted;}catch(...){return E::invalid_extent;}
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
    std::set<Uuid> ids;for(u32 i=0;i<root.page_count;++i)Common(headers[i],root,record.database_uuid,record.bootstrap_uuid,size,i,ids);
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
  try{return Decode(pages,r,database,bootstrap,budget);}
  catch(E e){return Fail<NativeManagementExtentRead>(e);}catch(const std::bad_alloc&){return Fail<NativeManagementExtentRead>(E::resource_exhausted);}
  catch(const std::length_error&){return Fail<NativeManagementExtentRead>(E::resource_exhausted);}catch(...){return Fail<NativeManagementExtentRead>(E::invalid_record);}
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
    return Decode(pages,r,database,bootstrap,budget);
  }catch(E e){return Fail<NativeManagementExtentRead>(e);}catch(const std::bad_alloc&){return Fail<NativeManagementExtentRead>(E::resource_exhausted);}
  catch(const std::length_error&){return Fail<NativeManagementExtentRead>(E::resource_exhausted);}catch(...){return Fail<NativeManagementExtentRead>(E::io_failure);}
}
} // namespace scratchbird::storage::database

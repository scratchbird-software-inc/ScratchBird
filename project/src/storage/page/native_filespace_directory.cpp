// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_filespace_directory.hpp"
#include "native_directory_chain_backing.hpp"
#include "native_metadata_memory.hpp"
#include <map>
#include "native_decoded_storage_ranges.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <string_view>

namespace scratchbird::storage::page {
namespace {
using namespace scratchbird::core::platform;
using E=NativeDirectoryError;
namespace hash=scratchbird::core::hash;
constexpr std::size_t start=384, seal=296, base_width=192, extended_width=320;
bool Zero(const byte* p,std::size_t n){return std::all_of(p,p+n,[](byte b){return !b;});}
bool V7(const Uuid& id){return !id.is_nil()&&(id.bytes[6]>>4)==7&&(id.bytes[8]&0xc0)==0x80;}
Uuid GetUuid(const byte* p){Uuid id;std::copy_n(p,16,id.bytes.begin());return id;}
void PutUuid(byte* p,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),p);}
disk::NativePageReference GetRef(const byte* p){return {GetUuid(p),LoadLittle64(p+16),LoadLittle64(p+24),GetUuid(p+32)};}
void PutRef(byte* p,const disk::NativePageReference& r){PutUuid(p,r.filespace_uuid);StoreLittle64(p+16,r.page_number);StoreLittle64(p+24,r.page_generation);PutUuid(p+32,r.page_size_profile_uuid);}
bool Ref(const disk::NativePageReference& r){const auto* p=disk::FindCanonicalFilespacePageProfile(r.page_size_profile_uuid);
  return V7(r.filespace_uuid)&&p&&r.page_number&&r.page_generation&&r.page_number<std::numeric_limits<u64>::max()/p->page_size_bytes;}
auto Digest(std::span<const byte> b,bool clear){const std::array<byte,32> zero{};
  const hash::HashDigestSegment parts[]={{b.data(),seal},{clear?zero.data():b.data()+seal,32},{b.data()+seal+32,b.size()-seal-32}};
  return hash::ComputeSha256DigestPartsNative(parts,3);}
NativeFilespaceDirectoryResult Fail(E e){NativeFilespaceDirectoryResult r;r.error=e;return r;}
NativeFilespaceDirectoryChainResult ChainFail(E e){NativeFilespaceDirectoryChainResult r;r.error=e;return r;}
bool SameBootstrap(const disk::FilespaceBootstrap& a,const disk::FilespaceBootstrap& b){
  return a.database_uuid==b.database_uuid&&a.filespace_uuid==b.filespace_uuid&&a.page_size_profile_uuid==b.page_size_profile_uuid&&
    a.checksum_profile_uuid==b.checksum_profile_uuid&&a.encryption_profile_uuid==b.encryption_profile_uuid&&
    a.page_size_bytes==b.page_size_bytes&&a.durable_format_generation==b.durable_format_generation&&
    a.flags==b.flags&&a.filespace_role==b.filespace_role&&a.lifecycle_state==b.lifecycle_state;}
template<class Directory> bool Extended(const Directory& d){return !d.creator_operation_uuid.is_nil()||
  std::any_of(d.records.begin(),d.records.end(),[](const auto& r){return r.allocation_root.has_value();});}
template<class Directory> E Validate(const Directory& d,std::span<Uuid> zero_ids={}){const auto& h=d.header;
  if(!disk::EncodeNativeCommonPageHeader(h).ok()||h.page_type!=9||h.flags)return E::invalid_header;
  const bool transaction=V7(d.creator_transaction_uuid)&&d.creator_local_transaction_id&&d.creator_operation_uuid.is_nil();
  const bool operation=V7(d.creator_operation_uuid)&&d.creator_transaction_uuid.is_nil()&&!d.creator_local_transaction_id;
  const auto width=Extended(d)?extended_width:base_width;
  if(!V7(d.object_uuid)||!(transaction||operation)||!d.directory_generation||
      !d.total_records||d.first_record>=d.total_records||d.records.empty()||d.records.size()>d.total_records-d.first_record||
      d.records.size()>(h.page_size_bytes-start)/width)return E::invalid_family;
  if((d.records.size()==d.total_records-d.first_record)==d.next.has_value()||d.next.has_value()==Zero(d.next_sha256.data(),32))return E::invalid_reference;
  if(d.next&&(!Ref(*d.next)||(d.next->filespace_uuid==h.filespace_uuid&&
      (d.next->page_number==h.page_number||d.next->page_size_profile_uuid!=h.page_size_profile_uuid))))return E::invalid_reference;
  std::vector<Uuid> owning_scratch;
  if(zero_ids.empty()){owning_scratch.resize(d.records.size());zero_ids=owning_scratch;}
  if(zero_ids.size()<d.records.size())return E::resource_exhausted;
  zero_ids=zero_ids.first(d.records.size());
  for(std::size_t i=0;i<d.records.size();++i)zero_ids[i]=d.records[i].page_zero_uuid;
  std::sort(zero_ids.begin(),zero_ids.end());
  if(std::adjacent_find(zero_ids.begin(),zero_ids.end())!=zero_ids.end())return E::invalid_record;
  Uuid previous;
  for(const auto& r:d.records){const auto& b=r.bootstrap;
    if(disk::ValidateFilespaceBootstrap(b)!=disk::FilespaceBootstrapError::none||b.database_uuid!=h.database_uuid||
        !(previous<b.filespace_uuid)||!V7(r.locator_uuid)||!V7(r.page_zero_uuid)||
        r.page_zero_uuid==h.page_uuid||!r.page_zero_generation||!r.root_set_generation||!r.total_pages||
        r.total_pages>std::numeric_limits<u64>::max()/b.page_size_bytes)return E::invalid_record;
    if(r.operation&&!Ref(*r.operation))return E::invalid_reference;
    if(r.allocation_root){const auto& a=*r.allocation_root;
      if(!Ref(a.page)||a.page.filespace_uuid!=b.filespace_uuid||a.page.page_size_profile_uuid!=b.page_size_profile_uuid||
          a.page.page_number>=r.total_pages||!V7(a.object_uuid)||Zero(a.sha256.data(),a.sha256.size())||!a.map_generation||!a.capacity_generation||
          (a.page.filespace_uuid==h.filespace_uuid&&a.page.page_number==h.page_number))return E::invalid_reference;
    }
    previous=b.filespace_uuid;
  }
  return E::none;
}
}

NativeFilespaceDirectoryResult EncodeNativeFilespaceDirectory(const NativeFilespaceDirectory& d) noexcept {
  try{const auto valid=Validate(d);if(valid!=E::none)return Fail(valid);
    const bool extended=Extended(d);const auto width=extended?extended_width:base_width;
    std::vector<byte> b(d.header.page_size_bytes,0);const auto common=disk::EncodeNativeCommonPageHeader(d.header);
    if(!common.ok())return Fail(E::invalid_header);std::copy(common.bytes->begin(),common.bytes->end(),b.begin());
    auto* f=b.data()+128;std::copy_n(extended?"SBFDIR02":"SBFDIR01",8,f);StoreLittle16(f+8,extended?2:1);StoreLittle16(f+10,256);StoreLittle32(f+12,start+width*d.records.size());
    PutUuid(f+16,d.object_uuid);StoreLittle64(f+32,d.directory_generation);PutUuid(f+40,d.creator_transaction_uuid);
    StoreLittle64(f+56,d.creator_local_transaction_id);StoreLittle64(f+64,d.total_records);StoreLittle64(f+72,d.first_record);
    StoreLittle32(f+80,d.records.size());if(d.next)PutRef(f+88,*d.next);std::copy(d.next_sha256.begin(),d.next_sha256.end(),f+136);
    if(extended){StoreLittle32(f+84,extended_width);PutUuid(f+200,d.creator_operation_uuid);}
    for(std::size_t i=0;i<d.records.size();++i){auto* p=b.data()+start+i*width;const auto& r=d.records[i];const auto& a=r.bootstrap;
      PutUuid(p,a.filespace_uuid);PutUuid(p+16,a.page_size_profile_uuid);PutUuid(p+32,a.checksum_profile_uuid);PutUuid(p+48,a.encryption_profile_uuid);
      PutUuid(p+64,r.locator_uuid);PutUuid(p+80,r.page_zero_uuid);StoreLittle64(p+96,r.page_zero_generation);StoreLittle64(p+104,r.root_set_generation);
      StoreLittle64(p+112,r.total_pages);StoreLittle64(p+120,r.verification_epoch);StoreLittle16(p+128,a.filespace_role);StoreLittle16(p+130,a.lifecycle_state);
      StoreLittle32(p+132,a.flags);StoreLittle32(p+136,a.page_size_bytes);StoreLittle32(p+140,a.durable_format_generation);if(r.operation)PutRef(p+144,*r.operation);
      if(r.allocation_root){const auto& root=*r.allocation_root;PutRef(p+192,root.page);PutUuid(p+240,root.object_uuid);
        std::copy(root.sha256.begin(),root.sha256.end(),p+256);StoreLittle64(p+288,root.map_generation);StoreLittle64(p+296,root.capacity_generation);}
    }
    const auto digest=Digest(b,true);if(!digest.ok())return Fail(E::hash_failure);std::copy(digest.digest.begin(),digest.digest.end(),b.begin()+seal);
    return {E::none,d,std::move(b)};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_family);}
}
namespace {
template<class Directory,class Prepare> E DecodeValues(std::span<const byte> b,Directory& d,
    Prepare prepare,std::span<Uuid> uniqueness_scratch={}) {
    if(b.size()<start)return E::invalid_header;const auto h=disk::DecodeNativeCommonPageHeader(b.data(),128);
    if(!h.ok()||h.header->page_type!=9||h.header->flags||b.size()!=h.header->page_size_bytes)return E::invalid_header;
    const auto digest=Digest(b,true);if(!digest.ok())return E::hash_failure;
    if(!std::equal(digest.digest.begin(),digest.digest.end(),b.begin()+seal))return E::invalid_integrity;
    const auto* f=b.data()+128;const auto count=LoadLittle32(f+80),used=LoadLittle32(f+12);
    const std::string_view magic(reinterpret_cast<const char*>(f),8);const auto version=LoadLittle16(f+8);
    const bool extended=magic=="SBFDIR02"&&version==2;
    const auto width=extended?extended_width:base_width;
    if((!extended&&(magic!="SBFDIR01"||version!=1))||LoadLittle16(f+10)!=256||
        !count||count>(b.size()-start)/width||used!=start+count*width||!Zero(b.data()+used,b.size()-used)||
        (extended?(LoadLittle32(f+84)!=extended_width||!Zero(f+216,40)):(!Zero(f+84,4)||!Zero(f+200,56))))return E::invalid_family;
    if(!prepare(d,count))return E::resource_exhausted;
    d.header=*h.header;d.object_uuid=GetUuid(f+16);d.directory_generation=LoadLittle64(f+32);
    d.creator_transaction_uuid=GetUuid(f+40);d.creator_local_transaction_id=LoadLittle64(f+56);d.total_records=LoadLittle64(f+64);d.first_record=LoadLittle64(f+72);
    if(extended)d.creator_operation_uuid=GetUuid(f+200);
    if(!Zero(f+88,48))d.next=GetRef(f+88);std::copy_n(f+136,32,d.next_sha256.begin());
    for(u32 i=0;i<count;++i){const auto* p=b.data()+start+i*width;NativeFilespaceDirectoryRecord r;auto& a=r.bootstrap;
      a.database_uuid=h.header->database_uuid;a.filespace_uuid=GetUuid(p);a.page_size_profile_uuid=GetUuid(p+16);a.checksum_profile_uuid=GetUuid(p+32);a.encryption_profile_uuid=GetUuid(p+48);
      r.locator_uuid=GetUuid(p+64);r.page_zero_uuid=GetUuid(p+80);r.page_zero_generation=LoadLittle64(p+96);r.root_set_generation=LoadLittle64(p+104);
      r.total_pages=LoadLittle64(p+112);r.verification_epoch=LoadLittle64(p+120);a.filespace_role=LoadLittle16(p+128);a.lifecycle_state=LoadLittle16(p+130);
      a.flags=LoadLittle32(p+132);a.page_size_bytes=LoadLittle32(p+136);a.durable_format_generation=LoadLittle32(p+140);if(!Zero(p+144,48))r.operation=GetRef(p+144);
      if(extended){if(!Zero(p+304,16))return E::invalid_record;
        if(!Zero(p+192,112)){NativeFilespaceAllocationRoot root;root.page=GetRef(p+192);root.object_uuid=GetUuid(p+240);
          std::copy_n(p+256,32,root.sha256.begin());root.map_generation=LoadLittle64(p+288);root.capacity_generation=LoadLittle64(p+296);r.allocation_root=std::move(root);}}
      d.records[i]=std::move(r);
    }
    if(Extended(d)!=extended)return E::invalid_family;
    return Validate(d,uniqueness_scratch);
}
} // namespace
NativeFilespaceDirectoryResult DecodeNativeFilespaceDirectory(const std::vector<byte>& b) noexcept {
  try{
    NativeFilespaceDirectory d;
    const auto valid=DecodeValues(b,d,[](auto& out,std::size_t count){out.records.resize(count);return true;});
    if(valid!=E::none)return Fail(valid);return {E::none,std::move(d),b};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_family);}
}

NativeFilespaceDirectoryViewResult DecodeNativeFilespaceDirectoryInto(std::span<const byte> b,
    std::span<NativeFilespaceDirectoryRecord> records,std::span<Uuid> scratch) noexcept {
  try{
    if(!detail::DisjointNativeDecodeRegions(b,records,scratch))return {E::invalid_backing,{}};
    NativeFilespaceDirectoryView d;
    const auto valid=DecodeValues(b,d,[&](auto& out,std::size_t count){
      if(count>records.size()||count>scratch.size())return false;
      out.records=records.first(count);return true;
    },scratch);
    if(valid!=E::none)return {valid,{}};return {E::none,std::move(d)};
  }catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}
   catch(const std::length_error&){return {E::resource_exhausted,{}};}
   catch(...){return {E::invalid_family,{}};}
}

static NativeDirectoryChainView ReadDirectoryBacked(
    const Uuid& database_uuid,std::span<const disk::NativeFilespaceDevice> devices,
    const disk::FilespaceRootReference& head,u64 budget,
    const std::span<const NativeHistoricalFilespaceImageView>* history,
    const std::array<byte,32>* root_sha256,std::pmr::memory_resource& resource,
    std::span<disk::FileDevice::ReadLatencyBatch* const> observations,
    NativeDirectoryChainDeviceRead& receipt) noexcept {
  const auto fail=[](E e){NativeDirectoryChainView r;r.error=e;return r;};
  try{
    disk::detail::NativeMetadataScratch scratch{&resource};
    std::pmr::map<disk::FileDevice*,disk::FileDevice::ReadLatencyBatch*> batches(&resource);
    const auto read=[&](disk::FileDevice& device,u64 offset,void* bytes,std::size_t count){
      const auto it=batches.find(&device);
      auto io=it==batches.end()?device.ReadAt(offset,bytes,count):it->second->ReadAt(offset,bytes,count);
      receipt.io_status=io.status;receipt.io_diagnostic=std::move(io.diagnostic);
      if(io.bytes_transferred>std::numeric_limits<u64>::max()-receipt.physical_bytes_read)throw E::invalid_reference;
      receipt.physical_bytes_read+=io.bytes_transferred;return io;
    };
    const auto extent=[&](disk::FileDevice& device){auto io=device.Size();
      receipt.io_status=io.status;receipt.io_diagnostic=std::move(io.diagnostic);return io;};

    if(!V7(database_uuid)||!V7(head.object_uuid)||head.kind!=5||head.page_type!=9||devices.empty()||
        !Ref({head.filespace_uuid,head.page_number,head.page_generation,head.page_size_profile_uuid}))return fail(E::invalid_reference);
    if(history&&(!root_sha256||Zero(root_sha256->data(),root_sha256->size())||history->size()!=devices.size()))
      return fail(E::invalid_reference);
    if(!observations.empty()){
      if(observations.size()!=devices.size())return fail(E::invalid_reference);
      for(std::size_t i=0;i<devices.size();++i){
        if(!devices[i].device||!observations[i]||&observations[i]->device()!=devices[i].device)return fail(E::invalid_reference);
        batches.emplace(devices[i].device,observations[i]);
      }
    }
    std::pmr::vector<disk::NativeFilespaceDevice> ordered(devices.begin(),devices.end(),&resource);std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    std::pmr::vector<std::unique_lock<std::recursive_mutex>> guards(&resource);std::pmr::vector<disk::FilespacePageZeroView> zeros(&resource);
    std::pmr::vector<u64> sizes(&resource);
    std::pmr::vector<const NativeHistoricalFilespaceImageView*> historical_images(&resource);
    u64 retained_bytes=0;
    guards.reserve(ordered.size());zeros.reserve(ordered.size());
    std::pmr::set<disk::FileDevice*> device_ids(&resource);Uuid prior;
    for(const auto& device:ordered){if(!V7(device.filespace_uuid)||!(prior<device.filespace_uuid)||!device.device||!device_ids.insert(device.device).second||
        !disk::FindCanonicalFilespacePageProfile(device.page_size_profile_uuid))return fail(E::invalid_filespace);
      guards.push_back(device.device->AcquireOperationGuard());prior=device.filespace_uuid;}
    if(history){
      sizes.reserve(ordered.size());historical_images.reserve(history->size());
      for(const auto& image:*history)historical_images.push_back(&image);
      std::sort(historical_images.begin(),historical_images.end(),[](const auto* a,const auto* b){return a->filespace_uuid<b->filespace_uuid;});
      for(std::size_t i=0;i<ordered.size();++i)
        if(historical_images[i]->filespace_uuid!=ordered[i].filespace_uuid)return fail(E::invalid_filespace);
    }
    for(const auto& device:ordered){const disk::FilespaceBootstrapBinding binding{database_uuid,device.filespace_uuid,device.page_size_profile_uuid};
      const std::span<const byte>* bytes=nullptr;
      if(history){
        bytes=&historical_images[zeros.size()]->page_zero;
        if(bytes->size()>budget-retained_bytes)return fail(E::resource_exhausted);
        retained_bytes+=bytes->size();
        if(disk::kFilespaceBootstrapBytes>budget-retained_bytes)return fail(E::resource_exhausted);
        retained_bytes+=disk::kFilespaceBootstrapBytes;
      }
      std::span<const byte> zero_image;
      auto zero=bytes?disk::DecodeFilespacePageZeroInto(scratch.Copy<byte>(*bytes),scratch.Array<disk::FilespaceRootReference>(32),&binding):
        disk::detail::ReadNativeMetadataPageZero(binding,scratch,
          [&](u64 offset,void* data,std::size_t count){return read(*device.device,offset,data,count);},
          [&]{return extent(*device.device);},zero_image);
      if(!zero.ok())return fail(zero.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:
        zero.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:
        zero.error==disk::FilespacePageZeroError::io_failure?E::io_failure:E::invalid_filespace);
      if(history){
        auto prefix=scratch.Array<byte>(disk::kFilespaceBootstrapBytes);
        const auto io=read(*device.device,0,prefix.data(),prefix.size());
        if(!io.ok()||io.bytes_transferred!=prefix.size())return fail(E::io_failure);
        const auto actual=disk::DecodeFilespaceBootstrap(prefix.data(),prefix.size(),&binding);
        if(!actual.ok())return fail(actual.error==disk::FilespaceBootstrapError::resource_exhausted?E::resource_exhausted:
          actual.error==disk::FilespaceBootstrapError::hash_provider_failure?E::hash_failure:
          actual.error==disk::FilespaceBootstrapError::io_failure||actual.error==disk::FilespaceBootstrapError::device_not_open?E::io_failure:E::invalid_filespace);
        const auto size=extent(*device.device);if(!size.ok())return fail(E::io_failure);
        if(size.size_bytes<zero.record->total_pages*zero.record->bootstrap.page_size_bytes)return fail(E::invalid_filespace);
        sizes.push_back(size.size_bytes);
      }
      zeros.push_back(std::move(*zero.record));}
    std::pmr::vector<NativeDirectoryChainPageView> pages(&resource);NativeDirectoryChainView result;disk::NativePageReference next{head.filespace_uuid,head.page_number,head.page_generation,head.page_size_profile_uuid};
    result.retained_image_bytes=retained_bytes;
    std::pmr::set<std::pair<Uuid,u64>> slots(&resource);std::pmr::set<Uuid> page_ids(&resource);Uuid last_record;
    u64 ordinal=0;
    while(true){const auto it=std::lower_bound(ordered.begin(),ordered.end(),next.filespace_uuid,[](const auto& a,const auto& b){return a.filespace_uuid<b;});
      if(it==ordered.end()||it->filespace_uuid!=next.filespace_uuid||it->page_size_profile_uuid!=next.page_size_profile_uuid)return fail(E::invalid_filespace);
      const auto& zero=zeros[static_cast<std::size_t>(it-ordered.begin())];const auto size=zero.bootstrap.page_size_bytes;
      if(zero.bootstrap.filespace_role>4||next.page_number>=zero.total_pages)return fail(E::invalid_filespace);
      if(!slots.insert({next.filespace_uuid,next.page_number}).second)return fail(E::chain_mismatch);
      if(size>budget-result.retained_image_bytes)return fail(E::resource_exhausted);
      auto bytes=scratch.Array<byte>(size);const auto io=read(*it->device,next.page_number*size,bytes.data(),size);
      if(!io.ok()||io.bytes_transferred!=size)return fail(E::io_failure);
      if(pages.empty()&&root_sha256){const auto digest=Digest(bytes,false);
        if(!digest.ok())return fail(E::hash_failure);
        if(digest.digest!=*root_sha256)return fail(E::invalid_integrity);}
      const auto count=std::min<std::size_t>(LoadLittle32(bytes.data()+208),(bytes.size()-384)/192);
      auto loaded=DecodeNativeFilespaceDirectoryInto(bytes,scratch.Array<NativeFilespaceDirectoryRecord>(count),scratch.Array<Uuid>(count));
      if(!loaded.ok())return fail(loaded.error);const auto& d=*loaded.directory;const auto& h=d.header;
      if(h.database_uuid!=database_uuid||h.filespace_uuid!=next.filespace_uuid||h.page_size_profile_uuid!=next.page_size_profile_uuid||
          h.page_number!=next.page_number||h.page_generation!=next.page_generation||d.object_uuid!=head.object_uuid)return fail(E::binding_mismatch);
      if(!page_ids.insert(h.page_uuid).second||d.first_record!=ordinal||!(last_record<d.records.front().bootstrap.filespace_uuid))return fail(E::chain_mismatch);
      if(!pages.empty()){const auto& first=pages.front().directory;const auto& prev=pages.back().directory;
        const auto digest=Digest(bytes,false);if(!digest.ok())return fail(E::hash_failure);
        if(digest.digest!=prev.next_sha256)return fail(E::invalid_integrity);
        if(d.directory_generation!=first.directory_generation||d.creator_transaction_uuid!=first.creator_transaction_uuid||
            d.creator_local_transaction_id!=first.creator_local_transaction_id||d.creator_operation_uuid!=first.creator_operation_uuid||d.total_records!=first.total_records)return fail(E::chain_mismatch);}
      ordinal+=d.records.size();last_record=d.records.back().bootstrap.filespace_uuid;
      const auto successor=d.next;result.retained_image_bytes+=size;pages.push_back({d,bytes});
      if(!successor)break;next=*successor;
    }
    std::pmr::vector<const NativeFilespaceDirectoryRecord*> records(&resource);
    for(const auto& image:pages)for(const auto& r:image.directory.records){
      if(!page_ids.insert(r.page_zero_uuid).second)return fail(E::chain_mismatch);records.push_back(&r);}
    const auto lookup=[&](const Uuid& id){return std::lower_bound(records.begin(),records.end(),id,[](const auto* a,const auto& b){return a->bootstrap.filespace_uuid<b;});};
    for(const auto& zero:zeros){const auto it=lookup(zero.bootstrap.filespace_uuid);
      if(it==records.end()||(*it)->bootstrap.filespace_uuid!=zero.bootstrap.filespace_uuid)return fail(E::binding_mismatch);const auto& r=**it;
      if(!SameBootstrap(r.bootstrap,zero.bootstrap)||r.page_zero_uuid!=zero.page_uuid||r.page_zero_generation!=zero.page_generation||
          r.root_set_generation!=zero.root_set_generation||r.total_pages!=zero.total_pages)return fail(E::binding_mismatch);}
    for(const auto* r:records)if(r->operation){const auto it=lookup(r->operation->filespace_uuid);
      if(it!=records.end()&&(*it)->bootstrap.filespace_uuid==r->operation->filespace_uuid&&
          ((*it)->bootstrap.page_size_profile_uuid!=r->operation->page_size_profile_uuid||r->operation->page_number>=(*it)->total_pages))return fail(E::invalid_reference);}
    if(history)for(std::size_t i=0;i<ordered.size();++i){
      const auto size=extent(*ordered[i].device);if(!size.ok())return fail(E::io_failure);
      if(size.size_bytes!=sizes[i])return fail(E::physical_extent_changed);
    }
    result.pages=scratch.Copy<NativeDirectoryChainPageView>(pages);result.error=E::none;return result;
  }catch(E e){return fail(e);}catch(const std::bad_alloc&){return fail(E::resource_exhausted);}catch(const std::length_error&){return fail(E::resource_exhausted);}catch(...){return fail(E::io_failure);}
}


static NativeFilespaceDirectoryChainResult ReadDirectory(
    const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& files,
    const disk::FilespaceRootReference& head,u64 budget,
    const std::vector<NativeHistoricalFilespaceImage>* history=nullptr,
    const std::array<byte,32>* root_sha256=nullptr) noexcept {
  try{
    disk::detail::NativeMetadataHeapMemory heap;std::pmr::monotonic_buffer_resource resource(&heap);
    disk::detail::NativeMetadataScratch scratch{&resource};
    std::span<NativeHistoricalFilespaceImageView> input;
    if(history){input=scratch.Array<NativeHistoricalFilespaceImageView>(history->size());
      for(std::size_t i=0;i<history->size();++i)input[i]={(*history)[i].filespace_uuid,(*history)[i].page_zero};}
    const std::span<const NativeHistoricalFilespaceImageView> retained=input;
    NativeDirectoryChainDeviceRead receipt;
    const auto view=ReadDirectoryBacked(database,files,head,budget,history?&retained:nullptr,
      root_sha256,resource,{},receipt);
    if(!view.ok())return ChainFail(view.error);
    NativeFilespaceDirectoryChainResult out;out.retained_image_bytes=view.retained_image_bytes;
    out.pages.reserve(view.pages.size());
    // Owning compatibility materializes only after complete source verification.
    for(const auto& image:view.pages){
      const auto& d=image.directory;NativeFilespaceDirectory copy;
      copy.header=d.header;copy.object_uuid=d.object_uuid;copy.directory_generation=d.directory_generation;
      copy.creator_transaction_uuid=d.creator_transaction_uuid;copy.creator_local_transaction_id=d.creator_local_transaction_id;
      copy.creator_operation_uuid=d.creator_operation_uuid;copy.total_records=d.total_records;
      copy.first_record=d.first_record;copy.next=d.next;copy.next_sha256=d.next_sha256;
      copy.records.assign(d.records.begin(),d.records.end());
      NativeFilespaceDirectoryResult page;page.error=E::none;page.directory=std::move(copy);
      page.bytes.assign(image.image.begin(),image.image.end());out.pages.push_back(std::move(page));
    }
    out.error=E::none;return out;
  }catch(const std::bad_alloc&){return ChainFail(E::resource_exhausted);}
   catch(const std::length_error&){return ChainFail(E::resource_exhausted);}
   catch(...){return ChainFail(E::io_failure);}
}
NativeDirectoryChainDeviceRead detail::ReadNativeDirectoryChainBacked(
    const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,
    const disk::FilespaceRootReference& head,u64 budget,NativeDirectoryChainReadContext context,
    const std::array<byte,32>* root_sha256,std::span<const NativeHistoricalFilespaceImageView> history,
    std::span<disk::FileDevice::ReadLatencyBatch* const> batches,std::pmr::memory_resource& resource) noexcept {
  NativeDirectoryChainDeviceRead out;using C=NativeDirectoryChainReadContext;
  if((context!=C::current&&context!=C::historical)||
     (context==C::historical?(!root_sha256||history.size()!=files.size()):(root_sha256||!history.empty()))||
     files.empty()||files.size()!=batches.size())return out;
  out.chain=ReadDirectoryBacked(database,files,head,budget,context==C::historical?&history:nullptr,
    root_sha256,resource,batches,out);return out;
}
NativeDirectoryChainDeviceRead ReadNativeFilespaceDirectoryChainInto(
    const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,
    const disk::FilespaceRootReference& head,u64 budget,NativeDirectoryChainReadContext context,
    const std::array<byte,32>* root_sha256,std::span<const NativeHistoricalFilespaceImageView> history,
    std::span<disk::FileDevice::ReadLatencyBatch* const> batches,std::span<byte> backing) noexcept {
  NativeDirectoryChainDeviceRead out;using C=NativeDirectoryChainReadContext;
  if((context!=C::current&&context!=C::historical)||
     (context==C::historical?(!root_sha256||history.size()!=files.size()):(root_sha256||!history.empty()))||
     files.empty()||files.size()!=batches.size())return out;
  for(std::size_t i=0;i<files.size();++i)if(!files[i].device||!batches[i]||&batches[i]->device()!=files[i].device)return out;
  const auto disjoint=[&](auto input){return disk::detail::DisjointNativeDecodeRegions(backing,input);};
  bool valid=disjoint(std::span{&database,1})&&disjoint(files)&&disjoint(std::span{&head,1})&&
    disjoint(history)&&disjoint(batches)&&(!root_sha256||disjoint(std::span{root_sha256,1}));
  for(std::size_t i=0;i<files.size();++i)valid=valid&&disjoint(std::span{files[i].device,1})&&disjoint(std::span{batches[i],1});
  for(const auto& entry:history)valid=valid&&disjoint(entry.page_zero);
  if(!valid){out.chain.error=E::invalid_backing;return out;}
  disk::detail::NativeMetadataMemory resource(backing);
  out=detail::ReadNativeDirectoryChainBacked(database,files,head,budget,context,root_sha256,history,batches,resource);
  if(out.ok())out.chain.backing_bytes_used=resource.used();return out;
}
NativeFilespaceDirectoryChainResult ReadNativeFilespaceDirectoryFromOpenDevices(
    const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,
    const disk::FilespaceRootReference& head,u64 budget) noexcept {
  return ReadDirectory(database,devices,head,budget);
}
NativeFilespaceDirectoryChainResult ReadNativeFilespaceDirectoryAtHistoricalRootFromOpenDevices(
    const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,
    const disk::FilespaceRootReference& head,const std::array<byte,32>& root_sha256,
    const std::vector<NativeHistoricalFilespaceImage>& history,u64 budget) noexcept {
  return ReadDirectory(database,devices,head,budget,&history,&root_sha256);
}
}  // namespace scratchbird::storage::page

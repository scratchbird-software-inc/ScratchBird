// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_retention_root.hpp"
#include "hash_digest_parts.hpp"
#include "disk_device.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <string_view>

namespace scratchbird::storage::page {
namespace {
using namespace scratchbird::core::platform;
using E=NativeRetentionError;
namespace hash=scratchbird::core::hash;
constexpr std::size_t start=384,width=160,seal=328;
bool Zero(const byte* p,std::size_t n){return std::all_of(p,p+n,[](byte b){return !b;});}
bool V7(const Uuid& id){return !id.is_nil()&&(id.bytes[6]>>4)==7&&(id.bytes[8]&0xc0)==0x80;}
Uuid GetUuid(const byte* p){Uuid id;std::copy_n(p,16,id.bytes.begin());return id;}
void PutUuid(byte* p,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),p);}
disk::NativePageReference GetRef(const byte* p){return {GetUuid(p),LoadLittle64(p+16),LoadLittle64(p+24),GetUuid(p+32)};}
void PutRef(byte* p,const disk::NativePageReference& r){PutUuid(p,r.filespace_uuid);StoreLittle64(p+16,r.page_number);StoreLittle64(p+24,r.page_generation);PutUuid(p+32,r.page_size_profile_uuid);}
bool Ref(const disk::NativePageReference& r){const auto* p=disk::FindCanonicalFilespacePageProfile(r.page_size_profile_uuid);return V7(r.filespace_uuid)&&p&&r.page_number&&r.page_generation&&r.page_number<std::numeric_limits<u64>::max()/p->page_size_bytes;}
auto Digest(const byte* b,std::size_t size,std::size_t field,bool clear=true){const std::array<byte,32> zero{};
  const hash::HashDigestSegment parts[]={{b,field},{clear?zero.data():b+field,32},{b+field+32,size-field-32}};return hash::ComputeSha256DigestParts(parts,3);}
NativeRetentionPageResult Fail(E e){NativeRetentionPageResult r;r.error=e;return r;}
NativeRetentionChainResult ChainFail(E e){NativeRetentionChainResult r;r.error=e;return r;}
bool Legal(const NativeRetentionPin& r){return r.kind==NativeRetentionKind::legal_hold||r.access==NativeRetentionAccess::legal_hold;}
E Validate(const NativeRetentionPage& v){const auto& h=v.header;const bool root=h.page_type==0x303;
  if(!disk::EncodeNativeCommonPageHeader(h).ok()||(!root&&h.page_type!=0x304)||h.flags||!h.page_number)return E::invalid_header;
  if(!V7(v.object_uuid)||!V7(v.creator_transaction_uuid)||!v.epoch||!v.creator_local_transaction_id||(v.flags&~u64{1})||v.records.size()>(h.page_size_bytes-start)/width)return E::invalid_family;
  if(root){if(!v.records.empty()||v.first_record||v.legal_hold_pins>v.total_pins)return E::invalid_family;
    if(!v.total_pins){if(v.legal_hold_pins||v.lowest_start||v.highest_end||v.next)return E::invalid_family;}
    else if(!v.lowest_start||(v.highest_end&&v.highest_end<=v.lowest_start)||!v.next)return E::invalid_family;
  }else{if(!v.total_pins||v.first_record>=v.total_pins||v.records.empty()||v.records.size()>v.total_pins-v.first_record||v.legal_hold_pins||v.lowest_start||v.highest_end)return E::invalid_family;
    if((v.records.size()<v.total_pins-v.first_record)!=v.next.has_value())return E::invalid_reference;}
  if(v.next.has_value()==Zero(v.next_sha256.data(),32))return E::invalid_reference;
  if(v.next&&(!Ref(*v.next)||(v.next->filespace_uuid==h.filespace_uuid&&(v.next->page_number==h.page_number||v.next->page_size_profile_uuid!=h.page_size_profile_uuid))))return E::invalid_reference;
  Uuid prior;
  for(const auto& r:v.records){const auto kind=static_cast<u16>(r.kind),access=static_cast<u16>(r.access);
    if(!V7(r.pin_uuid)||!(prior<r.pin_uuid)||!V7(r.owner_uuid)||!V7(r.timeline_uuid)||(!r.filespace_uuid.is_nil()&&!V7(r.filespace_uuid))||
        !kind||kind>8||!access||access>6||!r.start_local||(r.end_local&&r.end_local<=r.start_local)||
        (r.flags&~u32{31})||!r.blocked_operations||(r.blocked_operations&~u64{31})||((r.flags&1)&&!(r.blocked_operations&1))||
        ((r.flags&4)&&((r.flags&2)||!(r.flags&1)))||(Legal(r)&&(r.flags&2))||
        (access==1&&(r.flags&8))||(kind==4&&!(v.flags&1))||(kind==5&&r.filespace_uuid.is_nil()))return E::invalid_record;
    prior=r.pin_uuid;
  }
  return E::none;
}
}
NativeRetentionPageResult EncodeNativeRetentionPage(const NativeRetentionPage& v) noexcept {
  try{const auto valid=Validate(v);if(valid!=E::none)return Fail(valid);std::vector<byte> b(v.header.page_size_bytes,0);
    const auto common=disk::EncodeNativeCommonPageHeader(v.header);if(!common.ok())return Fail(E::invalid_header);std::copy(common.bytes->begin(),common.bytes->end(),b.begin());auto* f=b.data()+128;
    std::copy_n(v.header.page_type==0x303?"SBPIN001":"SBPINL01",8,f);StoreLittle16(f+8,1);StoreLittle16(f+10,256);StoreLittle32(f+12,start+width*v.records.size());
    PutUuid(f+16,v.object_uuid);StoreLittle64(f+32,v.epoch);PutUuid(f+40,v.creator_transaction_uuid);StoreLittle64(f+56,v.creator_local_transaction_id);StoreLittle64(f+64,v.flags);StoreLittle64(f+72,v.total_pins);StoreLittle64(f+80,v.first_record);StoreLittle32(f+88,v.records.size());
    if(v.next)PutRef(f+96,*v.next);std::copy(v.next_sha256.begin(),v.next_sha256.end(),f+144);StoreLittle64(f+176,v.legal_hold_pins);StoreLittle64(f+184,v.lowest_start);StoreLittle64(f+192,v.highest_end);
    for(std::size_t i=0;i<v.records.size();++i){auto* p=b.data()+start+i*width;const auto& r=v.records[i];PutUuid(p,r.pin_uuid);PutUuid(p+16,r.owner_uuid);StoreLittle16(p+32,static_cast<u16>(r.kind));StoreLittle16(p+34,static_cast<u16>(r.access));StoreLittle32(p+36,r.flags);StoreLittle64(p+40,r.start_local);StoreLittle64(p+48,r.end_local);PutUuid(p+56,r.timeline_uuid);PutUuid(p+72,r.filespace_uuid);StoreLittle64(p+88,r.retain_until_local);StoreLittle64(p+96,r.retain_until_unix_ns);StoreLittle64(p+104,r.blocked_operations);
      const auto hash=Digest(p,width,112);if(!hash.ok())return Fail(E::hash_failure);std::copy(hash.digest.begin(),hash.digest.end(),p+112);}
    const auto hash=Digest(b.data(),b.size(),seal);if(!hash.ok())return Fail(E::hash_failure);std::copy(hash.digest.begin(),hash.digest.end(),b.begin()+seal);return {E::none,v,std::move(b)};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_family);}
}
NativeRetentionPageResult DecodeNativeRetentionPage(const std::vector<byte>& b) noexcept {
  try{if(b.size()<start)return Fail(E::invalid_header);const auto header=disk::DecodeNativeCommonPageHeader(b.data(),128);
    if(!header.ok()||(header.header->page_type!=0x303&&header.header->page_type!=0x304)||header.header->flags||b.size()!=header.header->page_size_bytes)return Fail(E::invalid_header);
    const auto hash=Digest(b.data(),b.size(),seal);if(!hash.ok())return Fail(E::hash_failure);if(!std::equal(hash.digest.begin(),hash.digest.end(),b.begin()+seal))return Fail(E::invalid_integrity);
    const auto* f=b.data()+128;const auto count=LoadLittle32(f+88),used=LoadLittle32(f+12);
    if(std::string_view(reinterpret_cast<const char*>(f),8)!=(header.header->page_type==0x303?"SBPIN001":"SBPINL01")||LoadLittle16(f+8)!=1||LoadLittle16(f+10)!=256||count>(b.size()-start)/width||used!=start+count*width||!Zero(f+92,4)||!Zero(f+232,24)||!Zero(b.data()+used,b.size()-used))return Fail(E::invalid_family);
    NativeRetentionPage v;v.header=*header.header;v.object_uuid=GetUuid(f+16);v.epoch=LoadLittle64(f+32);v.creator_transaction_uuid=GetUuid(f+40);v.creator_local_transaction_id=LoadLittle64(f+56);v.flags=LoadLittle64(f+64);v.total_pins=LoadLittle64(f+72);v.first_record=LoadLittle64(f+80);
    if(!Zero(f+96,48))v.next=GetRef(f+96);std::copy_n(f+144,32,v.next_sha256.begin());v.legal_hold_pins=LoadLittle64(f+176);v.lowest_start=LoadLittle64(f+184);v.highest_end=LoadLittle64(f+192);v.records.reserve(count);
    for(u32 i=0;i<count;++i){const auto* p=b.data()+start+i*width;const auto record_hash=Digest(p,width,112);if(!record_hash.ok())return Fail(E::hash_failure);if(!std::equal(record_hash.digest.begin(),record_hash.digest.end(),p+112))return Fail(E::invalid_integrity);if(!Zero(p+144,16))return Fail(E::invalid_record);
      NativeRetentionPin r;r.pin_uuid=GetUuid(p);r.owner_uuid=GetUuid(p+16);r.kind=static_cast<NativeRetentionKind>(LoadLittle16(p+32));r.access=static_cast<NativeRetentionAccess>(LoadLittle16(p+34));r.flags=LoadLittle32(p+36);r.start_local=LoadLittle64(p+40);r.end_local=LoadLittle64(p+48);r.timeline_uuid=GetUuid(p+56);r.filespace_uuid=GetUuid(p+72);r.retain_until_local=LoadLittle64(p+88);r.retain_until_unix_ns=LoadLittle64(p+96);r.blocked_operations=LoadLittle64(p+104);v.records.push_back(r);}
    const auto valid=Validate(v);if(valid!=E::none)return Fail(valid);return {E::none,std::move(v),b};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_family);}
}
NativeRetentionChainResult ReadNativeRetentionRootFromOpenDevices(const Uuid& database_uuid,const std::vector<disk::NativeFilespaceDevice>& devices,
    const Uuid& object_uuid,const disk::NativePageReference& root,u64 budget) noexcept {
  try{if(!V7(database_uuid)||!V7(object_uuid)||!Ref(root)||devices.empty()||!budget)return ChainFail(E::invalid_reference);
    auto ordered=devices;std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    std::vector<std::unique_lock<std::recursive_mutex>> guards;std::map<Uuid,disk::FilespacePageZero> zeros;std::set<disk::FileDevice*> device_ids;std::set<Uuid> zero_ids;Uuid previous;
    guards.reserve(ordered.size());for(const auto& file:ordered){if(!V7(file.filespace_uuid)||!(previous<file.filespace_uuid)||!file.device||!device_ids.insert(file.device).second||!disk::FindCanonicalFilespacePageProfile(file.page_size_profile_uuid))return ChainFail(E::invalid_filespace);guards.push_back(file.device->AcquireOperationGuard());previous=file.filespace_uuid;}
    for(const auto& file:ordered){const disk::FilespaceBootstrapBinding binding{database_uuid,file.filespace_uuid,file.page_size_profile_uuid};auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*file.device,&binding);
      if(!zero.ok())return ChainFail(zero.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:zero.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:zero.error==disk::FilespacePageZeroError::io_failure?E::io_failure:E::invalid_filespace);
      if(!zero_ids.insert(zero.record->page_uuid).second)return ChainFail(E::invalid_filespace);zeros.emplace(file.filespace_uuid,std::move(*zero.record));}
    NativeRetentionChainResult result;auto next=root;std::set<std::pair<Uuid,u64>> slots;std::set<Uuid> page_ids;Uuid prior_pin;u64 pins=0,legal=0,low=0,high=0;bool open_ended=false;
    for(;;){const auto zero=zeros.find(next.filespace_uuid);if(zero==zeros.end()||zero->second.bootstrap.filespace_role>4||zero->second.bootstrap.page_size_profile_uuid!=next.page_size_profile_uuid||next.page_number>=zero->second.total_pages)return ChainFail(E::invalid_filespace);
      const auto size=zero->second.bootstrap.page_size_bytes;if(size>budget-result.retained_image_bytes)return ChainFail(E::resource_exhausted);if(!slots.emplace(next.filespace_uuid,next.page_number).second)return ChainFail(E::chain_mismatch);
      const auto file=std::lower_bound(ordered.begin(),ordered.end(),next.filespace_uuid,[](const auto& f,const auto& id){return f.filespace_uuid<id;});std::vector<byte> bytes(size);const auto io=file->device->ReadAt(next.page_number*size,bytes.data(),bytes.size());if(!io.ok()||io.bytes_transferred!=bytes.size())return ChainFail(E::io_failure);
      auto loaded=DecodeNativeRetentionPage(bytes);if(!loaded.ok())return ChainFail(loaded.error);const auto& v=*loaded.page;const auto& h=v.header;
      if(h.database_uuid!=database_uuid||h.filespace_uuid!=next.filespace_uuid||h.page_size_profile_uuid!=next.page_size_profile_uuid||h.page_number!=next.page_number||h.page_generation!=next.page_generation||v.object_uuid!=object_uuid||h.page_type!=(result.images.empty()?0x303:0x304))return ChainFail(E::binding_mismatch);
      if(zero_ids.contains(h.page_uuid)||!page_ids.insert(h.page_uuid).second)return ChainFail(E::chain_mismatch);
      if(!result.images.empty()){const auto& first=*result.images.front().page;const auto& prior=*result.images.back().page;
        if(v.epoch!=first.epoch||v.creator_transaction_uuid!=first.creator_transaction_uuid||v.creator_local_transaction_id!=first.creator_local_transaction_id||v.flags!=first.flags||v.total_pins!=first.total_pins||v.first_record!=pins)return ChainFail(E::chain_mismatch);
        const auto hash=Digest(bytes.data(),bytes.size(),seal,false);if(!hash.ok())return ChainFail(E::hash_failure);if(hash.digest!=prior.next_sha256)return ChainFail(E::invalid_integrity);
        for(const auto& pin:v.records){if(!(prior_pin<pin.pin_uuid))return ChainFail(E::chain_mismatch);prior_pin=pin.pin_uuid;++pins;if(Legal(pin))++legal;if(!low||pin.start_local<low)low=pin.start_local;if(!pin.end_local)open_ended=true;else high=std::max(high,pin.end_local);}
      }
      result.retained_image_bytes+=bytes.size();const auto follow=v.next;result.images.push_back(std::move(loaded));
      if(!follow){const auto& first=*result.images.front().page;if(pins!=first.total_pins||legal!=first.legal_hold_pins||low!=first.lowest_start||(open_ended?0:high)!=first.highest_end)return ChainFail(E::summary_mismatch);result.error=E::none;return result;}next=*follow;
    }
  }catch(const std::bad_alloc&){return ChainFail(E::resource_exhausted);}catch(const std::length_error&){return ChainFail(E::resource_exhausted);}catch(...){return ChainFail(E::io_failure);}
}
}  // namespace scratchbird::storage::page

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_horizon_root.hpp"
#include "hash_digest_parts.hpp"
#include "disk_device.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace scratchbird::storage::page {
namespace {
using namespace scratchbird::core::platform;
using E=NativeHorizonError;
namespace hash=scratchbird::core::hash;
constexpr std::size_t start=512,width=192,seal=408;
bool Zero(const byte* p,std::size_t n){return std::all_of(p,p+n,[](byte b){return !b;});}
bool V7(const Uuid& id){return !id.is_nil()&&(id.bytes[6]>>4)==7&&(id.bytes[8]&0xc0)==0x80;}
Uuid GetUuid(const byte* p){Uuid id;std::copy_n(p,16,id.bytes.begin());return id;}
void PutUuid(byte* p,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),p);}
disk::NativePageReference GetRef(const byte* p){return {GetUuid(p),LoadLittle64(p+16),LoadLittle64(p+24),GetUuid(p+32)};}
void PutRef(byte* p,const disk::NativePageReference& r){PutUuid(p,r.filespace_uuid);StoreLittle64(p+16,r.page_number);StoreLittle64(p+24,r.page_generation);PutUuid(p+32,r.page_size_profile_uuid);}
bool Ref(const disk::NativePageReference& r){const auto* p=disk::FindCanonicalFilespacePageProfile(r.page_size_profile_uuid);
  return V7(r.filespace_uuid)&&p&&r.page_number&&r.page_generation&&r.page_number<std::numeric_limits<u64>::max()/p->page_size_bytes;}
auto Digest(const std::vector<byte>& b,bool clear){const std::array<byte,32> zero{};
  const hash::HashDigestSegment parts[]={{b.data(),seal},{clear?zero.data():b.data()+seal,32},{b.data()+seal+32,b.size()-seal-32}};
  return hash::ComputeSha256DigestParts(parts,3);}
NativeHorizonResult Fail(E e){NativeHorizonResult r;r.error=e;return r;}
NativeHorizonChainResult ChainFail(E e){NativeHorizonChainResult r;r.error=e;return r;}
auto Key(const NativeHorizonRecord& r){return std::tuple{r.timeline_uuid,r.kind,r.owner_uuid};}
using Slot=std::pair<Uuid,u64>;
using Target=std::tuple<u32,u64,Uuid,Uuid,u64>;
struct References {
  std::map<Uuid,Uuid> profiles;
  std::map<Slot,Target> slots;
  bool Add(const disk::NativePageReference& r,u32 type,const Uuid& object,u64 generation){
    if(!Ref(r))return false;
    const auto [profile,inserted]=profiles.emplace(r.filespace_uuid,r.page_size_profile_uuid);
    if(!inserted&&profile->second!=r.page_size_profile_uuid)return false;
    const Target value{type,r.page_generation,r.page_size_profile_uuid,object,generation};
    const auto [at,fresh]=slots.emplace(Slot{r.filespace_uuid,r.page_number},value);
    return fresh||at->second==value;
  }
};
bool AddReferences(References& refs,const NativeHorizonRoot& v){const auto& h=v.header;
  if(!refs.Add({h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid},0x302,v.object_uuid,v.epoch)||
      !refs.Add(v.retention,0x303,v.retention_object_uuid,0))return false;
  if(v.next&&!refs.Add(*v.next,0x302,v.object_uuid,v.epoch))return false;
  for(const auto& r:v.records)if(r.checkpoint&&!refs.Add(*r.checkpoint,0x300,r.checkpoint_object_uuid,r.checkpoint_generation))return false;
  return true;
}
E Validate(const NativeHorizonRoot& v){const auto& h=v.header;
  if(!disk::EncodeNativeCommonPageHeader(h).ok()||h.page_type!=0x302||h.flags)return E::invalid_header;
  if(!V7(v.object_uuid)||!V7(v.creator_transaction_uuid)||!v.epoch||!v.creator_local_transaction_id||(v.flags&~u64{1})||
      !V7(v.retention_object_uuid)||Zero(v.retention_sha256.data(),32)||v.first_record>v.total_records||
      v.records.size()>v.total_records-v.first_record||v.records.size()>(h.page_size_bytes-start)/width)return E::invalid_family;
  if(v.records.empty()&&(v.total_records||v.first_record))return E::invalid_family;
  if((v.records.size()<v.total_records-v.first_record)!=v.next.has_value()||v.next.has_value()==Zero(v.next_sha256.data(),32))return E::invalid_reference;
  if(v.next&&v.next->filespace_uuid==h.filespace_uuid&&v.next->page_number==h.page_number)return E::invalid_reference;
  std::optional<decltype(Key(NativeHorizonRecord{}))> prior;std::set<Uuid> identities;u64 minimum=0;
  for(const auto& r:v.records){const auto kind=static_cast<u16>(r.kind),owner=static_cast<u16>(r.owner_kind);
    if(!kind||kind>18||!owner||owner>8||!r.local_boundary||!V7(r.owner_uuid)||!V7(r.horizon_uuid)||!V7(r.timeline_uuid)||!identities.insert(r.horizon_uuid).second||(!r.pin_uuid.is_nil()&&!V7(r.pin_uuid))||
        (r.flags&~u32{15})||((r.flags&4)&&!(r.flags&1))||
        (r.flags?!V7(r.diagnostic_uuid):!r.diagnostic_uuid.is_nil())||
        ((kind>=11||owner==5)&&!(v.flags&1))||(kind>=11&&owner!=5))return E::invalid_record;
    if(r.checkpoint?(!V7(r.checkpoint_object_uuid)||!r.checkpoint_generation):(!r.checkpoint_object_uuid.is_nil()||r.checkpoint_generation))return E::invalid_reference;
    const auto key=Key(r);if(prior&&!( *prior<key))return E::invalid_record;prior=key;
    if((r.flags&1)&&(!minimum||r.local_boundary<minimum))minimum=r.local_boundary;
  }
  if(v.minimum_blocker!=minimum)return E::invalid_record;
  References refs;if(!AddReferences(refs,v))return E::invalid_reference;
  return E::none;
}
}

NativeHorizonResult EncodeNativeHorizonRoot(const NativeHorizonRoot& v) noexcept {
  try{const auto valid=Validate(v);if(valid!=E::none)return Fail(valid);
    std::vector<byte> b(v.header.page_size_bytes,0);const auto common=disk::EncodeNativeCommonPageHeader(v.header);
    if(!common.ok())return Fail(E::invalid_header);std::copy(common.bytes->begin(),common.bytes->end(),b.begin());auto* f=b.data()+128;
    std::copy_n("SBHOR002",8,f);StoreLittle16(f+8,2);StoreLittle16(f+10,384);StoreLittle32(f+12,start+width*v.records.size());
    PutUuid(f+16,v.object_uuid);StoreLittle64(f+32,v.epoch);PutUuid(f+40,v.creator_transaction_uuid);StoreLittle64(f+56,v.creator_local_transaction_id);
    StoreLittle64(f+64,v.flags);StoreLittle64(f+72,v.total_records);StoreLittle64(f+80,v.first_record);StoreLittle32(f+88,v.records.size());
    PutRef(f+96,v.retention);PutUuid(f+144,v.retention_object_uuid);std::copy(v.retention_sha256.begin(),v.retention_sha256.end(),f+160);
    if(v.next)PutRef(f+192,*v.next);std::copy(v.next_sha256.begin(),v.next_sha256.end(),f+240);StoreLittle64(f+272,v.minimum_blocker);
    for(std::size_t i=0;i<v.records.size();++i){const auto& r=v.records[i];auto* p=b.data()+start+i*width;
      StoreLittle16(p,static_cast<u16>(r.kind));StoreLittle16(p+2,static_cast<u16>(r.owner_kind));StoreLittle32(p+4,r.flags);StoreLittle64(p+8,r.local_boundary);
      PutUuid(p+16,r.owner_uuid);PutUuid(p+32,r.pin_uuid);PutUuid(p+48,r.checkpoint_object_uuid);StoreLittle64(p+64,r.checkpoint_generation);PutUuid(p+72,r.diagnostic_uuid);if(r.checkpoint)PutRef(p+88,*r.checkpoint);PutUuid(p+136,r.horizon_uuid);PutUuid(p+152,r.timeline_uuid);
    }
    const auto digest=Digest(b,true);if(!digest.ok())return Fail(E::hash_failure);std::copy(digest.digest.begin(),digest.digest.end(),b.begin()+seal);
    return {E::none,v,std::move(b)};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_family);}
}
NativeHorizonResult DecodeNativeHorizonRoot(const std::vector<byte>& b) noexcept {
  try{if(b.size()<start)return Fail(E::invalid_header);const auto h=disk::DecodeNativeCommonPageHeader(b.data(),128);
    if(!h.ok()||h.header->page_type!=0x302||h.header->flags||b.size()!=h.header->page_size_bytes)return Fail(E::invalid_header);
    const auto digest=Digest(b,true);if(!digest.ok())return Fail(E::hash_failure);if(!std::equal(digest.digest.begin(),digest.digest.end(),b.begin()+seal))return Fail(E::invalid_integrity);
    const auto* f=b.data()+128;const auto count=LoadLittle32(f+88),used=LoadLittle32(f+12);
    if(std::string_view(reinterpret_cast<const char*>(f),8)!="SBHOR002"||LoadLittle16(f+8)!=2||LoadLittle16(f+10)!=384||
        count>(b.size()-start)/width||used!=start+width*count||!Zero(f+92,4)||!Zero(f+312,72)||!Zero(b.data()+used,b.size()-used))return Fail(E::invalid_family);
    NativeHorizonRoot v;v.header=*h.header;v.object_uuid=GetUuid(f+16);v.epoch=LoadLittle64(f+32);v.creator_transaction_uuid=GetUuid(f+40);v.creator_local_transaction_id=LoadLittle64(f+56);
    v.flags=LoadLittle64(f+64);v.total_records=LoadLittle64(f+72);v.first_record=LoadLittle64(f+80);v.retention=GetRef(f+96);v.retention_object_uuid=GetUuid(f+144);std::copy_n(f+160,32,v.retention_sha256.begin());
    if(!Zero(f+192,48))v.next=GetRef(f+192);std::copy_n(f+240,32,v.next_sha256.begin());v.minimum_blocker=LoadLittle64(f+272);v.records.reserve(count);
    for(u32 i=0;i<count;++i){const auto* p=b.data()+start+i*width;if(!Zero(p+168,24))return Fail(E::invalid_record);NativeHorizonRecord r;
      r.kind=static_cast<NativeHorizonKind>(LoadLittle16(p));r.owner_kind=static_cast<NativeHorizonOwner>(LoadLittle16(p+2));r.flags=LoadLittle32(p+4);r.local_boundary=LoadLittle64(p+8);
      r.owner_uuid=GetUuid(p+16);r.pin_uuid=GetUuid(p+32);r.checkpoint_object_uuid=GetUuid(p+48);r.checkpoint_generation=LoadLittle64(p+64);r.diagnostic_uuid=GetUuid(p+72);if(!Zero(p+88,48))r.checkpoint=GetRef(p+88);r.horizon_uuid=GetUuid(p+136);r.timeline_uuid=GetUuid(p+152);v.records.push_back(std::move(r));
    }
    const auto valid=Validate(v);if(valid!=E::none)return Fail(valid);return {E::none,std::move(v),b};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_family);}
}

NativeHorizonChainResult ReadNativeHorizonRootFromOpenDevices(const Uuid& database_uuid,const std::vector<disk::NativeFilespaceDevice>& devices,
    const Uuid& object_uuid,const disk::NativePageReference& head,u64 budget) noexcept {
  try{if(!V7(database_uuid)||!V7(object_uuid)||!Ref(head)||devices.empty()||!budget)return ChainFail(E::invalid_reference);
    auto ordered=devices;std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    std::vector<std::unique_lock<std::recursive_mutex>> guards;std::map<Uuid,disk::FilespacePageZero> zeros;std::set<disk::FileDevice*> devices_seen;std::set<Uuid> zero_ids;
    Uuid prior;guards.reserve(ordered.size());
    for(const auto& file:ordered){if(!V7(file.filespace_uuid)||!(prior<file.filespace_uuid)||!file.device||!devices_seen.insert(file.device).second||!disk::FindCanonicalFilespacePageProfile(file.page_size_profile_uuid))return ChainFail(E::invalid_filespace);
      guards.push_back(file.device->AcquireOperationGuard());prior=file.filespace_uuid;}
    for(const auto& file:ordered){const disk::FilespaceBootstrapBinding binding{database_uuid,file.filespace_uuid,file.page_size_profile_uuid};auto z=disk::ReadFilespacePageZeroFromOpenDevice(*file.device,&binding);
      if(!z.ok())return ChainFail(z.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:z.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:z.error==disk::FilespacePageZeroError::io_failure?E::io_failure:E::invalid_filespace);
      if(!zero_ids.insert(z.record->page_uuid).second)return ChainFail(E::invalid_filespace);zeros.emplace(file.filespace_uuid,std::move(*z.record));}
    const auto capacity=[&](const disk::NativePageReference& r){const auto z=zeros.find(r.filespace_uuid);return z!=zeros.end()&&z->second.bootstrap.filespace_role<=4&&
      z->second.bootstrap.page_size_profile_uuid==r.page_size_profile_uuid&&r.page_number<z->second.total_pages;};
    NativeHorizonChainResult result;auto next=head;std::set<Slot> slots;std::set<Uuid> page_ids,horizon_ids;References refs;
    std::optional<decltype(Key(NativeHorizonRecord{}))> previous_key;u64 ordinal=0;
    for(;;){if(!capacity(next))return ChainFail(E::invalid_filespace);const auto& zero=zeros.at(next.filespace_uuid);
      if(zero.bootstrap.page_size_bytes>budget-result.retained_image_bytes)return ChainFail(E::resource_exhausted);
      if(!slots.emplace(next.filespace_uuid,next.page_number).second)return ChainFail(E::chain_mismatch);
      const auto file=std::lower_bound(ordered.begin(),ordered.end(),next.filespace_uuid,[](const auto& f,const auto& id){return f.filespace_uuid<id;});
      std::vector<byte> bytes(zero.bootstrap.page_size_bytes);const auto io=file->device->ReadAt(next.page_number*bytes.size(),bytes.data(),bytes.size());
      if(!io.ok()||io.bytes_transferred!=bytes.size())return ChainFail(E::io_failure);auto loaded=DecodeNativeHorizonRoot(bytes);if(!loaded.ok())return ChainFail(loaded.error);
      const auto& v=*loaded.root;const auto& h=v.header;
      if(h.database_uuid!=database_uuid||h.filespace_uuid!=next.filespace_uuid||h.page_size_profile_uuid!=next.page_size_profile_uuid||h.page_number!=next.page_number||h.page_generation!=next.page_generation||v.object_uuid!=object_uuid)return ChainFail(E::binding_mismatch);
      if(zero_ids.contains(h.page_uuid)||!page_ids.insert(h.page_uuid).second||v.first_record!=ordinal||!AddReferences(refs,v))return ChainFail(E::chain_mismatch);
      if(!capacity(v.retention))return ChainFail(E::invalid_filespace);
      for(const auto& r:v.records){if(r.checkpoint&&!capacity(*r.checkpoint))return ChainFail(E::invalid_filespace);if(!horizon_ids.insert(r.horizon_uuid).second)return ChainFail(E::chain_mismatch);const auto key=Key(r);if(previous_key&&!( *previous_key<key))return ChainFail(E::chain_mismatch);previous_key=key;}
      if(!result.pages.empty()){const auto& first=*result.pages.front().root;const auto& previous=*result.pages.back().root;
        if(v.epoch!=first.epoch||v.creator_transaction_uuid!=first.creator_transaction_uuid||v.creator_local_transaction_id!=first.creator_local_transaction_id||v.flags!=first.flags||v.total_records!=first.total_records||
            v.retention!=first.retention||v.retention_object_uuid!=first.retention_object_uuid||v.retention_sha256!=first.retention_sha256)return ChainFail(E::chain_mismatch);
        const auto digest=Digest(bytes,false);if(!digest.ok())return ChainFail(E::hash_failure);if(digest.digest!=previous.next_sha256)return ChainFail(E::invalid_integrity);
      }
      ordinal+=v.records.size();result.retained_image_bytes+=bytes.size();const auto follow=v.next;result.pages.push_back(std::move(loaded));
      if(!follow){result.error=E::none;return result;}next=*follow;
    }
  }catch(const std::bad_alloc&){return ChainFail(E::resource_exhausted);}catch(const std::length_error&){return ChainFail(E::resource_exhausted);}catch(...){return ChainFail(E::io_failure);}
}
}  // namespace scratchbird::storage::page

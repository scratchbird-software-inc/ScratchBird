// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_system_state.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <string_view>
namespace scratchbird::storage::database {
namespace {
using namespace scratchbird::core::platform;
namespace hash=scratchbird::core::hash;
using E=NativeSystemStateError;
constexpr std::size_t used=512,seal=408;
bool Zero(const byte* p,std::size_t n){return std::all_of(p,p+n,[](byte b){return !b;});}
bool V7(const Uuid& id){return !id.is_nil()&&(id.bytes[6]>>4)==7&&(id.bytes[8]&0xc0)==0x80;}
Uuid GetUuid(const byte* p){Uuid id;std::copy_n(p,16,id.bytes.begin());return id;}
void PutUuid(byte* p,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),p);}
disk::NativePageReference GetRef(const byte* p){return {GetUuid(p),LoadLittle64(p+16),LoadLittle64(p+24),GetUuid(p+32)};}
void PutRef(byte* p,const disk::NativePageReference& r){PutUuid(p,r.filespace_uuid);StoreLittle64(p+16,r.page_number);StoreLittle64(p+24,r.page_generation);PutUuid(p+32,r.page_size_profile_uuid);}
bool Ref(const disk::NativePageReference& r){const auto* p=disk::FindCanonicalFilespacePageProfile(r.page_size_profile_uuid);
  return V7(r.filespace_uuid)&&p&&r.page_number&&r.page_generation&&r.page_number<std::numeric_limits<u64>::max()/p->page_size_bytes;}
NativeSystemStateResult Fail(E e){NativeSystemStateResult r;r.error=e;return r;}
auto Digest(const std::vector<byte>& b,bool clear=true){const std::array<byte,32> zero{};const hash::HashDigestSegment parts[]={{b.data(),seal},{clear?zero.data():b.data()+seal,32},{b.data()+seal+32,b.size()-seal-32}};
  return hash::ComputeSha256DigestParts(parts,3);}
E Validate(const NativeSystemState& s){const auto& h=s.header;
  if(!disk::EncodeNativeCommonPageHeader(h).ok()||h.page_type!=8||h.flags)return E::invalid_header;
  if(!V7(s.object_uuid)||!V7(s.creator_transaction_uuid)||!V7(s.transition_operation_uuid))return E::invalid_identity;
  if(!s.state_generation||!s.restart_generation||s.startup_counter<s.restart_generation||!s.creator_local_transaction_id)return E::invalid_family;
  const auto state=static_cast<u16>(s.lifecycle),recovery=static_cast<u16>(s.recovery);
  if(state<1||state>10||recovery<1||recovery>7||(s.flags&~u32(15)))return E::invalid_state;
  const bool clean=s.flags&NativeSystemFlag::clean,dirty=s.flags&NativeSystemFlag::dirty,fenced=s.flags&NativeSystemFlag::write_fenced;
  if(clean==dirty||clean!=(s.lifecycle==NativeSystemLifecycle::closed)||
      ((!fenced)&&(s.lifecycle!=NativeSystemLifecycle::ready||recovery>=4)))return E::invalid_state;
  if(s.clean_local_transaction_id? !V7(s.clean_transaction_uuid):!s.clean_transaction_uuid.is_nil())return E::invalid_identity;
  if(s.checkpoint_generation){if(!s.checkpoint||!V7(s.checkpoint_object_uuid))return E::invalid_reference;}
  else if(s.checkpoint||!s.checkpoint_object_uuid.is_nil())return E::invalid_reference;
  if(clean&&(!s.clean_local_transaction_id||!s.checkpoint_generation))return E::invalid_state;
  if((s.state_generation==1)==s.predecessor.has_value()||s.predecessor.has_value()==Zero(s.predecessor_sha256.data(),32))return E::invalid_reference;
  for(const auto* ref:{&s.checkpoint,&s.predecessor})if(*ref){const auto& r=**ref;
    if(!Ref(r)||(r.filespace_uuid==h.filespace_uuid&&(r.page_size_profile_uuid!=h.page_size_profile_uuid||r.page_number==h.page_number)))return E::invalid_reference;}
  if(s.checkpoint&&s.predecessor&&s.checkpoint->filespace_uuid==s.predecessor->filespace_uuid&&
      (s.checkpoint->page_number==s.predecessor->page_number||s.checkpoint->page_size_profile_uuid!=s.predecessor->page_size_profile_uuid))return E::invalid_reference;
  return E::none;
}
}
NativeSystemStateResult EncodeNativeSystemState(const NativeSystemState& s) noexcept {
  try{const auto valid=Validate(s);if(valid!=E::none)return Fail(valid);std::vector<byte> b(s.header.page_size_bytes,0);
    const auto h=disk::EncodeNativeCommonPageHeader(s.header);if(!h.ok())return Fail(E::invalid_header);std::copy(h.bytes->begin(),h.bytes->end(),b.begin());
    auto* f=b.data()+128;std::copy_n("SBSYS001",8,f);StoreLittle16(f+8,1);StoreLittle16(f+10,384);StoreLittle32(f+12,512);
    PutUuid(f+16,s.object_uuid);StoreLittle64(f+32,s.state_generation);StoreLittle64(f+40,s.restart_generation);StoreLittle64(f+48,s.startup_counter);
    PutUuid(f+56,s.creator_transaction_uuid);StoreLittle64(f+72,s.creator_local_transaction_id);StoreLittle16(f+80,static_cast<u16>(s.lifecycle));StoreLittle16(f+82,static_cast<u16>(s.recovery));StoreLittle32(f+84,s.flags);
    StoreLittle64(f+88,s.checkpoint_generation);if(s.checkpoint)PutRef(f+96,*s.checkpoint);PutUuid(f+144,s.checkpoint_object_uuid);
    PutUuid(f+160,s.clean_transaction_uuid);StoreLittle64(f+176,s.clean_local_transaction_id);PutUuid(f+184,s.transition_operation_uuid);
    if(s.predecessor)PutRef(f+200,*s.predecessor);std::copy(s.predecessor_sha256.begin(),s.predecessor_sha256.end(),f+248);
    const auto digest=Digest(b);if(!digest.ok())return Fail(E::hash_failure);std::copy(digest.digest.begin(),digest.digest.end(),b.begin()+seal);
    return {E::none,s,std::move(b)};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_family);}
}
NativeSystemStateResult DecodeNativeSystemState(const std::vector<byte>& b) noexcept {
  try{if(b.size()<used)return Fail(E::invalid_header);const auto h=disk::DecodeNativeCommonPageHeader(b.data(),128);
    if(!h.ok()||h.header->page_type!=8||h.header->flags||b.size()!=h.header->page_size_bytes)return Fail(E::invalid_header);
    const auto digest=Digest(b);if(!digest.ok())return Fail(E::hash_failure);if(!std::equal(digest.digest.begin(),digest.digest.end(),b.begin()+seal))return Fail(E::invalid_integrity);
    const auto* f=b.data()+128;if(std::string_view(reinterpret_cast<const char*>(f),8)!="SBSYS001"||LoadLittle16(f+8)!=1||LoadLittle16(f+10)!=384||LoadLittle32(f+12)!=512||!Zero(f+312,72)||!Zero(b.data()+used,b.size()-used))return Fail(E::invalid_family);
    NativeSystemState s;s.header=*h.header;s.object_uuid=GetUuid(f+16);s.state_generation=LoadLittle64(f+32);s.restart_generation=LoadLittle64(f+40);s.startup_counter=LoadLittle64(f+48);
    s.creator_transaction_uuid=GetUuid(f+56);s.creator_local_transaction_id=LoadLittle64(f+72);s.lifecycle=static_cast<NativeSystemLifecycle>(LoadLittle16(f+80));s.recovery=static_cast<NativeSystemRecovery>(LoadLittle16(f+82));s.flags=LoadLittle32(f+84);
    s.checkpoint_generation=LoadLittle64(f+88);if(!Zero(f+96,48))s.checkpoint=GetRef(f+96);s.checkpoint_object_uuid=GetUuid(f+144);
    s.clean_transaction_uuid=GetUuid(f+160);s.clean_local_transaction_id=LoadLittle64(f+176);s.transition_operation_uuid=GetUuid(f+184);
    if(!Zero(f+200,48))s.predecessor=GetRef(f+200);std::copy_n(f+248,32,s.predecessor_sha256.begin());const auto valid=Validate(s);if(valid!=E::none)return Fail(valid);
    return {E::none,std::move(s),b};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_family);}
}
NativeSystemStateResult ReadNativeSystemStateFromOpenDevice(disk::FileDevice& device,const Uuid& database_uuid,const disk::FilespaceRootReference& ref) noexcept {
  try{if(!V7(database_uuid)||!V7(ref.object_uuid)||ref.kind!=1||ref.page_type!=8||!Ref({ref.filespace_uuid,ref.page_number,ref.page_generation,ref.page_size_profile_uuid}))return Fail(E::invalid_reference);
    auto guard=device.AcquireOperationGuard();const disk::FilespaceBootstrapBinding binding{database_uuid,ref.filespace_uuid,ref.page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(device,&binding);
    if(!zero.ok())return Fail(zero.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:
      zero.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:zero.error==disk::FilespacePageZeroError::io_failure?E::io_failure:E::invalid_filespace);
    const auto& z=*zero.record;if(z.bootstrap.filespace_role>4||ref.page_number>=z.total_pages)return Fail(E::invalid_filespace);
    std::vector<byte> bytes(z.bootstrap.page_size_bytes);const auto io=device.ReadAt(ref.page_number*z.bootstrap.page_size_bytes,bytes.data(),bytes.size());
    if(!io.ok()||io.bytes_transferred!=bytes.size())return Fail(E::io_failure);auto r=DecodeNativeSystemState(bytes);if(!r.ok())return r;
    const auto& s=*r.state;const auto& h=s.header;
    if(h.database_uuid!=database_uuid||h.filespace_uuid!=ref.filespace_uuid||h.page_size_profile_uuid!=ref.page_size_profile_uuid||h.page_number!=ref.page_number||h.page_generation!=ref.page_generation||s.object_uuid!=ref.object_uuid)return Fail(E::binding_mismatch);
    for(const auto* target:{&s.checkpoint,&s.predecessor})if(*target&&(**target).filespace_uuid==h.filespace_uuid&&(**target).page_number>=z.total_pages)return Fail(E::invalid_reference);
    return r;
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::io_failure);}
}
NativeSystemStateHistoryResult ReadNativeSystemStateHistoryFromOpenDevices(
    const Uuid& database_uuid,const std::vector<disk::NativeFilespaceDevice>& devices,
    const disk::FilespaceRootReference& head,const disk::FilespaceRootReference& terminal,u64 budget) noexcept {
  const auto fail=[](E e){NativeSystemStateHistoryResult r;r.error=e;return r;};
  const auto valid=[](const auto& ref){return ref.kind==1&&ref.page_type==8&&V7(ref.object_uuid)&&Ref({ref.filespace_uuid,ref.page_number,ref.page_generation,ref.page_size_profile_uuid});};
  const auto same=[](const auto& a,const auto& b){return a.kind==b.kind&&a.page_type==b.page_type&&a.object_uuid==b.object_uuid&&
    a.filespace_uuid==b.filespace_uuid&&a.page_size_profile_uuid==b.page_size_profile_uuid&&a.page_number==b.page_number&&a.page_generation==b.page_generation;};
  try{
    if(!V7(database_uuid)||devices.empty()||!budget||!valid(head)||!valid(terminal)||head.object_uuid!=terminal.object_uuid)return fail(E::invalid_reference);
    auto ordered=devices;std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    std::vector<std::unique_lock<std::recursive_mutex>> guards;guards.reserve(ordered.size());
    std::set<disk::FileDevice*> pointers;Uuid prior;
    for(const auto& file:ordered){if(!V7(file.filespace_uuid)||!(prior<file.filespace_uuid)||!file.device||!pointers.insert(file.device).second||
        !disk::FindCanonicalFilespacePageProfile(file.page_size_profile_uuid))return fail(E::invalid_filespace);
      guards.push_back(file.device->AcquireOperationGuard());prior=file.filespace_uuid;}
    NativeSystemStateHistoryResult result;auto next=head;
    std::set<std::pair<Uuid,u64>> slots;std::set<Uuid> identities;
    while(true){const auto file=std::lower_bound(ordered.begin(),ordered.end(),next.filespace_uuid,[](const auto& a,const auto& id){return a.filespace_uuid<id;});
      if(file==ordered.end()||file->filespace_uuid!=next.filespace_uuid||file->page_size_profile_uuid!=next.page_size_profile_uuid)return fail(E::invalid_filespace);
      const auto* profile=disk::FindCanonicalFilespacePageProfile(next.page_size_profile_uuid);
      if(!profile||profile->page_size_bytes>budget-result.retained_image_bytes)return fail(E::resource_exhausted);
      if(!slots.insert({next.filespace_uuid,next.page_number}).second)return fail(E::history_mismatch);
      auto loaded=ReadNativeSystemStateFromOpenDevice(*file->device,database_uuid,next);if(!loaded.ok())return fail(loaded.error);
      const auto& s=*loaded.state;if(!identities.insert(s.header.page_uuid).second)return fail(E::history_mismatch);
      if(!result.pages.empty()){const auto& newer=*result.pages.back().state;const auto digest=Digest(loaded.bytes,false);
        if(!digest.ok())return fail(E::hash_failure);if(digest.digest!=newer.predecessor_sha256)return fail(E::invalid_integrity);
        if(s.state_generation==std::numeric_limits<u64>::max()||newer.state_generation!=s.state_generation+1||
            newer.restart_generation<s.restart_generation||newer.startup_counter<s.startup_counter||newer.checkpoint_generation<s.checkpoint_generation)return fail(E::history_mismatch);
        if(newer.checkpoint_generation==s.checkpoint_generation&&(newer.checkpoint!=s.checkpoint||newer.checkpoint_object_uuid!=s.checkpoint_object_uuid))return fail(E::history_mismatch);
        if((newer.clean_transaction_uuid!=s.clean_transaction_uuid||newer.clean_local_transaction_id!=s.clean_local_transaction_id)&&
            (newer.lifecycle!=NativeSystemLifecycle::closed||!newer.clean_local_transaction_id))return fail(E::history_mismatch);
      }
      result.retained_image_bytes+=loaded.bytes.size();const auto predecessor=s.predecessor;result.pages.push_back(std::move(loaded));
      if(same(next,terminal)){result.error=E::none;return result;}
      if(!predecessor)return fail(E::history_mismatch);
      next={1,8,predecessor->filespace_uuid,predecessor->page_number,predecessor->page_generation,predecessor->page_size_profile_uuid,head.object_uuid};
    }
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted);}catch(const std::length_error&){return fail(E::resource_exhausted);}catch(...){return fail(E::io_failure);}
}
}  // namespace scratchbird::storage::database

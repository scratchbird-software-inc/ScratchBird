// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_row_data_page.hpp"
#include "filespace_page_zero.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>

namespace scratchbird::storage::page {
namespace {
using E=NativeRowDataError;
NativeRowDataResult Fail(E e){return {e,{},{}};}
bool Bound(const NativeRowDataPage& p) {
  return p.header.page_number==p.body.page_number&&p.header.page_generation==p.body.page_generation&&
      !p.body.next_page_number&&p.body.compaction_generation&&p.body.segment_id&&p.body.segment_generation&&
      std::all_of(p.body.rows.begin(),p.body.rows.end(),[](const auto& r){return r.stable_slot_id!=0;});
}
core::hash::HashDigestResult Digest(const std::vector<byte>& bytes) {
  const std::array<byte,32> zero{};
  const core::hash::HashDigestSegment parts[]={{bytes.data(),bytes.size()-32},{zero.data(),zero.size()}};
  return core::hash::ComputeSha256DigestParts(parts,2);
}
}
NativeRowDataResult EncodeNativeRowDataPage(const NativeRowDataPage& p) noexcept {
  try {
    const auto h=disk::EncodeNativeCommonPageHeader(p.header);
    if(!h.ok()||p.header.page_type!=0x0100)return Fail(E::invalid_header);
    if(!Bound(p))return Fail(E::invalid_body);
    auto body=BuildRowDataPageBody(p.body,p.header.page_size_bytes-32);
    if(!body.ok())return Fail(E::invalid_body);
    std::vector<byte> b(p.header.page_size_bytes,0);
    std::copy(h.bytes->begin(),h.bytes->end(),b.begin());
    std::copy(body.serialized.begin(),body.serialized.end(),b.begin()+128);
    const auto hash=Digest(b);if(!hash.ok())return Fail(E::hash_failure);
    std::copy(hash.digest.begin(),hash.digest.end(),b.end()-32);
    return {E::none,NativeRowDataPage{p.header,std::move(body.body)},std::move(b)};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
   catch(...){return Fail(E::invalid_body);}
}
NativeRowDataResult DecodeNativeRowDataPage(const std::vector<byte>& b) noexcept {
  try {
    const auto h=disk::DecodeNativeCommonPageHeader(b.data(),std::min<std::size_t>(b.size(),128));
    if(!h.ok()||h.header->page_type!=0x0100||b.size()!=h.header->page_size_bytes)return Fail(E::invalid_header);
    const auto hash=Digest(b);if(!hash.ok())return Fail(E::hash_failure);
    if(!std::equal(hash.digest.begin(),hash.digest.end(),b.end()-32))return Fail(E::invalid_integrity);
    std::vector<byte> bytes(b.begin()+128,b.end()-32);
    auto body=ParseRowDataPageBody(bytes,h.header->page_number);
    if(!body.ok())return Fail(E::invalid_body);
    NativeRowDataPage p{*h.header,std::move(body.body)};if(!Bound(p))return Fail(E::invalid_body);
    const auto canonical=BuildRowDataPageBody(p.body,p.header.page_size_bytes-32);
    if(!canonical.ok()||canonical.serialized!=bytes)return Fail(E::invalid_body);
    return {E::none,std::move(p),b};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
   catch(...){return Fail(E::invalid_body);}
}
NativeRowDataResult ReadNativeRowDataPageFromOpenDevice(disk::FileDevice& device,
    const core::platform::Uuid& database_uuid,const NativeRowDataReference& ref) noexcept {
  try {
    const auto& p=ref.page;const auto* profile=disk::FindCanonicalFilespacePageProfile(p.page_size_profile_uuid);
    const auto valid=[](const core::platform::Uuid& id){return core::uuid::IsEngineIdentityUuid(id);};
    if(!valid(database_uuid)||!valid(ref.relation_uuid)||!valid(p.filespace_uuid)||
        (ref.page_uuid&&!valid(*ref.page_uuid))||!profile||!p.page_number||!p.page_generation||
        p.page_number>=std::numeric_limits<u64>::max()/profile->page_size_bytes||
        !disk::CheckFileDeviceExtent(p.page_number*profile->page_size_bytes,profile->page_size_bytes).ok())
      return Fail(E::binding_mismatch);
    const auto guard=device.AcquireOperationGuard();
    const disk::FilespaceBootstrapBinding binding{database_uuid,p.filespace_uuid,p.page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(device,&binding);
    if(!zero.ok()) {
      using Z=disk::FilespacePageZeroError;
      return Fail(zero.error==Z::resource_exhausted?E::resource_exhausted:
          zero.error==Z::hash_provider_failure?E::hash_failure:zero.error==Z::io_failure?E::io_failure:E::invalid_filespace);
    }
    const auto role=zero.record->bootstrap.filespace_role;
    if(role==14)return Fail(E::import_requires_authority);
    if((role!=5&&role!=8&&role!=10)||p.page_number>=zero.record->total_pages)return Fail(E::invalid_filespace);
    if(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted)
      return Fail(E::encrypted_requires_crypto_authority);
    if(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required)
      return Fail(E::cluster_requires_authority);
    std::vector<byte> bytes(profile->page_size_bytes);
    const auto io=device.ReadAt(p.page_number*profile->page_size_bytes,bytes.data(),bytes.size());
    if(!io.ok()||io.bytes_transferred!=bytes.size())return Fail(E::io_failure);
    const disk::NativeCommonPageHeaderBinding expected{binding,p.page_number,p.page_generation,0x0100,ref.page_uuid};
    const auto h=disk::DecodeNativeCommonPageHeader(bytes.data(),128,&expected);
    if(!h.ok())return Fail(E::binding_mismatch);
    if(h.header->flags&1u)return Fail(E::encrypted_requires_crypto_authority);
    if(h.header->flags&2u)return Fail(E::cluster_requires_authority);
    if(h.header->flags&12u)return Fail(E::header_policy_requires_authority);
    auto result=DecodeNativeRowDataPage(bytes);
    if(result.ok()&&result.page->body.relation_uuid.value!=ref.relation_uuid)return Fail(E::binding_mismatch);
    return result;
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
   catch(...){return Fail(E::io_failure);}
}
}  // namespace scratchbird::storage::page

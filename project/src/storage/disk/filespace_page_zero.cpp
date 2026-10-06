// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "filespace_page_zero.hpp"
#include "crypto_memory_adapter.hpp"
#include "native_common_page_header.hpp"
#include "disk_device.hpp"
#include "native_decoded_storage_ranges.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#include <type_traits>

namespace scratchbird::storage::disk {
namespace {
using namespace scratchbird::core::platform;
using Error = FilespacePageZeroError;
constexpr std::size_t common_offset = 4096, family_offset = 4224;
constexpr std::size_t directory_offset = 4480, digest_offset = 4448;
constexpr std::array<byte,8> family_magic{{'S','B','F','Z','V','0','0','1'}};
bool V7(const Uuid& id) noexcept { return (id.bytes[6]&0xf0u)==0x70u && (id.bytes[8]&0xc0u)==0x80u; }
bool Same(const Uuid& a,const Uuid& b) noexcept { return a.bytes==b.bytes; }
bool Zero(const byte* b,std::size_t n) noexcept {
  return std::all_of(b,b+n,[](byte v){return v==0;});
}
void PutUuid(byte* b,const Uuid& id) noexcept { std::copy(id.bytes.begin(),id.bytes.end(),b); }
Uuid GetUuid(const byte* b) noexcept { Uuid id; std::copy_n(b,16,id.bytes.begin()); return id; }
bool FullDigest(const byte* b,std::size_t size,std::array<byte,32>& out) noexcept {
  if(auto* prepared=core::hash::CurrentPreparedSha256()){
    const std::array<byte,32> zero{};
    const core::hash::HashDigestSegment parts[]={{b,digest_offset},{zero.data(),zero.size()},
      {b+directory_offset,size-directory_offset}};
    const auto result=prepared->Compute(parts,3);out=result.digest;return result.ok();
  }
  std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
  std::array<byte,32> zero{}; unsigned count=0;
  return ctx && EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr)==1
      && EVP_DigestUpdate(ctx.get(),b,digest_offset)==1
      && EVP_DigestUpdate(ctx.get(),zero.data(),zero.size())==1
      && EVP_DigestUpdate(ctx.get(),b+directory_offset,size-directory_offset)==1
      && EVP_DigestFinal_ex(ctx.get(),out.data(),&count)==1 && count==out.size();
}
FilespacePageZeroDecodeResult Failure(Error error) noexcept { return {error,std::nullopt}; }
Error BootstrapError(FilespaceBootstrapError error) noexcept {
  if(error==FilespaceBootstrapError::hash_provider_failure) return Error::hash_provider_failure;
  if(error==FilespaceBootstrapError::resource_exhausted) return Error::resource_exhausted;
  if(error==FilespaceBootstrapError::device_not_open) return Error::device_not_open;
  if(error==FilespaceBootstrapError::io_failure) return Error::io_failure;
  return Error::invalid_bootstrap;
}
template<class Roots> Error Validate(const FilespacePageZeroData<Roots>& value) {
  const auto& h=value.bootstrap;
  if(ValidateFilespaceBootstrap(h)!=FilespaceBootstrapError::none) return Error::invalid_bootstrap;
  if(!V7(value.page_uuid)||!V7(value.creation_operation_uuid)||!V7(value.writer_identity_uuid)
      ||!value.page_generation||!value.root_set_generation) return Error::invalid_family;
  constexpr auto max=std::numeric_limits<u64>::max();
  if(!value.total_pages||value.total_pages>max/h.page_size_bytes
      ||value.free_pages>=value.total_pages
      ||value.preallocated_pages>=value.total_pages-value.free_pages
      ||!CheckFileDeviceExtent(value.total_pages*h.page_size_bytes,0).ok()) return Error::invalid_capacity;
  if(value.roots.size()>32) return Error::invalid_root_directory;
  u16 previous=0; u32 kinds=0;
  for(const auto& root:value.roots) {
    const auto type=CanonicalPageZeroRootPageType(root.kind);
    if(!type||root.kind<=previous||root.page_type!=type||!V7(root.filespace_uuid)
        ||!V7(root.object_uuid)||!root.page_number||!root.page_generation)
      return Error::invalid_root_directory;
    previous=root.kind; kinds|=u32{1}<<root.kind;
    const auto* profile=FindCanonicalFilespacePageProfile(root.page_size_profile_uuid);
    if(!profile||root.page_number>=max/profile->page_size_bytes
        ||!CheckFileDeviceExtent(root.page_number*profile->page_size_bytes,profile->page_size_bytes).ok())
      return Error::invalid_root_directory;
    if(Same(root.filespace_uuid,h.filespace_uuid)
        &&(!Same(root.page_size_profile_uuid,h.page_size_profile_uuid)||root.page_number>=value.total_pages))
      return Error::invalid_root_directory;
    if(root.kind==3&&!Same(root.filespace_uuid,h.filespace_uuid)) return Error::invalid_root_directory;
    if(root.kind>=15&&root.kind<=17&&(h.flags&FilespaceBootstrapFlag::cluster_authority_required)==0)
      return Error::invalid_root_directory;
    for(const auto& other:value.roots) {
      if(&root==&other||!Same(root.filespace_uuid,other.filespace_uuid)) continue;
      if(!Same(root.page_size_profile_uuid,other.page_size_profile_uuid)) return Error::invalid_root_directory;
      if(root.page_number==other.page_number && (root.page_generation!=other.page_generation
          ||root.page_type!=other.page_type||!Same(root.object_uuid,other.object_uuid)))
        return Error::invalid_root_directory;
    }
  }
  const auto first=std::find_if(value.roots.begin(),value.roots.end(),[](const auto& r){return r.kind==18;});
  const auto second=std::find_if(value.roots.begin(),value.roots.end(),[](const auto& r){return r.kind==19;});
  if((first==value.roots.end())!=(second==value.roots.end()))return Error::required_root_missing;
  if(first!=value.roots.end()&&(h.filespace_role>4||first->filespace_uuid!=h.filespace_uuid||
      second->filespace_uuid!=h.filespace_uuid||first->object_uuid!=second->object_uuid||first->page_number==second->page_number))
    return Error::invalid_root_directory;
  const auto watermark_first=std::find_if(value.roots.begin(),value.roots.end(),[](const auto& r){return r.kind==20;});
  const auto watermark_second=std::find_if(value.roots.begin(),value.roots.end(),[](const auto& r){return r.kind==21;});
  if((watermark_first==value.roots.end())!=(watermark_second==value.roots.end()))return Error::required_root_missing;
  if(watermark_first!=value.roots.end()) {
    if(first==value.roots.end())return Error::required_root_missing;
    if(h.filespace_role>4||watermark_first->filespace_uuid!=h.filespace_uuid||
        watermark_second->filespace_uuid!=h.filespace_uuid||watermark_first->object_uuid!=watermark_second->object_uuid||
        watermark_first->object_uuid==first->object_uuid||watermark_first->page_number==watermark_second->page_number)
      return Error::invalid_root_directory;
    for(const auto& root:value.roots)if(root.kind<20&&root.filespace_uuid==h.filespace_uuid&&
        (root.page_number==watermark_first->page_number||root.page_number==watermark_second->page_number))
      return Error::invalid_root_directory;
  }
  if(h.lifecycle_state==1||h.lifecycle_state==2) {
    const u32 required=h.filespace_role<=4?0x3feu:0x8u;
    if((kinds&required)!=required) return Error::required_root_missing;
  }
  return Error::none;
}
bool WriteCommon(byte* b,const FilespacePageZero& value) noexcept {
  const auto& h=value.bootstrap;
  NativeCommonPageHeader common;
  common.page_size_bytes=h.page_size_bytes;
  common.page_type=h.filespace_role<=4?1:2;
  common.database_uuid=h.database_uuid; common.filespace_uuid=h.filespace_uuid;
  common.page_uuid=value.page_uuid; common.page_generation=value.page_generation;
  common.flags=h.flags&FilespaceBootstrapFlag::cluster_authority_required;
  common.page_size_profile_uuid=h.page_size_profile_uuid;
  const auto encoded=EncodeNativeCommonPageHeader(common);
  if(!encoded.ok()) return false;
  std::copy(encoded.bytes->begin(),encoded.bytes->end(),b);
  return true;
}
bool ReadCommon(const byte* b,const FilespaceBootstrap& h,Uuid& page,u64& generation) noexcept {
  const auto decoded=DecodeNativeCommonPageHeader(b,kNativeCommonPageHeaderBytes);
  if(!decoded.ok()) return false;
  const auto& common=*decoded.header;
  if(common.page_size_bytes!=h.page_size_bytes||common.page_type!=(h.filespace_role<=4?1u:2u)
      ||common.page_number!=0||common.flags!=(h.flags&FilespaceBootstrapFlag::cluster_authority_required)
      ||!Same(common.database_uuid,h.database_uuid)||!Same(common.filespace_uuid,h.filespace_uuid)
      ||!Same(common.page_size_profile_uuid,h.page_size_profile_uuid)) return false;
  page=common.page_uuid; generation=common.page_generation;
  return true;
}
}  // namespace

u32 CanonicalPageZeroRootPageType(u16 kind) noexcept {
  // Root-kind -> Core page symbol codes (page-types.yaml), not prototype enums.
  constexpr std::array<u32,22> types{{0,0x8,0x5,0x3,0x301,0x9,0xa,0xb,0x5,
      0x300,0x303,0x305,0x307,0x30b,0x515,0x406,0x401,0x309,0x30e,0x30e,0x500,0x500}};
  return kind<types.size()?types[kind]:0;
}
FilespacePageZeroEncodeResult EncodeFilespacePageZero(const FilespacePageZero& value) noexcept {
  try {
    const auto error=Validate(value);
    if(error!=Error::none) return {error,std::nullopt};
    const auto preamble=EncodeFilespaceBootstrap(value.bootstrap);
    if(!preamble.ok()) return {BootstrapError(preamble.error),std::nullopt};
    std::vector<byte> bytes(value.bootstrap.page_size_bytes,0);
    std::copy(preamble.bytes->begin(),preamble.bytes->end(),bytes.begin());
    if(!WriteCommon(bytes.data()+common_offset,value)) return {Error::invalid_common_header,std::nullopt};
    auto* b=bytes.data()+family_offset; const auto& h=value.bootstrap;
    std::copy(family_magic.begin(),family_magic.end(),b);
    StoreLittle32(b+8,256); StoreLittle32(b+12,static_cast<u32>(value.roots.size()));
    StoreLittle64(b+16,256+80*value.roots.size()); StoreLittle64(b+24,value.root_set_generation);
    PutUuid(b+32,h.database_uuid); PutUuid(b+48,h.filespace_uuid); PutUuid(b+64,h.page_size_profile_uuid);
    PutUuid(b+80,h.checksum_profile_uuid); PutUuid(b+96,h.encryption_profile_uuid); PutUuid(b+112,value.page_uuid);
    StoreLittle32(b+128,h.page_size_bytes); StoreLittle32(b+132,h.durable_format_generation);
    StoreLittle16(b+136,h.filespace_role); StoreLittle16(b+138,h.lifecycle_state); StoreLittle32(b+140,h.flags);
    StoreLittle64(b+144,value.total_pages); StoreLittle64(b+152,value.free_pages);
    StoreLittle64(b+160,value.preallocated_pages); StoreLittle64(b+168,value.page_generation);
    PutUuid(b+176,value.creation_operation_uuid); PutUuid(b+192,value.writer_identity_uuid);
    StoreLittle64(b+208,value.creation_utc_millis); StoreLittle32(b+216,80);
    auto* entry=bytes.data()+directory_offset;
    for(const auto& root:value.roots) {
      StoreLittle16(entry,root.kind); StoreLittle32(entry+4,root.page_type); PutUuid(entry+8,root.filespace_uuid);
      StoreLittle64(entry+24,root.page_number); StoreLittle64(entry+32,root.page_generation);
      PutUuid(entry+40,root.page_size_profile_uuid); PutUuid(entry+56,root.object_uuid); entry+=80;
    }
    std::array<byte,32> digest{};
    if(!FullDigest(bytes.data(),bytes.size(),digest)) return {Error::hash_provider_failure,std::nullopt};
    std::copy(digest.begin(),digest.end(),bytes.begin()+digest_offset);
    return {Error::none,std::move(bytes)};
  } catch(const std::bad_alloc&) { return {Error::resource_exhausted,std::nullopt}; }
    catch(const std::length_error&) { return {Error::resource_exhausted,std::nullopt}; }
    catch(...) { return {Error::invalid_family,std::nullopt}; }
}

namespace {
template<class Roots> Error DecodeValues(const byte* bytes,std::size_t size,
    const FilespaceBootstrapBinding* expected,FilespacePageZeroData<Roots>& value) {
  try {
    if(!bytes||size<kFilespaceBootstrapBytes) return Error::invalid_bootstrap;
    const auto preamble=DecodeFilespaceBootstrap(bytes,kFilespaceBootstrapBytes,expected);
    if(!preamble.ok()) return BootstrapError(preamble.error);
    const auto& h=*preamble.preamble;
    if(size!=h.page_size_bytes) return Error::invalid_capacity;
    value.bootstrap=h;
    if(!ReadCommon(bytes+common_offset,h,value.page_uuid,value.page_generation))
      return Error::invalid_common_header;
    const auto* b=bytes+family_offset;
    const u32 count=LoadLittle32(b+12);
    if(!std::equal(family_magic.begin(),family_magic.end(),b)||LoadLittle32(b+8)!=256||count>32
        ||LoadLittle64(b+16)!=256+80ull*count||LoadLittle32(b+216)!=80||!Zero(b+220,4)
        ||!Zero(bytes+directory_offset+80*count,size-directory_offset-80*count))
      return Error::invalid_family;
    std::array<byte,32> digest{};
    if(!FullDigest(bytes,size,digest)) return Error::hash_provider_failure;
    if(!std::equal(digest.begin(),digest.end(),bytes+digest_offset)) return Error::integrity_mismatch;
    if(!Same(GetUuid(b+32),h.database_uuid)||!Same(GetUuid(b+48),h.filespace_uuid)
        ||!Same(GetUuid(b+64),h.page_size_profile_uuid)||!Same(GetUuid(b+80),h.checksum_profile_uuid)
        ||!Same(GetUuid(b+96),h.encryption_profile_uuid)||!Same(GetUuid(b+112),value.page_uuid)
        ||LoadLittle32(b+128)!=h.page_size_bytes||LoadLittle32(b+132)!=h.durable_format_generation
        ||LoadLittle16(b+136)!=h.filespace_role||LoadLittle16(b+138)!=h.lifecycle_state
        ||LoadLittle32(b+140)!=h.flags||LoadLittle64(b+168)!=value.page_generation)
      return Error::invalid_family;
    value.root_set_generation=LoadLittle64(b+24); value.total_pages=LoadLittle64(b+144);
    value.free_pages=LoadLittle64(b+152); value.preallocated_pages=LoadLittle64(b+160);
    value.creation_operation_uuid=GetUuid(b+176); value.writer_identity_uuid=GetUuid(b+192);
    value.creation_utc_millis=LoadLittle64(b+208);
    if constexpr(std::is_same_v<Roots,std::vector<FilespaceRootReference>>)value.roots.resize(count);
    else {if(count>value.roots.size())return Error::resource_exhausted;value.roots=value.roots.first(count);}
    const auto* entry=bytes+directory_offset;
    for(u32 i=0;i<count;++i,entry+=80) {
      if(LoadLittle16(entry+2)!=0||!Zero(entry+72,8)) return Error::invalid_root_directory;
      FilespaceRootReference root;
      root.kind=LoadLittle16(entry); root.page_type=LoadLittle32(entry+4);
      root.filespace_uuid=GetUuid(entry+8); root.page_number=LoadLittle64(entry+24);
      root.page_generation=LoadLittle64(entry+32); root.page_size_profile_uuid=GetUuid(entry+40);
      root.object_uuid=GetUuid(entry+56); value.roots[i]=root;
    }
    const auto error=Validate(value);
    if(error!=Error::none) return error;
    return Error::none;
  } catch(const std::bad_alloc&) { return Error::resource_exhausted; }
    catch(const std::length_error&) { return Error::resource_exhausted; }
    catch(...) { return Error::invalid_family; }
}
} // namespace
FilespacePageZeroDecodeResult DecodeFilespacePageZero(
    const byte* bytes,std::size_t size,const FilespaceBootstrapBinding* expected) noexcept {
  FilespacePageZero value;const auto error=DecodeValues(bytes,size,expected,value);
  if(error!=Error::none)return Failure(error);
  return {Error::none,std::move(value)};
}
FilespacePageZeroViewResult DecodeFilespacePageZeroInto(std::span<const byte> bytes,
    std::span<FilespaceRootReference> roots,const FilespaceBootstrapBinding* expected) noexcept {
  if(!detail::DisjointNativeDecodeRegions(bytes,roots))return {Error::invalid_backing,std::nullopt};
  FilespacePageZeroView value;value.roots=roots;
  const auto error=DecodeValues(bytes.data(),bytes.size(),expected,value);
  if(error!=Error::none)return {error,std::nullopt};
  return {Error::none,value};
}

FilespacePageZeroDecodeResult ReadFilespacePageZeroFromOpenDevice(
    FileDevice& device,const FilespaceBootstrapBinding* expected) noexcept {
  try {
    // Primitive read/size locks alone leave gaps in this multi-read observation.
    // Retain this exact device through probe, decode and final capacity check.
    const auto guard=device.AcquireOperationGuard();
    const auto probe=ReadFilespaceBootstrapFromOpenDevice(device,expected);
    if(!probe.ok()) return Failure(BootstrapError(probe.error));
    const auto preamble=EncodeFilespaceBootstrap(*probe.preamble);
    if(!preamble.ok()) return Failure(BootstrapError(preamble.error));
    const auto before=device.Size();
    const auto size=probe.preamble->page_size_bytes;
    if(!before.ok()) return Failure(Error::io_failure);
    if(before.size_bytes<size||before.size_bytes%size!=0) return Failure(Error::invalid_capacity);
    std::vector<byte> bytes(size);
    const auto read=device.ReadAt(0,bytes.data(),bytes.size());
    if(!read.ok()||read.bytes_transferred!=bytes.size()) return Failure(Error::io_failure);
    if(!std::equal(preamble.bytes->begin(),preamble.bytes->end(),bytes.begin())) return Failure(Error::probe_changed);
    auto decoded=DecodeFilespacePageZero(bytes.data(),bytes.size(),expected);
    if(!decoded.ok()) return decoded;
    const auto after=device.Size();
    if(!after.ok()) return Failure(Error::io_failure);
    if(before.size_bytes!=after.size_bytes||after.size_bytes!=decoded.record->total_pages*size)
      return Failure(Error::invalid_capacity);
    return decoded;
  } catch(const std::bad_alloc&) { return Failure(Error::resource_exhausted); }
    catch(const std::length_error&) { return Failure(Error::resource_exhausted); }
    catch(...) { return Failure(Error::io_failure); }
}
FilespacePageZeroRecoveryObservation ObserveFilespacePageZeroForRecoveryFromOpenDevice(
    FileDevice& device,const FilespaceBootstrapBinding& expected) noexcept {
  const auto fail=[](Error error){FilespacePageZeroRecoveryObservation r;r.error=error;return r;};
  try {
    const auto guard=device.AcquireOperationGuard();
    const auto probe=ReadFilespaceBootstrapFromOpenDevice(device,&expected);
    if(!probe.ok())return fail(BootstrapError(probe.error));
    const auto preamble=EncodeFilespaceBootstrap(*probe.preamble);
    if(!preamble.ok())return fail(BootstrapError(preamble.error));
    const auto before=device.Size();const auto size=probe.preamble->page_size_bytes;
    if(!before.ok())return fail(Error::io_failure);
    if(before.size_bytes<size)return fail(Error::invalid_capacity);
    std::vector<byte> bytes(size);
    const auto read=device.ReadAt(0,bytes.data(),bytes.size());
    if(!read.ok()||read.bytes_transferred!=bytes.size())return fail(Error::io_failure);
    if(!std::equal(preamble.bytes->begin(),preamble.bytes->end(),bytes.begin()))return fail(Error::probe_changed);
    auto decoded=DecodeFilespacePageZero(bytes.data(),bytes.size(),&expected);
    if(!decoded.ok())return fail(decoded.error);
    const auto after=device.Size();if(!after.ok())return fail(Error::io_failure);
    if(before.size_bytes!=after.size_bytes)return fail(Error::probe_changed);
    FilespacePageZeroRecoveryObservation result;
    result.declared_bytes=decoded.record->total_pages*size;
    result.observed_bytes=after.size_bytes;result.complete_pages=after.size_bytes/size;
    result.trailing_bytes=after.size_bytes%size;
    result.relation=result.observed_bytes==result.declared_bytes?FilespaceExtentRelation::matching:
      result.observed_bytes<result.declared_bytes?FilespaceExtentRelation::shorter:FilespaceExtentRelation::longer;
    result.record=std::move(decoded.record);result.error=Error::none;return result;
  }catch(const std::bad_alloc&){return fail(Error::resource_exhausted);}
   catch(const std::length_error&){return fail(Error::resource_exhausted);}
   catch(...){return fail(Error::io_failure);}
}
FilespaceRecoveryRootCandidates ProbeFilespaceRecoveryRootCandidatesFromOpenDevice(
    FileDevice& device,const FilespaceBootstrapBinding& binding,u64 budget) noexcept {
  using E=FilespaceRecoveryRootError;
  FilespaceRecoveryRootCandidates result;
  const auto require=[](bool valid,E error){if(!valid)throw error;};
  try {
    require(budget>=kFilespaceBootstrapBytes,E::resource_exhausted);
    const auto guard=device.AcquireOperationGuard();require(device.is_open(),E::invalid_request);
    SerializedFilespaceBootstrap prefix{};
    const auto read=[&](byte* out,std::size_t bytes){auto io=device.ReadAt(0,out,bytes);
      if(!io.ok()||io.bytes_transferred!=bytes){result.diagnostic=std::move(io.diagnostic);throw E::io_failure;}};
    read(prefix.data(),prefix.size());
    const auto probe=DecodeFilespaceBootstrap(prefix.data(),prefix.size(),&binding);
    if(!probe.ok()){result.image_error=BootstrapError(probe.error);
      throw probe.error==FilespaceBootstrapError::hash_provider_failure?E::hash_failure:
            probe.error==FilespaceBootstrapError::resource_exhausted?E::resource_exhausted:E::invalid_bootstrap;}
    const auto& bootstrap=*probe.preamble;const auto size=bootstrap.page_size_bytes;
    require(bootstrap.filespace_role>=1&&bootstrap.filespace_role<=4,E::invalid_bootstrap);
    require(!(bootstrap.flags&FilespaceBootstrapFlag::payload_encrypted),E::encrypted_requires_authority);
    require(!(bootstrap.flags&FilespaceBootstrapFlag::cluster_authority_required),E::cluster_requires_authority);
    require(size<=budget-kFilespaceBootstrapBytes,E::resource_exhausted);
    const auto extent=[&]{auto value=device.Size();if(!value.ok()){result.diagnostic=std::move(value.diagnostic);throw E::io_failure;}return value.size_bytes;};
    const auto observed=extent();require(observed>=size,E::invalid_extent);
    std::vector<byte> image(size);read(image.data(),image.size());
    require(std::equal(prefix.begin(),prefix.end(),image.begin()),E::changed_observation);
    std::vector<FilespaceRootReference> roots;roots.reserve(32);u16 previous=0;u32 kinds=0;
    std::size_t offset=directory_offset;
    for(unsigned i=0;i<32;++i,offset+=80){const auto* entry=image.data()+offset;if(Zero(entry,80))break;
      FilespaceRootReference r{LoadLittle16(entry),LoadLittle32(entry+4),GetUuid(entry+8),
        LoadLittle64(entry+24),LoadLittle64(entry+32),GetUuid(entry+40),GetUuid(entry+56)};
      require(r.kind>previous&&r.kind<=21&&CanonicalPageZeroRootPageType(r.kind)==r.page_type&&r.page_type&&
        !LoadLittle16(entry+2)&&Zero(entry+72,8)&&V7(r.filespace_uuid)&&V7(r.object_uuid)&&r.page_number&&r.page_generation,E::invalid_directory);
      const auto* profile=FindCanonicalFilespacePageProfile(r.page_size_profile_uuid);require(profile,E::invalid_directory);
      require(r.page_number<=std::numeric_limits<u64>::max()/profile->page_size_bytes&&
        CheckFileDeviceExtent(r.page_number*u64{profile->page_size_bytes},profile->page_size_bytes).ok(),E::invalid_directory);
      if(r.filespace_uuid==binding.filespace_uuid)require(r.page_size_profile_uuid==binding.page_size_profile_uuid&&
        r.page_number<observed/size,E::invalid_extent);
      if(r.kind==3)require(r.filespace_uuid==binding.filespace_uuid,E::invalid_directory);
      // Cluster-only roots cannot appear in this clear local recovery profile.
      require(r.kind<15||r.kind>17,E::cluster_requires_authority);
      for(const auto& old:roots)if(old.filespace_uuid==r.filespace_uuid){require(old.page_size_profile_uuid==r.page_size_profile_uuid,E::invalid_directory);
        if(old.page_number==r.page_number)require(old.page_type==r.page_type&&old.page_generation==r.page_generation&&old.object_uuid==r.object_uuid,E::invalid_directory);}
      previous=r.kind;kinds|=u32{1}<<r.kind;roots.push_back(r);
    }
    require(Zero(image.data()+offset,image.size()-offset),E::invalid_directory);
    constexpr u32 required=0x3feu|(u32{15}<<18);require((kinds&required)==required,E::invalid_directory);
    const auto root=[&](u16 kind)->const FilespaceRootReference&{return *std::find_if(roots.begin(),roots.end(),[&](const auto& r){return r.kind==kind;});};
    for(u16 first:{u16{18},u16{20}}){const auto& a=root(first);const auto& b=root(first+1);
      require(a.filespace_uuid==binding.filespace_uuid&&b.filespace_uuid==binding.filespace_uuid&&a.object_uuid==b.object_uuid&&a.page_number!=b.page_number,E::invalid_directory);}
    require(root(18).object_uuid!=root(20).object_uuid,E::invalid_directory);
    for(const auto& r:roots)if(r.kind<20&&r.filespace_uuid==binding.filespace_uuid)
      require(r.page_number!=root(20).page_number&&r.page_number!=root(21).page_number,E::invalid_directory);
    require(extent()==observed,E::changed_observation);
    result.bootstrap=bootstrap;result.roots=std::move(roots);result.observed_size_bytes=observed;result.error=E::none;
  }catch(E error){result.error=error;}catch(const std::bad_alloc&){result.error=E::resource_exhausted;}
   catch(const std::length_error&){result.error=E::resource_exhausted;}catch(...){result.error=E::io_failure;}
  return result;
}
namespace {
FilespacePageZeroBodyResult MutateGrowthBody(FileDevice& device,
    const FilespaceBootstrapBinding& binding, const std::vector<byte>& before,
    const std::vector<byte>& after, u64 budget, bool repair) noexcept {
  using E=FilespacePageZeroBodyError;
  FilespacePageZeroBodyResult result;
  const auto require=[](bool valid,E error){if(!valid)throw error;};
  const auto image_error=[&](Error error){
    if(error==Error::none)return;
    result.image_error=error;
    throw error==Error::hash_provider_failure?E::hash_failure:
          error==Error::resource_exhausted?E::resource_exhausted:E::image_failure;
  };
  try {
    require(before.size()==after.size()&&before.size()>=directory_offset,E::invalid_request);
    require(before.size()<=budget/4,E::resource_exhausted);
    const auto original=DecodeFilespacePageZero(before.data(),before.size(),&binding);
    image_error(original.error);
    const auto target=DecodeFilespacePageZero(after.data(),after.size(),&binding);
    image_error(target.error);
    const auto& a=*original.record;const auto& b=*target.record;
    require(a.page_generation!=std::numeric_limits<u64>::max()&&
            a.root_set_generation!=std::numeric_limits<u64>::max()&&
            b.page_generation==a.page_generation+1&&
            b.root_set_generation==a.root_set_generation+1&&b.total_pages>a.total_pages,
            E::invalid_transition);
    auto normalized=b;normalized.page_generation=a.page_generation;
    normalized.root_set_generation=a.root_set_generation;normalized.total_pages=a.total_pages;
    normalized.free_pages=a.free_pages;normalized.preallocated_pages=a.preallocated_pages;
    const auto normalized_image=EncodeFilespacePageZero(normalized);image_error(normalized_image.error);
    require(*normalized_image.bytes==before,E::invalid_transition);
    const auto immutable=[&](const std::vector<byte>& image){
      return std::equal(before.begin(),before.begin()+common_offset,image.begin())&&
             std::equal(before.begin()+directory_offset,before.end(),image.begin()+directory_offset);
    };
    require(immutable(after),E::invalid_transition);
    std::vector<byte> scratch(before.size());
    const auto guard=device.AcquireOperationGuard();
    require(device.is_open()&&!device.read_only(),E::invalid_device);
    const u64 required_size=b.total_pages*b.bootstrap.page_size_bytes;
    const auto extent=[&]{auto size=device.Size();
      if(!size.ok()){result.diagnostic=std::move(size.diagnostic);throw E::io_failure;}
      result.observed_size_bytes=size.size_bytes;require(size.size_bytes==required_size,E::extent_mismatch);
    };
    const auto read=[&]{auto io=device.ReadAt(0,scratch.data(),scratch.size());
      if(!io.ok()||io.bytes_transferred!=scratch.size()){
        result.diagnostic=std::move(io.diagnostic);throw E::io_failure;}
    };
    extent();read();
    require(immutable(scratch),E::preimage_changed);
    result.original_preimage_verified=scratch==before;
    result.target_already_present=scratch==after;
    if(!result.original_preimage_verified&&!result.target_already_present){
      require(repair,E::preimage_changed);
      const auto actual=DecodeFilespacePageZero(scratch.data(),scratch.size(),&binding);
      result.observed_body_error=actual.error;
      if(actual.error==Error::hash_provider_failure||actual.error==Error::resource_exhausted)
        image_error(actual.error);
      require(!actual.ok(),E::preimage_changed);
      result.damaged_body_observed=true;
    }
    // Reobserve the external extent after all validation and before effects.
    extent();
    if(!result.target_already_present){
      constexpr auto length=directory_offset-common_offset;
      result.write_attempted=result.uncertain_write=true;
      auto io=device.WriteAt(common_offset,after.data()+common_offset,length);
      if(io.bytes_transferred<=length)result.confirmed_bytes=io.bytes_transferred;
      if(!io.ok()||io.bytes_transferred!=length){
        result.diagnostic=std::move(io.diagnostic);throw E::io_failure;}
      result.uncertain_write=false;
    }
    result.sync_attempted=true;
    auto sync=device.Sync();
    if(!sync.ok()){result.diagnostic=std::move(sync.diagnostic);throw E::io_failure;}
    result.sync_completed=true;
    read();require(scratch==after,E::readback_mismatch);extent();
    result.postimage_verified=true;result.error=E::none;
  }catch(E error){result.error=error;}
   catch(const std::bad_alloc&){result.error=E::resource_exhausted;}
   catch(const std::length_error&){result.error=E::resource_exhausted;}
   catch(...){result.error=E::io_failure;}
  return result;
}
} // namespace
FilespacePageZeroBodyResult WriteFilespacePageZeroGrowthBodyFromOpenDevice(
    FileDevice& device,const FilespaceBootstrapBinding& binding,const std::vector<byte>& before,
    const std::vector<byte>& after,u64 budget) noexcept {
  return MutateGrowthBody(device,binding,before,after,budget,false);
}
FilespacePageZeroBodyResult RepairFilespacePageZeroGrowthBodyFromOpenDevice(
    FileDevice& device,const FilespaceBootstrapBinding& binding,const std::vector<byte>& before,
    const std::vector<byte>& after,u64 budget) noexcept {
  return MutateGrowthBody(device,binding,before,after,budget,true);
}
}  // namespace scratchbird::storage::disk

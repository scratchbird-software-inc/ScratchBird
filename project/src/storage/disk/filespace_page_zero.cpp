// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "filespace_page_zero.hpp"
#include "native_common_page_header.hpp"
#include "disk_device.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

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
Error Validate(const FilespacePageZero& value) {
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
    if(root.kind>=15&&(h.flags&FilespaceBootstrapFlag::cluster_authority_required)==0)
      return Error::invalid_root_directory;
    for(const auto& other:value.roots) {
      if(&root==&other||!Same(root.filespace_uuid,other.filespace_uuid)) continue;
      if(!Same(root.page_size_profile_uuid,other.page_size_profile_uuid)) return Error::invalid_root_directory;
      if(root.page_number==other.page_number && (root.page_generation!=other.page_generation
          ||root.page_type!=other.page_type||!Same(root.object_uuid,other.object_uuid)))
        return Error::invalid_root_directory;
    }
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
  constexpr std::array<u32,18> types{{0,0x8,0x5,0x3,0x301,0x9,0xa,0xb,0x5,
      0x300,0x303,0x305,0x307,0x30b,0x515,0x406,0x401,0x309}};
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

FilespacePageZeroDecodeResult DecodeFilespacePageZero(
    const byte* bytes,std::size_t size,const FilespaceBootstrapBinding* expected) noexcept {
  try {
    if(!bytes||size<kFilespaceBootstrapBytes) return Failure(Error::invalid_bootstrap);
    const auto preamble=DecodeFilespaceBootstrap(bytes,kFilespaceBootstrapBytes,expected);
    if(!preamble.ok()) return Failure(BootstrapError(preamble.error));
    const auto& h=*preamble.preamble;
    if(size!=h.page_size_bytes) return Failure(Error::invalid_capacity);
    FilespacePageZero value; value.bootstrap=h;
    if(!ReadCommon(bytes+common_offset,h,value.page_uuid,value.page_generation))
      return Failure(Error::invalid_common_header);
    const auto* b=bytes+family_offset;
    const u32 count=LoadLittle32(b+12);
    if(!std::equal(family_magic.begin(),family_magic.end(),b)||LoadLittle32(b+8)!=256||count>32
        ||LoadLittle64(b+16)!=256+80ull*count||LoadLittle32(b+216)!=80||!Zero(b+220,4)
        ||!Zero(bytes+directory_offset+80*count,size-directory_offset-80*count))
      return Failure(Error::invalid_family);
    std::array<byte,32> digest{};
    if(!FullDigest(bytes,size,digest)) return Failure(Error::hash_provider_failure);
    if(!std::equal(digest.begin(),digest.end(),bytes+digest_offset)) return Failure(Error::integrity_mismatch);
    if(!Same(GetUuid(b+32),h.database_uuid)||!Same(GetUuid(b+48),h.filespace_uuid)
        ||!Same(GetUuid(b+64),h.page_size_profile_uuid)||!Same(GetUuid(b+80),h.checksum_profile_uuid)
        ||!Same(GetUuid(b+96),h.encryption_profile_uuid)||!Same(GetUuid(b+112),value.page_uuid)
        ||LoadLittle32(b+128)!=h.page_size_bytes||LoadLittle32(b+132)!=h.durable_format_generation
        ||LoadLittle16(b+136)!=h.filespace_role||LoadLittle16(b+138)!=h.lifecycle_state
        ||LoadLittle32(b+140)!=h.flags||LoadLittle64(b+168)!=value.page_generation)
      return Failure(Error::invalid_family);
    value.root_set_generation=LoadLittle64(b+24); value.total_pages=LoadLittle64(b+144);
    value.free_pages=LoadLittle64(b+152); value.preallocated_pages=LoadLittle64(b+160);
    value.creation_operation_uuid=GetUuid(b+176); value.writer_identity_uuid=GetUuid(b+192);
    value.creation_utc_millis=LoadLittle64(b+208);
    value.roots.reserve(count); const auto* entry=bytes+directory_offset;
    for(u32 i=0;i<count;++i,entry+=80) {
      if(LoadLittle16(entry+2)!=0||!Zero(entry+72,8)) return Failure(Error::invalid_root_directory);
      FilespaceRootReference root;
      root.kind=LoadLittle16(entry); root.page_type=LoadLittle32(entry+4);
      root.filespace_uuid=GetUuid(entry+8); root.page_number=LoadLittle64(entry+24);
      root.page_generation=LoadLittle64(entry+32); root.page_size_profile_uuid=GetUuid(entry+40);
      root.object_uuid=GetUuid(entry+56); value.roots.push_back(root);
    }
    const auto error=Validate(value);
    if(error!=Error::none) return Failure(error);
    return {Error::none,std::move(value)};
  } catch(const std::bad_alloc&) { return Failure(Error::resource_exhausted); }
    catch(const std::length_error&) { return Failure(Error::resource_exhausted); }
    catch(...) { return Failure(Error::invalid_family); }
}

FilespacePageZeroDecodeResult ReadFilespacePageZeroFromOpenDevice(
    FileDevice& device,const FilespaceBootstrapBinding* expected) noexcept {
  try {
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
}  // namespace scratchbird::storage::disk

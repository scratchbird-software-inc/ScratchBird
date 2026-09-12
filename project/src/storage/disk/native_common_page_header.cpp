// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_common_page_header.hpp"
#include "disk_device.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace scratchbird::storage::disk {
namespace {
using namespace scratchbird::core::platform;
using Error = NativeCommonPageHeaderError;
constexpr std::array<byte,8> magic{{'S','B','P','G','V','0','0','2'}};
bool V7(const Uuid& id) noexcept { return (id.bytes[6]&0xf0u)==0x70u && (id.bytes[8]&0xc0u)==0x80u; }
void PutUuid(byte* b,const Uuid& id) noexcept { std::copy(id.bytes.begin(),id.bytes.end(),b); }
Uuid GetUuid(const byte* b) noexcept { Uuid id; std::copy_n(b,16,id.bytes.begin()); return id; }
NativeCommonPageHeaderDecodeResult Failure(Error error) noexcept { return {error,std::nullopt}; }
bool PageNumberValid(u32 type,u64 page) noexcept {
  return (page==0)==(type==1||type==2);
}
bool BindingValid(const NativeCommonPageHeaderBinding& expected) noexcept {
  return V7(expected.filespace.database_uuid)&&V7(expected.filespace.filespace_uuid)
      &&FindCanonicalFilespacePageProfile(expected.filespace.page_size_profile_uuid)
      &&expected.page_generation&&IsRegisteredNativePageType(expected.page_type)
      &&PageNumberValid(expected.page_type,expected.page_number)
      &&(!expected.page_uuid||V7(*expected.page_uuid));
}
Error ValidateIdentity(const NativeCommonPageHeader& h) noexcept {
  if(h.flags&~u64{15}) return Error::unknown_flags;
  const auto* profile=FindCanonicalFilespacePageProfile(h.page_size_profile_uuid);
  if(!profile||profile->page_size_bytes!=h.page_size_bytes) return Error::unsupported_profile;
  if(!V7(h.database_uuid)||!V7(h.filespace_uuid)||!V7(h.page_uuid)) return Error::invalid_identity;
  if(!h.page_generation) return Error::invalid_generation;
  return Error::none;
}
Error ValidateLayout(const NativeCommonPageHeader& h) noexcept {
  if(!IsRegisteredNativePageType(h.page_type)) return Error::unregistered_type;
  if(!PageNumberValid(h.page_type,h.page_number)) return Error::invalid_page_number;
  return Error::none;
}
Error MatchIdentity(const NativeCommonPageHeader& h,const NativeCommonPageHeaderBinding& b) noexcept {
  if(!BindingValid(b)) return Error::invalid_binding;
  if(h.database_uuid.bytes!=b.filespace.database_uuid.bytes
      ||h.filespace_uuid.bytes!=b.filespace.filespace_uuid.bytes
      ||h.page_size_profile_uuid.bytes!=b.filespace.page_size_profile_uuid.bytes
      ||h.page_number!=b.page_number||h.page_generation!=b.page_generation
      ||(b.page_uuid&&h.page_uuid.bytes!=b.page_uuid->bytes)) return Error::binding_mismatch;
  return Error::none;
}
}  // namespace

bool IsRegisteredNativePageType(u32 code) noexcept {
  // Exact contiguous allocated sets in Core/page-types.yaml, not its wider
  // reserved code_ranges. Membership does not assert implementation status.
  return code<=0x13 || (code>=0x100&&code<=0x108)
      ||(code>=0x200&&code<=0x20e)||(code>=0x300&&code<=0x30d)
      ||(code>=0x400&&code<=0x408)||(code>=0x500&&code<=0x518)
      ||(code>=0x600&&code<=0x602)||code==0xffe||code==0xfff;
}
u64 ComputeNativeCommonPageHeaderChecksum(const NativeCommonPageHeaderBytes& b) noexcept {
  u64 value=14695981039346656037ull;
  for(std::size_t i=0;i<b.size();++i) { value^=(i>=96&&i<104)?0:b[i]; value*=1099511628211ull; }
  return value;
}
NativeCommonPageHeaderEncodeResult EncodeNativeCommonPageHeader(const NativeCommonPageHeader& h) noexcept {
  auto error=ValidateIdentity(h);
  if(error==Error::none) error=ValidateLayout(h);
  if(error!=Error::none) return {error,std::nullopt};
  NativeCommonPageHeaderBytes b{};
  std::copy(magic.begin(),magic.end(),b.begin());
  StoreLittle32(b.data()+8,128); StoreLittle32(b.data()+12,h.page_size_bytes);
  StoreLittle32(b.data()+16,h.page_type); StoreLittle16(b.data()+20,1); StoreLittle16(b.data()+22,1);
  PutUuid(b.data()+24,h.database_uuid); PutUuid(b.data()+40,h.filespace_uuid); PutUuid(b.data()+56,h.page_uuid);
  StoreLittle64(b.data()+72,h.page_number); StoreLittle64(b.data()+80,h.page_generation);
  StoreLittle64(b.data()+88,h.flags); PutUuid(b.data()+104,h.page_size_profile_uuid);
  StoreLittle16(b.data()+120,1); StoreLittle64(b.data()+96,ComputeNativeCommonPageHeaderChecksum(b));
  return {Error::none,b};
}
NativeCommonPageHeaderDecodeResult DecodeNativeCommonPageHeader(
    const byte* bytes,std::size_t size,const NativeCommonPageHeaderBinding* expected) noexcept {
  if(!bytes||size!=128) return Failure(Error::invalid_framing);
  NativeCommonPageHeaderBytes b; std::copy_n(bytes,b.size(),b.begin());
  if(!std::equal(magic.begin(),magic.end(),b.begin())||LoadLittle32(b.data()+8)!=128)
    return Failure(Error::invalid_framing);
  if(LoadLittle64(b.data()+96)!=ComputeNativeCommonPageHeaderChecksum(b)) return Failure(Error::checksum_mismatch);
  if(LoadLittle16(b.data()+20)!=1||LoadLittle16(b.data()+22)!=1||LoadLittle16(b.data()+120)!=1)
    return Failure(Error::invalid_extension);
  if(std::any_of(b.begin()+122,b.end(),[](byte v){return v!=0;})) return Failure(Error::reserved_nonzero);
  NativeCommonPageHeader h;
  h.page_size_bytes=LoadLittle32(b.data()+12); h.page_type=LoadLittle32(b.data()+16);
  h.database_uuid=GetUuid(b.data()+24); h.filespace_uuid=GetUuid(b.data()+40); h.page_uuid=GetUuid(b.data()+56);
  h.page_number=LoadLittle64(b.data()+72); h.page_generation=LoadLittle64(b.data()+80);
  h.flags=LoadLittle64(b.data()+88); h.page_size_profile_uuid=GetUuid(b.data()+104);
  auto error=ValidateIdentity(h);
  if(error==Error::none&&expected) error=MatchIdentity(h,*expected);
  if(error==Error::none) error=ValidateLayout(h);
  if(error==Error::none&&expected&&h.page_type!=expected->page_type) error=Error::binding_mismatch;
  if(error!=Error::none) return Failure(error);
  return {Error::none,h};
}
NativeCommonPageHeaderDecodeResult ReadNativeCommonPageHeaderFromOpenDevice(
    FileDevice& device,const NativeCommonPageHeaderBinding& expected) noexcept {
  try {
    if(!BindingValid(expected)) return Failure(Error::invalid_binding);
    if(!device.is_open()) return Failure(Error::device_not_open);
    const auto size=FindCanonicalFilespacePageProfile(expected.filespace.page_size_profile_uuid)->page_size_bytes;
    if(expected.page_number>std::numeric_limits<u64>::max()/size) return Failure(Error::invalid_extent);
    const auto page_offset=expected.page_number*size;
    if(!CheckFileDeviceExtent(page_offset,size).ok()) return Failure(Error::invalid_extent);
    const auto before=device.Size();
    if(!before.ok()) return Failure(Error::io_failure);
    if(before.size_bytes%size||page_offset>before.size_bytes||size>before.size_bytes-page_offset)
      return Failure(Error::invalid_extent);
    NativeCommonPageHeaderBytes bytes{};
    const auto offset=page_offset+(expected.page_number==0?4096:0);
    const auto read=device.ReadAt(offset,bytes.data(),bytes.size());
    if(!read.ok()||read.bytes_transferred!=bytes.size()) return Failure(Error::io_failure);
    auto decoded=DecodeNativeCommonPageHeader(bytes.data(),bytes.size(),&expected);
    if(!decoded.ok()) return decoded;
    const auto after=device.Size();
    if(!after.ok()) return Failure(Error::io_failure);
    if(after.size_bytes!=before.size_bytes) return Failure(Error::invalid_extent);
    return decoded;
  } catch(const std::bad_alloc&) { return Failure(Error::resource_exhausted); }
    catch(const std::length_error&) { return Failure(Error::resource_exhausted); }
    catch(...) { return Failure(Error::io_failure); }
}
}  // namespace scratchbird::storage::disk

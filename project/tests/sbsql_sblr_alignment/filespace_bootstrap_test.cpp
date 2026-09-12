// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "filespace_bootstrap.hpp"
#include "disk_device.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
bool hash_failure = false, hash_short = false, read_failure = false;
bool track_read = false, fail_allocation = false;
unsigned reads = 0;
std::size_t read_bytes = 0;
}
void* operator new(std::size_t count) {
  if (fail_allocation) { fail_allocation = false; throw std::bad_alloc(); }
  if (auto* p = std::malloc(count ? count : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t count) { return ::operator new(count); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
extern "C" int __real_EVP_Digest(const void*, size_t, unsigned char*, unsigned int*, const EVP_MD*, ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* data, size_t count, unsigned char* out,
                                 unsigned int* bytes, const EVP_MD* md, ENGINE* engine) {
  if (hash_failure) { hash_failure = false; return 0; }
  const int result = __real_EVP_Digest(data, count, out, bytes, md, engine);
  if (hash_short) { hash_short = false; *bytes = 31; }
  return result;
}
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* out, size_t bytes, off_t offset) {
  if (track_read) { ++reads; read_bytes += bytes; }
  if (read_failure) { read_failure = false; errno = EIO; return -1; }
  return __real_pread(fd, out, bytes, offset);
}

namespace {
namespace disk = scratchbird::storage::disk;
using disk::byte;
using disk::Uuid;
using Error = disk::FilespaceBootstrapError;
using Bytes = disk::SerializedFilespaceBootstrap;
unsigned checks = 0;
void Check(bool ok, std::string_view why,
           std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " line=" + std::to_string(at.line()));
}
// Independent test identities and byte oracle, not production profile/serializer helpers.
Uuid Id(byte tag) {
  return Uuid{{1,2,3,4,5,6,0x71,8,0x89,10,11,12,13,14,15,tag}};
}
constexpr std::array<unsigned,5> sizes{{8192,16384,32768,65536,131072}};
constexpr std::array<std::array<byte,3>,5> suffixes{{
    {{0,0x81,0x92}},{{1,0x63,0x84}},{{3,0x27,0x68}},{{6,0x55,0x36}},{{0x13,0x10,0x72}}}};
disk::FilespaceBootstrap Header(unsigned profile = 0) {
  disk::FilespaceBootstrap h;
  h.database_uuid = Id(1); h.filespace_uuid = Id(2);
  h.page_size_profile_uuid = Uuid{{0,0,0,0,0,0,0x70,0,0x80,0,0,0,0,
      suffixes[profile][0],suffixes[profile][1],suffixes[profile][2]}};
  h.checksum_profile_uuid = Uuid{{0x01,0xa0,0x8e,0x13,0x41,0x6b,0x73,0xf6,
      0xb3,0xc5,0xbd,0xde,0x88,0xb1,0xf2,0xd9}};
  h.page_size_bytes = sizes[profile]; h.filespace_role = 1; h.lifecycle_state = 1;
  return h;
}
void Number(Bytes& bytes, unsigned offset, unsigned width, unsigned value) {
  for (unsigned n = 0; n != width; ++n) bytes[offset+n] = static_cast<byte>(value >> (n*8));
}
void UuidBytes(Bytes& bytes, unsigned offset, Uuid id) {
  for (unsigned n = 0; n != 16; ++n) bytes[offset+n] = id.bytes[n];
}
void Seal(Bytes& bytes) {
  Check(SHA256(bytes.data(), 104, bytes.data()+104) != nullptr, "independent SHA256 oracle");
}
Bytes Oracle(const disk::FilespaceBootstrap& h) {
  Bytes b{};
  b[0]='S'; b[1]='B'; b[2]='F'; b[3]='P';
  Number(b,4,2,1); Number(b,6,2,4096); Number(b,8,4,h.page_size_bytes);
  Number(b,12,4,h.flags); UuidBytes(b,16,h.database_uuid); UuidBytes(b,32,h.filespace_uuid);
  UuidBytes(b,48,h.page_size_profile_uuid); Number(b,64,4,h.durable_format_generation);
  Number(b,68,2,h.filespace_role); Number(b,70,2,h.lifecycle_state);
  UuidBytes(b,72,h.checksum_profile_uuid); UuidBytes(b,88,h.encryption_profile_uuid);
  Seal(b); return b;
}
void Reject(const Bytes& bytes, Error expected) {
  const auto result = disk::DecodeFilespaceBootstrap(bytes.data(),bytes.size());
  Check(!result.ok() && !result.preamble && result.error == expected, "exact atomic decode refusal");
}
void Codecs() {
  for (unsigned p = 0; p != sizes.size(); ++p) {
    auto h = Header(p);
    const auto* by_size = disk::FindCanonicalFilespacePageProfileForSize(sizes[p]);
    const auto* by_id = disk::FindCanonicalFilespacePageProfile(h.page_size_profile_uuid);
    Check(by_size && by_size == by_id && by_id->uuid.bytes == h.page_size_profile_uuid.bytes
        && by_id->layout_generation == 1 && by_id->alignment_bytes == 4096, "exact registry profile");
    for (unsigned flags=0; flags!=4; ++flags) for (unsigned role=1; role<=14; ++role)
      for (unsigned state=1; state<=15; ++state) {
        h.flags=flags; h.filespace_role=static_cast<disk::u16>(role); h.lifecycle_state=static_cast<disk::u16>(state);
        h.encryption_profile_uuid = (flags & 1) ? Id(3) : Uuid{};
        const auto expected = Oracle(h);
        const auto encoded = disk::EncodeFilespaceBootstrap(h);
        Check(encoded.ok() && *encoded.bytes == expected, "all profiles flags roles states exact bytes");
        const disk::FilespaceBootstrapBinding binding{h.database_uuid,h.filespace_uuid,h.page_size_profile_uuid};
        const auto decoded = disk::DecodeFilespaceBootstrap(expected.data(),expected.size(),&binding);
        Check(decoded.ok() && Oracle(*decoded.preamble) == expected, "independent decode and binding");
      }
  }
  for (auto size : {0u,4096u,8191u,8193u,262144u,0xffffffffu})
    Check(!disk::FindCanonicalFilespacePageProfileForSize(size), "unregistered size refused");
  Check(!disk::FindCanonicalFilespacePageProfile(Id(99)), "unknown v7 profile is not invented");
  const auto h = Header(); const auto good = Oracle(h);
  for (unsigned at=0; at<good.size(); ++at) for (unsigned bit=0; bit<8; ++bit) {
    auto bad=good; bad[at]^=static_cast<byte>(1u<<bit);
    auto result=disk::DecodeFilespaceBootstrap(bad.data(),bad.size());
    Check(!result.ok() && !result.preamble, "every preamble bit protected");
  }
  for (std::size_t size=0; size<4096; ++size) {
    const auto result=disk::DecodeFilespaceBootstrap(good.data(),size);
    Check(!result.ok() && !result.preamble && result.error==Error::invalid_framing, "all truncations atomic");
  }
  Check(!disk::DecodeFilespaceBootstrap(nullptr,4096).ok(), "null data");
  Check(!disk::DecodeFilespaceBootstrap(good.data(),4097).ok(), "trailing bytes");
  for (unsigned offset : {16u,32u,48u,72u}) for (unsigned version=0; version<16; ++version) {
    if(version==7) continue;
    auto bad=good; bad[offset+6]=static_cast<byte>(version<<4); Seal(bad);
    Check(!disk::DecodeFilespaceBootstrap(bad.data(),bad.size()).ok(), "resealed non-v7 identity refused");
  }
  for(unsigned offset : {16u,32u,48u,72u}) for(unsigned variant : {0u,0x40u,0xc0u}) {
    auto bad=good; bad[offset+8]=static_cast<byte>(variant); Seal(bad);
    Check(!disk::DecodeFilespaceBootstrap(bad.data(),bad.size()).ok(), "resealed non-RFC variant refused");
  }
  auto field = [&](unsigned offset, unsigned width, unsigned value, Error error) {
    auto bad=good; Number(bad,offset,width,value); Seal(bad); Reject(bad,error);
  };
  field(8,4,4096,Error::unsupported_page_profile);
  field(8,4,16384,Error::unsupported_page_profile);
  field(64,4,0,Error::unsupported_format_generation);
  field(64,4,2,Error::unsupported_format_generation);
  for(unsigned bit=2; bit<32; ++bit) field(12,4,1u<<bit,Error::unknown_flags);
  field(68,2,0,Error::invalid_role); field(68,2,15,Error::invalid_role);
  field(70,2,0,Error::invalid_state); field(70,2,16,Error::invalid_state);
  field(12,4,1,Error::invalid_encryption_profile);
  for(unsigned offset : {16u,32u,48u,72u,88u}) {
    auto bad=good; UuidBytes(bad,offset,Id(99)); Seal(bad);
    if(offset==16 || offset==32) {
      const disk::FilespaceBootstrapBinding binding{h.database_uuid,h.filespace_uuid,h.page_size_profile_uuid};
      const auto result=disk::DecodeFilespaceBootstrap(bad.data(),bad.size(),&binding);
      Check(!result.preamble && result.error==Error::binding_mismatch, "wrong owned identity");
    } else Check(!disk::DecodeFilespaceBootstrap(bad.data(),bad.size()).ok(), "unknown profile binding");
  }
  for(unsigned at=0; at<3; ++at) {
    disk::FilespaceBootstrapBinding expected{h.database_uuid,h.filespace_uuid,h.page_size_profile_uuid};
    if(at==0) expected.database_uuid={}; else if(at==1) expected.filespace_uuid={}; else expected.page_size_profile_uuid={};
    const auto result=disk::DecodeFilespaceBootstrap(good.data(),good.size(),&expected);
    Check(!result.preamble && result.error==Error::binding_mismatch, "nil expected binding refused");
  }
  for(unsigned which=0; which!=10; ++which) {
    auto bad=h;
    switch(which) {
      case 0: bad.database_uuid={}; break; case 1: bad.filespace_uuid={}; break;
      case 2: bad.page_size_profile_uuid={}; break; case 3: bad.page_size_bytes=4096; break;
      case 4: bad.durable_format_generation=2; break; case 5: bad.flags=4; break;
      case 6: bad.checksum_profile_uuid={}; break; case 7: bad.filespace_role=0; break;
      case 8: bad.lifecycle_state=16; break; case 9: bad.encryption_profile_uuid=Id(3); break;
    }
    const auto result=disk::EncodeFilespaceBootstrap(bad);
    Check(!result.ok() && !result.bytes, "invalid encode no partial carrier");
  }
  hash_failure=true;
  auto encoded=disk::EncodeFilespaceBootstrap(h);
  Check(!encoded.bytes && encoded.error==Error::hash_provider_failure && !hash_failure, "encode provider failure");
  hash_short=true; encoded=disk::EncodeFilespaceBootstrap(h);
  Check(!encoded.bytes && encoded.error==Error::hash_provider_failure && !hash_short, "encode short hash");
  hash_failure=true; auto decoded=disk::DecodeFilespaceBootstrap(good.data(),good.size());
  Check(!decoded.preamble && decoded.error==Error::hash_provider_failure && !hash_failure, "decode provider failure");
  hash_short=true; decoded=disk::DecodeFilespaceBootstrap(good.data(),good.size());
  Check(!decoded.preamble && decoded.error==Error::hash_provider_failure && !hash_short, "decode short hash");
}
bool LockFailure(const disk::IoResult& result) {
  return !result.ok() && (result.diagnostic.diagnostic_code=="SB-STORAGE-DISK-OWNER-LOCK-HELD"
      || result.diagnostic.diagnostic_code=="SB-STORAGE-DISK-DATA-OWNER-LOCK-HELD"
      || result.diagnostic.diagnostic_code=="SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD");
}
struct Fixture {
  std::filesystem::path root;
  Fixture() {
    std::string pattern=(std::filesystem::temp_directory_path()/"sb_bootstrap.XXXXXX").string();
    std::vector<char> bytes(pattern.begin(),pattern.end()); bytes.push_back(0);
    const auto* made=::mkdtemp(bytes.data()); Check(made,"temporary fixture directory"); root=made;
  }
  ~Fixture() { std::error_code error; std::filesystem::remove_all(root,error); }
};
void Exclusive(const std::string& path) {
  disk::FileDevice second;
  Check(LockFailure(second.Open(path,disk::FileOpenMode::open_existing)),"same-process exclusion");
  const pid_t child=::fork(); Check(child>=0,"fork ownership probe");
  if(child==0) { ::execl("/proc/self/exe","filespace_bootstrap_test","--probe",path.c_str(),nullptr); ::_exit(125); }
  int status=0; Check(::waitpid(child,&status,0)==child,"wait fresh process");
  Check(WIFEXITED(status) && WEXITSTATUS(status)==0,"fresh-exec process exclusion");
}
void Files() {
  Fixture fixture;
  {
    // One process simultaneously owns five differently profiled physical
    // filespaces. No database-wide size is used by the structural probe.
    std::array<disk::FileDevice,5> devices;
    std::array<disk::FilespaceBootstrap,5> headers;
    for(unsigned p=0;p<sizes.size();++p) {
      headers[p]=Header(p); headers[p].filespace_uuid=Id(static_cast<byte>(20+p));
      const auto path=(fixture.root/("mixed"+std::to_string(p))).string();
      Check(devices[p].Open(path,disk::FileOpenMode::create_new).ok(),"simultaneous filespace ownership");
      const auto bytes=Oracle(headers[p]);
      std::vector<byte> page(sizes[p]); std::copy(bytes.begin(),bytes.end(),page.begin());
      Check(devices[p].WriteAt(0,page.data(),page.size()).ok() && devices[p].Sync().ok(),"mixed profile bytes persisted");
    }
    for(unsigned p=0;p<sizes.size();++p) {
      const auto& h=headers[p];
      const disk::FilespaceBootstrapBinding expected{h.database_uuid,h.filespace_uuid,h.page_size_profile_uuid};
      auto result=disk::ReadFilespaceBootstrapFromOpenDevice(devices[p],&expected);
      Check(result.ok() && result.preamble->page_size_bytes==sizes[p],"mixed owner profile selected exactly");
      const auto& other=headers[(p+1)%sizes.size()];
      const disk::FilespaceBootstrapBinding wrong{h.database_uuid,other.filespace_uuid,other.page_size_profile_uuid};
      result=disk::ReadFilespaceBootstrapFromOpenDevice(devices[p],&wrong);
      Check(!result.preamble && result.error==Error::binding_mismatch,"cross-filespace binding cannot substitute");
    }
  }
  for(unsigned p=0;p<sizes.size();++p) {
    const auto path=(fixture.root/("profile"+std::to_string(p))).string();
    const auto h=Header(p); const auto bytes=Oracle(h);
    disk::FileDevice device;
    Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"create private bootstrap fixture");
    // Intentionally NOT a complete node/page-zero body. This API only probes the preamble.
    std::vector<byte> image(sizes[p]); std::copy(bytes.begin(),bytes.end(),image.begin());
    Check(device.WriteAt(0,image.data(),image.size()).ok() && device.Sync().ok(),"persist independent fixture");
    reads=0; read_bytes=0; track_read=true;
    auto result=disk::ReadFilespaceBootstrapFromOpenDevice(device); track_read=false;
    Check(result.ok() && Oracle(*result.preamble)==bytes && reads==1 && read_bytes==4096,"exact fixed owned probe");
    Exclusive(path);
    read_failure=true; result=disk::ReadFilespaceBootstrapFromOpenDevice(device);
    Check(!result.preamble && result.error==Error::io_failure && !read_failure,"actual pread EIO");
    Check(device.is_open(),"I/O refusal retains caller ownership"); Exclusive(path);
    hash_failure=true; result=disk::ReadFilespaceBootstrapFromOpenDevice(device);
    Check(!result.preamble && result.error==Error::hash_provider_failure,"owned probe hash failure");
    read_failure=true; fail_allocation=true;
    result=disk::ReadFilespaceBootstrapFromOpenDevice(device);
    const bool allocation_consumed=!fail_allocation; fail_allocation=false; read_failure=false;
    Check(allocation_consumed && !result.preamble && result.error==Error::resource_exhausted,"I/O diagnostic allocation is contained");
    Check(device.is_open(),"allocation refusal retains caller ownership");
    Bytes after{}; Check(device.ReadAt(0,after.data(),after.size()).ok() && after==bytes,"all failures preserve bytes");
    Check(device.Close().ok(),"release fixture owner");
    Check(device.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"reopen readonly");
    result=disk::ReadFilespaceBootstrapFromOpenDevice(device);
    Check(result.ok() && device.read_only(),"readonly probe"); Exclusive(path);
    Check(device.Close().ok(),"release readonly owner");
    result=disk::ReadFilespaceBootstrapFromOpenDevice(device);
    Check(!result.preamble && result.error==Error::device_not_open,"closed owned probe refused");
  }
  for(auto length : {0u,1u,103u,135u,4095u}) {
    const auto path=(fixture.root/("short"+std::to_string(length))).string();
    disk::FileDevice device; Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"create short fixture");
    const auto bytes=Oracle(Header());
    Check(device.WriteAt(0,bytes.data(),length).ok(),"write actual short file");
    const auto result=disk::ReadFilespaceBootstrapFromOpenDevice(device);
    Check(!result.preamble && result.error==Error::io_failure,"short physical file no parsed authority");
  }
}
}  // namespace
int main(int argc,char** argv) {
  if(argc==3 && std::string_view(argv[1])=="--probe") {
    disk::FileDevice device;
    return LockFailure(device.Open(argv[2],disk::FileOpenMode::open_existing)) ? 0 : 1;
  }
  try { Codecs(); Files(); std::cout<<"PASS checks="<<checks<<" structural_bootstrap_only=true\n"; return 0; }
  catch(const std::exception& error) { std::cerr<<"FAIL "<<error.what()<<" checks="<<checks<<'\n'; return 1; }
}

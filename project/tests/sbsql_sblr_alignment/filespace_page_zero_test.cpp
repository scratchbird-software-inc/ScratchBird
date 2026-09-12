// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "filespace_page_zero.hpp"
#include "disk_device.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace {
long allocation_budget=-1;
unsigned hash_fault=0,reads=0,read_fault=0;
std::size_t observed_read_bytes=0;
bool track_reads=false;
const std::vector<unsigned char>* replace_on_second_read=nullptr;
}
void* operator new(std::size_t bytes) {
  if(allocation_budget==0) { allocation_budget=-1; throw std::bad_alloc(); }
  if(allocation_budget>0) --allocation_budget;
  if(auto* p=std::malloc(bytes?bytes:1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new() {
  if(hash_fault==1) { hash_fault=0; return nullptr; } return __real_EVP_MD_CTX_new();
}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* m,ENGINE* e) {
  if(hash_fault==2) { hash_fault=0; return 0; } return __real_EVP_DigestInit_ex(c,m,e);
}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* b,size_t n) {
  if(hash_fault==3) { hash_fault=0; return 0; } return __real_EVP_DigestUpdate(c,b,n);
}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* b,unsigned int* n) {
  if(hash_fault==4) { hash_fault=0; return 0; }
  const int result=__real_EVP_DigestFinal_ex(c,b,n);
  if(hash_fault==5) { hash_fault=0; *n=31; } return result;
}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* b,size_t n,off_t offset) {
  if(track_reads) { ++reads; observed_read_bytes+=n; }
  if(read_fault&&reads==read_fault) { read_fault=0; errno=EIO; return -1; }
  if(replace_on_second_read&&reads==2) {
    const auto* image=replace_on_second_read; replace_on_second_read=nullptr;
    if(::pwrite(fd,image->data(),image->size(),0)!=static_cast<ssize_t>(image->size())||::fsync(fd)!=0) {
      errno=EIO; return -1;
    }
  }
  return __real_pread(fd,b,n,offset);
}

namespace {
namespace disk=scratchbird::storage::disk;
using disk::byte; using disk::Uuid; using disk::u64;
using Error=disk::FilespacePageZeroError;
using Bytes=std::vector<byte>;
unsigned checks=0;
void Check(bool ok,std::string_view why,std::source_location at=std::source_location::current()) {
  ++checks; if(!ok) throw std::runtime_error(std::string(why)+" line="+std::to_string(at.line()));
}
Uuid Id(byte tag) { return Uuid{{1,2,3,4,5,6,0x71,8,0x89,10,11,12,13,14,15,tag}}; }
constexpr std::array<unsigned,5> sizes{{8192,16384,32768,65536,131072}};
constexpr std::array<std::array<byte,3>,5> suffixes{{
  {{0,0x81,0x92}},{{1,0x63,0x84}},{{3,0x27,0x68}},{{6,0x55,0x36}},{{0x13,0x10,0x72}}}};
constexpr std::array<unsigned,18> root_types{{0,8,5,3,769,9,10,11,5,768,771,773,775,779,1301,1030,1025,777}};
Uuid Profile(unsigned p) { return Uuid{{0,0,0,0,0,0,0x70,0,0x80,0,0,0,0,suffixes[p][0],suffixes[p][1],suffixes[p][2]}}; }
disk::FilespacePageZero Example(unsigned p=0,unsigned role=1,unsigned state=1,unsigned flags=0) {
  disk::FilespacePageZero v; auto& h=v.bootstrap;
  h.database_uuid=Id(1); h.filespace_uuid=Id(2); h.page_size_profile_uuid=Profile(p);
  h.page_size_bytes=sizes[p]; h.filespace_role=static_cast<disk::u16>(role);
  h.lifecycle_state=static_cast<disk::u16>(state); h.flags=flags;
  h.checksum_profile_uuid=Uuid{{0x01,0xa0,0x8e,0x13,0x41,0x6b,0x73,0xf6,0xb3,0xc5,0xbd,0xde,0x88,0xb1,0xf2,0xd9}};
  if(flags&1) h.encryption_profile_uuid=Id(4);
  v.page_uuid=Id(3); v.creation_operation_uuid=Id(5); v.writer_identity_uuid=Id(6);
  v.page_generation=7; v.root_set_generation=8; v.total_pages=64; v.free_pages=10;
  v.preallocated_pages=11; v.creation_utc_millis=1790000000000ull;
  for(unsigned kind=1;kind<=9;++kind) if(role<=4||kind==3)
    v.roots.push_back({static_cast<disk::u16>(kind),root_types[kind],h.filespace_uuid,
      kind+10,kind+100,h.page_size_profile_uuid,Id(static_cast<byte>(40+kind))});
  return v;
}
void Number(Bytes& b,std::size_t at,unsigned width,u64 value) {
  for(unsigned n=0;n<width;++n) b[at+n]=static_cast<byte>(value>>(8*n));
}
void PutUuid(Bytes& b,std::size_t at,Uuid id) {
  for(unsigned n=0;n<16;++n) b[at+n]=id.bytes[n];
}
void CommonSeal(Bytes& b) {
  u64 fnv=14695981039346656037ull;
  for(unsigned i=0;i<128;++i) { fnv^=(i>=96&&i<104)?0:b[4096+i]; fnv*=1099511628211ull; }
  Number(b,4192,8,fnv);
}
void FullSeal(Bytes& b) {
  std::fill(b.begin()+4448,b.begin()+4480,0); std::array<byte,32> digest{};
  Check(SHA256(b.data(),b.size(),digest.data())!=nullptr,"independent full SHA256");
  std::copy(digest.begin(),digest.end(),b.begin()+4448);
}
Bytes Oracle(const disk::FilespacePageZero& v) {
  const auto& h=v.bootstrap; Bytes b(h.page_size_bytes,0);
  const std::string_view pm="SBFP",cm="SBPGV002",fm="SBFZV001";
  std::copy(pm.begin(),pm.end(),b.begin()); Number(b,4,2,1); Number(b,6,2,4096);
  Number(b,8,4,h.page_size_bytes); Number(b,12,4,h.flags); PutUuid(b,16,h.database_uuid);
  PutUuid(b,32,h.filespace_uuid); PutUuid(b,48,h.page_size_profile_uuid);
  Number(b,64,4,h.durable_format_generation); Number(b,68,2,h.filespace_role); Number(b,70,2,h.lifecycle_state);
  PutUuid(b,72,h.checksum_profile_uuid); PutUuid(b,88,h.encryption_profile_uuid);
  Check(SHA256(b.data(),104,b.data()+104)!=nullptr,"independent preamble SHA256");
  std::copy(cm.begin(),cm.end(),b.begin()+4096); Number(b,4104,4,128);
  Number(b,4108,4,h.page_size_bytes); Number(b,4112,4,h.filespace_role<=4?1:2);
  Number(b,4116,2,1); Number(b,4118,2,1); PutUuid(b,4120,h.database_uuid);
  PutUuid(b,4136,h.filespace_uuid); PutUuid(b,4152,v.page_uuid); Number(b,4176,8,v.page_generation);
  Number(b,4184,8,h.flags&2); PutUuid(b,4200,h.page_size_profile_uuid); Number(b,4216,2,1); CommonSeal(b);
  constexpr unsigned f=4224;
  std::copy(fm.begin(),fm.end(),b.begin()+f); Number(b,f+8,4,256); Number(b,f+12,4,v.roots.size());
  Number(b,f+16,8,256+80*v.roots.size()); Number(b,f+24,8,v.root_set_generation);
  PutUuid(b,f+32,h.database_uuid); PutUuid(b,f+48,h.filespace_uuid); PutUuid(b,f+64,h.page_size_profile_uuid);
  PutUuid(b,f+80,h.checksum_profile_uuid); PutUuid(b,f+96,h.encryption_profile_uuid); PutUuid(b,f+112,v.page_uuid);
  Number(b,f+128,4,h.page_size_bytes); Number(b,f+132,4,h.durable_format_generation);
  Number(b,f+136,2,h.filespace_role); Number(b,f+138,2,h.lifecycle_state); Number(b,f+140,4,h.flags);
  Number(b,f+144,8,v.total_pages); Number(b,f+152,8,v.free_pages); Number(b,f+160,8,v.preallocated_pages);
  Number(b,f+168,8,v.page_generation); PutUuid(b,f+176,v.creation_operation_uuid); PutUuid(b,f+192,v.writer_identity_uuid);
  Number(b,f+208,8,v.creation_utc_millis); Number(b,f+216,4,80);
  unsigned at=4480;
  for(const auto& r:v.roots) {
    Number(b,at,2,r.kind); Number(b,at+4,4,r.page_type); PutUuid(b,at+8,r.filespace_uuid);
    Number(b,at+24,8,r.page_number); Number(b,at+32,8,r.page_generation);
    PutUuid(b,at+40,r.page_size_profile_uuid); PutUuid(b,at+56,r.object_uuid); at+=80;
  }
  FullSeal(b); return b;
}
void Reject(const Bytes& b,Error expected,std::source_location at=std::source_location::current()) {
  const auto r=disk::DecodeFilespacePageZero(b.data(),b.size());
  Check(!r.record&&r.error==expected,"precise atomic page-zero refusal actual="+
      std::to_string(static_cast<unsigned>(r.error))+" expected="+
      std::to_string(static_cast<unsigned>(expected)),at);
}
void Invalid(const disk::FilespacePageZero& v,Error expected) {
  Reject(Oracle(v),expected); const auto e=disk::EncodeFilespacePageZero(v);
  Check(!e.bytes&&e.error==expected,"invalid producer emits no bytes");
}
void Codecs() {
  for(unsigned k=0;k<root_types.size();++k) Check(disk::CanonicalPageZeroRootPageType(static_cast<disk::u16>(k))==root_types[k],"exact root symbol mapping");
  Check(disk::CanonicalPageZeroRootPageType(65535)==0,"unknown root kind");
  for(unsigned p=0;p<5;++p) for(unsigned role=1;role<=14;++role)
    for(unsigned state=1;state<=15;++state) for(unsigned flags=0;flags<4;++flags) {
      const auto v=Example(p,role,state,flags); const auto b=Oracle(v);
      const auto e=disk::EncodeFilespacePageZero(v);
      Check(e.ok()&&*e.bytes==b,"independent complete page-zero bytes");
      const disk::FilespaceBootstrapBinding binding{v.bootstrap.database_uuid,v.bootstrap.filespace_uuid,v.bootstrap.page_size_profile_uuid};
      const auto d=disk::DecodeFilespacePageZero(b.data(),b.size(),&binding);
      Check(d.ok()&&Oracle(*d.record)==b,"complete decoded metadata and bound identity");
    }
  const auto good=Oracle(Example());
  for(unsigned at=0;at<good.size();++at) for(unsigned bit=0;bit<8;++bit) {
    auto b=good; b[at]^=static_cast<byte>(1u<<bit);
    const auto d=disk::DecodeFilespacePageZero(b.data(),b.size()); Check(!d.ok()&&!d.record,"all image bits integrity protected");
  }
  for(unsigned n=0;n<good.size();++n) Check(!disk::DecodeFilespacePageZero(good.data(),n).record,"all image truncations atomic");
  Check(!disk::DecodeFilespacePageZero(nullptr,good.size()).record,"null image");
  Check(!disk::DecodeFilespacePageZero(good.data(),good.size()+1).record,"trailing image");
  for(unsigned offset : {4104u,4108u,4112u,4116u,4118u,4120u,4136u,4168u,4184u,4200u,4216u,4218u}) {
    auto b=good; b[offset]^=1; CommonSeal(b); FullSeal(b); Reject(b,Error::invalid_common_header);
  }
  // A different valid page UUID is valid common-header structure but must fail
  // the family duplicate-identity check, not pretend the UUID itself is invalid.
  { auto b=good; b[4152]^=1; CommonSeal(b); FullSeal(b); Reject(b,Error::invalid_family); }
  for(unsigned offset : {4256u,4272u,4288u,4304u,4320u,4336u,4352u,4356u,4360u,4362u,4364u,4392u,4440u,4444u}) {
    auto b=good; b[offset]^=1; FullSeal(b); Reject(b,Error::invalid_family);
  }
  auto number=[&](unsigned at,unsigned width,u64 value,Error error) { auto b=good; Number(b,at,width,value); FullSeal(b); Reject(b,error); };
  number(4236,4,33,Error::invalid_family); number(4482,2,1,Error::invalid_root_directory);
  number(4552,8,1,Error::invalid_root_directory); number(5200,1,1,Error::invalid_family);
  { auto v=Example(); v.roots.erase(v.roots.begin()); Invalid(v,Error::required_root_missing); }
  { auto v=Example(); v.roots[0].kind=0; Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].page_type=13; Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); std::swap(v.roots[0],v.roots[1]); Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].page_number=0; Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].page_generation=0; Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].page_number=64; Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].page_number=std::numeric_limits<u64>::max(); Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].page_size_profile_uuid=Profile(1); Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].object_uuid={}; Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[2].filespace_uuid=Id(50); Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].filespace_uuid=Id(50); v.roots[1].filespace_uuid=Id(50);
    v.roots[1].page_size_profile_uuid=Profile(1); Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.roots[0].page_number=v.roots[1].page_number; Invalid(v,Error::invalid_root_directory); }
  { auto v=Example(); v.free_pages=64; Invalid(v,Error::invalid_capacity); }
  { auto v=Example(); v.preallocated_pages=54; Invalid(v,Error::invalid_capacity); }
  { auto v=Example(); v.total_pages=std::numeric_limits<u64>::max(); Invalid(v,Error::invalid_capacity); }
  { auto v=Example(); v.page_generation=0; Check(!disk::EncodeFilespacePageZero(v).bytes,"zero page generation"); }
  { auto v=Example(); v.root_set_generation=0; Invalid(v,Error::invalid_family); }
  { auto v=Example(); v.writer_identity_uuid={}; Invalid(v,Error::invalid_family); }
  { auto v=Example(); v.creation_operation_uuid.bytes[6]=0x41; Invalid(v,Error::invalid_family); }
  for(unsigned kind=10;kind<=17;++kind) {
    auto v=Example(0,1,1,2); v.roots.push_back({static_cast<disk::u16>(kind),root_types[kind],v.bootstrap.filespace_uuid,
      30+kind,1,v.bootstrap.page_size_profile_uuid,Id(static_cast<byte>(60+kind))});
    Check(disk::EncodeFilespacePageZero(v).ok(),"all optional root kinds");
    if(kind>=15) { v.bootstrap.flags=0; Invalid(v,Error::invalid_root_directory); }
  }
  { auto v=Example(); v.roots[7]=v.roots[1]; v.roots[7].kind=8;
    Check(disk::EncodeFilespacePageZero(v).ok(),"identical shared catalog/feature authority alias"); }
  { auto v=Example(0,5,7); v.roots.clear(); v.total_pages=1; v.free_pages=0; v.preallocated_pages=0;
    Check(disk::EncodeFilespacePageZero(v).ok(),"non-serving initialization record, not open success");
    v.bootstrap.lifecycle_state=1; Invalid(v,Error::required_root_missing); }
  for(unsigned fault=1;fault<=5;++fault) {
    auto v=Example(); hash_fault=fault; const auto e=disk::EncodeFilespacePageZero(v);
    Check(!e.bytes&&e.error==Error::hash_provider_failure&&hash_fault==0,"encode real digest provider failure");
    hash_fault=fault; const auto d=disk::DecodeFilespacePageZero(good.data(),good.size());
    Check(!d.record&&d.error==Error::hash_provider_failure&&hash_fault==0,"decode real digest provider failure");
  }
  for(unsigned i=0;i<2;++i) {
    auto v=Example(); allocation_budget=0;
    if(i==0) { const auto e=disk::EncodeFilespacePageZero(v); allocation_budget=-1;
      Check(!e.bytes&&e.error==Error::resource_exhausted,"encode allocation atomicity"); }
    else { const auto d=disk::DecodeFilespacePageZero(good.data(),good.size()); allocation_budget=-1;
      Check(!d.record&&d.error==Error::resource_exhausted,"decode allocation atomicity"); }
  }
}
bool Locked(const disk::IoResult& r) {
  return !r.ok()&&(r.diagnostic.diagnostic_code=="SB-STORAGE-DISK-OWNER-LOCK-HELD"
      ||r.diagnostic.diagnostic_code=="SB-STORAGE-DISK-DATA-OWNER-LOCK-HELD"
      ||r.diagnostic.diagnostic_code=="SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD");
}
struct Fixture {
  std::filesystem::path root;
  Fixture() { std::string s=(std::filesystem::temp_directory_path()/"sb_pagezero.XXXXXX").string();
    std::vector<char> b(s.begin(),s.end()); b.push_back(0); const auto* p=::mkdtemp(b.data()); Check(p,"temp root"); root=p; }
  ~Fixture() { std::error_code e; std::filesystem::remove_all(root,e); }
};
void Exclusive(const std::string& path) {
  disk::FileDevice second; Check(Locked(second.Open(path,disk::FileOpenMode::open_existing)),"local owner exclusion");
  const auto pid=::fork(); Check(pid>=0,"fork ownership probe");
  if(pid==0) { ::execl("/proc/self/exe","pagezero_test","--probe",path.c_str(),nullptr); ::_exit(125); }
  int s=0; Check(::waitpid(pid,&s,0)==pid&&WIFEXITED(s)&&WEXITSTATUS(s)==0,"fresh-exec process excluded");
}
void Files() {
  Fixture fixture;
  for(unsigned p=0;p<5;++p) {
    const auto path=(fixture.root/std::to_string(p)).string(); auto v=Example(p); const auto bytes=Oracle(v);
    disk::FileDevice device; Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"own fixture file");
    Check(device.WriteAt(0,bytes.data(),bytes.size()).ok(),"write actual page zero");
    const byte zero=0; Check(device.WriteAt(v.total_pages*sizes[p]-1,&zero,1).ok()&&device.Sync().ok(),"physical capacity");
    reads=0; observed_read_bytes=0; track_reads=true;
    auto r=disk::ReadFilespacePageZeroFromOpenDevice(device); track_reads=false;
    Check(r.ok()&&Oracle(*r.record)==bytes&&reads==2&&observed_read_bytes==4096+sizes[p],"exact probe then full-page read");
    Exclusive(path);
    for(unsigned fault=1;fault<=2;++fault) {
      reads=0; read_fault=fault; track_reads=true; r=disk::ReadFilespacePageZeroFromOpenDevice(device); track_reads=false;
      Check(!r.record&&r.error==Error::io_failure&&read_fault==0,"actual first/second pread failure");
    }
    auto changed=v; changed.bootstrap.database_uuid=Id(90); const auto alternate=Oracle(changed);
    reads=0; track_reads=true; replace_on_second_read=&alternate;
    r=disk::ReadFilespacePageZeroFromOpenDevice(device); track_reads=false;
    Check(!r.record&&r.error==Error::probe_changed&&!replace_on_second_read,"actual intervening page replacement rejected");
    Check(device.WriteAt(0,bytes.data(),bytes.size()).ok()&&device.Sync().ok(),"restore own race fixture");
    Exclusive(path);
    Check(device.Close().ok()&&device.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"readonly reopen");
    r=disk::ReadFilespacePageZeroFromOpenDevice(device); Check(r.ok()&&device.read_only(),"readonly complete metadata probe");
    const disk::FilespaceBootstrapBinding wrong{Id(77),v.bootstrap.filespace_uuid,v.bootstrap.page_size_profile_uuid};
    r=disk::ReadFilespacePageZeroFromOpenDevice(device,&wrong); Check(!r.record&&r.error==Error::invalid_bootstrap,"wrong node binding");
    Check(device.Close().ok(),"close fixture"); r=disk::ReadFilespacePageZeroFromOpenDevice(device);
    Check(!r.record&&r.error==Error::device_not_open,"closed owned reader");
  }
  const auto path=(fixture.root/"capacity-mismatch").string(); auto v=Example(); const auto bytes=Oracle(v);
  disk::FileDevice d; Check(d.Open(path,disk::FileOpenMode::create_new).ok(),"capacity fixture");
  Check(d.WriteAt(0,bytes.data(),bytes.size()).ok(),"only pagezero persisted");
  auto r=disk::ReadFilespacePageZeroFromOpenDevice(d); Check(!r.record&&r.error==Error::invalid_capacity,"no fabricated remaining pages");
  const byte z=0; Check(d.WriteAt(v.total_pages*sizes[0],&z,1).ok(),"actual unaccounted appended byte");
  r=disk::ReadFilespacePageZeroFromOpenDevice(d); Check(!r.record&&r.error==Error::invalid_capacity,"partial trailing page refused");
}
}  // namespace
int main(int argc,char** argv) {
  if(argc==3&&std::string_view(argv[1])=="--probe") { disk::FileDevice d; return Locked(d.Open(argv[2],disk::FileOpenMode::open_existing))?0:1; }
  try { Codecs(); Files(); std::cout<<"PASS checks="<<checks<<" page_zero_metadata_only=true\n"; return 0; }
  catch(const std::exception& e) { allocation_budget=-1; std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n'; return 1; }
}

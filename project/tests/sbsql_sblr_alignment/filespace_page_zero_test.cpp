// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "filespace_page_zero.hpp"
#include "catalog_page.hpp"
#include "physical_mga_cow_store.hpp"
#include "transaction_inventory_page.hpp"
#include "database_dirty_manifest.hpp"
#include "disk_device.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <atomic>
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
#include <thread>
#include <unistd.h>

namespace {
long allocation_budget=-1;
unsigned long observed_allocations=0;
bool count_allocations=false;
unsigned hash_fault=0,reads=0,read_fault=0;
unsigned full_digest_fault=0;
std::size_t observed_read_bytes=0;
bool track_reads=false;
const std::vector<unsigned char>* replace_on_second_read=nullptr;
}
void* operator new(std::size_t bytes) {
  if(count_allocations) ++observed_allocations;
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
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* b,size_t n,unsigned char* out,unsigned int* count,const EVP_MD* md,ENGINE* e) {
  if(full_digest_fault && --full_digest_fault==0) return 0;
  return __real_EVP_Digest(b,n,out,count,md,e);
}
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
namespace page=scratchbird::storage::page;
using RootError=page::NativeCatalogRootError;
page::NativeCatalogRoot RootExample(unsigned p=0) {
  page::NativeCatalogRoot r;
  r.header={sizes[p],5,Id(1),Id(2),Id(90),12,102,0,Profile(p)};
  r.object_uuid=Id(42); r.creator_transaction_uuid=Id(91);
  r.creator_local_transaction_id=17; r.catalog_generation=1;
  r.schema_epoch=2; r.security_epoch=3;
  for(unsigned role=1;role<=6;++role)
    r.roots.push_back({static_cast<disk::u16>(role),role%2?6u:512u,
      {Id(2),20+role,7,Profile(p)},Id(static_cast<byte>(100+role))});
  return r;
}
void RootSeal(Bytes& b) {
  std::fill(b.begin()+304,b.begin()+336,0);
  std::array<byte,32> digest{};
  Check(SHA256(b.data(),b.size(),digest.data())!=nullptr,"independent catalog SHA256");
  std::copy(digest.begin(),digest.end(),b.begin()+304);
}
Bytes RootOracle(const page::NativeCatalogRoot& r) {
  Bytes b(r.header.page_size_bytes,0); const auto& h=r.header;
  const std::string_view cm="SBPGV002",fm="SBCROOT1";
  std::copy(cm.begin(),cm.end(),b.begin()); Number(b,8,4,128);
  Number(b,12,4,h.page_size_bytes); Number(b,16,4,h.page_type);
  Number(b,20,2,1); Number(b,22,2,1); PutUuid(b,24,h.database_uuid);
  PutUuid(b,40,h.filespace_uuid); PutUuid(b,56,h.page_uuid);
  Number(b,72,8,h.page_number); Number(b,80,8,h.page_generation);
  Number(b,88,8,h.flags); PutUuid(b,104,h.page_size_profile_uuid); Number(b,120,2,1);
  u64 fnv=14695981039346656037ull;
  for(unsigned i=0;i<128;++i) { fnv^=b[i]; fnv*=1099511628211ull; }
  Number(b,96,8,fnv);
  std::copy(fm.begin(),fm.end(),b.begin()+128); Number(b,136,2,1); Number(b,138,2,256);
  Number(b,140,4,384+80*r.roots.size()); Number(b,144,2,r.root_kind);
  Number(b,146,2,r.roots.size()); Number(b,152,8,r.catalog_generation);
  Number(b,160,8,r.schema_epoch); Number(b,168,8,r.security_epoch);
  Number(b,176,8,r.resource_epoch); Number(b,184,8,r.creator_local_transaction_id);
  PutUuid(b,192,r.object_uuid); PutUuid(b,208,r.creator_transaction_uuid);
  auto put_ref=[&](std::size_t at,const page::NativeCatalogPageReference& ref) {
    PutUuid(b,at,ref.filespace_uuid); Number(b,at+16,8,ref.page_number);
    Number(b,at+24,8,ref.page_generation); PutUuid(b,at+32,ref.page_size_profile_uuid);
  };
  if(r.predecessor) put_ref(224,*r.predecessor);
  std::copy(r.predecessor_sha256.begin(),r.predecessor_sha256.end(),b.begin()+272);
  for(std::size_t i=0;i<r.roots.size();++i) {
    const auto& ref=r.roots[i]; const auto at=384+80*i;
    Number(b,at,2,ref.role); Number(b,at+4,4,ref.page_type); put_ref(at+8,ref.page);
    PutUuid(b,at+56,ref.object_uuid);
  }
  RootSeal(b); return b;
}
void RootReject(const Bytes& b,RootError expected,std::source_location at=std::source_location::current()) {
  const auto r=page::DecodeNativeCatalogRoot(b);
  Check(!r.ok()&&!r.root&&r.bytes.empty()&&r.error==expected,"atomic catalog root refusal actual="+
    std::to_string(static_cast<unsigned>(r.error))+" expected="+
    std::to_string(static_cast<unsigned>(expected)),at);
}
void RootInvalid(const page::NativeCatalogRoot& r,RootError error) {
  RootReject(RootOracle(r),error); const auto e=page::EncodeNativeCatalogRoot(r);
  Check(!e.ok()&&!e.root&&e.bytes.empty()&&e.error==error,"invalid root emits no image");
}
void CatalogRoots() {
  for(unsigned p=0;p<5;++p) for(unsigned kind:{2u,8u}) {
    auto r=RootExample(p); r.root_kind=static_cast<disk::u16>(kind);
    if(kind==8) r.roots.erase(r.roots.begin(),r.roots.end()-1);
    for(unsigned generation:{1u,2u}) {
      r.catalog_generation=generation;
      if(generation==2) {
        r.predecessor=page::NativeCatalogPageReference{Id(2),10,1,Profile(p)};
        r.predecessor_sha256.fill(9);
      }
      const auto expected=RootOracle(r); const auto e=page::EncodeNativeCatalogRoot(r);
      Check(e.ok()&&e.bytes==expected,"all profiles/kinds/generations independent root bytes");
      const auto d=page::DecodeNativeCatalogRoot(expected);
      Check(d.ok()&&RootOracle(*d.root)==expected,"independent root decode");
    }
  }
  const auto good=RootOracle(RootExample());
  for(std::size_t at=0;at<good.size();++at) {
    auto b=good; b[at]^=1; const auto r=page::DecodeNativeCatalogRoot(b);
    Check(!r.ok()&&!r.root&&r.bytes.empty(),"every byte corruption rejected without partial root");
  }
  for(unsigned at:{128u,136u,138u,140u,148u,336u,383u,864u,8191u}) {
    auto b=good; b[at]^=1; RootSeal(b); RootReject(b,RootError::invalid_family);
  }
  for(unsigned at:{386u,387u,456u,463u}) {
    auto b=good; b[at]=1; RootSeal(b); RootReject(b,RootError::invalid_roots);
  }
  for(std::size_t size:{0u,127u,383u,8191u,8193u,131073u}) {
    auto b=good; b.resize(size); RootReject(b,RootError::invalid_header);
  }
  {auto r=RootExample(); r.root_kind=7; Check(!page::EncodeNativeCatalogRoot(r).ok(),"unknown kind producer");}
  {auto r=RootExample(); r.header.page_type=6; RootInvalid(r,RootError::invalid_header);}
  {auto r=RootExample(); r.object_uuid.bytes[6]=0x41; RootInvalid(r,RootError::invalid_family);}
  {auto r=RootExample(); r.creator_transaction_uuid={}; RootInvalid(r,RootError::invalid_family);}
  {auto r=RootExample(); r.creator_local_transaction_id=0; RootInvalid(r,RootError::invalid_family);}
  {auto r=RootExample(); r.catalog_generation=0; RootInvalid(r,RootError::invalid_family);}
  {auto r=RootExample(); r.schema_epoch=0; RootInvalid(r,RootError::invalid_family);}
  {auto r=RootExample(); r.security_epoch=0; RootInvalid(r,RootError::invalid_family);}
  {auto r=RootExample(); r.catalog_generation=2; RootInvalid(r,RootError::invalid_reference);}
  {auto r=RootExample(); r.predecessor_sha256[0]=1; RootInvalid(r,RootError::invalid_reference);}
  {auto r=RootExample(); r.roots[0].role=2; RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.roots[0].page_type=5; RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.roots[0].object_uuid={}; RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.roots[0].page.page_number=12; RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.roots[0].page.page_generation=0; RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.roots[0].page.page_number=std::numeric_limits<u64>::max(); RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.roots[0].page.page_size_profile_uuid=Profile(1); RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.roots[1].page=r.roots[0].page; RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.roots[1].object_uuid=r.roots[0].object_uuid; RootInvalid(r,RootError::invalid_roots);}
  {auto r=RootExample(); r.catalog_generation=2;
    r.predecessor=page::NativeCatalogPageReference{Id(2),12,1,Profile(0)};
    r.predecessor_sha256[0]=1; RootInvalid(r,RootError::invalid_reference);
    r.predecessor->page_number=21; RootInvalid(r,RootError::invalid_roots);
    r.predecessor->page_number=10; r.predecessor->page_size_profile_uuid=Profile(1);
    RootInvalid(r,RootError::invalid_reference);}
  {auto r=RootExample(); r.roots[0].page.filespace_uuid=Id(88); r.roots[0].page.page_size_profile_uuid=Profile(1);
    Check(page::EncodeNativeCatalogRoot(r).ok(),"cross-filespace profile retained, not opened");
    r.roots[1].page.filespace_uuid=Id(88); RootInvalid(r,RootError::invalid_roots);}
  for(unsigned fault=1;fault<=5;++fault) {
    auto value=RootExample(); hash_fault=fault; const auto e=page::EncodeNativeCatalogRoot(value);
    Check(!e.ok()&&e.bytes.empty()&&!e.root&&e.error==RootError::hash_failure&&!hash_fault,"root encode digest failure");
    hash_fault=fault; RootReject(good,RootError::hash_failure); Check(!hash_fault,"root decode digest fault consumed");
  }
  for(unsigned i=0;i<2;++i) {
    const auto value=RootExample(); allocation_budget=0;
    const auto r=i? page::DecodeNativeCatalogRoot(good):page::EncodeNativeCatalogRoot(value);
    allocation_budget=-1;
    Check(!r.ok()&&!r.root&&r.bytes.empty()&&r.error==RootError::resource_exhausted,"root allocation failure atomicity");
  }
}
void CatalogRootFiles() {
  Fixture fixture;
  for(unsigned p=0;p<5;++p) {
    auto z=Example(p); auto root=RootExample(p); const auto image=RootOracle(root);
    const auto path=(fixture.root/("catalog-"+std::to_string(p))).string();
    disk::FileDevice device; Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"own root file");
    const auto pagezero=Oracle(z); const byte zero=0;
    Check(device.WriteAt(0,pagezero.data(),pagezero.size()).ok()
      &&device.WriteAt(z.total_pages*sizes[p]-1,&zero,1).ok(),"real root filespace capacity");
    const auto& ref=z.roots[1];
    Check(device.WriteAt(ref.page_number*sizes[p],image.data(),image.size()).ok()&&device.Sync().ok(),"real root persisted");
    auto r=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),ref);
    Check(r.ok()&&r.bytes==image,"read exact canonical catalog target, no prototype fallback");
    auto feature=ref; feature.kind=8;
    Check(page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),feature).ok(),"shared feature root selects catalog role6");
    Exclusive(path);
    for(unsigned fault=1;fault<=3;++fault) {
      reads=0; read_fault=fault; track_reads=true;
      r=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),ref); track_reads=false;
      Check(!r.ok()&&!r.root&&r.bytes.empty()&&r.error==RootError::io_failure&&!read_fault,"root actual pread failure");
    }
    for(unsigned field=0;field<4;++field) {
      auto bad=ref;
      if(field==0) ++bad.page_generation;
      if(field==1) bad.object_uuid=Id(99);
      if(field==2) bad.filespace_uuid=Id(99);
      if(field==3) bad.page_size_profile_uuid=Profile((p+1)%5);
      r=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),bad);
      Check(!r.ok()&&!r.root&&r.bytes.empty(),"exact owner/generation/filespace/profile binding");
    }
    Check(!page::ReadNativeCatalogRootFromOpenDevice(device,Id(99),ref).ok(),"other database refused");
    auto changed=root; changed.header.flags=1; auto badimage=RootOracle(changed);
    Check(device.WriteAt(ref.page_number*sizes[p],badimage.data(),badimage.size()).ok(),"encrypted-header fixture");
    r=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),ref);
    Check(r.error==RootError::encrypted_requires_crypto_authority&&!r.root&&r.bytes.empty(),"no plaintext encrypted root admission");
    changed=root; changed.root_kind=8; changed.roots.erase(changed.roots.begin(),changed.roots.end()-1);
    badimage=RootOracle(changed);
    Check(device.WriteAt(ref.page_number*sizes[p],badimage.data(),badimage.size()).ok(),"dedicated feature root persisted");
    Check(page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),feature).ok(),"dedicated feature root read");
    r=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),ref);
    Check(r.error==RootError::binding_mismatch&&!r.root,"dedicated feature root cannot stand in for catalog");
    changed=root; changed.header.database_uuid=Id(99); badimage=RootOracle(changed);
    Check(device.WriteAt(ref.page_number*sizes[p],badimage.data(),badimage.size()).ok(),"other node root image fixture");
    r=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),ref);
    Check(r.error==RootError::binding_mismatch&&!r.root,"actual image node binding checked");
    changed=root; changed.roots[0].page.page_number=z.total_pages; badimage=RootOracle(changed);
    Check(device.WriteAt(ref.page_number*sizes[p],badimage.data(),badimage.size()).ok(),"outside-capacity target fixture");
    r=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),ref);
    Check(r.error==RootError::invalid_reference&&!r.root,"actual local target capacity checked");
    Check(device.WriteAt(ref.page_number*sizes[p],image.data(),image.size()).ok()&&device.Sync().ok(),"restore own root fixture");
    Check(device.Close().ok()&&device.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"reopen canonical root read-only");
    r=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),ref);
    Check(r.ok()&&r.bytes==image&&device.read_only(),"read-only reopen actual root");
    Bytes actual(image.size()); Check(device.ReadAt(ref.page_number*sizes[p],actual.data(),actual.size()).ok()
      &&actual==image,"root reader did not mutate persisted image"); Exclusive(path);
    Check(device.Close().ok(),"close root fixture");
    Check(!page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),ref).ok(),"closed root device refused");
    const auto pid=::fork(); Check(pid>=0,"fork root reopen verifier");
    if(pid==0) { const auto profile=std::to_string(p);
      ::execl("/proc/self/exe","pagezero_test","--catalog-probe",path.c_str(),profile.c_str(),nullptr); ::_exit(125); }
    int status=0;
    Check(::waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh process verifies exact durable root");
  }
}
std::array<byte,32> WholeRootHash(const Bytes& bytes) {
  std::array<byte,32> out{};
  Check(SHA256(bytes.data(),bytes.size(),out.data())!=nullptr,"independent complete root image digest");
  return out;
}
disk::FilespaceRootReference RootRef(const page::NativeCatalogRoot& root) {
  const auto& h=root.header;
  return {2,5,h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid,root.object_uuid};
}
page::NativeCatalogPageReference PageRef(const page::NativeCatalogRoot& root) {
  const auto& h=root.header; return {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};
}
struct RootRangeFixture {
  Fixture files;
  disk::FileDevice first,second;
  std::array<page::NativeCatalogRoot,3> roots;
  std::array<Bytes,3> images;
  std::vector<page::NativeCatalogFilespaceDevice> devices;
  RootRangeFixture() {
    roots[0]=RootExample(); roots[1]=RootExample(1); roots[2]=RootExample();
    roots[1].header.filespace_uuid=Id(7); roots[1].header.page_number=34;
    roots[1].header.page_generation=202; roots[1].header.page_uuid=Id(93);
    for(auto& target:roots[1].roots) target.page.filespace_uuid=Id(7);
    roots[2].header.page_number=35; roots[2].header.page_generation=302; roots[2].header.page_uuid=Id(94);
    for(unsigned i=0;i<3;++i) {
      roots[i].catalog_generation=i+1; roots[i].schema_epoch=2+i;
      roots[i].security_epoch=3+i; roots[i].resource_epoch=i;
      if(i) { roots[i].predecessor=PageRef(roots[i-1]); roots[i].predecessor_sha256=WholeRootHash(images[i-1]); }
      images[i]=RootOracle(roots[i]);
    }
    Check(first.Open((files.root/"first").string(),disk::FileOpenMode::create_new).ok()
      &&second.Open((files.root/"second").string(),disk::FileOpenMode::create_new).ok(),"own two filespaces of one node");
    auto a=Example(),b=Example(1); b.bootstrap.filespace_uuid=Id(7); b.page_uuid=Id(4);
    for(auto& ref:b.roots) ref.filespace_uuid=Id(7);
    a.roots[1]=RootRef(roots[2]); b.roots[1]=RootRef(roots[1]); const byte zero=0;
    const auto ab=Oracle(a),bb=Oracle(b);
    Check(first.WriteAt(0,ab.data(),ab.size()).ok()&&second.WriteAt(0,bb.data(),bb.size()).ok()
      &&first.WriteAt(a.total_pages*sizes[0]-1,&zero,1).ok()
      &&second.WriteAt(b.total_pages*sizes[1]-1,&zero,1).ok(),"persist actual mixed-profile page zeros");
    Restore();
    devices={{Id(7),Profile(1),&second},{Id(2),Profile(0),&first}};
  }
  void Write(unsigned i,const Bytes& bytes) {
    auto& device=i==1?second:first;
    Check(device.WriteAt(roots[i].header.page_number*roots[i].header.page_size_bytes,bytes.data(),bytes.size()).ok(),"write own range root fixture");
  }
  void Restore() { for(unsigned i=0;i<3;++i) Write(i,images[i]);
    Check(first.Sync().ok()&&second.Sync().ok(),"sync actual range fixture"); }
  page::NativeCatalogRootRangeResult Read(u64 budget=32768,unsigned terminal=0) {
    return page::ReadNativeCatalogRootRangeFromOpenDevices(Id(1),devices,RootRef(roots[2]),RootRef(roots[terminal]),budget);
  }
};
void RangeReject(const page::NativeCatalogRootRangeResult& r,RootError error,
                 std::source_location at=std::source_location::current()) {
  Check(!r.ok()&&r.roots.empty()&&!r.retained_image_bytes&&r.error==error,"range fails without prefix actual="+
    std::to_string(static_cast<unsigned>(r.error))+" expected="+std::to_string(static_cast<unsigned>(error)),at);
}
void CatalogRootRanges() {
  RootRangeFixture f;
  auto r=f.Read(); Check(r.ok()&&r.roots.size()==3&&r.retained_image_bytes==32768,"actual mixed-profile root range");
  for(unsigned i=0;i<3;++i) Check(r.roots[i].bytes==f.images[2-i],"exact ordered independent range images");
  auto ordered=f.devices; std::reverse(f.devices.begin(),f.devices.end());
  Check(f.Read().ok(),"input order does not control filespace locking"); f.devices=ordered;
  Check(f.Read(8192,2).ok(),"one-root exact head/terminal range");
  Check(f.Read(24576,1).ok(),"explicit retained terminal stops before older history");
  for(u64 budget:{0ull,8191ull,8192ull,24575ull,24576ull,32767ull}) RangeReject(f.Read(budget),RootError::resource_exhausted);
  for(const auto [budget,expected_reads]:std::array<std::pair<u64,unsigned>,3>{{{8191,4},{8192,7},{24576,10}}}) {
    reads=0; track_reads=true; r=f.Read(budget); track_reads=false;
    RangeReject(r,RootError::resource_exhausted);
    Check(reads==expected_reads,"byte allowance checked before reading the next root image");
  }
  {auto middle=f.roots[1],head=f.roots[2];
    middle.header.filespace_uuid=Id(2); middle.header.page_size_profile_uuid=Profile(0);
    middle.header.page_size_bytes=sizes[0];
    for(auto& target:middle.roots) { target.page.filespace_uuid=Id(2); target.page.page_size_profile_uuid=Profile(0); }
    const auto image=RootOracle(middle);
    Check(f.first.WriteAt(middle.header.page_number*sizes[0],image.data(),image.size()).ok(),"actual same-filespace predecessor image");
    head.predecessor=PageRef(middle); head.predecessor_sha256=WholeRootHash(image); f.Write(2,RootOracle(head));
    r=f.Read(24576); Check(r.ok()&&r.roots.size()==3&&r.retained_image_bytes==24576
      &&r.roots[1].bytes==image,"actual three-generation same-filespace range"); f.Restore();}
  auto bad_terminal=RootRef(f.roots[0]); ++bad_terminal.page_generation;
  RangeReject(page::ReadNativeCatalogRootRangeFromOpenDevices(Id(1),f.devices,RootRef(f.roots[2]),bad_terminal,65536),RootError::history_mismatch);
  bad_terminal=RootRef(f.roots[0]); bad_terminal.object_uuid=Id(99);
  RangeReject(page::ReadNativeCatalogRootRangeFromOpenDevices(Id(1),f.devices,RootRef(f.roots[2]),bad_terminal,65536),RootError::invalid_reference);
  f.devices.pop_back(); RangeReject(f.Read(),RootError::invalid_filespace); f.devices=ordered;
  f.devices.push_back(f.devices[0]); RangeReject(f.Read(),RootError::invalid_filespace); f.devices=ordered;
  f.devices[1].device=f.devices[0].device; RangeReject(f.Read(),RootError::invalid_filespace); f.devices=ordered;
  f.devices[0].device=nullptr; RangeReject(f.Read(),RootError::invalid_filespace); f.devices=ordered;
  f.devices[0].page_size_profile_uuid=Profile(0); RangeReject(f.Read(),RootError::invalid_filespace); f.devices=ordered;
  RangeReject(page::ReadNativeCatalogRootRangeFromOpenDevices(Id(99),f.devices,RootRef(f.roots[2]),RootRef(f.roots[0]),32768),RootError::invalid_filespace);
  // An unused device cannot smuggle a second node into an owning-node range.
  disk::FileDevice other; Check(other.Open((f.files.root/"other-node").string(),disk::FileOpenMode::create_new).ok(),"other-node fixture");
  auto z=Example(); z.bootstrap.database_uuid=Id(99); z.bootstrap.filespace_uuid=Id(88);
  for(auto& ref:z.roots) ref.filespace_uuid=Id(88);
  const auto zb=Oracle(z); const byte zero=0;
  Check(other.WriteAt(0,zb.data(),zb.size()).ok()&&other.WriteAt(z.total_pages*sizes[0]-1,&zero,1).ok(),"other-node actual pagezero");
  f.devices.push_back({Id(88),Profile(0),&other}); RangeReject(f.Read(),RootError::invalid_filespace); f.devices=ordered;
  reads=0; track_reads=true; r=f.Read(); track_reads=false;
  Check(r.ok()&&reads==13,"two filespace probes and three actual root reads");
  for(unsigned fault=1;fault<=13;++fault) {
    reads=0; read_fault=fault; track_reads=true; r=f.Read(); track_reads=false;
    RangeReject(r,RootError::io_failure); Check(!read_fault,"range actual read fault consumed");
  }
  for(unsigned fault:{1u,2u}) { full_digest_fault=fault; r=f.Read();
    RangeReject(r,RootError::hash_failure); Check(!full_digest_fault,"actual predecessor full-image hash failure"); }
  for(unsigned i:{0u,1u}) {
    auto damaged=f.images[i]; damaged[700]^=1; f.Write(i,damaged);
    Check(!f.Read().ok(),"deep image corruption not accepted as missing history");
    r=f.Read(); Check(r.roots.empty()&&!r.retained_image_bytes,"deep corruption emits no prefix"); f.Restore();
  }
  // Reseal both sides so these tests exercise links/epochs, not just checksums.
  for(unsigned field=0;field<6;++field) {
    auto middle=f.roots[1],head=f.roots[2];
    if(field==0) middle.catalog_generation=4;
    if(field==1) middle.schema_epoch=head.schema_epoch+1;
    if(field==2) middle.security_epoch=head.security_epoch+1;
    if(field==3) middle.resource_epoch=head.resource_epoch+1;
    if(field==4) middle.object_uuid=Id(99);
    if(field==5) middle.predecessor=PageRef(head); // cycle; no digest fixed point is needed to reject it.
    const auto bytes=RootOracle(middle); f.Write(1,bytes); head.predecessor_sha256=WholeRootHash(bytes);
    f.Write(2,RootOracle(head));
    RangeReject(f.Read(),field==4?RootError::binding_mismatch:RootError::history_mismatch); f.Restore();
  }
  {auto head=f.roots[2]; head.predecessor_sha256[0]^=1; f.Write(2,RootOracle(head));
    RangeReject(f.Read(),RootError::invalid_integrity); f.Restore();}
  // Retained ranges do not claim authority over images older than their terminal.
  {auto damaged=f.images[0]; damaged[500]^=1; f.Write(0,damaged);
    Check(f.Read(24576,1).ok(),"unrequested older image is outside the proven range"); f.Restore();}
  bool saw_failure=false,saw_success=false;
  const auto head=RootRef(f.roots[2]),terminal=RootRef(f.roots[0]);
  observed_allocations=0; count_allocations=true;
  r=page::ReadNativeCatalogRootRangeFromOpenDevices(Id(1),f.devices,head,terminal,32768);
  count_allocations=false;
  const auto allocation_count=observed_allocations;
  Check(r.ok()&&allocation_count>0,"measure actual range allocation positions");
  for(unsigned long budget=0;budget<=allocation_count;++budget) {
    allocation_budget=budget;
    r=page::ReadNativeCatalogRootRangeFromOpenDevices(Id(1),f.devices,head,terminal,32768);
    allocation_budget=-1;
    if(r.ok()) { saw_success=true; break; }
    RangeReject(r,RootError::resource_exhausted); saw_failure=true;
  }
  Check(saw_failure&&saw_success,"all allocation failure positions through successful complete range");
  std::atomic<bool> concurrent_ok=true;
  auto reverse=f.devices; std::reverse(reverse.begin(),reverse.end());
  auto reader=[&](const auto& devices) { for(unsigned i=0;i<16;++i) {
    const auto result=page::ReadNativeCatalogRootRangeFromOpenDevices(Id(1),devices,head,terminal,32768);
    if(!result.ok()||result.roots.size()!=3||result.retained_image_bytes!=32768) concurrent_ok=false;
  }};
  std::thread a([&]{reader(f.devices);}),b([&]{reader(reverse);}); a.join(); b.join();
  Check(concurrent_ok,"concurrent reversed-order range readers complete");
  for(unsigned i=0;i<3;++i) {
    Bytes bytes(f.images[i].size()); auto& device=i==1?f.second:f.first;
    Check(device.ReadAt(f.roots[i].header.page_number*f.roots[i].header.page_size_bytes,bytes.data(),bytes.size()).ok()
      &&bytes==f.images[i],"range reads/failures did not mutate durable roots");
  }
  Exclusive(f.first.path()); Exclusive(f.second.path());
  Check(f.first.Close().ok()&&f.second.Close().ok(),"close node filespaces before fresh process range read");
  const auto first=(f.files.root/"first").string(),second=(f.files.root/"second").string();
  const auto pid=::fork(); Check(pid>=0,"fork range reopen verifier");
  if(pid==0) { ::execl("/proc/self/exe","pagezero_test","--range-probe",first.c_str(),second.c_str(),nullptr); ::_exit(125); }
  int status=0; Check(::waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh process validates mixed-profile retained range");
}
namespace db=scratchbird::storage::database;
namespace catalog=scratchbird::core::catalog;
namespace platform=scratchbird::core::platform;
db::NativeCatalogLeafPage LeafExample(unsigned p=0) {
  db::NativeCatalogLeafPage leaf; leaf.header=RootExample(p).header;
  leaf.header.page_type=6; leaf.header.page_number=21; leaf.header.page_generation=7; leaf.header.page_uuid=Id(150);
  auto& b=leaf.body; b.relation_uuid={platform::UuidKind::object,Id(101)};
  b.segment_id=1; b.segment_generation=2; b.compaction_generation=3;
  b.page_number=21; b.page_generation=7;
  for(unsigned i=0;i<2;++i) {
    page::RowDataRecord row; row.row_uuid={platform::UuidKind::row,Id(static_cast<byte>(160+i))};
    row.version_uuid=Id(static_cast<byte>(170+i)); row.transaction_uuid={platform::UuidKind::transaction,Id(162)};
    row.local_transaction_id=13; row.internal_row_ordinal=i+1; row.stable_slot_id=i+1;
    catalog::CatalogMetadataVersion metadata;
    metadata.record.header.kind=catalog::CatalogRecordKind::sql_object;
    metadata.record.header.row_uuid=row.row_uuid;
    metadata.record.header.object_uuid={platform::UuidKind::object,Id(static_cast<byte>(180+i))};
    metadata.record.header.parent_uuid={platform::UuidKind::schema,Id(164)};
    metadata.owning_schema_uuid=metadata.record.header.parent_uuid;
    metadata.owner_uuid={platform::UuidKind::principal,Id(165)}; metadata.audit_uuid={platform::UuidKind::object,Id(166)};
    metadata.creator_transaction_uuid=row.transaction_uuid; metadata.creator_local_transaction_id=13;
    metadata.definition_version=metadata.schema_epoch=metadata.security_epoch=metadata.catalog_generation=1;
    metadata.dependency_generation=metadata.invalidation_generation=1;
    metadata.lifecycle=catalog::CatalogObjectLifecycle::active; metadata.status=catalog::CatalogObjectStatus::active;
    metadata.trace_search_key="LEAF-STORAGE-ORACLE"; metadata.object_subtype="application"; metadata.retention_class="catalog_history";
    // Opaque common-envelope storage fixture, not an admitted SQL definition.
    metadata.record.payload="opaque-leaf-family-oracle";
    const auto encoded=catalog::EncodeCatalogMetadataVersion(metadata); Check(encoded.ok(),"leaf metadata fixture");
    page::RowDataCell cell; cell.column_ordinal=1;
    cell.value.type_id=scratchbird::core::datatypes::CanonicalTypeId::binary; cell.value.payload=encoded.bytes;
    row.cells.push_back(cell); b.rows.push_back(row);
  }
  return leaf;
}
u64 BodyFnv(const byte* b,std::size_t n) {
  u64 hash=1469598103934665603ull; for(std::size_t i=0;i<n;++i) {hash^=b[i];hash*=1099511628211ull;} return hash;
}
void LeafSeal(Bytes& b) {
  std::fill(b.end()-32,b.end(),0); const auto digest=WholeRootHash(b);
  std::copy(digest.begin(),digest.end(),b.end()-32);
}
Bytes LeafOracle(const db::NativeCatalogLeafPage& leaf) {
  const auto& h=leaf.header; const auto& body=leaf.body; Bytes b(h.page_size_bytes,0);
  const std::string_view cm="SBPGV002",rm="SBROW003",vm="SBDVAL01";
  std::copy(cm.begin(),cm.end(),b.begin()); Number(b,8,4,128); Number(b,12,4,h.page_size_bytes);
  Number(b,16,4,h.page_type); Number(b,20,2,1); Number(b,22,2,1);
  PutUuid(b,24,h.database_uuid); PutUuid(b,40,h.filespace_uuid); PutUuid(b,56,h.page_uuid);
  Number(b,72,8,h.page_number); Number(b,80,8,h.page_generation); Number(b,88,8,h.flags);
  PutUuid(b,104,h.page_size_profile_uuid); Number(b,120,2,1);
  u64 common=14695981039346656037ull; for(unsigned i=0;i<128;++i) {common^=b[i];common*=1099511628211ull;} Number(b,96,8,common);
  std::copy(rm.begin(),rm.end(),b.begin()+128); Number(b,136,4,96); Number(b,140,4,body.rows.size());
  Number(b,152,8,body.next_page_number); PutUuid(b,168,body.relation_uuid.value);
  Number(b,184,8,body.page_generation); Number(b,192,8,body.segment_id);
  Number(b,200,8,body.segment_generation); Number(b,208,8,body.compaction_generation);
  struct Slot {unsigned stable,offset,size;u64 hash;bool deleted;}; std::vector<Slot> slots;
  unsigned at=224;
  for(unsigned i=0;i<body.rows.size();++i) {
    const auto& row=body.rows[i]; const auto start=at;
    PutUuid(b,at,row.row_uuid.value); PutUuid(b,at+16,row.transaction_uuid.value);
    Number(b,at+32,8,row.local_transaction_id); Number(b,at+40,8,row.row_version);
    Number(b,at+48,2,row.deleted?1:0); Number(b,at+50,2,row.cells.size()); Number(b,at+52,4,i+1);
    Number(b,at+60,4,row.stable_slot_id); PutUuid(b,at+72,row.version_uuid);
    Number(b,at+88,8,row.previous_row_version); Number(b,at+96,8,row.next_row_version);
    PutUuid(b,at+104,row.previous_version_uuid); PutUuid(b,at+120,row.next_version_uuid); at+=136;
    for(const auto& cell:row.cells) {
      const auto& payload=cell.value.payload; const unsigned value=at+16;
      Number(b,at,2,cell.column_ordinal); Number(b,at+4,4,32+payload.size());
      std::copy(vm.begin(),vm.end(),b.begin()+value); Number(b,value+8,4,static_cast<unsigned>(cell.value.type_id));
      Number(b,value+12,2,(cell.value.is_null?1:0)|(cell.value.payload_is_toast_reference?2:0));
      Number(b,value+14,2,32); Number(b,value+16,4,payload.size());
      Number(b,value+24,8,BodyFnv(payload.data(),payload.size()));
      std::copy(payload.begin(),payload.end(),b.begin()+value+32);
      Number(b,at+8,8,BodyFnv(b.data()+value,32+payload.size())); at+=48+payload.size();
    }
    Number(b,start+56,4,at-start); const auto hash=BodyFnv(b.data()+start,at-start); Number(b,start+64,8,hash);
    slots.push_back({row.stable_slot_id,start-128,at-start,hash,row.deleted});
  }
  Number(b,216,4,at-128);
  for(const auto& slot:slots) {
    Number(b,at,4,slot.stable); Number(b,at+4,4,slot.offset); Number(b,at+8,4,slot.size);
    Number(b,at+12,4,slot.deleted?1:0); Number(b,at+16,8,slot.hash); at+=24;
  }
  Number(b,144,4,at-128); Number(b,220,4,b.size()-32-at);
  Number(b,160,8,BodyFnv(b.data()+128,b.size()-160)); LeafSeal(b); return b;
}
void LeafReject(const db::NativeCatalogLeafResult& r,db::NativeCatalogLeafError error,
                std::source_location at=std::source_location::current()) {
  Check(!r.ok()&&!r.page&&r.metadata.empty()&&r.bytes.empty()&&r.error==error,"leaf rejects without partial page/metadata actual="+
    std::to_string(static_cast<unsigned>(r.error))+" expected="+std::to_string(static_cast<unsigned>(error)),at);
}
void CatalogLeaves() {
  using Error=db::NativeCatalogLeafError;
  for(unsigned p=0;p<5;++p) {
    auto leaf=LeafExample(p); const auto expected=LeafOracle(leaf);
    const auto e=db::EncodeNativeCatalogLeaf(leaf);
    Check(e.ok()&&e.bytes==expected&&e.metadata.size()==2,"all profiles exact independent catalog leaf bytes");
    const auto d=db::DecodeNativeCatalogLeaf(expected);
    Check(d.ok()&&LeafOracle(*d.page)==expected&&d.metadata.size()==2,"independent canonical leaf decode");
    for(const auto& row:leaf.body.rows) Check(d.metadata.at(row.version_uuid).record.header.row_uuid.value==row.row_uuid.value,
      "metadata keyed by actual native version UUID");
    leaf.body.rows.clear(); const auto empty=db::EncodeNativeCatalogLeaf(leaf);
    Check(empty.ok()&&empty.bytes==LeafOracle(leaf)&&empty.metadata.empty(),"empty allocated leaf, not absent table success");
  }
  const auto leaf=LeafExample(); const auto good=LeafOracle(leaf);
  for(std::size_t at=0;at<good.size();++at) {
    auto b=good; b[at]^=1; const auto r=db::DecodeNativeCatalogLeaf(b);
    Check(!r.ok()&&!r.page&&r.metadata.empty()&&r.bytes.empty(),"every leaf byte corruption fails without prefix");
  }
  for(std::size_t size:{0u,127u,8191u,8193u}) {auto b=good;b.resize(size);LeafReject(db::DecodeNativeCatalogLeaf(b),Error::invalid_header);}
  for(unsigned mode=0;mode<9;++mode) {
    auto bad=leaf;
    if(mode==0) bad.header.page_type=5;
    if(mode==1) bad.body.page_generation=8;
    if(mode==2) bad.body.next_page_number=99;
    if(mode==3) bad.body.compaction_generation=0;
    if(mode==4) bad.body.rows[1].stable_slot_id=0;
    if(mode==5) bad.body.rows[1].transaction_uuid.value=Id(99);
    if(mode==6) ++bad.body.rows[1].local_transaction_id;
    if(mode==7) bad.body.rows[1].cells[0].column_ordinal=2;
    if(mode==8) bad.body.rows[1].deleted=true;
    const auto expected=mode==0?Error::invalid_header:mode<5?Error::invalid_body:Error::invalid_metadata;
    LeafReject(db::EncodeNativeCatalogLeaf(bad),expected);
    LeafReject(db::DecodeNativeCatalogLeaf(LeafOracle(bad)),expected);
  }
  {auto bad=leaf;++bad.body.page_number;LeafReject(db::EncodeNativeCatalogLeaf(bad),Error::invalid_body);}
  {auto b=good; b[b.size()-33]=1; Number(b,160,8,0); Number(b,160,8,BodyFnv(b.data()+128,b.size()-160)); LeafSeal(b);
    LeafReject(db::DecodeNativeCatalogLeaf(b),Error::invalid_body);}
  for(unsigned i=0;i<2;++i) {
    allocation_budget=0; const auto r=i?db::DecodeNativeCatalogLeaf(good):db::EncodeNativeCatalogLeaf(leaf);
    allocation_budget=-1; LeafReject(r,Error::resource_exhausted);
  }
  for(unsigned fault=1;fault<=5;++fault) {
    hash_fault=fault; const auto r=db::DecodeNativeCatalogLeaf(good);
    LeafReject(r,Error::hash_failure); Check(!hash_fault,"leaf digest provider fault consumed");
  }
  for(unsigned mode=0;mode<2;++mode) {
    full_digest_fault=1; auto r=mode?db::DecodeNativeCatalogLeaf(good):db::EncodeNativeCatalogLeaf(leaf);
    LeafReject(r,Error::hash_failure); Check(!full_digest_fault,"nested metadata hash failure propagated");
    observed_allocations=0;count_allocations=true;
    r=mode?db::DecodeNativeCatalogLeaf(good):db::EncodeNativeCatalogLeaf(leaf);
    count_allocations=false;const auto count=observed_allocations;Check(r.ok(),"leaf allocation baseline");
    bool succeeded=false;
    for(unsigned long position=0;position<=count;++position) {
      allocation_budget=position;r=mode?db::DecodeNativeCatalogLeaf(good):db::EncodeNativeCatalogLeaf(leaf);allocation_budget=-1;
      if(r.ok()) {succeeded=true;break;} LeafReject(r,Error::resource_exhausted);
    }
    Check(succeeded,"every leaf allocation position through complete page");
  }
}
void CatalogLeafFiles() {
  using Error=db::NativeCatalogLeafError; Fixture fixture;
  for(unsigned p=0;p<5;++p) {
    auto leaf=LeafExample(p); const auto image=LeafOracle(leaf); auto z=Example(p,p==0?5:1);
    const auto ref=RootExample(p).roots[0]; const auto pagezero=Oracle(z);
    const auto path=(fixture.root/("leaf-"+std::to_string(p))).string(); disk::FileDevice device;
    Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"own canonical leaf file"); const byte zero=0;
    Check(device.WriteAt(0,pagezero.data(),pagezero.size()).ok()&&device.WriteAt(z.total_pages*sizes[p]-1,&zero,1).ok()
      &&device.WriteAt(21*sizes[p],image.data(),image.size()).ok()&&device.Sync().ok(),"actual canonical leaf persisted");
    auto r=db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),ref);
    Check(r.ok()&&r.bytes==image&&r.metadata.size()==2,"read actual bound leaf in primary/secondary filespace");
    if(p!=0) {
      const auto root_image=RootOracle(RootExample(p));
      Check(device.WriteAt(12*sizes[p],root_image.data(),root_image.size()).ok()&&device.Sync().ok(),"persist owning canonical catalog root");
      const auto actual_root=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),z.roots[1]);
      Check(actual_root.ok(),"resolve actual root before leaf traversal");
      r=db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),actual_root.root->roots[0]);
      Check(r.ok()&&r.bytes==image&&r.metadata.size()==2,"actual pagezero to root to leaf to bound metadata");
    }
    for(unsigned field=0;field<4;++field) {
      auto bad=ref;
      if(field==0) ++bad.page.page_generation;
      if(field==1) bad.object_uuid=Id(99);
      if(field==2) bad.page.page_number=22;
      if(field==3) bad.page.filespace_uuid=Id(99);
      r=db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),bad);
      Check(!r.ok()&&!r.page&&r.metadata.empty()&&r.bytes.empty(),"wrong actual leaf binding no partial metadata");
    }
    for(unsigned fault=1;fault<=3;++fault) {
      reads=0;read_fault=fault;track_reads=true;r=db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),ref);track_reads=false;
      LeafReject(r,Error::io_failure);Check(!read_fault,"actual leaf pread fault consumed");
    }
    auto encrypted=leaf;encrypted.header.flags=1;const auto encrypted_image=LeafOracle(encrypted);
    Check(device.WriteAt(21*sizes[p],encrypted_image.data(),encrypted_image.size()).ok(),"encrypted leaf fixture");
    LeafReject(db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),ref),Error::encrypted_requires_crypto_authority);
    Check(device.WriteAt(21*sizes[p],image.data(),image.size()).ok()&&device.Sync().ok(),"restore own leaf fixture");
    Exclusive(path); Check(device.Close().ok()&&device.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"readonly leaf reopen");
    r=db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),ref);
    Check(r.ok()&&r.bytes==image&&device.read_only(),"actual reopened leaf metadata");
    Bytes actual(image.size());Check(device.ReadAt(21*sizes[p],actual.data(),actual.size()).ok()&&actual==image,"leaf reads/faults never mutate image");
    Check(device.Close().ok(),"close leaf file");LeafReject(db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),ref),Error::invalid_filespace);
    const auto pid=::fork();Check(pid>=0,"fork leaf reopen verifier");
    if(pid==0) {const auto profile=std::to_string(p);::execl("/proc/self/exe","pagezero_test","--leaf-probe",path.c_str(),profile.c_str(),nullptr);::_exit(125);}
    int status=0;Check(::waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh process validates exact leaf bytes and metadata");
  }
}
}  // namespace
namespace mga=scratchbird::transaction::mga;
page::NativeTransactionInventoryPage InventoryExample(unsigned profile=0) {
  page::NativeTransactionInventoryPage p;
  p.header={sizes[profile],0x0301,Id(1),Id(2),Id(92),14,104,0,Profile(profile)};
  p.object_uuid=Id(44); p.inventory_generation=19;
  p.inventory.next_local_transaction_id=3; p.inventory.next_commit_sequence=1;
  mga::TransactionInventoryEntry e; e.identity.local_id=mga::MakeLocalTransactionId(1);
  e.identity.transaction_uuid={scratchbird::core::platform::UuidKind::transaction,Id(93)};
  e.identity.scope=mga::TransactionScope::local_node; e.state=mga::TransactionState::active;
  e.begin_unix_epoch_millis=1790000000123ull;
  p.inventory.entries.push_back(e); return p;
}
disk::NativePageReference InventoryRef(const page::NativeTransactionInventoryPage& p) {
  return {p.header.filespace_uuid,p.header.page_number,p.header.page_generation,p.header.page_size_profile_uuid};
}
void InventorySeal(Bytes& b) {
  std::fill(b.begin()+320,b.begin()+352,0); std::array<byte,32> digest{};
  Check(SHA256(b.data(),b.size(),digest.data())!=nullptr,"independent inventory SHA256");
  std::copy(digest.begin(),digest.end(),b.begin()+320);
}
// Independent framing/number packing. The fixture deliberately controls the
// expected summaries; this oracle never calls production inventory/horizon code.
Bytes InventoryOracle(const page::NativeTransactionInventoryPage& p,u64 oit,u64 oat,u64 ost) {
  auto common=RootExample(); common.header=p.header;
  auto b=RootOracle(common); std::fill(b.begin()+128,b.end(),0);
  const std::string_view magic="SBTINV01"; std::copy(magic.begin(),magic.end(),b.begin()+128);
  Number(b,136,2,1); Number(b,138,2,256); Number(b,140,4,384+72*p.inventory.entries.size());
  PutUuid(b,144,p.object_uuid); Number(b,160,8,p.inventory_generation);
  Number(b,168,8,p.inventory.next_local_transaction_id); Number(b,176,8,p.inventory.next_commit_sequence);
  Number(b,184,4,p.inventory.entries.size());
  const auto ref=[&](std::size_t at,const disk::NativePageReference& r) {
    PutUuid(b,at,r.filespace_uuid); Number(b,at+16,8,r.page_number);
    Number(b,at+24,8,r.page_generation); PutUuid(b,at+32,r.page_size_profile_uuid);
  };
  if(p.previous) ref(192,*p.previous); if(p.next) ref(240,*p.next);
  Number(b,288,8,oit); Number(b,296,8,oat); Number(b,304,8,ost);
  for(std::size_t i=0;i<p.inventory.entries.size();++i) {
    const auto& e=p.inventory.entries[i]; const auto at=384+72*i;
    Number(b,at,8,e.identity.local_id.value); PutUuid(b,at+8,e.identity.transaction_uuid.value);
    Number(b,at+24,2,static_cast<unsigned>(e.identity.scope)); Number(b,at+26,2,static_cast<unsigned>(e.state));
    const unsigned origin=e.archived_from_state==mga::TransactionState::committed?1:
      e.archived_from_state==mga::TransactionState::rolled_back?2:e.archived_from_state==mga::TransactionState::failed_terminal?3:0;
    Number(b,at+28,4,(e.evidence_record_required?1:0)|(e.evidence_record_written?2:0)|(e.rollback_only?4:0)|(origin<<3));
    Number(b,at+32,8,e.begin_unix_epoch_millis); Number(b,at+40,8,e.final_unix_epoch_millis);
    Number(b,at+48,8,e.begin_visible_through_local_transaction_id);
    Number(b,at+56,8,e.begin_visible_through_commit_sequence); Number(b,at+64,8,e.commit_sequence);
  }
  InventorySeal(b); return b;
}
void InventoryReject(const page::NativeTransactionInventoryPageResult& r) {
  Check(!r.ok()&&!r.page&&r.bytes.empty(),"invalid inventory returns no page prefix");
}
void CanonicalInventoryImages() {
  using E=page::NativeInventoryError;
  for(unsigned profile=0;profile<5;++profile) {
    auto p=InventoryExample(profile); const auto oracle=InventoryOracle(p,1,1,1);
    const auto encoded=page::EncodeNativeTransactionInventoryPage(p);
    if(!encoded.ok()) std::cerr<<"inventory_encode_error="<<static_cast<unsigned>(encoded.error)<<'\n';
    else if(encoded.bytes!=oracle) {
      const auto mismatch=std::mismatch(encoded.bytes.begin(),encoded.bytes.end(),oracle.begin());
      std::cerr<<"inventory_oracle_offset="<<(mismatch.first-encoded.bytes.begin())
        <<" actual="<<static_cast<unsigned>(*mismatch.first)<<" expected="<<static_cast<unsigned>(*mismatch.second)<<'\n';
    }
    Check(encoded.ok()&&encoded.bytes==oracle,"independent canonical inventory bytes all profiles");
    const auto decoded=page::DecodeNativeTransactionInventoryPage(oracle);
    Check(decoded.ok()&&decoded.page->inventory.entries.size()==1
      &&decoded.page->inventory.entries[0].identity.transaction_uuid.value==Id(93)
      &&!decoded.page->inventory.publication_base,"decode binary identity without invented publication base");
    p.inventory.entries.clear(); const auto empty=InventoryOracle(p,3,3,3);
    Check(page::EncodeNativeTransactionInventoryPage(p).bytes==empty
      &&page::DecodeNativeTransactionInventoryPage(empty).ok(),"empty allocated native inventory slice");
    const auto capacity=(sizes[profile]-384)/72;
    for(unsigned i=0;i<capacity;++i) {
      auto e=InventoryExample().inventory.entries[0]; e.identity.local_id=mga::MakeLocalTransactionId(i+1);
      e.identity.transaction_uuid.value=Id(93); e.identity.transaction_uuid.value.bytes[14]=static_cast<byte>(i>>8);
      e.identity.transaction_uuid.value.bytes[15]=static_cast<byte>(i);
      p.inventory.entries.push_back(e);
    }
    p.inventory.next_local_transaction_id=capacity+1;
    const auto full=page::EncodeNativeTransactionInventoryPage(p);
    Check(full.ok()&&full.bytes==InventoryOracle(p,1,1,1)
      &&page::DecodeNativeTransactionInventoryPage(full.bytes).page->inventory.entries.size()==capacity,"exact full inventory capacity");
    p.inventory.entries.push_back(p.inventory.entries.back()); InventoryReject(page::EncodeNativeTransactionInventoryPage(p));
  }
  auto p=InventoryExample(); const auto good=InventoryOracle(p,1,1,1);
  for(std::size_t i=0;i<good.size();++i) { auto b=good;b[i]^=1;InventoryReject(page::DecodeNativeTransactionInventoryPage(b)); }
  for(std::size_t at:{128u,136u,138u,140u,176u,184u,188u,192u,240u,288u,296u,304u,312u,352u,456u}) {
    auto b=good; b[at]^=1; InventorySeal(b); InventoryReject(page::DecodeNativeTransactionInventoryPage(b));
  }
  for(std::size_t at:{144u,160u,168u}) {
    auto b=good;std::fill_n(b.begin()+at,at==144?16:8,0);InventorySeal(b);InventoryReject(page::DecodeNativeTransactionInventoryPage(b));
  }
  for(unsigned state:{0u,14u,65535u}) { auto b=good;Number(b,410,2,state);InventorySeal(b);InventoryReject(page::DecodeNativeTransactionInventoryPage(b)); }
  for(unsigned scope:{2u,65535u}) { auto b=good;Number(b,408,2,scope);InventorySeal(b);InventoryReject(page::DecodeNativeTransactionInventoryPage(b)); }
  for(std::size_t at:{398u,400u,412u,448u}) {
    auto b=good; b[at]=255; InventorySeal(b); InventoryReject(page::DecodeNativeTransactionInventoryPage(b));
  }
  for(std::size_t size:{0u,127u,8191u,8193u}) {auto b=good;b.resize(size);InventoryReject(page::DecodeNativeTransactionInventoryPage(b));}
  for(auto origin:{mga::TransactionState::committed,mga::TransactionState::rolled_back,mga::TransactionState::failed_terminal}) {
    auto archived=p;auto& e=archived.inventory.entries[0];e.state=mga::TransactionState::archived;e.archived_from_state=origin;
    if(origin==mga::TransactionState::committed) {e.commit_sequence=1;archived.inventory.next_commit_sequence=2;}
    const auto bytes=InventoryOracle(archived,3,3,3);const auto r=page::DecodeNativeTransactionInventoryPage(bytes);
    Check(r.ok()&&r.page->inventory.entries[0].archived_from_state==origin
      &&page::EncodeNativeTransactionInventoryPage(archived).bytes==bytes,"archive terminal origin survives canonical inventory");
  }
  for(unsigned fault=1;fault<=4;++fault) {
    hash_fault=fault;const auto r=page::DecodeNativeTransactionInventoryPage(good);
    Check(!r.ok()&&r.error==E::hash_failure&&hash_fault==0&&!r.page&&r.bytes.empty(),"inventory digest backend failure");
  }
  for(unsigned mode=0;mode<2;++mode) {
    bool success=false;
    for(long budget=0;budget<100;++budget) {
      allocation_budget=budget;const auto r=mode?page::DecodeNativeTransactionInventoryPage(good):page::EncodeNativeTransactionInventoryPage(p);allocation_budget=-1;
      if(r.ok()){success=true;break;} Check(r.error==E::resource_exhausted&&!r.page&&r.bytes.empty(),"inventory allocation fails atomically");
    }
    Check(success,"inventory all allocation positions complete");
  }
}
void CanonicalInventoryChains() {
  using E=page::NativeInventoryError; Fixture fixture; disk::FileDevice first,second;
  auto head=InventoryExample(),tail=InventoryExample(1);
  tail.header.filespace_uuid=Id(7);tail.header.page_number=17;tail.header.page_uuid=Id(94);
  tail.inventory.entries[0].identity.local_id=mga::MakeLocalTransactionId(2);
  tail.inventory.entries[0].identity.transaction_uuid.value=Id(95);tail.inventory.entries[0].state=mga::TransactionState::rolled_back;
  head.next=InventoryRef(tail);tail.previous=InventoryRef(head);
  auto z1=Example(),z2=Example(1);z2.bootstrap.filespace_uuid=Id(7);
  for(auto& root:z2.roots)root.filespace_uuid=Id(7);
  const auto path1=(fixture.root/"inventory-primary").string(),path2=(fixture.root/"inventory-secondary-primary").string();
  Check(first.Open(path1,disk::FileOpenMode::create_new).ok()&&second.Open(path2,disk::FileOpenMode::create_new).ok(),"own both inventory filespaces");
  auto persist=[&](disk::FileDevice& d,const auto& zero,const auto& p,u64 horizon) {
    const auto z=Oracle(zero),b=InventoryOracle(p,horizon,horizon,horizon);const byte v=0;
    Check(d.WriteAt(0,z.data(),z.size()).ok()&&d.WriteAt(zero.total_pages*zero.bootstrap.page_size_bytes-1,&v,1).ok()
      &&d.WriteAt(p.header.page_number*p.header.page_size_bytes,b.data(),b.size()).ok()&&d.Sync().ok(),"persist actual canonical inventory images");
  };
  persist(first,z1,head,1);persist(second,z2,tail,3);
  const std::vector<disk::NativeFilespaceDevice> devices{{Id(7),Profile(1),&second},{Id(2),Profile(0),&first}};
  const auto root=z1.roots[3];
  const auto read=[&](u64 budget=24576) {return page::ReadNativeTransactionInventoryChainFromOpenDevices(Id(1),devices,root,budget);};
  const auto check_empty=[&](const auto& r) {Check(!r.ok()&&r.pages.empty()&&r.inventory.entries.empty()&&!r.inventory.publication_base&&!r.horizons.valid&&r.retained_image_bytes==0,"chain failure retains no prefix or authority");};
  auto r=read();Check(r.ok()&&r.pages.size()==2&&r.inventory.entries.size()==2&&r.retained_image_bytes==24576
    &&r.horizons.oldest_active_transaction.value==1&&!r.inventory.publication_base,"full mixed-profile binary chain and horizons");
  r=read(24575);check_empty(r);Check(r.error==E::resource_exhausted,"exact retained image budget");
  for(unsigned field=0;field<7;++field) {
    auto bad=tail;
    if(field==0)bad.previous.reset();if(field==1)++bad.previous->page_generation;
    if(field==2)++bad.inventory_generation;if(field==3)++bad.inventory.next_local_transaction_id;
    if(field==4)bad.header.page_uuid=head.header.page_uuid;
    if(field==5)bad.inventory.entries[0].identity.transaction_uuid=head.inventory.entries[0].identity.transaction_uuid;
    if(field==6)bad.inventory.entries[0].identity.local_id=mga::MakeLocalTransactionId(1);
    persist(second,z2,bad,bad.inventory.next_local_transaction_id);check_empty(read());
  }
  persist(second,z2,tail,3);
  auto duplicate=devices;duplicate.push_back(devices[0]);
  check_empty(page::ReadNativeTransactionInventoryChainFromOpenDevices(Id(1),duplicate,root,24576));
  for(unsigned field=0;field<4;++field){auto bad=root;if(field==0)++bad.page_generation;if(field==1)bad.object_uuid=Id(98);if(field==2)bad.kind=2;if(field==3)bad.filespace_uuid=Id(98);check_empty(page::ReadNativeTransactionInventoryChainFromOpenDevices(Id(1),devices,bad,24576));}
  for(unsigned fault=1;fault<=4;++fault){hash_fault=fault;r=read();Check(hash_fault==0,"chain hash fault consumed");check_empty(r);Check(r.error==E::hash_failure,"chain hash backend refusal");}
  reads=0;track_reads=true;r=read();track_reads=false;const auto read_count=reads;
  Check(r.ok()&&read_count>3,"measure complete actual chain reads");
  for(unsigned fault=1;fault<=read_count;++fault){reads=0;read_fault=fault;track_reads=true;r=read();track_reads=false;Check(read_fault==0,"chain actual read fault consumed");check_empty(r);}
  observed_allocations=0;count_allocations=true;r=read();count_allocations=false;
  const auto allocation_count=observed_allocations;Check(r.ok(),"measure complete chain allocations");
  bool success=false;
  for(unsigned long budget=0;budget<=allocation_count;++budget){allocation_budget=static_cast<long>(budget);r=read();allocation_budget=-1;if(r.ok()){success=true;break;}check_empty(r);Check(r.error==E::resource_exhausted,"chain allocation diagnostic");}
  Check(success,"complete chain under all allocation positions");
  auto bad_tail=InventoryOracle(tail,3,3,3);bad_tail[420]^=1;
  Check(second.WriteAt(tail.header.page_number*tail.header.page_size_bytes,bad_tail.data(),bad_tail.size()).ok()&&second.Sync().ok(),"persist late-page corruption");
  r=read();check_empty(r);Check(r.error==E::invalid_integrity,"late-page corruption rejects earlier inventory prefix");
  persist(second,z2,tail,3);
  auto committed_head=head,committed_tail=tail;
  for(auto* p:{&committed_head,&committed_tail}){p->inventory.entries[0].state=mga::TransactionState::committed;p->inventory.entries[0].commit_sequence=1;p->inventory.next_commit_sequence=2;}
  persist(first,z1,committed_head,3);persist(second,z2,committed_tail,3);
  r=read();check_empty(r);Check(r.error==E::invalid_inventory,"duplicate commit order across individually valid pages refused");
  committed_tail.inventory.entries[0].commit_sequence=2;committed_head.inventory.next_commit_sequence=committed_tail.inventory.next_commit_sequence=3;
  persist(first,z1,committed_head,3);persist(second,z2,committed_tail,3);
  r=read();Check(r.ok()&&r.inventory.entries[1].commit_sequence==2,"distinct global committed order admitted");
  persist(first,z1,head,1);persist(second,z2,tail,3);
  auto encrypted=tail;encrypted.header.flags=1;persist(second,z2,encrypted,3);
  r=read();check_empty(r);Check(r.error==E::encrypted_requires_crypto_authority,"raw encrypted inventory never interpreted as plaintext");
  persist(second,z2,tail,3);
  auto missing=devices;missing.erase(missing.begin());check_empty(page::ReadNativeTransactionInventoryChainFromOpenDevices(Id(1),missing,root,24576));
  auto mismatched=devices;mismatched[0].page_size_profile_uuid=Profile(0);check_empty(page::ReadNativeTransactionInventoryChainFromOpenDevices(Id(1),mismatched,root,24576));
  std::atomic<unsigned> completed=0;
  auto reverse=devices;std::reverse(reverse.begin(),reverse.end());
  const auto concurrent=[&](const auto& owners){for(unsigned n=0;n<16;++n){const auto result=page::ReadNativeTransactionInventoryChainFromOpenDevices(Id(1),owners,root,24576);if(result.ok()&&result.inventory.entries.size()==2)++completed;}};
  std::thread one([&]{concurrent(devices);}),two([&]{concurrent(reverse);});one.join();two.join();
  Check(completed==32,"opposite supplied device orders converge on one binary lock order");
  Exclusive(path1);Exclusive(path2);
  Check(first.Close().ok()&&second.Close().ok(),"close canonical inventory filespaces");
  Check(first.Open(path1,disk::FileOpenMode::open_existing_read_only).ok()&&second.Open(path2,disk::FileOpenMode::open_existing_read_only).ok(),"reopen canonical inventory filespaces");
  r=read();Check(r.ok()&&r.inventory.entries[1].state==mga::TransactionState::rolled_back,"reopened canonical inventory exact outcomes");
  Check(first.Close().ok()&&second.Close().ok(),"release before fresh process inventory reader");
  const auto child=::fork();Check(child>=0,"fork independent inventory reader");
  if(child==0){::execl("/proc/self/exe","inventory-chain-probe","--inventory-chain-probe",fixture.root.c_str(),nullptr);::_exit(125);}
  int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable reads actual canonical inventory chain");
}
db::NativeCheckpointRoot CheckpointExample(unsigned profile=0) {
  db::NativeCheckpointRoot r;r.header={sizes[profile],0x300,Id(1),Id(2),Id(96),19,109,0,Profile(profile)};
  r.object_uuid=Id(49);r.checkpoint_generation=1;r.root_set_generation=8;
  r.selected_local_transaction_id=17;r.stable_local_transaction_id=12;r.local_durable_transaction_id=16;
  r.timeline_uuid=Id(97);r.creator_transaction_uuid=Id(98);r.creator_local_transaction_id=17;r.completed=true;
  constexpr unsigned type[]={0,769,770,9,3,5,10,11,8,5,771,773,775,776,777,779};
  for(unsigned role=1;role<=10;++role) {
    db::NativeCheckpointRootReference root;root.role=role;root.page_type=type[role];
    root.page={Id(2),30+role,80+role,Profile(profile)};root.object_uuid=Id(static_cast<byte>(110+role));
    const std::string fixture="checkpoint-target-image-oracle-"+std::to_string(role);
    Check(SHA256(reinterpret_cast<const byte*>(fixture.data()),fixture.size(),root.sha256.data())!=nullptr,"independent target digest fixture");
    r.roots.push_back(root);
  }
  return r;
}
void CheckpointSeal(Bytes& b,bool root_set=true) {
  std::array<byte,32> digest{};
  if(root_set) {
    std::size_t used=0;for(unsigned i=0;i<4;++i)used|=static_cast<std::size_t>(b[140+i])<<(8*i);
    Check(used>=512&&used<=b.size(),"independent checkpoint root extent before sealing");
    const std::string_view domain="SBCPSET1";Bytes material(domain.begin(),domain.end());
    material.insert(material.end(),b.begin()+160,b.begin()+256);
    material.insert(material.end(),b.begin()+512,b.begin()+used);
    Check(SHA256(material.data(),material.size(),digest.data())!=nullptr,"independent checkpoint root-set SHA256");
    std::copy(digest.begin(),digest.end(),b.begin()+336);
  }
  std::fill(b.begin()+368,b.begin()+400,0);
  Check(SHA256(b.data(),b.size(),digest.data())!=nullptr,"independent checkpoint full SHA256");
  std::copy(digest.begin(),digest.end(),b.begin()+368);
}
Bytes CheckpointOracle(const db::NativeCheckpointRoot& r) {
  auto common=RootExample();common.header=r.header;auto b=RootOracle(common);std::fill(b.begin()+128,b.end(),0);
  const std::string_view magic="SBCPNT01";std::copy(magic.begin(),magic.end(),b.begin()+128);
  Number(b,136,2,1);Number(b,138,2,384);Number(b,140,4,512+112*r.roots.size());PutUuid(b,144,r.object_uuid);
  Number(b,160,8,r.checkpoint_generation);Number(b,168,8,r.root_set_generation);Number(b,176,8,r.selected_local_transaction_id);
  Number(b,184,8,r.stable_local_transaction_id);Number(b,192,8,r.local_durable_transaction_id);Number(b,200,8,r.cluster_quorum_transaction_id);
  PutUuid(b,208,r.timeline_uuid);PutUuid(b,224,r.creator_transaction_uuid);Number(b,240,8,r.creator_local_transaction_id);Number(b,248,8,r.flags);
  const auto ref=[&](std::size_t at,const disk::NativePageReference& p){PutUuid(b,at,p.filespace_uuid);Number(b,at+16,8,p.page_number);Number(b,at+24,8,p.page_generation);PutUuid(b,at+32,p.page_size_profile_uuid);};
  if(r.predecessor)ref(256,*r.predecessor);std::copy(r.predecessor_sha256.begin(),r.predecessor_sha256.end(),b.begin()+304);Number(b,400,8,r.completed?1:0);
  for(std::size_t i=0;i<r.roots.size();++i){const auto& root=r.roots[i];const auto at=512+112*i;Number(b,at,2,root.role);Number(b,at+4,4,root.page_type);ref(at+8,root.page);PutUuid(b,at+56,root.object_uuid);std::copy(root.sha256.begin(),root.sha256.end(),b.begin()+at+72);}
  CheckpointSeal(b);return b;
}
void CheckpointReject(const db::NativeCheckpointRootResult& r) {Check(!r.ok()&&!r.root&&r.bytes.empty(),"checkpoint refuses without partial root");}
void CanonicalCheckpoints() {
  using E=db::NativeCheckpointError;
  for(unsigned profile=0;profile<5;++profile) {
    auto r=CheckpointExample(profile);const auto expected=CheckpointOracle(r);const auto encoded=db::EncodeNativeCheckpointRoot(r);
    Check(encoded.ok()&&encoded.bytes==expected,"independent checkpoint image all profiles");
    const auto decoded=db::DecodeNativeCheckpointRoot(expected);Check(decoded.ok()&&decoded.root->completed&&decoded.root->roots.size()==10,"decode complete checkpoint image");
    r.completed=false;const auto incomplete=CheckpointOracle(r);const auto inspection=db::DecodeNativeCheckpointRoot(incomplete);
    Check(inspection.ok()&&!inspection.root->completed&&db::EncodeNativeCheckpointRoot(r).bytes==incomplete,"incomplete remains inspection data not completed root");
  }
  auto r=CheckpointExample();const auto good=CheckpointOracle(r);
  for(std::size_t at=0;at<good.size();++at){auto b=good;b[at]^=1;CheckpointReject(db::DecodeNativeCheckpointRoot(b));}
  for(std::size_t at:{128u,136u,138u,140u,400u,408u,511u,514u,616u,1632u}) {
    auto b=good;b[at]=255;CheckpointSeal(b,false);CheckpointReject(db::DecodeNativeCheckpointRoot(b));
  }
  for(std::size_t at:{144u,160u,168u,176u,208u,224u,240u}) {
    auto b=good;std::fill_n(b.begin()+at,(at==144||at==208||at==224)?16:8,0);CheckpointSeal(b);CheckpointReject(db::DecodeNativeCheckpointRoot(b));
  }
  for(std::size_t at:{184u,192u,200u,240u,248u}){auto b=good;Number(b,at,8,255);CheckpointSeal(b);CheckpointReject(db::DecodeNativeCheckpointRoot(b));}
  for(std::size_t at:{512u,516u,520u,536u,544u,552u,568u,584u}) {
    auto b=good;std::fill_n(b.begin()+at,at==584?32:at==520||at==552||at==568?16:at==536||at==544?8:2,0);CheckpointSeal(b);CheckpointReject(db::DecodeNativeCheckpointRoot(b));
  }
  auto changed=good;changed[336]^=1;CheckpointSeal(changed,false);CheckpointReject(db::DecodeNativeCheckpointRoot(changed));
  for(std::size_t size:{0u,127u,8191u,8193u}){auto b=good;b.resize(size);CheckpointReject(db::DecodeNativeCheckpointRoot(b));}
  auto alias=r;alias.roots[8]=alias.roots[4];alias.roots[8].role=9;
  Check(db::EncodeNativeCheckpointRoot(alias).ok(),"exact catalog/feature shared root admitted");
  for(unsigned field=0;field<4;++field){auto bad=alias;if(field==0)++bad.roots[8].page.page_generation;if(field==1)bad.roots[8].object_uuid=Id(199);if(field==2)bad.roots[8].sha256[0]^=1;if(field==3)bad.roots[8].page.page_size_profile_uuid=Profile(1);CheckpointReject(db::EncodeNativeCheckpointRoot(bad));CheckpointReject(db::DecodeNativeCheckpointRoot(CheckpointOracle(bad)));}
  auto successor=r;successor.checkpoint_generation=2;successor.header.page_number=20;successor.header.page_generation=110;
  successor.predecessor=disk::NativePageReference{r.header.filespace_uuid,r.header.page_number,r.header.page_generation,r.header.page_size_profile_uuid};
  Check(SHA256(good.data(),good.size(),successor.predecessor_sha256.data())!=nullptr,"actual predecessor full-image hash");
  Check(db::EncodeNativeCheckpointRoot(successor).bytes==CheckpointOracle(successor),"generation-linked checkpoint image");
  auto missing=successor;missing.predecessor.reset();CheckpointReject(db::EncodeNativeCheckpointRoot(missing));
  auto cluster=r;auto root=cluster.roots.back();root.role=14;root.page_type=777;root.page.page_number=49;root.object_uuid=Id(198);cluster.roots.push_back(root);cluster.flags=4;cluster.cluster_quorum_transaction_id=15;
  Check(db::EncodeNativeCheckpointRoot(cluster).ok()&&db::DecodeNativeCheckpointRoot(CheckpointOracle(cluster)).ok(),"cluster checkpoint framing not cluster authority");
  cluster.flags=0;CheckpointReject(db::EncodeNativeCheckpointRoot(cluster));
  for(unsigned mode=0;mode<2;++mode) {
    observed_allocations=0;count_allocations=true;auto measured=mode?db::DecodeNativeCheckpointRoot(good):db::EncodeNativeCheckpointRoot(r);count_allocations=false;
    const auto count=observed_allocations;Check(measured.ok(),"measure complete checkpoint allocations");bool success=false;
    for(unsigned long budget=0;budget<=count;++budget){allocation_budget=budget;const auto result=mode?db::DecodeNativeCheckpointRoot(good):db::EncodeNativeCheckpointRoot(r);allocation_budget=-1;if(result.ok()){success=true;break;}CheckpointReject(result);Check(result.error==E::resource_exhausted,"checkpoint allocation diagnostic");}
    Check(success,"every checkpoint allocation position");
    for(unsigned fault=1;fault<=4;++fault){hash_fault=fault;const auto result=mode?db::DecodeNativeCheckpointRoot(good):db::EncodeNativeCheckpointRoot(r);Check(hash_fault==0&&result.error==E::hash_failure,"checkpoint hashing failure");CheckpointReject(result);}
  }
}
void CanonicalCheckpointFiles() {
  using E=db::NativeCheckpointError;Fixture fixture;
  for(unsigned profile=0;profile<5;++profile) {
    auto r=CheckpointExample(profile);const auto image=CheckpointOracle(r);auto z=Example(profile);const auto zero=Oracle(z);
    disk::FileDevice device;const auto path=(fixture.root/("checkpoint-"+std::to_string(profile))).string();
    Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"own checkpoint filespace");const byte padding=0;
    Check(device.WriteAt(0,zero.data(),zero.size()).ok()&&device.WriteAt(z.total_pages*sizes[profile]-1,&padding,1).ok()
      &&device.WriteAt(19*sizes[profile],image.data(),image.size()).ok()&&device.Sync().ok(),"persist actual checkpoint image");
    auto read=[&]{return db::ReadNativeCheckpointRootFromOpenDevice(device,Id(1),z.roots[8]);};
    auto result=read();Check(result.ok()&&result.bytes==image&&result.root->completed,"actual pagezero-bound checkpoint read");
    for(unsigned field=0;field<5;++field){auto bad=z.roots[8];if(field==0)++bad.page_generation;if(field==1)bad.object_uuid=Id(200);if(field==2)bad.kind=2;if(field==3)bad.filespace_uuid=Id(200);if(field==4)bad.page_size_profile_uuid=Profile((profile+1)%5);CheckpointReject(db::ReadNativeCheckpointRootFromOpenDevice(device,Id(1),bad));}
    if(profile==0) {
      reads=0;track_reads=true;result=read();track_reads=false;const auto count=reads;Check(result.ok(),"measure complete checkpoint reads");
      for(unsigned fault=1;fault<=count;++fault){reads=0;read_fault=fault;track_reads=true;result=read();track_reads=false;Check(read_fault==0,"actual checkpoint read fault consumed");CheckpointReject(result);}
      auto incomplete=r;incomplete.completed=false;const auto bytes=CheckpointOracle(incomplete);
      Check(device.WriteAt(19*sizes[profile],bytes.data(),bytes.size()).ok()&&device.Sync().ok(),"persist incomplete checkpoint");
      result=read();Check(result.ok()&&!result.root->completed,"device reader cannot upgrade incomplete marker");
      auto encrypted=r;encrypted.header.flags=1;const auto opaque=CheckpointOracle(encrypted);
      Check(device.WriteAt(19*sizes[profile],opaque.data(),opaque.size()).ok()&&device.Sync().ok(),"persist raw-encrypted checkpoint fixture");
      result=read();CheckpointReject(result);Check(result.error==E::encrypted_requires_crypto_authority,"checkpoint needs crypto authority");
      Check(device.WriteAt(19*sizes[profile],image.data(),image.size()).ok()&&device.Sync().ok(),"restore isolated complete fixture");
      Exclusive(path);
    }
    Check(device.Close().ok()&&device.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"checkpoint readonly reopen");
    result=read();Check(result.ok()&&result.bytes==image,"checkpoint survives actual close and reopen");
  }
}
void CanonicalCheckpointInventoryPair() {
  using E=db::NativeCheckpointError;Fixture fixture;disk::FileDevice device;
  const auto path=(fixture.root/"checkpoint-inventory").string();const auto z=Example();const auto zero=Oracle(z);const byte padding=0;
  Check(device.Open(path,disk::FileOpenMode::create_new).ok()&&device.WriteAt(0,zero.data(),zero.size()).ok()
    &&device.WriteAt(z.total_pages*sizes[0]-1,&padding,1).ok(),"create canonical checkpoint/inventory node fixture");
  auto inventory=InventoryExample();inventory.inventory.next_local_transaction_id=18;inventory.inventory.next_commit_sequence=2;
  auto& entry=inventory.inventory.entries.front();entry.identity.local_id=mga::MakeLocalTransactionId(17);
  entry.identity.transaction_uuid.value=Id(98);entry.state=mga::TransactionState::committed;entry.commit_sequence=1;
  auto checkpoint=CheckpointExample();checkpoint.roots.front().page=InventoryRef(inventory);checkpoint.roots.front().object_uuid=inventory.object_uuid;
  const auto persist=[&](const auto& actual_inventory,auto actual_checkpoint,u64 horizon) {
    const auto image=InventoryOracle(actual_inventory,horizon,horizon,horizon);
    Check(SHA256(image.data(),image.size(),actual_checkpoint.roots.front().sha256.data())!=nullptr,"actual canonical inventory head digest");
    const auto root=CheckpointOracle(actual_checkpoint);
    Check(device.WriteAt(14*sizes[0],image.data(),image.size()).ok()&&device.WriteAt(19*sizes[0],root.data(),root.size()).ok()
      &&device.Sync().ok(),"persist actual checkpoint-to-inventory binding");
    return root;
  };
  const std::vector<disk::NativeFilespaceDevice> devices{{Id(2),Profile(0),&device}};
  const auto read=[&](u64 budget=16384){return db::VerifyNativeCheckpointInventoryFromOpenDevices(Id(1),devices,z.roots[8],budget);};
  const auto empty=[&](const auto& r){Check(!r.ok()&&!r.checkpoint&&r.inventory.entries.empty()&&!r.inventory.publication_base
    &&r.inventory_generation==0&&r.retained_image_bytes==0&&std::all_of(r.checkpoint_sha256.begin(),r.checkpoint_sha256.end(),[](byte b){return b==0;}),"failed checkpoint inventory pair returns no authority prefix");};
  persist(inventory,checkpoint,18);auto result=read();
  Check(result.ok()&&result.inventory.entries.size()==1&&result.inventory_generation==19&&result.retained_image_bytes==16384
    &&!result.inventory.publication_base,"actual checkpoint and complete inventory creator binding");
  result=read(16383);empty(result);Check(result.error==E::inventory_failure
    &&result.inventory_error==page::NativeInventoryError::resource_exhausted,"pair budget includes checkpoint and chain");
  for(auto state:{mga::TransactionState::active,mga::TransactionState::prepared,mga::TransactionState::limbo,
      mga::TransactionState::failed_terminal,mga::TransactionState::rolled_back}) {
    auto bad=inventory;bad.inventory.entries[0].state=state;bad.inventory.entries[0].commit_sequence=0;
    const auto horizon=state==mga::TransactionState::rolled_back?18:17;
    auto image=InventoryOracle(bad,horizon,state==mga::TransactionState::failed_terminal?18:horizon,state==mga::TransactionState::failed_terminal?18:horizon);
    auto root=checkpoint;Check(SHA256(image.data(),image.size(),root.roots.front().sha256.data())!=nullptr,"reseal actual noncommitted inventory");const auto bytes=CheckpointOracle(root);
    Check(device.WriteAt(14*sizes[0],image.data(),image.size()).ok()&&device.WriteAt(19*sizes[0],bytes.data(),bytes.size()).ok()&&device.Sync().ok(),"persist noncommitted creator case");
    result=read();empty(result);Check(result.error==E::creator_not_committed,"noncommitted creator cannot certify checkpoint inventory");
  }
  for(auto origin:{mga::TransactionState::committed,mga::TransactionState::rolled_back,mga::TransactionState::failed_terminal}) {
    auto archived=inventory;auto& e=archived.inventory.entries[0];e.state=mga::TransactionState::archived;e.archived_from_state=origin;
    if(origin!=mga::TransactionState::committed)e.commit_sequence=0;
    persist(archived,checkpoint,18);result=read();
    if(origin==mga::TransactionState::committed)Check(result.ok()&&result.inventory.entries[0].archived_from_state==origin,"exact archived committed creator admitted");
    else{empty(result);Check(result.error==E::creator_not_committed,"archive location is not checkpoint commit authority");}
  }
  auto mismatch=checkpoint;mismatch.creator_transaction_uuid=Id(199);persist(inventory,mismatch,18);result=read();empty(result);Check(result.error==E::inventory_mismatch,"binary checkpoint creator UUID mismatch");
  auto global=inventory;global.inventory.entries[0].identity.scope=mga::TransactionScope::cluster_global;
  persist(global,checkpoint,18);result=read();empty(result);Check(result.error==E::inventory_mismatch,"global transaction cannot certify standalone checkpoint");
  mismatch=checkpoint;mismatch.selected_local_transaction_id=18;persist(inventory,mismatch,18);result=read();empty(result);Check(result.error==E::inventory_mismatch,"selected transaction boundary must bind inventory next counter");
  mismatch=checkpoint;mismatch.completed=false;persist(inventory,mismatch,18);result=read();empty(result);Check(result.error==E::incomplete,"incomplete checkpoint cannot certify inventory pair");
  const auto root_bytes=persist(inventory,checkpoint,18);auto bad_hash=root_bytes;bad_hash[512+72]^=1;CheckpointSeal(bad_hash);
  Check(device.WriteAt(19*sizes[0],bad_hash.data(),bad_hash.size()).ok()&&device.Sync().ok(),"persist wrong expected inventory hash with valid checkpoint seals");
  result=read();empty(result);Check(result.error==E::invalid_integrity,"checkpoint expected hash binds actual inventory bytes");
  persist(inventory,checkpoint,18);
  reads=0;track_reads=true;result=read();track_reads=false;const auto read_count=reads;Check(result.ok(),"measure complete bound pair reads");
  for(unsigned fault=1;fault<=read_count;++fault){reads=0;read_fault=fault;track_reads=true;result=read();track_reads=false;Check(read_fault==0,"pair actual read fault consumed");empty(result);}
  observed_allocations=0;count_allocations=true;result=read();count_allocations=false;const auto allocation_count=observed_allocations;Check(result.ok(),"measure bound pair allocations");bool success=false;
  for(unsigned long budget=0;budget<=allocation_count;++budget){allocation_budget=budget;result=read();allocation_budget=-1;if(result.ok()){success=true;break;}empty(result);Check(result.error==E::resource_exhausted||(result.error==E::inventory_failure&&result.inventory_error==page::NativeInventoryError::resource_exhausted),"pair allocation refusal classified");}
  Check(success,"every bound pair allocation position");
  Exclusive(path);Check(device.Close().ok(),"close complete pair before fresh process");
  const auto child=::fork();Check(child>=0,"fork independent checkpoint inventory reader");
  if(child==0){::execl("/proc/self/exe","checkpoint-inventory-probe","--checkpoint-inventory-probe",path.c_str(),nullptr);::_exit(125);}
  int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable verifies persisted checkpoint inventory pair");
}
disk::FilespaceRootReference CheckpointRef(const db::NativeCheckpointRoot& r) {
  return {9,0x300,r.header.filespace_uuid,r.header.page_number,r.header.page_generation,r.header.page_size_profile_uuid,r.object_uuid};
}
void CanonicalCheckpointHistory() {
  using E=db::NativeCheckpointError;Fixture fixture;disk::FileDevice first,second;
  const auto path1=(fixture.root/"history-first").string(),path2=(fixture.root/"history-second").string();
  auto z1=Example(),z2=Example(1);z2.bootstrap.filespace_uuid=Id(7);for(auto& root:z2.roots)root.filespace_uuid=Id(7);
  Check(first.Open(path1,disk::FileOpenMode::create_new).ok()&&second.Open(path2,disk::FileOpenMode::create_new).ok(),"own checkpoint history filespaces");
  const auto prepare=[&](disk::FileDevice& d,const auto& z){const auto bytes=Oracle(z);const byte padding=0;Check(d.WriteAt(0,bytes.data(),bytes.size()).ok()&&d.WriteAt(z.total_pages*z.bootstrap.page_size_bytes-1,&padding,1).ok(),"prepare actual history filespace");};
  prepare(first,z1);prepare(second,z2);
  std::array<db::NativeCheckpointRoot,3> roots{CheckpointExample(),CheckpointExample(1),CheckpointExample()};
  std::array<page::NativeTransactionInventoryPage,3> inventories{InventoryExample(),InventoryExample(1),InventoryExample()};
  for(unsigned i=0;i<3;++i) {
    auto& r=roots[i];auto& inv=inventories[i];
    // Unchanged roots still belong to the first filespace/profile. Only this
    // checkpoint and its new inventory head move to the second filespace.
    if(i)r.roots=roots[0].roots;
    if(i==1){r.header.filespace_uuid=Id(7);inv.header.filespace_uuid=Id(7);}
    r.header.page_number=i==0?19:i==1?23:24;r.header.page_generation=109+i;r.header.page_uuid=Id(static_cast<byte>(150+i));
    r.checkpoint_generation=i+1;r.root_set_generation=8+i;r.selected_local_transaction_id=17+i;
    r.stable_local_transaction_id=12+i;r.local_durable_transaction_id=16+i;
    r.creator_local_transaction_id=17+i;r.creator_transaction_uuid=Id(static_cast<byte>(98+i));
    inv.header.page_number=i==2?15:14;inv.header.page_generation=104+i;inv.header.page_uuid=Id(static_cast<byte>(160+i));
    inv.inventory_generation=19+i;inv.inventory.next_local_transaction_id=18+i;inv.inventory.next_commit_sequence=2+i;inv.inventory.entries.clear();
    for(unsigned n=0;n<=i;++n){mga::TransactionInventoryEntry e;e.identity.local_id=mga::MakeLocalTransactionId(17+n);
      e.identity.transaction_uuid={scratchbird::core::platform::UuidKind::transaction,Id(static_cast<byte>(98+n))};e.identity.scope=mga::TransactionScope::local_node;e.state=mga::TransactionState::committed;e.commit_sequence=n+1;inv.inventory.entries.push_back(e);}
    r.roots.front().page=InventoryRef(inv);r.roots.front().object_uuid=inv.object_uuid;
  }
  const auto persist=[&](auto values,const auto& invs){
    std::array<Bytes,3> images;
    for(unsigned i=0;i<3;++i){auto& r=values[i];const auto& inv=invs[i];const auto boundary=inv.inventory.next_local_transaction_id;
      const auto bytes=InventoryOracle(inv,boundary,boundary,boundary);
      Check(SHA256(bytes.data(),bytes.size(),r.roots.front().sha256.data())!=nullptr,"actual history inventory head hash");
      if(i){const auto& previous=values[i-1];r.predecessor=disk::NativePageReference{previous.header.filespace_uuid,previous.header.page_number,previous.header.page_generation,previous.header.page_size_profile_uuid};Check(SHA256(images[i-1].data(),images[i-1].size(),r.predecessor_sha256.data())!=nullptr,"actual retained predecessor hash");}
      images[i]=CheckpointOracle(r);auto& device=i==1?second:first;
      Check(device.WriteAt(inv.header.page_number*inv.header.page_size_bytes,bytes.data(),bytes.size()).ok()
        &&device.WriteAt(r.header.page_number*r.header.page_size_bytes,images[i].data(),images[i].size()).ok()&&device.Sync().ok(),"persist actual checkpoint history");
    }
    return images;
  };
  auto images=persist(roots,inventories);
  const std::vector<disk::NativeFilespaceDevice> devices{{Id(7),Profile(1),&second},{Id(2),Profile(0),&first}};
  const auto head=CheckpointRef(roots[2]),terminal=CheckpointRef(roots[0]);
  const auto read=[&](u64 budget=65536){return db::VerifyNativeCheckpointHistoryFromOpenDevices(Id(1),devices,head,terminal,budget);};
  const auto empty=[&](const auto& r){Check(!r.ok()&&r.checkpoints.empty()&&r.retained_image_bytes==0,"history failure exposes no verified prefix");};
  auto result=read();Check(result.ok()&&result.checkpoints.size()==3&&result.retained_image_bytes==65536,"complete mixed-filespace checkpoint history error="+std::to_string(static_cast<unsigned>(result.error)));
  for(unsigned i=0;i<3;++i){std::array<byte,32> digest{};Check(SHA256(images[2-i].data(),images[2-i].size(),digest.data())!=nullptr,"independent actual checkpoint history digest");Check(result.checkpoints[i].checkpoint_sha256==digest&&!result.checkpoints[i].inventory.publication_base,"history digests are actual complete bytes without CAS authority");}
  result=read(65535);empty(result);
  auto one=db::VerifyNativeCheckpointHistoryFromOpenDevices(Id(1),devices,head,head,16384);Check(one.ok()&&one.checkpoints.size()==1,"exact singleton retained history");
  auto bounded=db::VerifyNativeCheckpointHistoryFromOpenDevices(Id(1),devices,head,CheckpointRef(roots[1]),49152);Check(bounded.ok()&&bounded.checkpoints.size()==2,"exact non-genesis retained terminal");
  // New checkpoint images may reuse one unchanged immutable inventory and
  // root set; publication generations are not checkpoint/page generations.
  std::array<Bytes,3> repeated_images;std::array<db::NativeCheckpointRoot,3> repeated_roots;
  const auto first_inventory=InventoryOracle(inventories[0],18,18,18);
  for(unsigned i=0;i<3;++i){auto repeated=roots[0];repeated.header=roots[i].header;repeated.checkpoint_generation=i+1;
    Check(SHA256(first_inventory.data(),first_inventory.size(),repeated.roots.front().sha256.data())!=nullptr,"same immutable inventory head digest");
    if(i){const auto& previous=repeated_roots[i-1];repeated.predecessor=disk::NativePageReference{previous.header.filespace_uuid,previous.header.page_number,previous.header.page_generation,previous.header.page_size_profile_uuid};Check(SHA256(repeated_images[i-1].data(),repeated_images[i-1].size(),repeated.predecessor_sha256.data())!=nullptr,"reused-root predecessor digest");}
    repeated_images[i]=CheckpointOracle(repeated);repeated_roots[i]=repeated;auto& device=i==1?second:first;
    Check(device.WriteAt(repeated.header.page_number*repeated.header.page_size_bytes,repeated_images[i].data(),repeated_images[i].size()).ok()&&device.Sync().ok(),"persist checkpoint retaining unchanged roots");
  }
  result=read();Check(result.ok()&&result.retained_image_bytes==57344&&result.checkpoints[0].inventory_generation==19
    &&result.checkpoints[0].checkpoint->root_set_generation==8,"unchanged immutable roots may retain publication generations");
  images=persist(roots,inventories);
  auto absent=terminal;++absent.page_generation;empty(db::VerifyNativeCheckpointHistoryFromOpenDevices(Id(1),devices,head,absent,65536));
  for(unsigned field=0;field<9;++field){auto bad_roots=roots;auto bad_inventories=inventories;
    if(field==0)bad_roots[1].timeline_uuid=Id(199);
    if(field==1)bad_roots[1].checkpoint_generation=4;
    if(field==2)bad_roots[1].root_set_generation=11;
    if(field==3)bad_roots[1].root_set_generation=10;
    if(field==4)bad_roots[1].stable_local_transaction_id=15;
    if(field==5)bad_inventories[1].inventory_generation=22;
    if(field==6)bad_inventories[1].inventory_generation=21;
    if(field==7)bad_inventories[1].inventory.next_commit_sequence=5;
    if(field==8)bad_roots[1].header.page_uuid=bad_roots[0].header.page_uuid;
    persist(bad_roots,bad_inventories);result=read();empty(result);Check(result.error==E::history_mismatch,"resealed history semantic drift refused");
  }
  for(unsigned field=0;field<9;++field){auto changed=inventories;auto& entry=changed[2].inventory.entries[0];
    if(field==0)entry.identity.transaction_uuid.value=Id(201);
    if(field==1)entry.identity.scope=mga::TransactionScope::cluster_global;
    if(field==2)++entry.begin_unix_epoch_millis;
    if(field==3)++entry.begin_visible_through_local_transaction_id;
    if(field==4)++entry.final_unix_epoch_millis;
    if(field==5){entry.state=mga::TransactionState::rolled_back;entry.commit_sequence=0;}
    if(field==6)entry.evidence_record_required=false;
    if(field==7){entry.commit_sequence=changed[2].inventory.next_commit_sequence++;}
    if(field==8){entry.begin_visible_through_commit_sequence=1;entry.commit_sequence=changed[2].inventory.next_commit_sequence++;}
    persist(roots,changed);
    // Each complete, independently resealed checkpoint/inventory pair is valid;
    // only the actual predecessor relation exposes these inconsistent facts.
    auto pair=db::VerifyNativeCheckpointInventoryFromOpenDevices(Id(1),devices,head,16384);
    Check(pair.ok(),"mutated history head remains independently valid field="+std::to_string(field));
    result=read();empty(result);Check(result.error==E::inventory_mismatch,"resealed immutable inventory history refused field="+std::to_string(field));
  }
  auto incomplete=roots;incomplete[0].completed=false;persist(incomplete,inventories);result=read();empty(result);Check(result.error==E::incomplete,"late incomplete predecessor discards newer prefix");
  images=persist(roots,inventories);auto wrong=images[2];wrong[304]^=1;CheckpointSeal(wrong);
  Check(first.WriteAt(24*sizes[0],wrong.data(),wrong.size()).ok()&&first.Sync().ok(),"persist wrong predecessor digest with valid image seals");
  result=read();empty(result);Check(result.error==E::invalid_integrity,"history compares actual predecessor bytes");
  images=persist(roots,inventories);
  reads=0;track_reads=true;result=read();track_reads=false;const auto count=reads;Check(result.ok(),"measure complete history reads");
  for(unsigned fault=1;fault<=count;++fault){reads=0;read_fault=fault;track_reads=true;result=read();track_reads=false;Check(read_fault==0,"history read fault consumed");empty(result);}
  observed_allocations=0;count_allocations=true;result=read();count_allocations=false;const auto allocations=observed_allocations;Check(result.ok(),"measure complete history allocations");bool success=false;
  for(unsigned long budget=0;budget<=allocations;++budget){allocation_budget=budget;result=read();allocation_budget=-1;if(result.ok()){success=true;break;}empty(result);Check(result.error==E::resource_exhausted||(result.error==E::inventory_failure&&result.inventory_error==page::NativeInventoryError::resource_exhausted),"history allocation failure diagnostic");}
  Check(success,"all history allocation positions through completion");
  std::atomic<unsigned> completed=0;auto reverse=devices;std::reverse(reverse.begin(),reverse.end());
  const auto concurrent=[&](const auto& owners){for(unsigned i=0;i<8;++i){const auto r=db::VerifyNativeCheckpointHistoryFromOpenDevices(Id(1),owners,head,terminal,65536);if(r.ok()&&r.checkpoints.size()==3)++completed;}};
  std::thread a([&]{concurrent(devices);}),b([&]{concurrent(reverse);});a.join();b.join();Check(completed==16,"history guards are ordered across nested inventory reads");
  Exclusive(path1);Exclusive(path2);Check(first.Close().ok()&&second.Close().ok(),"release history before fresh executable");
  const auto child=::fork();Check(child>=0,"fork native history reader");
  if(child==0){::execl("/proc/self/exe","checkpoint-history-probe","--checkpoint-history-probe",fixture.root.c_str(),nullptr);::_exit(125);}
  int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable verifies complete retained history");
}
int main(int argc,char** argv) {
  if(argc==3&&std::string_view(argv[1])=="--checkpoint-history-probe") {
    const std::filesystem::path path=argv[2];disk::FileDevice first,second;
    if(!first.Open((path/"history-first").string(),disk::FileOpenMode::open_existing_read_only).ok()||!second.Open((path/"history-second").string(),disk::FileOpenMode::open_existing_read_only).ok())return 2;
    auto initial=CheckpointExample(),head=initial;head.header.page_number=24;head.header.page_generation=111;
    const auto r=db::VerifyNativeCheckpointHistoryFromOpenDevices(Id(1),{{Id(2),Profile(0),&first},{Id(7),Profile(1),&second}},CheckpointRef(head),CheckpointRef(initial),65536);
    return r.ok()&&r.checkpoints.size()==3&&r.checkpoints.front().inventory.next_commit_sequence==4&&r.checkpoints.back().inventory.next_commit_sequence==2?0:3;
  }
  if(argc==3&&std::string_view(argv[1])=="--checkpoint-inventory-probe") {
    disk::FileDevice device;if(!device.Open(argv[2],disk::FileOpenMode::open_existing_read_only).ok())return 2;
    const auto r=db::VerifyNativeCheckpointInventoryFromOpenDevices(Id(1),{{Id(2),Profile(0),&device}},Example().roots[8],16384);
    return r.ok()&&r.inventory_generation==19&&r.inventory.entries.size()==1
      &&r.inventory.entries[0].identity.transaction_uuid.value==Id(98)&&r.inventory.entries[0].state==mga::TransactionState::committed?0:3;
  }
  if(argc==3&&std::string_view(argv[1])=="--inventory-chain-probe") {
    disk::FileDevice first,second;const std::filesystem::path root=argv[2];
    if(!first.Open((root/"inventory-primary").string(),disk::FileOpenMode::open_existing_read_only).ok()
      ||!second.Open((root/"inventory-secondary-primary").string(),disk::FileOpenMode::open_existing_read_only).ok())return 2;
    const auto r=page::ReadNativeTransactionInventoryChainFromOpenDevices(Id(1),{{Id(2),Profile(0),&first},{Id(7),Profile(1),&second}},Example().roots[3],24576);
    return r.ok()&&r.pages.size()==2&&r.inventory.entries.size()==2&&!r.inventory.publication_base
      &&r.inventory.entries[0].state==mga::TransactionState::active&&r.inventory.entries[1].state==mga::TransactionState::rolled_back?0:3;
  }
  if(argc==4&&std::string_view(argv[1])=="--leaf-probe") {
    if(std::string_view(argv[3]).size()!=1||argv[3][0]<'0'||argv[3][0]>'4') return 2;
    const auto p=static_cast<unsigned>(argv[3][0]-'0');disk::FileDevice d;
    if(!d.Open(argv[2],disk::FileOpenMode::open_existing_read_only).ok()) return 3;
    const auto r=db::ReadNativeCatalogLeafFromOpenDevice(d,Id(1),RootExample(p).roots[0]);
    return r.ok()&&r.metadata.size()==2&&r.bytes==LeafOracle(LeafExample(p))?0:4;
  }
  if(argc==4&&std::string_view(argv[1])=="--range-probe") {
    disk::FileDevice first,second;
    if(!first.Open(argv[2],disk::FileOpenMode::open_existing_read_only).ok()
        ||!second.Open(argv[3],disk::FileOpenMode::open_existing_read_only).ok()) return 3;
    const std::vector<page::NativeCatalogFilespaceDevice> devices{{Id(7),Profile(1),&second},{Id(2),Profile(0),&first}};
    auto initial=RootExample(),head=RootExample(); head.header.page_number=35; head.header.page_generation=302;
    const auto r=page::ReadNativeCatalogRootRangeFromOpenDevices(Id(1),devices,RootRef(head),RootRef(initial),32768);
    return r.ok()&&r.roots.size()==3&&r.retained_image_bytes==32768
      &&r.roots[0].root->catalog_generation==3&&r.roots[1].root->catalog_generation==2
      &&r.roots[2].root->catalog_generation==1?0:4;
  }
  if(argc==3&&std::string_view(argv[1])=="--probe") { disk::FileDevice d; return Locked(d.Open(argv[2],disk::FileOpenMode::open_existing))?0:1; }
  if(argc==4&&std::string_view(argv[1])=="--catalog-probe") {
    if(std::string_view(argv[3]).size()!=1||argv[3][0]<'0'||argv[3][0]>'4') return 2;
    const auto p=static_cast<unsigned>(argv[3][0]-'0'); disk::FileDevice d;
    if(!d.Open(argv[2],disk::FileOpenMode::open_existing_read_only).ok()) return 3;
    const auto r=page::ReadNativeCatalogRootFromOpenDevice(d,Id(1),Example(p).roots[1]);
    return r.ok()&&r.bytes==RootOracle(RootExample(p))?0:4;
  }
  try { CanonicalCheckpointHistory(); CanonicalCheckpoints(); CanonicalCheckpointFiles(); CanonicalCheckpointInventoryPair(); CanonicalInventoryImages(); CanonicalInventoryChains(); Codecs(); Files(); CatalogRoots(); CatalogRootFiles(); CatalogRootRanges(); CatalogLeaves(); CatalogLeafFiles();
    std::cout<<"PASS checks="<<checks<<" canonical_page_image_and_chain_only=true\n"; return 0; }
  catch(const std::exception& e) { allocation_budget=-1; std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n'; return 1; }
}

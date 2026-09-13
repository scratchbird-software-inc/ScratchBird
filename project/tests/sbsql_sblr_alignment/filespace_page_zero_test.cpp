// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "filespace_page_zero.hpp"
#include "catalog_page.hpp"
#include "native_index_btree_page.hpp"
#include "physical_mga_cow_store.hpp"
#include "catalog_schema_definition.hpp"
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
std::atomic<bool> pause_next_tree_read{false},tree_read_paused{false},resume_tree_read{false};
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
  if(pause_next_tree_read.exchange(false)) {tree_read_paused=true;while(!resume_tree_read.load())std::this_thread::yield();}
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
page::NativeBtreePage BtreeExample(unsigned p=0,unsigned type=0x200,bool branch=false) {
  page::NativeBtreePage r;r.header={sizes[p],type,Id(1),Id(2),Id(90),12,102,0,Profile(p)};
  r.dependencies={Id(42),3,8,Id(43),Id(44),Id(45),{}};r.dependencies.dependency_map_sha256.fill(19);
  r.creator_transaction_uuid=Id(98);r.creator_local_transaction_id=17;r.maintenance_state=1;r.tree_level=branch?1:0;
  const auto ref=[&](u64 n){return disk::NativePageReference{Id(2),n,7,Profile(p)};};
  if(type!=0x200){r.parent=ref(9);r.left=ref(10);r.right=ref(11);
    r.low_fence=page::NativeBtreeKey{{0},Id(60),Id(61)};r.high_fence=page::NativeBtreeKey{{255,1},Id(60),Id(61)};}
  if(branch)r.first_child=ref(20);
  for(unsigned i=0;i<3;++i){page::NativeBtreeCell c;c.key={{static_cast<byte>(i==2?255:0)},Id(static_cast<byte>(70+i)),Id(static_cast<byte>(80+i))};
    if(i==1)c.key.encoded_key.push_back(0);if(branch)c.child=ref(21+i);
    else{c.deleted=i==0;c.base_page=disk::NativePageReference{Id(7),50+i,4,Profile((p+1)%5)};}r.cells.push_back(c);}
  return r;
}
void BtreeSeal(Bytes& b) {std::fill(b.begin()+512,b.begin()+544,0);std::array<byte,32> digest{};
  Check(SHA256(b.data(),b.size(),digest.data())!=nullptr,"independent full native B-tree hash");std::copy(digest.begin(),digest.end(),b.begin()+512);}
Bytes BtreeOracle(const page::NativeBtreePage& r) {
  auto common=RootExample();common.header=r.header;auto b=RootOracle(common);std::fill(b.begin()+128,b.end(),0);
  const std::string_view magic="SBBTP001";std::copy(magic.begin(),magic.end(),b.begin()+128);Number(b,136,2,1);Number(b,138,2,512);
  const auto& d=r.dependencies;PutUuid(b,144,d.index_uuid);Number(b,160,8,d.descriptor_generation);Number(b,168,8,d.storage_generation);
  PutUuid(b,176,d.key_profile_uuid);PutUuid(b,192,d.visibility_profile_uuid);PutUuid(b,208,d.dependency_map_uuid);
  std::copy(d.dependency_map_sha256.begin(),d.dependency_map_sha256.end(),b.begin()+224);PutUuid(b,256,r.creator_transaction_uuid);
  Number(b,272,8,r.creator_local_transaction_id);Number(b,280,2,r.maintenance_state);Number(b,282,2,r.tree_level);
  const auto ref=[&](std::size_t at,const auto& p){if(!p)return;PutUuid(b,at,p->filespace_uuid);Number(b,at+16,8,p->page_number);Number(b,at+24,8,p->page_generation);PutUuid(b,at+32,p->page_size_profile_uuid);};
  ref(288,r.parent);ref(336,r.left);ref(384,r.right);ref(432,r.first_child);
  Number(b,496,4,r.cells.size());Number(b,500,4,std::count_if(r.cells.begin(),r.cells.end(),[](const auto& c){return c.deleted;}));Number(b,504,4,640);
  std::size_t at=640+4*r.cells.size();const auto fence=[&](const auto& k,std::size_t slot){if(!k)return;Number(b,slot,4,at);Number(b,slot+4,4,40+k->encoded_key.size());
    Number(b,at,4,k->encoded_key.size());PutUuid(b,at+8,k->row_uuid);PutUuid(b,at+24,k->version_uuid);std::copy(k->encoded_key.begin(),k->encoded_key.end(),b.begin()+at+40);at+=40+k->encoded_key.size();};
  fence(r.low_fence,480);fence(r.high_fence,488);
  for(unsigned i=0;i<r.cells.size();++i){const auto& c=r.cells[i];Number(b,640+4*i,4,at);Number(b,at,4,144+c.key.encoded_key.size());Number(b,at+4,4,c.key.encoded_key.size());Number(b,at+8,4,c.deleted?1:0);
    PutUuid(b,at+16,c.key.row_uuid);PutUuid(b,at+32,c.key.version_uuid);ref(at+48,c.child);ref(at+96,c.base_page);std::copy(c.key.encoded_key.begin(),c.key.encoded_key.end(),b.begin()+at+144);at+=144+c.key.encoded_key.size();}
  Number(b,140,4,at);BtreeSeal(b);return b;
}
void NativeBtreePages() {
  using E=page::NativeBtreeError;
  const auto empty=[&](const auto& r){Check(!r.ok()&&!r.page&&r.bytes.empty(),"native B-tree refusal returns no image or authority prefix");};
  for(unsigned p=0;p<5;++p)for(unsigned variant=0;variant<4;++variant){const bool branch=variant%2;
    const auto r=BtreeExample(p,variant<2?0x200:variant==2?0x202:0x201,branch);
    const auto bytes=BtreeOracle(r),encoded=page::EncodeNativeBtreePage(r).bytes;
    Check(bytes==encoded,"all profiles and roles independently packed B-tree image");const auto decoded=page::DecodeNativeBtreePage(bytes);
    Check(decoded.ok()&&BtreeOracle(*decoded.page)==bytes,"decode independent B-tree frame without prototype body");}
  const auto good=BtreeExample();const auto bytes=BtreeOracle(good);
  for(std::size_t at=0;at<bytes.size();++at){auto bad=bytes;bad[at]^=1;empty(page::DecodeNativeBtreePage(bad));}
  for(unsigned which=0;which<22;++which){auto r=BtreeExample(0,0x201,true);
    if(which==0)r.dependencies.index_uuid={};if(which==1)r.dependencies.descriptor_generation=0;if(which==2)r.dependencies.storage_generation=0;
    if(which==3)r.dependencies.key_profile_uuid={};if(which==4)r.dependencies.visibility_profile_uuid={};if(which==5)r.dependencies.dependency_map_uuid={};
    if(which==6)r.dependencies.dependency_map_sha256.fill(0);if(which==7)r.creator_transaction_uuid={};if(which==8)r.creator_local_transaction_id=0;
    if(which==9)r.maintenance_state=10;if(which==10)r.tree_level=0;if(which==11)r.parent.reset();if(which==12)r.low_fence.reset();
    if(which==13)r.first_child.reset();if(which==14)r.cells[1].child=r.first_child;if(which==15)r.cells[1].child->page_size_profile_uuid=Profile(1);
    if(which==16)r.cells[1].child->page_number=r.header.page_number;if(which==17)r.cells[1].key.row_uuid={};if(which==18)r.cells[1].key=r.cells[0].key;
    if(which==19)r.high_fence=r.cells.back().key;if(which==20)r.cells[1].deleted=true;if(which==21)r.cells[1].base_page=r.parent;
    empty(page::EncodeNativeBtreePage(r));empty(page::DecodeNativeBtreePage(BtreeOracle(r)));}
  for(auto offset:{128u,136u,138u,140u,284u,504u,508u,544u,639u,640u,644u,656u,660u,664u,8191u}){
    auto bad=bytes;bad[offset]^=1;BtreeSeal(bad);empty(page::DecodeNativeBtreePage(bad));}
  for(auto size:{0u,127u,639u,8191u,8193u}){auto bad=bytes;bad.resize(size);empty(page::DecodeNativeBtreePage(bad));}
  {auto r=good;r.cells.clear();Check(page::EncodeNativeBtreePage(r).ok()&&page::DecodeNativeBtreePage(BtreeOracle(r)).ok(),"empty allocated root leaf is representable");}
  {auto r=good;r.cells.resize(1);r.cells[0].key.encoded_key.resize(sizes[0],0);const auto e=page::EncodeNativeBtreePage(r);empty(e);Check(e.error==E::resource_exhausted,"oversized inline key cannot be truncated");}
  // Tie-breaking is binary, including bytes after the first machine word.
  {auto a=good.cells[0].key,b=a;b.row_uuid.bytes[15]^=1;Check(page::CompareNativeBtreeKeys(a,b)!=0,"full row UUID participates in tuple order");
    b=a;b.version_uuid.bytes[15]^=1;Check(page::CompareNativeBtreeKeys(a,b)!=0,"full native version UUID participates in tuple order");}
  for(unsigned mode=0;mode<2;++mode)for(unsigned fault=1;fault<=5;++fault){hash_fault=fault;
    const auto result=mode?page::DecodeNativeBtreePage(bytes):page::EncodeNativeBtreePage(good);
    Check(hash_fault==0,"B-tree family digest backend fault consumed");empty(result);Check(result.error==E::hash_failure,"B-tree family digest error preserved");}
  observed_allocations=0;count_allocations=true;auto encoded=page::EncodeNativeBtreePage(good);count_allocations=false;
  const auto allocation_count=observed_allocations;Check(encoded.ok(),"measure B-tree encoder allocations");bool encoder_success=false;
  for(unsigned long budget=0;budget<=allocation_count;++budget){allocation_budget=budget;encoded=page::EncodeNativeBtreePage(good);allocation_budget=-1;
    if(encoded.ok()){encoder_success=true;break;}empty(encoded);Check(encoded.error==E::resource_exhausted,"B-tree encoder allocation refusal classified");}
  Check(encoder_success,"every B-tree encoder allocation fault position");
  for(unsigned p=0;p<5;++p){Fixture fixture;disk::FileDevice device;const auto path=(fixture.root/"native-btree").string();const auto r=BtreeExample(p);
    const auto z=Example(p,p%2?5:6);const auto zero=Oracle(z),image=BtreeOracle(r);const byte padding=0;
    Check(device.Open(path,disk::FileOpenMode::create_new).ok()&&device.WriteAt(0,zero.data(),zero.size()).ok()
      &&device.WriteAt(z.total_pages*sizes[p]-1,&padding,1).ok()&&device.WriteAt(12*sizes[p],image.data(),image.size()).ok()&&device.Sync().ok(),"persist native B-tree in actual index/data filespace");
    const disk::NativePageReference ref{Id(2),12,102,Profile(p)};
    const auto read=[&]{return page::ReadNativeBtreePageFromOpenDevice(device,Id(1),ref,0x200,r.dependencies);};
    auto result=read();Check(result.ok()&&result.bytes==image,"retained-device exact B-tree image/dependency binding");
    for(unsigned which=0;which<7;++which){auto d=r.dependencies;if(which==0)d.index_uuid=Id(200);if(which==1)++d.descriptor_generation;if(which==2)++d.storage_generation;
      if(which==3)d.key_profile_uuid=Id(200);if(which==4)d.visibility_profile_uuid=Id(200);if(which==5)d.dependency_map_uuid=Id(200);if(which==6)d.dependency_map_sha256[31]^=1;
      result=page::ReadNativeBtreePageFromOpenDevice(device,Id(1),ref,0x200,d);empty(result);Check(result.error==E::binding_mismatch,"every exact descriptor dependency binding enforced");}
    auto wrong=ref;++wrong.page_generation;result=page::ReadNativeBtreePageFromOpenDevice(device,Id(1),wrong,0x200,r.dependencies);empty(result);Check(result.error==E::binding_mismatch,"exact page generation required");
    result=page::ReadNativeBtreePageFromOpenDevice(device,Id(1),ref,0x202,r.dependencies);empty(result);Check(result.error==E::binding_mismatch,"exact page role required");
    for(u64 flags:{1u,2u,4u,8u}){auto flagged=r;flagged.header.flags=flags;const auto flag_image=BtreeOracle(flagged);
      if(flags!=1)Check(page::EncodeNativeBtreePage(flagged).ok()&&page::DecodeNativeBtreePage(flag_image).ok(),"non-encrypted flag image inspection is not serving authority");
      Check(device.WriteAt(12*sizes[p],flag_image.data(),flag_image.size()).ok(),"persist actual flagged B-tree image");
      result=read();empty(result);Check(result.error==(flags==1?E::encrypted_requires_crypto_authority:flags==2?E::cluster_requires_authority:E::header_policy_requires_authority),"truthful crypto cluster or header policy requirement");}
    {auto foreign=r;foreign.header.database_uuid=Id(201);foreign.header.flags=2;const auto foreign_image=BtreeOracle(foreign);
      Check(device.WriteAt(12*sizes[p],foreign_image.data(),foreign_image.size()).ok(),"persist foreign cluster-flagged image");
      result=read();empty(result);Check(result.error==E::binding_mismatch,"exact node identity precedes cluster policy classification");}
    Check(device.WriteAt(12*sizes[p],image.data(),image.size()).ok(),"restore ordinary B-tree image");
    for(unsigned fault=1;fault<=5;++fault){hash_fault=fault;result=read();Check(hash_fault==0,"native B-tree hash provider fault consumed");empty(result);}
    reads=0;track_reads=true;result=read();track_reads=false;const auto count=reads;Check(result.ok(),"measure B-tree actual reads");
    for(unsigned fault=1;fault<=count;++fault){reads=0;read_fault=fault;track_reads=true;result=read();track_reads=false;Check(read_fault==0,"native B-tree read fault consumed");empty(result);}
    observed_allocations=0;count_allocations=true;result=read();count_allocations=false;const auto allocations=observed_allocations;Check(result.ok(),"measure B-tree reader allocations");
    bool success=false;for(unsigned long budget=0;budget<=allocations;++budget){allocation_budget=budget;result=read();allocation_budget=-1;
      if(result.ok()){success=true;break;}empty(result);Check(result.error==E::resource_exhausted,"native B-tree allocation refusal classified");}Check(success,"every B-tree reader allocation fault position");
    for(unsigned role:{1u,10u,14u}){const auto bad_zero=Oracle(Example(p,role));
      Check(disk::DecodeFilespacePageZero(bad_zero.data(),bad_zero.size()).ok(),"nonserving role has valid canonical page zero");
      Check(device.WriteAt(0,bad_zero.data(),bad_zero.size()).ok(),"write nonserving filespace role");result=read();empty(result);Check(result.error==E::invalid_filespace,"primary archive and import candidates not B-tree serving filespaces");}
    Check(device.WriteAt(0,zero.data(),zero.size()).ok()&&device.Sync().ok(),"restore owned B-tree filespace");Exclusive(path);
    Check(device.Close().ok(),"close B-tree before independent reopen");const auto child=::fork();Check(child>=0,"fork B-tree persisted reader");
    if(child==0){const auto profile=std::to_string(p);::execl("/proc/self/exe","native-btree-probe","--native-btree-probe",path.c_str(),profile.c_str(),nullptr);::_exit(125);}
    int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable reads actual canonical B-tree image");
  }
}
std::vector<page::NativeBtreePage> BtreeTreeExample(unsigned p) {
  const auto q=(p+1)%5,s=(p+2)%5;const auto base=BtreeExample(p);std::vector<page::NativeBtreePage> pages(7);
  for(unsigned i=0;i<7;++i){auto& r=pages[i];r.header=base.header;r.header.page_type=i==0?0x200:i<3?0x201:0x202;
    r.header.page_uuid=Id(static_cast<byte>(90+i));r.header.page_number=i==0?12:i==1?15:i==2?16:17+i;r.header.page_generation=111+i;
    if(i==1||i==4||i==6){r.header.filespace_uuid=Id(7);r.header.page_size_bytes=sizes[q];r.header.page_size_profile_uuid=Profile(q);}
    r.dependencies=base.dependencies;r.creator_transaction_uuid=base.creator_transaction_uuid;r.creator_local_transaction_id=17;r.maintenance_state=1;r.tree_level=i==0?2:i<3?1:0;}
  const auto ref=[&](unsigned i){const auto& h=pages[i].header;return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};};
  const auto key=[](unsigned n){return page::NativeBtreeKey{{static_cast<byte>(n)},Id(static_cast<byte>(70+n)),Id(static_cast<byte>(120+n))};};
  const auto branch=[&](unsigned i,unsigned first,unsigned second,unsigned pivot){auto& r=pages[i];r.first_child=ref(first);page::NativeBtreeCell c;c.key=key(pivot);c.child=ref(second);r.cells.push_back(c);};
  branch(0,1,2,40);branch(1,3,4,20);branch(2,5,6,60);
  pages[1].parent=ref(0);pages[2].parent=ref(0);pages[1].right=ref(2);pages[2].left=ref(1);
  pages[1].high_fence=key(40);pages[2].low_fence=key(40);
  for(unsigned i=3;i<7;++i){auto& r=pages[i];r.parent=ref(i<5?1:2);
    if(i>3){r.left=ref(i-1);r.low_fence=key((i-3)*20);}if(i<6){r.right=ref(i+1);r.high_fence=key((i-2)*20);}
    for(unsigned n:{i==3?10u:(i-3)*20,i==3?15u:(i-3)*20+10}){page::NativeBtreeCell c;c.key=key(n);c.deleted=n==15;
      c.base_page=disk::NativePageReference{Id(9),50+i,3,Profile(s)};r.cells.push_back(c);}}
  return pages;
}
std::vector<page::NativeBtreePage> BtreeDeepTreeExample(unsigned p) {
  const auto original=BtreeTreeExample(p);std::vector<page::NativeBtreePage> nodes{original[0]};
  nodes.insert(nodes.end(),original.begin(),original.end());nodes.insert(nodes.end(),original.begin(),original.end());
  nodes[0].header.page_number=30;nodes[0].header.page_generation=200;nodes[0].header.page_uuid=Id(150);nodes[0].tree_level=3;
  for(unsigned i=8;i<15;++i){auto& n=nodes[i];n.header.page_number+=20;n.header.page_generation+=30;n.header.page_uuid=Id(static_cast<byte>(160+i-8));
    const auto rebase=[](auto& ref){if(ref){ref->page_number+=20;ref->page_generation+=30;}};
    rebase(n.parent);rebase(n.left);rebase(n.right);rebase(n.first_child);for(auto& c:n.cells){rebase(c.child);c.key.encoded_key[0]+=100;}
    if(n.low_fence)n.low_fence->encoded_key[0]+=100;if(n.high_fence)n.high_fence->encoded_key[0]+=100;}
  const auto ref=[&](unsigned i){const auto& h=nodes[i].header;return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};};
  const page::NativeBtreeKey boundary{{100},Id(170),Id(220)};
  nodes[0].first_child=ref(1);nodes[0].cells[0].child=ref(8);nodes[0].cells[0].key=boundary;
  nodes[1].header.page_type=0x201;nodes[8].header.page_type=0x201;nodes[1].parent=ref(0);nodes[8].parent=ref(0);
  for(const auto pair:{std::pair{1u,8u},std::pair{3u,9u},std::pair{7u,11u}}){nodes[pair.first].right=ref(pair.second);nodes[pair.second].left=ref(pair.first);
    nodes[pair.first].high_fence=boundary;nodes[pair.second].low_fence=boundary;}
  return nodes;
}
void NativeBtreeTrees() {
  using E=page::NativeBtreeError;
  const auto empty=[&](const auto& r){Check(!r.ok()&&r.pages.empty()&&r.leaves.empty()&&r.retained_image_bytes==0,"native tree failure exposes no images leaf order or counters");};
  for(unsigned p=0;p<5;++p){const auto q=(p+1)%5,s=(p+2)%5;Fixture fixture;disk::FileDevice first,second,base;
    const auto path1=(fixture.root/"tree-first").string(),path2=(fixture.root/"tree-second").string(),path3=(fixture.root/"tree-base").string();
    auto z1=Example(p,6),z2=Example(q,5),z3=Example(s,1);z2.bootstrap.filespace_uuid=Id(7);z3.bootstrap.filespace_uuid=Id(9);
    for(auto& r:z2.roots)r.filespace_uuid=Id(7);for(auto& r:z3.roots)r.filespace_uuid=Id(9);
    const auto prepare=[&](auto& device,const auto& path,const auto& zero){const auto image=Oracle(zero);const byte padding=0;
      Check(device.Open(path,disk::FileOpenMode::create_new).ok()&&device.WriteAt(0,image.data(),image.size()).ok()
        &&device.WriteAt(zero.total_pages*zero.bootstrap.page_size_bytes-1,&padding,1).ok(),"prepare actual retained tree filespace");};
    prepare(first,path1,z1);prepare(second,path2,z2);prepare(base,path3,z3);
    const auto original=BtreeTreeExample(p);auto nodes=original;
    const auto reference=[](const auto& node){const auto& h=node.header;return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};};
    const auto persist=[&]{for(const auto& r:nodes){const auto bytes=BtreeOracle(r);auto& device=r.header.filespace_uuid==Id(2)?first:second;
      Check(device.WriteAt(r.header.page_number*r.header.page_size_bytes,bytes.data(),bytes.size()).ok(),"persist independently packed navigation image");}
      Check(first.Sync().ok()&&second.Sync().ok()&&base.Sync().ok(),"sync actual navigation tree");};
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(9),Profile(s),&base},{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};
    const u64 budget=4*sizes[p]+3*sizes[q];const auto root=reference(original[0]);
    const auto read=[&](u64 allowance){return page::ReadNativeBtreeTreeFromOpenDevices(Id(1),devices,root,original[0].dependencies,allowance);};
    persist();auto result=read(budget);Check(result.ok()&&result.pages.size()==7&&result.leaves==std::vector<std::size_t>{2,3,5,6}
      &&result.retained_image_bytes==budget,"complete cross-profile two-level navigation tree and exact retained budget");
    for(unsigned i=0;i<4;++i)Check(result.pages[result.leaves[i]].bytes==BtreeOracle(original[3+i]),"verified leaf order uses actual independently packed images");
    result=read(budget-1);empty(result);Check(result.error==E::resource_exhausted,"late subtree one-byte-short budget returns no valid prefix");
    result=read(0);empty(result);Check(result.error==E::invalid_reference,"zero navigation allowance refused");
    auto reversed=devices;std::reverse(reversed.begin(),reversed.end());
    std::atomic<bool> concurrent_ok{true};std::thread t1([&]{for(unsigned i=0;i<3;++i)if(!read(budget).ok())concurrent_ok=false;});
    std::thread t2([&]{for(unsigned i=0;i<3;++i)if(!page::ReadNativeBtreeTreeFromOpenDevices(Id(1),reversed,root,original[0].dependencies,budget).ok())concurrent_ok=false;});
    t1.join();t2.join();Check(concurrent_ok,"opposing caller orders share consistent retained-filespace lock ordering");
    if(p==0){
      const auto mutex_of=[](auto& device){auto guard=device.AcquireOperationGuard();return guard.mutex();};
      const std::array mutexes{mutex_of(first),mutex_of(second),mutex_of(base)};
      tree_read_paused=false;resume_tree_read=false;pause_next_tree_read=true;std::atomic<bool> done{false};page::NativeBtreeTreeResult paused_result;
      std::thread paused_reader([&]{paused_result=read(budget);done=true;});
      while(!tree_read_paused.load()&&!done.load())std::this_thread::yield();
      bool all_held=tree_read_paused.load();for(auto* mutex:mutexes)if(mutex->try_lock()){all_held=false;mutex->unlock();}
      resume_tree_read=true;paused_reader.join();pause_next_tree_read=false;
      Check(all_held&&paused_result.ok(),"all filespace guards held before the first physical read through complete traversal");
      for(unsigned fault=0;fault<14;++fault){nodes=original;E expected=E::tree_reference_mismatch;
        if(fault==0)++nodes[6].parent->page_generation;
        if(fault==1){nodes[2].tree_level=2;expected=E::tree_level_mismatch;}
        if(fault==2){nodes[4].low_fence->encoded_key={19};nodes[3].high_fence=nodes[4].low_fence;expected=E::tree_fence_mismatch;}
        if(fault==3){nodes[4].right=reference(nodes[6]);expected=E::tree_sibling_mismatch;}
        if(fault==4){nodes[2].left.reset();expected=E::tree_sibling_mismatch;}
        if(fault==5){nodes[6].right=reference(nodes[3]);nodes[6].high_fence=page::NativeBtreeKey{{80},Id(150),Id(200)};expected=E::tree_fence_mismatch;}
        if(fault==6)nodes[5].header.page_uuid=nodes[3].header.page_uuid;
        if(fault==7)nodes[2].first_child=reference(nodes[4]);
        if(fault==8){nodes[6].dependencies.dependency_map_sha256[31]^=1;expected=E::binding_mismatch;}
        if(fault==9){for(auto& c:nodes[6].cells)c.base_page->page_size_profile_uuid=Profile(4);expected=E::invalid_filespace;}
        if(fault==10){for(auto& c:nodes[6].cells)c.base_page->page_number=64;expected=E::invalid_filespace;}
        if(fault==11){nodes[1].right.reset();expected=E::tree_sibling_mismatch;}
        if(fault==12||fault==13){auto& key=*nodes[4].low_fence;if(fault==12)--key.row_uuid.bytes[15];else --key.version_uuid.bytes[15];
          nodes[3].high_fence=key;expected=E::tree_fence_mismatch;}
        for(const auto& n:nodes)Check(page::DecodeNativeBtreePage(BtreeOracle(n)).ok(),"negative tree still has individually valid native images");
        persist();result=read(budget);empty(result);Check(result.error==expected,"exact cross-page tree fault classification actual="+std::to_string(static_cast<unsigned>(result.error))+" expected="+std::to_string(static_cast<unsigned>(expected)));}
      nodes=original;persist();
      auto duplicate=devices;duplicate.push_back(devices[0]);result=page::ReadNativeBtreeTreeFromOpenDevices(Id(1),duplicate,root,original[0].dependencies,budget);empty(result);Check(result.error==E::invalid_filespace,"duplicate retained filespace refused");
      duplicate=devices;duplicate[0].device=&first;result=page::ReadNativeBtreeTreeFromOpenDevices(Id(1),duplicate,root,original[0].dependencies,budget);empty(result);Check(result.error==E::invalid_filespace,"aliased retained device refused");
      duplicate=devices;duplicate.erase(duplicate.begin());result=page::ReadNativeBtreeTreeFromOpenDevices(Id(1),duplicate,root,original[0].dependencies,budget);empty(result);Check(result.error==E::invalid_filespace,"base locator cannot reference an unbound filespace");
      auto foreign=z3;foreign.bootstrap.database_uuid=Id(201);const auto foreign_bytes=Oracle(foreign);
      Check(base.WriteAt(0,foreign_bytes.data(),foreign_bytes.size()).ok(),"write foreign node binding on unvisited primary device");result=read(budget);empty(result);Check(result.error==E::invalid_filespace,"unvisited foreign filespace not silently ignored");
      const auto original_zero=Oracle(z3);Check(base.WriteAt(0,original_zero.data(),original_zero.size()).ok(),"restore primary node binding");
      auto corrupt=BtreeOracle(nodes.back());corrupt[512]^=1;Check(second.WriteAt(nodes.back().header.page_number*sizes[q],corrupt.data(),corrupt.size()).ok(),"corrupt final leaf self digest");
      result=read(budget);empty(result);Check(result.error==E::invalid_integrity,"corrupted final leaf refuses whole earlier tree");persist();
      reads=0;track_reads=true;result=read(budget);track_reads=false;const auto read_count=reads;Check(result.ok(),"measure complete tree reads");
      for(unsigned fault=1;fault<=read_count;++fault){reads=0;read_fault=fault;track_reads=true;result=read(budget);track_reads=false;Check(read_fault==0,"each actual tree read fault consumed");empty(result);}
      observed_allocations=0;count_allocations=true;result=read(budget);count_allocations=false;const auto allocations=observed_allocations;Check(result.ok(),"measure complete tree allocations");bool success=false;
      for(unsigned long allowance=0;allowance<=allocations;++allowance){allocation_budget=allowance;result=read(budget);allocation_budget=-1;if(result.ok()){success=true;break;}
        empty(result);Check(result.error==E::resource_exhausted,"every tree allocation refusal classified");}Check(success,"every complete tree allocation fault position");
      for(unsigned fault=1;fault<=5;++fault){hash_fault=fault;result=read(budget);Check(hash_fault==0,"tree digest backend fault consumed");empty(result);Check(result.error==E::hash_failure,"tree hash provider error preserved");}
      // This changes only root reachability. Unreachable stale images remain an
      // allocation-map verifier obligation, not fabricated tree members.
      nodes[0].tree_level=0;nodes[0].first_child.reset();nodes[0].cells.clear();persist();result=read(sizes[p]);
      Check(result.ok()&&result.pages.size()==1&&result.leaves==std::vector<std::size_t>{0}&&result.retained_image_bytes==sizes[p],"actual empty allocated root tree");
      nodes=BtreeDeepTreeExample(p);persist();const auto deep_root=reference(nodes[0]);const u64 deep_budget=2*budget+sizes[p];
      const auto deep_read=[&]{return page::ReadNativeBtreeTreeFromOpenDevices(Id(1),devices,deep_root,original[0].dependencies,deep_budget);};
      result=deep_read();Check(result.ok()&&result.pages.size()==15&&result.leaves==std::vector<std::size_t>{3,4,6,7,10,11,13,14}
        &&result.retained_image_bytes==deep_budget,"three-level actual tree includes branch siblings across different parents");
      const auto right=nodes[3].right;nodes[3].right=reference(nodes[10]);
      Check(page::DecodeNativeBtreePage(BtreeOracle(nodes[3])).ok(),"cross-parent branch sibling fault is locally valid");persist();result=deep_read();empty(result);
      Check(result.error==E::tree_sibling_mismatch,"branch sibling reciprocity checked across parent boundaries");nodes[3].right=right;persist();
    }
    Exclusive(path1);Exclusive(path2);Exclusive(path3);Check(first.Close().ok()&&second.Close().ok()&&base.Close().ok(),"close tree owners before fresh executable");
    const auto child=::fork();Check(child>=0,"fork independent native tree reader");
    if(child==0){const auto profile=std::to_string(p);::execl("/proc/self/exe","native-btree-tree-probe",p==0?"--native-btree-deep-probe":"--native-btree-tree-probe",fixture.root.c_str(),profile.c_str(),nullptr);::_exit(125);}
    int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable verifies actual cross-profile navigation tree");
  }
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
struct CatalogBindingFixtureImages {
  std::vector<page::NativeBtreePage> nodes;
  std::vector<db::NativeCatalogLeafPage> leaves;
};
CatalogBindingFixtureImages CatalogBindingImages(unsigned p) {
  const auto q=(p+1)%5,s=(p+2)%5;CatalogBindingFixtureImages images;images.nodes=BtreeTreeExample(p);
  // Fixture row/version identities are disjoint from each other and page,
  // object and transaction identities, just as separately issued UUIDs are.
  const auto key_identity=[](auto& key){key.row_uuid.bytes[14]=0x41;key.version_uuid.bytes[14]=0x42;};
  for(auto& node:images.nodes){if(node.low_fence)key_identity(*node.low_fence);if(node.high_fence)key_identity(*node.high_fence);
    for(auto& cell:node.cells)key_identity(cell.key);}
  for(unsigned j=0;j<4;++j){auto leaf=LeafExample(j==3?q:s);auto& h=leaf.header;
    h.filespace_uuid=j==3?Id(7):Id(9);h.page_number=53+j;h.page_generation=3;h.page_uuid=Id(static_cast<byte>(150+j));
    leaf.body.page_number=h.page_number;leaf.body.page_generation=h.page_generation;
    for(unsigned i=0;i<2;++i){auto& row=leaf.body.rows[i];auto& cell=images.nodes[3+j].cells[i];
      row.row_uuid.value=cell.key.row_uuid;row.version_uuid=cell.key.version_uuid;
      auto metadata=catalog::DecodeCatalogMetadataVersion(row.cells[0].value.payload);Check(metadata.ok(),"existing common metadata fixture decoded");
      metadata.record.record.header.row_uuid=row.row_uuid;metadata.record.record.header.object_uuid.value=Id(static_cast<byte>(180+2*j+i));
      const auto encoded=catalog::EncodeCatalogMetadataVersion(metadata.record);Check(encoded.ok(),"bound common metadata fixture produced");row.cells[0].value.payload=encoded.bytes;
      cell.base_page=disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
    images.leaves.push_back(std::move(leaf));}
  auto extra=images.nodes[3].cells[0];extra.key.encoded_key={12};images.nodes[3].cells.insert(images.nodes[3].cells.begin()+1,extra);
  return images;
}
void NativeCatalogRelationBindings() {
  using E=db::NativeCatalogRelationError;using LE=db::NativeCatalogLeafError;
  const auto empty=[&](const auto& r){Check(!r.ok()&&!r.index&&r.catalogs.empty()&&r.bindings.empty()&&r.retained_image_bytes==0,"catalog join refusal exposes no tree pages metadata bindings or counters");};
  for(unsigned p=0;p<5;++p){const auto q=(p+1)%5,s=(p+2)%5;Fixture fixture;disk::FileDevice first,second,base;
    const auto path1=(fixture.root/"catalog-index").string(),path2=(fixture.root/"catalog-data").string(),path3=(fixture.root/"catalog-primary").string();
    auto z1=Example(p,6),z2=Example(q,5),z3=Example(s,1);z2.bootstrap.filespace_uuid=Id(7);z3.bootstrap.filespace_uuid=Id(9);
    for(auto& r:z2.roots)r.filespace_uuid=Id(7);for(auto& r:z3.roots)r.filespace_uuid=Id(9);
    const auto prepare=[&](auto& device,const auto& path,const auto& zero){const auto image=Oracle(zero);const byte padding=0;
      Check(device.Open(path,disk::FileOpenMode::create_new).ok()&&device.WriteAt(0,image.data(),image.size()).ok()
        &&device.WriteAt(zero.total_pages*zero.bootstrap.page_size_bytes-1,&padding,1).ok(),"prepare real catalog relation filespace");};
    prepare(first,path1,z1);prepare(second,path2,z2);prepare(base,path3,z3);
    const auto original=CatalogBindingImages(p);auto images=original;
    const auto reference=[](const auto& h){return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};};
    const auto persist=[&]{for(const auto& n:images.nodes){const auto b=BtreeOracle(n);auto& device=n.header.filespace_uuid==Id(2)?first:second;
      Check(device.WriteAt(n.header.page_number*n.header.page_size_bytes,b.data(),b.size()).ok(),"persist catalog navigation image");}
      for(const auto& leaf:images.leaves){const auto b=LeafOracle(leaf);auto& device=leaf.header.filespace_uuid==Id(9)?base:second;
        Check(device.WriteAt(leaf.header.page_number*leaf.header.page_size_bytes,b.data(),b.size()).ok(),"persist actual native catalog leaf");}
      Check(first.Sync().ok()&&second.Sync().ok()&&base.Sync().ok(),"sync native catalog index and base images");};
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(9),Profile(s),&base},{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};
    const page::NativeCatalogRootReference indexed{1,0x200,reference(original.nodes[0].header),original.nodes[0].dependencies.index_uuid};
    const db::NativeCatalogRelationBinding binding{Id(101),original.nodes[0].dependencies};
    const page::NativeCatalogRootReference direct{1,6,reference(original.leaves[0].header),Id(101)};
    const u64 budget=4*sizes[p]+4*sizes[q]+3*sizes[s];
    const auto read=[&](u64 allowance){return db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),devices,indexed,binding,allowance);};
    persist();auto result=read(budget);Check(result.ok()&&result.index&&result.index->pages.size()==7&&result.catalogs.size()==4&&result.bindings.size()==9
      &&result.retained_image_bytes==budget,"actual catalog tree to row/version join with distinct-image budget");
    for(const auto& b:result.bindings){Check(b.index_entry.has_value(),"indexed binding retains its source entry");
      const auto& cell=result.index->pages[b.index_entry->page_index].page->cells[b.index_entry->cell_index];
      const auto& leaf=result.catalogs[b.catalog_page_index];const auto& row=leaf.page->body.rows[b.catalog_row_index];
      Check(row.row_uuid.value==cell.key.row_uuid&&row.version_uuid==cell.key.version_uuid
        &&leaf.metadata.at(row.version_uuid).record.header.row_uuid.value==row.row_uuid.value,"bindings refer to actual native rows and validated metadata");}
    Check(result.bindings[0].catalog_page_index==result.bindings[1].catalog_page_index&&result.bindings[0].catalog_row_index==result.bindings[1].catalog_row_index,
      "multiple keys retain separate bindings without duplicate base images");
    const auto deleted=result.bindings[2].index_entry;Check(deleted&&result.index->pages[deleted->page_index].page->cells[deleted->cell_index].deleted,
      "deleted index entry remains bound for owning MGA interpretation");
    result=read(budget-1);empty(result);Check(result.error==E::resource_exhausted,"one-byte-short last base image refuses whole join");
    result=db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),devices,direct,{Id(101),{}},sizes[s]);
    Check(result.ok()&&!result.index&&result.catalogs.size()==1&&result.bindings.size()==2&&result.retained_image_bytes==sizes[s]
      &&!result.bindings[0].index_entry,"direct leaf route enumerates actual retained rows");
    result=db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),devices,direct,{Id(101),{}},sizes[s]-1);empty(result);Check(result.error==E::resource_exhausted,"direct leaf exact budget boundary");
    if(p==0){
      for(unsigned fault=0;fault<9;++fault){images=original;E expected=E::invalid_locator;
        if(fault==0){images.nodes[6].cells[1].key.row_uuid=Id(240);expected=E::binding_mismatch;}
        if(fault==1)images.nodes[6].cells[1].key.version_uuid=Id(241);
        if(fault==2)images.nodes[6].cells[1].base_page=images.nodes[3].cells[0].base_page;
        if(fault==3)images.nodes[6].cells[1].base_page.reset();
        if(fault==4){++images.nodes[3].cells[1].base_page->page_generation;expected=E::binding_mismatch;}
        if(fault==5){images.leaves[3].header.page_uuid=images.leaves[0].header.page_uuid;expected=E::duplicate_identity;}
        if(fault==6){images.leaves[3].body.rows[0].version_uuid=images.leaves[0].body.rows[1].version_uuid;expected=E::duplicate_identity;}
        if(fault==7)images.nodes[3].cells[0].base_page=reference(images.nodes[0].header);
        if(fault==8){auto row=images.leaves[3].body.rows[0];row.row_uuid.value=Id(240);row.version_uuid=Id(241);row.internal_row_ordinal=3;row.stable_slot_id=3;
          images.leaves[3].body.rows.push_back(row);expected=E::leaf_failure;}
        persist();result=read(budget);empty(result);Check(result.error==expected,"exact catalog binding fault="+std::to_string(fault)+" actual="+std::to_string(static_cast<unsigned>(result.error))+" expected="+std::to_string(static_cast<unsigned>(expected)));
        if(fault==8)Check(result.leaf_error==LE::invalid_metadata,"unindexed malformed retained row is not skipped");}
      images=original;persist();
      auto wrong=binding;wrong.relation_uuid=Id(240);result=db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),devices,indexed,wrong,budget);empty(result);
      Check(result.error==E::leaf_failure&&result.leaf_error==LE::binding_mismatch,"actual relation owner binding required");
      auto bad_head=indexed;bad_head.object_uuid=Id(240);result=db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),devices,bad_head,binding,budget);empty(result);Check(result.error==E::binding_mismatch,"head index UUID must match exact dependency tuple");
      result=db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),devices,direct,binding,budget);empty(result);Check(result.error==E::binding_mismatch,"direct head cannot inherit an unrelated index dependency tuple");
      for(u64 flags:{1u,2u,4u,8u}){images=original;images.leaves[3].header.flags=flags;persist();result=read(budget);empty(result);
        Check(result.error==E::leaf_failure&&result.leaf_error==(flags==1?LE::encrypted_requires_crypto_authority:flags==2?LE::cluster_requires_authority:LE::header_policy_requires_authority),"late base image retains truthful crypto cluster header policy failure");}
      images=original;persist();auto corrupt=LeafOracle(images.leaves[3]);corrupt.back()^=1;
      Check(second.WriteAt(56*sizes[q],corrupt.data(),corrupt.size()).ok(),"corrupt actual final catalog leaf digest");result=read(budget);empty(result);Check(result.error==E::leaf_failure&&result.leaf_error==LE::invalid_integrity,"late corrupt catalog page exposes no earlier metadata");persist();
      const auto mutex_of=[](auto& device){auto guard=device.AcquireOperationGuard();return guard.mutex();};const std::array mutexes{mutex_of(first),mutex_of(second),mutex_of(base)};
      tree_read_paused=false;resume_tree_read=false;pause_next_tree_read=true;std::atomic<bool> done{false};db::NativeCatalogRelationImageResult paused_result;
      std::thread reader([&]{paused_result=read(budget);done=true;});while(!tree_read_paused.load()&&!done.load())std::this_thread::yield();
      bool all_held=tree_read_paused.load();for(auto* mutex:mutexes)if(mutex->try_lock()){all_held=false;mutex->unlock();}resume_tree_read=true;reader.join();pause_next_tree_read=false;
      Check(all_held&&paused_result.ok(),"all catalog join filespace guards held before the first physical read");
      reads=0;track_reads=true;result=read(budget);track_reads=false;const auto read_count=reads;Check(result.ok(),"measure complete catalog join reads");
      for(unsigned fault=1;fault<=read_count;++fault){reads=0;read_fault=fault;track_reads=true;result=read(budget);track_reads=false;Check(read_fault==0,"each actual catalog join read fault consumed");empty(result);}
      observed_allocations=0;count_allocations=true;result=read(budget);count_allocations=false;const auto allocations=observed_allocations;Check(result.ok(),"measure catalog join allocations");bool success=false;
      for(unsigned long allowance=0;allowance<=allocations;++allowance){allocation_budget=allowance;result=read(budget);allocation_budget=-1;if(result.ok()){success=true;break;}
        empty(result);Check(result.error==E::resource_exhausted||(result.error==E::tree_failure&&result.tree_error==page::NativeBtreeError::resource_exhausted)
          ||(result.error==E::leaf_failure&&result.leaf_error==LE::resource_exhausted),"catalog join allocation refusal classified");}Check(success,"every catalog join allocation fault position");
      for(unsigned fault=1;fault<=5;++fault){hash_fault=fault;result=read(budget);Check(hash_fault==0,"catalog join digest backend fault consumed");empty(result);Check(result.error==E::hash_failure,"catalog join digest failure preserved");}
      images.leaves[0].body.rows.clear();persist();result=db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),devices,direct,{Id(101),{}},sizes[s]);
      Check(result.ok()&&result.catalogs.size()==1&&result.bindings.empty(),"empty allocated direct catalog leaf, not invented existence");
      images=original;images.nodes[0].tree_level=0;images.nodes[0].first_child.reset();images.nodes[0].cells.clear();persist();result=read(sizes[p]);
      Check(result.ok()&&result.index&&result.index->pages.size()==1&&result.catalogs.empty()&&result.bindings.empty(),"empty allocated catalog index is represented without invented base rows");images=original;persist();
    }
    Exclusive(path1);Exclusive(path2);Exclusive(path3);Check(first.Close().ok()&&second.Close().ok()&&base.Close().ok(),"close catalog join files before independent process");
    const auto child=::fork();Check(child>=0,"fork actual catalog relation reader");if(child==0){const auto profile=std::to_string(p);
      ::execl("/proc/self/exe","catalog-relation-probe","--catalog-relation-probe",fixture.root.c_str(),profile.c_str(),nullptr);::_exit(125);}
    int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable binds persisted index entries to catalog rows");
  }
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
void CanonicalCheckpointCatalogRoots() {
  using E=db::NativeCheckpointError;
  for(unsigned p=0;p<5;++p) {
    const unsigned q=(p+1)%5;Fixture fixture;disk::FileDevice first,second;
    const auto path1=(fixture.root/"catalog-first").string(),path2=(fixture.root/"catalog-second").string();
    auto z1=Example(p),z2=Example(q);z2.bootstrap.filespace_uuid=Id(7);
    for(auto& root:z2.roots)root.filespace_uuid=Id(7);
    Check(first.Open(path1,disk::FileOpenMode::create_new).ok()&&second.Open(path2,disk::FileOpenMode::create_new).ok(),"own checkpoint catalog filespaces");
    const auto prepare=[&](disk::FileDevice& d,const auto& z){const auto bytes=Oracle(z);const byte padding=0;
      Check(d.WriteAt(0,bytes.data(),bytes.size()).ok()&&d.WriteAt(z.total_pages*z.bootstrap.page_size_bytes-1,&padding,1).ok(),"prepare actual catalog filespace");};
    prepare(first,z1);prepare(second,z2);
    auto inventory=InventoryExample(p);inventory.inventory.next_local_transaction_id=18;inventory.inventory.next_commit_sequence=3;
    auto& creator=inventory.inventory.entries.front();creator.identity.local_id=mga::MakeLocalTransactionId(11);
    creator.identity.transaction_uuid.value=Id(91);creator.state=mga::TransactionState::committed;creator.commit_sequence=1;
    auto checkpoint_creator=creator;checkpoint_creator.identity.local_id=mga::MakeLocalTransactionId(17);
    checkpoint_creator.identity.transaction_uuid.value=Id(98);checkpoint_creator.commit_sequence=2;
    inventory.inventory.entries.push_back(checkpoint_creator);
    auto catalog=RootExample(p);catalog.creator_local_transaction_id=11;
    auto feature=RootExample(q);feature.root_kind=8;feature.creator_local_transaction_id=11;
    feature.header.filespace_uuid=Id(7);feature.header.page_number=13;feature.header.page_uuid=Id(89);feature.object_uuid=Id(43);
    feature.roots.erase(feature.roots.begin(),feature.roots.end()-1);
    for(auto& target:feature.roots)target.page.filespace_uuid=Id(7);
    auto checkpoint=CheckpointExample(p);
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};
    const u64 shared_budget=3*sizes[p],distinct_budget=shared_budget+sizes[q];
    const auto persist=[&](const auto& inv,const auto& cat,const auto& feat,bool shared,u64 oit=18,u64 oat=18) {
      auto cp=checkpoint;const auto inv_bytes=InventoryOracle(inv,oit,oat,oat),cat_bytes=RootOracle(cat),feat_bytes=RootOracle(feat);
      cp.roots[0].page=InventoryRef(inv);cp.roots[0].object_uuid=inv.object_uuid;
      Check(SHA256(inv_bytes.data(),inv_bytes.size(),cp.roots[0].sha256.data())!=nullptr,"independent inventory target hash");
      const auto bind=[&](auto& target,const auto& root,const auto& bytes){target.page={root.header.filespace_uuid,root.header.page_number,root.header.page_generation,root.header.page_size_profile_uuid};
        target.object_uuid=root.object_uuid;Check(SHA256(bytes.data(),bytes.size(),target.sha256.data())!=nullptr,"independent complete catalog target hash");};
      bind(cp.roots[4],cat,cat_bytes);if(shared)bind(cp.roots[8],cat,cat_bytes);else bind(cp.roots[8],feat,feat_bytes);
      const auto cp_bytes=CheckpointOracle(cp);
      Check(first.WriteAt(14*sizes[p],inv_bytes.data(),inv_bytes.size()).ok()&&first.WriteAt(12*sizes[p],cat_bytes.data(),cat_bytes.size()).ok()
        &&second.WriteAt(13*sizes[q],feat_bytes.data(),feat_bytes.size()).ok()&&first.WriteAt(19*sizes[p],cp_bytes.data(),cp_bytes.size()).ok()
        &&first.Sync().ok()&&second.Sync().ok(),"persist actual checkpoint inventory catalog feature images");
      return cp;
    };
    const auto read=[&](u64 budget){return db::VerifyNativeCheckpointCatalogRootsFromOpenDevices(Id(1),devices,CheckpointRef(checkpoint),budget);};
    const auto empty=[&](const auto& r){Check(!r.ok()&&r.catalogs.empty()&&r.retained_image_bytes==0&&!r.checkpoint_inventory.checkpoint
      &&r.checkpoint_inventory.inventory.entries.empty()&&!r.checkpoint_inventory.inventory.publication_base,"catalog join failure returns no verified prefix or CAS base");};
    persist(inventory,catalog,feature,true);auto result=read(shared_budget);
    Check(result.ok()&&result.catalogs.size()==1&&result.feature_root_index==0&&result.retained_image_bytes==shared_budget
      &&result.catalogs[0].root->creator_transaction_uuid==Id(91)&&result.checkpoint_inventory.inventory.entries.size()==2
      &&!result.checkpoint_inventory.inventory.publication_base,"shared catalog feature image charged once with committed binary creator");
    result=read(shared_budget-1);empty(result);Check(result.error==E::resource_exhausted,"shared root exact budget boundary");
    persist(inventory,catalog,feature,false);result=read(distinct_budget);
    Check(result.ok()&&result.catalogs.size()==2&&result.feature_root_index==1&&result.retained_image_bytes==distinct_budget
      &&result.catalogs[1].root->object_uuid==Id(43)&&result.catalogs[1].root->root_kind==8,"distinct cross-profile feature root actual image binding");
    result=read(distinct_budget-1);empty(result);Check(result.error==E::resource_exhausted,"distinct root exact budget boundary");
    result=read(2*sizes[p]-1);empty(result);Check(result.error==E::inventory_failure
      &&result.checkpoint_inventory.inventory_error==page::NativeInventoryError::resource_exhausted,"nested inventory refusal retains its actual cause");
    for(bool bad_feature:{false,true}) {
      auto cat=catalog,feat=feature;auto& bad=bad_feature?feat:cat;bad.creator_transaction_uuid=Id(200);
      persist(inventory,cat,feat,false);result=read(distinct_budget);empty(result);Check(result.error==E::catalog_creator_mismatch,"catalog and feature exact creator UUID required");
      bad.creator_transaction_uuid=Id(91);bad.creator_local_transaction_id=10;
      persist(inventory,cat,feat,false);result=read(distinct_budget);empty(result);Check(result.error==E::catalog_creator_mismatch,"missing catalog creator local number refused");
    }
    for(auto state:{mga::TransactionState::active,mga::TransactionState::prepared,mga::TransactionState::limbo,
        mga::TransactionState::failed_terminal,mga::TransactionState::rolled_back}) {
      auto inv=inventory;auto& e=inv.inventory.entries[0];e.state=state;e.commit_sequence=0;
      const u64 oit=state==mga::TransactionState::rolled_back?18:11;
      const u64 oat=state==mga::TransactionState::failed_terminal?18:oit;
      persist(inv,catalog,feature,false,oit,oat);result=read(distinct_budget);empty(result);
      Check(result.error==E::catalog_creator_not_committed,"uncommitted catalog creator cannot borrow committed checkpoint authority");
    }
    for(auto origin:{mga::TransactionState::committed,mga::TransactionState::rolled_back,mga::TransactionState::failed_terminal}) {
      auto inv=inventory;auto& e=inv.inventory.entries[0];e.state=mga::TransactionState::archived;e.archived_from_state=origin;
      if(origin!=mga::TransactionState::committed)e.commit_sequence=0;
      persist(inv,catalog,feature,false);result=read(distinct_budget);
      if(origin==mga::TransactionState::committed)Check(result.ok(),"archived committed catalog creator retained");
      else{empty(result);Check(result.error==E::catalog_creator_not_committed,"archived noncommit is not catalog publication authority");}
    }
    auto inv=inventory;inv.inventory.entries[0].identity.scope=mga::TransactionScope::cluster_global;
    persist(inv,catalog,feature,false);result=read(distinct_budget);empty(result);Check(result.error==E::catalog_creator_mismatch,"global creator cannot certify standalone catalog");
    for(unsigned role:{4u,8u}) {
      auto cp=persist(inventory,catalog,feature,false);cp.roots[role].sha256[0]^=1;auto bytes=CheckpointOracle(cp);
      Check(first.WriteAt(19*sizes[p],bytes.data(),bytes.size()).ok(),"persist sealed checkpoint with wrong complete root hash");
      result=read(distinct_budget);empty(result);Check(result.error==E::invalid_integrity,"complete actual catalog or feature image hash must match checkpoint");
    }
    persist(inventory,catalog,feature,false);auto corrupt=RootOracle(feature);corrupt[304]^=1;
    Check(second.WriteAt(13*sizes[q],corrupt.data(),corrupt.size()).ok(),"corrupt feature image after valid catalog");
    result=read(distinct_budget);empty(result);Check(result.error==E::catalog_failure&&result.catalog_error==RootError::invalid_integrity,"feature self digest failure after catalog exposes no prefix");
    persist(inventory,catalog,feature,false);
    for(unsigned fault=1;fault<=5;++fault){hash_fault=fault;result=read(distinct_budget);Check(hash_fault==0,"catalog join digest backend fault consumed");empty(result);}
    // Full-image target hashes are separate from each image's internal seal.
    bool digest_end=false;
    for(unsigned fault=1;fault<32;++fault){full_digest_fault=fault;result=read(distinct_budget);
      if(full_digest_fault){full_digest_fault=0;Check(result.ok(),"full-image hash sweep reached end");digest_end=true;break;}
      empty(result);Check(result.error==E::hash_failure
        ||(result.error==E::inventory_failure&&result.checkpoint_inventory.inventory_error==page::NativeInventoryError::hash_failure)
        ||(result.error==E::catalog_failure&&result.catalog_error==RootError::hash_failure),
        "complete target digest provider error propagated: "+std::to_string(static_cast<unsigned>(result.error)));}
    Check(digest_end,"all complete target hash calls faulted");
    reads=0;track_reads=true;result=read(distinct_budget);track_reads=false;const auto read_count=reads;Check(result.ok(),"measure actual catalog join reads");
    for(unsigned fault=1;fault<=read_count;++fault){reads=0;read_fault=fault;track_reads=true;result=read(distinct_budget);track_reads=false;Check(read_fault==0,"catalog join read fault consumed");empty(result);}
    observed_allocations=0;count_allocations=true;result=read(distinct_budget);count_allocations=false;const auto allocation_count=observed_allocations;Check(result.ok(),"measure catalog join allocations");
    bool success=false;
    for(unsigned long budget=0;budget<=allocation_count;++budget){allocation_budget=budget;result=read(distinct_budget);allocation_budget=-1;
      if(result.ok()){success=true;break;}empty(result);Check(result.error==E::resource_exhausted
        ||(result.error==E::inventory_failure&&result.checkpoint_inventory.inventory_error==page::NativeInventoryError::resource_exhausted)
        ||(result.error==E::catalog_failure&&result.catalog_error==RootError::resource_exhausted),"catalog allocation refusal classified");}
    Check(success,"every actual catalog join allocation position");
    Exclusive(path1);Exclusive(path2);Check(first.Close().ok()&&second.Close().ok(),"close catalog fixtures before fresh process");
    const auto child=::fork();Check(child>=0,"fork independent checkpoint catalog reader");
    if(child==0){const auto profile=std::to_string(p);::execl("/proc/self/exe","checkpoint-catalog-probe","--checkpoint-catalog-probe",fixture.root.c_str(),profile.c_str(),nullptr);::_exit(125);}
    int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable verifies persisted catalog and feature roots");
  }
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
struct CatalogTestPin {
  mga::SnapshotVectorResult published;
  mga::PublishedSnapshotPin pin;
  CatalogTestPin(const mga::LocalTransactionInventory& inventory,u64 reader) {
    published=mga::PublishStatementStableSnapshotVector(inventory,mga::MakeLocalTransactionId(reader),1790000001000ull);
    Check(published.ok(),"publish actual inventory snapshot");pin=mga::RetainPublishedSnapshotVector(published.descriptor.snapshot_uuid);Check(pin.valid(),"retain actual engine snapshot pin");
  }
  ~CatalogTestPin(){mga::RevokePublishedSnapshotVector(published.descriptor.snapshot_uuid);mga::ReleasePublishedSnapshotVector(published.descriptor.snapshot_uuid);}
};
void CheckpointCatalogRelations() {
  using E=db::NativeCheckpointCatalogRelationError;
  const auto empty=[](const auto& r){Check(!r.ok()&&!r.checkpoint.checkpoint_inventory.checkpoint&&r.checkpoint.catalogs.empty()
    &&r.checkpoint.checkpoint_inventory.inventory.entries.empty()&&!r.checkpoint.checkpoint_inventory.inventory.publication_base
    &&!r.relation.index&&r.relation.catalogs.empty()&&r.relation.bindings.empty()&&r.row_creators.empty()
    &&r.navigation_creator_entries.empty()&&!r.retained_image_bytes,"checkpoint relation failure exposes no prefix");};
  for(unsigned p=0;p<5;++p){const unsigned q=(p+1)%5,s=(p+2)%5;Fixture fixture;disk::FileDevice first,second,base;
    auto z1=Example(p,6),z2=Example(q,5),z3=Example(s,1);z2.bootstrap.filespace_uuid=Id(7);z3.bootstrap.filespace_uuid=Id(9);
    for(auto& r:z2.roots)r.filespace_uuid=Id(7);for(auto& r:z3.roots)r.filespace_uuid=Id(9);
    const auto prepare=[&](auto& device,const char* name,const auto& zero){const auto bytes=Oracle(zero);const byte padding=0;
      Check(device.Open((fixture.root/name).string(),disk::FileOpenMode::create_new).ok()&&device.WriteAt(0,bytes.data(),bytes.size()).ok()
        &&device.WriteAt(zero.total_pages*zero.bootstrap.page_size_bytes-1,&padding,1).ok(),"prepare checkpoint relation actual filespace");};
    prepare(first,"index",z1);prepare(second,"data",z2);prepare(base,"primary",z3);
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(9),Profile(s),&base},{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};
    const auto original=CatalogBindingImages(p);auto images=original;auto inventory=InventoryExample(s);inventory.header.filespace_uuid=Id(9);inventory.inventory.entries.clear();
    inventory.inventory.next_local_transaction_id=18;inventory.inventory.next_commit_sequence=4;
    for(unsigned i=0;i<3;++i){auto entry=InventoryExample().inventory.entries[0];entry.identity.local_id=mga::MakeLocalTransactionId(i==0?11:i==1?13:17);
      entry.identity.transaction_uuid.value=Id(i==0?91:i==1?162:98);entry.state=mga::TransactionState::committed;entry.commit_sequence=i+1;inventory.inventory.entries.push_back(entry);}
    const auto ref=[](const auto& h){return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};};
    auto cat=RootExample(s);cat.header.filespace_uuid=Id(9);cat.header.page_number=28;cat.creator_local_transaction_id=11;
    for(auto& target:cat.roots)target.page.filespace_uuid=Id(9);
    cat.roots[0]={1,0x200,ref(images.nodes[0].header),images.nodes[0].dependencies.index_uuid};
    auto feature=cat;feature.root_kind=8;feature.object_uuid=Id(43);feature.header.page_uuid=Id(89);feature.header.page_number=29;feature.roots={{6,6,ref(images.leaves[0].header),Id(101)}};
    auto cp=CheckpointExample(s);cp.header.filespace_uuid=Id(9);
    const auto persist=[&](bool distinct=false,u64 oit=18,u64 oat=18){
      for(const auto& node:images.nodes){const auto b=BtreeOracle(node);auto& device=node.header.filespace_uuid==Id(2)?first:second;
        Check(device.WriteAt(node.header.page_number*node.header.page_size_bytes,b.data(),b.size()).ok(),"persist checkpoint relation navigation");}
      for(const auto& leaf:images.leaves){const auto b=LeafOracle(leaf);auto& device=leaf.header.filespace_uuid==Id(9)?base:second;
        Check(device.WriteAt(leaf.header.page_number*leaf.header.page_size_bytes,b.data(),b.size()).ok(),"persist checkpoint relation rows");}
      const auto ib=InventoryOracle(inventory,oit,oat,oat),cb=RootOracle(cat),fb=RootOracle(feature);
      const auto bind=[&](auto& target,const auto& header,const auto& object,const auto& bytes){target.page=ref(header);target.object_uuid=object;
        Check(SHA256(bytes.data(),bytes.size(),target.sha256.data())!=nullptr,"independently bind checkpoint target hash");};
      bind(cp.roots[0],inventory.header,inventory.object_uuid,ib);bind(cp.roots[4],cat.header,cat.object_uuid,cb);
      if(distinct)bind(cp.roots[8],feature.header,feature.object_uuid,fb);else bind(cp.roots[8],cat.header,cat.object_uuid,cb);
      const auto pb=CheckpointOracle(cp);Check(base.WriteAt(14*sizes[s],ib.data(),ib.size()).ok()&&base.WriteAt(28*sizes[s],cb.data(),cb.size()).ok()
        &&base.WriteAt(29*sizes[s],fb.data(),fb.size()).ok()&&base.WriteAt(19*sizes[s],pb.data(),pb.size()).ok()
        &&base.Sync().ok()&&first.Sync().ok()&&second.Sync().ok(),"persist actual checkpoint inventory and catalog role");};
    const db::NativeCatalogRelationBinding binding{Id(101),images.nodes[0].dependencies};const u64 budget=4*sizes[p]+4*sizes[q]+6*sizes[s];
    const auto read=[&](u64 limit){return db::ReadNativeCheckpointCatalogRelationFromOpenDevices(Id(1),devices,CheckpointRef(cp),2,1,binding,limit);};
    persist();auto result=read(budget);Check(result.ok()&&result.retained_image_bytes==budget&&result.row_creators.size()==8
      &&result.navigation_creator_entries.size()==7&&result.relation.bindings.size()==9,"checkpoint actual catalog role and every creator joined error="+std::to_string(static_cast<unsigned>(result.error))
        +" checkpoint="+std::to_string(static_cast<unsigned>(result.checkpoint.error))+" catalog="+std::to_string(static_cast<unsigned>(result.checkpoint.catalog_error))
        +" inventory="+std::to_string(static_cast<unsigned>(result.checkpoint.checkpoint_inventory.inventory_error))+" relation="+std::to_string(static_cast<unsigned>(result.relation.error)));
    for(const auto& row:result.row_creators)Check(row.inventory_entry_index==1,"actual row creator entry");for(auto entry:result.navigation_creator_entries)Check(entry==2,"actual navigation creator entry");
    result=read(budget-1);empty(result);Check(result.error==E::relation_failure&&result.relation.error==db::NativeCatalogRelationError::resource_exhausted,"combined exact budget enforced");
    result=read(3*sizes[s]);empty(result);Check(result.error==E::resource_exhausted,"no relation allowance remains");
    result=db::ReadNativeCheckpointCatalogRelationFromOpenDevices(Id(1),devices,CheckpointRef(cp),8,1,binding,budget);Check(result.ok()&&result.checkpoint.feature_root_index==0,"shared feature role selected");
    persist(true);result=db::ReadNativeCheckpointCatalogRelationFromOpenDevices(Id(1),devices,CheckpointRef(cp),8,6,{Id(101),{}},5*sizes[s]);
    Check(result.ok()&&result.row_creators.size()==2&&result.navigation_creator_entries.empty()&&result.retained_image_bytes==5*sizes[s],"distinct feature direct route follows stored role");
    result=db::ReadNativeCheckpointCatalogRelationFromOpenDevices(Id(1),devices,CheckpointRef(cp),8,1,binding,budget);empty(result);Check(result.error==E::missing_relation,"absent feature role has no catalog fallback");
    if(p==0){const auto saved=inventory.inventory.entries[1];
      for(auto state:{mga::TransactionState::active,mga::TransactionState::prepared,mga::TransactionState::rolled_back,mga::TransactionState::failed_terminal,mga::TransactionState::archived}){
        auto& entry=inventory.inventory.entries[1];entry=saved;entry.state=state;entry.commit_sequence=0;if(state==mga::TransactionState::archived)entry.archived_from_state=mga::TransactionState::rolled_back;
        const bool active=state==mga::TransactionState::active||state==mga::TransactionState::prepared;
        persist(false,active||state==mga::TransactionState::failed_terminal?13:18,active?13:18);result=read(budget);
        Check(result.ok()&&result.checkpoint.checkpoint_inventory.inventory.entries[1].state==state,"retained actual outcome is not replaced by checkpoint commit");}
      inventory.inventory.entries[1]=saved;inventory.inventory.entries[1].identity.transaction_uuid.value=Id(240);persist();result=read(budget);empty(result);Check(result.error==E::creator_mismatch,"exact binary row creator required");
      inventory.inventory.entries[1]=saved;inventory.inventory.entries[1].identity.scope=mga::TransactionScope::cluster_global;persist();result=read(budget);empty(result);Check(result.error==E::cluster_requires_authority,"global creator requires cluster authority");inventory.inventory.entries[1]=saved;
      images.nodes.back().creator_transaction_uuid=Id(240);persist();result=read(budget);empty(result);Check(result.error==E::creator_mismatch,"late navigation creator checked");images=original;
      auto row=images.leaves.back().body.rows.back();row.version_uuid=Id(241);row.row_uuid.value=Id(242);row.transaction_uuid.value=Id(240);row.internal_row_ordinal=row.stable_slot_id=3;
      auto metadata=catalog::DecodeCatalogMetadataVersion(row.cells[0].value.payload);Check(metadata.ok(),"unindexed creator envelope fixture");metadata.record.record.header.row_uuid=row.row_uuid;metadata.record.creator_transaction_uuid=row.transaction_uuid;
      const auto encoded=catalog::EncodeCatalogMetadataVersion(metadata.record);Check(encoded.ok(),"well-formed unindexed creator fixture");row.cells[0].value.payload=encoded.bytes;images.leaves.back().body.rows.push_back(row);
      persist();result=read(budget);empty(result);Check(result.error==E::creator_mismatch,"unindexed later creator checked before publication");images=original;persist();
      reads=0;track_reads=true;result=read(budget);track_reads=false;const auto count=reads;Check(result.ok(),"measure checkpoint relation reads");
      for(unsigned fault=1;fault<=count;++fault){reads=0;read_fault=fault;track_reads=true;result=read(budget);track_reads=false;Check(!read_fault,"actual read fault consumed");empty(result);}
      observed_allocations=0;count_allocations=true;result=read(budget);count_allocations=false;const auto allocations=observed_allocations;Check(result.ok(),"measure checkpoint relation allocations");bool success=false;
      for(unsigned long n=0;n<=allocations;++n){allocation_budget=n;result=read(budget);allocation_budget=-1;if(result.ok()){success=true;break;}empty(result);}Check(success,"every checkpoint relation allocation fault");
      for(unsigned fault=1;fault<=5;++fault){hash_fault=fault;result=read(budget);Check(!hash_fault,"actual hash fault consumed");empty(result);}
      const auto mutex_of=[](auto& device){auto guard=device.AcquireOperationGuard();return guard.mutex();};const std::array mutexes{mutex_of(first),mutex_of(second),mutex_of(base)};
      tree_read_paused=false;resume_tree_read=false;pause_next_tree_read=true;std::atomic<bool> done=false;db::NativeCheckpointCatalogRelationResult paused;
      std::thread reader([&]{paused=read(budget);done=true;});while(!tree_read_paused.load()&&!done.load())std::this_thread::yield();bool held=tree_read_paused.load();
      for(auto* mutex:mutexes)if(mutex->try_lock()){held=false;mutex->unlock();}resume_tree_read=true;reader.join();pause_next_tree_read=false;Check(held&&paused.ok(),"all filespace guards span checkpoint and creator join");
    }
    const auto saved_inventory=inventory;const auto saved_cp=cp;
    using PE=db::NativePinnedCatalogReadError;
    const auto no_rows=[](const auto& r){Check(!r.ok()&&r.rows.empty()&&r.observations.empty()&&r.snapshot_uuid.is_nil()
      &&r.source.row_creators.empty()&&r.source.relation.catalogs.empty()&&!r.source.relation.index
      &&r.source.checkpoint.catalogs.empty()&&!r.source.checkpoint.checkpoint_inventory.checkpoint
      &&r.source.checkpoint.checkpoint_inventory.inventory.entries.empty(),"pinned refusal exposes no snapshot source or rows");};
    const auto pinned=[&](const auto& pin,const auto& identity){return db::ReadNativePinnedCatalogVersionsFromOpenDevices(Id(1),devices,CheckpointRef(cp),2,1,binding,identity,pin,budget);};
    inventory.inventory.entries[1].state=mga::TransactionState::active;inventory.inventory.entries[1].commit_sequence=0;persist(false,13,13);
    auto loaded=read(budget);Check(loaded.ok(),"actual own-writer inventory loaded");const auto own_identity=loaded.checkpoint.checkpoint_inventory.inventory.entries[1].identity;
    CatalogTestPin own_pin(loaded.checkpoint.checkpoint_inventory.inventory,13);
    auto selected=pinned(own_pin.pin,own_identity);Check(selected.ok()&&selected.rows.size()==8&&selected.snapshot_uuid==own_pin.published.descriptor.snapshot_uuid.value,"native pinned own versions selected across filespaces");
    for(const auto& row:selected.rows)Check(row.provisional&&row.effective_lifecycle==catalog::CatalogObjectLifecycle::creating&&row.effective_status==catalog::CatalogObjectStatus::proposed,"own creating versions never inherit committed checkpoint status");
    mga::ReleasePublishedSnapshotVector(own_pin.published.descriptor.snapshot_uuid);selected=pinned(own_pin.pin,own_identity);Check(selected.ok(),"retained pin survives publication owner release");
    mga::PublishedSnapshotPin absent;selected=pinned(absent,own_identity);no_rows(selected);Check(selected.error==PE::snapshot_failure&&selected.diagnostic.diagnostic_code=="SB-MGA-SNAPSHOT-VECTOR-UNKNOWN","absent pin keeps native diagnostic");
    auto foreign=own_identity;foreign.transaction_uuid.value=Id(240);selected=pinned(own_pin.pin,foreign);no_rows(selected);Check(selected.error==PE::reader_mismatch,"pin exact owner UUID required");
    foreign=own_identity;foreign.transaction_uuid.value.bytes[6]=0x41;selected=pinned(own_pin.pin,foreign);no_rows(selected);Check(selected.error==PE::invalid_reader,"non-v7 system reader rejected");
    mga::RevokePublishedSnapshotVector(own_pin.published.descriptor.snapshot_uuid);selected=pinned(own_pin.pin,own_identity);no_rows(selected);Check(selected.error==PE::snapshot_failure&&selected.diagnostic.diagnostic_code=="SB-MGA-SNAPSHOT-VECTOR-REVOKED","revocation keeps native diagnostic");
    inventory=saved_inventory;cp=saved_cp;
    if(p==0){
      auto writer=inventory.inventory.entries[1];writer.identity.local_id=mga::MakeLocalTransactionId(15);writer.identity.transaction_uuid.value=Id(215);writer.state=mga::TransactionState::active;writer.commit_sequence=0;
      inventory.inventory.entries.insert(inventory.inventory.entries.begin()+2,writer);auto reader=writer;reader.identity.local_id=mga::MakeLocalTransactionId(19);reader.identity.transaction_uuid.value=Id(219);
      inventory.inventory.entries.push_back(reader);inventory.inventory.next_local_transaction_id=20;cp.selected_local_transaction_id=19;
      const auto rewrite=[](auto& row,const auto& edit){auto metadata=catalog::DecodeCatalogMetadataVersion(row.cells[0].value.payload);Check(metadata.ok(),"decode common history fixture");
        metadata.record.record.header.row_uuid=row.row_uuid;metadata.record.creator_transaction_uuid=row.transaction_uuid;metadata.record.creator_local_transaction_id=row.local_transaction_id;edit(metadata.record);
        const auto encoded=catalog::EncodeCatalogMetadataVersion(metadata.record);Check(encoded.ok(),"encode common history fixture");row.cells[0].value.payload=encoded.bytes;};
      rewrite(images.leaves[0].body.rows[0],[](auto& metadata){metadata.schema_epoch=2;});
      const auto old=images.leaves[0].body.rows[0];auto& next=images.leaves[3].body.rows[0];next.row_uuid=old.row_uuid;next.row_version=2;next.previous_row_version=1;next.previous_version_uuid=old.version_uuid;
      next.transaction_uuid=writer.identity.transaction_uuid;next.local_transaction_id=15;images.nodes[6].cells[0].key.row_uuid=old.row_uuid.value;
      images.nodes[6].cells[0].key.encoded_key={61}; // Strictly above the retained key60 lower fence after changing its row tie-breaker.
      rewrite(next,[&](auto& metadata){metadata.record.header.object_uuid.value=Id(180);metadata.definition_version=2;metadata.schema_epoch=3;});const auto next_version=next.version_uuid;
      // A well-formed unrelated retained row is not a candidate of this index.
      auto extra=images.leaves[3].body.rows[1];extra.row_uuid.value=Id(242);extra.version_uuid=Id(243);extra.internal_row_ordinal=extra.stable_slot_id=3;
      rewrite(extra,[](auto& metadata){metadata.record.header.object_uuid.value=Id(244);});images.leaves[3].body.rows.push_back(extra);
      persist(false,15,15);loaded=read(budget);Check(loaded.ok(),"load multi-page catalog history and active exclusions error="+std::to_string(static_cast<unsigned>(loaded.error))+" relation="+std::to_string(static_cast<unsigned>(loaded.relation.error))+" tree="+std::to_string(static_cast<unsigned>(loaded.relation.tree_error)));CatalogTestPin history_pin(loaded.checkpoint.checkpoint_inventory.inventory,19);
      inventory.inventory.entries[2].state=mga::TransactionState::prepared;persist(false,15,15);loaded=read(budget);Check(loaded.ok(),"actual prepared catalog writer inventory");
      CatalogTestPin doubt_pin(loaded.checkpoint.checkpoint_inventory.inventory,19);const auto& excluded=doubt_pin.published.descriptor.in_doubt_excluded_local_transaction_ids;
      Check(std::find(excluded.begin(),excluded.end(),15)!=excluded.end(),"actual published in-doubt exclusion contains prepared writer");
      inventory.inventory.entries[2].state=mga::TransactionState::active;persist(false,15,15);
      const auto expected=[&](const auto& value,const auto& version,bool retirement=false,bool provisional=false){Check(value.ok()&&value.rows.size()==7,"one snapshot-selected version per actual candidate, unrelated retained row excluded");
        Check(std::is_sorted(value.rows.begin(),value.rows.end(),[](const auto& a,const auto& b){return a.metadata.record.header.row_uuid.value<b.metadata.record.header.row_uuid.value;}),"selected catalog rows have deterministic binary UUID order");
        const auto found=std::find_if(value.rows.begin(),value.rows.end(),[&](const auto& row){return row.metadata.record.header.row_uuid.value==old.row_uuid.value;});
        Check(found!=value.rows.end()&&found->version_uuid==version&&found->metadata.record.header.deleted==retirement&&found->provisional==provisional,"independent expected catalog version and retirement outcome");};
      selected=pinned(history_pin.pin,reader.identity);expected(selected,old.version_uuid);Check(std::any_of(selected.observations.begin(),selected.observations.end(),[](const auto& o){return o.decision==mga::VisibilityDecision::wait_for_transaction;}),"other writer wait observation retained");
      inventory.inventory.entries[2].state=mga::TransactionState::committed;inventory.inventory.entries[2].commit_sequence=4;inventory.inventory.next_commit_sequence=5;persist(false,19,19);
      selected=pinned(history_pin.pin,reader.identity);expected(selected,old.version_uuid);
      selected=pinned(doubt_pin.pin,reader.identity);expected(selected,old.version_uuid);
      loaded=read(budget);Check(loaded.ok(),"actual later committed catalog inventory");CatalogTestPin fresh_pin(loaded.checkpoint.checkpoint_inventory.inventory,19);
      selected=pinned(fresh_pin.pin,reader.identity);expected(selected,next_version);
      inventory.inventory.entries[2].state=mga::TransactionState::archived;inventory.inventory.entries[2].archived_from_state=mga::TransactionState::committed;persist(false,19,19);
      selected=pinned(history_pin.pin,reader.identity);expected(selected,old.version_uuid);selected=pinned(fresh_pin.pin,reader.identity);expected(selected,next_version);
      rewrite(images.leaves[3].body.rows[0],[](auto& metadata){metadata.record.header.deleted=true;metadata.retired_transaction_uuid=metadata.creator_transaction_uuid;metadata.lifecycle=catalog::CatalogObjectLifecycle::dropped;metadata.status=catalog::CatalogObjectStatus::retired;});persist(false,19,19);
      selected=pinned(history_pin.pin,reader.identity);expected(selected,old.version_uuid);selected=pinned(fresh_pin.pin,reader.identity);expected(selected,next_version,true);
      auto& owned=images.leaves[3].body.rows[0];owned.transaction_uuid=reader.identity.transaction_uuid;owned.local_transaction_id=19;rewrite(owned,[](auto& metadata){metadata.retired_transaction_uuid=metadata.creator_transaction_uuid;});persist(false,19,19);
      selected=pinned(history_pin.pin,reader.identity);expected(selected,next_version,true,true);
      for(const auto& row:selected.rows)if(row.version_uuid==next_version)Check(row.effective_lifecycle==catalog::CatalogObjectLifecycle::dropping&&row.effective_status==catalog::CatalogObjectStatus::proposed,"own retirement remains provisional dropping");
      rewrite(owned,[](auto& metadata){metadata.record.header.deleted=false;metadata.retired_transaction_uuid={};metadata.lifecycle=catalog::CatalogObjectLifecycle::active;metadata.status=catalog::CatalogObjectStatus::active;});persist(false,19,19);
      selected=pinned(history_pin.pin,reader.identity);expected(selected,next_version,false,true);
      for(const auto& row:selected.rows)if(row.version_uuid==next_version)Check(row.effective_lifecycle==catalog::CatalogObjectLifecycle::altering,"own successor is provisional altering");
      owned.transaction_uuid=writer.identity.transaction_uuid;owned.local_transaction_id=15;rewrite(owned,[](auto&){});const auto history_images=images;
      // Complete mutable schemas also preserve origin across different loaded
      // filespace pages, not just within a single native row page.
      const auto as_schema=[&](auto& row,const auto& origin,u64 origin_number){rewrite(row,[&](auto& metadata){
        catalog::CatalogSchemaDefinition d;d.schema_object_uuid=metadata.record.header.object_uuid;
        d.database_catalog_object_uuid={platform::UuidKind::object,Id(246)};
        d.parent_schema_uuid={platform::UuidKind::object,Id(164)};
        d.origin_transaction_uuid=origin;d.origin_local_transaction_id=origin_number;
        metadata.record.header.kind=catalog::CatalogRecordKind::schema;metadata.record.header.parent_uuid=d.parent_schema_uuid;
        metadata.default_name_uuid={platform::UuidKind::object,Id(248)};metadata.name_vector_uuid={platform::UuidKind::object,Id(249)};
        const auto e=catalog::EncodeCatalogSchemaDefinition(d);Check(e.ok(),"complete schema cross-page fixture");
        metadata.record.payload.assign(e.bytes.begin(),e.bytes.end());});};
      as_schema(images.leaves[0].body.rows[0],old.transaction_uuid,old.local_transaction_id);
      as_schema(images.leaves[3].body.rows[0],old.transaction_uuid,old.local_transaction_id);
      persist(false,19,19);selected=pinned(fresh_pin.pin,reader.identity);expected(selected,next_version);
      selected=pinned(history_pin.pin,reader.identity);expected(selected,old.version_uuid);
      as_schema(images.leaves[3].body.rows[0],writer.identity.transaction_uuid,15);
      persist(false,19,19);loaded=read(budget);Check(loaded.ok(),"origin corruption remains individually valid on separate pages");
      selected=pinned(fresh_pin.pin,reader.identity);no_rows(selected);Check(selected.error==PE::invalid_chain,"cross-page schema origin replacement refused");
      selected=pinned(history_pin.pin,reader.identity);no_rows(selected);Check(selected.error==PE::invalid_chain,"hidden cross-page schema origin replacement refused");
      images=history_images;
      // An unindexed row on another page still reserves its object identity.
      // Outcome comes from native inventory, not visibility or lifecycle text.
      const auto duplicate_inventory=inventory;
      for(const auto [state,origin]:std::vector<std::pair<mga::TransactionState,mga::TransactionState>>{
          {mga::TransactionState::active,mga::TransactionState::none},
          {mga::TransactionState::prepared,mga::TransactionState::none},
          {mga::TransactionState::committed,mga::TransactionState::none},
          {mga::TransactionState::rolled_back,mga::TransactionState::none},
          {mga::TransactionState::failed_terminal,mga::TransactionState::none},
          {mga::TransactionState::archived,mga::TransactionState::committed},
          {mga::TransactionState::archived,mga::TransactionState::rolled_back},
          {mga::TransactionState::archived,mga::TransactionState::failed_terminal}}) {
        images=history_images;inventory=duplicate_inventory;
        auto& competing=images.leaves[3].body.rows.back();
        competing.transaction_uuid=writer.identity.transaction_uuid;competing.local_transaction_id=15;
        rewrite(competing,[&](auto& metadata){metadata.record.header.object_uuid.value=Id(180);});
        auto& creator=inventory.inventory.entries[2];creator.state=state;creator.archived_from_state=origin;
        const auto outcome=state==mga::TransactionState::archived?origin:state;
        creator.commit_sequence=outcome==mga::TransactionState::committed?4:0;
        const bool pending=outcome==mga::TransactionState::active||outcome==mga::TransactionState::prepared;
        persist(false,pending||state==mga::TransactionState::failed_terminal?15:19,pending?15:19);
        loaded=read(budget);Check(loaded.ok(),"duplicate fixture has valid physical images and native inventory state="
          +std::to_string(static_cast<unsigned>(state))+" origin="+std::to_string(static_cast<unsigned>(origin))
          +" checkpoint="+std::to_string(static_cast<unsigned>(loaded.checkpoint.error))
          +" inventory="+std::to_string(static_cast<unsigned>(loaded.checkpoint.checkpoint_inventory.inventory_error)));
        selected=pinned(history_pin.pin,reader.identity);
        const bool released=outcome==mga::TransactionState::rolled_back||outcome==mga::TransactionState::failed_terminal;
        if(released)expected(selected,old.version_uuid);
        else {no_rows(selected);Check(selected.error==PE::duplicate_identity,"hidden cross-row object UUID collision cannot publish catalog rows");}
      }
      inventory=duplicate_inventory;images=history_images;
      for(unsigned fault=0;fault<6;++fault){images=history_images;auto& bad=images.leaves[3].body.rows[0];
        if(fault==0){bad.row_version=1;bad.previous_row_version=0;bad.previous_version_uuid={};}
        if(fault==1)rewrite(bad,[](auto& metadata){metadata.record.header.object_uuid.value=Id(245);});
        if(fault==2)bad.previous_version_uuid=images.leaves[0].body.rows[1].version_uuid;
        if(fault==3)rewrite(bad,[](auto& metadata){metadata.definition_version=1;});
        if(fault==4)rewrite(bad,[](auto& metadata){metadata.definition_version=3;});
        if(fault==5)rewrite(bad,[](auto& metadata){metadata.schema_epoch=1;});
        persist(false,19,19);selected=pinned(fresh_pin.pin,reader.identity);no_rows(selected);Check(selected.error==PE::invalid_chain,"hidden cross-page catalog identity/sequence mismatch");}
      images=history_images;images.leaves[0].body.rows.erase(images.leaves[0].body.rows.begin());images.leaves[0].body.rows[0].internal_row_ordinal=1;
      images.nodes[3].cells.erase(images.nodes[3].cells.begin(),images.nodes[3].cells.begin()+2);persist(false,19,19);selected=pinned(history_pin.pin,reader.identity);no_rows(selected);Check(selected.error==PE::missing_version,"missing excluded successor predecessor is not invented absence");
      images=history_images;inventory.inventory.entries[2].state=mga::TransactionState::limbo;inventory.inventory.entries[2].archived_from_state=mga::TransactionState::none;inventory.inventory.entries[2].commit_sequence=0;persist(false,15,15);
      selected=pinned(history_pin.pin,reader.identity);no_rows(selected);Check(selected.error==PE::requires_recovery,"traversed limbo metadata requires recovery without prefix");
      inventory.inventory.entries[2].state=mga::TransactionState::committed;inventory.inventory.entries[2].commit_sequence=4;persist(false,19,19);
      inventory.inventory.entries.back().state=mga::TransactionState::committed;inventory.inventory.entries.back().commit_sequence=5;inventory.inventory.next_commit_sequence=6;persist(false,20,20);
      selected=pinned(fresh_pin.pin,reader.identity);no_rows(selected);Check(selected.error==PE::reader_mismatch,"terminal actual reader cannot reuse a live pin");
      inventory.inventory.entries.back().state=mga::TransactionState::active;inventory.inventory.entries.back().commit_sequence=0;persist(false,19,19);
      reads=0;track_reads=true;selected=pinned(history_pin.pin,reader.identity);track_reads=false;const auto read_count=reads;expected(selected,old.version_uuid);
      for(unsigned fault=1;fault<=read_count;++fault){reads=0;read_fault=fault;track_reads=true;selected=pinned(history_pin.pin,reader.identity);track_reads=false;Check(!read_fault,"pinned physical read fault consumed");no_rows(selected);}
      observed_allocations=0;count_allocations=true;selected=pinned(history_pin.pin,reader.identity);count_allocations=false;const auto allocations=observed_allocations;expected(selected,old.version_uuid);bool success=false;
      std::cout<<"catalog_pinned_allocation_fault_sites="<<allocations<<std::endl;
      for(unsigned long n=0;n<=allocations;++n){allocation_budget=n;selected=pinned(history_pin.pin,reader.identity);allocation_budget=-1;if(selected.ok()){success=true;break;}no_rows(selected);}Check(success,"all pinned selection allocation failures are atomic");
      tree_read_paused=false;resume_tree_read=false;pause_next_tree_read=true;std::atomic<bool> done=false;db::NativePinnedCatalogReadResult revoked;
      std::thread reading([&]{revoked=pinned(history_pin.pin,reader.identity);done=true;});while(!tree_read_paused.load()&&!done.load())std::this_thread::yield();const bool paused=tree_read_paused.load();
      mga::RevokePublishedSnapshotVector(history_pin.published.descriptor.snapshot_uuid);resume_tree_read=true;reading.join();pause_next_tree_read=false;Check(paused,"physical pinned read paused before concurrent revoke");no_rows(revoked);Check(revoked.error==PE::snapshot_failure,"pin revoked during physical read cannot publish rows");
      inventory=saved_inventory;cp=saved_cp;images=original;
    }
    inventory.inventory.entries[1].state=mga::TransactionState::active;inventory.inventory.entries[1].commit_sequence=0;
    persist(false,13,13);Exclusive(first.path());Exclusive(second.path());Exclusive(base.path());Check(first.Close().ok()&&second.Close().ok()&&base.Close().ok(),"close before independent process");
    const auto child=::fork();Check(child>=0,"fork checkpoint relation reader");if(child==0){const auto profile=std::to_string(p);
      ::execl("/proc/self/exe","checkpoint-relation-probe","--checkpoint-relation-probe",fixture.root.c_str(),profile.c_str(),nullptr);::_exit(125);}
    int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh process follows checkpoint through relation creators");
  }
}
int main(int argc,char** argv) {
  if(argc==4&&std::string_view(argv[1])=="--checkpoint-relation-probe") {
    const auto p=static_cast<unsigned>(std::stoul(argv[3])),q=(p+1)%5,s=(p+2)%5;const std::filesystem::path dir=argv[2];disk::FileDevice first,second,base;
    if(!first.Open((dir/"index").string(),disk::FileOpenMode::open_existing_read_only).ok()||!second.Open((dir/"data").string(),disk::FileOpenMode::open_existing_read_only).ok()
      ||!base.Open((dir/"primary").string(),disk::FileOpenMode::open_existing_read_only).ok())return 2;
    auto cp=CheckpointExample(s);cp.header.filespace_uuid=Id(9);const auto expected=CatalogBindingImages(p);
    const auto r=db::ReadNativeCheckpointCatalogRelationFromOpenDevices(Id(1),{{Id(9),Profile(s),&base},{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}},CheckpointRef(cp),2,1,
      {Id(101),expected.nodes[0].dependencies},4*sizes[p]+4*sizes[q]+6*sizes[s]);
    if(!r.ok()||r.row_creators.size()!=8||r.navigation_creator_entries.size()!=7||r.relation.bindings.size()!=9)return 3;
    CatalogTestPin pin(r.checkpoint.checkpoint_inventory.inventory,13);
    const auto selected=db::ReadNativePinnedCatalogVersionsFromOpenDevices(Id(1),{{Id(9),Profile(s),&base},{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}},CheckpointRef(cp),2,1,
      {Id(101),expected.nodes[0].dependencies},r.checkpoint.checkpoint_inventory.inventory.entries[1].identity,pin.pin,4*sizes[p]+4*sizes[q]+6*sizes[s]);
    return selected.ok()&&selected.rows.size()==8&&std::all_of(selected.rows.begin(),selected.rows.end(),[](const auto& row){return row.provisional&&row.effective_lifecycle==catalog::CatalogObjectLifecycle::creating;})?0:4;
  }
  if(argc==4&&std::string_view(argv[1])=="--catalog-relation-probe") {
    const auto p=static_cast<unsigned>(std::stoul(argv[3])),q=(p+1)%5,s=(p+2)%5;const std::filesystem::path dir=argv[2];disk::FileDevice first,second,base;
    if(!first.Open((dir/"catalog-index").string(),disk::FileOpenMode::open_existing_read_only).ok()||!second.Open((dir/"catalog-data").string(),disk::FileOpenMode::open_existing_read_only).ok()
      ||!base.Open((dir/"catalog-primary").string(),disk::FileOpenMode::open_existing_read_only).ok())return 2;
    const auto expected=CatalogBindingImages(p);const auto& h=expected.nodes[0].header;
    const auto result=db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),{{Id(9),Profile(s),&base},{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}},
      {1,0x200,{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid},expected.nodes[0].dependencies.index_uuid},
      {Id(101),expected.nodes[0].dependencies},4*sizes[p]+4*sizes[q]+3*sizes[s]);
    if(!result.ok()||!result.index||result.bindings.size()!=9||result.catalogs.size()!=4)return 3;
    for(unsigned i=0;i<4;++i)if(result.catalogs[i].bytes!=LeafOracle(expected.leaves[i]))return 4;return 0;
  }
  if(argc==4&&(std::string_view(argv[1])=="--native-btree-tree-probe"||std::string_view(argv[1])=="--native-btree-deep-probe")) {
    const auto p=static_cast<unsigned>(std::stoul(argv[3])),q=(p+1)%5,s=(p+2)%5;const std::filesystem::path dir=argv[2];
    disk::FileDevice first,second,base;if(!first.Open((dir/"tree-first").string(),disk::FileOpenMode::open_existing_read_only).ok()
      ||!second.Open((dir/"tree-second").string(),disk::FileOpenMode::open_existing_read_only).ok()||!base.Open((dir/"tree-base").string(),disk::FileOpenMode::open_existing_read_only).ok())return 2;
    const bool deep=std::string_view(argv[1])=="--native-btree-deep-probe";
    const auto expected=deep?BtreeDeepTreeExample(p):BtreeTreeExample(p);const auto& h=expected[0].header;
    const auto result=page::ReadNativeBtreeTreeFromOpenDevices(Id(1),{{Id(9),Profile(s),&base},{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}},
      {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid},expected[0].dependencies,(deep?2:1)*(4*sizes[p]+3*sizes[q])+(deep?sizes[p]:0));
    const std::vector<std::size_t> leaf_order=deep?std::vector<std::size_t>{3,4,6,7,10,11,13,14}:std::vector<std::size_t>{2,3,5,6};
    if(!result.ok()||result.pages.size()!=expected.size()||result.leaves!=leaf_order)return 3;
    for(unsigned i=0;i<leaf_order.size();++i){const auto target=deep?(i<4?4+i:7+i):3+i;
      if(result.pages[result.leaves[i]].bytes!=BtreeOracle(expected[target]))return 4;}return 0;
  }
  if(argc==4&&std::string_view(argv[1])=="--native-btree-probe") {
    const auto p=static_cast<unsigned>(std::stoul(argv[3]));const auto expected=BtreeExample(p);disk::FileDevice device;
    if(!device.Open(argv[2],disk::FileOpenMode::open_existing_read_only).ok())return 2;
    const auto result=page::ReadNativeBtreePageFromOpenDevice(device,Id(1),{Id(2),12,102,Profile(p)},0x200,expected.dependencies);
    return result.ok()&&result.bytes==BtreeOracle(expected)?0:3;
  }
  if(argc==4&&std::string_view(argv[1])=="--checkpoint-catalog-probe") {
    const std::filesystem::path path=argv[2];const unsigned p=static_cast<unsigned>(std::stoul(argv[3])),q=(p+1)%5;
    disk::FileDevice first,second;
    if(!first.Open((path/"catalog-first").string(),disk::FileOpenMode::open_existing_read_only).ok()
      ||!second.Open((path/"catalog-second").string(),disk::FileOpenMode::open_existing_read_only).ok())return 2;
    const auto r=db::VerifyNativeCheckpointCatalogRootsFromOpenDevices(Id(1),{{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}},CheckpointRef(CheckpointExample(p)),3*sizes[p]+sizes[q]);
    return r.ok()&&r.catalogs.size()==2&&r.feature_root_index==1&&r.catalogs[0].root->creator_transaction_uuid==Id(91)
      &&r.catalogs[1].root->object_uuid==Id(43)&&!r.checkpoint_inventory.inventory.publication_base?0:3;
  }
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
  try { CheckpointCatalogRelations(); std::cout<<"checkpoint_catalog_checks="<<checks<<std::endl; NativeCatalogRelationBindings(); NativeBtreeTrees(); NativeBtreePages(); CanonicalCheckpointCatalogRoots(); CanonicalCheckpointHistory(); CanonicalCheckpoints(); CanonicalCheckpointFiles(); CanonicalCheckpointInventoryPair(); CanonicalInventoryImages(); CanonicalInventoryChains(); Codecs(); Files(); CatalogRoots(); CatalogRootFiles(); CatalogRootRanges(); CatalogLeaves(); CatalogLeafFiles();
    std::cout<<"PASS checks="<<checks<<" canonical_page_image_and_chain_only=true\n"; return 0; }
  catch(const std::exception& e) { allocation_budget=-1; std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n'; return 1; }
}

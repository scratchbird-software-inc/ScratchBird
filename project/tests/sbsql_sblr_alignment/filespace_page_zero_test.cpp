// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "filespace_page_zero.hpp"
#include "catalog_page.hpp"
#include "native_index_btree_page.hpp"
#include "physical_mga_cow_store.hpp"
#include "catalog_schema_definition.hpp"
#include "catalog_metric_retention_policy.hpp"
#include "transaction_inventory_page.hpp"
#include "database_dirty_manifest.hpp"
#include "native_checkpoint_selection.hpp"
#include "native_management_control_authority.hpp"
#include "native_filespace_initialization.hpp"
#include "disk_device.hpp"
#include "uuid.hpp"
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
#include <set>
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
unsigned stage_write_fault=0,stage_sync_fault=0,stage_writes=0,stage_syncs=0;
unsigned stage_write_fault_after=0,stage_sync_fault_after=0;
unsigned stage_corrupt_read=0;
unsigned initialization_entropy_fault=0,initialization_entropy_calls=0;
bool initialization_repeat_entropy=false;
const scratchbird::core::platform::TypedUuid* revoke_on_stage_sync=nullptr;
unsigned observed_full_digests=0;
bool count_full_digests=false;
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
extern "C" int __real_RAND_bytes(unsigned char*,int);
extern "C" int __wrap_RAND_bytes(unsigned char* out,int count) {
  ++initialization_entropy_calls;
  if(initialization_entropy_fault&&--initialization_entropy_fault==0)return 0;
  if(initialization_repeat_entropy){std::fill_n(out,count,0xab);return 1;}
  return __real_RAND_bytes(out,count);
}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* b,size_t n,unsigned char* out,unsigned int* count,const EVP_MD* md,ENGINE* e) {
  if(count_full_digests) ++observed_full_digests;
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
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_pwrite(int fd,const void* b,size_t n,off_t offset) {
  ++stage_writes;
  if(stage_write_fault&&(!stage_write_fault_after||--stage_write_fault_after==0)){const auto mode=stage_write_fault;stage_write_fault=0;
    if(mode==2&&n>1){const auto part=__real_pwrite(fd,b,n/2,offset);if(part<0)return part;}
    if(mode==3&&n>64){const auto part=__real_pwrite(fd,b,64,offset);if(part<0)return part;}
    errno=EIO;return -1;}
  return __real_pwrite(fd,b,n,offset);
}
extern "C" int __wrap_fsync(int fd) {
  ++stage_syncs;if(stage_sync_fault&&(!stage_sync_fault_after||--stage_sync_fault_after==0)){stage_sync_fault=0;errno=EIO;return -1;}
  const auto result=__real_fsync(fd);
  if(result==0&&revoke_on_stage_sync){const auto id=*revoke_on_stage_sync;revoke_on_stage_sync=nullptr;scratchbird::transaction::mga::RevokePublishedSnapshotVector(id);}
  return result;
}
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
  const auto result=__real_pread(fd,b,n,offset);
  if(stage_corrupt_read&&reads==stage_corrupt_read&&result>0){stage_corrupt_read=0;static_cast<unsigned char*>(b)[0]^=1;}
  return result;
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
  for(const auto& pair:{std::pair{1u,8u},std::pair{3u,9u},std::pair{7u,11u}}){nodes[pair.first].right=ref(pair.second);nodes[pair.second].left=ref(pair.first);
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
  for(unsigned p=0;p<5;++p) for(unsigned kind:{2u,6u,7u,8u}) {
    auto r=RootExample(p); r.root_kind=static_cast<disk::u16>(kind);
    if(kind!=2) {
      const auto target=r.roots[kind==6?4:kind==7?3:5];r.roots={target};
      r.header.page_type=kind==6?10:kind==7?11:5;
    }
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
  {auto r=RootExample(); r.root_kind=9; Check(!page::EncodeNativeCatalogRoot(r).ok(),"unknown kind producer");}
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
  return {root.root_kind,h.page_type,h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid,root.object_uuid};
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
  for(const auto& [budget,expected_reads]:std::array<std::pair<u64,unsigned>,3>{{{8191,4},{8192,7},{24576,10}}}) {
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
    page::RowDataRecord row;
    row.storage_generation = 1; row.row_uuid={platform::UuidKind::row,Id(static_cast<byte>(160+i))};
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
  const std::string_view cm="SBPGV002",rm="SBROW004",vm="SBDVAL01";
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
    PutUuid(b,at+104,row.previous_version_uuid); PutUuid(b,at+120,row.next_version_uuid);
    Number(b,at+136,8,row.storage_generation); at+=144;
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
  for(platform::u64 generation:{platform::u64{0},leaf.body.page_generation+1,
      std::numeric_limits<platform::u64>::max()}) {
    auto bad=leaf;bad.body.rows.back().storage_generation=generation;
    LeafReject(db::EncodeNativeCatalogLeaf(bad),Error::invalid_body);
    // Independent oracle refreshes all row, body and page integrity fields.
    LeafReject(db::DecodeNativeCatalogLeaf(LeafOracle(bad)),Error::invalid_body);
  }
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
// Spec-table oracle for fixtures that vary lifecycle/archived outcomes. Kept
// separate from the explicit-summary encoder used for malformed-summary tests.
Bytes InventoryStateOracle(const page::NativeTransactionInventoryPage& p) {
  auto oit=p.inventory.next_local_transaction_id,oat=oit;
  for(const auto& e:p.inventory.entries){
    const auto state=e.state==mga::TransactionState::archived?e.archived_from_state:e.state;
    const bool active=e.state==mga::TransactionState::active||e.state==mga::TransactionState::read_only_active;
    if(active)oat=std::min(oat,e.identity.local_id.value);
    for(auto hold:{mga::TransactionState::created,mga::TransactionState::active,mga::TransactionState::read_only_active,
        mga::TransactionState::preparing,mga::TransactionState::prepared,mga::TransactionState::committing,
        mga::TransactionState::rolling_back,mga::TransactionState::limbo,mga::TransactionState::recovering,mga::TransactionState::failed_terminal})
      if(state==hold)oit=std::min(oit,e.identity.local_id.value);
  }
  return InventoryOracle(p,oit,oat,oat);
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
    const auto bytes=InventoryOracle(archived,origin==mga::TransactionState::failed_terminal?1:3,3,3);const auto r=page::DecodeNativeTransactionInventoryPage(bytes);
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
    const bool operation=b[136]==2&&b[137]==0;
    const std::string_view domain=operation?"SBCPSET2":"SBCPSET1";Bytes material(domain.begin(),domain.end());
    material.insert(material.end(),b.begin()+160,b.begin()+256);
    if(operation)material.insert(material.end(),b.begin()+408,b.begin()+424);
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
  const bool operation=!r.creator_operation_uuid.is_nil();
  const std::string_view magic=operation?"SBCPNT02":"SBCPNT01";std::copy(magic.begin(),magic.end(),b.begin()+128);
  Number(b,136,2,operation?2:1);Number(b,138,2,384);Number(b,140,4,512+112*r.roots.size());PutUuid(b,144,r.object_uuid);
  Number(b,160,8,r.checkpoint_generation);Number(b,168,8,r.root_set_generation);Number(b,176,8,r.selected_local_transaction_id);
  Number(b,184,8,r.stable_local_transaction_id);Number(b,192,8,r.local_durable_transaction_id);Number(b,200,8,r.cluster_quorum_transaction_id);
  PutUuid(b,208,r.timeline_uuid);PutUuid(b,224,r.creator_transaction_uuid);Number(b,240,8,r.creator_local_transaction_id);Number(b,248,8,r.flags);
  const auto ref=[&](std::size_t at,const disk::NativePageReference& p){PutUuid(b,at,p.filespace_uuid);Number(b,at+16,8,p.page_number);Number(b,at+24,8,p.page_generation);PutUuid(b,at+32,p.page_size_profile_uuid);};
  if(r.predecessor)ref(256,*r.predecessor);std::copy(r.predecessor_sha256.begin(),r.predecessor_sha256.end(),b.begin()+304);Number(b,400,8,r.completed?1:0);
  PutUuid(b,408,r.creator_operation_uuid);
  for(std::size_t i=0;i<r.roots.size();++i){const auto& root=r.roots[i];const auto at=512+112*i;Number(b,at,2,root.role);Number(b,at+4,4,root.page_type);ref(at+8,root.page);PutUuid(b,at+56,root.object_uuid);std::copy(root.sha256.begin(),root.sha256.end(),b.begin()+at+72);}
  CheckpointSeal(b);return b;
}
void CheckpointReject(const db::NativeCheckpointRootResult& r) {Check(!r.ok()&&!r.root&&r.bytes.empty(),"checkpoint refuses without partial root");}
void CanonicalOperationCheckpoints() {
  using E=db::NativeCheckpointError;
  for(unsigned profile=0;profile<5;++profile){
    auto operation=CheckpointExample(profile);operation.creator_transaction_uuid={};
    operation.creator_local_transaction_id=0;operation.creator_operation_uuid=Id(201);
    for(bool completed:{false,true}){auto r=operation;r.completed=completed;
      const auto expected=CheckpointOracle(r);
      const auto encoded=db::EncodeNativeCheckpointRoot(r);
      Check(encoded.ok()&&encoded.bytes==expected,"independent operation-owned checkpoint bytes");
      const auto decoded=db::DecodeNativeCheckpointRoot(expected);
      Check(decoded.ok()&&decoded.root->creator_transaction_uuid.is_nil()&&decoded.root->creator_local_transaction_id==0&&
        decoded.root->creator_operation_uuid==r.creator_operation_uuid&&decoded.root->completed==completed&&
        db::EncodeNativeCheckpointRoot(*decoded.root).bytes==expected,"operation owner round trip without completion upgrade");
    }
    const auto good=CheckpointOracle(operation);
    Uuid nil{},v7=Id(202),v4=v7,bad_variant=v7;v4.bytes[6]=0x41;bad_variant.bytes[8]=0;
    const std::array<Uuid,4> ids{nil,v7,v4,bad_variant};
    for(unsigned tx=0;tx<4;++tx)for(unsigned op=0;op<4;++op)
      for(u64 local:{u64{0},operation.selected_local_transaction_id,operation.selected_local_transaction_id+1,std::numeric_limits<u64>::max()}){
        auto r=operation;r.creator_transaction_uuid=ids[tx];r.creator_operation_uuid=ids[op];r.creator_local_transaction_id=local;
        const bool valid=(tx==1&&op==0&&local>0&&local<=r.selected_local_transaction_id)||(tx==0&&op==1&&local==0);
        const auto encoded=db::EncodeNativeCheckpointRoot(r),decoded=db::DecodeNativeCheckpointRoot(CheckpointOracle(r));
        Check(encoded.ok()==valid&&decoded.ok()==valid,"exclusive checkpoint creator truth table");
        if(!valid){CheckpointReject(encoded);CheckpointReject(decoded);}
      }
    for(std::size_t at=424;at<512;++at){auto bad=good;bad[at]=1;CheckpointSeal(bad,false);CheckpointReject(db::DecodeNativeCheckpointRoot(bad));}
    auto changed=good;PutUuid(changed,408,Id(203));CheckpointSeal(changed,false);
    auto result=db::DecodeNativeCheckpointRoot(changed);CheckpointReject(result);
    Check(result.error==E::invalid_integrity,"operation UUID is bound inside root-set digest, not just outer seal");
    CheckpointSeal(changed);Check(db::DecodeNativeCheckpointRoot(changed).ok(),"fully resealed alternate owner is structural data only");
    for(unsigned fault=0;fault<5;++fault){auto bad=good;
      if(fault==0)bad[135]='1';if(fault==1)Number(bad,136,2,1);
      if(fault==2){bad[135]='1';Number(bad,136,2,1);}
      if(fault==3)Number(bad,136,2,3);
      if(fault==4){bad=CheckpointOracle(CheckpointExample(profile));bad[135]='2';Number(bad,136,2,2);}
      CheckpointSeal(bad);CheckpointReject(db::DecodeNativeCheckpointRoot(bad));
    }
    for(unsigned prior_owner=0;prior_owner<2;++prior_owner){auto prior=CheckpointExample(profile);
      if(prior_owner)prior=operation;auto next=operation;next.checkpoint_generation=2;next.root_set_generation++;
      next.header.page_number=20;next.header.page_generation++;next.header.page_uuid=Id(204);
      next.predecessor=disk::NativePageReference{prior.header.filespace_uuid,prior.header.page_number,prior.header.page_generation,prior.header.page_size_profile_uuid};
      const auto before=CheckpointOracle(prior);Check(SHA256(before.data(),before.size(),next.predecessor_sha256.data())!=nullptr,"independent mixed-owner predecessor hash");
      Check(db::EncodeNativeCheckpointRoot(next).bytes==CheckpointOracle(next),"operation checkpoint retains versioned predecessor reference");
    }
    for(unsigned mode=0;mode<2;++mode){bool success=false;
      for(long n=0;n<100;++n){allocation_budget=n;const auto r=mode?db::DecodeNativeCheckpointRoot(good):db::EncodeNativeCheckpointRoot(operation);allocation_budget=-1;
        if(r.ok()){success=true;break;}CheckpointReject(r);Check(r.error==E::resource_exhausted,"operation checkpoint allocation fault");}
      Check(success,"operation checkpoint allocation sweep terminates");
      for(unsigned fault=1;fault<=5;++fault){hash_fault=fault;
        const auto r=mode?db::DecodeNativeCheckpointRoot(good):db::EncodeNativeCheckpointRoot(operation);
        Check(hash_fault==0&&r.error==E::hash_failure,"operation checkpoint hash-provider fault");CheckpointReject(r);}
    }
  }
}
void CanonicalCheckpoints() {
  using E=db::NativeCheckpointError;
  CanonicalOperationCheckpoints();
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
  for(unsigned profile=0;profile<5;++profile) for(unsigned owner=0;owner<2;++owner) {
    auto r=CheckpointExample(profile);if(owner){r.creator_transaction_uuid={};r.creator_local_transaction_id=0;r.creator_operation_uuid=Id(201);}
    const auto image=CheckpointOracle(r);auto z=Example(profile);const auto zero=Oracle(z);
    disk::FileDevice device;const auto path=(fixture.root/("checkpoint-"+std::to_string(profile)+"-"+std::to_string(owner))).string();
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
  const auto persist=[&](const auto& actual_inventory,auto actual_checkpoint,u64 horizon,u64 active_horizon=0) {
    const auto oat=active_horizon?active_horizon:horizon;
    const auto image=InventoryOracle(actual_inventory,horizon,oat,oat);
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
  result=read(16383);empty(result);Check(result.error==E::resource_exhausted
    &&result.inventory_error==page::NativeInventoryError::resource_exhausted,"pair budget includes checkpoint and chain");
  for(auto state:{mga::TransactionState::active,mga::TransactionState::prepared,mga::TransactionState::limbo,
      mga::TransactionState::failed_terminal,mga::TransactionState::rolled_back}) {
    auto bad=inventory;bad.inventory.entries[0].state=state;bad.inventory.entries[0].commit_sequence=0;
    const auto horizon=state==mga::TransactionState::rolled_back?18:17;
    auto image=InventoryOracle(bad,horizon,state==mga::TransactionState::active?17:18,state==mga::TransactionState::active?17:18);
    auto root=checkpoint;Check(SHA256(image.data(),image.size(),root.roots.front().sha256.data())!=nullptr,"reseal actual noncommitted inventory");const auto bytes=CheckpointOracle(root);
    Check(device.WriteAt(14*sizes[0],image.data(),image.size()).ok()&&device.WriteAt(19*sizes[0],bytes.data(),bytes.size()).ok()&&device.Sync().ok(),"persist noncommitted creator case");
    result=read();empty(result);Check(result.error==E::creator_not_committed,"noncommitted creator cannot certify checkpoint inventory");
  }
  for(auto origin:{mga::TransactionState::committed,mga::TransactionState::rolled_back,mga::TransactionState::failed_terminal}) {
    auto archived=inventory;auto& e=archived.inventory.entries[0];e.state=mga::TransactionState::archived;e.archived_from_state=origin;
    if(origin!=mga::TransactionState::committed)e.commit_sequence=0;
    persist(archived,checkpoint,origin==mga::TransactionState::failed_terminal?17:18,18);result=read();
    if(origin==mga::TransactionState::committed)Check(result.ok()&&result.inventory.entries[0].archived_from_state==origin,"exact archived committed creator admitted");
    else{empty(result);Check(result.error==E::creator_not_committed,"archive location is not checkpoint commit authority");}
  }
  auto mismatch=checkpoint;mismatch.creator_transaction_uuid=Id(199);persist(inventory,mismatch,18);result=read();empty(result);Check(result.error==E::inventory_mismatch,"binary checkpoint creator UUID mismatch");
  auto operation=checkpoint;operation.creator_transaction_uuid={};operation.creator_local_transaction_id=0;operation.creator_operation_uuid=Id(201);
  const auto operation_bytes=persist(inventory,operation,18);
  Check(db::DecodeNativeCheckpointRoot(operation_bytes).ok(),"operation-owned checkpoint is structurally valid");
  result=read(128*sizes[0]);empty(result);Check(result.error==E::creator_not_committed,"inventory outcome cannot substitute for actual selected operation authority");
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
    result=read(2*sizes[p]-1);empty(result);Check(result.error==E::resource_exhausted
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
      const u64 oat=state==mga::TransactionState::active?11:18;
      persist(inv,catalog,feature,false,oit,oat);result=read(distinct_budget);empty(result);
      Check(result.error==E::catalog_creator_not_committed,"uncommitted catalog creator cannot borrow committed checkpoint authority");
    }
    for(auto origin:{mga::TransactionState::committed,mga::TransactionState::rolled_back,mga::TransactionState::failed_terminal}) {
      auto inv=inventory;auto& e=inv.inventory.entries[0];e.state=mga::TransactionState::archived;e.archived_from_state=origin;
      if(origin!=mga::TransactionState::committed)e.commit_sequence=0;
      persist(inv,catalog,feature,false,origin==mga::TransactionState::failed_terminal?11:18,18);result=read(distinct_budget);
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
Bytes AllocationOracle(const page::NativeAllocationMap& map) {
  auto common=RootExample();common.header=map.header;
  auto b=RootOracle(common);std::fill(b.begin()+128,b.end(),0);
  unsigned version=map.creator_operation_uuid.is_nil()?1:2;
  for(const auto& r:map.records)if(!r.creator_operation_uuid.is_nil())version=2;
  std::copy_n(version==1?"SBABM001":"SBABM002",8,b.begin()+128);Number(b,136,2,version);Number(b,138,2,256);
  const auto bitmap=(map.states.size()+1)/2,at=(384+bitmap+7)&~std::size_t(7);
  Number(b,140,4,at+128*map.records.size());PutUuid(b,144,map.object_uuid);
  Number(b,160,8,map.map_generation);Number(b,168,8,map.capacity_generation);
  Number(b,176,8,map.total_pages);Number(b,184,8,map.first_page);Number(b,192,8,map.states.size());
  PutUuid(b,200,map.creator_transaction_uuid);Number(b,216,8,map.creator_local_transaction_id);
  if(map.next){PutUuid(b,224,map.next->filespace_uuid);Number(b,240,8,map.next->page_number);
    Number(b,248,8,map.next->page_generation);PutUuid(b,256,map.next->page_size_profile_uuid);}
  std::copy(map.next_sha256.begin(),map.next_sha256.end(),b.begin()+272);
  Number(b,304,4,bitmap);Number(b,308,4,map.records.size());
  PutUuid(b,344,map.creator_operation_uuid);
  for(std::size_t i=0;i<map.states.size();++i)b[384+i/2]|=static_cast<byte>(map.states[i])<<(4*(i%2));
  for(std::size_t i=0;i<map.records.size();++i){const auto& r=map.records[i];const auto pos=at+128*i;
    Number(b,pos,8,r.page_number);PutUuid(b,pos+8,r.allocation_uuid);PutUuid(b,pos+24,r.page_uuid);
    PutUuid(b,pos+40,r.owner_uuid);PutUuid(b,pos+56,r.creator_transaction_uuid);
    Number(b,pos+72,8,r.creator_local_transaction_id);Number(b,pos+80,8,r.page_generation);
    Number(b,pos+88,8,r.reuse_horizon);Number(b,pos+96,4,r.page_type);PutUuid(b,pos+100,r.creator_operation_uuid);}
  Check(SHA256(b.data(),b.size(),b.data()+312)!=nullptr,"independent allocation image seal");return b;
}
constexpr std::array<u64,5> initialization_coverage{{60,124,252,507,1017}};
db::NativeFilespaceInitializationRequest InitializationRequest(unsigned p=0,unsigned role=1,u64 total=5,unsigned flags=0){
  db::NativeFilespaceInitializationRequest r;r.bootstrap=Example(p,role,7,flags).bootstrap;
  r.operation_uuid=Id(5);r.writer_uuid=Id(6);r.creator={{1},{scratchbird::core::platform::UuidKind::transaction,Id(90)},mga::TransactionScope::local_node};
  r.creation_utc_millis=1790000000000ULL;r.total_pages=total;return r;
}
void VerifyInitializedFilespace(disk::FileDevice& device,const db::NativeFilespaceInitializationRequest& request,
    unsigned p,const db::NativeFilespaceInitializationReceipt* receipt=nullptr){
  const u64 size=sizes[p],coverage=initialization_coverage[p];const auto total=request.total_pages;
  const u64 count=(total+coverage-1)/coverage,budget=(count+2)*size;
  const auto z=disk::ReadFilespacePageZeroFromOpenDevice(device);Check(z.ok(),"actual initialized page zero");
  const auto& zero=*z.record;const auto& b=request.bootstrap;
  Check(device.Size().size_bytes==total*size&&zero.bootstrap.database_uuid==b.database_uuid&&zero.bootstrap.filespace_uuid==b.filespace_uuid&&
    zero.bootstrap.page_size_profile_uuid==b.page_size_profile_uuid&&zero.bootstrap.page_size_bytes==size&&zero.bootstrap.filespace_role==b.filespace_role&&
    zero.bootstrap.flags==b.flags&&zero.bootstrap.encryption_profile_uuid==b.encryption_profile_uuid&&zero.bootstrap.checksum_profile_uuid==b.checksum_profile_uuid&&
    zero.bootstrap.lifecycle_state==7&&zero.page_generation==1&&zero.root_set_generation==1&&zero.total_pages==total&&zero.free_pages==total-count-1&&
    zero.preallocated_pages==0&&zero.creation_utc_millis==request.creation_utc_millis&&zero.creation_operation_uuid==request.operation_uuid&&
    zero.writer_identity_uuid==request.writer_uuid&&zero.roots.size()==1,"exact non-serving bootstrap, capacity and creation lineage");
  const auto& root=zero.roots.front();Check(root.kind==3&&root.page_type==3&&root.filespace_uuid==b.filespace_uuid&&root.page_number==1&&
    root.page_generation==1&&root.page_size_profile_uuid==b.page_size_profile_uuid,"sole initial allocation root");
  const auto chain=page::ReadNativeAllocationChainFromOpenDevice(device,{b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid},budget);
  Check(chain.ok()&&chain.pages.size()==count&&chain.state_counts[0]==total-count-1&&chain.state_counts[2]==count+1,"actual complete initial allocation chain");
  std::set<Uuid> ids{b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid,b.checksum_profile_uuid,b.encryption_profile_uuid,
    request.operation_uuid,request.writer_uuid,request.creator.transaction_uuid.value};
  const auto fresh=[&](const Uuid& id){u64 timestamp=0;for(unsigned i=0;i<6;++i)timestamp=(timestamp<<8)|id.bytes[i];
    Check((id.bytes[6]>>4)==7&&(id.bytes[8]&0xc0)==0x80&&timestamp==request.creation_utc_millis&&ids.insert(id).second,"fresh distinct binary UUIDv7 identity");};
  fresh(zero.page_uuid);fresh(root.object_uuid);
  for(const auto& image:chain.pages)fresh(image.map->header.page_uuid);
  Bytes actual(size);Check(device.ReadAt(0,actual.data(),actual.size()).ok()&&actual==Oracle(zero),"independent exact initialized page-zero image");
  for(u64 i=0;i<count;++i){const auto& map=*chain.pages[i].map;const auto& h=map.header;const auto first=i*coverage,covered=std::min(coverage,total-first);
    Check(h.page_size_bytes==size&&h.page_type==3&&h.database_uuid==b.database_uuid&&h.filespace_uuid==b.filespace_uuid&&h.page_number==i+1&&
      h.page_generation==1&&h.flags==0&&h.page_size_profile_uuid==b.page_size_profile_uuid&&map.object_uuid==root.object_uuid&&map.map_generation==1&&
      map.capacity_generation==1&&map.total_pages==total&&map.first_page==first&&map.states.size()==covered&&map.creator_transaction_uuid==request.creator.transaction_uuid.value&&
      map.creator_local_transaction_id==request.creator.local_id.value,"exact initialized map identity, creator and independently specified coverage");
    Check(map.next.has_value()==(i+1<count),"complete map chain terminality");
    if(map.next){const auto& next=*map.next;Check(next.filespace_uuid==b.filespace_uuid&&next.page_number==i+2&&next.page_generation==1&&
      next.page_size_profile_uuid==b.page_size_profile_uuid&&map.next_sha256==WholeRootHash(chain.pages[i+1].bytes),"exact actual map successor reference and whole-image hash");}
    else Check(map.next_sha256==std::array<byte,32>{},"terminal map has no hidden link digest");
    std::size_t records=0;for(u64 n=first;n<first+covered;++n){Check(map.states[n-first]==(n<=count?page::NativeAllocationState::allocated:page::NativeAllocationState::free),"only actual control pages allocated");
      if(n>count)continue;Check(records<map.records.size(),"every control page has allocation identity");const auto& r=map.records[records++];fresh(r.allocation_uuid);
      Check(r.page_number==n&&r.page_uuid==(n?chain.pages[n-1].map->header.page_uuid:zero.page_uuid)&&r.owner_uuid==(n?root.object_uuid:b.filespace_uuid)&&
        r.creator_transaction_uuid==request.creator.transaction_uuid.value&&r.creator_local_transaction_id==request.creator.local_id.value&&r.page_generation==1&&
        r.reuse_horizon==0&&r.page_type==(n?3u:b.filespace_role<=4?1u:2u),"exact physical control-page allocation and pending creation lineage");}
    Check(records==map.records.size(),"free pages carry no fabricated allocations");
    Check(device.ReadAt((i+1)*size,actual.data(),actual.size()).ok()&&actual==AllocationOracle(map)&&actual==chain.pages[i].bytes,"independent complete initialized map image");
  }
  for(u64 n=count+1;n<total;++n)Check(device.ReadAt(n*size,actual.data(),actual.size()).ok()&&std::all_of(actual.begin(),actual.end(),[](byte v){return !v;}),"every actual free physical page is zero");
  if(receipt)Check(receipt->database_uuid==b.database_uuid&&receipt->filespace_uuid==b.filespace_uuid&&receipt->page_zero_uuid==zero.page_uuid&&
    receipt->map_uuid==root.object_uuid&&receipt->operation_uuid==request.operation_uuid&&receipt->writer_uuid==request.writer_uuid&&
    receipt->creator.transaction_uuid.value==request.creator.transaction_uuid.value&&receipt->creator.local_id.value==request.creator.local_id.value&&
    receipt->creator.scope==request.creator.scope&&receipt->allocation_root.kind==3&&receipt->allocation_root.page_type==3&&
    receipt->allocation_root.filespace_uuid==b.filespace_uuid&&receipt->allocation_root.page_number==1&&receipt->allocation_root.page_generation==1&&
    receipt->allocation_root.page_size_profile_uuid==b.page_size_profile_uuid&&receipt->allocation_root.object_uuid==root.object_uuid&&
    receipt->page_zero_sha256==WholeRootHash(Oracle(zero))&&receipt->allocation_head_sha256==WholeRootHash(chain.pages.front().bytes)&&
    receipt->total_pages==total&&receipt->free_pages==total-count-1&&receipt->map_pages==count,"receipt derives from actual complete initialized storage");
}
void CanonicalFilespaceInitialization(){using E=db::NativeFilespaceInitializationError;
  for(unsigned p=0;p<5;++p)for(unsigned role=1;role<=15;++role){Fixture fixture;disk::FileDevice device;
    const auto path=(fixture.root/"initializing").string();auto request=InitializationRequest(p,role==15?1:role,role==1?initialization_coverage[p]+1:5,role==15?1:0);
    const u64 count=(request.total_pages+initialization_coverage[p]-1)/initialization_coverage[p],budget=(count+2)*sizes[p];
    Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"exclusively own empty initialization device");
    auto result=db::InitializeNativeFilespaceOnOpenDevice(device,request,budget-1);
    Check(!result.ok()&&!result.receipt&&result.error==E::resource_exhausted&&device.Size().size_bytes==0,"one-byte image-budget deficit cannot write");
    result=db::InitializeNativeFilespaceOnOpenDevice(device,request,budget);Check(result.ok(),"real initializing filespace writer");
    VerifyInitializedFilespace(device,request,p,&*result.receipt);
    const auto prior=disk::ReadFilespacePageZeroFromOpenDevice(device);result=db::InitializeNativeFilespaceOnOpenDevice(device,request,budget);
    Check(!result.ok()&&!result.receipt&&result.error==E::device_not_empty&&Oracle(*disk::ReadFilespacePageZeroFromOpenDevice(device).record)==Oracle(*prior.record),"nonempty initializing device cannot be reinterpreted or retried");
    if(role==1){Exclusive(path);Check(device.Close().ok(),"release initialization owner before fresh reader");const auto child=::fork();Check(child>=0,"fork initialized filespace reader");
      if(child==0){const auto profile=std::to_string(p),total=std::to_string(request.total_pages);::execl("/proc/self/exe","filespace-initialization-probe","--filespace-initialization-probe",path.c_str(),profile.c_str(),total.c_str(),nullptr);::_exit(125);}
      int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh process verifies real initialization without creator caches");}
  }
  Fixture fixture;disk::FileDevice device;unsigned attempt=0;auto request=InitializationRequest();const u64 budget=3*sizes[0];std::string path;
  const auto reset=[&](){if(device.is_open())Check(device.Close().ok(),"close owned initialization fault fixture");path=(fixture.root/std::to_string(attempt++)).string();
    Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"new empty initialization fault fixture");};
  const auto initialize=[&](){return db::InitializeNativeFilespaceOnOpenDevice(device,request,budget);};
  const auto empty=[&](const auto& r){Check(!r.ok()&&!r.receipt,"initialization failure never returns a receipt");};
  const auto image=[&](){const auto size=device.Size();Check(size.ok(),"read actual interrupted extent");Bytes bytes(size.size_bytes);if(!bytes.empty())Check(device.ReadAt(0,bytes.data(),bytes.size()).ok(),"read actual interrupted bytes");return bytes;};
  const auto preserved=[&](){const auto before=image();if(!before.empty()){const auto again=initialize();empty(again);Check(again.error==E::device_not_empty&&image()==before,"interrupted owned bytes preserved without implicit repair");}};
  reset();initialization_entropy_calls=stage_writes=stage_syncs=reads=observed_full_digests=observed_allocations=0;
  track_reads=count_full_digests=count_allocations=true;auto result=initialize();track_reads=count_full_digests=count_allocations=false;
  Check(result.ok(),"cold initialization succeeds");const auto entropy_count=initialization_entropy_calls,write_count=stage_writes,read_count=reads,digest_count=observed_full_digests;
  const auto allocation_count=observed_allocations;Check(entropy_count==5&&write_count==7&&stage_syncs==1,"all identities issued before exact physical writes and sync");
  for(unsigned fault=1;fault<=entropy_count;++fault){reset();initialization_entropy_fault=fault;result=initialize();Check(!initialization_entropy_fault,"each entropy failure consumed");empty(result);
    Check(result.error==E::identity_failure&&device.Size().size_bytes==0,"no generated substitute after randomness failure");}
  reset();initialization_repeat_entropy=true;result=initialize();initialization_repeat_entropy=false;empty(result);Check(result.error==E::identity_failure&&device.Size().size_bytes==0,"duplicate generated UUIDs refuse before writes");
  for(unsigned fault=1;fault<=write_count;++fault)for(unsigned mode:{1u,3u}){reset();stage_write_fault=mode;stage_write_fault_after=fault;result=initialize();
    Check(!stage_write_fault&&!stage_write_fault_after,"every early and late physical write fault consumed");empty(result);Check(result.error==E::io_failure,"write failure classified");preserved();}
  reset();stage_sync_fault=1;result=initialize();Check(!stage_sync_fault,"initialization sync fault consumed");empty(result);preserved();
  for(unsigned fault=1;fault<=read_count;++fault){reset();reads=0;read_fault=fault;track_reads=true;result=initialize();track_reads=false;Check(!read_fault,"every initialization read failure consumed");empty(result);preserved();}
  for(unsigned fault=1;fault<=read_count;++fault){reset();reads=0;stage_corrupt_read=fault;track_reads=true;result=initialize();track_reads=false;Check(!stage_corrupt_read,"every final verification read corruption consumed");empty(result);preserved();}
  for(unsigned fault=1;fault<=digest_count;++fault){reset();full_digest_fault=fault;result=initialize();Check(!full_digest_fault,"every initialization full hash failure consumed");empty(result);preserved();}
  for(unsigned fault=1;fault<=5;++fault){reset();hash_fault=fault;result=initialize();Check(!hash_fault,"every multipart hash failure consumed");empty(result);preserved();}
  bool allocation_end=false;unsigned long consumed=0;
  for(unsigned long fault=0;fault<=allocation_count;++fault){reset();allocation_budget=static_cast<long>(fault);result=initialize();const bool injected=allocation_budget<0;allocation_budget=-1;
    if(!injected){Check(result.ok(),"uninjected allocation sweep termination succeeds");VerifyInitializedFilespace(device,request,0,&*result.receipt);allocation_end=true;break;}
    ++consumed;empty(result);preserved();}
  Check(allocation_end,"all reachable allocation failure positions exercised");
  std::cout<<"initialization fault sites: allocations="<<consumed<<" entropy="<<entropy_count<<" writes="<<write_count<<" reads="<<read_count<<" full_hashes="<<digest_count<<std::endl;
  for(unsigned fault=0;fault<17;++fault){reset();auto changed=request;
    if(fault==0)changed.bootstrap.lifecycle_state=1;if(fault==1)changed.bootstrap.page_size_bytes=4096;
    if(fault==2)changed.bootstrap.database_uuid={};if(fault==3)changed.bootstrap.filespace_uuid={};if(fault==4)changed.bootstrap.page_size_profile_uuid=Id(91);
    if(fault==5)changed.operation_uuid={};if(fault==6)changed.writer_uuid.bytes[6]=0x41;if(fault==7)changed.creator.transaction_uuid.value={};
    if(fault==8)changed.creator.transaction_uuid.kind=scratchbird::core::platform::UuidKind::object;if(fault==9)changed.creator.local_id.value=0;
    if(fault==10)changed.creator.scope=mga::TransactionScope::cluster_global;if(fault==11)changed.creation_utc_millis=0;if(fault==12)changed.creation_utc_millis=0x1000000000000ULL;
    if(fault==13)changed.total_pages=0;if(fault==14)changed.total_pages=1;if(fault==15)changed.total_pages=std::numeric_limits<u64>::max();
    if(fault==16)changed.bootstrap.flags|=disk::FilespaceBootstrapFlag::cluster_authority_required;
    result=db::InitializeNativeFilespaceOnOpenDevice(device,changed,budget);empty(result);Check(device.Size().size_bytes==0,"invalid input preserves empty owned device");
    if(fault==16)Check(result.error==E::cluster_requires_authority,"cluster initialization stays at provider boundary");}
  reset();Check(device.Close().ok()&&device.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"empty read-only fixture");result=initialize();empty(result);Check(result.error==E::invalid_device,"read-only initialization refused");
  Check(device.Close().ok(),"closed initialization fixture");result=initialize();empty(result);Check(result.error==E::invalid_device,"closed initialization refused");
  reset();const byte unknown=0xa5;Check(device.WriteAt(0,&unknown,1).ok(),"one unknown existing byte");result=initialize();empty(result);Check(result.error==E::device_not_empty&&image()==Bytes{unknown},"unknown bytes are never truncated or converted");
  reset();pause_next_tree_read=true;tree_read_paused=false;resume_tree_read=false;std::atomic<bool> second_started=false,second_finished=false;
  db::NativeFilespaceInitializationResult first,second;
  std::thread a([&](){first=initialize();});while(!tree_read_paused.load())std::this_thread::yield();
  std::thread b([&](){second_started=true;second=initialize();second_finished=true;});while(!second_started.load())std::this_thread::yield();
  const bool was_blocked=!second_finished.load();resume_tree_read=true;a.join();b.join();
  Check(was_blocked&&first.ok()&&!second.ok()&&!second.receipt&&second.error==E::device_not_empty,"retained guard excludes concurrent initialization through final readback");VerifyInitializedFilespace(device,request,0,&*first.receipt);
  reset();request.total_pages=2;result=initialize();Check(result.ok()&&result.receipt->free_pages==0,"minimum complete physical capacity initializes without free pages");VerifyInitializedFilespace(device,request,0,&*result.receipt);
  request.total_pages=initialization_coverage[0]+1;
  for(unsigned fault:{1u,61u,62u,63u,64u}){reset();stage_write_fault=3;stage_write_fault_after=fault;
    result=db::InitializeNativeFilespaceOnOpenDevice(device,request,4*sizes[0]);empty(result);
    Check(!stage_write_fault&&!stage_write_fault_after,"multi-map zeroing, first and second map, and final page-zero write faults consumed");preserved();}
  reset();request.total_pages=initialization_coverage[0]*initialization_coverage[0]+1;
  result=db::InitializeNativeFilespaceOnOpenDevice(device,request,63*sizes[0]);Check(result.ok()&&result.receipt->map_pages==61,"control pages can span allocation coverage ranges");VerifyInitializedFilespace(device,request,0,&*result.receipt);
}
void CanonicalCheckpointAllocation() {
  using E=db::NativeCheckpointError;using S=page::NativeAllocationState;
  for(unsigned profile=0;profile<5;++profile){Fixture fixture;disk::FileDevice device;
    const auto path=(fixture.root/"checkpoint-allocation").string();auto zero=Example(profile);
    zero.free_pages=zero.preallocated_pages=0;
    auto inventory=InventoryExample(profile);inventory.inventory.next_local_transaction_id=18;
    inventory.inventory.next_commit_sequence=2;
    auto& original=inventory.inventory.entries.front();original.identity.local_id=mga::MakeLocalTransactionId(16);
    original.identity.transaction_uuid.value=Id(99);
    auto creator=original;creator.identity.local_id=mga::MakeLocalTransactionId(17);creator.identity.transaction_uuid.value=Id(98);
    creator.state=mga::TransactionState::committed;creator.commit_sequence=1;inventory.inventory.entries.push_back(creator);
    auto checkpoint=CheckpointExample(profile);
    page::NativeAllocationMap map;map.header={sizes[profile],3,Id(1),Id(2),Id(70),13,103,0,Profile(profile)};
    map.object_uuid=Id(43);map.map_generation=5;map.capacity_generation=6;map.total_pages=64;
    map.creator_transaction_uuid=Id(98);map.creator_local_transaction_id=17;map.states.assign(64,S::quarantined);
    for(unsigned number:{0u,13u,14u,19u}){
      map.states[number]=S::allocated;page::NativeAllocationRecord r;
      r.page_number=number;r.allocation_uuid=Id(static_cast<byte>(120+number));
      r.creator_transaction_uuid=Id(99);r.creator_local_transaction_id=16;
      if(number==0){r.page_uuid=zero.page_uuid;r.page_generation=zero.page_generation;r.page_type=1;r.owner_uuid=Id(2);}
      if(number==13){r.page_uuid=map.header.page_uuid;r.page_generation=map.header.page_generation;r.page_type=3;r.owner_uuid=map.object_uuid;}
      if(number==14){r.page_uuid=inventory.header.page_uuid;r.page_generation=inventory.header.page_generation;r.page_type=0x301;r.owner_uuid=inventory.object_uuid;}
      if(number==19){r.page_uuid=checkpoint.header.page_uuid;r.page_generation=checkpoint.header.page_generation;r.page_type=0x300;r.owner_uuid=checkpoint.object_uuid;}
      map.records.push_back(r);
    }
    Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"own checkpoint allocation fixture");
    const byte padding=0;Check(device.WriteAt(64*sizes[profile]-1,&padding,1).ok(),"actual checkpoint allocation extent");
    const auto persist=[&](const auto& inv,const auto& allocation,const auto& z,auto cp,u64 oit=16,u64 oat=16,bool stale_digest=false){
      const auto inv_bytes=InventoryOracle(inv,oit,oat,oat),map_bytes=AllocationOracle(allocation),zero_bytes=Oracle(z);
      cp.roots[0].page=InventoryRef(inv);cp.roots[0].object_uuid=inv.object_uuid;
      Check(SHA256(inv_bytes.data(),inv_bytes.size(),cp.roots[0].sha256.data())!=nullptr,"independent inventory binding");
      auto& target=cp.roots[3];target.page={Id(2),13,103,Profile(profile)};target.object_uuid=Id(43);
      const auto expected=stale_digest?AllocationOracle(map):map_bytes;
      Check(SHA256(expected.data(),expected.size(),target.sha256.data())!=nullptr,"independent allocation head binding");
      const auto cp_bytes=CheckpointOracle(cp);
      for(const auto& [number,bytes]:std::vector<std::pair<u64,const Bytes*>>{{0,&zero_bytes},{13,&map_bytes},{14,&inv_bytes},{19,&cp_bytes}}){
        const auto written=device.WriteAt(number*sizes[profile],bytes->data(),bytes->size());
        Check(written.ok()&&written.bytes_transferred==bytes->size(),"persist exact checkpoint allocation fixture image");}
      Check(device.Sync().ok(),"sync actual checkpoint allocation fixture");
    };
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(2),Profile(profile),&device}};
    const u64 budget=3*sizes[profile];
    const auto read=[&](u64 limit){return db::VerifyCurrentNativeCheckpointAllocationFromOpenDevices(Id(1),devices,CheckpointRef(checkpoint),limit);};
    const auto empty=[&](const auto& r){Check(!r.ok()&&!r.checkpoint_inventory.checkpoint&&
      r.checkpoint_inventory.inventory.entries.empty()&&!r.checkpoint_inventory.inventory.publication_base&&
      r.checkpoint_inventory.retained_image_bytes==0&&r.allocation.pages.empty()&&r.retained_image_bytes==0&&
      r.allocation.retained_image_bytes==0&&std::all_of(r.allocation.state_counts.begin(),r.allocation.state_counts.end(),[](u64 n){return n==0;}),
      "checkpoint allocation failure exposes no authority prefix");};
    persist(inventory,map,zero,checkpoint);
    if(profile==0){byte warm=0;for(unsigned n=0;n<4097;++n){const auto io=device.ReadAt(0,&warm,1);Check(io.ok()&&io.bytes_transferred==1,"warm optional allocation-reader telemetry before fault measurement");}}
    reads=0;track_reads=true;count_allocations=true;observed_allocations=0;
    auto result=read(budget);count_allocations=false;track_reads=false;const auto allocation_count=observed_allocations;const auto read_count=reads;
    Check(result.ok()&&result.retained_image_bytes==budget&&result.allocation.pages.size()==1&&
      result.allocation.pages[0].bytes==AllocationOracle(map)&&result.allocation.state_counts[2]==4&&
      result.allocation.state_counts[6]==60&&result.checkpoint_inventory.inventory.entries.size()==2&&
      !result.checkpoint_inventory.inventory.publication_base,"actual current checkpoint allocation and active original creator binding");
    result=read(budget-1);empty(result);Check(result.error==E::allocation_failure&&
      result.allocation_error==page::NativeAllocationError::resource_exhausted,"aggregate allocation budget boundary");
    for(unsigned fault=1;fault<=3;++fault){full_digest_fault=fault;result=read(budget);
      Check(full_digest_fault==0&&result.error==E::hash_failure,"checkpoint inventory allocation digest provider failure");empty(result);}
    for(unsigned fault=1;fault<=read_count;++fault){reads=0;read_fault=fault;track_reads=true;result=read(budget);track_reads=false;read_fault=0;empty(result);}
    if(profile==0){unsigned long consumed=0;
      for(unsigned long n=0;n<=allocation_count;++n){const auto loss=device.failed_io_latency_observations();
        allocation_budget=static_cast<long>(n);result=read(budget);const auto remaining=allocation_budget;allocation_budget=-1;
        if(remaining<0)++consumed;
        if(result.ok()){Check(result.retained_image_bytes==budget&&result.allocation.pages.size()==1&&result.allocation.pages.front().bytes==AllocationOracle(map),"successful allocation injection retains complete exact authority");
          if(remaining<0)Check(device.failed_io_latency_observations()==loss+1,"only recorded optional telemetry loss permits consumed allocation success");}
        else{Check(remaining<0,"each failed allocation reader actually consumed its injection");empty(result);
          Check(result.error==E::resource_exhausted||result.allocation_error==page::NativeAllocationError::resource_exhausted||
            result.checkpoint_inventory.inventory_error==page::NativeInventoryError::resource_exhausted,"allocation failure preserves resource classification");}
        if(n<allocation_count)Check(remaining<0,"every measured allocation failure position reached");
        else Check(result.ok()&&remaining>=0,"allocation fault sweep reaches uninjected terminal success");}
      Check(consumed==allocation_count,"complete measured allocation fault coverage");
      std::cout<<"checkpoint allocation fault positions="<<allocation_count<<" consumed="<<consumed<<std::endl;
    }
    for(unsigned mutation=0;mutation<7;++mutation){auto altered=map;
      if(mutation==0)altered.creator_transaction_uuid=Id(90);
      if(mutation==1){altered.creator_transaction_uuid=Id(99);altered.creator_local_transaction_id=16;}
      if(mutation==2)altered.records.back().creator_transaction_uuid=Id(90);
      if(mutation==3)altered.records.back().creator_local_transaction_id=15;
      if(mutation==4)altered.creator_local_transaction_id=18;
      if(mutation==5){altered.creator_transaction_uuid={};altered.creator_local_transaction_id=0;altered.creator_operation_uuid=Id(90);}
      if(mutation==6){auto& r=altered.records.back();r.creator_transaction_uuid={};r.creator_local_transaction_id=0;r.creator_operation_uuid=Id(90);}
      if(mutation>=5)Check(page::DecodeNativeAllocationMap(AllocationOracle(altered)).ok(),"structural operation lineage alone is not durable operation authority");
      persist(inventory,altered,zero,checkpoint);result=read(budget);empty(result);
      if(mutation>=5){Check(result.error==E::resource_exhausted,"operation proof cannot turn exhausted allowance into an identity mismatch");
        result=read(32*u64{sizes[profile]});empty(result);}
      Check(result.error==(mutation==1?E::allocation_creator_not_committed:
        mutation==2||mutation==3||mutation==6?E::allocation_record_creator_mismatch:E::allocation_creator_mismatch),"exact map/original creator refusal");
    }
    auto altered=map;altered.capacity_generation++;persist(inventory,altered,zero,checkpoint,16,16,true);
    result=read(budget);empty(result);Check(result.error==E::invalid_integrity,"resealed map cannot evade checkpoint head digest");
    for(unsigned mutation=0;mutation<3;++mutation){auto z=zero;
      if(mutation==0)z.root_set_generation++;if(mutation==1)z.roots[8].page_generation++;
      if(mutation==2)z.roots[2].object_uuid=Id(90);
      persist(inventory,map,z,checkpoint);result=read(budget);empty(result);
      Check(result.error==E::binding_mismatch,"only actual current checkpoint allocation roots can bind");
    }
    for(auto state:{mga::TransactionState::active,mga::TransactionState::prepared,mga::TransactionState::limbo,
        mga::TransactionState::rolled_back,mga::TransactionState::failed_terminal,mga::TransactionState::committed}){
      auto inv=inventory;auto& e=inv.inventory.entries.front();e.state=state;
      if(state==mga::TransactionState::committed){e.commit_sequence=1;inv.inventory.entries[1].commit_sequence=2;inv.inventory.next_commit_sequence=3;}
      const bool final=state==mga::TransactionState::committed||state==mga::TransactionState::rolled_back;
      const u64 oit=final?18:16,oat=state==mga::TransactionState::active?16:18;
      persist(inv,map,zero,checkpoint,oit,oat);Check(read(budget).ok(),"original transaction outcome does not discard retained allocation history");
      if(state==mga::TransactionState::committed||state==mga::TransactionState::rolled_back||state==mga::TransactionState::failed_terminal){
        e.archived_from_state=state;e.state=mga::TransactionState::archived;
        persist(inv,map,zero,checkpoint,oit,18);result=read(budget);
        Check(result.ok(),"archived original allocation identity preserved");
        auto archived_map=map;archived_map.creator_transaction_uuid=Id(99);archived_map.creator_local_transaction_id=16;
        persist(inv,archived_map,zero,checkpoint,oit,18);result=read(budget);
        if(state==mga::TransactionState::committed)Check(result.ok(),"archived committed map creator admitted");
        else{empty(result);Check(result.error==E::allocation_creator_not_committed,"archived noncommitted map creator refused");}
      }
    }
    persist(inventory,map,zero,checkpoint);
    Check(device.Close().ok()&&device.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"reopen actual checkpoint allocation filespace");
    Check(read(budget).ok()&&device.read_only(),"current allocation reader preserves owned read-only device");
  }
}

void CanonicalPolicyRoots() {
  using E=db::NativeCheckpointError;
  for(unsigned p=0;p<5;++p){const unsigned q=(p+1)%5;Fixture fixture;disk::FileDevice first,second;
    const auto path1=(fixture.root/"policy-primary").string(),path2=(fixture.root/"policy-secondary").string();
    auto z1=Example(p),z2=Example(q);z2.bootstrap.filespace_uuid=Id(7);z2.page_uuid=Id(8);
    for(auto& ref:z2.roots)ref.filespace_uuid=Id(7);
    Check(first.Open(path1,disk::FileOpenMode::create_new).ok()&&second.Open(path2,disk::FileOpenMode::create_new).ok(),"own policy-root filespaces");
    const auto prepare=[&](auto& device,const auto& zero){const auto bytes=Oracle(zero);const byte pad=0;
      Check(device.WriteAt(0,bytes.data(),bytes.size()).ok()&&device.WriteAt(zero.total_pages*zero.bootstrap.page_size_bytes-1,&pad,1).ok(),"actual policy-root filespace capacity");};
    prepare(first,z1);prepare(second,z2);
    auto inventory=InventoryExample(p);inventory.inventory.next_local_transaction_id=18;inventory.inventory.next_commit_sequence=4;
    auto& e=inventory.inventory.entries.front();e.identity.local_id=mga::MakeLocalTransactionId(11);e.identity.transaction_uuid.value=Id(91);
    e.state=mga::TransactionState::committed;e.commit_sequence=1;
    auto active=e;active.identity.local_id=mga::MakeLocalTransactionId(16);active.identity.transaction_uuid.value=Id(99);
    active.state=mga::TransactionState::active;active.commit_sequence=0;
    auto policy_creator=e;policy_creator.identity.local_id=mga::MakeLocalTransactionId(12);
    policy_creator.identity.transaction_uuid.value=Id(92);policy_creator.commit_sequence=2;
    auto creator=e;creator.identity.local_id=mga::MakeLocalTransactionId(17);creator.identity.transaction_uuid.value=Id(98);creator.commit_sequence=3;
    inventory.inventory.entries.push_back(policy_creator);
    inventory.inventory.entries.push_back(active);inventory.inventory.entries.push_back(creator);
    auto catalog=RootExample(p);catalog.creator_local_transaction_id=11;
    std::array<page::NativeCatalogRoot,2> policies{catalog,catalog};
    for(unsigned index=0;index<2;++index){auto& root=policies[index];
      root.root_kind=static_cast<disk::u16>(index+6);root.header.page_type=index?11:10;
      root.header.page_number=16+index;root.header.page_generation=106+index;root.header.page_uuid=Id(86+index);
      root.object_uuid=Id(46+index);root.roots={catalog.roots[index?3:4]};
      root.creator_transaction_uuid=Id(92);root.creator_local_transaction_id=12;
      if(index){root.header.filespace_uuid=Id(7);root.header.page_size_bytes=sizes[q];root.header.page_size_profile_uuid=Profile(q);}
    }
    const auto checkpoint=CheckpointExample(p);
    const auto put=[&](auto& device,u64 page,unsigned size,const Bytes& bytes){const auto io=device.WriteAt(page*size,bytes.data(),bytes.size());
      Check(io.ok()&&io.bytes_transferred==bytes.size()&&device.Sync().ok(),"persist exact policy-root image");};
    const auto persist=[&](const auto& values,bool stale_digest=false){auto cp=checkpoint;
      const auto inv=InventoryStateOracle(inventory),cat=RootOracle(catalog);
      cp.roots[0].page=InventoryRef(inventory);cp.roots[0].object_uuid=inventory.object_uuid;cp.roots[0].sha256=WholeRootHash(inv);
      const auto bind=[&](auto& target,const auto& root,const Bytes& bytes){target.page=PageRef(root);target.object_uuid=root.object_uuid;target.sha256=WholeRootHash(bytes);};
      bind(cp.roots[4],catalog,cat);cp.roots[8]=cp.roots[4];cp.roots[8].role=9;
      for(unsigned i=0;i<2;++i){const auto bytes=RootOracle(values[i]);
        bind(cp.roots[5+i],policies[i],stale_digest?RootOracle(policies[i]):bytes);
        put(i?second:first,16+i,i?sizes[q]:sizes[p],bytes);}
      put(first,12,sizes[p],cat);put(first,14,sizes[p],inv);put(first,19,sizes[p],CheckpointOracle(cp));
    };
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};
    const u64 budget=4*sizes[p]+sizes[q];
    const auto read=[&](u64 limit){return db::VerifyNativeCheckpointPolicyRootsFromOpenDevices(Id(1),devices,CheckpointRef(checkpoint),limit);};
    const auto empty=[&](const auto& r){Check(!r.ok()&&r.retained_image_bytes==0&&r.catalog.catalogs.empty()&&
      !r.catalog.checkpoint_inventory.checkpoint&&r.catalog.checkpoint_inventory.inventory.entries.empty()&&
      !r.catalog.checkpoint_inventory.inventory.publication_base&&!r.policies[0].root&&!r.policies[1].root&&
      r.policies[0].bytes.empty()&&r.policies[1].bytes.empty(),"policy-root join refuses without verified prefix");};
    persist(policies);observed_allocations=0;count_allocations=true;reads=0;track_reads=true;
    auto result=read(budget);count_allocations=false;track_reads=false;const auto allocations=observed_allocations;const auto read_count=reads;
    Check(result.ok()&&result.retained_image_bytes==budget&&result.policies[0].bytes==RootOracle(policies[0])&&
      result.policies[1].bytes==RootOracle(policies[1])&&result.catalog.catalogs.size()==1&&
      !result.catalog.checkpoint_inventory.inventory.publication_base,"actual mixed-profile checkpoint configuration/security roots");
    result=read(budget-1);empty(result);Check(result.error==E::resource_exhausted,"policy-root shared budget includes final image");
    for(unsigned fault=1;fault<=read_count;++fault){reads=0;read_fault=fault;track_reads=true;result=read(budget);track_reads=false;read_fault=0;empty(result);}
    bool digest_end=false;
    for(unsigned fault=1;fault<32;++fault){full_digest_fault=fault;result=read(budget);
      if(full_digest_fault){full_digest_fault=0;Check(result.ok(),"policy-root full-image hash sweep reached end");digest_end=true;break;}
      empty(result);Check(result.error==E::hash_failure
        ||(result.error==E::inventory_failure&&result.catalog.checkpoint_inventory.inventory_error==page::NativeInventoryError::hash_failure)
        ||(result.error==E::catalog_failure&&result.catalog_error==RootError::hash_failure),
        "checkpoint/catalog/policy full digest provider failures");}
    Check(digest_end,"all policy-root full-image hash calls faulted");
    if(p==0){bool success=false;
      for(unsigned long fault=0;fault<=allocations;++fault){allocation_budget=static_cast<long>(fault);result=read(budget);allocation_budget=-1;
        if(result.ok()){
          Check(result.retained_image_bytes==budget&&result.policies[0].bytes==RootOracle(policies[0])&&
            result.policies[1].bytes==RootOracle(policies[1]),"allocation sweep success retains exact policy images");
          if(fault==allocations)success=true;
        }else empty(result);}
      Check(success,"all measured policy-root join allocation failures");std::cout<<"policy-root allocation fault positions="<<allocations<<std::endl;
    }
    for(auto origin:{mga::TransactionState::committed,mga::TransactionState::rolled_back,mga::TransactionState::failed_terminal}){
      auto& archived=inventory.inventory.entries[1];archived.state=mga::TransactionState::archived;archived.archived_from_state=origin;
      archived.commit_sequence=origin==mga::TransactionState::committed?2:0;
      persist(policies);result=read(budget);
      if(origin==mga::TransactionState::committed)Check(result.ok(),"archived committed policy creator retained");
      else{empty(result);Check(result.error==E::catalog_creator_not_committed,"archived noncommit cannot authorize dedicated policy roots");}
    }
    inventory.inventory.entries[1]=policy_creator;
    inventory.inventory.entries[1].identity.scope=mga::TransactionScope::cluster_global;
    persist(policies);result=read(budget);empty(result);
    Check(result.error==E::catalog_creator_mismatch,"cluster creator cannot authorize standalone policy root");
    inventory.inventory.entries[1]=policy_creator;
    for(unsigned index=0;index<2;++index){
      for(unsigned mutation=0;mutation<6;++mutation){auto changed=policies;auto& r=changed[index];
        if(mutation==0)r.roots[0].page.page_number++;
        if(mutation==1)r.roots[0].page.page_generation++;
        if(mutation==2)r.roots[0].object_uuid=Id(200);
        if(mutation==3)r.roots[0].page_type=r.roots[0].page_type==6?0x200:6;
        if(mutation==4)r.creator_transaction_uuid=Id(200);
        if(mutation==5){r.creator_transaction_uuid=Id(99);r.creator_local_transaction_id=16;}
        Check(page::DecodeNativeCatalogRoot(RootOracle(r)).ok(),"wrong policy binding remains individually valid native image");
        persist(changed);result=read(budget);empty(result);
        Check(result.error==(mutation<4?E::policy_relation_mismatch:mutation==4?E::catalog_creator_mismatch:E::catalog_creator_not_committed),
              "exact policy relation or creator refusal");
      }
      auto changed=policies;changed[index].security_epoch++;persist(changed,true);result=read(budget);empty(result);
      Check(result.error==E::invalid_integrity,"resealed dedicated root still binds checkpoint digest");
      // Kind and canonical type are inseparable even when all target fields are valid.
      auto wrong=policies[index];wrong.header.page_type=index?10:11;
      RootInvalid(wrong,RootError::invalid_header);
      wrong=policies[index];wrong.roots[0].role=index?5:4;RootInvalid(wrong,RootError::invalid_roots);
      wrong=policies[index];wrong.roots.push_back(wrong.roots[0]);RootInvalid(wrong,RootError::invalid_roots);
      // Both dedicated families retain real immutable history with their own type.
      auto newer=policies[index];newer.catalog_generation=2;newer.header.page_number=18;
      newer.header.page_uuid=Id(190+index);newer.header.page_generation++;
      newer.predecessor=PageRef(policies[index]);newer.predecessor_sha256=WholeRootHash(RootOracle(policies[index]));
      auto& device=index?second:first;const auto size=index?sizes[q]:sizes[p];
      put(device,policies[index].header.page_number,size,RootOracle(policies[index]));put(device,18,size,RootOracle(newer));
      const auto range=page::ReadNativeCatalogRootRangeFromOpenDevices(Id(1),devices,RootRef(newer),RootRef(policies[index]),2*size);
      Check(range.ok()&&range.roots.size()==2&&range.roots[0].bytes==RootOracle(newer)&&
            range.roots[1].bytes==RootOracle(policies[index]),"dedicated policy root history preserves native type and exact images");
      auto bad_ref=RootRef(policies[index]);bad_ref.kind=8;bad_ref.page_type=5;
      Check(!page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),bad_ref).ok(),"feature reference cannot adopt dedicated policy root");
    }
    persist(policies);
    Check(first.Close().ok()&&second.Close().ok()&&first.Open(path1,disk::FileOpenMode::open_existing_read_only).ok()&&
      second.Open(path2,disk::FileOpenMode::open_existing_read_only).ok(),"read-only policy filespace reopen");
    Check(read(budget).ok()&&first.read_only()&&second.read_only(),"reopened policy roots preserve ownership mode");
  }
}

Bytes DirectoryOracle(const page::NativeFilespaceDirectory& value) {
  auto common=RootExample();common.header=value.header;auto b=RootOracle(common);std::fill(b.begin()+128,b.end(),0);
  const auto ref=[&](std::size_t at,const disk::NativePageReference& r){PutUuid(b,at,r.filespace_uuid);Number(b,at+16,8,r.page_number);Number(b,at+24,8,r.page_generation);PutUuid(b,at+32,r.page_size_profile_uuid);};
  std::copy_n("SBFDIR01",8,b.begin()+128);Number(b,136,2,1);Number(b,138,2,256);Number(b,140,4,384+192*value.records.size());
  PutUuid(b,144,value.object_uuid);Number(b,160,8,value.directory_generation);PutUuid(b,168,value.creator_transaction_uuid);Number(b,184,8,value.creator_local_transaction_id);
  Number(b,192,8,value.total_records);Number(b,200,8,value.first_record);Number(b,208,4,value.records.size());if(value.next)ref(216,*value.next);
  std::copy(value.next_sha256.begin(),value.next_sha256.end(),b.begin()+264);
  for(std::size_t i=0;i<value.records.size();++i){const auto at=384+i*192;const auto& r=value.records[i];const auto& a=r.bootstrap;
    PutUuid(b,at,a.filespace_uuid);PutUuid(b,at+16,a.page_size_profile_uuid);PutUuid(b,at+32,a.checksum_profile_uuid);PutUuid(b,at+48,a.encryption_profile_uuid);
    PutUuid(b,at+64,r.locator_uuid);PutUuid(b,at+80,r.page_zero_uuid);Number(b,at+96,8,r.page_zero_generation);Number(b,at+104,8,r.root_set_generation);
    Number(b,at+112,8,r.total_pages);Number(b,at+120,8,r.verification_epoch);Number(b,at+128,2,a.filespace_role);Number(b,at+130,2,a.lifecycle_state);
    Number(b,at+132,4,a.flags);Number(b,at+136,4,a.page_size_bytes);Number(b,at+140,4,a.durable_format_generation);if(r.operation)ref(at+144,*r.operation);
  }
  const auto digest=WholeRootHash(b);std::copy(digest.begin(),digest.end(),b.begin()+296);return b;
}


Bytes DirectoryAllocationOracle(const page::NativeFilespaceDirectory& value){
  const auto legacy=DirectoryOracle(value);Bytes b(legacy.size());std::copy_n(legacy.begin(),384,b.begin());
  std::copy_n("SBFDIR02",8,b.begin()+128);Number(b,136,2,2);Number(b,140,4,384+320*value.records.size());Number(b,212,4,320);
  PutUuid(b,328,value.creator_operation_uuid);std::fill(b.begin()+296,b.begin()+328,0);
  for(std::size_t i=0;i<value.records.size();++i){const auto at=384+320*i;std::copy_n(legacy.begin()+384+192*i,192,b.begin()+at);
    if(const auto& root=value.records[i].allocation_root){PutUuid(b,at+192,root->page.filespace_uuid);Number(b,at+208,8,root->page.page_number);
      Number(b,at+216,8,root->page.page_generation);PutUuid(b,at+224,root->page.page_size_profile_uuid);PutUuid(b,at+240,root->object_uuid);
      std::copy(root->sha256.begin(),root->sha256.end(),b.begin()+at+256);Number(b,at+288,8,root->map_generation);Number(b,at+296,8,root->capacity_generation);}}
  const auto digest=WholeRootHash(b);std::copy(digest.begin(),digest.end(),b.begin()+296);return b;
}

void CanonicalDirectoryAllocationBindings(){using E=page::NativeDirectoryError;
  const auto empty=[](const auto& r){Check(!r.ok()&&!r.directory&&r.bytes.empty(),"directory allocation codec refuses without a prefix");};
  for(unsigned p=0;p<5;++p){const unsigned q=(p+1)%5;auto primary=Example(p),secondary=Example(q,5);
    secondary.bootstrap.filespace_uuid=Id(7);secondary.page_uuid=Id(8);for(auto& r:secondary.roots)r.filespace_uuid=Id(7);
    page::NativeFilespaceDirectory d;d.header={sizes[p],9,Id(1),Id(2),Id(80),15,105,0,Profile(p)};d.object_uuid=Id(45);d.directory_generation=5;
    d.creator_transaction_uuid=Id(91);d.creator_local_transaction_id=11;d.total_records=2;
    for(const auto* z:{&primary,&secondary})d.records.push_back({z->bootstrap,Id(z==&primary?120:121),z->page_uuid,z->page_generation,z->root_set_generation,z->total_pages,0,{}});
    const auto binding=[&](unsigned index){const auto& r=d.records[index];page::NativeFilespaceAllocationRoot root;
      root.page={r.bootstrap.filespace_uuid,13,103,r.bootstrap.page_size_profile_uuid};root.object_uuid=Id(43+index);root.sha256.fill(static_cast<byte>(index+1));root.map_generation=5;root.capacity_generation=6;return root;};
    const auto original=d;
    for(unsigned mode=0;mode<4;++mode){d=original;
      if(mode&1){d.records[0].allocation_root=binding(0);d.records[1].allocation_root=binding(1);}
      if(mode&2){d.creator_transaction_uuid={};d.creator_local_transaction_id=0;d.creator_operation_uuid=Id(93);}
      const auto oracle=mode?DirectoryAllocationOracle(d):DirectoryOracle(d);const auto encoded=page::EncodeNativeFilespaceDirectory(d);
      Check(encoded.ok()&&encoded.bytes==oracle,"independent exact directory version/creator/root image");
      const auto decoded=page::DecodeNativeFilespaceDirectory(oracle);
      Check(decoded.ok()&&decoded.directory->creator_operation_uuid==d.creator_operation_uuid&&decoded.directory->creator_transaction_uuid==d.creator_transaction_uuid&&
        decoded.directory->creator_local_transaction_id==d.creator_local_transaction_id&&decoded.directory->records.size()==2&&
        decoded.directory->records[0].allocation_root==d.records[0].allocation_root&&decoded.directory->records[1].allocation_root==d.records[1].allocation_root&&
        page::EncodeNativeFilespaceDirectory(*decoded.directory).bytes==oracle,"binary allocation references and exclusive creator survive decoding");
      if(mode&2){
        // Matcher contract only: these in-memory entries are NOT durable
        // publication or selected graph authority evidence.
        db::NativeManagementControlGraph proof;page::NativeFilespaceDirectoryChainResult chain;
        chain.error=E::none;chain.pages.push_back(decoded);const auto sha=WholeRootHash(oracle);
        db::NativeManagementPublishedCheckpoint publication;
        publication.directory_root={3,9,{Id(2),15,105,Profile(p)},d.object_uuid,sha};
        proof.publications.emplace(d.creator_operation_uuid,publication);
        page::NativeAllocationRecord record;record.page_number=15;record.allocation_uuid=Id(97);record.page_uuid=d.header.page_uuid;
        record.page_generation=105;record.page_type=9;record.owner_uuid=d.object_uuid;record.creator_operation_uuid=d.creator_operation_uuid;
        proof.allocations.emplace(std::make_pair(Id(2),u64{15}),record);
        Check(db::MatchesNativeManagementPublishedDirectory(proof,chain,sha),"directory matcher uses original entries without recursive selected authority");
        Check(!proof.ok(),"matcher fixture cannot masquerade as verified control graph");
        for(unsigned field=0;field<10;++field){auto wrong=proof;auto& r=wrong.publications.begin()->second.directory_root;
          if(field==0)r.role=4;if(field==1)r.page_type=3;if(field==2)r.page.filespace_uuid=Id(7);
          if(field==3)++r.page.page_number;if(field==4)++r.page.page_generation;if(field==5)r.page.page_size_profile_uuid=Profile(q);
          if(field==6)r.object_uuid=Id(99);if(field==7)r.sha256[0]^=1;if(field==8)wrong.publications.clear();if(field==9)wrong.allocations.clear();
          Check(!db::MatchesNativeManagementPublishedDirectory(wrong,chain,sha),"directory matcher rejects substituted publication root or missing original allocation");}
        for(unsigned field=0;field<9;++field){auto wrong=proof;auto& r=wrong.allocations.begin()->second;
          if(field==0)++r.page_number;if(field==1)r.page_uuid=Id(99);if(field==2)++r.page_generation;
          if(field==3)r.page_type=3;if(field==4)r.owner_uuid=Id(99);if(field==5)r.creator_operation_uuid=Id(99);
          if(field==6)r.creator_transaction_uuid=Id(91);if(field==7)r.creator_local_transaction_id=11;
          if(field==8){wrong.allocations.clear();wrong.allocations.emplace(std::make_pair(Id(7),u64{15}),record);}
          Check(!db::MatchesNativeManagementPublishedDirectory(wrong,chain,sha),"directory matcher rejects reparented reused or foreign original records");}
        for(unsigned field=0;field<7;++field){auto wrong=chain;auto& image=*wrong.pages.front().directory;
          if(field==0)image.creator_operation_uuid=Id(99);if(field==1)image.creator_transaction_uuid=Id(91);
          if(field==2)image.creator_local_transaction_id=11;if(field==3)image.header.page_type=3;
          if(field==4)wrong.pages.front().directory.reset();if(field==5)wrong.error=E::chain_mismatch;if(field==6)wrong.pages.clear();
          Check(!db::MatchesNativeManagementPublishedDirectory(proof,wrong,sha),"directory matcher rejects incomplete or nonexclusive chain images");}
        auto tail=d;tail.header.filespace_uuid=Id(7);tail.header.page_size_profile_uuid=Profile(q);tail.header.page_size_bytes=sizes[q];
        tail.header.page_number=16;tail.header.page_uuid=Id(98);tail.header.page_generation=106;
        page::NativeFilespaceDirectoryResult tail_image;tail_image.error=E::none;tail_image.directory=tail;chain.pages.push_back(tail_image);
        Check(!db::MatchesNativeManagementPublishedDirectory(proof,chain,sha),"every directory continuation requires its own original allocation");
        auto tail_record=record;tail_record.page_number=16;tail_record.page_uuid=Id(98);tail_record.page_generation=106;tail_record.allocation_uuid=Id(100);
        proof.allocations.emplace(std::make_pair(Id(7),u64{16}),tail_record);
        Check(db::MatchesNativeManagementPublishedDirectory(proof,chain,sha),"matcher checks continuation binary filespace and slot against exact original record");
        chain.pages.back().directory->creator_operation_uuid=Id(99);
        Check(!db::MatchesNativeManagementPublishedDirectory(proof,chain,sha),"continuation cannot borrow another operation publication");
      }
      if(!mode)continue;
      for(unsigned mutation=0;mutation<18;++mutation){auto bad=d;bad.records[0].allocation_root=binding(0);auto& root=*bad.records[0].allocation_root;
        if(mutation==0){bad.creator_transaction_uuid=Id(91);bad.creator_local_transaction_id=11;bad.creator_operation_uuid=Id(93);}
        if(mutation==1){bad.creator_transaction_uuid={};bad.creator_local_transaction_id=0;bad.creator_operation_uuid={};}
        if(mutation==2){bad.creator_transaction_uuid=Id(91);bad.creator_local_transaction_id=0;bad.creator_operation_uuid={};}
        if(mutation==3){bad.creator_transaction_uuid={};bad.creator_local_transaction_id=0;bad.creator_operation_uuid=Id(93);bad.creator_operation_uuid.bytes[6]=0x40;}
        if(mutation==4)root.page.filespace_uuid=Id(7);if(mutation==5)root.page.page_size_profile_uuid=Profile(q);
        if(mutation==6)root.page.page_number=0;if(mutation==7)root.page.page_generation=0;
        if(mutation==8)root.page.page_number=bad.records[0].total_pages;if(mutation==9)root.page.page_number=bad.header.page_number;
        if(mutation==10)root.object_uuid={};if(mutation==11)root.object_uuid.bytes[6]=0x40;
        if(mutation==12)root.sha256.fill(0);if(mutation==13)root.map_generation=0;if(mutation==14)root.capacity_generation=0;
        if(mutation==15)root.page.filespace_uuid={};if(mutation==16)root.page.page_size_profile_uuid={};if(mutation==17)root={};
        empty(page::EncodeNativeFilespaceDirectory(bad));const auto bytes=DirectoryAllocationOracle(bad);
        if(mutation!=17)empty(page::DecodeNativeFilespaceDirectory(bytes));
        else{const auto r=page::DecodeNativeFilespaceDirectory(bytes);Check(r.ok()&&!r.directory->records[0].allocation_root,"all-zero wire tuple means absence, not a partial allocation root");}
      }
      for(unsigned mutation=0;mutation<9;++mutation){auto bytes=oracle;
        if(mutation==0)bytes[135]='1';if(mutation==1)Number(bytes,136,2,1);if(mutation==2)Number(bytes,212,4,192);
        if(mutation==3)bytes[344]=1;if(mutation==4)bytes[384+304]=1;if(mutation==5)bytes.back()=1;
        if(mutation==6)Number(bytes,140,4,384+192*d.records.size());if(mutation==7)Number(bytes,208,4,0xffffffff);
        if(mutation==8){std::fill(bytes.begin()+384+192,bytes.begin()+384+304,0);bytes[384+256]=1;}
        std::fill(bytes.begin()+296,bytes.begin()+328,0);const auto hash=WholeRootHash(bytes);std::copy(hash.begin(),hash.end(),bytes.begin()+296);
        empty(page::DecodeNativeFilespaceDirectory(bytes));
      }
      for(unsigned route=0;route<2;++route){const auto call=[&](){return route?page::DecodeNativeFilespaceDirectory(oracle):page::EncodeNativeFilespaceDirectory(d);};
        for(unsigned fault=1;fault<=5;++fault){hash_fault=fault;const auto r=call();Check(!hash_fault&&r.error==E::hash_failure,"directory extension consumes hash backend failures");empty(r);}
        if(p==0){observed_allocations=0;count_allocations=true;const auto good=call();count_allocations=false;const auto count=observed_allocations;Check(good.ok(),"directory allocation fault baseline");
          for(unsigned long n=0;n<=count;++n){allocation_budget=n;const auto r=call();const auto remaining=allocation_budget;allocation_budget=-1;
            if(n<count){Check(remaining<0&&r.error==E::resource_exhausted,"directory extension allocation failure consumed");empty(r);}
            else Check(r.ok()&&remaining>=0&&r.bytes==oracle,"directory extension allocation terminal exact success");}}
      }
    }
    d=original;empty(page::DecodeNativeFilespaceDirectory(DirectoryAllocationOracle(d)));
  }
}

void CanonicalCatalogLeafStaging(){using E=db::NativeCatalogLeafStageError;using S=page::NativeAllocationState;
  for(unsigned p=0;p<5;++p)for(unsigned role=1;role<=5;++role){const unsigned q=(p+1)%5;const bool primary=role<=4;
    Fixture fixture;disk::FileDevice first,second;auto z1=Example(p,primary?role:1),z2=Example(q,5);z2.bootstrap.filespace_uuid=Id(7);z2.page_uuid=Id(8);for(auto& root:z2.roots)root.filespace_uuid=Id(7);
    z1.free_pages=z2.free_pages=z1.preallocated_pages=z2.preallocated_pages=0;
    const auto path1=(fixture.root/"stage-primary").string(),path2=(fixture.root/"stage-secondary").string();
    Check(first.Open(path1,disk::FileOpenMode::create_new).ok()&&second.Open(path2,disk::FileOpenMode::create_new).ok(),"own catalog staging filespaces");
    const byte pad=0;Check(first.WriteAt(64*sizes[p]-1,&pad,1).ok()&&second.WriteAt(64*sizes[q]-1,&pad,1).ok(),"actual catalog staging capacity");
    auto& target=primary?first:second;const auto profile=primary?p:q;const auto fs=primary?Id(2):Id(7);const auto& zero=primary?z1:z2;
    auto inv=InventoryExample(p);inv.inventory.next_local_transaction_id=18;inv.inventory.next_commit_sequence=2;
    auto& active=inv.inventory.entries[0];active.identity.local_id=mga::MakeLocalTransactionId(13);active.identity.transaction_uuid.value=Id(162);active.state=mga::TransactionState::active;active.commit_sequence=0;
    auto committed=active;committed.identity.local_id=mga::MakeLocalTransactionId(17);committed.identity.transaction_uuid.value=Id(98);committed.state=mga::TransactionState::committed;committed.commit_sequence=1;inv.inventory.entries.push_back(committed);
    const auto owner=inv.inventory.entries.front().identity;auto cp=CheckpointExample(p);auto leaf=LeafExample(profile);leaf.header.filespace_uuid=fs;
    page::NativeAllocationMap map;map.header={sizes[profile],3,Id(1),fs,Id(70),13,103,0,Profile(profile)};map.object_uuid=Id(43);map.map_generation=5;map.capacity_generation=6;map.total_pages=64;map.creator_transaction_uuid=Id(98);map.creator_local_transaction_id=17;map.states.assign(64,S::quarantined);
    for(unsigned n:{0u,13u,21u}){page::NativeAllocationRecord r;r.page_number=n;r.allocation_uuid=Id(120+n);r.creator_transaction_uuid=Id(98);r.creator_local_transaction_id=17;map.states[n]=S::allocated;
      if(n==0){r.page_uuid=zero.page_uuid;r.page_generation=zero.page_generation;r.page_type=primary?1:2;r.owner_uuid=fs;}
      if(n==13){r.page_uuid=map.header.page_uuid;r.page_generation=103;r.page_type=3;r.owner_uuid=map.object_uuid;}
      if(n==21){r.page_uuid=leaf.header.page_uuid;r.page_generation=7;r.page_type=6;r.owner_uuid=leaf.body.relation_uuid.value;r.creator_transaction_uuid=Id(162);r.creator_local_transaction_id=13;map.states[n]=S::reserved;}
      map.records.push_back(r);}
    page::NativeFilespaceDirectory directory;directory.header={sizes[p],9,Id(1),Id(2),Id(80),15,105,0,Profile(p)};directory.object_uuid=Id(45);directory.directory_generation=5;directory.creator_transaction_uuid=Id(98);directory.creator_local_transaction_id=17;directory.total_records=2;
    for(const auto* z:{&z1,&z2})directory.records.push_back({z->bootstrap,Id(z==&z1?190:191),z->page_uuid,z->page_generation,z->root_set_generation,z->total_pages,0,{}});
    const auto put=[&](auto& file,u64 number,unsigned size,const Bytes& bytes){const auto io=file.WriteAt(number*size,bytes.data(),bytes.size());Check(io.ok()&&io.bytes_transferred==bytes.size()&&file.Sync().ok(),"persist catalog staging fixture bytes");};
    const auto persist=[&](){const auto ib=InventoryOracle(inv,13,13,13),ab=AllocationOracle(map),dbb=DirectoryOracle(directory);
      cp.roots[0].page=InventoryRef(inv);cp.roots[0].object_uuid=inv.object_uuid;cp.roots[0].sha256=WholeRootHash(ib);
      cp.roots[2].page={Id(2),15,105,Profile(p)};cp.roots[2].object_uuid=Id(45);cp.roots[2].sha256=WholeRootHash(dbb);
      if(primary){cp.roots[3].page={fs,13,103,Profile(profile)};cp.roots[3].object_uuid=Id(43);cp.roots[3].sha256=WholeRootHash(ab);}
      put(first,0,sizes[p],Oracle(z1));put(second,0,sizes[q],Oracle(z2));put(first,14,sizes[p],ib);put(first,15,sizes[p],dbb);put(target,13,sizes[profile],ab);put(first,19,sizes[p],CheckpointOracle(cp));};
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};
    const u64 budget=3*sizes[p]+3*sizes[profile];const Bytes blank(sizes[profile],0),expected=LeafOracle(leaf);
    const auto reset=[&](){put(target,21,sizes[profile],blank);};
    const auto stage=[&](u64 limit){return db::StageNativeCatalogLeafFromOpenDevices(devices,CheckpointRef(cp),owner,leaf,limit);};
    const auto empty=[&](const auto& r){Check(!r.ok()&&!r.receipt,"failed stage returns no receipt");};
    const auto bytes=[&](){Bytes b(sizes[profile]);const auto r=target.ReadAt(21*sizes[profile],b.data(),b.size());Check(r.ok()&&r.bytes_transferred==b.size(),"independent actual staged bytes");return b;};
    persist();reset();reads=observed_full_digests=0;observed_allocations=0;stage_writes=stage_syncs=0;track_reads=count_allocations=count_full_digests=true;
    auto result=stage(budget);track_reads=count_allocations=count_full_digests=false;const auto nr=reads,nf=observed_full_digests;const auto na=observed_allocations;
    if(!result.ok())std::cerr<<"stage error="<<static_cast<int>(result.error)<<" checkpoint="<<static_cast<int>(result.checkpoint_error)<<" allocation="<<static_cast<int>(result.allocation_error)<<" leaf="<<static_cast<int>(result.leaf_error)<<std::endl;
    Check(result.ok()&&stage_writes==1&&stage_syncs==1&&result.receipt->allocation_uuid==Id(141)&&result.receipt->page_uuid==leaf.header.page_uuid&&result.receipt->transaction.transaction_uuid.value==Id(162)&&result.receipt->page.filespace_uuid==fs&&result.receipt->sha256==WholeRootHash(expected)&&bytes()==expected,"actual reserved canonical leaf receipt and independent bytes");
    stage_writes=stage_syncs=0;result=stage(budget);Check(result.ok()&&!stage_writes&&stage_syncs==1,"exact idempotent stage retry still syncs");
    auto conflicting=expected;conflicting.back()^=1;put(target,21,sizes[profile],conflicting);empty(stage(budget));Check(bytes()==conflicting,"different nonzero destination preserved");
    reset();empty(stage(budget-1));Check(bytes()==blank,"budget exhausted before any write");
    if(p==0&&role==1){
      for(unsigned long n=0;n<=na;++n){reset();allocation_budget=n;result=stage(budget);allocation_budget=-1;if(result.ok())Check(bytes()==expected,"allocation recovery exact staging");else{empty(result);const auto actual=bytes();Check(actual==blank||actual==expected,"allocation failure preserves reserved preimage or written staging, never grants publication");}if(n==na)Check(result.ok(),"allocation sweep terminal staging success");}
      for(unsigned n=1;n<=nr;++n){reset();reads=0;read_fault=n;track_reads=true;result=stage(budget);track_reads=false;Check(!read_fault,"staging read failure consumed");empty(result);}
      for(unsigned n=1;n<=nf;++n){reset();full_digest_fault=n;result=stage(budget);Check(!full_digest_fault,"staging full hash fault consumed");empty(result);Check(bytes()==blank,"hash failure before write");}
      for(unsigned mode=1;mode<=5;++mode){reset();hash_fault=mode;result=stage(budget);Check(!hash_fault,"staging multipart provider fault consumed");empty(result);Check(bytes()==blank,"provider failure before write");}
      for(unsigned mode=1;mode<=2;++mode){reset();stage_write_fault=mode;result=stage(budget);Check(!stage_write_fault,"actual write fault consumed");empty(result);if(mode==2){const auto partial=bytes();empty(stage(budget));Check(bytes()==partial,"partial destination preserved for recovery");}}
      reset();stage_sync_fault=1;result=stage(budget);Check(!stage_sync_fault,"actual stage sync failure consumed");empty(result);Check(bytes()==expected&&stage(budget).ok(),"sync failure retry revalidates existing exact image");
      reset();reads=0;stage_corrupt_read=nr;track_reads=true;result=stage(budget);track_reads=false;Check(!stage_corrupt_read,"actual final readback corruption injected");empty(result);Check(result.error==E::readback_mismatch&&bytes()==expected,"readback corruption never certifies written image");
      std::cout<<"catalog stage allocations="<<na<<" reads="<<nr<<" full digests="<<nf<<std::endl;
    }
    reset();const auto original_map=map;
    for(const auto state:{S::free,S::allocated,S::reusable_pending_mga,S::reusable_free,S::compacting,S::quarantined,S::preallocated}){map=original_map;map.states[21]=state;if(state==S::free)map.records.pop_back();persist();empty(stage(budget));Check(bytes()==blank,"only actual reservation grants stage");}
    map=original_map;for(unsigned n=0;n<5;++n){map=original_map;auto& r=map.records.back();if(n==0)r.owner_uuid=Id(201);if(n==1)r.page_uuid=Id(202);if(n==2)r.page_generation++;if(n==3)r.page_type=512;if(n==4){r.creator_transaction_uuid=Id(98);r.creator_local_transaction_id=17;}persist();result=stage(budget);empty(result);Check(result.error==E::reservation_mismatch&&bytes()==blank,"exact reserved owner/page/type/creator binding");}
    map=original_map;persist();auto wrong_owner=owner;wrong_owner.transaction_uuid.value=Id(203);empty(db::StageNativeCatalogLeafFromOpenDevices(devices,CheckpointRef(cp),wrong_owner,leaf,budget));Check(bytes()==blank,"wrong active owner cannot stage");
    for(unsigned n=0;n<4;++n){map=original_map;auto changed=leaf;if(n==0)map.records.back().allocation_uuid=map.records.front().allocation_uuid;else{const auto id=n==1?map.records.front().page_uuid:n==2?cp.header.page_uuid:directory.header.page_uuid;map.records.back().page_uuid=changed.header.page_uuid=id;}persist();result=db::StageNativeCatalogLeafFromOpenDevices(devices,CheckpointRef(cp),owner,changed,budget);empty(result);Check(result.error==E::reservation_mismatch&&bytes()==blank,"duplicate allocation/page and known control UUIDs cannot stage");}map=original_map;
    map.records.back().creator_transaction_uuid=Id(98);map.records.back().creator_local_transaction_id=17;persist();result=db::StageNativeCatalogLeafFromOpenDevices(devices,CheckpointRef(cp),committed.identity,leaf,budget);empty(result);Check(result.error==E::creator_not_active&&bytes()==blank,"committed transaction cannot perform new staging");map=original_map;persist();
    inv.inventory.entries[0].rollback_only=true;persist();result=stage(budget);empty(result);Check(result.error==E::creator_rollback_only&&bytes()==blank,"rollback-only active owner cannot stage");inv.inventory.entries[0].rollback_only=false;persist();
    result=stage(budget);Check(result.ok(),"final actual leaf staging");const auto target_path=primary?path1:path2;
    Check(target.Close().ok()&&target.Open(target_path,disk::FileOpenMode::open_existing_read_only).ok(),"independent read-only stage reopen");
    Check(bytes()==expected&&db::DecodeNativeCatalogLeaf(bytes()).ok(),"reopened canonical rows and metadata");empty(stage(budget));Check(bytes()==expected,"read-only destination unchanged");
  }
}
void CanonicalRowDataStaging(){using E=db::NativeRowDataStageError;using S=page::NativeAllocationState;
  for(unsigned p=0;p<5;++p)for(unsigned role:{5u,8u,10u}){const unsigned q=(p+1)%5;const bool primary=role<=4;
    Fixture fixture;disk::FileDevice first,second;auto z1=Example(p,primary?role:1),z2=Example(q,role);z2.bootstrap.filespace_uuid=Id(7);z2.page_uuid=Id(8);for(auto& root:z2.roots)root.filespace_uuid=Id(7);
    z1.free_pages=z2.free_pages=z1.preallocated_pages=z2.preallocated_pages=0;
    const auto path1=(fixture.root/"row-stage-primary").string(),path2=(fixture.root/"row-stage-secondary").string();
    Check(first.Open(path1,disk::FileOpenMode::create_new).ok()&&second.Open(path2,disk::FileOpenMode::create_new).ok(),"own row-data staging filespaces");
    const byte pad=0;Check(first.WriteAt(64*sizes[p]-1,&pad,1).ok()&&second.WriteAt(64*sizes[q]-1,&pad,1).ok(),"actual row-data staging capacity");
    auto& target=primary?first:second;const auto profile=primary?p:q;const auto fs=primary?Id(2):Id(7);const auto& zero=primary?z1:z2;
    auto inv=InventoryExample(p);inv.inventory.next_local_transaction_id=18;inv.inventory.next_commit_sequence=2;
    auto& active=inv.inventory.entries[0];active.identity.local_id=mga::MakeLocalTransactionId(13);active.identity.transaction_uuid.value=Id(162);active.state=mga::TransactionState::active;active.commit_sequence=0;
    auto committed=active;committed.identity.local_id=mga::MakeLocalTransactionId(17);committed.identity.transaction_uuid.value=Id(98);committed.state=mga::TransactionState::committed;committed.commit_sequence=1;inv.inventory.entries.push_back(committed);
    const auto owner=inv.inventory.entries.front().identity;auto cp=CheckpointExample(p);const auto seed=LeafExample(profile);page::NativeRowDataPage leaf{seed.header,seed.body};leaf.header.page_type=0x0100;leaf.header.filespace_uuid=fs;
    page::NativeAllocationMap map;map.header={sizes[profile],3,Id(1),fs,Id(70),13,103,0,Profile(profile)};map.object_uuid=Id(43);map.map_generation=5;map.capacity_generation=6;map.total_pages=64;map.creator_transaction_uuid=Id(98);map.creator_local_transaction_id=17;map.states.assign(64,S::quarantined);
    for(unsigned n:{0u,13u,21u}){page::NativeAllocationRecord r;r.page_number=n;r.allocation_uuid=Id(120+n);r.creator_transaction_uuid=Id(98);r.creator_local_transaction_id=17;map.states[n]=S::allocated;
      if(n==0){r.page_uuid=zero.page_uuid;r.page_generation=zero.page_generation;r.page_type=primary?1:2;r.owner_uuid=fs;}
      if(n==13){r.page_uuid=map.header.page_uuid;r.page_generation=103;r.page_type=3;r.owner_uuid=map.object_uuid;}
      if(n==21){r.page_uuid=leaf.header.page_uuid;r.page_generation=7;r.page_type=0x0100;r.owner_uuid=leaf.body.relation_uuid.value;r.creator_transaction_uuid=Id(162);r.creator_local_transaction_id=13;map.states[n]=S::reserved;}
      map.records.push_back(r);}
    page::NativeFilespaceDirectory directory;directory.header={sizes[p],9,Id(1),Id(2),Id(80),15,105,0,Profile(p)};directory.object_uuid=Id(45);directory.directory_generation=5;directory.creator_transaction_uuid=Id(98);directory.creator_local_transaction_id=17;directory.total_records=2;
    for(const auto* z:{&z1,&z2})directory.records.push_back({z->bootstrap,Id(z==&z1?190:191),z->page_uuid,z->page_generation,z->root_set_generation,z->total_pages,0,{}});
    const auto put=[&](auto& file,u64 number,unsigned size,const Bytes& bytes){const auto io=file.WriteAt(number*size,bytes.data(),bytes.size());Check(io.ok()&&io.bytes_transferred==bytes.size()&&file.Sync().ok(),"persist row-data staging fixture bytes");};
    const auto persist=[&](){const auto ib=InventoryOracle(inv,13,13,13),ab=AllocationOracle(map),dbb=DirectoryOracle(directory);
      cp.roots[0].page=InventoryRef(inv);cp.roots[0].object_uuid=inv.object_uuid;cp.roots[0].sha256=WholeRootHash(ib);
      cp.roots[2].page={Id(2),15,105,Profile(p)};cp.roots[2].object_uuid=Id(45);cp.roots[2].sha256=WholeRootHash(dbb);
      if(primary){cp.roots[3].page={fs,13,103,Profile(profile)};cp.roots[3].object_uuid=Id(43);cp.roots[3].sha256=WholeRootHash(ab);}
      put(first,0,sizes[p],Oracle(z1));put(second,0,sizes[q],Oracle(z2));put(first,14,sizes[p],ib);put(first,15,sizes[p],dbb);put(target,13,sizes[profile],ab);put(first,19,sizes[p],CheckpointOracle(cp));};
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};
    const u64 budget=3*sizes[p]+3*sizes[profile];const Bytes blank(sizes[profile],0),expected=LeafOracle({leaf.header,leaf.body});
    const auto reset=[&](){put(target,21,sizes[profile],blank);};
    const auto stage=[&](u64 limit){return db::StageNativeRowDataPageFromOpenDevices(devices,CheckpointRef(cp),owner,leaf,limit);};
    const auto empty=[&](const auto& r){Check(!r.ok()&&!r.receipt,"failed stage returns no receipt");};
    const auto bytes=[&](){Bytes b(sizes[profile]);const auto r=target.ReadAt(21*sizes[profile],b.data(),b.size());Check(r.ok()&&r.bytes_transferred==b.size(),"independent actual staged bytes");return b;};
    persist();reset();reads=observed_full_digests=0;observed_allocations=0;stage_writes=stage_syncs=0;track_reads=count_allocations=count_full_digests=true;
    auto result=stage(budget);track_reads=count_allocations=count_full_digests=false;const auto nr=reads,nf=observed_full_digests;const auto na=observed_allocations;
    if(!result.ok())std::cerr<<"stage error="<<static_cast<int>(result.error)<<" checkpoint="<<static_cast<int>(result.checkpoint_error)<<" allocation="<<static_cast<int>(result.allocation_error)<<" leaf="<<static_cast<int>(result.page_error)<<std::endl;
    Check(result.ok()&&stage_writes==1&&stage_syncs==1&&result.receipt->allocation_uuid==Id(141)&&result.receipt->page_uuid==leaf.header.page_uuid&&result.receipt->transaction.transaction_uuid.value==Id(162)&&result.receipt->page.filespace_uuid==fs&&result.receipt->sha256==WholeRootHash(expected)&&bytes()==expected,"actual reserved canonical row-data receipt and independent bytes");
    stage_writes=stage_syncs=0;result=stage(budget);Check(result.ok()&&!stage_writes&&stage_syncs==1,"exact idempotent stage retry still syncs");
    auto conflicting=expected;conflicting.back()^=1;put(target,21,sizes[profile],conflicting);empty(stage(budget));Check(bytes()==conflicting,"different nonzero destination preserved");
    reset();empty(stage(budget-1));Check(bytes()==blank,"budget exhausted before any write");
    if(p==0&&role==5){
      for(unsigned long n=0;n<=na;++n){reset();allocation_budget=n;result=stage(budget);allocation_budget=-1;if(result.ok())Check(bytes()==expected,"allocation recovery exact staging");else{empty(result);const auto actual=bytes();Check(actual==blank||actual==expected,"allocation failure preserves reserved preimage or written staging, never grants publication");}if(n==na)Check(result.ok(),"allocation sweep terminal staging success");}
      for(unsigned n=1;n<=nr;++n){reset();reads=0;read_fault=n;track_reads=true;result=stage(budget);track_reads=false;Check(!read_fault,"staging read failure consumed");empty(result);}
      for(unsigned n=1;n<=nf;++n){reset();full_digest_fault=n;result=stage(budget);Check(!full_digest_fault,"staging full hash fault consumed");empty(result);Check(bytes()==blank,"hash failure before write");}
      for(unsigned mode=1;mode<=5;++mode){reset();hash_fault=mode;result=stage(budget);Check(!hash_fault,"staging multipart provider fault consumed");empty(result);Check(bytes()==blank,"provider failure before write");}
      for(unsigned mode=1;mode<=2;++mode){reset();stage_write_fault=mode;result=stage(budget);Check(!stage_write_fault,"actual write fault consumed");empty(result);if(mode==2){const auto partial=bytes();empty(stage(budget));Check(bytes()==partial,"partial destination preserved for recovery");}}
      reset();stage_sync_fault=1;result=stage(budget);Check(!stage_sync_fault,"actual stage sync failure consumed");empty(result);Check(bytes()==expected&&stage(budget).ok(),"sync failure retry revalidates existing exact image");
      reset();reads=0;stage_corrupt_read=nr;track_reads=true;result=stage(budget);track_reads=false;Check(!stage_corrupt_read,"actual final readback corruption injected");empty(result);Check(result.error==E::readback_mismatch&&bytes()==expected,"readback corruption never certifies written image");
      std::cout<<"row-data stage allocations="<<na<<" reads="<<nr<<" full digests="<<nf<<std::endl;
    }
    reset();const auto original_map=map;
    for(const auto state:{S::free,S::allocated,S::reusable_pending_mga,S::reusable_free,S::compacting,S::quarantined,S::preallocated}){map=original_map;map.states[21]=state;if(state==S::free)map.records.pop_back();persist();empty(stage(budget));Check(bytes()==blank,"only actual reservation grants stage");}
    map=original_map;for(unsigned n=0;n<5;++n){map=original_map;auto& r=map.records.back();if(n==0)r.owner_uuid=Id(201);if(n==1)r.page_uuid=Id(202);if(n==2)r.page_generation++;if(n==3)r.page_type=512;if(n==4){r.creator_transaction_uuid=Id(98);r.creator_local_transaction_id=17;}persist();result=stage(budget);empty(result);Check(result.error==E::reservation_mismatch&&bytes()==blank,"exact reserved owner/page/type/creator binding");}
    map=original_map;persist();auto wrong_owner=owner;wrong_owner.transaction_uuid.value=Id(203);empty(db::StageNativeRowDataPageFromOpenDevices(devices,CheckpointRef(cp),wrong_owner,leaf,budget));Check(bytes()==blank,"wrong active owner cannot stage");
    for(unsigned n=0;n<4;++n){map=original_map;auto changed=leaf;if(n==0)map.records.back().allocation_uuid=map.records.front().allocation_uuid;else{const auto id=n==1?map.records.front().page_uuid:n==2?cp.header.page_uuid:directory.header.page_uuid;map.records.back().page_uuid=changed.header.page_uuid=id;}persist();result=db::StageNativeRowDataPageFromOpenDevices(devices,CheckpointRef(cp),owner,changed,budget);empty(result);Check(result.error==E::reservation_mismatch&&bytes()==blank,"duplicate allocation/page and known control UUIDs cannot stage");}map=original_map;
    map.records.back().creator_transaction_uuid=Id(98);map.records.back().creator_local_transaction_id=17;persist();result=db::StageNativeRowDataPageFromOpenDevices(devices,CheckpointRef(cp),committed.identity,leaf,budget);empty(result);Check(result.error==E::creator_not_active&&bytes()==blank,"committed transaction cannot perform new staging");map=original_map;persist();
    inv.inventory.entries[0].rollback_only=true;persist();result=stage(budget);empty(result);Check(result.error==E::creator_rollback_only&&bytes()==blank,"rollback-only active owner cannot stage");inv.inventory.entries[0].rollback_only=false;persist();
    const auto pristine=leaf;
    for(unsigned fault=0;fault<4;++fault){leaf=pristine;auto& row=leaf.body.rows.back();
      if(fault==0)row.transaction_uuid.value=Id(203);
      if(fault==1)row.local_transaction_id=99;
      if(fault==2){row.deleted=true;row.transaction_uuid.value=Id(203);}
      if(fault==3){row.deleted=true;row.cells[0].value.type_id=static_cast<scratchbird::core::datatypes::CanonicalTypeId>(0xffffffffu);}
      stage_writes=stage_syncs=0;result=stage(budget);empty(result);
      Check(result.error==(fault==3?E::page_failure:E::row_creator_mismatch)&&!stage_writes&&!stage_syncs&&bytes()==blank,"all retained rows validated before staging");}
    leaf=pristine;
    for(unsigned flag:{1u,2u,4u,8u}){leaf.header.flags=flag;result=stage(budget);empty(result);Check(result.error==E::header_requires_authority&&bytes()==blank,"row header flags cannot grant policy");}
    leaf=pristine;leaf.body.next_page_number=1;result=stage(budget);empty(result);Check(result.error==E::page_failure&&bytes()==blank,"prototype unbound page links cannot stage");leaf=pristine;
    leaf.body.rows.back().transaction_uuid.value=Id(98);leaf.body.rows.back().local_transaction_id=17;
    result=stage(budget);Check(result.ok()&&bytes()==LeafOracle({leaf.header,leaf.body}),"retained committed row creator need not be active");reset();
    const auto current_inventory=inv;auto rolled_back=inv.inventory.entries.front();rolled_back.identity.local_id=mga::MakeLocalTransactionId(18);
    rolled_back.identity.transaction_uuid.value=Id(204);rolled_back.state=mga::TransactionState::rolled_back;rolled_back.commit_sequence=0;
    inv.inventory.next_local_transaction_id=19;inv.inventory.entries.push_back(rolled_back);persist();
    leaf.body.rows.back().transaction_uuid=rolled_back.identity.transaction_uuid;leaf.body.rows.back().local_transaction_id=18;leaf.body.rows.back().deleted=true;
    result=stage(budget);Check(result.ok()&&bytes()==LeafOracle({leaf.header,leaf.body}),"retained rolled-back row is preserved without a visibility claim");
    reset();inv=current_inventory;persist();leaf=pristine;
    result=stage(budget);Check(result.ok(),"final actual row-data staging");const auto target_path=primary?path1:path2;
    Check(target.Close().ok()&&target.Open(target_path,disk::FileOpenMode::open_existing_read_only).ok(),"independent read-only stage reopen");
    Check(bytes()==expected&&page::DecodeNativeRowDataPage(bytes()).ok(),"reopened canonical row-data image");
    const page::NativeRowDataReference reference{leaf.body.relation_uuid.value,{fs,21,7,Profile(profile)},leaf.header.page_uuid};
    const auto retained=page::ReadNativeRowDataPageFromOpenDevice(target,Id(1),reference);
    Check(retained.ok()&&retained.bytes==expected,"actual reopened native row reader");
    auto wrong_reference=reference;wrong_reference.relation_uuid=Id(200);
    const auto denied=page::ReadNativeRowDataPageFromOpenDevice(target,Id(1),wrong_reference);
    Check(denied.error==page::NativeRowDataError::binding_mismatch&&!denied.page&&denied.bytes.empty(),"reopened relation identity bound to caller");
empty(stage(budget));Check(bytes()==expected,"read-only destination unchanged");
  }
}
Bytes SelectionOracle(const db::NativeCheckpointSelection&);
void CanonicalCatalogRootStaging(){using E=db::NativeCatalogRootStageError;using S=page::NativeAllocationState;
  for(unsigned p=0;p<5;++p)for(unsigned role=1;role<=4;++role)for(unsigned kind:{2u,6u,7u,8u}){
    Fixture fixture;disk::FileDevice first,second;const unsigned q=(p+1)%5;
    auto z1=Example(p,role),z2=Example(q);z2.bootstrap.filespace_uuid=Id(7);z2.page_uuid=Id(8);for(auto& r:z2.roots)r.filespace_uuid=Id(7);
    z1.free_pages=z1.preallocated_pages=z2.free_pages=z2.preallocated_pages=0;
    const auto path1=(fixture.root/"root-stage-primary").string(),path2=(fixture.root/"root-stage-history").string();
    Check(first.Open(path1,disk::FileOpenMode::create_new).ok()&&second.Open(path2,disk::FileOpenMode::create_new).ok(),"own root staging devices");
    const byte pad=0;Check(first.WriteAt(64*sizes[p]-1,&pad,1).ok()&&second.WriteAt(64*sizes[q]-1,&pad,1).ok(),"root staging actual capacities");
    auto inv=InventoryExample(p);inv.inventory.next_local_transaction_id=18;inv.inventory.next_commit_sequence=2;
    auto& active=inv.inventory.entries[0];active.identity.local_id=mga::MakeLocalTransactionId(13);active.identity.transaction_uuid.value=Id(162);active.state=mga::TransactionState::active;active.commit_sequence=0;
    auto committed=active;committed.identity.local_id=mga::MakeLocalTransactionId(17);committed.identity.transaction_uuid.value=Id(98);committed.state=mga::TransactionState::committed;committed.commit_sequence=1;inv.inventory.entries.push_back(committed);
    const auto owner=inv.inventory.entries.front().identity;
    auto old=RootExample(q);old.header.filespace_uuid=Id(7);old.root_kind=kind;old.header.page_type=kind==6?10:kind==7?11:5;old.object_uuid=Id(180+kind);old.creator_transaction_uuid=Id(98);old.resource_epoch=1;
    for(auto& r:old.roots)r.page.filespace_uuid=Id(7);
    if(kind!=2){const auto entry=old.roots[kind==6?4:kind==7?3:5];old.roots={entry};}
    auto root=old;root.header={sizes[p],old.header.page_type,Id(1),Id(2),Id(150),30,7,0,Profile(p)};
    root.creator_transaction_uuid=Id(162);root.creator_local_transaction_id=13;root.catalog_generation=2;
    root.predecessor=disk::NativePageReference{Id(7),12,102,Profile(q)};root.predecessor_sha256=WholeRootHash(RootOracle(old));
    auto cp=CheckpointExample(p);const unsigned selected_role=kind==2?5:kind==6?6:kind==7?7:9;
    const bool selected=p==0&&role==1&&kind==2;
    if(selected){z1.roots.push_back({18,0x30e,Id(2),27,1,Profile(p),Id(154)});z1.roots.push_back({19,0x30e,Id(2),28,1,Profile(p),Id(154)});}
    page::NativeAllocationMap map;map.header={sizes[p],3,Id(1),Id(2),Id(70),13,103,0,Profile(p)};map.object_uuid=Id(43);map.map_generation=map.capacity_generation=1;
    map.total_pages=64;map.creator_transaction_uuid=Id(98);map.creator_local_transaction_id=17;map.states.assign(64,S::quarantined);
    for(unsigned n:{0u,13u,30u}){page::NativeAllocationRecord r;r.page_number=n;r.allocation_uuid=Id(200+n);r.creator_transaction_uuid=Id(98);r.creator_local_transaction_id=17;map.states[n]=S::allocated;
      if(n==0){r.page_uuid=z1.page_uuid;r.page_generation=7;r.page_type=1;r.owner_uuid=Id(2);}
      if(n==13){r.page_uuid=Id(70);r.page_generation=103;r.page_type=3;r.owner_uuid=Id(43);}
      if(n==30){r.page_uuid=Id(150);r.page_generation=7;r.page_type=root.header.page_type;r.owner_uuid=root.object_uuid;r.creator_transaction_uuid=Id(162);r.creator_local_transaction_id=13;map.states[n]=S::reserved;}
      map.records.push_back(r);}
    if(selected){map.header.page_number=24;map.header.page_generation=104;map.records[1].page_number=24;map.records[1].page_generation=104;map.states[13]=S::quarantined;map.states[24]=S::allocated;
      for(unsigned n:{27u,28u}){page::NativeAllocationRecord r;r.page_number=n;r.allocation_uuid=Id(220+n);r.page_uuid=Id(n==27?155:156);r.page_generation=1;r.page_type=0x30e;r.owner_uuid=Id(154);r.creator_transaction_uuid=Id(98);r.creator_local_transaction_id=17;map.records.push_back(r);map.states[n]=S::allocated;}
      for(const auto& control:std::vector<db::NativeInventoryPageBinding>{{inv.header,inv.object_uuid},{cp.header,cp.object_uuid}}){
        const auto& h=control.header;map.states[h.page_number]=S::allocated;
        map.records.push_back({h.page_number,Id(100+h.page_number),h.page_uuid,control.object_uuid,Id(98),17,h.page_generation,0,h.page_type,{}});}
      std::sort(map.records.begin(),map.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});}
    page::NativeFilespaceDirectory directory;directory.header={sizes[p],9,Id(1),Id(2),Id(80),15,105,0,Profile(p)};directory.object_uuid=Id(45);directory.directory_generation=1;directory.creator_transaction_uuid=Id(98);directory.creator_local_transaction_id=17;directory.total_records=2;
    const auto put=[&](auto& file,u64 number,unsigned size,const Bytes& b){const auto io=file.WriteAt(number*size,b.data(),b.size());Check(io.ok()&&io.bytes_transferred==b.size()&&file.Sync().ok(),"persist independent root staging fixture");};
    const auto persist=[&](){directory.records.clear();for(const auto* z:{&z1,&z2})directory.records.push_back({z->bootstrap,Id(z==&z1?190:191),z->page_uuid,z->page_generation,z->root_set_generation,z->total_pages,0,{}});
      const auto ib=InventoryOracle(inv,13,13,13),mb=AllocationOracle(map),dbb=DirectoryOracle(directory),prior=RootOracle(old);
      cp.roots[0]={1,0x301,InventoryRef(inv),inv.object_uuid,WholeRootHash(ib)};
      cp.roots[2]={3,9,{Id(2),15,105,Profile(p)},Id(45),WholeRootHash(dbb)};
      cp.roots[3]={4,3,{Id(2),map.header.page_number,map.header.page_generation,Profile(p)},Id(43),WholeRootHash(mb)};
      cp.roots[selected_role-1]={static_cast<disk::u16>(selected_role),old.header.page_type,{Id(7),12,102,Profile(q)},old.object_uuid,WholeRootHash(prior)};
      const auto cb=CheckpointOracle(cp);
      put(first,0,sizes[p],Oracle(z1));put(second,0,sizes[q],Oracle(z2));put(first,14,sizes[p],ib);put(first,map.header.page_number,sizes[p],mb);put(first,15,sizes[p],dbb);put(second,12,sizes[q],prior);put(first,19,sizes[p],cb);
      if(selected){db::NativeCheckpointSelection selection;selection.header={sizes[p],0x30e,Id(1),Id(2),Id(155),27,1,0,Profile(p)};selection.object_uuid=Id(154);selection.bootstrap_uuid=z1.page_uuid;selection.publication_uuid=Id(153);selection.selection_generation=1;selection.checkpoint={Id(2),19,109,Profile(p)};selection.checkpoint_object_uuid=cp.object_uuid;selection.checkpoint_sha256=WholeRootHash(cb);selection.checkpoint_generation=1;selection.root_set_generation=8;selection.timeline_uuid=cp.timeline_uuid;
        put(first,27,sizes[p],SelectionOracle(selection));selection.header.page_number=28;selection.header.page_uuid=Id(156);put(first,28,sizes[p],SelectionOracle(selection));}};
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};
    // A selector-bound genesis retains two selector images and its selected
    // allocation image in addition to the checkpoint/inventory pair. The
    // directory, staging allocation, predecessor and write/readback images
    // are separate work; none may be lost when resolving the selected pair.
    const u64 budget=(selected?9:6)*u64{sizes[p]}+sizes[q];const Bytes blank(sizes[p],0),expected=RootOracle(root);
    const auto reset=[&](){put(first,30,sizes[p],blank);};
    const auto bytes=[&](){Bytes b(sizes[p]);Check(first.ReadAt(30*sizes[p],b.data(),b.size()).ok(),"read actual staged root");return b;};
    const auto stage=[&](u64 b){return db::StageNativeCatalogRootSuccessorFromOpenDevices(devices,CheckpointRef(cp),owner,root,b);};
    const char* phase="initial";
    const auto empty=[&](const auto& r){if(r.ok()||r.receipt)std::cerr<<"unexpected root stage receipt phase="<<phase<<std::endl;Check(!r.ok()&&!r.receipt,"failed root staging returns no receipt");};
    persist();reset();
    if(selected){
      const auto directory_proof=db::VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(Id(1),devices,CheckpointRef(cp),budget);
      Check(directory_proof.ok()&&directory_proof.retained_image_bytes==6*u64{sizes[p]},"selected root staging charges two selectors checkpoint inventory selected map and directory");
      stage_writes=stage_syncs=0;const auto insufficient=stage(6*u64{sizes[p]}+sizes[q]);empty(insufficient);
      Check(insufficient.error==E::resource_exhausted&&!stage_writes&&!stage_syncs&&bytes()==blank,"former uncharged root allowance refuses before durable I/O");
    }
    reads=observed_full_digests=0;observed_allocations=0;stage_writes=stage_syncs=0;track_reads=count_allocations=count_full_digests=true;
    auto result=stage(budget);track_reads=count_allocations=count_full_digests=false;const auto nr=reads,nf=observed_full_digests;const auto na=observed_allocations;
    if(!result.ok())std::cerr<<"root stage error="<<static_cast<int>(result.error)<<" cp="<<static_cast<int>(result.checkpoint_error)<<" root="<<static_cast<int>(result.root_error)<<std::endl;
    Check(result.ok()&&stage_writes==1&&stage_syncs==1&&result.receipt->root_uuid==root.object_uuid&&result.receipt->allocation_uuid==Id(230)&&result.receipt->sha256==WholeRootHash(expected)&&bytes()==expected,"actual staged root receipt and independent bytes");
    stage_writes=stage_syncs=0;Check(stage(budget).ok()&&!stage_writes&&stage_syncs==1,"idempotent root retry still syncs");
    auto conflict=expected;conflict.back()^=1;put(first,30,sizes[p],conflict);empty(stage(budget));Check(bytes()==conflict,"different reserved root data preserved");
    reset();phase="budget";empty(stage(budget-1));Check(bytes()==blank,"root budget checked before write");
    if(p==0&&role==1&&kind==2){
      phase="allocation";
      for(unsigned long n=0;n<=na;++n){reset();allocation_budget=n;result=stage(budget);allocation_budget=-1;if(result.ok())Check(bytes()==expected,"allocation sweep valid root bytes");else{empty(result);const auto b=bytes();Check(b==blank||b==expected,"allocation failure grants no root publication");}if(n==na)Check(result.ok(),"root allocation sweep terminal success");}
      phase="read";for(unsigned n=1;n<=nr;++n){reset();reads=0;read_fault=n;track_reads=true;result=stage(budget);track_reads=false;Check(!read_fault,"root read fault consumed");empty(result);}
      phase="full digest";for(unsigned n=1;n<=nf;++n){reset();full_digest_fault=n;result=stage(budget);Check(!full_digest_fault,"root full digest fault consumed");empty(result);Check(bytes()==blank,"root hash fault before write");}
      phase="multipart";for(unsigned n=1;n<=5;++n){reset();hash_fault=n;result=stage(budget);Check(!hash_fault,"root multipart failure consumed");empty(result);}
      phase="write";for(unsigned mode=1;mode<=3;++mode){reset();stage_write_fault=mode;result=stage(budget);Check(!stage_write_fault,"root write fault consumed");empty(result);
        if(mode==2)Check(bytes()==expected&&stage(budget).ok(),"short write containing all nonzero root bytes allows exact-image retry");
        if(mode==3){const auto partial=bytes();Check(partial!=blank&&partial!=expected,"short root header is genuinely partial");empty(stage(budget));Check(bytes()==partial,"partial root remains for recovery");}}
      reset();stage_sync_fault=1;empty(stage(budget));Check(!stage_sync_fault&&bytes()==expected&&stage(budget).ok(),"root sync failure retry validates actual bytes");
      reset();reads=0;stage_corrupt_read=nr;track_reads=true;result=stage(budget);track_reads=false;Check(!stage_corrupt_read&&result.error==E::readback_mismatch,"root readback corruption consumed");empty(result);
      std::cout<<"root stage allocations="<<na<<" reads="<<nr<<" digests="<<nf<<std::endl;
    }
    reset();const auto saved=root;
    for(unsigned n=0;n<9;++n){root=saved;if(n==0)root.predecessor->page_number++;if(n==1)root.predecessor_sha256[0]^=1;if(n==2)root.catalog_generation++;if(n==3)root.schema_epoch--;if(n==4)root.security_epoch--;if(n==5)root.resource_epoch--;if(n==6)root.creator_transaction_uuid=Id(98);if(n==7)root.header.flags=1;if(n==8)root.roots[0].role=0;empty(stage(budget));Check(bytes()==blank,"invalid root successor preserved destination");}
    root=saved;const auto original_map=map;
    for(unsigned n=0;n<6;++n){map=original_map;if(n==0)map.states[30]=S::allocated;if(n==1)map.records.back().owner_uuid=Id(202);if(n==2)map.records.back().page_generation++;if(n==3)map.records.back().page_uuid=Id(203);if(n==4)map.records.back().page_type=6;if(n==5)map.records.back().allocation_uuid=map.records.front().allocation_uuid;persist();empty(stage(budget));Check(bytes()==blank,"invalid reservation cannot stage root");}
    map=original_map;root.header.page_uuid=old.header.page_uuid;map.records.back().page_uuid=old.header.page_uuid;persist();empty(stage(budget));Check(bytes()==blank,"new root cannot reuse predecessor page UUID");root=saved;map=original_map;
    const auto prior_saved=old;
    old.header.flags=2;root.predecessor_sha256=WholeRootHash(RootOracle(old));persist();result=stage(budget);empty(result);Check(result.error==E::header_requires_authority,"predecessor header policy is not silently cleared");old=prior_saved;root=saved;
    z2.bootstrap.flags=disk::FilespaceBootstrapFlag::cluster_authority_required;persist();result=stage(budget);empty(result);Check(result.error==E::cluster_requires_authority,"cluster-owned predecessor requires its provider");z2.bootstrap.flags=0;
    z2.bootstrap.flags=disk::FilespaceBootstrapFlag::payload_encrypted;z2.bootstrap.encryption_profile_uuid=Id(4);persist();result=stage(budget);empty(result);Check(result.error==E::header_requires_authority,"encrypted predecessor requires crypto authority");z2.bootstrap.flags=0;z2.bootstrap.encryption_profile_uuid={};
    if(kind==8){old.root_kind=2;old.roots=RootExample(q).roots;for(auto& r:old.roots)r.page.filespace_uuid=Id(7);root.predecessor_sha256=WholeRootHash(RootOracle(old));persist();result=stage(budget);empty(result);Check(result.error==E::predecessor_mismatch,"shared feature root is not a dedicated-root predecessor");old=prior_saved;root=saved;}
    old.creator_transaction_uuid=Id(162);old.creator_local_transaction_id=13;root.predecessor_sha256=WholeRootHash(RootOracle(old));persist();result=stage(budget);empty(result);Check(result.error==E::predecessor_creator_mismatch,"uncommitted predecessor cannot authorize successor");
    old=prior_saved;root=saved;inv.inventory.entries[0].rollback_only=true;persist();result=stage(budget);empty(result);Check(result.error==E::creator_rollback_only,"root writer rollback-only exclusion");inv.inventory.entries[0].rollback_only=false;persist();
    if(p==0&&role==1&&kind==2){auto duplicate=devices;duplicate.push_back(devices.front());empty(db::StageNativeCatalogRootSuccessorFromOpenDevices(duplicate,CheckpointRef(cp),owner,root,budget));
      auto reverse=devices;std::reverse(reverse.begin(),reverse.end());std::atomic<unsigned> completed{0};stage_writes=stage_syncs=0;
      std::thread a([&]{if(stage(budget).ok())++completed;}),b([&]{if(db::StageNativeCatalogRootSuccessorFromOpenDevices(reverse,CheckpointRef(cp),owner,root,budget).ok())++completed;});
      a.join();b.join();Check(completed==2&&stage_writes==1&&stage_syncs==2,"opposite-order root writers serialize one write and an exact retry");}
    Check(stage(budget).ok(),"final root staging");Bytes prior(sizes[q]);Check(second.ReadAt(12*sizes[q],prior.data(),prior.size()).ok()&&prior==RootOracle(old),"published predecessor remains unchanged");
    Bytes checkpoint_bytes(sizes[p]);Check(first.ReadAt(19*sizes[p],checkpoint_bytes.data(),checkpoint_bytes.size()).ok()&&checkpoint_bytes==CheckpointOracle(cp),"staging never publishes a checkpoint");
    Check(first.Close().ok()&&first.Open(path1,disk::FileOpenMode::open_existing_read_only).ok(),"independent root staging reopen");Check(bytes()==expected&&page::DecodeNativeCatalogRoot(bytes()).ok(),"reopened root exact image");empty(stage(budget));
    Check(first.Close().ok()&&second.Close().ok(),"release root filespaces for fresh executable");const auto child=::fork();Check(child>=0,"fork actual root reader");
    if(child==0){const auto profile=std::to_string(p),family=std::to_string(kind);::execl("/proc/self/exe","root-stage-probe","--catalog-root-stage-probe",fixture.root.c_str(),profile.c_str(),family.c_str(),nullptr);::_exit(125);}
    int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable resolves staged root and unchanged predecessor");
  }
}

void CanonicalBtreeStaging(){using E=db::NativeBtreeStageError;using S=page::NativeAllocationState;
  for(unsigned p=0;p<5;++p)for(unsigned role:{5u,6u})for(unsigned variant=0;variant<5;++variant){
    Fixture fixture;disk::FileDevice first,second;const unsigned q=(p+1)%5;
    auto z1=Example(p),z2=Example(q,role);z2.bootstrap.filespace_uuid=Id(7);z2.page_uuid=Id(8);for(auto& r:z2.roots)r.filespace_uuid=Id(7);
    z1.free_pages=z1.preallocated_pages=z2.free_pages=z2.preallocated_pages=0;
    const auto path1=(fixture.root/"btree-stage-primary").string(),path2=(fixture.root/"btree-stage-secondary").string();
    Check(first.Open(path1,disk::FileOpenMode::create_new).ok()&&second.Open(path2,disk::FileOpenMode::create_new).ok(),"own B-tree staging filespaces");
    const byte pad=0;Check(first.WriteAt(64*sizes[p]-1,&pad,1).ok()&&second.WriteAt(64*sizes[q]-1,&pad,1).ok(),"actual B-tree capacities");
    auto inv=InventoryExample(p);inv.inventory.next_local_transaction_id=18;inv.inventory.next_commit_sequence=2;
    auto& writer=inv.inventory.entries[0];writer.identity.local_id=mga::MakeLocalTransactionId(13);writer.identity.transaction_uuid.value=Id(162);writer.state=mga::TransactionState::active;writer.commit_sequence=0;
    auto committed=writer;committed.identity.local_id=mga::MakeLocalTransactionId(17);committed.identity.transaction_uuid.value=Id(98);committed.state=mga::TransactionState::committed;committed.commit_sequence=1;inv.inventory.entries.push_back(committed);
    const auto owner=inv.inventory.entries.front().identity;auto cp=CheckpointExample(p);
    auto node=BtreeExample(q,variant<3?0x200:variant==3?0x201:0x202,variant==2||variant==3);
    node.header.filespace_uuid=Id(7);node.header.page_number=30;node.creator_transaction_uuid=Id(162);node.creator_local_transaction_id=13;
    node.dependencies.index_uuid=Id(180);node.dependencies.key_profile_uuid=Id(181);node.dependencies.visibility_profile_uuid=Id(182);node.dependencies.dependency_map_uuid=Id(183);
    const auto relocate=[&](auto& ref){if(ref){ref->filespace_uuid=Id(7);ref->page_size_profile_uuid=Profile(q);}};
    relocate(node.parent);relocate(node.left);relocate(node.right);relocate(node.first_child);
    for(auto& cell:node.cells){relocate(cell.child);if(cell.base_page){cell.base_page->filespace_uuid=Id(2);cell.base_page->page_size_profile_uuid=Profile(p);}}
    if(variant==0)node.cells.clear();auto dependencies=node.dependencies;
    page::NativeAllocationMap map;map.header={sizes[q],3,Id(1),Id(7),Id(70),13,103,0,Profile(q)};map.object_uuid=Id(43);map.map_generation=map.capacity_generation=1;map.total_pages=64;
    map.creator_transaction_uuid=Id(98);map.creator_local_transaction_id=17;map.states.assign(64,S::quarantined);
    for(unsigned n:{0u,13u,30u}){page::NativeAllocationRecord r;r.page_number=n;r.allocation_uuid=Id(200+n);r.creator_transaction_uuid=Id(98);r.creator_local_transaction_id=17;map.states[n]=S::allocated;
      if(n==0){r.page_uuid=Id(8);r.page_generation=7;r.page_type=2;r.owner_uuid=Id(7);}
      if(n==13){r.page_uuid=Id(70);r.page_generation=103;r.page_type=3;r.owner_uuid=Id(43);}
      if(n==30){r.page_uuid=node.header.page_uuid;r.page_generation=node.header.page_generation;r.page_type=node.header.page_type;r.owner_uuid=node.dependencies.index_uuid;r.creator_transaction_uuid=Id(162);r.creator_local_transaction_id=13;map.states[n]=S::reserved;}map.records.push_back(r);}
    page::NativeFilespaceDirectory directory;directory.header={sizes[p],9,Id(1),Id(2),Id(150),15,105,0,Profile(p)};directory.object_uuid=Id(45);directory.directory_generation=1;
    directory.creator_transaction_uuid=Id(98);directory.creator_local_transaction_id=17;directory.total_records=2;
    const auto put=[&](auto& device,u64 number,unsigned size,const Bytes& b){const auto io=device.WriteAt(number*size,b.data(),b.size());Check(io.ok()&&io.bytes_transferred==b.size()&&device.Sync().ok(),"persist independent B-tree staging fixture");};
    const auto persist=[&](){directory.records.clear();for(const auto* z:{&z1,&z2})directory.records.push_back({z->bootstrap,Id(z==&z1?190:191),z->page_uuid,z->page_generation,z->root_set_generation,z->total_pages,0,{}});
      const auto ib=InventoryOracle(inv,13,13,13),mb=AllocationOracle(map),dbb=DirectoryOracle(directory);
      cp.roots[0]={1,0x301,InventoryRef(inv),inv.object_uuid,WholeRootHash(ib)};cp.roots[2]={3,9,{Id(2),15,105,Profile(p)},Id(45),WholeRootHash(dbb)};
      put(first,0,sizes[p],Oracle(z1));put(second,0,sizes[q],Oracle(z2));put(first,14,sizes[p],ib);put(first,15,sizes[p],dbb);put(second,13,sizes[q],mb);put(first,19,sizes[p],CheckpointOracle(cp));};
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};
    const u64 budget=3*sizes[p]+3*sizes[q];const Bytes blank(sizes[q],0),expected=BtreeOracle(node);
    const auto reset=[&](){put(second,30,sizes[q],blank);};
    const auto bytes=[&](){Bytes b(sizes[q]);const auto io=second.ReadAt(30*sizes[q],b.data(),b.size());Check(io.ok()&&io.bytes_transferred==b.size(),"read actual staged B-tree bytes");return b;};
    const auto stage=[&](u64 limit){return db::StageNativeBtreePageFromOpenDevices(devices,CheckpointRef(cp),owner,dependencies,node,limit);};
    const auto empty=[&](const auto& r){Check(!r.ok()&&!r.receipt,"B-tree stage failure has no receipt");};
    persist();reset();reads=observed_full_digests=0;observed_allocations=0;stage_writes=stage_syncs=0;track_reads=count_allocations=count_full_digests=true;auto result=stage(budget);track_reads=count_allocations=count_full_digests=false;
    const auto na=observed_allocations;const auto nr=reads,nf=observed_full_digests;
    if(!result.ok())std::cerr<<"B-tree stage error="<<static_cast<int>(result.error)<<" cp="<<static_cast<int>(result.checkpoint_error)<<" page="<<static_cast<int>(result.page_error)<<std::endl;
    Check(result.ok()&&stage_writes==1&&stage_syncs==1&&result.receipt->index_uuid==dependencies.index_uuid&&result.receipt->allocation_uuid==Id(230)&&result.receipt->page_uuid==node.header.page_uuid&&result.receipt->transaction.transaction_uuid.value==Id(162)&&result.receipt->sha256==WholeRootHash(expected)&&bytes()==expected,"actual B-tree staged receipt and complete bytes");
    stage_writes=stage_syncs=0;Check(stage(budget).ok()&&!stage_writes&&stage_syncs==1,"B-tree exact retry still syncs");
    auto conflict=expected;conflict.back()^=1;put(second,30,sizes[q],conflict);empty(stage(budget));Check(bytes()==conflict,"nonzero competing B-tree page preserved");reset();empty(stage(budget-1));Check(bytes()==blank,"B-tree budget exhausted before write");
    if(p==0&&role==5&&variant==1){
      for(unsigned long n=0;n<=na;++n){reset();allocation_budget=n;result=stage(budget);allocation_budget=-1;if(result.ok())Check(bytes()==expected,"B-tree allocation sweep exact image");else{empty(result);const auto b=bytes();Check(b==blank||b==expected,"B-tree allocation fault does not certify publication");}if(n==na)Check(result.ok(),"B-tree allocation sweep terminal success");}
      for(unsigned n=1;n<=nr;++n){reset();reads=0;read_fault=n;track_reads=true;result=stage(budget);track_reads=false;Check(!read_fault,"B-tree read fault consumed");empty(result);}
      for(unsigned n=1;n<=nf;++n){reset();full_digest_fault=n;result=stage(budget);Check(!full_digest_fault,"B-tree full hash fault consumed");empty(result);Check(bytes()==blank,"B-tree digest failure before write");}
      for(unsigned mode=1;mode<=5;++mode){reset();hash_fault=mode;result=stage(budget);Check(!hash_fault,"B-tree multipart fault consumed");empty(result);}
      for(unsigned mode=1;mode<=3;++mode){reset();stage_write_fault=mode;result=stage(budget);Check(!stage_write_fault,"B-tree write fault consumed");empty(result);if(mode==2)Check(bytes()==expected&&stage(budget).ok(),"half-page containing full B-tree payload can retry exactly");if(mode==3){const auto partial=bytes();Check(partial!=blank&&partial!=expected,"genuinely partial B-tree header");empty(stage(budget));Check(bytes()==partial,"partial B-tree data preserved for recovery");}}
      reset();stage_sync_fault=1;empty(stage(budget));Check(!stage_sync_fault&&bytes()==expected&&stage(budget).ok(),"B-tree sync failure exact retry");
      reset();reads=0;stage_corrupt_read=nr;track_reads=true;result=stage(budget);track_reads=false;Check(!stage_corrupt_read&&result.error==E::readback_mismatch,"B-tree readback mismatch consumed");empty(result);
      std::cout<<"B-tree stage allocations="<<na<<" reads="<<nr<<" digests="<<nf<<std::endl;
    }
    reset();const auto saved=node;const auto original_dependencies=dependencies;
    for(unsigned n=0;n<7;++n){dependencies=original_dependencies;if(n==0)dependencies.index_uuid=Id(184);if(n==1)dependencies.descriptor_generation++;if(n==2)dependencies.storage_generation++;if(n==3)dependencies.key_profile_uuid=Id(184);if(n==4)dependencies.visibility_profile_uuid=Id(184);if(n==5)dependencies.dependency_map_uuid=Id(184);if(n==6)dependencies.dependency_map_sha256[0]^=1;result=stage(budget);empty(result);Check(result.error==E::dependency_mismatch&&bytes()==blank,"every expected dependency member is exact");}
    dependencies=original_dependencies;node.dependencies.dependency_map_sha256={};dependencies=node.dependencies;result=stage(budget);empty(result);Check(result.error==E::page_failure&&result.page_error==page::NativeBtreeError::invalid_dependencies&&bytes()==blank,"tuple equality does not bypass canonical dependency validation");node=saved;dependencies=original_dependencies;
    for(unsigned n=0;n<5;++n){node=saved;if(n==0)node.creator_transaction_uuid=Id(98);if(n==1)node.creator_local_transaction_id=17;if(n==2)node.header.flags=2;if(n==3)node.maintenance_state=0;if(n==4)node.dependencies.dependency_map_sha256={};empty(stage(budget));Check(bytes()==blank,"invalid B-tree writer/header/family does not write");}node=saved;
    const auto original_map=map;
    for(unsigned n=0;n<7;++n){map=original_map;if(n==0)map.states[30]=S::allocated;if(n==1)map.records.back().owner_uuid=Id(202);if(n==2)map.records.back().page_generation++;if(n==3)map.records.back().page_type=6;if(n==4)map.records.back().page_uuid=Id(203);if(n==5)map.records.back().allocation_uuid=map.records.front().allocation_uuid;if(n==6){map.creator_transaction_uuid=Id(162);map.creator_local_transaction_id=13;}persist();empty(stage(budget));Check(bytes()==blank,"invalid B-tree allocation authority");}map=original_map;
    inv.inventory.entries[0].rollback_only=true;persist();result=stage(budget);empty(result);Check(result.error==E::creator_rollback_only,"B-tree rollback-only writer refused");inv.inventory.entries[0].rollback_only=false;
    node.creator_transaction_uuid=Id(98);node.creator_local_transaction_id=17;map.records.back().creator_transaction_uuid=Id(98);map.records.back().creator_local_transaction_id=17;persist();
    result=db::StageNativeBtreePageFromOpenDevices(devices,CheckpointRef(cp),committed.identity,dependencies,node,budget);empty(result);Check(result.error==E::creator_not_active&&bytes()==blank,"committed creator cannot perform new B-tree staging");node=saved;map=original_map;
    z2.bootstrap.flags=disk::FilespaceBootstrapFlag::payload_encrypted;z2.bootstrap.encryption_profile_uuid=Id(4);persist();result=stage(budget);empty(result);Check(result.error==E::header_requires_authority&&bytes()==blank,"actual encrypted B-tree destination not bypassed");z2.bootstrap.flags=0;z2.bootstrap.encryption_profile_uuid={};
    const auto original_zero=z2;for(unsigned bad_role:{1u,4u,7u,14u}){z2=Example(q,bad_role);z2.bootstrap.filespace_uuid=Id(7);z2.page_uuid=Id(8);z2.free_pages=z2.preallocated_pages=0;for(auto& r:z2.roots)r.filespace_uuid=Id(7);persist();result=stage(budget);empty(result);Check(result.error==E::invalid_destination&&bytes()==blank,"B-tree rejects illegal destination role before map use");}z2=original_zero;persist();
    if(p==0&&role==5&&variant==1){auto duplicate=devices;duplicate.push_back(devices.front());empty(db::StageNativeBtreePageFromOpenDevices(duplicate,CheckpointRef(cp),owner,dependencies,node,budget));
      auto reverse=devices;std::reverse(reverse.begin(),reverse.end());std::atomic<unsigned> completed{0};stage_writes=stage_syncs=0;
      std::thread a([&]{if(stage(budget).ok())++completed;}),b([&]{if(db::StageNativeBtreePageFromOpenDevices(reverse,CheckpointRef(cp),owner,dependencies,node,budget).ok())++completed;});a.join();b.join();Check(completed==2&&stage_writes==1&&stage_syncs==2,"opposite-order B-tree writers preserve serialized exact retry");}
    Check(stage(budget).ok(),"final B-tree staging");Bytes checkpoint(sizes[p]);Check(first.ReadAt(19*sizes[p],checkpoint.data(),checkpoint.size()).ok()&&checkpoint==CheckpointOracle(cp),"B-tree stage does not select roots");
    Check(second.Close().ok()&&second.Open(path2,disk::FileOpenMode::open_existing_read_only).ok(),"independent B-tree staging reopen");Check(bytes()==expected&&page::ReadNativeBtreePageFromOpenDevice(second,Id(1),{Id(7),30,node.header.page_generation,Profile(q)},node.header.page_type,dependencies).ok(),"staged B-tree accepted by actual reader");empty(stage(budget));
    Check(first.Close().ok()&&second.Close().ok(),"release node for fresh B-tree process");const auto child=::fork();Check(child>=0,"fork actual staged B-tree reader");
    if(child==0){const auto profile=std::to_string(p),shape=std::to_string(variant);::execl("/proc/self/exe","btree-stage-probe","--btree-stage-probe",fixture.root.c_str(),profile.c_str(),shape.c_str(),nullptr);::_exit(125);}
    int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable resolves staged B-tree");
  }
}
void CanonicalCheckpointDirectory(bool extended=false) {
  using E=db::NativeCheckpointError;
  const auto image=[](const page::NativeFilespaceDirectory& d){return !d.creator_operation_uuid.is_nil()||std::any_of(d.records.begin(),d.records.end(),[](const auto& r){return r.allocation_root.has_value();})?DirectoryAllocationOracle(d):DirectoryOracle(d);};
  for(unsigned p=0;p<5;++p){const unsigned q=(p+1)%5;Fixture fixture;disk::FileDevice first,second;
    const auto path1=(fixture.root/"checkpoint-directory-primary").string(),path2=(fixture.root/"checkpoint-directory-secondary").string();
    auto z1=Example(p),z2=Example(q);z2.bootstrap.filespace_uuid=Id(7);z2.page_uuid=Id(8);for(auto& root:z2.roots)root.filespace_uuid=Id(7);
    Check(first.Open(path1,disk::FileOpenMode::create_new).ok()&&second.Open(path2,disk::FileOpenMode::create_new).ok(),"own actual checkpoint directory filespaces");
    const byte pad=0;Check(first.WriteAt(z1.total_pages*sizes[p]-1,&pad,1).ok()&&second.WriteAt(z2.total_pages*sizes[q]-1,&pad,1).ok(),"actual checkpoint directory capacities");
    auto inv=InventoryExample(p);inv.inventory.next_local_transaction_id=18;inv.inventory.next_commit_sequence=3;
    auto& initial=inv.inventory.entries.front();initial.identity.local_id=mga::MakeLocalTransactionId(11);initial.identity.transaction_uuid.value=Id(91);
    initial.state=mga::TransactionState::committed;initial.commit_sequence=1;
    auto active=initial;active.identity.local_id=mga::MakeLocalTransactionId(16);active.identity.transaction_uuid.value=Id(99);active.state=mga::TransactionState::active;active.commit_sequence=0;
    auto creator=initial;creator.identity.local_id=mga::MakeLocalTransactionId(17);creator.identity.transaction_uuid.value=Id(98);creator.commit_sequence=2;
    inv.inventory.entries.push_back(active);inv.inventory.entries.push_back(creator);
    const auto checkpoint=CheckpointExample(p);
    const auto record=[](const auto& zero,const Uuid& locator){return page::NativeFilespaceDirectoryRecord{zero.bootstrap,locator,zero.page_uuid,zero.page_generation,zero.root_set_generation,zero.total_pages,0,{}};};
    page::NativeFilespaceDirectory head;
    head.header={sizes[p],9,Id(1),Id(2),Id(80),15,105,0,Profile(p)};head.object_uuid=Id(45);head.directory_generation=5;
    head.creator_transaction_uuid=Id(91);head.creator_local_transaction_id=11;head.total_records=2;head.records={record(z1,Id(120))};
    auto tail=head;tail.header={sizes[q],9,Id(1),Id(7),Id(81),15,105,0,Profile(q)};tail.first_record=1;tail.records={record(z2,Id(121))};
    if(extended){page::NativeFilespaceAllocationRoot binding{{Id(2),13,103,Profile(p)},Id(43),{},2,1};binding.sha256.fill(0x5a);
      head.records[0].allocation_root=binding;
      if(p%2){binding.page.filespace_uuid=Id(7);binding.page.page_size_profile_uuid=Profile(q);binding.object_uuid=Id(179);tail.records[0].allocation_root=binding;}}
    head.next=disk::NativePageReference{Id(7),15,105,Profile(q)};head.next_sha256=WholeRootHash(image(tail));
    const auto put=[&](auto& device,u64 number,unsigned size,const Bytes& bytes){const auto io=device.WriteAt(number*size,bytes.data(),bytes.size());
      Check(io.ok()&&io.bytes_transferred==bytes.size()&&device.Sync().ok(),"persist independent checkpoint directory fixture");};
    const auto persist=[&](const auto& inventory,const auto& primary,auto h,auto t,bool stale=false){
      const auto allocation=h.records[0].allocation_root;h.records[0]=record(primary,Id(120));h.records[0].allocation_root=allocation;h.next_sha256=WholeRootHash(image(t));
      auto cp=checkpoint;const auto ib=InventoryStateOracle(inventory),hb=image(h);
      cp.roots[0].page=InventoryRef(inventory);cp.roots[0].object_uuid=inventory.object_uuid;cp.roots[0].sha256=WholeRootHash(ib);
      auto& target=cp.roots[2];target.page={Id(2),15,105,Profile(p)};target.object_uuid=Id(45);target.sha256=WholeRootHash(stale?image(head):hb);
      put(first,0,sizes[p],Oracle(primary));put(second,0,sizes[q],Oracle(z2));put(first,14,sizes[p],ib);
      put(first,15,sizes[p],hb);put(second,15,sizes[q],image(t));put(first,19,sizes[p],CheckpointOracle(cp));
    };
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};
    const u64 limit=3*sizes[p]+sizes[q];const auto read=[&](u64 budget){return db::VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(Id(1),devices,CheckpointRef(checkpoint),budget);};
    const auto empty=[&](const auto& r){Check(!r.ok()&&!r.retained_image_bytes&&r.directory.pages.empty()&&!r.directory.retained_image_bytes&&
      !r.checkpoint_inventory.checkpoint&&r.checkpoint_inventory.inventory.entries.empty()&&!r.checkpoint_inventory.inventory.publication_base&&
      !r.checkpoint_inventory.retained_image_bytes,"checkpoint directory failure exposes no authority prefix");};
    persist(inv,z1,head,tail);
    if(p==0){byte warm=0;for(unsigned n=0;n<4097;++n)Check(first.ReadAt(0,&warm,1).ok()&&second.ReadAt(0,&warm,1).ok(),"warm directory reader telemetry before complete fault measurement");}
    reads=observed_allocations=observed_full_digests=0;count_allocations=track_reads=count_full_digests=true;auto result=read(limit);track_reads=count_allocations=count_full_digests=false;
    const auto nr=reads,nf=observed_full_digests;const auto na=observed_allocations;
    Check(result.ok()&&result.retained_image_bytes==limit&&result.directory.pages.size()==2&&result.directory.pages[0].bytes==image(head)&&
      result.directory.pages[1].bytes==image(tail)&&!result.checkpoint_inventory.inventory.publication_base,"actual current checkpoint directory inventory binding");
    Check(result.directory.pages[0].directory->records[0].allocation_root==head.records[0].allocation_root&&result.directory.pages[1].directory->records[0].allocation_root==tail.records[0].allocation_root,"actual mixed-version directory chain retains every allocation descriptor");
    result=read(limit-1);empty(result);Check(result.error==E::directory_failure&&result.directory_error==page::NativeDirectoryError::resource_exhausted,"shared budget charges final directory image");
    for(unsigned fault=1;fault<=nr;++fault){reads=0;read_fault=fault;track_reads=true;result=read(limit);track_reads=false;const auto unconsumed=read_fault;read_fault=0;Check(!unconsumed,"every measured directory read failure is consumed");empty(result);}
    for(unsigned fault=1;fault<=nf;++fault){full_digest_fault=fault;result=read(limit);Check(full_digest_fault==0&&(result.error==E::hash_failure||result.directory_error==page::NativeDirectoryError::hash_failure),"checkpoint inventory directory complete hash provider failure");empty(result);}
    for(unsigned fault=1;fault<=5;++fault){hash_fault=fault;result=read(limit);Check(hash_fault==0,"checkpoint directory part hash fault consumed");empty(result);}
    if(p==0){unsigned long consumed=0;for(unsigned long n=0;n<=na;++n){const auto loss=first.failed_io_latency_observations()+second.failed_io_latency_observations();
        allocation_budget=n;result=read(limit);const auto remaining=allocation_budget;allocation_budget=-1;
        if(n<na){Check(remaining<0,"every measured directory allocation failure is consumed");++consumed;}
        if(result.ok()){Check(result.retained_image_bytes==limit&&result.directory.pages[0].bytes==image(head)&&result.directory.pages[1].bytes==image(tail),"allocation sweep exact directory authority");
          if(remaining<0)Check(first.failed_io_latency_observations()+second.failed_io_latency_observations()==loss+1,"only recorded optional telemetry failure preserves directory read success");}
        else{empty(result);Check(result.error==E::resource_exhausted||result.directory_error==page::NativeDirectoryError::resource_exhausted||result.checkpoint_inventory.inventory_error==page::NativeInventoryError::resource_exhausted,"directory allocation failure keeps resource classification");}
        if(n==na)Check(result.ok()&&remaining>=0,"measured checkpoint directory allocation sweep terminal uninjected success");}
      Check(consumed==na,"complete directory allocation failure coverage");
      std::cout<<"checkpoint directory allocation sites="<<na<<" consumed="<<consumed<<" reads="<<nr<<" digests="<<nf<<std::endl;}
    for(unsigned change=0;change<3;++change){auto z=z1;if(change==0)z.root_set_generation++;if(change==1)z.roots[8].object_uuid=Id(199);if(change==2)z.roots[4].object_uuid=Id(199);
      persist(inv,z,head,tail);result=read(limit);empty(result);Check(result.error==E::binding_mismatch,"current primary checkpoint and directory roots exact");}
    for(unsigned change=0;change<3;++change){auto h=head,t=tail;if(change==0)h.creator_transaction_uuid=t.creator_transaction_uuid=Id(199);
      if(change==1)h.creator_local_transaction_id=t.creator_local_transaction_id=10;
      if(change==2){h.creator_transaction_uuid=t.creator_transaction_uuid=Id(99);h.creator_local_transaction_id=t.creator_local_transaction_id=16;}
      persist(inv,z1,h,t);result=read(limit);empty(result);Check(result.error==(change==2?E::directory_creator_not_committed:E::directory_creator_mismatch),"actual directory creator identity and committed outcome required");}
    for(auto origin:{mga::TransactionState::committed,mga::TransactionState::rolled_back,mga::TransactionState::failed_terminal}){
      auto changed=inv;auto& e=changed.inventory.entries[0];e.state=mga::TransactionState::archived;e.archived_from_state=origin;if(origin!=mga::TransactionState::committed)e.commit_sequence=0;
      persist(changed,z1,head,tail);result=read(limit);if(origin==mga::TransactionState::committed)Check(result.ok(),"archived committed directory creator admitted");
      else{empty(result);Check(result.error==E::directory_creator_not_committed,"archived noncommit cannot authorize directory");}}
    auto changed=inv;changed.inventory.entries[0].identity.scope=mga::TransactionScope::cluster_global;
    persist(changed,z1,head,tail);result=read(limit);empty(result);Check(result.error==E::directory_creator_mismatch,"cluster creator cannot certify standalone directory");
    auto h=head,t=tail;h.directory_generation++;t.directory_generation++;persist(inv,z1,h,t,true);result=read(limit);empty(result);Check(result.error==E::invalid_integrity,"resealed directory still binds exact checkpoint digest");
    if(extended){auto oh=head,ot=tail;for(auto* d:{&oh,&ot}){d->creator_transaction_uuid={};d->creator_local_transaction_id=0;d->creator_operation_uuid=Id(248);}
      const auto structural=[&]{return page::ReadNativeFilespaceDirectoryFromOpenDevices(Id(1),devices,{5,9,Id(2),15,105,Profile(p),Id(45)},sizes[p]+sizes[q]);};
      persist(inv,z1,oh,ot);const auto operation=structural();
      Check(operation.ok()&&operation.pages.size()==2&&operation.pages[0].directory->creator_operation_uuid==Id(248)&&operation.pages[1].directory->creator_operation_uuid==Id(248),"actual operation directory chain retains exclusive binary creator");
      empty(read(limit)); // Structural identity alone is never an operation publication proof.
      ot.creator_operation_uuid=Id(249);persist(inv,z1,oh,ot);const auto mismatch=structural();
      Check(!mismatch.ok()&&mismatch.error==page::NativeDirectoryError::chain_mismatch&&mismatch.pages.empty()&&!mismatch.retained_image_bytes,"actual directory chain cannot splice distinct operation creators");
    }
    persist(inv,z1,head,tail);Check(first.Close().ok()&&second.Close().ok()&&first.Open(path1,disk::FileOpenMode::open_existing_read_only).ok()&&
      second.Open(path2,disk::FileOpenMode::open_existing_read_only).ok(),"checkpoint directory owned read-only reopen");
    Check(read(limit).ok()&&first.read_only()&&second.read_only(),"reopened current directory and inventory verified");
  }
}

Bytes HorizonOracle(const page::NativeHorizonRoot& v){auto common=RootExample();common.header=v.header;auto b=RootOracle(common);std::fill(b.begin()+128,b.end(),0);
  const auto ref=[&](std::size_t at,const disk::NativePageReference& r){PutUuid(b,at,r.filespace_uuid);Number(b,at+16,8,r.page_number);Number(b,at+24,8,r.page_generation);PutUuid(b,at+32,r.page_size_profile_uuid);};
  std::copy_n("SBHOR002",8,b.begin()+128);Number(b,136,2,2);Number(b,138,2,384);Number(b,140,4,512+192*v.records.size());PutUuid(b,144,v.object_uuid);Number(b,160,8,v.epoch);PutUuid(b,168,v.creator_transaction_uuid);Number(b,184,8,v.creator_local_transaction_id);
  Number(b,192,8,v.flags);Number(b,200,8,v.total_records);Number(b,208,8,v.first_record);Number(b,216,4,v.records.size());ref(224,v.retention);PutUuid(b,272,v.retention_object_uuid);std::copy(v.retention_sha256.begin(),v.retention_sha256.end(),b.begin()+288);
  if(v.next)ref(320,*v.next);std::copy(v.next_sha256.begin(),v.next_sha256.end(),b.begin()+368);Number(b,400,8,v.minimum_blocker);
  for(std::size_t i=0;i<v.records.size();++i){const auto at=512+i*192;const auto& r=v.records[i];Number(b,at,2,static_cast<disk::u16>(r.kind));Number(b,at+2,2,static_cast<disk::u16>(r.owner_kind));Number(b,at+4,4,r.flags);Number(b,at+8,8,r.local_boundary);
    PutUuid(b,at+16,r.owner_uuid);PutUuid(b,at+32,r.pin_uuid);PutUuid(b,at+48,r.checkpoint_object_uuid);Number(b,at+64,8,r.checkpoint_generation);PutUuid(b,at+72,r.diagnostic_uuid);if(r.checkpoint)ref(at+88,*r.checkpoint);PutUuid(b,at+136,r.horizon_uuid);PutUuid(b,at+152,r.timeline_uuid);}
  const auto digest=WholeRootHash(b);std::copy(digest.begin(),digest.end(),b.begin()+408);return b;
}
Bytes RetentionOracle(const page::NativeRetentionPage& v){const auto& h=v.header;Bytes b(h.page_size_bytes,0);std::copy_n("SBPGV002",8,b.begin());Number(b,8,4,128);Number(b,12,4,h.page_size_bytes);Number(b,16,4,h.page_type);Number(b,20,2,1);Number(b,22,2,1);PutUuid(b,24,h.database_uuid);PutUuid(b,40,h.filespace_uuid);PutUuid(b,56,h.page_uuid);Number(b,72,8,h.page_number);Number(b,80,8,h.page_generation);Number(b,88,8,h.flags);PutUuid(b,104,h.page_size_profile_uuid);Number(b,120,2,1);
  u64 fnv=14695981039346656037ull;for(unsigned i=0;i<128;++i){fnv^=b[i];fnv*=1099511628211ull;}Number(b,96,8,fnv);
  std::copy_n(h.page_type==0x303?"SBPIN001":"SBPINL01",8,b.begin()+128);Number(b,136,2,1);Number(b,138,2,256);Number(b,140,4,384+160*v.records.size());PutUuid(b,144,v.object_uuid);Number(b,160,8,v.epoch);PutUuid(b,168,v.creator_transaction_uuid);Number(b,184,8,v.creator_local_transaction_id);Number(b,192,8,v.flags);Number(b,200,8,v.total_pins);Number(b,208,8,v.first_record);Number(b,216,4,v.records.size());if(v.next){const auto& r=*v.next;PutUuid(b,224,r.filespace_uuid);Number(b,240,8,r.page_number);Number(b,248,8,r.page_generation);PutUuid(b,256,r.page_size_profile_uuid);}std::copy(v.next_sha256.begin(),v.next_sha256.end(),b.begin()+272);Number(b,304,8,v.legal_hold_pins);Number(b,312,8,v.lowest_start);Number(b,320,8,v.highest_end);
  for(std::size_t i=0;i<v.records.size();++i){const auto at=384+i*160;const auto& r=v.records[i];PutUuid(b,at,r.pin_uuid);PutUuid(b,at+16,r.owner_uuid);Number(b,at+32,2,static_cast<disk::u16>(r.kind));Number(b,at+34,2,static_cast<disk::u16>(r.access));Number(b,at+36,4,r.flags);Number(b,at+40,8,r.start_local);Number(b,at+48,8,r.end_local);PutUuid(b,at+56,r.timeline_uuid);PutUuid(b,at+72,r.filespace_uuid);Number(b,at+88,8,r.retain_until_local);Number(b,at+96,8,r.retain_until_unix_ns);Number(b,at+104,8,r.blocked_operations);const auto digest=WholeRootHash(Bytes(b.begin()+at,b.begin()+at+160));std::copy(digest.begin(),digest.end(),b.begin()+at+112);}
  const auto digest=WholeRootHash(b);std::copy(digest.begin(),digest.end(),b.begin()+328);return b;
}
void CanonicalCheckpointHorizons(){using E=db::NativeCheckpointError;
  for(unsigned p=0;p<5;++p){const unsigned q=(p+1)%5;Fixture fixture;disk::FileDevice first,second;const auto path1=(fixture.root/"horizon-primary").string(),path2=(fixture.root/"horizon-shadow").string();
    auto z1=Example(p),z2=Example(q,2);z2.bootstrap.filespace_uuid=Id(7);z2.page_uuid=Id(8);for(auto& r:z2.roots)r.filespace_uuid=Id(7);
    Check(first.Open(path1,disk::FileOpenMode::create_new).ok()&&second.Open(path2,disk::FileOpenMode::create_new).ok(),"own actual checkpoint horizon devices");const byte pad=0;
    Check(first.WriteAt(z1.total_pages*sizes[p]-1,&pad,1).ok()&&second.WriteAt(z2.total_pages*sizes[q]-1,&pad,1).ok(),"actual checkpoint horizon capacities");
    auto inv=InventoryExample(p);inv.inventory.next_local_transaction_id=18;inv.inventory.next_commit_sequence=4;
    auto& original=inv.inventory.entries.front();original.identity.local_id=mga::MakeLocalTransactionId(11);original.identity.transaction_uuid.value=Id(91);original.state=mga::TransactionState::committed;original.commit_sequence=1;
    auto active=original;active.identity.local_id=mga::MakeLocalTransactionId(16);active.identity.transaction_uuid.value=Id(99);active.state=mga::TransactionState::active;active.commit_sequence=0;
    auto writer=original;writer.identity.local_id=mga::MakeLocalTransactionId(17);writer.identity.transaction_uuid.value=Id(98);writer.commit_sequence=3;auto pin_writer=original;pin_writer.identity.local_id=mga::MakeLocalTransactionId(12);pin_writer.identity.transaction_uuid.value=Id(184);pin_writer.commit_sequence=2;inv.inventory.entries.push_back(pin_writer);inv.inventory.entries.push_back(active);inv.inventory.entries.push_back(writer);
    const auto checkpoint=CheckpointExample(p);page::NativeHorizonRoot head;head.header={sizes[p],0x302,Id(1),Id(2),Id(80),24,124,0,Profile(p)};head.object_uuid=Id(112);head.epoch=3;head.creator_transaction_uuid=Id(91);head.creator_local_transaction_id=11;head.total_records=2;
    head.retention=checkpoint.roots[9].page;head.retention_object_uuid=checkpoint.roots[9].object_uuid;head.retention_sha256=checkpoint.roots[9].sha256;head.next=disk::NativePageReference{Id(7),25,125,Profile(q)};
    page::NativeHorizonRecord record;record.local_boundary=16;record.owner_uuid=Id(92);record.checkpoint_object_uuid=Id(49);record.checkpoint_generation=1;record.checkpoint=disk::NativePageReference{Id(2),19,109,Profile(p)};record.horizon_uuid=Id(180);record.timeline_uuid=Id(97);head.records.push_back(record);
    auto tail=head;tail.header={sizes[q],0x302,Id(1),Id(7),Id(81),25,125,0,Profile(q)};tail.first_record=1;tail.next.reset();tail.records[0].kind=page::NativeHorizonKind::oat;tail.records[0].local_boundary=18;tail.records[0].horizon_uuid=Id(181);
    page::NativeRetentionPage pin_root;pin_root.header={sizes[p],0x303,Id(1),Id(2),Id(182),40,90,0,Profile(p)};pin_root.object_uuid=checkpoint.roots[9].object_uuid;pin_root.epoch=4;pin_root.creator_transaction_uuid=Id(184);pin_root.creator_local_transaction_id=12;pin_root.total_pins=1;pin_root.lowest_start=4;pin_root.next=disk::NativePageReference{Id(7),42,142,Profile(q)};
    auto pin_leaf=pin_root;pin_leaf.header={sizes[q],0x304,Id(1),Id(7),Id(183),42,142,0,Profile(q)};pin_leaf.lowest_start=0;pin_leaf.next.reset();
    page::NativeRetentionPin pin;pin.pin_uuid=Id(90);pin.owner_uuid=Id(94);pin.timeline_uuid=Id(97);pin.start_local=4;pin.flags=1;pin.blocked_operations=3;pin.retain_until_unix_ns=1;pin_leaf.records.push_back(pin);
    pin_root.next_sha256=WholeRootHash(RetentionOracle(pin_leaf));head.retention_sha256=tail.retention_sha256=WholeRootHash(RetentionOracle(pin_root));head.records[0].pin_uuid=tail.records[0].pin_uuid=pin.pin_uuid;
    const auto put=[&](auto& file,u64 number,unsigned size,const Bytes& bytes){const auto io=file.WriteAt(number*size,bytes.data(),bytes.size());Check(io.ok()&&io.bytes_transferred==bytes.size()&&file.Sync().ok(),"persist independently authored checkpoint horizon image");};
    const auto persist=[&](const auto& inventory,const auto& primary,auto a,auto b,bool older=false,bool stale=false){auto cp=checkpoint;auto pr=pin_root;const auto leaf=RetentionOracle(pin_leaf);if(pr.next)pr.next_sha256=WholeRootHash(leaf);else pr.next_sha256.fill(0);const auto root=RetentionOracle(pr);cp.roots[9].sha256=WholeRootHash(root);if(a.retention_sha256==head.retention_sha256)a.retention_sha256=cp.roots[9].sha256;if(b.retention_sha256==head.retention_sha256)b.retention_sha256=cp.roots[9].sha256;put(first,40,sizes[p],root);put(second,42,sizes[q],leaf);const auto ib=InventoryStateOracle(inventory),tb=HorizonOracle(b);a.next_sha256=WholeRootHash(tb);const auto hb=HorizonOracle(a);
      cp.roots[0].page=InventoryRef(inventory);cp.roots[0].object_uuid=inventory.object_uuid;cp.roots[0].sha256=WholeRootHash(ib);cp.roots[1].page={Id(2),24,124,Profile(p)};cp.roots[1].object_uuid=Id(112);cp.roots[1].sha256=WholeRootHash(hb);if(stale)cp.roots[1].sha256[0]^=1;
      if(older){auto old=cp;old.header.page_number=20;old.header.page_generation=110;old.header.page_uuid=Id(95);old.root_set_generation=7;const auto bytes=CheckpointOracle(old);put(first,20,sizes[p],bytes);
        auto unrelated=old;unrelated.header.page_number=21;unrelated.header.page_generation=111;unrelated.header.page_uuid=Id(94);put(first,21,sizes[p],CheckpointOracle(unrelated));cp.checkpoint_generation=2;cp.predecessor=disk::NativePageReference{Id(2),20,110,Profile(p)};cp.predecessor_sha256=WholeRootHash(bytes);}
      put(first,0,sizes[p],Oracle(primary));put(second,0,sizes[q],Oracle(z2));put(first,14,sizes[p],ib);put(first,24,sizes[p],hb);put(second,25,sizes[q],tb);put(first,19,sizes[p],CheckpointOracle(cp));};
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};const u64 limit=4*sizes[p]+2*sizes[q],history_limit=6*sizes[p]+2*sizes[q];
    const auto read=[&](u64 budget){return db::VerifyCurrentNativeCheckpointHorizonFromOpenDevices(Id(1),devices,CheckpointRef(checkpoint),budget);};
    const auto empty=[&](const auto& r){Check(!r.ok()&&!r.retained_image_bytes&&r.checkpoints.checkpoints.empty()&&!r.checkpoints.retained_image_bytes&&r.horizons.pages.empty()&&!r.horizons.retained_image_bytes&&r.retention.images.empty()&&!r.retention.retained_image_bytes,"checkpoint horizon failure returns no prefix");};
    const auto hash_failure=[&](const auto& r){return r.error==E::hash_failure||(r.error==E::inventory_failure&&r.checkpoints.inventory_error==page::NativeInventoryError::hash_failure)||(r.error==E::horizon_failure&&r.horizon_error==page::NativeHorizonError::hash_failure)||(r.error==E::retention_failure&&r.retention_error==page::NativeRetentionError::hash_failure);};
    persist(inv,z1,head,tail);reads=observed_full_digests=0;observed_allocations=0;track_reads=count_allocations=count_full_digests=true;auto result=read(limit);track_reads=count_allocations=count_full_digests=false;const auto nr=reads,nf=observed_full_digests;const auto na=observed_allocations;
    auto expected=head;expected.next_sha256=WholeRootHash(HorizonOracle(tail));Check(result.ok()&&result.retained_image_bytes==limit&&result.horizons.pages.size()==2&&result.checkpoints.checkpoints.size()==1&&result.horizons.pages[0].bytes==HorizonOracle(expected)&&result.horizons.pages[1].bytes==HorizonOracle(tail)&&result.retention.images.size()==2&&result.retention.images[0].bytes==RetentionOracle(pin_root)&&result.retention.images[1].bytes==RetentionOracle(pin_leaf)&&!result.checkpoints.checkpoints[0].inventory.publication_base,"current committed actual horizon binding without CAS or cleanup grant");
    result=read(limit-1);empty(result);for(unsigned n=1;n<=nr;++n){reads=0;read_fault=n;track_reads=true;result=read(limit);track_reads=false;read_fault=0;empty(result);}
    for(unsigned n=1;n<=nf;++n){full_digest_fault=n;result=read(limit);Check(!full_digest_fault&&hash_failure(result),"current horizon full hash failure consumed and propagated");empty(result);}full_digest_fault=nf+1;result=read(limit);Check(full_digest_fault==1&&result.ok(),"horizon full hash terminal success");full_digest_fault=0;
    for(unsigned n=1;n<=5;++n){hash_fault=n;result=read(limit);Check(!hash_fault,"horizon multipart hash failure consumed");empty(result);}
    if(p==0){for(unsigned long n=0;n<=na;++n){allocation_budget=n;result=read(limit);allocation_budget=-1;if(result.ok())Check(result.retained_image_bytes==limit&&result.horizons.pages.size()==2&&result.checkpoints.checkpoints.size()==1,"complete current horizon allocation recovery");else empty(result);if(n==na)Check(result.ok(),"current horizon allocation terminal success");}std::cout<<"checkpoint horizon allocation sites="<<na<<" full digests="<<nf<<std::endl;}
    auto zero=z1;zero.root_set_generation++;persist(inv,zero,head,tail);result=read(limit);empty(result);Check(result.error==E::binding_mismatch,"horizon checkpoint must be current");
    persist(inv,z1,head,tail,false,true);result=read(limit);empty(result);Check(result.error==E::invalid_integrity,"complete horizon head digest matches actual checkpoint");
    for(unsigned n=0;n<7;++n){auto a=head,b=tail;if(n==0)a.creator_transaction_uuid=b.creator_transaction_uuid=Id(200);if(n==1){a.creator_transaction_uuid=b.creator_transaction_uuid=Id(99);a.creator_local_transaction_id=b.creator_local_transaction_id=16;}
      if(n==2)a.retention_object_uuid=b.retention_object_uuid=Id(200);if(n==3){a.retention.page_number=b.retention.page_number=41;}if(n==4){a.retention_sha256[0]^=1;b.retention_sha256[0]^=1;}
      if(n==5)a.records[0].local_boundary=19;if(n==6)a.flags=b.flags=1;persist(inv,z1,a,b);result=read(limit);empty(result);Check(result.error==(n==0?E::horizon_creator_mismatch:n==1?E::horizon_creator_not_committed:n<=4?E::horizon_retention_mismatch:n==5?E::horizon_boundary_mismatch:E::binding_mismatch),"specific horizon authority failure cause");}
    for(const auto origin:{mga::TransactionState::committed,mga::TransactionState::rolled_back,mga::TransactionState::failed_terminal}){auto inventory=inv;auto& e=inventory.inventory.entries[0];e.state=mga::TransactionState::archived;e.archived_from_state=origin;if(origin!=mga::TransactionState::committed)e.commit_sequence=0;persist(inventory,z1,head,tail);result=read(limit);if(origin==mga::TransactionState::committed)Check(result.ok(),"archived committed horizon creator");else{empty(result);Check(result.error==E::horizon_creator_not_committed,"archived noncommit cannot certify horizon");}}
    auto inventory=inv;inventory.inventory.entries[0].identity.scope=mga::TransactionScope::cluster_global;persist(inventory,z1,head,tail);result=read(limit);empty(result);Check(result.error==E::horizon_creator_mismatch,"global creator cannot certify standalone horizon");
    auto a=head,b=tail;a.records[0].checkpoint_generation=2;persist(inv,z1,a,b);result=read(limit);empty(result);Check(result.error==E::horizon_failure||result.error==E::horizon_observation_mismatch,"future checkpoint observation rejected");
    a=head;a.records[0].checkpoint->page_number=21;a.records[0].checkpoint->page_generation=111;persist(inv,z1,a,tail);result=read(limit);empty(result);Check(result.error==E::horizon_observation_mismatch,"alternate current-generation checkpoint reference rejected");
    a=head;a.header.page_uuid=checkpoint.header.page_uuid;persist(inv,z1,a,tail);result=read(limit);empty(result);Check(result.error==E::binding_mismatch,"known checkpoint and horizon page UUIDs cannot alias");
    a=head;b=tail;for(auto* v:{&a,&b}){v->records[0].checkpoint.reset();v->records[0].checkpoint_object_uuid={};v->records[0].checkpoint_generation=0;}persist(inv,z1,a,b);Check(read(limit).ok(),"absent observations retain current pair only");
    for(const auto outcome:{mga::TransactionState::committed,mga::TransactionState::rolled_back,mga::TransactionState::failed_terminal}){auto retained_inv=inv;auto& e=retained_inv.inventory.entries[1];e.state=mga::TransactionState::archived;e.archived_from_state=outcome;if(outcome!=mga::TransactionState::committed)e.commit_sequence=0;
      persist(retained_inv,z1,head,tail);result=read(limit);if(outcome==mga::TransactionState::committed)Check(result.ok(),"archived committed retention creator");else{empty(result);Check(result.error==E::retention_creator_not_committed,"archived noncommit cannot certify retention");}}
    auto global_pin_inv=inv;global_pin_inv.inventory.entries[1].identity.scope=mga::TransactionScope::cluster_global;persist(global_pin_inv,z1,head,tail);result=read(limit);empty(result);Check(result.error==E::retention_creator_mismatch,"global retention creator cannot certify standalone publication");
    const auto saved_root=pin_root,saved_leaf=pin_leaf;
    for(unsigned n=0;n<7;++n){pin_root=saved_root;pin_leaf=saved_leaf;
      if(n==0)pin_root.creator_transaction_uuid=pin_leaf.creator_transaction_uuid=Id(200);
      if(n==1){pin_root.creator_transaction_uuid=pin_leaf.creator_transaction_uuid=Id(99);pin_root.creator_local_transaction_id=pin_leaf.creator_local_transaction_id=16;}
      if(n==2)pin_root.flags=pin_leaf.flags=1;
      if(n==3)pin_leaf.records[0].pin_uuid=Id(201);
      if(n==4)pin_leaf.records[0].timeline_uuid=Id(202);
      if(n==5)pin_root.header.page_uuid=head.header.page_uuid;
      if(n==6)pin_leaf.header.page_uuid=checkpoint.header.page_uuid;
      persist(inv,z1,head,tail);result=read(limit);empty(result);
      Check(result.error==(n==0?E::retention_creator_mismatch:n==1?E::retention_creator_not_committed:n==3?E::horizon_pin_missing:n==4?E::horizon_pin_lineage_mismatch:E::binding_mismatch),"specific actual retention/pin binding failure");
    }
    pin_root=saved_root;pin_leaf=saved_leaf;persist(inv,z1,head,tail);
    auto resealed=pin_root;resealed.epoch++;put(first,40,sizes[p],RetentionOracle(resealed));result=read(limit);empty(result);
    Check(result.error==E::retention_failure,"retention chain publication mismatch refuses before digest");
    pin_root.epoch++;pin_leaf.epoch++;persist(inv,z1,head,tail);
    // Restore only the selected root and its matching leaf; checkpoint/horizon still bind the newer publication.
    put(first,40,sizes[p],RetentionOracle(saved_root));put(second,42,sizes[q],RetentionOracle(saved_leaf));result=read(limit);empty(result);
    Check(result.error==E::invalid_integrity,"valid actual retention chain must match complete selected root digest");
    pin_root=saved_root;pin_leaf=saved_leaf;
    pin_root.creator_transaction_uuid=pin_leaf.creator_transaction_uuid=Id(98);pin_root.creator_local_transaction_id=pin_leaf.creator_local_transaction_id=17;
    persist(inv,z1,head,tail);Check(read(limit).ok(),"retention and horizon may have distinct committed creators");
    pin_root=saved_root;pin_leaf=saved_leaf;
    auto no_pin_head=head,no_pin_tail=tail;no_pin_head.records[0].pin_uuid=no_pin_tail.records[0].pin_uuid={};
    persist(inv,z1,no_pin_head,no_pin_tail);result=read(limit);Check(result.ok()&&result.retention.images[1].page->records[0].retain_until_unix_ns==1,"unused expired but unreleased pin remains active");
    pin_root.total_pins=pin_root.lowest_start=0;pin_root.next.reset();pin_root.next_sha256.fill(0);
    persist(inv,z1,no_pin_head,no_pin_tail);result=read(limit-sizes[q]);Check(result.ok()&&result.retention.images.size()==1&&result.retained_image_bytes==limit-sizes[q],"actual empty retention root with absent pin references");
    persist(inv,z1,head,tail);result=read(limit);empty(result);Check(result.error==E::horizon_pin_missing,"empty actual pin set cannot certify a referenced pin");
    pin_root=saved_root;pin_leaf=saved_leaf;
    auto lineage_head=head,lineage_tail=tail;lineage_head.records[0].timeline_uuid=lineage_tail.records[0].timeline_uuid=Id(203);pin_leaf.records[0].timeline_uuid=Id(203);
    persist(inv,z1,lineage_head,lineage_tail);result=read(limit);Check(result.ok()&&result.retention.images[1].page->records[0].owner_uuid==Id(94),"retained lineage need not be current checkpoint lineage or share horizon owner");
    pin_root=saved_root;pin_leaf=saved_leaf;
    a=head;b=tail;a.records[0].checkpoint_generation=2;b.records[0].checkpoint=disk::NativePageReference{Id(2),20,110,Profile(p)};persist(inv,z1,a,b,true);
    reads=observed_full_digests=0;observed_allocations=0;track_reads=count_allocations=count_full_digests=true;result=read(history_limit);track_reads=count_allocations=count_full_digests=false;const auto hn=reads,hf=observed_full_digests;const auto ha=observed_allocations;
    Check(result.ok()&&result.horizons.pages.size()==2&&result.checkpoints.checkpoints.size()==2&&result.retained_image_bytes==history_limit&&result.checkpoints.checkpoints.front().checkpoint->checkpoint_generation==2&&result.checkpoints.checkpoints.back().checkpoint->checkpoint_generation==1,"one actual history resolves mixed current and historical observations");
    result=read(history_limit-1);empty(result);for(unsigned n=1;n<=hn;++n){reads=0;read_fault=n;track_reads=true;result=read(history_limit);track_reads=false;read_fault=0;empty(result);}
    for(unsigned n=1;n<=hf;++n){full_digest_fault=n;result=read(history_limit);Check(!full_digest_fault&&hash_failure(result),"historical horizon full hash failure cause");empty(result);}full_digest_fault=hf+1;result=read(history_limit);Check(full_digest_fault==1&&result.ok(),"historical horizon full hash terminal success");full_digest_fault=0;
    if(p==0){for(unsigned long n=0;n<=ha;++n){allocation_budget=n;result=read(history_limit);allocation_budget=-1;if(result.ok())Check(result.horizons.pages.size()==2&&result.checkpoints.checkpoints.size()==2&&result.retained_image_bytes==history_limit,"complete horizon history allocation recovery");else empty(result);if(n==ha)Check(result.ok(),"horizon history allocation terminal success");}std::cout<<"checkpoint horizon history allocation sites="<<ha<<" full digests="<<hf<<std::endl;}
    b.records[0].checkpoint=disk::NativePageReference{Id(2),21,111,Profile(p)};persist(inv,z1,a,b,true);result=read(history_limit);empty(result);Check(result.error==E::history_mismatch,"independently valid unrelated older checkpoint is not ancestry");
    persist(inv,z1,head,tail);Check(first.Close().ok()&&second.Close().ok()&&first.Open(path1,disk::FileOpenMode::open_existing_read_only).ok()&&second.Open(path2,disk::FileOpenMode::open_existing_read_only).ok(),"actual read-only checkpoint horizon reopen");Check(read(limit).ok()&&first.read_only()&&second.read_only(),"current horizon binding after actual reopen");
  }
}

Bytes SystemStateOracle(const db::NativeSystemState& s) {
  auto common=RootExample();common.header=s.header;auto b=RootOracle(common);std::fill(b.begin()+128,b.end(),0);
  const auto ref=[&](std::size_t at,const disk::NativePageReference& r){PutUuid(b,at,r.filespace_uuid);Number(b,at+16,8,r.page_number);Number(b,at+24,8,r.page_generation);PutUuid(b,at+32,r.page_size_profile_uuid);};
  std::copy_n("SBSYS001",8,b.begin()+128);Number(b,136,2,1);Number(b,138,2,384);Number(b,140,4,512);PutUuid(b,144,s.object_uuid);
  Number(b,160,8,s.state_generation);Number(b,168,8,s.restart_generation);Number(b,176,8,s.startup_counter);PutUuid(b,184,s.creator_transaction_uuid);Number(b,200,8,s.creator_local_transaction_id);
  Number(b,208,2,static_cast<disk::u16>(s.lifecycle));Number(b,210,2,static_cast<disk::u16>(s.recovery));Number(b,212,4,s.flags);Number(b,216,8,s.checkpoint_generation);
  if(s.checkpoint)ref(224,*s.checkpoint);PutUuid(b,272,s.checkpoint_object_uuid);PutUuid(b,288,s.clean_transaction_uuid);Number(b,304,8,s.clean_local_transaction_id);PutUuid(b,312,s.transition_operation_uuid);
  if(s.predecessor)ref(328,*s.predecessor);std::copy(s.predecessor_sha256.begin(),s.predecessor_sha256.end(),b.begin()+376);
  const auto digest=WholeRootHash(b);std::copy(digest.begin(),digest.end(),b.begin()+408);return b;
}
void CanonicalCheckpointSystemState() {
  using E=db::NativeCheckpointError;
  for(unsigned p=0;p<5;++p){const unsigned q=(p+1)%5;Fixture fixture;disk::FileDevice first,second;
    const auto path1=(fixture.root/"checkpoint-system-primary").string(),path2=(fixture.root/"checkpoint-system-shadow").string();
    auto z1=Example(p),z2=Example(q,2);z2.bootstrap.filespace_uuid=Id(7);z2.page_uuid=Id(8);for(auto& r:z2.roots)r.filespace_uuid=Id(7);
    z1.roots[0].filespace_uuid=Id(7);z1.roots[0].page_size_profile_uuid=Profile(q);
    Check(first.Open(path1,disk::FileOpenMode::create_new).ok()&&second.Open(path2,disk::FileOpenMode::create_new).ok(),"own actual checkpoint system-state devices");
    const byte pad=0;Check(first.WriteAt(z1.total_pages*sizes[p]-1,&pad,1).ok()&&second.WriteAt(z2.total_pages*sizes[q]-1,&pad,1).ok(),"actual system-state filespace capacities");
    auto inv=InventoryExample(p);inv.inventory.next_local_transaction_id=18;inv.inventory.next_commit_sequence=4;
    auto& original=inv.inventory.entries.front();original.identity.local_id=mga::MakeLocalTransactionId(11);original.identity.transaction_uuid.value=Id(91);original.state=mga::TransactionState::committed;original.commit_sequence=1;
    auto clean=original;clean.identity.local_id=mga::MakeLocalTransactionId(12);clean.identity.transaction_uuid.value=Id(92);clean.commit_sequence=2;
    auto active=original;active.identity.local_id=mga::MakeLocalTransactionId(16);active.identity.transaction_uuid.value=Id(99);active.state=mga::TransactionState::active;active.commit_sequence=0;
    auto writer=original;writer.identity.local_id=mga::MakeLocalTransactionId(17);writer.identity.transaction_uuid.value=Id(98);writer.commit_sequence=3;
    inv.inventory.entries.push_back(clean);inv.inventory.entries.push_back(active);inv.inventory.entries.push_back(writer);
    const auto checkpoint=CheckpointExample(p);
    db::NativeSystemState state;state.header={sizes[q],8,Id(1),Id(7),Id(80),11,101,0,Profile(q)};state.object_uuid=Id(41);state.state_generation=1;state.restart_generation=1;state.startup_counter=2;
    state.creator_transaction_uuid=Id(91);state.creator_local_transaction_id=11;state.lifecycle=db::NativeSystemLifecycle::opening;
    state.checkpoint_generation=1;state.checkpoint=disk::NativePageReference{Id(2),19,109,Profile(p)};state.checkpoint_object_uuid=Id(49);
    state.clean_transaction_uuid=Id(92);state.clean_local_transaction_id=12;state.transition_operation_uuid=Id(93);
    const auto put=[&](auto& device,u64 number,unsigned size,const Bytes& bytes){const auto io=device.WriteAt(number*size,bytes.data(),bytes.size());Check(io.ok()&&io.bytes_transferred==bytes.size()&&device.Sync().ok(),"persist independent current system-state fixture");};
    const auto persist=[&](const auto& inventory,const auto& primary,const auto& s,bool older=false,bool stale=false,bool clean_checkpoint=false){auto cp=checkpoint;
      const auto ib=InventoryStateOracle(inventory),sb=SystemStateOracle(s);
      cp.roots[0].page=InventoryRef(inventory);cp.roots[0].object_uuid=inventory.object_uuid;cp.roots[0].sha256=WholeRootHash(ib);
      cp.roots[7].page={Id(7),11,101,Profile(q)};cp.roots[7].object_uuid=Id(41);cp.roots[7].sha256=WholeRootHash(stale?SystemStateOracle(state):sb);
      if(clean_checkpoint)cp.flags|=1;
      if(older){auto old=cp;old.header.page_number=20;old.header.page_generation=110;old.header.page_uuid=Id(95);old.root_set_generation=7;
        const auto bytes=CheckpointOracle(old);put(first,20,sizes[p],bytes);
        auto unrelated=old;unrelated.header.page_number=21;unrelated.header.page_generation=111;unrelated.header.page_uuid=Id(94);put(first,21,sizes[p],CheckpointOracle(unrelated));
        cp.checkpoint_generation=2;cp.predecessor=disk::NativePageReference{Id(2),20,110,Profile(p)};cp.predecessor_sha256=WholeRootHash(bytes);}
      put(first,0,sizes[p],Oracle(primary));put(second,0,sizes[q],Oracle(z2));put(first,14,sizes[p],ib);put(second,11,sizes[q],sb);put(first,19,sizes[p],CheckpointOracle(cp));
    };
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};
    const u64 limit=2*sizes[p]+sizes[q],history_limit=4*sizes[p]+sizes[q];
    const auto read=[&](u64 budget){return db::VerifyCurrentNativeCheckpointSystemStateFromOpenDevices(Id(1),devices,CheckpointRef(checkpoint),budget);};
    const auto empty=[&](const auto& r){Check(!r.ok()&&!r.retained_image_bytes&&r.checkpoints.checkpoints.empty()&&!r.checkpoints.retained_image_bytes&&!r.system_state.state&&r.system_state.bytes.empty(),"checkpoint system-state failure returns no authority prefix");};
    persist(inv,z1,state);reads=observed_full_digests=0;observed_allocations=0;track_reads=count_allocations=count_full_digests=true;auto result=read(limit);track_reads=count_allocations=count_full_digests=false;
    const auto nr=reads,nf=observed_full_digests;const auto na=observed_allocations;
    Check(result.ok()&&result.retained_image_bytes==limit&&result.checkpoints.checkpoints.size()==1&&result.system_state.bytes==SystemStateOracle(state)&&
      !result.checkpoints.checkpoints[0].inventory.publication_base,"actual current observed checkpoint and committed system-state fields");
    result=read(limit-1);empty(result);Check(result.error==E::resource_exhausted,"system-state shared image budget");
    for(unsigned n=1;n<=nr;++n){reads=0;read_fault=n;track_reads=true;result=read(limit);track_reads=false;read_fault=0;empty(result);}
    const auto hash_failure=[&](const auto& r){return r.error==E::hash_failure||
      (r.error==E::inventory_failure&&r.checkpoints.inventory_error==page::NativeInventoryError::hash_failure)||
      (r.error==E::system_state_failure&&r.system_error==db::NativeSystemStateError::hash_failure);};
    for(unsigned n=1;n<=nf;++n){full_digest_fault=n;result=read(limit);Check(!full_digest_fault&&hash_failure(result),"current system-state complete hash provider failure");empty(result);}
    full_digest_fault=nf+1;result=read(limit);Check(full_digest_fault==1&&result.ok(),"current system-state full digest sweep terminal success");full_digest_fault=0;
    for(unsigned n=1;n<=5;++n){hash_fault=n;result=read(limit);Check(!hash_fault,"system-state part hash failure consumed");empty(result);}
    if(p==0){for(unsigned long n=0;n<=na;++n){allocation_budget=n;result=read(limit);allocation_budget=-1;
        if(result.ok())Check(result.retained_image_bytes==limit&&result.checkpoints.checkpoints.size()==1&&result.system_state.bytes==SystemStateOracle(state),"system-state allocation recovery exact current binding");else empty(result);if(n==na)Check(result.ok(),"current system-state allocation sweep completed");}
      std::cout<<"checkpoint system-state allocation sites="<<na<<std::endl;}
    for(unsigned n=0;n<3;++n){auto zero=z1;if(n==0)zero.root_set_generation++;if(n==1)zero.roots[8].object_uuid=Id(200);if(n==2)zero.roots[0].object_uuid=Id(200);
      persist(inv,zero,state);result=read(limit);empty(result);Check(result.error==E::binding_mismatch,"current checkpoint and system root selection exact");}
    for(unsigned n=0;n<4;++n){auto s=state;if(n==0)s.creator_transaction_uuid=Id(200);if(n==1)s.clean_transaction_uuid=Id(200);
      if(n==2){s.creator_transaction_uuid=Id(99);s.creator_local_transaction_id=16;}if(n==3){s.clean_transaction_uuid=Id(99);s.clean_local_transaction_id=16;}
      persist(inv,z1,s);result=read(limit);empty(result);const E errors[]={E::system_state_creator_mismatch,E::system_state_clean_mismatch,E::system_state_creator_not_committed,E::system_state_clean_not_committed};Check(result.error==errors[n],"distinct current and historical clean creator causes");}
    for(unsigned index:{0u,1u})for(auto origin:{mga::TransactionState::committed,mga::TransactionState::rolled_back,mga::TransactionState::failed_terminal}){
      auto changed=inv;auto& e=changed.inventory.entries[index];e.state=mga::TransactionState::archived;e.archived_from_state=origin;if(origin!=mga::TransactionState::committed)e.commit_sequence=0;
      persist(changed,z1,state);result=read(limit);if(origin==mga::TransactionState::committed)Check(result.ok(),"archived committed system-state creator admitted");else{empty(result);Check(result.error==(index?E::system_state_clean_not_committed:E::system_state_creator_not_committed),"archived noncommit cannot certify system state");}}
    auto changed=inv;changed.inventory.entries[1].identity.scope=mga::TransactionScope::cluster_global;persist(changed,z1,state);result=read(limit);empty(result);Check(result.error==E::system_state_clean_mismatch,"global historical clean creator cannot certify standalone state");
    auto s=state;s.flags|=db::NativeSystemFlag::cluster;persist(inv,z1,s);result=read(limit);empty(result);Check(result.error==E::binding_mismatch,"cluster observation matches checkpoint and bootstrap");
    s=state;s.lifecycle=db::NativeSystemLifecycle::closed;s.flags=5;persist(inv,z1,s);result=read(limit);empty(result);Check(result.error==E::binding_mismatch,"clean state cannot borrow nonclean checkpoint marker");
    persist(inv,z1,s,false,false,true);Check(read(limit).ok(),"matching clean observations retain committed identities but do not certify shutdown");
    s=state;s.startup_counter++;persist(inv,z1,s,false,true);result=read(limit);empty(result);Check(result.error==E::invalid_integrity,"resealed system state still binds complete checkpoint digest");
    s=state;s.checkpoint_generation++;persist(inv,z1,s);result=read(limit);empty(result);Check(result.error==E::system_state_observation_mismatch,"selected checkpoint observation generation exact");
    s=state;s.checkpoint_generation=0;s.checkpoint.reset();s.checkpoint_object_uuid={};persist(inv,z1,s);Check(read(limit).ok(),"pre-checkpoint nonclean observation retains current pair");
    s=state;s.checkpoint=disk::NativePageReference{Id(2),20,110,Profile(p)};persist(inv,z1,s,true);
    reads=observed_full_digests=0;observed_allocations=0;track_reads=count_allocations=count_full_digests=true;result=read(history_limit);track_reads=count_allocations=count_full_digests=false;
    const auto history_reads=reads,history_digests=observed_full_digests;const auto history_allocations=observed_allocations;
    Check(result.ok()&&result.retained_image_bytes==history_limit&&result.checkpoints.checkpoints.size()==2&&
      result.checkpoints.checkpoints.front().checkpoint->checkpoint_generation==2&&result.checkpoints.checkpoints.back().checkpoint->checkpoint_generation==1&&result.system_state.bytes==SystemStateOracle(s),"actual current-to-observed checkpoint ancestry");
    result=read(history_limit-1);empty(result);
    for(unsigned n=1;n<=history_reads;++n){reads=0;read_fault=n;track_reads=true;result=read(history_limit);track_reads=false;read_fault=0;empty(result);}
    for(unsigned n=1;n<=history_digests;++n){full_digest_fault=n;result=read(history_limit);empty(result);
      Check(!full_digest_fault&&hash_failure(result),
        "observed checkpoint history hash failure cause="+std::to_string(static_cast<unsigned>(result.error))+" inventory="+std::to_string(static_cast<unsigned>(result.checkpoints.inventory_error))+" system="+std::to_string(static_cast<unsigned>(result.system_error))+" site="+std::to_string(n));}
    full_digest_fault=history_digests+1;result=read(history_limit);Check(full_digest_fault==1&&result.ok(),"observed checkpoint digest sweep terminal success");full_digest_fault=0;
    if(p==0)std::cout<<"checkpoint system full digest sites="<<nf<<" observed-history="<<history_digests<<std::endl;
    if(p==0){for(unsigned long n=0;n<=history_allocations;++n){allocation_budget=n;result=read(history_limit);allocation_budget=-1;
        if(result.ok())Check(result.retained_image_bytes==history_limit&&result.checkpoints.checkpoints.size()==2&&result.system_state.bytes==SystemStateOracle(s),"history allocation recovery retains exact complete binding");else empty(result);
        if(n==history_allocations)Check(result.ok(),"observed checkpoint allocation sweep completed");}std::cout<<"checkpoint system observed-history allocation sites="<<history_allocations<<std::endl;}
    s.checkpoint=disk::NativePageReference{Id(2),21,111,Profile(p)};persist(inv,z1,s,true);result=read(history_limit);empty(result);Check(result.error==E::history_mismatch,"unrelated valid older checkpoint is not observed ancestry");
    persist(inv,z1,state);Check(first.Close().ok()&&second.Close().ok()&&first.Open(path1,disk::FileOpenMode::open_existing_read_only).ok()&&second.Open(path2,disk::FileOpenMode::open_existing_read_only).ok(),"read-only system checkpoint reopen");
    Check(read(limit).ok()&&first.read_only()&&second.read_only(),"reopened current system-state binding");
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
void CanonicalCatalogVersionStaging(bool metric_policy=false){using E=db::NativeCatalogVersionStageError;using S=page::NativeAllocationState;
  for(unsigned p=0;p<5;++p)for(unsigned role=1;role<=5;++role){const unsigned q=(p+1)%5;const bool primary=role<=4;
    Fixture fixture;disk::FileDevice first,second;auto z1=Example(p,primary?role:1),z2=Example(q,5);z2.bootstrap.filespace_uuid=Id(7);z2.page_uuid=Id(8);for(auto& root:z2.roots)root.filespace_uuid=Id(7);
    z1.free_pages=z2.free_pages=z1.preallocated_pages=z2.preallocated_pages=0;
    const auto path1=(fixture.root/"stage-primary").string(),path2=(fixture.root/"stage-secondary").string();
    Check(first.Open(path1,disk::FileOpenMode::create_new).ok()&&second.Open(path2,disk::FileOpenMode::create_new).ok(),"own catalog staging filespaces");
    const byte pad=0;Check(first.WriteAt(64*sizes[p]-1,&pad,1).ok()&&second.WriteAt(64*sizes[q]-1,&pad,1).ok(),"actual catalog staging capacity");
    auto& target=primary?first:second;const auto profile=primary?p:q;const auto fs=primary?Id(2):Id(7);const auto& zero=primary?z1:z2;
    auto inv=InventoryExample(p);inv.inventory.next_local_transaction_id=18;inv.inventory.next_commit_sequence=2;
    auto& active=inv.inventory.entries[0];active.identity.local_id=mga::MakeLocalTransactionId(13);active.identity.transaction_uuid.value=Id(162);active.state=mga::TransactionState::active;active.commit_sequence=0;
    auto committed=active;committed.identity.local_id=mga::MakeLocalTransactionId(17);committed.identity.transaction_uuid.value=Id(98);committed.state=mga::TransactionState::committed;committed.commit_sequence=1;inv.inventory.entries.push_back(committed);
    const auto owner=inv.inventory.entries.front().identity;auto cp=CheckpointExample(p);auto leaf=LeafExample(profile);leaf.header.filespace_uuid=fs;
    auto source_leaf=LeafExample(p);source_leaf.header.page_number=source_leaf.body.page_number=30;source_leaf.header.page_uuid=Id(151);
    const auto bind_policy=[](auto& metadata,const catalog::CatalogMetricRetentionPolicy* prior=nullptr){
      catalog::CatalogMetricRetentionPolicy record;
      if(prior)record=*prior;
      else {record.origin_transaction_uuid=metadata.creator_transaction_uuid;record.origin_local_transaction_id=metadata.creator_local_transaction_id;record.policy.policy_name="actual-native-policy";}
      record.policy.policy_uuid=metadata.record.header.object_uuid.value;record.policy.generation=metadata.definition_version;
      metadata.record.header.kind=catalog::CatalogRecordKind::policy;metadata.record.header.parent_uuid.kind=platform::UuidKind::object;
      metadata.default_name_uuid={platform::UuidKind::object,Id(231)};metadata.name_vector_uuid={platform::UuidKind::object,Id(232)};metadata.object_subtype="metric_retention";
      const auto payload=catalog::EncodeCatalogMetricRetentionPolicy(record);Check(payload.ok(),"encode actual metric policy definition");
      metadata.record.payload.assign(payload.bytes.begin(),payload.bytes.end());
    };
    if(metric_policy){auto metadata=catalog::DecodeCatalogMetadataVersion(source_leaf.body.rows[0].cells[0].value.payload);Check(metadata.ok(),"source metadata for native policy");
      bind_policy(metadata.record);const auto encoded=catalog::EncodeCatalogMetadataVersion(metadata.record);Check(encoded.ok(),"native metric family source binding");source_leaf.body.rows[0].cells[0].value.payload=encoded.bytes;}
    auto catalog_root=RootExample(p);catalog_root.creator_transaction_uuid=Id(98);
    catalog_root.roots[0].page.page_number=30;
    leaf.body.rows.clear();
    page::NativeAllocationMap map;map.header={sizes[profile],3,Id(1),fs,Id(70),13,103,0,Profile(profile)};map.object_uuid=Id(43);map.map_generation=5;map.capacity_generation=6;map.total_pages=64;map.creator_transaction_uuid=Id(98);map.creator_local_transaction_id=17;map.states.assign(64,S::quarantined);
    for(unsigned n:{0u,13u,21u}){page::NativeAllocationRecord r;r.page_number=n;r.allocation_uuid=Id(120+n);r.creator_transaction_uuid=Id(98);r.creator_local_transaction_id=17;map.states[n]=S::allocated;
      if(n==0){r.page_uuid=zero.page_uuid;r.page_generation=zero.page_generation;r.page_type=primary?1:2;r.owner_uuid=fs;}
      if(n==13){r.page_uuid=map.header.page_uuid;r.page_generation=103;r.page_type=3;r.owner_uuid=map.object_uuid;}
      if(n==21){r.page_uuid=leaf.header.page_uuid;r.page_generation=7;r.page_type=6;r.owner_uuid=leaf.body.relation_uuid.value;r.creator_transaction_uuid=Id(162);r.creator_local_transaction_id=13;map.states[n]=S::reserved;}
      map.records.push_back(r);}
    page::NativeFilespaceDirectory directory;directory.header={sizes[p],9,Id(1),Id(2),Id(80),15,105,0,Profile(p)};directory.object_uuid=Id(45);directory.directory_generation=5;directory.creator_transaction_uuid=Id(98);directory.creator_local_transaction_id=17;directory.total_records=2;
    for(const auto* z:{&z1,&z2})directory.records.push_back({z->bootstrap,Id(z==&z1?190:191),z->page_uuid,z->page_generation,z->root_set_generation,z->total_pages,0,{}});
    const auto put=[&](auto& file,u64 number,unsigned size,const Bytes& bytes){const auto io=file.WriteAt(number*size,bytes.data(),bytes.size());Check(io.ok()&&io.bytes_transferred==bytes.size()&&file.Sync().ok(),"persist catalog staging fixture bytes");};
    const auto persist=[&](){const auto ib=InventoryOracle(inv,13,13,13),ab=AllocationOracle(map),dbb=DirectoryOracle(directory),cb=RootOracle(catalog_root);
      cp.selected_local_transaction_id=inv.inventory.next_local_transaction_id-1;
      cp.roots[4].page={Id(2),12,102,Profile(p)};cp.roots[4].object_uuid=Id(42);cp.roots[4].sha256=WholeRootHash(cb);
      cp.roots[8]=cp.roots[4];cp.roots[8].role=9;
      put(first,12,sizes[p],cb);put(first,30,sizes[p],LeafOracle(source_leaf));
      cp.roots[0].page=InventoryRef(inv);cp.roots[0].object_uuid=inv.object_uuid;cp.roots[0].sha256=WholeRootHash(ib);
      cp.roots[2].page={Id(2),15,105,Profile(p)};cp.roots[2].object_uuid=Id(45);cp.roots[2].sha256=WholeRootHash(dbb);
      if(primary){cp.roots[3].page={fs,13,103,Profile(profile)};cp.roots[3].object_uuid=Id(43);cp.roots[3].sha256=WholeRootHash(ab);}
      put(first,0,sizes[p],Oracle(z1));put(second,0,sizes[q],Oracle(z2));put(first,14,sizes[p],ib);put(first,15,sizes[p],dbb);put(target,13,sizes[profile],ab);put(first,19,sizes[p],CheckpointOracle(cp));};
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};

    const u64 budget=std::max(4*sizes[p],3*sizes[p]+3*sizes[profile]);const Bytes blank(sizes[profile],0);
    const auto reset=[&](){put(target,21,sizes[profile],blank);};
    const auto actual=[&](auto& file,u64 number,unsigned size){Bytes b(size);const auto io=file.ReadAt(number*size,b.data(),b.size());Check(io.ok()&&io.bytes_transferred==b.size(),"read actual version staging bytes");return b;};
    persist();CatalogTestPin snapshot(inv.inventory,13);
    auto decoded=catalog::DecodeCatalogMetadataVersion(source_leaf.body.rows[0].cells[0].value.payload);Check(decoded.ok(),"decode source metadata fixture");
    db::NativeCatalogVersionMutation request;request.relation_uuid=leaf.body.relation_uuid;request.page_number=21;request.transaction=owner;request.metadata=decoded.record;
    request.metadata.record.header.row_uuid.value=Id(210);request.metadata.record.header.object_uuid.value=Id(211);
    if(metric_policy)bind_policy(request.metadata);
    const auto create=request;const auto source_bytes=actual(first,30,sizes[p]);const auto root_bytes=actual(first,12,sizes[p]);const auto zero_bytes=actual(first,0,sizes[p]);
    const auto stage=[&](u64 limit){return db::StageNativeCatalogVersionFromOpenDevices(devices,CheckpointRef(cp),2,1,{Id(101),{}},snapshot.pin,request,leaf,limit);};
    const auto empty=[&](const auto& r){Check(!r.ok()&&!r.row&&!r.stage.receipt,"failed version staging returns no receipt");};
    const auto unchanged=[&](){Check(actual(first,30,sizes[p])==source_bytes&&actual(first,12,sizes[p])==root_bytes&&actual(first,0,sizes[p])==zero_bytes,"staging preserves predecessor and current-root selection");};
    const auto verify=[&](const auto& result,u64 sequence,const auto& predecessor,bool retired){
      if(!result.ok())std::cerr<<"version stage error="<<static_cast<int>(result.error)<<" source="<<static_cast<int>(result.source_error)<<" physical="<<static_cast<int>(result.stage.error)<<std::endl;
      Check(result.ok(),"actual pinned native catalog version staging");
      const auto bytes=actual(target,21,sizes[profile]);const auto stored=db::DecodeNativeCatalogLeaf(bytes);
      Check(stored.ok()&&stored.page->body.rows.size()==1,"read actual staged successor");
      const auto& row=stored.page->body.rows[0];const auto& metadata=stored.metadata.at(row.version_uuid);
      Check(row.version_uuid==result.row->version_uuid&&scratchbird::core::uuid::IsEngineIdentityUuid(row.version_uuid)&&row.version_uuid!=Id(170)&&row.version_uuid!=Id(171)&&
        row.row_uuid.value==request.metadata.record.header.row_uuid.value&&row.row_version==sequence&&row.previous_version_uuid==predecessor&&row.previous_row_version==(sequence==1?0:1)&&
        row.storage_generation==7&&row.stable_slot_id==1&&!row.deleted&&metadata.record.header.deleted==retired&&metadata.definition_version==request.metadata.definition_version&&
        row.transaction_uuid.value==Id(162)&&row.local_transaction_id==13&&metadata.record.header.object_uuid.value==request.metadata.record.header.object_uuid.value&&
        result.row->filespace_uuid.value==fs&&result.row->page_number==21&&result.row->storage_generation==7&&result.stage.receipt->sha256==WholeRootHash(bytes),"generated identity and actual successor residency/metadata");
      Check(LeafOracle(*stored.page)==bytes,"independent successor image packing");unchanged();
    };
    reset();auto result=stage(budget);verify(result,1,platform::Uuid{},false);
    reset();request.metadata=decoded.record;request.expected_version_uuid=Id(170);request.metadata.definition_version=2;
    if(metric_policy){const auto origin=catalog::DecodeCatalogMetricRetentionPolicy(decoded.record.record.payload);Check(origin.ok(),"load persisted policy origin");bind_policy(request.metadata,&*origin.record);}
    result=stage(budget);verify(result,2,Id(170),false);
    reset();request.metadata.record.header.deleted=true;request.metadata.lifecycle=catalog::CatalogObjectLifecycle::dropped;request.metadata.status=catalog::CatalogObjectStatus::retired;request.metadata.retired_transaction_uuid=owner.transaction_uuid;
    result=stage(budget);verify(result,2,Id(170),true);
    if(metric_policy){
      const auto original=catalog::DecodeCatalogMetricRetentionPolicy(decoded.record.record.payload);Check(original.ok(),"retained metric origin");
      request.metadata=decoded.record;request.metadata.definition_version=2;bind_policy(request.metadata,&*original.record);
      const auto replacement=request;
      for(unsigned bad=0;bad<4;++bad){reset();request=replacement;
        auto changed=catalog::DecodeCatalogMetricRetentionPolicy(request.metadata.record.payload);Check(changed.ok(),"replacement metric payload");
        if(bad==0)changed.record->origin_transaction_uuid.value=Id(240);
        if(bad==1)--changed.record->origin_local_transaction_id;
        if(bad==2)changed.record->policy.scope="node";
        if(bad==3){request.metadata.object_subtype="generic";request.metadata.record.payload="opaque-other-policy";}
        else {const auto payload=catalog::EncodeCatalogMetricRetentionPolicy(*changed.record);Check(payload.ok(),"changed origin individually well-formed");request.metadata.record.payload.assign(payload.bytes.begin(),payload.bytes.end());}
        Check(catalog::EncodeCatalogMetadataVersion(request.metadata).ok(),"origin attack has valid common envelope");
        const auto refused=stage(budget);empty(refused);Check(refused.error==E::stale_version&&actual(target,21,sizes[profile])==blank,"native checkpoint writer rejects changed policy origin/scope/family before writes");unchanged();
      }
      reset();request=replacement;request.metadata.authority_scope=catalog::CatalogAuthorityScope::cluster;
      empty(stage(budget));Check(actual(target,21,sizes[profile])==blank,"local native writer cannot create cluster policy authority");
      request=replacement;
      if(p==0&&role==1){
        for(unsigned mode=1;mode<=2;++mode){reset();stage_write_fault=mode;empty(stage(budget));Check(!stage_write_fault,"policy physical write failure consumed");unchanged();}
        reset();stage_sync_fault=1;empty(stage(budget));Check(!stage_sync_fault,"policy physical sync failure consumed");unchanged();
      }
      reset();result=stage(budget);verify(result,2,Id(170),false);
      const auto retained=db::DecodeNativeCatalogLeaf(actual(target,21,sizes[profile]));
      const auto final_policy=catalog::DecodeCatalogMetricRetentionPolicy(retained.metadata.begin()->second.record.payload);
      Check(final_policy.ok()&&final_policy.record->policy.generation==2&&final_policy.record->policy.policy_uuid==original.record->policy.policy_uuid&&
        final_policy.record->origin_transaction_uuid.value==original.record->origin_transaction_uuid.value&&final_policy.record->origin_local_transaction_id==13,"actual stored policy preserves binary identity and original creator");
      continue;
    }
    for(unsigned bad=0;bad<7;++bad){reset();request=create;
      if(bad==0)request.metadata.record.header.object_uuid.value=Id(180);
      if(bad==1)request.metadata.record.header.row_uuid.value=Id(160);
      if(bad==2)request.expected_version_uuid=Id(170);
      if(bad==3)request.metadata.definition_version=2;
      if(bad==4)request.metadata.creator_local_transaction_id=17;
      if(bad==5)request.relation_uuid.value=Id(212);
      if(bad==6)request.page_number=22;
      empty(stage(budget));Check(actual(target,21,sizes[profile])==blank,"invalid replacement/identity cannot write");unchanged();
    }
    request=create;reset();empty(stage(1));Check(actual(target,21,sizes[profile])==blank,"insufficient actual-image budget refuses before write");
    request=create;reset();
    catalog::CatalogNameEntry name;name.name_entry_uuid=request.metadata.record.header.object_uuid;name.name_vector_uuid={platform::UuidKind::object,Id(246)};name.object_uuid={platform::UuidKind::object,Id(245)};
    name.object_class="schema";name.scope_uuid={platform::UuidKind::object,Id(164)};name.parent_schema_uuid=request.metadata.owning_schema_uuid;name.language_tag="en";
    name.dialect_profile_uuid={platform::UuidKind::object,Id(247)};name.identifier_profile_uuid={platform::UuidKind::object,Id(248)};name.raw_name_text=name.display_name="actual staged name";
    name.normalized_lookup_key={0,0xfe,1};name.exact_lookup_key={0xff,0};name.catalog_generation_id=1;name.created_transaction_uuid=owner.transaction_uuid;
    name.security_policy_uuid={platform::UuidKind::object,Id(249)};name.resource_epoch=name.name_resolution_epoch=1;name.lifecycle_state=catalog::CatalogNameLifecycle::active;
    request.metadata.record.header.kind=catalog::CatalogRecordKind::localized_name;request.metadata.record.header.parent_uuid=name.name_vector_uuid;request.metadata.default_name_uuid=name.name_entry_uuid;
    request.metadata.name_vector_uuid=name.name_vector_uuid;request.metadata.security_policy_uuid=name.security_policy_uuid;request.metadata.resource_epoch=1;request.metadata.object_subtype="name_entry";request.metadata.record.payload.clear();request.name_payload=name;
    const auto name_create=request;
    result=stage(budget);verify(result,1,platform::Uuid{},false);
    const auto name_image=db::DecodeNativeCatalogLeaf(actual(target,21,sizes[profile]));const auto& name_row=name_image.page->body.rows[0];
    catalog::CatalogNameVersionBinding resident;resident.database_uuid={platform::UuidKind::database,Id(1)};resident.filespace_uuid={platform::UuidKind::filespace,fs};resident.row_uuid=request.metadata.record.header.row_uuid;
    resident.version_uuid={platform::UuidKind::row,result.row->version_uuid};resident.catalog_object_uuid=name.name_entry_uuid;resident.creating_transaction_uuid=owner.transaction_uuid;
    resident.page_id=21;resident.slot_id=1;resident.storage_generation=7;resident.version_sequence=1;resident.creating_transaction_number=13;resident.catalog_generation=1;
    const auto& name_payload=name_image.metadata.at(name_row.version_uuid).record.payload;
    const auto materialized=catalog::DecodeCatalogNameEnvelope(Bytes(name_payload.begin(),name_payload.end()),resident);
    Check(materialized.ok()&&std::get<catalog::CatalogNameEntry>(materialized.record->payload).raw_name_text=="actual staged name","typed name envelope bound to actual newly staged residency");
    request=create;
    if(p==0&&role==1){
      reset();reads=observed_full_digests=observed_allocations=0;track_reads=count_full_digests=count_allocations=true;
      result=stage(budget);track_reads=count_full_digests=count_allocations=false;Check(result.ok(),"measure successful complete version staging");
      const auto nr=reads,nf=observed_full_digests;const auto na=observed_allocations;
      for(unsigned long n=0;n<=na;++n){reset();allocation_budget=n;result=stage(budget);allocation_budget=-1;
        if(!result.ok())empty(result);else Check(db::DecodeNativeCatalogLeaf(actual(target,21,sizes[profile])).ok(),"allocation recovery preserves actual canonical successor");
        unchanged();if(n==na)Check(result.ok(),"version allocation sweep reaches success");}
      for(unsigned n=1;n<=nr;++n){reset();reads=0;read_fault=n;track_reads=true;result=stage(budget);track_reads=false;Check(!read_fault,"version actual read fault consumed");empty(result);unchanged();}
      for(unsigned n=1;n<=nf;++n){reset();full_digest_fault=n;result=stage(budget);Check(!full_digest_fault,"version full digest fault consumed");empty(result);unchanged();}
      for(unsigned mode=1;mode<=5;++mode){reset();hash_fault=mode;result=stage(budget);Check(!hash_fault,"version multipart digest failure consumed");empty(result);unchanged();}
      reset();reads=0;stage_corrupt_read=nr;track_reads=true;result=stage(budget);track_reads=false;Check(!stage_corrupt_read,"version actual readback corruption consumed");empty(result);unchanged();
      std::cout<<"catalog version allocations="<<na<<" reads="<<nr<<" full digests="<<nf<<std::endl;
      for(unsigned mode=1;mode<=2;++mode){reset();stage_write_fault=mode;empty(stage(budget));Check(!stage_write_fault,"version write failure actually exercised");unchanged();}
      reset();stage_sync_fault=1;empty(stage(budget));Check(!stage_sync_fault,"version sync failure actually exercised");unchanged();
      reset();inv.inventory.entries[0].rollback_only=true;persist();empty(stage(budget));Check(actual(target,21,sizes[profile])==blank,"rollback-only writer cannot stage version");inv.inventory.entries[0].rollback_only=false;persist();
      const auto saved_source=source_leaf;const auto saved_inventory=inv;
      const auto rewrite=[](auto& row,const auto& edit){auto metadata=catalog::DecodeCatalogMetadataVersion(row.cells[0].value.payload);Check(metadata.ok(),"decode conflict fixture metadata");edit(metadata.record);
        const auto encoded=catalog::EncodeCatalogMetadataVersion(metadata.record);Check(encoded.ok(),"encode conflict fixture metadata");row.cells[0].value.payload=encoded.bytes;};
      auto other=committed;other.identity.local_id=mga::MakeLocalTransactionId(18);other.identity.transaction_uuid.value=Id(213);other.state=mga::TransactionState::active;other.commit_sequence=0;
      inv.inventory.entries.push_back(other);inv.inventory.next_local_transaction_id=19;
      auto& predecessor=source_leaf.body.rows[0];predecessor.transaction_uuid=committed.identity.transaction_uuid;predecessor.local_transaction_id=17;
      rewrite(predecessor,[&](auto& m){m.creator_transaction_uuid=committed.identity.transaction_uuid;m.creator_local_transaction_id=17;});
      auto successor=predecessor;successor.version_uuid=Id(214);successor.row_version=2;successor.previous_version_uuid=Id(170);successor.previous_row_version=1;successor.stable_slot_id=successor.internal_row_ordinal=3;
      successor.transaction_uuid=other.identity.transaction_uuid;successor.local_transaction_id=18;
      rewrite(successor,[&](auto& m){m.creator_transaction_uuid=other.identity.transaction_uuid;m.creator_local_transaction_id=18;m.definition_version=2;});source_leaf.body.rows.push_back(successor);
      request=create;request.metadata=decoded.record;request.metadata.definition_version=2;request.expected_version_uuid=Id(170);
      for(unsigned committed_later=0;committed_later<2;++committed_later){
        if(committed_later){inv.inventory.entries.back().state=mga::TransactionState::committed;inv.inventory.entries.back().commit_sequence=2;inv.inventory.next_commit_sequence=3;}
        persist();reset();const auto retained=actual(first,30,sizes[p]);result=stage(budget);empty(result);
        if(result.error!=(committed_later?E::stale_version:E::row_reserved))std::cerr<<"conflict committed="<<committed_later<<" error="<<static_cast<int>(result.error)<<" source="<<static_cast<int>(result.source_error)<<std::endl;
        Check(result.error==(committed_later?E::stale_version:E::row_reserved)&&actual(target,21,sizes[profile])==blank&&actual(first,30,sizes[p])==retained,"hidden committed successor or unresolved writer prevents stale replacement");
      }
      for(const auto outcome:{mga::TransactionState::rolled_back,mga::TransactionState::failed_terminal})for(bool archived:{false,true}){
        source_leaf=saved_source;inv=saved_inventory;other.state=archived?mga::TransactionState::archived:outcome;other.archived_from_state=archived?outcome:mga::TransactionState::none;other.commit_sequence=0;
        inv.inventory.entries.push_back(other);inv.inventory.next_local_transaction_id=19;
        auto& released=source_leaf.body.rows[0];released.transaction_uuid=other.identity.transaction_uuid;released.local_transaction_id=18;
        rewrite(released,[&](auto& m){m.creator_transaction_uuid=other.identity.transaction_uuid;m.creator_local_transaction_id=18;});
        persist();reset();const auto retained=actual(first,30,sizes[p]);request=create;request.metadata.record.header.object_uuid.value=Id(180);result=stage(budget);
        Check(result.ok()&&result.row->row_version==1&&result.row->previous_version_uuid.is_nil()&&actual(first,30,sizes[p])==retained,"proven failed original creator releases object reservation without overwriting history");
        reset();request.metadata.record.header.row_uuid.value=Id(160);result=stage(budget);empty(result);Check(result.error==E::stale_version&&actual(target,21,sizes[profile])==blank,"released object still requires a fresh logical row identity");
      }
      source_leaf=saved_source;inv=saved_inventory;source_leaf.body.rows[0].row_version=std::numeric_limits<u64>::max();persist();reset();
      request=create;request.metadata=decoded.record;request.metadata.definition_version=2;request.expected_version_uuid=Id(170);
      result=stage(budget);empty(result);Check(result.error==E::version_overflow&&actual(target,21,sizes[profile])==blank,"retained sequence overflow never wraps");
      source_leaf=saved_source;auto source_name_binding=resident;source_name_binding.filespace_uuid.value=Id(2);source_name_binding.version_uuid.value=Id(170);source_name_binding.page_id=30;source_name_binding.storage_generation=1;
      const auto source_name=catalog::EncodeCatalogNameEnvelope({source_name_binding,*name_create.name_payload});Check(source_name.ok(),"source typed name residency fixture");
      source_leaf.body.rows[0].row_uuid=name_create.metadata.record.header.row_uuid;
      rewrite(source_leaf.body.rows[0],[&](auto& m){m=name_create.metadata;m.record.payload.assign(source_name.bytes.begin(),source_name.bytes.end());});
      persist();reset();const auto retained_name=actual(first,30,sizes[p]);request=name_create;request.expected_version_uuid=Id(170);request.metadata.definition_version=2;
      auto& replacement_name=std::get<catalog::CatalogNameEntry>(*request.name_payload);replacement_name.raw_name_text=replacement_name.display_name="replacement staged name";replacement_name.normalized_lookup_key={0,0xfe,2};
      result=stage(budget);Check(result.ok()&&result.row->row_version==2&&result.row->previous_version_uuid==Id(170)&&actual(first,30,sizes[p])==retained_name,"name replacement retains original identity and predecessor bytes");
      const auto replaced=db::DecodeNativeCatalogLeaf(actual(target,21,sizes[profile]));Check(replaced.ok(),"actual replacement name page");
      auto replaced_binding=resident;replaced_binding.version_uuid.value=result.row->version_uuid;replaced_binding.version_sequence=2;
      const auto& replaced_payload=replaced.metadata.at(result.row->version_uuid).record.payload;const auto replaced_name=catalog::DecodeCatalogNameEnvelope(Bytes(replaced_payload.begin(),replaced_payload.end()),replaced_binding);
      Check(replaced_name.ok()&&std::get<catalog::CatalogNameEntry>(replaced_name.record->payload).raw_name_text=="replacement staged name","replacement name uses new physical residency");
      reset();replacement_name.created_transaction_uuid=committed.identity.transaction_uuid;result=stage(budget);empty(result);Check(result.error==E::stale_version&&actual(target,21,sizes[profile])==blank,"name replacement cannot rewrite original creator");
      source_leaf=saved_source;inv=saved_inventory;persist();unchanged();
    }
    request=create;reset();revoke_on_stage_sync=&snapshot.published.descriptor.snapshot_uuid;result=stage(budget);
    Check(!revoke_on_stage_sync&&result.error==E::snapshot_failure,"snapshot revoked during actual stage sync");empty(result);
    Check(db::DecodeNativeCatalogLeaf(actual(target,21,sizes[profile])).ok(),"post-write snapshot refusal does not misrepresent durable reserved bytes");unchanged();
    reset();empty(stage(budget));Check(actual(target,21,sizes[profile])==blank,"revoked snapshot cannot stage");unchanged();
  }
}
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
        const bool active=state==mga::TransactionState::active;
        persist(false,active||state==mga::TransactionState::prepared||state==mga::TransactionState::failed_terminal?13:18,active?13:18);result=read(budget);
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
      inventory.inventory.entries[2].state=mga::TransactionState::prepared;persist(false,15,19);loaded=read(budget);Check(loaded.ok(),"actual prepared catalog writer inventory");
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
      // Names bind the actual resident context on both filespaces and retain
      // original creation across independently stored version predecessors.
      const auto as_name=[&](unsigned leaf_index,const auto& origin,bool wrong_residency=false) {
        auto& leaf=images.leaves[leaf_index];auto& row=leaf.body.rows[0];
        rewrite(row,[&](auto& metadata) {
          catalog::CatalogNameEntry entry;entry.name_entry_uuid=metadata.record.header.object_uuid;
          entry.name_vector_uuid={platform::UuidKind::object,Id(246)};entry.object_uuid={platform::UuidKind::object,Id(245)};
          entry.object_class="schema";entry.scope_uuid={platform::UuidKind::object,Id(164)};
          entry.parent_schema_uuid=metadata.owning_schema_uuid;entry.language_tag="en";
          entry.dialect_profile_uuid={platform::UuidKind::object,Id(247)};entry.identifier_profile_uuid={platform::UuidKind::object,Id(248)};
          entry.raw_name_text=entry.display_name=row.row_version==1?"original":"replacement";
          entry.normalized_lookup_key={0,0xfe,static_cast<byte>(row.row_version)};entry.exact_lookup_key={0xff,0};
          entry.catalog_generation_id=metadata.catalog_generation;entry.created_transaction_uuid=origin;
          entry.security_policy_uuid={platform::UuidKind::object,Id(249)};entry.resource_epoch=1;
          entry.name_resolution_epoch=row.row_version;entry.lifecycle_state=catalog::CatalogNameLifecycle::active;
          metadata.record.header.kind=catalog::CatalogRecordKind::localized_name;
          metadata.record.header.parent_uuid=entry.name_vector_uuid;metadata.default_name_uuid=entry.name_entry_uuid;
          metadata.name_vector_uuid=entry.name_vector_uuid;metadata.security_policy_uuid=entry.security_policy_uuid;
          metadata.resource_epoch=entry.resource_epoch;metadata.object_subtype="name_entry";
          catalog::CatalogNameVersionBinding b;
          b.database_uuid={platform::UuidKind::database,leaf.header.database_uuid};b.filespace_uuid={platform::UuidKind::filespace,leaf.header.filespace_uuid};
          b.row_uuid=row.row_uuid;b.version_uuid={platform::UuidKind::row,row.version_uuid};b.catalog_object_uuid=entry.name_entry_uuid;
          b.creating_transaction_uuid=row.transaction_uuid;b.page_id=leaf.body.page_number;b.slot_id=row.stable_slot_id;
          b.storage_generation=row.storage_generation+(wrong_residency?1:0);b.version_sequence=row.row_version;
          b.creating_transaction_number=row.local_transaction_id;b.catalog_generation=metadata.catalog_generation;
          const auto encoded=catalog::EncodeCatalogNameEnvelope({b,entry});Check(encoded.ok(),"complete cross-page name fixture");
          metadata.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
        });
      };
      as_name(0,old.transaction_uuid);as_name(3,old.transaction_uuid);persist(false,19,19);
      selected=pinned(fresh_pin.pin,reader.identity);expected(selected,next_version);
      for(const auto& row:selected.rows)if(row.version_uuid==next_version)Check(row.name_payload &&
        std::get<catalog::CatalogNameEntry>(*row.name_payload).display_name=="replacement","pinned successor name not materialized");
      selected=pinned(history_pin.pin,reader.identity);expected(selected,old.version_uuid);
      for(const auto& row:selected.rows)if(row.version_uuid==old.version_uuid)Check(row.name_payload &&
        std::get<catalog::CatalogNameEntry>(*row.name_payload).display_name=="original","pinned historical name not preserved");
      as_name(3,writer.identity.transaction_uuid);persist(false,19,19);loaded=read(budget);
      Check(loaded.ok(),"cross-page forged name origin must remain individually valid");
      selected=pinned(fresh_pin.pin,reader.identity);no_rows(selected);Check(selected.error==PE::invalid_chain,"cross-page name origin replacement admitted");
      selected=pinned(history_pin.pin,reader.identity);no_rows(selected);Check(selected.error==PE::invalid_chain,"hidden cross-page name origin replacement admitted");
      as_name(3,old.transaction_uuid,true);persist(false,19,19);loaded=read(budget);empty(loaded);
      selected=pinned(fresh_pin.pin,reader.identity);no_rows(selected);Check(selected.error==PE::source_failure,"name envelope trusted its own invented residency");
      images=history_images;
      // An unindexed row on another page still reserves its object identity.
      // Outcome comes from native inventory, not visibility or lifecycle text.
      const auto duplicate_inventory=inventory;
      for(const auto& [state,origin]:std::vector<std::pair<mga::TransactionState,mga::TransactionState>>{
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
        persist(false,pending||outcome==mga::TransactionState::failed_terminal?15:19,state==mga::TransactionState::active?15:19);
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
      images=history_images;inventory.inventory.entries[2].state=mga::TransactionState::limbo;inventory.inventory.entries[2].archived_from_state=mga::TransactionState::none;inventory.inventory.entries[2].commit_sequence=0;persist(false,15,19);
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
Bytes SelectionOracle(const db::NativeCheckpointSelection& s){
  auto common=RootExample();common.header=s.header;auto b=RootOracle(common);std::fill(b.begin()+128,b.end(),0);
  std::copy_n("SBDCP001",8,b.begin()+128);Number(b,136,2,1);Number(b,138,2,384);Number(b,140,4,512);
  PutUuid(b,144,s.object_uuid);PutUuid(b,160,s.bootstrap_uuid);Number(b,176,8,s.selection_generation);PutUuid(b,184,s.publication_uuid);
  const auto ref=[&](unsigned at,const auto& r){PutUuid(b,at,r.filespace_uuid);Number(b,at+16,8,r.page_number);Number(b,at+24,8,r.page_generation);PutUuid(b,at+32,r.page_size_profile_uuid);};
  ref(200,s.checkpoint);PutUuid(b,248,s.checkpoint_object_uuid);std::copy(s.checkpoint_sha256.begin(),s.checkpoint_sha256.end(),b.begin()+264);
  Number(b,296,8,s.checkpoint_generation);Number(b,304,8,s.root_set_generation);PutUuid(b,312,s.timeline_uuid);Number(b,328,8,s.previous_selection_generation);
  if(s.previous_checkpoint)ref(336,*s.previous_checkpoint);PutUuid(b,384,s.previous_checkpoint_object_uuid);std::copy(s.previous_checkpoint_sha256.begin(),s.previous_checkpoint_sha256.end(),b.begin()+400);
  const auto digest=WholeRootHash(b);std::copy(digest.begin(),digest.end(),b.begin()+432);return b;
}
void CanonicalBoundCheckpointSelection(bool inventory_staging=false,bool mixed_inventory=false,bool history_only=false,unsigned history_shard=0,unsigned history_stride=1,int inventory_allocation_shard=-1,bool directory_staging=false,bool directory_controls=false,bool directory_graph=false){using E=db::NativeCheckpointSelectionError;using S=page::NativeAllocationState;
  for(unsigned p=0;p<5;++p)for(unsigned role=1;role<=4;++role){
    if(inventory_allocation_shard>=0&&(p!=0||role!=1))continue;
    Fixture fixture;disk::FileDevice device,second_device;const unsigned q=(p+1)%5;const auto path=(fixture.root/"bound-selector").string();auto zero=Example(p,role);zero.free_pages=zero.preallocated_pages=0;
    auto second_zero=Example(q,5);second_zero.bootstrap.filespace_uuid=Id(7);second_zero.page_uuid=Id(8);second_zero.free_pages=second_zero.preallocated_pages=0;for(auto& r:second_zero.roots)r.filespace_uuid=Id(7);
    second_zero.roots.front().object_uuid=Id(179);
    zero.roots.push_back({18,0x30e,Id(2),31,1,Profile(p),Id(154)});zero.roots.push_back({19,0x30e,Id(2),32,1,Profile(p),Id(154)});
    for(unsigned bad=0;bad<6;++bad){auto z=zero;if(bad==0)z.roots.pop_back();if(bad==1)z.roots.erase(z.roots.end()-2);if(bad==2)z.roots.back().object_uuid=Id(177);
      if(bad==3){z.roots.back().filespace_uuid=Id(7);z.roots.back().page_size_profile_uuid=Profile(q);}if(bad==4)z.roots.back().page_number=31;if(bad==5)z.bootstrap.filespace_role=5;
      Invalid(z,bad<2?disk::FilespacePageZeroError::required_root_missing:disk::FilespacePageZeroError::invalid_root_directory);}
    Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"own bound selector node");const byte pad=0;Check(device.WriteAt(64*sizes[p]-1,&pad,1).ok(),"selector actual capacity");
    Check(second_device.Open((fixture.root/"bound-selector-secondary").string(),disk::FileOpenMode::create_new).ok()&&second_device.WriteAt(64*sizes[q]-1,&pad,1).ok(),"own mixed-profile secondary filespace");
    auto inv=InventoryExample(p);inv.inventory.next_local_transaction_id=18;inv.inventory.next_commit_sequence=2;
    auto& writer=inv.inventory.entries[0];writer.identity.local_id=mga::MakeLocalTransactionId(13);writer.identity.transaction_uuid.value=Id(162);writer.state=mga::TransactionState::active;writer.commit_sequence=0;
    auto committed=writer;committed.identity.local_id=mga::MakeLocalTransactionId(17);committed.identity.transaction_uuid.value=Id(98);committed.state=mga::TransactionState::committed;committed.commit_sequence=1;inv.inventory.entries.push_back(committed);
    auto initial=CheckpointExample(p),current=initial;current.header.page_number=36;current.header.page_generation=110;current.header.page_uuid=Id(95);current.checkpoint_generation=2;current.root_set_generation=9;
    current.roots[4].page.page_number=50;current.roots[5].page.page_number=51;
    current.predecessor=disk::NativePageReference{Id(2),19,109,Profile(p)};
    page::NativeAllocationMap map;map.header={sizes[p],3,Id(1),Id(2),Id(164),35,104,0,Profile(p)};map.object_uuid=Id(43);map.map_generation=2;map.capacity_generation=1;
    map.total_pages=64;map.creator_transaction_uuid=Id(98);map.creator_local_transaction_id=17;map.states.assign(64,S::quarantined);
    for(unsigned n:{0u,21u,31u,32u,35u}){page::NativeAllocationRecord r;r.page_number=n;r.allocation_uuid=Id(180+n);r.creator_transaction_uuid=Id(98);r.creator_local_transaction_id=17;map.states[n]=S::allocated;
      if(n==0){r.page_uuid=zero.page_uuid;r.page_generation=zero.page_generation;r.page_type=role<=4?1:2;r.owner_uuid=Id(2);}
      if(n==21){r.page_uuid=Id(150);r.page_generation=7;r.page_type=6;r.owner_uuid=Id(101);r.creator_transaction_uuid=Id(162);r.creator_local_transaction_id=13;map.states[n]=S::reserved;}
      if(n==31||n==32){r.page_uuid=Id(n==31?155:156);r.page_generation=1;r.page_type=0x30e;r.owner_uuid=Id(154);}
      if(n==35){r.page_uuid=Id(164);r.page_generation=104;r.page_type=3;r.owner_uuid=Id(43);}map.records.push_back(r);}
    for(const auto& control:std::vector<db::NativeInventoryPageBinding>{{inv.header,inv.object_uuid},{initial.header,initial.object_uuid},{current.header,current.object_uuid}}){
      const auto& h=control.header;map.states[h.page_number]=S::allocated;
      map.records.push_back({h.page_number,Id(100+h.page_number),h.page_uuid,control.object_uuid,Id(98),17,h.page_generation,0,h.page_type,{}});}
    std::sort(map.records.begin(),map.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});
    // Selected map counters differ from the immutable initial bootstrap.
    map.states[60]=S::free;
    page::NativeFilespaceDirectory directory;directory.header={sizes[p],9,Id(1),Id(2),Id(165),15,105,0,Profile(p)};directory.object_uuid=Id(45);directory.directory_generation=1;directory.creator_transaction_uuid=Id(98);directory.creator_local_transaction_id=17;directory.total_records=2;
    directory.records.push_back({zero.bootstrap,Id(166),zero.page_uuid,zero.page_generation,zero.root_set_generation,zero.total_pages,0,{}});
    directory.records.push_back({second_zero.bootstrap,Id(176),second_zero.page_uuid,second_zero.page_generation,second_zero.root_set_generation,second_zero.total_pages,0,{}});
    page::NativeRetentionPage retention;retention.header={sizes[p],0x303,Id(1),Id(2),Id(167),40,140,0,Profile(p)};retention.object_uuid=Id(168);retention.epoch=1;retention.creator_transaction_uuid=Id(98);retention.creator_local_transaction_id=17;
    page::NativeHorizonRoot horizon;horizon.header={sizes[p],0x302,Id(1),Id(2),Id(169),41,141,0,Profile(p)};horizon.object_uuid=Id(170);horizon.epoch=1;horizon.creator_transaction_uuid=Id(98);horizon.creator_local_transaction_id=17;
    horizon.retention={Id(2),40,140,Profile(p)};horizon.retention_object_uuid=Id(168);
    db::NativeSystemState system;system.header={sizes[p],8,Id(1),Id(2),Id(171),11,101,0,Profile(p)};system.object_uuid=Id(41);system.state_generation=system.restart_generation=system.startup_counter=1;
    system.creator_transaction_uuid=Id(98);system.creator_local_transaction_id=17;system.lifecycle=db::NativeSystemLifecycle::opening;system.recovery=db::NativeSystemRecovery::checkpoint_rebuild;system.flags=db::NativeSystemFlag::dirty|db::NativeSystemFlag::write_fenced;
    system.checkpoint_generation=2;system.checkpoint=disk::NativePageReference{Id(2),36,110,Profile(p)};system.checkpoint_object_uuid=Id(49);system.transition_operation_uuid=Id(172);
    db::NativeCheckpointSelection selection;selection.header={sizes[p],0x30e,Id(1),Id(2),Id(155),31,1,0,Profile(p)};selection.object_uuid=Id(154);selection.bootstrap_uuid=zero.page_uuid;
    selection.publication_uuid=Id(153);selection.selection_generation=2;selection.checkpoint={Id(2),36,110,Profile(p)};selection.checkpoint_object_uuid=Id(49);selection.checkpoint_generation=2;selection.root_set_generation=9;selection.timeline_uuid=Id(97);
    selection.previous_selection_generation=1;selection.previous_checkpoint=disk::NativePageReference{Id(2),19,109,Profile(p)};selection.previous_checkpoint_object_uuid=Id(49);
    std::optional<page::NativeTransactionInventoryPage> inventory_tail, preceding_inventory;
    std::array<u64,3> inventory_summary{13,13,13}, preceding_summary{13,13,13};
    const auto put=[&](u64 number,const Bytes& b){Check(device.WriteAt(number*sizes[p],b.data(),b.size()).ok()&&device.Sync().ok(),"persist independently authored selector graph");};
    const auto persist=[&](){const bool extended=!directory.creator_operation_uuid.is_nil()||std::any_of(directory.records.begin(),directory.records.end(),[](const auto& r){return r.allocation_root.has_value();});
      const auto ib=InventoryOracle(inv,inventory_summary[0],inventory_summary[1],inventory_summary[2]),mb=AllocationOracle(map),dbb=extended?DirectoryAllocationOracle(directory):DirectoryOracle(directory),rb=RetentionOracle(retention),sb=SystemStateOracle(system);horizon.retention_sha256=WholeRootHash(rb);const auto hb=HorizonOracle(horizon);
      for(auto* cp:{&initial,&current}){cp->roots[0]={1,0x301,InventoryRef(inv),inv.object_uuid,WholeRootHash(ib)};
        cp->roots[2]={3,9,{Id(2),15,105,Profile(p)},Id(45),WholeRootHash(dbb)};}
      if(preceding_inventory){const auto& prior=*preceding_inventory;
        const auto prior_bytes=InventoryOracle(prior,preceding_summary[0],preceding_summary[1],preceding_summary[2]);
        initial.roots[0]={1,0x301,InventoryRef(prior),prior.object_uuid,WholeRootHash(prior_bytes)};
        initial.selected_local_transaction_id=prior.inventory.next_local_transaction_id-1;
        current.selected_local_transaction_id=inv.inventory.next_local_transaction_id-1;
        put(prior.header.page_number,prior_bytes);}
      current.roots[3]={4,3,{Id(2),35,104,Profile(p)},Id(43),WholeRootHash(mb)};
      current.roots[2]={3,9,{Id(2),15,105,Profile(p)},Id(45),WholeRootHash(dbb)};
      current.roots[7]={8,8,{Id(2),11,101,Profile(p)},Id(41),WholeRootHash(sb)};
      current.roots[1]={2,0x302,{Id(2),41,141,Profile(p)},Id(170),WholeRootHash(hb)};
      current.roots[9]={10,0x303,{Id(2),40,140,Profile(p)},Id(168),WholeRootHash(rb)};
      const auto old=CheckpointOracle(initial);current.predecessor_sha256=WholeRootHash(old);selection.previous_checkpoint_sha256=current.predecessor_sha256;
      const auto cp=CheckpointOracle(current);selection.checkpoint_sha256=WholeRootHash(cp);
      auto other=selection;other.header.page_number=32;other.header.page_uuid=Id(156);
      const auto secondary_bytes=Oracle(second_zero);Check(second_device.WriteAt(0,secondary_bytes.data(),secondary_bytes.size()).ok()&&second_device.Sync().ok(),"persist secondary bootstrap");
      if(inventory_tail){const auto tail_bytes=InventoryOracle(*inventory_tail,18,18,18);
        Check(second_device.WriteAt(inventory_tail->header.page_number*sizes[q],tail_bytes.data(),tail_bytes.size()).ok()&&second_device.Sync().ok(),"persist mixed-profile inventory continuation");}
      put(0,Oracle(zero));put(14,ib);put(35,mb);put(15,dbb);put(40,rb);put(41,hb);put(11,sb);put(19,old);put(36,cp);put(31,SelectionOracle(selection));put(32,SelectionOracle(other));};
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(7),Profile(q),&second_device},{Id(2),Profile(p),&device}};const u64 budget=8*sizes[p];
    const auto read=[&](u64 limit){return db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),devices,Id(2),limit);};
    const auto empty=[&](const auto& r){Check(!r.ok()&&!r.selection&&r.slots[0].empty()&&r.slots[1].empty()&&!r.checkpoint_inventory.checkpoint&&!r.predecessor.checkpoint&&r.checkpoint_inventory.inventory_pages.empty()&&r.predecessor.inventory_pages.empty()&&r.allocation.pages.empty()&&!r.retained_image_bytes,"bound selector failure returns no prefix");};
    const auto consumers=[&](const auto& ref,bool expected){Check(db::VerifyCurrentNativeCheckpointAllocationFromOpenDevices(Id(1),devices,ref,budget).ok()==expected,"current allocation uses selected root");
      Check(db::VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(Id(1),devices,ref,budget).ok()==expected,"current directory uses selected root");
      const auto sr=db::VerifyCurrentNativeCheckpointSystemStateFromOpenDevices(Id(1),devices,ref,budget);
      if(sr.ok()!=expected)std::cerr<<"selected system error="<<static_cast<int>(sr.error)<<" state="<<static_cast<int>(sr.system_error)<<std::endl;
      Check(sr.ok()==expected,"current system state uses selected root");
      Check(db::VerifyCurrentNativeCheckpointHorizonFromOpenDevices(Id(1),devices,ref,9*u64{sizes[p]}).ok()==expected,"current horizons use selected root and charge horizon plus retention images");};
    if(directory_controls||directory_graph){
      if(directory_graph){zero.root_set_generation=initial.root_set_generation=1;current.root_set_generation=selection.root_set_generation=2;directory.records[0].root_set_generation=1;}
      second_zero=Example(q,2);second_zero.bootstrap.filespace_uuid=Id(7);second_zero.page_uuid=Id(8);second_zero.free_pages=second_zero.preallocated_pages=0;
      for(auto& r:second_zero.roots)r.filespace_uuid=Id(7);second_zero.roots[2].object_uuid=Id(179);directory.records[1].bootstrap=second_zero.bootstrap;
      inventory_tail=inv;inventory_tail->header={sizes[q],0x301,Id(1),Id(7),Id(201),14,104,0,Profile(q)};
      inventory_tail->inventory.entries.erase(inventory_tail->inventory.entries.begin());inventory_tail->previous=InventoryRef(inv);inv.inventory.entries.pop_back();inv.next=InventoryRef(*inventory_tail);
      page::NativeAllocationMap secondary;secondary.header={sizes[q],3,Id(1),Id(7),Id(202),13,103,0,Profile(q)};
      secondary.object_uuid=Id(179);secondary.map_generation=secondary.capacity_generation=1;secondary.total_pages=64;
      secondary.creator_transaction_uuid=Id(98);secondary.creator_local_transaction_id=17;secondary.states.assign(64,S::quarantined);
      for(unsigned n:{0u,13u,14u}){page::NativeAllocationRecord r;r.page_number=n;r.allocation_uuid=Id(220+n);r.creator_transaction_uuid=Id(98);r.creator_local_transaction_id=17;secondary.states[n]=S::allocated;
        if(n==0){r.page_uuid=Id(8);r.page_generation=7;r.page_type=1;r.owner_uuid=Id(7);}
        if(n==13){r.page_uuid=Id(202);r.page_generation=103;r.page_type=3;r.owner_uuid=Id(179);}
        if(n==14){r.page_uuid=Id(201);r.page_generation=104;r.page_type=0x301;r.owner_uuid=inv.object_uuid;}secondary.records.push_back(r);}
      const auto store_map=[&](const auto& m){const auto b=AllocationOracle(m);Check(second_device.WriteAt(m.header.page_number*sizes[q],b.data(),b.size()).ok()&&second_device.Sync().ok(),"persist selected secondary control map");};
      store_map(secondary);const auto bootstrap_map=secondary;
      secondary.header.page_number=22;secondary.header.page_generation=104;secondary.header.page_uuid=Id(204);secondary.map_generation=2;
      secondary.states[22]=S::allocated;secondary.records.push_back({22,Id(242),Id(204),Id(179),Id(98),17,104,0,3,{}});store_map(secondary);
      directory.records[1].allocation_root=page::NativeFilespaceAllocationRoot{{Id(7),22,104,Profile(q)},Id(179),WholeRootHash(AllocationOracle(secondary)),2,1};
      directory.records[0].allocation_root=page::NativeFilespaceAllocationRoot{{Id(2),35,104,Profile(p)},Id(43),WholeRootHash(AllocationOracle(map)),map.map_generation,map.capacity_generation};
      const auto baseline=directory;const u64 limit=8*u64{sizes[p]}+3*u64{sizes[q]};
      if(directory_graph){
        const auto graph=[&](u64 allowance){return db::ReadNativeManagementControlGraphFromOpenDevices(Id(1),devices,Id(2),
          {selection.checkpoint,selection.checkpoint_object_uuid,selection.checkpoint_sha256,selection.checkpoint_generation,selection.root_set_generation,selection.timeline_uuid},allowance);};
        const auto no_graph=[&](const auto& r){Check(!r.ok()&&!r.anchor&&r.publications.empty()&&r.allocations.empty()&&!r.verified_image_bytes,"invalid directory graph exposes no authority prefix");};
        persist();const auto original=graph(64*u64{sizes[p]});
        if(!original.ok())std::cerr<<"directory graph fixture error="<<int(original.error)<<'\n';
        Check(original.ok(),"independent transaction-created genesis ancestry admits graph fixture");
        for(unsigned bad=0;bad<9;++bad){directory=baseline;auto& r=*directory.records[1].allocation_root;
          if(bad==0)r.sha256[0]^=1;if(bad==1)++r.map_generation;if(bad==2)++r.capacity_generation;if(bad==3)++r.page.page_generation;
          if(bad==4)r.object_uuid=Id(178);if(bad==5)r.page.page_number=23;
          if(bad==6){directory.creator_transaction_uuid=Id(162);directory.creator_local_transaction_id=13;}
          if(bad==7)directory.creator_transaction_uuid=Id(177);
          if(bad==8){directory.creator_transaction_uuid={};directory.creator_local_transaction_id=0;directory.creator_operation_uuid=Id(177);}
          persist();stage_writes=stage_syncs=0;no_graph(graph(64*u64{sizes[p]}));Check(!stage_writes&&!stage_syncs,"directory graph refusal is read-only");}
        directory=baseline;persist();const u64 graph_limit=14*u64{sizes[p]}+4*u64{sizes[q]};const auto admitted=graph(graph_limit);
        if(!admitted.ok()||admitted.verified_image_bytes!=graph_limit)std::cerr<<"directory graph error="<<int(admitted.error)<<" bytes="<<admitted.verified_image_bytes<<" expected="<<graph_limit<<'\n';
        Check(admitted.ok()&&admitted.verified_image_bytes==graph_limit&&admitted.publications.empty()&&admitted.allocations.empty(),"immutable graph charges complete history checkpoints inventory maps and bound directory");
        no_graph(graph(graph_limit-1));
        auto stale=bootstrap_map;stale.records.pop_back();stale.states[14]=S::quarantined;store_map(stale);
        Check(graph(graph_limit).ok(),"immutable graph uses its own checkpoint directory instead of obsolete bootstrap map");
        directory.records[1].allocation_root.reset();persist();no_graph(graph(graph_limit));store_map(bootstrap_map);Check(graph(graph_limit).ok(),"graph bootstrap fallback only when binding absent");
        directory=baseline;
        for(unsigned bad=0;bad<3;++bad){auto changed=secondary;auto& r=changed.records[1];
          if(bad==0)r.allocation_uuid=map.records.front().allocation_uuid;
          if(bad==1)r.page_uuid=map.records.front().page_uuid;
          if(bad==2){changed.creator_transaction_uuid=Id(162);changed.creator_local_transaction_id=13;}
          store_map(changed);directory.records[1].allocation_root->sha256=WholeRootHash(AllocationOracle(changed));persist();no_graph(graph(graph_limit));}
        store_map(secondary);directory=baseline;persist();Check(graph(graph_limit).ok(),"restored original graph ownership admitted");
        if(p==0&&role==1){byte warm=0;for(unsigned n=0;n<4097;++n)Check(device.ReadAt(0,&warm,1).ok()&&second_device.ReadAt(0,&warm,1).ok(),"warm graph reader telemetry before full fault measurement");
          reads=observed_full_digests=observed_allocations=0;track_reads=count_full_digests=count_allocations=true;const auto measured=graph(graph_limit);track_reads=count_full_digests=count_allocations=false;
          Check(measured.ok(),"measure immutable directory graph fault sites");const auto nr=reads,nf=observed_full_digests;const auto na=observed_allocations;stage_writes=stage_syncs=0;
          for(unsigned n=1;n<=nr;++n){reads=0;read_fault=n;track_reads=true;const auto r=graph(graph_limit);track_reads=false;Check(!read_fault,"immutable graph read fault consumed");no_graph(r);}
          for(unsigned n=1;n<=nf;++n){full_digest_fault=n;const auto r=graph(graph_limit);Check(!full_digest_fault,"immutable graph full digest fault consumed");no_graph(r);Check(r.error==db::NativeManagementControlAuthorityError::hash_failure,"immutable graph hash failure classification");}
          for(unsigned n=1;n<=5;++n){hash_fault=n;const auto r=graph(graph_limit);Check(!hash_fault,"immutable graph multipart backend fault consumed");no_graph(r);}
          unsigned long consumed=0;
          for(unsigned long n=0;n<=na;++n){const auto loss=device.failed_io_latency_observations()+second_device.failed_io_latency_observations();
            allocation_budget=n;const auto r=graph(graph_limit);const auto remaining=allocation_budget;allocation_budget=-1;
            if(n<na){Check(remaining<0,"immutable graph allocation fault consumed");++consumed;}
            if(r.ok()){Check(r.anchor==measured.anchor&&r.verified_image_bytes==graph_limit&&r.allocations.empty()&&r.publications.empty(),"injected graph success preserves full exact authority");
              if(remaining<0)Check(device.failed_io_latency_observations()+second_device.failed_io_latency_observations()==loss+1,"only recorded optional telemetry loss preserves graph success");}
            else{no_graph(r);Check(r.error==db::NativeManagementControlAuthorityError::resource_exhausted,"immutable graph allocation resource classification");}
            if(n==na)Check(r.ok()&&remaining>=0,"immutable graph terminal uninjected allocation success");
          }
          Check(consumed==na&&!stage_writes&&!stage_syncs,"complete immutable graph fault sweep is read-only");
          std::cout<<"directory graph allocations="<<na<<" consumed="<<consumed<<" reads="<<nr<<" digests="<<nf<<std::endl;
        }
        // An explicitly inspected ancestor owns its own directory, even when
        // the currently selected graph is unavailable. This grants no fallback
        // selection or serving permission for the older checkpoint.
        auto historical_directory=baseline;historical_directory.header.page_number=16;historical_directory.header.page_generation=106;historical_directory.header.page_uuid=Id(206);
        historical_directory.records[1].allocation_root=page::NativeFilespaceAllocationRoot{{Id(7),13,103,Profile(q)},Id(179),WholeRootHash(AllocationOracle(bootstrap_map)),1,1};
        const auto hb=DirectoryAllocationOracle(historical_directory);put(16,hb);
        auto ancestor=initial;ancestor.roots[2]={3,9,{Id(2),16,106,Profile(p)},Id(45),WholeRootHash(hb)};ancestor.roots[3]=current.roots[3];
        // The original example's catalog root used slot35, now occupied by
        // the selected map. Preserve distinct physical root addresses.
        ancestor.roots[4]=current.roots[4];ancestor.roots[5]=current.roots[5];
        const auto ab=CheckpointOracle(ancestor);Check(db::DecodeNativeCheckpointRoot(ab).ok(),"historical checkpoint fixture has no conflicting physical root aliases");put(19,ab);put(15,Bytes(sizes[p]));
        const db::NativeManagementCheckpointAnchor anchor{{Id(2),19,109,Profile(p)},Id(49),WholeRootHash(ab),1,1,Id(97)};
        const auto historical=db::ReadNativeManagementControlGraphFromOpenDevices(Id(1),devices,Id(2),anchor,11*u64{sizes[p]}+4*u64{sizes[q]});
        if(!historical.ok()||historical.verified_image_bytes!=11*u64{sizes[p]}+4*u64{sizes[q]})std::cerr<<"historical directory graph error="<<int(historical.error)<<" bytes="<<historical.verified_image_bytes<<'\n';
        Check(historical.ok()&&historical.anchor==anchor&&historical.verified_image_bytes==11*u64{sizes[p]}+4*u64{sizes[q]},"immutable ancestor uses its own directory and root without borrowing selected graph");
        persist();
        Check(device.Close().ok()&&second_device.Close().ok(),"release immutable directory graph fixture for fresh executable");
        const auto child=::fork();Check(child>=0,"fork immutable directory graph reader");
        if(child==0){const auto profile=std::to_string(p);::execl("/proc/self/exe","directory-graph-probe","--directory-graph-probe",fixture.root.c_str(),profile.c_str(),nullptr);::_exit(125);}
        int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable verifies actual immutable directory control graph");
        continue;
      }
      for(unsigned bad=0;bad<13;++bad){directory=baseline;auto& binding=*directory.records[1].allocation_root;
        if(bad==0)binding.sha256[0]^=1;if(bad==1)++binding.map_generation;if(bad==2)++binding.capacity_generation;
        if(bad==3)++binding.page.page_generation;if(bad==4)binding.object_uuid=Id(178);if(bad==5)binding.page.page_number=23;
        if(bad==6){directory.creator_transaction_uuid=Id(162);directory.creator_local_transaction_id=13;}
        if(bad==7){directory.creator_transaction_uuid=Id(177);directory.creator_local_transaction_id=17;}
        auto& primary=*directory.records[0].allocation_root;
        if(bad==8)primary.sha256[0]^=1;if(bad==9)++primary.map_generation;if(bad==10)++primary.capacity_generation;
        if(bad==11)++primary.page.page_number;if(bad==12)primary.object_uuid=Id(178);
        persist();stage_writes=stage_syncs=0;empty(read(limit));Check(!stage_writes&&!stage_syncs,"invalid directory control binding performs no writes");}
      directory=baseline;persist();auto admitted=read(limit);
      Check(admitted.ok()&&admitted.retained_image_bytes==limit&&admitted.checkpoint_inventory.inventory_pages.size()==2&&admitted.predecessor.inventory_pages.size()==2,"selected control map binds directory and charges all mixed-profile authority work");
      const auto exhausted=read(limit-1);empty(exhausted);Check(exhausted.error==E::resource_exhausted,"selected directory verification work cannot be omitted from allowance");
      auto stale=bootstrap_map;stale.records.pop_back();stale.states[14]=S::quarantined;store_map(stale);
      Check(read(limit).ok(),"selected secondary control map supersedes obsolete bootstrap authority");
      directory.records[1].allocation_root.reset();persist();empty(read(limit));
      store_map(bootstrap_map);Check(read(limit).ok(),"absent directory binding retains valid bootstrap control map");
      directory=baseline;persist();
      auto resealed=directory;++resealed.directory_generation;put(15,DirectoryAllocationOracle(resealed));empty(read(limit));persist();
      for(unsigned bad=0;bad<11;++bad){auto changed=secondary;
        auto it=std::find_if(changed.records.begin(),changed.records.end(),[](const auto& r){return r.page_number==14;});
        if(bad==0){changed.records.erase(it);changed.states[14]=S::quarantined;}
        else{auto& r=*it;if(bad==1)changed.states[14]=S::reserved;if(bad==2)r.page_uuid=Id(205);if(bad==3)++r.page_generation;
          if(bad==4)r.page_type=6;if(bad==5)r.owner_uuid=Id(205);if(bad==6){r.creator_transaction_uuid=Id(162);r.creator_local_transaction_id=13;}
          if(bad==7)r.allocation_uuid=map.records.front().allocation_uuid;if(bad==8)r.page_uuid=inv.header.page_uuid;
          if(bad==9){changed.creator_transaction_uuid=Id(162);changed.creator_local_transaction_id=13;}if(bad==10)r.creator_transaction_uuid=Id(205);}
        store_map(changed);directory.records[1].allocation_root->sha256=WholeRootHash(AllocationOracle(changed));persist();stage_writes=stage_syncs=0;
        empty(read(limit));Check(!stage_writes&&!stage_syncs,"selected explicit control map retains original ownership and state checks");
      }
      store_map(secondary);directory=baseline;persist();
      if(p==0&&role==1){byte warm=0;for(unsigned n=0;n<4097;++n)Check(device.ReadAt(0,&warm,1).ok()&&second_device.ReadAt(0,&warm,1).ok(),"warm selected control telemetry before full fault measurement");
        reads=observed_full_digests=observed_allocations=0;track_reads=count_full_digests=count_allocations=true;admitted=read(limit);track_reads=count_full_digests=count_allocations=false;
        Check(admitted.ok(),"measure complete selected directory control authority");const auto nr=reads,nf=observed_full_digests;const auto na=observed_allocations;
        stage_writes=stage_syncs=0;
        for(unsigned n=1;n<=nr;++n){reads=0;read_fault=n;track_reads=true;auto r=read(limit);track_reads=false;Check(!read_fault,"selected directory control read fault consumed");empty(r);}
        for(unsigned n=1;n<=nf;++n){full_digest_fault=n;auto r=read(limit);Check(!full_digest_fault,"selected directory control digest fault consumed");empty(r);Check(r.error==E::hash_failure,"selected control hash failure classification");}
        for(unsigned n=1;n<=5;++n){hash_fault=n;auto r=read(limit);Check(!hash_fault,"selected directory control multipart hash fault consumed");empty(r);}
        unsigned long consumed=0;
        for(unsigned long n=0;n<=na;++n){const auto loss=device.failed_io_latency_observations()+second_device.failed_io_latency_observations();
          allocation_budget=n;auto r=read(limit);const auto remaining=allocation_budget;allocation_budget=-1;
          if(n<na){Check(remaining<0,"selected directory control allocation fault consumed");++consumed;}
          if(r.ok()){Check(r.retained_image_bytes==limit&&r.selection->checkpoint_sha256==admitted.selection->checkpoint_sha256&&r.checkpoint_inventory.inventory_pages.size()==2&&r.predecessor.inventory_pages.size()==2,"successful injected selected control read retains complete authority");
            if(remaining<0)Check(device.failed_io_latency_observations()+second_device.failed_io_latency_observations()==loss+1,"only recorded telemetry loss preserves selected control read success");}
          else{empty(r);Check(r.error==E::resource_exhausted,"selected control allocation resource classification");}
          if(n==na)Check(r.ok()&&remaining>=0,"selected control allocation sweep terminal uninjected success");
        }
        Check(consumed==na&&!stage_writes&&!stage_syncs,"complete selected control fault sweep is read-only");
        std::cout<<"directory controls allocations="<<na<<" consumed="<<consumed<<" reads="<<nr<<" digests="<<nf<<std::endl;
      }
      Check(device.Close().ok()&&second_device.Close().ok(),"release directory-selected control fixture for independent executable");
      const auto child=::fork();Check(child>=0,"fork directory selected control reader");
      if(child==0){const auto profile=std::to_string(p);::execl("/proc/self/exe","directory-controls-probe","--directory-controls-probe",fixture.root.c_str(),profile.c_str(),nullptr);::_exit(125);}
      int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable admits directory-selected secondary controls");
      continue;
    }
    if(directory_staging){
      auto secondary=map;secondary.header={sizes[q],3,Id(1),Id(7),Id(202),22,104,0,Profile(q)};
      secondary.object_uuid=Id(179);secondary.records.clear();secondary.states.assign(64,S::quarantined);
      secondary.map_generation=3;secondary.capacity_generation=2;
      for(unsigned n:{0u,21u,22u}){page::NativeAllocationRecord r;r.page_number=n;r.allocation_uuid=Id(220+n);
        r.creator_transaction_uuid=Id(98);r.creator_local_transaction_id=17;secondary.states[n]=S::allocated;
        if(n==0){r.page_uuid=second_zero.page_uuid;r.page_generation=second_zero.page_generation;r.page_type=2;r.owner_uuid=Id(7);}
        if(n==22){r.page_uuid=Id(202);r.page_generation=104;r.page_type=3;r.owner_uuid=Id(179);}
        if(n==21){r.page_uuid=Id(203);r.page_generation=7;r.page_type=6;r.owner_uuid=Id(101);r.creator_transaction_uuid=Id(162);r.creator_local_transaction_id=13;secondary.states[n]=S::reserved;}
        secondary.records.push_back(r);}
      const auto bind=[](const auto& m){return page::NativeFilespaceAllocationRoot{{m.header.filespace_uuid,m.header.page_number,m.header.page_generation,m.header.page_size_profile_uuid},m.object_uuid,WholeRootHash(AllocationOracle(m)),m.map_generation,m.capacity_generation};};
      directory.records[0].allocation_root=bind(map);directory.records[1].allocation_root=bind(secondary);
      const auto secondary_bytes=AllocationOracle(secondary);
      Check(second_device.WriteAt(22*sizes[q],secondary_bytes.data(),secondary_bytes.size()).ok()&&second_device.Sync().ok(),"persist directory-selected secondary map");
      // The bootstrap map remains well-formed but cannot reserve the destination.
      auto old=secondary;old.header.page_number=13;old.header.page_generation=103;old.map_generation=1;old.capacity_generation=1;
      old.states[22]=S::quarantined;old.states[13]=S::allocated;old.states[21]=S::allocated;
      old.records.back().page_number=13;old.records.back().page_generation=103;
      std::sort(old.records.begin(),old.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});
      const auto old_bytes=AllocationOracle(old);Check(second_device.WriteAt(13*sizes[q],old_bytes.data(),old_bytes.size()).ok()&&second_device.Sync().ok(),"persist stale bootstrap allocation map");
      persist();
      const auto baseline=directory;
      for(unsigned target=0;target<2;++target){auto leaf=LeafExample(target?q:p);if(target){leaf.header.filespace_uuid=Id(7);leaf.header.page_uuid=Id(203);}
        auto& dest=target?second_device:device;const auto size=target?sizes[q]:sizes[p];const Bytes blank(size);Bytes observed(size);
        const auto stage=[&](u64 allowance){return db::StageNativeCatalogLeafFromOpenDevices(devices,CheckpointRef(current),inv.inventory.entries[0].identity,leaf,allowance);};
        const auto no_write=[&](const auto& r){Check(!r.ok()&&!r.receipt&&!stage_writes&&!stage_syncs,"invalid selected allocation descriptor returns no receipt or writes");Check(dest.ReadAt(21*size,observed.data(),size).ok()&&observed==blank,"refused selected map leaves destination blank");};
        for(unsigned bad=0;bad<7;++bad){directory=baseline;auto& binding=*directory.records[target].allocation_root;
          if(bad==0)binding.sha256[0]^=1;if(bad==1)++binding.map_generation;if(bad==2)++binding.capacity_generation;
          if(bad==3)++binding.page.page_generation;if(bad==4)binding.object_uuid=Id(178);if(bad==5)binding.page.page_number=23;
          if(bad==6){directory.records[target].allocation_root.reset();if(!target)continue;}
          persist();stage_writes=stage_syncs=0;no_write(stage(64*u64{sizes[p]}));}
        directory=baseline;persist();stage_writes=stage_syncs=0;no_write(stage(1));
        const auto staged=stage(64*u64{sizes[p]});
        if(!staged.ok())std::cerr<<"directory stage target="<<target<<" profile="<<p<<" error="<<static_cast<int>(staged.error)<<" map="<<static_cast<int>(staged.allocation_error)<<'\n';
        Check(staged.ok()&&stage_writes==1&&stage_syncs==1,"actual staging uses exact selected directory allocation map");
        Check(dest.ReadAt(21*size,observed.data(),size).ok()&&observed==LeafOracle(leaf),"selected directory staging writes independent exact leaf image");
        stage_writes=stage_syncs=0;Check(stage(64*u64{sizes[p]}).ok()&&!stage_writes&&stage_syncs==1,"selected directory staging exact retry");
        if(p==0&&role==1){
          byte warm=0;for(unsigned n=0;n<4097;++n)Check(device.ReadAt(0,&warm,1).ok()&&second_device.ReadAt(0,&warm,1).ok(),"warm selected directory staging telemetry");
          reads=observed_allocations=observed_full_digests=0;track_reads=count_allocations=count_full_digests=true;
          const auto measured=stage(64*u64{sizes[p]});track_reads=count_allocations=count_full_digests=false;
          Check(measured.ok(),"measure selected directory retry fault positions");const auto nr=reads,nf=observed_full_digests;const auto na=observed_allocations;
          const auto refused=[&](const auto& r){Check(!r.ok()&&!r.receipt&&!stage_writes,"selected directory fault publishes no receipt and preserves staged bytes");};
          stage_writes=0;
          for(unsigned n=1;n<=nr;++n){reads=0;read_fault=n;track_reads=true;const auto r=stage(64*u64{sizes[p]});track_reads=false;Check(!read_fault,"selected directory read fault consumed");refused(r);}
          for(unsigned n=1;n<=nf;++n){full_digest_fault=n;const auto r=stage(64*u64{sizes[p]});Check(!full_digest_fault,"selected directory full hash fault consumed");refused(r);}
          for(unsigned n=1;n<=5;++n){hash_fault=n;const auto r=stage(64*u64{sizes[p]});Check(!hash_fault,"selected directory multipart hash fault consumed");refused(r);}
          unsigned long consumed=0;
          for(unsigned long n=0;n<=na;++n){const auto loss=device.failed_io_latency_observations()+second_device.failed_io_latency_observations();
            allocation_budget=n;const auto r=stage(64*u64{sizes[p]});const auto remaining=allocation_budget;allocation_budget=-1;
            if(n<na){Check(remaining<0,"selected directory allocation fault consumed");++consumed;}
            if(r.ok()){if(remaining<0)Check(device.failed_io_latency_observations()+second_device.failed_io_latency_observations()==loss+1,"only recorded optional telemetry failure can preserve directory staging success");}
            else{refused(r);using L=db::NativeCatalogLeafStageError;Check(r.error==L::resource_exhausted||r.checkpoint_error==db::NativeCheckpointError::resource_exhausted||r.directory_error==page::NativeDirectoryError::resource_exhausted||r.allocation_error==page::NativeAllocationError::resource_exhausted||r.leaf_error==db::NativeCatalogLeafError::resource_exhausted,"selected directory allocation failure retains resource classification");}
            if(n==na)Check(r.ok()&&remaining>=0,"selected directory allocation sweep terminal uninjected success");
          }
          Check(consumed==na&&dest.ReadAt(21*size,observed.data(),size).ok()&&observed==LeafOracle(leaf),"complete selected directory fault sweep preserves exact staged image");
          std::cout<<"directory stage target="<<target<<" allocations="<<na<<" consumed="<<consumed<<" reads="<<nr<<" digests="<<nf<<std::endl;
        }
      }
      Check(device.Close().ok()&&second_device.Close().ok(),"release selected directory staging fixture for independent process");
      const auto child=::fork();Check(child>=0,"fork selected directory staging reader");
      if(child==0){const auto profile=std::to_string(p);::execl("/proc/self/exe","directory-stage-probe","--directory-stage-probe",fixture.root.c_str(),profile.c_str(),nullptr);::_exit(125);}
      int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable follows selected directory roots and verifies staged images");
      continue;
    }
    if(history_only){
      system.checkpoint_generation=1;system.checkpoint=selection.previous_checkpoint;
      page::NativeHorizonRecord observation;observation.local_boundary=13;observation.owner_uuid=Id(172);
      observation.checkpoint_object_uuid=initial.object_uuid;observation.checkpoint_generation=1;
      observation.checkpoint=selection.previous_checkpoint;observation.horizon_uuid=Id(173);observation.timeline_uuid=Id(97);
      horizon.total_records=1;horizon.records.push_back(observation);persist();
      const auto system_read=[&](u64 limit){return db::VerifyCurrentNativeCheckpointSystemStateFromOpenDevices(Id(1),devices,CheckpointRef(current),limit);};
      const auto horizon_read=[&](u64 limit){return db::VerifyCurrentNativeCheckpointHorizonFromOpenDevices(Id(1),devices,CheckpointRef(current),limit);};
      // Selected admission 7P, one older checkpoint/inventory 2P, then
      // system state P or horizon and retention 2P. No selected proof refund.
      const u64 system_limit=10*u64{sizes[p]},horizon_limit=11*u64{sizes[p]};
      const auto sr=system_read(system_limit);const auto hr=horizon_read(horizon_limit);
      if(!sr.ok()||sr.retained_image_bytes!=system_limit||!hr.ok()||hr.retained_image_bytes!=horizon_limit)
        std::cerr<<"selected history system="<<static_cast<int>(sr.error)<<" bytes="<<sr.retained_image_bytes
          <<" horizon="<<static_cast<int>(hr.error)<<" bytes="<<hr.retained_image_bytes<<std::endl;
      Check(sr.ok()&&sr.retained_image_bytes==system_limit&&sr.checkpoints.checkpoints.size()==2&&
        sr.checkpoints.retained_image_bytes==9*u64{sizes[p]}&&sr.checkpoints.checkpoints.front().retained_image_bytes==7*u64{sizes[p]}&&
        sr.checkpoints.checkpoints.back().checkpoint->header.page_number==19,"system history retains actual selected-root authority work");
      Check(hr.ok()&&hr.retained_image_bytes==horizon_limit&&hr.checkpoints.checkpoints.size()==2&&
        hr.checkpoints.retained_image_bytes==9*u64{sizes[p]}&&hr.checkpoints.checkpoints.front().retained_image_bytes==7*u64{sizes[p]}&&
        hr.horizons.pages.front().root->records.front().checkpoint==selection.previous_checkpoint,"horizon history retains actual selected-root authority work");
      const auto empty_system=[&](const auto& r){Check(!r.ok()&&!r.retained_image_bytes&&r.checkpoints.checkpoints.empty()&&
        !r.checkpoints.retained_image_bytes&&!r.system_state.state&&r.system_state.bytes.empty(),"selected system failure exposes no prefix");};
      const auto empty_horizon=[&](const auto& r){Check(!r.ok()&&!r.retained_image_bytes&&r.checkpoints.checkpoints.empty()&&
        !r.checkpoints.retained_image_bytes&&r.horizons.pages.empty()&&r.retention.images.empty(),"selected horizon failure exposes no prefix");};
      auto short_system=system_read(system_limit-1);empty_system(short_system);
      Check(short_system.error==db::NativeCheckpointError::resource_exhausted||
        (short_system.error==db::NativeCheckpointError::inventory_failure&&short_system.checkpoints.inventory_error==page::NativeInventoryError::resource_exhausted),"selected system exact allowance enforced");
      auto short_horizon=horizon_read(horizon_limit-1);empty_horizon(short_horizon);
      Check(short_horizon.error==db::NativeCheckpointError::resource_exhausted||
        (short_horizon.error==db::NativeCheckpointError::retention_failure&&short_horizon.retention_error==page::NativeRetentionError::resource_exhausted),"selected horizon exact allowance enforced");
      if(p==0&&role==1){
        Bytes before(64*sizes[p]),after(before.size());
        Check(device.ReadAt(0,before.data(),before.size()).ok(),"snapshot complete selected history fixture");
        byte warm=0;for(unsigned n=0;n<4097;++n){Check(device.ReadAt(0,&warm,1).ok()&&second_device.ReadAt(0,&warm,1).ok(),"warm bounded optional read telemetry before measuring fault positions");}
        const auto faults=[&](const auto& reader,const auto& no_prefix,u64 limit){
          reads=observed_full_digests=observed_allocations=0;track_reads=count_full_digests=count_allocations=true;
          const auto baseline=reader(limit);track_reads=count_full_digests=count_allocations=false;
          Check(baseline.ok(),"selected history fault baseline succeeds");
          const auto nr=reads,nf=observed_full_digests;const auto na=observed_allocations;
          for(unsigned n=1;n<=nr;++n){reads=0;read_fault=n;track_reads=true;const auto value=reader(limit);track_reads=false;
            Check(!read_fault,"selected history read failure consumed");no_prefix(value);}
          for(unsigned n=1;n<=nf;++n){full_digest_fault=n;const auto value=reader(limit);
            Check(!full_digest_fault,"selected history full digest failure consumed");no_prefix(value);}
          for(unsigned mode=1;mode<=5;++mode){hash_fault=mode;const auto value=reader(limit);
            Check(!hash_fault,"selected history multipart failure consumed");no_prefix(value);}
          unsigned long consumed=0,positions=0;
          for(unsigned long n=history_shard;n<=na;n+=history_stride){++positions;const auto loss=device.failed_io_latency_observations()+second_device.failed_io_latency_observations();
            allocation_budget=n;const auto value=reader(limit);const auto remaining=allocation_budget;allocation_budget=-1;
            if(remaining<0)++consumed;
            if(value.ok()){Check(value.retained_image_bytes==limit,"selected history allocation success is complete");
              if(remaining<0)Check(device.failed_io_latency_observations()+second_device.failed_io_latency_observations()==loss+1,"only recorded telemetry loss permits consumed allocation success");}
            else{Check(remaining<0,"selected history allocation failure consumed");no_prefix(value);}
            if(n==na)Check(value.ok(),"selected history allocation terminal success");}
          Check(reader(limit).ok(),"selected history succeeds after every fault shard");
          std::cout<<"selected history budget="<<limit<<" allocations="<<na<<" reads="<<nr<<" digests="<<nf
            <<" shard="<<history_shard<<" stride="<<history_stride<<" positions="<<positions<<" consumed="<<consumed<<std::endl;
        };
        stage_writes=stage_syncs=0;faults(system_read,empty_system,system_limit);faults(horizon_read,empty_horizon,horizon_limit);
        Check(!stage_writes&&!stage_syncs&&device.ReadAt(0,after.data(),after.size()).ok()&&after==before,"selected history failures preserve every primary byte without writes or barriers");
      }
      stage_writes=stage_syncs=0;
      system.checkpoint->page_number=18;persist();stage_writes=stage_syncs=0;empty_system(system_read(system_limit));
      Check(!stage_writes&&!stage_syncs,"unreachable system observation does not mutate files");
      system.checkpoint=selection.previous_checkpoint;horizon.records.front().checkpoint->page_number=18;persist();stage_writes=stage_syncs=0;
      empty_horizon(horizon_read(horizon_limit));Check(!stage_writes&&!stage_syncs,"unreachable horizon observation does not mutate files");
      continue;
    }
    if(inventory_staging){
      auto head=inv,tail=inv;head.header.page_number=23;head.header.page_generation=107;head.header.page_uuid=Id(204);head.inventory_generation=20;
      tail.header.page_number=24;tail.header.page_generation=108;tail.header.page_uuid=Id(205);tail.inventory_generation=20;
      page::NativeAllocationMap secondary_map;
      if(mixed_inventory){
        second_zero=Example(q,2);second_zero.bootstrap.filespace_uuid=Id(7);second_zero.page_uuid=Id(8);second_zero.free_pages=second_zero.preallocated_pages=0;
        for(auto& r:second_zero.roots)r.filespace_uuid=Id(7);second_zero.roots[2].object_uuid=Id(179);directory.records[1].bootstrap=second_zero.bootstrap;
        tail.header.filespace_uuid=Id(7);tail.header.page_size_bytes=sizes[q];tail.header.page_size_profile_uuid=Profile(q);
        secondary_map.header={sizes[q],3,Id(1),Id(7),Id(202),13,103,0,Profile(q)};secondary_map.object_uuid=Id(179);
        secondary_map.map_generation=secondary_map.capacity_generation=1;secondary_map.total_pages=64;
        secondary_map.creator_transaction_uuid=Id(98);secondary_map.creator_local_transaction_id=17;secondary_map.states.assign(64,S::quarantined);
        secondary_map.states[0]=secondary_map.states[13]=S::allocated;
        secondary_map.records.push_back({0,Id(220),Id(8),Id(7),Id(98),17,7,0,1,{}});
        secondary_map.records.push_back({13,Id(233),Id(202),Id(179),Id(98),17,103,0,3,{}});
      }
      head.inventory.entries.pop_back();tail.inventory.entries.erase(tail.inventory.entries.begin());head.next=InventoryRef(tail);tail.previous=InventoryRef(head);
      std::vector<page::NativeTransactionInventoryPage> chain{head,tail};
      for(const auto& page:chain){const auto& h=page.header;auto& owning_map=h.filespace_uuid==Id(2)?map:secondary_map;owning_map.states[h.page_number]=S::reserved;
        owning_map.records.push_back({h.page_number,Id(120+h.page_number),h.page_uuid,page.object_uuid,Id(162),13,h.page_generation,0,0x301,{}});}
      std::sort(map.records.begin(),map.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});
      const auto persist_stage=[&](){persist();if(mixed_inventory){const auto mb=AllocationOracle(secondary_map);Check(second_device.WriteAt(13*sizes[q],mb.data(),mb.size()).ok()&&second_device.Sync().ok(),"persist mixed inventory reservation map");}};persist_stage();
      // Selected successor admission retains 7P: two selector images, both
      // checkpoint/inventory pairs and the selected allocation map. Add the
      // directory P, destination map, both candidate images and one scratch
      // page. Mixed filespaces require the maximum of the two destination passes.
      const u64 old_stage_budget=mixed_inventory?std::max(8*u64{sizes[p]}+sizes[q],4*u64{sizes[p]}+3*u64{sizes[q]}):9*u64{sizes[p]};
      const u64 stage_budget=mixed_inventory?std::max(11*u64{sizes[p]}+sizes[q],9*u64{sizes[p]}+3*u64{sizes[q]}):12*u64{sizes[p]};const Bytes blank(sizes[p],0),tail_blank(tail.header.page_size_bytes,0);const std::vector<Bytes> expected{InventoryOracle(head,13,13,13),InventoryOracle(tail,18,18,18)};
      const auto put_tail=[&](const Bytes& b){auto& target=mixed_inventory?second_device:device;Check(target.WriteAt(24*u64{tail.header.page_size_bytes},b.data(),b.size()).ok()&&target.Sync().ok(),"persist inventory destination preimage");};
      const auto stage=[&](u64 limit){return db::StageNativeInventorySuccessorFromOpenDevices(devices,CheckpointRef(current),inv.inventory.entries.front().identity,chain,limit);};
      const auto bytes=[&](unsigned slot){const bool second=mixed_inventory&&slot==24;const auto size=second?sizes[q]:sizes[p];auto& target=second?second_device:device;Bytes b(size);const auto io=target.ReadAt(slot*u64{size},b.data(),b.size());Check(io.ok()&&io.bytes_transferred==b.size(),"read complete staged inventory bytes");return b;};
      // Inspect the actual destination on every reset. Most injected failures
      // occur before mutation, so rewriting and synchronizing an already zero
      // page adds no recovery coverage. Changed bytes still get a durable reset.
      const auto reset=[&](){if(bytes(23)!=blank)put(23,blank);if(bytes(24)!=tail_blank)put_tail(tail_blank);};
      const auto no_receipts=[&](const auto& r){Check(!r.ok()&&r.receipts.empty(),"inventory staging failure has no successful prefix");};
      const auto resource_failure=[](const auto& r){return r.error==db::NativeInventoryStageError::resource_exhausted||r.checkpoint_error==db::NativeCheckpointError::resource_exhausted||r.directory_error==page::NativeDirectoryError::resource_exhausted||r.allocation_error==page::NativeAllocationError::resource_exhausted;};
      auto dirty_head=blank,dirty_tail=tail_blank;dirty_head.front()=0xa5;dirty_tail.back()=0x5a;put(23,dirty_head);put_tail(dirty_tail);
      stage_writes=stage_syncs=0;reset();Check(stage_writes==2&&stage_syncs==2&&bytes(23)==blank&&bytes(24)==tail_blank,"fixture reset durably restores both actual dirty destinations");
      stage_writes=stage_syncs=0;reset();Check(!stage_writes&&!stage_syncs,"already blank actual destinations need no fixture rewrite");
      reset();const auto selected_directory=db::VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(Id(1),devices,CheckpointRef(current),stage_budget);
      Check(selected_directory.ok()&&selected_directory.retained_image_bytes==8*u64{sizes[p]},"inventory staging retains selected successor and predecessor proof plus directory");
      stage_writes=stage_syncs=0;const auto insufficient=stage(old_stage_budget);no_receipts(insufficient);
      Check(resource_failure(insufficient)&&!stage_writes&&!stage_syncs&&bytes(23)==blank&&bytes(24)==tail_blank,"old inventory allowance refuses without a staged prefix or barriers");
      const auto cold=stage(stage_budget);
      if(!cold.ok())std::cerr<<"cold inventory stage error="<<int(cold.error)<<" cp="<<int(cold.checkpoint_error)<<" map="<<int(cold.allocation_error)<<" inv="<<int(cold.inventory_error)<<'\n';
      Check(cold.ok(),"cold inventory stage initializes transition consistency");
      if(p==0&&role==1&&!mixed_inventory){byte warm=0;for(unsigned n=0;n<4097;++n){
        const auto a=device.ReadAt(0,&warm,1),b=second_device.ReadAt(0,&warm,1);
        Check(a.ok()&&a.bytes_transferred==1&&b.ok()&&b.bytes_transferred==1,"warm bounded inventory telemetry before fault-site measurement");}}
      reset();reads=observed_allocations=observed_full_digests=0;stage_writes=stage_syncs=0;track_reads=count_allocations=count_full_digests=true;
      auto staged=stage(stage_budget);track_reads=count_allocations=count_full_digests=false;
      const auto nr=reads,nf=observed_full_digests;const auto na=observed_allocations;
      if(!staged.ok())std::cerr<<"inventory stage error="<<static_cast<int>(staged.error)<<" cp="<<static_cast<int>(staged.checkpoint_error)<<" map="<<static_cast<int>(staged.allocation_error)<<" inv="<<static_cast<int>(staged.inventory_error)<<std::endl;
      Check(staged.ok()&&staged.receipts.size()==2&&stage_writes==2&&stage_syncs==2,"complete inventory chain physically staged");
      for(unsigned i=0;i<2;++i){const auto& receipt=staged.receipts[i];Check(bytes(23+i)==expected[i]&&receipt.page==InventoryRef(chain[i])&&receipt.page_uuid==chain[i].header.page_uuid&&receipt.inventory_uuid==Id(44)&&receipt.allocation_uuid==Id(143+i)&&receipt.transaction.transaction_uuid.value==Id(162)&&receipt.sha256==WholeRootHash(expected[i]),"ordered receipts match independently encoded actual inventory pages");}
      stage_writes=stage_syncs=0;Check(stage(stage_budget).ok()&&stage_writes==0&&stage_syncs==2,"idempotent complete inventory retry syncs both pages");
      Check(read(budget).checkpoint_inventory.inventory_generation==19&&bytes(14)==InventoryOracle(inv,13,13,13),"staging does not publish inventory or change old bytes");
      reset();stage_writes=stage_syncs=0;const auto short_budget=stage(stage_budget-1);no_receipts(short_budget);Check(resource_failure(short_budget)&&!stage_writes&&!stage_syncs&&bytes(23)==blank&&bytes(24)==tail_blank,"one-byte-short complete inventory preflight budget refuses before writes");
      const auto original=chain;
      for(unsigned fault=0;fault<12;++fault){chain=original;
        if(fault==0)chain.back().previous.reset();if(fault==1)chain.back().inventory_generation++;if(fault==2)chain.back().object_uuid=Id(206);
        if(fault==3)chain.back().header.page_uuid=chain.front().header.page_uuid;if(fault==4)chain.back().inventory.entries.front().identity.transaction_uuid.value=chain.front().inventory.entries.front().identity.transaction_uuid.value;
        if(fault==5)for(auto& image:chain)image.inventory_generation=19;
        if(fault==6)chain.back().inventory.next_commit_sequence++;
        if(fault==7)chain.back().header.flags=1;if(fault==8)chain.back().header.page_generation++;
        if(fault==9)chain.front().inventory.entries.front().identity.transaction_uuid.value=Id(206);
        if(fault==10)chain.front().inventory.entries.front().begin_unix_epoch_millis++;
        if(fault==11){chain.back().inventory.entries.front().state=mga::TransactionState::active;chain.back().inventory.entries.front().commit_sequence=0;}
        stage_writes=0;no_receipts(stage(stage_budget));Check(!stage_writes&&bytes(23)==blank&&bytes(24)==tail_blank,"late invalid inventory member causes no prefix write");}
      chain=original;auto occupied=tail_blank;occupied[0]=1;put_tail(occupied);stage_writes=0;no_receipts(stage(stage_budget));Check(!stage_writes&&bytes(23)==blank&&bytes(24)==occupied,"nonzero late destination rejected before first write");reset();
      auto& reservation_map=mixed_inventory?secondary_map:map;const auto original_map=reservation_map;
      for(unsigned fault=0;fault<6;++fault){reservation_map=original_map;auto& record=*std::find_if(reservation_map.records.begin(),reservation_map.records.end(),[](const auto& r){return r.page_number==24;});
        if(fault==0)reservation_map.states[24]=S::allocated;if(fault==1)record.owner_uuid=Id(206);if(fault==2){record.creator_transaction_uuid=Id(98);record.creator_local_transaction_id=17;}
        if(fault==3)record.page_uuid=Id(206);if(fault==4)record.page_generation++;if(fault==5)record.page_type=6;
        persist_stage();stage_writes=0;no_receipts(stage(stage_budget));Check(!stage_writes&&bytes(23)==blank&&bytes(24)==tail_blank,"late actual reservation mismatch refuses before first write");}
      reservation_map=original_map;inv.inventory.entries.front().rollback_only=true;persist_stage();stage_writes=0;no_receipts(stage(stage_budget));Check(!stage_writes,"rollback-only inventory owner cannot stage");
      inv.inventory.entries.front().rollback_only=false;persist_stage();
      if(p==0&&role==1&&!mixed_inventory){
        if(inventory_allocation_shard>=0){unsigned long positions=0,consumed=0;
          for(unsigned long fault=static_cast<unsigned>(inventory_allocation_shard);fault<=na;fault+=16){
            reset();const auto loss=device.failed_io_latency_observations()+second_device.failed_io_latency_observations();++positions;
            allocation_budget=fault;staged=stage(stage_budget);const auto remaining=allocation_budget;allocation_budget=-1;
            if(remaining<0)++consumed;
            if(staged.ok()){Check(staged.receipts.size()==2&&bytes(23)==expected[0]&&bytes(24)==expected[1],"inventory success wrote exact complete chain");
              if(remaining<0)Check(device.failed_io_latency_observations()+second_device.failed_io_latency_observations()==loss+1,"only isolated recorded telemetry loss permits consumed allocation fault");}
            else{Check(remaining<0,"inventory allocation injection actually consumed");no_receipts(staged);}
            if(fault<na)Check(remaining<0,"every measured inventory allocation position reached");
            else Check(staged.ok()&&remaining>=0,"inventory allocation terminal position succeeds without injection");
          }
          reset();Check(stage(stage_budget).ok()&&bytes(23)==expected[0]&&bytes(24)==expected[1],"inventory staging succeeds after every allocation shard");
          std::cout<<"inventory stage allocations="<<na<<" shard="<<inventory_allocation_shard<<" stride=16 positions="<<positions<<" consumed="<<consumed<<std::endl;
        }
        for(unsigned fault=1;fault<=nr;++fault){reset();reads=0;read_fault=fault;track_reads=true;staged=stage(stage_budget);track_reads=false;Check(!read_fault,"inventory read fault consumed");no_receipts(staged);}
        for(unsigned fault=1;fault<=nf;++fault){reset();full_digest_fault=fault;staged=stage(stage_budget);Check(!full_digest_fault,"inventory full hash fault consumed");no_receipts(staged);}
        for(unsigned mode=1;mode<=5;++mode){reset();hash_fault=mode;no_receipts(stage(stage_budget));Check(!hash_fault,"inventory multipart hash fault consumed");}
        reset();stage_write_fault=3;no_receipts(stage(stage_budget));Check(!stage_write_fault,"inventory partial write fault consumed");const auto partial=bytes(23);no_receipts(stage(stage_budget));Check(partial!=blank&&partial!=expected[0]&&bytes(23)==partial&&bytes(24)==blank,"partial inventory image preserved without prefix receipt");
        reset();stage_sync_fault=1;no_receipts(stage(stage_budget));Check(!stage_sync_fault&&stage(stage_budget).ok(),"inventory sync failure can retry exact bytes");
        reset();reads=0;track_reads=true;stage_corrupt_read=nr;no_receipts(stage(stage_budget));track_reads=false;Check(!stage_corrupt_read&&bytes(23)==expected[0]&&bytes(24)==expected[1],"late readback corruption returns no receipt despite completed writes");
        reset();stage_write_fault=3;stage_write_fault_after=2;no_receipts(stage(stage_budget));
        Check(!stage_write_fault&&!stage_write_fault_after&&bytes(23)==expected[0]&&bytes(24)!=blank&&bytes(24)!=expected[1],"late inventory write failure leaves no successful-prefix receipt");
        const auto late_partial=bytes(24);no_receipts(stage(stage_budget));Check(bytes(23)==expected[0]&&bytes(24)==late_partial,"late partial destination preserved on retry");
        reset();stage_sync_fault=1;stage_sync_fault_after=2;no_receipts(stage(stage_budget));
        Check(!stage_sync_fault&&!stage_sync_fault_after&&bytes(23)==expected[0]&&bytes(24)==expected[1]&&stage(stage_budget).ok(),"late sync failure requires complete-chain retry");
        reset();stage_writes=stage_syncs=0;auto reversed=devices;std::reverse(reversed.begin(),reversed.end());std::atomic<unsigned> completed{0};
        std::thread one([&]{if(stage(stage_budget).ok())++completed;}),two([&]{if(db::StageNativeInventorySuccessorFromOpenDevices(reversed,CheckpointRef(current),inv.inventory.entries.front().identity,chain,stage_budget).ok())++completed;});
        one.join();two.join();Check(completed==2&&stage_writes==2&&stage_syncs==4,"opposite-order inventory batches serialize writes and exact retry");
        std::cout<<"inventory stage measured_allocations="<<na<<" reads="<<nr<<" digests="<<nf<<" allocation_shard="<<inventory_allocation_shard<<std::endl;
      }
      if(mixed_inventory){
        reset();stage_write_fault=3;stage_write_fault_after=2;no_receipts(stage(stage_budget));Check(!stage_write_fault&&!stage_write_fault_after&&bytes(23)==expected[0]&&bytes(24)!=tail_blank&&bytes(24)!=expected[1],"mixed-filespace late partial write returns no prefix receipt");
        const auto partial=bytes(24);no_receipts(stage(stage_budget));Check(bytes(24)==partial,"mixed partial image preserved");
        reset();stage_sync_fault=1;stage_sync_fault_after=2;no_receipts(stage(stage_budget));Check(!stage_sync_fault&&!stage_sync_fault_after&&stage(stage_budget).ok(),"mixed late sync failure permits exact retry");
        reset();reads=0;track_reads=true;stage_corrupt_read=nr;no_receipts(stage(stage_budget));track_reads=false;Check(!stage_corrupt_read&&bytes(23)==expected[0]&&bytes(24)==expected[1],"mixed final readback corruption yields no receipt");
      }
      reset();Check(stage(stage_budget).ok(),"final inventory stage");Check(device.Close().ok()&&second_device.Close().ok(),"release inventory staging fixture");
      const auto child=::fork();Check(child>=0,"fork inventory staging probe");if(child==0){const auto profile=std::to_string(p);::execl("/proc/self/exe","inventory-stage-probe","--inventory-stage-probe",fixture.root.c_str(),profile.c_str(),nullptr);::_exit(125);}
      int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable verifies actual staged inventory chain");continue;
    }
    persist();reads=observed_full_digests=observed_allocations=0;track_reads=count_full_digests=count_allocations=true;auto result=read(budget);track_reads=count_full_digests=count_allocations=false;
    const auto nr=reads,nf=observed_full_digests;const auto na=observed_allocations;
    if(!result.ok())std::cerr<<"bound selector error="<<static_cast<int>(result.error)<<" cp="<<static_cast<int>(result.checkpoint_error)<<" map="<<static_cast<int>(result.allocation_error)<<std::endl;
    Check(result.ok()&&result.selection->selection_generation==2&&result.checkpoint_inventory.checkpoint->header.page_number==36&&result.predecessor.checkpoint->header.page_number==19&&result.allocation.state_counts[0]==1&&result.retained_image_bytes==7*sizes[p],"actual bound selector and newer allocation counts");
    consumers(CheckpointRef(current),true);consumers(CheckpointRef(initial),false);empty(read(7*sizes[p]-1));
    Check(!page::ReadNativeAllocationChainFromOpenDevice(device,{Id(1),Id(2),Profile(p)},budget).ok(),"new map was not read from the stale bootstrap allocation address");
    auto leaf=LeafExample(p);stage_writes=stage_syncs=0;
    const auto insufficient_leaf=db::StageNativeCatalogLeafFromOpenDevices(devices,CheckpointRef(current),inv.inventory.entries[0].identity,leaf,budget);
    Check(!insufficient_leaf.ok()&&!insufficient_leaf.receipt&&!stage_writes&&!stage_syncs,"selected leaf cannot use an allowance omitting predecessor and selector work");
    const auto staged=db::StageNativeCatalogLeafFromOpenDevices(devices,CheckpointRef(current),inv.inventory.entries[0].identity,leaf,11*u64{sizes[p]});
    Check(staged.ok(),"actual leaf staging follows selected map");Bytes actual(sizes[p]);Check(device.ReadAt(21*sizes[p],actual.data(),actual.size()).ok()&&actual==LeafOracle(leaf),"selected-map staging persists exact leaf");
    if(p==0&&role==1){
      const auto write_count=stage_writes;
      for(unsigned long n=0;n<=na;++n){allocation_budget=n;result=read(budget);allocation_budget=-1;if(!result.ok())empty(result);if(n==na)Check(result.ok(),"bound selection allocation sweep terminal success");}
      for(unsigned n=1;n<=nr;++n){reads=0;read_fault=n;track_reads=true;result=read(budget);track_reads=false;Check(!read_fault,"bound selection read fault consumed");empty(result);}
      for(unsigned n=1;n<=nf;++n){full_digest_fault=n;result=read(budget);Check(!full_digest_fault,"bound selection full digest fault consumed");empty(result);}
      for(unsigned mode=1;mode<=5;++mode){hash_fault=mode;result=read(budget);Check(!hash_fault,"bound selection multipart fault consumed");empty(result);}
      std::cout<<"bound selector allocations="<<na<<" reads="<<nr<<" digests="<<nf<<std::endl;
      Check(stage_writes==write_count,"selection reads and their faults never write node pages");
      const auto initial_map_ref=zero.roots[2];zero.roots[2].page_number=35;zero.roots[2].page_generation=104;persist();
      Check(page::ReadNativeAllocationChainFromOpenDevice(device,{Id(1),Id(2),Profile(p)},budget).error==page::NativeAllocationError::counter_mismatch,"bootstrap-root read still enforces initial counter agreement");
      Check(read(budget).ok(),"selected map counts do not inherit initial bootstrap counters");zero.roots[2]=initial_map_ref;persist();
      second_zero.bootstrap.flags|=disk::FilespaceBootstrapFlag::payload_encrypted;second_zero.bootstrap.encryption_profile_uuid=Id(4);directory.records[1].bootstrap=second_zero.bootstrap;persist();
      auto encrypted_leaf=LeafExample(q);encrypted_leaf.header.filespace_uuid=Id(7);encrypted_leaf.header.page_uuid=Id(203);
      page::NativeAllocationMap secondary_map;secondary_map.header={sizes[q],3,Id(1),Id(7),Id(202),13,103,0,Profile(q)};
      secondary_map.object_uuid=Id(179);secondary_map.map_generation=secondary_map.capacity_generation=1;secondary_map.total_pages=64;
      secondary_map.creator_transaction_uuid=Id(98);secondary_map.creator_local_transaction_id=17;secondary_map.states.assign(64,S::quarantined);
      for(unsigned n:{0u,13u,21u}){page::NativeAllocationRecord r;r.page_number=n;r.allocation_uuid=Id(220+n);r.creator_transaction_uuid=Id(98);r.creator_local_transaction_id=17;secondary_map.states[n]=S::allocated;
        if(n==0){r.page_uuid=second_zero.page_uuid;r.page_generation=second_zero.page_generation;r.page_type=2;r.owner_uuid=Id(7);}
        if(n==13){r.page_uuid=Id(202);r.page_generation=103;r.page_type=3;r.owner_uuid=Id(179);}
        if(n==21){r.page_uuid=Id(203);r.page_generation=7;r.page_type=6;r.owner_uuid=Id(101);r.creator_transaction_uuid=Id(162);r.creator_local_transaction_id=13;secondary_map.states[n]=S::reserved;}
        secondary_map.records.push_back(r);}
      const auto secondary_map_bytes=AllocationOracle(secondary_map);
      Check(second_device.WriteAt(13*sizes[q],secondary_map_bytes.data(),secondary_map_bytes.size()).ok()&&second_device.Sync().ok(),"persist valid encrypted-destination reservation metadata");
      const auto encrypted=db::StageNativeCatalogLeafFromOpenDevices(devices,CheckpointRef(current),inv.inventory.entries[0].identity,encrypted_leaf,16*sizes[p]);
      if(encrypted.error!=db::NativeCatalogLeafStageError::header_requires_authority)std::cerr<<"encrypted stage error="<<static_cast<int>(encrypted.error)<<" cp="<<static_cast<int>(encrypted.checkpoint_error)<<" dir="<<static_cast<int>(encrypted.directory_error)<<std::endl;
      Check(!encrypted.ok()&&!encrypted.receipt&&encrypted.error==db::NativeCatalogLeafStageError::header_requires_authority,"actual encrypted filespace cannot be bypassed by clear leaf flags");
      Bytes untouched(sizes[q]);Check(second_device.ReadAt(21*sizes[q],untouched.data(),untouched.size()).ok()&&std::all_of(untouched.begin(),untouched.end(),[](byte b){return b==0;}),"encrypted destination remains unwritten without owning crypto path");
      second_zero.bootstrap.flags=0;second_zero.bootstrap.encryption_profile_uuid={};directory.records[1].bootstrap=second_zero.bootstrap;persist();
      const auto clear_stage=db::StageNativeCatalogLeafFromOpenDevices(devices,CheckpointRef(current),inv.inventory.entries[0].identity,encrypted_leaf,16*sizes[p]);
      if(!clear_stage.ok())std::cerr<<"clear secondary stage error="<<static_cast<int>(clear_stage.error)<<" map="<<static_cast<int>(clear_stage.allocation_error)<<" leaf="<<static_cast<int>(clear_stage.leaf_error)<<std::endl;
      Check(clear_stage.ok(),"same valid reservation stages after removing destination encryption requirement");
      Check(second_device.ReadAt(21*sizes[q],untouched.data(),untouched.size()).ok()&&untouched==LeafOracle(encrypted_leaf),"unencrypted secondary persists exact native leaf");
      auto reverse=devices;std::reverse(reverse.begin(),reverse.end());std::atomic<unsigned> completed{0};
      std::thread a([&]{for(unsigned i=0;i<4;++i)if(read(budget).ok())++completed;}),b([&]{for(unsigned i=0;i<4;++i)if(db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),reverse,Id(2),budget).ok())++completed;});
      a.join();b.join();Check(completed==8,"actual opposite-order callers share binary-ordered guards");
    }
    if(role==1){
      // Independently framed, separately allocated old/current inventories.
      // Reseal the complete graph: checksum or page-structure rejection cannot
      // stand in for validating the relationship between admitted snapshots.
      const auto saved_inv=inv;const auto saved_map=map;const auto saved_initial=initial,saved_current=current;
      inv.inventory.entries.front().evidence_record_written=true;
      inv.inventory.entries.back().commit_sequence=2;inv.inventory.next_commit_sequence=3;
      auto terminal=inv.inventory.entries.front();terminal.identity.local_id=mga::MakeLocalTransactionId(15);
      terminal.identity.transaction_uuid.value=Id(206);terminal.state=mga::TransactionState::rolled_back;
      terminal.final_unix_epoch_millis=200;inv.inventory.entries.insert(inv.inventory.entries.begin()+1,terminal);
      const auto base=inv;
      preceding_inventory=base;preceding_inventory->header.page_number=22;preceding_inventory->header.page_uuid=Id(207);
      map.states[22]=S::allocated;map.records.push_back({22,Id(208),Id(207),inv.object_uuid,Id(98),17,inv.header.page_generation,0,0x301,{}});
      std::sort(map.records.begin(),map.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});
      const auto old_base=*preceding_inventory;
      const auto reset_history=[&](){inv=base;preceding_inventory=old_base;inventory_summary=preceding_summary={13,13,13};};
      const auto assert_history=[&](bool valid,unsigned history_case=99){persist();
        Check(db::VerifyNativeCheckpointInventoryFromOpenDevices(Id(1),devices,CheckpointRef(initial),budget).ok(),"predecessor independently admits");
        const auto candidate=db::VerifyNativeCheckpointInventoryFromOpenDevices(Id(1),devices,CheckpointRef(current),budget);
        if(!candidate.ok())std::cerr<<"history fixture valid="<<valid<<" case="<<history_case<<" profile="<<p<<" error="<<static_cast<unsigned>(candidate.error)<<" inventory="<<static_cast<unsigned>(candidate.inventory_error)<<'\n';
        Check(candidate.ok(),"successor independently admits");
        const auto writes=stage_writes;const auto observed=read(budget);
        Check(stage_writes==writes,"inventory evolution admission is read-only");
        if(valid)Check(observed.ok(),"legal selected inventory evolution");
        else {empty(observed);Check(observed.error==E::checkpoint_binding_mismatch,"resealed conflicting inventory history refused at selection boundary");consumers(CheckpointRef(current),false);}
      };
      assert_history(true);
      for(unsigned fault=0;fault<23;++fault){reset_history();auto& active=inv.inventory.entries[0];auto& ended=inv.inventory.entries[1];auto& committed_entry=inv.inventory.entries[2];
        if(fault==0)active.begin_unix_epoch_millis++;
        if(fault==1)active.begin_visible_through_local_transaction_id++;
        if(fault==2)active.begin_visible_through_commit_sequence=1;
        if(fault==3)active.evidence_record_required=false;
        if(fault==4)active.evidence_record_written=false;
        if(fault==5)ended.identity.transaction_uuid.value=Id(209);
        if(fault==6)ended.identity.scope=mga::TransactionScope::cluster_global;
        if(fault==7)committed_entry.final_unix_epoch_millis++;
        if(fault==8){committed_entry.commit_sequence=3;inv.inventory.next_commit_sequence=4;}
        if(fault==9)ended.state=mga::TransactionState::failed_terminal;
        if(fault==10){ended.state=mga::TransactionState::active;ended.final_unix_epoch_millis=0;}
        if(fault==11)ended.final_unix_epoch_millis++;
        if(fault==12){inv.inventory.entries.erase(inv.inventory.entries.begin());inventory_summary={18,18,18};}
        if(fault==13){active.state=mga::TransactionState::created;inventory_summary={13,18,18};}
        if(fault==14){auto entry=active;entry.identity.local_id=mga::MakeLocalTransactionId(14);entry.identity.transaction_uuid.value=Id(210);inv.inventory.entries.insert(inv.inventory.entries.begin()+1,entry);}
        if(fault==15){auto entry=ended;entry.identity.local_id=mga::MakeLocalTransactionId(18);inv.inventory.entries.erase(inv.inventory.entries.begin()+1);inv.inventory.entries.push_back(entry);inv.inventory.next_local_transaction_id=19;}
        if(fault==16)ended.evidence_record_written=false;
        if(fault==17){auto& old=preceding_inventory->inventory.entries[1];old.state=mga::TransactionState::archived;old.archived_from_state=mga::TransactionState::rolled_back;}
        if(fault==18){preceding_inventory->inventory.entries[1].state=mga::TransactionState::failed_terminal;inv.inventory.entries.erase(inv.inventory.entries.begin()+1);}
        if(fault==19){active.state=mga::TransactionState::committed;active.commit_sequence=1;inventory_summary={18,18,18};}
        if(fault==20){auto entry=active;entry.identity.local_id=mga::MakeLocalTransactionId(18);entry.identity.transaction_uuid.value=Id(211);entry.state=mga::TransactionState::committed;entry.commit_sequence=1;inv.inventory.entries.push_back(entry);inv.inventory.next_local_transaction_id=19;}
        if(fault==21){preceding_inventory->inventory.entries[2].evidence_record_written=true;committed_entry.evidence_record_written=false;}
        if(fault==22){auto& old=preceding_inventory->inventory.entries[1];old.state=ended.state=mga::TransactionState::archived;old.archived_from_state=mga::TransactionState::rolled_back;ended.archived_from_state=mga::TransactionState::failed_terminal;}
        assert_history(false,fault);
        if(fault==0){
          Check(device.Close().ok()&&second_device.Close().ok(),"release conflicting history before independent reopen");
          const auto child=::fork();Check(child>=0,"fork conflicting history probe");
          if(child==0){const auto profile=std::to_string(p);::execl("/proc/self/exe","bound-selector-probe","--bound-selector-probe",fixture.root.c_str(),profile.c_str(),"invalid-evolution",nullptr);::_exit(125);}
          int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh process rejects resealed conflicting inventory history");
          Check(device.Open(path,disk::FileOpenMode::open_existing).ok()&&second_device.Open((fixture.root/"bound-selector-secondary").string(),disk::FileOpenMode::open_existing).ok(),"reopen owned history fixture");
        }
      }
      for(unsigned progress=0;progress<10;++progress){reset_history();auto& active=inv.inventory.entries[0];auto& ended=inv.inventory.entries[1];
        if(progress==0){active.state=mga::TransactionState::preparing;inventory_summary={13,18,18};}
        if(progress==1){active.state=mga::TransactionState::prepared;inventory_summary={13,18,18};}
        if(progress==2){active.state=mga::TransactionState::rolling_back;inventory_summary={13,18,18};}
        if(progress==3){active.state=mga::TransactionState::committed;active.commit_sequence=3;inv.inventory.next_commit_sequence=4;inventory_summary={18,18,18};}
        if(progress==4){ended.state=mga::TransactionState::archived;ended.archived_from_state=mga::TransactionState::rolled_back;}
        if(progress==5)inv.inventory.entries.erase(inv.inventory.entries.begin()+1);
        if(progress==6)preceding_inventory->inventory.entries.front().rollback_only=true;
        if(progress==7){preceding_inventory->inventory.entries.front().state=mga::TransactionState::created;preceding_summary={13,18,18};}
        if(progress==8){auto entry=active;entry.identity.local_id=mga::MakeLocalTransactionId(18);entry.identity.transaction_uuid.value=Id(211);entry.state=mga::TransactionState::created;inv.inventory.entries.push_back(entry);inv.inventory.next_local_transaction_id=19;}
        if(progress==9){active.state=mga::TransactionState::archived;active.archived_from_state=mga::TransactionState::committed;active.commit_sequence=3;inv.inventory.next_commit_sequence=4;inventory_summary={18,18,18};}
        assert_history(true,progress);
      }
      // Consistency positives above do not authorize transitions or pruning.
      preceding_inventory.reset();inv=saved_inv;map=saved_map;initial=saved_initial;current=saved_current;inventory_summary={13,13,13};persist();
    }
    for(unsigned n=0;n<5;++n){auto changed=selection;if(n==0)changed.bootstrap_uuid=Id(173);if(n==1)changed.checkpoint_sha256[0]^=1;if(n==2)changed.root_set_generation++;if(n==3)changed.timeline_uuid=Id(174);if(n==4)changed.previous_checkpoint_sha256[0]^=1;
      auto other=changed;other.header.page_number=32;other.header.page_uuid=Id(156);put(31,SelectionOracle(changed));put(32,SelectionOracle(other));empty(read(budget));consumers(CheckpointRef(current),false);persist();}
    {const auto prior_current=current;
      current.creator_transaction_uuid={};current.creator_local_transaction_id=0;current.creator_operation_uuid=Id(201);
      Check(db::DecodeNativeCheckpointRoot(CheckpointOracle(current)).ok(),"operation checkpoint is structural data before actual authority lookup");
      persist();const auto rejected=read(budget+64*sizes[p]);empty(rejected);
      if(rejected.error!=E::checkpoint_failure||rejected.checkpoint_error!=db::NativeCheckpointError::creator_not_committed)
        std::cerr<<"unproved operation creator error="<<static_cast<int>(rejected.error)<<" checkpoint="<<static_cast<int>(rejected.checkpoint_error)<<std::endl;
      Check(rejected.error==E::checkpoint_failure&&rejected.checkpoint_error==db::NativeCheckpointError::creator_not_committed,
        "selected checkpoint cannot invent operation authority from a complete marker");consumers(CheckpointRef(current),false);
      current=prior_current;persist();}
    const auto saved=map;
    for(unsigned n=0;n<11;++n){map=saved;auto& r=*std::find_if(map.records.begin(),map.records.end(),[](const auto& r){return r.page_number==31;});if(n==0)map.states[31]=S::reserved;if(n==1)r.page_uuid=Id(175);if(n==2)r.page_generation++;if(n==3)r.owner_uuid=Id(175);if(n==4)r.page_type=6;if(n==5){r.creator_transaction_uuid=Id(162);r.creator_local_transaction_id=13;}
      if(n==6)r.allocation_uuid=map.records[0].allocation_uuid;if(n==7){map.creator_transaction_uuid=Id(162);map.creator_local_transaction_id=13;}if(n==8){map.header.page_uuid=Id(155);std::find_if(map.records.begin(),map.records.end(),[](const auto& r){return r.page_number==35;})->page_uuid=Id(155);}
      if(n==9){map.creator_transaction_uuid={};map.creator_local_transaction_id=0;map.creator_operation_uuid=Id(176);}
      if(n==10){r.creator_transaction_uuid={};r.creator_local_transaction_id=0;r.creator_operation_uuid=Id(176);}
      if(n>=9)Check(page::DecodeNativeAllocationMap(AllocationOracle(map)).ok(),"well-formed operation lineage fixture is not a publication grant");
      persist();const auto rejected=read(budget);empty(rejected);
      if(n>=9){Check(rejected.error==E::creator_mismatch,"selected publication requires actual operation evidence");consumers(CheckpointRef(current),false);}}
    for(unsigned slot:{14u,19u,36u})for(unsigned fault=0;fault<11;++fault){map=saved;
      auto it=std::find_if(map.records.begin(),map.records.end(),[&](const auto& r){return r.page_number==slot;});auto& record=*it;
      if(fault==0){map.states[slot]=S::quarantined;map.records.erase(it);}
      if(fault==1)map.states[slot]=S::reserved;
      if(fault==2)record.page_uuid=Id(175);
      if(fault==3)record.page_generation++;
      if(fault==4)record.owner_uuid=Id(175);
      if(fault==5)record.page_type=6;
      if(fault==6){record.creator_transaction_uuid=Id(162);record.creator_local_transaction_id=13;}
      if(fault==7){map.states[slot]=S::reusable_pending_mga;record.reuse_horizon=1;}
      if(fault==8){map.states[slot]=S::reusable_free;record.reuse_horizon=1;}
      if(fault==9)map.states[slot]=S::compacting;
      if(fault==10)map.states[slot]=S::preallocated;
      persist();const auto writes_before=stage_writes;const auto refused=read(budget);empty(refused);
      Check(refused.error==E::allocation_binding_mismatch||refused.error==E::creator_mismatch||refused.error==E::allocation_failure,"selected control allocation refusal");
      Check(stage_writes==writes_before,"control allocation refusal never writes");
    }
    map=saved;
    {
      const auto old_second=second_zero;const auto old_inv=inv;
      second_zero=Example(q,2);second_zero.bootstrap.filespace_uuid=Id(7);second_zero.page_uuid=Id(8);second_zero.free_pages=second_zero.preallocated_pages=0;
      for(auto& r:second_zero.roots)r.filespace_uuid=Id(7);
      second_zero.roots[2].object_uuid=Id(179);
      directory.records[1].bootstrap=second_zero.bootstrap;
      inventory_tail=inv;inventory_tail->header={sizes[q],0x301,Id(1),Id(7),Id(201),14,104,0,Profile(q)};
      inventory_tail->inventory.entries.erase(inventory_tail->inventory.entries.begin());inventory_tail->previous=InventoryRef(inv);
      inv.inventory.entries.pop_back();inv.next=InventoryRef(*inventory_tail);
      page::NativeAllocationMap control_map;control_map.header={sizes[q],3,Id(1),Id(7),Id(202),13,103,0,Profile(q)};
      control_map.object_uuid=Id(179);control_map.map_generation=control_map.capacity_generation=1;control_map.total_pages=64;
      control_map.creator_transaction_uuid=Id(98);control_map.creator_local_transaction_id=17;control_map.states.assign(64,S::quarantined);
      for(unsigned n:{0u,13u,14u}){page::NativeAllocationRecord r;r.page_number=n;r.allocation_uuid=Id(220+n);r.creator_transaction_uuid=Id(98);r.creator_local_transaction_id=17;control_map.states[n]=S::allocated;
        if(n==0){r.page_uuid=Id(8);r.page_generation=7;r.page_type=1;r.owner_uuid=Id(7);}
        if(n==13){r.page_uuid=Id(202);r.page_generation=103;r.page_type=3;r.owner_uuid=Id(179);}
        if(n==14){r.page_uuid=Id(201);r.page_generation=104;r.page_type=0x301;r.owner_uuid=inv.object_uuid;}
        control_map.records.push_back(r);}
      const auto store_controls=[&](const auto& map_image){persist();const auto mb=AllocationOracle(map_image);
        Check(second_device.WriteAt(13*sizes[q],mb.data(),mb.size()).ok()&&second_device.Sync().ok(),"persist continuation allocation authority");};
      const u64 mixed_budget=8*sizes[p]+3*sizes[q];store_controls(control_map);
      reads=observed_allocations=observed_full_digests=0;track_reads=count_allocations=count_full_digests=true;
      auto admitted=read(mixed_budget);track_reads=count_allocations=count_full_digests=false;
      const auto mixed_reads=reads,mixed_digests=observed_full_digests;const auto mixed_allocations=observed_allocations;
      if(!admitted.ok())std::cerr<<"mixed control allocation error="<<static_cast<int>(admitted.error)<<" cp="<<static_cast<int>(admitted.checkpoint_error)<<" map="<<static_cast<int>(admitted.allocation_error)<<std::endl;
      Check(admitted.ok()&&admitted.retained_image_bytes==mixed_budget&&admitted.checkpoint_inventory.inventory_pages.size()==2&&admitted.predecessor.inventory_pages.size()==2&&
        admitted.checkpoint_inventory.inventory_pages.back().header.filespace_uuid==Id(7),"actual complete mixed-profile control allocations admitted");
      empty(read(mixed_budget-1));
      if(p==0&&role==1){
        for(unsigned long fault=0;fault<=mixed_allocations;++fault){const auto loss=device.failed_io_latency_observations()+second_device.failed_io_latency_observations();
          allocation_budget=fault;const auto r=read(mixed_budget);allocation_budget=-1;
          if(r.ok()){Check(r.selection->checkpoint_sha256==admitted.selection->checkpoint_sha256&&r.retained_image_bytes==admitted.retained_image_bytes&&
              r.checkpoint_inventory.inventory_pages.size()==2&&r.predecessor.inventory_pages.size()==2,"complete mixed control admission despite telemetry loss");
            if(fault<mixed_allocations)Check(device.failed_io_latency_observations()+second_device.failed_io_latency_observations()==loss+1,"mixed read success requires recorded observation loss");}
          else empty(r);if(fault==mixed_allocations)Check(r.ok(),"mixed control allocation fault terminal success");}
        for(unsigned fault=1;fault<=mixed_reads;++fault){reads=0;read_fault=fault;track_reads=true;const auto r=read(mixed_budget);track_reads=false;Check(!read_fault,"mixed control read fault consumed");empty(r);}
        for(unsigned fault=1;fault<=mixed_digests;++fault){full_digest_fault=fault;const auto r=read(mixed_budget);Check(!full_digest_fault,"mixed control hash fault consumed");empty(r);}
        std::cout<<"mixed control allocations="<<mixed_allocations<<" reads="<<mixed_reads<<" digests="<<mixed_digests<<std::endl;
      }
      for(unsigned fault=0;fault<11;++fault){auto changed=control_map;
        if(fault==0){changed.records.pop_back();changed.states[14]=S::quarantined;store_controls(changed);empty(read(mixed_budget));continue;}
        auto& record=changed.records.back();
        if(fault==1)changed.states[14]=S::reserved;
        if(fault==2)record.page_uuid=Id(203);
        if(fault==3)record.page_generation++;
        if(fault==4)record.page_type=6;
        if(fault==5)record.owner_uuid=Id(203);
        if(fault==6){record.creator_transaction_uuid=Id(162);record.creator_local_transaction_id=13;}
        if(fault==7)record.allocation_uuid=map.records.front().allocation_uuid;
        if(fault==8)record.page_uuid=inv.header.page_uuid;
        if(fault==9){changed.creator_transaction_uuid=Id(162);changed.creator_local_transaction_id=13;for(auto& r:changed.records){r.creator_transaction_uuid=Id(162);r.creator_local_transaction_id=13;}}
        if(fault==10)record.creator_transaction_uuid=Id(203);
        store_controls(changed);empty(read(mixed_budget));
      }
      second_zero=old_second;directory.records[1].bootstrap=second_zero.bootstrap;inv=old_inv;inventory_tail.reset();
    }
    map=saved;persist();auto bad=SelectionOracle(selection);bad.back()^=1;put(31,bad);empty(read(budget));consumers(CheckpointRef(initial),false);persist();
    Check(device.Close().ok()&&device.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"independent read-only selector reopen");Check(read(budget).ok(),"actual bound selector survives reopen");consumers(CheckpointRef(current),true);
    Check(device.Close().ok()&&second_device.Close().ok(),"release node ownership before fresh selector process");
    const auto child=::fork();Check(child>=0,"fork actual selector reader");
    if(child==0){const auto profile=std::to_string(p);::execl("/proc/self/exe","bound-selector-probe","--bound-selector-probe",fixture.root.c_str(),profile.c_str(),nullptr);::_exit(125);}
    int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable binds actual selector and retained targets");
  }
}
int main(int argc,char** argv) {
  if(argc==4&&std::string_view(argv[1])=="--directory-graph-probe"){
    try{const std::filesystem::path root=argv[2];const auto p=static_cast<unsigned>(std::stoul(argv[3]));if(p>=5)return 2;const auto q=(p+1)%5;
      disk::FileDevice first,second;if(!first.Open((root/"bound-selector").string(),disk::FileOpenMode::open_existing_read_only).ok()||!second.Open((root/"bound-selector-secondary").string(),disk::FileOpenMode::open_existing_read_only).ok())return 3;
      const std::vector<disk::NativeFilespaceDevice> files{{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}};
      const auto bound=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),files,Id(2),8*u64{sizes[p]}+3*u64{sizes[q]});if(!bound.ok())return 4;const auto& s=*bound.selection;
      const auto r=db::ReadNativeManagementControlGraphFromOpenDevices(Id(1),files,Id(2),{s.checkpoint,s.checkpoint_object_uuid,s.checkpoint_sha256,s.checkpoint_generation,s.root_set_generation,s.timeline_uuid},14*u64{sizes[p]}+4*u64{sizes[q]});
      return r.ok()&&r.verified_image_bytes==14*u64{sizes[p]}+4*u64{sizes[q]}&&r.publications.empty()&&r.allocations.empty()?0:5;
    }catch(...){return 6;}}
  if(argc==2&&std::string_view(argv[1])=="--directory-graph-only"){
    try{CanonicalBoundCheckpointSelection(false,false,false,0,1,-1,false,false,true);std::cout<<"directory immutable graph checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==4&&std::string_view(argv[1])=="--directory-controls-probe"){
    try{const std::filesystem::path root=argv[2];const auto p=static_cast<unsigned>(std::stoul(argv[3]));if(p>=5)return 2;const auto q=(p+1)%5;
      disk::FileDevice first,second;if(!first.Open((root/"bound-selector").string(),disk::FileOpenMode::open_existing_read_only).ok()||!second.Open((root/"bound-selector-secondary").string(),disk::FileOpenMode::open_existing_read_only).ok())return 3;
      const auto r=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),{{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}},Id(2),8*u64{sizes[p]}+3*u64{sizes[q]});
      return r.ok()&&r.retained_image_bytes==8*u64{sizes[p]}+3*u64{sizes[q]}&&r.selection->selection_generation==2&&r.checkpoint_inventory.inventory_pages.size()==2&&r.predecessor.inventory_pages.size()==2&&r.checkpoint_inventory.inventory_pages.back().header.filespace_uuid==Id(7)?0:4;
    }catch(...){return 5;}}
  if(argc==2&&std::string_view(argv[1])=="--directory-controls-only"){
    try{CanonicalBoundCheckpointSelection(false,false,false,0,1,-1,false,true);std::cout<<"directory selected controls checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==2&&std::string_view(argv[1])=="--checkpoint-directory-allocation-only"){
    try{CanonicalCheckpointDirectory(true);std::cout<<"extended directory chain checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==4&&std::string_view(argv[1])=="--directory-stage-probe"){
    try{const std::filesystem::path root=argv[2];const auto p=static_cast<unsigned>(std::stoul(argv[3]));if(p>=5)return 2;const auto q=(p+1)%5;
      disk::FileDevice first,second;if(!first.Open((root/"bound-selector").string(),disk::FileOpenMode::open_existing_read_only).ok()||!second.Open((root/"bound-selector-secondary").string(),disk::FileOpenMode::open_existing_read_only).ok())return 3;
      const std::vector<disk::NativeFilespaceDevice> files{{Id(2),Profile(p),&first},{Id(7),Profile(q),&second}};
      const auto directory=db::VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(Id(1),files,{9,0x300,Id(2),36,110,Profile(p),Id(49)},64*u64{sizes[p]});
      if(!directory.ok()||directory.directory.pages.size()!=1||directory.directory.pages.front().directory->records.size()!=2)return 4;
      for(unsigned target=0;target<2;++target){const auto& record=directory.directory.pages.front().directory->records[target];if(!record.allocation_root)return 5;
        const auto& r=*record.allocation_root;const auto profile=target?q:p;const auto fs=Id(target?7:2);auto& file=target?second:first;
        if(r.page!=disk::NativePageReference{fs,target?22u:35u,104,Profile(profile)}||r.object_uuid!=Id(target?179:43)||r.map_generation!=(target?3u:2u)||r.capacity_generation!=(target?2u:1u))return 6;
        const auto map=page::ReadNativeAllocationChainAtRootFromOpenDevice(file,{Id(1),fs,Profile(profile)},{3,3,fs,r.page.page_number,104,Profile(profile),r.object_uuid},sizes[profile]);
        if(!map.ok()||WholeRootHash(map.pages.front().bytes)!=r.sha256||map.pages.front().map->map_generation!=r.map_generation||map.pages.front().map->capacity_generation!=r.capacity_generation)return 7;
        auto leaf=LeafExample(profile);if(target){leaf.header.filespace_uuid=Id(7);leaf.header.page_uuid=Id(203);}Bytes bytes(sizes[profile]);const auto io=file.ReadAt(21*u64{sizes[profile]},bytes.data(),bytes.size());
        if(!io.ok()||io.bytes_transferred!=bytes.size()||bytes!=LeafOracle(leaf))return 8;
      }return 0;
    }catch(...){return 9;}}
  if(argc==2&&std::string_view(argv[1])=="--directory-stage-only"){
    try{CanonicalBoundCheckpointSelection(false,false,false,0,1,-1,true);std::cout<<"directory selected staging checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==2&&std::string_view(argv[1])=="--checkpoint-owner-only"){
    try {CanonicalCheckpoints();CanonicalCheckpointFiles();CanonicalCheckpointInventoryPair();
      std::cout<<"checkpoint owner checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}
  }
  if(argc==2&&std::string_view(argv[1])=="--filespace-initialization-only"){
    try{CanonicalFilespaceInitialization();std::cout<<"filespace initialization checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==5&&std::string_view(argv[1])=="--filespace-initialization-probe"){
    try{const auto p=static_cast<unsigned>(std::stoul(argv[3]));if(p>=5)return 2;disk::FileDevice device;
      if(!device.Open(argv[2],disk::FileOpenMode::open_existing_read_only).ok())return 3;
      VerifyInitializedFilespace(device,InitializationRequest(p,1,std::stoull(argv[4])),p);return 0;
    }catch(...){return 4;}}
  if(argc==2&&std::string_view(argv[1])=="--inventory-stage-mixed-only"){
    try{CanonicalBoundCheckpointSelection(true,true);std::cout<<"mixed inventory stage checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==2&&std::string_view(argv[1])=="--inventory-stage-only"){
    try{CanonicalBoundCheckpointSelection(true);std::cout<<"inventory stage checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==3&&std::string_view(argv[1])=="--inventory-stage-allocation-shard"){
    const std::string_view value=argv[2];int shard=-1;
    if(value.size()==1&&value[0]>='0'&&value[0]<='9')shard=value[0]-'0';
    else if(value.size()==2&&value[0]=='1'&&value[1]>='0'&&value[1]<='5')shard=10+value[1]-'0';
    if(shard<0)return 2;
    try{CanonicalBoundCheckpointSelection(true,false,false,0,1,shard);std::cout<<"inventory allocation shard checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==4&&std::string_view(argv[1])=="--inventory-stage-probe"){
    const std::filesystem::path root=argv[2];const auto p=static_cast<unsigned>(std::stoul(argv[3]));if(p>=5)return 2;
    const auto q=(p+1)%5;disk::FileDevice device,second;if(!device.Open((root/"bound-selector").string(),disk::FileOpenMode::open_existing_read_only).ok()||!second.Open((root/"bound-selector-secondary").string(),disk::FileOpenMode::open_existing_read_only).ok())return 3;
    const auto r=page::ReadNativeTransactionInventoryChainFromOpenDevices(Id(1),{{Id(2),Profile(p),&device},{Id(7),Profile(q),&second}},{4,0x301,Id(2),23,107,Profile(p),Id(44)},2*sizes[p]+sizes[q]);
    return r.ok()&&r.pages.size()==2&&r.pages.front().page->inventory_generation==20&&r.inventory.entries.size()==2&&r.inventory.entries.front().identity.transaction_uuid.value==Id(162)?0:4;}
  if(argc==5&&std::string_view(argv[1])=="--btree-stage-probe"){
    const std::filesystem::path path=argv[2];const auto p=static_cast<unsigned>(std::stoul(argv[3])),variant=static_cast<unsigned>(std::stoul(argv[4]));if(p>=5||variant>=5)return 2;const auto q=(p+1)%5;
    disk::FileDevice device;if(!device.Open((path/"btree-stage-secondary").string(),disk::FileOpenMode::open_existing_read_only).ok())return 3;
    auto dependencies=BtreeExample(q).dependencies;dependencies.index_uuid=Id(180);dependencies.key_profile_uuid=Id(181);dependencies.visibility_profile_uuid=Id(182);dependencies.dependency_map_uuid=Id(183);
    const auto r=page::ReadNativeBtreePageFromOpenDevice(device,Id(1),{Id(7),30,102,Profile(q)},variant<3?0x200:variant==3?0x201:0x202,dependencies);
    return r.ok()&&r.page->creator_transaction_uuid==Id(162)&&r.page->creator_local_transaction_id==13&&r.page->cells.size()==(variant==0?0u:3u)?0:4;
  }
  if(argc==2&&std::string_view(argv[1])=="--btree-stage-only"){
    try{CanonicalBtreeStaging();std::cout<<"B-tree stage checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}
  }
  if(argc==5&&std::string_view(argv[1])=="--catalog-root-stage-probe"){
    const std::filesystem::path path=argv[2];const auto p=static_cast<unsigned>(std::stoul(argv[3])),kind=static_cast<unsigned>(std::stoul(argv[4]));if(p>=5||(kind!=2&&kind!=6&&kind!=7&&kind!=8))return 2;const auto q=(p+1)%5;
    disk::FileDevice first,second;if(!first.Open((path/"root-stage-primary").string(),disk::FileOpenMode::open_existing_read_only).ok()||!second.Open((path/"root-stage-history").string(),disk::FileOpenMode::open_existing_read_only).ok())return 3;
    const auto type=kind==6?10u:kind==7?11u:5u;
    const auto root=page::ReadNativeCatalogRootFromOpenDevice(first,Id(1),{static_cast<disk::u16>(kind),type,Id(2),30,7,Profile(p),Id(180+kind)});
    const auto prior=page::ReadNativeCatalogRootFromOpenDevice(second,Id(1),{static_cast<disk::u16>(kind),type,Id(7),12,102,Profile(q),Id(180+kind)});
    return root.ok()&&prior.ok()&&root.root->catalog_generation==2&&prior.root->catalog_generation==1&&root.root->predecessor_sha256==WholeRootHash(prior.bytes)?0:4;
  }
  if(argc==2&&std::string_view(argv[1])=="--catalog-root-stage-only"){
    try{CanonicalCatalogRootStaging();std::cout<<"catalog root stage checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}
  }
  if((argc==4||argc==5)&&std::string_view(argv[1])=="--bound-selector-probe"){
    const std::filesystem::path root=argv[2];const auto p=static_cast<unsigned>(std::stoul(argv[3]));if(p>=5)return 2;const auto q=(p+1)%5;
    disk::FileDevice first,second;
    if(!first.Open((root/"bound-selector").string(),disk::FileOpenMode::open_existing_read_only).ok()||
      !second.Open((root/"bound-selector-secondary").string(),disk::FileOpenMode::open_existing_read_only).ok())return 3;
    const auto r=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),{{Id(7),Profile(q),&second},{Id(2),Profile(p),&first}},Id(2),8*sizes[p]);
    if(argc==5)return std::string_view(argv[4])=="invalid-evolution"&&!r.ok()&&
      r.error==db::NativeCheckpointSelectionError::checkpoint_binding_mismatch&&!r.selection&&
      r.slots[0].empty()&&r.slots[1].empty()&&!r.checkpoint_inventory.checkpoint&&!r.predecessor.checkpoint&&
      r.allocation.pages.empty()&&!r.retained_image_bytes?0:5;
    return r.ok()&&r.selection->selection_generation==2&&r.checkpoint_inventory.checkpoint->header.page_number==36&&
      r.predecessor.checkpoint->header.page_number==19&&r.allocation.state_counts[0]==1?0:4;
  }
  if(argc==2&&std::string_view(argv[1])=="--bound-selector-only"){
    try{CanonicalBoundCheckpointSelection();std::cout<<"bound selector checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==2&&std::string_view(argv[1])=="--bound-history-only"){
    try{CanonicalBoundCheckpointSelection(false,false,true);std::cout<<"bound history checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==3&&std::string_view(argv[1])=="--bound-history-shard"&&std::string_view(argv[2]).size()==1&&argv[2][0]>='0'&&argv[2][0]<='7'){
    try{CanonicalBoundCheckpointSelection(false,false,true,static_cast<unsigned>(argv[2][0]-'0'),8);std::cout<<"bound history checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==2&&std::string_view(argv[1])=="--catalog-version-stage-only"){
    try{CanonicalCatalogVersionStaging();std::cout<<"catalog version stage checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==2&&std::string_view(argv[1])=="--row-data-stage-only"){
    try {CanonicalRowDataStaging();std::cout<<"PASS row-data-stage checks="<<checks<<'\n';return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}
  }
  if(argc==2&&std::string_view(argv[1])=="--catalog-stage-only"){
    try{CanonicalCatalogLeafStaging();std::cout<<"catalog stage checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==2&&std::string_view(argv[1])=="--checkpoint-horizon-only"){
    try{CanonicalCheckpointHorizons();std::cout<<"checkpoint horizon checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==2&&std::string_view(argv[1])=="--checkpoint-system-only") {
    try { CanonicalCheckpointSystemState();std::cout<<"checkpoint system-state checks="<<checks<<" failures=0\n";return 0; }
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}
  }
  if(argc==2&&std::string_view(argv[1])=="--checkpoint-directory-only") {
    try { CanonicalCheckpointDirectory();std::cout<<"checkpoint directory checks="<<checks<<" failures=0\n";return 0; }
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}
  }
  if(argc==2&&std::string_view(argv[1])=="--directory-allocation-only"){
    try{CanonicalDirectoryAllocationBindings();std::cout<<"directory allocation binding checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}
  if(argc==2&&std::string_view(argv[1])=="--policy-roots-only") {
    try{CanonicalPolicyRoots();std::cout<<"policy-root checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}
  }
  if(argc==2&&std::string_view(argv[1])=="--checkpoint-allocation-only") {
    try {CanonicalCheckpointAllocation();std::cout<<"checkpoint allocation checks="<<checks<<" failures=0\n";return 0;}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}
  }
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
  if(argc!=1){std::cerr<<"FAIL unknown test mode or invalid argument count\n";return 2;}
  try { CheckpointCatalogRelations(); std::cout<<"checkpoint_catalog_checks="<<checks<<std::endl; NativeCatalogRelationBindings(); NativeBtreeTrees(); NativeBtreePages(); CanonicalCheckpointCatalogRoots(); CanonicalCheckpointHistory(); CanonicalCheckpoints(); CanonicalCheckpointFiles(); CanonicalCheckpointInventoryPair(); CanonicalInventoryImages(); CanonicalInventoryChains(); Codecs(); Files(); CatalogRoots(); CatalogRootFiles(); CatalogRootRanges(); CatalogLeaves(); CatalogLeafFiles();
    std::cout<<"PASS checks="<<checks<<" canonical_page_image_and_chain_only=true\n"; return 0; }
  catch(const std::exception& e) { allocation_budget=-1; std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n'; return 1; }
}

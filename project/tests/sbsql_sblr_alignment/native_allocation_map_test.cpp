// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_allocation_map.hpp"
#include "disk_device.hpp"
#include "filespace_page_zero.hpp"
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
#include <unistd.h>

namespace {
long allocation_budget = -1;
bool count_allocations = false;
unsigned long observed_allocations = 0;
unsigned reads = 0, fail_read = 0;
bool fail_hash = false;
}
void* operator new(std::size_t n) {
  if (count_allocations) ++observed_allocations;
  if (allocation_budget == 0) { allocation_budget = -1; throw std::bad_alloc(); }
  if (allocation_budget > 0) --allocation_budget;
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* out, size_t n, off_t at) {
  ++reads;
  if (fail_read && reads == fail_read) { errno = EIO; return -1; }
  return __real_pread(fd, out, n, at);
}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new() {
  if (fail_hash) { fail_hash = false; return nullptr; }
  return __real_EVP_MD_CTX_new();
}
namespace {
namespace p = scratchbird::storage::page;
namespace d = scratchbird::storage::disk;
using namespace scratchbird::core::platform;
using S = p::NativeAllocationState;
using E = p::NativeAllocationError;
using Bytes = std::vector<byte>;
std::size_t checks = 0;
void Check(bool pass, const char* message, std::source_location where = std::source_location::current()) {
  ++checks;
  if (!pass) { std::cerr << where.line() << ": " << message << '\n'; throw std::runtime_error("native allocation conformance failed"); }
}
Uuid Id(byte n) { return Uuid{{1,2,3,4,5,6,0x71,8,0x89,10,11,12,13,14,15,n}}; }
void Num(Bytes& b, std::size_t at, unsigned size, u64 n) {
  for (unsigned i = 0; i < size; ++i) b[at+i] = static_cast<byte>(n >> (8*i));
}
void Put(Bytes& b, std::size_t at, const Uuid& id) { std::copy(id.bytes.begin(), id.bytes.end(), b.begin()+at); }
std::array<byte,32> Hash(const Bytes& b) {
  std::array<byte,32> out{}; Check(SHA256(b.data(), b.size(), out.data()) != nullptr, "independent SHA256"); return out;
}
void Seal(Bytes& b) {
  std::fill(b.begin()+312, b.begin()+344, 0); const auto hash = Hash(b);
  std::copy(hash.begin(), hash.end(), b.begin()+312);
}
// Independent bytes, including the common header and its FNV checksum.
Bytes Oracle(const p::NativeAllocationMap& m) {
  Bytes b(m.header.page_size_bytes, 0); const auto& h = m.header;
  std::copy_n("SBPGV002",8,b.begin()); Num(b,8,4,128); Num(b,12,4,h.page_size_bytes);
  Num(b,16,4,h.page_type); Num(b,20,2,1); Num(b,22,2,1); Put(b,24,h.database_uuid);
  Put(b,40,h.filespace_uuid); Put(b,56,h.page_uuid); Num(b,72,8,h.page_number);
  Num(b,80,8,h.page_generation); Num(b,88,8,h.flags); Put(b,104,h.page_size_profile_uuid); Num(b,120,2,1);
  u64 fnv = 14695981039346656037ull;
  for (unsigned i=0;i<128;++i) { fnv ^= b[i]; fnv *= 1099511628211ull; } Num(b,96,8,fnv);
  const auto bitmap = (m.states.size()+1)/2, at = (384+bitmap+7)&~std::size_t(7);
  unsigned version=m.creator_operation_uuid.is_nil()?1:2;
  for(const auto& r:m.records)if(!r.creator_operation_uuid.is_nil())version=2;
  std::copy_n(version==1?"SBABM001":"SBABM002",8,b.begin()+128); Num(b,136,2,version); Num(b,138,2,256);
  Num(b,140,4,at+128*m.records.size()); Put(b,144,m.object_uuid); Num(b,160,8,m.map_generation);
  Num(b,168,8,m.capacity_generation); Num(b,176,8,m.total_pages); Num(b,184,8,m.first_page);
  Num(b,192,8,m.states.size()); Put(b,200,m.creator_transaction_uuid); Num(b,216,8,m.creator_local_transaction_id);
  if (m.next) { Put(b,224,m.next->filespace_uuid); Num(b,240,8,m.next->page_number);
    Num(b,248,8,m.next->page_generation); Put(b,256,m.next->page_size_profile_uuid); }
  std::copy(m.next_sha256.begin(),m.next_sha256.end(),b.begin()+272);
  Num(b,304,4,bitmap); Num(b,308,4,m.records.size());
  Put(b,344,m.creator_operation_uuid);
  for (std::size_t i=0;i<m.states.size();++i) b[384+i/2] |= static_cast<byte>(m.states[i]) << (4*(i%2));
  for (std::size_t i=0;i<m.records.size();++i) { const auto& r=m.records[i]; const auto pos=at+128*i;
    Num(b,pos,8,r.page_number); Put(b,pos+8,r.allocation_uuid); Put(b,pos+24,r.page_uuid);
    Put(b,pos+40,r.owner_uuid); Put(b,pos+56,r.creator_transaction_uuid); Num(b,pos+72,8,r.creator_local_transaction_id);
    Num(b,pos+80,8,r.page_generation); Num(b,pos+88,8,r.reuse_horizon); Num(b,pos+96,4,r.page_type);
    Put(b,pos+100,r.creator_operation_uuid); }
  Seal(b); return b;
}
p::NativeAllocationMap Example(unsigned profile=0) {
  const auto& size = d::kCanonicalFilespacePageProfiles[profile];
  p::NativeAllocationMap m;
  m.header = {size.page_size_bytes,3,Id(1),Id(2),Id(10),1,4,0,size.uuid};
  m.object_uuid=Id(20); m.map_generation=5; m.capacity_generation=6; m.total_pages=11;
  m.creator_transaction_uuid=Id(31); m.creator_local_transaction_id=10;
  m.states={S::allocated,S::allocated,S::free,S::reserved,S::allocated,S::reusable_pending_mga,
            S::reusable_free,S::compacting,S::quarantined,S::preallocated,S::quarantined};
  for (unsigned i=0;i<m.states.size();++i) {
    if (m.states[i]==S::free || i==8) continue;
    p::NativeAllocationRecord r{i,Id(40+i),Id(60+i),Id(80+i),Id(30),8,3,0,6};
    if (i==0) { r.page_uuid=Id(3); r.page_generation=7; r.page_type=2; r.owner_uuid=Id(2); }
    if (i==1) { r.page_uuid=Id(10); r.page_generation=4; r.page_type=3; r.owner_uuid=Id(20); }
    if (i==3 || i==9) { r.page_uuid={}; r.page_generation=0; }
    if (i==5 || i==6) r.reuse_horizon=9;
    m.records.push_back(r);
  }
  return m;
}
void Empty(const p::NativeAllocationMapResult& r) { Check(!r.ok() && !r.map && r.bytes.empty(), "no partial map failure"); }
void Empty(const p::NativeAllocationChainResult& r) {
  Check(!r.ok() && r.pages.empty() && r.retained_image_bytes==0 &&
        std::all_of(r.state_counts.begin(),r.state_counts.end(),[](u64 n){return n==0;}), "no partial chain failure");
}
void Codecs() {
  for (unsigned profile=0;profile<5;++profile) {
    const auto m=Example(profile); const auto expected=Oracle(m); const auto encoded=p::EncodeNativeAllocationMap(m);
    Check(encoded.ok() && encoded.bytes==expected,"complete independent allocation image");
    const auto decoded=p::DecodeNativeAllocationMap(expected);
    Check(decoded.ok() && decoded.map->states==m.states && decoded.map->records==m.records,
          "all allocation states and exact binary ownership records");
    Check(p::EncodeNativeAllocationMap(*decoded.map).bytes==expected,"decoded metadata exact re-encoding");
  }
  const auto good=Example(); const auto bytes=Oracle(good);
  for (unsigned mutation=0;mutation<25;++mutation) {
    auto m=good;
    if(mutation==0)m.object_uuid={}; if(mutation==1)m.creator_transaction_uuid.bytes[6]=0x41;
    if(mutation==2)m.map_generation=0; if(mutation==3)m.capacity_generation=0;
    if(mutation==4)m.first_page=std::numeric_limits<u64>::max(); if(mutation==5)m.total_pages=0;
    if(mutation==6)m.states[2]=static_cast<S>(8); if(mutation==7)m.records[0].allocation_uuid={};
    if(mutation==8)m.records[0].owner_uuid={}; if(mutation==9)m.records[0].creator_transaction_uuid={};
    if(mutation==10)m.records[0].page_uuid={}; if(mutation==11)m.records[0].page_generation=0;
    if(mutation==12)m.records[0].creator_local_transaction_id=11; if(mutation==13)m.records[0].page_type=0xdead;
    if(mutation==14)m.records[0].page_number=1; if(mutation==15)m.records[0].page_number=11;
    if(mutation==16)m.records.erase(m.records.begin()); if(mutation==17)m.states[0]=S::free;
    if(mutation==18)m.records[4].reuse_horizon=0; if(mutation==19)m.records[0].reuse_horizon=1;
    if(mutation==20)m.next=d::NativePageReference{Id(2),2,1,good.header.page_size_profile_uuid};
    if(mutation==21)m.next_sha256[0]=1; if(mutation==22)m.total_pages=12;
    if(mutation==23)m.records[0].page_uuid.bytes[8]=0; if(mutation==24)m.records[0].creator_local_transaction_id=0;
    Empty(p::EncodeNativeAllocationMap(m)); Empty(p::DecodeNativeAllocationMap(Oracle(m)));
  }
  for (std::size_t at : {128u,136u,138u,140u,304u,308u,344u,383u,389u,390u,391u,492u,8191u}) {
    auto bad=bytes; bad[at]^=0x80; Seal(bad); Empty(p::DecodeNativeAllocationMap(bad));
  }
  for (std::size_t n : {0u,127u,383u,8191u,8193u}) { auto bad=bytes;bad.resize(n);Empty(p::DecodeNativeAllocationMap(bad)); }
  for (std::size_t at=0;at<bytes.size();++at) { auto bad=bytes;bad[at]^=1;Empty(p::DecodeNativeAllocationMap(bad)); }
  for (unsigned mode=0;mode<2;++mode) {
    fail_hash=true;const auto r=mode?p::DecodeNativeAllocationMap(bytes):p::EncodeNativeAllocationMap(good);
    Check(!fail_hash && r.error==E::hash_failure,"hash failure surfaced");Empty(r);
    bool succeeded=false;
    for(long budget=0;budget<100;++budget) {
      allocation_budget=budget;const auto result=mode?p::DecodeNativeAllocationMap(bytes):p::EncodeNativeAllocationMap(good);allocation_budget=-1;
      if(result.ok()){succeeded=true;break;} Empty(result); Check(result.error==E::resource_exhausted,"allocation failure classified");
    }
    Check(succeeded,"all allocation failure positions passed");
  }
}
template<class T> void OperationOwned(T& value, byte id=100) {
  value.creator_transaction_uuid={};value.creator_local_transaction_id=0;value.creator_operation_uuid=Id(id);
}
void OperationCodecs() {
  for(unsigned profile=0;profile<5;++profile) {
    // Every combination of original record owners, independently of the map
    // creator. This includes reserved/uninitialized and retained/reuse states.
    const auto original=Example(profile);
    for(unsigned map_owner=0;map_owner<2;++map_owner)
      for(unsigned mask=0;mask<(1u<<original.records.size());++mask) {
        auto m=original;if(map_owner)OperationOwned(m);
        for(unsigned i=0;i<m.records.size();++i)if(mask&(1u<<i))OperationOwned(m.records[i],110+i);
        const auto expected=Oracle(m);const auto encoded=p::EncodeNativeAllocationMap(m);
        Check(encoded.ok()&&encoded.bytes==expected,"independent mixed-owner image");
        Check(expected[135]==((mask||map_owner)?'2':'1')&&expected[136]==((mask||map_owner)?2:1),
              "canonical version selected by actual owner presence");
        const auto decoded=p::DecodeNativeAllocationMap(expected);
        Check(decoded.ok()&&decoded.map->records==m.records&&
              decoded.map->creator_transaction_uuid==m.creator_transaction_uuid&&
              decoded.map->creator_local_transaction_id==m.creator_local_transaction_id&&
              decoded.map->creator_operation_uuid==m.creator_operation_uuid,"exclusive binary owners preserved");
        Check(p::EncodeNativeAllocationMap(*decoded.map).bytes==expected,"operation lineage exact re-encoding");
      }
    Uuid nil{},v7=Id(101),v4=v7,bad_variant=v7;v4.bytes[6]=0x41;bad_variant.bytes[8]=0x09;
    const std::array<Uuid,4> ids{nil,v7,v4,bad_variant};
    const std::array<u64,3> numbers{0,1,std::numeric_limits<u64>::max()};
    for(unsigned level=0;level<2;++level)for(unsigned tx=0;tx<4;++tx)
      for(unsigned op=0;op<4;++op)for(auto number:numbers) {
        auto m=original;OperationOwned(m);for(auto& r:m.records)OperationOwned(r);
        if(level==0){m.creator_transaction_uuid=ids[tx];m.creator_operation_uuid=ids[op];m.creator_local_transaction_id=number;}
        else {auto& r=m.records[0];r.creator_transaction_uuid=ids[tx];r.creator_operation_uuid=ids[op];r.creator_local_transaction_id=number;}
        const bool valid=(tx==1&&op==0&&number!=0)||(tx==0&&op==1&&number==0);
        const auto encoded=p::EncodeNativeAllocationMap(m),decoded=p::DecodeNativeAllocationMap(Oracle(m));
        Check(encoded.ok()==valid&&decoded.ok()==valid,"complete owner identity and number truth table");
        if(!valid){Empty(encoded);Empty(decoded);}
      }
    auto m=original;OperationOwned(m);for(auto& r:m.records)OperationOwned(r);
    const auto bytes=Oracle(m);
    const auto records_at=(384+(m.states.size()+1)/2+7)&~std::size_t(7);
    for(std::size_t at=360;at<384;++at){auto bad=bytes;bad[at]=1;Seal(bad);Empty(p::DecodeNativeAllocationMap(bad));}
    for(unsigned i=0;i<m.records.size();++i)for(unsigned offset=116;offset<128;++offset){
      auto bad=bytes;bad[records_at+128*i+offset]=1;Seal(bad);Empty(p::DecodeNativeAllocationMap(bad));}
    for(unsigned mutation=0;mutation<5;++mutation){auto bad=bytes;
      if(mutation==0)bad[135]='1';if(mutation==1)Num(bad,136,2,1);
      if(mutation==2){bad[135]='1';Num(bad,136,2,1);}
      if(mutation==3)Num(bad,136,2,3);
      if(mutation==4){bad=Oracle(original);bad[135]='2';Num(bad,136,2,2);}
      Seal(bad);Empty(p::DecodeNativeAllocationMap(bad));
    }
    // An operation-owned record does not waive numeric ordering for the
    // remaining transaction-owned records of a transaction-owned map.
    auto ordered=original;OperationOwned(ordered.records.back());
    ordered.records.front().creator_local_transaction_id=11;
    Empty(p::EncodeNativeAllocationMap(ordered));Empty(p::DecodeNativeAllocationMap(Oracle(ordered)));
    for(unsigned mode=0;mode<2;++mode){
      fail_hash=true;const auto r=mode?p::DecodeNativeAllocationMap(bytes):p::EncodeNativeAllocationMap(m);
      Check(!fail_hash&&r.error==E::hash_failure,"operation image hash failure");Empty(r);
      bool complete=false;
      for(long budget=0;budget<100;++budget){allocation_budget=budget;
        const auto result=mode?p::DecodeNativeAllocationMap(bytes):p::EncodeNativeAllocationMap(m);allocation_budget=-1;
        if(result.ok()){complete=true;break;}Empty(result);Check(result.error==E::resource_exhausted,"operation image allocation failure");}
      Check(complete,"operation image allocation sweep complete");
    }
  }
}
struct Fixture {
  std::filesystem::path root;
  Fixture() { std::string path=(std::filesystem::temp_directory_path()/"sb-native-allocation.XXXXXX").string();
    auto* result=::mkdtemp(path.data());Check(result,"create isolated fixture");root=result; }
  ~Fixture(){std::error_code e;std::filesystem::remove_all(root,e);}
};
void RetainedChain() {
  for (unsigned profile=0;profile<5;++profile) for(unsigned ownership=0;ownership<4;++ownership) {
    Fixture fixture;auto full=Example(profile); full.states[2]=S::allocated;
    full.records.insert(full.records.begin()+2,{2,Id(42),Id(11),Id(20),Id(30),8,5,0,3});
    if(ownership==1||ownership==3)OperationOwned(full);
    if(ownership>=2)for(auto& r:full.records)if(ownership==3||r.page_number>=5)OperationOwned(r,110+r.page_number);
    auto head=full,tail=full; head.states.resize(5);
    head.records.erase(std::remove_if(head.records.begin(),head.records.end(),[](const auto& r){return r.page_number>=5;}),head.records.end());
    tail.first_page=5;tail.states.erase(tail.states.begin(),tail.states.begin()+5);
    tail.records.erase(std::remove_if(tail.records.begin(),tail.records.end(),[](const auto& r){return r.page_number<5;}),tail.records.end());
    tail.header.page_number=2;tail.header.page_generation=5;tail.header.page_uuid=Id(11);
    const auto tail_bytes=Oracle(tail);head.next=d::NativePageReference{Id(2),2,5,head.header.page_size_profile_uuid};
    head.next_sha256=Hash(tail_bytes); const auto head_bytes=Oracle(head);
    d::FilespacePageZero zero; auto& b=zero.bootstrap;
    b.database_uuid=Id(1);b.filespace_uuid=Id(2);b.page_size_profile_uuid=head.header.page_size_profile_uuid;
    b.page_size_bytes=head.header.page_size_bytes;b.checksum_profile_uuid=d::kNativeBootstrapIntegrityProfile;
    b.filespace_role=5;b.lifecycle_state=1;zero.page_uuid=Id(3);zero.creation_operation_uuid=Id(4);zero.writer_identity_uuid=Id(5);
    zero.page_generation=7;zero.root_set_generation=8;zero.total_pages=11;zero.free_pages=1;zero.preallocated_pages=1;
    zero.roots.push_back({3,3,Id(2),1,4,b.page_size_profile_uuid,Id(20)});
    const auto zero_bytes=d::EncodeFilespacePageZero(zero);Check(zero_bytes.ok(),"actual filespace metadata fixture");
    d::FileDevice device;const auto path=(fixture.root/"node").string();
    Check(device.Open(path,d::FileOpenMode::create_new).ok(),"own filespace fixture");
    const byte padding=0;
    const auto write=[&](u64 number,const Bytes& image){const auto io=device.WriteAt(number*b.page_size_bytes,image.data(),image.size());
      Check(io.ok()&&io.bytes_transferred==image.size()&&device.Sync().ok(),"persist actual fixture image");};
    Check(device.WriteAt(zero.total_pages*b.page_size_bytes-1,&padding,1).ok(),"allocate actual fixture length");
    write(0,*zero_bytes.bytes);write(1,head_bytes);write(2,tail_bytes);
    const d::FilespaceBootstrapBinding binding{Id(1),Id(2),b.page_size_profile_uuid};
    const auto read=[&](u64 budget){return p::ReadNativeAllocationChainFromOpenDevice(device,binding,budget);};
    const u64 limit=2*b.page_size_bytes;
    reads=0;observed_allocations=0;count_allocations=true;auto result=read(limit);count_allocations=false;
    const auto read_count=reads;const auto allocation_count=observed_allocations;
    Check(result.ok()&&result.pages.size()==2&&result.retained_image_bytes==limit&&result.state_counts[4]==1&&
          result.state_counts[7]==1&&result.pages[0].bytes==head_bytes&&result.pages[1].bytes==tail_bytes,
          "actual complete multi-page allocation chain");
    Empty(read(limit-1));
    for(unsigned fault=1;fault<=read_count;++fault){reads=0;fail_read=fault;result=read(limit);fail_read=0;Empty(result);}
    if(profile==0){bool success=false;
      for(unsigned long budget=0;budget<=allocation_count;++budget){allocation_budget=static_cast<long>(budget);result=read(limit);allocation_budget=-1;
        if(result.ok()){success=true;break;}Empty(result);Check(result.error==E::resource_exhausted,"retained allocation failure classified");}
      Check(success,"all retained-chain allocation failure positions");
      std::cout << "retained allocation fault positions=" << allocation_count << '\n';
    }
    for(unsigned mutation=0;mutation<10;++mutation){auto bad_head=head,bad_tail=tail;
      if(mutation==0)bad_tail.map_generation++;
      if(mutation==1)bad_tail.header.page_uuid=Id(10);
      if(mutation==2){if(bad_tail.creator_operation_uuid.is_nil())bad_tail.creator_transaction_uuid=Id(90);
        else bad_tail.creator_operation_uuid=Id(90);}
      if(mutation==3)bad_head.records[1].page_uuid=Id(90);
      if(mutation==4)bad_head.records[0].page_generation++;
      if(mutation==5)bad_head.records[2].owner_uuid=Id(90);
      if(mutation==6)bad_head.next->page_generation++;
      if(mutation==7)bad_tail.object_uuid=Id(90);
      if(mutation==8){bad_head.header.page_uuid=zero.page_uuid;bad_head.records[1].page_uuid=zero.page_uuid;}
      if(mutation==9)OperationOwned(bad_tail,101);
      const auto changed_tail=Oracle(bad_tail);bad_head.next_sha256=Hash(changed_tail);
      if(mutation==2||mutation==9)Check(p::DecodeNativeAllocationMap(changed_tail).ok()&&
        p::DecodeNativeAllocationMap(Oracle(bad_head)).ok(),"lineage mismatch preserves individually valid images");
      write(1,Oracle(bad_head));write(2,changed_tail);const auto rejected=read(limit);Empty(rejected);
      if(mutation==2||mutation==9)Check(rejected.error==E::chain_mismatch,"actual chain binds complete creator tuple");
    }
    write(1,head_bytes);write(2,tail_bytes);
    auto changed=tail_bytes;changed[392+8+15]^=1;Seal(changed);write(2,changed);Empty(read(limit));write(2,tail_bytes);
    { // Individually valid nonterminal images must not navigate back into the root.
      auto cycle_head=head,cycle_tail=full;
      cycle_head.states.resize(4);
      cycle_head.records.erase(std::remove_if(cycle_head.records.begin(),cycle_head.records.end(),
          [](const auto& r){return r.page_number>=4;}),cycle_head.records.end());
      cycle_tail.header=tail.header;cycle_tail.first_page=4;
      cycle_tail.states={S::allocated,S::reusable_pending_mga,S::reusable_free,S::compacting};
      cycle_tail.records.erase(std::remove_if(cycle_tail.records.begin(),cycle_tail.records.end(),
          [](const auto& r){return r.page_number<4||r.page_number>=8;}),cycle_tail.records.end());
      cycle_tail.next=d::NativePageReference{Id(2),1,4,b.page_size_profile_uuid};cycle_tail.next_sha256[0]=1;
      const auto cycle_bytes=Oracle(cycle_tail);cycle_head.next_sha256=Hash(cycle_bytes);
      Check(p::DecodeNativeAllocationMap(cycle_bytes).ok()&&p::EncodeNativeAllocationMap(cycle_head).ok(),
            "cycle fixture images individually valid");
      write(1,Oracle(cycle_head));write(2,cycle_bytes);const auto cycle=read(limit);
      Empty(cycle);Check(cycle.error==E::chain_mismatch,"physical root cycle rejected before reread");
      write(1,head_bytes);write(2,tail_bytes);
    }
    auto wrong_zero=zero;wrong_zero.free_pages=2;auto wrong=d::EncodeFilespacePageZero(wrong_zero);Check(wrong.ok(),"counter mismatch fixture");
    write(0,*wrong.bytes);Empty(read(limit));write(0,*zero_bytes.bytes);
    fail_hash=true;result=read(limit);Check(!fail_hash,"retained hash failure reached provider");Empty(result);
    Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing_read_only).ok(),"reopen read-only actual filespace");
    Check(read(limit).ok()&&device.read_only(),"retained reader preserves read-only ownership");
    Check(device.Close().ok(),"close fixture");Empty(read(limit));
  }
}
}  // namespace
int main(){
  try { Codecs();OperationCodecs();RetainedChain();std::cout<<"native allocation checks="<<checks<<" failures=0\n"; }
  catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_filespace_directory.hpp"
#include "disk_device.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <new>
#include <mutex>
#include <source_location>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#ifdef SB_NATIVE_DIRECTORY_MEMORY_TESTS
#include "native_filespace_directory_memory.hpp"
#include <thread>
#endif
namespace {
long allocation_budget=-1;
bool count_allocations=false;
unsigned long allocations=0;
unsigned reads=0,fail_read=0,hash_calls=0,fail_hash=0;
unsigned resize_at_read=0;off_t resize_to=0;bool resized=false;
unsigned stats=0,fail_stat=0;unsigned long writes=0,syncs=0;
std::mutex pause_mutex;std::condition_variable pause_cv;
bool pause_next=false,entered=false,released=false;
void* last_read_buffer=nullptr;
unsigned short_read=0,eof_read=0;
}
void* operator new(std::size_t n){if(count_allocations)++allocations;
  if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;
  if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
#ifdef SB_NATIVE_DIRECTORY_MEMORY_TESTS
namespace {
std::recursive_mutex* allocation_device_mutex=nullptr;
std::recursive_mutex* deallocation_device_mutex=nullptr;
bool allocation_lock_free=false,deallocation_lock_free=false;
void ProbeCleanup(){if(deallocation_device_mutex){auto* mutex=deallocation_device_mutex;deallocation_device_mutex=nullptr;
  bool available=false;std::thread worker([&]{available=mutex->try_lock();if(available)mutex->unlock();});worker.join();deallocation_lock_free=available;}}
}
void* operator new(std::size_t n,std::align_val_t a){
  if(count_allocations)++allocations;
  if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;
  if(allocation_device_mutex){auto* mutex=allocation_device_mutex;allocation_device_mutex=nullptr;
    bool available=false;std::thread worker([&]{available=mutex->try_lock();if(available)mutex->unlock();});worker.join();allocation_lock_free=available;}
  void* p=nullptr;if(posix_memalign(&p,static_cast<std::size_t>(a),n?n:1)==0)return p;throw std::bad_alloc();
}
void operator delete(void* p,std::align_val_t)noexcept{ProbeCleanup();std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t)noexcept{ProbeCleanup();std::free(p);}
#endif
#ifdef SB_NATIVE_DIRECTORY_MEMORY_TESTS
namespace {unsigned method_fault=0;}
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* p,size_t s,unsigned char* b,unsigned int* n,const EVP_MD* m,ENGINE* e){
  if(method_fault==1){method_fault=0;return 0;}return __real_EVP_Digest(p,s,b,n,m,e);}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* m,ENGINE* e){
  if(method_fault==2){method_fault=0;return 0;}return __real_EVP_DigestInit_ex(c,m,e);}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* b,size_t n){
  if(method_fault==3){method_fault=0;return 0;}return __real_EVP_DigestUpdate(c,b,n);}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* b,unsigned int* n){
  if(method_fault==4){method_fault=0;return 0;}const auto r=__real_EVP_DigestFinal_ex(c,b,n);
  if(method_fault==5){method_fault=0;*n=31;}return r;}
#endif
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* p,size_t n,off_t o){++reads;last_read_buffer=p;if(fail_read==reads){errno=EIO;return -1;}
  if(eof_read&&reads==eof_read)return 0;
  if(resize_at_read==reads)resized=::ftruncate(fd,resize_to)==0;
  {std::unique_lock lock(pause_mutex);if(pause_next){pause_next=false;entered=true;pause_cv.notify_all();pause_cv.wait(lock,[]{return released;});}}
  return __real_pread(fd,p,short_read&&reads==short_read?n-1:n,o);}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){++hash_calls;if(fail_hash==hash_calls)return nullptr;return __real_EVP_MD_CTX_new();}
#ifdef NATIVE_HISTORICAL_IO_FAULTS
extern "C" int __real_fstat(int,struct stat*);
extern "C" int __wrap_fstat(int fd,struct stat* out){++stats;if(fail_stat==stats){errno=EIO;return -1;}return __real_fstat(fd,out);}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t n,off_t at){++writes;return __real_pwrite(fd,data,n,at);}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){++syncs;return __real_fsync(fd);}
#endif
namespace {
namespace p=scratchbird::storage::page;namespace d=scratchbird::storage::disk;
using namespace scratchbird::core::platform;
using Bytes=std::vector<byte>;using E=p::NativeDirectoryError;
std::size_t checks=0;
void Check(bool good,const char* message,std::source_location at=std::source_location::current()){
  ++checks;if(!good)throw std::runtime_error(std::string(message)+" line="+std::to_string(at.line()));}
Uuid Id(byte n){Uuid id;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=n;return id;}
void Num(Bytes& b,std::size_t at,unsigned n,u64 v){for(unsigned i=0;i<n;++i)b[at+i]=static_cast<byte>(v>>(i*8));}
void Put(Bytes& b,std::size_t at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}
void Ref(Bytes& b,std::size_t at,const d::NativePageReference& r){Put(b,at,r.filespace_uuid);Num(b,at+16,8,r.page_number);Num(b,at+24,8,r.page_generation);Put(b,at+32,r.page_size_profile_uuid);}
std::array<byte,32> Hash(const Bytes& b){std::array<byte,32> out{};Check(SHA256(b.data(),b.size(),out.data())!=nullptr,"independent hash");return out;}
void Seal(Bytes& b){std::fill(b.begin()+296,b.begin()+328,0);const auto h=Hash(b);std::copy(h.begin(),h.end(),b.begin()+296);}
template<class Directory> Bytes Oracle(const Directory& value){const auto& h=value.header;Bytes b(h.page_size_bytes,0);
  const bool extended=!value.creator_operation_uuid.is_nil()||std::any_of(value.records.begin(),value.records.end(),[](const auto& r){return r.allocation_root.has_value();});
  const std::size_t width=extended?320:192;
  std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,h.page_type);Num(b,20,2,1);Num(b,22,2,1);
  Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Num(b,88,8,h.flags);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);
  u64 fnv=14695981039346656037ull;for(unsigned i=0;i<128;++i){fnv^=b[i];fnv*=1099511628211ull;}Num(b,96,8,fnv);
  std::copy_n(extended?"SBFDIR02":"SBFDIR01",8,b.begin()+128);Num(b,136,2,extended?2:1);Num(b,138,2,256);Num(b,140,4,384+width*value.records.size());
  Put(b,144,value.object_uuid);Num(b,160,8,value.directory_generation);Put(b,168,value.creator_transaction_uuid);Num(b,184,8,value.creator_local_transaction_id);
  Num(b,192,8,value.total_records);Num(b,200,8,value.first_record);Num(b,208,4,value.records.size());if(value.next)Ref(b,216,*value.next);
  std::copy(value.next_sha256.begin(),value.next_sha256.end(),b.begin()+264);
  if(extended){Num(b,212,4,320);Put(b,328,value.creator_operation_uuid);}
  for(std::size_t i=0;i<value.records.size();++i){const auto at=384+i*width;const auto& r=value.records[i];const auto& a=r.bootstrap;
    Put(b,at,a.filespace_uuid);Put(b,at+16,a.page_size_profile_uuid);Put(b,at+32,a.checksum_profile_uuid);Put(b,at+48,a.encryption_profile_uuid);
    Put(b,at+64,r.locator_uuid);Put(b,at+80,r.page_zero_uuid);Num(b,at+96,8,r.page_zero_generation);Num(b,at+104,8,r.root_set_generation);
    Num(b,at+112,8,r.total_pages);Num(b,at+120,8,r.verification_epoch);Num(b,at+128,2,a.filespace_role);Num(b,at+130,2,a.lifecycle_state);
    Num(b,at+132,4,a.flags);Num(b,at+136,4,a.page_size_bytes);Num(b,at+140,4,a.durable_format_generation);if(r.operation)Ref(b,at+144,*r.operation);
    if(r.allocation_root){const auto& root=*r.allocation_root;Ref(b,at+192,root.page);Put(b,at+240,root.object_uuid);
      std::copy(root.sha256.begin(),root.sha256.end(),b.begin()+at+256);Num(b,at+288,8,root.map_generation);Num(b,at+296,8,root.capacity_generation);}
  }Seal(b);return b;
}
d::FilespacePageZero Zero(unsigned profile,byte fs){d::FilespacePageZero z;auto& b=z.bootstrap;const auto& registered=d::kCanonicalFilespacePageProfiles[profile];
  b.database_uuid=Id(1);b.filespace_uuid=Id(fs);b.page_size_profile_uuid=registered.uuid;b.page_size_bytes=registered.page_size_bytes;
  b.checksum_profile_uuid=d::kNativeBootstrapIntegrityProfile;b.filespace_role=1;b.lifecycle_state=1;
  z.page_uuid=Id(30+fs);z.creation_operation_uuid=Id(40+fs);z.writer_identity_uuid=Id(50+fs);z.page_generation=3;z.root_set_generation=5;z.total_pages=64;
  constexpr unsigned types[]={0,8,5,3,769,9,10,11,5,768};
  for(unsigned k=1;k<=9;++k)z.roots.push_back({static_cast<u16>(k),types[k],Id(fs),10+k,100+k,registered.uuid,Id(60+k)});
  return z;
}
p::NativeFilespaceDirectory Example(unsigned profile=0){auto zero=Zero(profile,2);p::NativeFilespaceDirectory value;
  value.header={zero.bootstrap.page_size_bytes,9,Id(1),Id(2),Id(20),15,105,0,zero.bootstrap.page_size_profile_uuid};
  value.object_uuid=Id(65);value.directory_generation=8;value.creator_transaction_uuid=Id(80);value.creator_local_transaction_id=9;value.total_records=1;
  value.records.push_back({zero.bootstrap,Id(90),zero.page_uuid,zero.page_generation,zero.root_set_generation,zero.total_pages,0,{}});return value;
}
void Empty(const p::NativeFilespaceDirectoryResult& r){Check(!r.ok()&&!r.directory&&r.bytes.empty(),"no failed directory prefix");}
void Empty(const p::NativeFilespaceDirectoryChainResult& r){Check(!r.ok()&&r.pages.empty()&&!r.retained_image_bytes,"no failed chain prefix");}
void Invalid(const p::NativeFilespaceDirectory& v){Empty(p::EncodeNativeFilespaceDirectory(v));Empty(p::DecodeNativeFilespaceDirectory(Oracle(v)));}
void Codecs(){for(unsigned profile=0;profile<5;++profile){auto v=Example(profile);
    for(unsigned role=1;role<=14;++role)for(unsigned state=1;state<=15;++state){v.records[0].bootstrap.filespace_role=role;v.records[0].bootstrap.lifecycle_state=state;
      const auto b=Oracle(v);auto r=p::EncodeNativeFilespaceDirectory(v);Check(r.ok()&&r.bytes==b,"independent all-profile directory encoding");
      r=p::DecodeNativeFilespaceDirectory(b);Check(r.ok()&&Oracle(*r.directory)==b,"independent all-state directory decoding");}
    v=Example(profile);v.records[0].bootstrap.flags=3;v.records[0].bootstrap.encryption_profile_uuid=Id(99);
    v.records[0].operation=d::NativePageReference{Id(2),25,3,v.header.page_size_profile_uuid};
    Check(p::DecodeNativeFilespaceDirectory(Oracle(v)).ok(),"encrypted cluster declaration and operation structurally representable");
  }
  const auto good=Oracle(Example());for(std::size_t i=0;i<good.size();++i){auto b=good;b[i]^=1;Empty(p::DecodeNativeFilespaceDirectory(b));}
  for(unsigned at:{128u,136u,138u,140u,212u,328u,383u,8191u}){auto b=good;b[at]^=1;Seal(b);Empty(p::DecodeNativeFilespaceDirectory(b));}
  for(unsigned change=0;change<22;++change){auto v=Example();auto& r=v.records.front();
    switch(change){case 0:v.header.page_type=5;break;case 1:v.header.flags=1;break;case 2:v.object_uuid={};break;
      case 3:v.creator_transaction_uuid.bytes[6]=0x40;break;case 4:v.directory_generation=0;break;case 5:v.creator_local_transaction_id=0;break;
      case 6:v.total_records=0;break;case 7:v.first_record=1;break;case 8:r.locator_uuid={};break;case 9:r.page_zero_uuid=v.header.page_uuid;break;
      case 10:r.page_zero_generation=0;break;case 11:r.root_set_generation=0;break;case 12:r.total_pages=0;break;
      case 13:r.total_pages=std::numeric_limits<u64>::max();break;case 14:r.bootstrap.filespace_role=15;break;case 15:r.bootstrap.lifecycle_state=0;break;
      case 16:r.bootstrap.flags=4;break;case 17:r.bootstrap.encryption_profile_uuid=Id(8);break;case 18:r.bootstrap.page_size_bytes=4096;break;
      case 19:r.bootstrap.database_uuid=Id(9);Empty(p::EncodeNativeFilespaceDirectory(v));continue;
      case 20:v.total_records=2;v.records.push_back(r);break;case 21:r.operation=d::NativePageReference{Id(2),0,1,v.header.page_size_profile_uuid};break;}
    Invalid(v);
  }
  hash_calls=0;fail_hash=1;Empty(p::DecodeNativeFilespaceDirectory(good));fail_hash=0;
  for(bool encode:{false,true}){count_allocations=true;allocations=0;auto r=encode?p::EncodeNativeFilespaceDirectory(Example()):p::DecodeNativeFilespaceDirectory(good);count_allocations=false;
    Check(r.ok(),"measure codec allocations");const auto total=allocations;
    for(unsigned long n=0;n<total;++n){auto v=Example();allocation_budget=n;r=encode?p::EncodeNativeFilespaceDirectory(v):p::DecodeNativeFilespaceDirectory(good);allocation_budget=-1;
      if(!r.ok())Empty(r);else Check(r.bytes==good,"allocation recovery has exact image");}}
}
void BorrowedCodecs(){
  for(unsigned profile=0;profile<5;++profile)for(unsigned secondary=0;secondary<5;++secondary)for(unsigned variant=0;variant<3;++variant){
    auto value=Example(profile);const auto second=Zero(secondary,3);
    value.records.push_back({second.bootstrap,Id(91),second.page_uuid,second.page_generation,second.root_set_generation,second.total_pages,1,{}});value.total_records=2;
    // Reverse the page-zero UUID order independently of sorted filespace IDs.
    std::swap(value.records[0].page_zero_uuid,value.records[1].page_zero_uuid);
    if(variant==1){value.creator_transaction_uuid={};value.creator_local_transaction_id=0;value.creator_operation_uuid=Id(98);}
    if(variant==2)for(auto& r:value.records){p::NativeFilespaceAllocationRoot root;
      root.page={r.bootstrap.filespace_uuid,17,5,r.bootstrap.page_size_profile_uuid};root.object_uuid=Id(100);root.sha256.fill(0x71);
      root.map_generation=9;root.capacity_generation=11;r.allocation_root=root;}
    auto bytes=Oracle(value);std::vector<p::NativeFilespaceDirectoryRecord> records(value.records.size());std::vector<Uuid> scratch(records.size());
    allocations=0;count_allocations=true;allocation_budget=0;
    const auto result=p::DecodeNativeFilespaceDirectoryInto(bytes,records,scratch);
    const auto remaining=allocation_budget;allocation_budget=-1;count_allocations=false;
    Check(result.ok()&&remaining==0&&allocations==0,"caller-backed directory has no hidden vectors or UUID tree allocations");
    Check(result.directory->records.data()==records.data()&&Oracle(*result.directory)==bytes,"full independent v1/v2 mixed-profile metadata and unchanged record order");
    Check(scratch[0]<scratch[1]&&records[0].bootstrap.filespace_uuid==value.records[0].bootstrap.filespace_uuid,"binary UUID scratch sorting does not reorder directory records");
    for(unsigned part=0;part<2;++part){const auto r=p::DecodeNativeFilespaceDirectoryInto(bytes,
      std::span(records).first(records.size()-(part==0)),std::span(scratch).first(scratch.size()-(part==1)));
      Check(!r.directory&&r.error==E::resource_exhausted,"short record or UUID scratch publishes no directory prefix");}
    auto invalid=p::DecodeNativeFilespaceDirectoryInto(bytes,records,{reinterpret_cast<Uuid*>(records.data()),scratch.size()});
    Check(!invalid.directory&&invalid.error==E::invalid_backing,"overlapping output metadata is refused before writes");
    invalid=p::DecodeNativeFilespaceDirectoryInto(bytes,records,{reinterpret_cast<Uuid*>(bytes.data()),scratch.size()});
    Check(!invalid.directory&&invalid.error==E::invalid_backing&&bytes==Oracle(value),"overlap with encoded image cannot change its bytes");
    Bytes unaligned(1,0);unaligned.insert(unaligned.end(),bytes.begin(),bytes.end());
    Check(p::DecodeNativeFilespaceDirectoryInto(std::span<const byte>(unaligned).subspan(1),records,scratch).ok(),"unaligned directory image decoded bytewise");
    auto duplicate=value;duplicate.records[1].page_zero_uuid=duplicate.records[0].page_zero_uuid;
    auto duplicate_result=p::DecodeNativeFilespaceDirectoryInto(Oracle(duplicate),records,scratch);
    Check(!duplicate_result.directory&&duplicate_result.error==E::invalid_record,"duplicate binary page-zero UUID still refused");
    for(unsigned at:{128u,136u,138u,140u,208u,383u}){auto bad=bytes;bad[at]^=0x80;Seal(bad);
      const auto owned=p::DecodeNativeFilespaceDirectory(bad);const auto borrowed=p::DecodeNativeFilespaceDirectoryInto(bad,records,scratch);
      Check(!borrowed.directory&&borrowed.error==owned.error,"resealed family corruption retains shared refusal classification");}
    hash_calls=0;fail_hash=1;const auto hash_failure=p::DecodeNativeFilespaceDirectoryInto(bytes,records,scratch);fail_hash=0;
    Check(!hash_failure.directory&&hash_failure.error==E::hash_failure,"real directory digest failure preserved");
    Check(p::DecodeNativeFilespaceDirectoryInto(bytes,records,scratch).ok(),"valid retry restores complete decoded fields");
    std::fill(bytes.begin(),bytes.end(),0);std::fill(scratch.begin(),scratch.end(),Uuid{});
    Check(Oracle(*result.directory)==Oracle(value),"decoded records retain neither encoded image nor scratch lifetime");
  }
  for(unsigned profile=0;profile<5;++profile){auto value=Example(profile);const auto count=(value.header.page_size_bytes-384)/192;
    value.records.resize(count,value.records.front());value.total_records=count;
    const auto id=[](u32 n){auto r=Id(0);for(unsigned i=0;i<4;++i)r.bytes[15-i]=byte(n>>(8*i));return r;};
    for(unsigned i=0;i<count;++i){auto& r=value.records[i];r.bootstrap.filespace_uuid=id(1000+i);r.page_zero_uuid=id(10000+i);r.locator_uuid=id(20000+i);}
    auto bytes=Oracle(value);std::vector<p::NativeFilespaceDirectoryRecord> records(count);std::vector<Uuid> scratch(count);
    allocation_budget=0;auto result=p::DecodeNativeFilespaceDirectoryInto(bytes,records,scratch);const auto remaining=allocation_budget;allocation_budget=-1;
    Check(result.ok()&&remaining==0&&result.directory->records.size()==count&&Oracle(*result.directory)==bytes,"maximum-count directory uses exact admitted metadata and scratch");
  }
  const auto good=Oracle(Example());std::array<p::NativeFilespaceDirectoryRecord,1> records;std::array<Uuid,1> scratch;
  for(std::size_t i=0;i<good.size();++i){auto bad=good;bad[i]^=1;auto r=p::DecodeNativeFilespaceDirectoryInto(bad,records,scratch);Check(!r.directory&&!r.ok(),"every corrupted byte refuses without borrowed prefix");}
  for(unsigned size:{0u,127u,383u,8191u,8193u}){auto bad=good;bad.resize(size);
    const auto r=p::DecodeNativeFilespaceDirectoryInto(bad,records,scratch);Check(!r.directory&&r.error==p::DecodeNativeFilespaceDirectory(bad).error,"all invalid directory sizes preserve owning refusal");}
}
#ifdef SB_NATIVE_DIRECTORY_MEMORY_TESTS
namespace db=scratchbird::storage::database;
namespace m=scratchbird::core::memory;
using ME=db::NativeDirectoryMemoryError;
struct MemoryFixture {
  m::MemoryManager manager;
  m::HierarchicalMemoryBudgetLedger ledger{3,5};
  db::NativeStorageMemoryBinding binding{Id(1),Id(61),Id(62),Id(63)};
  db::NativeStorageMemory memory;
  static auto Policy(){auto p=m::DefaultLocalEngineMemoryPolicy();p.hard_limit_bytes=2097152;p.per_context_limit_bytes=2097152;return p;}
  explicit MemoryFixture(u64 bytes):manager(Policy()){
    m::ReservationBackedMemoryResourceRequest r;r.memory_manager=&manager;r.reservation_ledger=&ledger;
    r.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.category=m::MemoryCategory::page_buffer;r.requested_bytes=bytes;r.memory_class="page_buffer";
    r.route_label="storage.directory.conformance";r.purpose="actual directory image and metadata";
    r.binary_operation_uuid=binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
    r.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(64).bytes},
      {m::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
    r.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label="directory resource conformance";
    for(const auto& scope:r.scope_chain){m::HierarchicalMemoryBudget b;b.scope=scope;b.hard_limit_bytes=bytes;
      b.provenance=r.provenance;Check(ledger.SetBudget(b).ok(),"actual parent budget");}
    auto grant=m::AcquireReservationBackedMemoryResource(r);Check(grant.ok(),"actual node-issued metadata grant");
    auto adopted=db::AdoptNativeStorageMemory(binding,grant.resource);Check(adopted.ok()&&!grant.resource,"exclusive native adoption");
    memory=std::move(adopted.memory);
  }
  void Empty(){const auto s=manager.Snapshot();Check(!s.current_bytes&&!s.reserved_capacity_bytes&&
    !s.active_capacity_reservation_count&&!ledger.Snapshot().current_bytes,"all real directory charges released");}
};
struct MemoryFile {
  std::filesystem::path directory,path;
  d::FileDevice device;
  p::NativeFilespaceDirectory value;
  Bytes bytes;
  d::NativeCommonPageHeaderBinding expected;
  explicit MemoryFile(unsigned profile):value(Example(profile)),bytes(Oracle(value)){
    char name[]="/tmp/sb-directory-memory-XXXXXX";const auto* made=mkdtemp(name);if(!made)throw std::runtime_error("mkdtemp");
    directory=made;path=directory/"native.bin";
    Check(device.Open(path.string(),d::FileOpenMode::create_new).ok(),"actual directory source");
    expected={{value.header.database_uuid,value.header.filespace_uuid,value.header.page_size_profile_uuid},
      value.header.page_number,value.header.page_generation,9,value.header.page_uuid};
    Bootstrap();Store(bytes);Check(device.Sync().ok()&&device.Close().ok()&&
      device.Open(path.string(),d::FileOpenMode::open_existing).ok(),"cold reopen independent directory image");
  }
  ~MemoryFile(){(void)device.Close();std::error_code ec;std::filesystem::remove_all(directory,ec);}
  void Bootstrap(u32 flags=0,Uuid database=Id(1)){
    Bytes b(4096,0);std::copy_n("SBFP",4,b.begin());Num(b,4,2,1);Num(b,6,2,4096);
    Num(b,8,4,value.header.page_size_bytes);Num(b,12,4,flags);Put(b,16,database);
    Put(b,32,value.header.filespace_uuid);Put(b,48,value.header.page_size_profile_uuid);
    Num(b,64,4,1);Num(b,68,2,1);Num(b,70,2,1);Put(b,72,d::kNativeBootstrapIntegrityProfile);
    if(flags&1)Put(b,88,Id(71));std::array<byte,32> digest{};
    Check(SHA256(b.data(),104,digest.data())!=nullptr,"independent bootstrap digest");
    std::copy(digest.begin(),digest.end(),b.begin()+104);
    Check(device.WriteAt(0,b.data(),b.size()).ok()&&device.Sync().ok(),"actual independent bootstrap");
  }
  void Store(const Bytes& image){Check(device.WriteAt(value.header.page_number*u64(value.header.page_size_bytes),image.data(),image.size()).ok()&&device.Sync().ok(),"actual independent directory");}
  auto Read(MemoryFixture& f){reads=hash_calls=0;return db::ReadNativeFilespaceDirectoryWithMemoryFromOpenDevice(device,expected,value.object_uuid,f.memory,f.binding);}
};
void NoDirectory(const db::NativeDirectoryMemoryResult& r){Check(!r.ok()&&!r.directory&&r.image.empty()&&!r.arena,"refusal exposes no image metadata or owner prefix");}
void MemoryTests(){
  for(unsigned profile=0;profile<5;++profile){MemoryFile file(profile);
    const auto capacity=db::NativeDirectoryWorkspaceBytes(file.value.header.page_size_profile_uuid);
    Check(capacity==file.bytes.size()+((file.bytes.size()-384)/192)*(sizeof(p::NativeFilespaceDirectoryRecord)+sizeof(Uuid))+2*(alignof(std::max_align_t)-1),"independent native backing size formula");
    {
      MemoryFixture f(capacity);
      {auto guard=file.device.AcquireOperationGuard();allocation_device_mutex=guard.mutex();}
      allocation_lock_free=false;auto read=file.Read(f);
      Check(read.ok()&&allocation_lock_free,"real metadata backing allocated outside device guard");
      Check(last_read_buffer==read.image.data()&&read.image.size()==file.bytes.size()&&
        std::equal(read.image.begin(),read.image.end(),file.bytes.begin(),file.bytes.end()),"actual read destination is returned charged image");
      const auto begin=reinterpret_cast<std::uintptr_t>(read.image.data());
      const auto inside=[&](const void* ptr,usize bytes){auto n=reinterpret_cast<std::uintptr_t>(ptr);return n>=begin&&n-begin<=capacity&&bytes<=capacity-(n-begin);};
      Check(inside(read.directory->records.data(),read.directory->records.size_bytes())&&
        Oracle(*read.directory)==file.bytes,"actual metadata resides in same charged block with exact binary records");
      Check(f.manager.Snapshot().current_bytes==capacity&&f.memory.Snapshot().allocated_bytes==capacity&&
        f.ledger.Snapshot().current_bytes==capacity&&read.arena.Snapshot().retained_bytes==capacity,"actual physical parent and arena charges agree");
      auto full=file.Read(f);NoDirectory(full);Check(full.error==ME::memory_allocation_failure&&reads==0,"simultaneous live image prevents uncharged second reader");
      const auto revoked=f.ledger.CleanupOwner(f.binding.owner_uuid.bytes);Check(revoked.retained_bytes==capacity,"revocation retains actual image and metadata");
      auto denied=file.Read(f);NoDirectory(denied);Check(denied.error==ME::memory_binding_failure&&reads==0,"revoked grant prevents new source reads");
      f.memory={};Check(f.manager.Snapshot().current_bytes==capacity,"returned metadata owner survives caller workspace");
      std::thread worker([retained=std::move(read)]()mutable{allocation_budget=0;retained={};if(allocation_budget!=0)std::abort();allocation_budget=-1;});worker.join();f.Empty();
    }
    MemoryFixture valid(capacity);const auto no_writes=writes,no_syncs=syncs;
    const auto original_binding=file.expected;
    for(unsigned invalid=0;invalid<7;++invalid){
      if(invalid==0)file.expected.page_number=0;
      if(invalid==1)file.expected.page_number=std::numeric_limits<u64>::max();
      if(invalid==2)file.expected.page_generation=0;
      if(invalid==3)file.expected.page_type=0x30e;
      if(invalid==4)file.expected.filespace.filespace_uuid={};
      if(invalid==5)file.expected.filespace.page_size_profile_uuid=Id(99);
      if(invalid==6)file.expected.page_uuid=Uuid{};
      auto r=file.Read(valid);NoDirectory(r);Check(r.error==ME::invalid_request&&reads==0&&
        !valid.memory.Snapshot().allocation_count,"invalid typed page binding refuses before physical admission");
      file.expected=original_binding;
    }
    for(unsigned field=0;field<4;++field){auto wrong=valid.binding;
      const std::array<Uuid*,4> fields{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};*fields[field]=Id(99);
      reads=0;auto denied=db::ReadNativeFilespaceDirectoryWithMemoryFromOpenDevice(file.device,file.expected,file.value.object_uuid,valid.memory,wrong);
      NoDirectory(denied);Check(denied.error==ME::memory_binding_failure&&!reads&&!valid.manager.Snapshot().current_bytes,"exact binary memory identity before allocation or I/O");}
    MemoryFixture small(capacity-1);auto short_grant=file.Read(small);NoDirectory(short_grant);
    Check(short_grant.error==ME::memory_allocation_failure&&!reads&&!small.manager.Snapshot().current_bytes,"one byte short cannot obtain image or metadata");small.memory={};small.Empty();
    file.expected.page_generation++;{auto r=file.Read(valid);NoDirectory(r);Check(r.error==ME::header_failure,"stale generation binds actual source");}file.expected.page_generation--;
    auto object=db::ReadNativeFilespaceDirectoryWithMemoryFromOpenDevice(file.device,file.expected,Id(99),valid.memory,valid.binding);NoDirectory(object);Check(object.error==ME::object_mismatch,"exact directory object identity");
    for(unsigned n=1;n<=2;++n){fail_read=n;auto r=file.Read(valid);fail_read=0;NoDirectory(r);
      Check(reads==n&&!valid.manager.Snapshot().current_bytes,"every real read failure releases admitted payload");}
    short_read=2;{auto r=file.Read(valid);Check(r.ok()&&reads==3,"legal short physical read completes");}short_read=0;
    short_read=2;eof_read=3;{auto r=file.Read(valid);NoDirectory(r);Check(r.error==ME::io_failure&&r.page_bytes_read==file.bytes.size()-1,"partial then EOF retains actual read progress only");}short_read=eof_read=0;
    for(unsigned fault=1;fault<=5;++fault){method_fault=fault;auto r=file.Read(valid);NoDirectory(r);Check(method_fault==0,"each real digest-provider failure reached");}
    fail_hash=1;{auto r=file.Read(valid);fail_hash=0;NoDirectory(r);Check(r.directory_error==E::hash_failure,"actual digest context failure typed");}
    Check(writes==no_writes&&syncs==no_syncs,"reader and refusal paths never write or sync source");
    auto corrupt=file.bytes;corrupt.back()^=1;file.Store(corrupt);
    {auto guard=file.device.AcquireOperationGuard();deallocation_device_mutex=guard.mutex();}deallocation_lock_free=false;
    {auto r=file.Read(valid);NoDirectory(r);Check(r.error==ME::directory_failure&&deallocation_lock_free,"failed image cleanup occurs after releasing device guard");}file.Store(file.bytes);
    file.Bootstrap(0,Id(99));{auto r=file.Read(valid);NoDirectory(r);Check(r.error==ME::bootstrap_failure,"actual bootstrap identity mismatch");}file.Bootstrap(1);
    {auto r=file.Read(valid);Check(r.ok(),"directory metadata remains cleartext in encrypted filespace");}file.Bootstrap();
    for(unsigned version=0;version<3;++version){auto variant=file.value;
      if(version==0){variant.creator_transaction_uuid={};variant.creator_local_transaction_id=0;variant.creator_operation_uuid=Id(98);}
      if(version==1){for(auto& record:variant.records){p::NativeFilespaceAllocationRoot root;
        root.page={record.bootstrap.filespace_uuid,17,5,record.bootstrap.page_size_profile_uuid};root.object_uuid=Id(100);
        root.sha256.fill(0x71);root.map_generation=9;root.capacity_generation=11;record.allocation_root=root;}}
      if(version==2){const auto count=(file.bytes.size()-384)/192;variant.records.resize(count,variant.records.front());variant.total_records=count;
        const auto id=[](u32 n){auto result=Id(0);for(unsigned i=0;i<4;++i)result.bytes[15-i]=byte(n>>(8*i));return result;};
        for(unsigned i=0;i<count;++i){auto& record=variant.records[i];record.bootstrap.filespace_uuid=id(1000+i);record.page_zero_uuid=id(10000+i);record.locator_uuid=id(20000+i);}}
      const auto encoded=Oracle(variant);file.Store(encoded);
      {auto r=file.Read(valid);Check(r.ok()&&Oracle(*r.directory)==encoded,"actual governed directory supports operation lineage allocation roots and maximum record count");}
    }
    for(unsigned member_profile=0;member_profile<5;++member_profile){
      auto variant=file.value;const auto member=Zero(member_profile,3);
      variant.records.push_back({member.bootstrap,Id(91),member.page_uuid,member.page_generation,
        member.root_set_generation,member.total_pages,0,{}});variant.total_records=2;
      // Independently sorted filespace identities and reverse UUID scratch order.
      std::swap(variant.records[0].page_zero_uuid,variant.records[1].page_zero_uuid);
      for(bool extended:{false,true}){
        if(extended){variant.creator_transaction_uuid={};variant.creator_local_transaction_id=0;variant.creator_operation_uuid=Id(98);}
        const auto encoded=Oracle(variant);file.Store(encoded);
        auto r=file.Read(valid);Check(r.ok()&&Oracle(*r.directory)==encoded,
          "actual governed directory retains mixed-profile records across all 25 pairs and both versions");
      }
    }
    file.Store(file.bytes);
    if(!profile){unsigned long sites=0;
      {allocations=0;count_allocations=true;auto r=file.Read(valid);count_allocations=false;sites=allocations;Check(r.ok(),"measure full admitted reader allocation sites");}
      for(unsigned long n=0;n<sites;++n){allocation_budget=n;auto r=file.Read(valid);allocation_budget=-1;
        if(r.ok())Check(Oracle(*r.directory)==file.bytes,"optional telemetry loss cannot change decoded result");
        else NoDirectory(r);
        r={};Check(!valid.manager.Snapshot().current_bytes&&!valid.memory.Snapshot().allocated_bytes,"every allocation fault retains zero physical payload after cleanup");
        {auto retry=file.Read(valid);Check(retry.ok(),"same-owner retry after each allocation failure");}
      }
      Check(sites>0,"allocation sweep executed");std::cout<<"governed directory metadata faults="<<sites<<'\n';
    }
    Check(file.device.Close().ok(),"close source");auto closed=file.Read(valid);NoDirectory(closed);
    Check(closed.error==ME::bootstrap_failure&&closed.bootstrap_error==d::FilespaceBootstrapError::device_not_open,"reader never reopens closed source");
    valid.memory={};valid.Empty();
  }
}
#endif
struct Fixture{std::filesystem::path root;Fixture(){char pattern[]="/tmp/sb-directory-test-XXXXXX";const auto p=mkdtemp(pattern);Check(p,"owned temporary fixture");root=p;}
  ~Fixture(){std::error_code e;std::filesystem::remove_all(root,e);}};
void Historical(d::FileDevice& first,d::FileDevice& second,const d::FilespacePageZero& z1,
                const d::FilespacePageZero& z2,const Bytes& head,const Bytes& tail) {
  const auto zero1=d::EncodeFilespacePageZero(z1),zero2=d::EncodeFilespacePageZero(z2);
  Check(zero1.ok()&&zero2.ok(),"historical complete page-zero images");
  const std::vector<d::NativeFilespaceDevice> devices{{Id(3),z2.bootstrap.page_size_profile_uuid,&second},{Id(2),z1.bootstrap.page_size_profile_uuid,&first}};
  const std::vector<p::NativeHistoricalFilespaceImage> history{{Id(2),*zero1.bytes},{Id(3),*zero2.bytes}};
  const d::FilespaceRootReference ref{5,9,Id(2),15,105,z1.bootstrap.page_size_profile_uuid,Id(65)};
  const auto digest=Hash(head);const u64 limit=2*(head.size()+tail.size())+8192;
  const auto read=[&](u64 budget){return p::ReadNativeFilespaceDirectoryAtHistoricalRootFromOpenDevices(Id(1),devices,ref,digest,history,budget);};
  const auto verify=[&](const p::NativeFilespaceDirectoryChainResult& r){
    Check(r.ok()&&r.pages.size()==2&&r.retained_image_bytes==limit&&r.pages[0].bytes==head&&r.pages[1].bytes==tail,
          "historical whole directory image oracle and bounded accounting");};
  const auto file_hash=[&](d::FileDevice& device){const auto size=device.Size();Check(size.ok(),"fixture size");Bytes b(size.size_bytes);
    const auto io=device.ReadAt(0,b.data(),b.size());Check(io.ok()&&io.bytes_transferred==b.size(),"whole fixture read");return Hash(b);};
  const auto original1=file_hash(first),original2=file_hash(second);
  const auto initial_writes=writes,initial_syncs=syncs;
  reads=hash_calls=stats=0;allocations=0;count_allocations=true;auto good=read(limit);count_allocations=false;
  const auto nr=reads,nh=hash_calls,ns=stats;const auto na=allocations;verify(good);
#ifdef NATIVE_HISTORICAL_IO_FAULTS
  Check(ns==4,"both initial and final actual member sizes measured");
#endif
  for(const u64 budget:{u64{0},u64{head.size()},limit-1}){const auto r=read(budget);Empty(r);Check(r.error==E::resource_exhausted,"historical image budget refusal");}
  for(unsigned at=1;at<=nr;++at){reads=0;fail_read=at;const auto r=read(limit);fail_read=0;Empty(r);Check(r.error==E::io_failure,"historical I/O failure category");}
  for(unsigned at=1;at<=nh;++at){hash_calls=0;fail_hash=at;const auto r=read(limit);fail_hash=0;Empty(r);Check(r.error==E::hash_failure,"historical digest failure category");}
  for(unsigned at=1;at<=ns;++at){stats=0;fail_stat=at;const auto r=read(limit);fail_stat=0;Empty(r);Check(r.error==E::io_failure,"historical size failure category");}
  for(unsigned long at=0;at<na;++at){const auto lost=first.failed_io_latency_observations()+second.failed_io_latency_observations();
    allocation_budget=at;const auto r=read(limit);allocation_budget=-1;
    if(r.ok()){Check(first.failed_io_latency_observations()+second.failed_io_latency_observations()>lost,"only measured optional telemetry allocation may fail");verify(r);}
    else{Empty(r);Check(r.error==E::resource_exhausted,"historical allocation failure category");}}
  for(unsigned mutation=0;mutation<12;++mutation){auto retained=history;auto sha=digest;auto root=ref;auto files=devices;
    if(mutation==0)sha[0]^=1;if(mutation==1)sha={};if(mutation==2)retained.pop_back();
    if(mutation==3)retained.push_back(retained.front());if(mutation==4)retained[1]=retained[0];
    if(mutation==5)retained[1].filespace_uuid=Id(4);if(mutation==6)retained[0].page_zero[5000]^=1;
    if(mutation==7)root.page_generation++;if(mutation==8)files[0].device=files[1].device;
    if(mutation>=9){auto changed=z2;if(mutation==9)changed.page_generation++;
      if(mutation==10)changed.total_pages--;if(mutation==11)changed.bootstrap.database_uuid=Id(99);
      const auto b=d::EncodeFilespacePageZero(changed);Check(b.ok(),"valid historical binding negative");retained[1].page_zero=*b.bytes;}
    const auto r=p::ReadNativeFilespaceDirectoryAtHistoricalRootFromOpenDevices(Id(1),files,root,sha,retained,limit);Empty(r);
    if(mutation==0)Check(r.error==E::invalid_integrity,"external directory root digest cannot be substituted");
  }
  Check(file_hash(first)==original1&&file_hash(second)==original2&&writes==initial_writes&&syncs==initial_syncs,
        "historical success and fault paths preserve all bytes without writes or syncs");
  auto reversed=history;std::reverse(reversed.begin(),reversed.end());
  verify(p::ReadNativeFilespaceDirectoryAtHistoricalRootFromOpenDevices(Id(1),devices,ref,digest,reversed,limit));
  const u64 length1=z1.total_pages*head.size(),length2=z2.total_pages*tail.size();
  for(unsigned member=0;member<2;++member){auto& device=member?second:first;const auto length=member?length2:length1;const auto size=member?tail.size():head.size();
    for(u64 extra:{u64{1},u64{size}}){std::filesystem::resize_file(device.path(),length+extra);verify(read(limit));
      Empty(p::ReadNativeFilespaceDirectoryFromOpenDevices(Id(1),devices,ref,head.size()+tail.size()));}
    auto latest=member?z2:z1;latest.total_pages++;latest.page_generation++;latest.root_set_generation++;
    const auto encoded=d::EncodeFilespacePageZero(latest);Check(encoded.ok(),"newer current member metadata");
    Check(device.WriteAt(0,encoded.bytes->data(),encoded.bytes->size()).ok()&&device.Sync().ok(),"persist newer metadata");
    Check(d::ReadFilespacePageZeroFromOpenDevice(device).ok(),"newer current member independently valid");verify(read(limit));
    Empty(p::ReadNativeFilespaceDirectoryFromOpenDevices(Id(1),devices,ref,head.size()+tail.size()));
    latest.bootstrap.database_uuid=Id(99);const auto misbound=d::EncodeFilespacePageZero(latest);
    Check(misbound.ok()&&device.WriteAt(0,misbound.bytes->data(),misbound.bytes->size()).ok(),"actual misbound bootstrap fixture");
    const auto wrong_actual=read(limit);Empty(wrong_actual);Check(wrong_actual.error==E::invalid_filespace,"actual member identity checked independently of retained image");
    const auto& original=member?*zero2.bytes:*zero1.bytes;Check(device.WriteAt(0,original.data(),original.size()).ok(),"restore original metadata");
    std::filesystem::resize_file(device.path(),length-1);const auto short_file=read(limit);Empty(short_file);
    Check(short_file.error==E::invalid_filespace,"short historical member refused even when directory pages remain readable");
    std::filesystem::resize_file(device.path(),length);
    reads=0;resized=false;resize_at_read=member?4:3;resize_to=length+1;
    const auto raced=read(limit);resize_at_read=0;Empty(raced);
    Check(resized&&raced.error==E::physical_extent_changed,"historical directory notices either member length changing");
    std::filesystem::resize_file(device.path(),length);
  }
  Check(file_hash(first)==original1&&file_hash(second)==original2,"exact restored historical files");
  const auto path1=first.path(),path2=second.path();
  Check(first.Close().ok()&&second.Close().ok()&&first.Open(path1,d::FileOpenMode::open_existing_read_only).ok()&&
        second.Open(path2,d::FileOpenMode::open_existing_read_only).ok(),"historical readonly reopen");
  verify(read(limit));Check(first.read_only()&&second.read_only(),"historical reads preserve readonly handles");
  {std::lock_guard lock(pause_mutex);pause_next=true;entered=false;released=false;}
  auto reading=std::async(std::launch::async,[&]{return read(limit);});
  struct ReleasePause {~ReleasePause(){std::lock_guard lock(pause_mutex);released=true;pause_cv.notify_all();}} release_pause;
  {std::unique_lock lock(pause_mutex);Check(pause_cv.wait_for(lock,std::chrono::seconds(5),[]{return entered;}),"historical directory reached actual read");}
  std::promise<void> close_started;auto started=close_started.get_future();
  auto closing=std::async(std::launch::async,[&]{close_started.set_value();return second.Close();});
  started.wait();const bool held=closing.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout;
  {std::lock_guard lock(pause_mutex);released=true;pause_cv.notify_all();}
  const auto completed=reading.get();const auto closed=closing.get();
  Check(held&&closed.ok(),"other member close waits while first member is being verified");verify(completed);
  Check(first.Close().ok()&&first.Open(path1,d::FileOpenMode::open_existing).ok()&&
        second.Open(path2,d::FileOpenMode::open_existing).ok(),"restore fixture ownership");
  std::cout<<"historical directory profiles="<<head.size()<<'/'<<tail.size()<<" reads="<<nr<<" hashes="<<nh<<" stats="<<ns<<" allocations="<<na<<'\n';
}
void Chains(){for(unsigned profile=0;profile<5;++profile)for(unsigned secondary=0;secondary<5;++secondary){Fixture fixture;d::FileDevice first,second;auto z1=Zero(profile,2),z2=Zero(secondary,3);
    const auto path1=(fixture.root/"first").string(),path2=(fixture.root/"second").string();Check(first.Open(path1,d::FileOpenMode::create_new).ok()&&second.Open(path2,d::FileOpenMode::create_new).ok(),"owned mixed-profile filespaces");
    const auto prepare=[&](auto& device,const auto& z){const auto b=d::EncodeFilespacePageZero(z);Check(b.ok(),"page-zero fixture encoding");const byte pad=0;
      Check(device.WriteAt(z.total_pages*z.bootstrap.page_size_bytes-1,&pad,1).ok()&&device.WriteAt(0,b.bytes->data(),b.bytes->size()).ok()&&device.Sync().ok(),"actual filespace header and capacity");};prepare(first,z1);prepare(second,z2);
    auto head=Example(profile),tail=Example(secondary);tail.header.filespace_uuid=Id(3);tail.header.page_uuid=Id(21);
    tail.records={{z2.bootstrap,Id(91),z2.page_uuid,z2.page_generation,z2.root_set_generation,z2.total_pages,4,{}}};tail.total_records=2;tail.first_record=1;
    head.total_records=2;head.next=d::NativePageReference{Id(3),15,105,z2.bootstrap.page_size_profile_uuid};head.next_sha256=Hash(Oracle(tail));
    const auto put=[&](auto& device,const auto& image){const auto b=Oracle(image);const auto io=device.WriteAt(image.header.page_number*image.header.page_size_bytes,b.data(),b.size());Check(io.ok()&&io.bytes_transferred==b.size()&&device.Sync().ok(),"actual directory page persistence");};
    const auto persist=[&](auto h,const auto& t,bool reseal=true){if(reseal)h.next_sha256=Hash(Oracle(t));put(first,h);put(second,t);};persist(head,tail);
    const std::vector<d::NativeFilespaceDevice> devices{{Id(3),z2.bootstrap.page_size_profile_uuid,&second},{Id(2),z1.bootstrap.page_size_profile_uuid,&first}};
    const d::FilespaceRootReference ref{5,9,Id(2),15,105,z1.bootstrap.page_size_profile_uuid,Id(65)};const u64 limit=z1.bootstrap.page_size_bytes+z2.bootstrap.page_size_bytes;
    const auto read=[&](u64 budget){return p::ReadNativeFilespaceDirectoryFromOpenDevices(Id(1),devices,ref,budget);};
    reads=hash_calls=0;allocations=0;count_allocations=true;auto r=read(limit);count_allocations=false;const auto nr=reads,nh=hash_calls;const auto na=allocations;
    Check(r.ok()&&r.pages.size()==2&&r.retained_image_bytes==limit&&r.pages[0].bytes==Oracle(head)&&r.pages[1].bytes==Oracle(tail),"actual complete directory and headers");Empty(read(limit-1));
    for(unsigned fault=1;fault<=nr;++fault){reads=0;fail_read=fault;r=read(limit);fail_read=0;Empty(r);}
    for(unsigned fault=1;fault<=nh;++fault){hash_calls=0;fail_hash=fault;r=read(limit);fail_hash=0;Empty(r);Check(r.error==E::hash_failure,"exact nested directory hash failure");}
    if(profile==0){for(unsigned long n=0;n<=na;++n){allocation_budget=n;r=read(limit);allocation_budget=-1;
        if(r.ok())Check(r.pages[0].bytes==Oracle(head)&&r.pages[1].bytes==Oracle(tail),"allocation sweep valid complete chain");else Empty(r);
        if(n==na)Check(r.ok(),"allocation sweep reaches measured terminal success");}std::cout<<"directory allocation sites="<<na<<'\n';}
    for(unsigned change=0;change<16;++change){auto t=tail;auto h=head;switch(change){
      case 0:t.directory_generation++;break;case 1:t.creator_transaction_uuid=Id(82);break;case 2:t.creator_local_transaction_id++;break;
      case 3:t.records[0].page_zero_generation++;break;case 4:t.records[0].root_set_generation++;break;case 5:t.records[0].total_pages++;break;
      case 6:t.records[0].bootstrap.lifecycle_state=2;break;case 7:t.records[0].bootstrap.filespace_role=2;break;
      case 8:t.records[0].page_zero_uuid=Id(34);break;case 9:t.records[0].bootstrap.filespace_uuid=Id(4);break;
      case 10:t.records[0].page_zero_uuid=h.records[0].page_zero_uuid;break;case 11:t.header.page_uuid=h.header.page_uuid;break;
      case 12:t.records[0].operation=d::NativePageReference{Id(2),64,3,z1.bootstrap.page_size_profile_uuid};break;
      case 13:t.records[0].bootstrap.filespace_uuid=Id(2);break;
      case 14:t.header.database_uuid=Id(5);t.records[0].bootstrap.database_uuid=Id(5);break;
      case 15:t.header.page_generation++;break;}
      Check(p::DecodeNativeFilespaceDirectory(Oracle(t)).ok(),"wrong chain fact independently valid image");persist(h,t);Empty(read(limit));}
    auto t=tail;t.records[0].verification_epoch++;persist(head,t,false);r=read(limit);Empty(r);Check(r.error==E::invalid_integrity,"exact successor hash binding");
    auto h=head;t=tail;h.total_records=t.total_records=3;
    t.next=d::NativePageReference{Id(2),15,105,z1.bootstrap.page_size_profile_uuid};t.next_sha256.fill(1);
    persist(h,t);r=read(limit+z1.bootstrap.page_size_bytes);Empty(r);Check(r.error==E::chain_mismatch,"actual continuation cycle rejected");
    t.first_record=0;persist(h,t);r=read(limit);Empty(r);Check(r.error==E::chain_mismatch,"actual discontinuous ordinal rejected");
    h=head;t=tail;h.total_records=t.total_records=3;
    auto closed=t.records[0];closed.bootstrap.filespace_uuid=Id(4);closed.bootstrap.filespace_role=12;closed.bootstrap.lifecycle_state=3;
    closed.page_zero_uuid=Id(34);closed.locator_uuid=Id(92);t.records.push_back(closed);
    persist(h,t);r=read(limit);Check(r.ok()&&r.pages.back().directory->records.size()==2,"closed directory entry retained without opening locator");
    for(const auto& alias:{head.records[0].page_zero_uuid,head.header.page_uuid}){
      auto duplicate_zero=t;duplicate_zero.records.back().page_zero_uuid=alias;
      Check(p::DecodeNativeFilespaceDirectory(Oracle(duplicate_zero)).ok(),"cross-image closed alias is individually valid");
      persist(h,duplicate_zero);r=read(limit);Empty(r);Check(r.error==E::chain_mismatch,"closed entry cannot alias retained page or page-zero identity");
    }
    auto missing=devices;missing.erase(missing.begin());Empty(p::ReadNativeFilespaceDirectoryFromOpenDevices(Id(1),missing,ref,limit));
    auto duplicate=devices;duplicate.push_back(devices.front());Empty(p::ReadNativeFilespaceDirectoryFromOpenDevices(Id(1),duplicate,ref,limit));
    persist(head,tail);Historical(first,second,z1,z2,Oracle(head),Oracle(tail));
    Check(first.Close().ok()&&second.Close().ok()&&first.Open(path1,d::FileOpenMode::open_existing_read_only).ok()&&second.Open(path2,d::FileOpenMode::open_existing_read_only).ok(),"reopen owned read-only filespaces");
    Check(read(limit).ok()&&first.read_only()&&second.read_only(),"directory read-only reopen");
  }}
}
int main(){try{Codecs();BorrowedCodecs();Chains();
#ifdef SB_NATIVE_DIRECTORY_MEMORY_TESTS
    MemoryTests();
#endif
    std::cout<<"PASS directory checks="<<checks<<" not_SQL_E2E=true\n";return 0;}
  catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}

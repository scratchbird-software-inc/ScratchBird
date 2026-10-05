// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_control_bundle.hpp"
#include "native_management_control_bundle_memory.hpp"
#include "physical_mga_cow_store.hpp"
#include "native_management_control_allocation.hpp"
#include "native_control_allocation_backing.hpp"
#include "native_metadata_memory.hpp"
#include "native_management_history.hpp"
#include "native_management_history_memory.hpp"
#include "native_management_control_authority_memory.hpp"
#include "native_bound_checkpoint_selection_memory.hpp"
#include "native_current_checkpoint_source_memory.hpp"
#include "native_selected_checkpoint_read_lease.hpp"
#include "native_selected_checkpoint_memory_lease.hpp"
#include "native_owned_checkpoint_source.hpp"
#include "native_catalog_leaf_lease_reader.hpp"
#include "native_catalog_roots_lease_reader.hpp"
#include "native_catalog_relation_lease_reader.hpp"
#include "native_catalog_visibility_lease_reader.hpp"
#include "native_allocation_lease_reader.hpp"
#include "row_version_observation.hpp"
#ifdef SB_NATIVE_ROUTE_SOURCE_TESTS
#include "../../src/server/database_ownership.hpp"
#endif
#include "native_checkpoint_inventory_memory.hpp"
#include "native_management_control_authority.hpp"
#include "native_management_publication_recovery.hpp"
#include "native_creation_workspace.hpp"
#include "native_storage_action_intent.hpp"
#include "disk_device.hpp"
#include <filesystem>
#include <unistd.h>
#include <openssl/evp.h>
#include "native_btree_tree_lease_reader.hpp"
#include "native_btree_tree_memory.hpp"
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <future>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <iostream>
#include <limits>
#include <new>
#include <mutex>
#include <source_location>
#include <set>
#include <stdexcept>
#include <cerrno>
#include <sys/wait.h>
namespace {std::uint64_t history_observed_read_bytes=0;bool control_authority_memory_tests=false;}
#include <sys/stat.h>
#include <thread>
namespace {long allocation_budget=-1;bool counting=false;unsigned long allocations=0;unsigned hash_fault=0,hash_target=1,hash_seen=0;bool hash_active=false,hash_counting=false;
unsigned entropy_fault=0,entropy_calls=0;
unsigned preallocation_fault=0,preallocation_calls=0;
int preallocation_fd=-1;off_t preallocation_offset=0,preallocation_bytes=0;
unsigned preallocation_death=0;
std::recursive_mutex* memory_probe_mutex=nullptr;
bool memory_probe_backing_only=false;
unsigned memory_probes=0,memory_locked_probes=0;
void ProbeMemoryFence(bool backing=false){
 if(memory_probe_backing_only&&!backing)return;
 auto* mutex=memory_probe_mutex;if(!mutex)return;memory_probe_mutex=nullptr;
 bool available=false;std::thread probe([&]{available=mutex->try_lock();if(available)mutex->unlock();});probe.join();
 ++memory_probes;if(!available)++memory_locked_probes;memory_probe_mutex=mutex;
}
std::atomic<unsigned> preallocation_pause{0};
off_t growth_extend_to=0;
std::atomic<unsigned> historical_read_pause{0};
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
unsigned historical_stats=0,historical_stat_fault=0;bool historical_stat_counting=false;
#endif
bool repeated_entropy=false;
std::atomic<bool> owned_clock_controlled{false};
std::atomic<std::uint64_t> owned_clock_millis{1700000000123ULL},owned_clock_ticks{1};
off_t corrupt_offset=0;bool corrupt_was_zero=false;
bool io_counting=false;unsigned reads=0,writes=0,syncs=0,read_fault=0,write_fault=0,sync_fault=0,kill_write=0,kill_sync=0,corrupt_read=0;bool kill_after_sync=false;std::size_t torn_bytes=0;int allocation_shard=-1;
int inventory_allocation_route=-1,inventory_allocation_shard=-1,inventory_install_stage=-1,inventory_install_route=-1,inventory_install_shard=-1;bool inventory_read_only=false;bool inventory_publication_mode=false,inventory_publication_cold=false;const unsigned char* replacement_bytes=nullptr;std::size_t replacement_length=0;off_t replacement_offset=0;unsigned replacement_at=0,replacement_seen=0;
int inventory_resolution_stage=-1,inventory_resolution_route=-1,inventory_resolution_shard=-1;bool inventory_mixed_mode=false,inventory_resolution_mode=false,inventory_resolution_requests=false,inventory_reconstruction_faults=false,trace_io=false;std::array<off_t,256> io_trace{};unsigned io_trace_count=0;const unsigned char* race_bytes=nullptr;std::size_t race_length=0;off_t race_offset=0;unsigned race_seen=0;
void Trace(off_t value){if(trace_io){if(io_trace_count==io_trace.size())std::abort();io_trace[io_trace_count++]=value;}}}
void* operator new(std::size_t n){ProbeMemoryFence();if(counting)++allocations;if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void* operator new(std::size_t n,std::align_val_t alignment){
 ProbeMemoryFence(true);
 if(counting)++allocations;if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;
 const auto a=static_cast<std::size_t>(alignment);n=n?n:1;
 if(n>std::numeric_limits<std::size_t>::max()-(a-1))throw std::bad_alloc();
 if(auto* p=std::aligned_alloc(a,(n+a-1)/a*a))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete(void* p,std::align_val_t) noexcept{ProbeMemoryFence(true);std::free(p);}
void operator delete[](void* p,std::align_val_t) noexcept{ProbeMemoryFence(true);std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept{ProbeMemoryFence(true);std::free(p);}
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept{ProbeMemoryFence(true);std::free(p);}
extern "C" int __real_RAND_bytes(unsigned char*,int);
extern "C" int __wrap_RAND_bytes(unsigned char* out,int count){++entropy_calls;if(entropy_fault&&!--entropy_fault)return 0;if(repeated_entropy){std::fill_n(out,count,0);if(count)out[count-1]=1;return 1;}return __real_RAND_bytes(out,count);}
extern "C" scratchbird::core::time::ClockSnapshotResult __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
extern "C" scratchbird::core::time::ClockSnapshotResult __wrap__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv(){
 if(!owned_clock_controlled)return __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
 scratchbird::core::time::ClockSnapshotResult result;
 result.value={{owned_clock_ticks++},{static_cast<std::int64_t>(owned_clock_millis/1000),static_cast<std::uint32_t>((owned_clock_millis%1000)*1000000)}};
 return result;
}
void operator delete(void* p) noexcept{ProbeMemoryFence();std::free(p);}void operator delete[](void* p) noexcept{ProbeMemoryFence();std::free(p);}
void operator delete(void* p,std::size_t) noexcept{ProbeMemoryFence();std::free(p);}void operator delete[](void* p,std::size_t) noexcept{ProbeMemoryFence();std::free(p);}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){hash_active=(hash_fault||hash_counting)&&++hash_seen==hash_target&&hash_fault;if(hash_active&&hash_fault==1){hash_fault=0;return nullptr;}return __real_EVP_MD_CTX_new();}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* m,ENGINE* e){if(hash_active&&hash_fault==2){hash_fault=0;return 0;}return __real_EVP_DigestInit_ex(c,m,e);}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* b,size_t n){if(hash_active&&hash_fault==3){hash_fault=0;return 0;}return __real_EVP_DigestUpdate(c,b,n);}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* b,unsigned int* n){if(hash_active&&hash_fault==4){hash_fault=0;return 0;}const int r=__real_EVP_DigestFinal_ex(c,b,n);if(hash_active&&hash_fault==5){hash_fault=0;*n=31;}return r;}
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* data,size_t bytes,unsigned char* out,unsigned int* size,const EVP_MD* md,ENGINE* engine) {
  const bool selected=(hash_fault||hash_counting)&&++hash_seen==hash_target&&hash_fault;
  const auto mode=selected?hash_fault:0;
  if(selected)hash_fault=0;
  if(mode&&mode!=5)return 0;
  const auto result=__real_EVP_Digest(data,bytes,out,size,md,engine);
  if(mode==5)*size=31;
  return result;
}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
extern "C" int __real_fstat(int,struct stat*);
extern "C" int __wrap_fstat(int fd,struct stat* value){
 if(historical_stat_counting&&++historical_stats==historical_stat_fault){errno=EIO;return -1;}
 return __real_fstat(fd,value);
}
#endif
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* data,size_t n,off_t at){
 if(at==0){unsigned expected=1;if(historical_read_pause.compare_exchange_strong(expected,2))while(historical_read_pause==2)std::this_thread::yield();}
 if(race_bytes&&at==race_offset&&n==race_length&&++race_seen==2&&__real_pwrite(fd,race_bytes,race_length,at)!=static_cast<ssize_t>(race_length)){errno=EIO;return -1;}
 if(io_counting&&++reads==read_fault){errno=EIO;return -1;}const auto result=__real_pread(fd,data,n,at);
 if(io_counting&&result>0)history_observed_read_bytes+=static_cast<std::uint64_t>(result);
 if(growth_extend_to&&at==0&&result>0){const auto size=growth_extend_to;growth_extend_to=0;if(ftruncate(fd,size)){errno=EIO;return -1;}}
 if(io_counting&&reads==corrupt_read&&result>0){corrupt_offset=at;const auto* bytes=static_cast<const unsigned char*>(data);corrupt_was_zero=std::all_of(bytes,bytes+result,[](auto b){return !b;});static_cast<unsigned char*>(data)[result-1]^=1;}
 if(replacement_bytes&&at==replacement_offset&&n==replacement_length&&result==static_cast<ssize_t>(n)&&++replacement_seen==replacement_at)std::copy_n(replacement_bytes,n,static_cast<unsigned char*>(data));return result;
}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t n,off_t at){
 Trace(at);
 if(io_counting){++writes;if(writes==kill_write){if(torn_bytes&&__real_pwrite(fd,data,std::min(n,torn_bytes),at)<0)_exit(85);_exit(86);}
   if(writes==write_fault){if(torn_bytes){const auto result=__real_pwrite(fd,data,std::min(n,torn_bytes),at);if(result<0)return result;}
     errno=EIO;return -1;}}
 return __real_pwrite(fd,data,n,at);
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){Trace(-1);if(io_counting){++syncs;if(syncs==kill_sync&&!kill_after_sync)_exit(86);if(syncs==sync_fault){errno=EIO;return -1;}}const auto result=__real_fsync(fd);if(io_counting&&syncs==kill_sync&&kill_after_sync)_exit(result==0?86:85);return result;}
#if defined(__linux__)
extern "C" int __real_posix_fallocate(int,off_t,off_t);
extern "C" int __wrap_posix_fallocate(int fd,off_t offset,off_t bytes){
 unsigned expected=1;if(preallocation_pause.compare_exchange_strong(expected,2))while(preallocation_pause==2)std::this_thread::yield();
 preallocation_fd=fd;preallocation_offset=offset;preallocation_bytes=bytes;
 if(preallocation_death==1)_exit(86);
 if(preallocation_fault){++preallocation_calls;
  if(preallocation_fault==1)return ENOSPC;
  if(preallocation_fault==3)return EOPNOTSUPP;
  if(preallocation_fault==7){if(ftruncate(fd,offset+bytes/2+1))return errno;return EIO;}
 }
 const auto result=__real_posix_fallocate(fd,offset,bytes);
 if(preallocation_death==2)_exit(result?85:86);
 if(!result){
  if(preallocation_fault==2)return EIO;
  if(preallocation_fault==4)sync_fault=syncs+1;
  if(preallocation_fault==5)write_fault=writes+1;
  if(preallocation_fault==6)write_fault=writes+2;
 }
 return result;
}
#endif


namespace {
using namespace scratchbird::core::platform;
namespace db=scratchbird::storage::database;namespace d=scratchbird::storage::disk;namespace mga=scratchbird::transaction::mga;
using Bytes=std::vector<byte>;using E=db::NativePublicationPlanError;unsigned checks=0;
void Check(bool v,const char* why,std::source_location at=std::source_location::current()){++checks;if(!v)throw std::runtime_error(std::string(why)+" line="+std::to_string(at.line()));}
Uuid Id(unsigned n){Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[14]=n>>8;id.bytes[15]=n;return id;}
void Num(Bytes& b,std::size_t at,unsigned n,u64 v){for(unsigned i=0;i<n;++i)b[at+i]=v>>(8*i);}
void Put(Bytes& b,std::size_t at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}
void Ref(Bytes& b,std::size_t at,const d::NativePageReference& r){Put(b,at,r.filespace_uuid);Num(b,at+16,8,r.page_number);Num(b,at+24,8,r.page_generation);Put(b,at+32,r.page_size_profile_uuid);}
auto Sha(const Bytes& b){std::array<byte,32> h{};Check(SHA256(b.data(),b.size(),h.data()),"independent SHA");return h;}
void HeaderSeal(Bytes& b){std::fill(b.begin()+96,b.begin()+104,0);u64 hash=14695981039346656037ull;for(unsigned i=0;i<128;++i){hash^=b[i];hash*=1099511628211ull;}Num(b,96,8,hash);}
void PlanSeal(Bytes& b){std::fill(b.begin()+688,b.begin()+720,0);const auto h=Sha(b);std::copy(h.begin(),h.end(),b.begin()+688);}
Bytes Oracle(const db::NativePublicationPlan& p){
 const auto& h=p.header;Bytes b(h.page_size_bytes);std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,0x500);Num(b,20,2,1);Num(b,22,2,1);
 Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);HeaderSeal(b);
const bool v2=p.management_extent.has_value(),v3=p.control_bundle.has_value(),v4=p.base_selection_generation.has_value(),v5=p.intent.recovery_profile!=0,v6=p.intent.recovery_profile==2,v7=p.intent.recovery_profile==3,v8=v3&&p.control_bundle->directory_count,v9=p.intent.startup_binding.has_value();std::copy_n(v9?"SBPPM009":v8?"SBPPM008":v7?"SBPPM007":v6?"SBPPM006":v5?"SBPPM005":v4?"SBPPM004":v3?"SBPPM003":v2?"SBPPM002":"SBPPM001",8,b.begin()+128);Num(b,136,2,v9?9:v8?8:v7?7:v6?6:v5?5:v4?4:v3?3:v2?2:1);Num(b,138,2,v3?1024:v2?896:640);Num(b,140,4,v3?1152:v2?1024:768);
 Put(b,144,p.object_uuid);Put(b,160,p.bootstrap_uuid);Put(b,176,p.timeline_uuid);Put(b,192,p.operation_uuid);Put(b,208,p.intent.initiator_uuid);Put(b,224,p.intent.request_context_uuid);Put(b,240,p.intent.policy_snapshot_uuid);Put(b,256,p.security_snapshot_uuid);
 std::copy(p.intent.normalized_request_sha256.begin(),p.intent.normalized_request_sha256.end(),b.begin()+272);std::copy(p.reservation_state_sha256.begin(),p.reservation_state_sha256.end(),b.begin()+304);
 Num(b,336,8,p.reserved_generation);Num(b,344,8,p.base_checkpoint_generation);Num(b,352,8,p.base_root_set_generation);Num(b,360,8,p.target_root_set_generation);
 Ref(b,368,p.base_checkpoint);Put(b,416,p.base_checkpoint_object_uuid);std::copy(p.base_checkpoint_sha256.begin(),p.base_checkpoint_sha256.end(),b.begin()+432);Ref(b,464,p.target_checkpoint);Put(b,512,p.target_checkpoint_object_uuid);std::copy(p.target_graph_sha256.begin(),p.target_graph_sha256.end(),b.begin()+528);
 if(p.previous_plan)Ref(b,560,*p.previous_plan);Put(b,608,p.previous_plan_object_uuid);std::copy(p.previous_plan_sha256.begin(),p.previous_plan_sha256.end(),b.begin()+624);
 Num(b,656,8,p.catalog_generation);Num(b,664,8,p.configuration_generation);Num(b,672,8,p.security_generation);Num(b,680,2,p.intent.initiator_kind);Num(b,682,2,1);Num(b,684,2,2);
 if(p.management_extent){const auto& r=*p.management_extent;Num(b,720,4,p.generation_guard_flags);Ref(b,736,r.first);Put(b,784,r.object_uuid);Put(b,800,r.operation_uuid);Num(b,816,8,r.revision);Num(b,824,4,r.aggregate_bytes);Num(b,828,4,r.page_count);std::copy(r.aggregate_sha256.begin(),r.aggregate_sha256.end(),b.begin()+832);std::copy(r.first_page_sha256.begin(),r.first_page_sha256.end(),b.begin()+864);}
 if(p.control_bundle){const auto& r=*p.control_bundle;Ref(b,896,r.first);Put(b,944,r.object_uuid);Num(b,960,8,r.map_count);Num(b,968,8,r.page_count);std::copy(r.aggregate_sha256.begin(),r.aggregate_sha256.end(),b.begin()+976);std::copy(r.first_page_sha256.begin(),r.first_page_sha256.end(),b.begin()+1008);}
 if(v4)Num(b,1040,8,*p.base_selection_generation);if(v5)Num(b,1048,2,p.intent.recovery_profile);if((v6||v8)&&p.control_bundle)Num(b,1056,8,p.control_bundle->inventory_count);
 if(v8){Num(b,1064,8,p.control_bundle->directory_count);Num(b,1072,8,p.control_bundle->payload_bytes);Num(b,1080,8,p.control_bundle->growth_image_count);}
 if(v9){const auto& binding=*p.intent.startup_binding;Put(b,1088,binding.operation_uuid);Put(b,1104,binding.session_uuid);Put(b,1120,binding.transaction_uuid);Num(b,1136,8,binding.local_transaction_id);Num(b,1144,8,binding.fence_generation);}
 PlanSeal(b);return b;
}
void Failed(const db::NativePublicationPlanImage& r){Check(!r.ok()&&!r.plan&&r.bytes.empty()&&std::all_of(r.sha256.begin(),r.sha256.end(),[](byte v){return !v;}),"no failed plan prefix");}
void Bad(const db::NativePublicationPlan& p){Failed(db::EncodeNativePublicationPlan(p));const auto raw=Oracle(p);Failed(db::DecodeNativePublicationPlan(raw));}
void StartupPlan(db::NativePublicationPlan p) {
 p.intent.startup_binding=db::NativeStartupBinding{p.management_extent->operation_uuid,Id(31000),Id(31001),13,19};
 const auto raw=Oracle(p);const auto encoded=db::EncodeNativePublicationPlan(p);
 Check(encoded.ok()&&encoded.bytes==raw&&encoded.sha256==Sha(raw),"version9 complete independent startup binding bytes");
 const auto decoded=db::DecodeNativePublicationPlan(raw);
 Check(decoded.ok()&&decoded.plan->intent==p.intent&&decoded.plan->management_extent==p.management_extent&&
   decoded.plan->control_bundle==p.control_bundle&&db::EncodeNativePublicationPlan(*decoded.plan).bytes==raw,"startup plan preserves exact binding and reconstruction inputs");
 for(unsigned field=0;field<3;++field)for(unsigned invalid=0;invalid<3;++invalid){auto bad=p;auto& b=*bad.intent.startup_binding;
   auto& id=field==0?b.operation_uuid:field==1?b.session_uuid:b.transaction_uuid;
   if(!invalid)id={};if(invalid==1)id.bytes[6]=0x40;if(invalid==2)id.bytes[8]=0xc0;Bad(bad);}
 for(unsigned field=0;field<4;++field){auto bad=p;auto& b=*bad.intent.startup_binding;
   if(!field)b.local_transaction_id=0;if(field==1)b.local_transaction_id=UINT64_MAX;if(field==2)b.fence_generation=0;if(field==3)b.operation_uuid=Id(31002);Bad(bad);}
 for(u16 profile:{0,1,3,4,5}){auto bad=p;bad.intent.recovery_profile=profile;Bad(bad);}
 for(unsigned version=1;version<9;++version){auto bad=raw;bad[135]='0'+version;Num(bad,136,2,version);PlanSeal(bad);Failed(db::DecodeNativePublicationPlan(bad));}
 for(unsigned at=1088;at<1152;++at){auto bad=raw;bad[at]^=1;const auto r=db::DecodeNativePublicationPlan(bad);Failed(r);Check(r.error==E::invalid_integrity,"every binary startup byte is sealed");}
 auto maximum=p;maximum.intent.startup_binding->local_transaction_id=UINT64_MAX-1;maximum.intent.startup_binding->fence_generation=UINT64_MAX;
 Check(db::EncodeNativePublicationPlan(maximum).bytes==Oracle(maximum)&&db::DecodeNativePublicationPlan(Oracle(maximum)).ok(),"startup plan exact unsigned integer bounds");
 for(unsigned route=0;route<2;++route){const auto call=[&]{return route?db::DecodeNativePublicationPlan(raw):db::EncodeNativePublicationPlan(p);};
   allocations=0;counting=true;Check(call().ok(),"startup plan allocation baseline");counting=false;const auto sites=allocations;
   for(unsigned long at=0;at<sites;++at){allocation_budget=at;const auto r=call();const auto left=allocation_budget;allocation_budget=-1;
     Check(left==-1&&r.error==E::resource_exhausted,"startup plan exact allocation failure consumed");Failed(r);}
   hash_counting=true;hash_seen=0;Check(call().ok(),"startup plan hash baseline");hash_counting=false;const auto hashes=hash_seen;
   for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto r=call();
     Check(!hash_fault&&r.error==E::hash_failure,"startup plan every provider fault consumed");Failed(r);}
 }
}
struct Fixture {
 std::filesystem::path path;d::FileDevice device,secondary;Bytes secondary_before;std::vector<d::NativeFilespaceDevice> devices;u64 size,budget,total_pages;
 std::unique_ptr<scratchbird::core::uuid::StandaloneUuidV7Issuer> issuer;
 void ResetIssuer(){issuer=std::make_unique<scratchbird::core::uuid::StandaloneUuidV7Issuer>(
   scratchbird::core::uuid::StandaloneUuidV7Binding{Id(1),Id(10)},scratchbird::core::uuid::StandaloneUuidV7Policy{{},0,1000});}
 Fixture(unsigned profile,u64 capacity=256):total_pages(capacity){
  const auto* temporary=std::getenv("TMPDIR");
  auto name=((temporary&&*temporary?std::filesystem::path(temporary):std::filesystem::temp_directory_path())/"sb-management-bundle.XXXXXX").string();
  Check(mkdtemp(name.data()),"isolated fixture directory");path=std::filesystem::path(name)/"node";
  const auto& page=d::kCanonicalFilespacePageProfiles[profile];size=page.page_size_bytes;budget=1024*size;
  db::NativeFilespaceInitializationRequest r;r.bootstrap={Id(1),Id(2),page.uuid,d::kNativeBootstrapIntegrityProfile,{},page.page_size_bytes,1,0,1,7};r.operation_uuid=Id(3);r.writer_uuid=Id(4);
  r.creator.transaction_uuid={UuidKind::transaction,Id(5)};r.creator.local_id=mga::MakeLocalTransactionId(1);r.creator.scope=mga::TransactionScope::local_node;r.creation_utc_millis=1789357072000ULL;r.total_pages=total_pages;
  Check(device.Open(path.string(),d::FileOpenMode::create_new).ok(),"owned device");devices={{Id(2),page.uuid,&device}};
  r.policy_snapshot_uuid=Id(10);ResetIssuer();
  Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(device,r,budget,*issuer).ok(),"actual genesis");
 }
 ~Fixture(){device.Close();secondary.Close();std::error_code ec;std::filesystem::remove_all(path.parent_path(),ec);}
 Bytes Read(u64 page,u64 count=1){Bytes b(size*count);const auto r=device.ReadAt(page*size,b.data(),b.size());Check(r.ok()&&r.bytes_transferred==b.size(),"owned read");return b;}
};
auto GraphOracle(Bytes b){const auto used=LoadLittle32(b.data()+140);std::fill(b.begin()+336,b.begin()+400,0);bool found=false;
 for(std::size_t at=512;at<used;at+=112)if(LoadLittle16(b.data()+at)==16){std::fill(b.begin()+at+72,b.begin()+at+104,0);found=true;}
 Check(found,"oracle plan role");const std::string_view domain="SBPPGR01";b.insert(b.begin(),domain.begin(),domain.end());return Sha(b);
}
Bytes Finish(db::NativePublicationPlan& p,db::NativeCheckpointRoot cp){
 cp.header.database_uuid=p.header.database_uuid;cp.header.filespace_uuid=p.target_checkpoint.filespace_uuid;cp.header.page_number=p.target_checkpoint.page_number;cp.header.page_generation=p.target_checkpoint.page_generation;cp.header.page_size_profile_uuid=p.target_checkpoint.page_size_profile_uuid;
 cp.object_uuid=p.target_checkpoint_object_uuid;cp.timeline_uuid=p.timeline_uuid;cp.creator_transaction_uuid={};cp.creator_local_transaction_id=0;cp.creator_operation_uuid=p.operation_uuid;cp.checkpoint_generation=p.reserved_generation;cp.root_set_generation=p.target_root_set_generation;cp.predecessor=p.base_checkpoint;cp.predecessor_sha256=p.base_checkpoint_sha256;
 db::NativeCheckpointRootReference role;role.role=16;role.page_type=0x500;role.page={p.header.filespace_uuid,p.header.page_number,p.header.page_generation,p.header.page_size_profile_uuid};role.object_uuid=p.object_uuid;role.sha256.fill(1);
 if(cp.roots.back().role==16)cp.roots.back()=role;else cp.roots.push_back(role);
 auto first=db::EncodeNativeCheckpointRoot(cp);Check(first.ok(),"target checkpoint fixture");
 const auto projected=db::ComputeNativePublicationTargetGraphDigest(first.bytes);Check(projected.ok()&&projected.sha256==GraphOracle(first.bytes),"independent target projection");p.target_graph_sha256=projected.sha256;
 const auto plan=db::EncodeNativePublicationPlan(p);Check(plan.ok()&&plan.bytes==Oracle(p),"independent plan image");cp.roots.back().sha256=plan.sha256;
 const auto final=db::EncodeNativeCheckpointRoot(cp);Check(final.ok()&&db::ComputeNativePublicationTargetGraphDigest(final.bytes).sha256==p.target_graph_sha256,"noncircular final graph");return final.bytes;
}

namespace records {
using O=db::NativeManagementOperation;using S=db::NativeManagementStep;
using OS=db::NativeManagementState;using SS=db::NativeManagementStepState;using Pages=std::vector<Bytes>;
Uuid Id(unsigned n){Uuid u;u.bytes[0]=1;u.bytes[6]=0x70;u.bytes[8]=0x80;u.bytes[14]=n>>8;u.bytes[15]=n;return u;}
bool Terminal(unsigned s){return s>=13;}
O Example(unsigned state=4){O o;o.database_uuid=Id(1);o.bootstrap_uuid=Id(2);o.uuid=Id(3);o.descriptor_uuid=Id(4);o.family_uuid=Id(5);o.target_type_uuid=Id(6);o.target_uuid=Id(7);
 o.initiator_uuid=Id(8);o.request_context_uuid=Id(9);o.policy_snapshot_uuid=Id(10);o.security_snapshot_uuid=Id(11);
 o.created_at=Id(1000);o.updated_at=Id(2000);o.normalized_request_sha256.fill(19);o.generation_guards={0,1,2,std::nullopt};
 o.idempotency_key=std::string("operation\0key",13);o.state=OS(state);o.initiator_kind=4;
 if(state>=3){o.phase_uuid=Id(12);o.resource_plan_uuid=Id(13);o.lock_plan_uuid=Id(14);}
 if(Terminal(state)){o.terminal_at=Id(1900);o.result_uuid=Id(15);o.boundary_uuid=Id(16);o.diagnostic_uuid=Id(17);}
 return o;
}
S Step(unsigned state=1,unsigned ordinal=1){S s;s.uuid=Id(3000+ordinal);s.operation_uuid=Id(3);s.family_uuid=Id(21);s.target_uuid=Id(7);s.ordinal=ordinal;s.state=SS(state);
 s.idempotency_key=std::string("step\0key",8);s.precondition_sha256.fill(22);s.mutation=db::NativeManagementMutation::page;s.compensation=db::NativeManagementCompensation::retry;
 if(state==3||state==4||state==5||state==6||state==7||state==9||state==10)s.started_at=Id(1200);
 if(state>=7){s.completed_at=Id(1300);s.diagnostic_uuid=Id(25);s.evidence_uuid=Id(26);s.metric_evidence_uuid=Id(27);}
 if(state==7||state==9){s.postcondition_sha256.fill(23);s.boundary_uuid=Id(24);}return s;
}
void Num(Bytes& b,std::size_t at,unsigned n,u64 v){for(unsigned i=0;i<n;++i)b[at+i]=static_cast<byte>(v>>(8*i));}
void Put(Bytes& b,std::size_t at,const Uuid& u){std::copy(u.bytes.begin(),u.bytes.end(),b.begin()+at);}
auto Sha(const Bytes& b){std::array<byte,32> h{};Check(SHA256(b.data(),b.size(),h.data()),"independent SHA256");return h;}
void Seal(Bytes& b){std::fill(b.begin()+480,b.begin()+512,0);const auto h=Sha(b);std::copy(h.begin(),h.end(),b.begin()+480);}
Bytes Oracle(const O& o){Bytes b(512);std::copy_n("SBMGO001",8,b.begin());Num(b,8,2,1);Num(b,10,2,512);
 Put(b,16,o.database_uuid);Put(b,32,o.bootstrap_uuid);Put(b,48,o.uuid);Put(b,64,o.descriptor_uuid);Put(b,80,o.family_uuid);Put(b,96,o.target_type_uuid);Put(b,112,o.target_uuid);Put(b,128,o.initiator_uuid);Put(b,144,o.request_context_uuid);Put(b,160,o.policy_snapshot_uuid);Put(b,176,o.security_snapshot_uuid);Put(b,192,o.phase_uuid);Put(b,208,o.boundary_uuid);Put(b,224,o.created_at);Put(b,240,o.updated_at);Put(b,256,o.terminal_at);
 std::copy(o.normalized_request_sha256.begin(),o.normalized_request_sha256.end(),b.begin()+272);Put(b,304,o.resource_plan_uuid);Put(b,320,o.lock_plan_uuid);Put(b,336,o.result_uuid);Put(b,352,o.diagnostic_uuid);Put(b,368,o.evidence_uuid);Put(b,384,o.metric_evidence_uuid);Put(b,400,o.cluster_uuid);
 u32 flags=(o.evidence_required?16u:0u)|(o.metrics_required?32u:0u);for(unsigned i=0;i<4;++i)if(o.generation_guards[i]){flags|=1u<<i;Num(b,416+8*i,8,*o.generation_guards[i]);}
 Num(b,448,8,o.revision);Num(b,456,4,o.steps.size());Num(b,460,4,o.idempotency_key.size());Num(b,464,2,u16(o.state));Num(b,466,2,u16(o.scope));Num(b,468,2,o.initiator_kind);Num(b,470,2,u16(o.restart));Num(b,472,4,flags);
 b.insert(b.end(),o.idempotency_key.begin(),o.idempotency_key.end());
 for(const auto& s:o.steps){const auto at=b.size();b.resize(at+256);Put(b,at,s.uuid);Put(b,at+16,s.operation_uuid);Put(b,at+32,s.family_uuid);Put(b,at+48,s.target_uuid);std::copy(s.precondition_sha256.begin(),s.precondition_sha256.end(),b.begin()+at+64);std::copy(s.postcondition_sha256.begin(),s.postcondition_sha256.end(),b.begin()+at+96);Put(b,at+128,s.started_at);Put(b,at+144,s.completed_at);Put(b,at+160,s.evidence_uuid);Put(b,at+176,s.metric_evidence_uuid);Put(b,at+192,s.diagnostic_uuid);Put(b,at+208,s.boundary_uuid);
  Num(b,at+224,4,s.ordinal);Num(b,at+228,4,s.idempotency_key.size());Num(b,at+232,2,u16(s.state));Num(b,at+234,2,u16(s.mutation));Num(b,at+236,2,u16(s.compensation));Num(b,at+238,2,u16(s.recovery));Num(b,at+240,4,(s.evidence_required?1u:0u)|(s.metrics_required?2u:0u)|(s.idempotent?4u:0u));b.insert(b.end(),s.idempotency_key.begin(),s.idempotency_key.end());}
 Num(b,12,4,b.size());Seal(b);return b;
}

O Record(std::size_t bytes){auto o=Example();const auto count=(bytes-513)/257;const auto remainder=(bytes-513)%257;
 o.idempotency_key=std::string(1+remainder,'k');for(unsigned i=1;i<=count;++i){auto s=Step(1,i);s.idempotency_key="s";o.steps.push_back(s);}Check(Oracle(o).size()==bytes,"exact aggregate fixture length");return o;}
std::vector<d::NativeCommonPageHeader> Headers(const O& o,unsigned profile){const auto& p=d::kCanonicalFilespacePageProfiles[profile];const auto bytes=Oracle(o).size();const auto capacity=p.page_size_bytes-384;
 std::vector<d::NativeCommonPageHeader> out;for(unsigned i=0;i<(bytes+capacity-1)/capacity;++i)out.push_back({p.page_size_bytes,0x500,o.database_uuid,Id(60),Id(6000+i),64+i,2,0,p.uuid});return out;}
void HeaderSeal(Bytes& b){std::fill(b.begin()+96,b.begin()+104,0);u64 hash=14695981039346656037ull;for(unsigned i=0;i<128;++i){hash^=b[i];hash*=1099511628211ull;}Num(b,96,8,hash);}
void PageSeal(Bytes& b){std::fill(b.begin()+320,b.begin()+352,0);const auto h=Sha(b);std::copy(h.begin(),h.end(),b.begin()+320);}
Pages ExtentOracle(const O& o,const std::vector<d::NativeCommonPageHeader>& headers){const auto aggregate=Oracle(o);const auto digest=Sha(aggregate);Pages pages(headers.size());std::array<byte,32> next{};
 for(std::size_t left=headers.size();left;--left){const auto i=left-1;const auto& h=headers[i];auto& b=pages[i];b.resize(h.page_size_bytes);
  std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,0x500);Num(b,20,2,1);Num(b,22,2,1);Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);HeaderSeal(b);
  const auto capacity=b.size()-384,offset=i*capacity,length=std::min(capacity,aggregate.size()-offset);
  std::copy_n("SBMGP001",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,256);Num(b,140,4,384+length);
  Put(b,144,Id(5000));Put(b,160,o.bootstrap_uuid);Put(b,176,o.uuid);Num(b,192,8,o.revision);Num(b,200,4,aggregate.size());Num(b,204,4,offset);Num(b,208,4,i);Num(b,212,4,headers.size());Num(b,216,8,headers[0].page_number);
  std::copy(digest.begin(),digest.end(),b.begin()+224);Num(b,256,4,length);std::copy(next.begin(),next.end(),b.begin()+288);std::copy_n(aggregate.begin()+offset,length,b.begin()+384);PageSeal(b);next=Sha(b);}
 return pages;}
void Rechain(Pages& pages,db::NativeManagementExtentRoot& root){std::array<byte,32> next{};for(std::size_t left=pages.size();left;--left){auto& b=pages[left-1];std::copy(next.begin(),next.end(),b.begin()+288);HeaderSeal(b);PageSeal(b);next=Sha(b);}root.first_page_sha256=next;}

}


using CE=db::NativeManagementControlAllocationError;
#include "native_management_control_allocation_memory_checks.hpp"
namespace page=scratchbird::storage::page;
using State=page::NativeAllocationState;
using Maps=std::vector<page::NativeAllocationMap>;
using Pages=std::vector<Bytes>;

struct BundleBacking {
 std::vector<std::span<const byte>> pages;
 Bytes bytes;
 explicit BundleBacking(const Pages& input){
  std::size_t total=0;for(const auto& raw:input){pages.emplace_back(raw);total+=raw.size();}
  Check(total<(std::numeric_limits<std::size_t>::max()-65536)/32,"bounded fixture size");
  bytes.resize(total*32+65536);
 }
};
void EmptyView(const db::NativeManagementControlBundleViewRead& r){
 Check(!r.ok()&&!r.total_pages&&!r.backing_bytes_used&&r.page_headers.empty()&&r.allocation_images.empty()&&
  r.inventory_images.empty()&&r.directory_images.empty()&&r.growth_images.empty(),"no bounded control bundle prefix");
}
void SameView(const db::NativeManagementControlBundleRead& owning,
 const db::NativeManagementControlBundleViewRead& view,std::span<const byte> backing){
 Check(view.error==owning.error,"complete bounded control bundle error parity");
 if(!owning.ok()){EmptyView(view);return;}
 Check(view.ok()&&view.total_pages==owning.total_pages&&view.backing_bytes_used&&view.backing_bytes_used<=backing.size(),
  "complete bounded control bundle capacity and charged backing");
 const auto inside=[&](const void* p,std::size_t bytes){
  const auto at=reinterpret_cast<std::uintptr_t>(p),first=reinterpret_cast<std::uintptr_t>(backing.data());
  return at>=first&&at-first<=backing.size()&&bytes<=backing.size()-(at-first);
 };
 const auto images=[&](const auto& owned,const auto& borrowed){
  Check(owned.size()==borrowed.size(),"complete image family retained");
  if(!borrowed.empty())Check(inside(borrowed.data(),borrowed.size_bytes()),"retained descriptors use caller backing");
  for(std::size_t i=0;i<owned.size();++i)Check(std::equal(owned[i].begin(),owned[i].end(),borrowed[i].begin(),borrowed[i].end())&&
    inside(borrowed[i].data(),borrowed[i].size()),"exact original image retained in caller backing");
 };
 images(owning.allocation_images,view.allocation_images);images(owning.inventory_images,view.inventory_images);
 images(owning.directory_images,view.directory_images);images(owning.growth_images,view.growth_images);
 Check(owning.page_headers.size()==view.page_headers.size()&&inside(view.page_headers.data(),view.page_headers.size_bytes()),
  "complete physical headers use caller backing");
 for(std::size_t i=0;i<owning.page_headers.size();++i){
  const auto a=d::EncodeNativeCommonPageHeader(owning.page_headers[i]),b=d::EncodeNativeCommonPageHeader(view.page_headers[i]);
  Check(a.ok()&&b.ok()&&a.bytes==b.bytes,"all physical header fields retained");
 }
}
db::NativeManagementControlBundleRead DecodeBundle(const Pages& pages,const db::NativeManagementControlBundleRoot& root,
 const Uuid& database,const Uuid& bootstrap,u64 budget){
 const bool compare=allocation_budget<0&&!counting&&!hash_counting&&!hash_fault;
 auto result=db::DecodeNativeManagementControlBundle(pages,root,database,bootstrap,budget);
 if(compare){
  BundleBacking backing(pages);allocation_budget=0;
  const auto view=db::DecodeNativeManagementControlBundleInto(backing.pages,root,database,bootstrap,budget,backing.bytes);
  const bool untouched=allocation_budget==0;allocation_budget=-1;
  Check(untouched,"complete bundle decode uses no C++ heap fallback");SameView(result,view,backing.bytes);
 }
 return result;
}
void BoundedBundleChecks(const Pages& pages,const db::NativeManagementControlBundleRoot& root,
 const Uuid& database,const Uuid& bootstrap,u64 budget){
 using Error=db::NativeManagementControlBundleError;
 BundleBacking backing(pages);const auto owned=db::DecodeNativeManagementControlBundle(pages,root,database,bootstrap,budget);
 Check(owned.ok(),"bounded fixture is complete");
 const auto call=[&](std::span<byte> bytes){
  allocation_budget=0;const auto r=db::DecodeNativeManagementControlBundleInto(backing.pages,root,database,bootstrap,budget,bytes);
  const bool untouched=allocation_budget==0;allocation_budget=-1;Check(untouched,"bounded result does not allocate process storage");return r;
 };
 const auto full=call(backing.bytes);SameView(owned,full,backing.bytes);const auto used=full.backing_bytes_used;
 Check(used>1,"nonempty backing footprint");SameView(owned,call(std::span(backing.bytes).first(used)),backing.bytes);
 for(const auto n:{std::size_t{0},std::size_t{1},used/2,used-1}){
  const auto r=call(std::span(backing.bytes).first(n));Check(r.error==Error::resource_exhausted,"exact and short backing failure");EmptyView(r);
 }
 // Non-aligned byte storage must either account for padding or exhaust safely.
 for(std::size_t offset=1;offset<alignof(std::max_align_t);++offset){
  const auto r=call(std::span(backing.bytes).subspan(offset));SameView(owned,r,std::span(backing.bytes).subspan(offset));
 }
 const auto alias=[&](std::span<byte> bytes){
  const Bytes before(bytes.begin(),bytes.end());const auto r=call(bytes);
  Check(r.error==Error::invalid_workspace&&std::equal(before.begin(),before.end(),bytes.begin()),"whole aliased input rejected before mutation");EmptyView(r);
 };
 for(const auto& raw:pages){alias({const_cast<byte*>(raw.data()),raw.size()});alias({const_cast<byte*>(raw.data())+raw.size()-1,1});}
 alias({reinterpret_cast<byte*>(backing.pages.data()),backing.pages.size()*sizeof(backing.pages[0])});
 alias({reinterpret_cast<byte*>(const_cast<db::NativeManagementControlBundleRoot*>(&root)),sizeof(root)});
 alias({reinterpret_cast<byte*>(const_cast<Uuid*>(&database)),sizeof(database)});
 alias({reinterpret_cast<byte*>(const_cast<Uuid*>(&bootstrap)),sizeof(bootstrap)});
 hash_seen=0;hash_counting=true;const auto baseline=call(backing.bytes);hash_counting=false;Check(baseline.ok(),"bounded provider baseline");
 const auto hashes=hash_seen;Check(hashes>0,"all component provider sites observed");
 for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){
  hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto r=call(backing.bytes);
  const bool consumed=!hash_fault;hash_fault=0;hash_active=false;
  Check(consumed&&r.error==Error::hash_failure,"every bounded provider failure retains its exact error under heap denial");EmptyView(r);
 }
}

namespace memory=scratchbird::core::memory;
struct BundleMemoryFixture {
 memory::MemoryManager manager;
 memory::HierarchicalMemoryBudgetLedger ledger{3,5};
 db::NativeStorageMemoryBinding binding{Id(1),Id(60001),Id(60002),Id(60003)};
 db::NativeStorageMemory memory;
 static auto Policy(){auto p=scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();
  p.hard_limit_bytes=p.per_context_limit_bytes=256ULL*1024*1024;return p;}
 explicit BundleMemoryFixture(u64 bytes):manager(Policy()){
  namespace m=scratchbird::core::memory;
  m::ReservationBackedMemoryResourceRequest request;request.memory_manager=&manager;request.reservation_ledger=&ledger;
  request.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
  request.category=m::MemoryCategory::page_buffer;request.requested_bytes=bytes;request.memory_class="page_buffer";
  request.route_label="storage.control-bundle.conformance";request.purpose="complete native control bundle source";
  request.binary_operation_uuid=binding.operation_uuid.bytes;
  request.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
  request.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
  request.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
  request.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(60004).bytes},
    {m::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
  request.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
  request.provenance.source_label="actual bundle backing conformance";
  for(const auto& scope:request.scope_chain){m::HierarchicalMemoryBudget budget;budget.scope=scope;
   budget.hard_limit_bytes=bytes;budget.provenance=request.provenance;Check(ledger.SetBudget(budget).ok(),"actual parent memory limit");}
  auto grant=m::AcquireReservationBackedMemoryResource(request);Check(grant.ok(),"actual bundle grant issued");
  auto adopted=db::AdoptNativeStorageMemory(binding,grant.resource);Check(adopted.ok()&&!grant.resource,"exclusive binary-bound bundle memory adoption");
  memory=std::move(adopted.memory);
 }
 void Empty(){const auto state=manager.Snapshot();Check(!state.current_bytes&&!state.reserved_capacity_bytes&&
  !state.active_capacity_reservation_count&&!ledger.Snapshot().current_bytes,"all bundle memory charges released");}
};
void BundleDeviceMemoryChecks(const d::NativeFilespaceDevice& file,const db::NativeManagementControlBundleRoot& root,
 const Uuid& bootstrap,u64 allowance,const Pages& input,db::NativeManagementControlReadContext context,
 const Bytes& historical,const std::filesystem::path& path,bool deep){
 using C=db::NativeManagementControlReadContext;using E=db::NativeManagementControlBundleError;
 using M=db::NativeManagementControlBundleMemoryError;
 const auto expected=context==C::current?db::ReadNativeManagementControlBundleFromOpenDevice(file,root,Id(1),bootstrap,allowance):
  context==C::historical_before?db::ReadNativeManagementControlBundleAtHistoricalPageZeroFromOpenDevice(file,root,Id(1),bootstrap,historical,allowance):
  db::ReadNativeManagementControlBundleAtHistoricalResultFromOpenDevice(file,root,Id(1),bootstrap,historical,allowance);
 Check(expected.ok(),"complete actual source for governed read");
 BundleBacking backing(input);const auto capacity=backing.bytes.size();
 const auto direct=[&](std::span<byte> region){
  d::FileDevice::ReadLatencyBatch observations(*file.device);
  allocation_budget=0;const auto r=db::ReadNativeManagementControlBundleInto(file,root,Id(1),bootstrap,allowance,context,historical,observations,region);
  const bool untouched=allocation_budget==0;allocation_budget=-1;Check(untouched,"actual bundle image metadata and scratch have no process allocation");return r.bundle;
 };
 const auto full=direct(backing.bytes);SameView(expected,full,backing.bytes);
 SameView(expected,direct(std::span(backing.bytes).first(full.backing_bytes_used)),backing.bytes);
 const auto short_backing=direct(std::span(backing.bytes).first(full.backing_bytes_used-1));
 Check(short_backing.error==E::resource_exhausted,"one byte short complete read backing refuses");EmptyView(short_backing);
 {d::FileDevice foreign;d::FileDevice::ReadLatencyBatch observations(foreign);reads=0;io_counting=true;
  const auto wrong=db::ReadNativeManagementControlBundleInto(file,root,Id(1),bootstrap,allowance,context,historical,observations,backing.bytes);
  io_counting=false;Check(wrong.bundle.error==E::invalid_request&&!reads&&!wrong.physical_bytes_read,"foreign observation route cannot supply actual reads");EmptyView(wrong.bundle);}
 BundleMemoryFixture f(capacity);const db::NativeManagementControlBundleMemoryLimits limits{allowance,capacity};
 const auto call=[&](const db::NativeStorageMemoryBinding& binding){
  return db::ReadNativeManagementControlBundleWithMemoryFromOpenDevice(file,root,Id(1),bootstrap,limits,f.memory,binding,context,historical);};
 const auto failed=[&](const auto& r){Check(!r.ok()&&!r.arena,"failed source has no owner or prefix");EmptyView(r.bundle);};
 std::recursive_mutex* mutex=nullptr;{auto fence=file.device->AcquireOperationGuard();mutex=fence.mutex();}
 memory_probes=memory_locked_probes=0;if(deep)memory_probe_mutex=mutex;
 auto result=call(f.binding);memory_probe_mutex=nullptr;Check(result.ok(),"actual shared grant backs complete source read");
 const auto page_bytes=d::FindCanonicalFilespacePageProfile(file.page_size_profile_uuid)->page_size_bytes;
 const auto physical_bytes=(root.page_count+1)*page_bytes+(context==C::current?4096:0);
 Check(result.io_status.ok()&&result.physical_bytes_read==physical_bytes,"complete actual physical read accounting");
 if(deep)Check(memory_probes&&!memory_locked_probes,"all observed allocation cleanup and telemetry control work occurs outside read fence");
 const auto first=static_cast<const byte*>(result.bundle.allocation_images.front().data());
 // The retained image is inside one real charged arena; use its independently
 // allocated region via the first payload address and all ledger dimensions.
 Check(first&&f.manager.Snapshot().current_bytes==capacity&&f.ledger.Snapshot().current_bytes==capacity&&
  f.memory.Snapshot().allocated_bytes==capacity&&result.arena.Snapshot().retained_bytes==capacity,
  "every actual memory ledger retains complete backing");
 Check(result.bundle.total_pages==expected.total_pages&&result.bundle.allocation_images.size()==expected.allocation_images.size(),
  "governed source has complete expected capacity and images");
 const auto same_images=[&](const auto& a,const auto& b){Check(a.size()==b.size(),"actual grant retains whole image family");
  for(std::size_t i=0;i<a.size();++i)Check(std::equal(a[i].begin(),a[i].end(),b[i].begin(),b[i].end()),"actual grant retains all original image bytes");};
 same_images(expected.allocation_images,result.bundle.allocation_images);same_images(expected.inventory_images,result.bundle.inventory_images);
 same_images(expected.directory_images,result.bundle.directory_images);same_images(expected.growth_images,result.bundle.growth_images);
 reads=0;io_counting=true;auto exhausted=call(f.binding);io_counting=false;failed(exhausted);
 Check(exhausted.error==M::memory_allocation_failure&&!reads,"held backing prevents an uncharged second reader");
 for(unsigned field=0;field<4;++field){auto wrong=f.binding;const std::array<Uuid*,4> values{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};
  *values[field]=Id(60009);reads=0;io_counting=true;const auto refused=call(wrong);io_counting=false;failed(refused);
  Check(refused.error==M::memory_binding_failure&&!reads,"all four binary owner fields checked before I/O");}
 if(deep){
  Check(file.device->Close().ok(),"close source while complete backing remains retained");
  for(std::size_t i=0;i<expected.growth_images.size();++i)Check(std::equal(expected.growth_images[i].begin(),expected.growth_images[i].end(),
   result.bundle.growth_images[i].begin(),result.bundle.growth_images[i].end()),"closed source cannot invalidate retained growth bytes");
  Check(file.device->Open(path.string(),d::FileOpenMode::open_existing).ok(),"reopen actual source after retained read");
 }
 Check(f.ledger.CleanupOwner(f.binding.owner_uuid.bytes).retained_bytes==capacity,"revocation retains live source backing");
 reads=0;io_counting=true;auto revoked=call(f.binding);io_counting=false;failed(revoked);
 Check(revoked.error==M::memory_binding_failure&&!reads,"revoked resource refuses new source I/O");
 f.memory={};Check(f.manager.Snapshot().current_bytes==capacity,"result owns backing independently of memory workspace");
 if(deep)memory_probe_mutex=mutex;result={};memory_probe_mutex=nullptr;
 if(deep)Check(!memory_locked_probes,"final source backing cleanup is outside device guard");f.Empty();
 if(!deep)return;
 BundleMemoryFixture valid(capacity);
 const auto read=[&]{return db::ReadNativeManagementControlBundleWithMemoryFromOpenDevice(file,root,Id(1),bootstrap,
   limits,valid.memory,valid.binding,context,historical);};
 reads=0;io_counting=true;{const auto r=read();Check(r.ok(),"actual read failure sweep baseline");}io_counting=false;const auto sites=reads;
 // Backend error diagnostics have their own provider accounting contract.
 // Here probe the actual aligned backing lifetime, not those transient strings.
 memory_probe_backing_only=true;
 for(unsigned site=1;site<=sites;++site){reads=0;read_fault=site;io_counting=true;memory_probe_mutex=mutex;
  const auto r=read();memory_probe_mutex=nullptr;io_counting=false;read_fault=0;failed(r);
  Check(r.bundle_error==E::io_failure&&!valid.manager.Snapshot().current_bytes&&!memory_locked_probes,
   "every actual read failure releases backing after its fence");
  const auto completed=site-1;
  const auto bytes=completed?context==C::current?4096+(completed-1)*u64{page_bytes}:completed*u64{page_bytes}:0;
  Check(!r.io_status.ok()&&r.physical_bytes_read==bytes&&
   r.io_diagnostic.diagnostic_code=="SB-STORAGE-DISK-READ-SHORT",
   "actual read failure preserves backend status diagnostic and completed bytes");}
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
 historical_stats=0;historical_stat_counting=true;
 {const auto r=read();Check(r.ok(),"actual extent observation baseline");}
 historical_stat_counting=false;const auto extent_sites=historical_stats;
 Check(extent_sites==2,"both real extent observations retained");
 for(unsigned site=1;site<=extent_sites;++site){historical_stats=0;historical_stat_fault=site;historical_stat_counting=true;
  const auto r=read();historical_stat_counting=false;historical_stat_fault=0;failed(r);
  const auto bytes=context==C::current?4096+(site==2?u64{page_bytes}:0):(site==2?physical_bytes:0);
  Check(r.bundle_error==E::io_failure&&!r.io_status.ok()&&r.physical_bytes_read==bytes&&
   r.io_diagnostic.diagnostic_code=="SB-STORAGE-DISK-SIZE-FAILED"&&!valid.manager.Snapshot().current_bytes,
   "every actual extent failure retains typed diagnostic and prior reads without a payload prefix");}
#endif
 memory_probe_backing_only=false;
 BundleMemoryFixture small(capacity-1);reads=0;io_counting=true;
 auto refused=db::ReadNativeManagementControlBundleWithMemoryFromOpenDevice(file,root,Id(1),bootstrap,limits,small.memory,small.binding,context,historical);
 io_counting=false;failed(refused);Check(refused.error==M::memory_allocation_failure&&!reads,"one byte short real grant refuses before reads");
 small.memory={};small.Empty();valid.memory={};valid.Empty();
}
d::NativePageReference Self(const d::NativeCommonPageHeader& h){return {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
Bytes MapOracle(const page::NativeAllocationMap& m) {
  Bytes b(m.header.page_size_bytes, 0); const auto& h = m.header;
  std::copy_n("SBPGV002",8,b.begin()); Num(b,8,4,128); Num(b,12,4,h.page_size_bytes);
  Num(b,16,4,h.page_type); Num(b,20,2,1); Num(b,22,2,1); Put(b,24,h.database_uuid);
  Put(b,40,h.filespace_uuid); Put(b,56,h.page_uuid); Num(b,72,8,h.page_number);
  Num(b,80,8,h.page_generation); Num(b,88,8,h.flags); Put(b,104,h.page_size_profile_uuid); Num(b,120,2,1);
  u64 fnv = 14695981039346656037ull;
  for (unsigned i=0;i<128;++i) { fnv ^= b[i]; fnv *= 1099511628211ull; } Num(b,96,8,fnv);
  const auto bitmap = (m.states.size()+1)/2, at = (384+bitmap+7)&~std::size_t(7);
  Check(at<=b.size()&&m.records.size()<=(b.size()-at)/128,"map oracle fits the registered physical page");
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
  std::fill(b.begin()+312,b.begin()+344,0);const auto sha=Sha(b);std::copy(sha.begin(),sha.end(),b.begin()+312);return b;
}

Pages EncodeMaps(Maps& maps){Pages out(maps.size());for(std::size_t n=maps.size();n;--n){auto& m=maps[n-1];if(n<maps.size()){m.next=Self(maps[n].header);m.next_sha256=Sha(out[n]);}else{m.next.reset();m.next_sha256={};}const auto encoded=page::EncodeNativeAllocationMap(m);Check(encoded.ok(),"valid allocation fixture");out[n-1]=MapOracle(m);Check(out[n-1]==encoded.bytes,"independent allocation image");}return out;}
page::NativeAllocationMap& Cover(Maps& maps,u64 n){for(auto& m:maps)if(n>=m.first_page&&n-m.first_page<m.states.size())return m;throw std::runtime_error("fixture range");}
page::NativeAllocationRecord& Find(Maps& maps,u64 n){auto& m=Cover(maps,n);for(auto& r:m.records)if(r.page_number==n)return r;throw std::runtime_error("fixture record");}
struct Graph {
 Fixture& fixture;d::FilespacePageZero zero;db::NativeCheckpointRoot base,target;db::NativePublicationPlan plan;Maps before,after;Bytes base_bytes,target_bytes,plan_bytes;Pages before_bytes,after_bytes,extent;
 Graph(Fixture& f,bool retained_operation=false,unsigned extent_count=1,const Graph* previous=nullptr):fixture(f){
  const auto z=d::ReadFilespacePageZeroFromOpenDevice(f.device);Check(z.ok(),"actual page zero");zero=*z.record;
  const auto ref=std::find_if(zero.roots.begin(),zero.roots.end(),[](const auto& r){return r.kind==9;});const auto cp=db::ReadNativeCheckpointRootFromOpenDevice(f.device,Id(1),*ref);Check(cp.ok(),"actual base checkpoint");base=*cp.root;
  const auto allocation=page::ReadNativeAllocationChainFromOpenDevice(f.device,{Id(1),Id(2),zero.bootstrap.page_size_profile_uuid},f.budget);Check(allocation.ok(),"actual base allocation");for(const auto& image:allocation.pages)before.push_back(*image.map);
  if(previous){base=*db::DecodeNativeCheckpointRoot(previous->target_bytes).root;before=previous->after;}
  const unsigned offset=previous?60:0;const u64 generation=base.checkpoint_generation+1;
  if(retained_operation){auto& m=Cover(before,200);m.states[200-m.first_page]=State::quarantined;page::NativeAllocationRecord r;r.page_number=200;r.allocation_uuid=Id(850);r.owner_uuid=Id(851);r.creator_operation_uuid=Id(852);r.page_type=0x500;m.records.push_back(r);std::sort(m.records.begin(),m.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});}
  before_bytes=EncodeMaps(before);auto& root=*std::find_if(base.roots.begin(),base.roots.end(),[](const auto& r){return r.role==4;});root.sha256=Sha(before_bytes.front());const auto base_encoded=db::EncodeNativeCheckpointRoot(base);Check(base_encoded.ok(),"base fixture");base_bytes=base_encoded.bytes;
  auto o=records::Example(1);o.uuid=Id(333);o.bootstrap_uuid=zero.page_uuid;o.security_snapshot_uuid={};o.generation_guards={};
  if(previous){o=records::Example(2);o.uuid=Id(333);o.bootstrap_uuid=zero.page_uuid;o.revision=2;o.updated_at=Id(2002);}
  if(extent_count>1){o=records::Example();o.uuid=Id(333);o.bootstrap_uuid=zero.page_uuid;for(unsigned i=0;i<extent_count*100;++i){auto s=records::Step(1,i+1);s.operation_uuid=o.uuid;o.steps.push_back(s);}}
  const auto record=records::Oracle(o);const auto count=(record.size()+f.size-385)/(f.size-384);std::vector<d::NativeCommonPageHeader> headers;
  for(unsigned n=0;n<count;++n)headers.push_back({u32(f.size),0x500,Id(1),Id(2),Id(6200+offset+n),102+offset+n,generation,0,zero.bootstrap.page_size_profile_uuid});
  const auto ext=db::EncodeNativeManagementExtent(o,Id(5000+offset),headers,f.budget);Check(ext.ok(),"extent fixture");extent=records::ExtentOracle(o,headers);auto extent_root=*ext.root;for(auto& b:extent)Put(b,144,Id(5000+offset));records::Rechain(extent,extent_root);Check(extent==ext.pages,"independent extent fixture");
  plan.header={u32(f.size),0x500,Id(1),Id(2),Id(6000+offset),100+offset,generation,0,zero.bootstrap.page_size_profile_uuid};plan.object_uuid=Id(302);plan.bootstrap_uuid=zero.page_uuid;plan.timeline_uuid=base.timeline_uuid;plan.operation_uuid=Id(800+offset);plan.intent={o.initiator_uuid,o.request_context_uuid,o.policy_snapshot_uuid,o.normalized_request_sha256,o.initiator_kind};plan.security_snapshot_uuid=o.security_snapshot_uuid;plan.generation_guard_flags=0;for(unsigned i=0;i<3;++i)if(o.generation_guards[i])plan.generation_guard_flags|=1u<<i;plan.catalog_generation=o.generation_guards[0].value_or(0);plan.configuration_generation=o.generation_guards[1].value_or(0);plan.security_generation=o.generation_guards[2].value_or(0);
  plan.reservation_state_sha256.fill(9);plan.base_checkpoint=Self(base.header);plan.base_checkpoint_object_uuid=plan.target_checkpoint_object_uuid=base.object_uuid;plan.base_checkpoint_sha256=Sha(base_bytes);plan.base_checkpoint_generation=base.checkpoint_generation;plan.base_root_set_generation=base.root_set_generation;plan.reserved_generation=plan.target_root_set_generation=generation;plan.target_checkpoint={Id(2),101+offset,generation,zero.bootstrap.page_size_profile_uuid};plan.management_extent=ext.root;
  if(previous){plan.previous_plan=Self(previous->plan.header);plan.previous_plan_object_uuid=previous->plan.object_uuid;plan.previous_plan_sha256=Sha(previous->plan_bytes);}
  target=base;target.header.page_uuid=Id(6001+offset);after=before;for(unsigned n=0;n<after.size();++n){auto& m=after[n];m.header.page_number=140+offset+n;m.header.page_uuid=Id(6100+offset+n);m.header.page_generation=m.map_generation=generation;m.creator_transaction_uuid={};m.creator_local_transaction_id=0;m.creator_operation_uuid=plan.operation_uuid;}
  unsigned allocation_id=7000+offset;const auto add=[&](const d::NativeCommonPageHeader& h,const Uuid& owner){auto& m=Cover(after,h.page_number);Check(m.states[h.page_number-m.first_page]==State::free,"new slot free");m.states[h.page_number-m.first_page]=State::allocated;page::NativeAllocationRecord r;r.page_number=h.page_number;r.allocation_uuid=Id(allocation_id++);r.page_uuid=h.page_uuid;r.owner_uuid=owner;r.page_generation=h.page_generation;r.page_type=h.page_type;r.creator_operation_uuid=plan.operation_uuid;m.records.push_back(r);};
  auto target_header=target.header;target_header.page_number=101+offset;target_header.page_generation=generation;add(target_header,target.object_uuid);add(plan.header,plan.object_uuid);for(const auto& h:headers)add(h,plan.management_extent->object_uuid);for(const auto& m:after)add(m.header,m.object_uuid);
  for(auto& m:after)std::sort(m.records.begin(),m.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});Refresh();
 }
 void Refresh(){after_bytes=EncodeMaps(after);auto& root=*std::find_if(target.roots.begin(),target.roots.end(),[](const auto& r){return r.role==4;});root.page=Self(after.front().header);root.object_uuid=after.front().object_uuid;root.sha256=Sha(after_bytes.front());target_bytes=Finish(plan,target);plan_bytes=Oracle(plan);}
 u64 Budget()const{u64 n=base_bytes.size()+target_bytes.size()+plan_bytes.size();for(const auto* v:{&before_bytes,&after_bytes,&extent})for(const auto& b:*v)n+=b.size();return n;}
 CE CheckGraph(u64 budget=0)const{
  const bool bounded=!counting&&allocation_budget<0&&!hash_counting&&!hash_fault;const auto limit=budget?budget:Budget();
  const auto result=db::ValidateNativeManagementControlAllocation(base_bytes,target_bytes,plan_bytes,extent,before_bytes,after_bytes,limit);
  if(bounded)BoundedControlCheck(base_bytes,target_bytes,plan_bytes,extent,before_bytes,after_bytes,limit,{},{},nullptr,result);
  return result;}
};

using BE=db::NativeManagementControlBundleError;
using BundleRoot=db::NativeManagementControlBundleRoot;
struct Bundle {
 Graph& graph;std::vector<d::NativeCommonPageHeader> headers;BundleRoot root;Pages pages,inventory;
 Bundle(Graph& g,unsigned inventory_count=0,unsigned placement_offset=0,unsigned inventory_offset=0):graph(g){const u64 size=g.fixture.size,count=((g.after.size()+inventory_count)*size+size-385)/(size-384);
  std::vector<page::NativeTransactionInventoryPage> chain;
  for(unsigned n=0;n<inventory_count;++n){page::NativeTransactionInventoryPage p;p.header={u32(size),0x301,Id(1),Id(2),Id(9500+inventory_offset+n),220+inventory_offset+n,g.plan.reserved_generation,0,g.zero.bootstrap.page_size_profile_uuid};p.object_uuid=Id(910);p.inventory_generation=g.plan.reserved_generation;p.inventory.next_local_transaction_id=inventory_count+1;p.inventory.next_commit_sequence=2;
   mga::TransactionInventoryEntry e;e.identity.transaction_uuid={UuidKind::transaction,Id(9600+n)};e.identity.local_id=mga::MakeLocalTransactionId(n+1);e.identity.scope=mga::TransactionScope::local_node;e.state=n?mga::TransactionState::created:mga::TransactionState::committed;e.begin_unix_epoch_millis=1000+n;e.begin_visible_through_local_transaction_id=n;e.begin_visible_through_commit_sequence=n?1:0;if(!n){e.commit_sequence=1;e.final_unix_epoch_millis=2000;e.evidence_record_written=true;}p.inventory.entries.push_back(e);chain.push_back(p);
   auto& m=Cover(g.after,p.header.page_number);Check(m.states[p.header.page_number-m.first_page]==State::free,"inventory slot free");m.states[p.header.page_number-m.first_page]=State::allocated;page::NativeAllocationRecord r;r.page_number=p.header.page_number;r.allocation_uuid=Id(8500+inventory_offset+n);r.page_uuid=p.header.page_uuid;r.owner_uuid=p.object_uuid;r.page_generation=g.plan.reserved_generation;r.page_type=0x301;r.creator_operation_uuid=g.plan.operation_uuid;m.records.push_back(r);
  }
  for(unsigned n=0;n<chain.size();++n){auto& p=chain[n];if(n)p.previous=Self(chain[n-1].header);if(n+1<chain.size())p.next=Self(chain[n+1].header);const auto image=page::EncodeNativeTransactionInventoryPage(p);Check(image.ok(),"complete candidate inventory fixture");inventory.push_back(image.bytes);}
  for(unsigned n=0;n<count;++n){d::NativeCommonPageHeader h{u32(size),0x500,Id(1),Id(2),Id(9000+placement_offset+n),180+placement_offset+n,g.plan.reserved_generation,0,g.zero.bootstrap.page_size_profile_uuid};headers.push_back(h);auto& m=Cover(g.after,h.page_number);Check(m.states[h.page_number-m.first_page]==State::free,"bundle slot free");m.states[h.page_number-m.first_page]=State::allocated;page::NativeAllocationRecord r;r.page_number=h.page_number;r.allocation_uuid=Id(8000+placement_offset+n);r.page_uuid=h.page_uuid;r.owner_uuid=Id(900+placement_offset);r.page_generation=g.plan.reserved_generation;r.page_type=0x500;r.creator_operation_uuid=g.plan.operation_uuid;m.records.push_back(r);}
  for(auto& m:g.after)std::sort(m.records.begin(),m.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});g.Refresh();
  root.first=Self(headers.front());root.object_uuid=Id(900+placement_offset);root.operation_uuid=g.plan.operation_uuid;root.map_count=g.after.size();root.inventory_count=inventory_count;root.page_count=headers.size();Oracle();
 }
 u64 Budget()const{return root.page_count*graph.fixture.size+4*(root.map_count+root.inventory_count)*graph.fixture.size+2*graph.fixture.size;}
 void Reseal(){std::array<byte,32> next{};for(std::size_t n=pages.size();n;--n){auto& b=pages[n-1];std::copy(next.begin(),next.end(),b.begin()+288);std::fill(b.begin()+320,b.begin()+352,0);const auto hash=Sha(b);std::copy(hash.begin(),hash.end(),b.begin()+320);next=Sha(b);}root.first_page_sha256=next;}
 void Oracle(){const u64 size=graph.fixture.size,capacity=size-384;Bytes payload;for(const auto* chain:{&graph.after_bytes,&inventory})for(const auto& b:*chain)payload.insert(payload.end(),b.begin(),b.end());root.aggregate_sha256=Sha(payload);pages.assign(headers.size(),Bytes(size));
  for(unsigned i=0;i<pages.size();++i){const auto& h=headers[i];auto& b=pages[i];std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,size);Num(b,16,4,0x500);Num(b,20,2,1);Num(b,22,2,1);Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);HeaderSeal(b);const u64 offset=i*capacity,length=std::min<u64>(capacity,payload.size()-offset);
   std::copy_n(root.inventory_count?"SBMCB002":"SBMCB001",8,b.begin()+128);Num(b,136,2,root.inventory_count?2:1);Num(b,138,2,256);Num(b,140,4,384+length);Put(b,144,root.object_uuid);Put(b,160,graph.zero.page_uuid);Put(b,176,root.operation_uuid);Num(b,192,8,root.map_count);Num(b,200,8,payload.size());Num(b,208,8,offset);Num(b,216,8,i);Num(b,224,8,root.page_count);Num(b,232,8,root.first.page_number);std::copy(root.aggregate_sha256.begin(),root.aggregate_sha256.end(),b.begin()+240);Num(b,272,4,length);Num(b,276,8,root.inventory_count);std::copy_n(payload.begin()+offset,length,b.begin()+384);}
  Reseal();
 }
 auto Encode()const{return db::EncodeNativeManagementControlBundle(graph.after_bytes,Id(1),graph.zero.page_uuid,root.object_uuid,root.operation_uuid,headers,Budget(),inventory);}
 auto Decode(u64 budget=0)const{return DecodeBundle(pages,root,Id(1),graph.zero.page_uuid,budget?budget:Budget());}
 auto Read()const{return db::ReadNativeManagementControlBundleFromOpenDevice(graph.fixture.devices.front(),root,Id(1),graph.zero.page_uuid,Budget());}
 void Install(){for(std::size_t n=0;n<pages.size();++n){const auto& b=pages[n];const auto io=graph.fixture.device.WriteAt((root.first.page_number+n)*graph.fixture.size,b.data(),b.size());Check(io.ok()&&io.bytes_transferred==b.size(),"isolated bundle fixture write");}Check(graph.fixture.device.Sync().ok(),"isolated bundle fixture sync");}
 void Good(const db::NativeManagementControlBundleRead& r)const{Check(r.ok()&&r.allocation_images==graph.after_bytes&&r.inventory_images==inventory&&r.page_headers.size()==headers.size()&&r.total_pages==graph.after.front().total_pages,"complete original target map and inventory images");for(unsigned n=0;n<headers.size();++n)Check(r.page_headers[n].page_uuid==headers[n].page_uuid&&Self(r.page_headers[n])==Self(headers[n]),"verified bundle page bindings");}
};
void Empty(const db::NativeManagementControlBundleRead& r){Check(!r.ok()&&r.allocation_images.empty()&&r.inventory_images.empty()&&r.directory_images.empty()&&r.growth_images.empty()&&r.page_headers.empty()&&!r.total_pages,"no failed bundle result prefix");}
void Empty(const db::NativeManagementControlBundleImage& r){Check(!r.ok()&&!r.root&&r.pages.empty(),"no failed bundle image prefix");}
Bytes SelectorOracle(const db::NativeCheckpointSelection& s){const auto& h=s.header;Bytes b(h.page_size_bytes);std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,0x30e);Num(b,20,2,1);Num(b,22,2,1);Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);HeaderSeal(b);
 std::copy_n("SBDCP001",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,384);Num(b,140,4,512);Put(b,144,s.object_uuid);Put(b,160,s.bootstrap_uuid);Num(b,176,8,s.selection_generation);Put(b,184,s.publication_uuid);Ref(b,200,s.checkpoint);Put(b,248,s.checkpoint_object_uuid);std::copy(s.checkpoint_sha256.begin(),s.checkpoint_sha256.end(),b.begin()+264);Num(b,296,8,s.checkpoint_generation);Num(b,304,8,s.root_set_generation);Put(b,312,s.timeline_uuid);Num(b,328,8,s.previous_selection_generation);if(s.previous_checkpoint)Ref(b,336,*s.previous_checkpoint);Put(b,384,s.previous_checkpoint_object_uuid);std::copy(s.previous_checkpoint_sha256.begin(),s.previous_checkpoint_sha256.end(),b.begin()+400);const auto sha=Sha(b);std::copy(sha.begin(),sha.end(),b.begin()+432);return b;}

void Integration(Fixture& f,unsigned profile){
 Graph g(f);Bundle b(g);g.plan.control_bundle=b.root;g.Refresh();
 const auto inspect=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);Check(inspect.ok(),"actual coordinator before bundle fixture");
 auto reserved=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*inspect.snapshot,g.plan.operation_uuid,f.budget,&g.plan.intent);Check(reserved.ok(),"actual retained lease");g.plan.reservation_state_sha256=reserved.lease->snapshot().state_sha256;g.Refresh();
 reads=writes=syncs=0;io_counting=true;const auto refused=db::InstallNativeManagementPublicationOnLease(*reserved.lease,g.plan,g.target_bytes,g.extent,f.budget);io_counting=false;Check(refused.error==db::NativePublicationError::invalid_request&&!reads&&!writes&&!syncs,"extent-only installer cannot acknowledge missing bundle");reserved.lease.reset();
 const u64 allowance=g.Budget()+b.pages.size()*f.size;const auto delta=[&](){return db::ValidateNativeManagementControlAllocation(g.base_bytes,g.target_bytes,g.plan_bytes,g.extent,g.before_bytes,g.after_bytes,allowance,b.pages);};
 Check(delta()==CE::none,"complete version3 control delta includes bundle");Check(db::ValidateNativeManagementControlAllocation(g.base_bytes,g.target_bytes,g.plan_bytes,g.extent,g.before_bytes,g.after_bytes,allowance)!=CE::none,"missing declared bundle rejected");
 Graph alternate(f);Bundle different(alternate);alternate.plan.control_bundle=different.root;Find(alternate.after,100).allocation_uuid=Id(9999);alternate.Refresh();Check(db::ValidateNativeManagementControlAllocation(alternate.base_bytes,alternate.target_bytes,alternate.plan_bytes,alternate.extent,alternate.before_bytes,alternate.after_bytes,alternate.Budget()+different.pages.size()*f.size,different.pages)==CE::binding_mismatch,"bundle must retain exact intended allocation UUIDs");
 const auto plan_image=db::EncodeNativePublicationPlan(g.plan);Check(plan_image.ok()&&plan_image.bytes==Oracle(g.plan),"independent version3 plan");const auto decoded=db::DecodeNativePublicationPlan(g.plan_bytes);Check(decoded.ok()&&decoded.plan->control_bundle==g.plan.control_bundle,"version3 root round trip");
 for(unsigned fault=0;fault<5;++fault){auto bad=g.plan;switch(fault){case 0:bad.control_bundle->first.page_number=bad.header.page_number;break;case 1:bad.control_bundle->first.page_number=bad.management_extent->first.page_number;break;case 2:bad.control_bundle->object_uuid=bad.management_extent->object_uuid;break;case 3:bad.control_bundle->operation_uuid=Id(999);break;case 4:bad.management_extent.reset();break;}Failed(db::EncodeNativePublicationPlan(bad));}
 for(unsigned at:{136u,138u,140u,1040u,1151u,1152u}){auto raw=g.plan_bytes;raw[at]^=1;PlanSeal(raw);Failed(db::DecodeNativePublicationPlan(raw));}
 const auto write=[&](u64 n,const Bytes& bytes){const auto io=f.device.WriteAt(n*f.size,bytes.data(),bytes.size());Check(io.ok()&&io.bytes_transferred==bytes.size(),"isolated selected fixture write");};
 write(g.plan.header.page_number,g.plan_bytes);write(g.plan.target_checkpoint.page_number,g.target_bytes);for(unsigned n=0;n<g.extent.size();++n)write(g.plan.management_extent->first.page_number+n,g.extent[n]);for(unsigned n=0;n<g.after_bytes.size();++n)write(g.after[n].header.page_number,g.after_bytes[n]);b.Install();
 for(unsigned i=0;i<2;++i){const auto root=std::find_if(g.zero.roots.begin(),g.zero.roots.end(),[&](const auto& r){return r.kind==18+i;});const auto old=db::DecodeNativeCheckpointSelection(f.Read(root->page_number));Check(old.ok(),"actual initial selector");auto s=*old.selection;s.previous_selection_generation=s.selection_generation;s.previous_checkpoint=s.checkpoint;s.previous_checkpoint_object_uuid=s.checkpoint_object_uuid;s.previous_checkpoint_sha256=s.checkpoint_sha256;++s.selection_generation;s.publication_uuid=g.plan.operation_uuid;s.checkpoint=g.plan.target_checkpoint;s.checkpoint_object_uuid=g.plan.target_checkpoint_object_uuid;s.checkpoint_sha256=Sha(g.target_bytes);s.checkpoint_generation=g.plan.reserved_generation;s.root_set_generation=g.plan.target_root_set_generation;const auto encoded_selector=db::EncodeNativeCheckpointSelection(s);const auto raw=SelectorOracle(s);Check(encoded_selector.ok()&&encoded_selector.bytes==raw,"independent selected fixture");write(root->page_number,raw);}Check(f.device.Sync().ok(),"selected fixture barrier");
 const auto history=[&](){return db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),f.budget);};const auto selected=history();Check(selected.ok()&&selected.entries.size()==1&&selected.entries[0].bundle_pages.size()==b.headers.size()&&selected.entries[0].control_allocation_images==g.after_bytes,"selected physical history reads complete bundle");
 auto broken=b.pages.front();broken.back()^=1;write(b.root.first.page_number,broken);Check(f.device.Sync().ok(),"corrupt selected bundle fixture");const auto missing=history();Check(!missing.ok()&&!missing.selection&&missing.entries.empty()&&missing.latest.empty()&&missing.idempotency.empty(),"broken selected bundle exposes no history prefix");b.Install();
 const auto select_hash=[&](const std::array<byte,32>& sha){for(unsigned i=0;i<2;++i){const auto root=std::find_if(g.zero.roots.begin(),g.zero.roots.end(),[&](const auto& r){return r.kind==18+i;});auto s=*db::DecodeNativeCheckpointSelection(f.Read(root->page_number)).selection;s.checkpoint_sha256=sha;write(root->page_number,SelectorOracle(s));}Check(f.device.Sync().ok(),"resealed selector fixture");};
 auto wrong_maps=*db::DecodeNativeCheckpointRoot(g.target_bytes).root;*std::find_if(wrong_maps.roots.begin(),wrong_maps.roots.end(),[](const auto& r){return r.role==4;})=*std::find_if(g.base.roots.begin(),g.base.roots.end(),[](const auto& r){return r.role==4;});auto redirected=g.plan;const auto wrong_target=Finish(redirected,wrong_maps);write(g.plan.header.page_number,Oracle(redirected));write(g.plan.target_checkpoint.page_number,wrong_target);select_hash(Sha(wrong_target));const auto mismatched=history();Check(!mismatched.ok()&&!mismatched.selection&&mismatched.entries.empty(),"selected checkpoint binds the exact bundled allocation root");write(g.plan.header.page_number,g.plan_bytes);write(g.plan.target_checkpoint.page_number,g.target_bytes);select_hash(Sha(g.target_bytes));Check(history().ok(),"restore actual selected bundle fixture");
 if(profile)return;
 for(unsigned mode=0;mode<2;++mode){const auto call=[&](){if(!mode)return int(delta());const auto r=history();if(!r.ok())Check(!r.selection&&r.entries.empty()&&r.latest.empty()&&r.idempotency.empty()&&!r.verified_image_bytes,"no version3 history failure prefix");return int(r.error);};const int resource=mode?int(db::NativeManagementHistoryError::resource_exhausted):int(CE::resource_exhausted),hash_error=mode?int(db::NativeManagementHistoryError::hash_failure):int(CE::hash_failure);
  counting=true;allocations=0;Check(call()==0,"version3 consumer allocation baseline");counting=false;const auto sites=allocations;for(unsigned long at=0;at<sites;++at){const auto loss=f.device.failed_io_latency_observations();allocation_budget=at;const auto e=call();allocation_budget=-1;Check(e==resource||(mode&&e==0&&f.device.failed_io_latency_observations()==loss+1),"version3 consumer required allocation classification");}
  hash_counting=true;hash_seen=0;Check(call()==0,"version3 consumer hash baseline");hash_counting=false;const auto hashes=hash_seen;for(unsigned fault=1;fault<=5;++fault)for(unsigned at=1;at<=hashes;++at){hash_fault=fault;hash_target=at;hash_seen=0;hash_active=false;const auto e=call();const bool consumed=!hash_fault;hash_fault=0;Check(consumed&&e==hash_error,"version3 consumer complete hash failure propagation");}
  std::cout<<"bundle consumer mode="<<mode<<" allocations="<<sites<<" hashes="<<hashes<<"\n";
 }
 reads=writes=syncs=0;io_counting=true;Check(history().ok(),"version3 history read baseline");io_counting=false;const auto sites=reads;Check(!writes&&!syncs,"history composition performs no writes");for(unsigned at=1;at<=sites;++at){reads=0;read_fault=at;io_counting=true;const auto r=history();io_counting=false;read_fault=0;Check(r.error==db::NativeManagementHistoryError::io_failure&&!r.selection&&r.entries.empty()&&r.latest.empty()&&r.idempotency.empty(),"every version3 history read failure");}
}
Bytes InventoryOracle(const page::NativeTransactionInventoryPage& p){
 const auto& h=p.header;Bytes b(h.page_size_bytes);std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,0x301);Num(b,20,2,1);Num(b,22,2,1);Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Num(b,88,8,h.flags);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);
 u64 header_hash=14695981039346656037ull;for(unsigned i=0;i<128;++i){header_hash^=b[i];header_hash*=1099511628211ull;}Num(b,96,8,header_hash);
 std::copy_n("SBTINV01",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,256);Num(b,140,4,384+72*p.inventory.entries.size());Put(b,144,p.object_uuid);Num(b,160,8,p.inventory_generation);Num(b,168,8,p.inventory.next_local_transaction_id);Num(b,176,8,p.inventory.next_commit_sequence);Num(b,184,4,p.inventory.entries.size());if(p.previous)Ref(b,192,*p.previous);if(p.next)Ref(b,240,*p.next);
 u64 oit=p.inventory.next_local_transaction_id,oat=oit;
 for(std::size_t i=0;i<p.inventory.entries.size();++i){const auto& e=p.inventory.entries[i];const auto at=384+72*i;Num(b,at,8,e.identity.local_id.value);Put(b,at+8,e.identity.transaction_uuid.value);Num(b,at+24,2,u16(e.identity.scope));Num(b,at+26,2,u16(e.state));const unsigned origin=e.archived_from_state==mga::TransactionState::committed?1:e.archived_from_state==mga::TransactionState::rolled_back?2:e.archived_from_state==mga::TransactionState::failed_terminal?3:0;Num(b,at+28,4,(e.evidence_record_required?1:0)|(e.evidence_record_written?2:0)|(e.rollback_only?4:0)|(origin<<3));Num(b,at+32,8,e.begin_unix_epoch_millis);Num(b,at+40,8,e.final_unix_epoch_millis);Num(b,at+48,8,e.begin_visible_through_local_transaction_id);Num(b,at+56,8,e.begin_visible_through_commit_sequence);Num(b,at+64,8,e.commit_sequence);
  const auto outcome=e.state==mga::TransactionState::archived?e.archived_from_state:e.state;if(outcome!=mga::TransactionState::committed&&outcome!=mga::TransactionState::rolled_back)oit=std::min(oit,e.identity.local_id.value);if(e.state==mga::TransactionState::active||e.state==mga::TransactionState::read_only_active)oat=std::min(oat,e.identity.local_id.value);
 }
 Num(b,288,8,oit);Num(b,296,8,oat);Num(b,304,8,oat);std::fill(b.begin()+320,b.begin()+352,0);const auto seal=Sha(b);std::copy(seal.begin(),seal.end(),b.begin()+320);return b;
}
Bytes DirectoryImageOracle(const page::NativeFilespaceDirectory& p){
 const auto& h=p.header;Bytes b(h.page_size_bytes);std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,9);Num(b,20,2,1);Num(b,22,2,1);Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);HeaderSeal(b);
 std::copy_n("SBFDIR02",8,b.begin()+128);Num(b,136,2,2);Num(b,138,2,256);Num(b,140,4,384+320*p.records.size());Put(b,144,p.object_uuid);Num(b,160,8,p.directory_generation);Put(b,168,p.creator_transaction_uuid);Num(b,184,8,p.creator_local_transaction_id);Num(b,192,8,p.total_records);Num(b,200,8,p.first_record);Num(b,208,4,p.records.size());Num(b,212,4,320);if(p.next)Ref(b,216,*p.next);std::copy(p.next_sha256.begin(),p.next_sha256.end(),b.begin()+264);Put(b,328,p.creator_operation_uuid);
 for(std::size_t i=0;i<p.records.size();++i){const auto& r=p.records[i];const auto& a=r.bootstrap;const auto at=384+320*i;Put(b,at,a.filespace_uuid);Put(b,at+16,a.page_size_profile_uuid);Put(b,at+32,a.checksum_profile_uuid);Put(b,at+48,a.encryption_profile_uuid);Put(b,at+64,r.locator_uuid);Put(b,at+80,r.page_zero_uuid);Num(b,at+96,8,r.page_zero_generation);Num(b,at+104,8,r.root_set_generation);Num(b,at+112,8,r.total_pages);Num(b,at+120,8,r.verification_epoch);Num(b,at+128,2,a.filespace_role);Num(b,at+130,2,a.lifecycle_state);Num(b,at+132,4,a.flags);Num(b,at+136,4,a.page_size_bytes);Num(b,at+140,4,a.durable_format_generation);if(r.operation)Ref(b,at+144,*r.operation);if(r.allocation_root){const auto& root=*r.allocation_root;Ref(b,at+192,root.page);Put(b,at+240,root.object_uuid);std::copy(root.sha256.begin(),root.sha256.end(),b.begin()+at+256);Num(b,at+288,8,root.map_generation);Num(b,at+296,8,root.capacity_generation);}}
 const auto sha=Sha(b);std::copy(sha.begin(),sha.end(),b.begin()+296);return b;
}
struct DirectoryBundle {
 Fixture& fixture;d::FilespacePageZero zero;Maps maps;std::vector<page::NativeTransactionInventoryPage> inventories;
 std::vector<page::NativeFilespaceDirectory> directories;std::vector<d::NativeCommonPageHeader> headers;BundleRoot root;Pages map_images,inventory_images,directory_images,growth_images,pages;
 DirectoryBundle(Fixture& f,unsigned secondary_profile,bool reverse,int growing=-1):fixture(f){
  const auto z=d::ReadFilespacePageZeroFromOpenDevice(f.device);Check(z.ok(),"mixed bundle primary bootstrap");zero=*z.record;
  const auto other=reverse?Id(0):Id(7);const auto& q=d::kCanonicalFilespacePageProfiles[secondary_profile];
  root.object_uuid=Id(10001);root.operation_uuid=Id(10002);root.map_count=2;root.inventory_count=2;root.directory_count=2;
  root.payload_bytes=4*f.size+2*u64{q.page_size_bytes}+48;
  const bool split_primary=growing==1&&f.size==8192&&q.page_size_bytes==131072;
  if(growing>=0){root.growth_image_count=2;root.payload_bytes+=2*(growing?q.page_size_bytes:f.size)+16;}
  if(split_primary){++root.map_count;root.payload_bytes+=f.size+8;}
  root.page_count=(root.payload_bytes+f.size-385)/(f.size-384);
  for(unsigned i=0;i<root.page_count;++i)headers.push_back({u32(f.size),0x500,Id(1),Id(2),Id(10100+i),(growing>=0?40u:180u)+i,2,0,zero.bootstrap.page_size_profile_uuid});root.first=Self(headers.front());
  for(unsigned i=0;i<2;++i){const auto fs=i?other:Id(2);const auto profile=i?q.uuid:zero.bootstrap.page_size_profile_uuid;const auto size=i?q.page_size_bytes:u32(f.size);
   page::NativeAllocationMap m;m.header={size,3,Id(1),fs,Id(10200+i),140,2,0,profile};m.object_uuid=Id(10300+i);m.map_generation=2;m.capacity_generation=1;m.total_pages=256;m.creator_operation_uuid=root.operation_uuid;m.states.resize(256,State::free);maps.push_back(m);
   page::NativeTransactionInventoryPage inventory;inventory.header={size,0x301,Id(1),fs,Id(10400+i),222,2,0,profile};inventory.object_uuid=Id(10410);inventory.inventory_generation=2;inventory.inventory.next_local_transaction_id=3;inventory.inventory.next_commit_sequence=1;
   mga::TransactionInventoryEntry e;e.identity.transaction_uuid={UuidKind::transaction,Id(10420+i)};e.identity.local_id=mga::MakeLocalTransactionId(i+1);e.identity.scope=mga::TransactionScope::local_node;e.state=mga::TransactionState::created;e.begin_unix_epoch_millis=1000+i;e.begin_visible_through_local_transaction_id=i;inventory.inventory.entries.push_back(e);inventories.push_back(inventory);
   page::NativeFilespaceDirectory dir;dir.header={u32(f.size),9,Id(1),Id(2),Id(10500+i),220+i,2,0,zero.bootstrap.page_size_profile_uuid};dir.object_uuid=Id(10510);dir.directory_generation=2;dir.creator_operation_uuid=root.operation_uuid;dir.total_records=2;dir.first_record=i;directories.push_back(dir);
  }
  inventories[0].next=Self(inventories[1].header);inventories[1].previous=Self(inventories[0].header);
  unsigned allocation=11000;const auto add=[&](const d::NativeCommonPageHeader& h,const Uuid& owner){auto& map=h.filespace_uuid==Id(2)?maps[0]:maps[1];Check(map.states[h.page_number]==State::free,"mixed bundle disjoint physical image slots");map.states[h.page_number]=State::allocated;
   page::NativeAllocationRecord r;r.page_number=h.page_number;r.allocation_uuid=Id(allocation++);r.page_uuid=h.page_uuid;r.owner_uuid=owner;r.page_generation=2;r.page_type=h.page_type;r.creator_operation_uuid=root.operation_uuid;map.records.push_back(r);};
  for(const auto& h:headers)add(h,root.object_uuid);for(const auto& m:maps)add(m.header,m.object_uuid);for(const auto& p:inventories)add(p.header,p.object_uuid);for(const auto& p:directories)add(p.header,p.object_uuid);
  for(auto& m:maps)std::sort(m.records.begin(),m.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});std::sort(maps.begin(),maps.end(),[](const auto& a,const auto& b){return a.header.filespace_uuid<b.header.filespace_uuid;});
  for(unsigned i=0;i<2;++i){const auto& m=maps[i];page::NativeFilespaceDirectoryRecord r;r.bootstrap=zero.bootstrap;r.bootstrap.filespace_uuid=m.header.filespace_uuid;r.bootstrap.page_size_profile_uuid=m.header.page_size_profile_uuid;r.bootstrap.page_size_bytes=m.header.page_size_bytes;
   if(m.header.filespace_uuid!=Id(2))r.bootstrap.filespace_role=5;r.locator_uuid=Id(10600+i);r.page_zero_uuid=m.header.filespace_uuid==Id(2)?zero.page_uuid:Id(10610);r.page_zero_generation=m.header.filespace_uuid==Id(2)?zero.page_generation:1;r.root_set_generation=m.header.filespace_uuid==Id(2)?zero.root_set_generation:1;r.total_pages=256;directories[i].records.push_back(r);}
  directories[0].next=Self(directories[1].header);
  if(growing>=0){const auto fs=growing?other:Id(2);auto& map=*std::find_if(maps.begin(),maps.end(),[&](const auto& m){return m.header.filespace_uuid==fs;});
    auto& member=std::find_if(directories.begin(),directories.end(),[&](const auto& dir){return dir.records.front().bootstrap.filespace_uuid==fs;})->records.front();
    auto before=zero;before.bootstrap=member.bootstrap;before.page_uuid=member.page_zero_uuid;
    if(growing){before.roots={{3,3,fs,1,1,q.uuid,Id(13000)}};before.page_generation=7;before.root_set_generation=9;}
    const auto initial=d::EncodeFilespacePageZero(before);Check(initial.ok(),"canonical original growth image");
    auto after=before;++after.page_generation;++after.root_set_generation;after.total_pages+=4;
    map.total_pages=after.total_pages;map.capacity_generation++;map.states.resize(after.total_pages,State::free);map.states[0]=State::allocated;
    page::NativeAllocationRecord z;z.allocation_uuid=Id(13001);z.page_uuid=after.page_uuid;z.owner_uuid=fs;z.page_generation=after.page_generation;
    z.page_type=growing?2:1;z.creator_operation_uuid=root.operation_uuid;map.records.insert(map.records.begin(),z);
    if(reverse){for(u64 n=after.total_pages-2;n<after.total_pages;++n){map.states[n]=State::preallocated;
      page::NativeAllocationRecord r;r.page_number=n;r.allocation_uuid=Id(16000+n);r.owner_uuid=Id(16001);r.page_type=0x100;r.creator_operation_uuid=root.operation_uuid;map.records.push_back(r);}}
    after.free_pages=std::count(map.states.begin(),map.states.end(),State::free);after.preallocated_pages=std::count(map.states.begin(),map.states.end(),State::preallocated);
    const auto final=d::EncodeFilespacePageZero(after);Check(final.ok(),"canonical resulting growth image");growth_images={*initial.bytes,*final.bytes};
    member.page_zero_generation=after.page_generation;member.root_set_generation=after.root_set_generation;member.total_pages=after.total_pages;
  }
  if(split_primary){auto head=std::find_if(maps.begin(),maps.end(),[](const auto& m){return m.header.filespace_uuid==Id(2);});
    auto tail=*head;tail.header.page_number=141;tail.header.page_uuid=Id(13010);tail.first_page=80;tail.states.erase(tail.states.begin(),tail.states.begin()+80);
    tail.records.erase(tail.records.begin(),std::lower_bound(tail.records.begin(),tail.records.end(),u64{80},[](const auto& r,u64 n){return r.page_number<n;}));
    head->states.resize(80);head->records.erase(std::lower_bound(head->records.begin(),head->records.end(),u64{80},[](const auto& r,u64 n){return r.page_number<n;}),head->records.end());
    page::NativeAllocationRecord r;r.page_number=141;r.allocation_uuid=Id(13011);r.page_uuid=tail.header.page_uuid;r.page_generation=2;r.page_type=3;r.owner_uuid=tail.object_uuid;r.creator_operation_uuid=root.operation_uuid;
    tail.states[141-80]=State::allocated;tail.records.push_back(r);std::sort(tail.records.begin(),tail.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});
    head->next=Self(tail.header);maps.insert(head+1,std::move(tail));
  }
  Refresh();
 }
 u64 Budget()const{return root.page_count*fixture.size+4*root.payload_bytes+2*fixture.size;}
 void Refresh(){map_images.clear();inventory_images.clear();directory_images.resize(2);
  map_images.resize(maps.size());for(std::size_t left=maps.size();left;--left){const auto i=left-1;auto& m=maps[i];if(m.next){Check(i+1<maps.size(),"map successor exists");m.next_sha256=Sha(map_images[i+1]);}
    const auto encoded=page::EncodeNativeAllocationMap(m);const auto raw=MapOracle(m);Check(encoded.ok()&&encoded.bytes==raw,"independent mixed bundle map image");map_images[i]=raw;}
  for(const auto& p:inventories){const auto raw=InventoryOracle(p);const auto encoded=page::EncodeNativeTransactionInventoryPage(p);Check(encoded.ok()&&encoded.bytes==raw,"independent mixed inventory image");inventory_images.push_back(raw);}
  for(unsigned i=0;i<2;++i){auto& r=directories[i].records.front();const auto at=std::find_if(maps.begin(),maps.end(),[&](const auto& m){return m.header.filespace_uuid==r.bootstrap.filespace_uuid;})-maps.begin();const auto& m=maps[at];r.allocation_root=page::NativeFilespaceAllocationRoot{Self(m.header),m.object_uuid,Sha(map_images[at]),m.map_generation,m.capacity_generation};}
  for(unsigned left=2;left;--left){const auto i=left-1;if(!i)directories[0].next_sha256=Sha(directory_images[1]);const auto raw=DirectoryImageOracle(directories[i]);const auto encoded=page::EncodeNativeFilespaceDirectory(directories[i]);Check(encoded.ok()&&encoded.bytes==raw,"independent mixed directory image");directory_images[i]=raw;}Oracle();
 }
 void Oracle(){Bytes payload;for(const auto* group:{&map_images,&inventory_images,&directory_images,&growth_images})for(const auto& raw:*group){const auto at=payload.size();payload.resize(at+8);Num(payload,at,8,raw.size());payload.insert(payload.end(),raw.begin(),raw.end());}
  Check(payload.size()==root.payload_bytes,"independent framed payload exact length");root.aggregate_sha256=Sha(payload);pages.assign(headers.size(),Bytes(fixture.size));
  for(unsigned i=0;i<pages.size();++i){auto& b=pages[i];const auto& h=headers[i];std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,fixture.size);Num(b,16,4,0x500);Num(b,20,2,1);Num(b,22,2,1);Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);HeaderSeal(b);
   const u64 offset=i*(fixture.size-384),length=std::min<u64>(fixture.size-384,payload.size()-offset);std::copy_n("SBMCB003",8,b.begin()+128);Num(b,136,2,3);Num(b,138,2,256);Num(b,140,4,384+length);Put(b,144,root.object_uuid);Put(b,160,zero.page_uuid);Put(b,176,root.operation_uuid);Num(b,192,8,root.map_count);Num(b,200,8,root.payload_bytes);Num(b,208,8,offset);Num(b,216,8,i);Num(b,224,8,root.page_count);Num(b,232,8,root.first.page_number);std::copy(root.aggregate_sha256.begin(),root.aggregate_sha256.end(),b.begin()+240);Num(b,272,4,length);Num(b,276,8,root.inventory_count);Num(b,284,4,8);Num(b,352,8,root.directory_count);std::copy_n(payload.begin()+offset,length,b.begin()+384);}
  if(root.growth_image_count)for(auto& b:pages){std::copy_n("SBMCB004",8,b.begin()+128);Num(b,136,2,4);Num(b,360,8,root.growth_image_count);}
  Reseal();
 }
 void ResealPayload(){Bytes payload;for(const auto& raw:pages){const auto length=LoadLittle32(raw.data()+272);payload.insert(payload.end(),raw.begin()+384,raw.begin()+384+length);}root.aggregate_sha256=Sha(payload);for(auto& raw:pages)std::copy(root.aggregate_sha256.begin(),root.aggregate_sha256.end(),raw.begin()+240);Reseal();}
 void Reseal(){std::array<byte,32> next{};for(std::size_t n=pages.size();n;--n){auto& raw=pages[n-1];std::copy(next.begin(),next.end(),raw.begin()+288);std::fill(raw.begin()+320,raw.begin()+352,0);const auto seal=Sha(raw);std::copy(seal.begin(),seal.end(),raw.begin()+320);next=Sha(raw);}root.first_page_sha256=next;}
 auto Encode()const{return db::EncodeNativeManagementControlBundle(map_images,Id(1),zero.page_uuid,root.object_uuid,root.operation_uuid,headers,Budget(),inventory_images,directory_images,growth_images);}
 auto Decode(u64 budget=0)const{return DecodeBundle(pages,root,Id(1),zero.page_uuid,budget?budget:Budget());}
 auto Read()const{return db::ReadNativeManagementControlBundleFromOpenDevice(fixture.devices.front(),root,Id(1),zero.page_uuid,Budget());}
 void Install(){for(std::size_t i=0;i<pages.size();++i){const auto r=fixture.device.WriteAt((root.first.page_number+i)*fixture.size,pages[i].data(),pages[i].size());Check(r.ok()&&r.bytes_transferred==pages[i].size(),"isolated actual mixed bundle write");}Check(fixture.device.Sync().ok(),"actual mixed bundle barrier");}
 void Good(const db::NativeManagementControlBundleRead& r)const{Check(r.ok()&&r.allocation_images==map_images&&r.inventory_images==inventory_images&&r.directory_images==directory_images&&r.total_pages==256&&r.page_headers.size()==headers.size(),"complete exact original mixed bundle reconstruction");}
};
void GrowthBundle(unsigned primary,unsigned secondary,bool reverse,int growing){
 std::cout<<"growth bundle primary="<<primary<<" secondary="<<secondary<<" reverse="<<reverse<<" growing="<<growing<<'\n';
 Fixture f(primary);DirectoryBundle b(f,secondary,reverse,growing);
 BoundedBundleChecks(b.pages,b.root,Id(1),b.zero.page_uuid,b.Budget());
 const auto old_primary=f.Read(0);
 const auto historical=[&]{return db::ReadNativeManagementControlBundleAtHistoricalPageZeroFromOpenDevice(f.devices.front(),b.root,Id(1),b.zero.page_uuid,old_primary,b.Budget());};
 const auto historical_result=[&]{return db::ReadNativeManagementControlBundleAtHistoricalResultFromOpenDevice(f.devices.front(),b.root,Id(1),b.zero.page_uuid,growing?old_primary:b.growth_images[1],b.Budget());};
 const auto good=[&](const db::NativeManagementControlBundleRead& r){Check(r.ok()&&r.growth_images==b.growth_images&&r.allocation_images==b.map_images&&
   r.inventory_images==b.inventory_images&&r.directory_images==b.directory_images&&r.total_pages==(growing?256:260),"exact original growth reconstruction images");};
 const auto encoded=b.Encode();Check(encoded.ok()&&encoded.root==b.root&&encoded.pages==b.pages,"independent version4 frames and seals");good(b.Decode());
 Empty(b.Decode(b.Budget()-1));b.Install();good(historical());
 if(!growing)Empty(historical_result());else good(historical_result());
 if(!growing){Empty(b.Read());const byte tail=0;Check(f.device.WriteAt(260*f.size-1,&tail,1).ok(),"fixture grown extent");
   Empty(b.Read());Check(f.device.WriteAt(0,b.growth_images[1].data(),b.growth_images[1].size()).ok()&&f.device.Sync().ok(),"fixture complete after metadata");}
 good(b.Read());good(historical());good(historical_result());
 for(const auto context:{db::NativeManagementControlReadContext::current,db::NativeManagementControlReadContext::historical_before,
     db::NativeManagementControlReadContext::historical_result}){
   const Bytes retained=context==db::NativeManagementControlReadContext::current?Bytes{}:
     context==db::NativeManagementControlReadContext::historical_before?old_primary:growing?old_primary:b.growth_images[1];
   BundleDeviceMemoryChecks(f.devices.front(),b.root,b.zero.page_uuid,b.Budget(),b.pages,context,retained,f.path,primary==secondary&&!reverse);
 }
 Empty(db::ReadNativeManagementControlBundleAtHistoricalResultFromOpenDevice(f.devices.front(),b.root,Id(1),b.zero.page_uuid,growing?old_primary:b.growth_images[1],b.Budget()-1));
 {const auto& raw=growing?old_primary:b.growth_images[1];const auto decoded=d::DecodeFilespacePageZero(raw.data(),raw.size());Check(decoded.ok(),"historical result context fixture");
  for(unsigned field=0;field<3;++field){auto wrong=*decoded.record;if(!field)++wrong.page_generation;if(field==1)++wrong.root_set_generation;if(field==2)--wrong.free_pages;
   const auto image=d::EncodeFilespacePageZero(wrong);Check(image.ok(),"canonical wrong historical result");
   if(field==2&&growing)continue; // Non-growth counters are not directory authority.
   Empty(db::ReadNativeManagementControlBundleAtHistoricalResultFromOpenDevice(f.devices.front(),b.root,Id(1),b.zero.page_uuid,*image.bytes,b.Budget()));}}
 if(!growing)Empty(db::ReadNativeManagementControlBundleAtHistoricalResultFromOpenDevice(f.devices.front(),b.root,Id(1),b.zero.page_uuid,old_primary,b.Budget()));
 const auto intact=f.Read(0);
 for(unsigned variant=0;variant<3;++variant){auto torn=intact;
   if(variant==0)std::fill(torn.begin()+4096,torn.begin()+4480,0);
   if(variant==1)torn[4300]^=1;
   if(variant==2)std::copy_n(old_primary.begin()+4096,97,torn.begin()+4096);
   Check(f.device.WriteAt(0,torn.data(),torn.size()).ok()&&f.device.Sync().ok(),"actual torn mutable body fixture");
   good(historical());good(historical_result());Check(f.Read(0)==torn,"historical bundle reads never repair damaged metadata");
 }
 Check(f.device.WriteAt(0,intact.data(),intact.size()).ok()&&f.device.Sync().ok(),"restore current metadata");
 for(unsigned at:{0u,4480u,static_cast<unsigned>(f.size-1)}){auto wrong=intact;wrong[at]^=1;
   Check(f.device.WriteAt(0,wrong.data(),wrong.size()).ok(),"immutable damage fixture");Empty(historical());Empty(historical_result());}
 Check(f.device.WriteAt(0,intact.data(),intact.size()).ok()&&f.device.Sync().ok(),"restore immutable metadata");
 Check(f.device.Close().ok()&&f.device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"growth bundle read-only reopen");good(b.Read());good(historical());good(historical_result());
 Check(f.device.Close().ok(),"growth bundle release before process");const auto child=fork();Check(child>=0,"growth bundle independent process");
 if(!child){d::FileDevice device;if(!device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok())_exit(80);
   const auto r=db::ReadNativeManagementControlBundleFromOpenDevice({Id(2),b.zero.bootstrap.page_size_profile_uuid,&device},b.root,Id(1),b.zero.page_uuid,b.Budget());
   const auto h=db::ReadNativeManagementControlBundleAtHistoricalPageZeroFromOpenDevice({Id(2),b.zero.bootstrap.page_size_profile_uuid,&device},b.root,Id(1),b.zero.page_uuid,old_primary,b.Budget());
   const auto a=db::ReadNativeManagementControlBundleAtHistoricalResultFromOpenDevice({Id(2),b.zero.bootstrap.page_size_profile_uuid,&device},b.root,Id(1),b.zero.page_uuid,growing?old_primary:b.growth_images[1],b.Budget());
   _exit(r.ok()&&h.ok()&&a.ok()&&a.growth_images==b.growth_images&&h.growth_images==b.growth_images&&h.directory_images==b.directory_images&&r.growth_images==b.growth_images&&r.directory_images==b.directory_images&&r.allocation_images==b.map_images?0:81);}
 int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"growth images retained across independent reopen");
 Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"growth fixture reopen");
 const auto expected_size=(growing?256:260)*f.size;
 {const auto decoded=d::DecodeFilespacePageZero(intact.data(),intact.size());Check(decoded.ok(),"canonical current result");auto newer=*decoded.record;
  ++newer.page_generation;++newer.root_set_generation;newer.total_pages+=8;newer.free_pages+=8;
  const auto image=d::EncodeFilespacePageZero(newer);Check(image.ok(),"canonical later capacity image");
  std::filesystem::resize_file(f.path,newer.total_pages*f.size);Check(f.device.WriteAt(0,image.bytes->data(),image.bytes->size()).ok()&&f.device.Sync().ok(),"actual later capacity fixture");
  Empty(b.Read());good(historical_result());good(historical());
  Check(f.device.WriteAt(0,intact.data(),intact.size()).ok(),"restore prior exact result");std::filesystem::resize_file(f.path,expected_size);Check(f.device.Sync().ok(),"restored result barrier");}
 growth_extend_to=expected_size+1;const auto changed=historical();Check(!growth_extend_to&&changed.error==BE::physical_extent_changed,"actual mid-read physical extent change refuses");Empty(changed);
 growth_extend_to=expected_size+2;const auto result_changed=historical_result();Check(!growth_extend_to&&result_changed.error==BE::physical_extent_changed,"historical result size race refuses");Empty(result_changed);
 good(historical()); // unchanged partial tail is observed, never granted as free capacity
 std::filesystem::resize_file(f.path,256*f.size-1);Empty(historical());Empty(historical_result());
 std::filesystem::resize_file(f.path,expected_size);good(historical());good(historical_result());
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
 historical_stats=0;historical_stat_counting=true;good(historical());historical_stat_counting=false;Check(historical_stats==2,"two actual historical extent observations");
 for(unsigned site=1;site<=2;++site){historical_stats=0;historical_stat_fault=site;historical_stat_counting=true;const auto r=historical();historical_stat_counting=false;historical_stat_fault=0;
   Check(r.error==BE::io_failure&&historical_stats>=site,"historical extent failure retained");Empty(r);}
 for(unsigned site=1;site<=2;++site){historical_stats=0;historical_stat_fault=site;historical_stat_counting=true;const auto r=historical_result();historical_stat_counting=false;historical_stat_fault=0;
   Check(r.error==BE::io_failure&&historical_stats>=site,"historical result extent failure retained");Empty(r);}
#endif
 if(!reverse&&primary==secondary)for(bool result_context:{false,true}){historical_read_pause=1;auto reader=std::async(std::launch::async,[&]{return result_context?historical_result():historical();});
   struct Release {~Release(){historical_read_pause=3;}} release;
   const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
   while(historical_read_pause!=2&&std::chrono::steady_clock::now()<limit)std::this_thread::yield();
   Check(historical_read_pause==2,"historical reader paused with device ownership");std::atomic<bool> close_entered=false;auto closer=std::async(std::launch::async,[&]{close_entered=true;return f.device.Close();});
   while(!close_entered&&std::chrono::steady_clock::now()<limit)std::this_thread::yield();
   const bool blocked=closer.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout;historical_read_pause=3;
   const auto result=reader.get();const auto closed=closer.get();Check(close_entered&&blocked&&closed.ok(),"concurrent Close waits for complete historical verification");good(result);
   Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"historical concurrency reopen");historical_read_pause=0;
 }
 const auto saved_images=b.growth_images,saved_pages=b.pages;const auto saved_root=b.root;
 if(!growing){auto wrong=*d::DecodeFilespacePageZero(old_primary.data(),old_primary.size()).record;wrong.writer_identity_uuid=Id(18000);
   const auto image=d::EncodeFilespacePageZero(wrong);Check(image.ok(),"canonical substituted before image");
   Empty(db::ReadNativeManagementControlBundleAtHistoricalPageZeroFromOpenDevice(f.devices.front(),b.root,Id(1),b.zero.page_uuid,*image.bytes,b.Budget()));}
 for(unsigned field=0;field<9;++field){auto after=*d::DecodeFilespacePageZero(saved_images[1].data(),saved_images[1].size()).record;
   if(field==0)++after.page_generation;if(field==1)++after.root_set_generation;if(field==2)--after.free_pages;
   if(field==3)after.writer_identity_uuid=Id(14001);if(field==4)after.roots.front().object_uuid=Id(14002);
   if(field==5)after.total_pages--;if(field==6)after.preallocated_pages++;if(field==7)after.page_uuid=Id(14003);
   if(field==8)++after.creation_utc_millis;
   const auto image=d::EncodeFilespacePageZero(after);Check(image.ok(),"valid unrelated growth image");b.growth_images[1]=*image.bytes;b.Oracle();Empty(b.Encode());Empty(b.Decode());b.growth_images=saved_images;
 }
 b.Oracle();std::swap(b.growth_images[0],b.growth_images[1]);b.Oracle();Empty(b.Encode());Empty(b.Decode());b.growth_images=saved_images;b.Oracle();
 for(unsigned field:{135u,136u,360u,368u,383u}){b.pages=saved_pages;b.root=saved_root;b.pages.front()[field]^=1;b.Reseal();Empty(b.Decode());}
 b.pages=saved_pages;b.root=saved_root;
 for(u64 count:{u64{0},u64{1},u64{3},std::numeric_limits<u64>::max()}){b.root.growth_image_count=count;Empty(b.Decode());}b.root=saved_root;
 u64 growth_offset=0;for(const auto* group:{&b.map_images,&b.inventory_images,&b.directory_images})for(const auto& raw:*group)growth_offset+=8+raw.size();
 for(u64 length:{u64{0},u64{127},u64{4096},u64{8191},std::numeric_limits<u64>::max()}){
   b.pages=saved_pages;for(unsigned i=0;i<8;++i){const auto at=growth_offset+i;b.pages[at/(f.size-384)][384+at%(f.size-384)]=static_cast<byte>(length>>(8*i));}
   b.ResealPayload();Empty(b.Decode());}b.pages=saved_pages;b.root=saved_root;
 b.growth_images.pop_back();Empty(b.Encode());b.growth_images=saved_images;b.growth_images.push_back(saved_images[1]);Empty(b.Encode());b.growth_images=saved_images;
 if(!growing){auto wrong=*d::DecodeFilespacePageZero(saved_images[1].data(),saved_images[1].size()).record;wrong.writer_identity_uuid=Id(15000);
   const auto image=d::EncodeFilespacePageZero(wrong);Check(image.ok()&&f.device.WriteAt(0,image.bytes->data(),image.bytes->size()).ok()&&f.device.Sync().ok(),"different valid actual metadata");
   Empty(b.Read());Check(f.device.WriteAt(0,saved_images[1].data(),saved_images[1].size()).ok()&&f.device.Sync().ok(),"restore exact after metadata");good(b.Read());}
 // Sweep all required allocations and every provider failure at each hash site.
 // Largest mixed framing is included in addition to each equal-profile case.
 if(reverse||!(primary==secondary||(primary==0&&secondary==4)))return;
 byte scratch=0;for(unsigned i=0;i<4097;++i)Check(f.device.ReadAt(0,&scratch,1).ok(),"growth telemetry warmup");
 const auto original=f.Read(0,growing?256:260);
 for(unsigned route=0;route<5;++route){const auto call=[&]{if(!route){const auto r=b.Encode();if(!r.ok())Empty(r);return r.error;}
     const auto r=route==1?b.Decode():route==2?b.Read():route==3?historical():historical_result();if(!r.ok())Empty(r);return r.error;};
   counting=true;allocations=0;Check(call()==BE::none,"growth allocation baseline");counting=false;const auto sites=allocations;
   for(unsigned long at=0;at<=sites;++at){const auto loss=f.device.failed_io_latency_observations();allocation_budget=at;const auto r=call();const auto left=allocation_budget;allocation_budget=-1;
     Check(at==sites?(r==BE::none&&left>=0):(left<0&&(r==BE::resource_exhausted||(r==BE::none&&f.device.failed_io_latency_observations()==loss+1))),"growth allocation failure retained or measured telemetry loss");}
   hash_counting=true;hash_seen=0;Check(call()==BE::none,"growth hash baseline");hash_counting=false;const auto hashes=hash_seen;
   for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto r=call();Check(!hash_fault&&r==BE::hash_failure,"every growth provider failure consumed");}
 }
 reads=writes=syncs=0;io_counting=true;good(b.Read());io_counting=false;const auto sites=reads;
 for(unsigned at=1;at<=sites;++at){reads=0;read_fault=at;io_counting=true;const auto r=b.Read();io_counting=false;read_fault=0;Check(r.error==BE::io_failure&&reads>=at,"growth read error consumed");Empty(r);}
 Check(!writes&&!syncs&&f.Read(0,growing?256:260)==original,"growth reconstruction failures never mutate storage");
 reads=writes=syncs=0;io_counting=true;good(historical());io_counting=false;const auto old_sites=reads;
 for(unsigned at=1;at<=old_sites;++at){reads=0;read_fault=at;io_counting=true;const auto r=historical();io_counting=false;read_fault=0;
   Check(r.error==BE::io_failure&&reads>=at,"historical bundle read error consumed");Empty(r);}
 Check(!writes&&!syncs&&f.Read(0,growing?256:260)==original,"historical bundle failures preserve all physical bytes");
 reads=writes=syncs=0;io_counting=true;good(historical_result());io_counting=false;const auto result_sites=reads;
 for(unsigned at=1;at<=result_sites;++at){reads=0;read_fault=at;io_counting=true;const auto r=historical_result();io_counting=false;read_fault=0;
   Check(r.error==BE::io_failure&&reads>=at,"historical result read error consumed");Empty(r);}
 Check(!writes&&!syncs&&f.Read(0,growing?256:260)==original,"historical result failures preserve all physical bytes");
}
// Complete immutable transitions, not fabricated runtime admission. Primary
// genesis is read from a real file; all candidate images have independent oracles.
struct DirectoryTransition {
 Fixture fixture;Graph graph;Uuid other,changed;unsigned profile;bool reserve;
 std::map<Uuid,Maps> before,after;std::map<Uuid,d::FilespacePageZero> zeros;
 db::NativeManagementDirectoryBase base;page::NativeFilespaceDirectory old_directory,directory;
 db::NativeStorageActionIntent request;records::O operation;
 Pages before_images,after_images,inventory_before,inventory_after,growth,bundle,extent;
 std::vector<d::NativeCommonPageHeader> bundle_headers;Bytes base_cp,target_cp,plan_image;
 unsigned identity=30000;
 Uuid extent_page_uuid=Id(29910);u64 extent_slot=102;
 DirectoryTransition(unsigned primary,unsigned secondary,bool reverse,unsigned recovery,bool secondary_target,bool preallocated,bool inventory_on_primary=false)
   :fixture(primary,512),graph(fixture),other(reverse?Id(0):Id(7)),changed(secondary_target?other:Id(2)),profile(recovery),reserve(preallocated){
  auto& p=graph.plan;const auto& q=d::kCanonicalFilespacePageProfiles[secondary];
  zeros.emplace(Id(2),graph.zero);auto z=graph.zero;z.bootstrap.filespace_uuid=other;z.bootstrap.filespace_role=5;
  z.bootstrap.page_size_profile_uuid=q.uuid;z.bootstrap.page_size_bytes=q.page_size_bytes;z.page_uuid=Id(29000);
  z.page_generation=7;z.root_set_generation=9;z.free_pages=510;z.preallocated_pages=0;
  z.roots={{3,3,other,1,1,q.uuid,Id(29001)}};zeros.emplace(other,z);
  before.emplace(Id(2),graph.before);page::NativeAllocationMap m;
  m.header={q.page_size_bytes,3,Id(1),other,Id(29002),1,1,0,q.uuid};m.object_uuid=Id(29001);
  m.map_generation=m.capacity_generation=1;m.total_pages=512;m.states.resize(512,State::free);m.creator_transaction_uuid=Id(5);m.creator_local_transaction_id=1;
  before.emplace(other,Maps{m});
  Add(before, {q.page_size_bytes,2,Id(1),other,z.page_uuid,0,7,0,q.uuid},other,{});
  Add(before,m.header,m.object_uuid,{});
  const auto root=std::find_if(graph.base.roots.begin(),graph.base.roots.end(),[](const auto& r){return r.role==3;});
  Check(root!=graph.base.roots.end(),"genesis directory root");const auto raw=fixture.Read(root->page.page_number);
  const auto decoded=page::DecodeNativeFilespaceDirectory(raw);Check(decoded.ok()&&!decoded.directory->next,"genesis directory image");old_directory=*decoded.directory;
  page::NativeFilespaceDirectoryRecord member;member.bootstrap=z.bootstrap;member.locator_uuid=Id(29003);member.page_zero_uuid=z.page_uuid;
  member.page_zero_generation=z.page_generation;member.root_set_generation=z.root_set_generation;member.total_pages=z.total_pages;
  old_directory.records.push_back(member);
  // A directory member with no changed allocation chain must remain byte-exact.
  auto unchanged=member;unchanged.bootstrap.filespace_uuid=Id(11);unchanged.page_zero_uuid=Id(29009);unchanged.locator_uuid=Id(29008);
  old_directory.records.push_back(unchanged);old_directory.total_records=3;
  std::sort(old_directory.records.begin(),old_directory.records.end(),[](const auto& a,const auto& b){return a.bootstrap.filespace_uuid<b.bootstrap.filespace_uuid;});
  RefreshBase();directory=old_directory;directory.header.page_uuid=Id(identity++);directory.header.page_number=220;
  directory.header.page_generation=directory.directory_generation=p.reserved_generation;
  directory.creator_transaction_uuid={};directory.creator_local_transaction_id=0;directory.creator_operation_uuid=p.operation_uuid;
  p.base_selection_generation=1;p.intent.recovery_profile=profile;
  for(const auto& [fs,maps]:before){const auto& source=maps.front();Maps result;const u64 width=fs==Id(2)?32:64;
    const u64 total=512+(profile==4&&fs==changed?4:0);
    for(u64 n=0;n<512;n+=width){auto candidate=source;candidate.header.page_uuid=Id(identity++);candidate.header.page_number=200+n/width;
      candidate.header.page_generation=candidate.map_generation=p.reserved_generation;candidate.creator_transaction_uuid={};candidate.creator_local_transaction_id=0;candidate.creator_operation_uuid=p.operation_uuid;
      candidate.total_pages=total;candidate.capacity_generation+=(profile==4&&fs==changed);candidate.first_page=n;
      const u64 end=n+width==512?total:n+width;candidate.states.assign(end-n,State::free);candidate.records.clear();candidate.next.reset();candidate.next_sha256={};
      for(u64 i=n;i<std::min<u64>(end,512);++i){const auto& old=Cover(before.at(fs),i);candidate.states[i-n]=old.states[i-old.first_page];
        for(const auto& r:old.records)if(r.page_number==i)candidate.records.push_back(r);}
      result.push_back(std::move(candidate));}
    after.emplace(fs,std::move(result));}
  if(profile==2){const auto root=std::find_if(graph.base.roots.begin(),graph.base.roots.end(),[](const auto& r){return r.role==1;});
    const auto raw=fixture.Read(root->page.page_number);inventory_before.push_back(raw);const auto decoded=page::DecodeNativeTransactionInventoryPage(raw);
    Check(decoded.ok()&&!decoded.page->next,"actual inventory base");auto inv=*decoded.page;
    const auto& ip=inventory_on_primary?d::kCanonicalFilespacePageProfiles[primary]:q;
    inv.header={ip.page_size_bytes,0x301,Id(1),inventory_on_primary?Id(2):other,Id(identity++),222,p.reserved_generation,0,ip.uuid};inv.inventory_generation=p.reserved_generation;
    mga::TransactionInventoryEntry e;e.identity.transaction_uuid={UuidKind::transaction,Id(identity++)};e.identity.local_id=mga::MakeLocalTransactionId(inv.inventory.next_local_transaction_id++);
    e.identity.scope=mga::TransactionScope::local_node;e.state=mga::TransactionState::created;e.begin_unix_epoch_millis=2000000000000ULL;
    e.begin_visible_through_local_transaction_id=e.identity.local_id.value-1;e.begin_visible_through_commit_sequence=inv.inventory.next_commit_sequence-1;inv.inventory.entries.push_back(e);
    const auto image=InventoryOracle(inv);Check(page::EncodeNativeTransactionInventoryPage(inv).bytes==image,"independent candidate inventory");inventory_after.push_back(image);
    Add(after,inv.header,inv.object_uuid,p.operation_uuid);auto& target=*std::find_if(graph.target.roots.begin(),graph.target.roots.end(),[](const auto& r){return r.role==1;});
    target={1,0x301,Self(inv.header),inv.object_uuid,Sha(image)};graph.target.selected_local_transaction_id=inv.inventory.next_local_transaction_id-1;
    operation=records::Example(1);operation.uuid=Id(29900);operation.bootstrap_uuid=graph.zero.page_uuid;operation.security_snapshot_uuid={};operation.generation_guards={};
  }else{
    const auto& z=zeros.at(changed);const auto& m=before.at(changed).front();const auto& member=*std::find_if(old_directory.records.begin(),old_directory.records.end(),[&](const auto& r){return r.bootstrap.filespace_uuid==changed;});
    request.request_uuid=Id(29901);request.operation_uuid=Id(29900);request.database_uuid=Id(1);request.filespace_uuid=changed;request.locator_uuid=member.locator_uuid;
    request.page_zero_uuid=z.page_uuid;request.page_size_profile_uuid=z.bootstrap.page_size_profile_uuid;request.page_size_bytes=z.bootstrap.page_size_bytes;
    request.policy_snapshot_uuid=Id(10);request.storage_profile_uuid=Id(29902);request.initiator_uuid=Id(8);request.request_context_uuid=Id(9);
    request.policy_uuid=Id(29903);request.policy_version_uuid=Id(29904);request.attachment_uuid=Id(29905);request.attachment_version_uuid=Id(29906);request.storage_profile_version_uuid=Id(29907);
    request.attachment_generation=request.storage_profile_generation=1;
    request.checkpoint={9,0x300,Id(2),graph.base.header.page_number,graph.base.header.page_generation,graph.zero.bootstrap.page_size_profile_uuid,graph.base.object_uuid};
    request.allocation_root={3,3,changed,m.header.page_number,m.header.page_generation,m.header.page_size_profile_uuid,m.object_uuid};
    request.checkpoint_sha256=Sha(base_cp);request.allocation_sha256=Sha(EncodeMaps(before.at(changed)).front());
    request.checkpoint_generation=graph.base.checkpoint_generation;request.checkpoint_root_set_generation=graph.base.root_set_generation;
    request.directory_generation=old_directory.directory_generation;request.filespace_root_set_generation=z.root_set_generation;request.page_zero_generation=z.page_generation;
    request.map_generation=m.map_generation;request.capacity_generation=m.capacity_generation;request.current_total_pages=512;
    request.first_page=profile==4?512:256;request.page_count=4;request.maximum_total_pages=profile==4?516:512;
    request.maximum_work_bytes=4*u64{z.bootstrap.page_size_bytes};request.maximum_retained_image_bytes=fixture.budget;
    request.action=profile==4?db::NativeStorageAction::physical_growth:db::NativeStorageAction::page_preallocation;
    request.intended_state=reserve?db::NativeStorageIntentState::preallocated:db::NativeStorageIntentState::free;
    if(reserve){request.allocation_owner_uuid=Id(29908);request.allocation_page_type=0x100;
      for(u64 n=request.first_page;n<request.first_page+4;++n){auto& m=Cover(after.at(changed),n);m.states[n-m.first_page]=State::preallocated;
        page::NativeAllocationRecord r;r.page_number=n;r.allocation_uuid=Id(identity++);r.owner_uuid=request.allocation_owner_uuid;r.page_type=request.allocation_page_type;r.creator_operation_uuid=p.operation_uuid;m.records.push_back(r);}}
    if(profile==4)++Find(after.at(changed),0).page_generation;
    operation=records::Example(1);operation.uuid=request.operation_uuid;operation.bootstrap_uuid=graph.zero.page_uuid;operation.target_uuid=changed;
    operation.security_snapshot_uuid={};operation.generation_guards={};
  }
  SetExtent();auto h=graph.target.header;h.page_number=p.target_checkpoint.page_number;h.page_generation=p.reserved_generation;
  Add(after,h,graph.target.object_uuid,p.operation_uuid);Add(after,p.header,p.object_uuid,p.operation_uuid);
  for(const auto& raw:extent){const auto h=d::DecodeNativeCommonPageHeader(raw.data(),128);Check(h.ok(),"extent header");Add(after,*h.header,p.management_extent->object_uuid,p.operation_uuid);}
  Add(after,directory.header,directory.object_uuid,p.operation_uuid);
  for(const auto& [fs,maps]:after)for(const auto& map:maps)Add(after,map.header,map.object_uuid,p.operation_uuid);
  u64 payload=fixture.size+8;for(const auto& [fs,maps]:after)for(const auto& m:maps)payload+=m.header.page_size_bytes+8;
  for(const auto& image:inventory_after)payload+=image.size()+8;if(profile==4)payload+=2*(zeros.at(changed).bootstrap.page_size_bytes+8);
  const u64 count=(payload+fixture.size-385)/(fixture.size-384);Check(300+count<=512,"bundle fits original capacity");
  for(u64 n=0;n<count;++n){d::NativeCommonPageHeader h{u32(fixture.size),0x500,Id(1),Id(2),Id(identity++),300+n,p.reserved_generation,0,graph.zero.bootstrap.page_size_profile_uuid};
    bundle_headers.push_back(h);Add(after,h,Id(29909),p.operation_uuid);}
  Refresh();
 }
 void Add(std::map<Uuid,Maps>& groups,const d::NativeCommonPageHeader& h,const Uuid& owner,const Uuid& creator){
  auto& m=Cover(groups.at(h.filespace_uuid),h.page_number);Check(m.states[h.page_number-m.first_page]==State::free,"transition slot free");m.states[h.page_number-m.first_page]=State::allocated;
  page::NativeAllocationRecord r;r.page_number=h.page_number;r.allocation_uuid=Id(identity++);r.page_uuid=h.page_uuid;r.owner_uuid=owner;
  r.page_generation=h.page_generation;r.page_type=h.page_type;r.creator_operation_uuid=creator;
  if(creator.is_nil()){r.creator_transaction_uuid=Id(5);r.creator_local_transaction_id=1;}m.records.push_back(r);
 }
 Pages MapsImages(std::map<Uuid,Maps>& groups){Pages images;for(auto& [fs,maps]:groups){for(auto& m:maps)std::sort(m.records.begin(),m.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});
    const auto encoded=EncodeMaps(maps);images.insert(images.end(),encoded.begin(),encoded.end());}return images;}
 void BindMembers(page::NativeFilespaceDirectory& dir,std::map<Uuid,Maps>& groups){for(auto& r:dir.records){if(!groups.contains(r.bootstrap.filespace_uuid))continue;const auto& m=groups.at(r.bootstrap.filespace_uuid).front();
    r.allocation_root=page::NativeFilespaceAllocationRoot{Self(m.header),m.object_uuid,Sha(MapOracle(m)),m.map_generation,m.capacity_generation};}}
 void RefreshBase(){before_images=MapsImages(before);BindMembers(old_directory,before);const auto raw=DirectoryImageOracle(old_directory);
  Check(page::EncodeNativeFilespaceDirectory(old_directory).bytes==raw,"independent base directory");base.directory_images={raw};base.page_zero_images.clear();
  for(const auto& [fs,z]:zeros){const auto encoded=d::EncodeFilespacePageZero(z);Check(encoded.ok(),"canonical original page zero");base.page_zero_images.push_back(*encoded.bytes);}
  auto& dir=*std::find_if(graph.base.roots.begin(),graph.base.roots.end(),[](const auto& r){return r.role==3;});dir.sha256=Sha(raw);
  base_cp=db::EncodeNativeCheckpointRoot(graph.base).bytes;Check(!base_cp.empty(),"transition base checkpoint");graph.plan.base_checkpoint_sha256=Sha(base_cp);
 }
 void SetExtent(){if(profile!=2){const auto encoded=db::EncodeNativeStorageActionIntent(request,fixture.budget);Check(encoded.ok(),"directory exact storage intent");operation.normalized_request_bytes=encoded.bytes;operation.normalized_request_sha256=Sha(encoded.bytes);}
  auto& p=graph.plan;p.intent={operation.initiator_uuid,operation.request_context_uuid,operation.policy_snapshot_uuid,operation.normalized_request_sha256,operation.initiator_kind,u16(profile)};
  std::vector<d::NativeCommonPageHeader> headers{{u32(fixture.size),0x500,Id(1),Id(2),extent_page_uuid,extent_slot,p.reserved_generation,0,graph.zero.bootstrap.page_size_profile_uuid}};
  const auto encoded=db::EncodeNativeManagementExtent(operation,Id(5000),headers,fixture.budget);Check(encoded.ok(),"directory bound management extent");extent=encoded.pages;p.management_extent=encoded.root;
 }
 void Refresh(){after_images=MapsImages(after);BindMembers(directory,after);growth.clear();
  if(profile==4){const auto& z=zeros.at(changed);auto next=z;++next.page_generation;++next.root_set_generation;next.total_pages=z.total_pages+4;next.free_pages=next.preallocated_pages=0;
    for(const auto& m:after.at(changed)){next.free_pages+=std::count(m.states.begin(),m.states.end(),State::free);next.preallocated_pages+=std::count(m.states.begin(),m.states.end(),State::preallocated);}
    const auto first=d::EncodeFilespacePageZero(z),last=d::EncodeFilespacePageZero(next);Check(first.ok()&&last.ok(),"exact growth pair");growth={*first.bytes,*last.bytes};
    for(auto& r:directory.records)if(r.bootstrap.filespace_uuid==changed){r.page_zero_generation=next.page_generation;r.root_set_generation=next.root_set_generation;r.total_pages=next.total_pages;}}
  const auto dir=DirectoryImageOracle(directory);Check(page::EncodeNativeFilespaceDirectory(directory).bytes==dir,"independent complete candidate directory");
  auto& p=graph.plan;const auto encoded=db::EncodeNativeManagementControlBundle(after_images,Id(1),graph.zero.page_uuid,Id(29909),p.operation_uuid,bundle_headers,fixture.budget,inventory_after,{dir},growth);
  Check(encoded.ok(),"structurally canonical complete transition bundle");bundle=encoded.pages;p.control_bundle=encoded.root;
  auto& d=*std::find_if(graph.target.roots.begin(),graph.target.roots.end(),[](const auto& r){return r.role==3;});d={3,9,Self(directory.header),directory.object_uuid,Sha(dir)};
  const auto& map=after.at(Id(2)).front();auto& m=*std::find_if(graph.target.roots.begin(),graph.target.roots.end(),[](const auto& r){return r.role==4;});m={4,3,Self(map.header),map.object_uuid,Sha(MapOracle(map))};
  target_cp=Finish(p,graph.target);plan_image=Oracle(p);
 }
 u64 Budget()const{u64 total=base_cp.size()+target_cp.size()+plan_image.size();for(const auto* group:{&extent,&before_images,&after_images,&bundle,&base.directory_images,&base.page_zero_images})for(const auto& raw:*group)total+=raw.size();
  for(const auto* group:{&inventory_before,&inventory_after})for(const auto& raw:*group)total+=4*raw.size();return total;}
 CE Validate(u64 budget=0)const{
  const bool bounded=!counting&&allocation_budget<0&&!hash_counting&&!hash_fault;const auto limit=budget?budget:Budget();
  const auto result=db::ValidateNativeManagementDirectoryControlAllocation(base_cp,target_cp,plan_image,extent,before_images,after_images,base,limit,bundle,inventory_before);
  if(bounded)BoundedControlCheck(base_cp,target_cp,plan_image,extent,before_images,after_images,limit,bundle,inventory_before,&base,result);
  return result;}
};
#include "native_management_history_memory_checks.hpp"
#include "native_management_control_authority_memory_checks.hpp"
#include "native_checkpoint_inventory_memory_checks.hpp"
#define SB_BOUND_SELECTION_HASH_PROBE 1
#include "native_bound_checkpoint_selection_memory_checks.hpp"
#define SB_CURRENT_SOURCE_HASH_PROBE 1
#include "native_current_checkpoint_source_memory_checks.hpp"
#define SB_SELECTED_LEASE_HASH_PROBE 1
#include "native_selected_checkpoint_memory_lease_checks.hpp"
#include "native_owned_checkpoint_memory_checks.hpp"

struct DirectoryHistoryFixture {
 DirectoryTransition t;d::FileDevice untouched;std::filesystem::path secondary_path,untouched_path;u64 budget;
 DirectoryHistoryFixture(unsigned primary,unsigned secondary,bool reverse,unsigned profile,bool target,bool reserve,bool inventory_on_primary=false,bool install=true)
   :t(primary,secondary,reverse,profile,target,reserve,inventory_on_primary),budget(8*t.fixture.budget){
  auto& f=t.fixture;secondary_path=f.path.parent_path()/"secondary";untouched_path=f.path.parent_path()/"untouched";
  Check(f.secondary.Open(secondary_path.string(),d::FileOpenMode::create_new).ok()&&untouched.Open(untouched_path.string(),d::FileOpenMode::create_new).ok(),"owned actual historical members");
  auto z=t.zeros.at(t.other);auto extra=z;extra.bootstrap.filespace_uuid=Id(11);extra.page_uuid=Id(29009);extra.roots.front().filespace_uuid=Id(11);extra.roots.front().object_uuid=Id(29010);
  const byte end=0;for(const auto& entry:{std::pair{&f.secondary,z},std::pair{&untouched,extra}}){const auto image=d::EncodeFilespacePageZero(entry.second);
    Check(image.ok()&&entry.first->WriteAt(512*u64{entry.second.bootstrap.page_size_bytes}-1,&end,1).ok()&&entry.first->WriteAt(0,image.bytes->data(),image.bytes->size()).ok()&&entry.first->Sync().ok(),"canonical actual member files");}
  f.devices.push_back({t.other,z.bootstrap.page_size_profile_uuid,&f.secondary});f.devices.push_back({Id(11),z.bootstrap.page_size_profile_uuid,&untouched});
  auto extra_map=t.before.at(t.other).front();extra_map.header.filespace_uuid=Id(11);extra_map.header.page_uuid=Id(29011);extra_map.object_uuid=Id(29010);
  for(auto& record:extra_map.records){record.allocation_uuid=Id(29012+record.page_number);record.page_uuid=record.page_number?extra_map.header.page_uuid:extra.page_uuid;record.owner_uuid=record.page_number?extra_map.object_uuid:Id(11);}
  Write(MapOracle(extra_map));
  for(const auto& raw:t.before_images)Write(raw);Write(t.base.directory_images.front());Write(t.base_cp);
  if(!install){const auto hash=Sha(t.base_cp);for(const auto& root:t.graph.zero.roots){
      if(root.kind==18||root.kind==19){const auto decoded=db::DecodeNativeCheckpointSelection(f.Read(root.page_number));Check(decoded.ok(),"original base selector");auto s=*decoded.selection;s.checkpoint_sha256=hash;Write(SelectorOracle(s));}
      if(root.kind==20||root.kind==21){auto raw=f.Read(root.page_number);const auto decoded=db::DecodeNativePublicationWatermark(raw);Check(decoded.ok()&&!decoded.state->intent,"genesis base watermark");
        std::copy(hash.begin(),hash.end(),raw.begin()+304);Bytes state(raw.begin()+128,raw.begin()+368);const auto digest=Sha(state);std::copy(digest.begin(),digest.end(),raw.begin()+368);
        std::fill(raw.begin()+400,raw.begin()+432,0);const auto full=Sha(raw);std::copy(full.begin(),full.end(),raw.begin()+400);
        auto w=*decoded.state;w.base_checkpoint_sha256=hash;Check(db::EncodeNativePublicationWatermark(w).bytes==raw,"independent complete initial watermark image");Write(raw);}}
    for(const auto& file:f.devices)Check(file.device->Sync().ok(),"complete original fixture barrier");return;}
  for(const auto* group:{&t.after_images,&t.inventory_after,&t.extent,&t.bundle})for(const auto& raw:*group)Write(raw);
  Write(DirectoryImageOracle(t.directory));Write(t.plan_image);Write(t.target_cp);
  if(!t.growth.empty()){const auto decoded=d::DecodeFilespacePageZero(t.growth.back().data(),t.growth.back().size());Check(decoded.ok(),"growth result for actual history");
    const auto& z=*decoded.record;auto* device=File(z.bootstrap.filespace_uuid);Check(device->WriteAt(z.total_pages*u64{z.bootstrap.page_size_bytes}-1,&end,1).ok()&&device->WriteAt(0,t.growth.back().data(),t.growth.back().size()).ok(),"actual historical growth result");}
  Select();for(const auto& file:f.devices)Check(file.device->Sync().ok(),"actual fixture barrier");
 }
 void Select(){auto& f=t.fixture;for(unsigned n=0;n<2;++n){const auto root=std::find_if(t.graph.zero.roots.begin(),t.graph.zero.roots.end(),[&](const auto& r){return r.kind==18+n;});
    auto decoded=db::DecodeNativeCheckpointSelection(f.Read(root->page_number));Check(decoded.ok(),"actual original selector");auto s=*decoded.selection;
    s.previous_selection_generation=*t.graph.plan.base_selection_generation;s.previous_checkpoint=t.graph.plan.base_checkpoint;s.previous_checkpoint_object_uuid=t.graph.plan.base_checkpoint_object_uuid;s.previous_checkpoint_sha256=t.graph.plan.base_checkpoint_sha256;
    s.selection_generation=s.previous_selection_generation+1;s.publication_uuid=t.graph.plan.operation_uuid;s.checkpoint=t.graph.plan.target_checkpoint;s.checkpoint_object_uuid=t.graph.plan.target_checkpoint_object_uuid;
    s.checkpoint_sha256=Sha(t.target_cp);s.checkpoint_generation=t.graph.plan.reserved_generation;s.root_set_generation=t.graph.plan.target_root_set_generation;Write(SelectorOracle(s));}
 }
 d::FileDevice* File(const Uuid& id){for(const auto& f:t.fixture.devices)if(f.filespace_uuid==id)return f.device;throw std::runtime_error("missing fixture member");}
 void Write(const Bytes& raw){const auto h=d::DecodeNativeCommonPageHeader(raw.data(),128);Check(h.ok(),"actual historical image header");const auto& v=*h.header;
  const auto io=File(v.filespace_uuid)->WriteAt(v.page_number*u64{v.page_size_bytes},raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size(),"actual historical image fixture write");}
 auto Read()const{return db::ReadNativeManagementHistoryFromOpenDevices(Id(1),t.fixture.devices,Id(2),budget);}
 std::map<Uuid,Bytes> Context()const{std::map<Uuid,Bytes> result;for(const auto& file:t.fixture.devices){const auto* p=d::FindCanonicalFilespacePageProfile(file.page_size_profile_uuid);
   Bytes raw(p->page_size_bytes);const auto io=file.device->ReadAt(0,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size()&&d::DecodeFilespacePageZero(raw.data(),raw.size()).ok(),"exact canonical result context from actual files");result.emplace(file.filespace_uuid,std::move(raw));}return result;}
 void NextGrowth(){Check(t.profile==3||t.profile==4,"storage-request predecessor");auto& p=t.graph.plan;const auto old=p;const auto old_plan=t.plan_image;
  if(!t.growth.empty()){const auto& raw=t.growth.back();const auto decoded=d::DecodeFilespacePageZero(raw.data(),raw.size());Check(decoded.ok(),"previous growth result");t.zeros.at(t.changed)=*decoded.record;}
  t.before=t.after;t.old_directory=t.directory;t.graph.base=*db::DecodeNativeCheckpointRoot(t.target_cp).root;
  t.graph.target=t.graph.base;t.graph.target.header.page_uuid=Id(t.identity++);t.RefreshBase();t.profile=4;
  ++p.reserved_generation;++p.target_root_set_generation;++*p.base_selection_generation;
  p.previous_plan=Self(old.header);p.previous_plan_object_uuid=old.object_uuid;p.previous_plan_sha256=Sha(old_plan);
  p.header.page_uuid=Id(t.identity++);p.header.page_number=120;p.header.page_generation=p.reserved_generation;p.operation_uuid=Id(t.identity++);
  p.base_checkpoint=Self(t.graph.base.header);p.base_checkpoint_generation=t.graph.base.checkpoint_generation;p.base_root_set_generation=t.graph.base.root_set_generation;
  p.target_checkpoint={Id(2),121,p.reserved_generation,p.header.page_size_profile_uuid};p.reservation_state_sha256.fill(37);
  t.directory=t.old_directory;t.directory.header.page_uuid=Id(t.identity++);t.directory.header.page_number=240;
  t.directory.header.page_generation=t.directory.directory_generation=p.reserved_generation;t.directory.creator_operation_uuid=p.operation_uuid;
  t.after=t.before;for(auto& [fs,maps]:t.after){for(std::size_t n=0;n<maps.size();++n){auto& m=maps[n];m.header.page_number=48+n;m.header.page_uuid=Id(t.identity++);
    m.header.page_generation=m.map_generation=p.reserved_generation;m.creator_operation_uuid=p.operation_uuid;
    if(fs==t.changed){m.total_pages+=4;++m.capacity_generation;if(n+1==maps.size())m.states.resize(m.states.size()+4,State::free);}}}
  const auto& z=t.zeros.at(t.changed);const auto& map=t.before.at(t.changed).front();auto& r=t.request;
  r.request_uuid=Id(t.identity++);r.operation_uuid=Id(t.identity++);r.action=db::NativeStorageAction::physical_growth;
  r.current_total_pages=z.total_pages;r.first_page=z.total_pages;r.maximum_total_pages=z.total_pages+4;r.page_count=4;
  r.checkpoint={9,0x300,Id(2),p.base_checkpoint.page_number,p.base_checkpoint.page_generation,p.base_checkpoint.page_size_profile_uuid,p.base_checkpoint_object_uuid};
  r.checkpoint_sha256=p.base_checkpoint_sha256;r.checkpoint_generation=p.base_checkpoint_generation;r.checkpoint_root_set_generation=p.base_root_set_generation;
  r.allocation_root={3,3,t.changed,map.header.page_number,map.header.page_generation,map.header.page_size_profile_uuid,map.object_uuid};r.allocation_sha256=Sha(MapOracle(map));
  r.map_generation=map.map_generation;r.capacity_generation=map.capacity_generation;r.directory_generation=t.old_directory.directory_generation;
  r.page_zero_generation=z.page_generation;r.filespace_root_set_generation=z.root_set_generation;
  t.operation.uuid=r.operation_uuid;t.operation.idempotency_key+="-second-growth";t.extent_page_uuid=Id(t.identity++);t.extent_slot=122;t.SetExtent();
  ++Find(t.after.at(t.changed),0).page_generation;
  if(t.reserve)for(u64 n=r.first_page;n<r.first_page+4;++n){auto& m=Cover(t.after.at(t.changed),n);m.states[n-m.first_page]=State::preallocated;
    page::NativeAllocationRecord a;a.page_number=n;a.allocation_uuid=Id(t.identity++);a.owner_uuid=r.allocation_owner_uuid;a.page_type=r.allocation_page_type;a.creator_operation_uuid=p.operation_uuid;m.records.push_back(a);}
  auto cp=t.graph.target.header;cp.page_number=121;cp.page_generation=p.reserved_generation;t.Add(t.after,cp,t.graph.target.object_uuid,p.operation_uuid);
  t.Add(t.after,p.header,p.object_uuid,p.operation_uuid);t.Add(t.after,t.directory.header,t.directory.object_uuid,p.operation_uuid);
  for(const auto& raw:t.extent){const auto h=d::DecodeNativeCommonPageHeader(raw.data(),128);Check(h.ok(),"successor extent header");t.Add(t.after,*h.header,p.management_extent->object_uuid,p.operation_uuid);}
  for(const auto& [fs,maps]:t.after)for(const auto& m:maps)t.Add(t.after,m.header,m.object_uuid,p.operation_uuid);
  u64 payload=t.fixture.size+8+2*(u64{z.bootstrap.page_size_bytes}+8);for(const auto& [fs,maps]:t.after)for(const auto& m:maps)payload+=m.header.page_size_bytes+8;
  const auto count=(payload+t.fixture.size-385)/(t.fixture.size-384);t.bundle_headers.clear();Check(count<48,"equal-profile repeated fixture bundle fits original free run");
  for(u64 n=0;n<count;++n){d::NativeCommonPageHeader h{u32(t.fixture.size),0x500,Id(1),Id(2),Id(t.identity++),150+n,p.reserved_generation,0,p.header.page_size_profile_uuid};
    t.bundle_headers.push_back(h);t.Add(t.after,h,Id(29909),p.operation_uuid);}
  t.Refresh();Check(t.Validate()==CE::none,"second growth preserves complete original allocation history");
  for(const auto* group:{&t.after_images,&t.extent,&t.bundle})for(const auto& raw:*group)Write(raw);Write(DirectoryImageOracle(t.directory));Write(t.plan_image);Write(t.target_cp);
  const auto decoded=d::DecodeFilespacePageZero(t.growth.back().data(),t.growth.back().size());Check(decoded.ok(),"successor result image");const auto& next=*decoded.record;const byte end=0;
  auto* device=File(t.changed);Check(device->WriteAt(next.total_pages*u64{next.bootstrap.page_size_bytes}-1,&end,1).ok()&&device->WriteAt(0,t.growth.back().data(),t.growth.back().size()).ok(),"actual repeated growth fixture");
  Select();for(const auto& file:t.fixture.devices)Check(file.device->Sync().ok(),"repeated growth barrier");
 }
 void Good(const db::NativeManagementGraphHistory& h)const{Check(h.ok()&&h.entries.size()==1&&h.entries.front().control_allocation_images==t.after_images&&
    h.entries.front().control_inventory_images==t.inventory_after&&h.entries.front().control_directory_images==Pages{DirectoryImageOracle(t.directory)}&&
    h.entries.front().control_growth_images==t.growth&&h.entries.front().record.normalized_request_bytes==t.operation.normalized_request_bytes,"complete actual directory/growth provenance");}
 void Good(const db::NativeManagementHistory& h)const{Check(h.selection.has_value(),"actual selected history retains its selection");Good(static_cast<const db::NativeManagementGraphHistory&>(h));}
 void Reopen(bool writable=false){auto& f=t.fixture;for(const auto& file:f.devices)if(file.device->is_open())Check(file.device->Close().ok(),"close historical fixtures");
   const auto mode=writable?d::FileOpenMode::open_existing:d::FileOpenMode::open_existing_read_only;
   Check(f.device.Open(f.path.string(),mode).ok()&&f.secondary.Open(secondary_path.string(),mode).ok()&&
     untouched.Open(untouched_path.string(),mode).ok(),"owned actual history reopen");}
};
void EmptyHistory(const db::NativeManagementHistory& h){Check(!h.ok()&&!h.anchor&&!h.selection&&h.entries.empty()&&h.latest.empty()&&h.idempotency.empty()&&!h.verified_image_bytes,"failed history has no provenance prefix");}
#include "native_allocation_lease_checks.hpp"
void DirectoryInstallationRefusals(unsigned primary){for(bool readonly:{false,true}){
 DirectoryHistoryFixture f(primary,primary,true,readonly?3:2,true,true,false,false);auto& t=f.t;
 if(readonly)Check(t.fixture.secondary.Close().ok()&&t.fixture.secondary.Open(f.secondary_path.string(),d::FileOpenMode::open_existing_read_only).ok(),"readonly candidate member fixture");
 const auto initial=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);Check(initial.ok(),"ineligible destination does not invalidate original selected files");
 auto held=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),t.fixture.devices,Id(2),*initial.snapshot,t.graph.plan.operation_uuid,f.budget,&t.graph.plan.intent);Check(held.ok(),"reserved destination-refusal fixture");
 t.graph.plan.reservation_state_sha256=held.lease->snapshot().state_sha256;t.Refresh();const auto original=held.lease->effects();
 reads=writes=syncs=0;io_counting=true;const auto result=db::InstallNativeManagementControlGraphOnLease(*held.lease,t.graph.plan,t.target_cp,t.extent,t.bundle,f.budget);io_counting=false;
 Check(result.error==(readonly?db::NativePublicationError::invalid_device:db::NativePublicationError::allocation_mismatch)&&!result.snapshot&&!writes&&!syncs&&result.effects==original,"ineligible control placement refuses before any candidate effects");
 const auto tiny=db::InstallNativeManagementControlGraphOnLease(*held.lease,t.graph.plan,t.target_cp,t.extent,t.bundle,1);
 Check(tiny.error==db::NativePublicationError::resource_exhausted&&!tiny.snapshot&&tiny.effects==original,"bounded refusal retains only prior reservation effects");
}}
void DirectoryInstallation(unsigned primary,unsigned secondary,bool reverse,unsigned profile,bool target,bool resume,bool reserve=true){
 DirectoryHistoryFixture f(primary,secondary,reverse,profile,target,reserve,true,false);auto& t=f.t;
 // Publication retains both original and candidate ancestry. Its fixture
 // ceiling accounts for the largest supplied member, not just primary pages.
 f.budget=8*1024*std::max<u64>(t.fixture.size,t.zeros.at(t.other).bootstrap.page_size_bytes);
 const auto initial=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);
 if(!initial.ok())std::cerr<<"directory initial admission="<<int(initial.error)<<'\n';Check(initial.ok(),"complete actual original directory admission");
 auto held=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),t.fixture.devices,Id(2),*initial.snapshot,t.graph.plan.operation_uuid,f.budget,&t.graph.plan.intent);
 Check(held.ok(),"actual directory publication lease");t.graph.plan.reservation_state_sha256=held.lease->snapshot().state_sha256;t.Refresh();Check(t.Validate()==CE::none,"exact lease-bound complete directory transition");
 std::map<Uuid,Bytes> expected;
 const auto all=[&](const d::NativeFilespaceDevice& file){const auto size=file.device->Size();Check(size.ok(),"complete fixture extent");Bytes raw(size.size_bytes);const auto io=file.device->ReadAt(0,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size(),"complete actual filespace image");return raw;};
 for(const auto& file:t.fixture.devices)expected.emplace(file.filespace_uuid,all(file));
 const auto overlay=[&](const Bytes& raw){const auto h=d::DecodeNativeCommonPageHeader(raw.data(),128);Check(h.ok(),"oracle target header");auto& bytes=expected.at(h.header->filespace_uuid);const auto offset=h.header->page_number*u64{raw.size()};Check(offset<=bytes.size()&&raw.size()<=bytes.size()-offset,"oracle target capacity");std::copy(raw.begin(),raw.end(),bytes.begin()+offset);};
 for(const auto* group:{&t.after_images,&t.inventory_after,&t.bundle,&t.extent})for(const auto& raw:*group)overlay(raw);overlay(t.plan_image);overlay(t.target_cp);overlay(DirectoryImageOracle(t.directory));
 for(const auto& root:t.graph.zero.roots){if(root.kind==20||root.kind==21){auto raw=t.fixture.Read(root.page_number);Check(LoadLittle16(raw.data()+136)==4,"profiled original watermark format");
     Ref(raw,516,Self(t.graph.plan.header));Put(raw,564,t.graph.plan.object_uuid);const auto hash=Sha(t.plan_image);std::copy(hash.begin(),hash.end(),raw.begin()+580);std::copy(t.graph.plan.reservation_state_sha256.begin(),t.graph.plan.reservation_state_sha256.end(),raw.begin()+612);
     Bytes material{'S','B','P','A','G','S','T','4'};material.insert(material.end(),raw.begin()+128,raw.begin()+368);material.insert(material.end(),raw.begin()+432,raw.begin()+768);const auto state=Sha(material);std::copy(state.begin(),state.end(),raw.begin()+368);
     std::fill(raw.begin()+400,raw.begin()+432,0);const auto seal=Sha(raw);std::copy(seal.begin(),seal.end(),raw.begin()+400);overlay(raw);}
   if(root.kind==18||root.kind==19){const auto decoded=db::DecodeNativeCheckpointSelection(t.fixture.Read(root.page_number));Check(decoded.ok(),"original exact selector oracle input");auto s=*decoded.selection;
     s.previous_selection_generation=s.selection_generation;s.previous_checkpoint=s.checkpoint;s.previous_checkpoint_object_uuid=s.checkpoint_object_uuid;s.previous_checkpoint_sha256=s.checkpoint_sha256;++s.selection_generation;
     s.publication_uuid=t.graph.plan.operation_uuid;s.checkpoint=t.graph.plan.target_checkpoint;s.checkpoint_object_uuid=t.graph.plan.target_checkpoint_object_uuid;s.checkpoint_sha256=Sha(t.target_cp);s.checkpoint_generation=t.graph.plan.reserved_generation;s.root_set_generation=t.graph.plan.target_root_set_generation;overlay(SelectorOracle(s));}}
 const auto installed=db::InstallNativeManagementControlGraphOnLease(*held.lease,t.graph.plan,t.target_cp,t.extent,t.bundle,f.budget);
 if(!installed.ok())std::cerr<<"directory install profile="<<profile<<" primary="<<primary<<" secondary="<<secondary<<" error="<<int(installed.error)<<'\n';
 Check(installed.ok()&&installed.effects.installed_graph_verified&&installed.snapshot->selection.checkpoint_sha256==initial.snapshot->selection.checkpoint_sha256,"mixed control pages actually installed without selecting target");
 if(profile==4)for(const auto& file:t.fixture.devices){const auto& original=t.zeros.at(file.filespace_uuid==Id(11)?t.other:file.filespace_uuid);
   const auto size=file.device->Size();Check(size.ok()&&size.size_bytes==original.total_pages*u64{original.bootstrap.page_size_bytes},"growth staging cannot extend any actual member");
   if(file.filespace_uuid==t.changed){Bytes zero(t.growth.front().size());Check(file.device->ReadAt(0,zero.data(),zero.size()).ok()&&zero==t.growth.front(),"growth staging preserves exact original mutable body");}}
 if(resume){const auto pending=*installed.snapshot;held.lease.reset();f.Reopen(true);
   held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),t.fixture.devices,Id(2),pending,t.graph.plan.operation_uuid,t.graph.plan.intent,f.budget);Check(held.ok(),"reacquire exact original mixed publication");
   const auto replay=db::ResumeNativeManagementControlGraphOnLease(*held.lease,f.budget);
   if(!replay.ok())std::cerr<<"directory resume profile="<<profile<<" error="<<int(replay.error)<<'\n';
   Check(replay.ok()&&!replay.effects.write_attempts&&replay.effects.installed_graph_verified,"complete mixed reconstruction is exact and does not rewrite identical installed pages");}
 // This test invokes the actual physical primitive explicitly. It does not
 // qualify the still-separate runtime storage admission/worker action adapter.
 if(profile==3){const auto physical=f.File(t.changed)->PreallocateExtent(t.request.first_page*u64{t.request.page_size_bytes},t.request.page_count*u64{t.request.page_size_bytes});
   Check(physical.ok()&&!physical.logical_size_extended&&f.File(t.changed)->Sync().ok(),"actual contained physical preallocation before metadata selection");}
 db::NativePublicationInspection published;
 if(profile==4){const auto before=held.lease->effects();const auto premature=db::PublishNativeManagementControlGraphOnLease(*held.lease,f.budget);
   Check(!premature.ok()&&!premature.snapshot&&premature.effects==before,"candidate growth graph cannot be selected before physical extension and metadata publication");
   const auto physical=f.File(t.changed)->PreallocateExtent(t.request.first_page*u64{t.request.page_size_bytes},t.request.page_count*u64{t.request.page_size_bytes});
   Check(physical.ok()&&physical.logical_size_extended&&f.File(t.changed)->Sync().ok(),"explicit real physical growth after durable reconstruction staging");
   const d::FilespaceBootstrapBinding binding{Id(1),t.changed,t.request.page_size_profile_uuid};
   const auto body=d::WriteFilespacePageZeroGrowthBodyFromOpenDevice(*f.File(t.changed),binding,t.growth.front(),t.growth.back(),f.budget);
   Check(body.ok()&&body.postimage_verified&&body.sync_completed,"actual synchronized growth body before selector recovery");
   auto& bytes=expected.at(t.changed);bytes.resize((t.request.current_total_pages+t.request.page_count)*u64{t.request.page_size_bytes});std::copy(t.growth.back().begin(),t.growth.back().end(),bytes.begin());
   held.lease.reset();published=db::RecoverNativeManagementCheckpointPublicationOnOpenDevices(Id(1),t.fixture.devices,Id(2),t.graph.plan.operation_uuid,t.graph.plan.intent,f.budget);
 }else published=db::PublishNativeManagementControlGraphOnLease(*held.lease,f.budget);
 if(!published.ok())std::cerr<<"directory publish profile="<<profile<<" primary="<<primary<<" secondary="<<secondary<<" reverse="<<reverse<<" target="<<target<<" resume="<<resume<<" error="<<int(published.error)<<'\n';
 Check(published.ok()&&published.effects.selected_graph_verified&&published.snapshot->selection.checkpoint_sha256==Sha(t.target_cp),"actual mixed directory graph selected after complete publication");
 held.lease.reset();const auto actual=db::ReadNativeManagementControlAuthorityFromOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);Check(actual.ok()&&actual.publications.size()==1,"ordinary metadata ancestry admits actual mixed publication");
 for(const auto& file:t.fixture.devices)Check(all(file)==expected.at(file.filespace_uuid),"whole-file oracle preserves every unrelated byte and exact selectors/anchors across all members");
 for(const auto* group:{&t.after_images,&t.inventory_after,&t.bundle,&t.extent})for(const auto& raw:*group){const auto h=d::DecodeNativeCommonPageHeader(raw.data(),128);Check(h.ok(),"expected installed header");Bytes observed(raw.size());
   Check(f.File(h.header->filespace_uuid)->ReadAt(h.header->page_number*u64{raw.size()},observed.data(),observed.size()).ok()&&observed==raw,"actual mixed publication equals independent complete image");}
 f.Reopen();f.Good(f.Read());const auto cold=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);Check(cold.ok()&&cold.selection->checkpoint_sha256==Sha(t.target_cp),"read-only reopen independently admits actually published mixed graph");
 if(primary==secondary&&!reverse){for(const auto& file:t.fixture.devices)Check(file.device->Close().ok(),"close published files before independent reader");
   const auto child=fork();Check(child>=0,"independent mixed publication observer");if(!child){f.Reopen();const auto actual=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);
     _exit(actual.ok()&&actual.selection->checkpoint_sha256==Sha(t.target_cp)?0:84);}
   int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"independent process observes actual selected mixed publication");}
}
void EmptyGraph(const db::NativeManagementGraphHistory& h){Check(!h.ok()&&!h.anchor&&h.entries.empty()&&h.latest.empty()&&h.idempotency.empty()&&!h.verified_image_bytes,"failed graph has no provenance prefix");}
struct DirectoryInstallRequest {
 DirectoryHistoryFixture f;db::NativePublicationReservation held;unsigned stage;
 db::NativePublicationSnapshot original;
 std::map<std::pair<Uuid,u64>,Bytes> original_pages;
 DirectoryInstallRequest(unsigned profile,unsigned phase,bool saturate=false,unsigned primary=0,unsigned secondary=1,bool reverse=true,bool target=true):f(primary,secondary,reverse,profile,target,true,true,false),stage(phase){
  f.budget=16*1024*std::max<u64>(f.t.fixture.size,d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes);auto& t=f.t;const auto initial=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);Check(initial.ok(),"fault fixture original selected state");
  held=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),t.fixture.devices,Id(2),*initial.snapshot,t.graph.plan.operation_uuid,f.budget,&t.graph.plan.intent);Check(held.ok(),"fault fixture exact lease");
  t.graph.plan.reservation_state_sha256=held.lease->snapshot().state_sha256;t.Refresh();
  if(stage){const auto result=db::InstallNativeManagementControlGraphOnLease(*held.lease,t.graph.plan,t.target_cp,t.extent,t.bundle,f.budget);Check(result.ok(),"fault fixture original installation");
    if(stage==1){const auto pending=*result.snapshot;held.lease.reset();Pages targets=t.after_images;targets.insert(targets.end(),t.inventory_after.begin(),t.inventory_after.end());targets.push_back(DirectoryImageOracle(t.directory));targets.push_back(t.target_cp);
      for(const auto& raw:targets){const auto h=d::DecodeNativeCommonPageHeader(raw.data(),128);Check(h.ok(),"recoverable candidate page source");Bytes zero(raw.size());Check(f.File(h.header->filespace_uuid)->WriteAt(h.header->page_number*u64{zero.size()},zero.data(),zero.size()).ok(),"actual missing candidate page after retained reconstruction anchor");}
      for(const auto& file:t.fixture.devices)Check(file.device->Sync().ok(),"missing metadata fixture barrier");f.Reopen(true);
      held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),t.fixture.devices,Id(2),pending,t.graph.plan.operation_uuid,t.graph.plan.intent,f.budget);Check(held.ok(),"cold reacquired reconstruction lease");
    }else if(profile==3){const auto physical=f.File(t.changed)->PreallocateExtent(t.request.first_page*u64{t.request.page_size_bytes},t.request.page_count*u64{t.request.page_size_bytes});Check(physical.ok()&&f.File(t.changed)->Sync().ok(),"actual fault fixture physical preparation");}}
  if(saturate){byte scratch=0;for(const auto& file:t.fixture.devices)for(unsigned i=0;i<4097;++i)Check(file.device->ReadAt(0,&scratch,1).ok(),"installation telemetry saturation");}
  original=held.lease->snapshot();
  const auto retain=[&](const Uuid& fs,u64 slot,u64 size){Bytes raw(size);const auto io=f.File(fs)->ReadAt(slot*size,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size(),"independent original fault-fixture page");original_pages.emplace(std::make_pair(fs,slot),std::move(raw));};
  for(const auto* group:{&t.after_images,&t.inventory_after,&t.extent,&t.bundle})for(const auto& raw:*group){const auto h=d::DecodeNativeCommonPageHeader(raw.data(),128);Check(h.ok(),"fault-fixture target header");retain(h.header->filespace_uuid,h.header->page_number,raw.size());}
  retain(Id(2),t.directory.header.page_number,t.fixture.size);retain(Id(2),t.graph.plan.header.page_number,t.fixture.size);retain(Id(2),t.graph.plan.target_checkpoint.page_number,t.fixture.size);
  for(const auto& root:t.graph.zero.roots)if(root.kind>=18&&root.kind<=21)retain(Id(2),root.page_number,t.fixture.size);
 }
 void Reset(){held.lease.reset();std::set<Uuid> changed;
   for(const auto& [key,before]:original_pages){Bytes raw(before.size());auto* file=f.File(key.first);const auto io=file->ReadAt(key.second*u64{raw.size()},raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size(),"observe actual previous fault effects");
     if(raw!=before){const auto write=file->WriteAt(key.second*u64{before.size()},before.data(),before.size());Check(write.ok()&&write.bytes_transferred==before.size(),"restore exact independent fault-fixture preimage");changed.insert(key.first);}}
   for(const auto& fs:changed)Check(f.File(fs)->Sync().ok(),"fault-fixture reset is physically synchronized");
   held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),f.t.fixture.devices,Id(2),original,f.t.graph.plan.operation_uuid,f.t.graph.plan.intent,f.budget);Check(held.ok(),"fresh lease verifies exact restored original state between isolated fault cases");
 }
 db::NativePublicationInspection Call(){auto& t=f.t;if(stage==2)return db::PublishNativeManagementControlGraphOnLease(*held.lease,f.budget);
   if(stage==1)return db::ResumeNativeManagementControlGraphOnLease(*held.lease,f.budget);
   return db::InstallNativeManagementControlGraphOnLease(*held.lease,t.graph.plan,t.target_cp,t.extent,t.bundle,f.budget);}
 u64 Loss()const{u64 result=0;for(const auto& file:f.t.fixture.devices)result+=file.device->failed_io_latency_observations();return result;}
 void Effects(const db::NativePublicationInspection& r,const db::NativePublicationEffects& before){Check(r.effects==held.lease->effects()&&r.effects.observed&&r.effects.write_attempts-before.write_attempts==writes&&
   r.effects.sync_attempts-before.sync_attempts==syncs&&r.effects.confirmed_bytes>=before.confirmed_bytes&&r.effects.successful_syncs>=before.successful_syncs,"every real mixed-file effect retained on success or failure");}
};
void DirectoryInstallFaults(unsigned profile,unsigned stage,unsigned route,unsigned shard){using E=db::NativePublicationError;
 unsigned nreads=0,nwrites=0,nsyncs=0,nhashes=0,nstats=0;unsigned long nallocations=0;
 DirectoryInstallRequest r(profile,stage,route==2);
 {const auto before=r.held.lease->effects();reads=writes=syncs=0;allocations=0;hash_seen=0;
  counting=route==2;hash_counting=route==1;
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
  historical_stats=0;historical_stat_counting=route==0;
#endif
  io_counting=true;const auto result=r.Call();counting=hash_counting=io_counting=false;
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
  historical_stat_counting=false;nstats=historical_stats;
#endif
  Check(result.ok(),"actual installation fault baseline succeeds");r.Effects(result,before);nreads=reads;nwrites=writes;nsyncs=syncs;nhashes=hash_seen;nallocations=allocations;}
 std::cout<<"directory effects profile="<<profile<<" stage="<<stage<<" route="<<route<<" reads="<<nreads<<" writes="<<nwrites<<" syncs="<<nsyncs<<" hashes="<<nhashes<<" allocations="<<nallocations<<'\n';
 const auto run=[&](unsigned kind,unsigned long at,std::size_t torn=0){r.Reset();const auto before=r.held.lease->effects();const auto loss=r.Loss();
  reads=writes=syncs=0;io_counting=true;if(kind==0)read_fault=at;if(kind==1){write_fault=at;torn_bytes=torn;}if(kind==2)sync_fault=at;
  if(kind==3){hash_fault=shard%5+1;hash_target=at;hash_seen=0;hash_active=false;}if(kind==4)allocation_budget=at;
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
  if(kind==5){historical_stats=0;historical_stat_counting=true;historical_stat_fault=at;}
#endif
  const auto result=r.Call();const auto left=allocation_budget;allocation_budget=-1;const bool hash_consumed=!hash_fault;hash_fault=0;read_fault=write_fault=sync_fault=0;torn_bytes=0;io_counting=false;
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
  historical_stat_counting=false;historical_stat_fault=0;if(kind==5)Check(historical_stats>=at,"actual mixed publication size fault consumed");
#endif
  r.Effects(result,before);
  if(kind==4&&result.ok()){Check(left==-1&&r.Loss()==loss+1,"only measured optional telemetry loss may preserve installation success");return;}
  const auto expected=kind==3?E::hash_failure:kind==4?E::resource_exhausted:E::io_failure;
  if(result.error!=expected)std::cerr<<"directory fault kind="<<kind<<" at="<<at<<" error="<<int(result.error)<<'\n';
  Check(result.error==expected&&!result.snapshot,"required mixed publication fault cannot return completion");
  if(kind==0)Check(reads>=at,"actual read fault consumed");if(kind==1)Check(writes>=at&&result.effects.uncertain_write,"failed or torn write retains uncertainty");if(kind==2)Check(syncs>=at,"actual synchronization fault consumed");
  if(kind==3)Check(hash_consumed,"actual digest fault consumed");if(kind==4)Check(left==-1,"actual allocation fault consumed");
 };
 if(route==2){for(unsigned long at=shard;at<nallocations;at+=16)run(4,at);}
 else if(route==1){for(unsigned at=shard/5+1;at<=nhashes;at+=stage==2?4:1)run(3,at);}
 else{for(unsigned at=1;at<=nreads;++at)run(0,at);for(unsigned at=1;at<=nwrites;++at){run(1,at);run(1,at,17);}for(unsigned at=1;at<=nsyncs;++at)run(2,at);for(unsigned at=1;at<=nstats;++at)run(5,at);}
 r.Reset();
}
void DirectoryPublicationCrashes(unsigned profile,unsigned primary,unsigned secondary,bool damaged_payload=false){
 for(bool reverse:{false,true})for(bool target:{false,true}){
  if(profile==2&&target)continue;
  DirectoryInstallRequest r(profile,2,false,primary,secondary,reverse,target);auto& f=r.f;auto& t=f.t;
  reads=writes=syncs=0;io_counting=true;const auto baseline=r.Call();io_counting=false;
  Check(baseline.ok(),"mixed publication crash baseline");const auto nw=writes,ns=syncs;
  if(damaged_payload)Check(nw==2,"damaged payload sweep covers both selector writes");
  for(unsigned kind=0;kind<4;++kind)for(unsigned at=1;at<=(kind<2?nw:ns);++at){
   if(damaged_payload&&kind!=1)continue;
   r.Reset();r.held.lease.reset();for(const auto& file:t.fixture.devices)Check(file.device->Close().ok(),"close every parent member before writer interruption");
   const auto child=fork();Check(child>=0,"fork actual mixed publication writer");
   if(!child){f.Reopen(true);r.held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),t.fixture.devices,Id(2),r.original,t.graph.plan.operation_uuid,t.graph.plan.intent,f.budget);
    if(!r.held.ok())_exit(81);writes=syncs=0;kill_write=kind<2?at:0;kill_sync=kind>=2?at:0;kill_after_sync=kind==3;torn_bytes=kind==1?(damaged_payload?256:f.t.fixture.size/2):0;io_counting=true;
    (void)r.Call();_exit(87);}
   int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"mixed publication writer exits at exact physical boundary");
   if(damaged_payload){
    f.Reopen();unsigned damaged=0,valid=0;
    for(const auto& root:t.graph.zero.roots)if(root.kind==18||root.kind==19){
     const auto bytes=t.fixture.Read(root.page_number);const auto decoded=db::DecodeNativeCheckpointSelection(bytes);
     Check(d::DecodeNativeCommonPageHeader(bytes.data(),128).ok(),"torn selector retains valid common header");
     if(decoded.error==db::NativeCheckpointSelectionError::invalid_integrity)++damaged;
     else {Check(decoded.ok(),"other selector remains valid after torn payload");++valid;}
    }
    Check(damaged==1&&valid==1,"actual selector payload is damaged before independent recovery");
    for(const auto& file:t.fixture.devices)Check(file.device->Close().ok(),"close payload inspection before independent recovery");
   }
   const auto recovery=fork();Check(recovery>=0,"fork independent mixed publication recovery");
   if(!recovery){f.Reopen(true);const auto result=db::RecoverNativeManagementCheckpointPublicationOnOpenDevices(Id(1),t.fixture.devices,Id(2),t.graph.plan.operation_uuid,t.graph.plan.intent,f.budget);
    if(!result.ok()){std::cerr<<"mixed recovery error="<<int(result.error)<<'\n';_exit(83);}
    const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);
    const auto authority=db::ReadNativeManagementControlAuthorityFromOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);
    _exit(selected.ok()&&selected.selection->checkpoint_sha256==Sha(t.target_cp)&&authority.ok()&&authority.publications.size()==1&&authority.publications.contains(t.graph.plan.operation_uuid)?0:84);}
   Check(waitpid(recovery,&status,0)==recovery&&WIFEXITED(status),"independent mixed recovery exits normally");
   if(WEXITSTATUS(status))std::cerr<<"mixed crash profile="<<profile<<" primary="<<primary<<" secondary="<<secondary<<" kind="<<kind<<" at="<<at<<'\n';
   Check(WEXITSTATUS(status)==0,"every interrupted mixed selection retains original publication and recovers exact graph");f.Reopen(true);
  }
 }
}
void DirectoryGrowthStagingCrashes(unsigned primary,unsigned secondary){
 for(bool target:{false,true}){
  DirectoryInstallRequest r(4,0,false,primary,secondary,true,target);auto& f=r.f;auto& t=f.t;
  reads=writes=syncs=0;io_counting=true;const auto baseline=r.Call();io_counting=false;
  Check(baseline.ok(),"growth staging crash baseline");const auto nw=writes,ns=syncs;
  for(unsigned kind=0;kind<4;++kind)for(unsigned at=1;at<=(kind<2?nw:ns);++at){
   r.Reset();r.held.lease.reset();for(const auto& file:t.fixture.devices)Check(file.device->Close().ok(),"close growth staging parent");
   const auto child=fork();Check(child>=0,"fork actual growth staging writer");
   if(!child){f.Reopen(true);r.held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),t.fixture.devices,Id(2),r.original,t.graph.plan.operation_uuid,t.graph.plan.intent,f.budget);
    if(!r.held.ok())_exit(81);writes=syncs=0;kill_write=kind<2?at:0;kill_sync=kind>=2?at:0;kill_after_sync=kind==3;torn_bytes=kind==1?600:0;io_counting=true;(void)r.Call();_exit(87);}
   int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"growth staging interruption reaches exact physical boundary");
   const auto recovery=fork();Check(recovery>=0,"fork original growth staging recovery");
   if(!recovery){f.Reopen(true);const auto observed=db::RecoverNativePublicationGenerationOnOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);
    if(!observed.ok()||observed.snapshot->watermark.operation_uuid!=t.graph.plan.operation_uuid||observed.snapshot->watermark.intent!=t.graph.plan.intent||
      observed.snapshot->selection.checkpoint_sha256!=r.original.selection.checkpoint_sha256)_exit(82);
    auto held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),t.fixture.devices,Id(2),*observed.snapshot,t.graph.plan.operation_uuid,t.graph.plan.intent,f.budget);
    if(!held.ok())_exit(83);reads=writes=syncs=0;io_counting=true;const auto restored=db::ResumeNativeManagementControlGraphOnLease(*held.lease,f.budget);io_counting=false;
    const bool anchored=observed.snapshot->watermark.publication_plan.has_value();
    if(anchored?!restored.ok():(restored.error!=db::NativePublicationError::invalid_request||writes||syncs))_exit(84);
    // An unanchored interrupted request remains pending with its real staged
    // bytes. Refusal is not completion or permission to allocate a new attempt.
    held.lease.reset();const auto selected=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);
    if(!selected.ok()||selected.snapshot->selection.checkpoint_sha256!=r.original.selection.checkpoint_sha256)_exit(85);
    for(const auto& file:t.fixture.devices){const auto& before=t.zeros.at(file.filespace_uuid==Id(11)?t.other:file.filespace_uuid);const auto size=file.device->Size();
     if(!size.ok()||size.size_bytes!=before.total_pages*u64{before.bootstrap.page_size_bytes})_exit(88);}
    Bytes actual(t.growth.front().size());const auto io=f.File(t.changed)->ReadAt(0,actual.data(),actual.size());
    _exit(io.ok()&&io.bytes_transferred==actual.size()&&actual==t.growth.front()?0:89);}
   Check(waitpid(recovery,&status,0)==recovery&&WIFEXITED(status),"growth staging recovery exits normally");
   if(WEXITSTATUS(status))std::cerr<<"growth staging primary="<<primary<<" secondary="<<secondary<<" target="<<target<<" kind="<<kind<<" at="<<at<<" status="<<WEXITSTATUS(status)<<'\n';
   Check(WEXITSTATUS(status)==0,"durable growth anchor implies reconstructible metadata and never unperformed physical effects");f.Reopen(true);
  }
 }
}
void DirectoryAuthority(DirectoryHistoryFixture& f,unsigned expected=1){
 const auto actual=db::ReadNativeManagementControlAuthorityFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),f.budget);
 if(!actual.ok())std::cerr<<"directory authority profile="<<f.t.profile<<" error="<<int(actual.error)<<'\n';
 Check(actual.ok()&&actual.publications.size()==expected,"actual complete directory control ancestry");
 if(control_authority_memory_tests)control_authority_memory::Checks(f.t.fixture.devices,actual,f.budget);
 const auto& p=f.t.graph.plan;const auto operation=actual.publications.find(p.operation_uuid);
 Check(operation!=actual.publications.end()&&operation->second.page==p.target_checkpoint&&operation->second.sha256==Sha(f.t.target_cp),"exact selected directory publication");
 std::size_t allocations=0,preallocations=0;
 for(const auto& [fs,maps]:f.t.after)for(const auto& map:maps)for(const auto& r:map.records){if(r.creator_operation_uuid.is_nil())continue;
   const auto state=map.states[r.page_number-map.first_page];Check(db::MatchesNativeManagementControlAllocation(actual,fs,r,state),"actual every retained cross-filespace allocation");
   if(state==State::allocated)++allocations;else if(state==State::preallocated)++preallocations;else Check(false,"retained record state");}
 Check(actual.allocations.size()==allocations&&actual.preallocations.size()==preallocations,"no additional allocation authority");
 const auto graph=db::ReadNativeManagementControlGraphFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),*actual.anchor,f.budget);
 Check(graph.ok()&&graph.allocations==actual.allocations&&graph.preallocations==actual.preallocations,"explicit actual graph and selected metadata agree");
 if(control_authority_memory_tests)control_authority_memory::Checks(f.t.fixture.devices,graph,f.budget,control_authority_memory::C::anchored,&*actual.anchor);
}
void DirectoryGrowthSelectionRecovery(unsigned primary,unsigned secondary,int fault_route=-1,unsigned shard=0){
 for(bool reverse:{false,true})for(bool target:{false,true})for(bool reserved:{false,true}){
  if(fault_route>=0&&fault_route<3&&(!reverse||!reserved))continue;
  DirectoryHistoryFixture f(primary,secondary,reverse,4,target,reserved);auto& t=f.t;auto& p=t.graph.plan;
  f.budget=32768*std::max<u64>(t.fixture.size,d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes);
  const auto slot=[&](unsigned kind)->const d::FilespaceRootReference&{const auto at=std::find_if(t.graph.zero.roots.begin(),t.graph.zero.roots.end(),[&](const auto& r){return r.kind==kind;});Check(at!=t.graph.zero.roots.end(),"growth recovery original control slot");return *at;};
  const auto genesis=db::DecodeNativePublicationWatermark(t.fixture.Read(slot(20).page_number));Check(genesis.ok(),"growth recovery original watermark shape");
  auto original=*genesis.state;original.base_checkpoint=p.base_checkpoint;original.base_checkpoint_object_uuid=p.base_checkpoint_object_uuid;
  original.base_checkpoint_sha256=p.base_checkpoint_sha256;original.base_checkpoint_generation=p.base_checkpoint_generation;original.base_root_set_generation=p.base_root_set_generation;
  const auto original_image=db::EncodeNativePublicationWatermark(original);Check(original_image.ok(),"growth recovery exact original base watermark");
  auto pending=original;pending.previous_watermark=original.watermark;pending.watermark=p.reserved_generation;
  pending.previous_state_sha256=original_image.state_sha256;pending.operation_uuid=p.operation_uuid;pending.intent=p.intent;
  const auto bare=db::EncodeNativePublicationWatermark(pending);Check(bare.ok(),"canonical retained growth request watermark");
  p.reservation_state_sha256=bare.state_sha256;t.Refresh();f.Write(t.plan_image);f.Write(t.target_cp);f.Select();
  pending.publication_plan=db::NativePublicationWatermark::PlanAnchor{Self(p.header),p.object_uuid,Sha(t.plan_image),bare.state_sha256};
  for(unsigned i=0;i<2;++i){const auto old=db::DecodeNativePublicationWatermark(t.fixture.Read(slot(20+i).page_number));Check(old.ok(),"growth watermark replica header");
   pending.header=old.state->header;const auto image=db::EncodeNativePublicationWatermark(pending);Check(image.ok(),"exact anchored growth watermark");f.Write(image.bytes);}
  std::array<Bytes,2> old_selectors,new_selectors;
  for(unsigned i=0;i<2;++i){new_selectors[i]=t.fixture.Read(slot(18+i).page_number);const auto decoded=db::DecodeNativeCheckpointSelection(new_selectors[i]);Check(decoded.ok(),"complete growth target selector");
   auto old=*decoded.selection;old.selection_generation=*p.base_selection_generation;old.previous_selection_generation=0;old.previous_checkpoint.reset();old.previous_checkpoint_object_uuid={};old.previous_checkpoint_sha256={};
   old.publication_uuid=t.graph.zero.creation_operation_uuid;old.checkpoint=p.base_checkpoint;old.checkpoint_object_uuid=p.base_checkpoint_object_uuid;old.checkpoint_sha256=p.base_checkpoint_sha256;
   old.checkpoint_generation=p.base_checkpoint_generation;old.root_set_generation=p.base_root_set_generation;old_selectors[i]=SelectorOracle(old);Check(db::DecodeNativeCheckpointSelection(old_selectors[i]).ok(),"independent exact original selector");}
  std::vector<std::pair<d::FileDevice*,Bytes>> expected;
  for(const auto& file:t.fixture.devices){Check(file.device->Sync().ok(),"fully installed physical growth fixture barrier");const auto size=file.device->Size();Check(size.ok(),"actual grown fixture size");Bytes raw(size.size_bytes);
   const auto io=file.device->ReadAt(0,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size(),"complete grown fixture postimage");expected.emplace_back(file.device,std::move(raw));}
  const auto call=[&]{return db::RecoverNativeManagementCheckpointPublicationOnOpenDevices(Id(1),t.fixture.devices,Id(2),p.operation_uuid,p.intent,f.budget);};
  const auto reset=[&]{for(const auto& raw:old_selectors)f.Write(raw);Check(t.fixture.device.Sync().ok(),"original growth selector fault preimages synchronized");};
  const auto exact=[&](bool selected){for(const auto& [file,bytes]:expected){const auto size=file->Size();Check(size.ok()&&size.size_bytes==bytes.size(),"growth selector work never changes capacity");Bytes actual(bytes.size());
    const auto io=file->ReadAt(0,actual.data(),actual.size());Check(io.ok()&&io.bytes_transferred==actual.size(),"complete actual growth effect image");
    if(!selected&&file==&t.fixture.device)for(unsigned i=0;i<2;++i)std::copy(new_selectors[i].begin(),new_selectors[i].end(),actual.begin()+slot(18+i).page_number*t.fixture.size);
    Check(actual==bytes,"growth recovery cannot affect anything except its original selector slots");}};
  if(fault_route>=0&&fault_route<3){
   reset();for(const auto& file:t.fixture.devices){byte scratch=0;for(unsigned i=0;i<4097;++i)Check(file.device->ReadAt(0,&scratch,1).ok(),"growth telemetry warmup");}
   reads=writes=syncs=hash_seen=0;allocations=0;io_counting=true;hash_counting=fault_route==1;counting=fault_route==2;
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
   historical_stats=0;historical_stat_counting=fault_route==0;
#endif
   const auto baseline=call();io_counting=hash_counting=counting=false;
   const auto nr=reads,nw=writes,ns=syncs,nh=hash_seen;const auto na=allocations;
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
   historical_stat_counting=false;const auto nstats=historical_stats;
#endif
   Check(baseline.ok(),"growth recovery fault baseline");exact(true);
   std::cout<<"growth recovery target="<<target<<" route="<<fault_route<<" reads="<<nr<<" writes="<<nw<<" syncs="<<ns<<" hashes="<<nh<<" allocations="<<na<<'\n';
   const auto loss=[&]{u64 n=0;for(const auto& file:t.fixture.devices)n+=file.device->failed_io_latency_observations();return n;};
   const auto run=[&](unsigned kind,unsigned long at,std::size_t torn=0){reset();const auto lost=loss();reads=writes=syncs=0;io_counting=true;preallocation_fd=-1;
    if(kind==0)read_fault=at;if(kind==1){write_fault=at;torn_bytes=torn;}if(kind==2)sync_fault=at;
    if(kind==3){hash_fault=shard%5+1;hash_target=at;hash_seen=0;hash_active=false;}if(kind==4)allocation_budget=at;
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
    if(kind==5){historical_stats=0;historical_stat_counting=true;historical_stat_fault=at;}
#endif
    const auto result=call();const auto left=allocation_budget;allocation_budget=-1;const bool consumed=!hash_fault;
    hash_fault=read_fault=write_fault=sync_fault=0;torn_bytes=0;io_counting=false;
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
    historical_stat_counting=false;historical_stat_fault=0;if(kind==5)Check(historical_stats>=at,"growth stat fault consumed");
#endif
    Check(result.effects.observed&&result.effects.write_attempts==writes&&result.effects.sync_attempts==syncs&&preallocation_fd==-1,"growth recovery reports every physical effect without allocating again");
    if(kind==4&&result.ok())Check(left==-1&&loss()==lost+1,"only accounted optional telemetry allocation failure permits growth completion");
    else{const auto want=kind==3?db::NativePublicationError::hash_failure:kind==4?db::NativePublicationError::resource_exhausted:db::NativePublicationError::io_failure;
     if(result.error!=want)std::cerr<<"growth recovery fault kind="<<kind<<" site="<<at<<" target="<<target<<" error="<<unsigned(result.error)<<'\n';
     Check(result.error==want&&!result.snapshot,"growth fault preserves typed error and no completion snapshot");}
    if(kind==0)Check(reads>=at,"growth read fault consumed");if(kind==1)Check(writes>=at&&result.effects.uncertain_write,"growth torn selector has retained uncertain effects");
    if(kind==2)Check(syncs>=at,"growth sync fault consumed");if(kind==3)Check(consumed,"growth digest fault consumed");if(kind==4)Check(left==-1,"growth allocation failure consumed");exact(result.ok());
   };
   if(fault_route==2){for(unsigned long at=shard;at<na;at+=32)run(4,at);}
   else if(fault_route==1){for(unsigned at=shard/5+1;at<=nh;at+=4)run(3,at);}
   else{for(unsigned at=1;at<=nr;++at)run(0,at);for(unsigned at=1;at<=nw;++at){run(1,at);run(1,at,256);}for(unsigned at=1;at<=ns;++at)run(2,at);
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
    for(unsigned at=1;at<=nstats;++at)run(5,at);
#endif
   }
   reset();Check(call().ok(),"original growth operation remains recoverable after fault sweep");exact(true);continue;
  }
  if(fault_route==3){reset();reads=writes=syncs=0;io_counting=true;const auto baseline=call();io_counting=false;Check(baseline.ok(),"growth crash baseline");const auto nw=writes,ns=syncs;
   for(unsigned kind=0;kind<4;++kind)for(unsigned at=1;at<=(kind<2?nw:ns);++at){reset();for(const auto& file:t.fixture.devices)Check(file.device->Close().ok(),"close growth parent before interruption");
    const auto child=fork();Check(child>=0,"fork actual growth selector recovery");if(!child){f.Reopen(true);writes=syncs=0;kill_write=kind<2?at:0;kill_sync=kind>=2?at:0;kill_after_sync=kind==3;torn_bytes=kind==1?256:0;io_counting=true;(void)call();_exit(87);}
    int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"growth recovery exits at each actual selector write or barrier");
    const auto recovery=fork();Check(recovery>=0,"fork independent resumed growth recovery");if(!recovery){f.Reopen(true);const auto result=call();_exit(result.ok()?0:83);}
    Check(waitpid(recovery,&status,0)==recovery&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"interrupted growth recovery completes only original operation");f.Reopen(true);exact(true);
   }continue;
  }
  reset();for(unsigned defect=0;defect<3;++defect){auto invalid=t.growth.back();if(defect==0)invalid=t.growth.front();
   if(defect==1){std::copy_n(t.growth.front().begin()+4096,97,invalid.begin()+4096);
    Check(invalid!=t.growth.front()&&invalid!=t.growth.back()&&!d::DecodeFilespacePageZero(invalid.data(),invalid.size()).ok(),"growth negative contains actual torn metadata rather than an unchanged prefix");}
   if(defect==2){const auto decoded=d::DecodeFilespacePageZero(invalid.data(),invalid.size());Check(decoded.ok(),"growth metadata contradiction source");auto z=*decoded.record;++z.page_generation;const auto encoded=d::EncodeFilespacePageZero(z);Check(encoded.ok(),"canonical contradictory growth body");invalid=*encoded.bytes;}
   auto* file=f.File(t.changed);Check(file->WriteAt(0,invalid.data(),invalid.size()).ok()&&file->Sync().ok(),"actual incomplete or contradictory growth body");
   reads=writes=syncs=0;io_counting=true;const auto denied=call();io_counting=false;
   Check(!denied.ok()&&!denied.snapshot&&!writes&&!syncs,"selector completion cannot repair or bless incomplete physical-growth metadata");
   Bytes observed(invalid.size());Check(file->ReadAt(0,observed.data(),observed.size()).ok()&&observed==invalid,"refusal preserves actual damaged growth bytes");
   Check(file->WriteAt(0,t.growth.back().data(),t.growth.back().size()).ok()&&file->Sync().ok(),"restore exact completed physical-growth test body");}
  for(unsigned variant=0;variant<5;++variant){auto selectors=variant==2?new_selectors:old_selectors;
   if(variant==1||variant==4)selectors[0]=new_selectors[0];
   if(variant==3||variant==4){const unsigned damaged=variant==3?0:1;std::copy_n(new_selectors[damaged].begin(),256,selectors[damaged].begin());
    Check(db::DecodeNativeCheckpointSelection(selectors[damaged]).error==db::NativeCheckpointSelectionError::invalid_integrity,"growth recovery actual damaged selector payload");}
   for(unsigned i=0;i<2;++i)f.Write(selectors[i]);Check(t.fixture.device.Sync().ok(),"growth selection preimage barrier");
   reads=writes=syncs=0;io_counting=true;auto wrong=p.intent;wrong.request_context_uuid=Id(32000);
   const auto refused=db::RecoverNativeManagementCheckpointPublicationOnOpenDevices(Id(1),t.fixture.devices,Id(2),p.operation_uuid,wrong,f.budget);io_counting=false;
   if(refused.error!=db::NativePublicationError::request_mismatch)std::cerr<<"growth wrong-request refusal="<<unsigned(refused.error)<<'\n';
   Check(refused.error==db::NativePublicationError::request_mismatch&&!refused.snapshot&&!writes&&!syncs,"growth selection cannot substitute another request");
   preallocation_fd=-1;const auto result=db::RecoverNativeManagementCheckpointPublicationOnOpenDevices(Id(1),t.fixture.devices,Id(2),p.operation_uuid,p.intent,f.budget);
   if(!result.ok())std::cerr<<"growth selection primary="<<primary<<" secondary="<<secondary<<" target="<<target<<" variant="<<variant<<" error="<<unsigned(result.error)<<'\n';
   Check(result.ok()&&result.effects.installed_graph_verified&&result.effects.selected_graph_verified&&preallocation_fd==-1,
     "actual fully installed growth selection recovers without claiming or repeating physical allocation");
   for(const auto& [file,bytes]:expected){const auto size=file->Size();Check(size.ok()&&size.size_bytes==bytes.size(),"selector recovery never changes physical capacity");Bytes actual(bytes.size());
    const auto io=file->ReadAt(0,actual.data(),actual.size());Check(io.ok()&&io.bytes_transferred==actual.size()&&actual==bytes,"independent complete grown-node byte oracle");}
  }
  for(const auto& file:t.fixture.devices)Check(file.device->Close().ok(),"close all grown members before independent admission");
  const auto child=fork();Check(child>=0,"fresh grown publication observer");
  if(!child){f.Reopen();const auto selected=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);
   _exit(selected.ok()&&selected.snapshot->selection.checkpoint_sha256==Sha(t.target_cp)?0:84);}
  int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh process admits recovered growth selection");
 }
}
void DirectoryControlInstalledPages(DirectoryHistoryFixture& f){
 Pages images;for(const auto& [fs,maps]:f.t.after)images.push_back(MapOracle(maps.front()));images.push_back(DirectoryImageOracle(f.t.directory));
 for(const auto& raw:f.t.inventory_after)images.push_back(raw);
 for(const auto& raw:images){const auto h=d::DecodeNativeCommonPageHeader(raw.data(),128);Check(h.ok(),"actual installed control corruption source");const auto& header=*h.header;
   auto* file=f.File(header.filespace_uuid);Bytes corrupt(raw.size());const auto offset=header.page_number*u64{raw.size()};
   Check(file->WriteAt(offset,corrupt.data(),corrupt.size()).ok(),"remove actual installed control image without changing reconstruction history");
   f.Good(f.Read());reads=writes=syncs=0;io_counting=true;
   const auto denied=db::ReadNativeManagementControlAuthorityFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),f.budget);io_counting=false;
   Check(!denied.ok()&&!denied.anchor&&!denied.selection&&denied.publications.empty()&&denied.allocations.empty()&&denied.preallocations.empty()&&!denied.verified_image_bytes&&
     !writes&&!syncs,"valid embedded reconstruction cannot replace missing installed control bytes");
   Bytes observed(raw.size());Check(file->ReadAt(offset,observed.data(),observed.size()).ok()&&observed==corrupt,"verification does not reconstruct missing pages");
   Check(file->WriteAt(offset,raw.data(),raw.size()).ok()&&file->Sync().ok(),"restore independently encoded actual control image");DirectoryAuthority(f);
 }
}
void HistoricalInventory(DirectoryHistoryFixture& f){
 const auto& cp=f.t.graph.base;const auto at=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==1;});Check(at!=cp.roots.end(),"original inventory root");
 const d::FilespaceRootReference root{4,at->page_type,at->page.filespace_uuid,at->page.page_number,at->page.page_generation,at->page.page_size_profile_uuid,at->object_uuid};
 auto context=f.Context();for(const auto& [id,z]:f.t.zeros){const auto encoded=d::EncodeFilespacePageZero(z);Check(encoded.ok(),"original canonical inventory context");context.at(id)=*encoded.bytes;}
 const auto raw=f.t.fixture.Read(root.page_number);const auto decoded=page::DecodeNativeTransactionInventoryPage(raw);Check(decoded.ok()&&InventoryOracle(*decoded.page)==raw,"independent actual original inventory image");
 u64 minimum=raw.size();for(const auto& [id,image]:context)minimum+=3*image.size();
 const auto call=[&](const std::map<Uuid,Bytes>& images,const std::array<byte,32>& hash,u64 budget){return page::ReadNativeTransactionInventoryChainAtHistoricalRootFromOpenDevices(Id(1),f.t.fixture.devices,root,hash,images,budget);};
 const auto good=[&](const page::NativeTransactionInventoryChainResult& r){Check(r.ok()&&r.pages.size()==1&&r.pages.front().bytes==raw&&r.retained_image_bytes==minimum,"complete exact historical inventory with bounded contexts");};
 const auto empty=[&](const page::NativeTransactionInventoryChainResult& r){Check(!r.ok()&&r.pages.empty()&&r.inventory.entries.empty()&&!r.retained_image_bytes,"failed inventory returns no prefix");};
 reads=writes=syncs=0;io_counting=true;good(call(context,at->sha256,minimum));io_counting=false;Check(!writes&&!syncs,"historical inventory has no effects");
 const auto short_budget=call(context,at->sha256,minimum-1);Check(short_budget.error==page::NativeInventoryError::resource_exhausted,"exact historical inventory image ceiling");empty(short_budget);
 auto wrong=at->sha256;wrong.front()^=1;empty(call(context,wrong,minimum));auto missing=context;missing.erase(missing.begin());empty(call(missing,at->sha256,minimum));
 const auto original=f.t.fixture.Read(0);auto torn=original;std::fill(torn.begin()+4096,torn.begin()+4480,0);
 Check(f.t.fixture.device.WriteAt(0,torn.data(),torn.size()).ok(),"torn actual primary inventory context fixture");good(call(context,at->sha256,minimum));Check(f.t.fixture.Read(0)==torn,"inventory reader never repairs metadata");
 torn.front()^=1;Check(f.t.fixture.device.WriteAt(0,torn.data(),torn.size()).ok(),"invalid immutable inventory context fixture");empty(call(context,at->sha256,minimum));
 Check(f.t.fixture.device.WriteAt(0,original.data(),original.size()).ok()&&f.t.fixture.device.Sync().ok(),"restore actual inventory context");good(call(context,at->sha256,minimum));
}
void DirectoryControlFaults(unsigned profile,unsigned route,unsigned shard){
 using E=db::NativeManagementControlAuthorityError;
 for(bool target:{false,true})for(unsigned anchored=0;anchored<3;++anchored){if(profile==2&&target)continue;
  DirectoryHistoryFixture f(0,0,true,profile,target,true,true);const auto history=f.Read();f.Good(history);
  const auto context=f.Context();if(anchored==2){auto torn=context.at(Id(2));std::fill(torn.begin()+4096,torn.begin()+4480,0);Check(f.t.fixture.device.WriteAt(0,torn.data(),torn.size()).ok(),"actual torn current body during control failure qualification");}
  const auto call=[&]() -> db::NativeManagementControlGraph {if(anchored==2)return db::ReadNativeManagementControlGraphAtHistoricalContextFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),*history.anchor,context,f.budget);
    if(anchored)return db::ReadNativeManagementControlGraphFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),*history.anchor,f.budget);
    auto h=db::ReadNativeManagementControlAuthorityFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),f.budget);Check(h.ok()?h.selection.has_value():!h.selection,"failed selected graph cannot retain a selector");return std::move(static_cast<db::NativeManagementControlGraph&>(h));};
  const auto good=[&](const db::NativeManagementControlGraph& h){Check(h.ok()&&h.publications.size()==1,"control fault baseline");};
  const auto empty=[&](const db::NativeManagementControlGraph& h){Check(!h.ok()&&!h.anchor&&h.publications.empty()&&h.allocations.empty()&&h.preallocations.empty()&&!h.verified_image_bytes,"failed control graph has no authority prefix");};
  const auto loss=[&]{u64 n=0;for(const auto& file:f.t.fixture.devices)n+=file.device->failed_io_latency_observations();return n;};
  byte scratch=0;for(const auto& file:f.t.fixture.devices)for(unsigned i=0;i<4097;++i)Check(file.device->ReadAt(0,&scratch,1).ok(),"control fault telemetry warmup");
  if(route==2){counting=true;allocations=0;const auto measured=call();counting=false;good(measured);const auto sites=allocations;
    std::cout<<"control allocations="<<sites<<" profile="<<profile<<" target="<<target<<" anchored="<<anchored<<'\n';
    for(unsigned long at=shard;at<=sites;at+=8){const auto previous=loss();allocation_budget=at;const auto h=call();const auto left=allocation_budget;allocation_budget=-1;
      if(at==sites)Check(left>=0&&h.ok(),"exact control allocation boundary");else{if(h.error!=E::resource_exhausted&&!h.ok())std::cerr<<"control allocation at="<<at<<" error="<<int(h.error)<<'\n';
        Check(left==-1&&(h.error==E::resource_exhausted||(h.ok()&&loss()==previous+1)),"required control allocation fails or measured optional observation lost");if(!h.ok())empty(h);}}
  }else if(route==1){hash_counting=true;hash_seen=0;const auto measured=call();hash_counting=false;good(measured);const auto sites=hash_seen;
    for(unsigned at=1;at<=sites;++at){hash_fault=shard+1;hash_target=at;hash_seen=0;hash_active=false;const auto h=call();
      if(h.error!=E::hash_failure)std::cerr<<"control hash at="<<at<<" error="<<int(h.error)<<'\n';Check(!hash_fault&&h.error==E::hash_failure,"every control graph digest fault consumed");empty(h);}
  }else{reads=writes=syncs=0;io_counting=true;const auto measured=call();io_counting=false;good(measured);const auto sites=reads;Check(!writes&&!syncs,"control graph read baseline has no effects");
    for(unsigned at=1;at<=sites;++at){reads=writes=syncs=0;read_fault=at;io_counting=true;const auto h=call();io_counting=false;read_fault=0;
      if(h.error!=E::io_failure)std::cerr<<"control read at="<<at<<" error="<<int(h.error)<<'\n';Check(reads>=at&&!writes&&!syncs&&h.error==E::io_failure,"every control graph actual read fault consumed");empty(h);}
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
    historical_stats=0;historical_stat_counting=true;const auto measured_size=call();historical_stat_counting=false;good(measured_size);const auto size_sites=historical_stats;
    for(unsigned at=1;at<=size_sites;++at){historical_stats=0;historical_stat_fault=at;historical_stat_counting=true;const auto h=call();historical_stat_counting=false;historical_stat_fault=0;
      Check(historical_stats>=at&&h.error==E::io_failure,"every control graph actual size fault consumed");empty(h);}
#endif
  }
 }
}
void DirectoryControlClose(unsigned profile){
 for(bool target:{false,true})for(bool historical:{false,true})for(unsigned member=0;member<3;++member){
  DirectoryHistoryFixture f(profile,profile,true,4,target,true);const auto h=f.Read();f.Good(h);const auto context=f.Context();
  if(historical){auto torn=context.at(Id(2));std::fill(torn.begin()+4096,torn.begin()+4480,0);Check(f.t.fixture.device.WriteAt(0,torn.data(),torn.size()).ok(),"torn current metadata for guarded control read");}
  historical_read_pause=1;auto reader=std::async(std::launch::async,[&]() -> db::NativeManagementControlGraph {
    if(historical)return db::ReadNativeManagementControlGraphAtHistoricalContextFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),*h.anchor,context,f.budget);
    auto r=db::ReadNativeManagementControlAuthorityFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),f.budget);return std::move(static_cast<db::NativeManagementControlGraph&>(r));});
  struct Release {~Release(){historical_read_pause=3;}} release;
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(historical_read_pause!=2&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  Check(historical_read_pause==2,"control graph paused with all actual device guards");std::atomic<bool> entered=false;
  auto closer=std::async(std::launch::async,[&]{entered=true;return f.t.fixture.devices[member].device->Close();});
  while(!entered&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  const bool blocked=closer.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout;historical_read_pause=3;
  const auto result=reader.get();const auto closed=closer.get();historical_read_pause=0;
  Check(entered&&blocked&&closed.ok()&&result.ok()&&result.publications.size()==1,"Close on every participating filespace waits for complete actual control verification");
 }
}
void RepeatedDirectoryHistory(unsigned profile,int only_size=-1){for(unsigned size=0;size<5;++size)for(bool target:{false,true}){
 if(only_size>=0&&size!=unsigned(only_size))continue;
 DirectoryHistoryFixture f(size,size,true,profile,target,true);const auto first=f.Read();f.Good(first);const auto original=first.entries.front();const auto context=f.Context();
 const auto original_graph=db::ReadNativeManagementControlAuthorityFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),f.budget);Check(original_graph.ok(),"original graph before further growth");f.NextGrowth();
 DirectoryAuthority(f,2);const auto next=f.Read();Check(next.ok()&&next.entries.size()==2&&next.entries[0].control_growth_images==original.control_growth_images&&
   next.entries[0].control_directory_images==original.control_directory_images&&next.entries[1].control_growth_images==f.t.growth,"reverse growth history retains both exact generations");
 const auto historical=[&](const std::map<Uuid,Bytes>& images){return db::ReadNativeManagementGraphHistoryAtHistoricalContextFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),*first.anchor,images,f.budget);};
 const auto old=historical(context);Check(old.ok()&&old.entries.size()==1&&old.entries[0].control_growth_images==original.control_growth_images,"explicit older anchor uses its original result context after later growth");
 history_memory::Checks(f.t.fixture.devices,next,f.budget);
 history_memory::Checks(f.t.fixture.devices,old,f.budget,history_memory::C::historical_result,&*first.anchor,&context);
 const auto control=[&]{return db::ReadNativeManagementControlGraphAtHistoricalContextFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),*first.anchor,context,f.budget);};
 const auto earlier=control();Check(earlier.ok()&&earlier.allocations==original_graph.allocations&&earlier.preallocations==original_graph.preallocations,"older actual control graph is not compared to later page-zero capacity");
 if(control_authority_memory_tests)control_authority_memory::Checks(f.t.fixture.devices,earlier,f.budget,control_authority_memory::C::historical_result,&*first.anchor,&context,size==0&&!target);
 auto missing=context;missing.erase(missing.begin());EmptyGraph(historical(missing));auto wrong=context;wrong.begin()->second.back()^=1;EmptyGraph(historical(wrong));
 auto substituted=context;auto& image=substituted.begin()->second;const auto decoded=d::DecodeFilespacePageZero(image.data(),image.size());Check(decoded.ok(),"historical context negative source");
 auto zero=*decoded.record;++zero.page_generation;const auto canonical=d::EncodeFilespacePageZero(zero);Check(canonical.ok(),"canonical substituted historical context");image=*canonical.bytes;EmptyGraph(historical(substituted));
 const auto first_file=context.begin()->first;auto* first_device=f.File(first_file);const auto original_size=first_device->Size();Check(original_size.ok(),"historical race size source");
 growth_extend_to=original_size.size_bytes+1;const auto raced=historical(context);Check(!growth_extend_to&&!raced.ok(),"whole historical graph rejects an actual changing filespace extent");EmptyGraph(raced);
 std::filesystem::resize_file(first_file==Id(2)?f.t.fixture.path:f.secondary_path,original_size.size_bytes);
 const auto actual=f.Context();auto torn=actual.at(f.t.changed);std::fill(torn.begin()+4096,torn.begin()+4480,0);auto* device=f.File(f.t.changed);
 Check(device->WriteAt(0,torn.data(),torn.size()).ok()&&device->Sync().ok(),"actual torn later result fixture");EmptyHistory(f.Read());
 const auto recovered=historical(context);Check(recovered.ok()&&recovered.entries.size()==1&&recovered.entries[0].control_directory_images==original.control_directory_images,"historical graph never needs a valid current mutable body");
 history_memory::Checks(f.t.fixture.devices,recovered,f.budget,history_memory::C::historical_result,&*first.anchor,&context);
 const auto actual_old=control();Check(actual_old.ok()&&actual_old.allocations==original_graph.allocations&&actual_old.preallocations==original_graph.preallocations,"torn current body cannot invalidate exact retained actual old control ancestry");
 if(control_authority_memory_tests)control_authority_memory::Checks(f.t.fixture.devices,actual_old,f.budget,control_authority_memory::C::historical_result,&*first.anchor,&context);
 Bytes observed(torn.size());Check(device->ReadAt(0,observed.data(),observed.size()).ok()&&observed==torn,"historical graph does not repair a torn body");
 const auto& restored=actual.at(f.t.changed);Check(device->WriteAt(0,restored.data(),restored.size()).ok()&&device->Sync().ok(),"restore exact current result");
 f.Reopen();const auto reopened=f.Read();Check(reopened.ok()&&reopened.entries.size()==2&&reopened.entries[0].control_allocation_images==original.control_allocation_images,"old allocation capacity survives repeated growth and readonly reopen");
}}

#include "native_catalog_relation_memory_checks.hpp"
#include "native_catalog_visibility_memory_checks.hpp"
void DirectoryGuardedBtree(unsigned primary,unsigned secondary){
 // Actual non-serving storage fixture, not allocation or catalog publication.
 DirectoryHistoryFixture f(primary,secondary,false,2,false,false,true,false);
 auto& storage=f.t.fixture;const auto& profile=d::kCanonicalFilespacePageProfiles[secondary];
 const auto size=profile.page_size_bytes;std::array<page::NativeBtreePage,3> nodes;
 const page::NativeBtreeKey split{{2},Id(61310),Id(61311)};
 for(unsigned i=0;i<3;++i){auto& n=nodes[i];
  n.header={size,i?0x202u:0x200u,Id(1),i==2?Id(11):f.t.other,Id(61300+i),248+i,1,0,profile.uuid};
  if(i==2){n.header.page_size_bytes=storage.size;n.header.page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[primary].uuid;}
  n.dependencies={Id(61303),1,1,Id(61304),Id(61305),Id(61306),{}};n.dependencies.dependency_map_sha256.fill(37);
  n.creator_transaction_uuid=Id(5);n.creator_local_transaction_id=1;n.maintenance_state=1;
 }
 nodes[0].tree_level=1;nodes[0].first_child=Self(nodes[1].header);
 nodes[0].cells.push_back({split,false,Self(nodes[2].header),{}});
 nodes[1].parent=nodes[2].parent=Self(nodes[0].header);
 nodes[1].right=Self(nodes[2].header);nodes[2].left=Self(nodes[1].header);
 nodes[1].high_fence=nodes[2].low_fence=split;
 const d::NativePageReference base{Id(2),5,1,d::kCanonicalFilespacePageProfiles[primary].uuid};
 nodes[1].cells.push_back({{{1},Id(61312),Id(61313)},false,{},base});
 nodes[2].cells.push_back({split,true,{},base});
 for(const auto& n:nodes){const auto encoded=page::EncodeNativeBtreePage(n);
  Check(encoded.ok(),"valid canonical native navigation fixture");f.Write(encoded.bytes);}
 for(const auto& id:{f.t.other,Id(11)}){
  auto* device=f.File(id);Bytes zero_image(size),map_image(size);
  Check(device->ReadAt(0,zero_image.data(),size).ok()&&device->ReadAt(size,map_image.data(),size).ok(),"actual original navigation member state");
  auto decoded_zero=d::DecodeFilespacePageZero(zero_image.data(),zero_image.size());
  auto decoded_map=page::DecodeNativeAllocationMap(map_image);
  Check(decoded_zero.ok()&&decoded_map.ok(),"actual map and page zero for navigation fixture");
  auto zero=*decoded_zero.record;auto map=*decoded_map.map;
  if(id==Id(11)){
   const auto& child_profile=d::kCanonicalFilespacePageProfiles[primary];
   zero.bootstrap.page_size_bytes=child_profile.page_size_bytes;zero.bootstrap.page_size_profile_uuid=child_profile.uuid;
   for(auto& r:zero.roots)r.page_size_profile_uuid=child_profile.uuid;
   map.header.page_size_bytes=child_profile.page_size_bytes;map.header.page_size_profile_uuid=child_profile.uuid;
   std::filesystem::resize_file(f.untouched_path,512*static_cast<u64>(child_profile.page_size_bytes));
   for(auto& member:storage.devices)if(member.filespace_uuid==id)member.page_size_profile_uuid=child_profile.uuid;
  }
  for(const auto& n:nodes)if(n.header.filespace_uuid==id){
   Check(map.states[n.header.page_number]==State::free,"native fixture tree slot was genuinely free");
   map.states[n.header.page_number]=State::allocated;--zero.free_pages;
   map.records.push_back({n.header.page_number,Id(61400+n.header.page_number),n.header.page_uuid,n.dependencies.index_uuid,
     n.creator_transaction_uuid,n.creator_local_transaction_id,1,0,n.header.page_type,{}});
  }
  std::sort(map.records.begin(),map.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});
  f.Write(MapOracle(map));const auto encoded=d::EncodeFilespacePageZero(zero);Check(encoded.ok(),"rebound navigation page zero");
  Check(device->WriteAt(0,encoded.bytes->data(),encoded.bytes->size()).ok()&&device->Sync().ok(),"native navigation fixture barrier");
 }
 // Bind the changed third-member profile into the complete selected fixture.
 auto directory=*page::DecodeNativeFilespaceDirectory(f.t.base.directory_images.front()).directory;
 for(auto& member:directory.records)if(member.bootstrap.filespace_uuid==Id(11)){
  member.bootstrap.page_size_bytes=storage.size;member.bootstrap.page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[primary].uuid;}
 const auto directory_image=DirectoryImageOracle(directory);f.Write(directory_image);
 auto checkpoint=*db::DecodeNativeCheckpointRoot(f.t.base_cp).root;
 for(auto& ref:checkpoint.roots)if(ref.role==3)ref.sha256=Sha(directory_image);
 const auto cp=db::EncodeNativeCheckpointRoot(checkpoint);Check(cp.ok(),"complete cross-profile navigation source checkpoint");f.Write(cp.bytes);
 const auto digest=Sha(cp.bytes);
 for(const auto& ref:f.t.graph.zero.roots){
  if(ref.kind==18||ref.kind==19){auto selector=*db::DecodeNativeCheckpointSelection(storage.Read(ref.page_number)).selection;
   selector.checkpoint_sha256=digest;f.Write(SelectorOracle(selector));}
  if(ref.kind==20||ref.kind==21){auto watermark=*db::DecodeNativePublicationWatermark(storage.Read(ref.page_number)).state;
   watermark.base_checkpoint_sha256=digest;const auto encoded=db::EncodeNativePublicationWatermark(watermark);
   Check(encoded.ok(),"cross-profile navigation source watermark");f.Write(encoded.bytes);}
 }
 Check(storage.device.Sync().ok(),"complete cross-profile native source barrier");
 const auto root=Self(nodes[0].header);const auto dependencies=nodes[0].dependencies;
 const db::NativeBtreeTreeMemoryLimits limits{3,2*static_cast<std::size_t>(size)+storage.size};
 const auto expected=page::ReadNativeBtreeTreeFromOpenDevices(Id(1),storage.devices,root,dependencies,limits.maximum_retained_image_bytes);
 Check(expected.ok()&&expected.pages.size()==3&&expected.leaves.size()==2,"independent owning traversal of actual tree before source admission");
 const auto& maximum=d::kCanonicalFilespacePageProfiles.back();
 const auto bytes=db::NativeBtreeTreeWorkspaceBytes(maximum.uuid,storage.devices.size(),limits);
 checkpoint_inventory_memory::Grant grant(bytes),source_grant(32*1024*1024);
 for(unsigned field=0;field<4;++field){auto wrong=grant.binding;
  const std::array<Uuid*,4> ids{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};*ids[field]=Id(61999);
  const auto no=db::PrepareNativeBtreeTreeRead(maximum.uuid,storage.devices.size(),limits,grant.memory,wrong);
  Check(!no.ok()&&!no.workspace&&no.memory_error==db::NativeStorageMemoryError::invalid_binding,"every binary tree grant dimension enforced");}
 {checkpoint_inventory_memory::Grant short_grant(bytes-1);
  const auto no=db::PrepareNativeBtreeTreeRead(maximum.uuid,storage.devices.size(),limits,short_grant.memory,short_grant.binding);
  Check(!no.ok()&&!no.workspace,"one-byte-short actual tree backing cannot fall back");short_grant.memory={};short_grant.Empty();}
 auto prepared=db::PrepareNativeBtreeTreeRead(maximum.uuid,storage.devices.size(),limits,grant.memory,grant.binding);
 Check(prepared.ok(),"complete actual navigation backing admitted before source fences");
 auto moved=std::move(prepared.workspace);
 {
  auto source=db::AcquireNativeSelectedCheckpointMemoryLease(Id(1),storage.devices,Id(2),
    {16*1024*std::max<u64>(storage.size,size),32*1024*1024},source_grant.memory,source_grant.binding);
  Check(source.ok(),"actual selected checkpoint source and cohort held for navigation");
  const auto read=[&]{return db::NativeBtreeTreeLeaseReader::Read(*source.lease,root,dependencies,prepared.workspace);};
  const auto failed=[](const auto& r){Check(!r.ok()&&r.pages.empty()&&r.leaves.empty()&&!r.retained_image_bytes,
    "failed guarded tree exposes no navigation prefix");};
  const auto absent=read();failed(absent);Check(!absent.physical_bytes_read,"moved tree backing refuses without reads");
  prepared.workspace=std::move(moved);
  reads=writes=syncs=0;io_counting=true;hash_counting=true;hash_seen=0;allocation_budget=0;
  auto actual=read();const auto left=allocation_budget;allocation_budget=-1;io_counting=hash_counting=false;
  const auto read_sites=reads,hash_sites=hash_seen;
  Check(left==0&&actual.ok()&&read_sites==9&&!writes&&!syncs&&actual.pages.size()==expected.pages.size()&&
    actual.retained_image_bytes==expected.retained_image_bytes&&std::equal(actual.leaves.begin(),actual.leaves.end(),expected.leaves.begin(),expected.leaves.end()),
    "complete retained navigation tree uses no heap under the existing source guards");
  for(std::size_t i=0;i<actual.pages.size();++i)Check(std::equal(actual.pages[i].bytes.begin(),actual.pages[i].bytes.end(),
    expected.pages[i].bytes.begin(),expected.pages[i].bytes.end()),"guarded navigation matches complete independent owning image");
  for(unsigned mode=0;mode<2;++mode)for(unsigned site=1;site<=read_sites;++site){
   reads=0;history_observed_read_bytes=0;io_counting=true;if(mode)corrupt_read=site;else read_fault=site;
   const auto no=read();io_counting=false;read_fault=corrupt_read=0;failed(no);
   Check(reads>=site&&no.physical_bytes_read==history_observed_read_bytes,"every guarded tree read/corruption preserves physical receipt");}
  for(unsigned mode=1;mode<=5;++mode)for(unsigned site=1;site<=hash_sites;++site){
   hash_fault=mode;hash_target=site;hash_seen=0;hash_active=false;
   const auto no=read();const bool consumed=!hash_fault;hash_fault=0;hash_active=false;failed(no);
   Check(consumed,"each guarded tree hash failure consumed");}
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
  for(unsigned site=1;site<=storage.devices.size();++site){
   historical_stat_counting=true;historical_stats=0;historical_stat_fault=site;
   const auto no=read();historical_stat_counting=false;historical_stat_fault=0;failed(no);
   Check(historical_stats>=site&&no.tree_error==page::NativeBtreeError::io_failure,"each retained member size failure refuses");}
#endif
  auto wrong=dependencies;wrong.storage_generation++;
  const auto no=db::NativeBtreeTreeLeaseReader::Read(*source.lease,root,wrong,prepared.workspace);failed(no);
  Check(no.tree_error==page::NativeBtreeError::binding_mismatch,"complete expected dependency tuple remains required");
  allocation_budget=0;const auto retried=read();const auto remaining=allocation_budget;allocation_budget=-1;
  Check(retried.ok()&&remaining==0,"same retained source can retry complete tree without new backing");
 }
 prepared={};grant.memory={};grant.Empty();
 for(unsigned mode=0;mode<4;++mode){
  if(mode==3&&primary==0&&secondary==0)continue;
  auto bounded=limits;auto devices=storage.devices.size();auto maximum_profile=maximum.uuid;
  if(mode==0)--devices;if(mode==1)--bounded.maximum_pages;if(mode==2)--bounded.maximum_retained_image_bytes;
  if(mode==3)maximum_profile=d::kCanonicalFilespacePageProfiles.front().uuid;
  checkpoint_inventory_memory::Grant narrow(db::NativeBtreeTreeWorkspaceBytes(maximum_profile,devices,bounded));
  auto backing=db::PrepareNativeBtreeTreeRead(maximum_profile,devices,bounded,narrow.memory,narrow.binding);
  Check(backing.ok(),"bounded real tree workspace prepared before source lease");
  {
   auto source=db::AcquireNativeSelectedCheckpointMemoryLease(Id(1),storage.devices,Id(2),
     {16*1024*std::max<u64>(storage.size,size),32*1024*1024},source_grant.memory,source_grant.binding);
   Check(source.ok(),"bounded tree test retains actual selected source");allocation_budget=0;
   const auto no=db::NativeBtreeTreeLeaseReader::Read(*source.lease,root,dependencies,backing.workspace);
   const auto left=allocation_budget;allocation_budget=-1;
   Check(left==0&&!no.ok()&&no.error==db::NativeBtreeTreeMemoryError::resource_exhausted&&
     no.pages.empty()&&no.leaves.empty()&&!no.retained_image_bytes&&((mode==1||mode==2)||!no.physical_bytes_read),
     "every insufficient guarded tree bound fails without fallback or a successful prefix");
  }
  backing={};narrow.memory={};narrow.Empty();
 }
 source_grant.memory={};source_grant.Empty();
 f.Reopen();const auto reopened=page::ReadNativeBtreeTreeFromOpenDevices(Id(1),storage.devices,root,dependencies,limits.maximum_retained_image_bytes);
 Check(reopened.ok()&&reopened.pages.size()==3,"independent read-only reopen preserves actual tree");
}
void DirectoryDistinctCatalogRoots(unsigned primary,unsigned secondary,bool via_route,unsigned creator_fault=0){
 // Build a complete non-serving physical fixture before source admission. This
 // is not an allocation/publication API: map, directory, zero, checkpoint and
 // both selector/watermark replicas are all rebound as fixture construction.
 DirectoryHistoryFixture f(primary,secondary,false,2,false,false,true,false);
 auto& t=f.t;auto& storage=t.fixture;auto cp=*db::DecodeNativeCheckpointRoot(t.base_cp).root;
 const auto cat=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==5;});
 Check(cat!=cp.roots.end(),"distinct-root actual catalog reference");
 const d::FilespaceRootReference cat_ref{2,cat->page_type,cat->page.filespace_uuid,cat->page.page_number,
   cat->page.page_generation,cat->page.page_size_profile_uuid,cat->object_uuid};
 const auto existing=page::ReadNativeCatalogRootFromOpenDevice(*f.File(cat->page.filespace_uuid),Id(1),cat_ref);
 Check(existing.ok(),"distinct-root original catalog image");auto feature=*existing.root;
 const auto& p=d::kCanonicalFilespacePageProfiles[secondary];
 feature.header={p.page_size_bytes,5,Id(1),t.other,Id(61201),248,1,0,p.uuid};
 feature.root_kind=8;feature.object_uuid=Id(61202);
 feature.roots.erase(feature.roots.begin(),feature.roots.end()-1);
 Bytes changed_inventory;
 if(creator_fault==1)feature.creator_transaction_uuid=Id(61203);
 if(creator_fault==2)feature.creator_local_transaction_id=999999;
 if(creator_fault>=3){
  auto inventory_ref=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==1;});
  Check(inventory_ref!=cp.roots.end(),"actual inventory for unresolved root creator fixture");
  const auto stored=page::DecodeNativeTransactionInventoryPage(storage.Read(inventory_ref->page.page_number));
  Check(stored.ok(),"actual original inventory image");auto inventory=*stored.page;
  auto begun=mga::BeginLocalTransaction(inventory.inventory,{UuidKind::transaction,Id(61204)},2000000000000ULL);
  Check(begun.ok(),"valid pure inventory candidate for uncommitted creator");
  const auto identity=begun.inventory.entries.back().identity;
  if(creator_fault==4){auto rollback=mga::RollbackLocalTransaction(begun.inventory,identity.local_id,2000000000001ULL);
   Check(rollback.ok(),"valid rollback creator inventory candidate");begun.inventory=std::move(rollback.inventory);}
  inventory.inventory=std::move(begun.inventory);changed_inventory=InventoryOracle(inventory);
  Check(page::EncodeNativeTransactionInventoryPage(inventory).bytes==changed_inventory,"independent complete creator inventory bytes");
  inventory_ref->sha256=Sha(changed_inventory);cp.selected_local_transaction_id=inventory.inventory.next_local_transaction_id-1;
  feature.creator_transaction_uuid=identity.transaction_uuid.value;feature.creator_local_transaction_id=identity.local_id.value;
 }
 const auto image=page::EncodeNativeCatalogRoot(feature);Check(image.ok(),"distinct-root complete feature image");
 auto maps=t.before.at(t.other);auto& map=Cover(maps,248);
 Check(map.states[248-map.first_page]==State::free,"distinct feature slot is actually free in the fixture map");
 map.states[248-map.first_page]=State::allocated;
 map.records.push_back({248,Id(61200),feature.header.page_uuid,feature.object_uuid,
   existing.root->creator_transaction_uuid,existing.root->creator_local_transaction_id,1,0,5,{}});
 std::sort(map.records.begin(),map.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});
 Find(maps,0).page_type=1;
 const auto map_images=EncodeMaps(maps);
 auto other_zero=t.zeros.at(t.other);other_zero.bootstrap.filespace_role=2;--other_zero.free_pages;
 other_zero.roots.push_back({8,5,t.other,248,1,p.uuid,feature.object_uuid});
 const auto other_image=d::EncodeFilespacePageZero(other_zero);Check(other_image.ok(),"complete non-serving metadata member zero");
 auto primary_zero=t.graph.zero;
 auto zero_feature=std::find_if(primary_zero.roots.begin(),primary_zero.roots.end(),[](const auto& r){return r.kind==8;});
 Check(zero_feature!=primary_zero.roots.end(),"actual feature directory slot");*zero_feature=other_zero.roots.back();
 const auto primary_image=d::EncodeFilespacePageZero(primary_zero);Check(primary_image.ok(),"primary cross-profile feature reference");
 auto directory=*page::DecodeNativeFilespaceDirectory(t.base.directory_images.front()).directory;
 auto member=std::find_if(directory.records.begin(),directory.records.end(),[&](const auto& r){return r.bootstrap.filespace_uuid==t.other;});
 Check(member!=directory.records.end(),"actual metadata member directory record");member->bootstrap=other_zero.bootstrap;
 if(member->allocation_root)member->allocation_root->sha256=Sha(map_images.front());
 const auto directory_image=DirectoryImageOracle(directory);
 Check(page::EncodeNativeFilespaceDirectory(directory).bytes==directory_image,"independent complete rebound directory");
 auto cp_directory=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==3;});
 auto cp_feature=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==9;});
 Check(cp_directory!=cp.roots.end()&&cp_feature!=cp.roots.end(),"actual checkpoint directory and feature roles");
 cp_directory->sha256=Sha(directory_image);
 *cp_feature={9,5,Self(feature.header),feature.object_uuid,Sha(image.bytes)};
 const auto checkpoint=db::EncodeNativeCheckpointRoot(cp);Check(checkpoint.ok(),"distinct feature bound checkpoint");
 const auto write=[&](d::FileDevice& device,u64 at,const Bytes& raw){auto io=device.WriteAt(at,raw.data(),raw.size());
   Check(io.ok()&&io.bytes_transferred==raw.size(),"write complete distinct-root fixture image");};
 for(const auto& raw:map_images)f.Write(raw);
 if(!changed_inventory.empty())f.Write(changed_inventory);
 f.Write(image.bytes);f.Write(directory_image);f.Write(checkpoint.bytes);
 write(storage.device,0,*primary_image.bytes);write(storage.secondary,0,*other_image.bytes);
 const auto digest=Sha(checkpoint.bytes);
 for(const auto& ref:primary_zero.roots){
  if(ref.kind==18||ref.kind==19){auto selector=*db::DecodeNativeCheckpointSelection(storage.Read(ref.page_number)).selection;
   selector.checkpoint_sha256=digest;write(storage.device,ref.page_number*storage.size,SelectorOracle(selector));}
  if(ref.kind==20||ref.kind==21){auto watermark=*db::DecodeNativePublicationWatermark(storage.Read(ref.page_number)).state;
   watermark.base_checkpoint_sha256=digest;const auto encoded=db::EncodeNativePublicationWatermark(watermark);
   Check(encoded.ok(),"distinct feature original watermark binding");write(storage.device,ref.page_number*storage.size,encoded.bytes);}
 }
 for(const auto& device:storage.devices)Check(device.device->Sync().ok(),"distinct root fixture barrier");
 const auto allowance=16*1024*std::max<u64>(storage.size,p.page_size_bytes);
 if(creator_fault){
  using E=db::NativeCatalogRootsLeaseError;
  const auto& maximum=d::kCanonicalFilespacePageProfiles.back();
  constexpr std::size_t source_bytes=32*1024*1024;
  checkpoint_inventory_memory::Grant source_grant(source_bytes),root_grant(db::NativeCatalogRootsWorkspaceBytes(maximum.uuid));
  auto prepared=db::PrepareNativeCatalogRootsRead(maximum.uuid,root_grant.memory,root_grant.binding);
  Check(prepared.ok(),"root failure fixture has actual admitted backing");
  {
   auto source=db::AcquireNativeSelectedCheckpointMemoryLease(Id(1),storage.devices,Id(2),{allowance,source_bytes},source_grant.memory,source_grant.binding);
   Check(source.ok(),"actual selected source remains valid independently of the bad catalog creator");
   allocation_budget=0;
   const auto rejected=db::NativeCatalogRootsLeaseReader::Read(*source.lease,storage.size+p.page_size_bytes,prepared.workspace);
   const auto left=allocation_budget;allocation_budget=-1;
   Check(left==0&&rejected.error==(creator_fault<=2?E::creator_mismatch:E::creator_not_committed)&&
     rejected.catalogs.empty()&&!rejected.retained_image_bytes&&
     rejected.physical_bytes_read==2*d::kFilespaceBootstrapBytes+2*(storage.size+p.page_size_bytes),
     "late mismatched missing unresolved or rollback creator has an exact failure and no verified first-root prefix");
  }
  prepared={};root_grant.memory={};source_grant.memory={};root_grant.Empty();source_grant.Empty();return;
 }
 owned_source_memory::Checks(storage.devices,allowance,false,via_route);
 f.Reopen();
 const d::FilespaceRootReference checkpoint_ref{9,0x300,cp.header.filespace_uuid,cp.header.page_number,
   cp.header.page_generation,cp.header.page_size_profile_uuid,cp.object_uuid};
 const auto reopened=db::VerifyNativeCheckpointCatalogRootsFromOpenDevices(Id(1),storage.devices,checkpoint_ref,allowance);
 Check(reopened.ok()&&reopened.catalogs.size()==2&&reopened.feature_root_index==1&&
   reopened.catalogs[1].bytes==image.bytes,"actual distinct cross-profile feature survives independent read-only reopen");
}
void DirectoryOwnedSourceMemory(unsigned primary,unsigned secondary,bool reverse,unsigned profile,
 bool target,bool reserve,bool initial,bool via_route,int fault_route=9,unsigned shard=0,unsigned shards=1,bool populated=false){
 DirectoryHistoryFixture f(primary,secondary,reverse,profile,target,reserve,profile==2,!initial);
 const auto allowance=16*1024*std::max<u64>(f.t.fixture.size,d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes);
 owned_source_memory::Checks(f.t.fixture.devices,allowance,
   primary==secondary&&!reverse&&!target&&reserve==(profile!=2),via_route,fault_route,shard,shards,populated);
 f.Reopen();
 if(populated){
  const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),allowance);
  Check(selected.ok(),"reopened populated fixture selected source");
  const auto& roots=selected.checkpoint_inventory.checkpoint->roots;
  const auto root=std::find_if(roots.begin(),roots.end(),[](const auto& r){return r.role==5;});
  Check(root!=roots.end(),"reopened populated fixture catalog binding");
  auto* device=f.File(root->page.filespace_uuid);
  const d::FilespaceRootReference ref{2,root->page_type,root->page.filespace_uuid,root->page.page_number,
    root->page.page_generation,root->page.page_size_profile_uuid,root->object_uuid};
  const auto catalog=page::ReadNativeCatalogRootFromOpenDevice(*device,Id(1),ref);
  Check(catalog.ok(),"reopened populated fixture catalog root");
  const auto object=std::find_if(catalog.root->roots.begin(),catalog.root->roots.end(),[](const auto& r){return r.role==1;});
  Check(object!=catalog.root->roots.end(),"reopened populated fixture object head");
  const auto leaf=db::ReadNativeCatalogLeafFromOpenDevice(*f.File(object->page.filespace_uuid),Id(1),*object);
  Check(leaf.ok()&&leaf.metadata.size()==2,"populated records survive independent read-only device reopen");
 }
}
void DirectorySelectedLeaseMemory(unsigned primary,unsigned secondary,bool reverse,unsigned profile,
 bool target,bool reserve,bool initial=false,bool repeated=false,int route=8,unsigned shard=0,unsigned shards=1){
 DirectoryHistoryFixture f(primary,secondary,reverse,profile,target,reserve,profile==2,!initial);
 if(repeated)f.NextGrowth();
 const auto allowance=16*1024*std::max<u64>(f.t.fixture.size,d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes);
 const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),allowance);
 Check(selected.ok(),"complete independent selected lease source");
 const auto& raw=initial?f.t.base_cp:f.t.target_cp;
 Check(db::EncodeNativeCheckpointRoot(*selected.checkpoint_inventory.checkpoint).bytes==raw&&
  selected.selection->checkpoint_sha256==Sha(raw),"lease source binds exact independently encoded checkpoint");
 const auto& s=*selected.selection;
 const d::FilespaceRootReference root{9,0x300,s.checkpoint.filespace_uuid,s.checkpoint.page_number,
  s.checkpoint.page_generation,s.checkpoint.page_size_profile_uuid,s.checkpoint_object_uuid};
 const auto directory=db::VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(Id(1),f.t.fixture.devices,root,allowance);
 Check(directory.ok()&&directory.directory.pages.size()==1&&directory.directory.pages.front().bytes==
  (initial?f.t.base.directory_images.front():DirectoryImageOracle(f.t.directory)),
  "lease source binds exact independent complete directory bytes");
 const bool deep=primary==secondary&&!reverse&&!target&&reserve==(profile!=2);
 selected_lease_memory::Checks(f.t.fixture.devices,allowance,deep,route,shard,shards);
 if(route!=8)return;
 f.Reopen();selected_lease_memory::Checks(f.t.fixture.devices,allowance);
}
template<bool allocation> void DirectoryCurrentSourceMemory(unsigned primary,unsigned secondary,bool reverse,unsigned profile,bool target,bool reserve,bool repeated=false,int route=8,unsigned shard=0,unsigned shards=1){
 DirectoryHistoryFixture f(primary,secondary,reverse,profile,target,reserve,profile==2);if(repeated)f.NextGrowth();
 // This composite reader verifies the selector and the current object's full
 // proof. Size its positive fixture allowance for both member profiles, not
 // only the primary; actual backing grants remain independently bounded.
 const auto image_budget=8*1024*std::max<u64>(f.t.fixture.size,d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes);
 const auto& cp=f.t.graph.plan.target_checkpoint;
 const d::FilespaceRootReference root{9,0x300,cp.filespace_uuid,cp.page_number,cp.page_generation,cp.page_size_profile_uuid,f.t.graph.plan.target_checkpoint_object_uuid};
 const auto read=[&](u64 limit){
  if constexpr(allocation)return db::VerifyCurrentNativeCheckpointAllocationFromOpenDevices(Id(1),f.t.fixture.devices,root,limit);
  else return db::VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(Id(1),f.t.fixture.devices,root,limit);};
 const auto expected=read(image_budget);
 if(!expected.ok()){
  std::cerr<<"current source allocation="<<allocation<<" profile="<<profile<<" primary="<<primary<<" secondary="<<secondary
   <<" reverse="<<reverse<<" target="<<target<<" reserve="<<reserve<<" budget="<<image_budget<<" error="<<unsigned(expected.error)
   <<" inventory="<<unsigned(expected.checkpoint_inventory.inventory_error);
  if constexpr(allocation)std::cerr<<" allocation_error="<<unsigned(expected.allocation_error);
  else std::cerr<<" directory_error="<<unsigned(expected.directory_error);
  std::cerr<<'\n';
 }
 Check(expected.ok(),"actual independently authored current source after publication");
 if(!allocation&&profile==2&&primary==0&&secondary==4&&!reverse&&!target&&!reserve&&!repeated){
  const auto short_source=read(f.budget);
  Check(expected.retained_image_bytes>f.budget&&short_source.error==db::NativeCheckpointError::resource_exhausted,
   "primary-only mixed-profile allowance remains a real resource refusal");
  Check(!short_source.checkpoint_inventory.checkpoint&&!short_source.retained_image_bytes,
   "insufficient composite source allowance exposes no checkpoint prefix");
 }
 Check(db::EncodeNativeCheckpointRoot(*expected.checkpoint_inventory.checkpoint).bytes==f.t.target_cp&&
  expected.checkpoint_inventory.checkpoint->creator_operation_uuid==f.t.graph.plan.operation_uuid,
  "current source resolves exact independent checkpoint image and publication identity");
 if constexpr(allocation){const auto& maps=f.t.after.at(Id(2));Check(expected.allocation.pages.size()==maps.size(),"full original allocation chain dimensions");
  for(std::size_t n=0;n<maps.size();++n)Check(expected.allocation.pages[n].bytes==MapOracle(maps[n]),"every independent current allocation image");}
 else Check(expected.directory.pages.size()==1&&expected.directory.pages.front().bytes==DirectoryImageOracle(f.t.directory),"complete independent current directory image");
 const bool deep=primary==secondary&&!reverse&&!target&&reserve==(profile!=2);
 current_source_memory::Checks<allocation>(f.t.fixture.devices,root,expected,image_budget,deep,route,shard,shards);
 if(route!=8)return;
 f.Reopen();const auto reopened=read(image_budget);Check(reopened.ok(),"current source after real read-only reopen");
 current_source_memory::Checks<allocation>(f.t.fixture.devices,root,reopened,image_budget);
}
void DirectorySelectionMemory(unsigned primary,unsigned secondary,bool reverse,unsigned profile,bool target,bool reserve,bool repeated=false,int fault_route=8,unsigned fault_shard=0,unsigned fault_shards=1){
 DirectoryHistoryFixture f(primary,secondary,reverse,profile,target,reserve,profile==2);
 if(repeated)f.NextGrowth();
 const auto read=[&]{return db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),f.budget);};
 const auto original=read();Check(original.ok(),"complete independently authored directory selector");
 const auto& cp=*original.checkpoint_inventory.checkpoint;
 Check(db::EncodeNativeCheckpointRoot(cp).bytes==f.t.target_cp&&cp.creator_operation_uuid==f.t.graph.plan.operation_uuid&&
  original.selection->checkpoint==f.t.graph.plan.target_checkpoint&&original.selection->checkpoint_sha256==Sha(f.t.target_cp),
  "actual selected checkpoint equals the independent fixture image and binary operation");
 const bool deep=primary==0&&secondary==0&&!reverse&&!target&&reserve==(profile!=2);
 bound_selection_memory::Checks(f.t.fixture.devices,original,f.budget,deep,fault_route,fault_shard,fault_shards);
 if(fault_route!=8)return;
 f.Reopen();const auto reopened=read();Check(reopened.ok(),"read-only reopen complete selector");
 bound_selection_memory::Checks(f.t.fixture.devices,reopened,f.budget);
}
void DirectoryHistory(unsigned primary,unsigned secondary,bool reverse,unsigned profile,bool target,bool reserve){
 DirectoryHistoryFixture f(primary,secondary,reverse,profile,target,reserve);reads=writes=syncs=0;io_counting=true;const auto first=f.Read();io_counting=false;
 if(!first.ok())std::cerr<<"history profile="<<profile<<" primary="<<primary<<" secondary="<<secondary<<" error="<<int(first.error)<<'\n';
 f.Good(first);Check(!writes&&!syncs,"history never installs or selects candidate pages");
 const bool deep_memory=primary==0&&secondary==0&&!reverse&&!target&&reserve==(profile!=2);
 history_memory::Checks(f.t.fixture.devices,first,f.budget,history_memory::C::selected,nullptr,nullptr,deep_memory);
 HistoricalInventory(f);if(profile!=2){DirectoryAuthority(f);if(primary==secondary&&!reverse)DirectoryControlInstalledPages(f);}
 else{const auto denied=db::ReadNativeManagementControlAuthorityFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),f.budget);
   Check(!denied.ok()&&!denied.anchor&&!denied.selection&&denied.allocations.empty()&&denied.publications.empty(),"secondary-data inventory images are not eligible actual inventory");
   DirectoryHistoryFixture primary_inventory(primary,secondary,reverse,profile,target,reserve,true);DirectoryAuthority(primary_inventory);
   if(primary==secondary&&!reverse)DirectoryControlInstalledPages(primary_inventory);primary_inventory.Reopen();DirectoryAuthority(primary_inventory);}
 const auto anchored=db::ReadNativeManagementGraphHistoryFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),*first.anchor,f.budget);
 Check(anchored.ok()&&anchored.entries.size()==1&&anchored.entries.front().control_growth_images==f.t.growth,"exact graph anchor retains directory growth history");
 const auto historical=db::ReadNativeManagementGraphHistoryAtHistoricalContextFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),*first.anchor,f.Context(),f.budget);f.Good(historical);
 history_memory::Checks(f.t.fixture.devices,anchored,f.budget,history_memory::C::anchored,&*first.anchor);
 const auto memory_context=f.Context();
 history_memory::Checks(f.t.fixture.devices,historical,f.budget,history_memory::C::historical_result,&*first.anchor,&memory_context,deep_memory);
 if(primary==secondary){auto files=f.t.fixture.devices;files.pop_back();EmptyHistory(db::ReadNativeManagementHistoryFromOpenDevices(Id(1),files,Id(2),f.budget));
  const auto plan=f.t.graph.plan;const auto cp=f.t.target_cp;auto wrong=f.t.graph.target;
  *std::find_if(wrong.roots.begin(),wrong.roots.end(),[](const auto& r){return r.role==3;})=*std::find_if(f.t.graph.base.roots.begin(),f.t.graph.base.roots.end(),[](const auto& r){return r.role==3;});
  f.t.target_cp=Finish(f.t.graph.plan,wrong);f.Write(f.t.target_cp);f.Write(Oracle(f.t.graph.plan));f.Select();Check(f.t.fixture.device.Sync().ok(),"resealed wrong-directory graph fixture");
  const auto mismatch=f.Read();Check(mismatch.error==db::NativeManagementHistoryError::history_mismatch,"self-consistent selector/plan/checkpoint cannot redirect the retained directory");EmptyHistory(mismatch);
  f.t.graph.plan=plan;f.t.target_cp=cp;f.Write(cp);f.Write(f.t.plan_image);f.Select();Check(f.t.fixture.device.Sync().ok(),"restore complete graph");f.Good(f.Read());}
 f.Reopen();f.Good(f.Read());if(profile!=2)DirectoryAuthority(f);
 if(primary==secondary&&!reverse){for(const auto& file:f.t.fixture.devices)Check(file.device->Close().ok(),"release devices before cold history");
  const auto child=fork();Check(child>=0,"independent historical process");if(!child){f.Reopen();const auto h=f.Read();const auto a=db::ReadNativeManagementControlAuthorityFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),f.budget);
    _exit(h.ok()&&h.entries.size()==1&&h.entries.front().control_growth_images==f.t.growth&&(profile==2?!a.ok():a.ok())?0:81);}
  int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh process verifies actual mixed history");}
}
void DirectoryHistoryFaults(unsigned profile){for(bool target:{false,true})for(unsigned route=0;route<2;++route){
 if(profile==2&&target)continue;DirectoryHistoryFixture f(0,0,true,profile,target,true);
 const auto context=f.Context();const auto selected=f.Read();f.Good(selected);
 const auto call=[&]() -> db::NativeManagementGraphHistory {if(route)return db::ReadNativeManagementGraphHistoryAtHistoricalContextFromOpenDevices(Id(1),f.t.fixture.devices,Id(2),*selected.anchor,context,f.budget);
   auto h=f.Read();if(!h.ok())EmptyHistory(h);else Check(h.selection.has_value(),"selected fault result retains its selection");return std::move(static_cast<db::NativeManagementGraphHistory&>(h));};
 const auto loss=[&]{u64 total=0;for(const auto& file:f.t.fixture.devices)total+=file.device->failed_io_latency_observations();return total;};
 byte scratch=0;for(const auto& file:f.t.fixture.devices)for(unsigned i=0;i<4097;++i)Check(file.device->ReadAt(0,&scratch,1).ok(),"history telemetry warmup");
 allocations=0;counting=true;const auto baseline=call();counting=false;const auto sites=allocations;f.Good(baseline);
 for(unsigned long at=0;at<=sites;++at){const auto old_loss=loss();allocation_budget=at;const auto h=call();const auto remaining=allocation_budget;allocation_budget=-1;
  if(at==sites)Check(remaining>=0&&h.ok(),"history complete allocation boundary");
  else{if(h.error!=db::NativeManagementHistoryError::resource_exhausted&&!h.ok())std::cerr<<"history allocation at="<<at<<" error="<<int(h.error)<<'\n';
   Check(remaining==-1&&(h.error==db::NativeManagementHistoryError::resource_exhausted||(h.ok()&&loss()==old_loss+1)),"required history allocation or measured optional loss");if(!h.ok())EmptyGraph(h);}}
 hash_counting=true;hash_seen=0;const auto measured=call();hash_counting=false;const auto hashes=hash_seen;f.Good(measured);
 for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto h=call();
   Check(!hash_fault&&h.error==db::NativeManagementHistoryError::hash_failure,"every history digest failure preserved");EmptyGraph(h);}
 reads=writes=syncs=0;io_counting=true;const auto actual=call();io_counting=false;const auto read_sites=reads;f.Good(actual);Check(!writes&&!syncs,"history baseline no effects");
 for(unsigned at=1;at<=read_sites;++at){reads=writes=syncs=0;read_fault=at;io_counting=true;const auto h=call();io_counting=false;read_fault=0;
   Check(reads>=at&&!writes&&!syncs&&h.error==db::NativeManagementHistoryError::io_failure,"all actual history read failures consumed without effects");EmptyGraph(h);}
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
 historical_stats=0;historical_stat_counting=true;const auto sized=call();historical_stat_counting=false;const auto size_sites=historical_stats;f.Good(sized);
 for(unsigned at=1;at<=size_sites;++at){historical_stats=0;historical_stat_fault=at;historical_stat_counting=true;const auto h=call();historical_stat_counting=false;historical_stat_fault=0;
  Check(historical_stats>=at&&h.error==db::NativeManagementHistoryError::io_failure,"every history size failure consumed");EmptyGraph(h);}
#endif
 std::cout<<"directory history profile="<<profile<<" target="<<target<<" route="<<route<<" allocations="<<sites<<" hashes="<<hashes<<" reads="<<read_sites<<'\n';
}}
void DirectoryDelta(unsigned primary,unsigned secondary,bool reverse,unsigned profile,bool target,bool reserve){
 DirectoryTransition t(primary,secondary,reverse,profile,target,reserve);const auto original=t.fixture.Read(0,512);
 const auto result=t.Validate();if(result!=CE::none)std::cerr<<"directory delta primary="<<primary<<" secondary="<<secondary<<" profile="<<profile<<" target="<<target<<" error="<<int(result)<<'\n';
 Check(result==CE::none,"complete exact directory allocation transition");
 BoundedControlCheck(t.base_cp,t.target_cp,t.plan_image,t.extent,t.before_images,t.after_images,t.Budget(),t.bundle,t.inventory_before,&t.base,CE::none,primary==0&&secondary==0&&!reverse);
 Check(t.Validate(t.Budget()-1)==CE::resource_exhausted,"exact directory input budget boundary");
 Check(db::ValidateNativeManagementControlAllocation(t.base_cp,t.target_cp,t.plan_image,t.extent,t.before_images,t.after_images,t.Budget(),t.bundle,t.inventory_before)==CE::invalid_plan,"legacy entrypoint cannot omit base directory");
 const auto initial_after=t.after;const auto initial_directory=t.directory;const auto initial_base=t.base;const auto initial_request=t.request;
 const auto initial_target=t.graph.target;
 const auto reject=[&](unsigned mode){const auto error=t.Validate();if(error==CE::none)std::cerr<<"accepted invalid directory delta mode="<<mode<<" profile="<<profile<<'\n';Check(error!=CE::none,"canonical images cannot authorize an unrelated directory/allocation mutation");};
 for(unsigned mode=0;mode<16;++mode){
  if(mode>=4&&primary!=secondary)continue;
  if(profile==2&&mode>=8&&mode<=13)continue;
  t.after=initial_after;t.directory=initial_directory;t.base=initial_base;t.request=initial_request;t.graph.target=initial_target;
  if(mode==0)Find(t.after.at(t.other),0).allocation_uuid=Id(55000);
  if(mode==1)++t.directory.records.front().verification_epoch;
  if(mode==2)t.directory.records.back().locator_uuid=Id(55001);
  if(mode==3){const auto n=t.before.at(Id(2)).front().header.page_number;Find(t.after.at(Id(2)),n).owner_uuid=Id(55002);}
  if(mode==4)t.base.page_zero_images.pop_back();
  if(mode==5)t.base.page_zero_images.push_back(t.base.page_zero_images.front());
  if(mode==6)t.base.directory_images.front().back()^=1;
  if(mode==7){auto& m=Cover(t.after.at(t.other),20);m.states[20-m.first_page]=State::quarantined;}
  if(mode==8)t.request.locator_uuid=Id(55003);
  if(mode==9)++t.request.directory_generation;
  if(mode==10)++t.request.page_zero_generation;
  if(mode==11)++t.request.map_generation;
  if(mode==12)t.request.allocation_sha256[0]^=1;
  if(mode==13)t.request.checkpoint_sha256[0]^=1;
  if(mode==14)t.graph.target.stable_local_transaction_id=0;
  if(mode==15){auto& z=t.base.page_zero_images.front();const auto decoded=d::DecodeFilespacePageZero(z.data(),z.size());Check(decoded.ok(),"original zero for exact mismatch");auto next=*decoded.record;++next.root_set_generation;const auto raw=d::EncodeFilespacePageZero(next);Check(raw.ok(),"canonical mismatched zero");z=*raw.bytes;}
  t.SetExtent();t.Refresh();reject(mode);
 }
 t.after=initial_after;t.directory=initial_directory;t.base=initial_base;t.request=initial_request;t.graph.target=initial_target;t.SetExtent();t.Refresh();
 if(profile!=2&&!target){
  // First publication into a secondary member may still use its immutable
  // original allocation root rather than an explicit directory descriptor.
  auto& member=*std::find_if(t.old_directory.records.begin(),t.old_directory.records.end(),[&](const auto& r){return r.bootstrap.filespace_uuid==t.other;});
  member.allocation_root.reset();const auto raw=DirectoryImageOracle(t.old_directory);Check(page::EncodeNativeFilespaceDirectory(t.old_directory).bytes==raw,"canonical implicit secondary root");
  t.base.directory_images={raw};auto& role=*std::find_if(t.graph.base.roots.begin(),t.graph.base.roots.end(),[](const auto& r){return r.role==3;});role.sha256=Sha(raw);
  t.base_cp=db::EncodeNativeCheckpointRoot(t.graph.base).bytes;t.graph.plan.base_checkpoint_sha256=Sha(t.base_cp);t.request.checkpoint_sha256=Sha(t.base_cp);t.SetExtent();t.Refresh();
  Check(t.Validate()==CE::none,"original secondary allocation root remains supported");
 }
 Check(t.fixture.Read(0,512)==original,"immutable transition validation performs no I/O effects");
}
void DirectoryDeltaFaults(unsigned profile){
 DirectoryTransition t(0,0,true,profile,true,true);
 allocations=0;counting=true;const auto baseline=t.Validate();counting=false;const auto sites=allocations;Check(baseline==CE::none,"directory delta allocation baseline");
 for(unsigned long at=0;at<=sites;++at){allocation_budget=at;const auto result=t.Validate();const auto remaining=allocation_budget;allocation_budget=-1;
   if(at==sites)Check(remaining>=0&&result==CE::none,"directory delta complete allocation boundary");
   else {if(remaining!=-1||result!=CE::resource_exhausted)std::cerr<<"directory allocation failure at="<<at<<" result="<<int(result)<<'\n';Check(remaining==-1&&result==CE::resource_exhausted,"directory delta required allocation failure preserved");}}
 hash_counting=true;hash_seen=0;const auto measured=t.Validate();hash_counting=false;const auto hashes=hash_seen;Check(measured==CE::none,"directory delta hash baseline");
 for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto result=t.Validate();
   Check(!hash_fault&&result==CE::hash_failure,"directory delta required digest failure preserved");}
 std::cout<<"directory delta profile="<<profile<<" allocation_sites="<<sites<<" hash_sites="<<hashes<<'\n';
}
void DirectoryPlan(unsigned primary,unsigned secondary,bool reverse){
 Fixture f(primary);Graph g(f);DirectoryBundle source(f,secondary,reverse);
 const auto original=f.Read(0,256);
 for(unsigned profile=2;profile<=4;++profile){
  auto p=g.plan;p.control_bundle=source.root;p.operation_uuid=source.root.operation_uuid;
  p.base_selection_generation=1;p.intent.recovery_profile=profile;
  auto& r=*p.control_bundle;
  // Only profile2 uses the actual inventory-bearing fixture bundle. Other
  // profiles exercise root/plan framing, not a claim of publication admission.
  if(profile!=2){r.inventory_count=0;r.payload_bytes-=f.size+d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes+16;}
  if(profile==4){r.growth_image_count=2;r.payload_bytes+=2*(u64{d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes}+8);}
  r.page_count=(r.payload_bytes+f.size-385)/(f.size-384);
  const auto raw=Oracle(p);const auto encoded=db::EncodeNativePublicationPlan(p);
  Check(encoded.ok()&&encoded.bytes==raw&&encoded.sha256==Sha(raw),"version8 exact independent bytes and full reference digest");
  const auto decoded=db::DecodeNativePublicationPlan(raw);
  Check(decoded.ok()&&decoded.plan->control_bundle==p.control_bundle&&decoded.plan->intent==p.intent&&
    decoded.plan->base_selection_generation==p.base_selection_generation&&db::EncodeNativePublicationPlan(*decoded.plan).bytes==raw,
    "version8 retains all mixed-profile/growth commitments");
  if(profile==2)StartupPlan(p);
  Check(db::ValidateNativeManagementControlAllocation(g.base_bytes,g.target_bytes,raw,g.extent,g.before_bytes,g.after_bytes,f.budget)==CE::invalid_plan,
    "primary-only validator cannot silently ignore a directory delta");
  const auto invalid=[&](const db::NativePublicationPlan& bad,unsigned field){const auto encoded_bad=db::EncodeNativePublicationPlan(bad);
   if(encoded_bad.ok())std::cerr<<"version8 invalid encoding profile="<<profile<<" field="<<field<<'\n';Failed(encoded_bad);
   auto image=Oracle(bad);std::copy_n("SBPPM008",8,image.begin()+128);Num(image,136,2,8);
   const auto& b=*bad.control_bundle;Num(image,1056,8,b.inventory_count);Num(image,1064,8,b.directory_count);
   Num(image,1072,8,b.payload_bytes);Num(image,1080,8,b.growth_image_count);PlanSeal(image);const auto decoded_bad=db::DecodeNativePublicationPlan(image);
   if(field==15){Check(decoded_bad.ok()&&decoded_bad.plan->control_bundle->operation_uuid==p.operation_uuid,
     "wire bundle operation is derived from the single committed plan identity");return;}
   if(decoded_bad.ok())std::cerr<<"version8 invalid decoding profile="<<profile<<" field="<<field<<'\n';Failed(decoded_bad);};
  for(unsigned field=0;field<17;++field){auto bad=p;auto& b=*bad.control_bundle;switch(field){
   case 0:bad.intent.recovery_profile=0;break;case 1:bad.intent.recovery_profile=1;break;
   case 2:bad.intent.recovery_profile=5;break;case 3:b.directory_count=0;break;case 4:b.payload_bytes=0;break;
   case 5:b.payload_bytes=std::numeric_limits<u64>::max();break;case 6:b.directory_count=std::numeric_limits<u64>::max();break;
   case 7:b.map_count=std::numeric_limits<u64>::max();break;case 8:b.inventory_count=profile==2?0:1;break;
   case 9:b.growth_image_count=profile==4?0:2;break;case 10:b.growth_image_count=1;break;
   case 11:++b.page_count;break;case 12:bad.base_selection_generation.reset();break;
   case 13:bad.base_selection_generation=std::numeric_limits<u64>::max();break;
   case 14:b.first.page_size_profile_uuid={};break;case 15:b.operation_uuid=Id(19000);break;
   case 16:b.first.page_number=std::numeric_limits<u64>::max();break;}invalid(bad,field);}
  for(unsigned at:{135u,136u,138u,140u,1050u,1055u,1088u,1151u,1152u}){auto bad=raw;bad[at]^=1;PlanSeal(bad);Failed(db::DecodeNativePublicationPlan(bad));}
  for(unsigned version=1;version<8;++version){auto bad=raw;bad[135]='0'+version;Num(bad,136,2,version);PlanSeal(bad);Failed(db::DecodeNativePublicationPlan(bad));}
  {auto bad=raw;Num(bad,1064,8,0);Num(bad,1072,8,0);Num(bad,1080,8,0);PlanSeal(bad);Failed(db::DecodeNativePublicationPlan(bad));}
  // Every committed field affects the full-image seal, even if another value
  // might be structurally admissible with a different authoritative bundle.
  for(unsigned at:{1056u,1063u,1064u,1071u,1072u,1079u,1080u,1087u}){auto bad=raw;bad[at]^=1;Check(db::DecodeNativePublicationPlan(bad).error==E::invalid_integrity,"extension field is sealed");}
  if(primary!=secondary)continue;
  for(unsigned route=0;route<2;++route){const auto call=[&]{return route?db::DecodeNativePublicationPlan(raw):db::EncodeNativePublicationPlan(p);};
   allocations=0;counting=true;const auto baseline=call();counting=false;const auto sites=allocations;Check(baseline.ok(),"version8 allocation baseline");
   for(unsigned long at=0;at<=sites;++at){allocation_budget=at;const auto result=call();const auto remaining=allocation_budget;allocation_budget=-1;
    if(at==sites)Check(remaining>=0&&result.ok(),"version8 complete allocation boundary");
    else{Check(remaining==-1&&result.error==E::resource_exhausted,"version8 consumed allocation failure");Failed(result);}}
   hash_counting=true;hash_seen=0;const auto measured=call();hash_counting=false;const auto hashes=hash_seen;Check(measured.ok(),"version8 hash baseline");
   for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;
    const auto result=call();Check(!hash_fault&&result.error==E::hash_failure,"version8 consumed hash failure");Failed(result);}
  }
 }
 Check(f.Read(0,256)==original,"plan validation never modifies the actual device");
}
void HistoricalResultPlacement(unsigned profile){
 Fixture f(profile);DirectoryBundle b(f,profile,false,0);
 auto& map=*std::find_if(b.maps.begin(),b.maps.end(),[](const auto& m){return m.header.filespace_uuid==Id(2);});
 map.total_pages=256+b.headers.size();map.states.resize(map.total_pages,State::free);
 for(std::size_t n=0;n<b.headers.size();++n){auto& h=b.headers[n];const auto at=std::find_if(map.records.begin(),map.records.end(),[&](const auto& r){return r.page_number==h.page_number;});
  Check(at!=map.records.end(),"original bundle allocation for placement negative");map.states[h.page_number]=State::free;h.page_number=256+n;
  at->page_number=h.page_number;map.states[h.page_number]=State::allocated;}
 std::sort(map.records.begin(),map.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});b.root.first=Self(b.headers.front());
 auto after=*d::DecodeFilespacePageZero(b.growth_images[1].data(),b.growth_images[1].size()).record;after.total_pages=map.total_pages;
 after.free_pages=std::count(map.states.begin(),map.states.end(),State::free);const auto image=d::EncodeFilespacePageZero(after);Check(image.ok(),"canonical misplaced growth result");b.growth_images[1]=*image.bytes;
 for(auto& dir:b.directories)for(auto& member:dir.records)if(member.bootstrap.filespace_uuid==Id(2))member.total_pages=after.total_pages;
 b.Refresh();Check(b.Encode().ok()&&b.Decode().ok(),"misplaced bundle remains a canonical image graph");b.Install();
 Check(f.device.WriteAt(0,image.bytes->data(),image.bytes->size()).ok()&&f.device.Sync().ok(),"actual misplaced result fixture");
 const auto before=f.Read(0,after.total_pages);const auto rejected=db::ReadNativeManagementControlBundleAtHistoricalResultFromOpenDevice(f.devices.front(),b.root,Id(1),b.zero.page_uuid,*image.bytes,b.Budget());
 Check(rejected.error==BE::invalid_extent,"result capacity cannot legitimize anchoring in its own growth suffix");Empty(rejected);Check(f.Read(0,after.total_pages)==before,"placement refusal has no effects");
}
void MixedDirectoryBundle(unsigned primary,unsigned secondary,bool reverse){
 if(primary==secondary&&!reverse)HistoricalResultPlacement(primary);
 DirectoryPlan(primary,secondary,reverse);
 GrowthBundle(primary,secondary,reverse,0);GrowthBundle(primary,secondary,reverse,1);
 Fixture f(primary);DirectoryBundle b(f,secondary,reverse);
 BoundedBundleChecks(b.pages,b.root,Id(1),b.zero.page_uuid,b.Budget());
 const auto original_zero=f.Read(0);
 const auto encoded=b.Encode();Check(encoded.ok()&&encoded.root==b.root&&encoded.pages==b.pages,"independent complete version3 framing bytes and full hashes");b.Good(b.Decode());const auto short_budget=b.Decode(b.Budget()-1);Check(short_budget.error==BE::resource_exhausted,"mixed bundle exact checked allowance");Empty(short_budget);b.Install();b.Good(b.Read());
 BundleDeviceMemoryChecks(f.devices.front(),b.root,b.zero.page_uuid,b.Budget(),b.pages,db::NativeManagementControlReadContext::current,{},f.path,primary==secondary&&!reverse);
 b.Good(db::ReadNativeManagementControlBundleAtHistoricalPageZeroFromOpenDevice(f.devices.front(),b.root,Id(1),b.zero.page_uuid,original_zero,b.Budget()));
 b.Good(db::ReadNativeManagementControlBundleAtHistoricalResultFromOpenDevice(f.devices.front(),b.root,Id(1),b.zero.page_uuid,original_zero,b.Budget()));
 Check(f.device.Close().ok()&&f.device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"mixed bundle actual read-only reopen");b.Good(b.Read());Check(f.device.Close().ok(),"mixed bundle close before independent process");
 const auto child=fork();Check(child>=0,"mixed bundle independent process");if(!child){d::FileDevice own;if(!own.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok())_exit(80);const d::NativeFilespaceDevice file{Id(2),b.zero.bootstrap.page_size_profile_uuid,&own};const auto result=db::ReadNativeManagementControlBundleFromOpenDevice(file,b.root,Id(1),b.zero.page_uuid,b.Budget());_exit(result.ok()&&result.directory_images==b.directory_images&&result.allocation_images==b.map_images&&result.inventory_images==b.inventory_images?0:81);}int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"mixed-size original bytes reconstructed in independent process");Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"mixed bundle parent reopen");
 if(primary||secondary||reverse)return;
 const auto saved_pages=b.pages;const auto saved_root=b.root;
 for(unsigned field:{135u,136u,156u,192u,200u,208u,224u,272u,276u,284u,352u,360u,383u}){b.pages=saved_pages;b.root=saved_root;b.pages.front()[field]^=1;b.Reseal();Empty(b.Decode());}b.pages=saved_pages;b.root=saved_root;
 for(unsigned field=0;field<5;++field){b.root=saved_root;if(field==0)b.root.directory_count=0;if(field==1)b.root.payload_bytes=0;if(field==2)b.root.payload_bytes=std::numeric_limits<u64>::max();if(field==3)b.root.map_count=std::numeric_limits<u64>::max();if(field==4)b.root.inventory_count=std::numeric_limits<u64>::max();Empty(b.Decode());}b.root=saved_root;
 for(u64 length:{u64{0},u64{1},f.size-1,f.size+1,std::numeric_limits<u64>::max()}){b.pages=saved_pages;b.root=saved_root;Num(b.pages.front(),384,8,length);b.ResealPayload();Empty(b.Decode());}b.pages=saved_pages;b.root=saved_root;
 const auto saved_maps=b.maps;
 for(unsigned field=0;field<9;++field){b.maps=saved_maps;auto& m=b.maps[0];auto& record=*std::find_if(m.records.begin(),m.records.end(),[](const auto& r){return r.page_number==220;});
  if(field==0)record.owner_uuid=Id(12000);if(field==1)record.creator_operation_uuid=Id(12000);if(field==2)record.page_uuid=Id(12000);if(field==3)++record.page_generation;if(field==4)record.page_type=3;if(field==5)m.states[220]=State::reserved;
  if(field==6)record.allocation_uuid=b.maps[1].records.front().allocation_uuid;if(field==7)record.page_uuid=b.maps[1].records.front().page_uuid;if(field==8)m.creator_operation_uuid=Id(12000);
  b.Refresh();Empty(b.Encode());Empty(b.Decode());}
 b.maps=saved_maps;b.Refresh();const auto saved_directories=b.directories;
 for(unsigned field=0;field<13;++field){b.directories=saved_directories;auto& dir=b.directories[1];auto& member=dir.records.front();auto& allocation=*member.allocation_root;
  if(field==0)dir.creator_operation_uuid=Id(12000);if(field==1)dir.object_uuid=Id(12000);if(field==2)++dir.directory_generation;
  if(field==3)allocation.sha256[0]^=1;if(field==4)++allocation.map_generation;if(field==5)++allocation.capacity_generation;if(field==6)allocation.object_uuid=Id(12000);if(field==7)++allocation.page.page_generation;
  if(field==8)member.allocation_root.reset();if(field==9)member.page_zero_uuid=b.headers.front().page_uuid;if(field==10)member.page_zero_uuid=b.directories[0].records.front().page_zero_uuid;
  if(field==11){dir.creator_operation_uuid={};dir.creator_transaction_uuid=Id(12000);dir.creator_local_transaction_id=1;}if(field==12)++member.total_pages;
  b.directory_images[1]=DirectoryImageOracle(dir);b.directories[0].next_sha256=Sha(b.directory_images[1]);b.directory_images[0]=DirectoryImageOracle(b.directories[0]);b.Oracle();Empty(b.Encode());Empty(b.Decode());}
 b.directories=saved_directories;b.Refresh();
 const auto saved_inventory=b.inventories;
 for(unsigned field=0;field<6;++field){b.inventories=saved_inventory;auto& p=b.inventories[1];if(field==0)p.object_uuid=Id(12000);if(field==1)p.previous.reset();if(field==2)++p.inventory_generation;if(field==3)++p.inventory.next_local_transaction_id;if(field==4)p.inventory.entries.front().identity.transaction_uuid=b.inventories[0].inventory.entries.front().identity.transaction_uuid;if(field==5)p.inventory.entries.front().identity.local_id=mga::MakeLocalTransactionId(1);
  b.inventory_images[1]=InventoryOracle(p);b.Oracle();Empty(b.Encode());Empty(b.Decode());}
 b.inventories=saved_inventory;b.Refresh();
 // Canonical but physically false primary membership must fail actual-device admission.
 const auto primary_record=b.directories[0].records.front();
 for(unsigned field=0;field<3;++field){auto& r=b.directories[0].records.front();r=primary_record;if(field==0)++r.page_zero_generation;if(field==1)++r.root_set_generation;if(field==2)r.page_zero_uuid=Id(12000);
  b.directory_images[0]=DirectoryImageOracle(b.directories[0]);b.Oracle();b.Good(b.Decode());b.Install();const auto rejected=b.Read();Check(rejected.error==BE::binding_mismatch,"actual primary bootstrap identity cannot be replaced by a canonical directory assertion");Empty(rejected);}
 b.directories=saved_directories;b.Refresh();b.Install();
 const auto original=f.Read(0,256);
 for(unsigned route=0;route<3;++route){const auto call=[&](){if(!route){const auto r=b.Encode();if(!r.ok())Empty(r);return r.error;}const auto r=route==1?b.Decode():b.Read();if(!r.ok())Empty(r);return r.error;};
  byte scratch=0;for(unsigned i=0;i<4097;++i)Check(f.device.ReadAt(0,&scratch,1).ok(),"mixed bundle optional telemetry warmup");
  counting=true;allocations=0;Check(call()==BE::none,"mixed bundle allocation baseline");counting=false;const auto sites=allocations;
  for(unsigned long at=0;at<=sites;++at){const auto loss=f.device.failed_io_latency_observations();allocation_budget=at;const auto result=call();const auto remaining=allocation_budget;allocation_budget=-1;
   if(at==sites)Check(result==BE::none&&remaining>=0,"mixed bundle uninjected allocation terminal");else Check(remaining<0&&(result==BE::resource_exhausted||(result==BE::none&&f.device.failed_io_latency_observations()==loss+1)),"all mixed bundle allocation failures consumed or explicit optional observation loss");}
  hash_counting=true;hash_seen=0;Check(call()==BE::none,"mixed bundle full hash baseline");hash_counting=false;const auto hashes=hash_seen;
  for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto result=call();Check(!hash_fault&&result==BE::hash_failure,"every mixed bundle digest failure is consumed and preserved");}
  std::cout<<"mixed bundle route="<<route<<" allocations="<<sites<<" hashes="<<hashes<<'\n';
 }
 reads=writes=syncs=0;io_counting=true;b.Good(b.Read());io_counting=false;const auto sites=reads;Check(!writes&&!syncs,"mixed bundle reader is mutation-free");
 for(unsigned at=1;at<=sites;++at){reads=0;read_fault=at;io_counting=true;const auto r=b.Read();io_counting=false;read_fault=0;Check(reads>=at&&r.error==BE::io_failure,"all actual mixed bundle read failures consumed");Empty(r);reads=0;corrupt_read=at;io_counting=true;const auto corrupt=b.Read();io_counting=false;corrupt_read=0;Empty(corrupt);}Check(f.Read(0,256)==original,"mixed bundle failures preserve every actual node byte");
 Graph g(f);Bundle old(g);g.plan.control_bundle=old.root;g.Refresh();for(unsigned field=0;field<3;++field){auto wrong=g.plan;if(field==0)wrong.control_bundle->directory_count=1;if(field==1)wrong.control_bundle->payload_bytes=1;if(field==2)wrong.control_bundle->growth_image_count=2;Failed(db::EncodeNativePublicationPlan(wrong));}
}
void InventoryHistory(Fixture& f,Graph& g,Bundle& b,unsigned profile){
 using HE=db::NativeManagementHistoryError;using AE=db::NativeManagementControlAuthorityError;
 const auto write=[&](u64 page,const Bytes& bytes){const auto r=f.device.WriteAt(page*f.size,bytes.data(),bytes.size());Check(r.ok()&&r.bytes_transferred==bytes.size(),"actual inventory history fixture write");};
 const auto install=[&](Graph& graph,Bundle& bundle){write(graph.plan.header.page_number,graph.plan_bytes);write(graph.plan.target_checkpoint.page_number,graph.target_bytes);for(unsigned i=0;i<graph.extent.size();++i)write(graph.plan.management_extent->first.page_number+i,graph.extent[i]);for(unsigned i=0;i<graph.after_bytes.size();++i)write(graph.after[i].header.page_number,graph.after_bytes[i]);bundle.Install();for(const auto& raw:bundle.inventory){const auto h=d::DecodeNativeCommonPageHeader(raw.data(),128);Check(h.ok(),"inventory fixture header");write(h.header->page_number,raw);}Check(f.device.Sync().ok(),"actual inventory graph fixture barrier");};
 const auto select=[&](Graph& graph){for(unsigned i=0;i<2;++i){const auto r=std::find_if(g.zero.roots.begin(),g.zero.roots.end(),[&](const auto& r){return r.kind==18+i;});const auto old=db::DecodeNativeCheckpointSelection(f.Read(r->page_number));Check(old.ok(),"original selector fixture");auto s=*old.selection;s.previous_selection_generation=s.selection_generation;s.previous_checkpoint=s.checkpoint;s.previous_checkpoint_object_uuid=s.checkpoint_object_uuid;s.previous_checkpoint_sha256=s.checkpoint_sha256;++s.selection_generation;s.publication_uuid=graph.plan.operation_uuid;s.checkpoint=graph.plan.target_checkpoint;s.checkpoint_object_uuid=graph.plan.target_checkpoint_object_uuid;s.checkpoint_sha256=Sha(graph.target_bytes);s.checkpoint_generation=graph.plan.reserved_generation;s.root_set_generation=graph.plan.target_root_set_generation;const auto raw=SelectorOracle(s);Check(db::EncodeNativeCheckpointSelection(s).bytes==raw,"independent inventory selector bytes");write(r->page_number,raw);}Check(f.device.Sync().ok(),"selected fixture barrier");};
 const auto history=[&](u64 budget=0){return db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),budget?budget:f.budget);};
 const auto authority=[&](u64 budget=0){return db::ReadNativeManagementControlAuthorityFromOpenDevices(Id(1),f.devices,Id(2),budget?budget:f.budget);};
 const auto empty_history=[&](const auto& r){Check(!r.ok()&&!r.anchor&&!r.selection&&r.entries.empty()&&r.latest.empty()&&r.idempotency.empty()&&!r.verified_image_bytes,"no partial inventory history");};
 const auto empty_authority=[&](const auto& r){Check(!r.ok()&&!r.anchor&&!r.selection&&r.allocations.empty()&&r.publications.empty()&&!r.verified_image_bytes,"no partial inventory original-allocation authority");};
 install(g,b);const db::NativeManagementCheckpointAnchor anchor{g.plan.target_checkpoint,g.plan.target_checkpoint_object_uuid,Sha(g.target_bytes),g.plan.reserved_generation,g.plan.target_root_set_generation,g.plan.timeline_uuid};
 const auto unselected=db::ReadNativeManagementControlGraphFromOpenDevices(Id(1),f.devices,Id(2),anchor,f.budget);Check(unselected.ok()&&unselected.publications.size()==1,"actual unselected inventory graph is a physical proof only");select(g);
 const auto h=history();Check(h.ok()&&h.entries.size()==1&&h.entries.front().control_inventory_images==b.inventory,"history retains all actual bundle inventory bytes");const auto a=authority();Check(a.ok()&&a.publications.size()==1&&a.allocations.contains({Id(2),221}),"actual inventory authority retains exact original allocations");
 Check(history(h.verified_image_bytes).ok()&&authority(a.verified_image_bytes).ok(),"exact measured history and authority image work");empty_history(history(h.verified_image_bytes-1));empty_authority(authority(a.verified_image_bytes-1));
 const auto selector_hash=[&](const std::array<byte,32>& sha){for(unsigned i=0;i<2;++i){const auto r=std::find_if(g.zero.roots.begin(),g.zero.roots.end(),[&](const auto& r){return r.kind==18+i;});auto s=*db::DecodeNativeCheckpointSelection(f.Read(r->page_number)).selection;s.checkpoint_sha256=sha;write(r->page_number,SelectorOracle(s));}Check(f.device.Sync().ok(),"rebound selector fixture barrier");};
 for(unsigned field=0;field<3;++field){auto wrong=g.target;auto& root=*std::find_if(wrong.roots.begin(),wrong.roots.end(),[](const auto& r){return r.role==1;});if(field==0)root=*std::find_if(g.base.roots.begin(),g.base.roots.end(),[](const auto& r){return r.role==1;});if(field==1)--wrong.selected_local_transaction_id;if(field==2)root.object_uuid=Id(998);auto p=g.plan;const auto raw=Finish(p,wrong);write(p.header.page_number,Oracle(p));write(p.target_checkpoint.page_number,raw);selector_hash(Sha(raw));empty_history(history());empty_authority(authority());}
 write(g.plan.header.page_number,g.plan_bytes);write(g.plan.target_checkpoint.page_number,g.target_bytes);selector_hash(Sha(g.target_bytes));Check(history().ok()&&authority().ok(),"restore exact inventory checkpoint head and counter");
 auto continuation=*page::DecodeNativeTransactionInventoryPage(b.inventory[1]).page;++continuation.inventory.entries.front().begin_unix_epoch_millis;const auto changed=InventoryOracle(continuation);Check(page::DecodeNativeTransactionInventoryPage(changed).ok(),"substituted continuation individually canonical");write(221,changed);Check(f.device.Sync().ok(),"changed continuation fixture sync");Check(history().ok(),"retained history does not claim installation authority");empty_authority(authority());write(221,b.inventory[1]);Check(f.device.Sync().ok()&&authority().ok(),"restore exact retained continuation");
 Check(f.device.Close().ok()&&f.device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"readonly inventory graph reopen");Check(authority().ok(),"readonly full inventory graph proof");Check(f.device.Close().ok(),"close before fresh inventory reader");const auto child=fork();Check(child>=0,"independent inventory graph process");if(!child){d::FileDevice owned;if(!owned.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok())_exit(80);std::vector<d::NativeFilespaceDevice> files{{Id(2),g.zero.bootstrap.page_size_profile_uuid,&owned}};const auto r=db::ReadNativeManagementControlAuthorityFromOpenDevices(Id(1),files,Id(2),f.budget);_exit(r.ok()&&r.publications.size()==1?0:81);}int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"cold independent inventory graph proof");Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"parent inventory reader reopen");
 if(profile)return;
 Graph later(f,false,1,&g);Bundle metadata(later,0,50);later.plan.control_bundle=metadata.root;later.plan.base_selection_generation=2;later.plan.intent.recovery_profile=1;later.Refresh();install(later,metadata);select(later);const auto mh=history();Check(mh.ok()&&mh.entries.size()==2&&mh.entries.front().control_inventory_images==b.inventory&&mh.entries.back().control_inventory_images.empty(),"metadata successor retains inventory-bearing ancestry");Check(authority().ok(),"actual metadata successor consumes retained inventory proof");write(221,changed);Check(f.device.Sync().ok(),"metadata continuation substitution");empty_authority(authority());write(221,b.inventory[1]);Check(f.device.Sync().ok()&&authority().ok(),"restore metadata ancestor continuation");
 replacement_bytes=changed.data();replacement_length=changed.size();replacement_offset=221*f.size;replacement_at=replacement_seen=0;Check(authority().ok(),"canonical continuation substitution baseline");const auto occurrences=replacement_seen;Check(occurrences==4,"inventory-bearing target plus metadata base target and final anchor reads");for(unsigned at=1;at<=occurrences;++at){replacement_seen=0;replacement_at=at;empty_authority(authority());Check(replacement_seen>=at,"targeted canonical substitution consumed");}replacement_bytes=nullptr;
 const auto original=f.Read(0,256);byte scratch=0;for(unsigned i=0;i<4097;++i)Check(f.device.ReadAt(0,&scratch,1).ok(),"stabilize optional metrics before failure sweeps");
 for(unsigned route=0;route<2;++route){if(inventory_allocation_route>=0&&route!=unsigned(inventory_allocation_route))continue;const auto call=[&](){if(!route){const auto r=history();if(!r.ok())empty_history(r);return int(r.error);}const auto r=authority();if(!r.ok())empty_authority(r);return int(r.error);};const int resource=route?int(AE::resource_exhausted):int(HE::resource_exhausted),hash=route?int(AE::hash_failure):int(HE::hash_failure),io=route?int(AE::io_failure):int(HE::io_failure);
  allocations=0;counting=true;Check(call()==0,"actual inventory reader allocation baseline");counting=false;const auto sites=allocations;
  if(inventory_allocation_route>=0){unsigned long consumed=0;for(unsigned long at=inventory_allocation_shard;at<sites;at+=4){const auto loss=f.device.failed_io_latency_observations();allocation_budget=at;const auto result=call();const auto left=allocation_budget;allocation_budget=-1;Check(left==-1&&(result==resource||(result==0&&f.device.failed_io_latency_observations()==loss+1)),"all actual inventory reader allocations consumed or optional metric loss observed");++consumed;}Check(f.Read(0,256)==original,"inventory allocation shard preserves whole node");std::cout<<"inventory allocation route="<<route<<" shard="<<inventory_allocation_shard<<" sites="<<sites<<" consumed="<<consumed<<'\n';continue;}
  hash_counting=true;hash_seen=0;Check(call()==0,"actual inventory reader hash baseline");hash_counting=false;const auto hashes=hash_seen;for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto result=call();Check(!hash_fault&&result==hash,"every actual inventory reader digest failure");}
  reads=writes=syncs=0;io_counting=true;Check(call()==0,"actual inventory reader I/O baseline");io_counting=false;const auto ios=reads;Check(!writes&&!syncs,"actual inventory history/authority never writes or syncs");for(unsigned at=1;at<=ios;++at){reads=0;read_fault=at;io_counting=true;const auto result=call();io_counting=false;read_fault=0;Check(result==io,"every actual inventory reader read failure");reads=0;corrupt_read=at;io_counting=true;const auto corrupted=call();io_counting=false;corrupt_read=0;Check(corrupted!=0,"every corrupted actual inventory reader image refuses");}Check(f.Read(0,256)==original,"all inventory reader fault paths preserve the whole node");std::cout<<"inventory reader route="<<route<<" allocations="<<sites<<" hashes="<<hashes<<" reads="<<ios<<'\n';
 }
}
void InventoryInstallFaults(Fixture& f,Graph& g,Bundle& b,db::NativePublicationReservation& held,const Bytes& expected,u64 budget){
 using PE=db::NativePublicationError;const auto initial_pending=held.lease->snapshot();Bytes initial_bytes;
 if(inventory_reconstruction_faults){initial_bytes=f.Read(0,256);Check(db::InstallNativeManagementControlGraphOnLease(*held.lease,g.plan,g.target_bytes,g.extent,b.pages,budget).ok(),"persist complete original reconstruction input");Bytes zero(f.size);const auto erase=[&](u64 slot){const auto r=f.device.WriteAt(slot*f.size,zero.data(),zero.size());Check(r.ok()&&r.bytes_transferred==zero.size(),"missing unselected target fixture");};for(const auto& raw:b.inventory){const auto header=d::DecodeNativeCommonPageHeader(raw.data(),128);Check(header.ok(),"reconstruction target identity");erase(header.header->page_number);}for(const auto& map:g.after)erase(map.header.page_number);erase(g.plan.target_checkpoint.page_number);Check(f.device.Sync().ok(),"persist interrupted inventory target fixture");}
 const auto pending=held.lease->snapshot();const auto before=f.Read(0,256);
 const auto reset=[&]{held.lease.reset();const auto write=f.device.WriteAt(0,before.data(),before.size());Check(write.ok()&&write.bytes_transferred==before.size()&&f.device.Sync().ok(),"restore isolated reserved inventory fault fixture");held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),pending,g.plan.operation_uuid,g.plan.intent,budget);Check(held.ok(),"actual exact inventory fault lease resume");};
 const auto limited=[&](u64 limit){return inventory_reconstruction_faults?db::ResumeNativeManagementControlGraphOnLease(*held.lease,limit):db::InstallNativeManagementControlGraphOnLease(*held.lease,g.plan,g.target_bytes,g.extent,b.pages,limit);};const auto call=[&]{return limited(budget);};
 const auto failed=[&](const auto& r){Check(!r.ok()&&!r.snapshot,"failed inventory installation returns no snapshot");for(const auto& root:g.zero.roots)if(root.kind==18||root.kind==19){const auto raw=f.Read(root.page_number);Check(std::equal(raw.begin(),raw.end(),before.begin()+root.page_number*f.size),"every failed installation leaves selectors unchanged");}};
 byte scratch=0;for(unsigned n=0;n<4097;++n)Check(f.device.ReadAt(0,&scratch,1).ok(),"stabilize inventory installer metrics");
 if(inventory_install_route==0){reads=writes=syncs=0;io_counting=true;const auto good=call();io_counting=false;const auto nr=reads,nw=writes,ns=syncs;Check(good.ok()&&nr&&nw&&ns&&f.Read(0,256)==expected,"complete actual inventory installer I/O baseline");
  for(unsigned kind=0;kind<5;++kind){const unsigned count=kind==0||kind==3?nr:kind==2?ns:nw;for(unsigned at=1;at<=count;++at){reset();reads=writes=syncs=0;read_fault=kind==0?at:0;write_fault=kind==1||kind==4?at:0;sync_fault=kind==2?at:0;corrupt_read=kind==3?at:0;torn_bytes=kind==4?f.size/2:0;io_counting=true;const auto r=call();io_counting=false;const bool consumed=kind==0||kind==3?reads>=at:kind==2?syncs>=at:writes>=at;const bool mutation=writes||syncs;read_fault=write_fault=sync_fault=corrupt_read=0;torn_bytes=0;Check(consumed,"every inventory installation I/O fault consumed");failed(r);if(kind!=3)Check(r.error==PE::io_failure,"inventory installation preserves backend I/O failure");if(mutation)Check(call().error==PE::stale_base,"ambiguous inventory I/O poisons retained lease");}}
  std::cout<<"inventory installer stage="<<inventory_install_stage<<" reads="<<nr<<" writes="<<nw<<" syncs="<<ns<<'\n';
  u64 low=0,high=budget;while(low<high){reset();const auto middle=low+(high-low)/2;const auto result=limited(middle);if(result.ok())high=middle;else{Check(result.error==PE::resource_exhausted&&!result.snapshot&&f.Read(0,256)==before,"insufficient inventory allowance refuses before any durable mutation");low=middle+1;}}Check(low>0,"nonzero complete inventory installation allowance");reset();Check(limited(low).ok()&&f.Read(0,256)==expected,"exact minimal complete inventory work allowance");reset();const auto short_work=limited(low-1);Check(short_work.error==PE::resource_exhausted&&!short_work.snapshot&&f.Read(0,256)==before,"one-byte-short inventory work refuses unchanged");
  for(const auto& raw:b.inventory){const auto header=d::DecodeNativeCommonPageHeader(raw.data(),128);Check(header.ok(),"preimage test inventory identity");const auto slot=header.header->page_number;Bytes occupied(f.size);occupied.back()=0x5a;if(!inventory_reconstruction_faults){reset();auto w=f.device.WriteAt(slot*f.size,occupied.data(),occupied.size());Check(w.ok()&&w.bytes_transferred==occupied.size()&&f.device.Sync().ok(),"occupied inventory destination fixture");const auto occupied_file=f.Read(0,256);reads=writes=syncs=0;io_counting=true;const auto denied=call();io_counting=false;Check(denied.error==PE::preimage_changed&&!writes&&!syncs&&f.Read(0,256)==occupied_file,"unanchored occupied inventory destination never overwritten");failed(denied);}
   reset();race_bytes=occupied.data();race_length=occupied.size();race_offset=slot*f.size;race_seen=0;const auto raced=call();race_bytes=nullptr;Check(race_seen==2&&raced.error==PE::preimage_changed&&f.Read(slot)==occupied,"actual inventory preimage change between snapshot and write preserved");failed(raced);Check(call().error==PE::stale_base,"inventory preimage race poisons ambiguous lease");
  }
 }else if(inventory_install_route==1){hash_counting=true;hash_seen=0;const auto good=call();hash_counting=false;const auto sites=hash_seen;Check(good.ok()&&sites,"inventory installer hash baseline");for(unsigned mode=inventory_install_shard+1;mode<=unsigned(inventory_install_shard+1);++mode)for(unsigned at=1;at<=sites;++at){reset();hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto r=call();const bool consumed=!hash_fault;hash_fault=0;Check(consumed&&r.error==PE::hash_failure,"every inventory installation digest fault consumed");failed(r);}std::cout<<"inventory installer stage="<<inventory_install_stage<<" hashes="<<sites<<'\n';
 }else if(inventory_install_route==2){allocations=0;counting=true;const auto good=call();counting=false;const auto sites=allocations;Check(good.ok()&&sites,"inventory installer allocation baseline");const unsigned shards=inventory_reconstruction_faults?(inventory_install_stage?64:16):(inventory_install_stage?32:8);std::cout<<"inventory installer stage="<<inventory_install_stage<<" allocation_sites="<<sites<<" shards="<<shards<<'\n';reset();unsigned long consumed=0;for(unsigned long at=inventory_install_shard;at<sites;at+=shards){const auto loss=f.device.failed_io_latency_observations();reads=writes=syncs=0;io_counting=true;allocation_budget=at;const auto r=call();io_counting=false;const auto left=allocation_budget;allocation_budget=-1;Check(left==-1&&(r.error==PE::resource_exhausted||(r.ok()&&f.device.failed_io_latency_observations()==loss+1)),"every inventory installation allocation consumed or metric loss observed");if(!r.ok()){Check(!writes&&!syncs&&f.Read(0,256)==before,"required allocation failure precedes all durable mutation");failed(r);}else{Check(f.Read(0,256)==expected,"optional metric failure never truncates installed inventory");reset();}++consumed;}std::cout<<"inventory installer stage="<<inventory_install_stage<<" allocation_sites="<<sites<<" shard="<<inventory_install_shard<<" consumed="<<consumed<<'\n';
 }else{
  reads=writes=syncs=0;io_counting=true;const auto good=call();io_counting=false;const auto nw=writes,ns=syncs;Check(good.ok()&&nw&&ns,"inventory process-loss baseline");unsigned recovered=0,refused=0;
  for(unsigned kind=0;kind<4;++kind)for(unsigned at=1;at<=(kind<2?nw:ns);++at){
   reset();held.lease.reset();Check(f.device.Close().ok(),"close parent device before installer process loss");const auto child=fork();Check(child>=0,"fork interrupted inventory installer");
   if(!child){d::FileDevice file;if(!file.Open(f.path.string(),d::FileOpenMode::open_existing).ok())_exit(80);std::vector<d::NativeFilespaceDevice> files{{Id(2),g.zero.bootstrap.page_size_profile_uuid,&file}};auto r=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),pending,g.plan.operation_uuid,g.plan.intent,budget);if(!r.ok())_exit(81);reads=writes=syncs=0;kill_write=kind<2?at:0;kill_sync=kind>=2?at:0;kill_after_sync=kind==3;torn_bytes=kind==1?f.size/2:0;io_counting=true;(void)db::InstallNativeManagementControlGraphOnLease(*r.lease,g.plan,g.target_bytes,g.extent,b.pages,budget);_exit(87);}
   int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"installer process dies at each exact write or barrier boundary");Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"inspect interrupted inventory file");const auto interrupted=f.Read(0,256);
   const auto exact=[&](u64 page){return std::equal(interrupted.begin()+page*f.size,interrupted.begin()+(page+1)*f.size,expected.begin()+page*f.size);};bool complete=exact(g.plan.header.page_number);for(const auto& root:g.zero.roots)if(root.kind==20||root.kind==21)complete=complete&&exact(root.page_number);for(unsigned i=0;i<g.extent.size();++i)complete=complete&&exact(g.plan.management_extent->first.page_number+i);for(unsigned i=0;i<b.pages.size();++i)complete=complete&&exact(b.root.first.page_number+i);
   for(const auto& root:g.zero.roots)if(root.kind==18||root.kind==19)Check(std::equal(interrupted.begin()+root.page_number*f.size,interrupted.begin()+(root.page_number+1)*f.size,before.begin()+root.page_number*f.size),"process loss never publishes inventory selectors");Check(f.device.Close().ok(),"close before independent inventory recovery process");const auto recovery=fork();Check(recovery>=0,"fork independent inventory recovery");
   if(!recovery){d::FileDevice file;if(!file.Open(f.path.string(),d::FileOpenMode::open_existing).ok())_exit(80);std::vector<d::NativeFilespaceDevice> files{{Id(2),g.zero.bootstrap.page_size_profile_uuid,&file}};reads=writes=syncs=0;io_counting=true;const auto observed=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),budget);bool ok=false;if(observed.ok()){auto r=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),*observed.snapshot,g.plan.operation_uuid,g.plan.intent,budget);if(r.ok()){reads=writes=syncs=0;const auto result=db::ResumeNativeManagementControlGraphOnLease(*r.lease,budget);ok=result.ok();if(!ok&&result.snapshot)_exit(84);}else reads=writes=syncs=0;}io_counting=false;_exit(complete?(ok?0:82):(!ok&&!writes&&!syncs?0:83));}
   const auto waited=waitpid(recovery,&status,0);if(waited!=recovery||!WIFEXITED(status)||WEXITSTATUS(status)!=0)std::cerr<<"inventory death kind="<<kind<<" at="<<at<<" complete="<<complete<<" status="<<status<<'\n';Check(waited==recovery&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"cold recovery uses only complete durable original inputs");Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"reopen after recovery process");const auto final=f.Read(0,256);if(complete)Check(final==expected,"cold recovery exact full-file oracle");else{auto anchored=interrupted,unanchored=interrupted;for(const auto& root:g.zero.roots)if(root.kind==20||root.kind==21){const auto offset=root.page_number*f.size;std::copy_n(expected.begin()+offset,f.size,anchored.begin()+offset);std::copy_n(before.begin()+offset,f.size,unanchored.begin()+offset);}Check(final==interrupted||final==anchored||final==unanchored,"incomplete reconstruction preserves all bytes except canonical lease watermark repair");}if(complete)++recovered;else{
    const auto repaired=db::RecoverNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),budget);Check(repaired.ok(),"repair pending watermark before explicit incomplete-input resolution");const auto pending_file=f.Read(0,256);auto resolved_file=pending_file;const auto resolution=Id(15500);for(const auto& root:g.zero.roots)if(root.kind==20||root.kind==21){const auto offset=root.page_number*f.size;Bytes raw(pending_file.begin()+offset,pending_file.begin()+offset+f.size);Num(raw,646,2,2);Put(raw,648,resolution);std::copy(repaired.snapshot->state_sha256.begin(),repaired.snapshot->state_sha256.end(),raw.begin()+664);Bytes state{'S','B','P','A','G','S','T','4'};state.insert(state.end(),raw.begin()+128,raw.begin()+368);state.insert(state.end(),raw.begin()+432,raw.begin()+768);const auto state_hash=Sha(state);std::copy(state_hash.begin(),state_hash.end(),raw.begin()+368);std::fill(raw.begin()+400,raw.begin()+432,0);const auto image_hash=Sha(raw);std::copy(image_hash.begin(),image_hash.end(),raw.begin()+400);std::copy(raw.begin(),raw.end(),resolved_file.begin()+offset);}
    Check(f.device.Close().ok(),"close parent before independent incomplete-candidate resolution");const auto resolver=fork();Check(resolver>=0,"fork independent incomplete-input resolver");if(!resolver){d::FileDevice file;if(!file.Open(f.path.string(),d::FileOpenMode::open_existing).ok())_exit(80);std::vector<d::NativeFilespaceDevice> files{{Id(2),g.zero.bootstrap.page_size_profile_uuid,&file}};const auto seen=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),budget);if(!seen.ok())_exit(81);const auto result=db::AbandonNativeInventoryPublicationOnOpenDevices(Id(1),files,Id(2),*seen.snapshot,g.plan.operation_uuid,g.plan.intent,resolution,budget);_exit(result.ok()&&result.snapshot->selection.checkpoint_sha256==pending.selection.checkpoint_sha256?0:82);}Check(waitpid(resolver,&status,0)==resolver&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"every incomplete installation boundary has an actual fresh-process resolution");Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok()&&f.Read(0,256)==resolved_file,"incomplete-input resolution exact bytes preserve selected inventory and orphan pages");++refused;
   }
  }
  Check(recovered&&refused,"both recoverable and incomplete process-loss boundaries exercised");std::cout<<"inventory installer stage="<<inventory_install_stage<<" process_loss_recovered="<<recovered<<" incomplete_refused_and_resolved="<<refused<<'\n';
 }
 reset();Check(f.Read(0,256)==before,"fault suite restores exact pending fixture for final installation");
 if(inventory_reconstruction_faults){held.lease.reset();const auto r=f.device.WriteAt(0,initial_bytes.data(),initial_bytes.size());Check(r.ok()&&r.bytes_transferred==initial_bytes.size()&&f.device.Sync().ok(),"restore isolated unanchored fixture after cold faults");held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),initial_pending,g.plan.operation_uuid,g.plan.intent,budget);Check(held.ok(),"resume original exact lease after cold fault suite");}
}
void InventoryResolutionFaults(Fixture& f,Graph& g,const db::NativePublicationSnapshot& pending,const Uuid& resolution,const Bytes& before,const Bytes& expected,u64 budget){
 using PE=db::NativePublicationError;
 const auto reset=[&]{const auto w=f.device.WriteAt(0,before.data(),before.size());Check(w.ok()&&w.bytes_transferred==before.size()&&f.device.Sync().ok(),"restore isolated pending inventory resolution fault fixture");};
 const auto call=[&]{return db::AbandonNativeInventoryPublicationOnOpenDevices(Id(1),f.devices,Id(2),pending,g.plan.operation_uuid,g.plan.intent,resolution,budget);};
 const auto unchanged=[&]{auto actual=f.Read(0,256);for(const auto& root:g.zero.roots)if(root.kind==20||root.kind==21)std::copy_n(before.begin()+root.page_number*f.size,f.size,actual.begin()+root.page_number*f.size);Check(actual==before,"inventory resolution failure preserves all non-watermark bytes");};
 const auto failed=[&](const auto& r){Check(!r.ok()&&!r.snapshot,"failed inventory resolution returns no success snapshot");unchanged();};
 byte scratch=0;for(unsigned i=0;i<4097;++i)Check(f.device.ReadAt(0,&scratch,1).ok(),"warm resolution telemetry without changing durable bytes");
 if(inventory_resolution_route==0){reads=writes=syncs=0;io_counting=true;const auto good=call();io_counting=false;const auto nr=reads,nw=writes,ns=syncs;Check(good.ok()&&nw==2&&ns==3&&f.Read(0,256)==expected,"actual resolution I/O baseline and full-file oracle");
  for(unsigned kind=0;kind<5;++kind)for(unsigned at=1;at<=(kind==0||kind==3?nr:kind==2?ns:nw);++at){reset();reads=writes=syncs=0;read_fault=kind==0?at:0;write_fault=kind==1||kind==4?at:0;sync_fault=kind==2?at:0;corrupt_read=kind==3?at:0;torn_bytes=kind==4?f.size/2:0;io_counting=true;const auto r=call();io_counting=false;const bool consumed=kind==0||kind==3?reads>=at:kind==2?syncs>=at:writes>=at;read_fault=write_fault=sync_fault=corrupt_read=0;torn_bytes=0;Check(consumed,"every inventory resolution read write sync and corruption fault consumed");failed(r);if(kind!=3)Check(r.error==PE::io_failure,"resolution preserves backend I/O failure");}
  std::cout<<"inventory resolution stage="<<inventory_resolution_stage<<" reads="<<nr<<" writes="<<nw<<" syncs="<<ns<<'\n';
 }else if(inventory_resolution_route==1){hash_counting=true;hash_seen=0;const auto good=call();hash_counting=false;const auto sites=hash_seen;Check(good.ok()&&sites,"inventory resolution hash baseline");for(unsigned mode=inventory_resolution_shard+1;mode<=unsigned(inventory_resolution_shard+1);++mode)for(unsigned at=1;at<=sites;++at){reset();hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto r=call();const bool consumed=!hash_fault;hash_fault=0;Check(consumed&&r.error==PE::hash_failure,"every inventory resolution digest fault consumed");failed(r);}std::cout<<"inventory resolution stage="<<inventory_resolution_stage<<" hashes="<<sites<<'\n';
 }else if(inventory_resolution_route==2){counting=true;allocations=0;const auto good=call();counting=false;const auto sites=allocations;Check(good.ok()&&sites,"inventory resolution allocation baseline");const unsigned shards=inventory_resolution_stage?32:8;std::cout<<"inventory resolution stage="<<inventory_resolution_stage<<" allocation_sites="<<sites<<" shards="<<shards<<'\n';bool restore=true;unsigned long consumed=0;for(unsigned long at=inventory_resolution_shard;at<sites;at+=shards){if(restore)reset();const auto lost=f.device.failed_io_latency_observations();reads=writes=syncs=0;io_counting=true;allocation_budget=at;const auto r=call();const auto left=allocation_budget;allocation_budget=-1;io_counting=false;restore=writes||syncs;Check(left==-1,"every inventory resolution allocation fault consumed");if(r.ok()){Check(f.device.failed_io_latency_observations()==lost+1&&f.Read(0,256)==expected,"optional telemetry loss requires complete durable resolution");restore=true;}else{Check(r.error==PE::resource_exhausted,"resolution preserves required allocation failure even after durable mutation");failed(r);if(!restore)Check(f.Read(0,256)==before,"reuse pending fixture only after byte-preserving allocation failure");}++consumed;}std::cout<<"inventory resolution stage="<<inventory_resolution_stage<<" allocation_sites="<<sites<<" shard="<<inventory_resolution_shard<<" consumed="<<consumed<<'\n';
 }
 if(inventory_resolution_route==3){
  for(unsigned kind=0;kind<4;++kind)for(unsigned at=1;at<=(kind<2?2u:3u);++at){reset();Check(f.device.Close().ok(),"close parent before inventory resolution interruption");const auto child=fork();Check(child>=0,"fork interrupted inventory resolution");if(!child){d::FileDevice file;if(!file.Open(f.path.string(),d::FileOpenMode::open_existing).ok())_exit(80);std::vector<d::NativeFilespaceDevice> files{{Id(2),g.zero.bootstrap.page_size_profile_uuid,&file}};reads=writes=syncs=0;kill_write=kind<2?at:0;kill_sync=kind>=2?at:0;kill_after_sync=kind==3;torn_bytes=kind==1?f.size/2:0;io_counting=true;(void)db::AbandonNativeInventoryPublicationOnOpenDevices(Id(1),files,Id(2),pending,g.plan.operation_uuid,g.plan.intent,resolution,budget);_exit(87);}int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"inventory resolution process dies at requested write or barrier");Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"reopen interrupted resolution for byte oracle");const auto interrupted=f.Read(0,256);const auto first=std::find_if(g.zero.roots.begin(),g.zero.roots.end(),[](const auto& r){return r.kind==20;});Check(first!=g.zero.roots.end(),"original first resolution watermark slot");const auto offset=first->page_number*f.size;const bool keep_resolution=std::equal(interrupted.begin()+offset,interrupted.begin()+offset+f.size,expected.begin()+offset);Check(f.device.Close().ok(),"close parent before fresh duplex recovery");
   const auto recovery=fork();Check(recovery>=0,"fork inventory resolution duplex recovery");if(!recovery){d::FileDevice file;if(!file.Open(f.path.string(),d::FileOpenMode::open_existing).ok())_exit(80);std::vector<d::NativeFilespaceDevice> files{{Id(2),g.zero.bootstrap.page_size_profile_uuid,&file}};const auto result=db::RecoverNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),budget);_exit(result.ok()&&bool(result.snapshot->watermark.abandonment)==keep_resolution&&result.snapshot->selection.checkpoint_sha256==pending.selection.checkpoint_sha256?0:81);}Check(waitpid(recovery,&status,0)==recovery&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"bound survivor controls cold inventory resolution repair");Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok()&&f.Read(0,256)==(keep_resolution?expected:before),"fresh recovery exact full-file pending-or-resolved oracle");const auto final=call();Check(final.ok()&&f.Read(0,256)==expected,"original exact inventory resolution request completes after process loss");
  }
 }
 if(inventory_resolution_route==4){
  reset();auto guard=f.device.AcquireOperationGuard();std::promise<void> entered;auto entry=entered.get_future();auto work=std::async(std::launch::async,[&]{entered.set_value();return call();});entry.get();const bool blocked=work.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout;guard.unlock();const auto result=work.get();Check(blocked&&result.ok()&&f.Read(0,256)==expected,"inventory resolution waits for owning device guard then publishes exact bytes");
  reset();std::promise<void> release;auto ready=release.get_future().share();const auto compete=[&](const Uuid& id){ready.wait();return db::AbandonNativeInventoryPublicationOnOpenDevices(Id(1),f.devices,Id(2),pending,g.plan.operation_uuid,g.plan.intent,id,budget);};auto first=std::async(std::launch::async,compete,resolution);auto second=std::async(std::launch::async,compete,Id(14500));release.set_value();const auto left=first.get(),right=second.get();Check(left.ok()!=right.ok(),"concurrent distinct inventory resolutions have exactly one winner");const auto& winner=left.ok()?left:right;const auto& loser=left.ok()?right:left;Check(loser.error==PE::request_mismatch&&!loser.snapshot,"concurrent loser receives no substitute success");const auto actual=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),budget);Check(actual.ok()&&actual.snapshot->state_sha256==winner.snapshot->state_sha256&&actual.snapshot->watermark.abandonment->resolution_uuid==(left.ok()?resolution:Id(14500)),"ordinary admission matches exact concurrent resolution winner");unchanged();
 }
 reset();Check(f.Read(0,256)==before,"resolution fault suite restores exact pending state");
}
void InventoryResolutionRequestChecks(Fixture& f,const Graph& g,const db::NativePublicationSnapshot& pending,
    const Uuid& resolution,const Bytes& expected,u64 budget){
 using PE=db::NativePublicationError;
 // Exercise each logical CAS component against actual files, both before
 // resolution and on exact-resolution replay. The durable state never changes.
 for(unsigned field=0;field<17;++field){auto stale=pending;auto& selected=stale.selection;
  switch(field){
   case 0:stale.state_sha256[0]^=1;break;
   case 1:++stale.watermark.watermark;break;
   case 2:stale.watermark.header.database_uuid=Id(17001);break;
   case 3:stale.watermark.header.filespace_uuid=Id(17002);break;
   case 4:++selected.selection_generation;break;
   case 5:selected.publication_uuid=Id(17003);break;
   case 6:selected.bootstrap_uuid=Id(17004);break;
   case 7:selected.object_uuid=Id(17005);break;
   case 8:selected.checkpoint.filespace_uuid=Id(17006);break;
   case 9:++selected.checkpoint.page_number;break;
   case 10:++selected.checkpoint.page_generation;break;
   case 11:selected.checkpoint.page_size_profile_uuid=Id(17007);break;
   case 12:selected.checkpoint_object_uuid=Id(17008);break;
   case 13:selected.checkpoint_sha256[0]^=1;break;
   case 14:++selected.checkpoint_generation;break;
   case 15:++selected.root_set_generation;break;
   case 16:selected.timeline_uuid=Id(17009);break;
  }
  reads=writes=syncs=0;io_counting=true;
  const auto result=db::AbandonNativeInventoryPublicationOnOpenDevices(Id(1),f.devices,Id(2),stale,g.plan.operation_uuid,g.plan.intent,resolution,budget);
  io_counting=false;
  Check(result.error==PE::stale_base&&!result.snapshot&&!writes&&!syncs&&f.Read(0,256)==expected,"each inventory resolution CAS mismatch refuses without mutation or barriers");
 }
 for(unsigned field=0;field<9;++field){auto request=pending;auto operation=g.plan.operation_uuid;auto intent=g.plan.intent;auto resolve=resolution;auto wanted=PE::invalid_request;
  switch(field){
   case 0:operation={};break;
   case 1:operation=Id(17101);break;
   case 2:resolve={};break;
   case 3:resolve=operation;break;
   case 4:intent.recovery_profile=1;break;
   case 5:request.watermark.intent.reset();break;
   case 6:request.watermark.abandonment=db::NativePublicationWatermark::Abandonment{resolution,pending.state_sha256};break;
   case 7:intent.normalized_request_sha256[0]^=1;request.watermark.intent=intent;wanted=PE::request_mismatch;break;
   case 8:operation=Id(17102);request.watermark.operation_uuid=operation;wanted=PE::request_mismatch;break;
  }
  reads=writes=syncs=0;io_counting=true;
  const auto result=db::AbandonNativeInventoryPublicationOnOpenDevices(Id(1),f.devices,Id(2),request,operation,intent,resolve,budget);
  io_counting=false;
  Check(result.error==wanted&&!result.snapshot&&!writes&&!syncs&&f.Read(0,256)==expected,"inventory resolution request identity and profile mismatches have no mutation or barriers");
 }
}
void InventoryResolution(Fixture& f,Graph& g,Bundle& b,db::NativePublicationReservation& held,u64 budget){
 using PE=db::NativePublicationError;const auto original=f.Read(0,256);const auto original_pending=held.lease->snapshot();
 const auto restore=[&]{held.lease.reset();const auto r=f.device.WriteAt(0,original.data(),original.size());Check(r.ok()&&r.bytes_transferred==original.size()&&f.device.Sync().ok(),"restore isolated pending resolution fixture");held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),original_pending,g.plan.operation_uuid,g.plan.intent,budget);Check(held.ok(),"resume original resolution fixture lease");};
 for(unsigned phase=0;phase<3;++phase){restore();if(phase){Check(db::InstallNativeManagementControlGraphOnLease(*held.lease,g.plan,g.target_bytes,g.extent,b.pages,budget).ok(),"persist exact candidate before inventory resolution");if(phase==2){Bytes zero(f.size);const auto r=f.device.WriteAt(b.root.first.page_number*f.size,zero.data(),zero.size());Check(r.ok()&&r.bytes_transferred==zero.size()&&f.device.Sync().ok(),"incomplete reconstruction input fixture");}}
  const auto pending=held.lease->snapshot();const auto before=f.Read(0,256);auto expected=before;const auto resolution=Id(13000+phase);for(const auto& root:g.zero.roots)if(root.kind==20||root.kind==21){const auto offset=root.page_number*f.size;Bytes raw(before.begin()+offset,before.begin()+offset+f.size);Num(raw,646,2,2);Put(raw,648,resolution);std::copy(pending.state_sha256.begin(),pending.state_sha256.end(),raw.begin()+664);Bytes state{'S','B','P','A','G','S','T','4'};state.insert(state.end(),raw.begin()+128,raw.begin()+368);state.insert(state.end(),raw.begin()+432,raw.begin()+768);const auto digest=Sha(state);std::copy(digest.begin(),digest.end(),raw.begin()+368);std::fill(raw.begin()+400,raw.begin()+432,0);const auto seal=Sha(raw);std::copy(seal.begin(),seal.end(),raw.begin()+400);std::copy(raw.begin(),raw.end(),expected.begin()+offset);}
  const auto wrong=db::AbandonNativeMetadataPublicationOnOpenDevices(Id(1),f.devices,Id(2),pending,g.plan.operation_uuid,g.plan.intent,resolution,budget);Check(wrong.error==PE::invalid_request&&!wrong.snapshot&&f.Read(0,256)==before,"metadata API cannot resolve inventory profile");
  if(phase==2&&inventory_resolution_stage>=0&&g.plan.reserved_generation==u64(inventory_resolution_stage+2)){held.lease.reset();InventoryResolutionFaults(f,g,pending,resolution,before,expected,budget);held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),pending,g.plan.operation_uuid,g.plan.intent,budget);Check(held.ok(),"resume pending lease after inventory resolution fault grid");}
  if(inventory_resolution_requests)InventoryResolutionRequestChecks(f,g,pending,resolution,before,budget);
  const auto resolved=db::AbandonNativeInventoryPublicationOnOpenDevices(Id(1),f.devices,Id(2),pending,g.plan.operation_uuid,g.plan.intent,resolution,budget);Check(resolved.ok()&&resolved.snapshot->watermark.abandonment&&resolved.snapshot->watermark.abandonment->resolution_uuid==resolution&&resolved.snapshot->selection.checkpoint_sha256==pending.selection.checkpoint_sha256&&f.Read(0,256)==expected,"inventory resolution exact full-file oracle preserves selected starting state");
  if(inventory_resolution_requests)InventoryResolutionRequestChecks(f,g,pending,resolution,expected,budget);
  const auto stale=db::InstallNativeManagementControlGraphOnLease(*held.lease,g.plan,g.target_bytes,g.extent,b.pages,budget);Check(!stale.ok()&&!stale.snapshot&&f.Read(0,256)==expected,"resolved inventory attempt invalidates retained installation lease");held.lease.reset();
  reads=writes=syncs=0;io_counting=true;const auto retry=db::AbandonNativeInventoryPublicationOnOpenDevices(Id(1),f.devices,Id(2),pending,g.plan.operation_uuid,g.plan.intent,resolution,budget);io_counting=false;Check(retry.ok()&&!writes&&syncs==3&&f.Read(0,256)==expected,"same inventory resolution retry repeats barriers without rewriting bytes");const auto conflict=db::AbandonNativeInventoryPublicationOnOpenDevices(Id(1),f.devices,Id(2),pending,g.plan.operation_uuid,g.plan.intent,Id(14000),budget);Check(!conflict.ok()&&!conflict.snapshot&&f.Read(0,256)==expected,"different resolution identity cannot replace durable disposition");
  const auto cold=db::RecoverNativeManagementCheckpointPublicationOnOpenDevices(Id(1),f.devices,Id(2),g.plan.operation_uuid,g.plan.intent,budget);Check(!cold.ok()&&!cold.snapshot&&f.Read(0,256)==expected,"cold selection never publishes abandoned inventory candidate");
  Check(f.device.Close().ok(),"close resolved inventory node before independent admission");const auto child=fork();Check(child>=0,"fork resolved inventory admission");if(!child){d::FileDevice owned;if(!owned.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok())_exit(80);std::vector<d::NativeFilespaceDevice> files{{Id(2),g.zero.bootstrap.page_size_profile_uuid,&owned}};const auto actual=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),budget);_exit(actual.ok()&&actual.snapshot->watermark.abandonment&&actual.snapshot->state_sha256==resolved.snapshot->state_sha256&&actual.snapshot->selection.checkpoint_sha256==pending.selection.checkpoint_sha256?0:81);}int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh readonly process admits resolution and unchanged selected inventory");Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"reopen resolved inventory node");
  const auto observed=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),budget);Check(observed.ok()&&observed.snapshot->state_sha256==resolved.snapshot->state_sha256,"ordinary admission proves exact resolved inventory watermark");auto next=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*observed.snapshot,Id(12000),budget,&g.plan.intent);Check(next.ok()&&next.lease->snapshot().watermark.watermark==pending.watermark.watermark+1&&!next.lease->snapshot().watermark.abandonment&&!next.lease->snapshot().watermark.publication_plan&&next.lease->snapshot().selection.checkpoint_sha256==pending.selection.checkpoint_sha256,"new inventory attempt burns next generation without erasing selected starting allocation");next.lease.reset();
 }
 restore();
}
bool inventory_staging_admission=false;int inventory_allocation_read_shard=-1;
int inventory_consumer=-1,inventory_consumer_fault=-1,inventory_consumer_shard=-1;
struct ConsumerObservation {bool baseline=false,io=false,hash=false,resource=false,prefix=false;std::array<int,5> errors{};};
template<class Call>
void InventoryConsumerFaults(Fixture& f,const Bytes& before,Call call){
 if(inventory_consumer<0)return;
 byte scratch=0;for(unsigned i=0;i<4097;++i)Check(f.device.ReadAt(0,&scratch,1).ok(),"warm consumer telemetry before exhaustive failures");
 reads=writes=syncs=0;allocations=0;hash_seen=0;
 io_counting=true;counting=inventory_consumer_fault==2;hash_counting=inventory_consumer_fault==1;
 const auto good=call();counting=hash_counting=io_counting=false;
 const unsigned long sites=inventory_consumer_fault==0?reads:inventory_consumer_fault==1?hash_seen:allocations;
 Check(good.baseline&&sites&&!writes&&!syncs&&f.Read(0,256)==before,"actual read-only consumer fault baseline");
 const unsigned stride=inventory_consumer_fault==2?128:8;
 const unsigned lane=inventory_consumer_fault==1?inventory_consumer_shard/5:inventory_consumer_shard;
 const unsigned mode=inventory_consumer_shard%5+1;
 std::cout<<"consumer="<<inventory_consumer<<" fault="<<inventory_consumer_fault<<" sites="<<sites<<" shard="<<inventory_consumer_shard<<" stride="<<stride<<'\n';
 unsigned long consumed=0;
 for(unsigned long at=lane+(inventory_consumer_fault==2?0:1);at<sites+(inventory_consumer_fault==2?0:1);at+=stride){
  const auto lost=f.device.failed_io_latency_observations();reads=writes=syncs=0;
  if(inventory_consumer_fault==0)read_fault=at;
  if(inventory_consumer_fault==1){hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;}
  if(inventory_consumer_fault==2)allocation_budget=at;
  io_counting=true;const auto failed=call();io_counting=false;
  const bool reached=inventory_consumer_fault==0?reads>=at:inventory_consumer_fault==1?!hash_fault:allocation_budget==-1;
  read_fault=hash_fault=0;allocation_budget=-1;
  Check(reached,"every consumer fault position consumed");
  const bool diagnostic=inventory_consumer_fault==0?failed.io:inventory_consumer_fault==1?failed.hash:failed.resource;
  const bool optional_telemetry=inventory_consumer_fault==2&&failed.baseline&&f.device.failed_io_latency_observations()==lost+1;
  if(!((diagnostic&&!failed.prefix)||optional_telemetry)){std::cerr<<"consumer="<<inventory_consumer<<" fault="<<inventory_consumer_fault<<" position="<<at<<" io="<<failed.io<<" hash="<<failed.hash<<" resource="<<failed.resource<<" prefix="<<failed.prefix<<" baseline="<<failed.baseline;for(const auto code:failed.errors)std::cerr<<" error="<<code;std::cerr<<'\n';}
  Check((diagnostic&&!failed.prefix)||optional_telemetry,"required consumer failures preserve exact backend class and have no result prefix");
  Check(!writes&&!syncs&&f.Read(0,256)==before,"consumer failure never changes or syncs actual files");++consumed;
 }
 std::cout<<"consumer fault consumed="<<consumed<<'\n';
}
void InventoryStagingAdmission(Fixture& f,const Graph& graph,const Bundle& bundle,u64 budget){
 const auto before=f.Read(0,256);const auto active=*page::DecodeNativeTransactionInventoryPage(bundle.inventory[1]).page;
 const auto owner=active.inventory.entries.front().identity;
 const auto cp=*db::DecodeNativeCheckpointRoot(graph.target_bytes).root;const d::FilespaceRootReference checkpoint{9,0x300,cp.header.filespace_uuid,cp.header.page_number,cp.header.page_generation,cp.header.page_size_profile_uuid,cp.object_uuid};
 const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),budget);
 const auto directory=db::VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(Id(1),f.devices,checkpoint,budget);
 Check(selected.ok()&&directory.ok(),"actual selected and directory work baselines");
 if(directory.retained_image_bytes!=selected.retained_image_bytes+directory.directory.retained_image_bytes)std::cerr<<"directory charged="<<directory.retained_image_bytes<<" selected="<<selected.retained_image_bytes<<" directory="<<directory.directory.retained_image_bytes<<'\n';
 Check(directory.retained_image_bytes==selected.retained_image_bytes+directory.directory.retained_image_bytes,"current-directory consumer retains the complete actual selected verification work");
 reads=writes=syncs=0;io_counting=true;const auto allocation=db::VerifyCurrentNativeCheckpointAllocationFromOpenDevices(Id(1),f.devices,checkpoint,budget);io_counting=false;const unsigned allocation_reads=reads;
 if(!allocation.ok())std::cerr<<"current allocation error="<<int(allocation.error)<<'\n';
 Check(allocation.ok()&&!writes&&!syncs&&f.Read(0,256)==before,"current allocation reader admits actual operation-published maps without mutation");
 Check(allocation.allocation.pages.size()==graph.after_bytes.size(),"complete current allocation chain returned");
 for(std::size_t i=0;i<graph.after_bytes.size();++i)Check(allocation.allocation.pages[i].bytes==graph.after_bytes[i],"actual allocation images match independent original publication bytes");
 const auto proof=db::ReadNativeManagementControlAuthorityFromOpenDevices(Id(1),f.devices,Id(2),budget);
 Check(proof.ok()&&allocation.retained_image_bytes==selected.retained_image_bytes+allocation.allocation.retained_image_bytes+proof.verified_image_bytes,"allocation consumer charges both actual selected and original-record proof work");
 const auto original_directory=*std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==3;});
 const auto& retained_directory=proof.publications.at(cp.creator_operation_uuid).directory_root;
 Check(retained_directory.role==3&&retained_directory.page_type==9&&retained_directory.page==original_directory.page&&
   retained_directory.object_uuid==original_directory.object_uuid&&retained_directory.sha256==original_directory.sha256,
   "actual durable publication proof retains exact original target directory root");
 Check(!db::MatchesNativeManagementPublishedDirectory(proof,directory.directory,original_directory.sha256),
   "transaction-created directory cannot borrow checkpoint operation ownership");
 if(inventory_allocation_read_shard>=0){unsigned consumed=0;
  for(unsigned at=unsigned(inventory_allocation_read_shard)+1;at<=allocation_reads;at+=8){reads=writes=syncs=0;read_fault=at;io_counting=true;
   const auto failed=db::VerifyCurrentNativeCheckpointAllocationFromOpenDevices(Id(1),f.devices,checkpoint,budget);
   io_counting=false;const bool reached=reads>=at;read_fault=0;
   Check(reached,"every current-allocation read fault is consumed");
   Check((failed.error==db::NativeCheckpointError::io_failure||(failed.error==db::NativeCheckpointError::allocation_failure&&failed.allocation_error==page::NativeAllocationError::io_failure))&&!failed.checkpoint_inventory.checkpoint&&!failed.allocation.ok()&&!writes&&!syncs&&f.Read(0,256)==before,"current allocation read failure preserves backend classification and has no proof prefix or mutation");++consumed;
  }
  std::cout<<"current allocation read_sites="<<allocation_reads<<" shard="<<inventory_allocation_read_shard<<" consumed="<<consumed<<'\n';
 }
 const auto exact=db::VerifyCurrentNativeCheckpointAllocationFromOpenDevices(Id(1),f.devices,checkpoint,allocation.retained_image_bytes);
 Check(exact.ok()&&exact.retained_image_bytes==allocation.retained_image_bytes,"current allocation reader admits exact complete verification allowance");
 reads=writes=syncs=0;io_counting=true;const auto short_budget=db::VerifyCurrentNativeCheckpointAllocationFromOpenDevices(Id(1),f.devices,checkpoint,allocation.retained_image_bytes-1);io_counting=false;
 Check(short_budget.error==db::NativeCheckpointError::resource_exhausted&&!short_budget.checkpoint_inventory.checkpoint&&!short_budget.allocation.ok()&&!writes&&!syncs&&f.Read(0,256)==before,"one-byte-short allocation proof budget has no result prefix or mutation");
 reads=writes=syncs=0;io_counting=true;const auto no_proof_budget=db::VerifyCurrentNativeCheckpointAllocationFromOpenDevices(Id(1),f.devices,checkpoint,selected.retained_image_bytes+allocation.allocation.retained_image_bytes);io_counting=false;
 Check(no_proof_budget.error==db::NativeCheckpointError::resource_exhausted&&!no_proof_budget.checkpoint_inventory.checkpoint&&!no_proof_budget.allocation.ok()&&!writes&&!syncs&&f.Read(0,256)==before,"zero remaining operation proof allowance is a resource failure, not a creator mismatch");
 for(unsigned field=0;field<3;++field){auto stale=checkpoint;if(field==0)stale.object_uuid=Id(18100);else if(field==1)++stale.page_generation;else{const auto old=*db::DecodeNativeCheckpointRoot(graph.base_bytes).root;stale={9,0x300,old.header.filespace_uuid,old.header.page_number,old.header.page_generation,old.header.page_size_profile_uuid,old.object_uuid};}
  reads=writes=syncs=0;io_counting=true;const auto rejected=db::VerifyCurrentNativeCheckpointAllocationFromOpenDevices(Id(1),f.devices,stale,budget);io_counting=false;
  Check(rejected.error==db::NativeCheckpointError::binding_mismatch&&!rejected.checkpoint_inventory.checkpoint&&!rejected.allocation.ok()&&!writes&&!syncs&&f.Read(0,256)==before,"foreign identity generation and historical checkpoint cannot supply current allocation authority");
 }
 db::NativeCatalogLeafPage leaf;leaf.header={u32(f.size),6,Id(1),Id(2),Id(18001),250,4,0,cp.header.page_size_profile_uuid};leaf.body.relation_uuid.value=Id(18002);
 reads=writes=syncs=0;io_counting=true;const auto no_staging_proof_budget=db::StageNativeCatalogLeafFromOpenDevices(f.devices,checkpoint,owner,leaf,directory.retained_image_bytes+allocation.allocation.retained_image_bytes);io_counting=false;
 Check(no_staging_proof_budget.error==db::NativeCatalogLeafStageError::resource_exhausted&&!no_staging_proof_budget.receipt&&!writes&&!syncs&&f.Read(0,256)==before,"zero remaining staging proof allowance refuses without misclassifying owner or returning a receipt");
 if(inventory_consumer>=0)InventoryConsumerFaults(f,before,[&]{
  if(inventory_consumer==0){const auto r=db::VerifyCurrentNativeCheckpointAllocationFromOpenDevices(Id(1),f.devices,checkpoint,budget);using E=db::NativeCheckpointError;using A=page::NativeAllocationError;
   return ConsumerObservation{r.ok(),r.error==E::io_failure||(r.error==E::allocation_failure&&r.allocation_error==A::io_failure),r.error==E::hash_failure||(r.error==E::allocation_failure&&r.allocation_error==A::hash_failure),r.error==E::resource_exhausted||(r.error==E::allocation_failure&&r.allocation_error==A::resource_exhausted),r.checkpoint_inventory.checkpoint.has_value()||!r.checkpoint_inventory.inventory.entries.empty()||!r.allocation.pages.empty(),{int(r.error),int(r.checkpoint_inventory.error),int(r.allocation_error),0,0}};
  }
  const auto r=db::StageNativeCatalogLeafFromOpenDevices(f.devices,checkpoint,owner,leaf,budget);using E=db::NativeCatalogLeafStageError;using C=db::NativeCheckpointError;using A=page::NativeAllocationError;using D=page::NativeDirectoryError;using I=page::NativeInventoryError;
  return ConsumerObservation{r.error==E::reservation_mismatch&&!r.receipt,r.error==E::io_failure||r.checkpoint_error==C::io_failure||r.allocation_error==A::io_failure||r.directory_error==D::io_failure||r.inventory_error==I::io_failure,r.error==E::hash_failure||r.checkpoint_error==C::hash_failure||r.allocation_error==A::hash_failure||r.directory_error==D::hash_failure||r.inventory_error==I::hash_failure,r.error==E::resource_exhausted||r.checkpoint_error==C::resource_exhausted||r.allocation_error==A::resource_exhausted||r.directory_error==D::resource_exhausted||r.inventory_error==I::resource_exhausted,r.receipt.has_value(),{int(r.error),int(r.checkpoint_error),int(r.allocation_error),int(r.directory_error),int(r.inventory_error)}};
 });
 reads=writes=syncs=0;io_counting=true;const auto result=db::StageNativeCatalogLeafFromOpenDevices(f.devices,checkpoint,owner,leaf,budget);io_counting=false;
 if(result.error!=db::NativeCatalogLeafStageError::reservation_mismatch)std::cerr<<"operation-owned staging error="<<int(result.error)<<" checkpoint="<<int(result.checkpoint_error)<<'\n';
 Check(result.error==db::NativeCatalogLeafStageError::reservation_mismatch&&!result.receipt&&!writes&&!syncs&&f.Read(0,256)==before,"proved operation map does not turn an unreserved free page into a writer reservation");
 const auto reject=[&](const auto& writer,db::NativeCatalogLeafStageError wanted){reads=writes=syncs=0;io_counting=true;const auto denied=db::StageNativeCatalogLeafFromOpenDevices(f.devices,checkpoint,writer,leaf,budget);io_counting=false;Check(denied.error==wanted&&!denied.receipt&&!writes&&!syncs&&f.Read(0,256)==before,"operation-owned map preserves writer identity state and control-page reservation exclusions");};
 auto foreign=owner;foreign.transaction_uuid.value=Id(18003);reject(foreign,db::NativeCatalogLeafStageError::creator_mismatch);
 const auto starting=*page::DecodeNativeTransactionInventoryPage(bundle.inventory[2]).page;reject(starting.inventory.entries.front().identity,db::NativeCatalogLeafStageError::creator_not_active);
 leaf.header=graph.after.front().header;leaf.header.page_type=6;leaf.body.relation_uuid.value=graph.after.front().object_uuid;reject(owner,db::NativeCatalogLeafStageError::reservation_mismatch);
}
void InventoryPublication(Fixture& f,Graph& g,Bundle& b,unsigned profile){
 const u64 budget=4*f.budget;
 const auto inspect=[&]{return db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),budget);};
 const auto check=[&](Graph& graph,Bundle& bundle,mga::TransactionState state){const auto current=inspect();Check(current.ok()&&current.snapshot->selection.checkpoint_generation==graph.plan.reserved_generation,"ordinary actual admission selects the inventory publication");const auto bound=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),budget);Check(bound.ok()&&bound.checkpoint_inventory.inventory.next_local_transaction_id==4,"ordinary bound reader sees complete inventory counter");const auto& entries=bound.checkpoint_inventory.inventory.entries;Check(entries.size()==3&&entries[1].state==state&&entries[2].state==mga::TransactionState::created,"independent durable starting versus activation state");for(unsigned i=0;i<bundle.inventory.size();++i){const auto h=d::DecodeNativeCommonPageHeader(bundle.inventory[i].data(),128);Check(h.ok()&&f.Read(h.header->page_number)==bundle.inventory[i],"exact installed inventory bytes");}Check(f.Read(graph.plan.header.page_number)==graph.plan_bytes&&f.Read(graph.plan.target_checkpoint.page_number)==graph.target_bytes,"exact installed plan and checkpoint bytes");};
 const auto run=[&](Graph& graph,Bundle& bundle,mga::TransactionState state){const auto before=inspect();Check(before.ok(),"actual inventory publication base");graph.plan.base_selection_generation=before.snapshot->selection.selection_generation;graph.plan.intent.recovery_profile=2;
  auto held=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*before.snapshot,graph.plan.operation_uuid,budget,&graph.plan.intent);Check(held.ok(),"actual inventory publication reservation");graph.plan.reservation_state_sha256=held.lease->snapshot().state_sha256;graph.Refresh();auto expected=f.Read(0,256);
  const auto overlay=[&](u64 page,const Bytes& raw){Check(page<256&&raw.size()==f.size,"expected publication image extent");std::copy(raw.begin(),raw.end(),expected.begin()+page*f.size);};
  overlay(graph.plan.header.page_number,graph.plan_bytes);overlay(graph.plan.target_checkpoint.page_number,graph.target_bytes);for(unsigned i=0;i<graph.extent.size();++i)overlay(graph.plan.management_extent->first.page_number+i,graph.extent[i]);for(unsigned i=0;i<graph.after_bytes.size();++i)overlay(graph.after[i].header.page_number,graph.after_bytes[i]);for(unsigned i=0;i<bundle.pages.size();++i)overlay(bundle.root.first.page_number+i,bundle.pages[i]);for(const auto& raw:bundle.inventory){const auto h=d::DecodeNativeCommonPageHeader(raw.data(),128);Check(h.ok(),"independent expected inventory slot");overlay(h.header->page_number,raw);}
  for(unsigned i=0;i<2;++i){const auto r=std::find_if(graph.zero.roots.begin(),graph.zero.roots.end(),[&](const auto& r){return r.kind==20+i;});auto raw=f.Read(r->page_number);Ref(raw,516,Self(graph.plan.header));Put(raw,564,graph.plan.object_uuid);const auto plan_hash=Sha(graph.plan_bytes);std::copy(plan_hash.begin(),plan_hash.end(),raw.begin()+580);std::copy(graph.plan.reservation_state_sha256.begin(),graph.plan.reservation_state_sha256.end(),raw.begin()+612);Bytes state_bytes{'S','B','P','A','G','S','T','4'};state_bytes.insert(state_bytes.end(),raw.begin()+128,raw.begin()+368);state_bytes.insert(state_bytes.end(),raw.begin()+432,raw.begin()+768);const auto state_hash=Sha(state_bytes);std::copy(state_hash.begin(),state_hash.end(),raw.begin()+368);std::fill(raw.begin()+400,raw.begin()+432,0);const auto seal=Sha(raw);std::copy(seal.begin(),seal.end(),raw.begin()+400);overlay(r->page_number,raw);}
  if(inventory_install_stage>=0&&graph.plan.reserved_generation==u64(inventory_install_stage+2))InventoryInstallFaults(f,graph,bundle,held,expected,budget);
  if(inventory_resolution_mode)InventoryResolution(f,graph,bundle,held,budget);
  std::vector<off_t> order(f.devices.size(),-1);for(unsigned kind:{20u,21u}){const auto r=std::find_if(graph.zero.roots.begin(),graph.zero.roots.end(),[&](const auto& r){return r.kind==kind;});Check(r!=graph.zero.roots.end(),"independent anchor write order");order.push_back(r->page_number*f.size);order.push_back(-1);}order.push_back(graph.plan.header.page_number*f.size);order.push_back(-1);for(unsigned i=graph.extent.size();i>0;--i)order.push_back((graph.plan.management_extent->first.page_number+i-1)*f.size);order.push_back(-1);for(unsigned i=bundle.pages.size();i>0;--i)order.push_back((bundle.root.first.page_number+i-1)*f.size);order.push_back(-1);for(auto i=bundle.inventory.rbegin();i!=bundle.inventory.rend();++i){const auto h=d::DecodeNativeCommonPageHeader(i->data(),128);Check(h.ok(),"independent inventory ordering identity");order.push_back(h.header->page_number*f.size);}order.push_back(-1);for(auto i=graph.after.rbegin();i!=graph.after.rend();++i)order.push_back(i->header.page_number*f.size);order.push_back(-1);order.push_back(graph.plan.target_checkpoint.page_number*f.size);order.push_back(-1);io_trace_count=0;trace_io=true;
  const auto installed=db::InstallNativeManagementControlGraphOnLease(*held.lease,graph.plan,graph.target_bytes,graph.extent,bundle.pages,budget);trace_io=false;Check(io_trace_count==order.size()&&std::equal(order.begin(),order.end(),io_trace.begin()),"exact anchor plan extent bundle reverse-inventory reverse-map checkpoint write and barrier order");if(!installed.ok())std::cerr<<"inventory install error="<<int(installed.error)<<'\n';Check(installed.ok()&&installed.snapshot->selection.selection_generation==before.snapshot->selection.selection_generation,"actual inventory installation preserves selected predecessor");Check(f.Read(0,256)==expected,"independent complete file oracle including anchor and unchanged selectors");
  reads=writes=syncs=0;io_counting=true;const auto retry=db::InstallNativeManagementControlGraphOnLease(*held.lease,graph.plan,graph.target_bytes,graph.extent,bundle.pages,budget);io_counting=false;Check(retry.ok()&&!writes&&syncs==8+f.devices.size()&&f.Read(0,256)==expected,"exact warm inventory retry preserves bytes and repeats all required barriers");
  if(inventory_publication_cold){held.lease.reset();Bytes zero(f.size);const auto erase=[&](u64 page){const auto r=f.device.WriteAt(page*f.size,zero.data(),zero.size());Check(r.ok()&&r.bytes_transferred==zero.size(),"isolated missing unselected image fixture");};for(const auto& raw:bundle.inventory){const auto h=d::DecodeNativeCommonPageHeader(raw.data(),128);erase(h.header->page_number);}for(const auto& map:graph.after)erase(map.header.page_number);erase(graph.plan.target_checkpoint.page_number);Check(f.device.Sync().ok()&&f.device.Close().ok(),"durable missing target images before cold process");
   const auto child=fork();Check(child>=0,"fresh cold reconstruction process");if(!child){d::FileDevice file;if(!file.Open(f.path.string(),d::FileOpenMode::open_existing).ok())_exit(80);std::vector<d::NativeFilespaceDevice> files{{Id(2),graph.zero.bootstrap.page_size_profile_uuid,&file}};const auto observed=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),budget);if(!observed.ok())_exit(81);auto resumed=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),*observed.snapshot,graph.plan.operation_uuid,graph.plan.intent,budget);if(!resumed.ok())_exit(82);const auto reconstructed=db::ResumeNativeManagementControlGraphOnLease(*resumed.lease,budget);if(!reconstructed.ok())_exit(83);resumed.lease.reset();const auto published=db::RecoverNativeManagementCheckpointPublicationOnOpenDevices(Id(1),files,Id(2),graph.plan.operation_uuid,graph.plan.intent,budget);_exit(published.ok()&&published.snapshot->selection.checkpoint_generation==graph.plan.reserved_generation?0:84);}int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"cold reconstruction and exact selector recovery from retained original bytes");Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"parent reopen after cold reconstruction");
  }else{const auto published=db::PublishNativeManagementControlGraphOnLease(*held.lease,budget);if(!published.ok())std::cerr<<"inventory publish error="<<int(published.error)<<'\n';Check(published.ok()&&published.snapshot->selection.selection_generation==before.snapshot->selection.selection_generation+1,"actual selector publication acknowledges exact candidate");held.lease.reset();}check(graph,bundle,state);
  if(inventory_resolution_mode){const auto after=f.Read(0,256);const auto rejected=db::AbandonNativeInventoryPublicationOnOpenDevices(Id(1),f.devices,Id(2),*installed.snapshot,graph.plan.operation_uuid,graph.plan.intent,Id(15000),budget);Check(!rejected.ok()&&!rejected.snapshot&&f.Read(0,256)==after,"already selected inventory publication cannot be abandoned");}
 };
 run(g,b,mga::TransactionState::created);
 const auto activation_state=inventory_read_only?mga::TransactionState::read_only_active:mga::TransactionState::active;
 Graph activation(f,false,1,&g);Bundle next(activation,3,50,3);
 for(unsigned i=0;i<3;++i){auto p=*page::DecodeNativeTransactionInventoryPage(next.inventory[i]).page;const auto prior=*page::DecodeNativeTransactionInventoryPage(b.inventory[i]).page;p.object_uuid=prior.object_uuid;p.inventory=prior.inventory;if(i==1)p.inventory.entries.front().state=activation_state;next.inventory[i]=InventoryOracle(p);Find(activation.after,p.header.page_number).owner_uuid=p.object_uuid;}
 activation.after_bytes=EncodeMaps(activation.after);next.Oracle();activation.plan.control_bundle=next.root;activation.plan.intent.recovery_profile=2;activation.plan.base_selection_generation=2;auto& root=*std::find_if(activation.target.roots.begin(),activation.target.roots.end(),[](const auto& r){return r.role==1;});const auto head=*page::DecodeNativeTransactionInventoryPage(next.inventory.front()).page;root={1,0x301,Self(head.header),head.object_uuid,Sha(next.inventory.front())};activation.target.selected_local_transaction_id=head.inventory.next_local_transaction_id-1;activation.Refresh();run(activation,next,activation_state);
 if(inventory_staging_admission)InventoryStagingAdmission(f,activation,next,budget);
 Check(f.device.Close().ok()&&(!inventory_mixed_mode||f.secondary.Close().ok()),"close fully published inventory node");const auto child=fork();Check(child>=0,"fresh-process published inventory admission");if(!child){d::FileDevice file,second;if(!file.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok())_exit(80);std::vector<d::NativeFilespaceDevice> files{{Id(2),g.zero.bootstrap.page_size_profile_uuid,&file}};if(inventory_mixed_mode){if(!second.Open((f.path.parent_path()/"secondary").string(),d::FileOpenMode::open_existing_read_only).ok())_exit(82);files.push_back({Id(16000),d::kCanonicalFilespacePageProfiles[(profile+1)%5].uuid,&second});}const auto bound=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),files,Id(2),budget);_exit(bound.ok()&&bound.checkpoint_inventory.inventory.entries.size()==3&&bound.checkpoint_inventory.inventory.entries[1].state==activation_state?0:81);}int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh process observes separate durable starting and activation");Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"readonly parent reopen");if(inventory_mixed_mode){Check(f.secondary.Open((f.path.parent_path()/"secondary").string(),d::FileOpenMode::open_existing_read_only).ok(),"readonly predecessor filespace reopen");Bytes actual(f.secondary_before.size());const auto read=f.secondary.ReadAt(0,actual.data(),actual.size());Check(read.ok()&&read.bytes_transferred==actual.size()&&actual==f.secondary_before,"inventory publication preserves every secondary predecessor byte");}check(activation,next,activation_state);std::cout<<"inventory publication profile="<<profile<<" read_only="<<inventory_read_only<<" starting_and_activation=true physical_only=true\n";
}
void MixedInventoryPredecessor(Fixture& f,unsigned profile){
 const auto zero=d::ReadFilespacePageZeroFromOpenDevice(f.device);Check(zero.ok(),"mixed predecessor primary bootstrap");const auto& z=*zero.record;const auto root=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==4;});Check(root!=z.roots.end(),"primary inventory fixture root");auto head=*page::DecodeNativeTransactionInventoryPage(f.Read(root->page_number)).page;auto tail=head;const auto& secondary_profile=d::kCanonicalFilespacePageProfiles[(profile+1)%5];tail.header={secondary_profile.page_size_bytes,0x301,Id(1),Id(16000),Id(16002),32,1,0,secondary_profile.uuid};tail.previous=Self(head.header);tail.next.reset();head.inventory.entries.clear();head.next=Self(tail.header);const auto head_bytes=InventoryOracle(head),tail_bytes=InventoryOracle(tail);Check(page::DecodeNativeTransactionInventoryPage(head_bytes).ok()&&page::DecodeNativeTransactionInventoryPage(tail_bytes).ok(),"canonical mixed inventory images with committed creator only in continuation");
 auto secondary_zero=z;secondary_zero.bootstrap.filespace_uuid=Id(16000);secondary_zero.bootstrap.page_size_profile_uuid=secondary_profile.uuid;secondary_zero.bootstrap.page_size_bytes=secondary_profile.page_size_bytes;secondary_zero.bootstrap.filespace_role=2;secondary_zero.page_uuid=Id(16004);secondary_zero.free_pages=253;secondary_zero.preallocated_pages=0;secondary_zero.roots={{3,3,Id(16000),13,1,secondary_profile.uuid,Id(16001)},{4,0x301,Id(16000),32,1,secondary_profile.uuid,head.object_uuid}};
 page::NativeAllocationMap map;map.header={secondary_profile.page_size_bytes,3,Id(1),Id(16000),Id(16003),13,1,0,secondary_profile.uuid};map.object_uuid=Id(16001);map.map_generation=map.capacity_generation=1;map.total_pages=256;map.creator_transaction_uuid=Id(5);map.creator_local_transaction_id=1;map.states.assign(256,State::free);for(unsigned slot:{0u,13u,32u}){page::NativeAllocationRecord record;record.page_number=slot;record.allocation_uuid=Id(16100+slot);record.page_uuid=slot==0?secondary_zero.page_uuid:slot==13?map.header.page_uuid:tail.header.page_uuid;record.owner_uuid=slot==0?Id(16000):slot==13?map.object_uuid:tail.object_uuid;record.creator_transaction_uuid=Id(5);record.creator_local_transaction_id=1;record.page_generation=1;record.page_type=slot==0?1:slot==13?3:0x301;map.states[slot]=State::allocated;map.records.push_back(record);}const auto map_bytes=MapOracle(map);Check(page::DecodeNativeAllocationMap(map_bytes).ok(),"exact original secondary control allocations");const auto bootstrap=d::EncodeFilespacePageZero(secondary_zero);Check(bootstrap.ok(),"canonical secondary fixture bootstrap");
 Check(f.secondary.Open((f.path.parent_path()/"secondary").string(),d::FileOpenMode::create_new).ok(),"own mixed predecessor filespace");Bytes secondary_file(256*secondary_profile.page_size_bytes);std::copy(bootstrap.bytes->begin(),bootstrap.bytes->end(),secondary_file.begin());std::copy(map_bytes.begin(),map_bytes.end(),secondary_file.begin()+13*secondary_profile.page_size_bytes);std::copy(tail_bytes.begin(),tail_bytes.end(),secondary_file.begin()+32*secondary_profile.page_size_bytes);const auto stored=f.secondary.WriteAt(0,secondary_file.data(),secondary_file.size());Check(stored.ok()&&stored.bytes_transferred==secondary_file.size()&&f.secondary.Sync().ok(),"persist complete isolated secondary predecessor fixture");f.secondary_before=secondary_file;f.devices.push_back({Id(16000),secondary_profile.uuid,&f.secondary});
 const auto write=[&](u64 slot,const Bytes& raw){const auto r=f.device.WriteAt(slot*f.size,raw.data(),raw.size());Check(r.ok()&&r.bytes_transferred==raw.size(),"rebind isolated mixed predecessor fixture");};write(root->page_number,head_bytes);const auto checkpoint=std::find_if(z.roots.begin(),z.roots.end(),[](const auto& r){return r.kind==9;});Check(checkpoint!=z.roots.end(),"primary genesis checkpoint fixture");auto cp=*db::DecodeNativeCheckpointRoot(f.Read(checkpoint->page_number)).root;auto& inventory=*std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==1;});inventory.sha256=Sha(head_bytes);
 // Independently append the actual secondary bootstrap to the V1 directory.
 // Supplying a device without its checkpoint-bound membership is not authority.
 auto& directory_root=*std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==3;});auto directory=f.Read(directory_root.page.page_number);
 Check(std::equal(directory.begin()+128,directory.begin()+136,"SBFDIR01")&&LoadLittle32(directory.data()+208)==1,"single-page genesis directory fixture");
 Num(directory,140,4,768);Num(directory,192,8,2);Num(directory,208,4,2);constexpr std::size_t at=576;const auto& a=secondary_zero.bootstrap;
 Put(directory,at,a.filespace_uuid);Put(directory,at+16,a.page_size_profile_uuid);Put(directory,at+32,a.checksum_profile_uuid);Put(directory,at+48,a.encryption_profile_uuid);
 Put(directory,at+64,Id(16005));Put(directory,at+80,secondary_zero.page_uuid);Num(directory,at+96,8,secondary_zero.page_generation);Num(directory,at+104,8,secondary_zero.root_set_generation);Num(directory,at+112,8,secondary_zero.total_pages);
 Num(directory,at+128,2,a.filespace_role);Num(directory,at+130,2,a.lifecycle_state);Num(directory,at+132,4,a.flags);Num(directory,at+136,4,a.page_size_bytes);Num(directory,at+140,4,a.durable_format_generation);
 std::fill(directory.begin()+296,directory.begin()+328,0);const auto seal=Sha(directory);std::copy(seal.begin(),seal.end(),directory.begin()+296);directory_root.sha256=Sha(directory);write(directory_root.page.page_number,directory);
 const auto rebound=db::EncodeNativeCheckpointRoot(cp);Check(rebound.ok(),"mixed predecessor directory-bound checkpoint fixture");write(checkpoint->page_number,rebound.bytes);const auto complete_cp_hash=Sha(rebound.bytes);
 for(const auto& r:z.roots)if(r.kind==18||r.kind==19){auto selector=*db::DecodeNativeCheckpointSelection(f.Read(r.page_number)).selection;selector.checkpoint_sha256=complete_cp_hash;write(r.page_number,SelectorOracle(selector));}else if(r.kind==20||r.kind==21){auto watermark=*db::DecodeNativePublicationWatermark(f.Read(r.page_number)).state;watermark.base_checkpoint_sha256=complete_cp_hash;const auto bytes=db::EncodeNativePublicationWatermark(watermark);Check(bytes.ok(),"mixed predecessor initial watermark binding");write(r.page_number,bytes.bytes);}Check(f.device.Sync().ok(),"mixed predecessor complete fixture barrier");const auto admitted=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),4*f.budget);Check(admitted.ok()&&admitted.checkpoint_inventory.inventory_pages.size()==2&&admitted.checkpoint_inventory.inventory.entries.size()==1,"actual bound selected inventory requires the owned mixed-profile continuation");
 const auto original=f.Read(0,256);const auto other=f.devices.back();f.devices.pop_back();const auto absent=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),4*f.budget);Check(!absent.ok()&&!absent.snapshot&&f.Read(0,256)==original,"missing secondary inventory device cannot become a head-only admission");f.devices.push_back(other);auto corrupt=tail_bytes;corrupt.back()^=1;auto damage=f.secondary.WriteAt(32*secondary_profile.page_size_bytes,corrupt.data(),corrupt.size());Check(damage.ok()&&damage.bytes_transferred==corrupt.size()&&f.secondary.Sync().ok(),"isolated corrupted predecessor continuation");const auto rejected=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),4*f.budget);Check(!rejected.ok()&&!rejected.snapshot&&f.Read(0,256)==original,"actual continuation corruption refuses without mutating primary");damage=f.secondary.WriteAt(32*secondary_profile.page_size_bytes,tail_bytes.data(),tail_bytes.size());Check(damage.ok()&&damage.bytes_transferred==tail_bytes.size()&&f.secondary.Sync().ok(),"restore complete immutable secondary predecessor");
}
void GenesisDirectoryProof(Fixture& f){
 // This starts from durable genesis, without any fabricated publication proof.
 const auto zero=d::ReadFilespacePageZeroFromOpenDevice(f.device);Check(zero.ok(),"directory graph actual primary genesis");
 const auto ref=*std::find_if(zero.record->roots.begin(),zero.record->roots.end(),[](const auto& r){return r.kind==9;});
 const auto base=db::ReadNativeCheckpointRootFromOpenDevice(f.device,Id(1),ref);Check(base.ok(),"directory graph genesis checkpoint");
 const auto root=*std::find_if(base.root->roots.begin(),base.root->roots.end(),[](const auto& r){return r.role==3;});
 const auto original=f.Read(root.page.page_number);const auto parsed=page::DecodeNativeFilespaceDirectory(original);Check(parsed.ok(),"directory graph genesis directory");
 const auto write=[&](u64 n,const Bytes& bytes){const auto r=f.device.WriteAt(n*f.size,bytes.data(),bytes.size());Check(r.ok()&&r.bytes_transferred==bytes.size(),"directory graph isolated primary image write");};
 const auto read=[&](const Bytes& bytes){const auto& cp=*base.root;return db::ReadNativeManagementControlGraphFromOpenDevices(Id(1),f.devices,Id(2),
   {Self(cp.header),cp.object_uuid,Sha(bytes),cp.checkpoint_generation,cp.root_set_generation,cp.timeline_uuid},f.budget);};
 Check(read(base.bytes).ok(),"primary-only actual genesis directory admitted");
 for(unsigned mutation=0;mutation<3;++mutation){auto directory=*parsed.directory;
   if(mutation==0){directory.creator_transaction_uuid={};directory.creator_local_transaction_id=0;directory.creator_operation_uuid=Id(333);}
   if(mutation==1)directory.creator_transaction_uuid=Id(333);
   if(mutation==2)directory.creator_local_transaction_id=2;
   const auto image=page::EncodeNativeFilespaceDirectory(directory);Check(image.ok(),"canonical unproved directory creator fixture");
   auto cp=*base.root;auto& changed=*std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==3;});changed.sha256=Sha(image.bytes);
   const auto rebound=db::EncodeNativeCheckpointRoot(cp);Check(rebound.ok(),"fully rebound primary-only directory creator fixture");
   write(root.page.page_number,image.bytes);write(ref.page_number,rebound.bytes);Check(f.device.Sync().ok(),"persist primary-only creator substitution");
   reads=writes=syncs=0;io_counting=true;const auto rejected=read(rebound.bytes);io_counting=false;
   Check(rejected.error==db::NativeManagementControlAuthorityError::creator_mismatch&&!rejected.anchor&&rejected.publications.empty()&&rejected.allocations.empty()&&!rejected.verified_image_bytes&&!writes&&!syncs,
     "primary-only directory cannot borrow operation UUID membership or absent transaction identity");
 }
 write(root.page.page_number,original);write(ref.page_number,base.bytes);Check(f.device.Sync().ok()&&read(base.bytes).ok(),"restore exact actual primary genesis directory");
}
void InventoryAllocation(unsigned profile){
 Fixture f(profile);if(inventory_staging_admission&&inventory_consumer<0&&inventory_allocation_read_shard<0)GenesisDirectoryProof(f);
 if(inventory_mixed_mode)MixedInventoryPredecessor(f,profile);Graph g(f);Bundle b(g,3);const auto old_root=*std::find_if(g.base.roots.begin(),g.base.roots.end(),[](const auto& r){return r.role==1;});Pages before;auto ref=std::optional{old_root.page};
 while(ref){const auto file=std::find_if(f.devices.begin(),f.devices.end(),[&](const auto& d){return d.filespace_uuid==ref->filespace_uuid;});const auto* pp=d::FindCanonicalFilespacePageProfile(ref->page_size_profile_uuid);Check(file!=f.devices.end()&&pp,"owned predecessor profile fixture");Bytes raw(pp->page_size_bytes);const auto read=file->device->ReadAt(ref->page_number*pp->page_size_bytes,raw.data(),raw.size());Check(read.ok()&&read.bytes_transferred==raw.size(),"actual complete predecessor fixture read");auto p=page::DecodeNativeTransactionInventoryPage(raw);Check(p.ok(),"actual selected-before inventory fixture");before.push_back(raw);ref=p.page->next;}
 Check(before.size()==(inventory_mixed_mode?2u:1u),"genesis fixture complete inventory image count");auto old=*page::DecodeNativeTransactionInventoryPage(before.front()).page;for(unsigned i=1;i<before.size();++i){const auto part=page::DecodeNativeTransactionInventoryPage(before[i]);Check(part.ok(),"decoded complete predecessor continuation");old.inventory.entries.insert(old.inventory.entries.end(),part.page->inventory.entries.begin(),part.page->inventory.entries.end());}std::vector<page::NativeTransactionInventoryPage> candidate;
 auto proposed=old.inventory;
 for(unsigned i=0;i<3;++i){auto p=*page::DecodeNativeTransactionInventoryPage(b.inventory[i]).page;p.object_uuid=old.object_uuid;p.inventory.next_local_transaction_id=old.inventory.next_local_transaction_id+2;p.inventory.next_commit_sequence=old.inventory.next_commit_sequence;
  if(!i)p.inventory.entries=old.inventory.entries;else{auto& e=p.inventory.entries.front();auto begun=inventory_read_only?mga::BeginLocalReadOnlyTransaction(proposed,e.identity.transaction_uuid,e.begin_unix_epoch_millis):mga::BeginLocalTransaction(proposed,e.identity.transaction_uuid,e.begin_unix_epoch_millis);Check(begun.ok(),"real BEGIN helper builds sequential native publication candidates");e=begun.entry;proposed=std::move(begun.inventory);e.state=mga::TransactionState::created;}
  Find(g.after,p.header.page_number).owner_uuid=p.object_uuid;candidate.push_back(p);
 }
 const auto refresh=[&]{for(unsigned i=0;i<3;++i){b.inventory[i]=InventoryOracle(candidate[i]);const auto encoded=page::EncodeNativeTransactionInventoryPage(candidate[i]);Check(encoded.ok()&&encoded.bytes==b.inventory[i],"independent inventory payload bytes");}g.after_bytes=EncodeMaps(g.after);b.Oracle();g.plan.control_bundle=b.root;g.plan.base_selection_generation=1;g.plan.intent.recovery_profile=2;
  auto& root=*std::find_if(g.target.roots.begin(),g.target.roots.end(),[](const auto& r){return r.role==1;});root={1,0x301,Self(candidate.front().header),old.object_uuid,Sha(b.inventory.front())};g.target.selected_local_transaction_id=candidate.front().inventory.next_local_transaction_id-1;g.Refresh();};refresh();
 const auto budget=[&]{u64 n=g.Budget();for(const auto& raw:b.pages)n+=raw.size();for(const auto* images:{&before,&b.inventory})for(const auto& raw:*images)n+=4*raw.size();return n;};
 const auto call=[&](u64 limit=0){return db::ValidateNativeManagementControlAllocation(g.base_bytes,g.target_bytes,g.plan_bytes,g.extent,g.before_bytes,g.after_bytes,limit?limit:budget(),b.pages,before);};
 Check(call()==CE::none,"complete original allocation plus starting inventory delta");Check(call(budget()-1)==CE::resource_exhausted,"inventory delta exact work allowance");
 if(inventory_publication_mode){InventoryPublication(f,g,b,profile);return;}
 Check(db::ValidateNativeManagementControlAllocation(g.base_bytes,g.target_bytes,g.plan_bytes,g.extent,g.before_bytes,g.after_bytes,budget(),b.pages)==CE::invalid_request,"profile2 cannot omit actual predecessor images");
 const auto saved_before=before;before.push_back(before.front());Check(call()!=CE::none,"before chain cannot carry trailing duplicate images");before=saved_before;
 for(unsigned field=0;field<3;++field){auto wrong=g.target;if(field==0)--wrong.selected_local_transaction_id;if(field==1)++wrong.local_durable_transaction_id;if(field==2)wrong.roots.front()=g.base.roots.front();auto p=g.plan;const auto cp=Finish(p,wrong);const auto image=Oracle(p);Check(db::ValidateNativeManagementControlAllocation(g.base_bytes,cp,image,g.extent,g.before_bytes,g.after_bytes,budget(),b.pages,before)!=CE::none,"no unrelated scalar or wrong inventory-root publication");}
 auto saved=candidate;candidate[1].inventory.entries.front().state=mga::TransactionState::active;refresh();Check(call()==CE::invalid_delta,"new active cannot bypass starting publication");candidate=saved;
 candidate[1].inventory.entries.front().begin_visible_through_commit_sequence=0;refresh();Check(call()==CE::invalid_delta,"new starting entry binds original commit boundary");candidate=saved;refresh();
 const auto good_maps=g.after;auto& record=Find(g.after,221);record.creator_operation_uuid=Id(999);g.after_bytes=EncodeMaps(g.after);b.Oracle();g.plan.control_bundle=b.root;g.Refresh();Check(call()!=CE::none,"inventory original creator cannot be replaced");g.after=good_maps;refresh();
 const auto good_before=g.before;auto& slot=Cover(g.before,221);slot.states[221-slot.first_page]=State::quarantined;g.before_bytes=EncodeMaps(g.before);auto altered_base=g.base;auto& base_map=*std::find_if(altered_base.roots.begin(),altered_base.roots.end(),[](const auto& r){return r.role==4;});base_map.sha256=Sha(g.before_bytes.front());const auto changed=db::EncodeNativeCheckpointRoot(altered_base);Check(changed.ok(),"occupied before slot fixture");auto p=g.plan;p.base_checkpoint_sha256=Sha(changed.bytes);auto cp=g.target;const auto target=Finish(p,cp);Check(db::ValidateNativeManagementControlAllocation(changed.bytes,target,Oracle(p),g.extent,g.before_bytes,g.after_bytes,budget(),b.pages,before)==CE::invalid_delta,"valid after inventory allocation cannot consume nonfree predecessor slot");g.before=good_before;g.before_bytes=EncodeMaps(g.before);
 auto encrypted=before.front();Num(encrypted,88,8,1);HeaderSeal(encrypted);before.front()=encrypted;auto eb=g.base;auto& er=*std::find_if(eb.roots.begin(),eb.roots.end(),[](const auto& r){return r.role==1;});er.sha256=Sha(encrypted);const auto ebase=db::EncodeNativeCheckpointRoot(eb);Check(ebase.ok(),"encrypted predecessor checkpoint fixture");p=g.plan;p.base_checkpoint_sha256=Sha(ebase.bytes);const auto ecp=Finish(p,g.target);Check(db::ValidateNativeManagementControlAllocation(ebase.bytes,ecp,Oracle(p),g.extent,g.before_bytes,g.after_bytes,budget(),b.pages,before)==CE::encrypted_requires_authority,"ciphertext predecessor requires owning crypto boundary");before=saved_before;
 Check(call()==CE::none,"restore complete valid inventory allocation delta");
 const auto initial_base=g.base;const auto initial_base_bytes=g.base_bytes;const auto initial_plan=g.plan;
 const auto predecessor=[&]{auto prior=old;prior.inventory.next_local_transaction_id=candidate.front().inventory.next_local_transaction_id;prior.inventory.next_commit_sequence=candidate.front().inventory.next_commit_sequence;prior.inventory.entries.clear();for(const auto& item:candidate)prior.inventory.entries.insert(prior.inventory.entries.end(),item.inventory.entries.begin(),item.inventory.entries.end());before={InventoryOracle(prior)};Check(page::DecodeNativeTransactionInventoryPage(before.front()).ok(),"canonical independent preceding starting inventory");g.base=initial_base;g.base.selected_local_transaction_id=prior.inventory.next_local_transaction_id-1;auto& root=*std::find_if(g.base.roots.begin(),g.base.roots.end(),[](const auto& r){return r.role==1;});root.sha256=Sha(before.front());const auto cp=db::EncodeNativeCheckpointRoot(g.base);Check(cp.ok(),"preceding starting checkpoint fixture");g.base_bytes=cp.bytes;g.plan.base_checkpoint_sha256=Sha(cp.bytes);};
 predecessor();candidate[1].inventory.entries.front().state=mga::TransactionState::active;refresh();Check(call()==CE::none,"activation consumes a real preceding starting image delta");candidate=saved;
 candidate[1].inventory.entries.front().identity.scope=mga::TransactionScope::cluster_global;predecessor();refresh();Check(call()==CE::none,"unchanged cluster inventory entry carries no local decision grant");candidate[1].inventory.entries.front().state=mga::TransactionState::active;refresh();Check(call()==CE::cluster_requires_authority,"cluster transition requires owning cluster authority");
 candidate=saved;predecessor();candidate.front().inventory.entries.clear();refresh();Check(call()==CE::none,"structural terminal removal remains separate from owning retention permission");
 candidate=saved;before=saved_before;g.base=initial_base;g.base_bytes=initial_base_bytes;g.plan=initial_plan;refresh();Check(call()==CE::none,"restore starting allocation after lifecycle and cluster grids");InventoryHistory(f,g,b,profile);if(profile||inventory_allocation_route>=0)return;
 allocations=0;counting=true;Check(call()==CE::none,"inventory control delta allocation baseline");counting=false;const auto sites=allocations;for(unsigned long at=0;at<sites;++at){allocation_budget=at;const auto result=call();const auto left=allocation_budget;allocation_budget=-1;Check(left==-1&&result==CE::resource_exhausted,"inventory control delta consumes every allocation failure");}
 hash_counting=true;hash_seen=0;Check(call()==CE::none,"inventory control delta hash baseline");hash_counting=false;const auto hashes=hash_seen;for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto result=call();Check(!hash_fault&&result==CE::hash_failure,"inventory control delta propagates every hash failure");}
 std::cout<<"inventory control delta allocations="<<sites<<" hashes="<<hashes<<'\n';
}
bool SameOwnedInventory(const mga::LocalTransactionInventory& actual,const mga::LocalTransactionInventory& expected) {
 if(actual.next_local_transaction_id!=expected.next_local_transaction_id||actual.next_commit_sequence!=expected.next_commit_sequence||
    actual.entries.size()!=expected.entries.size())return false;
 for(std::size_t n=0;n<actual.entries.size();++n) {
  const auto& a=actual.entries[n];const auto& b=expected.entries[n];
  if(a.identity.local_id.value!=b.identity.local_id.value||a.identity.transaction_uuid.kind!=b.identity.transaction_uuid.kind||
    a.identity.transaction_uuid.value!=b.identity.transaction_uuid.value||a.identity.scope!=b.identity.scope||
    a.state!=b.state||a.archived_from_state!=b.archived_from_state||a.begin_unix_epoch_millis!=b.begin_unix_epoch_millis||
    a.final_unix_epoch_millis!=b.final_unix_epoch_millis||a.begin_visible_through_local_transaction_id!=b.begin_visible_through_local_transaction_id||
    a.begin_visible_through_commit_sequence!=b.begin_visible_through_commit_sequence||a.commit_sequence!=b.commit_sequence||
    a.evidence_record_required!=b.evidence_record_required||a.evidence_record_written!=b.evidence_record_written||a.rollback_only!=b.rollback_only)return false;
 }
 return true;
}
void OwnedInventoryPublication(unsigned profile,bool read_only,bool mixed=false,bool startup=false) {
 Fixture f(profile);f.budget*=mixed?16:4;
 if(mixed)MixedInventoryPredecessor(f,profile);
 const auto initial=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
 Check(initial.ok(),"owned constructor actual initial checkpoint");
 auto inventory=initial.checkpoint_inventory.inventory;
 const unsigned count=startup?1:profile==0?(f.size-384)/72+1:2;
 for(unsigned i=0;i<count;++i) {
  auto begun=read_only?mga::BeginLocalReadOnlyTransaction(inventory,{UuidKind::transaction,Id(19000+i)},3000+i):
    mga::BeginLocalTransaction(inventory,{UuidKind::transaction,Id(19000+i)},3000+i);
  Check(begun.ok(),"owned constructor actual BEGIN candidate");inventory=std::move(begun.inventory);
  inventory.entries.back().state=mga::TransactionState::created;
 }
 auto zero=d::ReadFilespacePageZeroFromOpenDevice(f.device);Check(zero.ok(),"owned constructor actual bootstrap");
 for(unsigned phase=0;phase<3;++phase) {
  if(phase)inventory.entries[1].state=read_only?mga::TransactionState::read_only_active:mga::TransactionState::active;
  auto record=records::Example(phase+1);record.uuid=Id(20000);record.bootstrap_uuid=zero.record->page_uuid;
  record.revision=phase+1;record.updated_at=Id(2000+phase);
  if(!phase){record.security_snapshot_uuid={};record.generation_guards={};}
  record.idempotency_key="owned-inventory";
  if(startup){record.normalized_request_bytes=Bytes(64);record.request_context_uuid=Id(21000);
    Put(record.normalized_request_bytes,0,record.uuid);Put(record.normalized_request_bytes,16,record.request_context_uuid);
    Put(record.normalized_request_bytes,32,inventory.entries[1].identity.transaction_uuid.value);
    Num(record.normalized_request_bytes,48,8,inventory.entries[1].identity.local_id.value);Num(record.normalized_request_bytes,56,8,19);
    record.normalized_request_sha256=Sha(record.normalized_request_bytes);}
  if(phase==2)for(unsigned n=0;n<40;++n){auto step=records::Step(1,n+1);step.operation_uuid=record.uuid;record.steps.push_back(step);}
  db::NativePublicationIntent intent{record.initiator_uuid,record.request_context_uuid,record.policy_snapshot_uuid,
    record.normalized_request_sha256,record.initiator_kind};intent.recovery_profile=2;
  if(startup)intent.startup_binding=db::NativeStartupBinding{record.uuid,record.request_context_uuid,
    inventory.entries[1].identity.transaction_uuid.value,inventory.entries[1].identity.local_id.value,19};
  const auto before=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(before.ok(),"owned constructor actual selected base");
  if(startup){const auto untouched=f.Read(0,256);
    for(unsigned wrong=0;wrong<2;++wrong){auto forged=intent;
      if(!wrong)++forged.startup_binding->local_transaction_id;
      else forged.startup_binding->transaction_uuid=initial.checkpoint_inventory.inventory.entries.front().identity.transaction_uuid.value;
      const auto refused=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*before.snapshot,Id(20100+phase),f.budget,&forged);
      Check(!refused.ok()&&!refused.effects.write_attempts&&f.Read(0,256)==untouched,"startup binding must match actual allocation or exact next local number");}}
  auto held=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*before.snapshot,Id(20100+phase),f.budget,&intent);
  Check(held.ok(),"owned constructor actual generation reservation");
  const auto pending=f.Read(0,256);
  if(startup){const auto snapshot=held.lease->snapshot();
    Check(snapshot.watermark.intent==intent&&snapshot.selection.checkpoint_generation==before.snapshot->selection.checkpoint_generation,
      "complete startup tuple durable before inventory selection changes");
    held.lease.reset();
    const auto abandoned=db::AbandonNativeInventoryPublicationOnOpenDevices(Id(1),f.devices,Id(2),snapshot,Id(20100+phase),intent,Id(21200+phase),f.budget);
    Check(!abandoned.ok()&&!abandoned.effects.write_attempts&&f.Read(0,256)==pending,"generic physical abandonment cannot reconcile startup");
    const auto displaced=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),snapshot,Id(21300+phase),f.budget,&intent);
    Check(displaced.error==db::NativePublicationError::operation_pending&&!displaced.effects.write_attempts,"fresh attempt cannot erase pending startup binding");
    for(unsigned field=0;field<5;++field){auto forged=intent;auto& b=*forged.startup_binding;
      if(!field)b.operation_uuid=Id(21400);if(field==1)b.session_uuid=Id(21400);if(field==2)b.transaction_uuid=Id(21400);
      if(field==3)++b.local_transaction_id;if(field==4)++b.fence_generation;
      const auto refused=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),snapshot,Id(20100+phase),forged,f.budget);
      Check(refused.error==db::NativePublicationError::request_mismatch&&!refused.effects.write_attempts&&f.Read(0,256)==pending,"exact retry compares every binary startup binding field");}
    held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),snapshot,Id(20100+phase),intent,f.budget);
    Check(held.ok()&&held.lease->snapshot().watermark.intent==intent,"exact original startup attempt resumes without new identity");
    auto wrong_inventory=inventory;wrong_inventory.entries[1].identity.transaction_uuid.value=Id(21500);
    const auto mismatch=db::PublishNativeInventoryOnLease(*held.lease,record,wrong_inventory,f.budget,*f.issuer);
    Check(!mismatch.ok()&&!mismatch.effects.selector_write_attempted&&f.Read(0,256)==pending,"startup publication cannot install a different transaction");
  }
  auto wrong=record;wrong.request_context_uuid=Id(20200);
  const auto mismatch=db::PublishNativeInventoryOnLease(*held.lease,wrong,inventory,f.budget,*f.issuer);
  if(mismatch.error!=db::NativePublicationError::request_mismatch)std::cerr<<"owned mismatch error="<<int(mismatch.error)<<'\n';
  Check(mismatch.error==db::NativePublicationError::request_mismatch&&!mismatch.snapshot&&f.Read(0,256)==pending,
    "owned constructor binds exact request before any graph write");
  for(unsigned field=0;field<2;++field){
   auto binding=f.issuer->binding();(field?binding.policy_snapshot_uuid:binding.database_uuid)=Id(20550);
   scratchbird::core::uuid::StandaloneUuidV7Issuer wrong_issuer(binding,{{},0,1000});
   const auto crossed=db::PublishNativeInventoryOnLease(*held.lease,record,inventory,f.budget,wrong_issuer);
   Check(crossed.error==db::NativePublicationError::request_mismatch&&!crossed.snapshot&&f.Read(0,256)==pending,
     "node and policy issuer bindings must match actual lease and owning record");
  }
  const auto short_budget=db::PublishNativeInventoryOnLease(*held.lease,record,inventory,0,*f.issuer);
  Check(short_budget.error==db::NativePublicationError::resource_exhausted&&!short_budget.snapshot&&f.Read(0,256)==pending,
    "owned constructor resource failure has no write or receipt");
  entropy_fault=1;const auto no_identity=db::PublishNativeInventoryOnLease(*held.lease,record,inventory,f.budget,*f.issuer);
  Check(!entropy_fault&&no_identity.error==db::NativePublicationError::identity_failure&&!no_identity.snapshot&&f.Read(0,256)==pending,
    "owned constructor entropy failure never manufactures identity or publication");
  if(!phase) {
   auto skipped=inventory;skipped.entries[1].state=mga::TransactionState::active;
   const auto refused=db::PublishNativeInventoryOnLease(*held.lease,record,skipped,f.budget,*f.issuer);
   Check(!refused.ok()&&!refused.snapshot&&f.Read(0,256)==pending,"owned constructor cannot bypass durable starting");
  }
  auto unsorted=inventory;std::reverse(unsorted.entries.begin(),unsorted.entries.end());
  const auto published=db::PublishNativeInventoryOnLease(*held.lease,record,unsorted,f.budget,*f.issuer);
  if(!published.ok())std::cerr<<"owned publication profile="<<profile<<" phase="<<phase<<" error="<<int(published.error)<<'\n';
  Check(published.ok()&&published.snapshot->selection.selection_generation==before.snapshot->selection.selection_generation+1,
    "production constructor actually selects its generated graph");
  Check(published.effects==held.lease->effects()&&published.effects.observed&&
    published.effects.write_attempts&&published.effects.confirmed_bytes==published.effects.attempted_bytes&&
    published.effects.successful_syncs==published.effects.sync_attempts&&!published.effects.uncertain_write&&
    published.effects.installed_graph_verified&&published.effects.selector_write_attempted&&published.effects.selected_graph_verified,
    "successful publication retains complete lease I/O and distinct graph verification outcomes");
  const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(selected.ok()&&SameOwnedInventory(selected.checkpoint_inventory.inventory,inventory),
    "ordinary selected admission sees exact generated inventory");
  if(startup){const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
    Check(history.ok()&&history.entries.size()==phase+1,"startup exact retained publication history");
    for(const auto& entry:history.entries)Check(entry.plan.intent.startup_binding==intent.startup_binding&&
      entry.record.normalized_request_bytes==record.normalized_request_bytes,"startup binding and full request survive subsequent publication phases");
    auto next=record;const auto absent=db::ValidateNativeManagementHistoryAppend(history,next,f.budget);
    Check(absent==db::NativeManagementHistoryError::history_mismatch,"bare replay cannot omit startup binding");
    for(unsigned field=0;field<5;++field){auto changed=intent.startup_binding;auto& b=*changed;
      if(!field)b.operation_uuid=Id(21600);if(field==1)b.session_uuid=Id(21600);if(field==2)b.transaction_uuid=Id(21600);
      if(field==3)++b.local_transaction_id;if(field==4)++b.fence_generation;
      Check(db::ValidateNativeManagementHistoryAppend(history,next,f.budget,changed)==db::NativeManagementHistoryError::history_mismatch,
        "retained history cannot rebind startup tuple or fence generation");}
    Check(db::ValidateNativeManagementHistoryAppend(history,next,f.budget,intent.startup_binding)==db::NativeManagementHistoryError::none,
      "exact startup revision replay remains permitted");}
  for(const auto& root:initial.checkpoint_inventory.checkpoint->roots)if(root.role==3&&mixed){
   const auto old=page::DecodeNativeFilespaceDirectory(f.Read(root.page.page_number));Check(old.ok()&&!old.directory->next,"independent original mixed directory");
   const auto found=std::find_if(selected.checkpoint_inventory.checkpoint->roots.begin(),selected.checkpoint_inventory.checkpoint->roots.end(),[](const auto& r){return r.role==3;});Check(found!=selected.checkpoint_inventory.checkpoint->roots.end(),"new directory root retained");
   const auto raw=f.Read(found->page.page_number);const auto actual=page::DecodeNativeFilespaceDirectory(raw);Check(actual.ok()&&Sha(raw)==found->sha256&&Self(actual.directory->header)==found->page&&
     actual.directory->header.page_generation==published.snapshot->watermark.watermark&&actual.directory->header.page_uuid!=old.directory->header.page_uuid,"actual new directory image and reserved page identity");
   auto expected=*old.directory;expected.header=actual.directory->header;expected.directory_generation=published.snapshot->watermark.watermark;expected.creator_transaction_uuid={};expected.creator_local_transaction_id=0;expected.creator_operation_uuid=Id(20100+phase);
   const auto& head=selected.allocation.pages.front();for(auto& member:expected.records)if(member.bootstrap.filespace_uuid==Id(2))member.allocation_root=page::NativeFilespaceAllocationRoot{Self(head.map->header),head.map->object_uuid,Sha(head.bytes),head.map->map_generation,head.map->capacity_generation};
   Check(raw==DirectoryImageOracle(expected),"independent complete mixed directory transition preserves all other member fields");
  }else if(root.role!=1&&root.role!=4&&root.role!=16)
   Check(std::find(selected.checkpoint_inventory.checkpoint->roots.begin(),selected.checkpoint_inventory.checkpoint->roots.end(),root)!=
     selected.checkpoint_inventory.checkpoint->roots.end(),"owned constructor preserves every noninventory nonallocation root");
  for(std::size_t n=0;n<initial.allocation.pages.size();++n)for(const auto& old:initial.allocation.pages[n].map->records) {
   const auto& records=selected.allocation.pages[n].map->records;
   const auto found=std::find_if(records.begin(),records.end(),[&](const auto& r){return r.page_number==old.page_number;});
   Check(found!=records.end()&&*found==old,"owned constructor preserves original retained allocation bytes");
  }
  const auto after=f.Read(0,256);
  const auto retry=db::PublishNativeInventoryOnLease(*held.lease,record,inventory,f.budget,*f.issuer);
  Check(retry.effects==published.effects,"rejected replay preserves original effects without claiming new writes");
  Check(retry.error==db::NativePublicationError::operation_pending&&!retry.snapshot&&f.Read(0,256)==after,
    "anchored constructor never substitutes a freshly generated graph");
  held.lease.reset();
 }
 Check(f.device.Close().ok()&&(!mixed||f.secondary.Close().ok()),"close production-generated node before cold observation");
 const auto child=fork();Check(child>=0,"owned constructor independent observer");
 if(!child) {
  d::FileDevice file,second;if(!file.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok())_exit(80);
  std::vector<d::NativeFilespaceDevice> files{{Id(2),zero.record->bootstrap.page_size_profile_uuid,&file}};
  if(mixed){if(!second.Open((f.path.parent_path()/"secondary").string(),d::FileOpenMode::open_existing_read_only).ok())_exit(82);
    files.push_back({Id(16000),d::kCanonicalFilespacePageProfiles[(profile+1)%5].uuid,&second});}
  const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),files,Id(2),f.budget);
  if(startup){const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),files,Id(2),f.budget);
    if(!history.ok()||history.entries.size()!=3)_exit(83);
    for(const auto& entry:history.entries)if(!entry.plan.intent.startup_binding||
      entry.plan.intent.startup_binding->operation_uuid!=Id(20000)||entry.plan.intent.startup_binding->session_uuid!=Id(21000)||
      entry.plan.intent.startup_binding->transaction_uuid!=Id(19000)||entry.plan.intent.startup_binding->fence_generation!=19||
      entry.record.normalized_request_bytes.size()!=64)_exit(84);}
  _exit(selected.ok()&&selected.selection->selection_generation==4&&SameOwnedInventory(selected.checkpoint_inventory.inventory,inventory)?0:81);
 }
 int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
   "independent process admits production-generated starting and activation graph");
 if(mixed){Check(f.secondary.Open((f.path.parent_path()/"secondary").string(),d::FileOpenMode::open_existing_read_only).ok(),"reopen preserved secondary");
  Bytes actual(f.secondary_before.size());const auto read=f.secondary.ReadAt(0,actual.data(),actual.size());
  Check(read.ok()&&read.bytes_transferred==actual.size()&&actual==f.secondary_before,"owned constructor preserves every secondary byte");}
 std::cout<<"owned inventory profile="<<profile<<" read_only="<<read_only<<" mixed="<<mixed<<" startup="<<startup<<" PASS\n";
}
struct OwnedInventoryRequest {
 mga::LocalTransactionInventory inventory;
 db::NativeManagementOperation record;
 db::NativePublicationIntent intent;
 db::NativePublicationSnapshot pending;
 db::NativePublicationReservation held;
 explicit OwnedInventoryRequest(Fixture& f,unsigned sequence=0,bool original_context=false,bool startup=false,bool activation=false) {
  const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(selected.ok(),"owned failure fixture actual selected inventory");
  auto begun=mga::BeginLocalTransaction(selected.checkpoint_inventory.inventory,{UuidKind::transaction,Id(19000+sequence)},3000);
  Check(begun.ok(),"owned failure fixture actual BEGIN");inventory=std::move(begun.inventory);
  inventory.entries.back().state=mga::TransactionState::created;
  const auto zero=d::ReadFilespacePageZeroFromOpenDevice(f.device);Check(zero.ok(),"owned failure fixture bootstrap");
  record=records::Example(1);record.uuid=Id(20000+sequence);record.bootstrap_uuid=zero.record->page_uuid;
  record.security_snapshot_uuid={};record.generation_guards={};record.idempotency_key="owned-failure";
  if(sequence){record.idempotency_key+=std::to_string(sequence);if(!original_context)record.request_context_uuid=Id(21000+sequence);}
  if(startup){record.normalized_request_bytes=Bytes(64);
    Put(record.normalized_request_bytes,0,record.uuid);Put(record.normalized_request_bytes,16,record.request_context_uuid);
    Put(record.normalized_request_bytes,32,inventory.entries.back().identity.transaction_uuid.value);
    Num(record.normalized_request_bytes,48,8,inventory.entries.back().identity.local_id.value);Num(record.normalized_request_bytes,56,8,19);
    record.normalized_request_sha256=Sha(record.normalized_request_bytes);}
  intent={record.initiator_uuid,record.request_context_uuid,record.policy_snapshot_uuid,
    record.normalized_request_sha256,record.initiator_kind};intent.recovery_profile=2;
  if(startup)intent.startup_binding=db::NativeStartupBinding{record.uuid,record.request_context_uuid,
    inventory.entries.back().identity.transaction_uuid.value,inventory.entries.back().identity.local_id.value,19};
  const auto before=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(before.ok(),"owned failure fixture actual base");
  held=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*before.snapshot,Id(20100+sequence),f.budget,&intent);
  Check(held.ok(),"owned failure fixture actual reservation");pending=held.lease->snapshot();
  if(activation){Check(startup&&db::PublishNativeInventoryOnLease(*held.lease,record,inventory,f.budget,*f.issuer).ok(),"durable startup starting before activation fault fixture");
    held.lease.reset();inventory.entries.back().state=mga::TransactionState::active;
    record.revision=2;record.state=db::NativeManagementState::authorized;record.updated_at=Id(2001);
    const auto authorized=records::Example(2);record.security_snapshot_uuid=authorized.security_snapshot_uuid;record.generation_guards=authorized.generation_guards;
    const auto actual=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);Check(actual.ok(),"startup selected starting base");
    held=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*actual.snapshot,Id(20120),f.budget,&intent);
    Check(held.ok(),"startup activation retains original operation binding");pending=held.lease->snapshot();}
 }
};
void OwnedDirectoryInventory(unsigned primary){
 for(unsigned secondary=0;secondary<5;++secondary)for(bool reverse:{false,true}){
  DirectoryHistoryFixture f(primary,secondary,reverse,2,false,true,true,false);auto& t=f.t;
  f.budget=t.fixture.budget=16*1024*std::max<u64>(t.fixture.size,d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes);
  const auto read=[&](const Uuid& fs){const auto& zero=t.zeros.at(fs==Id(11)?t.other:fs);Bytes raw(zero.total_pages*u64{zero.bootstrap.page_size_bytes});
    const auto io=f.File(fs)->ReadAt(0,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size(),"independent unchanged member snapshot");return raw;};
  const auto secondary_before=read(t.other),unchanged_before=read(Id(11));
  for(unsigned repeat=0;repeat<2;++repeat){
    OwnedInventoryRequest request(t.fixture,repeat);const auto result=db::PublishNativeInventoryOnLease(*request.held.lease,request.record,request.inventory,f.budget,*t.fixture.issuer);
    if(!result.ok())std::cerr<<"owned directory primary="<<primary<<" secondary="<<secondary<<" repeat="<<repeat<<" error="<<int(result.error)<<'\n';
    Check(result.ok()&&result.effects.installed_graph_verified&&result.effects.selected_graph_verified,"production assembler publishes complete actual directory-bearing inventory");
    request.held.lease.reset();const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);
    Check(selected.ok()&&SameOwnedInventory(selected.checkpoint_inventory.inventory,request.inventory),"actual directory publication retains complete inventory");
    const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);
    Check(history.ok()&&history.entries.size()==repeat+1&&!history.entries.back().control_directory_images.empty(),"production assembly carries retained canonical directory images");
    const auto& cp=*selected.checkpoint_inventory.checkpoint;const auto root=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==3;});Check(root!=cp.roots.end(),"actual published directory root");
    const auto& p=root->page;const auto directory=page::ReadNativeFilespaceDirectoryFromOpenDevices(Id(1),t.fixture.devices,{5,9,p.filespace_uuid,p.page_number,p.page_generation,p.page_size_profile_uuid,root->object_uuid},f.budget);
    Check(directory.ok(),"actual production-built directory reads");std::size_t members=0;
    for(const auto& image:directory.pages){Check(image.bytes==DirectoryImageOracle(*image.directory),"independent complete directory byte oracle");
      for(const auto& member:image.directory->records){++members;auto expected=*std::find_if(t.old_directory.records.begin(),t.old_directory.records.end(),[&](const auto& r){return r.bootstrap.filespace_uuid==member.bootstrap.filespace_uuid;});
        if(member.bootstrap.filespace_uuid==Id(2)){const auto& head=selected.allocation.pages.front();expected.allocation_root=page::NativeFilespaceAllocationRoot{Self(head.map->header),head.map->object_uuid,Sha(head.bytes),head.map->map_generation,head.map->capacity_generation};}
        auto oracle=*image.directory;oracle.records={expected};oracle.total_records=1;oracle.first_record=0;oracle.next.reset();oracle.next_sha256={};auto actual=oracle;actual.records={member};
        Check(DirectoryImageOracle(actual)==DirectoryImageOracle(oracle),"every unchanged directory field and exact primary allocation descriptor");}}
    Check(members==3&&read(t.other)==secondary_before&&read(Id(11))==unchanged_before,"all unrelated files preserved by actual primary assembler");
  }
  f.Reopen();Check(db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget).ok(),"cold admission of repeated production-built directory graph");
 }
}
void OwnedDirectoryRepacking(unsigned profile){
 DirectoryHistoryFixture f(profile,profile,true,2,false,true,true,false);auto& t=f.t;
 f.budget=t.fixture.budget=32768*t.fixture.size;
 std::vector<std::unique_ptr<d::FileDevice>> extra_files;const auto per_page=(t.fixture.size-384)/320;
 // Two actual source pages: the first deliberately holds three records, so the
 // constructor must repack rather than reproduce caller-selected boundaries.
 for(unsigned n=0;n<per_page;++n){auto zero=t.zeros.at(t.other);zero.bootstrap.filespace_uuid=Id(40000+n*8);zero.page_uuid=Id(40001+n*8);
  const auto& p=d::kCanonicalFilespacePageProfiles[n%5];zero.bootstrap.page_size_profile_uuid=p.uuid;zero.bootstrap.page_size_bytes=p.page_size_bytes;
  zero.total_pages=2;zero.free_pages=0;zero.roots.front()={3,3,zero.bootstrap.filespace_uuid,1,1,p.uuid,Id(40002+n*8)};
  auto map=t.before.at(t.other).front();map.header={p.page_size_bytes,3,Id(1),zero.bootstrap.filespace_uuid,Id(40003+n*8),1,1,0,p.uuid};map.object_uuid=Id(40002+n*8);map.total_pages=2;map.states.resize(2);map.records.resize(2);
  for(auto& r:map.records){r.allocation_uuid=Id(40004+n*8+r.page_number);r.owner_uuid=r.page_number?map.object_uuid:zero.bootstrap.filespace_uuid;r.page_uuid=r.page_number?map.header.page_uuid:zero.page_uuid;}
  auto file=std::make_unique<d::FileDevice>();Check(file->Open((t.fixture.path.parent_path()/std::to_string(n)).string(),d::FileOpenMode::create_new).ok(),"real repacking member");
  const auto z=d::EncodeFilespacePageZero(zero);const auto m=MapOracle(map);Check(z.ok()&&file->WriteAt(0,z.bytes->data(),z.bytes->size()).ok()&&file->WriteAt(p.page_size_bytes,m.data(),m.size()).ok()&&file->Sync().ok(),"complete actual repacking member bytes");
  t.fixture.devices.push_back({zero.bootstrap.filespace_uuid,p.uuid,file.get()});extra_files.push_back(std::move(file));
  page::NativeFilespaceDirectoryRecord member;member.bootstrap=zero.bootstrap;member.page_zero_uuid=zero.page_uuid;member.locator_uuid=Id(40006+n*8);member.page_zero_generation=zero.page_generation;member.root_set_generation=zero.root_set_generation;member.total_pages=2;
  member.allocation_root=page::NativeFilespaceAllocationRoot{Self(map.header),map.object_uuid,Sha(m),map.map_generation,map.capacity_generation};t.old_directory.records.push_back(member);
 }
 std::sort(t.old_directory.records.begin(),t.old_directory.records.end(),[](const auto& a,const auto& b){return a.bootstrap.filespace_uuid<b.bootstrap.filespace_uuid;});
 const auto member_count=t.old_directory.records.size();t.old_directory.total_records=member_count;
 auto tail=t.old_directory;tail.header.page_uuid=Id(50000);tail.header.page_number=150;tail.first_record=3;tail.records.erase(tail.records.begin(),tail.records.begin()+3);tail.next.reset();tail.next_sha256={};
 t.Add(t.before,tail.header,tail.object_uuid,{});const auto maps=t.MapsImages(t.before);t.BindMembers(t.old_directory,t.before);
 tail.records.assign(t.old_directory.records.begin()+3,t.old_directory.records.end());
 auto head=t.old_directory;head.records.resize(3);head.next=Self(tail.header);const auto tail_bytes=DirectoryImageOracle(tail);head.next_sha256=Sha(tail_bytes);const auto head_bytes=DirectoryImageOracle(head);
 Check(page::EncodeNativeFilespaceDirectory(head).bytes==head_bytes&&page::EncodeNativeFilespaceDirectory(tail).bytes==tail_bytes,"independent complete multi-page source directory");
 for(const auto& raw:maps)f.Write(raw);f.Write(tail_bytes);f.Write(head_bytes);
 auto cp=t.graph.base;for(auto& root:cp.roots){if(root.role==3)root.sha256=Sha(head_bytes);if(root.role==4)root.sha256=Sha(MapOracle(t.before.at(Id(2)).front()));}
 const auto encoded=db::EncodeNativeCheckpointRoot(cp);Check(encoded.ok(),"directory repacking base checkpoint");f.Write(encoded.bytes);const auto digest=Sha(encoded.bytes);
 for(const auto& root:t.graph.zero.roots){if(root.kind==18||root.kind==19){auto selection=*db::DecodeNativeCheckpointSelection(t.fixture.Read(root.page_number)).selection;selection.checkpoint_sha256=digest;f.Write(SelectorOracle(selection));}
  if(root.kind==20||root.kind==21){auto watermark=*db::DecodeNativePublicationWatermark(t.fixture.Read(root.page_number)).state;watermark.base_checkpoint_sha256=digest;const auto image=db::EncodeNativePublicationWatermark(watermark);Check(image.ok(),"repacking original watermark");f.Write(image.bytes);}}
 Check(t.fixture.device.Sync().ok(),"complete independent source directory barrier");OwnedInventoryRequest request(t.fixture);
 const auto result=db::PublishNativeInventoryOnLease(*request.held.lease,request.record,request.inventory,f.budget,*t.fixture.issuer);
 if(!result.ok())std::cerr<<"directory repacking profile="<<profile<<" error="<<int(result.error)<<'\n';
 Check(result.ok(),"production assembler repacks a complete actual directory");request.held.lease.reset();
 const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);Check(history.ok()&&history.entries.size()==1,"repacked graph independently selected");
 const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);Check(selected.ok(),"repacked primary allocation independently read");
 const auto& first_map=selected.allocation.pages.front();const page::NativeFilespaceAllocationRoot expected_primary{Self(first_map.map->header),first_map.map->object_uuid,Sha(first_map.bytes),first_map.map->map_generation,first_map.map->capacity_generation};
 const auto& images=history.entries.front().control_directory_images;Check(images.size()==2,"full directory count requires exactly two production pages");
 std::size_t covered=0;for(const auto& raw:images){const auto decoded=page::DecodeNativeFilespaceDirectory(raw);Check(decoded.ok()&&raw==DirectoryImageOracle(*decoded.directory),"independent repacked directory bytes");
  const auto& dir=*decoded.directory;Check(dir.first_record==covered&&dir.total_records==member_count&&dir.records.size()==std::min<std::size_t>(per_page,member_count-covered),"exact full-page then remainder packing");
  for(const auto& member:dir.records){auto expected=t.old_directory.records[covered++];if(member.bootstrap.filespace_uuid==Id(2))expected.allocation_root=expected_primary;
    auto a=dir,b=dir;a.records={member};b.records={expected};a.total_records=b.total_records=1;a.first_record=b.first_record=0;a.next.reset();b.next.reset();a.next_sha256=b.next_sha256={};Check(DirectoryImageOracle(a)==DirectoryImageOracle(b),"every retained member survives repacking");}}
 Check(covered==member_count,"no omitted directory members");
}
u64 DenseCapacity(unsigned profile,bool dense){return dense?216+(d::kCanonicalFilespacePageProfiles[profile].page_size_bytes-384)/128:256;}
void PublishCompactAllocationBase(Fixture& f){
 Graph g(f);auto compact=g.after.front();compact.states.clear();compact.records.clear();
 for(const auto& map:g.after){compact.states.insert(compact.states.end(),map.states.begin(),map.states.end());
  compact.records.insert(compact.records.end(),map.records.begin(),map.records.end());}
 for(std::size_t i=1;i<g.after.size();++i){const auto n=g.after[i].header.page_number;compact.states[n]=State::free;
  compact.records.erase(std::remove_if(compact.records.begin(),compact.records.end(),[&](const auto& r){return r.page_number==n;}),compact.records.end());}
 compact.next.reset();compact.next_sha256={};g.after={std::move(compact)};
 const auto old=db::DecodeNativeManagementExtent(g.extent,*g.plan.management_extent,Id(1),g.zero.page_uuid,f.budget);
 Check(old.ok(),"compact map original management extent");auto record=*old.record;record.idempotency_key="compact-map-base";
 record.normalized_request_sha256.fill(0x38);g.plan.intent.normalized_request_sha256=record.normalized_request_sha256;
 const auto extent=db::EncodeNativeManagementExtent(record,g.plan.management_extent->object_uuid,old.page_headers,f.budget);
 Check(extent.ok(),"distinct compact map management operation");g.extent=extent.pages;g.plan.management_extent=extent.root;
 g.Refresh();Bundle b(g);g.plan.control_bundle=b.root;
 g.plan.intent.recovery_profile=1;g.plan.base_selection_generation=1;g.Refresh();
 const auto before=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);
 Check(before.ok(),"compact map actual original selection");
 auto held=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*before.snapshot,g.plan.operation_uuid,f.budget,&g.plan.intent);
 Check(held.ok(),"compact map actual reservation");g.plan.reservation_state_sha256=held.lease->snapshot().state_sha256;g.Refresh();
 const auto installed=db::InstallNativeManagementControlGraphOnLease(*held.lease,g.plan,g.target_bytes,g.extent,b.pages,f.budget);
 if(!installed.ok())std::cerr<<"compact install error="<<unsigned(installed.error)<<'\n';
 Check(installed.ok()&&db::PublishNativeManagementControlGraphOnLease(*held.lease,f.budget).ok(),"actual compact map graph and selectors published");held.lease.reset();
 const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
 Check(selected.ok()&&selected.allocation.pages.size()==1&&selected.allocation.pages.front().bytes==MapOracle(*selected.allocation.pages.front().map),
  "independent complete compact map bytes before dense mutation");
}
struct OwnedPreallocationRequest {
 db::NativeStorageActionIntent storage;
 db::NativeManagementOperation record;
 db::NativePublicationIntent intent;
 db::NativePublicationReservation held;
 explicit OwnedPreallocationRequest(Fixture& f,unsigned ordinal=0,unsigned reused=0,bool dense=false,Uuid target=Id(2),u64 range_count=0,bool growth=false,bool reserve=true) {
  if(dense)PublishCompactAllocationBase(f);
  const auto before=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(before.ok(),"preallocation actual base");const auto& s=before.snapshot->selection;
  const d::FilespaceRootReference root{9,0x300,Id(2),s.checkpoint.page_number,s.checkpoint.page_generation,s.checkpoint.page_size_profile_uuid,s.checkpoint_object_uuid};
  const auto capacity=db::ReadNativeFilespaceCapacityFromOpenDevices(Id(1),f.devices,root,target,f.budget);
  if(!capacity.ok())std::cerr<<"preallocation capacity error="<<unsigned(capacity.error)<<" checkpoint="<<unsigned(capacity.checkpoint_error)<<" directory="<<unsigned(capacity.directory_error)
    <<" allocation="<<unsigned(capacity.allocation_error)<<" management="<<unsigned(capacity.management_error)<<" bootstrap="<<unsigned(capacity.bootstrap_error)<<'\n';
  Check(capacity.ok(),"preallocation actual capacity");const auto& c=*capacity.observation;
  storage.request_uuid=Id(21000+(reused==2?0:ordinal*1000));
  storage.operation_uuid=Id(21001+(reused==1?0:ordinal*1000));storage.database_uuid=c.database_uuid;
  storage.filespace_uuid=c.filespace_uuid;storage.locator_uuid=c.locator_uuid;storage.page_zero_uuid=c.page_zero_uuid;
  storage.page_size_profile_uuid=c.page_size_profile_uuid;storage.page_size_bytes=c.page_size_bytes;
  storage.policy_snapshot_uuid=Id(10);storage.storage_profile_uuid=Id(21002);storage.initiator_uuid=Id(8);storage.request_context_uuid=Id(9);
  storage.policy_uuid=Id(21003);storage.policy_version_uuid=Id(21004);storage.attachment_uuid=Id(21005);
  storage.attachment_version_uuid=Id(21006);storage.storage_profile_version_uuid=Id(21007);
  storage.attachment_generation=storage.storage_profile_generation=1;
  storage.checkpoint=c.checkpoint;storage.allocation_root=c.allocation_root;
  storage.checkpoint_sha256=c.checkpoint_sha256;storage.allocation_sha256=c.allocation_sha256;
  storage.checkpoint_generation=c.checkpoint_generation;storage.checkpoint_root_set_generation=c.checkpoint_root_set_generation;
  storage.directory_generation=c.directory_generation;storage.filespace_root_set_generation=c.filespace_root_set_generation;
  storage.page_zero_generation=c.page_zero_generation;storage.map_generation=c.map_generation;storage.capacity_generation=c.capacity_generation;
  storage.current_total_pages=storage.maximum_total_pages=c.total_pages;storage.first_page=200+(range_count?range_count:8)*ordinal;storage.page_count=range_count?range_count:dense?(f.size-384)/128:4;
  storage.maximum_work_bytes=storage.page_count*u64{c.page_size_bytes};storage.maximum_retained_image_bytes=f.budget;
  storage.action=db::NativeStorageAction::page_preallocation;storage.intended_state=db::NativeStorageIntentState::preallocated;
  storage.allocation_owner_uuid=Id(21008+ordinal*1000);storage.allocation_page_type=1;
  if(growth){storage.action=db::NativeStorageAction::physical_growth;storage.first_page=c.total_pages;storage.maximum_total_pages=c.total_pages+storage.page_count;
    if(!reserve){storage.intended_state=db::NativeStorageIntentState::free;storage.allocation_owner_uuid={};storage.allocation_page_type=0;}}
  const auto encoded=db::EncodeNativeStorageActionIntent(storage,f.budget);Check(encoded.ok(),"complete preallocation request");
  record=records::Example(1);record.uuid=storage.operation_uuid;record.bootstrap_uuid=s.bootstrap_uuid;
  if(ordinal)record.idempotency_key+="-distinct-request";
  record.target_uuid=storage.filespace_uuid;record.security_snapshot_uuid={};record.generation_guards={};
  record.normalized_request_bytes=encoded.bytes;record.normalized_request_sha256=Sha(encoded.bytes);
  intent={record.initiator_uuid,record.request_context_uuid,record.policy_snapshot_uuid,record.normalized_request_sha256,record.initiator_kind,u16(growth?4:3)};
  held=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*before.snapshot,Id(21100+ordinal*1000),f.budget,&intent);
  Check(held.ok(),"durable preallocation generation");
 }
};
void OwnedAppendAdmission(unsigned primary,unsigned secondary){
 for(bool inventory:{false,true})for(bool target:{false,true}){
  DirectoryHistoryFixture fixture(primary,secondary,true,inventory?2:3,target,true,true,false);auto& f=fixture.t.fixture;
  f.budget=32768*std::max<u64>(f.size,d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes);
  const auto snapshot=[&]{std::map<Uuid,Bytes> images;for(const auto& file:f.devices){const auto size=file.device->Size();Check(size.ok(),"append admission actual extent");Bytes raw(size.size_bytes);
    const auto io=file.device->ReadAt(0,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size(),"append admission complete bytes");images.emplace(file.filespace_uuid,std::move(raw));}return images;};
  const auto exercise=[&](auto& first,auto& next,auto&& publish){
    const auto original=next.record;const auto before=snapshot();
    for(unsigned fault=0;fault<2;++fault){next.record=original;
      if(fault==0)next.record.idempotency_key=first.record.idempotency_key;
      else next.record.revision=2;
      reads=writes=syncs=preallocation_calls=0;preallocation_fault=1;io_counting=true;const auto refused=publish(next);io_counting=false;preallocation_fault=0;
      const auto error=[&]{if constexpr(requires{refused.error;})return refused.error;else return refused.publication.error;}();
      Check(!refused.ok()&&error==db::NativePublicationError::request_mismatch&&!writes&&!syncs&&!preallocation_calls&&snapshot()==before,
        "conflicting retained key or noninitial revision refuses before metadata or physical effects");
      const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
      Check(history.ok()&&history.entries.size()==1&&history.entries.front().record.uuid==first.record.uuid,
        "refusal preserves the independently readable original selected history");
    }
    next.record=original;Check(publish(next).ok(),"original valid retained request still publishes after rejected appends");
    const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
    Check(history.ok()&&history.entries.size()==2&&history.entries.back().record.uuid==original.uuid,
      "successful retry publishes the original binary operation identity");
  };
  if(inventory){
    const auto publish=[&](auto& request){return db::PublishNativeInventoryOnLease(*request.held.lease,request.record,request.inventory,f.budget,*f.issuer);};
    OwnedInventoryRequest first(f);Check(publish(first).ok(),"actual initial inventory publication for append conflict");first.held.lease.reset();
    OwnedInventoryRequest next(f,1);exercise(first,next,publish);
    next.held.lease.reset();OwnedInventoryRequest duplicate(f,2,true);const auto before=snapshot();
    reads=writes=syncs=0;io_counting=true;const auto refused=publish(duplicate);io_counting=false;
    Check(!refused.ok()&&refused.error==db::NativePublicationError::request_mismatch&&!writes&&!syncs&&snapshot()==before,"same semantic identity cannot be reassigned by supplying a distinct idempotency key");
    duplicate.record.descriptor_uuid=Id(23002);Check(publish(duplicate).ok(),"distinct descriptor admits a genuinely distinct management request");
  }else{
    const auto publish=[&](auto& request){return db::PublishNativePreallocationOnLease(*request.held.lease,request.record,f.budget,*f.issuer);};
    OwnedPreallocationRequest first(f,0,0,false,fixture.t.changed);Check(publish(first).ok(),"actual initial preallocation publication for append conflict");first.held.lease.reset();
    OwnedPreallocationRequest next(f,1,0,false,fixture.t.changed);exercise(first,next,publish);
  }
  fixture.Reopen();Check(db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),f.budget).ok(),"cold history after append refusal and genuine retry");
 }
 std::cout<<"owned append admission primary="<<primary<<" secondary="<<secondary<<" PASS\n";
}
void OwnedGrowthPublication(unsigned primary,unsigned secondary,bool subdivide=false){
 for(bool reverse:{false,true})for(bool target:{false,true})for(bool reserve:{false,true}){
  DirectoryHistoryFixture f(primary,secondary,reverse,4,target,reserve,true,false);auto& t=f.t;
  f.budget=t.fixture.budget=32768*std::max<u64>(t.fixture.size,d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes);
  const auto all=[&](d::FileDevice* file){const auto size=file->Size();Check(size.ok(),"growth original real extent");Bytes raw(size.size_bytes);
    const auto io=file->ReadAt(0,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size(),"complete independent growth file snapshot");return raw;};
  std::optional<db::NativeManagementOperation> previous;
  for(unsigned ordinal=0;ordinal<2;++ordinal){const auto target_size=target?d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes:t.fixture.size;
   const u64 count=subdivide?(target_size-384)/128+1:4;
   OwnedPreallocationRequest request(t.fixture,ordinal,0,false,t.changed,count,true,reserve);
   std::map<Uuid,Bytes> expected;std::map<Uuid,u64> original_sizes;
   for(const auto& file:t.fixture.devices){auto raw=all(file.device);original_sizes.emplace(file.filespace_uuid,raw.size());expected.emplace(file.filespace_uuid,std::move(raw));}
   const auto before=d::DecodeFilespacePageZero(expected.at(t.changed).data(),target_size);Check(before.ok(),"original actual growth body");
   const auto short_budget=db::PublishNativeFilespaceGrowthOnLease(*request.held.lease,request.record,1,*t.fixture.issuer);
   Check(short_budget.error==db::NativePublicationError::resource_exhausted&&!short_budget.physical_attempted&&!short_budget.physical,"growth construction short budget cannot cause physical work");
   for(const auto& file:t.fixture.devices)Check(all(file.device)==expected.at(file.filespace_uuid),"short-budget growth retains every actual byte");
   preallocation_fd=-1;const auto result=db::PublishNativeFilespaceGrowthOnLease(*request.held.lease,request.record,f.budget,*t.fixture.issuer);
   if(!result.ok())std::cerr<<"owned growth primary="<<primary<<" secondary="<<secondary<<" target="<<target<<" reserve="<<reserve<<" ordinal="<<ordinal<<" error="<<unsigned(result.error)
     <<" preparation="<<unsigned(result.preparation.error)<<" selection="<<unsigned(result.selection.error)<<'\n';
   Check(result.ok()&&result.request_uuid==request.storage.request_uuid&&result.operation_uuid==request.storage.operation_uuid&&
     result.publication_attempt_uuid==Id(21100+ordinal*1000)&&result.physical_attempted&&result.physical->logical_size_extended&&
     result.physical_sync_attempted&&result.page_zero->original_preimage_verified&&result.selection.effects.selected_graph_verified,
     "owning publisher actually extends the original target and selects complete native growth");
   struct stat requested{},actual{};const auto path=target?f.secondary_path:t.fixture.path;
   Check(stat(path.c_str(),&requested)==0&&preallocation_fd>=0&&fstat(preallocation_fd,&actual)==0&&requested.st_dev==actual.st_dev&&requested.st_ino==actual.st_ino&&
     preallocation_offset==request.storage.first_page*u64{target_size}&&preallocation_bytes==count*target_size,"growth physical call targets exact actual member and byte stride");
   reads=writes=syncs=0;io_counting=true;const auto duplicate=db::PublishNativeFilespaceGrowthOnLease(*request.held.lease,request.record,f.budget,*t.fixture.issuer);io_counting=false;
   Check(duplicate.error==db::NativePublicationError::stale_base&&!duplicate.physical_attempted&&!writes&&!syncs,"consumed growth lease cannot repeat physical work");
   request.held.lease.reset();const auto reconciled=db::ReconcileNativeFilespaceGrowthFromOpenDevices(Id(1),t.fixture.devices,Id(2),request.record,f.budget);
   Check(reconciled.ok()&&reconciled.observation->disposition==db::NativePreallocationDisposition::selected&&reconciled.observation->publication_attempt_uuid==result.publication_attempt_uuid,"growth retains original request and publication identities");
   const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);Check(history.ok()&&history.entries.size()==ordinal+1,"complete actual owned growth history");const auto& entry=history.entries.back();
   Check(entry.control_growth_images.size()==2&&entry.control_growth_images.front()==Bytes(expected.at(t.changed).begin(),expected.at(t.changed).begin()+target_size),"retained exact original growth body");
   auto& target_image=expected.at(t.changed);target_image.resize((request.storage.current_total_pages+count)*target_size);
   std::copy(entry.control_growth_images.back().begin(),entry.control_growth_images.back().end(),target_image.begin());
   const auto overlay=[&](const Bytes& bytes){const auto h=d::DecodeNativeCommonPageHeader(bytes.data(),128);Check(h.ok(),"growth oracle artifact identity");auto& file=expected.at(h.header->filespace_uuid);const auto offset=h.header->page_number*u64{bytes.size()};
     const auto original_size=original_sizes.at(h.header->filespace_uuid);
     Check(offset<=original_size&&bytes.size()<=original_size-offset,"growth control placement remains in original file capacity");std::copy(bytes.begin(),bytes.end(),file.begin()+offset);};
   u64 free=0,preallocated=0,covered=0;unsigned target_maps=0;
   for(const auto& bytes:entry.control_allocation_images){const auto decoded=page::DecodeNativeAllocationMap(bytes);Check(decoded.ok()&&bytes==MapOracle(*decoded.map),"independent complete owned growth map encoding");const auto& map=*decoded.map;
    if(map.header.filespace_uuid==t.changed){++target_maps;Check(map.total_pages==request.storage.current_total_pages+count&&map.capacity_generation==request.storage.capacity_generation+1,"only target capacity advances exactly once");
     free+=std::count(map.states.begin(),map.states.end(),State::free);preallocated+=std::count(map.states.begin(),map.states.end(),State::preallocated);
     for(u64 n=std::max(map.first_page,request.storage.first_page);n<map.first_page+map.states.size()&&n<request.storage.first_page+count;++n){++covered;
      Check(map.states[n-map.first_page]==(reserve?State::preallocated:State::free),"complete requested suffix state");
      const auto record=std::find_if(map.records.begin(),map.records.end(),[&](const auto& r){return r.page_number==n;});
      if(reserve)Check(record!=map.records.end()&&record->owner_uuid==request.storage.allocation_owner_uuid&&record->page_type==request.storage.allocation_page_type&&record->creator_operation_uuid==result.publication_attempt_uuid&&record->page_uuid.is_nil()&&!record->page_generation,"exact fresh growth reservation records");
      else Check(record==map.records.end(),"free growth suffix cannot contain manufactured records");}}
    overlay(bytes);}
   Check(covered==count&&(!subdivide||!reserve||target_maps>1),"entire growth suffix covered including required map subdivision");
   const auto after=d::DecodeFilespacePageZero(entry.control_growth_images.back().data(),target_size);Check(after.ok(),"canonical actual growth after body");auto expected_zero=*before.record;
   ++expected_zero.page_generation;++expected_zero.root_set_generation;expected_zero.total_pages+=count;expected_zero.free_pages=free;expected_zero.preallocated_pages=preallocated;
   const auto encoded=d::EncodeFilespacePageZero(expected_zero);Check(encoded.ok()&&*encoded.bytes==entry.control_growth_images.back(),"exact allowed page-zero fields and complete map counters");
   for(const auto& raw:entry.control_directory_images){const auto decoded=page::DecodeNativeFilespaceDirectory(raw);Check(decoded.ok()&&raw==DirectoryImageOracle(*decoded.directory),"independent directory encoding after owned growth");overlay(raw);}
   const auto extent=db::EncodeNativeManagementExtent(entry.record,entry.plan.management_extent->object_uuid,entry.extent_pages,f.budget);Check(extent.ok(),"exact retained growth extent");for(const auto& raw:extent.pages)overlay(raw);
   const auto bundle=db::EncodeNativeManagementControlBundle(entry.control_allocation_images,Id(1),entry.plan.bootstrap_uuid,entry.plan.control_bundle->object_uuid,entry.plan.operation_uuid,entry.bundle_pages,f.budget,{},entry.control_directory_images,entry.control_growth_images);
   Check(bundle.ok(),"exact complete growth reconstruction");for(const auto& raw:bundle.pages)overlay(raw);overlay(Oracle(entry.plan));
   const auto base=db::DecodeNativeCheckpointRoot(t.fixture.Read(entry.plan.base_checkpoint.page_number));Check(base.ok(),"independent original growth checkpoint");auto checkpoint=*base.root;
   const page::NativeAllocationRecord* target_record=nullptr;std::vector<page::NativeAllocationMap> decoded_maps;
   for(const auto& raw:entry.control_allocation_images){const auto image=page::DecodeNativeAllocationMap(raw);Check(image.ok(),"oracle target allocation identity");decoded_maps.push_back(*image.map);}
   for(const auto& map:decoded_maps)if(map.header.filespace_uuid==Id(2))for(const auto& record:map.records)if(record.page_number==entry.plan.target_checkpoint.page_number)target_record=&record;
   Check(target_record,"checkpoint owns a retained allocation record");checkpoint.header=entry.plan.header;checkpoint.header.page_type=0x300;checkpoint.header.page_number=entry.plan.target_checkpoint.page_number;checkpoint.header.page_uuid=target_record->page_uuid;
   checkpoint.object_uuid=entry.plan.target_checkpoint_object_uuid;checkpoint.creator_transaction_uuid={};checkpoint.creator_local_transaction_id=0;checkpoint.creator_operation_uuid=entry.plan.operation_uuid;
   checkpoint.checkpoint_generation=entry.plan.reserved_generation;checkpoint.root_set_generation=entry.plan.target_root_set_generation;checkpoint.predecessor=entry.plan.base_checkpoint;checkpoint.predecessor_sha256=entry.plan.base_checkpoint_sha256;
   const auto replace=[&](db::NativeCheckpointRootReference root){const auto at=std::find_if(checkpoint.roots.begin(),checkpoint.roots.end(),[&](const auto& r){return r.role==root.role;});if(at==checkpoint.roots.end())checkpoint.roots.push_back(root);else *at=root;};
   for(const auto& raw:entry.control_allocation_images){const auto image=page::DecodeNativeAllocationMap(raw);if(image.map->header.filespace_uuid==Id(2)&&!image.map->first_page){replace({4,3,Self(image.map->header),image.map->object_uuid,Sha(raw)});break;}}
   const auto directory=page::DecodeNativeFilespaceDirectory(entry.control_directory_images.front());Check(directory.ok(),"oracle candidate directory identity");replace({3,9,Self(directory.directory->header),directory.directory->object_uuid,Sha(entry.control_directory_images.front())});
   replace({16,0x500,Self(entry.plan.header),entry.plan.object_uuid,Sha(Oracle(entry.plan))});const auto checkpoint_image=db::EncodeNativeCheckpointRoot(checkpoint);Check(checkpoint_image.ok(),"fully specified expected growth checkpoint");overlay(checkpoint_image.bytes);
   for(const auto& root:t.graph.zero.roots)if(root.kind>=18&&root.kind<=21){const auto begin=root.page_number*t.fixture.size;Bytes prior(expected.at(Id(2)).begin()+begin,expected.at(Id(2)).begin()+begin+t.fixture.size);
    if(root.kind<20){const auto old=db::DecodeNativeCheckpointSelection(prior);Check(old.ok(),"original independent selector");auto next=*old.selection;
     next.selection_generation=*entry.plan.base_selection_generation+1;next.previous_selection_generation=*entry.plan.base_selection_generation;next.publication_uuid=entry.plan.operation_uuid;
     next.checkpoint=entry.plan.target_checkpoint;next.checkpoint_object_uuid=entry.plan.target_checkpoint_object_uuid;next.checkpoint_sha256=Sha(checkpoint_image.bytes);next.checkpoint_generation=entry.plan.reserved_generation;next.root_set_generation=entry.plan.target_root_set_generation;
     next.previous_checkpoint=entry.plan.base_checkpoint;next.previous_checkpoint_object_uuid=entry.plan.base_checkpoint_object_uuid;next.previous_checkpoint_sha256=entry.plan.base_checkpoint_sha256;overlay(SelectorOracle(next));
    }else{const auto old=db::DecodeNativePublicationWatermark(prior);Check(old.ok(),"original independent reserved watermark");auto next=*old.state;next.publication_plan=db::NativePublicationWatermark::PlanAnchor{Self(entry.plan.header),entry.plan.object_uuid,Sha(Oracle(entry.plan)),entry.plan.reservation_state_sha256};const auto image=db::EncodeNativePublicationWatermark(next);Check(image.ok(),"expected anchored original watermark");overlay(image.bytes);}}
   for(const auto& file:t.fixture.devices)Check(all(file.device)==expected.at(file.filespace_uuid),"complete grown-node oracle preserves unrelated files and every unmodified byte");
   if(previous){const auto retained=db::ReconcileNativeFilespaceGrowthFromOpenDevices(Id(1),t.fixture.devices,Id(2),*previous,f.budget);Check(retained.ok()&&retained.observation->disposition==db::NativePreallocationDisposition::selected&&retained.observation->publication_generation<reconciled.observation->publication_generation,"original growth request remains discoverable after a later publication");}
   previous=request.record;
  }
  f.Reopen();const auto cold=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);Check(cold.ok(),"cold read-only admission of repeated actual owned growth");
 }
}
void OwnedGrowthAdmission(unsigned primary,unsigned secondary){
 for(bool target:{false,true})for(bool reserve:{false,true}){
  DirectoryHistoryFixture fixture(primary,secondary,true,4,target,reserve,true,false);auto& f=fixture.t.fixture;
  f.budget=32768*std::max<u64>(f.size,d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes);
  OwnedPreallocationRequest original(f,0,0,false,fixture.t.changed,4,true,reserve);
  const auto pending=db::ReconcileNativeFilespaceGrowthFromOpenDevices(Id(1),f.devices,Id(2),original.record,f.budget);
  Check(pending.ok()&&pending.observation->disposition==db::NativePreallocationDisposition::pending_unanchored&&pending.observation->publication_attempt_uuid==Id(21100),"original growth request is pending without manufactured identity");
  const auto snapshot=[&]{std::map<Uuid,Bytes> images;for(const auto& file:f.devices){const auto size=file.device->Size();Check(size.ok(),"admission actual file capacity");Bytes raw(size.size_bytes);const auto io=file.device->ReadAt(0,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size(),"admission exact file preimage");images.emplace(file.filespace_uuid,std::move(raw));}return images;};
  const auto before=snapshot();
  for(unsigned mode=0;mode<5;++mode){auto changed=original.storage;
   if(mode==0)changed.request_uuid=Id(27001);
   if(mode==1)changed.operation_uuid=Id(27002);
   if(mode==2){++changed.page_count;++changed.maximum_total_pages;changed.maximum_work_bytes+=changed.page_size_bytes;}
   if(mode==3)changed.policy_version_uuid=Id(27003);
   if(mode==4)++changed.capacity_generation;
   auto record=original.record;record.uuid=changed.operation_uuid;const auto encoded=db::EncodeNativeStorageActionIntent(changed,f.budget);Check(encoded.ok(),"conflicting growth request is structurally valid");record.normalized_request_bytes=encoded.bytes;record.normalized_request_sha256=Sha(encoded.bytes);
   reads=writes=syncs=0;io_counting=true;
   const auto result=db::PublishNativeFilespaceGrowthOnLease(*original.held.lease,record,f.budget,*f.issuer);io_counting=false;
   Check(!result.ok()&&!result.physical_attempted&&!result.physical&&!writes&&!syncs&&snapshot()==before,"changed original growth intent has no physical or metadata effects");
  }
  auto* device=fixture.File(fixture.t.changed);const auto path=target?fixture.secondary_path:f.path;
  Check(device->Close().ok()&&device->Open(path.string(),d::FileOpenMode::open_existing_read_only).ok(),"readonly growth target fixture");
  reads=writes=syncs=0;io_counting=true;const auto readonly=db::PublishNativeFilespaceGrowthOnLease(*original.held.lease,original.record,f.budget,*f.issuer);io_counting=false;
  Check(!readonly.ok()&&readonly.error==db::NativePublicationError::invalid_device&&!readonly.physical_attempted&&!writes&&!syncs&&snapshot()==before,"readonly target refuses before any effects");
  Check(device->Close().ok()&&device->Open(path.string(),d::FileOpenMode::open_existing).ok(),"restore writable growth target");
  Check(db::PublishNativeFilespaceGrowthOnLease(*original.held.lease,original.record,f.budget,*f.issuer).ok(),"preflight refusal does not poison unchanged original lease");original.held.lease.reset();
  const auto selected=snapshot();
  for(unsigned mode=0;mode<5;++mode){auto changed=original.storage;
   if(mode==0)changed.request_uuid=Id(27001);
   if(mode==1)changed.operation_uuid=Id(27002);
   if(mode==2)changed.policy_version_uuid=Id(27003);
   if(mode==3){changed.request_uuid=Id(27001);changed.operation_uuid=Id(27002);}
   if(mode==4){changed.action=db::NativeStorageAction::page_preallocation;changed.first_page=200;changed.maximum_total_pages=changed.current_total_pages;changed.intended_state=db::NativeStorageIntentState::preallocated;changed.allocation_owner_uuid=Id(27004);changed.allocation_page_type=1;}
   auto record=original.record;record.uuid=changed.operation_uuid;const auto encoded=db::EncodeNativeStorageActionIntent(changed,f.budget);Check(encoded.ok(),"historical conflicting growth identity is structurally valid");record.normalized_request_bytes=encoded.bytes;record.normalized_request_sha256=Sha(encoded.bytes);
   reads=writes=syncs=0;io_counting=true;
   const auto observed=mode==4?db::ReconcileNativePreallocationFromOpenDevices(Id(1),f.devices,Id(2),record,f.budget):db::ReconcileNativeFilespaceGrowthFromOpenDevices(Id(1),f.devices,Id(2),record,f.budget);io_counting=false;
   Check(mode==3?(observed.ok()&&observed.observation->disposition==db::NativePreallocationDisposition::absent):(!observed.ok()&&observed.error==db::NativePublicationError::request_mismatch&&!observed.observation),"conflicting reuse of either identity across intents or action families cannot be absent or completed");
   Check(!writes&&!syncs&&snapshot()==selected,"historical reconciliation is read only on every member");
  }
  {OwnedPreallocationRequest next(f,1,0,false,fixture.t.changed,4,true,reserve);const auto key=next.record.idempotency_key;
   next.record.idempotency_key=original.record.idempotency_key;const auto pending_bytes=snapshot();reads=writes=syncs=0;preallocation_fd=-1;io_counting=true;
   const auto collision=db::PublishNativeFilespaceGrowthOnLease(*next.held.lease,next.record,f.budget,*f.issuer);io_counting=false;
   Check(collision.error==db::NativePublicationError::request_mismatch&&!collision.physical_attempted&&!writes&&!syncs&&preallocation_fd==-1&&snapshot()==pending_bytes,"management idempotency collision is refused before graph installation or physical effects");
   next.record.idempotency_key=key;Check(db::PublishNativeFilespaceGrowthOnLease(*next.held.lease,next.record,f.budget,*f.issuer).ok(),"unchanged original new request can proceed after rejected idempotency collision");}
 }
}
void OwnedGrowthPhysicalRecovery(unsigned primary,unsigned secondary){
 for(bool target:{false,true})for(bool reserve:{false,true})for(unsigned mode=1;mode<=8;++mode){
  DirectoryHistoryFixture fixture(primary,secondary,true,4,target,reserve,true,false);auto& f=fixture.t.fixture;
  f.budget=32768*std::max<u64>(f.size,d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes);
  OwnedPreallocationRequest request(f,0,0,false,fixture.t.changed,4,true,reserve);
  preallocation_calls=reads=writes=syncs=0;preallocation_fault=mode==8?5:mode;torn_bytes=mode==8?128:0;io_counting=true;
  const auto result=db::PublishNativeFilespaceGrowthOnLease(*request.held.lease,request.record,f.budget,*f.issuer);
  io_counting=false;preallocation_fault=read_fault=write_fault=sync_fault=0;torn_bytes=0;
  Check(preallocation_calls==1&&result.preparation.ok()&&result.physical_attempted&&result.physical,"actual growth failure retains phase and physical result");
  if(mode==3)Check(result.ok()&&!result.physical->platform_preallocation_succeeded&&result.physical->logical_size_extended,"unsupported reserve actually grows logical extent without claiming physical reserve");
  else Check(!result.ok()&&result.error==db::NativePublicationError::io_failure&&!result.selection.ok(),"partial physical or metadata effects never report completed growth");
  const auto duplicate=db::PublishNativeFilespaceGrowthOnLease(*request.held.lease,request.record,f.budget,*f.issuer);
  Check(!duplicate.ok()&&duplicate.error==db::NativePublicationError::stale_base&&!duplicate.physical_attempted,"ambiguous or completed growth lease cannot repeat physical work");
  request.held.lease.reset();fixture.Reopen(true);
  const auto before=fixture.File(fixture.t.changed)->Size();Check(before.ok(),"physical extent observed after failed original call");
  reads=writes=syncs=0;preallocation_fd=-1;io_counting=true;
  auto wrong=request.record;auto changed=request.storage;changed.policy_version_uuid=Id(28000);
  const auto encoded=db::EncodeNativeStorageActionIntent(changed,f.budget);Check(encoded.ok(),"conflicting recovery request is canonical");wrong.normalized_request_bytes=encoded.bytes;wrong.normalized_request_sha256=Sha(encoded.bytes);
  const auto refused=db::RecoverNativeFilespaceGrowthOnOpenDevices(Id(1),f.devices,Id(2),Id(21100),wrong,f.budget);io_counting=false;
  Check(!refused.ok()&&refused.error==db::NativePublicationError::request_mismatch&&!refused.physical_attempted&&!writes&&!syncs&&preallocation_fd==-1,"original anchored growth conflict refuses before physical or metadata writes");
  const auto recovered=db::RecoverNativeFilespaceGrowthOnOpenDevices(Id(1),f.devices,Id(2),Id(21100),request.record,f.budget);
  if(!recovered.ok())std::cerr<<"growth recovery primary="<<primary<<" secondary="<<secondary<<" target="<<target<<" reserve="<<reserve<<" mode="<<mode<<" error="<<unsigned(recovered.error)<<" phase="<<unsigned(recovered.phase)<<" original="<<recovered.original_graph_verified<<" selection="<<unsigned(recovered.selection.error)<<'\n';
  Check(recovered.ok()&&recovered.request_uuid==request.storage.request_uuid&&recovered.operation_uuid==request.storage.operation_uuid&&recovered.publication_attempt_uuid==Id(21100)&&recovered.observed_extent_bytes==before.size_bytes,"same original operation repairs actual growth after cold reopen");
  Check(mode==3?(recovered.already_selected&&!recovered.physical_attempted&&!recovered.physical):(!recovered.already_selected&&recovered.physical_attempted&&recovered.physical&&recovered.physical->ok()&&recovered.physical_sync&&recovered.physical_sync->ok()&&recovered.page_zero&&recovered.page_zero->ok()),"completed retry never allocates again and unresolved recovery reports every actual phase");
  if(mode==7)Check(before.size_bytes>recovered.original_extent_bytes&&before.size_bytes<recovered.target_extent_bytes&&before.size_bytes%request.storage.page_size_bytes,"unaligned partial extension recovered without truncation");
  if(mode==8)Check(recovered.page_zero->damaged_body_observed,"torn mutable body was actually detected and repaired");
  const auto again=db::RecoverNativeFilespaceGrowthOnOpenDevices(Id(1),f.devices,Id(2),Id(21100),request.record,f.budget);
  Check(again.ok()&&again.already_selected&&!again.physical_attempted&&!again.physical,"selected original-operation recovery is allocation-idempotent");
  fixture.Reopen();const auto receipt=db::ReconcileNativeFilespaceGrowthFromOpenDevices(Id(1),f.devices,Id(2),request.record,f.budget);
  Check(receipt.ok()&&receipt.observation->disposition==db::NativePreallocationDisposition::selected,"cold ordinary admission reconciles original growth after physical repair");
 }
}
void OwnedGrowthRecoveryFaults(unsigned route,unsigned shard){
 for(bool target:{false,true}){
  DirectoryHistoryFixture fixture(0,1,true,4,target,true,true,false);auto& f=fixture.t.fixture;f.budget=32768*16384;
  OwnedPreallocationRequest request(f,0,0,false,fixture.t.changed,4,true,true);
  preallocation_fault=7;const auto interrupted=db::PublishNativeFilespaceGrowthOnLease(*request.held.lease,request.record,f.budget,*f.issuer);preallocation_fault=0;
  Check(!interrupted.ok()&&interrupted.physical_attempted,"fault sweep starts from actual interrupted physical extension");request.held.lease.reset();
  const auto snapshot=[&]{std::map<Uuid,Bytes> images;for(const auto& file:f.devices){const auto size=file.device->Size();Check(size.ok(),"growth sweep extent");Bytes raw(size.size_bytes);const auto io=file.device->ReadAt(0,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size(),"growth sweep whole-file image");images.emplace(file.filespace_uuid,std::move(raw));}return images;};
  const auto original=snapshot();
  const auto reset=[&]{for(const auto& file:f.devices){const auto& raw=original.at(file.filespace_uuid);const auto path=file.filespace_uuid==Id(2)?f.path:file.filespace_uuid==Id(11)?fixture.untouched_path:fixture.secondary_path;
    Check(::truncate(path.c_str(),raw.size())==0,"reset exact owned fault fixture extent");const auto io=file.device->WriteAt(0,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size()&&file.device->Sync().ok(),"restore exact partial-growth fixture");}};
  for(const auto& file:f.devices){byte scratch=0;for(unsigned i=0;i<4097;++i)Check(file.device->ReadAt(0,&scratch,1).ok(),"growth recovery telemetry warmup");}
  const auto call=[&]{return db::RecoverNativeFilespaceGrowthOnOpenDevices(Id(1),f.devices,Id(2),Id(21100),request.record,f.budget);};
  reads=writes=syncs=hash_seen=0;allocations=0;io_counting=true;hash_counting=route==1;counting=route==2;
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
  historical_stats=0;historical_stat_counting=route==0;
#endif
  const auto baseline=call();io_counting=hash_counting=counting=false;const auto nr=reads,nw=writes,ns=syncs,nh=hash_seen;const auto na=allocations;
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
  historical_stat_counting=false;const auto nstats=historical_stats;
#endif
  Check(baseline.ok(),"original-operation recovery fault baseline");const auto expected=snapshot();
  std::cout<<"owned growth recovery faults target="<<target<<" route="<<route<<" reads="<<nr<<" writes="<<nw<<" syncs="<<ns<<" hashes="<<nh<<" allocations="<<na<<'\n';
  const auto loss=[&]{u64 n=0;for(const auto& file:f.devices)n+=file.device->failed_io_latency_observations();return n;};
  const auto run=[&](unsigned kind,unsigned long at,std::size_t torn=0){reset();const auto lost=loss();reads=writes=syncs=0;io_counting=true;
    if(kind==0)read_fault=at;if(kind==1){write_fault=at;torn_bytes=torn;}if(kind==2)sync_fault=at;
    if(kind==3){hash_fault=shard%5+1;hash_target=at;hash_seen=0;hash_active=false;}if(kind==4)allocation_budget=at;
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
    if(kind==5){historical_stats=0;historical_stat_counting=true;historical_stat_fault=at;}
#endif
    const auto result=call();const auto left=allocation_budget;allocation_budget=-1;const bool consumed=!hash_fault;hash_fault=read_fault=write_fault=sync_fault=0;torn_bytes=0;io_counting=false;
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
    historical_stat_counting=false;historical_stat_fault=0;if(kind==5)Check(historical_stats>=at,"original growth stat fault consumed");
#endif
    const auto want=kind==3?db::NativePublicationError::hash_failure:kind==4?db::NativePublicationError::resource_exhausted:db::NativePublicationError::io_failure;
    if(kind==4&&result.ok())Check(left==-1&&loss()==lost+1,"only accounted optional telemetry failure permits completed original growth recovery");
    else{if(result.error!=want)std::cerr<<"owned growth recovery fault kind="<<kind<<" site="<<at<<" target="<<target<<" error="<<unsigned(result.error)<<" phase="<<unsigned(result.phase)<<'\n';
      Check(result.error==want&&!result.ok(),"every required growth recovery fault retains its typed failure");}
    if(kind==0)Check(reads>=at,"growth recovery read fault consumed");if(kind==1)Check(writes>=at,"growth recovery write fault consumed");if(kind==2)Check(syncs>=at,"growth recovery sync fault consumed");if(kind==3)Check(consumed,"growth recovery hash fault consumed");if(kind==4)Check(left==-1,"growth recovery allocation fault consumed");
    if(!result.original_graph_verified)Check(!result.physical_attempted&&!writes&&!syncs&&snapshot()==original,"failed authority verification leaves every file byte and extent unchanged");
    const auto recovered=call();if(!recovered.ok())std::cerr<<"growth fault retry kind="<<kind<<" at="<<at<<" error="<<unsigned(recovered.error)<<" phase="<<unsigned(recovered.phase)<<'\n';
    Check(recovered.ok()&&snapshot()==expected,"same retained operation recovers after every injected failure with exact whole-node result");
  };
  if(route==2){for(unsigned long at=shard;at<na;at+=32)run(4,at);}
  else if(route==1){for(unsigned at=shard/5+1;at<=nh;at+=4)run(3,at);}
  else{for(unsigned at=1;at<=nr;++at)run(0,at);for(unsigned at=1;at<=nw;++at){run(1,at);run(1,at,128);}for(unsigned at=1;at<=ns;++at)run(2,at);
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
    for(unsigned at=1;at<=nstats;++at)run(5,at);
#endif
  }
 }
}
void OwnedGrowthRecoveryRefusals(unsigned primary,unsigned secondary){
 for(bool target:{false,true}){
  DirectoryHistoryFixture fixture(primary,secondary,true,4,target,true,true,false);auto& f=fixture.t.fixture;
  f.budget=32768*std::max<u64>(f.size,d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes);
  OwnedPreallocationRequest request(f,0,0,false,fixture.t.changed,4,true,true);
  preallocation_fault=7;const auto interrupted=db::PublishNativeFilespaceGrowthOnLease(*request.held.lease,request.record,f.budget,*f.issuer);preallocation_fault=0;
  Check(!interrupted.ok()&&interrupted.preparation.ok()&&interrupted.physical_attempted,"recovery refusals start from genuine partial extension");
  const auto anchor=*request.held.lease->snapshot().watermark.publication_plan;request.held.lease.reset();
  const auto plan=db::DecodeNativePublicationPlan(f.Read(anchor.page.page_number));Check(plan.ok(),"actual retained original recovery plan");
  const auto snapshot=[&]{std::map<Uuid,Bytes> images;for(const auto& file:f.devices){const auto size=file.device->Size();Check(size.ok(),"negative recovery physical size");Bytes raw(size.size_bytes);const auto io=file.device->ReadAt(0,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size(),"negative recovery full physical image");images.emplace(file.filespace_uuid,std::move(raw));}return images;};
  const auto original=snapshot();const auto path=[&](const Uuid& fs){return fs==Id(2)?f.path:fs==Id(11)?fixture.untouched_path:fixture.secondary_path;};
  const auto reset=[&]{for(const auto& file:f.devices){const auto& raw=original.at(file.filespace_uuid);Check(::truncate(path(file.filespace_uuid).c_str(),raw.size())==0,"restore negative recovery fixture extent");const auto io=file.device->WriteAt(0,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size()&&file.device->Sync().ok(),"restore negative recovery fixture bytes");}};
  for(unsigned mode=0;mode<13;++mode){reset();auto files=f.devices;auto attempt=Id(21100);u64 budget=f.budget;
   if(mode==0)attempt=Id(28100);
   if(mode==1)files.pop_back();
   if(mode==2)budget=1;
   if(mode>=3&&mode<=8){Uuid fs=Id(2);u64 offset=0;
    if(mode==3)offset=anchor.page.page_number*f.size+400;
    if(mode==4)offset=plan.plan->control_bundle->first.page_number*f.size+400;
    if(mode==5)offset=plan.plan->target_checkpoint.page_number*f.size+400;
    if(mode==6){fs=request.storage.filespace_uuid;offset=request.storage.allocation_root.page_number*u64{request.storage.page_size_bytes}+400;}
    if(mode==7){fs=request.storage.filespace_uuid;offset=16;}
    if(mode==8){const auto root=std::find_if(fixture.t.graph.zero.roots.begin(),fixture.t.graph.zero.roots.end(),[](const auto& r){return r.kind==20;});Check(root!=fixture.t.graph.zero.roots.end(),"original recovery watermark root");offset=root->page_number*f.size+400;}
    byte changed=original.at(fs).at(offset)^0x5a;Check(fixture.File(fs)->WriteAt(offset,&changed,1).ok()&&fixture.File(fs)->Sync().ok(),"actual corrupted recovery authority fixture");
   }
   if(mode==9){const auto& raw=original.at(request.storage.filespace_uuid);const auto z=d::DecodeFilespacePageZero(raw.data(),request.storage.page_size_bytes);Check(z.ok(),"original growth body before contradiction");auto wrong=*z.record;++wrong.page_generation;const auto image=d::EncodeFilespacePageZero(wrong);Check(image.ok()&&fixture.File(request.storage.filespace_uuid)->WriteAt(0,image.bytes->data(),image.bytes->size()).ok(),"canonical contradictory body is not a tear");}
   if(mode==10||mode==11){const auto bytes=(request.storage.current_total_pages+(mode==10?request.storage.page_count+1:0))*u64{request.storage.page_size_bytes}-(mode==11?1:0);Check(::truncate(path(request.storage.filespace_uuid).c_str(),bytes)==0,"unexpected actual extent fixture");}
   if(mode==12){auto zero=original.at(Id(11));zero[5000]^=1;Check(fixture.untouched.WriteAt(0,zero.data(),zero.size()).ok(),"unrelated immutable member damage fixture");}
   const auto damaged=snapshot();reads=writes=syncs=0;preallocation_fd=-1;io_counting=true;
   const auto result=db::RecoverNativeFilespaceGrowthOnOpenDevices(Id(1),files,Id(2),attempt,request.record,budget);io_counting=false;
   if(result.ok()||result.physical_attempted||writes||syncs||preallocation_fd!=-1||snapshot()!=damaged)std::cerr<<"growth recovery refusal defect="<<mode<<" target="<<target<<" error="<<unsigned(result.error)<<" phase="<<unsigned(result.phase)<<" attempted="<<result.physical_attempted<<" writes="<<writes<<" syncs="<<syncs<<" physical_fd="<<preallocation_fd<<'\n';
   Check(!result.ok()&&!result.physical_attempted&&!result.physical&&!writes&&!syncs&&preallocation_fd==-1&&snapshot()==damaged,"contradictory recovery inputs retain all bytes and cause no new physical or metadata effects");
  }
  reset();Check(db::RecoverNativeFilespaceGrowthOnOpenDevices(Id(1),f.devices,Id(2),Id(21100),request.record,f.budget).ok(),"restored exact original growth evidence remains recoverable");
 }
}
void OwnedGrowthCrashes(unsigned primary,unsigned secondary){
 for(bool target:{false,true})for(bool reserve:{false,true}){
  DirectoryHistoryFixture fixture(primary,secondary,true,4,target,reserve,true,false);auto& f=fixture.t.fixture;
  f.budget=32768*std::max<u64>(f.size,d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes);
  OwnedPreallocationRequest request(f,0,0,false,fixture.t.changed,4,true,reserve);
  preallocation_fault=1;const auto staged=db::PublishNativeFilespaceGrowthOnLease(*request.held.lease,request.record,f.budget,*f.issuer);preallocation_fault=0;
  Check(!staged.ok()&&staged.preparation.ok()&&staged.physical_attempted,"growth crash fixture has authoritative staged metadata and no extension");request.held.lease.reset();
  const auto snapshot=[&]{std::map<Uuid,Bytes> images;for(const auto& file:f.devices){const auto size=file.device->Size();Check(size.ok(),"growth crash physical size");Bytes raw(size.size_bytes);const auto io=file.device->ReadAt(0,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size(),"growth crash whole-node image");images.emplace(file.filespace_uuid,std::move(raw));}return images;};
  const auto original=snapshot();
  const auto restore=[&]{fixture.Reopen(true);for(const auto& file:f.devices){const auto& raw=original.at(file.filespace_uuid);const auto path=file.filespace_uuid==Id(2)?f.path:file.filespace_uuid==Id(11)?fixture.untouched_path:fixture.secondary_path;
    Check(::truncate(path.c_str(),raw.size())==0,"restore owned pre-extension crash extent");const auto io=file.device->WriteAt(0,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size()&&file.device->Sync().ok(),"restore exact anchored pre-extension metadata");}};
  const auto call=[&]{return db::RecoverNativeFilespaceGrowthOnOpenDevices(Id(1),f.devices,Id(2),Id(21100),request.record,f.budget);};
  reads=writes=syncs=0;io_counting=true;const auto baseline=call();io_counting=false;const auto nw=writes,ns=syncs;
  Check(baseline.ok()&&nw&&ns,"original growth crash baseline");const auto expected=snapshot();
  for(unsigned kind=0;kind<6;++kind)for(unsigned at=1;at<=(kind<2?nw:kind<4?ns:1);++at){restore();
    for(const auto& file:f.devices)Check(file.device->Close().ok(),"close parent before independent growth worker");
    const auto child=fork();Check(child>=0,"fork original-operation growth worker");if(!child){fixture.Reopen(true);writes=syncs=0;kill_write=kind<2?at:0;kill_sync=kind>=2&&kind<4?at:0;kill_after_sync=kind==3;torn_bytes=kind==1?128:0;preallocation_death=kind==4?1:kind==5?2:0;io_counting=true;(void)call();_exit(87);}
    int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"worker stops at each growth write barrier and allocation boundary");
    const auto recovery=fork();Check(recovery>=0,"fork independent original growth recovery");if(!recovery){fixture.Reopen(true);const auto recovered=call();_exit(recovered.ok()?0:83);}
    Check(waitpid(recovery,&status,0)==recovery&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"cold independent process completes the original retained growth operation");fixture.Reopen();
    Check(snapshot()==expected,"every growth interruption converges to the same exact whole-node image");
    const auto selected=db::ReconcileNativeFilespaceGrowthFromOpenDevices(Id(1),f.devices,Id(2),request.record,f.budget);
    Check(selected.ok()&&selected.observation->disposition==db::NativePreallocationDisposition::selected&&selected.observation->publication_attempt_uuid==Id(21100),"cold crash receipt retains original binary attempt");
  }
 }
}
void PreallocationDeltaNegatives(Fixture& f,u64 first=200,bool dense=false) {
 const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
 Check(history.ok()&&!history.entries.empty(),"preallocation delta retained source");const auto& entry=history.entries.back();
 const auto base_bytes=f.Read(entry.plan.base_checkpoint.page_number);
 const auto base=db::DecodeNativeCheckpointRoot(base_bytes);Check(base.ok(),"preallocation delta base");
 const auto root=std::find_if(base.root->roots.begin(),base.root->roots.end(),[](const auto& r){return r.role==4;});
 Check(root!=base.root->roots.end(),"preallocation delta allocation root");
 const auto before=page::ReadNativeAllocationChainAtRootFromOpenDevice(f.device,{Id(1),Id(2),root->page.page_size_profile_uuid},
   {3,3,Id(2),root->page.page_number,root->page.page_generation,root->page.page_size_profile_uuid,root->object_uuid},f.budget);
 Check(before.ok(),"preallocation delta actual original allocation");Pages before_bytes;for(const auto& image:before.pages)before_bytes.push_back(image.bytes);
 const auto target=db::DecodeNativeCheckpointRoot(f.Read(entry.plan.target_checkpoint.page_number));Check(target.ok(),"preallocation delta target");
 const auto extent=db::EncodeNativeManagementExtent(entry.record,entry.plan.management_extent->object_uuid,entry.extent_pages,f.budget);
 Check(extent.ok()&&extent.root==entry.plan.management_extent,"preallocation exact retained extent");
 Maps original;for(const auto& bytes:entry.control_allocation_images){const auto image=page::DecodeNativeAllocationMap(bytes);Check(image.ok(),"preallocation original map");original.push_back(*image.map);}
 const auto run=[&](Maps maps,bool valid){
  const auto after=EncodeMaps(maps);auto plan=entry.plan;auto checkpoint=*target.root;
  const auto bundle=db::EncodeNativeManagementControlBundle(after,Id(1),plan.bootstrap_uuid,
    plan.control_bundle->object_uuid,plan.operation_uuid,entry.bundle_pages,f.budget);
  Check(bundle.ok(),"mutated preallocation bundle remains structurally canonical");plan.control_bundle=bundle.root;
  auto& root=*std::find_if(checkpoint.roots.begin(),checkpoint.roots.end(),[](const auto& r){return r.role==4;});root.sha256=Sha(after.front());
  const auto checkpoint_bytes=Finish(plan,checkpoint);const auto plan_bytes=Oracle(plan);
  const auto result=db::ValidateNativeManagementControlAllocation(base_bytes,checkpoint_bytes,plan_bytes,extent.pages,before_bytes,after,f.budget,bundle.pages);
  Check(valid?result==CE::none:result!=CE::none,"exact preallocation delta rejects self-consistent forged semantic change");
 };
 run(original,true);
 for(unsigned mode=0;mode<8;++mode){auto maps=original;auto& r=Find(maps,first);auto& map=Cover(maps,first);
  if(mode==0)r.owner_uuid=Id(25000);
  if(mode==1)r.page_type=2;
  if(mode==2)r.creator_operation_uuid=Id(25001);
  if(mode==3){r.page_uuid=Id(25002);r.page_generation=1;}
  if(mode==4)map.states[first-map.first_page]=State::reserved;
  if(mode==5){map.states[first-map.first_page]=State::free;map.records.erase(std::remove_if(map.records.begin(),map.records.end(),[&](const auto& v){return v.page_number==first;}),map.records.end());}
  if(mode==6){auto extra=r;extra.page_number=first-1;extra.allocation_uuid=Id(25003);
   if(dense){map.states[first-map.first_page]=State::free;map.records.erase(std::remove_if(map.records.begin(),map.records.end(),[&](const auto& v){return v.page_number==first;}),map.records.end());}
   auto& m=Cover(maps,first-1);m.states[first-1-m.first_page]=State::preallocated;m.records.push_back(extra);std::sort(m.records.begin(),m.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});}
  if(mode==7){const auto old=before.pages.front().map->records.front().page_number;Find(maps,old).owner_uuid=Id(25004);}
  run(std::move(maps),false);
 }
}
// Construct a larger original fixture, not evidence of a production growth action.
void EnlargeInitialSecondaryFixture(DirectoryHistoryFixture& fixture,u64 total){
 auto& t=fixture.t;auto& zero=t.zeros.at(t.other);if(total<=zero.total_pages)return;
 auto& map=t.before.at(t.other).front();map.states.resize(total,State::free);map.total_pages=total;
 zero.free_pages+=total-zero.total_pages;zero.total_pages=total;
 const auto map_image=MapOracle(map);
 for(auto& member:t.old_directory.records)if(member.bootstrap.filespace_uuid==t.other){member.total_pages=total;
  member.allocation_root=page::NativeFilespaceAllocationRoot{Self(map.header),map.object_uuid,Sha(map_image),map.map_generation,map.capacity_generation};}
 const byte end=0;Check(t.fixture.secondary.WriteAt(total*u64{zero.bootstrap.page_size_bytes}-1,&end,1).ok(),"larger original secondary fixture extent");
 const auto image=d::EncodeFilespacePageZero(zero);Check(image.ok()&&t.fixture.secondary.WriteAt(0,image.bytes->data(),image.bytes->size()).ok(),"larger original secondary fixture metadata");
 fixture.Write(map_image);const auto dir=DirectoryImageOracle(t.old_directory);fixture.Write(dir);
 auto checkpoint=t.graph.base;auto root=std::find_if(checkpoint.roots.begin(),checkpoint.roots.end(),[](const auto& r){return r.role==3;});
 Check(root!=checkpoint.roots.end(),"larger fixture original directory root");root->sha256=Sha(dir);
 const auto cp=db::EncodeNativeCheckpointRoot(checkpoint);Check(cp.ok(),"larger fixture original checkpoint");fixture.Write(cp.bytes);const auto digest=Sha(cp.bytes);
 for(const auto& r:t.graph.zero.roots){
  if(r.kind==18||r.kind==19){auto s=*db::DecodeNativeCheckpointSelection(t.fixture.Read(r.page_number)).selection;s.checkpoint_sha256=digest;fixture.Write(SelectorOracle(s));}
  if(r.kind==20||r.kind==21){auto w=*db::DecodeNativePublicationWatermark(t.fixture.Read(r.page_number)).state;w.base_checkpoint_sha256=digest;
   const auto bytes=db::EncodeNativePublicationWatermark(w);Check(bytes.ok(),"larger fixture original watermark");fixture.Write(bytes.bytes);}}
 for(const auto& member:t.fixture.devices)Check(member.device->Sync().ok(),"larger original fixture barrier");
}
void OwnedDirectoryPreallocation(unsigned primary,bool secondary_target=false,bool subdivide=false){
 for(unsigned secondary=0;secondary<5;++secondary)for(bool reverse:{false,true}){
  if(subdivide&&secondary&&secondary!=primary)continue;
  DirectoryHistoryFixture f(primary,secondary,reverse,3,false,true,true,false);auto& t=f.t;
  f.budget=t.fixture.budget=16384*std::max<u64>(t.fixture.size,d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes);
  const u64 dense_count=subdivide?(d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes-384)/128+1:0;
  if(subdivide)EnlargeInitialSecondaryFixture(f,300+2*dense_count);
  const auto read=[&](const Uuid& fs){const auto size=f.File(fs)->Size();Check(size.ok(),"actual untouched file size");Bytes raw(size.size_bytes);const auto io=f.File(fs)->ReadAt(0,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size(),"complete untouched file bytes");return raw;};
  const auto other=read(t.other),untouched=read(Id(11));
  auto expected_other=other;
  for(unsigned repeat=0;repeat<2;++repeat){OwnedPreallocationRequest request(t.fixture,repeat,0,false,secondary_target?t.other:Id(2),dense_count);
   if(secondary_target&&!repeat){
    const auto original_effects=request.held.lease->effects();
    const auto refused=[&](u64 allowance,db::NativePublicationError expected){reads=writes=syncs=0;io_counting=true;
     const auto result=db::PublishNativePreallocationOnLease(*request.held.lease,request.record,allowance,*t.fixture.issuer);io_counting=false;
     Check(result.publication.error==expected&&!result.publication.snapshot&&!result.physical_attempted&&!writes&&!syncs&&result.publication.effects==original_effects,
       "secondary preflight refusal has no new publication or physical effects");};
    refused(1,db::NativePublicationError::resource_exhausted);
    Check(t.fixture.secondary.Close().ok()&&t.fixture.secondary.Open(f.secondary_path.string(),d::FileOpenMode::open_existing_read_only).ok(),"readonly owned target fixture");
    refused(f.budget,db::NativePublicationError::invalid_device);
    Check(t.fixture.secondary.Close().ok()&&t.fixture.secondary.Open(f.secondary_path.string(),d::FileOpenMode::open_existing).ok(),"restore writable owned target");
    const u64 offset=request.storage.first_page*u64{request.storage.page_size_bytes};byte dirty=0x5a;
    Check(t.fixture.secondary.WriteAt(offset,&dirty,1).ok()&&t.fixture.secondary.Sync().ok(),"dirty free target range fixture");
    refused(f.budget,db::NativePublicationError::preimage_changed);dirty=0;
    Check(t.fixture.secondary.WriteAt(offset,&dirty,1).ok()&&t.fixture.secondary.Sync().ok(),"restore owned target range preimage");
   }
   preallocation_fd=-1;
   const auto result=db::PublishNativePreallocationOnLease(*request.held.lease,request.record,f.budget,*t.fixture.issuer);
   if(!result.ok())std::cerr<<"owned directory preallocation primary="<<primary<<" secondary="<<secondary<<" error="<<int(result.publication.error)<<'\n';
   Check(result.ok()&&result.physical_attempted&&result.physical&&result.physical->ok()&&!result.physical->logical_size_extended&&result.physical_sync_completed&&result.publication.effects.selected_graph_verified,"actual target physical preallocation and directory publication");
   struct stat requested{},actual{};const auto path=secondary_target?f.secondary_path:t.fixture.path;
   Check(stat(path.c_str(),&requested)==0&&preallocation_fd>=0&&fstat(preallocation_fd,&actual)==0&&requested.st_dev==actual.st_dev&&requested.st_ino==actual.st_ino&&
     preallocation_offset==request.storage.first_page*u64{request.storage.page_size_bytes}&&preallocation_bytes==request.storage.page_count*u64{request.storage.page_size_bytes},
     "physical reservation uses the actual target file descriptor and its own byte stride");
   request.held.lease.reset();const auto reconciliation=db::ReconcileNativePreallocationFromOpenDevices(Id(1),t.fixture.devices,Id(2),request.record,f.budget);
   Check(reconciliation.ok()&&reconciliation.observation->disposition==db::NativePreallocationDisposition::selected&&reconciliation.observation->request_uuid==request.storage.request_uuid&&reconciliation.observation->operation_uuid==request.storage.operation_uuid,"directory-bearing physical operation reconciles original binary request");
   const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);
   Check(history.ok()&&history.entries.size()==repeat+1&&!history.entries.back().control_directory_images.empty(),"owning physical operation retains complete directory reconstruction");
   for(const auto& raw:history.entries.back().control_allocation_images){const auto image=page::DecodeNativeAllocationMap(raw);Check(image.ok()&&raw==MapOracle(*image.map),"independent complete retained map image");
    if(image.map->header.filespace_uuid==t.other){const auto offset=image.map->header.page_number*u64{raw.size()};Check(offset<=expected_other.size()&&raw.size()<=expected_other.size()-offset,"secondary map destination within original member");
     std::copy(raw.begin(),raw.end(),expected_other.begin()+offset);}}
   const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget);Check(selected.ok(),"actual owned preallocation selected graph");
   const auto& cp=*selected.checkpoint_inventory.checkpoint;
   const d::FilespaceRootReference cp_ref{9,0x300,Id(2),cp.header.page_number,cp.header.page_generation,cp.header.page_size_profile_uuid,cp.object_uuid};
   const auto capacity=db::ReadNativeFilespaceCapacityFromOpenDevices(Id(1),t.fixture.devices,cp_ref,request.storage.filespace_uuid,f.budget);
   Check(capacity.ok(),"actual selected target capacity");
   const auto target_maps=page::ReadNativeAllocationChainAtRootFromOpenDevice(*f.File(request.storage.filespace_uuid),
     {Id(1),request.storage.filespace_uuid,request.storage.page_size_profile_uuid},capacity.observation->allocation_root,f.budget);
   Check(target_maps.ok(),"actual selected target map chain");
   if(subdivide)Check(target_maps.pages.size()>1,"actual dense secondary reservation requires owned map subdivision");
   for(u64 n=request.storage.first_page;n-request.storage.first_page<request.storage.page_count;++n){bool found=false;
    for(const auto& image:target_maps.pages)if(n>=image.map->first_page&&n-image.map->first_page<image.map->states.size()){
      Check(image.bytes==MapOracle(*image.map)&&image.map->states[n-image.map->first_page]==State::preallocated,"independent complete preallocation map bytes");
      for(const auto& record:image.map->records)if(record.page_number==n){found=true;Check(record.owner_uuid==request.storage.allocation_owner_uuid&&record.page_type==request.storage.allocation_page_type&&record.creator_operation_uuid==Id(21100+repeat*1000)&&record.page_uuid.is_nil()&&!record.page_generation,"exact original owned reservation record");}}
    Bytes bytes(request.storage.page_size_bytes);const auto io=f.File(request.storage.filespace_uuid)->ReadAt(n*u64{bytes.size()},bytes.data(),bytes.size());
    Check(found&&io.ok()&&io.bytes_transferred==bytes.size()&&bytes==Bytes(bytes.size()),"physical reservation does not invent initialized data");}
   Check(read(t.other)==expected_other&&read(Id(11))==untouched,"complete independent secondary file oracle and untouched third member");
  }
  f.Reopen();Check(db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),t.fixture.devices,Id(2),f.budget).ok(),"cold directory-bearing physical publication");
 }
}
void OwnedSecondaryConcurrentClose(unsigned primary,unsigned secondary,bool growth=false,bool primary_target=false){
 for(unsigned member=0;member<3;++member){
  DirectoryHistoryFixture fixture(primary,secondary,true,growth?4:3,!primary_target,true,true,false);auto& f=fixture.t.fixture;
  f.budget=16384*std::max<u64>(f.size,d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes);
  preallocation_pause=1;
  auto writer=std::async(std::launch::async,[&]{
   OwnedPreallocationRequest request(f,0,0,false,primary_target?Id(2):fixture.t.other,0,growth);
   bool success=false;
   if(growth){const auto result=db::PublishNativeFilespaceGrowthOnLease(*request.held.lease,request.record,f.budget,*f.issuer);success=result.ok()&&result.selection.effects.selected_graph_verified;}
   else{const auto result=db::PublishNativePreallocationOnLease(*request.held.lease,request.record,f.budget,*f.issuer);success=result.ok()&&result.publication.effects.selected_graph_verified;}
   return std::pair{success,request.record};
  });
  struct Release {~Release(){preallocation_pause=3;}} release;
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  while(preallocation_pause!=2&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  Check(preallocation_pause==2,"owning publisher reaches actual target allocation with retained guards");
  auto* target=fixture.File(member==0?Id(2):member==1?fixture.t.other:Id(11));std::atomic<bool> entered=false;
  auto closer=std::async(std::launch::async,[&]{entered=true;return target->Close();});
  while(!entered&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  const bool blocked=closer.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout;
  preallocation_pause=3;const auto outcome=writer.get();const auto closed=closer.get();
  Check(entered&&blocked&&closed.ok()&&outcome.first,
    "concurrent Close on every node member waits through physical reservation and selected publication");
  preallocation_pause=0;fixture.Reopen();
  const auto reconciled=growth?db::ReconcileNativeFilespaceGrowthFromOpenDevices(Id(1),f.devices,Id(2),outcome.second,f.budget):db::ReconcileNativePreallocationFromOpenDevices(Id(1),f.devices,Id(2),outcome.second,f.budget);
  Check(reconciled.ok()&&reconciled.observation->disposition==db::NativePreallocationDisposition::selected,
    "result and original request survive writer lifetime and concurrent close before cold reconciliation");
 }
}
void OwnedPreallocationPublication(unsigned profile,bool dense=false) {
 Fixture f(profile,DenseCapacity(profile,dense));f.budget*=8;OwnedPreallocationRequest request(f,0,0,dense);
 const auto first=request.storage.first_page,count=request.storage.page_count;
 const auto generation=request.storage.checkpoint_generation+1;
 const auto pending=db::ReconcileNativePreallocationFromOpenDevices(Id(1),f.devices,Id(2),request.record,f.budget);
 Check(pending.ok()&&pending.observation->disposition==db::NativePreallocationDisposition::pending_unanchored&&
   pending.observation->publication_attempt_uuid==Id(21100),"original pending preallocation lookup without manufactured identity");
 {auto changed=request.storage;changed.request_uuid=Id(25201);changed.operation_uuid=Id(25202);
  auto other=request.record;other.uuid=changed.operation_uuid;
  other.normalized_request_bytes=db::EncodeNativeStorageActionIntent(changed,f.budget).bytes;
  other.normalized_request_sha256=Sha(other.normalized_request_bytes);
  const auto blocked=db::ReconcileNativePreallocationFromOpenDevices(Id(1),f.devices,Id(2),other,f.budget);
  Check(blocked.ok()&&blocked.observation->disposition==db::NativePreallocationDisposition::other_operation_pending&&
    blocked.observation->publication_attempt_uuid.is_nil(),"another pending request cannot look absent or inherit original attempt");}
 const auto before=f.Read(0,f.total_pages);
 const auto published=db::PublishNativePreallocationOnLease(*request.held.lease,request.record,f.budget,*f.issuer);
 if(!published.ok())std::cerr<<"preallocation publication error="<<unsigned(published.publication.error)<<" attempted="<<published.physical_attempted<<'\n';
 Check(published.ok()&&published.physical_attempted&&published.physical_sync_completed&&
   published.publication.effects.installed_graph_verified&&published.publication.effects.selected_graph_verified&&
   published.physical->offset==first*f.size&&published.physical->bytes==count*f.size&&!published.physical->logical_size_extended,
   "actual preallocation and selected publication");
 request.held.lease.reset();
 std::size_t expected_history=dense?2:1;const std::size_t history_index=dense?1:0;
 const auto verify=[&]{
  reads=writes=syncs=0;io_counting=true;
  const auto receipt=db::ReconcileNativePreallocationFromOpenDevices(Id(1),f.devices,Id(2),request.record,f.budget);
  io_counting=false;
  Check(receipt.ok()&&!writes&&!syncs&&receipt.observation->disposition==db::NativePreallocationDisposition::selected&&
    receipt.observation->request_uuid==request.storage.request_uuid&&receipt.observation->operation_uuid==request.storage.operation_uuid&&
    receipt.observation->publication_attempt_uuid==Id(21100)&&receipt.observation->publication_generation==generation&&
    receipt.observation->root_set_generation==generation,"exact historical preallocation receipt after reopen or later publication");
  const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(selected.ok(),"selected preallocation graph admission");
  Check(selected.allocation.state_counts[7]==count,"exact preallocation count");
  if(dense)Check(selected.allocation.pages.size()>1,"dense records require actual additional map pages beyond the single published base map");
  for(const auto& image:selected.allocation.pages){
   Check(image.bytes==MapOracle(*image.map),"independent complete preallocation map bytes");
   for(const auto& r:image.map->records)if(r.page_number>=first&&r.page_number-first<count)
    Check(image.map->states[r.page_number-image.map->first_page]==State::preallocated&&r.owner_uuid==Id(21008)&&
      r.page_type==1&&r.page_uuid.is_nil()&&!r.page_generation&&r.creator_operation_uuid==Id(21100)&&
      r.creator_transaction_uuid.is_nil()&&!r.creator_local_transaction_id&&!r.reuse_horizon,
      "exact retained owner type state and operation lineage");
  }
  const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(history.ok()&&history.entries.size()==expected_history&&history.entries[history_index].record.normalized_request_bytes==request.record.normalized_request_bytes,
    "exact durable original preallocation request");
  Check(Oracle(history.entries[history_index].plan)==db::EncodeNativePublicationPlan(history.entries[history_index].plan).bytes,
    "independent version7 plan image");
  const auto after=f.Read(0,f.total_pages);
  Check(std::equal(before.begin()+first*f.size,before.begin()+(first+count)*f.size,after.begin()+first*f.size),"reserved pages remain uninitialized and unchanged");
 };
 verify();PreallocationDeltaNegatives(f,first,dense);
 for(unsigned mode=0;mode<4;++mode){
  auto changed=request.storage;if(mode==0)changed.allocation_owner_uuid=Id(25200);
  if(mode==1||mode==3)changed.request_uuid=Id(25201);
  if(mode>=2)changed.operation_uuid=Id(25202);
  auto record=request.record;record.uuid=changed.operation_uuid;
  record.normalized_request_bytes=db::EncodeNativeStorageActionIntent(changed,f.budget).bytes;
  record.normalized_request_sha256=Sha(record.normalized_request_bytes);
  const auto receipt=db::ReconcileNativePreallocationFromOpenDevices(Id(1),f.devices,Id(2),record,f.budget);
  Check(mode==3?(receipt.ok()&&receipt.observation->disposition==db::NativePreallocationDisposition::absent):
    (!receipt.ok()&&!receipt.observation&&receipt.error==db::NativePublicationError::request_mismatch),
    "conflicting reuse of either original identity cannot look absent or completed");
 }
 {OwnedInventoryRequest next(f);
  const auto earlier=db::ReconcileNativePreallocationFromOpenDevices(Id(1),f.devices,Id(2),request.record,f.budget);
  Check(earlier.ok()&&earlier.observation->disposition==db::NativePreallocationDisposition::selected&&
    earlier.observation->publication_generation==generation&&earlier.observation->snapshot.watermark.watermark==generation+1,
    "later pending work does not erase original selected receipt");
  const auto successor=db::PublishNativeInventoryOnLease(*next.held.lease,next.record,next.inventory,f.budget,*f.issuer);
  if(!successor.ok())std::cerr<<"successor inventory profile="<<profile<<" dense="<<dense<<" error="<<unsigned(successor.error)<<'\n';
  Check(successor.ok(),"later inventory publication preserves earlier preallocation lineage");}
 ++expected_history;verify();
 Check(f.device.Close().ok()&&f.device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"preallocation readonly reopen");verify();
 Check(f.device.Close().ok(),"preallocation close for independent reader");
 const auto child=fork();Check(child>=0,"preallocation child reader");if(!child){
  if(!f.device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok())_exit(80);
  try{verify();_exit(0);}catch(...){_exit(81);}
 }
 int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh process selected preallocation verification");
 std::cout<<"native preallocation profile="<<profile<<" dense="<<dense<<" PASS\n";
 if(!dense)OwnedPreallocationPublication(profile,true);
}
void OwnedPreallocationFaults(unsigned profile,bool dense=false,int secondary_profile=-1,bool reverse=false) {
 for(unsigned mode=1;mode<=6;++mode){
  std::unique_ptr<DirectoryHistoryFixture> mixed;std::unique_ptr<Fixture> single;
  if(secondary_profile>=0)mixed=std::make_unique<DirectoryHistoryFixture>(profile,secondary_profile,reverse,3,true,true,true,false);
  else single=std::make_unique<Fixture>(profile,DenseCapacity(profile,dense));
  auto& f=mixed?mixed->t.fixture:*single;
  f.budget=mixed?16384*std::max<u64>(f.size,d::kCanonicalFilespacePageProfiles[secondary_profile].page_size_bytes):8*f.budget;
  OwnedPreallocationRequest request(f,0,0,dense,mixed?mixed->t.other:Id(2));
  const auto first=request.storage.first_page,count=request.storage.page_count;
  const auto range=[&]{auto* file=mixed?mixed->File(request.storage.filespace_uuid):&f.device;Bytes raw(count*u64{request.storage.page_size_bytes});
   const auto io=file->ReadAt(first*u64{request.storage.page_size_bytes},raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size(),"actual physical fault target bytes");return raw;};
  const auto before=range();preallocation_calls=reads=writes=syncs=0;
  preallocation_fault=mode;io_counting=true;
  const auto result=db::PublishNativePreallocationOnLease(*request.held.lease,request.record,f.budget,*f.issuer);
  io_counting=false;preallocation_fault=read_fault=write_fault=sync_fault=0;
  Check(preallocation_calls==1&&result.physical_attempted&&result.physical&&
    result.publication.effects.installed_graph_verified&&range()==before,
    "physical preallocation failure retains original result and unchanged page bytes");
  if(mode==3){
   Check(result.ok()&&!result.physical->platform_preallocation_succeeded&&result.physical->logical_extent_available,
     "unsupported physical reserve is logical allocation only");continue;
  }
  Check(!result.ok()&&!result.publication.snapshot&&result.publication.error==db::NativePublicationError::io_failure,
    "injected physical or publication failure cannot report completion");
  Check(result.physical->platform_preallocation_succeeded==(mode>=4)&&result.physical_sync_completed==(mode>=5),
    "physical reserve and synchronization dimensions remain distinct");
  const auto no_retry=db::PublishNativePreallocationOnLease(*request.held.lease,request.record,f.budget,*f.issuer);
  Check(!no_retry.ok()&&!no_retry.physical_attempted&&no_retry.publication.error==db::NativePublicationError::stale_base,
    "failed lease cannot execute another physical request");
  request.held.lease.reset();
  if(mode==6){
   const auto unresolved=db::ReconcileNativePreallocationFromOpenDevices(Id(1),f.devices,Id(2),request.record,f.budget);
   Check(!unresolved.ok()&&!unresolved.observation,"torn selector state requires recovery and cannot return false absence");
   const auto recovered=db::RecoverNativeManagementCheckpointPublicationOnOpenDevices(Id(1),f.devices,Id(2),Id(21100),request.intent,f.budget);
   Check(recovered.ok()&&recovered.effects.selected_graph_verified,"partial selector publication recovers exact installed preallocation");
  }else{
   const auto pending=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);
   Check(pending.ok(),"failed preallocation retains pending generation");
   const auto observed=db::ReconcileNativePreallocationFromOpenDevices(Id(1),f.devices,Id(2),request.record,f.budget);
   Check(observed.ok()&&observed.observation->disposition==db::NativePreallocationDisposition::pending_anchored&&
     observed.observation->publication_attempt_uuid==Id(21100),"failed physical call reconciles original anchored attempt");
   auto resumed=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*pending.snapshot,Id(21100),request.intent,f.budget);
   Check(resumed.ok(),"original preallocation resume lease");
   auto conflict=request.record;auto changed=request.storage;changed.allocation_owner_uuid=Id(22000);
   conflict.normalized_request_bytes=db::EncodeNativeStorageActionIntent(changed,f.budget).bytes;
   conflict.normalized_request_sha256=Sha(conflict.normalized_request_bytes);
   const auto conflict_receipt=db::ReconcileNativePreallocationFromOpenDevices(Id(1),f.devices,Id(2),conflict,f.budget);
   Check(!conflict_receipt.ok()&&conflict_receipt.error==db::NativePublicationError::request_mismatch&&!conflict_receipt.observation,
     "anchored same-identity conflict refuses rather than appearing to be another operation");
   const auto retained=f.Read(0,256);reads=writes=syncs=0;io_counting=true;
   const auto refused=db::PublishNativePreallocationOnLease(*resumed.lease,conflict,f.budget,*f.issuer);
   io_counting=false;Check(!refused.ok()&&!refused.physical_attempted&&!writes&&!syncs&&f.Read(0,256)==retained,
     "conflicting owner retry cannot replace retained native request or write");
   const auto retried=db::PublishNativePreallocationOnLease(*resumed.lease,request.record,f.budget,*f.issuer);
   if(!retried.ok())std::cerr<<"preallocation retry mode="<<mode<<" error="<<unsigned(retried.publication.error)<<'\n';
   Check(retried.ok(),"exact original pending preallocation resumes to durable selected graph");
  }
  const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(selected.ok(),"physical recovery selected graph");const auto& s=*selected.selection;
  const d::FilespaceRootReference root{9,0x300,Id(2),s.checkpoint.page_number,s.checkpoint.page_generation,s.checkpoint.page_size_profile_uuid,s.checkpoint_object_uuid};
  const auto capacity=db::ReadNativeFilespaceCapacityFromOpenDevices(Id(1),f.devices,root,request.storage.filespace_uuid,f.budget);
  Check(capacity.ok()&&capacity.observation->state_counts[7]==count&&range()==before,
    "recovery preserves exact native preallocation and uninitialized page bytes");
 }
 std::cout<<"native preallocation faults profile="<<profile<<" dense="<<dense<<" PASS\n";
 if(!dense&&secondary_profile<0)OwnedPreallocationFaults(profile,true);
}
void OwnedPreallocationRepeat(unsigned profile) {
 for(unsigned reused=0;reused<3;++reused){
  Fixture f(profile);f.budget*=8;OwnedPreallocationRequest first(f);
  Check(db::PublishNativePreallocationOnLease(*first.held.lease,first.record,f.budget,*f.issuer).ok(),"first actual allocation for replay test");
  first.held.lease.reset();OwnedPreallocationRequest next(f,1,reused);const auto before=f.Read(0,256);
  reads=writes=syncs=0;io_counting=true;
  const auto result=db::PublishNativePreallocationOnLease(*next.held.lease,next.record,f.budget,*f.issuer);
  io_counting=false;
  if(reused){
   Check(!result.ok()&&result.publication.error==db::NativePublicationError::request_mismatch&&
     !result.physical_attempted&&!writes&&!syncs&&f.Read(0,256)==before,
     "fresh-capacity request with reused request or operation identity refuses before storage effects");
  }else{
   if(!result.ok())std::cerr<<"repeat preallocation profile="<<profile<<" error="<<unsigned(result.publication.error)<<'\n';
   Check(result.ok(),"distinct admitted requests can allocate separate ranges");next.held.lease.reset();
   const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
   Check(selected.ok()&&selected.allocation.state_counts[7]==8,"separate requests preserve both exact preallocated ranges");
   const auto a=db::ReconcileNativePreallocationFromOpenDevices(Id(1),f.devices,Id(2),first.record,f.budget);
   const auto b=db::ReconcileNativePreallocationFromOpenDevices(Id(1),f.devices,Id(2),next.record,f.budget);
   Check(a.ok()&&b.ok()&&a.observation->disposition==db::NativePreallocationDisposition::selected&&
     b.observation->disposition==db::NativePreallocationDisposition::selected&&
     a.observation->publication_generation==2&&b.observation->publication_generation==3&&
     a.observation->publication_attempt_uuid!=b.observation->publication_attempt_uuid,
     "lookup retains each original publication rather than substituting newest receipt");
  }
 }
 std::cout<<"native preallocation repeat profile="<<profile<<" PASS\n";
}
void OwnedPreallocationReconciliationFaults(unsigned stage,unsigned route,unsigned shard) {
 Fixture f(0);f.budget*=8;OwnedPreallocationRequest request(f);
 if(stage){
  if(stage==1)preallocation_fault=1;
  const auto effect=db::PublishNativePreallocationOnLease(*request.held.lease,request.record,f.budget,*f.issuer);
  preallocation_fault=0;
  Check(stage==1?(!effect.ok()&&effect.physical_attempted):effect.ok(),"reconciliation actual pending or selected fixture");
  request.held.lease.reset();
  if(stage==2){OwnedInventoryRequest next(f);Check(db::PublishNativeInventoryOnLease(*next.held.lease,next.record,next.inventory,f.budget,*f.issuer).ok(),
    "reconciliation after actual later publication");}
 }
 byte scratch=0;for(unsigned n=0;n<4097;++n)Check(f.device.ReadAt(0,&scratch,1).ok(),"stable read observation capacity");
 const auto original=f.Read(0,256);
 const auto call=[&]{return db::ReconcileNativePreallocationFromOpenDevices(Id(1),f.devices,Id(2),request.record,f.budget);};
 reads=writes=syncs=hash_seen=allocations=0;io_counting=hash_counting=counting=true;
 const auto baseline=call();io_counting=hash_counting=counting=false;
 Check(baseline.ok()&&!writes&&!syncs,"reconciliation baseline reads only");
 const auto sites=route==0?reads:route==1?hash_seen:allocations;Check(sites>0,"reconciliation fault sites exist");
 for(unsigned long at=route==2?shard:0;at<sites;at+=route==2?8:1){
  const auto loss=f.device.failed_io_latency_observations();reads=writes=syncs=hash_seen=0;io_counting=true;
  if(route==0)read_fault=at+1;
  if(route==1){hash_fault=shard+1;hash_target=at+1;hash_active=false;}
  if(route==2)allocation_budget=at;
  const auto result=call();const bool consumed=route==0?reads>=at+1:route==1?!hash_fault:allocation_budget==-1;
  io_counting=hash_counting=counting=false;read_fault=hash_fault=0;hash_active=false;allocation_budget=-1;
  Check(consumed&&!writes&&!syncs,"every reconciliation fault consumed without mutation");
  if(result.ok())Check(route==2&&f.device.failed_io_latency_observations()==loss+1&&
    result.observation->disposition==baseline.observation->disposition,"only recorded optional latency loss can return observation");
  else{
   const auto expected=route==0?db::NativePublicationError::io_failure:route==1?db::NativePublicationError::hash_failure:db::NativePublicationError::resource_exhausted;
   if(result.error!=expected)std::cerr<<"reconcile stage="<<stage<<" route="<<route<<" at="<<at<<" error="<<unsigned(result.error)<<'\n';
   Check(result.error==expected&&!result.observation,"reconciliation failure preserves cause without false absence or completion");
  }
 }
 Check(f.Read(0,256)==original,"reconciliation sweep preserves complete physical file");
 std::cout<<"native preallocation reconcile stage="<<stage<<" route="<<route<<" shard="<<shard<<" sites="<<sites<<" PASS\n";
}
void OwnedPreallocationCrashes(unsigned profile,bool dense=false,int secondary_profile=-1,bool reverse=false) {
 std::unique_ptr<DirectoryHistoryFixture> mixed;std::unique_ptr<Fixture> single;
 if(secondary_profile>=0)mixed=std::make_unique<DirectoryHistoryFixture>(profile,secondary_profile,reverse,3,true,true,true,false);
 else single=std::make_unique<Fixture>(profile,DenseCapacity(profile,dense));
 auto& f=mixed?mixed->t.fixture:*single;
 f.budget=mixed?16384*std::max<u64>(f.size,d::kCanonicalFilespacePageProfiles[secondary_profile].page_size_bytes):8*f.budget;
 OwnedPreallocationRequest request(f,0,0,dense,mixed?mixed->t.other:Id(2));
 const auto first=request.storage.first_page,count=request.storage.page_count;
 const auto pending=request.held.lease->snapshot();
 std::vector<std::pair<d::FileDevice*,Bytes>> originals;
 for(const auto& member:f.devices){const auto size=member.device->Size();Check(size.ok(),"crash fixture member size");Bytes raw(size.size_bytes);
  const auto io=member.device->ReadAt(0,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size(),"complete crash fixture original member");originals.emplace_back(member.device,std::move(raw));}
 const auto reopen=[&]{if(mixed)mixed->Reopen(true);else Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"reopen primary crash fixture");};
 const auto generation=pending.selection.selection_generation+1;
 reads=writes=syncs=0;io_counting=true;
 const auto baseline=db::PublishNativePreallocationOnLease(*request.held.lease,request.record,f.budget,*f.issuer);
 io_counting=false;Check(baseline.ok(),"preallocation crash sweep baseline");const auto nw=writes,ns=syncs;
 request.held.lease.reset();unsigned completed=0;
 for(unsigned kind=0;kind<6;++kind)for(unsigned at=1;at<=(kind<2?nw:kind<4?ns:1);++at){
  for(const auto& [device,original]:originals){const auto write=device->WriteAt(0,original.data(),original.size());
   Check(write.ok()&&write.bytes_transferred==original.size()&&device->Sync().ok(),"restore own complete crash fixture member");
   Check(device->Close().ok(),"close every parent member before real preallocation interruption");}
  const auto child=fork();Check(child>=0,"fork actual interrupted preallocation writer");
  if(!child){
   reopen();const auto& files=f.devices;
   auto held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),pending,Id(21100),request.intent,f.budget);
   if(!held.ok())_exit(81);f.ResetIssuer();reads=writes=syncs=0;
   kill_write=kind<2?at:0;kill_sync=kind>=2&&kind<4?at:0;kill_after_sync=kind==3;
   torn_bytes=kind==1?f.size/2:0;preallocation_death=kind>=4?kind-3:0;io_counting=true;
   (void)db::PublishNativePreallocationOnLease(*held.lease,request.record,f.budget,*f.issuer);_exit(87);
  }
  int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,
    "preallocation writer stops at every write sync and physical reserve boundary");
  const auto recovery=fork();Check(recovery>=0,"fork independent original-request preallocation recovery");
  if(!recovery){
   reopen();const auto& files=f.devices;
   const auto seen=db::RecoverNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),f.budget);
   db::NativePublicationInspection result;
   if(seen.ok()){
    if(seen.snapshot->selection.selection_generation==generation)result=seen;
    else{
     auto held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),*seen.snapshot,Id(21100),request.intent,f.budget);
     if(!held.ok())_exit(81);f.ResetIssuer();
     const auto retried=db::PublishNativePreallocationOnLease(*held.lease,request.record,f.budget,*f.issuer);
     if(!retried.ok())_exit(82);result=retried.publication;
    }
   }else result=db::RecoverNativeManagementCheckpointPublicationOnOpenDevices(Id(1),files,Id(2),Id(21100),request.intent,f.budget);
   if(!result.ok())_exit(83);
   const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),files,Id(2),f.budget);
   const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),files,Id(2),f.budget);
   u64 reserved=0;
   if(selected.ok()){const auto& s=*selected.selection;
    const d::FilespaceRootReference root{9,0x300,Id(2),s.checkpoint.page_number,s.checkpoint.page_generation,s.checkpoint.page_size_profile_uuid,s.checkpoint_object_uuid};
    const auto capacity=db::ReadNativeFilespaceCapacityFromOpenDevices(Id(1),files,root,request.storage.filespace_uuid,f.budget);
    if(!capacity.ok())_exit(88);reserved=capacity.observation->state_counts[7];}
   _exit(selected.ok()&&selected.selection->selection_generation==generation&&reserved==count&&
     history.ok()&&history.entries.size()==(dense?2u:1u)&&history.entries.back().plan.operation_uuid==Id(21100)&&
     history.entries.back().record.normalized_request_bytes==request.record.normalized_request_bytes?0:84);
  }
  Check(waitpid(recovery,&status,0)==recovery&&WIFEXITED(status),"preallocation recovery process exits normally");
  if(WEXITSTATUS(status))std::cerr<<"preallocation crash profile="<<profile<<" kind="<<kind<<" at="<<at<<" status="<<WEXITSTATUS(status)<<'\n';
  Check(WEXITSTATUS(status)==0,"every interrupted preallocation resumes original request without abandonment");
  reopen();auto* target=mixed?mixed->File(request.storage.filespace_uuid):&f.device;
  Bytes raw(count*u64{request.storage.page_size_bytes});const auto io=target->ReadAt(first*u64{request.storage.page_size_bytes},raw.data(),raw.size());
  Check(io.ok()&&io.bytes_transferred==raw.size()&&raw==Bytes(raw.size()),"recovery preserves uninitialized reserved bytes");++completed;
 }
 std::cout<<"native preallocation crash profile="<<profile<<" dense="<<dense<<" boundaries="<<completed<<" PASS\n";
 if(!dense&&!mixed)OwnedPreallocationCrashes(profile,true);
}
void OwnedPreallocationSweeps(unsigned route,unsigned shard,bool dense=false,bool secondary=false) {
 std::unique_ptr<DirectoryHistoryFixture> mixed;std::unique_ptr<Fixture> single;
 if(secondary)mixed=std::make_unique<DirectoryHistoryFixture>(0,1,true,3,true,true,true,false);
 else single=std::make_unique<Fixture>(0,DenseCapacity(0,dense));
 auto& f=mixed?mixed->t.fixture:*single;f.budget=mixed?16384*16384:8*f.budget;
 OwnedPreallocationRequest request(f,0,0,dense,mixed?mixed->t.other:Id(2));
 byte scratch=0;for(const auto& file:f.devices)for(unsigned n=0;n<4097;++n)Check(file.device->ReadAt(0,&scratch,1).ok(),"stable preallocation observation capacity");
 const auto pending=request.held.lease->snapshot();
 std::vector<std::pair<d::FileDevice*,Bytes>> originals;
 for(const auto& member:f.devices){const auto size=member.device->Size();Check(size.ok(),"fault fixture member size");Bytes raw(size.size_bytes);
  const auto io=member.device->ReadAt(0,raw.data(),raw.size());Check(io.ok()&&io.bytes_transferred==raw.size(),"complete fault fixture original member");originals.emplace_back(member.device,std::move(raw));}
 const auto call=[&]{return db::PublishNativePreallocationOnLease(*request.held.lease,request.record,f.budget,*f.issuer);};
 reads=writes=syncs=hash_seen=allocations=0;io_counting=hash_counting=counting=true;
 const auto baseline=call();io_counting=hash_counting=counting=false;
 Check(baseline.ok(),"preallocation fault sweep baseline");
 const auto sites=route==0?reads:route==1?hash_seen:route==2?allocations:route==4?syncs:writes;
 Check(sites>0,"preallocation fault sweep sites exist");
 const auto reset=[&]{
  request.held.lease.reset();for(const auto& [device,original]:originals){const auto write=device->WriteAt(0,original.data(),original.size());
   Check(write.ok()&&write.bytes_transferred==original.size()&&device->Sync().ok(),"restore only owned fault fixture members");}f.ResetIssuer();
  request.held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),pending,Id(21100),request.intent,f.budget);
  Check(request.held.ok(),"resume exact original fixture generation");
 };
 const auto losses=[&]{u64 total=0;for(const auto& file:f.devices)total+=file.device->failed_io_latency_observations();return total;};
 for(unsigned long at=route==2?shard:0;at<sites;at+=(route==2?(secondary?32:8):1)){
  reset();const auto loss=losses();reads=writes=syncs=hash_seen=0;
  if(route==0){read_fault=at+1;io_counting=true;}
  if(route==1){hash_fault=secondary?shard+1:1;hash_target=at+1;hash_active=false;}
  if(route==2)allocation_budget=at;
  if(route>=3){io_counting=true;if(route==4)sync_fault=at+1;else{write_fault=at+1;torn_bytes=route==5?256:0;}}
  const auto result=call();const bool consumed=route==0?reads>=at+1:route==1?!hash_fault:route==2?allocation_budget==-1:route==4?syncs>=at+1:writes>=at+1;
  io_counting=hash_counting=counting=false;read_fault=write_fault=sync_fault=hash_fault=0;torn_bytes=0;hash_active=false;allocation_budget=-1;
  Check(consumed,"every new preallocation fault site consumed");
  if(result.ok()){
   Check(route==2&&losses()==loss+1,"only explicitly recorded optional observation loss can succeed");
  }else{
   if(result.publication.snapshot||result.publication.error!=(route==0||route>=3?db::NativePublicationError::io_failure:
      route==1?db::NativePublicationError::hash_failure:db::NativePublicationError::resource_exhausted))
    std::cerr<<"preallocation fault route="<<route<<" at="<<at<<" error="<<unsigned(result.publication.error)<<'\n';
   Check(!result.publication.snapshot&&result.publication.error==(route==0||route>=3?db::NativePublicationError::io_failure:
     route==1?db::NativePublicationError::hash_failure:db::NativePublicationError::resource_exhausted),
     "preallocation preserves exact I/O hash or allocation failure");
  }
  Check(result.publication.effects==request.held.lease->effects(),"failed result retains cumulative actual publication effects");
  auto* target=mixed?mixed->File(request.storage.filespace_uuid):&f.device;
  Bytes raw(request.storage.page_count*u64{request.storage.page_size_bytes});const auto io=target->ReadAt(request.storage.first_page*u64{request.storage.page_size_bytes},raw.data(),raw.size());
  Check(io.ok()&&io.bytes_transferred==raw.size()&&raw==Bytes(raw.size()),"fault never initializes reserved data pages");
 }
 std::cout<<"native preallocation sweep route="<<route<<" shard="<<shard<<" dense="<<dense<<" sites="<<sites<<" PASS\n";
 if(!dense&&!secondary)OwnedPreallocationSweeps(route,shard,true);
}
void OwnedInventoryPlacement(unsigned layout) {
 Fixture f(0);f.budget*=4;
 const auto base=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
 Check(base.ok(),"owned placement actual allocation");
 std::vector<u64> free;
 for(const auto& image:base.allocation.pages)for(std::size_t n=0;n<image.map->states.size();++n)
  if(image.map->states[n]==page::NativeAllocationState::free)free.push_back(image.map->first_page+n);
 Check(free.size()>20,"owned placement free extent");
 Bytes dirty(f.size);dirty.back()=0x5a;
 std::set<u64> occupied;
 for(std::size_t n=0;n<free.size();++n)if(layout==1?(n<16&&n%3==0):true) {
  const auto write=f.device.WriteAt(free[n]*f.size,dirty.data(),dirty.size());
  Check(write.ok()&&write.bytes_transferred==dirty.size(),"dirty free actual fixture");occupied.insert(free[n]);
 }
 Check(f.device.Sync().ok(),"dirty free fixture barrier");
 OwnedInventoryRequest request(f);const auto before=f.Read(0,256);
 const auto result=db::PublishNativeInventoryOnLease(*request.held.lease,request.record,request.inventory,f.budget,*f.issuer);
 if(layout==2) {
  Check(result.error==db::NativePublicationError::allocation_exhausted&&!result.snapshot&&f.Read(0,256)==before,
    "all dirty free slots exhaust allocation without overwrite or false receipt");return;
 }
 Check(result.ok(),"fragmented primary inventory actually publishes");
 const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
 Check(selected.ok(),"fragmented inventory ordinary admission");
 const auto plan=db::DecodeNativePublicationPlan(f.Read(result.snapshot->watermark.publication_plan->page.page_number));
 Check(plan.ok(),"fragmented actual plan");
 std::set<u64> used=occupied;
 const auto take=[&](u64 count) {
  std::vector<u64> run;
  for(const auto n:free) {
   if(used.contains(n)){run.clear();continue;}
   if(!run.empty()&&n!=run.back()+1)run.clear();run.push_back(n);
   if(run.size()==count){used.insert(run.begin(),run.end());return run.front();}
  }
  throw std::runtime_error("independent placement oracle exhausted");
 };
 Check(plan.plan->management_extent->first.page_number==take(plan.plan->management_extent->page_count),"lowest zero extent run");
 Check(plan.plan->control_bundle->first.page_number==take(plan.plan->control_bundle->page_count),"lowest remaining zero bundle run");
 Check(plan.plan->header.page_number==take(1)&&plan.plan->target_checkpoint.page_number==take(1),"lowest remaining plan and checkpoint slots");
 for(const auto& image:selected.allocation.pages)Check(image.map->header.page_number==take(1),"lowest remaining allocation-map slot");
 for(const auto& image:selected.checkpoint_inventory.inventory_pages)Check(image.header.page_number==take(1),"lowest remaining inventory slot");
 for(const auto n:occupied)Check(f.Read(n)==dirty,"dirty free orphan bytes remain unchanged");
 std::cout<<"owned inventory fragmented placement PASS\n";
}
void OwnedInventoryBudgetAndStale() {
 Fixture f(0);f.budget*=4;OwnedInventoryRequest request(f);
 const auto before=f.Read(0,256);
 const auto bound=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
 const auto record=db::EncodeNativeManagementOperation(request.record,f.budget);
 Check(bound.ok()&&record.ok(),"owned exact allowance independent inputs");
 const auto ceil=[](u64 n,u64 d){return n/d+(n%d!=0);};
 const u64 maps=bound.allocation.pages.size(),inventory=1,extent=ceil(record.bytes.size(),f.size-384);
 const u64 bundle=ceil((maps+inventory)*f.size,f.size-384);
 const u64 probes=extent+bundle+2+maps+inventory;
 const auto directory_root=std::find_if(bound.checkpoint_inventory.checkpoint->roots.begin(),bound.checkpoint_inventory.checkpoint->roots.end(),[](const auto& r){return r.role==3;});
 Check(directory_root!=bound.checkpoint_inventory.checkpoint->roots.end(),"independent allowance directory source");const auto& dir=*directory_root;
 const auto directory=page::ReadNativeFilespaceDirectoryFromOpenDevices(Id(1),f.devices,{5,9,dir.page.filespace_uuid,dir.page.page_number,dir.page.page_generation,dir.page.page_size_profile_uuid,dir.object_uuid},f.budget);
 Check(directory.ok(),"actual source directory retained-image allowance");
 const u64 allowance=bound.retained_image_bytes+2*directory.retained_image_bytes+(20+10*maps+14*inventory+4*extent+4*bundle+probes)*f.size+4*record.bytes.size();
 entropy_fault=1;
 const auto short_work=db::PublishNativeInventoryOnLease(*request.held.lease,request.record,request.inventory,allowance-1,*f.issuer);
 Check(entropy_fault==1&&short_work.error==db::NativePublicationError::resource_exhausted&&!short_work.snapshot&&f.Read(0,256)==before,
   "one byte short of independent construction charge refuses before issuing identity or writing");
 const auto exact=db::PublishNativeInventoryOnLease(*request.held.lease,request.record,request.inventory,allowance,*f.issuer);
 Check(!entropy_fault&&exact.error==db::NativePublicationError::identity_failure&&!exact.snapshot&&f.Read(0,256)==before,
   "exact independent construction allowance reaches real identity issuance without mutation");
 repeated_entropy=true;entropy_calls=0;owned_clock_controlled=true;owned_clock_millis=u64{1}<<40;f.ResetIssuer();
 const auto collision=db::PublishNativeInventoryOnLease(*request.held.lease,request.record,request.inventory,f.budget,*f.issuer);
 repeated_entropy=false;owned_clock_controlled=false;
 Check(entropy_calls==1&&collision.error==db::NativePublicationError::identity_failure&&!collision.snapshot&&f.Read(0,256)==before,
   "newly generated binary UUID colliding with retained database identity refuses without substitute or graph write");
 const auto abandoned=db::AbandonNativeInventoryPublicationOnOpenDevices(Id(1),f.devices,Id(2),request.pending,Id(20100),request.intent,Id(20500),f.budget);
 Check(abandoned.ok(),"owning explicit resolution changes actual pending snapshot");const auto resolved=f.Read(0,256);
 const auto stale=db::PublishNativeInventoryOnLease(*request.held.lease,request.record,request.inventory,f.budget,*f.issuer);
 Check(stale.error==db::NativePublicationError::stale_base&&!stale.snapshot&&f.Read(0,256)==resolved,
   "constructor rereads actual state and refuses obsolete retained lease");
 std::cout<<"owned inventory exact construction allowance="<<allowance<<" stale-base PASS\n";
}
void ReservationEffects(unsigned profile,bool startup=false) {
 Fixture f(profile);f.budget*=4;
 const auto base=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);
 Check(base.ok()&&base.effects.observed&&!base.effects.write_attempts&&!base.effects.sync_attempts,"inspection records known zero effects");
 const auto before=f.Read(0,256);
 db::NativePublicationIntent intent{Id(20101),Id(20102),Id(20103),{},4,2};intent.normalized_request_sha256.fill(7);
 if(startup){const auto actual=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
   Check(actual.ok(),"actual next transaction before startup reservation");
   intent.startup_binding=db::NativeStartupBinding{Id(20110),Id(20102),Id(20111),actual.checkpoint_inventory.inventory.next_local_transaction_id,19};}
 for(unsigned fault=0;fault<8;++fault){
  const auto restored=f.device.WriteAt(0,before.data(),before.size());Check(restored.ok()&&f.device.Sync().ok(),"isolated reservation effect fixture restore");
  reads=writes=syncs=0;write_fault=fault>=1&&fault<=4?1+(fault-1)%2:0;
  torn_bytes=fault==3||fault==4?f.size/2:0;sync_fault=fault>=5?fault-4:0;
  io_counting=true;auto result=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*base.snapshot,Id(20100),f.budget,&intent);io_counting=false;
  write_fault=sync_fault=0;torn_bytes=0;const auto& effect=result.effects;
  Check(effect.observed&&effect.write_attempts==writes&&effect.attempted_bytes==u64{writes}*f.size&&
    effect.sync_attempts==syncs&&effect.successful_syncs==syncs-(fault>=5?1:0)&&
    effect.uncertain_write==(fault>=1&&fault<=4)&&!effect.selector_write_attempted&&!effect.selected_graph_verified,
    "reservation result retains effects even when no lease can be returned");
  if(!fault){Check(result.ok()&&result.lease->effects()==effect,"successful reservation copies exact cumulative interval");
    const auto pending=result.lease->snapshot();result.lease.reset();
    auto resumed=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),pending,Id(20100),intent,f.budget);
    Check(resumed.ok()&&resumed.effects.observed&&!resumed.effects.write_attempts&&resumed.effects.sync_attempts&&
      resumed.lease->effects()==resumed.effects,"resume starts a new interval without erasing the original durable pending request");
  }else Check(result.error==db::NativePublicationError::io_failure&&!result.lease,"failed reservation returns no live lease");
  if(fault==3||fault==4)Check(f.Read(0,256)!=before,"torn reservation retains real changed bytes");
  if(startup){result.lease.reset();const auto recovered=db::RecoverNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);
    Check(recovered.ok(),"startup reservation duplex is recoverable after every write and sync fault");
    const auto& w=recovered.snapshot->watermark;
    Check((w.operation_uuid==Id(20100)&&w.intent==intent)||
      (w.operation_uuid==base.snapshot->watermark.operation_uuid&&!w.intent),"recovery preserves exact startup tuple or proves no new reservation selected");
    Check(recovered.snapshot->selection.checkpoint==base.snapshot->selection.checkpoint&&
      recovered.snapshot->selection.checkpoint_sha256==base.snapshot->selection.checkpoint_sha256,"reservation faults do not allocate or activate a transaction");}
 }
 if(startup)for(unsigned kind=0;kind<4;++kind)for(unsigned at=1;at<=(kind<2?2u:3u);++at){
   Check(f.device.WriteAt(0,before.data(),before.size()).ok()&&f.device.Sync().ok()&&f.device.Close().ok(),"restore and release own startup crash fixture");
   const auto child=fork();Check(child>=0,"fork startup reservation interruption");
   if(!child){if(!f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok())_exit(80);
     writes=syncs=reads=0;kill_write=kind<2?at:0;kill_sync=kind>=2?at:0;kill_after_sync=kind==3;torn_bytes=kind==1?f.size/2:0;io_counting=true;
     (void)db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*base.snapshot,Id(20100),f.budget,&intent);_exit(87);}
   int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"startup writer dies at requested physical barrier");
   const auto reader=fork();Check(reader>=0,"fork independent startup binding recovery");
   if(!reader){d::FileDevice device;if(!device.Open(f.path.string(),d::FileOpenMode::open_existing).ok())_exit(80);
     std::vector<d::NativeFilespaceDevice> files{{Id(2),d::kCanonicalFilespacePageProfiles[profile].uuid,&device}};
     const auto recovered=db::RecoverNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),f.budget);
     if(!recovered.ok())_exit(81);const auto& w=recovered.snapshot->watermark;
     if(!((w.operation_uuid==Id(20100)&&w.intent==intent)||(w.operation_uuid==base.snapshot->watermark.operation_uuid&&!w.intent)))_exit(82);
     if(recovered.snapshot->selection.checkpoint_sha256!=base.snapshot->selection.checkpoint_sha256)_exit(83);
     if(w.intent){auto resumed=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),*recovered.snapshot,Id(20100),intent,f.budget);
       if(!resumed.ok()||resumed.lease->snapshot().watermark.intent!=intent)_exit(84);}
     _exit(0);}
   Check(waitpid(reader,&status,0)==reader&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"independent reopen retains exact original binding before any BEGIN inventory effects");
   Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"startup parent reopens after independent recovery");
 }
}
void OwnedInventoryFaults(unsigned route,unsigned shard,unsigned profile=0,bool directory=false,bool retained_history=false,bool startup=false,bool activation=false) {
 using PE=db::NativePublicationError;
 std::unique_ptr<DirectoryHistoryFixture> mixed;std::unique_ptr<Fixture> single;
 if(directory)mixed=std::make_unique<DirectoryHistoryFixture>(profile,(profile+1)%5,true,2,false,true,true,false);
 else single=std::make_unique<Fixture>(profile);
 auto& f=directory?mixed->t.fixture:*single;f.budget*=directory?32:4;
 if(retained_history){Check(route<3,"retained history fault routes");f.budget*=4;OwnedInventoryRequest original(f);
  Check(db::PublishNativeInventoryOnLease(*original.held.lease,original.record,original.inventory,f.budget,*f.issuer).ok(),"actual prior selected record for append fault coverage");}
 OwnedInventoryRequest request(f,retained_history?1:0,false,startup,activation);
 const auto loss=[&]{u64 total=0;for(const auto& file:f.devices)total+=file.device->failed_io_latency_observations();return total;};
 const auto before=f.Read(0,f.total_pages);
 const auto reset=[&] {
  request.held.lease.reset();const auto write=f.device.WriteAt(0,before.data(),before.size());
  Check(write.ok()&&write.bytes_transferred==before.size()&&f.device.Sync().ok(),"restore isolated pending fault fixture");
  request.held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),request.pending,request.pending.watermark.operation_uuid,request.intent,f.budget);
  Check(request.held.ok(),"resume exact isolated pending request");
  f.ResetIssuer();
 };
 const auto call=[&](u64 budget){return db::PublishNativeInventoryOnLease(*request.held.lease,request.record,request.inventory,budget,*f.issuer);};
 byte scratch=0;for(const auto& file:f.devices)for(unsigned n=0;n<4097;++n)Check(file.device->ReadAt(0,&scratch,1).ok(),"stabilize optional observation allocations");
 owned_clock_controlled=true;owned_clock_millis=1700000000123ULL;
 // Genesis used the real clock. Start the fault experiment with a fresh issuer
 // bound to its controlled clock, as every subsequent reset already does.
 f.ResetIssuer();
 reads=writes=syncs=entropy_calls=hash_seen=0;allocations=0;
 const auto initial_effects=request.held.lease->effects();
 counting=hash_counting=io_counting=true;const auto good=call(f.budget);counting=hash_counting=io_counting=false;
 const auto nr=reads,nw=writes,ns=syncs,nh=hash_seen,ne=entropy_calls;const auto na=allocations;
 if(!good.ok())std::cerr<<"owned baseline startup="<<startup<<" activation="<<activation<<" error="<<int(good.error)<<'\n';
 Check(good.ok()&&nr&&nw&&ns&&nh&&ne&&na,"owned complete publication fault baseline");
 Check(good.effects==request.held.lease->effects()&&good.effects.observed&&
   good.effects.write_attempts==initial_effects.write_attempts+nw&&good.effects.sync_attempts==initial_effects.sync_attempts+ns&&
   good.effects.confirmed_bytes==good.effects.attempted_bytes&&!good.effects.uncertain_write&&good.effects.selected_graph_verified,
   "effect receipt counts actual primitive calls including retained reservation");
 std::cout<<"owned inventory fault baseline reads="<<nr<<" writes="<<nw<<" syncs="<<ns<<" hashes="<<nh<<" identities="<<ne<<" allocations="<<na<<'\n';
 if(route==0||route==4) {
  unsigned skipped_dirty=0;
  for(unsigned kind=0;kind<5;++kind)for(unsigned at=1;at<=(kind==0||kind==3?nr:kind==2?ns:nw);++at) {
   if(route==4&&(kind==0||kind==3))continue;
   reset();reads=writes=syncs=0;read_fault=kind==0?at:0;write_fault=kind==1||kind==4?at:0;
   sync_fault=kind==2?at:0;corrupt_read=kind==3?at:0;torn_bytes=kind==4?f.size/2:0;
   const auto prior=request.held.lease->effects();
   io_counting=true;const auto result=call(f.budget);io_counting=false;
   const bool consumed=kind==0||kind==3?reads>=at:kind==2?syncs>=at:writes>=at;
   const auto& effect=result.effects;
   Check(effect==request.held.lease->effects()&&effect.observed&&
     effect.write_attempts==prior.write_attempts+writes&&effect.sync_attempts==prior.sync_attempts+syncs&&
     effect.successful_syncs==prior.successful_syncs+syncs-((kind==2&&consumed)?1:0)&&
     effect.attempted_bytes==prior.attempted_bytes+u64{writes}*f.size&&effect.confirmed_bytes<=effect.attempted_bytes&&
     effect.uncertain_write==((kind==1||kind==4)&&consumed),"error receipt retains exact attempts and uncertainty instead of false zero effects");
   if(!result.ok())Check(!effect.selected_graph_verified,"failed selected admission never claims verified publication");
   if(kind==4&&consumed)Check(f.Read(0,f.total_pages)!=before,"torn write has real retained effects even when backend reports zero progress");
   read_fault=write_fault=sync_fault=corrupt_read=0;torn_bytes=0;
   if(kind==3&&consumed&&result.ok()) {
    Check(corrupt_was_zero&&corrupt_offset>=0&&u64(corrupt_offset)%f.size==0,
      "only a physically zero free-slot probe may admit an alternate destination after corruption");
    const u64 page_number=u64(corrupt_offset)/f.size;
    const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
    Check(selected.ok()&&selected.checkpoint_inventory.inventory.entries.size()==request.inventory.entries.size()&&
      selected.checkpoint_inventory.inventory.entries.back().identity.transaction_uuid.value==request.inventory.entries.back().identity.transaction_uuid.value&&
      selected.checkpoint_inventory.inventory.entries.back().identity.local_id.value==request.inventory.entries.back().identity.local_id.value&&
      selected.checkpoint_inventory.inventory.entries.back().state==request.inventory.entries.back().state,
      "dirty-probe substitution still publishes exact complete requested inventory");
    bool remains_free=false;
    for(const auto& image:selected.allocation.pages)if(page_number>=image.map->first_page&&page_number-image.map->first_page<image.map->states.size())
      remains_free=image.map->states[page_number-image.map->first_page]==page::NativeAllocationState::free&&
        std::none_of(image.map->records.begin(),image.map->records.end(),[&](const auto& r){return r.page_number==page_number;});
    Check(remains_free&&f.Read(page_number)==Bytes(f.size),"dirty-probed slot is neither allocated nor overwritten");
    ++skipped_dirty;continue;
   }
   if(!consumed||result.ok()||result.snapshot)std::cerr<<"owned I/O kind="<<kind<<" at="<<at<<" error="<<int(result.error)<<" reads="<<reads<<" writes="<<writes<<" syncs="<<syncs<<'\n';
   Check(consumed&&!result.ok()&&!result.snapshot,"owned publication consumes every I/O fault without receipt");
   if(kind!=3)Check(result.error==PE::io_failure,"owned publication preserves actual I/O errors");
   if(!writes)Check(f.Read(0,f.total_pages)==before,"read-only failure leaves every pending byte unchanged");
  }
  for(unsigned at=1;at<=ne;++at) {
   reset();entropy_fault=at;const auto result=call(f.budget);
   Check(!entropy_fault&&result.error==PE::identity_failure&&!result.snapshot&&f.Read(0,f.total_pages)==before,"every generated identity entropy failure precedes graph mutation");
  }
  if(route==0)Check(skipped_dirty>0,"read-corrupted free slots exercise truthful alternate placement");
  std::cout<<"owned inventory corrupted free probes skipped="<<skipped_dirty<<'\n';
 }else if(route==1) {
  for(unsigned at=retained_history?shard/5+1:1;at<=nh;at+=retained_history?4:1) {
   reset();hash_fault=(retained_history?shard%5:shard)+1;hash_target=at;hash_seen=0;hash_active=false;
   const auto result=call(f.budget);const bool consumed=!hash_fault;hash_fault=0;
   Check(consumed&&result.error==PE::hash_failure&&!result.snapshot,"owned publication consumes every digest fault without receipt");
  }
 }else if(route==2) {
  for(unsigned long at=shard;at<na;at+=retained_history?128:32) {
   reset();const auto previous_loss=loss();allocation_budget=at;
   const auto result=call(f.budget);const auto remaining=allocation_budget;allocation_budget=-1;
   Check(remaining<0&&(result.error==PE::resource_exhausted||(result.ok()&&loss()==previous_loss+1)),
    "owned publication consumes required allocation failures or records optional observation loss");
   Check(result.ok()||!result.snapshot,"allocation failure never returns publication receipt");
  }
 }else {
  // Deliberately stop the real writer, then recover using only its durable
  // inputs in a fresh process. No parent-supplied target images or UUIDs.
  unsigned completed=0,incomplete=0;
  for(unsigned kind=0;kind<4;++kind)for(unsigned at=1;at<=(kind<2?nw:ns);++at) {
   reset();request.held.lease.reset();Check(f.device.Close().ok(),"close parent before owned writer death");
   const auto child=fork();Check(child>=0,"fork actual owned writer");
   if(!child) {
    d::FileDevice file;if(!file.Open(f.path.string(),d::FileOpenMode::open_existing).ok())_exit(80);
    std::vector<d::NativeFilespaceDevice> files{{Id(2),d::kCanonicalFilespacePageProfiles[profile].uuid,&file}};
    auto held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),request.pending,request.pending.watermark.operation_uuid,request.intent,f.budget);
    if(!held.ok())_exit(81);f.ResetIssuer();reads=writes=syncs=0;kill_write=kind<2?at:0;kill_sync=kind>=2?at:0;
    kill_after_sync=kind==3;torn_bytes=kind==1?f.size/2:0;io_counting=true;
    (void)db::PublishNativeInventoryOnLease(*held.lease,request.record,request.inventory,f.budget,*f.issuer);_exit(87);
   }
   int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"owned writer dies at exact write or barrier");
   const auto recovery=fork();Check(recovery>=0,"fresh owned publication recovery process");
   if(!recovery) {
    d::FileDevice file;if(!file.Open(f.path.string(),d::FileOpenMode::open_existing).ok())_exit(80);
    std::vector<d::NativeFilespaceDevice> files{{Id(2),d::kCanonicalFilespacePageProfiles[profile].uuid,&file}};
    auto observed=db::RecoverNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),f.budget);
    db::NativePublicationInspection result;
    if(observed.ok()) {
     if(observed.snapshot->selection.selection_generation==request.pending.selection.selection_generation+1)result=observed;
     else if(observed.snapshot->watermark.publication_plan) {
      auto held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),*observed.snapshot,request.pending.watermark.operation_uuid,request.intent,f.budget);
      if(held.ok()) {
       result=db::ResumeNativeManagementControlGraphOnLease(*held.lease,f.budget);
       if(result.ok())result=db::PublishNativeManagementControlGraphOnLease(*held.lease,f.budget);
      }
     }else if(startup){
      if(observed.snapshot->watermark.intent!=request.intent)_exit(90);
      auto held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),*observed.snapshot,request.pending.watermark.operation_uuid,request.intent,f.budget);
      if(!held.ok())_exit(91);f.ResetIssuer();
      // Owning family request is supplied, never parent-built target pages or
      // replacement transaction/operation identities. This is storage proof,
      // not the 01f startup family's recovery/admission implementation.
      result=db::PublishNativeInventoryOnLease(*held.lease,request.record,request.inventory,f.budget,*f.issuer);
     }
    }else result=db::RecoverNativeManagementCheckpointPublicationOnOpenDevices(Id(1),files,Id(2),request.pending.watermark.operation_uuid,request.intent,f.budget);
    if(result.ok()) {
     if(!result.effects.observed)_exit(89);
     const auto final=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),files,Id(2),f.budget);
     if(startup){const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),files,Id(2),f.budget);
       if(!history.ok()||history.entries.empty()||history.entries.back().plan.intent.startup_binding!=request.intent.startup_binding||
         history.entries.back().record.normalized_request_bytes!=request.record.normalized_request_bytes)_exit(92);}
     _exit(final.ok()&&final.selection->selection_generation==request.pending.selection.selection_generation+1&&SameOwnedInventory(final.checkpoint_inventory.inventory,request.inventory)?0:82);
    }
    if(startup)_exit(93); // Startup may not use unbound physical abandonment.
    if(result.snapshot)_exit(83);
    // Incomplete immutable inputs cannot be invented. The original selected
    // inventory must still be intact and the pending generation resolvable.
    observed=db::RecoverNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),f.budget);
    if(!observed.ok())_exit(84);
    const auto abandoned=db::AbandonNativeInventoryPublicationOnOpenDevices(Id(1),files,Id(2),*observed.snapshot,Id(20100),request.intent,Id(20500),f.budget);
    const auto final=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),files,Id(2),f.budget);
    _exit(abandoned.ok()&&final.ok()&&final.selection->selection_generation==1&&final.checkpoint_inventory.inventory.entries.size()==1?10:85);
   }
   Check(waitpid(recovery,&status,0)==recovery&&WIFEXITED(status),"owned recovery process terminates normally");
   if(WEXITSTATUS(status)!=0&&WEXITSTATUS(status)!=10)std::cerr<<"owned recovery kind="<<kind<<" at="<<at<<" status="<<WEXITSTATUS(status)<<'\n';
   Check(WEXITSTATUS(status)==0||WEXITSTATUS(status)==10,"owned interrupted publication recovers exact inputs or explicitly abandons incomplete candidate");
   WEXITSTATUS(status)==0?++completed:++incomplete;
   Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"parent reopens recovered owned file");
  }
  Check(completed&&(startup?!incomplete:bool(incomplete)),"owned death sweep retains expected reconstruction and incomplete-input boundaries");
  std::cout<<"owned recovery completed="<<completed<<" incomplete="<<incomplete<<'\n';
 }
 owned_clock_controlled=false;
 std::cout<<"owned inventory fault route="<<route<<" shard="<<shard<<" PASS\n";
}
void InventoryPlan(Graph& g,const Bundle& b){
 auto p=g.plan;p.control_bundle=b.root;p.base_selection_generation=1;p.intent.recovery_profile=2;
 StartupPlan(p);
 const auto raw=Oracle(p),encoded=db::EncodeNativePublicationPlan(p).bytes;Check(encoded==raw,"independent complete inventory plan version6 bytes");
 const auto decoded=db::DecodeNativePublicationPlan(raw);Check(decoded.ok()&&decoded.plan->intent==p.intent&&decoded.plan->control_bundle==p.control_bundle&&decoded.plan->base_selection_generation==p.base_selection_generation&&db::EncodeNativePublicationPlan(*decoded.plan).bytes==raw,"version6 preserves complete descriptor and request");
 for(unsigned kind=0;kind<13;++kind){auto bad=p;switch(kind){case 0:bad.intent.recovery_profile=0;break;case 1:bad.intent.recovery_profile=1;break;case 2:bad.intent.recovery_profile=3;break;case 3:bad.control_bundle.reset();break;case 4:bad.management_extent.reset();break;case 5:bad.base_selection_generation.reset();break;case 6:bad.base_selection_generation=0;break;case 7:bad.base_selection_generation=std::numeric_limits<u64>::max();break;case 8:bad.control_bundle->inventory_count=0;break;case 9:bad.control_bundle->inventory_count=std::numeric_limits<u64>::max();break;case 10:++bad.control_bundle->inventory_count;break;case 11:bad.control_bundle->map_count=std::numeric_limits<u64>::max();break;case 12:++bad.control_bundle->page_count;break;}Bad(bad);}
 for(unsigned at:{135u,136u,138u,140u,1048u,1049u,1050u,1055u,1056u,1063u,1064u,1151u,1152u}){auto bad=raw;bad[at]^=1;PlanSeal(bad);Failed(db::DecodeNativePublicationPlan(bad));}
 for(unsigned version=1;version<6;++version){auto bad=raw;bad[135]='0'+version;Num(bad,136,2,version);if(version==5)Num(bad,1048,2,1);PlanSeal(bad);Failed(db::DecodeNativePublicationPlan(bad));}
 if(g.fixture.size!=d::kCanonicalFilespacePageProfiles[0].page_size_bytes)return;
 for(unsigned route=0;route<2;++route){const auto call=[&]{return route?db::DecodeNativePublicationPlan(raw):db::EncodeNativePublicationPlan(p);};allocations=0;counting=true;Check(call().ok(),"version6 allocation baseline");counting=false;const auto sites=allocations;
  for(unsigned long at=0;at<sites;++at){allocation_budget=at;const auto r=call();const auto left=allocation_budget;allocation_budget=-1;Check(left==-1&&r.error==E::resource_exhausted,"version6 consumed allocation fault");Failed(r);}
  hash_counting=true;hash_seen=0;Check(call().ok(),"version6 hash baseline");hash_counting=false;const auto hashes=hash_seen;for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto r=call();Check(!hash_fault&&r.error==E::hash_failure,"version6 complete hash failures");Failed(r);}
  std::cout<<"inventory plan route="<<route<<" allocations="<<sites<<" hashes="<<hashes<<'\n';
 }
}
void InventoryBundle(unsigned profile){
 // Version2 is checked separately from the complete mixed-profile fixtures.
 Fixture f(profile);Graph g(f);Bundle b(g,3);const auto encoded=b.Encode();Check(encoded.ok()&&*encoded.root==b.root&&encoded.pages==b.pages,"independent version2 bundle bytes include exact complete inventory");b.Good(b.Decode());Check(b.Decode(b.Budget()-1).error==BE::resource_exhausted,"version2 exact checked image allowance");
 BoundedBundleChecks(b.pages,b.root,Id(1),g.zero.page_uuid,b.Budget());
 auto forbidden=g.plan;forbidden.control_bundle=b.root;Failed(db::EncodeNativePublicationPlan(forbidden));
 InventoryPlan(g,b);
 const auto historical_zero=f.Read(0);
 b.Install();const auto original=f.Read(0,256);reads=writes=syncs=0;io_counting=true;const auto actual=b.Read();io_counting=false;b.Good(actual);Check(!writes&&!syncs&&f.Read(0,256)==original,"inventory bundle actual reading never publishes or changes bytes");
 BundleDeviceMemoryChecks(f.devices.front(),b.root,g.zero.page_uuid,b.Budget(),b.pages,db::NativeManagementControlReadContext::current,{},f.path,true);
 b.Good(db::ReadNativeManagementControlBundleAtHistoricalPageZeroFromOpenDevice(f.devices.front(),b.root,Id(1),g.zero.page_uuid,historical_zero,b.Budget()));
 b.Good(db::ReadNativeManagementControlBundleAtHistoricalResultFromOpenDevice(f.devices.front(),b.root,Id(1),g.zero.page_uuid,historical_zero,b.Budget()));
 Check(f.device.Close().ok()&&f.device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"version2 read-only reopen");b.Good(b.Read());Check(f.device.Close().ok(),"version2 close before cold reader");const auto child=fork();Check(child>=0,"version2 independent process fork");if(!child){d::FileDevice owned;if(!owned.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok())_exit(80);const d::NativeFilespaceDevice file{Id(2),g.zero.bootstrap.page_size_profile_uuid,&owned};const auto read=db::ReadNativeManagementControlBundleFromOpenDevice(file,b.root,Id(1),g.zero.page_uuid,b.Budget());_exit(read.ok()&&read.inventory_images==b.inventory&&read.allocation_images==g.after_bytes?0:81);}int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"cold reconstruction retains exact original inventory images");Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"version2 parent reopen");
 if(profile)return;
 const auto good_root=b.root;for(unsigned kind=0;kind<4;++kind){b.root=good_root;if(kind==0)b.root.inventory_count=0;if(kind==1)++b.root.inventory_count;if(kind==2)b.root.inventory_count=std::numeric_limits<u64>::max();if(kind==3)b.root.map_count=std::numeric_limits<u64>::max();Empty(b.Decode());}b.root=good_root;
 for(unsigned offset:{135u,136u,276u,283u,284u,287u,352u,383u}){const auto saved=b.pages;b.pages[1][offset]^=1;b.Reseal();Empty(b.Decode());b.pages=saved;b.Reseal();}
 for(unsigned kind=0;kind<10;++kind){Graph altered(f);Bundle wrong(altered,3);auto image=page::DecodeNativeTransactionInventoryPage(wrong.inventory[1]);Check(image.ok(),"inventory mutation fixture");auto p=*image.page;
  if(kind==0)p.previous.reset();if(kind==1)p.next.reset();if(kind==2)p.object_uuid=Id(911);if(kind==3)p.inventory_generation=3;if(kind==4)p.inventory.next_local_transaction_id=5;if(kind==5)p.header.page_uuid=Id(9900);if(kind==6)p.header.page_generation=3;if(kind==7)p.inventory.entries[0].identity.transaction_uuid.value=Id(9600);if(kind==8)p.header.filespace_uuid=Id(9);if(kind==9){p.inventory.entries[0].state=mga::TransactionState::committed;p.inventory.entries[0].commit_sequence=1;p.inventory.entries[0].begin_visible_through_commit_sequence=0;p.inventory.entries[0].final_unix_epoch_millis=2001;}
  const auto bad=page::EncodeNativeTransactionInventoryPage(p);Check(bad.ok(),"individually canonical wrong inventory image");wrong.inventory[1]=bad.bytes;wrong.Oracle();Empty(wrong.Encode());Empty(wrong.Decode());
 }
 for(unsigned kind=0;kind<5;++kind){Graph altered(f);Bundle wrong(altered,3);auto& record=Find(altered.after,221);if(kind==0)record.owner_uuid=Id(911);if(kind==1)record.creator_operation_uuid=Id(999);if(kind==2)record.page_generation=3;if(kind==3)record.page_uuid=Id(9900);if(kind==4)record.page_type=0x500;altered.after_bytes=EncodeMaps(altered.after);wrong.Oracle();Empty(wrong.Encode());Empty(wrong.Decode());}
 b.Install();byte scratch=0;for(unsigned i=0;i<4097;++i){const auto io=f.device.ReadAt(0,&scratch,1);Check(io.ok()&&io.bytes_transferred==1,"version2 metric capacity stabilized");}
 for(unsigned route=0;route<3;++route){const auto call=[&](){if(route==0){const auto r=b.Encode();if(!r.ok())Empty(r);return r.error;}const auto r=route==1?b.Decode():b.Read();if(!r.ok())Empty(r);return r.error;};
  allocations=0;counting=true;const auto base=call();counting=false;const auto sites=allocations;Check(base==BE::none&&sites,"version2 allocation baseline");
  for(unsigned long at=0;at<sites;++at){const auto loss=f.device.failed_io_latency_observations();allocation_budget=at;const auto error=call();const auto remaining=allocation_budget;allocation_budget=-1;Check(remaining==-1&&(error==BE::resource_exhausted||(route==2&&error==BE::none&&f.device.failed_io_latency_observations()==loss+1)),"every version2 allocation consumed and classified");}
  hash_seen=0;hash_counting=true;Check(call()==BE::none,"version2 hash baseline");hash_counting=false;const auto hashes=hash_seen;
  for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto error=call();const bool consumed=!hash_fault;hash_fault=0;hash_active=false;Check(consumed&&error==BE::hash_failure,"all version2 digest failures consumed and preserved");}
  std::cout<<"inventory bundle route="<<route<<" allocations="<<sites<<" hashes="<<hashes<<"\n";
 }
 reads=writes=syncs=0;io_counting=true;const auto measured=b.Read();io_counting=false;b.Good(measured);const auto sites=reads;Check(!writes&&!syncs,"version2 reader has no writes");
 for(unsigned mode=0;mode<2;++mode)for(unsigned at=1;at<=sites;++at){reads=0;if(mode)corrupt_read=at;else read_fault=at;io_counting=true;const auto r=b.Read();io_counting=false;read_fault=corrupt_read=0;Check(mode||r.error==BE::io_failure,"version2 exact I/O cause");Empty(r);}
 Check(f.Read(0,256)==original,"all version2 reader failures preserve full node");
}
void Test(unsigned profile){Fixture f(profile);Graph g(f);Bundle b(g);BoundedBundleChecks(b.pages,b.root,Id(1),g.zero.page_uuid,b.Budget());const auto encoded=b.Encode();Check(encoded.ok()&&*encoded.root==b.root&&encoded.pages==b.pages,"independent bundle bytes and root");b.Good(b.Decode());const auto short_budget=b.Decode(b.Budget()-1);Check(short_budget.error==BE::resource_exhausted,"exact bundle allowance");Empty(short_budget);
 const auto historical_zero=f.Read(0);
 b.Install();b.Good(b.Read());const auto untouched=f.Read(g.after.front().header.page_number);Check(std::all_of(untouched.begin(),untouched.end(),[](byte v){return !v;}),"bundle reconstructs target map not yet installed");
 BundleDeviceMemoryChecks(f.devices.front(),b.root,g.zero.page_uuid,b.Budget(),b.pages,db::NativeManagementControlReadContext::current,{},f.path,true);
 b.Good(db::ReadNativeManagementControlBundleAtHistoricalPageZeroFromOpenDevice(f.devices.front(),b.root,Id(1),g.zero.page_uuid,historical_zero,b.Budget()));
 b.Good(db::ReadNativeManagementControlBundleAtHistoricalResultFromOpenDevice(f.devices.front(),b.root,Id(1),g.zero.page_uuid,historical_zero,b.Budget()));
 Check(f.device.Close().ok()&&f.device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"read-only reopen");b.Good(b.Read());Check(f.device.Close().ok(),"close before process reader");const auto child=fork();Check(child>=0,"fork independent bundle reader");if(!child){d::FileDevice own;if(!own.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok())_exit(80);const d::NativeFilespaceDevice file{Id(2),g.zero.bootstrap.page_size_profile_uuid,&own};const auto read=db::ReadNativeManagementControlBundleFromOpenDevice(file,b.root,Id(1),g.zero.page_uuid,b.Budget());_exit(read.ok()&&read.allocation_images==g.after_bytes?0:81);}int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"independent process reconstruction");Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"parent reopen");
 if(profile){Integration(f,profile);return;}
 for(unsigned mode=0;mode<2;++mode){const auto call=[&](){return mode?b.Decode().error:b.Encode().error;};counting=true;allocations=0;Check(call()==BE::none,"codec allocation baseline");counting=false;const auto sites=allocations;for(unsigned long at=0;at<sites;++at){allocation_budget=at;const auto e=call();allocation_budget=-1;Check(e==BE::resource_exhausted,"every bundle codec allocation fault");}
  hash_counting=true;hash_seen=0;Check(call()==BE::none,"codec hash baseline");hash_counting=false;const auto hashes=hash_seen;for(unsigned fault=1;fault<=5;++fault)for(unsigned at=1;at<=hashes;++at){hash_fault=fault;hash_target=at;hash_seen=0;hash_active=false;const auto e=call();const bool consumed=!hash_fault;hash_fault=0;Check(consumed&&e==BE::hash_failure,"all codec hashes preserve provider failure");}
  std::cout<<"bundle codec mode="<<mode<<" allocations="<<sites<<" hashes="<<hashes<<"\n";
 }
 for(unsigned fault=0;fault<9;++fault){Graph bad_graph(f);Bundle bad(bad_graph);switch(fault){case 0:Find(bad_graph.after,180).owner_uuid=Id(901);break;case 1:Find(bad_graph.after,180).creator_operation_uuid=Id(333);break;case 2:Find(bad_graph.after,180).page_generation=3;break;case 3:Find(bad_graph.after,180).page_uuid=Id(999);break;case 4:for(auto& m:bad_graph.after)m.creator_operation_uuid=Id(333);break;case 5:for(auto& m:bad_graph.after)m.map_generation=3;break;case 6:bad_graph.after[1].object_uuid=Id(903);break;case 7:Find(bad_graph.after,180).allocation_uuid=Find(bad_graph.after,181).allocation_uuid;break;case 8:Find(bad_graph.after,140).creator_operation_uuid=Id(333);break;}
  bad_graph.after_bytes=EncodeMaps(bad_graph.after);bad.Oracle();Empty(bad.Encode());Empty(bad.Decode());}
 for(unsigned offset:{136u,138u,140u,144u,160u,176u,192u,200u,208u,216u,224u,232u,240u,272u,276u,352u}){auto saved=b.pages;b.pages[1][offset]^=1;b.Reseal();Empty(b.Decode());b.pages=std::move(saved);b.Reseal();}
 {const auto saved=b.pages;const auto original=b.root;b.root.aggregate_sha256[0]^=1;for(auto& p:b.pages)std::copy(b.root.aggregate_sha256.begin(),b.root.aggregate_sha256.end(),p.begin()+240);b.Reseal();Empty(b.Decode());b.pages=saved;b.root=original;}
 const auto good_root=b.root;for(unsigned fault=0;fault<6;++fault){b.root=good_root;switch(fault){case 0:b.root.map_count=std::numeric_limits<u64>::max()/f.size+1;break;case 1:b.root.page_count++;break;case 2:b.root.first.page_number=std::numeric_limits<u64>::max()/f.size;break;case 3:b.root.aggregate_sha256[0]^=1;break;case 4:b.root.first_page_sha256[0]^=1;break;case 5:b.root.object_uuid=Id(1);break;}Empty(b.Decode());}b.root=good_root;
 for(std::size_t n=0;n<b.pages.size();++n){auto saved=b.pages[n];b.pages[n].back()^=1;Empty(b.Decode());b.pages[n]=std::move(saved);}
 {Graph wrong_capacity(f);Bundle different(wrong_capacity);for(auto& m:wrong_capacity.after)++m.total_pages;wrong_capacity.after.back().states.push_back(State::free);wrong_capacity.after_bytes=EncodeMaps(wrong_capacity.after);different.Oracle();different.Good(different.Decode());different.Install();const auto read=different.Read();Check(read.error==BE::binding_mismatch,"actual capacity binds contained map images");Empty(read);}
 b.Install();reads=writes=syncs=0;hash_counting=true;hash_seen=0;io_counting=true;const auto measured=b.Read();hash_counting=false;io_counting=false;b.Good(measured);const auto read_sites=reads,hash_sites=hash_seen;Check(!writes&&!syncs,"reader never writes or syncs");
 for(unsigned at=1;at<=read_sites;++at){reads=0;read_fault=at;io_counting=true;const auto r=b.Read();io_counting=false;read_fault=0;Check(r.error==BE::io_failure,"every actual bundle read fault");Empty(r);}
 for(unsigned fault=1;fault<=5;++fault)for(unsigned at=1;at<=hash_sites;++at){hash_fault=fault;hash_target=at;hash_seen=0;hash_active=false;const auto r=b.Read();const bool consumed=!hash_fault;hash_fault=0;Check(consumed&&r.error==BE::hash_failure,"all reader hashes preserve provider failure");Empty(r);}
 counting=true;allocations=0;Check(b.Read().ok(),"reader allocation baseline");counting=false;const auto sites=allocations;for(unsigned long at=0;at<sites;++at){const auto loss=f.device.failed_io_latency_observations();allocation_budget=at;const auto r=b.Read();allocation_budget=-1;if(r.ok()){Check(f.device.failed_io_latency_observations()==loss+1,"only recorded observation loss may succeed");b.Good(r);}else{Check(r.error==BE::resource_exhausted,"every reader allocation fault");Empty(r);}}
 std::cout<<"bundle pages="<<b.pages.size()<<" maps="<<g.after.size()<<" reads="<<read_sites<<" hashes="<<hash_sites<<" allocations="<<sites<<"\n";
 Integration(f,profile);
}
}
int main(int argc,char** argv){
 try{
 std::cout<<std::unitbuf;
 if(argc==4&&std::string_view(argv[1])=="--directory-delta-shard"){
   const auto profile=std::stoi(argv[2]),primary=std::stoi(argv[3]);
   Check(profile>=2&&profile<=4&&primary>=0&&primary<5,"directory delta shard arguments");
   unsigned cases=0;
   // Exact partition of --directory-delta by primary page profile. Keep every
   // secondary profile, ordering, target and state, including all deep fault
   // sweeps inside DirectoryDelta. The legacy aggregate remains an oracle.
   for(unsigned secondary=0;secondary<5;++secondary)
    for(bool reverse:{false,true})for(bool target:{false,true})for(bool reserve:{false,true}){
     if(profile==2&&(target||reserve))continue;
     if(profile==3&&!reserve)continue;
     DirectoryDelta(primary,secondary,reverse,profile,target,reserve);++cases;
    }
   Check(cases==(profile==2?10u:profile==3?20u:40u),"complete exact directory delta partition");
   std::cout<<"PASS directory delta shard profile="<<profile<<" primary="<<primary<<" cases="<<cases
     <<" checks="<<checks<<" not_runtime_acceptance=true\n";return 0;
 }
 if(argc==3&&std::string_view(argv[1])=="--startup-binding"){
   const auto profile=std::stoi(argv[2]);Check(profile>=0&&profile<5,"startup binding profile");
   ReservationEffects(profile,true);
   for(bool read_only:{false,true})for(bool mixed:{false,true})OwnedInventoryPublication(profile,read_only,mixed,true);
   std::cout<<"PASS startup native binding checks="<<checks<<" not_runtime_recovery_acceptance=true\n";return 0;
 }
 if(argc==5&&std::string_view(argv[1])=="--startup-binding-faults"){
   const auto profile=std::stoi(argv[2]),phase=std::stoi(argv[3]),route=std::stoi(argv[4]);
   Check(profile>=0&&profile<5&&phase>=0&&phase<2&&(route==3||route==4),"startup publication fault profile");
   OwnedInventoryFaults(route,0,profile,false,false,true,phase!=0);return 0;
 }
 if(argc==5&&std::string_view(argv[1])=="--startup-binding-sweep"){
   const auto phase=std::stoi(argv[2]),route=std::stoi(argv[3]),shard=std::stoi(argv[4]);
   Check(phase>=0&&phase<2&&route>=0&&route<3&&shard>=0&&shard<(route==2?32:route==1?5:1),"startup complete backend sweep arguments");
   OwnedInventoryFaults(route,shard,0,false,false,true,phase!=0);return 0;
 }
 if(argc==3&&(std::string_view(argv[1])=="--owned-directory-preallocation"||std::string_view(argv[1])=="--owned-secondary-preallocation"||std::string_view(argv[1])=="--owned-secondary-subdivision")){
   const auto profile=std::stoi(argv[2]);Check(profile>=0&&profile<5,"owned preallocation directory profile");OwnedDirectoryPreallocation(profile,std::string_view(argv[1])!="--owned-directory-preallocation",std::string_view(argv[1])=="--owned-secondary-subdivision");return 0;
 }
 if(argc==5&&(std::string_view(argv[1])=="--owned-secondary-crashes"||std::string_view(argv[1])=="--owned-secondary-physical-faults")){
   const auto primary=std::stoi(argv[2]),secondary=std::stoi(argv[3]),reverse=std::stoi(argv[4]);
   Check(primary>=0&&primary<5&&secondary>=0&&secondary<5&&reverse>=0&&reverse<2,"secondary preallocation crash profiles");
   if(std::string_view(argv[1])=="--owned-secondary-crashes")OwnedPreallocationCrashes(primary,false,secondary,reverse);
   else OwnedPreallocationFaults(primary,false,secondary,reverse);return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--owned-secondary-sweeps"){
   const auto route=std::stoi(argv[2]),shard=std::stoi(argv[3]);Check(route>=0&&route<6&&shard>=0&&shard<(route==2?32:route==1?5:1),"secondary preallocation fault shard");
   OwnedPreallocationSweeps(route,shard,false,true);return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--owned-secondary-close"){
   const auto primary=std::stoi(argv[2]),secondary=std::stoi(argv[3]);Check(primary>=0&&primary<5&&secondary>=0&&secondary<5,"secondary allocation close profiles");
   OwnedSecondaryConcurrentClose(primary,secondary);return 0;
 }
 if(argc==4&&(std::string_view(argv[1])=="--directory-growth-selection"||std::string_view(argv[1])=="--directory-growth-selection-crashes")){
   const auto primary=std::stoi(argv[2]),secondary=std::stoi(argv[3]);Check(primary>=0&&primary<5&&secondary>=0&&secondary<5,"growth selector recovery profiles");
   DirectoryGrowthSelectionRecovery(primary,secondary,std::string_view(argv[1])=="--directory-growth-selection-crashes"?3:-1);return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--directory-growth-staging-crashes"){
   const auto primary=std::stoi(argv[2]),secondary=std::stoi(argv[3]);Check(primary>=0&&primary<5&&secondary>=0&&secondary<5,"growth staging crash profiles");
   DirectoryGrowthStagingCrashes(primary,secondary);return 0;
 }
 if(argc==4&&(std::string_view(argv[1])=="--owned-growth"||std::string_view(argv[1])=="--owned-growth-subdivision")){
   const auto primary=std::stoi(argv[2]),secondary=std::stoi(argv[3]);Check(primary>=0&&primary<5&&secondary>=0&&secondary<5,"owned growth profiles");
   OwnedGrowthPublication(primary,secondary,std::string_view(argv[1])=="--owned-growth-subdivision");return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--owned-append-admission"){
   const auto primary=std::stoi(argv[2]),secondary=std::stoi(argv[3]);Check(primary>=0&&primary<5&&secondary>=0&&secondary<5,"append admission profiles");
   OwnedAppendAdmission(primary,secondary);return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--owned-append-faults"){
   const auto route=std::stoi(argv[2]),shard=std::stoi(argv[3]);Check(route>=0&&route<3&&shard>=0&&shard<(route==2?128:route==1?20:1),"append fault route and shard");
   OwnedInventoryFaults(route,shard,0,false,true);return 0;
 }
 if(argc==4&&(std::string_view(argv[1])=="--owned-growth-admission"||std::string_view(argv[1])=="--owned-growth-close")){
   const auto primary=std::stoi(argv[2]),secondary=std::stoi(argv[3]);Check(primary>=0&&primary<5&&secondary>=0&&secondary<5,"owned growth admission profiles");
   if(std::string_view(argv[1])=="--owned-growth-admission")OwnedGrowthAdmission(primary,secondary);
   else{OwnedSecondaryConcurrentClose(primary,secondary,true,false);OwnedSecondaryConcurrentClose(primary,secondary,true,true);}return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--owned-growth-physical-recovery"){
   const auto primary=std::stoi(argv[2]),secondary=std::stoi(argv[3]);Check(primary>=0&&primary<5&&secondary>=0&&secondary<5,"owned growth physical recovery profiles");
   OwnedGrowthPhysicalRecovery(primary,secondary);return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--owned-growth-recovery-faults"){
   const auto route=std::stoi(argv[2]),shard=std::stoi(argv[3]);Check(route>=0&&route<3&&shard>=0&&shard<(route==2?32:route==1?20:1),"original growth recovery fault shard");
   OwnedGrowthRecoveryFaults(route,shard);return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--owned-growth-recovery-refusals"){
   const auto primary=std::stoi(argv[2]),secondary=std::stoi(argv[3]);Check(primary>=0&&primary<5&&secondary>=0&&secondary<5,"growth recovery refusal profiles");
   OwnedGrowthRecoveryRefusals(primary,secondary);return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--owned-growth-crashes"){
   const auto primary=std::stoi(argv[2]),secondary=std::stoi(argv[3]);Check(primary>=0&&primary<5&&secondary>=0&&secondary<5,"owned growth crash profiles");
   OwnedGrowthCrashes(primary,secondary);return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--directory-growth-selection-faults"){
   const auto route=std::stoi(argv[2]),shard=std::stoi(argv[3]);Check(route>=0&&route<3&&shard>=0&&shard<(route==2?32:route==1?20:1),"growth recovery fault shard");
   DirectoryGrowthSelectionRecovery(0,1,route,shard);return 0;
 }
 if(argc==3&&std::string_view(argv[1])=="--owned-directory-repacking"){
   const auto profile=std::stoi(argv[2]);Check(profile>=0&&profile<5,"owned directory repacking profile");OwnedDirectoryRepacking(profile);return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--owned-directory-faults"){
   const auto route=std::stoi(argv[2]),shard=std::stoi(argv[3]);Check(route>=0&&route<3&&shard>=0&&shard<(route==2?32:route==1?5:1),"owned directory fault shard");
   OwnedInventoryFaults(route,shard,0,true);return 0;
 }
 if(argc==3&&std::string_view(argv[1])=="--owned-directory-inventory"){
   const auto primary=std::stoi(argv[2]);Check(primary>=0&&primary<5,"owned directory primary profile");OwnedDirectoryInventory(primary);return 0;
 }
 if(argc==5&&(std::string_view(argv[1])=="--directory-publication-crashes"||std::string_view(argv[1])=="--directory-publication-torn-payload")){
   const auto profile=std::stoi(argv[2]),primary=std::stoi(argv[3]),secondary=std::stoi(argv[4]);Check((profile==2||profile==3)&&primary>=0&&primary<5&&secondary>=0&&secondary<5,"mixed publication crash profile");
   DirectoryPublicationCrashes(profile,primary,secondary,std::string_view(argv[1])=="--directory-publication-torn-payload");return 0;
 }
 if(argc==6&&std::string_view(argv[1])=="--directory-install-faults"){
   const auto profile=std::stoi(argv[2]),stage=std::stoi(argv[3]),route=std::stoi(argv[4]),shard=std::stoi(argv[5]);
   Check(profile>=2&&profile<=4&&stage>=0&&stage<(profile==4?2:3)&&route>=0&&route<3&&shard>=0&&shard<(route==2?16:route==1?(stage==2?20:5):1),"directory installation fault arguments");
   DirectoryInstallFaults(profile,stage,route,shard);return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--directory-install"){
   const auto profile=std::stoi(argv[2]),primary=std::stoi(argv[3]);Check(profile>=2&&profile<=4&&primary>=0&&primary<5,"directory installation profile");
   if(profile==2)DirectoryInstallationRefusals(primary);
   for(unsigned secondary=0;secondary<5;++secondary)for(bool reverse:{false,true})for(bool target:{false,true})for(bool resume:{false,true}){
     if(profile==2&&target)continue;DirectoryInstallation(primary,secondary,reverse,profile,target,resume);
     if(profile==4)DirectoryInstallation(primary,secondary,reverse,profile,target,resume,false);}
   return 0;
 }
 if(argc==3&&std::string_view(argv[1])=="--directory-control-close"){
   const auto profile=std::stoi(argv[2]);Check(profile>=0&&profile<5,"control graph Close profile");DirectoryControlClose(profile);return 0;
 }

 if(argc==5&&std::string_view(argv[1])=="--guarded-allocation"){
  const auto scenario=std::stoi(argv[2]),primary=std::stoi(argv[3]),secondary=std::stoi(argv[4]);
  Check(scenario>=0&&scenario<4&&primary>=0&&primary<5&&secondary>=0&&secondary<5,"guarded allocation profiles");
  DirectoryAllocationLease(scenario,primary,secondary);std::cout<<"PASS guarded allocation checks="<<checks<<'\n';return 0;
 }
 if(argc==3&&std::string_view(argv[1])=="--guarded-visibility-observation"){
  const auto profile=std::stoi(argv[2]);Check(profile>=0&&profile<5,"guarded visibility profile");
  DirectoryGuardedVisibilityObservation(profile);std::cout<<"PASS guarded visibility observation checks="<<checks<<'\n';return 0;
 }
 if(argc==3&&std::string_view(argv[1])=="--guarded-snapshot-observation"){
  const auto profile=std::stoi(argv[2]);Check(profile>=0&&profile<5,"guarded snapshot profile");
  DirectoryGuardedSnapshotObservation(profile);std::cout<<"PASS guarded snapshot observation checks="<<checks<<'\n';return 0;
 }
 if((argc==5||argc==6)&&(std::string_view(argv[1])=="--guarded-catalog-relation"||std::string_view(argv[1])=="--guarded-catalog-visibility")){
  const auto indexed=std::stoi(argv[2]),primary=std::stoi(argv[3]),secondary=std::stoi(argv[4]);
  const auto fault=argc==6?std::stoi(argv[5]):0;
  Check(indexed>=0&&indexed<=1&&primary>=0&&primary<5&&secondary>=0&&secondary<5&&fault>=0&&fault<=24&&
    (fault<=9||std::string_view(argv[1])=="--guarded-catalog-visibility")&&
    (!fault||(fault==9?!indexed:bool(indexed))),"guarded catalog relation arguments");
  DirectoryCatalogRelationMemory(primary,secondary,indexed,fault,std::string_view(argv[1])=="--guarded-catalog-visibility");std::cout<<"PASS guarded catalog relation checks="<<checks<<'\n';return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--guarded-btree-memory"){
  const auto primary=std::stoi(argv[2]),secondary=std::stoi(argv[3]);
  Check(primary>=0&&primary<5&&secondary>=0&&secondary<5,"guarded tree profiles");
  DirectoryGuardedBtree(primary,secondary);std::cout<<"PASS guarded tree checks="<<checks<<'\n';return 0;
 }
 if((argc==5||argc==6)&&std::string_view(argv[1])=="--catalog-distinct-roots-memory"){
  const auto route=std::stoi(argv[2]),primary=std::stoi(argv[3]),secondary=std::stoi(argv[4]);
  const auto fault=argc==6?std::stoi(argv[5]):0;
  Check(route>=0&&route<=1&&primary>=0&&primary<5&&secondary>=0&&secondary<5&&fault>=0&&fault<=4,"distinct catalog root arguments");
  DirectoryDistinctCatalogRoots(primary,secondary,route,fault);
  std::cout<<"PASS distinct guarded catalog roots checks="<<checks<<'\n';return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--catalog-populated-owned-source-memory"){
  const auto via_route=std::stoi(argv[2]),profile=std::stoi(argv[3]);
  Check(via_route>=0&&via_route<=1&&profile>=0&&profile<5,"populated guarded catalog arguments");
  for(bool reverse:{false,true})
   DirectoryOwnedSourceMemory(profile,(profile+1)%5,reverse,2,false,false,true,via_route,9,0,1,true);
  std::cout<<"PASS populated guarded catalog checks="<<checks<<'\n';return 0;
 }
 if(argc==7&&std::string_view(argv[1])=="--directory-owned-source-memory"){
  const auto via_route=std::stoi(argv[2]),initial=std::stoi(argv[3]),profile=std::stoi(argv[4]),
    primary=std::stoi(argv[5]),secondary=std::stoi(argv[6]);
  Check(via_route>=0&&via_route<=1&&initial>=0&&initial<=1&&profile>=2&&profile<=4&&
    primary>=0&&primary<5&&secondary>=0&&secondary<5,"owned source pair arguments");
  for(bool reverse:{false,true})for(bool target:{false,true})for(bool reserve:{false,true}){
   if(profile==2&&(target||reserve))continue;if(profile==3&&!reserve)continue;
   DirectoryOwnedSourceMemory(primary,secondary,reverse,profile,target,reserve,initial,via_route);
  }
  std::cout<<"PASS owned source pair checks="<<checks<<'\n';return 0;
 }
 if(argc==7&&std::string_view(argv[1])=="--directory-owned-source-faults"){
  const auto via_route=std::stoi(argv[2]),initial=std::stoi(argv[3]),profile=std::stoi(argv[4]),
    route=std::stoi(argv[5]),shard=std::stoi(argv[6]);
  const unsigned shards=route<=6?16:1;
  Check(via_route>=0&&via_route<=1&&initial>=0&&initial<=1&&profile>=2&&profile<=4&&
    route>=0&&route<=8&&shard>=0&&unsigned(shard)<shards,"owned source fault arguments");
  DirectoryOwnedSourceMemory(0,0,false,profile,false,profile!=2,initial,via_route,route,shard,shards);
  std::cout<<"PASS owned source fault checks="<<checks<<'\n';return 0;
 }
 if(argc==6&&std::string_view(argv[1])=="--directory-selected-lease-memory"){
  const auto initial=std::stoi(argv[2]),profile=std::stoi(argv[3]),primary=std::stoi(argv[4]),secondary=std::stoi(argv[5]);
  Check(initial>=0&&initial<=1&&profile>=2&&profile<=4&&primary>=0&&primary<5&&secondary>=0&&secondary<5,"selected lease pair arguments");
  for(bool reverse:{false,true})for(bool target:{false,true})for(bool reserve:{false,true}){
   if(profile==2&&(target||reserve))continue;if(profile==3&&!reserve)continue;
   DirectorySelectedLeaseMemory(primary,secondary,reverse,profile,target,reserve,initial);
  }
  std::cout<<"PASS selected lease pair checks="<<checks<<'\n';return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--directory-selected-lease-repeat"){
  const auto profile=std::stoi(argv[2]),size=std::stoi(argv[3]);
  Check((profile==3||profile==4)&&size>=0&&size<5,"selected lease repeated size arguments");
  for(bool target:{false,true})DirectorySelectedLeaseMemory(size,size,false,profile,target,true,false,true);
  std::cout<<"PASS repeated selected lease checks="<<checks<<'\n';return 0;
 }
 if(argc==6&&std::string_view(argv[1])=="--directory-selected-lease-faults"){
  const auto initial=std::stoi(argv[2]),profile=std::stoi(argv[3]),route=std::stoi(argv[4]),shard=std::stoi(argv[5]);
  const unsigned shards=route<=5?16:1;
  Check(initial>=0&&initial<=1&&profile>=2&&profile<=4&&route>=0&&route<=7&&shard>=0&&unsigned(shard)<shards,"selected lease fault arguments");
  DirectorySelectedLeaseMemory(0,0,false,profile,false,profile!=2,initial,false,route,shard,shards);
  std::cout<<"PASS selected lease fault checks="<<checks<<'\n';return 0;
 }
 if(argc==6&&std::string_view(argv[1])=="--directory-current-source-memory"){
  const auto allocation=std::stoi(argv[2]),profile=std::stoi(argv[3]),primary=std::stoi(argv[4]),secondary=std::stoi(argv[5]);
  Check(allocation>=0&&allocation<=1&&profile>=2&&profile<=4&&primary>=0&&primary<5&&secondary>=0&&secondary<5,"current source pair arguments");
  for(bool reverse:{false,true})for(bool target:{false,true})for(bool reserve:{false,true}){
   if(profile==2&&(target||reserve))continue;if(profile==3&&!reserve)continue;
   if(allocation)DirectoryCurrentSourceMemory<true>(primary,secondary,reverse,profile,target,reserve);
   else DirectoryCurrentSourceMemory<false>(primary,secondary,reverse,profile,target,reserve);
  }
  std::cout<<"PASS current source pair checks="<<checks<<'\n';return 0;
 }
 if(argc==5&&std::string_view(argv[1])=="--directory-current-source-repeat-memory"){
  const auto allocation=std::stoi(argv[2]),profile=std::stoi(argv[3]),size=std::stoi(argv[4]);
  Check(allocation>=0&&allocation<=1&&(profile==3||profile==4)&&size>=0&&size<5,"current source repeated size arguments");
  for(bool target:{false,true}){
   if(allocation)DirectoryCurrentSourceMemory<true>(size,size,false,profile,target,true,true);
   else DirectoryCurrentSourceMemory<false>(size,size,false,profile,target,true,true);
  }
  std::cout<<"PASS repeated current source checks="<<checks<<'\n';return 0;
 }
 if(argc==6&&std::string_view(argv[1])=="--directory-current-source-faults"){
  const auto allocation=std::stoi(argv[2]),profile=std::stoi(argv[3]),route=std::stoi(argv[4]),shard=std::stoi(argv[5]);const unsigned shards=route<=5?4:1;
  Check(allocation>=0&&allocation<=1&&profile>=2&&profile<=4&&route>=0&&route<=7&&shard>=0&&unsigned(shard)<shards,"current source fault route and shard");
  if(allocation)DirectoryCurrentSourceMemory<true>(0,0,false,profile,false,profile!=2,false,route,shard,shards);
  else DirectoryCurrentSourceMemory<false>(0,0,false,profile,false,profile!=2,false,route,shard,shards);
  std::cout<<"PASS current source fault route="<<route<<" shard="<<shard<<" checks="<<checks<<'\n';return 0;
 }
 if(argc==6&&std::string_view(argv[1])=="--directory-selection-faults"){
   const auto profile=std::stoi(argv[2]),repeated=std::stoi(argv[3]),route=std::stoi(argv[4]),shard=std::stoi(argv[5]);
   const unsigned shards=route<=5?(repeated?16:4):1;
   Check(profile>=2&&profile<=4&&repeated>=0&&repeated<=1&&(!repeated||profile>2)&&route>=0&&route<=7&&shard>=0&&unsigned(shard)<shards,"selector fault route and shard arguments");
   DirectorySelectionMemory(0,0,false,profile,false,profile!=2,repeated,route,shard,shards);
   std::cout<<"PASS directory selector fault route="<<route<<" shard="<<shard<<" checks="<<checks<<'\n';return 0;
 }
 if(argc==5&&std::string_view(argv[1])=="--directory-selection-memory"){
   const auto profile=std::stoi(argv[2]),primary=std::stoi(argv[3]),secondary=std::stoi(argv[4]);
   Check(profile>=2&&profile<=4&&primary>=0&&primary<5&&secondary>=0&&secondary<5,"memory selector pair arguments");
   for(bool reverse:{false,true})for(bool target:{false,true})for(bool reserve:{false,true}){
     if(profile==2&&(target||reserve))continue;if(profile==3&&!reserve)continue;
     DirectorySelectionMemory(primary,secondary,reverse,profile,target,reserve);
   }
   std::cout<<"PASS directory selector memory pair checks="<<checks<<'\n';return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--directory-selection-repeat-memory"){
   const auto profile=std::stoi(argv[2]),size=std::stoi(argv[3]);
   Check((profile==3||profile==4)&&size>=0&&size<5,"repeated memory selector size arguments");
   for(bool target:{false,true})DirectorySelectionMemory(size,size,false,profile,target,true,true);
   std::cout<<"PASS repeated directory selector memory checks="<<checks<<'\n';return 0;
 }
 if(argc==5&&std::string_view(argv[1])=="--directory-history-memory"){
   const auto profile=std::stoi(argv[2]),primary=std::stoi(argv[3]),secondary=std::stoi(argv[4]);
   Check(profile>=2&&profile<=4&&primary>=0&&primary<5&&secondary>=0&&secondary<5,"memory history pair arguments");
   control_authority_memory_tests=true;
   for(bool reverse:{false,true})for(bool target:{false,true})for(bool reserve:{false,true}){
     if(profile==2&&(target||reserve))continue;if(profile==3&&!reserve)continue;
     DirectoryHistory(primary,secondary,reverse,profile,target,reserve);
   }
   std::cout<<"PASS directory control memory pair checks="<<checks<<'\n';return 0;
 }
 if(argc==4&&std::string_view(argv[1])=="--directory-history-repeat-memory"){
   const auto profile=std::stoi(argv[2]),size=std::stoi(argv[3]);
   Check((profile==3||profile==4)&&size>=0&&size<5,"repeated memory history size arguments");
   control_authority_memory_tests=true;RepeatedDirectoryHistory(profile,size);
   std::cout<<"PASS repeated directory control memory checks="<<checks<<'\n';return 0;
 }
 if(argc==5&&std::string_view(argv[1])=="--directory-control-faults"){
   const auto profile=std::stoi(argv[2]),route=std::stoi(argv[3]),shard=std::stoi(argv[4]);
   Check(profile>=2&&profile<=4&&route>=0&&route<=2&&shard>=0&&shard<(route==2?8:route==1?5:1),"control graph fault arguments");
   DirectoryControlFaults(profile,route,shard);return 0;
 }
 if(argc==3&&std::string_view(argv[1])=="--directory-history-repeat"){const auto profile=std::stoi(argv[2]);Check(profile==3||profile==4,"repeat history profile");RepeatedDirectoryHistory(profile);return 0;}if(argc==3&&std::string_view(argv[1])=="--directory-history-faults"){const auto profile=std::stoi(argv[2]);Check(profile>=2&&profile<=4,"history fault profile");DirectoryHistoryFaults(profile);return 0;}if(argc==4&&std::string_view(argv[1])=="--directory-history"){const auto profile=std::stoi(argv[2]),p=std::stoi(argv[3]);Check(profile>=2&&profile<=4&&p>=0&&p<5,"directory history profile");for(unsigned q=0;q<5;++q)for(bool reverse:{false,true})for(bool target:{false,true})for(bool reserve:{false,true}){if(profile==2&&(target||reserve))continue;if(profile==3&&!reserve)continue;DirectoryHistory(p,q,reverse,profile,target,reserve);}std::cout<<"PASS directory history checks="<<checks<<" not_runtime_acceptance=true\n";return 0;}if(argc==3&&std::string_view(argv[1])=="--directory-delta-faults"){const auto profile=std::stoi(argv[2]);Check(profile>=2&&profile<=4,"directory fault profile");DirectoryDeltaFaults(profile);return 0;}if(argc==3&&std::string_view(argv[1])=="--directory-delta"){const auto profile=std::stoi(argv[2]);Check(profile>=2&&profile<=4,"directory delta profile");for(unsigned p=0;p<5;++p)for(unsigned q=0;q<5;++q)for(bool reverse:{false,true})for(bool target:{false,true})for(bool reserve:{false,true}){if(profile==2&&(target||reserve))continue;if(profile==3&&!reserve)continue;DirectoryDelta(p,q,reverse,profile,target,reserve);}std::cout<<"PASS directory delta checks="<<checks<<" not_runtime_acceptance=true\n";return 0;}if(argc==2&&std::string_view(argv[1])=="--native-preallocation-repeat"){for(unsigned p=0;p<5;++p)OwnedPreallocationRepeat(p);}else if(argc==5&&std::string_view(argv[1])=="--native-preallocation-reconcile-faults"){const auto stage=std::stoi(argv[2]),route=std::stoi(argv[3]),shard=std::stoi(argv[4]);Check(stage>=0&&stage<3&&route>=0&&route<3&&shard>=0&&shard<(route==0?1:route==1?5:8),"reconciliation fault arguments");OwnedPreallocationReconciliationFaults(stage,route,shard);}else if(argc==3&&std::string_view(argv[1])=="--native-preallocation-crashes"){const auto profile=std::stoi(argv[2]);Check(profile>=0&&profile<5,"preallocation crash profile");OwnedPreallocationCrashes(profile);}else if(argc==4&&std::string_view(argv[1])=="--native-preallocation-sweep"){const auto route=std::stoi(argv[2]),shard=std::stoi(argv[3]);Check(route>=0&&route<=2&&shard>=0&&shard<(route==2?8:1),"preallocation sweep arguments");OwnedPreallocationSweeps(route,shard);}else if(argc==2&&std::string_view(argv[1])=="--native-preallocation-faults"){for(unsigned p=0;p<5;++p)OwnedPreallocationFaults(p);}else if(argc==2&&std::string_view(argv[1])=="--native-preallocation"){for(unsigned p=0;p<5;++p)OwnedPreallocationPublication(p);}else if(argc==2&&std::string_view(argv[1])=="--publication-effects"){for(unsigned p=0;p<5;++p){ReservationEffects(p);OwnedInventoryFaults(4,0,p);}}else if(argc==2&&std::string_view(argv[1])=="--owned-inventory-placement"){OwnedInventoryPlacement(1);OwnedInventoryPlacement(2);OwnedInventoryBudgetAndStale();}else if(argc==4&&std::string_view(argv[1])=="--owned-inventory-faults"){const auto route=std::stoi(argv[2]),shard=std::stoi(argv[3]);Check(route>=0&&route<4&&shard>=0&&shard<(route==2?32:route==1?5:1),"owned fault arguments");OwnedInventoryFaults(route,shard);}else if(argc==2&&(std::string_view(argv[1])=="--owned-inventory-publication"||std::string_view(argv[1])=="--owned-inventory-mixed")){for(unsigned p=0;p<5;++p)for(bool ro:{false,true})OwnedInventoryPublication(p,ro,std::string_view(argv[1])=="--owned-inventory-mixed");}else if(argc==2&&std::string_view(argv[1])=="--directory-bundle"){for(unsigned p=0;p<5;++p)for(unsigned q=0;q<5;++q)for(bool reverse:{false,true})MixedDirectoryBundle(p,q,reverse);}else if(argc==5&&std::string_view(argv[1])=="--inventory-consumer-faults"){inventory_publication_mode=inventory_staging_admission=true;inventory_consumer=std::stoi(argv[2]);inventory_consumer_fault=std::stoi(argv[3]);inventory_consumer_shard=std::stoi(argv[4]);Check(inventory_consumer>=0&&inventory_consumer<2&&inventory_consumer_fault>=0&&inventory_consumer_fault<3&&inventory_consumer_shard>=0&&inventory_consumer_shard<(inventory_consumer_fault==2?128:inventory_consumer_fault==1?40:8),"consumer fault arguments");InventoryAllocation(0);}else if(argc==3&&std::string_view(argv[1])=="--inventory-allocation-reads"){inventory_publication_mode=inventory_staging_admission=true;inventory_allocation_read_shard=std::stoi(argv[2]);Check(inventory_allocation_read_shard>=0&&inventory_allocation_read_shard<8,"allocation reader fault shard");InventoryAllocation(0);}else if(argc==2&&std::string_view(argv[1])=="--inventory-staging-admission"){inventory_publication_mode=inventory_staging_admission=true;for(unsigned profile=0;profile<5;++profile)InventoryAllocation(profile);}else if(argc==2&&std::string_view(argv[1])=="--inventory-publication-mixed"){inventory_publication_mode=inventory_mixed_mode=true;for(unsigned profile=0;profile<5;++profile)InventoryAllocation(profile);}else if(argc==5&&std::string_view(argv[1])=="--inventory-resolution-faults"){inventory_publication_mode=inventory_resolution_mode=true;inventory_resolution_stage=std::stoi(argv[2]);inventory_resolution_route=std::stoi(argv[3]);inventory_resolution_shard=std::stoi(argv[4]);Check(inventory_resolution_stage>=0&&inventory_resolution_stage<2&&inventory_resolution_route>=0&&inventory_resolution_route<5&&inventory_resolution_shard>=0&&inventory_resolution_shard<(inventory_resolution_route==2?(inventory_resolution_stage?32:8):inventory_resolution_route==1?5:1),"inventory resolution fault arguments");InventoryAllocation(0);}else if(argc==2&&(std::string_view(argv[1])=="--inventory-resolution"||std::string_view(argv[1])=="--inventory-resolution-requests")){inventory_resolution_requests=std::string_view(argv[1])=="--inventory-resolution-requests";inventory_publication_mode=inventory_resolution_mode=true;for(unsigned profile=0;profile<5;++profile)InventoryAllocation(profile);}else if(argc==5&&(std::string_view(argv[1])=="--inventory-install-faults"||std::string_view(argv[1])=="--inventory-reconstruction-faults")){inventory_publication_mode=true;inventory_reconstruction_faults=std::string_view(argv[1])=="--inventory-reconstruction-faults";inventory_install_stage=std::stoi(argv[2]);inventory_install_route=std::stoi(argv[3]);inventory_install_shard=std::stoi(argv[4]);Check(inventory_install_stage>=0&&inventory_install_stage<2&&inventory_install_route>=0&&inventory_install_route<(inventory_reconstruction_faults?3:4)&&inventory_install_shard>=0&&inventory_install_shard<(inventory_install_route==2?(inventory_reconstruction_faults?(inventory_install_stage?64:16):(inventory_install_stage?32:8)):inventory_install_route==1?5:1),"inventory installer fault arguments");InventoryAllocation(0);}else if(argc==2&&(std::string_view(argv[1])=="--inventory-publication"||std::string_view(argv[1])=="--inventory-publication-cold"||std::string_view(argv[1])=="--inventory-publication-read-only")){inventory_publication_mode=true;inventory_publication_cold=std::string_view(argv[1])=="--inventory-publication-cold";inventory_read_only=std::string_view(argv[1])=="--inventory-publication-read-only";for(unsigned profile=0;profile<5;++profile)InventoryAllocation(profile);}else if(argc==4&&std::string_view(argv[1])=="--inventory-reader-allocations"){inventory_allocation_route=std::stoi(argv[2]);inventory_allocation_shard=std::stoi(argv[3]);Check(inventory_allocation_route>=0&&inventory_allocation_route<2&&inventory_allocation_shard>=0&&inventory_allocation_shard<4,"inventory reader allocation shard arguments");InventoryAllocation(0);}else{Check(argc==1,"test arguments");for(unsigned profile=0;profile<5;++profile){InventoryAllocation(profile);InventoryBundle(profile);Test(profile);}}std::cout<<"PASS management control bundle checks="<<checks<<" not_SQL_E2E=true\n";return 0;}catch(const std::exception& e){allocation_budget=-1;hash_fault=0;io_counting=false;std::cerr<<"FAIL management control bundle checks="<<checks<<" "<<e.what()<<'\n';return 1;}}

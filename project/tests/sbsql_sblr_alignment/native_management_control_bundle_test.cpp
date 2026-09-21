// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_control_bundle.hpp"
#include "physical_mga_cow_store.hpp"
#include "native_management_control_allocation.hpp"
#include "native_management_history.hpp"
#include "native_management_control_authority.hpp"
#include "native_management_publication_recovery.hpp"
#include "native_creation_workspace.hpp"
#include "disk_device.hpp"
#include <filesystem>
#include <unistd.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <future>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <source_location>
#include <set>
#include <stdexcept>
#include <cerrno>
#include <sys/wait.h>
namespace {long allocation_budget=-1;bool counting=false;unsigned long allocations=0;unsigned hash_fault=0,hash_target=1,hash_seen=0;bool hash_active=false,hash_counting=false;
unsigned entropy_fault=0,entropy_calls=0;
bool repeated_entropy=false;
std::atomic<bool> owned_clock_controlled{false};
std::atomic<std::uint64_t> owned_clock_millis{1700000000123ULL},owned_clock_ticks{1};
off_t corrupt_offset=0;bool corrupt_was_zero=false;
bool io_counting=false;unsigned reads=0,writes=0,syncs=0,read_fault=0,write_fault=0,sync_fault=0,kill_write=0,kill_sync=0,corrupt_read=0;bool kill_after_sync=false;std::size_t torn_bytes=0;int allocation_shard=-1;
int inventory_allocation_route=-1,inventory_allocation_shard=-1,inventory_install_stage=-1,inventory_install_route=-1,inventory_install_shard=-1;bool inventory_read_only=false;bool inventory_publication_mode=false,inventory_publication_cold=false;const unsigned char* replacement_bytes=nullptr;std::size_t replacement_length=0;off_t replacement_offset=0;unsigned replacement_at=0,replacement_seen=0;
int inventory_resolution_stage=-1,inventory_resolution_route=-1,inventory_resolution_shard=-1;bool inventory_mixed_mode=false,inventory_resolution_mode=false,inventory_resolution_requests=false,inventory_reconstruction_faults=false,trace_io=false;std::array<off_t,256> io_trace{};unsigned io_trace_count=0;const unsigned char* race_bytes=nullptr;std::size_t race_length=0;off_t race_offset=0;unsigned race_seen=0;
void Trace(off_t value){if(trace_io){if(io_trace_count==io_trace.size())std::abort();io_trace[io_trace_count++]=value;}}}
void* operator new(std::size_t n){if(counting)++allocations;if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
extern "C" int __real_RAND_bytes(unsigned char*,int);
extern "C" int __wrap_RAND_bytes(unsigned char* out,int count){++entropy_calls;if(entropy_fault&&!--entropy_fault)return 0;if(repeated_entropy){std::fill_n(out,count,0);if(count)out[count-1]=1;return 1;}return __real_RAND_bytes(out,count);}
extern "C" scratchbird::core::time::ClockSnapshotResult __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
extern "C" scratchbird::core::time::ClockSnapshotResult __wrap__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv(){
 if(!owned_clock_controlled)return __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
 scratchbird::core::time::ClockSnapshotResult result;
 result.value={{owned_clock_ticks++},{static_cast<std::int64_t>(owned_clock_millis/1000),static_cast<std::uint32_t>((owned_clock_millis%1000)*1000000)}};
 return result;
}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
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
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* data,size_t n,off_t at){
 if(race_bytes&&at==race_offset&&n==race_length&&++race_seen==2&&__real_pwrite(fd,race_bytes,race_length,at)!=static_cast<ssize_t>(race_length)){errno=EIO;return -1;}
 if(io_counting&&++reads==read_fault){errno=EIO;return -1;}const auto result=__real_pread(fd,data,n,at);
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
 const bool v2=p.management_extent.has_value(),v3=p.control_bundle.has_value(),v4=p.base_selection_generation.has_value(),v5=p.intent.recovery_profile!=0,v6=p.intent.recovery_profile==2;std::copy_n(v6?"SBPPM006":v5?"SBPPM005":v4?"SBPPM004":v3?"SBPPM003":v2?"SBPPM002":"SBPPM001",8,b.begin()+128);Num(b,136,2,v6?6:v5?5:v4?4:v3?3:v2?2:1);Num(b,138,2,v3?1024:v2?896:640);Num(b,140,4,v3?1152:v2?1024:768);
 Put(b,144,p.object_uuid);Put(b,160,p.bootstrap_uuid);Put(b,176,p.timeline_uuid);Put(b,192,p.operation_uuid);Put(b,208,p.intent.initiator_uuid);Put(b,224,p.intent.request_context_uuid);Put(b,240,p.intent.policy_snapshot_uuid);Put(b,256,p.security_snapshot_uuid);
 std::copy(p.intent.normalized_request_sha256.begin(),p.intent.normalized_request_sha256.end(),b.begin()+272);std::copy(p.reservation_state_sha256.begin(),p.reservation_state_sha256.end(),b.begin()+304);
 Num(b,336,8,p.reserved_generation);Num(b,344,8,p.base_checkpoint_generation);Num(b,352,8,p.base_root_set_generation);Num(b,360,8,p.target_root_set_generation);
 Ref(b,368,p.base_checkpoint);Put(b,416,p.base_checkpoint_object_uuid);std::copy(p.base_checkpoint_sha256.begin(),p.base_checkpoint_sha256.end(),b.begin()+432);Ref(b,464,p.target_checkpoint);Put(b,512,p.target_checkpoint_object_uuid);std::copy(p.target_graph_sha256.begin(),p.target_graph_sha256.end(),b.begin()+528);
 if(p.previous_plan)Ref(b,560,*p.previous_plan);Put(b,608,p.previous_plan_object_uuid);std::copy(p.previous_plan_sha256.begin(),p.previous_plan_sha256.end(),b.begin()+624);
 Num(b,656,8,p.catalog_generation);Num(b,664,8,p.configuration_generation);Num(b,672,8,p.security_generation);Num(b,680,2,p.intent.initiator_kind);Num(b,682,2,1);Num(b,684,2,2);
 if(p.management_extent){const auto& r=*p.management_extent;Num(b,720,4,p.generation_guard_flags);Ref(b,736,r.first);Put(b,784,r.object_uuid);Put(b,800,r.operation_uuid);Num(b,816,8,r.revision);Num(b,824,4,r.aggregate_bytes);Num(b,828,4,r.page_count);std::copy(r.aggregate_sha256.begin(),r.aggregate_sha256.end(),b.begin()+832);std::copy(r.first_page_sha256.begin(),r.first_page_sha256.end(),b.begin()+864);}
 if(p.control_bundle){const auto& r=*p.control_bundle;Ref(b,896,r.first);Put(b,944,r.object_uuid);Num(b,960,8,r.map_count);Num(b,968,8,r.page_count);std::copy(r.aggregate_sha256.begin(),r.aggregate_sha256.end(),b.begin()+976);std::copy(r.first_page_sha256.begin(),r.first_page_sha256.end(),b.begin()+1008);}
 if(v4)Num(b,1040,8,*p.base_selection_generation);if(v5)Num(b,1048,2,p.intent.recovery_profile);if(v6&&p.control_bundle)Num(b,1056,8,p.control_bundle->inventory_count);PlanSeal(b);return b;
}
void Failed(const db::NativePublicationPlanImage& r){Check(!r.ok()&&!r.plan&&r.bytes.empty()&&std::all_of(r.sha256.begin(),r.sha256.end(),[](byte v){return !v;}),"no failed plan prefix");}
void Bad(const db::NativePublicationPlan& p){Failed(db::EncodeNativePublicationPlan(p));const auto raw=Oracle(p);Failed(db::DecodeNativePublicationPlan(raw));}
struct Fixture {
 std::filesystem::path path;d::FileDevice device,secondary;Bytes secondary_before;std::vector<d::NativeFilespaceDevice> devices;u64 size,budget;
 std::unique_ptr<scratchbird::core::uuid::StandaloneUuidV7Issuer> issuer;
 void ResetIssuer(){issuer=std::make_unique<scratchbird::core::uuid::StandaloneUuidV7Issuer>(
   scratchbird::core::uuid::StandaloneUuidV7Binding{Id(1),Id(10)},scratchbird::core::uuid::StandaloneUuidV7Policy{{},0,1000});}
 Fixture(unsigned profile){
  const auto* temporary=std::getenv("TMPDIR");
  auto name=((temporary&&*temporary?std::filesystem::path(temporary):std::filesystem::temp_directory_path())/"sb-management-bundle.XXXXXX").string();
  Check(mkdtemp(name.data()),"isolated fixture directory");path=std::filesystem::path(name)/"node";
  const auto& page=d::kCanonicalFilespacePageProfiles[profile];size=page.page_size_bytes;budget=1024*size;
  db::NativeFilespaceInitializationRequest r;r.bootstrap={Id(1),Id(2),page.uuid,d::kNativeBootstrapIntegrityProfile,{},page.page_size_bytes,1,0,1,7};r.operation_uuid=Id(3);r.writer_uuid=Id(4);
  r.creator.transaction_uuid={UuidKind::transaction,Id(5)};r.creator.local_id=mga::MakeLocalTransactionId(1);r.creator.scope=mga::TransactionScope::local_node;r.creation_utc_millis=1789357072000ULL;r.total_pages=256;
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
namespace page=scratchbird::storage::page;
using State=page::NativeAllocationState;
using Maps=std::vector<page::NativeAllocationMap>;
using Pages=std::vector<Bytes>;
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
 CE CheckGraph(u64 budget=0)const{return db::ValidateNativeManagementControlAllocation(base_bytes,target_bytes,plan_bytes,extent,before_bytes,after_bytes,budget?budget:Budget());}
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
 auto Decode(u64 budget=0)const{return db::DecodeNativeManagementControlBundle(pages,root,Id(1),graph.zero.page_uuid,budget?budget:Budget());}
 auto Read()const{return db::ReadNativeManagementControlBundleFromOpenDevice(graph.fixture.devices.front(),root,Id(1),graph.zero.page_uuid,Budget());}
 void Install(){for(std::size_t n=0;n<pages.size();++n){const auto& b=pages[n];const auto io=graph.fixture.device.WriteAt((root.first.page_number+n)*graph.fixture.size,b.data(),b.size());Check(io.ok()&&io.bytes_transferred==b.size(),"isolated bundle fixture write");}Check(graph.fixture.device.Sync().ok(),"isolated bundle fixture sync");}
 void Good(const db::NativeManagementControlBundleRead& r)const{Check(r.ok()&&r.allocation_images==graph.after_bytes&&r.inventory_images==inventory&&r.page_headers.size()==headers.size()&&r.total_pages==graph.after.front().total_pages,"complete original target map and inventory images");for(unsigned n=0;n<headers.size();++n)Check(r.page_headers[n].page_uuid==headers[n].page_uuid&&Self(r.page_headers[n])==Self(headers[n]),"verified bundle page bindings");}
};
void Empty(const db::NativeManagementControlBundleRead& r){Check(!r.ok()&&r.allocation_images.empty()&&r.inventory_images.empty()&&r.directory_images.empty()&&r.page_headers.empty()&&!r.total_pages,"no failed bundle result prefix");}
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
 std::vector<page::NativeFilespaceDirectory> directories;std::vector<d::NativeCommonPageHeader> headers;BundleRoot root;Pages map_images,inventory_images,directory_images,pages;
 DirectoryBundle(Fixture& f,unsigned secondary_profile,bool reverse):fixture(f){
  const auto z=d::ReadFilespacePageZeroFromOpenDevice(f.device);Check(z.ok(),"mixed bundle primary bootstrap");zero=*z.record;
  const auto other=reverse?Id(0):Id(7);const auto& q=d::kCanonicalFilespacePageProfiles[secondary_profile];
  root.object_uuid=Id(10001);root.operation_uuid=Id(10002);root.map_count=2;root.inventory_count=2;root.directory_count=2;
  root.payload_bytes=4*f.size+2*u64{q.page_size_bytes}+48;root.page_count=(root.payload_bytes+f.size-385)/(f.size-384);
  for(unsigned i=0;i<root.page_count;++i)headers.push_back({u32(f.size),0x500,Id(1),Id(2),Id(10100+i),180+i,2,0,zero.bootstrap.page_size_profile_uuid});root.first=Self(headers.front());
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
  directories[0].next=Self(directories[1].header);Refresh();
 }
 u64 Budget()const{return root.page_count*fixture.size+4*root.payload_bytes+2*fixture.size;}
 void Refresh(){map_images.clear();inventory_images.clear();directory_images.resize(2);
  for(const auto& m:maps){const auto encoded=page::EncodeNativeAllocationMap(m);const auto raw=MapOracle(m);Check(encoded.ok()&&encoded.bytes==raw,"independent mixed bundle map image");map_images.push_back(raw);}
  for(const auto& p:inventories){const auto raw=InventoryOracle(p);const auto encoded=page::EncodeNativeTransactionInventoryPage(p);Check(encoded.ok()&&encoded.bytes==raw,"independent mixed inventory image");inventory_images.push_back(raw);}
  for(unsigned i=0;i<2;++i){auto& r=directories[i].records.front();const auto& m=maps[i];r.allocation_root=page::NativeFilespaceAllocationRoot{Self(m.header),m.object_uuid,Sha(map_images[i]),m.map_generation,m.capacity_generation};}
  for(unsigned left=2;left;--left){const auto i=left-1;if(!i)directories[0].next_sha256=Sha(directory_images[1]);const auto raw=DirectoryImageOracle(directories[i]);const auto encoded=page::EncodeNativeFilespaceDirectory(directories[i]);Check(encoded.ok()&&encoded.bytes==raw,"independent mixed directory image");directory_images[i]=raw;}Oracle();
 }
 void Oracle(){Bytes payload;for(const auto* group:{&map_images,&inventory_images,&directory_images})for(const auto& raw:*group){const auto at=payload.size();payload.resize(at+8);Num(payload,at,8,raw.size());payload.insert(payload.end(),raw.begin(),raw.end());}
  Check(payload.size()==root.payload_bytes,"independent framed payload exact length");root.aggregate_sha256=Sha(payload);pages.assign(headers.size(),Bytes(fixture.size));
  for(unsigned i=0;i<pages.size();++i){auto& b=pages[i];const auto& h=headers[i];std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,fixture.size);Num(b,16,4,0x500);Num(b,20,2,1);Num(b,22,2,1);Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);HeaderSeal(b);
   const u64 offset=i*(fixture.size-384),length=std::min<u64>(fixture.size-384,payload.size()-offset);std::copy_n("SBMCB003",8,b.begin()+128);Num(b,136,2,3);Num(b,138,2,256);Num(b,140,4,384+length);Put(b,144,root.object_uuid);Put(b,160,zero.page_uuid);Put(b,176,root.operation_uuid);Num(b,192,8,root.map_count);Num(b,200,8,root.payload_bytes);Num(b,208,8,offset);Num(b,216,8,i);Num(b,224,8,root.page_count);Num(b,232,8,root.first.page_number);std::copy(root.aggregate_sha256.begin(),root.aggregate_sha256.end(),b.begin()+240);Num(b,272,4,length);Num(b,276,8,root.inventory_count);Num(b,284,4,8);Num(b,352,8,root.directory_count);std::copy_n(payload.begin()+offset,length,b.begin()+384);}
  Reseal();
 }
 void ResealPayload(){Bytes payload;for(const auto& raw:pages){const auto length=LoadLittle32(raw.data()+272);payload.insert(payload.end(),raw.begin()+384,raw.begin()+384+length);}root.aggregate_sha256=Sha(payload);for(auto& raw:pages)std::copy(root.aggregate_sha256.begin(),root.aggregate_sha256.end(),raw.begin()+240);Reseal();}
 void Reseal(){std::array<byte,32> next{};for(std::size_t n=pages.size();n;--n){auto& raw=pages[n-1];std::copy(next.begin(),next.end(),raw.begin()+288);std::fill(raw.begin()+320,raw.begin()+352,0);const auto seal=Sha(raw);std::copy(seal.begin(),seal.end(),raw.begin()+320);next=Sha(raw);}root.first_page_sha256=next;}
 auto Encode()const{return db::EncodeNativeManagementControlBundle(map_images,Id(1),zero.page_uuid,root.object_uuid,root.operation_uuid,headers,Budget(),inventory_images,directory_images);}
 auto Decode(u64 budget=0)const{return db::DecodeNativeManagementControlBundle(pages,root,Id(1),zero.page_uuid,budget?budget:Budget());}
 auto Read()const{return db::ReadNativeManagementControlBundleFromOpenDevice(fixture.devices.front(),root,Id(1),zero.page_uuid,Budget());}
 void Install(){for(std::size_t i=0;i<pages.size();++i){const auto r=fixture.device.WriteAt((root.first.page_number+i)*fixture.size,pages[i].data(),pages[i].size());Check(r.ok()&&r.bytes_transferred==pages[i].size(),"isolated actual mixed bundle write");}Check(fixture.device.Sync().ok(),"actual mixed bundle barrier");}
 void Good(const db::NativeManagementControlBundleRead& r)const{Check(r.ok()&&r.allocation_images==map_images&&r.inventory_images==inventory_images&&r.directory_images==directory_images&&r.total_pages==256&&r.page_headers.size()==headers.size(),"complete exact original mixed bundle reconstruction");}
};
void MixedDirectoryBundle(unsigned primary,unsigned secondary,bool reverse){Fixture f(primary);DirectoryBundle b(f,secondary,reverse);
 const auto encoded=b.Encode();Check(encoded.ok()&&encoded.root==b.root&&encoded.pages==b.pages,"independent complete version3 framing bytes and full hashes");b.Good(b.Decode());const auto short_budget=b.Decode(b.Budget()-1);Check(short_budget.error==BE::resource_exhausted,"mixed bundle exact checked allowance");Empty(short_budget);b.Install();b.Good(b.Read());
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
 Graph g(f);Bundle old(g);g.plan.control_bundle=old.root;g.Refresh();for(unsigned field=0;field<2;++field){auto wrong=g.plan;if(field==0)wrong.control_bundle->directory_count=1;else wrong.control_bundle->payload_bytes=1;Failed(db::EncodeNativePublicationPlan(wrong));}
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
void OwnedInventoryPublication(unsigned profile,bool read_only,bool mixed=false) {
 Fixture f(profile);f.budget*=4;
 if(mixed)MixedInventoryPredecessor(f,profile);
 const auto initial=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
 Check(initial.ok(),"owned constructor actual initial checkpoint");
 auto inventory=initial.checkpoint_inventory.inventory;
 const unsigned count=profile==0?(f.size-384)/72+1:2;
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
  if(phase==2)for(unsigned n=0;n<40;++n){auto step=records::Step(1,n+1);step.operation_uuid=record.uuid;record.steps.push_back(step);}
  db::NativePublicationIntent intent{record.initiator_uuid,record.request_context_uuid,record.policy_snapshot_uuid,
    record.normalized_request_sha256,record.initiator_kind};intent.recovery_profile=2;
  const auto before=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(before.ok(),"owned constructor actual selected base");
  auto held=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*before.snapshot,Id(20100+phase),f.budget,&intent);
  Check(held.ok(),"owned constructor actual generation reservation");
  const auto pending=f.Read(0,256);
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
  const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(selected.ok()&&SameOwnedInventory(selected.checkpoint_inventory.inventory,inventory),
    "ordinary selected admission sees exact generated inventory");
  for(const auto& root:initial.checkpoint_inventory.checkpoint->roots)if(root.role!=1&&root.role!=4&&root.role!=16)
   Check(std::find(selected.checkpoint_inventory.checkpoint->roots.begin(),selected.checkpoint_inventory.checkpoint->roots.end(),root)!=
     selected.checkpoint_inventory.checkpoint->roots.end(),"owned constructor preserves every noninventory nonallocation root");
  for(std::size_t n=0;n<initial.allocation.pages.size();++n)for(const auto& old:initial.allocation.pages[n].map->records) {
   const auto& records=selected.allocation.pages[n].map->records;
   const auto found=std::find_if(records.begin(),records.end(),[&](const auto& r){return r.page_number==old.page_number;});
   Check(found!=records.end()&&*found==old,"owned constructor preserves original retained allocation bytes");
  }
  const auto after=f.Read(0,256);
  const auto retry=db::PublishNativeInventoryOnLease(*held.lease,record,inventory,f.budget,*f.issuer);
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
  _exit(selected.ok()&&selected.selection->selection_generation==4&&SameOwnedInventory(selected.checkpoint_inventory.inventory,inventory)?0:81);
 }
 int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
   "independent process admits production-generated starting and activation graph");
 if(mixed){Check(f.secondary.Open((f.path.parent_path()/"secondary").string(),d::FileOpenMode::open_existing_read_only).ok(),"reopen preserved secondary");
  Bytes actual(f.secondary_before.size());const auto read=f.secondary.ReadAt(0,actual.data(),actual.size());
  Check(read.ok()&&read.bytes_transferred==actual.size()&&actual==f.secondary_before,"owned constructor preserves every secondary byte");}
 std::cout<<"owned inventory profile="<<profile<<" read_only="<<read_only<<" mixed="<<mixed<<" PASS\n";
}
struct OwnedInventoryRequest {
 mga::LocalTransactionInventory inventory;
 db::NativeManagementOperation record;
 db::NativePublicationIntent intent;
 db::NativePublicationSnapshot pending;
 db::NativePublicationReservation held;
 explicit OwnedInventoryRequest(Fixture& f) {
  const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(selected.ok(),"owned failure fixture actual selected inventory");
  auto begun=mga::BeginLocalTransaction(selected.checkpoint_inventory.inventory,{UuidKind::transaction,Id(19000)},3000);
  Check(begun.ok(),"owned failure fixture actual BEGIN");inventory=std::move(begun.inventory);
  inventory.entries.back().state=mga::TransactionState::created;
  const auto zero=d::ReadFilespacePageZeroFromOpenDevice(f.device);Check(zero.ok(),"owned failure fixture bootstrap");
  record=records::Example(1);record.uuid=Id(20000);record.bootstrap_uuid=zero.record->page_uuid;
  record.security_snapshot_uuid={};record.generation_guards={};record.idempotency_key="owned-failure";
  intent={record.initiator_uuid,record.request_context_uuid,record.policy_snapshot_uuid,
    record.normalized_request_sha256,record.initiator_kind};intent.recovery_profile=2;
  const auto before=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(before.ok(),"owned failure fixture actual base");
  held=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*before.snapshot,Id(20100),f.budget,&intent);
  Check(held.ok(),"owned failure fixture actual reservation");pending=held.lease->snapshot();
 }
};
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
 const u64 allowance=bound.retained_image_bytes+(20+10*maps+14*inventory+4*extent+4*bundle+probes)*f.size+4*record.bytes.size();
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
void OwnedInventoryFaults(unsigned route,unsigned shard) {
 using PE=db::NativePublicationError;
 Fixture f(0);f.budget*=4;OwnedInventoryRequest request(f);
 const auto before=f.Read(0,256);
 const auto reset=[&] {
  request.held.lease.reset();const auto write=f.device.WriteAt(0,before.data(),before.size());
  Check(write.ok()&&write.bytes_transferred==before.size()&&f.device.Sync().ok(),"restore isolated pending fault fixture");
  request.held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),request.pending,Id(20100),request.intent,f.budget);
  Check(request.held.ok(),"resume exact isolated pending request");
  f.ResetIssuer();
 };
 const auto call=[&](u64 budget){return db::PublishNativeInventoryOnLease(*request.held.lease,request.record,request.inventory,budget,*f.issuer);};
 byte scratch=0;for(unsigned n=0;n<4097;++n)Check(f.device.ReadAt(0,&scratch,1).ok(),"stabilize optional observation allocations");
 owned_clock_controlled=true;owned_clock_millis=1700000000123ULL;
 reads=writes=syncs=entropy_calls=hash_seen=0;allocations=0;
 counting=hash_counting=io_counting=true;const auto good=call(f.budget);counting=hash_counting=io_counting=false;
 const auto nr=reads,nw=writes,ns=syncs,nh=hash_seen,ne=entropy_calls;const auto na=allocations;
 Check(good.ok()&&nr&&nw&&ns&&nh&&ne&&na,"owned complete publication fault baseline");
 std::cout<<"owned inventory fault baseline reads="<<nr<<" writes="<<nw<<" syncs="<<ns<<" hashes="<<nh<<" identities="<<ne<<" allocations="<<na<<'\n';
 if(route==0) {
  unsigned skipped_dirty=0;
  for(unsigned kind=0;kind<5;++kind)for(unsigned at=1;at<=(kind==0||kind==3?nr:kind==2?ns:nw);++at) {
   reset();reads=writes=syncs=0;read_fault=kind==0?at:0;write_fault=kind==1||kind==4?at:0;
   sync_fault=kind==2?at:0;corrupt_read=kind==3?at:0;torn_bytes=kind==4?f.size/2:0;
   io_counting=true;const auto result=call(f.budget);io_counting=false;
   const bool consumed=kind==0||kind==3?reads>=at:kind==2?syncs>=at:writes>=at;
   read_fault=write_fault=sync_fault=corrupt_read=0;torn_bytes=0;
   if(kind==3&&consumed&&result.ok()) {
    Check(corrupt_was_zero&&corrupt_offset>=0&&u64(corrupt_offset)%f.size==0,
      "only a physically zero free-slot probe may admit an alternate destination after corruption");
    const u64 page_number=u64(corrupt_offset)/f.size;
    const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
    Check(selected.ok()&&selected.checkpoint_inventory.inventory.entries.size()==request.inventory.entries.size()&&
      selected.checkpoint_inventory.inventory.entries.back().identity.transaction_uuid.value==request.inventory.entries.back().identity.transaction_uuid.value&&
      selected.checkpoint_inventory.inventory.entries.back().identity.local_id.value==request.inventory.entries.back().identity.local_id.value&&
      selected.checkpoint_inventory.inventory.entries.back().state==mga::TransactionState::created,
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
   if(!writes)Check(f.Read(0,256)==before,"read-only failure leaves every pending byte unchanged");
  }
  for(unsigned at=1;at<=ne;++at) {
   reset();entropy_fault=at;const auto result=call(f.budget);
   Check(!entropy_fault&&result.error==PE::identity_failure&&!result.snapshot&&f.Read(0,256)==before,"every generated identity entropy failure precedes graph mutation");
  }
  Check(skipped_dirty>0,"read-corrupted free slots exercise truthful alternate placement");
  std::cout<<"owned inventory corrupted free probes skipped="<<skipped_dirty<<'\n';
 }else if(route==1) {
  for(unsigned at=1;at<=nh;++at) {
   reset();hash_fault=shard+1;hash_target=at;hash_seen=0;hash_active=false;
   const auto result=call(f.budget);const bool consumed=!hash_fault;hash_fault=0;
   Check(consumed&&result.error==PE::hash_failure&&!result.snapshot,"owned publication consumes every digest fault without receipt");
  }
 }else if(route==2) {
  for(unsigned long at=shard;at<na;at+=32) {
   reset();const auto loss=f.device.failed_io_latency_observations();allocation_budget=at;
   const auto result=call(f.budget);const auto remaining=allocation_budget;allocation_budget=-1;
   Check(remaining<0&&(result.error==PE::resource_exhausted||(result.ok()&&f.device.failed_io_latency_observations()==loss+1)),
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
    std::vector<d::NativeFilespaceDevice> files{{Id(2),d::kCanonicalFilespacePageProfiles[0].uuid,&file}};
    auto held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),request.pending,Id(20100),request.intent,f.budget);
    if(!held.ok())_exit(81);f.ResetIssuer();reads=writes=syncs=0;kill_write=kind<2?at:0;kill_sync=kind>=2?at:0;
    kill_after_sync=kind==3;torn_bytes=kind==1?f.size/2:0;io_counting=true;
    (void)db::PublishNativeInventoryOnLease(*held.lease,request.record,request.inventory,f.budget,*f.issuer);_exit(87);
   }
   int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"owned writer dies at exact write or barrier");
   const auto recovery=fork();Check(recovery>=0,"fresh owned publication recovery process");
   if(!recovery) {
    d::FileDevice file;if(!file.Open(f.path.string(),d::FileOpenMode::open_existing).ok())_exit(80);
    std::vector<d::NativeFilespaceDevice> files{{Id(2),d::kCanonicalFilespacePageProfiles[0].uuid,&file}};
    auto observed=db::RecoverNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),f.budget);
    db::NativePublicationInspection result;
    if(observed.ok()) {
     if(observed.snapshot->selection.selection_generation==2)result=observed;
     else if(observed.snapshot->watermark.publication_plan) {
      auto held=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),*observed.snapshot,Id(20100),request.intent,f.budget);
      if(held.ok()) {
       result=db::ResumeNativeManagementControlGraphOnLease(*held.lease,f.budget);
       if(result.ok())result=db::PublishNativeManagementControlGraphOnLease(*held.lease,f.budget);
      }
     }
    }else result=db::RecoverNativeManagementCheckpointPublicationOnOpenDevices(Id(1),files,Id(2),Id(20100),request.intent,f.budget);
    if(result.ok()) {
     const auto final=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),files,Id(2),f.budget);
     _exit(final.ok()&&final.selection->selection_generation==2&&final.checkpoint_inventory.inventory.entries.size()==2&&
       final.checkpoint_inventory.inventory.entries[1].state==mga::TransactionState::created?0:82);
    }
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
  Check(completed&&incomplete,"owned death sweep covers complete and incomplete graphs");
  std::cout<<"owned recovery completed="<<completed<<" incomplete="<<incomplete<<'\n';
 }
 owned_clock_controlled=false;
 std::cout<<"owned inventory fault route="<<route<<" shard="<<shard<<" PASS\n";
}
void InventoryPlan(Graph& g,const Bundle& b){
 auto p=g.plan;p.control_bundle=b.root;p.base_selection_generation=1;p.intent.recovery_profile=2;
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
 Fixture f(profile);Graph g(f);Bundle b(g,3);const auto encoded=b.Encode();Check(encoded.ok()&&*encoded.root==b.root&&encoded.pages==b.pages,"independent version2 bundle bytes include exact complete inventory");b.Good(b.Decode());Check(b.Decode(b.Budget()-1).error==BE::resource_exhausted,"version2 exact checked image allowance");
 auto forbidden=g.plan;forbidden.control_bundle=b.root;Failed(db::EncodeNativePublicationPlan(forbidden));
 InventoryPlan(g,b);
 b.Install();const auto original=f.Read(0,256);reads=writes=syncs=0;io_counting=true;const auto actual=b.Read();io_counting=false;b.Good(actual);Check(!writes&&!syncs&&f.Read(0,256)==original,"inventory bundle actual reading never publishes or changes bytes");
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
void Test(unsigned profile){Fixture f(profile);Graph g(f);Bundle b(g);const auto encoded=b.Encode();Check(encoded.ok()&&*encoded.root==b.root&&encoded.pages==b.pages,"independent bundle bytes and root");b.Good(b.Decode());const auto short_budget=b.Decode(b.Budget()-1);Check(short_budget.error==BE::resource_exhausted,"exact bundle allowance");Empty(short_budget);
 b.Install();b.Good(b.Read());const auto untouched=f.Read(g.after.front().header.page_number);Check(std::all_of(untouched.begin(),untouched.end(),[](byte v){return !v;}),"bundle reconstructs target map not yet installed");
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
int main(int argc,char** argv){try{std::cout<<std::unitbuf;if(argc==2&&std::string_view(argv[1])=="--owned-inventory-placement"){OwnedInventoryPlacement(1);OwnedInventoryPlacement(2);OwnedInventoryBudgetAndStale();}else if(argc==4&&std::string_view(argv[1])=="--owned-inventory-faults"){const auto route=std::stoi(argv[2]),shard=std::stoi(argv[3]);Check(route>=0&&route<4&&shard>=0&&shard<(route==2?32:route==1?5:1),"owned fault arguments");OwnedInventoryFaults(route,shard);}else if(argc==2&&(std::string_view(argv[1])=="--owned-inventory-publication"||std::string_view(argv[1])=="--owned-inventory-mixed")){for(unsigned p=0;p<5;++p)for(bool ro:{false,true})OwnedInventoryPublication(p,ro,std::string_view(argv[1])=="--owned-inventory-mixed");}else if(argc==2&&std::string_view(argv[1])=="--directory-bundle"){for(unsigned p=0;p<5;++p)for(unsigned q=0;q<5;++q)for(bool reverse:{false,true})MixedDirectoryBundle(p,q,reverse);}else if(argc==5&&std::string_view(argv[1])=="--inventory-consumer-faults"){inventory_publication_mode=inventory_staging_admission=true;inventory_consumer=std::stoi(argv[2]);inventory_consumer_fault=std::stoi(argv[3]);inventory_consumer_shard=std::stoi(argv[4]);Check(inventory_consumer>=0&&inventory_consumer<2&&inventory_consumer_fault>=0&&inventory_consumer_fault<3&&inventory_consumer_shard>=0&&inventory_consumer_shard<(inventory_consumer_fault==2?128:inventory_consumer_fault==1?40:8),"consumer fault arguments");InventoryAllocation(0);}else if(argc==3&&std::string_view(argv[1])=="--inventory-allocation-reads"){inventory_publication_mode=inventory_staging_admission=true;inventory_allocation_read_shard=std::stoi(argv[2]);Check(inventory_allocation_read_shard>=0&&inventory_allocation_read_shard<8,"allocation reader fault shard");InventoryAllocation(0);}else if(argc==2&&std::string_view(argv[1])=="--inventory-staging-admission"){inventory_publication_mode=inventory_staging_admission=true;for(unsigned profile=0;profile<5;++profile)InventoryAllocation(profile);}else if(argc==2&&std::string_view(argv[1])=="--inventory-publication-mixed"){inventory_publication_mode=inventory_mixed_mode=true;for(unsigned profile=0;profile<5;++profile)InventoryAllocation(profile);}else if(argc==5&&std::string_view(argv[1])=="--inventory-resolution-faults"){inventory_publication_mode=inventory_resolution_mode=true;inventory_resolution_stage=std::stoi(argv[2]);inventory_resolution_route=std::stoi(argv[3]);inventory_resolution_shard=std::stoi(argv[4]);Check(inventory_resolution_stage>=0&&inventory_resolution_stage<2&&inventory_resolution_route>=0&&inventory_resolution_route<5&&inventory_resolution_shard>=0&&inventory_resolution_shard<(inventory_resolution_route==2?(inventory_resolution_stage?32:8):inventory_resolution_route==1?5:1),"inventory resolution fault arguments");InventoryAllocation(0);}else if(argc==2&&(std::string_view(argv[1])=="--inventory-resolution"||std::string_view(argv[1])=="--inventory-resolution-requests")){inventory_resolution_requests=std::string_view(argv[1])=="--inventory-resolution-requests";inventory_publication_mode=inventory_resolution_mode=true;for(unsigned profile=0;profile<5;++profile)InventoryAllocation(profile);}else if(argc==5&&(std::string_view(argv[1])=="--inventory-install-faults"||std::string_view(argv[1])=="--inventory-reconstruction-faults")){inventory_publication_mode=true;inventory_reconstruction_faults=std::string_view(argv[1])=="--inventory-reconstruction-faults";inventory_install_stage=std::stoi(argv[2]);inventory_install_route=std::stoi(argv[3]);inventory_install_shard=std::stoi(argv[4]);Check(inventory_install_stage>=0&&inventory_install_stage<2&&inventory_install_route>=0&&inventory_install_route<(inventory_reconstruction_faults?3:4)&&inventory_install_shard>=0&&inventory_install_shard<(inventory_install_route==2?(inventory_reconstruction_faults?(inventory_install_stage?64:16):(inventory_install_stage?32:8)):inventory_install_route==1?5:1),"inventory installer fault arguments");InventoryAllocation(0);}else if(argc==2&&(std::string_view(argv[1])=="--inventory-publication"||std::string_view(argv[1])=="--inventory-publication-cold"||std::string_view(argv[1])=="--inventory-publication-read-only")){inventory_publication_mode=true;inventory_publication_cold=std::string_view(argv[1])=="--inventory-publication-cold";inventory_read_only=std::string_view(argv[1])=="--inventory-publication-read-only";for(unsigned profile=0;profile<5;++profile)InventoryAllocation(profile);}else if(argc==4&&std::string_view(argv[1])=="--inventory-reader-allocations"){inventory_allocation_route=std::stoi(argv[2]);inventory_allocation_shard=std::stoi(argv[3]);Check(inventory_allocation_route>=0&&inventory_allocation_route<2&&inventory_allocation_shard>=0&&inventory_allocation_shard<4,"inventory reader allocation shard arguments");InventoryAllocation(0);}else{Check(argc==1,"test arguments");for(unsigned profile=0;profile<5;++profile){InventoryAllocation(profile);InventoryBundle(profile);Test(profile);}}std::cout<<"PASS management control bundle checks="<<checks<<" not_SQL_E2E=true\n";return 0;}catch(const std::exception& e){allocation_budget=-1;hash_fault=0;io_counting=false;std::cerr<<"FAIL management control bundle checks="<<checks<<" "<<e.what()<<'\n';return 1;}}

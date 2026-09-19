// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_control_bundle.hpp"
#include "native_management_control_allocation.hpp"
#include "native_management_history.hpp"
#include "native_creation_workspace.hpp"
#include "disk_device.hpp"
#include <filesystem>
#include <unistd.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <source_location>
#include <stdexcept>
#include <cerrno>
#include <sys/wait.h>
namespace {long allocation_budget=-1;bool counting=false;unsigned long allocations=0;unsigned hash_fault=0,hash_target=1,hash_seen=0;bool hash_active=false,hash_counting=false;
bool io_counting=false;unsigned reads=0,writes=0,syncs=0,read_fault=0,write_fault=0,sync_fault=0,kill_write=0,corrupt_read=0;std::size_t torn_bytes=0;int allocation_shard=-1;}
void* operator new(std::size_t n){if(counting)++allocations;if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
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
extern "C" ssize_t __wrap_pread(int fd,void* data,size_t n,off_t at){
 if(io_counting&&++reads==read_fault){errno=EIO;return -1;}const auto result=__real_pread(fd,data,n,at);
 if(io_counting&&reads==corrupt_read&&result>0)static_cast<unsigned char*>(data)[result-1]^=1;return result;
}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t n,off_t at){
 if(io_counting){++writes;if(writes==kill_write)_exit(86);
   if(writes==write_fault){if(torn_bytes){const auto result=__real_pwrite(fd,data,std::min(n,torn_bytes),at);if(result<0)return result;}
     errno=EIO;return -1;}}
 return __real_pwrite(fd,data,n,at);
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){if(io_counting&&++syncs==sync_fault){errno=EIO;return -1;}return __real_fsync(fd);}


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
 std::filesystem::path path;d::FileDevice device;std::vector<d::NativeFilespaceDevice> devices;u64 size,budget;
 Fixture(unsigned profile){
  char name[]="/tmp/sb-management-bundle.XXXXXX";Check(mkdtemp(name),"isolated fixture directory");path=std::filesystem::path(name)/"node";
  const auto& page=d::kCanonicalFilespacePageProfiles[profile];size=page.page_size_bytes;budget=1024*size;
  db::NativeFilespaceInitializationRequest r;r.bootstrap={Id(1),Id(2),page.uuid,d::kNativeBootstrapIntegrityProfile,{},page.page_size_bytes,1,0,1,7};r.operation_uuid=Id(3);r.writer_uuid=Id(4);
  r.creator.transaction_uuid={UuidKind::transaction,Id(5)};r.creator.local_id=mga::MakeLocalTransactionId(1);r.creator.scope=mga::TransactionScope::local_node;r.creation_utc_millis=1789357072000ULL;r.total_pages=256;
  Check(device.Open(path.string(),d::FileOpenMode::create_new).ok(),"owned device");devices={{Id(2),page.uuid,&device}};
  Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(device,r,budget).ok(),"actual genesis");
 }
 ~Fixture(){device.Close();std::error_code ec;std::filesystem::remove_all(path.parent_path(),ec);}
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
 Bundle(Graph& g,unsigned inventory_count=0):graph(g){const u64 size=g.fixture.size,count=((g.after.size()+inventory_count)*size+size-385)/(size-384);
  std::vector<page::NativeTransactionInventoryPage> chain;
  for(unsigned n=0;n<inventory_count;++n){page::NativeTransactionInventoryPage p;p.header={u32(size),0x301,Id(1),Id(2),Id(9500+n),220+n,2,0,g.zero.bootstrap.page_size_profile_uuid};p.object_uuid=Id(910);p.inventory_generation=2;p.inventory.next_local_transaction_id=inventory_count+1;p.inventory.next_commit_sequence=2;
   mga::TransactionInventoryEntry e;e.identity.transaction_uuid={UuidKind::transaction,Id(9600+n)};e.identity.local_id=mga::MakeLocalTransactionId(n+1);e.identity.scope=mga::TransactionScope::local_node;e.state=n?mga::TransactionState::created:mga::TransactionState::committed;e.begin_unix_epoch_millis=1000+n;e.begin_visible_through_local_transaction_id=n;e.begin_visible_through_commit_sequence=n?1:0;if(!n){e.commit_sequence=1;e.final_unix_epoch_millis=2000;e.evidence_record_written=true;}p.inventory.entries.push_back(e);chain.push_back(p);
   auto& m=Cover(g.after,p.header.page_number);Check(m.states[p.header.page_number-m.first_page]==State::free,"inventory slot free");m.states[p.header.page_number-m.first_page]=State::allocated;page::NativeAllocationRecord r;r.page_number=p.header.page_number;r.allocation_uuid=Id(8500+n);r.page_uuid=p.header.page_uuid;r.owner_uuid=p.object_uuid;r.page_generation=2;r.page_type=0x301;r.creator_operation_uuid=g.plan.operation_uuid;m.records.push_back(r);
  }
  for(unsigned n=0;n<chain.size();++n){auto& p=chain[n];if(n)p.previous=Self(chain[n-1].header);if(n+1<chain.size())p.next=Self(chain[n+1].header);const auto image=page::EncodeNativeTransactionInventoryPage(p);Check(image.ok(),"complete candidate inventory fixture");inventory.push_back(image.bytes);}
  for(unsigned n=0;n<count;++n){d::NativeCommonPageHeader h{u32(size),0x500,Id(1),Id(2),Id(9000+n),180+n,2,0,g.zero.bootstrap.page_size_profile_uuid};headers.push_back(h);auto& m=Cover(g.after,h.page_number);Check(m.states[h.page_number-m.first_page]==State::free,"bundle slot free");m.states[h.page_number-m.first_page]=State::allocated;page::NativeAllocationRecord r;r.page_number=h.page_number;r.allocation_uuid=Id(8000+n);r.page_uuid=h.page_uuid;r.owner_uuid=Id(900);r.page_generation=2;r.page_type=0x500;r.creator_operation_uuid=g.plan.operation_uuid;m.records.push_back(r);}
  for(auto& m:g.after)std::sort(m.records.begin(),m.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});g.Refresh();
  root.first=Self(headers.front());root.object_uuid=Id(900);root.operation_uuid=g.plan.operation_uuid;root.map_count=g.after.size();root.inventory_count=inventory_count;root.page_count=headers.size();Oracle();
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
void Empty(const db::NativeManagementControlBundleRead& r){Check(!r.ok()&&r.allocation_images.empty()&&r.inventory_images.empty()&&r.page_headers.empty()&&!r.total_pages,"no failed bundle result prefix");}
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
void InventoryAllocation(unsigned profile){
 Fixture f(profile);Graph g(f);Bundle b(g,3);const auto old_root=*std::find_if(g.base.roots.begin(),g.base.roots.end(),[](const auto& r){return r.role==1;});Pages before;auto ref=std::optional{old_root.page};
 while(ref){auto raw=f.Read(ref->page_number);auto p=page::DecodeNativeTransactionInventoryPage(raw);Check(p.ok(),"actual selected-before inventory fixture");before.push_back(raw);ref=p.page->next;}
 Check(before.size()==1,"genesis fixture has one inventory image");const auto old=*page::DecodeNativeTransactionInventoryPage(before.front()).page;std::vector<page::NativeTransactionInventoryPage> candidate;
 for(unsigned i=0;i<3;++i){auto p=*page::DecodeNativeTransactionInventoryPage(b.inventory[i]).page;p.object_uuid=old.object_uuid;p.inventory.next_local_transaction_id=old.inventory.next_local_transaction_id+2;p.inventory.next_commit_sequence=old.inventory.next_commit_sequence;
  if(!i)p.inventory.entries=old.inventory.entries;else{auto& e=p.inventory.entries.front();e.identity.local_id=mga::MakeLocalTransactionId(old.inventory.next_local_transaction_id+i-1);e.begin_visible_through_local_transaction_id=e.identity.local_id.value-1;e.begin_visible_through_commit_sequence=old.inventory.next_commit_sequence-1;}
  Find(g.after,p.header.page_number).owner_uuid=p.object_uuid;candidate.push_back(p);
 }
 const auto refresh=[&]{for(unsigned i=0;i<3;++i){b.inventory[i]=InventoryOracle(candidate[i]);const auto encoded=page::EncodeNativeTransactionInventoryPage(candidate[i]);Check(encoded.ok()&&encoded.bytes==b.inventory[i],"independent inventory payload bytes");}g.after_bytes=EncodeMaps(g.after);b.Oracle();g.plan.control_bundle=b.root;g.plan.base_selection_generation=1;g.plan.intent.recovery_profile=2;
  auto& root=*std::find_if(g.target.roots.begin(),g.target.roots.end(),[](const auto& r){return r.role==1;});root={1,0x301,Self(candidate.front().header),old.object_uuid,Sha(b.inventory.front())};g.target.selected_local_transaction_id=candidate.front().inventory.next_local_transaction_id-1;g.Refresh();};refresh();
 const auto budget=[&]{u64 n=g.Budget();for(const auto& raw:b.pages)n+=raw.size();for(const auto* images:{&before,&b.inventory})for(const auto& raw:*images)n+=4*raw.size();return n;};
 const auto call=[&](u64 limit=0){return db::ValidateNativeManagementControlAllocation(g.base_bytes,g.target_bytes,g.plan_bytes,g.extent,g.before_bytes,g.after_bytes,limit?limit:budget(),b.pages,before);};
 Check(call()==CE::none,"complete original allocation plus starting inventory delta");Check(call(budget()-1)==CE::resource_exhausted,"inventory delta exact work allowance");
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
 candidate=saved;before=saved_before;g.base=initial_base;g.base_bytes=initial_base_bytes;g.plan=initial_plan;refresh();Check(call()==CE::none,"restore starting allocation after lifecycle and cluster grids");if(profile)return;
 allocations=0;counting=true;Check(call()==CE::none,"inventory control delta allocation baseline");counting=false;const auto sites=allocations;for(unsigned long at=0;at<sites;++at){allocation_budget=at;const auto result=call();const auto left=allocation_budget;allocation_budget=-1;Check(left==-1&&result==CE::resource_exhausted,"inventory control delta consumes every allocation failure");}
 hash_counting=true;hash_seen=0;Check(call()==CE::none,"inventory control delta hash baseline");hash_counting=false;const auto hashes=hash_seen;for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto result=call();Check(!hash_fault&&result==CE::hash_failure,"inventory control delta propagates every hash failure");}
 std::cout<<"inventory control delta allocations="<<sites<<" hashes="<<hashes<<'\n';
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
int main(){try{for(unsigned profile=0;profile<5;++profile){InventoryAllocation(profile);InventoryBundle(profile);Test(profile);}std::cout<<"PASS management control bundle checks="<<checks<<" reconstruction_input_only=true not_allocation_publication_or_SQL_E2E=true\n";return 0;}catch(const std::exception& e){allocation_budget=-1;hash_fault=0;io_counting=false;std::cerr<<"FAIL management control bundle checks="<<checks<<" "<<e.what()<<'\n';return 1;}}

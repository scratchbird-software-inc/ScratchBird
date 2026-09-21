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
bool io_counting=false;unsigned reads=0,writes=0,syncs=0,read_fault=0,write_fault=0,sync_fault=0,kill_write=0,corrupt_read=0,corrupt_write=0;std::size_t torn_bytes=0;int allocation_shard=-1;}
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
 const auto result=__real_pwrite(fd,data,n,at);
 if(io_counting&&writes==corrupt_write&&result>0){unsigned char last=0;if(__real_pread(fd,&last,1,at+result-1)==1){last^=1;(void)__real_pwrite(fd,&last,1,at+result-1);}}
 return result;
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
 const bool v2=p.management_extent.has_value(),v3=p.control_bundle.has_value();std::copy_n(v3?"SBPPM003":v2?"SBPPM002":"SBPPM001",8,b.begin()+128);Num(b,136,2,v3?3:v2?2:1);Num(b,138,2,v3?1024:v2?896:640);Num(b,140,4,v3?1152:v2?1024:768);
 Put(b,144,p.object_uuid);Put(b,160,p.bootstrap_uuid);Put(b,176,p.timeline_uuid);Put(b,192,p.operation_uuid);Put(b,208,p.intent.initiator_uuid);Put(b,224,p.intent.request_context_uuid);Put(b,240,p.intent.policy_snapshot_uuid);Put(b,256,p.security_snapshot_uuid);
 std::copy(p.intent.normalized_request_sha256.begin(),p.intent.normalized_request_sha256.end(),b.begin()+272);std::copy(p.reservation_state_sha256.begin(),p.reservation_state_sha256.end(),b.begin()+304);
 Num(b,336,8,p.reserved_generation);Num(b,344,8,p.base_checkpoint_generation);Num(b,352,8,p.base_root_set_generation);Num(b,360,8,p.target_root_set_generation);
 Ref(b,368,p.base_checkpoint);Put(b,416,p.base_checkpoint_object_uuid);std::copy(p.base_checkpoint_sha256.begin(),p.base_checkpoint_sha256.end(),b.begin()+432);Ref(b,464,p.target_checkpoint);Put(b,512,p.target_checkpoint_object_uuid);std::copy(p.target_graph_sha256.begin(),p.target_graph_sha256.end(),b.begin()+528);
 if(p.previous_plan)Ref(b,560,*p.previous_plan);Put(b,608,p.previous_plan_object_uuid);std::copy(p.previous_plan_sha256.begin(),p.previous_plan_sha256.end(),b.begin()+624);
 Num(b,656,8,p.catalog_generation);Num(b,664,8,p.configuration_generation);Num(b,672,8,p.security_generation);Num(b,680,2,p.intent.initiator_kind);Num(b,682,2,1);Num(b,684,2,2);
 if(p.management_extent){const auto& r=*p.management_extent;Num(b,720,4,p.generation_guard_flags);Ref(b,736,r.first);Put(b,784,r.object_uuid);Put(b,800,r.operation_uuid);Num(b,816,8,r.revision);Num(b,824,4,r.aggregate_bytes);Num(b,828,4,r.page_count);std::copy(r.aggregate_sha256.begin(),r.aggregate_sha256.end(),b.begin()+832);std::copy(r.first_page_sha256.begin(),r.first_page_sha256.end(),b.begin()+864);}
 if(p.control_bundle){const auto& r=*p.control_bundle;Ref(b,896,r.first);Put(b,944,r.object_uuid);Num(b,960,8,r.map_count);Num(b,968,8,r.page_count);std::copy(r.aggregate_sha256.begin(),r.aggregate_sha256.end(),b.begin()+976);std::copy(r.first_page_sha256.begin(),r.first_page_sha256.end(),b.begin()+1008);}PlanSeal(b);return b;
}
void Failed(const db::NativePublicationPlanImage& r){Check(!r.ok()&&!r.plan&&r.bytes.empty()&&std::all_of(r.sha256.begin(),r.sha256.end(),[](byte v){return !v;}),"no failed plan prefix");}
void Bad(const db::NativePublicationPlan& p){Failed(db::EncodeNativePublicationPlan(p));const auto raw=Oracle(p);Failed(db::DecodeNativePublicationPlan(raw));}
struct Fixture {
 std::filesystem::path path;d::FileDevice device;std::vector<d::NativeFilespaceDevice> devices;u64 size,budget;
 Fixture(unsigned profile){
  char name[]="/tmp/sb-management-control-install.XXXXXX";Check(mkdtemp(name),"isolated fixture directory");path=std::filesystem::path(name)/"node";
  const auto& page=d::kCanonicalFilespacePageProfiles[profile];size=page.page_size_bytes;budget=1024*size;
  db::NativeFilespaceInitializationRequest r;r.bootstrap={Id(1),Id(2),page.uuid,d::kNativeBootstrapIntegrityProfile,{},page.page_size_bytes,1,0,1,7};r.operation_uuid=Id(3);r.writer_uuid=Id(4);
  r.creator.transaction_uuid={UuidKind::transaction,Id(5)};r.creator.local_id=mga::MakeLocalTransactionId(1);r.creator.scope=mga::TransactionScope::local_node;r.creation_utc_millis=1789357072000ULL;r.total_pages=256;
  Check(device.Open(path.string(),d::FileOpenMode::create_new).ok(),"owned device");devices={{Id(2),page.uuid,&device}};
  r.policy_snapshot_uuid=Id(10);scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({r.bootstrap.database_uuid,r.policy_snapshot_uuid},{{},0,1000});Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(device,r,budget,issuer).ok(),"actual genesis");
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
 Graph& graph;std::vector<d::NativeCommonPageHeader> headers;BundleRoot root;Pages pages;
 Bundle(Graph& g):graph(g){const u64 size=g.fixture.size,count=(g.after.size()*size+size-385)/(size-384);
  for(unsigned n=0;n<count;++n){d::NativeCommonPageHeader h{u32(size),0x500,Id(1),Id(2),Id(9000+n),180+n,2,0,g.zero.bootstrap.page_size_profile_uuid};headers.push_back(h);auto& m=Cover(g.after,h.page_number);Check(m.states[h.page_number-m.first_page]==State::free,"bundle slot free");m.states[h.page_number-m.first_page]=State::allocated;page::NativeAllocationRecord r;r.page_number=h.page_number;r.allocation_uuid=Id(8000+n);r.page_uuid=h.page_uuid;r.owner_uuid=Id(900);r.page_generation=2;r.page_type=0x500;r.creator_operation_uuid=g.plan.operation_uuid;m.records.push_back(r);}
  for(auto& m:g.after)std::sort(m.records.begin(),m.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});g.Refresh();
  root.first=Self(headers.front());root.object_uuid=Id(900);root.operation_uuid=g.plan.operation_uuid;root.map_count=g.after.size();root.page_count=headers.size();Oracle();
 }
 u64 Budget()const{return root.page_count*graph.fixture.size+4*root.map_count*graph.fixture.size+2*graph.fixture.size;}
 void Reseal(){std::array<byte,32> next{};for(std::size_t n=pages.size();n;--n){auto& b=pages[n-1];std::copy(next.begin(),next.end(),b.begin()+288);std::fill(b.begin()+320,b.begin()+352,0);const auto hash=Sha(b);std::copy(hash.begin(),hash.end(),b.begin()+320);next=Sha(b);}root.first_page_sha256=next;}
 void Oracle(){const u64 size=graph.fixture.size,capacity=size-384;Bytes payload;for(const auto& b:graph.after_bytes)payload.insert(payload.end(),b.begin(),b.end());root.aggregate_sha256=Sha(payload);pages.assign(headers.size(),Bytes(size));
  for(unsigned i=0;i<pages.size();++i){const auto& h=headers[i];auto& b=pages[i];std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,size);Num(b,16,4,0x500);Num(b,20,2,1);Num(b,22,2,1);Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);HeaderSeal(b);const u64 offset=i*capacity,length=std::min<u64>(capacity,payload.size()-offset);
   std::copy_n("SBMCB001",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,256);Num(b,140,4,384+length);Put(b,144,root.object_uuid);Put(b,160,graph.zero.page_uuid);Put(b,176,root.operation_uuid);Num(b,192,8,root.map_count);Num(b,200,8,payload.size());Num(b,208,8,offset);Num(b,216,8,i);Num(b,224,8,root.page_count);Num(b,232,8,root.first.page_number);std::copy(root.aggregate_sha256.begin(),root.aggregate_sha256.end(),b.begin()+240);Num(b,272,4,length);std::copy_n(payload.begin()+offset,length,b.begin()+384);}
  Reseal();
 }
 auto Encode()const{return db::EncodeNativeManagementControlBundle(graph.after_bytes,Id(1),graph.zero.page_uuid,root.object_uuid,root.operation_uuid,headers,Budget());}
 auto Decode(u64 budget=0)const{return db::DecodeNativeManagementControlBundle(pages,root,Id(1),graph.zero.page_uuid,budget?budget:Budget());}
 auto Read()const{return db::ReadNativeManagementControlBundleFromOpenDevice(graph.fixture.devices.front(),root,Id(1),graph.zero.page_uuid,Budget());}
 void Install(){for(std::size_t n=0;n<pages.size();++n){const auto& b=pages[n];const auto io=graph.fixture.device.WriteAt((root.first.page_number+n)*graph.fixture.size,b.data(),b.size());Check(io.ok()&&io.bytes_transferred==b.size(),"isolated bundle fixture write");}Check(graph.fixture.device.Sync().ok(),"isolated bundle fixture sync");}
 void Good(const db::NativeManagementControlBundleRead& r)const{Check(r.ok()&&r.allocation_images==graph.after_bytes&&r.page_headers.size()==headers.size()&&r.total_pages==graph.after.front().total_pages,"complete original target map images");for(unsigned n=0;n<headers.size();++n)Check(r.page_headers[n].page_uuid==headers[n].page_uuid&&Self(r.page_headers[n])==Self(headers[n]),"verified bundle page bindings");}
};
using PE=db::NativePublicationError;
void Empty(const db::NativePublicationInspection& r){Check(!r.ok()&&!r.snapshot,"no failed installation snapshot");}
void ResetIO(){reads=writes=syncs=read_fault=write_fault=sync_fault=kill_write=corrupt_read=corrupt_write=0;torn_bytes=0;}
void Write(Fixture& f,u64 n,const Bytes& b){const auto r=f.device.WriteAt(n*f.size,b.data(),b.size());Check(r.ok()&&r.bytes_transferred==b.size(),"isolated fixture restore");Check(f.device.Sync().ok(),"isolated restore sync");}
auto Reserve(Fixture& f,Graph& g){const auto inspected=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);Check(inspected.ok(),"inspect actual base");
 auto r=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*inspected.snapshot,g.plan.operation_uuid,f.budget,&g.plan.intent);Check(r.ok(),"reserve actual intent");g.plan.reservation_state_sha256=r.lease->snapshot().state_sha256;g.Refresh();return r;}
auto Resume(Fixture& f,const Graph& g){const auto repaired=db::RecoverNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);Check(repaired.ok(),"repair actual watermark");
 auto r=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*repaired.snapshot,g.plan.operation_uuid,g.plan.intent,f.budget);Check(r.ok(),"resume exact actual intent");return r;}
auto Install(Fixture& f,const Graph& g,const Bundle& b,db::NativePublicationReservation& r){return db::InstallNativeManagementControlGraphOnLease(*r.lease,g.plan,g.target_bytes,g.extent,b.pages,f.budget);}
void PostState(Fixture& f,const Graph& g,const Bundle& b,const Bytes& before){
 Bytes expected=before;
 const auto put=[&](u64 n,const Bytes& bytes){std::copy(bytes.begin(),bytes.end(),expected.begin()+n*f.size);};
 for(const auto& root:g.zero.roots)if(root.kind==20||root.kind==21)put(root.page_number,f.Read(root.page_number));
 put(g.plan.header.page_number,g.plan_bytes);put(g.plan.target_checkpoint.page_number,g.target_bytes);
 for(unsigned i=0;i<g.extent.size();++i)put(g.plan.management_extent->first.page_number+i,g.extent[i]);
 for(unsigned i=0;i<b.pages.size();++i)put(b.root.first.page_number+i,b.pages[i]);
 for(unsigned i=0;i<g.after_bytes.size();++i)put(g.after[i].header.page_number,g.after_bytes[i]);
 Check(f.Read(0,256)==expected,"entire file equals only intended unselected writes; selectors unchanged");
 const auto inspected=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);
 Check(inspected.ok()&&inspected.snapshot->selection.checkpoint==g.plan.base_checkpoint&&inspected.snapshot->selection.checkpoint_generation==1,"old selected base still authoritative");
 Check(inspected.snapshot->watermark.publication_plan&&inspected.snapshot->watermark.publication_plan->sha256==Sha(g.plan_bytes),"durable exact anchored plan");
}
void Profiles(){
 for(unsigned profile=0;profile<5;++profile){Fixture f(profile);Graph g(f);Bundle b(g);g.plan.control_bundle=b.root;g.Refresh();auto r=Reserve(f,g);const auto before=f.Read(0,256);
  ResetIO();io_counting=true;auto installed=Install(f,g,b,r);io_counting=false;Check(installed.ok(),"actual complete control installation");
  Check(writes==4+g.extent.size()+b.pages.size()+g.after.size()&&syncs==8,"all dependency writes and eight barriers");PostState(f,g,b,before);
  ResetIO();io_counting=true;auto retry=Install(f,g,b,r);io_counting=false;Check(retry.ok()&&!writes&&syncs>=5,"exact retry skips writes but verifies barriers");PostState(f,g,b,before);
  r.lease.reset();Check(f.device.Close().ok(),"close after durable install");Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"fresh open");
  r=Resume(f,g);ResetIO();io_counting=true;const auto cold=db::ResumeNativeManagementControlGraphOnLease(*r.lease,f.budget);io_counting=false;
  if(!cold.ok())std::cerr<<"cold error="<<unsigned(cold.error)<<"\n";
  Check(cold.ok()&&!writes,"cold reconstruction exact identity no rewriting");PostState(f,g,b,before);
 }
}
void Invalid(){
 Fixture f(0);Graph g(f);Bundle b(g);g.plan.control_bundle=b.root;g.Refresh();auto r=Reserve(f,g);const auto before=f.Read(0,256);
 for(unsigned mode=0;mode<8;++mode){auto p=g.plan;auto cp=g.target_bytes;auto ext=g.extent;auto bundle=b.pages;u64 budget=f.budget;
  switch(mode){case 0:cp.push_back(0);break;case 1:ext.push_back(ext.front());break;case 2:bundle.front().push_back(0);break;case 3:bundle.clear();break;case 4:p.intent.request_context_uuid=Id(777);break;case 5:p.target_graph_sha256[0]^=1;break;case 6:budget=1;break;case 7:p.control_bundle->map_count=std::numeric_limits<u64>::max();break;}
  ResetIO();io_counting=true;auto failed=db::InstallNativeManagementControlGraphOnLease(*r.lease,p,cp,ext,bundle,budget);io_counting=false;Empty(failed);Check(!writes&&!syncs&&f.Read(0,256)==before,"invalid input leaves full file untouched");
 }
 for(u64 target:{g.plan.header.page_number,g.plan.management_extent->first.page_number,b.root.first.page_number,g.after.front().header.page_number,g.plan.target_checkpoint.page_number}){
  Bytes occupied(f.size);occupied.back()=11;Write(f,target,occupied);const auto bad=f.Read(0,256);ResetIO();io_counting=true;auto refused=Install(f,g,b,r);io_counting=false;
  Empty(refused);Check(refused.error==PE::preimage_changed&&!writes&&!syncs&&f.Read(0,256)==bad,"unanchored occupied slot preserved");Write(f,target,Bytes(f.size));
 }
 {Graph changed(f);Bundle changed_bundle(changed);Find(changed.after,changed.base.header.page_number).owner_uuid=Id(999);changed.Refresh();changed_bundle.Oracle();changed.plan.control_bundle=changed_bundle.root;changed.plan.reservation_state_sha256=g.plan.reservation_state_sha256;changed.Refresh();ResetIO();io_counting=true;const auto invalid_delta=Install(f,changed,changed_bundle,r);io_counting=false;Empty(invalid_delta);Check(invalid_delta.error==PE::allocation_mismatch&&!writes&&!syncs&&f.Read(0,256)==before,"resealed target cannot change retained allocation ownership");}
 for(const auto& root:g.zero.roots)if(root.kind==20||root.kind==21){auto wm=db::DecodeNativePublicationWatermark(f.Read(root.page_number));Check(wm.ok(),"actual watermark before stale-base fixture");wm.state->operation_uuid=Id(777);auto encoded=db::EncodeNativePublicationWatermark(*wm.state);Check(encoded.ok(),"valid foreign pending operation fixture");Write(f,root.page_number,encoded.bytes);}
 const auto foreign=f.Read(0,256);ResetIO();io_counting=true;const auto stale=Install(f,g,b,r);io_counting=false;Empty(stale);Check(stale.error==PE::stale_base&&!writes&&!syncs&&f.Read(0,256)==foreign,"actual stale base defeats retained caller snapshot");Write(f,0,before);
 Check(Install(f,g,b,r).ok(),"preflight failure does not poison lease");PostState(f,g,b,before);
 r.lease.reset();const auto installed=f.Read(0,256);Check(f.device.Close().ok(),"close writable device");Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"read-only reopen");
 const auto inspected=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);Check(inspected.ok(),"inspect read-only pending operation");ResetIO();io_counting=true;
 auto readonly=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*inspected.snapshot,g.plan.operation_uuid,g.plan.intent,f.budget);io_counting=false;
 Check(!readonly.ok()&&!readonly.lease&&!writes&&!syncs&&f.Read(0,256)==installed,"read-only device cannot grant installer lease");
}
void Faults(){
 Fixture f(0);Graph g(f,false,2);Bundle b(g);g.plan.control_bundle=b.root;g.Refresh();auto r=Reserve(f,g);const auto original=f.Read(0,256);
 ResetIO();hash_seen=0;hash_counting=true;counting=true;allocations=0;io_counting=true;const auto good=Install(f,g,b,r);io_counting=false;counting=false;hash_counting=false;
 const unsigned nr=reads,nw=writes,ns=syncs,nh=hash_seen;const auto na=allocations;Check(good.ok(),"measured actual installer");r.lease.reset();
 std::cout<<"installer reads="<<nr<<" writes="<<nw<<" syncs="<<ns<<" hashes="<<nh<<" allocations="<<na<<"\n";
 for(unsigned mode=0;mode<7;++mode){const unsigned total=mode==0||mode==5?nr:mode==1||mode==3||mode==6?nw:mode==2?ns:5*nh;
  for(unsigned at=1;at<=total;++at){Write(f,0,original);r=Resume(f,g);ResetIO();hash_seen=0;
   if(mode==0)read_fault=at;if(mode==1||mode==3)write_fault=at;if(mode==2)sync_fault=at;if(mode==3)torn_bytes=f.size/2;
   if(mode==5)corrupt_read=at;
   if(mode==6)corrupt_write=at;
   if(mode==4){hash_fault=1+(at-1)/nh;hash_target=1+(at-1)%nh;}io_counting=true;const auto failed=Install(f,g,b,r);io_counting=false;hash_fault=0;hash_active=false;
   Empty(failed);if(mode==4)Check(failed.error==PE::hash_failure&&!writes&&!syncs,"hash failures all preflight no mutation");
   const bool changed=writes||syncs;ResetIO();
   if(changed){const auto poisoned=Install(f,g,b,r);Empty(poisoned);Check(poisoned.error==PE::stale_base,"ambiguous lease cannot retry");}
   r.lease.reset();r=Resume(f,g);Check(Install(f,g,b,r).ok(),"exact input resumes every interrupted write");r.lease.reset();PostState(f,g,b,original);
  }
 }
 ResetIO();
}
void ColdFaults(){
 Fixture f(0);Graph g(f,false,2);Bundle b(g);g.plan.control_bundle=b.root;g.Refresh();auto r=Reserve(f,g);Check(Install(f,g,b,r).ok(),"cold fault prerequisites installed");r.lease.reset();
 for(const auto& m:g.after)Write(f,m.header.page_number,Bytes(f.size));Write(f,g.plan.target_checkpoint.page_number,Bytes(f.size));const auto original=f.Read(0,256);r=Resume(f,g);
 ResetIO();hash_seen=0;hash_counting=true;io_counting=true;const auto baseline=db::ResumeNativeManagementControlGraphOnLease(*r.lease,f.budget);io_counting=false;hash_counting=false;
 const unsigned nr=reads,nw=writes,ns=syncs,nh=hash_seen;Check(baseline.ok(),"cold reconstruction measured");r.lease.reset();
 std::cout<<"cold reads="<<nr<<" writes="<<nw<<" syncs="<<ns<<" hashes="<<nh<<"\n";
 for(unsigned mode=0;mode<5;++mode){const unsigned count=mode==0?nr:mode==1?nw:mode==2?ns:mode==3?5*nh:nr;
  for(unsigned at=1;at<=count;++at){Write(f,0,original);r=Resume(f,g);ResetIO();hash_seen=0;
   if(mode==0)read_fault=at;if(mode==1){write_fault=at;torn_bytes=f.size/2;}if(mode==2)sync_fault=at;
   if(mode==3){hash_fault=1+(at-1)/nh;hash_target=1+(at-1)%nh;}if(mode==4)corrupt_read=at;
   io_counting=true;const auto failed=db::ResumeNativeManagementControlGraphOnLease(*r.lease,f.budget);io_counting=false;hash_fault=0;hash_active=false;Empty(failed);
   if(mode==3)Check(failed.error==PE::hash_failure&&!writes&&!syncs&&f.Read(0,256)==original,"cold hash backend failures cannot fabricate or install a target");
   ResetIO();r.lease.reset();r=Resume(f,g);Check(db::ResumeNativeManagementControlGraphOnLease(*r.lease,f.budget).ok(),"cold retry repairs each interrupted reconstruction");r.lease.reset();PostState(f,g,b,original);
  }
 }
 ResetIO();
}
void Allocations(unsigned shard,bool cold){
 Fixture f(0);Graph g(f,false,2);Bundle b(g);g.plan.control_bundle=b.root;g.Refresh();auto r=Reserve(f,g);Bytes original=f.Read(0,256);
 if(cold){Check(Install(f,g,b,r).ok(),"persist reconstruction dependencies");r.lease.reset();for(const auto& m:g.after)Write(f,m.header.page_number,Bytes(f.size));Write(f,g.plan.target_checkpoint.page_number,Bytes(f.size));original=f.Read(0,256);r=Resume(f,g);}
 // Optional latency history retains 4096 rows. Stabilize its vector capacity
 // before measuring fault positions; otherwise a measurement-only growth can
 // leave the last replay fault unfired. Keep telemetry enabled and still
 // require its explicit lost-observation counter for any successful fault.
 byte scratch=0;for(unsigned n=0;n<4097;++n){const auto read=f.device.ReadAt(0,&scratch,1);Check(read.ok()&&read.bytes_transferred==1,"warm optional telemetry capacity without changing durable state");}
 const auto install=[&]{return cold?db::ResumeNativeManagementControlGraphOnLease(*r.lease,f.budget):Install(f,g,b,r);};
 const auto restore=[&]{r.lease.reset();Write(f,0,original);r=Resume(f,g);};
 counting=true;allocations=0;const auto baseline=install();counting=false;const auto sites=allocations;Check(baseline.ok(),"allocation measurement");restore();
 for(unsigned long at=shard;at<sites;at+=4){const auto lost=f.device.failed_io_latency_observations();ResetIO();io_counting=true;allocation_budget=at;const auto failed=install();const auto remaining=allocation_budget;allocation_budget=-1;io_counting=false;
  if(failed.ok()){if(f.device.failed_io_latency_observations()!=lost+1)std::cerr<<"successful fault site="<<at<<" sites="<<sites<<" remaining="<<remaining<<" lost delta="<<(f.device.failed_io_latency_observations()-lost)<<"\n";Check(f.device.failed_io_latency_observations()==lost+1,"only recorded optional I/O telemetry loss permits success");PostState(f,g,b,original);restore();}
  else{Empty(failed);if(failed.error!=PE::resource_exhausted||writes)std::cerr<<"allocation site="<<at<<" error="<<unsigned(failed.error)<<" writes="<<writes<<"\n";
   Check(failed.error==PE::resource_exhausted&&!writes,"required allocation failure before first write");Check(f.Read(0,256)==original,"allocation failure preserves every byte");}
 }
 ResetIO();std::cout<<(cold?"cold":"install")<<" allocation sites="<<sites<<" shard="<<shard<<"\n";
}
void Death(){
 Fixture f(0);Graph g(f,false,2);Bundle b(g);g.plan.control_bundle=b.root;g.Refresh();auto r=Reserve(f,g);const auto original=f.Read(0,256);r.lease.reset();
 const unsigned bundle_start=4+g.extent.size(),map_start=bundle_start+b.pages.size(),checkpoint=map_start+g.after.size();
 for(unsigned at:{3u,4u,bundle_start,bundle_start+1,map_start,map_start+1,checkpoint}){
  Write(f,0,original);const pid_t pid=fork();Check(pid>=0,"fork installer process");
  if(pid==0){auto child=Resume(f,g);ResetIO();kill_write=at;io_counting=true;(void)Install(f,g,b,child);_exit(87);}
  int status=0;Check(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"process died at requested durable stage");
  if(at<map_start){for(const auto& m:g.after)Check(f.Read(m.header.page_number)==Bytes(f.size),"no target map before complete durable bundle barrier");Check(f.Read(g.plan.target_checkpoint.page_number)==Bytes(f.size),"no target checkpoint before complete bundle barrier");}
  r=Resume(f,g);const auto interrupted=f.Read(0,256);ResetIO();io_counting=true;const auto cold=db::ResumeNativeManagementControlGraphOnLease(*r.lease,f.budget);io_counting=false;
  if(at<map_start){Empty(cold);Check(!writes&&!syncs&&f.Read(0,256)==interrupted,"incomplete reconstruction input no invented recovery");Check(Install(f,g,b,r).ok(),"original exact caller input repairs incomplete prefix");}
  else Check(cold.ok(),"complete bundle enables cold map and checkpoint reconstruction");
  r.lease.reset();PostState(f,g,b,original);
 }
 ResetIO();
}
}
int main(int argc,char** argv){try{const std::string mode=argc>1?argv[1]:"all";Check(mode=="all"||mode=="profiles"||mode=="invalid"||mode=="faults"||mode=="death"||mode=="cold-faults"||mode=="allocations"||mode=="cold-allocations","known test mode");if(mode=="allocations"||mode=="cold-allocations"){Check(argc==3,"allocation shard argument");const auto shard=std::stoul(argv[2]);Check(shard<4,"allocation shard range");Allocations(shard,mode=="cold-allocations");}if(mode=="all"||mode=="profiles")Profiles();if(mode=="all"||mode=="invalid")Invalid();if(mode=="all"||mode=="faults")Faults();if(mode=="all"||mode=="death")Death();if(mode=="all"||mode=="cold-faults")ColdFaults();std::cout<<"native management control installation checks="<<checks<<"\n";return 0;}catch(const std::exception& e){allocation_budget=-1;hash_fault=0;io_counting=false;std::cerr<<e.what()<<"\n";return 1;}}

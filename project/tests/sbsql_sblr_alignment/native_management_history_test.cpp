// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
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
 const bool v2=p.management_extent.has_value();std::copy_n(v2?"SBPPM002":"SBPPM001",8,b.begin()+128);Num(b,136,2,v2?2:1);Num(b,138,2,v2?896:640);Num(b,140,4,v2?1024:768);
 Put(b,144,p.object_uuid);Put(b,160,p.bootstrap_uuid);Put(b,176,p.timeline_uuid);Put(b,192,p.operation_uuid);Put(b,208,p.intent.initiator_uuid);Put(b,224,p.intent.request_context_uuid);Put(b,240,p.intent.policy_snapshot_uuid);Put(b,256,p.security_snapshot_uuid);
 std::copy(p.intent.normalized_request_sha256.begin(),p.intent.normalized_request_sha256.end(),b.begin()+272);std::copy(p.reservation_state_sha256.begin(),p.reservation_state_sha256.end(),b.begin()+304);
 Num(b,336,8,p.reserved_generation);Num(b,344,8,p.base_checkpoint_generation);Num(b,352,8,p.base_root_set_generation);Num(b,360,8,p.target_root_set_generation);
 Ref(b,368,p.base_checkpoint);Put(b,416,p.base_checkpoint_object_uuid);std::copy(p.base_checkpoint_sha256.begin(),p.base_checkpoint_sha256.end(),b.begin()+432);Ref(b,464,p.target_checkpoint);Put(b,512,p.target_checkpoint_object_uuid);std::copy(p.target_graph_sha256.begin(),p.target_graph_sha256.end(),b.begin()+528);
 if(p.previous_plan)Ref(b,560,*p.previous_plan);Put(b,608,p.previous_plan_object_uuid);std::copy(p.previous_plan_sha256.begin(),p.previous_plan_sha256.end(),b.begin()+624);
 Num(b,656,8,p.catalog_generation);Num(b,664,8,p.configuration_generation);Num(b,672,8,p.security_generation);Num(b,680,2,p.intent.initiator_kind);Num(b,682,2,1);Num(b,684,2,2);
 if(p.management_extent){const auto& r=*p.management_extent;Num(b,720,4,p.generation_guard_flags);Ref(b,736,r.first);Put(b,784,r.object_uuid);Put(b,800,r.operation_uuid);Num(b,816,8,r.revision);Num(b,824,4,r.aggregate_bytes);Num(b,828,4,r.page_count);std::copy(r.aggregate_sha256.begin(),r.aggregate_sha256.end(),b.begin()+832);std::copy(r.first_page_sha256.begin(),r.first_page_sha256.end(),b.begin()+864);}PlanSeal(b);return b;
}
void Failed(const db::NativePublicationPlanImage& r){Check(!r.ok()&&!r.plan&&r.bytes.empty()&&std::all_of(r.sha256.begin(),r.sha256.end(),[](byte v){return !v;}),"no failed plan prefix");}
void Bad(const db::NativePublicationPlan& p){Failed(db::EncodeNativePublicationPlan(p));const auto raw=Oracle(p);Failed(db::DecodeNativePublicationPlan(raw));}
struct Fixture {
 std::filesystem::path path;d::FileDevice device;std::vector<d::NativeFilespaceDevice> devices;u64 size,budget;
 Fixture(unsigned profile){
  char name[]="/tmp/sb-management-history.XXXXXX";Check(mkdtemp(name),"isolated fixture directory");path=std::filesystem::path(name)/"node";
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

using HE=db::NativeManagementHistoryError;using O=db::NativeManagementOperation;using OS=db::NativeManagementState;
using Pages=std::vector<Bytes>;
void Empty(const db::NativeManagementHistory& r){Check(!r.ok()&&!r.selection&&r.entries.empty()&&r.latest.empty()&&r.idempotency.empty()&&!r.verified_image_bytes,"no failed history prefix or indexes");}
void Reset(){io_counting=false;reads=writes=syncs=read_fault=write_fault=sync_fault=kill_write=corrupt_read=0;torn_bytes=0;}
d::NativePageReference Self(const db::NativeCheckpointRoot& cp){const auto& h=cp.header;return {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
Bytes SelectorOracle(const db::NativeCheckpointSelection& s){const auto& h=s.header;Bytes b(h.page_size_bytes);std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,0x30e);Num(b,20,2,1);Num(b,22,2,1);Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);HeaderSeal(b);
 std::copy_n("SBDCP001",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,384);Num(b,140,4,512);Put(b,144,s.object_uuid);Put(b,160,s.bootstrap_uuid);Num(b,176,8,s.selection_generation);Put(b,184,s.publication_uuid);Ref(b,200,s.checkpoint);Put(b,248,s.checkpoint_object_uuid);std::copy(s.checkpoint_sha256.begin(),s.checkpoint_sha256.end(),b.begin()+264);Num(b,296,8,s.checkpoint_generation);Num(b,304,8,s.root_set_generation);Put(b,312,s.timeline_uuid);Num(b,328,8,s.previous_selection_generation);if(s.previous_checkpoint)Ref(b,336,*s.previous_checkpoint);Put(b,384,s.previous_checkpoint_object_uuid);std::copy(s.previous_checkpoint_sha256.begin(),s.previous_checkpoint_sha256.end(),b.begin()+400);const auto sha=Sha(b);std::copy(sha.begin(),sha.end(),b.begin()+432);return b;}
struct Chain {
 Fixture& f;d::FilespacePageZero zero;db::NativeCheckpointRoot genesis,current;Bytes genesis_bytes,current_bytes;std::array<db::NativeCheckpointSelection,2> initial,selected;std::vector<O> records;
 Chain(Fixture& fixture):f(fixture){const auto z=d::ReadFilespacePageZeroFromOpenDevice(f.device);Check(z.ok(),"actual primary");zero=*z.record;
  for(unsigned i=0;i<2;++i){const auto r=std::find_if(zero.roots.begin(),zero.roots.end(),[&](const auto& v){return v.kind==18+i;});Check(r!=zero.roots.end(),"actual selector slot");const auto decoded=db::DecodeNativeCheckpointSelection(f.Read(r->page_number));Check(decoded.ok(),"genesis selector");initial[i]=*decoded.selection;}
  genesis_bytes=f.Read(initial[0].checkpoint.page_number);const auto cp=db::DecodeNativeCheckpointRoot(genesis_bytes);Check(cp.ok(),"genesis checkpoint");genesis=*cp.root;current=genesis;current_bytes=genesis_bytes;selected=initial;}
 void Write(u64 page,const Bytes& b){const auto r=f.device.WriteAt(page*f.size,b.data(),b.size());Check(r.ok()&&r.bytes_transferred==b.size(),"independent isolated selected-history fixture write");}
 void Select(const Uuid& attempt){for(unsigned i=0;i<2;++i){auto& s=selected[i];s.previous_selection_generation=s.selection_generation++;s.previous_checkpoint=s.checkpoint;s.previous_checkpoint_object_uuid=s.checkpoint_object_uuid;s.previous_checkpoint_sha256=s.checkpoint_sha256;s.publication_uuid=attempt;s.checkpoint=Self(current);s.checkpoint_object_uuid=current.object_uuid;s.checkpoint_sha256=Sha(current_bytes);s.checkpoint_generation=current.checkpoint_generation;s.root_set_generation=current.root_set_generation;const auto b=SelectorOracle(s);const auto encoded=db::EncodeNativeCheckpointSelection(s);Check(encoded.ok()&&encoded.bytes==b,"independent selected bytes");Write(s.header.page_number,b);}Check(f.device.Sync().ok(),"fixture sync");}
 std::vector<O> Baseline(){auto a=records::Example(1);a.uuid=Id(333);a.bootstrap_uuid=zero.page_uuid;a.security_snapshot_uuid={};a.generation_guards={};auto b=a;b.uuid=Id(444);b.request_context_uuid=Id(88);b.idempotency_key="other";
  auto authorized=a;authorized.state=OS::authorized;authorized.revision=2;authorized.updated_at=Id(2002);authorized.security_snapshot_uuid=Id(11);authorized.generation_guards={0,1,2,std::nullopt};
  auto planned=authorized;planned.state=OS::planned;planned.revision=3;planned.updated_at=Id(2003);planned.phase_uuid=Id(12);planned.resource_plan_uuid=Id(13);planned.lock_plan_uuid=Id(14);
  auto terminal=b;terminal.state=OS::failed_terminal;terminal.revision=2;terminal.updated_at=Id(2010);terminal.terminal_at=Id(2009);terminal.result_uuid=Id(15);terminal.diagnostic_uuid=Id(17);
  auto running=planned;running.state=OS::running;running.revision=4;running.updated_at=Id(2004);auto step=records::Step();step.operation_uuid=a.uuid;running.steps={step};
  return {a,b,authorized,planned,planned,terminal,running};}
 void Build(std::vector<O> desired,unsigned mutation=0){records=std::move(desired);current=genesis;current_bytes=genesis_bytes;selected=initial;std::optional<db::NativePublicationPlan> previous;std::array<byte,32> previous_hash{};
  for(unsigned i=0;i<records.size();++i){const auto& o=records[i];const u64 page=40+4*i,generation=i+2;std::vector<d::NativeCommonPageHeader> headers{{static_cast<u32>(f.size),0x500,Id(1),Id(2),Id(7002+3*i),page+2,generation,0,zero.bootstrap.page_size_profile_uuid}};
   const auto extent=db::EncodeNativeManagementExtent(o,Id(5000+i),headers,f.budget);Check(extent.ok(),"record fixture structurally valid");auto expected=records::ExtentOracle(o,headers);for(auto& p:expected)Put(p,144,Id(5000+i));auto root=*extent.root;records::Rechain(expected,root);Check(expected==extent.pages&&root.first_page_sha256==extent.root->first_page_sha256,"independent extent bytes");
   db::NativePublicationPlan p;p.header={static_cast<u32>(f.size),0x500,Id(1),Id(2),Id(7000+3*i),page,generation,0,zero.bootstrap.page_size_profile_uuid};p.object_uuid=Id(302);p.bootstrap_uuid=zero.page_uuid;p.timeline_uuid=current.timeline_uuid;p.operation_uuid=Id(800+i);if(mutation==1&&i==3)p.operation_uuid=Id(800);p.intent={o.initiator_uuid,o.request_context_uuid,o.policy_snapshot_uuid,o.normalized_request_sha256,o.initiator_kind};p.security_snapshot_uuid=o.security_snapshot_uuid;p.reservation_state_sha256.fill(9);p.base_checkpoint=Self(current);p.base_checkpoint_object_uuid=p.target_checkpoint_object_uuid=current.object_uuid;p.base_checkpoint_sha256=Sha(current_bytes);p.base_checkpoint_generation=current.checkpoint_generation;p.base_root_set_generation=current.root_set_generation;p.reserved_generation=generation;p.target_root_set_generation=generation;p.target_checkpoint={Id(2),page+1,generation,zero.bootstrap.page_size_profile_uuid};p.management_extent=extent.root;p.generation_guard_flags=0;for(unsigned n=0;n<3;++n)if(o.generation_guards[n])p.generation_guard_flags|=1u<<n;p.catalog_generation=o.generation_guards[0].value_or(0);p.configuration_generation=o.generation_guards[1].value_or(0);p.security_generation=o.generation_guards[2].value_or(0);
   if(previous){p.previous_plan=d::NativePageReference{Id(2),previous->header.page_number,previous->header.page_generation,previous->header.page_size_profile_uuid};p.previous_plan_object_uuid=previous->object_uuid;p.previous_plan_sha256=previous_hash;}
   if(mutation==2&&i==3){p.previous_plan.reset();p.previous_plan_object_uuid={};p.previous_plan_sha256={};}
   if(mutation==3&&i==3)--p.base_checkpoint_generation;
   if(mutation==4&&i==3)p.bootstrap_uuid=Id(999);
   auto cp=current;cp.header.page_uuid=Id(7001+3*i);const auto target=Finish(p,cp);const auto plan=Oracle(p);Write(page,plan);Write(page+1,target);Write(page+2,expected.front());previous=p;previous_hash=Sha(plan);current=*db::DecodeNativeCheckpointRoot(target).root;current_bytes=target;Select(p.operation_uuid);
  }}
 void Transaction(bool drop_head=false){auto next=current;next.header.page_uuid=Id(7900);next.header.page_number=100;next.header.page_generation++;next.checkpoint_generation++;next.root_set_generation++;next.creator_operation_uuid={};next.creator_transaction_uuid=genesis.creator_transaction_uuid;next.creator_local_transaction_id=genesis.creator_local_transaction_id;next.predecessor=Self(current);next.predecessor_sha256=Sha(current_bytes);if(drop_head)next.roots.pop_back();const auto bytes=db::EncodeNativeCheckpointRoot(next);Check(bytes.ok(),"transaction checkpoint fixture");current=next;current_bytes=bytes.bytes;Write(100,current_bytes);Select(Id(990));}
 auto Read(u64 budget=0){return db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),budget?budget:f.budget);}
 void Verify(const db::NativeManagementHistory& result){Check(result.ok()&&result.entries.size()==records.size()&&result.latest.size()==2&&result.idempotency.size()==2,"complete chronological interleaved history");for(unsigned i=0;i<records.size();++i)Check(result.entries[i].record==records[i]&&result.entries[i].extent_pages.size()==1,"exact record and verified page binding");Check(result.latest.at(Id(333))==6&&result.latest.at(Id(444))==5,"latest binary UUID indexes");}
};
void Test(unsigned profile){Fixture f(profile);Chain chain(f);const auto empty=chain.Read();Check(empty.ok()&&empty.entries.empty()&&empty.latest.empty(),"actual genesis empty history");auto baseline=chain.Baseline();chain.Build(baseline);const auto read=chain.Read();chain.Verify(read);const auto needed=read.verified_image_bytes;chain.Verify(chain.Read(needed));Empty(chain.Read(needed-1));chain.Transaction();chain.Verify(chain.Read());
 // The history reader must not implicitly disable allocation/creator admission.
 Check(!db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget).ok(),"physical history alone is not full selected allocation authority");
 Check(f.device.Close().ok()&&f.device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"read-only reopen");chain.Verify(chain.Read());Check(f.device.Close().ok(),"close before process read");const auto child=fork();Check(child>=0,"fork reader");if(!child){d::FileDevice own;if(!own.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok())_exit(80);std::vector<d::NativeFilespaceDevice> files{{Id(2),chain.zero.bootstrap.page_size_profile_uuid,&own}};const auto r=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),files,Id(2),f.budget);_exit(r.ok()&&r.entries.size()==baseline.size()&&r.entries.back().record==baseline.back()?0:81);}int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"independent process full history");Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"parent reopen");
 if(profile==0){
  chain.Build(baseline);Reset();io_counting=true;hash_counting=true;hash_seen=0;const auto measured=chain.Read();hash_counting=false;io_counting=false;chain.Verify(measured);const auto read_sites=reads,hash_sites=hash_seen;Check(!writes&&!syncs,"history never mutates or syncs");
  if(allocation_shard>=0){counting=true;allocations=0;Check(chain.Read().ok(),"allocation baseline");counting=false;const auto sites=allocations;
   for(unsigned long at=allocation_shard;at<sites;at+=4){const auto lost=f.device.failed_io_latency_observations();allocation_budget=at;const auto r=chain.Read();allocation_budget=-1;if(r.ok()){Check(f.device.failed_io_latency_observations()==lost+1,"only recorded I/O observation loss permits success");chain.Verify(r);}else{Check(r.error==HE::resource_exhausted,"all required allocations classified");Empty(r);}}
   std::cout<<"history allocations="<<sites<<" shard="<<allocation_shard<<"\n";return;}
  for(unsigned at=1;at<=read_sites;++at){Reset();read_fault=at;io_counting=true;const auto r=chain.Read();io_counting=false;read_fault=0;Check(r.error==HE::io_failure,"every physical read failure");Empty(r);}
  for(unsigned at=1;at<=read_sites;++at){Reset();corrupt_read=at;io_counting=true;const auto r=chain.Read();io_counting=false;corrupt_read=0;Empty(r);}
  for(const auto& entry:measured.entries){Check(db::BindNativePublicationPlanToManagementRecord(entry.plan,entry.record)==E::none,"complete record binding");auto altered=entry.record;altered.updated_at=Id(2200);Check(db::BindNativePublicationPlanToManagementRecord(entry.plan,altered)!=E::none,"full record commitment includes fields outside plan provenance");altered=entry.record;altered.request_context_uuid=Id(99);Check(db::BindNativePublicationPlanToManagementRecord(entry.plan,altered)!=E::none,"record request provenance binding");}
  Empty(db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),0));
  Empty(db::ReadNativeManagementHistoryFromOpenDevices(Id(1),{},Id(2),f.budget));
  Empty(db::ReadNativeManagementHistoryFromOpenDevices(Id(99),f.devices,Id(2),f.budget));
  Empty(db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(99),f.budget));
  auto duplicate=f.devices;duplicate.push_back(duplicate.front());Empty(db::ReadNativeManagementHistoryFromOpenDevices(Id(1),duplicate,Id(2),f.budget));
  for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hash_sites;++at){hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto r=chain.Read();const bool consumed=!hash_fault;hash_fault=0;Check(consumed&&r.error==HE::hash_failure,"all history hashes preserve operational failure");Empty(r);}
  for(unsigned fault=0;fault<7;++fault){auto bad=baseline;HE expected=HE::transition_failure;switch(fault){case 0:bad[0]=bad[2];bad[0].revision=1;break;case 1:bad[2].revision=4;break;case 2:bad.back().idempotency_key="rewritten";break;case 3:bad[1].idempotency_key=bad[0].idempotency_key;bad[5].idempotency_key=bad[0].idempotency_key;expected=HE::idempotency_conflict;break;case 4:bad[1].request_context_uuid=bad[0].request_context_uuid;bad[5].request_context_uuid=bad[0].request_context_uuid;expected=HE::idempotency_conflict;break;case 5:{auto next=bad[1];next.revision=3;next.updated_at=Id(2020);bad.push_back(next);break;}case 6:bad.back().steps[0].uuid=Id(444);expected=HE::history_mismatch;break;}chain.Build(bad);const auto r=chain.Read();Check(r.error==expected,"resealed semantically inconsistent history rejected");Empty(r);}
  for(unsigned mutation=1;mutation<=4;++mutation){chain.Build(baseline,mutation);const auto r=chain.Read();Check(r.error==HE::history_mismatch,"actual plan checkpoint linkage mismatch");Empty(r);}
  for(unsigned origin=0;origin<2;++origin){auto first=origin?baseline[0]:baseline[2];if(origin)first.revision=2;chain.Build({first});const auto r=chain.Read();Check(r.error==HE::transition_failure,"history origin independently requires created revision one");Empty(r);}
  chain.Build(baseline);chain.Transaction(true);Check(chain.Read().error==HE::history_mismatch,"transaction cannot discard management history");
  chain.Build(baseline);auto s=chain.selected[0];s.previous_checkpoint_sha256[0]^=1;for(unsigned i=0;i<2;++i){s.header=chain.selected[i].header;chain.Write(s.header.page_number,SelectorOracle(s));}Check(f.device.Sync().ok(),"broken selector linkage fixture");Check(chain.Read().error==HE::history_mismatch,"selector predecessor binds actual checkpoint");
  chain.Build(baseline);auto broken=f.Read(42);broken[500]^=1;chain.Write(42,broken);Check(f.device.Sync().ok(),"damaged old extent");Empty(chain.Read());
  chain.Build(baseline);auto redirected=f.Read(40);redirected[304]^=1;PlanSeal(redirected);chain.Write(40,redirected);Check(f.device.Sync().ok(),"resealed old plan without checkpoint endorsement");Empty(chain.Read());
  chain.Build(baseline);chain.Verify(chain.Read());
  std::cout<<"history reads="<<read_sites<<" hashes="<<hash_sites<<" allowance="<<needed<<"\n";
 }
}
}
int main(int argc,char** argv){try{if(argc==3&&std::string_view(argv[1])=="--allocations"){allocation_shard=std::stoi(argv[2]);Check(allocation_shard>=0&&allocation_shard<4,"allocation shard");}else Check(argc==1,"arguments");for(unsigned p=0;p<(allocation_shard<0?5u:1u);++p)Test(p);std::cout<<"PASS native management history checks="<<checks<<" physical_history_only=true not_allocation_authorization_effects_or_SQL_E2E=true\n";return 0;}catch(const std::exception& e){allocation_budget=-1;hash_fault=0;io_counting=false;std::cerr<<"FAIL history checks="<<checks<<" "<<e.what()<<'\n';return 1;}}

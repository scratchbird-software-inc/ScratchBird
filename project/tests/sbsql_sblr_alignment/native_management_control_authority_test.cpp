// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_control_bundle.hpp"
#include "native_management_control_authority.hpp"
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
#include <type_traits>
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
 const bool v2=p.management_extent.has_value(),v3=p.control_bundle.has_value(),v4=p.base_selection_generation.has_value();std::copy_n(v4?"SBPPM004":v3?"SBPPM003":v2?"SBPPM002":"SBPPM001",8,b.begin()+128);Num(b,136,2,v4?4:v3?3:v2?2:1);Num(b,138,2,v3?1024:v2?896:640);Num(b,140,4,v3?1152:v2?1024:768);
 Put(b,144,p.object_uuid);Put(b,160,p.bootstrap_uuid);Put(b,176,p.timeline_uuid);Put(b,192,p.operation_uuid);Put(b,208,p.intent.initiator_uuid);Put(b,224,p.intent.request_context_uuid);Put(b,240,p.intent.policy_snapshot_uuid);Put(b,256,p.security_snapshot_uuid);
 std::copy(p.intent.normalized_request_sha256.begin(),p.intent.normalized_request_sha256.end(),b.begin()+272);std::copy(p.reservation_state_sha256.begin(),p.reservation_state_sha256.end(),b.begin()+304);
 Num(b,336,8,p.reserved_generation);Num(b,344,8,p.base_checkpoint_generation);Num(b,352,8,p.base_root_set_generation);Num(b,360,8,p.target_root_set_generation);
 Ref(b,368,p.base_checkpoint);Put(b,416,p.base_checkpoint_object_uuid);std::copy(p.base_checkpoint_sha256.begin(),p.base_checkpoint_sha256.end(),b.begin()+432);Ref(b,464,p.target_checkpoint);Put(b,512,p.target_checkpoint_object_uuid);std::copy(p.target_graph_sha256.begin(),p.target_graph_sha256.end(),b.begin()+528);
 if(p.previous_plan)Ref(b,560,*p.previous_plan);Put(b,608,p.previous_plan_object_uuid);std::copy(p.previous_plan_sha256.begin(),p.previous_plan_sha256.end(),b.begin()+624);
 Num(b,656,8,p.catalog_generation);Num(b,664,8,p.configuration_generation);Num(b,672,8,p.security_generation);Num(b,680,2,p.intent.initiator_kind);Num(b,682,2,1);Num(b,684,2,2);
 if(p.management_extent){const auto& r=*p.management_extent;Num(b,720,4,p.generation_guard_flags);Ref(b,736,r.first);Put(b,784,r.object_uuid);Put(b,800,r.operation_uuid);Num(b,816,8,r.revision);Num(b,824,4,r.aggregate_bytes);Num(b,828,4,r.page_count);std::copy(r.aggregate_sha256.begin(),r.aggregate_sha256.end(),b.begin()+832);std::copy(r.first_page_sha256.begin(),r.first_page_sha256.end(),b.begin()+864);}
 if(p.control_bundle){const auto& r=*p.control_bundle;Ref(b,896,r.first);Put(b,944,r.object_uuid);Num(b,960,8,r.map_count);Num(b,968,8,r.page_count);std::copy(r.aggregate_sha256.begin(),r.aggregate_sha256.end(),b.begin()+976);std::copy(r.first_page_sha256.begin(),r.first_page_sha256.end(),b.begin()+1008);}if(v4)Num(b,1040,8,*p.base_selection_generation);PlanSeal(b);return b;
}
void Failed(const db::NativePublicationPlanImage& r){Check(!r.ok()&&!r.plan&&r.bytes.empty()&&std::all_of(r.sha256.begin(),r.sha256.end(),[](byte v){return !v;}),"no failed plan prefix");}
void Bad(const db::NativePublicationPlan& p){Failed(db::EncodeNativePublicationPlan(p));const auto raw=Oracle(p);Failed(db::DecodeNativePublicationPlan(raw));}
struct Fixture {
 std::filesystem::path path;d::FileDevice device;std::vector<d::NativeFilespaceDevice> devices;u64 size,budget;
 Fixture(unsigned profile){
  char name[]="/tmp/sb-management-control-authority.XXXXXX";Check(mkdtemp(name),"isolated fixture directory");path=std::filesystem::path(name)/"node";
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
 Graph(Fixture& f,bool retained_operation=false,unsigned extent_count=1,const Graph* previous=nullptr,u64 generation_override=0):fixture(f){
  const auto z=d::ReadFilespacePageZeroFromOpenDevice(f.device);Check(z.ok(),"actual page zero");zero=*z.record;
  const auto ref=std::find_if(zero.roots.begin(),zero.roots.end(),[](const auto& r){return r.kind==9;});const auto cp=db::ReadNativeCheckpointRootFromOpenDevice(f.device,Id(1),*ref);Check(cp.ok(),"actual base checkpoint");base=*cp.root;
  const auto allocation=page::ReadNativeAllocationChainFromOpenDevice(f.device,{Id(1),Id(2),zero.bootstrap.page_size_profile_uuid},f.budget);Check(allocation.ok(),"actual base allocation");for(const auto& image:allocation.pages)before.push_back(*image.map);
  if(previous){base=*db::DecodeNativeCheckpointRoot(previous->target_bytes).root;before=previous->after;}
  const unsigned offset=previous?60:0;const u64 generation=generation_override?generation_override:base.checkpoint_generation+1;
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
 Bundle(Graph& g):graph(g){const unsigned offset=g.plan.header.page_number==100?0:60;const u64 generation=g.plan.reserved_generation;const u64 size=g.fixture.size,count=(g.after.size()*size+size-385)/(size-384);
  for(unsigned n=0;n<count;++n){d::NativeCommonPageHeader h{u32(size),0x500,Id(1),Id(2),Id(9000+offset+n),180+(offset?40:0)+n,generation,0,g.zero.bootstrap.page_size_profile_uuid};headers.push_back(h);auto& m=Cover(g.after,h.page_number);Check(m.states[h.page_number-m.first_page]==State::free,"bundle slot free");m.states[h.page_number-m.first_page]=State::allocated;page::NativeAllocationRecord r;r.page_number=h.page_number;r.allocation_uuid=Id(8000+offset+n);r.page_uuid=h.page_uuid;r.owner_uuid=Id(900+offset);r.page_generation=generation;r.page_type=0x500;r.creator_operation_uuid=g.plan.operation_uuid;m.records.push_back(r);}
  for(auto& m:g.after)std::sort(m.records.begin(),m.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});g.Refresh();
  root.first=Self(headers.front());root.object_uuid=Id(900+offset);root.operation_uuid=g.plan.operation_uuid;root.map_count=g.after.size();root.page_count=headers.size();Oracle();
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
void Empty(const db::NativeManagementControlBundleRead& r){Check(!r.ok()&&r.allocation_images.empty()&&r.page_headers.empty()&&!r.total_pages,"no failed bundle result prefix");}
void Empty(const db::NativeManagementControlBundleImage& r){Check(!r.ok()&&!r.root&&r.pages.empty(),"no failed bundle image prefix");}
Bytes SelectorOracle(const db::NativeCheckpointSelection& s){const auto& h=s.header;Bytes b(h.page_size_bytes);std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,0x30e);Num(b,20,2,1);Num(b,22,2,1);Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);HeaderSeal(b);
 std::copy_n("SBDCP001",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,384);Num(b,140,4,512);Put(b,144,s.object_uuid);Put(b,160,s.bootstrap_uuid);Num(b,176,8,s.selection_generation);Put(b,184,s.publication_uuid);Ref(b,200,s.checkpoint);Put(b,248,s.checkpoint_object_uuid);std::copy(s.checkpoint_sha256.begin(),s.checkpoint_sha256.end(),b.begin()+264);Num(b,296,8,s.checkpoint_generation);Num(b,304,8,s.root_set_generation);Put(b,312,s.timeline_uuid);Num(b,328,8,s.previous_selection_generation);if(s.previous_checkpoint)Ref(b,336,*s.previous_checkpoint);Put(b,384,s.previous_checkpoint_object_uuid);std::copy(s.previous_checkpoint_sha256.begin(),s.previous_checkpoint_sha256.end(),b.begin()+400);const auto sha=Sha(b);std::copy(sha.begin(),sha.end(),b.begin()+432);return b;}


using AE=db::NativeManagementControlAuthorityError;
void Empty(const db::NativeManagementControlGraph& r){Check(!r.ok()&&!r.anchor&&r.publications.empty()&&r.allocations.empty()&&!r.verified_image_bytes,"no failed graph proof prefix");}
void Empty(const db::NativeManagementControlAuthority& r){Empty(static_cast<const db::NativeManagementControlGraph&>(r));Check(!r.selection,"no failed selected creator prefix");}
void Empty(const db::NativeManagementGraphHistory& r){Check(!r.ok()&&!r.anchor&&r.entries.empty()&&r.latest.empty()&&r.idempotency.empty()&&!r.verified_image_bytes,"no failed graph history prefix");}
void Reset(){reads=writes=syncs=read_fault=write_fault=sync_fault=corrupt_read=kill_write=0;torn_bytes=0;hash_fault=0;hash_active=false;}
void Write(Fixture& f,u64 n,const Bytes& b){const auto r=f.device.WriteAt(n*f.size,b.data(),b.size());Check(r.ok()&&r.bytes_transferred==b.size(),"isolated fixture write");}
void SelectFixture(Fixture& f,const Graph& g){
 // Independent test-only selector fixture, not a production publisher.
 for(unsigned i=0;i<2;++i){const auto ref=std::find_if(g.zero.roots.begin(),g.zero.roots.end(),[&](const auto& r){return r.kind==18+i;});const auto old=db::DecodeNativeCheckpointSelection(f.Read(ref->page_number));Check(old.ok(),"actual selector fixture header");auto s=*old.selection;
  s.selection_generation=g.plan.base_selection_generation?*g.plan.base_selection_generation+1:g.plan.reserved_generation;s.previous_selection_generation=s.selection_generation-1;s.previous_checkpoint=g.plan.base_checkpoint;s.previous_checkpoint_object_uuid=g.plan.base_checkpoint_object_uuid;s.previous_checkpoint_sha256=g.plan.base_checkpoint_sha256;
  s.publication_uuid=g.plan.operation_uuid;s.checkpoint=g.plan.target_checkpoint;s.checkpoint_object_uuid=g.plan.target_checkpoint_object_uuid;s.checkpoint_sha256=Sha(g.target_bytes);s.checkpoint_generation=g.plan.reserved_generation;s.root_set_generation=g.plan.target_root_set_generation;
  const auto encoded=db::EncodeNativeCheckpointSelection(s);const auto raw=SelectorOracle(s);Check(encoded.ok()&&encoded.bytes==raw,"independent selector oracle");Write(f,ref->page_number,raw);}
 Check(f.device.Sync().ok(),"isolated selector fixture barrier");
}
void Install(Fixture& f,Graph& g,const Bundle& b){
 const auto inspect=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);Check(inspect.ok(),"actual bound coordinator");
 auto lease=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*inspect.snapshot,g.plan.operation_uuid,f.budget,&g.plan.intent);Check(lease.ok(),"actual next generation lease");
 g.plan.reservation_state_sha256=lease.lease->snapshot().state_sha256;g.Refresh();
 const auto installed=db::InstallNativeManagementControlGraphOnLease(*lease.lease,g.plan,g.target_bytes,g.extent,b.pages,f.budget);if(!installed.ok())std::cerr<<"install error="<<unsigned(installed.error)<<"\n";Check(installed.ok(),"actual complete unselected graph");
}
void RawGraph(Fixture& f,const Graph& g,Bundle& b){Write(f,g.base.header.page_number,g.base_bytes);for(unsigned i=0;i<g.before_bytes.size();++i)Write(f,g.before[i].header.page_number,g.before_bytes[i]);
 Write(f,g.plan.header.page_number,g.plan_bytes);Write(f,g.plan.target_checkpoint.page_number,g.target_bytes);for(unsigned i=0;i<g.extent.size();++i)Write(f,g.plan.management_extent->first.page_number+i,g.extent[i]);for(unsigned i=0;i<g.after_bytes.size();++i)Write(f,g.after[i].header.page_number,g.after_bytes[i]);b.Install();SelectFixture(f,g);}
auto Read(Fixture& f,u64 budget=0){return db::ReadNativeManagementControlAuthorityFromOpenDevices(Id(1),f.devices,Id(2),budget?budget:f.budget);}
auto Anchor(const db::NativeCheckpointRoot& cp,const Bytes& bytes){return db::NativeManagementCheckpointAnchor{Self(cp.header),cp.object_uuid,Sha(bytes),cp.checkpoint_generation,cp.root_set_generation,cp.timeline_uuid};}
auto Anchor(const Graph& g){return Anchor(*db::DecodeNativeCheckpointRoot(g.target_bytes).root,g.target_bytes);}
auto GraphRead(Fixture& f,const db::NativeManagementCheckpointAnchor& anchor,u64 budget=0){return db::ReadNativeManagementControlGraphFromOpenDevices(Id(1),f.devices,Id(2),anchor,budget?budget:f.budget);}
auto GraphHistory(Fixture& f,const db::NativeManagementCheckpointAnchor& anchor,u64 budget=0){return db::ReadNativeManagementGraphHistoryFromOpenDevices(Id(1),f.devices,Id(2),anchor,budget?budget:f.budget);}
auto Inventory(Fixture& f,const Graph& g){const auto& p=g.plan;return db::VerifyNativeCheckpointInventoryFromOpenDevices(Id(1),f.devices,{9,0x300,p.target_checkpoint.filespace_uuid,p.target_checkpoint.page_number,p.target_checkpoint.page_generation,p.target_checkpoint.page_size_profile_uuid,p.target_checkpoint_object_uuid},f.budget);}
auto Bound(Fixture& f){return db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),f.devices,Id(2),f.budget);}
void Good(Fixture& f,const Graph& g,std::size_t count){
 const auto before=f.Read(0,256);Reset();io_counting=true;const auto proof=Read(f);const auto inventory=Inventory(f,g);const auto bound=Bound(f);io_counting=false;
 if(!proof.ok()||!inventory.ok()||!bound.ok())std::cerr<<"reader errors="<<unsigned(proof.error)<<","<<unsigned(inventory.error)<<","<<unsigned(bound.error)<<"\n";
 Check(proof.ok()&&proof.publications.size()==count,"actual selected complete creator proof");Check(inventory.ok()&&bound.ok(),"inventory and selected allocation consumers compose actual proof");
 const auto& e=*g.plan.management_extent;const auto& bundle=*g.plan.control_bundle;
 const u64 extent_work=u64(e.page_count)*f.size+4*u64(e.aggregate_bytes)+2*f.size;
 const u64 bundle_work=bundle.page_count*f.size+4*bundle.map_count*f.size+2*f.size;
 const u64 expected_work=(13+15*count+(4*count+1)*g.after.size())*f.size+2*count*(extent_work+bundle_work);
 Check(proof.verified_image_bytes==expected_work,"independent cumulative image-work accounting for one-page inventories and equal-size revisions");
 Check(!writes&&!syncs&&before==f.Read(0,256),"all creator readers leave full node unchanged");
 Check(db::MatchesNativeManagementPublishedCheckpoint(proof,*inventory.checkpoint,Sha(g.target_bytes)),"exact selected checkpoint identity");
 auto changed_digest=Sha(g.target_bytes);changed_digest.front()^=1;
 Check(!db::MatchesNativeManagementPublishedCheckpoint(proof,*inventory.checkpoint,changed_digest),"publication binds the complete stored checkpoint hash");
 for(unsigned field=0;field<5;++field){auto changed=*inventory.checkpoint;
  if(field==0)changed.header.page_number++;
  if(field==1)changed.object_uuid=Id(9999);
  if(field==2)changed.creator_operation_uuid=Id(9999);
  if(field==3)changed.creator_transaction_uuid=Id(9999);
  if(field==4)changed.creator_local_transaction_id=1;
  Check(!db::MatchesNativeManagementPublishedCheckpoint(proof,changed,Sha(g.target_bytes)),"publication requires exact reference object and exclusive attempt creator");
 }
 for(const auto& map:g.after){Check(db::MatchesNativeManagementControlMap(proof,map),"exact original map creator");for(const auto& r:map.records)if(!r.creator_operation_uuid.is_nil()){
  Check(db::MatchesNativeManagementControlAllocation(proof,Id(2),r,State::allocated),"exact original retained allocation");auto changed=r;changed.allocation_uuid=Id(9999);Check(!db::MatchesNativeManagementControlAllocation(proof,Id(2),changed,State::allocated),"UUID membership is not full record proof");Check(!db::MatchesNativeManagementControlAllocation(proof,Id(2),r,State::quarantined),"nonallocated retained control is not admitted");}}
 const auto exact=Read(f,proof.verified_image_bytes);Check(exact.ok()&&exact.verified_image_bytes==proof.verified_image_bytes,"exact charged proof budget");Empty(Read(f,proof.verified_image_bytes-1));
}
struct TransactionTail {
 Fixture& f;const Graph& previous;db::NativeCheckpointRoot cp;Maps maps;Pages images;Bytes bytes;
 TransactionTail(Fixture& fixture,const Graph& g):f(fixture),previous(g){cp=*db::DecodeNativeCheckpointRoot(g.target_bytes).root;maps=g.after;const u64 generation=cp.checkpoint_generation+1;
  cp.predecessor=Self(cp.header);cp.predecessor_sha256=Sha(g.target_bytes);cp.header.page_number=230;cp.header.page_uuid=Id(11000);cp.header.page_generation=generation;cp.checkpoint_generation=cp.root_set_generation=generation;cp.creator_operation_uuid={};cp.creator_transaction_uuid=g.base.creator_transaction_uuid.is_nil()?Id(5):g.base.creator_transaction_uuid;cp.creator_local_transaction_id=1;
  for(unsigned i=0;i<maps.size();++i){auto& m=maps[i];m.header.page_number=210+i;m.header.page_uuid=Id(11100+i);m.header.page_generation=m.map_generation=generation;m.creator_operation_uuid={};m.creator_transaction_uuid=cp.creator_transaction_uuid;m.creator_local_transaction_id=1;}
  unsigned id=13000;const auto add=[&](const d::NativeCommonPageHeader& h,const Uuid& owner){auto& m=Cover(maps,h.page_number);Check(m.states[h.page_number-m.first_page]==State::free,"new transaction fixture control free");m.states[h.page_number-m.first_page]=State::allocated;page::NativeAllocationRecord r;r.page_number=h.page_number;r.page_uuid=h.page_uuid;r.page_generation=h.page_generation;r.page_type=h.page_type;r.owner_uuid=owner;r.allocation_uuid=Id(id++);r.creator_transaction_uuid=cp.creator_transaction_uuid;r.creator_local_transaction_id=1;m.records.push_back(r);};
  add(cp.header,cp.object_uuid);for(const auto& m:maps)add(m.header,m.object_uuid);for(auto& m:maps)std::sort(m.records.begin(),m.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});Refresh();
 }
 void Refresh(){images=EncodeMaps(maps);auto& root=*std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==4;});root.page=Self(maps.front().header);root.object_uuid=maps.front().object_uuid;root.sha256=Sha(images.front());const auto encoded=db::EncodeNativeCheckpointRoot(cp);Check(encoded.ok(),"transaction-created checkpoint fixture with retained management head");bytes=encoded.bytes;}
 void Install(){for(unsigned i=0;i<maps.size();++i)Write(f,maps[i].header.page_number,images[i]);Write(f,cp.header.page_number,bytes);
  for(unsigned i=0;i<2;++i){const auto slot=std::find_if(previous.zero.roots.begin(),previous.zero.roots.end(),[&](const auto& r){return r.kind==18+i;});auto s=*db::DecodeNativeCheckpointSelection(f.Read(slot->page_number)).selection;s.selection_generation=cp.checkpoint_generation;s.previous_selection_generation=s.selection_generation-1;s.previous_checkpoint=cp.predecessor;s.previous_checkpoint_object_uuid=cp.object_uuid;s.previous_checkpoint_sha256=cp.predecessor_sha256;s.publication_uuid=Id(15000);s.checkpoint=Self(cp.header);s.checkpoint_object_uuid=cp.object_uuid;s.checkpoint_sha256=Sha(bytes);s.checkpoint_generation=cp.checkpoint_generation;s.root_set_generation=cp.root_set_generation;Write(f,slot->page_number,SelectorOracle(s));}
  Check(f.device.Sync().ok(),"isolated transaction-tail fixture persisted");
 }
};
void Retained(){
 Fixture f(0);f.budget*=4;Graph g(f);Bundle b(g);g.plan.control_bundle=b.root;g.Refresh();Install(f,g,b);SelectFixture(f,g);TransactionTail tail(f,g);tail.Install();
 const auto actual=f.Read(0,256);Reset();io_counting=true;const auto proof=Read(f);const auto bound=Bound(f);io_counting=false;Check(proof.ok()&&proof.publications.size()==1&&bound.ok()&&bound.checkpoint_inventory.checkpoint->creator_operation_uuid.is_nil(),"transaction-created selected tail retains operation allocation proofs");Check(!writes&&!syncs&&f.Read(0,256)==actual,"mixed creator read no mutation");
 const auto original=tail.maps;
 for(unsigned mode=0;mode<3;++mode){tail.maps=original;auto& m=Cover(tail.maps,g.plan.header.page_number);
  if(mode==0){m.states[g.plan.header.page_number-m.first_page]=State::free;m.records.erase(std::remove_if(m.records.begin(),m.records.end(),[&](const auto& r){return r.page_number==g.plan.header.page_number;}),m.records.end());}
  if(mode==1)Find(tail.maps,g.plan.header.page_number).allocation_uuid=Id(15500);
  if(mode==2)m.states[g.plan.header.page_number-m.first_page]=State::quarantined;
  tail.Refresh();tail.Install();Check(db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),f.budget).ok(),"old physical operation bytes still readable after invalid current allocation mutation");
  const auto before=f.Read(0,256);Reset();io_counting=true;const auto rejected=Read(f);const auto rejected_bound=Bound(f);io_counting=false;Empty(rejected);Check(!rejected_bound.ok()&&!writes&&!syncs&&f.Read(0,256)==before,"current allocation must retain each exact original management control");
 }
 Write(f,0,actual);Check(Read(f).ok()&&Bound(f).ok(),"restored actual mixed creator tail");
 // The final transaction maps are intact; the historical operation's own
 // separately stored maps must still agree with its immutable bundle.
 auto changed=*page::DecodeNativeAllocationMap(g.after_bytes.front()).map;
 changed.records.front().allocation_uuid=Id(15501);const auto encoded=page::EncodeNativeAllocationMap(changed);
 Check(encoded.ok(),"canonical altered historical operation map");Write(f,changed.header.page_number,encoded.bytes);
 Check(db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),f.budget).ok(),"historical bundle integrity does not inspect separately stored maps");
 const auto tampered=f.Read(0,256);Reset();io_counting=true;const auto rejected=Read(f);const auto rejected_bound=Bound(f);io_counting=false;
 Empty(rejected);Check(!rejected_bound.ok()&&!writes&&!syncs&&f.Read(0,256)==tampered,"retained historical target maps must be read and bound even with intact current maps");
 Write(f,0,actual);Check(Read(f).ok()&&Bound(f).ok(),"restored historical and current map authority");
}
int Call(Fixture& f,const Graph& g,unsigned route,const db::NativeManagementCheckpointAnchor& anchor){
 if(route==3){const auto r=GraphHistory(f,anchor);if(!r.ok())Empty(r);return int(r.error);}
 if(route==4){const auto r=GraphRead(f,anchor);if(!r.ok())Empty(r);return int(r.error);}
 if(route==0){const auto r=Read(f);if(!r.ok())Empty(r);return int(r.error);}
 if(route==1){const auto r=Inventory(f,g);if(!r.ok())Check(!r.checkpoint&&r.inventory_pages.empty()&&!r.retained_image_bytes,"no failed inventory authority prefix");return int(r.error);}
 const auto r=Bound(f);if(!r.ok())Check(!r.selection&&!r.checkpoint_inventory.checkpoint&&!r.predecessor.checkpoint&&r.allocation.pages.empty()&&!r.retained_image_bytes,"no failed selected authority prefix");return int(r.error);
}
void Faults(unsigned route,int shard=-1){
 Fixture f(0);f.budget*=4;Graph g(f);Bundle b(g);g.plan.control_bundle=b.root;g.plan.base_selection_generation=1;g.Refresh();Install(f,g,b);if(route<3)SelectFixture(f,g);const auto anchor=Anchor(g);const auto original=f.Read(0,256);
 byte scratch=0;for(unsigned i=0;i<4097;++i){const auto read=f.device.ReadAt(0,&scratch,1);Check(read.ok()&&read.bytes_transferred==1,"stabilize optional metric-history capacity");}
 using HE=db::NativeManagementHistoryError;
 const int resource=route==0||route==4?int(AE::resource_exhausted):route==3?int(HE::resource_exhausted):route==1?int(db::NativeCheckpointError::resource_exhausted):int(db::NativeCheckpointSelectionError::resource_exhausted);
 const int hash=route==0||route==4?int(AE::hash_failure):route==3?int(HE::hash_failure):route==1?int(db::NativeCheckpointError::hash_failure):int(db::NativeCheckpointSelectionError::hash_failure);
 const int io=route==0||route==4?int(AE::io_failure):route==3?int(HE::io_failure):route==1?int(db::NativeCheckpointError::io_failure):int(db::NativeCheckpointSelectionError::io_failure);
 if(shard>=0){counting=true;allocations=0;const auto baseline=Call(f,g,route,anchor);counting=false;const auto sites=allocations;Check(!baseline,"allocation baseline");
  for(unsigned long at=shard;at<sites;at+=4){const auto lost=f.device.failed_io_latency_observations();Reset();io_counting=true;allocation_budget=at;const int result=Call(f,g,route,anchor);const auto remaining=allocation_budget;allocation_budget=-1;io_counting=false;
   if(!result)Check(remaining==-1&&f.device.failed_io_latency_observations()==lost+1,"only consumed optional observation failure may succeed");
   else{if(result!=resource)std::cerr<<"route="<<route<<" allocation="<<at<<" result="<<result<<" expected="<<resource<<"\n";Check(result==resource,"required allocation errors retained through consumer");}
   Check(!writes&&!syncs,"allocation failure cannot mutate node");
  }
  Check(f.Read(0,256)==original,"allocation sweep leaves entire file intact");std::cout<<"route="<<route<<" allocation sites="<<sites<<" shard="<<shard<<"\n";return;
 }
 Reset();io_counting=true;hash_counting=true;hash_seen=0;const int baseline=Call(f,g,route,anchor);io_counting=false;hash_counting=false;const auto nr=reads,nh=hash_seen;Check(!baseline&&!writes&&!syncs,"measured actual creator reader");
 for(unsigned mode=0;mode<2;++mode)for(unsigned at=1;at<=nr;++at){Reset();if(mode)corrupt_read=at;else read_fault=at;io_counting=true;const int result=Call(f,g,route,anchor);io_counting=false;Check(result&&(mode||result==io)&&!writes&&!syncs,"every read failure/corruption refuses without writes");}
 for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=nh;++at){Reset();hash_fault=mode;hash_target=at;hash_seen=0;io_counting=true;const int result=Call(f,g,route,anchor);io_counting=false;const bool consumed=!hash_fault;hash_fault=0;
  if(result!=hash)std::cerr<<"route="<<route<<" hash="<<at<<" mode="<<mode<<" result="<<result<<" expected="<<hash<<"\n";
  Check(consumed&&result==hash&&!writes&&!syncs,"every hash provider failure retained through consumer");}
 Reset();Check(f.Read(0,256)==original,"entire operational failure sweep leaves node unchanged");std::cout<<"route="<<route<<" reads="<<nr<<" hashes="<<nh<<"\n";
}
void SequenceCodecFaults(const Graph& g,const db::NativePublicationLease& lease){
 const auto call=[&](unsigned route){
  if(route==2)return db::BindNativePublicationPlanToLease(g.plan,lease,g.target_bytes);
  const auto result=route?db::DecodeNativePublicationPlan(g.plan_bytes):db::EncodeNativePublicationPlan(g.plan);
  if(!result.ok())Failed(result);return result.error;
 };
 for(unsigned route=0;route<3;++route){
  allocations=0;counting=true;const auto baseline=call(route);counting=false;const auto sites=allocations;
  Check(baseline==E::none&&sites,"version4 codec/binder allocation baseline");
  for(unsigned long at=0;at<=sites;++at){allocation_budget=at;const auto result=call(route);const auto remaining=allocation_budget;allocation_budget=-1;
   Check(at==sites?result==E::none&&remaining==0:result==E::resource_exhausted&&remaining==-1,"every required version4 allocation is consumed and classified");
  }
  hash_seen=0;hash_counting=true;const auto hashed=call(route);hash_counting=false;const auto hashes=hash_seen;
  Check(hashed==E::none&&hashes,"version4 codec/binder hash baseline");
  for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_fault=mode;hash_target=at;hash_seen=0;const auto result=call(route);const bool consumed=!hash_fault;hash_fault=0;hash_active=false;
   Check(consumed&&result==E::hash_failure,"every version4 hash failure is consumed and classified");
  }
 }
 Reset();
}
void Sequences(){
 for(unsigned profile=0;profile<5;++profile){
  Fixture f(profile);f.budget*=4;
  for(unsigned burn=0;burn<3;++burn){const auto base=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);Check(base.ok(),"actual pre-burn inspection");
   auto reserved=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*base.snapshot,Id(30000+burn),f.budget);
   Check(reserved.ok()&&reserved.lease->snapshot().watermark.watermark==burn+2&&reserved.lease->snapshot().selection.selection_generation==1,"actual durable reservation burns do not select a checkpoint");
  }
  Graph g(f,false,1,nullptr,5);Bundle b(g);g.plan.control_bundle=b.root;g.plan.base_selection_generation=1;g.Refresh();
  const auto inspected=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);Check(inspected.ok(),"actual post-burn inspection");
  auto reserved=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*inspected.snapshot,g.plan.operation_uuid,f.budget,&g.plan.intent);
  Check(reserved.ok()&&reserved.lease->snapshot().watermark.watermark==5&&reserved.lease->snapshot().selection.selection_generation==1,"intent reservation retains distinct actual selector and checkpoint counters");
  g.plan.reservation_state_sha256=reserved.lease->snapshot().state_sha256;g.Refresh();
  const auto image=db::EncodeNativePublicationPlan(g.plan);const auto decoded=db::DecodeNativePublicationPlan(g.plan_bytes);
  Check(image.ok()&&decoded.ok()&&image.bytes==Oracle(g.plan)&&image.sha256==Sha(g.plan_bytes)&&decoded.sha256==image.sha256&&decoded.plan->base_selection_generation==1,"independent full version4 image and digest roundtrip");
  Check(LoadLittle16(g.plan_bytes.data()+136)==4&&LoadLittle16(g.plan_bytes.data()+138)==1024&&LoadLittle32(g.plan_bytes.data()+140)==1152&&LoadLittle64(g.plan_bytes.data()+1040)==1,"exact version4 selector field layout");
  auto old=g.plan;old.base_selection_generation.reset();const auto old_image=db::EncodeNativePublicationPlan(old);const auto old_decoded=db::DecodeNativePublicationPlan(old_image.bytes);
  Check(old_image.ok()&&old_image.bytes==Oracle(old)&&old_decoded.ok()&&!old_decoded.plan->base_selection_generation&&LoadLittle16(old_image.bytes.data()+136)==3,"version3 remains unchanged and has no inferred selector sequence");
  for(const auto value:{u64(0),std::numeric_limits<u64>::max()}){auto bad=g.plan;bad.base_selection_generation=value;Bad(bad);}
  {auto bad=g.plan;bad.control_bundle.reset();Bad(bad);}
  {auto high=g.plan;high.base_selection_generation=std::numeric_limits<u64>::max()-1;const auto encoded=db::EncodeNativePublicationPlan(high);Check(encoded.ok()&&encoded.bytes==Oracle(high)&&db::DecodeNativePublicationPlan(encoded.bytes).plan->base_selection_generation==high.base_selection_generation,"largest nonwrapping selector base roundtrip");}
  for(unsigned mode=0;mode<6;++mode){auto raw=g.plan_bytes;
   if(mode==0)raw[1048]=1;
   if(mode==1)raw[1151]=1;
   if(mode==2)raw.back()=1;
   if(mode==3)Num(raw,136,2,3);
   if(mode==4){std::copy_n("SBPPM003",8,raw.begin()+128);Num(raw,136,2,3);}
   if(mode==5){std::copy_n("SBPPM002",8,raw.begin()+128);Num(raw,136,2,2);Num(raw,138,2,896);Num(raw,140,4,1024);}
   PlanSeal(raw);Failed(db::DecodeNativePublicationPlan(raw));
  }
  const auto original=f.Read(0,256);auto wrong=g.plan;wrong.base_selection_generation=2;const auto wrong_target=Finish(wrong,g.target);
  Reset();io_counting=true;const auto bind=db::BindNativePublicationPlanToLease(wrong,*reserved.lease,wrong_target);
  const auto rejected=db::InstallNativeManagementControlGraphOnLease(*reserved.lease,wrong,wrong_target,g.extent,b.pages,f.budget);io_counting=false;
  Check(bind==E::binding_mismatch&&rejected.error==db::NativePublicationError::binding_mismatch&&!rejected.snapshot&&!writes&&!syncs&&f.Read(0,256)==original,"wrong actual base selector counter fails before any mutation");
  if(profile==0)SequenceCodecFaults(g,*reserved.lease);
  Check(db::InstallNativeManagementControlGraphOnLease(*reserved.lease,g.plan,g.target_bytes,g.extent,b.pages,f.budget).ok(),"install exact version4 graph under real lease");reserved.lease.reset();
  Check(f.device.Close().ok()&&f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"cold reopen installed version4 plan");
  Write(f,g.plan.target_checkpoint.page_number,Bytes(f.size));Check(f.device.Sync().ok(),"isolated missing target checkpoint persisted");
  const auto pending=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(pending.ok()&&pending.snapshot->selection.selection_generation==1&&pending.snapshot->watermark.watermark==5,"cold actual old selection and burned watermark preserved");
  auto resumed=db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*pending.snapshot,g.plan.operation_uuid,g.plan.intent,f.budget);Check(resumed.ok(),"cold exact original request lease");
  const auto reconstructed=db::ResumeNativeManagementControlGraphOnLease(*resumed.lease,f.budget);Check(reconstructed.ok(),"cold reconstruction with original selector sequence");resumed.lease.reset();
  const auto stored=db::DecodeNativePublicationPlan(f.Read(g.plan.header.page_number));Check(stored.ok()&&stored.bytes==g.plan_bytes&&stored.plan->base_selection_generation==1&&f.Read(g.plan.target_checkpoint.page_number)==g.target_bytes,"cold reconstruction neither rewrites counter nor mints identities");
  Check(GraphRead(f,Anchor(g)).ok()&&Read(f).ok()&&Read(f).publications.empty()&&!Inventory(f,g).ok(),"reconstructed graph does not grant selected status");
  SelectFixture(f,g);Good(f,g,1);
  const auto selected=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(selected.ok()&&selected.snapshot->selection.selection_generation==2&&selected.snapshot->selection.checkpoint_generation==5,"selected sequence increments once despite three real reservation burns");
  for(unsigned slot=0;slot<2;++slot){const auto ref=std::find_if(g.zero.roots.begin(),g.zero.roots.end(),[&](const auto& r){return r.kind==18+slot;});auto s=*db::DecodeNativeCheckpointSelection(f.Read(ref->page_number)).selection;
   s.selection_generation=5;s.previous_selection_generation=4;const auto raw=SelectorOracle(s);Check(db::DecodeNativeCheckpointSelection(raw).ok(),"wrong counter pair remains individually canonical");Write(f,ref->page_number,raw);
  }
  const auto mismatched=f.Read(0,256);Reset();io_counting=true;const auto authority=Read(f);const auto inventory=Inventory(f,g);const auto bound=Bound(f);const auto history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),f.budget);const auto graph=GraphRead(f,Anchor(g));io_counting=false;
  Empty(authority);Check(!inventory.ok()&&!bound.ok()&&!history.ok()&&graph.ok()&&!writes&&!syncs&&f.Read(0,256)==mismatched,"canonical but incorrect actual sequence refused by selected consumers only");SelectFixture(f,g);
  if(profile==0){Graph next(f,false,1,&g);Bundle next_bundle(next);next.plan.control_bundle=next_bundle.root;next.plan.base_selection_generation=2;next.Refresh();Install(f,next,next_bundle);SelectFixture(f,next);Good(f,next,2);
   const auto second=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);Check(second.ok()&&second.snapshot->selection.selection_generation==3&&second.snapshot->selection.checkpoint_generation==6,"second publication preserves independent counter progression");
  }
  Check(f.device.Close().ok()&&f.device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"version4 read-only reopen");const auto reopened=Read(f);Check(reopened.ok()&&reopened.selection->selection_generation==(profile?2:3),"actual selected sequence survives read-only reopen");
 }
}
void Profiles(){
 for(unsigned profile=0;profile<5;++profile){Fixture f(profile);f.budget*=4;Graph g(f);Bundle b(g);g.plan.control_bundle=b.root;g.Refresh();Install(f,g,b);
  const auto unselected=Read(f);Check(unselected.ok()&&unselected.publications.empty()&&!Inventory(f,g).ok(),"unselected graphs cannot supply selected creator authority");
  SelectFixture(f,g);Good(f,g,1);Check(f.device.Close().ok()&&f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"actual reopened node");Good(f,g,1);
  if(profile==0){Graph next(f,false,1,&g);Bundle next_bundle(next);next.plan.control_bundle=next_bundle.root;next.Refresh();Install(f,next,next_bundle);SelectFixture(f,next);Good(f,next,2);}
  Check(f.device.Close().ok()&&f.device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"read-only reopen");const auto read=Read(f);Check(read.ok(),"read-only selected creator proof");
 }
}
void Graphs(){
 static_assert(!std::is_convertible_v<db::NativeManagementGraphHistory,db::NativeManagementHistory>);
 static_assert(!std::is_convertible_v<db::NativeManagementControlGraph,db::NativeManagementControlAuthority>);
 for(unsigned profile=0;profile<5;++profile){
  Fixture f(profile);f.budget*=4;Graph g(f);Bundle b(g);g.plan.control_bundle=b.root;g.Refresh();Install(f,g,b);
  const auto anchor=Anchor(g);const auto original=f.Read(0,256);
  Reset();io_counting=true;const auto graph=GraphRead(f,anchor);const auto history=GraphHistory(f,anchor);io_counting=false;
  Check(graph.ok()&&graph.anchor==anchor&&graph.publications.size()==1&&history.ok()&&history.anchor==anchor&&history.entries.size()==1,"actual installed unselected immutable graph inspection");
  Check(!writes&&!syncs&&f.Read(0,256)==original,"unselected graph inspection cannot mutate selectors or any node byte");
  Check(Read(f).ok()&&Read(f).publications.empty()&&!Inventory(f,g).ok(),"graph inspection never promotes unselected publication");
  Check(GraphRead(f,anchor,graph.verified_image_bytes).ok()&&GraphHistory(f,anchor,history.verified_image_bytes).ok(),"exact anchored verification budgets");
  Empty(GraphRead(f,anchor,graph.verified_image_bytes-1));Empty(GraphHistory(f,anchor,history.verified_image_bytes-1));
  for(unsigned field=0;field<12;++field){auto bad=anchor;
   if(field==0)bad.checkpoint.filespace_uuid=Id(3000);
   if(field==1)bad.checkpoint.page_number++;
   if(field==2)bad.checkpoint.page_generation++;
   if(field==3)bad.checkpoint.page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[(profile+1)%5].uuid;
   if(field==4)bad.checkpoint_object_uuid=Id(3000);
   if(field==5)bad.checkpoint_sha256.front()^=1;
   if(field==6)bad.checkpoint_generation++;
   if(field==7)bad.root_set_generation++;
   if(field==8)bad.timeline_uuid=Id(3000);
   if(field==9)bad.checkpoint_sha256={};
   if(field==10)bad.checkpoint_generation=0;
   if(field==11)bad.checkpoint_object_uuid={};
   Reset();io_counting=true;const auto failed_graph=GraphRead(f,bad);const auto failed_history=GraphHistory(f,bad);io_counting=false;Empty(failed_graph);Empty(failed_history);
   Check(!writes&&!syncs&&f.Read(0,256)==original,"bad caller inspection anchor has no mutation or proof prefix");
  }
  for(unsigned damaged=1;damaged<=3;++damaged){Write(f,0,original);
   for(unsigned slot=0;slot<2;++slot)if(damaged&(1u<<slot)){const auto ref=std::find_if(g.zero.roots.begin(),g.zero.roots.end(),[&](const auto& r){return r.kind==18+slot;});auto bytes=f.Read(ref->page_number);bytes.back()^=1;Write(f,ref->page_number,bytes);}
   const auto before=f.Read(0,256);Reset();io_counting=true;const auto inspected=GraphRead(f,anchor);const auto inspected_history=GraphHistory(f,anchor);const auto selected=Read(f);const auto bound=Bound(f);io_counting=false;
   Check(inspected.ok()&&inspected_history.ok()&&!bound.ok(),"immutable graph inspection ignores damaged selectors without bypassing selected admission");Empty(selected);
   Check(!writes&&!syncs&&f.Read(0,256)==before,"damaged selectors are never repaired or overlaid by inspection");
  }
  Write(f,0,original);SelectFixture(f,g);const auto selected=Read(f);const auto selected_history=db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),f.budget);
  Check(selected.ok()&&selected.anchor==anchor&&selected.allocations==graph.allocations&&selected.verified_image_bytes==graph.verified_image_bytes+4*f.size,"selected proof adds exactly actual pair verification work");
  Check(selected_history.ok()&&selected_history.anchor==anchor&&selected_history.latest==history.latest&&selected_history.idempotency==history.idempotency&&selected_history.verified_image_bytes==history.verified_image_bytes+4*f.size,"selected and anchored histories share exact physical ancestry");
  const auto peer=std::find_if(g.zero.roots.begin(),g.zero.roots.end(),[](const auto& r){return r.kind==19;});
  const auto stable_peer=f.Read(peer->page_number);Bytes old_peer(original.begin()+peer->page_number*f.size,original.begin()+(peer->page_number+1)*f.size);Write(f,peer->page_number,old_peer);
  const auto interrupted=f.Read(0,256);Reset();io_counting=true;const auto pending_graph=GraphRead(f,anchor);const auto pending_history=GraphHistory(f,anchor);const auto pending_selection=Read(f);io_counting=false;
  Check(pending_graph.ok()&&pending_history.ok(),"actual first-new second-old selector interruption permits only anchored inspection");Empty(pending_selection);
  Check(!writes&&!syncs&&f.Read(0,256)==interrupted,"graph inspection never completes interrupted selector publication");Write(f,peer->page_number,stable_peer);
  const auto historical=GraphRead(f,Anchor(g.base,g.base_bytes));Check(historical.ok()&&historical.publications.empty(),"historical genesis inspection is not current-root selection");
  if(profile==0){TransactionTail tail(f,g);tail.Install();const auto root=Anchor(tail.cp,tail.bytes);const auto tail_graph=GraphRead(f,root);Check(tail_graph.ok()&&tail_graph.publications.size()==1&&GraphHistory(f,root).ok(),"anchored transaction tail retains operation graph");
   auto& map=Cover(tail.maps,g.plan.header.page_number);map.states[g.plan.header.page_number-map.first_page]=State::free;map.records.erase(std::remove_if(map.records.begin(),map.records.end(),[&](const auto& r){return r.page_number==g.plan.header.page_number;}),map.records.end());tail.Refresh();tail.Install();
   const auto bad=Anchor(tail.cp,tail.bytes);Check(GraphHistory(f,bad).ok(),"retained physical history alone cannot authorize freed controls");Empty(GraphRead(f,bad));Write(f,0,original);
  }
  if(profile==0){SelectFixture(f,g);Graph next(f,false,1,&g);Bundle next_bundle(next);next.plan.control_bundle=next_bundle.root;next.Refresh();Install(f,next,next_bundle);
   const auto next_anchor=Anchor(next);const auto pending=GraphRead(f,next_anchor);const auto pending_history=GraphHistory(f,next_anchor);
   Check(pending.ok()&&pending.publications.size()==2&&pending_history.ok()&&pending_history.entries.size()==2,"actual pending second operation graph includes the selected first publication");
   Check(Read(f).publications.size()==1&&!Inventory(f,next).ok(),"pending graph inspection does not select the second operation");Write(f,0,original);
  }
  auto bad_map=*page::DecodeNativeAllocationMap(g.after_bytes.front()).map;bad_map.records.front().allocation_uuid=Id(3000);const auto encoded=page::EncodeNativeAllocationMap(bad_map);Check(encoded.ok(),"altered stored map remains canonical");Write(f,bad_map.header.page_number,encoded.bytes);
  Check(GraphHistory(f,anchor).ok(),"bundle-only anchored history is not actual stored-map proof");Empty(GraphRead(f,anchor));Write(f,0,original);
  Check(f.device.Close().ok()&&f.device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"anchored actual read-only reopen");Check(GraphRead(f,anchor).ok()&&GraphHistory(f,anchor).ok(),"unselected graph persists across read-only reopen");
 }
}
void Invalid(){
 Fixture f(0);f.budget*=4;Graph g(f);Bundle b(g);g.plan.control_bundle=b.root;g.Refresh();Install(f,g,b);SelectFixture(f,g);const auto original=f.Read(0,256);const auto original_after=g.after;
 const auto reject=[&]{const auto before=f.Read(0,256);Reset();io_counting=true;const auto proof=Read(f);const auto inv=Inventory(f,g);const auto bound=Bound(f);io_counting=false;Empty(proof);Check(!inv.ok()&&!bound.ok()&&!writes&&!syncs&&before==f.Read(0,256),"actual consumers reject false creator proof without mutation");};
 // The bundle remains valid while a separately stored target map is changed.
 auto map=*page::DecodeNativeAllocationMap(g.after_bytes.front()).map;map.records.front().allocation_uuid=Id(9988);const auto raw=page::EncodeNativeAllocationMap(map);Check(raw.ok(),"canonical wrong stored target map");Write(f,map.header.page_number,raw.bytes);
 Check(db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),f.budget).ok(),"physical bundle history alone is insufficient");reject();Write(f,0,original);
 for(unsigned mode=0;mode<4;++mode){g.after=original_after;
  if(mode==0)Find(g.after,g.base.header.page_number).owner_uuid=Id(9988);
  if(mode==1){auto& m=Cover(g.after,g.base.header.page_number);m.states[g.base.header.page_number-m.first_page]=State::quarantined;}
  if(mode==2){auto& m=Cover(g.after,240);m.states[240-m.first_page]=State::allocated;auto record=Find(g.after,g.plan.header.page_number);record.page_number=240;record.allocation_uuid=Id(9988);record.page_uuid=Id(9989);m.records.push_back(record);std::sort(m.records.begin(),m.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});}
  if(mode==3)Find(g.after,g.plan.header.page_number).creator_operation_uuid=Id(9988);
  g.Refresh();b.Oracle();g.plan.control_bundle=b.root;g.Refresh();RawGraph(f,g,b);reject();Write(f,0,original);
 }
 g.after=original_after;g.Refresh();b.Oracle();g.plan.control_bundle=b.root;g.Refresh();
 {Graph changed(f);Bundle changed_bundle(changed);for(auto& map:changed.before)map.creator_transaction_uuid=Id(9977);changed.before_bytes=EncodeMaps(changed.before);
  std::find_if(changed.base.roots.begin(),changed.base.roots.end(),[](const auto& r){return r.role==4;})->sha256=Sha(changed.before_bytes.front());const auto encoded=db::EncodeNativeCheckpointRoot(changed.base);Check(encoded.ok(),"resealed base checkpoint fixture");changed.base_bytes=encoded.bytes;changed.plan.base_checkpoint_sha256=Sha(changed.base_bytes);changed.plan.control_bundle=changed_bundle.root;changed.Refresh();RawGraph(f,changed,changed_bundle);
  Check(db::ReadNativeManagementHistoryFromOpenDevices(Id(1),f.devices,Id(2),f.budget).ok(),"physical history does not prove transaction map creator");Check(Read(f).error==AE::creator_mismatch,"exact original transaction creator still required");reject();Write(f,0,original);}
 const auto slot=std::find_if(g.zero.roots.begin(),g.zero.roots.end(),[](const auto& r){return r.kind==19;});auto torn=f.Read(slot->page_number);torn.back()^=1;Write(f,slot->page_number,torn);reject();Write(f,0,original);Good(f,g,1);
}
}
int main(int argc,char** argv){try{const std::string mode=argc>1?argv[1]:"all";Check(mode=="all"||mode=="profiles"||mode=="invalid"||mode=="retained"||mode=="graphs"||mode=="sequences"||mode=="faults"||mode=="allocations","known test mode");if(mode=="faults"||mode=="allocations"){Check(argc==(mode=="faults"?3:4),"fault route and shard arguments");const auto route=std::stoul(argv[2]);Check(route<5,"fault route range");int shard=-1;if(mode=="allocations"){shard=std::stoi(argv[3]);Check(shard>=0&&shard<4,"allocation shard range");}Faults(route,shard);}if(mode=="all"||mode=="profiles")Profiles();if(mode=="all"||mode=="invalid")Invalid();if(mode=="all"||mode=="retained")Retained();if(mode=="all"||mode=="graphs")Graphs();if(mode=="all"||mode=="sequences")Sequences();std::cout<<"native management control authority checks="<<checks<<"\n";return 0;}catch(const std::exception& e){allocation_budget=-1;io_counting=false;hash_fault=0;std::cerr<<e.what()<<"\n";return 1;}}

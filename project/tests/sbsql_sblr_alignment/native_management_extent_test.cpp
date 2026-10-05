// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_management_extent.hpp"
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
#include <unistd.h>
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
using O=db::NativeManagementOperation;using S=db::NativeManagementStep;
using OS=db::NativeManagementState;using SS=db::NativeManagementStepState;
using E=db::NativeManagementExtentError;using Bytes=std::vector<byte>;using Pages=std::vector<Bytes>;
unsigned checks=0;constexpr u64 budget=1<<26;
void Check(bool v,const char* why,std::source_location at=std::source_location::current()){++checks;if(!v)throw std::runtime_error(std::string(why)+" line="+std::to_string(at.line()));}
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
template<class Operation> Bytes Oracle(const Operation& o){Bytes b(512);const bool exact=!o.normalized_request_bytes.empty();std::copy_n(exact?"SBMGO002":"SBMGO001",8,b.begin());Num(b,8,2,exact?2:1);Num(b,10,2,512);Num(b,476,4,o.normalized_request_bytes.size());
 Put(b,16,o.database_uuid);Put(b,32,o.bootstrap_uuid);Put(b,48,o.uuid);Put(b,64,o.descriptor_uuid);Put(b,80,o.family_uuid);Put(b,96,o.target_type_uuid);Put(b,112,o.target_uuid);Put(b,128,o.initiator_uuid);Put(b,144,o.request_context_uuid);Put(b,160,o.policy_snapshot_uuid);Put(b,176,o.security_snapshot_uuid);Put(b,192,o.phase_uuid);Put(b,208,o.boundary_uuid);Put(b,224,o.created_at);Put(b,240,o.updated_at);Put(b,256,o.terminal_at);
 std::copy(o.normalized_request_sha256.begin(),o.normalized_request_sha256.end(),b.begin()+272);Put(b,304,o.resource_plan_uuid);Put(b,320,o.lock_plan_uuid);Put(b,336,o.result_uuid);Put(b,352,o.diagnostic_uuid);Put(b,368,o.evidence_uuid);Put(b,384,o.metric_evidence_uuid);Put(b,400,o.cluster_uuid);
 u32 flags=(o.evidence_required?16u:0u)|(o.metrics_required?32u:0u);for(unsigned i=0;i<4;++i)if(o.generation_guards[i]){flags|=1u<<i;Num(b,416+8*i,8,*o.generation_guards[i]);}
 Num(b,448,8,o.revision);Num(b,456,4,o.steps.size());Num(b,460,4,o.idempotency_key.size());Num(b,464,2,u16(o.state));Num(b,466,2,u16(o.scope));Num(b,468,2,o.initiator_kind);Num(b,470,2,u16(o.restart));Num(b,472,4,flags);
 b.insert(b.end(),o.idempotency_key.begin(),o.idempotency_key.end());
 b.insert(b.end(),o.normalized_request_bytes.begin(),o.normalized_request_bytes.end());
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
void Failed(const db::NativeManagementExtentImage& r){Check(!r.ok()&&!r.root&&r.pages.empty(),"no failed image prefix");}
void Failed(const db::NativeManagementExtentRead& r){Check(!r.ok()&&!r.record&&r.page_headers.empty(),"no failed read prefix");}
struct ViewBacking {
  Bytes aggregate;
  std::vector<d::NativeCommonPageHeader> headers;
  std::vector<db::NativeManagementStepView> steps;
  std::vector<Uuid> identities;
  std::vector<std::span<const byte>> pages;
  ViewBacking(const Pages& input,const db::NativeManagementExtentRoot& root)
    :aggregate(root.aggregate_bytes),headers(root.page_count),steps(root.aggregate_bytes/257),
     identities(std::max(2*std::size_t{root.page_count}+1,2*steps.size()+7)){
    for(const auto& page:input)pages.emplace_back(page);
  }
  db::NativeManagementExtentViewWorkspace Workspace(){
    return {aggregate,headers,steps,identities};
  }
};
void Failed(const db::NativeManagementExtentViewRead& r){
  Check(!r.ok()&&!r.record&&r.page_headers.empty()&&r.aggregate.empty(),"no borrowed extent prefix");
}
void Failed(const db::NativeManagementExtentEncoding& r){
  Check(!r.ok()&&!r.root&&r.pages.empty(),"no bounded encoded extent prefix");
}
void EncodingParity(const db::NativeManagementOperationView& record,
    std::span<const d::NativeCommonPageHeader> headers,const db::NativeManagementExtentRoot& root,const Pages& expected){
  const auto aggregate_oracle=Oracle(record);const auto size=headers.front().page_size_bytes;
  Bytes output(headers.size()*size+19,0xa7),aggregate(aggregate_oracle.size());
  std::vector<Uuid> identities(std::max(2*headers.size()+1,2*record.steps.size()+7));
  allocation_budget=0;const auto result=db::EncodeNativeManagementExtentInto(record,root.object_uuid,headers,budget,
    std::span<byte>(output).subspan(1),aggregate,identities);
  const auto left=allocation_budget;allocation_budget=-1;
  Check(left==0&&result.ok()&&*result.root==root&&result.pages.data()==output.data()+1&&result.pages.size()==headers.size()*size,
    "bounded complete physical extent and root use caller backing without C++ allocation");
  for(std::size_t i=0;i<expected.size();++i)Check(std::equal(expected[i].begin(),expected[i].end(),result.pages.begin()+i*size),"every bounded physical page equals independent stored bytes");
  Check(aggregate==aggregate_oracle&&Oracle(record)==aggregate_oracle&&output.front()==0xa7&&
    std::all_of(output.begin()+1+result.pages.size(),output.end(),[](byte b){return b==0xa7;}),"unaligned extent output preserves suffix and immutable operation backing");
}
db::NativeManagementExtentRead Decode(const Pages& pages,const db::NativeManagementExtentRoot& root,
    const Uuid& database,const Uuid& bootstrap,u64 limit){
  const bool compare=allocation_budget<0&&!counting&&!hash_counting&&!hash_fault;
  auto result=db::DecodeNativeManagementExtent(pages,root,database,bootstrap,limit);
  if(compare){
    ViewBacking backing(pages,root);
    allocation_budget=0;
    const auto view=db::DecodeNativeManagementExtentInto(backing.pages,root,database,bootstrap,limit,backing.Workspace());
    const bool no_allocation=allocation_budget==0;allocation_budget=-1;
    Check(no_allocation&&view.error==result.error,"complete borrowed extent parity without C++ allocation");
    if(result.ok()){
      EncodingParity(*view.record,view.page_headers,root,pages);
      Check(view.ok()&&Oracle(*view.record)==Oracle(*result.record),"independent complete borrowed aggregate reconstruction");
      Check(view.aggregate.data()==backing.aggregate.data()&&view.aggregate.size()==root.aggregate_bytes&&
        view.page_headers.data()==backing.headers.data()&&view.page_headers.size()==result.page_headers.size(),
        "retained aggregate and headers use only caller backing");
      Check(view.record->idempotency_key.data()==reinterpret_cast<const char*>(backing.aggregate.data()+512),
        "operation key borrows reconstructed canonical bytes");
      Check(view.record->steps.empty()||view.record->steps.data()==backing.steps.data(),"borrowed steps use exact caller array");
      for(std::size_t i=0;i<view.page_headers.size();++i){
        const auto a=d::EncodeNativeCommonPageHeader(view.page_headers[i]);
        const auto b=d::EncodeNativeCommonPageHeader(result.page_headers[i]);
        Check(a.ok()&&b.ok()&&a.bytes==b.bytes,"complete physical header parity");
      }
    }else Failed(view);
  }
  return result;
}
void Good(const O& o,const std::vector<d::NativeCommonPageHeader>& h){const auto expected=ExtentOracle(o,h);const auto encoded=db::EncodeNativeManagementExtent(o,Id(5000),h,budget);
 Check(encoded.ok()&&encoded.pages==expected&&encoded.root->first_page_sha256==Sha(expected[0])&&encoded.root->aggregate_sha256==Sha(Oracle(o)),"independent full extent bytes and hash layers");
 const auto decoded=Decode(expected,*encoded.root,o.database_uuid,o.bootstrap_uuid,budget);Check(decoded.ok()&&*decoded.record==o&&decoded.page_headers.size()==h.size(),"all reconstructed aggregate fields and page bindings");for(std::size_t i=0;i<h.size();++i)Check(decoded.page_headers[i].page_uuid==h[i].page_uuid&&decoded.page_headers[i].page_number==h[i].page_number&&decoded.page_headers[i].page_generation==h[i].page_generation,"complete verified physical page identities");}
void CodecFaults(const O& o,const std::vector<d::NativeCommonPageHeader>& headers){const auto baseline=db::EncodeNativeManagementExtent(o,Id(5000),headers,budget);Check(baseline.ok(),"fault baseline");const auto root=*baseline.root;
 for(unsigned mode=0;mode<2;++mode){
  const auto call=[&](){if(mode){const auto r=Decode(baseline.pages,root,o.database_uuid,o.bootstrap_uuid,budget);if(!r.ok())Failed(r);return r.error;}const auto r=db::EncodeNativeManagementExtent(o,Id(5000),headers,budget);if(!r.ok())Failed(r);return r.error;};
  counting=true;allocations=0;Check(call()==E::none,"codec allocation baseline");counting=false;const auto sites=allocations;
  for(unsigned long i=0;i<sites;++i){allocation_budget=i;const auto error=call();allocation_budget=-1;Check(error==E::resource_exhausted,"every codec allocation failure");}
  hash_counting=true;hash_seen=0;Check(call()==E::none,"hash baseline");hash_counting=false;const auto hashes=hash_seen;
  for(unsigned fault=1;fault<=5;++fault)for(unsigned at=1;at<=hashes;++at){hash_fault=fault;hash_target=at;hash_seen=0;hash_active=false;const auto error=call();const bool consumed=!hash_fault;hash_fault=0;Check(consumed&&error==E::hash_failure,"every page aggregate and chain hash failure");}
 }
 const u64 need=u64{root.page_count}*headers[0].page_size_bytes+4*u64{root.aggregate_bytes}+2*u64{headers[0].page_size_bytes};
 Check(db::EncodeNativeManagementExtent(o,Id(5000),headers,need).ok(),"exact extent image budget");Failed(db::EncodeNativeManagementExtent(o,Id(5000),headers,need-1));Failed(Decode(baseline.pages,root,o.database_uuid,o.bootstrap_uuid,need-1));
 for(unsigned field=0;field<18;++field){auto r=root;switch(field){case 0:r.object_uuid={};break;case 1:r.object_uuid=o.uuid;break;case 2:r.operation_uuid=Id(40);break;case 3:r.revision++;break;case 4:r.aggregate_bytes++;break;case 5:r.page_count++;break;case 6:r.first.page_number++;break;case 7:r.first.page_generation++;break;case 8:r.first.filespace_uuid=Id(41);break;case 9:r.first.page_size_profile_uuid=Id(41);break;case 10:r.first_page_sha256={};break;case 11:r.aggregate_sha256={};break;case 12:r.aggregate_sha256[0]^=1;break;case 13:r.first_page_sha256[0]^=1;break;case 14:r.first.page_number=~u64{0};break;case 15:r.aggregate_bytes=512;break;case 16:r.page_count=0;break;case 17:r.first.page_generation=0;break;}Failed(Decode(baseline.pages,r,o.database_uuid,o.bootstrap_uuid,budget));}
 for(std::size_t at:{128u,136u,138u,140u,144u,160u,176u,192u,200u,204u,208u,212u,216u,224u,256u,260u,287u,352u,383u}){auto pages=baseline.pages;auto r=root;pages[1][at]^=1;Rechain(pages,r);Failed(Decode(pages,r,o.database_uuid,o.bootstrap_uuid,budget));}
 auto pages=baseline.pages;auto r=root;pages.back().back()=1;Rechain(pages,r);Failed(Decode(pages,r,o.database_uuid,o.bootstrap_uuid,budget));
 auto invalid=o;invalid.normalized_request_sha256={};pages=ExtentOracle(invalid,headers);r=root;r.aggregate_sha256=Sha(Oracle(invalid));r.first_page_sha256=Sha(pages[0]);Check(Decode(pages,r,o.database_uuid,o.bootstrap_uuid,budget).error==E::invalid_record,"valid physical seals cannot authorize an invalid aggregate");
 pages=baseline.pages;r=root;std::copy(pages[1].begin()+320,pages[1].begin()+352,pages[0].begin()+288);PageSeal(pages[0]);r.first_page_sha256=Sha(pages[0]);Failed(Decode(pages,r,o.database_uuid,o.bootstrap_uuid,budget));
 pages=baseline.pages;r=root;Put(pages[1],56,headers[0].page_uuid);Rechain(pages,r);Check(Decode(pages,r,o.database_uuid,o.bootstrap_uuid,budget).error==E::invalid_identity,"resealed duplicate physical page identity refused");
 pages=baseline.pages;std::swap(pages[0],pages[1]);Failed(Decode(pages,root,o.database_uuid,o.bootstrap_uuid,budget));pages=baseline.pages;pages.pop_back();Failed(Decode(pages,root,o.database_uuid,o.bootstrap_uuid,budget));
 for(unsigned field=0;field<8;++field){auto h=headers;switch(field){case 0:h[1].page_uuid=h[0].page_uuid;break;case 1:h[0].page_uuid=Id(5000);break;case 2:h[1].page_number++;break;case 3:h[1].page_generation++;break;case 4:h[1].flags=1;break;case 5:h[1].page_size_bytes*=2;break;case 6:h.pop_back();break;case 7:h[1].page_uuid.bytes[6]=0x40;break;}Failed(db::EncodeNativeManagementExtent(o,Id(5000),h,budget));}
}
void BorrowedBounds(){
  auto o=Record(2*(8192-384)+17);
  o.normalized_request_bytes.assign(8311,0x9b);o.normalized_request_sha256=Sha(o.normalized_request_bytes);
  const auto image=db::EncodeNativeManagementExtent(o,Id(5000),Headers(o,0),budget);
  Check(image.ok(),"bounded extent fixture");const auto root=*image.root;
  ViewBacking backing(image.pages,root);
  const auto needed=std::max(2*std::size_t{root.page_count}+1,2*o.steps.size()+7);
  auto exact=backing.Workspace();exact.steps=exact.steps.first(o.steps.size());exact.identities=exact.identities.first(needed);
  const auto call=[&](auto workspace){
    allocation_budget=0;const auto result=db::DecodeNativeManagementExtentInto(backing.pages,root,o.database_uuid,o.bootstrap_uuid,budget,workspace);
    const bool untouched=allocation_budget==0;allocation_budget=-1;
    Check(untouched,"all bounded decode paths avoid C++ allocation");return result;
  };
  auto good=call(exact);Check(good.ok()&&Oracle(*good.record)==Oracle(o),"exact capacity whole extent under heap denial");
  for(unsigned field=0;field<4;++field){
    auto w=exact;switch(field){
      case 0:w.aggregate=w.aggregate.first(w.aggregate.size()-1);break;
      case 1:w.page_headers=w.page_headers.first(w.page_headers.size()-1);break;
      case 2:w.steps=w.steps.first(w.steps.size()-1);break;
      case 3:w.identities=w.identities.first(w.identities.size()-1);break;
    }
    const auto result=call(w);Check(result.error==E::resource_exhausted,"each one-short backing refused");Failed(result);
  }
  const auto unchanged=backing.aggregate;
  for(unsigned alias=0;alias<4;++alias){
    auto w=exact;
    if(alias==0)w.page_headers={reinterpret_cast<d::NativeCommonPageHeader*>(w.aggregate.data()),1};
    if(alias==1)w.steps={reinterpret_cast<db::NativeManagementStepView*>(w.aggregate.data()),1};
    if(alias==2)w.identities={reinterpret_cast<Uuid*>(w.aggregate.data()),1};
    if(alias==3)w.steps={reinterpret_cast<db::NativeManagementStepView*>(w.page_headers.data()),1};
    const auto result=call(w);Check(result.error==E::invalid_workspace&&backing.aggregate==unchanged,"mutually aliasing backing rejected before writes");Failed(result);
  }
  for(unsigned field=0;field<2;++field){
    auto w=exact;
    if(field==0)w.page_headers={reinterpret_cast<d::NativeCommonPageHeader*>(w.aggregate.data()+1),1};
    else w.steps={reinterpret_cast<db::NativeManagementStepView*>(w.aggregate.data()+1),1};
    Check(call(w).error==E::invalid_workspace&&backing.aggregate==unchanged,"misaligned decoded storage refused");
  }
  for(std::size_t page=0;page<image.pages.size();++page){
    const auto before=image.pages[page];
    for(const std::size_t offset:{std::size_t{0},std::size_t{320},std::size_t{8192-16}}){
      auto w=exact;w.identities={reinterpret_cast<Uuid*>(const_cast<byte*>(image.pages[page].data())+offset),1};
      const auto result=call(w);Check(result.error==E::invalid_workspace&&image.pages[page]==before,"whole input page including header seal and tail excluded");Failed(result);
    }
  }
  auto* root_alias=std::construct_at(reinterpret_cast<db::NativeManagementExtentRoot*>(backing.aggregate.data()),root);
  auto snapshot=backing.aggregate;
  allocation_budget=0;auto aliased=db::DecodeNativeManagementExtentInto(backing.pages,*root_alias,o.database_uuid,o.bootstrap_uuid,budget,exact);
  bool untouched=allocation_budget==0;allocation_budget=-1;
  Check(untouched&&aliased.error==E::invalid_workspace&&backing.aggregate==snapshot,"root object cannot be overwritten through backing");
  std::destroy_at(root_alias);
  auto* descriptors=std::construct_at(reinterpret_cast<std::span<const byte>*>(backing.aggregate.data()),backing.pages.front());
  // Construct all descriptor objects in the caller aggregate and pass that exact array.
  for(std::size_t i=1;i<backing.pages.size();++i)std::construct_at(descriptors+i,backing.pages[i]);
  snapshot=backing.aggregate;
  allocation_budget=0;aliased=db::DecodeNativeManagementExtentInto({descriptors,backing.pages.size()},root,o.database_uuid,o.bootstrap_uuid,budget,exact);
  untouched=allocation_budget==0;allocation_budget=-1;
  Check(untouched&&aliased.error==E::invalid_workspace&&backing.aggregate==snapshot,"outer input descriptor array excluded before writes");
  for(std::size_t i=0;i<backing.pages.size();++i)std::destroy_at(descriptors+i);
  for(bool database_alias:{false,true}){
    auto* identity=std::construct_at(reinterpret_cast<Uuid*>(backing.aggregate.data()),database_alias?o.database_uuid:o.bootstrap_uuid);
    snapshot=backing.aggregate;allocation_budget=0;
    aliased=db::DecodeNativeManagementExtentInto(backing.pages,root,database_alias?*identity:o.database_uuid,
        database_alias?o.bootstrap_uuid:*identity,budget,exact);
    untouched=allocation_budget==0;allocation_budget=-1;
    Check(untouched&&aliased.error==E::invalid_workspace&&backing.aggregate==snapshot,"database and bootstrap argument objects excluded");
    std::destroy_at(identity);
  }
  hash_counting=true;hash_seen=0;good=call(exact);hash_counting=false;const auto hashes=hash_seen;
  Check(good.ok()&&hashes==2*root.page_count+3,"all page chains seals request and aggregate hashes accounted");
  for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){
    hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;
    const auto result=call(exact);const bool consumed=!hash_fault;hash_fault=0;
    Check(consumed&&result.error==E::hash_failure,"every borrowed hash failure remains exact under heap denial");Failed(result);
  }
  const auto max_offset=static_cast<u64>(std::numeric_limits<std::streamoff>::max());
  auto edge=root;edge.first.page_number=max_offset/8192-root.page_count;
  allocation_budget=0;
  const auto edge_ok=db::ValidateNativeManagementExtentRoot(edge,o.database_uuid,o.bootstrap_uuid,budget);
  ++edge.first.page_number;
  const auto edge_bad=db::ValidateNativeManagementExtentRoot(edge,o.database_uuid,o.bootstrap_uuid,budget);
  untouched=allocation_budget==0;allocation_budget=-1;
  Check(untouched&&edge_ok==E::none&&edge_bad==E::invalid_extent,"exact signed device extent boundary without diagnostic allocation");
  auto h=Headers(o,0);
  const auto bucket=[&](const Uuid& id){
    u64 value=14695981039346656037ULL;for(auto b:id.bytes){value^=b;value*=1099511628211ULL;}
    return value%(2*h.size()+1);
  };
  Uuid collision;for(unsigned n=10000;n<65000;++n){auto id=Id(n);if(id!=h[0].page_uuid&&bucket(id)==bucket(h[0].page_uuid)){collision=id;break;}}
  Check(!collision.is_nil(),"full identity collision fixture found");h[1].page_uuid=collision;
  Good(o,h);h[1].page_uuid=h[0].page_uuid;
  auto duplicate=ExtentOracle(o,h);auto duplicate_root=root;duplicate_root.first_page_sha256=Sha(duplicate.front());
  Check(Decode(duplicate,duplicate_root,o.database_uuid,o.bootstrap_uuid,budget).error==E::invalid_identity,"full identical UUID refused despite valid resealed chain");
}
struct Fixture {
 std::filesystem::path path;d::FileDevice device;d::NativeFilespaceDevice file;Uuid bootstrap;u64 size;
 Fixture(unsigned profile){char name[]="/tmp/sb-management-extent.XXXXXX";Check(mkdtemp(name),"isolated fixture");path=std::filesystem::path(name)/"node";const auto& p=d::kCanonicalFilespacePageProfiles[profile];size=p.page_size_bytes;
  db::NativeFilespaceInitializationRequest r;r.bootstrap={Id(1),Id(60),p.uuid,d::kNativeBootstrapIntegrityProfile,{},p.page_size_bytes,1,0,1,7};r.operation_uuid=Id(70);r.writer_uuid=Id(71);r.creator.transaction_uuid={UuidKind::transaction,Id(72)};r.creator.local_id=mga::MakeLocalTransactionId(1);r.creator.scope=mga::TransactionScope::local_node;r.creation_utc_millis=1789357072000ULL;r.total_pages=128;
  Check(device.Open(path.string(),d::FileOpenMode::create_new).ok(),"owned fixture device");file={Id(60),p.uuid,&device};r.policy_snapshot_uuid=Id(10);scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({r.bootstrap.database_uuid,r.policy_snapshot_uuid},{{},0,1000});Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(device,r,budget,issuer).ok(),"actual genesis");const auto z=d::ReadFilespacePageZeroFromOpenDevice(device);Check(z.ok(),"actual bootstrap");bootstrap=z.record->page_uuid;}
 ~Fixture(){device.Close();std::error_code ec;std::filesystem::remove_all(path.parent_path(),ec);}
 void Write(const Pages& pages){for(std::size_t i=0;i<pages.size();++i){const auto r=device.WriteAt((64+i)*size,pages[i].data(),pages[i].size());Check(r.ok()&&r.bytes_transferred==pages[i].size(),"isolated unselected extent fixture write");}Check(device.Sync().ok(),"fixture sync");}
};
void Physical(unsigned profile,bool exact=false){Fixture f(profile);auto o=Record(2*(f.size-384)+17);o.bootstrap_uuid=f.bootstrap;
 if(exact){o.normalized_request_bytes.resize(f.size+37);for(std::size_t n=0;n<o.normalized_request_bytes.size();++n)o.normalized_request_bytes[n]=static_cast<byte>(n);o.normalized_request_sha256=Sha(o.normalized_request_bytes);}
 const auto headers=Headers(o,profile);const auto pages=ExtentOracle(o,headers);const auto encoded=db::EncodeNativeManagementExtent(o,Id(5000),headers,budget);Check(encoded.ok()&&encoded.pages==pages,"physical oracle");const auto root=*encoded.root;
 // Persist the actual bounded encoder output, not merely the oracle image.
 ViewBacking source(pages,root);const auto borrowed=db::DecodeNativeManagementExtentInto(source.pages,root,o.database_uuid,o.bootstrap_uuid,budget,source.Workspace());Check(borrowed.ok(),"physical bounded source");
 Bytes staged(headers.size()*f.size),aggregate(root.aggregate_bytes);std::vector<Uuid> identities(std::max(2*headers.size()+1,2*borrowed.record->steps.size()+7));
 {auto guard=f.device.AcquireOperationGuard();allocation_budget=0;
  const auto bounded=db::EncodeNativeManagementExtentInto(*borrowed.record,root.object_uuid,headers,budget,staged,aggregate,identities);
  const auto left=allocation_budget;allocation_budget=-1;
  Check(left==0&&bounded.ok()&&*bounded.root==root,"actual device guard bounded complete extent encoding");
  for(std::size_t i=0;i<pages.size();++i){const auto bytes=bounded.pages.subspan(i*f.size,f.size);
   Check(std::equal(pages[i].begin(),pages[i].end(),bytes.begin()),"physical bounded image independent oracle");
   const auto written=f.device.WriteAt((64+i)*f.size,bytes.data(),bytes.size());Check(written.ok()&&written.bytes_transferred==bytes.size(),"write actual bounded extent output");}
  Check(f.device.Sync().ok(),"bounded extent sync before reopen");}
 const auto call=[&](){return db::ReadNativeManagementExtentFromOpenDevice(f.file,root,o.database_uuid,o.bootstrap_uuid,budget);};
 io_counting=true;reads=writes=syncs=0;const auto read=call();io_counting=false;const auto sites=reads;Check(read.ok()&&*read.record==o&&writes==0&&syncs==0,"read complete actual extent without writes or syncs");
 Check(f.device.Close().ok()&&f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok()&&call().ok(),"owned reopen");
 Check(f.device.Close().ok(),"close before independent process");const auto child=fork();Check(child>=0,"fork independent reader");if(!child){d::FileDevice own;if(!own.Open(f.path.string(),d::FileOpenMode::open_existing).ok())_exit(81);auto file=f.file;file.device=&own;const auto result=db::ReadNativeManagementExtentFromOpenDevice(file,root,o.database_uuid,o.bootstrap_uuid,budget);_exit(result.ok()&&*result.record==o?0:82);}int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"independent process full record");Check(f.device.Open(f.path.string(),d::FileOpenMode::open_existing).ok(),"parent reopen");
 if(profile==0){for(unsigned at=1;at<=sites;++at){reads=0;read_fault=at;io_counting=true;const auto r=call();io_counting=false;read_fault=0;Check(r.error==E::io_failure,"every actual read fault");Failed(r);}
  hash_counting=true;hash_seen=0;Check(call().ok(),"reader hash baseline");hash_counting=false;const auto hashes=hash_seen;
  for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_fault=mode;hash_target=at;hash_seen=0;hash_active=false;const auto r=call();const bool consumed=!hash_fault;hash_fault=0;Check(consumed&&r.error==E::hash_failure,"all reader hashes preserve failure");Failed(r);}
  counting=true;allocations=0;Check(call().ok(),"reader allocation baseline");counting=false;const auto allocs=allocations;
  for(unsigned long at=0;at<allocs;++at){const auto lost=f.device.failed_io_latency_observations();allocation_budget=at;const auto r=call();allocation_budget=-1;if(r.ok())Check(f.device.failed_io_latency_observations()==lost+1&&*r.record==o,"only explicit nonauthoritative telemetry loss allows successful fault read");else{Check(r.error==E::resource_exhausted,"all reader allocations classified");Failed(r);}}
  std::cout<<"reader pages="<<pages.size()<<" reads="<<sites<<" hashes="<<hashes<<" allocations="<<allocs<<"\n";
 }
 auto wrong=root;wrong.first.page_number=127;Check(db::ReadNativeManagementExtentFromOpenDevice(f.file,wrong,o.database_uuid,o.bootstrap_uuid,budget).error==E::invalid_extent,"actual file extent bound");
 const auto zero=d::ReadFilespacePageZeroFromOpenDevice(f.device);Check(zero.ok(),"root alias fixture");wrong=root;wrong.first.page_number=zero.record->roots.front().page_number;Check(db::ReadNativeManagementExtentFromOpenDevice(f.file,wrong,o.database_uuid,o.bootstrap_uuid,budget).error==E::invalid_extent,"actual root slots cannot alias operation extent");
 Failed(db::ReadNativeManagementExtentFromOpenDevice(f.file,root,Id(44),o.bootstrap_uuid,budget));Failed(db::ReadNativeManagementExtentFromOpenDevice(f.file,root,o.database_uuid,Id(44),budget));
 auto damaged=pages;damaged[1][500]^=1;f.Write(damaged);Failed(call());f.Write(pages);
 Check(f.device.Close().ok()&&f.device.Open(f.path.string(),d::FileOpenMode::open_existing_read_only).ok(),"read-only owned reopen");Check(call().ok(),"read-only physical inspection");
 Check(f.device.Close().ok(),"close reader fixture");Failed(call());
}
void EncodingBounds(){
 for(unsigned profile=0;profile<5;++profile)for(bool exact:{false,true}){
  auto record=Record(d::kCanonicalFilespacePageProfiles[profile].page_size_bytes);
  if(exact){record.normalized_request_bytes={0,255,1,0,2};record.normalized_request_sha256=Sha(record.normalized_request_bytes);}
  auto headers=Headers(record,profile);auto expected=ExtentOracle(record,headers);
  const auto image=db::EncodeNativeManagementExtent(record,Id(5000),headers,budget);Check(image.ok()&&image.pages==expected,"independent extent encoding boundary fixture");
  ViewBacking backing(expected,*image.root);const auto decoded=db::DecodeNativeManagementExtentInto(backing.pages,*image.root,record.database_uuid,record.bootstrap_uuid,budget,backing.Workspace());Check(decoded.ok(),"bounded immutable operation for extent encoding");
  auto view=*decoded.record;const auto object=Id(5000);const auto size=headers.front().page_size_bytes;
  const auto slots=std::max(2*headers.size()+1,2*view.steps.size()+7);
  Bytes output(headers.size()*size,0xa9),aggregate(Oracle(record).size(),0xb7);
  const auto untouched=output;const auto original=Oracle(view);std::vector<Uuid> identities(slots);
  const u64 allowance=output.size()+4*aggregate.size()+2*size;
  const auto call=[&](const auto& value,std::span<const d::NativeCommonPageHeader> pages,u64 limit,std::span<byte> dest,std::span<byte> work,std::span<Uuid> ids){
   allocation_budget=0;const auto r=db::EncodeNativeManagementExtentInto(value,object,pages,limit,dest,work,ids);
   const auto left=allocation_budget;allocation_budget=-1;Check(left==0,"extent encoding and refusals need no C++ allocation");return r;};
  const auto refuse=[&](const auto& value,std::span<const d::NativeCommonPageHeader> pages,u64 limit,std::span<byte> dest,std::span<byte> work,std::span<Uuid> ids,E error){
   const auto r=call(value,pages,limit,dest,work,ids);Failed(r);Check(r.error==error&&output==untouched&&Oracle(view)==original,"extent pre-write refusal preserves physical output and input");};
  for(std::size_t n:{std::size_t{0},std::size_t{1},output.size()-1})refuse(view,headers,budget,std::span<byte>(output).first(n),aggregate,identities,E::resource_exhausted);
  for(std::size_t n:{std::size_t{0},std::size_t{1},aggregate.size()-1})refuse(view,headers,budget,output,std::span<byte>(aggregate).first(n),identities,E::resource_exhausted);
  for(std::size_t n:{std::size_t{0},slots-1})refuse(view,headers,budget,output,aggregate,std::span<Uuid>(identities).first(n),E::resource_exhausted);
  refuse(view,headers,allowance-1,output,aggregate,identities,E::resource_exhausted);
  refuse(view,{},budget,output,aggregate,identities,E::invalid_request);
  auto altered=headers;altered[1].page_uuid=altered[0].page_uuid;refuse(view,altered,budget,output,aggregate,identities,E::invalid_identity);
  altered=headers;altered[1].page_number++;refuse(view,altered,budget,output,aggregate,identities,E::binding_mismatch);
  auto malformed=view;malformed.uuid={};refuse(malformed,headers,budget,output,aggregate,identities,E::invalid_record);
  const auto bytes=[](const void* p,std::size_t n){return std::span<byte>{static_cast<byte*>(const_cast<void*>(p)),n};};
  std::vector<std::span<byte>> inputs{bytes(&view,sizeof(view)),bytes(&object,sizeof(object)),bytes(headers.data(),headers.size()*sizeof(headers[0])),
    bytes(view.steps.data(),view.steps.size_bytes()),bytes(view.idempotency_key.data(),view.idempotency_key.size()),bytes(view.steps.back().idempotency_key.data(),view.steps.back().idempotency_key.size())};
  if(exact)inputs.push_back(bytes(view.normalized_request_bytes.data(),view.normalized_request_bytes.size()));
  for(auto input:inputs){
   refuse(view,headers,budget,input,aggregate,identities,E::invalid_workspace);
   refuse(view,headers,budget,output,input,identities,E::invalid_workspace);
   refuse(view,headers,budget,output,aggregate,{reinterpret_cast<Uuid*>(input.data()),slots},E::invalid_workspace);
  }
  refuse(view,headers,budget,output,output,identities,E::invalid_workspace);
  refuse(view,headers,budget,output,aggregate,{reinterpret_cast<Uuid*>(output.data()),slots},E::invalid_workspace);
  refuse(view,headers,budget,output,aggregate,{reinterpret_cast<Uuid*>(aggregate.data()),slots},E::invalid_workspace);
  refuse(view,headers,budget,output,aggregate,{reinterpret_cast<Uuid*>(output.data()+output.size()-1),slots},E::invalid_workspace);
  refuse(view,{reinterpret_cast<const d::NativeCommonPageHeader*>(reinterpret_cast<const byte*>(headers.data())+1),headers.size()},budget,output,aggregate,identities,E::invalid_workspace);
  refuse(view,{headers.data(),std::numeric_limits<std::size_t>::max()/sizeof(headers[0])+1},budget,output,aggregate,identities,E::invalid_workspace);
  malformed=view;malformed.steps={reinterpret_cast<const db::NativeManagementStepView*>(reinterpret_cast<const byte*>(view.steps.data())+1),1};
  refuse(malformed,headers,budget,output,aggregate,identities,E::invalid_workspace);
  Bytes spare(output.size()+32,0x91);malformed=view;malformed.idempotency_key={reinterpret_cast<const char*>(spare.data()+output.size()+16),1};
  refuse(malformed,headers,budget,spare,aggregate,identities,E::invalid_workspace);
  Check(std::all_of(spare.begin(),spare.end(),[](byte b){return b==0x91;}),"unused physical output suffix remains protected input");
  const auto accepted=call(view,headers,allowance,output,aggregate,identities);Check(accepted.ok()&&*accepted.root==*image.root,"exact conservative extent allowance succeeds");
  for(unsigned fault=1;fault<=5;++fault)for(unsigned digest=1;digest<=2*headers.size()+(exact?3u:2u);++digest){
   hash_fault=fault;hash_target=digest;hash_seen=0;hash_active=false;
   const auto r=call(view,headers,budget,output,aggregate,identities);const bool consumed=!hash_fault;hash_fault=0;
   Failed(r);Check(consumed&&r.error==E::hash_failure&&Oracle(view)==original,"all request aggregate and physical page hashes have prefix-free fixed failures");
   const auto retry=call(view,headers,budget,output,aggregate,identities);Check(retry.ok()&&*retry.root==*image.root,"exact bounded extent retry after provider failure");
  }
  EncodingParity(view,headers,*image.root,expected);
 }
}
void Test(){BorrowedBounds();EncodingBounds();for(unsigned profile=0;profile<5;++profile){const u64 cap=d::kCanonicalFilespacePageProfiles[profile].page_size_bytes-384;for(const u64 size:{u64{513},cap,cap+1,2*cap+17}){const auto o=Record(size);Good(o,Headers(o,profile));}Physical(profile);Physical(profile,true);}
 const auto o=Record(2*(8192-384)+17);CodecFaults(o,Headers(o,0));
 auto exact=o;exact.normalized_request_bytes.assign(8192+37,0xab);exact.normalized_request_sha256=Sha(exact.normalized_request_bytes);Good(exact,Headers(exact,0));CodecFaults(exact,Headers(exact,0));
 const auto tiny=Record(513);const auto image=db::EncodeNativeManagementExtent(tiny,Id(5000),Headers(tiny,0),budget);for(std::size_t at=0;at<image.pages[0].size();++at){auto bad=image.pages;bad[0][at]^=1;Failed(Decode(bad,*image.root,tiny.database_uuid,tiny.bootstrap_uuid,budget));}
 std::cout<<"PASS native management extent checks="<<checks<<" referenced_physical_bytes_only=true not_selected_authority_or_SQL_E2E=true\n";
}
}
int main(){try{Test();return 0;}catch(const std::exception& e){allocation_budget=-1;hash_fault=0;io_counting=false;std::cerr<<"FAIL management extent checks="<<checks<<" "<<e.what()<<'\n';return 1;}}

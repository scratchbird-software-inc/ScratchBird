// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_publication_plan.hpp"
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
namespace {long allocation_budget=-1;bool counting=false;unsigned long allocations=0;unsigned hash_fault=0,hash_target=1,hash_seen=0;bool hash_active=false;}
void* operator new(std::size_t n){if(counting)++allocations;if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){hash_active=hash_fault&&++hash_seen==hash_target;if(hash_active&&hash_fault==1){hash_fault=0;return nullptr;}return __real_EVP_MD_CTX_new();}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* m,ENGINE* e){if(hash_active&&hash_fault==2){hash_fault=0;return 0;}return __real_EVP_DigestInit_ex(c,m,e);}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* b,size_t n){if(hash_active&&hash_fault==3){hash_fault=0;return 0;}return __real_EVP_DigestUpdate(c,b,n);}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* b,unsigned int* n){if(hash_active&&hash_fault==4){hash_fault=0;return 0;}const int r=__real_EVP_DigestFinal_ex(c,b,n);if(hash_active&&hash_fault==5){hash_fault=0;*n=31;}return r;}
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* data,size_t bytes,unsigned char* out,unsigned int* size,const EVP_MD* md,ENGINE* engine) {
  const bool selected=hash_fault&&++hash_seen==hash_target;
  const auto mode=selected?hash_fault:0;
  if(selected)hash_fault=0;
  if(mode&&mode!=5)return 0;
  const auto result=__real_EVP_Digest(data,bytes,out,size,md,engine);
  if(mode==5)*size=31;
  return result;
}


namespace {
using namespace scratchbird::core::platform;
namespace db=scratchbird::storage::database;namespace d=scratchbird::storage::disk;namespace mga=scratchbird::transaction::mga;
using Bytes=std::vector<byte>;using E=db::NativePublicationPlanError;unsigned checks=0;
void Check(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
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
 std::copy_n("SBPPM001",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,640);Num(b,140,4,768);
 Put(b,144,p.object_uuid);Put(b,160,p.bootstrap_uuid);Put(b,176,p.timeline_uuid);Put(b,192,p.operation_uuid);Put(b,208,p.intent.initiator_uuid);Put(b,224,p.intent.request_context_uuid);Put(b,240,p.intent.policy_snapshot_uuid);Put(b,256,p.security_snapshot_uuid);
 std::copy(p.intent.normalized_request_sha256.begin(),p.intent.normalized_request_sha256.end(),b.begin()+272);std::copy(p.reservation_state_sha256.begin(),p.reservation_state_sha256.end(),b.begin()+304);
 Num(b,336,8,p.reserved_generation);Num(b,344,8,p.base_checkpoint_generation);Num(b,352,8,p.base_root_set_generation);Num(b,360,8,p.target_root_set_generation);
 Ref(b,368,p.base_checkpoint);Put(b,416,p.base_checkpoint_object_uuid);std::copy(p.base_checkpoint_sha256.begin(),p.base_checkpoint_sha256.end(),b.begin()+432);Ref(b,464,p.target_checkpoint);Put(b,512,p.target_checkpoint_object_uuid);std::copy(p.target_graph_sha256.begin(),p.target_graph_sha256.end(),b.begin()+528);
 if(p.previous_plan)Ref(b,560,*p.previous_plan);Put(b,608,p.previous_plan_object_uuid);std::copy(p.previous_plan_sha256.begin(),p.previous_plan_sha256.end(),b.begin()+624);
 Num(b,656,8,p.catalog_generation);Num(b,664,8,p.configuration_generation);Num(b,672,8,p.security_generation);Num(b,680,2,p.intent.initiator_kind);Num(b,682,2,1);Num(b,684,2,2);PlanSeal(b);return b;
}
void Failed(const db::NativePublicationPlanImage& r){Check(!r.ok()&&!r.plan&&r.bytes.empty()&&std::all_of(r.sha256.begin(),r.sha256.end(),[](byte v){return !v;}),"no failed plan prefix");}
void Bad(const db::NativePublicationPlan& p){Failed(db::EncodeNativePublicationPlan(p));const auto raw=Oracle(p);Failed(db::DecodeNativePublicationPlan(raw));}
struct Fixture {
 std::filesystem::path path;d::FileDevice device;std::vector<d::NativeFilespaceDevice> devices;u64 size,budget;
 Fixture(unsigned profile){
  char name[]="/tmp/sb-publication-plan.XXXXXX";Check(mkdtemp(name),"isolated fixture directory");path=std::filesystem::path(name)/"node";
  const auto& page=d::kCanonicalFilespacePageProfiles[profile];size=page.page_size_bytes;budget=256*size;
  db::NativeFilespaceInitializationRequest r;r.bootstrap={Id(1),Id(2),page.uuid,d::kNativeBootstrapIntegrityProfile,{},page.page_size_bytes,1,0,1,7};r.operation_uuid=Id(3);r.writer_uuid=Id(4);
  r.creator.transaction_uuid={UuidKind::transaction,Id(5)};r.creator.local_id=mga::MakeLocalTransactionId(1);r.creator.scope=mga::TransactionScope::local_node;r.creation_utc_millis=1789357072000ULL;r.total_pages=64;
  Check(device.Open(path.string(),d::FileOpenMode::create_new).ok(),"owned device");devices={{Id(2),page.uuid,&device}};
  Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(device,r,budget).ok(),"actual genesis");
 }
 ~Fixture(){device.Close();std::error_code ec;std::filesystem::remove(path,ec);std::filesystem::remove(path.parent_path(),ec);}
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
template<class F> void Allocations(F&& call){
 counting=true;allocations=0;const auto baseline=call();counting=false;const auto sites=allocations;Check(baseline,"allocation baseline");
 for(unsigned long n=0;n<sites;++n){allocation_budget=n;const auto succeeded=call();allocation_budget=-1;Check(!succeeded,"allocation failure withholds result");}
}
void Test(unsigned profile){
 Fixture f(profile);const auto prior=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),f.budget);Check(prior.ok(),"actual reserve base");
 db::NativePublicationIntent intent;intent.initiator_uuid=Id(4);intent.request_context_uuid=Id(90);intent.policy_snapshot_uuid=Id(91);intent.normalized_request_sha256.fill(92);intent.initiator_kind=4;
 auto reservation=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),f.devices,Id(2),*prior.snapshot,Id(300),f.budget,&intent);Check(reservation.ok(),"real retained intent lease");
 const auto unchanged=f.Read(0,64);const auto& s=reservation.lease->snapshot();const auto& w=s.watermark;
 auto old=db::DecodeNativeCheckpointRoot(f.Read(w.base_checkpoint.page_number));Check(old.ok(),"actual base checkpoint bytes");
 db::NativePublicationPlan p;p.header={static_cast<u32>(f.size),0x500,Id(1),Id(2),Id(301),30,1,0,w.header.page_size_profile_uuid};
 p.object_uuid=Id(302);p.bootstrap_uuid=w.bootstrap_uuid;p.timeline_uuid=w.timeline_uuid;p.operation_uuid=w.operation_uuid;p.security_snapshot_uuid=Id(303);p.intent=intent;
 p.reservation_state_sha256=s.state_sha256;p.base_checkpoint_sha256=w.base_checkpoint_sha256;p.target_graph_sha256.fill(1);p.reserved_generation=w.watermark;p.base_checkpoint_generation=w.base_checkpoint_generation;p.base_root_set_generation=w.base_root_set_generation;p.target_root_set_generation=2;
 p.base_checkpoint=w.base_checkpoint;p.target_checkpoint={Id(2),31,1,w.header.page_size_profile_uuid};p.base_checkpoint_object_uuid=p.target_checkpoint_object_uuid=w.base_checkpoint_object_uuid;p.catalog_generation=p.configuration_generation=p.security_generation=1;
 auto cp=*old.root;cp.header.page_uuid=Id(304);const auto target=Finish(p,cp);const auto encoded=db::EncodeNativePublicationPlan(p);
 const auto decoded=db::DecodeNativePublicationPlan(Oracle(p));Check(decoded.ok()&&decoded.sha256==encoded.sha256&&db::EncodeNativePublicationPlan(*decoded.plan).bytes==encoded.bytes,"plan decode preserves complete bytes");
 Check(encoded.sha256==Sha(encoded.bytes),"plan root reference hashes all stored bytes including embedded seal");
 Check(db::BindNativePublicationPlanToLease(p,*reservation.lease,target)==E::none,"actual lease preflight");
 auto wrong_plan_digest=*db::DecodeNativeCheckpointRoot(target).root;wrong_plan_digest.roots.back().sha256[0]^=1;const auto wrong_digest_image=db::EncodeNativeCheckpointRoot(wrong_plan_digest);
 Check(wrong_digest_image.ok()&&db::BindNativePublicationPlanToLease(p,*reservation.lease,wrong_digest_image.bytes)==E::binding_mismatch,"target must bind exact plan image seal");
 const std::array<Uuid db::NativePublicationPlan::*,7> ids{&db::NativePublicationPlan::object_uuid,&db::NativePublicationPlan::bootstrap_uuid,&db::NativePublicationPlan::timeline_uuid,&db::NativePublicationPlan::operation_uuid,&db::NativePublicationPlan::security_snapshot_uuid,&db::NativePublicationPlan::base_checkpoint_object_uuid,&db::NativePublicationPlan::target_checkpoint_object_uuid};
 for(auto member:ids)for(unsigned fault=0;fault<3;++fault){auto bad=p;if(!fault)bad.*member={};else if(fault==1)(bad.*member).bytes[6]=0x40;else (bad.*member).bytes[8]=0xc0;Bad(bad);}
 for(unsigned field=0;field<3;++field){auto bad=p;auto* id=field==0?&bad.intent.initiator_uuid:field==1?&bad.intent.request_context_uuid:&bad.intent.policy_snapshot_uuid;*id={};Bad(bad);}
 for(unsigned field=0;field<9;++field){auto bad=p;switch(field){case 0:bad.reserved_generation=1;break;case 1:bad.base_checkpoint_generation=0;break;case 2:bad.base_root_set_generation=0;break;case 3:bad.target_root_set_generation=1;break;case 4:bad.target_root_set_generation=3;break;case 5:bad.catalog_generation=0;break;case 6:bad.configuration_generation=0;break;case 7:bad.security_generation=0;break;case 8:bad.intent.initiator_kind=9;break;}Bad(bad);}
 for(unsigned field=0;field<4;++field){auto bad=p;(field==0?bad.intent.normalized_request_sha256:field==1?bad.reservation_state_sha256:field==2?bad.base_checkpoint_sha256:bad.target_graph_sha256).fill(0);Bad(bad);}
 for(unsigned at=686;at<768;++at)if(at<688||at>=720){auto b=Oracle(p);b[at]=1;PlanSeal(b);Failed(db::DecodeNativePublicationPlan(b));}
 for(auto at:{std::size_t{128},std::size_t{136},std::size_t{138},std::size_t{140},std::size_t{682},std::size_t{684},std::size_t{768},encoded.bytes.size()-1}){auto b=encoded.bytes;b[at]^=0x40;PlanSeal(b);Failed(db::DecodeNativePublicationPlan(b));}
 auto previous=p;previous.previous_plan=d::NativePageReference{Id(2),29,1,w.header.page_size_profile_uuid};previous.previous_plan_object_uuid=p.object_uuid;previous.previous_plan_sha256.fill(7);Check(db::DecodeNativePublicationPlan(Oracle(previous)).ok(),"previous plan image");previous.previous_plan_object_uuid={};Bad(previous);
 for(unsigned field=0;field<7;++field){auto bad=p;switch(field){case 0:bad.target_checkpoint=bad.base_checkpoint;break;case 1:bad.target_checkpoint.page_number=30;break;case 2:bad.target_checkpoint.page_number=std::numeric_limits<u64>::max();break;case 3:bad.target_checkpoint.page_size_profile_uuid=Id(999);break;case 4:bad.previous_plan_sha256.fill(1);break;case 5:bad.previous_plan=d::NativePageReference{Id(2),30,1,w.header.page_size_profile_uuid};bad.previous_plan_object_uuid=p.object_uuid;bad.previous_plan_sha256.fill(1);break;case 6:bad.previous_plan=d::NativePageReference{Id(2),29,1,w.header.page_size_profile_uuid};bad.previous_plan_object_uuid=p.object_uuid;break;}Bad(bad);}
 auto all_roots=*db::DecodeNativeCheckpointRoot(target).root;const auto plan_role=all_roots.roots.back();all_roots.roots.pop_back();
 const std::array<u32,5> extra_types{0x305,0x307,0x308,0x309,0x30b};
 for(unsigned i=0;i<5;++i){auto role=plan_role;role.role=11+i;role.page_type=extra_types[i];role.page.page_number=41+i;role.object_uuid=Id(401+i);all_roots.roots.push_back(role);}
 all_roots.roots.push_back(plan_role);all_roots.flags|=4;const auto sixteen=db::EncodeNativeCheckpointRoot(all_roots);
 const auto decoded_sixteen=db::DecodeNativeCheckpointRoot(sixteen.bytes);
 Check(sixteen.ok()&&decoded_sixteen.ok()&&decoded_sixteen.root->roots.size()==16,"all sixteen checkpoint roles roundtrip");
 Check(!db::ComputeNativePublicationTargetGraphDigest(sixteen.bytes).ok(),"local plan never supplies cluster authority");
 all_roots.roots.erase(all_roots.roots.begin()+13);all_roots.flags&=~u64{4};const auto fifteen=db::EncodeNativeCheckpointRoot(all_roots);
 Check(fifteen.ok()&&db::ComputeNativePublicationTargetGraphDigest(fifteen.bytes).ok(),"all fifteen local roles project");
 all_roots.roots.back().role=17;Check(!db::EncodeNativeCheckpointRoot(all_roots).ok(),"unknown checkpoint role refused");
 for(unsigned field=0;field<14;++field){auto changed=p;switch(field){case 0:changed.operation_uuid=Id(305);break;case 1:changed.bootstrap_uuid=Id(306);break;case 2:changed.timeline_uuid=Id(307);break;case 3:changed.intent.initiator_uuid=Id(308);break;case 4:changed.intent.request_context_uuid=Id(309);break;case 5:changed.intent.policy_snapshot_uuid=Id(310);break;case 6:changed.intent.normalized_request_sha256[0]^=1;break;case 7:changed.reservation_state_sha256[0]^=1;break;case 8:changed.base_checkpoint_sha256[0]^=1;break;case 9:changed.intent.initiator_kind=3;break;case 10:changed.reserved_generation=3;break;case 11:changed.header.database_uuid=Id(311);break;case 12:changed.base_checkpoint.page_number=28;break;case 13:changed.base_checkpoint.page_generation=2;break;}
  const auto forged=Finish(changed,cp);Check(db::BindNativePublicationPlanToLease(changed,*reservation.lease,forged)==E::binding_mismatch,"resealed plan cannot change retained reservation");
 }
 auto changed=cp;changed.creator_operation_uuid=Id(300);changed.creator_transaction_uuid={};changed.creator_local_transaction_id=0;changed.header.page_number=31;changed.checkpoint_generation=changed.root_set_generation=2;changed.predecessor=p.base_checkpoint;changed.predecessor_sha256=p.base_checkpoint_sha256;
 const auto no_plan=db::EncodeNativeCheckpointRoot(changed);Check(no_plan.ok()&&!db::ComputeNativePublicationTargetGraphDigest(no_plan.bytes).ok(),"operation checkpoint without plan cannot project");
 for(unsigned mode=1;mode<=5;++mode)for(unsigned digest=1;digest<=2;++digest){hash_fault=mode;hash_target=digest;hash_seen=0;Failed(db::EncodeNativePublicationPlan(p));hash_fault=0;hash_active=false;
  hash_fault=mode;hash_target=digest;hash_seen=0;Failed(db::DecodeNativePublicationPlan(encoded.bytes));hash_fault=0;hash_active=false;}
 for(unsigned mode=1;mode<=5;++mode){
  for(unsigned digest=1;digest<=3;++digest){hash_fault=mode;hash_target=digest;hash_seen=0;const auto failed=db::ComputeNativePublicationTargetGraphDigest(target);hash_fault=0;hash_active=false;
   Check(hash_seen>=digest&&failed.error==E::hash_failure&&std::all_of(failed.sha256.begin(),failed.sha256.end(),[](byte v){return !v;}),"every graph hash failure has no digest prefix");}
  for(unsigned digest=1;digest<=7;++digest){hash_fault=mode;hash_target=digest;hash_seen=0;const auto failed=db::BindNativePublicationPlanToLease(p,*reservation.lease,target);hash_fault=0;hash_active=false;
   Check(hash_seen>=digest&&failed==E::hash_failure,"every lease preflight hash failure refuses binding");}
 }
 Allocations([&]{return db::EncodeNativePublicationPlan(p).ok();});Allocations([&]{return db::DecodeNativePublicationPlan(encoded.bytes).ok();});
 Allocations([&]{return db::ComputeNativePublicationTargetGraphDigest(target).ok();});Allocations([&]{return db::BindNativePublicationPlanToLease(p,*reservation.lease,target)==E::none;});
 Check(f.Read(0,64)==unchanged,"preflight never writes plan or checkpoint or reports execution");
 reservation.lease.reset();
}
}
int main(){try{for(unsigned p=0;p<5;++p)Test(p);std::cout<<"PASS native publication plan checks="<<checks<<" preflight_only=true\n";}catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL plan "<<checks<<": "<<e.what()<<'\n';return 1;}}

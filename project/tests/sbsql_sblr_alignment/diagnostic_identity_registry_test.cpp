// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_diagnostic_identity_registry.hpp"
#include "core/diagnostics/canonical_diagnostic_catalog.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <new>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <vector>
#ifdef __linux__
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __linux__
namespace fault {
bool armed=false,crash=false,partial=false,partial_pending=false;
unsigned kind=0,nth=0;unsigned calls[4]{};
const char* journal=nullptr;const char* directory=nullptr;
bool Target(int fd){
  if(!armed)return false;
  char link[64],path[4096];const int n=std::snprintf(link,sizeof link,"/proc/self/fd/%d",fd);
  if(n<=0||static_cast<std::size_t>(n)>=sizeof link)return false;
  const auto bytes=::readlink(link,path,sizeof path);
  if(bytes<0)return false;
  const auto value=std::string_view(path,static_cast<std::size_t>(bytes));
  return value==journal||value==directory;
}
bool Hit(unsigned operation,int fd){
  if(!Target(fd))return false;
  const auto at=++calls[operation];
  if(kind!=operation||at!=nth)return false;
  if(crash)_exit(86);
  errno=EIO;return true;
}
void Arm(unsigned operation,unsigned at,const std::string& path,const std::string& root,bool die=false,bool short_write=false){
  kind=operation;nth=at;journal=path.c_str();directory=root.c_str();crash=die;partial=short_write;partial_pending=false;
  std::fill(std::begin(calls),std::end(calls),0);armed=true;
}
}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_pread(int fd,void* b,size_t n,off_t at){
  if(fault::Hit(1,fd))return -1;
  return __real_pread(fd,b,n,at);
}
extern "C" ssize_t __wrap_pwrite(int fd,const void* b,size_t n,off_t at){
  if(fault::partial_pending&&fault::Target(fd)){errno=EIO;return -1;}
  if(fault::Hit(2,fd)){
    if(fault::partial&&n>1){fault::partial_pending=true;return __real_pwrite(fd,b,n/2,at);}
    return -1;
  }
  return __real_pwrite(fd,b,n,at);
}
extern "C" int __wrap_fsync(int fd){if(fault::Hit(3,fd))return -1;return __real_fsync(fd);}
#endif
namespace allocation_fault {
thread_local long remaining=-1;
thread_local std::size_t calls=0;
thread_local bool track=false, injected=false;
}
void* operator new(std::size_t n){
  if(allocation_fault::track)++allocation_fault::calls;
  if(allocation_fault::remaining==0){
    allocation_fault::remaining=-1;allocation_fault::injected=true;throw std::bad_alloc();
  }
  if(allocation_fault::remaining>0)--allocation_fault::remaining;
  if(void* p=std::malloc(n?n:1))return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace api=scratchbird::engine::internal_api;
namespace catalog=scratchbird::core::diagnostics;
namespace hash=scratchbird::core::hash;
using Bytes=std::vector<std::uint8_t>;
using Row=api::SblrDiagnosticIdentityRowV1;
using Snapshot=api::SblrDiagnosticIdentitySnapshotV1;
using Uuid=api::SblrDiagnosticIdentityUuidV1;
std::size_t checks=0;
void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
std::uint64_t Get(const Bytes& b,std::size_t at,unsigned width){
  Check(at<=b.size()&&width<=b.size()-at,"oracle read extent");
  std::uint64_t n=0;for(unsigned i=0;i<width;++i)n|=std::uint64_t(b[at+i])<<(8*i);return n;
}
void Put(Bytes& b,std::size_t at,std::uint64_t n,unsigned width){
  for(unsigned i=0;i<width;++i)b.at(at+i)=static_cast<std::uint8_t>(n>>(8*i));
}
template<class A> void Copy(Bytes& b,std::size_t at,const A&a){std::copy(a.begin(),a.end(),b.begin()+at);}
auto Digest(std::string_view domain,const Bytes& data){
  Bytes material(domain.begin(),domain.end());material.insert(material.end(),data.begin(),data.end());
  const auto hash=hash::ComputeSha256Digest(material);Check(hash.ok()&&hash.digest_bytes==32,"oracle SHA256");return hash.digest;
}
std::string Identity(){
  const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  auto id=scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::object,now);
  Check(id.ok(),"fixture identity");return scratchbird::core::uuid::UuidToString(id.value.value);
}
Uuid Raw(const std::string& text){auto id=scratchbird::core::uuid::ParseUuid(text);Check(id.ok(),"fixture uuid");return id.value.bytes;}
Bytes Read(const std::filesystem::path& p){
  std::ifstream f(p,std::ios::binary);Check(bool(f),"open fixture read");
  return Bytes(std::istreambuf_iterator<char>(f),{});
}
void Write(const std::filesystem::path& p,const Bytes& b){
  std::ofstream f(p,std::ios::binary|std::ios::trunc);Check(bool(f),"open fixture write");
  f.write(reinterpret_cast<const char*>(b.data()),b.size());f.close();Check(bool(f),"fixture write");
}
void Rehash(Bytes& b){
  const auto frame=Get(b,8,8);Bytes material(b.begin(),b.begin()+128);
  material.insert(material.end(),b.begin()+160,b.begin()+frame);
  auto digest=Digest("ScratchBird.DiagnosticIdentityRegistryFrame.V1",material);
  Copy(b,128,digest);Copy(b,frame+32,digest);
}
Bytes Oracle(const Snapshot& s){
  Bytes b(160);std::copy_n("DIDR",4,b.begin());Put(b,4,1,2);Put(b,6,160,2);
  Copy(b,16,s.snapshot_uuid);Put(b,32,s.generation,8);Copy(b,40,s.database_uuid);
  Put(b,56,s.rows.size(),4);Copy(b,64,s.source_sha256);
  for(const auto&r:s.rows){
    const auto at=b.size();b.resize(at+((76+r.canonical_code.size()+3)&~std::size_t(3)),0);
    Copy(b,at,r.diagnostic_uuid);Put(b,at+16,r.diagnostic_generation,8);Put(b,at+24,r.precedence_ordinal,4);
    b[at+28]=r.severity_code;b[at+29]=r.redaction_class;Put(b,at+32,r.maximum_safe_field_count,4);
    Bytes material(b.begin()+at,b.begin()+at+40);
    auto digest=Digest("ScratchBird.DiagnosticIdentityRegistryRow.V1",material);Copy(b,at+40,digest);
    Put(b,at+72,r.canonical_code.size(),2);std::copy(r.canonical_code.begin(),r.canonical_code.end(),b.begin()+at+76);
  }
  auto at=b.size();Put(b,8,at,8);b.resize(at+64,0);
  std::copy_n("DICP",4,b.begin()+at);Put(b,at+4,1,2);Put(b,at+6,64,2);
  Copy(b,at+8,s.snapshot_uuid);Put(b,at+24,s.generation,8);Rehash(b);return b;
}
Snapshot ReadSnapshot(const Bytes& b){
  Snapshot s;Check(b.size()>=224,"oracle frame minimum");
  std::copy_n(b.begin()+16,16,s.snapshot_uuid.begin());s.generation=Get(b,32,8);
  std::copy_n(b.begin()+40,16,s.database_uuid.begin());
  std::copy_n(b.begin()+64,32,s.source_sha256.begin());
  std::copy_n(b.begin()+128,32,s.evidence_sha256.begin());
  const auto count=Get(b,56,4);Check(count>0&&count<=4096,"oracle row count");
  std::size_t at=160;
  for(std::size_t i=0;i<count;++i){
    Check(at<=b.size()&&76<=b.size()-at,"oracle row extent");Row r;
    std::copy_n(b.begin()+at,16,r.diagnostic_uuid.begin());r.diagnostic_generation=Get(b,at+16,8);
    r.precedence_ordinal=Get(b,at+24,4);r.severity_code=b[at+28];r.redaction_class=b[at+29];
    r.maximum_safe_field_count=Get(b,at+32,4);std::copy_n(b.begin()+at+40,32,r.row_identity_sha256.begin());
    const auto length=Get(b,at+72,2);Check(length>0&&length<=512&&length<=b.size()-at-76,"oracle code extent");
    r.canonical_code.assign(reinterpret_cast<const char*>(b.data()+at+76),length);
    s.rows.push_back(std::move(r));at+=(76+length+3)&~std::size_t(3);
  }
  Check(at+64==b.size(),"oracle footer extent");
  auto exact=Oracle(s);std::copy_n(b.begin()+96,32,exact.begin()+96);Rehash(exact);
  Check(exact==b,"independent journal bytes");return s;
}
void CheckProjection(const api::SblrDiagnosticIdentityResultV1& projected,const Snapshot& complete){
  Check(projected.ok&&projected.snapshot.snapshot_uuid==complete.snapshot_uuid&&
        projected.snapshot.generation==complete.generation,"projection snapshot binding");
  std::size_t visible=0;
  for(const auto& row:complete.rows){
    if(row.severity_code==9||row.severity_code==11||row.severity_code==12||row.severity_code==13)continue;
    Check(visible<projected.snapshot.rows.size(),"projection omitted allowed row");
    const auto& actual=projected.snapshot.rows[visible++];
    Check(actual.canonical_code==row.canonical_code&&actual.diagnostic_uuid==row.diagnostic_uuid&&
          actual.diagnostic_generation==row.diagnostic_generation&&actual.precedence_ordinal==row.precedence_ordinal&&
          actual.row_identity_sha256==row.row_identity_sha256,"projection identity or rank changed");
  }
  Check(visible==projected.snapshot.rows.size(),"restricted row disclosed");
}
api::EngineRequestContext Context(const std::string& path){
  api::EngineRequestContext c;c.database_path=path;c.database_uuid.canonical=Identity();
  c.principal_uuid.canonical=Identity();c.session_uuid.canonical=Identity();
  c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;
  c.security_epoch=1;c.catalog_generation_id=1;return c;
}
void Visibility(api::EngineRequestContext c){
  auto mask=[&]{return api::SblrDiagnosticIdentityVisibilityMaskV1(c);};
  Check(mask()==0x05fe,"ordinary mask");
  c.trace_tags={"right:READ_DIAGNOSTIC_DETAIL","right:AUDIT_READ"};
  Check(mask()==0x05fe,"trace labels disclosed metadata");
  auto& a=c.authorization_context;a.present=true;a.principal_uuid=c.principal_uuid;
  a.security_epoch=1;a.policy_epoch=1;a.catalog_generation_id=1;
  a.effective_subjects.push_back({c.principal_uuid,"principal"});
  api::EngineMaterializedAuthorizationGrant g;g.grant_uuid.canonical=Identity();g.subject_uuid=c.principal_uuid;
  g.subject_kind="principal";g.target_uuid=c.database_uuid;g.right="READ_DIAGNOSTIC_DETAIL";g.security_epoch=1;
  a.grants.push_back(g);Check(mask()==0x17fe,"detail grant mask");
  g.right="AUDIT_READ";a.grants.push_back(g);Check(mask()==0x1ffe,"audit grant mask");
  g.deny=true;a.grants.push_back(g);Check(mask()==0x17fe,"explicit audit deny");a.grants.pop_back();
  g.right="READ_DIAGNOSTIC_DETAIL";a.grants.push_back(g);Check(mask()==0x05fe,"detail deny overrides");a.grants.pop_back();
  a.principal_uuid.canonical=Identity();Check(mask()==0x05fe,"principal mismatch");a.principal_uuid=c.principal_uuid;
  a.security_epoch=2;Check(mask()==0x05fe,"expired epoch");a.security_epoch=1;
  api::EngineMaterializedAuthorizationPolicy policy;policy.subject_uuid=c.principal_uuid;
  policy.subject_kind="principal";policy.target_uuid=c.database_uuid;policy.right="READ_DIAGNOSTIC_DETAIL";
  policy.requires_runtime_recheck=true;a.policies.push_back(policy);
  Check(mask()==0x05fe,"runtime pending disclosed metadata");a.policies.back().requires_runtime_recheck=false;
  a.policies.back().deny=true;Check(mask()==0x05fe,"policy deny disclosed metadata");a.policies.clear();
  a.grants[0].target_uuid.canonical=Identity();Check(mask()==0x05fe,"cross-node grant");
  c.security_context_present=false;Check(mask()==0,"unauthenticated visibility");
}
#ifdef __linux__
void IoFaults(const api::EngineRequestContext& c,const std::string& path,const std::string& root,const Bytes& past){
  Write(path,past);fault::Arm(0,0,path,root);
  auto baseline=api::LoadSblrDiagnosticIdentitySnapshotV1(c);fault::armed=false;
  const std::array<unsigned,4> calls{0,fault::calls[1],fault::calls[2],fault::calls[3]};
  Check(baseline.ok&&calls[1]>0&&calls[2]==2&&calls[3]>=3,"actual read/write/sync fault boundaries absent");
  unsigned exercised=0,crashed=0;
  for(unsigned kind=1;kind<=3;++kind)for(unsigned at=1;at<=calls[kind];++at){
    for(unsigned mode=0;mode<(kind==2?3u:2u);++mode){
      Write(path,past);const bool die=mode==1,partial=mode==2;
      if(die){
        const auto pid=fork();Check(pid>=0,"fault child fork");
        if(pid==0){fault::Arm(kind,at,path,root,true);(void)api::LoadSblrDiagnosticIdentitySnapshotV1(c);_exit(87);}
        int status=0;Check(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"crash boundary not reached");++crashed;
      }else{
        fault::Arm(kind,at,path,root,false,partial);auto refused=api::LoadSblrDiagnosticIdentitySnapshotV1(c);fault::armed=false;
        Check(fault::calls[kind]>=at&&!refused.ok,"I/O failure published success");
        Check(refused.diagnostic.code=="SBLR.EXECUTION_FAILED","I/O failure classification");++exercised;
      }
      const auto evidence=Read(path);
      Check(evidence.size()>=past.size()&&std::equal(past.begin(),past.end(),evidence.begin()),"fault rewrote prior commit");
      const bool no_append=evidence==past;
      const bool committed=evidence.size()>past.size()+64&&std::equal(evidence.end()-64,evidence.end()-60,"DICP");
      auto recovered=api::LoadSblrDiagnosticIdentitySnapshotV1(c);
      Check(recovered.ok==(no_append||committed),"fault recovery accepted torn evidence or rejected complete commit");
      if(recovered.ok)Check(recovered.snapshot.generation==2,"recovery epoch changed");
      else Check(Read(path)==evidence&&recovered.diagnostic.code=="SBLR.ERROR_VECTOR.STALE","torn evidence silently repaired");
    }
  }
  std::cout<<"diagnostic_registry_io_failures="<<exercised<<" crash_boundaries="<<crashed<<'\n';
}
#endif
void AllocationFaults(const api::EngineRequestContext& c,const std::string& path,const Bytes& original){
  allocation_fault::calls=0;allocation_fault::track=true;
  auto baseline=api::LoadSblrDiagnosticIdentitySnapshotV1(c);
  allocation_fault::track=false;const auto count=allocation_fault::calls;
  Check(baseline.ok&&count>0,"allocation baseline");
  for(std::size_t at=0;at<count;++at){
    api::SblrDiagnosticIdentityResultV1 result;
    allocation_fault::remaining=static_cast<long>(at);allocation_fault::injected=false;
    try {result=api::LoadSblrDiagnosticIdentitySnapshotV1(c);}
    catch(const std::exception& error){allocation_fault::remaining=-1;
      std::cerr<<"allocation_index="<<at<<" exception="<<error.what()<<'\n';
      throw std::runtime_error("registry leaked allocation exception");}
    allocation_fault::remaining=-1;
    if(!allocation_fault::injected||result.ok)std::cerr<<"allocation_index="<<at<<" injected="<<allocation_fault::injected<<" ok="<<result.ok<<'\n';
    Check(allocation_fault::injected&&!result.ok,"allocation failure published success");
    Check(result.snapshot.rows.empty()&&result.snapshot.generation==0,"allocation failure published snapshot");
    const auto recovered=api::LoadSblrDiagnosticIdentitySnapshotV1(c);
    if(!recovered.ok)std::cerr<<"allocation_recovery_index="<<at<<" key="<<recovered.diagnostic.message_key<<'\n';
    Check(recovered.ok,"allocation failure poisoned a later registry load");
  }
  Check(Read(path)==original,"allocation fault changed durable registry");
  std::cout<<"diagnostic_registry_allocation_failures="<<count<<'\n';
}
void BootstrapAllocationFaults(const api::EngineRequestContext& c,const std::string& path,const Bytes& original){
  Check(std::filesystem::remove(path),"bootstrap fixture reset");
  allocation_fault::calls=0;allocation_fault::track=true;
  auto baseline=api::LoadSblrDiagnosticIdentitySnapshotV1(c);
  allocation_fault::track=false;const auto count=allocation_fault::calls;
  Check(baseline.ok&&count>0,"bootstrap allocation baseline");
  for(std::size_t at=0;at<count;++at){
    std::error_code ec;std::filesystem::remove(path,ec);Check(!ec,"bootstrap isolated fixture reset");
    api::SblrDiagnosticIdentityResultV1 result;
    allocation_fault::remaining=static_cast<long>(at);allocation_fault::injected=false;
    try {result=api::LoadSblrDiagnosticIdentitySnapshotV1(c);}
    catch(const std::exception& error){allocation_fault::remaining=-1;
      std::cerr<<"bootstrap_allocation_index="<<at<<" exception="<<error.what()<<'\n';
      throw std::runtime_error("bootstrap leaked allocation exception");}
    allocation_fault::remaining=-1;
    if(!allocation_fault::injected||result.ok)std::cerr<<"bootstrap_allocation_index="<<at<<" injected="<<allocation_fault::injected<<" ok="<<result.ok<<'\n';
    Check(allocation_fault::injected&&!result.ok,"bootstrap allocation failure published success");
    Check(result.snapshot.rows.empty()&&result.snapshot.generation==0,"bootstrap failure published snapshot");
    const bool exists=std::filesystem::exists(path);
    const auto evidence=exists?Read(path):Bytes{};
    const auto recovered=api::LoadSblrDiagnosticIdentitySnapshotV1(c);
    if(recovered.ok)Check(recovered.snapshot.generation==1,"bootstrap recovery generation");
    else {
      Check(exists&&recovered.diagnostic.code=="SBLR.ERROR_VECTOR.STALE","bootstrap failure retained unowned lock");
      Check(Read(path)==evidence,"bootstrap recovery recreated torn evidence");
    }
  }
  Write(path,original);
  std::cout<<"diagnostic_registry_bootstrap_allocation_failures="<<count<<'\n';
}
template<class Operation>
void RefusalAllocationFaults(Operation operation,const char* expected_code,const char* label){
  allocation_fault::calls=0;allocation_fault::track=true;
  auto baseline=operation();
  allocation_fault::track=false;const auto count=allocation_fault::calls;
  Check(!baseline.ok&&baseline.diagnostic.code==expected_code&&count>0,"refusal allocation baseline");
  for(std::size_t at=0;at<count;++at){
    api::SblrDiagnosticIdentityResultV1 result;
    allocation_fault::remaining=static_cast<long>(at);allocation_fault::injected=false;
    try {result=operation();}
    catch(const std::exception& error){allocation_fault::remaining=-1;
      std::cerr<<"refusal="<<label<<" allocation_index="<<at<<" exception="<<error.what()<<'\n';
      throw std::runtime_error("refusal leaked allocation exception");}
    allocation_fault::remaining=-1;
    Check(allocation_fault::injected&&!result.ok,"refusal allocation published success");
    Check(result.snapshot.rows.empty()&&result.snapshot.generation==0,"refusal published partial snapshot");
  }
  std::cout<<"diagnostic_registry_refusal_allocation_failures "<<label<<'='<<count<<'\n';
}
int main(int argc,char** argv){
  try {
    if(argc==6 && std::string_view(argv[1])=="--restart"){
      api::EngineRequestContext c;c.database_path=argv[2];c.database_uuid.canonical=argv[3];
      c.principal_uuid.canonical=argv[4];c.session_uuid.canonical=argv[5];
      c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;
      const auto before=Read(c.database_path+".sb.sblr_diagnostic_identity_registry.v1");
      auto loaded=api::LoadSblrDiagnosticIdentitySnapshotV1(c);
      Check(loaded.ok&&loaded.snapshot.generation==1,"fresh-process load");
      CheckProjection(loaded,ReadSnapshot(before));
      Check(Read(c.database_path+".sb.sblr_diagnostic_identity_registry.v1")==before,"restart journal changed");return 0;
    }
    const auto root=std::filesystem::temp_directory_path()/("sb_diag_registry_"+Identity());
    Check(std::filesystem::create_directory(root),"fixture directory");
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove_all(path,e);}} cleanup{root};
    const auto base=(root/"node").string();Write(base,Bytes{0});
    auto c=Context(base);const auto path=base+".sb.sblr_diagnostic_identity_registry.v1";
    auto denied=c;denied.security_context_present=false;
    Check(!api::LoadSblrDiagnosticIdentitySnapshotV1(denied).ok && !std::filesystem::exists(path),"unauthenticated bootstrap");
    denied=c;denied.statement_metadata_snapshot_engine_owned=false;
    Check(!api::LoadSblrDiagnosticIdentitySnapshotV1(denied).ok && !std::filesystem::exists(path),"unowned bootstrap");
    auto initial=api::LoadSblrDiagnosticIdentitySnapshotV1(c);
    if(!initial.ok)std::cerr<<initial.diagnostic.code<<':'<<initial.diagnostic.message_key<<'\n';
    Check(initial.ok,"initial normalization");
    const auto original=Read(path);const auto s=ReadSnapshot(original);
    const auto definitions=catalog::CanonicalDiagnosticCodeCatalog();
    CheckProjection(initial,s);
    // Complete durable normalization includes the real internal-only row;
    // the authenticated public cohort excludes it without renumbering.
    Check(s.rows.size()==definitions.size && s.rows.size()>1000,"representative registry substituted");
    Check(s.generation==1&&s.source_sha256==catalog::CanonicalDiagnosticCodeSourceSha256(),"snapshot source");
    Check(initial.diagnostic.code.empty()&&!initial.diagnostic.error,"invented success diagnostic");
    std::set<Uuid> ids{s.snapshot_uuid,s.database_uuid};
    for(std::size_t i=0;i<s.rows.size();++i){
      const auto&r=s.rows[i];Check(r.canonical_code==definitions.data[i].code,"code mapping");
      Check(r.severity_code==static_cast<std::uint8_t>(definitions.data[i].severity),"severity mapping");
      Check(r.diagnostic_generation==1&&r.precedence_ordinal==i+1&&r.maximum_safe_field_count==145,"row metadata");
      Check((r.diagnostic_uuid[6]&0xf0)==0x70&&(r.diagnostic_uuid[8]&0xc0)==0x80&&ids.insert(r.diagnostic_uuid).second,"row identity");
    }
    Check(original==Oracle(s),"independent journal bytes");
    const auto internal=std::find_if(s.rows.begin(),s.rows.end(),[](const Row& r){return r.severity_code==13;});
    Check(internal!=s.rows.end(),"actual internal row fixture absent");
    auto internal_lookup=api::LookupSblrDiagnosticIdentityV1(c,s,internal->diagnostic_uuid,1);
    Check(!internal_lookup.ok&&internal_lookup.diagnostic.code=="SECURITY.ACCESS_DENIED","internal identity disclosed");
    auto again=api::LoadSblrDiagnosticIdentitySnapshotV1(c);
    Check(again.ok&&again.snapshot.snapshot_uuid==s.snapshot_uuid&&Read(path)==original,"unchanged source manufactured epoch");
    std::vector<std::future<api::SblrDiagnosticIdentityResultV1>> readers;
    for(unsigned i=0;i<8;++i)readers.push_back(std::async(std::launch::async,[c]{return api::LoadSblrDiagnosticIdentitySnapshotV1(c);}));
    for(auto& future:readers){auto r=future.get();Check(r.ok&&r.snapshot.snapshot_uuid==s.snapshot_uuid,"concurrent identity divergence");}
    Check(Read(path)==original,"concurrent reads appended evidence");
#ifdef __linux__
    auto pid=fork();Check(pid>=0,"fork");
    if(pid==0){execl(argv[0],argv[0],"--restart",base.c_str(),c.database_uuid.canonical.c_str(),c.principal_uuid.canonical.c_str(),c.session_uuid.canonical.c_str(),nullptr);_exit(127);}
    int status=0;Check(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh-process restart");
#endif
    auto found=api::LookupSblrDiagnosticIdentityV1(c,s,s.rows.back().diagnostic_uuid,1);
    Check(found.ok&&found.row.precedence_ordinal==s.rows.size(),"non-prefix lookup");
    auto absent=Raw(Identity());auto hidden=api::LookupSblrDiagnosticIdentityV1(c,s,absent,1);
    auto wrong_generation=api::LookupSblrDiagnosticIdentityV1(c,s,s.rows.back().diagnostic_uuid,2);
    Check(!hidden.ok&&!wrong_generation.ok&&hidden.diagnostic.code=="SECURITY.ACCESS_DENIED"&&hidden.diagnostic.message_key==wrong_generation.diagnostic.message_key,"existence disclosure");
    auto frozen=s;frozen.generation++;
    Check(api::LookupSblrDiagnosticIdentityV1(c,frozen,s.rows[0].diagnostic_uuid,1).diagnostic.code=="SBLR.ERROR_VECTOR.STALE","stale snapshot");
    Visibility(c);
    auto Refuse=[&](const Bytes& bad,const char* why){
      Write(path,bad);auto r=api::LoadSblrDiagnosticIdentitySnapshotV1(c);
      Check(!r.ok,why);Check(Read(path)==bad,"invalid authority overwritten");
    };
    Refuse(Bytes{'S','\t','o','l','d','\n'},"old UUID text accepted");
    for(std::size_t end=0;end<240;++end)Refuse(Bytes(original.begin(),original.begin()+end),"truncated header/first row");
    for(std::size_t end=original.size()-65;end<original.size();++end)Refuse(Bytes(original.begin(),original.begin()+end),"torn evidence/footer");
    for(auto at:{0u,4u,6u,8u,16u,32u,40u,56u,60u,64u,96u,128u,160u,176u,184u,188u,189u,190u,192u,196u,200u,232u,234u,236u}){
      auto bad=original;bad.at(at)^=1;Refuse(bad,"corrupt frame accepted");
    }
    auto bad=original;bad.push_back(0);Refuse(bad,"trailing evidence accepted");
    // Rehash contradictory structures to distinguish semantics from checksum checking.
    bad=original;Put(bad,60,1,4);Rehash(bad);Refuse(bad,"reserved header accepted");
    bad=original;Put(bad,56,s.rows.size()-1,4);Rehash(bad);Refuse(bad,"incomplete registry accepted");
    auto altered=s;altered.rows[0].severity_code=altered.rows[0].severity_code==3?2:3;
    altered.rows[0].redaction_class=1;Refuse(Oracle(altered),"same-source metadata contradiction");
    altered=s;altered.rows[0].diagnostic_uuid=altered.rows[1].diagnostic_uuid;
    Refuse(Oracle(altered),"duplicate row identity");
    altered=s;altered.database_uuid=Raw(Identity());Refuse(Oracle(altered),"wrong database binding");
    // Genuine source transition oracle: a previous catalog lacking its final
    // registration and with one different row's metadata. The real normalizer
    // must append, retain identities, advance only changed generations, and
    // allocate the new registration, never overwrite the previous bytes.
    auto historical=s;historical.source_sha256[0]^=1;historical.rows.pop_back();
    historical.rows[0].severity_code=historical.rows[0].severity_code==3?2:3;
    historical.rows[0].redaction_class=1;auto past=Oracle(historical);Write(path,past);
    auto upgraded=api::LoadSblrDiagnosticIdentitySnapshotV1(c);Check(upgraded.ok,"source transition");
    Check(upgraded.snapshot.generation==2&&upgraded.snapshot.snapshot_uuid!=s.snapshot_uuid,"snapshot generation");
    auto appended=Read(path);auto upgraded_full=ReadSnapshot(Bytes(appended.begin()+past.size(),appended.end()));
    CheckProjection(upgraded,upgraded_full);
    Check(upgraded_full.rows[0].diagnostic_uuid==s.rows[0].diagnostic_uuid&&upgraded_full.rows[0].diagnostic_generation==2,"changed metadata generation");
    for(std::size_t i=1;i<historical.rows.size();++i)
      Check(upgraded_full.rows[i].diagnostic_uuid==s.rows[i].diagnostic_uuid&&upgraded_full.rows[i].diagnostic_generation==1,"unchanged identity generation");
    Check(appended.size()>past.size()&&std::equal(past.begin(),past.end(),appended.begin()),"transition rewrote committed prefix");
    auto recovered=api::LoadSblrDiagnosticIdentitySnapshotV1(c);
    Check(recovered.ok&&recovered.snapshot.snapshot_uuid==upgraded.snapshot.snapshot_uuid&&Read(path)==appended,"successor recovery");
#ifdef __linux__
    IoFaults(c,path,root.string(),past);
#endif
    auto broken_chain=appended;broken_chain[past.size()+96]^=1;Refuse(broken_chain,"broken predecessor hash");
    // An old registration may not silently disappear.
    historical=s;historical.source_sha256[0]^=1;auto extra=historical.rows.back();
    extra.canonical_code="zzzz.TEST_ONLY_PRIOR_REGISTRATION";extra.diagnostic_uuid=Raw(Identity());extra.precedence_ordinal++;
    historical.rows.push_back(extra);Refuse(Oracle(historical),"removed registration silently discarded");
    Write(path,original);
    AllocationFaults(c,path,original);
    BootstrapAllocationFaults(c,path,original);
    auto unauthenticated=c;unauthenticated.security_context_present=false;
    RefusalAllocationFaults([&]{return api::LoadSblrDiagnosticIdentitySnapshotV1(unauthenticated);},
                           "SECURITY.ACCESS_DENIED","unauthenticated");
    RefusalAllocationFaults([&]{return api::LookupSblrDiagnosticIdentityV1(c,s,absent,1);},
                           "SECURITY.ACCESS_DENIED","hidden");
    std::cout<<"PASS diagnostic_identity_registry checks="<<checks<<" rows="<<s.rows.size()<<'\n';
  }catch(const std::exception& e){std::cerr<<"FAIL diagnostic_identity_registry "<<e.what()<<'\n';return 1;}
}

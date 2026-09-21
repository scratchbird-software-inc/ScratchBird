// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_source_map_descriptor_registry.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <thread>
#if defined(SB_SOURCE_MAP_FAULTS)
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace scratchbird::engine::internal_api;
using Bytes=std::vector<std::uint8_t>;
static std::atomic<long> fail_allocation{-1};
static std::atomic<unsigned> allocation_failures{0};
void* operator new(std::size_t n) {
  const auto remaining=fail_allocation.load();
  if(remaining>=0 && fail_allocation.fetch_sub(1)==0) {
    ++allocation_failures;
    throw std::bad_alloc();
  }
  if(void* p=std::malloc(n?n:1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
static unsigned checks=0;
static void Check(bool ok,const char* why) {
  ++checks; if(!ok) throw std::runtime_error(why);
}
static EngineUuid Id(unsigned n) {
  EngineUuid id;
  id.bytes={1,0x9b,0x12,0x34,0x56,0x78,0x70,0,0x80,0,0,0,0,0,0,0};
  id.bytes[14]=n>>8; id.bytes[15]=n; return id;
}
static EngineRequestContext Context(const std::string& path) {
  EngineRequestContext c;
  c.database_path=path; c.database_uuid=Id(1); c.session_uuid=Id(2);
  c.transaction_uuid=Id(3); c.principal_uuid=Id(4); c.statement_receipt_uuid=Id(5);
  c.security_context_present=true; c.statement_metadata_snapshot_engine_owned=true;
  return c;
}
static auto Issue(const EngineRequestContext& c) {
  SblrSourceMapHashV2 bound{}; bound.fill(0xa5);
  scratchbird::engine::sblr::SblrSourceMapEntryV1 e;
  e.node_id=1; e.redaction_class=4;
  scratchbird::engine::sblr::SblrSourceMapEntryV1 visible;
  visible.node_id=2; visible.source_artifact_uuid=Id(21).bytes;
  visible.source_artifact_generation=1; visible.byte_length=4;
  return IssueSblrSourceMapDescriptorV1(c,c.statement_receipt_uuid,bound,Id(6),77,{e,visible});
}
static auto Lookup(const EngineRequestContext& c,const SblrSourceMapDescriptorSnapshotV1& s) {
  return LookupSblrSourceMapDescriptorV1(c,c.statement_receipt_uuid,s.descriptor_uuid,
      s.descriptor_generation,s.bound_ast_sha256,s.registry_snapshot_uuid,s.registry_generation);
}
static Bytes Read(const std::string& path) {
  std::ifstream f(path,std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(f),{});
}
static void Write(const std::string& path,const Bytes& b) {
  std::ofstream f(path,std::ios::binary|std::ios::trunc);
  f.write(reinterpret_cast<const char*>(b.data()),b.size()); f.close();
  Check(bool(f),"fixture write");
}
static std::uint64_t Number(const Bytes& b,std::size_t at,unsigned size) {
  Check(at+size<=b.size(),"oracle bounds"); std::uint64_t n=0;
  for(unsigned i=0;i<size;++i) n|=std::uint64_t(b[at+i])<<(i*8);
  return n;
}
static auto Digest(std::string_view domain,const Bytes& b,std::size_t start,
                   std::size_t prefix,std::size_t body,std::size_t end) {
  Bytes input(domain.begin(),domain.end());
  input.insert(input.end(),b.begin()+start,b.begin()+prefix);
  input.insert(input.end(),b.begin()+body,b.begin()+end);
  const auto hash=scratchbird::core::hash::ComputeSha256Digest(input);
  Check(hash.ok(),"oracle hash"); return hash.digest;
}
static void Seal(Bytes& b) {
  const auto h=Digest("ScratchBird.SourceMapRegistrySnapshot.V2",b,0,96,128,b.size());
  std::copy(h.begin(),h.end(),b.begin()+96);
}
static void Verify(const Bytes& b,unsigned count,std::uint64_t generation,unsigned active) {
  Check(b.size()>=128 && std::equal(b.begin(),b.begin()+8,"SBSMR2\0\0"),"binary header");
  Check(Number(b,8,8)==b.size() && Number(b,16,2)==2 && Number(b,18,2)==128,"header extent");
  Check(Number(b,20,4)==count && Number(b,24,8)==generation,"publication count/generation");
  const auto db=Id(1);
  Check(std::equal(db.bytes.begin(),db.bytes.end(),b.begin()+32),"database bytes");
  const auto hash=Digest("ScratchBird.SourceMapRegistrySnapshot.V2",b,0,96,128,b.size());
  Check(std::equal(hash.begin(),hash.end(),b.begin()+96),"snapshot hash");
  unsigned live=0; std::size_t at=128; std::array<std::uint8_t,16> previous{};
  for(unsigned i=0;i<count;++i) {
    const auto size=Number(b,at,4);
    Check(size>=264 && at+size<=b.size(),"record extent");
    std::array<std::uint8_t,16> id{}; std::copy_n(b.begin()+at+32,16,id.begin());
    Check(id>previous,"descriptor order"); previous=id;
    Check(Number(b,at+8,8)==1 && Number(b,at+16,8)==77,"immutable generation");
    Check(Number(b,at+24,8)<=generation,"record publication");
    Check(Number(b,at+224,4)==size-264,"SMVD extent");
    const auto rh=Digest("ScratchBird.SourceMapRegistryRecord.V2",b,at,at+232,at+264,at+size);
    Check(std::equal(rh.begin(),rh.end(),b.begin()+at+232),"record hash");
    const auto v=scratchbird::engine::sblr::DecodeSblrSourceMapDescriptorVectorV1(b.data()+at+264,size-264);
    Check(v.status==scratchbird::engine::sblr::SblrSourceMapDecodeStatusV1::ok,"canonical SMVD");
    Check(v.vector.descriptor_uuid==id && v.vector.registry_generation==77,"SMVD binding");
    live+=b[at+4]==1; Check(b[at+4]==1 || b[at+4]==2,"lifecycle"); at+=size;
  }
  Check(at==b.size() && live==active,"whole cohort");
}
struct Files {
  std::filesystem::path directory;
  Files() {
    const auto id=scratchbird::core::uuid::IssueRuntimeIdentityV7(); Check(id.has_value(),"fixture UUID");
    directory=std::filesystem::temp_directory_path()/("sb-source-map-"+scratchbird::core::uuid::UuidToString(*id));
    Check(std::filesystem::create_directory(directory),"fixture directory");
  }
  ~Files() { std::error_code e; std::filesystem::remove_all(directory,e); }
};
#if defined(SB_SOURCE_MAP_FAULTS)
enum Fault { none,write_failure,file_sync_failure,rename_failure,directory_sync_failure,
             crash_before_rename,crash_after_rename };
static std::atomic<int> fault{none};
static bool renamed=false;
static bool Temporary(int fd) {
  char link[64],path[4096]; std::snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
  const auto n=::readlink(link,path,sizeof(path)-1); if(n<0) return false; path[n]=0;
  return std::strstr(path,".sb.sblr_source_map_registry.v1.tmp.") && !std::strstr(path,".sb.owner.lock");
}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" int __real_fsync(int);
// std::filesystem lives in a shared library: wrapping libc rename would miss
// its internal call. Intercept the executable's actual filesystem call.
extern "C" void RealRename(const std::filesystem::path&,const std::filesystem::path&,
                           std::error_code&) noexcept
  asm("__real__ZNSt10filesystem6renameERKNS_7__cxx114pathES3_RSt10error_code");
extern "C" void WrapRename(const std::filesystem::path&,const std::filesystem::path&,
                           std::error_code&) noexcept
  asm("__wrap__ZNSt10filesystem6renameERKNS_7__cxx114pathES3_RSt10error_code");
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t off) {
  if(fault==write_failure && Temporary(fd)) { fault=none; errno=EIO; return -1; }
  return __real_pwrite(fd,p,n,off);
}
extern "C" int __wrap_fsync(int fd) {
  struct stat s{};
  if(::fstat(fd,&s)==0 && ((fault==file_sync_failure && Temporary(fd) && s.st_size>0) ||
      (fault==directory_sync_failure && renamed && S_ISDIR(s.st_mode)))) {
    fault=none; errno=EIO; return -1;
  }
  return __real_fsync(fd);
}
extern "C" void WrapRename(const std::filesystem::path& from,
                           const std::filesystem::path& to,std::error_code& error) noexcept {
  if(std::strstr(from.c_str(),".sb.sblr_source_map_registry.v1.tmp.")) {
    if(fault==crash_before_rename) ::_exit(86);
    if(fault==rename_failure) { fault=none; error=std::make_error_code(std::errc::io_error); return; }
    RealRename(from,to,error);
    if(!error) { renamed=true; if(fault==crash_after_rename) ::_exit(86); } return;
  }
  RealRename(from,to,error);
}
static void ChildSuccess(pid_t child,int expected) {
  int status=0; Check(child>0 && ::waitpid(child,&status,0)==child,"wait child");
  Check(WIFEXITED(status) && WEXITSTATUS(status)==expected,"child exit");
}
#endif
int main(int argc,char** argv) {
  try {
    if(argc==3 && std::string_view(argv[1])=="recover")
      return RecoverSblrSourceMapDescriptorRegistryV1(Context(argv[2])).error?2:0;
    Files files; const auto base=(files.directory/"node").string(); Write(base,{1});
    auto c=Context(base); const auto path=base+".sb.sblr_source_map_registry.v1";
    Check(!RevokeSblrSourceMapDescriptorsV1(c,c.statement_receipt_uuid).error,"absent revoke");
    Check(!std::filesystem::exists(path),"absent registry created");
    const auto first=Issue(c); Check(first.ok && !first.diagnostic.error,"issue");
    Verify(Read(path),1,1,1); const auto body=first.snapshot.canonical_smvd;
    Check(first.snapshot.registry_generation==77 && Lookup(c,first.snapshot).ok,"lookup immutable generation");
    const auto second=Issue(c); Check(second.ok,"second issue");
    auto other=c; other.statement_receipt_uuid=Id(7);
    const auto third=Issue(other); Check(third.ok,"other receipt issue");
    const auto before=Read(path); Verify(before,3,3,3);
    for(const auto& item:std::filesystem::directory_iterator(files.directory))
      Check(item.path().filename().string().find(".tmp.")==std::string::npos,"successful publication temporary leaked");
    Check(first.snapshot.descriptor_uuid!=second.snapshot.descriptor_uuid,"unique descriptors");
    for(unsigned owner=0;owner<7;++owner) {
      auto wrong=c;
      if(owner==0) wrong.database_uuid=Id(90);
      if(owner==1) wrong.principal_uuid=Id(90);
      if(owner==2) wrong.session_uuid=Id(90);
      if(owner==3) wrong.transaction_uuid=Id(90);
      if(owner==4) wrong.statement_receipt_uuid=Id(90);
      if(owner==5) wrong.security_context_present=false;
      if(owner==6) wrong.statement_metadata_snapshot_engine_owned=false;
      auto stale=first.snapshot; stale.bound_ast_sha256.fill(0);
      const auto denied=Lookup(wrong,stale);
      Check(!denied.ok && denied.diagnostic.code=="SECURITY.ACCESS_DENIED","owner before stale");
      stale.descriptor_generation=0;
      Check(Lookup(wrong,stale).diagnostic.code=="SECURITY.ACCESS_DENIED","owner before zero generation");
      if(owner!=4) Check(RevokeSblrSourceMapDescriptorsV1(wrong,c.statement_receipt_uuid).error,"foreign revoke");
      if(owner!=4) Check(!Issue(wrong).ok,"foreign issue in existing receipt cohort");
      Check(Read(path)==before,"foreign access mutated state");
    }
    // A validly sealed file with conflicting receipt owners must be refused
    // as a whole before revoking any otherwise matching record.
    auto mixed=before; std::size_t record_at=128;
    for(unsigned row=0;row<3;++row) {
      const auto record_size=Number(mixed,record_at,4);
      const auto receipt=c.statement_receipt_uuid;
      if(std::equal(receipt.bytes.begin(),receipt.bytes.end(),mixed.begin()+record_at+64)) {
        const auto principal=Id(99);
        std::copy(principal.bytes.begin(),principal.bytes.end(),mixed.begin()+record_at+128);
        const auto digest=Digest("ScratchBird.SourceMapRegistryRecord.V2",mixed,record_at,
                                record_at+232,record_at+264,record_at+record_size);
        std::copy(digest.begin(),digest.end(),mixed.begin()+record_at+232);
        break;
      }
      record_at+=record_size;
    }
    Seal(mixed); Write(path,mixed);
    Check(RevokeSblrSourceMapDescriptorsV1(c,c.statement_receipt_uuid).code=="SECURITY.ACCESS_DENIED" &&
          Read(path)==mixed,"mixed owner cohort partially revoked");
    Check(!Issue(c).ok && Read(path)==mixed,"mixed owner cohort extended");
    Write(path,before);
    auto stale=first.snapshot; ++stale.registry_generation;
    Check(Lookup(c,stale).diagnostic.code=="SBLR.SOURCE_MAP.STALE","stale generation");
    Check(!RevokeSblrSourceMapDescriptorsV1(c,c.statement_receipt_uuid).error,"cohort revoke");
    const auto revoked=Read(path); Verify(revoked,3,4,1);
    Check(!Lookup(c,first.snapshot).ok && !Lookup(c,second.snapshot).ok && Lookup(other,third.snapshot).ok,"exact cohort");
    Check(!RevokeSblrSourceMapDescriptorsV1(c,c.statement_receipt_uuid).error && Read(path)==revoked,"idempotent revoke");
    Check(std::search(revoked.begin(),revoked.end(),body.begin(),body.end())!=revoked.end(),"immutable SMVD");
    for(std::size_t i=0;i<revoked.size();++i) {
      auto damaged=revoked; damaged[i]^=0x40; Write(path,damaged);
      Check(!Lookup(other,third.snapshot).ok && Read(path)==damaged,"corruption accepted/overwritten");
    }
    for(std::size_t i:{128+4,128+5,128+8,128+160,128+224,128+264}) {
      auto damaged=revoked; damaged[i]^=0x20; Seal(damaged); Write(path,damaged);
      Check(!Lookup(other,third.snapshot).ok,"inner corruption");
    }
    for(const Bytes damaged:{Bytes{},Bytes{'S','B','S','M','R','1','\n'},Bytes(revoked.begin(),revoked.end()-1)}) {
      Write(path,damaged); Check(!Issue(c).ok && Read(path)==damaged,"legacy/truncated overwrite");
    }
    auto trailing=revoked; trailing.push_back(0); Write(path,trailing);
    Check(!Issue(c).ok && Read(path)==trailing,"trailing bytes"); Write(path,revoked);
    // Exercise every allocation in a real issuance, including I/O adapters.
    // Caller-side argument allocation can throw before registry entry; a
    // completed publication must always remain discoverable for cleanup.
    unsigned allocation_points=0;
    for(long ordinal=0;ordinal<4096;++ordinal) {
      Write(path,before);
      const auto failures=allocation_failures.load();
      bool accepted=false;
      fail_allocation=ordinal;
      try { accepted=Issue(c).ok; } catch(const std::bad_alloc&) {}
      fail_allocation=-1;
      const auto visible=Read(path);
      Check(!accepted || visible!=before,"successful issuance did not publish");
      if(visible!=before) Verify(visible,4,4,4);
      Check(Lookup(other,third.snapshot).ok,"allocation fault lost reload/ownership");
      Check(!RevokeSblrSourceMapDescriptorsV1(c,c.statement_receipt_uuid).error,"allocation retry cleanup");
      if(allocation_failures==failures) { Check(accepted,"allocation sweep terminal issue"); break; }
      ++allocation_points;
      Check(ordinal<4095,"allocation sweep did not reach end");
    }
    Check(allocation_points>0,"allocation injection absent");
    Write(path,revoked);
#if defined(SB_SOURCE_MAP_FAULTS)
    for(unsigned operation=0;operation<3;++operation)
    for(auto failure:{write_failure,file_sync_failure,rename_failure,directory_sync_failure}) {
      Write(path,before); renamed=false; fault=failure;
      const auto refused=operation==0 ? RevokeSblrSourceMapDescriptorsV1(c,c.statement_receipt_uuid) :
          operation==1 ? Issue(c).diagnostic : RecoverSblrSourceMapDescriptorRegistryV1(c);
      if(!refused.error || fault!=none)
        std::cerr<<"fault="<<failure<<" remaining="<<fault.load()<<" code="<<refused.code<<'\n';
      Check(refused.error && fault==none,"publication fault not observed");
      const bool published=failure==directory_sync_failure;
      if(published) Verify(Read(path),operation==1?4:3,4,operation==0?1:operation==1?4:0);
      else Check(Read(path)==before,"pre-rename changed snapshot");
      if(operation==2) {
        Check(!RecoverSblrSourceMapDescriptorRegistryV1(c).error,"recovery retry");
        Verify(Read(path),3,4,0);
      } else {
        Check(!RevokeSblrSourceMapDescriptorsV1(c,c.statement_receipt_uuid).error,"retry");
        Verify(Read(path),operation==1 && published?4:3,operation==1 && published?5:4,1);
      }
    }
    for(auto crash:{crash_before_rename,crash_after_rename}) {
      Write(path,before); const auto child=::fork();
      if(child==0) { fault=crash; (void)RevokeSblrSourceMapDescriptorsV1(c,c.statement_receipt_uuid); ::_exit(87); }
      ChildSuccess(child,86);
      if(crash==crash_before_rename) Check(Read(path)==before,"pre-rename crash");
      else Verify(Read(path),3,4,1);
      Check(!RevokeSblrSourceMapDescriptorsV1(c,c.statement_receipt_uuid).error,"crash retry");
      Verify(Read(path),3,4,1);
    }
    const auto child=::fork();
    if(child==0) { ::execl(argv[0],argv[0],"recover",base.c_str(),nullptr); ::_exit(88); }
    ChildSuccess(child,0); Verify(Read(path),3,5,0);
#else
    Check(!RecoverSblrSourceMapDescriptorRegistryV1(c).error,"recovery"); Verify(Read(path),3,5,0);
#endif
    Check(!Lookup(other,third.snapshot).ok,"recovery active descriptor");
    const auto recovered=Read(path);
    Check(!RecoverSblrSourceMapDescriptorRegistryV1(c).error && Read(path)==recovered,"idempotent recovery");
    std::vector<SblrSourceMapRegistryResultV1> results(8); std::vector<std::thread> threads;
    for(unsigned i=0;i<results.size();++i) threads.emplace_back([&,i] { results[i]=Issue(c); });
    for(auto& t:threads) t.join();
    for(const auto& r:results) Check(r.ok && Lookup(c,r.snapshot).ok,"concurrent issue");
    Verify(Read(path),11,13,8);
    const auto base2=(files.directory/"other-node").string(); Write(base2,{1});
    auto node=c; node.database_path=base2; node.database_uuid=Id(100);
    const auto isolated=Issue(node); Check(isolated.ok,"second node issue");
    Check(!Lookup(c,isolated.snapshot).ok && !Lookup(node,results[0].snapshot).ok,"node isolation");
    Check(!RevokeSblrSourceMapDescriptorsV1(c,c.statement_receipt_uuid).error,"final revoke"); Verify(Read(path),11,14,0);
    auto nil_transaction=node; nil_transaction.statement_receipt_uuid=Id(101);
    nil_transaction.transaction_uuid={};
    const auto optional=Issue(nil_transaction);
    Check(optional.ok && Lookup(nil_transaction,optional.snapshot).ok,"optional nil transaction");
    auto mismatch=nil_transaction; mismatch.transaction_uuid=Id(3);
    Check(Lookup(mismatch,optional.snapshot).diagnostic.code=="SECURITY.ACCESS_DENIED","nil transaction binding");
    std::cout<<"SOURCE_MAP binary atomic registry "<<checks<<" checks; "
             <<allocation_points<<" allocation failures PASS\n";
  } catch(const std::exception& e) { std::cerr<<"SOURCE_MAP check "<<checks<<": "<<e.what()<<'\n'; return 1; }
}

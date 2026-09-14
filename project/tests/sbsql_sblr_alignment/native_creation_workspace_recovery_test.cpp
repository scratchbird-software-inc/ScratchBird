// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_creation_workspace_recovery.hpp"
#include "disk_device.hpp"
#include "hash_digest.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <openssl/evp.h>

using namespace scratchbird::core::platform;
namespace db=scratchbird::storage::database;
namespace disk=scratchbird::storage::disk;
namespace page=scratchbird::storage::page;
namespace mga=scratchbird::transaction::mga;
namespace fs=std::filesystem;
namespace {
unsigned checks=0;
void Check(bool b,const char* message){++checks;if(!b)throw std::runtime_error(message);}
enum Call {write_call,read_call,sync_call,random_call,hash_call,context_call,init_call,update_call,final_call,call_count};
bool armed=false,count_allocations=false;
unsigned fault_kind=call_count,fault_at=0,allocations=0,allocation_fault=0,corrupt_read=0;
std::array<unsigned,call_count> calls{};
struct Event{Call kind;u64 offset;};
std::array<Event,2048> events{};unsigned event_count=0;
bool stop_creation=false;
u64 second_offset=18*8192;
std::atomic<bool> pause_write=false,paused=false,resume=false;
bool Hit(Call kind,u64 offset=0){if(!armed)return false;
  if(event_count<events.size())events[event_count++]={kind,offset};
  return ++calls[kind]==fault_at&&kind==fault_kind;
}
void Arm(unsigned kind=call_count,unsigned at=0){calls={};event_count=0;corrupt_read=0;fault_kind=kind;fault_at=at;armed=true;}
void Disarm(){armed=false;}
Uuid Id(unsigned n){Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[14]=n>>8;id.bytes[15]=n;return id;}
db::NativeFilespaceInitializationRequest Request(unsigned profile=0,u64 total=19){
  const auto& p=disk::kCanonicalFilespacePageProfiles[profile];db::NativeFilespaceInitializationRequest r;
  r.bootstrap={Id(1),Id(2),p.uuid,disk::kNativeBootstrapIntegrityProfile,{},p.page_size_bytes,1,0,1,7};
  r.operation_uuid=Id(3);r.writer_uuid=Id(4);r.creator.transaction_uuid={UuidKind::transaction,Id(5)};
  r.creator.local_id=mga::MakeLocalTransactionId(1);r.creator.scope=mga::TransactionScope::local_node;
  r.total_pages=total;r.creation_utc_millis=1789357072000ULL;return r;
}
struct Fixture {
  fs::path directory;unsigned serial=0;
  Fixture(){char name[]="/tmp/sb-creation-recovery-test.XXXXXX";const auto* p=mkdtemp(name);Check(p,"mkdtemp");directory=p;}
  ~Fixture(){std::error_code ec;fs::remove_all(directory,ec);}
  fs::path Next(){return directory/("node-"+std::to_string(serial++));}
};
disk::FilespaceBootstrapBinding Binding(const db::NativeFilespaceInitializationRequest& r){
  return {r.bootstrap.database_uuid,r.bootstrap.filespace_uuid,r.bootstrap.page_size_profile_uuid};
}
auto Recover(disk::FileDevice& d,const db::NativeFilespaceInitializationRequest& r,u64 budget=0){
  return db::RecoverNativeCreationWorkspaceSelectionOnOpenDevice(d,Binding(r),r.operation_uuid,budget?budget:256ULL*r.bootstrap.page_size_bytes);
}
std::vector<byte> Read(disk::FileDevice& d,u64 offset,u64 size){std::vector<byte> bytes(size);
  const auto io=d.ReadAt(offset,bytes.data(),bytes.size());Check(io.ok()&&io.bytes_transferred==bytes.size(),"actual read");return bytes;}
void Write(disk::FileDevice& d,u64 offset,const std::vector<byte>& bytes){const auto io=d.WriteAt(offset,bytes.data(),bytes.size());
  Check(io.ok()&&io.bytes_transferred==bytes.size(),"actual write");}
void Restore(disk::FileDevice& d,const std::vector<byte>& bytes){Write(d,0,bytes);Check(d.Sync().ok(),"fixture sync");}
std::vector<byte> Page(const std::vector<byte>& bytes,u64 n,u64 size){return {bytes.begin()+n*size,bytes.begin()+(n+1)*size};}
void SetPage(std::vector<byte>& bytes,u64 n,const std::vector<byte>& image){std::copy(image.begin(),image.end(),bytes.begin()+n*image.size());}
void ResealCheckpoint(std::vector<byte>& bytes,u64 size,u64 maps,unsigned role,const std::vector<byte>& replacement){
  auto cp=db::DecodeNativeCheckpointRoot(Page(bytes,maps+1,size));Check(cp.ok(),"mutation checkpoint decode");
  const auto hash=scratchbird::core::hash::ComputeSha256Digest(replacement);Check(hash.ok(),"mutation hash");
  cp.root->roots[role-1].sha256=hash.digest;
  if(role==5)cp.root->roots[8].sha256=hash.digest; // Feature shares this physical catalog.
  auto encoded=db::EncodeNativeCheckpointRoot(*cp.root);Check(encoded.ok(),"mutation cp seal");
  SetPage(bytes,maps+1,encoded.bytes);
  const auto cp_hash=scratchbird::core::hash::ComputeSha256Digest(encoded.bytes);Check(cp_hash.ok(),"mutation checkpoint hash");
  for(u64 n: {maps+16,maps+17}){auto s=db::DecodeNativeCheckpointSelection(Page(bytes,n,size));Check(s.ok(),"mutation selector decode");
    s.selection->checkpoint_sha256=cp_hash.digest;auto e=db::EncodeNativeCheckpointSelection(*s.selection);Check(e.ok(),"mutation selector seal");SetPage(bytes,n,e.bytes);}
}
void Ordered(unsigned repaired,u64 size,u64 first){
  unsigned syncs=0,writes=0;bool first_compared=false;
  for(unsigned i=0;i<event_count;++i){const auto e=events[i];
    if(e.kind==sync_call)++syncs;
    if(e.kind==read_call&&syncs==2&&e.offset==first*size)first_compared=true;
    if(e.kind==write_call){++writes;
      Check((e.offset==first*size&&(repaired&1)&&syncs==1)||
        (e.offset==(first+1)*size&&(repaired&2)&&syncs==2&&first_compared),"dependency and per-slot sync/readback ordering");}
  }
  Check(syncs==3&&writes==unsigned((repaired&1)!=0)+unsigned((repaired&2)!=0)&&!calls[random_call],"exact recovery barriers/writes and no new identities");
}
}
void* operator new(std::size_t n){if(count_allocations&&++allocations==allocation_fault)throw std::bad_alloc();
  if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);} void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);} void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t offset){
  if(stop_creation&&offset==static_cast<off_t>(second_offset)&&n>=128&&static_cast<const byte*>(p)[0])_exit(86);
  if(pause_write.exchange(false)){paused=true;while(!resume.load())std::this_thread::yield();}
  if(Hit(write_call,offset)){errno=EIO;return -1;}return __real_pwrite(fd,p,n,offset);
}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* p,size_t n,off_t offset){
  if(Hit(read_call,offset)){errno=EIO;return -1;}const auto r=__real_pread(fd,p,n,offset);
  if(armed&&corrupt_read&&calls[read_call]==corrupt_read&&r>0)static_cast<byte*>(p)[r-1]^=1;return r;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){if(Hit(sync_call)){errno=EIO;return -1;}return __real_fsync(fd);}
extern "C" int __real_RAND_bytes(unsigned char*,int);
extern "C" int __wrap_RAND_bytes(unsigned char* p,int n){if(Hit(random_call))return 0;return __real_RAND_bytes(p,n);}
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* p,size_t n,unsigned char* out,unsigned int* s,const EVP_MD* t,ENGINE* e){
  if(Hit(hash_call))return 0;return __real_EVP_Digest(p,n,out,s,t,e);}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){if(Hit(context_call))return nullptr;return __real_EVP_MD_CTX_new();}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* t,ENGINE* e){if(Hit(init_call))return 0;return __real_EVP_DigestInit_ex(c,t,e);}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* p,size_t n){if(Hit(update_call))return 0;return __real_EVP_DigestUpdate(c,p,n);}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* p,unsigned int* n){if(Hit(final_call))return 0;return __real_EVP_DigestFinal_ex(c,p,n);}

int main(int argc,char** argv){try{
  if(argc==4&&std::string_view(argv[1])=="--reopen"){
    const auto r=Request(std::stoul(argv[3]),64);disk::FileDevice d;Check(d.Open(argv[2],disk::FileOpenMode::open_existing).ok(),"fresh process owns graph");
    Arm();const auto recovered=Recover(d,r);Disarm();Check(recovered.ok()&&!recovered.repaired_slots&&!calls[write_call],"fresh executable idempotent actual graph admission");return 0;
  }
  Fixture f;
  const auto request=Request();const u64 size=8192;
  disk::FileDevice device;const auto path=f.Next();Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"fixture owns actual file");
  Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,25*size).ok(),"actual initial workspace");
  const auto original=Read(device,0,19*size);auto damaged=original;std::fill(damaged.begin()+18*size,damaged.end(),0);
  Restore(device,damaged);Check(Recover(device,request).ok(),"warm actual recovery");
  if(argc>=2&&std::string_view(argv[1])=="--allocations"){
    const unsigned shard=argc>=3?std::stoul(argv[2]):0;Check(shard<4,"allocation shard");
    const std::string_view mode=argc==4?argv[3]:"second";
    Check(mode=="first"||mode=="second"||mode=="stable","allocation recovery profile");
    if(mode=="first"){damaged=original;std::fill(damaged.begin()+17*size,damaged.begin()+18*size,0);}
    if(mode=="stable")damaged=original;
    Restore(device,damaged);allocations=0;allocation_fault=1;count_allocations=true;
    const auto warm=Recover(device,request);count_allocations=false;Check(!warm.ok()&&!warm.receipt,"warm failed recovery");
    Check(device.Close().ok(),"close before independent allocation children");
    // Measure the same reopened-device operation in a child of this warmed
    // parent. Measuring on the create_new parent's device counted one extra
    // allocation (6134 vs 6133); that nonexistent child site is not a fault.
    int channel[2];Check(pipe(channel)==0,"allocation baseline channel");
    const auto baseline_child=fork();Check(baseline_child>=0,"allocation baseline fork");
    if(baseline_child==0){close(channel[0]);try{disk::FileDevice d;
      Check(d.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"baseline child owns file");Restore(d,damaged);
      allocations=0;allocation_fault=0;count_allocations=true;const auto baseline=Recover(d,request);count_allocations=false;
      Check(baseline.ok()&&allocations,"actual isolated allocation baseline");
      const auto n=write(channel[1],&allocations,sizeof allocations);Check(n==sizeof allocations&&d.Close().ok(),"return baseline count");_exit(0);
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';_exit(1);}}
    close(channel[1]);unsigned sites=0;const auto count_bytes=read(channel[0],&sites,sizeof sites);close(channel[0]);int baseline_status=0;
    Check(waitpid(baseline_child,&baseline_status,0)==baseline_child&&WIFEXITED(baseline_status)&&WEXITSTATUS(baseline_status)==0&&
      count_bytes==sizeof sites&&sites,"isolated repeatable allocation site count");
    for(unsigned at=1+shard;at<=sites;at+=4){const auto child=fork();Check(child>=0,"allocation fork");
      if(child==0){try{disk::FileDevice d;Check(d.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"allocation child owns file");
        Restore(d,damaged);allocations=0;allocation_fault=at;count_allocations=true;const auto r=Recover(d,request);count_allocations=false;
        if(allocations<at||r.ok()||r.receipt||r.repaired_slots)std::cerr<<"observed allocations="<<allocations<<" error="<<static_cast<unsigned>(r.error)<<" receipt="<<r.receipt.has_value()<<'\n';
        Check(allocations>=at&&!r.ok()&&!r.receipt&&!r.repaired_slots,"every reached allocation fault returns no receipt");
        Check(d.Close().ok(),"allocation child releases file");_exit(0);
      }catch(const std::exception& e){std::cerr<<"allocation "<<at<<": "<<e.what()<<'\n';_exit(1);}}
      int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"isolated allocation failure child");
    }
    std::cout<<"recovery allocation sites="<<sites<<" mode="<<mode<<" shard="<<shard<<" checks="<<checks<<'\n';return 0;
  }
  for(unsigned profile=0;profile<5;++profile){const auto r=Request(profile,64);const u64 p=r.bootstrap.page_size_bytes;
    disk::FileDevice d;const auto filename=f.Next();Check(d.Open(filename.string(),disk::FileOpenMode::create_new).ok(),"profile fixture");
    const auto created=db::InitializeNativeCreationWorkspaceOnOpenDevice(d,r,64*p);Check(created.ok(),"all-profile genesis");
    const auto bytes=Read(d,0,64*p);const u64 maps=profile==0?2:1;
    for(unsigned slot=0;slot<2;++slot)for(unsigned mode=0;mode<3;++mode){auto broken=bytes;const auto off=(maps+16+slot)*p;
      if(mode==0)std::fill(broken.begin()+off,broken.begin()+off+p,0);
      else broken[off+(mode==1?0:432)]^=1;Restore(d,broken);Arm();const auto recovered=Recover(d,r);Disarm();
      Check(recovered.ok()&&recovered.repaired_slots==(1u<<slot),"either zero/torn/digest-damaged selector repaired");Ordered(1u<<slot,p,maps+16);
      Check(Read(d,0,64*p)==bytes&&recovered.receipt->publication_uuid==created.receipt->publication_uuid&&
        recovered.receipt->checkpoint_sha256==created.receipt->checkpoint_sha256,"exact original bytes and publication lineage preserved");}
    Arm();const auto stable=Recover(d,r);Disarm();Check(stable.ok()&&!stable.repaired_slots,"stable genesis recovery");Ordered(0,p,maps+16);
    {auto orphan=bytes;orphan.back()=1;Restore(d,orphan);Arm();const auto rejected=Recover(d,r);Disarm();
      Check(!rejected.ok()&&!rejected.receipt&&!calls[write_call]&&Read(d,0,bytes.size())==orphan,
        "nonzero physically free page is not hidden as pristine genesis");Restore(d,bytes);}
    Check(d.Close().ok(),"release graph before exec");const auto child=fork();Check(child>=0,"reopen fork");
    if(child==0){const auto argument=std::to_string(profile);execl(argv[0],argv[0],"--reopen",filename.c_str(),argument.c_str(),nullptr);_exit(127);}
    int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh process recovery admission");
  }
  {const auto filename=f.Next();const auto child=fork();Check(child>=0,"actual creation loss fork");
    if(child==0){disk::FileDevice d;if(!d.Open(filename.string(),disk::FileOpenMode::create_new).ok())_exit(87);
      stop_creation=true;(void)db::InitializeNativeCreationWorkspaceOnOpenDevice(d,request,25*size);_exit(88);}
    int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"actual process loss before second selector");
    disk::FileDevice d;Check(d.Open(filename.string(),disk::FileOpenMode::open_existing).ok(),"new owner after process loss");
    const auto before=Read(d,0,19*size);const auto first=db::DecodeNativeCheckpointSelection(Page(before,17,size));Check(first.ok(),"surviving original publication");
    const auto recovered=Recover(d,request);Check(recovered.ok()&&recovered.repaired_slots==2&&recovered.receipt->publication_uuid==first.selection->publication_uuid,
      "real interrupted constructor repaired from graph without new publication");
    const auto after=Read(d,0,19*size);Check(std::equal(before.begin(),before.begin()+18*size,after.begin()),"process-loss repair changes only missing slot");}
  const auto refuse=[&](const std::vector<byte>& bytes){Restore(device,bytes);Arm();const auto r=Recover(device,request);Disarm();
    Check(!r.ok()&&!r.receipt&&!r.repaired_slots&&!calls[write_call]&&Read(device,0,bytes.size())==bytes,"invalid graph refuses without mutation or receipt");};
  {const auto large=Request(0,3601);disk::FileDevice d;Check(d.Open(f.Next().string(),disk::FileOpenMode::create_new).ok(),"multi-map recovery fixture");
    const auto created=db::InitializeNativeCreationWorkspaceOnOpenDevice(d,large,85*size);Check(created.ok()&&created.receipt->map_pages==61,"controls span allocation coverage ranges");
    const auto before=Read(d,0,large.total_pages*size);Write(d,78*size,std::vector<byte>(size,0));
    const auto recovered=Recover(d,large,85*size);Check(recovered.ok()&&recovered.repaired_slots==2&&Read(d,0,before.size())==before,
      "multi-map graph and original cross-range selector allocation recovered");}
  {disk::FileDevice d;Check(d.Open(f.Next().string(),disk::FileOpenMode::create_new).ok(),"extent mismatch fixture");
    auto extra=damaged;extra.push_back(0);Restore(d,extra);Arm();const auto r=Recover(d,request);Disarm();
    Check(!r.ok()&&!r.receipt&&!calls[write_call]&&Read(d,0,extra.size())==extra,"extra physical extent refuses without truncation");}
  {auto bytes=damaged;std::fill(bytes.begin()+17*size,bytes.begin()+18*size,0);refuse(bytes);}
  for(unsigned mode=0;mode<6;++mode){auto bytes=original;auto s=db::DecodeNativeCheckpointSelection(Page(bytes,18,size));Check(s.ok(),"conflict fixture");
    if(mode==0)s.selection->publication_uuid=Id(50);
    if(mode==1)s.selection->bootstrap_uuid=Id(51);
    if(mode==2)s.selection->header.page_uuid=Id(52);
    if(mode==3)s.selection->object_uuid=Id(53);
    if(mode==4)s.selection->timeline_uuid=Id(54);
    if(mode==5)s.selection->checkpoint_generation=2;
    auto encoded=db::EncodeNativeCheckpointSelection(*s.selection);Check(encoded.ok(),"valid contradictory selector encoding");SetPage(bytes,18,encoded.bytes);refuse(bytes);}
  for(u64 n=0;n<17;++n){auto bytes=damaged;bytes[n*size+size-1]^=1;refuse(bytes);}
  // Resealed graphs must still satisfy genesis, not merely their checksums.
  for(unsigned mode=0;mode<7;++mode){auto bytes=original;unsigned role=0;u64 n=0;std::vector<byte> replacement;
    if(mode==0){n=5;role=8;auto x=db::DecodeNativeSystemState(Page(bytes,n,size));Check(x.ok(),"system mutation");
      x.state->startup_counter=2;auto e=db::EncodeNativeSystemState(*x.state);Check(e.ok(),"system mutation seal");replacement=std::move(e.bytes);}
    if(mode==1){n=7;role=2;auto x=page::DecodeNativeHorizonRoot(Page(bytes,n,size));Check(x.ok(),"horizon mutation");
      x.root->records[0].local_boundary=1;auto e=page::EncodeNativeHorizonRoot(*x.root);Check(e.ok(),"horizon mutation seal");replacement=std::move(e.bytes);}
    if(mode==2){n=4;role=3;auto x=page::DecodeNativeFilespaceDirectory(Page(bytes,n,size));Check(x.ok(),"directory mutation");
      x.directory->records[0].verification_epoch=2;auto e=page::EncodeNativeFilespaceDirectory(*x.directory);Check(e.ok(),"directory mutation seal");replacement=std::move(e.bytes);}
    if(mode==3){n=8;role=5;auto x=page::DecodeNativeCatalogRoot(Page(bytes,n,size));Check(x.ok(),"catalog mutation");
      x.root->schema_epoch=2;auto e=page::EncodeNativeCatalogRoot(*x.root);Check(e.ok(),"catalog mutation seal");replacement=std::move(e.bytes);}
    if(mode>=4){n=1;role=4;auto x=page::DecodeNativeAllocationMap(Page(bytes,n,size));Check(x.ok(),"allocation mutation");
      if(mode==4)x.map->records[18].owner_uuid=Id(60);
      if(mode==5)x.map->records[18].allocation_uuid=x.map->records[17].allocation_uuid;
      if(mode==6)x.map->records[18].creator_transaction_uuid=Id(61);
      auto e=page::EncodeNativeAllocationMap(*x.map);Check(e.ok(),"allocation mutation seal");replacement=std::move(e.bytes);}
    SetPage(bytes,n,replacement);ResealCheckpoint(bytes,size,1,role,replacement);std::fill(bytes.begin()+18*size,bytes.end(),0);refuse(bytes);
  }
  Restore(device,damaged);auto wrong=request;wrong.operation_uuid=Id(40);Arm();const auto rejected=Recover(device,wrong);Disarm();
  Check(!rejected.ok()&&!rejected.receipt&&!calls[write_call],"wrong operation cannot repair");
  wrong=request;wrong.bootstrap.database_uuid=Id(41);Check(!Recover(device,wrong).ok(),"wrong database binding");
  Check(!Recover(device,request,25*size-1).ok()&&Recover(device,request,25*size).ok(),"exact retained-image budget boundary");
  for(unsigned slot=0;slot<3;++slot){auto broken=original;
    if(slot<2)std::fill(broken.begin()+(17+slot)*size,broken.begin()+(18+slot)*size,0);
    Restore(device,broken);Arm();Check(Recover(device,request).ok(),"fault baseline");Disarm();const auto sites=calls;
    for(unsigned kind=0;kind<call_count;++kind)for(unsigned at=1;at<=sites[kind];++at){Restore(device,broken);Arm(kind,at);
      const auto r=Recover(device,request);Disarm();Check(calls[kind]>=at&&!r.ok()&&!r.receipt&&!r.repaired_slots,"every provider/I/O failure withholds receipt");
      Check(Recover(device,request).ok()&&Read(device,0,original.size())==original,"retry observes actual partially repaired state");}
    for(unsigned at=1;at<=sites[read_call];++at){Restore(device,broken);Arm();corrupt_read=at;const auto r=Recover(device,request);Disarm();
      Check(!r.ok()&&!r.receipt&&!r.repaired_slots,"every corrupted read withholds receipt");
      Check(Recover(device,request).ok()&&Read(device,0,original.size())==original,"corrupted observation cannot alter other graph bytes");}
    std::cout<<"recovery slot="<<slot<<" sites";for(auto n:sites)std::cout<<' '<<n;std::cout<<'\n';
  }
  Restore(device,damaged);pause_write=true;paused=false;resume=false;std::atomic<bool> entered=false,acquired=false;
  db::NativeCreationRecoveryResult result;
  std::thread recovery([&]{result=Recover(device,request);});while(!paused.load())std::this_thread::yield();
  std::thread competing([&]{entered=true;const auto guard=device.AcquireOperationGuard();acquired=true;});
  while(!entered.load())std::this_thread::yield();Check(!acquired.load(),"recovery retains guard through actual write");resume=true;
  recovery.join();competing.join();Check(result.ok()&&acquired.load(),"guard releases only after completed recovery");
  Check(device.Close().ok(),"close recovered graph");
  Check(device.Open(path.string(),disk::FileOpenMode::open_existing_read_only).ok()&&!Recover(device,request).ok(),"read-only owner cannot repair");
  Check(device.Close().ok()&&!Recover(device,request).ok(),"unopened device cannot recover");
  std::cout<<"native creation recovery checks="<<checks<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"native creation recovery failure: "<<e.what()<<" checks="<<checks<<'\n';return 1;}}

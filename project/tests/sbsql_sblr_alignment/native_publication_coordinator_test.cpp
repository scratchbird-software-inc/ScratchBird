// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_publication_coordinator.hpp"
#include "native_creation_workspace.hpp"
#include "disk_device.hpp"
#include "hash_digest.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <openssl/evp.h>

using namespace scratchbird::core::platform;
namespace db=scratchbird::storage::database;
namespace disk=scratchbird::storage::disk;
namespace mga=scratchbird::transaction::mga;
namespace fs=std::filesystem;
namespace {
unsigned checks=0;
void Check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
enum Call {read_call,write_call,sync_call,digest_call,context_call,init_call,update_call,final_call,call_count};
std::array<unsigned,call_count> calls{};
bool armed=false,kill_second=false;
bool count_allocations=false;
unsigned allocations=0,allocation_fault=0;
unsigned fault_kind=call_count,fault_at=0,torn_at=0,torn_bytes=0;
u64 first_offset=0,second_offset=0;
unsigned phase=0;
bool ordered=true;
bool Hit(Call c){return armed&&++calls[c]==fault_at&&c==fault_kind;}
void Arm(unsigned kind=call_count,unsigned at=0){calls={};fault_kind=kind;fault_at=at;phase=0;ordered=true;armed=true;}
void Off(){armed=false;}
Uuid Id(unsigned n){Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[14]=n>>8;id.bytes[15]=n;return id;}
struct Fixture {
  fs::path root;unsigned serial=0;
  Fixture(){char name[]="/tmp/sb-publication-coordinator.XXXXXX";const auto p=mkdtemp(name);Check(p,"mkdtemp");root=p;}
  ~Fixture(){std::error_code ec;fs::remove_all(root,ec);}
  fs::path Next(){return root/("node-"+std::to_string(serial++));}
};
db::NativeFilespaceInitializationRequest Request(unsigned profile){
  const auto& p=disk::kCanonicalFilespacePageProfiles[profile];db::NativeFilespaceInitializationRequest r;
  r.bootstrap={Id(1),Id(2),p.uuid,disk::kNativeBootstrapIntegrityProfile,{},p.page_size_bytes,1,0,1,7};
  r.operation_uuid=Id(3);r.writer_uuid=Id(4);r.creator.transaction_uuid={UuidKind::transaction,Id(5)};
  r.creator.local_id=mga::MakeLocalTransactionId(1);r.creator.scope=mga::TransactionScope::local_node;
  r.creation_utc_millis=1789357072000ULL;r.total_pages=64;return r;
}
struct Node {
  disk::FileDevice device;db::NativeFilespaceInitializationRequest request;
  std::vector<disk::NativeFilespaceDevice> devices;
  u64 size,budget,first;
  Node(const fs::path& path,unsigned profile,bool create=true):request(Request(profile)),size(request.bootstrap.page_size_bytes),budget(256*size),first((profile?1:2)+18){
    Check(device.Open(path.string(),create?disk::FileOpenMode::create_new:disk::FileOpenMode::open_existing).ok(),"owned node open");
    devices={{Id(2),request.bootstrap.page_size_profile_uuid,&device}};
    if(create)Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,budget).ok(),"actual complete construction");
    first_offset=first*size;second_offset=(first+1)*size;
  }
  auto Inspect(){return db::InspectNativePublicationGenerationOnOpenDevices(Id(1),devices,Id(2),budget);}
  auto Recover(){return db::RecoverNativePublicationGenerationOnOpenDevices(Id(1),devices,Id(2),budget);}
  auto Reserve(const db::NativePublicationSnapshot& base,unsigned op){return db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),devices,Id(2),base,Id(op),budget);}
  auto ReserveIntent(const db::NativePublicationSnapshot& base,unsigned op,const db::NativePublicationIntent& intent){return db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),devices,Id(2),base,Id(op),budget,&intent);}
  auto Resume(const db::NativePublicationSnapshot& base,unsigned op,const db::NativePublicationIntent& intent){return db::ResumeNativePublicationGenerationOnOpenDevices(Id(1),devices,Id(2),base,Id(op),intent,budget);}
  std::vector<byte> Read(){std::vector<byte> bytes(64*size);const auto r=device.ReadAt(0,bytes.data(),bytes.size());Check(r.ok()&&r.bytes_transferred==bytes.size(),"actual complete read");return bytes;}
  void Restore(const std::vector<byte>& bytes){const auto r=device.WriteAt(0,bytes.data(),bytes.size());Check(r.ok()&&r.bytes_transferred==bytes.size()&&device.Sync().ok(),"isolated fixture restore");}
};
db::NativePublicationIntent Intent(){db::NativePublicationIntent i;i.initiator_uuid=Id(4);i.request_context_uuid=Id(90);i.policy_snapshot_uuid=Id(91);i.normalized_request_sha256.fill(92);i.initiator_kind=4;return i;}
auto Page(const std::vector<byte>& bytes,u64 n,u64 size){return std::vector<byte>(bytes.begin()+n*size,bytes.begin()+(n+1)*size);}
void Replace(std::vector<byte>& bytes,u64 n,const std::vector<byte>& page){std::copy(page.begin(),page.end(),bytes.begin()+n*page.size());}
void UnchangedExceptWatermarks(const std::vector<byte>& a,const std::vector<byte>& b,const Node& n){
  Check(a.size()==b.size()&&std::equal(a.begin(),a.begin()+n.first*n.size,b.begin())&&
    std::equal(a.begin()+(n.first+2)*n.size,a.end(),b.begin()+(n.first+2)*n.size),"no checkpoint, inventory, allocation or free-page changes");
}
// Typed physical fixture for an already selected equal-state successor. This
// bypasses the unfinished root publisher; it qualifies coordinator binding only.
void InstallSelectedCheckpointFixture(Node& n){
  auto bytes=n.Read();const auto old=n.Inspect();Check(old.ok()&&old.snapshot->watermark.watermark==2,"fixture has actual reserved generation2");
  const auto prior=old.snapshot->selection;
  auto cp=db::DecodeNativeCheckpointRoot(Page(bytes,prior.checkpoint.page_number,n.size));Check(cp.ok(),"fixture original checkpoint");
  auto inv=scratchbird::storage::page::DecodeNativeTransactionInventoryPage(Page(bytes,cp.root->roots[0].page.page_number,n.size));Check(inv.ok(),"fixture original inventory");
  auto map=scratchbird::storage::page::DecodeNativeAllocationMap(Page(bytes,cp.root->roots[3].page.page_number,n.size));Check(map.ok(),"fixture original allocation");
  const auto ref=[&](u64 number){return disk::NativePageReference{Id(2),number,1,n.request.bootstrap.page_size_profile_uuid};};
  const auto hash=[](const auto& b){const auto h=scratchbird::core::hash::ComputeSha256Digest(b);Check(h.ok(),"fixture complete hash");return h.digest;};
  inv.page->header.page_number=24;inv.page->header.page_uuid=Id(201);inv.page->inventory_generation=2;
  const auto inventory=scratchbird::storage::page::EncodeNativeTransactionInventoryPage(*inv.page);Check(inventory.ok(),"fixture successor inventory");
  map.map->header.page_number=26;map.map->header.page_uuid=Id(203);map.map->map_generation=2;
  const std::array<u32,4> types{0x301,0x300,3,3};const std::array<Uuid,4> owners{inv.page->object_uuid,cp.root->object_uuid,map.map->object_uuid,map.map->object_uuid};
  for(unsigned i=0;i<4;++i){const u64 page=24+i;map.map->states[page]=scratchbird::storage::page::NativeAllocationState::allocated;
    map.map->records.push_back({page,Id(211+i),Id(201+i),owners[i],Id(5),1,1,0,types[i]});}
  Check(map.map->next.has_value(),"fixture uses two-map profile");
  auto tail=scratchbird::storage::page::DecodeNativeAllocationMap(Page(bytes,map.map->next->page_number,n.size));Check(tail.ok(),"fixture original map tail");
  tail.map->header.page_number=27;tail.map->header.page_uuid=Id(204);tail.map->map_generation=2;
  const auto tail_image=scratchbird::storage::page::EncodeNativeAllocationMap(*tail.map);Check(tail_image.ok(),"fixture successor map tail");
  map.map->next=ref(27);map.map->next_sha256=hash(tail_image.bytes);Replace(bytes,27,tail_image.bytes);
  const auto allocation=scratchbird::storage::page::EncodeNativeAllocationMap(*map.map);Check(allocation.ok(),"fixture successor allocation");
  cp.root->header.page_number=25;cp.root->header.page_uuid=Id(202);cp.root->checkpoint_generation=cp.root->root_set_generation=2;
  cp.root->predecessor=prior.checkpoint;cp.root->predecessor_sha256=prior.checkpoint_sha256;
  cp.root->roots[0].page=ref(24);cp.root->roots[0].sha256=hash(inventory.bytes);
  cp.root->roots[3].page=ref(26);cp.root->roots[3].sha256=hash(allocation.bytes);
  const auto checkpoint=db::EncodeNativeCheckpointRoot(*cp.root);Check(checkpoint.ok(),"fixture successor checkpoint");
  Replace(bytes,24,inventory.bytes);Replace(bytes,25,checkpoint.bytes);Replace(bytes,26,allocation.bytes);
  for(unsigned i=0;i<2;++i){auto s=db::DecodeNativeCheckpointSelection(Page(bytes,n.first-2+i,n.size));Check(s.ok(),"fixture original selector");
    s.selection->previous_selection_generation=1;s.selection->previous_checkpoint=prior.checkpoint;
    s.selection->previous_checkpoint_object_uuid=prior.checkpoint_object_uuid;s.selection->previous_checkpoint_sha256=prior.checkpoint_sha256;
    s.selection->selection_generation=s.selection->checkpoint_generation=s.selection->root_set_generation=2;
    s.selection->checkpoint=ref(25);s.selection->checkpoint_sha256=hash(checkpoint.bytes);
    s.selection->publication_uuid=old.snapshot->watermark.operation_uuid;
    const auto encoded=db::EncodeNativeCheckpointSelection(*s.selection);Check(encoded.ok(),"fixture successor selector");Replace(bytes,n.first-2+i,encoded.bytes);
  }
  n.Restore(bytes);
}
void IntentReservations(Fixture& fixture){
  using E=db::NativePublicationError;
  for(unsigned profile=0;profile<5;++profile){const auto path=fixture.Next();Node n(path,profile);
    const auto original=n.Read();const auto initial=n.Inspect();Check(initial.ok(),"intent initial actual base");
    const auto request=Intent();auto input=request;Arm();auto reserved=n.ReserveIntent(*initial.snapshot,300,input);const auto reserve_calls=calls;Off();
    Check(reserved.ok()&&reserve_calls[write_call]==2&&reserve_calls[sync_call]==3,"actual durable intent reservation");
    input.normalized_request_sha256={};Check(reserved.lease->snapshot().watermark.intent==request,"lease owns request copy");reserved.lease.reset();
    const auto pending_bytes=n.Read();UnchangedExceptWatermarks(original,pending_bytes,n);
    for(unsigned slot=0;slot<2;++slot){const auto b=Page(pending_bytes,n.first+slot,n.size);
      Check(std::equal(request.initiator_uuid.bytes.begin(),request.initiator_uuid.bytes.end(),b.begin()+432)&&
        std::equal(request.request_context_uuid.bytes.begin(),request.request_context_uuid.bytes.end(),b.begin()+448)&&
        std::equal(request.policy_snapshot_uuid.bytes.begin(),request.policy_snapshot_uuid.bytes.end(),b.begin()+464)&&
        std::equal(request.normalized_request_sha256.begin(),request.normalized_request_sha256.end(),b.begin()+480)&&
        b[135]=='2'&&b[136]==2&&b[512]==4&&b[513]==0&&b[514]==1&&b[515]==0,"independent persisted binary intent fields");}
    auto pending=n.Inspect();Check(pending.ok()&&pending.snapshot->watermark.intent==request,"actual intent inspection");
    for(bool plain:{false,true}){Arm();auto r=plain?n.Reserve(*pending.snapshot,301):n.ReserveIntent(*pending.snapshot,301,request);Off();
      Check(!r.ok()&&!r.lease&&r.error==E::operation_pending&&!calls[write_call]&&n.Read()==pending_bytes,"pending request cannot be displaced");}
    for(unsigned field=0;field<7;++field){auto bad=request;auto expected=*pending.snapshot;unsigned op=300;
      if(field==0)bad.initiator_uuid=Id(93);if(field==1)bad.request_context_uuid=Id(93);if(field==2)bad.policy_snapshot_uuid=Id(93);
      if(field==3)bad.normalized_request_sha256[0]^=1;if(field==4)bad.initiator_kind=5;if(field==5)op=301;if(field==6)expected=*initial.snapshot;
      Arm();auto r=n.Resume(expected,op,bad);Off();Check(!r.ok()&&!r.lease&&!calls[write_call]&&!calls[sync_call]&&
        r.error==(field==6?E::stale_base:E::request_mismatch)&&n.Read()==pending_bytes,"resume exact request and base before effects");}
    std::array<unsigned,call_count> resume_calls;
    {Arm();auto resumed=n.Resume(*pending.snapshot,300,request);resume_calls=calls;Off();
      Check(resumed.ok()&&!resume_calls[write_call]&&resume_calls[sync_call]==3&&
        resumed.lease->snapshot().state_sha256==pending.snapshot->state_sha256&&n.Read()==pending_bytes,"resume reacquires durable unchanged reservation");
      std::atomic<bool> entered=false,acquired=false;std::thread contender([&]{entered=true;auto guard=n.device.AcquireOperationGuard();acquired=true;});
      while(!entered.load())std::this_thread::yield();const bool held=!acquired.load();resumed.lease.reset();contender.join();Check(held&&acquired,"resumed lease retains and releases device guard");}
    if(profile==0){
      for(unsigned kind=0;kind<call_count;++kind)for(unsigned at=1;at<=resume_calls[kind];++at){Arm(kind,at);auto r=n.Resume(*pending.snapshot,300,request);Off();
        Check(calls[kind]>=at&&!r.ok()&&!r.lease&&n.Read()==pending_bytes,"every resume fault withholds lease without changing storage");}
      for(unsigned kind=0;kind<call_count;++kind)for(unsigned at=1;at<=reserve_calls[kind];++at){n.Restore(original);Arm(kind,at);auto r=n.ReserveIntent(*initial.snapshot,300,request);Off();
        Check(calls[kind]>=at&&!r.ok()&&!r.lease,"every intent reserve physical/hash fault");const auto recovered=n.Recover();
        Check(recovered.ok()&&((recovered.snapshot->watermark.watermark==1&&!recovered.snapshot->watermark.intent)||
          (recovered.snapshot->watermark.watermark==2&&recovered.snapshot->watermark.intent==request)),"recovery preserves exact surviving request");
        UnchangedExceptWatermarks(original,n.Read(),n);}
      for(unsigned slot=1;slot<=2;++slot)for(unsigned prefix:{1u,432u,500u,520u,640u}){n.Restore(original);Arm();torn_at=slot;torn_bytes=prefix;
        auto r=n.ReserveIntent(*initial.snapshot,300,request);torn_at=0;Off();Check(!r.ok()&&!r.lease,"torn intent never yields lease");
        const auto recovered=n.Recover();Check(recovered.ok()&&(!recovered.snapshot->watermark.intent||recovered.snapshot->watermark.intent==request),"torn intent retains exact recoverable provenance");}
      n.Restore(pending_bytes);InstallSelectedCheckpointFixture(n);
      const auto selected=n.Inspect();Check(selected.ok()&&selected.snapshot->selection.checkpoint_generation==2,"intent follows independently selected checkpoint fixture");
      auto following=request;following.request_context_uuid=Id(94);following.normalized_request_sha256.fill(95);
      Arm(write_call,2);auto interrupted=n.ReserveIntent(*selected.snapshot,301,following);Off();Check(!interrupted.ok()&&!interrupted.lease,"interrupt next intent after prior selection");
      const auto recovered=n.Recover();Check(recovered.ok()&&recovered.snapshot->watermark.watermark==3&&recovered.snapshot->watermark.intent==following,"next intent lineage survives ordered recovery");
      {auto resumed=n.Resume(*recovered.snapshot,301,following);Check(resumed.ok()&&resumed.lease->snapshot().watermark.base_checkpoint_generation==2,"resume next request against actual newer checkpoint");}
      std::cout<<"intent reserve sites=";for(auto v:reserve_calls)std::cout<<v<<',';std::cout<<" resume sites=";for(auto v:resume_calls)std::cout<<v<<',';std::cout<<'\n';
    }
    if(profile==0){n.Restore(original);Check(n.device.Close().ok(),"release before intent process-loss fault");
      const auto killed=fork();Check(killed>=0,"fork intent writer");if(killed==0){Node owned(path,profile,false);const auto base=owned.Inspect();
        if(!base.ok())_exit(94);Arm();kill_second=true;(void)owned.ReserveIntent(*base.snapshot,300,request);_exit(95);}
      int exit_status=0;Check(waitpid(killed,&exit_status,0)==killed&&WIFEXITED(exit_status)&&WEXITSTATUS(exit_status)==86,"intent process lost after first durable replica");
      {Node recovering(path,profile,false);Check(!recovering.Inspect().ok(),"mixed intent replica is not silently promoted");
        const auto recovered=recovering.Recover();Check(recovered.ok()&&recovered.snapshot->watermark.watermark==2&&recovered.snapshot->watermark.intent==request,"recovery binds actual surviving intent after process loss");}
    }else {n.Restore(pending_bytes);Check(n.device.Close().ok(),"release intent node before fresh executable");}
    const auto child=fork();Check(child>=0,"intent fresh process fork");if(child==0){const auto p=std::to_string(profile);execl("/proc/self/exe","intent-probe","--intent-probe",path.c_str(),p.c_str(),nullptr);_exit(95);}
    int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable binds and resumes actual intent");
    {Node owned(path,profile,false);const auto state=owned.Inspect();Check(state.ok(),"owned intent after probe");
      Arm();auto wrong=db::ResumeNativePublicationGenerationOnOpenDevices(Id(99),owned.devices,Id(2),*state.snapshot,Id(300),request,owned.budget);Off();
      Check(!wrong.ok()&&!wrong.lease&&!calls[write_call]&&!calls[sync_call],"resume refuses foreign database binding");
      Check(owned.device.Close().ok()&&owned.device.Open(path.string(),disk::FileOpenMode::open_existing_read_only).ok(),"open intent read-only");
      Arm();auto readonly=owned.Resume(*state.snapshot,300,request);Off();Check(!readonly.ok()&&!readonly.lease&&!calls[write_call]&&!calls[sync_call],"read-only intent cannot issue resumed lease");}
  }
}
}
void* operator new(std::size_t n){if(count_allocations&&++allocations==allocation_fault)throw std::bad_alloc();
  if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* b,size_t n,off_t o){if(Hit(read_call)){errno=EIO;return -1;}const auto r=__real_pread(fd,b,n,o);
  if(armed&&r==static_cast<ssize_t>(n)){if(static_cast<u64>(o)==first_offset&&(phase==2||(phase==0&&calls[sync_call]>=2)))phase=3;
    if(static_cast<u64>(o)==second_offset&&phase==5)phase=6;}return r;}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* b,size_t n,off_t o){
  if(Hit(write_call)){errno=EIO;return -1;}
  if(armed&&static_cast<u64>(o)==second_offset){ordered&=phase==3;if(kill_second)_exit(ordered?86:89);}
  if(armed&&torn_at&&calls[write_call]==torn_at){const auto ignored=__real_pwrite(fd,b,std::min<std::size_t>(n,torn_bytes),o);(void)ignored;errno=EIO;return -1;}
  const auto r=__real_pwrite(fd,b,n,o);if(armed&&r==static_cast<ssize_t>(n)){
    if(static_cast<u64>(o)==first_offset)phase=1;if(static_cast<u64>(o)==second_offset)phase=4;}return r;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){if(Hit(sync_call)){errno=EIO;return -1;}const auto r=__real_fsync(fd);
  if(armed&&r==0){if(phase==1)phase=2;if(phase==4)phase=5;}return r;}
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* p,size_t n,unsigned char* out,unsigned int* size,const EVP_MD* md,ENGINE* e){
  if(Hit(digest_call))return 0;return __real_EVP_Digest(p,n,out,size,md,e);}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){return Hit(context_call)?nullptr:__real_EVP_MD_CTX_new();}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* md,ENGINE* e){return Hit(init_call)?0:__real_EVP_DigestInit_ex(c,md,e);}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* p,size_t n){return Hit(update_call)?0:__real_EVP_DigestUpdate(c,p,n);}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* out,unsigned int* n){return Hit(final_call)?0:__real_EVP_DigestFinal_ex(c,out,n);}
int main(int argc,char** argv){try{
  Fixture fixture;
  if(argc==4&&std::string_view(argv[1])=="--intent-probe"){
    const unsigned profile=std::stoul(argv[3]);Check(profile<5,"intent probe profile");Node n(argv[2],profile,false);
    const auto before=n.Read();const auto state=n.Inspect();Check(state.ok()&&state.snapshot->watermark.intent==Intent(),"fresh intent actual metadata");
    {auto resumed=n.Resume(*state.snapshot,300,Intent());Check(resumed.ok()&&resumed.lease->snapshot().watermark.watermark==2,"fresh process resumed existing generation");}
    Check(n.Read()==before,"fresh resume leaves entire node unchanged");return 0;
  }
  if(argc>=2&&std::string_view(argv[1])=="--allocations"){
    const unsigned shard=argc>=3?std::stoul(argv[2]):0;Check(shard<4,"allocation shard");
    const std::string_view mode=argc==4?argv[3]:"reserve";
    Check(mode=="reserve"||mode=="reserve_intent"||mode=="resume_intent"||mode=="recover_first"||mode=="recover_second"||mode=="recover_stable","allocation operation profile");
    const auto path=fixture.Next();Node n(path,0);const auto original=n.Read();const auto initial=n.Inspect();Check(initial.ok(),"allocation base");
    auto starting=original;
    std::optional<db::NativePublicationSnapshot> resume_base;
    if(mode=="resume_intent"){{auto r=n.ReserveIntent(*initial.snapshot,80,Intent());Check(r.ok(),"durable intent allocation baseline");}
      starting=n.Read();const auto observed=n.Inspect();Check(observed.ok(),"resume allocation base");resume_base=observed.snapshot;}
    if(mode=="recover_first"||mode=="recover_second"){const auto slot=n.first+(mode=="recover_second"?1:0);
      std::fill(starting.begin()+slot*n.size,starting.begin()+(slot+1)*n.size,0);}
    const auto invoke=[&](Node& node){if(mode=="reserve"||mode=="reserve_intent"||mode=="resume_intent"){
      auto r=mode=="resume_intent"?node.Resume(*resume_base,80,Intent()):mode=="reserve_intent"?node.ReserveIntent(*initial.snapshot,80,Intent()):node.Reserve(*initial.snapshot,80);return std::pair{r.ok(),r.lease!=nullptr};}
      const auto r=node.Recover();return std::pair{r.ok(),r.snapshot.has_value()};};
    n.Restore(starting);count_allocations=true;allocations=0;allocation_fault=1;const auto warm=invoke(n);count_allocations=false;Check(!warm.first&&!warm.second,"warm failure path");
    Check(n.device.Close().ok(),"release node for allocation children");
    int channel[2];Check(pipe(channel)==0,"allocation count pipe");const auto measured=fork();Check(measured>=0,"baseline fork");
    if(measured==0){close(channel[0]);Node owned(path,0,false);owned.Restore(starting);
      count_allocations=true;allocations=0;allocation_fault=0;const auto r=invoke(owned);count_allocations=false;
      if(!r.first||!r.second)_exit(81);const auto count=write(channel[1],&allocations,sizeof allocations);_exit(count==sizeof allocations?0:82);}
    close(channel[1]);unsigned sites=0;const auto count=read(channel[0],&sites,sizeof sites);close(channel[0]);int status=0;
    Check(waitpid(measured,&status,0)==measured&&WIFEXITED(status)&&WEXITSTATUS(status)==0&&count==sizeof sites&&sites,"measured reopened-device allocation sites");
    for(unsigned at=1+shard;at<=sites;at+=4){const auto child=fork();Check(child>=0,"allocation fault fork");
      if(child==0){Node owned(path,0,false);owned.Restore(starting);
        count_allocations=true;allocations=0;allocation_fault=at;const auto r=invoke(owned);count_allocations=false;
        if(allocations<at||r.first||r.second){std::cerr<<"allocation site="<<at<<" observed="<<allocations<<'\n';_exit(83);}
        const auto recovered=owned.Recover();_exit(recovered.ok()&&(mode=="reserve"||mode=="reserve_intent"||owned.Read()==(mode=="resume_intent"?starting:original))?0:84);}
      Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"every reached allocation failure withholds lease and remains recoverable");}
    std::cout<<"PASS coordinator allocation sites="<<sites<<" mode="<<mode<<" shard="<<shard<<" checks="<<checks<<'\n';return 0;
  }
  IntentReservations(fixture);
  for(unsigned profile=0;profile<5;++profile){const auto path=fixture.Next();Node n(path,profile);const auto initial=n.Read();
    const auto inspected=n.Inspect();Check(inspected.ok()&&inspected.snapshot->watermark.watermark==1,"actual bound initial watermark");
    auto reserved=n.Reserve(*inspected.snapshot,10);Check(reserved.ok(),"actual durable reservation");
    const auto& state=reserved.lease->snapshot();Check(state.watermark.watermark==2&&state.watermark.previous_watermark==1&&
      state.watermark.operation_uuid==Id(10)&&state.watermark.previous_state_sha256==inspected.snapshot->state_sha256&&
      state.selection.checkpoint_generation==1,"generation advances without fake checkpoint/transaction publication");
    std::atomic<bool> entered=false,acquired=false;
    std::thread contender([&]{entered=true;auto guard=n.device.AcquireOperationGuard();acquired=true;});
    while(!entered.load())std::this_thread::yield();const bool held=!acquired.load();
    reserved.lease.reset();contender.join();Check(held,"lease retains device guard");
    Check(acquired.load(),"lease destruction releases ownership");
    const auto after=n.Read();UnchangedExceptWatermarks(initial,after,n);
    Arm();const auto stale=n.Reserve(*inspected.snapshot,11);Off();Check(!stale.ok()&&!stale.lease&&
      stale.error==db::NativePublicationError::stale_base&&!calls[write_call],"stale exact base refuses before writes");
    Check(n.device.Close().ok(),"close before independent reopen");
    {Node reopened(path,profile,false);auto observed=reopened.Inspect();Check(observed.ok()&&observed.snapshot->watermark.watermark==2,"durable reservation after reopen");
      auto next=reopened.Reserve(*observed.snapshot,11);Check(next.ok()&&next.lease->snapshot().watermark.watermark==3,"unpublished generation is not reused");}
  }
  const auto path=fixture.Next();Node n(path,0);const auto original=n.Read();const auto initial=n.Inspect();Check(initial.ok(),"fault base");
  std::array<unsigned,call_count> baseline{};
  {Arm();auto r=n.Reserve(*initial.snapshot,20);baseline=calls;Off();Check(r.ok()&&ordered&&phase==6,"measured real reservation and individual durability/readback barriers");}
  for(unsigned kind=0;kind<call_count;++kind)for(unsigned at=1;at<=baseline[kind];++at){n.Restore(original);Arm(kind,at);auto r=n.Reserve(*initial.snapshot,20);Off();
    Check(calls[kind]>=at&&!r.ok()&&!r.lease,"every measured physical/hash failure returns no lease");
    UnchangedExceptWatermarks(original,n.Read(),n);const auto recovered=n.Recover();Check(recovered.ok(),"actual interrupted reservation recoverable");
    const auto w=recovered.snapshot->watermark.watermark;Check(w==1||w==2,"recovery neither invents nor drops a durable attempt");
    auto next=n.Reserve(*recovered.snapshot,21);Check(next.ok()&&next.lease->snapshot().watermark.watermark==w+1,"retry uses recovered high watermark");
  }
  for(unsigned slot=1;slot<=2;++slot)for(unsigned prefix:{1u,200u,420u,512u}){n.Restore(original);Arm();torn_at=slot;torn_bytes=prefix;
    auto failed=n.Reserve(*initial.snapshot,30);torn_at=0;Off();Check(!failed.ok()&&!failed.lease,"partial physical write has no lease");
    const auto bytes=n.Read();const auto first=db::DecodeNativePublicationWatermark(Page(bytes,n.first,n.size));
    const auto second=db::DecodeNativePublicationWatermark(Page(bytes,n.first+1,n.size));
    const u64 expected=first.ok()?first.state->watermark:second.state->watermark;
    const auto recovered=n.Recover();Check(recovered.ok()&&recovered.snapshot->watermark.watermark==expected,"torn slot recovery preserves surviving durable state");
    UnchangedExceptWatermarks(original,n.Read(),n);
  }
  n.Restore(original);Arm(write_call,2);auto interrupted=n.Reserve(*initial.snapshot,32);Off();Check(!interrupted.ok(),"real interrupted repair baseline");
  const auto pending_bytes=n.Read();Arm();const auto repair=n.Recover();const auto repair_sites=calls;Off();Check(repair.ok(),"measured actual repair");
  for(unsigned kind=0;kind<call_count;++kind)for(unsigned at=1;at<=repair_sites[kind];++at){n.Restore(pending_bytes);Arm(kind,at);
    const auto r=n.Recover();Off();Check(calls[kind]>=at&&!r.ok()&&!r.snapshot,"every measured recovery I/O/hash failure is empty");
    const auto retried=n.Recover();Check(retried.ok()&&retried.snapshot->watermark.watermark==2,"retry observes real partial repair without lowering watermark");
    UnchangedExceptWatermarks(original,n.Read(),n);
  }
  for(unsigned mode=0;mode<4;++mode){auto bytes=pending_bytes;
    if(mode==0){auto first=Page(bytes,n.first,n.size),second=Page(bytes,n.first+1,n.size);
      // Preserve each physical header while reversing semantic generation order.
      auto a=db::DecodeNativePublicationWatermark(first),b=db::DecodeNativePublicationWatermark(second);
      const auto ah=a.state->header,bh=b.state->header;a.state->header=bh;b.state->header=ah;
      Replace(bytes,n.first,db::EncodeNativePublicationWatermark(*b.state).bytes);
      Replace(bytes,n.first+1,db::EncodeNativePublicationWatermark(*a.state).bytes);}
    if(mode==1){auto w=db::DecodeNativePublicationWatermark(Page(bytes,n.first,n.size));w.state->watermark=3;w.state->previous_watermark=2;
      Replace(bytes,n.first,db::EncodeNativePublicationWatermark(*w.state).bytes);}
    if(mode==2)std::fill(bytes.begin()+n.first*n.size,bytes.begin()+(n.first+2)*n.size,0);
    if(mode==3){auto w=db::DecodeNativePublicationWatermark(Page(bytes,n.first,n.size));
      w.state->operation_uuid=Id(33);auto second=db::DecodeNativePublicationWatermark(Page(bytes,n.first+1,n.size));
      w.state->header=second.state->header;Replace(bytes,n.first+1,db::EncodeNativePublicationWatermark(*w.state).bytes);}
    n.Restore(bytes);Arm();const auto r=n.Recover();Off();Check(!r.ok()&&!r.snapshot&&!calls[write_call]&&n.Read()==bytes,"reversed, nonadjacent, lost or conflicting histories refuse unchanged");
  }
  n.Restore(original);
  for(unsigned mode=0;mode<4;++mode){Uuid op;
    if(mode==1){op=Id(80);op.bytes[6]=0x40;}
    if(mode==2)op=initial.snapshot->watermark.operation_uuid;
    if(mode==3)op=initial.snapshot->selection.publication_uuid;
    Arm();auto r=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),n.devices,Id(2),*initial.snapshot,op,n.budget);Off();
    Check(!r.ok()&&!r.lease&&!calls[write_call]&&n.Read()==original,"nil, data-version and reused operation identities refuse");
  }
  {disk::FileDevice foreign;const auto foreign_path=fixture.Next();Check(foreign.Open(foreign_path.string(),disk::FileOpenMode::create_new).ok(),"foreign node fixture");
    auto request=Request(1);request.bootstrap.database_uuid=Id(95);request.bootstrap.filespace_uuid=Id(96);
    Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(foreign,request,256ULL*request.bootstrap.page_size_bytes).ok(),"actual foreign database graph");
    auto supplied=n.devices;supplied.push_back({Id(96),request.bootstrap.page_size_profile_uuid,&foreign});
    Arm();const auto r=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),supplied,Id(2),n.budget);Off();
    Check(!r.ok()&&!r.snapshot&&!calls[write_call],"unused supplied device cannot cross database identity boundary");}
  {disk::FileDevice secondary;const auto secondary_path=fixture.Next();Check(secondary.Open(secondary_path.string(),disk::FileOpenMode::create_new).ok(),"same-node secondary fixture");
    auto request=Request(1);request.bootstrap.filespace_uuid=Id(97);request.bootstrap.filespace_role=5;
    Check(db::InitializeNativeFilespaceOnOpenDevice(secondary,request,256ULL*request.bootstrap.page_size_bytes).ok(),"actual secondary filespace initialization");
    auto supplied=n.devices;supplied.push_back({Id(97),request.bootstrap.page_size_profile_uuid,&secondary});
    for(unsigned fault=0;fault<3;++fault){n.Restore(original);Arm(fault?sync_call:call_count,fault);
      auto r=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),supplied,Id(2),*initial.snapshot,Id(98),n.budget);Off();
      if(fault)Check(!r.ok()&&!r.lease&&!calls[write_call]&&n.Read()==original,"every writable dependency sync precedes all reservation writes");
      else Check(r.ok()&&calls[sync_call]==4&&calls[write_call]==2&&ordered&&phase==6,"mixed-profile dependencies sync before the two primary slot barriers");}
  }
  for(unsigned mode=0;mode<10;++mode){n.Restore(original);auto bad=*initial.snapshot;
    if(mode==0)bad.state_sha256[0]^=1;if(mode==1)++bad.watermark.watermark;
    if(mode==2)++bad.selection.selection_generation;if(mode==3)bad.selection.publication_uuid=Id(91);
    if(mode==4)bad.selection.checkpoint_sha256[0]^=1;if(mode==5)++bad.selection.checkpoint.page_generation;
    if(mode==6)bad.selection.checkpoint_object_uuid=Id(92);if(mode==7)++bad.selection.root_set_generation;
    if(mode==8)bad.selection.timeline_uuid=Id(93);if(mode==9)bad.watermark.header.database_uuid=Id(94);
    Arm();auto r=n.Reserve(bad,40);Off();Check(!r.ok()&&!r.lease&&!calls[write_call]&&n.Read()==original,"all changed CAS identities refuse without mutation");
  }
  for(unsigned mode=0;mode<4;++mode){n.Restore(original);auto bytes=original;
    auto w=db::DecodeNativePublicationWatermark(Page(bytes,n.first,n.size));Check(w.ok(),"resealed mutation setup");
    if(mode==0)w.state->bootstrap_uuid=Id(50);if(mode==1)w.state->base_checkpoint_sha256[0]^=1;
    if(mode==2)w.state->operation_uuid=Id(51);if(mode==3)w.state->header.page_uuid=Id(52);
    auto e=db::EncodeNativePublicationWatermark(*w.state);Check(e.ok(),"structurally valid misbound state");Replace(bytes,n.first,e.bytes);n.Restore(bytes);
    Arm();const auto r=n.Recover();Off();Check(!r.ok()&&!r.snapshot&&!calls[write_call]&&n.Read()==bytes,"misbound valid image is never a repair candidate");
  }
  n.Restore(original);
  for(unsigned mode=0;mode<6;++mode){auto bytes=original;
    for(unsigned slot=0;slot<2;++slot){auto w=db::DecodeNativePublicationWatermark(Page(bytes,n.first+slot,n.size));Check(w.ok(),"paired binding fixture decode");
      if(mode==0)w.state->bootstrap_uuid=Id(53);
      if(mode==1)w.state->base_checkpoint_sha256[0]^=1;
      if(mode==2)w.state->operation_uuid=Id(54);
      if(mode==3)w.state->timeline_uuid=Id(57);
      if(mode==4)w.state->base_root_set_generation=2;
      if(mode==5)w.state->base_checkpoint.page_generation=2;
      auto e=db::EncodeNativePublicationWatermark(*w.state);Check(e.ok(),"paired valid resealed image");Replace(bytes,n.first+slot,e.bytes);}
    Check(db::ClassifyNativePublicationWatermarkPair(Page(bytes,n.first,n.size),Page(bytes,n.first+1,n.size)).ok(),"image-only pair really agrees");
    n.Restore(bytes);Arm();const auto r=n.Recover();Off();
    Check(!r.ok()&&!r.snapshot&&!calls[write_call]&&n.Read()==bytes,"equal images cannot substitute for actual bootstrap/checkpoint authority");}
  n.Restore(original);
  {auto r=n.Reserve(*initial.snapshot,55);Check(r.ok(),"actual reservation before selected-successor fixture");}
  InstallSelectedCheckpointFixture(n);
  {const auto selected=n.Inspect();if(!selected.ok()){const auto bound=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),n.devices,Id(2),n.budget);
      std::cerr<<"fixture admission error="<<static_cast<unsigned>(selected.error)<<" selector="<<static_cast<unsigned>(bound.error)<<" checkpoint="<<static_cast<unsigned>(bound.checkpoint_error)<<" allocation="<<static_cast<unsigned>(bound.allocation_error)<<'\n';}
    Check(selected.ok()&&selected.snapshot->selection.checkpoint_generation==2&&
      selected.snapshot->watermark.base_checkpoint_generation==1,"published watermark binds actual selected predecessor");
    auto successor=n.Reserve(*selected.snapshot,56);Check(successor.ok()&&successor.lease->snapshot().watermark.watermark==3&&
      successor.lease->snapshot().watermark.base_checkpoint_generation==2,"next reservation captures actual newer current checkpoint");}
  n.Restore(original);
  {auto bytes=original;for(unsigned i=0;i<2;++i){auto w=db::DecodeNativePublicationWatermark(Page(bytes,n.first+i,n.size));
      w.state->watermark=std::numeric_limits<u64>::max();w.state->previous_watermark=w.state->watermark-1;
      w.state->operation_uuid=Id(60);w.state->previous_state_sha256[0]=1;auto e=db::EncodeNativePublicationWatermark(*w.state);Check(e.ok(),"maximum watermark fixture");Replace(bytes,n.first+i,e.bytes);}
    n.Restore(bytes);const auto max=n.Inspect();Check(max.ok(),"maximum is representable");Arm();auto r=n.Reserve(*max.snapshot,61);Off();
    Check(r.error==db::NativePublicationError::generation_exhausted&&!r.lease&&!calls[write_call]&&n.Read()==bytes,"exhaustion cannot wrap or mutate");}
  n.Restore(original);Check(n.device.Close().ok(),"release before actual process-loss test");
  const auto child=fork();Check(child>=0,"process-loss fork");
  if(child==0){Node owned(path,0,false);const auto base=owned.Inspect();if(!base.ok())_exit(87);second_offset=(owned.first+1)*owned.size;
    Arm();kill_second=true;(void)owned.Reserve(*base.snapshot,70);_exit(88);}
  int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"process lost after first durable replica");
  {Node owned(path,0,false);const auto before=owned.Read();const auto pending=owned.Inspect();Check(!pending.ok()&&!pending.snapshot&&pending.error==db::NativePublicationError::repair_required,"read-only inspection cannot promote adjacent replicas");
    auto recovered=owned.Recover();Check(recovered.ok()&&recovered.snapshot->watermark.watermark==2&&recovered.snapshot->watermark.operation_uuid==Id(70),"new process preserves unacknowledged reservation");
    UnchangedExceptWatermarks(before,owned.Read(),owned);auto next=owned.Reserve(*recovered.snapshot,71);Check(next.ok()&&next.lease->snapshot().watermark.watermark==3,"new process cannot reuse burned generation");}
  {Node readonly(path,0,false);Check(readonly.device.Close().ok()&&readonly.device.Open(path.string(),disk::FileOpenMode::open_existing_read_only).ok(),"actual read-only reopen");
    const auto observed=readonly.Inspect();Check(observed.ok(),"read-only stable inspection");
    Arm();auto r=readonly.Reserve(*observed.snapshot,72);const auto repair=readonly.Recover();Off();
    Check(!r.ok()&&!r.lease&&!repair.ok()&&!repair.snapshot&&!calls[write_call],"read-only device cannot reserve or repair");}
  std::cout<<"PASS native publication coordinator checks="<<checks<<" sites=";for(auto n:baseline)std::cout<<n<<',';std::cout<<'\n';return 0;
}catch(const std::exception& e){Off();count_allocations=false;std::cerr<<"FAIL coordinator checks="<<checks<<": "<<e.what()<<'\n';return 1;}}

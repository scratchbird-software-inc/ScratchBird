// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "hash_digest.hpp"
#include "memory.hpp"
#include "transaction_inventory_page.hpp"
#include "transaction_recovery.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
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
void Check(bool b,const char* why){++checks;if(!b)throw std::runtime_error(why);}
Uuid Id(unsigned n){Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[14]=n>>8;id.bytes[15]=n;return id;}
constexpr u64 millis=1789357072000ULL;
bool armed=false,stop_activation=false;
unsigned replacements=0,fail_replace=0,hashes=0,fail_hash=0,writes=0,fail_write=0,syncs=0,fail_sync=0;
u64 target_id=0;Uuid target_uuid=Id(100);std::array<u32,4> phases{};std::array<u16,4> states{};std::array<u64,4> generations{};
std::atomic<bool> pause_activation=false,paused=false,resume=false;
std::vector<byte> Get(const fs::path& path){std::ifstream in(path,std::ios::binary);Check(bool(in),"read actual artifact");
  return {std::istreambuf_iterator<char>(in),{}};}
void Put(const fs::path& path,const std::vector<byte>& bytes){std::ofstream out(path,std::ios::binary|std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());out.close();Check(bool(out),"write owned fixture bytes");}
void Arm(){replacements=hashes=writes=syncs=0;fail_replace=fail_hash=fail_write=fail_sync=0;phases={};states={};generations={};armed=true;}
struct Fixture {
  fs::path directory,path,journal;u32 page_size=0;
  Fixture(){char name[]="/tmp/sb-starting-publication.XXXXXX";const auto* p=mkdtemp(name);Check(p,"fixture directory");directory=p;path=directory/"node";journal=path.string()+".sb.txn_publish";
    db::DatabaseCreateConfig config;config.path=path.string();config.database_uuid={UuidKind::database,Id(1)};config.filespace_uuid={UuidKind::filespace,Id(2)};
    config.creation_unix_epoch_millis=millis;config.resource_seed_pack_root=SB_BOOTSTRAP_SEED_PACK_ROOT;config.require_resource_seed_pack=true;
    config.bootstrap_principal_name="ROOT";config.require_bootstrap_principal=true;
    config.bootstrap_credential_fingerprint="local-password-pbkdf2-sha256:v1:iterations=600000:salt=89abcdef0123456789abcdef01234567:verifier=abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    const auto result=db::CreateDatabaseFile(config);if(!result.ok())std::cerr<<result.diagnostic.diagnostic_code<<'\n';
    Check(result.ok(),"actual database fixture creation");page_size=result.state.header.page_size;
  }
  ~Fixture(){std::error_code ec;fs::remove_all(directory,ec);}
};
const mga::TransactionInventoryEntry& Entry(const mga::LocalTransactionInventory& inv,u64 id){
  const auto at=std::find_if(inv.entries.begin(),inv.entries.end(),[&](const auto& e){return e.identity.local_id.value==id;});
  Check(at!=inv.entries.end(),"actual allocated identity retained");return *at;
}
void Starting(const db::LocalTransactionStoreResult& loaded,u64 id,bool read_only=false){
  Check(loaded.ok()&&loaded.inventory.next_local_transaction_id>id&&loaded.inventory.publication_base.has_value(),"actual selected allocation and provenance");
  const auto& entry=Entry(loaded.inventory,id);Check(entry.state==mga::TransactionState::created&&entry.identity.transaction_uuid.value==Id(100)&&
    !entry.commit_sequence&&!entry.final_unix_epoch_millis&&!entry.evidence_record_written,"starting is neither active nor committed");
  Check(loaded.horizons.oldest_interesting_transaction.value==id&&loaded.horizons.oldest_active_transaction.value>id,
    "starting pins OIT but not active concurrency");(void)read_only;
}
}
extern "C" void __real__ZNSt10filesystem6renameERKNS_7__cxx114pathES3_RSt10error_code(const fs::path&,const fs::path&,std::error_code&);
extern "C" void __wrap__ZNSt10filesystem6renameERKNS_7__cxx114pathES3_RSt10error_code(const fs::path& from,const fs::path& to,std::error_code& ec){
  if(armed){++replacements;const auto bytes=Get(from);Check(bytes.size()>=144&&std::equal(bytes.begin(),bytes.begin()+8,"SBTXP005"),"actual binary publication carrier");
    const auto old_count=LoadLittle64(bytes.data()+48),new_count=LoadLittle64(bytes.data()+56);
    Check(old_count+new_count==(bytes.size()-144)/72,"independent carrier count framing");
    if(replacements<=4){phases[replacements-1]=LoadLittle32(bytes.data()+12);generations[replacements-1]=LoadLittle64(bytes.data()+16);
      for(u64 i=0;i<new_count;++i){const auto* e=bytes.data()+112+72*(old_count+i);
        if(LoadLittle64(e)==target_id){states[replacements-1]=LoadLittle16(e+26);Check(std::equal(e+8,e+24,target_uuid.bytes.begin()),"same binary UUID in both publications");}}
    }
    if(replacements==3){if(stop_activation)_exit(86);
      if(pause_activation.exchange(false)){paused=true;while(!resume.load())std::this_thread::yield();}}
    if(replacements==fail_replace){ec=std::make_error_code(std::errc::io_error);return;}
  }
  __real__ZNSt10filesystem6renameERKNS_7__cxx114pathES3_RSt10error_code(from,to,ec);
}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t o){if(armed&&++writes==fail_write){errno=EIO;return -1;}return __real_pwrite(fd,p,n,o);}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){if(armed&&++syncs==fail_sync){errno=EIO;return -1;}return __real_fsync(fd);}
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* p,size_t n,unsigned char* out,unsigned int* s,const EVP_MD* t,ENGINE* e){
  if(armed&&++hashes==fail_hash)return 0;return __real_EVP_Digest(p,n,out,s,t,e);}

int main(int argc,char** argv){try{
  auto policy=scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();policy.policy_name="transaction_starting_publication_test";
  Check(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(policy,"transaction_starting_publication_test").ok(),"fixture memory policy");
  if(argc==5&&std::string_view(argv[1])=="--inspect"){
    disk::FileDevice d;Check(d.Open(argv[2],disk::FileOpenMode::open_existing_read_only).ok(),"fresh process opens interrupted node");
    Starting(db::LoadLocalTransactionInventoryFromOpenDevice(&d,std::stoul(argv[3])),std::stoull(argv[4]));return 0;
  }
  Fixture f;disk::FileDevice device;Check(device.Open(f.path.string(),disk::FileOpenMode::open_existing).ok(),"owned database");
  const auto initial=db::LoadLocalTransactionInventoryFromOpenDevice(&device,f.page_size);Check(initial.ok(),"actual initial inventory");
  Check(db::PersistLocalTransactionInventoryToOpenDevice(&device,f.page_size,initial.inventory).ok(),"establish actual equal-state publication carrier");
  const auto base=db::LoadLocalTransactionInventoryFromOpenDevice(&device,f.page_size);Check(base.ok()&&base.inventory.publication_base,"actual selected starting base");
  target_id=base.inventory.next_local_transaction_id;
  const auto original=Get(f.path),journal=Get(f.journal);
  {scratchbird::storage::page::TransactionInventoryPageBody body;body.page_number=7;body.inventory=base.inventory;
    const auto valid=scratchbird::storage::page::BuildTransactionInventoryPageBody(body,f.page_size);Check(valid.ok(),"actual native-v3 codec fixture");
    Arm();fail_hash=2;const auto failed=scratchbird::storage::page::BuildTransactionInventoryPageBody(body,f.page_size);armed=false;
    Check(!failed.ok()&&failed.serialized.empty(),"body checksum provider failure returns no encoded image");
    auto zero_digest=valid.serialized;std::fill(zero_digest.begin()+64,zero_digest.begin()+96,0);
    Arm();fail_hash=1;const auto decoded=scratchbird::storage::page::ParseTransactionInventoryPageBody(zero_digest,7);armed=false;
    Check(!decoded.ok()&&decoded.serialized.empty()&&decoded.body.inventory.entries.empty(),"provider failure is not a matching zero checksum or partial decoded authority");}
  const auto restore=[&]{armed=false;const auto w=device.WriteAt(0,original.data(),original.size());Check(w.ok()&&w.bytes_transferred==original.size(),"restore owned main bytes");Put(f.journal,journal);Check(device.Sync().ok(),"restore sync");};
  const auto begin=[&](bool ro){return ro?mga::BeginLocalReadOnlyTransaction(base.inventory,{UuidKind::transaction,Id(100)},millis+1):
      mga::BeginLocalTransaction(base.inventory,{UuidKind::transaction,Id(100)},millis+1);};
  for(bool ro:{false,true}){restore();const auto candidate=begin(ro);Check(candidate.ok(),"pure candidate");Arm();
    const auto published=db::PersistLocalTransactionInventoryToOpenDevice(&device,f.page_size,candidate.inventory);armed=false;
    Check(published.ok()&&replacements==4&&phases==std::array<u32,4>{1,2,1,2}&&
      states==std::array<u16,4>{1,1,static_cast<u16>(ro?13:2),static_cast<u16>(ro?13:2)},"starting durable before active or read-only-active publication");
    const auto g=base.inventory.publication_base->generation;
    Check(generations==std::array<u64,4>{g+1,g+1,g+2,g+2}&&published.inventory.publication_base->generation==g+2,"independent allocation/activation attempts");
    const auto actual=db::LoadLocalTransactionInventoryFromOpenDevice(&device,f.page_size);Check(actual.ok()&&Entry(actual.inventory,target_id).state==(ro?mga::TransactionState::read_only_active:mga::TransactionState::active),"actual selected active result");
    const auto sites=std::array<unsigned,4>{replacements,writes,syncs,hashes};
    std::vector<unsigned> recovered_hash_sites;
    for(unsigned kind=0;kind<sites.size();++kind)for(unsigned at=1;at<=sites[kind];++at){restore();Arm();
      if(kind==0)fail_replace=at;if(kind==1)fail_write=at;if(kind==2)fail_sync=at;if(kind==3)fail_hash=at;
      const auto failure=db::PersistLocalTransactionInventoryToOpenDevice(&device,f.page_size,candidate.inventory);armed=false;
      if(failure.ok()){Check(kind==3&&at!=15&&at!=25&&replacements==4&&phases==std::array<u32,4>{1,2,1,2}&&
          states==std::array<u16,4>{1,1,static_cast<u16>(ro?13:2),static_cast<u16>(ro?13:2)},"only independently recovered primary hash faults may complete both publications");
        recovered_hash_sites.push_back(at);}
      const auto observed=db::LoadLocalTransactionInventoryFromOpenDevice(&device,f.page_size);Check(observed.ok(),"actual recovery after failed publication");
      Check(observed.inventory.next_local_transaction_id==target_id||observed.inventory.next_local_transaction_id==target_id+1,"no forged or regressed transaction counter");
      if(failure.ok())Check(observed.inventory.next_local_transaction_id==target_id+1&&
        Entry(observed.inventory,target_id).state==(ro?mga::TransactionState::read_only_active:mga::TransactionState::active)&&
        observed.inventory.publication_base==failure.inventory.publication_base,"recovered provider fault requires actual complete current authority");
      if(observed.inventory.next_local_transaction_id>target_id){const auto& e=Entry(observed.inventory,target_id);
        Check(e.state==mga::TransactionState::created||e.state==mga::TransactionState::active||e.state==mga::TransactionState::read_only_active,"actual failure leaves starting or already durable active state, never invented rollback");}
    }
    Check(recovered_hash_sites==std::vector<unsigned>{1,2,3,7,8,9,17,18,19},
      "only traced primary checksum/chain/provenance reads recover through independent complete carrier; encoders must fail");
    restore();Arm();fail_replace=3;const auto failed=db::PersistLocalTransactionInventoryToOpenDevice(&device,f.page_size,candidate.inventory);armed=false;
    Check(!failed.ok(),"activation fault");const auto allocated=db::LoadLocalTransactionInventoryFromOpenDevice(&device,f.page_size);Starting(allocated,target_id,ro);
    const auto before=Get(f.journal);Arm();const auto stale=db::PersistLocalTransactionInventoryToOpenDevice(&device,f.page_size,candidate.inventory);armed=false;
    Check(!stale.ok()&&!replacements&&!writes&&Get(f.journal)==before,"pre-allocation base becomes stale without mutation");
    const auto next=mga::BeginLocalTransaction(allocated.inventory,{UuidKind::transaction,Id(101)},millis+2);Check(next.ok()&&next.entry.identity.local_id.value==target_id+1,"failed activation number cannot be reused");
    Check(db::PersistLocalTransactionInventoryToOpenDevice(&device,f.page_size,next.inventory).ok(),"fresh successor admission uses next number");
    const auto retained=db::LoadLocalTransactionInventoryFromOpenDevice(&device,f.page_size);Check(retained.ok()&&Entry(retained.inventory,target_id).state==mga::TransactionState::created&&
      Entry(retained.inventory,target_id+1).state==mga::TransactionState::active,"unresolved starting identity remains retained beside successor");
    std::cout<<"starting ro="<<ro<<" sites";for(auto n:sites)std::cout<<' '<<n;std::cout<<" recovered_primary_hash_sites";for(auto n:recovered_hash_sites)std::cout<<' '<<n;std::cout<<'\n';
  }
  restore();const auto candidate=begin(false);
  {const auto active=db::PersistLocalTransactionInventoryToOpenDevice(&device,f.page_size,candidate.inventory);Check(active.ok(),"autocommit predecessor active");
    const auto committed=mga::CommitLocalTransaction(active.inventory,mga::MakeLocalTransactionId(target_id),millis+2);Check(committed.ok(),"actual no-op transaction commit candidate");
    const auto replacement=mga::BeginLocalTransaction(committed.inventory,{UuidKind::transaction,Id(101)},millis+3);Check(replacement.ok(),"replacement BEGIN candidate");
    const auto predecessor_id=target_id;target_id=replacement.entry.identity.local_id.value;target_uuid=Id(101);Arm();fail_replace=3;
    const auto failed=db::PersistLocalTransactionInventoryToOpenDevice(&device,f.page_size,replacement.inventory);armed=false;
    Check(!failed.ok(),"replacement activation failure");const auto current=db::LoadLocalTransactionInventoryFromOpenDevice(&device,f.page_size);
    Check(current.ok()&&Entry(current.inventory,predecessor_id).state==mga::TransactionState::committed&&
      Entry(current.inventory,predecessor_id).commit_sequence==committed.entry.commit_sequence&&
      Entry(current.inventory,target_id).state==mga::TransactionState::created&&current.inventory.next_local_transaction_id==target_id+1,
      "replacement failure preserves actual preceding commit and consumes replacement number");
    target_id=predecessor_id;target_uuid=Id(100);}
  restore();Check(device.Close().ok(),"release before crash owner");
  const auto child=fork();Check(child>=0,"actual crash fork");
  if(child==0){disk::FileDevice d;if(!d.Open(f.path.string(),disk::FileOpenMode::open_existing).ok())_exit(87);Arm();stop_activation=true;
    (void)db::PersistLocalTransactionInventoryToOpenDevice(&d,f.page_size,candidate.inventory);_exit(88);}
  int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"actual process loss after durable allocation before activation");
  const auto inspector=fork();Check(inspector>=0,"fresh executable fork");
  if(inspector==0){const auto size=std::to_string(f.page_size),id=std::to_string(target_id);execl(argv[0],argv[0],"--inspect",f.path.c_str(),size.c_str(),id.c_str(),nullptr);_exit(127);}
  Check(waitpid(inspector,&status,0)==inspector&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable sees durable starting state and horizons");
  Check(device.Open(f.path.string(),disk::FileOpenMode::open_existing).ok(),"reopen after process loss");restore();
  // The second generation must be available before any first-phase mutation.
  auto exhausted=journal;StoreLittle64(exhausted.data()+16,std::numeric_limits<u64>::max()-1);
  StoreLittle64(exhausted.data()+96,std::numeric_limits<u64>::max()-2);
  const auto digest=scratchbird::core::hash::ComputeSha256Digest(exhausted.data(),exhausted.size()-32);Check(digest.ok(),"independent exhaustion fixture digest");
  std::copy(digest.digest.begin(),digest.digest.end(),exhausted.end()-32);Put(f.journal,exhausted);
  const auto high=db::LoadLocalTransactionInventoryFromOpenDevice(&device,f.page_size);Check(high.ok(),"actual high generation authority");
  const auto overflow=mga::BeginLocalTransaction(high.inventory,{UuidKind::transaction,Id(100)},millis+1);Check(overflow.ok(),"exhaustion candidate");Arm();
  const auto refused=db::PersistLocalTransactionInventoryToOpenDevice(&device,f.page_size,overflow.inventory);armed=false;
  Check(!refused.ok()&&!replacements&&!writes&&Get(f.journal)==exhausted&&Get(f.path)==original,"two-generation exhaustion refuses before mutation");
  restore();pause_activation=true;paused=false;resume=false;std::atomic<bool> entered=false,acquired=false;db::LocalTransactionStoreResult result;Arm();
  std::thread writer([&]{result=db::PersistLocalTransactionInventoryToOpenDevice(&device,f.page_size,candidate.inventory);});
  while(!paused.load())std::this_thread::yield();std::thread contender([&]{entered=true;const auto guard=device.AcquireOperationGuard();acquired=true;});
  while(!entered.load())std::this_thread::yield();Check(!acquired.load(),"single retained guard covers both phases");resume=true;writer.join();contender.join();armed=false;
  Check(result.ok()&&acquired.load(),"activation completes before guard release");
  Check(device.Close().ok(),"release before readonly");Check(device.Open(f.path.string(),disk::FileOpenMode::open_existing_read_only).ok(),"readonly fixture owner");
  Arm();const auto readonly=db::PersistLocalTransactionInventoryToOpenDevice(&device,f.page_size,candidate.inventory);armed=false;
  Check(!readonly.ok()&&!replacements&&!writes,"readonly cannot allocate or activate");
  Check(device.Close().ok()&&device.Open(f.path.string(),disk::FileOpenMode::open_existing).ok(),"multi-page publication owner");restore();
  // Actual binary storage fixture, not a successful cluster operation. The
  // trusted raw publisher checks snapshot consistency, not provider authority.
  {auto cluster=candidate.inventory;cluster.entries.back().identity.scope=mga::TransactionScope::cluster_global;
    const auto active=db::PersistLocalTransactionInventoryToOpenDevice(&device,f.page_size,cluster);
    Check(active.ok(),"persist binary cluster-scope recovery fixture");cluster=active.inventory;
    cluster.entries.back().state=mga::TransactionState::committing;cluster.entries.back().evidence_record_written=true;
    Check(db::PersistLocalTransactionInventoryToOpenDevice(&device,f.page_size,cluster).ok(),"persist interrupted cluster evidence fixture");
    Check(device.Close().ok()&&device.Open(f.path.string(),disk::FileOpenMode::open_existing_read_only).ok(),"reopen cluster recovery fixture readonly");
    const auto loaded=db::LoadLocalTransactionInventoryFromOpenDevice(&device,f.page_size);Check(loaded.ok(),"load actual binary cluster recovery state");
    const auto before=Get(f.path),carrier=Get(f.journal);
    const auto recovered=mga::ApplyLocalTransactionInventoryRecovery(loaded.inventory,millis+10);
    Check(recovered.ok()&&recovered.write_admission_must_remain_fenced&&!recovered.inventory_changed&&
      Entry(recovered.recovered_inventory,target_id).identity.scope==mga::TransactionScope::cluster_global&&
      Entry(recovered.recovered_inventory,target_id).state==mga::TransactionState::committing&&
      !Entry(recovered.recovered_inventory,target_id).commit_sequence&&
      recovered.recovered_inventory.next_commit_sequence==loaded.inventory.next_commit_sequence&&
      recovered.recovered_inventory.publication_base==loaded.inventory.publication_base&&Get(f.path)==before&&Get(f.journal)==carrier,
      "actual reopened cluster evidence cannot become local commit or altered provenance");
    Check(device.Close().ok()&&device.Open(f.path.string(),disk::FileOpenMode::open_existing).ok(),"restore writable owner after cluster read");restore();}
  auto many=base.inventory;const unsigned count=scratchbird::storage::page::MaxTransactionInventoryEntriesPerPage(f.page_size)+1;
  for(unsigned i=0;i<count;++i){auto next=mga::BeginLocalTransaction(std::move(many),{UuidKind::transaction,Id(100+i)},millis+1+i);
    Check(next.ok(),"multi-page active candidate");many=std::move(next.inventory);}
  Arm();fail_replace=3;const auto interrupted=db::PersistLocalTransactionInventoryToOpenDevice(&device,f.page_size,many);armed=false;
  Check(!interrupted.ok(),"multi-page activation interruption");const auto allocated=db::LoadLocalTransactionInventoryFromOpenDevice(&device,f.page_size);
  Check(allocated.ok()&&allocated.inventory.next_local_transaction_id==target_id+count,"all multi-page starting numbers durable");
  for(unsigned i=0;i<count;++i)Check(Entry(allocated.inventory,target_id+i).state==mga::TransactionState::created&&
    Entry(allocated.inventory,target_id+i).identity.transaction_uuid.value==Id(100+i),"no active prefix or lost identity across starting pages");
  many.publication_base=allocated.inventory.publication_base;Arm();const auto activated=db::PersistLocalTransactionInventoryToOpenDevice(&device,f.page_size,many);armed=false;
  Check(activated.ok()&&replacements==2,"exact already allocated identities activate without reallocating numbers");
  const auto final=db::LoadLocalTransactionInventoryFromOpenDevice(&device,f.page_size);Check(final.ok(),"actual multi-page activation load");
  for(unsigned i=0;i<count;++i)Check(Entry(final.inventory,target_id+i).state==mga::TransactionState::active,"every multi-page identity activated");
  std::cout<<"durable starting publication checks="<<checks<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"starting publication failure: "<<e.what()<<" checks="<<checks<<'\n';return 1;}}

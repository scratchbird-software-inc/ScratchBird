// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_creation_workspace.hpp"
#include "native_publication_watermark.hpp"
#include "disk_device.hpp"
#include "physical_mga_cow_store.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <set>
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
bool count_allocations=false,repeat_entropy=false;
unsigned allocations=0,allocation_fault=0,context_fault=0;
std::atomic<bool> pause_write=false,write_paused=false,resume_write=false;
unsigned checks=0;
void Check(bool value,const char* what){++checks;if(!value)throw std::runtime_error(what);}
enum Call {write_call,read_call,sync_call,random_call,hash_call,call_count};
std::array<unsigned,call_count> calls{};
bool armed=false,stop_before_second_selector=false;
bool tamper_watermark_after_publication=false,tamper_done=false;
unsigned fault_kind=call_count,fault_at=0,corrupt_at=0;
unsigned selector_phase=0;
unsigned watermark_written=0,watermark_synced=0,watermark_compared=0;
u64 page_bytes=8192,total_pages=21,selector_page=17;
bool ordered=true;
bool Hit(Call kind){if(!armed)return false;return ++calls[kind]==fault_at&&kind==fault_kind;}
void Arm(unsigned kind=call_count,unsigned at=0){calls={};fault_kind=kind;fault_at=at;corrupt_at=0;selector_phase=0;watermark_written=watermark_synced=watermark_compared=0;ordered=true;armed=true;}
void Disarm(){armed=false;}
Uuid Id(unsigned n){Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[14]=n>>8;id.bytes[15]=n;return id;}
db::NativeFilespaceInitializationRequest Request(unsigned profile=0,u64 total=21){
  const auto& p=disk::kCanonicalFilespacePageProfiles[profile];
  db::NativeFilespaceInitializationRequest r;
  r.bootstrap={Id(1),Id(2),p.uuid,disk::kNativeBootstrapIntegrityProfile,{},p.page_size_bytes,1,0,1,7};
  r.operation_uuid=Id(3);r.writer_uuid=Id(4);
  r.creator.transaction_uuid={UuidKind::transaction,Id(5)};
  r.creator.local_id=mga::MakeLocalTransactionId(1);r.creator.scope=mga::TransactionScope::local_node;
  r.total_pages=total;r.creation_utc_millis=1789357072000ULL;return r;
}
struct Fixture{
  fs::path root;
  unsigned serial=0;
  Fixture(){char name[]="/tmp/sb-native-creation-test.XXXXXX";const auto* p=mkdtemp(name);if(!p)throw std::runtime_error("mkdtemp");root=p;}
  ~Fixture(){std::error_code ignored;fs::remove_all(root,ignored);}
  fs::path Next(){const auto number=std::to_string(serial++);
    return root/("node-"+std::string(12-number.size(),'0')+number);}
};
std::vector<byte> Read(disk::FileDevice& device,u64 n,u64 size){
  std::vector<byte> bytes(size);const auto io=device.ReadAt(n*size,bytes.data(),bytes.size());
  Check(io.ok()&&io.bytes_transferred==bytes.size(),"actual full page read");return bytes;
}
void Inspect(disk::FileDevice& device,const db::NativeFilespaceInitializationRequest& request){
  const auto& b=request.bootstrap;const auto size=b.page_size_bytes;
  const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(device);
  Check(zero.ok(),"actual page zero");const auto& z=*zero.record;
  Check(z.bootstrap.database_uuid==b.database_uuid&&z.bootstrap.filespace_uuid==b.filespace_uuid&&
    z.bootstrap.lifecycle_state==7&&z.bootstrap.filespace_role==b.filespace_role&&z.bootstrap.flags==0,
    "exact initializing bootstrap, never online");
  Check(z.creation_operation_uuid==request.operation_uuid&&z.writer_identity_uuid==request.writer_uuid&&
    z.creation_utc_millis==request.creation_utc_millis&&z.page_generation==1&&z.root_set_generation==1,
    "actual creation lineage");
  const std::vector<disk::NativeFilespaceDevice> devices{{b.filespace_uuid,b.page_size_profile_uuid,&device}};
  const u64 budget=256*size;
  const auto bound=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(b.database_uuid,devices,b.filespace_uuid,budget);
  Check(bound.ok(),"actual duplex/inventory/allocation admission");
  const auto& cp=*bound.checkpoint_inventory.checkpoint;
  const disk::FilespaceRootReference checkpoint{9,0x300,b.filespace_uuid,cp.header.page_number,1,b.page_size_profile_uuid,cp.object_uuid};
  Check(cp.checkpoint_generation==1&&cp.root_set_generation==1&&cp.completed&&cp.flags==0&&
    cp.selected_local_transaction_id==1&&cp.stable_local_transaction_id==1&&cp.local_durable_transaction_id==1&&
    !cp.cluster_quorum_transaction_id&&!cp.predecessor&&cp.creator_transaction_uuid==Id(5)&&cp.creator_local_transaction_id==1,
    "construction checkpoint only");
  const auto& inv=bound.checkpoint_inventory.inventory;
  Check(inv.entries.size()==1&&inv.next_local_transaction_id==2&&inv.next_commit_sequence==2&&!inv.publication_base,
    "no invented active transaction or publication base");
  const auto& entry=inv.entries.front();
  Check(entry.identity.transaction_uuid.value==Id(5)&&entry.identity.scope==mga::TransactionScope::local_node&&
    entry.identity.local_id.value==1&&entry.state==mga::TransactionState::committed&&entry.commit_sequence==1&&
    entry.evidence_record_required&&entry.evidence_record_written&&!entry.rollback_only&&
    entry.begin_unix_epoch_millis==request.creation_utc_millis&&entry.final_unix_epoch_millis==request.creation_utc_millis,
    "real single construction inventory outcome");
  const std::array<u64,5> coverage{60,124,252,507,1017};
  unsigned profile=0;while(disk::kCanonicalFilespacePageProfiles[profile].page_size_bytes!=size)++profile;
  const u64 maps=(request.total_pages+coverage[profile]-1)/coverage[profile],used=maps+20;
  Check(z.total_pages==request.total_pages&&z.free_pages==request.total_pages-used&&!z.preallocated_pages&&
    bound.allocation.pages.size()==maps&&bound.allocation.state_counts[2]==used&&bound.allocation.state_counts[0]==z.free_pages,
    "independent exact coverage and capacity counts");
  const std::array<u32,20> control_types{0,0x300,0x301,9,8,0x303,0x302,5,10,11,6,6,6,6,6,6,0x30e,0x30e,0x500,0x500};
  const auto catalog_image=page::DecodeNativeCatalogRoot(Read(device,maps+7,size));
  Check(catalog_image.ok(),"actual catalog reference owner identities");
  std::map<u64,Uuid> expected_owners{{0,b.filespace_uuid},{maps+1,cp.object_uuid},
    {maps+16,bound.selection->object_uuid},{maps+17,bound.selection->object_uuid}};
  const auto watermark_first=db::DecodeNativePublicationWatermark(Read(device,maps+18,size));
  const auto watermark_second=db::DecodeNativePublicationWatermark(Read(device,maps+19,size));
  Check(watermark_first.ok()&&watermark_second.ok(),"actual duplex watermark dependency pages");
  for(const auto* image:{&watermark_first,&watermark_second}){const auto& w=*image->state;
    Check(w.header.page_type==0x500&&w.header.page_generation==1&&!w.header.flags&&
      w.header.database_uuid==b.database_uuid&&w.header.filespace_uuid==b.filespace_uuid&&
      w.header.page_size_profile_uuid==b.page_size_profile_uuid&&w.bootstrap_uuid==z.page_uuid&&
      w.timeline_uuid==cp.timeline_uuid&&w.operation_uuid==request.operation_uuid&&
      w.watermark==1&&w.base_checkpoint_generation==1&&w.base_root_set_generation==1&&!w.previous_watermark&&
      w.base_checkpoint==disk::NativePageReference{b.filespace_uuid,maps+1,1,b.page_size_profile_uuid}&&
      w.base_checkpoint_object_uuid==cp.object_uuid&&w.base_checkpoint_sha256==bound.selection->checkpoint_sha256&&
      w.previous_state_sha256==std::array<byte,32>{},"exact original construction watermark binding");
  }
  Check(watermark_first.state->object_uuid==watermark_second.state->object_uuid&&
    watermark_first.state->object_uuid!=bound.selection->object_uuid&&
    watermark_first.state->header.page_number==maps+18&&watermark_second.state->header.page_number==maps+19&&
    watermark_first.state_sha256==watermark_second.state_sha256&&
    std::equal(watermark_first.bytes.begin()+128,watermark_first.bytes.begin()+400,watermark_second.bytes.begin()+128),
    "independent physical placement and equal replica payloads");
  expected_owners.emplace(maps+18,watermark_first.state->object_uuid);
  expected_owners.emplace(maps+19,watermark_second.state->object_uuid);
  for(u64 n=1;n<=maps;++n)expected_owners.emplace(n,bound.allocation.pages.front().map->object_uuid);
  for(const auto& r:cp.roots)expected_owners.emplace(r.page.page_number,r.object_uuid);
  for(const auto& r:catalog_image.root->roots)expected_owners.emplace(r.page.page_number,r.object_uuid);
  std::set<Uuid> identities{b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid,b.checksum_profile_uuid,
    request.operation_uuid,request.writer_uuid,request.creator.transaction_uuid.value};
  const auto unique=[&](const Uuid& id){Check(scratchbird::core::uuid::IsEngineIdentityUuid(id)&&identities.insert(id).second,
    "fresh unique binary UUIDv7, not slot-derived or aliased");};
  unique(cp.timeline_uuid);unique(bound.selection->publication_uuid);
  std::set<Uuid> owners;
  u64 allocated=0;
  for(const auto& image:bound.allocation.pages){const auto& map=*image.map;
    Check(map.creator_transaction_uuid==Id(5)&&map.creator_local_transaction_id==1&&map.map_generation==1&&
      map.capacity_generation==1,"actual map creator and generations");
    for(const auto& record:map.records){++allocated;const auto n=record.page_number;
      Check(n<used&&map.states[n-map.first_page]==page::NativeAllocationState::allocated&&
        record.creator_transaction_uuid==Id(5)&&record.creator_local_transaction_id==1&&record.page_generation==1&&
        expected_owners.contains(n)&&expected_owners.at(n)==record.owner_uuid,
        "every physical control has actual committed allocation");
      unique(record.allocation_uuid);unique(record.page_uuid);
      const auto bytes=Read(device,n,size);const auto header=disk::DecodeNativeCommonPageHeader(bytes.data()+(n==0?4096:0),128);
      Check(header.ok()&&header.header->page_uuid==record.page_uuid&&header.header->page_number==n&&
        header.header->page_generation==1&&header.header->page_type==record.page_type&&
        record.page_type==(n==0?1:n<=maps?3:control_types[n-maps]),"allocation matches actual native bytes");
      if(n&&owners.insert(record.owner_uuid).second)unique(record.owner_uuid);
    }
  }
  Check(allocated==used,"no missing, extra or free control records");
  for(u64 n=used;n<z.total_pages;++n){const auto bytes=Read(device,n,size);
    Check(std::all_of(bytes.begin(),bytes.end(),[](byte v){return !v;}),"all physical free bytes are zero");}
  const std::array<u64,14> zero_slots{maps+4,maps+7,1,maps+2,maps+3,maps+8,maps+9,maps+7,maps+1,maps+5,maps+16,maps+17,maps+18,maps+19};
  Check(z.roots.size()==14&&cp.roots.size()==10,"exact owning root sets");
  for(unsigned i=0;i<z.roots.size();++i)Check(z.roots[i].kind==(i<10?i+1:i+8)&&
    z.roots[i].page_number==zero_slots[i]&&z.roots[i].page_generation==1,"independent page-zero layout");
  Check(disk::CanonicalPageZeroRootPageType(20)==0x500&&disk::CanonicalPageZeroRootPageType(21)==0x500&&
    disk::CanonicalPageZeroRootPageType(22)==0,"exact new root registry codes");
  for(unsigned mode=0;mode<10;++mode){auto invalid=z;
    if(mode==0)invalid.roots.erase(invalid.roots.begin()+12);
    if(mode==1)invalid.roots.pop_back();
    if(mode==2)invalid.roots.erase(invalid.roots.begin()+10,invalid.roots.begin()+12);
    if(mode==3)invalid.bootstrap.filespace_role=5;
    if(mode==4)invalid.roots[12].filespace_uuid=Id(990);
    if(mode==5)invalid.roots[13].filespace_uuid=Id(990);
    if(mode==6)invalid.roots[13].object_uuid=Id(990);
    if(mode==7)invalid.roots[13].page_number=invalid.roots[12].page_number;
    if(mode==8)invalid.roots[12].object_uuid=invalid.roots[13].object_uuid=invalid.roots[10].object_uuid;
    if(mode==9)invalid.roots[12].page_number=invalid.roots[10].page_number;
    Check(!disk::EncodeFilespacePageZero(invalid).ok(),"watermark directory cannot omit, alias or misbind paired roots");
  }
  const std::array<u64,10> checkpoint_slots{maps+2,maps+6,maps+3,1,maps+7,maps+8,maps+9,maps+4,maps+7,maps+5};
  for(unsigned i=0;i<cp.roots.size();++i){const auto& r=cp.roots[i];
    const auto bytes=Read(device,r.page.page_number,size);const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);
    Check(r.role==i+1&&r.page.page_number==checkpoint_slots[i]&&digest.ok()&&digest.digest==r.sha256,
      "all ten checkpoint references bind actual full-image hashes");}
  const auto policy=db::VerifyNativeCheckpointPolicyRootsFromOpenDevices(b.database_uuid,devices,checkpoint,budget);
  Check(policy.ok(),"actual catalog, feature, configuration and security roots");
  const auto& catalog=*policy.catalog.catalogs.front().root;
  Check(catalog.roots.size()==6&&catalog.catalog_generation==1&&catalog.schema_epoch==1&&catalog.security_epoch==1&&
    catalog.resource_epoch==0&&!catalog.predecessor,"empty construction catalog epochs not resource activation");
  for(unsigned i=0;i<6;++i){const auto& r=catalog.roots[i];
    Check(r.role==i+1&&r.page_type==6&&r.page.page_number==maps+10+i,"six exact real relation heads");
    const auto leaf=db::ReadNativeCatalogLeafFromOpenDevice(device,b.database_uuid,r);
    Check(leaf.ok()&&leaf.page->body.rows.empty()&&leaf.metadata.empty()&&leaf.page->body.relation_uuid.value==r.object_uuid,
      "empty is actual empty catalog storage, not fake seed/security rows");}
  const auto directory=db::VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(b.database_uuid,devices,checkpoint,budget);
  Check(directory.ok()&&directory.directory.pages.size()==1&&directory.directory.pages.front().directory->records.size()==1,
    "actual single-filespace directory");
  const auto& dr=directory.directory.pages.front().directory->records.front();unique(dr.locator_uuid);
  Check(dr.page_zero_uuid==z.page_uuid&&dr.verification_epoch==1&&!dr.operation&&dr.total_pages==z.total_pages,
    "directory actual construction binding");
  const auto system=db::VerifyCurrentNativeCheckpointSystemStateFromOpenDevices(b.database_uuid,devices,checkpoint,budget);
  Check(system.ok(),"actual system state and checkpoint observation");const auto& s=*system.system_state.state;
  Check(s.lifecycle==db::NativeSystemLifecycle::creating&&s.flags==(db::NativeSystemFlag::dirty|db::NativeSystemFlag::write_fenced)&&
    s.checkpoint_generation==1&&!s.clean_local_transaction_id&&s.transition_operation_uuid==Id(3),"never claims ready or clean");
  const auto horizon=db::VerifyCurrentNativeCheckpointHorizonFromOpenDevices(b.database_uuid,devices,checkpoint,budget);
  Check(horizon.ok()&&horizon.retention.images.front().page->records.empty(),"actual empty retention graph");
  const auto& h=*horizon.horizons.pages.front().root;Check(h.records.size()==3&&!h.minimum_blocker,"three derived local horizons");
  for(unsigned i=0;i<3;++i){const auto& r=h.records[i];unique(r.horizon_uuid);
    Check(static_cast<unsigned>(r.kind)==i+1&&r.local_boundary==2&&r.owner_uuid==Id(4)&&r.timeline_uuid==cp.timeline_uuid&&
      !r.flags&&!r.checkpoint&&r.pin_uuid.is_nil(),"no invented retention pin or checkpoint observation");}
}
}  // namespace

void* operator new(std::size_t n){
  if(count_allocations&&++allocations==allocation_fault)throw std::bad_alloc();
  if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}

extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* bytes,size_t count,off_t offset){
  if(pause_write.exchange(false)){write_paused=true;while(!resume_write.load())std::this_thread::yield();}
  if(Hit(write_call)){errno=EIO;return -1;}
  const bool selector=armed&&offset>=static_cast<off_t>(selector_page*page_bytes)&&
    offset<=static_cast<off_t>((selector_page+1)*page_bytes)&&
    count>=128&&static_cast<const byte*>(bytes)[0];
  if(selector){
    const bool second=offset==static_cast<off_t>((selector_page+1)*page_bytes);
    ordered&=calls[sync_call]>=1+unsigned(second)&&calls[read_call]>=total_pages+unsigned(second)&&watermark_compared==3;
    if(second)ordered&=selector_phase==3;
    if(second&&stop_before_second_selector)_exit(ordered?86:87);
  }
  const auto n=__real_pwrite(fd,bytes,count,offset);
  if(armed&&n==static_cast<ssize_t>(page_bytes)&&count==page_bytes&&static_cast<const byte*>(bytes)[0])
    for(unsigned i=0;i<2;++i)if(offset==static_cast<off_t>((selector_page+2+i)*page_bytes))watermark_written|=1u<<i;
  if(selector&&n==static_cast<ssize_t>(count))selector_phase=offset==static_cast<off_t>(selector_page*page_bytes)?1:4;
  return n;
}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_pread(int fd,void* bytes,size_t count,off_t offset){
  if(Hit(read_call)){errno=EIO;return -1;}
  const auto n=__real_pread(fd,bytes,count,offset);
  if(armed&&n==static_cast<ssize_t>(page_bytes)&&count==page_bytes){
    for(unsigned i=0;i<2;++i)if(offset==static_cast<off_t>((selector_page+2+i)*page_bytes)&&
      (watermark_synced&(1u<<i)))watermark_compared|=1u<<i;
    if(selector_phase==2&&offset==static_cast<off_t>(selector_page*page_bytes))selector_phase=3;
    if(selector_phase==5&&offset==static_cast<off_t>((selector_page+1)*page_bytes)){
      selector_phase=6;
      if(tamper_watermark_after_publication){const byte damaged='X';
        tamper_done=__real_pwrite(fd,&damaged,1,static_cast<off_t>((selector_page+2)*page_bytes))==1&&__real_fsync(fd)==0;
      }
    }
  }
  if(armed&&corrupt_at&&calls[read_call]==corrupt_at&&n>0)static_cast<byte*>(bytes)[n-1]^=1;
  return n;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){if(Hit(sync_call)){errno=EIO;return -1;}const auto r=__real_fsync(fd);
  if(armed&&r==0){watermark_synced|=watermark_written;if(selector_phase==1)selector_phase=2;if(selector_phase==4)selector_phase=5;}return r;}
extern "C" int __real_RAND_bytes(unsigned char*,int);
extern "C" int __wrap_RAND_bytes(unsigned char* bytes,int count){
  if(Hit(random_call))return 0;if(repeat_entropy){std::fill_n(bytes,count,0xab);return 1;}return __real_RAND_bytes(bytes,count);}
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* data,size_t count,unsigned char* md,unsigned int* size,const EVP_MD* type,ENGINE* engine){
  if(Hit(hash_call))return 0;return __real_EVP_Digest(data,count,md,size,type,engine);
}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){if(context_fault==1)return nullptr;return __real_EVP_MD_CTX_new();}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* t,ENGINE* e){return context_fault==2?0:__real_EVP_DigestInit_ex(c,t,e);}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* p,size_t n){return context_fault==3?0:__real_EVP_DigestUpdate(c,p,n);}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* p,unsigned int* n){
  if(context_fault==4)return 0;const auto r=__real_EVP_DigestFinal_ex(c,p,n);if(context_fault==5&&r==1)--*n;return r;}

int main(int argc,char** argv){try{
  if(argc==4&&std::string_view(argv[1])=="--inspect"){
    disk::FileDevice device;Check(device.Open(argv[2],disk::FileOpenMode::open_existing_read_only).ok(),"fresh process owns node");
    Inspect(device,Request(std::stoul(argv[3]),64));Check(device.Close().ok(),"fresh process closes owned node");return 0;
  }
  Fixture fixture;
  if(argc>=2&&(std::string_view(argv[1])=="--allocation-only"||std::string_view(argv[1])=="--allocation-baseline"||std::string_view(argv[1])=="--allocation-site")){
    const bool baseline_only=std::string_view(argv[1])=="--allocation-baseline";
    const bool single=std::string_view(argv[1])=="--allocation-site";
    const unsigned wanted=single&&argc==3?std::stoul(argv[2]):0;
    const unsigned shard=argc==3&&!single?std::stoul(argv[2]):0,shards=argc==3&&!single?4:1;
    Check(shard<shards,"allocation shard range");
    // Initialize process-wide immutable registries before counting repeatable
    // constructor sites. Otherwise a cold baseline includes one-time library
    // allocations that cannot be injected again in later attempts.
    {disk::FileDevice device;const auto path=fixture.Next();
      Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"registry warm-up device");
      Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(device,Request(),23*8192).ok(),"actual constructor registry warm-up");
      Check(device.Close().ok()&&fs::remove(path),"clean closed warm-up construction");}
    {disk::FileDevice device;const auto path=fixture.Next();const auto request=Request();
      Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"failure registry warm-up device");
      allocations=0;allocation_fault=1;count_allocations=true;
      const auto failure=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,23*8192);count_allocations=false;
      Check(!failure.ok()&&!failure.receipt,"actual failure registry warm-up");
      Check(device.Close().ok()&&fs::remove(path),"clean closed failure warm-up");}
    unsigned sites=0;
    for(unsigned at=0;at<=sites;++at){if(at&&(baseline_only||(single?at!=wanted:(at-1)%shards!=shard)))continue;
      // Each fault starts from the same warmed process state. Error paths in
      // previous attempts must not change allocation indexes for later ones.
      if(at){const auto child=fork();Check(child>=0,"allocation fault fork");
        if(child>0){int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
          "isolated allocation fault and preservation checks");continue;}}
      disk::FileDevice device;const auto path=fixture.Next();
      Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"allocation fault device");
      const auto request=Request();const auto lost=device.failed_io_latency_observations();allocations=0;allocation_fault=at;count_allocations=true;
      const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,23*8192);count_allocations=false;
      if(at==0){Check(r.ok(),"allocation baseline");sites=allocations;
        Check(!single||(wanted>0&&wanted<=sites),"requested allocation site actually exists");
        std::cout<<"allocation_sites="<<sites<<std::endl;}
      else {const bool isolated_observation=r.ok()&&device.failed_io_latency_observations()==lost+1;
        Check(allocations>=at&&(isolated_observation||(!r.ok()&&!r.receipt)),"required allocation failure cannot certify a partial graph");
        if(isolated_observation)Inspect(device,request);
        if(device.Size().size_bytes)Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,23*8192).error==
          db::NativeCreationWorkspaceError::device_not_empty,"allocation failure preserves nonempty attempt");}
      Check(device.Close().ok()&&fs::remove(path),"remove only the closed disposable attempt after its preservation checks");
      if(at)_exit(0);
    }
    std::cout<<"PASS native creation allocation checks="<<checks<<" sites="<<sites<<" shard="<<shard<<'/'<<shards<<'\n';return 0;
  }
  for(unsigned profile=0;profile<5;++profile){auto request=Request(profile,64);disk::FileDevice device;const auto path=fixture.Next();
    Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"exclusive new device");
    page_bytes=request.bootstrap.page_size_bytes;total_pages=request.total_pages;selector_page=(profile?1:2)+16;
    Arm();const auto result=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,128*page_bytes);Disarm();
    if(!result.ok())std::cerr<<"creation error="<<static_cast<int>(result.error)<<" checkpoint="<<static_cast<int>(result.checkpoint_error)<<" selector="<<static_cast<int>(result.selection_error)<<'\n';
    Check(result.ok()&&ordered&&selector_phase==6,"all page profiles have real dependency and individual selector publication barriers");Inspect(device,request);
    const auto before=Read(device,0,page_bytes);
    const auto replay=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,128*page_bytes);
    Check(!replay.ok()&&!replay.receipt&&replay.error==db::NativeCreationWorkspaceError::device_not_empty&&
      Read(device,0,page_bytes)==before,"nonempty replay preserves published construction");
    Check(device.Close().ok(),"release before fresh executable");
    const auto child=fork();Check(child>=0,"fork");
    if(child==0){const auto p=std::to_string(profile);execl(argv[0],argv[0],"--inspect",path.c_str(),p.c_str(),nullptr);_exit(127);}
    int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable reads actual construction graph");
  }
  const auto request=Request();page_bytes=8192;total_pages=21;selector_page=17;
  {disk::FileDevice device;Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new).ok(),"post-publication mutation fixture");
    Arm();tamper_watermark_after_publication=true;tamper_done=false;
    const auto result=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,23*page_bytes);
    tamper_watermark_after_publication=false;Disarm();
    Check(tamper_done&&!result.ok()&&!result.receipt,"actual watermark re-admission must precede the creation receipt");
  }
  {const auto path=fixture.Next();const auto child=fork();Check(child>=0,"selector publication process-loss fork");
    if(child==0){disk::FileDevice device;
      if(!device.Open(path.string(),disk::FileOpenMode::create_new).ok())_exit(88);
      Arm();stop_before_second_selector=true;
      (void)db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,23*page_bytes);_exit(89);}
    int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,
      "process loss before second selector follows first selector durability and verification");
    disk::FileDevice device;Check(device.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"new process owns interrupted construction");
    const auto first=Read(device,17,page_bytes),second=Read(device,18,page_bytes);
    Check(db::DecodeNativeCheckpointSelection(first).ok()&&std::all_of(second.begin(),second.end(),[](byte v){return !v;}),
      "actual first selector survives while second is not yet written");
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(2),request.bootstrap.page_size_profile_uuid,&device}};
    const auto classified=db::ClassifyNativeCheckpointSelectionPair(first,second);
    Check(!classified.ok()&&!classified.selection&&classified.error==db::NativeCheckpointSelectionError::repair_required,
      "image pair requires repair after loss between selectors");
    const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),devices,Id(2),23*page_bytes);
    Check(!selected.ok()&&!selected.selection&&selected.slots[0].empty()&&selected.slots[1].empty()&&
      !selected.checkpoint_inventory.ok()&&selected.error==db::NativeCheckpointSelectionError::slot_binding_mismatch,
      "bound reader rejects the missing second common header without granting partial current-root authority");
    const auto replay=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,23*page_bytes);
    Check(!replay.ok()&&!replay.receipt&&replay.error==db::NativeCreationWorkspaceError::device_not_empty&&
      Read(device,17,page_bytes)==first&&Read(device,18,page_bytes)==second,"restart preserves interrupted pair without creation replay");}
  {const auto large=Request(0,3601);disk::FileDevice device;
    Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new).ok(),"cross-map control fixture");
    const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,large,83*8192);
    Check(r.ok()&&r.receipt->map_pages==61,"control allocations span multiple coverage ranges");Inspect(device,large);}
  {disk::FileDevice device;Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new).ok(),"duplicate entropy fixture");
    Arm();repeat_entropy=true;const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,23*page_bytes);repeat_entropy=false;Disarm();
    Check(!r.ok()&&r.error==db::NativeCreationWorkspaceError::identity_failure&&calls[write_call]==0&&device.Size().size_bytes==0,
      "duplicate generated identities refuse before all writes");}
  for(unsigned mode=1;mode<=5;++mode){disk::FileDevice device;
    Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new).ok(),"multipart provider fixture");
    Arm();context_fault=mode;const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,23*page_bytes);context_fault=0;Disarm();
    Check(!r.ok()&&!r.receipt&&calls[write_call]==0&&device.Size().size_bytes==0,"multipart hash failures cannot create unsigned roots");}
  {disk::FileDevice device;const auto path=fixture.Next();Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"guard retention fixture");
    pause_write=true;write_paused=false;resume_write=false;std::atomic<bool> entered=false,acquired=false;
    db::NativeCreationWorkspaceResult result;
    std::thread writer([&]{result=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,23*page_bytes);});
    while(!write_paused.load())std::this_thread::yield();
    std::thread observer([&]{entered=true;const auto guard=device.AcquireOperationGuard();acquired=true;});
    while(!entered.load())std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));const bool held=!acquired.load();
    disk::FileDevice other;const auto competing=other.Open(path.string(),disk::FileOpenMode::open_existing_read_only);
    resume_write=true;writer.join();observer.join();
    Check(held&&acquired&&result.ok()&&!competing.ok(),"device and node ownership remain held through real initialization");}
  std::array<unsigned,call_count> baseline{};
  {disk::FileDevice device;Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new).ok(),"minimum capacity device");
    Arm();const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,23*page_bytes);baseline=calls;Disarm();
    Check(r.ok()&&r.receipt->free_pages==0&&ordered&&selector_phase==6,"exact minimum capacity and image budget");Inspect(device,request);}
  for(unsigned kind=0;kind<call_count;++kind)for(unsigned at=1;at<=baseline[kind];++at){
    disk::FileDevice device;Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new).ok(),"fault fixture");
    Arm(kind,at);const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,23*page_bytes);const auto observed=calls;Disarm();
    Check(!r.ok()&&!r.receipt&&observed[kind]>=at,"every injected physical/provider failure returns no receipt");
    const auto size=device.Size();Check(size.ok()&&device.is_open(),"failure preserves open owner");
    if((kind==sync_call&&at==2)||(kind==read_call&&at==total_pages+1)){
      const auto first=Read(device,selector_page,page_bytes),second=Read(device,selector_page+1,page_bytes);
      Check(db::DecodeNativeCheckpointSelection(first).ok()&&std::all_of(second.begin(),second.end(),[](byte v){return !v;}),
        "first selector sync/read failure leaves second selector untouched");}
    if(kind==random_call||(kind==hash_call&&at<=19))Check(observed[write_call]==0&&size.size_bytes==0,"preparation failure precedes all writes");
    if(size.size_bytes){const auto replay=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,23*page_bytes);
      Check(!replay.ok()&&replay.error==db::NativeCreationWorkspaceError::device_not_empty&&device.Size().size_bytes==size.size_bytes,
        "partial and unacknowledged construction is never truncated or retried");}
  }
  for(unsigned at=1;at<=23;++at){disk::FileDevice device;Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new).ok(),"readback corruption fixture");
    Arm();corrupt_at=at;const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,23*page_bytes);Disarm();
    Check(!r.ok()&&!r.receipt&&r.error==db::NativeCreationWorkspaceError::readback_mismatch,"every dependency and selector readback byte mismatch refuses");
    if(at==total_pages+1){const auto second=Read(device,selector_page+1,page_bytes);
      Check(std::all_of(second.begin(),second.end(),[](byte v){return !v;}),"first selector comparison failure prevents second publication");}}
  for(unsigned variant=0;variant<10;++variant){auto bad=request;u64 budget=23*page_bytes;
    if(variant==0)bad.creator.local_id.value=2;if(variant==1)bad.bootstrap.lifecycle_state=1;
    if(variant==2)bad.total_pages=20;if(variant==3)--budget;if(variant==4)bad.operation_uuid=bad.writer_uuid;
    if(variant==5)bad.creation_utc_millis=0;if(variant==6)bad.bootstrap.filespace_role=5;
    if(variant==7)bad.creator.scope=mga::TransactionScope::cluster_global;
    if(variant==8)bad.bootstrap.flags=disk::FilespaceBootstrapFlag::cluster_authority_required;
    if(variant==9){bad.bootstrap.flags=disk::FilespaceBootstrapFlag::payload_encrypted;bad.bootstrap.encryption_profile_uuid=Id(90);}
    disk::FileDevice device;Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new).ok(),"invalid input fixture");
    Arm();const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,bad,budget);Disarm();
    Check(!r.ok()&&!r.receipt&&calls[write_call]==0&&device.Size().size_bytes==0,"invalid lineage/capacity/provider never touches storage");}
  std::cout<<"PASS native creation workspace checks="<<checks<<" fault sites=";
  for(const auto n:baseline)std::cout<<n<<',';std::cout<<'\n';return 0;
}catch(const std::exception& e){Disarm();std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}

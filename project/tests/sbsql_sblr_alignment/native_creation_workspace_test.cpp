// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_creation_workspace.hpp"
#include "allocation_map_inplace_page.hpp"
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
#include <memory>
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
bool count_allocations=false,repeat_entropy=false,zero_entropy=false,clock_failure=false,clock_allocation_failure=false;
constexpr u64 observed_uuid_millis=u64{1}<<40;
unsigned allocations=0,allocation_fault=0,context_fault=0;
std::atomic<bool> pause_write=false,write_paused=false,resume_write=false;
unsigned checks=0;
void Check(bool value,const char* what){++checks;if(!value)throw std::runtime_error(what);}
enum Call {write_call,read_call,sync_call,random_call,hash_call,call_count};
std::array<unsigned,call_count> calls{};
bool armed=false;
bool tamper_done=false;
// SB_NATIVE_FIXED_ROOT_READ (drift removal R3b3): the fixed-root commit
// protocol. zero_phase 1 = page zero written after every other page was written,
// synchronized and read back; 2 = page zero synchronized; 3 = page zero read
// back.
bool stop_before_page_zero=false,tamper_after_page_zero=false;
unsigned zero_phase=0;u64 tamper_page=0;
unsigned fault_kind=call_count,fault_at=0,corrupt_at=0;
unsigned zero_writes=0,zero_partial_at=0;
u64 zero_bytes=0,largest_zero_write=0;
// The smallest file: maps + 14 controls of the fixed-root graph (13 control
// pages) + the transaction header page (SB_TRANSACTION_HEADER_PAGE, allocated
// last). The smallest image budget adds one in-flight encoder image and one
// readback image. (The drift-era graph had 20 controls.)
// step4 (SB_NATIVE_COMMENT_RELATION): + the head leaf of role 7, the comments
// relation (unindexed here, so it has a head leaf), allocated after the header
// page: 17 pages.
u64 page_bytes=8192,total_pages=17;
unsigned first_write_hashes=~0u;  // digest calls before the first physical write
unsigned torn_at=0;  // the write torn in half (then failed), counted as calls[write_call]
bool ordered=true;
bool Hit(Call kind){if(!armed)return false;return ++calls[kind]==fault_at&&kind==fault_kind;}
void Arm(unsigned kind=call_count,unsigned at=0){calls={};fault_kind=kind;fault_at=at;corrupt_at=0;zero_phase=0;first_write_hashes=~0u;zero_writes=0;zero_bytes=largest_zero_write=0;ordered=true;armed=true;}
void Disarm(){armed=false;}
// 32 bits in the last four bytes; below 65536 the UUIDs are those of the earlier 16-bit form.
Uuid Id(unsigned n){Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[12]=n>>24;id.bytes[13]=n>>16;id.bytes[14]=n>>8;id.bytes[15]=n;return id;}
db::NativeFilespaceInitializationRequest Request(unsigned profile=0,u64 total=17){  // step4: + role 7's head leaf (was 16)
  const auto& p=disk::kCanonicalFilespacePageProfiles[profile];
  db::NativeFilespaceInitializationRequest r;
  r.bootstrap={Id(1),Id(2),p.uuid,disk::kNativeBootstrapIntegrityProfile,{},p.page_size_bytes,1,0,1,7};
  r.operation_uuid=Id(3);r.writer_uuid=Id(4);r.policy_snapshot_uuid=Id(10);
  r.creator.transaction_uuid={UuidKind::transaction,Id(5)};
  r.creator.local_id=mga::MakeLocalTransactionId(1);r.creator.scope=mga::TransactionScope::local_node;
  r.total_pages=total;r.creation_utc_millis=1789357072000ULL;return r;
}
struct Fixture{
  fs::path root;
  unsigned serial=0;
  std::unique_ptr<scratchbird::core::uuid::StandaloneUuidV7Issuer> issuer;
  void ResetIssuer(){issuer=std::make_unique<scratchbird::core::uuid::StandaloneUuidV7Issuer>(scratchbird::core::uuid::StandaloneUuidV7Binding{Id(1),Id(10)},scratchbird::core::uuid::StandaloneUuidV7Policy{{},0,1000});}
  Fixture(){auto name=(fs::temp_directory_path()/"sb-native-creation-test.XXXXXX").string();const auto* p=mkdtemp(name.data());if(!p)throw std::runtime_error("mkdtemp");root=p;}
  ~Fixture(){std::error_code ignored;fs::remove_all(root,ignored);}
  fs::path Next(){ResetIssuer();const auto number=std::to_string(serial++);
    return root/("node-"+std::string(12-number.size(),'0')+number);}
};
std::vector<byte> Read(disk::FileDevice& device,u64 n,u64 size){
  std::vector<byte> bytes(size);const auto io=device.ReadAt(n*size,bytes.data(),bytes.size());
  Check(io.ok()&&io.bytes_transferred==bytes.size(),"actual full page read");return bytes;
}
// SB_NATIVE_FIXED_ROOT_READ (drift removal R3b3): the fixed-root genesis
// graph, read independently. Page zero names kinds 1-8 and 10 and no
// checkpoint, selector or watermark root; the fixed-root read admits page zero,
// the in-place inventory, the allocation chain and the directory; every
// allocated page is one control of its type and owner, with a fresh identity;
// no checkpoint, horizon-root, selector or watermark page exists; free pages
// are zero; the catalog, configuration, security, system-state and retention
// pages decode to the construction's values.
void Inspect(disk::FileDevice& device,const db::NativeFilespaceInitializationRequest& request){
  const auto& b=request.bootstrap;const auto size=b.page_size_bytes;
  const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(device);
  Check(zero.ok(),"actual page zero");const auto& z=*zero.record;
  Check(z.bootstrap.database_uuid==b.database_uuid&&z.bootstrap.filespace_uuid==b.filespace_uuid&&
    z.bootstrap.lifecycle_state==1&&z.bootstrap.filespace_role==b.filespace_role&&z.bootstrap.flags==0,
    "exact bootstrap: page zero is written last, so a complete file is an online filespace (owner 2026-11-07)");
  Check(z.creation_operation_uuid==request.operation_uuid&&z.writer_identity_uuid==request.writer_uuid&&
    z.creation_utc_millis==request.creation_utc_millis&&z.page_generation==1&&z.root_set_generation==1,
    "actual creation lineage");
  const std::vector<disk::NativeFilespaceDevice> devices{{b.filespace_uuid,b.page_size_profile_uuid,&device}};
  const u64 budget=256*size;
  std::vector<byte> backing(2*budget+(u64{1}<<20));
  const auto fixed=db::AcquireNativeFixedRootBackedLease(b.database_uuid,devices,b.filespace_uuid,
      {budget,backing.size()},backing);
  Check(fixed.ok(),"actual fixed-root admission of page zero, inventory, allocation and directory");
  const auto& lease=*fixed.lease;
  Check(lease.identity().page_zero_uuid==z.page_uuid&&lease.identity().root_set_generation==1,"the fixed root is this page zero");
  const auto& inv=lease.inventory();
  Check(inv.entries.size()==1&&inv.next_local_transaction_id==2&&inv.next_commit_sequence==2,
    "no invented active transaction");
  const auto& entry=inv.entries.front();
  // In the in-place inventory a transaction's begin time is its UUIDv7 time
  // prefix (the UUID is issued at BEGIN; SB_TRANSACTION_INVENTORY_READER); the
  // header records only the completion time.
  u64 creator_begin=0;for(unsigned n=0;n<6;++n)creator_begin=(creator_begin<<8)|request.creator.transaction_uuid.value.bytes[n];
  Check(entry.identity.transaction_uuid.value==Id(5)&&entry.identity.scope==mga::TransactionScope::local_node&&
    entry.identity.local_id.value==1&&entry.state==mga::TransactionState::committed&&entry.commit_sequence==1&&
    entry.evidence_record_required&&entry.evidence_record_written&&!entry.rollback_only&&
    entry.begin_unix_epoch_millis==creator_begin&&
    entry.final_unix_epoch_millis==std::max<u64>(request.creation_utc_millis,creator_begin)&&
    entry.begin_unix_epoch_millis<=entry.final_unix_epoch_millis,
    "real single construction inventory outcome");
  // SB_ALLOCATION_MAP_IN_PLACE (owner Q22 = 1): the fixed-root create writes
  // the in-place allocation map. Pages covered per map page: the largest N
  // with roundup128(512 + ceil(N/2)) + 128 N <= page size (its fixed header and
  // mutable unit are 512 bytes, then 4-bit states, then a 128-byte record per
  // covered page on a 128-byte boundary). The sealed map covered one more.
  const std::array<u64,5> coverage{59,123,251,506,1016};
  unsigned profile=0;while(disk::kCanonicalFilespacePageProfiles[profile].page_size_bytes!=size)++profile;
  // Page zero, the maps, 13 control pages and the transaction header page;
  // step4: then role 7's head leaf.
  const u64 maps=(request.total_pages+coverage[profile]-1)/coverage[profile],used=maps+16;
  const auto& chain=lease.allocation();
  Check(z.total_pages==request.total_pages&&z.free_pages==request.total_pages-used&&!z.preallocated_pages&&
    chain.pages.size()==maps&&chain.state_counts[2]==used&&chain.state_counts[0]==z.free_pages,
    "independent exact coverage and capacity counts");
  // Controls: inventory, directory, system state, retention, catalog,
  // configuration, security, six relation heads; then the header page, a
  // second family under the inventory page type.
  // step4: role 7's head leaf (type 6) after the header page.
  const std::array<u32,16> control_types{0,0x301,9,8,0x303,5,10,11,6,6,6,6,6,6,0x301,6};
  const std::array<u16,9> kinds{1,2,3,4,5,6,7,8,10};
  const std::array<u64,9> zero_slots{maps+3,maps+5,1,maps+1,maps+2,maps+6,maps+7,maps+5,maps+4};
  Check(z.roots.size()==kinds.size(),"exact fixed root set: kinds 1-8 and 10, no checkpoint, selector or watermark root");
  for(unsigned i=0;i<z.roots.size();++i){
    Check(z.roots[i].kind==kinds[i]&&z.roots[i].page_number==zero_slots[i]&&z.roots[i].page_generation==1&&
      z.roots[i].filespace_uuid==b.filespace_uuid,"independent page-zero layout");
  }
  for(unsigned mode=0;mode<2;++mode){auto invalid=z;
    if(mode==0)invalid.roots.push_back({18,0x30e,b.filespace_uuid,maps+20,1,b.page_size_profile_uuid,Id(990)});
    if(mode==1){invalid.roots.push_back({20,0x500,b.filespace_uuid,maps+20,1,b.page_size_profile_uuid,Id(990)});
      invalid.roots.push_back({21,0x500,b.filespace_uuid,maps+21,1,b.page_size_profile_uuid,Id(990)});}
    Check(!disk::EncodeFilespacePageZero(invalid).ok(),"page zero refuses an unpaired selector root and a watermark pair without selectors");
  }
  const auto catalog_ref=z.roots[1];
  const auto catalog_image=page::ReadNativeCatalogRootFromOpenDevice(device,b.database_uuid,catalog_ref);
  Check(catalog_image.ok(),"actual catalog root");
  std::map<u64,Uuid> expected_owners{{0,b.filespace_uuid}};
  for(u64 n=1;n<=maps;++n)expected_owners.emplace(n,chain.pages.front().map.object_uuid);
  for(const auto& r:z.roots)expected_owners.emplace(r.page_number,r.object_uuid);
  expected_owners.emplace(maps+14,z.roots[3].object_uuid);  // the header page belongs to the inventory object
  for(const auto& r:catalog_image.root->roots)expected_owners.emplace(r.page.page_number,r.object_uuid);
  std::set<Uuid> identities{b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid,b.checksum_profile_uuid,
    request.operation_uuid,request.writer_uuid,request.creator.transaction_uuid.value,request.policy_snapshot_uuid};
  const auto unique=[&](const Uuid& id){u64 timestamp=0;for(unsigned n=0;n<6;++n)timestamp=(timestamp<<8)|id.bytes[n];
    Check(timestamp==observed_uuid_millis&&timestamp!=request.creation_utc_millis,"UUID timestamp comes from observed clock not construction time");
    Check(identities.insert(id).second,"fresh unique binary UUIDv7, not slot-derived or aliased");};
  std::set<Uuid> owners;
  u64 allocated=0;
  for(const auto& image:chain.pages){const auto& map=image.map;
    Check(map.creator_transaction_uuid==Id(5)&&map.creator_local_transaction_id==1&&map.map_generation==1&&
      map.capacity_generation==1,"actual map creator and generations");
    for(const auto& record:map.records){++allocated;const auto n=record.page_number;
      Check(n<used&&map.states[n-map.first_page]==page::NativeAllocationState::allocated&&
        record.creator_transaction_uuid==Id(5)&&record.creator_local_transaction_id==1&&record.page_generation==1&&
        expected_owners.contains(n)&&expected_owners.at(n)==record.owner_uuid,
        "every physical control has actual committed allocation");
      Check(record.page_type!=0x300&&record.page_type!=0x302&&record.page_type!=0x30e&&record.page_type!=0x500,
        "no checkpoint, horizon-root, selector or watermark page is allocated");
      unique(record.allocation_uuid);unique(record.page_uuid);
      const auto bytes=Read(device,n,size);const auto header=disk::DecodeNativeCommonPageHeader(bytes.data()+(n==0?4096:0),128);
      Check(header.ok()&&header.header->page_uuid==record.page_uuid&&header.header->page_number==n&&
        header.header->page_generation==1&&header.header->page_type==record.page_type&&
        record.page_type==(n==0?1:n<=maps?3:control_types[n-maps]),"allocation matches actual native bytes");
      if(n&&owners.insert(record.owner_uuid).second)unique(record.owner_uuid);
    }
  }
  Check(allocated==used,"no missing, extra or free control records");
  for(u64 n=1;n<=maps;++n)
    Check(page::IsAllocationMapInplaceImage(Read(device,n,size)),"every map page the fixed-root create writes is in place");
  for(u64 n=used;n<z.total_pages;++n){const auto bytes=Read(device,n,size);
    Check(std::all_of(bytes.begin(),bytes.end(),[](byte v){return !v;}),"all physical free bytes are zero");}
  const auto& catalog=*catalog_image.root;
  Check(catalog.roots.size()==7&&catalog.catalog_generation==1&&catalog.schema_epoch==1&&catalog.security_epoch==1&&
    catalog.resource_epoch==0&&!catalog.predecessor&&catalog.creator_transaction_uuid==Id(5)&&catalog.creator_local_transaction_id==1,
    "empty construction catalog epochs not resource activation");
  // step4: seven relation heads; role 7's head leaf follows the header page.
  for(unsigned i=0;i<7;++i){const auto& r=catalog.roots[i];
    Check(r.role==i+1&&r.page_type==6&&r.page.page_number==(i<6?maps+8+i:maps+15),"step4: seven exact real relation heads");
    const auto leaf=db::ReadNativeCatalogLeafFromOpenDevice(device,b.database_uuid,r);
    Check(leaf.ok()&&leaf.page->body.rows.empty()&&leaf.metadata.empty()&&leaf.page->body.relation_uuid.value==r.object_uuid,
      "empty is actual empty catalog storage, not fake seed/security rows");}
  for(const unsigned at:{5u,6u}){
    const auto policy=page::ReadNativeCatalogRootFromOpenDevice(device,b.database_uuid,z.roots[at]);
    Check(policy.ok()&&policy.root->root_kind==z.roots[at].kind&&policy.root->roots.size()==1&&
      policy.root->creator_transaction_uuid==Id(5),"actual configuration and security roots");
  }
  const auto& directory_pages=lease.directory().pages;
  Check(directory_pages.size()==1&&directory_pages.front().directory.records.size()==1,"actual single-filespace directory");
  const auto& dr=directory_pages.front().directory.records.front();unique(dr.locator_uuid);
  Check(dr.page_zero_uuid==z.page_uuid&&dr.verification_epoch==1&&!dr.operation&&dr.total_pages==z.total_pages,
    "directory actual construction binding");
  // SB_MGA_DATABASE_STATE (owner 2026-11-07): the database's administrative
  // mode, Firebird's model; a complete create is online and read-write.
  const auto state_page=Read(device,maps+3,size);
  const auto system=db::DecodeNativeDatabaseStateValue(state_page);
  Check(system.ok(),"actual database state");const auto& st=*system.state;
  Check(st.mode==db::NativeDatabaseMode::online&&st.flags==0&&st.creator_transaction_uuid==Id(5)&&
    st.creator_local_transaction_id==1&&st.object_uuid==z.roots[0].object_uuid,
    "a complete create is an online, read-write database owned by the construction transaction");
  Check(!db::DecodeNativeSystemStateValue(state_page).ok(),"the create writes no drift-era system state");
  const auto retention=page::DecodeNativeRetentionPage(Read(device,maps+4,size));
  Check(retention.ok()&&retention.page->records.empty()&&retention.page->epoch==1,"actual empty retention page");
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
  if(armed&&count&&std::all_of(static_cast<const byte*>(bytes),static_cast<const byte*>(bytes)+count,[](byte b){return b==0;})){
    ++zero_writes;zero_bytes+=count;largest_zero_write=std::max<u64>(largest_zero_write,count);
    if(zero_partial_at==zero_writes){
      const auto written=__real_pwrite(fd,bytes,count/2,offset);
      if(written!=static_cast<ssize_t>(count/2))std::abort();
      errno=EIO;return -1;
    }
  }
  if(armed&&first_write_hashes==~0u)first_write_hashes=calls[hash_call];
  if(Hit(write_call)){errno=EIO;return -1;}
  if(armed&&torn_at&&calls[write_call]==torn_at&&count){
    const auto half=__real_pwrite(fd,bytes,count/2,offset);
    if(half!=static_cast<ssize_t>(count/2))std::abort();
    errno=EIO;return -1;
  }
  const bool page_zero_image=armed&&offset==0&&count==page_bytes&&static_cast<const byte*>(bytes)[0];
  if(page_zero_image){
    ordered&=zero_phase==0&&calls[sync_call]>=1&&calls[read_call]>=total_pages;
    if(stop_before_page_zero)_exit(ordered?86:87);
  }else if(armed&&zero_phase&&count)ordered=false;  // nothing is written after the commit point
  const auto n=__real_pwrite(fd,bytes,count,offset);
  if(page_zero_image&&n==static_cast<ssize_t>(count))zero_phase=1;
  return n;
}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_pread(int fd,void* bytes,size_t count,off_t offset){
  if(Hit(read_call)){errno=EIO;return -1;}
  const auto n=__real_pread(fd,bytes,count,offset);
  if(armed&&zero_phase==2&&offset==0&&n==static_cast<ssize_t>(page_bytes)){
    zero_phase=3;
    if(tamper_after_page_zero){const byte damaged='X';
      tamper_done=__real_pwrite(fd,&damaged,1,static_cast<off_t>(tamper_page*page_bytes+page_bytes/2))==1&&__real_fsync(fd)==0;}
  }
  if(armed&&corrupt_at&&calls[read_call]==corrupt_at&&n>0)static_cast<byte*>(bytes)[n-1]^=1;
  return n;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){if(Hit(sync_call)){errno=EIO;return -1;}const auto r=__real_fsync(fd);
  if(armed&&r==0&&zero_phase==1)zero_phase=2;return r;}
extern "C" int __real_RAND_bytes(unsigned char*,int);
extern "C" int __wrap_RAND_bytes(unsigned char* bytes,int count){
  if(Hit(random_call))return 0;if(zero_entropy){std::fill_n(bytes,count,0);return 1;}if(repeat_entropy){if(count!=16)return 0;const auto collision=Id(3);std::copy(collision.bytes.begin(),collision.bytes.end(),bytes);return 1;}return __real_RAND_bytes(bytes,count);}
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

extern "C" scratchbird::core::time::ClockSnapshotResult __wrap__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv() {
  if(clock_allocation_failure)throw std::bad_alloc();
  scratchbird::core::time::ClockSnapshotResult result;
  if(clock_failure){result.status={StatusCode::time_source_unavailable,Severity::error,Subsystem::time};return result;}
  result.value={{100},{static_cast<i64>(observed_uuid_millis/1000),static_cast<u32>((observed_uuid_millis%1000)*1000000)}};
  return result;
}
#include "native_creation_catalog_checks.hpp"
#include "native_metric_config_checks.hpp"
#include "native_indexed_creation_failure_checks.hpp"
#include "native_catalog_authority_checks.hpp"
int main(int argc,char** argv){try{
  if((argc==3||argc==4)&&std::string_view(argv[1])=="--indexed-faults") {
    NativeIndexedCreationFailureChecks(std::stoul(argv[2]),argc==4?std::stoul(argv[3]):1);
    std::cout<<"PASS indexed_fault_checks="<<checks<<'\n';return 0;
  }
  if(argc==2&&std::string_view(argv[1])=="--zero-batches"){
    Fixture fixture;
    for(unsigned profile=0;profile<5;++profile){
      const auto request=Request(profile,65);page_bytes=request.bootstrap.page_size_bytes;total_pages=request.total_pages;
      const u64 maps=profile?1:2;
      for(const u64 extra:{u64{0},u64{4},u64{64}}){
        disk::FileDevice device;Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"zero batch device");
        // step4: one more image (role 7's head leaf), so the same batch sizes need one more page of budget (was maps+17).
        Arm();const auto result=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,(maps+18+extra)*page_bytes,*fixture.issuer);Disarm();
        const u64 batch=std::min<u64>(16,extra+1);
        Check(result.ok()&&ordered&&zero_phase==3&&zero_writes==(total_pages+batch-1)/batch&&
          zero_bytes==total_pages*page_bytes&&largest_zero_write==batch*page_bytes,
          "bounded coalescing covers all bytes including final partial batch without changing barriers");
        Inspect(device,request);
      }
      for(unsigned at=1;at<=5;++at){
        disk::FileDevice device;Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"partial zero batch device");
        Arm();zero_partial_at=at;const auto result=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,256*page_bytes,*fixture.issuer);Disarm();zero_partial_at=0;
        Check(!result.ok()&&!result.receipt&&result.error==db::NativeCreationWorkspaceError::io_failure&&
          zero_writes==at&&!zero_phase&&!calls[sync_call],"partial zero batch cannot write page zero or a receipt");
        const auto length=device.Size();Check(length.ok()&&length.size_bytes==(u64{at-1}*16+(at==5?0:8))*page_bytes+(at==5?page_bytes/2:0),"exact physical prefix retained after failed zero write");
        const auto retained=Read(device,0,length.size_bytes);
        const auto retry=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,256*page_bytes,*fixture.issuer);
        Check(retry.error==db::NativeCreationWorkspaceError::device_not_empty&&!retry.receipt&&Read(device,0,length.size_bytes)==retained,"failed batch cannot be silently retried or truncated");
      }
    }
    std::cout<<"PASS bounded zero initialization checks="<<checks<<'\n';return 0;
  }
  if(argc==2&&std::string_view(argv[1])=="--catalog-authority") {
    NativeCatalogAuthorityChecks();std::cout<<"native catalog authority checks="<<checks<<'\n';return 0;
  }
  if(argc==3&&std::string_view(argv[1])=="--metric-config") {
    NativeMetricConfigChecks(argv[2]);std::cout<<"native metric config checks="<<checks<<'\n';return 0;
  }
  if(argc==2&&std::string(argv[1])=="--populated") {
    PopulatedWorkspaceChecks();std::cout<<"populated native workspace checks="<<checks<<'\n';return 0;
  }
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
      Check(device.Open(path.string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"registry warm-up device");
      Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(device,Request(),24*8192,*fixture.issuer).ok(),"actual constructor registry warm-up");
      Check(device.Close().ok()&&fs::remove(path),"clean closed warm-up construction");}
    {disk::FileDevice device;const auto path=fixture.Next();const auto request=Request();
      Check(device.Open(path.string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"failure registry warm-up device");
      allocations=0;allocation_fault=1;count_allocations=true;
      const auto failure=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,24*8192,*fixture.issuer);count_allocations=false;
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
      Check(device.Open(path.string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"allocation fault device");
      const auto request=Request();const auto lost=device.failed_io_latency_observations();allocations=0;allocation_fault=at;count_allocations=true;
      const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,24*8192,*fixture.issuer);count_allocations=false;
      if(at==0){Check(r.ok(),"allocation baseline");sites=allocations;
        Check(!single||(wanted>0&&wanted<=sites),"requested allocation site actually exists");
        std::cout<<"allocation_sites="<<sites<<std::endl;}
      else {const bool isolated_observation=r.ok()&&device.failed_io_latency_observations()==lost+1;
        Check(allocations>=at&&(isolated_observation||(!r.ok()&&!r.receipt)),"required allocation failure cannot certify a partial graph");
        if(isolated_observation)Inspect(device,request);
        if(device.Size().size_bytes)Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,24*8192,*fixture.issuer).error==
          db::NativeCreationWorkspaceError::device_not_empty,"allocation failure preserves nonempty attempt");}
      Check(device.Close().ok()&&fs::remove(path),"remove only the closed disposable attempt after its preservation checks");
      if(at)_exit(0);
    }
    std::cout<<"PASS native creation allocation checks="<<checks<<" sites="<<sites<<" shard="<<shard<<'/'<<shards<<'\n';return 0;
  }
  for(unsigned profile=0;profile<5;++profile){auto request=Request(profile,64);disk::FileDevice device;const auto path=fixture.Next();
    Check(device.Open(path.string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"exclusive new device");
    page_bytes=request.bootstrap.page_size_bytes;total_pages=request.total_pages;
    Arm();const auto result=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,128*page_bytes,*fixture.issuer);Disarm();
    if(!result.ok())std::cerr<<"creation error="<<static_cast<int>(result.error)<<'\n';
    Check(result.ok()&&ordered&&zero_phase==3,
        "every page is written, synchronized and read back before page zero, which is written, synchronized and read back last");
    Inspect(device,request);
    const auto before=Read(device,0,page_bytes);
    const auto replay=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,128*page_bytes,*fixture.issuer);
    Check(!replay.ok()&&!replay.receipt&&replay.error==db::NativeCreationWorkspaceError::device_not_empty&&
      Read(device,0,page_bytes)==before,"nonempty replay preserves published construction");
    Check(device.Close().ok(),"release before fresh executable");
    const auto child=fork();Check(child>=0,"fork");
    if(child==0){const auto p=std::to_string(profile);execl(argv[0],argv[0],"--inspect",path.c_str(),p.c_str(),nullptr);_exit(127);}
    int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable reads actual construction graph");
  }
  // The smallest fixed-root file: page zero, one map, 13 controls and the
  // header page; the smallest image budget adds an encoder and a readback image.
  // step4: + role 7's head leaf: 17 pages, 19 images.
  const auto request=Request();page_bytes=8192;total_pages=17;
  const u64 minimum_budget=19*page_bytes;
  // SB_NATIVE_FIXED_ROOT_READ (drift removal R3b3): a fixed root changed
  // after page zero is written and read back is caught by the create's own
  // read of the file, before any receipt.
  {disk::FileDevice device;Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"post-commit mutation fixture");
    Arm();tamper_after_page_zero=true;tamper_done=false;tamper_page=3;  // the directory page (one map, then inventory)
    const auto result=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,minimum_budget,*fixture.issuer);
    tamper_after_page_zero=false;Disarm();
    Check(tamper_done&&!result.ok()&&!result.receipt,"the create's read of the file must precede its receipt");
  }
  // Process loss before the commit point: every other page is durable and
  // read back, page zero is still zero, and the file is not a database.
  {const auto path=fixture.Next();const auto child=fork();Check(child>=0,"page-zero process-loss fork");
    if(child==0){disk::FileDevice device;
      if(!device.Open(path.string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok())_exit(88);
      fixture.ResetIssuer();Arm();stop_before_page_zero=true;
      (void)db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,minimum_budget,*fixture.issuer);_exit(89);}
    int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,
      "process loss before page zero follows every other page's durability and readback");
    // S1 (owner 2026-11-13): the restarting owner opens the native file with
    // native ownership, as any native creator must; the replay is then refused
    // for the reason under test (device_not_empty), not for the device mode.
    disk::FileDevice device;Check(device.Open(path.string(),disk::FileOpenMode::open_existing,disk::DatabaseFileOwnership::native_data_file).ok(),"new process owns interrupted construction");
    const auto zero_page=Read(device,0,page_bytes);
    Check(std::all_of(zero_page.begin(),zero_page.end(),[](byte v){return !v;}),"page zero is still zero: the commit point was never reached");
    Check(!disk::ReadFilespacePageZeroFromOpenDevice(device).ok(),"an interrupted create has no page zero");
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(2),request.bootstrap.page_size_profile_uuid,&device}};
    std::vector<byte> backing(2*minimum_budget+(u64{1}<<20));
    Check(!db::AcquireNativeFixedRootBackedLease(Id(1),devices,Id(2),{minimum_budget,backing.size()},backing).ok(),
      "an interrupted create is not a database: the fixed-root read refuses it");
    const auto before=Read(device,0,total_pages*page_bytes);
    const auto replay=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,minimum_budget,*fixture.issuer);
    Check(!replay.ok()&&!replay.receipt&&replay.error==db::NativeCreationWorkspaceError::device_not_empty&&
      Read(device,0,total_pages*page_bytes)==before,"restart preserves the interrupted file without creation replay");}
  {// Owner Q9 = 1 (2026-11-05): a creator UUID stamped after the admitted
    // creation time completes at its own begin time, never before it begins;
    // the page-zero creation time stays the admitted one.
    const u64 later=Request().creation_utc_millis+60000;
    Uuid creator=Id(5);for(unsigned n=0;n<6;++n)creator.bytes[n]=static_cast<byte>(later>>(8*(5-n)));
    auto request=Request();request.creator.transaction_uuid={UuidKind::transaction,creator};
    disk::FileDevice device;Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"later creator fixture");
    Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,minimum_budget,*fixture.issuer).ok(),"later creator construction");
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(2),request.bootstrap.page_size_profile_uuid,&device}};
    std::vector<byte> backing(2*256*page_bytes+(u64{1}<<20));
    const auto bound=db::AcquireNativeFixedRootBackedLease(Id(1),devices,Id(2),{256*page_bytes,backing.size()},backing);
    Check(bound.ok()&&bound.lease->inventory().entries.size()==1,"later creator admission");
    const auto& entry=bound.lease->inventory().entries.front();
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(device);
    Check(zero.ok()&&zero.record->creation_utc_millis==request.creation_utc_millis&&
        entry.identity.transaction_uuid.value==creator&&entry.begin_unix_epoch_millis==later&&
        entry.final_unix_epoch_millis==later,"construction completes at the later of creation and begin");}
  {const auto large=Request(0,3601);disk::FileDevice device;
    Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"cross-map control fixture");
    // The final admission's exact image budget: the maps twice, four controls,
    // and the in-place inventory's header page (SB_TRANSACTION_HEADER_PAGE).
    // 3601 pages at 59 per in-place map page: 62 map pages.
    const auto required=(2*62+5)*u64{8192};
    const auto insufficient=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,large,required-1,*fixture.issuer);
    Check(!insufficient.ok()&&!insufficient.receipt&&insufficient.error==db::NativeCreationWorkspaceError::resource_exhausted&&!device.Size().size_bytes,"final selected admission budget deficit refuses before writes");
    const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,large,required,*fixture.issuer);
    if(!r.ok())std::cerr<<"large creation error="<<static_cast<int>(r.error)<<'\n';
    Check(r.ok()&&r.receipt->map_pages==62,"control allocations span multiple coverage ranges");Inspect(device,large);}
  {disk::FileDevice device;Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"duplicate entropy fixture");
    Arm();repeat_entropy=true;const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,minimum_budget,*fixture.issuer);repeat_entropy=false;Disarm();
    Check(!r.ok()&&r.error==db::NativeCreationWorkspaceError::identity_failure&&calls[write_call]==0&&device.Size().size_bytes==0,
      "duplicate generated identities refuse before all writes");}
  for(unsigned mode=1;mode<=5;++mode){disk::FileDevice device;
    Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"multipart provider fixture");
    Arm();context_fault=mode;const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,minimum_budget,*fixture.issuer);context_fault=0;Disarm();
    Check(!r.ok()&&!r.receipt&&calls[write_call]==0&&device.Size().size_bytes==0,"multipart hash failures cannot create unsigned roots");}
  {disk::FileDevice device;const auto path=fixture.Next();Check(device.Open(path.string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"guard retention fixture");
    pause_write=true;write_paused=false;resume_write=false;std::atomic<bool> entered=false,acquired=false;
    db::NativeCreationWorkspaceResult result;
    std::thread writer([&]{result=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,minimum_budget,*fixture.issuer);});
    while(!write_paused.load())std::this_thread::yield();
    std::thread observer([&]{entered=true;const auto guard=device.AcquireOperationGuard();acquired=true;});
    while(!entered.load())std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));const bool held=!acquired.load();
    disk::FileDevice other;const auto competing=other.Open(path.string(),disk::FileOpenMode::open_existing_read_only);
    resume_write=true;writer.join();observer.join();
    Check(held&&acquired&&result.ok()&&!competing.ok(),"device and node ownership remain held through real initialization");}
  for(bool allocation:{false,true}){disk::FileDevice device;Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"issuer source failure fixture");
    clock_failure=!allocation;clock_allocation_failure=allocation;Arm();
    const auto refused=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,minimum_budget,*fixture.issuer);
    Disarm();clock_failure=clock_allocation_failure=false;
    Check(!refused.ok()&&!refused.receipt&&refused.error==(allocation?db::NativeCreationWorkspaceError::resource_exhausted:db::NativeCreationWorkspaceError::identity_failure)&&!calls[write_call]&&!device.Size().size_bytes,"issuer failure remains exact and precedes writes");
  }
  std::array<unsigned,call_count> baseline{};
  {disk::FileDevice device;Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"minimum capacity device");
    Arm();const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,minimum_budget,*fixture.issuer);baseline=calls;Disarm();
    Check(r.ok()&&r.receipt->free_pages==0&&ordered&&zero_phase==3,"exact minimum capacity and image budget");Inspect(device,request);}
  const unsigned preparation_hashes=first_write_hashes;
  Check(preparation_hashes!=~0u&&preparation_hashes>0,"the baseline digests every image before its first write");
  // Owner Q12 = 1 (drift removal R3c): there is no create recovery. A file
  // left by an interrupted create is not a database: the fixed-root read the
  // server's open verification starts from refuses it.
  const auto refused_by_fixed_root=[&](disk::FileDevice& device){
    std::vector<byte> backing(2*minimum_budget+(u64{1}<<20));
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(2),request.bootstrap.page_size_profile_uuid,&device}};
    return !db::AcquireNativeFixedRootBackedLease(Id(1),devices,Id(2),{minimum_budget,backing.size()},backing).ok();
  };
  for(unsigned kind=0;kind<call_count;++kind)for(unsigned at=1;at<=baseline[kind];++at){
    disk::FileDevice device;Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"fault fixture");
    Arm(kind,at);const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,minimum_budget,*fixture.issuer);const auto observed=calls;Disarm();
    Check(!r.ok()&&!r.receipt&&observed[kind]>=at,"every injected physical/provider failure returns no receipt");
    const auto size=device.Size();Check(size.ok()&&device.is_open(),"failure preserves open owner");
    if((kind==sync_call&&at==1)||(kind==read_call&&at<=total_pages)||(kind==write_call&&at<=baseline[write_call])){
      const auto zero_page=size.size_bytes>=page_bytes?Read(device,0,page_bytes):std::vector<byte>{};
      Check(std::all_of(zero_page.begin(),zero_page.end(),[](byte v){return !v;}),
        "a failure before the commit point leaves page zero unwritten");
      Check(refused_by_fixed_root(device),"a file interrupted before the commit point is not a database");}
    if(kind==random_call||(kind==hash_call&&at<=preparation_hashes))Check(observed[write_call]==0&&size.size_bytes==0,"preparation failure precedes all writes");
    if(size.size_bytes){const auto replay=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,minimum_budget,*fixture.issuer);
      Check(!replay.ok()&&replay.error==db::NativeCreationWorkspaceError::device_not_empty&&device.Size().size_bytes==size.size_bytes,
        "partial and unacknowledged construction is never truncated or retried");}
  }
  for(unsigned at=1;at<=total_pages+1;++at){disk::FileDevice device;Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"readback corruption fixture");
    Arm();corrupt_at=at;const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,minimum_budget,*fixture.issuer);Disarm();
    Check(!r.ok()&&!r.receipt&&r.error==db::NativeCreationWorkspaceError::readback_mismatch,"every page and page-zero readback byte mismatch refuses");
    if(at<=total_pages){const auto zero_page=Read(device,0,page_bytes);
      Check(std::all_of(zero_page.begin(),zero_page.end(),[](byte v){return !v;}),"a readback mismatch before the commit point leaves page zero unwritten");}}
  // Every write torn in half, page zero's included: the create refuses and the
  // file it leaves is not a database.
  for(unsigned at=1;at<=baseline[write_call];++at){disk::FileDevice device;
    Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"torn write fixture");
    Arm();torn_at=at;const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,minimum_budget,*fixture.issuer);torn_at=0;Disarm();
    Check(!r.ok()&&!r.receipt&&r.error==db::NativeCreationWorkspaceError::io_failure,"a torn write refuses the create");
    Check(refused_by_fixed_root(device),"a file left by a torn write, page zero's included, is not a database");}
  for(unsigned variant=0;variant<15;++variant){auto bad=request;u64 budget=minimum_budget;
    if(variant==0)bad.creator.local_id.value=2;if(variant==1)bad.bootstrap.lifecycle_state=1;
    if(variant==2)bad.total_pages=15;if(variant==3)--budget;if(variant==4)bad.operation_uuid=bad.writer_uuid;
    if(variant==5)bad.creation_utc_millis=0;if(variant==6)bad.bootstrap.filespace_role=5;
    if(variant==7)bad.creator.scope=mga::TransactionScope::cluster_global;
    if(variant==8)bad.bootstrap.flags=disk::FilespaceBootstrapFlag::cluster_authority_required;
    if(variant==9){bad.bootstrap.flags=disk::FilespaceBootstrapFlag::payload_encrypted;bad.bootstrap.encryption_profile_uuid=Id(90);}
    if(variant==10)bad.policy_snapshot_uuid={};if(variant==11)bad.policy_snapshot_uuid.bytes[6]=0x40;
    if(variant==12)bad.policy_snapshot_uuid=Id(91);if(variant==13)bad.bootstrap.database_uuid=Id(91);if(variant==14)bad.policy_snapshot_uuid=bad.writer_uuid;
    disk::FileDevice device;Check(device.Open(fixture.Next().string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"invalid input fixture");
    Arm();const auto r=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,bad,budget,*fixture.issuer);Disarm();
    Check(!r.ok()&&!r.receipt&&calls[write_call]==0&&device.Size().size_bytes==0,"invalid lineage/capacity/provider never touches storage");}
  std::cout<<"PASS native creation workspace checks="<<checks<<" fault sites=";
  for(const auto n:baseline)std::cout<<n<<',';std::cout<<'\n';return 0;
}catch(const std::exception& e){Disarm();std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}

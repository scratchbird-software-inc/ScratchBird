// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_storage_action_intent.hpp"
#include "native_management_extent.hpp"
#include "native_creation_workspace.hpp"
#include "disk_device.hpp"
#include "uuid.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <source_location>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace {
long allocation_budget=-1;
bool counting=false;
unsigned long allocations=0;
unsigned hash_fault=0,hash_target=1,hash_seen=0;
bool hash_active=false;
}
void* operator new(std::size_t n){
  if(counting)++allocations;
  if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}
  if(allocation_budget>0)--allocation_budget;
  if(auto* p=std::malloc(n?n:1))return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){
  hash_active=hash_fault&&++hash_seen==hash_target;
  if(hash_active&&hash_fault==1){hash_fault=0;return nullptr;}
  return __real_EVP_MD_CTX_new();
}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* m,ENGINE* e){
  if(hash_active&&hash_fault==2){hash_fault=0;return 0;}return __real_EVP_DigestInit_ex(c,m,e);
}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* b,size_t n){
  if(hash_active&&hash_fault==3){hash_fault=0;return 0;}return __real_EVP_DigestUpdate(c,b,n);
}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* b,unsigned int* n){
  if(hash_active&&hash_fault==4){hash_fault=0;return 0;}
  const int r=__real_EVP_DigestFinal_ex(c,b,n);if(hash_active&&hash_fault==5){hash_fault=0;*n=31;}return r;
}
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* p,size_t n,unsigned char* out,unsigned int* size,const EVP_MD* md,ENGINE* e){
  const bool hit=hash_fault&&++hash_seen==hash_target;const auto mode=hit?hash_fault:0;
  if(hit)hash_fault=0;if(mode&&mode!=5)return 0;
  const auto r=__real_EVP_Digest(p,n,out,size,md,e);if(mode==5)*size=31;return r;
}
namespace {
using namespace scratchbird::core::platform;
namespace db=scratchbird::storage::database;
namespace disk=scratchbird::storage::disk;
using I=db::NativeStorageActionIntent;
using E=db::NativeStorageIntentError;
using O=db::NativeManagementOperation;
using Bytes=std::vector<byte>;
unsigned checks=0;
void Check(bool condition,const char* text,std::source_location at=std::source_location::current()){
  ++checks;if(!condition)throw std::runtime_error(std::string(text)+" line="+std::to_string(at.line()));
}
Uuid Id(unsigned n){Uuid r;r.bytes[0]=1;r.bytes[6]=0x70;r.bytes[8]=0x80;r.bytes[14]=n>>8;r.bytes[15]=n;return r;}
void Number(Bytes& b,std::size_t at,unsigned width,u64 n){
  for(unsigned k=0;k<width;++k){b.at(at+k)=n%256;n/=256;}
}
void Identity(Bytes& b,std::size_t at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}
std::array<byte,32> Sha(const Bytes& b){std::array<byte,32> out{};Check(SHA256(b.data(),b.size(),out.data())!=nullptr,"independent SHA256");return out;}
void Seal(Bytes& b){const Bytes prefix(b.begin(),b.begin()+608);const auto hash=Sha(prefix);std::copy(hash.begin(),hash.end(),b.begin()+608);}
I Example(unsigned profile,unsigned action){
  I i;i.action=static_cast<db::NativeStorageAction>(action);
  i.request_uuid=Id(1);i.operation_uuid=Id(2);i.database_uuid=Id(3);i.filespace_uuid=Id(4);
  i.locator_uuid=Id(5);i.page_zero_uuid=Id(6);i.page_size_profile_uuid=disk::kCanonicalFilespacePageProfiles[profile].uuid;
  i.policy_snapshot_uuid=Id(7);i.storage_profile_uuid=Id(8);i.initiator_uuid=Id(9);i.request_context_uuid=Id(10);
  // Checkpoint is in a different, mixed-profile member; allocation is local.
  i.checkpoint={9,0x300,Id(11),12,13,disk::kCanonicalFilespacePageProfiles[(profile+1)%5].uuid,Id(14)};
  i.allocation_root={3,3,i.filespace_uuid,15,16,i.page_size_profile_uuid,Id(17)};
  for(unsigned n=0;n<32;++n){i.checkpoint_sha256[n]=n;i.allocation_sha256[n]=255-n;}
  i.checkpoint_generation=18;i.checkpoint_root_set_generation=19;i.directory_generation=20;
  i.filespace_root_set_generation=21;i.page_zero_generation=22;i.map_generation=23;i.capacity_generation=24;
  i.catalog_generation=25;i.policy_generation=26;i.security_generation=27;
  i.current_total_pages=64;i.first_page=action==1?64:32;i.page_count=8;i.maximum_total_pages=128;
  i.page_size_bytes=disk::kCanonicalFilespacePageProfiles[profile].page_size_bytes;
  i.maximum_work_bytes=8*i.page_size_bytes;i.maximum_retained_image_bytes=640;
  i.intended_state=action==1?db::NativeStorageIntentState::free:db::NativeStorageIntentState::preallocated;
  return i;
}
// Literal format offsets and field list, independent of production serializer.
Bytes Oracle(const I& i){
  Bytes b(640,0);const std::string magic="SBSINT01";std::copy(magic.begin(),magic.end(),b.begin());
  Number(b,8,2,1);Number(b,10,2,static_cast<u16>(i.action));Number(b,12,4,640);
  std::size_t at=16;
  for(const auto id:{i.request_uuid,i.operation_uuid,i.database_uuid,i.filespace_uuid,i.locator_uuid,
      i.page_zero_uuid,i.page_size_profile_uuid,i.policy_snapshot_uuid,i.storage_profile_uuid,i.initiator_uuid,i.request_context_uuid}){
    Identity(b,at,id);at+=16;
  }
  for(const auto r:{i.checkpoint,i.allocation_root}){
    Number(b,at,2,r.kind);Number(b,at+4,4,r.page_type);Identity(b,at+8,r.filespace_uuid);
    Number(b,at+24,8,r.page_number);Number(b,at+32,8,r.page_generation);
    Identity(b,at+40,r.page_size_profile_uuid);Identity(b,at+56,r.object_uuid);at+=80;
  }
  std::copy(i.checkpoint_sha256.begin(),i.checkpoint_sha256.end(),b.begin()+352);
  std::copy(i.allocation_sha256.begin(),i.allocation_sha256.end(),b.begin()+384);
  at=416;
  for(const auto n:{i.checkpoint_generation,i.checkpoint_root_set_generation,i.directory_generation,
      i.filespace_root_set_generation,i.page_zero_generation,i.map_generation,i.capacity_generation,
      i.catalog_generation,i.policy_generation,i.security_generation,i.current_total_pages,i.first_page,
      i.page_count,i.maximum_total_pages,i.maximum_work_bytes,i.maximum_retained_image_bytes}){
    Number(b,at,8,n);at+=8;
  }
  Number(b,544,4,i.page_size_bytes);Number(b,548,2,static_cast<u16>(i.intended_state));Seal(b);return b;
}
void Empty(const db::NativeStorageIntentImage& r,E expected){
  Check(r.error==expected&&!r.ok()&&!r.intent&&r.bytes.empty(),"exact failure and no partial intent/image");
}
void Good(const I& i){
  Check(db::ValidateNativeStorageActionIntent(i)==E::none,"valid intent shape");const auto raw=Oracle(i);
  const auto encoded=db::EncodeNativeStorageActionIntent(i,640);Check(encoded.ok()&&encoded.bytes==raw,"exact oracle encode");
  const auto decoded=db::DecodeNativeStorageActionIntent(raw,640);
  Check(decoded.ok()&&decoded.bytes==raw&&Oracle(*decoded.intent)==raw,"all decoded fields match independent oracle");
}
O Operation(const I& i){
  O o;o.database_uuid=i.database_uuid;o.bootstrap_uuid=Id(40);o.uuid=i.operation_uuid;
  o.descriptor_uuid=Id(41);o.family_uuid=Id(42);o.target_type_uuid=Id(43);o.target_uuid=i.filespace_uuid;
  o.initiator_uuid=i.initiator_uuid;o.request_context_uuid=i.request_context_uuid;o.policy_snapshot_uuid=i.policy_snapshot_uuid;
  o.created_at=Id(100);o.updated_at=Id(101);o.idempotency_key="fixed request key";o.initiator_kind=4;
  o.normalized_request_bytes=Oracle(i);o.normalized_request_sha256=Sha(o.normalized_request_bytes);return o;
}
void Malformed(const I& i){
  const auto raw=Oracle(i);
  for(std::size_t size=0;size<640;++size){Bytes b(raw.begin(),raw.begin()+size);Empty(db::DecodeNativeStorageActionIntent(b,640),E::invalid_header);}
  auto b=raw;b.push_back(0);Empty(db::DecodeNativeStorageActionIntent(b,641),E::invalid_header);
  for(std::size_t n=0;n<16;++n){b=raw;b[n]^=0xff;Seal(b);Empty(db::DecodeNativeStorageActionIntent(b,640),E::invalid_header);}
  for(std::size_t n=0;n<640;++n){
    const bool reserved=(n>=194&&n<196)||(n>=264&&n<272)||(n>=274&&n<276)||(n>=344&&n<352)||(n>=550&&n<608);
    if(reserved){b=raw;b[n]=1;Seal(b);Empty(db::DecodeNativeStorageActionIntent(b,640),E::invalid_header);}
  }
  for(std::size_t n=16;n<192;n+=16)for(unsigned fault=0;fault<3;++fault){
    b=raw;if(fault==0)std::fill_n(b.begin()+n,16,0);else if(fault==1)b[n+6]=0x40;else b[n+8]=0;
    Seal(b);Empty(db::DecodeNativeStorageActionIntent(b,640),E::invalid_identity);
  }
  for(std::size_t n=352;n<416;++n){b=raw;b[n]^=1;Empty(db::DecodeNativeStorageActionIntent(b,640),E::invalid_integrity);}
  for(std::size_t n=608;n<640;++n){b=raw;b[n]^=1;Empty(db::DecodeNativeStorageActionIntent(b,640),E::invalid_integrity);}
  for(const u64 budget:{0u,1u,639u}){
    Empty(db::EncodeNativeStorageActionIntent(i,budget),E::resource_exhausted);
    Empty(db::DecodeNativeStorageActionIntent(raw,budget),E::resource_exhausted);
  }
}
void Bounds(I i){
  const auto invalid=[&](I x,E error){Check(db::ValidateNativeStorageActionIntent(x)==error,"shape refusal");Empty(db::EncodeNativeStorageActionIntent(x,640),error);Empty(db::DecodeNativeStorageActionIntent(Oracle(x),640),error);};
  auto x=i;x.page_size_bytes=4096;invalid(x,E::invalid_profile);
  x=i;x.page_size_profile_uuid=Id(99);invalid(x,E::invalid_profile);
  for(unsigned root=0;root<2;++root)for(unsigned fault=0;fault<9;++fault){
    x=i;auto& r=root?x.allocation_root:x.checkpoint;
    switch(fault){case 0:r.kind=0;break;case 1:r.page_type=1;break;case 2:r.filespace_uuid={};break;
      case 3:r.page_number=0;break;case 4:r.page_generation=0;break;case 5:r.page_size_profile_uuid=Id(99);break;
      case 6:r.object_uuid={};break;case 7:r.filespace_uuid.bytes[6]=0x40;break;case 8:r.object_uuid.bytes[8]=0;break;}
    invalid(x,E::invalid_reference);
  }
  x=i;x.allocation_root.filespace_uuid=Id(99);invalid(x,E::invalid_reference);
  x=i;x.allocation_root.page_size_profile_uuid=i.checkpoint.page_size_profile_uuid;invalid(x,E::invalid_reference);
  x=i;x.allocation_root.page_number=i.current_total_pages;invalid(x,E::invalid_reference);
  x=i;x.checkpoint.page_number=std::numeric_limits<u64>::max();invalid(x,E::invalid_reference);
  x=i;x.checkpoint.filespace_uuid=i.filespace_uuid;invalid(x,E::invalid_reference);
  x.checkpoint.page_size_profile_uuid=i.page_size_profile_uuid;Good(x);
  x.checkpoint.page_number=i.current_total_pages;invalid(x,E::invalid_reference);
  x=i;x.checkpoint_sha256={};invalid(x,E::invalid_integrity);x=i;x.allocation_sha256={};invalid(x,E::invalid_integrity);
  for(const auto member:{&I::checkpoint_generation,&I::checkpoint_root_set_generation,&I::directory_generation,
      &I::filespace_root_set_generation,&I::page_zero_generation,&I::map_generation,&I::capacity_generation}){
    x=i;x.*member=0;invalid(x,E::invalid_range);
  }
  for(const auto member:{&I::page_count,&I::maximum_total_pages,&I::maximum_work_bytes,&I::maximum_retained_image_bytes}){
    x=i;x.*member=0;invalid(x,E::invalid_range);
  }
  x=i;x.catalog_generation=x.policy_generation=x.security_generation=0;Good(x);
  x=i;x.maximum_work_bytes--;invalid(x,E::invalid_range);
  x=i;x.maximum_retained_image_bytes=639;invalid(x,E::invalid_range);
  x=i;x.intended_state=static_cast<db::NativeStorageIntentState>(1);invalid(x,E::invalid_range);
  x=i;x.first_page=std::numeric_limits<u64>::max();invalid(x,E::invalid_range);
  x=i;x.page_count=std::numeric_limits<u64>::max();invalid(x,E::invalid_range);
  const auto limit=std::numeric_limits<u64>::max()/i.page_size_bytes;
  x=i;x.maximum_total_pages=limit+1;invalid(x,E::invalid_range);
  x=i;x.maximum_total_pages=limit;x.current_total_pages=limit-1;
  x.first_page=i.action==db::NativeStorageAction::physical_growth?limit-1:limit-2;
  x.page_count=1;x.maximum_work_bytes=i.page_size_bytes;Good(x);
  if(i.action==db::NativeStorageAction::physical_growth){
    x=i;x.maximum_total_pages=i.current_total_pages+i.page_count;Good(x);
    --x.maximum_total_pages;invalid(x,E::invalid_range);
    x=i;x.first_page--;invalid(x,E::invalid_range);
    x=i;x.intended_state=db::NativeStorageIntentState::preallocated;Good(x);
  }else{
    x=i;x.first_page=i.current_total_pages-i.page_count;Good(x);
    ++x.first_page;invalid(x,E::invalid_range);x=i;x.first_page=0;invalid(x,E::invalid_range);
    x=i;x.intended_state=db::NativeStorageIntentState::free;invalid(x,E::invalid_range);
  }
}
void Binding(const I& i){
  const auto o=Operation(i);
  Check(db::ReadNativeStorageActionIntentFromOperation(o,640).ok(),"created intent binds without granting authorization");
  for(const auto member:{&O::uuid,&O::database_uuid,&O::target_uuid,&O::initiator_uuid,&O::request_context_uuid,&O::policy_snapshot_uuid}){
    auto changed=o;changed.*member=Id(88);Empty(db::ReadNativeStorageActionIntentFromOperation(changed,640),E::binding_mismatch);
  }
  auto authorized=o;authorized.state=db::NativeManagementState::authorized;authorized.security_snapshot_uuid=Id(55);
  authorized.generation_guards={i.catalog_generation,i.policy_generation,i.security_generation,std::nullopt};
  Check(db::ReadNativeStorageActionIntentFromOperation(authorized,640).ok(),"present matching guards");
  for(unsigned n=0;n<3;++n){auto bad=authorized;(*bad.generation_guards[n])++;Empty(db::ReadNativeStorageActionIntentFromOperation(bad,640),E::binding_mismatch);}
  auto bad=authorized;bad.scope=db::NativeManagementScope::cluster;bad.cluster_uuid=Id(77);bad.generation_guards[3]=0;
  Empty(db::ReadNativeStorageActionIntentFromOperation(bad,640),E::binding_mismatch);
  bad=o;bad.normalized_request_sha256[0]^=1;const auto failure=db::ReadNativeStorageActionIntentFromOperation(bad,640);
  Empty(failure,E::operation_failure);Check(failure.operation_error==db::NativeManagementOperationError::invalid_integrity,"preserved nested digest failure");
  bad=o;bad.normalized_request_bytes.clear();Empty(db::ReadNativeStorageActionIntentFromOperation(bad,640),E::invalid_header);
  Empty(db::ReadNativeStorageActionIntentFromOperation(o,639),E::resource_exhausted);
  const auto stored=db::EncodeNativeManagementOperation(authorized,4096);Check(stored.ok(),"real operation codec accepts exact typed bytes");
  const auto reopened=db::DecodeNativeManagementOperation(stored.bytes,4096);Check(reopened.ok(),"real operation decode");
  const auto bound=db::ReadNativeStorageActionIntentFromOperation(*reopened.record,640);
  Check(bound.ok()&&bound.bytes==Oracle(i),"binary typed intent survives retained operation encoding");
  auto retry=authorized;retry.revision++;retry.updated_at=Id(102);
  Check(db::ValidateNativeManagementOperationEvolution(authorized,retry)==db::NativeManagementOperationError::none,"unchanged intent retry");
  auto conflicting=i;--conflicting.page_count;retry.normalized_request_bytes=Oracle(conflicting);retry.normalized_request_sha256=Sha(retry.normalized_request_bytes);
  Check(db::ValidateNativeManagementOperationEvolution(authorized,retry)==db::NativeManagementOperationError::immutable_field,"conflicting bounded work is not a retry");
}
void Faults(const I& i){
  const auto raw=Oracle(i);const auto operation=Operation(i);
  for(unsigned mode=0;mode<3;++mode){
    const auto call=[&](){return mode==0?db::EncodeNativeStorageActionIntent(i,640):
      mode==1?db::DecodeNativeStorageActionIntent(raw,640):db::ReadNativeStorageActionIntentFromOperation(operation,640);};
    counting=true;allocations=0;const auto measured=call();counting=false;
    Check(measured.ok(),"allocation baseline succeeds");const auto sites=allocations;Check(sites>0,"fault sites measured");
    for(unsigned long n=0;n<sites;++n){allocation_budget=n;const auto r=call();const bool consumed=allocation_budget==-1;allocation_budget=-1;
      Check(consumed,"each allocation fault reached");Empty(r,E::resource_exhausted);}
    for(unsigned site=1;site<=(mode==2?2u:1u);++site)for(unsigned fault=1;fault<=5;++fault){
      hash_target=site;hash_seen=0;hash_active=false;hash_fault=fault;const auto r=call();const bool consumed=hash_fault==0;hash_fault=0;
      Check(consumed,"every hash backend fault reached");Empty(r,E::hash_failure);
    }
  }
}
struct TemporaryDirectory {
  std::filesystem::path path;
  TemporaryDirectory(){char name[]="/tmp/sb-storage-intent.XXXXXX";Check(mkdtemp(name)!=nullptr,"unique real-file fixture");path=name;}
  ~TemporaryDirectory(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}
};
void Physical(const I& i){
  TemporaryDirectory temporary;const auto path=(temporary.path/"node").string();
  disk::FileDevice device;Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"create owned native member");
  const auto& p=*disk::FindCanonicalFilespacePageProfile(i.checkpoint.page_size_profile_uuid);
  db::NativeFilespaceInitializationRequest request;
  request.bootstrap={i.database_uuid,i.checkpoint.filespace_uuid,p.uuid,disk::kNativeBootstrapIntegrityProfile,
    {},p.page_size_bytes,1,0,1,7};
  request.operation_uuid=Id(200);request.writer_uuid=Id(201);
  request.creator.transaction_uuid={UuidKind::transaction,Id(202)};
  request.creator.local_id=scratchbird::transaction::mga::MakeLocalTransactionId(1);
  request.creator.scope=scratchbird::transaction::mga::TransactionScope::local_node;
  request.creation_utc_millis=1789357072000ULL;request.total_pages=128;
  request.policy_snapshot_uuid=i.policy_snapshot_uuid;
  scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({i.database_uuid,i.policy_snapshot_uuid},{{},0,1000});
  Check(db::InitializeNativeCreationWorkspaceOnOpenDevice(device,request,1<<26,issuer).ok(),"actual native fixture bootstrap");
  const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(device);Check(zero.ok(),"read actual fixture bootstrap");
  auto operation=Operation(i);operation.bootstrap_uuid=zero.record->page_uuid;
  const std::vector<disk::NativeCommonPageHeader> headers{{p.page_size_bytes,0x500,i.database_uuid,
    i.checkpoint.filespace_uuid,Id(203),64,1,0,p.uuid}};
  const auto extent=db::EncodeNativeManagementExtent(operation,Id(204),headers,1<<26);
  Check(extent.ok()&&extent.pages.size()==1,"typed request fits actual immutable extent");
  const auto write=device.WriteAt(64*u64{p.page_size_bytes},extent.pages[0].data(),extent.pages[0].size());
  Check(write.ok()&&write.bytes_transferred==p.page_size_bytes&&device.Sync().ok(),"actual extent write and sync");
  Check(device.Close().ok(),"close writer before independent reopen");
  const auto read=[&](){
    disk::FileDevice reader;if(!reader.Open(path,disk::FileOpenMode::open_existing_read_only).ok())return false;
    const auto retained=db::ReadNativeManagementExtentFromOpenDevice(
      {i.checkpoint.filespace_uuid,p.uuid,&reader},*extent.root,i.database_uuid,operation.bootstrap_uuid,1<<26);
    if(!retained.ok()||*retained.record!=operation)return false;
    const auto intent=db::ReadNativeStorageActionIntentFromOperation(*retained.record,640);
    return intent.ok()&&intent.bytes==Oracle(i)&&Oracle(*intent.intent)==Oracle(i);
  };
  Check(read(),"real-file read-only reopen preserves typed request and complete operation");
  const auto child=fork();Check(child>=0,"independent recovery reader process");
  if(child==0){try{_exit(read()?0:1);}catch(...){_exit(2);}}
  int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
    "independent process recovers exact typed intent, not parent cache");
  // The extent is intentionally unselected. This does not assert allocation,
  // operation admission, checkpoint publication, or actual growth effects.
}
}
int main(){
  try{for(unsigned profile=0;profile<5;++profile)for(unsigned action=1;action<=2;++action){
    const auto i=Example(profile,action);Good(i);Malformed(i);Bounds(i);Binding(i);Faults(i);Physical(i);
  }std::cout<<"PASS native storage action intent checks="<<checks<<'\n';return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}

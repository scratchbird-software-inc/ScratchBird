// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_inventory_publication_delta.hpp"
#include "isolation.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <source_location>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
namespace {
long allocation_budget=-1; bool counting=false; unsigned long allocations=0;
unsigned hash_fault=0,hash_target=0,hash_seen=0; bool hash_counting=false,hash_active=false;
}
void* operator new(std::size_t n){if(counting)++allocations;if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){hash_active=(hash_fault||hash_counting)&&++hash_seen==hash_target&&hash_fault;if(hash_active&&hash_fault==1){hash_fault=0;return nullptr;}return __real_EVP_MD_CTX_new();}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* m,ENGINE* e){if(hash_active&&hash_fault==2){hash_fault=0;return 0;}return __real_EVP_DigestInit_ex(c,m,e);}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* b,size_t n){if(hash_active&&hash_fault==3){hash_fault=0;return 0;}return __real_EVP_DigestUpdate(c,b,n);}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* b,unsigned int* n){if(hash_active&&hash_fault==4){hash_fault=0;return 0;}const int r=__real_EVP_DigestFinal_ex(c,b,n);if(hash_active&&hash_fault==5){hash_fault=0;*n=31;}return r;}
extern "C" int __real_EVP_Digest(const void*,size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* p,size_t n,unsigned char* b,unsigned int* size,const EVP_MD* m,ENGINE* e){const bool selected=(hash_fault||hash_counting)&&++hash_seen==hash_target&&hash_fault;const auto mode=selected?hash_fault:0;if(selected)hash_fault=0;if(mode&&mode!=5)return 0;const int r=__real_EVP_Digest(p,n,b,size,m,e);if(mode==5)*size=31;return r;}
namespace {
using namespace scratchbird::core::platform;
namespace db=scratchbird::storage::database;namespace disk=scratchbird::storage::disk;namespace page=scratchbird::storage::page;namespace mga=scratchbird::transaction::mga;
using Bytes=std::vector<byte>;using Images=std::vector<Bytes>;using Entry=mga::TransactionInventoryEntry;using State=mga::TransactionState;using E=db::NativeInventoryDeltaError;
unsigned checks=0;
void Check(bool ok,const char* why,std::source_location location=std::source_location::current()){++checks;if(!ok)throw std::runtime_error(std::string(why)+" line="+std::to_string(location.line()));}
Uuid Id(unsigned n){Uuid u;u.bytes[0]=1;u.bytes[6]=0x70;u.bytes[8]=0x80;u.bytes[14]=n>>8;u.bytes[15]=n;return u;}
void Num(Bytes& b,std::size_t at,unsigned count,u64 n){for(unsigned i=0;i<count;++i)b[at+i]=n>>(8*i);}
void Put(Bytes& b,std::size_t at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),b.begin()+at);}
auto Sha(const Bytes& bytes){std::array<byte,32> out{};Check(SHA256(bytes.data(),bytes.size(),out.data()),"independent SHA256");return out;}
auto Ref(const page::NativeTransactionInventoryPage& p){const auto& h=p.header;return disk::NativePageReference{h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
void PutRef(Bytes& b,std::size_t at,const disk::NativePageReference& r){Put(b,at,r.filespace_uuid);Num(b,at+16,8,r.page_number);Num(b,at+24,8,r.page_generation);Put(b,at+32,r.page_size_profile_uuid);}
void Seal(Bytes& b){std::fill(b.begin()+320,b.begin()+352,0);const auto sha=Sha(b);std::copy(sha.begin(),sha.end(),b.begin()+320);}
Bytes Oracle(const page::NativeTransactionInventoryPage& p){
 const auto& h=p.header;Bytes b(h.page_size_bytes);std::copy_n("SBPGV002",8,b.begin());Num(b,8,4,128);Num(b,12,4,h.page_size_bytes);Num(b,16,4,0x301);Num(b,20,2,1);Num(b,22,2,1);Put(b,24,h.database_uuid);Put(b,40,h.filespace_uuid);Put(b,56,h.page_uuid);Num(b,72,8,h.page_number);Num(b,80,8,h.page_generation);Num(b,88,8,h.flags);Put(b,104,h.page_size_profile_uuid);Num(b,120,2,1);
 u64 header_hash=14695981039346656037ull;for(unsigned i=0;i<128;++i){header_hash^=b[i];header_hash*=1099511628211ull;}Num(b,96,8,header_hash);
 std::copy_n("SBTINV01",8,b.begin()+128);Num(b,136,2,1);Num(b,138,2,256);Num(b,140,4,384+72*p.inventory.entries.size());Put(b,144,p.object_uuid);Num(b,160,8,p.inventory_generation);Num(b,168,8,p.inventory.next_local_transaction_id);Num(b,176,8,p.inventory.next_commit_sequence);Num(b,184,4,p.inventory.entries.size());if(p.previous)PutRef(b,192,*p.previous);if(p.next)PutRef(b,240,*p.next);
 u64 oit=p.inventory.next_local_transaction_id,oat=oit;
 for(std::size_t i=0;i<p.inventory.entries.size();++i){const auto& e=p.inventory.entries[i];const auto at=384+72*i;Num(b,at,8,e.identity.local_id.value);Put(b,at+8,e.identity.transaction_uuid.value);Num(b,at+24,2,u16(e.identity.scope));Num(b,at+26,2,u16(e.state));const unsigned origin=e.archived_from_state==State::committed?1:e.archived_from_state==State::rolled_back?2:e.archived_from_state==State::failed_terminal?3:0;Num(b,at+28,4,(e.evidence_record_required?1:0)|(e.evidence_record_written?2:0)|(e.rollback_only?4:0)|(e.stable_snapshot?32:0)|(origin<<3));Num(b,at+32,8,e.begin_unix_epoch_millis);Num(b,at+40,8,e.final_unix_epoch_millis);Num(b,at+48,8,e.begin_visible_through_local_transaction_id);Num(b,at+56,8,e.begin_visible_through_commit_sequence);Num(b,at+64,8,e.commit_sequence);
  const auto outcome=e.state==State::archived?e.archived_from_state:e.state;if(outcome!=State::committed&&outcome!=State::rolled_back)oit=std::min(oit,e.identity.local_id.value);if(e.state==State::active||e.state==State::read_only_active)oat=std::min(oat,e.identity.local_id.value);
 }
 Num(b,288,8,oit);Num(b,296,8,oat);Num(b,304,8,oat);Seal(b);return b;
}
Entry MakeEntry(u64 local,State state=State::created){Entry e;e.identity.transaction_uuid={UuidKind::transaction,Id(100+local)};e.identity.local_id=mga::MakeLocalTransactionId(local);e.identity.scope=mga::TransactionScope::local_node;e.state=state;e.begin_unix_epoch_millis=1000+local;e.begin_visible_through_local_transaction_id=local-1;e.begin_visible_through_commit_sequence=local==1?0:1;if(state==State::committed){e.commit_sequence=1;e.final_unix_epoch_millis=2000;e.evidence_record_written=true;}return e;}
struct Fixture {
 std::vector<page::NativeTransactionInventoryPage> before,after;Images old_images,new_images;db::NativeCheckpointRootReference old_root,new_root;u64 budget=0;
 Fixture(unsigned first=0,unsigned second=0){
  const auto make=[](unsigned profile,unsigned file,u64 number,unsigned uuid,u64 generation){page::NativeTransactionInventoryPage p;const auto& f=disk::kCanonicalFilespacePageProfiles[profile];p.header={f.page_size_bytes,0x301,Id(1),Id(file),Id(uuid),number,7,0,f.uuid};p.object_uuid=Id(3);p.inventory_generation=generation;p.inventory.next_commit_sequence=2;return p;};
  before={make(first,2,10,10,4)};before[0].inventory.next_local_transaction_id=2;before[0].inventory.entries={MakeEntry(1,State::committed)};
  after={make(first,2,11,11,9),make(second,4,12,12,9)};for(auto& p:after)p.inventory.next_local_transaction_id=3;after[0].inventory.entries=before[0].inventory.entries;after[1].inventory.entries={MakeEntry(2)};Relink();Refresh();
 }
 void Relink(){for(auto* chain:{&before,&after})for(std::size_t i=0;i<chain->size();++i){auto& p=(*chain)[i];p.previous=i?std::optional{Ref((*chain)[i-1])}:std::nullopt;p.next=i+1<chain->size()?std::optional{Ref((*chain)[i+1])}:std::nullopt;}}
 void Refresh(){old_images.clear();new_images.clear();budget=0;for(auto* chain:{&before,&after})for(const auto& p:*chain){auto bytes=Oracle(p);budget+=4*bytes.size();(chain==&before?old_images:new_images).push_back(std::move(bytes));}old_root={1,0x301,Ref(before.front()),Id(3),Sha(old_images.front())};new_root={1,0x301,Ref(after.front()),Id(3),Sha(new_images.front())};}
 auto CheckDelta(u64 allowance=0){return db::ValidateNativeInventoryPublicationDelta(Id(1),old_root,new_root,9,old_images,new_images,allowance?allowance:budget);}
};
void Failed(const db::NativeInventoryDeltaResult& r){Check(!r.ok()&&!r.delta,"failure carries no inventory or difference prefix");}
void ColdFaults(){
 Fixture f(0,4);int channel[2];Check(pipe(channel)==0,"cold allocation measurement pipe");const auto child=fork();Check(child>=0,"cold allocation baseline fork");
 if(child==0){close(channel[0]);allocations=0;counting=true;const auto result=f.CheckDelta();counting=false;const auto sites=allocations;const auto n=write(channel[1],&sites,sizeof(sites));_exit(result.ok()&&n==sizeof(sites)?0:1);}
 close(channel[1]);unsigned long sites=0;const auto n=read(channel[0],&sites,sizeof(sites));close(channel[0]);int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0&&n==sizeof(sites)&&sites>0,"actual first-use allocation count");
 for(unsigned long at=0;at<=sites;++at){const auto isolated=fork();Check(isolated>=0,"isolated first-use allocation fault");if(isolated==0){allocation_budget=at;const auto result=f.CheckDelta();const auto remaining=allocation_budget;allocation_budget=-1;const bool ok=at==sites?result.ok()&&remaining==0:result.error==E::resource_exhausted&&!result.delta&&remaining==-1;_exit(ok?0:1);}status=0;Check(waitpid(isolated,&status,0)==isolated&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"cold required allocation failure including lazy lifecycle initialization has no prefix");}
 std::cout<<"cold allocation sites="<<sites<<"\n";
}
void Profiles(){for(unsigned a=0;a<5;++a)for(unsigned b=0;b<5;++b){Fixture f(a,b);const auto result=f.CheckDelta();Check(result.ok()&&result.delta->verified_image_bytes==f.budget&&!result.delta->cluster_difference&&result.delta->differences.size()==1&&!result.delta->differences[0].before&&result.delta->differences[0].after->identity.local_id.value==2,"mixed-size independent starting images and exact added difference");for(const auto& p:f.after){const auto encoded=page::EncodeNativeTransactionInventoryPage(p);Check(encoded.ok()&&encoded.bytes==Oracle(p),"independent full-image oracle agrees");}Failed(f.CheckDelta(f.budget-1));
  f.before=f.after;for(auto& p:f.before)p.inventory_generation=8;for(auto& p:f.after){p.header.page_number+=20;p.header.page_uuid=Id(1000+p.header.page_number);}f.after[1].inventory.entries[0].state=State::active;f.Relink();f.Refresh();const auto activation=f.CheckDelta();Check(activation.ok()&&activation.delta->differences.size()==1&&activation.delta->differences[0].before->state==State::created&&activation.delta->differences[0].after->state==State::active,"activation preserves actual starting predecessor");
  f.after[1].inventory.entries[0].state=State::created;f.Refresh();Check(f.CheckDelta().ok()&&f.CheckDelta().delta->differences.empty(),"equal state remains valid new publication");
 }}
void Negatives(){
 for(unsigned field=0;field<16;++field){Fixture f;auto& e=f.after[1].inventory.entries[0];if(field==0)e.state=State::active;if(field==1)e.state=State::read_only_active;if(field==2)e.state=State::prepared;if(field==3)e.final_unix_epoch_millis=1;if(field==4)e.rollback_only=true;if(field==5)e.evidence_record_written=true;if(field==6)e.begin_visible_through_local_transaction_id=0;if(field==7)e.begin_visible_through_commit_sequence=0;if(field==8)e.identity.scope=mga::TransactionScope::cluster_global;
  if(field==9){e.identity.local_id=mga::MakeLocalTransactionId(3);e.begin_visible_through_local_transaction_id=2;for(auto& p:f.after)p.inventory.next_local_transaction_id=4;}
  if(field==10)for(auto& p:f.after)p.inventory.next_local_transaction_id=4;if(field==11)e.identity.transaction_uuid.value=f.before[0].inventory.entries[0].identity.transaction_uuid.value;if(field==12)f.after[0].inventory.entries[0].begin_unix_epoch_millis++;if(field==13)f.after[0].inventory.entries[0].final_unix_epoch_millis++;if(field==14)f.after[0].inventory.entries[0].evidence_record_written=false;if(field==15){e.state=State::committed;e.commit_sequence=2;e.final_unix_epoch_millis=2001;for(auto& p:f.after)p.inventory.next_commit_sequence=3;}
  f.Refresh();Failed(f.CheckDelta());
 }
 for(unsigned field=0;field<12;++field){Fixture f;if(field==0)f.after[1].header.database_uuid=Id(9);if(field==1)f.after[1].object_uuid=Id(9);if(field==2)f.after[1].previous.reset();if(field==3)f.after[0].next.reset();if(field==4)f.after[1].inventory_generation=8;if(field==5)f.after[1].inventory.next_local_transaction_id=4;if(field==6)f.after[1].header.page_uuid=f.before[0].header.page_uuid;if(field==7){f.after[1].header.filespace_uuid=Id(2);f.after[1].header.page_number=10;f.Relink();}if(field==8)f.after[1].header.flags=1;if(field==9)f.after[1].next=Ref(f.before[0]);if(field==10)f.after[1].header.page_uuid=f.after[0].header.page_uuid;if(field==11){f.after[0].inventory_generation=4;f.after[1].inventory_generation=4;}f.Refresh();Failed(f.CheckDelta());}
 Fixture f;auto root=f.new_root;root.sha256[0]^=1;Failed(db::ValidateNativeInventoryPublicationDelta(Id(1),f.old_root,root,9,f.old_images,f.new_images,f.budget));root=f.new_root;root.role=2;Failed(db::ValidateNativeInventoryPublicationDelta(Id(1),f.old_root,root,9,f.old_images,f.new_images,f.budget));
 f.new_images.back().back()^=1;Failed(f.CheckDelta());
}
void Differences(){
 Fixture f;f.after[1].inventory.entries.clear();for(auto& p:f.after)p.inventory.next_local_transaction_id=2;f.after[0].inventory.entries.clear();f.Refresh();auto removed=f.CheckDelta();Check(removed.ok()&&removed.delta->differences.size()==1&&removed.delta->differences[0].before->state==State::committed&&!removed.delta->differences[0].after,"terminal removal retained explicitly for retention authority");
 f.before[0].inventory.entries[0].identity.scope=mga::TransactionScope::cluster_global;f.Refresh();Check(f.CheckDelta().ok()&&f.CheckDelta().delta->cluster_difference,"removed cluster terminal state requires cluster authority");
 f.after[0].inventory.entries=f.before[0].inventory.entries;f.Refresh();Check(f.CheckDelta().ok()&&!f.CheckDelta().delta->cluster_difference,"unchanged cluster entry does not invent a changed outcome");
 f.before[0].inventory.entries[0]=MakeEntry(1,State::created);f.after[0].inventory.entries[0]=f.before[0].inventory.entries[0];f.after[0].inventory.entries[0].identity.scope=mga::TransactionScope::cluster_global;f.Refresh();Failed(f.CheckDelta());
 f.after[0].inventory.entries.clear();f.Refresh();Failed(f.CheckDelta());
}
void Boundaries(){
 {Fixture f;f.after[1].header.flags=1;f.Refresh();f.new_images[1][400]^=1;const auto encrypted=f.CheckDelta();Check(encrypted.error==E::encrypted_requires_authority&&!encrypted.delta,"encrypted payload is routed to its owning authority before plaintext decoding");}
 {Fixture f;f.after[1].inventory.entries.push_back(MakeEntry(3));for(auto& p:f.after)p.inventory.next_local_transaction_id=4;f.Refresh();const auto delta=f.CheckDelta();Check(delta.ok()&&delta.delta->differences.size()==2&&delta.delta->differences[0].after->identity.local_id.value==2&&delta.delta->differences[1].after->identity.local_id.value==3,"contiguous batch allocation reports every starting identity in number order");}
 {Fixture f;const auto max=std::numeric_limits<u64>::max();f.before[0].inventory.entries.clear();f.before[0].inventory.next_local_transaction_id=max-1;for(auto& p:f.after)p.inventory.next_local_transaction_id=max;f.after[0].inventory.entries.clear();auto& e=f.after[1].inventory.entries[0];e.identity.local_id=mga::MakeLocalTransactionId(max-1);e.begin_visible_through_local_transaction_id=max-2;f.Refresh();Check(f.CheckDelta().ok(),"last representable allocation never wraps its next counter");e.identity.local_id=mga::MakeLocalTransactionId(max);f.Refresh();Failed(f.CheckDelta());}
 {Fixture f;const auto& profile=disk::kCanonicalFilespacePageProfiles[1];f.after[0].header.page_size_profile_uuid=profile.uuid;f.after[0].header.page_size_bytes=profile.page_size_bytes;f.Relink();f.Refresh();for(const auto& bytes:f.new_images)Check(page::DecodeNativeTransactionInventoryPage(bytes).ok(),"profile substitution images individually canonical");Failed(f.CheckDelta());}
 for(unsigned field=0;field<5;++field){Fixture f;f.before[0].inventory.next_local_transaction_id=3;f.before[0].inventory.entries.push_back(MakeEntry(2,State::active));f.after[1].inventory.entries[0]=f.before[0].inventory.entries[1];auto& entry=f.after[1].inventory.entries[0];
  if(field==0)entry.rollback_only=true;if(field==1)entry.evidence_record_written=true;if(field==2)entry.evidence_record_required=false;if(field==3)entry.begin_visible_through_commit_sequence=0;if(field==4){entry.state=State::committed;entry.commit_sequence=2;entry.final_unix_epoch_millis=3000;for(auto& p:f.after)p.inventory.next_commit_sequence=3;}
  f.Refresh();const auto result=f.CheckDelta();if(field==2||field==3)Failed(result);else Check(result.ok()&&result.delta->differences.size()==1&&result.delta->differences[0].before->state==State::active&&result.delta->differences[0].after->rollback_only==entry.rollback_only&&result.delta->differences[0].after->evidence_record_written==entry.evidence_record_written&&result.delta->differences[0].after->commit_sequence==entry.commit_sequence,"every changed durable field is retained for execution authority");
 }
 {Fixture f;f.before[0].inventory.next_local_transaction_id=3;auto active=MakeEntry(2,State::active);active.identity.scope=mga::TransactionScope::cluster_global;f.before[0].inventory.entries.push_back(active);f.after[1].inventory.entries[0]=active;f.after[1].inventory.entries[0].rollback_only=true;f.Refresh();const auto result=f.CheckDelta();Check(result.ok()&&result.delta->cluster_difference&&result.delta->differences.size()==1,"retained cluster change is explicit and supplies no local decision grant");}
 {Fixture f;f.before=f.after;for(auto& p:f.before)p.inventory_generation=4;f.before[1].inventory.entries[0]=MakeEntry(2,State::committed);f.before[1].inventory.entries[0].begin_visible_through_commit_sequence=0;f.Relink();f.Refresh();for(const auto& bytes:f.old_images)Check(page::DecodeNativeTransactionInventoryPage(bytes).ok(),"duplicate commit sequence lies across individually canonical pages");Failed(f.CheckDelta());}
 {Fixture f;f.new_images.pop_back();Failed(f.CheckDelta());f.Refresh();f.new_images.push_back(f.new_images.back());Failed(f.CheckDelta());f.Refresh();f.new_images[1].resize(127);Failed(f.CheckDelta());f.Refresh();Failed(db::ValidateNativeInventoryPublicationDelta(Id(1),f.old_root,f.new_root,0,f.old_images,f.new_images,f.budget));Failed(db::ValidateNativeInventoryPublicationDelta(Id(1),f.old_root,f.new_root,9,f.old_images,f.new_images,0));}
}
// Exercise real BEGIN candidates, not just fixture entries that already carry
// the expected boundary. This remains image admission, not durable BEGIN.
void BeginCandidates() {
 for (unsigned profile=0; profile<5; ++profile)
  for (bool read_only : {false,true})
   for (unsigned predecessor=0; predecessor<17; ++predecessor) {
    Fixture f(profile,4-profile);
    auto& old=f.before.front().inventory;
    old.next_local_transaction_id=8;
    old.next_commit_sequence=4;
    old.entries.clear();
    if (predecessor<15) {
      auto entry=MakeEntry(7);
      entry.state=predecessor<12 ? static_cast<State>(predecessor+1) : State::archived;
      if (predecessor==12) entry.state=State::read_only_active;
      if (entry.state==State::archived)
        entry.archived_from_state=predecessor==11 ? State::committed :
          predecessor==13 ? State::rolled_back : State::failed_terminal;
      const auto outcome=entry.state==State::archived ? entry.archived_from_state : entry.state;
      if (outcome==State::committed || outcome==State::rolled_back || outcome==State::failed_terminal) {
        entry.final_unix_epoch_millis=2000;
        entry.evidence_record_written=true;
      }
      if (outcome==State::committed) entry.commit_sequence=3;
      old.entries.push_back(entry);
    }
    // Also cover a pristine inventory and a pruned predecessor with a retained
    // committed entry far below the non-reusable allocation high watermark.
    if (predecessor==15) {
      old.next_local_transaction_id=1;
      old.next_commit_sequence=1;
    } else if (predecessor==16) old.entries.push_back(MakeEntry(1,State::committed));
    const auto begun=read_only ? mga::BeginLocalReadOnlyTransaction(old,{UuidKind::transaction,Id(900)},3000) :
      mga::BeginLocalTransaction(old,{UuidKind::transaction,Id(900)},3000);
    Check(begun.ok(),"actual pure BEGIN accepts valid retained predecessor");
    Check(begun.entry.begin_visible_through_local_transaction_id==old.next_local_transaction_id-1,
      "BEGIN records allocated-number boundary independent of retained committed entries");
    Check(begun.entry.begin_visible_through_commit_sequence==old.next_commit_sequence-1,
      "BEGIN preserves independent commit-order boundary");
    Check(begun.entry.state==(read_only ? State::read_only_active : State::active) &&
      begun.inventory.next_local_transaction_id==old.next_local_transaction_id+1,
      "pure BEGIN candidate state and non-reused allocation");
    for(auto& p:f.after) {
      p.inventory.next_local_transaction_id=begun.inventory.next_local_transaction_id;
      p.inventory.next_commit_sequence=begun.inventory.next_commit_sequence;
    }
    f.after[0].inventory.entries=old.entries;
    f.after[1].inventory.entries={begun.entry};
    f.Refresh();
    Check(f.CheckDelta().error==E::starting_allocation_required,
      "actual active BEGIN candidate cannot skip durable starting publication");
    f.after[1].inventory.entries.front().state=State::created;
    f.Refresh();
    const auto starting=f.CheckDelta();
    Check(starting.ok() && starting.delta->differences.size()==1 &&
      starting.delta->differences.front().after->identity.transaction_uuid.value==Id(900),
      "actual BEGIN immutable fields pass complete native starting-image admission");
    f.before=f.after;
    for(auto& p:f.before) p.inventory_generation=8;
    for(auto& p:f.after) {p.header.page_number+=20;p.header.page_uuid=Id(1000+p.header.page_number);}
    f.after[1].inventory.entries.front()=begun.entry;
    f.Relink();f.Refresh();
    Check(f.CheckDelta().ok(),"actual BEGIN activation preserves its starting predecessor");
   }
}
void BeginCounterEdges() {
 for (bool read_only : {false,true}) {
  const auto begin=[&](const mga::LocalTransactionInventory& inventory,TypedUuid id) {
    return read_only ? mga::BeginLocalReadOnlyTransaction(inventory,id,3000) :
      mga::BeginLocalTransaction(inventory,id,3000);
  };
  auto inventory=mga::MakeEmptyLocalTransactionInventory();
  inventory.next_local_transaction_id=std::numeric_limits<u64>::max()-1;
  inventory.next_commit_sequence=std::numeric_limits<u64>::max();
  auto begun=begin(inventory,{UuidKind::transaction,Id(901)});
  Check(begun.ok() && begun.entry.begin_visible_through_local_transaction_id==std::numeric_limits<u64>::max()-2 &&
    begun.entry.begin_visible_through_commit_sequence==std::numeric_limits<u64>::max()-1 &&
    begun.inventory.next_local_transaction_id==std::numeric_limits<u64>::max(),
    "last representable BEGIN allocation retains both high watermarks without wrap");
  Check(!begin(begun.inventory,{UuidKind::transaction,Id(902)}).ok(),"exhausted BEGIN refuses counter reuse");
  Check(!begin(inventory,{UuidKind::object,Id(902)}).ok(),"BEGIN refuses wrong typed UUID kind");
  auto old_version=Id(902);old_version.bytes[6]=0x40;
  Check(!begin(inventory,{UuidKind::transaction,old_version}).ok(),"BEGIN refuses non-v7 system identity");
  inventory.next_local_transaction_id=0;
  Check(!begin(inventory,{UuidKind::transaction,Id(902)}).ok(),"zero allocation counter refuses");
  inventory.next_local_transaction_id=1;inventory.next_commit_sequence=0;
  Check(!begin(inventory,{UuidKind::transaction,Id(902)}).ok(),"zero commit counter refuses before subtraction");
 }
}
void BeginIsolation() {
 for (bool read_only : {false,true}) for (bool prior_commit : {false,true}) {
  const auto writer=mga::BeginLocalTransaction(mga::MakeEmptyLocalTransactionInventory(),{UuidKind::transaction,Id(910)},1000);
  Check(writer.ok(),"begin concurrent earlier-number writer");
  auto inventory=writer.inventory;
  std::optional<Entry> prior;
  if (prior_commit) {
    const auto begun=mga::BeginLocalTransaction(inventory,{UuidKind::transaction,Id(911)},1001);
    Check(begun.ok(),"begin preexisting committed writer");
    const auto committed=mga::CommitLocalTransaction(begun.inventory,begun.entry.identity.local_id,1002);
    Check(committed.ok(),"commit preexisting writer");
    inventory=committed.inventory;prior=committed.entry;
  }
  const auto reader=read_only ? mga::BeginLocalReadOnlyTransaction(inventory,{UuidKind::transaction,Id(912)},1003) :
    mga::BeginLocalTransaction(inventory,{UuidKind::transaction,Id(912)},1003);
  Check(reader.ok(),"begin reader while lower-number writer remains active");
  const auto committed=mga::CommitLocalTransaction(reader.inventory,writer.entry.identity.local_id,1004);
  Check(committed.ok(),"earlier-number writer commits after reader BEGIN");
  const auto snapshot=mga::CreateLocalTransactionSnapshot(committed.inventory,reader.entry.identity.local_id);
  Check(snapshot.ok() && snapshot.snapshot.transaction_start_visible_through_local_transaction.value==reader.entry.identity.local_id.value-1 &&
    snapshot.snapshot.transaction_start_visible_through_commit_sequence==(prior_commit ? 1u : 0u),
    "snapshot retains exact BEGIN allocation and commit boundaries after concurrent commit");
  const auto row=[](const Entry& entry) {
    mga::RowVersionMetadata value;
    value.identity.row.row_uuid={UuidKind::row,Id(920)};
    value.identity.creator_transaction=entry.identity;
    value.identity.version_uuid=Id(921);value.identity.version_sequence=1;
    value.state=mga::RowVersionState::committed;value.creator_transaction_state=State::committed;
    value.creator_commit_sequence=entry.commit_sequence;value.payload_present=true;
    return value;
  };
  for (const auto level : {mga::IsolationLevel::read_committed,mga::IsolationLevel::repeatable_read,mga::IsolationLevel::serializable}) {
    const auto policy=mga::SnapshotPolicyForIsolation(level,snapshot.snapshot);
    const auto visible=mga::EvaluateVisibility(row(committed.entry),policy);
    Check(visible.ok() && visible.decision==(level==mga::IsolationLevel::read_committed ?
      mga::VisibilityDecision::visible : mga::VisibilityDecision::invisible),
      "commit sequence keeps late lower-number commits out of repeatable-read and serializable snapshots");
    if (prior) {
      const auto existing=mga::EvaluateVisibility(row(*prior),policy);
      Check(existing.ok() && existing.decision==mga::VisibilityDecision::visible,
        "all supported isolation levels retain pre-BEGIN committed visibility");
    }
  }
 }
}
void StableSnapshotBoundary() {
  for (bool stable : {false, true}) {
    const auto writer = mga::BeginLocalTransaction(mga::MakeEmptyLocalTransactionInventory(),
        {UuidKind::transaction, Id(920)}, 2000);
    auto prior = mga::BeginLocalTransaction(writer.inventory, {UuidKind::transaction, Id(921)}, 2001);
    prior = mga::CommitLocalTransaction(prior.inventory, prior.entry.identity.local_id, 2002);
    auto reader = mga::BeginLocalTransaction(prior.inventory, {UuidKind::transaction, Id(922)}, 2003);
    Check(reader.ok(), "begin snapshot reader");
    reader.inventory.entries.back().stable_snapshot = stable;
    const auto committed = mga::CommitLocalTransaction(reader.inventory, writer.entry.identity.local_id, 2004);
    Check(committed.ok(), "commit earlier-number writer after reader begin");
    const auto horizons = mga::ComputeLocalTransactionHorizons(committed.inventory);
    Check(horizons.ok() && horizons.horizons.oldest_snapshot_transaction.value ==
          (stable ? writer.entry.identity.local_id.value : reader.entry.identity.local_id.value),
          "stable readers retain earlier-number late writers between statements");
    const auto snapshot = mga::PublishStatementStableSnapshotVector(committed.inventory,
        reader.entry.identity.local_id, 2005);
    Check(snapshot.ok(), "publish admitted transaction snapshot");
    const auto& exclusions = snapshot.descriptor.active_excluded_local_transaction_ids;
    Check((std::find(exclusions.begin(), exclusions.end(), writer.entry.identity.local_id.value) != exclusions.end()) == stable,
          "stable snapshots exclude late lower-number commits; read committed refreshes");
    Check(snapshot.descriptor.visible_committed_high_watermark >= prior.entry.identity.local_id.value,
          "prior committed row remains visible");
    mga::ReleasePublishedSnapshotVector(snapshot.descriptor.snapshot_uuid);
    Fixture f;
    f.before[0].inventory.entries[0].stable_snapshot = stable;
    f.after[0].inventory.entries[0].stable_snapshot = stable;
    f.Refresh();
    const auto decoded = page::DecodeNativeTransactionInventoryPage(f.old_images[0]);
    Check(decoded.ok() && decoded.page->inventory.entries[0].stable_snapshot == stable,
          "native inventory preserves admitted snapshot setting");
    const auto encoded = page::EncodeNativeTransactionInventoryPage(f.before[0]);
    Check(encoded.ok() && encoded.bytes == f.old_images[0], "stable flag encoder matches independent binary oracle");
    Check(f.CheckDelta().ok(), "unchanged snapshot setting admitted");
    f.after[0].inventory.entries[0].stable_snapshot = !stable;
    f.Refresh();
    Failed(f.CheckDelta());
  }
}
void Faults(){Fixture f(0,4);allocations=0;counting=true;const auto base=f.CheckDelta();counting=false;const auto sites=allocations;Check(base.ok()&&sites,"allocation baseline");for(unsigned long at=0;at<=sites;++at){allocation_budget=at;const auto result=f.CheckDelta();const auto left=allocation_budget;allocation_budget=-1;if(at==sites)Check(result.ok()&&left==0,"exact allocation success boundary");else{Check(result.error==E::resource_exhausted&&left==-1,"all required allocations consumed and classified");Failed(result);}}
 hash_seen=0;hash_counting=true;const auto hashed=f.CheckDelta();hash_counting=false;const auto hashes=hash_seen;Check(hashed.ok()&&hashes,"hash baseline");for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_target=at;hash_fault=mode;hash_seen=0;const auto result=f.CheckDelta();const bool consumed=!hash_fault;hash_fault=0;hash_active=false;Check(consumed&&result.error==E::hash_failure,"every digest failure consumed and classified");Failed(result);}std::cout<<"allocation sites="<<sites<<" hashes="<<hashes<<"\n";
}
}
int main(){try{ColdFaults();Profiles();Negatives();Differences();Boundaries();BeginCandidates();BeginCounterEdges();BeginIsolation();StableSnapshotBoundary();Faults();std::cout<<"native inventory publication delta checks="<<checks<<"\n";return 0;}catch(const std::exception& e){allocation_budget=-1;counting=false;hash_fault=0;std::cerr<<e.what()<<"\n";return 1;}}

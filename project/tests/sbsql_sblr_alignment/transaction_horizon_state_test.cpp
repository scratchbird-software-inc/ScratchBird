// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "transaction_horizon.hpp"
#include "transaction_cleanup_horizon_service.hpp"
#include "native_creation_workspace.hpp"
#include "transaction_inventory_page.hpp"
#include "disk_device.hpp"
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <source_location>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace scratchbird::core::platform;
namespace mga=scratchbird::transaction::mga;
namespace db=scratchbird::storage::database;
namespace page=scratchbird::storage::page;
namespace disk=scratchbird::storage::disk;
namespace fs=std::filesystem;
namespace {
unsigned checks=0,allocations=0;long allocation_budget=-1;bool count_allocations=false;
void Check(bool ok,const char* why,std::source_location at=std::source_location::current()){
  ++checks;if(!ok)throw std::runtime_error(std::string(why)+" line="+std::to_string(at.line()));
}
Uuid Id(unsigned n){Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[14]=n>>8;id.bytes[15]=n;return id;}
using State=mga::TransactionState;
struct Case {State state,origin;bool interesting,active;};
constexpr std::array cases{
  Case{State::created,State::none,true,false},Case{State::active,State::none,true,true},
  Case{State::read_only_active,State::none,true,true},Case{State::preparing,State::none,true,false},
  Case{State::prepared,State::none,true,false},Case{State::committing,State::none,true,false},
  Case{State::committed,State::none,false,false},Case{State::rolling_back,State::none,true,false},
  Case{State::rolled_back,State::none,false,false},Case{State::limbo,State::none,true,false},
  Case{State::recovering,State::none,true,false},Case{State::failed_terminal,State::none,true,false},
  Case{State::archived,State::committed,false,false},Case{State::archived,State::rolled_back,false,false},
  Case{State::archived,State::failed_terminal,true,false}};
mga::LocalTransactionInventory Inventory(const Case& c){
  mga::LocalTransactionInventory inv;inv.next_local_transaction_id=11;inv.next_commit_sequence=3;
  mga::TransactionInventoryEntry e;e.identity.transaction_uuid={UuidKind::transaction,Id(10)};
  e.identity.local_id=mga::MakeLocalTransactionId(7);e.identity.scope=mga::TransactionScope::local_node;
  e.state=c.state;e.archived_from_state=c.origin;e.begin_unix_epoch_millis=100;e.begin_visible_through_local_transaction_id=6;
  if(c.state==State::committed||c.origin==State::committed)e.commit_sequence=2;
  if(c.state==State::committed||c.state==State::rolled_back||c.state==State::failed_terminal||c.state==State::archived)e.final_unix_epoch_millis=200;
  e.evidence_record_written=true;inv.entries.push_back(e);return inv;
}
void Empty(const mga::TransactionHorizonResult& r){Check(!r.ok()&&!r.status.ok()&&!r.horizons.valid&&
  !r.horizons.next_transaction_id.value&&!r.horizons.oldest_interesting_transaction.value&&
  !r.horizons.oldest_active_transaction.value&&!r.horizons.oldest_snapshot_transaction.value,"no invalid horizon prefix");}
void Expected(const mga::LocalTransactionHorizons& h,u64 oit,u64 oat,u64 ost,u64 next=11){
  Check(h.valid&&h.oldest_interesting_transaction.value==oit&&h.oldest_active_transaction.value==oat&&
    h.oldest_snapshot_transaction.value==ost&&h.next_transaction_id.value==next,"exact horizon state table");
}
void Projection(){
  for(const auto& c:cases)for(auto scope:{mga::TransactionScope::local_node,mga::TransactionScope::cluster_global})for(unsigned flags=0;flags<8;++flags){
    auto inv=Inventory(c);auto& e=inv.entries.front();e.identity.scope=scope;
    e.evidence_record_required=flags&1;e.evidence_record_written=flags&2;e.rollback_only=flags&4;
    const auto r=mga::ComputeLocalTransactionHorizons(inv);Check(r.ok(),"valid lifecycle projection");
    Expected(r.horizons,c.interesting?7:11,c.active?7:11,c.active?7:11);
    mga::LocalTransactionHorizonRequest request{inv,{mga::MakeLocalTransactionId(8),mga::MakeLocalTransactionId(3)}};
    const auto snapshot=mga::ComputeLocalTransactionHorizons(request);Check(snapshot.ok(),"explicit snapshot projection");
    Expected(snapshot.horizons,c.interesting?7:11,c.active?7:11,3);
    request.active_snapshot_horizons={mga::MakeLocalTransactionId(11)};
    Check(mga::ComputeLocalTransactionHorizons(request).horizons.oldest_snapshot_transaction.value==11,"explicit next boundary is representable");
    mga::AuthoritativeCleanupHorizonRequest cleanup;cleanup.inventory=inv;cleanup.inventory_authoritative=cleanup.inventory_complete=true;
    const auto held=mga::ComputeAuthoritativeCleanupHorizon(cleanup);
    Check(held.ok()&&held.cleanup_horizon.value==(c.interesting?7u:11u),"cleanup preserves inventory hold");
    Check(held.blockers.size()==(c.interesting?1u:0u),"cleanup blocker set agrees with horizon");
    if(c.interesting)Check(held.blockers.front().kind==(c.active?mga::CleanupHorizonBlockerKind::active_transaction:mga::CleanupHorizonBlockerKind::unresolved_outcome)&&
      held.blockers.front().local_transaction_id.value==7,"unresolved is not active accounting");
  }
  const auto active=Inventory(cases[1]);
  for(unsigned raw=0;raw<=65535;++raw){auto inv=active;auto& e=inv.entries.front();e.state=static_cast<State>(raw);
    const auto known=std::find_if(cases.begin(),cases.end(),[&](const auto& c){return c.state==e.state&&c.origin==State::none;});
    if(known!=cases.end())inv=Inventory(*known);
    const auto r=mga::ComputeLocalTransactionHorizons(inv);
    if(known==cases.end())Empty(r);else {Check(r.ok(),"admitted raw lifecycle value");Expected(r.horizons,known->interesting?7:11,known->active?7:11,known->active?7:11);}
    inv=active;inv.entries.front().state=State::archived;inv.entries.front().archived_from_state=static_cast<State>(raw);
    const auto archived=std::find_if(cases.begin(),cases.end(),[&](const auto& c){return c.state==State::archived&&c.origin==static_cast<State>(raw);});
    if(archived!=cases.end())inv=Inventory(*archived);
    const auto a=mga::ComputeLocalTransactionHorizons(inv);
    if(archived==cases.end())Empty(a);else {Check(a.ok(),"admitted raw archived origin");Expected(a.horizons,archived->interesting?7:11,11,11);}
  }
  for(unsigned fault=0;fault<22;++fault){auto inv=active;auto& e=inv.entries.front();
    if(fault==0)inv.next_local_transaction_id=0;if(fault==1)inv.next_commit_sequence=0;
    if(fault==2)e.identity.local_id.value=0;if(fault==3)e.identity.local_id.value=11;
    if(fault==4)e.identity.transaction_uuid.value={};if(fault==5)e.identity.transaction_uuid.value.bytes[6]=0x40;
    if(fault==6)e.identity.transaction_uuid.kind=UuidKind::object;if(fault==7)e.identity.scope=static_cast<mga::TransactionScope>(99);
    if(fault==8)e.begin_visible_through_commit_sequence=3;if(fault==9)e.commit_sequence=1;
    if(fault==10){e.state=State::committed;e.commit_sequence=0;}
    if(fault==11){e.state=State::committed;e.commit_sequence=3;}
    if(fault==12){e.state=State::committed;e.commit_sequence=e.begin_visible_through_commit_sequence=1;}
    if(fault==13)e.archived_from_state=State::committed;
    if(fault==14)inv.entries.push_back(e);
    if(fault==15){auto other=e;other.identity.local_id.value=8;inv.entries.push_back(other);}
    if(fault==16){e.state=State::committed;e.commit_sequence=1;auto other=e;other.identity.local_id.value=8;other.identity.transaction_uuid.value=Id(11);inv.entries.push_back(other);}
    if(fault==17){e.state=State::archived;e.archived_from_state=State::active;}
    if(fault==18){e.state=State::archived;e.archived_from_state=State::failed_terminal;e.commit_sequence=1;}
    if(fault==19){e.state=State::archived;e.archived_from_state=State::committed;}
    if(fault==20)e.state=State::none;
    if(fault==21){auto other=e;other.identity.local_id.value=8;other.identity.transaction_uuid.value=Id(11);other.state=static_cast<State>(65535);inv.entries.push_back(other);}
    Empty(mga::ComputeLocalTransactionHorizons(inv));
  }
  for(auto n:{u64{0},u64{12},std::numeric_limits<u64>::max()}){
    mga::LocalTransactionHorizonRequest request{active,{mga::MakeLocalTransactionId(1),mga::MakeLocalTransactionId(n)}};
    Empty(mga::ComputeLocalTransactionHorizons(request));
  }
  auto unordered=active;auto second=unordered.entries.front();second.identity.local_id.value=2;second.identity.transaction_uuid.value=Id(12);second.begin_visible_through_local_transaction_id=1;
  unordered.entries.push_back(second);Expected(mga::ComputeLocalTransactionHorizons(unordered).horizons,2,2,2);
  std::reverse(unordered.entries.begin(),unordered.entries.end());Expected(mga::ComputeLocalTransactionHorizons(unordered).horizons,2,2,2);
  auto maximum=active;maximum.next_local_transaction_id=std::numeric_limits<u64>::max();maximum.entries.front().identity.local_id.value=maximum.next_local_transaction_id-1;
  Expected(mga::ComputeLocalTransactionHorizons(maximum).horizons,maximum.next_local_transaction_id-1,maximum.next_local_transaction_id-1,maximum.next_local_transaction_id-1,maximum.next_local_transaction_id);
  Expected(mga::ComputeLocalTransactionHorizons(mga::MakeEmptyLocalTransactionInventory()).horizons,1,1,1,1);
  for(bool invalid:{false,true}){auto inv=active;if(invalid)inv.entries.front().state=static_cast<State>(65535);
    allocations=0;count_allocations=true;auto baseline=mga::ComputeLocalTransactionHorizons(inv);count_allocations=false;const unsigned count=allocations;
    for(unsigned n=0;n<=count;++n){allocation_budget=n;bool threw=false;
      try{const auto result=mga::ComputeLocalTransactionHorizons(inv);allocation_budget=-1;if(invalid)Empty(result);else Check(result.ok(),"allocation success projection");}
      catch(const std::bad_alloc&){allocation_budget=-1;threw=true;}
      Check(threw==(n<count),"all projection allocation failure sites propagate without result");
    }
    std::cout<<"horizon_allocation_sites invalid="<<invalid<<" count="<<count<<'\n';
  }
}
}
void* operator new(std::size_t n){if(count_allocations)++allocations;if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}if(allocation_budget>0)--allocation_budget;
  if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
int main(int argc,char** argv)try{
  // W4d (2026-11-08): NativeImages (horizon summaries in the legacy SBTINV01
  // page, built on the transitional create, and its fresh-process probe) was
  // deleted with that format's writer; the in-place inventory's horizons are
  // covered by the transaction inventory writer and open verification tests.
  Projection();std::cout<<"PASS horizon state checks="<<checks<<'\n';return 0;
}catch(const std::exception& e){allocation_budget=-1;count_allocations=false;std::cerr<<"FAIL horizon state checks="<<checks<<" "<<e.what()<<'\n';return 1;}

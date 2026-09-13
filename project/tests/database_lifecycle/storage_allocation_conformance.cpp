// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_allocation_lifecycle.hpp"
#include "uuid.hpp"
#include "../common/single_tu_allocation_fault.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;
std::size_t check_count=0;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  ++check_count;
  if (!condition) {
    Fail(message);
  }
}

u64 NextMillis() {
  static u64 next = 1779501900000ull;
  return ++next;
}

TypedUuid NewUuid(UuidKind kind) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NextMillis());
  Require(generated.ok(), "DBLC-013AF UUID generation failed");
  return generated.value;
}

page::PageAllocationRequest BaseRequest(const page::PageAllocationLedger& ledger) {
  page::PageAllocationRequest request;
  request.database_uuid = ledger.database_uuid;
  request.filespace_uuid = ledger.filespace_uuid;
  request.owner_object_uuid = NewUuid(UuidKind::object);
  request.creator_transaction_uuid = NewUuid(UuidKind::transaction);
  request.creator_local_transaction_id = 10;
  request.page_family = "data";
  request.page_count = 4;
  request.engine_authoritative = true;
  return request;
}

page::PageAllocationLedger NewLedger() {
  page::PageAllocationLedger ledger;
  ledger.database_uuid = NewUuid(UuidKind::database);
  ledger.filespace_uuid = NewUuid(UuidKind::filespace);
  ledger.free_extents.push_back({100, 16});
  return ledger;
}

void TestAllocationAndMGAReuse() {
  auto ledger = NewLedger();
  const auto allocated = page::ReservePageAllocation(&ledger, BaseRequest(ledger));
  Require(allocated.ok(), "DBLC-013AF allocation was refused");
  Require(allocated.allocation.start_page == 100, "DBLC-013AF allocation start mismatch");
  Require(allocated.allocation.page_count == 4, "DBLC-013AF allocation count mismatch");
  Require(ledger.free_extents.size() == 1 && ledger.free_extents.front().start_page == 104,
          "DBLC-013AF free map did not consume extent");

  page::PageReleaseRequest release;
  release.allocation_uuid = allocated.allocation.allocation_uuid;
  release.cleanup_horizon_local_transaction_id = 11;
  release.engine_mga_authoritative = true;
  const auto reusable = page::MarkPageAllocationReusable(&ledger, release);
  Require(reusable.ok() && reusable.changed,
          "DBLC-013AF reusable-pending transition failed");
  Require(reusable.allocation.state == page::PageAllocationLifecycleState::reusable_pending_mga,
          "DBLC-013AF reusable-pending state mismatch");

  release.cleanup_horizon_local_transaction_id = 10;
  const auto blocked = page::ReclaimReusablePageAllocation(&ledger, release);
  Require(!blocked.ok(), "DBLC-013AF reclaim before MGA horizon was accepted");
  Require(blocked.diagnostic.diagnostic_code ==
              "SB-STORAGE-PAGE-ALLOCATION-BLOCKED-BY-MGA-HORIZON",
          "DBLC-013AF wrong MGA horizon diagnostic");

  release.cleanup_horizon_local_transaction_id = 20;
  const auto reclaimed = page::ReclaimReusablePageAllocation(&ledger, release);
  Require(reclaimed.ok() && reclaimed.changed, "DBLC-013AF reclaim after MGA horizon failed");
  Require(reclaimed.allocation.state == page::PageAllocationLifecycleState::reusable_free,
          "DBLC-013AF reclaim state mismatch");
  Require(!ledger.free_extents.empty(), "DBLC-013AF reclaimed extent missing from free map");
}

void TestRefusalsAndCompaction() {
  auto ledger = NewLedger();
  auto request = BaseRequest(ledger);
  request.engine_authoritative = false;
  const auto external = page::ReservePageAllocation(&ledger, request);
  Require(!external.ok(), "DBLC-013AF non-engine allocation was accepted");
  Require(external.diagnostic.diagnostic_code ==
              "SB-STORAGE-PAGE-ALLOCATION-NOT-ENGINE-AUTHORITATIVE",
          "DBLC-013AF non-engine diagnostic mismatch");

  request = BaseRequest(ledger);
  request.cluster_route_requested = true;
  const auto cluster = page::ReservePageAllocation(&ledger, request);
  Require(!cluster.ok(), "DBLC-013AF cluster allocation route was accepted");
  Require(cluster.diagnostic.diagnostic_code ==
              "SB-STORAGE-PAGE-ALLOCATION-CLUSTER-ROUTE-UNAVAILABLE",
          "DBLC-013AF cluster diagnostic mismatch");

  request = BaseRequest(ledger);
  request.page_count = 32;
  const auto too_large = page::ReservePageAllocation(&ledger, request);
  Require(!too_large.ok(), "DBLC-013AF over-allocation was accepted");
  Require(too_large.diagnostic.diagnostic_code ==
              "SB-STORAGE-PAGE-ALLOCATION-INSUFFICIENT-FREE-SPACE",
          "DBLC-013AF over-allocation diagnostic mismatch");

  const auto allocated = page::ReservePageAllocation(&ledger, BaseRequest(ledger));
  Require(allocated.ok(), "DBLC-013AF allocation for compaction failed");
  page::PageReleaseRequest release;
  release.allocation_uuid = allocated.allocation.allocation_uuid;
  release.cleanup_horizon_local_transaction_id = 20;
  release.engine_mga_authoritative = true;
  Require(page::MarkPageAllocationReusable(&ledger, release).ok(),
          "DBLC-013AF reusable-pending for compaction failed");

  page::PageCompactionRequest compact;
  compact.engine_authoritative = true;
  compact.shutdown_or_maintenance_fenced = true;
  const auto blocked_compaction = page::CompactPageFreeSpace(&ledger, compact);
  Require(!blocked_compaction.ok(), "DBLC-013AF compaction ignored pending MGA pages");
  Require(blocked_compaction.diagnostic.diagnostic_code ==
              "SB-STORAGE-PAGE-ALLOCATION-COMPACTION-BLOCKED-BY-MGA",
          "DBLC-013AF compaction diagnostic mismatch");
}

void TestRecoveryClassification() {
  auto ledger = NewLedger();
  const auto allocated = page::ReservePageAllocation(&ledger, BaseRequest(ledger));
  Require(allocated.ok(), "DBLC-013AF allocation for recovery failed");

  page::PageAllocationEntry incomplete = allocated.allocation;
  incomplete.allocation_uuid = NewUuid(UuidKind::object);
  incomplete.state = page::PageAllocationLifecycleState::reserved;
  ledger.allocations.push_back(incomplete);

  const auto recovery = page::ClassifyPageAllocationLedgerForRecovery(ledger);
  Require(recovery.ok(), "DBLC-013AF recovery classification failed");
  bool retained_allocated = false;
  bool failed_closed_reserved = false;
  for (const auto& classification : recovery.classifications) {
    retained_allocated = retained_allocated ||
                         classification.action == page::PageAllocationRecoveryAction::retain;
    failed_closed_reserved = failed_closed_reserved ||
                             classification.fail_closed;
  }
  Require(retained_allocated, "DBLC-013AF recovery did not retain allocated pages");
  Require(failed_closed_reserved, "DBLC-013AF recovery did not fail closed reserved pages");
}

void TestDurabilityOrderingRefusesNonDurableGeneration() {
  auto ledger = NewLedger();
  auto request = BaseRequest(ledger);
  request.durability_fence_satisfied = false;
  const auto non_durable = page::ReservePageAllocation(&ledger, request);
  Require(!non_durable.ok(), "DBLC-013AF non-durable allocation generation was accepted");
  Require(non_durable.diagnostic.diagnostic_code ==
              "SB-STORAGE-PAGE-ALLOCATION-DURABILITY-FENCE-REQUIRED",
          "DBLC-013AF non-durable allocation diagnostic mismatch");

  request = BaseRequest(ledger);
  const auto allocated = page::ReservePageAllocation(&ledger, request);
  Require(allocated.ok(), "DBLC-013AF durable allocation was refused");
  auto corrupt = allocated.allocation;
  corrupt.allocation_uuid = NewUuid(UuidKind::object);
  corrupt.published_page_generation = corrupt.durable_page_generation + 1;
  corrupt.durability_fence_satisfied = false;
  ledger.allocations.push_back(corrupt);

  const auto recovery = page::ClassifyPageAllocationLedgerForRecovery(ledger);
  Require(recovery.ok(), "DBLC-013AF durability recovery classification failed");
  bool failed_closed_non_durable = false;
  for (const auto& classification : recovery.classifications) {
    failed_closed_non_durable = failed_closed_non_durable ||
                                (classification.fail_closed &&
                                 classification.stable_reason ==
                                     "allocation references non-durable page generation");
  }
  Require(failed_closed_non_durable,
          "DBLC-013AF recovery did not fail closed non-durable page generation");
}

page::PageAllocationEntry RecoveryEntry() {
  page::PageAllocationEntry entry;
  entry.allocation_uuid=NewUuid(UuidKind::object);entry.database_uuid=NewUuid(UuidKind::database);
  entry.filespace_uuid=NewUuid(UuidKind::filespace);entry.creator_transaction_uuid=NewUuid(UuidKind::transaction);
  entry.owner_object_uuid=NewUuid(UuidKind::object);entry.policy_uuid=NewUuid(UuidKind::object);entry.capacity_evidence_uuid=NewUuid(UuidKind::object);
  entry.creator_local_transaction_id=10;entry.start_page=100;entry.page_count=4;
  entry.durable_page_generation=entry.published_page_generation=7;entry.durability_fence_satisfied=true;
  entry.page_family="data";entry.state=page::PageAllocationLifecycleState::allocated;
  entry.reusable_after_local_transaction_id=10;return entry;
}
void TestRecoveryIdentityAndStateClosure() {
  using S=page::PageAllocationLifecycleState;using A=page::PageAllocationRecoveryAction;
  const auto valid=RecoveryEntry();
  const auto refused=[](const auto& entry){const auto result=page::ClassifyPageAllocationForRecovery(entry);
    Require(result.action==A::fail_closed&&result.fail_closed&&!result.stable_reason.empty(),"malformed allocation recovery must fail closed with reason");
    Require(result.allocation_uuid.value==entry.allocation_uuid.value&&result.observed_state==entry.state,"recovery never replaces malformed input identity/state");};
  for(unsigned state=8;state<65536;++state){auto entry=valid;entry.state=static_cast<S>(state);refused(entry);}
  auto entry=valid;entry.state=static_cast<S>(std::numeric_limits<unsigned>::max());refused(entry);
  for(unsigned state=0;state<8;++state){entry=valid;entry.state=static_cast<S>(state);const auto result=page::ClassifyPageAllocationForRecovery(entry);
    const A expected=state==0||state==4?A::release_to_free_map:state==1||state==5?A::fail_closed:state==6?A::quarantine:A::retain;
    Require(result.action==expected&&result.fail_closed==(state==1||state==5||state==6),"closed allocation state/action mapping");}
  for(auto member:{&page::PageAllocationEntry::allocation_uuid,&page::PageAllocationEntry::database_uuid,&page::PageAllocationEntry::filespace_uuid,&page::PageAllocationEntry::creator_transaction_uuid}){
    entry=valid;entry.*member={};refused(entry);entry=valid;(entry.*member).kind=UuidKind::unknown;refused(entry);
    entry=valid;(entry.*member).value.bytes[6]=0x41;refused(entry);entry=valid;(entry.*member).value.bytes[8]=0;refused(entry);}
  for(auto member:{&page::PageAllocationEntry::owner_object_uuid,&page::PageAllocationEntry::policy_uuid,&page::PageAllocationEntry::capacity_evidence_uuid}){
    entry=valid;(entry.*member).kind=UuidKind::unknown;refused(entry);entry=valid;(entry.*member).kind=UuidKind::transaction;refused(entry);
    entry=valid;(entry.*member).value.bytes[6]=0x41;refused(entry);entry=valid;entry.*member={};Require(!page::ClassifyPageAllocationForRecovery(entry).fail_closed,"fully absent optional identity accepted");
    (entry.*member).kind=UuidKind::object;Require(!page::ClassifyPageAllocationForRecovery(entry).fail_closed,"typed nil optional identity accepted");
    (entry.*member).kind=UuidKind::transaction;refused(entry);}
  for(unsigned fault=0;fault<9;++fault){entry=valid;
    if(fault==0)entry.creator_local_transaction_id=0;if(fault==1)entry.start_page=0;if(fault==2)entry.page_count=0;
    if(fault==3)entry.start_page=std::numeric_limits<u64>::max()-2;if(fault==4)entry.page_family="not_a_page_family";
    if(fault==5){entry.state=S::preallocated;entry.policy_uuid={};}
    if(fault==6){entry.state=S::reusable_pending_mga;entry.reusable_after_local_transaction_id=0;}
    if(fault==7){entry.state=S::reusable_free;entry.reusable_after_local_transaction_id=0;}
    if(fault==8)entry.durability_fence_satisfied=false;refused(entry);}
  entry=valid;entry.start_page=std::numeric_limits<u64>::max()-entry.page_count;Require(!page::ClassifyPageAllocationForRecovery(entry).fail_closed,"exact representable exclusive extent end accepted");
}
void TestRecoveryLedgerOwnershipAndReuse() {
  using S=page::PageAllocationLifecycleState;using A=page::PageAllocationRecoveryAction;
  auto entry=RecoveryEntry();page::PageAllocationLedger ledger;ledger.database_uuid=entry.database_uuid;ledger.filespace_uuid=entry.filespace_uuid;ledger.allocations={entry};
  auto classify=[&]{const auto r=page::ClassifyPageAllocationLedgerForRecovery(ledger);Require(r.ok()&&r.classifications.size()==ledger.allocations.size(),"complete ledger projection preserves input order");return r;};
  for(bool database:{false,true}){auto bad=ledger;if(database)bad.database_uuid={};else bad.filespace_uuid={};const auto r=page::ClassifyPageAllocationLedgerForRecovery(bad);
    Require(!r.ok()&&r.classifications.empty(),"malformed ledger identity exposes no prefix");}
  ledger.allocations[0].database_uuid=NewUuid(UuidKind::database);Require(classify().classifications[0].fail_closed,"foreign database allocation refused");ledger.allocations={entry};
  ledger.allocations[0].filespace_uuid=NewUuid(UuidKind::filespace);Require(classify().classifications[0].fail_closed,"foreign filespace allocation refused");ledger.allocations={entry,entry};
  auto r=classify();Require(r.classifications[0].fail_closed&&r.classifications[1].fail_closed,"both duplicate allocation identities refused");
  auto old=entry;old.state=S::reusable_free;auto current=entry;current.allocation_uuid=NewUuid(UuidKind::object);
  for(unsigned state=1;state<8;++state){if(state==4)continue;current.state=static_cast<S>(state);
    for(u64 start:{97,100,103}){current.start_page=start;ledger.allocations={old,current};r=classify();
      Require(r.classifications[0].action==A::retain&&!r.classifications[0].fail_closed,"old released history cannot free a partially or fully overlapping current owner");}}
  current.state=S::allocated;current.start_page=104;ledger.allocations={old,current};r=classify();Require(r.classifications[0].action==A::release_to_free_map,"adjacent non-overlap does not hide free history");
  auto wide=current,narrow=current;wide.allocation_uuid=NewUuid(UuidKind::object);wide.start_page=90;wide.page_count=40;
  narrow.allocation_uuid=NewUuid(UuidKind::object);narrow.start_page=100;narrow.page_count=2;
  old.start_page=115;old.page_count=2;ledger.allocations={old,narrow,wide};r=classify();
  Require(r.classifications[0].action==A::retain,"nested retained ranges require maximum-end prefix, not just nearest range");
  ledger.allocations[0].state=S::free;r=classify();Require(r.classifications[0].action==A::retain,"free history also cannot release current ownership");
  current.start_page=std::numeric_limits<u64>::max();ledger.allocations={old,current};r=classify();Require(r.classifications[0].fail_closed&&r.classifications[1].fail_closed,"malformed owner extent prevents unsafe free-map suggestions");
  Require(ledger.allocations[0].state==S::reusable_free&&ledger.allocations[1].start_page==std::numeric_limits<u64>::max(),"classification does not mutate source ledger");
  ledger.allocations={old,narrow,wide};bool completed=false;std::size_t failures=0;
  for(std::ptrdiff_t budget=0;budget<10000;++budget){allocations_before_failure=budget;allocation_attempts=0;
    try {const auto actual=page::ClassifyPageAllocationLedgerForRecovery(ledger);allocations_before_failure=-1;
      Require(actual.ok()&&actual.classifications.size()==3&&actual.classifications[0].action==A::retain,"allocation-fault sweep reaches complete correct projection");completed=true;break;
    }catch(const std::bad_alloc&){allocations_before_failure=-1;++failures;}
    Require(ledger.allocations.size()==3&&ledger.allocations[0].allocation_uuid.value==old.allocation_uuid.value&&ledger.allocations[0].state==S::reusable_free
      &&ledger.allocations[1].start_page==100&&ledger.allocations[2].page_count==40,"every allocation failure preserves ledger ownership and ranges");
  }
  Require(completed&&failures>0,"every fallible classification allocation tested through success");
}
void TestAllocationSelectorKinds() {
  auto ledger=NewLedger();auto request=BaseRequest(ledger);request.owner_object_uuid.kind=UuidKind::unknown;
  const auto invalid=page::ReservePageAllocation(&ledger,request);
  Require(!invalid.ok()&&ledger.allocations.empty()&&ledger.free_extents[0].start_page==100,"non-nil unknown-kind optional owner is not treated as absent");
  const auto allocated=page::ReservePageAllocation(&ledger,BaseRequest(ledger));Require(allocated.ok(),"selector fixture allocation");
  auto selector=allocated.allocation.allocation_uuid;selector.kind=UuidKind::transaction;
  Require(page::FindPageAllocation(ledger,selector)==nullptr,"read lookup refuses same UUID bytes with wrong selector kind");
  page::PageReleaseRequest release;release.allocation_uuid=selector;release.cleanup_horizon_local_transaction_id=20;release.engine_mga_authoritative=true;
  Require(!page::MarkPageAllocationReusable(&ledger,release).ok()&&ledger.allocations[0].state==page::PageAllocationLifecycleState::allocated,"mutable lookup cannot release through wrong selector kind");
  ledger.allocations[0].allocation_uuid.kind=UuidKind::transaction;
  selector=allocated.allocation.allocation_uuid;
  Require(page::FindPageAllocation(ledger,selector)==nullptr,"valid selector cannot match malformed stored allocation kind");
  release.allocation_uuid=selector;
  Require(!page::MarkPageAllocationReusable(&ledger,release).ok()&&ledger.allocations[0].state==page::PageAllocationLifecycleState::allocated,"mutable lookup cannot release a malformed stored allocation identity");
}
}  // namespace

int main() {
  TestRecoveryIdentityAndStateClosure();
  TestRecoveryLedgerOwnershipAndReuse();
  TestAllocationSelectorKinds();
  TestAllocationAndMGAReuse();
  TestRefusalsAndCompaction();
  TestRecoveryClassification();
  TestDurabilityOrderingRefusesNonDurableGeneration();
  std::cout<<"PASS checks="<<check_count<<" allocation_ledger_projection_only=true\n";
  return EXIT_SUCCESS;
}

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// Included after the actual selected-source fixtures; no mocked storage lease.
void DirectoryGuardedVisibilityObservation(unsigned profile){
 using D=mga::VisibilityDecision;using R=mga::RowVersionState;using T=mga::TransactionState;
 constexpr auto U=D::unknown,V=D::visible,I=D::invisible,W=D::wait_for_transaction,Q=D::requires_recovery;
 // Independent state/outcome table: rows are native row states, columns are
 // every defined creator state in its public enum order. Non-own delete effect.
 constexpr std::array<std::array<D,14>,8> expected{{
   {U,U,U,U,U,U,U,U,U,U,U,U,U,U},
   {U,W,W,W,W,W,W,W,W,W,W,W,U,W},
   {U,W,W,W,W,W,W,W,W,W,W,W,U,W},
   {U,W,W,W,W,W,V,W,W,W,W,W,U,W},
   {U,I,I,I,I,I,I,I,I,I,I,I,U,I},
   {U,W,W,W,W,W,V,W,I,Q,Q,I,U,W},
   {U,Q,Q,Q,Q,Q,Q,Q,Q,Q,Q,Q,U,Q},
   {U,Q,Q,Q,Q,Q,Q,Q,Q,Q,Q,Q,U,Q}
 }};
 mga::RowVersionMetadata base;
 base.identity={{{UuidKind::row,Id(63200)}},{{7},{UuidKind::transaction,Id(63201)},mga::TransactionScope::local_node},3,Id(63202)};
 base.state=R::committed;base.creator_transaction_state=T::committed;base.creator_commit_sequence=9;base.payload_present=true;
 struct Case {mga::RowVersionMetadata metadata;mga::VisibilitySnapshot snapshot;bool effect;D decision;mga::VisibilityResult owning;};
 std::vector<Case> cases;
 for(unsigned row=0;row<8;++row)for(unsigned tx=0;tx<14;++tx)for(unsigned own=0;own<2;++own)
 for(unsigned effect=0;effect<2;++effect)for(unsigned limit=0;limit<13;++limit){
  Case c;c.metadata=base;c.metadata.state=static_cast<R>(row);c.metadata.creator_transaction_state=static_cast<T>(tx);
  c.snapshot.reader_transaction={own?7u:8u};c.effect=effect;c.decision=expected[row][tx];
  const bool valid=row&&tx&&tx!=12;
  if(valid&&row==5&&!effect)c.decision=I;
  if(valid&&own&&(row==1||row==2||(row==5&&effect&&(tx==1||tx==2||tx==3||tx==4||tx==5||tx==7||tx==13))))c.decision=V;
  switch(limit){
   case 1:c.snapshot.visible_through_local_transaction_id=5;c.snapshot.visible_through_local_transaction_id_is_boundary=true;break;
   case 2:c.snapshot.visible_through_local_transaction_id=7;c.snapshot.visible_through_local_transaction_id_is_boundary=true;break;
   case 3:c.snapshot.active_excluded_local_transaction_ids={7};break;
   case 4:c.snapshot.in_doubt_excluded_local_transaction_ids={7};break;
   case 5:c.snapshot.visible_through_commit_sequence=3;c.snapshot.visible_through_commit_sequence_is_boundary=true;break;
   case 6:c.snapshot.visible_through_commit_sequence=9;c.snapshot.visible_through_commit_sequence_is_boundary=true;break;
   case 7:c.metadata.creator_commit_sequence=0;c.snapshot.visible_through_commit_sequence=9;c.snapshot.visible_through_commit_sequence_is_boundary=true;break;
   case 8:c.snapshot.visible_through_local_transaction_id_is_boundary=true;break;
   case 9:c.snapshot.visible_through_commit_sequence_is_boundary=true;break;
   case 10:c.snapshot.visible_through_commit_sequence=std::numeric_limits<u64>::max();c.snapshot.visible_through_commit_sequence_is_boundary=true;
     c.snapshot.visible_through_local_transaction_id=std::numeric_limits<u64>::max();break;
   case 11:c.snapshot.visible_through_local_transaction_id=5;break;
   case 12:c.snapshot.active_excluded_local_transaction_ids={9,7,1};break;
  }
  if(tx==6&&(row==3||(row==5&&effect))){
   if(limit==1||limit==3||limit==4||limit==5||limit==8||limit==9||limit==11||limit==12)c.decision=I;
   if(limit==7)c.decision=U;
  }
  c.owning=effect?mga::EvaluateVersionEffectVisibility(c.metadata,c.snapshot):mga::EvaluateVisibility(c.metadata,c.snapshot);
  Check(c.owning.decision==c.decision,"owning visibility satisfies independent complete state/boundary matrix");
  cases.push_back(std::move(c));
 }
 struct Invalid {mga::RowVersionMetadata metadata;const char* code;const char* key;const char* origin;};
 std::vector<Invalid> invalid;
 const auto add=[&](auto mutation,const char* code,const char* key,bool transaction=false){
   auto m=base;mutation(m);invalid.push_back({m,code,key,transaction?"transaction.mga.state":"transaction.mga.row_version"});};
 add([](auto& m){m.identity.version_uuid={};},"CATALOG.INVALID_INPUT","row_version.invalid_version_uuid");
 add([](auto& m){m.identity.version_uuid=m.identity.row.row_uuid.value;},"CATALOG.INVALID_INPUT","row_version.invalid_version_uuid");
 add([](auto& m){m.identity.row.row_uuid.kind=UuidKind::object;},"SB-ROW-INVALID-ROW-UUID-KIND","row_version.invalid_row_uuid_kind");
 add([](auto& m){m.identity.row.row_uuid.value.bytes[6]=0x40;},"SB-ROW-ROW-UUID-MUST-BE-V7","row_version.row_uuid_must_be_v7");
 add([](auto& m){m.identity.creator_transaction.local_id={};},"SB-TXN-INVALID-LOCAL-TRANSACTION-ID","transaction.invalid_local_transaction_id",true);
 add([](auto& m){m.identity.creator_transaction.transaction_uuid.kind=UuidKind::object;},"SB-TXN-INVALID-TRANSACTION-UUID-KIND","transaction.invalid_transaction_uuid_kind",true);
 add([](auto& m){m.identity.creator_transaction.transaction_uuid.value.bytes[6]=0x40;},"SB-TXN-UUID-MUST-BE-V7","transaction.transaction_uuid_must_be_v7",true);
 add([](auto& m){m.identity.creator_transaction.scope=mga::TransactionScope::unknown;},"SB-TXN-UNKNOWN-TRANSACTION-SCOPE","transaction.unknown_transaction_scope",true);
 add([](auto& m){m.identity.version_sequence=0;},"SB-ROW-INVALID-VERSION-SEQUENCE","row_version.invalid_version_sequence");
 add([](auto& m){m.state=R::unknown;},"SB-ROW-UNKNOWN-VERSION-STATE","row_version.unknown_version_state");
 add([](auto& m){m.creator_transaction_state=T::none;},"SB-ROW-UNKNOWN-CREATOR-TRANSACTION-STATE","row_version.unknown_creator_transaction_state");
 add([](auto& m){m.creator_transaction_state=T::archived;},"SB-ROW-UNKNOWN-CREATOR-TRANSACTION-STATE","row_version.unknown_creator_transaction_state");
 add([](auto& m){m.payload_present=false;},"SB-ROW-MISSING-VERSION-PAYLOAD","row_version.missing_version_payload");
 add([](auto& m){m.chain.previous_version_sequence=3;},"SB-ROW-INVALID-PREVIOUS-VERSION-LINK","row_version.invalid_previous_version_link");
 add([](auto& m){m.chain.next_version_sequence=3;},"SB-ROW-INVALID-NEXT-VERSION-LINK","row_version.invalid_next_version_link");
 for(const auto& c:invalid){const auto r=mga::ValidateRowVersionMetadata(c.metadata);
  Check(!r.ok()&&r.diagnostic.diagnostic_code==c.code&&r.diagnostic.message_key==c.key&&r.diagnostic.source_component==c.origin,
    "invalid metadata retains precise message vector and originating subsystem");}
 DirectoryHistoryFixture f(profile,(profile+1)%5,false,2,false,false,true,false);
 checkpoint_inventory_memory::Grant source_memory(32*1024*1024);
 {
  auto source=db::AcquireNativeSelectedCheckpointMemoryLease(Id(1),f.t.fixture.devices,Id(2),
    {32*1024*1024,32*1024*1024},source_memory.memory,source_memory.binding);
  Check(source.ok(),"real source fence for visibility calculations");allocation_budget=0;
  for(const auto& c:cases){const auto s=mga::BorrowVisibilitySnapshot(c.snapshot);
   Check(s.active_excluded_local_transaction_ids.data()==c.snapshot.active_excluded_local_transaction_ids.data()&&
     s.in_doubt_excluded_local_transaction_ids.data()==c.snapshot.in_doubt_excluded_local_transaction_ids.data(),"exclusions borrowed without copy");
   const auto r=c.effect?mga::ObserveVersionEffectVisibility(c.metadata,s):mga::ObserveVisibility(c.metadata,s);
   Check(r.decision==c.decision&&r.outcome.status.code==c.owning.status.code&&r.outcome.status.severity==c.owning.status.severity&&
     r.outcome.status.subsystem==c.owning.status.subsystem,"guarded visibility matches independent matrix and unchanged owning statuses");
   Check(r.outcome.diagnostic_code==c.owning.diagnostic.diagnostic_code&&r.outcome.message_key==c.owning.diagnostic.message_key,
     "guarded diagnostic code and key preserve owning result");
   if(*r.outcome.detail)Check(c.owning.diagnostic.arguments.size()==1&&
     c.owning.diagnostic.arguments[0].key=="detail"&&c.owning.diagnostic.arguments[0].text()&&
     *c.owning.diagnostic.arguments[0].text()==r.outcome.detail,"guarded diagnostic argument preserved");
  }
  for(const auto& c:invalid){const auto r=mga::ObserveRowVersionMetadata(c.metadata);
   Check(!r.ok()&&std::string_view(r.diagnostic_code)==c.code&&std::string_view(r.message_key)==c.key,"invalid guarded metadata exact no-allocation diagnostics");}
  auto own=base;own.state=R::uncommitted;own.creator_transaction_state=T::active;
  mga::VisibilitySnapshotView prohibited;prohibited.reader_transaction={7};prohibited.allow_reader_own_uncommitted=false;
  Check(mga::ObserveVersionEffectVisibility(own,prohibited).decision==W,"own visibility requires explicit enabled owning semantics");
  const auto left=allocation_budget;allocation_budget=-1;Check(left==0,"every matrix success wait recovery and error avoids heap under actual source guards");
 }
 source_memory.memory={};source_memory.Empty();
 Check(cases.size()==5824&&invalid.size()==15,"exact exhaustive visibility and invalid metadata counts");
}

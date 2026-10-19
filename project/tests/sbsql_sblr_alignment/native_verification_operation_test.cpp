// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/internal_api/verification/native_verification_history.hpp"
#include "hash_digest.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>
namespace {bool deny_allocations=false;unsigned allocation_attempts=0;}
void* operator new(std::size_t n){if(deny_allocations){++allocation_attempts;throw std::bad_alloc();}if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
namespace {
namespace v=scratchbird::engine::internal_api;namespace db=scratchbird::storage::database;
using namespace scratchbird::core::platform;
using O=v::NativeVerificationOperationState;using E=v::NativeVerificationStateError;
using P=v::NativeVerificationPhase;using C=v::NativeVerificationCompletion;
using Bytes=std::vector<byte>;
unsigned checks=0;
void Check(bool condition,const char* reason,std::source_location location=std::source_location::current()){
 ++checks;if(!condition)throw std::runtime_error(std::string(reason)+" line="+std::to_string(location.line()));
}
Uuid Id(unsigned n){Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[14]=n>>8;id.bytes[15]=n;return id;}
struct Fixture {
 std::array<Uuid,2> files{Id(30),Id(31)},objects{Id(40),Id(41)};
 std::array<v::NativeVerificationPageRange,3> ranges{{{Id(30),0,2},{Id(30),2,8},{Id(31),0,~u64{0}}}};
 O state;
 Fixture(){state.operation_uuid=Id(1);state.database_uuid=Id(2);state.database_lineage_uuid=Id(3);
  state.repair_policy_uuid=Id(4);state.quarantine_policy_uuid=Id(5);state.salvage_policy_uuid=Id(6);state.evidence_policy_uuid=Id(7);
  state.acting_subject_uuid=Id(8);state.maintenance_boundary_uuid=Id(9);state.snapshot_boundary_uuid=Id(10);
  state.cluster_uuid=Id(11);state.cluster_epoch=0;state.created_at_micros=1000;state.updated_at_micros=1001;
  state.verification_levels=1023;state.filespaces=files;state.objects=objects;state.page_ranges=ranges;
  state.last_verified_boundary="catalog:12";
 }
};
void Number(Bytes& out,std::size_t at,unsigned width,u64 n){for(unsigned i=0;i<width;++i)out[at+i]=byte(n>>(8*i));}
void Identity(Bytes& out,std::size_t at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),out.begin()+at);}
Bytes DetailOracle(const v::NativeStartupTransactionDetail& d){const bool marker=d.admission_side_effect_started.has_value();Bytes b(marker?328:320);
 std::copy_n(marker?"SBTXD002":"SBTXD001",8,b.begin());Number(b,8,2,marker?2:1);Number(b,10,2,b.size());Number(b,12,4,b.size());
 Identity(b,16,d.transaction_uuid);Identity(b,32,d.database_uuid);Identity(b,48,d.engine_instance_uuid);Identity(b,64,d.principal_uuid);
 Identity(b,80,d.attachment_uuid);Identity(b,96,d.session_uuid);Identity(b,112,d.policy_snapshot_uuid);Identity(b,128,d.security_snapshot_uuid);
 Identity(b,144,d.visibility_epoch_uuid);Identity(b,160,d.isolation_profile_uuid);Identity(b,176,d.operation_uuid);Identity(b,192,d.data_snapshot_uuid);
 Identity(b,208,d.evidence_root_uuid);Identity(b,224,d.isolation_version_uuid);Identity(b,240,d.transaction_policy_uuid);Identity(b,256,d.transaction_policy_version_uuid);
 Number(b,272,8,d.local_transaction_id);Number(b,280,8,d.started_at_micros);Number(b,288,8,d.completed_at_micros);
 Number(b,296,8,d.isolation_generation);Number(b,304,8,d.transaction_policy_generation);
 Number(b,312,2,d.authority_class);Number(b,314,2,d.intent);Number(b,316,2,d.read_only);Number(b,318,2,u16(d.final_state));
 if(marker)b[320]=*d.admission_side_effect_started?1:0;return b;
}
v::NativeStartupTransactionDetail Detail(){v::NativeStartupTransactionDetail d;
 d.transaction_uuid=Id(301);d.database_uuid=Id(2);d.engine_instance_uuid=Id(302);d.principal_uuid=Id(8);
 d.attachment_uuid=Id(303);d.session_uuid=Id(304);d.policy_snapshot_uuid=Id(305);d.security_snapshot_uuid=Id(306);
 d.visibility_epoch_uuid=Id(307);d.isolation_profile_uuid=Id(308);d.operation_uuid=Id(1);d.data_snapshot_uuid=Id(10);
 d.isolation_version_uuid=Id(309);d.transaction_policy_uuid=Id(310);d.transaction_policy_version_uuid=Id(311);
 d.local_transaction_id=2;d.started_at_micros=1000;d.isolation_generation=1;d.transaction_policy_generation=3;return d;
}
Bytes Oracle(const O& o){Bytes out(256+(o.transaction_detail?(o.transaction_detail->admission_side_effect_started.has_value()?328:320):0)+16*o.filespaces.size()+16*o.objects.size()+32*o.page_ranges.size()+o.last_verified_boundary.size());
 std::copy_n(o.transaction_detail?"SBVR0002":"SBVR0001",8,out.begin());Number(out,8,2,o.transaction_detail?2:1);Number(out,10,2,256);Number(out,12,4,out.size());
 Identity(out,16,o.operation_uuid);Identity(out,32,o.database_uuid);Identity(out,48,o.database_lineage_uuid);
 Identity(out,64,o.repair_policy_uuid);Identity(out,80,o.quarantine_policy_uuid);Identity(out,96,o.salvage_policy_uuid);Identity(out,112,o.evidence_policy_uuid);
 Identity(out,128,o.acting_subject_uuid);Identity(out,144,o.snapshot_boundary_uuid);Identity(out,160,o.maintenance_boundary_uuid);Identity(out,176,o.cluster_uuid);
 Number(out,192,8,o.cluster_epoch.value_or(0));Number(out,200,8,o.created_at_micros);Number(out,208,8,o.updated_at_micros);
 Number(out,216,2,u16(o.verification_class));Number(out,218,2,u16(o.authority_class));Number(out,220,2,u16(o.phase));Number(out,222,2,u16(o.completion));
 Number(out,224,4,o.verification_levels);Number(out,228,4,o.cluster_epoch.has_value());Number(out,232,4,o.filespaces.size());
 Number(out,236,4,o.objects.size());Number(out,240,4,o.page_ranges.size());Number(out,244,4,o.last_verified_boundary.size());
 std::size_t at=256;if(o.transaction_detail){const auto d=DetailOracle(*o.transaction_detail);Number(out,248,4,d.size());
   std::copy(d.begin(),d.end(),out.begin()+at);at+=d.size();}
 for(const auto& id:o.filespaces){Identity(out,at,id);at+=16;}for(const auto& id:o.objects){Identity(out,at,id);at+=16;}
 for(const auto& range:o.page_ranges){Identity(out,at,range.filespace_uuid);Number(out,at+16,8,range.first_page);Number(out,at+24,8,range.page_count);at+=32;}
 std::copy(o.last_verified_boundary.begin(),o.last_verified_boundary.end(),out.begin()+at);return out;
}
struct Workspace {
 std::array<Uuid,4> files,objects;std::array<v::NativeVerificationPageRange,8> ranges;
 auto View(){return v::NativeVerificationStateWorkspace{files,objects,ranges};}
};
void Empty(const v::NativeVerificationStateImage& value){Check(!value.ok()&&!value.record&&value.bytes.empty(),"refusal has no record or prefix");}
template<class Call> auto NoHeap(Call&& call){allocation_attempts=0;deny_allocations=true;auto value=call();deny_allocations=false;
 Check(!allocation_attempts,"bounded codec uses no heap on success or refusal");return value;}
void Roundtrip(const O& o){const auto expected=Oracle(o);Bytes output(expected.size()+13,0xa7);Workspace workspace;
 const auto encoded=NoHeap([&]{return v::EncodeNativeVerificationOperationStateInto(o,expected.size(),std::span(output).subspan(1));});
 Check(encoded.ok()&&encoded.bytes.size()==expected.size()&&std::equal(expected.begin(),expected.end(),encoded.bytes.begin()),"independent complete encoding");
 Check(output.front()==0xa7&&std::all_of(output.begin()+1+expected.size(),output.end(),[](byte b){return b==0xa7;}),"unaligned output preserves guards");
 const auto decoded=NoHeap([&]{return v::DecodeNativeVerificationOperationStateInto(expected,expected.size(),workspace.View());});
 Check(decoded.ok()&&Oracle(*decoded.record)==expected,"every decoded field independently reconstructs bytes");
 Check(decoded.record->last_verified_boundary.data()==reinterpret_cast<const char*>(expected.data()+expected.size()-o.last_verified_boundary.size()),"boundary borrows immutable aggregate bytes");
}
void StartupDetail(){using DE=v::NativeStartupDetailError;using S=scratchbird::transaction::mga::TransactionState;
 auto d=Detail();const auto expected=DetailOracle(d);Bytes out(329,0x87);
 const auto encoded=NoHeap([&]{return v::EncodeNativeStartupTransactionDetailInto(d,std::span(out).subspan(1));});
 Check(encoded.ok()&&std::ranges::equal(encoded.bytes,expected)&&out.front()==0x87&&out.back()==0x87,
     "complete detail byte oracle and unaligned output guards");
 const auto decoded=NoHeap([&]{return v::DecodeNativeStartupTransactionDetail(expected);});
 Check(decoded.ok()&&*decoded.record==d&&DetailOracle(*decoded.record)==expected,"all detail fields independently decoded");
 for(unsigned size=0;size<320;++size){
  const auto short_read=NoHeap([&]{return v::DecodeNativeStartupTransactionDetail(std::span(expected).first(size));});
  Check(!short_read.ok()&&!short_read.record&&short_read.bytes.empty(),"every detail prefix refused");
  std::fill(out.begin(),out.end(),0x87);
  const auto short_write=NoHeap([&]{return v::EncodeNativeStartupTransactionDetailInto(d,std::span(out).first(size));});
  Check(short_write.error==DE::resource_exhausted&&std::all_of(out.begin(),out.end(),[](byte b){return b==0x87;}),"every short detail output unchanged");
 }
 auto trailing=expected;trailing.push_back(0);Check(!v::DecodeNativeStartupTransactionDetail(trailing).ok(),"detail trailing bytes refused");
 for(unsigned i=0;i<16;++i){if(i==12)continue;
  auto bad=expected;bad[16+16*i+6]=0x40;
  Check(NoHeap([&]{return v::DecodeNativeStartupTransactionDetail(bad);}).error==DE::invalid_identity,"every detail identity requires engine v7 domain");
  std::fill_n(bad.begin()+16+16*i,16,0);
  Check(!v::DecodeNativeStartupTransactionDetail(bad).ok(),"every required detail identity is present");
  for(unsigned j=0;j<16;++j){if(i==j||j==12)continue;bad=expected;
   std::copy_n(bad.begin()+16+16*j,16,bad.begin()+16+16*i);
   Check(!v::DecodeNativeStartupTransactionDetail(bad).ok(),"crossed ownership identities refused");}
 }
 for(unsigned fault=0;fault<14;++fault){auto bad=d;
  switch(fault){case 0:bad.local_transaction_id=1;break;case 1:bad.local_transaction_id=UINT64_MAX;break;
   case 2:bad.started_at_micros=0;break;case 3:bad.isolation_generation=0;break;case 4:bad.transaction_policy_generation=0;break;
   case 5:bad.authority_class=5;break;case 6:bad.intent=2;break;case 7:bad.read_only=1;break;
   case 8:bad.final_state=S::active;break;case 9:bad.completed_at_micros=1001;break;
   case 10:bad.final_state=S::committed;break;case 11:bad.final_state=S::committed;bad.completed_at_micros=1002;break;
   case 12:bad.final_state=S::rolled_back;bad.completed_at_micros=999;bad.evidence_root_uuid=Id(312);break;
   case 13:bad.evidence_root_uuid=d.operation_uuid;break;
  }
  std::fill(out.begin(),out.end(),0x87);const auto invalid=NoHeap([&]{return v::EncodeNativeStartupTransactionDetailInto(bad,out);});
  Check(!invalid.ok()&&std::all_of(out.begin(),out.end(),[](byte b){return b==0x87;}),"invalid detail refuses before output mutation");
 }
 Check(NoHeap([&]{return v::EncodeNativeStartupTransactionDetailInto(d,{reinterpret_cast<byte*>(&d),sizeof d});}).error==DE::invalid_workspace,
     "detail output cannot overwrite its own source");
 for(S final:{S::committed,S::rolled_back,S::failed_terminal}){auto done=d;done.final_state=final;done.completed_at_micros=1002;done.evidence_root_uuid=Id(312);
  Check(v::ValidateNativeStartupTransactionDetailEvolution(d,done)==DE::none&&
      v::ValidateNativeStartupTransactionDetailEvolution(done,done)==DE::none&&
      v::ValidateNativeStartupTransactionDetailEvolution(done,d)==DE::invalid_transition,"representation permits one-way final outcome, never reversal");
  auto changed=done;++changed.completed_at_micros;
  Check(v::ValidateNativeStartupTransactionDetailEvolution(done,changed)==DE::invalid_transition,"terminal detail immutable");
 }
 for(unsigned i=0;i<5;++i){auto changed=d;
  switch(i){case 0:++changed.local_transaction_id;break;case 1:++changed.started_at_micros;break;case 2:++changed.isolation_generation;break;
   case 3:++changed.transaction_policy_generation;break;case 4:changed.visibility_epoch_uuid=Id(320);break;}
  Check(v::ValidateNativeStartupTransactionDetailEvolution(d,changed)==DE::immutable_field,"detail cannot replace original transaction policy or snapshot lifetime");
 }
 Fixture f;auto& o=f.state;o.phase=P::boundary_established;o.cluster_uuid={};o.cluster_epoch.reset();o.transaction_detail=d;
 Roundtrip(o);const auto v2=Oracle(o);Workspace workspace;
 for(unsigned size=0;size<v2.size();++size)Empty(NoHeap([&]{return v::DecodeNativeVerificationOperationStateInto(std::span(v2).first(size),v2.size(),workspace.View());}));
 for(unsigned fault=0;fault<6;++fault){auto bad=v2;
  switch(fault){case 0:bad[7]='1';break;case 1:Number(bad,8,2,1);break;case 2:Number(bad,248,4,319);break;
   case 3:bad[252]=1;break;case 4:bad[256]='X';break;case 5:bad.push_back(0);Number(bad,12,4,bad.size());break;}
  Empty(NoHeap([&]{return v::DecodeNativeVerificationOperationStateInto(bad,bad.size(),workspace.View());}));
 }
 auto before=o;before.transaction_detail.reset();before.snapshot_boundary_uuid={};
 Check(v::ValidateNativeVerificationOperationEvolution(before,o)==E::none,"complete actual snapshot and detail join the same established boundary");
 for(unsigned fault=0;fault<12;++fault){auto a=before,b=o;
  switch(fault){case 0:a.phase=P::admitted;break;case 1:b.phase=P::identity_verified;break;
   case 2:a=o;b.transaction_detail.reset();break;case 3:b.transaction_detail->transaction_uuid=Id(322);a=o;break;
   case 4:b.transaction_detail->data_snapshot_uuid=Id(322);break;case 5:b.transaction_detail->database_uuid=Id(322);break;
   case 6:b.transaction_detail->operation_uuid=Id(322);break;case 7:b.transaction_detail->principal_uuid=Id(322);break;
   case 8:b.authority_class=v::NativeVerificationAuthority::local_sysarch;break;case 9:b.cluster_uuid=Id(11);b.cluster_epoch=0;break;
   case 10:++b.transaction_detail->started_at_micros;break;case 11:b.transaction_detail->final_state=S::committed;
     b.transaction_detail->completed_at_micros=1002;b.transaction_detail->evidence_root_uuid=Id(312);break;
  }
  Check(v::ValidateNativeVerificationOperationEvolution(a,b)!=E::none,"invalid detail acquisition substitution or enclosing binding refused");
 }
}
void AdmissionMarker(){using DE=v::NativeStartupDetailError;using S=scratchbird::transaction::mga::TransactionState;
 auto legacy=Detail(),clear=legacy,started=legacy;clear.admission_side_effect_started=false;started.admission_side_effect_started=true;
 for(const auto& d:{clear,started}){const auto expected=DetailOracle(d);Bytes out(330,0x91);
  const auto encoded=NoHeap([&]{return v::EncodeNativeStartupTransactionDetailInto(d,std::span(out).subspan(1));});
  Check(encoded.ok()&&std::ranges::equal(encoded.bytes,expected)&&out.front()==0x91&&out.back()==0x91,"v2 complete independent bytes and guards");
  const auto decoded=NoHeap([&]{return v::DecodeNativeStartupTransactionDetail(expected);});
  Check(decoded.ok()&&decoded.record==d,"v2 distinguishes clear and fenced marker");
  for(unsigned n=0;n<328;++n){
   Check(!NoHeap([&]{return v::DecodeNativeStartupTransactionDetail(std::span(expected).first(n));}).ok(),"every v2 prefix refuses including v1 width");
   std::fill(out.begin(),out.end(),0x91);
   const auto short_write=NoHeap([&]{return v::EncodeNativeStartupTransactionDetailInto(d,std::span(out).first(n));});
   Check(short_write.error==DE::resource_exhausted&&std::ranges::all_of(out,[](byte b){return b==0x91;}),"short v2 output unchanged");
  }
  for(unsigned at=320;at<328;++at){auto bad=expected;bad[at]=at==320?2:1;
   Check(!NoHeap([&]{return v::DecodeNativeStartupTransactionDetail(bad);}).ok(),"closed marker and every reserved byte");}
  for(unsigned fault=0;fault<5;++fault){auto bad=expected;
   switch(fault){case 0:bad[7]='1';break;case 1:Number(bad,8,2,1);break;case 2:Number(bad,10,2,320);break;
    case 3:Number(bad,12,4,320);break;case 4:bad.push_back(0);break;}
   Check(!v::DecodeNativeStartupTransactionDetail(bad).ok(),"v2 cannot masquerade as legacy or accept trailing bytes");
  }
  Fixture f;f.state.phase=P::boundary_established;f.state.cluster_uuid={};f.state.cluster_epoch.reset();f.state.transaction_detail=d;Roundtrip(f.state);
 }
 for(const auto& pair:{std::pair{clear,clear},std::pair{clear,started},std::pair{started,started},std::pair{legacy,started}})
  Check(v::ValidateNativeStartupTransactionDetailEvolution(pair.first,pair.second)==DE::none,"monotonic marker and conservative legacy upgrade");
 for(const auto& pair:{std::pair{started,clear},std::pair{clear,legacy},std::pair{started,legacy},std::pair{legacy,clear}})
  Check(v::ValidateNativeStartupTransactionDetailEvolution(pair.first,pair.second)==DE::invalid_transition,"never erase effects or infer clear from unknown legacy");
 for(auto state:{S::committed,S::rolled_back}){auto done=clear;done.final_state=state;done.completed_at_micros=1002;done.evidence_root_uuid=Id(312);
  Check(v::ValidateNativeStartupTransactionDetail(done)==DE::invalid_record,"commit and admission rollback require fenced effects");
  done.admission_side_effect_started=true;Check(v::ValidateNativeStartupTransactionDetail(done)==DE::none,"fenced final detail remains representation only");
 }
 auto dead=clear;dead.final_state=S::failed_terminal;dead.completed_at_micros=1002;dead.evidence_root_uuid=Id(312);
 Check(v::ValidateNativeStartupTransactionDetailEvolution(clear,dead)==DE::none,"no-effect denial may record failed terminal with actual final evidence");
}
void DetailAggregateHistory(){Fixture f;auto initial=f.state;
 initial.cluster_uuid={};initial.cluster_epoch.reset();initial.maintenance_boundary_uuid={};initial.snapshot_boundary_uuid={};
 initial.updated_at_micros=initial.created_at_micros;initial.last_verified_boundary={};
 auto d=Detail();d.admission_side_effect_started=true;const db::NativeStartupBinding binding{d.operation_uuid,d.session_uuid,d.transaction_uuid,2,1};
 v::NativeStartupVerificationRequest request{binding,initial};Bytes request_bytes(2048);
 const auto request_image=v::EncodeNativeStartupVerificationRequestInto(request,request_bytes.size(),request_bytes);
 Check(request_image.ok(),"actual canonical first-startup request representation");
 auto boundary=initial;boundary.phase=P::boundary_established;boundary.maintenance_boundary_uuid=Id(9);++boundary.updated_at_micros;
 auto detailed=boundary;detailed.transaction_detail=d;detailed.snapshot_boundary_uuid=d.data_snapshot_uuid;++detailed.updated_at_micros;
 db::NativeManagementOperation record;record.uuid=d.operation_uuid;record.database_uuid=d.database_uuid;
 record.bootstrap_uuid=Id(100);record.descriptor_uuid=Id(101);record.family_uuid=Id(102);record.target_type_uuid=Id(103);record.target_uuid=d.database_uuid;
 record.initiator_uuid=d.principal_uuid;record.request_context_uuid=d.session_uuid;record.policy_snapshot_uuid=d.policy_snapshot_uuid;
 record.security_snapshot_uuid=d.security_snapshot_uuid;record.created_at=record.updated_at=Id(108);record.initiator_kind=4;
 record.idempotency_key="startup-detail";record.normalized_request_bytes.assign(request_image.bytes.begin(),request_image.bytes.end());
 record.normalized_request_sha256=scratchbird::core::hash::ComputeSha256DigestNative(record.normalized_request_bytes.data(),record.normalized_request_bytes.size()).digest;
 std::array<db::NativeManagementOperationImage,3> images;
 std::array<db::NativeManagementHistoryEntryView,3> entries;
 std::array<db::NativeManagementStepView,1> steps;std::array<Uuid,7> ids;
 const std::array states{initial,boundary,detailed};
 for(unsigned n=0;n<3;++n){record.revision=n+1;record.updated_at=Id(108+n);record.operation_state_bytes=Oracle(states[n]);
  if(n){record.state=db::NativeManagementState::authorized;record.generation_guards={1,1,1,std::nullopt};record.boundary_uuid=Id(9);}
  images[n]=db::EncodeNativeManagementOperation(record,8192);Check(images[n].ok(),"valid sealed detail history aggregate");
  const auto decoded=db::DecodeNativeManagementOperationInto(images[n].bytes,8192,{steps,ids});Check(decoded.ok(),"sealed detail history read");
  entries[n].record=*decoded.record;entries[n].aggregate=images[n].bytes;entries[n].plan.intent.startup_binding=binding;
 }
 db::NativeManagementHistoryView history;history.error=db::NativeManagementHistoryError::none;
 history.anchor=db::NativeManagementCheckpointAnchor{};history.selection=db::NativeCheckpointSelection{};
 history.selection->header.database_uuid=d.database_uuid;history.entries=entries;
 std::array<byte,8192> scratch;Workspace workspace;
 const auto inspect=[&]{return NoHeap([&]{return v::VerifyNativeVerificationHistory(history,8192,scratch);});};
 const auto observed=inspect();Check(observed.ok()&&observed.verified_operations==1&&observed.verified_revisions==3,
     "complete mixed v1/v2 history binds immutable original request, snapshot and transaction detail");
 for(unsigned fault=0;fault<5;++fault){entries[2].plan.intent.startup_binding=binding;auto& changed=*entries[2].plan.intent.startup_binding;
  switch(fault){case 0:changed.transaction_uuid=Id(330);break;case 1:++changed.local_transaction_id;break;
   case 2:changed.operation_uuid=Id(330);break;case 3:changed.session_uuid=Id(330);break;case 4:entries[2].plan.intent.startup_binding.reset();break;}
  const auto invalid=inspect();Check(!invalid.ok()&&invalid.error==v::NativeVerificationHistoryError::binding_mismatch,
      "detail cannot change or omit the retained startup transaction binding");
 }
 entries[2].plan.intent.startup_binding=binding;
 for(unsigned fault=0;fault<4;++fault){auto changed=entries[2].record;
  switch(fault){case 0:changed.request_context_uuid=Id(331);break;case 1:changed.policy_snapshot_uuid=Id(331);break;
   case 2:changed.security_snapshot_uuid=Id(331);break;case 3:changed.initiator_uuid=Id(331);break;}
  const auto invalid=NoHeap([&]{return v::ReadNativeVerificationStateFromOperationInto(changed,8192,workspace.View(),ids);});
  Check(!invalid.ok()&&invalid.error==E::binding_mismatch,"detail binds the actual enclosing service and policy snapshots");
 }
}
void StartupRequest(){
 Fixture f;auto initial=f.state;initial.snapshot_boundary_uuid={};initial.maintenance_boundary_uuid={};
 initial.cluster_uuid={};initial.cluster_epoch.reset();initial.updated_at_micros=initial.created_at_micros;
 initial.last_verified_boundary={};
 v::NativeStartupVerificationRequest request{{initial.operation_uuid,Id(80),Id(81),2,1},initial};
 Bytes expected(64);Identity(expected,0,initial.operation_uuid);Identity(expected,16,Id(80));Identity(expected,32,Id(81));
 Number(expected,48,8,2);Number(expected,56,8,1);const auto state=Oracle(initial);expected.insert(expected.end(),state.begin(),state.end());
 Bytes output(expected.size()+9,0xa7);Workspace w;
 const auto encoded=NoHeap([&]{return v::EncodeNativeStartupVerificationRequestInto(request,expected.size(),output);});
 Check(encoded.ok()&&std::equal(expected.begin(),expected.end(),encoded.bytes.begin(),encoded.bytes.end())&&output.back()==0xa7,
     "complete canonical startup tuple and initial state match independent bytes");
 const auto decoded=NoHeap([&]{return v::DecodeNativeStartupVerificationRequestInto(expected,expected.size(),w.View());});
 Check(decoded.ok()&&decoded.request->binding==request.binding&&Oracle(decoded.request->initial)==state,
     "canonical startup request reconstructs all fields independently");
 for(std::size_t size=0;size<expected.size();++size){
  const auto partial=NoHeap([&]{return v::DecodeNativeStartupVerificationRequestInto(std::span(expected).first(size),expected.size(),w.View());});
  Check(!partial.ok()&&!partial.request&&partial.bytes.empty(),"every startup request truncation exposes no partial binding");
  std::fill(output.begin(),output.end(),0xa7);
  const auto short_output=NoHeap([&]{return v::EncodeNativeStartupVerificationRequestInto(request,expected.size(),std::span(output).first(size));});
  Check(!short_output.ok()&&std::all_of(output.begin(),output.end(),[](byte b){return b==0xa7;}),
      "every short startup request output remains unchanged");
 }
 for(unsigned fault=0;fault<14;++fault){auto bad=request;
  switch(fault){case 0:bad.binding.session_uuid={};break;case 1:bad.binding.transaction_uuid=bad.binding.session_uuid;break;
   case 2:bad.binding.local_transaction_id=1;break;case 3:bad.binding.local_transaction_id=UINT64_MAX;break;
   case 4:bad.binding.fence_generation=0;break;case 5:bad.initial.operation_uuid=Id(82);break;
   case 6:bad.initial.acting_subject_uuid={};break;case 7:bad.initial.snapshot_boundary_uuid=Id(83);break;
   case 8:bad.initial.maintenance_boundary_uuid=Id(84);break;case 9:++bad.initial.updated_at_micros;break;
   case 10:bad.initial.verification_class=v::NativeVerificationClass::online_consistency;break;
   case 11:bad.initial.authority_class=v::NativeVerificationAuthority::local_sysarch;break;
   case 12:bad.initial.phase=P::boundary_established;break;case 13:bad.initial.completion=C::paused;break;
  }
  const auto invalid=NoHeap([&]{return v::EncodeNativeStartupVerificationRequestInto(bad,output.size(),output);});
  Check(!invalid.ok()&&!invalid.request,"startup origin cannot substitute identities, authority, time or an already executed boundary");
 }
 auto alias=w.View();alias.filespaces={reinterpret_cast<Uuid*>(expected.data()+16),2};
 const auto saved=expected;
 Check(NoHeap([&]{return v::DecodeNativeStartupVerificationRequestInto(expected,expected.size(),alias);}).error==E::invalid_workspace&&expected==saved,
     "startup request decode cannot overwrite its original tuple");
 db::NativeManagementOperationView record;record.database_uuid=initial.database_uuid;record.bootstrap_uuid=Id(90);record.uuid=initial.operation_uuid;
 record.descriptor_uuid=Id(91);record.family_uuid=Id(92);record.target_type_uuid=Id(93);record.target_uuid=initial.database_uuid;
 record.initiator_uuid=initial.acting_subject_uuid;record.request_context_uuid=request.binding.session_uuid;
 record.policy_snapshot_uuid=Id(94);record.created_at=record.updated_at=Id(95);record.initiator_kind=4;
 record.normalized_request_bytes=expected;record.operation_state_bytes=state;record.idempotency_key="startup-oracle";
 const auto hash=scratchbird::core::hash::ComputeSha256DigestNative(expected.data(),expected.size());Check(hash.ok(),"actual canonical request seal");
 record.normalized_request_sha256=hash.digest;
 std::array<db::NativeManagementHistoryEntryView,1> entries;entries[0].record=record;entries[0].plan.intent.startup_binding=request.binding;
 db::NativeManagementHistoryView history;history.error=db::NativeManagementHistoryError::none;history.anchor=db::NativeManagementCheckpointAnchor{};
 history.selection=db::NativeCheckpointSelection{};history.selection->header.database_uuid=initial.database_uuid;history.entries=entries;
 std::array<byte,8192> scratch;
 const auto inspect=[&]{return NoHeap([&]{return v::VerifyNativeVerificationHistory(history,8192,scratch);});};
 Check(inspect().ok(),"cold startup family admits a complete matching origin");
 for(unsigned fault=0;fault<3;++fault){entries[0].record=record;entries[0].plan.intent.startup_binding=request.binding;
  if(fault==0)++entries[0].plan.intent.startup_binding->fence_generation;
  if(fault==1)entries[0].record.request_context_uuid=Id(97);
  if(fault==2)entries[0].record.initiator_uuid=Id(98);
  const auto refused=inspect();Check(refused.error==v::NativeVerificationHistoryError::binding_mismatch&&!refused.selection,
      "cold startup family rejects a crossed tuple, session or acting subject despite intact canonical bytes");
 }
}
void ShapeAndFraming(){Fixture f;Roundtrip(f.state);auto genesis=f.state;genesis.database_lineage_uuid=genesis.database_uuid;Roundtrip(genesis);auto local=f.state;local.cluster_uuid={};local.cluster_epoch.reset();local.acting_subject_uuid={};local.snapshot_boundary_uuid={};Roundtrip(local);
 for(unsigned klass=1;klass<=9;++klass)for(unsigned authority=1;authority<=6;++authority){auto o=f.state;o.verification_class=v::NativeVerificationClass(klass);o.authority_class=v::NativeVerificationAuthority(authority);Roundtrip(o);}
 for(unsigned bit=0;bit<10;++bit){auto o=f.state;o.verification_levels=1u<<bit;Roundtrip(o);}
 for(unsigned phase=1;phase<=22;++phase){auto o=f.state;o.phase=P(phase);o.completion=phase==20?C::review_required:phase==21?C::completed_clean:phase==22?C::failed:C::running;Roundtrip(o);}
 for(C completion:{C::completed_clean,C::completed_findings,C::completed_repaired,C::quarantined}){auto o=f.state;o.phase=P::completed;o.completion=completion;Roundtrip(o);}
 std::string largest(4096,'b');largest[0]=0;auto o=f.state;o.last_verified_boundary=largest;Roundtrip(o);
 const auto reject=[&](const O& invalid){Bytes bytes(8192,0xa9);const auto before=bytes;
  const auto result=NoHeap([&]{return v::EncodeNativeVerificationOperationStateInto(invalid,8192,bytes);});Empty(result);Check(bytes==before,"invalid shape refused before output writes");
  const auto raw=Oracle(invalid);Workspace w;Empty(NoHeap([&]{return v::DecodeNativeVerificationOperationStateInto(raw,raw.size(),w.View());}));};
 for(unsigned field=0;field<23;++field){o=f.state;switch(field){
  case 0:o.operation_uuid={};break;case 1:o.database_uuid={};break;case 2:o.database_lineage_uuid={};break;
  case 3:o.repair_policy_uuid={};break;case 4:o.evidence_policy_uuid.bytes[6]=0x40;break;
  case 5:o.verification_class=v::NativeVerificationClass(10);break;case 6:o.authority_class=v::NativeVerificationAuthority(0);break;
  case 7:o.phase=P(23);break;case 8:o.completion=C(10);break;case 9:o.verification_levels=0;break;case 10:o.verification_levels=1024;break;
  case 11:o.created_at_micros=0;break;case 12:o.updated_at_micros=999;break;case 13:o.cluster_epoch.reset();break;
  case 14:o.cluster_uuid={};break;case 15:o.filespaces={};break;case 16:o.phase=P::completed;break;
  case 17:o.completion=C::completed_clean;break;case 18:o.last_verified_boundary="\x80";break;
  case 19:o.phase=P::catalog_verifying;o.maintenance_boundary_uuid={};break;
  case 20:o.verification_class=v::NativeVerificationClass::online_consistency;o.phase=P::catalog_verifying;o.snapshot_boundary_uuid={};break;
  case 21:o.phase=P::cluster_verifying;o.cluster_uuid={};o.cluster_epoch.reset();break;
  case 22:o.verification_class=v::NativeVerificationClass::cluster;o.cluster_uuid={};o.cluster_epoch.reset();break;
 }reject(o);}
 auto saved=f.files;f.files[1]=f.files[0];reject(f.state);f.files=saved;std::swap(f.files[0],f.files[1]);reject(f.state);f.files=saved;
 const auto saved_ranges=f.ranges;
 for(unsigned field=0;field<4;++field){f.ranges=saved_ranges;switch(field){case 0:f.ranges[1].first_page=1;break;case 1:f.ranges[1].page_count=0;break;case 2:f.ranges[2].first_page=2;break;case 3:f.ranges[1].filespace_uuid=Id(99);break;}reject(f.state);}f.ranges=saved_ranges;
 const auto raw=Oracle(f.state);Workspace w;
 for(std::size_t size=0;size<raw.size();++size){Bytes truncated(raw.begin(),raw.begin()+size);if(size>=16)Number(truncated,12,4,size);Empty(NoHeap([&]{return v::DecodeNativeVerificationOperationStateInto(truncated,raw.size(),w.View());}));}
 for(std::size_t at:{0u,7u,8u,10u,12u,228u,232u,236u,240u,244u,248u,255u}){auto bad=raw;bad[at]^=0xff;Empty(NoHeap([&]{return v::DecodeNativeVerificationOperationStateInto(bad,bad.size(),w.View());}));}
 auto extra=raw;extra.push_back(0);Number(extra,12,4,extra.size());Empty(NoHeap([&]{return v::DecodeNativeVerificationOperationStateInto(extra,extra.size(),w.View());}));
 o=local;auto absent_epoch=Oracle(o);Number(absent_epoch,192,8,1);Empty(NoHeap([&]{return v::DecodeNativeVerificationOperationStateInto(absent_epoch,absent_epoch.size(),w.View());}));
}
void Bounds(){Fixture f;const auto raw=Oracle(f.state);Workspace w;Bytes out(raw.size(),0x87);const auto before=out;
 for(u64 budget:{u64{0},u64{255},u64(raw.size()-1)}){
  auto result=NoHeap([&]{return v::EncodeNativeVerificationOperationStateInto(f.state,budget,out);});Empty(result);Check(result.error==E::resource_exhausted&&out==before,"whole state budget before writes");
  result=NoHeap([&]{return v::DecodeNativeVerificationOperationStateInto(raw,budget,w.View());});Empty(result);Check(result.error==E::resource_exhausted,"decode whole state budget");
 }
 for(unsigned part=0;part<3;++part){auto short_w=w.View();if(part==0)short_w.filespaces=short_w.filespaces.first(1);if(part==1)short_w.objects=short_w.objects.first(1);if(part==2)short_w.page_ranges=short_w.page_ranges.first(2);
  const auto result=NoHeap([&]{return v::DecodeNativeVerificationOperationStateInto(raw,raw.size(),short_w);});Empty(result);Check(result.error==E::resource_exhausted,"each decoded target array bounded");}
 const auto mutable_bytes=[](const void* p,std::size_t n){return std::span<byte>{static_cast<byte*>(const_cast<void*>(p)),n};};
 for(auto region:{mutable_bytes(&f.state,sizeof(f.state)),mutable_bytes(f.files.data(),sizeof(f.files)),mutable_bytes(f.objects.data(),sizeof(f.objects)),mutable_bytes(f.ranges.data(),sizeof(f.ranges)),mutable_bytes(f.state.last_verified_boundary.data(),f.state.last_verified_boundary.size())}){
  const auto result=NoHeap([&]{return v::EncodeNativeVerificationOperationStateInto(f.state,raw.size(),region);});Empty(result);Check(result.error==E::invalid_workspace&&Oracle(f.state)==raw,"whole output excludes every borrowed input");}
 Bytes carrier=raw;carrier.resize(raw.size()+4096,0xa6);const auto untouched=carrier;
 for(unsigned alias=0;alias<4;++alias){auto bad=w.View();
  if(alias==0)bad.filespaces={reinterpret_cast<Uuid*>(carrier.data()+1),2};
  if(alias==1)bad.objects=bad.filespaces;
  if(alias==2)bad.page_ranges={reinterpret_cast<v::NativeVerificationPageRange*>(carrier.data()+1),3};
  if(alias==3)bad.page_ranges={reinterpret_cast<v::NativeVerificationPageRange*>(w.ranges.data()),std::numeric_limits<std::size_t>::max()};
  const auto result=NoHeap([&]{return v::DecodeNativeVerificationOperationStateInto(std::span<const byte>(carrier).first(raw.size()),raw.size(),bad);});Empty(result);
  Check(result.error==E::invalid_workspace&&carrier==untouched,"decoder alias and overflow refusal preserve immutable source");}
 auto invalid=f.state;invalid.page_ranges={reinterpret_cast<const v::NativeVerificationPageRange*>(reinterpret_cast<const byte*>(f.ranges.data())+1),1};
 Check(v::ValidateNativeVerificationOperationState(invalid)==E::invalid_workspace,"constructed range view alignment checked");
}
void Evolution(){Fixture f;
 const std::array<std::vector<unsigned>,22> edges{{{2},{3},{4},{5},{6},{7},{8},{9},{10},{11,12},{12},{13},{14,16},{15},{16},{21},{16},{19},{16},{},{},{}}};
 const auto state=[&](unsigned phase){auto o=f.state;o.phase=P(phase);o.completion=phase==20?C::review_required:phase==21?C::completed_clean:phase==22?C::failed:C::running;return o;};
 for(unsigned from=1;from<=22;++from)for(unsigned to=1;to<=22;++to){auto a=state(from),b=state(to);b.updated_at_micros++;
  const bool expected=from<20&&(from==to||to==17||to==18||to==20||to==22||std::find(edges[from-1].begin(),edges[from-1].end(),to)!=edges[from-1].end());
  const auto actual=NoHeap([&]{return v::ValidateNativeVerificationOperationEvolution(a,b);});Check((actual==E::none)==expected,"all full verification phase edges and illegal skips");
  Check(v::ValidateNativeVerificationOperationEvolution(a,a)==E::none,"exact retained-state replay including terminal states");
 }
 auto a=state(1);a.snapshot_boundary_uuid={};a.maintenance_boundary_uuid={};auto b=a;b.phase=P::boundary_established;b.maintenance_boundary_uuid=Id(70);
 Check(v::ValidateNativeVerificationOperationEvolution(a,b)==E::none,"boundary acquired once on the real boundary phase");
 a=b;b.phase=P::identity_verified;b.maintenance_boundary_uuid=Id(71);Check(v::ValidateNativeVerificationOperationEvolution(a,b)==E::immutable_field,"established maintenance boundary cannot change");
 a=state(6);b=a;b.updated_at_micros++;
 for(auto member:{&O::operation_uuid,&O::database_uuid,&O::database_lineage_uuid,&O::repair_policy_uuid,&O::quarantine_policy_uuid,
    &O::salvage_policy_uuid,&O::evidence_policy_uuid,&O::acting_subject_uuid,&O::snapshot_boundary_uuid,&O::maintenance_boundary_uuid,&O::cluster_uuid}){
  auto changed=b;changed.*member=Id(90);Check(v::ValidateNativeVerificationOperationEvolution(a,changed)==E::immutable_field,"all retained identity bindings immutable");}
 for(unsigned field=0;field<6;++field){auto changed=b;switch(field){case 0:changed.verification_class=v::NativeVerificationClass::targeted;break;case 1:changed.authority_class=v::NativeVerificationAuthority::local_sysarch;break;
  case 2:changed.verification_levels=1;break;case 3:changed.cluster_epoch=1;break;case 4:changed.created_at_micros++;break;case 5:changed.objects=changed.objects.first(1);break;}
  Check(v::ValidateNativeVerificationOperationEvolution(a,changed)==E::immutable_field,"class scope authority and creation binding immutable");}
 b.updated_at_micros=a.updated_at_micros-1;Check(v::ValidateNativeVerificationOperationEvolution(a,b)==E::invalid_transition,"update time never moves backwards");
 b=a;b.last_verified_boundary="catalog:13";Check(v::ValidateNativeVerificationOperationEvolution(a,b)==E::none,"same phase can advance safe boundary at the same microsecond");
}
void AggregateBinding(){Fixture f;f.state.cluster_uuid={};f.state.cluster_epoch.reset();const auto state=Oracle(f.state);
 db::NativeManagementOperation record;record.uuid=f.state.operation_uuid;record.database_uuid=f.state.database_uuid;
 record.bootstrap_uuid=Id(100);record.descriptor_uuid=Id(101);record.family_uuid=Id(102);record.target_type_uuid=Id(103);record.target_uuid=Id(104);
 record.initiator_uuid=Id(105);record.request_context_uuid=Id(106);record.policy_snapshot_uuid=Id(107);record.created_at=Id(108);record.updated_at=Id(109);
 record.initiator_kind=4;record.idempotency_key="verify-fixture";record.normalized_request_bytes={0,1,2,3};
 record.normalized_request_sha256=scratchbird::core::hash::ComputeSha256DigestNative(record.normalized_request_bytes.data(),record.normalized_request_bytes.size()).digest;
 record.operation_state_bytes=state;const auto encoded=db::EncodeNativeManagementOperation(record,8192);Check(encoded.ok(),"valid enclosing V3 declaration fixture");
 std::array<db::NativeManagementStepView,1> steps;std::array<Uuid,7> ids;
 const auto decoded=db::DecodeNativeManagementOperationInto(encoded.bytes,8192,{steps,ids});Check(decoded.ok(),"complete enclosing aggregate seal admitted");Workspace w;
 auto result=NoHeap([&]{return v::ReadNativeVerificationStateFromOperationInto(*decoded.record,8192,w.View(),ids);});
 Check(result.ok()&&Oracle(*result.record)==state,"typed state read from exact aggregate bytes after common record validation");
 for(u64 budget:{u64(state.size()),u64(encoded.bytes.size()-1)}){
  const auto before=ids;
  auto refused=NoHeap([&]{return v::ReadNativeVerificationStateFromOperationInto(*decoded.record,budget,w.View(),ids);});
  Empty(refused);Check(refused.error==E::resource_exhausted&&ids==before,"family admission bounds the complete aggregate before validation scratch writes");
 }
 result=NoHeap([&]{return v::ReadNativeVerificationStateFromOperationInto(*decoded.record,encoded.bytes.size(),w.View(),ids);});
 Check(result.ok(),"exact complete aggregate budget admits its nested typed state");
 for(unsigned field=0;field<4;++field){auto altered=*decoded.record;switch(field){case 0:altered.uuid=Id(99);break;case 1:altered.database_uuid=Id(99);break;
  case 2:altered.normalized_request_sha256[0]^=1;break;case 3:altered.operation_state_bytes={};break;}
  Empty(NoHeap([&]{return v::ReadNativeVerificationStateFromOperationInto(altered,8192,w.View(),ids);}));}
 auto alias=w.View();alias.filespaces={reinterpret_cast<Uuid*>(const_cast<byte*>(decoded.record->normalized_request_bytes.data())),2};
 result=NoHeap([&]{return v::ReadNativeVerificationStateFromOperationInto(*decoded.record,8192,alias,ids);});Empty(result);Check(result.error==E::invalid_workspace,"family decode cannot overwrite exact original request");
 Workspace after_workspace;
 auto successor=record;successor.revision=2;successor.updated_at=Id(110);successor.state=db::NativeManagementState::authorized;
 successor.security_snapshot_uuid=Id(111);successor.generation_guards={1,1,1,std::nullopt};
 auto next_state=f.state;next_state.phase=P::boundary_established;next_state.updated_at_micros++;
 successor.operation_state_bytes=Oracle(next_state);
 const auto later=db::EncodeNativeManagementOperation(successor,8192);Check(later.ok(),"valid authorized next common revision");
 std::array<db::NativeManagementStepView,1> later_steps;std::array<Uuid,7> later_ids;
 const auto later_view=db::DecodeNativeManagementOperationInto(later.bytes,8192,{later_steps,later_ids});Check(later_view.ok(),"later aggregate completely admitted");
 const auto evolve=[&](const auto& a,const auto& b,auto first,auto second,u64 budget=8192){return NoHeap([&]{
  return v::ValidateNativeVerificationManagementEvolution(a,b,budget,first,second,ids);});};
 Check(evolve(*decoded.record,*later_view.record,w.View(),after_workspace.View()).ok(),"common revision and verification phase advance together");
 Check(evolve(*decoded.record,*decoded.record,w.View(),after_workspace.View()).ok(),"exact common and typed replay");
 auto rewound=*later_view.record;rewound.revision=1;
 auto evolution=evolve(*decoded.record,rewound,w.View(),after_workspace.View());
 Check(!evolution.ok()&&evolution.operation_error==db::NativeManagementOperationError::invalid_transition,"typed phase cannot bypass common revision checks");
 auto illegal_state=next_state;illegal_state.phase=P::identity_verified;const auto illegal=Oracle(illegal_state);
 auto skipped=*later_view.record;skipped.operation_state_bytes=illegal;
 evolution=evolve(*decoded.record,skipped,w.View(),after_workspace.View());
 Check(!evolution.ok()&&evolution.error==E::invalid_transition,"otherwise valid common revision cannot skip a verification phase");
 auto changed=next_state;changed.evidence_policy_uuid=Id(112);const auto changed_bytes=Oracle(changed);
 auto replaced=*later_view.record;replaced.operation_state_bytes=changed_bytes;
 evolution=evolve(*decoded.record,replaced,w.View(),after_workspace.View());
 Check(!evolution.ok()&&evolution.error==E::immutable_field,"common evolution cannot replace typed policy binding");
 const auto original_later=later.bytes;auto cross=w.View();cross.objects={reinterpret_cast<Uuid*>(const_cast<byte*>(later_view.record->operation_state_bytes.data())),2};
 evolution=evolve(*decoded.record,*later_view.record,cross,after_workspace.View());
 Check(!evolution.ok()&&evolution.error==E::invalid_workspace&&later.bytes==original_later,"first endpoint scratch cannot overwrite the second endpoint");
 evolution=evolve(*decoded.record,*later_view.record,w.View(),w.View());
 Check(!evolution.ok()&&evolution.error==E::invalid_workspace,"two typed-state workspaces cannot alias");
 evolution=evolve(*decoded.record,*later_view.record,w.View(),after_workspace.View(),later.bytes.size()-1);
 Check(!evolution.ok()&&evolution.error==E::resource_exhausted,"both complete operation sizes bounded before evolution");
 // Fixture views exercise family-history validation; these representational
 // declarations never authorize custody, policy activation or readiness.
 std::array<db::NativeManagementHistoryEntryView,3> entries;
 entries[0].record=*decoded.record;entries[0].aggregate=encoded.bytes;
 entries[2].record=*later_view.record;entries[2].aggregate=later.bytes;
 auto other_state=f.state;other_state.operation_uuid=Id(120);const auto other_bytes=Oracle(other_state);
 entries[1].record=*decoded.record;entries[1].record.uuid=other_state.operation_uuid;
 entries[1].record.idempotency_key="other-verify";entries[1].record.operation_state_bytes=other_bytes;
 db::NativeManagementHistoryView history;history.error=db::NativeManagementHistoryError::none;
 history.anchor=db::NativeManagementCheckpointAnchor{};history.selection=db::NativeCheckpointSelection{};
 history.selection->header.database_uuid=f.state.database_uuid;history.entries=entries;
 std::array<byte,8192> scratch;
 const auto inspect=[&](auto backing,u64 budget=8192){return NoHeap([&]{return v::VerifyNativeVerificationHistory(history,budget,backing);});};
 auto observed=inspect(std::span(scratch));
 Check(observed.ok()&&observed.verified_operations==2&&observed.verified_revisions==3&&observed.workspace_bytes>0,
     "interleaved complete histories retain independent phase predecessors without heap fallback");
 auto refused=inspect(std::span(scratch).first(1));
 Check(!refused.ok()&&refused.error==v::NativeVerificationHistoryError::resource_exhausted&&!refused.selection&&!refused.verified_revisions,
     "exhausted bounded history scratch exposes no partial completion");
 refused=inspect(std::span(scratch),encoded.bytes.size()-1);
 Check(!refused.ok()&&refused.error==v::NativeVerificationHistoryError::resource_exhausted,"history bounds each enclosing aggregate");
 for(unsigned mutation=0;mutation<6;++mutation){auto saved=entries[2].record;
  switch(mutation){
   case 0:entries[2].record=skipped;break;
   case 1:entries[2].record=replaced;break;
   case 2:entries[2].record=rewound;break;
   case 3:entries[2].record.operation_state_bytes={};break;
   case 4:entries[2].record.database_uuid=Id(125);break;
   case 5:entries[2].record.operation_state_bytes=other_bytes;break;
  }
  refused=inspect(std::span(scratch));
  Check(!refused.ok()&&!refused.selection&&refused.failed_operation_uuid==entries[2].record.uuid,
      "retained history refuses phase skips, policy changes, revision rewind, dropped family and source rebinding");
  entries[2].record=saved;
 }
 history.entries=std::span(entries).subspan(2);
 refused=inspect(std::span(scratch));
 Check(!refused.ok()&&refused.error==v::NativeVerificationHistoryError::invalid_origin,"history cannot begin with a later revision");
 history.entries=entries;
 const auto alias_backing=[](auto span){return std::span<byte>{reinterpret_cast<byte*>(const_cast<std::remove_const_t<typename decltype(span)::element_type>*>(span.data())),span.size_bytes()};};
 std::array<std::span<const byte>,1> images{std::span<const byte>{encoded.bytes}};
 entries[0].control_inventory_images=images;
 for(auto backing:{alias_backing(history.entries),alias_backing(entries[0].aggregate),alias_backing(entries[2].record.operation_state_bytes),alias_backing(std::span(images))}){
  refused=inspect(backing);
  Check(!refused.ok()&&refused.error==v::NativeVerificationHistoryError::invalid_workspace,"history scratch excludes all retained aggregate and image-index storage");
 }
 entries[0].control_inventory_images={};
 history.entries={};observed=inspect(std::span(scratch));
 Check(observed.ok()&&!observed.verified_operations&&!observed.verified_revisions,"empty history does not synthesize a verification operation");
}
}
int main(){try{ShapeAndFraming();Bounds();Evolution();AggregateBinding();StartupRequest();StartupDetail();AdmissionMarker();DetailAggregateHistory();std::cout<<"native verification operation PASS checks="<<checks<<" representation_only=true\n";return 0;}
 catch(const std::exception& e){deny_allocations=false;std::cerr<<"native verification operation FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}

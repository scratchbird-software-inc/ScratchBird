// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/internal_api/security/native_policy_activation_history.hpp"
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
namespace cat=scratchbird::core::catalog;
using namespace scratchbird::core::platform;
using S=v::NativePolicyActivationState;using E=v::NativePolicyActivationStateError;
using A=v::NativePolicyActivationAction;using B=cat::CatalogPolicyActivation;
using Bytes=std::array<byte,256>;
unsigned checks=0;
void Check(bool condition,const char* reason,std::source_location loc=std::source_location::current()){
 ++checks;if(!condition)throw std::runtime_error(std::string(reason)+" line="+std::to_string(loc.line()));
}
template<class Call>auto NoHeap(Call&& call){allocation_attempts=0;deny_allocations=true;auto result=call();deny_allocations=false;
 Check(!allocation_attempts,"bounded policy intent validation performs no allocation");return result;}
Uuid Id(unsigned n){Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[14]=n>>8;id.bytes[15]=n;return id;}
S State(){S s;s.operation_uuid=Id(1);s.database_uuid=Id(2);s.policy_uuid=Id(3);s.policy_version_uuid=Id(4);
 s.body_uuid=Id(5);s.boundary_uuid=Id(6);s.creating_transaction_uuid=Id(7);
 s.policy_generation=1;s.body_generation=3;s.sequence=1;s.creating_local_transaction_id=20;return s;}
void Number(Bytes& out,std::size_t at,unsigned width,u64 n){for(unsigned i=0;i<width;++i)out[at+i]=byte(n>>(8*i));}
void Identity(Bytes& out,std::size_t at,const Uuid& id){std::copy(id.bytes.begin(),id.bytes.end(),out.begin()+at);}
Bytes Oracle(const S& s){Bytes out{};std::copy_n("SBPA0001",8,out.begin());Number(out,8,2,1);Number(out,10,2,256);Number(out,12,4,256);
 Identity(out,16,s.operation_uuid);Identity(out,32,s.database_uuid);Identity(out,48,s.policy_uuid);Identity(out,64,s.policy_version_uuid);
 Identity(out,80,s.body_uuid);Identity(out,96,s.boundary_uuid);Identity(out,112,s.creating_transaction_uuid);
 Identity(out,128,s.predecessor_uuid);Identity(out,144,s.cluster_uuid);
 Number(out,160,8,s.policy_generation);Number(out,168,8,s.body_generation);Number(out,176,8,s.sequence);
 Number(out,184,8,s.creating_local_transaction_id);Number(out,192,8,s.cluster_epoch.value_or(0));
 Number(out,200,2,u16(s.action));Number(out,202,2,u16(s.boundary));Number(out,204,4,s.cluster_epoch.has_value());return out;}
void Empty(const v::NativePolicyActivationStateImage& value){Check(!value.ok()&&!value.record&&value.bytes.empty(),"refusal exposes no prefix or record");}
void Roundtrip(const S& s){const auto expected=Oracle(s);std::array<byte,270> output;output.fill(0xa7);
 const auto encoded=NoHeap([&]{return v::EncodeNativePolicyActivationStateInto(s,256,std::span(output).subspan(1));});
 Check(encoded.ok()&&encoded.bytes.size()==256&&std::equal(expected.begin(),expected.end(),encoded.bytes.begin()),"complete independent byte oracle");
 Check(output.front()==0xa7&&std::all_of(output.begin()+257,output.end(),[](byte b){return b==0xa7;}),"unaligned output preserves surrounding bytes");
 const auto decoded=NoHeap([&]{return v::DecodeNativePolicyActivationState(expected,256);});
 Check(decoded.ok()&&*decoded.record==s,"all fields round trip");}
void Shape(){auto state=State();Roundtrip(state);
 for(unsigned boundary=1;boundary<=6;++boundary){auto s=state;s.boundary=B(boundary);if(boundary==6){s.cluster_uuid=Id(8);s.cluster_epoch=0;}Roundtrip(s);}
 auto cluster=state;cluster.cluster_uuid=Id(8);cluster.cluster_epoch=0;Roundtrip(cluster);
 cluster.cluster_epoch=UINT64_MAX;Roundtrip(cluster);auto transaction=state;transaction.boundary_uuid=transaction.creating_transaction_uuid;Roundtrip(transaction);
 auto revoke=state;revoke.sequence=2;revoke.predecessor_uuid=Id(9);revoke.action=A::revoke;Roundtrip(revoke);
 const auto reject=[&](const S& s){Bytes output;output.fill(0xab);const auto before=output;
  Empty(NoHeap([&]{return v::EncodeNativePolicyActivationStateInto(s,256,output);}));Check(output==before,"invalid intent cannot write output");
  const auto bytes=Oracle(s);Empty(NoHeap([&]{return v::DecodeNativePolicyActivationState(bytes,256);}));};
 for(unsigned field=0;field<24;++field){auto s=state;switch(field){
  case 0:s.operation_uuid={};break;case 1:s.database_uuid.bytes[6]=0x40;break;case 2:s.policy_uuid.bytes[8]=0;break;
  case 3:s.policy_version_uuid=s.policy_uuid;break;case 4:s.body_uuid=s.database_uuid;break;
  case 5:s.boundary_uuid=s.operation_uuid;break;case 6:s.creating_transaction_uuid=s.body_uuid;break;
  case 7:s.boundary=B::new_connection;s.boundary_uuid=s.creating_transaction_uuid;break;
  case 8:s.policy_generation=0;break;case 9:s.body_generation=0;break;case 10:s.sequence=0;break;case 11:s.creating_local_transaction_id=0;break;
  case 12:s.action=A(0);break;case 13:s.boundary=B(7);break;case 14:s.cluster_epoch=0;break;
  case 15:s.cluster_uuid=Id(8);break;case 16:s.boundary=B::cluster_epoch;break;case 17:s.predecessor_uuid=Id(9);break;
  case 18:s.sequence=2;break;case 19:s.action=A::revoke;break;
  case 20:s.cluster_uuid=s.body_uuid;s.cluster_epoch=0;break;case 21:s.predecessor_uuid=s.operation_uuid;s.sequence=2;break;
  case 22:s.predecessor_uuid=s.boundary_uuid;s.sequence=2;break;case 23:s.cluster_uuid=s.predecessor_uuid=Id(9);s.cluster_epoch=0;s.sequence=2;break;
 }reject(s);}
 const auto raw=Oracle(state);
 for(std::size_t size=0;size<256;++size)Empty(NoHeap([&]{return v::DecodeNativePolicyActivationState(std::span(raw).first(size),256);}));
 for(std::size_t at:{0u,7u,8u,10u,12u,204u,208u,255u}){auto bytes=raw;bytes[at]^=0xff;Empty(NoHeap([&]{return v::DecodeNativePolicyActivationState(bytes,256);}));}
 auto absent=raw;Number(absent,192,8,1);Empty(NoHeap([&]{return v::DecodeNativePolicyActivationState(absent,256);}));
 std::array<byte,257> extra{};std::copy(raw.begin(),raw.end(),extra.begin());Empty(v::DecodeNativePolicyActivationState(extra,257));
 Bytes output;output.fill(0xa9);const auto before=output;
 for(u64 budget:{0u,255u}){auto result=NoHeap([&]{return v::EncodeNativePolicyActivationStateInto(state,budget,output);});
  Check(result.error==E::resource_exhausted&&output==before,"budget checked before writes");Empty(result);
  result=NoHeap([&]{return v::DecodeNativePolicyActivationState(raw,budget);});Check(result.error==E::resource_exhausted,"decode budget");Empty(result);}
 const auto alias=std::span<byte>{reinterpret_cast<byte*>(&state),sizeof(state)};
 const auto bad=NoHeap([&]{return v::EncodeNativePolicyActivationStateInto(state,256,alias);});
 Check(bad.error==E::invalid_workspace&&Oracle(state)==raw,"output cannot alias immutable source");
}
S Successor(const S& before){auto s=before;s.operation_uuid=Id(10);s.predecessor_uuid=before.operation_uuid;++s.sequence;
 s.creating_transaction_uuid=Id(11);s.creating_local_transaction_id=1;return s;}
void Evolution(){const auto before=State();auto after=Successor(before);
 Check(NoHeap([&]{return v::ValidateNativePolicyActivationSuccessor(before,after);})==E::none,"creation transaction numbers do not impose commit order");
 after.action=A::revoke;Check(v::ValidateNativePolicyActivationSuccessor(before,after)==E::none,"explicit revocation of active tuple");
 auto reactivated=Successor(after);reactivated.operation_uuid=Id(12);reactivated.creating_transaction_uuid=Id(13);reactivated.creating_local_transaction_id=2;reactivated.action=A::activate;
 Check(v::ValidateNativePolicyActivationSuccessor(after,reactivated)==E::none,"reactivation follows selected revocation");
 reactivated.action=A::revoke;Check(v::ValidateNativePolicyActivationSuccessor(after,reactivated)==E::invalid_predecessor,"revoked tuple cannot be revoked again");
 for(unsigned field=0;field<12;++field){auto s=Successor(before);switch(field){case 0:s.predecessor_uuid=Id(99);break;case 1:s.sequence=3;break;
  case 2:s.database_uuid=Id(99);break;case 3:s.policy_uuid=Id(99);break;case 4:s.boundary_uuid=Id(99);break;case 5:s.boundary=B::new_statement;break;
  case 6:s.body_generation++;break;case 7:s.policy_version_uuid=Id(99);break;case 8:s.policy_generation++;break;
  case 9:s.cluster_uuid=Id(99);s.cluster_epoch=0;break;
  case 10:s.creating_transaction_uuid=before.creating_transaction_uuid;break;
  case 11:s.creating_local_transaction_id=before.creating_local_transaction_id;break;}
  Check(v::ValidateNativePolicyActivationSuccessor(before,s)==E::invalid_predecessor,"incorrect predecessor or tuple refused");}
 auto replacement=Successor(before);replacement.policy_generation++;replacement.policy_version_uuid=Id(14);replacement.body_uuid=Id(15);replacement.body_generation=1;
 Check(v::ValidateNativePolicyActivationSuccessor(before,replacement)==E::none,"new version can explicitly select a replacement body");
 replacement.action=A::revoke;Check(v::ValidateNativePolicyActivationSuccessor(before,replacement)==E::invalid_predecessor,"revocation must match the selected tuple");
 auto last=before;last.sequence=UINT64_MAX;last.predecessor_uuid=Id(99);
 Check(v::ValidateNativePolicyActivationSuccessor(last,Successor(last))!=E::none,"sequence cannot wrap");
}
void Descriptor(){const auto s=State();cat::CatalogPolicyDescriptor p;p.policy_uuid=s.policy_uuid;p.policy_version_uuid=s.policy_version_uuid;
 p.database_uuid=s.database_uuid;p.body_uuid=s.body_uuid;p.body_generation=s.body_generation;p.generation=s.policy_generation;
 p.policy_class=cat::CatalogPolicyClass::transaction;p.scope=cat::CatalogPolicyScope::database;p.scope_uuid=s.database_uuid;
 p.authority_owner_uuid=Id(30);p.precedence_rank=1;p.activation_boundary=s.boundary;p.enforcement_points=1;
 p.creating_transaction_number=p.origin_local_transaction_id=1;
 p.origin_transaction_uuid=p.creating_transaction_uuid={UuidKind::transaction,Id(31)};
 Check(NoHeap([&]{return v::BindNativePolicyActivationDescriptor(s,p);})==E::none,"exact valid descriptor tuple binds");
 for(unsigned field=0;field<10;++field){auto d=p;switch(field){case 0:d.policy_uuid=Id(99);break;case 1:d.policy_version_uuid=Id(99);break;
  case 2:d.body_uuid=Id(99);break;case 3:d.body_generation++;break;case 4:d.generation++;break;case 5:d.database_uuid=d.scope_uuid=Id(99);break;
  case 6:d.activation_boundary=B::new_connection;break;case 7:d.cluster_uuid=Id(99);break;case 8:d.enforcement_points=0;break;case 9:d.precedence_rank=0;break;}
  Check(v::BindNativePolicyActivationDescriptor(s,d)==E::binding_mismatch,"mismatched or invalid descriptor cannot bind");}
}
db::NativeManagementOperation Aggregate(const S& state){db::NativeManagementOperation r;
 r.uuid=state.operation_uuid;r.database_uuid=state.database_uuid;r.target_uuid=state.policy_uuid;r.boundary_uuid=state.boundary_uuid;
 r.cluster_uuid=state.cluster_uuid;r.scope=state.cluster_uuid.is_nil()?db::NativeManagementScope::local_database:db::NativeManagementScope::cluster;
 r.generation_guards[3]=state.cluster_epoch;r.evidence_required=true;r.bootstrap_uuid=Id(100);r.descriptor_uuid=Id(101);
 r.family_uuid=Id(102);r.target_type_uuid=Id(103);r.initiator_uuid=Id(105);r.request_context_uuid=Id(106);r.policy_snapshot_uuid=Id(107);
 r.created_at=Id(108);r.updated_at=Id(109);r.initiator_kind=4;r.idempotency_key="policy-activation-fixture";
 const auto bytes=Oracle(state);r.normalized_request_bytes.assign(bytes.begin(),bytes.end());r.operation_state_bytes=r.normalized_request_bytes;
 r.normalized_request_sha256=scratchbird::core::hash::ComputeSha256DigestNative(r.normalized_request_bytes.data(),r.normalized_request_bytes.size()).digest;
 return r;}
void Aggregates(){auto state=State();auto first=Aggregate(state);const auto image=db::EncodeNativeManagementOperation(first,8192);Check(image.ok(),"valid V3 fixture");
 std::array<db::NativeManagementStepView,1> steps,later_steps;std::array<Uuid,7> ids,later_ids;
 const auto view=db::DecodeNativeManagementOperationInto(image.bytes,8192,{steps,ids});Check(view.ok(),"aggregate seal validated");
 const auto read=[&](const auto& r,u64 budget=8192){return NoHeap([&]{return v::ReadNativePolicyActivationStateFromOperationInto(r,budget,ids);});};
 Check(read(*view.record,image.bytes.size()).ok(),"exact full aggregate budget accepted");
 const auto untouched=ids;auto bad=read(*view.record,image.bytes.size()-1);Empty(bad);
 Check(bad.error==E::resource_exhausted&&ids==untouched,"whole aggregate bounded before hash or scratch writes");
 for(unsigned field=0;field<8;++field){auto altered=*view.record;switch(field){case 0:altered.uuid=Id(99);break;case 1:altered.database_uuid=Id(99);break;
  case 2:altered.target_uuid=Id(99);break;case 3:altered.boundary_uuid=Id(99);break;case 4:altered.evidence_required=false;break;
  case 5:altered.normalized_request_sha256[0]^=1;break;case 6:altered.operation_state_bytes={};break;
  case 7:altered.scope=db::NativeManagementScope::cluster;altered.cluster_uuid=Id(99);altered.generation_guards[3]=0;break;}
  Empty(read(altered));}
 auto mismatch=first;mismatch.normalized_request_bytes[255]=1;
 mismatch.normalized_request_sha256=scratchbird::core::hash::ComputeSha256DigestNative(mismatch.normalized_request_bytes.data(),mismatch.normalized_request_bytes.size()).digest;
 auto mismatch_image=db::EncodeNativeManagementOperation(mismatch,8192);Check(mismatch_image.ok(),"individually valid common request");
 auto mismatch_view=db::DecodeNativeManagementOperationInto(mismatch_image.bytes,8192,{later_steps,later_ids});Check(mismatch_view.ok(),"altered request correctly sealed");
 Check(read(*mismatch_view.record).error==E::binding_mismatch,"request must equal the complete typed intent");
 auto later=first;later.revision++;later.updated_at=Id(110);later.state=db::NativeManagementState::authorized;
 later.security_snapshot_uuid=Id(111);later.generation_guards={1,1,1,std::nullopt};
 const auto next=db::EncodeNativeManagementOperation(later,8192);Check(next.ok(),"valid next common revision");
 const auto next_view=db::DecodeNativeManagementOperationInto(next.bytes,8192,{later_steps,later_ids});Check(next_view.ok(),"next aggregate seal");
 const auto evolve=[&](const auto& a,const auto& b,u64 budget=8192){return NoHeap([&]{return v::ValidateNativePolicyActivationManagementEvolution(a,b,budget,ids);});};
 Check(evolve(*view.record,*next_view.record).ok()&&evolve(*view.record,*view.record).ok(),"common lifecycle advances while intent is immutable; exact replay accepted");
 auto skipped=*next_view.record;skipped.revision++;
 Check(!evolve(*view.record,skipped).ok(),"family cannot hide a skipped aggregate revision");
 auto replaced=*next_view.record;auto changed=state;changed.body_generation++;const auto changed_bytes=Oracle(changed);replaced.operation_state_bytes=changed_bytes;
 Check(!evolve(*view.record,replaced).ok(),"individually valid replacement intent cannot replace original request");
 auto alias=std::span<Uuid>{reinterpret_cast<Uuid*>(const_cast<byte*>(view.record->normalized_request_bytes.data())),7};
 const auto original=image.bytes;
 Check(v::ReadNativePolicyActivationStateFromOperationInto(*view.record,8192,alias).error==E::invalid_workspace&&image.bytes==original,"aggregate scratch cannot overwrite immutable request");
 Check(v::ValidateNativePolicyActivationManagementEvolution(*next_view.record,*view.record,8192,alias).error==E::invalid_workspace&&image.bytes==original,"both endpoints checked for scratch overlap before mutation");
 state.cluster_uuid=Id(8);state.cluster_epoch=0;auto clustered=Aggregate(state);auto cluster_image=db::EncodeNativeManagementOperation(clustered,8192);
 Check(cluster_image.ok(),"present zero cluster epoch preserved in common aggregate");
 auto cluster_view=db::DecodeNativeManagementOperationInto(cluster_image.bytes,8192,{later_steps,later_ids});Check(cluster_view.ok()&&read(*cluster_view.record).ok(),"exact cluster binding accepted");
}
void PredecessorHistory(){
 const auto initial=State();auto revoked=Successor(initial);revoked.action=A::revoke;
 auto reactivated=Successor(revoked);reactivated.operation_uuid=Id(12);reactivated.creating_transaction_uuid=Id(13);
 reactivated.creating_local_transaction_id=2;reactivated.action=A::activate;
 auto unrelated=State();unrelated.operation_uuid=Id(50);unrelated.policy_uuid=Id(51);
 auto revised=Aggregate(initial);revised.revision=2;revised.updated_at=Id(110);
 revised.state=db::NativeManagementState::authorized;revised.security_snapshot_uuid=Id(111);revised.generation_guards={1,1,1,std::nullopt};
 const std::array<db::NativeManagementOperation,5> baseline{Aggregate(initial),Aggregate(unrelated),revised,Aggregate(revoked),Aggregate(reactivated)};
 const auto inspect=[&](const auto& records){
  std::array<db::NativeManagementOperationImage,5> images;
  std::array<std::array<db::NativeManagementStepView,1>,5> steps;
  std::array<std::array<Uuid,7>,5> ids;
  std::array<db::NativeManagementHistoryEntryView,5> entries;
  for(unsigned i=0;i<records.size();++i){images[i]=db::EncodeNativeManagementOperation(records[i],8192);
   Check(images[i].ok(),"each predecessor test aggregate is independently well formed");
   const auto decoded=db::DecodeNativeManagementOperationInto(images[i].bytes,8192,{steps[i],ids[i]});Check(decoded.ok(),"actual sealed predecessor aggregate");
   entries[i].record=*decoded.record;entries[i].aggregate=images[i].bytes;
  }
  db::NativeManagementHistoryView history;history.error=db::NativeManagementHistoryError::none;history.anchor.emplace();history.selection.emplace();
  history.selection->header.database_uuid=initial.database_uuid;history.entries=std::span(entries).first(records.size());
  std::array<byte,16384> scratch;
  return NoHeap([&]{return v::VerifyNativePolicyActivationHistory(history,8192,scratch);});
 };
 const auto result=inspect(baseline);
 Check(result.ok()&&result.verified_operations==4&&result.verified_revisions==5,
   "retained interleaved predecessors resolve through immutable common revisions, revocation and reactivation");
 for(unsigned field=0;field<12;++field){auto state=revoked;switch(field){
  case 0:state.predecessor_uuid=Id(99);break;case 1:state.sequence=3;break;
  case 2:state.policy_uuid=Id(90);break;case 3:++state.body_generation;break;
  case 4:state.policy_version_uuid=Id(90);break;case 5:state.boundary_uuid=Id(90);break;
  case 6:state.boundary=B::new_statement;break;case 7:state.cluster_uuid=Id(90);state.cluster_epoch=0;break;
  case 8:state.predecessor_uuid=reactivated.operation_uuid;break;case 9:state.predecessor_uuid=unrelated.operation_uuid;break;
  case 10:state.creating_transaction_uuid=initial.creating_transaction_uuid;break;
  case 11:state.creating_local_transaction_id=initial.creating_local_transaction_id;break;}
  auto records=baseline;records[3]=Aggregate(state);const auto bad=inspect(records);
  Check(!bad.ok()&&bad.error==v::NativePolicyActivationHistoryError::invalid_predecessor&&
      bad.failed_operation_uuid==revoked.operation_uuid&&!bad.selection&&!bad.verified_operations&&!bad.verified_revisions,
      "individually valid orphan, future, foreign, skipped or inconsistent predecessor intent cannot pass retained history");
 }
 std::array missing{baseline[1],baseline[3],baseline[4]};
 Check(inspect(missing).error==v::NativePolicyActivationHistoryError::invalid_predecessor,"omitting the predecessor retains no partial chain");
 std::array forward{baseline[3],baseline[0]};
 Check(inspect(forward).error==v::NativePolicyActivationHistoryError::invalid_predecessor,"future predecessor cannot justify earlier publication");
 auto ordinary=baseline[0];ordinary.operation_state_bytes.clear();std::array wrong_family{ordinary,baseline[3]};
 Check(inspect(wrong_family).error==v::NativePolicyActivationHistoryError::invalid_predecessor,"ordinary operation UUID is not a policy predecessor");
 auto replacement=reactivated;replacement.policy_generation=2;replacement.policy_version_uuid=Id(80);replacement.body_uuid=Id(81);replacement.body_generation=1;
 auto replaced=baseline;replaced[4]=Aggregate(replacement);Check(inspect(replaced).ok(),"new version with explicit replacement body resolves its actual retained predecessor");
}
void History(){auto first=Aggregate(State());auto later=first;later.revision=2;later.updated_at=Id(110);
 later.state=db::NativeManagementState::authorized;later.security_snapshot_uuid=Id(111);later.generation_guards={1,1,1,std::nullopt};
 auto second_state=State();second_state.operation_uuid=Id(50);second_state.policy_uuid=Id(51);auto second=Aggregate(second_state);
 std::array<db::NativeManagementOperation,3> records{first,second,later};
 std::array<db::NativeManagementOperationImage,3> images;
 std::array<std::array<db::NativeManagementStepView,1>,3> steps;
 std::array<std::array<Uuid,7>,3> ids;
 std::array<db::NativeManagementHistoryEntryView,3> entries;
 for(unsigned i=0;i<3;++i){images[i]=db::EncodeNativeManagementOperation(records[i],8192);Check(images[i].ok(),"history aggregate encoded");
  const auto decoded=db::DecodeNativeManagementOperationInto(images[i].bytes,8192,{steps[i],ids[i]});Check(decoded.ok(),"history aggregate seal");
  entries[i].record=*decoded.record;entries[i].aggregate=images[i].bytes;}
 db::NativeManagementHistoryView history;history.error=db::NativeManagementHistoryError::none;history.anchor.emplace();history.selection.emplace();
 history.selection->header.database_uuid=first.database_uuid;history.entries=entries;
 std::array<byte,16384> scratch;
 const auto inspect=[&](auto backing,u64 budget=8192){return NoHeap([&]{return v::VerifyNativePolicyActivationHistory(history,budget,backing);});};
 auto result=inspect(std::span(scratch));
 Check(result.ok()&&result.verified_operations==2&&result.verified_revisions==3,"interleaved policy histories preserve immutable per-operation intent");
 auto refused=inspect(std::span(scratch).first(1));
 Check(!refused.ok()&&refused.error==v::NativePolicyActivationHistoryError::resource_exhausted&&!refused.selection&&!refused.verified_revisions,"bounded workspace refusal exposes no partial selection");
 refused=inspect(std::span(scratch),images[0].bytes.size()-1);
 Check(!refused.ok()&&refused.error==v::NativePolicyActivationHistoryError::resource_exhausted,"whole aggregate budget checked before family history validation");
 for(unsigned field=0;field<5;++field){const auto saved=entries[2].record;switch(field){
  case 0:entries[2].record.revision=3;break;case 1:entries[2].record.operation_state_bytes={};break;
  case 2:entries[2].record.database_uuid=Id(99);break;case 3:entries[2].record.boundary_uuid=Id(99);break;
  case 4:entries[2].record.operation_state_bytes=entries[1].record.operation_state_bytes;break;}
  refused=inspect(std::span(scratch));Check(!refused.ok()&&!refused.selection&&refused.failed_operation_uuid==first.uuid,"broken history cannot select partial valid prefix");
  entries[2].record=saved;}
 history.entries=std::span(entries).subspan(2);refused=inspect(std::span(scratch));
 Check(!refused.ok()&&refused.error==v::NativePolicyActivationHistoryError::invalid_origin,"history cannot start at an authorized revision");history.entries=entries;
 const auto alias=[](auto span){return std::span<byte>{reinterpret_cast<byte*>(const_cast<std::remove_const_t<typename decltype(span)::element_type>*>(span.data())),span.size_bytes()};};
 std::array<std::span<const byte>,1> nested{std::span<const byte>{images[1].bytes}};entries[1].control_inventory_images=nested;
 for(auto backing:{alias(history.entries),alias(entries[0].aggregate),alias(entries[2].record.operation_state_bytes),alias(std::span(nested)),alias(nested[0])}){
  refused=inspect(backing);Check(!refused.ok()&&refused.error==v::NativePolicyActivationHistoryError::invalid_workspace,"history scratch excludes every nested immutable source");}
 history.entries={};result=inspect(std::span(scratch));Check(result.ok()&&!result.verified_operations&&!result.verified_revisions,"empty history cannot invent an activation");
}
}
int main(){try{Shape();Evolution();Descriptor();Aggregates();History();PredecessorHistory();std::cout<<checks<<" policy activation intent checks passed (no activation authority claimed)\n";return 0;}
 catch(const std::exception& e){deny_allocations=false;std::cerr<<e.what()<<'\n';return 1;}}

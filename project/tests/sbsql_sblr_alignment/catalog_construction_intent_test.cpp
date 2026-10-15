// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_construction_intent.hpp"
#include "native_construction_audit.hpp"
#include "native_creation_workspace.hpp"
#include "native_creation_workspace_recovery.hpp"
#include "physical_mga_cow_store.hpp"
#include "bootstrap_policy_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

thread_local bool heap_denied=false;
void* operator new(std::size_t size){if(heap_denied)throw std::bad_alloc();if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace c=scratchbird::core::catalog;namespace p=scratchbird::core::platform;
namespace f=scratchbird::tests::bootstrap_policy;namespace db=scratchbird::storage::database;
namespace d=scratchbird::storage::disk;namespace u=scratchbird::core::uuid;namespace mga=scratchbird::transaction::mga;
#if defined(SB_CONSTRUCTION_AUDIT_CLOCK_WRAP)
namespace t=scratchbird::core::time;
thread_local unsigned clock_mode=0,clock_calls=0;
extern "C" t::ClockSnapshotResult __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
extern "C" t::ClockSnapshotResult __wrap__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv(){
  ++clock_calls;if(!clock_mode)return __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
  t::ClockSnapshotResult out;
  if(clock_mode==1||(clock_mode==4&&clock_calls==2)){
    out.status={p::StatusCode::time_source_unavailable,p::Severity::error,p::Subsystem::time};return out;
  }
  out.value={{clock_calls},{clock_mode==2?-1:clock_mode==3?0:1789357072,clock_mode==3?0U:123456789U}};
  return out;
}
#endif
namespace {
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok){heap_denied=false;throw std::runtime_error(why);}}
c::CatalogConstructionIntent Value(){
  c::CatalogConstructionIntent r;
  r.event_uuid=f::Id(100);r.database_uuid=f::Id(1);r.engine_uuid=f::Id(6);r.operation_uuid=f::Id(3);r.filespace_uuid=f::Id(2);
  r.origin_transaction_uuid={p::UuidKind::transaction,f::Id(5)};r.origin_local_transaction_id=1;
  r.event_time_utc_us=1789357072123456ULL;r.catalog_generation=1;
  r.event=c::ConstructionAuditEvent::construction_intent;r.event_class=c::ConstructionAuditClass::bootstrap_construction;
  r.policy_decision=c::ConstructionAuditDecision::not_evaluated;r.redaction=c::ConstructionAuditRedaction::safe_message_only;return r;
}
// Independent field assembly freezes schema/field tags and wire numbers. It
// does not use the production schema, encoder, enums or size calculator.
std::string Golden(const c::CatalogConstructionIntent& r){
  f::Golden g(65618);g.Identity(1,r.event_uuid);g.Identity(2,r.database_uuid);g.Identity(3,r.engine_uuid);
  g.Identity(4,r.operation_uuid);g.Identity(5,r.filespace_uuid);g.Identity(6,r.origin_transaction_uuid.value);
  g.Integer(7,r.origin_local_transaction_id);g.Integer(8,r.event_time_utc_us);g.Integer(9,r.catalog_generation);
  g.Integer(10,static_cast<p::u64>(r.event));g.Integer(11,static_cast<p::u64>(r.event_class));
  g.Integer(12,static_cast<p::u64>(r.policy_decision));g.Integer(13,static_cast<p::u64>(r.redaction));
  if(r.actor_uuid)g.Identity(14,r.actor_uuid->value);return g.Finish();
}
c::CatalogMetadataVersion Metadata(const c::CatalogConstructionIntent& r,p::Uuid row=f::Id(101)){
  auto m=f::Metadata(0,r.origin_transaction_uuid,f::Id(60),Golden(r));m.record.header.kind=c::CatalogRecordKind::audit_evidence;
  m.record.header.object_uuid={p::UuidKind::object,r.event_uuid};m.record.header.row_uuid={p::UuidKind::row,row};
  m.object_subtype="node_construction_intent";m.audit_uuid=m.record.header.object_uuid;
  m.default_name_uuid={};m.name_vector_uuid={};return m;
}
void Roundtrip(const c::CatalogConstructionIntent& r){
  const auto wire=Golden(r);std::array<p::byte,400> out;out.fill(0x5a);
  heap_denied=true;const auto size=c::CatalogConstructionIntentEncodedBytes(r);
  const auto encoded=c::EncodeCatalogConstructionIntentInto(r,out);const auto decoded=c::DecodeCatalogConstructionIntent(wire);heap_denied=false;
  Check(size&&*size==wire.size()&&encoded.ok()&&decoded.ok()&&encoded.bytes.size()==wire.size()&&
      std::equal(encoded.bytes.begin(),encoded.bytes.end(),reinterpret_cast<const p::byte*>(wire.data()))&&Golden(*decoded.record)==wire,
      "first-use no-heap exact independent complete event bytes");
  Check(wire.size()==(r.actor_uuid?304:280)&&std::all_of(out.begin()+wire.size(),out.end(),[](auto b){return b==0x5a;}),"exact native extent and untouched tail");
  for(std::size_t n=0;n<wire.size();++n){out.fill(0x5a);heap_denied=true;
    const auto bad_out=c::EncodeCatalogConstructionIntentInto(r,std::span(out).first(n));
    const auto bad_in=c::DecodeCatalogConstructionIntent(std::string_view(wire).substr(0,n));heap_denied=false;
    Check(!bad_out.ok()&&bad_out.bytes.empty()&&!bad_in.ok()&&!bad_in.record&&
        std::all_of(out.begin(),out.end(),[](auto b){return b==0x5a;}),"every short input/output refuses without heap or partial output");}
}
void Refuse(const c::CatalogConstructionIntent& r,bool wire=true){
  const auto bytes=wire?Golden(r):std::string{};std::array<p::byte,400> out;out.fill(0x5a);heap_denied=true;
  const auto size=c::CatalogConstructionIntentEncodedBytes(r);const auto encoded=c::EncodeCatalogConstructionIntentInto(r,out);
  const auto decoded=wire?c::DecodeCatalogConstructionIntent(bytes):c::CatalogConstructionIntentResult{};heap_denied=false;
  Check(!size&&!encoded.ok()&&encoded.bytes.empty()&&!decoded.ok()&&!decoded.record&&
      std::all_of(out.begin(),out.end(),[](auto b){return b==0x5a;}),"invalid native event refuses before allocation/output");
}
void Values(){
  const auto value=Value();Roundtrip(value);auto actor=value;actor.actor_uuid=p::TypedUuid{p::UuidKind::principal,f::Id(50)};Roundtrip(actor);
  for(const auto time:{p::u64{1},UINT64_MAX}){auto r=value;r.event_time_utc_us=time;Roundtrip(r);}
  for(unsigned n=0;n<19;++n){auto bad=actor;
    if(n==0)bad.event_uuid={};if(n==1)bad.database_uuid={};if(n==2)bad.engine_uuid={};if(n==3)bad.operation_uuid={};
    if(n==4)bad.filespace_uuid={};if(n==5)bad.origin_transaction_uuid.value={};if(n==6)bad.origin_transaction_uuid.kind=p::UuidKind::object;
    if(n==7)bad.origin_local_transaction_id=0;if(n==8)bad.origin_local_transaction_id=2;if(n==9)bad.event_time_utc_us=0;
    if(n==10)bad.catalog_generation=0;if(n==11)bad.catalog_generation=2;if(n==12)bad.event=static_cast<c::ConstructionAuditEvent>(0);
    if(n==13)bad.event_class=static_cast<c::ConstructionAuditClass>(2);if(n==14)bad.policy_decision=static_cast<c::ConstructionAuditDecision>(2);
    if(n==15)bad.redaction=static_cast<c::ConstructionAuditRedaction>(UINT64_MAX);if(n==16)bad.actor_uuid->value={};
    if(n==17)bad.actor_uuid->kind=p::UuidKind::object;if(n==18)bad.event_uuid.bytes[6]=0x40;
    Refuse(bad,n!=6&&n!=17);
  }
  for(unsigned i=0;i<7;++i)for(unsigned j=0;j<i;++j){auto bad=actor;
    std::array<p::Uuid*,7> ids{&bad.event_uuid,&bad.database_uuid,&bad.engine_uuid,&bad.operation_uuid,&bad.filespace_uuid,&bad.origin_transaction_uuid.value,&bad.actor_uuid->value};
    *ids[i]=*ids[j];Refuse(bad);}
  const auto original=Golden(actor);
  for(std::size_t at=24;at<original.size();){const auto length=p::LoadLittle32(original.data()+at+4);
    for(unsigned mode=0;mode<5;++mode){auto bad=original;
      if(mode==0){bad.erase(at,8+length);f::Put(bad,8,bad.size(),4);f::Put(bad,12,13,4);}
      if(mode==1)f::Put(bad,at,65000,2);if(mode==2)f::Put(bad,at+2,4,1);
      if(mode==3)f::Put(bad,at+3,1,1);if(mode==4)f::Put(bad,at+4,length-1,4);
      heap_denied=true;const auto d=c::DecodeCatalogConstructionIntent(bad);heap_denied=false;
      // Optional actor omission is the only valid missing field.
      Check((mode==0&&at+8+length==original.size())?d.ok():!d.ok(),"independent missing/unknown/type/reserved/width rejection");
    }at+=8+length;
  }
  auto corrupt=original;corrupt.push_back(0);heap_denied=true;const auto trailing=c::DecodeCatalogConstructionIntent(corrupt);heap_denied=false;
  Check(!trailing.ok(),"no trailing wire bytes");
  auto r=value;std::array<p::byte,sizeof(r)> before,after;std::memcpy(before.data(),&r,sizeof(r));
  for(std::size_t at=0;at<sizeof(r);++at){heap_denied=true;const auto encoded=c::EncodeCatalogConstructionIntentInto(r,{reinterpret_cast<p::byte*>(&r)+at,1});heap_denied=false;
    std::memcpy(after.data(),&r,sizeof(r));Check(!encoded.ok()&&before==after,"whole input object overlap refuses unchanged");}
}
void Bindings(){
  const auto v=Value();const auto m=Metadata(v);const auto view=c::BorrowCatalogMetadataVersion(m);
  heap_denied=true;const auto size=c::CatalogMetadataVersionEncodedBytes(view);heap_denied=false;
  Check(size&&c::CatalogConstructionIntentMatchesMetadata(view)&&c::CatalogConstructionIntentMatchesDatabase(view,v.database_uuid)&&
      !c::CatalogConstructionIntentMatchesDatabase(view,f::Id(200)),"native common/self-anchor/database binding");
  for(unsigned n=0;n<14;++n){auto bad=m;
    if(n==0)bad.record.header.kind=c::CatalogRecordKind::config_profile;if(n==1)bad.record.header.object_uuid.value=f::Id(200);
    if(n==2)bad.object_subtype="other";if(n==3)bad.audit_uuid.value=bad.record.header.row_uuid.value;
    if(n==4)bad.audit_uuid.value=v.operation_uuid;if(n==5)bad.creator_transaction_uuid.value=f::Id(201);
    if(n==6)bad.creator_local_transaction_id=2;if(n==7)bad.record.header.parent_uuid.value=f::Id(202);
    if(n==8)bad.owning_schema_uuid.kind=p::UuidKind::object;if(n==9)bad.authority_scope=c::CatalogAuthorityScope::cluster;
    if(n==10)f::Put(bad.record.payload,16,65617,4);if(n==11)bad.record.payload="legacy fake audit string";
    if(n==12)bad.default_name_uuid={p::UuidKind::object,f::Id(300)};if(n==13)bad.record.header.row_uuid.value=v.engine_uuid;
    heap_denied=true;const auto result=c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(bad));heap_denied=false;
    Check(!result,"common dispatcher cannot bypass exact family binding");
  }
  auto next=m;next.definition_version=2;next.creator_transaction_uuid.value=f::Id(7);next.creator_local_transaction_id=2;
  Check(c::CatalogMetadataPreservesFamilyOrigin(m,next),"later metadata may retain exact immutable intent");
  for(unsigned n=0;n<4;++n){auto changed=v;if(n==0)++changed.event_time_utc_us;if(n==1)changed.engine_uuid=f::Id(301);
    if(n==2)changed.actor_uuid=p::TypedUuid{p::UuidKind::principal,f::Id(50)};if(n==3)changed.origin_transaction_uuid.value=f::Id(302);
    auto bad=next;bad.record.payload=Golden(changed);Check(!c::CatalogMetadataPreservesFamilyOrigin(m,bad),"event cannot be rewritten by a metadata successor");}
}
db::NativeFilespaceInitializationRequest Request(const d::CanonicalFilespacePageProfile& profile){
  db::NativeFilespaceInitializationRequest r;
  r.bootstrap={f::Id(1),f::Id(2),profile.uuid,d::kNativeBootstrapIntegrityProfile,{},profile.page_size_bytes,1,0,1,7};
  r.operation_uuid=f::Id(3);r.writer_uuid=f::Id(4);r.policy_snapshot_uuid=f::Id(10);
  r.creator.transaction_uuid={p::UuidKind::transaction,f::Id(5)};r.creator.local_id=mga::MakeLocalTransactionId(1);
  r.creator.scope=mga::TransactionScope::local_node;r.total_pages=64;r.creation_utc_millis=1789357072000ULL;return r;
}
void TimeAndProducer(){
  Check(db::NativeConstructionAuditTimeMicros({1,123456789})==1123456&&
      db::NativeConstructionAuditTimeMicros({0,1000})==1,"checked actual seconds/nanoseconds to UTC microseconds");
  for(const auto t:{p::WallClockTime{-1,0},{0,0},{0,999},{1,1000000000},{INT64_MAX,0}})
    Check(!db::NativeConstructionAuditTimeMicros(t),"invalid or unrepresentable acquisition has no event time");
  constexpr auto last=UINT64_MAX/1000000,part=UINT64_MAX%1000000;
  Check(db::NativeConstructionAuditTimeMicros({static_cast<p::i64>(last),static_cast<p::u32>(part*1000+999)})==UINT64_MAX&&
      !db::NativeConstructionAuditTimeMicros({static_cast<p::i64>(last),static_cast<p::u32>((part+1)*1000)}),"exact uint64 time bound without overflow");
  const auto r=Request(d::kCanonicalFilespacePageProfiles[0]);u::StandaloneUuidV7Issuer issuer({f::Id(1),f::Id(10)},{{},0,1000});
  const auto actual=db::AcquireNativeConstructionIntent(r,f::Id(6),issuer);
  Check(actual.ok()&&actual.row_uuid.kind==p::UuidKind::row&&actual.event->event_time_utc_us==
      db::NativeConstructionAuditTimeMicros(actual.event_observation->wall_clock)&&!actual.event->actor_uuid&&
      actual.event->event_time_utc_us!=r.creation_utc_millis*1000,"producer uses actual accepted clock observation, never stored creation time or invented actor");
  auto wrong=r;wrong.bootstrap.flags|=d::FilespaceBootstrapFlag::cluster_authority_required;
  const auto cluster=db::AcquireNativeConstructionIntent(wrong,f::Id(6),issuer);
  Check(!cluster.ok()&&!cluster.event&&cluster.error==db::NativeConstructionAuditError::cluster_requires_authority,"no local-time fallback for cluster construction");
  u::StandaloneUuidV7Issuer invalid({f::Id(1),f::Id(10)},{{},0,0});const auto failed=db::AcquireNativeConstructionIntent(r,f::Id(6),invalid);
  Check(!failed.ok()&&!failed.event&&failed.identity_error==u::StandaloneUuidV7Error::invalid_policy,"actual issuer cause retained without fake record");
#if defined(SB_CONSTRUCTION_AUDIT_CLOCK_WRAP)
  for(unsigned mode=1;mode<=4;++mode){
    u::StandaloneUuidV7Issuer controlled({f::Id(1),f::Id(10)},{{},0,1000});clock_mode=mode;clock_calls=0;
    const auto fail=db::AcquireNativeConstructionIntent(r,f::Id(6),controlled);clock_mode=0;
    Check(!fail.ok()&&!fail.event&&!fail.event_observation&&!fail.row_uuid.valid()&&
        fail.error==db::NativeConstructionAuditError::time_source_failure&&clock_calls==(mode==4?2U:1U),
        "actual acquisition failure/negative/zero/second-issue failure yields no record or partial identities");
  }
  clock_calls=0;auto malformed=Value();malformed.event_time_utc_us=0;const auto wire=Golden(malformed);
  heap_denied=true;const auto persisted=c::DecodeCatalogConstructionIntent(wire);heap_denied=false;
  Check(!persisted.ok()&&!clock_calls,"persisted time rejection never reads/acquires a clock");
#endif
}
void Physical(){
  scratchbird::tests::OwnedTempDirectory artifacts;
  for(const auto& profile:d::kCanonicalFilespacePageProfiles){
    const auto request=Request(profile);u::StandaloneUuidV7Issuer issuer({f::Id(1),f::Id(10)},{{},0,1000});
    const auto acquired=db::AcquireNativeConstructionIntent(request,f::Id(6),issuer);Check(acquired.ok(),"actual owning construction issuance");
    const auto expected=Golden(*acquired.event);const auto metadata=Metadata(*acquired.event,acquired.row_uuid.value);
    db::NativeCreationCatalogSeed seed;seed[0].push_back({metadata,{}});
    const auto path=(artifacts.path()/std::to_string(profile.page_size_bytes)).string();d::FileDevice device;
    Check(device.Open(path,d::FileOpenMode::create_new).ok(),"real owned construction file");
    const auto budget=128*p::u64{profile.page_size_bytes};
    for(unsigned n=0;n<4;++n){auto wrong=*acquired.event;if(n==0)wrong.database_uuid=f::Id(400);
      if(n==1)wrong.operation_uuid=f::Id(401);if(n==2)wrong.filespace_uuid=f::Id(402);if(n==3)wrong.origin_transaction_uuid.value=f::Id(403);
      seed[0][0].metadata=Metadata(wrong,acquired.row_uuid.value);
      const auto refused=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer);
      const auto extent=device.Size();Check(!refused.ok()&&!refused.receipt&&extent.ok()&&extent.size_bytes==0,"foreign construction binding refuses before physical effects");
    }
    seed[0][0].metadata=metadata;
    const auto made=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer);
    Check(made.ok(),"actual audit row joins same native construction cohort");
    Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing).ok(),"independent actual file reopen");
    const std::vector<d::NativeFilespaceDevice> devices{{f::Id(2),profile.uuid,&device}};
    const auto rows=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,made.receipt->checkpoint,2,1,
        {made.receipt->relations[0].object_uuid,{}},budget);
    Check(rows.ok()&&rows.rows.size()==1&&rows.rows[0].metadata.record.payload==expected&&
        c::CatalogConstructionIntentMatchesMetadata(c::BorrowCatalogMetadataVersion(rows.rows[0].metadata)),"exact typed event/self-anchor persists and reopens");
    const d::FilespaceBootstrapBinding binding{request.bootstrap.database_uuid,request.bootstrap.filespace_uuid,request.bootstrap.page_size_profile_uuid};
    const auto recovered=db::RecoverPopulatedNativeCreationWorkspaceSelectionOnOpenDevice(device,binding,request.operation_uuid,seed,budget);
    Check(recovered.ok()&&!recovered.repaired_slots,"recovery validates exact retained original intent");
    // Resolve the actual second-selector root. Allocation-map count differs
    // by page profile; a constant page18 would corrupt a different dependency.
    const auto before_cut=d::ReadFilespacePageZeroFromOpenDevice(device);Check(before_cut.ok(),"actual native root directory before recovery cut");
    const auto second=std::find_if(before_cut.record->roots.begin(),before_cut.record->roots.end(),[](const auto& root){return root.kind==19;});
    Check(second!=before_cut.record->roots.end(),"actual second selector exists");
    std::vector<p::byte> missing_selector(profile.page_size_bytes,0);
    Check(device.WriteAt(second->page_number*p::u64{profile.page_size_bytes},missing_selector.data(),missing_selector.size()).ok()&&device.Sync().ok(),
        "simulate interrupted second selector with real retained first selection");
    auto changed_seed=seed;auto changed=*acquired.event;++changed.event_time_utc_us;changed_seed[0][0].metadata.record.payload=Golden(changed);
    const auto refused=db::RecoverPopulatedNativeCreationWorkspaceSelectionOnOpenDevice(device,binding,request.operation_uuid,changed_seed,budget);
    Check(!refused.ok(),"recovery cannot replace retained event with fresh intent");
    const auto repaired=db::RecoverPopulatedNativeCreationWorkspaceSelectionOnOpenDevice(device,binding,request.operation_uuid,seed,budget);
    Check(repaired.ok()&&repaired.repaired_slots==2,"original event/cohort alone repairs missing second selector");
    const auto retained=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,repaired.receipt->checkpoint,2,1,
        {repaired.receipt->relations[0].object_uuid,{}},budget);
    Check(retained.ok()&&retained.rows.size()==1&&retained.rows[0].metadata.record.payload==expected,
        "recovery retains original binary event identity/time/non-evaluation, not new success evidence");
    const auto zero=d::ReadFilespacePageZeroFromOpenDevice(device);
    Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==7,"retained event is not activation or serving proof");
    Check(device.Close().ok(),"private device released");
  }
}
}
int main()try{Values();Bindings();TimeAndProducer();Physical();std::cout<<"PASS construction intent "<<checks<<" checks\n";return 0;}
catch(const std::exception& e){heap_denied=false;std::cerr<<e.what()<<'\n';return 1;}

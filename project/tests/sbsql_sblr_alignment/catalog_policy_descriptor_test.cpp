// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_policy_descriptor.hpp"
#include "native_policy_descriptor_creation.hpp"
#include "native_creation_workspace.hpp"
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

thread_local long allocation_budget=-1;
thread_local unsigned allocation_count=0;
void* operator new(std::size_t size){++allocation_count;if(allocation_budget>=0&&allocation_budget--==0)throw std::bad_alloc();
  if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace c=scratchbird::core::catalog;namespace p=scratchbird::core::platform;
namespace f=scratchbird::tests::bootstrap_policy;namespace db=scratchbird::storage::database;
namespace d=scratchbird::storage::disk;namespace u=scratchbird::core::uuid;namespace mga=scratchbird::transaction::mga;
static_assert(static_cast<p::u64>(c::CatalogPolicyClass::donor_compatibility)==23&&
    static_cast<p::u64>(c::CatalogPolicyScope::placement_unit)==21&&
    static_cast<p::u64>(c::CatalogPolicyDefault::deny)==2&&
    static_cast<p::u64>(c::CatalogPolicyException::fail_closed)==2&&
    static_cast<p::u64>(c::CatalogPolicyRecovery::deny_on_uncertain)==4&&
    static_cast<p::u64>(c::CatalogPolicyActivation::new_transaction)==2);
static_assert([]{
  constexpr c::CatalogPolicyClass classes[]{c::CatalogPolicyClass::authentication,c::CatalogPolicyClass::authorization,c::CatalogPolicyClass::connection,
    c::CatalogPolicyClass::transaction,c::CatalogPolicyClass::statement,c::CatalogPolicyClass::parser_cache,c::CatalogPolicyClass::sblr_binding,
    c::CatalogPolicyClass::udr_trust,c::CatalogPolicyClass::dml,c::CatalogPolicyClass::read,c::CatalogPolicyClass::metadata,c::CatalogPolicyClass::lock_admission,
    c::CatalogPolicyClass::backup_restore,c::CatalogPolicyClass::archive,c::CatalogPolicyClass::cleanup_retention,c::CatalogPolicyClass::verification_repair,
    c::CatalogPolicyClass::placement_cluster,c::CatalogPolicyClass::audit,c::CatalogPolicyClass::evidence,c::CatalogPolicyClass::disclosure_redaction,
    c::CatalogPolicyClass::support,c::CatalogPolicyClass::legal_hold,c::CatalogPolicyClass::donor_compatibility};
  for(unsigned n=0;n<23;++n)if(static_cast<p::u64>(classes[n])!=n+1)return false;return true;
}());
namespace {
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok){allocation_budget=-1;throw std::runtime_error(why);}}
c::CatalogPolicyDescriptor Value(unsigned n=0){
  c::CatalogPolicyDescriptor r;r.policy_uuid=f::Id(100+n);r.policy_version_uuid=f::Id(200+n);r.database_uuid=f::Id(1);
  r.generation=1;r.policy_class=c::CatalogPolicyClass::metadata;r.creating_transaction_number=1;
  r.scope=c::CatalogPolicyScope::database;r.scope_uuid=r.database_uuid;r.authority_owner_uuid=f::Id(50);
  r.precedence_rank=3;r.enforcement_points=c::kCatalogPolicyEnforcementMask;
  r.origin_transaction_uuid=r.creating_transaction_uuid={p::UuidKind::transaction,f::Id(5)};r.origin_local_transaction_id=1;return r;
}
std::string Golden(const c::CatalogPolicyDescriptor& r){
  f::Golden g(65619);g.Identity(1,r.policy_uuid);g.Identity(2,r.policy_version_uuid);g.Identity(3,r.database_uuid);
  g.Integer(4,r.generation);g.Integer(5,static_cast<p::u64>(r.policy_class));g.Integer(6,r.creating_transaction_number);
  if(r.effective_transaction_number)g.Integer(7,*r.effective_transaction_number);
  if(r.superseding_transaction_number)g.Integer(8,*r.superseding_transaction_number);
  g.Integer(9,static_cast<p::u64>(r.scope));g.Identity(10,r.scope_uuid);g.Identity(11,r.authority_owner_uuid);
  g.Integer(12,r.precedence_rank);g.Integer(13,static_cast<p::u64>(r.default_decision));g.Integer(14,static_cast<p::u64>(r.exception_behavior));
  if(r.evidence_profile_uuid)g.Identity(15,*r.evidence_profile_uuid);if(r.disclosure_profile_uuid)g.Identity(16,*r.disclosure_profile_uuid);
  if(r.retention_profile_uuid)g.Identity(17,*r.retention_profile_uuid);
  g.Integer(18,static_cast<p::u64>(r.recovery_behavior));g.Integer(19,static_cast<p::u64>(r.activation_boundary));
  g.Integer(20,r.cache_invalidation_mask);g.Integer(21,r.enforcement_points);
  if(r.cluster_uuid)g.Identity(22,*r.cluster_uuid);g.Identity(23,r.origin_transaction_uuid.value);
  g.Integer(24,r.origin_local_transaction_id);g.Identity(25,r.creating_transaction_uuid.value);return g.Finish();
}
c::CatalogMetadataVersion Metadata(const c::CatalogPolicyDescriptor& r,unsigned n=0){
  auto m=f::Metadata(0,r.creating_transaction_uuid,f::Id(60),Golden(r));m.record.header.kind=c::CatalogRecordKind::policy;
  m.record.header.object_uuid={p::UuidKind::object,r.policy_uuid};m.record.header.row_uuid={p::UuidKind::row,f::Id(1000+3*n)};
  m.default_name_uuid={p::UuidKind::object,f::Id(2000+2*n)};m.name_vector_uuid={p::UuidKind::object,f::Id(2001+2*n)};
  m.object_subtype="policy_descriptor";m.owner_uuid.value=r.authority_owner_uuid;m.creator_local_transaction_id=r.creating_transaction_number;
  m.definition_version=r.generation;if(r.scope==c::CatalogPolicyScope::cluster_member||r.scope==c::CatalogPolicyScope::placement_unit)m.authority_scope=c::CatalogAuthorityScope::cluster;
  return m;
}
void Roundtrip(const c::CatalogPolicyDescriptor& r,bool truncations=false){
  const auto wire=Golden(r);std::array<p::byte,600> out;out.fill(0x5a);allocation_budget=0;allocation_count=0;
  const auto size=c::CatalogPolicyDescriptorEncodedBytes(r);const auto encoded=c::EncodeCatalogPolicyDescriptorInto(r,out);
  const auto decoded=c::DecodeCatalogPolicyDescriptor(wire);const auto allocs=allocation_count;allocation_budget=-1;
  Check(!allocs&&size&&*size==wire.size()&&encoded.ok()&&decoded.ok()&&encoded.bytes.size()==wire.size()&&
      std::equal(encoded.bytes.begin(),encoded.bytes.end(),reinterpret_cast<const p::byte*>(wire.data()))&&Golden(*decoded.record)==wire,
      "first-use heap-free complete independent descriptor byte oracle");
  Check(std::all_of(out.begin()+wire.size(),out.end(),[](auto b){return b==0x5a;}),"unused destination tail retained");
  if(truncations)for(std::size_t n=0;n<wire.size();++n){out.fill(0x5a);allocation_budget=0;allocation_count=0;
    const auto short_out=c::EncodeCatalogPolicyDescriptorInto(r,std::span(out).first(n));
    const auto short_in=c::DecodeCatalogPolicyDescriptor(std::string_view(wire).substr(0,n));const auto allocs=allocation_count;allocation_budget=-1;
    Check(!allocs&&!short_out.ok()&&short_out.bytes.empty()&&!short_in.ok()&&!short_in.record&&
        std::all_of(out.begin(),out.end(),[](auto b){return b==0x5a;}),"all short input/output refuses atomically without heap");}
}
void Refuse(const c::CatalogPolicyDescriptor& r,bool wire=true){
  const auto bytes=wire?Golden(r):std::string{};std::array<p::byte,600> out;out.fill(0x5a);allocation_budget=0;allocation_count=0;
  const auto size=c::CatalogPolicyDescriptorEncodedBytes(r);const auto encoded=c::EncodeCatalogPolicyDescriptorInto(r,out);
  const auto decoded=wire?c::DecodeCatalogPolicyDescriptor(bytes):c::CatalogPolicyDescriptorResult{};
  const auto allocs=allocation_count;allocation_budget=-1;
  Check(!allocs&&!size&&!encoded.ok()&&encoded.bytes.empty()&&!decoded.ok()&&!decoded.record&&
      std::all_of(out.begin(),out.end(),[](auto b){return b==0x5a;}),"invalid native descriptor refuses without partial value or writes");
}
void Values(){
  const auto v=Value();Check(v.default_decision==c::CatalogPolicyDefault::deny&&v.exception_behavior==c::CatalogPolicyException::fail_closed&&
      v.recovery_behavior==c::CatalogPolicyRecovery::deny_on_uncertain&&v.activation_boundary==c::CatalogPolicyActivation::new_transaction,
      "existing normative deny/fail-closed defaults preserved");Roundtrip(v,true);Check(Golden(v).size()==384,"independent minimum extent");
  auto all=v;all.effective_transaction_number=2;all.superseding_transaction_number=3;all.evidence_profile_uuid=f::Id(300);
  all.disclosure_profile_uuid=f::Id(301);all.retention_profile_uuid=f::Id(302);all.cluster_uuid=f::Id(303);Roundtrip(all,true);
  Check(Golden(all).size()==512,"independent maximum extent");
  for(unsigned mask=0;mask<64;++mask){auto r=v;
    if(mask&1)r.effective_transaction_number=2;if(mask&2)r.superseding_transaction_number=3;
    if(mask&4)r.evidence_profile_uuid=f::Id(300);if(mask&8)r.disclosure_profile_uuid=f::Id(301);
    if(mask&16)r.retention_profile_uuid=f::Id(302);if(mask&32)r.cluster_uuid=f::Id(303);Roundtrip(r);}
  for(unsigned policy=1;policy<=23;++policy)for(unsigned scope=1;scope<=21;++scope){auto r=v;
    r.policy_class=static_cast<c::CatalogPolicyClass>(policy);r.scope=static_cast<c::CatalogPolicyScope>(scope);
    if(scope!=1)r.scope_uuid=f::Id(400);if(scope>=20)r.cluster_uuid=f::Id(303);Roundtrip(r);}
  for(unsigned n=0;n<64;++n){auto r=v;r.cache_invalidation_mask=p::u64{1}<<n;if(n<8)Roundtrip(r);else Refuse(r);
    r=v;r.enforcement_points=p::u64{1}<<n;if(n<25)Roundtrip(r);else Refuse(r);}
  for(unsigned n=0;n<5;++n){auto r=v;r.default_decision=static_cast<c::CatalogPolicyDefault>(n+1);r.exception_behavior=static_cast<c::CatalogPolicyException>(n+1);Roundtrip(r);}
  for(unsigned n=1;n<=6;++n){auto r=v;r.activation_boundary=static_cast<c::CatalogPolicyActivation>(n);Roundtrip(r);}
  for(unsigned n=1;n<=4;++n){auto r=v;r.recovery_behavior=static_cast<c::CatalogPolicyRecovery>(n);Roundtrip(r);}
  auto maximum=v;maximum.generation=UINT64_MAX;maximum.creating_transaction_number=maximum.origin_local_transaction_id=UINT64_MAX;
  maximum.effective_transaction_number=maximum.superseding_transaction_number=UINT64_MAX;maximum.precedence_rank=65535;
  maximum.cache_invalidation_mask=255;Roundtrip(maximum);
  for(unsigned n=0;n<29;++n){auto r=v;
    if(n==0)r.policy_uuid={};if(n==1)r.policy_version_uuid={};if(n==2)r.database_uuid={};if(n==3)r.scope_uuid={};if(n==4)r.authority_owner_uuid={};
    if(n==5)r.generation=0;if(n==6)r.policy_class=static_cast<c::CatalogPolicyClass>(24);if(n==7)r.scope=static_cast<c::CatalogPolicyScope>(22);
    if(n==8)r.precedence_rank=0;if(n==9)r.default_decision=static_cast<c::CatalogPolicyDefault>(0);if(n==10)r.exception_behavior=static_cast<c::CatalogPolicyException>(6);
    if(n==11)r.recovery_behavior=static_cast<c::CatalogPolicyRecovery>(5);if(n==12)r.activation_boundary=static_cast<c::CatalogPolicyActivation>(7);
    if(n==13)r.enforcement_points=0;if(n==14)r.creating_transaction_number=0;if(n==15)r.origin_local_transaction_id=0;
    if(n==16)r.origin_transaction_uuid.kind=p::UuidKind::object;if(n==17)r.creating_transaction_uuid.kind=p::UuidKind::object;
    if(n==18)r.scope_uuid=f::Id(400);if(n==19)r.scope=c::CatalogPolicyScope::cluster_member;
    if(n==20)r.effective_transaction_number=0;if(n==21)r.superseding_transaction_number=0;
    if(n==22){r.effective_transaction_number=3;r.superseding_transaction_number=2;}
    if(n==23)r.policy_uuid=r.policy_version_uuid;if(n==24)r.policy_version_uuid=r.database_uuid;
    if(n==25)r.origin_transaction_uuid.value=f::Id(6);if(n==26)r.evidence_profile_uuid=p::Uuid{};
    if(n==27)r.retention_profile_uuid=f::Id(304),r.retention_profile_uuid->bytes[6]=0x40;
    if(n==28)r.policy_uuid.bytes[8]=0;
    Refuse(r,n!=16&&n!=17);
  }
  // Wire precedence must be checked before narrowing to the native uint16.
  auto wire=Golden(v);for(std::size_t at=24;at<wire.size();){const auto tag=p::LoadLittle16(wire.data()+at);const auto length=p::LoadLittle32(wire.data()+at+4);
    if(tag==12){for(const auto rank:{p::u64{65536},UINT64_MAX}){auto bad=wire;f::Put(bad,at+8,rank,8);
      allocation_budget=0;const auto r=c::DecodeCatalogPolicyDescriptor(bad);allocation_budget=-1;Check(!r.ok(),"out-of-range wire precedence cannot truncate into valid rank");}}
    for(unsigned mode=0;mode<4;++mode){auto bad=wire;if(mode==0){bad.erase(at,8+length);f::Put(bad,8,bad.size(),4);f::Put(bad,12,18,4);}
      if(mode==1)f::Put(bad,at,65000,2);if(mode==2)f::Put(bad,at+2,4,1);if(mode==3)f::Put(bad,at+4,length-1,4);
      allocation_budget=0;const auto r=c::DecodeCatalogPolicyDescriptor(bad);allocation_budget=-1;Check(!r.ok(),"required field/type/width/unknown refusal");}
    at+=8+length;
  }
  auto object=v;std::array<p::byte,sizeof(object)> before,after;std::memcpy(before.data(),&object,sizeof(object));
  for(std::size_t n=0;n<sizeof(object);++n){allocation_budget=0;const auto r=c::EncodeCatalogPolicyDescriptorInto(object,{reinterpret_cast<p::byte*>(&object)+n,1});allocation_budget=-1;
    std::memcpy(after.data(),&object,sizeof(object));Check(!r.ok()&&before==after,"whole input object cannot be overwritten");}
}
void Bindings(){
  const auto v=Value();const auto m=Metadata(v);const auto view=c::BorrowCatalogMetadataVersion(m);
  allocation_budget=0;const auto bytes=c::CatalogMetadataVersionEncodedBytes(view);allocation_budget=-1;
  Check(bytes&&c::CatalogPolicyDescriptorMatchesDatabase(view,f::Id(1))&&!c::CatalogPolicyDescriptorMatchesDatabase(view,f::Id(2)),"exact native common and owning database binding");
  for(const auto kind:{p::UuidKind::principal,p::UuidKind::object,p::UuidKind::schema,p::UuidKind::database,p::UuidKind::cluster}){
    auto owner=m;owner.owner_uuid.kind=kind;allocation_budget=0;const auto size=c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(owner));allocation_budget=-1;
    Check(size.has_value(),"policy authority owner is not incorrectly restricted to user principals");
  }
  for(const auto kind:{p::UuidKind::unknown,p::UuidKind::row,p::UuidKind::page,p::UuidKind::transaction,p::UuidKind::session}){
    auto owner=m;owner.owner_uuid.kind=kind;allocation_budget=0;const auto size=c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(owner));allocation_budget=-1;
    Check(!size,"non-owner identity domain cannot be an authority owner");
  }
  for(unsigned n=0;n<13;++n){auto bad=m;
    if(n==0)bad.object_subtype="other";if(n==1)bad.record.header.kind=c::CatalogRecordKind::config_profile;
    if(n==2)bad.record.header.object_uuid.value=f::Id(9);if(n==3)bad.owner_uuid.value=f::Id(51);if(n==4)bad.definition_version=2;
    if(n==5)bad.creator_transaction_uuid.value=f::Id(6);if(n==6)bad.creator_local_transaction_id=2;
    if(n==7)bad.record.header.parent_uuid.value=f::Id(61);if(n==8)bad.audit_uuid={};if(n==9)bad.default_name_uuid={};
    if(n==10)f::Put(bad.record.payload,16,65618,4);if(n==11)bad.record.payload="area=metadata;mode=strict";if(n==12)bad.authority_scope=c::CatalogAuthorityScope::cluster;
    allocation_budget=0;const auto size=c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(bad));allocation_budget=-1;Check(!size,"kind/subtype/schema/common mutation cannot bypass policy binding");
  }
  auto next=v;next.generation=2;next.policy_version_uuid=f::Id(201);next.creating_transaction_uuid.value=f::Id(6);next.creating_transaction_number=2;
  auto successor=Metadata(next);Check(c::CatalogMetadataPreservesFamilyOrigin(m,successor),"admitted new generation retains policy class/database/origin and new version identity");
  for(unsigned n=0;n<5;++n){auto bad=next;if(n==0)bad.policy_version_uuid=v.policy_version_uuid;if(n==1)bad.policy_class=c::CatalogPolicyClass::authorization;
    if(n==2)bad.database_uuid=bad.scope_uuid=f::Id(9);if(n==3)bad.origin_transaction_uuid.value=f::Id(7);if(n==4)bad.origin_local_transaction_id=2;
    Check(!c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(bad)),"policy identity/class/database/origin cannot change or version identity be reused");}
}
struct Item {
  c::CatalogPolicyDescriptor descriptor;
  c::CatalogMetadataVersion definition,vector_metadata,entry_metadata;
  c::CatalogNameVector vector;c::CatalogNameEntry entry;
  std::array<c::CatalogCreationNameEntryInput,1> entries;
  explicit Item(unsigned n):descriptor(Value(n)),definition(Metadata(descriptor,n)){
    definition.record.payload.clear();vector_metadata=entry_metadata=definition;
    vector_metadata.record.header.kind=entry_metadata.record.header.kind=c::CatalogRecordKind::localized_name;
    vector_metadata.record.header.object_uuid=definition.name_vector_uuid;vector_metadata.record.header.row_uuid.value=f::Id(1001+3*n);
    vector_metadata.record.header.parent_uuid=definition.record.header.object_uuid;vector_metadata.object_subtype="name_vector";
    entry_metadata.record.header.object_uuid=definition.default_name_uuid;entry_metadata.record.header.row_uuid.value=f::Id(1002+3*n);
    entry_metadata.record.header.parent_uuid=definition.name_vector_uuid;entry_metadata.object_subtype="name_entry";entry_metadata.resource_epoch=1;
    vector.name_vector_uuid=entry.name_vector_uuid=definition.name_vector_uuid;vector.object_uuid=entry.object_uuid=definition.record.header.object_uuid;
    vector.default_name_entry_uuid=entry.name_entry_uuid=definition.default_name_uuid;vector.object_class=entry.object_class="policy";
    vector.owning_schema_uuid=definition.owning_schema_uuid;vector.default_language_tag="en-US";
    vector.name_collision_policy_uuid={p::UuidKind::object,f::Id(55)};vector.catalog_generation_id=1;
    vector.security_policy_uuid=definition.security_policy_uuid;vector.lifecycle_state=c::CatalogNameLifecycle::active;
    entry.scope_uuid=definition.record.header.parent_uuid;entry.parent_schema_uuid=definition.owning_schema_uuid;entry.language_tag="en-US";
    entry.dialect_profile_uuid={p::UuidKind::object,f::Id(56)};entry.identifier_profile_uuid={p::UuidKind::object,f::Id(57)};
    entry.raw_name_text=entry.display_name="explicit_policy_"+std::to_string(n);entry.normalized_lookup_key.assign(entry.raw_name_text.begin(),entry.raw_name_text.end());
    entry.created_transaction_uuid=descriptor.creating_transaction_uuid;entry.security_policy_uuid=definition.security_policy_uuid;
    entry.resource_epoch=entry.name_resolution_epoch=entry.catalog_generation_id=1;entry.default_for_language=entry.default_for_object=true;
    entry.lifecycle_state=c::CatalogNameLifecycle::active;
  }
  db::NativeInitialPolicyDescriptorInput Input(){entries[0]={&entry_metadata,&entry};return {&descriptor,{&definition,&vector_metadata,&vector,entries}};}
};
void Creation(){
  Item first(0),second(1);std::array inputs{first.Input(),second.Input()};
  db::NativeInitialPolicyDescriptorRequest request{f::Id(1),first.descriptor.creating_transaction_uuid,"en-US",inputs,{100,1000000}};
  const auto made=db::AssembleNativeInitialPolicyDescriptorRecords(request);
  Check(made.ok()&&made.records->definitions.size()==2&&made.records->names.size()==4&&made.records->definitions[0].metadata.record.payload==Golden(first.descriptor),
      "actual typed creation source yields complete coherent definition/name cohort");
  const auto retained=made.records->definitions[0].metadata.record.payload;
  allocation_count=0;const auto counted=db::AssembleNativeInitialPolicyDescriptorRecords(request);const auto sites=allocation_count;
  Check(counted.ok()&&sites,"real owning allocation sites counted");
  for(unsigned n=0;n<sites;++n){allocation_budget=n;const auto failed=db::AssembleNativeInitialPolicyDescriptorRecords(request);allocation_budget=-1;
    Check(!failed.ok()&&!failed.records&&failed.error==db::NativeInitialPolicyDescriptorError::resource_exhausted,"every actual owning allocation failure returns no partial package");}
  auto exact=request;exact.limits={6,made.records->encoded_bytes};Check(db::AssembleNativeInitialPolicyDescriptorRecords(exact).ok(),"exact combined record/byte budgets admitted");
  --exact.limits.maximum_encoded_bytes;Check(!db::AssembleNativeInitialPolicyDescriptorRecords(exact).ok(),"one-short byte budget refused");
  exact=request;exact.limits.maximum_records=5;Check(!db::AssembleNativeInitialPolicyDescriptorRecords(exact).ok(),"one-short record budget refused");
  for(unsigned mode=0;mode<6;++mode){const auto saved=second.descriptor;const auto saved_meta=second.definition;
    if(mode==0)second.descriptor.database_uuid=f::Id(9);if(mode==1)second.descriptor.effective_transaction_number=1;
    if(mode==2)second.descriptor.policy_version_uuid=first.descriptor.policy_version_uuid;
    if(mode==3)second.descriptor.policy_version_uuid=first.definition.record.header.row_uuid.value;
    if(mode==4)second.definition.record.payload="area=metadata;mode=strict";if(mode==5)second.descriptor.origin_transaction_uuid.value=f::Id(6);
    allocation_count=0;allocation_budget=0;const auto failed=db::AssembleNativeInitialPolicyDescriptorRecords(request);const auto allocs=allocation_count;allocation_budget=-1;
    Check(!allocs&&!failed.ok()&&!failed.records,"foreign origin/node, guessed text, premature effectiveness and whole-cohort version identity collisions refuse before copies");
    second.descriptor=saved;second.definition=saved_meta;
  }
  first.entry.display_name="changed after assembly";
  Check(made.records->definitions[0].metadata.record.payload==retained&&made.records->names[1].name.has_value(),"result owns exact admitted source after caller changes");
  // Cross the64-identity inline scratch threshold; test its real allocation
  // and the complete larger package, not only the small fixed-array lane.
  std::vector<Item> large;large.reserve(10);for(unsigned n=0;n<10;++n)large.emplace_back(n);
  std::vector<db::NativeInitialPolicyDescriptorInput> large_inputs;large_inputs.reserve(large.size());
  for(auto& item:large)large_inputs.push_back(item.Input());auto big=request;big.policies=large_inputs;big.limits.maximum_records=1000;
  allocation_count=0;const auto large_made=db::AssembleNativeInitialPolicyDescriptorRecords(big);const auto large_sites=allocation_count;
  Check(large_made.ok()&&large_made.records->definitions.size()==10&&large_made.records->names.size()==20,"larger actual cohort uses bounded canonical identity index");
  for(unsigned n=0;n<large_sites;++n){allocation_budget=n;const auto failed=db::AssembleNativeInitialPolicyDescriptorRecords(big);allocation_budget=-1;
    Check(!failed.ok()&&!failed.records&&failed.error==db::NativeInitialPolicyDescriptorError::resource_exhausted,"large scratch and every owning allocation failure remain atomic");}
  large.back().descriptor.policy_version_uuid=large.front().descriptor.policy_version_uuid;
  const auto duplicate=db::AssembleNativeInitialPolicyDescriptorRecords(big);
  Check(!duplicate.ok()&&duplicate.error==db::NativeInitialPolicyDescriptorError::duplicate_identity&&!duplicate.records,"large index rejects duplicate policy-version identity");
}
void Physical(){
  scratchbird::tests::OwnedTempDirectory artifacts;
  for(const auto& profile:d::kCanonicalFilespacePageProfiles){
    Item item(0);std::array inputs{item.Input()};const auto made=db::AssembleNativeInitialPolicyDescriptorRecords(
        {f::Id(1),item.descriptor.creating_transaction_uuid,"en-US",inputs,{10,1000000}});Check(made.ok(),"actual native policy package before physical admission");
    db::NativeCreationCatalogSeed seed;seed[0]=made.records->definitions;seed[1]=made.records->names;
    db::NativeFilespaceInitializationRequest request;
    request.bootstrap={f::Id(1),f::Id(2),profile.uuid,d::kNativeBootstrapIntegrityProfile,{},profile.page_size_bytes,1,0,1,7};
    request.operation_uuid=f::Id(3);request.writer_uuid=f::Id(4);request.policy_snapshot_uuid=f::Id(10);
    request.creator.transaction_uuid=item.descriptor.creating_transaction_uuid;request.creator.local_id=mga::MakeLocalTransactionId(1);
    request.creator.scope=mga::TransactionScope::local_node;request.total_pages=64;request.creation_utc_millis=1789357072000ULL;
    const auto budget=128*p::u64{profile.page_size_bytes};u::StandaloneUuidV7Issuer issuer({f::Id(1),f::Id(10)},{{},0,1000});
    const auto path=(artifacts.path()/std::to_string(profile.page_size_bytes)).string();d::FileDevice device;
    Check(device.Open(path,d::FileOpenMode::create_new).ok(),"real private native policy file");
    auto bad=item.descriptor;bad.database_uuid=bad.scope_uuid=f::Id(9);seed[0][0].metadata.record.payload=Golden(bad);
    const auto refused=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer);
    const auto extent=device.Size();Check(!refused.ok()&&extent.ok()&&!extent.size_bytes,"foreign native policy refuses before I/O");
    seed[0][0].metadata.record.payload=Golden(item.descriptor);
    const auto created=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer);Check(created.ok(),"real native policy/common/alias persistence");
    Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing).ok(),"independent native policy reopen");
    const std::vector<d::NativeFilespaceDevice> devices{{f::Id(2),profile.uuid,&device}};
    const auto rows=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,created.receipt->checkpoint,2,1,{created.receipt->relations[0].object_uuid,{}},budget);
    Check(rows.ok()&&rows.rows.size()==1&&rows.rows[0].metadata.record.payload==Golden(item.descriptor)&&
        c::CatalogPolicyDescriptorMatchesDatabase(c::BorrowCatalogMetadataVersion(rows.rows[0].metadata),f::Id(1)),"every typed descriptor field and origin survives actual native reopen");
    const auto names=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,created.receipt->checkpoint,2,2,{created.receipt->relations[1].object_uuid,{}},budget);
    Check(names.ok()&&names.rows.size()==2&&names.rows[0].name_payload&&names.rows[1].name_payload,"actual allocated name residency survives reopen");
    const auto zero=d::ReadFilespacePageZeroFromOpenDevice(device);Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==7,"policy declaration persistence is not evaluated permission or activation");
    Check(device.Close().ok(),"actual owned policy device released");
  }
}
}
int main()try{Values();Bindings();Creation();Physical();std::cout<<"PASS policy descriptor "<<checks<<" checks\n";return 0;}
catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<'\n';return 1;}

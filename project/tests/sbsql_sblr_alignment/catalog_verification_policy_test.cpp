// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_verification_policy.hpp"
#include "native_verification_policy_creation.hpp"
#include "native_creation_workspace.hpp"
#include "native_creation_catalog_input.hpp"
#include "physical_mga_cow_store.hpp"
#include "bootstrap_policy_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>
thread_local long allocation_budget=-1;
thread_local unsigned allocation_count=0;
void* operator new(std::size_t size){++allocation_count;if(allocation_budget>=0&&allocation_budget--==0)throw std::bad_alloc();
  if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace c=scratchbird::core::catalog;namespace p=scratchbird::core::platform;namespace f=scratchbird::tests::bootstrap_policy;
namespace db=scratchbird::storage::database;namespace d=scratchbird::storage::disk;namespace u=scratchbird::core::uuid;namespace mga=scratchbird::transaction::mga;
namespace {
using R=c::VerificationPolicyRole;using A=c::VerificationRepairAction;using Body=c::CatalogVerificationPolicyBody;
unsigned checks=0;
void Check(bool ok,const char* why,std::source_location loc=std::source_location::current()){++checks;
 if(!ok){allocation_budget=-1;throw std::runtime_error(std::string(why)+" line="+std::to_string(loc.line()));}}
template<class Call>auto NoHeap(Call&& call){allocation_budget=0;allocation_count=0;auto result=call();const auto calls=allocation_count;allocation_budget=-1;
 Check(!calls,"bounded policy body codecs never allocate");return result;}
Body Value(unsigned n=0){Body b;b.body_uuid=f::Id(100+n);b.database_uuid=f::Id(1);b.evidence_policy_uuid=f::Id(3000);
 b.generation=1;b.verification_class_mask=0x1ff;b.authority_class_mask=0x3f;b.verification_level_mask=0x3ff;
 b.maximum_scan_pages=1048576;b.maximum_work_bytes=64*1024*1024;b.maximum_findings=1024;
 b.origin_transaction_uuid=b.creating_transaction_uuid={p::UuidKind::transaction,f::Id(5)};
 b.origin_local_transaction_id=b.creating_local_transaction_id=1;return b;}
std::string Golden(const Body& r){f::Golden g(65621);g.Identity(1,r.body_uuid);g.Identity(2,r.database_uuid);g.Integer(3,r.generation);
 g.Integer(4,p::u64(r.role));g.Integer(5,r.verification_class_mask);g.Integer(6,r.authority_class_mask);g.Integer(7,r.verification_level_mask);
 g.Integer(8,r.allowed_action_mask);g.Integer(9,r.review_action_mask);g.Integer(10,p::u64(r.uncertain_action));
 g.Integer(11,r.maximum_scan_pages);g.Integer(12,r.maximum_work_bytes);g.Integer(13,r.maximum_findings);g.Identity(14,r.evidence_policy_uuid);
 if(r.access_policy_uuid)g.Identity(15,*r.access_policy_uuid);g.Integer(16,r.flags);g.Identity(17,r.origin_transaction_uuid.value);
 g.Integer(18,r.origin_local_transaction_id);g.Identity(19,r.creating_transaction_uuid.value);g.Integer(20,r.creating_local_transaction_id);return g.Finish();}
c::CatalogMetadataVersion Metadata(const Body& r,unsigned n=0){auto m=f::Metadata(0,r.creating_transaction_uuid,f::Id(60),Golden(r));
 m.record.header.object_uuid={p::UuidKind::object,r.body_uuid};m.record.header.row_uuid={p::UuidKind::row,f::Id(1000+3*n)};
 m.default_name_uuid={p::UuidKind::object,f::Id(2000+2*n)};m.name_vector_uuid={p::UuidKind::object,f::Id(2001+2*n)};
 m.object_subtype="verification_policy_body";m.creator_local_transaction_id=r.creating_local_transaction_id;m.definition_version=r.generation;return m;}
void Roundtrip(const Body& value){const auto expected=Golden(value);std::array<p::byte,512> output;output.fill(0xa7);
 const auto size=NoHeap([&]{return c::CatalogVerificationPolicyEncodedBytes(value);});Check(size&&*size==expected.size()&&*size<=c::kCatalogVerificationPolicyMaxBytes,"independent complete byte count");
 const auto encoded=NoHeap([&]{return c::EncodeCatalogVerificationPolicyInto(value,std::span(output).subspan(1));});
 Check(encoded.ok()&&std::string_view(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size())==expected,"exact independent SBCV fields");
 Check(output.front()==0xa7&&std::all_of(output.begin()+expected.size()+1,output.end(),[](auto b){return b==0xa7;}),"unaligned destination preserves guards");
 const auto decoded=NoHeap([&]{return c::DecodeCatalogVerificationPolicy(expected);});Check(decoded.ok()&&Golden(*decoded.record)==expected,"all decoded fields preserved");}
void Refuse(const Body& value){const auto raw=Golden(value);std::array<p::byte,512> output;output.fill(0xa7);const auto before=output;
 const auto encoded=NoHeap([&]{return c::EncodeCatalogVerificationPolicyInto(value,output);});
 Check(!encoded.ok()&&encoded.bytes.empty()&&output==before&&!NoHeap([&]{return c::CatalogVerificationPolicyEncodedBytes(value);}),"invalid body leaves all output untouched");
 // Identity domains are supplied by the SBCV schema, not serialized in the
 // raw16 value. A wrong in-memory domain is an encoder-only malformed input.
 if(value.origin_transaction_uuid.kind==p::UuidKind::transaction&&value.creating_transaction_uuid.kind==p::UuidKind::transaction){
  const auto decoded=NoHeap([&]{return c::DecodeCatalogVerificationPolicy(raw);});Check(!decoded.ok()&&!decoded.record,"invalid values cannot enter through decode");}}
void Values(){const auto base=Value();Roundtrip(base);Check(Golden(base).size()==368,"non-quarantine extent");
 for(unsigned role=1;role<=3;++role){auto b=base;b.role=R(role);if(role==2)b.access_policy_uuid=f::Id(3001);if(role==3)b.flags|=0x100;Roundtrip(b);
  if(role==2)Check(Golden(b).size()==392,"quarantine extent includes exact access policy");
  for(unsigned action=1;action<=18;++action){auto v=b;v.flags|=1;v.allowed_action_mask|=p::u64{1}<<(action-1);v.review_action_mask=v.allowed_action_mask;
   const bool common=action==1||action==11||action>=16;const bool allowed=common||(role==1&&action<=10)||(role==2&&action>=12&&action<=14)||(role==3&&action==15);
   if(allowed)Roundtrip(v);else Refuse(v);}
 }
 for(unsigned field=0;field<30;++field){auto b=base;switch(field){case 0:b.body_uuid={};break;case 1:b.database_uuid.bytes[6]=0x40;break;
  case 2:b.evidence_policy_uuid=b.body_uuid;break;case 3:b.generation=0;break;case 4:b.role=R(4);break;case 5:b.verification_class_mask=0;break;
  case 6:b.verification_class_mask=0x200;break;case 7:b.authority_class_mask=0;break;case 8:b.authority_class_mask=64;break;
  case 9:b.verification_level_mask=0;break;case 10:b.verification_level_mask=1024;break;case 11:b.allowed_action_mask=0;break;
  case 12:b.allowed_action_mask|=p::u64{1}<<18;break;case 13:b.review_action_mask=2;break;case 14:b.uncertain_action=A::rebuild_index;break;
  case 15:b.allowed_action_mask=1;break;case 16:b.maximum_scan_pages=0;break;case 17:b.maximum_work_bytes=4095;break;case 18:b.maximum_findings=0;break;
  case 19:b.access_policy_uuid=f::Id(3001);break;case 20:b.flags|=0x200;break;case 21:b.flags&=~p::u64{8};break;
  case 22:b.flags|=2;break;case 23:b.flags|=4;break;case 24:b.flags|=0x100;break;case 25:b.allowed_action_mask|=2;break;
  case 26:b.origin_transaction_uuid.kind=p::UuidKind::object;break;case 27:b.creating_transaction_uuid.value=f::Id(7);break;
  case 28:b.creating_local_transaction_id=0;break;case 29:b.origin_local_transaction_id=0;break;}Refuse(b);}
 for(unsigned bit=3;bit<=7;++bit){auto b=base;b.flags&=~(p::u64{1}<<bit);Refuse(b);}
 auto salvage=base;salvage.role=R::salvage;salvage.flags|=0x104;Roundtrip(salvage);salvage.flags|=3;Refuse(salvage);
 auto online=base;online.flags|=3;online.allowed_action_mask|=2;Roundtrip(online);
 auto maximum=base;maximum.generation=maximum.creating_local_transaction_id=maximum.maximum_scan_pages=maximum.maximum_work_bytes=maximum.maximum_findings=UINT64_MAX;
 maximum.creating_transaction_uuid.value=f::Id(7);Roundtrip(maximum);
 auto inconsistent=base;inconsistent.generation=2;inconsistent.creating_local_transaction_id=2;Refuse(inconsistent);
 inconsistent=base;inconsistent.generation=2;inconsistent.creating_transaction_uuid.value=f::Id(7);Refuse(inconsistent);
 const auto raw=Golden(base);std::array<p::byte,512> output;output.fill(0xa4);const auto before=output;
 for(std::size_t n=0;n<raw.size();++n){Check(!NoHeap([&]{return c::DecodeCatalogVerificationPolicy(std::string_view(raw).substr(0,n));}).ok(),"every truncated body refused");
  Check(!NoHeap([&]{return c::EncodeCatalogVerificationPolicyInto(base,std::span(output).first(n));}).ok()&&output==before,"every short output unchanged");}
 auto bytes=raw;bytes.push_back(0);Check(!c::DecodeCatalogVerificationPolicy(bytes).ok(),"trailing bytes refused");
 for(unsigned at:{0u,4u,6u,8u,12u,16u,20u,24u,26u,28u}){bytes=raw;bytes[at]^=0x7f;Check(!c::DecodeCatalogVerificationPolicy(bytes).ok(),"malformed framing field type width or schema refused");}
 auto alias=base;const auto original=Golden(alias);auto region=std::span<p::byte>{reinterpret_cast<p::byte*>(&alias),sizeof(alias)};
 Check(!NoHeap([&]{return c::EncodeCatalogVerificationPolicyInto(alias,region);}).ok()&&Golden(alias)==original,"whole input/output overlap refuses before writes");}
void Bindings(){auto b=Value();auto m=Metadata(b);auto view=c::BorrowCatalogMetadataVersion(m);
 c::CatalogPolicyDescriptor descriptor;descriptor.policy_uuid=f::Id(700);descriptor.policy_version_uuid=f::Id(701);
 descriptor.database_uuid=b.database_uuid;descriptor.body_uuid=b.body_uuid;descriptor.body_generation=b.generation;
 descriptor.generation=1;descriptor.policy_class=c::CatalogPolicyClass::verification_repair;
 descriptor.creating_transaction_number=descriptor.origin_local_transaction_id=1;
 descriptor.origin_transaction_uuid=descriptor.creating_transaction_uuid=b.creating_transaction_uuid;
 descriptor.scope=c::CatalogPolicyScope::database;descriptor.scope_uuid=b.database_uuid;descriptor.authority_owner_uuid=f::Id(50);
 descriptor.precedence_rank=1;descriptor.enforcement_points=1;
 Check(NoHeap([&]{return c::CatalogVerificationPolicyMatchesDescriptor(b,descriptor,R::repair);}),"exact descriptor selects matching class body role and generation");
 for(unsigned field=0;field<5;++field){auto bad=descriptor;auto role=R::repair;switch(field){case 0:bad.body_uuid=f::Id(702);break;
  case 1:bad.body_generation++;break;case 2:bad.database_uuid=bad.scope_uuid=f::Id(703);break;
  case 3:bad.policy_class=c::CatalogPolicyClass::evidence;break;case 4:role=R::quarantine;break;}
  Check(!c::CatalogVerificationPolicyMatchesDescriptor(b,bad,role),"wrong class database body generation or verification role cannot bind");}
 Check(c::CatalogVerificationPolicyMatchesHeader(view.record)&&c::CatalogVerificationPolicyMatchesMetadata(view)&&!c::ValidateCatalogMetadataVersionView(view),"full common metadata dispatch accepts typed body");
 Check(c::CatalogVerificationPolicyMatchesDatabase(view,f::Id(1))&&!c::CatalogVerificationPolicyMatchesDatabase(view,f::Id(2)),"native owning database binding");
 for(unsigned n=0;n<5;++n){auto bad=m;switch(n){case 0:bad.object_subtype="policy_descriptor";break;case 1:bad.record.header.kind=c::CatalogRecordKind::config_profile;break;
  case 2:bad.definition_version++;break;case 3:bad.creator_local_transaction_id++;break;case 4:bad.creator_transaction_uuid.value=f::Id(99);break;}
  Check(c::ValidateCatalogMetadataVersionView(c::BorrowCatalogMetadataVersion(bad)).has_value(),"common dispatcher rejects body/header/subtype/creator mismatch");}
 auto next=b;next.generation=2;next.creating_transaction_uuid.value=f::Id(7);next.creating_local_transaction_id=2;
 auto newer=Metadata(next);Check(c::CatalogMetadataPreservesFamilyOrigin(m,newer),"higher generation retains exact origin");
 auto same=b;same.maximum_findings++;Check(!c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(same)),"same-generation policy bytes immutable");
 next.role=R::salvage;next.flags|=0x100;Check(!c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(next)),"role is stable across generations");
 db::NativeCreationCatalogRecord record{m,{}};std::vector<p::Uuid> reserved;
 db::ReserveNativeCreationCatalogReferences(record,[&](const p::Uuid& id){reserved.push_back(id);});
 for(const auto& id:{b.body_uuid,b.database_uuid,b.evidence_policy_uuid,b.origin_transaction_uuid.value,b.creating_transaction_uuid.value})
  Check(std::find(reserved.begin(),reserved.end(),id)!=reserved.end(),"storage issuer reserves complete body references");}
struct Item {
  c::CatalogVerificationPolicyBody descriptor;
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
    vector.default_name_entry_uuid=entry.name_entry_uuid=definition.default_name_uuid;vector.object_kind=entry.object_kind=c::CatalogRecordKind::policy;
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
  db::NativeInitialVerificationPolicyInput Input(){entries[0]={&entry_metadata,&entry};return {&descriptor,{&definition,&vector_metadata,&vector,entries}};}
};
void Creation(){Item first(0),second(1);std::array inputs{first.Input(),second.Input()};
 db::NativeInitialVerificationPolicyRequest request{f::Id(1),first.descriptor.creating_transaction_uuid,"en-US",inputs,{100,1000000}};
 const auto made=db::AssembleNativeInitialVerificationPolicyRecords(request);
 Check(made.ok()&&made.records->definitions.size()==2&&made.records->names.size()==4&&made.records->definitions[0].metadata.record.payload==Golden(first.descriptor),"typed creation source preserves rules and complete names");
 auto exact=request;exact.limits={6,made.records->encoded_bytes};Check(db::AssembleNativeInitialVerificationPolicyRecords(exact).ok(),"exact package budgets admitted");
 --exact.limits.maximum_encoded_bytes;Check(!db::AssembleNativeInitialVerificationPolicyRecords(exact).ok(),"one-short byte budget refused");
 allocation_count=0;Check(db::AssembleNativeInitialVerificationPolicyRecords(request).ok(),"count real creation allocation sites");const auto sites=allocation_count;
 for(unsigned n=0;n<sites;++n){allocation_budget=n;const auto failed=db::AssembleNativeInitialVerificationPolicyRecords(request);allocation_budget=-1;
  Check(!failed.ok()&&!failed.records&&failed.error==db::NativeInitialPolicyDescriptorError::resource_exhausted,"allocation failures expose no partial declarations");}
 const auto saved=second.descriptor;second.descriptor.evidence_policy_uuid=first.definition.record.header.row_uuid.value;
 Check(db::AssembleNativeInitialVerificationPolicyRecords(request).error==db::NativeInitialPolicyDescriptorError::duplicate_identity,"evidence reference cannot alias a newly defined row");second.descriptor=saved;
 second.descriptor.generation=2;Check(!db::AssembleNativeInitialVerificationPolicyRecords(request).ok(),"construction requires original semantic generation");}
void Physical(){scratchbird::tests::OwnedTempDirectory artifacts;
 for(const auto& profile:d::kCanonicalFilespacePageProfiles){for(unsigned role=1;role<=3;++role){Item item(0);item.descriptor.role=R(role);
  if(role==2)item.descriptor.access_policy_uuid=f::Id(3001);if(role==3)item.descriptor.flags|=0x100;
  std::array inputs{item.Input()};const auto made=db::AssembleNativeInitialVerificationPolicyRecords({f::Id(1),item.descriptor.creating_transaction_uuid,"en-US",inputs,{10,1000000}});
  Check(made.ok(),"actual typed body declarations before physical construction");db::NativeCreationCatalogSeed seed;seed[4]=made.records->definitions;seed[1]=made.records->names;
  db::NativeFilespaceInitializationRequest request;request.bootstrap={f::Id(1),f::Id(2),profile.uuid,d::kNativeBootstrapIntegrityProfile,{},profile.page_size_bytes,1,0,1,7};
  request.operation_uuid=f::Id(3);request.writer_uuid=f::Id(4);request.policy_snapshot_uuid=f::Id(10);
  request.creator.transaction_uuid=item.descriptor.creating_transaction_uuid;request.creator.local_id=mga::MakeLocalTransactionId(1);request.creator.scope=mga::TransactionScope::local_node;
  request.total_pages=64;request.creation_utc_millis=1789357072000ULL;const auto budget=128*p::u64{profile.page_size_bytes};u::StandaloneUuidV7Issuer issuer({f::Id(1),f::Id(10)},{{},0,1000});
  const auto path=(artifacts.path()/(std::to_string(profile.page_size_bytes)+"-"+std::to_string(role))).string();d::FileDevice device;
  Check(device.Open(path,d::FileOpenMode::create_new).ok(),"owned native physical body file");auto foreign=item.descriptor;foreign.database_uuid=f::Id(9);seed[4][0].metadata.record.payload=Golden(foreign);
  Check(!db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer).ok()&&device.Size().size_bytes==0,"foreign body refuses before physical effects");
  seed[4][0].metadata.record.payload=Golden(item.descriptor);const auto created=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer);
  Check(created.ok(),"complete native body and names persisted");Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing).ok(),"independent native body reopen");
  const std::vector<d::NativeFilespaceDevice> devices{{f::Id(2),profile.uuid,&device}};
  const auto rows=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,db::NativeCatalogSource::PageZero(created.receipt->filespace_uuid),2,5,{created.receipt->relations[4].object_uuid,{}},budget);
  Check(rows.ok()&&rows.rows.size()==1&&rows.rows[0].metadata.record.payload==Golden(item.descriptor)&&c::CatalogVerificationPolicyMatchesDatabase(c::BorrowCatalogMetadataVersion(rows.rows[0].metadata),f::Id(1)),"all limits, action rules and origins survive native reopen");
  const auto zero=d::ReadFilespacePageZeroFromOpenDevice(device);Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==1,"persisted policy declarations do not activate the node: page zero stays as the create wrote it, an online filespace (owner 2026-11-07); serving is the server's act at open");Check(device.Close().ok(),"owned body file closed");
 }} }
}
int main(){try{Values();Bindings();Creation();Physical();std::cout<<checks<<" verification policy body checks passed (declarations only)\n";return 0;}
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<'\n';return 1;}}

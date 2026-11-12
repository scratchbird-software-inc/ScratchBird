// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_evidence_policy.hpp"
#include "catalog_named_creation.hpp"
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
using Body=c::CatalogEvidencePolicyBody;
unsigned checks=0;
void Check(bool ok,const char* why,std::source_location loc=std::source_location::current()){++checks;
 if(!ok){allocation_budget=-1;throw std::runtime_error(std::string(why)+" line="+std::to_string(loc.line()));}}
template<class Call>auto NoHeap(Call&& call){allocation_budget=0;allocation_count=0;auto result=call();const auto calls=allocation_count;allocation_budget=-1;
 Check(!calls,"bounded evidence policy codecs never allocate");return result;}
Body Value(unsigned n=0){Body b;b.body_uuid=f::Id(100+n);b.database_uuid=f::Id(1);b.storage_filespace_uuid=f::Id(2);b.generation=1;
 b.required_evidence_classes.entries[0]="verification_finding";b.required_evidence_classes.entries[1]="verification_operation";b.required_evidence_classes.count=2;
 b.required_search_keys.entries[0]="SB-MGA-VERIFY";b.required_search_keys.count=1;
 b.maximum_record_bytes=4096;b.maximum_batch_bytes=1048576;
 b.origin_transaction_uuid=b.creating_transaction_uuid={p::UuidKind::transaction,f::Id(5)};
 b.origin_local_transaction_id=b.creating_local_transaction_id=1;return b;}
std::string Set(const c::CatalogEvidenceStringSet& set){std::string bytes(4,0);f::Put(bytes,0,set.count,4);
 for(std::size_t i=0;i<set.count;++i){const auto at=bytes.size();bytes.resize(at+4);f::Put(bytes,at,set.entries[i].size(),4);bytes.append(set.entries[i]);}return bytes;}
std::string Golden(const Body& r){f::Golden g(65622);g.Identity(1,r.body_uuid);g.Identity(2,r.database_uuid);g.Integer(3,r.generation);
 g.Field(4,8,Set(r.required_evidence_classes));g.Field(5,8,Set(r.required_search_keys));g.Identity(6,r.storage_filespace_uuid);
 g.Integer(7,p::u64(r.retention_class));g.Integer(8,p::u64(r.redaction_rule));g.Integer(9,p::u64(r.digest_rule));g.Integer(10,p::u64(r.external_export_rule));
 g.Integer(11,p::u64(r.chain_of_custody_rule));g.Integer(12,p::u64(r.cluster_replication_rule));if(r.cluster_uuid)g.Identity(13,*r.cluster_uuid);
 g.Integer(14,r.maximum_record_bytes);g.Integer(15,r.maximum_batch_bytes);g.Integer(16,r.flags);g.Identity(17,r.origin_transaction_uuid.value);
 g.Integer(18,r.origin_local_transaction_id);g.Identity(19,r.creating_transaction_uuid.value);g.Integer(20,r.creating_local_transaction_id);return g.Finish();}
c::CatalogMetadataVersion Metadata(const Body& r,unsigned n=0){auto m=f::Metadata(0,r.creating_transaction_uuid,f::Id(60),Golden(r));
 m.record.header.object_uuid={p::UuidKind::object,r.body_uuid};m.record.header.row_uuid={p::UuidKind::row,f::Id(1000+3*n)};
 m.default_name_uuid={p::UuidKind::object,f::Id(2000+2*n)};m.name_vector_uuid={p::UuidKind::object,f::Id(2001+2*n)};
 m.object_subtype="evidence_policy_body";m.creator_local_transaction_id=r.creating_local_transaction_id;m.definition_version=r.generation;
 if(r.cluster_uuid)m.authority_scope=c::CatalogAuthorityScope::cluster;return m;}
void Roundtrip(const Body& value){const auto expected=Golden(value);std::array<p::byte,c::kCatalogEvidencePolicyMaxBytes+2> output;output.fill(0xa7);
 const auto size=NoHeap([&]{return c::CatalogEvidencePolicyEncodedBytes(value);});Check(size&&*size==expected.size()&&*size<=c::kCatalogEvidencePolicyMaxBytes,"independent complete byte count");
 const auto encoded=NoHeap([&]{return c::EncodeCatalogEvidencePolicyInto(value,std::span(output).subspan(1));});
 Check(encoded.ok()&&std::string_view(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size())==expected,"exact independent SBCV fields");
 Check(output.front()==0xa7&&std::all_of(output.begin()+expected.size()+1,output.end(),[](auto b){return b==0xa7;}),"unaligned destination preserves guards");
 const auto decoded=NoHeap([&]{return c::DecodeCatalogEvidencePolicy(expected);});Check(decoded.ok()&&Golden(*decoded.record)==expected,"all decoded strings and fields retained");}
void Refuse(const Body& value,bool wire=true){std::array<p::byte,c::kCatalogEvidencePolicyMaxBytes> output;output.fill(0xa7);const auto before=output;
 const auto encoded=NoHeap([&]{return c::EncodeCatalogEvidencePolicyInto(value,output);});
 Check(!encoded.ok()&&encoded.bytes.empty()&&output==before&&!NoHeap([&]{return c::CatalogEvidencePolicyEncodedBytes(value);}),"invalid body leaves output unchanged");
 if(wire){const auto raw=Golden(value);const auto decoded=NoHeap([&]{return c::DecodeCatalogEvidencePolicy(raw);});Check(!decoded.ok()&&!decoded.record,"wire invalid body refused");}}
void Values(){const auto base=Value();Roundtrip(base);
 for(unsigned redaction=1;redaction<=6;++redaction){auto b=base;b.redaction_rule=c::EvidenceRedactionRule(redaction);
  if(redaction==6){b.external_export_rule=c::EvidenceExportRule::authorized_request_only;b.chain_of_custody_rule=c::EvidenceCustodyRule::record_every_transfer;}Roundtrip(b);}
 for(unsigned replication=2;replication<=3;++replication){auto b=base;b.cluster_uuid=f::Id(3000);b.cluster_replication_rule=c::EvidenceReplicationRule(replication);Roundtrip(b);}
 for(unsigned retention=1;retention<=10;++retention){auto b=base;b.retention_class=c::EvidenceRetentionClass(retention);Roundtrip(b);}
 for(unsigned flags=24;flags<=31;++flags){auto b=base;b.flags=flags;Roundtrip(b);}
 for(unsigned field=0;field<30;++field){auto b=base;switch(field){case 0:b.body_uuid={};break;case 1:b.database_uuid.bytes[6]=0x40;break;
  case 2:b.storage_filespace_uuid=b.body_uuid;break;case 3:b.generation=0;break;case 4:b.retention_class=c::EvidenceRetentionClass(11);break;
  case 5:b.redaction_rule=c::EvidenceRedactionRule(0);break;case 6:b.digest_rule=c::EvidenceDigestRule(3);break;
  case 7:b.external_export_rule=c::EvidenceExportRule(0);break;case 8:b.chain_of_custody_rule=c::EvidenceCustodyRule(3);break;
  case 9:b.cluster_replication_rule=c::EvidenceReplicationRule(4);break;case 10:b.cluster_uuid=f::Id(3000);break;
  case 11:b.cluster_replication_rule=c::EvidenceReplicationRule::cluster_replicated;break;case 12:b.maximum_record_bytes=255;break;
  case 13:b.maximum_batch_bytes=4095;break;case 14:b.flags=32;break;case 15:b.flags=23;break;
  case 16:b.redaction_rule=c::EvidenceRedactionRule::digest_only;b.digest_rule=c::EvidenceDigestRule::none;break;
  case 17:b.redaction_rule=c::EvidenceRedactionRule::external_reference;break;
  case 18:b.origin_local_transaction_id=0;break;case 19:b.creating_transaction_uuid.value=f::Id(8);break;
  case 20:b.creating_local_transaction_id=2;break;case 21:b.origin_transaction_uuid.kind=p::UuidKind::object;break;
  case 22:b.required_evidence_classes.count=0;break;case 23:b.required_search_keys.count=65;break;
  case 24:b.required_evidence_classes.entries[1]=b.required_evidence_classes.entries[0];break;
  case 25:std::swap(b.required_evidence_classes.entries[0],b.required_evidence_classes.entries[1]);break;
  case 26:b.required_search_keys.entries[0]="";break;case 27:b.required_search_keys.entries[0]=std::string_view("A\0B",3);break;
  case 28:b.required_search_keys.entries[0]=std::string_view("\xc0\xaf",2);break;
  case 29:b.required_search_keys.entries[0]=std::string_view("\xed\xa0\x80",3);break;}
  Refuse(b,field!=21&&field!=23);
 }
 std::array<std::string,64> strings;auto maximum=base;maximum.cluster_uuid=f::Id(3000);maximum.cluster_replication_rule=c::EvidenceReplicationRule::cluster_quorum;
 for(unsigned i=0;i<64;++i){strings[i]=std::string(128,'a');strings[i][0]='A'+i/26;strings[i][1]='A'+i%26;
  maximum.required_evidence_classes.entries[i]=strings[i];maximum.required_search_keys.entries[i]=strings[i];}
 maximum.required_evidence_classes.count=maximum.required_search_keys.count=64;Roundtrip(maximum);
 Check(Golden(maximum).size()==c::kCatalogEvidencePolicyMaxBytes,"maximum set envelope exact extent");
 std::string long_key(129,'a');auto long_body=base;long_body.required_search_keys.entries[0]=long_key;Refuse(long_body);
 for(const auto bad_utf8:{std::string_view("\xf4\x90\x80\x80",4),std::string_view("\xe2\x82",2),std::string_view("\x80",1)}){
  auto malformed=base;malformed.required_search_keys.entries[0]=bad_utf8;Refuse(malformed);
 }
 auto unicode=base;unicode.required_search_keys.entries[0]="\xe2\x82\xac";Roundtrip(unicode);
 const auto raw=Golden(base);std::array<p::byte,c::kCatalogEvidencePolicyMaxBytes> out;out.fill(0xa7);const auto before=out;
 for(std::size_t n=0;n<raw.size();++n){Check(!NoHeap([&]{return c::DecodeCatalogEvidencePolicy(std::string_view(raw).substr(0,n));}).ok(),"every truncation refused");
  Check(!NoHeap([&]{return c::EncodeCatalogEvidencePolicyInto(base,std::span(out).first(n));}).ok()&&out==before,"every short output untouched");}
 auto extra=raw;extra.push_back(0);Check(!c::DecodeCatalogEvidencePolicy(extra).ok(),"trailing byte refused");
 for(unsigned offset:{0u,4u,6u,8u,12u,16u,20u,24u,26u,28u}){auto bad=raw;bad[offset]^=0x7f;Check(!c::DecodeCatalogEvidencePolicy(bad).ok(),"invalid framing schema field refused");}
 for(unsigned count:{0u,1u,3u,65u,0xffffffffu}){auto bad=raw;f::Put(bad,96,count,4);
  Check(!c::DecodeCatalogEvidencePolicy(bad).ok(),"nested string count mismatch or overflow refused");}
 for(unsigned length:{0u,129u,0xffffffffu}){auto bad=raw;f::Put(bad,100,length,4);
  Check(!c::DecodeCatalogEvidencePolicy(bad).ok(),"nested string length mismatch or overflow refused");}
 auto alias=base;Check(!NoHeap([&]{return c::EncodeCatalogEvidencePolicyInto(alias,{reinterpret_cast<p::byte*>(&alias),sizeof(alias)});}).ok()&&Golden(alias)==raw,"input object overlap refused");
 std::array<p::byte,20000> shared{};std::copy_n("source-key",10,shared.begin());alias.required_search_keys.entries[0]={reinterpret_cast<char*>(shared.data()),10};const auto prior=shared;
 Check(!NoHeap([&]{return c::EncodeCatalogEvidencePolicyInto(alias,shared);}).ok()&&shared==prior,"borrowed string overlap refused");
}
void Bindings(){auto b=Value();auto m=Metadata(b);auto v=c::BorrowCatalogMetadataVersion(m);
 Check(c::CatalogEvidencePolicyMatchesHeader(v.record)&&c::CatalogEvidencePolicyMatchesMetadata(v)&&!c::ValidateCatalogMetadataVersionView(v),"common dispatcher admits exact evidence body");
 Check(c::CatalogEvidencePolicyMatchesDatabase(v,f::Id(1))&&!c::CatalogEvidencePolicyMatchesDatabase(v,f::Id(9)),"body owning database exact");
 for(unsigned field=0;field<6;++field){auto bad=m;switch(field){case 0:bad.object_subtype="other";break;case 1:bad.record.header.kind=c::CatalogRecordKind::config_profile;break;
  case 2:++bad.definition_version;break;case 3:++bad.creator_local_transaction_id;break;case 4:bad.creator_transaction_uuid.value=f::Id(99);break;
  case 5:bad.authority_scope=c::CatalogAuthorityScope::cluster;break;}
  Check(c::ValidateCatalogMetadataVersionView(c::BorrowCatalogMetadataVersion(bad)).has_value(),"common mismatched metadata refused");}
 auto next=b;next.generation=2;next.creating_local_transaction_id=2;next.creating_transaction_uuid.value=f::Id(7);
 Check(c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(next)),"successor retains original creator");
 next.generation=1;Check(!c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(next)),"initial generation cannot change creator");
 next=b;++next.maximum_batch_bytes;Check(!c::CatalogMetadataPreservesFamilyOrigin(m,Metadata(next)),"same generation immutable");
 c::CatalogPolicyDescriptor descriptor;descriptor.policy_uuid=f::Id(700);descriptor.policy_version_uuid=f::Id(701);descriptor.database_uuid=b.database_uuid;
 descriptor.body_uuid=b.body_uuid;descriptor.body_generation=b.generation;descriptor.generation=1;descriptor.policy_class=c::CatalogPolicyClass::evidence;
 descriptor.creating_transaction_number=descriptor.origin_local_transaction_id=1;descriptor.origin_transaction_uuid=descriptor.creating_transaction_uuid=b.creating_transaction_uuid;
 descriptor.scope=c::CatalogPolicyScope::database;descriptor.scope_uuid=b.database_uuid;descriptor.authority_owner_uuid=f::Id(50);descriptor.precedence_rank=1;descriptor.enforcement_points=1;
 Check(NoHeap([&]{return c::CatalogEvidencePolicyMatchesDescriptor(b,descriptor);}),"evidence descriptor selects exact typed body");
 for(unsigned field=0;field<4;++field){auto bad=descriptor;if(field==0)bad.body_uuid=f::Id(702);if(field==1)++bad.body_generation;
  if(field==2)bad.policy_class=c::CatalogPolicyClass::audit;if(field==3)bad.database_uuid=bad.scope_uuid=f::Id(9);
  Check(!c::CatalogEvidencePolicyMatchesDescriptor(b,bad),"wrong descriptor class generation body or database refused");}
 db::NativeCreationCatalogRecord record{m,{}};std::vector<p::Uuid> ids;db::ReserveNativeCreationCatalogReferences(record,[&](auto id){ids.push_back(id);});
 for(const auto& id:{b.body_uuid,b.database_uuid,b.storage_filespace_uuid,b.origin_transaction_uuid.value,b.creating_transaction_uuid.value})
  Check(std::find(ids.begin(),ids.end(),id)!=ids.end(),"storage reserves all evidence policy references");
}
struct Item {
  c::CatalogEvidencePolicyBody descriptor;
  c::CatalogMetadataVersion definition,vector_metadata,entry_metadata;
  c::CatalogNameVector vector;c::CatalogNameEntry entry;
  std::array<c::CatalogCreationNameEntryInput,1> entries;
  explicit Item(unsigned n):descriptor(Value(n)),definition(Metadata(descriptor,n)){
    vector_metadata=entry_metadata=definition;vector_metadata.record.payload.clear();entry_metadata.record.payload.clear();
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
  c::CatalogNamedCreationInput Input(){entries[0]={&entry_metadata,&entry};return {&definition,&vector_metadata,&vector,entries};}
};
void Physical(){scratchbird::tests::OwnedTempDirectory artifacts;
 for(const auto& profile:d::kCanonicalFilespacePageProfiles){Item item(0);std::array inputs{item.Input()};
  const auto made=c::AssembleLocalNamedCreationRecords(f::Id(1),item.descriptor.creating_transaction_uuid,"en-US",inputs,{10,1000000});
  Check(made.ok()&&made.records->definitions.size()==1&&made.records->names.size()==2,"typed body and complete supplied names assemble");
  db::NativeCreationCatalogSeed seed;seed[4]=made.records->definitions;seed[1]=made.records->names;
  db::NativeFilespaceInitializationRequest request;request.bootstrap={f::Id(1),f::Id(2),profile.uuid,d::kNativeBootstrapIntegrityProfile,{},profile.page_size_bytes,1,0,1,7};
  request.operation_uuid=f::Id(3);request.writer_uuid=f::Id(4);request.policy_snapshot_uuid=f::Id(10);
  request.creator.transaction_uuid=item.descriptor.creating_transaction_uuid;request.creator.local_id=mga::MakeLocalTransactionId(1);request.creator.scope=mga::TransactionScope::local_node;
  request.total_pages=64;request.creation_utc_millis=1789357072000ULL;const auto budget=128*p::u64{profile.page_size_bytes};u::StandaloneUuidV7Issuer issuer({f::Id(1),f::Id(10)},{{},0,1000});
  const auto path=(artifacts.path()/std::to_string(profile.page_size_bytes)).string();d::FileDevice device;
  Check(device.Open(path,d::FileOpenMode::create_new).ok(),"real native evidence policy file");auto foreign=item.descriptor;foreign.database_uuid=f::Id(9);seed[4][0].metadata.record.payload=Golden(foreign);
  Check(!db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer).ok()&&device.Size().size_bytes==0,"foreign body refuses before physical effects");
  seed[4][0].metadata.record.payload=Golden(item.descriptor);const auto created=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer);
  Check(created.ok(),"real selected native evidence policy persists");Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing).ok(),"independent reopen");
  const std::vector<d::NativeFilespaceDevice> devices{{f::Id(2),profile.uuid,&device}};
  const auto rows=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(f::Id(1),devices,db::NativeCatalogSource::PageZero(created.receipt->filespace_uuid),2,5,{created.receipt->relations[4].object_uuid,{}},budget);
  Check(rows.ok()&&rows.rows.size()==1&&rows.rows[0].metadata.record.payload==Golden(item.descriptor)&&
    c::CatalogEvidencePolicyMatchesDatabase(c::BorrowCatalogMetadataVersion(rows.rows[0].metadata),f::Id(1)),"exact classes keys storage rules and origin survive native reopen");
  const auto zero=d::ReadFilespacePageZeroFromOpenDevice(device);Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==1,"body persistence is not node activation: page zero stays as the create wrote it, an online filespace (owner 2026-11-07); serving is the server's act at open");
  Check(device.Close().ok(),"owned evidence body file closed");
 }}
}
int main(){try{Values();Bindings();Physical();std::cout<<checks<<" evidence policy checks passed (declarations only)\n";return 0;}
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<'\n';return 1;}}

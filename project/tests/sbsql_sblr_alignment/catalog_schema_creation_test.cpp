// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_schema_creation.hpp"
#include "catalog_database_record_codec.hpp"
#include "catalog_schema_record_codec.hpp"
#include "admitted_datatype_cohort.hpp"
#include "native_creation_workspace.hpp"
#include "physical_mga_cow_store.hpp"
#include "../support/owned_temp_directory.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace c=scratchbird::core::catalog;
namespace p=scratchbird::core::platform;
namespace db=scratchbird::storage::database;
namespace disk=scratchbird::storage::disk;
namespace u=scratchbird::core::uuid;
namespace mga=scratchbird::transaction::mga;
thread_local long allocation_budget=-1;
thread_local unsigned allocation_calls=0;
void* operator new(std::size_t size) {
  if(allocation_budget>=0){++allocation_calls;if(allocation_budget--==0)throw std::bad_alloc();}
  if(auto* out=std::malloc(size?size:1))return out;throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
namespace {
using E=c::CatalogInitialSchemaError;
void Check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
p::Uuid Id(unsigned n){p::Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;
  id.bytes[12]=n>>24;id.bytes[13]=n>>16;id.bytes[14]=n>>8;id.bytes[15]=n;return id;}
p::TypedUuid Typed(p::UuidKind kind,unsigned n){return {kind,Id(n)};}
const auto tx=Typed(p::UuidKind::transaction,5);
c::CatalogMetadataVersion Metadata(unsigned n,c::CatalogRecordKind kind,const char* subtype) {
  c::CatalogMetadataVersion m;m.record.header.kind=kind;
  m.record.header.object_uuid=Typed(p::UuidKind::object,n);m.record.header.row_uuid=Typed(p::UuidKind::row,n+100000);
  m.owner_uuid=Typed(p::UuidKind::principal,50);m.audit_uuid=Typed(p::UuidKind::object,51);
  m.security_policy_uuid=Typed(p::UuidKind::object,52);
  m.creator_transaction_uuid=tx;m.creator_local_transaction_id=1;
  m.definition_version=m.schema_epoch=m.security_epoch=m.catalog_generation=m.dependency_generation=m.invalidation_generation=1;
  m.lifecycle=c::CatalogObjectLifecycle::active;m.status=c::CatalogObjectStatus::active;
  m.trace_search_key="INITIAL-SCHEMA-BOUND-INPUT";m.object_subtype=subtype;m.retention_class="catalog_history";
  return m;
}
c::CatalogMetadataVersion Database(unsigned profile=0) {
  auto meta=Metadata(100,c::CatalogRecordKind::database,"database");
  c::CatalogDatabaseRecord record;
  record.database_uuid=Typed(p::UuidKind::database,1);record.database_header_format_major=1;
  record.catalog_manifest_format_version=1;record.page_size=disk::kCanonicalFilespacePageProfiles[profile].page_size_bytes;
  record.creator_transaction_number=1;
  record.datatype_cohort=c::CatalogDatabaseDatatypeCohort{
      {p::UuidKind::object,scratchbird::core::datatypes::kDatatypeCohortV10},10,10};
  const auto encoded=c::EncodeCatalogDatabaseRecord(record);Check(encoded.ok(),"actual database cohort definition");
  meta.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());return meta;
}
struct Item {
  c::CatalogSchemaDefinition definition;
  c::CatalogMetadataVersion metadata,vector_metadata,entry_metadata;
  c::CatalogNameVector vector;
  c::CatalogNameEntry entry;
  std::array<c::CatalogCreationNameEntryInput,1> entries;
};
struct Package {
  c::CatalogMetadataVersion database=Database();
  std::vector<Item> items;
  std::vector<c::CatalogInitialSchemaInput> inputs;
  explicit Package(unsigned count=6) :items(count) {
    for(unsigned i=0;i<count;++i) {
      auto& x=items[i];const auto type=static_cast<c::CatalogSchemaType>(1+i%6);
      const unsigned oid=1000+4*i;
      x.metadata=Metadata(oid,c::CatalogRecordKind::schema,c::CatalogSchemaTypeName(type));
      x.metadata.default_name_uuid=Typed(p::UuidKind::object,oid+2);x.metadata.name_vector_uuid=Typed(p::UuidKind::object,oid+1);
      x.definition.schema_object_uuid=x.metadata.record.header.object_uuid;
      x.definition.database_catalog_object_uuid=database.record.header.object_uuid;
      if(i)x.definition.parent_schema_uuid=items[i-1].definition.schema_object_uuid;
      x.definition.schema_type=type;x.definition.default_filespace_uuid=Typed(p::UuidKind::filespace,2);
      x.definition.default_charset_uuid=Typed(p::UuidKind::object,53);
      x.definition.default_collation_uuid=Typed(p::UuidKind::object,54);
      x.definition.permissions_policy_uuid=x.metadata.security_policy_uuid;
      x.definition.origin_transaction_uuid=tx;x.definition.origin_local_transaction_id=1;
      x.metadata.record.header.parent_uuid=i?x.definition.parent_schema_uuid:database.record.header.object_uuid;
      if(i)x.metadata.owning_schema_uuid={p::UuidKind::schema,x.definition.parent_schema_uuid.value};
      x.vector_metadata=Metadata(oid+1,c::CatalogRecordKind::localized_name,"name_vector");
      x.vector_metadata.record.header.parent_uuid=x.definition.schema_object_uuid;
      x.vector_metadata.default_name_uuid=x.metadata.default_name_uuid;
      x.vector_metadata.name_vector_uuid=x.metadata.name_vector_uuid;
      x.vector_metadata.owning_schema_uuid=x.metadata.owning_schema_uuid;
      x.vector.name_vector_uuid=x.metadata.name_vector_uuid;x.vector.object_uuid=x.definition.schema_object_uuid;
      x.vector.object_class="schema";if(i)x.vector.owning_schema_uuid=x.metadata.owning_schema_uuid;
      x.vector.default_language_tag="en-US";x.vector.default_name_entry_uuid=x.metadata.default_name_uuid;
      x.vector.name_collision_policy_uuid=Typed(p::UuidKind::object,55);
      x.vector.catalog_generation_id=1;x.vector.security_policy_uuid=x.metadata.security_policy_uuid;
      x.vector.lifecycle_state=c::CatalogNameLifecycle::active;
      x.entry_metadata=Metadata(oid+2,c::CatalogRecordKind::localized_name,"name_entry");
      x.entry_metadata.record.header.parent_uuid=x.metadata.name_vector_uuid;
      x.entry_metadata.default_name_uuid=x.metadata.default_name_uuid;x.entry_metadata.name_vector_uuid=x.metadata.name_vector_uuid;
      x.entry_metadata.owning_schema_uuid=x.metadata.owning_schema_uuid;x.entry_metadata.resource_epoch=1;
      auto& e=x.entry;e.name_entry_uuid=x.metadata.default_name_uuid;e.name_vector_uuid=x.metadata.name_vector_uuid;
      e.object_uuid=x.definition.schema_object_uuid;e.object_class="schema";
      e.scope_uuid=x.metadata.record.header.parent_uuid;if(i)e.parent_schema_uuid=x.metadata.owning_schema_uuid;
      e.language_tag="en-US";e.dialect_profile_uuid=Typed(p::UuidKind::object,56);e.identifier_profile_uuid=Typed(p::UuidKind::object,57);
      e.raw_name_text=e.display_name="schema_"+std::to_string(i);
      e.normalized_lookup_key.assign(e.raw_name_text.begin(),e.raw_name_text.end());
      e.catalog_generation_id=1;e.created_transaction_uuid=tx;e.security_policy_uuid=x.metadata.security_policy_uuid;
      e.resource_epoch=e.name_resolution_epoch=1;e.default_for_language=e.default_for_object=true;
      e.lifecycle_state=c::CatalogNameLifecycle::active;
    }
    Rebind();
  }
  void Rebind() {
    inputs.clear();inputs.reserve(items.size());
    for(auto& x:items){x.entries[0]={&x.entry_metadata,&x.entry};
      inputs.push_back({&x.definition,&x.metadata,&x.vector_metadata,&x.vector,x.entries});}
  }
  void Parent(unsigned index,p::TypedUuid parent) {
    auto& x=items[index];x.definition.parent_schema_uuid=parent;
    x.metadata.record.header.parent_uuid=parent;
    x.metadata.owning_schema_uuid={p::UuidKind::schema,parent.value};
    x.vector_metadata.owning_schema_uuid=x.metadata.owning_schema_uuid;
    x.entry_metadata.owning_schema_uuid=x.metadata.owning_schema_uuid;
    x.vector.owning_schema_uuid=x.metadata.owning_schema_uuid;
    x.entry.parent_schema_uuid=x.metadata.owning_schema_uuid;x.entry.scope_uuid=parent;
  }
  auto Assemble(c::CatalogNamedCreationLimits limits={100000,64*1024*1024}) const {
    return c::AssembleLocalInitialSchemaRecords(Id(1),database,tx,"en-US",inputs,limits);
  }
};
void LogicalChecks() {
  Package package;auto made=package.Assemble();Check(made.ok()&&made.records->definitions.size()==6&&
      made.records->names.size()==12,"whole local schema family and names assembled");
  for(std::size_t i=0;i<package.items.size();++i) {
    const auto& meta=made.records->definitions[i].metadata;
    Check(c::CatalogSchemaDefinitionMatchesMetadata(meta),"complete native family metadata binding");
    const auto decoded=c::DecodeCatalogSchemaDefinition(meta.record.payload);
    const auto encoded=c::EncodeCatalogSchemaDefinition(package.items[i].definition);
    Check(decoded.ok()&&std::string(encoded.bytes.begin(),encoded.bytes.end())==meta.record.payload,
          "exact real defaults/origin/permissions and binary payload retained");
    Check(package.items[i].metadata.record.payload.empty(),"producer input not mutated");
  }
  const auto exact=c::CatalogNamedCreationLimits{18,made.records->encoded_bytes};
  Check(package.Assemble(exact).ok(),"exact whole package byte and row bounds");
  Package first(1);const auto first_made=first.Assemble();Check(first_made.ok(),"first-definition budget fixture");
  for(const auto limit:{c::CatalogNamedCreationLimits{17,exact.maximum_encoded_bytes},
      c::CatalogNamedCreationLimits{18,exact.maximum_encoded_bytes-1}}) {
    allocation_calls=0;allocation_budget=0;const auto failed=package.Assemble(limit);
    const bool no_allocation=allocation_calls==0;allocation_budget=-1;
    Check(no_allocation&&failed.error==E::resource_exhausted&&!failed.records,
          "complete one-short bound refuses before any owning copy or heap scratch");
  }
  allocation_calls=0;allocation_budget=0;
  const auto exhausted=package.Assemble({18,first_made.records->encoded_bytes});
  const bool no_exhaustion_copy=allocation_calls==0;allocation_budget=-1;
  Check(no_exhaustion_copy&&exhausted.error==E::resource_exhausted&&!exhausted.records,
        "zero remaining whole-package bytes is resource exhaustion, not malformed context");
  for(unsigned collision=0;collision<6;++collision) {
    Package bad;
    auto& object=bad.database.record.header.object_uuid.value;
    auto& row=bad.database.record.header.row_uuid.value;
    switch(collision) {
      case 0:row=object;break;
      case 1:row=Id(1);break;
      case 2:row=tx.value;break;
      case 3:object=Id(1);break;
      case 4:object=tx.value;break;
      case 5:object=Id(1);row=tx.value;break;
    }
    allocation_calls=0;allocation_budget=0;const auto failed=bad.Assemble();
    const bool no_allocation=allocation_calls==0;allocation_budget=-1;
    Check(no_allocation&&failed.error==E::invalid_database&&!failed.records,
          "enclosing database object/row/database/transaction identities remain distinct without replacements");
  }
  for(unsigned field=0;field<3;++field) {
    Package bad;auto& x=bad.items.back();
    if(field==0)x.metadata.trace_search_key.assign(200000,'x');
    if(field==1)x.vector_metadata.trace_search_key.assign(200000,'x');
    if(field==2)x.entry.display_name.assign(200000,'x');
    allocation_calls=0;allocation_budget=0;const auto failed=bad.Assemble();
    const bool no_allocation=allocation_calls==0;allocation_budget=-1;
    Check(no_allocation&&failed.error==(field==2?E::invalid_names:E::invalid_metadata)&&!failed.records,
          "malformed final metadata/name refuses before oversized owning copies");
  }
  std::reverse(package.inputs.begin(),package.inputs.end());
  Check(package.Assemble().ok(),"child-before-parent input order is not authority or topology");
  std::reverse(package.inputs.begin(),package.inputs.end());
  for(unsigned options=0;options<32;++options)for(unsigned type=1;type<=6;++type) {
    Package variation;auto& x=variation.items.back();
    x.definition.schema_type=static_cast<c::CatalogSchemaType>(type);
    x.metadata.object_subtype=c::CatalogSchemaTypeName(x.definition.schema_type);
    if(!(options&1)) {
      x.definition.parent_schema_uuid={};x.metadata.record.header.parent_uuid=variation.database.record.header.object_uuid;
      x.metadata.owning_schema_uuid={};x.vector_metadata.owning_schema_uuid={};x.entry_metadata.owning_schema_uuid={};
      x.vector.owning_schema_uuid.reset();x.entry.parent_schema_uuid.reset();x.entry.scope_uuid=x.metadata.record.header.parent_uuid;
    }
    if(!(options&2))x.definition.default_filespace_uuid={};
    if(!(options&4))x.definition.default_charset_uuid={};
    if(!(options&8))x.definition.default_collation_uuid={};
    if(!(options&16)){x.definition.permissions_policy_uuid={};x.metadata.security_policy_uuid={};}
    const auto changed=variation.Assemble();
    Check(changed.ok(),"every local schema type/default-presence permutation assembles without inferred defaults");
    const auto expected=c::EncodeCatalogSchemaDefinition(x.definition);
    Check(changed.records->definitions.back().metadata.record.payload==std::string(expected.bytes.begin(),expected.bytes.end()),
          "optional absence and exact typed defaults preserved independently of name disclosure policy");
  }
  for(unsigned fault=0;fault<14;++fault) {
    Package bad;auto& x=bad.items.back();E expected=E::invalid_definition;
    switch(fault) {
      case 0:x.definition.database_catalog_object_uuid=Typed(p::UuidKind::object,99);break;
      case 1:x.definition.origin_transaction_uuid=Typed(p::UuidKind::transaction,99);break;
      case 2:x.definition.origin_local_transaction_id=2;break;
      case 3:x.definition.schema_type=c::CatalogSchemaType::cluster;break;
      case 4:bad.Parent(5,Typed(p::UuidKind::object,99));expected=E::missing_parent;break;
      case 5:bad.Parent(0,x.definition.schema_object_uuid);expected=E::cyclic_parent;break;
      case 6:x.definition.schema_object_uuid=bad.items.front().definition.schema_object_uuid;
        x.metadata.record.header.object_uuid=x.definition.schema_object_uuid;
        x.vector.object_uuid=x.entry.object_uuid=x.vector_metadata.record.header.parent_uuid=x.definition.schema_object_uuid;
        expected=E::duplicate_identity;break;
      case 7:x.metadata.owner_uuid={};expected=E::invalid_metadata;break;
      case 8:x.metadata.security_policy_uuid=Typed(p::UuidKind::object,99);expected=E::invalid_metadata;break;
      case 9:x.metadata.record.payload="legacy_cached_schema_path";expected=E::invalid_metadata;break;
      case 10:x.entry.default_for_object=false;expected=E::invalid_names;break;
      case 11:x.metadata.creator_local_transaction_id=2;expected=E::invalid_metadata;break;
      case 12:bad.database.record.header.kind=c::CatalogRecordKind::schema;expected=E::invalid_database;break;
      case 13:x.metadata.record.header.row_uuid=bad.database.record.header.row_uuid;expected=E::duplicate_identity;break;
    }
    bad.Rebind();const auto failed=bad.Assemble();
    if(failed.error!=expected)std::cerr<<"fault="<<fault<<" observed="<<static_cast<unsigned>(failed.error)<<'\n';
    Check(failed.error==expected&&!failed.records,"malformed final schema has exact refusal/no package");
  }
  Package deep(4096);Check(deep.Assemble().ok(),"deep forest uses bounded iterative traversal, not call-stack recursion");
  deep.Parent(0,deep.items.back().definition.schema_object_uuid);
  Check(deep.Assemble().error==E::cyclic_parent,"deep cyclic graph refuses without recursive stack overflow");
  allocation_calls=0;allocation_budget=1000000;const auto counted=package.Assemble();
  const auto sites=allocation_calls;allocation_budget=-1;Check(counted.ok()&&sites>0,"actual owning allocation sweep calibration");
  for(unsigned site=0;site<sites;++site) {
    allocation_calls=0;allocation_budget=site;const auto failed=package.Assemble();
    const bool consumed=allocation_budget<0;allocation_budget=-1;
    Check(consumed&&failed.error==E::resource_exhausted&&!failed.records,"each actual allocation failure preserves no partial package");
  }
}
void PhysicalChecks() {
  scratchbird::tests::OwnedTempDirectory artifacts;
  for(unsigned profile=0;profile<5;++profile) {
    const auto& p=disk::kCanonicalFilespacePageProfiles[profile];Package package;
    package.database=Database(profile);const auto made=package.Assemble();Check(made.ok(),"schema producer package");
    db::NativeCreationCatalogSeed seed;seed[0]=made.records->definitions;seed[1]=made.records->names;
    db::NativeCreationCatalogIndexes indexes;
    for(unsigned role=0;role<2;++role) {
      scratchbird::storage::page::NativeBtreeDependencies deps;
      deps.index_uuid=Id(8010+role);deps.descriptor_generation=deps.storage_generation=1;
      deps.key_profile_uuid=Id(8020+role);deps.visibility_profile_uuid=Id(8030+role);
      deps.dependency_map_uuid=Id(8040+role);deps.dependency_map_sha256.fill(static_cast<p::byte>(1+role));
      indexes[role]=db::NativeCreationCatalogIndexBinding{Id(8000+role),deps};
    }
    db::NativeFilespaceInitializationRequest request;
    request.bootstrap={Id(1),Id(2),p.uuid,disk::kNativeBootstrapIntegrityProfile,{},p.page_size_bytes,1,0,1,7};
    request.operation_uuid=Id(3);request.writer_uuid=Id(4);request.policy_snapshot_uuid=Id(10);
    request.creator.transaction_uuid=tx;request.creator.local_id=mga::MakeLocalTransactionId(1);
    request.creator.scope=mga::TransactionScope::local_node;request.total_pages=96;request.creation_utc_millis=1789357072000ULL;
    u::StandaloneUuidV7Issuer issuer({Id(1),Id(10)},{{},0,1000});
    const auto path=(artifacts.path()/std::to_string(profile)).string();disk::FileDevice device;
    Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"actual native schema filespace owner");
    const auto budget=512*p::u64{p.page_size_bytes};
    const auto created=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,request,seed,indexes,budget,issuer);
    Check(created.ok(),"actual typed schema package writes native construction graph");
    Check(device.Close().ok()&&device.Open(path,disk::FileOpenMode::open_existing).ok(),"independent native schema reopen");
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(2),p.uuid,&device}};
    const auto rows=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(Id(1),devices,created.receipt->checkpoint,
        2,1,{indexes[0]->relation_uuid,indexes[0]->dependencies},budget);
    Check(rows.ok()&&rows.rows.size()==seed[0].size(),"actual persisted native schema relation visibility");
    for(const auto& row:rows.rows) {
      const auto original=std::find_if(seed[0].begin(),seed[0].end(),[&](const auto& r) {
        return r.metadata.record.header.object_uuid.value==row.metadata.record.header.object_uuid.value;});
      Check(original!=seed[0].end()&&row.metadata.record.payload==original->metadata.record.payload&&
          c::CatalogSchemaDefinitionMatchesMetadata(row.metadata),"exact full schema definition survives native bytes/reopen");
    }
    const auto names=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(Id(1),devices,created.receipt->checkpoint,
        2,2,{indexes[1]->relation_uuid,indexes[1]->dependencies},budget);
    Check(names.ok()&&names.rows.size()==seed[1].size(),"all actual schema name records persist");
    for(const auto& row:names.rows) {
      const auto original=std::find_if(seed[1].begin(),seed[1].end(),[&](const auto& r) {
        return r.metadata.record.header.object_uuid.value==row.metadata.record.header.object_uuid.value;});
      Check(original!=seed[1].end()&&row.name_payload,"persisted actual name identity exists");
      const auto encode=[](const auto& value) {
        return std::visit([](const auto& name) {
          if constexpr(std::is_same_v<std::decay_t<decltype(name)>,c::CatalogNameVector>)return c::EncodeCatalogNameVector(name);
          else return c::EncodeCatalogNameEntry(name);
        },value);
      };
      Check(encode(*original->name).bytes==encode(*row.name_payload).bytes,
            "exact supplied schema display/scope/language/profile/default/policy bytes survive native storage");
    }
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(device);
    Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==7,"storage construction does not falsely activate node/serve");
    Check(device.Close().ok(),"release native schema owner");
  }
  artifacts.Cleanup();
}
} // namespace
int main() try {LogicalChecks();PhysicalChecks();std::cout<<"PASS complete initial schema family construction\n";return 0;}
catch(const std::exception& error){allocation_budget=-1;std::cerr<<error.what()<<'\n';return 1;}

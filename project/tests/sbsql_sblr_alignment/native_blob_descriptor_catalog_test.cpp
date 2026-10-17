// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/owned_bootstrap_database_fixture.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "ddl/create_api.hpp"
#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/name_registry.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_storage_identity.hpp"
#include "mga_relation_store/native_blob_descriptor_validation.hpp"
#include <array>
#include <fstream>
#include <iterator>
#include <tuple>

namespace api=scratchbird::engine::internal_api;
namespace db=scratchbird::storage::database;
namespace dt=scratchbird::core::datatypes;
namespace t=scratchbird::tests;
namespace {
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
void Good(const api::EngineApiResult& result,const char* why){
  if(!result.ok)for(const auto& d:result.diagnostics)std::cerr<<d.code<<':'<<d.detail<<'\n';
  Check(result.ok,why);
}
std::string Bytes(const std::filesystem::path& path){
  std::ifstream file(path,std::ios::binary);
  if(!file)return {};
  return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
class Fixture final {
 public:
  Fixture(){
    config.path=(directory.path()/"native_blob.sbdb").string();
    config.database_uuid={scratchbird::core::platform::UuidKind::database,t::FixtureUuid(0x424c4341,1)};
    config.filespace_uuid={scratchbird::core::platform::UuidKind::filespace,t::FixtureUuid(0x424c4341,2)};
    t::ConfigureCredentialedFixtureBootstrap(config);
    const auto created=db::CreateDatabaseFile(config);
    Check(created.ok()&&created.create_finality==db::DatabaseCreateFinalityClass::committed,
        "credentialed real-file BLOB fixture creation failed");
    Begin(false);
  }
  ~Fixture()noexcept{
    if(active){api::EngineRollbackTransactionRequest rollback;rollback.context=context;
      if(!api::EngineRollbackTransaction(rollback).ok)std::terminate();}
  }
  void Begin(bool read_only){
    context=t::BootstrapFixtureOwnerContext(config);
    const auto catalog=db::ReadDatabaseCatalogIdentity(config.path,context.database_uuid);
    Check(catalog.ok()&&catalog.record->datatype_cohort.has_value(),"actual persisted datatype cohort absent");
    const auto& cohort=*catalog.record->datatype_cohort;
    context.datatype_catalog_snapshot_uuid=cohort.catalog_snapshot_uuid.value;
    context.datatype_catalog_generation=cohort.catalog_generation;
    context.datatype_registry_generation=cohort.registry_generation;
    session=std::make_unique<t::FixtureEngineSession>(context);
    api::EngineBeginTransactionRequest begin;begin.context=context;begin.isolation_level="read_committed";
    if(read_only)begin.transaction_policy_profile.encoded_profiles.push_back("transaction_read_mode:read_only");
    const auto result=api::EngineBeginTransaction(begin);Good(result,"actual BLOB fixture transaction begin");
    context.transaction_uuid=result.transaction_uuid;context.local_transaction_id=result.local_transaction_id;
    context.snapshot_visible_through_local_transaction_id=result.snapshot_visible_through_local_transaction_id;
    context.transaction_isolation_level=result.isolation_level;context.read_only_mode=result.read_only;active=true;
  }
  void CommitClose(){
    api::EngineCommitTransactionRequest commit;commit.context=context;
    Good(api::EngineCommitTransaction(commit),"BLOB catalog transaction durable completion");active=false;
    session->Close();session.reset();
  }
  void Finish(){CommitClose();directory.Cleanup();}
  t::OwnedTempDirectory directory;
  db::DatabaseCreateConfig config;
  api::EngineRequestContext context;
  std::unique_ptr<t::FixtureEngineSession> session;
  bool active=false;
};
api::EngineColumnDefinition Column(const api::EngineRequestContext& context){
  const auto manifest=dt::LoadCurrentCoreDatatypeCatalogManifest();Check(manifest.ok(),"current descriptor manifest unavailable");
  const auto descriptor=dt::LookupDatatypeCatalogRow(manifest.manifest,dt::CanonicalTypeId::blob);
  Check(descriptor.ok()&&descriptor.manifest.descriptor_rows.size()==1,"native BLOB manifest descriptor missing");
  const auto& declaration=descriptor.manifest.descriptor_rows.front();
  dt::DatatypeStorageIdentityV3 binding;
  Check(dt::LookupDatatypeStorageIdentityV3(context.datatype_catalog_snapshot_uuid,
      context.datatype_catalog_generation,context.datatype_registry_generation,
      declaration.descriptor_uuid.value,declaration.descriptor_epoch,&binding)&&binding.codec.has_value(),
      "actual issued BLOB storage tuple unavailable");
  api::EngineColumnDefinition column;column.names.push_back({"en","primary","","payload",true});
  column.requested_column_uuid=api::GenerateCrudEngineUuid("object");
  column.descriptor.descriptor_uuid=api::GenerateCrudEngineUuid("object");
  column.descriptor.descriptor_kind="scalar";column.descriptor.canonical_type_name="blob";
  column.descriptor.datatype_descriptor_uuid=binding.descriptor_uuid;
  column.descriptor.datatype_descriptor_generation=binding.descriptor_generation;
  column.descriptor.type_uuid=binding.type_uuid;
  column.descriptor.datatype_cohort={context.datatype_catalog_snapshot_uuid,
      context.datatype_catalog_generation,context.datatype_registry_generation};
  column.descriptor.encoded_descriptor="type=blob";return column;
}
auto State(const api::EngineRequestContext& context){
  const auto catalog=api::LoadCatalogObjectLifecycleState(context);Check(catalog.ok,"read actual object/column/name state");
  const auto names=api::LoadNameRegistryState(context,context.local_transaction_id);Check(names.ok,"read actual name registry");
  const auto crud=api::LoadCrudState(context);Check(crud.ok,"read actual table state");
  return std::tuple{catalog.state.objects.size(),catalog.state.names.size(),catalog.state.columns.size(),
      names.state.entries.size(),crud.state.tables.size(),catalog.state.metadata_epoch,catalog.state.name_resolution_epoch};
}
api::EngineCreateTableResult Create(Fixture& f,std::string name,api::EngineColumnDefinition column){
  t::FixtureEngineRequest<api::EngineCreateTableRequest> request(*f.session,f.context);
  request.target_schema.uuid=f.context.current_schema_uuid;request.target_schema.object_kind="schema";
  request.table_names.push_back({"en","primary","",std::move(name),true});
  request.table_columns.push_back(std::move(column));return api::EngineCreateTable(request);
}
void Run(){
  t::ConfigureOwnedBootstrapFixtureMemory("native-blob-catalog-modifier-validation");
  Fixture f;
  const auto schema=api::GenerateCrudEngineUuid("object");
  {t::FixtureEngineRequest<api::EngineCreateSchemaRequest> request(*f.session,f.context);
    request.target_object.uuid=schema;request.target_object.object_kind="schema";
    request.localized_names.push_back({"en","primary","","native_blob_schema",true});
    Good(api::EngineCreateSchema(request),"credentialed schema creation");}
  f.context.current_schema_uuid=schema;
  const auto metadata=std::filesystem::path(f.config.path+".sb.mga_relation_metadata");
  std::vector<api::EngineUuid> tables;
  std::vector<std::vector<std::pair<std::string,std::string>>> retained;
  // Creation and load-only reopen, including a presentation alias and each
  // single redundant nullability spelling, are genuine positive routes.
  for(unsigned mode=0;mode<5;++mode){auto column=Column(f.context);
    if(mode==1||mode==4){column.descriptor.canonical_type_name=mode==1?"localized_blob_alias":"localized.blob.alias";
      column.descriptor.encoded_descriptor="canonical="+column.descriptor.canonical_type_name;}
    if(mode==2)column.descriptor.encoded_descriptor+=";nullability=nullable";
    if(mode==3){column.nullable=false;column.descriptor.encoded_descriptor+=";not_null=true";}
    const auto created=Create(f,"binary_payload_"+std::to_string(mode),column);Good(created,"native binary BLOB creation refused");
    Check(!created.effective_table_descriptor.descriptor_uuid.is_nil(),"creation omitted real relation publication");
    tables.push_back(created.primary_object.uuid);
    const auto before=Bytes(metadata);Check(!before.empty(),"creation did not persist a relation descriptor");
    const auto loaded=api::LoadMgaRelationStorageDescriptor(f.context,tables.back());
    Check(loaded.ok&&loaded.descriptor.columns.size()==1,"actual persisted BLOB descriptor load failed");
    Check(Bytes(metadata)==before,"load-only BLOB descriptor changed durable metadata");
    const auto& stored=loaded.descriptor.columns.front();
    Check(stored.nullable==column.nullable&&stored.value_descriptor.type_uuid==column.descriptor.type_uuid&&
        stored.value_descriptor.datatype_descriptor_uuid==column.descriptor.datatype_descriptor_uuid&&
        stored.value_descriptor.datatype_cohort==column.descriptor.datatype_cohort&&stored.character_length==0&&
        stored.charset_uuid.is_nil()&&stored.collation_uuid.is_nil(),"persisted BLOB identity/resource/nullability changed");
    Check(api::ValidateNativeBinaryBlobDescriptorV1(stored.value_descriptor,stored.nullable).ok(),"real retained binary profile invalid");
    retained.push_back(api::SerializeMgaRelationStorageDescriptor(loaded.descriptor));
  }
  const auto baseline=State(f.context);
  const auto durable=Bytes(metadata);
  unsigned ordinal=0;
  for(const char* field:{"text_resource_storage","charset_generation","collation_generation","resource_epoch",
      "character_length","char_length"})for(const char* value:{"","0","large_object"}){
    for(bool alias:{false,true}){
      auto column=Column(f.context);
      if(alias){column.descriptor.canonical_type_name="localized_blob_alias";
        column.descriptor.encoded_descriptor="canonical=localized_blob_alias";}
      column.descriptor.encoded_descriptor+=";"+std::string(field)+"="+value;
      const auto result=Create(f,"refused_text_decoration_"+std::to_string(ordinal++),column);
      Check(!result.ok,"native binary BLOB text decoration created a table");
      Check(State(f.context)==baseline&&Bytes(metadata)==durable,"refused BLOB created object/name/column/storage effects");
    }
  }
  for(unsigned mutation=0;mutation<13;++mutation){auto column=Column(f.context);
    switch(mutation){
      case 0:column.descriptor.datatype_cohort={};break;
      case 1:column.descriptor.datatype_cohort.registry_generation=0;break;
      case 2:++column.descriptor.datatype_cohort.catalog_generation;break;
      case 3:column.descriptor.type_uuid={};break;
      case 4:column.descriptor.datatype_descriptor_uuid={};break;
      case 5:++column.descriptor.datatype_descriptor_generation;break;
      case 6:column.descriptor.charset_uuid=schema;break;
      case 7:column.descriptor.collation_uuid=schema;break;
      case 8:column.descriptor.canonical_type_name="integer";column.descriptor.encoded_descriptor="type=integer";break;
      case 9:++column.descriptor.datatype_cohort.registry_generation;break;
      case 10:column.descriptor.datatype_cohort.catalog_snapshot_uuid.bytes.back()^=1;break;
      case 11:column.descriptor.datatype_descriptor_generation=0;break;
      case 12:column.descriptor.canonical_type_name="localized_blob_alias";
        column.descriptor.datatype_cohort={};column.descriptor.encoded_descriptor="canonical=localized_blob_alias";break;
    }
    Check(!Create(f,"refused_binding_"+std::to_string(mutation),column).ok,"malformed BLOB binding admitted at DDL");
    Check(State(f.context)==baseline&&Bytes(metadata)==durable,"malformed BLOB binding published effects");
  }
  f.CommitClose();f.Begin(true);f.context.current_schema_uuid=schema;
  for(std::size_t i=0;i<tables.size();++i){const auto before=Bytes(metadata);
    const auto loaded=api::LoadMgaRelationStorageDescriptor(f.context,tables[i]);
    Check(loaded.ok&&api::SerializeMgaRelationStorageDescriptor(loaded.descriptor)==retained[i],
        "independent read-only reopen changed exact retained BLOB tuple");
    Check(Bytes(metadata)==before,"independent read-only reopen rewrote durable descriptor");
    auto mismatch=f.context;mismatch.transaction_uuid=api::GenerateCrudEngineUuid("transaction");
    Check(!api::LoadMgaRelationStorageDescriptor(mismatch,tables[i]).ok&&Bytes(metadata)==before,
        "refused exact-transaction reopen modified durable BLOB metadata");
  }
  f.Finish();
}
}
int main(){try{Run();std::cout<<checks<<" checks PASS\n";return 0;}
  catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

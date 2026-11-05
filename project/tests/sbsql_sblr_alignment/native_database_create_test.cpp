// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// SB_NATIVE_DATABASE_CREATE_PRODUCER (M1 step 2c): the production native create
// producer, at the storage level. It writes a real file at every native page
// profile, which is reopened independently and read back relation by relation
// through the production committed-catalog reader. Hosted open, credential
// authentication and the startup transaction are exercised by the native
// enrollment test; this test does not claim them.
#include "native_database_create.hpp"
#include "catalog_native_catalog_index.hpp"
#include "catalog_name_record_codec.hpp"
#include "catalog_schema_definition.hpp"
#include "native_catalog_relation_lease_reader.hpp"
#include "physical_mga_cow_store.hpp"
#include "filespace_page_zero.hpp"
#include "admitted_datatype_cohort.hpp"
#include "uuid.hpp"
#include "../support/owned_temp_directory.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <source_location>
#include <string>

namespace c=scratchbird::core::catalog;namespace p=scratchbird::core::platform;
namespace db=scratchbird::storage::database;namespace d=scratchbird::storage::disk;namespace u=scratchbird::core::uuid;
namespace {
int checks=0;
void Check(bool ok,const char* what,std::source_location at=std::source_location::current()) {
  ++checks;
  if(!ok){std::fprintf(stderr,"FAIL %s (line %u)\n",what,at.line());std::exit(1);}
}
// Independently derived (tests/support/engine_statement_fixture.hpp): PBKDF2 verifiers.
constexpr const char* kFingerprint="local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";
p::Uuid Fixed(unsigned n){p::Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[14]=n>>8;id.bytes[15]=n;return id;}
db::NativeDatabaseCreateRequest Request(const d::CanonicalFilespacePageProfile& profile) {
  db::NativeDatabaseCreateRequest r;
  r.policy_seed_pack_root=SB_DEFAULT_POLICY_PACK_ROOT;r.database_name="orders";r.database_default_language="en";
  r.page_size_profile_uuid=profile.uuid;r.page_size_bytes=profile.page_size_bytes;
  // As the enrollment fixture provisions them: 8 KiB needs more pages.
  r.total_pages=profile.page_size_bytes==8192?1024:512;
  r.creation_utc_millis=1789357072000ULL;r.engine_uuid=Fixed(4);
  r.datatype_cohort={{p::UuidKind::object,scratchbird::core::datatypes::kDatatypeCohortV11},11,11};
  r.bootstrap_principal_name="BootstrapAdmin";r.bootstrap_credential_fingerprint=kFingerprint;
  r.service_principal_name="NodeService";r.service_credential_fingerprint=kFingerprint;
  r.maximum_retained_image_bytes=db::NativeHostedSourceBackingBytes(profile.page_size_bytes);
  return r;
}
}  // namespace

int main() {
  scratchbird::tests::OwnedTempDirectory artifacts;
  unsigned file=0;
  const auto issuer=[]{return u::StandaloneUuidV7Issuer({Fixed(1),Fixed(10)},{{},0,1000});};

  // Refusals happen before the first write.
  {
    const auto& profile=d::kCanonicalFilespacePageProfiles.front();
    using E=db::NativeDatabaseCreateError;
    for(unsigned n=0;n<6;++n) {
      auto r=Request(profile);E expected=E::invalid_request;
      std::string bad_fingerprint=std::string(kFingerprint)+"x";
      switch(n){
        case 0:r.database_name={};break;
        case 1:r.service_principal_name={};expected=E::invalid_principal;break;
        case 2:r.service_credential_fingerprint=bad_fingerprint;expected=E::invalid_principal;break;
        case 3:r.bootstrap_credential_fingerprint={};expected=E::invalid_principal;break;
        case 4:r.total_pages=64;expected=E::capacity_exceeded;break;
        case 5:r.policy_seed_pack_root="/nonexistent-policy-pack";expected=E::policy_assembly;break;
      }
      const auto path=(artifacts.path()/("refused-"+std::to_string(n))).string();d::FileDevice device;
      Check(device.Open(path,d::FileOpenMode::create_new).ok(),"owned empty device");
      auto issue=issuer();
      const auto made=db::CreateNativeDatabaseOnOpenDevice(device,r,issue);
      if(made.error!=expected)std::fprintf(stderr,"case %u error=%d\n",n,static_cast<int>(made.error));
      const auto size=device.Size();
      Check(made.error==expected&&!made.receipt&&size.ok()&&!size.size_bytes,"refused before any write, with the exact cause");
      Check(device.Close().ok(),"refused device closes");
    }
  }

  for(const auto& profile:d::kCanonicalFilespacePageProfiles) {
    const auto path=(artifacts.path()/("native-"+std::to_string(file++))).string();d::FileDevice device;
    Check(device.Open(path,d::FileOpenMode::create_new).ok(),"owned empty primary device");
    auto issue=issuer();
    const auto request=Request(profile);
    const auto made=db::CreateNativeDatabaseOnOpenDevice(device,request,issue);
    if(!made.ok())std::fprintf(stderr,"profile %u error=%d policy=%d name=%d database=%d schema=%d workspace=%d\n",
        profile.page_size_bytes,static_cast<int>(made.error),static_cast<int>(made.policy_error),static_cast<int>(made.name_error),
        static_cast<int>(made.database_error),static_cast<int>(made.schema_error),static_cast<int>(made.workspace_error));
    Check(made.ok(),"the production producer writes a complete native database");
    const auto& r=*made.receipt;
    Check(u::IsEngineIdentityUuid(r.database_uuid)&&u::IsEngineIdentityUuid(r.filespace_uuid)&&
          r.workspace.database_uuid==r.database_uuid&&r.workspace.filespace_uuid==r.filespace_uuid,
          "fresh UUIDv7 database and filespace identities bind the written file");
    Check(r.workspace.relation_uuids[1]==c::kNativeNameCatalogRelationUuid,
          "the name/uuid catalog table carries the identity it has in every database");
    Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing).ok(),"independent reopen");
    const std::vector<d::NativeFilespaceDevice> devices{{r.filespace_uuid,profile.uuid,&device}};
    const auto budget=request.maximum_retained_image_bytes;
    std::array<std::vector<db::NativeCatalogVersionRow>,6> relations;
    for(unsigned i=0;i<6;++i) {
      const db::NativeCatalogRelationBinding binding{r.workspace.relation_uuids[i],r.workspace.index_dependencies[i]};
      auto read=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(r.database_uuid,devices,r.workspace.checkpoint,2,
          static_cast<p::u16>(i+1),binding,budget);
      Check(read.ok(),"every catalog relation reads back through the production reader");
      relations[i]=std::move(read.rows);
    }
    Check(relations[2].empty()&&!r.workspace.index_dependencies[2]&&
          r.workspace.index_dependencies[0]&&r.workspace.index_dependencies[1]&&r.workspace.index_dependencies[3]&&
          r.workspace.index_dependencies[4]&&r.workspace.index_dependencies[5],"every populated relation is indexed");

    // The hosted startup provenance rule: no row is newer than its catalog. The
    // initial catalog roots carry generation/schema/security 1 and resource 0,
    // so every definition declares no resource dependency; only a name entry
    // carries a resource epoch, the one its lookup keys were generated under
    // (SB_DRIFT_DEFINITION_RESOURCE_EPOCH).
    for(unsigned i=0;i<6;++i)for(const auto& row:relations[i]) {
      const auto& m=row.metadata;
      Check(m.catalog_generation==1&&m.schema_epoch==1&&m.security_epoch==1,
            "every row is at the initial catalog generation and epochs");
      if(m.object_subtype=="name_entry")Check(m.resource_epoch!=0,"a name entry carries its key's resource epoch");
      else Check(m.resource_epoch==0,"no initial row claims a resource dependency the catalog lacks");
    }

    // Every definition carries exactly one name, except the construction intent.
    std::map<p::Uuid,const db::NativeCatalogVersionRow*> definitions;
    std::map<p::Uuid,std::string> names;
    for(unsigned i=0;i<6;++i)for(const auto& row:relations[i]) {
      if(!row.name_payload){definitions.emplace(row.metadata.record.header.object_uuid.value,&row);continue;}
      if(const auto* entry=std::get_if<c::CatalogNameEntry>(&*row.name_payload))
        Check(names.emplace(entry->object_uuid.value,entry->raw_name_text).second,"one default name per object");
    }
    std::size_t unnamed=0,by_uuid=0,addressed=0;
    for(const auto& [id,row]:definitions) {
      const auto kind=row->metadata.record.header.kind;
      const auto found=names.find(id);
      if(row->metadata.object_subtype=="node_construction_intent"){++unnamed;Check(found==names.end(),"the construction intent is unnamed");continue;}
      Check(found!=names.end(),"every other definition is named");
      const bool by_name=kind==c::CatalogRecordKind::user_account||kind==c::CatalogRecordKind::role_account||
          kind==c::CatalogRecordKind::group_account||kind==c::CatalogRecordKind::schema||
          kind==c::CatalogRecordKind::database||kind==c::CatalogRecordKind::filespace;
      if(by_name){++addressed;Check(found->second!=u::UuidToString(id),"an object addressed by name keeps its name");}
      else{++by_uuid;Check(found->second==u::UuidToString(id),"any other object is named by its UUID text");}
    }
    Check(unnamed==1&&addressed>0&&by_uuid>0,"one construction intent, named and UUID-named objects");

    // The database, its primary filespace and the schema tree.
    std::multiset<std::string> schemas;std::size_t databases=0,filespaces=0,cluster_typed=0;
    for(const auto& [id,row]:definitions) {
      const auto kind=row->metadata.record.header.kind;
      if(kind==c::CatalogRecordKind::database){++databases;Check(names[id]=="orders","the database is named by CREATE's data");}
      if(kind==c::CatalogRecordKind::filespace){++filespaces;Check(names[id]=="filespace_0","the primary filespace is filespace_0");}
      if(kind==c::CatalogRecordKind::schema){
        schemas.insert(names[id]);
        const auto schema=c::DecodeCatalogSchemaDefinition(row->metadata.record.payload);
        Check(schema.ok()&&row->metadata.authority_scope==c::CatalogAuthorityScope::local,"every schema is a local object");
        cluster_typed+=schema.definition->schema_type==c::CatalogSchemaType::cluster;
      }
    }
    // Owner 2026-11-04: sys.catalog.mga (not sys.mga), and the cluster schema
    // with its canonical children in every database.
    const std::multiset<std::string> tree{"sys","catalog","mga","metrics","agents","security","configuration","management",
        "fn","udr","parser","storage","audit","compatibility","information","catalog_readable","diagnostics",
        "cluster","sys","security","metrics","application","user","users","public","remote","emulated","app"};
    Check(databases==1&&filespaces==1&&schemas==tree,"one database, one primary filespace and the 28-schema system tree");
    Check(cluster_typed==6,"the cluster schema and its five children are cluster-type local schemas");

    // The catalog index rows behind every binding.
    for(unsigned i=0;i<6;++i) {
      const auto& deps=r.workspace.index_dependencies[i];if(!deps)continue;
      const auto map=definitions.find(deps->dependency_map_uuid);
      Check(map!=definitions.end(),"each index binding's dependency map is a catalog row");
      const auto decoded=c::DecodeCatalogNativeCatalogIndexDependencyMap(map->second->metadata.record.payload);
      Check(decoded.ok()&&decoded.record->relation_role==i+1&&decoded.record->relation_uuid==r.workspace.relation_uuids[i]&&
            decoded.record->index_uuid==deps->index_uuid,"the map names its relation and index");
      Check(definitions.contains(deps->key_profile_uuid)&&definitions.contains(deps->visibility_profile_uuid),
            "both profiles are catalog rows");
    }
    // The principals: bootstrap, L5 and one per agent type, all in the security relation.
    Check(names[r.bootstrap_principal_uuid]=="BootstrapAdmin"&&names[r.service_principal_uuid]=="NodeService",
          "the bootstrap and L5 principals carry the names CREATE supplied");
    const auto zero=d::ReadFilespacePageZeroFromOpenDevice(device);
    Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==7,"construction is not activation: the file stays initializing");
    std::printf("profile %u: %zu definitions, %zu names, security %zu, configuration %zu\n",profile.page_size_bytes,
        definitions.size(),names.size(),relations[3].size(),relations[4].size());
    Check(device.Close().ok(),"owned file closed");
  }
  std::printf("PASS native database create producer checks=%d (storage level; hosted open separate)\n",checks);
  return 0;
}

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Real database file create/reopen in separate processes. Not SQL/IPC evidence.
#include "database_lifecycle.hpp"
#include "resource_seed_pack.hpp"
#include "memory.hpp"
#include "uuid.hpp"
#include "catalog_page.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog/resource_catalog_admission.hpp"
#include "local_transaction_store.hpp"
#include "transaction_inventory.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace db = scratchbird::storage::database;
namespace r = scratchbird::core::resources;
namespace uuid = scratchbird::core::uuid;
namespace memory = scratchbird::core::memory;
namespace fs = std::filesystem;
namespace page = scratchbird::storage::page;
namespace disk = scratchbird::storage::disk;
namespace platform = scratchbird::core::platform;
using scratchbird::core::platform::UuidKind;
namespace engine = scratchbird::engine::internal_api;
namespace mga = scratchbird::transaction::mga;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Good(const db::DatabaseLifecycleResult& result) {
  if (!result.ok()) {
    std::cerr << result.diagnostic.diagnostic_code << ':' << result.diagnostic.message_key;
    for (const auto& arg : result.diagnostic.arguments) std::cerr << ' ' << arg.key << '=' << arg.value;
    std::cerr << '\n';
    throw std::runtime_error("database lifecycle failure");
  }
}
std::uint64_t Now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string Quote(const std::string& value) {
  std::string result="'";
  for (char c:value) result += c=='\'' ? "'\\''" : std::string(1,c);
  return result+"'";
}
std::string IdentityBytes(const r::ResourceSeedCatalogImage& image) {
  std::string bytes;
  const auto append=[&](const platform::Uuid& id) {
    bytes.append(reinterpret_cast<const char*>(id.bytes.data()),id.bytes.size());
  };
  for (const auto& row:image.charsets) { append(row.resource_uuid);append(row.default_collation_uuid); }
  for (const auto& row:image.collations) { append(row.resource_uuid);append(row.charset_uuid); }
  for (const auto& row:image.timezones) append(row.resource_uuid);
  for (const auto& row:image.aliases) append(row.canonical_resource_uuid);
  for (const auto& row:image.artifacts) append(row.artifact_uuid);
  return bytes;
}
void ResourceAdmission(const fs::path& path) {
  db::DatabaseOpenConfig open; open.path=path.string(); open.read_only=true;
  open.suppress_background_agents=true;
  const auto initial=db::OpenDatabaseFile(open); Good(initial);
  const auto& image=initial.state.resource_seed_catalog;
  Require(!image.charsets.empty() && !image.collations.empty(),"resource descriptor oracle missing");
  engine::EngineRequestContext context;
  context.database_path=path.string(); context.database_uuid=initial.state.database_uuid.value;
  context.resource_epoch=image.resource_epoch;
  auto inventory=initial.state.local_transaction_inventory;
  const auto begin=[&](bool read_only) {
    const auto id=uuid::GenerateEngineIdentityV7(UuidKind::transaction,Now());
    Require(id.ok(),"resource admission transaction identity failed");
    const auto begun=read_only
        ? mga::BeginLocalReadOnlyTransaction(inventory,id.value,Now())
        : mga::BeginLocalTransaction(inventory,id.value,Now());
    Require(begun.ok(),"resource admission actual transaction begin failed");
    const auto persisted=db::PersistLocalTransactionInventoryToDatabase(path.string(),begun.inventory);
    Require(persisted.ok(),"resource admission transaction persistence failed");
    inventory=persisted.inventory;
    context.transaction_uuid=id.value.value;
    context.local_transaction_id=begun.entry.identity.local_id.value;
  };
  begin(false);
  unsigned checks=0;
  const auto check=[&](bool condition,const char* detail){++checks;Require(condition,detail);};
  const auto lookup=[&](const engine::EngineRequestContext& candidate) {
    return engine::LookupEngineResourceDescriptorByUuid(candidate,image.charsets.front().resource_uuid,"charset");
  };
  const auto refused=[&](const engine::EngineRequestContext& candidate,const char* key) {
    const auto resource=lookup(candidate);
    check(!resource.ok && resource.diagnostic.error && resource.diagnostic.code=="CATALOG.INVALID_INPUT" && resource.diagnostic.message_key==key &&
          resource.diagnostic.canonical_metadata.has_value() &&
          !resource.resource_descriptor.present && resource.resource_descriptor.resource_uuid.is_nil(),
          "resource lookup published authority for invalid context");
    const auto timezone=engine::LookupEngineTimezoneSeedAuthority(candidate);
    check(!timezone.ok && timezone.diagnostic.error && timezone.diagnostic.code=="CATALOG.INVALID_INPUT" && timezone.diagnostic.message_key==key &&
          timezone.diagnostic.canonical_metadata.has_value() &&
          !timezone.authority.active && timezone.authority.timezone_names.empty(),
          "timezone lookup published authority for invalid context");
  };
  const auto charset=lookup(context);
  check(charset.ok && charset.resource_descriptor.resource_uuid==image.charsets.front().resource_uuid &&
        charset.resource_descriptor.default_collation_uuid==image.charsets.front().default_collation_uuid &&
        charset.resource_descriptor.family_epoch==image.charsets.front().family_epoch &&
        charset.resource_descriptor.canonical_name==image.charsets.front().canonical_name,
        "actual charset lookup changed binary identity or cohort");
  const auto collation=engine::LookupEngineResourceDescriptorByUuid(
      context,image.collations.front().resource_uuid,"COLLATION");
  check(collation.ok && collation.resource_descriptor.resource_uuid==image.collations.front().resource_uuid &&
        collation.resource_descriptor.parent_resource_uuid==image.collations.front().charset_uuid &&
        collation.resource_descriptor.family_version==image.collations.front().family_version,
        "actual collation lookup changed binary identity or parent");
  const auto zone=engine::LookupEngineTimezoneSeedAuthority(context);
  check(zone.ok && zone.authority.active && zone.authority.timezone_epoch==image.timezone_epoch &&
        zone.authority.timezone_records==image.timezone_records,
        "actual timezone lookup rejected exact active transaction");
  auto bad=context; bad.database_uuid={}; refused(bad,"catalog.resource.database_required");
  bad=context; bad.database_path.clear(); refused(bad,"catalog.resource.database_required");
  bad=context; bad.database_uuid.bytes[6]=0x40;
  refused(bad,"catalog.resource.database_identity_mismatch");
  bad=context; bad.database_uuid.bytes[15]^=1;
  refused(bad,"catalog.resource.database_identity_mismatch");
  bad=context; bad.transaction_uuid={}; refused(bad,"catalog.resource.transaction_required");
  bad=context; bad.local_transaction_id=0; refused(bad,"catalog.resource.transaction_required");
  for (unsigned version=1;version<=6;++version) {
    bad=context; bad.transaction_uuid.bytes[6]=static_cast<unsigned char>(version<<4);
    refused(bad,"catalog.resource.transaction_invalid");
    auto identity=image.charsets.front().resource_uuid;
    identity.bytes[6]=static_cast<unsigned char>(version<<4);
    const auto result=engine::LookupEngineResourceDescriptorByUuid(context,identity,"charset");
    check(!result.ok && result.diagnostic.code=="CATALOG.INVALID_INPUT" && result.diagnostic.message_key=="catalog.resource.uuid_invalid" &&
          result.diagnostic.canonical_metadata.has_value() &&
          !result.resource_descriptor.present,"old UUID version admitted as system resource identity");
  }
  bad=context; bad.transaction_uuid.bytes[15]^=1;
  refused(bad,"catalog.resource.transaction_not_active");
  bad=context; bad.local_transaction_id+=100;
  refused(bad,"catalog.resource.transaction_not_active");
  bad=context; bad.resource_epoch=0; refused(bad,"catalog.resource.epoch_required");
  bad=context; ++bad.resource_epoch; refused(bad,"catalog.resource.epoch_stale");
  const auto wrong_family=engine::LookupEngineResourceDescriptorByUuid(
      context,image.collations.front().resource_uuid,"charset");
  check(!wrong_family.ok && wrong_family.diagnostic.code=="CATALOG.INVALID_INPUT" &&
        wrong_family.diagnostic.message_key=="catalog.resource.family_mismatch" &&
        wrong_family.diagnostic.canonical_metadata.has_value() &&
        !wrong_family.resource_descriptor.present,"resource family mismatch was not refused");
  const auto invalid_resource=[&](platform::Uuid id,const char* family,const char* key) {
    const auto result=engine::LookupEngineResourceDescriptorByUuid(context,id,family);
    check(!result.ok && result.diagnostic.code=="CATALOG.INVALID_INPUT" &&
          result.diagnostic.message_key==key && result.diagnostic.canonical_metadata.has_value() &&
          !result.resource_descriptor.present && result.resource_descriptor.resource_uuid.is_nil(),
          "invalid resource identity/family published a descriptor");
  };
  invalid_resource({},"charset","catalog.resource.uuid_required");
  invalid_resource(image.charsets.front().resource_uuid,"unknown","catalog.resource.family_invalid");
  auto invalid_id=image.charsets.front().resource_uuid; invalid_id.bytes[8]&=0x3f;
  invalid_resource(invalid_id,"charset","catalog.resource.uuid_invalid");
  const auto unknown_id=uuid::GenerateEngineIdentityV7(UuidKind::object,Now());
  Require(unknown_id.ok(),"absent resource fixture identity generation failed");
  const auto absent=engine::LookupEngineResourceDescriptorByUuid(context,unknown_id.value.value,"charset");
  check(!absent.ok && absent.diagnostic.code=="CATALOG.NAME.NOT_FOUND_OR_NOT_VISIBLE" &&
        absent.diagnostic.canonical_metadata.has_value() && !absent.resource_descriptor.present,
        "absent resource used a name fallback or unregistered diagnostic");
  bad=context; bad.database_path=(path.parent_path()/"missing.sbdb").string();
  const auto missing=lookup(bad);
  auto missing_config=open; missing_config.path=bad.database_path;
  const auto native_missing=db::OpenDatabaseFile(missing_config);
  check(!missing.ok && missing.diagnostic.native_source.has_value() && missing.diagnostic.canonical_metadata.has_value() &&
        !native_missing.ok() && missing.diagnostic.native_source->record.diagnostic_code==
            native_missing.diagnostic.diagnostic_code &&
        missing.diagnostic.native_source->record.message_key==native_missing.diagnostic.message_key &&
        !missing.resource_descriptor.present,"native storage failure cause was discarded");
  const auto finalize=[&] {
    const auto committed=mga::CommitLocalTransaction(inventory,{context.local_transaction_id},Now());
    Require(committed.ok(),"resource admission actual transaction commit failed");
    const auto persisted=db::PersistLocalTransactionInventoryToDatabase(path.string(),committed.inventory);
    Require(persisted.ok(),"resource admission commit persistence failed");
    inventory=persisted.inventory;
    refused(context,"catalog.resource.transaction_not_active");
  };
  finalize();
  begin(true);
  check(lookup(context).ok,"read-only-active transaction could not admit resource");
  check(engine::LookupEngineTimezoneSeedAuthority(context).ok,"read-only-active timezone admission failed");
  finalize();
  std::cout << "PASS actual resource engine admission checks=" << checks << '\n';
}
struct SavedPage { std::uint64_t number; std::uint32_t page_size; std::vector<platform::byte> body; };
void WritePage(const fs::path& path,const SavedPage& page) {
  disk::FileDevice device;
  Require(device.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"fixture write open failed");
  Require(device.WriteAt(page.number*page.page_size+disk::kPageHeaderSerializedBytes,
                        page.body.data(),page.body.size()).ok() && device.Sync().ok(),"fixture write failed");
}
enum class Corruption { artifact, cycle, alias_identity, alias_epoch };
SavedPage Corrupt(const fs::path& path,Corruption corruption) {
  SavedPage original{}; SavedPage changed{};
  {
    disk::FileDevice device;
    Require(device.Open(path.string(),disk::FileOpenMode::open_existing_read_only).ok(),"fixture read failed");
    disk::SerializedDatabaseHeader header{};
    Require(device.ReadAt(0,header.data(),header.size()).ok(),"fixture header read failed");
    const auto parsed=disk::ParseDatabaseHeader(header); Require(parsed.ok(),"fixture header invalid");
    std::uint64_t number=db::kCatalogPageNumber;
    while (number) {
      original={number,parsed.header.page_size,{}};
      original.body.resize(original.page_size-disk::kPageHeaderSerializedBytes);
      Require(device.ReadAt(number*original.page_size+disk::kPageHeaderSerializedBytes,
                            original.body.data(),original.body.size()).ok(),"fixture page read failed");
      const auto body=page::ParseCatalogPageBody(original.body,number); Require(body.ok(),"fixture catalog invalid");
      changed=original;
      if (corruption==Corruption::cycle) { platform::StoreLittle64(changed.body.data()+32,number); break; }
      std::size_t offset=page::kCatalogPageBodyHeaderBytes;
      bool selected=false;
      for (const auto& row:body.body.rows) {
        if ((corruption==Corruption::artifact && row.kind==page::CatalogPageRowKind::resource_seed_artifact) ||
            (corruption!=Corruption::artifact && row.kind==page::CatalogPageRowKind::charset_alias_record)) {
          if (corruption==Corruption::artifact) {
            Require(row.payload.size()>48 && row.payload.substr(0,4)=="RSAC","fixture artifact has wrong layout");
            changed.body[offset+20+row.payload.size()-1]^=1;
          } else {
            Require(row.payload.size()>24 && row.payload.substr(0,4)=="SBCV","fixture alias has wrong layout");
            bool field_found=false;
            for (std::size_t pos=24;pos+8<=row.payload.size();) {
              const auto* bytes=reinterpret_cast<const platform::byte*>(row.payload.data()+pos);
              const auto id=platform::LoadLittle16(bytes);
              const auto length=platform::LoadLittle32(bytes+4);
              Require(length<=row.payload.size()-pos-8,"fixture alias field out of bounds");
              if ((corruption==Corruption::alias_identity && id==4) ||
                  (corruption==Corruption::alias_epoch && id==6)) {
                if (id==4) { Require(length==16,"alias identity is not binary16"); changed.body[offset+20+pos+8+6]=0x40; }
                else { Require(length==8,"alias epoch is not u64"); changed.body[offset+20+pos+8]^=2; }
                field_found=true;break;
              }
              pos+=8+length;
            }
            Require(field_found,"fixture alias target field missing");
          }
          std::uint64_t hash=1469598103934665603ULL;
          for (std::size_t i=0;i<row.payload.size();++i) {
            hash^=changed.body[offset+20+i]; hash*=1099511628211ULL;
          }
          platform::StoreLittle64(changed.body.data()+offset+12,hash);
          selected=true; break;
        }
        offset+=20+row.payload.size();
      }
      if (selected) break;
      number=body.body.next_page_number;
    }
    Require(number!=0,"fixture artifact page not found");
  }
  platform::StoreLittle64(changed.body.data()+40,page::ComputeCatalogPageBodyChecksum(changed.body));
  Require(page::ParseCatalogPageBody(changed.body,changed.number).ok(),"fixture outer checksums not valid");
  WritePage(path,changed); return original;
}
int main(int argc,char** argv) {
  try {
    if (argc==3) {
      auto policy=memory::DefaultLocalEngineMemoryPolicy(); policy.policy_name="resource_content_lifecycle";
      Require(memory::ConfigureDefaultMemoryManagerForFixture(policy,"resource_content_lifecycle").ok(),
              "memory fixture configuration failed");
      const fs::path root=argv[2];
      if (std::string_view(argv[1])=="--create") {
        const auto now=Now();
        const auto database=uuid::GenerateEngineIdentityV7(UuidKind::database,now);
        const auto filespace=uuid::GenerateEngineIdentityV7(UuidKind::filespace,now+1);
        Require(database.ok() && filespace.ok(),"fixture UUID generation failed");
        db::DatabaseCreateConfig config;
        config.path=(root/"content.sbdb").string(); config.database_uuid=database.value;
        config.filespace_uuid=filespace.value; config.creation_unix_epoch_millis=now;
        config.resource_seed_pack_root=(root/"initial-resource-pack").string();
        config.require_resource_seed_pack=true;
        const auto created=db::CreateDatabaseFile(config); Good(created);
        Require(!created.state.resource_seed_catalog.artifacts.empty(),"create did not retain artifacts");
        const auto ids=IdentityBytes(created.state.resource_seed_catalog);
        std::ofstream identity_file(root/"identity-oracle.bin",std::ios::binary);
        identity_file.write(ids.data(),ids.size());identity_file.close();
        Require(identity_file.good(),"independent identity oracle write failed");
        return 0;
      }
      const std::string_view mode=argv[1];
      Require(mode=="--reopen" || mode=="--admission" || mode=="--refuse-artifact" || mode=="--refuse-chain","unknown fixture mode");
      Require(!fs::exists(root/"initial-resource-pack"),"seed directory still present during reopen");
      if(mode=="--admission") { ResourceAdmission(root/"content.sbdb"); return 0; }
      db::DatabaseOpenConfig config; config.path=(root/"content.sbdb").string();
      config.read_only=true; config.suppress_background_agents=true;
      const auto opened=db::OpenDatabaseFile(config);
      if (mode!="--reopen") {
        Require(!opened.ok() && opened.diagnostic.diagnostic_code ==
            (mode=="--refuse-artifact" ? "SB-CATALOG-RECORD-CODEC-FIELDS-MISSING" :
                                       "SB-CATALOG-PAGE-BODY-NEXT-CHAIN-TOO-LONG"),
                "corrupt resource database was not refused at expected boundary");
        std::cout << "PASS independent corruption refusal " << mode << '\n'; return 0;
      }
      Good(opened);
      r::ResourceSeedLoadConfig source; source.seed_pack_root=SB_BOOTSTRAP_SEED_PACK_ROOT;
      const auto oracle=r::LoadResourceSeedPack(source); Require(oracle.ok(),"independent oracle pack failed");
      const auto& actual=opened.state.resource_seed_catalog;
      std::ifstream identity_file(root/"identity-oracle.bin",std::ios::binary);
      Require(identity_file.good(),"independent identity oracle read failed");
      const std::string ids(std::istreambuf_iterator<char>(identity_file),{});
      Require(IdentityBytes(actual)==ids,"binary resource or alias identities changed across process restart");
      for (const auto& row:actual.aliases) {
        Require(!row.canonical_resource_uuid.is_nil() && (row.canonical_resource_uuid.bytes[6]>>4)==7,
                "bound alias lost its binary UUIDv7 target");
      }
      const auto ambiguous=r::ResolveResourceSeedAlias(actual,r::ResourceSeedFamily::charset,"gb2312");
      Require(!ambiguous.ok() && ambiguous.diagnostic.diagnostic_code=="SB_RESOURCE_ALIAS_AMBIGUOUS" &&
              ambiguous.alias.canonical_resource_uuid.is_nil(),"persisted alias ambiguity changed");
      Require(actual.artifacts.size()==oracle.image.artifacts.size(),"reopen artifact count changed");
      std::size_t bytes=0;
      for (std::size_t i=0;i<actual.artifacts.size();++i) {
        const auto& a=actual.artifacts[i]; const auto& e=oracle.image.artifacts[i];
        Require(!a.artifact_uuid.is_nil() && (a.artifact_uuid.bytes[6]>>4)==7,
                "persisted artifact identity is not binary system UUIDv7");
        Require(a.family==e.family && a.canonical_path==e.canonical_path && a.content && e.content &&
                *a.content==*e.content && a.content_hash==e.content_hash &&
                a.content_size_bytes==e.content_size_bytes,"reopen lost or changed resource content");
        bytes+=a.content->size();
      }
      std::cout << "PASS independent resource reopen artifacts=" << actual.artifacts.size()
                << " bytes=" << bytes << " timezone_identities=" << actual.timezones.size() << '\n';
      return 0;
    }
    Require(argc==1,"unexpected arguments");
    const auto id=uuid::GenerateEngineIdentityV7(UuidKind::object,Now()); Require(id.ok(),"temp identity failed");
    const auto root=fs::temp_directory_path()/("sb-resource-content-"+uuid::UuidToString(id.value.value));
    Require(fs::create_directory(root),"fixture directory already exists");
    struct Cleanup { fs::path root; ~Cleanup() {
      std::error_code error; const auto removed=fs::remove_all(root,error);
      std::cout << "resource_content_fixture_cleanup entries=" << removed << " error=" << error.message() << '\n';
    }} cleanup{root};
    const fs::path source=SB_BOOTSTRAP_SEED_PACK_ROOT, copy=root/"initial-resource-pack";
    fs::create_directory(copy);
    r::ResourceSeedLoadConfig load; load.seed_pack_root=source.string();
    const auto loaded=r::LoadResourceSeedPack(load); Require(loaded.ok(),"fixture resource load failed");
    // Copy only admitted data artifacts, never donor source trees.
    for (const auto& artifact:loaded.image.artifacts) {
      const fs::path relative=artifact.canonical_path;
      Require(!relative.is_absolute() && relative.string().find("..") == std::string::npos,
              "fixture artifact path escapes pack");
      fs::create_directories((copy/relative).parent_path());
      fs::copy_file(source/relative,copy/relative,fs::copy_options::skip_existing);
    }
    for (const auto* name:{"RESOURCE_SEED_MANIFEST.csv","RESOURCE_SEED_ARTIFACTS.csv"})
      fs::copy_file(source/name,copy/name);
    const auto program=Quote(fs::canonical(argv[0]).string());
    Require(std::system((program+" --create "+Quote(root.string())).c_str())==0,"create subprocess failed");
    fs::remove_all(copy);
    Require(std::system((program+" --reopen "+Quote(root.string())).c_str())==0,"independent reopen subprocess failed");
    Require(std::system((program+" --admission "+Quote(root.string())).c_str())==0,"actual resource admission subprocess failed");
    for (const auto corruption:{Corruption::artifact,Corruption::cycle,Corruption::alias_identity,Corruption::alias_epoch}) {
      const auto original=Corrupt(root/"content.sbdb",corruption);
      const auto command=program+(corruption==Corruption::cycle ? " --refuse-chain " : " --refuse-artifact ")+Quote(root.string());
      const auto refused=std::system(command.c_str());
      WritePage(root/"content.sbdb",original);
      Require(refused==0,"independent corruption refusal failed");
      std::cout << "verified_corruption_case=" << static_cast<unsigned>(corruption) << '\n';
    }
    Require(std::system((program+" --reopen "+Quote(root.string())).c_str())==0,"restored database did not reopen");
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}

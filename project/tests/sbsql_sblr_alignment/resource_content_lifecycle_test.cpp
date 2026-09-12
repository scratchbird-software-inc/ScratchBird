// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Real database file create/reopen in separate processes. Not SQL/IPC evidence.
#include "database_lifecycle.hpp"
#include "resource_seed_pack.hpp"
#include "memory.hpp"
#include "uuid.hpp"
#include "catalog_page.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
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
struct SavedPage { std::uint64_t number; std::uint32_t page_size; std::vector<platform::byte> body; };
void WritePage(const fs::path& path,const SavedPage& page) {
  disk::FileDevice device;
  Require(device.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"fixture write open failed");
  Require(device.WriteAt(page.number*page.page_size+disk::kPageHeaderSerializedBytes,
                        page.body.data(),page.body.size()).ok() && device.Sync().ok(),"fixture write failed");
}
SavedPage Corrupt(const fs::path& path,bool cycle) {
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
      if (cycle) { platform::StoreLittle64(changed.body.data()+32,number); break; }
      std::size_t offset=page::kCatalogPageBodyHeaderBytes;
      bool selected=false;
      for (const auto& row:body.body.rows) {
        if (row.kind==page::CatalogPageRowKind::resource_seed_artifact) {
          Require(row.payload.size()>48 && row.payload.substr(0,4)=="RSAC","fixture artifact has wrong layout");
          changed.body[offset+20+row.payload.size()-1]^=1;
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
        return 0;
      }
      const std::string_view mode=argv[1];
      Require(mode=="--reopen" || mode=="--refuse-artifact" || mode=="--refuse-chain","unknown fixture mode");
      Require(!fs::exists(root/"initial-resource-pack"),"seed directory still present during reopen");
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
                << " bytes=" << bytes << '\n';
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
    for (const bool cycle:{false,true}) {
      const auto original=Corrupt(root/"content.sbdb",cycle);
      const auto command=program+(cycle ? " --refuse-chain " : " --refuse-artifact ")+Quote(root.string());
      const auto refused=std::system(command.c_str());
      WritePage(root/"content.sbdb",original);
      Require(refused==0,"independent corruption refusal failed");
    }
    Require(std::system((program+" --reopen "+Quote(root.string())).c_str())==0,"restored database did not reopen");
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}

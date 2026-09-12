// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog/schema_tree_api.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/catalog_lookup_api.hpp"
#include "observability/show_api.hpp"
#include "artifacts/artifact_api.hpp"
#include "ddl/create_api.hpp"
#include "database_lifecycle.hpp"
#include "catalog_page.hpp"
#include "catalog_record_codec.hpp"
#include "page_header.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#if defined(__linux__)
#include <sys/stat.h>
#endif

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
namespace p = scratchbird::core::platform;
namespace fs = std::filesystem;
unsigned checks=0, failures=0;
void Check(bool ok,const char* message) {
  ++checks; if(!ok) {++failures;std::cerr<<"FAIL "<<message<<'\n';}
}
void Setup(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
std::string Read(const fs::path& path) {
  std::ifstream in(path,std::ios::binary);Setup(in.is_open(),"read fixture");
  std::string s{std::istreambuf_iterator<char>(in),{}};Setup(!in.bad(),"read fixture bytes");return s;
}
void Write(const fs::path& path,const std::string& bytes) {
  std::ofstream out(path,std::ios::binary|std::ios::trunc);
  out.write(bytes.data(),bytes.size());out.close();Setup(!out.fail(),"write owned fixture");
}
p::u64 Get(const std::string& s,std::size_t off,unsigned n) {
  p::u64 v=0;for(unsigned i=0;i<n;++i)v|=p::u64(static_cast<unsigned char>(s.at(off+i)))<<(8*i);return v;
}
void Put(std::string& s,std::size_t off,p::u64 v,unsigned n) {
  for(unsigned i=0;i<n;++i)s.at(off+i)=static_cast<char>(v>>(8*i));
}
void BodyChecksum(std::string& bytes,p::u64 number) {
  const auto off=number*16384+disk::kPageHeaderSerializedBytes;
  std::vector<p::byte> body(bytes.begin()+off,bytes.begin()+(number+1)*16384);
  Put(bytes,off+40,page::ComputeCatalogPageBodyChecksum(body),8);
  body.assign(bytes.begin()+off,bytes.begin()+(number+1)*16384);
  Setup(page::ParseCatalogPageBody(body,number).ok(),"fault must retain valid outer catalog framing");
}
int main() {
  fs::path root;
  try {
    const auto millis=static_cast<p::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    const auto id=uuid::GenerateEngineIdentityV7(p::UuidKind::database,millis);
    const auto space=uuid::GenerateEngineIdentityV7(p::UuidKind::filespace,millis);
    Setup(id.ok()&&space.ok(),"UUID generation");
    root=fs::temp_directory_path()/("scratchbird_schema_read_"+uuid::UuidToString(id.value.value));
    Setup(fs::create_directory(root),"unique fixture directory");
    db::DatabaseCreateConfig cfg;cfg.path=(root/"primary.sdb").string();
    cfg.database_uuid=id.value;cfg.filespace_uuid=space.value;cfg.page_size=16384;
    cfg.creation_unix_epoch_millis=millis;cfg.allow_minimal_resource_bootstrap=true;cfg.require_resource_seed_pack=false;
    Setup(db::CreateDatabaseFile(cfg).ok(),"real database initialization");
    api::EngineRequestContext context;context.database_path=cfg.path;
    context.database_uuid.canonical=uuid::UuidToString(id.value.value);
    const auto original=Read(cfg.path);
    const auto candidate_id=uuid::GenerateEngineIdentityV7(p::UuidKind::object,millis);
    Setup(candidate_id.ok(),"candidate UUID generation");
    const auto candidate=uuid::UuidToString(candidate_id.value.value);
    api::EngineApiDiagnostic diagnostic;
    auto schemas=api::VisibleSchemaTreeRecords(context,0,diagnostic);
    Setup(!diagnostic.error&&schemas.size()==23,"real bootstrap schema tree");
    std::string target,parent;
    for(const auto& s:schemas) for(const auto& n:s.localized_names) {
      if(n.path=="users.public")target=s.schema_uuid;
      if(n.path=="users")parent=s.schema_uuid;
    }
    Setup(!target.empty()&&!parent.empty(),"independent known bootstrap paths");
    api::EngineLocalizedName name;name.language_tag="en";name.path="users.public";name.name="public";
    const auto healthy=[&] {
      const auto tree=api::VisibleSchemaTreeRecords(context,0,diagnostic);
      Check(!diagnostic.error&&tree.size()==23,"healthy complete tree after restoration");
      const auto found=api::FindVisibleSchemaTreeRecord(context,target,0,diagnostic);
      Check(!diagnostic.error&&found&&found->parent_schema_uuid==parent,"real UUID lookup");
      const auto names=api::LoadNameRegistryState(context,0);
      Check(names.ok&&!names.state.entries.empty(),"warm actual name cache");
      Check(api::SchemaTreePathConflict(context,candidate,parent,{name},0,diagnostic).has_value()
          &&!diagnostic.error,"real existing name conflict");
      Check(api::SchemaTreeWouldCreateCycle(context,parent,target,0,diagnostic)
          &&!diagnostic.error,"real proposed parent cycle");
    };
    const auto refused=[&] {
      // Cache populated while healthy must not conceal a corrupt primary.
      const auto names=api::LoadNameRegistryState(context,0);
      Check(!names.ok&&names.diagnostic.error&&names.state.entries.empty(),"no cached names after failed authority read");
      Check(api::VisibleSchemaTreeRecords(context,0,diagnostic).empty()&&diagnostic.error,"no successful partial tree");
      Check(!api::FindVisibleSchemaTreeRecord(context,target,0,diagnostic)&&diagnostic.error,"lookup error not absence");
      Check(!api::SchemaTreePathConflict(context,candidate,parent,{name},0,diagnostic)&&diagnostic.error,"read error not no-conflict");
      Check(!api::SchemaTreeWouldCreateCycle(context,parent,target,0,diagnostic)&&diagnostic.error,"read error not no-cycle");
      const auto result_error=[](const auto& r) {
        bool error=false;for(const auto& d:r.diagnostics)error|=d.error;
        return !r.ok&&error&&r.result_shape.rows.empty();
      };
      api::EngineListCatalogChildrenRequest list;list.context=context;
      Check(result_error(api::EngineListCatalogChildren(list)),"list discards partial output");
      api::EngineLookupObjectRequest lookup;lookup.context=context;lookup.target_object.uuid.canonical=target;
      Check(result_error(api::EngineLookupObject(lookup)),"object lookup propagates diagnostic");
      api::EngineShowCatalogRequest show;show.context=context;show.option_envelopes={"catalog_projection:sys.catalog"};
      Check(result_error(api::EngineShowCatalog(show)),"readable projection propagates diagnostic");
      api::EngineExportCatalogArtifactsRequest export_request;export_request.context=context;
      Check(result_error(api::EngineExportCatalogArtifacts(export_request)),"artifact export no partial success");
      api::EngineCreateSchemaRequest create;create.context=context;create.context.local_transaction_id=1;
      create.target_schema.uuid.canonical=parent;create.target_object.uuid.canonical=candidate;create.localized_names={name};
      const auto created=api::EngineCreateSchema(create);
      Check(result_error(created),"DDL cannot succeed after failed schema authority read");
      bool same_failure=false;
      for(const auto& d:created.diagnostics) same_failure|=d.error&&d.detail==diagnostic.detail;
      Check(same_failure,"DDL must propagate the actual schema read failure");
    };
    healthy();healthy();
    const auto mutate=[&](std::string bytes) {
      Write(cfg.path,bytes);refused();Check(Read(cfg.path)==bytes,"failed reads must not change database");
      Write(cfg.path,original);healthy();
    };
    auto bytes=original;bytes[0]^=1;mutate(bytes);
    mutate(original.substr(0,32));
    mutate(original.substr(0,2*16384+disk::kPageHeaderSerializedBytes+50));
    std::vector<p::u64> pages;
    for(p::u64 n=2;n!=0;n=Get(original,n*16384+disk::kPageHeaderSerializedBytes+32,8)) {
      Setup(pages.size()<1024,"healthy chain bound");pages.push_back(n);
    }
    Setup(pages.size()>1,"multi-page fixture for partial-read regression");
    const auto last=pages.back();
    bytes=original;bytes[last*16384+disk::kPageHeaderSerializedBytes+60]^=1;mutate(bytes);
    mutate(original.substr(0,last*16384+disk::kPageHeaderSerializedBytes+50));
    bytes=original;Put(bytes,last*16384+disk::kPageHeaderSerializedBytes+32,2,8);BodyChecksum(bytes,last);mutate(bytes);
    disk::SerializedPageHeader header_bytes{};
    std::copy_n(original.begin()+last*16384,header_bytes.size(),header_bytes.begin());
    const auto parsed_header=disk::ParsePageHeader(header_bytes);Setup(parsed_header.ok(),"real page header");
    for(unsigned fault=0;fault<4;++fault) {
      auto header=parsed_header.header;
      if(fault==0)header.page_type=disk::PageType::row_data;
      if(fault==1)++header.page_number;
      if(fault==2)header.page_size=32768;
      if(fault==3)header.database_uuid=uuid::GenerateEngineIdentityV7(p::UuidKind::database,millis).value.value;
      const auto encoded=disk::SerializePageHeader(header);Setup(encoded.ok(),"checksum-valid header mutation");
      bytes=original;std::copy(encoded.serialized.begin(),encoded.serialized.end(),bytes.begin()+last*16384);
      mutate(bytes);
    }
    // A valid outer row/page checksum must not let malformed schema payload be skipped.
    bool changed=false;
    for(const auto n:pages) {
      const auto off=n*16384+disk::kPageHeaderSerializedBytes;
      std::vector<p::byte> body(original.begin()+off,original.begin()+(n+1)*16384);
      const auto parsed=page::ParseCatalogPageBody(body,n);Setup(parsed.ok(),"parse healthy page");
      std::size_t rowoff=page::kCatalogPageBodyHeaderBytes;
      for(const auto& row:parsed.body.rows) {
        const auto decoded=scratchbird::core::catalog::DecodeCatalogTypedRecord(row);
        if(decoded.ok() && decoded.record.header.kind==scratchbird::core::catalog::CatalogRecordKind::schema && !changed) {
          bytes=original;Put(bytes,off+rowoff+20+96+16,65535,4);
          p::u64 hash=1469598103934665603ull;
          for(std::size_t i=0;i<row.payload.size();++i) {hash^=static_cast<unsigned char>(bytes[off+rowoff+20+i]);hash*=1099511628211ull;}
          Put(bytes,off+rowoff+12,hash,8);BodyChecksum(bytes,n);mutate(bytes);changed=true;
        }
        rowoff+=20+row.payload.size();
      }
    }
    Setup(changed,"actual schema payload fault located");
    const auto saved=root/"saved.primary";
    fs::rename(cfg.path,saved);refused();
    Setup(fs::create_directory(cfg.path),"directory fault");refused();fs::remove(cfg.path);
#if defined(__linux__)
    fs::create_symlink(root/"absent",cfg.path);refused();fs::remove(cfg.path);
    fs::create_symlink(cfg.path,cfg.path);refused();fs::remove(cfg.path);
    Setup(::mkfifo(cfg.path.c_str(),0600)==0,"primary FIFO");refused();fs::remove(cfg.path);
#endif
    fs::rename(saved,cfg.path);healthy();
    auto foreign=context.database_uuid.canonical;
    context.database_uuid.canonical=uuid::UuidToString(uuid::GenerateEngineIdentityV7(p::UuidKind::database,millis).value.value);
    refused();context.database_uuid.canonical=foreign;healthy();
    const fs::path journal=cfg.path+".sb.api_events";
    Setup(!fs::exists(journal),"refused DDL must not create journal");
    Write(journal,"");healthy();fs::remove(journal);
    Setup(fs::create_directory(journal),"journal directory");refused();fs::remove(journal);
#if defined(__linux__)
    fs::create_symlink(root/"absent",journal);refused();fs::remove(journal);
    fs::create_symlink(journal,journal);refused();fs::remove(journal);
    Setup(::mkfifo(journal.c_str(),0600)==0,"journal FIFO");refused();fs::remove(journal);
    fs::create_symlink("/proc/self/mem",journal);refused();fs::remove(journal);
    Write(journal,"");const auto permissions=fs::status(journal).permissions();
    fs::permissions(journal,fs::perms::none);
    Setup(!std::ifstream(journal).is_open(),"requires unprivileged Linux permission fault");
    refused();fs::permissions(journal,permissions);fs::remove(journal);
    const auto primary_permissions=fs::status(cfg.path).permissions();
    fs::permissions(cfg.path,fs::perms::none);
    Setup(!std::ifstream(cfg.path).is_open(),"primary permission fault");
    refused();fs::permissions(cfg.path,primary_permissions);
#endif
    healthy();Check(Read(cfg.path)==original,"complete final database unchanged");
    if(failures) {std::cerr<<failures<<" failures; retained "<<root<<'\n';return 1;}
    fs::remove_all(root); // Only the unique directory created by this test.
    std::cout<<"PASS schema authority "<<checks<<" checks\n";return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<"; retained "<<root<<'\n';return 1;}
}

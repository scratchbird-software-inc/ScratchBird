// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_schema_record_codec.hpp"
#include "catalog_record_codec.hpp"
#include "catalog_page.hpp"
#include "catalog/schema_tree_api.hpp"
#include "database_lifecycle.hpp"
#include "page_header.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>

namespace c=scratchbird::core::catalog;
namespace p=scratchbird::core::platform;
namespace db=scratchbird::storage::database;
namespace disk=scratchbird::storage::disk;
namespace page=scratchbird::storage::page;
namespace api=scratchbird::engine::internal_api;
namespace uuid=scratchbird::core::uuid;
namespace fs=std::filesystem;
unsigned checks=0,failures=0,cases=0;
void Check(bool v,const char* m) {++checks;if(!v){++failures;std::cerr<<"FAIL "<<m<<'\n';}}
void Setup(bool v,const char* m) {if(!v)throw std::runtime_error(m);}
void Put(std::string& s,std::size_t off,p::u64 v,unsigned n) {for(unsigned i=0;i<n;++i)s.at(off+i)=static_cast<char>(v>>(8*i));}
p::u64 Get(const std::string& s,std::size_t off,unsigned n) {p::u64 v=0;for(unsigned i=0;i<n;++i)v|=p::u64(static_cast<unsigned char>(s.at(off+i)))<<(8*i);return v;}
std::string Number(p::u64 n) {std::string s(8,'\0');Put(s,0,n,8);return s;}
void Field(std::string& s,unsigned id,unsigned tag,const std::string& value) {
  const auto off=s.size();s.resize(off+8,'\0');Put(s,off,id,2);Put(s,off+2,tag,1);Put(s,off+4,value.size(),4);s+=value;
}
std::string Identity(const p::TypedUuid& id) {return {reinterpret_cast<const char*>(id.value.bytes.data()),16};}
// Independent exact Core TLV oracle; never calls production schema or encoder.
std::string Golden(const c::CatalogSchemaRecord& r) {
  std::string s(24,'\0');s.replace(0,4,"SBCV");Put(s,4,1,2);Put(s,6,24,2);
  Put(s,12,8,4);Put(s,16,65539,4);Put(s,20,1,2);
  Field(s,1,5,Identity(r.schema_object_uuid));Field(s,2,5,Identity(r.parent_object_uuid));
  Field(s,3,3,r.path_cache);Field(s,4,3,r.name_cache);
  Field(s,5,2,std::string(1,char(r.root_schema)));Field(s,6,2,std::string(1,char(r.local_single_node_scope)));
  Field(s,7,2,std::string(1,char(r.recursive_schema_tree)));Field(s,8,1,Number(r.creator_transaction_number));
  Put(s,8,s.size(),4);return s;
}
void Refused(const std::string& bytes) {auto d=c::DecodeCatalogSchemaRecord(bytes);Check(!d.ok()&&!d.record,"invalid payload exposed record");}
void Roundtrip(const c::CatalogSchemaRecord& r) {
  const auto e=c::EncodeCatalogSchemaRecord(r);Check(e.ok(),"encode");
  if(!e.ok())return;Check(std::string(e.bytes.begin(),e.bytes.end())==Golden(r),"independent complete byte oracle");
  const auto d=c::DecodeCatalogSchemaRecord(Golden(r));Check(d.ok(),"decode golden");
  if(d.ok())Check(Golden(*d.record)==Golden(r),"all fields lossless");
}
void CodecChecks(c::CatalogSchemaRecord r) {
  r.path_cache=std::string("p\0;=\r\n",6);r.name_cache="na\xC3\xAFve";Roundtrip(r);
  auto valid=Golden(r);
  for(std::size_t n=0;n<valid.size();++n)Refused(valid.substr(0,n));
  Refused(valid+"x");Refused(std::string(130977,'x'));Refused("creator_tx=1\nschema_object_uuid=text");
  for(unsigned off:{0,4,6,8,12,16,20,22}){auto s=valid;Put(s,off,255,1);Refused(s);}
  std::vector<std::size_t> offsets;
  for(std::size_t off=24;off<valid.size();off+=8+Get(valid,off+4,4))offsets.push_back(off);
  for(unsigned i=0;i<offsets.size();++i) {
    const auto off=offsets[i];
    for(unsigned tag=0;tag<256;++tag)if(tag!=Get(valid,off+2,1)){auto s=valid;Put(s,off+2,tag,1);Refused(s);}
    for(unsigned id:{0u,65535u,i}){auto s=valid;Put(s,off,id,2);Refused(s);}
    auto s=valid;Put(s,off+3,1,1);Refused(s);
    s=valid;Put(s,off+4,0xffffffffu,4);Refused(s);
  }
  for(unsigned off:{unsigned(offsets[2]+8),unsigned(offsets[3]+8)}){auto s=valid;s[off]=char(255);Refused(s);}
  for(unsigned i=4;i<7;++i)for(unsigned v=2;v<256;++v){auto s=valid;Put(s,offsets[i]+8,v,1);Refused(s);}
  for(unsigned i=5;i<7;++i){auto s=valid;Put(s,offsets[i]+8,0,1);Refused(s);}
  for(p::u64 v:{p::u64{0},p::u64{2},~p::u64{0}}){auto s=valid;Put(s,offsets[7]+8,v,8);Refused(s);}
  for(unsigned slot=0;slot<2;++slot) {
    for(unsigned kind=0;kind<256;++kind)if(kind!=unsigned(p::UuidKind::object)) {
      auto a=r;(slot?a.parent_object_uuid:a.schema_object_uuid).kind=static_cast<p::UuidKind>(kind);
      const auto e=c::EncodeCatalogSchemaRecord(a);Check(!e.ok()&&e.bytes.empty(),"wrong reference kind encoded");
    }
    for(unsigned version=0;version<16;++version)if(version!=7) {
      auto s=valid;const auto pos=offsets[slot]+8+6;
      s[pos]=char((static_cast<unsigned char>(s[pos])&15)|(version<<4));Refused(s);
    }
    for(unsigned variant:{0u,64u,192u}){auto s=valid;Put(s,offsets[slot]+8+8,variant,1);Refused(s);}
    auto s=valid;s.replace(offsets[slot]+8,16,std::string(16,'\0'));Refused(s);
  }
  {auto a=r;a.parent_object_uuid=a.schema_object_uuid;Check(!c::EncodeCatalogSchemaRecord(a).ok(),"self parent encoded");Refused(Golden(a));}
  for(unsigned slot=0;slot<2;++slot) {
    auto a=r;a.path_cache="p";a.name_cache="n";
    auto& t=slot?a.name_cache:a.path_cache;t.clear();Check(!c::EncodeCatalogSchemaRecord(a).ok(),"empty cache encoded");Refused(Golden(a));
    t.assign(130844,'x');Roundtrip(a);t+='x';const auto e=c::EncodeCatalogSchemaRecord(a);Check(!e.ok()&&e.bytes.empty(),"oversize encoded");Refused(Golden(a));
  }
  {auto a=r;a.path_cache.assign(70000,'p');a.name_cache.assign(70000,'n');Check(!c::EncodeCatalogSchemaRecord(a).ok(),"combined bound");}
}
std::string Read(const fs::path& path) {std::ifstream in(path,std::ios::binary);Setup(in.is_open(),"fixture read");return {std::istreambuf_iterator<char>(in),{}};}
void Write(const fs::path& path,const std::string& s) {std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(s.data(),s.size());out.close();Setup(!out.fail(),"owned fixture write");}
p::u64 Fnv(std::string_view s) {p::u64 h=1469598103934665603ull;for(unsigned char b:s){h^=b;h*=1099511628211ull;}return h;}
struct Row {p::u64 page;std::size_t offset;c::CatalogTypedRecord record;c::CatalogSchemaRecord schema;};
void Replace(std::string& bytes,const Row& row,const std::string& payload,const p::TypedUuid* parent=nullptr,bool deleted=false) {
  const auto off=row.page*16384+disk::kPageHeaderSerializedBytes;
  const auto rec=off+row.offset+20;
  Setup(payload.size()==row.record.payload.size(),"same-sized fault");
  bytes.replace(rec+96,payload.size(),payload);
  if(parent)bytes.replace(rec+72,16,Identity(*parent));
  if(deleted)Put(bytes,rec+24,1,4);
  Put(bytes,off+row.offset+12,Fnv(std::string_view(bytes).substr(rec,96+payload.size())),8);
  std::vector<p::byte> body(bytes.begin()+off,bytes.begin()+(row.page+1)*16384);
  Put(bytes,off+40,page::ComputeCatalogPageBodyChecksum(body),8);
  body.assign(bytes.begin()+off,bytes.begin()+(row.page+1)*16384);
  Setup(page::ParseCatalogPageBody(body,row.page).ok(),"valid outer row/page integrity");
}
int main() {
  fs::path root;
  try {
    const auto now=static_cast<p::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    auto dbid=uuid::GenerateEngineIdentityV7(p::UuidKind::database,now);
    auto fsid=uuid::GenerateEngineIdentityV7(p::UuidKind::filespace,now);
    auto obj=uuid::GenerateEngineIdentityV7(p::UuidKind::object,now);
    auto parent=uuid::GenerateEngineIdentityV7(p::UuidKind::object,now);
    Setup(dbid.ok()&&fsid.ok()&&obj.ok()&&parent.ok(),"UUID generation");
    CodecChecks({obj.value,parent.value,"x","x"});
    root=fs::temp_directory_path()/("scratchbird_schema_binary_"+uuid::UuidToString(dbid.value.value));
    Setup(fs::create_directory(root),"unique fixture");
    db::DatabaseCreateConfig cfg;cfg.path=(root/"primary.sdb").string();cfg.database_uuid=dbid.value;cfg.filespace_uuid=fsid.value;
    cfg.page_size=16384;cfg.creation_unix_epoch_millis=now;cfg.allow_minimal_resource_bootstrap=true;cfg.require_resource_seed_pack=false;
    Setup(db::CreateDatabaseFile(cfg).ok(),"real database create");
    api::EngineRequestContext context;context.database_path=cfg.path;context.database_uuid.canonical=uuid::UuidToString(dbid.value.value);
    const auto original=Read(cfg.path);std::vector<Row> rows;std::vector<c::CatalogTypedRecord> records;
    for(p::u64 n=2,guard=0;n!=0;) {
      Setup(++guard<1024,"healthy traversal bound");
      const auto off=n*16384+disk::kPageHeaderSerializedBytes;
      std::vector<p::byte> body(original.begin()+off,original.begin()+(n+1)*16384);
      const auto parsed=page::ParseCatalogPageBody(body,n);Setup(parsed.ok(),"real page framing");
      std::size_t rowoff=page::kCatalogPageBodyHeaderBytes;
      for(const auto& row:parsed.body.rows) {
        if(row.kind==page::CatalogPageRowKind::typed_catalog_record) {
          const auto d=c::DecodeCatalogTypedRecord(row);Setup(d.ok(),"real common header");
          records.push_back(d.record);
          if(d.record.header.kind==c::CatalogRecordKind::schema) {
            const auto s=c::DecodeCatalogSchemaRecord(d.record.payload);Setup(s.ok(),"real binary schema payload");
            rows.push_back({n,rowoff,d.record,*s.record});
          }
        }
        rowoff+=20+row.payload.size();
      }
      n=parsed.body.next_page_number;
    }
    Check(rows.size()==23,"current bootstrap seed inventory");
    Check(c::ValidateCatalogSchemaGraph(records),"actual binary parent graph");
    for(const auto& row:rows) {
      const auto& s=row.schema;const auto dot=s.path_cache.rfind('.');
      Check(s.name_cache==(dot==std::string::npos?s.path_cache:s.path_cache.substr(dot+1)),"independent leaf name");
      Check(s.root_schema==(dot==std::string::npos),"independent root relation");
      c::CatalogSchemaRecord expected{row.record.header.object_uuid,row.record.header.parent_uuid,s.path_cache,s.name_cache,s.root_schema,true,true,1};
      Check(row.record.payload==Golden(expected),"every persisted field and binary header binding");
    }
    const auto healthy=[&] {
      db::DatabaseOpenConfig open;open.path=cfg.path;open.read_only=true;open.suppress_background_agents=true;
      Check(db::OpenDatabaseFile(open).ok(),"healthy real database admission");
      api::EngineApiDiagnostic diagnostic;
      Check(api::VisibleSchemaTreeRecords(context,0,diagnostic).size()==23&&!diagnostic.error,"healthy schema reader");
    };healthy();
    const auto fault=[&](const std::string& bytes) {
      ++cases;Write(cfg.path,bytes);
      for(bool ro:{true,false}) {
        db::DatabaseOpenConfig open;open.path=cfg.path;open.read_only=ro;open.suppress_background_agents=true;
        Check(!db::OpenDatabaseFile(open).ok(),"corrupt database admitted");
        Check(Read(cfg.path)==bytes,"failed admission changed bytes");
      }
      api::EngineApiDiagnostic diagnostic;
      Check(api::VisibleSchemaTreeRecords(context,0,diagnostic).empty()&&diagnostic.error,"invalid graph exposed schemas");
      Check(Read(cfg.path)==bytes,"failed schema read changed bytes");
      Write(cfg.path,original);healthy();
    };
    const auto& row=rows.front();
    for(unsigned offset:{0u,4u,16u,24u,26u,27u,28u,32u,56u}) {
      auto s=row.record.payload;s[offset]^=char(255);auto bytes=original;Replace(bytes,row,s);fault(bytes);
    }
    // Valid UUID syntax but mismatched header identities.
    for(unsigned slot:{0u,1u}) {
      auto s=row.schema;(slot?s.parent_object_uuid:s.schema_object_uuid)=obj.value;
      auto bytes=original;Replace(bytes,row,Golden(s));fault(bytes);
    }
    // Fully matching binary parent references, but absent or wrong-class parent.
    {auto s=row.schema;s.parent_object_uuid=obj.value;auto bytes=original;Replace(bytes,row,Golden(s),&s.parent_object_uuid);fault(bytes);}
    {auto s=row.schema;s.root_schema=false;auto bytes=original;Replace(bytes,row,Golden(s));fault(bytes);}
    {auto bytes=original;Replace(bytes,row,row.record.payload,nullptr,true);fault(bytes);}
    const auto first=std::find_if(rows.begin(),rows.end(),[](const Row& r){return !r.schema.root_schema;});
    Setup(first!=rows.end(),"first nonroot schema");
    const auto second=std::find_if(first+1,rows.end(),[](const Row& r){return !r.schema.root_schema;});
    Setup(first!=rows.end()&&second!=rows.end(),"two nonroot schemas");
    {auto a=first->schema,b=second->schema;a.parent_object_uuid=b.schema_object_uuid;b.parent_object_uuid=a.schema_object_uuid;
      auto bytes=original;Replace(bytes,*first,Golden(a),&a.parent_object_uuid);Replace(bytes,*second,Golden(b),&b.parent_object_uuid);fault(bytes);}
    {auto s=first->schema;s.root_schema=true;auto bytes=original;Replace(bytes,*first,Golden(s));fault(bytes);}
    {auto graph=records;graph.erase(std::remove_if(graph.begin(),graph.end(),[](const auto& r){return r.header.kind==c::CatalogRecordKind::schema;}),graph.end());Check(!c::ValidateCatalogSchemaGraph(graph),"empty graph accepted");}
    {auto graph=records;graph.push_back(first->record);Check(!c::ValidateCatalogSchemaGraph(graph),"duplicate schema accepted");}
    Check(Read(cfg.path)==original,"final fixture unchanged");
    if(failures){std::cerr<<failures<<" failures; retained "<<root<<'\n';return 1;}
    fs::remove_all(root);
    std::cout<<"PASS schema binary "<<checks<<" checks; "<<cases<<" real-file fault cases\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<"; retained "<<root<<'\n';return 1;}
}

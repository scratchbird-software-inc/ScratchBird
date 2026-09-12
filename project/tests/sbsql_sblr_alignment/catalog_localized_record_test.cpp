// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_localized_record_codec.hpp"
#include "catalog_record_codec.hpp"
#include "catalog_page.hpp"
#include "database_lifecycle.hpp"
#include "startup_state.hpp"
#include "page_header.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>

namespace c = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
namespace fs = std::filesystem;
unsigned checks=0, failures=0;
void Check(bool ok,const char* message) {
  ++checks; if(!ok && ++failures<20) std::cerr<<"FAIL "<<message<<'\n';
}
void Setup(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
void Put(std::string& s,std::size_t off,p::u64 value,unsigned count) {
  for(unsigned i=0;i<count;++i)s[off+i]=static_cast<char>((value>>(8*i))&255);
}
p::u64 Get(const std::string& s,std::size_t off,unsigned count) {
  p::u64 value=0;for(unsigned i=0;i<count;++i)value|=p::u64(static_cast<unsigned char>(s[off+i]))<<(8*i);
  return value;
}
std::string Number(p::u64 v) {std::string s(8,'\0');Put(s,0,v,8);return s;}
void Field(std::string& s,unsigned id,unsigned tag,const std::string& value) {
  const auto off=s.size();s.resize(off+8,'\0');
  Put(s,off,id,2);Put(s,off+2,tag,1);Put(s,off+4,value.size(),4);s+=value;
}
std::string Header(unsigned schema,unsigned count) {
  std::string s(24,'\0');s.replace(0,4,"SBCV");
  Put(s,4,1,2);Put(s,6,24,2);Put(s,12,count,4);Put(s,16,schema,4);Put(s,20,1,2);
  return s;
}
std::string Identity(const p::TypedUuid& id) {return {reinterpret_cast<const char*>(id.value.bytes.data()),16};}
// Independent Core wire layout, not the production encoder/schema.
std::string Golden(const c::CatalogLocalizedNameRecord& r) {
  auto s=Header(65541,6);
  Field(s,1,5,Identity(r.target_object_uuid));Field(s,2,3,r.language);
  Field(s,3,3,r.path);Field(s,4,3,r.name);Field(s,5,1,Number(r.name_class));
  Field(s,6,1,Number(r.creator_transaction_number));Put(s,8,s.size(),4);return s;
}
std::string Golden(const c::CatalogLocalizedCommentRecord& r) {
  auto s=Header(65542,4);
  Field(s,1,5,Identity(r.target_object_uuid));Field(s,2,3,r.language);
  Field(s,3,3,r.comment);Field(s,4,1,Number(r.creator_transaction_number));
  Put(s,8,s.size(),4);return s;
}
std::string Bytes(const c::CatalogValueEncodeResult& r) {return {r.bytes.begin(),r.bytes.end()};}
void Roundtrip(const c::CatalogLocalizedNameRecord& r) {
  const auto e=c::EncodeCatalogLocalizedName(r);Check(e.ok(),"name encode");
  if(!e.ok())return;Check(Bytes(e)==Golden(r),"complete name independent byte oracle");
  const auto d=c::DecodeCatalogLocalizedName(Bytes(e));Check(d.ok(),"name decode");
  if(d.ok())Check(Golden(*d.record)==Golden(r),"all name values preserved");
}
void Roundtrip(const c::CatalogLocalizedCommentRecord& r) {
  const auto e=c::EncodeCatalogLocalizedComment(r);Check(e.ok(),"comment encode");
  if(!e.ok())return;Check(Bytes(e)==Golden(r),"complete comment independent byte oracle");
  const auto d=c::DecodeCatalogLocalizedComment(Bytes(e));Check(d.ok(),"comment decode");
  if(d.ok())Check(Golden(*d.record)==Golden(r),"all comment values preserved");
}
void Refused(bool name,const std::string& s) {
  if(name) {const auto d=c::DecodeCatalogLocalizedName(s);Check(!d.ok()&&!d.record,"invalid name authority returned");}
  else {const auto d=c::DecodeCatalogLocalizedComment(s);Check(!d.ok()&&!d.record,"invalid comment authority returned");}
}
std::vector<unsigned> Offsets(const std::string& s) {
  std::vector<unsigned> result;
  for(unsigned off=24;off<s.size();off+=8+Get(s,off+4,4))result.push_back(off);
  return result;
}
void Malformed(bool name,const std::string& valid) {
  for(std::size_t n=0;n<valid.size();++n)Refused(name,valid.substr(0,n));
  Refused(name,valid+"x");Refused(name,std::string(131000,'x'));
  for(unsigned off : {0,4,6,8,12,16,20,22}) {auto s=valid;Put(s,off,255,1);Refused(name,s);}
  const auto offsets=Offsets(valid);
  for(unsigned i=0;i<offsets.size();++i) {
    const auto off=offsets[i];const auto tag=Get(valid,off+2,1);
    for(unsigned t=0;t<256;++t) {
      if(t==tag)continue;auto s=valid;Put(s,off+2,t,1);Refused(name,s);
    }
    for(unsigned id : {0u,65535u,i}) {auto s=valid;Put(s,off,id,2);Refused(name,s);}
    auto s=valid;Put(s,off+3,1,1);Refused(name,s);
    s=valid;Put(s,off+4,0xffffffffu,4);Refused(name,s);
    if(tag==3&&Get(valid,off+4,4)) {s=valid;s[off+8]=char(255);Refused(name,s);}
  }
  auto s=valid;Put(s,offsets.back()+8,0,8);Refused(name,s);
  if(name) {s=valid;Put(s,offsets[4]+8,0,8);Refused(name,s);}
}
void Codecs(const p::TypedUuid& target) {
  c::CatalogLocalizedNameRecord n{target,"en",std::string("x\0;=\r\n",6),"na\xC3\xAFve",1,1};
  c::CatalogLocalizedCommentRecord m{target,"en",std::string("x\0;=\r\n",6),1};
  Roundtrip(n);Roundtrip(m);Malformed(true,Golden(n));Malformed(false,Golden(m));
  Refused(false,Golden(n));Refused(true,Golden(m));
  for(unsigned kind=0;kind<256;++kind) {
    auto a=n;auto b=m;a.target_object_uuid.kind=b.target_object_uuid.kind=static_cast<p::UuidKind>(kind);
    if(kind==static_cast<unsigned>(p::UuidKind::object)) {Roundtrip(a);Roundtrip(b);}
    else {Check(!c::EncodeCatalogLocalizedName(a).ok(),"wrong name target kind");Check(!c::EncodeCatalogLocalizedComment(b).ok(),"wrong comment target kind");}
  }
  for(unsigned v=0;v<16;++v)for(unsigned variant=0;variant<4;++variant) {
    if(v==7&&variant==2)continue;auto a=n;auto b=m;
    a.target_object_uuid.value.bytes[6]=(target.value.bytes[6]&15)|(v<<4);
    a.target_object_uuid.value.bytes[8]=(target.value.bytes[8]&63)|(variant<<6);
    b.target_object_uuid=a.target_object_uuid;
    Check(!c::EncodeCatalogLocalizedName(a).ok(),"invalid name UUID encode");
    Check(!c::EncodeCatalogLocalizedComment(b).ok(),"invalid comment UUID encode");
    Refused(true,Golden(a));Refused(false,Golden(b));
  }
  {auto a=n;auto b=m;a.target_object_uuid.value={};b.target_object_uuid.value={};
    Check(!c::EncodeCatalogLocalizedName(a).ok(),"nil name encode");
    Check(!c::EncodeCatalogLocalizedComment(b).ok(),"nil comment encode");
    Refused(true,Golden(a));Refused(false,Golden(b));}
  {auto a=n;auto b=m;a.creator_transaction_number=0;b.creator_transaction_number=0;
    Check(!c::EncodeCatalogLocalizedName(a).ok(),"zero creator name encode");
    Check(!c::EncodeCatalogLocalizedComment(b).ok(),"zero creator comment encode");}
  {auto a=n;a.name_class=2;Check(!c::EncodeCatalogLocalizedName(a).ok(),"unknown name class");Refused(true,Golden(a));}
  for(unsigned slot=0;slot<3;++slot) {
    auto a=n;a.language.clear();a.path.clear();a.name.clear();
    auto& text=slot==0?a.language:slot==1?a.path:a.name;
    text.assign(130872,'x');Roundtrip(a);text+='x';
    const auto e=c::EncodeCatalogLocalizedName(a);Check(!e.ok()&&e.bytes.empty(),"oversized name emitted");
    Refused(true,Golden(a));
  }
  for(unsigned slot=0;slot<2;++slot) {
    auto b=m;b.language.clear();b.comment.clear();
    auto& text=slot==0?b.language:b.comment;text.assign(130896,'x');Roundtrip(b);text+='x';
    const auto e=c::EncodeCatalogLocalizedComment(b);Check(!e.ok()&&e.bytes.empty(),"oversized comment emitted");
    Refused(false,Golden(b));
  }
  {auto a=n;a.path.assign(70000,'p');a.name.assign(70000,'n');Check(!c::EncodeCatalogLocalizedName(a).ok(),"combined name limit");}
  {auto b=m;b.language.assign(70000,'l');b.comment.assign(70000,'c');Check(!c::EncodeCatalogLocalizedComment(b).ok(),"combined comment limit");}
  {auto a=n;auto b=m;a.language.clear();a.path.clear();a.name.clear();b.language.clear();b.comment.clear();
    a.creator_transaction_number=b.creator_transaction_number=~p::u64{0};Roundtrip(a);Roundtrip(b);}
}
p::u64 Fnv(const std::string& s) {p::u64 v=1469598103934665603ull;for(unsigned char b:s){v^=b;v*=1099511628211ull;}return v;}
std::string ReadAll(const std::string& path) {
  std::ifstream in(path,std::ios::binary);Setup(in.is_open(),"read fixture");
  return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
struct RowFixture {
  p::u64 page;std::size_t offset;std::vector<p::byte> body;c::CatalogTypedRecord record;
};
int main() {
  fs::path root;
  try {
    const auto millis=static_cast<p::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    const auto id=uuid::GenerateEngineIdentityV7(p::UuidKind::database,millis);
    const auto fsid=uuid::GenerateEngineIdentityV7(p::UuidKind::filespace,millis);
    const auto object=uuid::GenerateEngineIdentityV7(p::UuidKind::object,millis);
    Setup(id.ok()&&fsid.ok()&&object.ok(),"identity generation");Codecs(object.value);
    root=fs::temp_directory_path()/("scratchbird_localized_payload_"+uuid::UuidToString(id.value.value));
    Setup(fs::create_directory(root),"unique fixture directory");
    db::DatabaseCreateConfig cfg;cfg.path=(root/"primary.sdb").string();
    cfg.database_uuid=id.value;cfg.filespace_uuid=fsid.value;cfg.page_size=16384;
    cfg.creation_unix_epoch_millis=millis;cfg.allow_minimal_resource_bootstrap=true;cfg.require_resource_seed_pack=false;
    Setup(db::CreateDatabaseFile(cfg).ok(),"real database create");
    db::DatabaseOpenConfig open;open.path=cfg.path;open.read_only=true;open.suppress_background_agents=true;
    Check(db::OpenDatabaseFile(open).ok(),"fresh real database admission");
    std::vector<RowFixture> names,comments;
    {
      disk::FileDevice device;Setup(device.Open(cfg.path,disk::FileOpenMode::open_existing_read_only).ok(),"read actual database");
      p::u64 number=db::kCatalogPageNumber;
      for(unsigned guard=0;number!=0&&guard<1024;++guard) {
        std::vector<p::byte> body(cfg.page_size-disk::kPageHeaderSerializedBytes);
        Setup(device.ReadAt(number*cfg.page_size+disk::kPageHeaderSerializedBytes,body.data(),body.size()).ok(),"read page");
        const auto parsed=page::ParseCatalogPageBody(body,number);Setup(parsed.ok(),"parse actual page");
        std::size_t offset=page::kCatalogPageBodyHeaderBytes;
        for(const auto& row:parsed.body.rows) {
          if(row.kind==page::CatalogPageRowKind::typed_catalog_record) {
            const auto d=c::DecodeCatalogTypedRecord(row);Setup(d.ok(),"decode common record");
            if(d.record.header.kind==c::CatalogRecordKind::localized_name)names.push_back({number,offset,body,d.record});
            if(d.record.header.kind==c::CatalogRecordKind::localized_comment)comments.push_back({number,offset,body,d.record});
          }
          offset+=20+row.payload.size();
        }
        number=parsed.body.next_page_number;
      }
      Setup(number==0,"catalog page traversal did not terminate");
    }
    const std::set<std::string> expected_paths{"sys","sys.catalog","sys.metrics","sys.agents","sys.security",
      "sys.configuration","sys.management","sys.fn","sys.udr","sys.parser","sys.storage","sys.mga",
      "sys.audit","sys.compatibility","sys.information","sys.catalog_readable","sys.diagnostics",
      "cluster","users","users.public","emulated","remote","app","sys.information_schema","database"};
    Check(names.size()==25&&comments.size()==25,"complete25 localized record pairs persisted");
    std::map<std::array<p::byte,16>,std::string> paths;
    std::set<std::string> seen;
    for(const auto& row:names) {
      const auto d=c::DecodeCatalogLocalizedName(row.record.payload);Setup(d.ok(),"decode persisted name");
      const auto& n=*d.record;Check(expected_paths.contains(n.path),"unexpected bootstrap metadata path");
      const auto dot=n.path.rfind('.');const auto display=dot==std::string::npos?n.path:n.path.substr(dot+1);
      c::CatalogLocalizedNameRecord expected{row.record.header.parent_uuid,"en",n.path,display,1,1};
      Check(row.record.payload==Golden(expected),"persisted name differs from complete oracle");
      Check(seen.insert(n.path).second,"duplicate localized path");
      Check(paths.emplace(row.record.header.parent_uuid.value.bytes,n.path).second,"duplicate target name");
    }
    Check(seen==expected_paths,"bootstrap localized paths missing");
    for(const auto& row:comments) {
      const auto it=paths.find(row.record.header.parent_uuid.value.bytes);Setup(it!=paths.end(),"comment target lacks name");
      const auto& path=it->second;
      const auto comment=path=="database"?"ScratchBird bootstrap database object":
          path=="sys.information_schema"?"Legacy SQL information schema synonym for sys.information":
          "ScratchBird bootstrap schema: "+path;
      c::CatalogLocalizedCommentRecord expected{row.record.header.parent_uuid,"en",comment,1};
      Check(row.record.payload==Golden(expected),"persisted comment differs from complete oracle");
    }
    Setup(!names.empty()&&!comments.empty(),"localized fixtures missing");
    const auto write=[&](const RowFixture& row,const std::vector<p::byte>& body) {
      disk::FileDevice device;Setup(device.Open(cfg.path,disk::FileOpenMode::open_existing).ok(),"mutation device");
      Setup(device.WriteAt(row.page*cfg.page_size+disk::kPageHeaderSerializedBytes,body.data(),body.size()).ok(),"write mutation");
      Setup(device.Sync().ok(),"sync mutation");
    };
    const auto mutate=[&](const RowFixture& row,const std::string& payload,bool admitted) {
      auto body=row.body;
      const auto size=p::LoadLittle32(body.data()+row.offset+8);
      std::string record(reinterpret_cast<const char*>(body.data()+row.offset+20),size);
      Setup(payload.size()==row.record.payload.size()&&size==96+payload.size(),"mutation length");
      record.replace(96,payload.size(),payload);
      std::copy(record.begin(),record.end(),body.begin()+row.offset+20);
      p::StoreLittle64(body.data()+row.offset+12,Fnv(record));
      p::StoreLittle64(body.data()+40,page::ComputeCatalogPageBodyChecksum(body));
      Setup(page::ParseCatalogPageBody(body,row.page).ok(),"mutation must keep valid row/page checksums");
      write(row,body);const auto before=ReadAll(cfg.path);
      Check(db::OpenDatabaseFile(open).ok()==admitted,"actual localized admission result");
      Check(ReadAll(cfg.path)==before,"read-only localized admission changed bytes");
      if(!admitted) {
        auto writable=open;writable.read_only=false;
        Check(!db::OpenDatabaseFile(writable).ok(),"invalid localized payload admitted writable");
        Check(ReadAll(cfg.path)==before,"failed writable admission changed bytes");
      }
      write(row,row.body);Check(db::OpenDatabaseFile(open).ok(),"valid payload did not recover");
    };
    for(const auto* row:{&names.front(),&comments.front()}) {
      const auto valid=row->record.payload;
      auto s=valid;s.replace(32,16,Identity(object.value));mutate(*row,s,false);
      s=valid;s[38]&=15;mutate(*row,s,false);
      s=valid;s[16]^=1;mutate(*row,s,false);
      const auto off=Offsets(valid);
      s=valid;Put(s,off.back()+8,0,8);mutate(*row,s,false);
      s=valid;s[56]=char(255);mutate(*row,s,false);
      s.assign(valid.size(),'x');s.replace(0,19,"target_object_uuid=");mutate(*row,s,false);
      // Framed UTF-8 metadata is lossless, including embedded NUL and newline.
      s=valid;s[off[2]+8]='\0';s[off[2]+9]='\n';mutate(*row,s,true);
    }
    {const auto& row=names.front();auto s=row.record.payload;Put(s,Offsets(s)[4]+8,2,8);mutate(row,s,false);}
    if(failures){std::cerr<<"fixture retained at "<<root<<'\n';return 1;}
    fs::remove_all(root); // Only the unique test-owned directory above.
    std::cout<<"checks="<<checks<<" failures=0 localized_pairs="<<names.size()<<"\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<"; fixture retained at "<<root<<'\n';return 1;}
}

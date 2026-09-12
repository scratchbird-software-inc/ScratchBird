// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_resource_record_codec.hpp"
#include <array>
#include <iostream>
#include <limits>
#include <string>

namespace c=scratchbird::core::catalog;
namespace p=scratchbird::core::platform;
unsigned checks=0,failures=0;
void Check(bool ok,const char* reason) {
  ++checks;
  if (!ok && ++failures<20) std::cerr<<"FAIL "<<reason<<'\n';
}
p::TypedUuid Id(unsigned tail) {
  p::TypedUuid id;
  id.kind=p::UuidKind::object;
  id.value.bytes={0x01,0xa0,0x80,0,0,0,0x70,0,0x80,0,0,0,0,0,0,static_cast<p::byte>(tail)};
  return id;
}
std::string Bytes(const c::CatalogValueEncodeResult& r) { return {r.bytes.begin(),r.bytes.end()}; }
void Put(std::string& s,std::size_t offset,std::uint64_t value,unsigned width) {
  for(unsigned n=0;n<width;++n)s[offset+n]=static_cast<char>((value>>(8*n))&255);
}
std::string Number(std::uint64_t value,unsigned width=8) {
  std::string s(width,'\0');Put(s,0,value,width);return s;
}
std::string Identity(const p::TypedUuid& id) {
  return {reinterpret_cast<const char*>(id.value.bytes.data()),16};
}
std::string List(const std::vector<std::string>& values) {
  std::string s=Number(values.size(),4);
  for(const auto& v:values)s+=Number(v.size(),4)+v;
  return s;
}
struct Oracle {
  std::string bytes=std::string(24,'\0'); unsigned count=0;
  explicit Oracle(unsigned schema) {
    bytes.replace(0,4,"SBCV");Put(bytes,4,1,2);Put(bytes,6,24,2);
    Put(bytes,16,schema,4);Put(bytes,20,1,2);
  }
  void Field(unsigned id,unsigned tag,const std::string& value) {
    bytes+=Number(id,2)+Number(tag,1)+Number(0,1)+Number(value.size(),4)+value;++count;
  }
  std::string Finish() {Put(bytes,8,bytes.size(),4);Put(bytes,12,count,4);return bytes;}
};
std::string Golden(const c::CatalogCharsetRecord& r) {
  // Independent field IDs/tags/order from Core; no production schema or encoder.
  Oracle o(65556);
  o.Field(1,1,Number(1));o.Field(2,3,r.canonical_name);o.Field(3,5,Identity(r.resource_uuid));
  o.Field(4,8,List(r.aliases));o.Field(5,3,r.description);o.Field(6,1,Number(r.min_bytes));
  o.Field(7,1,Number(r.max_bytes));o.Field(8,2,Number(r.variable_width,1));
  o.Field(9,3,r.encoding_type);o.Field(10,3,r.iana_name);o.Field(11,8,List(r.supported_by));
  o.Field(12,3,r.default_collation_name);
  if(r.default_collation_uuid)o.Field(13,5,Identity(*r.default_collation_uuid));
  o.Field(14,3,r.source_path);o.Field(15,1,Number(r.resource_epoch));o.Field(16,1,Number(r.family_epoch));
  o.Field(17,3,r.family_version);o.Field(18,3,r.resource_seed_pack);o.Field(19,3,r.resource_seed_version);
  o.Field(20,2,Number(r.loaded_at_database_create,1));o.Field(21,2,Number(r.engine_owned,1));
  o.Field(22,1,Number(r.creator_transaction_number));return o.Finish();
}
std::string Golden(const c::CatalogCollationRecord& r) {
  Oracle o(65558);
  o.Field(1,1,Number(2));o.Field(2,3,r.canonical_name);o.Field(3,5,Identity(r.resource_uuid));
  o.Field(4,3,r.charset_name);o.Field(5,5,Identity(r.charset_uuid));
  o.Field(6,2,Number(r.default_for_charset,1));o.Field(7,3,r.default_authority);
  o.Field(8,2,Number(r.case_insensitive,1));o.Field(9,2,Number(r.accent_insensitive,1));
  o.Field(10,3,r.language);o.Field(11,3,r.description);o.Field(12,8,List(r.supported_by));
  o.Field(13,3,r.source_path);o.Field(14,1,Number(r.resource_epoch));o.Field(15,1,Number(r.family_epoch));
  o.Field(16,3,r.family_version);o.Field(17,3,r.resource_seed_pack);o.Field(18,3,r.resource_seed_version);
  o.Field(19,2,Number(r.loaded_at_database_create,1));o.Field(20,2,Number(r.engine_owned,1));
  o.Field(21,1,Number(r.creator_transaction_number));return o.Finish();
}
c::CatalogCharsetRecord Charset() {
  c::CatalogCharsetRecord r;
  r.canonical_name="UTF8";r.resource_uuid=Id(1);r.aliases={"utf-8","a,b",""};
  r.description=std::string("a\0\r\nb",5);r.min_bytes=1;r.max_bytes=4;r.variable_width=true;
  r.encoding_type="utf8";r.iana_name="UTF-8";r.supported_by={"native","a|b"};
  r.default_collation_name="UTF8_BIN";r.default_collation_uuid=Id(2);r.source_path="seed/charsets.json";
  r.resource_epoch=7;r.family_epoch=8;r.family_version="1";r.resource_seed_pack="pack";
  r.resource_seed_version="2026.1";r.loaded_at_database_create=true;r.engine_owned=true;
  r.creator_transaction_number=1;return r;
}
c::CatalogCollationRecord Collation() {
  c::CatalogCollationRecord r;
  r.canonical_name="UTF8_BIN";r.resource_uuid=Id(2);r.charset_name="UTF8";r.charset_uuid=Id(1);
  r.default_for_charset=true;r.default_authority="seed_pack.default_collations.v1";
  r.language="und";r.description=std::string("binary\0tail",11);r.supported_by={"native","a|b"};
  r.source_path="seed/collations.json";r.resource_epoch=7;r.family_epoch=9;r.family_version="1";
  r.resource_seed_pack="pack";r.resource_seed_version="2026.1";r.loaded_at_database_create=true;
  r.engine_owned=true;r.creator_transaction_number=1;return r;
}
template <typename Record,typename Encode,typename Decode>
void Codec(const Record& base,Encode encode,Decode decode) {
  const auto golden=Golden(base),encoded=Bytes(encode(base));
  Check(golden==encoded,"independent exact resource payload bytes");
  const auto decoded=decode(golden);
  Check(decoded.ok() && Golden(*decoded.record)==golden,"all resource fields preserved");
  const auto refuse=[&](const std::string& bytes) {
    const auto r=decode(bytes);Check(!r.ok() && !r.record,"invalid resource payload returned authority");
  };
  for(std::size_t n=0;n<golden.size();++n)refuse(golden.substr(0,n));
  refuse(golden+"x");refuse("creator_tx=1\ncanonical_name=UTF8\n");
  std::vector<std::size_t> offsets;
  for(unsigned n=0;n<24;++n)offsets.push_back(n);
  for(std::size_t off=24;off<golden.size();) {
    for(unsigned n=0;n<8;++n)offsets.push_back(off+n);
    std::uint32_t size=0;for(unsigned n=0;n<4;++n)size|=static_cast<unsigned char>(golden[off+4+n])<<(8*n);
    off+=8+size;
  }
  for(const auto offset:offsets)for(unsigned n=0;n<256;++n) {
    if(static_cast<unsigned char>(golden[offset])==n)continue;
    auto bad=golden;bad[offset]=static_cast<char>(n);refuse(bad);
  }
  auto r=base;r.canonical_name.clear();Check(!encode(r).ok(),"empty resource name admitted");
  r=base;r.resource_epoch=0;Check(!encode(r).ok(),"zero resource epoch admitted");
  r=base;r.family_epoch=0;Check(!encode(r).ok(),"zero family epoch admitted");
  r=base;r.creator_transaction_number=0;Check(!encode(r).ok(),"zero creator ordering admitted");
  r=base;r.description=std::string(130976,'x');Check(!encode(r).ok(),"combined record size overflow admitted");
  for(unsigned version=0;version<16;++version)if(version!=7) {
    r=base;r.resource_uuid.value.bytes[6]=static_cast<p::byte>(version<<4);
    Check(!encode(r).ok(),"non-v7 resource identity admitted");
  }
  r=base;r.resource_uuid.kind=p::UuidKind::schema;Check(!encode(r).ok(),"wrong resource identity kind admitted");
}
c::CatalogTypedRecord Header(c::CatalogRecordKind kind,p::TypedUuid object,p::TypedUuid parent,std::string payload) {
  c::CatalogTypedRecord r;r.header.kind=kind;r.header.object_uuid=object;r.header.parent_uuid=parent;
  r.payload=std::move(payload);return r;
}
int main() {
  const auto charset=Charset();const auto collation=Collation();
  Codec(charset,c::EncodeCatalogCharsetRecord,c::DecodeCatalogCharsetRecord);
  Codec(collation,c::EncodeCatalogCollationRecord,c::DecodeCatalogCollationRecord);
  auto no_default=charset;no_default.default_collation_name.clear();no_default.default_collation_uuid.reset();
  Codec(no_default,c::EncodeCatalogCharsetRecord,c::DecodeCatalogCharsetRecord);
  auto bad_charset=charset;bad_charset.default_collation_uuid.reset();
  Check(!c::EncodeCatalogCharsetRecord(bad_charset).ok(),"missing named default identity admitted");
  bad_charset=charset;bad_charset.variable_width=false;Check(!c::EncodeCatalogCharsetRecord(bad_charset).ok(),"width inconsistency admitted");
  bad_charset=charset;bad_charset.min_bytes=5;Check(!c::EncodeCatalogCharsetRecord(bad_charset).ok(),"reversed width admitted");
  bad_charset=charset;bad_charset.max_bytes=std::numeric_limits<std::uint64_t>::max();
  Check(!c::EncodeCatalogCharsetRecord(bad_charset).ok(),"u32 width overflow admitted");
  auto bad_collation=collation;bad_collation.default_authority.clear();
  Check(!c::EncodeCatalogCollationRecord(bad_collation).ok(),"default without authority admitted");
  const auto bundle=Header(c::CatalogRecordKind::resource_bundle,Id(3),Id(4),"bundle");
  const auto charset_row=Header(c::CatalogRecordKind::charset,Id(1),Id(3),Golden(charset));
  const auto collation_row=Header(c::CatalogRecordKind::collation,Id(2),Id(1),Golden(collation));
  const std::vector<c::CatalogTypedRecord> graph{bundle,charset_row,collation_row};
  Check(c::ValidateCatalogResourceGraph(graph),"valid binary resource graph refused");
  for(unsigned remove=0;remove<3;++remove) {
    auto g=graph;g.erase(g.begin()+remove);Check(!c::ValidateCatalogResourceGraph(g),"dangling resource graph admitted");
  }
  auto g=graph;g.push_back(charset_row);Check(!c::ValidateCatalogResourceGraph(g),"duplicate resource identity admitted");
  g=graph;g[0].header.kind=c::CatalogRecordKind::schema;Check(!c::ValidateCatalogResourceGraph(g),"wrong bundle class admitted");
  for(unsigned index:{1,2}) {
    g=graph;g[index].header.object_uuid=Id(9);Check(!c::ValidateCatalogResourceGraph(g),"payload/header identity mismatch admitted");
    g=graph;g[index].header.parent_uuid=Id(9);Check(!c::ValidateCatalogResourceGraph(g),"wrong parent identity admitted");
    g=graph;g[index].header.deleted=true;Check(!c::ValidateCatalogResourceGraph(g),"deleted resource admitted");
  }
  bad_collation=collation;bad_collation.resource_epoch=99;g=graph;g[2].payload=Golden(bad_collation);
  Check(!c::ValidateCatalogResourceGraph(g),"foreign resource epoch admitted");
  bad_collation=collation;bad_collation.resource_seed_pack="foreign";g=graph;g[2].payload=Golden(bad_collation);
  Check(!c::ValidateCatalogResourceGraph(g),"foreign seed pack admitted");
  bad_collation=collation;bad_collation.default_for_charset=false;g=graph;g[2].payload=Golden(bad_collation);
  Check(!c::ValidateCatalogResourceGraph(g),"false default target admitted");
  g=graph;g[1].payload=Golden(no_default);Check(!c::ValidateCatalogResourceGraph(g),"missing default back reference admitted");
  std::cout<<"catalog resource records checks="<<checks<<" failures="<<failures<<'\n';
  return failures?1:0;
}

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_name_record_codec.hpp"
#include "catalog_language.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>

namespace c = scratchbird::core::catalog;
using namespace scratchbird::core::platform;
unsigned checks=0, failures=0;
void Check(bool ok,const char* message) {
  ++checks;
  if (!ok && ++failures<16) std::cerr<<"FAIL "<<message<<'\n';
}
TypedUuid Identity(UuidKind kind, byte seed) {
  Uuid value{{0x01,0x92,0x13,0x24,0x35,0x46,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,seed}};
  return {kind,value};
}
c::CatalogNameVector Vector() {
  c::CatalogNameVector r;
  r.name_vector_uuid=Identity(UuidKind::object,1);
  r.object_uuid=Identity(UuidKind::object,2);
  r.object_kind=c::CatalogRecordKind::table_descriptor;
  r.owning_schema_uuid=Identity(UuidKind::schema,4);
  // SB_NATIVE_NAME_DEFAULTS_ON_ENTRIES (owner Q228 revisited): no
  // default_language_uuid (was fr-CA) or default_name_entry_uuid (was 6).
  r.name_collision_policy_uuid=Identity(UuidKind::object,7);
  r.catalog_generation_id=0x0102030405060708;
  r.security_policy_uuid=Identity(UuidKind::object,9);
  r.lifecycle_state=c::CatalogNameLifecycle::active;
  return r;
}
c::CatalogNameEntry Entry() {
  c::CatalogNameEntry r;
  r.name_entry_uuid=Identity(UuidKind::object,1);
  r.name_vector_uuid=Identity(UuidKind::object,2);
  r.object_uuid=Identity(UuidKind::object,3);
  r.object_kind=c::CatalogRecordKind::table_descriptor;
  r.scope_uuid=Identity(UuidKind::object,5);
  r.parent_object_uuid=Identity(UuidKind::object,6);
  r.parent_schema_uuid=Identity(UuidKind::schema,7);
  r.language_uuid=c::CatalogLanguageIdentity(c::kCatalogLanguage_fr_CA);
  r.name_class=c::CatalogNameClass::alias;
  r.reference_engine_uuid=Identity(UuidKind::object,10);
  r.dialect_profile_uuid=Identity(UuidKind::object,11);
  r.identifier_profile_uuid=Identity(UuidKind::object,12);
  r.case_fold_profile_uuid=Identity(UuidKind::object,13);
  r.quoted_identifier_profile_uuid=Identity(UuidKind::object,14);
  r.raw_name_text=std::string("A\n\r\0=B",6);
  // SB_NATIVE_NAME_ROW_V4 (owner Q147 = 1): no display_name. (Was: "Nom".)
  r.was_quoted=true;
  r.quote_style=c::CatalogNameQuoteStyle::double_quote;
  r.requires_exact_match=true;
  // SB_NATIVE_NAME_ROW_V4 (owner Q147 = 1, Q157 = 1): no stored keys or path.
  // (Was: normalized {0xff,0,0xc0,0xaf}, exact {0,0x80,0xfe}, full path
  // {0xff,0,0xee}, count 2.)
  r.search_path_eligible=false;
  r.default_for_language=true;
  r.default_for_object=false;
  r.catalog_generation_id=0x1112131415161718;
  r.created_transaction_uuid=Identity(UuidKind::transaction,28);
  r.dropped_transaction_uuid=Identity(UuidKind::transaction,29);
  r.security_policy_uuid=Identity(UuidKind::object,30);
  r.resource_epoch=0x2122232425262728;
  r.name_resolution_epoch=0x3132333435363738;
  r.lifecycle_state=c::CatalogNameLifecycle::dropping;
  return r;
}
// Independent field/byte oracle: numeric tags and field IDs transcribed from
// Core, not obtained from production schema or value codec serialization.
struct Field {
  u16 id;
  byte tag;
  std::vector<byte> bytes;
  UuidKind kind=UuidKind::unknown; // oracle-only; kind is not serialized in the value
  bool operator==(const Field&) const = default;
};
std::vector<byte> Little(u64 value,unsigned width) {
  std::vector<byte> out(width);
  for (unsigned i=0;i<width;++i) { out[i]=value%256; value/=256; }
  return out;
}
Field Number(u16 id,u64 v) { return {id,1,Little(v,8)}; }
Field Bool(u16 id,bool v) { return {id,2,{static_cast<byte>(v)}}; }
Field Text(u16 id,const std::string& v) { return {id,3,{v.begin(),v.end()}}; }
Field Bytes(u16 id,const std::vector<byte>& v) { return {id,4,v}; }
Field Id(u16 id,const TypedUuid& v) { return {id,5,{v.value.bytes.begin(),v.value.bytes.end()},v.kind}; }
void Optional(std::vector<Field>& f,u16 id,const std::optional<TypedUuid>& v) { if(v)f.push_back(Id(id,*v)); }
std::vector<Field> Fields(const c::CatalogNameVector& r) {
  std::vector<Field> f{Id(1,r.name_vector_uuid),Id(2,r.object_uuid),Number(3,static_cast<u64>(r.object_kind))};
  Optional(f,4,r.owning_schema_uuid);
  // (Was: 5 default_language_uuid, 6 default_name_entry_uuid; vector version 3.)
  f.push_back(Id(7,r.name_collision_policy_uuid));f.push_back(Number(8,r.catalog_generation_id));
  f.push_back(Id(9,r.security_policy_uuid));f.push_back(Number(10,static_cast<u64>(r.lifecycle_state)));
  return f;
}
std::vector<Field> Fields(const c::CatalogNameEntry& r) {
  std::vector<Field> f{Id(1,r.name_entry_uuid),Id(2,r.name_vector_uuid),Id(3,r.object_uuid),
    Number(4,static_cast<u64>(r.object_kind)),Id(5,r.scope_uuid)};
  Optional(f,6,r.parent_object_uuid);Optional(f,7,r.parent_schema_uuid);
  f.push_back(Id(8,r.language_uuid));f.push_back(Number(9,static_cast<u64>(r.name_class)));
  Optional(f,10,r.reference_engine_uuid);f.push_back(Id(11,r.dialect_profile_uuid));
  f.push_back(Id(12,r.identifier_profile_uuid));Optional(f,13,r.case_fold_profile_uuid);
  Optional(f,14,r.quoted_identifier_profile_uuid);f.push_back(Text(15,r.raw_name_text));
  // SB_NATIVE_NAME_ROW_V4: 16 and 20-23 are version 3's (see V3Fields).
  f.push_back(Bool(17,r.was_quoted));
  f.push_back(Number(18,static_cast<u64>(r.quote_style)));f.push_back(Bool(19,r.requires_exact_match));
  f.push_back(Bool(24,r.search_path_eligible));f.push_back(Bool(25,r.default_for_language));
  f.push_back(Bool(26,r.default_for_object));f.push_back(Number(27,r.catalog_generation_id));
  f.push_back(Id(28,r.created_transaction_uuid));Optional(f,29,r.dropped_transaction_uuid);
  f.push_back(Id(30,r.security_policy_uuid));f.push_back(Number(31,r.resource_epoch));
  f.push_back(Number(32,r.name_resolution_epoch));f.push_back(Number(33,static_cast<u64>(r.lifecycle_state)));
  return f;
}
// Version 3 of the entry (before SB_NATIVE_NAME_ROW_V4): the version 4 fields
// with display_name (16), both lookup keys (20, 21), the full path (22) and its
// count (23) in their places, as version 3 wrote them.
std::vector<Field> V3Fields(const c::CatalogNameEntry& r) {
  auto f=Fields(r);
  const auto at=[&](u16 id){return std::find_if(f.begin(),f.end(),[&](const Field& x){return x.id>id;});};
  f.insert(at(15),Text(16,r.raw_name_text));
  f.insert(at(19),{Bytes(20,{'n'}),Bytes(21,{'e'}),Bytes(22,{'n',0,0}),Number(23,1)});
  return f;
}
std::vector<byte> Oracle(u32 schema,const std::vector<Field>& fields,unsigned version=0) {
  std::vector<byte> out{0x53,0x42,0x43,0x56,1,0,24,0};
  u32 size=24;
  for(const auto& f:fields)size+=8+f.bytes.size();
  const auto append=[&](const std::vector<byte>& value){out.insert(out.end(),value.begin(),value.end());};
  append(Little(size,4));append(Little(fields.size(),4));append(Little(schema,4));
  // Schema version 4 for both: the vector's (SB_NATIVE_NAME_DEFAULTS_ON_ENTRIES,
  // owner Q228 revisited) and the entry's (SB_NATIVE_NAME_ROW_V4). (Was: the
  // vector's 3, SB_CATALOG_NO_TEXT_IDENTIFIERS; before that 3 for both.)
  if(!version)version=4;
  append({static_cast<byte>(version),0,0,0});
  for(const auto& f:fields){append(Little(f.id,2));append({f.tag,0});append(Little(f.bytes.size(),4));append(f.bytes);}
  return out;
}
#ifndef SB_NATIVE_NAME_MEMORY_TESTS
template<class F> auto WithoutNameHeap(F action){return action();}
#endif
c::CatalogValueEncodeResult Encode(const c::CatalogNameVector& r){
  const auto error=WithoutNameHeap([&]{return c::ValidateCatalogNameVector(c::BorrowCatalogNameVector(r));});
  auto result=c::EncodeCatalogNameVector(r);
  Check(error==result.error,"vector no-encode admission preserves exact encoder error");
  return result;
}
c::CatalogValueEncodeResult Encode(const c::CatalogNameEntry& r){
  const auto error=WithoutNameHeap([&]{return c::ValidateCatalogNameEntry(c::BorrowCatalogNameEntry(r));});
  auto result=c::EncodeCatalogNameEntry(r);
  Check(error==result.error,"entry no-encode admission preserves exact encoder error");
  return result;
}
auto DecodeView(const c::CatalogNameVector&,std::span<const byte> b){
  return WithoutNameHeap([&]{return c::DecodeCatalogNameVectorView(b);});
}
auto DecodeView(const c::CatalogNameEntry&,std::span<const byte> b){
  return WithoutNameHeap([&]{return c::DecodeCatalogNameEntryView(b);});
}
auto Materialize(const c::CatalogNameVectorView& v){return c::MaterializeCatalogNameVector(v);}
auto Materialize(const c::CatalogNameEntryView& v){return c::MaterializeCatalogNameEntry(v);}
template<class Record,class Result> auto CheckedDecode(const Record& r,const std::vector<byte>& b,Result owning){
  const auto borrowed=DecodeView(r,b);
  Check(borrowed.error==owning.error && borrowed.ok()==owning.ok(),"borrowed/owning exact error parity");
  if(borrowed.ok())Check(Fields(Materialize(*borrowed.record))==Fields(*owning.record),"borrowed complete field parity");
  else Check(!borrowed.record,"no partial borrowed record");
  return owning;
}
auto Decode(const c::CatalogNameVector& r,const std::vector<byte>& b){return CheckedDecode(r,b,c::DecodeCatalogNameVector(b));}
auto Decode(const c::CatalogNameEntry& r,const std::vector<byte>& b){return CheckedDecode(r,b,c::DecodeCatalogNameEntry(b));}
u32 SchemaId(const c::CatalogNameVector&){return 327681;}
u32 SchemaId(const c::CatalogNameEntry&){return 327682;}
template<class Result>void Refused(const Result& r) {
  Check(!r.ok() && r.error!=c::CatalogValueError::none,"malformed name record admitted");
  if constexpr(requires {r.record;})Check(!r.record.has_value(),"partial typed record exposed");
  else Check(r.bytes.empty(),"partial encoded record exposed");
}
template<class Record>void Roundtrip(const Record& record) {
  const auto fields=Fields(record);
  const auto expected=Oracle(SchemaId(record),fields);
  const auto encoded=Encode(record);
  Check(encoded.ok() && encoded.bytes==expected,"all-field independent byte oracle mismatch");
  const auto decoded=Decode(record,expected);
  Check(decoded.ok(),"independent oracle bytes refused");
  if(decoded.ok())Check(Fields(*decoded.record)==fields,"decoded field value/kind/absence changed");
}
template<class Record>void Invalid(const Record& record) {
  Refused(Encode(record));
  Refused(Decode(record,Oracle(SchemaId(record),Fields(record))));
}
template<class Record>void MutateUuid(const Record& base,u16 id,auto access) {
  auto r=base; const auto expected=access(r).kind;
  for(unsigned kind=0;kind<256;++kind) {
    r=base;access(r).kind=static_cast<UuidKind>(kind);
    if(kind==static_cast<unsigned>(expected))Roundtrip(r);
    else Refused(Encode(r)); // expected kind is schema-bound, never trusted from wire
  }
  for(unsigned version=0;version<16;++version)for(unsigned variant=0;variant<4;++variant) {
    r=base;access(r).value.bytes[6]=static_cast<byte>((version<<4)|7);
    access(r).value.bytes[8]=static_cast<byte>((variant<<6)|25);
    if(version==7 && variant==2)Roundtrip(r);else Invalid(r);
  }
  r=base;access(r).value={};Invalid(r);
  // Every field must bind to the documented numeric id and identity type.
  auto fields=Fields(base);
  const auto found=std::find_if(fields.begin(),fields.end(),[&](const auto& f){return f.id==id;});
  Check(found!=fields.end() && found->tag==5 && found->kind==expected,"identity schema/fixture drift");
}
void Identities() {
  auto v=Vector();
  MutateUuid(v,1,[](auto& r)->auto&{return r.name_vector_uuid;});
  MutateUuid(v,2,[](auto& r)->auto&{return r.object_uuid;});
  MutateUuid(v,4,[](auto& r)->auto&{return *r.owning_schema_uuid;});
  // (Was: MutateUuid of field 6, default_name_entry_uuid; retired.)
  MutateUuid(v,7,[](auto& r)->auto&{return r.name_collision_policy_uuid;});
  MutateUuid(v,9,[](auto& r)->auto&{return r.security_policy_uuid;});
  auto e=Entry();
  MutateUuid(e,1,[](auto& r)->auto&{return r.name_entry_uuid;});
  MutateUuid(e,2,[](auto& r)->auto&{return r.name_vector_uuid;});
  MutateUuid(e,3,[](auto& r)->auto&{return r.object_uuid;});
  MutateUuid(e,5,[](auto& r)->auto&{return r.scope_uuid;});
  MutateUuid(e,6,[](auto& r)->auto&{return *r.parent_object_uuid;});
  MutateUuid(e,7,[](auto& r)->auto&{return *r.parent_schema_uuid;});
  MutateUuid(e,10,[](auto& r)->auto&{return *r.reference_engine_uuid;});
  MutateUuid(e,11,[](auto& r)->auto&{return r.dialect_profile_uuid;});
  MutateUuid(e,12,[](auto& r)->auto&{return r.identifier_profile_uuid;});
  MutateUuid(e,13,[](auto& r)->auto&{return *r.case_fold_profile_uuid;});
  MutateUuid(e,14,[](auto& r)->auto&{return *r.quoted_identifier_profile_uuid;});
  MutateUuid(e,28,[](auto& r)->auto&{return r.created_transaction_uuid;});
  MutateUuid(e,29,[](auto& r)->auto&{return *r.dropped_transaction_uuid;});
  MutateUuid(e,30,[](auto& r)->auto&{return r.security_policy_uuid;});
  v.owning_schema_uuid.reset();Roundtrip(v);
  for(unsigned mask=0;mask<64;++mask) {
    e=Entry();
    if(mask&1)e.parent_object_uuid.reset();if(mask&2)e.parent_schema_uuid.reset();
    if(mask&4)e.case_fold_profile_uuid.reset();if(mask&8)e.quoted_identifier_profile_uuid.reset();
    if(mask&16)e.dropped_transaction_uuid.reset();
    if(mask&32)e.reference_engine_uuid.reset();
    Roundtrip(e);
  }
}
template<class Record>void Framing(const Record& record,const std::vector<u16>& optional) {
  const auto fields=Fields(record);const auto bytes=Oracle(SchemaId(record),fields);
  for(std::size_t n=0;n<bytes.size();++n)Refused(Decode(record,{bytes.begin(),bytes.begin()+n}));
  for(std::size_t i=0;i<fields.size();++i) {
    auto missing=fields;missing.erase(missing.begin()+i);
    auto decoded=Decode(record,Oracle(SchemaId(record),missing));
    if(std::find(optional.begin(),optional.end(),fields[i].id)!=optional.end())
      Check(decoded.ok() && Fields(*decoded.record)==missing,"optional field omission changed values");
    else Refused(decoded);
    for(unsigned tag=0;tag<256;++tag)if(tag!=fields[i].tag) {
      auto wrong=fields;wrong[i].tag=tag;Refused(Decode(record,Oracle(SchemaId(record),wrong)));
    }
    auto duplicate=fields;duplicate.insert(duplicate.begin()+i,fields[i]);
    Refused(Decode(record,Oracle(SchemaId(record),duplicate)));
    auto malformed=fields;malformed[i].id=0;Refused(Decode(record,Oracle(SchemaId(record),malformed)));
  }
  auto extra=fields;extra.push_back(Text(65535,"ignored?"));Refused(Decode(record,Oracle(SchemaId(record),extra)));
  for(const byte old:{byte{1},byte{2},byte{3}}){auto wrong_version=bytes;wrong_version[20]=old;if(old!=bytes[20])Refused(Decode(record,wrong_version));}  // 1: text class, 2: text language, 3: the entry's stored keys (SB_NATIVE_NAME_ROW_V4)
  auto reversed=fields;std::reverse(reversed.begin(),reversed.end());Refused(Decode(record,Oracle(SchemaId(record),reversed)));
  const std::string legacy="kind=5\nrow_uuid=01921324-3546-7788-99aa-bbccddeeff01\npayload=name=x\n";
  Refused(Decode(record,{legacy.begin(),legacy.end()}));
}
void SchemaContract() {
  const auto& v=c::CatalogNameVectorSchema();const auto& e=c::CatalogNameEntrySchema();
  // SB_NATIVE_NAME_ROW_V4 (owner Q147 = 1, Q157 = 1): the entry is version 4
  // with 28 fields. (Was: version 3, 33 fields.)
  // SB_NATIVE_NAME_DEFAULTS_ON_ENTRIES (owner Q228 revisited): the vector is
  // version 4 with 8 fields (5 and 6 retired). (Was: version 3, 10 fields.)
  Check(v.id==327681 && e.id==327682 && v.version==4 && e.version==4,"allocated schema identity drift");
  Check(v.fields.size()==8 && e.fields.size()==28,"Core field omitted");
  // A version 3 vector (with the default fields) is refused (Q164 = 1).
  {
    auto v3=Fields(Vector());
    const auto at=std::find_if(v3.begin(),v3.end(),[](const Field& x){return x.id>4;});
    v3.insert(at,{Id(5,c::CatalogLanguageIdentity(c::kCatalogLanguage_fr_CA)),Id(6,Identity(UuidKind::object,6))});
    Refused(c::DecodeCatalogNameVector(Oracle(327681,v3,3)));
    Refused(c::DecodeCatalogNameVector(Oracle(327681,v3,4)));  // the retired fields under version 4
  }
  // A version 3 entry (with the retired fields) is refused: no migration (Q164 = 1).
  Refused(c::DecodeCatalogNameEntry(Oracle(327682,V3Fields(Entry()),3)));
  Refused(c::DecodeCatalogNameEntryView(Oracle(327682,V3Fields(Entry()),3)));
  Refused(c::DecodeCatalogNameEntry(Oracle(327682,V3Fields(Entry()),4)));  // the retired fields under version 4
  const auto verify=[](const auto& schema,const auto& fields,const std::vector<u16>& optional) {
    for(std::size_t i=0;i<fields.size();++i) {
      const auto& s=schema.fields[i];const auto& f=fields[i];
      Check(s.id==f.id && static_cast<byte>(s.type)==f.tag && s.identity_kind==f.kind,"field schema mismatch");
      Check(s.required==(std::find(optional.begin(),optional.end(),f.id)==optional.end()),"field requirement mismatch");
      const u32 maximum=f.tag==5?16:f.tag==1?8:f.tag==2?1:131040;
      Check(s.maximum_bytes==maximum,"field limit mismatch");
    }
  };
  verify(v,Fields(Vector()),{4});verify(e,Fields(Entry()),{6,7,10,13,14,29});
  Refused(c::DecodeCatalogNameVector(Oracle(327682,Fields(Entry()))));
  Refused(c::DecodeCatalogNameEntry(Oracle(327681,Fields(Vector()))));
}
void SemanticBoundaries() {
  for(u64 raw=0;raw<256;++raw) {
    auto v=Vector();auto e=Entry();
    v.lifecycle_state=static_cast<c::CatalogNameLifecycle>(raw);
    e.lifecycle_state=static_cast<c::CatalogNameLifecycle>(raw);
    if(raw>=1 && raw<=7){Roundtrip(v);Roundtrip(e);}else{Invalid(v);Invalid(e);}
    e=Entry();e.name_class=static_cast<c::CatalogNameClass>(raw);
    if(raw>=1 && raw<=5)Roundtrip(e);else Invalid(e);
    e=Entry();e.quote_style=static_cast<c::CatalogNameQuoteStyle>(raw);
    if(raw<=5)Roundtrip(e);else Invalid(e);
  }
  auto v=Vector();auto e=Entry();
  v.catalog_generation_id=0;Invalid(v);
  v=Vector();v.object_kind=c::CatalogRecordKind::unknown;Invalid(v);
  v=Vector();v.object_kind=static_cast<c::CatalogRecordKind>(12);Invalid(v);  // not a listed kind
  // SB_CATALOG_LANGUAGE_CONSTANTS: a language is one of the constants, typed
  // object; every constant round-trips, anything else is refused.
  for(const auto& language:c::kCatalogLanguages){
    e=Entry();e.language_uuid=c::CatalogLanguageIdentity(language.uuid);Roundtrip(e);
  }
  // (Was: a vector without a default language, or with an engine UUID as
  // one, invalid; the vector has no default language now,
  // SB_NATIVE_NAME_DEFAULTS_ON_ENTRIES.)
  e=Entry();e.language_uuid=Identity(UuidKind::object,8);Invalid(e);
  for(unsigned kind=0;kind<256;++kind)if(kind!=static_cast<unsigned>(UuidKind::object)){
    e=Entry();e.language_uuid.kind=static_cast<UuidKind>(kind);Refused(Encode(e));
  }
  v=Vector();e=Entry();
  e.object_kind=c::CatalogRecordKind::unknown;Invalid(e);
  e=Entry();e.object_kind=static_cast<c::CatalogRecordKind>(12);Invalid(e);
 e=Entry();
  for(auto member:{&c::CatalogNameEntry::catalog_generation_id,&c::CatalogNameEntry::resource_epoch,
                   &c::CatalogNameEntry::name_resolution_epoch}) {
    e=Entry();e.*member=0;Invalid(e);
    e.*member=std::numeric_limits<u64>::max();Roundtrip(e);
  }
  // (Was: an empty full path with a count, or a count without a path, was
  // invalid; version 4 stores neither, SB_NATIVE_NAME_ROW_V4.)
  v=Vector();v.lifecycle_state=static_cast<c::CatalogNameLifecycle>(std::numeric_limits<u64>::max());Invalid(v);
  e=Entry();e.name_class=static_cast<c::CatalogNameClass>(std::numeric_limits<u64>::max());Invalid(e);
  e=Entry();e.quote_style=static_cast<c::CatalogNameQuoteStyle>(std::numeric_limits<u64>::max());Invalid(e);
  // UTF-8 validation applies to text, not binary profile-derived keys.
  // SB_NATIVE_NAME_ROW_V4: checked on the raw text, the one text field. (Was:
  // on display_name.)
  e=Entry();e.raw_name_text=std::string("\xed\xa0\x80",3);Invalid(e);
  e=Entry();e.raw_name_text.clear();Roundtrip(e);
  e=Entry();e.raw_name_text=std::string(131041,'x');Refused(Encode(e));
  auto bytes=Oracle(327682,Fields(e));Refused(Decode(e,bytes));
}
int main() {
  SchemaContract();Roundtrip(Vector());Roundtrip(Entry());Identities();
  Framing(Vector(),{4});Framing(Entry(),{6,7,10,13,14,29});SemanticBoundaries();
  std::cout<<"catalog name record schemas: checks="<<checks<<" failures="<<failures<<'\n';
  return failures?1:0;
}

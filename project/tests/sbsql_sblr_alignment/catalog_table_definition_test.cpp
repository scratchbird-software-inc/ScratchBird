// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_table_definition.hpp"
#include "catalog_table_dependencies.hpp"
#include "catalog_table_cohort_read.hpp"
#include "physical_mga_cow_store.hpp"
#include "disk_device.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
long fail_after = -1;
bool no_heap = false;
std::size_t allocations = 0, faults = 0, checks = 0;
}
void* operator new(std::size_t n) {
  ++allocations;
  if (no_heap || (fail_after >= 0 && fail_after-- == 0)) throw std::bad_alloc{};
  if (auto p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
namespace c = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
namespace {
void Check(bool ok,const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }
p::TypedUuid Id(unsigned n,p::UuidKind kind=p::UuidKind::object) {
  p::TypedUuid result{kind,{}};
  result.value.bytes[6]=0x70; result.value.bytes[8]=0x80;
  result.value.bytes[14]=static_cast<unsigned char>(n>>8); result.value.bytes[15]=static_cast<unsigned char>(n);
  return result;
}
void Put(std::string& s,std::size_t at,std::uint64_t value,unsigned width) {
  for (unsigned i=0;i<width;++i) { s.at(at+i)=static_cast<char>(value%256); value/=256; }
}
std::string Number(std::uint64_t n) { std::string s(8,0); Put(s,0,n,8); return s; }
std::string Identity(const p::TypedUuid& id) { return {reinterpret_cast<const char*>(id.value.bytes.data()),16}; }
struct Field { unsigned id,tag; std::string bytes; };
void Pair(std::vector<Field>& fields,unsigned id,const c::CatalogDefinitionReference& ref) {
  if (!ref.uuid.value.is_nil() || ref.generation) {
    fields.push_back({id,5,Identity(ref.uuid)}); fields.push_back({id+1,1,Number(ref.generation)});
  }
}
// Independent exact byte oracle: no production schema or value codec used.
std::string Wire(unsigned schema,const std::vector<Field>& fields) {
  std::string s(24,0); s.replace(0,4,"SBCV"); Put(s,4,1,2); Put(s,6,24,2);
  Put(s,12,fields.size(),4); Put(s,16,schema,4); Put(s,20,1,2);
  for (const auto& f:fields) {
    auto at=s.size(); s.resize(at+8,0); Put(s,at,f.id,2); Put(s,at+2,f.tag,1);
    Put(s,at+4,f.bytes.size(),4); s+=f.bytes;
  }
  Put(s,8,s.size(),4); return s;
}
std::vector<Field> Fields(const c::CatalogTableDefinition& d) {
  std::vector<Field> f{{1,5,Identity(d.table_uuid)},{2,5,Identity(d.database_uuid)},
      {3,5,Identity(d.origin_transaction_uuid)},{4,1,Number(d.origin_local_transaction_id)},
      {5,1,Number(d.next_column_ordinal)}};
  Pair(f,6,d.storage_descriptor); f.push_back({8,1,Number(static_cast<p::u64>(d.type_enforcement))});
  f.push_back({9,1,Number(d.type_enforcement_generation)});
  f.push_back({10,5,Identity(d.type_enforcement_transaction_uuid)}); Pair(f,11,d.coercion_profile);
  return f;
}
std::vector<Field> Fields(const c::CatalogColumnDefinition& d) {
  std::vector<Field> f{{1,5,Identity(d.column_uuid)},{2,5,Identity(d.table_uuid)},
      {3,5,Identity(d.origin_transaction_uuid)},{4,1,Number(d.origin_local_transaction_id)},
      {5,1,Number(d.ordinal)}};
  Pair(f,6,d.value_descriptor); Pair(f,8,d.datatype_descriptor);
  f.push_back({10,5,Identity(d.type_uuid)}); f.push_back({11,2,std::string(1,d.nullable?1:0)});
  Pair(f,12,d.domain); Pair(f,14,d.default_expression); Pair(f,16,d.generated_expression);
  f.push_back({18,1,Number(static_cast<p::u64>(d.generated_storage))}); Pair(f,19,d.identity_sequence);
  f.push_back({21,1,Number(static_cast<p::u64>(d.identity_mode))}); Pair(f,22,d.charset);
  Pair(f,24,d.collation); Pair(f,26,d.storage_profile); return f;
}
c::CatalogTableDefinition Table() {
  c::CatalogTableDefinition d;
  d.table_uuid=Id(1); d.database_uuid=Id(2,p::UuidKind::database);
  d.origin_transaction_uuid=Id(3,p::UuidKind::transaction); d.origin_local_transaction_id=9;
  d.next_column_ordinal=8; d.storage_descriptor={Id(4),5};
  d.type_enforcement_generation=1; d.type_enforcement_transaction_uuid=d.origin_transaction_uuid;
  return d;
}
c::CatalogColumnDefinition Column() {
  c::CatalogColumnDefinition d;
  d.column_uuid=Id(10); d.table_uuid=Id(1); d.origin_transaction_uuid=Id(3,p::UuidKind::transaction);
  d.origin_local_transaction_id=9; d.ordinal=7; d.value_descriptor={Id(11),8};
  d.datatype_descriptor={Id(12),9}; d.type_uuid=Id(13); return d;
}
std::string Bytes(const c::CatalogValueEncodeResult& r) { return {r.bytes.begin(),r.bytes.end()}; }
void Refuse(std::string_view bytes,bool table) {
  no_heap=true;
  bool rejected = table ? !c::DecodeCatalogTableDefinition(bytes).definition.has_value()
                        : !c::DecodeCatalogColumnDefinition(bytes).definition.has_value();
  no_heap=false;
  Check(rejected,"malformed family published a definition");
}
void FiniteProfiles() {
  for (unsigned mode=0;mode!=4;++mode) for (unsigned pair=0;pair!=4;++pair) {
    auto d=Table(); d.type_enforcement=static_cast<c::CatalogTableTypeEnforcement>(mode);
    if(pair&1)d.coercion_profile.uuid=Id(9);
    if(pair&2)d.coercion_profile.generation=3;
    const bool valid=(mode==1&&pair==0)||(mode==2&&pair==3);
    const auto encoded=c::EncodeCatalogTableDefinition(d);
    Check(encoded.ok()==valid && (valid||encoded.bytes.empty()),"enforcement/pair matrix differs");
    if(valid) {
      const auto golden=Wire(65700,Fields(d));
      Check(Bytes(encoded)==golden && golden.size()==(mode==1?224:264),"table independent byte oracle");
      no_heap=true; const auto decoded=c::DecodeCatalogTableDefinition(golden); no_heap=false;
      Check(decoded.ok()&&Wire(65700,Fields(*decoded.definition))==golden,"table fixed native decode");
    }
  }
  // All seven optional presence masks, both nullable values, and all enum
  // states (including one unknown state). Oracle uses raw mask predicates.
  for(unsigned mask=0;mask!=128;++mask) for(unsigned nullable=0;nullable!=2;++nullable)
    for(unsigned generated=0;generated!=4;++generated) for(unsigned identity=0;identity!=4;++identity) {
      auto d=Column();
      std::array<c::CatalogDefinitionReference*,7> refs{&d.domain,&d.default_expression,&d.generated_expression,
          &d.identity_sequence,&d.charset,&d.collation,&d.storage_profile};
      for(unsigned i=0;i!=7;++i) if(mask&(1U<<i))*refs[i]={Id(30+i),i+1};
      d.nullable=nullable; d.generated_storage=static_cast<c::CatalogGeneratedColumnStorage>(generated);
      d.identity_mode=static_cast<c::CatalogIdentityColumnMode>(identity);
      const bool gen=(mask&4)!=0, seq=(mask&8)!=0, def=(mask&2)!=0;
      const bool valid=generated<3&&identity<3&&gen==(generated!=0)&&seq==(identity!=0)&&
          !(gen&&seq)&&!(def&&(gen||seq))&&(!(mask&32)||(mask&16));
      const auto golden=Wire(65701,Fields(d));
      const auto encoded=c::EncodeCatalogColumnDefinition(d);
      Check(encoded.ok()==valid && (valid||encoded.bytes.empty()),"column finite mode matrix differs");
      no_heap=true; const auto decoded=c::DecodeCatalogColumnDefinition(golden); no_heap=false;
      Check(decoded.ok()==valid && (valid||!decoded.definition),"decode accepted invalid field combination");
      if(valid) {
        unsigned pairs=0;for(unsigned bit=0;bit<7;++bit)pairs+=(mask>>bit)&1;
        Check(golden.size()==273+40*pairs&&golden.size()<=473,
              "column exact required/optional length bounds differ");
        Check(Bytes(encoded)==golden&&Wire(65701,Fields(*decoded.definition))==golden,
              "column independent full-byte oracle differs");
      }
    }
}
void MalformedBytes() {
  for(bool table:{false,true}) {
    auto fields=table?Fields(Table()):Fields(Column());
    const auto schema=table?65700:65701; const auto golden=Wire(schema,fields);
    for(std::size_t n=0;n<golden.size();++n) Refuse(std::string_view(golden).substr(0,n),table);
    Refuse(golden+std::string(1,0),table);
    for(unsigned at:{0,4,6,8,12,16,20,22}) { auto bad=golden; bad[at]^=1; Refuse(bad,table); }
    for(std::size_t i=0;i<fields.size();++i) {
      auto bad=fields; bad.erase(bad.begin()+i); Refuse(Wire(schema,bad),table);
      for(unsigned mode=0;mode!=7;++mode) {
        bad=fields;
        if(mode==0)bad[i].tag=99;
        if(mode==1)bad[i].id=0;
        if(mode==2)bad[i].id=60000;
        if(mode==3)bad[i].bytes.pop_back();
        if(mode==4)bad[i].bytes.push_back(0);
        if(mode==5)bad.insert(bad.begin()+i,bad[i]);
        if(mode==6) { if(bad[i].tag!=5)continue; bad[i].bytes[6]=0x40; }
        Refuse(Wire(schema,bad),table);
      }
    }
  }
  auto d=Column();
  d.domain={Id(30),1};d.default_expression={Id(31),2};d.charset={Id(32),3};d.collation={Id(33),4};d.storage_profile={Id(34),5};
  const auto all=Fields(d);
  for(unsigned id:{12,13,14,15,22,23,24,25,26,27}) {
    auto bad=all; std::erase_if(bad,[&](const Field& f){return f.id==id;}); Refuse(Wire(65701,bad),false);
  }
  for(std::size_t i=0;i<all.size();++i) {
    if(all[i].tag==5) {
      auto bad=all;bad[i].bytes.assign(16,0);Refuse(Wire(65701,bad),false);
      bad=all;bad[i].bytes[8]=0x40;Refuse(Wire(65701,bad),false);
    }
    if(all[i].tag==1 && all[i].id!=5 && all[i].id!=18 && all[i].id!=21) {
      auto bad=all;bad[i].bytes.assign(8,0);Refuse(Wire(65701,bad),false);
    }
    if(all[i].tag==2) for(unsigned value=2;value!=256;++value) {
      auto bad=all;bad[i].bytes[0]=static_cast<char>(value);Refuse(Wire(65701,bad),false);
    }
  }
  // Identity kinds are schema-selected on disk, but direct typed writers must
  // reject a caller supplying the wrong kind rather than silently retyping it.
  for(unsigned field=0;field!=13;++field) {
    auto bad=d;
    std::array<p::TypedUuid*,13> ids{&bad.column_uuid,&bad.table_uuid,&bad.origin_transaction_uuid,
        &bad.value_descriptor.uuid,&bad.datatype_descriptor.uuid,&bad.type_uuid,
        &bad.domain.uuid,&bad.default_expression.uuid,&bad.generated_expression.uuid,
        &bad.identity_sequence.uuid,&bad.charset.uuid,&bad.collation.uuid,&bad.storage_profile.uuid};
    const bool supplied=!ids[field]->value.is_nil();
    if(!supplied)continue;
    ids[field]->kind=p::UuidKind::session;
    const auto result=c::EncodeCatalogColumnDefinition(bad);
    Check(!result.ok()&&result.bytes.empty(),"writer silently retyped binary identity");
  }
  for(auto value:std::array<std::uint64_t,4>{0,1,4294967296ULL,UINT64_MAX}) {
    auto t=Table();t.next_column_ordinal=value; const auto bytes=Wire(65700,Fields(t));
    no_heap=true;const auto r=c::DecodeCatalogTableDefinition(bytes);no_heap=false;
    Check(r.ok()==(value!=0&&value<=4294967296ULL),"native high-water limit");
    auto col=Column();col.ordinal=value;const auto b=Wire(65701,Fields(col));
    no_heap=true;const auto cr=c::DecodeCatalogColumnDefinition(b);no_heap=false;
    Check(cr.ok()==(value<=UINT32_MAX),"native stored ordinal limit");
  }
}
c::CatalogMetadataVersion Metadata(bool table) {
  const auto t=Table();const auto col=Column();c::CatalogMetadataVersion m;
  m.record.header.kind=table?c::CatalogRecordKind::table_descriptor:c::CatalogRecordKind::column_descriptor;
  m.record.header.row_uuid=Id(table?100:101,p::UuidKind::row);
  m.record.header.object_uuid=table?t.table_uuid:col.column_uuid;
  m.record.header.parent_uuid=table?Id(80):t.table_uuid;
  m.record.payload=table?Wire(65700,Fields(t)):Wire(65701,Fields(col));
  m.owner_uuid=Id(81,p::UuidKind::principal);m.audit_uuid=Id(82);
  m.default_name_uuid=Id(83);m.name_vector_uuid=Id(84);m.owning_schema_uuid=Id(80,p::UuidKind::schema);
  m.creator_transaction_uuid=t.origin_transaction_uuid;m.creator_local_transaction_id=9;
  m.definition_version=m.schema_epoch=m.security_epoch=m.catalog_generation=m.dependency_generation=m.invalidation_generation=1;
  m.lifecycle=c::CatalogObjectLifecycle::active;m.status=c::CatalogObjectStatus::active;
  m.trace_search_key="NATIVE-TABLE-ORACLE";m.retention_class="catalog_history";
  m.object_subtype=table?"ordinary_persistent_table":"persistent_table_column";
  if(table)m.storage_binding_uuid=t.storage_descriptor.uuid;
  return m;
}
void MetadataAndEvolution() {
  for(bool table:{false,true}) {
    const auto original=Metadata(table);const auto encoded=c::EncodeCatalogMetadataVersion(original);
    Check(encoded.ok(),"shared metadata dispatcher refused native family");
    const auto decoded=c::DecodeCatalogMetadataVersion(encoded.bytes);
    Check(decoded.ok()&&decoded.record.record.payload==original.record.payload,"native envelope lost family bytes");
    for(unsigned mutation=0;mutation!=10;++mutation) {
      auto bad=original;
      if(mutation==0)bad.record.header.object_uuid=Id(90);
      if(mutation==1)bad.record.header.parent_uuid=Id(91);
      if(mutation==2)bad.default_name_uuid={};
      if(mutation==3)bad.name_vector_uuid={};
      if(mutation==4)bad.owning_schema_uuid={};
      if(mutation==5)bad.object_subtype="wrong_family";
      if(mutation==6)bad.creator_transaction_uuid=Id(92,p::UuidKind::transaction);
      if(mutation==7)bad.creator_local_transaction_id=8;
      if(mutation==8)bad.authority_scope=c::CatalogAuthorityScope::temporary;
      if(mutation==9)bad.record.header.kind=c::CatalogRecordKind::sql_object;
      const auto result=c::EncodeCatalogMetadataVersion(bad);
      Check(!result.ok(),"shared metadata dispatcher bypassed family mismatch");
    }
    for(unsigned offset:{0,4,16,20}) {
      auto bad=original;bad.record.payload[offset]^=1;
      Check(!c::EncodeCatalogMetadataVersion(bad).ok(),"damaged schema marker bypassed subtype admission");
    }
    auto next=original;next.definition_version=2;next.creator_local_transaction_id=10;
    next.creator_transaction_uuid=Id(95,p::UuidKind::transaction);
    if(table) { auto d=Table(); d.next_column_ordinal=9;next.record.payload=Wire(65700,Fields(d)); }
    else { auto d=Column(); d.datatype_descriptor={Id(96),2};d.value_descriptor={Id(97),3};d.type_uuid=Id(98);
           next.record.payload=Wire(65701,Fields(d)); }
    Check(c::EncodeCatalogMetadataVersion(next).ok()&&c::CatalogMetadataPreservesFamilyOrigin(original,next),
          "approved definition evolution changed identity or origin");
    for(unsigned mutation=0;mutation!=3;++mutation) {
      auto bad=next;
      if(table) {
        auto d=Table();
        if(mutation==0)d.next_column_ordinal=7;
        if(mutation==1)d.origin_transaction_uuid=Id(99,p::UuidKind::transaction);
        if(mutation==2)d.database_uuid=Id(99,p::UuidKind::database);
        bad.record.payload=Wire(65700,Fields(d));
      } else {
        auto d=Column();
        if(mutation==0)d.ordinal=6;
        if(mutation==1)d.origin_transaction_uuid=Id(99,p::UuidKind::transaction);
        if(mutation==2)d.origin_local_transaction_id=8;
        bad.record.payload=Wire(65701,Fields(d));
      }
      Check(!c::CatalogMetadataPreservesFamilyOrigin(original,bad),"shared origin dispatcher accepted immutable mutation");
    }
    no_heap=true;
    const bool family=table?c::CatalogTableDefinitionMatchesMetadata(c::BorrowCatalogMetadataVersion(original)):
                            c::CatalogColumnDefinitionMatchesMetadata(c::BorrowCatalogMetadataVersion(original));
    no_heap=false;Check(family,"family predicate allocated or rejected valid metadata");
  }
}
void NativeNameIdentities() {
  for(bool table:{false,true}) {
    const auto original=Metadata(table);
    for(unsigned collision=0;collision!=4;++collision) {
      auto bad=original;
      if(collision==0||collision==3)bad.default_name_uuid=bad.record.header.object_uuid;
      if(collision==1||collision==3)bad.name_vector_uuid=bad.record.header.object_uuid;
      if(collision==2)bad.default_name_uuid=bad.name_vector_uuid;
      no_heap=true;
      const bool accepted=table?c::CatalogTableDefinitionMatchesMetadata(c::BorrowCatalogMetadataVersion(bad)):
                                c::CatalogColumnDefinitionMatchesMetadata(c::BorrowCatalogMetadataVersion(bad));
      no_heap=false;
      Check(!accepted,"native named object/vector/entry identities collided");
      no_heap=true;
      const auto diagnostic=c::ValidateCatalogMetadataVersionView(c::BorrowCatalogMetadataVersion(bad));
      no_heap=false;
      Check(diagnostic&&diagnostic->diagnostic_code=="CATALOG.INVALID_INPUT"&&
            diagnostic->message_key=="catalog.metadata_version.invalid"&&
            diagnostic->detail==(table?"table_definition_binding_invalid":"column_definition_binding_invalid"),
            "native name collision diagnostic changed");
      const auto encoded=c::EncodeCatalogMetadataVersion(bad);
      Check(!encoded.ok()&&encoded.bytes.empty(),"shared encoder published colliding native names");
      bad.definition_version=0;
      no_heap=true;const auto earlier=c::ValidateCatalogMetadataVersionView(c::BorrowCatalogMetadataVersion(bad));no_heap=false;
      Check(earlier&&earlier->detail=="counter_zero","native family bypassed common counter precedence");
    }
    auto next=original;next.default_name_uuid=Id(110);next.name_vector_uuid=Id(111);
    next.definition_version=2;next.creator_transaction_uuid=Id(112,p::UuidKind::transaction);
    next.creator_local_transaction_id=10;
    Check(c::EncodeCatalogMetadataVersion(next).ok(),"distinct replacement name identities rejected");
    no_heap=true;const bool preserved=c::CatalogMetadataPreservesFamilyOrigin(original,next);no_heap=false;
    Check(preserved&&next.record.payload==original.record.payload&&
          next.record.header.object_uuid.value==original.record.header.object_uuid.value,
          "name reference evolution changed native object identity or definition");
    // Structural name references alone do not publish a rename or prove lookup.
  }
}
void NativeRetirementRepresentation() {
  for(bool table:{false,true}) {
    const auto original=Metadata(table);auto retired=original;
    retired.record.header.deleted=true;
    retired.lifecycle=c::CatalogObjectLifecycle::dropped;
    retired.status=c::CatalogObjectStatus::retired;
    retired.definition_version=2;retired.creator_local_transaction_id=10;
    retired.creator_transaction_uuid=Id(113,p::UuidKind::transaction);
    retired.retired_transaction_uuid=retired.creator_transaction_uuid;
    const auto encoded=c::EncodeCatalogMetadataVersion(retired);
    Check(encoded.ok(),"native retirement envelope refused");
    const auto decoded=c::DecodeCatalogMetadataVersion(encoded.bytes);
    Check(decoded.ok()&&decoded.record.record.header.deleted&&
          decoded.record.lifecycle==c::CatalogObjectLifecycle::dropped&&
          decoded.record.status==c::CatalogObjectStatus::retired&&
          decoded.record.retired_transaction_uuid.value==retired.creator_transaction_uuid.value&&
          decoded.record.definition_version==2&&decoded.record.record.payload==original.record.payload,
          "native retirement lost retained definition or retirement binding");
    no_heap=true;const bool preserved=c::CatalogMetadataPreservesFamilyOrigin(original,decoded.record);no_heap=false;
    Check(preserved,"native retirement changed immutable origin");
    for(unsigned mutation=0;mutation!=5;++mutation) {
      auto bad=retired;
      if(mutation==0)bad.retired_transaction_uuid={};
      if(mutation==1)bad.retired_transaction_uuid=Id(114,p::UuidKind::transaction);
      if(mutation==2)bad.retired_transaction_uuid.kind=p::UuidKind::object;
      if(mutation==3)bad.lifecycle=c::CatalogObjectLifecycle::active;
      if(mutation==4)bad.status=c::CatalogObjectStatus::active;
      no_heap=true;const auto diagnostic=c::ValidateCatalogMetadataVersionView(c::BorrowCatalogMetadataVersion(bad));no_heap=false;
      Check(diagnostic&&diagnostic->diagnostic_code=="CATALOG.INVALID_INPUT"&&
            diagnostic->detail==(mutation==2?"transaction_reference_kind":"retirement_binding_invalid"),
            "native retirement diagnostic changed");
      const auto result=c::EncodeCatalogMetadataVersion(bad);
      Check(!result.ok()&&result.bytes.empty(),"invalid native retirement published bytes");
    }
    Check(!original.record.header.deleted&&original.definition_version==1&&
          original.retired_transaction_uuid.value.is_nil(),"retirement changed retained predecessor");
    // Representation only: no inventory finality, visibility or reclamation claim.
  }
}
void EveryNativeFieldEvolution() {
  unsigned covered=0;
  for(bool table:{true,false})for(unsigned field=1;field<=(table?12u:27u);++field) {
    auto original=Metadata(table);auto td=Table();auto cd=Column();
    if(table) {td.type_enforcement=c::CatalogTableTypeEnforcement::coercing;td.coercion_profile={Id(31),2};}
    else {
      cd.domain={Id(32),2};cd.charset={Id(33),3};cd.collation={Id(34),4};cd.storage_profile={Id(35),5};
      if(field==14||field==15)cd.default_expression={Id(36),6};
      if(field>=16&&field<=18){cd.generated_expression={Id(37),7};cd.generated_storage=c::CatalogGeneratedColumnStorage::stored;}
      if(field>=19&&field<=21){cd.identity_sequence={Id(38),8};cd.identity_mode=c::CatalogIdentityColumnMode::always;}
    }
    auto fields=table?Fields(td):Fields(cd);const unsigned schema=table?65700:65701;
    original.record.payload=Wire(schema,fields);const auto original_bytes=original.record.payload;
    auto next=original;next.definition_version=2;next.catalog_generation=2;
    next.creator_transaction_uuid=Id(95,p::UuidKind::transaction);next.creator_local_transaction_id=100;
    auto selected=std::find_if(fields.begin(),fields.end(),[&](const auto& f){return f.id==field;});
    Check(selected!=fields.end(),"field mutation coverage omitted a registered field");
    if(selected->tag==5) {
      const auto kind=(table&&field==2)?p::UuidKind::database:
          (field==3||(table&&field==10))?p::UuidKind::transaction:p::UuidKind::object;
      const auto id=Id(8000+field,kind);selected->bytes=Identity(id);
      if(field==1)next.record.header.object_uuid=id;
      if(!table&&field==2)next.record.header.parent_uuid=id;
      if(table&&field==6)next.storage_binding_uuid=id;
    } else if(selected->tag==2)selected->bytes=std::string(1,1);
    else {
      std::uint64_t value=48;
      if(field==4)value=8;
      if(field==5)value=table?9:6;
      if((table&&field==8)||(!table&&(field==18||field==21)))value=1;
      selected->bytes=Number(value);
    }
    // Enforcement is a coupled profile: STRICT removes both coercion fields.
    if(table&&field==8)fields.erase(std::remove_if(fields.begin(),fields.end(),
        [](const auto& f){return f.id==11||f.id==12;}),fields.end());
    next.record.payload=Wire(schema,fields);
    Check(c::EncodeCatalogMetadataVersion(original).ok()&&c::EncodeCatalogMetadataVersion(next).ok(),
          "field-change fixture is not two structurally valid versions");
    const auto before=c::BorrowCatalogMetadataVersion(original),after=c::BorrowCatalogMetadataVersion(next);
    no_heap=true;
    const bool preserves=table?c::CatalogTableDefinitionPreservesOrigin(before,after):
                               c::CatalogColumnDefinitionPreservesOrigin(before,after);
    no_heap=false;
    Check(preserves==(field>(table?4u:5u)),"field-level immutable/mutable distinction differs");
    if(table) {
      const auto decoded=c::DecodeCatalogTableDefinition(next.record.payload);
      Check(decoded.ok()&&Wire(schema,Fields(*decoded.definition))==next.record.payload,
            "table field evolution lost a stored value");
    } else {
      const auto decoded=c::DecodeCatalogColumnDefinition(next.record.payload);
      Check(decoded.ok()&&Wire(schema,Fields(*decoded.definition))==next.record.payload,
            "column field evolution lost a stored value");
    }
    Check(original.record.payload==original_bytes,"field comparison changed predecessor bytes");
    ++covered;
  }
  Check(covered==39,"native mutation field inventory incomplete");
  // This tests representation and immutable origin, not mutation admission:
  // mutable fields still need actual policy/dependency/storage/version checks.
}
void DefinitionDependencies() {
  // Independent field-order oracle, including absent optional pairs. The
  // expected list never uses the production schema or extractor to construct it.
  std::vector<std::pair<unsigned,c::CatalogMetadataVersion>> profiles;
  for (unsigned mask=0;mask<128;++mask) {
    auto d=Column();
    std::array<c::CatalogDefinitionReference*,7> optional{&d.domain,&d.default_expression,
        &d.generated_expression,&d.identity_sequence,&d.charset,&d.collation,&d.storage_profile};
    for(unsigned bit=0;bit<7;++bit)if(mask&(1u<<bit))*optional[bit]={Id(30+bit),20+bit};
    if(mask&4)d.generated_storage=c::CatalogGeneratedColumnStorage::stored;
    if(mask&8)d.identity_mode=c::CatalogIdentityColumnMode::always;
    const unsigned sources=((mask>>1)&1)+((mask>>2)&1)+((mask>>3)&1);
    const bool valid=sources<=1&&(!(mask&32)||(mask&16));
    auto m=Metadata(false);m.record.payload=Wire(65701,Fields(d));
    no_heap=true;
    const auto result=c::CatalogColumnDefinitionDependencies(c::BorrowCatalogMetadataVersion(m));
    no_heap=false;
    Check(result.ok()==valid,"dependency extraction validity differs from independent mask model");
    if(!valid) {
      Check(result.count==0&&result.source_uuid.value.is_nil()&&result.schema_id==0&&
            result.source_definition_version==0,"invalid dependency result exposed prefix");
      continue;
    }
    profiles.emplace_back(mask,m);
    std::vector<c::CatalogDefinitionDependency> expected{{6,d.value_descriptor},{8,d.datatype_descriptor}};
    constexpr unsigned fields[]{12,14,16,19,22,24,26};
    for(unsigned bit=0;bit<7;++bit)if(mask&(1u<<bit))expected.push_back({static_cast<p::u16>(fields[bit]),*optional[bit]});
    Check(result.schema_id==65701&&result.source_uuid.value==d.column_uuid.value&&
          result.source_definition_version==1&&result.count==expected.size(),"dependency source or count lost");
    for(std::size_t i=0;i<expected.size();++i) {
      const auto& a=result.references()[i];const auto& b=expected[i];
      Check(a.field_id==b.field_id&&a.target.uuid.kind==b.target.uuid.kind&&
            a.target.uuid.value==b.target.uuid.value&&a.target.generation==b.target.generation,
            "exact dependency field identity or generation lost");
    }
    auto after=m;after.definition_version=2;after.creator_local_transaction_id=10;
    after.creator_transaction_uuid=Id(95,p::UuidKind::transaction);
    std::array<c::CatalogDefinitionReference*,9> refs{&d.value_descriptor,&d.datatype_descriptor,&d.domain,
        &d.default_expression,&d.generated_expression,&d.identity_sequence,&d.charset,&d.collation,&d.storage_profile};
    for(auto* ref:refs)if(ref->generation) {
      const auto old=*ref;
      for(unsigned change=0;change<2;++change) {
        *ref=old;if(change==0)++ref->generation;else ref->uuid=Id(220);
        after.record.payload=Wire(65701,Fields(d));
        no_heap=true;const auto diff=c::CompareCatalogDefinitionDependencies(
            c::BorrowCatalogMetadataVersion(m),c::BorrowCatalogMetadataVersion(after));no_heap=false;
        Check(diff.ok()&&diff.removed_count==1&&diff.added_count==1,"changed exact reference not isolated");
        const auto& removed=diff.predecessor.entries[diff.removed_from_successor[0]];
        const auto& added=diff.successor.entries[diff.added_in_successor[0]];
        Check(removed.field_id==added.field_id&&removed.target.uuid.value==old.uuid.value&&
              removed.target.generation==old.generation&&added.target.uuid.value==ref->uuid.value&&
              added.target.generation==ref->generation,"dependency comparison lost old or new binding");
      }
      *ref=old;
    }
    d.nullable=!d.nullable;after.record.payload=Wire(65701,Fields(d));
    no_heap=true;const auto unchanged=c::CompareCatalogDefinitionDependencies(
        c::BorrowCatalogMetadataVersion(m),c::BorrowCatalogMetadataVersion(after));no_heap=false;
    Check(unchanged.ok()&&unchanged.removed_count==0&&unchanged.added_count==0,
          "definition-only change invented dependency mutations");
    // Extracted dependencies own their binary values, not input string views.
    m.record.payload.clear();
    Check(result.entries[0].target.uuid.value==d.value_descriptor.uuid.value,"dependency output borrowed destroyed payload");
  }
  Check(profiles.size()==48,"independent optional dependency profile count");
  // All 48 x 48 legal optional-reference transitions, including removal of a
  // charset together with its collation and changes between expression roles.
  constexpr unsigned optional_fields[]{12,14,16,19,22,24,26};
  for(const auto& [old_mask,old_metadata]:profiles)for(const auto& [new_mask,new_metadata]:profiles) {
    no_heap=true;const auto diff=c::CompareCatalogDefinitionDependencies(
        c::BorrowCatalogMetadataVersion(old_metadata),c::BorrowCatalogMetadataVersion(new_metadata));no_heap=false;
    Check(diff.ok(),"valid optional dependency transition refused");
    std::size_t removed=0,added=0;
    for(unsigned bit=0;bit<7;++bit) {
      if((old_mask&(1u<<bit))&&!(new_mask&(1u<<bit))) {
        Check(removed<diff.removed_count&&diff.predecessor.entries[diff.removed_from_successor[removed]].field_id==optional_fields[bit],
              "optional removal set/order mismatch");++removed;
      }
      if((new_mask&(1u<<bit))&&!(old_mask&(1u<<bit))) {
        Check(added<diff.added_count&&diff.successor.entries[diff.added_in_successor[added]].field_id==optional_fields[bit],
              "optional addition set/order mismatch");++added;
      }
    }
    Check(diff.removed_count==removed&&diff.added_count==added,"optional transition invented extra mutations");
  }
  auto a=Metadata(false),b=a;auto d=Column();
  d.default_expression={Id(30),7};a.record.payload=Wire(65701,Fields(d));
  d.default_expression={};d.generated_expression={Id(30),7};d.generated_storage=c::CatalogGeneratedColumnStorage::virtual_value;
  b.record.payload=Wire(65701,Fields(d));b.definition_version=2;
  no_heap=true;const auto role=c::CompareCatalogDefinitionDependencies(
      c::BorrowCatalogMetadataVersion(a),c::BorrowCatalogMetadataVersion(b));no_heap=false;
  Check(role.ok()&&role.removed_count==1&&role.added_count==1&&
        role.predecessor.entries[role.removed_from_successor[0]].field_id==14&&
        role.successor.entries[role.added_in_successor[0]].field_id==16,
        "same target under different dependency role incorrectly retained");
  d=Column();d.domain=d.charset={Id(31),8};b.record.payload=Wire(65701,Fields(d));
  no_heap=true;const auto shared=c::CatalogColumnDefinitionDependencies(c::BorrowCatalogMetadataVersion(b));no_heap=false;
  Check(shared.ok()&&shared.count==4&&shared.entries[2].field_id==12&&shared.entries[3].field_id==22,
        "shared target collapsed distinct field references");
  for(unsigned bad=0;bad<6;++bad) {
    auto invalid=b;
    if(bad==0)invalid.record.payload.resize(20);
    if(bad==1)invalid.record.header.object_uuid=Id(230);
    if(bad==2)invalid.object_subtype="generic";
    if(bad==3)invalid.owning_schema_uuid={};
    if(bad==4){auto changed=d;changed.ordinal++;invalid.record.payload=Wire(65701,Fields(changed));}
    if(bad==5){auto changed=d;changed.origin_local_transaction_id=8;invalid.record.payload=Wire(65701,Fields(changed));}
    no_heap=true;const auto refused=c::CompareCatalogDefinitionDependencies(
        c::BorrowCatalogMetadataVersion(b),c::BorrowCatalogMetadataVersion(invalid));no_heap=false;
    Check(!refused.ok()&&refused.removed_count==0&&refused.added_count==0&&
          refused.predecessor.count==0&&refused.successor.count==0,"invalid comparison leaked partial reference list");
  }
  auto table=Metadata(true);auto t=Table();
  for(bool coercing:{false,true}) {
    if(coercing){t.type_enforcement=c::CatalogTableTypeEnforcement::coercing;t.coercion_profile={Id(60),4};}
    table.record.payload=Wire(65700,Fields(t));
    no_heap=true;const auto result=c::CatalogTableDefinitionDependencies(c::BorrowCatalogMetadataVersion(table));no_heap=false;
    Check(result.ok()&&result.schema_id==65700&&result.count==(coercing?2:1)&&
          result.entries[0].field_id==6&&result.entries[0].target.uuid.value==t.storage_descriptor.uuid.value,
          "table storage/coercion dependency extraction");
    if(coercing)Check(result.entries[1].field_id==11&&result.entries[1].target.generation==4,"coercion generation lost");
  }
  auto strict=Metadata(true);
  no_heap=true;const auto added=c::CompareCatalogDefinitionDependencies(
      c::BorrowCatalogMetadataVersion(strict),c::BorrowCatalogMetadataVersion(table));
  const auto removed=c::CompareCatalogDefinitionDependencies(
      c::BorrowCatalogMetadataVersion(table),c::BorrowCatalogMetadataVersion(strict));
  const auto wrong_family=c::CompareCatalogDefinitionDependencies(
      c::BorrowCatalogMetadataVersion(strict),c::BorrowCatalogMetadataVersion(b));no_heap=false;
  Check(added.ok()&&added.added_count==1&&added.removed_count==0&&
        removed.ok()&&removed.removed_count==1&&removed.added_count==0,"optional reference addition/removal lost");
  Check(!wrong_family.ok()&&wrong_family.predecessor.count==0,"cross-family dependency comparison admitted");
  auto maximum=Metadata(false);auto limit=Column();limit.value_descriptor.generation=UINT64_MAX;
  maximum.definition_version=UINT64_MAX;maximum.record.payload=Wire(65701,Fields(limit));
  no_heap=true;const auto exact=c::CatalogColumnDefinitionDependencies(c::BorrowCatalogMetadataVersion(maximum));no_heap=false;
  Check(exact.ok()&&exact.source_definition_version==UINT64_MAX&&exact.entries[0].target.generation==UINT64_MAX,
        "maximum generation was narrowed or replaced");
}
void AllocationFailures() {
  auto table=Table();auto column=Column();
  table.type_enforcement=c::CatalogTableTypeEnforcement::coercing;table.coercion_profile={Id(9),3};
  column.domain={Id(30),1};column.default_expression={Id(31),2};column.charset={Id(32),3};
  column.collation={Id(33),4};column.storage_profile={Id(34),5};
  for(bool is_table:{false,true}) {
    auto encode=[&] { return is_table?c::EncodeCatalogTableDefinition(table):c::EncodeCatalogColumnDefinition(column); };
    (void)encode(); allocations=0; (void)encode(); const auto sites=allocations;
    Check(sites>0,"encoder fault coverage empty");
    for(std::size_t n=0;n<sites;++n) {
      bool threw=false;fail_after=static_cast<long>(n);
      try {(void)encode();}catch(const std::bad_alloc&){threw=true;}
      fail_after=-1;Check(threw,"encoding allocation failure swallowed");++faults;
    }
    Check(Bytes(encode())==(is_table?Wire(65700,Fields(table)):Wire(65701,Fields(column))),
          "failed encode damaged retained source or subsequent call");
  }
}
void NativeCohorts() {
  auto table=Metadata(true);auto t=Table();t.next_column_ordinal=2;table.record.payload=Wire(65700,Fields(t));
  std::array<c::CatalogMetadataVersion,2> columns{Metadata(false),Metadata(false)};
  for(unsigned i=0;i!=2;++i) {
    auto d=Column();d.column_uuid=Id(10+i);d.ordinal=i;d.value_descriptor={Id(20+i),1};
    columns[i].record.header.object_uuid=d.column_uuid;
    columns[i].record.header.row_uuid=Id(100+i,p::UuidKind::row);
    columns[i].default_name_uuid=Id(110+i);columns[i].name_vector_uuid=Id(120+i);
    columns[i].record.payload=Wire(65701,Fields(d));
  }
  std::array views{c::BorrowCatalogMetadataVersion(columns[0]),c::BorrowCatalogMetadataVersion(columns[1])};
  const auto tv=c::BorrowCatalogMetadataVersion(table);
  Check(c::CatalogTableColumnCohortMatches(tv,views,true),"fresh native cohort refused");
  std::reverse(views.begin(),views.end());
  Check(!c::CatalogTableColumnCohortMatches(tv,views,true)&&c::CatalogTableColumnCohortMatches(tv,views,false),
        "fresh order confused with existing stored ordinals");
  std::reverse(views.begin(),views.end());
  Check(!c::CatalogTableColumnCohortMatches(tv,{},false),"empty cohort admitted");
  auto duplicate=views;duplicate[1]=duplicate[0];
  Check(!c::CatalogTableColumnCohortMatches(tv,duplicate,false),"duplicate native column admitted");
  auto wrong=views;wrong[1].owning_schema_uuid=Id(77,p::UuidKind::schema);
  Check(!c::CatalogTableColumnCohortMatches(tv,wrong,false),"cross-schema column admitted");
  auto next_table=table;next_table.definition_version=2;
  next_table.creator_transaction_uuid=Id(130,p::UuidKind::transaction);next_table.creator_local_transaction_id=10;
  const std::array survivor{views[0]};
  Check(c::CatalogTableColumnCohortPreservesHistory(tv,views,c::BorrowCatalogMetadataVersion(next_table),survivor),
        "drop highest column changed native allocation history");
  auto added=columns[1];auto d=Column();d.column_uuid=Id(140);d.value_descriptor={Id(141),1};
  d.ordinal=1;added.record.header.object_uuid=d.column_uuid;added.record.payload=Wire(65701,Fields(d));
  std::array next_columns{views[0],c::BorrowCatalogMetadataVersion(added)};
  Check(!c::CatalogTableColumnCohortPreservesHistory(tv,views,c::BorrowCatalogMetadataVersion(next_table),next_columns),
        "replacement identity reused dropped ordinal");
  d.ordinal=2;added.record.payload=Wire(65701,Fields(d));next_columns[1]=c::BorrowCatalogMetadataVersion(added);
  t.next_column_ordinal=3;next_table.record.payload=Wire(65700,Fields(t));
  Check(c::CatalogTableColumnCohortPreservesHistory(tv,views,c::BorrowCatalogMetadataVersion(next_table),next_columns),
        "new ordinal above lifetime high-water refused");
  const auto before_table=table.record.payload,before_added=added.record.payload;
  const auto next_view=c::BorrowCatalogMetadataVersion(next_table);
  allocations=0;(void)c::CatalogTableColumnCohortPreservesHistory(tv,views,next_view,next_columns);
  const auto sites=allocations;Check(sites>0,"native cohort allocation coverage empty");
  for(std::size_t n=0;n!=sites;++n) {
    bool threw=false;fail_after=static_cast<long>(n);
    try {(void)c::CatalogTableColumnCohortPreservesHistory(tv,views,next_view,next_columns);}
    catch(const std::bad_alloc&){threw=true;}
    fail_after=-1;Check(threw&&table.record.payload==before_table&&added.record.payload==before_added,
                        "native cohort failure changed source or returned success");++faults;
  }
}
void SelectedCohortDecode() {
  using E=c::CatalogTableCohortDecodeError;
  const auto unchanged=[](const auto& out,const auto& baseline) {
    for(std::size_t i=0;i<out.size();++i)
      if(Wire(65701,Fields(out[i]))!=Wire(65701,Fields(baseline[i])))return false;
    return true;
  };
  for(unsigned n=1;n<=8;++n) {
    auto table=Metadata(true);auto t=Table();t.next_column_ordinal=UINT64_C(4294967296);
    table.record.payload=Wire(65700,Fields(t));
    std::vector<c::CatalogMetadataVersion> columns(n,Metadata(false));
    std::vector<c::CatalogColumnDefinition> expected;
    for(unsigned i=0;i<n;++i) {
      auto d=Column();d.column_uuid=Id(400+i);d.value_descriptor={Id(500+i),7+i};
      d.ordinal=UINT32_MAX-2*i;d.nullable=i%2;
      columns[i].record.header.object_uuid=d.column_uuid;columns[i].record.header.row_uuid=Id(600+i,p::UuidKind::row);
      columns[i].record.payload=Wire(65701,Fields(d));expected.push_back(d);
    }
    std::vector<c::CatalogMetadataVersionView> views;
    for(const auto& m:columns)views.push_back(c::BorrowCatalogMetadataVersion(m));
    std::vector<c::CatalogColumnDefinition> output(n+1,Column());
    for(auto& d:output){d.ordinal=123;d.nullable=true;}
    const auto sentinel=output;
    for(unsigned rotation=0;rotation<n;++rotation) {
      output=sentinel;
      const auto result=c::DecodeCatalogTableColumnCohort(c::BorrowCatalogMetadataVersion(table),views,output);
      Check(result.ok()&&result.columns_written==n&&Wire(65700,Fields(*result.table))==table.record.payload,
            "selected cohort table or count lost");
      for(unsigned i=0;i<n;++i)Check(Wire(65701,Fields(output[i]))==Wire(65701,Fields(expected[(i+rotation)%n])),
          "selected cohort reordered columns or changed sparse ordinal/binding");
      Check(Wire(65701,Fields(output[n]))==Wire(65701,Fields(sentinel[n])),"unused destination slot overwritten");
      std::rotate(views.begin(),views.begin()+1,views.end());
    }
    for(unsigned capacity=0;capacity<n;++capacity) {
      output=sentinel;no_heap=true;
      const auto result=c::DecodeCatalogTableColumnCohort(c::BorrowCatalogMetadataVersion(table),views,
          std::span<c::CatalogColumnDefinition>(output).first(capacity));no_heap=false;
      Check(result.error==E::insufficient_output&&!result.table&&result.columns_written==0&&unchanged(output,sentinel),
            "short output allocated or wrote a partial prefix");
    }
    for(unsigned position=0;position<n;++position)for(unsigned defect=0;defect<4;++defect) {
      auto bad=columns;auto d=expected[position];
      if(defect==0)bad[position].record.payload.resize(20);
      if(defect==1)bad[position].owning_schema_uuid=Id(700,p::UuidKind::schema);
      if(defect==2){d.ordinal=UINT64_C(4294967296);bad[position].record.payload=Wire(65701,Fields(d));}
      if(defect==3){d.table_uuid=Id(701);bad[position].record.header.parent_uuid=d.table_uuid;bad[position].record.payload=Wire(65701,Fields(d));}
      auto selected=views;for(unsigned i=0;i<n;++i)selected[i]=c::BorrowCatalogMetadataVersion(bad[i]);
      output=sentinel;const auto result=c::DecodeCatalogTableColumnCohort(c::BorrowCatalogMetadataVersion(table),selected,output);
      Check(result.error==E::invalid_columns&&!result.table&&result.columns_written==0&&unchanged(output,sentinel),
            "late invalid column produced partial output");
    }
    output=sentinel;allocations=0;
    const auto measured=c::DecodeCatalogTableColumnCohort(c::BorrowCatalogMetadataVersion(table),views,output);
    const auto sites=allocations;Check(measured.ok()&&sites>0,"cohort scratch allocation coverage absent");
    for(std::size_t site=0;site<sites;++site) {
      output=sentinel;bool threw=false;fail_after=static_cast<long>(site);
      try{(void)c::DecodeCatalogTableColumnCohort(c::BorrowCatalogMetadataVersion(table),views,output);}
      catch(const std::bad_alloc&){threw=true;}fail_after=-1;
      Check(threw&&unchanged(output,sentinel),"cohort allocation failure wrote output or was swallowed");++faults;
    }
    output=sentinel;auto invalid_table=table;invalid_table.record.payload.resize(20);
    no_heap=true;const auto invalid=c::DecodeCatalogTableColumnCohort(c::BorrowCatalogMetadataVersion(invalid_table),views,{});
    const auto empty=c::DecodeCatalogTableColumnCohort(c::BorrowCatalogMetadataVersion(table),{},output);no_heap=false;
    Check(invalid.error==E::invalid_table&&!invalid.table&&invalid.columns_written==0,
          "invalid table did not precede output capacity");
    Check(empty.error==E::invalid_columns&&!empty.table&&unchanged(output,sentinel),"empty selected cohort accepted");
    const auto retained=c::DecodeCatalogTableColumnCohort(c::BorrowCatalogMetadataVersion(table),views,output);
    table.record.payload.clear();for(auto& m:columns)m.record.payload.clear();
    Check(retained.ok()&&retained.table->next_column_ordinal==UINT64_C(4294967296)&&
          output[0].column_uuid.value==expected[0].column_uuid.value,
          "decoded cohort borrowed destroyed source payload");
  }
}
void PreallocatedCohortDecode() {
  using E=c::CatalogTableCohortDecodeError;
  const auto unchanged=[](const auto& a,const auto& b) {
    for(std::size_t i=0;i<a.size();++i)
      if(Wire(65701,Fields(a[i]))!=Wire(65701,Fields(b[i])))return false;
    return true;
  };
  for(unsigned n: {1u,2u,3u,4u,5u,6u,7u,8u,64u,257u}) {
    auto table=Metadata(true);auto t=Table();t.next_column_ordinal=UINT64_C(4294967296);
    table.record.payload=Wire(65700,Fields(t));
    std::vector<c::CatalogMetadataVersion> columns(n,Metadata(false));
    std::vector<c::CatalogColumnDefinition> expected;
    for(unsigned i=0;i<n;++i) {
      auto d=Column();d.column_uuid=Id(1000+i);d.value_descriptor={Id(2000+i),7+i};
      d.ordinal=UINT32_MAX-2*i;d.nullable=i%2;
      columns[i].record.header.object_uuid=d.column_uuid;columns[i].record.header.row_uuid=Id(3000+i,p::UuidKind::row);
      columns[i].record.payload=Wire(65701,Fields(d));expected.push_back(d);
    }
    std::vector<c::CatalogMetadataVersionView> views;
    for(const auto& m:columns)views.push_back(c::BorrowCatalogMetadataVersion(m));
    std::vector<c::CatalogColumnDefinition> output(n+1,Column());const auto sentinel=output;
    std::vector<c::CatalogColumnCohortScratch> scratch(n+1,{Id(4000).value,777});
    const auto tv=c::BorrowCatalogMetadataVersion(table);
    for(unsigned rotation=0;rotation<std::min(n,8u);++rotation) {
      output=sentinel;no_heap=true;
      const auto result=c::DecodeCatalogTableColumnCohort(tv,views,output,scratch);
      no_heap=false;
      Check(result.ok()&&result.columns_written==n&&Wire(65700,Fields(*result.table))==table.record.payload,
            "preallocated cohort failed or allocated");
      for(unsigned i=0;i<n;++i)Check(Wire(65701,Fields(output[i]))==Wire(65701,Fields(expected[(i+rotation)%n])),
            "scratch sorting reordered semantic output");
      Check(Wire(65701,Fields(output[n]))==Wire(65701,Fields(sentinel[n]))&&
            scratch[n].column_uuid==Id(4000).value&&scratch[n].ordinal==777,"bounded tail overwritten");
      std::rotate(views.begin(),views.begin()+1,views.end());
    }
    for(unsigned i=0;i<n;++i)views[i]=c::BorrowCatalogMetadataVersion(columns[i]);
    for(unsigned capacity=0;capacity<n;++capacity) {
      output=sentinel;no_heap=true;
      const auto short_scratch=c::DecodeCatalogTableColumnCohort(tv,views,output,
          std::span<c::CatalogColumnCohortScratch>(scratch).first(capacity));
      const auto short_output=c::DecodeCatalogTableColumnCohort(tv,views,
          std::span<c::CatalogColumnDefinition>(output).first(capacity),{});
      no_heap=false;
      Check(short_scratch.error==E::insufficient_scratch&&!short_scratch.table&&!short_scratch.columns_written&&
            short_output.error==E::insufficient_output&&!short_output.table&&!short_output.columns_written&&
            unchanged(output,sentinel),"capacity refusal precedence or output atomicity");
    }
    for(unsigned position=0;position<n;++position)for(unsigned defect=0;defect<6;++defect) {
      if(n>8&&position!=0&&position!=n/2&&position!=n-1)continue;
      if(n==1&&defect>=4)continue;
      auto bad=columns;auto d=expected[position];
      if(defect==0)bad[position].record.payload.resize(20);
      if(defect==1)bad[position].owning_schema_uuid=Id(5000,p::UuidKind::schema);
      if(defect==2)d.ordinal=UINT64_C(4294967296);
      if(defect==3){d.table_uuid=Id(5001);bad[position].record.header.parent_uuid=d.table_uuid;}
      if(defect==4){d.column_uuid=expected[(position+1)%n].column_uuid;bad[position].record.header.object_uuid=d.column_uuid;}
      if(defect==5)d.ordinal=expected[(position+1)%n].ordinal;
      if(defect>=2)bad[position].record.payload=Wire(65701,Fields(d));
      auto selected=views;for(unsigned i=0;i<n;++i)selected[i]=c::BorrowCatalogMetadataVersion(bad[i]);
      output=sentinel;no_heap=true;
      const auto result=c::DecodeCatalogTableColumnCohort(tv,selected,output,scratch);no_heap=false;
      Check(result.error==E::invalid_columns&&!result.table&&!result.columns_written&&unchanged(output,sentinel),
            "malformed or duplicate bounded cohort emitted prefix");
    }
    auto invalid=table;invalid.record.payload.resize(20);output=sentinel;no_heap=true;
    const auto bad_table=c::DecodeCatalogTableColumnCohort(c::BorrowCatalogMetadataVersion(invalid),views,{},{});
    const auto empty=c::DecodeCatalogTableColumnCohort(tv,{},output,{});no_heap=false;
    Check(bad_table.error==E::invalid_table&&!bad_table.table&&!bad_table.columns_written&&
          empty.error==E::invalid_columns&&!empty.table&&!empty.columns_written&&unchanged(output,sentinel),
          "bounded invalid-table/empty precedence");
    // Fresh creation uses the same predicate but has stronger ordering/origin checks.
    t.next_column_ordinal=n;table.record.payload=Wire(65700,Fields(t));
    for(unsigned i=0;i<n;++i){auto d=expected[i];d.ordinal=i;columns[i].record.payload=Wire(65701,Fields(d));
      views[i]=c::BorrowCatalogMetadataVersion(columns[i]);}
    no_heap=true;
    const bool fresh=c::CatalogTableColumnCohortMatchesWithScratch(c::BorrowCatalogMetadataVersion(table),views,true,scratch);
    std::reverse(views.begin(),views.end());
    const bool reversed=c::CatalogTableColumnCohortMatchesWithScratch(c::BorrowCatalogMetadataVersion(table),views,true,scratch);
    const bool existing=c::CatalogTableColumnCohortMatchesWithScratch(c::BorrowCatalogMetadataVersion(table),views,false,scratch);
    no_heap=false;Check(fresh&&reversed==(n==1)&&existing,"bounded fresh/existing cohort semantics diverged");
    std::vector<c::CatalogColumnCohortScratch> next_scratch(n+2,{Id(4001).value,778});
    std::vector<std::size_t> order(n+1,n+99);
    const auto old_table=c::BorrowCatalogMetadataVersion(table);
    for(unsigned mode=0;mode!=13;++mode) {
      if(mode==8&&n==1)continue;
      auto next_table=table;auto next_definition=t;
      next_table.definition_version=2;next_table.catalog_generation=2;
      next_table.creator_transaction_uuid=Id(130,p::UuidKind::transaction);next_table.creator_local_transaction_id=10;
      auto after=columns;
      if(mode==1&&n>1)after.erase(after.begin());
      if(mode==3)after.erase(after.begin());
      if(mode==12)after.erase(after.begin(),after.end()-1);
      if(mode==2||mode==3||mode==12) {
        auto added=Metadata(false);auto d=Column();d.column_uuid=Id(6000);d.value_descriptor={Id(6001),1};
        d.ordinal=mode==3?0:n;d.origin_transaction_uuid=Id(130,p::UuidKind::transaction);d.origin_local_transaction_id=10;
        added.creator_transaction_uuid=d.origin_transaction_uuid;added.creator_local_transaction_id=10;
        added.record.header.object_uuid=d.column_uuid;added.record.header.row_uuid=Id(6002,p::UuidKind::row);
        added.record.payload=Wire(65701,Fields(d));after.push_back(std::move(added));
        if(mode!=3)next_definition.next_column_ordinal=n+1;
      }
      if(mode==4||mode==5||mode==6) {
        auto d=*c::DecodeCatalogColumnDefinition(after[0].record.payload).definition;
        if(mode==4){d.ordinal=n;next_definition.next_column_ordinal=n+1;}
        if(mode==5){d.value_descriptor={Id(7000),2};d.datatype_descriptor={Id(7001),3};d.type_uuid=Id(7002);}
        if(mode==6)d.origin_transaction_uuid=Id(7003,p::UuidKind::transaction);
        after[0].definition_version=2;after[0].catalog_generation=2;
        after[0].creator_transaction_uuid=Id(130,p::UuidKind::transaction);after[0].creator_local_transaction_id=10;
        after[0].record.payload=Wire(65701,Fields(d));
      }
      if(mode==7)next_definition.origin_transaction_uuid=Id(7004,p::UuidKind::transaction);
      if(mode==8){after.pop_back();next_definition.next_column_ordinal=n-1;}
      if(mode==9)std::reverse(after.begin(),after.end());
      if(mode==10)after[0].owning_schema_uuid=Id(7005,p::UuidKind::schema);
      if(mode==11)next_definition.database_uuid=Id(7006,p::UuidKind::database);
      next_table.record.payload=Wire(65700,Fields(next_definition));
      std::vector<c::CatalogMetadataVersionView> next_views;
      for(const auto& m:after)next_views.push_back(c::BorrowCatalogMetadataVersion(m));
      const bool expected_history=mode!=3&&mode!=4&&mode!=6&&mode!=7&&mode!=8&&mode!=10&&mode!=11;
      const auto next=c::BorrowCatalogMetadataVersion(next_table);
      Check(c::CatalogTableColumnCohortPreservesHistory(old_table,views,next,next_views)==expected_history,
            "independent history case disagrees with existing validation");
      no_heap=true;
      const bool history=c::CatalogTableColumnCohortPreservesHistoryWithScratch(
          old_table,views,next,next_views,{scratch,next_scratch,order});
      no_heap=false;
      Check(history==expected_history,"bounded history allocated or changed identity/high-water semantics");
      if(next_views.size()>n) {
        no_heap=true;
        const bool short_grown=c::CatalogTableColumnCohortPreservesHistoryWithScratch(old_table,views,next,next_views,
            {scratch,std::span<c::CatalogColumnCohortScratch>(next_scratch).first(n),order});
        no_heap=false;Check(!short_grown,"grown successor accepted one-slot-short workspace");
      }
      Check(order[n]==n+99&&scratch[n].column_uuid==Id(4000).value&&scratch[n].ordinal==777&&
            next_scratch[n+1].column_uuid==Id(4001).value&&next_scratch[n+1].ordinal==778,
            "bounded history wrote an unused workspace tail");
    }
    for(unsigned capacity=0;capacity<n;++capacity) {
      no_heap=true;
      const bool old_short=c::CatalogTableColumnCohortPreservesHistoryWithScratch(old_table,views,old_table,views,
          {std::span<c::CatalogColumnCohortScratch>(scratch).first(capacity),next_scratch,order});
      const bool new_short=c::CatalogTableColumnCohortPreservesHistoryWithScratch(old_table,views,old_table,views,
          {scratch,std::span<c::CatalogColumnCohortScratch>(next_scratch).first(capacity),order});
      const bool index_short=c::CatalogTableColumnCohortPreservesHistoryWithScratch(old_table,views,old_table,views,
          {scratch,next_scratch,std::span<std::size_t>(order).first(capacity)});
      no_heap=false;Check(!old_short&&!new_short&&!index_short,"short history workspace admitted or allocated");
    }
    auto duplicate_old=views;if(n>1)duplicate_old[1]=duplicate_old[0];
    const auto invalid_view=c::BorrowCatalogMetadataVersion(invalid);
    no_heap=true;
    const bool invalid_old=c::CatalogTableColumnCohortPreservesHistoryWithScratch(invalid_view,views,old_table,views,
        {scratch,next_scratch,order});
    const bool invalid_new=c::CatalogTableColumnCohortPreservesHistoryWithScratch(old_table,views,invalid_view,views,
        {scratch,next_scratch,order});
    const bool empty_old=c::CatalogTableColumnCohortPreservesHistoryWithScratch(old_table,{},old_table,views,
        {scratch,next_scratch,order});
    const bool empty_new=c::CatalogTableColumnCohortPreservesHistoryWithScratch(old_table,views,old_table,{},
        {scratch,next_scratch,order});
    const bool duplicate=n>1&&c::CatalogTableColumnCohortPreservesHistoryWithScratch(old_table,duplicate_old,old_table,views,
        {scratch,next_scratch,order});
    no_heap=false;Check(!invalid_old&&!invalid_new&&!empty_old&&!empty_new&&!duplicate,
                       "invalid history predecessor/successor allocated or was accepted");
  }
}
void NativeImagePersistence() {
  namespace db=scratchbird::storage::database;
  namespace disk=scratchbird::storage::disk;
  namespace page=scratchbird::storage::page;
  namespace fs=std::filesystem;
  const auto nonce=std::chrono::steady_clock::now().time_since_epoch().count();
  fs::path root;
  bool owned=false;
  for(unsigned attempt=0;attempt!=100&&!owned;++attempt) {
    root=fs::temp_directory_path()/("sb_native_table_family_"+std::to_string(nonce)+"_"+std::to_string(attempt));
    owned=fs::create_directory(root);
  }
  Check(owned,"could not own unique native-image fixture");
  // This is a native-image persistence fixture, not an initialized database,
  // catalog-root publication, visibility receipt or replacement storage API.
  const std::array<unsigned,5> sizes{8192,16384,32768,65536,131072};
  const std::array<std::array<p::byte,3>,5> tails{{
      {0,0x81,0x92},{1,0x63,0x84},{3,0x27,0x68},{6,0x55,0x36},{0x13,0x10,0x72}}};
  try {
    for(unsigned profile=0;profile!=sizes.size();++profile) {
      auto profile_uuid=Id(0).value;
      std::copy(tails[profile].begin(),tails[profile].end(),profile_uuid.bytes.begin()+13);
      std::vector<p::byte> retained_predecessor;
      for(unsigned version=0;version!=2;++version) {
        std::array metadata{Metadata(true),Metadata(false)};
        if(version) {
          auto d=Column();d.value_descriptor={Id(96),2};d.datatype_descriptor={Id(97),3};d.type_uuid=Id(98);
          metadata[1].record.payload=Wire(65701,Fields(d));
          for(auto& m:metadata) { m.definition_version=2;m.catalog_generation=2;
            m.creator_transaction_uuid=Id(99,p::UuidKind::transaction);m.creator_local_transaction_id=10; }
        }
        db::NativeCatalogLeafPage leaf;
        leaf.header={sizes[profile],6,Id(2).value,Id(70).value,Id(71+version).value,21+version,7+version,0,profile_uuid};
        auto& body=leaf.body;body.relation_uuid=Id(200);body.segment_id=1;body.segment_generation=2;
        body.compaction_generation=3;body.page_number=leaf.header.page_number;body.page_generation=leaf.header.page_generation;
        for(unsigned i=0;i!=2;++i) {
          const auto encoded=c::EncodeCatalogMetadataVersion(metadata[i]);Check(encoded.ok(),"native fixture metadata");
          page::RowDataRecord row;row.storage_generation=1;row.row_uuid=metadata[i].record.header.row_uuid;
          row.version_uuid=Id(150+i+version*2).value;row.transaction_uuid=metadata[i].creator_transaction_uuid;
          row.local_transaction_id=metadata[i].creator_local_transaction_id;row.internal_row_ordinal=i+1;row.stable_slot_id=i+1;
          page::RowDataCell cell;cell.column_ordinal=1;cell.value.type_id=scratchbird::core::datatypes::CanonicalTypeId::binary;
          cell.value.payload=encoded.bytes;row.cells.push_back(std::move(cell));body.rows.push_back(std::move(row));
        }
        const auto image=db::EncodeNativeCatalogLeaf(leaf);
        Check(image.ok()&&image.bytes.size()==sizes[profile],"native family leaf image refused");
        const auto path=root/(std::to_string(sizes[profile])+"-"+std::to_string(version)+".leaf");
        {
          disk::FileDevice device;Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"exclusive fixture create");
          const auto written=device.WriteAt(0,image.bytes.data(),image.bytes.size());
          Check(written.ok()&&written.bytes_transferred==image.bytes.size()&&device.Sync().ok()&&device.Close().ok(),"actual native image write/sync/close");
        }
        std::vector<p::byte> reopened(sizes[profile]);
        {
          disk::FileDevice device;Check(device.Open(path.string(),disk::FileOpenMode::open_existing_read_only).ok(),"readonly fixture reopen");
          const auto read=device.ReadAt(0,reopened.data(),reopened.size());
          Check(read.ok()&&read.bytes_transferred==reopened.size()&&device.Close().ok(),"complete native image read");
        }
        Check(reopened==image.bytes,"actual file changed native image bytes");
        const auto decoded=db::DecodeNativeCatalogLeaf(reopened);
        Check(decoded.ok()&&decoded.metadata.size()==2,"reopened native leaf lost family records");
        const auto& stored_table=decoded.metadata.at(Id(150+version*2).value);
        const std::array stored_columns{c::BorrowCatalogMetadataVersion(decoded.metadata.at(Id(151+version*2).value))};
        std::array<c::CatalogColumnDefinition,1> materialized;
        const auto complete=c::DecodeCatalogTableColumnCohort(c::BorrowCatalogMetadataVersion(stored_table),stored_columns,materialized);
        Check(complete.ok()&&complete.columns_written==1&&
              Wire(65700,Fields(*complete.table))==metadata[0].record.payload&&
              Wire(65701,Fields(materialized[0]))==metadata[1].record.payload,
              "actual reopened image did not produce the complete owned cohort");
        {
          // Preallocate before retaining a real device guard. This qualifies
          // only structural materialization under a guard, not source lookup.
          std::array<c::CatalogColumnCohortScratch,1> scratch;
          std::array<c::CatalogColumnDefinition,1> bounded;
          c::CatalogTableCohortDecodeResult result;
          disk::FileDevice device;
          Check(device.Open(path.string(),disk::FileOpenMode::open_existing_read_only).ok(),"fenced materialization reopen");
          {
            const auto guard=device.AcquireOperationGuard();no_heap=true;
            result=c::DecodeCatalogTableColumnCohort(c::BorrowCatalogMetadataVersion(stored_table),stored_columns,bounded,scratch);
            no_heap=false;
          }
          Check(device.Close().ok()&&result.ok()&&result.columns_written==1&&
                Wire(65700,Fields(*result.table))==metadata[0].record.payload&&
                Wire(65701,Fields(bounded[0]))==metadata[1].record.payload,"fenced materialization allocated or lost fields");
        }
        for(unsigned i=0;i!=2;++i) {
          const auto found=decoded.metadata.find(Id(150+i+version*2).value);
          Check(found!=decoded.metadata.end()&&found->second.record.payload==metadata[i].record.payload&&
                found->second.record.header.object_uuid.value==metadata[i].record.header.object_uuid.value,
                "reopened family bytes or binary identity changed");
        }
        if(!version)retained_predecessor=reopened;
        else {
          const auto before=db::DecodeNativeCatalogLeaf(retained_predecessor);
          Check(before.ok()&&c::CatalogMetadataPreservesFamilyOrigin(before.metadata.at(Id(151).value),decoded.metadata.at(Id(153).value)),
                "native retype lost immutable identity/ordinal/origin");
          std::array<c::CatalogColumnCohortScratch,1> old_scratch,new_scratch;
          std::array<std::size_t,1> order;
          const std::array old_columns{c::BorrowCatalogMetadataVersion(before.metadata.at(Id(151).value))};
          const auto old_table=c::BorrowCatalogMetadataVersion(before.metadata.at(Id(150).value));
          bool preserved=false;disk::FileDevice device;
          Check(device.Open(path.string(),disk::FileOpenMode::open_existing_read_only).ok(),"fenced history reopen");
          {
            const auto guard=device.AcquireOperationGuard();no_heap=true;
            preserved=c::CatalogTableColumnCohortPreservesHistoryWithScratch(old_table,old_columns,
                c::BorrowCatalogMetadataVersion(stored_table),stored_columns,{old_scratch,new_scratch,order});
            no_heap=false;
          }
          Check(device.Close().ok()&&preserved,"fenced native-image history allocated or lost immutable bindings");
        }
        auto corrupt=reopened;corrupt[corrupt.size()/2]^=1;
        Check(!db::DecodeNativeCatalogLeaf(corrupt).ok(),"native image integrity damage accepted");
        Check(fs::remove(path),"owned native image cleanup failed");
        // Both devices are closed and destroyed before releasing only this
        // fixture's persistent ownership-lock artifacts.
        fs::remove(path.string()+".sb.owner.lock");
        fs::remove(path.string()+".sb.route.owner.lock");
      }
    }
    Check(fs::remove(root),"owned empty fixture directory cleanup failed");
  } catch(...) {
    std::cerr<<"Retained failing native image fixture: "<<root<<'\n';
    throw;
  }
}
}
int main() {
  try {
    FiniteProfiles();MalformedBytes();MetadataAndEvolution();NativeNameIdentities();NativeRetirementRepresentation();EveryNativeFieldEvolution();DefinitionDependencies();AllocationFailures();NativeCohorts();SelectedCohortDecode();PreallocatedCohortDecode();NativeImagePersistence();
    std::cout<<"PASS native table/column definition checks="<<checks<<" faults="<<faults<<"; no native CRUD claim\n";
    return 0;
  } catch(const std::exception& error) {
    no_heap=false;fail_after=-1;std::cerr<<error.what()<<'\n';return 1;
  }
}

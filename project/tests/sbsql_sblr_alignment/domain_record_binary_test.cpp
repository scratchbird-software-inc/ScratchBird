// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Compile the real codec owner into this component to exercise its private
// decoder without replacing transaction or persistence dependencies.
#include "../../src/engine/internal_api/domain_support/domain_store.cpp"
#include "catalog/name_registry_codec.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a=scratchbird::engine::internal_api;
static void Check(bool value,std::source_location at=std::source_location::current()){
  if(!value){std::cerr<<"failure at "<<at.line()<<'\n';std::abort();}
}
static a::EngineUuid Id(unsigned n){a::EngineUuid id;id.bytes={1,144,10,9,0,0,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)};return id;}
int main(){
  a::DomainRecord r;r.creator_tx=23;r.domain_uuid=Id(1);r.catalog_row_uuid=Id(2);r.schema_uuid=Id(3);r.base_descriptor_uuid=Id(4);r.default_name=std::string("name\0\t\n",7);r.base_descriptor_kind="scalar";r.base_canonical_type_name="int64";r.default_expression_envelope=std::string("\xff\0\n;=",5);
  const auto bytes=a::MakeDomainCreateEvent(r);Check(bytes.size()>152&&bytes.substr(0,8)=="SBDOMR02");
  std::size_t offset=88;for(auto id:{r.domain_uuid,r.catalog_row_uuid,r.schema_uuid,r.base_descriptor_uuid}){for(unsigned n=0;n<16;++n)Check(static_cast<std::uint8_t>(bytes[offset+n])==id.bytes[n]);offset+=16;}
  a::DomainBinaryAction action;a::DomainRecord decoded;Check(a::DecodeDomainEvent(bytes,&action,&decoded));Check(action==a::DomainBinaryAction::create&&decoded.domain_uuid==r.domain_uuid&&decoded.schema_uuid==r.schema_uuid&&decoded.default_name==r.default_name&&decoded.default_expression_envelope==r.default_expression_envelope);
  Check(a::DecodeDomainEvent(a::MakeDomainAlterEvent(r),&action,&decoded)&&action==a::DomainBinaryAction::alter);
  Check(a::DecodeDomainEvent(a::MakeDomainDropEvent(24,r.domain_uuid),&action,&decoded)&&action==a::DomainBinaryAction::drop&&decoded.dropped&&decoded.creator_tx==24&&decoded.domain_uuid==r.domain_uuid);
  auto reject=[&](const std::string& value){a::DomainRecord out;out.domain_uuid=Id(77);auto op=a::DomainBinaryAction::alter;Check(!a::DecodeDomainEvent(value,&op,&out));Check(out.domain_uuid==Id(77)&&op==a::DomainBinaryAction::alter);};
  for(std::size_t n=0;n<bytes.size();++n)reject(bytes.substr(0,n));
  for(std::size_t n=0;n<bytes.size();++n){auto damaged=bytes;damaged[n]^=1;reject(damaged);}
  reject(bytes+"x");reject("SBDOMAIN1\tDOMAIN_DROP\t24\t01900000-0000-7000-8000-000000000001");
  const auto binding=a::DomainColumnDescriptor(r.domain_uuid);Check(binding.substr(0,8)=="SBMETA02"&&a::DomainUuidFromColumnDescriptor(binding)==r.domain_uuid);
  a::CatalogColumnMetadata column;Check(a::DecodeCatalogColumnMetadata(binding,&column));
  column.text["default"]=std::string("expr\0;=",7);column.text["nullable"]="false";column.text["type"]="int64";column.identities["charset_uuid"]=Id(19);
  std::string full;Check(a::EncodeCatalogColumnMetadata(column,&full));
  Check(a::DomainUuidFromColumnDescriptor(full)==r.domain_uuid);
  a::CatalogColumnMetadata read_column;Check(a::DecodeCatalogColumnMetadata(full,&read_column));
  Check(read_column.text==column.text&&read_column.identities==column.identities);
  Check(!a::AdmitCatalogColumnMetadata("type=int64;domain_uuid=01900000-0000-7000-8000-000000000001",&read_column));
  Check(!a::AdmitCatalogColumnMetadata("type=int64;nullable=true;nullable=false",&read_column));
  Check(!a::DecodeCatalogColumnMetadata(full+"x",&read_column));
  for(std::size_t n=0;n<full.size();++n)Check(!a::DecodeCatalogColumnMetadata(full.substr(0,n),&read_column));
  Check(a::DomainUuidFromColumnDescriptor("domain:01900000-0000-7000-8000-000000000001").is_nil());
  for(std::size_t n=0;n<binding.size();++n)Check(a::DomainUuidFromColumnDescriptor(binding.substr(0,n)).is_nil());
  a::NameRegistryEntry name;name.name_entry_uuid=Id(10);name.object_uuid=Id(11);name.scope_uuid=Id(12);name.parent_object_uuid=Id(13);name.parent_schema_uuid=Id(14);name.object_class="schema";name.raw_name_text=std::string("x\0\t\ny",5);name.display_name=name.raw_name_text;name.creator_tx=19;
  std::string named;Check(a::EncodeNameRegistryEntry(name,&named));Check(named.substr(0,8)=="SBNAME02");offset=8;for(auto id:{name.name_entry_uuid,name.object_uuid,name.scope_uuid,name.parent_object_uuid,name.parent_schema_uuid}){for(unsigned n=0;n<16;++n)Check(static_cast<std::uint8_t>(named[offset+n])==id.bytes[n]);offset+=16;}
  a::NameRegistryEntry read;Check(a::DecodeNameRegistryEntry(named,&read)&&read.raw_name_text==name.raw_name_text&&read.parent_schema_uuid==name.parent_schema_uuid);
  for(std::size_t n=0;n<named.size();++n){read.object_uuid=Id(77);Check(!a::DecodeNameRegistryEntry(named.substr(0,n),&read)&&read.object_uuid==Id(77));}
  Check(!a::DecodeNameRegistryEntry(named+"x",&read));
  a::ApiBehaviorRecord event;event.creator_tx=name.creator_tx;event.object_uuid=name.name_entry_uuid;event.target_object_uuid=name.object_uuid;event.target_schema_uuid=name.scope_uuid;event.operation_id="catalog.name";event.object_kind="name_registry.entry";event.state="active";event.payload=named;
  std::string framed;Check(a::EncodeApiBehaviorRecord(event,&framed));for(std::size_t n=0;n<framed.size();++n){auto damaged=framed;damaged[n]^=1;a::ApiBehaviorRecord out;Check(!a::DecodeApiBehaviorRecord({reinterpret_cast<const std::uint8_t*>(damaged.data()),damaged.size()},&out));}
}

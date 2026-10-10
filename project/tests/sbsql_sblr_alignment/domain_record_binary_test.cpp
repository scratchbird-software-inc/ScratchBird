// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Compile the real codec owner into this component to exercise its private
// decoder without replacing transaction or persistence dependencies.
#include "../../src/engine/internal_api/domain_support/domain_store.cpp"
#include "catalog/name_registry_codec.hpp"
#include "domain_support/domain_base_descriptor_codec.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a=scratchbird::engine::internal_api;
static void Check(bool value,std::source_location at=std::source_location::current()){
  if(!value){std::cerr<<"failure at "<<at.line()<<'\n';std::abort();}
}
static a::EngineUuid Id(unsigned n){a::EngineUuid id;id.bytes={1,144,10,9,0,0,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)};return id;}
int main(){
  Check(a::IsSupportedDomainCheckEnvelope("all:gte:0;all:lt:12.5"));
  Check(!a::IsSupportedDomainCheckEnvelope("all:gte:0;"));
  Check(!a::IsSupportedDomainCheckEnvelope("all:;gte:0"));
  std::string nested;
  for(unsigned i=0;i<32768;++i)nested.append("all:");
  nested.append("gte:0");
  Check(a::IsSupportedDomainCheckEnvelope(nested));
  a::EngineDescriptor integer;integer.canonical_type_name="int64";
  std::string predicate_detail;
  Check(a::EvaluateDomainCheckPredicate(nested, integer, std::string("\1\0\0\0\0\0\0\0",8), &predicate_detail));
  a::EngineDescriptor base;
  base.descriptor_uuid=Id(20);base.type_uuid=Id(21);
  base.datatype_descriptor_uuid=Id(22);base.datatype_descriptor_generation=42;
  base.charset_uuid=Id(23);base.collation_uuid=Id(24);
  base.descriptor_kind="scalar";base.canonical_type_name="character";
  base.encoded_descriptor=std::string("metadata\0;=",11);
  std::string bound;Check(a::EncodeDomainBaseDescriptorV1(base,&bound));
  Check(bound.substr(0,8)=="SBDTDS01");
  std::size_t bound_offset=8;
  for(const auto id:{base.descriptor_uuid,base.type_uuid,base.datatype_descriptor_uuid}) {
    for(unsigned n=0;n<16;++n)Check(static_cast<std::uint8_t>(bound[bound_offset+n])==id.bytes[n]);
    bound_offset+=16;
  }
  Check(static_cast<unsigned char>(bound[56])==42);
  for(unsigned n=57;n<64;++n)Check(bound[n]==0);
  a::EngineDescriptor restored;Check(a::DecodeDomainBaseDescriptorV1(bound,&restored)&&restored==base);
  const auto reject_base=[&](const std::string& damaged) {
    auto retained=base;Check(!a::DecodeDomainBaseDescriptorV1(damaged,&retained));Check(retained==base);
  };
  for(std::size_t n=0;n<bound.size();++n)reject_base(bound.substr(0,n));
  reject_base(bound+"x");reject_base("canonical=character");
  for(const auto offset:{8U,24U,40U,64U,80U}) {
    auto damaged=bound;damaged[offset+6]=0x40;reject_base(damaged);
  }
  for(const auto member:{&a::EngineDescriptor::descriptor_uuid,&a::EngineDescriptor::type_uuid,
                         &a::EngineDescriptor::datatype_descriptor_uuid}) {
    auto missing=base;missing.*member={};auto kept=std::string("unchanged");
    Check(!a::EncodeDomainBaseDescriptorV1(missing,&kept)&&kept=="unchanged");
  }
  auto unbound=base;unbound.datatype_descriptor_generation=0;
  Check(!a::EncodeDomainBaseDescriptorV1(unbound,&bound));
  Check(a::DecodeDomainBaseDescriptorV1(bound,&restored)&&restored==base);
  auto plain=base;plain.charset_uuid={};plain.collation_uuid={};
  Check(a::EncodeDomainBaseDescriptorV1(plain,&bound));
  Check(a::DecodeDomainBaseDescriptorV1(bound,&restored)&&restored==plain);
  a::DomainInheritedBaseBindingV1 profile;
  profile.base=plain;profile.catalog_snapshot_uuid=Id(30);profile.catalog_generation=7;
  profile.registry_generation=8;profile.codec_uuid=Id(31);profile.codec_version=1;profile.codec_generation=9;
  profile.native_profile_fingerprint.assign(32,'\0');
  for(std::size_t i=0;i<8;++i){profile.policy_uuids[i]=Id(40+i);profile.policy_generations[i]=i+1;}
  std::string inherited;Check(a::EncodeDomainInheritedBaseBindingV1(profile,&inherited));
  Check(inherited.substr(0,8)=="SBDPFB01");
  Check(static_cast<unsigned char>(inherited[24])==7 && static_cast<unsigned char>(inherited[32])==8);
  Check(static_cast<unsigned char>(inherited[56])==1 && static_cast<unsigned char>(inherited[60])==9);
  for(std::size_t i=0;i<9;++i){
    for(unsigned j=0;j<16;++j)Check(static_cast<unsigned char>(inherited[68+24*i+j])==profile.policy_uuids[i].bytes[j]);
    Check(static_cast<unsigned char>(inherited[84+24*i])==profile.policy_generations[i]);
  }
  a::DomainInheritedBaseBindingV1 profile_read;
  Check(a::DecodeDomainInheritedBaseBindingV1(inherited,&profile_read)&&profile_read==profile);
  const auto reject_profile=[&](const std::string& damaged){
    auto retained=profile;Check(!a::DecodeDomainInheritedBaseBindingV1(damaged,&retained));Check(retained==profile);
  };
  for(std::size_t n=0;n<inherited.size();++n)reject_profile(inherited.substr(0,n));
  reject_profile(inherited+"x");reject_profile(bound);
  for(const auto offset:{8U,40U,68U}){auto damaged=inherited;damaged[offset+6]=0x40;reject_profile(damaged);}
  for(const auto offset:{24U,32U,56U,60U,84U}){auto damaged=inherited;damaged[offset]=0;reject_profile(damaged);}
  auto damaged=inherited;damaged[284]=1;reject_profile(damaged);
  auto missing=profile;missing.policy_uuids[0]={};
  std::string retained="unchanged";Check(!a::EncodeDomainInheritedBaseBindingV1(missing,&retained)&&retained=="unchanged");
  auto builtin=profile;builtin.policy_uuids={};builtin.policy_generations={};
  Check(a::EncodeDomainInheritedBaseBindingV1(builtin,&inherited));
  Check(a::DecodeDomainInheritedBaseBindingV1(inherited,&profile_read)&&profile_read==builtin);
  builtin.codec_uuid={};
  Check(a::EncodeDomainInheritedBaseBindingV1(builtin,&inherited));
  Check(a::DecodeDomainInheritedBaseBindingV1(inherited,&profile_read)&&profile_read==builtin);
  a::DomainRecord r;r.creator_tx=23;r.domain_uuid=Id(1);r.catalog_row_uuid=Id(2);r.schema_uuid=Id(3);r.base_descriptor_uuid=Id(4);r.default_name=std::string("name\0\t\n",7);r.base_descriptor_kind="scalar";r.base_canonical_type_name="int64";r.default_expression_envelope=std::string("\xff\0\n;=",5);
  r.base_descriptor_uuid=plain.datatype_descriptor_uuid;
  r.base_canonical_type_name=plain.canonical_type_name;
  r.base_encoded_descriptor=bound;
  const auto bytes=a::MakeDomainCreateEvent(r);Check(bytes.size()>152&&bytes.substr(0,8)=="SBDOMR02");
  std::size_t offset=88;for(auto id:{r.domain_uuid,r.catalog_row_uuid,r.schema_uuid,r.base_descriptor_uuid}){for(unsigned n=0;n<16;++n)Check(static_cast<std::uint8_t>(bytes[offset+n])==id.bytes[n]);offset+=16;}
  a::DomainBinaryAction action;a::DomainRecord decoded;Check(a::DecodeDomainEvent(bytes,&action,&decoded));Check(action==a::DomainBinaryAction::create&&decoded.domain_uuid==r.domain_uuid&&decoded.schema_uuid==r.schema_uuid&&decoded.default_name==r.default_name&&decoded.default_expression_envelope==r.default_expression_envelope);
  Check(decoded.base_encoded_descriptor==bound);
  Check(a::DecodeDomainBaseDescriptorV1(decoded.base_encoded_descriptor,&restored)&&restored==plain);
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

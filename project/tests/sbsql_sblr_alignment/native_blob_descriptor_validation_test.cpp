// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/native_blob_descriptor_validation.hpp"
#include "mga_relation_store/mga_relation_descriptor.hpp"
#include "datatype_blob.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <iostream>
#include <stdexcept>

namespace api=scratchbird::engine::internal_api;
namespace dt=scratchbird::core::datatypes;
namespace t=scratchbird::tests;
namespace {
unsigned checks=0;
void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
api::EngineDescriptor Blob(){
  dt::DatatypeStorageIdentityV3 row;
  Check(dt::LookupDatatypeStorageIdentityV3(dt::kBlobV11ReceiptUuid,11,11,
      t::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d830"),1,&row)&&row.codec.has_value(),
      "actual registered native BLOB tuple missing");
  api::EngineDescriptor value;
  value.descriptor_uuid=t::FixtureUuid(0x424c4f42,1);
  value.descriptor_kind="scalar";value.canonical_type_name="blob";
  value.datatype_descriptor_uuid=row.descriptor_uuid;
  value.datatype_descriptor_generation=row.descriptor_generation;
  value.type_uuid=row.type_uuid;value.datatype_cohort={dt::kBlobV11ReceiptUuid,11,11};
  value.encoded_descriptor="type=blob";return value;
}
void Pure(){
  const auto good=Blob();
  for(bool nullable:{false,true}){
    Check(api::ValidateNativeBinaryBlobDescriptorV1(good,nullable).ok(),"exact native BLOB refused");
    for(const auto& text:{std::string("type=blob"),std::string("canonical=blob"),
        std::string("canonical_type=blob"),std::string("type=blob;nullable=")+(nullable?"true":"false"),
        std::string("type=blob;nullability=")+(nullable?"nullable":"non_null"),
        std::string("type=blob;not_null=")+(nullable?"false":"true"),
        std::string("type=blob;default=0;check=declared;unique=false;generated=false;identity=false")}){
      auto value=good;value.encoded_descriptor=text;
      Check(api::ValidateNativeBinaryBlobDescriptorV1(value,nullable).ok(),"single exact declaration/constraint refused");
      api::CatalogColumnMetadata fields;Check(api::AdmitCatalogColumnMetadata(text,&fields),"fixture metadata decode");
      Check(api::EncodeCatalogColumnMetadata(fields,&value.encoded_descriptor),"fixture metadata frame");
      Check(api::ValidateNativeBinaryBlobDescriptorV1(value,nullable).ok(),"framed exact declaration refused");
    }
    for(const char* key:{"charset_generation","collation_generation","resource_epoch","character_length",
        "char_length","text_resource_storage","precision","scale","length","container_rank","timezone",
        "security_label","unknown"}){
      for(const char* payload:{"","0","large_object"}){
        for(bool alias:{false,true}){
          auto value=good;if(alias)value.canonical_type_name="localized_blob_alias";
          value.encoded_descriptor="type="+value.canonical_type_name+";"+key+"="+payload;
          auto result=api::ValidateNativeBinaryBlobDescriptorV1(value,nullable);
          Check(!result.ok()&&!result.resource_failure,"binary BLOB admitted decoration by value or alias");
          api::CatalogColumnMetadata fields;Check(api::AdmitCatalogColumnMetadata(value.encoded_descriptor,&fields),"fixture modifier decode");
          Check(api::EncodeCatalogColumnMetadata(fields,&value.encoded_descriptor),"fixture modifier frame");
          Check(!api::ValidateNativeBinaryBlobDescriptorV1(value,nullable).ok(),"framed BLOB admitted forbidden modifier");
        }
      }
    }
    for(unsigned mutation=0;mutation<24;++mutation){
      auto value=good;
      switch(mutation){
        case 0:value.datatype_cohort={};break;
        case 1:value.datatype_cohort.catalog_snapshot_uuid={};break;
        case 2:value.datatype_cohort.catalog_generation=0;break;
        case 3:value.datatype_cohort.registry_generation=0;break;
        case 4:++value.datatype_cohort.catalog_generation;break;
        case 5:++value.datatype_cohort.registry_generation;break;
        case 6:value.datatype_cohort.catalog_snapshot_uuid.bytes.back()^=1;break;
        case 7:value.datatype_descriptor_uuid={};break;
        case 8:++value.datatype_descriptor_generation;break;
        case 9:value.type_uuid={};break;
        case 10:value.type_uuid.bytes.back()^=1;break;
        case 11:value.datatype_descriptor_uuid.bytes.back()^=1;break;
        case 12:value.descriptor_uuid={};break;
        case 13:value.descriptor_kind="row";break;
        case 14:value.canonical_type_name="integer";value.encoded_descriptor="type=integer";break;
        case 15:value.encoded_descriptor="type=integer";break;
        case 16:value.charset_uuid=t::FixtureUuid(3,3);break;
        case 17:value.collation_uuid=t::FixtureUuid(3,3);break;
        case 18:value.canonical_type_name.clear();break;
        case 19:value.canonical_type_name=std::string("blob\0x",6);break;
        case 20:value.encoded_descriptor+=";nullable=true;not_null=false";break;
        case 21:value.encoded_descriptor+=";nullable=true;nullability=nullable";break;
        case 22:value.encoded_descriptor+=";nullability=nullable;not_null=false";break;
        case 23:value.encoded_descriptor+=";nullable="+std::string(nullable?"false":"true");break;
      }
      Check(!api::ValidateNativeBinaryBlobDescriptorV1(value,nullable).ok(),"malformed native BLOB identity/nullability admitted");
      if(mutation<12){value.canonical_type_name="localized_blob_alias";value.encoded_descriptor="type=localized_blob_alias";
        Check(!api::ValidateNativeBinaryBlobDescriptorV1(value,nullable).ok(),"alias hid malformed binary BLOB cohort");}
    }
    api::CatalogColumnMetadata tuple;
    const auto* codec=dt::FindDatatypeTypeCodecIdentityV3(good.datatype_cohort.catalog_snapshot_uuid,11,11,
        good.datatype_descriptor_uuid,1);
    Check(codec!=nullptr,"registered borrowed codec absent");const auto& row=codec->legacy_fields;
    tuple.identities={{"type_uuid",row.type_uuid},{"datatype_descriptor_uuid",row.descriptor_uuid},{"codec_uuid",row.codec_uuid}};
    tuple.text={{"type","blob"},{"datatype_descriptor_generation",std::to_string(row.descriptor_generation)},
      {"type_generation",std::to_string(row.type_generation)},{"codec_id",row.codec_id},
      {"codec_version",std::to_string(row.codec_version)},{"codec_generation",std::to_string(row.codec_generation)},
      {"null_encoding",std::to_string(row.null_encoding_code)}};
    auto exact=good;Check(api::EncodeCatalogColumnMetadata(tuple,&exact.encoded_descriptor),"exact tuple encode");
    Check(api::ValidateNativeBinaryBlobDescriptorV1(exact,nullable).ok(),"complete redundant tuple refused");
    for(const auto& [key,unused]:tuple.text){if(key=="type")continue;auto bad=tuple;bad.text[key]+="0";
      Check(api::EncodeCatalogColumnMetadata(bad,&exact.encoded_descriptor),"mutated tuple encode");
      Check(!api::ValidateNativeBinaryBlobDescriptorV1(exact,nullable).ok(),"wrong redundant codec field admitted");}
    for(const char* key:{"charset_uuid","collation_uuid","domain_uuid","codec_uuid","type_uuid","datatype_descriptor_uuid"}){
      auto bad=tuple;bad.identities[key]=t::FixtureUuid(3,4);
      Check(api::EncodeCatalogColumnMetadata(bad,&exact.encoded_descriptor),"mutated identity encode");
      Check(!api::ValidateNativeBinaryBlobDescriptorV1(exact,nullable).ok(),"wrong or forbidden redundant identity admitted");
    }
    auto alias=good;alias.canonical_type_name="localized_blob_alias";alias.encoded_descriptor="canonical=localized_blob_alias";
    Check(api::ValidateNativeBinaryBlobDescriptorV1(alias,nullable).ok(),"presentation alias inferred a different native type");
  }
  api::EngineDescriptor unrelated;unrelated.canonical_type_name="legacy_unrelated";
  Check(api::ValidateNativeBinaryBlobDescriptorV1(unrelated,true).ok(),"cohort-absent unrelated compatibility broadened");
  unrelated.datatype_cohort.registry_generation=1;
  Check(!api::ValidateNativeBinaryBlobDescriptorV1(unrelated,true).ok(),"malformed supplied cohort ignored");
  auto predecessor=good;predecessor.type_uuid={};predecessor.datatype_cohort={};
  predecessor.datatype_descriptor_uuid=t::FixtureUuidLiteral("f4010000-626c-7f62-8000-000000000000");
  Check(!api::ValidateNativeBinaryBlobDescriptorV1(predecessor,true).ok(),"known storage-only predecessor bypassed exact cohort");
}
void Retained(){
  api::MgaRelationStorageDescriptor relation;
  relation.descriptor_uuid=t::FixtureUuid(4,1);relation.database_uuid=t::FixtureUuid(4,2);
  relation.schema_uuid=t::FixtureUuid(4,3);relation.relation_uuid=t::FixtureUuid(4,4);
  relation.primary_filespace_uuid=t::FixtureUuid(4,5);relation.relation_generation=1;
  api::MgaRelationColumnStorageDescriptor column;column.column_uuid=t::FixtureUuid(4,6);
  column.column_generation=1;column.canonical_name_key="payload";column.value_descriptor=Blob();relation.columns={column};
  Check(!api::ValidateMgaRelationStorageDescriptor(relation).error,"valid retained binary BLOB refused");
  const auto fields=api::SerializeMgaRelationStorageDescriptor(relation);
  auto decoded=api::DeserializeMgaRelationStorageDescriptor(fields);
  Check(!api::ValidateMgaRelationStorageDescriptor(decoded).error,"serialized native binding failed retained validation");
  for(unsigned mutation=0;mutation<7;++mutation){auto bad=decoded;
    switch(mutation){
      case 0:bad.columns[0].character_length=1;break;
      case 1:bad.columns[0].value_descriptor.encoded_descriptor+=";text_resource_storage=large_object";break;
      case 2:bad.columns[0].value_descriptor.datatype_cohort={};break;
      case 3:bad.columns[0].value_descriptor.canonical_type_name="localized_blob_alias";
        bad.columns[0].value_descriptor.encoded_descriptor="canonical=localized_blob_alias;character_length=0";break;
      case 4:bad.columns[0].charset_uuid=bad.columns[0].value_descriptor.charset_uuid=t::FixtureUuid(4,7);break;
      case 5:bad.columns[0].value_descriptor.encoded_descriptor+=";resource_epoch=0";break;
      case 6:bad.columns[0].nullable=false;bad.columns[0].value_descriptor.encoded_descriptor+=";nullable=true";break;
    }
    Check(api::ValidateMgaRelationStorageDescriptor(bad).error,"retained descriptor admitted binary text disguise");
    Check(api::SerializeMgaRelationStorageDescriptor(decoded)==fields,"pure refused validation changed original representation");
  }
}
}
int main(){try{Pure();Retained();std::cout<<checks<<" checks PASS\n";return 0;}
  catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

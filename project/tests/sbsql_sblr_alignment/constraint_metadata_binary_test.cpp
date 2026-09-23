// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/dml/constraint_enforcement.cpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a=scratchbird::engine::internal_api;
void Check(bool ok,std::source_location at=std::source_location::current()){
  if(!ok){std::cerr<<"failure at "<<at.line()<<'\n';std::abort();}
}
a::EngineUuid Id(unsigned n){return a::EngineUuid{{1,144,0,0,0,0,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)}};}
int main(){
  a::CatalogColumnMetadata fields;
  fields.text={{"type","int64"},{"referenced_column","id"},{"constraint_mutation_batch_state","sealed"},{"default",std::string("literal:a\0;=",12)}};
  fields.identities={{"constraint_uuid",Id(1)},{"referenced_table_uuid",Id(2)},{"referenced_column_uuid",Id(3)},
      {"referenced_candidate_key_constraint_uuid",Id(4)},{"referenced_key_descriptor_uuid",Id(5)},
      {"referenced_support_uuid",Id(6)},{"constraint_mutation_batch_uuid",Id(7)}};
  auto encoded=a::DescriptorText(fields);auto decoded=a::DescriptorFields(encoded);
  Check(decoded.text==fields.text&&decoded.identities==fields.identities);
  const auto reference=a::ParseForeignKeyReference(decoded);
  Check(reference&&reference->parent_table_uuid==Id(2)&&reference->parent_column_uuid==Id(3)&&reference->key_descriptor_uuid==Id(5)&&reference->support_uuid==Id(6)&&reference->sealed);
  Check(a::FieldOrEmpty(decoded,{"constraint_uuid"}).empty());
  a::CatalogColumnMetadata legacy;legacy.text["references"]="01900000-0000-7000-8000-000000000002:id";
  Check(a::DescriptorDeclaresForeignKey(legacy)&&!a::ParseForeignKeyReference(legacy));
  bool rejected=false;try{a::DescriptorFields("type=int64;constraint_uuid=01900000-0000-7000-8000-000000000001");}catch(const std::invalid_argument&){rejected=true;}
  Check(rejected);
  a::CrudIndexRecord index;index.index_uuid=Id(8);
  auto proof=a::UniquePreflightProofKey(index,Id(9),std::string("key\0\n",5));
  std::vector<std::string> parts;Check(a::DecodeMgaMetadataFields(proof,&parts)&&parts[1]==a::MetadataUuidBytes(Id(8))&&parts[2]==a::MetadataUuidBytes(Id(9)));
  a::EngineRequestContext context;context.database_uuid=Id(10);context.transaction_uuid=Id(11);context.statement_uuid=Id(12);
  a::CrudTableRecord table;table.table_uuid=Id(13);
  auto diagnostic=a::ConstraintDiagnostic("CLI.CONSTRAINT_UNIQUE_VIOLATION","constraint.unique.violation",context,table,"unique",Id(1),"duplicate_key","id",Id(5),Id(6));
  Check(diagnostic.identity_fields.size()==6&&diagnostic.identity_fields[0].second==Id(1)&&diagnostic.identity_fields[1].second==Id(13));
  Check(diagnostic.detail.find(a::MetadataUuidBytes(Id(1)))==std::string::npos);
  Check(a::ConstraintUuid({},table,"id","unique").is_nil()); // no fabricated descriptor identity
}

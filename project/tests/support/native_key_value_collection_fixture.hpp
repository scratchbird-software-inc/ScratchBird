// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "published_mga_table_fixture.hpp"
#include "../../src/engine/internal_api/catalog/column_metadata_codec.hpp"
#include <stdexcept>

namespace scratchbird::tests {
// Administrative publication through actual native catalog/MGA APIs. The
// context already owns a real transaction; labels never confer authority.
inline engine::internal_api::EngineUuid PublishNativeKeyValueCollectionFixture(
    engine::internal_api::EngineRequestContext& context, std::string name,
    std::uint32_t key_character_limit=4096, std::uint32_t value_character_limit=4096) {
  namespace api=engine::internal_api;
  api::CrudTableRecord table;
  table.creator_tx=context.local_transaction_id;table.table_uuid=api::GenerateCrudEngineUuid("object");
  table.default_name=std::move(name);
  table.columns={{"key",std::string("type=text;nullable=false;unique=true")+
          (key_character_limit?";character_length="+std::to_string(key_character_limit):"")},
      {"value",std::string("type=text;nullable=false")+
          (value_character_limit?";character_length="+std::to_string(value_character_limit):"")},
      {"expires_after_local_transaction_id","type=uint64;nullable=false"}};
  api::CrudIndexRecord index;
  index.creator_tx=context.local_transaction_id;
  index.index_uuid=api::GenerateCrudEngineUuid("object");index.table_uuid=table.table_uuid;
  index.column_name="key";index.family=api::kCrudIndexFamilyBtree;
  index.profile=api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.default_name=table.default_name+"_key_unique";index.key_envelopes={"key"};index.unique=true;
  api::CatalogColumnMetadata key;
  if(!api::AdmitCatalogColumnMetadata(table.columns[0].second,&key))
    throw std::runtime_error("native KV fixture key metadata invalid");
  key.identities["candidate_key_constraint_uuid"]=api::GenerateCrudEngineUuid("object");
  key.identities["support_uuid"]=index.index_uuid;
  if(!api::EncodeCatalogColumnMetadata(key,&table.columns[0].second))
    throw std::runtime_error("native KV fixture binary constraint metadata invalid");
  const auto require=[](const api::EngineApiDiagnostic& diagnostic) {
    if(diagnostic.error)throw std::runtime_error(diagnostic.code+":"+diagnostic.message_key+":"+diagnostic.detail);
  };
  require(PublishMgaTableFixture(context,table,{"text","text","uint64"},{index}));
  require(api::AppendMgaIndexMetadata(context,index));
  const auto loaded=api::LoadMgaRelationStorageDescriptor(context,table.table_uuid);
  if(!loaded.ok){require(loaded.diagnostic);throw std::runtime_error("native KV fixture descriptor not published");}
  if(loaded.descriptor.relation_uuid!=table.table_uuid || loaded.descriptor.columns.size()!=3)
    throw std::runtime_error("native KV fixture actual relation/column publication mismatch");
  return table.table_uuid;
}
}  // namespace scratchbird::tests

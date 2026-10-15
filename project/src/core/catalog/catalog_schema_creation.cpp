// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_schema_creation.hpp"
#include "catalog_database_record_codec.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <numeric>
#include <stdexcept>

namespace scratchbird::core::catalog {
namespace {
using E=CatalogInitialSchemaError;
CatalogInitialSchemaResult Fail(E e,CatalogNamedCreationError name=CatalogNamedCreationError::none) {
  return {e,name,{}};
}
bool Same(const TypedUuid& a,const TypedUuid& b){return a.kind==b.kind&&a.value==b.value;}
bool First(const CatalogMetadataVersion& m,const TypedUuid& tx) {
  return m.authority_scope==CatalogAuthorityScope::local&&m.definition_version==1&&
      m.schema_epoch==1&&m.security_epoch==1&&m.catalog_generation==1&&
      m.creator_local_transaction_id==1&&Same(m.creator_transaction_uuid,tx)&&
      !m.record.header.deleted&&m.lifecycle==CatalogObjectLifecycle::active&&m.status==CatalogObjectStatus::active;
}
E NameError(CatalogNamedCreationError e) {
  switch(e) {
    case CatalogNamedCreationError::invalid_request:return E::invalid_request;
    case CatalogNamedCreationError::invalid_metadata:return E::invalid_metadata;
    case CatalogNamedCreationError::invalid_names:return E::invalid_names;
    case CatalogNamedCreationError::duplicate_identity:return E::duplicate_identity;
    case CatalogNamedCreationError::resource_exhausted:return E::resource_exhausted;
    case CatalogNamedCreationError::none:return E::none;
  }
  return E::invalid_request;
}
}
CatalogInitialSchemaResult AssembleLocalInitialSchemaRecords(
    const Uuid& database,const CatalogMetadataVersion& db,const TypedUuid& tx,
    std::string_view language,std::span<const CatalogInitialSchemaInput> inputs,
    CatalogNamedCreationLimits limits) noexcept {
  try {
    if(!uuid::IsEngineIdentityUuid(database)||tx.kind!=UuidKind::transaction||
        !uuid::IsEngineIdentityUuid(tx.value)||database==tx.value||inputs.empty()||
        language.empty()||language.size()>131040||!limits.maximum_records||!limits.maximum_encoded_bytes)
      return Fail(E::invalid_request);
    if(inputs.size()>limits.maximum_records/3)return Fail(E::resource_exhausted);
    if(db.record.header.kind!=CatalogRecordKind::database||!First(db,tx)||
        db.record.header.object_uuid.kind!=UuidKind::object||
        !CatalogMetadataVersionEncodedBytes(BorrowCatalogMetadataVersion(db)))return Fail(E::invalid_database);
    const auto decoded=DecodeCatalogDatabaseRecord(db.record.payload);
    if(!decoded.ok()||decoded.record->database_uuid.value!=database||
        decoded.record->creator_transaction_number!=1||!decoded.record->datatype_cohort)
      return Fail(E::invalid_database);
    const std::array<Uuid,4> enclosing{database,tx.value,
        db.record.header.object_uuid.value,db.record.header.row_uuid.value};
    for(std::size_t i=0;i<enclosing.size();++i)for(std::size_t j=0;j<i;++j)
      if(enclosing[i]==enclosing[j])return Fail(E::invalid_database);
    // Full borrowed metadata/name admission with canonical family bytes on the
    // stack. No owning fields or payloads are copied before whole-cohort bounds.
    u64 records=0,bytes=0;
    for(const auto& input:inputs) {
      if(!input.definition||!input.metadata||!input.vector_metadata||!input.vector||input.entries.empty())
        return Fail(E::invalid_request);
      const auto& d=*input.definition;const auto& m=*input.metadata;
      if(!m.record.payload.empty()||m.record.header.kind!=CatalogRecordKind::schema||!First(m,tx))
        return Fail(E::invalid_metadata);
      if(!Same(d.database_catalog_object_uuid,db.record.header.object_uuid)||
          !Same(d.origin_transaction_uuid,tx)||d.origin_local_transaction_id!=1||
          d.schema_type==CatalogSchemaType::cluster)return Fail(E::invalid_definition);
      std::array<byte,248> payload;
      const auto encoded=EncodeCatalogSchemaDefinitionInto(d,payload);
      if(!encoded.ok())return Fail(E::invalid_definition);
      auto view=BorrowCatalogMetadataVersion(m);
      view.record.payload={reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size()};
      if(!CatalogSchemaDefinitionMatchesMetadata(view))return Fail(E::invalid_metadata);
      const CatalogNamedCreationInput named{input.metadata,input.vector_metadata,input.vector,input.entries};
      const auto checked=PreflightLocalNamedCreationRecord(view,named,tx,language,
          {limits.maximum_records-records,limits.maximum_encoded_bytes-bytes});
      if(checked.error!=CatalogNamedCreationError::none)return Fail(NameError(checked.error),checked.error);
      records+=checked.records;bytes+=checked.encoded_bytes;
    }
    if(records>(std::numeric_limits<std::size_t>::max()-4)/2)return Fail(E::resource_exhausted);
    std::array<Uuid,256> small;
    std::vector<Uuid> large;
    const auto count=static_cast<std::size_t>(2*records+4);
    if(count>small.size())large.resize(count);
    auto* ids=large.empty()?small.data():large.data();
    std::copy(enclosing.begin(),enclosing.end(),ids);std::size_t at=4;
    for(const auto& input:inputs) {
      const auto append=[&](const CatalogMetadataVersion& m) {
        ids[at++]=m.record.header.object_uuid.value;ids[at++]=m.record.header.row_uuid.value;
      };
      append(*input.metadata);append(*input.vector_metadata);
      for(const auto& entry:input.entries)append(*entry.metadata);
    }
    std::sort(ids,ids+count);
    if(std::adjacent_find(ids,ids+count)!=ids+count)return Fail(E::duplicate_identity);
    // Raw binary ordering and indexed lookup: O(N log N), regardless of input
    // order. Iterative colors detect cycles in O(N), including arbitrarily deep
    // forests. No namespace name or hash digest participates in identity.
    std::vector<std::size_t> order(inputs.size());std::iota(order.begin(),order.end(),0);
    std::sort(order.begin(),order.end(),[&](auto a,auto b) {
      return inputs[a].definition->schema_object_uuid.value<inputs[b].definition->schema_object_uuid.value;
    });
    for(std::size_t i=1;i<order.size();++i)
      if(inputs[order[i-1]].definition->schema_object_uuid.value==inputs[order[i]].definition->schema_object_uuid.value)
        return Fail(E::duplicate_identity);
    const auto absent=std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> parents(inputs.size(),absent);
    for(std::size_t i=0;i<inputs.size();++i) {
      const auto& parent=inputs[i].definition->parent_schema_uuid;
      if(parent.value.is_nil())continue;
      const auto found=std::lower_bound(order.begin(),order.end(),parent.value,[&](auto index,const Uuid& id) {
        return inputs[index].definition->schema_object_uuid.value<id;
      });
      if(found==order.end()||inputs[*found].definition->schema_object_uuid.value!=parent.value)return Fail(E::missing_parent);
      parents[i]=*found;
    }
    std::vector<unsigned char> colors(inputs.size(),0);
    for(std::size_t i=0;i<inputs.size();++i) {
      auto at=i;
      while(at!=absent&&colors[at]==0){colors[at]=1;at=parents[at];}
      if(at!=absent&&colors[at]==1)return Fail(E::cyclic_parent);
      at=i;while(at!=absent&&colors[at]==1){colors[at]=2;at=parents[at];}
    }
    std::vector<CatalogMetadataVersion> definitions;definitions.reserve(inputs.size());
    for(const auto& input:inputs) {
      std::array<byte,248> payload;
      const auto encoded=EncodeCatalogSchemaDefinitionInto(*input.definition,payload);
      if(!encoded.ok())return Fail(E::invalid_definition);
      auto m=*input.metadata;m.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
      if(!CatalogSchemaDefinitionMatchesMetadata(m)||
          !CatalogMetadataVersionEncodedBytes(BorrowCatalogMetadataVersion(m)))return Fail(E::invalid_metadata);
      definitions.push_back(std::move(m));
    }
    std::vector<CatalogNamedCreationInput> names;names.reserve(inputs.size());
    for(std::size_t i=0;i<inputs.size();++i)
      names.push_back({&definitions[i],inputs[i].vector_metadata,inputs[i].vector,inputs[i].entries});
    auto assembled=AssembleLocalNamedCreationRecords(database,tx,language,names,limits);
    if(!assembled.ok())return Fail(NameError(assembled.error),assembled.error);
    // The owning database row/object are part of the same initial catalog even
    // though this returned family does not copy or rewrite that definition.
    for(const auto* records:{&assembled.records->definitions,&assembled.records->names})
      for(const auto& r:*records)for(const auto& id:{r.metadata.record.header.object_uuid,r.metadata.record.header.row_uuid})
        if(id.value==db.record.header.object_uuid.value||id.value==db.record.header.row_uuid.value)
          return Fail(E::duplicate_identity);
    return {E::none,CatalogNamedCreationError::none,std::move(assembled.records)};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
}
} // namespace scratchbird::core::catalog

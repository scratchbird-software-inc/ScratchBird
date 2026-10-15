// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_database_creation.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>

namespace scratchbird::storage::database {
namespace {
namespace c=core::catalog;
using namespace core::platform;
using E=NativeInitialDatabaseError;
NativeInitialDatabaseResult Fail(E e,c::CatalogNamedCreationError n=c::CatalogNamedCreationError::none) {
  return {e,n,{}};
}
E NameError(c::CatalogNamedCreationError e) {
  switch(e) {
    case c::CatalogNamedCreationError::none:return E::none;
    case c::CatalogNamedCreationError::invalid_request:return E::invalid_request;
    case c::CatalogNamedCreationError::invalid_metadata:return E::invalid_definition;
    case c::CatalogNamedCreationError::invalid_names:return E::invalid_names;
    case c::CatalogNamedCreationError::duplicate_identity:return E::duplicate_identity;
    case c::CatalogNamedCreationError::resource_exhausted:return E::resource_exhausted;
  }
  return E::invalid_request;
}
}
NativeInitialDatabaseResult AssembleNativeInitialDatabaseRecords(
    const NativeInitialDatabaseRequest& r) noexcept {
  try {
    const auto& named=r.definition;
    if(!r.construction||!named.definition||!named.vector_metadata||!named.vector||
        named.entries.empty()||!named.entries.data()||r.database_default_language.empty()||
        !r.database_default_language.data()||r.database_default_language.size()>131040||
        !r.limits.maximum_records||!r.limits.maximum_encoded_bytes)return Fail(E::invalid_request);
    const auto& physical=*r.construction;const auto& b=physical.bootstrap;
    if(disk::ValidateFilespaceBootstrap(b)!=disk::FilespaceBootstrapError::none||
        b.filespace_role!=1||b.lifecycle_state!=7||
        physical.creator.transaction_uuid.kind!=UuidKind::transaction||
        physical.creator.local_id.value!=1||
        physical.creator.scope!=transaction::mga::TransactionScope::local_node||
        !physical.creation_utc_millis||physical.creation_utc_millis>0xffffffffffffULL||
        !physical.total_pages)return Fail(E::invalid_construction);
    const std::array<Uuid,8> enclosing{b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid,
        b.checksum_profile_uuid,physical.operation_uuid,physical.writer_uuid,
        physical.creator.transaction_uuid.value,physical.policy_snapshot_uuid};
    for(std::size_t i=0;i<enclosing.size();++i) {
      if(!core::uuid::IsEngineIdentityUuid(enclosing[i]))return Fail(E::invalid_construction);
      for(std::size_t j=0;j<i;++j)if(enclosing[i]==enclosing[j])return Fail(E::invalid_construction);
    }
    const auto& metadata=*named.definition;
    if(metadata.record.header.kind!=c::CatalogRecordKind::database||
        metadata.object_subtype!="database"||!metadata.record.payload.empty()||
        !metadata.record.header.parent_uuid.value.is_nil()||!metadata.owning_schema_uuid.value.is_nil())
      return Fail(E::invalid_definition);
    c::CatalogDatabaseRecord definition;
    definition.database_uuid={UuidKind::database,b.database_uuid};
    // Native SBFP V1 has no minor revision. This catalog format is the native
    // six-relation construction contract, not the legacy SBDBV001 manifest.
    definition.database_header_format_major=b.durable_format_generation;
    definition.database_header_format_minor=0;
    definition.catalog_manifest_format_version=1;
    definition.page_size=b.page_size_bytes;
    definition.creation_unix_epoch_millis=physical.creation_utc_millis;
    definition.creator_transaction_number=physical.creator.local_id.value;
    definition.datatype_cohort=r.datatype_cohort;
    std::array<byte,232> backing;
    const auto encoded=c::EncodeCatalogDatabaseRecordInto(definition,backing);
    if(!encoded.ok())return Fail(E::invalid_definition);
    auto view=c::BorrowCatalogMetadataVersion(metadata);
    view.record.payload={reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size()};
    const auto checked=c::PreflightLocalNamedCreationRecord(view,named,
        physical.creator.transaction_uuid,r.database_default_language,r.limits);
    if(checked.error!=c::CatalogNamedCreationError::none)return Fail(NameError(checked.error),checked.error);
    if(named.vector->object_class!="database")return Fail(E::invalid_names);
    for(const auto& entry:named.entries)
      if(entry.entry->scope_uuid.kind!=UuidKind::object||
          entry.entry->scope_uuid.value!=metadata.record.header.object_uuid.value||entry.entry->parent_object_uuid||
          entry.entry->parent_schema_uuid)return Fail(E::invalid_names);
    // Do not allow any definition/name object or row to masquerade as the
    // primary, operation, issuer/source profile or actual construction owner.
    // Per-package duplicate admission remains in the shared named assembler.
    const auto distinct=[&](const c::CatalogMetadataVersion& m) {
      for(const auto& id:{m.record.header.object_uuid.value,m.record.header.row_uuid.value})
        if(id==r.datatype_cohort.catalog_snapshot_uuid.value||
            std::find(enclosing.begin(),enclosing.end(),id)!=enclosing.end())return false;
      return true;
    };
    if(!distinct(metadata)||!distinct(*named.vector_metadata))return Fail(E::duplicate_identity);
    for(const auto& entry:named.entries)if(!distinct(*entry.metadata))return Fail(E::duplicate_identity);
    if(checked.records>std::numeric_limits<std::size_t>::max()/2)return Fail(E::resource_exhausted);
    const auto count=static_cast<std::size_t>(2*checked.records);
    std::array<Uuid,64> small;
    std::vector<Uuid> large;
    if(count>small.size())large.resize(count);
    auto* ids=large.empty()?small.data():large.data();std::size_t at=0;
    const auto append=[&](const c::CatalogMetadataVersion& m) {
      ids[at++]=m.record.header.object_uuid.value;ids[at++]=m.record.header.row_uuid.value;
    };
    append(metadata);append(*named.vector_metadata);
    for(const auto& entry:named.entries)append(*entry.metadata);
    std::sort(ids,ids+count);
    if(std::adjacent_find(ids,ids+count)!=ids+count)return Fail(E::duplicate_identity);
    auto owned=metadata;
    owned.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
    const std::array<c::CatalogNamedCreationInput,1> inputs{{
        {&owned,named.vector_metadata,named.vector,named.entries}}};
    auto made=c::AssembleLocalNamedCreationRecords(b.database_uuid,physical.creator.transaction_uuid,
        r.database_default_language,inputs,r.limits);
    if(!made.ok())return Fail(NameError(made.error),made.error);
    return {E::none,c::CatalogNamedCreationError::none,std::move(made.records)};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
}
} // namespace scratchbird::storage::database

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_named_creation.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>

namespace scratchbird::core::catalog {
namespace {
using E=CatalogNamedCreationError;
CatalogNamedCreationResult Fail(E e){return {e,{}};}
template<class M> bool First(const M& m,const TypedUuid& tx) {
  return m.authority_scope==CatalogAuthorityScope::local&&m.definition_version==1&&
      m.schema_epoch==1&&m.security_epoch==1&&m.catalog_generation==1&&
      m.creator_local_transaction_id==1&&m.creator_transaction_uuid.kind==tx.kind&&
      m.creator_transaction_uuid.value==tx.value&&!m.record.header.deleted&&
      m.lifecycle==CatalogObjectLifecycle::active&&m.status==CatalogObjectStatus::active;
}
template<class F> void Each(std::span<const CatalogNamedCreationInput> inputs,F fn) {
  for(const auto& input:inputs) {
    fn(*input.definition);fn(*input.vector_metadata);
    for(const auto& entry:input.entries)fn(*entry.metadata);
  }
}
}
CatalogNamedCreationPreflightResult PreflightLocalNamedCreationRecord(
    const CatalogMetadataVersionView& d,const CatalogNamedCreationInput& input,
    const TypedUuid& tx,std::string_view language,CatalogNamedCreationLimits limits) noexcept {
  try {
    if(!uuid::IsEngineIdentityUuid(tx.value)||tx.kind!=UuidKind::transaction||
        language.empty()||language.size()>131040)
      return {E::invalid_request,0,0};
    if(!limits.maximum_records||!limits.maximum_encoded_bytes)
      return {E::resource_exhausted,0,0};
    u64 records=0,bytes=0;
    const auto account=[&](u64 n) {
      if(records==limits.maximum_records||n>limits.maximum_encoded_bytes-bytes)throw E::resource_exhausted;
      ++records;bytes+=n;
    };
      if(!input.vector_metadata||!input.vector||input.entries.empty())return {E::invalid_request,0,0};
      if(input.entries.size()>limits.maximum_records-records||limits.maximum_records-records-input.entries.size()<2)
        return {E::resource_exhausted,0,0};
      const auto& vm=*input.vector_metadata;const auto& v=*input.vector;
      if(!First(d,tx)||!First(vm,tx)||d.record.header.kind==CatalogRecordKind::localized_name||
          !vm.record.payload.empty())return {E::invalid_metadata,0,0};
      const auto extent=CatalogMetadataVersionEncodedBytes(d);
      const auto vector_extent=CatalogMetadataVersionEncodedBytes(BorrowCatalogMetadataVersion(vm));
      if(!extent||!vector_extent)return {E::invalid_metadata,0,0};
      const CatalogNamePayloadView vector=BorrowCatalogNameVector(v);
      const auto name_extent=CatalogNameEnvelopeEncodedBytes(vector);
      if(!name_extent||!CatalogNamePayloadMatchesMetadata(vector,BorrowCatalogMetadataVersion(vm))||
          v.lifecycle_state!=CatalogNameLifecycle::active||v.default_language_tag!=language||
          v.object_uuid.value!=d.record.header.object_uuid.value||
          v.name_vector_uuid.value!=d.name_vector_uuid.value||v.default_name_entry_uuid.value!=d.default_name_uuid.value||
          v.owning_schema_uuid.has_value()!=( !d.owning_schema_uuid.value.is_nil())||
          (v.owning_schema_uuid&&v.owning_schema_uuid->value!=d.owning_schema_uuid.value))return {E::invalid_names,0,0};
      account(*extent);account(*vector_extent+*name_extent);
      unsigned defaults=0;
      for(const auto& input_entry:input.entries) {
        if(!input_entry.metadata||!input_entry.entry)return {E::invalid_request,0,0};
        const auto& m=*input_entry.metadata;const auto& e=*input_entry.entry;
        if(!First(m,tx)||!m.record.payload.empty())return {E::invalid_metadata,0,0};
        const auto metadata_extent=CatalogMetadataVersionEncodedBytes(BorrowCatalogMetadataVersion(m));
        if(!metadata_extent)return {E::invalid_metadata,0,0};
        const CatalogNamePayloadView entry=BorrowCatalogNameEntry(e);
        const auto entry_extent=CatalogNameEnvelopeEncodedBytes(entry);
        const bool is_default=e.name_entry_uuid.value==v.default_name_entry_uuid.value;
        if(!entry_extent||!CatalogNamePayloadMatchesMetadata(entry,BorrowCatalogMetadataVersion(m))||
            e.lifecycle_state!=CatalogNameLifecycle::active||e.object_uuid.value!=d.record.header.object_uuid.value||
            e.name_vector_uuid.value!=v.name_vector_uuid.value||e.object_class!=v.object_class||
            e.default_for_object!=is_default||
            (is_default&&(e.language_tag!=language||!e.default_for_language)))return {E::invalid_names,0,0};
        defaults+=is_default;
        account(*metadata_extent+*entry_extent);
      }
      if(defaults!=1)return {E::invalid_names,0,0};

    return {E::none,records,bytes};
  }catch(E e){return {e,0,0};}
}
CatalogNamedCreationResult AssembleLocalNamedCreationRecords(
    const Uuid& database,const TypedUuid& tx,std::string_view language,
    std::span<const CatalogNamedCreationInput> inputs,CatalogNamedCreationLimits limits) noexcept {
  try {
    if(!uuid::IsEngineIdentityUuid(database)||tx.kind!=platform::UuidKind::transaction||
        !uuid::IsEngineIdentityUuid(tx.value)||database==tx.value||language.empty()||language.size()>131040||
        inputs.empty()||!limits.maximum_records||!limits.maximum_encoded_bytes)return Fail(E::invalid_request);
    if(inputs.size()>limits.maximum_records/3)return Fail(E::resource_exhausted);
    u64 records=0,bytes=0;
    for(const auto& input:inputs) {
      if(!input.definition)return Fail(E::invalid_request);
      const auto checked=PreflightLocalNamedCreationRecord(BorrowCatalogMetadataVersion(*input.definition),
          input,tx,language,{limits.maximum_records-records,limits.maximum_encoded_bytes-bytes});
      if(checked.error!=E::none)return Fail(checked.error);
      records+=checked.records;bytes+=checked.encoded_bytes;
    }
    // Sort exact raw16 identities, not names or an ambiguous digest. The small
    // bootstrap case needs no heap scratch. Larger cohorts use bounded scratch
    // and O(N log N) work, never quadratic repeated scans. Scratch is not a
    // grant, and no owning metadata/name copies precede identity validation.
    if(records>(std::numeric_limits<std::size_t>::max()-2)/2)return Fail(E::resource_exhausted);
    const auto identity_count=static_cast<std::size_t>(2*records+2);
    std::array<Uuid,256> small;
    std::vector<Uuid> large;
    if(identity_count>small.size())large.resize(identity_count);
    auto* identities=large.empty()?small.data():large.data();
    std::size_t at=0;identities[at++]=database;identities[at++]=tx.value;
    Each(inputs,[&](const auto& m) {
      identities[at++]=m.record.header.object_uuid.value;identities[at++]=m.record.header.row_uuid.value;
    });
    std::sort(identities,identities+identity_count);
    if(std::adjacent_find(identities,identities+identity_count)!=identities+identity_count)
      return Fail(E::duplicate_identity);
    CatalogNamedCreationSet out;out.encoded_bytes=bytes;
    out.definitions.reserve(inputs.size());out.names.reserve(records-inputs.size());
    for(const auto& input:inputs) {
      out.definitions.push_back({*input.definition,{}});
      out.names.push_back({*input.vector_metadata,*input.vector});
      for(const auto& entry:input.entries)out.names.push_back({*entry.metadata,*entry.entry});
    }
    return {E::none,std::move(out)};
  }catch(E e){return Fail(e);}
   catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
   catch(...){return Fail(E::invalid_request);}
}
} // namespace scratchbird::core::catalog

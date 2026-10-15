// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_policy_descriptor_creation.hpp"
#include "uuid.hpp"
#include <array>
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace scratchbird::storage::database {
namespace c=core::catalog;
using namespace core::platform;
using E=NativeInitialPolicyDescriptorError;
namespace {
NativeInitialPolicyDescriptorResult Fail(E e,c::CatalogNamedCreationError cause=c::CatalogNamedCreationError::none){return {e,cause,{}};}
E NameError(c::CatalogNamedCreationError e){
  switch(e){
    case c::CatalogNamedCreationError::none:return E::none;
    case c::CatalogNamedCreationError::invalid_request:return E::invalid_request;
    case c::CatalogNamedCreationError::invalid_metadata:return E::invalid_definition;
    case c::CatalogNamedCreationError::invalid_names:return E::invalid_names;
    case c::CatalogNamedCreationError::duplicate_identity:return E::duplicate_identity;
    case c::CatalogNamedCreationError::resource_exhausted:return E::resource_exhausted;
  }return E::invalid_request;
}
}
NativeInitialPolicyDescriptorResult AssembleNativeInitialPolicyDescriptorRecords(const NativeInitialPolicyDescriptorRequest& r) noexcept {
  try{
    if(!core::uuid::IsEngineIdentityUuid(r.database_uuid)||r.construction_transaction.kind!=UuidKind::transaction||
        !core::uuid::IsEngineIdentityUuid(r.construction_transaction.value)||r.database_uuid==r.construction_transaction.value||
        r.database_default_language.empty()||!r.database_default_language.data()||r.database_default_language.size()>131040||
        r.policies.empty()||!r.policies.data())return Fail(E::invalid_request);
    if(!r.limits.maximum_records||!r.limits.maximum_encoded_bytes||r.policies.size()>r.limits.maximum_records/3)
      return Fail(E::resource_exhausted);
    u64 records=0,bytes=0;
    for(const auto& input:r.policies){
      if(!input.descriptor||!input.named.definition)return Fail(E::invalid_request);
      const auto& p=*input.descriptor;const auto& m=*input.named.definition;
      if(p.generation!=1||p.database_uuid!=r.database_uuid||p.origin_transaction_uuid.value!=r.construction_transaction.value||
          p.origin_local_transaction_id!=1||p.creating_transaction_number!=1||p.cluster_uuid||
          p.effective_transaction_number||p.superseding_transaction_number||!m.record.payload.empty())return Fail(E::invalid_definition);
      std::array<byte,c::kCatalogPolicyDescriptorMaxBytes> backing;
      const auto encoded=c::EncodeCatalogPolicyDescriptorInto(p,backing);
      if(!encoded.ok())return Fail(E::invalid_definition);
      auto view=c::BorrowCatalogMetadataVersion(m);
      view.record.payload={reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size()};
      if(!c::CatalogPolicyDescriptorMatchesDatabase(view,r.database_uuid))return Fail(E::invalid_definition);
      const auto checked=c::PreflightLocalNamedCreationRecord(view,input.named,r.construction_transaction,
          r.database_default_language,{r.limits.maximum_records-records,r.limits.maximum_encoded_bytes-bytes});
      if(checked.error!=c::CatalogNamedCreationError::none)return Fail(NameError(checked.error),checked.error);
      if(input.named.vector->object_class!="policy")return Fail(E::invalid_names);
      records+=checked.records;bytes+=checked.encoded_bytes;
      // The enclosing node/transaction identity cannot become a new object/row.
      const auto distinct=[&](const c::CatalogMetadataVersion& value){
        for(const auto& id:{value.record.header.object_uuid.value,value.record.header.row_uuid.value})
          if(id==r.database_uuid||id==r.construction_transaction.value||id==p.policy_version_uuid)return false;
        return true;
      };
      if(!distinct(m)||!distinct(*input.named.vector_metadata))return Fail(E::duplicate_identity);
      for(const auto& entry:input.named.entries)if(!distinct(*entry.metadata))return Fail(E::duplicate_identity);
    }
    if(records>(std::numeric_limits<std::size_t>::max()-r.policies.size())/2)return Fail(E::resource_exhausted);
    const auto count=static_cast<std::size_t>(2*records+r.policies.size());
    std::array<Uuid,64> small;std::vector<Uuid> large;
    if(count>small.size())large.resize(count);
    auto* ids=large.empty()?small.data():large.data();std::size_t at=0;
    const auto append=[&](const c::CatalogMetadataVersion& m){ids[at++]=m.record.header.object_uuid.value;ids[at++]=m.record.header.row_uuid.value;};
    for(const auto& input:r.policies){ids[at++]=input.descriptor->policy_version_uuid;
      append(*input.named.definition);append(*input.named.vector_metadata);
      for(const auto& entry:input.named.entries)append(*entry.metadata);
    }
    std::sort(ids,ids+count);
    if(std::adjacent_find(ids,ids+count)!=ids+count)return Fail(E::duplicate_identity);
    // A typed body is resolved by the enclosing owning cohort, not any
    // descriptor/version/name/row emitted by this adapter. Shared body references
    // (including different explicitly selected generations) are not duplicate
    // definitions. Reuse the existing canonical index; no quadratic scan or
    // second scratch allocation is needed for these reference exclusions.
    for(const auto& input:r.policies)
      if(std::binary_search(ids,ids+count,input.descriptor->body_uuid))return Fail(E::duplicate_identity);
    // Exactly sized actual temporary owners; all borrowed definitions/names and
    // budgets were admitted above. Allocation failure returns no partial set.
    std::vector<c::CatalogMetadataVersion> owned(r.policies.size());
    std::vector<c::CatalogNamedCreationInput> named(r.policies.size());
    for(std::size_t i=0;i<r.policies.size();++i){
      const auto& input=r.policies[i];std::array<byte,c::kCatalogPolicyDescriptorMaxBytes> backing;
      const auto encoded=c::EncodeCatalogPolicyDescriptorInto(*input.descriptor,backing);
      if(!encoded.ok())return Fail(E::invalid_definition);
      owned[i]=*input.named.definition;owned[i].record.payload.assign(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size());
      named[i]=input.named;named[i].definition=&owned[i];
    }
    auto made=c::AssembleLocalNamedCreationRecords(r.database_uuid,r.construction_transaction,r.database_default_language,named,r.limits);
    if(!made.ok())return Fail(NameError(made.error),made.error);
    return {E::none,c::CatalogNamedCreationError::none,std::move(made.records)};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
}
} // namespace scratchbird::storage::database

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_verification_policy_creation.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>

namespace scratchbird::storage::database {
namespace c=core::catalog;using E=NativeInitialPolicyDescriptorError;
namespace {
NativeInitialPolicyDescriptorResult Fail(E error,c::CatalogNamedCreationError name=c::CatalogNamedCreationError::none){return {error,name,{}};}
E NameError(c::CatalogNamedCreationError e){switch(e){
  case c::CatalogNamedCreationError::none:return E::none;
  case c::CatalogNamedCreationError::invalid_request:return E::invalid_request;
  case c::CatalogNamedCreationError::invalid_metadata:return E::invalid_definition;
  case c::CatalogNamedCreationError::invalid_names:return E::invalid_names;
  case c::CatalogNamedCreationError::duplicate_identity:return E::duplicate_identity;
  case c::CatalogNamedCreationError::resource_exhausted:return E::resource_exhausted;
 }return E::invalid_request;}
}
NativeInitialPolicyDescriptorResult AssembleNativeInitialVerificationPolicyRecords(
    const NativeInitialVerificationPolicyRequest& r) noexcept {
  try{
    if(!core::uuid::IsEngineIdentityUuid(r.database_uuid)||r.construction_transaction.kind!=core::platform::UuidKind::transaction||
        !core::uuid::IsEngineIdentityUuid(r.construction_transaction.value)||r.database_uuid==r.construction_transaction.value||
        !c::IsKnownCatalogLanguage(r.database_default_language)||
        r.policies.empty()||!r.policies.data())return Fail(E::invalid_request);
    if(!r.limits.maximum_records||!r.limits.maximum_encoded_bytes||r.policies.size()>r.limits.maximum_records/3)
      return Fail(E::resource_exhausted);
    core::platform::u64 records=0,bytes=0;
    for(const auto& input:r.policies){
      if(!input.body||!input.named.definition)return Fail(E::invalid_request);
      const auto& p=*input.body;const auto& m=*input.named.definition;
      if(p.database_uuid!=r.database_uuid||p.generation!=1||p.origin_transaction_uuid.value!=r.construction_transaction.value||
          p.origin_local_transaction_id!=1||p.creating_local_transaction_id!=1||!m.record.payload.empty())return Fail(E::invalid_definition);
      std::array<core::platform::byte,c::kCatalogVerificationPolicyMaxBytes> backing;
      const auto encoded=c::EncodeCatalogVerificationPolicyInto(p,backing);if(!encoded.ok())return Fail(E::invalid_definition);
      auto view=c::BorrowCatalogMetadataVersion(m);view.record.payload={reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size()};
      if(!c::CatalogVerificationPolicyMatchesDatabase(view,r.database_uuid))return Fail(E::invalid_definition);
      const auto checked=c::PreflightLocalNamedCreationRecord(view,input.named,r.construction_transaction,r.database_default_language,
          {r.limits.maximum_records-records,r.limits.maximum_encoded_bytes-bytes});
      if(checked.error!=c::CatalogNamedCreationError::none)return Fail(NameError(checked.error),checked.error);
      if(input.named.vector->object_kind!=c::CatalogRecordKind::policy)return Fail(E::invalid_names);
      records+=checked.records;bytes+=checked.encoded_bytes;
    }
    if(records>std::numeric_limits<std::size_t>::max()/2)return Fail(E::resource_exhausted);
    std::vector<core::platform::Uuid> identities;identities.reserve(2*records);
    const auto append=[&](const auto& m){identities.push_back(m.record.header.object_uuid.value);identities.push_back(m.record.header.row_uuid.value);};
    for(const auto& input:r.policies){append(*input.named.definition);append(*input.named.vector_metadata);
      for(const auto& entry:input.named.entries)append(*entry.metadata);}
    std::sort(identities.begin(),identities.end());
    if(std::adjacent_find(identities.begin(),identities.end())!=identities.end())return Fail(E::duplicate_identity);
    for(const auto& input:r.policies){const auto& p=*input.body;
      for(const auto& id:{r.database_uuid,r.construction_transaction.value,p.evidence_policy_uuid,p.access_policy_uuid.value_or(core::platform::Uuid{})})
        if(!id.is_nil()&&std::binary_search(identities.begin(),identities.end(),id))return Fail(E::duplicate_identity);
    }
    std::vector<c::CatalogMetadataVersion> owned(r.policies.size());std::vector<c::CatalogNamedCreationInput> named(r.policies.size());
    for(std::size_t i=0;i<r.policies.size();++i){const auto& input=r.policies[i];std::array<core::platform::byte,c::kCatalogVerificationPolicyMaxBytes> backing;
      const auto encoded=c::EncodeCatalogVerificationPolicyInto(*input.body,backing);if(!encoded.ok())return Fail(E::invalid_definition);
      owned[i]=*input.named.definition;owned[i].record.payload.assign(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size());
      named[i]=input.named;named[i].definition=&owned[i];}
    auto made=c::AssembleLocalNamedCreationRecords(r.database_uuid,r.construction_transaction,r.database_default_language,named,r.limits);
    if(!made.ok())return Fail(NameError(made.error),made.error);return {E::none,c::CatalogNamedCreationError::none,std::move(made.records)};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
}
} // namespace scratchbird::storage::database

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_creation_workspace.hpp"
#include "datatype_binary.hpp"

namespace scratchbird::storage::database {
// Exact size of the encoded first-version row including its stable slot and
// direct-binary envelope. No heap allocation and no input materialization.
inline std::optional<core::platform::u64> NativeCreationCatalogRowBytes(
    const NativeCreationCatalogRecord& record) {
  auto bytes=core::catalog::CatalogMetadataVersionEncodedBytes(
    core::catalog::BorrowCatalogMetadataVersion(record.metadata));
  if(!bytes)return {};
  if(record.name) {
    const auto name=core::catalog::CatalogNameEnvelopeEncodedBytes(*record.name);
    if(!name)return {};
    *bytes+=*name;
  }
  return *bytes+144+16+core::datatypes::kDatatypeBinaryEnvelopeHeaderBytes+24;
}
// Reserve references, allowing repeated references and references to other
// definitions in this batch. Definition/row uniqueness is checked separately.
template<class Reserve> void ReserveNativeCreationCatalogReferences(
    const NativeCreationCatalogRecord& r,Reserve reserve) {
  const auto& m=r.metadata;
  for(const auto& id:{m.record.header.parent_uuid,m.owner_uuid,m.creator_transaction_uuid,
      m.retired_transaction_uuid,m.default_name_uuid,m.name_vector_uuid,m.security_policy_uuid,
      m.dependency_group_uuid,m.storage_binding_uuid,m.donor_overlay_uuid,m.audit_uuid,m.owning_schema_uuid})
    if(!id.value.is_nil())reserve(id.value);
  if(!r.name)return;
  std::visit([&](const auto& n){
    const auto optional=[&](const auto& id){if(id)reserve(id->value);};
    reserve(n.object_uuid.value);reserve(n.name_vector_uuid.value);reserve(n.security_policy_uuid.value);
    if constexpr(std::is_same_v<std::decay_t<decltype(n)>,core::catalog::CatalogNameVector>) {
      optional(n.owning_schema_uuid);reserve(n.default_name_entry_uuid.value);reserve(n.name_collision_policy_uuid.value);
    }else{
      reserve(n.name_entry_uuid.value);reserve(n.scope_uuid.value);optional(n.parent_object_uuid);optional(n.parent_schema_uuid);
      reserve(n.dialect_profile_uuid.value);reserve(n.identifier_profile_uuid.value);optional(n.case_fold_profile_uuid);
      optional(n.quoted_identifier_profile_uuid);reserve(n.created_transaction_uuid.value);optional(n.dropped_transaction_uuid);
    }
  },*r.name);
}
}  // namespace scratchbird::storage::database

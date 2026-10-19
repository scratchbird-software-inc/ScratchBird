// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_policy_descriptor.hpp"

namespace scratchbird::core::catalog {
enum class AuthorizationExternalMapping : u64 { disabled=1,internal_snapshot };
enum class AuthorizationStaleBehavior : u64 { deny=1,bounded_cached_snapshot };
enum class AuthorizationFailureBehavior : u64 { deny_request=1,disconnect };
// Complete declarations only. The owning evaluator resolves actual schemas,
// subjects, grants and row/column policies and retains committed activation.
struct CatalogAuthorizationPolicyBody {
  Uuid body_uuid;
  Uuid database_uuid;
  u64 generation=0;
  Uuid user_schema_uuid;
  Uuid role_schema_uuid;
  Uuid group_schema_uuid;
  AuthorizationExternalMapping external_mapping=AuthorizationExternalMapping::disabled;
  std::optional<Uuid> external_mapping_schema_uuid;
  Uuid grant_schema_uuid;
  Uuid row_policy_schema_uuid;
  Uuid column_policy_schema_uuid;
  u64 cache_lifetime_microseconds=0;
  u64 reload_trigger_mask=63;
  AuthorizationStaleBehavior stale_behavior=AuthorizationStaleBehavior::deny;
  u64 maximum_stale_microseconds=0;
  AuthorizationFailureBehavior failure_behavior=AuthorizationFailureBehavior::deny_request;
  u64 maximum_records=0;
  u64 maximum_membership_depth=0;
  u64 maximum_work=0;
  u64 flags=63;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
  TypedUuid creating_transaction_uuid;
  u64 creating_local_transaction_id=0;
  std::optional<Uuid> cluster_uuid;
};
inline constexpr u32 kCatalogAuthorizationPolicySchemaId=65623;
inline constexpr std::size_t kCatalogAuthorizationPolicyMaxBytes=520;
struct CatalogAuthorizationPolicyResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<CatalogAuthorizationPolicyBody> record;
  bool ok() const noexcept{return error==CatalogValueError::none&&record.has_value();}
};
std::optional<std::size_t> CatalogAuthorizationPolicyEncodedBytes(const CatalogAuthorizationPolicyBody&) noexcept;
CatalogValueEncodeIntoResult EncodeCatalogAuthorizationPolicyInto(const CatalogAuthorizationPolicyBody&,std::span<byte>) noexcept;
CatalogAuthorizationPolicyResult DecodeCatalogAuthorizationPolicy(std::string_view) noexcept;
bool IsCatalogAuthorizationPolicyPayload(std::string_view) noexcept;
bool CatalogAuthorizationPolicyMatchesHeader(const CatalogTypedRecordView&) noexcept;
bool CatalogAuthorizationPolicyMatchesMetadata(const CatalogMetadataVersionView&) noexcept;
bool CatalogAuthorizationPolicyMatchesDatabase(const CatalogMetadataVersionView&,const Uuid&) noexcept;
bool CatalogAuthorizationPolicyPreservesOrigin(const CatalogMetadataVersionView&,const CatalogMetadataVersionView&) noexcept;
bool CatalogAuthorizationPolicyMatchesDescriptor(const CatalogAuthorizationPolicyBody&,const CatalogPolicyDescriptor&) noexcept;
} // namespace scratchbird::core::catalog

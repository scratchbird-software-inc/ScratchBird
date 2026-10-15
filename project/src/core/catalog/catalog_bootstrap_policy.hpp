// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include <array>
#include <optional>

namespace scratchbird::core::catalog {
// Native definitions, not verified signatures, admission, current provider state
// or publication receipts. Text borrows immutable admitted backing. All counts
// describe the imported declaration, not observed durable effects.
enum class BootstrapProviderFamily : u64 {
  local_password=1,ldap_ad,oidc_jwt,saml,kerberos_pac,pam,radius,webauthn,
  certificate_mtls,workload_identity,managed_identity,custom_cpp_plugin
};
enum class BootstrapProviderAuthority : u64 {
  durable_catalog=1,external_provider_admission=2
};
enum class BootstrapVerifierPolicy : u64 { local_password_verifier_required=1 };
struct CatalogBootstrapPolicyPack {
  Uuid policy_pack_uuid;
  u64 generation=0;
  u64 imported_schema_version=0;
  std::string_view pack_id,pack_version,declared_signature_status;
  std::array<byte,32> content_sha256{};
  bool create_time_only=false,post_create_filesystem_authority=false,local_password_only=false;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
  // providers, enabled local-password, disabled external, roles, groups,
  // memberships, grants, profiles, default policies (in this fixed order).
  std::array<u64,9> declared_counts{};
};
struct CatalogBootstrapSecurityProvider {
  Uuid provider_uuid,policy_pack_uuid;
  u64 generation=0,policy_pack_generation=0;
  BootstrapProviderFamily family=static_cast<BootstrapProviderFamily>(0);
  BootstrapProviderAuthority authority=static_cast<BootstrapProviderAuthority>(0);
  bool enabled_by_default=false,external_provider=false;
  std::optional<BootstrapVerifierPolicy> verifier_policy;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
};
struct CatalogBootstrapPolicyProfile {
  Uuid profile_uuid,policy_pack_uuid;
  u64 generation=0,policy_pack_generation=0;
  std::string_view area,mode;
  // Actual materialized configuration definition, never a post-create path.
  std::optional<Uuid> resource_definition_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
};
template<class T> struct CatalogBootstrapPolicyResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<T> record;
  bool ok() const noexcept{return error==CatalogValueError::none&&record.has_value();}
};
CatalogValueEncodeIntoResult EncodeCatalogBootstrapPolicyPackInto(
    const CatalogBootstrapPolicyPack&,std::span<byte>);
CatalogValueEncodeIntoResult EncodeCatalogBootstrapSecurityProviderInto(
    const CatalogBootstrapSecurityProvider&,std::span<byte>);
CatalogValueEncodeIntoResult EncodeCatalogBootstrapPolicyProfileInto(
    const CatalogBootstrapPolicyProfile&,std::span<byte>);
CatalogBootstrapPolicyResult<CatalogBootstrapPolicyPack> DecodeCatalogBootstrapPolicyPack(std::string_view);
CatalogBootstrapPolicyResult<CatalogBootstrapSecurityProvider> DecodeCatalogBootstrapSecurityProvider(std::string_view);
CatalogBootstrapPolicyResult<CatalogBootstrapPolicyProfile> DecodeCatalogBootstrapPolicyProfile(std::string_view);
bool IsCatalogBootstrapPolicyPayload(std::string_view);
bool CatalogBootstrapPolicyMatchesHeader(const CatalogTypedRecordView&);
bool CatalogBootstrapPolicyMatchesMetadata(const CatalogMetadataVersionView&);
bool CatalogBootstrapPolicyPreservesOrigin(const CatalogMetadataVersionView&,const CatalogMetadataVersionView&);
} // namespace scratchbird::core::catalog

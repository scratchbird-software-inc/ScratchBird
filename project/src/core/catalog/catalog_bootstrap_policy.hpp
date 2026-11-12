// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include <array>
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
// Native definitions, not verified signatures, admission, current provider state
// or publication receipts. Identities, constants and values only. All counts
// describe the imported declaration, not observed durable effects.
enum class BootstrapProviderFamily : u64 {
  local_password=1,ldap_ad,oidc_jwt,saml,kerberos_pac,pam,radius,webauthn,
  certificate_mtls,workload_identity,managed_identity,custom_cpp_plugin
};
enum class BootstrapProviderAuthority : u64 {
  durable_catalog=1,external_provider_admission=2
};
enum class BootstrapVerifierPolicy : u64 { local_password_verifier_required=1 };
// 3b (SB_CATALOG_NO_TEXT_STEP3B): the policy pack's profile areas, profile modes
// and signature status are coding-time constants. The pack's text maps to them
// once, at import (the *FromImportText functions below), and an unknown value
// refuses; nothing defaults. Append only. The 14 areas are the complete set a
// pack must cover; the 14 modes are the modes the default pack's profiles use.
enum class BootstrapPolicyArea : u64 {
  security_provider_selection=1,standard_roles_groups_grants,default_security_posture,
  memory_resource_governance,storage_filespace_page_policy,transaction_mga_cleanup_archive_backup_forward,
  optimizer_statistics_feedback,index_maintenance,agent_policy,diagnostics,observability,
  unsupported_feature_behavior,cluster_boundary,release_default_configuration
};
inline constexpr u64 kBootstrapPolicyAreaCount=14;
enum class BootstrapPolicyMode : u64 {
  local_password_only=1,uuid_catalog_seed,deny_by_default,configured_policy_required,
  local_durable_fail_closed,mga_inventory_authority,catalog_backed_or_diagnostic_only,
  provider_admission_required,evidence_not_authority,stable_redacted_diagnostics,
  redacted_evidence_only,deterministic_fail_closed,external_provider_required,secure_defaults
};
inline constexpr u64 kBootstrapPolicyModeCount=14;
enum class BootstrapSignatureStatus : u64 { signature_ready_unsigned=1 };
// Import boundary only: the pack's exact text for each constant. Used where pack
// JSON is read, never on a catalog read path.
std::optional<BootstrapPolicyArea> BootstrapPolicyAreaFromImportText(std::string_view) noexcept;
std::optional<BootstrapPolicyMode> BootstrapPolicyModeFromImportText(std::string_view) noexcept;
std::optional<BootstrapSignatureStatus> BootstrapSignatureStatusFromImportText(std::string_view) noexcept;
struct CatalogBootstrapPolicyPack {
  Uuid policy_pack_uuid;
  u64 generation=0;
  u64 imported_schema_version=0;
  // 3b: schema version 2. The pack's text id is its reference name in the
  // names table (native create); its version string is not carried (only ever
  // checked to be nonempty). Version-1 tags 3 and 4 are retired.
  BootstrapSignatureStatus declared_signature_status=static_cast<BootstrapSignatureStatus>(0);
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
  // 3b: schema version 2; constants, not text.
  BootstrapPolicyArea area=static_cast<BootstrapPolicyArea>(0);
  BootstrapPolicyMode mode=static_cast<BootstrapPolicyMode>(0);
  // Actual materialized configuration definition, never a post-create path.
  std::optional<Uuid> resource_definition_uuid;
  std::optional<u64> resource_definition_generation;
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

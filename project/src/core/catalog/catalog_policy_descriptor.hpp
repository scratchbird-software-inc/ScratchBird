// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"

namespace scratchbird::core::catalog {
enum class CatalogPolicyClass : u64 {
  authentication=1,authorization,connection,transaction,statement,parser_cache,
  sblr_binding,udr_trust,dml,read,metadata,lock_admission,backup_restore,archive,
  cleanup_retention,verification_repair,placement_cluster,audit,evidence,
  disclosure_redaction,support,legal_hold,donor_compatibility
};
enum class CatalogPolicyScope : u64 {
  database=1,schema,object,relation,column,row_predicate,domain,udr_package,
  parser_package,user,role,group,external_authorization,connection,transaction,
  statement,filespace,archive,backup_restore,cluster_member,placement_unit
};
enum class CatalogPolicyDefault : u64 { allow=1,deny,defer,require_higher_authority,require_human_review };
enum class CatalogPolicyException : u64 { deny=1,fail_closed,fail_open_if_policy_allows,defer_to_higher_authority,quarantine };
enum class CatalogPolicyRecovery : u64 { rebuild_cache=1,require_revalidation,quarantine_on_uncertain,deny_on_uncertain };
enum class CatalogPolicyActivation : u64 { new_connection=1,new_transaction,new_statement,immediate_if_safe,scheduled_job,cluster_epoch };
// Declaration only, not policy evaluation, permission, committed MGA state or
// activation. No text UUID/numeric carrier and no guessed scope/profile owner.
struct CatalogPolicyDescriptor {
  Uuid policy_uuid,policy_version_uuid,database_uuid;
  // Exact class-specific body object and semantic definition generation, not
  // the physical MGA version UUID issued later by storage. Neither a display
  // name nor a coincident policy generation can select or activate a body.
  Uuid body_uuid;
  u64 body_generation=0;
  u64 generation=0;
  CatalogPolicyClass policy_class=static_cast<CatalogPolicyClass>(0);
  u64 creating_transaction_number=0;
  std::optional<u64> effective_transaction_number,superseding_transaction_number;
  CatalogPolicyScope scope=static_cast<CatalogPolicyScope>(0);
  Uuid scope_uuid,authority_owner_uuid;
  u16 precedence_rank=0;
  CatalogPolicyDefault default_decision=CatalogPolicyDefault::deny;
  CatalogPolicyException exception_behavior=CatalogPolicyException::fail_closed;
  std::optional<Uuid> evidence_profile_uuid,disclosure_profile_uuid,retention_profile_uuid;
  CatalogPolicyRecovery recovery_behavior=CatalogPolicyRecovery::deny_on_uncertain;
  CatalogPolicyActivation activation_boundary=CatalogPolicyActivation::new_transaction;
  u64 cache_invalidation_mask=0,enforcement_points=0;
  std::optional<Uuid> cluster_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
  TypedUuid creating_transaction_uuid;
};
inline constexpr u16 kCatalogPolicyDescriptorSchemaVersion=2;
inline constexpr std::size_t kCatalogPolicyDescriptorMaxBytes=552;
inline constexpr u64 kCatalogPolicyEnforcementMask=(u64{1}<<25)-1;
inline constexpr u64 kCatalogPolicyInvalidationMask=255;
struct CatalogPolicyDescriptorResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<CatalogPolicyDescriptor> record;
  bool ok()const noexcept{return error==CatalogValueError::none&&record.has_value();}
};
std::optional<std::size_t> CatalogPolicyDescriptorEncodedBytes(const CatalogPolicyDescriptor&);
CatalogValueEncodeIntoResult EncodeCatalogPolicyDescriptorInto(const CatalogPolicyDescriptor&,std::span<byte>);
CatalogPolicyDescriptorResult DecodeCatalogPolicyDescriptor(std::string_view);
bool IsCatalogPolicyDescriptorPayload(std::string_view);
bool CatalogPolicyDescriptorMatchesHeader(const CatalogTypedRecordView&);
bool CatalogPolicyDescriptorMatchesMetadata(const CatalogMetadataVersionView&);
bool CatalogPolicyDescriptorMatchesDatabase(const CatalogMetadataVersionView&,const Uuid&);
bool CatalogPolicyDescriptorPreservesOrigin(const CatalogMetadataVersionView&,const CatalogMetadataVersionView&);
} // namespace scratchbird::core::catalog

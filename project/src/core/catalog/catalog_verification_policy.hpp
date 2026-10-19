// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include "catalog_policy_descriptor.hpp"

namespace scratchbird::core::catalog {
enum class VerificationPolicyRole : u64 { repair=1,quarantine,salvage };
enum class VerificationRepairAction : u64 {
  no_repair_needed=1,rebuild_derived_structure,refresh_statistics,rebuild_index,
  rebuild_dependency_graph,rebuild_parser_cache,recompute_page_visibility_summary,
  reconcile_allocation_map,complete_recoverable_operation,rollback_recoverable_operation,
  verify_archive_again,quarantine_object,quarantine_page,quarantine_filespace,
  salvage_readable_versions,require_restore,require_cluster_resolution,administrator_review_required
};
inline constexpr u64 kVerificationPolicyAllowMutation=1,kVerificationPolicyAllowOnlineMutation=2,
  kVerificationPolicyAllowUnknownScan=4,kVerificationPolicyMandatoryFlags=0xf8,
  kVerificationPolicyMarkSalvagedOutput=0x100,kVerificationPolicyFlagMask=0x1ff;
inline constexpr u64 VerificationActionBit(VerificationRepairAction action) noexcept {
  const auto n=static_cast<u64>(action);return n>=1&&n<=18?u64{1}<<(n-1):0;
}
inline constexpr u64 kVerificationNonmutatingActions=(u64{1}<<0)|(u64{1}<<10)|(u64{7}<<15);
// Persisted class-specific limits and action rules. They are not a memory
// grant, reference proof, activation, approval or permission to mutate data.
struct CatalogVerificationPolicyBody {
  Uuid body_uuid,database_uuid;
  u64 generation=0;
  VerificationPolicyRole role=VerificationPolicyRole::repair;
  u64 verification_class_mask=0,authority_class_mask=0,verification_level_mask=0;
  u64 allowed_action_mask=kVerificationNonmutatingActions,review_action_mask=0;
  VerificationRepairAction uncertain_action=VerificationRepairAction::administrator_review_required;
  u64 maximum_scan_pages=0,maximum_work_bytes=0,maximum_findings=0;
  Uuid evidence_policy_uuid;
  std::optional<Uuid> access_policy_uuid;
  u64 flags=kVerificationPolicyMandatoryFlags;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
  TypedUuid creating_transaction_uuid;
  u64 creating_local_transaction_id=0;
};
inline constexpr u32 kCatalogVerificationPolicySchemaId=65621;
inline constexpr u16 kCatalogVerificationPolicySchemaVersion=1;
inline constexpr std::size_t kCatalogVerificationPolicyMaxBytes=400;
struct CatalogVerificationPolicyResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<CatalogVerificationPolicyBody> record;
  bool ok()const noexcept{return error==CatalogValueError::none&&record.has_value();}
};
std::optional<std::size_t> CatalogVerificationPolicyEncodedBytes(const CatalogVerificationPolicyBody&) noexcept;
CatalogValueEncodeIntoResult EncodeCatalogVerificationPolicyInto(const CatalogVerificationPolicyBody&,std::span<byte>) noexcept;
CatalogVerificationPolicyResult DecodeCatalogVerificationPolicy(std::string_view) noexcept;
bool IsCatalogVerificationPolicyPayload(std::string_view) noexcept;
bool CatalogVerificationPolicyMatchesHeader(const CatalogTypedRecordView&) noexcept;
bool CatalogVerificationPolicyMatchesMetadata(const CatalogMetadataVersionView&) noexcept;
bool CatalogVerificationPolicyMatchesDatabase(const CatalogMetadataVersionView&,const Uuid&) noexcept;
bool CatalogVerificationPolicyPreservesOrigin(const CatalogMetadataVersionView&,const CatalogMetadataVersionView&) noexcept;
// Exact declaration binding only. The owning resolver must still acquire both
// rows, resolve their references and retain actual committed activation.
bool CatalogVerificationPolicyMatchesDescriptor(const CatalogVerificationPolicyBody&,
    const CatalogPolicyDescriptor&,VerificationPolicyRole expected_role) noexcept;
} // namespace scratchbird::core::catalog

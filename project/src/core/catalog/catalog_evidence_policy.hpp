// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_policy_descriptor.hpp"
#include <array>

namespace scratchbird::core::catalog {
enum class EvidenceRetentionClass : u64 {
  hot_current=1,historical,archive_required,legal_hold,audit,support,
  backup_forward,cluster_recovery,quarantine,salvage
};
enum class EvidenceRedactionRule : u64 {cleartext=1,redacted,structured_redaction,digest_only,encrypted,external_reference};
enum class EvidenceDigestRule : u64 {none=1,sha256};
enum class EvidenceExportRule : u64 {deny=1,authorized_request_only};
enum class EvidenceCustodyRule : u64 {internal_mga=1,record_every_transfer};
enum class EvidenceReplicationRule : u64 {standalone=1,cluster_replicated,cluster_quorum};
struct CatalogEvidenceStringSet {
  std::array<std::string_view,64> entries{};
  std::size_t count=0;
};
// Borrowed, exact class-body declarations. Strings retain their actual input
// owner; these rules grant neither policy activation nor evidence-write success.
struct CatalogEvidencePolicyBody {
  Uuid body_uuid,database_uuid;
  u64 generation=0;
  CatalogEvidenceStringSet required_evidence_classes,required_search_keys;
  Uuid storage_filespace_uuid;
  EvidenceRetentionClass retention_class=EvidenceRetentionClass::audit;
  EvidenceRedactionRule redaction_rule=EvidenceRedactionRule::structured_redaction;
  EvidenceDigestRule digest_rule=EvidenceDigestRule::sha256;
  EvidenceExportRule external_export_rule=EvidenceExportRule::deny;
  EvidenceCustodyRule chain_of_custody_rule=EvidenceCustodyRule::internal_mga;
  EvidenceReplicationRule cluster_replication_rule=EvidenceReplicationRule::standalone;
  std::optional<Uuid> cluster_uuid;
  u64 maximum_record_bytes=0,maximum_batch_bytes=0,flags=31;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
  TypedUuid creating_transaction_uuid;
  u64 creating_local_transaction_id=0;
};
inline constexpr u32 kCatalogEvidencePolicySchemaId=65622;
inline constexpr std::size_t kCatalogEvidencePolicyMaxBytes=17280;
struct CatalogEvidencePolicyResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<CatalogEvidencePolicyBody> record;
  bool ok()const noexcept{return error==CatalogValueError::none&&record.has_value();}
};
std::optional<std::size_t> CatalogEvidencePolicyEncodedBytes(const CatalogEvidencePolicyBody&) noexcept;
CatalogValueEncodeIntoResult EncodeCatalogEvidencePolicyInto(const CatalogEvidencePolicyBody&,std::span<byte>) noexcept;
CatalogEvidencePolicyResult DecodeCatalogEvidencePolicy(std::string_view) noexcept;
bool IsCatalogEvidencePolicyPayload(std::string_view) noexcept;
bool CatalogEvidencePolicyMatchesHeader(const CatalogTypedRecordView&) noexcept;
bool CatalogEvidencePolicyMatchesMetadata(const CatalogMetadataVersionView&) noexcept;
bool CatalogEvidencePolicyMatchesDatabase(const CatalogMetadataVersionView&,const Uuid&) noexcept;
bool CatalogEvidencePolicyPreservesOrigin(const CatalogMetadataVersionView&,const CatalogMetadataVersionView&) noexcept;
bool CatalogEvidencePolicyMatchesDescriptor(const CatalogEvidencePolicyBody&,const CatalogPolicyDescriptor&) noexcept;
} // namespace scratchbird::core::catalog

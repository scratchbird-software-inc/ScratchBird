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
// 3b (SB_CATALOG_NO_TEXT_STEP3B): an evidence class is a coding-time constant.
// The policy pack's text maps to it once, at import, and an unknown class
// refuses. Append only; the value is the class's bit position plus one.
enum class EvidenceClass : u64 { verification=1 };
inline constexpr u64 kCatalogEvidenceClassMask=u64{1}<<(static_cast<u64>(EvidenceClass::verification)-1);
constexpr u64 CatalogEvidenceClassBit(EvidenceClass c) noexcept {return u64{1}<<(static_cast<u64>(c)-1);}
// Exact class-body declarations: identities, constants and values only. These
// rules grant neither policy activation nor evidence-write success.
// 3b: schema version 2. required_evidence_classes is a closed bit set of
// EvidenceClass (version 1 carried a text set). required_search_keys (version
// 1, tag 5) is retired: like the envelope's trace_search_key (owner Q57) it
// was an implementation trace key, only validated and copied by its readers.
struct CatalogEvidencePolicyBody {
  Uuid body_uuid,database_uuid;
  u64 generation=0;
  u64 required_evidence_classes=0;
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
inline constexpr std::size_t kCatalogEvidencePolicyMaxBytes=376;
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

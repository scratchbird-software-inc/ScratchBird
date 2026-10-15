// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"

namespace scratchbird::core::catalog {
enum class ConstructionAuditEvent : u64 { construction_intent=1 };
enum class ConstructionAuditClass : u64 { bootstrap_construction=1 };
enum class ConstructionAuditDecision : u64 { not_evaluated=1 };
enum class ConstructionAuditRedaction : u64 { safe_message_only=1 };
// Immutable evidence of intent only. No policy generation, invented policy
// decision, snapshot, completed effect or permission is encoded by this family.
struct CatalogConstructionIntent {
  Uuid event_uuid,database_uuid,engine_uuid,operation_uuid,filespace_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0,event_time_utc_us=0,catalog_generation=0;
  ConstructionAuditEvent event=static_cast<ConstructionAuditEvent>(0);
  ConstructionAuditClass event_class=static_cast<ConstructionAuditClass>(0);
  ConstructionAuditDecision policy_decision=static_cast<ConstructionAuditDecision>(0);
  ConstructionAuditRedaction redaction=static_cast<ConstructionAuditRedaction>(0);
  std::optional<TypedUuid> actor_uuid;
};
inline constexpr std::size_t kCatalogConstructionIntentMaxBytes=304;
struct CatalogConstructionIntentResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<CatalogConstructionIntent> record;
  bool ok()const noexcept{return error==CatalogValueError::none&&record.has_value();}
};
// Allocation-free, including first use. Caller retains immutable inputs and
// admitted backing; encoding alone is not authentication/publication authority.
std::optional<std::size_t> CatalogConstructionIntentEncodedBytes(const CatalogConstructionIntent&);
CatalogValueEncodeIntoResult EncodeCatalogConstructionIntentInto(const CatalogConstructionIntent&,std::span<byte>);
CatalogConstructionIntentResult DecodeCatalogConstructionIntent(std::string_view);
bool IsCatalogConstructionIntentPayload(std::string_view);
bool CatalogConstructionIntentMatchesHeader(const CatalogTypedRecordView&);
bool CatalogConstructionIntentMatchesMetadata(const CatalogMetadataVersionView&);
bool CatalogConstructionIntentMatchesDatabase(const CatalogMetadataVersionView&,const Uuid&);
bool CatalogConstructionIntentPreservesOrigin(const CatalogMetadataVersionView&,const CatalogMetadataVersionView&);
} // namespace scratchbird::core::catalog

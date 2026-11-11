// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"

namespace scratchbird::core::catalog {
// SEARCH_KEY: SB_AUDIT_CATALOG_METADATA_CHANGE (owner Q26 = 1, Q31 = 1, Q49 = 1,
// Q50 = 1). The registered catalog.metadata_change audit event: one per
// transaction that changes the catalog's system tables (definition rows),
// written by that transaction, immutable. Every definition row version the
// transaction writes cites it (audit_uuid); the objects it covers are those
// citing rows. Name rows (the UUID-to-name lookup table) are data and are not
// bound to it (Q50). No policy was evaluated (Q49 = 1: authorization is not yet
// wired to catalog writes): policy_decision is not_evaluated and no policy
// generation or evaluated policy is encoded, never a synthesized one.
enum class MetadataChangeAuditEvent : u64 { metadata_change = 1 };
enum class MetadataChangeAuditClass : u64 { metadata_change = 1 };
enum class MetadataChangeAuditDecision : u64 { not_evaluated = 1 };
enum class MetadataChangeAuditRedaction : u64 { safe_message_only = 1 };
struct CatalogMetadataChange {
  Uuid event_uuid, database_uuid, engine_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0, event_time_utc_us = 0, catalog_generation = 0;
  MetadataChangeAuditEvent event = static_cast<MetadataChangeAuditEvent>(0);
  MetadataChangeAuditClass event_class = static_cast<MetadataChangeAuditClass>(0);
  MetadataChangeAuditDecision policy_decision = static_cast<MetadataChangeAuditDecision>(0);
  MetadataChangeAuditRedaction redaction = static_cast<MetadataChangeAuditRedaction>(0);
  std::optional<TypedUuid> actor_uuid;
};
inline constexpr u32 kCatalogMetadataChangeSchemaId = 65634;
inline constexpr std::size_t kCatalogMetadataChangeMaxBytes = 256;
struct CatalogMetadataChangeResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogMetadataChange> record;
  bool ok() const noexcept { return error == CatalogValueError::none && record.has_value(); }
};
// Allocation-free. Encoding alone is not authorization or publication.
std::optional<std::size_t> CatalogMetadataChangeEncodedBytes(const CatalogMetadataChange&);
CatalogValueEncodeIntoResult EncodeCatalogMetadataChangeInto(const CatalogMetadataChange&, std::span<byte>);
CatalogMetadataChangeResult DecodeCatalogMetadataChange(std::string_view);
bool IsCatalogMetadataChangePayload(std::string_view);
bool CatalogMetadataChangeMatchesHeader(const CatalogTypedRecordView&);
// The row binds its event: kind audit_evidence, subtype catalog_metadata_change,
// object = the event, local, unnamed, its own audit anchor, catalog generation
// the payload's, and a first version created by the origin transaction.
bool CatalogMetadataChangeMatchesMetadata(const CatalogMetadataVersionView&);
bool CatalogMetadataChangeMatchesDatabase(const CatalogMetadataVersionView&, const Uuid&);
// The event is immutable: a successor keeps its payload.
bool CatalogMetadataChangePreservesOrigin(const CatalogMetadataVersionView&, const CatalogMetadataVersionView&);
}  // namespace scratchbird::core::catalog

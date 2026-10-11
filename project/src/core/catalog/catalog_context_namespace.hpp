// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"

namespace scratchbird::core::catalog {
// Persistent namespace definition only. Names/grants have their own catalog
// records; keys and values stay with their session/transaction retention owner.
struct CatalogContextNamespace {
  Uuid namespace_uuid;
  u64 generation = 0;
  Uuid database_uuid;
  Uuid schema_uuid;
  Uuid owner_uuid;
  Uuid security_label_uuid;
  Uuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  bool operator==(const CatalogContextNamespace&) const = default;
};
struct CatalogContextNamespaceResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogContextNamespace> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
// Decode uses fixed caller-stack backing and returns only inline binary/scalar
// fields: no heap, borrowed payload lifetime, or authorization implied.
CatalogContextNamespaceResult DecodeCatalogContextNamespace(std::string_view);
CatalogValueEncodeResult EncodeCatalogContextNamespace(const CatalogContextNamespace&);
bool IsCatalogContextNamespacePayload(std::string_view);
bool CatalogContextNamespaceMatchesHeader(const CatalogTypedRecordView&);
bool CatalogContextNamespaceMatchesMetadata(const CatalogMetadataVersionView&);
// Additional physical-residency constraint. Unrelated families pass through;
// namespace records must retain exact binary node identity. This is not live
// visibility or security admission. Safe in allocation-free leaf readers.
bool CatalogContextNamespaceMatchesDatabase(const CatalogMetadataVersionView&, const Uuid&);
bool CatalogContextNamespacePreservesOrigin(const CatalogMetadataVersionView&,
                                          const CatalogMetadataVersionView&);
}  // namespace scratchbird::core::catalog

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"

#include <map>
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {

// BOOTSTRAP-SECURITY-NATIVE-CATALOG-PAYLOAD-V1. Physical catalog references
// use the object domain; logical principal classification belongs to readers.
// These attributes are not a credential, grant, or transaction authority proof.
struct CatalogSecurityRecord {
  CatalogRecordKind kind = CatalogRecordKind::unknown;
  std::map<std::string, Uuid> identities;
  std::map<std::string, std::string> attributes;

  Uuid Identity(std::string_view name) const;
};

struct CatalogSecurityRecordDecodeResult {
  CatalogValueError error = CatalogValueError::none;
  std::optional<CatalogSecurityRecord> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};

bool IsCatalogSecurityRecordKind(CatalogRecordKind kind);
std::string_view CatalogSecurityPrimaryIdentityName(CatalogRecordKind kind);
const CatalogValueSchema& CatalogSecurityRecordSchema(CatalogRecordKind kind);
CatalogValueEncodeResult EncodeCatalogSecurityRecord(const CatalogSecurityRecord& record);
CatalogSecurityRecordDecodeResult DecodeCatalogSecurityRecord(
    CatalogRecordKind kind, std::string_view bytes);
bool CatalogSecurityPayloadMatchesHeader(const CatalogTypedRecord& record);

}  // namespace scratchbird::core::catalog

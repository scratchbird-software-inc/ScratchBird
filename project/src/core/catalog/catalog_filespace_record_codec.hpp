// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog_value_codec.hpp"
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
// CATALOG_FILESPACE_BINARY_PAYLOAD_V1. Manifest flags are declarations,
// not evidence that their corresponding runtime operations have executed.
struct CatalogFilespaceRecord {
  TypedUuid filespace_uuid{};
  TypedUuid database_uuid{};
  u64 filespace_role = 0;
  bool first_filespace = false;
  bool startup_authority = false;
  bool catalog_persistence_owner = false;
  bool filespace_manifest_owner = false;
  bool recovery_evidence_owner = false;
  bool read_only = false;
  u64 state = 0;
  u64 physical_filespace_id = 0;
  u64 lifecycle_generation = 0;
  u64 filespace_manifest_generation = 0;
  u64 catalog_manifest_format_version = 0;
  u64 resource_seed_manifest_format_version = 0;
  u64 registered_txn = 0;
  u64 last_lifecycle_transaction = 0;
  u64 uuid_source = 0;
  bool header_database_uuid_match_required = false;
  bool header_filespace_uuid_match_required = false;
  bool startup_state_coupled = false;
  bool page_header_coupled = false;
  bool open_validate_header = false;
  bool attach_admission_validate_header = false;
  bool transaction_admission_validate_filespace = false;
  bool maintenance_validate_header = false;
  bool verify_repair_validate_header = false;
  bool shutdown_validate_header = false;
  bool recovery_validate_header = false;
  bool drop_requires_database_lifecycle = false;
  bool quarantine_on_ambiguous = false;
  bool state_change_evidence_before_success = false;
  bool mga_visibility_required = false;
  bool path_is_locator_not_identity = false;
  bool duplicate_identity_refusal = false;
  bool stale_identity_refusal = false;
  u64 creator_transaction_number = 0;
};
struct CatalogFilespaceRecordDecodeResult {
  CatalogValueError error = CatalogValueError::none;
  std::optional<CatalogFilespaceRecord> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
const CatalogValueSchema& CatalogFilespaceRecordSchema();
CatalogValueEncodeResult EncodeCatalogFilespaceRecord(const CatalogFilespaceRecord& record);
CatalogFilespaceRecordDecodeResult DecodeCatalogFilespaceRecord(std::string_view bytes);
}  // namespace scratchbird::core::catalog

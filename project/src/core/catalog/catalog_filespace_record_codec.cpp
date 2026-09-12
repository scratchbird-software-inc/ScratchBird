// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_filespace_record_codec.hpp"
#include <limits>

namespace scratchbird::core::catalog {
namespace {
bool Valid(const CatalogFilespaceRecord& r) {
  constexpr auto max = std::numeric_limits<u32>::max();
  return r.filespace_role >= 1 && r.filespace_role <= 16 &&
      r.state >= 1 && r.state <= 16 && r.uuid_source == 1 &&
      r.physical_filespace_id <= max &&
      r.lifecycle_generation > 0 && r.filespace_manifest_generation > 0 &&
      r.catalog_manifest_format_version > 0 && r.catalog_manifest_format_version <= max &&
      r.resource_seed_manifest_format_version > 0 && r.resource_seed_manifest_format_version <= max &&
      r.registered_txn > 0 && r.last_lifecycle_transaction > 0 &&
      r.creator_transaction_number > 0;
}
}  // namespace
const CatalogValueSchema& CatalogFilespaceRecordSchema() {
  using T = CatalogValueType;
  static const CatalogValueSchema schema{65538, 1, {
      {1, T::engine_identity, true, 16, UuidKind::filespace},
      {2, T::engine_identity, true, 16, UuidKind::database},
      {3, T::unsigned_integer, true, 8},
      {4, T::boolean, true, 1},
      {5, T::boolean, true, 1},
      {6, T::boolean, true, 1},
      {7, T::boolean, true, 1},
      {8, T::boolean, true, 1},
      {9, T::boolean, true, 1},
      {10, T::unsigned_integer, true, 8},
      {11, T::unsigned_integer, true, 8},
      {12, T::unsigned_integer, true, 8},
      {13, T::unsigned_integer, true, 8},
      {14, T::unsigned_integer, true, 8},
      {15, T::unsigned_integer, true, 8},
      {16, T::unsigned_integer, true, 8},
      {17, T::unsigned_integer, true, 8},
      {18, T::unsigned_integer, true, 8},
      {19, T::boolean, true, 1},
      {20, T::boolean, true, 1},
      {21, T::boolean, true, 1},
      {22, T::boolean, true, 1},
      {23, T::boolean, true, 1},
      {24, T::boolean, true, 1},
      {25, T::boolean, true, 1},
      {26, T::boolean, true, 1},
      {27, T::boolean, true, 1},
      {28, T::boolean, true, 1},
      {29, T::boolean, true, 1},
      {30, T::boolean, true, 1},
      {31, T::boolean, true, 1},
      {32, T::boolean, true, 1},
      {33, T::boolean, true, 1},
      {34, T::boolean, true, 1},
      {35, T::boolean, true, 1},
      {36, T::boolean, true, 1},
      {37, T::unsigned_integer, true, 8},
  }};
  return schema;
}
CatalogValueEncodeResult EncodeCatalogFilespaceRecord(const CatalogFilespaceRecord& r) {
  if (!Valid(r)) return {CatalogValueError::invalid_value, {}};
  return EncodeCatalogValueBlock(CatalogFilespaceRecordSchema(), {
      {1, r.filespace_uuid},
      {2, r.database_uuid},
      {3, r.filespace_role},
      {4, r.first_filespace},
      {5, r.startup_authority},
      {6, r.catalog_persistence_owner},
      {7, r.filespace_manifest_owner},
      {8, r.recovery_evidence_owner},
      {9, r.read_only},
      {10, r.state},
      {11, r.physical_filespace_id},
      {12, r.lifecycle_generation},
      {13, r.filespace_manifest_generation},
      {14, r.catalog_manifest_format_version},
      {15, r.resource_seed_manifest_format_version},
      {16, r.registered_txn},
      {17, r.last_lifecycle_transaction},
      {18, r.uuid_source},
      {19, r.header_database_uuid_match_required},
      {20, r.header_filespace_uuid_match_required},
      {21, r.startup_state_coupled},
      {22, r.page_header_coupled},
      {23, r.open_validate_header},
      {24, r.attach_admission_validate_header},
      {25, r.transaction_admission_validate_filespace},
      {26, r.maintenance_validate_header},
      {27, r.verify_repair_validate_header},
      {28, r.shutdown_validate_header},
      {29, r.recovery_validate_header},
      {30, r.drop_requires_database_lifecycle},
      {31, r.quarantine_on_ambiguous},
      {32, r.state_change_evidence_before_success},
      {33, r.mga_visibility_required},
      {34, r.path_is_locator_not_identity},
      {35, r.duplicate_identity_refusal},
      {36, r.stale_identity_refusal},
      {37, r.creator_transaction_number},
  });
}
CatalogFilespaceRecordDecodeResult DecodeCatalogFilespaceRecord(std::string_view bytes) {
  if (bytes.size() != 464) return {CatalogValueError::invalid_framing, {}};
  const std::vector<byte> input(bytes.begin(), bytes.end());
  const auto decoded = DecodeCatalogValueBlock(CatalogFilespaceRecordSchema(), input);
  if (!decoded.ok()) return {decoded.error, {}};
  CatalogFilespaceRecord r;
  r.filespace_uuid = std::get<TypedUuid>(decoded.fields[0].value);
  r.database_uuid = std::get<TypedUuid>(decoded.fields[1].value);
  r.filespace_role = std::get<u64>(decoded.fields[2].value);
  r.first_filespace = std::get<bool>(decoded.fields[3].value);
  r.startup_authority = std::get<bool>(decoded.fields[4].value);
  r.catalog_persistence_owner = std::get<bool>(decoded.fields[5].value);
  r.filespace_manifest_owner = std::get<bool>(decoded.fields[6].value);
  r.recovery_evidence_owner = std::get<bool>(decoded.fields[7].value);
  r.read_only = std::get<bool>(decoded.fields[8].value);
  r.state = std::get<u64>(decoded.fields[9].value);
  r.physical_filespace_id = std::get<u64>(decoded.fields[10].value);
  r.lifecycle_generation = std::get<u64>(decoded.fields[11].value);
  r.filespace_manifest_generation = std::get<u64>(decoded.fields[12].value);
  r.catalog_manifest_format_version = std::get<u64>(decoded.fields[13].value);
  r.resource_seed_manifest_format_version = std::get<u64>(decoded.fields[14].value);
  r.registered_txn = std::get<u64>(decoded.fields[15].value);
  r.last_lifecycle_transaction = std::get<u64>(decoded.fields[16].value);
  r.uuid_source = std::get<u64>(decoded.fields[17].value);
  r.header_database_uuid_match_required = std::get<bool>(decoded.fields[18].value);
  r.header_filespace_uuid_match_required = std::get<bool>(decoded.fields[19].value);
  r.startup_state_coupled = std::get<bool>(decoded.fields[20].value);
  r.page_header_coupled = std::get<bool>(decoded.fields[21].value);
  r.open_validate_header = std::get<bool>(decoded.fields[22].value);
  r.attach_admission_validate_header = std::get<bool>(decoded.fields[23].value);
  r.transaction_admission_validate_filespace = std::get<bool>(decoded.fields[24].value);
  r.maintenance_validate_header = std::get<bool>(decoded.fields[25].value);
  r.verify_repair_validate_header = std::get<bool>(decoded.fields[26].value);
  r.shutdown_validate_header = std::get<bool>(decoded.fields[27].value);
  r.recovery_validate_header = std::get<bool>(decoded.fields[28].value);
  r.drop_requires_database_lifecycle = std::get<bool>(decoded.fields[29].value);
  r.quarantine_on_ambiguous = std::get<bool>(decoded.fields[30].value);
  r.state_change_evidence_before_success = std::get<bool>(decoded.fields[31].value);
  r.mga_visibility_required = std::get<bool>(decoded.fields[32].value);
  r.path_is_locator_not_identity = std::get<bool>(decoded.fields[33].value);
  r.duplicate_identity_refusal = std::get<bool>(decoded.fields[34].value);
  r.stale_identity_refusal = std::get<bool>(decoded.fields[35].value);
  r.creator_transaction_number = std::get<u64>(decoded.fields[36].value);
  if (!Valid(r)) return {CatalogValueError::invalid_value, {}};
  return {CatalogValueError::none, r};
}
}  // namespace scratchbird::core::catalog

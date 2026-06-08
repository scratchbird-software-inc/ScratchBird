// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-CATALOG-RECORDS-ANCHOR
#include "datatype_descriptor.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::catalog {

using scratchbird::core::datatypes::CanonicalTypeId;
using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr u32 kCatalogRecordSchemaVersionCurrent = 1;
inline constexpr u32 kCatalogRecordSchemaVersionMinSupported = 1;
inline constexpr u32 kCatalogRecordSchemaVersionMaxSupported = 1;

enum class CatalogRecordKind : u16 {
  database = 1,
  filespace = 2,
  schema = 3,
  sql_object = 4,
  localized_name = 5,
  localized_comment = 6,
  synonym_descriptor = 7,
  constraint_descriptor = 84,
  key_descriptor = 85,
  constraint_subject = 86,
  constraint_dependency = 87,
  constraint_support_structure = 88,
  column_descriptor = 89,
  datatype_descriptor = 10,
  domain = 11,
  charset = 20,
  charset_alias = 21,
  collation = 22,
  collation_tailoring = 23,
  timezone = 30,
  timezone_transition = 31,
  timezone_leap_second = 32,
  resource_bundle = 40,
  resource_artifact = 41,
  policy = 50,
  config_profile = 51,
  user_account = 60,
  group_account = 61,
  role_account = 62,
  grant_record = 63,
  masking_policy = 64,
  rls_policy = 65,
  udr_package = 70,
  parser_package = 71,
  sblr_module = 72,
  storage_descriptor = 80,
  table_descriptor = 81,
  index_descriptor = 82,
  toast_reference = 83,
  audit_evidence = 90,
  management_operation = 91,
  metric_descriptor = 92,
  metric_current_value = 93,
  protected_material = 94,
  protected_material_version = 95,
  protected_material_policy_binding = 96,
  protected_material_audit_event = 97,
  cluster_stub = 100,
  unknown = 0xffffu
};

enum class CatalogRecordScope : u16 {
  local_database,
  cluster_shared,
  compatibility_projection,
  private_cluster,
  unknown
};

struct CatalogRecordDescriptor {
  CatalogRecordKind kind = CatalogRecordKind::unknown;
  CatalogRecordScope scope = CatalogRecordScope::unknown;
  std::string stable_name;
  bool requires_row_uuid = true;
  bool requires_object_uuid = false;
  bool requires_parent_uuid = false;
  bool may_reference_toast = false;
  bool mutable_after_create = false;
  bool parser_visible = false;
  bool engine_authority = true;
};

struct CatalogRecordHeader {
  CatalogRecordKind kind = CatalogRecordKind::unknown;
  TypedUuid row_uuid;
  TypedUuid object_uuid;
  TypedUuid parent_uuid;
  u32 record_version = 1;
  bool deleted = false;
};

struct ProtectedMaterialCatalogRecord {
  CatalogRecordHeader header;
  TypedUuid active_version_uuid;
  TypedUuid owner_scope_uuid;
  TypedUuid retention_policy_uuid;
  TypedUuid access_policy_uuid;
  TypedUuid release_policy_uuid;
  TypedUuid purge_policy_uuid;
  TypedUuid audit_policy_uuid;
  std::string purpose_class;
  std::string storage_class;
  std::string lifecycle_state = "active";
  u64 catalog_generation_id = 0;
  u64 created_local_transaction_id = 0;
  u64 updated_local_transaction_id = 0;
  u64 security_epoch = 0;
};

struct ProtectedMaterialVersionCatalogRecord {
  CatalogRecordHeader header;
  TypedUuid protected_material_uuid;
  TypedUuid retention_policy_uuid;
  TypedUuid access_policy_uuid;
  TypedUuid release_policy_uuid;
  TypedUuid purge_policy_uuid;
  TypedUuid audit_policy_uuid;
  std::string protected_reference_hash;
  std::string protected_envelope_hash;
  std::string payload_hash;
  std::string storage_class;
  std::string rotation_state = "active";
  u64 version_number = 0;
  u64 valid_from_local_transaction_id = 0;
  u64 valid_until_local_transaction_id = 0;
  u64 retention_until_epoch_millis = 0;
  u64 catalog_generation_id = 0;
  u64 security_epoch = 0;
  bool legal_hold = false;
  bool purged = false;
};

struct ProtectedMaterialPolicyBindingCatalogRecord {
  CatalogRecordHeader header;
  TypedUuid protected_material_uuid;
  TypedUuid protected_material_version_uuid;
  TypedUuid policy_uuid;
  std::string policy_kind;
  std::string diagnostic_state;
  u64 catalog_generation_id = 0;
  u64 security_epoch = 0;
};

struct ProtectedMaterialAuditEventCatalogRecord {
  CatalogRecordHeader header;
  TypedUuid protected_material_uuid;
  TypedUuid protected_material_version_uuid;
  TypedUuid actor_uuid;
  std::string event_kind;
  std::string decision;
  std::string diagnostic_code;
  std::string redacted_detail;
  u64 event_epoch_millis = 0;
  u64 local_transaction_id = 0;
  u64 catalog_generation_id = 0;
  bool redaction_applied = true;
};

struct CatalogRecordValidationResult {
  Status status;
  CatalogRecordDescriptor descriptor;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* CatalogRecordKindName(CatalogRecordKind kind);
const char* CatalogRecordScopeName(CatalogRecordScope scope);
const std::vector<CatalogRecordDescriptor>& BuiltinCatalogRecordDescriptors();
CatalogRecordValidationResult LookupCatalogRecordDescriptor(CatalogRecordKind kind);
CatalogRecordValidationResult ValidateCatalogRecordDescriptor(const CatalogRecordDescriptor& descriptor);
DiagnosticRecord MakeCatalogRecordDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail = {});

}  // namespace scratchbird::core::catalog

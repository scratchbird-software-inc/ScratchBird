// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-CATALOG-RECORD-CODEC-ANCHOR
#include "catalog_page.hpp"
#include "catalog_records.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <string_view>
#include <optional>
#include <span>
#include <array>
#include <vector>

namespace scratchbird::core::catalog {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::storage::page::CatalogPageRow;

struct CatalogTypedRecord {
  CatalogRecordHeader header;
  std::string payload;
};
// Structural admission only. The immutable payload owner and its actual memory
// grant must outlive this view and every copy; the header identities are native
// values. This view confers no publication, activation, or storage authority.
struct CatalogTypedRecordView {
  CatalogRecordHeader header;
  std::string_view payload;
};
inline CatalogTypedRecordView BorrowCatalogTypedRecord(const CatalogTypedRecord& r) {
  return {r.header,r.payload};
}
// Static diagnostic identifiers/details in this boundary; no owning error
// strings or text identity carriers. Materialization is explicit at the caller.
struct CatalogRecordDiagnosticView {
  Status status;
  std::string_view diagnostic_code, message_key, detail, origin;
};
struct CatalogTypedRecordViewResult {
  std::optional<CatalogTypedRecordView> record;
  CatalogRecordDiagnosticView diagnostic;
  bool ok() const { return record.has_value(); }
};
CatalogTypedRecordViewResult ValidateCatalogTypedRecordView(CatalogTypedRecordView);
CatalogTypedRecordViewResult DecodeCatalogTypedRecordView(
    scratchbird::storage::page::CatalogPageRowKind, std::string_view);
DiagnosticRecord MaterializeCatalogRecordDiagnostic(const CatalogRecordDiagnosticView&);

enum class CatalogAuthorityScope : u16 {
  local = 1, cluster, donor_overlay, security_database, configuration_database,
  temporary, external_reference
};
enum class CatalogObjectLifecycle : u16 {
  creating = 1, active, renaming, altering, dropping, dropped, deferred,
  quarantined, repairing, archived
};
enum class CatalogObjectStatus : u16 {
  proposed = 1, active, disabled_by_policy, bridge_only, render_only, deferred,
  unsupported_by_policy, retired, quarantined
};
enum class CatalogVisibilityClass : u16 {
  public_metadata = 1, authenticated, administrator, sysarch, support_redacted,
  security_redacted, internal_only
};

// Full common metadata-version carrier for a native MGA catalog row. Family
// payload validation, authorization and dependency publication remain owning
// catalog operations; encoding alone grants no execution/publication authority.
struct CatalogMetadataVersion {
  CatalogTypedRecord record;
  CatalogAuthorityScope authority_scope = CatalogAuthorityScope::local;
  CatalogObjectLifecycle lifecycle = CatalogObjectLifecycle::creating;
  CatalogObjectStatus status = CatalogObjectStatus::proposed;
  CatalogVisibilityClass visibility = CatalogVisibilityClass::internal_only;
  u64 definition_version = 0;
  u64 schema_epoch = 0;
  u64 security_epoch = 0;
  u64 resource_epoch = 0;
  u64 catalog_generation = 0;
  u64 dependency_generation = 0;
  u64 invalidation_generation = 0;
  u64 creator_local_transaction_id = 0;
  TypedUuid owner_uuid;
  TypedUuid creator_transaction_uuid;
  TypedUuid retired_transaction_uuid;
  TypedUuid default_name_uuid;
  TypedUuid name_vector_uuid;
  TypedUuid security_policy_uuid;
  TypedUuid dependency_group_uuid;
  TypedUuid storage_binding_uuid;
  TypedUuid donor_overlay_uuid;
  TypedUuid audit_uuid;
  TypedUuid owning_schema_uuid;
  std::string trace_search_key;
  std::string object_subtype;
  std::string retention_class;
};

// Complete structural view. The immutable input and its actual grant must
// outlive every copy and every family-origin comparison. No runtime authority.
struct CatalogMetadataVersionView {
  CatalogTypedRecordView record;
  CatalogAuthorityScope authority_scope = CatalogAuthorityScope::local;
  CatalogObjectLifecycle lifecycle = CatalogObjectLifecycle::creating;
  CatalogObjectStatus status = CatalogObjectStatus::proposed;
  CatalogVisibilityClass visibility = CatalogVisibilityClass::internal_only;
  u64 definition_version = 0;
  u64 schema_epoch = 0;
  u64 security_epoch = 0;
  u64 resource_epoch = 0;
  u64 catalog_generation = 0;
  u64 dependency_generation = 0;
  u64 invalidation_generation = 0;
  u64 creator_local_transaction_id = 0;
  TypedUuid owner_uuid;
  TypedUuid creator_transaction_uuid;
  TypedUuid retired_transaction_uuid;
  TypedUuid default_name_uuid;
  TypedUuid name_vector_uuid;
  TypedUuid security_policy_uuid;
  TypedUuid dependency_group_uuid;
  TypedUuid storage_binding_uuid;
  TypedUuid donor_overlay_uuid;
  TypedUuid audit_uuid;
  TypedUuid owning_schema_uuid;
  std::string_view trace_search_key;
  std::string_view object_subtype;
  std::string_view retention_class;
};
inline CatalogMetadataVersionView BorrowCatalogMetadataVersion(const CatalogMetadataVersion& r) {
  return {BorrowCatalogTypedRecord(r.record),
      r.authority_scope,
      r.lifecycle,
      r.status,
      r.visibility,
      r.definition_version,
      r.schema_epoch,
      r.security_epoch,
      r.resource_epoch,
      r.catalog_generation,
      r.dependency_generation,
      r.invalidation_generation,
      r.creator_local_transaction_id,
      r.owner_uuid,
      r.creator_transaction_uuid,
      r.retired_transaction_uuid,
      r.default_name_uuid,
      r.name_vector_uuid,
      r.security_policy_uuid,
      r.dependency_group_uuid,
      r.storage_binding_uuid,
      r.donor_overlay_uuid,
      r.audit_uuid,
      r.owning_schema_uuid,
      r.trace_search_key,
      r.object_subtype,
      r.retention_class};
}
struct CatalogMetadataVersionViewResult {
  std::optional<CatalogMetadataVersionView> record;
  std::array<scratchbird::core::platform::byte,32> definition_sha256{};
  CatalogRecordDiagnosticView diagnostic;
  bool ok() const { return record.has_value(); }
};
CatalogMetadataVersionViewResult DecodeCatalogMetadataVersionView(
    std::span<const scratchbird::core::platform::byte> bytes);
std::optional<CatalogRecordDiagnosticView> ValidateCatalogMetadataVersionView(
    const CatalogMetadataVersionView&);
// Complete structural validation and exact envelope size, without allocation.
std::optional<std::size_t> CatalogMetadataVersionEncodedBytes(const CatalogMetadataVersionView&);
CatalogMetadataVersion MaterializeCatalogMetadataVersion(const CatalogMetadataVersionView&);


struct CatalogMetadataVersionCodecResult {
  Status status;
  CatalogMetadataVersion record;
  std::array<scratchbird::core::platform::byte, 32> definition_sha256{};
  std::vector<scratchbird::core::platform::byte> bytes;
  DiagnosticRecord diagnostic;
  bool ok() const { return status.ok(); }
};

CatalogMetadataVersionCodecResult EncodeCatalogMetadataVersion(const CatalogMetadataVersion& record);
CatalogMetadataVersionCodecResult DecodeCatalogMetadataVersion(
    const std::vector<scratchbird::core::platform::byte>& bytes);

// Complete composition of the admitted definition-family continuity checks.
// Callers retain common version/transaction checks and name-residency checks.
// May propagate allocation failure; never converts it into successful admission.
bool CatalogMetadataPreservesFamilyOrigin(
    const CatalogMetadataVersion& previous, const CatalogMetadataVersion& successor);
bool CatalogMetadataPreservesFamilyOrigin(
    const CatalogMetadataVersionView& previous, const CatalogMetadataVersionView& successor);

struct CatalogRecordCodecResult {
  Status status;
  CatalogTypedRecord record;
  CatalogPageRow row;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

CatalogRecordCodecResult EncodeCatalogTypedRecord(const CatalogTypedRecord& record, u32 ordinal);
CatalogRecordCodecResult DecodeCatalogTypedRecord(const CatalogPageRow& row);
DiagnosticRecord MakeCatalogRecordCodecDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {});

}  // namespace scratchbird::core::catalog

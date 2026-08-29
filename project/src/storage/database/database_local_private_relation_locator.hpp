// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CORE-ENGINE-PRIVATE-DB-SECURITY-LOCATOR-V1
#include "disk_device.hpp"
#include "page_header.hpp"
#include "runtime_platform.hpp"
#include "transaction_inventory.hpp"
#include "row_version.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::storage::database {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr u64 kDatabaseLocalPrivateSecurityRootPageNumberV1 = 5;
inline constexpr u64 kDatabaseLocalPrivateSecurityFirstDataPageNumberV1 = 6;
inline constexpr u64 kDatabaseLocalPrivateSecurityRelationGenerationV1 = 1;
inline constexpr const char* kDatabaseLocalPrivateSecurityRelationUuidV1 =
    "018f7a10-1280-7000-8000-000000000107";

inline constexpr u32 kDatabaseLocalPrivateSecurityMarkerBytesV1 = 176;
inline constexpr u32 kDatabaseLocalPrivateSecurityAnchorBytesV1 = 152;
inline constexpr u32 kDatabaseLocalPrivateSecurityLocatorBytesV1 = 272;
inline constexpr std::array<u32, 2>
    kDatabaseLocalPrivateSecurityAnchorBodyOffsetsV1 = {0, 160};
inline constexpr std::array<u32, 2>
    kDatabaseLocalPrivateSecurityLocatorBodyOffsetsV1 = {320, 608};

using SerializedDatabaseLocalPrivateSecurityMarkerV1 =
    std::array<byte, kDatabaseLocalPrivateSecurityMarkerBytesV1>;
using SerializedDatabaseLocalPrivateSecurityAnchorV1 =
    std::array<byte, kDatabaseLocalPrivateSecurityAnchorBytesV1>;
using SerializedDatabaseLocalPrivateSecurityLocatorV1 =
    std::array<byte, kDatabaseLocalPrivateSecurityLocatorBytesV1>;
using DatabaseLocalPrivateSecuritySha256V1 = std::array<byte, 32>;

inline constexpr u32 kDatabaseLocalSecurityBatchEnvelopeBytesV1 = 172;

// Shared SBSEDB01 v1.1 carrier. Both steady private-security I/O and the
// exclusive legacy temp-image migration use this one codec and lifecycle
// payload validator.
struct DatabaseLocalSecurityBatchEnvelopeV1 {
  Uuid database_uuid;
  Uuid relation_uuid;
  Uuid transaction_uuid;
  Uuid actor_principal_uuid;
  u64 creator_local_transaction_id = 0;
  u64 prior_security_context_generation = 0;
  u64 successor_security_context_generation = 0;
  Uuid predecessor_page_uuid;
  u64 predecessor_page_number = 0;
  u64 predecessor_page_generation = 0;
  std::vector<std::string> events;
};

inline constexpr const char* kDatabaseLocalPrivateSecurityLocatorInvalid =
    "SECURITY.CATALOG.PRIVATE_LOCATOR_INVALID";
inline constexpr const char* kDatabaseLocalPrivateSecurityLocatorMissing =
    "SECURITY.CATALOG.PRIVATE_LOCATOR_MISSING";
inline constexpr const char* kDatabaseLocalPrivateSecurityLocatorWriteFailed =
    "SECURITY.CATALOG.PRIVATE_LOCATOR_WRITE_FAILED";
inline constexpr const char* kDatabaseLocalPrivateSecurityLocatorUpgradeRequired =
    "SECURITY.CATALOG.PRIVATE_LOCATOR_UPGRADE_REQUIRED";

enum class DatabaseLocalPrivateSecurityLocatorClassV1 : u16 {
  invalid = 0,
  sealed_current = 1,
  exact_legacy_absence = 2,
};

enum class DatabaseLocalPrivateSecurityLocatorLineageV1 : u32 {
  fresh_bootstrap = 1,
  sealed_legacy_migration = 2,
};

struct DatabaseLocalPrivateSecurityMarkerV1 {
  u32 flags = 0;
  Uuid database_uuid;
  Uuid relation_uuid;
  u64 relation_generation = 0;
  u64 migration_scan_count = 0;
  Uuid locator_page_uuid;
  u64 locator_page_number = 0;
  u64 locator_page_generation = 0;
  u64 initial_locator_generation = 0;
  DatabaseLocalPrivateSecuritySha256V1 initial_locator_sha256{};
  DatabaseLocalPrivateSecuritySha256V1 marker_sha256{};
};

struct DatabaseLocalPrivateSecurityAnchorV1 {
  u32 copy_ordinal = 0;
  u32 flags = 0;
  Uuid database_uuid;
  Uuid relation_uuid;
  u64 relation_generation = 0;
  u64 anchor_generation = 0;
  u32 locator_slot = 0;
  u64 locator_generation = 0;
  DatabaseLocalPrivateSecuritySha256V1 locator_sha256{};
  DatabaseLocalPrivateSecuritySha256V1 anchor_sha256{};
};

struct DatabaseLocalPrivateSecurityLocatorV1 {
  u32 slot_ordinal = 0;
  DatabaseLocalPrivateSecurityLocatorLineageV1 lineage =
      DatabaseLocalPrivateSecurityLocatorLineageV1::fresh_bootstrap;
  Uuid database_uuid;
  Uuid filespace_uuid;
  Uuid relation_uuid;
  u64 relation_generation = 0;
  u64 locator_generation = 0;
  Uuid creator_transaction_uuid;
  u64 creator_local_transaction_id = 0;
  Uuid head_page_uuid;
  u64 head_page_number = 0;
  u64 head_page_generation = 0;
  Uuid tail_page_uuid;
  u64 tail_page_number = 0;
  u64 tail_page_generation = 0;
  u64 chain_page_count = 0;
  u64 extent_min_page = 0;
  u64 extent_max_page = 0;
  u64 security_context_generation = 0;
  DatabaseLocalPrivateSecuritySha256V1 prior_locator_sha256{};
  DatabaseLocalPrivateSecuritySha256V1 record_sha256{};
};

struct DatabaseLocalPrivateSecurityLocatorIoMetricsV1 {
  u64 locator_page_reads = 0;
  u64 security_chain_page_reads = 0;
  u64 legacy_scan_page_reads = 0;
  u64 locator_migration_count = 0;
};

struct DatabaseLocalPrivateSecurityLocatorInspectRequestV1 {
  scratchbird::storage::disk::FileDevice* device = nullptr;
  SerializedDatabaseLocalPrivateSecurityMarkerV1 marker_bytes{};
  TypedUuid expected_database_uuid;
  u32 page_size = 0;
  bool admit_exact_legacy_absence = false;
};

struct DatabaseLocalPrivateSecurityLocatorInspectResultV1 {
  Status status;
  DatabaseLocalPrivateSecurityLocatorClassV1 classification =
      DatabaseLocalPrivateSecurityLocatorClassV1::invalid;
  DatabaseLocalPrivateSecurityMarkerV1 marker;
  scratchbird::storage::disk::PageHeader locator_page_header;
  std::array<DatabaseLocalPrivateSecurityAnchorV1, 2> anchors{};
  std::array<DatabaseLocalPrivateSecurityLocatorV1, 2> locators{};
  std::array<bool, 2> anchor_valid{};
  std::array<bool, 2> locator_shape_valid{};
  std::array<bool, 2> locator_digest_valid{};
  std::array<bool, 2> locator_context_valid{};
  // Exact on-disk zero state. A zero inactive slot is reusable; a nonzero
  // malformed slot is never silently overwritten.
  std::array<bool, 2> locator_zero{};
  u32 anchored_locator_slot = 0;
  u64 anchored_locator_generation = 0;
  DatabaseLocalPrivateSecurityLocatorIoMetricsV1 metrics;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() &&
           classification != DatabaseLocalPrivateSecurityLocatorClassV1::invalid;
  }
};

struct DatabaseLocalPrivateSecurityLocatorVisibilityRequestV1 {
  const DatabaseLocalPrivateSecurityLocatorInspectResultV1* inspected = nullptr;
  const scratchbird::transaction::mga::LocalTransactionInventory* inventory =
      nullptr;
  scratchbird::transaction::mga::VisibilitySnapshot visibility_snapshot;
  bool use_latest_committed_snapshot = true;
  TypedUuid reader_transaction_uuid;
  scratchbird::transaction::mga::LocalTransactionId reader_local_transaction_id;
  TypedUuid transaction_inventory_filespace_uuid;
};

struct DatabaseLocalPrivateSecurityLocatorVisibilityResultV1 {
  Status status;
  DatabaseLocalPrivateSecurityLocatorV1 selected_locator;
  u32 selected_locator_slot = 0;
  bool anchored_locator_visible = false;
  bool selected_digest_linked_prior = false;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && selected_locator.locator_generation != 0;
  }
};

struct DatabaseLocalPrivateSecurityLocatorInitializeRequestV1 {
  scratchbird::storage::disk::FileDevice* device = nullptr;
  TypedUuid database_uuid;
  u32 page_size = 0;
  u64 bootstrap_security_context_generation = 0;
  u64 creation_unix_epoch_millis = 0;
};

struct DatabaseLocalPrivateSecurityLocatorInitializeResultV1 {
  Status status;
  SerializedDatabaseLocalPrivateSecurityMarkerV1 marker_bytes{};
  DatabaseLocalPrivateSecurityMarkerV1 marker;
  DatabaseLocalPrivateSecurityLocatorV1 locator;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DatabaseLocalPrivateSecurityLocatorSuccessorRequestV1 {
  std::string database_path;
  SerializedDatabaseLocalPrivateSecurityMarkerV1 marker_bytes{};
  TypedUuid expected_database_uuid;
  u32 page_size = 0;
  DatabaseLocalPrivateSecurityLocatorV1 selected_prior;
  TypedUuid creator_transaction_uuid;
  u64 creator_local_transaction_id = 0;
  TypedUuid head_page_uuid;
  u64 head_page_number = 0;
  u64 head_page_generation = 0;
  u64 expected_predecessor_page_number = 0;
  u64 security_context_generation = 0;
  // Test-only exact crash cut: after_locator, after_anchor_copy_0, or empty.
  std::string fault_injection_point;
};

struct DatabaseLocalPrivateSecurityLocatorSuccessorResultV1 {
  Status status;
  DatabaseLocalPrivateSecurityLocatorV1 locator;
  u64 anchor_generation = 0;
  DatabaseLocalPrivateSecurityLocatorIoMetricsV1 metrics;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DatabaseLocalPrivateSecurityLocatorMigrationRequestV1 {
  // The caller owns an exclusive same-directory temporary image. This API
  // never opens, renames, or scans the live source database.
  scratchbird::storage::disk::FileDevice* temporary_image = nullptr;
  TypedUuid database_uuid;
  u32 page_size = 0;
  u64 bootstrap_security_context_generation = 0;
  u64 migration_unix_epoch_millis = 0;
};

struct DatabaseLocalPrivateSecurityLocatorMigrationResultV1 {
  Status status;
  SerializedDatabaseLocalPrivateSecurityMarkerV1 marker_bytes{};
  DatabaseLocalPrivateSecurityMarkerV1 marker;
  DatabaseLocalPrivateSecurityLocatorV1 locator;
  DatabaseLocalPrivateSecurityLocatorIoMetricsV1 metrics;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

DatabaseLocalPrivateSecurityLocatorInspectResultV1
InspectDatabaseLocalPrivateSecurityLocatorV1(
    const DatabaseLocalPrivateSecurityLocatorInspectRequestV1& request);

DatabaseLocalPrivateSecurityLocatorVisibilityResultV1
SelectDatabaseLocalPrivateSecurityLocatorForVisibilityV1(
    const DatabaseLocalPrivateSecurityLocatorVisibilityRequestV1& request);

DatabaseLocalPrivateSecurityLocatorInitializeResultV1
InitializeFreshDatabaseLocalPrivateSecurityLocatorV1(
    const DatabaseLocalPrivateSecurityLocatorInitializeRequestV1& request);

DatabaseLocalPrivateSecurityLocatorSuccessorResultV1
PublishDatabaseLocalPrivateSecurityLocatorSuccessorV1(
    const DatabaseLocalPrivateSecurityLocatorSuccessorRequestV1& request);

DatabaseLocalPrivateSecurityLocatorMigrationResultV1
MigrateLegacyDatabaseLocalPrivateSecurityLocatorInTemporaryImageV1(
    const DatabaseLocalPrivateSecurityLocatorMigrationRequestV1& request);

SerializedDatabaseLocalPrivateSecurityMarkerV1
EncodeDatabaseLocalPrivateSecurityMarkerV1(
    const DatabaseLocalPrivateSecurityMarkerV1& marker);
SerializedDatabaseLocalPrivateSecurityAnchorV1
EncodeDatabaseLocalPrivateSecurityAnchorV1(
    const DatabaseLocalPrivateSecurityAnchorV1& anchor);
SerializedDatabaseLocalPrivateSecurityLocatorV1
EncodeDatabaseLocalPrivateSecurityLocatorV1(
    const DatabaseLocalPrivateSecurityLocatorV1& locator);

std::vector<byte> EncodeDatabaseLocalSecurityBatchEnvelopeV1(
    const DatabaseLocalSecurityBatchEnvelopeV1& batch,
    std::string* refusal = nullptr);
bool DecodeDatabaseLocalSecurityBatchEnvelopeV1(
    const std::vector<byte>& encoded,
    DatabaseLocalSecurityBatchEnvelopeV1* batch,
    std::string* refusal = nullptr);
bool ValidateDatabaseLocalSecurityLifecycleBatchV1(
    const DatabaseLocalSecurityBatchEnvelopeV1& batch,
    std::string* refusal = nullptr);

DiagnosticRecord MakeDatabaseLocalPrivateSecurityLocatorDiagnosticV1(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::storage::database

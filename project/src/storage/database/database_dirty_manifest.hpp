// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-DIRTY-MANIFEST-ANCHOR
#include "runtime_platform.hpp"
#include "uuid.hpp"
#include "native_common_page_header.hpp"
#include "filespace_page_zero.hpp"
#include "transaction_inventory_page.hpp"
#include "catalog_page.hpp"
#include "native_allocation_map.hpp"
#include "native_filespace_directory.hpp"
#include "native_system_state.hpp"
#include "native_horizon_root.hpp"
#include "native_retention_root.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::storage::database {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

// MGA-CANONICAL-CHECKPOINT-IMAGE-001. Image and exact-reference evidence,
// not a caller-asserted checkpoint selection or transaction-finality receipt.
struct NativeCheckpointRootReference {
  u16 role = 0;
  u32 page_type = 0;
  scratchbird::storage::disk::NativePageReference page;
  scratchbird::core::platform::Uuid object_uuid;
  std::array<scratchbird::core::platform::byte,32> sha256{};
  bool operator==(const NativeCheckpointRootReference&) const = default;
};
struct NativeCheckpointRoot {
  scratchbird::storage::disk::NativeCommonPageHeader header;
  scratchbird::core::platform::Uuid object_uuid;
  u64 checkpoint_generation = 0;
  u64 root_set_generation = 0;
  u64 selected_local_transaction_id = 0;
  u64 stable_local_transaction_id = 0;
  u64 local_durable_transaction_id = 0;
  u64 cluster_quorum_transaction_id = 0;
  scratchbird::core::platform::Uuid timeline_uuid;
  scratchbird::core::platform::Uuid creator_transaction_uuid;
  u64 creator_local_transaction_id = 0;
  u64 flags = 0;
  std::optional<scratchbird::storage::disk::NativePageReference> predecessor;
  std::array<scratchbird::core::platform::byte,32> predecessor_sha256{};
  bool completed = false;
  std::vector<NativeCheckpointRootReference> roots;
  // Exclusive alternative to creator transaction UUID/local number. The
  // image preserves lineage; a UUID is not durable operation authority.
  scratchbird::core::platform::Uuid creator_operation_uuid;
};
enum class NativeCheckpointError {
  none, invalid_header, invalid_family, invalid_reference, invalid_roots,
  invalid_integrity, hash_failure, resource_exhausted, invalid_filespace,
  binding_mismatch, io_failure, encrypted_requires_crypto_authority,
  incomplete, inventory_failure, inventory_mismatch, creator_not_committed,
  history_mismatch, catalog_failure, catalog_creator_mismatch, catalog_creator_not_committed,
  allocation_failure, allocation_creator_mismatch, allocation_creator_not_committed,
  allocation_record_creator_mismatch, policy_relation_mismatch, directory_failure,
  directory_creator_mismatch, directory_creator_not_committed, system_state_failure,
  system_state_creator_mismatch, system_state_creator_not_committed,
  system_state_clean_mismatch, system_state_clean_not_committed, system_state_observation_mismatch,
  horizon_failure, horizon_creator_mismatch, horizon_creator_not_committed,
  horizon_retention_mismatch, horizon_boundary_mismatch, horizon_observation_mismatch,
  retention_failure, retention_creator_mismatch, retention_creator_not_committed,
  horizon_pin_missing, horizon_pin_lineage_mismatch, cluster_requires_authority
};
struct NativeCheckpointRootResult {
  NativeCheckpointError error = NativeCheckpointError::invalid_family;
  std::optional<NativeCheckpointRoot> root;
  std::vector<scratchbird::core::platform::byte> bytes;
  bool ok() const noexcept { return error == NativeCheckpointError::none && root.has_value(); }
};
NativeCheckpointRootResult EncodeNativeCheckpointRoot(const NativeCheckpointRoot&) noexcept;
NativeCheckpointRootResult DecodeNativeCheckpointRoot(const std::vector<scratchbird::core::platform::byte>&) noexcept;
NativeCheckpointRootResult ReadNativeCheckpointRootFromOpenDevice(
    scratchbird::storage::disk::FileDevice&,
    const scratchbird::core::platform::Uuid& database_uuid,
    const scratchbird::storage::disk::FilespaceRootReference&) noexcept;

struct NativeInventoryPageBinding {
  scratchbird::storage::disk::NativeCommonPageHeader header;
  scratchbird::core::platform::Uuid object_uuid;
};
struct NativeCheckpointInventoryResult {
  NativeCheckpointError error = NativeCheckpointError::invalid_reference;
  scratchbird::storage::page::NativeInventoryError inventory_error = scratchbird::storage::page::NativeInventoryError::none;
  std::optional<NativeCheckpointRoot> checkpoint;
  std::array<scratchbird::core::platform::byte,32> checkpoint_sha256{};
  scratchbird::transaction::mga::LocalTransactionInventory inventory;
  // Actual verified chain identities, for allocation admission by the selector.
  std::vector<NativeInventoryPageBinding> inventory_pages;
  u64 inventory_generation = 0;
  u64 retained_image_bytes = 0;
  bool ok() const noexcept { return error == NativeCheckpointError::none && checkpoint.has_value(); }
};
// Actual checkpoint -> head digest -> complete inventory -> creator outcome.
// No overall root selection: other families and predecessor/recovery authority
// are independently required. No publication-CAS base is issued here.
NativeCheckpointInventoryResult VerifyNativeCheckpointInventoryFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>&,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u64 maximum_retained_image_bytes) noexcept;

struct NativeCheckpointCatalogResult {
  NativeCheckpointError error = NativeCheckpointError::invalid_reference;
  scratchbird::storage::page::NativeCatalogRootError catalog_error =
      scratchbird::storage::page::NativeCatalogRootError::none;
  NativeCheckpointInventoryResult checkpoint_inventory;
  // First is the catalog. Feature selects the same image or the second image.
  std::vector<scratchbird::storage::page::NativeCatalogRootResult> catalogs;
  std::size_t feature_root_index = 0;
  u64 retained_image_bytes = 0;
  bool ok() const noexcept {
    return error == NativeCheckpointError::none && checkpoint_inventory.ok()
        && !catalogs.empty() && feature_root_index < catalogs.size();
  }
};

struct NativeCheckpointAllocationResult {
  NativeCheckpointError error = NativeCheckpointError::invalid_reference;
  scratchbird::storage::page::NativeAllocationError allocation_error =
      scratchbird::storage::page::NativeAllocationError::none;
  NativeCheckpointInventoryResult checkpoint_inventory;
  scratchbird::storage::page::NativeAllocationChainResult allocation;
  u64 retained_image_bytes = 0;
  bool ok() const noexcept {
    return error == NativeCheckpointError::none && checkpoint_inventory.ok() && allocation.ok();
  }
};

struct NativeCheckpointDirectoryResult {
  NativeCheckpointError error = NativeCheckpointError::invalid_reference;
  scratchbird::storage::page::NativeDirectoryError directory_error =
      scratchbird::storage::page::NativeDirectoryError::none;
  NativeCheckpointInventoryResult checkpoint_inventory;
  scratchbird::storage::page::NativeFilespaceDirectoryChainResult directory;
  u64 retained_image_bytes = 0;
  bool ok() const noexcept {
    return error == NativeCheckpointError::none && checkpoint_inventory.ok() && directory.ok();
  }
};
// Current root selection and committed directory creator, not whole-root
// publication, attachment, historical epoch admission or a CAS base.
NativeCheckpointDirectoryResult VerifyCurrentNativeCheckpointDirectoryFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>&,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u64 maximum_retained_image_bytes) noexcept;

struct NativeCheckpointPolicyRootsResult {
  NativeCheckpointError error = NativeCheckpointError::invalid_reference;
  scratchbird::storage::page::NativeCatalogRootError catalog_error =
      scratchbird::storage::page::NativeCatalogRootError::none;
  NativeCheckpointCatalogResult catalog;
  // Configuration, then security. These images grant neither private-row
  // access nor configuration activation; their actual consumers own that work.
  std::array<scratchbird::storage::page::NativeCatalogRootResult, 2> policies;
  u64 retained_image_bytes = 0;
  bool ok() const noexcept {
    return error == NativeCheckpointError::none && catalog.ok() && policies[0].ok() && policies[1].ok();
  }
};
NativeCheckpointPolicyRootsResult VerifyNativeCheckpointPolicyRootsFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>&,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u64 maximum_retained_image_bytes) noexcept;
// MGA-CURRENT-CHECKPOINT-ALLOCATION-BINDING-001. Actual current primary roots,
// map digest and inventory creator binding; no reuse grant or publication base.
NativeCheckpointAllocationResult VerifyCurrentNativeCheckpointAllocationFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>&,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u64 maximum_retained_image_bytes) noexcept;
// Exact checkpoint/inventory and catalog/feature image/creator binding.
// Not leaf/index serving, snapshot admission, whole-root selection or a CAS base.
NativeCheckpointCatalogResult VerifyNativeCheckpointCatalogRootsFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>&,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u64 maximum_retained_image_bytes) noexcept;

struct NativeCheckpointHistoryResult {
  NativeCheckpointError error = NativeCheckpointError::invalid_reference;
  scratchbird::storage::page::NativeInventoryError inventory_error = scratchbird::storage::page::NativeInventoryError::none;
  std::vector<NativeCheckpointInventoryResult> checkpoints;
  u64 retained_image_bytes = 0;
  bool ok() const noexcept { return error == NativeCheckpointError::none && !checkpoints.empty(); }
};
// Exact head-through-terminal retained history; no partial range, inferred
// retention boundary, overall root-family admission or publication receipt.
NativeCheckpointHistoryResult VerifyNativeCheckpointHistoryFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>&,
    const scratchbird::storage::disk::FilespaceRootReference& head,
    const scratchbird::storage::disk::FilespaceRootReference& terminal,
    u64 maximum_retained_image_bytes) noexcept;

struct NativeCheckpointSystemStateResult {
  NativeCheckpointError error=NativeCheckpointError::invalid_reference;
  NativeSystemStateError system_error=NativeSystemStateError::none;
  NativeCheckpointHistoryResult checkpoints;
  NativeSystemStateResult system_state;
  u64 retained_image_bytes=0;
  bool ok() const noexcept {return error==NativeCheckpointError::none&&checkpoints.ok()&&system_state.ok();}
};
// Actual current root/creator/observation binding, NOT proof of shutdown,
// quiescence, operation completion, whole-root selection or a publication base.
NativeCheckpointSystemStateResult VerifyCurrentNativeCheckpointSystemStateFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>&,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u64 maximum_retained_image_bytes) noexcept;

struct NativeCheckpointHorizonResult {
  NativeCheckpointError error=NativeCheckpointError::invalid_reference;
  scratchbird::storage::page::NativeHorizonError horizon_error=scratchbird::storage::page::NativeHorizonError::none;
  scratchbird::storage::page::NativeRetentionError retention_error=scratchbird::storage::page::NativeRetentionError::none;
  NativeCheckpointHistoryResult checkpoints;
  scratchbird::storage::page::NativeHorizonChainResult horizons;
  scratchbird::storage::page::NativeRetentionChainResult retention;
  u64 retained_image_bytes=0;
  bool ok() const noexcept {return error==NativeCheckpointError::none&&checkpoints.ok()&&horizons.ok()&&retention.ok();}
};
// Current selection, committed creators, actual retention pins and checkpoint
// observations. Not live horizon calculation, policy permissions or cleanup authority.
NativeCheckpointHorizonResult VerifyCurrentNativeCheckpointHorizonFromOpenDevices(
    const scratchbird::core::platform::Uuid& database_uuid,
    const std::vector<scratchbird::storage::disk::NativeFilespaceDevice>&,
    const scratchbird::storage::disk::FilespaceRootReference& checkpoint,
    u64 maximum_retained_image_bytes) noexcept;

inline constexpr u32 kDirtyObjectManifestFormatVersion = 1;

enum class DirtyObjectKind : u16 {
  database_header,
  startup_state,
  transaction_inventory,
  catalog_page,
  allocation_map,
  row_data_page,
  index_page,
  filespace_header,
  metric_history,
  unknown
};

enum class DirtyManifestRecoveryAction : u16 {
  no_action,
  use_manifest,
  rebuild_by_scan,
  quarantine,
  fail_closed
};

struct DirtyObjectManifestEntry {
  DirtyObjectKind kind = DirtyObjectKind::unknown;
  TypedUuid object_uuid;
  u64 page_number = 0;
  u64 page_generation = 0;
  u64 object_checksum = 0;
  u64 local_transaction_id = 0;
  u64 operation_envelope_checksum = 0;
  u64 transaction_evidence_checksum = 0;
  bool dirty = true;
  bool authoritative = true;
};

struct DirtyObjectManifest {
  u32 format_version = kDirtyObjectManifestFormatVersion;
  u64 checkpoint_generation = 0;
  u64 manifest_checksum = 0;
  bool completed = false;
  bool classification_only = true;
  std::vector<DirtyObjectManifestEntry> entries;
};

struct DirtyObjectManifestResult {
  Status status;
  DirtyObjectManifest manifest;
  std::string serialized;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct DirtyManifestRecoveryClassification {
  DirtyObjectKind kind = DirtyObjectKind::unknown;
  TypedUuid object_uuid;
  u64 page_number = 0;
  DirtyManifestRecoveryAction action = DirtyManifestRecoveryAction::fail_closed;
  bool fail_closed = false;
  std::string stable_reason;
};

struct DirtyManifestRecoveryResult {
  Status status;
  bool rebuild_by_scan_required = false;
  bool quarantine_required = false;
  std::vector<DirtyManifestRecoveryClassification> classifications;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct CheckpointRootCandidate {
  u64 checkpoint_generation = 0;
  u64 predecessor_generation = 0;
  TypedUuid root_object_uuid;
  u64 root_checksum = 0;
  bool completed = false;
  bool authoritative = true;
};

struct CheckpointRootSelectionResult {
  Status status;
  bool selected = false;
  CheckpointRootCandidate root;
  std::vector<u64> predecessor_chain;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct DirtyManifestRecoveryRunEvidence {
  std::string recovery_run_uuid;
  u64 checkpoint_generation = 0;
  u64 classification_count = 0;
  u64 classification_checksum = 0;
  std::string recovery_action;
  bool completed = false;
};

struct DirtyManifestRecoveryRunEvidenceResult {
  Status status;
  bool already_recorded = false;
  DirtyManifestRecoveryRunEvidence evidence;
  std::string serialized;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

const char* DirtyObjectKindName(DirtyObjectKind kind);
const char* DirtyManifestRecoveryActionName(DirtyManifestRecoveryAction action);

DirtyObjectManifestResult BuildDirtyObjectManifest(const DirtyObjectManifest& manifest);
DirtyObjectManifestResult ParseDirtyObjectManifest(const std::string& serialized);
DirtyManifestRecoveryResult ClassifyDirtyObjectManifestForRecovery(const DirtyObjectManifest& manifest);
CheckpointRootSelectionResult SelectCheckpointRootSet(const std::vector<CheckpointRootCandidate>& candidates);
DirtyManifestRecoveryRunEvidenceResult PersistDirtyManifestRecoveryRunEvidence(
    const std::string& evidence_store_path,
    const DirtyObjectManifest& manifest,
    const DirtyManifestRecoveryResult& recovery,
    const std::string& recovery_run_uuid);
DiagnosticRecord MakeDirtyManifestDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail = {});

}  // namespace scratchbird::storage::database

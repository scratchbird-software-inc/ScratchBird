// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_filespace_initialization.hpp"
#include "database_dirty_manifest.hpp"
#include "catalog_name_envelope.hpp"
#include "catalog_named_creation.hpp"
#include "native_index_btree_page.hpp"

namespace scratchbird::storage::database {
enum class NativePinnedCatalogReadError;
enum class NativeCatalogRelationError;
enum class NativeCreationWorkspaceError {
  none, invalid_request, invalid_device, device_not_empty, invalid_capacity,
  resource_exhausted, identity_failure, encoding_failure, hash_failure,
  io_failure, readback_mismatch, graph_failure, cluster_requires_authority,
  encrypted_requires_crypto_authority, invalid_catalog_input, catalog_capacity_exceeded
};
struct NativeCreationWorkspaceReceipt {
  core::platform::Uuid database_uuid, filespace_uuid, operation_uuid;
  core::platform::Uuid page_zero_uuid, timeline_uuid, publication_uuid;
  transaction::mga::TransactionIdentity construction_transaction;
  std::array<page::NativeCatalogRootReference,6> relations;
  std::array<core::platform::Uuid,6> relation_uuids;
  std::array<std::optional<page::NativeBtreeDependencies>,6> index_dependencies;
  core::platform::u64 total_pages=0, free_pages=0, map_pages=0;
  std::array<core::platform::u64,6> catalog_rows{};
};
struct NativeCreationWorkspaceResult {
  NativeCreationWorkspaceError error=NativeCreationWorkspaceError::invalid_request;
  std::optional<core::platform::u64> failed_page_number;
  std::optional<NativePinnedCatalogReadError> catalog_read_error;
  std::optional<NativeCatalogRelationError> catalog_relation_error;
  page::NativeBtreeError btree_error=page::NativeBtreeError::none;
  std::optional<NativeCreationWorkspaceReceipt> receipt;
  bool ok() const noexcept {return error==NativeCreationWorkspaceError::none&&receipt.has_value();}
};
// Real local control-graph genesis on an empty owned primary. The only
// committed transaction certifies construction, not populated catalogs or
// CREATE DATABASE success. Remains initializing / creating / write-fenced.
// Never opens, closes, deletes or repairs a caller's device. Failure can leave
// owned partial bytes; a subsequent call refuses a nonempty device.
NativeCreationWorkspaceResult InitializeNativeCreationWorkspaceOnOpenDevice(
    disk::FileDevice&,const NativeFilespaceInitializationRequest&,
    core::platform::u64 maximum_retained_image_bytes,
    core::uuid::StandaloneUuidV7Issuer&) noexcept;

using NativeCreationCatalogRecord = core::catalog::CatalogCreationRecord;
// Role1..6 correspond to the six native catalog relations. Inputs are complete
// owning-catalog definitions, not strings interpreted by storage. The caller
// retains admitted input memory and owns family/reference/security admission.
// One native leaf per role: a row set which does not fit is refused BEFORE I/O,
// never truncated. This bounded construction API does not replace subsequent
// indexed catalog population or ordinary CREATE activation.
using NativeCreationCatalogSeed = std::array<std::vector<NativeCreationCatalogRecord>,6>;
// SEARCH_KEY: SB_NATIVE_NAME_PATH. Sets every name entry's full path in the
// names relation (role 2, seed[1]) by the one rule (core ResolveCatalogNamePath),
// from the seed's own names. False, with the seed unchanged, when a path does not
// resolve (an unnamed ancestor, two language defaults on an ancestor, a cycle,
// too deep). Idempotent. The database create calls it where it assembles its
// seed, before its pre-write plan, so both callers of the layout planner see the
// same row sizes. The workspace writes the seed it is given and does not judge
// whether it is a complete catalog (component tests write partial ones);
// startup verification refuses a database whose stored paths do not hold.
bool FillNativeCreationNamePaths(NativeCreationCatalogSeed& seed);
NativeCreationWorkspaceResult InitializePopulatedNativeCreationWorkspaceOnOpenDevice(
    disk::FileDevice&,const NativeFilespaceInitializationRequest&,
    const NativeCreationCatalogSeed&,core::platform::u64 maximum_retained_image_bytes,
    core::uuid::StandaloneUuidV7Issuer&) noexcept;

// Storage construction inputs, NOT proof of key/dependency/security admission.
// The owning catalog retains the actual profile/map definitions and admits
// canonical raw16 row-UUID ordering before calling. Index and relation are new
// distinct owner-issued identities; dependency/profile identities are retained
// references. No dependency map digest or profile is synthesized by storage.
struct NativeCreationCatalogIndexBinding {
  core::platform::Uuid relation_uuid;
  page::NativeBtreeDependencies dependencies;
};
using NativeCreationCatalogIndexes = std::array<std::optional<NativeCreationCatalogIndexBinding>,6>;
// A missing index retains the original single-leaf bound. A supplied index
// packs complete base leaves and a real native navigation tree, built bottom-up
// with as many levels as the rows need (SB_CATALOG_BTREE_N_LEVEL). Overcapacity
// (pages or image budget) is refused before I/O, never truncated. This is
// construction, not serving activation or a general index admission/publication API.
NativeCreationWorkspaceResult InitializeIndexedNativeCreationWorkspaceOnOpenDevice(
    disk::FileDevice&,const NativeFilespaceInitializationRequest&,
    const NativeCreationCatalogSeed&,const NativeCreationCatalogIndexes&,
    core::platform::u64 maximum_retained_image_bytes,
    core::uuid::StandaloneUuidV7Issuer&) noexcept;
}  // namespace scratchbird::storage::database

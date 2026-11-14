// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_filespace_initialization.hpp"
#include "database_dirty_manifest.hpp"
#include "catalog_name_envelope.hpp"
#include "catalog_named_creation.hpp"
#include "native_index_btree_page.hpp"
#include "catalog_page.hpp"
#include <memory>

namespace scratchbird::storage::database {
enum class NativePinnedCatalogReadError;
enum class NativeCatalogRelationError;
enum class NativeCreationWorkspaceError {
  none, invalid_request, invalid_device, device_not_empty, invalid_capacity,
  resource_exhausted, identity_failure, encoding_failure, hash_failure,
  io_failure, readback_mismatch, graph_failure, cluster_requires_authority,
  encrypted_requires_crypto_authority, invalid_catalog_input, catalog_capacity_exceeded
};
// SEARCH_KEY: SB_NATIVE_NAME_TREES (owner 2026-11-09). The dependency tuples
// of the two name trees the create builds over the names relation: the
// authoritative name tree (page-zero root kind 22) and the UUID-to-name tree
// (kind 23). Both or neither: a caller that supplies them gets the trees and
// their roots; one that does not (component tests writing partial catalogs)
// gets a filespace without them.
struct NativeCreationCatalogNameTrees {
  std::optional<page::NativeBtreeDependencies> authoritative,uuid;
  bool requested() const noexcept {return authoritative.has_value()&&uuid.has_value();}
};
struct NativeCreationWorkspaceReceipt {
  core::platform::Uuid database_uuid, filespace_uuid, operation_uuid;
  core::platform::Uuid page_zero_uuid, timeline_uuid, publication_uuid;
  transaction::mga::TransactionIdentity construction_transaction;
  // SB_NATIVE_COMMENT_RELATION: one per catalog relation role, 1..7.
  std::array<page::NativeCatalogRootReference,page::kNativeCatalogRelationRoles> relations;
  std::array<core::platform::Uuid,page::kNativeCatalogRelationRoles> relation_uuids;
  std::array<std::optional<page::NativeBtreeDependencies>,page::kNativeCatalogRelationRoles> index_dependencies;
  core::platform::u64 total_pages=0, free_pages=0, map_pages=0;
  std::array<core::platform::u64,page::kNativeCatalogRelationRoles> catalog_rows{};
  // SB_NATIVE_NAME_TREES: the trees' page-zero roots (kinds 22, 23), cells and
  // pages, when they were built.
  std::array<std::optional<disk::FilespaceRootReference>,2> name_tree_roots;
  std::array<core::platform::u64,2> name_tree_cells{},name_tree_pages{};
  // SB_NATIVE_COMMENT_TREE: the comment tree's page-zero root (kind 24), when
  // it was built (empty: the create writes no comments).
  std::optional<disk::FilespaceRootReference> comment_tree_root;
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
// Role1..7 correspond to the seven native catalog relations (role 7, the
// comments relation, SB_NATIVE_COMMENT_RELATION). Inputs are complete
// owning-catalog definitions, not strings interpreted by storage. The caller
// retains admitted input memory and owns family/reference/security admission.
// One native leaf per role: a row set which does not fit is refused BEFORE I/O,
// never truncated. This bounded construction API does not replace subsequent
// indexed catalog population or ordinary CREATE activation.
using NativeCreationCatalogSeed = std::array<std::vector<NativeCreationCatalogRecord>,page::kNativeCatalogRelationRoles>;
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
using NativeCreationCatalogIndexes = std::array<std::optional<NativeCreationCatalogIndexBinding>,page::kNativeCatalogRelationRoles>;
// A missing index retains the original single-leaf bound. A supplied index
// packs complete base leaves and a real native navigation tree, built bottom-up
// with as many levels as the rows need (SB_CATALOG_BTREE_N_LEVEL). Overcapacity
// (pages or image budget) is refused before I/O, never truncated. This is
// construction, not serving activation or a general index admission/publication API.
NativeCreationWorkspaceResult InitializeIndexedNativeCreationWorkspaceOnOpenDevice(
    disk::FileDevice&,const NativeFilespaceInitializationRequest&,
    const NativeCreationCatalogSeed&,const NativeCreationCatalogIndexes&,
    core::platform::u64 maximum_retained_image_bytes,
    core::uuid::StandaloneUuidV7Issuer&,
    const NativeCreationCatalogNameTrees& name_trees={},
    // SB_NATIVE_COMMENT_TREE (2026-11-13): the dependency tuple of the comments
    // relation's (target, language) tree; with it the create builds the tree
    // (an empty root, page-zero kind 24). It needs the comments relation
    // (role 7) indexed and empty.
    const std::optional<page::NativeBtreeDependencies>& comment_tree={}) noexcept;

// SEARCH_KEY: SB_NATIVE_CREATE_WRITE_CONSUMES_PLAN (owner Q134 = 1,
// 2026-11-14). InitializeIndexedNativeCreationWorkspaceOnOpenDevice in two
// halves, so the assembly can run before the database file exists:
//  - PlanIndexedNativeCreationWorkspace runs every step before the first
//    write (validation, capacity and budget checks, the layout, every storage
//    identity from the issuer, every page image encoded and hashed) and
//    touches no device. Every refusal it can produce happens here.
//  - WriteNativeCreationWorkspacePlanOnOpenDevice consumes the plan: it checks
//    the device (owned native data file, open, writable, empty), writes the
//    images, syncs, reads back, writes page zero last and verifies the file as
//    the server opens it. Its refusals are the device's only.
// InitializeIndexedNativeCreationWorkspaceOnOpenDevice is plan + write under
// one device guard, with its refusal order unchanged. (Was: one function,
// which assembled and wrote after the caller had opened the file.)
class NativeCreationWorkspacePlan {
 public:
  struct State;  // native_creation_workspace.cpp
  NativeCreationWorkspacePlan() noexcept;
  explicit NativeCreationWorkspacePlan(std::unique_ptr<State>) noexcept;
  NativeCreationWorkspacePlan(NativeCreationWorkspacePlan&&) noexcept;
  NativeCreationWorkspacePlan& operator=(NativeCreationWorkspacePlan&&) noexcept;
  NativeCreationWorkspacePlan(const NativeCreationWorkspacePlan&)=delete;
  NativeCreationWorkspacePlan& operator=(const NativeCreationWorkspacePlan&)=delete;
  ~NativeCreationWorkspacePlan();
  bool ready() const noexcept {return state_!=nullptr;}
  std::unique_ptr<State> release() noexcept;  // defined where State is complete
 private:
  std::unique_ptr<State> state_;
};
struct NativeCreationWorkspacePlanResult {
  NativeCreationWorkspaceError error=NativeCreationWorkspaceError::invalid_request;
  std::optional<core::platform::u64> failed_page_number;
  page::NativeBtreeError btree_error=page::NativeBtreeError::none;
  NativeCreationWorkspacePlan plan;
  bool ok() const noexcept {return error==NativeCreationWorkspaceError::none&&plan.ready();}
};
NativeCreationWorkspacePlanResult PlanIndexedNativeCreationWorkspace(
    const NativeFilespaceInitializationRequest&,const NativeCreationCatalogSeed&,
    const NativeCreationCatalogIndexes&,core::platform::u64 maximum_retained_image_bytes,
    core::uuid::StandaloneUuidV7Issuer&,const NativeCreationCatalogNameTrees& name_trees={},
    const std::optional<page::NativeBtreeDependencies>& comment_tree={}) noexcept;
NativeCreationWorkspaceResult WriteNativeCreationWorkspacePlanOnOpenDevice(
    disk::FileDevice&,NativeCreationWorkspacePlan&&) noexcept;
}  // namespace scratchbird::storage::database

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_native_catalog_index.hpp"
#include "native_creation_workspace.hpp"
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::storage::database {
// SEARCH_KEY: SB_CLARIFY_NATIVE_CATALOG_INDEX_PROFILE_ROWS (owner decision
// 2026-11-04, option a). The owning creator's half of a native catalog
// relation index: the key profile, visibility profile and per-relation
// dependency map rows, and the index bindings that reference them, built
// together so they cannot disagree. Storage still admits only structure; the
// engine compares each stored index tuple against these rows when the file
// opens.
//
// The names relation (role 2) is the name/uuid catalog table, one shared object
// with the same identity in every database
// (SB_DRIFT_NAME_CATALOG_TABLE_UNIVERSAL_UUID): its relation identity is
// kNativeNameCatalogRelationUuid, never a caller's.
//
// Rows carry payloads only. Their metadata envelopes and names belong to the
// caller, like every other definition the caller submits.
struct NativeCatalogIndexRoleIdentities {
  // Nil for role 2, which takes the constant.
  core::platform::Uuid relation_uuid;
  core::platform::Uuid index_uuid,map_uuid;
};
// SB_NATIVE_NAME_TREES: the issued identities of the two name trees' indexes
// and dependency maps; their key profiles are the fixed values.
struct NativeCatalogNameTreeIdentities {
  core::platform::Uuid authoritative_index_uuid,authoritative_map_uuid,uuid_index_uuid,uuid_map_uuid;
};
struct NativeCatalogIndexRowsRequest {
  core::platform::Uuid database_uuid;
  core::platform::TypedUuid construction_transaction;
  core::platform::u64 construction_local_transaction_id=1;
  // Position i is catalog relation role i+1; an absent role is not indexed.
  std::array<std::optional<NativeCatalogIndexRoleIdentities>,6> roles;
  // Absent: no name trees (component fixtures).
  std::optional<NativeCatalogNameTreeIdentities> name_trees;
};
struct NativeCatalogIndexRow {
  core::platform::Uuid object_uuid;
  std::string_view object_subtype;
  std::string payload;
};
enum class NativeCatalogIndexRowsError {
  none,invalid_request,duplicate_identity,encoding_failure,hash_failure,resource_exhausted
};
struct NativeCatalogIndexRowsResult {
  NativeCatalogIndexRowsError error=NativeCatalogIndexRowsError::invalid_request;
  // The key profile, the visibility profile, then one dependency map per
  // indexed role in role order; with name trees, then their two key profiles
  // and two dependency maps. Empty when nothing is indexed.
  std::vector<NativeCatalogIndexRow> rows;
  NativeCreationCatalogIndexes indexes;
  NativeCreationCatalogNameTrees name_trees;
  bool ok() const noexcept {return error==NativeCatalogIndexRowsError::none;}
};
NativeCatalogIndexRowsResult BuildNativeCatalogIndexRows(const NativeCatalogIndexRowsRequest&) noexcept;
} // namespace scratchbird::storage::database

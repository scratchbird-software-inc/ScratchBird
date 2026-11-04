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
struct NativeCatalogIndexRowsRequest {
  core::platform::Uuid database_uuid;
  core::platform::TypedUuid construction_transaction;
  core::platform::u64 construction_local_transaction_id=1;
  // Position i is catalog relation role i+1; an absent role is not indexed.
  std::array<std::optional<NativeCatalogIndexRoleIdentities>,6> roles;
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
  // indexed role in role order. Empty when no role is indexed.
  std::vector<NativeCatalogIndexRow> rows;
  NativeCreationCatalogIndexes indexes;
  bool ok() const noexcept {return error==NativeCatalogIndexRowsError::none;}
};
NativeCatalogIndexRowsResult BuildNativeCatalogIndexRows(const NativeCatalogIndexRowsRequest&) noexcept;
} // namespace scratchbird::storage::database

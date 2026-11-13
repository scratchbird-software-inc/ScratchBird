// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_native_catalog_index.hpp"
#include "catalog_page.hpp"
#include <optional>

namespace scratchbird::storage::database {
// SEARCH_KEY: SB_NATIVE_COMMENT_RELATION (owner Q77 = 2, 2026-11-13). The
// catalog relations whose identity is a universal constant, the same in every
// database: the names relation (role 2, SB_DRIFT_NAME_CATALOG_TABLE_UNIVERSAL_UUID)
// and the comments relation (role 7). Their identity is known without reading
// a row, so a reader or writer may use it for such a relation when it is
// indexed and still empty (the comments relation is empty at create, and its
// empty index reaches no base leaf to read an identity from). Every other
// relation's identity is issued per database and read from its leaves.
inline std::optional<core::platform::Uuid> NativeCatalogConstantRelationUuid(core::platform::u16 role) noexcept {
  if (role == 2) return core::catalog::kNativeNameCatalogRelationUuid;
  if (role == page::kNativeCommentRelationRole) return core::catalog::kNativeCommentCatalogRelationUuid;
  return std::nullopt;
}
}  // namespace scratchbird::storage::database

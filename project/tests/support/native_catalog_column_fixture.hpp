// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../../src/engine/internal_api/catalog/column_metadata_codec.hpp"
#include <cstdlib>
namespace scratchbird::tests {
inline std::string NativeCatalogColumnFixture(
    engine::internal_api::CatalogColumnMetadata fields) {
  std::string bytes;
  if (!engine::internal_api::EncodeCatalogColumnMetadata(fields, &bytes)) std::abort();
  return bytes;
}
}  // namespace scratchbird::tests

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "binary_catalog_metadata.hpp"
namespace scratchbird::engine::internal_api {
using CatalogConstraintMetadata = BinaryCatalogMetadata;
inline bool EncodeCatalogConstraintMetadata(const CatalogConstraintMetadata& fields, std::string* output) {
  return EncodeBinaryCatalogMetadata(fields, "constraint.v2", output);
}
inline bool DecodeCatalogConstraintMetadata(std::string_view bytes, CatalogConstraintMetadata* output) {
  return DecodeBinaryCatalogMetadata(bytes, "constraint.v2", output);
}
inline EngineUuid CatalogConstraintUuid(const CatalogConstraintMetadata& fields, const std::string& key,
                                       EngineUuid fallback = {}) {
  return BinaryCatalogUuid(fields, key, fallback);
}
}  // namespace scratchbird::engine::internal_api

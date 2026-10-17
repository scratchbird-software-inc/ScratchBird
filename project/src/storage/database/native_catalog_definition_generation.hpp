// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_crypto_inline_private_key_policy.hpp"
#include "catalog_policy_descriptor.hpp"
#include <limits>

namespace scratchbird::storage::database {
// Structural successor calculation only, never MGA visibility or admission.
// Policy semantic definitions can remain unchanged while common metadata gets
// a new physical row version. Their exact payload must remain byte-identical;
// no other family's existing increment rule is relaxed.
inline bool NativeCatalogDefinitionGenerationFollows(
    const core::catalog::CatalogMetadataVersion& before,
    const core::catalog::CatalogMetadataVersion& after) {
  namespace c=core::catalog;
  if(before.definition_version==after.definition_version &&
      (before.object_subtype=="policy_descriptor" || before.object_subtype=="crypto_inline_private_key"))
    return before.record.payload==after.record.payload &&
        c::CatalogMetadataPreservesFamilyOrigin(before,after);
  return before.definition_version!=std::numeric_limits<core::platform::u64>::max() &&
      after.definition_version==before.definition_version+1;
}
} // namespace scratchbird::storage::database

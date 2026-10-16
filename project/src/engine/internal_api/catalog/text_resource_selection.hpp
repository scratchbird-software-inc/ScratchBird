// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog/name_resolution_api.hpp"

#include <optional>
#include <functional>

namespace scratchbird::core::resources { struct ResourceSeedCatalogImage; }

namespace scratchbird::engine::internal_api {

// Metadata lookup selectors only, not authorization or profile receipts.
// A present pair requires a charset; nil collation requests that charset's
// registered default. Absence, rather than nil charset, permits inheritance.
struct EngineTextResourceSelectorsV1 {
  EngineUuid charset_uuid;
  EngineUuid collation_uuid;
};
struct EngineTextResourceSelectionSourcesV1 {
  std::optional<EngineTextResourceSelectorsV1> explicit_bound;
  std::optional<EngineTextResourceSelectorsV1> session_override;
  std::optional<EngineTextResourceSelectorsV1> current_schema;
  std::optional<EngineTextResourceSelectorsV1> node_database;
};
enum class EngineTextResourceSelectionSourceV1 : std::uint8_t {
  none, explicit_bound, session_override, current_schema, node_database
};
struct EngineTextResourceSelectionResultV1 {
  EngineTextResourcePairLookupResult pair;
  EngineTextResourceSelectionSourceV1 source =
      EngineTextResourceSelectionSourceV1::none;
};

// CTB-TEXT-PROFILE-SELECTION-PRECEDENCE-20261016. First present pair wins;
// resolve its optional collation from its own charset, never a weaker source.
// A failed winner does not fall through or publish a source. This function
// resolves actual physical resource metadata, not session/default authority.
// The receipt owner must obtain admitted defaults from their owning providers
// and validate the resulting bound generations before issuing/using profiles.
EngineTextResourceSelectionResultV1 SelectEngineTextResourcePairV1(
    const EngineRequestContext& context,
    const EngineTextResourceSelectionSourcesV1& sources);

class EngineTextResourceReadScopeV1;
using EngineTextResourceCatalogVisitorV1 =
    std::function<void(const EngineTextResourceReadScopeV1&)>;

// One actual physical admission/image for a synchronous group of metadata
// lookups. Not a cache or execution permission. The visitor must not retain
// the scope, detach work, or change/finish its owning transaction. No device
// fence is held during the visitor. Exceptions unwind the owning image.
EngineApiDiagnostic WithEngineTextResourceCatalogV1(
    const EngineRequestContext& context,
    const EngineTextResourceCatalogVisitorV1& visitor);

class EngineTextResourceReadScopeV1 final {
 public:
  EngineTextResourceReadScopeV1(const EngineTextResourceReadScopeV1&) = delete;
  EngineTextResourceReadScopeV1& operator=(const EngineTextResourceReadScopeV1&) = delete;
  EngineTextResourcePairLookupResult LookupPair(
      const EngineUuid& charset, const EngineUuid& collation) const;
  EngineTextResourceSelectionResultV1 SelectPair(
      const EngineTextResourceSelectionSourcesV1& sources) const;
 private:
  friend EngineApiDiagnostic WithEngineTextResourceCatalogV1(
      const EngineRequestContext&, const EngineTextResourceCatalogVisitorV1&);
  EngineTextResourceReadScopeV1(const EngineUuid& database,
      const core::resources::ResourceSeedCatalogImage& image) noexcept
      : database_(database), image_(image) {}
  const EngineUuid database_;
  const core::resources::ResourceSeedCatalogImage& image_;
};

}  // namespace scratchbird::engine::internal_api

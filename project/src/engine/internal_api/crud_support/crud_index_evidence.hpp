// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog/binary_catalog_metadata.hpp"
#include "core/datatypes/canonical_utf8.hpp"
#include <array>
#include <stdexcept>

namespace scratchbird::engine::internal_api {
// The lookup's binary metadata is an internal structured carrier, not a UTF-8
// evidence label. Preserve its native identity and every observation separately.
inline void AppendCrudIndexEvidence(
    std::vector<EngineEvidenceReference>* output, std::string_view encoded,
    std::string_view kind = "index_lookup") {
  constexpr std::array<std::string_view, 8> names{
      "index_family", "index_profile", "predicate_kind", "candidate_count",
      "visible_count", "row_recheck_applied", "exactness", "fallback_mode"};
  BinaryCatalogMetadata fields;
  if (!output || (kind != "index_lookup" &&
                 kind != "transactional_index_resolution_evidence") ||
      !DecodeBinaryCatalogMetadata(encoded, "crud.index_evidence.v2", &fields) ||
      fields.identities.size() != 1 || !fields.identities.contains("index_uuid") ||
      !scratchbird::core::uuid::IsEngineIdentityUuid(fields.identities.at("index_uuid")) ||
      fields.text.size() != names.size()) {
    throw std::invalid_argument("crud_index_evidence_carrier_invalid");
  }
  std::vector<EngineEvidenceReference> projected;
  projected.reserve(names.size() + 1);
  projected.push_back({std::string(kind), fields.identities.at("index_uuid")});
  for (const auto name : names) {
    const auto found = fields.text.find(std::string(name));
    if (found == fields.text.end() || found->second.empty() ||
        found->second.find('\0') != std::string::npos ||
        !scratchbird::core::datatypes::ValidateCanonicalUtf8(
            reinterpret_cast<const std::uint8_t*>(found->second.data()),
            found->second.size(), nullptr)) {
      throw std::invalid_argument("crud_index_evidence_observation_invalid");
    }
    projected.push_back({std::string(kind) + "." + std::string(name), found->second});
  }
  output->insert(output->end(), projected.begin(), projected.end());
}
}  // namespace scratchbird::engine::internal_api

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"
#include "catalog/datatype_bootstrap_identity.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include "uuid.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace scratchbird::tests {

inline engine::internal_api::EngineUuid ParsedFixtureUuid(std::string text) {
  const auto parsed = core::uuid::ParseUuid(text);
  if (!parsed.ok()) throw std::runtime_error("fixture UUID text is invalid: " + text);
  return parsed.value;
}

inline engine::internal_api::EngineDescriptor ExactScalarDescriptorFixture(
    const core::datatypes::CanonicalTypeId type_id,
    std::string canonical_type_name,
    const engine::internal_api::EngineUuid& occurrence_uuid,
    std::string encoded_descriptor) {
  namespace api = engine::internal_api;
  namespace dt = core::datatypes;
  const dt::DatatypeTypeCodecIdentityRowV1* selected = nullptr;
  // This is a frozen representation fixture, not a live statement receipt.
  for (const auto& entry : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& row = entry.legacy_fields;
    if (row.catalog_snapshot_uuid != dt::kDatatypeCohortV5 ||
        row.catalog_generation != 5 || row.registry_generation != 5 ||
        row.canonical_binary_type_code !=
            static_cast<std::uint32_t>(type_id)) {
      continue;
    }
    if (selected != nullptr) {
      throw std::runtime_error("datatype fixture identity is ambiguous");
    }
    selected = &row;
  }
  if (selected == nullptr) {
    throw std::runtime_error("datatype fixture identity is unavailable");
  }
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid = occurrence_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(canonical_type_name);
  descriptor.encoded_descriptor = std::move(encoded_descriptor);
  descriptor.type_uuid = selected->type_uuid;
  descriptor.datatype_descriptor_uuid = selected->descriptor_uuid;
  descriptor.datatype_descriptor_generation = selected->descriptor_generation;
  descriptor.datatype_cohort = {selected->catalog_snapshot_uuid,
      selected->catalog_generation, selected->registry_generation};
  return descriptor;
}

}  // namespace scratchbird::tests

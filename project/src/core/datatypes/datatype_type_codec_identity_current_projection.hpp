// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

// SEARCH_KEY: SB_DATATYPE_V1_CONSUMER_CURRENT_COHORT
//
// Consumers that still read the frozen V1 codec-identity carrier. The V1 table
// holds only the historical cohorts (V3-V5); since the bootstrap cohort moved
// to V11 (0f9dfc97c) every such consumer missed the current rows and refused
// valid descriptors. These helpers keep the V1 contract and reach current rows
// through the designed one-way projection, ProjectDatatypeTypeCodecIdentityV3ToV1,
// which refuses a row V1 cannot represent. Matching stays exact: no default
// snapshot, no nearest generation. Consumers that need the V3 policy fields
// (ordering, hash, operation) must read V3 directly; V1 never carried them.

#include "datatype_catalog_manifest.hpp"
#include "datatype_type_codec_identity_v3.hpp"

#include <span>
#include <vector>

namespace scratchbird::core::datatypes {

namespace current_projection_detail {
inline bool SameV1Key(const DatatypeTypeCodecIdentityRowV1& a,
                      const DatatypeTypeCodecIdentityRowV1& b) {
  return a.catalog_snapshot_uuid == b.catalog_snapshot_uuid &&
         a.catalog_generation == b.catalog_generation &&
         a.registry_generation == b.registry_generation &&
         a.descriptor_uuid == b.descriptor_uuid &&
         a.descriptor_generation == b.descriptor_generation;
}
}  // namespace current_projection_detail

// Historical V1 rows plus the V1 projection of every current V3 row not already
// present. Built once; the rows live for the process, like the V1 table.
inline std::span<const DatatypeTypeCodecIdentityRowV1>
CurrentDatatypeTypeCodecIdentityRowsAsV1() {
  static const std::vector<DatatypeTypeCodecIdentityRowV1> rows = [] {
    std::vector<DatatypeTypeCodecIdentityRowV1> out(
        CurrentDatatypeTypeCodecIdentityRowsV1().begin(),
        CurrentDatatypeTypeCodecIdentityRowsV1().end());
    const auto historical = out.size();
    for (const auto& current : CurrentDatatypeTypeCodecIdentityRowsV3()) {
      const auto projected = ProjectDatatypeTypeCodecIdentityV3ToV1(current);
      if (!projected.ok) continue;
      bool present = false;
      for (std::size_t i = 0; i < historical && !present; ++i)
        present = current_projection_detail::SameV1Key(out[i], projected.row);
      if (!present) out.push_back(projected.row);
    }
    return out;
  }();
  return rows;
}

// Exact lookup over the same population.
inline DatatypeTypeCodecIdentityLookupV1 LookupCurrentDatatypeTypeCodecIdentityAsV1(
    const scratchbird::core::platform::Uuid& catalog_snapshot_uuid,
    u64 catalog_generation, u64 registry_generation,
    const scratchbird::core::platform::Uuid& descriptor_uuid,
    u64 descriptor_generation) {
  auto historical = LookupDatatypeTypeCodecIdentityV1(
      catalog_snapshot_uuid, catalog_generation, registry_generation,
      descriptor_uuid, descriptor_generation);
  if (historical.ok) return historical;
  const auto* current = FindDatatypeTypeCodecIdentityV3(
      catalog_snapshot_uuid, catalog_generation, registry_generation,
      descriptor_uuid, descriptor_generation);
  if (current == nullptr) return historical;
  auto projected = ProjectDatatypeTypeCodecIdentityV3ToV1(*current);
  DatatypeTypeCodecIdentityLookupV1 result;
  if (!projected.ok) {
    result.diagnostic_id = std::string(projected.diagnostic_id);
    return result;
  }
  result.ok = true;
  result.row = std::move(projected.row);
  return result;
}

}  // namespace scratchbird::core::datatypes

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_catalog_manifest.hpp"
#include "admitted_datatype_cohort.hpp"
#include <algorithm>

namespace scratchbird::core::datatypes {

// Catalog identity for retained storage/index values. This is deliberately
// not a DatatypeTypeCodecIdentityRowV1: no literal codec is fabricated.
struct DatatypeStorageIdentityV1 {
  platform::Uuid descriptor_uuid;
  u64 descriptor_generation = 0;
  platform::Uuid type_uuid;
  CanonicalTypeId type_id = CanonicalTypeId::unknown;
};

inline bool LookupDatatypeStorageIdentityV1(
    const platform::Uuid& snapshot, u64 catalog_generation, u64 registry_generation,
    const platform::Uuid& descriptor, u64 descriptor_generation,
    DatatypeStorageIdentityV1* output) {
  if (!output) return false;
  const auto codec = LookupDatatypeTypeCodecIdentityV1(
      snapshot, catalog_generation, registry_generation, descriptor, descriptor_generation);
  DatatypeStorageIdentityV1 selected;
  if (codec.ok) {
    if (codec.row.canonical_binary_type_code == static_cast<u32>(CanonicalTypeId::unknown))
      return false;
    selected = {codec.row.descriptor_uuid, codec.row.descriptor_generation,
                codec.row.type_uuid, static_cast<CanonicalTypeId>(codec.row.canonical_binary_type_code)};
  } else {
    const auto registry = CurrentDatatypeTypeCodecIdentityRowsV1();
    // A known codec descriptor never downgrades to storage-only admission.
    if (std::any_of(registry.begin(), registry.end(), [&](const auto& row) {
          return row.descriptor_uuid == descriptor;
        }) || snapshot != kDatatypeCohortV4 || catalog_generation != 4 || registry_generation != 4)
      return false;
    static const auto catalog = LoadCurrentCoreDatatypeCatalogManifest();
    if (!catalog.ok()) return false;
    const DatatypeCatalogDescriptorRow* row = nullptr;
    for (const auto& candidate : catalog.manifest.descriptor_rows) {
      if (candidate.descriptor_uuid.value != descriptor) continue;
      if (row != nullptr) return false;
      row = &candidate;
    }
    if (!row || !row->descriptor_authoritative || row->descriptor_epoch != descriptor_generation ||
        row->type_id == CanonicalTypeId::unknown)
      return false;
    selected = {row->descriptor_uuid.value, row->descriptor_epoch,
                row->descriptor_uuid.value, row->type_id};
  }
  *output = selected;
  return true;
}
}  // namespace scratchbird::core::datatypes

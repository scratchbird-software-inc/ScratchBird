// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_catalog_manifest.hpp"
#include "admitted_datatype_cohort.hpp"
#include <algorithm>
#include <optional>

namespace scratchbird::core::datatypes {

// Catalog identity for retained storage/index values. This is deliberately
// not a DatatypeTypeCodecIdentityRowV1: no literal codec is fabricated.
struct DatatypeStorageIdentityV1 {
  platform::Uuid descriptor_uuid;
  u64 descriptor_generation = 0;
  platform::Uuid type_uuid;
  CanonicalTypeId type_id = CanonicalTypeId::unknown;
  std::optional<DatatypeTypeCodecIdentityRowV1> codec;
};

inline bool LookupDatatypeStorageIdentityV1(
    const platform::Uuid& snapshot, u64 catalog_generation, u64 registry_generation,
    const platform::Uuid& descriptor, u64 descriptor_generation,
    DatatypeStorageIdentityV1* output) {
  if (!output) return false;
  const auto codec = LookupDatatypeTypeCodecIdentityV3(
      snapshot, catalog_generation, registry_generation, descriptor, descriptor_generation);
  DatatypeStorageIdentityV1 selected;
  if (codec.ok) {
    const auto& legacy = codec.row.legacy_fields;
    if (legacy.canonical_binary_type_code == static_cast<u32>(CanonicalTypeId::unknown))
      return false;
    selected = {legacy.descriptor_uuid, legacy.descriptor_generation,
                legacy.type_uuid,
                static_cast<CanonicalTypeId>(legacy.canonical_binary_type_code),
                ProjectDatatypeTypeCodecIdentityV3ToV1(codec.row)};
  } else {
    const auto registry = CurrentDatatypeTypeCodecIdentityRowsV3();
    // A codec admitted in this or an earlier cohort never downgrades to
    // storage-only admission. A successor must not retroactively change V4's
    // decimal storage identity or pretend it had a canonical value codec.
    const bool codec_admitted_at_or_before =
        std::any_of(registry.begin(), registry.end(), [&](const auto& row) {
          return row.legacy_fields.descriptor_uuid == descriptor &&
                 row.legacy_fields.catalog_generation <= catalog_generation;
        });
    const bool codec_admitted_in_later_cohort =
        std::any_of(registry.begin(), registry.end(), [&](const auto& row) {
          return row.legacy_fields.descriptor_uuid == descriptor &&
                 row.legacy_fields.catalog_generation > catalog_generation;
        });
    const platform::Uuid decimal_float_storage_only_predecessor{{
        0xa1,0x00,0x00,0x00,0x10,0x65,0x73,0x69,
        0xad,0x61,0x6c,0x5f,0x66,0x6c,0x6f,0x61}};
    if (codec_admitted_at_or_before ||
        (codec_admitted_in_later_cohort &&
         descriptor != decimal_float_storage_only_predecessor) ||
        !((snapshot == kDatatypeCohortV4 && catalog_generation == 4 && registry_generation == 4) ||
                (snapshot == kDatatypeCohortV5 && catalog_generation == 5 && registry_generation == 5) ||
                (snapshot == kDatatypeCohortV6 && catalog_generation == 6 && registry_generation == 6)))
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
                row->descriptor_uuid.value, row->type_id, std::nullopt};
  }
  *output = selected;
  return true;
}
}  // namespace scratchbird::core::datatypes

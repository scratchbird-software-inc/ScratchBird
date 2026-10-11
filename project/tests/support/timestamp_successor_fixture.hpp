// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../../src/core/datatypes/datatype_timestamp.hpp"
#include <memory>
#include <stdexcept>

namespace scratchbird::tests {
inline std::shared_ptr<const core::datatypes::TimestampValidatedProfileHandleV3>
D711TimestampProfile() {
  namespace dt = core::datatypes;
  const dt::DatatypeTypeCodecIdentityRowV3* identity = nullptr;
  for (const auto& row : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    if (row.legacy_fields.catalog_snapshot_uuid == dt::kDatatypeCohortV11 &&
        row.legacy_fields.canonical_binary_type_code ==
            static_cast<core::platform::u32>(dt::CanonicalTypeId::timestamp)) {
      if (identity != nullptr) throw std::runtime_error("duplicate D711 TIMESTAMP");
      identity = &row;
    }
  }
  if (identity == nullptr) throw std::runtime_error("missing D711 TIMESTAMP");
  auto result = dt::BuildTimestampValidatedProfileHandleV3(
      {dt::kDatatypeCohortV11, dt::kDatatypeCohortV11, 11, 11}, *identity);
  if (!result.ok()) throw std::runtime_error("D711 TIMESTAMP admission failed");
  return std::make_shared<const dt::TimestampValidatedProfileHandleV3>(
      std::move(result.profile));
}
}  // namespace scratchbird::tests

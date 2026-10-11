// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../../src/core/datatypes/datatype_type_codec_identity_v3.hpp"
#include <memory>
#include <stdexcept>

namespace scratchbird::tests {
template <typename Profile, typename Write>
void CheckTemporalProfileStringOverlap(const Profile& original, Write write) {
  using Row = core::datatypes::DatatypeTypeCodecIdentityRowV1;
  for (const auto member : {&Row::codec_id, &Row::canonical_name,
       &Row::canonical_byte_order, &Row::canonical_representation,
       &Row::canonical_charset, &Row::invalid_encoding_diagnostic_id,
       &Row::comparison_profile}) {
    auto profile = std::make_shared<Profile>(original);
    auto& value = profile->identity.legacy_fields.*member;
    if (member == &Row::codec_id || member == &Row::canonical_name) value.assign(128, 'L');
    value.reserve(value.size()+256);
    const auto before = value;
    for (const auto offset : {std::size_t{0}, value.size(), value.size()+1}) {
      // Every write fits the actual allocation. Even reserved storage is an
      // input buffer, not an eligible output destination.
      auto result = write(profile, reinterpret_cast<core::platform::byte*>(value.data()+offset));
      if (result.ok() || result.diagnostic.diagnostic_code != "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID" ||
          value != before || value.data()[value.size()] != '\0')
        throw std::runtime_error("temporal profile string overlap accepted or changed input");
    }
  }
}
}  // namespace scratchbird::tests

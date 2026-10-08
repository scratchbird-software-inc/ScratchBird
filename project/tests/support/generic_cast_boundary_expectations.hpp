// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_operations.hpp"
#include <optional>
#include <string>
#include <string_view>

namespace scratchbird::tests {

struct CastRefusalExpectation {
  std::string_view code;
  std::string_view detail;
};

// Independent exact oracle for calls through the generic V1/V2 operation
// boundary. The current BLOB, temporal and BIT profiles require their bound
// APIs; their boundary refusal precedes an unrelated scalar policy refusal.
// This is NOT an oracle for the supported profile-aware casts themselves.
inline std::optional<CastRefusalExpectation> GenericCastProfileBoundary(
    core::datatypes::CanonicalTypeId type) {
  using T = core::datatypes::CanonicalTypeId;
  switch (type) {
    case T::blob:
      return CastRefusalExpectation{"BLOB.V1_V2_REFUSED", "base_blob_v1_v2_route_refused"};
    case T::date:
      return CastRefusalExpectation{"CTI.TEMPORAL.DESCRIPTOR_INVALID", "date_d710_profile_required"};
    case T::time:
      return CastRefusalExpectation{"CTI.TEMPORAL.DESCRIPTOR_INVALID", "time_d710_profile_required"};
    case T::timestamp:
      return CastRefusalExpectation{"CTI.TEMPORAL.DESCRIPTOR_INVALID", "timestamp_d710_profile_required"};
    case T::interval:
      return CastRefusalExpectation{"CTI.INTERVAL.DESCRIPTOR_INVALID", "interval_d710_profile_required"};
    case T::bit_string:
      return CastRefusalExpectation{"CTB.BIT.DESCRIPTOR_INVALID", "bit_string_v3_profile_required"};
    default:
      return std::nullopt;
  }
}

// Independent finite +1 component fixtures, not production encoders. They
// distinguish a valid but unadmitted numeric pair from a malformed carrier.
inline std::optional<std::string> CanonicalDecimalPeerOne(
    core::datatypes::CanonicalTypeId type) {
  using T = core::datatypes::CanonicalTypeId;
  if (type == T::decimal) {
    std::string bytes(24, '\0');
    bytes[1] = 1; bytes[2] = 1; bytes[4] = 1;
    return bytes;
  }
  if (type == T::decimal_float) {
    std::string bytes(16, '\0');
    bytes[0] = 1; bytes[14] = 0x40; bytes[15] = 0x30;
    return bytes;
  }
  return std::nullopt;
}

}  // namespace scratchbird::tests

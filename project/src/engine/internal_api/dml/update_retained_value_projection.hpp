// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include <bit>
#include <charconv>
#include <string_view>

namespace scratchbird::engine::internal_api {
// The caller has revalidated the column's exact catalog/type/codec cohort.
// Canonical UPDATE carriers own native bytes only; the legacy retained-row
// executor has decimal integer and UTF-8 text payload profiles. Project once
// at that boundary, without keeping a second authoritative payload in either.
inline bool ProjectBoundUpdateValueToRetained(
    const EngineTypedValue& native, std::string_view codec,
    EngineTypedValue* output) {
  if (!output || !native.encoded_value.empty()) return false;
  const bool text = codec == "datatype.text.utf8.v1";
  const std::size_t width = codec == "datatype.int32.le.v1" ? 4 :
      codec == "datatype.int64.le.v1" ? 8 : 0;
  if (!text && !width) return false;
  std::string payload;
  if (native.isSqlNull()) {
    if (!native.binary_value.empty()) return false;
  } else {
    if (native.state != EngineValueState::value) return false;
    if (text) {
      payload.assign(native.binary_value.begin(), native.binary_value.end());
    } else {
      if (native.binary_value.size() != width) return false;
      std::uint64_t bits = 0;
      for (std::size_t i = 0; i < width; ++i)
        bits |= std::uint64_t(native.binary_value[i]) << (i * 8);
      const std::int64_t value = width == 4
          ? std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bits))
          : std::bit_cast<std::int64_t>(bits);
      char decimal[32];
      const auto formatted = std::to_chars(decimal, decimal + sizeof(decimal), value);
      if (formatted.ec != std::errc{}) return false;
      payload.assign(decimal, formatted.ptr);
    }
  }
  auto retained = native;
  retained.binary_value.clear();
  retained.encoded_value = std::move(payload);
  *output = std::move(retained);
  return true;
}
}  // namespace scratchbird::engine::internal_api

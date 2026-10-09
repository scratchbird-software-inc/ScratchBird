// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include <string_view>

namespace scratchbird::engine::internal_api {
// The caller has revalidated the column's exact catalog/type/codec cohort.
// Canonical UPDATE integers and retained storage share native bytes. Only the
// UTF-8 character payload changes its owning arm; no numeric rendering belongs
// at this boundary. Keep a single authoritative payload in either case.
inline bool ProjectBoundUpdateValueToRetained(
    const EngineTypedValue& native, std::string_view codec,
    EngineTypedValue* output) {
  if (!output || !native.encoded_value.empty()) return false;
  const bool text = codec == "datatype.text.utf8.v1";
  const std::size_t width = codec == "datatype.int32.le.v1" ? 4 :
      codec == "datatype.int64.le.v1" ? 8 : 0;
  if (!text && !width) return false;
  if (native.isSqlNull()) {
    if (!native.binary_value.empty()) return false;
  } else {
    if (native.state != EngineValueState::value) return false;
    if (!text && native.binary_value.size() != width) return false;
  }
  if (!text) {
    if (output != &native) *output = native;
    return true;
  }
  auto retained = native;
  retained.encoded_value.assign(native.binary_value.begin(), native.binary_value.end());
  retained.binary_value.clear();
  *output = std::move(retained);
  return true;
}
}  // namespace scratchbird::engine::internal_api

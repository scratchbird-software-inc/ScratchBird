// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../core/uuid/uuid.hpp"
#include <algorithm>
#include <string_view>

namespace scratchbird::server {
inline bool ReadNativeIdentitySelector(std::string_view bytes,
                                       core::platform::Uuid* output) {
  if (!output || bytes.size() != 16) return false;
  core::platform::Uuid identity;
  std::copy(bytes.begin(), bytes.end(), identity.bytes.begin());
  if (!core::uuid::IsEngineIdentityUuid(identity)) return false;
  *output = identity;
  return true;
}
inline bool NativeIdentitySelectorMatches(std::string_view bytes,
                                          const core::platform::Uuid& expected) {
  core::platform::Uuid selected;
  return ReadNativeIdentitySelector(bytes, &selected) && selected == expected;
}
} // namespace scratchbird::server

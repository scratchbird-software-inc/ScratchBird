// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../../src/core/platform/runtime_platform.hpp"

#include <cstdint>

namespace scratchbird::tests {

// Fixed binary test identities, not a runtime UUID generator. The namespace and
// ordinal are explicit fixture coordinates, never hashes of identity labels.
constexpr core::platform::Uuid FixtureUuid(std::uint32_t domain,
                                          std::uint32_t ordinal) noexcept {
  core::platform::Uuid uuid{};
  uuid.bytes[0] = 0x01;
  uuid.bytes[1] = 0x9d;
  uuid.bytes[6] = 0x70;
  uuid.bytes[8] = 0x80;
  for (unsigned i = 0; i < 4; ++i) {
    uuid.bytes[2 + i] = static_cast<core::platform::byte>(domain >> (24 - 8 * i));
    uuid.bytes[12 + i] = static_cast<core::platform::byte>(ordinal >> (24 - 8 * i));
  }
  return uuid;
}

static_assert(!FixtureUuid(1, 1).is_nil());
static_assert(FixtureUuid(1, 1) != FixtureUuid(1, 2));
static_assert(FixtureUuid(1, 1) != FixtureUuid(2, 1));

}  // namespace scratchbird::tests

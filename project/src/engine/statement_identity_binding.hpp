// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "engine/sblr/sblr_literal_runtime.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <optional>
#include <span>

namespace scratchbird::engine {

// Private receipt/wire comparison; identity shape does not establish authority.
// The caller must first validate the containing packet and retained owner.
inline bool MatchesStatementIdentity(
    const core::platform::Uuid& identity,
    std::span<const std::uint8_t> wire,
    bool optional = false) noexcept {
  return wire.size() == identity.bytes.size() &&
         (core::uuid::IsEngineIdentityUuid(identity) ||
          (optional && identity.is_nil())) &&
         std::equal(identity.bytes.begin(), identity.bytes.end(), wire.begin());
}

// Exact registered descriptor selection for the existing SBLP v1 integer and
// exact-decimal demand classes. No catalog epoch, generation or visibility is
// supplied here: the caller must perform the owning catalog lookup afterwards.
inline std::optional<core::platform::Uuid> LiteralDemandDescriptorIdentityV1(
    const sblr::SblrLiteralDemandV1& demand) noexcept {
  if (sblr::IsAdmittedBigintLiteralDemandV1(demand))
    return core::platform::Uuid{{0x01, 0x9d, 0, 0, 0, 0, 0x70, 0,
                                0x80, 0, 0, 0, 0, 0, 0xd7, 0x11}};
  if (sblr::IsAdmittedExactDecimalLiteralDemandV1(demand))
    return core::platform::Uuid{{0xa0, 0, 0, 0, 0x64, 0x65, 0x73, 0x69,
                                0xad, 0x61, 0x6c, 0, 0, 0, 0, 0}};
  return std::nullopt;
}

}  // namespace scratchbird::engine

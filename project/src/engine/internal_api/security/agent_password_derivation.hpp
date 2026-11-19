// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_AGENT_PASSWORD_DERIVED (owner Q274, 2026-11-19: "Q237 uses the
// database UUID, xor'd with the security tables password's uuid. This is
// derivable from the database. As the agents are designed to run as threads,
// we can also say they can not have a connection outside the database itself").
// The ONE derivation of an internal (agent / L5) principal's password: the
// database's UUID (page zero, SB_ONE_DATABASE_UUID) XOR the principal's stored
// password seed (its security row, SB_AGENT_PASSWORD_SEED). The create mints
// the seed (16 CSPRNG bytes, Q276) so this result holds no zero byte - the
// shared password derivation and verifier refuse a NUL - and stores the
// PBKDF2 verifier of this result; a startup path derives the password from the
// file and the row and authenticates through the normal verifier path.
// Nothing outside the engine holds the password: the receipt does not carry it.
// The derived password is a secret: callers cleanse it.
inline std::array<unsigned char, 16> DeriveAgentPassword(const core::platform::Uuid& database_uuid,
                                                         std::span<const core::platform::byte, 16> seed) noexcept {
  std::array<unsigned char, 16> password{};
  for (std::size_t i = 0; i < password.size(); ++i)
    password[i] = static_cast<unsigned char>(static_cast<unsigned char>(database_uuid.bytes[i]) ^
                                             static_cast<unsigned char>(seed[i]));
  return password;
}
// Whether a derived password is admissible as a local-password secret (no NUL).
inline bool AgentPasswordAdmissible(std::span<const unsigned char, 16> password) noexcept {
  return std::none_of(password.begin(), password.end(), [](unsigned char b) { return b == 0; });
}

}  // namespace scratchbird::engine::internal_api

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "openpgp_packet.hpp"

namespace scratchbird::core::crypto {
// Internal, already-admitted password message mechanism; no runtime defaults,
// memory grant, public entropy override, authorization or publication authority.
// All borrowed inputs remain immutable through the complete call, including
// cancellation callbacks. The owner prevents concurrent mutation and release.
// Native single-password/one-literal grammar: v6 type4 SKESK, then v2 SEIPD.
// Wrapped-key cipher and SEIPD cipher must agree on session-key length, but
// wrapping and data cipher profiles need not be identical.
struct PasswordMessageProfile {
  SeipdProfile data{};
  std::uint8_t wrapping_cipher = 9, wrapping_aead = 2;
  Argon2S2kProfile s2k{};
};
struct PasswordMessageEntropy {
  PgpInput session_key, password_salt, wrapping_nonce, data_salt;
};
struct PasswordMessageSize {
  PgpCode code = PgpCode::invalid_packet;
  std::size_t message_bytes = 0, literal_bytes = 0, workspace_bytes = 0;
};
PasswordMessageSize PasswordMessageEncodedSize(PasswordMessageProfile,
                                               std::size_t data_bytes) noexcept;
// All outputs exclusive/private/disjoint from all inputs and each other.
// Workspace exact-sized/aligned; literal scratch exact-sized. Scratch/workspace
// erased after admission on EVERY exit, including success. Message erased on
// failure/exception. Entropy is fresh Core-RNG material supplied by the owner.
PgpCode EncryptPasswordMessage(PasswordMessageProfile, PgpInput password,
    PgpInput data, PasswordMessageEntropy, PgpOutput workspace,
    PgpOutput literal_scratch, PgpOutput message, PgpCancellation = {});

struct PasswordMessageDescription {
  PgpCode code = PgpCode::invalid_packet;
  Argon2SkeskDescription password{};
  std::size_t workspace_bytes = 0, data_body_bytes = 0;
};
// Untrusted structural/resource description only. Admit packet work factors
// before decrypting. Does not authenticate the supplied password or message.
PasswordMessageDescription InspectPasswordMessage(PgpInput, PgpCancellation = {});
struct PasswordMessagePlaintext {
  PgpCode code = PgpCode::invalid_packet;
  // Valid only on success: borrowed slice into caller-owned private plaintext
  // staging. Caller retains erasure responsibility through its publication fence.
  PgpInput data{};
  std::uint8_t literal_format = 0;
};
// Both staging buffers have exact data_body_bytes capacity (safe upper bound
// for contained literal packet). Unused plaintext capacity is zeroed. No heap
// allocation except provider allocations under the owning crypto memory scope.
// Verify the complete AEAD body/final length, then require exactly one literal
// packet. No compression, signed-message or padding grammar is implied by this
// explicitly named mechanism; a higher-level message dispatcher owns them.
PasswordMessagePlaintext DecryptPasswordMessage(PgpInput password, PgpInput message,
    PgpOutput workspace, PgpOutput body_scratch, PgpOutput plaintext_scratch,
    PgpCancellation = {});
}  // namespace scratchbird::core::crypto

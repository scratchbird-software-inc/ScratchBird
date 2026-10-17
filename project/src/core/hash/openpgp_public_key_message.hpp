// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "openpgp_packet.hpp"
#include "openpgp_x25519.hpp"

namespace scratchbird::core::crypto {
// Explicitly bounded single-recipient/one-literal mechanism, not a complete
// public-key builtin dispatcher, certificate validator, runtime admission,
// resource grant, compression policy or public result-publication authority.
// Caller validates actual certificate eligibility and owns protected staging,
// admitted backend memory and fresh Core-RNG entropy. Inputs remain immutable
// and retained through every synchronous cancellation callback.
struct X25519LiteralMessageEntropy {PgpInput session_key, ephemeral_secret, data_salt;};
struct X25519LiteralMessageSize {
  PgpCode code=PgpCode::invalid_packet;
  std::size_t message_bytes=0,literal_bytes=0;
};
X25519LiteralMessageSize X25519LiteralMessageEncodedSize(SeipdProfile,
    std::size_t data_bytes) noexcept;
// Exact native42-byte public body; session key16/24/32 matches data cipher;
// ephemeral secret and data salt exactly32 fresh bytes each. Outputs exclusive,
// exact-sized and disjoint from ALL inputs and each other. Invalid extents and
// structural profiles are untouched. Once accepted, literal scratch is erased
// even on success; message erased on failure/exception/cancellation. No entropy
// hook, external key selection or certificate/private-use authority is made.
PgpCode EncryptX25519LiteralMessage(SeipdProfile, PgpInput public_body,
    PgpInput data, X25519LiteralMessageEntropy, PgpOutput literal_scratch,
    PgpOutput message, PgpCancellation = {});
struct X25519LiteralMessageDescription {
  PgpCode code=PgpCode::invalid_packet;
  X25519PkeskDescription recipient{};
  std::size_t data_body_bytes=0;
};
// Exact v6 PKESK followed by one v2 SEIPD. SEIPD may use partial framing.
// Untrusted bounded structural sizes/fingerprint only, NEVER authentication.
X25519LiteralMessageDescription InspectX25519LiteralMessage(PgpInput,
    PgpCancellation = {});
struct X25519LiteralMessagePlaintext {
  PgpCode code=PgpCode::invalid_packet;
  PgpInput data{};
  std::uint8_t literal_format=0;
};
// Both exclusive staging buffers have EXACT data_body_bytes capacity, a safe
// upper bound on the contained literal. They are disjoint from every input and
// each other. Bad extents/structural prescan refusals leave them untouched.
// After accepting structure and exact extents, failure/exception/cancellation
// erases both; body scratch erased even on success, plaintext unused tail zero.
// Authenticate ALL inner bytes/final length BEFORE parsing the literal packet.
// Partial literal framing supported. The returned borrowed data slice is valid
// only on success; caller retains protected staging and erasure responsibility
// through its own policy/authority/publication fence. EVP-only backend heap.
X25519LiteralMessagePlaintext DecryptX25519LiteralMessage(PgpInput public_body,
    PgpInput recipient_secret, PgpInput message, PgpOutput body_scratch,
    PgpOutput plaintext_scratch, PgpCancellation = {});
}  // namespace scratchbird::core::crypto

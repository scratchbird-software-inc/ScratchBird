// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstddef>
#include <cstdint>

namespace scratchbird::core::crypto {
struct PgpInput { const std::uint8_t* data = nullptr; std::size_t size = 0; };
struct PgpOutput { std::uint8_t* data = nullptr; std::size_t size = 0; };
struct PgpCancellation { bool (*requested)(void*) = nullptr; void* context = nullptr; };
struct SeipdProfile {
  std::uint8_t cipher = 9;  // RFC9580 AES-256; also AES-128 (7), AES-192 (8).
  std::uint8_t aead = 2;    // OCB; GCM (3) also supported.
  std::uint8_t chunk = 10;  // 2^(10+6) bytes; RFC range 0..16.
};
enum class PgpCode { ok, invalid_profile, invalid_extent, invalid_packet,
                     size_overflow, provider_failure, authentication_failed, cancelled };
struct PgpSize { PgpCode code = PgpCode::invalid_packet; std::size_t bytes = 0; };

// SEIPD packet BODY only: excludes outer packet framing, session-key packets
// and inner literal-data parsing. Size inspection is never authentication.
PgpSize SeipdEncryptedSize(SeipdProfile, std::size_t plaintext_size) noexcept;
PgpSize SeipdPlaintextSize(PgpInput body) noexcept;

// Internal mechanism, not admission. Caller owns the key, fresh Core-RNG salt,
// exclusive PRIVATE STAGING buffers, resource/backend-memory admission and a
// synchronous probe. Output is not a public result or shared observable buffer.
// No product-facing deterministic entropy hook or permission receipt is made.
// Backend allocations use the configured OpenSSL allocator; this API does NOT
// establish a memory grant. Output size must exactly match the size query.
// Mutable output cannot overlap ANY input, including key/salt. Invalid extents
// are untouched. After acceptance, every failure/exception clears all output;
// derived key scratch is always erased. No plaintext may be observed/published
// until successful return. Cancellation probes may throw; erasure still occurs.
// The owning adapter must retain private staging through its own publication
// fence; this primitive cannot grant public result/transport visibility.
PgpCode EncryptSeipdV2(SeipdProfile, PgpInput key, PgpInput salt,
                      PgpInput plaintext, PgpOutput body, PgpCancellation = {});
PgpCode DecryptSeipdV2(PgpInput key, PgpInput body,
                      PgpOutput plaintext, PgpCancellation = {});

// Explicit RFC9580 type-3 (iterated/salted SHA256) S2K profile; this is NOT an
// Argon2 implementation or an automatic password-policy/default selection.
// Counts encode HASHED OCTETS, not iteration counts. Each call hashes at least
// one entire salt+password. Caller must admit that CPU work before entry.
// Body includes v6 SKESK header, S2K specifier, nonce, wrapped key and tag, but
// excludes the outer packet header. The packet's cipher determines the wrapping
// key size; the wrapped AES session key independently has 16, 24 or 32 bytes.
// All private-staging, cleanup and caller-admission rules above still apply.
// Salt is exactly 8 fresh Core-RNG bytes; nonce is 15 OCB or 12 GCM bytes.
// Structural inspection is not password verification or authentication.
PgpSize IteratedSkeskV6Size(std::uint8_t cipher, std::uint8_t aead, std::size_t session_key_bytes) noexcept;
PgpSize IteratedSkeskV6KeySize(PgpInput body) noexcept;
PgpCode EncryptIteratedSkeskV6(std::uint8_t cipher, std::uint8_t aead,
    std::uint8_t encoded_count, PgpInput password, PgpInput salt, PgpInput nonce,
    PgpInput session_key, PgpOutput body, PgpCancellation = {});
PgpCode DecryptIteratedSkeskV6(PgpInput password, PgpInput body,
    PgpOutput session_key, PgpCancellation = {});
}  // namespace scratchbird::core::crypto

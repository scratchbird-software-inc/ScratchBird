// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "openpgp_seipd.hpp"
#include <array>

namespace scratchbird::core::crypto {
struct X25519PkeskDescription {
  PgpCode code = PgpCode::invalid_packet;
  std::size_t session_key_bytes = 0;
  std::array<std::uint8_t,32> recipient_fingerprint{}, ephemeral_public_key{};
};

// Exact RFC9580 native v6 X25519 PKESK BODY, without outer packet framing.
// Only AES session-key sizes16/24/32 and an explicit v6 recipient are supported.
// Inspection is structural, NEVER certificate/authentication/resource admission.
// Failure descriptions contain no partial fields. Native public bodies are
// exactly42 bytes: version6, creation BE32, algorithm25, length BE32=32, key32.
PgpSize X25519PkeskV6Size(std::size_t session_key_bytes) noexcept;
X25519PkeskDescription InspectX25519PkeskV6(PgpInput body) noexcept;
PgpCode X25519PublicKeyFingerprintV6(PgpInput public_body, PgpOutput fingerprint,
                                   PgpCancellation = {});

// Internal bounded mechanism, NOT key eligibility/EXECUTE/private-key-use or
// resource admission. Caller validates certificate bindings/flags/revocations/
// time and owns actual protected key/output staging and backend-memory grants.
// Encrypt consumes a fresh, already-admitted32-byte Core-RNG ephemeral secret;
// it does not issue entropy or expose a product deterministic-entropy option.
// Decrypt derives and constant-time matches the recipient's public material and
// complete SHA256 v6 fingerprint before unwrapping. Native byte order is retained.
// Fingerprint matching is not message/certificate authentication; the enclosing
// v2 SEIPD and certificate owner must complete their own admission/publication.
//
// Exact-size, nonwrapping extents only. Output is exclusive private staging,
// disjoint from EVERY input. Bad extents/overlaps and structural refusals are
// untouched. After accepting structure/extents, any failure or thrown probe
// erases ALL output; every owned derived-secret/backend copy is released and
// scrubbed before the final cancellation probe. Caller input is never erased.
// No heap containers are used; EVP allocations require prior caller admission.
// Shared-secret/all-zero, recipient mismatch and unwrap failures publish no key.
PgpCode EncryptX25519PkeskV6(PgpInput public_body, PgpInput ephemeral_secret,
    PgpInput session_key, PgpOutput body, PgpCancellation = {});
PgpCode DecryptX25519PkeskV6(PgpInput public_body, PgpInput recipient_secret,
    PgpInput body, PgpOutput session_key, PgpCancellation = {});
}  // namespace scratchbird::core::crypto

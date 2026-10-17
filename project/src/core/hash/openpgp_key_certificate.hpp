// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "openpgp_packet.hpp"
#include "argon2_kdf.hpp"
#include <array>
#include <cstdint>
#include <optional>

namespace scratchbird::core::crypto {
enum class NativeKeyCertificateCode {
  ok, invalid_extent, invalid_limits, resource_limit, invalid_certificate,
  invalid_signature, unsupported_profile, expired, revoked, not_yet_valid,
  no_eligible_key, recipient_not_found, passphrase_required,
  authentication_failed, provider_failure, cancelled
};
enum class NativeKeyCertificatePurpose { encrypt, decrypt };
struct NativeKeyCertificateLimits {
  std::size_t maximum_bytes = 0, maximum_packets = 0,
              maximum_signature_subpackets = 0;
};
struct NativeKeyCertificateQuery {
  // Actual owning runtime supplies its admitted verification time. This
  // mechanism reads no clock and issues no time/policy/authorization authority.
  std::uint64_t verification_time_seconds = 0;
  NativeKeyCertificatePurpose purpose = NativeKeyCertificatePurpose::encrypt;
  // Exactly binary32 for decrypt; absent for encrypt. Not a UUID or key ID.
  PgpInput recipient_fingerprint{};
};
struct NativeKeyCertificateSelection {
  NativeKeyCertificateCode code = NativeKeyCertificateCode::invalid_certificate;
  std::array<std::uint8_t, 32> primary_fingerprint{}, recipient_fingerprint{};
  // Borrowed actual packet body slices, valid only on success and only while
  // the complete immutable certificate/source owner remains retained.
  PgpInput public_body{}, secret_body{};
  // Authenticated newest direct-key preferences. These are observations, not
  // admission or permission to silently substitute an algorithm.
  PgpInput preferred_aead_ciphersuites{}, preferred_hashes{},
           preferred_compression{}, features{};
  std::uint8_t secret_packet_type = 0;
  std::uint32_t creation_seconds = 0;
};
// Complete native v6 Ed25519-primary/X25519-encryption-subkey certificate
// validation/selection. No EXECUTE/private-key permission, grant or source lease
// is issued. Inputs remain immutable through synchronous callbacks. Provider
// allocations require the owning explicit crypto-memory scope.
NativeKeyCertificateSelection ValidateNativeKeyCertificate(PgpInput,
    NativeKeyCertificateQuery, NativeKeyCertificateLimits, PgpCancellation = {});

struct NativeSecretKeyDescription {
  NativeKeyCertificateCode code = NativeKeyCertificateCode::invalid_certificate;
  std::uint64_t workspace_bytes = 0;
  Argon2idParameters argon2{};
  std::uint8_t cipher = 0, aead = 0;
  bool encrypted = false;
};
// Untrusted structural work description, never a resource grant, key release
// or certificate validation. Body includes the public fields. Type is5 or7.
NativeSecretKeyDescription InspectNativeSecretKey(std::uint8_t packet_type,
                                                PgpInput body) noexcept;
// Revalidates the complete certificate and exact recipient; a saved selection
// is NOT authority. Decrypt query required. Missing passphrase is distinct from
// an explicitly supplied empty passphrase. Workspace is exact-sized/aligned
// actual admitted backing; output is exclusive/private/exactly32 bytes.
// All mutable extents are disjoint from every input and each other. Preconditions
// leave unaccepted outputs unchanged. After admission, workspace is erased on
// every exit and output on failure/exception/cancellation. After success the
// caller retains erasure/resource/publication responsibility for the secret.
NativeKeyCertificateCode UnlockNativeCertificateKey(PgpInput certificate,
    NativeKeyCertificateQuery, NativeKeyCertificateLimits,
    std::optional<PgpInput> passphrase, PgpOutput workspace, PgpOutput secret,
    PgpCancellation = {});
}  // namespace scratchbird::core::crypto

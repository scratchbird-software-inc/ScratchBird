// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "openpgp_key_certificate.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>

namespace scratchbird::core::crypto {
namespace {
using Code = NativeKeyCertificateCode;
constexpr std::size_t kPublicBytes = 42;
bool Extent(PgpInput in) noexcept {
  return (!in.size || in.data) && in.size <= UINTPTR_MAX - reinterpret_cast<std::uintptr_t>(in.data);
}
bool Overlap(PgpInput a, PgpInput b) noexcept {
  if (!a.size || !b.size) return false;
  const auto x = reinterpret_cast<std::uintptr_t>(a.data), y = reinterpret_cast<std::uintptr_t>(b.data);
  return x < y + b.size && y < x + a.size;
}
bool Poll(PgpCancellation p) { return p.requested && p.requested(p.context); }
std::uint32_t U32(const std::uint8_t* p) noexcept {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
         (std::uint32_t(p[2]) << 8) | p[3];
}
void Put32(std::uint8_t* p, std::uint32_t v) noexcept {
  for (unsigned i = 0; i < 4; ++i) p[3-i] = static_cast<std::uint8_t>(v >> (8*i));
}
bool PublicShape(PgpInput body, unsigned algorithm) noexcept {
  return Extent(body) && body.size >= kPublicBytes && body.data[0] == 6 &&
         body.data[5] == algorithm && U32(body.data + 6) == 32;
}
template <std::size_t N> struct Secret {
  std::array<std::uint8_t, N> bytes{};
  ~Secret() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
};
struct PendingSecret {
  PgpOutput workspace, output;
  bool accepted = false;
  ~PendingSecret() {
    if (workspace.size) OPENSSL_cleanse(workspace.data, workspace.size);
    if (!accepted && output.size) OPENSSL_cleanse(output.data, output.size);
  }
};
using Md = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
Code DerivedMatches(unsigned algorithm, PgpInput secret, PgpInput public_key) {
  if (secret.size != 32 || public_key.size != 32) return Code::invalid_extent;
  Key key(EVP_PKEY_new_raw_private_key(algorithm == 25 ? EVP_PKEY_X25519 : EVP_PKEY_ED25519,
                                     nullptr, secret.data, secret.size), EVP_PKEY_free);
  std::array<std::uint8_t, 32> derived{};
  std::size_t size = derived.size();
  if (!key || EVP_PKEY_get_raw_public_key(key.get(), derived.data(), &size) != 1 || size != derived.size())
    return Code::provider_failure;
  return CRYPTO_memcmp(derived.data(), public_key.data, size) == 0 ? Code::ok : Code::invalid_certificate;
}
Code Fingerprint(PgpInput body, std::array<std::uint8_t, 32>& out) {
  Md hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  const std::uint8_t prefix[]{0x9b, 0, 0, 0, 42};
  unsigned size = 0;
  return hash && EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) == 1 &&
         EVP_DigestUpdate(hash.get(), prefix, sizeof(prefix)) == 1 &&
         EVP_DigestUpdate(hash.get(), body.data, kPublicBytes) == 1 &&
         EVP_DigestFinal_ex(hash.get(), out.data(), &size) == 1 && size == out.size()
      ? Code::ok : Code::provider_failure;
}
struct SecretDescription {
  NativeSecretKeyDescription observation{};
  PgpInput salt{}, nonce{}, ciphertext{}, tag{}, plaintext{};
};
SecretDescription SecretShape(std::uint8_t type, PgpInput body) noexcept {
  SecretDescription out;
  if (!Extent(body)) { out.observation.code = Code::invalid_extent; return out; }
  if ((type != 5 && type != 7) || body.size <= kPublicBytes ||
      !PublicShape(body, type == 5 ? 27 : 25)) return out;
  const auto usage = body.data[42];
  if (!usage) {
    if (body.size != 75) return out;
    out.observation.code = Code::ok;
    out.plaintext = {body.data + 43, 32};
    return out;
  }
  if (usage != 253) { out.observation.code = Code::unsupported_profile; return out; }
  if (body.size < 44) return out;
  const std::size_t parameters = body.data[43];
  if (parameters > body.size - 44 || parameters < 23 || body.size - 44 - parameters != 48) return out;
  const auto* p = body.data + 44;
  const auto cipher = p[0], aead = p[1];
  if (cipher < 7 || cipher > 9 || (aead != 2 && aead != 3)) {
    out.observation.code = Code::unsupported_profile; return out;
  }
  const auto nonce_bytes = aead == 2 ? 15u : 12u;
  if (parameters != 23 + nonce_bytes || p[2] != 20 || p[3] != 4 ||
      !p[20] || !p[21] || p[22] > 31 || p[22] < 3) return out;
  Argon2idParameters argon{std::uint32_t{1} << p[22], p[20], p[21]};
  if (std::uint64_t(argon.memory_kib) < 8ULL * argon.lanes) return out;
  const auto work = Argon2idWorkspaceBytes(argon);
  if (work.code != Argon2Code::ok || work.bytes > SIZE_MAX) {
    out.observation.code = Code::resource_limit; return out;
  }
  out.observation = {Code::ok, work.bytes, argon, cipher, aead, true};
  out.salt = {p + 4, 16}; out.nonce = {p + 23, nonce_bytes};
  out.ciphertext = {p + parameters, 32}; out.tag = {p + parameters + 32, 16};
  return out;
}
struct Packet {
  std::uint8_t type = 0;
  PgpInput body{};
};
Code Next(PgpInput certificate, std::size_t& at, std::size_t& count,
          const NativeKeyCertificateLimits& limits, Packet& result, PgpCancellation probe) {
  if (Poll(probe)) return Code::cancelled;
  if (count == limits.maximum_packets) return Code::resource_limit;
  const auto packet = InspectPacket({certificate.data + at, certificate.size - at}, probe);
  if (packet.code == PgpCode::cancelled) return Code::cancelled;
  if (packet.code != PgpCode::ok || packet.partial || !packet.packet_bytes) return Code::invalid_certificate;
  result = {packet.type, {certificate.data + at + packet.first_body_offset, packet.body_bytes}};
  at += packet.packet_bytes; ++count;
  return Code::ok;
}
struct Policy {
  std::uint32_t created = 0, signature_expiry = 0, key_expiry = 0;
  std::uint8_t flags = 0;
  bool has_created = false, has_issuer = false, has_flags = false;
  bool revocable = true;
  PgpInput issuer{}, aead_preferences{}, hash_preferences{}, compression_preferences{}, features{};
};
bool BytesEqual(PgpInput a, PgpInput b) noexcept {
  return a.size == b.size && (!a.size || std::memcmp(a.data, b.data, a.size) == 0);
}
bool SamePolicy(const Policy& a, const Policy& b) noexcept {
  return a.signature_expiry == b.signature_expiry && a.key_expiry == b.key_expiry &&
         a.flags == b.flags && a.has_flags == b.has_flags && a.revocable == b.revocable &&
         BytesEqual(a.aead_preferences, b.aead_preferences) &&
         BytesEqual(a.hash_preferences, b.hash_preferences) &&
         BytesEqual(a.compression_preferences, b.compression_preferences) && BytesEqual(a.features, b.features);
}
bool SubpacketLength(PgpInput in, std::size_t& at, std::size_t& bytes) noexcept {
  if (at == in.size) return false;
  const auto first = in.data[at++];
  if (first < 192) bytes = first;
  else if (first < 255) {
    if (at == in.size) return false;
    bytes = (std::size_t(first) - 192) * 256 + in.data[at++] + 192;
  } else {
    if (in.size - at < 4) return false;
    bytes = U32(in.data + at); at += 4;
  }
  return bytes && bytes <= in.size - at;
}
Code Subpackets(PgpInput bytes, bool hashed, Policy& policy, std::size_t& count,
                const NativeKeyCertificateLimits& limits, PgpCancellation probe) {
  // Policy fields cannot depend on the order of contradictory authenticated
  // duplicates within one signature. Equal duplicate values are harmless.
  std::array<PgpInput, 40> seen{};
  std::array<bool, 40> present{};
  for (std::size_t at = 0; at < bytes.size;) {
    if (Poll(probe)) return Code::cancelled;
    if (count == limits.maximum_signature_subpackets) return Code::resource_limit;
    ++count;
    std::size_t size = 0;
    if (!SubpacketLength(bytes, at, size)) return Code::invalid_signature;
    const auto tag = bytes.data[at];
    const auto type = tag & 127;
    const PgpInput value{bytes.data + at + 1, size - 1};
    at += size;
    if (hashed && type < seen.size() && (type == 2 || type == 3 || type == 7 || type == 9 ||
        type == 21 || type == 22 || type == 27 || type == 30 || type == 33 || type == 39)) {
      if (present[type] && !BytesEqual(seen[type], value)) return Code::invalid_signature;
      seen[type] = value; present[type] = true;
    }
    bool known = true;
    switch (type) {
      case 2:
        if (value.size != 4) return Code::invalid_signature;
        if (hashed) { policy.created = U32(value.data); policy.has_created = true; }
        break;
      case 3:
        if (value.size != 4) return Code::invalid_signature;
        if (hashed) policy.signature_expiry = U32(value.data);
        break;
      case 9:
        if (value.size != 4) return Code::invalid_signature;
        if (hashed) policy.key_expiry = U32(value.data);
        break;
      case 16:
        if (value.size != 8) return Code::invalid_signature;
        break;  // Advisory key IDs are never issuer identity authority.
      case 27:
        if (!value.size) return Code::invalid_signature;
        if (hashed) { policy.flags = value.data[0]; policy.has_flags = true; }
        break;
      case 33:
        if (value.size != 33 || value.data[0] != 6) return Code::invalid_signature;
        if (hashed) { policy.issuer = {value.data + 1, 32}; policy.has_issuer = true; }
        break;
      case 11: case 21: case 22: case 30:
        if (!value.size) return Code::invalid_signature;
        if (hashed && type == 21) policy.hash_preferences = value;
        if (hashed && type == 22) policy.compression_preferences = value;
        if (hashed && type == 30) policy.features = value;
        break;
      case 39:
        if (!value.size || value.size % 2) return Code::invalid_signature;
        if (hashed) policy.aead_preferences = value;
        break;
      case 4: case 7: case 25:
        if (value.size != 1 || value.data[0] > 1) return Code::invalid_signature;
        if (hashed && type == 7) policy.revocable = value.data[0] != 0;
        break;
      case 29:
        if (!value.size) return Code::invalid_signature;
        break;
      default: known = false; break;
    }
    if (!known && (tag & 128)) return Code::invalid_signature;
  }
  return Code::ok;
}
struct HashProfile { const EVP_MD* digest = nullptr; std::size_t bytes = 0, salt = 0; };
HashProfile Hash(unsigned id) {
  switch (id) {
    case 8: return {EVP_sha256(), 32, 16};
    case 9: return {EVP_sha384(), 48, 24};
    case 10: return {EVP_sha512(), 64, 32};
    case 12: return {EVP_sha3_256(), 32, 16};
    case 14: return {EVP_sha3_512(), 64, 32};
    default: return {};
  }
}
Code Update(EVP_MD_CTX* hash, PgpInput input, PgpCancellation probe) {
  for (std::size_t at = 0; at < input.size;) {
    if (Poll(probe)) return Code::cancelled;
    const auto n = std::min(std::size_t{4096}, input.size - at);
    if (EVP_DigestUpdate(hash, input.data + at, n) != 1) return Code::provider_failure;
    at += n;
  }
  return Code::ok;
}
Code HashBody(EVP_MD_CTX* hash, std::uint8_t domain, PgpInput body, PgpCancellation probe) {
  if (body.size > UINT32_MAX) return Code::invalid_signature;
  std::array<std::uint8_t, 5> prefix{domain}; Put32(prefix.data() + 1, static_cast<std::uint32_t>(body.size));
  auto code = Update(hash, {prefix.data(), prefix.size()}, probe);
  return code == Code::ok ? Update(hash, body, probe) : code;
}
struct ParsedSignature {
  PgpInput bytes{};
  HashProfile profile{};
  std::size_t hashed_end = 0, tail = 0;
  std::uint8_t type = 0;
};
Code ParseSignature(PgpInput signature, const std::array<std::uint8_t, 32>& primary_fingerprint,
    std::size_t& subpacket_count, const NativeKeyCertificateLimits& limits,
    Policy& policy, ParsedSignature& parsed, PgpCancellation probe) {
  if (signature.size < 8 || signature.data[0] != 6 || signature.data[2] != 27) return Code::invalid_signature;
  const auto profile = Hash(signature.data[3]);
  if (!profile.digest) return Code::invalid_signature;
  const std::size_t hashed_bytes = U32(signature.data + 4);
  if (hashed_bytes > signature.size - 8) return Code::invalid_signature;
  const std::size_t hashed_end = 8 + hashed_bytes;
  if (signature.size - hashed_end < 4) return Code::invalid_signature;
  const std::size_t unhashed_bytes = U32(signature.data + hashed_end);
  if (unhashed_bytes > signature.size - hashed_end - 4) return Code::invalid_signature;
  const auto tail = hashed_end + 4 + unhashed_bytes;
  if (signature.size - tail != 3 + profile.salt + 64 || signature.data[tail + 2] != profile.salt)
    return Code::invalid_signature;
  auto code = Subpackets({signature.data + 8, hashed_bytes}, true, policy, subpacket_count, limits, probe);
  if (code != Code::ok) return code;
  code = Subpackets({signature.data + hashed_end + 4, unhashed_bytes}, false, policy,
                    subpacket_count, limits, probe);
  if (code != Code::ok) return code;
  if (!policy.has_created || !policy.has_issuer ||
      CRYPTO_memcmp(policy.issuer.data, primary_fingerprint.data(), 32)) return Code::invalid_signature;
  parsed = {signature, profile, hashed_end, tail, signature.data[1]};
  return Code::ok;
}
Code VerifyParsedSignature(const ParsedSignature& parsed, PgpInput primary, PgpInput subject,
    unsigned subject_type, PgpCancellation probe) {
  const auto signature = parsed.bytes;
  const auto profile = parsed.profile;
  const auto hashed_end = parsed.hashed_end, tail = parsed.tail;
  Md hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!hash || EVP_DigestInit_ex(hash.get(), profile.digest, nullptr) != 1) return Code::provider_failure;
  auto code = Update(hash.get(), {signature.data + tail + 3, profile.salt}, probe);
  if (code != Code::ok) return code;
  code = HashBody(hash.get(), 0x9b, primary, probe);
  if (code != Code::ok) return code;
  if (subject_type) {
    code = HashBody(hash.get(), subject_type == 13 ? 0xb4 : subject_type == 17 ? 0xd1 : 0x9b,
                    subject, probe);
    if (code != Code::ok) return code;
  }
  code = Update(hash.get(), {signature.data, hashed_end}, probe);
  if (code != Code::ok) return code;
  std::array<std::uint8_t, 6> trailer{6, 255};
  Put32(trailer.data() + 2, static_cast<std::uint32_t>(hashed_end));
  code = Update(hash.get(), {trailer.data(), trailer.size()}, probe);
  if (code != Code::ok) return code;
  std::array<std::uint8_t, EVP_MAX_MD_SIZE> digest{};
  unsigned digest_bytes = 0;
  if (EVP_DigestFinal_ex(hash.get(), digest.data(), &digest_bytes) != 1 || digest_bytes != profile.bytes)
    return Code::provider_failure;
  if (CRYPTO_memcmp(digest.data(), signature.data + tail, 2)) return Code::invalid_signature;
  Key key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, primary.data + 10, 32), EVP_PKEY_free);
  Md verify(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!key || !verify || EVP_DigestVerifyInit(verify.get(), nullptr, nullptr, nullptr, key.get()) != 1)
    return Code::provider_failure;
  const auto verified = EVP_DigestVerify(verify.get(), signature.data + tail + 3 + profile.salt,
                                       64, digest.data(), digest_bytes);
  if (Poll(probe)) return Code::cancelled;
  return verified == 1 ? Code::ok : verified == 0 ? Code::invalid_signature : Code::provider_failure;
}
struct Group {
  PgpInput public_body{}, secret_body{}, identity_body{};
  std::uint8_t packet_type = 0, identity_type = 0;
  std::array<std::uint8_t, 32> fingerprint{};
  std::uint32_t created = 0;
  Policy newest{};
  std::optional<std::uint32_t> certification_revoked_at{};
  bool has_binding = false, revoked = false, conflicting = false;
  bool has_expired_binding = false;
};
Code ChoosePolicy(Group& group, const Policy& policy) {
  if (!group.has_binding || policy.created > group.newest.created) {
    group.newest = policy; group.has_binding = true; group.conflicting = false;
  } else if (policy.created == group.newest.created && !SamePolicy(policy, group.newest)) group.conflicting = true;
  return Code::ok;
}
Code Eligible(const Group& group, std::uint64_t now, bool primary) {
  if (group.revoked) return Code::revoked;
  if (!group.has_binding) return group.has_expired_binding ? Code::expired : Code::invalid_signature;
  if (group.newest.revocable && group.certification_revoked_at &&
      group.newest.created <= *group.certification_revoked_at)
    return Code::invalid_signature;
  if (group.conflicting) return Code::invalid_certificate;
  if (group.created > now) return Code::not_yet_valid;
  const auto& p = group.newest;
  if (p.signature_expiry && now >= std::uint64_t(p.created) + p.signature_expiry) return Code::expired;
  if (p.key_expiry && now >= std::uint64_t(group.created) + p.key_expiry) return Code::expired;
  if (!primary && (!p.has_flags || !(p.flags & 12) || (p.flags & 3))) return Code::no_eligible_key;
  return Code::ok;
}
}  // namespace

NativeSecretKeyDescription InspectNativeSecretKey(std::uint8_t type, PgpInput body) noexcept {
  return SecretShape(type, body).observation;
}

NativeKeyCertificateSelection ValidateNativeKeyCertificate(PgpInput certificate,
    NativeKeyCertificateQuery query, NativeKeyCertificateLimits limits, PgpCancellation probe) {
  NativeKeyCertificateSelection out;
  const auto fail = [&](Code code) { NativeKeyCertificateSelection failed; failed.code = code; return failed; };
  if (!Extent(certificate) || !Extent(query.recipient_fingerprint)) return fail(Code::invalid_extent);
  if (!limits.maximum_bytes || !limits.maximum_packets || !limits.maximum_signature_subpackets)
    return fail(Code::invalid_limits);
  if (certificate.size > limits.maximum_bytes) return fail(Code::resource_limit);
  if ((query.purpose == NativeKeyCertificatePurpose::encrypt && query.recipient_fingerprint.size) ||
      (query.purpose == NativeKeyCertificatePurpose::decrypt && query.recipient_fingerprint.size != 32) ||
      (query.purpose != NativeKeyCertificatePurpose::encrypt && query.purpose != NativeKeyCertificatePurpose::decrypt))
    return fail(Code::invalid_extent);
  if (Poll(probe)) return fail(Code::cancelled);
  Group primary, group;
  unsigned phase = 0;
  std::size_t at = 0, packet_count = 0, subpacket_count = 0;
  bool primary_present = false, selected = false, padding = false;
  Code exact_failure = Code::recipient_not_found;
  const auto finish_group = [&]() -> Code {
    if (!group.packet_type) return Code::ok;
    if (group.packet_type == 13 || group.packet_type == 17) return Code::ok;
    // Every subkey must have an authenticated primary binding, even if it is
    // not selected. Expiry/flags/revocation determine current eligibility.
    if (!group.has_binding && !group.has_expired_binding) return Code::invalid_signature;
    if (group.conflicting) return Code::invalid_certificate;
    const auto eligible = Eligible(group, query.verification_time_seconds, false);
    if (query.purpose == NativeKeyCertificatePurpose::decrypt) {
      if (CRYPTO_memcmp(group.fingerprint.data(), query.recipient_fingerprint.data, 32)) return Code::ok;
      if (selected) return Code::invalid_certificate;
      if (eligible != Code::ok) { exact_failure = eligible; return Code::ok; }
      if (group.packet_type != 7) { exact_failure = Code::recipient_not_found; return Code::ok; }
    } else {
      if (eligible != Code::ok) return Code::ok;
      if (selected && (group.created < out.creation_seconds ||
          (group.created == out.creation_seconds &&
           !std::lexicographical_compare(group.fingerprint.begin(), group.fingerprint.end(),
                                         out.recipient_fingerprint.begin(), out.recipient_fingerprint.end()))))
        return Code::ok;
    }
    out.primary_fingerprint = primary.fingerprint;
    out.recipient_fingerprint = group.fingerprint;
    out.public_body = group.public_body; out.secret_body = group.secret_body;
    out.secret_packet_type = group.packet_type == 7 ? 7 : 0;
    out.creation_seconds = group.created; selected = true;
    return Code::ok;
  };
  while (at < certificate.size) {
    Packet packet;
    const auto packet_begin = at;
    auto code = Next(certificate, at, packet_count, limits, packet, probe);
    if (code != Code::ok) return fail(code);
    if (packet.type == 10) {
      if (packet.body.size != 3 || std::memcmp(packet.body.data, "PGP", 3)) return fail(Code::invalid_certificate);
      continue;
    }
    if (packet.type >= 40) continue;  // RFC4.3 noncritical extensions, no authority.
    if (padding) return fail(Code::invalid_certificate);
    if (!primary_present) {
      if ((packet.type != 6 && packet.type != 5) || !PublicShape(packet.body, 27))
        return fail(Code::unsupported_profile);
      if (packet.type == 6 && packet.body.size != 42) return fail(Code::invalid_certificate);
      if (packet.type == 5) {
        const auto secret = SecretShape(5, packet.body);
        if (secret.observation.code != Code::ok) return fail(secret.observation.code);
        if (!secret.observation.encrypted) {
          code = DerivedMatches(27, secret.plaintext, {packet.body.data + 10, 32});
          if (code != Code::ok) return fail(code);
        }
      }
      primary.public_body = {packet.body.data, 42}; primary.packet_type = packet.type;
      primary.created = U32(packet.body.data + 1);
      code = Fingerprint(primary.public_body, primary.fingerprint);
      if (code != Code::ok) return fail(code);
      primary_present = true; continue;
    }
    if (packet.type == 2) {
      Policy policy; ParsedSignature signature;
      code = ParseSignature(packet.body, primary.fingerprint, subpacket_count, limits, policy, signature, probe);
      if (code == Code::provider_failure || code == Code::cancelled || code == Code::resource_limit) return fail(code);
      if (code != Code::ok) continue;
      const auto type = signature.type;
      if (type == 0x30) {
        // Certification revocations sign either a Direct Key signature's
        // primary target or an actual UID/attribute target. Packet adjacency
        // is not authority: a valid revocation moved after a subkey must not
        // disappear, or be applied to the wrong key. Parse/count subpackets
        // once, then resolve the signed target through bounded verification.
        bool matched = false, primary_match = false;
        PgpInput matched_identity{};
        unsigned matched_identity_type = 0;
        code = VerifyParsedSignature(signature, primary.public_body, {}, 0, probe);
        if (code == Code::provider_failure || code == Code::cancelled) return fail(code);
        if (code == Code::ok) { matched = true; primary_match = true; }
        for (std::size_t candidate_at = 0, candidate_count = 0; candidate_at < certificate.size;) {
          Packet candidate;
          code = Next(certificate, candidate_at, candidate_count, limits, candidate, probe);
          if (code != Code::ok) return fail(code);
          if (candidate.type != 13 && candidate.type != 17) continue;
          code = VerifyParsedSignature(signature, primary.public_body, candidate.body, candidate.type, probe);
          if (code == Code::provider_failure || code == Code::cancelled) return fail(code);
          if (code != Code::ok) continue;
          if (matched && (primary_match || matched_identity_type != candidate.type ||
                          !BytesEqual(matched_identity, candidate.body)))
            return fail(Code::invalid_certificate);
          matched = true; matched_identity = candidate.body; matched_identity_type = candidate.type;
        }
        if (!matched || policy.created < primary.created || policy.created > query.verification_time_seconds)
          continue;
        auto* target = primary_match ? &primary :
            (group.packet_type == 13 || group.packet_type == 17) &&
                group.packet_type == matched_identity_type && BytesEqual(group.identity_body, matched_identity) ? &group : nullptr;
        // Human labels never confer engine identity/recipient eligibility.
        // A matched noncurrent label therefore has no engine policy to revoke.
        if (target && (!target->certification_revoked_at || policy.created > *target->certification_revoked_at))
          target->certification_revoked_at = policy.created;
        continue;
      }
      const bool primary_signature = packet.body.size >= 2 &&
          (packet.body.data[1] == 0x20 || packet.body.data[1] == 0x1f);
      const auto subject_type = primary_signature ? 0 :
          group.packet_type == 13 || group.packet_type == 17 ? group.packet_type : group.packet_type ? 14 : 0;
      const auto subject = subject_type == 13 || subject_type == 17 ? group.identity_body : group.public_body;
      code = VerifyParsedSignature(signature, primary.public_body, subject, subject_type, probe);
      if (code == Code::provider_failure || code == Code::cancelled || code == Code::resource_limit) return fail(code);
      // RFC5.2.5: malformed/unknown/invalid signatures never validate, but must
      // not suppress a different valid signature in the same certificate.
      if (code != Code::ok) continue;
      if (policy.created < primary.created || (subject_type && group.packet_type && policy.created < group.created))
        continue;
      if (policy.created > query.verification_time_seconds) continue;
      if ((type == 0x18 && subject_type != 14) ||
          ((type >= 0x10 && type <= 0x13) && subject_type != 13 && subject_type != 17))
        return fail(Code::invalid_certificate);
      const bool self_signature = type == 0x1f || type == 0x18 || (type >= 0x10 && type <= 0x13);
      if (self_signature && policy.signature_expiry && query.verification_time_seconds >=
          std::uint64_t(policy.created) + policy.signature_expiry) {
        // RFC5.2.3.10 chooses the newest VALID signature. An expired
        // signature is not a newer policy; a valid signature whose policy
        // expires the key or removes encryption flags remains authoritative.
        (subject_type ? group : primary).has_expired_binding = true;
        continue;
      }
      if (!subject_type) {
        if (type == 0x20) primary.revoked = true;
        else if (type == 0x1f) ChoosePolicy(primary, policy);
        else return fail(Code::invalid_certificate);
      } else if (subject_type == 14) {
        if (type == 0x28) group.revoked = true;
        else if (type == 0x18) ChoosePolicy(group, policy);
        else return fail(Code::invalid_certificate);
      } else {
        if (type >= 0x10 && type <= 0x13) ChoosePolicy(group, policy);
        else return fail(Code::invalid_certificate);
      }
      continue;
    }
    code = finish_group();
    if (code != Code::ok) return fail(code);
    group = {};
    if (packet.type == 13 || packet.type == 17) {
      if (phase == 2) return fail(Code::invalid_certificate);
      if (packet.type == 17) {
        // RFC5.12 permits opaque attribute values; their framing must still be
        // complete. Unknown attribute types do not become engine identities.
        if (!packet.body.size) return fail(Code::invalid_certificate);
        for (std::size_t attribute = 0; attribute < packet.body.size;) {
          if (Poll(probe)) return fail(Code::cancelled);
          if (subpacket_count == limits.maximum_signature_subpackets) return fail(Code::resource_limit);
          ++subpacket_count;
          std::size_t size = 0;
          if (!SubpacketLength(packet.body, attribute, size)) return fail(Code::invalid_certificate);
          attribute += size;
        }
      }
      phase = 1; group.packet_type = packet.type; group.identity_body = packet.body;
      continue;
    }
    if (packet.type == 14 || packet.type == 7) {
      phase = 2;
      if (!PublicShape(packet.body, 25)) return fail(Code::unsupported_profile);
      // A repeated key block cannot reset revocation/binding state. Reject
      // duplicate public identities rather than selecting whichever copy is
      // last. This bounded rescan owns no metadata heap or secret copy.
      for (std::size_t previous = 0, previous_count = 0; previous < packet_begin;) {
        Packet old;
        code = Next(certificate, previous, previous_count, limits, old, probe);
        if (code != Code::ok) return fail(code);
        if ((old.type == 14 || old.type == 7) && old.body.size >= 42 &&
            std::memcmp(old.body.data, packet.body.data, 42) == 0) return fail(Code::invalid_certificate);
      }
      if (packet.type == 14 && packet.body.size != 42) return fail(Code::invalid_certificate);
      if (packet.type == 7) {
        const auto secret = SecretShape(7, packet.body);
        if (secret.observation.code != Code::ok) return fail(secret.observation.code);
        if (!secret.observation.encrypted) {
          code = DerivedMatches(25, secret.plaintext, {packet.body.data + 10, 32});
          if (code != Code::ok) return fail(code);
        }
        group.secret_body = packet.body;
      }
      group.packet_type = packet.type; group.public_body = {packet.body.data, 42};
      group.created = U32(packet.body.data + 1);
      code = Fingerprint(group.public_body, group.fingerprint);
      if (code != Code::ok) return fail(code);
      continue;
    }
    if (packet.type == 21) { padding = true; continue; }
    return fail(Code::invalid_certificate);
  }
  if (!primary_present) return fail(Code::invalid_certificate);
  auto code = finish_group();
  if (code != Code::ok) return fail(code);
  code = Eligible(primary, query.verification_time_seconds, true);
  if (code != Code::ok) return fail(code);
  if (!selected) return fail(query.purpose == NativeKeyCertificatePurpose::decrypt ? exact_failure : Code::no_eligible_key);
  if (Poll(probe)) return fail(Code::cancelled);
  out.preferred_aead_ciphersuites = primary.newest.aead_preferences;
  out.preferred_hashes = primary.newest.hash_preferences;
  out.preferred_compression = primary.newest.compression_preferences;
  out.features = primary.newest.features;
  out.code = Code::ok; return out;
}

namespace {
const EVP_CIPHER* SecretCipher(unsigned cipher, unsigned aead) {
  if (aead == 3) return cipher == 7 ? EVP_aes_128_gcm() : cipher == 8 ? EVP_aes_192_gcm() : EVP_aes_256_gcm();
#ifndef OPENSSL_NO_OCB
  return cipher == 7 ? EVP_aes_128_ocb() : cipher == 8 ? EVP_aes_192_ocb() : EVP_aes_256_ocb();
#else
  return nullptr;
#endif
}
Code OpenSecret(std::uint8_t type, PgpInput body, const SecretDescription& info, PgpInput passphrase,
                PgpOutput workspace, PgpOutput output, PgpCancellation probe) {
  const auto key_bytes = std::size_t{16} + 8 * (info.observation.cipher - 7);
  Secret<32> s2k, key;
  const auto derived = DeriveArgon2idKey(info.observation.argon2,
      {passphrase.data, passphrase.size}, {info.salt.data, info.salt.size},
      {workspace.data, workspace.size}, {s2k.bytes.data(), key_bytes}, {probe.requested, probe.context});
  if (derived == Argon2Code::cancelled) return Code::cancelled;
  if (derived != Argon2Code::ok) return Code::provider_failure;
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> hkdf(
      EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
  const std::array<std::uint8_t, 4> domain{static_cast<std::uint8_t>(0xc0 | type), 6,
      info.observation.cipher, info.observation.aead};
  auto size = key_bytes;
  if (!hkdf || EVP_PKEY_derive_init(hkdf.get()) != 1 ||
      EVP_PKEY_CTX_set_hkdf_md(hkdf.get(), EVP_sha256()) != 1 ||
      EVP_PKEY_CTX_set1_hkdf_key(hkdf.get(), s2k.bytes.data(), static_cast<int>(key_bytes)) != 1 ||
      EVP_PKEY_CTX_add1_hkdf_info(hkdf.get(), domain.data(), domain.size()) != 1 ||
      EVP_PKEY_derive(hkdf.get(), key.bytes.data(), &size) != 1 || size != key_bytes) return Code::provider_failure;
  if (Poll(probe)) return Code::cancelled;
  const auto* cipher = SecretCipher(info.observation.cipher, info.observation.aead);
  CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  Secret<16> tag, final;
  std::memcpy(tag.bytes.data(), info.tag.data, 16);
  int n = 0, written = 0, finalized = 0;
  const std::uint8_t packet_tag = static_cast<std::uint8_t>(0xc0 | type);
  if (!cipher || !context || EVP_CipherInit_ex(context.get(), cipher, nullptr, nullptr, nullptr, 0) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(info.nonce.size), nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_TAG, 16, tag.bytes.data()) != 1 ||
      EVP_CipherInit_ex(context.get(), nullptr, nullptr, key.bytes.data(), info.nonce.data, 0) != 1 ||
      EVP_CipherUpdate(context.get(), nullptr, &n, &packet_tag, 1) != 1 ||
      EVP_CipherUpdate(context.get(), nullptr, &n, body.data, 42) != 1 ||
      EVP_CipherUpdate(context.get(), output.data, &written, info.ciphertext.data, 32) != 1)
    return Code::provider_failure;
  if (written < 0 || written > 32) return Code::provider_failure;
  if (EVP_CipherFinal_ex(context.get(), final.bytes.data(), &finalized) != 1) return Code::authentication_failed;
  if (finalized < 0 || finalized > 16 || written + finalized != 32) return Code::provider_failure;
  if (finalized) std::memcpy(output.data + written, final.bytes.data(), finalized);
  return Poll(probe) ? Code::cancelled : Code::ok;
}
}  // namespace

NativeKeyCertificateCode UnlockNativeCertificateKey(PgpInput certificate,
    NativeKeyCertificateQuery query, NativeKeyCertificateLimits limits,
    std::optional<PgpInput> passphrase, PgpOutput workspace, PgpOutput secret, PgpCancellation probe) {
  const PgpInput work{workspace.data, workspace.size}, out{secret.data, secret.size};
  if (!Extent(certificate) || !Extent(query.recipient_fingerprint) || !Extent(work) || !Extent(out) ||
      (passphrase && (!Extent(*passphrase) || passphrase->size > UINT32_MAX)) || secret.size != 32 ||
      (workspace.size && reinterpret_cast<std::uintptr_t>(workspace.data) % 8) ||
      query.purpose != NativeKeyCertificatePurpose::decrypt) return Code::invalid_extent;
  for (const auto input : {certificate, query.recipient_fingerprint, passphrase.value_or(PgpInput{})})
    if (Overlap(input, work) || Overlap(input, out)) return Code::invalid_extent;
  if (Overlap(work, out)) return Code::invalid_extent;
  const auto selected = ValidateNativeKeyCertificate(certificate, query, limits, probe);
  if (selected.code != Code::ok) return selected.code;
  const auto info = SecretShape(selected.secret_packet_type, selected.secret_body);
  if (info.observation.code != Code::ok) return info.observation.code;
  if (workspace.size != info.observation.workspace_bytes) return Code::invalid_extent;
  PendingSecret pending{workspace, secret};
  if (Poll(probe)) return Code::cancelled;
  if (info.observation.encrypted) {
    if (!passphrase) return Code::passphrase_required;
    const auto code = OpenSecret(selected.secret_packet_type, selected.secret_body, info, *passphrase,
                                  workspace, secret, probe);
    if (code != Code::ok) return code;
  } else std::memcpy(secret.data, info.plaintext.data, 32);
  const auto matched = DerivedMatches(25, {secret.data, secret.size}, {selected.public_body.data + 10, 32});
  if (matched != Code::ok) return matched == Code::provider_failure ? matched : Code::authentication_failed;
  if (Poll(probe)) return Code::cancelled;
  pending.accepted = true; return Code::ok;
}
}  // namespace scratchbird::core::crypto

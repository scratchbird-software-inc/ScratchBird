// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "openpgp_seipd.hpp"
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
constexpr std::size_t kHeader = 36, kTag = 16, kSlice = 65536;
bool ProfileValid(SeipdProfile p) noexcept {
  return p.cipher >= 7 && p.cipher <= 9 && (p.aead == 2 || p.aead == 3) && p.chunk <= 16;
}
std::size_t KeySize(SeipdProfile p) noexcept { return 16 + (p.cipher - 7) * 8; }
std::size_t ChunkSize(SeipdProfile p) noexcept { return std::size_t{1} << (p.chunk + 6); }
std::size_t NonceSize(SeipdProfile p) noexcept { return p.aead == 2 ? 15 : 12; }
bool Extent(PgpInput in) noexcept {
  const auto at = reinterpret_cast<std::uintptr_t>(in.data);
  return (!in.size || in.data) && in.size <= std::numeric_limits<std::uintptr_t>::max() - at;
}
bool Overlaps(PgpInput a, PgpInput b) noexcept {
  if (!a.size || !b.size) return false;
  const auto x = reinterpret_cast<std::uintptr_t>(a.data), y = reinterpret_cast<std::uintptr_t>(b.data);
  return x < y + b.size && y < x + a.size;
}
bool Buffers(PgpOutput output, PgpInput key, PgpInput input, PgpInput salt = {}) noexcept {
  const PgpInput out{output.data, output.size};
  return Extent(out) && Extent(key) && Extent(input) && Extent(salt) &&
      !Overlaps(out, key) && !Overlaps(out, input) && !Overlaps(out, salt);
}
struct OutputOwner {
  PgpOutput out;
  bool accepted = false;
  ~OutputOwner() { if (!accepted && out.size) OPENSSL_cleanse(out.data, out.size); }
};
template <std::size_t N> struct Secret {
  std::array<unsigned char, N> bytes{};
  ~Secret() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
};
bool Cancelled(PgpCancellation probe) { return probe.requested && probe.requested(probe.context); }
void BigEndian(std::uint8_t* out, std::uint64_t n) noexcept {
  for (unsigned i = 0; i < 8; ++i) out[7-i] = static_cast<std::uint8_t>(n >> (i*8));
}
const EVP_CIPHER* Cipher(SeipdProfile p) {
  if (p.aead == 2) {
#ifndef OPENSSL_NO_OCB
    if (p.cipher == 7) return EVP_aes_128_ocb();
    if (p.cipher == 8) return EVP_aes_192_ocb();
    return EVP_aes_256_ocb();
#else
    return nullptr;
#endif
  }
  if (p.cipher == 7) return EVP_aes_128_gcm();
  if (p.cipher == 8) return EVP_aes_192_gcm();
  return EVP_aes_256_gcm();
}
bool Derive(PgpInput key, PgpInput salt, const std::array<unsigned char, 13>& aad,
            std::uint8_t* output, std::size_t size) {
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
      EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
  if (!ctx || EVP_PKEY_derive_init(ctx.get()) <= 0 ||
      EVP_PKEY_CTX_set_hkdf_md(ctx.get(), EVP_sha256()) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_salt(ctx.get(), salt.data, static_cast<int>(salt.size)) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_key(ctx.get(), key.data, static_cast<int>(key.size)) <= 0 ||
      EVP_PKEY_CTX_add1_hkdf_info(ctx.get(), aad.data(), 5) <= 0) return false;
  auto actual = size;
  return EVP_PKEY_derive(ctx.get(), output, &actual) > 0 && actual == size;
}

PgpCode Chunk(EVP_CIPHER_CTX* ctx, const EVP_CIPHER* cipher, bool encrypt,
    PgpInput key, PgpInput nonce, PgpInput aad, PgpInput data, PgpOutput out,
    const std::uint8_t* input_tag, std::uint8_t* output_tag, PgpCancellation cancellation) {
  if (Cancelled(cancellation)) return PgpCode::cancelled;
  Secret<16> tag, final;
  if (!encrypt) std::memcpy(tag.bytes.data(), input_tag, kTag);
  if (EVP_CIPHER_CTX_reset(ctx) != 1 ||
      EVP_CipherInit_ex(ctx, cipher, nullptr, nullptr, nullptr, encrypt ? 1 : 0) != 1 ||
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce.size), nullptr) != 1 ||
      (!encrypt && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, kTag, tag.bytes.data()) != 1) ||
      EVP_CipherInit_ex(ctx, nullptr, nullptr, key.data, nonce.data, encrypt ? 1 : 0) != 1)
    return PgpCode::provider_failure;
  int written = 0;
  if (EVP_CipherUpdate(ctx, nullptr, &written, aad.data, static_cast<int>(aad.size)) != 1)
    return PgpCode::provider_failure;
  std::size_t read = 0, produced = 0;
  while (read < data.size) {
    if (Cancelled(cancellation)) return PgpCode::cancelled;
    const auto count = std::min(kSlice, data.size - read);
    if (EVP_CipherUpdate(ctx, out.data + produced, &written, data.data + read,
                         static_cast<int>(count)) != 1 || written < 0 ||
        static_cast<std::size_t>(written) > out.size - produced) return PgpCode::provider_failure;
    produced += static_cast<std::size_t>(written);
    read += count;
  }
  if (EVP_CipherFinal_ex(ctx, final.bytes.data(), &written) != 1)
    return encrypt ? PgpCode::provider_failure : PgpCode::authentication_failed;
  if (written < 0 || static_cast<std::size_t>(written) > final.bytes.size() ||
      static_cast<std::size_t>(written) != out.size - produced) return PgpCode::provider_failure;
  if (written) std::memcpy(out.data + produced, final.bytes.data(), static_cast<std::size_t>(written));
  if (encrypt) {
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, kTag, tag.bytes.data()) != 1)
      return PgpCode::provider_failure;
    std::memcpy(output_tag, tag.bytes.data(), kTag);
  }
  return PgpCode::ok;
}

PgpCode Process(bool encrypt, SeipdProfile profile, PgpInput key, PgpInput salt,
                PgpInput input, PgpOutput output, std::size_t plaintext_size, PgpCancellation cancellation) {
  OutputOwner pending{output};
  if (Cancelled(cancellation)) return PgpCode::cancelled;
  PgpCode result;
  {
    // Release backend contexts and all derived material BEFORE the last probe.
    Secret<39> material;
    Secret<15> nonce;
    std::array<unsigned char, 13> aad{0xd2, 2, profile.cipher, profile.aead, profile.chunk};
    const auto nonce_size = NonceSize(profile), key_size = KeySize(profile);
    if (!Derive(key, salt, aad, material.bytes.data(), key_size + nonce_size - 8))
      return PgpCode::provider_failure;
    std::memcpy(nonce.bytes.data(), material.bytes.data() + key_size, nonce_size - 8);
    const auto* cipher = Cipher(profile);
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!cipher || !ctx) return PgpCode::provider_failure;
    if (encrypt) {
      std::memcpy(output.data, aad.data() + 1, 4);
      std::memcpy(output.data + 4, salt.data, 32);
    }
    std::size_t plain_at = 0, body_at = kHeader;
    std::uint64_t index = 0;
    const auto chunks = encrypt ? (plaintext_size ? plaintext_size / ChunkSize(profile) +
        (plaintext_size % ChunkSize(profile) != 0) : 1) :
        (input.size - kHeader - kTag - plaintext_size) / kTag;
    do {
      const auto count = std::min(ChunkSize(profile), plaintext_size - plain_at);
      BigEndian(nonce.bytes.data() + nonce_size - 8, index);
      const auto* source = encrypt ? (count ? input.data + plain_at : nullptr) : input.data + body_at;
      auto* destination = encrypt ? output.data + body_at : (count ? output.data + plain_at : nullptr);
      result = Chunk(ctx.get(), cipher, encrypt, {material.bytes.data(), key_size},
          {nonce.bytes.data(), nonce_size}, {aad.data(), 5}, {source, count}, {destination, count},
          encrypt ? nullptr : input.data + body_at + count,
          encrypt ? output.data + body_at + count : nullptr, cancellation);
      if (result != PgpCode::ok) return result;
      plain_at += count;
      body_at += count + kTag;
      ++index;
    } while (index < chunks);
    BigEndian(nonce.bytes.data() + nonce_size - 8, index);
    BigEndian(aad.data() + 5, plaintext_size);
    result = Chunk(ctx.get(), cipher, encrypt, {material.bytes.data(), key_size},
        {nonce.bytes.data(), nonce_size}, {aad.data(), aad.size()}, {}, {},
        encrypt ? nullptr : input.data + body_at, encrypt ? output.data + body_at : nullptr, cancellation);
  }
  if (result != PgpCode::ok) return result;
  if (Cancelled(cancellation)) return PgpCode::cancelled;
  pending.accepted = true;
  return PgpCode::ok;
}
}  // namespace

PgpSize SeipdEncryptedSize(SeipdProfile p, std::size_t size) noexcept {
  if (!ProfileValid(p)) return {PgpCode::invalid_profile, 0};
  const auto chunk = ChunkSize(p);
  const auto chunks = size ? size / chunk + (size % chunk != 0) : 1;
  const auto max = std::numeric_limits<std::size_t>::max();
  if (size > max - kHeader - kTag || chunks > (max - kHeader - kTag - size) / kTag)
    return {PgpCode::size_overflow, 0};
  return {PgpCode::ok, kHeader + size + (chunks + 1) * kTag};
}
PgpSize SeipdPlaintextSize(PgpInput body) noexcept {
  if (!Extent(body)) return {PgpCode::invalid_extent, 0};
  if (body.size < kHeader + 2*kTag || body.data[0] != 2) return {PgpCode::invalid_packet, 0};
  SeipdProfile p{body.data[1], body.data[2], body.data[3]};
  if (!ProfileValid(p)) return {PgpCode::invalid_profile, 0};
  const auto encrypted = body.size - kHeader - kTag, full = ChunkSize(p) + kTag;
  const auto remainder = encrypted % full;
  if (remainder && remainder < kTag)
    return {PgpCode::invalid_packet, 0};
  const auto chunks = encrypted / full + (remainder != 0);
  return {PgpCode::ok, encrypted - chunks*kTag};
}
PgpCode EncryptSeipdV2(SeipdProfile p, PgpInput key, PgpInput salt,
                      PgpInput plaintext, PgpOutput body, PgpCancellation cancellation) {
  const auto size = SeipdEncryptedSize(p, plaintext.size);
  if (size.code != PgpCode::ok) return size.code;
  if (!Buffers(body, key, plaintext, salt) || key.size != KeySize(p) || salt.size != 32 || body.size != size.bytes)
    return PgpCode::invalid_extent;
  return Process(true, p, key, salt, plaintext, body, plaintext.size, cancellation);
}
PgpCode DecryptSeipdV2(PgpInput key, PgpInput body, PgpOutput plaintext, PgpCancellation cancellation) {
  if (!Buffers(plaintext, key, body)) return PgpCode::invalid_extent;
  const auto size = SeipdPlaintextSize(body);
  if (size.code != PgpCode::ok) return size.code;
  SeipdProfile p{body.data[1], body.data[2], body.data[3]};
  if (key.size != KeySize(p) || plaintext.size != size.bytes) return PgpCode::invalid_extent;
  return Process(false, p, key, {body.data + 4, 32}, body, plaintext, size.bytes, cancellation);
}
}  // namespace scratchbird::core::crypto

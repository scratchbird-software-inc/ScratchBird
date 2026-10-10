// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "openpgp_seipd.hpp"
#include "argon2_kdf.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
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
bool Derive(PgpInput key, PgpInput salt, PgpInput info,
            std::uint8_t* output, std::size_t size) {
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
      EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
  if (!ctx || EVP_PKEY_derive_init(ctx.get()) <= 0 ||
      EVP_PKEY_CTX_set_hkdf_md(ctx.get(), EVP_sha256()) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_salt(ctx.get(), salt.data, static_cast<int>(salt.size)) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_key(ctx.get(), key.data, static_cast<int>(key.size)) <= 0 ||
      EVP_PKEY_CTX_add1_hkdf_info(ctx.get(), info.data, static_cast<int>(info.size)) <= 0) return false;
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
    if (!Derive(key, salt, {aad.data(), 5}, material.bytes.data(), key_size + nonce_size - 8))
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

PgpCode IteratedKey(PgpInput password, PgpInput salt, std::uint8_t encoded,
    std::uint8_t* key, PgpCancellation cancellation) {
  // Avoid millions of provider calls for short passwords: fill a bounded
  // whole-period buffer once, then reuse it. Long passwords remain borrowed.
  Secret<4096> repeated;
  const auto period = password.size + salt.size; // Checked by public entry.
  const std::size_t count = (std::size_t{16} + (encoded & 15)) << ((encoded >> 4) + 6);
  auto remaining = std::max(count, period);
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
    return PgpCode::provider_failure;
  std::size_t batch = 0;
  if (period <= repeated.bytes.size()) {
    batch = repeated.bytes.size() / period * period;
    std::memcpy(repeated.bytes.data(), salt.data, salt.size);
    if (password.size) std::memcpy(repeated.bytes.data() + salt.size, password.data, password.size);
    for (std::size_t filled = period; filled < batch;) {
      const auto n = std::min(filled, batch - filled);
      std::memcpy(repeated.bytes.data() + filled, repeated.bytes.data(), n);
      filled += n;
    }
  }
  std::size_t offset = 0;
  while (remaining) {
    if (Cancelled(cancellation)) return PgpCode::cancelled;
    const auto* source = repeated.bytes.data();
    std::size_t n;
    if (batch) n = std::min(remaining, batch);
    else {
      const bool in_salt = offset < salt.size;
      source = in_salt ? salt.data + offset : password.data + offset - salt.size;
      n = std::min({remaining, std::size_t{4096}, (in_salt ? salt.size : period) - offset});
      offset += n;
      if (offset == period) offset = 0;
    }
    if (EVP_DigestUpdate(ctx.get(), source, n) != 1) return PgpCode::provider_failure;
    remaining -= n;
  }
  unsigned actual = 0;
  return EVP_DigestFinal_ex(ctx.get(), key, &actual) == 1 && actual == 32 ?
      PgpCode::ok : PgpCode::provider_failure;
}

template<class KeyDeriver>
PgpCode SessionKey(bool encrypt, SeipdProfile p, PgpInput specifier,
    PgpInput nonce, PgpInput input, PgpOutput output, PgpCancellation cancellation,KeyDeriver derive_key) {
  OutputOwner pending{output};
  {
    Secret<32> s2k, kek;
    const auto derived = derive_key(s2k.bytes.data());
    if (derived != PgpCode::ok) return derived;
    const std::array<unsigned char,4> info{0xc3,6,p.cipher,p.aead};
    const auto key_size = KeySize(p), start = 5 + specifier.size + nonce.size;
    const auto wrapped_size = encrypt ? input.size : output.size;
    if (!Derive({s2k.bytes.data(),key_size},{},{info.data(),info.size()},kek.bytes.data(),key_size))
      return PgpCode::provider_failure;
    const auto* cipher = Cipher(p);
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);
    if (!cipher || !ctx) return PgpCode::provider_failure;
    if (encrypt) {
      const std::array<unsigned char,5> header{6,static_cast<unsigned char>(3+specifier.size+nonce.size),p.cipher,p.aead,static_cast<unsigned char>(specifier.size)};
      std::memcpy(output.data,header.data(),header.size());
      std::memcpy(output.data+5,specifier.data,specifier.size);
      std::memcpy(output.data+5+specifier.size,nonce.data,nonce.size);
    }
    const auto result = Chunk(ctx.get(),cipher,encrypt,{kek.bytes.data(),key_size},nonce,
        {info.data(),info.size()}, {encrypt?input.data:input.data+start,wrapped_size},
        {encrypt?output.data+start:output.data,wrapped_size},
        encrypt?nullptr:input.data+start+wrapped_size,encrypt?output.data+start+wrapped_size:nullptr,cancellation);
    if (result != PgpCode::ok) return result;
  }
  if (Cancelled(cancellation)) return PgpCode::cancelled;
  pending.accepted=true;
  return PgpCode::ok;
}
PgpCode Argon2Key(Argon2S2kProfile profile,PgpInput password,PgpInput salt,PgpOutput workspace,
    std::uint8_t* key,std::size_t key_size,PgpCancellation cancellation) {
  const auto code=DeriveArgon2idKey({std::uint32_t{1}<<profile.memory_exponent,profile.passes,profile.lanes},
      {password.data,password.size},{salt.data,salt.size},{workspace.data,workspace.size},
      {key,key_size},{cancellation.requested,cancellation.context});
  if(code==Argon2Code::ok)return PgpCode::ok;
  if(code==Argon2Code::cancelled)return PgpCode::cancelled;
  return PgpCode::provider_failure; // All structural admission was already checked.
}
bool WorkspaceValid(PgpOutput work,PgpOutput output,std::initializer_list<PgpInput> inputs) noexcept {
  const PgpInput w{work.data,work.size},o{output.data,output.size};
  if(!Extent(w)||!Extent(o)||reinterpret_cast<std::uintptr_t>(work.data)%alignof(std::uint64_t)||Overlaps(w,o))return false;
  for(auto in:inputs)if(!Extent(in)||Overlaps(in,w)||Overlaps(in,o))return false;
  return true;
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
PgpSize IteratedSkeskV6Size(std::uint8_t cipher,std::uint8_t aead,std::size_t session_key_bytes) noexcept {
  const SeipdProfile p{cipher,aead,0};
  if (!ProfileValid(p)) return {PgpCode::invalid_profile,0};
  if (session_key_bytes!=16 && session_key_bytes!=24 && session_key_bytes!=32)
    return {PgpCode::invalid_extent,0};
  return {PgpCode::ok,16+NonceSize(p)+session_key_bytes+kTag};
}
PgpSize IteratedSkeskV6KeySize(PgpInput body) noexcept {
  if (!Extent(body)) return {PgpCode::invalid_extent,0};
  if (body.size<16 || body.data[0]!=6) return {PgpCode::invalid_packet,0};
  const SeipdProfile p{body.data[2],body.data[3],0};
  if (!ProfileValid(p)) return {PgpCode::invalid_profile,0};
  const auto overhead=16+NonceSize(p)+kTag;
  if (body.size<overhead || body.data[1]!=14+NonceSize(p) || body.data[4]!=11 ||
      body.data[5]!=3 || body.data[6]!=8) return {PgpCode::invalid_packet,0};
  const auto size=body.size-overhead;
  if (size!=16 && size!=24 && size!=32) return {PgpCode::invalid_packet,0};
  return {PgpCode::ok,size};
}
PgpCode EncryptIteratedSkeskV6(std::uint8_t cipher,std::uint8_t aead,std::uint8_t encoded_count,
    PgpInput password,PgpInput salt,PgpInput nonce,PgpInput session_key,PgpOutput body,PgpCancellation cancellation) {
  const auto expected=IteratedSkeskV6Size(cipher,aead,session_key.size);
  if (expected.code!=PgpCode::ok) return expected.code;
  const SeipdProfile p{cipher,aead,0};
  if (!Buffers(body,password,salt,nonce) || !Extent(session_key) ||
      Overlaps({body.data,body.size},session_key) || salt.size!=8 || nonce.size!=NonceSize(p) ||
      body.size!=expected.bytes ||
      password.size>std::numeric_limits<std::size_t>::max()-8 ||
      password.size>std::numeric_limits<std::uint64_t>::max()/8-8) return PgpCode::invalid_extent;
  std::array<unsigned char,11> spec{3,8};std::memcpy(spec.data()+2,salt.data,8);spec[10]=encoded_count;
  return SessionKey(true,p,{spec.data(),spec.size()},nonce,session_key,body,cancellation,[&](auto* key){
    if(Cancelled(cancellation))return PgpCode::cancelled;
    return IteratedKey(password,salt,encoded_count,key,cancellation);
  });
}
PgpCode DecryptIteratedSkeskV6(PgpInput password,PgpInput body,PgpOutput session_key,PgpCancellation cancellation) {
  if (!Buffers(session_key,password,body) || password.size>std::numeric_limits<std::size_t>::max()-8 ||
      password.size>std::numeric_limits<std::uint64_t>::max()/8-8)
    return PgpCode::invalid_extent;
  const auto expected=IteratedSkeskV6KeySize(body);
  if (expected.code!=PgpCode::ok) return expected.code;
  if (session_key.size!=expected.bytes) return PgpCode::invalid_extent;
  const SeipdProfile p{body.data[2],body.data[3],0};
  return SessionKey(false,p,{body.data+5,11},{body.data+16,NonceSize(p)},body,session_key,cancellation,[&](auto* key){
    if(Cancelled(cancellation))return PgpCode::cancelled;
    return IteratedKey(password,{body.data+7,8},body.data[15],key,cancellation);
  });
}
PgpSize Argon2S2kWorkspaceSize(Argon2S2kProfile p) noexcept {
  if(!p.passes||!p.lanes||p.memory_exponent<3||p.memory_exponent>31)return {PgpCode::invalid_profile,0};
  const auto size=Argon2idWorkspaceBytes({std::uint32_t{1}<<p.memory_exponent,p.passes,p.lanes});
  if(size.code==Argon2Code::size_overflow)return {PgpCode::size_overflow,0};
  if(size.code!=Argon2Code::ok)return {PgpCode::invalid_profile,0};
  return {PgpCode::ok,static_cast<std::size_t>(size.bytes)};
}
PgpSize Argon2SkeskV6Size(std::uint8_t cipher,std::uint8_t aead,std::size_t key_size) noexcept {
  auto size=IteratedSkeskV6Size(cipher,aead,key_size);
  if(size.code==PgpCode::ok)size.bytes+=9;return size;
}
Argon2SkeskDescription InspectArgon2SkeskV6(PgpInput body) noexcept {
  Argon2SkeskDescription out;
  if(!Extent(body)){out.code=PgpCode::invalid_extent;return out;}
  if(body.size<25||body.data[0]!=6||body.data[4]!=20||body.data[5]!=4)return out;
  out.cipher=body.data[2];out.aead=body.data[3];out.s2k={body.data[22],body.data[23],body.data[24]};
  const SeipdProfile p{out.cipher,out.aead,0};
  if(!ProfileValid(p)){out.code=PgpCode::invalid_profile;return out;}
  const auto overhead=25+NonceSize(p)+16;
  if(body.size<overhead||body.data[1]!=23+NonceSize(p))return out;
  const auto key_size=body.size-overhead;
  if(key_size!=16&&key_size!=24&&key_size!=32)return out;
  const auto workspace=Argon2S2kWorkspaceSize(out.s2k);out.code=workspace.code;
  if(out.code==PgpCode::ok){out.session_key_bytes=key_size;out.workspace_bytes=workspace.bytes;}
  return out;
}
PgpCode EncryptArgon2SkeskV6(std::uint8_t cipher,std::uint8_t aead,Argon2S2kProfile profile,
    PgpInput password,PgpInput salt,PgpInput nonce,PgpInput session_key,PgpOutput workspace,PgpOutput body,PgpCancellation cancellation) {
  const auto size=Argon2SkeskV6Size(cipher,aead,session_key.size),work=Argon2S2kWorkspaceSize(profile);
  if(size.code!=PgpCode::ok)return size.code;if(work.code!=PgpCode::ok)return work.code;
  const SeipdProfile p{cipher,aead,0};
  if(password.size>UINT32_MAX||salt.size!=16||nonce.size!=NonceSize(p)||body.size!=size.bytes||workspace.size!=work.bytes||
     !WorkspaceValid(workspace,body,{password,salt,nonce,session_key}))return PgpCode::invalid_extent;
  std::array<unsigned char,20> spec{4};std::memcpy(spec.data()+1,salt.data,16);
  spec[17]=profile.passes;spec[18]=profile.lanes;spec[19]=profile.memory_exponent;
  return SessionKey(true,p,{spec.data(),spec.size()},nonce,session_key,body,cancellation,[&](auto* key){
    return Argon2Key(profile,password,salt,workspace,key,KeySize(p),cancellation);
  });
}
PgpCode DecryptArgon2SkeskV6(PgpInput password,PgpInput body,PgpOutput workspace,PgpOutput session_key,PgpCancellation cancellation) {
  if(!WorkspaceValid(workspace,session_key,{password,body})||password.size>UINT32_MAX)return PgpCode::invalid_extent;
  const auto description=InspectArgon2SkeskV6(body);if(description.code!=PgpCode::ok)return description.code;
  if(session_key.size!=description.session_key_bytes||workspace.size!=description.workspace_bytes)return PgpCode::invalid_extent;
  const SeipdProfile p{description.cipher,description.aead,0};
  return SessionKey(false,p,{body.data+5,20},{body.data+25,NonceSize(p)},body,session_key,cancellation,[&](auto* key){
    return Argon2Key(description.s2k,password,{body.data+6,16},workspace,key,KeySize(p),cancellation);
  });
}
}  // namespace scratchbird::core::crypto

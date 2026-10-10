// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "openpgp_seipd.hpp"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace c = scratchbird::core::crypto;
using Bytes = std::vector<std::uint8_t>;
using Code = c::PgpCode;
unsigned checks = 0;
#ifdef SB_PGP_PROVIDER_FAULTS
unsigned failure_kind = 0, failure_at = 0, provider_calls = 0;
bool Fail(unsigned kind) { return kind == failure_kind && ++provider_calls == failure_at; }
extern "C" {
EVP_PKEY_CTX* __real_EVP_PKEY_CTX_new_id(int, ENGINE*);
EVP_PKEY_CTX* __wrap_EVP_PKEY_CTX_new_id(int id, ENGINE* engine) { return Fail(1) ? nullptr : __real_EVP_PKEY_CTX_new_id(id, engine); }
int __real_EVP_PKEY_derive(EVP_PKEY_CTX*, unsigned char*, std::size_t*);
int __wrap_EVP_PKEY_derive(EVP_PKEY_CTX* ctx, unsigned char* out, std::size_t* size) { return Fail(2) ? 0 : __real_EVP_PKEY_derive(ctx, out, size); }
EVP_CIPHER_CTX* __real_EVP_CIPHER_CTX_new();
EVP_CIPHER_CTX* __wrap_EVP_CIPHER_CTX_new() { return Fail(3) ? nullptr : __real_EVP_CIPHER_CTX_new(); }
int __real_EVP_CipherInit_ex(EVP_CIPHER_CTX*, const EVP_CIPHER*, ENGINE*, const unsigned char*, const unsigned char*, int);
int __wrap_EVP_CipherInit_ex(EVP_CIPHER_CTX* ctx, const EVP_CIPHER* cipher, ENGINE* engine, const unsigned char* key, const unsigned char* iv, int enc) {
  return Fail(4) ? 0 : __real_EVP_CipherInit_ex(ctx, cipher, engine, key, iv, enc);
}
int __real_EVP_CipherUpdate(EVP_CIPHER_CTX*, unsigned char*, int*, const unsigned char*, int);
int __wrap_EVP_CipherUpdate(EVP_CIPHER_CTX* ctx, unsigned char* out, int* written, const unsigned char* in, int size) {
  if (Fail(5)) { if (out && size) out[0] = 0xcc; return 0; }
  return __real_EVP_CipherUpdate(ctx, out, written, in, size);
}
int __real_EVP_CipherFinal_ex(EVP_CIPHER_CTX*, unsigned char*, int*);
int __wrap_EVP_CipherFinal_ex(EVP_CIPHER_CTX* ctx, unsigned char* out, int* written) {
  if (Fail(6)) { out[0] = 0xcc; return 0; }
  return __real_EVP_CipherFinal_ex(ctx, out, written);
}
int __real_EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX*, int, int, void*);
int __wrap_EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX* ctx, int type, int count, void* data) {
  return Fail(7) ? 0 : __real_EVP_CIPHER_CTX_ctrl(ctx, type, count, data);
}
}
#endif
void Check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
c::PgpInput In(const Bytes& v) { return {v.data(), v.size()}; }
c::PgpOutput Out(Bytes& v) { return {v.data(), v.size()}; }
bool Zero(const Bytes& v) { return std::all_of(v.begin(), v.end(), [](auto x) { return x == 0; }); }
Bytes Hex(std::string_view s) {
  Check(s.size() % 2 == 0, "bad oracle hex");
  Bytes out;
  const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i < s.size(); i += 2) out.push_back(nibble(s[i])*16 + nibble(s[i+1]));
  return out;
}
Bytes Encode(c::SeipdProfile p, const Bytes& key, const Bytes& salt, const Bytes& plain) {
  const auto size = c::SeipdEncryptedSize(p, plain.size());
  Check(size.code == Code::ok, "size admission");
  Bytes body(size.bytes, 0xa5);
  Check(c::EncryptSeipdV2(p, In(key), In(salt), In(plain), Out(body)) == Code::ok, "encrypt");
  return body;
}
void Decode(const Bytes& key, const Bytes& body, const Bytes& expected) {
  const auto size = c::SeipdPlaintextSize(In(body));
  Check(size.code == Code::ok && size.bytes == expected.size(), "receive size");
  Bytes plain(size.bytes, 0xa5);
  Check(c::DecryptSeipdV2(In(key), In(body), Out(plain)) == Code::ok && plain == expected, "decrypt exact bytes");
}
void RfcVectors() {
  // Independent RFC9580 Appendices A.10.3/A.10.4 and A.11.3/A.11.4.
  // https://www.rfc-editor.org/rfc/rfc9580.html#appendix-A.10.3
  // Bodies omit the d2/69 outer packet header. Plaintext is Literal+Padding.
  struct Vector { unsigned mode; const char* key; const char* body; const char* padding; };
  const Vector vectors[]{
    {2, "28e79ab82397d3c63de24ac217d7b791",
     "0207020620a661f731fc9a3032b5623326027e3a5d8db5748ebeff0b0c5910d09ecdd641"
     "ff9fd38562758035bc49754ce1bf3fffa7dad0a3b8104f5133cf42a4100a83eef4ca1b4801"
     "a8846bf42bcda7c8ce9d65e212f301cbcd98fdcade694a877ad4247323f6e857",
     "d50eae6aa1649b56aa835b2613902bd2"},
    {3, "1936fc8568980274bb900d8319360c77",
     "02070306fcb94490bcb98bbdc9d106c6090266940f72e89edc21b5596b1576b101ed0f9f"
     "fc6fc6d65bbfd24dcd0790966e6d1e85a30053784cb1d8b6a0699ef12155a7b2ad6258531b"
     "57651fd7777912fa95e35d9b40216f69a4c248db28ff4331f1632907399e6ff9",
     "d50e1ce2269a9eddef81032172b7ed7c"}
  };
  for (const auto& v : vectors) {
    const auto key = Hex(v.key), body = Hex(v.body);
    auto plain = Hex("cb1362000000000048656c6c6f2c20776f726c6421");
    const auto padding = Hex(v.padding); plain.insert(plain.end(), padding.begin(), padding.end());
    const Bytes salt(body.begin()+4, body.begin()+36);
    Check(Encode({7, static_cast<std::uint8_t>(v.mode), 6}, key, salt, plain) == body, "RFC ciphertext/tag known answer");
    Decode(key, body, plain);
    for (std::size_t i = 4; i < body.size(); ++i) {
      auto corrupted = body; corrupted[i] ^= 1;
      Bytes output(plain.size(), 0xa5);
      Check(c::DecryptSeipdV2(In(key), In(corrupted), Out(output)) == Code::authentication_failed && Zero(output),
            "tampering cannot publish plaintext, including final tag");
    }
    for (std::size_t i = 0; i < key.size(); ++i) {
      auto bad = key; bad[i] ^= 1; Bytes output(plain.size(), 0xa5);
      Check(c::DecryptSeipdV2(In(bad), In(body), Out(output)) == Code::authentication_failed && Zero(output), "wrong key erases output");
    }
  }
}
void Boundaries() {
  const Bytes salt(32, 0x71);
  for (unsigned cipher = 7; cipher <= 9; ++cipher) for (unsigned mode : {2u, 3u}) {
    const Bytes key(16 + (cipher-7)*8, 0x31);
    for (unsigned chunk = 0; chunk <= 16; ++chunk) {
      const c::SeipdProfile p{static_cast<std::uint8_t>(cipher), static_cast<std::uint8_t>(mode), static_cast<std::uint8_t>(chunk)};
      const std::size_t width = std::size_t{1} << (chunk+6);
      for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{15}, std::size_t{16}, std::size_t{17}, width-1, width, width+1, 2*width}) {
        Bytes plain(size);
        for (std::size_t i = 0; i < size; ++i) plain[i] = static_cast<std::uint8_t>(i*71+3);
        const auto body = Encode(p, key, salt, plain);
        Check(body.size() == 36 + size + 16*(size ? size/width+(size%width != 0) : 1)+16, "exact chunk sizing");
        Decode(key, body, plain);
        auto changed = body; changed.back() ^= 1;
        Bytes output(size, 0xa5);
        Check(c::DecryptSeipdV2(In(key), In(changed), Out(output)) == Code::authentication_failed && Zero(output), "final authentication after all data chunks");
      }
    }
  }
}
struct ProbeException {};
struct Probe {
  unsigned calls = 0, stop = 0;
  bool throwing = false;
  static bool Poll(void* context) {
    auto& p = *static_cast<Probe*>(context);
    if (++p.calls != p.stop) return false;
    if (p.throwing) throw ProbeException{};
    return true;
  }
  c::PgpCancellation callback() { return {Poll, this}; }
};
void Cancellation() {
  const Bytes key(32, 0x33), salt(32, 0x81), plain(150000, 0x27);
  const auto body = Encode({}, key, salt, plain);
  for (bool decrypt : {false, true}) for (bool throwing : {false, true}) {
    Probe counter;
    Bytes output(decrypt ? plain.size() : body.size(), 0xa5);
    const auto run = [&](Probe& p) { return decrypt ? c::DecryptSeipdV2(In(key), In(body), Out(output), p.callback()) :
        c::EncryptSeipdV2({}, In(key), In(salt), In(plain), Out(output), p.callback()); };
    Check(run(counter) == Code::ok && counter.calls > 5, "bounded probe points");
    for (unsigned at = 1; at <= counter.calls; ++at) {
      Probe probe{0, at, throwing}; std::fill(output.begin(), output.end(), 0xa5);
      bool threw = false;
      try { Check(run(probe) == Code::cancelled, "cancel return"); }
      catch (const ProbeException&) { threw = true; }
      Check(threw == throwing && probe.calls == at && Zero(output), "all cancellation/exception points erase accepted output");
    }
  }
}
void InvalidExtents() {
  Bytes key(32, 0x33), salt(32, 0x51), plain(65, 0x27);
  auto body = Encode({}, key, salt, plain);
  Bytes output(body.size(), 0xa5);
  auto bad_key = key; bad_key.pop_back();
  Check(c::EncryptSeipdV2({}, In(bad_key), In(salt), In(plain), Out(output)) == Code::invalid_extent, "short key");
  Check(std::all_of(output.begin(), output.end(), [](auto x) { return x == 0xa5; }), "invalid admission never writes");
  const auto saved = output;
  Check(c::EncryptSeipdV2({}, {output.data(),32}, In(salt), In(plain), Out(output)) == Code::invalid_extent && output == saved, "key/output alias");
  Check(c::EncryptSeipdV2({}, In(key), {output.data(),32}, In(plain), Out(output)) == Code::invalid_extent && output == saved, "salt/output alias");
  Check(c::EncryptSeipdV2({}, In(key), In(salt), {output.data(),plain.size()}, Out(output)) == Code::invalid_extent && output == saved, "plain/output alias");
  Check(c::EncryptSeipdV2({}, In(key), In(salt), {nullptr,65}, Out(output)) == Code::invalid_extent && output == saved, "null-positive input");
  Check(c::DecryptSeipdV2(In(key), In(body), {body.data()+10,plain.size()}) == Code::invalid_extent, "decrypt/input alias");
  Check(c::DecryptSeipdV2({output.data(),32}, In(body), {output.data(),plain.size()}) == Code::invalid_extent && output == saved, "decrypt/key alias");
  Check(c::EncryptSeipdV2({}, In(key), In(salt), In(plain), {output.data(),output.size()-1}) == Code::invalid_extent && output == saved, "short encryption output");
  Check(c::DecryptSeipdV2(In(key), In(body), {output.data(),plain.size()-1}) == Code::invalid_extent && output == saved, "short decryption output");
  for (std::size_t count = 0; count < body.size(); ++count) {
    Bytes truncated(body.begin(), body.begin()+count);
    const auto size = c::SeipdPlaintextSize(In(truncated));
    if (size.code != Code::ok) continue;
    Bytes decoded(size.bytes, 0xa5);
    Check(c::DecryptSeipdV2(In(key), In(truncated), Out(decoded)) != Code::ok && Zero(decoded), "truncated message never authenticates");
  }
  for (const c::SeipdProfile invalid : {c::SeipdProfile{6,2,10}, {10,2,10}, {9,0,10}, {9,1,10}, {9,2,17}})
    Check(c::SeipdEncryptedSize(invalid, 1).code == Code::invalid_profile, "unsupported profile never substitutes an algorithm");
  Check(c::SeipdEncryptedSize({}, std::numeric_limits<std::size_t>::max()).code == Code::size_overflow, "size overflow before allocation");
  auto malformed = body; malformed[0] = 1;
  Check(c::SeipdPlaintextSize(In(malformed)).code == Code::invalid_packet, "native default rejects legacy container");
}
void ProviderFailures() {
#ifdef SB_PGP_PROVIDER_FAULTS
  const Bytes key(32, 0x22), salt(32, 0x85), plain(150000, 0x73);
  for (unsigned mode : {2u,3u}) {
    const c::SeipdProfile profile{9,static_cast<std::uint8_t>(mode),10};
    const auto body = Encode(profile,key,salt,plain);
    for (bool decrypt : {false,true}) for (unsigned kind = 1; kind <= 7; ++kind) {
      Bytes output(decrypt ? plain.size() : body.size(), 0xa5);
      const auto run = [&] { return decrypt ? c::DecryptSeipdV2(In(key),In(body),Out(output)) :
          c::EncryptSeipdV2(profile,In(key),In(salt),In(plain),Out(output)); };
      failure_kind=kind; failure_at=0; provider_calls=0;
      const auto baseline=run(); const auto count=provider_calls; failure_kind=0;
      Check(baseline==Code::ok && count>0,"provider failure baseline");
      for (unsigned at=1; at<=count; ++at) {
        std::fill(output.begin(),output.end(),0xa5);
        failure_kind=kind; failure_at=at; provider_calls=0;
        const auto result=run(); const auto calls=provider_calls; failure_kind=0;
        Check(result!=Code::ok && calls==at && Zero(output),"every provider failure clears partial output");
        Check(run()==Code::ok,"provider failure does not poison subsequent independent call");
      }
    }
  }
#endif
}
void ReceiveShapeAndChunkIntegrity() {
  const Bytes key(16,0x23), salt(32,0x67), plain(128,0x81);
  const auto body=Encode({7,2,0},key,salt,plain);
  // Equal full chunks: ciphertext is still bound to its distinct nonce index.
  for (bool duplicate : {false,true}) {
    auto bad=body;
    if (duplicate) std::copy_n(body.begin()+36,80,bad.begin()+116);
    else std::swap_ranges(bad.begin()+36,bad.begin()+116,bad.begin()+116);
    Bytes out(128,0xa5);
    Check(c::DecryptSeipdV2(In(key),In(bad),Out(out))==Code::authentication_failed && Zero(out),"chunk reorder/duplication fails authentication");
  }
  for (unsigned field : {2u,3u}) {
    auto bad=body; bad[field]=field==2?3:1;
    const auto size=c::SeipdPlaintextSize(In(bad));
    Check(size.code==Code::ok,"changed header remains structurally valid");
    Bytes out(size.bytes,0xa5);
    Check(c::DecryptSeipdV2(In(key),In(bad),Out(out))==Code::authentication_failed && Zero(out),"valid header fields authenticated");
  }
  // Independent HKDF/HMAC + EVP construction of the accepted trailing-empty
  // chunk. Do not use the production HKDF/container encoder for new tags.
  const Bytes one_chunk(64,0x81);
  auto extended=Encode({7,2,0},key,salt,one_chunk);
  std::array<unsigned char,32> prk{}, material{};
  unsigned length=0;
  Check(HMAC(EVP_sha256(),salt.data(),32,key.data(),16,prk.data(),&length) && length==32,"reference extract");
  const std::array<unsigned char,6> info{0xd2,2,7,2,0,1};
  Check(HMAC(EVP_sha256(),prk.data(),32,info.data(),info.size(),material.data(),&length) && length==32,"reference expand");
  const auto empty_tag=[&](unsigned index, bool summary) {
    std::array<unsigned char,15> nonce{}; std::copy_n(material.begin()+16,7,nonce.begin()); nonce[14]=index;
    std::array<unsigned char,13> aad{0xd2,2,7,2,0}; aad[12]=64;
    std::array<unsigned char,16> tag{},scratch{};
    auto* ctx=EVP_CIPHER_CTX_new(); Check(ctx!=nullptr,"reference context");
    int n=0;
    const bool ok=EVP_EncryptInit_ex(ctx,EVP_aes_128_ocb(),nullptr,nullptr,nullptr)==1 &&
        EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_AEAD_SET_IVLEN,15,nullptr)==1 &&
        EVP_EncryptInit_ex(ctx,nullptr,nullptr,material.data(),nonce.data())==1 &&
        EVP_EncryptUpdate(ctx,nullptr,&n,aad.data(),summary?13:5)==1 &&
        EVP_EncryptFinal_ex(ctx,scratch.data(),&n)==1 && n==0 &&
        EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_AEAD_GET_TAG,16,tag.data())==1;
    EVP_CIPHER_CTX_free(ctx); Check(ok,"reference empty authentication tag"); return tag;
  };
  extended.resize(extended.size()-16);
  const auto empty=empty_tag(1,false), final=empty_tag(2,true);
  extended.insert(extended.end(),empty.begin(),empty.end());
  extended.insert(extended.end(),final.begin(),final.end());
  Decode(key,extended,one_chunk);
}
int main() try {
  RfcVectors(); Boundaries(); Cancellation(); InvalidExtents(); ProviderFailures(); ReceiveShapeAndChunkIntegrity();
  std::cout << checks << " authenticated OpenPGP container checks passed\n";
  return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "openpgp_seipd.hpp"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
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
EVP_MD_CTX* __real_EVP_MD_CTX_new();
EVP_MD_CTX* __wrap_EVP_MD_CTX_new() { return Fail(8) ? nullptr : __real_EVP_MD_CTX_new(); }
int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* ctx,const EVP_MD* md,ENGINE* engine) {
  return Fail(9) ? 0 : __real_EVP_DigestInit_ex(ctx,md,engine);
}
int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,std::size_t);
int __wrap_EVP_DigestUpdate(EVP_MD_CTX* ctx,const void* in,std::size_t size) {
  return Fail(10) ? 0 : __real_EVP_DigestUpdate(ctx,in,size);
}
int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned*);
int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* ctx,unsigned char* out,unsigned* size) {
  if (Fail(11)) { out[0]=0xcc; return 0; }
  return __real_EVP_DigestFinal_ex(ctx,out,size);
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
Bytes SessionEncode(unsigned cipher,unsigned aead,unsigned count,const Bytes& password,const Bytes& salt,const Bytes& nonce,const Bytes& key) {
  const auto size=c::IteratedSkeskV6Size(cipher,aead,key.size());Check(size.code==Code::ok,"SKESK size");
  Bytes out(size.bytes,0xa5);
  Check(c::EncryptIteratedSkeskV6(cipher,aead,count,In(password),In(salt),In(nonce),In(key),Out(out))==Code::ok,"SKESK encrypt");
  return out;
}
void SessionRfcVectors() {
  // RFC9580 A.10.1 and A.11.1. Packet bodies, no outer c3/length.
  const Bytes password{'p','a','s','s','w','o','r','d'};
  const char* bodies[]{
    "061d07020b030856a298d2f5e36453ffcfcc5c11664edb9db42590d7dc46b0"
    "7241b612c3812cfffbea00f2347b25641123f887ae60d4fd614e0837d819d36c",
    "061a07030b0308e9d39785b2070008ffb42e7c483ef4884457cb3726"
    "b9b3db9ff776e5f4d9a40952e2447298851abfff7526df2dd554417579a7799f"
  };
  const char* keys[]{"28e79ab82397d3c63de24ac217d7b791","1936fc8568980274bb900d8319360c77"};
  for(unsigned i=0;i<2;++i){
    const auto body=Hex(bodies[i]),key=Hex(keys[i]);
    const Bytes salt(body.begin()+7,body.begin()+15),nonce(body.begin()+16,body.begin()+(i==0?31:28));
    Check(SessionEncode(7,i+2,255,password,salt,nonce,key)==body,"RFC SKESK encryption known answer");
    Bytes output(key.size(),0xa5);
    Check(c::IteratedSkeskV6KeySize(In(body)).bytes==key.size(),"SKESK inspector");
    Check(c::DecryptIteratedSkeskV6(In(password),In(body),Out(output))==Code::ok&&output==key,"RFC SKESK recovered session key");
  }
}

Bytes ReferenceSession(unsigned cipher,unsigned aead,unsigned count,const Bytes& password,const Bytes& salt,const Bytes& nonce,const Bytes& key) {
  // Independent materialized S2K input, one-shot SHA256, then direct HMAC
  // extract/expand and EVP encrypt. Does not call the production derivation.
  const std::size_t period=salt.size()+password.size();
  const auto size=std::max(period,std::size_t(16+(count%16))*(std::size_t{1}<<(count/16+6)));
  Bytes stream(size);
  for(std::size_t i=0;i<size;++i)stream[i]=(i%period<8)?salt[i%period]:password[i%period-8];
  std::array<unsigned char,32> s2k{},prk{},kek{},zeros{};unsigned length=0;
  Check(SHA256(stream.data(),stream.size(),s2k.data())!=nullptr,"reference S2K digest");
  Check(HMAC(EVP_sha256(),zeros.data(),32,s2k.data(),16+(cipher-7)*8,prk.data(),&length)&&length==32,"reference SKESK extract");
  const std::array<unsigned char,5> info{0xc3,6,static_cast<unsigned char>(cipher),static_cast<unsigned char>(aead),1};
  Check(HMAC(EVP_sha256(),prk.data(),32,info.data(),info.size(),kek.data(),&length)&&length==32,"reference SKESK expand");
  const EVP_CIPHER* selected=aead==2?(cipher==7?EVP_aes_128_ocb():cipher==8?EVP_aes_192_ocb():EVP_aes_256_ocb()):
      (cipher==7?EVP_aes_128_gcm():cipher==8?EVP_aes_192_gcm():EVP_aes_256_gcm());
  Bytes out{6,static_cast<unsigned char>(14+nonce.size()),static_cast<unsigned char>(cipher),static_cast<unsigned char>(aead),11,3,8};
  out.insert(out.end(),salt.begin(),salt.end());out.push_back(count);out.insert(out.end(),nonce.begin(),nonce.end());
  const auto start=out.size();out.resize(start+key.size()+16);
  auto* ctx=EVP_CIPHER_CTX_new();Check(ctx!=nullptr,"reference SKESK context");
  int n=0,final=0;
  const bool ok=EVP_EncryptInit_ex(ctx,selected,nullptr,nullptr,nullptr)==1 &&
      EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_AEAD_SET_IVLEN,nonce.size(),nullptr)==1 &&
      EVP_EncryptInit_ex(ctx,nullptr,nullptr,kek.data(),nonce.data())==1 &&
      EVP_EncryptUpdate(ctx,nullptr,&n,info.data(),4)==1 &&
      EVP_EncryptUpdate(ctx,out.data()+start,&n,key.data(),key.size())==1 &&
      EVP_EncryptFinal_ex(ctx,out.data()+start+n,&final)==1 && std::size_t(n+final)==key.size() &&
      EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_AEAD_GET_TAG,16,out.data()+start+key.size())==1;
  EVP_CIPHER_CTX_free(ctx);Check(ok,"reference SKESK encryption");return out;
}
void SessionBoundariesAndFailures() {
  const Bytes salt(8,0x81);
  for(unsigned cipher=7;cipher<=9;++cipher)for(unsigned aead:{2u,3u}){
    const Bytes nonce(aead==2?15:12,0x73),key(16+(cipher-7)*8,0x61);
    for(unsigned password_size:{0u,1u,4087u,4088u,4089u,8192u,65537u})for(unsigned count:{0u,15u,16u,96u}){
      Bytes password(password_size);
      for(unsigned i=0;i<password_size;++i)password[i]=static_cast<std::uint8_t>(i*17);
      const auto body=SessionEncode(cipher,aead,count,password,salt,nonce,key);
      Check(body==ReferenceSession(cipher,aead,count,password,salt,nonce,key),"S2K period/count boundaries match independent construction");
      Bytes output(key.size(),0xa5);
      Check(c::DecryptIteratedSkeskV6(In(password),In(body),Out(output))==Code::ok&&output==key,"S2K boundary decode");
    }
    const Bytes password{'p',0,'x'};
    const auto body=SessionEncode(cipher,aead,0,password,salt,nonce,key);
    for(std::size_t at=7;at<body.size();++at){
      auto bad=body;bad[at]^=1;Bytes output(key.size(),0xa5);
      Check(c::DecryptIteratedSkeskV6(In(password),In(bad),Out(output))==Code::authentication_failed&&Zero(output),"all SKESK salt/count/nonce/ciphertext/tag mutations fail");
    }
    for(std::size_t n=0;n<body.size();++n){
      const auto inspected=c::IteratedSkeskV6KeySize({body.data(),n});
      if(inspected.code!=Code::ok)continue;
      Bytes partial(inspected.bytes,0xa5);
      Check(c::DecryptIteratedSkeskV6(In(password),{body.data(),n},Out(partial))==Code::authentication_failed&&Zero(partial),"truncated SKESK never authenticates");
    }
    for(unsigned field=0;field<7;++field){auto bad=body;bad[field]^=0x80;Check(c::IteratedSkeskV6KeySize(In(bad)).code!=Code::ok,"malformed SKESK profile/header rejected");}
    Bytes output(key.size(),0xa5),bad_password=password;bad_password.back()^=1;
    Check(c::DecryptIteratedSkeskV6(In(bad_password),In(body),Out(output))==Code::authentication_failed&&Zero(output),"wrong SKESK password publishes no key");
    for(bool decrypt:{false,true}){
      output.assign(decrypt?key.size():body.size(),0xa5);
      const auto run=[&](c::PgpCancellation probe={}){return decrypt?c::DecryptIteratedSkeskV6(In(password),In(body),Out(output),probe):
          c::EncryptIteratedSkeskV6(cipher,aead,0,In(password),In(salt),In(nonce),In(key),Out(output),probe);};
      Probe baseline;Check(run(baseline.callback())==Code::ok&&baseline.calls>3,"SKESK probe baseline");
      for(bool throwing:{false,true})for(unsigned at=1;at<=baseline.calls;++at){
        Probe probe{0,at,throwing};std::fill(output.begin(),output.end(),0xa5);bool threw=false;
        try{Check(run(probe.callback())==Code::cancelled,"SKESK cancellation result");}catch(const ProbeException&){threw=true;}
        Check(threw==throwing&&probe.calls==at&&Zero(output),"every SKESK cancellation/throw erases private result");
      }
#ifdef SB_PGP_PROVIDER_FAULTS
      for(unsigned kind=1;kind<=11;++kind){
        failure_kind=kind;failure_at=0;provider_calls=0;const auto initial=run();const auto count=provider_calls;failure_kind=0;
        Check(initial==Code::ok&&count>0,"SKESK provider phase baseline");
        for(unsigned at=1;at<=count;++at){
          std::fill(output.begin(),output.end(),0xa5);failure_kind=kind;failure_at=at;provider_calls=0;
          const auto result=run();const auto calls=provider_calls;failure_kind=0;
          Check(result!=Code::ok&&calls==at&&Zero(output),"every wrapped SKESK provider failure erases output");
          Check(run()==Code::ok,"independent SKESK retry");
        }
      }
#endif
    }
  }
}
void SessionExtentsAndKeySizes() {
  const Bytes password{'a',0,'b'},salt(8,0x57);
  for(unsigned cipher=7;cipher<=9;++cipher)for(unsigned aead:{2u,3u})for(unsigned key_size:{16u,24u,32u}){
    const Bytes nonce(aead==2?15:12,0x94),key(key_size,0x71);
    const auto body=SessionEncode(cipher,aead,1,password,salt,nonce,key);
    Check(body==ReferenceSession(cipher,aead,1,password,salt,nonce,key),"wrapping cipher and session cipher key sizes independent");
    Bytes output(key.size(),0xa5);
    Check(c::IteratedSkeskV6KeySize(In(body)).bytes==key.size()&&
      c::DecryptIteratedSkeskV6(In(password),In(body),Out(output))==Code::ok&&output==key,"independent wrapped key sizes recover");
    output.assign(body.size(),0xa5);const auto saved=output;
    const auto run=[&](c::PgpInput pass,c::PgpInput s,c::PgpInput iv,c::PgpInput k,c::PgpOutput out){
      return c::EncryptIteratedSkeskV6(cipher,aead,0,pass,s,iv,k,out);};
    Check(run({nullptr,1},In(salt),In(nonce),In(key),Out(output))==Code::invalid_extent&&output==saved,"SKESK null-positive password");
    Check(run({output.data(),3},In(salt),In(nonce),In(key),Out(output))==Code::invalid_extent&&output==saved,"SKESK password/output alias");
    Check(run(In(password),{output.data(),8},In(nonce),In(key),Out(output))==Code::invalid_extent&&output==saved,"SKESK salt/output alias");
    Check(run(In(password),In(salt),{output.data(),nonce.size()},In(key),Out(output))==Code::invalid_extent&&output==saved,"SKESK IV/output alias");
    Check(run(In(password),In(salt),In(nonce),{output.data(),key.size()},Out(output))==Code::invalid_extent&&output==saved,"SKESK key/output alias");
    Check(run(In(password),In(salt),In(nonce),In(key),{output.data(),output.size()-1})==Code::invalid_extent&&output==saved,"SKESK short output");
    Check(run(In(password),{salt.data(),7},In(nonce),In(key),Out(output))==Code::invalid_extent&&output==saved,"SKESK short salt");
    Check(run(In(password),In(salt),{nonce.data(),nonce.size()-1},In(key),Out(output))==Code::invalid_extent&&output==saved,"SKESK wrong nonce size");
    Check(run(In(password),In(salt),In(nonce),{key.data(),15},Out(output))==Code::invalid_extent&&output==saved,"SKESK invalid AES key size");
    Check(c::DecryptIteratedSkeskV6({output.data(),3},In(body),{output.data(),key.size()})==Code::invalid_extent&&output==saved,"SKESK decrypt password/output alias");
    Check(c::DecryptIteratedSkeskV6(In(password),In(output),{output.data()+8,key.size()})==Code::invalid_extent&&output==saved,"SKESK decrypt packet/output alias");
    if(sizeof(std::size_t)==8){
      Check(run({password.data(),std::numeric_limits<std::uint64_t>::max()/8},In(salt),In(nonce),In(key),Out(output))==Code::invalid_extent&&output==saved,"S2K SHA256 length overflow refuses before input read");
    }
  }
}
Bytes ReferenceArgonSession(unsigned cipher,unsigned aead,const Bytes& salt,const Bytes& nonce,const Bytes& key) {
  // Frozen libargon2 Argon2id-v0x13 answers, independent of our pinned provider:
  // m=64KiB, t=2, p=3; password[i]=19*i (8 bytes); salt[i]=i (16 bytes).
  // The requested output length participates in H0: do NOT truncate a32-byte KDF.
  const char* answers[]{"260b8859c233d0e9d1c361a411196e15",
    "3f253c42f21cce5c1354ded8571f8993067a6923559b5055",
    "a4326a9dc5f0f1b2dc23b92ca68d1d0c369fa1f7220c3095b00f57cc9aad67c1"};
  const auto s2k=Hex(answers[cipher-7]);
  std::array<unsigned char,32> zeros{},prk{},kek{};unsigned length=0;
  Check(HMAC(EVP_sha256(),zeros.data(),32,s2k.data(),s2k.size(),prk.data(),&length)&&length==32,"reference Argon SKESK extract");
  const std::array<unsigned char,5> info{0xc3,6,static_cast<unsigned char>(cipher),static_cast<unsigned char>(aead),1};
  Check(HMAC(EVP_sha256(),prk.data(),32,info.data(),info.size(),kek.data(),&length)&&length==32,"reference Argon SKESK expand");
  const EVP_CIPHER* selected=aead==2?(cipher==7?EVP_aes_128_ocb():cipher==8?EVP_aes_192_ocb():EVP_aes_256_ocb()):
      (cipher==7?EVP_aes_128_gcm():cipher==8?EVP_aes_192_gcm():EVP_aes_256_gcm());
  Bytes out{6,static_cast<unsigned char>(23+nonce.size()),static_cast<unsigned char>(cipher),static_cast<unsigned char>(aead),20,4};
  out.insert(out.end(),salt.begin(),salt.end());out.insert(out.end(),{2,3,6});out.insert(out.end(),nonce.begin(),nonce.end());
  const auto start=out.size();out.resize(start+key.size()+16);
  auto* ctx=EVP_CIPHER_CTX_new();Check(ctx!=nullptr,"reference Argon SKESK context");
  int n=0,final=0;
  const bool ok=EVP_EncryptInit_ex(ctx,selected,nullptr,nullptr,nullptr)==1 &&
      EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_AEAD_SET_IVLEN,nonce.size(),nullptr)==1 &&
      EVP_EncryptInit_ex(ctx,nullptr,nullptr,kek.data(),nonce.data())==1 &&
      EVP_EncryptUpdate(ctx,nullptr,&n,info.data(),4)==1 &&
      EVP_EncryptUpdate(ctx,out.data()+start,&n,key.data(),key.size())==1 &&
      EVP_EncryptFinal_ex(ctx,out.data()+start+n,&final)==1 && std::size_t(n+final)==key.size() &&
      EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_AEAD_GET_TAG,16,out.data()+start+key.size())==1;
  EVP_CIPHER_CTX_free(ctx);Check(ok,"reference Argon SKESK encryption");return out;
}
struct ArgonWorkspace {
  // uint64_t storage enforces the primitive's alignment on every platform.
  std::vector<std::uint64_t> words;
  explicit ArgonWorkspace(c::Argon2S2kProfile p) {
    const auto size=c::Argon2S2kWorkspaceSize(p);Check(size.code==Code::ok,"Argon workspace size");
    words.resize(size.bytes/8+2,UINT64_MAX);
  }
  c::PgpOutput out(){return {reinterpret_cast<std::uint8_t*>(words.data()+1),(words.size()-2)*8};}
  void dirty(){std::fill(words.begin(),words.end(),UINT64_MAX);}
  bool erased()const{return words.front()==UINT64_MAX&&words.back()==UINT64_MAX&&
      std::all_of(words.begin()+1,words.end()-1,[](auto v){return v==0;});}
  bool untouched()const{return std::all_of(words.begin(),words.end(),[](auto v){return v==UINT64_MAX;});}
};
void ArgonSessions() {
  const c::Argon2S2kProfile profile{2,3,6};
  const c::Argon2S2kProfile defaults;
  Check(defaults.passes==3&&defaults.lanes==4&&defaults.memory_exponent==16&&
      c::Argon2S2kWorkspaceSize(defaults).bytes==64*1024*1024,"approved native defaults");
  Bytes password(8),salt(16);for(unsigned i=0;i<8;++i)password[i]=19*i;
  for(unsigned i=0;i<16;++i)salt[i]=i;
  for(unsigned cipher=7;cipher<=9;++cipher)for(unsigned aead:{2u,3u})for(unsigned key_size:{16u,24u,32u}) {
    const Bytes nonce(aead==2?15:12,0x73),key(key_size,0x61);
    const auto expected=ReferenceArgonSession(cipher,aead,salt,nonce,key);
    const auto size=c::Argon2SkeskV6Size(cipher,aead,key_size);
    Check(size.code==Code::ok&&size.bytes==expected.size(),"Argon packet size");
    ArgonWorkspace work(profile);Bytes body(size.bytes,0xa5);
    Check(c::EncryptArgon2SkeskV6(cipher,aead,profile,In(password),In(salt),In(nonce),In(key),work.out(),Out(body))==Code::ok&&
      body==expected&&work.erased(),"independent Argon SKESK ciphertext and erased rounded workspace");
    const auto description=c::InspectArgon2SkeskV6(In(expected));
    Check(description.code==Code::ok&&description.cipher==cipher&&description.aead==aead&&
      description.s2k.passes==2&&description.s2k.lanes==3&&description.s2k.memory_exponent==6&&
      description.workspace_bytes==60*1024&&description.session_key_bytes==key_size,"untrusted work inspection");
    Bytes output(key_size,0xa5);work.dirty();
    Check(c::DecryptArgon2SkeskV6(In(password),In(expected),work.out(),Out(output))==Code::ok&&output==key&&work.erased(),"independent Argon SKESK receive");
    for(unsigned at=6;at<body.size();++at) {
      auto bad=body;bad[at]^=1;
      const auto info=c::InspectArgon2SkeskV6(In(bad));
      Check(info.code==Code::ok,"mutated Argon profile structurally valid");
      ArgonWorkspace altered(info.s2k);output.assign(key_size,0xa5);
      Check(c::DecryptArgon2SkeskV6(In(password),In(bad),altered.out(),Out(output))==Code::authentication_failed&&
        Zero(output)&&altered.erased(),"Argon salt/work/nonce/ciphertext/tag authenticated");
    }
    auto wrong=password;wrong.back()^=1;work.dirty();output.assign(key_size,0xa5);
    Check(c::DecryptArgon2SkeskV6(In(wrong),In(body),work.out(),Out(output))==Code::authentication_failed&&Zero(output)&&work.erased(),"Argon wrong password");
    for(bool decrypt:{false,true}) {
      output.assign(decrypt?key_size:body.size(),0xa5);
      const auto run=[&](c::PgpCancellation probe={}) {work.dirty();return decrypt?
        c::DecryptArgon2SkeskV6(In(password),In(body),work.out(),Out(output),probe):
        c::EncryptArgon2SkeskV6(cipher,aead,profile,In(password),In(salt),In(nonce),In(key),work.out(),Out(output),probe);};
      Probe baseline;Check(run(baseline.callback())==Code::ok&&baseline.calls>10&&work.erased(),"Argon SKESK probe baseline");
      for(bool throwing:{false,true})for(unsigned at=1;at<=baseline.calls;++at) {
        Probe probe{0,at,throwing};std::fill(output.begin(),output.end(),0xa5);bool threw=false;
        try{Check(run(probe.callback())==Code::cancelled,"Argon SKESK cancellation");}catch(const ProbeException&){threw=true;}
        Check(threw==throwing&&probe.calls==at&&Zero(output)&&work.erased(),"all Argon SKESK cancellation/throw points erase work/output");
      }
#ifdef SB_PGP_PROVIDER_FAULTS
      for(unsigned kind=1;kind<=7;++kind) {
        failure_kind=kind;failure_at=0;provider_calls=0;const auto initial=run();const auto count=provider_calls;failure_kind=0;
        Check(initial==Code::ok&&count>0&&work.erased(),"Argon provider baseline");
        for(unsigned at=1;at<=count;++at) {
          std::fill(output.begin(),output.end(),0xa5);failure_kind=kind;failure_at=at;provider_calls=0;
          const auto result=run();const auto calls=provider_calls;failure_kind=0;
          Check(result!=Code::ok&&calls==at&&Zero(output)&&work.erased(),"Argon provider failure after KDF erases work/output");
          Check(run()==Code::ok&&work.erased(),"Argon independent retry");
        }
      }
#endif
    }
  }
  // Structural rejection must occur before dereferencing or clearing buffers.
  ArgonWorkspace work(profile);const Bytes nonce(15,0x73),key(32,0x61);
  const auto body=ReferenceArgonSession(9,2,salt,nonce,key);Bytes output(body.size(),0xa5);const auto saved=output;
  const auto run=[&](c::PgpInput pass,c::PgpInput s,c::PgpInput n,c::PgpInput k,c::PgpOutput w,c::PgpOutput o){
    Probe probe;const auto result=c::EncryptArgon2SkeskV6(9,2,profile,pass,s,n,k,w,o,probe.callback());
    Check(result==Code::invalid_extent&&probe.calls==0&&work.untouched()&&output==saved,"Argon extent rejection has no effects");};
  run({nullptr,1},In(salt),In(nonce),In(key),work.out(),Out(output));
  run(In(password),{salt.data(),15},In(nonce),In(key),work.out(),Out(output));
  run(In(password),In(salt),{nonce.data(),14},In(key),work.out(),Out(output));
  run(In(password),In(salt),In(nonce),In(key),{work.out().data,work.out().size-8},Out(output));
  run(In(password),In(salt),In(nonce),In(key),{work.out().data+1,work.out().size},Out(output));
  run(In(password),In(salt),In(nonce),In(key),work.out(),{output.data(),output.size()-1});
  for(unsigned which=0;which<4;++which)for(bool in_workspace:{false,true}) {
    auto pass=In(password),s=In(salt),n=In(nonce),k=In(key);
    auto* alias=in_workspace?work.out().data:output.data();
    if(which==0)pass.data=alias;if(which==1)s.data=alias;if(which==2)n.data=alias;if(which==3)k.data=alias;
    run(pass,s,n,k,work.out(),Out(output));
  }
  run(In(password),In(salt),In(nonce),In(key),work.out(),{work.out().data,output.size()});
  for(c::Argon2S2kProfile invalid:{c::Argon2S2kProfile{0,3,6},{2,0,6},{2,3,2},{2,3,32},{2,255,3}})
    Check(c::Argon2S2kWorkspaceSize(invalid).code==Code::invalid_profile,"invalid Argon profile no fallback");
  for(unsigned field:{0u,1u,2u,3u,4u,5u,22u,23u,24u}) {
    auto bad=body;bad[field]=0;Probe probe;
    Check(c::DecryptArgon2SkeskV6(In(password),In(bad),work.out(),{output.data(),key.size()},probe.callback())!=Code::ok&&
      probe.calls==0&&work.untouched()&&output==saved,"invalid Argon packet before work");
  }
  for(std::size_t n=0;n<body.size();++n) {
    const auto info=c::InspectArgon2SkeskV6({body.data(),n});if(info.code!=Code::ok)continue;
    Bytes partial(info.session_key_bytes,0xa5);work.dirty();
    Check(c::DecryptArgon2SkeskV6(In(password),{body.data(),n},work.out(),Out(partial))==Code::authentication_failed&&
      Zero(partial)&&work.erased(),"truncated Argon packet never authenticates");
  }
  // Empty passwords are byte strings, not an implicit rejection/substitution.
  Bytes recovered(key.size(),0xa5);work.dirty();
  Check(c::EncryptArgon2SkeskV6(9,2,profile,{},In(salt),In(nonce),In(key),work.out(),Out(output))==Code::ok&&work.erased(),"empty Argon password encrypt");
  Check(c::DecryptArgon2SkeskV6({},In(output),work.out(),Out(recovered))==Code::ok&&recovered==key&&work.erased(),"empty Argon password receive");
}
int main() try {
  RfcVectors(); Boundaries(); Cancellation(); InvalidExtents(); ProviderFailures(); ReceiveShapeAndChunkIntegrity();
  SessionRfcVectors();SessionBoundariesAndFailures();SessionExtentsAndKeySizes();
  ArgonSessions();
  std::cout << checks << " authenticated OpenPGP container checks passed\n";
  return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "openpgp_x25519.hpp"
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
constexpr std::size_t kPublicBody=42, kHeader=69;
bool Extent(PgpInput in) noexcept {
  const auto at=reinterpret_cast<std::uintptr_t>(in.data);
  return (!in.size||in.data)&&in.size<=std::numeric_limits<std::uintptr_t>::max()-at;
}
bool Overlaps(PgpInput a,PgpInput b) noexcept {
  if(!a.size||!b.size)return false;
  const auto x=reinterpret_cast<std::uintptr_t>(a.data),y=reinterpret_cast<std::uintptr_t>(b.data);
  return x<y+b.size&&y<x+a.size;
}
bool Buffers(PgpOutput out,std::initializer_list<PgpInput> inputs) noexcept {
  const PgpInput destination{out.data,out.size};if(!Extent(destination))return false;
  for(const auto in:inputs)if(!Extent(in)||Overlaps(destination,in))return false;
  return true;
}
bool PublicBody(PgpInput in) noexcept {
  return in.size==kPublicBody&&in.data[0]==6&&in.data[5]==25&&
      in.data[6]==0&&in.data[7]==0&&in.data[8]==0&&in.data[9]==32;
}
bool Cancelled(PgpCancellation probe) {return probe.requested&&probe.requested(probe.context);}
struct OutputOwner {
  PgpOutput output;bool accepted=false;
  ~OutputOwner(){if(!accepted&&output.size)OPENSSL_cleanse(output.data,output.size);}
};
template<std::size_t N>struct Secret {
  std::array<unsigned char,N> bytes{};
  ~Secret(){OPENSSL_cleanse(bytes.data(),bytes.size());}
};
using Key=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;
using KeyContext=std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)>;
PgpCode Fingerprint(PgpInput body,std::uint8_t* out,PgpCancellation probe) {
  if(Cancelled(probe))return PgpCode::cancelled;
  const std::array<unsigned char,5> prefix{0x9b,0,0,0,42};
  std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
  unsigned length=0;
  if(!ctx||EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr)!=1||
      EVP_DigestUpdate(ctx.get(),prefix.data(),prefix.size())!=1||
      EVP_DigestUpdate(ctx.get(),body.data,body.size)!=1||
      EVP_DigestFinal_ex(ctx.get(),out,&length)!=1||length!=32)return PgpCode::provider_failure;
  return PgpCode::ok;
}
PgpCode PublicFromSecret(EVP_PKEY* key,std::uint8_t* output) {
  std::size_t size=32;
  return EVP_PKEY_get_raw_public_key(key,output,&size)==1&&size==32?PgpCode::ok:PgpCode::provider_failure;
}
PgpCode SharedKey(EVP_PKEY* secret,PgpInput peer,std::uint8_t* output,PgpCancellation probe) {
  if(Cancelled(probe))return PgpCode::cancelled;
  Key public_key(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519,nullptr,peer.data,peer.size),EVP_PKEY_free);
  KeyContext ctx(EVP_PKEY_CTX_new(secret,nullptr),EVP_PKEY_CTX_free);
  if(!public_key||!ctx||EVP_PKEY_derive_init(ctx.get())<=0||
      EVP_PKEY_derive_set_peer(ctx.get(),public_key.get())<=0)return PgpCode::provider_failure;
  std::size_t size=32;
  // EVP rejects low-order peers during derivation. Like a failed unwrap, a
  // failed cryptographic peer check does not expose partial secret material.
  if(EVP_PKEY_derive(ctx.get(),output,&size)<=0)return PgpCode::authentication_failed;
  if(size!=32)return PgpCode::provider_failure;
  const std::array<unsigned char,32> zero{};
  if(CRYPTO_memcmp(output,zero.data(),zero.size())==0)return PgpCode::authentication_failed;
  return PgpCode::ok;
}
PgpCode DeriveKek(PgpInput ephemeral,PgpInput recipient,PgpInput shared,
    std::uint8_t* output,PgpCancellation probe) {
  if(Cancelled(probe))return PgpCode::cancelled;
  Secret<96> input;
  std::memcpy(input.bytes.data(),ephemeral.data,32);
  std::memcpy(input.bytes.data()+32,recipient.data,32);
  std::memcpy(input.bytes.data()+64,shared.data,32);
  constexpr unsigned char info[]{'O','p','e','n','P','G','P',' ','X','2','5','5','1','9'};
  KeyContext ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF,nullptr),EVP_PKEY_CTX_free);
  if(!ctx||EVP_PKEY_derive_init(ctx.get())<=0||
      EVP_PKEY_CTX_set_hkdf_md(ctx.get(),EVP_sha256())<=0||
      EVP_PKEY_CTX_set1_hkdf_salt(ctx.get(),nullptr,0)<=0||
      EVP_PKEY_CTX_set1_hkdf_key(ctx.get(),input.bytes.data(),input.bytes.size())<=0||
      EVP_PKEY_CTX_add1_hkdf_info(ctx.get(),info,sizeof(info))<=0)return PgpCode::provider_failure;
  std::size_t size=16;
  return EVP_PKEY_derive(ctx.get(),output,&size)>0&&size==16?PgpCode::ok:PgpCode::provider_failure;
}
PgpCode Wrap(bool encrypt,PgpInput kek,PgpInput input,PgpOutput out,PgpCancellation probe) {
  if(Cancelled(probe))return PgpCode::cancelled;
  std::unique_ptr<EVP_CIPHER_CTX,decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);
  if(!ctx)return PgpCode::provider_failure;
  EVP_CIPHER_CTX_set_flags(ctx.get(),EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
  if(EVP_CipherInit_ex(ctx.get(),EVP_aes_128_wrap(),nullptr,kek.data,nullptr,encrypt?1:0)!=1)
    return PgpCode::provider_failure;
  Secret<48> staging;Secret<16> final;int written=0,tail=0;
  if(EVP_CipherUpdate(ctx.get(),staging.bytes.data(),&written,input.data,static_cast<int>(input.size))!=1)
    return encrypt?PgpCode::provider_failure:PgpCode::authentication_failed;
  if(written<0||static_cast<std::size_t>(written)!=out.size)return PgpCode::provider_failure;
  if(Cancelled(probe))return PgpCode::cancelled;
  if(EVP_CipherFinal_ex(ctx.get(),final.bytes.data(),&tail)!=1)
    return encrypt?PgpCode::provider_failure:PgpCode::authentication_failed;
  if(tail!=0)return PgpCode::provider_failure;
  std::memcpy(out.data,staging.bytes.data(),out.size);
  return PgpCode::ok;
}
PgpCode Process(bool encrypt,PgpInput public_body,PgpInput secret,PgpInput input,
    PgpOutput output,PgpCancellation probe) {
  OutputOwner pending{output};
  if(Cancelled(probe))return PgpCode::cancelled;
  {
    // Every EVP-owned private copy and derived scratch is gone before the
    // final publication probe. Borrowed caller secret/key bytes are unchanged.
    Secret<32> fingerprint,own_public,shared;Secret<16> kek;
    auto code=Fingerprint(public_body,fingerprint.bytes.data(),probe);if(code!=PgpCode::ok)return code;
    if(!encrypt&&CRYPTO_memcmp(fingerprint.bytes.data(),input.data+3,32)!=0)
      return PgpCode::authentication_failed;
    Key key(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519,nullptr,secret.data,secret.size),EVP_PKEY_free);
    if(!key)return PgpCode::provider_failure;
    if(Cancelled(probe))return PgpCode::cancelled;
    code=PublicFromSecret(key.get(),own_public.bytes.data());if(code!=PgpCode::ok)return code;
    if(!encrypt&&CRYPTO_memcmp(own_public.bytes.data(),public_body.data+10,32)!=0)
      return PgpCode::authentication_failed;
    const PgpInput ephemeral=encrypt?PgpInput{own_public.bytes.data(),32}:PgpInput{input.data+36,32};
    const PgpInput recipient{public_body.data+10,32};
    code=SharedKey(key.get(),encrypt?recipient:ephemeral,shared.bytes.data(),probe);if(code!=PgpCode::ok)return code;
    code=DeriveKek(ephemeral,recipient,{shared.bytes.data(),32},kek.bytes.data(),probe);if(code!=PgpCode::ok)return code;
    if(encrypt){
      const std::array<unsigned char,3> prefix{6,33,6};std::memcpy(output.data,prefix.data(),prefix.size());
      std::memcpy(output.data+3,fingerprint.bytes.data(),32);output.data[35]=25;
      std::memcpy(output.data+36,own_public.bytes.data(),32);output.data[68]=static_cast<unsigned char>(input.size+8);
    }
    code=Wrap(encrypt,{kek.bytes.data(),16},encrypt?input:PgpInput{input.data+kHeader,input.size-kHeader},
        encrypt?PgpOutput{output.data+kHeader,output.size-kHeader}:output,probe);
    if(code!=PgpCode::ok)return code;
  }
  if(Cancelled(probe))return PgpCode::cancelled;
  pending.accepted=true;return PgpCode::ok;
}
}  // namespace
PgpSize X25519PkeskV6Size(std::size_t bytes) noexcept {
  if(bytes!=16&&bytes!=24&&bytes!=32)return {PgpCode::invalid_extent,0};
  return {PgpCode::ok,kHeader+bytes+8};
}
X25519PkeskDescription InspectX25519PkeskV6(PgpInput body) noexcept {
  X25519PkeskDescription out;
  if(!Extent(body)){out.code=PgpCode::invalid_extent;return out;}
  if(body.size<kHeader||body.data[0]!=6||body.data[1]!=33||body.data[2]!=6)return out;
  if(body.data[35]!=25){out.code=PgpCode::invalid_profile;return out;}
  const auto wrapped=body.size-kHeader;
  if((wrapped!=24&&wrapped!=32&&wrapped!=40)||body.data[68]!=wrapped)return out;
  out.code=PgpCode::ok;out.session_key_bytes=wrapped-8;
  std::copy_n(body.data+3,32,out.recipient_fingerprint.begin());
  std::copy_n(body.data+36,32,out.ephemeral_public_key.begin());return out;
}
PgpCode X25519PublicKeyFingerprintV6(PgpInput public_body,PgpOutput fingerprint,PgpCancellation probe) {
  if(!Buffers(fingerprint,{public_body})||fingerprint.size!=32)return PgpCode::invalid_extent;
  if(!PublicBody(public_body))return PgpCode::invalid_packet;
  OutputOwner pending{fingerprint};
  const auto code=Fingerprint(public_body,fingerprint.data,probe);if(code!=PgpCode::ok)return code;
  if(Cancelled(probe))return PgpCode::cancelled;
  pending.accepted=true;return PgpCode::ok;
}
PgpCode EncryptX25519PkeskV6(PgpInput public_body,PgpInput secret,PgpInput session_key,
    PgpOutput body,PgpCancellation probe) {
  const auto size=X25519PkeskV6Size(session_key.size);if(size.code!=PgpCode::ok)return size.code;
  if(!Buffers(body,{public_body,secret,session_key})||secret.size!=32||body.size!=size.bytes)return PgpCode::invalid_extent;
  if(!PublicBody(public_body))return PgpCode::invalid_packet;
  return Process(true,public_body,secret,session_key,body,probe);
}
PgpCode DecryptX25519PkeskV6(PgpInput public_body,PgpInput secret,PgpInput body,
    PgpOutput session_key,PgpCancellation probe) {
  if(!Buffers(session_key,{public_body,secret,body})||secret.size!=32)return PgpCode::invalid_extent;
  if(!PublicBody(public_body))return PgpCode::invalid_packet;
  const auto description=InspectX25519PkeskV6(body);if(description.code!=PgpCode::ok)return description.code;
  if(session_key.size!=description.session_key_bytes)return PgpCode::invalid_extent;
  return Process(false,public_body,secret,body,session_key,probe);
}
}  // namespace scratchbird::core::crypto

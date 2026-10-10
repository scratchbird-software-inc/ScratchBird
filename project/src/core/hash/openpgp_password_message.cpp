// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "openpgp_password_message.hpp"
#include <openssl/crypto.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace scratchbird::core::crypto {
namespace {
bool Extent(PgpInput in) noexcept {
  const auto at = reinterpret_cast<std::uintptr_t>(in.data);
  return (!in.size || in.data) && in.size <= std::numeric_limits<std::uintptr_t>::max() - at;
}
PgpInput In(PgpOutput out) noexcept { return {out.data,out.size}; }
bool Overlap(PgpInput a, PgpInput b) noexcept {
  if (!a.size || !b.size) return false;
  const auto x = reinterpret_cast<std::uintptr_t>(a.data), y = reinterpret_cast<std::uintptr_t>(b.data);
  return x < y+b.size && y < x+a.size;
}
bool Buffers(PgpOutput work, PgpOutput scratch, PgpOutput output,
             std::initializer_list<PgpInput> inputs) noexcept {
  const std::array<PgpInput,3> writes{In(work),In(scratch),In(output)};
  if (reinterpret_cast<std::uintptr_t>(work.data)%alignof(std::uint64_t)) return false;
  for (auto w:writes) if (!Extent(w)) return false;
  for (std::size_t i=0;i<writes.size();++i) for (std::size_t j=0;j<i;++j)
    if (Overlap(writes[i],writes[j])) return false;
  for (auto in:inputs) {
    if (!Extent(in)) return false;
    for (auto w:writes) if (Overlap(in,w)) return false;
  }
  return true;
}
struct Erase {
  PgpOutput output;
  bool retained = false;
  ~Erase() { if (!retained && output.size) OPENSSL_cleanse(output.data,output.size); }
};
struct SessionKey {
  std::array<std::uint8_t,32> bytes{};
  ~SessionKey() { OPENSSL_cleanse(bytes.data(),bytes.size()); }
};
bool Cancelled(PgpCancellation probe) { return probe.requested && probe.requested(probe.context); }
struct Parsed {
  PasswordMessageDescription description{};
  PgpInput key_body{},data_packet{};
};
Parsed Parse(PgpInput message,PgpCancellation probe) {
  const auto key = InspectPacket(message,probe);
  if (key.code != PgpCode::ok) return {{key.code}};
  if (key.type != 3 || key.partial || key.packet_bytes == message.size) return {};
  const PgpInput key_body{message.data+key.first_body_offset,key.body_bytes};
  const auto password = InspectArgon2SkeskV6(key_body);
  if (password.code != PgpCode::ok) return {{password.code}};
  const PgpInput data_packet{message.data+key.packet_bytes,message.size-key.packet_bytes};
  const auto data = InspectPacket(data_packet,probe);
  if (data.code != PgpCode::ok) return {{data.code}};
  if (data.type != 18 || data.packet_bytes != data_packet.size || data.body_bytes < 68) return {};
  // A partial first segment is at least512 bytes, so the entire fixed SEIPD
  // header is contiguous in either framing. Structural checks only, not auth.
  const auto* header = data_packet.data+data.first_body_offset;
  if (header[0]!=2 || header[1]<7 || header[1]>9 ||
      (header[2]!=2 && header[2]!=3) || header[3]>16 ||
      password.session_key_bytes != std::size_t(16+(header[1]-7)*8)) return {};
  if (password.workspace_bytes > std::numeric_limits<std::size_t>::max())
    return {{PgpCode::size_overflow}};
  return {{PgpCode::ok,password,static_cast<std::size_t>(password.workspace_bytes),data.body_bytes},key_body,data_packet};
}
}  // namespace
PasswordMessageSize PasswordMessageEncodedSize(PasswordMessageProfile p,std::size_t bytes) noexcept {
  const auto literal=LiteralPacketEncodedSize(bytes);
  if(literal.code!=PgpCode::ok)return {literal.code};
  const auto encrypted=SeipdEncryptedSize(p.data,literal.bytes);
  if(encrypted.code!=PgpCode::ok)return {encrypted.code};
  const auto key=Argon2SkeskV6Size(p.wrapping_cipher,p.wrapping_aead,16+(p.data.cipher-7)*8);
  if(key.code!=PgpCode::ok)return {key.code};
  const auto work=Argon2S2kWorkspaceSize(p.s2k);
  if(work.code!=PgpCode::ok)return {work.code};
  const auto a=PacketEncodedSize(3,key.bytes),b=PacketEncodedSize(18,encrypted.bytes);
  if(a.code!=PgpCode::ok)return {a.code};
  if(b.code!=PgpCode::ok)return {b.code};
  if(a.bytes>std::numeric_limits<std::size_t>::max()-b.bytes)return {PgpCode::size_overflow};
  return {PgpCode::ok,a.bytes+b.bytes,literal.bytes,work.bytes};
}
PgpCode EncryptPasswordMessage(PasswordMessageProfile p,PgpInput password,PgpInput data,
    PasswordMessageEntropy entropy,PgpOutput work,PgpOutput literal,PgpOutput message,PgpCancellation probe) {
  const auto size=PasswordMessageEncodedSize(p,data.size);
  if(size.code!=PgpCode::ok)return size.code;
  const auto key_size=std::size_t(16+(p.data.cipher-7)*8);
  if(password.size>std::numeric_limits<std::uint32_t>::max() ||
      size.message_bytes!=message.size || size.literal_bytes!=literal.size || size.workspace_bytes!=work.size ||
      entropy.session_key.size!=key_size || entropy.password_salt.size!=16 || entropy.data_salt.size!=32 ||
      entropy.wrapping_nonce.size!=std::size_t(p.wrapping_aead==2?15:12) ||
      !Buffers(work,literal,message,{password,data,entropy.session_key,entropy.password_salt,entropy.wrapping_nonce,entropy.data_salt}))
    return PgpCode::invalid_extent;
  Erase pending{message};
  {
    Erase workspace{work},plaintext{literal};
    if(Cancelled(probe))return PgpCode::cancelled;
    auto code=EncodeLiteralPacket(data,literal,probe);
    if(code!=PgpCode::ok)return code;
    const auto key=Argon2SkeskV6Size(p.wrapping_cipher,p.wrapping_aead,key_size);
    const auto prefix=PacketPrefix(3,key.bytes);
    std::memcpy(message.data,prefix.bytes.data(),prefix.size);
    code=EncryptArgon2SkeskV6(p.wrapping_cipher,p.wrapping_aead,p.s2k,password,
        entropy.password_salt,entropy.wrapping_nonce,entropy.session_key,work,
        {message.data+prefix.size,key.bytes},probe);
    if(code!=PgpCode::ok)return code;
    const auto encrypted=SeipdEncryptedSize(p.data,literal.size);
    const auto second=PacketPrefix(18,encrypted.bytes);
    auto* next=message.data+prefix.size+key.bytes;
    std::memcpy(next,second.bytes.data(),second.size);
    code=EncryptSeipdV2(p.data,entropy.session_key,entropy.data_salt,In(literal),
        {next+second.size,encrypted.bytes},probe);
    if(code!=PgpCode::ok)return code;
  } // Erase plaintext/workspace before the final publication fence.
  if(Cancelled(probe))return PgpCode::cancelled;
  pending.retained=true;return PgpCode::ok;
}
PasswordMessageDescription InspectPasswordMessage(PgpInput message,PgpCancellation probe) {
  return Parse(message,probe).description;
}
PasswordMessagePlaintext DecryptPasswordMessage(PgpInput password,PgpInput message,
    PgpOutput work,PgpOutput body,PgpOutput plain,PgpCancellation probe) {
  if(password.size>std::numeric_limits<std::uint32_t>::max() ||
      !Buffers(work,body,plain,{password,message}))return {PgpCode::invalid_extent};
  const auto parsed=Parse(message,probe);
  if(parsed.description.code!=PgpCode::ok)return {parsed.description.code};
  if(work.size!=parsed.description.workspace_bytes || body.size!=parsed.description.data_body_bytes ||
      plain.size!=parsed.description.data_body_bytes)return {PgpCode::invalid_extent};
  Erase output{plain};
  PasswordMessagePlaintext result;
  {
    Erase workspace{work},ciphertext{body};
    SessionKey key;
    if(Cancelled(probe))return {PgpCode::cancelled};
    auto code=CopyPacketBody(parsed.data_packet,body,probe);
    if(code!=PgpCode::ok)return {code};
    const auto size=SeipdPlaintextSize(In(body));
    if(size.code!=PgpCode::ok)return {size.code};
    if(size.bytes>plain.size)return {PgpCode::invalid_packet};
    code=DecryptArgon2SkeskV6(password,parsed.key_body,work,
        {key.bytes.data(),parsed.description.password.session_key_bytes},probe);
    if(code!=PgpCode::ok)return {code};
    code=DecryptSeipdV2({key.bytes.data(),parsed.description.password.session_key_bytes},
        In(body),{plain.data,size.bytes},probe);
    if(code!=PgpCode::ok)return {code};
    // Authenticate ALL inner bytes before interpreting any literal or metadata.
    const PgpInput inner{plain.data,size.bytes};
    const auto packet=InspectPacket(inner,probe);
    if(packet.code!=PgpCode::ok)return {packet.code};
    if(packet.type!=11 || packet.packet_bytes!=inner.size)return {PgpCode::invalid_packet};
    // Reuse now-unneeded ciphertext staging for partial literal reassembly.
    code=CopyPacketBody(inner,{body.data,packet.body_bytes},probe);
    if(code!=PgpCode::ok)return {code};
    const auto literal=InspectLiteralBody({body.data,packet.body_bytes});
    if(literal.code!=PgpCode::ok)return {literal.code};
    // Return only data bytes; clear metadata and unused capacity. No heap copy.
    for(std::size_t at=0;at<literal.data.size;) {
      if(Cancelled(probe))return {PgpCode::cancelled};
      const auto n=std::min(std::size_t{4096},literal.data.size-at);
      std::memcpy(plain.data+at,literal.data.data+at,n);at+=n;
    }
    OPENSSL_cleanse(plain.data+literal.data.size,plain.size-literal.data.size);
    result={PgpCode::ok,{plain.data,literal.data.size},literal.format};
  }
  if(Cancelled(probe))return {PgpCode::cancelled};
  output.retained=true;return result;
}
}  // namespace scratchbird::core::crypto

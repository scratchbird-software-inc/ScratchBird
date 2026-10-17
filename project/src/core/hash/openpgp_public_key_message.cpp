// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "openpgp_public_key_message.hpp"
#include <openssl/crypto.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace scratchbird::core::crypto {
namespace {
bool Extent(PgpInput input) noexcept {
  const auto at=reinterpret_cast<std::uintptr_t>(input.data);
  return (!input.size||input.data)&&input.size<=std::numeric_limits<std::uintptr_t>::max()-at;
}
PgpInput In(PgpOutput out) noexcept{return {out.data,out.size};}
bool Overlap(PgpInput a,PgpInput b) noexcept {
  if(!a.size||!b.size)return false;
  const auto x=reinterpret_cast<std::uintptr_t>(a.data),y=reinterpret_cast<std::uintptr_t>(b.data);
  return x<y+b.size&&y<x+a.size;
}
bool Buffers(PgpOutput first,PgpOutput second,std::initializer_list<PgpInput> inputs) noexcept {
  if(!Extent(In(first))||!Extent(In(second))||Overlap(In(first),In(second)))return false;
  for(const auto input:inputs)if(!Extent(input)||Overlap(input,In(first))||Overlap(input,In(second)))return false;
  return true;
}
bool PublicBody(PgpInput input) noexcept {
  return input.size==42&&input.data[0]==6&&input.data[5]==25&&input.data[6]==0&&
      input.data[7]==0&&input.data[8]==0&&input.data[9]==32;
}
struct Erase {
  PgpOutput output;bool retained=false;
  ~Erase(){if(!retained&&output.size)OPENSSL_cleanse(output.data,output.size);}
};
struct SessionKey {
  std::array<unsigned char,32> bytes{};
  ~SessionKey(){OPENSSL_cleanse(bytes.data(),bytes.size());}
};
bool Cancelled(PgpCancellation probe){return probe.requested&&probe.requested(probe.context);}
struct Parsed {X25519LiteralMessageDescription description{};PgpInput key_body{},data_packet{};};
Parsed Parse(PgpInput message,PgpCancellation probe){
  const auto key=InspectPacket(message,probe);if(key.code!=PgpCode::ok)return {{key.code}};
  if(key.type!=1||key.partial||key.packet_bytes==message.size)return {};
  const PgpInput key_body{message.data+key.first_body_offset,key.body_bytes};
  const auto recipient=InspectX25519PkeskV6(key_body);if(recipient.code!=PgpCode::ok)return {{recipient.code}};
  const PgpInput data_packet{message.data+key.packet_bytes,message.size-key.packet_bytes};
  const auto data=InspectPacket(data_packet,probe);if(data.code!=PgpCode::ok)return {{data.code}};
  if(data.type!=18||data.packet_bytes!=data_packet.size||data.body_bytes<68)return {};
  // Admitted partial first segments are at least512 bytes, so the fixed36-byte
  // SEIPD header is contiguous. No cryptographic authenticity is implied.
  const auto* header=data_packet.data+data.first_body_offset;
  if(header[0]!=2||header[1]<7||header[1]>9||(header[2]!=2&&header[2]!=3)||header[3]>16||
      recipient.session_key_bytes!=std::size_t(16+(header[1]-7)*8))return {};
  return {{PgpCode::ok,recipient,data.body_bytes},key_body,data_packet};
}
}  // namespace
X25519LiteralMessageSize X25519LiteralMessageEncodedSize(SeipdProfile profile,std::size_t bytes) noexcept {
  const auto literal=LiteralPacketEncodedSize(bytes);if(literal.code!=PgpCode::ok)return {literal.code};
  const auto encrypted=SeipdEncryptedSize(profile,literal.bytes);if(encrypted.code!=PgpCode::ok)return {encrypted.code};
  const auto key=X25519PkeskV6Size(16+(profile.cipher-7)*8);if(key.code!=PgpCode::ok)return {key.code};
  const auto first=PacketEncodedSize(1,key.bytes),second=PacketEncodedSize(18,encrypted.bytes);
  if(first.code!=PgpCode::ok)return {first.code};if(second.code!=PgpCode::ok)return {second.code};
  if(first.bytes>std::numeric_limits<std::size_t>::max()-second.bytes)return {PgpCode::size_overflow};
  return {PgpCode::ok,first.bytes+second.bytes,literal.bytes};
}
PgpCode EncryptX25519LiteralMessage(SeipdProfile profile,PgpInput public_body,PgpInput data,
    X25519LiteralMessageEntropy entropy,PgpOutput literal,PgpOutput message,PgpCancellation probe){
  const auto size=X25519LiteralMessageEncodedSize(profile,data.size);if(size.code!=PgpCode::ok)return size.code;
  const auto key_size=std::size_t(16+(profile.cipher-7)*8);
  if(literal.size!=size.literal_bytes||message.size!=size.message_bytes||entropy.session_key.size!=key_size||
      entropy.ephemeral_secret.size!=32||entropy.data_salt.size!=32||
      !Buffers(literal,message,{public_body,data,entropy.session_key,entropy.ephemeral_secret,entropy.data_salt}))return PgpCode::invalid_extent;
  if(!PublicBody(public_body))return PgpCode::invalid_packet;
  Erase pending{message};
  {
    Erase scratch{literal};if(Cancelled(probe))return PgpCode::cancelled;
    auto code=EncodeLiteralPacket(data,literal,probe);if(code!=PgpCode::ok)return code;
    const auto key=X25519PkeskV6Size(key_size),encrypted=SeipdEncryptedSize(profile,literal.size);
    const auto first=PacketPrefix(1,key.bytes),second=PacketPrefix(18,encrypted.bytes);
    std::memcpy(message.data,first.bytes.data(),first.size);
    code=EncryptX25519PkeskV6(public_body,entropy.ephemeral_secret,entropy.session_key,
        {message.data+first.size,key.bytes},probe);if(code!=PgpCode::ok)return code;
    auto* next=message.data+first.size+key.bytes;std::memcpy(next,second.bytes.data(),second.size);
    code=EncryptSeipdV2(profile,entropy.session_key,entropy.data_salt,In(literal),
        {next+second.size,encrypted.bytes},probe);if(code!=PgpCode::ok)return code;
  }
  if(Cancelled(probe))return PgpCode::cancelled;
  pending.retained=true;return PgpCode::ok;
}
X25519LiteralMessageDescription InspectX25519LiteralMessage(PgpInput message,PgpCancellation probe){return Parse(message,probe).description;}
X25519LiteralMessagePlaintext DecryptX25519LiteralMessage(PgpInput public_body,PgpInput secret,PgpInput message,
    PgpOutput body,PgpOutput plain,PgpCancellation probe){
  if(secret.size!=32||!Buffers(body,plain,{public_body,secret,message}))return {PgpCode::invalid_extent};
  if(!PublicBody(public_body))return {PgpCode::invalid_packet};
  const auto parsed=Parse(message,probe);if(parsed.description.code!=PgpCode::ok)return {parsed.description.code};
  if(body.size!=parsed.description.data_body_bytes||plain.size!=parsed.description.data_body_bytes)return {PgpCode::invalid_extent};
  Erase output{plain};X25519LiteralMessagePlaintext result;
  {
    Erase ciphertext{body};SessionKey key;if(Cancelled(probe))return {PgpCode::cancelled};
    auto code=CopyPacketBody(parsed.data_packet,body,probe);if(code!=PgpCode::ok)return {code};
    const auto size=SeipdPlaintextSize(In(body));if(size.code!=PgpCode::ok)return {size.code};
    if(size.bytes>plain.size)return {PgpCode::invalid_packet};
    const auto key_size=parsed.description.recipient.session_key_bytes;
    code=DecryptX25519PkeskV6(public_body,secret,parsed.key_body,{key.bytes.data(),key_size},probe);
    if(code!=PgpCode::ok)return {code};
    code=DecryptSeipdV2({key.bytes.data(),key_size},In(body),{plain.data,size.bytes},probe);
    if(code!=PgpCode::ok)return {code};
    // Never inspect literal metadata until all ciphertext and the final SEIPD
    // length authentication have succeeded. The complete inner stream must be
    // exactly one literal packet; higher-level dispatch owns other grammars.
    const PgpInput inner{plain.data,size.bytes};const auto packet=InspectPacket(inner,probe);
    if(packet.code!=PgpCode::ok)return {packet.code};
    if(packet.type!=11||packet.packet_bytes!=inner.size)return {PgpCode::invalid_packet};
    code=CopyPacketBody(inner,{body.data,packet.body_bytes},probe);if(code!=PgpCode::ok)return {code};
    const auto literal=InspectLiteralBody({body.data,packet.body_bytes});if(literal.code!=PgpCode::ok)return {literal.code};
    for(std::size_t at=0;at<literal.data.size;){
      if(Cancelled(probe))return {PgpCode::cancelled};const auto count=std::min(std::size_t{4096},literal.data.size-at);
      std::memcpy(plain.data+at,literal.data.data+at,count);at+=count;
    }
    OPENSSL_cleanse(plain.data+literal.data.size,plain.size-literal.data.size);
    result={PgpCode::ok,{plain.data,literal.data.size},literal.format};
  }
  if(Cancelled(probe))return {PgpCode::cancelled};
  output.retained=true;return result;
}
}  // namespace scratchbird::core::crypto

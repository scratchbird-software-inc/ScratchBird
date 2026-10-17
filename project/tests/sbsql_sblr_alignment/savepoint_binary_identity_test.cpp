// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/mga_savepoint_marker_codec.hpp"
#include "hash_digest.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdio>
#include <cstdlib>
#include <set>

namespace api = scratchbird::engine::internal_api;
using Uuid = scratchbird::core::platform::Uuid;
static void Check(bool condition, const char* message) {
  if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(EXIT_FAILURE); }
}
static bool SameMarker(const api::MgaSavepointMarkerRecord& a,const api::MgaSavepointMarkerRecord& b){
  if(a.kind!=b.kind||a.uuid_identity!=b.uuid_identity||a.uuid!=b.uuid||a.identity!=b.identity||a.transaction!=b.transaction)return false;
  for(unsigned n=0;n<3;++n)if(a.cutoffs[n]!=b.cutoffs[n]||a.upper[n]!=b.upper[n])return false;
  return true;
}
static std::string RecheckFrame(std::string body){
  const auto size=static_cast<std::uint32_t>(body.size()+32);
  for(unsigned n=0;n<4;++n)body[8+n]=static_cast<char>(size>>(8*n));
  const auto digest=scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const std::uint8_t*>(body.data()),body.size());
  Check(digest.ok(),"statement fence independent checksum calculation failed");
  body.append(reinterpret_cast<const char*>(digest.digest.data()),digest.digest.size());return body;
}
static void StatementFence(){
  api::MgaSavepointMarkerRecord record;record.kind=4;record.uuid_identity=true;
  record.uuid=scratchbird::tests::FixtureUuid(6012,3);record.uuid.bytes[9]=0;record.uuid.bytes[15]=0xff;
  record.transaction=UINT64_MAX;
  record.cutoffs[0]=0;record.cutoffs[1]=UINT64_C(0x00ff001122334455);record.cutoffs[2]=UINT64_MAX-1;
  record.upper[0]=1;record.upper[1]=UINT64_C(0x00ff001122334456);record.upper[2]=UINT64_MAX;
  const auto encoded=api::EncodeMgaSavepointMarker(record);
  Check(encoded.size()==124&&api::MgaSavepointMarkerFrameSize(encoded)==124&&
      static_cast<unsigned char>(encoded[12])==4&&static_cast<unsigned char>(encoded[13])==1&&
      encoded[14]==0&&encoded[15]==0,"native statement fence exact frame/header changed");
  Check(encoded.substr(72,4)==std::string("\x10\0\0\0",4)&&encoded.substr(76,16)==std::string(
      reinterpret_cast<const char*>(record.uuid.bytes.data()),16),"statement fence UUID is not exact native binary16");
  const auto number=[&](std::size_t at){std::uint64_t value=0;for(unsigned n=0;n<8;++n)
      value|=std::uint64_t(static_cast<unsigned char>(encoded[at+n]))<<(8*n);return value;};
  Check(number(16)==record.transaction,"statement fence transaction identity changed");
  for(unsigned n=0;n<3;++n)Check(number(24+8*n)==record.cutoffs[n]&&number(48+8*n)==record.upper[n],
      "statement fence lost native cutoff/upper boundary bits");
  api::MgaSavepointMarkerRecord decoded;
  Check(api::DecodeMgaSavepointMarker(encoded,&decoded)&&SameMarker(decoded,record),
      "native statement fence complete roundtrip changed identity/kind/cutoffs");
  auto sentinel=record;sentinel.kind=2;sentinel.uuid_identity=false;sentinel.uuid=scratchbird::tests::FixtureUuid(6012,4);
  sentinel.identity="unchanged-output-sentinel";sentinel.transaction=23;
  for(unsigned n=0;n<3;++n){sentinel.cutoffs[n]=100+n;sentinel.upper[n]=200+n;}
  const auto refused=[&](std::string_view input){auto output=sentinel;
    Check(!api::DecodeMgaSavepointMarker(input,&output)&&SameMarker(output,sentinel),
        "malformed statement fence accepted or modified a caller output field");};
  for(std::size_t n=0;n<encoded.size();++n){refused(std::string_view(encoded).substr(0,n));
    auto altered=encoded;altered[n]^=1;refused(altered);}
  refused(encoded+"trailing");
  for(const std::string label:{"named-statement-label","019f2100-0000-7000-8000-00000000de01"}){
    auto invalid=record;invalid.identity=label;
    Check(api::EncodeMgaSavepointMarker(invalid).empty(),"native statement writer accepted a label/text UUID shadow");
    invalid.uuid_identity=false;invalid.uuid={};
    Check(api::EncodeMgaSavepointMarker(invalid).empty(),"kind4 statement writer accepted named/text UUID identity");
    // Authentic checksum, valid named-identity framing: rejection must follow
    // kind4's binary UUID contract rather than a damaged checksum or NUL label.
    auto body=encoded.substr(0,76);body[13]=0;
    for(unsigned n=0;n<4;++n)body[72+n]=static_cast<char>(label.size()>>(8*n));
    body+=label;refused(RecheckFrame(std::move(body)));
  }
  for(unsigned identity_kind:{2u,255u}){auto body=encoded.substr(0,92);body[13]=static_cast<char>(identity_kind);refused(RecheckFrame(std::move(body)));}
  for(unsigned width:{0u,1u,15u,17u,36u}){auto body=encoded.substr(0,76);
    for(unsigned n=0;n<4;++n)body[72+n]=static_cast<char>(width>>(8*n));
    body.append(width,'x');refused(RecheckFrame(std::move(body)));}
  for(unsigned version=0;version<16;++version){if(version==7)continue;
    auto invalid=record;invalid.uuid.bytes[6]=static_cast<unsigned char>((invalid.uuid.bytes[6]&15)|(version<<4));
    Check(api::EncodeMgaSavepointMarker(invalid).empty(),"kind4 statement writer accepted non-v7 identity including UUIDv4");
    auto body=encoded.substr(0,92);body[82]=static_cast<char>(invalid.uuid.bytes[6]);refused(RecheckFrame(std::move(body)));}
  for(unsigned variant:{0u,0x40u,0xc0u,0xffu}){auto invalid=record;invalid.uuid.bytes[8]=static_cast<unsigned char>(variant);
    Check(api::EncodeMgaSavepointMarker(invalid).empty(),"kind4 statement writer accepted non-RFC UUID variant");
    auto body=encoded.substr(0,92);body[84]=static_cast<char>(variant);refused(RecheckFrame(std::move(body)));}
  auto nil=record;nil.uuid={};Check(api::EncodeMgaSavepointMarker(nil).empty(),"kind4 statement writer accepted nil identity");
  auto body=encoded.substr(0,92);body.replace(76,16,16,'\0');refused(RecheckFrame(std::move(body)));
}

int main() {
  StatementFence();
  api::MgaSavepointMarkerRecord record;
  record.kind = 1;
  record.transaction = 17;
  record.uuid_identity = true;
  const auto original = scratchbird::tests::FixtureUuid(6012, 1);
  std::set<std::string> keys;
  for (unsigned bit = 0; bit <= 128; ++bit) {
    record.uuid = original;
    if (bit < 128) record.uuid.bytes[bit / 8] ^= static_cast<unsigned char>(1u << (bit % 8));
    const auto key = api::MgaSavepointUuidKey(record.uuid);
    Uuid decoded_key;
    Check(key.size() == 17 && key[0] == '\0' && keys.insert(key).second &&
              api::DecodeMgaSavepointUuidKey(key, &decoded_key) && decoded_key == record.uuid,
          "savepoint lookup key lost identity bits");
    const auto bytes = api::EncodeMgaSavepointMarker(record);
    if ((record.uuid.bytes[8] & 0xc0u) != 0x80u) {
      Check(bytes.empty(), "savepoint writer accepted a non-RFC UUID variant");
      continue;
    }
    Check(bytes.size() == 124 && api::MgaSavepointMarkerFrameSize(bytes) == 124 &&
              bytes.substr(76, 16) == std::string(
                  reinterpret_cast<const char*>(record.uuid.bytes.data()), 16),
          "SBSP2 native UUID width or offset changed");
    api::MgaSavepointMarkerRecord decoded;
    Check(api::DecodeMgaSavepointMarker(bytes, &decoded) && decoded.uuid_identity &&
              decoded.uuid == record.uuid && decoded.identity.empty() &&
              decoded.transaction == 17 && decoded.kind == 1,
          "savepoint round trip introduced text identity or changed bytes");
  }
  record.uuid = original;
  const auto bytes = api::EncodeMgaSavepointMarker(record);
  api::MgaSavepointMarkerRecord sentinel;
  sentinel.uuid = scratchbird::tests::FixtureUuid(6012, 2);
  for (std::size_t n = 0; n < bytes.size(); ++n) {
    auto decoded = sentinel;
    Check(!api::DecodeMgaSavepointMarker(std::string_view(bytes).substr(0, n), &decoded) &&
              decoded.uuid == sentinel.uuid, "truncated marker accepted or changed output");
    auto corrupt = bytes;
    corrupt[n] ^= 1;
    Check(!api::DecodeMgaSavepointMarker(corrupt, &decoded) && decoded.uuid == sentinel.uuid,
          "corrupt marker accepted or changed output");
  }
  for (std::uint32_t width : {0u, 15u, 17u, 36u}) {
    auto malformed = bytes.substr(0, 76);
    malformed.append(width, 'x');
    const auto size = static_cast<std::uint32_t>(malformed.size() + 32);
    for (unsigned i = 0; i < 4; ++i) {
      malformed[8 + i] = static_cast<char>(size >> (8 * i));
      malformed[72 + i] = static_cast<char>(width >> (8 * i));
    }
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(
        reinterpret_cast<const std::uint8_t*>(malformed.data()), malformed.size());
    Check(digest.ok(), "checksum calculation failed");
    malformed.append(reinterpret_cast<const char*>(digest.digest.data()), 32);
    auto decoded = sentinel;
    Check(!api::DecodeMgaSavepointMarker(malformed, &decoded) && decoded.uuid == sentinel.uuid,
          "UUID field accepted a non-16-byte width with a valid checksum");
  }
  Check(api::MgaSavepointUuidKey({}).empty(), "nil UUID formed a native key");
  auto decoded_key = original;
  Check(!api::DecodeMgaSavepointUuidKey(std::string(1, '\0') +
            "019f2100-0000-7000-8000-00000000de01", &decoded_key) && decoded_key == original,
        "text UUID shadow accepted as a native key");
  record.identity = "shadow";
  Check(api::EncodeMgaSavepointMarker(record).empty(), "native UUID accepted a label shadow");
  record.uuid_identity = false;
  record.uuid = {};
  record.identity = "019f2100-0000-7000-8000-00000000de01";
  api::MgaSavepointMarkerRecord named;
  Check(api::DecodeMgaSavepointMarker(api::EncodeMgaSavepointMarker(record), &named) &&
            !named.uuid_identity && named.uuid.is_nil() && named.identity == record.identity,
        "UUID-looking SQL savepoint label was reinterpreted as an identity");
}

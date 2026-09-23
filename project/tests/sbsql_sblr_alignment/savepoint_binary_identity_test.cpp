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

int main() {
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

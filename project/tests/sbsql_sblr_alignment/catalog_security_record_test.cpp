// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_security_record_codec.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>

namespace c = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
namespace u = scratchbird::core::uuid;
namespace {
void Check(bool value, const char* message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
p::Uuid Identity(unsigned suffix = 1) {
  p::Uuid id;
  id.bytes = {0, 0x0a, 0x0d, 0x20, 0x3d, 0xff, 0x7f, 0,
              0x80, 0xff, 0, 0x0a, 0x3d, 0, 0, static_cast<p::byte>(suffix)};
  return id;
}
std::string Bytes(const c::CatalogValueEncodeResult& encoded) {
  Check(encoded.ok(), "security encode unexpectedly refused");
  return {encoded.bytes.begin(), encoded.bytes.end()};
}
void Refused(c::CatalogRecordKind kind, const std::string& bytes) {
  const auto result = c::DecodeCatalogSecurityRecord(kind, bytes);
  Check(!result.ok() && !result.record, "malformed security payload published a record");
}
void Put(std::string& bytes, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<char>(value >> (8 * i)));
}
std::string Golden(unsigned kind, unsigned primary, const p::Uuid& id) {
  // Independently assembled from the durable registry: 24-byte SBCV header,
  // primary raw16 identity and required creator_tx text. No production encoder.
  std::string bytes = "SBCV";
  Put(bytes, 1, 2); Put(bytes, 24, 2); Put(bytes, 57, 4); Put(bytes, 2, 4);
  Put(bytes, 65536 + kind, 4); Put(bytes, 1, 2); Put(bytes, 0, 2);
  Put(bytes, primary, 2); Put(bytes, 5, 1); Put(bytes, 0, 1); Put(bytes, 16, 4);
  for (auto byte : id.bytes) bytes.push_back(static_cast<char>(byte));
  Put(bytes, 32, 2); Put(bytes, 3, 1); Put(bytes, 0, 1); Put(bytes, 1, 4);
  bytes.push_back('1');
  return bytes;
}
void CheckFamily(c::CatalogRecordKind kind, unsigned physical_kind, unsigned primary) {
  const auto name = std::string(c::CatalogSecurityPrimaryIdentityName(kind));
  c::CatalogSecurityRecord record{kind, {{name, Identity()}}, {{"creator_tx", "1"}}};
  const auto golden = Golden(physical_kind, primary, Identity());
  Check(Bytes(c::EncodeCatalogSecurityRecord(record)) == golden, "security encoding differs from independent golden");
  const auto decoded = c::DecodeCatalogSecurityRecord(kind, golden);
  Check(decoded.ok() && decoded.record->Identity(name) == Identity() &&
        decoded.record->attributes == record.attributes && decoded.record->identities.size() == 1,
        "golden decoding changed identities or attributes");
  for (std::size_t size = 0; size < golden.size(); ++size) Refused(kind, golden.substr(0, size));
  Refused(kind, golden + '\0');
  Refused(kind, "creator_tx=1\n" + name + "=018f7a10-1280-7000-8000-000000000106\n");
  for (unsigned index = 0; index < 16; ++index) {
    for (unsigned value = 0; value < 256; ++value) {
      auto id = Identity(); id.bytes[index] = value;
      record.identities[name] = id;
      const bool admitted = (id.bytes[6] >> 4) == 7 && (id.bytes[8] & 0xc0) == 0x80;
      const auto encoded = c::EncodeCatalogSecurityRecord(record);
      auto bytes = golden; bytes[32 + index] = static_cast<char>(value);
      if (!admitted) {
        Check(!encoded.ok() && encoded.bytes.empty(), "invalid UUID encoded or partial bytes escaped");
        Refused(kind, bytes);
      } else {
        Check(Bytes(encoded) == bytes, "identity byte was formatted or substituted");
        const auto roundtrip = c::DecodeCatalogSecurityRecord(kind, bytes);
        Check(roundtrip.ok() && roundtrip.record->Identity(name) == id, "native identity roundtrip failed");
      }
    }
  }
  record.identities[name] = {};
  Check(!c::EncodeCatalogSecurityRecord(record).ok(), "nil primary encoded");
  record.identities[name] = Identity();
  record.identities["policy_pack_uuid"] = {};
  Check(!c::EncodeCatalogSecurityRecord(record).ok(), "supplied nil optional identity encoded");
  record.identities.erase("policy_pack_uuid");
  record.attributes[name] = u::UuidToString(Identity());
  Check(!c::EncodeCatalogSecurityRecord(record).ok(), "text shadow identity encoded");
  record.attributes.erase(name);
  record.attributes["description"] = std::string(130001, 'x');
  Check(!c::EncodeCatalogSecurityRecord(record).ok(), "oversize attribute encoded");
  record.attributes["description"] = std::string(70000, 'x');
  record.attributes["role_name"] = std::string(70000, 'y');
  Check(!c::EncodeCatalogSecurityRecord(record).ok(), "oversize aggregate encoded");
  record.attributes = {{"creator_tx", "1"}};
  record.identities.clear();
  Check(!c::EncodeCatalogSecurityRecord(record).ok(), "missing owner encoded");

  c::CatalogTypedRecord outer;
  outer.header.kind = kind;
  outer.header.row_uuid = {p::UuidKind::row, Identity(2)};
  outer.header.object_uuid = {p::UuidKind::object, Identity()};
  outer.header.parent_uuid = {p::UuidKind::object, Identity(3)};
  outer.payload = golden;
  const auto framed = c::EncodeCatalogTypedRecord(outer, 1);
  Check(framed.ok() && c::DecodeCatalogTypedRecord(framed.row).ok(), "valid security header refused");
  outer.header.object_uuid.value = Identity(4);
  Check(!c::EncodeCatalogTypedRecord(outer, 1).ok(), "header and payload mismatch admitted");
  auto corrupt = framed.row;
  corrupt.payload[71] ^= 1;
  Check(!c::DecodeCatalogTypedRecord(corrupt).ok(), "persisted header mismatch admitted");
  outer.header.object_uuid.value = outer.header.row_uuid.value;
  outer.payload = Golden(physical_kind, primary, outer.header.row_uuid.value);
  Check(!c::EncodeCatalogTypedRecord(outer, 1).ok(), "row identity reused as object owner");
}
}  // namespace
int main() {
  const std::array kinds{c::CatalogRecordKind::user_account, c::CatalogRecordKind::group_account,
                        c::CatalogRecordKind::role_account, c::CatalogRecordKind::grant_record};
  for (unsigned i = 0; i < kinds.size(); ++i) {
    CheckFamily(kinds[i], 60 + i, 1 + i);
    for (unsigned j = 0; j < kinds.size(); ++j)
      if (i != j) Refused(kinds[i], Golden(60 + j, 1 + j, Identity()));
  }
  Refused(c::CatalogRecordKind::policy, Golden(60, 1, Identity()));
  std::cout << "catalog_security_binary_identity=passed octet_cases=16384\n";
}

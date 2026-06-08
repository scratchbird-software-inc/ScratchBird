// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <random>
#include <sstream>
#include <utility>
#include <vector>

namespace scratchbird::core::uuid {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr char kHex[] = "0123456789abcdef";
constexpr u64 kUuid60BitMask = 0x0fffffffffffffffull;

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

Status UuidErrorStatus() {
  return {StatusCode::uuid_invalid, Severity::error, Subsystem::uuid};
}

Status UuidOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::uuid};
}

std::string StripUuidDashes(std::string text) {
  std::string stripped;
  stripped.reserve(32);
  for (char c : text) {
    if (c != '-') stripped.push_back(c);
  }
  return stripped;
}

bool HasCanonicalDashPositions(const std::string& text) {
  if (text.size() == 32) return true;
  if (text.size() != 36) return false;
  return text[8] == '-' && text[13] == '-' && text[18] == '-' && text[23] == '-';
}

void StoreBe16(Uuid* uuid, std::size_t offset, u16 value) {
  uuid->bytes[offset] = static_cast<byte>((value >> 8) & 0xffu);
  uuid->bytes[offset + 1] = static_cast<byte>(value & 0xffu);
}

void StoreBe32(Uuid* uuid, std::size_t offset, u32 value) {
  uuid->bytes[offset] = static_cast<byte>((value >> 24) & 0xffu);
  uuid->bytes[offset + 1] = static_cast<byte>((value >> 16) & 0xffu);
  uuid->bytes[offset + 2] = static_cast<byte>((value >> 8) & 0xffu);
  uuid->bytes[offset + 3] = static_cast<byte>(value & 0xffu);
}

void SetVariantAndClockSequence(Uuid* uuid, u16 clock_sequence) {
  clock_sequence = static_cast<u16>(clock_sequence & 0x3fffu);
  uuid->bytes[8] = static_cast<byte>(0x80u | ((clock_sequence >> 8) & 0x3fu));
  uuid->bytes[9] = static_cast<byte>(clock_sequence & 0xffu);
}

void SetVersion(Uuid* uuid, u8 version) {
  uuid->bytes[6] = static_cast<byte>((uuid->bytes[6] & 0x0fu) | ((version & 0x0fu) << 4));
  uuid->bytes[8] = static_cast<byte>((uuid->bytes[8] & 0x3fu) | 0x80u);
}

Uuid MakeV1Layout(u8 version, u64 gregorian_100ns_timestamp, u16 clock_sequence, std::array<byte, 6> node) {
  Uuid uuid;
  const u64 timestamp = gregorian_100ns_timestamp & kUuid60BitMask;
  StoreBe32(&uuid, 0, static_cast<u32>(timestamp & 0xffffffffull));
  StoreBe16(&uuid, 4, static_cast<u16>((timestamp >> 32) & 0xffffull));
  StoreBe16(&uuid, 6, static_cast<u16>(((version & 0x0fu) << 12) | ((timestamp >> 48) & 0x0fffull)));
  SetVariantAndClockSequence(&uuid, clock_sequence);
  for (std::size_t i = 0; i < node.size(); ++i) {
    uuid.bytes[10 + i] = node[i];
  }
  return uuid;
}

std::array<byte, 16> RandomBytes16() {
  std::array<byte, 16> bytes{};
  std::random_device random;
  for (std::size_t i = 0; i < bytes.size(); i += 4) {
    const unsigned int value = random();
    bytes[i] = static_cast<byte>((value >> 24) & 0xffu);
    bytes[i + 1] = static_cast<byte>((value >> 16) & 0xffu);
    bytes[i + 2] = static_cast<byte>((value >> 8) & 0xffu);
    bytes[i + 3] = static_cast<byte>(value & 0xffu);
  }
  return bytes;
}

u32 LeftRotate32(u32 value, u32 count) {
  return static_cast<u32>((value << count) | (value >> (32u - count)));
}

std::array<byte, 16> Md5(const std::vector<byte>& input) {
  static constexpr u32 s[64] = {
      7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
      5,9,14,20, 5,9,14,20, 5,9,14,20, 5,9,14,20,
      4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
      6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21};
  static constexpr u32 k[64] = {
      0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
      0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
      0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
      0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
      0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
      0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
      0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
      0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};

  std::vector<byte> msg = input;
  const u64 bit_length = static_cast<u64>(msg.size()) * 8ull;
  msg.push_back(0x80u);
  while ((msg.size() % 64) != 56) msg.push_back(0);
  for (int i = 0; i < 8; ++i) msg.push_back(static_cast<byte>((bit_length >> (8 * i)) & 0xffu));

  u32 a0 = 0x67452301u;
  u32 b0 = 0xefcdab89u;
  u32 c0 = 0x98badcfeu;
  u32 d0 = 0x10325476u;

  for (std::size_t offset = 0; offset < msg.size(); offset += 64) {
    u32 m[16];
    for (int i = 0; i < 16; ++i) {
      const std::size_t p = offset + i * 4;
      m[i] = static_cast<u32>(msg[p]) |
             (static_cast<u32>(msg[p + 1]) << 8) |
             (static_cast<u32>(msg[p + 2]) << 16) |
             (static_cast<u32>(msg[p + 3]) << 24);
    }

    u32 a = a0;
    u32 b = b0;
    u32 c = c0;
    u32 d = d0;

    for (u32 i = 0; i < 64; ++i) {
      u32 f = 0;
      u32 g = 0;
      if (i < 16) {
        f = (b & c) | ((~b) & d);
        g = i;
      } else if (i < 32) {
        f = (d & b) | ((~d) & c);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
      } else {
        f = c ^ (b | (~d));
        g = (7 * i) % 16;
      }
      const u32 temp = d;
      d = c;
      c = b;
      b = b + LeftRotate32(a + f + k[i] + m[g], s[i]);
      a = temp;
    }

    a0 += a;
    b0 += b;
    c0 += c;
    d0 += d;
  }

  std::array<byte, 16> digest{};
  const u32 words[4] = {a0, b0, c0, d0};
  for (int i = 0; i < 4; ++i) {
    digest[i * 4] = static_cast<byte>(words[i] & 0xffu);
    digest[i * 4 + 1] = static_cast<byte>((words[i] >> 8) & 0xffu);
    digest[i * 4 + 2] = static_cast<byte>((words[i] >> 16) & 0xffu);
    digest[i * 4 + 3] = static_cast<byte>((words[i] >> 24) & 0xffu);
  }
  return digest;
}

std::array<byte, 20> Sha1(const std::vector<byte>& input) {
  std::vector<byte> msg = input;
  const u64 bit_length = static_cast<u64>(msg.size()) * 8ull;
  msg.push_back(0x80u);
  while ((msg.size() % 64) != 56) msg.push_back(0);
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<byte>((bit_length >> (8 * i)) & 0xffu));

  u32 h0 = 0x67452301u;
  u32 h1 = 0xefcdab89u;
  u32 h2 = 0x98badcfeu;
  u32 h3 = 0x10325476u;
  u32 h4 = 0xc3d2e1f0u;

  for (std::size_t offset = 0; offset < msg.size(); offset += 64) {
    u32 w[80];
    for (int i = 0; i < 16; ++i) {
      const std::size_t p = offset + i * 4;
      w[i] = (static_cast<u32>(msg[p]) << 24) |
             (static_cast<u32>(msg[p + 1]) << 16) |
             (static_cast<u32>(msg[p + 2]) << 8) |
             static_cast<u32>(msg[p + 3]);
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = LeftRotate32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    u32 a = h0;
    u32 b = h1;
    u32 c = h2;
    u32 d = h3;
    u32 e = h4;

    for (int i = 0; i < 80; ++i) {
      u32 f = 0;
      u32 k = 0;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5a827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ed9eba1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8f1bbcdcu;
      } else {
        f = b ^ c ^ d;
        k = 0xca62c1d6u;
      }
      const u32 temp = LeftRotate32(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = LeftRotate32(b, 30);
      b = a;
      a = temp;
    }

    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }

  std::array<byte, 20> digest{};
  const u32 words[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; ++i) {
    digest[i * 4] = static_cast<byte>((words[i] >> 24) & 0xffu);
    digest[i * 4 + 1] = static_cast<byte>((words[i] >> 16) & 0xffu);
    digest[i * 4 + 2] = static_cast<byte>((words[i] >> 8) & 0xffu);
    digest[i * 4 + 3] = static_cast<byte>(words[i] & 0xffu);
  }
  return digest;
}

std::vector<byte> NamespaceNameBytes(const Uuid& namespace_uuid, const std::string& name) {
  std::vector<byte> data;
  data.reserve(16 + name.size());
  data.insert(data.end(), namespace_uuid.bytes.begin(), namespace_uuid.bytes.end());
  data.insert(data.end(), name.begin(), name.end());
  return data;
}

TypedUuidResult MakeUuidWithVersion(UuidKind kind, Uuid uuid, u8 version) {
  SetVersion(&uuid, version);
  return MakeTypedUuid(kind, uuid);
}

}  // namespace

const char* UuidKindName(UuidKind kind) {
  switch (kind) {
    case UuidKind::database: return "database";
    case UuidKind::cluster: return "cluster";
    case UuidKind::filespace: return "filespace";
    case UuidKind::schema: return "schema";
    case UuidKind::object: return "object";
    case UuidKind::row: return "row";
    case UuidKind::page: return "page";
    case UuidKind::transaction: return "transaction";
    case UuidKind::session: return "session";
    case UuidKind::principal: return "principal";
    case UuidKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* UuidVersionName(u8 version) {
  switch (version) {
    case 1: return "time_node_v1";
    case 2: return "dce_security_v2";
    case 3: return "name_md5_v3";
    case 4: return "random_v4";
    case 5: return "name_sha1_v5";
    case 6: return "reordered_time_v6";
    case 7: return "unix_time_v7";
    default: return "unknown";
  }
}

bool IsEngineIdentityKind(UuidKind kind) {
  return kind != UuidKind::unknown;
}

bool UuidKindAllowsDurableIdentity(UuidKind kind) {
  switch (kind) {
    case UuidKind::database:
    case UuidKind::cluster:
    case UuidKind::filespace:
    case UuidKind::schema:
    case UuidKind::object:
    case UuidKind::row:
    case UuidKind::page:
    case UuidKind::transaction:
    case UuidKind::principal:
      return true;
    case UuidKind::session:
    case UuidKind::unknown:
      return false;
  }
  return false;
}

bool IsDurableEngineIdentityKind(UuidKind kind) {
  return UuidKindAllowsDurableIdentity(kind);
}

Uuid NilUuid() {
  return {};
}

bool IsNilUuid(const Uuid& uuid) {
  return uuid.is_nil();
}

bool IsValidUuidVariant(const Uuid& uuid) {
  return (uuid.bytes[8] & 0xc0u) == 0x80u;
}

u8 UuidVersion(const Uuid& uuid) {
  return static_cast<u8>((uuid.bytes[6] >> 4) & 0x0fu);
}

bool IsEngineIdentityUuid(const Uuid& uuid) {
  return !uuid.is_nil() && IsValidUuidVariant(uuid) && UuidVersion(uuid) == 7;
}

bool UuidVersionAllowed(const Uuid& uuid, const UuidVersionPolicy& policy) {
  switch (UuidVersion(uuid)) {
    case 1: return policy.allow_v1;
    case 2: return policy.allow_v2;
    case 3: return policy.allow_v3;
    case 4: return policy.allow_v4;
    case 5: return policy.allow_v5;
    case 6: return policy.allow_v6;
    case 7: return policy.allow_v7;
    default: return false;
  }
}

int CompareUuid128(const Uuid& left, const Uuid& right) {
  if (left.bytes < right.bytes) {
    return -1;
  }
  if (right.bytes < left.bytes) {
    return 1;
  }
  return 0;
}

UuidV7TimePrefixResult ExtractUuidV7TimePrefix(const Uuid& uuid) {
  UuidV7TimePrefixResult result;
  if (uuid.is_nil()) {
    result.refusal_reason = "nil_uuid";
    return result;
  }
  if (!IsValidUuidVariant(uuid)) {
    result.refusal_reason = "invalid_variant";
    return result;
  }
  if (UuidVersion(uuid) != 7) {
    result.refusal_reason = "not_uuid_v7";
    return result;
  }
  result.ok = true;
  result.fallback_to_uncompressed_uuid = false;
  result.unix_epoch_millis =
      (static_cast<u64>(uuid.bytes[0]) << 40) |
      (static_cast<u64>(uuid.bytes[1]) << 32) |
      (static_cast<u64>(uuid.bytes[2]) << 24) |
      (static_cast<u64>(uuid.bytes[3]) << 16) |
      (static_cast<u64>(uuid.bytes[4]) << 8) |
      static_cast<u64>(uuid.bytes[5]);
  return result;
}

UuidV7IndexCompareResult CompareUuidV7ForIndex(const TypedUuid& left,
                                               const TypedUuid& right,
                                               UuidKind expected_kind) {
  UuidV7IndexCompareResult result;
  if (expected_kind == UuidKind::unknown) {
    result.refusal_reason = "unsupported_kind";
    result.comparison = CompareUuid128(left.value, right.value);
    return result;
  }
  if (left.kind != expected_kind || right.kind != expected_kind) {
    result.refusal_reason = "kind_mismatch";
    result.comparison = CompareUuid128(left.value, right.value);
    return result;
  }
  const auto left_time = ExtractUuidV7TimePrefix(left.value);
  const auto right_time = ExtractUuidV7TimePrefix(right.value);
  if (!left_time.ok || !right_time.ok) {
    result.refusal_reason = left_time.ok ? right_time.refusal_reason : left_time.refusal_reason;
    result.comparison = CompareUuid128(left.value, right.value);
    return result;
  }
  result.ok = true;
  result.specialized_comparator_used = true;
  result.fallback_to_uncompressed_uuid = false;
  if (left_time.unix_epoch_millis < right_time.unix_epoch_millis) {
    result.comparison = -1;
  } else if (left_time.unix_epoch_millis > right_time.unix_epoch_millis) {
    result.comparison = 1;
  } else {
    result.comparison = CompareUuid128(left.value, right.value);
  }
  return result;
}

std::string UuidToString(const Uuid& uuid) {
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < uuid.bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
    const byte value = uuid.bytes[i];
    out.push_back(kHex[(value >> 4) & 0x0f]);
    out.push_back(kHex[value & 0x0f]);
  }
  return out;
}

UuidParseResult ParseUuid(std::string text) {
  UuidParseResult result;
  result.status = UuidOkStatus();

  if (!HasCanonicalDashPositions(text)) {
    result.status = UuidErrorStatus();
    result.diagnostic = MakeUuidDiagnostic(result.status, "SB-UUID-PARSE-SHAPE", "uuid.parse.invalid_shape", text);
    return result;
  }

  const std::string stripped = StripUuidDashes(text);
  if (stripped.size() != 32) {
    result.status = UuidErrorStatus();
    result.diagnostic = MakeUuidDiagnostic(result.status, "SB-UUID-PARSE-LENGTH", "uuid.parse.invalid_length", text);
    return result;
  }

  for (std::size_t i = 0; i < 16; ++i) {
    const int high = HexValue(stripped[i * 2]);
    const int low = HexValue(stripped[i * 2 + 1]);
    if (high < 0 || low < 0) {
      result.status = UuidErrorStatus();
      result.diagnostic = MakeUuidDiagnostic(result.status, "SB-UUID-PARSE-HEX", "uuid.parse.invalid_hex", text);
      return result;
    }
    result.value.bytes[i] = static_cast<byte>((high << 4) | low);
  }

  if (result.value.is_nil()) {
    result.status = UuidErrorStatus();
    result.diagnostic = MakeUuidDiagnostic(result.status, "SB-UUID-PARSE-NIL", "uuid.parse.nil_not_allowed", text);
    return result;
  }

  if (!IsValidUuidVariant(result.value)) {
    result.status = UuidErrorStatus();
    result.diagnostic = MakeUuidDiagnostic(result.status, "SB-UUID-PARSE-VARIANT", "uuid.parse.invalid_variant", text);
    return result;
  }

  return result;
}

TypedUuidResult MakeTypedUuid(UuidKind kind, Uuid value) {
  TypedUuidResult result;
  result.status = UuidOkStatus();
  result.value = {kind, value};

  if (kind == UuidKind::unknown) {
    result.status = UuidErrorStatus();
    result.diagnostic = MakeUuidDiagnostic(result.status, "SB-UUID-TYPED-UNKNOWN-KIND", "uuid.typed.unknown_kind");
    return result;
  }

  if (value.is_nil()) {
    result.status = UuidErrorStatus();
    result.diagnostic = MakeUuidDiagnostic(result.status, "SB-UUID-TYPED-NIL", "uuid.typed.nil_not_allowed", UuidKindName(kind));
    return result;
  }

  if (!IsValidUuidVariant(value)) {
    result.status = UuidErrorStatus();
    result.diagnostic = MakeUuidDiagnostic(result.status, "SB-UUID-TYPED-VARIANT", "uuid.typed.invalid_variant", UuidKindName(kind));
    return result;
  }

  if (UuidVersion(value) != 7) {
    result.status = UuidErrorStatus();
    result.diagnostic = MakeUuidDiagnostic(result.status, "SB-UUID-TYPED-ENGINE-IDENTITY-NOT-V7", "uuid.typed.engine_identity_requires_v7", UuidKindName(kind));
    return result;
  }

  return result;
}

TypedUuidResult ParseTypedUuid(UuidKind kind, std::string text) {
  const UuidParseResult parsed = ParseUuid(std::move(text));
  if (!parsed.ok()) {
    TypedUuidResult result;
    result.status = parsed.status;
    result.diagnostic = parsed.diagnostic;
    return result;
  }
  return MakeTypedUuid(kind, parsed.value);
}

TypedUuidResult MakeDurableEngineIdentityUuid(UuidKind kind, Uuid value) {
  if (!UuidKindAllowsDurableIdentity(kind)) {
    TypedUuidResult result;
    result.status = UuidErrorStatus();
    result.diagnostic = MakeUuidDiagnostic(result.status,
                                           "SB-UUID-DURABLE-IDENTITY-KIND",
                                           "uuid.durable_identity.kind_not_allowed",
                                           UuidKindName(kind));
    return result;
  }
  return MakeTypedUuid(kind, value);
}

TypedUuidResult ParseDurableEngineIdentityUuid(UuidKind kind, std::string text) {
  const UuidParseResult parsed = ParseUuid(std::move(text));
  if (!parsed.ok()) {
    TypedUuidResult result;
    result.status = parsed.status;
    result.diagnostic = parsed.diagnostic;
    return result;
  }
  return MakeDurableEngineIdentityUuid(kind, parsed.value);
}

TypedUuidResult GenerateTimeNodeV1(UuidKind kind,
                                   u64 gregorian_100ns_timestamp,
                                   u16 clock_sequence,
                                   std::array<byte, 6> node) {
  return MakeTypedUuid(kind, MakeV1Layout(1, gregorian_100ns_timestamp, clock_sequence, node));
}

TypedUuidResult GenerateDceSecurityV2(UuidKind kind,
                                      u64 gregorian_100ns_timestamp,
                                      u16 clock_sequence,
                                      std::array<byte, 6> node,
                                      u8 local_domain,
                                      u32 local_identifier) {
  Uuid uuid = MakeV1Layout(2, gregorian_100ns_timestamp, clock_sequence, node);
  StoreBe32(&uuid, 0, local_identifier);
  uuid.bytes[9] = local_domain;
  return MakeTypedUuid(kind, uuid);
}

TypedUuidResult GenerateNameBasedV3(UuidKind kind, const Uuid& namespace_uuid, std::string name) {
  const std::vector<byte> data = NamespaceNameBytes(namespace_uuid, name);
  const std::array<byte, 16> digest = Md5(data);
  Uuid uuid;
  uuid.bytes = digest;
  return MakeUuidWithVersion(kind, uuid, 3);
}

TypedUuidResult GenerateRandomV4(UuidKind kind) {
  Uuid uuid;
  uuid.bytes = RandomBytes16();
  return MakeUuidWithVersion(kind, uuid, 4);
}

TypedUuidResult GenerateNameBasedV5(UuidKind kind, const Uuid& namespace_uuid, std::string name) {
  const std::vector<byte> data = NamespaceNameBytes(namespace_uuid, name);
  const std::array<byte, 20> digest = Sha1(data);
  Uuid uuid;
  std::copy_n(digest.begin(), uuid.bytes.size(), uuid.bytes.begin());
  return MakeUuidWithVersion(kind, uuid, 5);
}

TypedUuidResult GenerateReorderedTimeV6(UuidKind kind,
                                        u64 gregorian_100ns_timestamp,
                                        u16 clock_sequence,
                                        std::array<byte, 6> node) {
  Uuid uuid;
  const u64 timestamp = gregorian_100ns_timestamp & kUuid60BitMask;
  uuid.bytes[0] = static_cast<byte>((timestamp >> 52) & 0xffu);
  uuid.bytes[1] = static_cast<byte>((timestamp >> 44) & 0xffu);
  uuid.bytes[2] = static_cast<byte>((timestamp >> 36) & 0xffu);
  uuid.bytes[3] = static_cast<byte>((timestamp >> 28) & 0xffu);
  uuid.bytes[4] = static_cast<byte>((timestamp >> 20) & 0xffu);
  uuid.bytes[5] = static_cast<byte>((timestamp >> 12) & 0xffu);
  uuid.bytes[6] = static_cast<byte>(0x60u | ((timestamp >> 8) & 0x0fu));
  uuid.bytes[7] = static_cast<byte>(timestamp & 0xffu);
  SetVariantAndClockSequence(&uuid, clock_sequence);
  for (std::size_t i = 0; i < node.size(); ++i) {
    uuid.bytes[10 + i] = node[i];
  }
  return MakeTypedUuid(kind, uuid);
}

TypedUuidResult GenerateUnixTimeV7(UuidKind kind, u64 unix_epoch_millis) {
  Uuid uuid;
  uuid.bytes = RandomBytes16();
  const u64 ms = unix_epoch_millis & 0x0000ffffffffffffull;
  uuid.bytes[0] = static_cast<byte>((ms >> 40) & 0xffu);
  uuid.bytes[1] = static_cast<byte>((ms >> 32) & 0xffu);
  uuid.bytes[2] = static_cast<byte>((ms >> 24) & 0xffu);
  uuid.bytes[3] = static_cast<byte>((ms >> 16) & 0xffu);
  uuid.bytes[4] = static_cast<byte>((ms >> 8) & 0xffu);
  uuid.bytes[5] = static_cast<byte>(ms & 0xffu);
  SetVersion(&uuid, 7);
  return MakeTypedUuid(kind, uuid);
}

UuidResult MakeCompatibilityUuidResult(Uuid uuid, u8 expected_version) {
  UuidResult result;
  result.status = UuidOkStatus();
  result.value = uuid;
  if (uuid.is_nil()) {
    result.status = UuidErrorStatus();
    result.diagnostic = MakeUuidDiagnostic(result.status, "SB-UUID-COMPATIBILITY-NIL", "uuid.compatibility.nil_not_allowed");
    return result;
  }
  if (!IsValidUuidVariant(uuid)) {
    result.status = UuidErrorStatus();
    result.diagnostic = MakeUuidDiagnostic(result.status, "SB-UUID-COMPATIBILITY-VARIANT", "uuid.compatibility.invalid_variant");
    return result;
  }
  if (UuidVersion(uuid) != expected_version) {
    result.status = UuidErrorStatus();
    result.diagnostic = MakeUuidDiagnostic(result.status, "SB-UUID-COMPATIBILITY-VERSION", "uuid.compatibility.invalid_version", UuidVersionName(UuidVersion(uuid)));
    return result;
  }
  return result;
}

TypedUuidResult GenerateEngineIdentityV7(UuidKind kind, u64 unix_epoch_millis) {
  return GenerateDurableEngineIdentityV7(kind, unix_epoch_millis);
}

TypedUuidResult GenerateDurableEngineIdentityV7(UuidKind kind, u64 unix_epoch_millis) {
  UuidResult raw = GenerateCompatibilityUnixTimeV7(unix_epoch_millis);
  if (!raw.ok()) {
    TypedUuidResult result;
    result.status = raw.status;
    result.diagnostic = raw.diagnostic;
    return result;
  }
  return MakeDurableEngineIdentityUuid(kind, raw.value);
}

UuidResult GenerateCompatibilityTimeNodeV1(u64 gregorian_100ns_timestamp, u16 clock_sequence, std::array<byte, 6> node) {
  return MakeCompatibilityUuidResult(MakeV1Layout(1, gregorian_100ns_timestamp, clock_sequence, node), 1);
}

UuidResult GenerateCompatibilityDceSecurityV2(u64 gregorian_100ns_timestamp, u16 clock_sequence, std::array<byte, 6> node, u8 local_domain, u32 local_identifier) {
  Uuid uuid = MakeV1Layout(2, gregorian_100ns_timestamp, clock_sequence, node);
  StoreBe32(&uuid, 0, local_identifier);
  uuid.bytes[9] = local_domain;
  return MakeCompatibilityUuidResult(uuid, 2);
}

UuidResult GenerateCompatibilityNameBasedV3(const Uuid& namespace_uuid, std::string name) {
  const std::vector<byte> data = NamespaceNameBytes(namespace_uuid, name);
  const std::array<byte, 16> digest = Md5(data);
  Uuid uuid;
  uuid.bytes = digest;
  SetVersion(&uuid, 3);
  return MakeCompatibilityUuidResult(uuid, 3);
}

UuidResult GenerateCompatibilityRandomV4() {
  Uuid uuid;
  uuid.bytes = RandomBytes16();
  SetVersion(&uuid, 4);
  return MakeCompatibilityUuidResult(uuid, 4);
}

UuidResult GenerateCompatibilityNameBasedV5(const Uuid& namespace_uuid, std::string name) {
  const std::vector<byte> data = NamespaceNameBytes(namespace_uuid, name);
  const std::array<byte, 20> digest = Sha1(data);
  Uuid uuid;
  std::copy_n(digest.begin(), uuid.bytes.size(), uuid.bytes.begin());
  SetVersion(&uuid, 5);
  return MakeCompatibilityUuidResult(uuid, 5);
}

UuidResult GenerateCompatibilityReorderedTimeV6(u64 gregorian_100ns_timestamp, u16 clock_sequence, std::array<byte, 6> node) {
  Uuid uuid;
  const u64 timestamp = gregorian_100ns_timestamp & kUuid60BitMask;
  uuid.bytes[0] = static_cast<byte>((timestamp >> 52) & 0xffu);
  uuid.bytes[1] = static_cast<byte>((timestamp >> 44) & 0xffu);
  uuid.bytes[2] = static_cast<byte>((timestamp >> 36) & 0xffu);
  uuid.bytes[3] = static_cast<byte>((timestamp >> 28) & 0xffu);
  uuid.bytes[4] = static_cast<byte>((timestamp >> 20) & 0xffu);
  uuid.bytes[5] = static_cast<byte>((timestamp >> 12) & 0xffu);
  uuid.bytes[6] = static_cast<byte>(0x60u | ((timestamp >> 8) & 0x0fu));
  uuid.bytes[7] = static_cast<byte>(timestamp & 0xffu);
  SetVariantAndClockSequence(&uuid, clock_sequence);
  for (std::size_t i = 0; i < node.size(); ++i) uuid.bytes[10 + i] = node[i];
  return MakeCompatibilityUuidResult(uuid, 6);
}

UuidResult GenerateCompatibilityUnixTimeV7(u64 unix_epoch_millis) {
  Uuid uuid;
  uuid.bytes = RandomBytes16();
  const u64 ms = unix_epoch_millis & 0x0000ffffffffffffull;
  uuid.bytes[0] = static_cast<byte>((ms >> 40) & 0xffu);
  uuid.bytes[1] = static_cast<byte>((ms >> 32) & 0xffu);
  uuid.bytes[2] = static_cast<byte>((ms >> 24) & 0xffu);
  uuid.bytes[3] = static_cast<byte>((ms >> 16) & 0xffu);
  uuid.bytes[4] = static_cast<byte>((ms >> 8) & 0xffu);
  uuid.bytes[5] = static_cast<byte>(ms & 0xffu);
  SetVersion(&uuid, 7);
  return MakeCompatibilityUuidResult(uuid, 7);
}

DiagnosticRecord MakeUuidDiagnostic(Status status,
                                    std::string diagnostic_code,
                                    std::string message_key,
                                    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) arguments.push_back({"detail", detail});
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.uuid");
}

}  // namespace scratchbird::core::uuid

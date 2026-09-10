// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/mga_savepoint_marker_codec.hpp"
#include "mga_relation_store/mga_row_codec.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {
constexpr char kMagic[] = "\x89SBSP2\r\n";
constexpr std::size_t kHeader = 12;
constexpr std::size_t kDigest = 32;
constexpr std::size_t kMinimum = kHeader + 4 + 7 * 8 + 4 + 1 + kDigest;
}

std::string MgaSavepointUuidKey(std::string_view uuid) {
  return std::string(1, '\0') + std::string(uuid);
}

std::uint32_t MgaSavepointMarkerFrameSize(std::string_view bytes) {
  if (bytes.size() < kHeader) return 0;
  if (bytes.substr(0, 8) != std::string_view(kMagic, 8)) return UINT32_MAX;
  std::uint32_t size = 0;
  for (unsigned i = 0; i < 4; ++i)
    size |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[8 + i])) << (8 * i);
  return size >= kMinimum && size <= kMgaSavepointFrameMaximum ? size : UINT32_MAX;
}

std::string EncodeMgaSavepointMarker(const MgaSavepointMarkerRecord& r) {
  if (r.kind < 1 || r.kind > 3 || !r.transaction || r.identity.empty()) return {};
  std::string body;
  AppendBinaryU8(&body, r.kind);
  AppendBinaryU8(&body, r.uuid_identity ? 1 : 0);
  AppendBinaryU16(&body, 0);
  AppendBinaryU64(&body, r.transaction);
  for (auto value : r.cutoffs) AppendBinaryU64(&body, value);
  for (auto value : r.upper) AppendBinaryU64(&body, value);
  if (r.uuid_identity) {
    const auto uuid = scratchbird::core::uuid::ParseUuid(r.identity);
    if (!uuid.ok() || scratchbird::core::uuid::IsNilUuid(uuid.value)) return {};
    AppendBinaryU32(&body, 16);
    body.append(reinterpret_cast<const char*>(uuid.value.bytes.data()), 16);
  } else {
    if (r.identity.find('\0') != std::string::npos ||
        r.identity.size() > kMgaSavepointFrameMaximum) return {};
    if (!AppendBinaryString(&body, r.identity)) return {};
  }
  if (body.size() + kHeader + kDigest > kMgaSavepointFrameMaximum) return {};
  std::string out(kMagic, 8);
  AppendBinaryU32(&out, static_cast<std::uint32_t>(body.size() + kHeader + kDigest));
  out += body;
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const scratchbird::core::platform::byte*>(out.data()), out.size());
  if (!digest.ok()) return {};
  out.append(reinterpret_cast<const char*>(digest.digest.data()), digest.digest.size());
  return out;
}

bool DecodeMgaSavepointMarker(std::string_view bytes, MgaSavepointMarkerRecord* r) {
  if (!r || bytes.size() < kMinimum || bytes.size() > kMgaSavepointFrameMaximum ||
      MgaSavepointMarkerFrameSize(bytes) != bytes.size()) return false;
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const scratchbird::core::platform::byte*>(bytes.data()), bytes.size() - kDigest);
  if (!digest.ok() || !std::equal(digest.digest.begin(), digest.digest.end(),
      reinterpret_cast<const scratchbird::core::platform::byte*>(bytes.data() + bytes.size() - kDigest))) return false;
  std::vector<scratchbird::core::index::byte> body(bytes.begin() + kHeader, bytes.end() - kDigest);
  std::size_t offset = 0;
  std::uint8_t identity_kind = 0;
  std::uint16_t reserved = 0;
  MgaSavepointMarkerRecord out;
  if (!ReadBinaryU8(body, &offset, &out.kind) || out.kind < 1 || out.kind > 3 ||
      !ReadBinaryU8(body, &offset, &identity_kind) || identity_kind > 1 ||
      !ReadBinaryU16(body, &offset, &reserved) || reserved != 0 ||
      !ReadBinaryU64(body, &offset, &out.transaction) || !out.transaction) return false;
  for (auto& value : out.cutoffs) if (!ReadBinaryU64(body, &offset, &value)) return false;
  for (auto& value : out.upper) if (!ReadBinaryU64(body, &offset, &value)) return false;
  out.uuid_identity = identity_kind == 1;
  std::uint32_t size = 0;
  if (!ReadBinaryU32(body, &offset, &size) || size == 0 || size != body.size() - offset) return false;
  if (out.uuid_identity) {
    if (size != 16) return false;
    scratchbird::core::platform::Uuid uuid{};
    std::copy_n(body.begin() + offset, 16, uuid.bytes.begin());
    if (scratchbird::core::uuid::IsNilUuid(uuid)) return false;
    out.identity = scratchbird::core::uuid::UuidToString(uuid);
  } else {
    out.identity.assign(reinterpret_cast<const char*>(body.data() + offset), size);
    if (out.identity.find('\0') != std::string::npos) return false;
  }
  *r = std::move(out);
  return true;
}
}  // namespace scratchbird::engine::internal_api

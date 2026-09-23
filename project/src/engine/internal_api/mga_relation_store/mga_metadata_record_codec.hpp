// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "mga_binary_identity_codec.hpp"
#include "hash_digest.hpp"
#include <algorithm>
#include <vector>

namespace scratchbird::engine::internal_api {
inline constexpr std::size_t kMgaMetadataMaximumBytes = 64u * 1024u * 1024u;
inline constexpr std::string_view kMgaMetadataRecordMagic = "SBMGAM02";
inline std::string MetadataUuidBytes(const EngineUuid& id) {
  return std::string(reinterpret_cast<const char*>(id.bytes.data()), id.bytes.size());
}
inline bool ReadMetadataUuid(std::string_view bytes, EngineUuid* output, bool optional = false) {
  if (!output || bytes.size() != 16) return false;
  EngineUuid id;
  std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data()), 16, id.bytes.begin());
  if (!core::uuid::IsEngineIdentityUuid(id) && !(optional && id.is_nil())) return false;
  *output = id;
  return true;
}
inline std::string EncodeMgaMetadataFields(const std::vector<std::string>& fields) {
  if (fields.empty() || fields.size() > 65536) return {};
  std::string payload;
  AppendBinaryU32(&payload, static_cast<std::uint32_t>(fields.size()));
  for (const auto& field : fields) {
    if (field.size() > kMgaMetadataMaximumBytes || !AppendBinaryString(&payload, field) ||
        payload.size() > kMgaMetadataMaximumBytes) return {};
  }
  std::string frame(kMgaMetadataRecordMagic);
  AppendBinaryU64(&frame, payload.size());
  frame += payload;
  const auto digest = core::hash::ComputeSha256Digest(
      reinterpret_cast<const core::platform::byte*>(frame.data()), frame.size());
  if (!digest.ok() || digest.digest_bytes != core::hash::kSha256DigestBytes) return {};
  frame.append(reinterpret_cast<const char*>(digest.digest.data()), digest.digest.size());
  return frame;
}
inline bool DecodeMgaMetadataFields(std::string_view frame, std::vector<std::string>* output) {
  if (!output || frame.size() < 52 || frame.size() > kMgaMetadataMaximumBytes + 48 ||
      !frame.starts_with(kMgaMetadataRecordMagic)) return false;
  const std::span<const std::uint8_t> input(reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size());
  std::size_t cursor = 8;
  std::uint64_t size = 0;
  if (!ReadBinaryU64(input, &cursor, &size) || size != frame.size() - 48) return false;
  const auto digest = core::hash::ComputeSha256Digest(input.data(), input.size() - 32);
  if (!digest.ok() || digest.digest_bytes != core::hash::kSha256DigestBytes ||
      !std::equal(digest.digest.begin(), digest.digest.end(), input.end() - 32)) return false;
  const auto payload = input.subspan(16, static_cast<std::size_t>(size));
  cursor = 0;
  std::uint32_t count = 0;
  if (!ReadBinaryU32(payload, &cursor, &count) || count == 0 || count > 65536 ||
      count > (payload.size() - cursor) / 4) return false;
  std::vector<std::string> fields;
  fields.reserve(count);
  for (std::uint32_t n = 0; n < count; ++n) {
    std::string field;
    if (!ReadBinaryString(payload, &cursor, &field)) return false;
    fields.push_back(std::move(field));
  }
  if (cursor != payload.size()) return false;
  output->swap(fields);
  return true;
}
inline bool DecodeMgaMetadataStream(std::span<const std::uint8_t> input,
                                     std::vector<std::string>* output) {
  if (!output) return false;
  std::vector<std::string> records;
  std::size_t cursor = 0;
  while (cursor < input.size()) {
    if (input.size() - cursor < 48) return false;
    std::size_t length_cursor = cursor + 8;
    std::uint64_t length = 0;
    if (!ReadBinaryU64(input, &length_cursor, &length) || length > kMgaMetadataMaximumBytes ||
        length > input.size() - cursor - 48) return false;
    std::string_view frame(reinterpret_cast<const char*>(input.data() + cursor), static_cast<std::size_t>(length) + 48);
    std::vector<std::string> fields;
    if (!DecodeMgaMetadataFields(frame, &fields)) return false;
    records.emplace_back(frame);
    cursor += frame.size();
  }
  output->swap(records);
  return true;
}
inline std::string EncodeMetadataPairs(const std::vector<std::pair<std::string, std::string>>& pairs) {
  if (pairs.size() > 32767) return {};
  std::vector<std::string> fields;
  // A format tag allows an empty pair vector without an ambiguous empty frame.
  fields.emplace_back("metadata.pairs.v2");
  for (const auto& [key, value] : pairs) { fields.push_back(key); fields.push_back(value); }
  return EncodeMgaMetadataFields(fields);
}
inline bool DecodeMetadataPairs(std::string_view bytes,
    std::vector<std::pair<std::string, std::string>>* output) {
  std::vector<std::string> fields;
  if (!output || !DecodeMgaMetadataFields(bytes, &fields) || fields[0] != "metadata.pairs.v2" || fields.size() % 2 != 1) return false;
  std::vector<std::pair<std::string, std::string>> pairs;
  for (std::size_t n = 1; n < fields.size(); n += 2) pairs.emplace_back(std::move(fields[n]), std::move(fields[n+1]));
  output->swap(pairs);
  return true;
}
} // namespace scratchbird::engine::internal_api

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "mga_relation_store/mga_metadata_record_codec.hpp"
#include <charconv>
namespace scratchbird::engine::internal_api {
inline constexpr std::string_view kMgaLargeValueLocatorMagic = "SBMGLV02";
inline bool IsMgaLargeValueLocator(std::string_view bytes) {
  return bytes.starts_with(kMgaLargeValueLocatorMagic);
}
inline std::string MakeMgaLargeValueLocator(const EngineUuid& id,
                                           std::uint64_t checksum,
                                           std::uint64_t size) {
  std::string bytes(kMgaLargeValueLocatorMagic);
  if (!AppendBinaryEngineUuid(&bytes, id)) return {};
  AppendBinaryU64(&bytes, checksum);
  AppendBinaryU64(&bytes, size);
  return bytes;
}
inline bool ReadMgaLargeValueLocator(std::string_view bytes, EngineUuid* id,
                                    std::uint64_t* checksum, std::uint64_t* size) {
  if (!id || !checksum || !size || bytes.size() != 40 || !IsMgaLargeValueLocator(bytes)) return false;
  const std::span<const std::uint8_t> input(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
  std::size_t cursor = 8;
  EngineUuid native;
  std::uint64_t hash = 0, length = 0;
  if (!ReadBinaryEngineUuid(input, &cursor, &native) ||
      !ReadBinaryU64(input, &cursor, &hash) || !ReadBinaryU64(input, &cursor, &length)) return false;
  *id = native; *checksum = hash; *size = length;
  return true;
}
inline bool LargeValueNumber(std::string_view text, std::uint64_t* value) {
  if (!value || text.empty() || (text.size() > 1 && text.front() == '0')) return false;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
// A validated record has native identity octets in its framed identity slots.
inline bool ValidateMgaLargeValueFields(const std::vector<std::string>& fields) {
  EngineUuid id;
  std::uint64_t value = 0;
  if (fields.size() < 4 || fields[0] != "SBMGL002" ||
      !LargeValueNumber(fields[2], &value) || value == 0 || !ReadMetadataUuid(fields[3], &id)) return false;
  if (fields[1] == "LARGE_VALUE_CHUNK") {
    return fields.size() == 7 && LargeValueNumber(fields[4], &value) &&
        fields[5].size() <= 2048 && LargeValueNumber(fields[6], &value);
  }
  if (fields[1] != "LARGE_VALUE" && fields[1] != "LARGE_VALUE_RECLAIMED") return false;
  if (fields.size() != (fields[1] == "LARGE_VALUE" ? 11u : 9u) ||
      !ReadMetadataUuid(fields[4], &id) || !ReadMetadataUuid(fields[5], &id) ||
      !ReadMetadataUuid(fields[6], &id)) return false;
  return fields[1] == "LARGE_VALUE_RECLAIMED" ||
      (LargeValueNumber(fields[8], &value) && LargeValueNumber(fields[9], &value) && fields[10] == "durable_uncommitted");
}
} // namespace scratchbird::engine::internal_api

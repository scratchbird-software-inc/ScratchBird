// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "mga_binary_fields.hpp"
#include "mga_binary_identity_codec.hpp"
#include <map>
#include <vector>

namespace scratchbird::engine::internal_api {
using MgaDescriptorFields = std::vector<std::pair<std::string, std::string>>;
using MgaDescriptorRecords = std::map<EngineUuid, MgaDescriptorFields>;
inline constexpr std::string_view kMgaDescriptorBinaryMagic = "SBMGADESC2";

// V2: magic, LE64 payload size, raw16 relation identity, LE32 pair count,
// then LE32-length-framed key/value octets. Field values retain their own
// existing codecs; this envelope does not interpret or issue their identities.
inline bool AppendMgaDescriptorRecord(const EngineUuid& relation,
    const MgaDescriptorFields& fields, std::string* output) {
  if (!output || fields.size() > std::numeric_limits<std::uint32_t>::max()) return false;
  std::string payload;
  if (!AppendBinaryEngineUuid(&payload, relation)) return false;
  AppendBinaryU32(&payload, static_cast<std::uint32_t>(fields.size()));
  for (const auto& [key, value] : fields) {
    if (!AppendBinaryString(&payload, key) || !AppendBinaryString(&payload, value)) return false;
  }
  std::string frame(kMgaDescriptorBinaryMagic);
  AppendBinaryU64(&frame, payload.size());
  frame += payload;
  output->append(frame);
  return true;
}

// Only binary16 identity records are admitted by system storage. Historical
// TEXT records must be converted outside the engine before admission.
// Decode the complete stream before publishing any cache entries.
inline bool DecodeMgaDescriptorRecords(std::span<const std::uint8_t> bytes,
                                      MgaDescriptorRecords* output) {
  if (!output) return false;
  MgaDescriptorRecords staged;
  std::size_t cursor = 0;
  while (cursor < bytes.size()) {
    const std::string_view tail(reinterpret_cast<const char*>(bytes.data() + cursor),
                                bytes.size() - cursor);
    EngineUuid relation;
    MgaDescriptorFields fields;
    if (tail.starts_with(kMgaDescriptorBinaryMagic)) {
      cursor += kMgaDescriptorBinaryMagic.size();
      std::uint64_t length = 0;
      if (!ReadBinaryU64(bytes, &cursor, &length) || length > bytes.size() - cursor) return false;
      const auto payload = bytes.subspan(cursor, static_cast<std::size_t>(length));
      std::size_t offset = 0;
      std::uint32_t count = 0;
      if (!ReadBinaryEngineUuid(payload, &offset, &relation) ||
          !ReadBinaryU32(payload, &offset, &count) || count > (payload.size() - offset) / 8) return false;
      for (std::uint32_t n = 0; n < count; ++n) {
        std::string key, value;
        if (!ReadBinaryString(payload, &offset, &key) ||
            !ReadBinaryString(payload, &offset, &value)) return false;
        fields.emplace_back(std::move(key), std::move(value));
      }
      if (offset != payload.size()) return false;
      cursor += static_cast<std::size_t>(length);
    } else {
      return false;
    }
    staged[relation] = std::move(fields);
  }
  output->swap(staged); return true;
}
} // namespace scratchbird::engine::internal_api

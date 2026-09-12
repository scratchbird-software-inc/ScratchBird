// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "resource_seed_pack.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace scratchbird::core::resources {
namespace artifact_content_detail {
constexpr std::size_t header_size = 48, fragment_size = 1024;
using platform::Uuid;
inline bool IdentityValid(const Uuid& id) noexcept {
  return !id.is_nil() && (id.bytes[6] >> 4) == 7 && (id.bytes[8] & 0xc0) == 0x80;
}
inline void AppendNumber(std::string& out, u64 value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) out.push_back(static_cast<char>(value >> (8 * i)));
}
inline u64 Number(std::string_view bytes, std::size_t pos, unsigned width) noexcept {
  u64 result = 0;
  for (unsigned i = 0; i < width; ++i)
    result |= u64{static_cast<unsigned char>(bytes[pos + i])} << (8 * i);
  return result;
}
inline bool Text(std::string_view bytes, std::size_t* pos, std::string* out) {
  if (bytes.size() - *pos < 4) return false;
  const auto length = Number(bytes, *pos, 4); *pos += 4;
  if (length > bytes.size() - *pos) return false;
  out->assign(bytes.substr(*pos, length)); *pos += length;
  return true;
}
}  // namespace artifact_content_detail

// Caller supplies an admitted owning-node catalog artifact identity. This codec
// never reads paths or grants authority to its own checksum/metadata fields.
inline bool EncodeResourceSeedArtifactContent(const ResourceSeedArtifact& artifact,
                                              std::vector<std::string>* output) noexcept {
  using namespace artifact_content_detail;
  if (!output) return false;
  try {
    if (!IdentityValid(artifact.artifact_uuid) || !ValidateResourceSeedArtifactContent(artifact)) return false;
    constexpr u64 max_size = std::numeric_limits<u32>::max();
    u64 total = 4 + 5 * 4 + 8;
    for (const auto* field : {&artifact.canonical_path, &artifact.source_pattern,
                              &artifact.required_catalog_rows, &artifact.create_time_action,
                              &artifact.content_hash}) {
      if (field->size() > max_size - total) return false;
      total += field->size();
    }
    if (artifact.content->size() > max_size - total) return false;
    total += artifact.content->size();
    std::string body; body.reserve(static_cast<std::size_t>(total));
    AppendNumber(body, static_cast<u16>(artifact.family), 2); AppendNumber(body, 0, 2);
    for (const auto* field : {&artifact.canonical_path, &artifact.source_pattern,
                              &artifact.required_catalog_rows, &artifact.create_time_action,
                              &artifact.content_hash}) {
      AppendNumber(body, field->size(), 4); body.append(*field);
    }
    AppendNumber(body, artifact.content_size_bytes, 8); body.append(*artifact.content);
    const auto count = (total + fragment_size - 1) / fragment_size;
    std::vector<std::string> staged; staged.reserve(static_cast<std::size_t>(count));
    for (u64 index = 0, offset = 0; index < count; ++index, offset += fragment_size) {
      const auto length = std::min<u64>(fragment_size, total - offset);
      std::string row; row.reserve(header_size + length); row.append("RSAC", 4);
      AppendNumber(row, 1, 2); AppendNumber(row, 0, 2);
      row.append(reinterpret_cast<const char*>(artifact.artifact_uuid.bytes.data()), 16);
      AppendNumber(row, total, 8); AppendNumber(row, index, 4); AppendNumber(row, count, 4);
      AppendNumber(row, length, 4); AppendNumber(row, 0, 4);
      row.append(body, static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
      staged.push_back(std::move(row));
    }
    output->swap(staged); return true;
  } catch (const std::bad_alloc&) { return false; }
    catch (const std::length_error&) { return false; }
}

inline bool DecodeResourceSeedArtifactContents(const std::vector<std::string_view>& rows,
                                               std::vector<ResourceSeedArtifact>* output) noexcept {
  using namespace artifact_content_detail;
  if (!output) return false;
  try {
    std::vector<ResourceSeedArtifact> staged;
    std::set<Uuid> identities;
    for (std::size_t start = 0; start < rows.size();) {
      const auto first = rows[start];
      if (first.size() < header_size) return false;
      const u64 total = Number(first, 24, 8), count = Number(first, 36, 4);
      if (total == 0 || total > std::numeric_limits<u32>::max() ||
          count != (total + fragment_size - 1) / fragment_size || count > rows.size() - start) return false;
      ResourceSeedArtifact artifact;
      std::copy_n(reinterpret_cast<const platform::byte*>(first.data() + 8), 16,
                  artifact.artifact_uuid.bytes.begin());
      if (!IdentityValid(artifact.artifact_uuid) || !identities.insert(artifact.artifact_uuid).second) return false;
      // Check every supplied fragment before allocating the declared body.
      for (u64 index = 0; index < count; ++index) {
        const auto row = rows[start + index];
        const auto length = std::min<u64>(fragment_size, total - index * fragment_size);
        if (row.size() != header_size + length || row.substr(0, 4) != "RSAC" ||
            Number(row, 4, 2) != 1 || Number(row, 6, 2) != 0 || Number(row, 44, 4) != 0 ||
            row.substr(8, 16) != first.substr(8, 16) || Number(row, 24, 8) != total ||
            Number(row, 32, 4) != index || Number(row, 36, 4) != count || Number(row, 40, 4) != length) return false;
      }
      std::string body; body.reserve(static_cast<std::size_t>(total));
      for (u64 index = 0; index < count; ++index) body.append(rows[start + index].substr(header_size));
      if (body.size() < 4 || Number(body, 2, 2) != 0 ||
          Number(body, 0, 2) >= static_cast<u16>(ResourceSeedFamily::unknown)) return false;
      artifact.family = static_cast<ResourceSeedFamily>(Number(body, 0, 2));
      std::size_t pos = 4;
      for (auto* field : {&artifact.canonical_path, &artifact.source_pattern,
                          &artifact.required_catalog_rows, &artifact.create_time_action,
                          &artifact.content_hash}) {
        if (!Text(body, &pos, field)) return false;
      }
      if (body.size() - pos < 8) return false;
      artifact.content_size_bytes = Number(body, pos, 8); pos += 8;
      if (artifact.content_size_bytes != body.size() - pos) return false;
      artifact.content = std::make_shared<const std::string>(body.substr(pos));
      artifact.status = ResourceSeedArtifactStatus::loaded;
      if (!ValidateResourceSeedArtifactContent(artifact)) return false;
      staged.push_back(std::move(artifact)); start += static_cast<std::size_t>(count);
    }
    output->swap(staged); return true;
  } catch (const std::bad_alloc&) { return false; }
    catch (const std::length_error&) { return false; }
}
}  // namespace scratchbird::core::resources

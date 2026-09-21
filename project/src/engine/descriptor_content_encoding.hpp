// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "internal_api/api_types.hpp"
#include "../core/hash/hash_digest.hpp"

#include <exception>
#include <string_view>

namespace scratchbird::engine::metadata {

class ContentHashFailure final : public std::exception {
 public:
  const char* what() const noexcept override {
    return "prepared content SHA-256 calculation failed";
  }
};

// Shared SBPD content encoding. This hashes metadata, not UUID spellings,
// and never issues identity or catalog/statement-use authority.
class ContentEncoder {
 public:
  explicit ContentEncoder(std::uint8_t domain)
      : bytes_{'S', 'B', 'P', 'D', static_cast<std::uint8_t>(domain == 1 ? 1 : 3), domain} {}

  void Number(std::uint64_t value) {
    for (unsigned i = 0; i != 8; ++i) {
      bytes_.push_back(static_cast<std::uint8_t>(value));
      value >>= 8;
    }
  }
  void Text(std::string_view value) {
    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
    Number(value.size());
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }
  void Uuid(const internal_api::EngineUuid& value) {
    bytes_.insert(bytes_.end(), value.bytes.begin(), value.bytes.end());
  }
  void Descriptor(const internal_api::EngineDescriptor& value) {
    Uuid(value.descriptor_uuid);
    Uuid(value.datatype_descriptor_uuid);
    Number(value.datatype_descriptor_generation);
    Uuid(value.type_uuid);
    Uuid(value.collation_uuid);
    Uuid(value.charset_uuid);
    Text(value.descriptor_kind);
    Text(value.canonical_type_name);
    Text(value.encoded_descriptor);
  }
  std::string Digest() const {
    const auto digest = core::hash::ComputeSha256Digest(bytes_);
    if (!digest.ok() || digest.digest_bytes != core::hash::kSha256DigestBytes)
      throw ContentHashFailure{};
    return "sha256:" + core::hash::HexLower(digest.digest);
  }

 private:
  std::vector<core::platform::byte> bytes_;
};

inline std::string DescriptorSetDigest(
    const std::vector<internal_api::EngineDescriptor>& descriptors,
    const std::vector<internal_api::EngineColumnDefinition>& columns) {
  ContentEncoder encoded(2);
  encoded.Number(descriptors.size());
  for (const auto& descriptor : descriptors) encoded.Descriptor(descriptor);
  encoded.Number(columns.size());
  for (const auto& column : columns) {
    encoded.Uuid(column.requested_column_uuid);
    encoded.Number(column.ordinal);
    encoded.Descriptor(column.descriptor);
    encoded.Number(column.nullable ? 1 : 0);
  }
  return encoded.Digest();
}
}  // namespace scratchbird::engine::metadata

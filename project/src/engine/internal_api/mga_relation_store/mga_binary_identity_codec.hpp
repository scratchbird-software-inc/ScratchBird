// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"
#include "mga_binary_fields.hpp"
#include "../../../core/uuid/uuid.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace scratchbird::engine::internal_api {

// Binary row/version authority only. The string is an encoded-byte buffer,
// not a UUID spelling. User UUID values use their datatype codec and policy.
// No allocation or destination modification occurs for invalid identities.
inline bool AppendBinaryEngineUuid(std::string* out, const EngineUuid& identity, bool optional = false) {
  if (out == nullptr ||
      (!scratchbird::core::uuid::IsEngineIdentityUuid(identity) && !(optional && identity.is_nil())) ||
      out->max_size() - out->size() < identity.bytes.size()) return false;
  out->append(reinterpret_cast<const char*>(identity.bytes.data()),
              identity.bytes.size());
  return true;
}

// Exactly sixteen network-order octets. Rejection preserves both the cursor
// and destination, including an out-of-range cursor near SIZE_MAX.
inline bool ReadBinaryEngineUuid(std::span<const std::uint8_t> bytes,
                                 std::size_t* offset, EngineUuid* out, bool optional = false) {
  if (offset == nullptr || out == nullptr || *offset > bytes.size() ||
      bytes.size() - *offset < 16) return false;
  EngineUuid candidate;
  std::copy_n(bytes.data() + *offset, candidate.bytes.size(), candidate.bytes.begin());
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(candidate) && !(optional && candidate.is_nil())) return false;
  *out = candidate;
  *offset += candidate.bytes.size();
  return true;
}

// SBDT revision1: exact48-byte column datatype binding. This is a metadata
// field value, not a new journal or an issuer of catalog authority.
inline bool HasMgaColumnDatatypeBinding(const EngineDescriptor& value) noexcept {
  return core::uuid::IsEngineIdentityUuid(value.datatype_descriptor_uuid) &&
      value.datatype_descriptor_generation != 0 && core::uuid::IsEngineIdentityUuid(value.type_uuid);
}
inline bool HasMgaColumnResourceIdentities(const EngineDescriptor& value) noexcept {
  return (value.charset_uuid.is_nil() || core::uuid::IsEngineIdentityUuid(value.charset_uuid)) &&
      (value.collation_uuid.is_nil() ||
       (!value.charset_uuid.is_nil() && core::uuid::IsEngineIdentityUuid(value.collation_uuid)));
}

// A string here owns raw binary octets, not a textual UUID representation.
inline bool EncodeMgaIdentityField(const EngineUuid& identity, bool optional, std::string* output) {
  if (!output || !(core::uuid::IsEngineIdentityUuid(identity) || (optional && identity.is_nil()))) return false;
  std::string staged(reinterpret_cast<const char*>(identity.bytes.data()), identity.bytes.size());
  output->swap(staged); return true;
}
inline bool ReadMgaIdentityField(std::span<const std::pair<std::string, std::string>> fields,
    std::string_view key, bool optional, EngineUuid* output) noexcept {
  if (!output) return false;
  const std::string* bytes = nullptr;
  for (const auto& field : fields) {
    if (field.first != key) continue;
    if (bytes) return false;
    bytes = &field.second;
  }
  if (!bytes || bytes->size() != 16) return false;
  EngineUuid staged;
  std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes->data()), 16, staged.bytes.begin());
  if (!(core::uuid::IsEngineIdentityUuid(staged) || (optional && staged.is_nil()))) return false;
  *output = staged; return true;
}
inline bool HasMgaBoundColumnIdentities(std::span<const EngineColumnDefinition> columns) noexcept {
  if (columns.empty()) return false;
  for (std::size_t i = 0; i < columns.size(); ++i) {
    const auto& column = columns[i];
    if (column.ordinal != i || !core::uuid::IsEngineIdentityUuid(column.requested_column_uuid) ||
        !core::uuid::IsEngineIdentityUuid(column.descriptor.descriptor_uuid) ||
        !HasMgaColumnResourceIdentities(column.descriptor) ||
        !HasMgaColumnDatatypeBinding(column.descriptor)) return false;
    for (std::size_t prior = 0; prior < i; ++prior) {
      if (columns[prior].requested_column_uuid == column.requested_column_uuid) return false;
    }
  }
  return true;
}
inline bool EncodeMgaColumnDatatypeBinding(const EngineDescriptor& value, std::string* output) {
  if (!output || !HasMgaColumnDatatypeBinding(value)) return false;
  std::string staged("SBDT\x01\x00\x30\x00", 8);
  staged.reserve(48);
  if (!AppendBinaryEngineUuid(&staged, value.datatype_descriptor_uuid)) return false;
  for (unsigned i = 0; i < 8; ++i)
    staged.push_back(static_cast<char>(value.datatype_descriptor_generation >> (8 * i)));
  if (!AppendBinaryEngineUuid(&staged, value.type_uuid)) return false;
  output->swap(staged); return true;
}
inline bool DecodeMgaColumnDatatypeBinding(std::span<const std::uint8_t> bytes,
                                           EngineDescriptor* output) noexcept {
  constexpr std::array<std::uint8_t, 8> prefix{'S','B','D','T',1,0,48,0};
  if (!output || bytes.size() != 48 || !std::equal(prefix.begin(), prefix.end(), bytes.begin())) return false;
  EngineUuid descriptor, type;
  std::size_t cursor = 8;
  if (!ReadBinaryEngineUuid(bytes, &cursor, &descriptor)) return false;
  std::uint64_t generation = 0;
  for (unsigned i = 0; i < 8; ++i) generation |= std::uint64_t(bytes[cursor++]) << (8 * i);
  if (!generation || !ReadBinaryEngineUuid(bytes, &cursor, &type)) return false;
  // Do not copy/replace unrelated occurrence, shape or rendering metadata.
  output->datatype_descriptor_uuid = descriptor;
  output->datatype_descriptor_generation = generation;
  output->type_uuid = type;
  return true;
}

inline bool ReadMgaColumnDatatypeBindingField(
    std::span<const std::pair<std::string, std::string>> fields,
    std::string_view column_prefix, EngineDescriptor* output) noexcept {
  constexpr std::string_view key = "datatype_binding_v1";
  const std::string* binding = nullptr;
  for (const auto& field : fields) {
    const std::string_view name = field.first;
    if (!name.starts_with(column_prefix) || name.substr(column_prefix.size()) != key) continue;
    if (binding) return false;
    binding = &field.second;
  }
  return binding && DecodeMgaColumnDatatypeBinding(
      {reinterpret_cast<const std::uint8_t*>(binding->data()), binding->size()}, output);
}

}  // namespace scratchbird::engine::internal_api

// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "relational_descriptor_codec.hpp"
#include "core/datatypes/canonical_utf8.hpp"
#include "core/uuid/uuid.hpp"

#include <algorithm>
#include <string_view>
#include <type_traits>

namespace scratchbird::engine::sblr {
namespace {
using Descriptor = internal_api::RelationalTypeDescriptor;
using Uuid = core::platform::Uuid;
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint16_t kAuthority = 1, kCollation = 2, kTimezone = 4;
constexpr std::uint16_t kWidth = 8, kPrecision = 16, kScale = 32;
bool TextValid(std::string_view value, std::size_t maximum) noexcept {
  return !value.empty() && value.size() <= maximum &&
      core::datatypes::ValidateCanonicalUtf8(
          reinterpret_cast<const std::uint8_t*>(value.data()), value.size()) &&
      std::ranges::none_of(value, [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}
void Put(Bytes& output, std::size_t offset, std::uint64_t value, unsigned size) {
  for (unsigned index = 0; index < size; ++index)
    output[offset + index] = static_cast<std::uint8_t>(value >> (8 * index));
}
std::uint64_t Get(const std::uint8_t* data, std::size_t offset, unsigned size) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < size; ++index)
    value |= std::uint64_t(data[offset + index]) << (8 * index);
  return value;
}
Uuid GetUuid(const std::uint8_t* data, std::size_t offset) {
  Uuid uuid;
  std::copy_n(data + offset, 16, uuid.bytes.begin());
  return uuid;
}
void PutUuid(Bytes& output, std::size_t offset, const Uuid& uuid) {
  std::copy(uuid.bytes.begin(), uuid.bytes.end(), output.begin() + offset);
}
}

bool ValidateRelationalTypeDescriptorV1(const Descriptor& value) noexcept {
  if (value.descriptor_id == 0 ||
      !core::uuid::IsEngineIdentityUuid(value.descriptor_uuid) ||
      !core::uuid::IsEngineIdentityUuid(value.type_uuid) ||
      (value.collation_uuid && !core::uuid::IsEngineIdentityUuid(*value.collation_uuid)) ||
      (value.timezone_profile_id && !TextValid(*value.timezone_profile_id, 1024)) ||
      (value.scale && (!value.precision || *value.scale > *value.precision))) return false;
  const auto nullability = static_cast<std::uint8_t>(value.nullability);
  if (nullability < 1 || nullability > 3) return false;
  if (value.datatype_identity_authoritative) {
    return nullability != 3 && value.descriptor_generation != 0 &&
        value.type_generation != 0 && value.codec_generation != 0 &&
        value.datatype_catalog_generation != 0 && value.datatype_registry_generation != 0 &&
        value.codec_version != 0 && TextValid(value.codec_id, 256) &&
        core::uuid::IsEngineIdentityUuid(value.statement_receipt_uuid) &&
        core::uuid::IsEngineIdentityUuid(value.datatype_catalog_snapshot_uuid);
  }
  return value.descriptor_generation == 0 && value.type_generation == 0 &&
      value.codec_generation == 0 && value.datatype_catalog_generation == 0 &&
      value.datatype_registry_generation == 0 && value.codec_version == 0 &&
      value.codec_id.empty() && value.statement_receipt_uuid.is_nil() &&
      value.datatype_catalog_snapshot_uuid.is_nil();
}

bool EncodeRelationalTypeDescriptorV1(const Descriptor& value, Bytes* output) {
  if (output == nullptr || !ValidateRelationalTypeDescriptorV1(value)) return false;
  const std::size_t timezone_size = value.timezone_profile_id ? value.timezone_profile_id->size() : 0;
  Bytes encoded(kRelationalDescriptorFixedBytesV1 + value.codec_id.size() + timezone_size, 0);
  Put(encoded, 0, 1, 2);
  const std::uint16_t flags = (value.datatype_identity_authoritative ? kAuthority : 0) |
      (value.collation_uuid ? kCollation : 0) | (value.timezone_profile_id ? kTimezone : 0) |
      (value.width ? kWidth : 0) | (value.precision ? kPrecision : 0) | (value.scale ? kScale : 0);
  Put(encoded, 2, flags, 2);
  Put(encoded, 4, value.descriptor_id, 4);
  PutUuid(encoded, 8, value.descriptor_uuid);
  PutUuid(encoded, 24, value.type_uuid);
  if (value.collation_uuid) PutUuid(encoded, 40, *value.collation_uuid);
  PutUuid(encoded, 56, value.statement_receipt_uuid);
  PutUuid(encoded, 72, value.datatype_catalog_snapshot_uuid);
  Put(encoded, 88, value.descriptor_generation, 8);
  Put(encoded, 96, value.type_generation, 8);
  Put(encoded, 104, value.codec_generation, 8);
  Put(encoded, 112, value.datatype_catalog_generation, 8);
  Put(encoded, 120, value.datatype_registry_generation, 8);
  Put(encoded, 128, value.codec_version, 2);
  Put(encoded, 130, static_cast<std::uint8_t>(value.nullability), 1);
  Put(encoded, 132, value.width.value_or(0), 4);
  Put(encoded, 136, value.precision.value_or(0), 4);
  Put(encoded, 140, value.scale.value_or(0), 4);
  Put(encoded, 144, value.codec_id.size(), 4);
  Put(encoded, 148, timezone_size, 4);
  auto cursor = std::copy(value.codec_id.begin(), value.codec_id.end(), encoded.begin() + 152);
  if (value.timezone_profile_id)
    std::copy(value.timezone_profile_id->begin(), value.timezone_profile_id->end(), cursor);
  output->swap(encoded);
  return true;
}

bool DecodeRelationalTypeDescriptorV1(const std::uint8_t* data, std::size_t size, Descriptor* output) {
  if (output == nullptr || data == nullptr || size < kRelationalDescriptorFixedBytesV1 ||
      size > kRelationalDescriptorMaximumBytesV1 || Get(data, 0, 2) != 1 || data[131] != 0) return false;
  const auto flags = Get(data, 2, 2);
  const auto codec_size = Get(data, 144, 4), timezone_size = Get(data, 148, 4);
  if ((flags & ~std::uint64_t{63}) != 0 || codec_size > 256 || timezone_size > 1024 ||
      size != 152 + codec_size + timezone_size ||
      (!(flags & kTimezone) && timezone_size != 0) ||
      (!(flags & kCollation) && !GetUuid(data, 40).is_nil()) ||
      (!(flags & kWidth) && Get(data, 132, 4) != 0) ||
      (!(flags & kPrecision) && Get(data, 136, 4) != 0) ||
      (!(flags & kScale) && Get(data, 140, 4) != 0)) return false;
  Descriptor decoded;
  decoded.descriptor_id = static_cast<std::uint32_t>(Get(data, 4, 4));
  decoded.descriptor_uuid = GetUuid(data, 8);
  decoded.type_uuid = GetUuid(data, 24);
  if (flags & kCollation) decoded.collation_uuid = GetUuid(data, 40);
  decoded.statement_receipt_uuid = GetUuid(data, 56);
  decoded.datatype_catalog_snapshot_uuid = GetUuid(data, 72);
  decoded.descriptor_generation = Get(data, 88, 8);
  decoded.type_generation = Get(data, 96, 8);
  decoded.codec_generation = Get(data, 104, 8);
  decoded.datatype_catalog_generation = Get(data, 112, 8);
  decoded.datatype_registry_generation = Get(data, 120, 8);
  decoded.codec_version = static_cast<std::uint16_t>(Get(data, 128, 2));
  decoded.nullability = static_cast<internal_api::RelationalNullability>(data[130]);
  decoded.datatype_identity_authoritative = (flags & kAuthority) != 0;
  if (flags & kWidth) decoded.width = static_cast<std::uint32_t>(Get(data, 132, 4));
  if (flags & kPrecision) decoded.precision = static_cast<std::uint32_t>(Get(data, 136, 4));
  if (flags & kScale) decoded.scale = static_cast<std::uint32_t>(Get(data, 140, 4));
  decoded.codec_id.assign(reinterpret_cast<const char*>(data + 152), static_cast<std::size_t>(codec_size));
  if (flags & kTimezone)
    decoded.timezone_profile_id.emplace(reinterpret_cast<const char*>(data + 152 + codec_size),
                                        static_cast<std::size_t>(timezone_size));
  if (!ValidateRelationalTypeDescriptorV1(decoded)) return false;
  static_assert(std::is_nothrow_move_assignable_v<Descriptor>);
  *output = std::move(decoded);
  return true;
}
}  // namespace scratchbird::engine::sblr

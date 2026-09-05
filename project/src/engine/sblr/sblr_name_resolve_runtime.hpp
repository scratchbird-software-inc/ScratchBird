#pragma once

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

inline constexpr std::uint16_t kSblrNameResolveOpcodeCode = 4865;
inline constexpr std::size_t kSblrNameResolveRequestBytes = 64;
inline constexpr std::size_t kSblrNameResolveDescriptorPrefixBytes = 256;
inline constexpr std::size_t kSblrNameResolveResultBytes = 192;
inline constexpr std::size_t kSblrNameResolveMaximumCanonicalNameBytes = 4096;

using SblrNameResolveUuidV1 = std::array<std::uint8_t, 16>;
using SblrNameResolveSha256V1 = std::array<std::uint8_t, 32>;

struct SblrNameResolveRequestV1 {
  SblrNameResolveUuidV1 statement_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
};

struct SblrNameResolveDescriptorV1 {
  SblrNameResolveUuidV1 resolution_uuid{};
  SblrNameResolveUuidV1 statement_receipt_uuid{};
  SblrNameResolveUuidV1 catalog_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  SblrNameResolveUuidV1 security_context_uuid{};
  SblrNameResolveUuidV1 namespace_uuid{};
  std::uint64_t namespace_generation = 0;
  std::string canonical_name_utf8;
  SblrNameResolveSha256V1 canonical_name_sha256{};
  std::uint8_t resolution_mode = 0;
  std::uint8_t object_class = 0;
  std::uint8_t case_folding_profile = 0;
  std::uint64_t executor_availability_generation = 0;
  SblrNameResolveSha256V1 descriptor_sha256{};
  SblrNameResolveUuidV1 parser_package_uuid{};
  SblrNameResolveUuidV1 language_profile_uuid{};
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
};

struct SblrNameResolveResultV1 {
  SblrNameResolveUuidV1 resolution_uuid{};
  SblrNameResolveUuidV1 resolved_object_uuid{};
  SblrNameResolveUuidV1 resolved_namespace_uuid{};
  std::uint64_t object_descriptor_generation = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  SblrNameResolveUuidV1 redaction_profile_uuid{};
  std::uint8_t status = 0;
  std::uint8_t visibility = 0;
  std::uint16_t object_class = 0;
  SblrNameResolveUuidV1 publication_evidence_uuid{};
  SblrNameResolveSha256V1 resolution_material_sha256{};
  SblrNameResolveSha256V1 executor_evidence_sha256{};
};

namespace name_resolve_detail {

inline void PutLe(std::vector<std::uint8_t>* out, std::uint64_t value,
                  std::size_t bytes) {
  for (std::size_t index = 0; index < bytes; ++index) {
    out->push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

inline std::uint64_t GetLe(const std::uint8_t* in, std::size_t bytes) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < bytes; ++index) {
    value |= static_cast<std::uint64_t>(in[index]) << (index * 8U);
  }
  return value;
}

template <std::size_t N>
inline bool NonZero(const std::array<std::uint8_t, N>& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

template <std::size_t N>
inline void Put(std::vector<std::uint8_t>* out,
                const std::array<std::uint8_t, N>& value) {
  out->insert(out->end(), value.begin(), value.end());
}

template <std::size_t N>
inline void Get(const std::uint8_t* in, std::array<std::uint8_t, N>* value) {
  std::copy_n(in, N, value->begin());
}

inline bool Zero(const std::uint8_t* begin, const std::uint8_t* end) {
  return std::all_of(begin, end,
                     [](std::uint8_t byte) { return byte == 0; });
}

inline bool Utf8(std::string_view text) {
  if (text.empty() || text.size() > kSblrNameResolveMaximumCanonicalNameBytes ||
      text.find('\0') != std::string_view::npos) {
    return false;
  }
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto lead = bytes[offset++];
    if (lead < 0x80) continue;
    if (lead >= 0xc2 && lead <= 0xdf) {
      if (offset >= text.size() || (bytes[offset++] & 0xc0U) != 0x80U)
        return false;
      continue;
    }
    if (lead >= 0xe0 && lead <= 0xef) {
      if (offset + 1 >= text.size() ||
          (bytes[offset] & 0xc0U) != 0x80U ||
          (bytes[offset + 1] & 0xc0U) != 0x80U ||
          (lead == 0xe0 && bytes[offset] < 0xa0U) ||
          (lead == 0xed && bytes[offset] >= 0xa0U)) {
        return false;
      }
      offset += 2;
      continue;
    }
    if (lead >= 0xf0 && lead <= 0xf4) {
      if (offset + 2 >= text.size() ||
          (bytes[offset] & 0xc0U) != 0x80U ||
          (bytes[offset + 1] & 0xc0U) != 0x80U ||
          (bytes[offset + 2] & 0xc0U) != 0x80U ||
          (lead == 0xf0 && bytes[offset] < 0x90U) ||
          (lead == 0xf4 && bytes[offset] >= 0x90U)) {
        return false;
      }
      offset += 3;
      continue;
    }
    return false;
  }
  return true;
}

inline SblrNameResolveSha256V1 Hash(const std::uint8_t* bytes,
                                    std::size_t size) {
  std::vector<std::uint8_t> material;
  if (size != 0) material.assign(bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

inline std::vector<std::uint8_t> Header(std::string_view magic,
                                        std::size_t header_bytes,
                                        std::size_t total_bytes) {
  std::vector<std::uint8_t> out(magic.begin(), magic.end());
  PutLe(&out, 1, 2);
  PutLe(&out, header_bytes, 2);
  PutLe(&out, total_bytes, 4);
  PutLe(&out, 0, 4);
  return out;
}

inline bool HeaderValid(const std::uint8_t* in, std::size_t size,
                        std::string_view magic, std::size_t header_bytes,
                        bool fixed) {
  return in != nullptr && size >= header_bytes && magic.size() == 4 &&
         std::equal(magic.begin(), magic.end(), in) &&
         GetLe(in + 4, 2) == 1 && GetLe(in + 6, 2) == header_bytes &&
         GetLe(in + 8, 4) == size && Zero(in + 12, in + 16) &&
         (!fixed || size == header_bytes);
}

inline bool DescriptorFieldsValid(const SblrNameResolveDescriptorV1& value) {
  const bool namespace_present = NonZero(value.namespace_uuid);
  if (namespace_present != (value.namespace_generation != 0) ||
      (value.resolution_mode == 1 && !namespace_present)) {
    return false;
  }
  return NonZero(value.resolution_uuid) &&
         NonZero(value.statement_receipt_uuid) &&
         NonZero(value.catalog_snapshot_uuid) &&
         value.catalog_generation != 0 &&
         NonZero(value.security_context_uuid) &&
         Utf8(value.canonical_name_utf8) &&
         (value.resolution_mode == 1 || value.resolution_mode == 2) &&
         value.object_class <= 16 &&
         value.case_folding_profile == 1 &&
         value.executor_availability_generation != 0 &&
         NonZero(value.parser_package_uuid) &&
         NonZero(value.language_profile_uuid) && value.security_epoch != 0 &&
         value.resource_epoch != 0;
}

inline bool ResultFieldsValid(const SblrNameResolveResultV1& value) {
  if (!NonZero(value.resolution_uuid) || value.catalog_generation == 0 ||
      value.security_epoch == 0 || !NonZero(value.redaction_profile_uuid) ||
      !NonZero(value.publication_evidence_uuid) ||
      !NonZero(value.resolution_material_sha256) ||
      !NonZero(value.executor_evidence_sha256)) {
    return false;
  }
  if (value.status == 1 && value.visibility == 1) {
    return NonZero(value.resolved_object_uuid) &&
           NonZero(value.resolved_namespace_uuid) &&
           value.object_descriptor_generation != 0;
  }
  if (value.status == 2 && value.visibility == 2) {
    return !NonZero(value.resolved_object_uuid) &&
           !NonZero(value.resolved_namespace_uuid) &&
           value.object_descriptor_generation == 0;
  }
  return false;
}

}  // namespace name_resolve_detail

inline std::vector<std::uint8_t> EncodeSblrNameResolveRequestV1(
    const SblrNameResolveRequestV1& value) {
  using namespace name_resolve_detail;
  if (!NonZero(value.statement_receipt_uuid) || value.occurrence == 0 ||
      value.catalog_generation == 0 || value.security_epoch == 0 ||
      value.resource_epoch == 0) {
    return {};
  }
  auto out = Header("SBNQ", kSblrNameResolveRequestBytes,
                    kSblrNameResolveRequestBytes);
  Put(&out, value.statement_receipt_uuid);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  return out;
}

inline bool DecodeSblrNameResolveRequestV1(
    const std::uint8_t* in, std::size_t size, SblrNameResolveRequestV1* output,
    std::string* detail = nullptr) {
  using namespace name_resolve_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "SBNQ", kSblrNameResolveRequestBytes, true)) {
    if (detail != nullptr) *detail = "name_resolve_request_header_invalid";
    return false;
  }
  SblrNameResolveRequestV1 value;
  Get(in + 16, &value.statement_receipt_uuid);
  value.occurrence = GetLe(in + 32, 8);
  value.catalog_generation = GetLe(in + 40, 8);
  value.security_epoch = GetLe(in + 48, 8);
  value.resource_epoch = GetLe(in + 56, 8);
  if (EncodeSblrNameResolveRequestV1(value) !=
      std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "name_resolve_request_fields_invalid";
    return false;
  }
  *output = std::move(value);
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrNameResolveDescriptorV1(
    const SblrNameResolveDescriptorV1& value) {
  using namespace name_resolve_detail;
  if (!DescriptorFieldsValid(value) ||
      value.canonical_name_utf8.size() >
          std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  const auto name_hash = Hash(
      reinterpret_cast<const std::uint8_t*>(value.canonical_name_utf8.data()),
      value.canonical_name_utf8.size());
  if (NonZero(value.canonical_name_sha256) &&
      value.canonical_name_sha256 != name_hash) {
    return {};
  }
  auto out = Header("SNRD", kSblrNameResolveDescriptorPrefixBytes,
                    kSblrNameResolveDescriptorPrefixBytes +
                        value.canonical_name_utf8.size());
  Put(&out, value.resolution_uuid);
  Put(&out, value.statement_receipt_uuid);
  Put(&out, value.catalog_snapshot_uuid);
  PutLe(&out, value.catalog_generation, 8);
  Put(&out, value.security_context_uuid);
  Put(&out, value.namespace_uuid);
  PutLe(&out, value.namespace_generation, 8);
  PutLe(&out, value.canonical_name_utf8.size(), 4);
  Put(&out, name_hash);
  out.push_back(value.resolution_mode);
  out.push_back(value.object_class);
  out.push_back(value.case_folding_profile);
  out.push_back(0);
  PutLe(&out, value.executor_availability_generation, 8);
  out.insert(out.end(), 32, 0);
  Put(&out, value.parser_package_uuid);
  Put(&out, value.language_profile_uuid);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  out.insert(out.end(), 16, 0);
  out.insert(out.end(), value.canonical_name_utf8.begin(),
             value.canonical_name_utf8.end());
  const auto descriptor_hash = Hash(out.data(), out.size());
  if (NonZero(value.descriptor_sha256) &&
      value.descriptor_sha256 != descriptor_hash) {
    return {};
  }
  std::copy(descriptor_hash.begin(), descriptor_hash.end(), out.begin() + 160);
  return out;
}

inline bool DecodeSblrNameResolveDescriptorV1(
    const std::uint8_t* in, std::size_t size,
    SblrNameResolveDescriptorV1* output, std::string* detail = nullptr) {
  using namespace name_resolve_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "SNRD", kSblrNameResolveDescriptorPrefixBytes,
                   false) ||
      in[151] != 0 || !Zero(in + 240, in + 256)) {
    if (detail != nullptr) *detail = "name_resolve_descriptor_header_invalid";
    return false;
  }
  const auto name_size = GetLe(in + 112, 4);
  if (name_size == 0 || name_size > kSblrNameResolveMaximumCanonicalNameBytes ||
      name_size != size - kSblrNameResolveDescriptorPrefixBytes) {
    if (detail != nullptr) *detail = "name_resolve_descriptor_extent_invalid";
    return false;
  }
  SblrNameResolveDescriptorV1 value;
  Get(in + 16, &value.resolution_uuid);
  Get(in + 32, &value.statement_receipt_uuid);
  Get(in + 48, &value.catalog_snapshot_uuid);
  value.catalog_generation = GetLe(in + 64, 8);
  Get(in + 72, &value.security_context_uuid);
  Get(in + 88, &value.namespace_uuid);
  value.namespace_generation = GetLe(in + 104, 8);
  Get(in + 116, &value.canonical_name_sha256);
  value.resolution_mode = in[148];
  value.object_class = in[149];
  value.case_folding_profile = in[150];
  value.executor_availability_generation = GetLe(in + 152, 8);
  Get(in + 160, &value.descriptor_sha256);
  Get(in + 192, &value.parser_package_uuid);
  Get(in + 208, &value.language_profile_uuid);
  value.security_epoch = GetLe(in + 224, 8);
  value.resource_epoch = GetLe(in + 232, 8);
  value.canonical_name_utf8.assign(
      reinterpret_cast<const char*>(in + kSblrNameResolveDescriptorPrefixBytes),
      name_size);
  auto hash_material = std::vector<std::uint8_t>(in, in + size);
  std::fill(hash_material.begin() + 160, hash_material.begin() + 192, 0);
  if (!DescriptorFieldsValid(value) ||
      value.canonical_name_sha256 !=
          Hash(in + kSblrNameResolveDescriptorPrefixBytes, name_size) ||
      value.descriptor_sha256 !=
          Hash(hash_material.data(), hash_material.size()) ||
      EncodeSblrNameResolveDescriptorV1(value) !=
          std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "name_resolve_descriptor_fields_invalid";
    return false;
  }
  *output = std::move(value);
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrNameResolveResultV1(
    const SblrNameResolveResultV1& value) {
  using namespace name_resolve_detail;
  if (!ResultFieldsValid(value)) return {};
  auto out = Header("SNRR", kSblrNameResolveResultBytes,
                    kSblrNameResolveResultBytes);
  Put(&out, value.resolution_uuid);
  Put(&out, value.resolved_object_uuid);
  Put(&out, value.resolved_namespace_uuid);
  PutLe(&out, value.object_descriptor_generation, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  Put(&out, value.redaction_profile_uuid);
  out.push_back(value.status);
  out.push_back(value.visibility);
  PutLe(&out, value.object_class, 2);
  PutLe(&out, 0, 4);
  Put(&out, value.publication_evidence_uuid);
  Put(&out, value.resolution_material_sha256);
  Put(&out, value.executor_evidence_sha256);
  return out;
}

inline bool DecodeSblrNameResolveResultV1(
    const std::uint8_t* in, std::size_t size, SblrNameResolveResultV1* output,
    std::string* detail = nullptr) {
  using namespace name_resolve_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "SNRR", kSblrNameResolveResultBytes, true) ||
      !Zero(in + 108, in + 112)) {
    if (detail != nullptr) *detail = "name_resolve_result_header_invalid";
    return false;
  }
  SblrNameResolveResultV1 value;
  Get(in + 16, &value.resolution_uuid);
  Get(in + 32, &value.resolved_object_uuid);
  Get(in + 48, &value.resolved_namespace_uuid);
  value.object_descriptor_generation = GetLe(in + 64, 8);
  value.catalog_generation = GetLe(in + 72, 8);
  value.security_epoch = GetLe(in + 80, 8);
  Get(in + 88, &value.redaction_profile_uuid);
  value.status = in[104];
  value.visibility = in[105];
  value.object_class = static_cast<std::uint16_t>(GetLe(in + 106, 2));
  Get(in + 112, &value.publication_evidence_uuid);
  Get(in + 128, &value.resolution_material_sha256);
  Get(in + 160, &value.executor_evidence_sha256);
  if (!ResultFieldsValid(value) ||
      EncodeSblrNameResolveResultV1(value) !=
          std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "name_resolve_result_fields_invalid";
    return false;
  }
  *output = std::move(value);
  return true;
}

inline SblrNameResolveSha256V1 ComputeSblrNameResolveEvidenceV1(
    std::string_view domain, const std::vector<std::uint8_t>& material) {
  std::vector<std::uint8_t> bytes(domain.begin(), domain.end());
  bytes.insert(bytes.end(), material.begin(), material.end());
  return name_resolve_detail::Hash(bytes.data(), bytes.size());
}

}  // namespace scratchbird::engine::sblr

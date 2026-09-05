// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

inline constexpr std::uint16_t kSblrDatabaseAttachOpcodeCode = 5120;
inline constexpr std::size_t kSblrDatabaseAttachRequestBytes = 64;
inline constexpr std::size_t kSblrDatabaseAttachDescriptorBytes = 288;
inline constexpr std::size_t kSblrDatabaseAttachResultBytes = 192;

using SblrDatabaseAttachUuidV1 = std::array<std::uint8_t, 16>;
using SblrDatabaseAttachSha256V1 = std::array<std::uint8_t, 32>;

struct SblrDatabaseAttachRequestV1 {
  SblrDatabaseAttachUuidV1 statement_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
};

struct SblrDatabaseAttachDescriptorV1 {
  SblrDatabaseAttachUuidV1 attach_uuid{};
  SblrDatabaseAttachUuidV1 statement_receipt_uuid{};
  SblrDatabaseAttachUuidV1 storage_uuid{};
  SblrDatabaseAttachUuidV1 alias_uuid{};
  SblrDatabaseAttachUuidV1 database_uuid{};
  SblrDatabaseAttachUuidV1 catalog_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  SblrDatabaseAttachUuidV1 security_context_uuid{};
  SblrDatabaseAttachUuidV1 policy_snapshot_uuid{};
  std::uint64_t policy_generation = 0;
  SblrDatabaseAttachUuidV1 transaction_uuid{};
  std::uint64_t transaction_generation = 0;
  std::uint8_t mode = 0;         // 1 read_only, 2 read_write.
  std::uint8_t alias_scope = 0;  // 1 session, 2 database.
  std::uint64_t executor_availability_generation = 0;
  SblrDatabaseAttachSha256V1 descriptor_sha256{};
  SblrDatabaseAttachSha256V1 storage_alias_binding_sha256{};
};

struct SblrDatabaseAttachResultV1 {
  SblrDatabaseAttachUuidV1 attach_uuid{};
  SblrDatabaseAttachUuidV1 database_uuid{};
  SblrDatabaseAttachUuidV1 alias_uuid{};
  std::uint64_t database_generation = 0;
  SblrDatabaseAttachUuidV1 catalog_epoch_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint8_t status = 0;               // 1 attached, 2 already_attached.
  std::uint8_t lifecycle_state = 0;      // 1 ready.
  std::uint8_t publication_barrier = 0;  // 1 durable.
  SblrDatabaseAttachUuidV1 attachment_evidence_uuid{};
  SblrDatabaseAttachSha256V1 result_material_sha256{};
  SblrDatabaseAttachSha256V1 executor_evidence_sha256{};
};

namespace database_attach_detail {

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

inline SblrDatabaseAttachSha256V1 Hash(
    std::string_view domain, const std::vector<std::uint8_t>& bytes) {
  std::vector<std::uint8_t> material(domain.begin(), domain.end());
  material.insert(material.end(), bytes.begin(), bytes.end());
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

inline std::vector<std::uint8_t> Header(std::string_view magic,
                                        std::size_t bytes) {
  std::vector<std::uint8_t> out(magic.begin(), magic.end());
  PutLe(&out, 1, 2);
  PutLe(&out, bytes, 2);
  PutLe(&out, bytes, 4);
  PutLe(&out, 0, 4);
  return out;
}

inline bool HeaderValid(const std::uint8_t* in, std::size_t size,
                        std::string_view magic, std::size_t bytes) {
  return in != nullptr && size == bytes && magic.size() == 4 &&
         std::equal(magic.begin(), magic.end(), in) &&
         GetLe(in + 4, 2) == 1 && GetLe(in + 6, 2) == bytes &&
         GetLe(in + 8, 4) == bytes && GetLe(in + 12, 4) == 0;
}

inline bool DescriptorFieldsValid(const SblrDatabaseAttachDescriptorV1& value) {
  return NonZero(value.attach_uuid) &&
         NonZero(value.statement_receipt_uuid) && NonZero(value.storage_uuid) &&
         NonZero(value.alias_uuid) && NonZero(value.database_uuid) &&
         NonZero(value.catalog_snapshot_uuid) && value.catalog_generation != 0 &&
         NonZero(value.security_context_uuid) &&
         NonZero(value.policy_snapshot_uuid) && value.policy_generation != 0 &&
         NonZero(value.transaction_uuid) && value.transaction_generation != 0 &&
         (value.mode == 1 || value.mode == 2) && value.alias_scope == 1 &&
         value.executor_availability_generation != 0 &&
         NonZero(value.storage_alias_binding_sha256);
}

inline bool ResultFieldsValid(const SblrDatabaseAttachResultV1& value) {
  return NonZero(value.attach_uuid) && NonZero(value.database_uuid) &&
         NonZero(value.alias_uuid) && value.database_generation != 0 &&
         NonZero(value.catalog_epoch_uuid) && value.catalog_generation != 0 &&
         (value.status == 1 || value.status == 2) &&
         value.lifecycle_state == 1 && value.publication_barrier == 1 &&
         NonZero(value.attachment_evidence_uuid);
}

}  // namespace database_attach_detail

inline SblrDatabaseAttachSha256V1 SblrDatabaseAttachBindingSha256V1(
    const SblrDatabaseAttachUuidV1& storage_uuid,
    const SblrDatabaseAttachUuidV1& alias_uuid, std::uint8_t mode,
    std::uint8_t alias_scope) {
  using namespace database_attach_detail;
  if (!NonZero(storage_uuid) || !NonZero(alias_uuid) ||
      (mode != 1 && mode != 2) || alias_scope != 1) {
    return {};
  }
  std::vector<std::uint8_t> material;
  Put(&material, storage_uuid);
  Put(&material, alias_uuid);
  material.push_back(mode);
  material.push_back(alias_scope);
  return Hash("ScratchBird.DatabaseAttachBinding.V1", material);
}

inline std::vector<std::uint8_t> EncodeSblrDatabaseAttachRequestV1(
    const SblrDatabaseAttachRequestV1& value) {
  using namespace database_attach_detail;
  if (!NonZero(value.statement_receipt_uuid) || value.occurrence == 0 ||
      value.catalog_generation == 0 || value.security_epoch == 0 ||
      value.resource_epoch == 0) {
    return {};
  }
  auto out = Header("SBAQ", kSblrDatabaseAttachRequestBytes);
  Put(&out, value.statement_receipt_uuid);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  return out;
}

inline bool DecodeSblrDatabaseAttachRequestV1(
    const std::uint8_t* in, std::size_t size,
    SblrDatabaseAttachRequestV1* output, std::string* detail = nullptr) {
  using namespace database_attach_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "SBAQ", kSblrDatabaseAttachRequestBytes)) {
    if (detail != nullptr) *detail = "database_attach_request_header_invalid";
    return false;
  }
  SblrDatabaseAttachRequestV1 value;
  Get(in + 16, &value.statement_receipt_uuid);
  value.occurrence = GetLe(in + 32, 8);
  value.catalog_generation = GetLe(in + 40, 8);
  value.security_epoch = GetLe(in + 48, 8);
  value.resource_epoch = GetLe(in + 56, 8);
  if (EncodeSblrDatabaseAttachRequestV1(value) !=
      std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "database_attach_request_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrDatabaseAttachDescriptorV1(
    const SblrDatabaseAttachDescriptorV1& value) {
  using namespace database_attach_detail;
  if (!DescriptorFieldsValid(value)) return {};
  if (value.storage_alias_binding_sha256 !=
      SblrDatabaseAttachBindingSha256V1(value.storage_uuid, value.alias_uuid,
                                        value.mode, value.alias_scope)) {
    return {};
  }
  auto out = Header("SADD", kSblrDatabaseAttachDescriptorBytes);
  Put(&out, value.attach_uuid);
  Put(&out, value.statement_receipt_uuid);
  Put(&out, value.storage_uuid);
  Put(&out, value.alias_uuid);
  Put(&out, value.database_uuid);
  Put(&out, value.catalog_snapshot_uuid);
  PutLe(&out, value.catalog_generation, 8);
  Put(&out, value.security_context_uuid);
  Put(&out, value.policy_snapshot_uuid);
  PutLe(&out, value.policy_generation, 8);
  Put(&out, value.transaction_uuid);
  PutLe(&out, value.transaction_generation, 8);
  out.push_back(value.mode);
  out.push_back(value.alias_scope);
  PutLe(&out, 0, 2);
  PutLe(&out, value.executor_availability_generation, 8);
  out.insert(out.end(), 32, 0);
  Put(&out, value.storage_alias_binding_sha256);
  out.insert(out.end(), 28, 0);
  const auto digest = Hash("ScratchBird.SblrDatabaseAttachDescriptor.V1", out);
  if (NonZero(value.descriptor_sha256) && value.descriptor_sha256 != digest) {
    return {};
  }
  std::copy(digest.begin(), digest.end(), out.begin() + 196);
  return out;
}

inline bool DecodeSblrDatabaseAttachDescriptorV1(
    const std::uint8_t* in, std::size_t size,
    SblrDatabaseAttachDescriptorV1* output, std::string* detail = nullptr) {
  using namespace database_attach_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "SADD", kSblrDatabaseAttachDescriptorBytes) ||
      !Zero(in + 186, in + 188) || !Zero(in + 260, in + 288)) {
    if (detail != nullptr) *detail = "database_attach_descriptor_header_invalid";
    return false;
  }
  SblrDatabaseAttachDescriptorV1 value;
  Get(in + 16, &value.attach_uuid);
  Get(in + 32, &value.statement_receipt_uuid);
  Get(in + 48, &value.storage_uuid);
  Get(in + 64, &value.alias_uuid);
  Get(in + 80, &value.database_uuid);
  Get(in + 96, &value.catalog_snapshot_uuid);
  value.catalog_generation = GetLe(in + 112, 8);
  Get(in + 120, &value.security_context_uuid);
  Get(in + 136, &value.policy_snapshot_uuid);
  value.policy_generation = GetLe(in + 152, 8);
  Get(in + 160, &value.transaction_uuid);
  value.transaction_generation = GetLe(in + 176, 8);
  value.mode = in[184];
  value.alias_scope = in[185];
  value.executor_availability_generation = GetLe(in + 188, 8);
  Get(in + 196, &value.descriptor_sha256);
  Get(in + 228, &value.storage_alias_binding_sha256);
  const auto encoded = EncodeSblrDatabaseAttachDescriptorV1(value);
  if (encoded != std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "database_attach_descriptor_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrDatabaseAttachResultV1(
    const SblrDatabaseAttachResultV1& value) {
  using namespace database_attach_detail;
  if (!ResultFieldsValid(value)) return {};
  auto out = Header("SBAR", kSblrDatabaseAttachResultBytes);
  Put(&out, value.attach_uuid);
  Put(&out, value.database_uuid);
  Put(&out, value.alias_uuid);
  PutLe(&out, value.database_generation, 8);
  Put(&out, value.catalog_epoch_uuid);
  PutLe(&out, value.catalog_generation, 8);
  out.push_back(value.status);
  out.push_back(value.lifecycle_state);
  out.push_back(value.publication_barrier);
  out.push_back(0);
  Put(&out, value.attachment_evidence_uuid);
  out.insert(out.end(), 64, 0);
  out.insert(out.end(), 12, 0);
  const auto material = Hash("ScratchBird.DatabaseAttachResultMaterial.V1", out);
  if (NonZero(value.result_material_sha256) &&
      value.result_material_sha256 != material) {
    return {};
  }
  std::copy(material.begin(), material.end(), out.begin() + 116);
  const auto evidence = Hash("ScratchBird.SblrDatabaseAttachResult.V1", out);
  if (NonZero(value.executor_evidence_sha256) &&
      value.executor_evidence_sha256 != evidence) {
    return {};
  }
  std::copy(evidence.begin(), evidence.end(), out.begin() + 148);
  return out;
}

inline bool DecodeSblrDatabaseAttachResultV1(
    const std::uint8_t* in, std::size_t size,
    SblrDatabaseAttachResultV1* output, std::string* detail = nullptr) {
  using namespace database_attach_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "SBAR", kSblrDatabaseAttachResultBytes) ||
      in[99] != 0 || !Zero(in + 180, in + 192)) {
    if (detail != nullptr) *detail = "database_attach_result_header_invalid";
    return false;
  }
  SblrDatabaseAttachResultV1 value;
  Get(in + 16, &value.attach_uuid);
  Get(in + 32, &value.database_uuid);
  Get(in + 48, &value.alias_uuid);
  value.database_generation = GetLe(in + 64, 8);
  Get(in + 72, &value.catalog_epoch_uuid);
  value.catalog_generation = GetLe(in + 88, 8);
  value.status = in[96];
  value.lifecycle_state = in[97];
  value.publication_barrier = in[98];
  Get(in + 100, &value.attachment_evidence_uuid);
  Get(in + 116, &value.result_material_sha256);
  Get(in + 148, &value.executor_evidence_sha256);
  const auto encoded = EncodeSblrDatabaseAttachResultV1(value);
  if (encoded != std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "database_attach_result_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

}  // namespace scratchbird::engine::sblr

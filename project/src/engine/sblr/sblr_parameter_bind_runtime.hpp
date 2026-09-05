// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/hash/hash_digest.hpp"
#include "sblr_parameter_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

using SblrParameterBindUuidV1 = std::array<std::uint8_t, 16>;
using SblrParameterBindSha256V1 = std::array<std::uint8_t, 32>;

struct SblrParameterBindRequestV1 {
  SblrParameterBindUuidV1 statement_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
};

struct SblrParameterBindDescriptorV1 {
  SblrParameterBindUuidV1 execution_uuid{};
  SblrParameterBindUuidV1 statement_receipt_uuid{};
  SblrParameterBindUuidV1 prepared_statement_uuid{};
  std::uint64_t prepared_generation = 0;
  SblrParameterBindUuidV1 parameter_set_uuid{};
  std::uint64_t parameter_set_generation = 0;
  SblrParameterBindSha256V1 ordered_slot_table_sha256{};
  SblrParameterBindUuidV1 batch_uuid{};
  std::uint64_t batch_generation = 0;
  SblrParameterBindUuidV1 dynamic_package_uuid{};
  std::uint64_t dynamic_generation = 0;
  SblrParameterBindUuidV1 catalog_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  SblrParameterBindUuidV1 mga_snapshot_uuid{};
  std::uint64_t executor_availability_generation = 0;
  std::vector<std::uint8_t> canonical_value_vector;
};

struct SblrParameterBindResultV1 {
  SblrParameterBindUuidV1 execution_uuid{};
  SblrParameterBindUuidV1 prepared_statement_uuid{};
  std::uint64_t prepared_generation = 0;
  SblrParameterBindUuidV1 parameter_set_uuid{};
  std::uint64_t parameter_set_generation = 0;
  SblrParameterBindSha256V1 ordered_slot_table_sha256{};
  SblrParameterBindUuidV1 batch_uuid{};
  std::uint64_t batch_generation = 0;
  std::uint8_t status = 0;
  std::uint8_t publication_barrier = 0;
  SblrParameterBindUuidV1 bind_evidence_uuid{};
  SblrParameterBindSha256V1 effect_evidence_sha256{};
};

namespace parameter_bind_detail {

inline constexpr std::size_t kRequestBytes = 64;
inline constexpr std::size_t kDescriptorPrefixBytes = 256;
inline constexpr std::size_t kResultBytes = 192;
inline constexpr std::size_t kMaximumValueVectorBytes = 32U * 1024U * 1024U;

inline void PutLe(std::vector<std::uint8_t>* out, std::uint64_t value,
                  std::size_t bytes) {
  for (std::size_t i = 0; i < bytes; ++i) {
    out->push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
  }
}

inline std::uint64_t GetLe(const std::uint8_t* in, std::size_t bytes) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < bytes; ++i) {
    value |= static_cast<std::uint64_t>(in[i]) << (i * 8U);
  }
  return value;
}

template <std::size_t N>
inline bool NonZero(const std::array<std::uint8_t, N>& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

template <std::size_t N>
inline bool PairValid(const std::array<std::uint8_t, N>& uuid,
                      std::uint64_t generation) {
  return NonZero(uuid) == (generation != 0);
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
         GetLe(in + 8, 4) == size && GetLe(in + 12, 4) == 0 &&
         (!fixed || size == header_bytes);
}

inline SblrParameterBindSha256V1 Hash(const std::uint8_t* in,
                                      std::size_t size) {
  std::vector<std::uint8_t> bytes(in, in + size);
  return scratchbird::core::hash::ComputeSha256Digest(bytes).digest;
}

inline bool RequestValid(const SblrParameterBindRequestV1& value) {
  return NonZero(value.statement_receipt_uuid) && value.occurrence != 0 &&
         value.catalog_generation != 0 && value.security_epoch != 0 &&
         value.resource_epoch != 0;
}

inline bool DescriptorShapeValid(
    const SblrParameterBindDescriptorV1& value) {
  return NonZero(value.execution_uuid) &&
         NonZero(value.statement_receipt_uuid) &&
         NonZero(value.prepared_statement_uuid) &&
         value.prepared_generation != 0 && NonZero(value.parameter_set_uuid) &&
         value.parameter_set_generation != 0 &&
         NonZero(value.ordered_slot_table_sha256) &&
         PairValid(value.batch_uuid, value.batch_generation) &&
         PairValid(value.dynamic_package_uuid, value.dynamic_generation) &&
         NonZero(value.catalog_snapshot_uuid) &&
         value.catalog_generation != 0 && value.security_epoch != 0 &&
         value.resource_epoch != 0 && NonZero(value.mga_snapshot_uuid) &&
         value.executor_availability_generation != 0 &&
         !value.canonical_value_vector.empty() &&
         value.canonical_value_vector.size() <= kMaximumValueVectorBytes &&
         value.canonical_value_vector.size() <=
             std::numeric_limits<std::uint32_t>::max();
}

inline bool ResultValid(const SblrParameterBindResultV1& value) {
  return NonZero(value.execution_uuid) &&
         NonZero(value.prepared_statement_uuid) &&
         value.prepared_generation != 0 && NonZero(value.parameter_set_uuid) &&
         value.parameter_set_generation != 0 &&
         NonZero(value.ordered_slot_table_sha256) &&
         PairValid(value.batch_uuid, value.batch_generation) &&
         value.status >= 1 && value.status <= 2 &&
         value.publication_barrier == 1 &&
         NonZero(value.bind_evidence_uuid);
}

inline bool ValueVectorMatches(const SblrParameterBindDescriptorV1& value,
                               std::string* detail) {
  const auto decoded = DecodeSblrParameterValueSetV1(
      value.canonical_value_vector.data(), value.canonical_value_vector.size());
  if (!decoded.ok || decoded.canonical_bytes != value.canonical_value_vector) {
    if (detail != nullptr) {
      *detail = decoded.detail.empty() ? "parameter_bind.value_vector_invalid"
                                      : decoded.detail;
    }
    return false;
  }
  if (decoded.value.parameter_set_descriptor_uuid != value.parameter_set_uuid ||
      decoded.value.descriptor_generation != value.parameter_set_generation ||
      decoded.value.execution_uuid != value.execution_uuid ||
      decoded.value.statement_receipt_uuid != value.statement_receipt_uuid ||
      decoded.value.records.empty() ||
      decoded.value.records.size() > std::numeric_limits<std::uint32_t>::max()) {
    if (detail != nullptr) *detail = "parameter_bind.value_vector_authority_mismatch";
    return false;
  }
  for (std::size_t index = 0; index < decoded.value.records.size(); ++index) {
    const auto& record = decoded.value.records[index];
    if (record.slot_ordinal != index ||
        record.direction == SblrParameterDirectionV1::out ||
        record.state == SblrParameterValueStateV1::unbound) {
      if (detail != nullptr) *detail = "parameter_bind.value_vector_slot_invalid";
      return false;
    }
  }
  return true;
}

}  // namespace parameter_bind_detail

inline std::vector<std::uint8_t> EncodeSblrParameterBindRequestV1(
    const SblrParameterBindRequestV1& value) {
  using namespace parameter_bind_detail;
  if (!RequestValid(value)) return {};
  auto out = Header("SBKQ", kRequestBytes, kRequestBytes);
  Put(&out, value.statement_receipt_uuid);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  return out;
}

inline bool DecodeSblrParameterBindRequestV1(
    const std::uint8_t* in, std::size_t size,
    SblrParameterBindRequestV1* output, std::string* detail) {
  using namespace parameter_bind_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "SBKQ", kRequestBytes, true)) {
    if (detail != nullptr) *detail = "parameter_bind.request_header_invalid";
    return false;
  }
  SblrParameterBindRequestV1 value;
  Get(in + 16, &value.statement_receipt_uuid);
  value.occurrence = GetLe(in + 32, 8);
  value.catalog_generation = GetLe(in + 40, 8);
  value.security_epoch = GetLe(in + 48, 8);
  value.resource_epoch = GetLe(in + 56, 8);
  const auto canonical = EncodeSblrParameterBindRequestV1(value);
  if (canonical.size() != size ||
      !std::equal(canonical.begin(), canonical.end(), in)) {
    if (detail != nullptr) *detail = "parameter_bind.request_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrParameterBindDescriptorV1(
    const SblrParameterBindDescriptorV1& value) {
  using namespace parameter_bind_detail;
  std::string detail;
  if (!DescriptorShapeValid(value) || !ValueVectorMatches(value, &detail)) {
    return {};
  }
  const auto total = kDescriptorPrefixBytes + value.canonical_value_vector.size();
  auto out = Header("SBKD", kDescriptorPrefixBytes, total);
  Put(&out, value.execution_uuid);
  Put(&out, value.statement_receipt_uuid);
  Put(&out, value.prepared_statement_uuid);
  PutLe(&out, value.prepared_generation, 8);
  Put(&out, value.parameter_set_uuid);
  PutLe(&out, value.parameter_set_generation, 8);
  Put(&out, value.ordered_slot_table_sha256);
  Put(&out, value.batch_uuid);
  PutLe(&out, value.batch_generation, 8);
  Put(&out, value.dynamic_package_uuid);
  PutLe(&out, value.dynamic_generation, 8);
  Put(&out, value.catalog_snapshot_uuid);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  Put(&out, value.mga_snapshot_uuid);
  PutLe(&out, value.canonical_value_vector.size(), 4);
  const auto decoded = DecodeSblrParameterValueSetV1(
      value.canonical_value_vector.data(), value.canonical_value_vector.size());
  PutLe(&out, decoded.value.records.size(), 4);
  PutLe(&out, value.executor_availability_generation, 8);
  PutLe(&out, 0, 8);
  out.insert(out.end(), value.canonical_value_vector.begin(),
             value.canonical_value_vector.end());
  return out;
}

inline bool DecodeSblrParameterBindDescriptorV1(
    const std::uint8_t* in, std::size_t size,
    SblrParameterBindDescriptorV1* output, std::string* detail) {
  using namespace parameter_bind_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "SBKD", kDescriptorPrefixBytes, false) ||
      !Zero(in + 248, in + 256)) {
    if (detail != nullptr) *detail = "parameter_bind.descriptor_header_invalid";
    return false;
  }
  const auto value_bytes = GetLe(in + 232, 4);
  const auto value_count = GetLe(in + 236, 4);
  if (value_bytes == 0 || value_bytes > kMaximumValueVectorBytes ||
      value_bytes != size - kDescriptorPrefixBytes || value_count == 0) {
    if (detail != nullptr) *detail = "parameter_bind.descriptor_extent_invalid";
    return false;
  }
  SblrParameterBindDescriptorV1 value;
  Get(in + 16, &value.execution_uuid);
  Get(in + 32, &value.statement_receipt_uuid);
  Get(in + 48, &value.prepared_statement_uuid);
  value.prepared_generation = GetLe(in + 64, 8);
  Get(in + 72, &value.parameter_set_uuid);
  value.parameter_set_generation = GetLe(in + 88, 8);
  Get(in + 96, &value.ordered_slot_table_sha256);
  Get(in + 128, &value.batch_uuid);
  value.batch_generation = GetLe(in + 144, 8);
  Get(in + 152, &value.dynamic_package_uuid);
  value.dynamic_generation = GetLe(in + 168, 8);
  Get(in + 176, &value.catalog_snapshot_uuid);
  value.catalog_generation = GetLe(in + 192, 8);
  value.security_epoch = GetLe(in + 200, 8);
  value.resource_epoch = GetLe(in + 208, 8);
  Get(in + 216, &value.mga_snapshot_uuid);
  value.executor_availability_generation = GetLe(in + 240, 8);
  value.canonical_value_vector.assign(in + kDescriptorPrefixBytes, in + size);
  if (!DescriptorShapeValid(value) || !ValueVectorMatches(value, detail)) {
    if (detail != nullptr && detail->empty()) {
      *detail = "parameter_bind.descriptor_fields_invalid";
    }
    return false;
  }
  const auto decoded = DecodeSblrParameterValueSetV1(
      value.canonical_value_vector.data(), value.canonical_value_vector.size());
  if (decoded.value.records.size() != value_count) {
    if (detail != nullptr) *detail = "parameter_bind.descriptor_value_count_invalid";
    return false;
  }
  const auto canonical = EncodeSblrParameterBindDescriptorV1(value);
  if (canonical.size() != size ||
      !std::equal(canonical.begin(), canonical.end(), in)) {
    if (detail != nullptr) *detail = "parameter_bind.descriptor_noncanonical";
    return false;
  }
  *output = std::move(value);
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrParameterBindResultV1(
    const SblrParameterBindResultV1& value) {
  using namespace parameter_bind_detail;
  if (!ResultValid(value)) return {};
  auto out = Header("SBKR", kResultBytes, kResultBytes);
  Put(&out, value.execution_uuid);
  Put(&out, value.prepared_statement_uuid);
  PutLe(&out, value.prepared_generation, 8);
  Put(&out, value.parameter_set_uuid);
  PutLe(&out, value.parameter_set_generation, 8);
  Put(&out, value.ordered_slot_table_sha256);
  Put(&out, value.batch_uuid);
  PutLe(&out, value.batch_generation, 8);
  out.push_back(value.status);
  out.push_back(value.publication_barrier);
  PutLe(&out, 0, 2);
  Put(&out, value.bind_evidence_uuid);
  out.insert(out.end(), 32, 0);
  PutLe(&out, 0, 4);
  const auto evidence = Hash(out.data(), out.size());
  if (NonZero(value.effect_evidence_sha256) &&
      value.effect_evidence_sha256 != evidence) {
    return {};
  }
  std::copy(evidence.begin(), evidence.end(), out.begin() + 156);
  return out;
}

inline bool DecodeSblrParameterBindResultV1(
    const std::uint8_t* in, std::size_t size,
    SblrParameterBindResultV1* output, std::string* detail) {
  using namespace parameter_bind_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "SBKR", kResultBytes, true) ||
      !Zero(in + 138, in + 140) || !Zero(in + 188, in + 192)) {
    if (detail != nullptr) *detail = "parameter_bind.result_header_invalid";
    return false;
  }
  SblrParameterBindResultV1 value;
  Get(in + 16, &value.execution_uuid);
  Get(in + 32, &value.prepared_statement_uuid);
  value.prepared_generation = GetLe(in + 48, 8);
  Get(in + 56, &value.parameter_set_uuid);
  value.parameter_set_generation = GetLe(in + 72, 8);
  Get(in + 80, &value.ordered_slot_table_sha256);
  Get(in + 112, &value.batch_uuid);
  value.batch_generation = GetLe(in + 128, 8);
  value.status = in[136];
  value.publication_barrier = in[137];
  Get(in + 140, &value.bind_evidence_uuid);
  Get(in + 156, &value.effect_evidence_sha256);
  if (!ResultValid(value) || !NonZero(value.effect_evidence_sha256)) {
    if (detail != nullptr) *detail = "parameter_bind.result_fields_invalid";
    return false;
  }
  std::vector<std::uint8_t> material(in, in + size);
  std::fill(material.begin() + 156, material.begin() + 188, 0);
  if (value.effect_evidence_sha256 != Hash(material.data(), material.size())) {
    if (detail != nullptr) *detail = "parameter_bind.result_evidence_invalid";
    return false;
  }
  const auto canonical = EncodeSblrParameterBindResultV1(value);
  if (canonical.size() != size ||
      !std::equal(canonical.begin(), canonical.end(), in)) {
    if (detail != nullptr) *detail = "parameter_bind.result_noncanonical";
    return false;
  }
  *output = value;
  return true;
}

}  // namespace scratchbird::engine::sblr

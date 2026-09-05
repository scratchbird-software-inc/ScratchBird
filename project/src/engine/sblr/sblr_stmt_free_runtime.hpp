#pragma once

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

using SblrStmtFreeUuidV1 = std::array<std::uint8_t, 16>;
using SblrStmtFreeSha256V1 = std::array<std::uint8_t, 32>;

struct SblrStmtFreeRequestV1 {
  SblrStmtFreeUuidV1 statement_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
};

struct SblrStmtFreeDescriptorV1 {
  SblrStmtFreeUuidV1 statement_uuid{};
  SblrStmtFreeUuidV1 statement_name_uuid{};
  SblrStmtFreeUuidV1 statement_receipt_uuid{};
  SblrStmtFreeUuidV1 catalog_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  SblrStmtFreeUuidV1 mga_snapshot_uuid{};
  std::uint64_t prepared_generation = 0;
  std::uint64_t executor_availability_generation = 0;
  SblrStmtFreeSha256V1 descriptor_sha256{};
};

struct SblrStmtFreeResultV1 {
  SblrStmtFreeUuidV1 statement_uuid{};
  std::uint64_t terminal_prepared_generation = 0;
  std::uint8_t terminal_state = 1;
  std::uint8_t publication_barrier = 1;
  SblrStmtFreeUuidV1 cleanup_evidence_uuid{};
  std::uint64_t executor_availability_generation = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  SblrStmtFreeSha256V1 effect_evidence_sha256{};
};

namespace stmt_free_detail {

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
                                        std::size_t size) {
  std::vector<std::uint8_t> out(magic.begin(), magic.end());
  PutLe(&out, 1, 2);
  PutLe(&out, size, 2);
  PutLe(&out, size, 4);
  PutLe(&out, 0, 4);
  return out;
}

inline bool HeaderValid(const std::uint8_t* in, std::size_t size,
                        std::string_view magic, std::size_t expected) {
  return in != nullptr && size == expected && magic.size() == 4 &&
         std::equal(magic.begin(), magic.end(), in) && GetLe(in + 4, 2) == 1 &&
         GetLe(in + 6, 2) == expected && GetLe(in + 8, 4) == expected &&
         Zero(in + 12, in + 16);
}

inline SblrStmtFreeSha256V1 Hash(const std::uint8_t* in, std::size_t size) {
  std::vector<std::uint8_t> bytes(in, in + size);
  return scratchbird::core::hash::ComputeSha256Digest(bytes).digest;
}

inline bool DescriptorValid(const SblrStmtFreeDescriptorV1& value) {
  return NonZero(value.statement_uuid) && NonZero(value.statement_name_uuid) &&
         NonZero(value.statement_receipt_uuid) &&
         NonZero(value.catalog_snapshot_uuid) && value.catalog_generation != 0 &&
         value.security_epoch != 0 && value.resource_epoch != 0 &&
         NonZero(value.mga_snapshot_uuid) && value.prepared_generation != 0 &&
         value.executor_availability_generation != 0;
}

inline bool ResultValid(const SblrStmtFreeResultV1& value) {
  return NonZero(value.statement_uuid) &&
         value.terminal_prepared_generation != 0 &&
         (value.terminal_state == 1 || value.terminal_state == 2) &&
         value.publication_barrier == 1 &&
         NonZero(value.cleanup_evidence_uuid) &&
         value.executor_availability_generation != 0 &&
         value.catalog_generation != 0 && value.security_epoch != 0 &&
         value.resource_epoch != 0;
}

}  // namespace stmt_free_detail

inline std::vector<std::uint8_t> EncodeSblrStmtFreeRequestV1(
    const SblrStmtFreeRequestV1& value) {
  using namespace stmt_free_detail;
  if (!NonZero(value.statement_receipt_uuid) || value.occurrence == 0 ||
      value.catalog_generation == 0 || value.security_epoch == 0 ||
      value.resource_epoch == 0) {
    return {};
  }
  auto out = Header("SBFQ", 64);
  Put(&out, value.statement_receipt_uuid);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  return out;
}

inline bool DecodeSblrStmtFreeRequestV1(const std::uint8_t* in,
                                        std::size_t size,
                                        SblrStmtFreeRequestV1* output,
                                        std::string* detail) {
  using namespace stmt_free_detail;
  if (output == nullptr || !HeaderValid(in, size, "SBFQ", 64)) {
    if (detail != nullptr) *detail = "stmt_free_request_invalid";
    return false;
  }
  SblrStmtFreeRequestV1 value;
  Get(in + 16, &value.statement_receipt_uuid);
  value.occurrence = GetLe(in + 32, 8);
  value.catalog_generation = GetLe(in + 40, 8);
  value.security_epoch = GetLe(in + 48, 8);
  value.resource_epoch = GetLe(in + 56, 8);
  if (EncodeSblrStmtFreeRequestV1(value).empty()) {
    if (detail != nullptr) *detail = "stmt_free_request_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrStmtFreeDescriptorV1(
    const SblrStmtFreeDescriptorV1& value) {
  using namespace stmt_free_detail;
  if (!DescriptorValid(value)) return {};
  auto out = Header("SBFD", 176);
  Put(&out, value.statement_uuid);
  Put(&out, value.statement_name_uuid);
  Put(&out, value.statement_receipt_uuid);
  Put(&out, value.catalog_snapshot_uuid);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  Put(&out, value.mga_snapshot_uuid);
  PutLe(&out, value.prepared_generation, 8);
  PutLe(&out, value.executor_availability_generation, 8);
  out.insert(out.end(), 32, 0);
  out.insert(out.end(), 8, 0);
  const auto evidence = Hash(out.data(), out.size());
  if (NonZero(value.descriptor_sha256) &&
      value.descriptor_sha256 != evidence) {
    return {};
  }
  std::copy(evidence.begin(), evidence.end(), out.begin() + 136);
  return out;
}

inline bool DecodeSblrStmtFreeDescriptorV1(
    const std::uint8_t* in, std::size_t size,
    SblrStmtFreeDescriptorV1* output, std::string* detail) {
  using namespace stmt_free_detail;
  if (output == nullptr || !HeaderValid(in, size, "SBFD", 176) ||
      !Zero(in + 168, in + 176)) {
    if (detail != nullptr) *detail = "stmt_free_descriptor_header_invalid";
    return false;
  }
  SblrStmtFreeDescriptorV1 value;
  Get(in + 16, &value.statement_uuid);
  Get(in + 32, &value.statement_name_uuid);
  Get(in + 48, &value.statement_receipt_uuid);
  Get(in + 64, &value.catalog_snapshot_uuid);
  value.catalog_generation = GetLe(in + 80, 8);
  value.security_epoch = GetLe(in + 88, 8);
  value.resource_epoch = GetLe(in + 96, 8);
  Get(in + 104, &value.mga_snapshot_uuid);
  value.prepared_generation = GetLe(in + 120, 8);
  value.executor_availability_generation = GetLe(in + 128, 8);
  Get(in + 136, &value.descriptor_sha256);
  if (!DescriptorValid(value) || !NonZero(value.descriptor_sha256)) {
    if (detail != nullptr) *detail = "stmt_free_descriptor_fields_invalid";
    return false;
  }
  std::vector<std::uint8_t> material(in, in + size);
  std::fill(material.begin() + 136, material.begin() + 168, 0);
  if (value.descriptor_sha256 != Hash(material.data(), material.size())) {
    if (detail != nullptr) *detail = "stmt_free_descriptor_hash_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrStmtFreeResultV1(
    const SblrStmtFreeResultV1& value) {
  using namespace stmt_free_detail;
  if (!ResultValid(value)) return {};
  auto out = Header("SBFR", 128);
  Put(&out, value.statement_uuid);
  PutLe(&out, value.terminal_prepared_generation, 8);
  out.push_back(value.terminal_state);
  out.push_back(value.publication_barrier);
  out.insert(out.end(), 2, 0);
  Put(&out, value.cleanup_evidence_uuid);
  PutLe(&out, value.executor_availability_generation, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  out.insert(out.end(), 32, 0);
  out.insert(out.end(), 4, 0);
  const auto evidence = Hash(out.data(), out.size());
  if (NonZero(value.effect_evidence_sha256) &&
      value.effect_evidence_sha256 != evidence) {
    return {};
  }
  std::copy(evidence.begin(), evidence.end(), out.begin() + 92);
  return out;
}

inline bool DecodeSblrStmtFreeResultV1(const std::uint8_t* in,
                                       std::size_t size,
                                       SblrStmtFreeResultV1* output,
                                       std::string* detail) {
  using namespace stmt_free_detail;
  if (output == nullptr || !HeaderValid(in, size, "SBFR", 128) ||
      !Zero(in + 42, in + 44) || !Zero(in + 124, in + 128)) {
    if (detail != nullptr) *detail = "stmt_free_result_header_invalid";
    return false;
  }
  SblrStmtFreeResultV1 value;
  Get(in + 16, &value.statement_uuid);
  value.terminal_prepared_generation = GetLe(in + 32, 8);
  value.terminal_state = in[40];
  value.publication_barrier = in[41];
  Get(in + 44, &value.cleanup_evidence_uuid);
  value.executor_availability_generation = GetLe(in + 60, 8);
  value.catalog_generation = GetLe(in + 68, 8);
  value.security_epoch = GetLe(in + 76, 8);
  value.resource_epoch = GetLe(in + 84, 8);
  Get(in + 92, &value.effect_evidence_sha256);
  if (!ResultValid(value) || !NonZero(value.effect_evidence_sha256)) {
    if (detail != nullptr) *detail = "stmt_free_result_fields_invalid";
    return false;
  }
  std::vector<std::uint8_t> material(in, in + size);
  std::fill(material.begin() + 92, material.begin() + 124, 0);
  if (value.effect_evidence_sha256 != Hash(material.data(), material.size())) {
    if (detail != nullptr) *detail = "stmt_free_result_evidence_invalid";
    return false;
  }
  *output = value;
  return true;
}

}  // namespace scratchbird::engine::sblr

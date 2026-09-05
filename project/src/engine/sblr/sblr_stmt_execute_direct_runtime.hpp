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

using SblrStmtExecuteDirectUuidV1 = std::array<std::uint8_t, 16>;
using SblrStmtExecuteDirectSha256V1 = std::array<std::uint8_t, 32>;

struct SblrStmtExecuteDirectRequestV1 {
  SblrStmtExecuteDirectUuidV1 statement_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
};

struct SblrStmtExecuteDirectDescriptorV1 {
  SblrStmtExecuteDirectUuidV1 execution_uuid{};
  SblrStmtExecuteDirectUuidV1 statement_receipt_uuid{};
  SblrStmtExecuteDirectUuidV1 catalog_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  SblrStmtExecuteDirectUuidV1 mga_snapshot_uuid{};
  SblrStmtExecuteDirectUuidV1 parameter_set_uuid{};
  std::uint64_t parameter_set_generation = 0;
  SblrStmtExecuteDirectUuidV1 batch_uuid{};
  std::uint64_t batch_generation = 0;
  SblrStmtExecuteDirectUuidV1 dynamic_package_uuid{};
  std::uint64_t dynamic_package_generation = 0;
  SblrStmtExecuteDirectUuidV1 result_descriptor_uuid{};
  SblrStmtExecuteDirectSha256V1 canonical_sblr_sha256{};
  SblrStmtExecuteDirectUuidV1 parser_package_uuid{};
  std::uint64_t executor_availability_generation = 0;
  std::vector<std::uint8_t> canonical_sblr_bytes;
  std::vector<std::uint8_t> canonical_parameter_bytes;
};

struct SblrStmtExecuteDirectResultV1 {
  SblrStmtExecuteDirectUuidV1 execution_uuid{};
  SblrStmtExecuteDirectUuidV1 statement_receipt_uuid{};
  SblrStmtExecuteDirectUuidV1 result_descriptor_uuid{};
  SblrStmtExecuteDirectUuidV1 result_handle_uuid{};
  SblrStmtExecuteDirectUuidV1 mga_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t executor_availability_generation = 0;
  std::uint8_t status = 1;
  std::uint8_t publication_barrier = 1;
  SblrStmtExecuteDirectSha256V1 effect_evidence_sha256{};
  SblrStmtExecuteDirectUuidV1 operation_evidence_uuid{};
};

namespace stmt_execute_direct_detail {

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
                                        std::size_t header,
                                        std::size_t total) {
  std::vector<std::uint8_t> out(magic.begin(), magic.end());
  PutLe(&out, 1, 2);
  PutLe(&out, header, 2);
  PutLe(&out, total, 4);
  PutLe(&out, 0, 4);
  return out;
}

inline bool HeaderValid(const std::uint8_t* in, std::size_t size,
                        std::string_view magic, std::size_t header,
                        std::size_t total) {
  return in != nullptr && size == total && magic.size() == 4 &&
         std::equal(magic.begin(), magic.end(), in) && GetLe(in + 4, 2) == 1 &&
         GetLe(in + 6, 2) == header && GetLe(in + 8, 4) == total &&
         Zero(in + 12, in + 16);
}

inline SblrStmtExecuteDirectSha256V1 Hash(const std::uint8_t* in,
                                         std::size_t size) {
  std::vector<std::uint8_t> bytes(in, in + size);
  return scratchbird::core::hash::ComputeSha256Digest(bytes).digest;
}

inline bool OptionalPairValid(const SblrStmtExecuteDirectUuidV1& uuid,
                              std::uint64_t generation) {
  return NonZero(uuid) == (generation != 0);
}

inline bool DescriptorValid(const SblrStmtExecuteDirectDescriptorV1& value) {
  return NonZero(value.execution_uuid) &&
         NonZero(value.statement_receipt_uuid) &&
         NonZero(value.catalog_snapshot_uuid) && value.catalog_generation != 0 &&
         value.security_epoch != 0 && value.resource_epoch != 0 &&
         NonZero(value.mga_snapshot_uuid) &&
         OptionalPairValid(value.parameter_set_uuid,
                           value.parameter_set_generation) &&
         OptionalPairValid(value.batch_uuid, value.batch_generation) &&
         OptionalPairValid(value.dynamic_package_uuid,
                           value.dynamic_package_generation) &&
         NonZero(value.parser_package_uuid) &&
         value.executor_availability_generation != 0 &&
         !value.canonical_sblr_bytes.empty() &&
         value.canonical_sblr_bytes.size() <=
             std::numeric_limits<std::uint32_t>::max() &&
         value.canonical_parameter_bytes.size() <=
             std::numeric_limits<std::uint32_t>::max();
}

inline bool ResultValid(const SblrStmtExecuteDirectResultV1& value) {
  const bool rowset_pair =
      NonZero(value.result_descriptor_uuid) == NonZero(value.result_handle_uuid);
  return NonZero(value.execution_uuid) &&
         NonZero(value.statement_receipt_uuid) && rowset_pair &&
         NonZero(value.mga_snapshot_uuid) && value.catalog_generation != 0 &&
         value.security_epoch != 0 && value.resource_epoch != 0 &&
         value.executor_availability_generation != 0 &&
         (value.status == 1 || value.status == 2) &&
         value.publication_barrier == 1 &&
         NonZero(value.operation_evidence_uuid);
}

}  // namespace stmt_execute_direct_detail

inline std::vector<std::uint8_t> EncodeSblrStmtExecuteDirectRequestV1(
    const SblrStmtExecuteDirectRequestV1& value) {
  using namespace stmt_execute_direct_detail;
  if (!NonZero(value.statement_receipt_uuid) || value.occurrence == 0 ||
      value.catalog_generation == 0 || value.security_epoch == 0 ||
      value.resource_epoch == 0) {
    return {};
  }
  auto out = Header("SBEQ", 64, 64);
  Put(&out, value.statement_receipt_uuid);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  return out;
}

inline bool DecodeSblrStmtExecuteDirectRequestV1(
    const std::uint8_t* in, std::size_t size,
    SblrStmtExecuteDirectRequestV1* output, std::string* detail) {
  using namespace stmt_execute_direct_detail;
  if (output == nullptr || !HeaderValid(in, size, "SBEQ", 64, 64)) {
    if (detail != nullptr) *detail = "stmt_execute_direct_request_invalid";
    return false;
  }
  SblrStmtExecuteDirectRequestV1 value;
  Get(in + 16, &value.statement_receipt_uuid);
  value.occurrence = GetLe(in + 32, 8);
  value.catalog_generation = GetLe(in + 40, 8);
  value.security_epoch = GetLe(in + 48, 8);
  value.resource_epoch = GetLe(in + 56, 8);
  if (EncodeSblrStmtExecuteDirectRequestV1(value).empty()) {
    if (detail != nullptr) *detail = "stmt_execute_direct_request_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrStmtExecuteDirectDescriptorV1(
    const SblrStmtExecuteDirectDescriptorV1& value) {
  using namespace stmt_execute_direct_detail;
  if (!DescriptorValid(value)) return {};
  const std::size_t total = 256 + value.canonical_sblr_bytes.size() +
                            value.canonical_parameter_bytes.size();
  if (total > std::numeric_limits<std::uint32_t>::max()) return {};
  auto out = Header("SBED", 256, total);
  Put(&out, value.execution_uuid);
  Put(&out, value.statement_receipt_uuid);
  Put(&out, value.catalog_snapshot_uuid);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  Put(&out, value.mga_snapshot_uuid);
  Put(&out, value.parameter_set_uuid);
  PutLe(&out, value.parameter_set_generation, 8);
  Put(&out, value.batch_uuid);
  PutLe(&out, value.batch_generation, 8);
  Put(&out, value.dynamic_package_uuid);
  PutLe(&out, value.dynamic_package_generation, 8);
  Put(&out, value.result_descriptor_uuid);
  PutLe(&out, value.canonical_sblr_bytes.size(), 4);
  PutLe(&out, value.canonical_parameter_bytes.size(), 4);
  const auto body_hash =
      Hash(value.canonical_sblr_bytes.data(), value.canonical_sblr_bytes.size());
  if (NonZero(value.canonical_sblr_sha256) &&
      value.canonical_sblr_sha256 != body_hash) {
    return {};
  }
  Put(&out, body_hash);
  Put(&out, value.parser_package_uuid);
  PutLe(&out, value.executor_availability_generation, 8);
  out.insert(out.end(), value.canonical_sblr_bytes.begin(),
             value.canonical_sblr_bytes.end());
  out.insert(out.end(), value.canonical_parameter_bytes.begin(),
             value.canonical_parameter_bytes.end());
  return out;
}

inline bool DecodeSblrStmtExecuteDirectDescriptorV1(
    const std::uint8_t* in, std::size_t size,
    SblrStmtExecuteDirectDescriptorV1* output, std::string* detail) {
  using namespace stmt_execute_direct_detail;
  if (output == nullptr || in == nullptr || size < 256 ||
      !HeaderValid(in, size, "SBED", 256, size)) {
    if (detail != nullptr) *detail = "stmt_execute_direct_descriptor_header_invalid";
    return false;
  }
  const auto body_size = GetLe(in + 192, 4);
  const auto parameter_size = GetLe(in + 196, 4);
  if (body_size + parameter_size != size - 256) {
    if (detail != nullptr) *detail = "stmt_execute_direct_descriptor_extent_invalid";
    return false;
  }
  SblrStmtExecuteDirectDescriptorV1 value;
  Get(in + 16, &value.execution_uuid);
  Get(in + 32, &value.statement_receipt_uuid);
  Get(in + 48, &value.catalog_snapshot_uuid);
  value.catalog_generation = GetLe(in + 64, 8);
  value.security_epoch = GetLe(in + 72, 8);
  value.resource_epoch = GetLe(in + 80, 8);
  Get(in + 88, &value.mga_snapshot_uuid);
  Get(in + 104, &value.parameter_set_uuid);
  value.parameter_set_generation = GetLe(in + 120, 8);
  Get(in + 128, &value.batch_uuid);
  value.batch_generation = GetLe(in + 144, 8);
  Get(in + 152, &value.dynamic_package_uuid);
  value.dynamic_package_generation = GetLe(in + 168, 8);
  Get(in + 176, &value.result_descriptor_uuid);
  Get(in + 200, &value.canonical_sblr_sha256);
  Get(in + 232, &value.parser_package_uuid);
  value.executor_availability_generation = GetLe(in + 248, 8);
  value.canonical_sblr_bytes.assign(in + 256, in + 256 + body_size);
  value.canonical_parameter_bytes.assign(in + 256 + body_size, in + size);
  if (!DescriptorValid(value) ||
      value.canonical_sblr_sha256 !=
          Hash(value.canonical_sblr_bytes.data(),
               value.canonical_sblr_bytes.size())) {
    if (detail != nullptr) *detail = "stmt_execute_direct_descriptor_fields_invalid";
    return false;
  }
  *output = std::move(value);
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrStmtExecuteDirectResultV1(
    const SblrStmtExecuteDirectResultV1& value) {
  using namespace stmt_execute_direct_detail;
  if (!ResultValid(value)) return {};
  auto out = Header("SBER", 192, 192);
  Put(&out, value.execution_uuid);
  Put(&out, value.statement_receipt_uuid);
  Put(&out, value.result_descriptor_uuid);
  Put(&out, value.result_handle_uuid);
  Put(&out, value.mga_snapshot_uuid);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  PutLe(&out, value.executor_availability_generation, 8);
  out.push_back(value.status);
  out.push_back(value.publication_barrier);
  out.insert(out.end(), 2, 0);
  out.insert(out.end(), 32, 0);
  Put(&out, value.operation_evidence_uuid);
  out.insert(out.end(), 12, 0);
  const auto evidence = Hash(out.data(), out.size());
  if (NonZero(value.effect_evidence_sha256) &&
      value.effect_evidence_sha256 != evidence) {
    return {};
  }
  std::copy(evidence.begin(), evidence.end(), out.begin() + 132);
  return out;
}

inline bool DecodeSblrStmtExecuteDirectResultV1(
    const std::uint8_t* in, std::size_t size,
    SblrStmtExecuteDirectResultV1* output, std::string* detail) {
  using namespace stmt_execute_direct_detail;
  if (output == nullptr || !HeaderValid(in, size, "SBER", 192, 192) ||
      !Zero(in + 130, in + 132) || !Zero(in + 180, in + 192)) {
    if (detail != nullptr) *detail = "stmt_execute_direct_result_header_invalid";
    return false;
  }
  SblrStmtExecuteDirectResultV1 value;
  Get(in + 16, &value.execution_uuid);
  Get(in + 32, &value.statement_receipt_uuid);
  Get(in + 48, &value.result_descriptor_uuid);
  Get(in + 64, &value.result_handle_uuid);
  Get(in + 80, &value.mga_snapshot_uuid);
  value.catalog_generation = GetLe(in + 96, 8);
  value.security_epoch = GetLe(in + 104, 8);
  value.resource_epoch = GetLe(in + 112, 8);
  value.executor_availability_generation = GetLe(in + 120, 8);
  value.status = in[128];
  value.publication_barrier = in[129];
  Get(in + 132, &value.effect_evidence_sha256);
  Get(in + 164, &value.operation_evidence_uuid);
  if (!ResultValid(value) || !NonZero(value.effect_evidence_sha256)) {
    if (detail != nullptr) *detail = "stmt_execute_direct_result_fields_invalid";
    return false;
  }
  std::vector<std::uint8_t> material(in, in + size);
  std::fill(material.begin() + 132, material.begin() + 164, 0);
  if (value.effect_evidence_sha256 != Hash(material.data(), material.size())) {
    if (detail != nullptr) *detail = "stmt_execute_direct_result_evidence_invalid";
    return false;
  }
  *output = value;
  return true;
}

}  // namespace scratchbird::engine::sblr

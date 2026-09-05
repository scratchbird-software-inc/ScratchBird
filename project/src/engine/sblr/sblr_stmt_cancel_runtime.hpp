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

using SblrStmtCancelUuidV1 = std::array<std::uint8_t, 16>;
using SblrStmtCancelSha256V1 = std::array<std::uint8_t, 32>;

struct SblrStmtCancelRequestV1 {
  SblrStmtCancelUuidV1 statement_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
};

struct SblrStmtCancelDescriptorV1 {
  SblrStmtCancelUuidV1 target_execution_uuid{};
  SblrStmtCancelUuidV1 target_statement_uuid{};
  SblrStmtCancelUuidV1 target_statement_receipt_uuid{};
  SblrStmtCancelUuidV1 cancel_operation_uuid{};
  SblrStmtCancelUuidV1 target_transaction_uuid{};
  std::uint64_t target_execution_generation = 0;
  std::uint8_t reason = 0;
  std::uint8_t mode = 0;
  std::uint64_t deadline_monotonic_ns = 0;
  std::uint64_t executor_availability_generation = 0;
  SblrStmtCancelSha256V1 descriptor_sha256{};
};

struct SblrStmtCancelResultV1 {
  SblrStmtCancelUuidV1 target_execution_uuid{};
  SblrStmtCancelUuidV1 cancel_operation_uuid{};
  std::uint8_t state = 0;
  std::uint8_t finality = 0;
  std::uint8_t publication_barrier = 0;
  SblrStmtCancelUuidV1 cancellation_evidence_uuid{};
  std::uint64_t target_execution_generation = 0;
  std::uint64_t executor_availability_generation = 0;
  SblrStmtCancelSha256V1 effect_evidence_sha256{};
};

namespace stmt_cancel_detail {

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

inline SblrStmtCancelSha256V1 Hash(const std::uint8_t* in,
                                  std::size_t size) {
  std::vector<std::uint8_t> bytes(in, in + size);
  return scratchbird::core::hash::ComputeSha256Digest(bytes).digest;
}

inline bool DescriptorValid(const SblrStmtCancelDescriptorV1& value) {
  return NonZero(value.target_execution_uuid) &&
         NonZero(value.target_statement_uuid) &&
         NonZero(value.target_statement_receipt_uuid) &&
         NonZero(value.cancel_operation_uuid) &&
         value.target_execution_generation != 0 && value.reason >= 1 &&
         value.reason <= 4 && value.mode >= 1 && value.mode <= 2 &&
         value.executor_availability_generation != 0;
}

inline bool ResultValid(const SblrStmtCancelResultV1& value) {
  return NonZero(value.target_execution_uuid) &&
         NonZero(value.cancel_operation_uuid) && value.state >= 1 &&
         value.state <= 4 && value.finality <= 1 &&
         value.publication_barrier == 1 &&
         NonZero(value.cancellation_evidence_uuid) &&
         value.target_execution_generation != 0 &&
         value.executor_availability_generation != 0;
}

}  // namespace stmt_cancel_detail

inline std::vector<std::uint8_t> EncodeSblrStmtCancelRequestV1(
    const SblrStmtCancelRequestV1& value) {
  using namespace stmt_cancel_detail;
  if (!NonZero(value.statement_receipt_uuid) || value.occurrence == 0 ||
      value.catalog_generation == 0 || value.security_epoch == 0 ||
      value.resource_epoch == 0) {
    return {};
  }
  auto out = Header("SBCQ", 64);
  Put(&out, value.statement_receipt_uuid);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  return out;
}

inline bool DecodeSblrStmtCancelRequestV1(
    const std::uint8_t* in, std::size_t size,
    SblrStmtCancelRequestV1* output, std::string* detail) {
  using namespace stmt_cancel_detail;
  if (output == nullptr || !HeaderValid(in, size, "SBCQ", 64)) {
    if (detail != nullptr) *detail = "stmt_cancel_request_invalid";
    return false;
  }
  SblrStmtCancelRequestV1 value;
  Get(in + 16, &value.statement_receipt_uuid);
  value.occurrence = GetLe(in + 32, 8);
  value.catalog_generation = GetLe(in + 40, 8);
  value.security_epoch = GetLe(in + 48, 8);
  value.resource_epoch = GetLe(in + 56, 8);
  const auto canonical = EncodeSblrStmtCancelRequestV1(value);
  if (canonical.size() != size ||
      !std::equal(canonical.begin(), canonical.end(), in)) {
    if (detail != nullptr) *detail = "stmt_cancel_request_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrStmtCancelDescriptorV1(
    const SblrStmtCancelDescriptorV1& value) {
  using namespace stmt_cancel_detail;
  if (!DescriptorValid(value)) return {};
  auto out = Header("SBCD", 176);
  Put(&out, value.target_execution_uuid);
  Put(&out, value.target_statement_uuid);
  Put(&out, value.target_statement_receipt_uuid);
  Put(&out, value.cancel_operation_uuid);
  Put(&out, value.target_transaction_uuid);
  PutLe(&out, value.target_execution_generation, 8);
  out.push_back(value.reason);
  out.push_back(value.mode);
  out.insert(out.end(), 2, 0);
  PutLe(&out, value.deadline_monotonic_ns, 8);
  PutLe(&out, value.executor_availability_generation, 8);
  out.insert(out.end(), 32, 0);
  out.insert(out.end(), 20, 0);
  const auto evidence = Hash(out.data(), out.size());
  if (NonZero(value.descriptor_sha256) &&
      value.descriptor_sha256 != evidence) {
    return {};
  }
  std::copy(evidence.begin(), evidence.end(), out.begin() + 124);
  return out;
}

inline bool DecodeSblrStmtCancelDescriptorV1(
    const std::uint8_t* in, std::size_t size,
    SblrStmtCancelDescriptorV1* output, std::string* detail) {
  using namespace stmt_cancel_detail;
  if (output == nullptr || !HeaderValid(in, size, "SBCD", 176) ||
      !Zero(in + 106, in + 108) || !Zero(in + 156, in + 176)) {
    if (detail != nullptr) *detail = "stmt_cancel_descriptor_header_invalid";
    return false;
  }
  SblrStmtCancelDescriptorV1 value;
  Get(in + 16, &value.target_execution_uuid);
  Get(in + 32, &value.target_statement_uuid);
  Get(in + 48, &value.target_statement_receipt_uuid);
  Get(in + 64, &value.cancel_operation_uuid);
  Get(in + 80, &value.target_transaction_uuid);
  value.target_execution_generation = GetLe(in + 96, 8);
  value.reason = in[104];
  value.mode = in[105];
  value.deadline_monotonic_ns = GetLe(in + 108, 8);
  value.executor_availability_generation = GetLe(in + 116, 8);
  Get(in + 124, &value.descriptor_sha256);
  if (!DescriptorValid(value) || !NonZero(value.descriptor_sha256)) {
    if (detail != nullptr) *detail = "stmt_cancel_descriptor_fields_invalid";
    return false;
  }
  std::vector<std::uint8_t> material(in, in + size);
  std::fill(material.begin() + 124, material.begin() + 156, 0);
  if (value.descriptor_sha256 != Hash(material.data(), material.size())) {
    if (detail != nullptr) *detail = "stmt_cancel_descriptor_hash_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrStmtCancelResultV1(
    const SblrStmtCancelResultV1& value) {
  using namespace stmt_cancel_detail;
  if (!ResultValid(value)) return {};
  auto out = Header("SBCR", 128);
  Put(&out, value.target_execution_uuid);
  Put(&out, value.cancel_operation_uuid);
  out.push_back(value.state);
  out.push_back(value.finality);
  out.push_back(value.publication_barrier);
  out.push_back(0);
  Put(&out, value.cancellation_evidence_uuid);
  PutLe(&out, value.target_execution_generation, 8);
  PutLe(&out, value.executor_availability_generation, 8);
  out.insert(out.end(), 32, 0);
  out.insert(out.end(), 12, 0);
  const auto evidence = Hash(out.data(), out.size());
  if (NonZero(value.effect_evidence_sha256) &&
      value.effect_evidence_sha256 != evidence) {
    return {};
  }
  std::copy(evidence.begin(), evidence.end(), out.begin() + 84);
  return out;
}

inline bool DecodeSblrStmtCancelResultV1(
    const std::uint8_t* in, std::size_t size, SblrStmtCancelResultV1* output,
    std::string* detail) {
  using namespace stmt_cancel_detail;
  if (output == nullptr || !HeaderValid(in, size, "SBCR", 128) ||
      in[51] != 0 || !Zero(in + 116, in + 128)) {
    if (detail != nullptr) *detail = "stmt_cancel_result_header_invalid";
    return false;
  }
  SblrStmtCancelResultV1 value;
  Get(in + 16, &value.target_execution_uuid);
  Get(in + 32, &value.cancel_operation_uuid);
  value.state = in[48];
  value.finality = in[49];
  value.publication_barrier = in[50];
  Get(in + 52, &value.cancellation_evidence_uuid);
  value.target_execution_generation = GetLe(in + 68, 8);
  value.executor_availability_generation = GetLe(in + 76, 8);
  Get(in + 84, &value.effect_evidence_sha256);
  if (!ResultValid(value) || !NonZero(value.effect_evidence_sha256)) {
    if (detail != nullptr) *detail = "stmt_cancel_result_fields_invalid";
    return false;
  }
  std::vector<std::uint8_t> material(in, in + size);
  std::fill(material.begin() + 84, material.begin() + 116, 0);
  if (value.effect_evidence_sha256 != Hash(material.data(), material.size())) {
    if (detail != nullptr) *detail = "stmt_cancel_result_evidence_invalid";
    return false;
  }
  *output = value;
  return true;
}

}  // namespace scratchbird::engine::sblr

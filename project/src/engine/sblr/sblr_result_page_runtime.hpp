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

inline constexpr std::uint16_t kSblrResultPageOpcodeCode = 4614;

using SblrResultPageUuidV1 = std::array<std::uint8_t, 16>;
using SblrResultPageSha256V1 = std::array<std::uint8_t, 32>;

struct SblrResultPageRequestV1 {
  SblrResultPageUuidV1 statement_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
};

struct SblrResultPageDescriptorV1 {
  SblrResultPageUuidV1 cursor_uuid{};
  std::uint64_t cursor_generation = 0;
  SblrResultPageUuidV1 statement_receipt_uuid{};
  SblrResultPageUuidV1 result_set_handle_uuid{};
  std::uint64_t result_set_handle_generation = 0;
  SblrResultPageUuidV1 snapshot_uuid{};
  SblrResultPageUuidV1 row_descriptor_uuid{};
  std::uint64_t row_descriptor_generation = 0;
  std::uint64_t page_number = 0;
  std::uint64_t first_row_offset = 0;
  std::uint64_t maximum_rows = 0;
  std::uint64_t maximum_bytes = 0;
  SblrResultPageUuidV1 continuation_uuid{};
  std::uint64_t continuation_generation = 0;
  SblrResultPageUuidV1 redaction_profile_uuid{};
  std::uint64_t redaction_generation = 0;
  SblrResultPageUuidV1 policy_snapshot_uuid{};
  std::uint64_t policy_generation = 0;
  SblrResultPageUuidV1 resource_budget_uuid{};
  std::uint64_t resource_budget_generation = 0;
  SblrResultPageSha256V1 descriptor_sha256{};
  std::uint64_t executor_availability_generation = 0;
};

struct SblrResultPageResultV1 {
  SblrResultPageUuidV1 cursor_uuid{};
  std::uint64_t cursor_generation = 0;
  std::uint8_t completion_state = 0;
  std::uint8_t terminal_state = 0;
  SblrResultPageUuidV1 result_set_handle_uuid{};
  std::uint64_t result_set_handle_generation = 0;
  SblrResultPageUuidV1 row_descriptor_uuid{};
  std::uint64_t row_descriptor_generation = 0;
  std::uint64_t returned_row_count = 0;
  std::uint64_t next_row_offset = 0;
  SblrResultPageUuidV1 next_continuation_uuid{};
  std::uint64_t next_continuation_generation = 0;
  SblrResultPageUuidV1 redaction_profile_uuid{};
  SblrResultPageSha256V1 result_material_sha256{};
  SblrResultPageSha256V1 executor_evidence_sha256{};
  std::uint64_t executor_availability_generation = 0;
  SblrResultPageUuidV1 publication_barrier_uuid{};
  SblrResultPageUuidV1 result_evidence_uuid{};
};

namespace result_page_detail {

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
                                        std::size_t bytes) {
  std::vector<std::uint8_t> out(magic.begin(), magic.end());
  PutLe(&out, 1, 2);
  PutLe(&out, bytes, 2);
  PutLe(&out, bytes, 4);
  PutLe(&out, 0, 4);
  return out;
}

inline bool HeaderValid(const std::uint8_t* in, std::size_t size,
                        std::string_view magic, std::size_t expected) {
  return in != nullptr && size == expected && magic.size() == 4 &&
         std::equal(magic.begin(), magic.end(), in) &&
         GetLe(in + 4, 2) == 1 && GetLe(in + 6, 2) == expected &&
         GetLe(in + 8, 4) == expected && Zero(in + 12, in + 16);
}

inline SblrResultPageSha256V1 Hash(const std::uint8_t* in,
                                  std::size_t size) {
  std::vector<std::uint8_t> bytes;
  if (size != 0) bytes.assign(in, in + size);
  return scratchbird::core::hash::ComputeSha256Digest(bytes).digest;
}

inline bool OptionalIdentityValid(const SblrResultPageUuidV1& uuid,
                                  std::uint64_t generation) {
  return NonZero(uuid) == (generation != 0);
}

inline bool DescriptorValid(const SblrResultPageDescriptorV1& value) {
  return NonZero(value.cursor_uuid) && value.cursor_generation != 0 &&
         NonZero(value.statement_receipt_uuid) &&
         NonZero(value.result_set_handle_uuid) &&
         value.result_set_handle_generation != 0 &&
         NonZero(value.snapshot_uuid) && NonZero(value.row_descriptor_uuid) &&
         value.row_descriptor_generation != 0 && value.maximum_rows != 0 &&
         value.maximum_bytes != 0 &&
         OptionalIdentityValid(value.continuation_uuid,
                               value.continuation_generation) &&
         ((value.page_number == 0 && value.first_row_offset == 0 &&
           !NonZero(value.continuation_uuid)) ||
          (value.page_number != 0 && NonZero(value.continuation_uuid))) &&
         NonZero(value.redaction_profile_uuid) &&
         value.redaction_generation != 0 &&
         NonZero(value.policy_snapshot_uuid) && value.policy_generation != 0 &&
         NonZero(value.resource_budget_uuid) &&
         value.resource_budget_generation != 0 &&
         value.executor_availability_generation != 0;
}

inline bool ResultValid(const SblrResultPageResultV1& value) {
  const bool terminal = value.terminal_state == 1;
  return NonZero(value.cursor_uuid) && value.cursor_generation != 0 &&
         value.completion_state == 1 && value.terminal_state <= 1 &&
         NonZero(value.result_set_handle_uuid) &&
         value.result_set_handle_generation != 0 &&
         NonZero(value.row_descriptor_uuid) &&
         value.row_descriptor_generation != 0 &&
         OptionalIdentityValid(value.next_continuation_uuid,
                               value.next_continuation_generation) &&
         (terminal != NonZero(value.next_continuation_uuid)) &&
         NonZero(value.redaction_profile_uuid) &&
         NonZero(value.result_material_sha256) &&
         NonZero(value.executor_evidence_sha256) &&
         value.executor_availability_generation != 0 &&
         NonZero(value.publication_barrier_uuid) &&
         NonZero(value.result_evidence_uuid);
}

}  // namespace result_page_detail

inline std::vector<std::uint8_t> EncodeSblrResultPageRequestV1(
    const SblrResultPageRequestV1& value) {
  using namespace result_page_detail;
  if (!NonZero(value.statement_receipt_uuid) || value.occurrence == 0 ||
      value.catalog_generation == 0 || value.security_epoch == 0 ||
      value.resource_epoch == 0) {
    return {};
  }
  auto out = Header("SRPQ", 64);
  Put(&out, value.statement_receipt_uuid);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  return out;
}

inline bool DecodeSblrResultPageRequestV1(
    const std::uint8_t* in, std::size_t size, SblrResultPageRequestV1* output,
    std::string* detail = nullptr) {
  using namespace result_page_detail;
  if (output == nullptr || !HeaderValid(in, size, "SRPQ", 64)) {
    if (detail != nullptr) *detail = "result_page_request_invalid";
    return false;
  }
  SblrResultPageRequestV1 value;
  Get(in + 16, &value.statement_receipt_uuid);
  value.occurrence = GetLe(in + 32, 8);
  value.catalog_generation = GetLe(in + 40, 8);
  value.security_epoch = GetLe(in + 48, 8);
  value.resource_epoch = GetLe(in + 56, 8);
  if (EncodeSblrResultPageRequestV1(value) !=
      std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "result_page_request_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrResultPageDescriptorV1(
    const SblrResultPageDescriptorV1& value) {
  using namespace result_page_detail;
  if (!DescriptorValid(value)) return {};
  auto out = Header("SRPD", 288);
  Put(&out, value.cursor_uuid);
  PutLe(&out, value.cursor_generation, 8);
  Put(&out, value.statement_receipt_uuid);
  Put(&out, value.result_set_handle_uuid);
  PutLe(&out, value.result_set_handle_generation, 8);
  Put(&out, value.snapshot_uuid);
  Put(&out, value.row_descriptor_uuid);
  PutLe(&out, value.row_descriptor_generation, 8);
  PutLe(&out, value.page_number, 8);
  PutLe(&out, value.first_row_offset, 8);
  PutLe(&out, value.maximum_rows, 8);
  PutLe(&out, value.maximum_bytes, 8);
  Put(&out, value.continuation_uuid);
  PutLe(&out, value.continuation_generation, 8);
  Put(&out, value.redaction_profile_uuid);
  PutLe(&out, value.redaction_generation, 8);
  Put(&out, value.policy_snapshot_uuid);
  PutLe(&out, value.policy_generation, 8);
  Put(&out, value.resource_budget_uuid);
  PutLe(&out, value.resource_budget_generation, 8);
  out.insert(out.end(), 32, 0);
  PutLe(&out, value.executor_availability_generation, 8);
  const auto evidence = Hash(out.data(), out.size());
  if (NonZero(value.descriptor_sha256) &&
      value.descriptor_sha256 != evidence) {
    return {};
  }
  std::copy(evidence.begin(), evidence.end(), out.begin() + 248);
  return out;
}

inline bool DecodeSblrResultPageDescriptorV1(
    const std::uint8_t* in, std::size_t size,
    SblrResultPageDescriptorV1* output, std::string* detail = nullptr) {
  using namespace result_page_detail;
  if (output == nullptr || !HeaderValid(in, size, "SRPD", 288)) {
    if (detail != nullptr) *detail = "result_page_descriptor_header_invalid";
    return false;
  }
  SblrResultPageDescriptorV1 value;
  Get(in + 16, &value.cursor_uuid);
  value.cursor_generation = GetLe(in + 32, 8);
  Get(in + 40, &value.statement_receipt_uuid);
  Get(in + 56, &value.result_set_handle_uuid);
  value.result_set_handle_generation = GetLe(in + 72, 8);
  Get(in + 80, &value.snapshot_uuid);
  Get(in + 96, &value.row_descriptor_uuid);
  value.row_descriptor_generation = GetLe(in + 112, 8);
  value.page_number = GetLe(in + 120, 8);
  value.first_row_offset = GetLe(in + 128, 8);
  value.maximum_rows = GetLe(in + 136, 8);
  value.maximum_bytes = GetLe(in + 144, 8);
  Get(in + 152, &value.continuation_uuid);
  value.continuation_generation = GetLe(in + 168, 8);
  Get(in + 176, &value.redaction_profile_uuid);
  value.redaction_generation = GetLe(in + 192, 8);
  Get(in + 200, &value.policy_snapshot_uuid);
  value.policy_generation = GetLe(in + 216, 8);
  Get(in + 224, &value.resource_budget_uuid);
  value.resource_budget_generation = GetLe(in + 240, 8);
  Get(in + 248, &value.descriptor_sha256);
  value.executor_availability_generation = GetLe(in + 280, 8);
  if (!DescriptorValid(value) || !NonZero(value.descriptor_sha256)) {
    if (detail != nullptr) *detail = "result_page_descriptor_fields_invalid";
    return false;
  }
  std::vector<std::uint8_t> material(in, in + size);
  std::fill(material.begin() + 248, material.begin() + 280, 0);
  if (value.descriptor_sha256 != Hash(material.data(), material.size())) {
    if (detail != nullptr) *detail = "result_page_descriptor_hash_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrResultPageResultV1(
    const SblrResultPageResultV1& value) {
  using namespace result_page_detail;
  if (!ResultValid(value)) return {};
  auto out = Header("SRPR", 256);
  Put(&out, value.cursor_uuid);
  PutLe(&out, value.cursor_generation, 8);
  out.push_back(value.completion_state);
  out.push_back(value.terminal_state);
  out.insert(out.end(), 6, 0);
  Put(&out, value.result_set_handle_uuid);
  PutLe(&out, value.result_set_handle_generation, 8);
  Put(&out, value.row_descriptor_uuid);
  PutLe(&out, value.row_descriptor_generation, 8);
  PutLe(&out, value.returned_row_count, 8);
  PutLe(&out, value.next_row_offset, 8);
  Put(&out, value.next_continuation_uuid);
  PutLe(&out, value.next_continuation_generation, 8);
  Put(&out, value.redaction_profile_uuid);
  Put(&out, value.result_material_sha256);
  Put(&out, value.executor_evidence_sha256);
  PutLe(&out, value.executor_availability_generation, 8);
  Put(&out, value.publication_barrier_uuid);
  Put(&out, value.result_evidence_uuid);
  return out;
}

inline bool DecodeSblrResultPageResultV1(
    const std::uint8_t* in, std::size_t size, SblrResultPageResultV1* output,
    std::string* detail = nullptr) {
  using namespace result_page_detail;
  if (output == nullptr || !HeaderValid(in, size, "SRPR", 256) ||
      !Zero(in + 42, in + 48)) {
    if (detail != nullptr) *detail = "result_page_result_header_invalid";
    return false;
  }
  SblrResultPageResultV1 value;
  Get(in + 16, &value.cursor_uuid);
  value.cursor_generation = GetLe(in + 32, 8);
  value.completion_state = in[40];
  value.terminal_state = in[41];
  Get(in + 48, &value.result_set_handle_uuid);
  value.result_set_handle_generation = GetLe(in + 64, 8);
  Get(in + 72, &value.row_descriptor_uuid);
  value.row_descriptor_generation = GetLe(in + 88, 8);
  value.returned_row_count = GetLe(in + 96, 8);
  value.next_row_offset = GetLe(in + 104, 8);
  Get(in + 112, &value.next_continuation_uuid);
  value.next_continuation_generation = GetLe(in + 128, 8);
  Get(in + 136, &value.redaction_profile_uuid);
  Get(in + 152, &value.result_material_sha256);
  Get(in + 184, &value.executor_evidence_sha256);
  value.executor_availability_generation = GetLe(in + 216, 8);
  Get(in + 224, &value.publication_barrier_uuid);
  Get(in + 240, &value.result_evidence_uuid);
  if (!ResultValid(value) ||
      EncodeSblrResultPageResultV1(value) !=
          std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "result_page_result_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

}  // namespace scratchbird::engine::sblr

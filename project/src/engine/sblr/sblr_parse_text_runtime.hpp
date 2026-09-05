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

inline constexpr std::uint16_t kSblrParseTextOpcodeCode = 4868;
inline constexpr std::size_t kSblrParseTextRequestBytes = 64;
inline constexpr std::size_t kSblrParseTextDescriptorPrefixBytes = 256;
inline constexpr std::size_t kSblrParseTextResultPrefixBytes = 256;
inline constexpr std::uint32_t kSblrParseTextMaximumInputBytes = 16U * 1024U * 1024U;
inline constexpr std::uint16_t kSblrParseTextMaximumDepth = 1024;

using SblrParseTextUuidV1 = std::array<std::uint8_t, 16>;
using SblrParseTextSha256V1 = std::array<std::uint8_t, 32>;

struct SblrParseTextRequestV1 {
  SblrParseTextUuidV1 statement_receipt_uuid{};
  std::uint64_t occurrence = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
};

// The executable descriptor contains only the hash/limits of the source text
// and the parser-produced canonical SBLR container. Raw text is confined to
// the authenticated private bind request and can never become SBLR authority.
struct SblrParseTextDescriptorV1 {
  SblrParseTextUuidV1 parse_uuid{};
  SblrParseTextUuidV1 statement_receipt_uuid{};
  SblrParseTextUuidV1 language_profile_uuid{};
  std::uint64_t language_profile_generation = 0;
  SblrParseTextUuidV1 parser_package_uuid{};
  std::uint16_t parser_package_version_major = 0;
  std::uint16_t parser_package_version_minor = 0;
  std::uint32_t parser_package_version_patch = 0;
  SblrParseTextUuidV1 catalog_snapshot_uuid{};
  std::uint64_t catalog_generation = 0;
  SblrParseTextUuidV1 security_context_uuid{};
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint32_t input_byte_count = 0;
  SblrParseTextSha256V1 canonical_input_sha256{};
  std::uint32_t requested_maximum_bytes = 0;
  std::uint16_t requested_maximum_depth = 0;
  std::uint8_t extension_capability = 0;
  std::uint64_t executor_availability_generation = 0;
  SblrParseTextSha256V1 descriptor_sha256{};
  std::vector<std::uint8_t> canonical_sblr_bytes;
};

struct SblrParseTextResultV1 {
  SblrParseTextUuidV1 parse_uuid{};
  SblrParseTextUuidV1 statement_receipt_uuid{};
  SblrParseTextUuidV1 language_profile_uuid{};
  std::uint64_t language_profile_generation = 0;
  SblrParseTextUuidV1 parser_package_uuid{};
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::vector<std::uint8_t> canonical_sblr_bytes;
  std::uint8_t status = 1;
  std::uint8_t publication_barrier = 1;
  SblrParseTextUuidV1 parse_evidence_uuid{};
  SblrParseTextSha256V1 result_evidence_sha256{};
  SblrParseTextSha256V1 executor_evidence_sha256{};
  std::uint64_t executor_availability_generation = 0;
};

namespace parse_text_detail {

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

inline SblrParseTextSha256V1 Hash(const std::uint8_t* bytes,
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

inline SblrParseTextSha256V1 DomainHash(
    std::string_view domain, const std::vector<std::uint8_t>& bytes) {
  std::vector<std::uint8_t> material(domain.begin(), domain.end());
  material.insert(material.end(), bytes.begin(), bytes.end());
  return Hash(material.data(), material.size());
}

}  // namespace parse_text_detail

inline SblrParseTextSha256V1 SblrParseTextInputSha256V1(
    std::string_view canonical_input_utf8) {
  return parse_text_detail::Hash(
      reinterpret_cast<const std::uint8_t*>(canonical_input_utf8.data()),
      canonical_input_utf8.size());
}

inline std::vector<std::uint8_t> EncodeSblrParseTextRequestV1(
    const SblrParseTextRequestV1& value) {
  using namespace parse_text_detail;
  if (!NonZero(value.statement_receipt_uuid) || value.occurrence == 0 ||
      value.catalog_generation == 0 || value.security_epoch == 0 ||
      value.resource_epoch == 0) {
    return {};
  }
  auto out = Header("SBTQ", kSblrParseTextRequestBytes,
                    kSblrParseTextRequestBytes);
  Put(&out, value.statement_receipt_uuid);
  PutLe(&out, value.occurrence, 8);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  return out;
}

inline bool DecodeSblrParseTextRequestV1(
    const std::uint8_t* in, std::size_t size, SblrParseTextRequestV1* output,
    std::string* detail = nullptr) {
  using namespace parse_text_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "SBTQ", kSblrParseTextRequestBytes, true)) {
    if (detail != nullptr) *detail = "parse_text_request_header_invalid";
    return false;
  }
  SblrParseTextRequestV1 value;
  Get(in + 16, &value.statement_receipt_uuid);
  value.occurrence = GetLe(in + 32, 8);
  value.catalog_generation = GetLe(in + 40, 8);
  value.security_epoch = GetLe(in + 48, 8);
  value.resource_epoch = GetLe(in + 56, 8);
  if (EncodeSblrParseTextRequestV1(value) !=
      std::vector<std::uint8_t>(in, in + size)) {
    if (detail != nullptr) *detail = "parse_text_request_fields_invalid";
    return false;
  }
  *output = value;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrParseTextDescriptorV1(
    const SblrParseTextDescriptorV1& value) {
  using namespace parse_text_detail;
  if (!NonZero(value.parse_uuid) ||
      !NonZero(value.statement_receipt_uuid) ||
      !NonZero(value.language_profile_uuid) ||
      value.language_profile_generation == 0 ||
      !NonZero(value.parser_package_uuid) ||
      value.parser_package_version_major == 0 ||
      !NonZero(value.catalog_snapshot_uuid) || value.catalog_generation == 0 ||
      !NonZero(value.security_context_uuid) || value.security_epoch == 0 ||
      value.resource_epoch == 0 || value.input_byte_count == 0 ||
      value.input_byte_count > kSblrParseTextMaximumInputBytes ||
      !NonZero(value.canonical_input_sha256) ||
      value.requested_maximum_bytes < value.input_byte_count ||
      value.requested_maximum_bytes > kSblrParseTextMaximumInputBytes ||
      value.requested_maximum_depth == 0 ||
      value.requested_maximum_depth > kSblrParseTextMaximumDepth ||
      value.extension_capability > 1 ||
      value.executor_availability_generation == 0 ||
      value.canonical_sblr_bytes.empty() ||
      value.canonical_sblr_bytes.size() > kSblrParseTextMaximumInputBytes ||
      value.canonical_sblr_bytes.size() >
          std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  auto out = Header("SPTD", kSblrParseTextDescriptorPrefixBytes,
                    kSblrParseTextDescriptorPrefixBytes +
                        value.canonical_sblr_bytes.size());
  Put(&out, value.parse_uuid);
  Put(&out, value.statement_receipt_uuid);
  Put(&out, value.language_profile_uuid);
  PutLe(&out, value.language_profile_generation, 8);
  Put(&out, value.parser_package_uuid);
  PutLe(&out, value.parser_package_version_major, 2);
  PutLe(&out, value.parser_package_version_minor, 2);
  PutLe(&out, value.parser_package_version_patch, 4);
  Put(&out, value.catalog_snapshot_uuid);
  PutLe(&out, value.catalog_generation, 8);
  Put(&out, value.security_context_uuid);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  PutLe(&out, value.input_byte_count, 4);
  Put(&out, value.canonical_input_sha256);
  PutLe(&out, value.requested_maximum_bytes, 4);
  PutLe(&out, value.requested_maximum_depth, 2);
  out.push_back(value.extension_capability);
  out.push_back(0);
  PutLe(&out, value.executor_availability_generation, 8);
  out.insert(out.end(), 32, 0);
  PutLe(&out, value.canonical_sblr_bytes.size(), 4);
  out.insert(out.end(), 16, 0);
  out.insert(out.end(), value.canonical_sblr_bytes.begin(),
             value.canonical_sblr_bytes.end());
  const auto descriptor = DomainHash(
      "ScratchBird.SblrParseTextDescriptor.V1", out);
  if (NonZero(value.descriptor_sha256) &&
      value.descriptor_sha256 != descriptor) {
    return {};
  }
  std::copy(descriptor.begin(), descriptor.end(), out.begin() + 204);
  return out;
}

inline bool DecodeSblrParseTextDescriptorV1(
    const std::uint8_t* in, std::size_t size,
    SblrParseTextDescriptorV1* output, std::string* detail = nullptr) {
  using namespace parse_text_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "SPTD", kSblrParseTextDescriptorPrefixBytes,
                   false) ||
      !Zero(in + 195, in + 196) || !Zero(in + 240, in + 256)) {
    if (detail != nullptr) *detail = "parse_text_descriptor_header_invalid";
    return false;
  }
  SblrParseTextDescriptorV1 value;
  Get(in + 16, &value.parse_uuid);
  Get(in + 32, &value.statement_receipt_uuid);
  Get(in + 48, &value.language_profile_uuid);
  value.language_profile_generation = GetLe(in + 64, 8);
  Get(in + 72, &value.parser_package_uuid);
  value.parser_package_version_major = GetLe(in + 88, 2);
  value.parser_package_version_minor = GetLe(in + 90, 2);
  value.parser_package_version_patch = GetLe(in + 92, 4);
  Get(in + 96, &value.catalog_snapshot_uuid);
  value.catalog_generation = GetLe(in + 112, 8);
  Get(in + 120, &value.security_context_uuid);
  value.security_epoch = GetLe(in + 136, 8);
  value.resource_epoch = GetLe(in + 144, 8);
  value.input_byte_count = GetLe(in + 152, 4);
  Get(in + 156, &value.canonical_input_sha256);
  value.requested_maximum_bytes = GetLe(in + 188, 4);
  value.requested_maximum_depth = GetLe(in + 192, 2);
  value.extension_capability = in[194];
  value.executor_availability_generation = GetLe(in + 196, 8);
  Get(in + 204, &value.descriptor_sha256);
  const auto sblr_bytes = GetLe(in + 236, 4);
  if (sblr_bytes != size - kSblrParseTextDescriptorPrefixBytes) {
    if (detail != nullptr) *detail = "parse_text_descriptor_extent_invalid";
    return false;
  }
  value.canonical_sblr_bytes.assign(
      in + kSblrParseTextDescriptorPrefixBytes, in + size);
  const auto encoded = EncodeSblrParseTextDescriptorV1(value);
  if (encoded.size() != size || !std::equal(encoded.begin(), encoded.end(), in)) {
    if (detail != nullptr) *detail = "parse_text_descriptor_noncanonical";
    return false;
  }
  *output = std::move(value);
  return true;
}

inline SblrParseTextSha256V1 SblrParseTextResultEvidenceSha256V1(
    const std::vector<std::uint8_t>& canonical_result_with_zero_evidence) {
  return parse_text_detail::DomainHash(
      "ScratchBird.SblrParseTextResult.V1",
      canonical_result_with_zero_evidence);
}

inline SblrParseTextSha256V1 SblrParseTextExecutorEvidenceSha256V1(
    const SblrParseTextSha256V1& result_evidence,
    std::uint64_t availability_generation) {
  using namespace parse_text_detail;
  std::vector<std::uint8_t> material(
      {'S','c','r','a','t','c','h','B','i','r','d','.',
       'S','b','l','r','P','a','r','s','e','T','e','x','t',
       'E','x','e','c','u','t','o','r','E','v','i','d','e','n','c','e','.',
       'V','1'});
  Put(&material, result_evidence);
  PutLe(&material, availability_generation, 8);
  return Hash(material.data(), material.size());
}

inline std::vector<std::uint8_t> EncodeSblrParseTextResultV1(
    const SblrParseTextResultV1& value) {
  using namespace parse_text_detail;
  if (!NonZero(value.parse_uuid) ||
      !NonZero(value.statement_receipt_uuid) ||
      !NonZero(value.language_profile_uuid) ||
      value.language_profile_generation == 0 ||
      !NonZero(value.parser_package_uuid) || value.catalog_generation == 0 ||
      value.security_epoch == 0 || value.resource_epoch == 0 ||
      value.canonical_sblr_bytes.empty() ||
      value.canonical_sblr_bytes.size() > kSblrParseTextMaximumInputBytes ||
      value.status != 1 || value.publication_barrier != 1 ||
      !NonZero(value.parse_evidence_uuid) ||
      value.executor_availability_generation == 0) {
    return {};
  }
  auto out = Header("SPTR", kSblrParseTextResultPrefixBytes,
                    kSblrParseTextResultPrefixBytes +
                        value.canonical_sblr_bytes.size());
  Put(&out, value.parse_uuid);
  Put(&out, value.statement_receipt_uuid);
  Put(&out, value.language_profile_uuid);
  PutLe(&out, value.language_profile_generation, 8);
  Put(&out, value.parser_package_uuid);
  PutLe(&out, value.catalog_generation, 8);
  PutLe(&out, value.security_epoch, 8);
  PutLe(&out, value.resource_epoch, 8);
  PutLe(&out, value.canonical_sblr_bytes.size(), 8);
  Put(&out, Hash(value.canonical_sblr_bytes.data(),
                 value.canonical_sblr_bytes.size()));
  out.push_back(value.status);
  out.push_back(value.publication_barrier);
  PutLe(&out, 0, 2);
  Put(&out, value.parse_evidence_uuid);
  out.insert(out.end(), 64, 0);
  PutLe(&out, value.executor_availability_generation, 8);
  out.insert(out.end(), 12, 0);
  out.insert(out.end(), value.canonical_sblr_bytes.begin(),
             value.canonical_sblr_bytes.end());
  const auto result_evidence = SblrParseTextResultEvidenceSha256V1(out);
  const auto executor_evidence = SblrParseTextExecutorEvidenceSha256V1(
      result_evidence, value.executor_availability_generation);
  if ((NonZero(value.result_evidence_sha256) &&
       value.result_evidence_sha256 != result_evidence) ||
      (NonZero(value.executor_evidence_sha256) &&
       value.executor_evidence_sha256 != executor_evidence)) {
    return {};
  }
  std::copy(result_evidence.begin(), result_evidence.end(), out.begin() + 172);
  std::copy(executor_evidence.begin(), executor_evidence.end(), out.begin() + 204);
  return out;
}

inline bool DecodeSblrParseTextResultV1(
    const std::uint8_t* in, std::size_t size, SblrParseTextResultV1* output,
    std::string* detail = nullptr) {
  using namespace parse_text_detail;
  if (output == nullptr ||
      !HeaderValid(in, size, "SPTR", kSblrParseTextResultPrefixBytes, false) ||
      !Zero(in + 154, in + 156) || !Zero(in + 244, in + 256)) {
    if (detail != nullptr) *detail = "parse_text_result_header_invalid";
    return false;
  }
  SblrParseTextResultV1 value;
  Get(in + 16, &value.parse_uuid);
  Get(in + 32, &value.statement_receipt_uuid);
  Get(in + 48, &value.language_profile_uuid);
  value.language_profile_generation = GetLe(in + 64, 8);
  Get(in + 72, &value.parser_package_uuid);
  value.catalog_generation = GetLe(in + 88, 8);
  value.security_epoch = GetLe(in + 96, 8);
  value.resource_epoch = GetLe(in + 104, 8);
  const auto sblr_bytes = GetLe(in + 112, 8);
  SblrParseTextSha256V1 sblr_sha{};
  Get(in + 120, &sblr_sha);
  value.status = in[152];
  value.publication_barrier = in[153];
  Get(in + 156, &value.parse_evidence_uuid);
  Get(in + 172, &value.result_evidence_sha256);
  Get(in + 204, &value.executor_evidence_sha256);
  value.executor_availability_generation = GetLe(in + 236, 8);
  if (sblr_bytes != size - kSblrParseTextResultPrefixBytes) {
    if (detail != nullptr) *detail = "parse_text_result_extent_invalid";
    return false;
  }
  value.canonical_sblr_bytes.assign(in + kSblrParseTextResultPrefixBytes,
                                    in + size);
  if (value.executor_availability_generation == 0 ||
      SblrParseTextExecutorEvidenceSha256V1(
          value.result_evidence_sha256,
          value.executor_availability_generation) !=
          value.executor_evidence_sha256 ||
      sblr_sha != Hash(value.canonical_sblr_bytes.data(),
                       value.canonical_sblr_bytes.size())) {
    if (detail != nullptr) *detail = "parse_text_result_evidence_invalid";
    return false;
  }
  const auto encoded = EncodeSblrParseTextResultV1(value);
  if (encoded.size() != size || !std::equal(encoded.begin(), encoded.end(), in)) {
    if (detail != nullptr) *detail = "parse_text_result_noncanonical";
    return false;
  }
  *output = std::move(value);
  return true;
}

}  // namespace scratchbird::engine::sblr

#include "sblr_bulk_import_stream_runtime.hpp"

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <string_view>

namespace scratchbird::engine::sblr {
namespace {

void AppendLe(std::vector<std::uint8_t>* output, std::uint64_t value,
              std::size_t extent) {
  for (std::size_t index = 0; index < extent; ++index) {
    output->push_back(
        static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

std::uint64_t ReadLe(const std::uint8_t* bytes, std::size_t extent) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < extent; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
  }
  return value;
}

template <typename Container>
bool AnyNonZero(const Container& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

template <std::size_t N>
bool FieldNonZero(const std::array<std::uint8_t, N>& body,
                  std::size_t offset, std::size_t extent) {
  return offset <= body.size() && extent <= body.size() - offset &&
         std::any_of(body.begin() + offset, body.begin() + offset + extent,
                     [](std::uint8_t byte) { return byte != 0; });
}

std::vector<std::uint8_t> Header(std::string_view magic,
                                 std::size_t extent) {
  std::vector<std::uint8_t> output(magic.begin(), magic.end());
  AppendLe(&output, 1, 2);
  AppendLe(&output, extent, 2);
  AppendLe(&output, extent, 4);
  AppendLe(&output, 0, 4);
  return output;
}

bool HeaderValid(const std::uint8_t* bytes, std::size_t size,
                 std::string_view magic, std::size_t extent) {
  return bytes != nullptr && size == extent && magic.size() == 4 &&
         std::equal(magic.begin(), magic.end(), bytes) &&
         ReadLe(bytes + 4, 2) == 1 && ReadLe(bytes + 6, 2) == extent &&
         ReadLe(bytes + 8, 4) == extent &&
         std::all_of(bytes + 12, bytes + 16,
                     [](std::uint8_t byte) { return byte == 0; });
}

BulkImportSha Evidence(std::string_view domain, const std::uint8_t* bytes,
                       std::size_t size) {
  std::vector<std::uint8_t> material(domain.begin(), domain.end());
  material.insert(material.end(), bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

bool DescriptorBodyValid(const SblrBulkImportStreamDescriptorV1& value,
                         std::string* detail) {
  const auto& body = value.canonical_body;
  const std::uint32_t flags =
      static_cast<std::uint32_t>(ReadLe(body.data() + 28, 4));
  const bool cluster_bound = (flags & 1U) != 0;
  const bool cluster_fence_present = FieldNonZero(body, 352, 16);
  const bool valid =
      FieldNonZero(body, 0, 16) && ReadLe(body.data() + 16, 8) != 0 &&
      ReadLe(body.data() + 24, 4) != 0 && (flags & ~1U) == 0 &&
      FieldNonZero(body, 32, 16) && ReadLe(body.data() + 48, 8) != 0 &&
      FieldNonZero(body, 56, 16) && ReadLe(body.data() + 72, 8) != 0 &&
      FieldNonZero(body, 80, 16) && ReadLe(body.data() + 96, 8) != 0 &&
      FieldNonZero(body, 104, 16) && FieldNonZero(body, 120, 16) &&
      ReadLe(body.data() + 136, 8) != 0 &&
      FieldNonZero(body, 144, 16) && ReadLe(body.data() + 160, 8) != 0 &&
      FieldNonZero(body, 168, 16) && ReadLe(body.data() + 184, 8) != 0 &&
      FieldNonZero(body, 192, 16) && ReadLe(body.data() + 208, 8) != 0 &&
      FieldNonZero(body, 216, 16) && ReadLe(body.data() + 232, 8) != 0 &&
      FieldNonZero(body, 240, 16) && ReadLe(body.data() + 256, 8) != 0 &&
      FieldNonZero(body, 264, 32) && FieldNonZero(body, 296, 32) &&
      FieldNonZero(body, 328, 16) && ReadLe(body.data() + 344, 8) != 0 &&
      cluster_bound == cluster_fence_present &&
      value.availability_generation != 0;
  if (!valid && detail != nullptr) {
    *detail = "bulk import descriptor semantic fields are incomplete";
  }
  return valid;
}

bool ResultBodyValid(const SblrBulkImportStreamResultV1& value,
                     std::string* detail) {
  const auto& body = value.canonical_body;
  const auto affected = ReadLe(body.data() + 48, 8);
  const auto rejected = ReadLe(body.data() + 56, 8);
  const auto input_bytes = ReadLe(body.data() + 64, 8);
  const auto chunks = ReadLe(body.data() + 72, 8);
  const bool valid =
      FieldNonZero(body, 0, 16) && ReadLe(body.data() + 16, 8) != 0 &&
      FieldNonZero(body, 24, 16) && ReadLe(body.data() + 40, 8) != 0 &&
      !std::equal(body.begin(), body.begin() + 16, body.begin() + 24) &&
      affected != 0 && affected <= 1'048'576ULL && rejected == 0 &&
      input_bytes != 0 && input_bytes <= 17'179'869'184ULL &&
      chunks != 0 && chunks <= 262'144ULL &&
      FieldNonZero(body, 80, 16) && ReadLe(body.data() + 96, 8) != 0 &&
      FieldNonZero(body, 104, 32) && value.availability_generation != 0;
  if (!valid && detail != nullptr) {
    *detail = "bulk import result semantic fields are incomplete";
  }
  return valid;
}

}  // namespace

BulkImportWireKind BulkImportWireKindOf(const std::uint8_t* bytes,
                                        std::size_t size) {
  if (bytes == nullptr || size < 4) {
    return static_cast<BulkImportWireKind>(0);
  }
  return static_cast<BulkImportWireKind>(ReadLe(bytes, 4));
}

const BulkImportUuid& RequestReceipt(
    const SblrBulkImportStreamRequestV1& value) {
  return value.receipt;
}

std::uint64_t RequestOccurrence(
    const SblrBulkImportStreamRequestV1& value) {
  return value.occurrence;
}

std::uint32_t RequestImportOccurrence(
    const SblrBulkImportStreamRequestV1& value) {
  return value.import_occurrence;
}

const std::array<std::uint8_t, 368>& DescriptorCanonicalBody(
    const SblrBulkImportStreamDescriptorV1& value) {
  return value.canonical_body;
}

const BulkImportSha& DescriptorEvidence(
    const SblrBulkImportStreamDescriptorV1& value) {
  return value.evidence;
}

std::uint64_t DescriptorAvailabilityGeneration(
    const SblrBulkImportStreamDescriptorV1& value) {
  return value.availability_generation;
}

const std::array<std::uint8_t, 136>& ResultCanonicalBody(
    const SblrBulkImportStreamResultV1& value) {
  return value.canonical_body;
}

const BulkImportSha& ResultEvidence(
    const SblrBulkImportStreamResultV1& value) {
  return value.evidence;
}

std::uint64_t ResultAvailabilityGeneration(
    const SblrBulkImportStreamResultV1& value) {
  return value.availability_generation;
}

bool ValidateSblrBulkImportStreamDescriptorV1(
    const SblrBulkImportStreamDescriptorV1& value, std::string* detail) {
  return DescriptorBodyValid(value, detail);
}

bool ValidateSblrBulkImportStreamResultV1(
    const SblrBulkImportStreamResultV1& value, std::string* detail) {
  return ResultBodyValid(value, detail);
}

std::vector<std::uint8_t> EncodeSblrBulkImportStreamRequestV1(
    const SblrBulkImportStreamRequestV1& value) {
  if (!AnyNonZero(value.receipt) || value.occurrence == 0 ||
      value.import_occurrence == 0) {
    return {};
  }
  auto output = Header("BIRQ", BulkImportWireLayout::request_size);
  output.insert(output.end(), value.receipt.begin(), value.receipt.end());
  AppendLe(&output, value.occurrence, 8);
  AppendLe(&output, value.import_occurrence, 4);
  output.insert(output.end(), 20, 0);
  return output;
}

bool DecodeSblrBulkImportStreamRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrBulkImportStreamRequestV1* output, std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "BIRQ", BulkImportWireLayout::request_size) ||
      !std::all_of(bytes + 44, bytes + 64,
                   [](std::uint8_t byte) { return byte == 0; })) {
    if (detail != nullptr) *detail = "bulk import request invalid";
    return false;
  }
  SblrBulkImportStreamRequestV1 value;
  std::copy_n(bytes + 16, 16, value.receipt.begin());
  value.occurrence = ReadLe(bytes + 32, 8);
  value.import_occurrence =
      static_cast<std::uint32_t>(ReadLe(bytes + 40, 4));
  if (EncodeSblrBulkImportStreamRequestV1(value).empty()) {
    if (detail != nullptr) *detail = "bulk import request identity invalid";
    return false;
  }
  *output = value;
  return true;
}

std::vector<std::uint8_t> EncodeSblrBulkImportStreamDescriptorV1(
    const SblrBulkImportStreamDescriptorV1& value, bool operation) {
  if (!DescriptorBodyValid(value, nullptr)) {
    return {};
  }
  auto output = Header(operation ? "BIRO" : "BIRD",
                       BulkImportWireLayout::descriptor_size);
  output.insert(output.end(), value.canonical_body.begin(),
                value.canonical_body.end());
  const auto evidence = Evidence(
      "ScratchBird.SblrBulkImportStreamDescriptor.V1", output.data() + 16,
      value.canonical_body.size());
  if (AnyNonZero(value.evidence) && value.evidence != evidence) {
    return {};
  }
  output.insert(output.end(), evidence.begin(), evidence.end());
  AppendLe(&output, value.availability_generation, 8);
  return output;
}

bool DecodeSblrBulkImportStreamDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrBulkImportStreamDescriptorV1* output, std::string* detail,
    bool operation) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, operation ? "BIRO" : "BIRD",
                   BulkImportWireLayout::descriptor_size)) {
    if (detail != nullptr) *detail = "bulk import descriptor invalid";
    return false;
  }
  SblrBulkImportStreamDescriptorV1 value;
  std::copy_n(bytes + 16, value.canonical_body.size(),
              value.canonical_body.begin());
  std::copy_n(bytes + 384, value.evidence.size(), value.evidence.begin());
  value.availability_generation = ReadLe(bytes + 416, 8);
  if (!AnyNonZero(value.evidence) ||
      !DescriptorBodyValid(value, detail) ||
      EncodeSblrBulkImportStreamDescriptorV1(value, operation).empty()) {
    if (detail != nullptr && detail->empty()) {
      *detail = "bulk import descriptor evidence invalid";
    }
    return false;
  }
  *output = value;
  return true;
}

std::vector<std::uint8_t> EncodeSblrBulkImportStreamResultV1(
    const SblrBulkImportStreamResultV1& value) {
  if (!ResultBodyValid(value, nullptr)) {
    return {};
  }
  auto output = Header("BIRS", BulkImportWireLayout::result_size);
  output.insert(output.end(), value.canonical_body.begin(),
                value.canonical_body.end());
  const auto evidence = Evidence(
      "ScratchBird.SblrBulkImportStreamResult.V1", output.data() + 16,
      value.canonical_body.size());
  if (AnyNonZero(value.evidence) && value.evidence != evidence) {
    return {};
  }
  output.insert(output.end(), evidence.begin(), evidence.end());
  AppendLe(&output, value.availability_generation, 8);
  return output;
}

bool DecodeSblrBulkImportStreamResultV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrBulkImportStreamResultV1* output, std::string* detail) {
  if (output == nullptr ||
      !HeaderValid(bytes, size, "BIRS", BulkImportWireLayout::result_size)) {
    if (detail != nullptr) *detail = "BIRS invalid";
    return false;
  }
  SblrBulkImportStreamResultV1 value;
  std::copy_n(bytes + 16, value.canonical_body.size(),
              value.canonical_body.begin());
  std::copy_n(bytes + 152, value.evidence.size(), value.evidence.begin());
  value.availability_generation = ReadLe(bytes + 184, 8);
  if (!AnyNonZero(value.evidence) || !ResultBodyValid(value, detail) ||
      EncodeSblrBulkImportStreamResultV1(value).empty()) {
    if (detail != nullptr && detail->empty()) {
      *detail = "BIRS evidence invalid";
    }
    return false;
  }
  *output = value;
  return true;
}

}  // namespace scratchbird::engine::sblr

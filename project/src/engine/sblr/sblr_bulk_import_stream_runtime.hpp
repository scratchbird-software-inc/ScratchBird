#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

using BulkImportUuid = std::array<std::uint8_t, 16>;
using BulkImportSha = std::array<std::uint8_t, 32>;

enum class BulkImportWireKind : std::uint32_t {
  request = 0x51524942,
  descriptor = 0x44524942,
  operation = 0x4f524942,
  result = 0x53524942,
};

struct BulkImportWireLayout {
  static constexpr std::size_t request_size = 64;
  static constexpr std::size_t descriptor_size = 424;
  static constexpr std::size_t result_size = 192;
};

struct SblrBulkImportStreamRequestV1 {
  BulkImportUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t import_occurrence = 0;
};
struct SblrBulkImportStreamDescriptorV1 {
  std::array<std::uint8_t, 368> canonical_body{};
  BulkImportSha evidence{};
  std::uint64_t availability_generation = 0;
};
struct SblrBulkImportStreamResultV1 {
  std::array<std::uint8_t, 136> canonical_body{};
  BulkImportSha evidence{};
  std::uint64_t availability_generation = 0;
};

// Typed accessors keep field offsets out of engine callers while retaining the
// Core V1 byte layout and its domain-separated evidence validation.
BulkImportWireKind BulkImportWireKindOf(const std::uint8_t*, std::size_t);
const BulkImportUuid& RequestReceipt(const SblrBulkImportStreamRequestV1&);
std::uint64_t RequestOccurrence(const SblrBulkImportStreamRequestV1&);
std::uint32_t RequestImportOccurrence(const SblrBulkImportStreamRequestV1&);
const std::array<std::uint8_t, 368>& DescriptorCanonicalBody(const SblrBulkImportStreamDescriptorV1&);
const BulkImportSha& DescriptorEvidence(const SblrBulkImportStreamDescriptorV1&);
std::uint64_t DescriptorAvailabilityGeneration(const SblrBulkImportStreamDescriptorV1&);
const std::array<std::uint8_t, 136>& ResultCanonicalBody(const SblrBulkImportStreamResultV1&);
const BulkImportSha& ResultEvidence(const SblrBulkImportStreamResultV1&);
std::uint64_t ResultAvailabilityGeneration(const SblrBulkImportStreamResultV1&);
bool ValidateSblrBulkImportStreamDescriptorV1(
    const SblrBulkImportStreamDescriptorV1&, std::string* = nullptr);
bool ValidateSblrBulkImportStreamResultV1(
    const SblrBulkImportStreamResultV1&, std::string* = nullptr);

std::vector<std::uint8_t> EncodeSblrBulkImportStreamRequestV1(const SblrBulkImportStreamRequestV1&);
bool DecodeSblrBulkImportStreamRequestV1(const std::uint8_t*, std::size_t, SblrBulkImportStreamRequestV1*, std::string*);
std::vector<std::uint8_t> EncodeSblrBulkImportStreamDescriptorV1(const SblrBulkImportStreamDescriptorV1&, bool);
bool DecodeSblrBulkImportStreamDescriptorV1(const std::uint8_t*, std::size_t, SblrBulkImportStreamDescriptorV1*, std::string*, bool);
std::vector<std::uint8_t> EncodeSblrBulkImportStreamResultV1(const SblrBulkImportStreamResultV1&);
bool DecodeSblrBulkImportStreamResultV1(const std::uint8_t*, std::size_t, SblrBulkImportStreamResultV1*, std::string*);
}

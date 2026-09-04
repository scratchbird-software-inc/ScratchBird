#include "sblr_dml_conditional_mutate_runtime.hpp"
#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <cstring>

namespace scratchbird::engine::sblr {
namespace {

void PutU64(std::vector<std::uint8_t>& bytes, std::size_t offset,
            std::uint64_t value) {
  for (unsigned i = 0; i != 8; ++i)
    bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

std::uint64_t GetU64(const std::uint8_t* bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i != 8; ++i)
    value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8);
  return value;
}

bool Header(const std::uint8_t* bytes, std::size_t size, const char* magic,
            std::size_t expected, std::string* diagnostic) {
  if (bytes == nullptr || size != expected || std::memcmp(bytes, magic, 4) != 0) {
    if (diagnostic != nullptr) *diagnostic = "conditional_mutate_wire_invalid";
    return false;
  }
  if (bytes[4] != 1 || bytes[5] != 0 || bytes[6] != 0 || bytes[7] != 0) {
    if (diagnostic != nullptr) *diagnostic = "conditional_mutate_header_invalid";
    return false;
  }
  return true;
}

bool NonZero(const std::uint8_t* bytes, std::size_t size) {
  for (std::size_t i = 0; i != size; ++i)
    if (bytes[i] != 0) return true;
  return false;
}

std::array<std::uint8_t, 32> Evidence(const char* domain,
                                      const std::uint8_t* bytes,
                                      std::size_t size) {
  std::vector<std::uint8_t> material(domain, domain + std::strlen(domain));
  material.insert(material.end(), bytes, bytes + size);
  std::array<std::uint8_t, 32> evidence{};
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(material);
  if (digest.ok()) std::copy(digest.digest.begin(), digest.digest.end(), evidence.begin());
  return evidence;
}

}  // namespace

std::vector<std::uint8_t> EncodeSblrDmlConditionalMutateRequestV1(
    const SblrDmlConditionalMutateRequestV1& value) {
  std::vector<std::uint8_t> bytes(64, 0);
  std::memcpy(bytes.data(), "CMRQ", 4);
  bytes[4] = 1;
  std::memcpy(bytes.data() + 16, value.receipt.data(), value.receipt.size());
  PutU64(bytes, 32, value.occurrence);
  PutU64(bytes, 40, value.mutation_occurrence);
  return bytes;
}

bool DecodeSblrDmlConditionalMutateRequestV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDmlConditionalMutateRequestV1* value, std::string* diagnostic) {
  if (value == nullptr || !Header(bytes, size, "CMRQ", 64, diagnostic)) return false;
  if (!NonZero(bytes + 16, 16) || GetU64(bytes, 32) == 0 || GetU64(bytes, 40) == 0) {
    if (diagnostic != nullptr) *diagnostic = "conditional_mutate_request_identity_invalid";
    return false;
  }
  std::memcpy(value->receipt.data(), bytes + 16, value->receipt.size());
  value->occurrence = GetU64(bytes, 32);
  value->mutation_occurrence = GetU64(bytes, 40);
  return true;
}

std::vector<std::uint8_t> EncodeSblrDmlConditionalMutateDescriptorV1(
    const SblrDmlConditionalMutateDescriptorV1& value, bool operand) {
  std::vector<std::uint8_t> bytes(488, 0);
  std::memcpy(bytes.data(), operand ? "CMO" : "CMDT", 4);
  bytes[4] = 1;
  std::memcpy(bytes.data() + 16, value.body.data(), 400);
  const auto evidence = Evidence("ScratchBird.SblrDmlConditionalMutateDescriptor.V1", bytes.data() + 16, 400);
  if (NonZero(value.evidence.data(), value.evidence.size()) && value.evidence != evidence) return {};
  std::memcpy(bytes.data() + 416, evidence.data(), evidence.size());
  PutU64(bytes, 448, value.availability);
  return bytes;
}

bool DecodeSblrDmlConditionalMutateDescriptorV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDmlConditionalMutateDescriptorV1* value, std::string* diagnostic,
    bool operand) {
  if (value == nullptr || !Header(bytes, size, operand ? "CMO" : "CMDT", 488, diagnostic))
    return false;
  if (GetU64(bytes, 448) == 0 || !NonZero(bytes + 416, 32)) {
    if (diagnostic != nullptr) *diagnostic = "conditional_mutate_descriptor_evidence_invalid";
    return false;
  }
  std::memcpy(value->body.data(), bytes + 16, value->body.size());
  std::memcpy(value->evidence.data(), bytes + 416, value->evidence.size());
  value->availability = GetU64(bytes, 448);
  return Evidence("ScratchBird.SblrDmlConditionalMutateDescriptor.V1", bytes + 16, 400) == value->evidence;
}

std::vector<std::uint8_t> EncodeSblrDmlConditionalMutateResultV1(
    const SblrDmlConditionalMutateResultV1& value) {
  std::vector<std::uint8_t> bytes(320, 0);
  std::memcpy(bytes.data(), "CMR", 4);
  bytes[4] = 1;
  std::memcpy(bytes.data() + 16, value.body.data(), value.body.size());
  const auto evidence = Evidence("ScratchBird.SblrDmlConditionalMutateResult.V1", bytes.data() + 16, 240);
  if (NonZero(value.evidence.data(), value.evidence.size()) && value.evidence != evidence) return {};
  std::memcpy(bytes.data() + 256, evidence.data(), evidence.size());
  PutU64(bytes, 288, value.availability);
  std::memcpy(bytes.data() + 296, value.publication_barrier.data(),
              value.publication_barrier.size());
  return bytes;
}

bool DecodeSblrDmlConditionalMutateResultV1(
    const std::uint8_t* bytes, std::size_t size,
    SblrDmlConditionalMutateResultV1* value, std::string* diagnostic) {
  if (value == nullptr || !Header(bytes, size, "CMR", 320, diagnostic)) return false;
  if (GetU64(bytes, 288) == 0 || !NonZero(bytes + 256, 32) ||
      !NonZero(bytes + 296, 16)) {
    if (diagnostic != nullptr) *diagnostic = "conditional_mutate_result_publication_invalid";
    return false;
  }
  std::memcpy(value->body.data(), bytes + 16, value->body.size());
  std::memcpy(value->evidence.data(), bytes + 256, value->evidence.size());
  value->availability = GetU64(bytes, 288);
  std::memcpy(value->publication_barrier.data(), bytes + 296,
              value->publication_barrier.size());
  return Evidence("ScratchBird.SblrDmlConditionalMutateResult.V1", bytes + 16, 240) == value->evidence;
}

}  // namespace scratchbird::engine::sblr

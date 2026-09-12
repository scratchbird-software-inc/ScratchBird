// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_table_truncate_runtime.hpp"
#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string_view>

namespace scratchbird::engine::sblr {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::string_view kDescriptorDomain = "ScratchBird.SblrTableTruncateDescriptor.V1";
constexpr std::string_view kResultDomain = "ScratchBird.SblrTableTruncateResult.V1";

void Put(Bytes& bytes, std::size_t offset, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i)
    bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::uint64_t Read(const std::uint8_t* bytes, unsigned width) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < width; ++i) value |= std::uint64_t(bytes[i]) << (8 * i);
  return value;
}
bool Nonzero(const std::uint8_t* bytes, std::size_t size) {
  return std::any_of(bytes, bytes + size, [](auto byte) { return byte != 0; });
}
bool SystemUuid(const std::uint8_t* bytes) {
  return (bytes[6] & 0xf0) == 0x70 && (bytes[8] & 0xc0) == 0x80;
}
bool Failure(std::string* detail, const char* reason) noexcept {
  if (detail) {
    try { *detail = reason; }
    catch (const std::bad_alloc&) {}
    catch (const std::length_error&) {}
  }
  return false;
}
bool Header(const std::uint8_t* bytes, std::size_t size,
            const char* magic, std::size_t expected) {
  return bytes && size == expected && std::memcmp(bytes, magic, 4) == 0 &&
      Read(bytes + 4, 2) == 1 && Read(bytes + 6, 2) == expected &&
      Read(bytes + 8, 4) == expected && Read(bytes + 12, 4) == 0;
}
Bytes Frame(const char* magic, std::size_t size) {
  Bytes bytes(size, 0);
  std::copy_n(magic, 4, bytes.begin());
  Put(bytes, 4, 1, 2); Put(bytes, 6, size, 2); Put(bytes, 8, size, 4);
  return bytes;
}
bool Hash(std::string_view domain, const std::uint8_t* bytes,
          std::size_t size, TableTruncateSha* output) {
  Bytes material(domain.begin(), domain.end());
  material.insert(material.end(), bytes, bytes + size);
  const auto hashed = scratchbird::core::hash::ComputeSha256Digest(material);
  if (!hashed.ok() || hashed.digest_bytes != output->size()) return false;
  *output = hashed.digest;
  return true;
}
// Absolute wire offsets from SBLR_TABLE_TRUNCATE_CANONICAL_FIELDS_V1.
// The caller supplies exactly the fixed-size body, starting at wire offset16.
std::uint64_t Field(const std::uint8_t* body, std::size_t offset, unsigned width) {
  return Read(body + offset - 16, width);
}
bool DescriptorFields(const SblrTableTruncateDescriptorV1& value) {
  const auto* body = value.canonical_body.data();
  for (const auto offset : {16,32,48,64,80,96,112,272,288,304,320})
    if (!SystemUuid(body + offset - 16)) return false;
  for (const auto offset : {128,136,176,184})
    if (Field(body, offset, 8) == 0) return false;
  if (Field(body, 144, 4) == 0) return false;
  const auto identity = Field(body, 148, 1);
  const auto dependency = Field(body, 149, 1);
  const auto flags = Field(body, 150, 2);
  const auto explicit_targets = Field(body, 152, 4);
  const auto final_targets = Field(body, 156, 4);
  if (identity < 1 || identity > 2 || dependency < 1 || dependency > 2 ||
      flags > 1 || explicit_targets == 0 || explicit_targets > final_targets ||
      final_targets > 65535) return false;
  for (const auto offset : {160,164,168,172})
    if (Field(body, offset, 4) > 1048576) return false;
  if (identity == 1 && Field(body, 172, 4) != 0) return false;
  if ((flags == 0) != (Field(body, 192, 8) == 0)) return false;
  for (const auto offset : {208,240,336})
    if (!Nonzero(body + offset - 16, 32)) return false;
  return !Nonzero(body + 368 - 16, 16) && value.availability_generation != 0;
}
bool ResultFields(const SblrTableTruncateResultV1& value) {
  const auto* body = value.canonical_body.data();
  for (const auto offset : {16,32,48,64,80,96})
    if (!SystemUuid(body + offset - 16)) return false;
  return Field(body, 112, 4) >= 1 && Field(body, 112, 4) <= 65535 &&
      Field(body, 116, 4) <= 1048576 && Field(body, 128, 4) == 1 &&
      Field(body, 132, 4) == 1 && !Nonzero(body + 136 - 16, 16) &&
      value.availability_generation != 0;
}
template<class T, class Validate>
Bytes Encode(const T& value, const char* magic, std::string_view domain,
             Validate validate) {
  try {
    if (!validate(value)) return {};
    TableTruncateSha digest{};
    if (!Hash(domain, value.canonical_body.data(), value.canonical_body.size(), &digest))
      return {};
    if (Nonzero(value.evidence.data(), value.evidence.size()) && value.evidence != digest)
      return {};
    const auto evidence_offset = 16 + value.canonical_body.size();
    auto bytes = Frame(magic, evidence_offset + 40);
    std::copy(value.canonical_body.begin(), value.canonical_body.end(), bytes.begin() + 16);
    std::copy(digest.begin(), digest.end(), bytes.begin() + evidence_offset);
    Put(bytes, evidence_offset + 32, value.availability_generation, 8);
    return bytes;
  } catch (const std::bad_alloc&) { return {}; }
    catch (const std::length_error&) { return {}; }
}
template<class T, class Validate>
bool Decode(const std::uint8_t* bytes, std::size_t size, T* output,
            std::string* detail, const char* magic, std::string_view domain,
            Validate validate) {
  try {
    T candidate;
    const auto evidence_offset = 16 + candidate.canonical_body.size();
    if (!output || !Header(bytes, size, magic, evidence_offset + 40))
      return Failure(detail, "truncate carrier header invalid");
    std::copy_n(bytes + 16, candidate.canonical_body.size(), candidate.canonical_body.begin());
    std::copy_n(bytes + evidence_offset, 32, candidate.evidence.begin());
    candidate.availability_generation = Read(bytes + evidence_offset + 32, 8);
    if (!validate(candidate)) return Failure(detail, "truncate carrier fields invalid");
    TableTruncateSha digest{};
    if (!Hash(domain, candidate.canonical_body.data(), candidate.canonical_body.size(), &digest) ||
        candidate.evidence != digest)
      return Failure(detail, "truncate carrier digest invalid");
    *output = candidate;
    return true;
  } catch (const std::bad_alloc&) { return Failure(detail, "truncate carrier allocation failed"); }
    catch (const std::length_error&) { return Failure(detail, "truncate carrier length invalid"); }
}
}  // namespace

std::vector<std::uint8_t> EncodeSblrTableTruncateRequestV1(const SblrTableTruncateRequestV1& value) {
  try {
    if (!SystemUuid(value.receipt.data()) || !value.occurrence || !value.truncate_occurrence)
      return {};
    auto bytes = Frame("TTRQ", 64);
    std::copy(value.receipt.begin(), value.receipt.end(), bytes.begin() + 16);
    Put(bytes, 32, value.occurrence, 8); Put(bytes, 40, value.truncate_occurrence, 4);
    return bytes;
  } catch (const std::bad_alloc&) { return {}; }
    catch (const std::length_error&) { return {}; }
}
bool DecodeSblrTableTruncateRequestV1(const std::uint8_t* bytes, std::size_t size,
    SblrTableTruncateRequestV1* output, std::string* detail) {
  if (!output || !Header(bytes, size, "TTRQ", 64))
    return Failure(detail, "truncate request header invalid");
  SblrTableTruncateRequestV1 candidate;
  std::copy_n(bytes + 16, 16, candidate.receipt.begin());
  candidate.occurrence = Read(bytes + 32, 8);
  candidate.truncate_occurrence = static_cast<std::uint32_t>(Read(bytes + 40, 4));
  if (!SystemUuid(candidate.receipt.data()) || !candidate.occurrence ||
      !candidate.truncate_occurrence || Nonzero(bytes + 44, 20))
    return Failure(detail, "truncate request fields invalid");
  *output = candidate;
  return true;
}
std::vector<std::uint8_t> EncodeSblrTableTruncateDescriptorV1(
    const SblrTableTruncateDescriptorV1& value, bool operand) {
  return Encode(value, operand ? "TTRO" : "TTRD", kDescriptorDomain, DescriptorFields);
}
bool DecodeSblrTableTruncateDescriptorV1(const std::uint8_t* bytes, std::size_t size,
    SblrTableTruncateDescriptorV1* output, std::string* detail, bool operand) {
  return Decode(bytes, size, output, detail, operand ? "TTRO" : "TTRD",
                kDescriptorDomain, DescriptorFields);
}
std::vector<std::uint8_t> EncodeSblrTableTruncateResultV1(const SblrTableTruncateResultV1& value) {
  return Encode(value, "TTRS", kResultDomain, ResultFields);
}
bool DecodeSblrTableTruncateResultV1(const std::uint8_t* bytes, std::size_t size,
    SblrTableTruncateResultV1* output, std::string* detail) {
  return Decode(bytes, size, output, detail, "TTRS", kResultDomain, ResultFields);
}
}  // namespace scratchbird::engine::sblr

#include "sblr_atomic_read_modify_write_runtime.hpp"

#include "core/hash/hash_digest.hpp"

#include <algorithm>
#include <cstring>

namespace scratchbird::engine::sblr {
namespace {
void Put(std::vector<std::uint8_t>& out, std::uint64_t value, std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) out.push_back((value >> (8 * i)) & 0xff);
}
std::uint64_t Get(const std::uint8_t* bytes, std::size_t size) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < size; ++i) value |= std::uint64_t(bytes[i]) << (8 * i);
  return value;
}
template <typename T> bool Nonzero(const T& value) {
  return std::any_of(value.begin(), value.end(), [](auto byte) { return byte != 0; });
}
std::vector<std::uint8_t> Header(const char* magic, std::size_t size) {
  std::vector<std::uint8_t> out(magic, magic + 4);
  Put(out, 1, 2); Put(out, size, 2); Put(out, size, 4); Put(out, 0, 4);
  return out;
}
bool ValidHeader(const std::uint8_t* bytes, std::size_t size,
                 const char* magic, std::size_t expected) {
  return bytes && size == expected && std::equal(bytes, bytes + 4, magic) &&
         Get(bytes + 4, 2) == 1 && Get(bytes + 6, 2) == expected &&
         Get(bytes + 8, 4) == expected &&
         std::all_of(bytes + 12, bytes + 16, [](auto byte) { return byte == 0; });
}
AtomicRmwSha Hash(const char* domain, const std::uint8_t* bytes, std::size_t size) {
  std::vector<std::uint8_t> material(domain, domain + std::strlen(domain));
  material.insert(material.end(), bytes, bytes + size);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}
}  // namespace

std::vector<std::uint8_t> EncodeSblrAtomicRmwRequestV1(const SblrAtomicRmwRequestV1& value) {
  if (!Nonzero(value.receipt) || value.occurrence == 0 || value.rmw_occurrence == 0) return {};
  auto out = Header("ARWQ", 64);
  out.insert(out.end(), value.receipt.begin(), value.receipt.end());
  Put(out, value.occurrence, 8); Put(out, value.rmw_occurrence, 4);
  out.insert(out.end(), 20, 0);
  return out;
}
bool DecodeSblrAtomicRmwRequestV1(const std::uint8_t* bytes, std::size_t size,
                                  SblrAtomicRmwRequestV1* out, std::string* detail) {
  if (!out || !ValidHeader(bytes, size, "ARWQ", 64) ||
      std::any_of(bytes + 44, bytes + 64, [](auto byte) { return byte != 0; })) {
    if (detail) *detail = "ARWQ invalid";
    return false;
  }
  SblrAtomicRmwRequestV1 value;
  std::copy_n(bytes + 16, 16, value.receipt.begin());
  value.occurrence = Get(bytes + 32, 8);
  value.rmw_occurrence = static_cast<std::uint32_t>(Get(bytes + 40, 4));
  if (EncodeSblrAtomicRmwRequestV1(value).empty()) return false;
  *out = value;
  return true;
}

std::vector<std::uint8_t> EncodeSblrAtomicRmwDescriptorV1(
    const SblrAtomicRmwDescriptorV1& value, bool operand) {
  if (!Nonzero(value.canonical_body) || value.availability_generation == 0) return {};
  auto out = Header(operand ? "ARWO" : "ARWD", 488);
  out.insert(out.end(), value.canonical_body.begin(), value.canonical_body.end());
  const auto evidence = Hash("ScratchBird.SblrAtomicReadModifyWriteDescriptor.V1",
                             out.data() + 16, 432);
  if (Nonzero(value.evidence) && value.evidence != evidence) return {};
  out.insert(out.end(), evidence.begin(), evidence.end());
  Put(out, value.availability_generation, 8);
  return out;
}
bool DecodeSblrAtomicRmwDescriptorV1(const std::uint8_t* bytes, std::size_t size,
                                     SblrAtomicRmwDescriptorV1* out,
                                     std::string* detail, bool operand) {
  if (!out || !ValidHeader(bytes, size, operand ? "ARWO" : "ARWD", 488)) {
    if (detail) *detail = "ARW descriptor invalid";
    return false;
  }
  SblrAtomicRmwDescriptorV1 value;
  std::copy_n(bytes + 16, 432, value.canonical_body.begin());
  std::copy_n(bytes + 448, 32, value.evidence.begin());
  value.availability_generation = Get(bytes + 480, 8);
  if (EncodeSblrAtomicRmwDescriptorV1(value, operand).empty()) return false;
  *out = value;
  return true;
}

std::vector<std::uint8_t> EncodeSblrAtomicRmwResultV1(const SblrAtomicRmwResultV1& value) {
  if (!Nonzero(value.canonical_body) || value.availability_generation == 0) return {};
  auto out = Header("ARWR", 224);
  out.insert(out.end(), value.canonical_body.begin(), value.canonical_body.end());
  const auto evidence = Hash("ScratchBird.SblrAtomicReadModifyWriteResult.V1",
                             out.data() + 16, 168);
  if (Nonzero(value.evidence) && value.evidence != evidence) return {};
  out.insert(out.end(), evidence.begin(), evidence.end());
  Put(out, value.availability_generation, 8);
  return out;
}
bool DecodeSblrAtomicRmwResultV1(const std::uint8_t* bytes, std::size_t size,
                                 SblrAtomicRmwResultV1* out, std::string* detail) {
  if (!out || !ValidHeader(bytes, size, "ARWR", 224)) {
    if (detail) *detail = "ARWR invalid";
    return false;
  }
  SblrAtomicRmwResultV1 value;
  std::copy_n(bytes + 16, 168, value.canonical_body.begin());
  std::copy_n(bytes + 184, 32, value.evidence.begin());
  value.availability_generation = Get(bytes + 216, 8);
  if (EncodeSblrAtomicRmwResultV1(value).empty()) return false;
  *out = value;
  return true;
}

}  // namespace scratchbird::engine::sblr

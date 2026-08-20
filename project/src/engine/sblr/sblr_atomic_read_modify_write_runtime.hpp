#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

using AtomicRmwUuid = std::array<std::uint8_t, 16>;
using AtomicRmwSha = std::array<std::uint8_t, 32>;

struct SblrAtomicRmwRequestV1 {
  AtomicRmwUuid receipt{};
  std::uint64_t occurrence = 0;
  std::uint32_t rmw_occurrence = 0;
};

struct SblrAtomicRmwDescriptorV1 {
  std::array<std::uint8_t, 432> canonical_body{};
  AtomicRmwSha evidence{};
  std::uint64_t availability_generation = 0;
};

struct SblrAtomicRmwResultV1 {
  std::array<std::uint8_t, 168> canonical_body{};
  AtomicRmwSha evidence{};
  std::uint64_t availability_generation = 0;
};

std::vector<std::uint8_t> EncodeSblrAtomicRmwRequestV1(
    const SblrAtomicRmwRequestV1& value);
bool DecodeSblrAtomicRmwRequestV1(const std::uint8_t* bytes,
                                  std::size_t size,
                                  SblrAtomicRmwRequestV1* out,
                                  std::string* detail);
std::vector<std::uint8_t> EncodeSblrAtomicRmwDescriptorV1(
    const SblrAtomicRmwDescriptorV1& value, bool operand);
bool DecodeSblrAtomicRmwDescriptorV1(const std::uint8_t* bytes,
                                     std::size_t size,
                                     SblrAtomicRmwDescriptorV1* out,
                                     std::string* detail,
                                     bool operand);
std::vector<std::uint8_t> EncodeSblrAtomicRmwResultV1(
    const SblrAtomicRmwResultV1& value);
bool DecodeSblrAtomicRmwResultV1(const std::uint8_t* bytes,
                                 std::size_t size,
                                 SblrAtomicRmwResultV1* out,
                                 std::string* detail);

}  // namespace scratchbird::engine::sblr

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrDmlCounterAddRequestV1 {
  std::array<std::uint8_t, 16> receipt{};
  std::uint64_t occurrence{};
  std::uint64_t counter_occurrence{};
};

struct SblrDmlCounterAddDescriptorV1 {
  std::array<std::uint8_t, 400> body{};
  std::array<std::uint8_t, 32> evidence{};
  std::uint64_t availability{};
};

struct SblrDmlCounterAddResultV1 {
  std::array<std::uint8_t, 240> body{};
  std::array<std::uint8_t, 32> evidence{};
  std::uint64_t availability{};
  std::array<std::uint8_t, 16> publication_barrier{};
};

std::vector<std::uint8_t> EncodeSblrDmlCounterAddRequestV1(const SblrDmlCounterAddRequestV1&);
bool DecodeSblrDmlCounterAddRequestV1(const std::uint8_t*, std::size_t,
                                      SblrDmlCounterAddRequestV1*, std::string*);
std::vector<std::uint8_t> EncodeSblrDmlCounterAddDescriptorV1(const SblrDmlCounterAddDescriptorV1&, bool operand);
bool DecodeSblrDmlCounterAddDescriptorV1(const std::uint8_t*, std::size_t,
                                         SblrDmlCounterAddDescriptorV1*, std::string*, bool operand);
std::vector<std::uint8_t> EncodeSblrDmlCounterAddResultV1(const SblrDmlCounterAddResultV1&);
bool DecodeSblrDmlCounterAddResultV1(const std::uint8_t*, std::size_t,
                                     SblrDmlCounterAddResultV1*, std::string*);

}  // namespace scratchbird::engine::sblr

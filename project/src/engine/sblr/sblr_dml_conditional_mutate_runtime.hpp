#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrDmlConditionalMutateRequestV1 {
  std::array<std::uint8_t, 16> receipt{};
  std::uint64_t occurrence{};
  std::uint64_t mutation_occurrence{};
};

struct SblrDmlConditionalMutateDescriptorV1 {
  std::array<std::uint8_t, 400> body{};
  std::array<std::uint8_t, 32> evidence{};
  std::uint64_t availability{};
};

struct SblrDmlConditionalMutateResultV1 {
  std::array<std::uint8_t, 240> body{};
  std::array<std::uint8_t, 32> evidence{};
  std::uint64_t availability{};
  std::array<std::uint8_t, 16> publication_barrier{};
};

std::vector<std::uint8_t> EncodeSblrDmlConditionalMutateRequestV1(
    const SblrDmlConditionalMutateRequestV1&);
bool DecodeSblrDmlConditionalMutateRequestV1(
    const std::uint8_t*, std::size_t,
    SblrDmlConditionalMutateRequestV1*, std::string*);

std::vector<std::uint8_t> EncodeSblrDmlConditionalMutateDescriptorV1(
    const SblrDmlConditionalMutateDescriptorV1&, bool operand);
bool DecodeSblrDmlConditionalMutateDescriptorV1(
    const std::uint8_t*, std::size_t,
    SblrDmlConditionalMutateDescriptorV1*, std::string*, bool operand);

std::vector<std::uint8_t> EncodeSblrDmlConditionalMutateResultV1(
    const SblrDmlConditionalMutateResultV1&);
bool DecodeSblrDmlConditionalMutateResultV1(
    const std::uint8_t*, std::size_t,
    SblrDmlConditionalMutateResultV1*, std::string*);

}  // namespace scratchbird::engine::sblr

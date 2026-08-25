#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {
struct SblrVersionedRevertRequestV1 {
  std::array<std::uint8_t, 16> operation{};
  std::array<std::uint8_t, 16> receipt{};
  std::uint32_t descriptor_length = 0;
};
struct SblrVersionedRevertDescriptorV1 { std::array<std::uint8_t, 376> body{}; };
struct SblrVersionedRevertResultV1 { std::array<std::uint8_t, 376> body{}; };

std::vector<std::uint8_t> EncodeSblrVersionedRevertRequestV1(const SblrVersionedRevertRequestV1&);
bool DecodeSblrVersionedRevertRequestV1(const std::uint8_t*, std::size_t, SblrVersionedRevertRequestV1*, std::string*);
std::vector<std::uint8_t> EncodeSblrVersionedRevertDescriptorV1(const SblrVersionedRevertDescriptorV1&);
bool DecodeSblrVersionedRevertDescriptorV1(const std::uint8_t*, std::size_t, SblrVersionedRevertDescriptorV1*, std::string*);
std::vector<std::uint8_t> EncodeSblrVersionedRevertResultV1(const SblrVersionedRevertResultV1&);
bool DecodeSblrVersionedRevertResultV1(const std::uint8_t*, std::size_t, SblrVersionedRevertResultV1*, std::string*);
}

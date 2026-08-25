#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {
using DdlAlterPublicationUuid = std::array<std::uint8_t, 16>;
using DdlAlterPublicationSha = std::array<std::uint8_t, 32>;
struct SblrDdlAlterPublicationRequestV1 {
  DdlAlterPublicationUuid operation{};
  DdlAlterPublicationUuid receipt{};
  std::uint32_t descriptor_length = 0;
};
struct SblrDdlAlterPublicationDescriptorV1 {
  std::array<std::uint8_t, 312> body{};
};
struct SblrDdlAlterPublicationResultV1 { std::array<std::uint8_t, 184> body{}; };
std::vector<std::uint8_t> EncodeSblrDdlAlterPublicationRequestV1(const SblrDdlAlterPublicationRequestV1&);
bool DecodeSblrDdlAlterPublicationRequestV1(const std::uint8_t*, std::size_t, SblrDdlAlterPublicationRequestV1*, std::string*);
std::vector<std::uint8_t> EncodeSblrDdlAlterPublicationDescriptorV1(const SblrDdlAlterPublicationDescriptorV1&);
bool DecodeSblrDdlAlterPublicationDescriptorV1(const std::uint8_t*, std::size_t, SblrDdlAlterPublicationDescriptorV1*, std::string*);
std::vector<std::uint8_t> EncodeSblrDdlAlterPublicationResultV1(const SblrDdlAlterPublicationResultV1&);
bool DecodeSblrDdlAlterPublicationResultV1(const std::uint8_t*, std::size_t, SblrDdlAlterPublicationResultV1*, std::string*);
}

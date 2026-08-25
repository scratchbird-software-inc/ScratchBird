#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {
struct SblrDdlAlterExtensionRequestV1 { std::array<std::uint8_t,16> operation{}, receipt{}; std::uint32_t descriptor_length=0; };
struct SblrDdlAlterExtensionDescriptorV1 { std::array<std::uint8_t,376> body{}; };
struct SblrDdlAlterExtensionResultV1 { std::array<std::uint8_t,376> body{}; };
std::vector<std::uint8_t> EncodeSblrDdlAlterExtensionRequestV1(const SblrDdlAlterExtensionRequestV1&);
bool DecodeSblrDdlAlterExtensionRequestV1(const std::uint8_t*,std::size_t,SblrDdlAlterExtensionRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlAlterExtensionDescriptorV1(const SblrDdlAlterExtensionDescriptorV1&);
bool DecodeSblrDdlAlterExtensionDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlAlterExtensionDescriptorV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlAlterExtensionResultV1(const SblrDdlAlterExtensionResultV1&);
bool DecodeSblrDdlAlterExtensionResultV1(const std::uint8_t*,std::size_t,SblrDdlAlterExtensionResultV1*,std::string*);
}

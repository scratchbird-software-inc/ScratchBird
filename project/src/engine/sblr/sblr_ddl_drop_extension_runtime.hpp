#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrDdlDropExtensionRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};
struct SblrDdlDropExtensionDescriptorV1{std::array<std::uint8_t,376>body{};};
struct SblrDdlDropExtensionResultV1{std::array<std::uint8_t,376>body{};};
std::vector<std::uint8_t> EncodeSblrDdlDropExtensionRequestV1(const SblrDdlDropExtensionRequestV1&); bool DecodeSblrDdlDropExtensionRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropExtensionRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropExtensionDescriptorV1(const SblrDdlDropExtensionDescriptorV1&); bool DecodeSblrDdlDropExtensionDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropExtensionDescriptorV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropExtensionResultV1(const SblrDdlDropExtensionResultV1&); bool DecodeSblrDdlDropExtensionResultV1(const std::uint8_t*,std::size_t,SblrDdlDropExtensionResultV1*,std::string*);
}

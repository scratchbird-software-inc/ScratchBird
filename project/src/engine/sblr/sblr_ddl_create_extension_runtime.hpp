#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr { struct SblrDdlCreateExtensionRequestV1 { std::array<std::uint8_t,16> operation{}, receipt{}; std::uint32_t descriptor_length=0; }; struct SblrDdlCreateExtensionDescriptorV1 { std::array<std::uint8_t,376> body{}; }; struct SblrDdlCreateExtensionResultV1 { std::array<std::uint8_t,376> body{}; }; std::vector<std::uint8_t> EncodeSblrDdlCreateExtensionRequestV1(const SblrDdlCreateExtensionRequestV1&); bool DecodeSblrDdlCreateExtensionRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateExtensionRequestV1*,std::string*); std::vector<std::uint8_t> EncodeSblrDdlCreateExtensionDescriptorV1(const SblrDdlCreateExtensionDescriptorV1&); bool DecodeSblrDdlCreateExtensionDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateExtensionDescriptorV1*,std::string*); std::vector<std::uint8_t> EncodeSblrDdlCreateExtensionResultV1(const SblrDdlCreateExtensionResultV1&); bool DecodeSblrDdlCreateExtensionResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateExtensionResultV1*,std::string*); }

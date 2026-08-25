#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrDdlCreateOperatorRequestV1 { std::array<std::uint8_t,16> operation{}, receipt{}; std::uint32_t descriptor_length=0; };
struct SblrDdlCreateOperatorDescriptorV1 { std::array<std::uint8_t,376> body{}; };
struct SblrDdlCreateOperatorResultV1 { std::array<std::uint8_t,376> body{}; };
std::vector<std::uint8_t> EncodeSblrDdlCreateOperatorRequestV1(const SblrDdlCreateOperatorRequestV1&);
bool DecodeSblrDdlCreateOperatorRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateOperatorRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateOperatorDescriptorV1(const SblrDdlCreateOperatorDescriptorV1&);
bool DecodeSblrDdlCreateOperatorDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateOperatorDescriptorV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateOperatorResultV1(const SblrDdlCreateOperatorResultV1&);
bool DecodeSblrDdlCreateOperatorResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateOperatorResultV1*,std::string*);
}

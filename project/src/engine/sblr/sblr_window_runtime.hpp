#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using WindowUuid=std::array<std::uint8_t,16>; using WindowSha=std::array<std::uint8_t,32>;
struct SblrWindowRequestV1 { WindowUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t window_occurrence=0; };
struct SblrWindowDescriptorV1 { std::array<std::uint8_t,392> body{}; WindowSha evidence{}; std::uint64_t availability=0; };
struct SblrWindowResultV1 { std::array<std::uint8_t,240> body{}; WindowSha evidence{}; std::uint64_t availability=0; WindowUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrWindowRequestV1(const SblrWindowRequestV1&); bool DecodeSblrWindowRequestV1(const std::uint8_t*,std::size_t,SblrWindowRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrWindowDescriptorV1(const SblrWindowDescriptorV1&,bool); bool DecodeSblrWindowDescriptorV1(const std::uint8_t*,std::size_t,SblrWindowDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrWindowResultV1(const SblrWindowResultV1&); bool DecodeSblrWindowResultV1(const std::uint8_t*,std::size_t,SblrWindowResultV1*,std::string*);
}

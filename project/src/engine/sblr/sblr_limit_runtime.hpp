#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using LimitUuid=std::array<std::uint8_t,16>; using LimitSha=std::array<std::uint8_t,32>;
struct SblrLimitRequestV1 { LimitUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t limit_occurrence=0; };
struct SblrLimitDescriptorV1 { std::array<std::uint8_t,392> body{}; LimitSha evidence{}; std::uint64_t availability=0; };
struct SblrLimitResultV1 { std::array<std::uint8_t,240> body{}; LimitSha evidence{}; std::uint64_t availability=0; LimitUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrLimitRequestV1(const SblrLimitRequestV1&); bool DecodeSblrLimitRequestV1(const std::uint8_t*,std::size_t,SblrLimitRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrLimitDescriptorV1(const SblrLimitDescriptorV1&,bool); bool DecodeSblrLimitDescriptorV1(const std::uint8_t*,std::size_t,SblrLimitDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrLimitResultV1(const SblrLimitResultV1&); bool DecodeSblrLimitResultV1(const std::uint8_t*,std::size_t,SblrLimitResultV1*,std::string*);
}

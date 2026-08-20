#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using KvStructuredReadUuid=std::array<std::uint8_t,16>; using KvStructuredReadSha=std::array<std::uint8_t,32>;
struct SblrKvStructuredReadRequestV1 { KvStructuredReadUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t read_occurrence=0; };
struct SblrKvStructuredReadDescriptorV1 { std::array<std::uint8_t,392> body{}; KvStructuredReadSha evidence{}; std::uint64_t availability=0; };
struct SblrKvStructuredReadResultV1 { std::array<std::uint8_t,240> body{}; KvStructuredReadSha evidence{}; std::uint64_t availability=0; KvStructuredReadUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrKvStructuredReadRequestV1(const SblrKvStructuredReadRequestV1&);
bool DecodeSblrKvStructuredReadRequestV1(const std::uint8_t*,std::size_t,SblrKvStructuredReadRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrKvStructuredReadDescriptorV1(const SblrKvStructuredReadDescriptorV1&,bool);
bool DecodeSblrKvStructuredReadDescriptorV1(const std::uint8_t*,std::size_t,SblrKvStructuredReadDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrKvStructuredReadResultV1(const SblrKvStructuredReadResultV1&);
bool DecodeSblrKvStructuredReadResultV1(const std::uint8_t*,std::size_t,SblrKvStructuredReadResultV1*,std::string*);
}

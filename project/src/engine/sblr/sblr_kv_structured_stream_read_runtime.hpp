#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using KvStructuredStreamReadUuid=std::array<std::uint8_t,16>; using KvStructuredStreamReadSha=std::array<std::uint8_t,32>;
struct SblrKvStructuredStreamReadRequestV1 { KvStructuredStreamReadUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t stream_read_occurrence=0; };
struct SblrKvStructuredStreamReadDescriptorV1 { std::array<std::uint8_t,400> body{}; KvStructuredStreamReadSha evidence{}; std::uint64_t availability=0; };
struct SblrKvStructuredStreamReadResultV1 { std::array<std::uint8_t,240> body{}; KvStructuredStreamReadSha evidence{}; std::uint64_t availability=0; KvStructuredStreamReadUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrKvStructuredStreamReadRequestV1(const SblrKvStructuredStreamReadRequestV1&);
bool DecodeSblrKvStructuredStreamReadRequestV1(const std::uint8_t*,std::size_t,SblrKvStructuredStreamReadRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrKvStructuredStreamReadDescriptorV1(const SblrKvStructuredStreamReadDescriptorV1&,bool);
bool DecodeSblrKvStructuredStreamReadDescriptorV1(const std::uint8_t*,std::size_t,SblrKvStructuredStreamReadDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrKvStructuredStreamReadResultV1(const SblrKvStructuredStreamReadResultV1&);
bool DecodeSblrKvStructuredStreamReadResultV1(const std::uint8_t*,std::size_t,SblrKvStructuredStreamReadResultV1*,std::string*);
}

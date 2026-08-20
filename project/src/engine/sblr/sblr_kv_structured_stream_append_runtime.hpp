#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using KvStructuredStreamAppendUuid=std::array<std::uint8_t,16>; using KvStructuredStreamAppendSha=std::array<std::uint8_t,32>;
struct SblrKvStructuredStreamAppendRequestV1 { KvStructuredStreamAppendUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t stream_append_occurrence=0; };
struct SblrKvStructuredStreamAppendDescriptorV1 { std::array<std::uint8_t,400> body{}; KvStructuredStreamAppendSha evidence{}; std::uint64_t availability=0; };
struct SblrKvStructuredStreamAppendResultV1 { std::array<std::uint8_t,240> body{}; KvStructuredStreamAppendSha evidence{}; std::uint64_t availability=0; KvStructuredStreamAppendUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrKvStructuredStreamAppendRequestV1(const SblrKvStructuredStreamAppendRequestV1&);
bool DecodeSblrKvStructuredStreamAppendRequestV1(const std::uint8_t*,std::size_t,SblrKvStructuredStreamAppendRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrKvStructuredStreamAppendDescriptorV1(const SblrKvStructuredStreamAppendDescriptorV1&,bool);
bool DecodeSblrKvStructuredStreamAppendDescriptorV1(const std::uint8_t*,std::size_t,SblrKvStructuredStreamAppendDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrKvStructuredStreamAppendResultV1(const SblrKvStructuredStreamAppendResultV1&);
bool DecodeSblrKvStructuredStreamAppendResultV1(const std::uint8_t*,std::size_t,SblrKvStructuredStreamAppendResultV1*,std::string*);
}

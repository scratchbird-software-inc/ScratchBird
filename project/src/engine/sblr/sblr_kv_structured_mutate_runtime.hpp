#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using KvStructuredMutateUuid=std::array<std::uint8_t,16>; using KvStructuredMutateSha=std::array<std::uint8_t,32>;
struct SblrKvStructuredMutateRequestV1 { KvStructuredMutateUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t mutate_occurrence=0; };
struct SblrKvStructuredMutateDescriptorV1 { std::array<std::uint8_t,400> body{}; KvStructuredMutateSha evidence{}; std::uint64_t availability=0; };
struct SblrKvStructuredMutateResultV1 { std::array<std::uint8_t,240> body{}; KvStructuredMutateSha evidence{}; std::uint64_t availability=0; KvStructuredMutateUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrKvStructuredMutateRequestV1(const SblrKvStructuredMutateRequestV1&);
bool DecodeSblrKvStructuredMutateRequestV1(const std::uint8_t*,std::size_t,SblrKvStructuredMutateRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrKvStructuredMutateDescriptorV1(const SblrKvStructuredMutateDescriptorV1&,bool);
bool DecodeSblrKvStructuredMutateDescriptorV1(const std::uint8_t*,std::size_t,SblrKvStructuredMutateDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrKvStructuredMutateResultV1(const SblrKvStructuredMutateResultV1&);
bool DecodeSblrKvStructuredMutateResultV1(const std::uint8_t*,std::size_t,SblrKvStructuredMutateResultV1*,std::string*);
}

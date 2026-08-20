#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using InsertUuid=std::array<std::uint8_t,16>; using InsertSha=std::array<std::uint8_t,32>;
struct SblrInsertRequestV1 { InsertUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t insert_occurrence=0; };
struct SblrInsertDescriptorV1 { std::array<std::uint8_t,384> canonical_body{}; InsertSha evidence{}; std::uint64_t availability_generation=0; };
struct SblrInsertResultV1 { std::array<std::uint8_t,136> canonical_body{}; InsertSha evidence{}; std::uint64_t availability_generation=0; };
std::vector<std::uint8_t> EncodeSblrInsertRequestV1(const SblrInsertRequestV1&);
bool DecodeSblrInsertRequestV1(const std::uint8_t*,std::size_t,SblrInsertRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrInsertDescriptorV1(const SblrInsertDescriptorV1&,bool operand);
bool DecodeSblrInsertDescriptorV1(const std::uint8_t*,std::size_t,SblrInsertDescriptorV1*,std::string*,bool operand);
std::vector<std::uint8_t> EncodeSblrInsertResultV1(const SblrInsertResultV1&);
bool DecodeSblrInsertResultV1(const std::uint8_t*,std::size_t,SblrInsertResultV1*,std::string*);
}

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlDropIndexUuid=std::array<std::uint8_t,16>; using DdlDropIndexSha=std::array<std::uint8_t,32>;
struct SblrDdlDropIndexRequestV1 { DdlDropIndexUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t index_occurrence=0; };
struct SblrDdlDropIndexDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlDropIndexSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlDropIndexResultV1 { std::array<std::uint8_t,240> body{}; DdlDropIndexSha evidence{}; std::uint64_t availability=0; DdlDropIndexUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlDropIndexRequestV1(const SblrDdlDropIndexRequestV1&);
bool DecodeSblrDdlDropIndexRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropIndexRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropIndexDescriptorV1(const SblrDdlDropIndexDescriptorV1&,bool);
bool DecodeSblrDdlDropIndexDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropIndexDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlDropIndexResultV1(const SblrDdlDropIndexResultV1&);
bool DecodeSblrDdlDropIndexResultV1(const std::uint8_t*,std::size_t,SblrDdlDropIndexResultV1*,std::string*);
}

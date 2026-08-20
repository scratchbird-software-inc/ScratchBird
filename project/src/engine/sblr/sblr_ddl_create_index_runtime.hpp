#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlCreateIndexUuid=std::array<std::uint8_t,16>; using DdlCreateIndexSha=std::array<std::uint8_t,32>;
struct SblrDdlCreateIndexRequestV1 { DdlCreateIndexUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t index_occurrence=0; };
struct SblrDdlCreateIndexDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlCreateIndexSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlCreateIndexResultV1 { std::array<std::uint8_t,240> body{}; DdlCreateIndexSha evidence{}; std::uint64_t availability=0; DdlCreateIndexUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlCreateIndexRequestV1(const SblrDdlCreateIndexRequestV1&);
bool DecodeSblrDdlCreateIndexRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateIndexRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateIndexDescriptorV1(const SblrDdlCreateIndexDescriptorV1&,bool);
bool DecodeSblrDdlCreateIndexDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateIndexDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlCreateIndexResultV1(const SblrDdlCreateIndexResultV1&);
bool DecodeSblrDdlCreateIndexResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateIndexResultV1*,std::string*);
}

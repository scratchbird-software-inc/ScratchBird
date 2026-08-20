#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlDropViewUuid=std::array<std::uint8_t,16>; using DdlDropViewSha=std::array<std::uint8_t,32>;
struct SblrDdlDropViewRequestV1 { DdlDropViewUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t domain_occurrence=0; };
struct SblrDdlDropViewDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlDropViewSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlDropViewResultV1 { std::array<std::uint8_t,240> body{}; DdlDropViewSha evidence{}; std::uint64_t availability=0; DdlDropViewUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlDropViewRequestV1(const SblrDdlDropViewRequestV1&);
bool DecodeSblrDdlDropViewRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropViewRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropViewDescriptorV1(const SblrDdlDropViewDescriptorV1&,bool);
bool DecodeSblrDdlDropViewDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropViewDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlDropViewResultV1(const SblrDdlDropViewResultV1&);
bool DecodeSblrDdlDropViewResultV1(const std::uint8_t*,std::size_t,SblrDdlDropViewResultV1*,std::string*);
}

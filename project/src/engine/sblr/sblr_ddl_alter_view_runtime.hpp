#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlAlterViewUuid=std::array<std::uint8_t,16>; using DdlAlterViewSha=std::array<std::uint8_t,32>;
struct SblrDdlAlterViewRequestV1 { DdlAlterViewUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t domain_occurrence=0; };
struct SblrDdlAlterViewDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlAlterViewSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlAlterViewResultV1 { std::array<std::uint8_t,240> body{}; DdlAlterViewSha evidence{}; std::uint64_t availability=0; DdlAlterViewUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlAlterViewRequestV1(const SblrDdlAlterViewRequestV1&);
bool DecodeSblrDdlAlterViewRequestV1(const std::uint8_t*,std::size_t,SblrDdlAlterViewRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlAlterViewDescriptorV1(const SblrDdlAlterViewDescriptorV1&,bool);
bool DecodeSblrDdlAlterViewDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlAlterViewDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlAlterViewResultV1(const SblrDdlAlterViewResultV1&);
bool DecodeSblrDdlAlterViewResultV1(const std::uint8_t*,std::size_t,SblrDdlAlterViewResultV1*,std::string*);
}

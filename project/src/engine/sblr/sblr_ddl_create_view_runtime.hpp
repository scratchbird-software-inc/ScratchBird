#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlCreateViewUuid=std::array<std::uint8_t,16>; using DdlCreateViewSha=std::array<std::uint8_t,32>;
struct SblrDdlCreateViewRequestV1 { DdlCreateViewUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t domain_occurrence=0; };
struct SblrDdlCreateViewDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlCreateViewSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlCreateViewResultV1 { std::array<std::uint8_t,240> body{}; DdlCreateViewSha evidence{}; std::uint64_t availability=0; DdlCreateViewUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlCreateViewRequestV1(const SblrDdlCreateViewRequestV1&);
bool DecodeSblrDdlCreateViewRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateViewRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateViewDescriptorV1(const SblrDdlCreateViewDescriptorV1&,bool);
bool DecodeSblrDdlCreateViewDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateViewDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlCreateViewResultV1(const SblrDdlCreateViewResultV1&);
bool DecodeSblrDdlCreateViewResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateViewResultV1*,std::string*);
}

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlDropSrsUuid=std::array<std::uint8_t,16>; using DdlDropSrsSha=std::array<std::uint8_t,32>;
struct SblrDdlDropSrsRequestV1 { DdlDropSrsUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t srs_occurrence=0; };
struct SblrDdlDropSrsDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlDropSrsSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlDropSrsResultV1 { std::array<std::uint8_t,240> body{}; DdlDropSrsSha evidence{}; std::uint64_t availability=0; DdlDropSrsUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlDropSrsRequestV1(const SblrDdlDropSrsRequestV1&); bool DecodeSblrDdlDropSrsRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropSrsRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropSrsDescriptorV1(const SblrDdlDropSrsDescriptorV1&,bool); bool DecodeSblrDdlDropSrsDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropSrsDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlDropSrsResultV1(const SblrDdlDropSrsResultV1&); bool DecodeSblrDdlDropSrsResultV1(const std::uint8_t*,std::size_t,SblrDdlDropSrsResultV1*,std::string*);
}

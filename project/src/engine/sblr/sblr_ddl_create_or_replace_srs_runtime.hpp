#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlCreateOrReplaceSrsUuid=std::array<std::uint8_t,16>; using DdlCreateOrReplaceSrsSha=std::array<std::uint8_t,32>;
struct SblrDdlCreateOrReplaceSrsRequestV1 { DdlCreateOrReplaceSrsUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t srs_occurrence=0; };
struct SblrDdlCreateOrReplaceSrsDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlCreateOrReplaceSrsSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlCreateOrReplaceSrsResultV1 { std::array<std::uint8_t,240> body{}; DdlCreateOrReplaceSrsSha evidence{}; std::uint64_t availability=0; DdlCreateOrReplaceSrsUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlCreateOrReplaceSrsRequestV1(const SblrDdlCreateOrReplaceSrsRequestV1&); bool DecodeSblrDdlCreateOrReplaceSrsRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateOrReplaceSrsRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateOrReplaceSrsDescriptorV1(const SblrDdlCreateOrReplaceSrsDescriptorV1&,bool); bool DecodeSblrDdlCreateOrReplaceSrsDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateOrReplaceSrsDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlCreateOrReplaceSrsResultV1(const SblrDdlCreateOrReplaceSrsResultV1&); bool DecodeSblrDdlCreateOrReplaceSrsResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateOrReplaceSrsResultV1*,std::string*);
}

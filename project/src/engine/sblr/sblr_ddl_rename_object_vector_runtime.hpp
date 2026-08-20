#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlRenameObjectVectorUuid=std::array<std::uint8_t,16>; using DdlRenameObjectVectorSha=std::array<std::uint8_t,32>;
struct SblrDdlRenameObjectVectorRequestV1 { DdlRenameObjectVectorUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t object_vector_occurrence=0; };
struct SblrDdlRenameObjectVectorDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlRenameObjectVectorSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlRenameObjectVectorResultV1 { std::array<std::uint8_t,240> body{}; DdlRenameObjectVectorSha evidence{}; std::uint64_t availability=0; DdlRenameObjectVectorUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlRenameObjectVectorRequestV1(const SblrDdlRenameObjectVectorRequestV1&);
bool DecodeSblrDdlRenameObjectVectorRequestV1(const std::uint8_t*,std::size_t,SblrDdlRenameObjectVectorRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlRenameObjectVectorDescriptorV1(const SblrDdlRenameObjectVectorDescriptorV1&,bool);
bool DecodeSblrDdlRenameObjectVectorDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlRenameObjectVectorDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlRenameObjectVectorResultV1(const SblrDdlRenameObjectVectorResultV1&);
bool DecodeSblrDdlRenameObjectVectorResultV1(const std::uint8_t*,std::size_t,SblrDdlRenameObjectVectorResultV1*,std::string*);
}
